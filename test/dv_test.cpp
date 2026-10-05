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
 *   6. the per-frame CPU time of all four operations (printed, not asserted).
 *
 * Usage: dv_test <male_8k.wav> <female_8k.wav>
 *        (the clips in test/clips/; the quiet and loud clips are derived)
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

using std::vector;
using namespace qdv_dv;

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
