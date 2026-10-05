// dv_codec_core.cpp - see dv_codec_core.h. GPL v3 or later; see ../NOTICE.md.

#include "dv_codec_core.h"

#include <cstring>

#include "mbevocoder.h"  // third_party/dv_codecs/DroidStar/mbe (+ imbe_vocoder)

namespace qdv_dv {

Codecs::Codecs() = default;
Codecs::~Codecs() = default;

bool Codecs::encode(int kind, const int16_t* pcm, uint8_t* out) {
  int16_t in[kPcmSamples];
  memcpy(in, pcm, sizeof in);  // the upstream coders take non-const buffers
  if (kind == kDstar) {
    if (!dsEnc_) dsEnc_.reset(new MBEVocoder());
    // encode_2400x1200 ORs bits into the caller's buffer and never clears it,
    // so the 9 bytes MUST start zeroed (the bench found this the hard way).
    memset(out, 0, kDstarFrameBytes);
    dsEnc_->encode_2400x1200(in, out);
    return true;
  }
  if (kind == kP25) {
    if (!p25Enc_) p25Enc_.reset(new imbe_vocoder());
    memset(out, 0, kP25FrameBytes);
    p25Enc_->encode_4400(in, out);
    return true;
  }
  return false;
}

bool Codecs::decode(int kind, const uint8_t* frame, int16_t* pcm) {
  if (kind == kDstar) {
    if (!dsDec_) dsDec_.reset(new MBEVocoder());
    uint8_t f[kDstarFrameBytes];
    memcpy(f, frame, sizeof f);
    memset(pcm, 0, kPcmSamples * sizeof(int16_t));
    dsDec_->decode_2400x1200(pcm, f);
    return true;
  }
  if (kind == kP25) {
    if (!p25Dec_) p25Dec_.reset(new imbe_vocoder());
    uint8_t f[kP25FrameBytes];
    memcpy(f, frame, sizeof f);
    memset(pcm, 0, kPcmSamples * sizeof(int16_t));
    p25Dec_->decode_4400(pcm, f);
    return true;
  }
  return false;
}

bool Codecs::reset(int kind, int which) {
  if (kind == kDstar) {
    if (which & 1) dsEnc_.reset();
    if (which & 2) dsDec_.reset();
    return true;
  }
  if (kind == kP25) {
    if (which & 1) p25Enc_.reset();
    if (which & 2) p25Dec_.reset();
    return true;
  }
  return false;
}

}  // namespace qdv_dv
