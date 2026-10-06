/*
 * dv_test.cpp - the D-Star / P25 (dv mode) codec test. GPL v3 or later.
 *
 * Proves, on whatever machine it runs on (Windows host, Android CLI):
 *   1. frame sizes: D-Star 9 bytes, P25 11 bytes per 20 ms;
 *   2. digital silence in -> near silence out, and the standard D-Star null
 *      frame 9E 8D 32 88 26 1A 3F 61 E8 decodes to near silence;
 *   3. THE D-STAR LOUDNESS FIX: decoded level tracks the input level (within
 *      kTrackDb on the normal and quiet clips) and the loud clip does not
 *      clip. Before the fix D-Star came out 13 to 28 dB too loud with
 *      thousands of clipped samples, so reverting the fix fails this test;
 *   4. P25 level tracks the input the same way (P25 was already fine);
 *   5. encoder / decoder reset gives a repeatable stream (same input -> same
 *      frames after a reset);
 *   6. the per-frame CPU time of all four operations (printed, not asserted);
 *   7. THE D-STAR PITCH FIX: the decoded voice keeps the input pitch within
 *      +/-1.5 % (before the fix D-Star played every voice 2.5 to 5 % sharp,
 *      the "chipmunk" echo on XLX073 E, so reverting the fix fails this test).
 *   8. (2026-10-06) the encoder's model of the decoder equals mbelib's decoder
 *      state after every frame (the b8 fix: before it, they split on most frames);
 *   9. (2026-10-06, optional args) on band-limited real speech the energy at
 *      3.7-4.0 kHz stays within +2 dB of the input (the top-band fade);
 *  10. (2026-10-06, optional args) REAL D-Star radio frames (G4KLX's en_US
 *      prompts) play at the pitch of the same recording in DMR AMBE+2 (the real-
 *      radio pitch scale: mbelib's "w0 guess" played them 3 % flat).
 *
 * Usage: dv_test <male_8k.wav> <female_8k.wav> [<dstar prompts> <dmr prompts>]
 *        (the clips in test/clips/; the quiet and loud clips are derived;
 *        the prompt files in test/refframes/)
 * Exit 0 = PASS.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dv_codec_core.h"
// Sections 8 and 10 use the vendored coder class directly (its decoder state is
// private; the test reads it to compare the encoder's model with the decoder).
#include <cinttypes>
#define private public
#include "mbevocoder.h"
#include "mbelib.h"
#undef private

using std::vector;
using namespace qdv_dv;

static vector<uint8_t> readFile(const char* path) {
  vector<uint8_t> b;
  FILE* f = fopen(path, "rb");
  if (!f) return b;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b.resize(n > 0 ? (size_t)n : 0);
  if (n > 0) fread(b.data(), 1, (size_t)n, f);
  fclose(f);
  return b;
}

// Energy at 3.7-4.0 kHz relative to 0.3-3.0 kHz (dB), over the active 32 ms blocks.
static double topBandDb(const vector<int16_t>& x) {
  const int N = 256;
  double top = 0, ref = 0, emax = 0;
  vector<double> e;
  for (size_t s = 0; s + N <= x.size(); s += N) {
    double v = 0;
    for (int i = 0; i < N; i++) v += (double)x[s + i] * x[s + i];
    e.push_back(v);
    emax = std::max(emax, v);
  }
  for (size_t b = 0; b < e.size(); b++) {
    if (e[b] < emax * 1e-3) continue;
    for (int k = 1; k < N / 2; k++) {
      double re = 0, im = 0;
      for (int i = 0; i < N; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / N);
        double v = x[b * N + i] * w;
        re += v * cos(2 * M_PI * k * i / N);
        im -= v * sin(2 * M_PI * k * i / N);
      }
      double p = re * re + im * im, hz = k * 8000.0 / N;
      if (hz >= 300 && hz < 3000) ref += p;
      if (hz >= 3700 && hz < 4000) top += p;
    }
  }
  return 10 * log10(top / (ref + 1e-30) + 1e-30);
}

static const double kTrackDb = 4.0;      // |out - in| active level, dB
static const size_t kLoudClipMax = 50;   // samples at full scale (was ~8700)

static vector<int16_t> readWav(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  vector<uint8_t> b((size_t)n);
  size_t rd = fread(b.data(), 1, b.size(), f);
  fclose(f);
  b.resize(rd);
  size_t off = 12;
  while (off + 8 <= b.size()) {
    uint32_t sz = b[off + 4] | (b[off + 5] << 8) | (b[off + 6] << 16) | ((uint32_t)b[off + 7] << 24);
    if (memcmp(&b[off], "data", 4) == 0) {
      size_t start = off + 8;
      size_t count = std::min<size_t>(sz / 2, (b.size() - start) / 2);
      vector<int16_t> out(count);
      for (size_t i = 0; i < count; i++) out[i] = (int16_t)(b[start + i * 2] | (b[start + i * 2 + 1] << 8));
      return out;
    }
    off += 8 + sz + (sz & 1);
  }
  return {};
}

static vector<int16_t> scaled(const vector<int16_t>& in, double db) {
  double g = pow(10.0, db / 20.0);
  vector<int16_t> o(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    double v = in[i] * g;
    o[i] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v)));
  }
  return o;
}

struct Level { double active; size_t clipped; };

// Active level: mean power of the 20 ms frames within 40 dB of the loudest one
// (the bench's "actRMS"), in dBFS. Clipped: samples with |x| >= 32760.
static Level level(const vector<int16_t>& p) {
  vector<double> fr;
  size_t clip = 0;
  for (int16_t v : p) if (std::abs((int)v) >= 32760) clip++;
  for (size_t i = 0; i + 160 <= p.size(); i += 160) {
    double e = 0;
    for (size_t k = 0; k < 160; k++) e += (double)p[i + k] * p[i + k];
    fr.push_back(e / 160);
  }
  double mx = fr.empty() ? 0 : *std::max_element(fr.begin(), fr.end());
  double s = 0;
  size_t n = 0;
  for (double e : fr) if (e > mx * 1e-4) { s += e; n++; }
  return {n ? 10 * log10(s / n / (32768.0 * 32768.0) + 1e-20) : -200.0, clip};
}

// ---- 7. pitch tracking (the D-Star "chipmunk" fix, 2026-10-05) ----
// YIN pitch per 10 ms hop (40 ms window, 55..420 Hz, threshold 0.12). Returns
// 0 for an unvoiced / silent hop.
static vector<double> yinTrack(const vector<int16_t>& x, vector<double>* rmsDb) {
  const int W = 320, H = 80, tmin = 8000 / 420, tmax = 8000 / 55;
  vector<double> f0;
  vector<double> d(tmax + 1), cm(tmax + 1);
  for (size_t s = 0; s + W + tmax < x.size(); s += H) {
    double e = 0;
    for (int i = 0; i < W; i++) e += (double)x[s + i] * x[s + i];
    if (rmsDb) rmsDb->push_back(10 * log10(e / W / (32768.0 * 32768.0) + 1e-20));
    d[0] = 0;
    for (int t = 1; t <= tmax; t++) {
      double acc = 0;
      for (int i = 0; i < W; i++) {
        double df = (double)x[s + i] - x[s + i + t];
        acc += df * df;
      }
      d[t] = acc;
    }
    double run = 0;
    cm[0] = 1;
    for (int t = 1; t <= tmax; t++) {
      run += d[t];
      cm[t] = run > 0 ? d[t] * t / run : 1;
    }
    int tau = -1;
    for (int t = tmin; t < tmax; t++) {
      if (cm[t] < 0.12) {
        while (t + 1 < tmax && cm[t + 1] < cm[t]) t++;
        tau = t;
        break;
      }
    }
    if (tau < 1) { f0.push_back(0); continue; }
    double y0 = cm[tau - 1], y1 = cm[tau], y2 = cm[tau + 1], den = y0 - 2 * y1 + y2;
    double sh = den != 0 ? 0.5 * (y0 - y2) / den : 0;
    f0.push_back(8000.0 / (tau + sh));
  }
  return f0;
}

// Median out/in pitch ratio over the hops voiced in both (output aligned by
// the lag, 0..300 ms, that best matches the loudness envelopes; only ratios
// inside 0.8..1.25 count, so this measures a pitch SHIFT, not octave jumps).
static double pitchRatio(const vector<int16_t>& in, const vector<int16_t>& out, size_t* used) {
  vector<double> ri, ro;
  vector<double> fi = yinTrack(in, &ri), fo = yinTrack(out, &ro);
  int bestLag = 0;
  double best = -1e300;
  for (int lag = 0; lag < 30; lag++) {
    double num = 0;
    size_t n = std::min(ri.size(), ro.size() - std::min(ro.size(), (size_t)lag));
    for (size_t i = 0; i < n; i++) num += ri[i] * ro[i + lag];
    if (num > best) { best = num; bestLag = lag; }
  }
  double mx = -1e300;
  for (double v : ri) mx = std::max(mx, v);
  vector<double> r;
  for (size_t i = 0; i < fi.size() && i + bestLag < fo.size(); i++) {
    if (ri[i] < mx - 35 || fi[i] <= 0 || fo[i + bestLag] <= 0) continue;
    double q = fo[i + bestLag] / fi[i];
    if (q > 0.8 && q < 1.25) r.push_back(q);
  }
  *used = r.size();
  if (r.empty()) return 0;
  std::sort(r.begin(), r.end());
  return r[r.size() / 2];
}

static const double kPitchTol = 0.015;  // +/-1.5 % (the D-Star bug was +2.5..+4.9 %)

struct Timing {
  double sum = 0, mx = 0;
  size_t n = 0;
  void add(double us) { sum += us; mx = std::max(mx, us); n++; }
};

static vector<int16_t> roundTrip(int kind, const vector<int16_t>& in, Timing& te, Timing& td,
                                 vector<uint8_t>* framesOut = nullptr) {
  Codecs c;
  const int fb = frameBytes(kind);
  vector<int16_t> out;
  size_t nf = in.size() / 160;
  out.resize(nf * 160);
  for (size_t f = 0; f < nf; f++) {
    uint8_t fr[16];
    memset(fr, 0xAA, sizeof fr);
    auto t0 = std::chrono::steady_clock::now();
    c.encode(kind, &in[f * 160], fr);
    auto t1 = std::chrono::steady_clock::now();
    c.decode(kind, fr, &out[f * 160]);
    auto t2 = std::chrono::steady_clock::now();
    te.add(std::chrono::duration<double, std::micro>(t1 - t0).count());
    td.add(std::chrono::duration<double, std::micro>(t2 - t1).count());
    if (framesOut) framesOut->insert(framesOut->end(), fr, fr + fb);
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: dv_test <male_8k.wav> <female_8k.wav>\n");
    return 2;
  }
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
  };

  vector<int16_t> male = readWav(argv[1]), female = readWav(argv[2]);
  if (male.size() < 8000 || female.size() < 8000) {
    printf("could not read clips\n");
    return 2;
  }
  struct Clip { const char* name; vector<int16_t> pcm; };
  vector<Clip> clips = {
      {"male", male},
      {"female", female},
      {"quiet (male -20 dB)", scaled(male, -20)},
      {"loud (female +12 dB, clipped)", scaled(female, 12)},
  };

  // 1. frame sizes
  printf("frame sizes\n");
  check(frameBytes(kDstar) == 9, "D-Star frame is 9 bytes");
  check(frameBytes(kP25) == 11, "P25 frame is 11 bytes");
  check(frameBytes(7) == 0, "unknown kind has no frame size");

  // 2. silence and the standard D-Star null frame
  printf("silence\n");
  {
    vector<int16_t> z(160 * 50, 0);
    for (int kind : {(int)kDstar, (int)kP25}) {
      Timing a, b;
      vector<int16_t> o = roundTrip(kind, z, a, b);
      int peak = 0;
      for (int16_t s : o) peak = std::max(peak, std::abs((int)s));
      char m[96];
      snprintf(m, sizeof m, "%s digital silence -> peak %d (< 300)", kind == kDstar ? "D-Star" : "P25", peak);
      check(peak < 300, m);
    }
    static const uint8_t kNull[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};
    Codecs c;
    int peak = 0;
    int16_t pcm[160];
    for (int k = 0; k < 25; k++) {
      c.decode(kDstar, kNull, pcm);
      for (int16_t s : pcm) peak = std::max(peak, std::abs((int)s));
    }
    char m[96];
    snprintf(m, sizeof m, "D-Star null frame x25 -> peak %d (< 300)", peak);
    check(peak < 300, m);
  }

  // 3 + 4. level tracking
  Timing de, dd, pe, pd;
  for (auto& cl : clips) {
    Level li = level(cl.pcm);
    printf("clip %s: in active %.1f dBFS, clipped %zu\n", cl.name, li.active, li.clipped);
    const bool loud = cl.name[0] == 'l';
    for (int kind : {(int)kDstar, (int)kP25}) {
      Timing& te = kind == kDstar ? de : pe;
      Timing& td = kind == kDstar ? dd : pd;
      Level lo = level(roundTrip(kind, cl.pcm, te, td));
      const char* kn = kind == kDstar ? "D-Star" : "P25";
      double diff = lo.active - li.active;
      char m[160];
      snprintf(m, sizeof m, "%s out active %.1f dBFS (change %+.1f dB, limit +/-%.0f), clipped %zu",
               kn, lo.active, diff, kTrackDb, lo.clipped);
      check(std::fabs(diff) <= kTrackDb, m);
      if (loud) {
        snprintf(m, sizeof m, "%s loud clip: %zu clipped samples out (limit %zu; input had %zu)", kn,
                 lo.clipped, kLoudClipMax, li.clipped);
        check(lo.clipped <= kLoudClipMax, m);
      } else {
        snprintf(m, sizeof m, "%s no clipping on a normal clip (%zu)", kn, lo.clipped);
        check(lo.clipped == 0, m);
      }
    }
  }

  // 5. reset gives a repeatable stream
  printf("reset\n");
  for (int kind : {(int)kDstar, (int)kP25}) {
    Codecs c;
    const int fb = frameBytes(kind);
    vector<uint8_t> a, b;
    for (int pass = 0; pass < 2; pass++) {
      vector<uint8_t>& dst = pass ? b : a;
      for (size_t f = 0; f < 100; f++) {
        uint8_t fr[16];
        c.encode(kind, &male[f * 160], fr);
        dst.insert(dst.end(), fr, fr + fb);
      }
      c.reset(kind, 1);
    }
    char m[96];
    snprintf(m, sizeof m, "%s encoder reset -> identical frames for identical input",
             kind == kDstar ? "D-Star" : "P25");
    check(a == b, m);
  }

  // 7. pitch: the decoded voice keeps the speaker's pitch. Before the D-Star
  // pitch fix the encoder picked b0 from a table built for the DMR pitch scale
  // and the decoder plays b0 on the D-Star scale, so every voice came back 2.5
  // to 5 % sharp ("chipmunk"). P25 is the control (it never had the bug).
  printf("pitch (out/in, median over voiced 10 ms hops, limit +/-%.1f %%)\n", kPitchTol * 100);
  for (int ci = 0; ci < 2; ci++) {
    for (int kind : {(int)kDstar, (int)kP25}) {
      Timing a, b;
      vector<int16_t> o = roundTrip(kind, clips[ci].pcm, a, b);
      size_t used = 0;
      double q = pitchRatio(clips[ci].pcm, o, &used);
      char m[160];
      snprintf(m, sizeof m, "%s %s pitch ratio %.3f (%zu voiced hops)", kind == kDstar ? "D-Star" : "P25",
               clips[ci].name, q, used);
      check(used >= 100 && std::fabs(q - 1.0) <= kPitchTol, m);
    }
  }

  // 8. THE ENCODER'S MODEL OF THE DECODER IS THE DECODER (2026-10-06). The encoder
  // predicts each frame from its own copy of the decoder state; if that copy differs
  // from what a real decoder computes from the same bits, every later frame is coded
  // against the wrong history. Before the b8 fix the encoder searched 16 entries for
  // b8 but sent only its LOW three bits while decoders read them as the TOP three, so
  // the two states split on most frames. Decoder = mbelib's own D-Star decoder.
  printf("encoder model vs decoder (spectral state after every frame)\n");
  for (int ci = 0; ci < 2; ci++) {
    MBEVocoder enc, dec;
    size_t frames = 0, split = 0;
    double worst = 0;
    for (size_t f = 0; f + 1 <= clips[ci].pcm.size() / 160; f++) {
      int16_t in[160], out[160];
      memcpy(in, &clips[ci].pcm[f * 160], sizeof in);
      uint8_t fr[9];
      memset(fr, 0, sizeof fr);
      enc.encode_2400x1200(in, fr);
      dec.decode_2400x1200(out, fr);
      const mbe_parms* a = enc.m_mbelibParms->m_prev_mp;
      const mbe_parms* b = dec.m_mbelibParms->m_prev_mp;
      if (a->L != b->L || std::fabs(a->w0 - b->w0) > 1e-5f) { split++; frames++; continue; }
      double d = 0;
      for (int l = 1; l <= a->L; l++) d = std::max(d, (double)std::fabs(a->log2Ml[l] - b->log2Ml[l]));
      worst = std::max(worst, d);
      if (d > 0.01) split++;
      frames++;
    }
    char m[192];
    snprintf(m, sizeof m, "D-Star %s: encoder state == decoder state on %zu of %zu frames (worst %.4f)",
             clips[ci].name, frames - split, frames, worst);
    check(split == 0, m);
  }

  // 10. THE REAL-RADIO PITCH SCALE (2026-10-06). Optional args 3 and 4: G4KLX's en_US
  // voice prompts as made by real DVSI encoders, D-Star (ircDDBGateway) and DMR AMBE+2
  // (DMRGateway), the SAME recording (test/refframes/). The DMR pitch scale is exact
  // (DVSI's MD-380 firmware round-trips it at 0.999), so the D-Star frames, played by
  // our decoder, must come out at the DMR rendition's pitch. mbelib's "w0 guess" played
  // them 3 % flat (0.97); the real-radio scale plays them at 1.00.
  if (argc >= 5) {
    printf("real D-Star radio frames vs the same recording in DMR (pitch, limit +/-1.5 %%)\n");
    vector<uint8_t> ds = readFile(argv[3]), dm = readFile(argv[4]);
    size_t o = (ds.size() >= 4 && !memcmp(ds.data(), "AMBE", 4)) ? 4 : 0;
    MBEVocoder d1, d2;
    vector<int16_t> pds, pdm;
    for (size_t i = o; i + 9 <= ds.size(); i += 9) {
      int16_t pcm[160];
      uint8_t f[9];
      memcpy(f, &ds[i], 9);
      memset(pcm, 0, sizeof pcm);
      d1.decode_2400x1200(pcm, f);
      pds.insert(pds.end(), pcm, pcm + 160);
    }
    for (size_t i = 0; i + 9 <= dm.size(); i += 9) {
      int16_t pcm[160];
      uint8_t f[9];
      memcpy(f, &dm[i], 9);
      memset(pcm, 0, sizeof pcm);
      d2.decode_2450x1150(pcm, f);
      pdm.insert(pdm.end(), pcm, pcm + 160);
    }
    // the D-Star file runs 14 frames (280 ms) ahead of the DMR file
    if (pds.size() > 14 * 160) pds.erase(pds.begin(), pds.begin() + 14 * 160);
    size_t used = 0;
    double q = pitchRatio(pdm, pds, &used);
    char m[160];
    snprintf(m, sizeof m, "real D-Star frames play at %.3f x the DMR rendition's pitch (%zu voiced hops)", q, used);
    check(used >= 300 && std::fabs(q - 1.0) <= kPitchTol, m);

    // 9. THE TOP-BAND FADE (2026-10-06), on band-limited real speech (the DMR
    // rendition above: a real voice, nothing above ~3.7 kHz, like a microphone
    // resampled to 8 kHz). The decoder's top harmonics reach 3.9-4.0 kHz but the
    // IMBE analysis stops near 3.7 kHz; holding the last analysed amplitude up
    // there put more energy at 3.7-4.0 kHz than the input had (+3 dB on this clip,
    // +7 to +19 dB on other real speakers). Limit: +2 dB (fixed: -1.5 Windows, +0.8 Android).
    printf("top band 3.7-4.0 kHz (relative to 0.3-3 kHz) on band-limited real speech\n");
    Timing ta, tb;
    vector<int16_t> o2 = roundTrip(kDstar, pdm, ta, tb);
    double di = topBandDb(pdm), dout = topBandDb(o2);
    snprintf(m, sizeof m, "D-Star top band %+.1f dB vs input (in %.1f, out %.1f; limit +2)", dout - di, di, dout);
    check(dout - di <= 2.0, m);
  }

  // 6. CPU time per frame (all clips)
  printf("cpu per 20 ms frame (this machine)\n");
  auto pr = [](const char* n, const Timing& t) {
    printf("  %-14s mean %7.1f us  max %8.1f us  (n=%zu)\n", n, t.sum / t.n, t.mx, t.n);
  };
  pr("dstar-encode", de);
  pr("dstar-decode", dd);
  pr("p25-encode", pe);
  pr("p25-decode", pd);

  printf("dv_test %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
