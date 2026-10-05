/*
 * qdv_test.cpp - correctness + ABI-contract test for qso_dmr_vocoder.
 *
 * Drives the stable qdv_* C ABI with firmware loaded at RUNTIME from files
 * (not baked in), proving the runtime-load refactor preserved correctness:
 *   - init success/failure contract (missing path, wrong-size core, double-init)
 *   - 1 kHz tone round-trip (energy stays at 1 kHz)
 *   - speech round-trip (loudness-envelope correlation, parametric metric)
 *   - a quick latency sanity check
 * Saves round-trip WAVs. Same source builds for Windows x64 and Android arm64.
 *
 * Usage: qdv_test <firmware.bin> <core.img> [speech_8k.wav] [out_dir]
 *
 * GPL: part of the qso_dmr_vocoder module. See ../LICENSE.
 */

#define _USE_MATH_DEFINES // MSVC: expose M_PI from <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>

#if defined(_WIN32)
#define NOMINMAX // keep windows.h from clobbering std::min/std::max used below
#include <windows.h> // MultiByteToWideChar / _wfopen for the non-ASCII path test
#endif

#include "qso_dmr_vocoder.h"

static const int kSampleRate = 8000;
static const int kFrameSamples = 160; // 20 ms
static const int kAmbeBytes = 7;

using std::vector;
using clock_type = std::chrono::steady_clock;
static double usBetween(clock_type::time_point a, clock_type::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

// ---- WAV I/O (8 kHz mono 16-bit PCM) ----
static vector<int16_t> readWav(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("  (could not open wav %s)\n", path.c_str()); return {}; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    vector<uint8_t> b(n); size_t rd = fread(b.data(), 1, n, f); fclose(f);
    (void)rd;
    size_t off = 12;
    while (off + 8 <= b.size()) {
        char id[5] = {0}; memcpy(id, &b[off], 4);
        uint32_t sz = b[off+4] | (b[off+5]<<8) | (b[off+6]<<16) | ((uint32_t)b[off+7]<<24);
        if (strcmp(id, "data") == 0) {
            size_t start = off + 8;
            size_t count = std::min<size_t>(sz/2, (b.size()-start)/2);
            vector<int16_t> out(count);
            for (size_t i = 0; i < count; i++)
                out[i] = (int16_t)(b[start+i*2] | (b[start+i*2+1]<<8));
            return out;
        }
        off += 8 + sz + (sz & 1);
    }
    return {};
}

static void writeWav(const std::string& path, const vector<int16_t>& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("  (could not write wav %s)\n", path.c_str()); return; }
    uint32_t dataBytes = (uint32_t)s.size() * 2;
    auto u32 = [&](uint32_t v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); fputc((v>>16)&0xff,f); fputc((v>>24)&0xff,f); };
    auto u16 = [&](uint16_t v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); };
    fwrite("RIFF",1,4,f); u32(36+dataBytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); u32(16); u16(1); u16(1); u32(kSampleRate); u32(kSampleRate*2); u16(2); u16(16);
    fwrite("data",1,4,f); u32(dataBytes);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

