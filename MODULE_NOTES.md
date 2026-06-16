# qso_dmr_vocoder - module notes

**Date:** 2026-06-13
**What this is:** the production-shaped DMR (AMBE+2) vocoder module derived from
the feasibility spike. Standalone, source-available, GPL. Loads firmware from
files at runtime (no firmware bytes in the tree or artifacts) and exposes a
stable versioned `extern "C"` ABI for QSO One to call via dart:ffi.
**Status:** module only. No protocol, no audio wiring, no UI, no QSO One app
changes. Those are later dispatches.

---

## 1. Firmware to guest-memory mapping (confirmed from the spike codec)

The upstream `MD380Emulator` maps two blobs into the dynarmic A32 guest address
space. Confirmed from `md380_vocoder.cpp` (`MD380Environment` regions +
`MD380Emulator(uint8_t* firmware, uint8_t* sram)` constructor):

| Blob (physical file) | Size | Constructor arg | Guest address | How mapped |
|---|---|---|---|---|
| **D002.032.bin** (MD380 application firmware) | 994,304 B | `firmware` | **0x0800C000** (window 0x100000 = 1 MiB) | `env.firmware = firmware` (pointer kept, **not copied** -> buffer must outlive the emulator); codec reads `firmware[vaddr - 0x0800C000]` |
| **d02032-core.img** (SRAM core image) | 131,072 B = **0x20000** | `sram` | **0x20000000** | `std::copy(sram, sram + 0x20000, env.sram.begin())` (copied; must be exactly 0x20000) |

Zero-initialized guest regions (not from files): TCRAM 0x20000 @ 0x10000000,
stack 0x10000 @ 0x21000000.

**Runtime loader reproduces this byte-for-byte.** `src/firmware_runtime.cpp`
provides the `firmware[]` / `sram[]` symbols the codec links against (declared
in `tables.h`) as zero-initialized BSS buffers, and fills them from the caller's
files in `qdv_init()`. The firmware buffer is sized to the full 0x100000 guest
window (the real blob is 994,304 B); the core buffer is exactly 0x20000. Proof
of fidelity: on both platforms the runtime-loaded round-trip output is
**byte-identical** to the spike's baked-in output (tone RMS 7123.1, speech RMS
2010.0, envelope 0.986 all match exactly).

---

## 2. The stable C ABI (`include/qso_dmr_vocoder.h`)

ABI version: **2** (`QDV_ABI_VERSION`). Bump on any change so a consumer can
refuse a mismatched module via `qdv_abi_version()`. v2 added
`qdv_unwrap_md380()`; a consumer that needs the unwrap pins `>= 2`.

```c
int          qdv_abi_version(void);                       // returns 2
const char*  qdv_build_id(void);                          // module + commit refs
int          qdv_init(const char* firmware_path,          // fallible: 0 = QDV_OK, else qdv_status
                      const char* core_path);
int          qdv_unwrap_md380(const char* wrapped_path,   // v2: GPL OEM unwrap, in-module
                      const char* unwrapped_path);        //     0 = QDV_OK, else qdv_status
void         qdv_encode(uint8_t* ambe_out,  const int16_t* pcm_in);   // 160 int16 -> 7 bytes
void         qdv_decode(const uint8_t* ambe_in, int16_t* pcm_out);    // 7 bytes -> 160 int16
void         qdv_encode_fec(uint8_t* ambe_out, const int16_t* pcm_in);
void         qdv_decode_fec(const uint8_t* ambe_in, int16_t* pcm_out);
void         qdv_shutdown(void);
```

Error codes (`qdv_status`): `QDV_OK=0`, `QDV_ERR_ALREADY_INIT=1`,
`QDV_ERR_NULL_ARG=2`, `QDV_ERR_FIRMWARE_OPEN=3`, `QDV_ERR_FIRMWARE_SIZE=4`,
`QDV_ERR_FIRMWARE_READ=5`, `QDV_ERR_CORE_OPEN=6`, `QDV_ERR_CORE_SIZE=7`,
`QDV_ERR_CORE_READ=8`, `QDV_ERR_UNWRAP_OPEN=9`, `QDV_ERR_UNWRAP_FORMAT=10`,
`QDV_ERR_UNWRAP_WRITE=11`.

