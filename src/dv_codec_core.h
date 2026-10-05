// dv_codec_core.h - the D-Star and P25 software voice codecs used by the
// helper's "dv" mode (see dv_mode.cpp) and by test/dv_test.cpp.
//
//   D-Star: AMBE 3600x2400 (2400 bit/s voice + D-Star FEC). 9-byte frames in
//           D-Star network order (LSB-first inside each byte, exactly the 9
//           AMBE bytes of a DPlus / DExtra / DCS voice packet, as DroidStar
//           copies them). Encoder: DroidStar MBEVocoder::encode_2400x1200 (the
//           OP25 AMBE encoder over the Yazev IMBE analysis) with the QSO One
//           loudness fix; decoder: mbelib via MBEVocoder::decode_2400x1200.
//   P25:    IMBE 7200x4400 (P25 Phase 1). 11-byte frames, 88 bits u0..u7
//           MSB-first, no FEC (the bytes of an MMDVM P25 network LDU record).
//           Pavel Yazev's fixed-point IMBE coder (encode_4400 / decode_4400).
//
// 160 signed 16-bit samples, 8 kHz mono, per 20 ms frame.
//
// None of this touches the MD-380 (qdv_*) codec, its firmware or its state.
// The codecs keep process-global state (mbelib rand(), ecc.c statics, the
// fixed-point Overflow flag), so a process runs ONE stream at a time; the app
// gives each D-Star / P25 session its own helper process.
//
// GPL v3 or later (it links GPL v3+ code: Yazev IMBE and the OP25 encoder).
// See ../NOTICE.md.

#pragma once

#include <cstdint>
#include <memory>

class MBEVocoder;
class imbe_vocoder;

namespace qdv_dv {

enum Kind : uint8_t { kDstar = 0, kP25 = 1 };

constexpr int kPcmSamples = 160;
constexpr int kDstarFrameBytes = 9;
constexpr int kP25FrameBytes = 11;

// Bytes per frame on the wire for [kind], or 0 for an unknown kind.
inline int frameBytes(int kind) {
  return kind == kDstar ? kDstarFrameBytes : kind == kP25 ? kP25FrameBytes : 0;
}

// One encoder state + one decoder state per kind, created lazily and
// recreated on reset (the upstream coders have no reset call; a fresh object
// is exactly the state of a new transmission / received stream).
class Codecs {
 public:
  Codecs();
  ~Codecs();
  // pcm: 160 samples in; out: frameBytes(kind) bytes. Returns false on bad kind.
  bool encode(int kind, const int16_t* pcm, uint8_t* out);
  // frame: frameBytes(kind) bytes in; pcm: 160 samples out.
  bool decode(int kind, const uint8_t* frame, int16_t* pcm);
  // which: bit0 = encoder, bit1 = decoder.
  bool reset(int kind, int which);

 private:
  std::unique_ptr<MBEVocoder> dsEnc_, dsDec_;
  std::unique_ptr<imbe_vocoder> p25Enc_, p25Dec_;
};

}  // namespace qdv_dv
