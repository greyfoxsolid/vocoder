/*
 * QSO One local addition (2026-10-06; listed in ../../../../NOTICE.md). GPL v3 or later.
 *
 * The D-Star (AMBE 3600x2400) pitch scale real radios use. mbelib decodes the pitch
 * code b0 with f0 = 2^(-4.311767578125 - 0.021336 (b0 + 0.5)) and calls it a "w0 guess".
 * Real DVSI D-Star encoders, measured frame by frame against the same recordings coded
 * in DMR AMBE+2 and P25 IMBE (exact public scales) for 10 speakers, put each pitch two
 * codes higher than that formula says: a real radio plays code b0 at the formula's
 * pitch for b0 - 2 (3.0 % higher). See QSO One Research/DSTAR_SOUND_2026-10-06.md.
 * Used by the encoder, its internal decoder model (mbevocoder.cpp) and the decoder
 * (ambe3600x2400.c). D-Star only; DMR / YSF (AMBE+2) keep AmbeW0table.
 */
#ifndef QSO_DSTAR_PITCH_H
#define QSO_DSTAR_PITCH_H
#define DSTAR_B0_SHIFT 2.0f
#endif
