// dv_mode.cpp - the helper's "dv" mode: D-Star (AMBE 2400) and P25 (IMBE)
// software voice codecs, in their OWN helper process.
//
// The same helper binary is started with the single argument "dv":
//     qso_dmr_vocoder_helper.exe dv          (Windows)
//     libqso_dmr_vocoder_helper.so dv        (Android, all ABIs)
// With no argument the helper runs its original DMR / YSF (MD-380) loop,
// unchanged. This mode NEVER calls any qdv_* function, needs no firmware, and
// shares nothing with the MD-380 codec or the app's DMR / YSF owner count; the
// two modes are separate processes.
//
// IPC (stdin requests, stdout replies, stderr log lines; all integers little-
// endian; one request in flight at a time):
//
//   'H'                                  hello
//       -> 'D' 'V' <u8 proto> <u8 caps> <u16 len> <build id, len bytes>
//          caps bit0 = D-Star, bit1 = P25. The app checks proto FIRST and logs a
//          mismatch, so a skew is a logged error, never a silent death.
//   'E' <u8 kind> <u16 n> <n x 320 bytes PCM>              encode n frames
//       -> <u8 status> <u32 codecUsTotal> <u32 codecUsMax> <n x frameBytes>
//   'D' <u8 kind> <u16 n> <n x frameBytes>                 decode n frames
//       -> <u8 status> <u32 codecUsTotal> <u32 codecUsMax> <n x 320 bytes PCM>
//   'R' <u8 kind> <u8 which>   reset (which: bit0 encoder, bit1 decoder)
//       -> <u8 status>
//   'Q'                        quit
//
//   kind: 0 = D-Star (9-byte frames), 1 = P25 (11-byte frames).
//   n: 1..kMaxFrames. codecUs*: time spent inside the codec (not the pipe), so
//   the app log can show the codec cost per frame on every device.
//
// A request the helper cannot parse (unknown command, bad kind, bad n) leaves
// the byte stream out of step, so the helper logs one line naming it and exits
// with code 3 (distinct from the MD-380 loop's 2). The app reports the exit.
//
// GPL v3 or later; see ../NOTICE.md.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dv_codec_core.h"
#include "dv_mode.h"

namespace {

constexpr uint8_t kDvProtoVersion = 1;
constexpr uint8_t kDvCaps = 0x03;  // D-Star + P25
constexpr int kMaxFrames = 50;     // 1 s of audio per request
constexpr const char* kDvBuildId =
    "qso_dv proto=1 dstar=ambe3600x2400(DroidStar c6a4c54, loudness-fix, pitch-fix) "
    "p25=imbe7200x4400(Yazev) mbelib=1.3.0";

volatile const char** g_dvStep = nullptr;

void setStep(const char* s) {
  if (g_dvStep) *g_dvStep = s;
}

void dvlog(const char* step, const char* msg) {
  fprintf(stderr, "[vhelper] mode=dv step=%s %s\n", step, msg);
  fflush(stderr);
}

bool readN(void* buf, size_t n) { return fread(buf, 1, n, stdin) == n; }

void writeN(const void* buf, size_t n) {
  fwrite(buf, 1, n, stdout);
  fflush(stdout);
}

void putU16(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xff));
  o.push_back((uint8_t)((v >> 8) & 0xff));
}

int protoFail(const char* what, int value) {
  char m[96];
  snprintf(m, sizeof m, "PROTO ERROR %s=%d (stream out of step) -> exit 3", what,
           value);
  dvlog("proto", m);
  return 3;
}

using Clock = std::chrono::steady_clock;

uint32_t usSince(Clock::time_point t0) {
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
                .count();
  return us < 0 ? 0u : (uint32_t)us;
}

}  // namespace

int qdv_dv_main(volatile const char** step) {
  g_dvStep = step;
  setStep("dv_start");
  {
    char m[192];
    snprintf(m, sizeof m, "%s; entering dv request loop", kDvBuildId);
    dvlog("start", m);
  }
  qdv_dv::Codecs codecs;
  std::vector<uint8_t> in, reply;
  uint8_t cmd;
  setStep("dv_loop");
  while (readN(&cmd, 1)) {
    switch (cmd) {
      case 'H': {
        reply.clear();
        reply.push_back('D');
        reply.push_back('V');
        reply.push_back(kDvProtoVersion);
        reply.push_back(kDvCaps);
        size_t len = strlen(kDvBuildId);
        putU16(reply, (uint32_t)len);
        reply.insert(reply.end(), kDvBuildId, kDvBuildId + len);
        writeN(reply.data(), reply.size());
        dvlog("hello", "answered");
        break;
      }
      case 'E':
      case 'D': {
        uint8_t kind;
        uint8_t nb[2];
        if (!readN(&kind, 1) || !readN(nb, 2)) return 2;
        const int n = nb[0] | (nb[1] << 8);
        const int fb = qdv_dv::frameBytes(kind);
        if (fb == 0) return protoFail("kind", kind);
        if (n < 1 || n > kMaxFrames) return protoFail("frames", n);
        const bool enc = cmd == 'E';
        const size_t inPer = enc ? qdv_dv::kPcmSamples * 2 : (size_t)fb;
        const size_t outPer = enc ? (size_t)fb : qdv_dv::kPcmSamples * 2;
        in.resize(inPer * n);
        if (!readN(in.data(), in.size())) return 2;
        reply.assign(9 + outPer * n, 0);
        uint32_t total = 0, mx = 0;
        setStep(enc ? (kind == qdv_dv::kDstar ? "dv_encode_dstar" : "dv_encode_p25")
                    : (kind == qdv_dv::kDstar ? "dv_decode_dstar" : "dv_decode_p25"));
        for (int i = 0; i < n; ++i) {
          auto t0 = Clock::now();
          if (enc) {
            int16_t pcm[qdv_dv::kPcmSamples];
            memcpy(pcm, &in[i * inPer], sizeof pcm);  // aligned copy
            codecs.encode(kind, pcm, &reply[9 + i * outPer]);
          } else {
            int16_t pcm[qdv_dv::kPcmSamples];
            codecs.decode(kind, &in[i * inPer], pcm);
            memcpy(&reply[9 + i * outPer], pcm, sizeof pcm);
          }
          uint32_t us = usSince(t0);
          total += us;
          if (us > mx) mx = us;
        }
        setStep("dv_loop");
        reply[0] = 0;  // status OK
        for (int i = 0; i < 4; ++i) reply[1 + i] = (uint8_t)((total >> (8 * i)) & 0xff);
        for (int i = 0; i < 4; ++i) reply[5 + i] = (uint8_t)((mx >> (8 * i)) & 0xff);
        writeN(reply.data(), reply.size());
        break;
      }
      case 'R': {
        uint8_t kind, which;
        if (!readN(&kind, 1) || !readN(&which, 1)) return 2;
        if (qdv_dv::frameBytes(kind) == 0) return protoFail("kind", kind);
        codecs.reset(kind, which);
        uint8_t ok = 0;
        writeN(&ok, 1);
        break;
      }
      case 'Q':
        dvlog("quit", "graceful shutdown requested");
        return 0;
      default:
        return protoFail("command", cmd);
    }
  }
  dvlog("parent_gone", "stdin closed; exiting");
  return 0;
}
