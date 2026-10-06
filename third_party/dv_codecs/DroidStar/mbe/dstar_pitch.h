/*
 * QSO One local addition (2026-10-06; listed in ../../../../NOTICE.md). GPL v3 or later.
 *
 * D-Star (AMBE 3600x2400) constants measured on a real DVSI AMBE-3000 chip (a DVMEGA
 * DVstick 30 used as a sealed box: frames in, audio out; no DVSI code or firmware read).
 * See QSO One Research/DSTAR_STICK_2026-10-06.md.
 *
 * 1. The pitch scale. mbelib decodes the pitch code b0 with its "w0 guess"
 *    f0 = 2^(-4.311767578125 - 0.021336 (b0 + 0.5)). The chip plays b0 at
 *    f0 = 2^(-DSTAR_F0_C0 - DSTAR_F0_C1 (b0 + 0.5))  (cycles per sample), measured on
 *    29 codes from 4 to 116: the fit residual is 0.3 cents. The earlier "two codes
 *    higher" rule (2026-10-06 morning) was right only near 190 Hz; it was 1-2 % wrong on
 *    male voices.
 * 2. The spectral prediction. Each frame's log spectral amplitudes are coded as a
 *    residual from DSTAR_SPEC_PRED x the previous frame's. mbelib uses 0.65 (the AMBE+2 /
 *    DMR value). Decoding the chip's own frames, 0.8 matches the chip best (band-envelope
 *    error 10.8 -> 6.9 dB); our frames coded against 0.65 played 16 dB too loud and
 *    smeared on the chip, and 6.7 dB too loud once coded against 0.78.
 * Used by the encoder, its internal decoder model (mbevocoder.cpp) and the decoder
 * (ambe3600x2400.c). D-Star only; DMR / YSF (AMBE+2) keep AmbeW0table and 0.65.
 */
#ifndef QSO_DSTAR_PITCH_H
#define QSO_DSTAR_PITCH_H
#define DSTAR_F0_C0 4.24734f
#define DSTAR_F0_C1 0.021762f
#define DSTAR_SPEC_PRED 0.8f
/* Encoder loudness (log2 units subtracted from the coded gain) and smoothing of the
 * frame-to-frame gain, set so the chip plays our frames at the input level with the
 * chip's own frame-to-frame gain movement (mean |change of b2| 3.85 vs the chip's 3.92). */
#define DSTAR_GAIN_ADJUST 1.8f
#define DSTAR_GAIN_SMOOTH 0.55f
/* Decoder output gain (dB): with the fixes above our decoder plays the chip's frames
 * about 5 dB under the chip; this brings received stations and our echo to the chip's
 * level. */
#define DSTAR_OUT_GAIN_DB 5.0f
#endif