static vector<int16_t> genTone(double freq, double seconds, double amp) {
    int n = (int)lround(seconds * kSampleRate); n -= n % kFrameSamples;
    vector<int16_t> out(n);
    for (int i = 0; i < n; i++)
        out[i] = (int16_t)lround(amp * 32767 * sin(2*M_PI*freq*i/kSampleRate));
    return out;
}
static double rms(const vector<int16_t>& x) {
    if (x.empty()) return 0; double s=0; for (auto v:x) s+=(double)v*v; return sqrt(s/x.size());
}
static double goertzelRatio(const vector<int16_t>& x, double freq) {
    double w=2*M_PI*freq/kSampleRate, coeff=2*cos(w), s1=0,s2=0,total=0;
    for (auto v:x){ double d=v; double s0=d+coeff*s1-s2; s2=s1; s1=s0; total+=d*d; }
    double power=s1*s1+s2*s2-coeff*s1*s2; if (total<=0) return 0;
    return (power/(x.size()/2.0))/(total/x.size())/x.size();
}
static double frameRms(const vector<int16_t>& x, int f){
    double s=0; int base=f*kFrameSamples; for (int i=0;i<kFrameSamples;i++){ double v=x[base+i]; s+=v*v; } return sqrt(s/kFrameSamples);
}
static void envelopeCorrelation(const vector<int16_t>& a, const vector<int16_t>& b,
                                int maxFrameLag, double& bestCorr, int& bestLag) {
    int nf=(int)std::min(a.size(),b.size())/kFrameSamples;
    vector<double> ea(nf), eb(nf);
    for (int f=0;f<nf;f++){ ea[f]=frameRms(a,f); eb[f]=frameRms(b,f); }
    bestCorr=-2; bestLag=0;
    for (int lag=0; lag<=maxFrameLag; lag++){
        double ma=0,mb=0; int cnt=0;
        for (int f=0; f+lag<nf; f++){ ma+=ea[f]; mb+=eb[f+lag]; cnt++; }
        if (!cnt) continue; ma/=cnt; mb/=cnt;
        double sab=0,saa=0,sbb=0;
        for (int f=0; f+lag<nf; f++){ double av=ea[f]-ma, bv=eb[f+lag]-mb; sab+=av*bv; saa+=av*av; sbb+=bv*bv; }
        double c=(saa<=0||sbb<=0)?0:sab/sqrt(saa*sbb);
        if (c>bestCorr){ bestCorr=c; bestLag=lag; }
    }
}

static uint8_t ambe[kAmbeBytes];
static int16_t pcmIn[kFrameSamples], pcmOut[kFrameSamples];

