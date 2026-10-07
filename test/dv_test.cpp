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
 *  11. (2026-10-06 pm) our decoder plays each pitch code within 0.3 % of a real DVSI chip;
 *  12. (2026-10-06 pm) the encoder moves the loudness index <= 5 steps per frame (chip 3.9);
 *  13. (2026-10-06 pm, optional arg 5) real D-Star frames decode at a real DVSI decoder's
 *      level (+/-3 dB) with a per-frame spread under 4.5 dB (chip reference file);
 *  14. (2026-10-06 evening, optional args 6-7) our decoder plays the SAME harmonic count as a
 *      real DVSI chip for every pitch code (the voice-shape fault: mbelib's table had 1-4 more);
 *  15. (2026-10-06 evening, optional arg 8) real D-Star frames: our decoder's formants F1-F3
 *      within 1 % of the chip's decode of the same frames;
 *  16. (optional args 9-10) our encoder's frames vs the chip encoder's frames of the same clips,
 *      both played by our decoder: formants within 1.5 %;
 *  10. (2026-10-06, optional args) REAL D-Star radio frames (G4KLX's en_US
 *      prompts) play at the pitch of the same recording in DMR AMBE+2 (the real-
 *      radio pitch scale: mbelib's "w0 guess" played them 3 % flat).
 *
 * Usage: dv_test <male_8k.wav> <female_8k.wav> [<dstar prompts> <dmr prompts> [<chip level ref>
 *        [<harmonic probe .ambe> <chip harmonics .txt> [<chip shape track .txt>
 *        [<male clip, chip-encoded .ambe> <female clip, chip-encoded .ambe>]]]]]
 *        (the clips in test/clips/; the quiet and loud clips are derived;
 *        the prompt files in test/refframes/)
 * Exit 0 = PASS.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
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

// ---- Voice shape (2026-10-06 evening, sections 14-16) -------------------------------
// Per 10 ms hop (32 ms Hamming frames): level (dB), voiced flag (active within 25 dB of
// the loudest frame and normalised autocorrelation peak >= 0.5 at a 60-400 Hz lag) and
// the three lowest LPC formants (order 10, pre-emphasis 0.9; roots with bandwidth under
// 500 Hz between 150 and 3800 Hz). Same rules as the bench's shape.py.
struct ShapeHop { double e; int v; double F[3]; };

static vector<std::complex<double>> polyRoots(const vector<double>& a) {
  // monic a[0] = 1: z^p + a1 z^(p-1) + ... + ap; Durand-Kerner
  const int p = (int)a.size() - 1;
  vector<std::complex<double>> r(p);
  for (int i = 0; i < p; i++) r[i] = std::polar(0.9, 2 * M_PI * (i + 0.25) / p);
  for (int it = 0; it < 500; it++) {
    double moved = 0;
    for (int i = 0; i < p; i++) {
      std::complex<double> num = 1.0;
      for (int k = 1; k <= p; k++) num = num * r[i] + a[k];
      std::complex<double> den = 1.0;
      for (int j = 0; j < p; j++) if (j != i) den *= (r[i] - r[j]);
      std::complex<double> d = num / den;
      r[i] -= d;
      moved = std::max(moved, std::abs(d));
    }
    if (moved < 1e-12) break;
  }
  return r;
}

static vector<ShapeHop> shapeTrack(const vector<int16_t>& x) {
  const int N = 256, H = 80, FS = 8000;
  vector<ShapeHop> out;
  if (x.size() < (size_t)N) return out;
  const size_t m = (x.size() - N) / H;
  vector<double> w(N);
  for (int i = 0; i < N; i++) w[i] = 0.54 - 0.46 * std::cos(2 * M_PI * i / (N - 1));
  double emax = -1e9;
  for (size_t f = 0; f < m; f++) {
    double s = 0;
    for (int i = 0; i < N; i++) s += (double)x[f * H + i] * x[f * H + i];
    ShapeHop h{10 * std::log10(s / N + 1e-9), 0, {0, 0, 0}};
    emax = std::max(emax, h.e);
    out.push_back(h);
  }
  for (size_t f = 0; f < m; f++) {
    if (out[f].e <= emax - 25) continue;
    vector<double> fr(N);
    double mean = 0;
    for (int i = 0; i < N; i++) mean += x[f * H + i];
    mean /= N;
    for (int i = 0; i < N; i++) fr[i] = x[f * H + i] - mean;
    double r0 = 0;
    for (int i = 0; i < N; i++) r0 += fr[i] * fr[i];
    if (r0 <= 0) continue;
    double best = -2;
    for (int k = FS / 400; k < FS / 60; k++) {
      double s = 0;
      for (int i = 0; i + k < N; i++) s += fr[i] * fr[i + k];
      best = std::max(best, s / r0);
    }
    if (best < 0.5) continue;
    // LPC (autocorrelation, Levinson) on the pre-emphasised, windowed frame
    vector<double> g(N);
    g[0] = x[f * H] * w[0];
    for (int i = 1; i < N; i++) g[i] = (x[f * H + i] - 0.9 * x[f * H + i - 1]) * w[i];
    const int P = 10;
    double R[P + 1];
    for (int k = 0; k <= P; k++) {
      double s = 0;
      for (int i = 0; i + k < N; i++) s += g[i] * g[i + k];
      R[k] = s;
    }
    R[0] *= 1.0001;
    vector<double> a(P + 1, 0.0);
    a[0] = 1;
    double err = R[0];
    for (int i = 1; i <= P; i++) {
      double acc = R[i];
      for (int j = 1; j < i; j++) acc += a[j] * R[i - j];
      double k = -acc / err;
      vector<double> na = a;
      for (int j = 1; j < i; j++) na[j] = a[j] + k * a[i - j];
      na[i] = k;
      a = na;
      err *= (1 - k * k);
    }
    vector<double> fq;
    for (auto& z : polyRoots(a)) {
      if (z.imag() <= 0) continue;
      double hz = std::arg(z) * FS / (2 * M_PI), bw = -std::log(std::abs(z)) * FS / M_PI;
      if (bw < 500 && hz > 150 && hz < 3800) fq.push_back(hz);
    }
    std::sort(fq.begin(), fq.end());
    if (fq.size() < 3) continue;
    out[f].v = 1;
    for (int j = 0; j < 3; j++) out[f].F[j] = fq[j];
  }
  return out;
}

// Median ratio ours/ref of F1..F3 over hops voiced in both; a pair counts when the two
// agree within 25 % (the same formant). Alignment (one lag for the whole file, +/-30 hops):
// the lag at which the most voiced hops carry the same F2 within 5 % (two decoders' level
// contours and waveforms do not line up exactly; the formant track does).
static void shapeRatios(const vector<ShapeHop>& ours, const vector<ShapeHop>& ref, double q[3], size_t* used) {
  int bestLag = 0;
  size_t bestN = 0;
  for (int lag = -30; lag <= 30; lag++) {
    size_t n = 0;
    for (size_t i = 0; i < ref.size(); i++) {
      long j = (long)i + lag;
      if (j < 0 || j >= (long)ours.size() || !ref[i].v || !ours[j].v) continue;
      if (std::fabs(ours[j].F[1] / ref[i].F[1] - 1.0) < 0.05) n++;
    }
    if (n > bestN) { bestN = n; bestLag = lag; }
  }
  *used = 0;
  for (int j = 0; j < 3; j++) {
    vector<double> r;
    for (size_t i = 0; i < ref.size(); i++) {
      long k = (long)i + bestLag;
      if (k < 0 || k >= (long)ours.size() || !ref[i].v || !ours[k].v) continue;
      double v = ours[k].F[j] / ref[i].F[j];
      if (v > 0.8 && v < 1.25) r.push_back(v);
    }
    std::sort(r.begin(), r.end());
    q[j] = r.empty() ? 0 : r[r.size() / 2];
    if (j == 0) *used = r.size();
  }
}

static vector<ShapeHop> readShapeTrack(const char* path) {
  vector<ShapeHop> t;
  FILE* f = fopen(path, "r");
  if (!f) return t;
  char line[160];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    ShapeHop h{0, 0, {0, 0, 0}};
    if (sscanf(line, "%lf %d %lf %lf %lf", &h.e, &h.v, &h.F[0], &h.F[1], &h.F[2]) == 5) t.push_back(h);
  }
  fclose(f);
  return t;
}

static vector<int16_t> decodeDstarFile(const char* path) {
  vector<uint8_t> ds = readFile(path);
  size_t o = (ds.size() >= 4 && !memcmp(ds.data(), "AMBE", 4)) ? 4 : 0;
  MBEVocoder d;
  vector<int16_t> out;
  for (size_t i = o; i + 9 <= ds.size(); i += 9) {
    int16_t pcm[160];
    uint8_t fr[9];
    memcpy(fr, &ds[i], 9);
    d.decode_2400x1200(pcm, fr);
    out.insert(out.end(), pcm, pcm + 160);
  }
  return out;
}

int main(int argc, char** argv) {
  // Reference maker (not a test): dv_test --shape-track <in.wav> <out.txt>
  if (argc == 4 && !strcmp(argv[1], "--shape-track")) {
    auto t = shapeTrack(readWav(argv[2]));
    FILE* f = fopen(argv[3], "w");
    if (!f || t.empty()) return 2;
    fprintf(f, "# dv_test voice-shape track (10 ms hops): level_dB voiced F1 F2 F3. QSO One dv_test pin 15.\n");
    for (auto& h : t) fprintf(f, "%.2f %d %.1f %.1f %.1f\n", h.e, h.v, h.F[0], h.F[1], h.F[2]);
    fclose(f);
    printf("%zu hops\n", t.size());
    return 0;
  }
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

  // 11. THE CHIP-MEASURED PITCH SCALE (2026-10-06 afternoon). A real DVSI AMBE-3000
  // (DVMEGA DVstick 30) decoding frames with the pitch code held at b0 played these
  // pitches (Hz; harmonic comb fit, 0.3 cent fit residual). Our decoder must play each
  // code within 0.3 %. The morning's "two codes" scale was 1.0-2.0 % off below 125 Hz
  // and 1.0-1.4 % off above 300 Hz (FAIL); mbelib's guess was 1-4.4 % off.
  {
    static const int kB0[] = {4, 20, 36, 52, 68, 84, 100, 116};
    static const double kChipHz[] = {393.55, 309.20, 242.86, 190.79, 149.84, 117.74, 92.53, 72.63};
    printf("D-Star pitch per code vs a real DVSI chip (limit +/-0.3 %%)\n");
    double worst = 0;
    int worstB0 = -1;
    for (int i = 0; i < 8; i++) {
      // the w0 the decoder (mbe_decodeAmbe2400Parms) sets for a frame carrying code b0
      char d[49];
      memset(d, 0, sizeof d);
      for (int k = 0; k < 6; k++) d[k] = (kB0[i] >> (6 - k)) & 1;
      d[48] = kB0[i] & 1;
      mbe_parms cur, prev, prevE;
      mbe_initMbeParms(&cur, &prev, &prevE);
      mbe_decodeAmbe2400Parms(d, &cur, &prev);
      double hz = cur.w0 / (2 * M_PI) * 8000.0;
      double err = std::fabs(hz / kChipHz[i] - 1.0);
      if (err > worst) { worst = err; worstB0 = kB0[i]; }
    }
    char m[160];
    snprintf(m, sizeof m, "decoder pitch per code within %.2f %% of the chip (worst at b0 %d)", worst * 100, worstB0);
    check(worst <= 0.003, m);
  }

  // 12. THE ENCODER'S LOUDNESS MOVEMENT (2026-10-06 afternoon). On the same speech a real
  // DVSI encoder moves the loudness index b2 3.9 steps per frame (mean |change|); ours moved
  // 7.6, which the chip's decoder played as a 5.4 dB rms frame-to-frame loudness wobble (a
  // real radio's own frames: 2.8). Limit 5.0 steps on the male clip.
  {
    MBEVocoder enc, dec;
    int prev = -1;
    double sum = 0;
    size_t n = 0;
    for (size_t f = 0; f + 1 <= male.size() / 160; f++) {
      uint8_t fr[9];
      int16_t out[160];
      memset(fr, 0, sizeof fr);
      enc.encode_2400x1200(&male[f * 160], fr);
      dec.decode_2400x1200(out, fr);
      const char* d = dec.ambe_d;
      int b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) | (d[4] << 2) | (d[5] << 1) | d[48];
      int b2 = (d[6] << 5) | (d[7] << 4) | (d[8] << 3) | (d[9] << 2) | (d[42] << 1) | d[43];
      if (b0 >= 120) { prev = -1; continue; }
      if (prev >= 0) { sum += std::abs(b2 - prev); n++; }
      prev = b2;
    }
    char m[160];
    snprintf(m, sizeof m, "D-Star loudness index moves %.2f steps per frame (real DVSI encoder 3.9; limit 5.0; %zu frames)",
             n ? sum / n : 99.0, n);
    check(n > 400 && sum / n <= 5.0, m);
  }

  // 13. OUR DECODER PLAYS REAL-RADIO FRAMES LIKE A REAL DECODER (2026-10-06 afternoon).
  // Optional arg 5: the per-frame level a real DVSI AMBE-3000 gave the en_US prompt frames
  // (test/refframes/dstar_en_US_chip_level_db.txt). Frame-aligned, active frames: our level
  // minus the chip's must average within +/-3 dB with a per-frame spread under 4.5 dB.
  // Before: -12.8 dB and 5.7 dB (mbelib's 0.65 spectral prediction; the chip uses 0.8).
  if (argc >= 6) {
    vector<uint8_t> ds = readFile(argv[3]);
    FILE* f = fopen(argv[5], "r");
    vector<double> ref;
    if (f) {
      char line[64];
      while (fgets(line, sizeof line, f)) if (line[0] != '#') ref.push_back(atof(line));
      fclose(f);
    }
    size_t o = (ds.size() >= 4 && !memcmp(ds.data(), "AMBE", 4)) ? 4 : 0;
    MBEVocoder d1;
    vector<double> ours;
    for (size_t i = o; i + 9 <= ds.size(); i += 9) {
      int16_t pcm[160];
      uint8_t fr[9];
      memcpy(fr, &ds[i], 9);
      d1.decode_2400x1200(pcm, fr);
      double e = 0;
      for (int k = 0; k < 160; k++) e += (double)pcm[k] * pcm[k];
      ours.push_back(10 * std::log10(e / 160 / (32768.0 * 32768.0) + 1e-12));
    }
    double mx = -999;
    for (double v : ref) mx = std::max(mx, v);
    double bestMean = 0, bestSd = 1e9;
    for (int lag = -3; lag <= 3; lag++) {
      double s = 0, s2 = 0;
      size_t n = 0;
      for (size_t i = 0; i < ref.size(); i++) {
        long j = (long)i - lag;
        if (j < 0 || j >= (long)ours.size() || ref[i] < mx - 30) continue;
        double dd = ours[j] - ref[i];
        s += dd; s2 += dd * dd; n++;
      }
      if (n < 500) continue;
      double mean = s / n, sd = std::sqrt(std::max(0.0, s2 / n - mean * mean));
      if (sd < bestSd) { bestSd = sd; bestMean = mean; }
    }
    char m[192];
    snprintf(m, sizeof m, "real D-Star frames: our level minus a real DVSI decoder's %+.2f dB (limit +/-3), per-frame spread %.2f dB (limit 4.5)",
             bestMean, bestSd);
    check(ref.size() > 1000 && std::fabs(bestMean) <= 3.0 && bestSd <= 4.5, m);
  }

  // 14. THE CHIP'S HARMONIC COUNT (2026-10-06 evening). Optional args 6 and 7: probe frames
  // (pitch code b0 held 25 frames each, all voiced, flat envelope; codes 0..119) and the
  // harmonic count a real DVSI AMBE-3000 played for each code. Our decoder must play the
  // same count for EVERY code (highest harmonic within 20 dB of the harmonics below 2 kHz).
  // Before: mbelib's AmbePlusLtable, 1-4 harmonics more on 102 of the 120 codes, which laid
  // the voice shape ~5 % off along frequency both ways.
  if (argc >= 8) {
    printf("D-Star harmonic count per pitch code vs a real DVSI chip\n");
    vector<int16_t> y = decodeDstarFile(argv[6]);
    vector<int> chipL(120, -1);
    FILE* f = fopen(argv[7], "r");
    if (f) {
      char line[160];
      while (fgets(line, sizeof line, f)) {
        int b, l;
        if (line[0] != '#' && sscanf(line, "%d %d", &b, &l) == 2 && b >= 0 && b < 120) chipL[b] = l;
      }
      fclose(f);
    }
    int bad = 0, firstBad = -1, ourL = 0, cl = 0;
    for (int b0 = 0; b0 < 120 && y.size() >= (size_t)(b0 + 1) * 25 * 160; b0++) {
      const int n = 15 * 160;
      const int16_t* s = &y[(size_t)(b0 * 25 + 10) * 160];
      const double f0 = std::pow(2.0, -4.24734 - 0.021762 * (b0 + 0.5));   // cycles per sample (chip scale, pin 11)
      vector<double> pw;
      for (int k = 1; k * f0 < 0.49875; k++) {
        double re = 0, im = 0;
        for (int i = 0; i < n; i++) {
          double wv = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
          re += s[i] * wv * std::cos(2 * M_PI * k * f0 * i);
          im -= s[i] * wv * std::sin(2 * M_PI * k * f0 * i);
        }
        pw.push_back(re * re + im * im);
      }
      vector<double> low;
      for (size_t k = 0; k < pw.size(); k++) if ((k + 1) * f0 * 8000 < 2000) low.push_back(pw[k]);
      std::sort(low.begin(), low.end());
      double ref = low.empty() ? 1 : low[low.size() / 2];
      int L = 0;
      for (size_t k = 0; k < pw.size(); k++) if (pw[k] > ref * 0.01) L = (int)k + 1;
      if (L != chipL[b0]) { bad++; if (firstBad < 0) { firstBad = b0; ourL = L; cl = chipL[b0]; } }
    }
    char m[192];
    if (bad) snprintf(m, sizeof m, "harmonic count differs from the chip on %d of 120 codes (first b0 %d: ours %d, chip %d)", bad, firstBad, ourL, cl);
    else snprintf(m, sizeof m, "harmonic count equals the chip's on all 120 codes");
    check(bad == 0 && chipL[119] > 0, m);
  }

  // 15. OUR DECODER PUTS THE VOICE SHAPE WHERE A REAL DECODER DOES (2026-10-06 evening).
  // Optional arg 8: the chip's decode of the en_US prompt frames (arg 3) as a shape track
  // (made by `dv_test --shape-track`). Our decode of the same frames: formants F1, F2, F3
  // within 1 % of the chip's (median over hops voiced in both). Before (663): 1.018 /
  // 1.048 / 1.044 (we played real radios ~5 % high, "chipmunkish").
  if (argc >= 9) {
    printf("voice shape: real D-Star frames, our decoder vs a real DVSI decoder (formants, limit +/-1 %%)\n");
    auto ours = shapeTrack(decodeDstarFile(argv[3]));
    auto ref = readShapeTrack(argv[8]);
    double q[3];
    size_t used = 0;
    shapeRatios(ours, ref, q, &used);
    char m[192];
    snprintf(m, sizeof m, "formants F1 %.3f F2 %.3f F3 %.3f x the chip's (%zu voiced hops)", q[0], q[1], q[2], used);
    check(used > 500 && std::fabs(q[0] - 1) <= 0.01 && std::fabs(q[1] - 1) <= 0.01 && std::fabs(q[2] - 1) <= 0.01, m);

  }

  // 16. OUR ENCODER PUTS THE VOICE SHAPE WHERE A REAL ENCODER DOES. Optional args 9 and 10:
  // the male and female clips (args 1-2, as is) encoded by the chip (a real radio's frames).
  // Both frame streams are played by OUR decoder, so its own small differences cancel and
  // only the encoders are compared: our frames' formants within 1.5 % of the chip's frames'
  // (1.5 %, not 1 %: going through a decoder that is not the chip leaves ~0.5-1 % of F1
  // noise; on the chip itself our frames sit within 0.4 % of the chip's own, bench p16_check).
  // Before (663): our encoder laid the shape on mbelib's harmonic count, ~5 % low against
  // the chip's frames (what real radios heard: bassy).
  if (argc >= 11) {
    printf("voice shape: our encoder vs a real encoder (both played by our decoder; formants, limit +/-1.5 %%)\n");
    for (int c = 0; c < 2; c++) {
      Timing ta, tb;
      vector<int16_t> o2 = roundTrip(kDstar, clips[c].pcm, ta, tb);
      auto ref = shapeTrack(decodeDstarFile(argv[9 + c]));
      double q[3];
      size_t used = 0;
      shapeRatios(shapeTrack(o2), ref, q, &used);
      char m[192];
      snprintf(m, sizeof m, "%s: formants F1 %.3f F2 %.3f F3 %.3f x the chip encoder's (%zu voiced hops)", clips[c].name, q[0], q[1], q[2], used);
      check(used > 150 && std::fabs(q[0] - 1) <= 0.015 && std::fabs(q[1] - 1) <= 0.015 && std::fabs(q[2] - 1) <= 0.015, m);
    }
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
