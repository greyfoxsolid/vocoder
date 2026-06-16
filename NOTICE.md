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