static vector<int16_t> roundTrip(const vector<int16_t>& sig) {
    int nFrames = (int)sig.size()/kFrameSamples;
    vector<int16_t> out(nFrames*kFrameSamples);
    for (int f=0; f<nFrames; f++){
        memcpy(pcmIn, &sig[f*kFrameSamples], kFrameSamples*2);
        qdv_encode(ambe, pcmIn);
        qdv_decode(ambe, pcmOut);
        memcpy(&out[f*kFrameSamples], pcmOut, kFrameSamples*2);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: qdv_test <firmware.bin> <core.img> [speech_8k.wav] [out_dir] [wrapped_oem.bin]\n");
        return 2;
    }
    const char* fw   = argv[1];
    const char* core = argv[2];
    const char* speech = argc > 3 ? argv[3] : nullptr;
    std::string outDir = argc > 4 ? argv[4] : ".";
    const char* wrapped = argc > 5 ? argv[5] : nullptr;
    auto outPath = [&](const char* n){ return outDir + "/" + n; };
    auto readAll = [](const std::string& p){
        FILE* f = fopen(p.c_str(), "rb");
        std::vector<uint8_t> b;
        if (!f) return b;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        b.resize(n); size_t r = fread(b.data(), 1, n, f); (void)r; fclose(f); return b;
    };

    int fails = 0;

    printf("=== qso_dmr_vocoder ABI test (runtime firmware load) ===\n");
    printf("abi_version = %d (header declares %d)\n", qdv_abi_version(), QDV_ABI_VERSION);
    printf("build_id    = %s\n", qdv_build_id());
    if (qdv_abi_version() != QDV_ABI_VERSION) { printf("FAIL: ABI version mismatch\n"); fails++; }

    // ---- firmware unwrap (GPL-side, qdv_unwrap_md380) ----
    printf("\n--- UNWRAP (qdv_unwrap_md380) ---\n");
    {
        int urc = qdv_unwrap_md380("does_not_exist_wrapped.bin", outPath("x.bin").c_str());
        printf("unwrap(missing) -> %d (expect %d QDV_ERR_UNWRAP_OPEN) %s\n",
               urc, QDV_ERR_UNWRAP_OPEN, urc==QDV_ERR_UNWRAP_OPEN?"OK":"FAIL");
        if (urc != QDV_ERR_UNWRAP_OPEN) fails++;
    }
    // B3 (TA2IW / F4LZT): a NON-ASCII path must OPEN. Write a >512-byte file with
    // the OutSecurityBin magic to a path containing Turkish/French characters,
    // then unwrap it. The content is not a full valid image, so the CORRECT result
    // is QDV_ERR_UNWRAP_FORMAT (10) — the point is it is NOT QDV_ERR_UNWRAP_OPEN
    // (9): the open SUCCEEDED. Against the pre-fix narrow fopen this returned 9
    // (could not open the non-ASCII path); the wide-open fix returns 10.
    {
        const char* nonAscii = "unwrap_boyaci_François_\xC4\xB1.bin"; // UTF-8 ı + ç/ç
        std::string np = outPath(nonAscii);
        std::vector<uint8_t> synth(600, 0);
        memcpy(synth.data(), "OutSecurityBin", 14);
#if defined(_WIN32)
        int wl = MultiByteToWideChar(CP_UTF8, 0, np.c_str(), -1, nullptr, 0);
        std::wstring wp((size_t)wl, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, np.c_str(), -1, &wp[0], wl);
        FILE* nf = _wfopen(wp.c_str(), L"wb");
#else
        FILE* nf = fopen(np.c_str(), "wb");
#endif
        if (nf) { fwrite(synth.data(), 1, synth.size(), nf); fclose(nf); }
        int urc = qdv_unwrap_md380(np.c_str(), outPath("na_out.bin").c_str());
        printf("unwrap(non-ASCII path) -> %d (must NOT be %d UNWRAP_OPEN) %s\n",
               urc, QDV_ERR_UNWRAP_OPEN, urc!=QDV_ERR_UNWRAP_OPEN?"OK":"FAIL");
        if (urc == QDV_ERR_UNWRAP_OPEN) fails++;
    }
    if (wrapped) {
        std::string outp = outPath("D002.032.fromwrap.bin");
        int urc = qdv_unwrap_md380(wrapped, outp.c_str());
        printf("unwrap(OEM image) -> %d (expect %d QDV_OK) %s\n",
               urc, QDV_OK, urc==QDV_OK?"OK":"FAIL");
        if (urc != QDV_OK) { fails++; }
        else {
            auto got = readAll(outp), ref = readAll(fw);
            bool same = !got.empty() && got.size()==ref.size() &&
                        memcmp(got.data(), ref.data(), got.size())==0;
            printf("unwrapped %zu bytes; byte-identical to known-good D002.032.bin (%zu)? %s\n",
                   got.size(), ref.size(), same?"YES PASS":"NO FAIL");
            if (!same) fails++;
        }
    } else {
        printf("(no wrapped OEM image arg; skipping positive unwrap check)\n");
    }

    // ---- init failure contract ----
    printf("\n--- INIT CONTRACT (fallible init) ---\n");
    int rc;
    rc = qdv_init("does_not_exist_firmware.bin", core);
    printf("init(missing firmware) -> %d  (expect %d QDV_ERR_FIRMWARE_OPEN) %s\n",
           rc, QDV_ERR_FIRMWARE_OPEN, rc==QDV_ERR_FIRMWARE_OPEN?"OK":"FAIL");
    if (rc != QDV_ERR_FIRMWARE_OPEN) fails++;

    // wrong-size core (garbage file)
    {
        std::string g = outPath("garbage_core.bin");
        FILE* gf = fopen(g.c_str(), "wb"); if (gf){ const char z[100]={0}; fwrite(z,1,100,gf); fclose(gf); }
        rc = qdv_init(fw, g.c_str());
        printf("init(good fw, 100-byte core) -> %d  (expect %d QDV_ERR_CORE_SIZE) %s\n",
               rc, QDV_ERR_CORE_SIZE, rc==QDV_ERR_CORE_SIZE?"OK":"FAIL");
        if (rc != QDV_ERR_CORE_SIZE) fails++;
        remove(g.c_str());
    }

    // ---- successful init ----
    auto t0 = clock_type::now();
    rc = qdv_init(fw, core);
    auto t1 = clock_type::now();
    printf("init(valid firmware + core) -> %d  (expect %d QDV_OK) %s  [%.1f ms]\n",
           rc, QDV_OK, rc==QDV_OK?"OK":"FAIL", usBetween(t0,t1)/1000.0);
    if (rc != QDV_OK) { printf("FATAL: cannot init; aborting\n"); return 1; }

    rc = qdv_init(fw, core);
    printf("init(again, already init) -> %d  (expect %d QDV_ERR_ALREADY_INIT) %s\n",
           rc, QDV_ERR_ALREADY_INIT, rc==QDV_ERR_ALREADY_INIT?"OK":"FAIL");
    if (rc != QDV_ERR_ALREADY_INIT) fails++;

    // ---- correctness: tone ----
    printf("\n--- CORRECTNESS: 1 kHz tone (2.0 s) ---\n");
    auto tone = genTone(1000, 2.0, 0.3);
    auto toneOut = roundTrip(tone);
    writeWav(outPath("tone_in.wav"), tone);
    writeWav(outPath("tone_out.wav"), toneOut);
    double toneRatio = goertzelRatio(toneOut, 1000);
    printf("input  RMS=%.1f  1kHz-fraction=%.3f\n", rms(tone), goertzelRatio(tone,1000));
    printf("output RMS=%.1f  1kHz-fraction=%.3f\n", rms(toneOut), toneRatio);
    bool tonePass = rms(toneOut) >= 50 && toneRatio > 0.3;
    printf("tone verdict: %s\n", tonePass ? "PASS (tone in -> tone out)" : "FAIL");
    if (!tonePass) fails++;

    // ---- correctness: speech ----
    if (speech) {
        printf("\n--- CORRECTNESS: real speech clip ---\n");
        auto full = readWav(speech);
        if (!full.empty()) {
            int n = std::min<int>((int)full.size(), kSampleRate*4); n -= n % kFrameSamples;
            vector<int16_t> sp(full.begin(), full.begin()+n);
            auto spOut = roundTrip(sp);
            writeWav(outPath("speech_in.wav"), sp);
            writeWav(outPath("speech_out.wav"), spOut);
            double env; int envLag; envelopeCorrelation(sp, spOut, 6, env, envLag);
            printf("%.2f s, %d frames | in RMS=%.1f out RMS=%.1f\n",
                   sp.size()/(double)kSampleRate, (int)sp.size()/kFrameSamples, rms(sp), rms(spOut));
            printf("loudness-envelope correlation=%.3f @ %d-frame lag\n", env, envLag);
            bool spPass = rms(spOut) >= 50 && env > 0.8;
            printf("speech verdict: %s\n", spPass ? "PASS (envelope tracks input)" : "FAIL");
            if (!spPass) fails++;
        } else { printf("(speech wav unreadable, skipping)\n"); }
    }

    // ---- latency sanity ----
    printf("\n--- LATENCY (sanity, 1000 calls) ---\n");
    auto probe = genTone(700, 0.02, 0.3);
    memcpy(pcmIn, probe.data(), kFrameSamples*2); qdv_encode(ambe, pcmIn);
    uint8_t pb[kAmbeBytes]; memcpy(pb, ambe, kAmbeBytes);
    auto timeIt = [&](bool enc){
        const int N=1000; vector<double> v(N);
        for (int i=0;i<N;i++){
            if (enc){ memcpy(pcmIn, probe.data(), kFrameSamples*2); auto a=clock_type::now(); qdv_encode(ambe, pcmIn); v[i]=usBetween(a,clock_type::now()); }
            else    { memcpy(ambe, pb, kAmbeBytes); auto a=clock_type::now(); qdv_decode(ambe, pcmOut); v[i]=usBetween(a,clock_type::now()); }
        }
        double m=0; for (int i=1;i<N;i++) m+=v[i]; return m/(N-1);
    };
    double encMean = timeIt(true), decMean = timeIt(false);
    printf("encode steady mean=%.1f us  decode steady mean=%.1f us  frame=%.1f us (%.2f%% of 20ms)\n",
           encMean, decMean, encMean+decMean, (encMean+decMean)/20000.0*100);

    qdv_shutdown();
    printf("\nqdv_shutdown() ok\n");
    printf("\n=== %s (%d check(s) failed) ===\n", fails==0?"ALL PASS":"FAILURES", fails);
    return fails==0 ? 0 : 1;
}
