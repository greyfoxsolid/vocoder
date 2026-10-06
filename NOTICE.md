# NOTICE / provenance - qso_dmr_vocoder

This module is **GPL** (GNU General Public License v3; see `LICENSE`). It is a
standalone, source-available artifact. The QSO One application links to it only
at runtime; QSO One's distributed binary does not contain it.

## Incorporated components and their licenses

| Component | Source | Commit / version | License |
|---|---|---|---|
| **md380_vocoder_dynarmic** (the AMBE+2 codec, vendored in `third_party/`) | github.com/nostar/md380_vocoder_dynarmic | `55c9a2c` | **GPL** (GPLv3 / GPLv2-or-later headers) |
| dynarmic (ARM JIT; fetched at build time via CMake `FetchContent`, pinned) | github.com/yuzu-mirror/dynarmic | `9d45823` | 0BSD / ISC-style (permissive) |
| mcl | dynarmic external | bundled | MIT |
| oaknut (arm64 host backend) | dynarmic external | bundled | MIT |
| fmt | dynarmic external | bundled | MIT |
| Zydis + Zycore (x86_64 host backend) | dynarmic external | bundled | MIT |
| xbyak (x86_64 host backend) | dynarmic external | bundled | BSD-3-Clause |
| robin-map | dynarmic external | bundled | MIT |

The combined work is distributed under the **GPL** because the codec
(`md380_vocoder_dynarmic`) is GPL. All other linked components are permissive
(0BSD / MIT / BSD-3) and are GPL-compatible.

## What this module does NOT contain (and never will)

- **No MD380 firmware bytes.** `D002.032.bin` (the TYT/Tytera proprietary OEM
  firmware) and `d02032-core.img` are **not** in this source tree and are **not**
  in any release artifact. They are non-redistributable. They are supplied to
  `qdv_init()` as runtime file paths and loaded into memory on the user's device.
  The upstream `xxd` build-time embed (`firmware.c` / `sram.c`) has been removed
  and replaced with `src/firmware_runtime.cpp`, a runtime file loader.

## Local modifications to the vendored GPL source

- `third_party/md380_vocoder_dynarmic/md380_vocoder.cpp`: added `md380_deinit()`
  (frees the emulator allocated by `md380_init()`), needed for the module's
  `qdv_shutdown()`. The codec math is unmodified.
- `third_party/md380_vocoder_dynarmic/md380_vocoder.h`: declared `md380_deinit()`.
- The global `firmware[]` / `sram[]` symbols the codec expects (declared in
  `tables.h`) are now provided as zero-initialized buffers by
  `src/firmware_runtime.cpp` and filled at runtime, instead of by the removed
  `xxd`-generated `firmware.c` / `sram.c`.

## GPL firmware-unwrap, ported into this module (ABI v2)

`src/md380_unwrap.cpp` implements `qdv_unwrap_md380()`, ported from md380tools
`md380-fw.py` (`MD380FW.unwrap` + `TYTFW.crypt`): a 256-byte header, then the
application image XOR-scrambled by a static 1024-byte cyclic key, then a
256-byte footer. The key and transform are GPL md380tools-derived. They live
here, in the GPL module, **on purpose**: QSO One (closed) fetches and extracts
the wrapped OEM file with generic, non-GPL code and calls this function across
the C ABI, so no GPL-derived material enters the closed binary. The MD380 key
is reproduced verbatim from `md380-fw.py:MD380FW.key`.

## Firmware provenance (for the operator who fetches it, not shipped here)

- `D002.032.bin` (994,304 bytes): unwrapped from md380.org
  `TYT-Tytera-MD-380-FW-v232.zip` (`Firmware 2.32/MD-380-D2.32(AD).bin`) via
  `md380-fw.py --unwrap`. SHA-256
  `c0351f250a834660641bca3b06be931c80cc1ed0ef808356c550b99cd0f4c632`.
- `d02032-core.img` (131,072 bytes): SHA-256
  `d40398183e9aa8db6288a62d9a0e0c233cb143ef5633680abd829cc03c719f66`.

Firmware integrity verification (SHA-256) is the responsibility of the QSO One
side before it calls `qdv_init()`; this module validates only sizes.

## D-Star and P25 voice codecs (the helper's separate "dv" mode, 2026-10-05)

The helper executable also carries two software voice codecs, run ONLY when the
helper is started with the argument `dv`, as its own process. That mode never
calls the MD-380 codec (`qdv_*`), needs no firmware, and shares no state with
the DMR / YSF mode. Source: `src/dv_mode.cpp`, `src/dv_codec_core.*`, and the
vendored code under `third_party/dv_codecs/`.