**`qdv_unwrap_md380` (v2)** turns a wrapped MD380 OEM image (the
`MD-380-D2.32(AD).bin` inside TYT's update zip) into the raw `D002.032.bin` the
codec needs (header parse + cyclic-XOR against the static md380tools key). This
GPL transform lives in the module by design so the closed caller never contains
GPL-derived code; the caller does only generic fetch/unzip/SHA. Validated:
`qdv_unwrap_md380` output is byte-identical to the known-good `D002.032.bin`
(SHA `c0351f25...4c632`).

`qdv_init` is fallible by design (runtime file load can fail). `qdv_encode` /
`qdv_decode` are no-ops if called before a successful init. `qdv_shutdown` frees
the emulator and allows re-init. The data shapes are unchanged from the spike
(one 20 ms frame: 160 int16 PCM <-> 7-byte 49-bit AMBE, MSB-first).

Only `qdv_*` symbols are exported (Windows: `__declspec(dllexport)` under
`QDV_BUILDING`; ELF: visibility default with the rest hidden). The internal
`md380_*` and dynarmic symbols stay private.

---

## 3. Build

Prereqs: CMake (3.20+), Ninja, a C++20 compiler (MSVC 14.44 / NDK 27 clang),
git (dynarmic is fetched via CMake `FetchContent`, pinned to commit `9d45823`),
and **Boost headers** (1.57+, header-only; dynarmic's `find_package(Boost)`).
**No python3 / unzip / xxd / curl** are needed: those were firmware-embed-time
tools in the spike and are gone with the runtime loader.

```
# Windows x64  (set BOOST_ROOT or let it default to the spike's boost headers)
build_windows.bat      ->  build-windows/qso_dmr_vocoder.dll  (+ qdv_test.exe)

# Android arm64 (NDK 27.0.12077973, arm64-v8a, android-24, c++_static)
build_android.bat      ->  build-android/libqso_dmr_vocoder.so (+ qdv_test)
```

Notes carried from the spike build lessons:
- We use our own CMake (not the upstream one), so the upstream `STATIC`
  `target_link_libraries` keyword bug does not apply; we link
  `target_link_libraries(qso_dmr_vocoder PRIVATE dynarmic)`.
- The MSVC C-vs-C++ file-scope-global mangling trap is avoided structurally:
  `firmware_runtime.cpp` is C++ (not a C `xxd` file), so its `firmware`/`sram`
  symbols mangle identically to the codec's C++ references. No generated C TUs
  remain.
- Android uses `ANDROID_STL=c++_static`, so the `.so` and the test exe are each
  self-contained (no `libc++_shared.so` to ship). The C ABI boundary means the
  two static libc++ copies never exchange C++ objects.

### Artifact sizes

ABI v2 artifacts (the hosted artifact should be the stripped `.so`):

| Artifact | Size | SHA-256 |
|---|---|---|
| `qso_dmr_vocoder.dll` (Win x64) | 4,178,944 B | `6f5225125c317a9f7c392d07e8f1fb3181baa24d9b0a624c3fb40448be3c39f2` |
| `libqso_dmr_vocoder.so` (arm64, unstripped) | 39,144,936 B | (build output; strip before hosting) |
| `libqso_dmr_vocoder.arm64.stripped.so` (arm64, stripped, **host this**) | 3,791,800 B | `d8f5810ca56f2658e894d880ea747a1bc667752e184fbfcc6ff71d08295b1631` |
| `qdv_test` (arm64) / `qdv_test.exe` | 290,088 B / ~29 KB | test runner, not shipped |

### No firmware bytes in tree or artifacts (verified)

- No `*.bin`, `*.img`, `firmware.c`, or `sram.c` committed in the module tree.
- High-entropy 256-byte needles (168 and 165 distinct byte values) taken from
  `D002.032.bin` and `d02032-core.img` do **not** appear in either the `.dll` or
  the `.so`. (An earlier all-zero 64-byte needle gave a false positive; it was a
  zero-padding run, not firmware content.)

---

## 4. Correctness through the ABI with firmware loaded at RUNTIME

Driven by `test/qdv_test.cpp` calling the `qdv_*` ABI, firmware supplied as file
paths to `qdv_init()` (the spike blobs, which live outside the module tree).

### Init contract (fallible init) - both platforms

| Call | Result | Expected |
|---|---|---|
| `qdv_init("missing", core)` | 3 (`QDV_ERR_FIRMWARE_OPEN`) | nonzero |
| `qdv_init(fw, 100-byte garbage)` | 7 (`QDV_ERR_CORE_SIZE`) | nonzero |
| `qdv_init(fw, core)` valid | 0 (`QDV_OK`) | 0 |
| `qdv_init(...)` again | 1 (`QDV_ERR_ALREADY_INIT`) | nonzero |

`qdv_init` success cost (one-time): **~9.4 ms** Windows, **~112 ms** Android.

### Correctness (1 kHz tone + real 8 kHz speech, 200 frames)

| Platform | tone: output 1 kHz energy fraction | speech: loudness-envelope correlation | verdict |
|---|---|---|---|
| Windows x64 | 0.985 | 0.986 @ 2-frame lag | PASS |
| Android arm64 (Pixel 10 Pro XL) | 0.985 | 0.986 @ 2-frame lag | PASS |

Both match the baked-in spike baseline (~0.986), confirming the runtime-load
refactor preserved correctness. Sample-level waveform correlation is not used:
AMBE+2 is parametric, so the loudness-envelope contour is the right fidelity
metric. Round-trip WAVs saved for listening: `out/` (Windows), `out-android/`
(Android): `tone_in/out.wav`, `speech_in/out.wav`.

### Latency sanity (1000 calls each; full numbers were the prior dispatch)

| Platform | encode mean | decode mean | per 20 ms frame | % of budget |
|---|---|---|---|---|
| Windows x64 | 1307 us | 272 us | 1579 us | 7.9% |
| Android arm64 (Pixel 10 Pro XL) | 682 us | 144 us | 826 us | 4.1% |

Consistent with the spike (the ABI wrapper adds nothing measurable).

---

## 5. What changed vs the spike (and what stayed GPL-as-is)

- **Removed:** the `xxd` firmware embed (`firmware.c`/`sram.c`), `download.sh`,
  and the firmware-embed build-tool requirements.
- **Added:** `src/firmware_runtime.cpp` (runtime loader + BSS firmware buffers),
  `src/md380_unwrap.cpp` (ABI v2: GPL OEM firmware unwrap, `qdv_unwrap_md380`),
  `src/qdv_abi.cpp` (the `extern "C"` ABI), `include/qso_dmr_vocoder.h`, our own
  `CMakeLists.txt`, GPL `LICENSE`, `NOTICE.md`, build scripts.
- **Vendored GPL source, codec math unmodified:** `md380_vocoder.cpp` (one
  addition: `md380_deinit()` for teardown), `md380_vocoder.h` (declared it),
  `tables.h` (unchanged). See `NOTICE.md` for the full local-modifications list.

---

## 6. Go/no-go (module scope)

**GO for the module.** It builds on Windows x64 and Android arm64, loads firmware
from files at runtime (firmware lives nowhere in its tree or artifacts), exposes
a stable versioned C ABI, fails cleanly on bad paths, and produces recognizable
audio (tone out; speech envelope 0.986, byte-identical to the baked-in baseline)
on both platforms. LICENSE and provenance are in place. QSO One's app tree is
untouched.

Standing items for the integration dispatch (out of scope here): the GPL/firmware
distribution model (QSO One fetches + SHA-verifies firmware, dynamically loads
this GPL module at arm's length), and the weak-hardware latency floor (only a
high-end Pixel measured; a W999-class device is still untested).