| Component | Source | Commit / version | License |
|---|---|---|---|
| **mbelib** (D-Star AMBE 2400 decoder, ECC; `third_party/dv_codecs/DroidStar/mbe/ambe3600x2400*`, `ambe3600x2450*`, `ecc*`, `mbelib*`) | github.com/szechyjs/mbelib 1.3.0 (`9a04ed5`), via DroidStar `c6a4c54` | Copyright (C) 2010 mbelib Author | **ISC** (full text: `LICENSE_ISC.txt`, upstream `third_party/dv_codecs/mbelib_upstream/COPYRIGHT`) |
| **IMBE vocoder** (P25 Phase 1 IMBE 7200x4400 encoder + decoder, fixed point; also the D-Star encoder's speech analysis; `third_party/dv_codecs/DroidStar/imbe_vocoder/`) | Pavel Yazev, Version 1.0 (c) 2009, via OP25 and DroidStar `c6a4c54` | 2009 | **GPL v3 or later**. `basic_op.h`, `basicop2.cc`, `typedef*.h` derive from the ETSI / ITU-T fixed-point basic-operator reference code |
| **OP25 AMBE encoder** (`encode_ambe()`, `b0_lookup[]`, `m_list` / `d_list` / `b_lengths` D-Star interleave, in `DroidStar/mbe/mbevocoder.cpp`) | OP25 `ambe_encoder.cc` / `p25p2_vf.cc`, "AMBE halfrate encoder - Copyright 2016 Max H. Parke KA1RBI", via DroidStar `c6a4c54` | 2016 | **GPL v3 or later**. DroidStar's copy of `mbevocoder.cpp` carries an ISC notice with no copyright line; the code is OP25's and is used here under the GPL |
| **MMDVMHost AMBE FEC tables** (`PRNG_TABLE`, `A_TABLE`, `B_TABLE`, `C_TABLE` in `DroidStar/mbe/vocoder_tables.h`) | MMDVMHost `AMBEFEC.cpp`, Copyright (C) 2010-2025 Jonathan Naylor G4KLX, (C) 2016 Mathias Weyland HB9FRV | master (2026-10) | **GPL v2 or later** (combined here under v3) |
| **DSD de-interleave tables** (`dW` / `dX`, `rW`..`rZ` in `mbevocoder.cpp`) | DSD, Copyright (C) 2010 DSD Author | | **ISC** |
| **DroidStar glue** (`MBEVocoder` class, `mbevocoder*.h`, `mbelib_parms.h`, `encode_4400` / `decode_4400` packing in `imbe_vocoder/encode.cc` / `decode.cc`) | github.com/nostar/DroidStar `c6a4c54`, Doug McLain AD8DP | 2019-2026 | **GPL v3 or later** (treated as GPL v3+ with the rest; several files carry an ISC notice) |

The combined helper stays **GPL v3 or later** (`LICENSE`). ISC and GPL v2-or-later
code combine into it.

**mbelib PATENT NOTICE** (from its upstream `README.md`, word for word; kept with
the source at `third_party/dv_codecs/mbelib_upstream/README.md`):

    This source code is provided for educational purposes only.  It is
    a written description of how certain voice encoding/decoding
    algorythims could be implemented.  Executable objects compiled or 
    derived from this package may be covered by one or more patents.
    Readers are strongly advised to check for any patent restrictions or 
    licencing requirements before compiling or using this source code.

### Local changes to the vendored D-Star / P25 code

Everything under `third_party/dv_codecs/` is an unmodified copy of DroidStar
`c6a4c54` (`mbe/`, `imbe_vocoder/`) and of mbelib's `COPYRIGHT` / `README.md`,
EXCEPT these changes, each marked "QSO One local change" in the source:

1. **The D-Star loudness fix** (`DroidStar/mbe/mbevocoder.cpp`, `encode_ambe()`).
   Upstream coded the loudness value `b2` as an ABSOLUTE number in D-Star mode
   (`if (dstar) diff_gain = gain;`), but every D-Star decoder (mbelib, and this
   encoder's own dequantizer) reads it as a CHANGE from the previous frame
   (`gamma = AmbePlusDg[b2] + 0.5 * prev_gamma`). Decoded speech came out 13 to
   28 dB too loud with thousands of clipped samples. Now it is always coded as
   `gain - 0.5 * prev_mp->gamma`, as the DMR branch always did. Proof:
   `test/dv_test.cpp` (decoded level within 4 dB of the input on normal, quiet
   and loud speech; with the old line restored the same test fails by +5 to
   +26 dB with thousands of clipped samples).
2. **Leak fix** (`DroidStar/mbe/mbevocoder.cpp`, `~MBEVocoder()`): free the
   parameter block the constructor allocates (upstream leaked it; dv mode makes a
   fresh coder at every reset).
3. **The D-Star pitch fix** (`DroidStar/mbe/mbevocoder.cpp`, `encode_ambe()`,
   2026-10-05). Upstream (from OP25 `ambe_encoder.cc`) chose the pitch index
   `b0` with `b0_lookup[]`, a table built for the DMR / AMBE+2 pitch table
   `AmbeW0table`, then moved `b0` until `AmbePlusLtable[b0]` equalled the IMBE
   harmonic count. A D-Star decoder (mbelib, and this encoder's own dequantizer)
   turns `b0` into a pitch with `f0 = 2^(-4.311767578125 - 0.021336 (b0 + 0.5))`,
   2 to 6 % higher than `AmbeW0table` for the same `b0`, so every voice played
   back 2.5 to 5 % sharp with its formants shifted up the same amount (heard as a
   "chipmunk" echo on XLX073 E). Now D-Star `b0` is the nearest decoder pitch to
   the measured pitch, `L` is the decoder's own harmonic count for that `b0`, and
   the IMBE harmonic amplitudes and voicing are resampled onto those `L`
   harmonics. DMR mode and P25 are unchanged (byte-identical output). Proof:
   `test/dv_test.cpp` section 7 (decoded pitch within 1.5 % of the input; with the
   upstream lines restored it fails at +4.3 % male / +2.8 % female). The helper's
   build id says `loudness-fix, pitch-fix`.
4. **The real-radio D-Star pitch scale** (2026-10-06; new file
   `DroidStar/mbe/dstar_pitch.h`, used in `mbevocoder.cpp` `make_f0()`, the
   dequantizer and `encode_ambe()`, and in `DroidStar/mbe/ambe3600x2400.c`, the
   mbelib decoder). mbelib's D-Star pitch formula is its own "w0 guess". Measured
   against real DVSI D-Star encoders (G4KLX's ircDDBGateway voice prompts, 10
   speakers, frame by frame against the same recordings in DMR AMBE+2 and P25
   IMBE, whose pitch scales are exact), real radios play code `b0` at the guess's
   pitch for `b0 - 2` (3.0 % higher). Encoder and decoder now both use that scale
   (`DSTAR_B0_SHIFT 2.0f`). Proof: `test/dv_test.cpp` section 10 (real D-Star
   frames decode at 0.996 x the DMR rendition's pitch; 0.967 before).
5. **The b8 fix** (`mbevocoder.cpp`, `encode_ambe()` and `encode_2400x1200()`,
   2026-10-06). A D-Star frame carries three bits of `b8`, which decoders read as
   the TOP three (mbelib: `b8 = bits << 1`). Upstream searched all 16 entries and
   sent the LOW three bits, so the decoder used another entry than the encoder chose
   and the encoder's model of the decoder split from the real decoder on every
   frame. Now D-Star searches the even entries and sends `b8 >> 1`. Proof:
   `test/dv_test.cpp` section 8 (encoder state equals mbelib's decoder state on
   every frame; 0 of 485 frames before).
6. **The top-band fade** (`mbevocoder.cpp`, `encode_ambe()`, 2026-10-06). Decoder
   harmonics above the IMBE analysis band (~3.7 kHz) took the last analysed
   amplitude, putting 7 to 19 dB more energy at 3.7-4.0 kHz than band-limited input
   had; they now fade 6 dB per harmonic. Proof: `test/dv_test.cpp` section 9.
   The build id says `loudness-fix, pitch-fix, radio-pitch-scale, b8-fix,
   top-band-fade`. DMR / YSF mode and P25 stay byte-identical.

The test files `test/refframes/dstar_en_US_ircddbgateway.ambe` (ircDDBGateway
`Data/en_US.ambe`) and `test/refframes/dmr_en_US_dmrgateway.ambe` (DMRGateway
`Audio/en_US.ambe`) are Jonathan Naylor G4KLX's voice prompts, GPL v2 or later,
copied unmodified from his public repositories for the real-radio pitch test.

### The armeabi-v7a (32-bit ARM) helper

The v7a helper is built from `v7a/` (the native nostar/md380_vocoder-lineage
MD-380 core, `md380_vocoder.c`, Copyright (C) 2020-2021 Doug McLain AD8DP, based on
md380tools, GPL v2 or later; plus its own IPC front end `v7a/vocoder_helper.cpp`)
by `build_android_v7a.bat`, and links the same dv sources as the other targets.
