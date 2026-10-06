# qso_dmr_vocoder

Standalone, source-available **GPL** DMR (AMBE+2 2450x1150) voice codec module.
It wraps the `md380_vocoder_dynarmic` codec (MD380 firmware run under the
dynarmic ARM JIT) behind a stable, versioned `extern "C"` ABI, and loads the
firmware from files **at runtime** rather than embedding it at build time.

This exists so QSO One (a closed application) can use a GPL codec at arm's
length: QSO One does not contain this module; it fetches and loads it at runtime
via `dart:ffi`. See `NOTICE.md` for licensing and provenance, and
`MODULE_NOTES.md` for the firmware memory mapping, the full ABI, build commands,
and correctness results.

## Important

- **GPL.** The whole module is GPL because the codec is GPL. Keep it a separate
  artifact; do not statically fold it into a proprietary binary.
- **No firmware here.** The MD380 firmware (`D002.032.bin`, `d02032-core.img`)
  is non-redistributable TYT code. It is never committed to this tree or shipped
  in a release. It is passed to `qdv_init()` as file paths at runtime.

## ABI (see `include/qso_dmr_vocoder.h`)

```c
int          qdv_abi_version(void);                       // QDV_ABI_VERSION
const char*  qdv_build_id(void);                          // build string
int          qdv_init(const char* firmware_path,          // 0 = QDV_OK, else qdv_status
                      const char* core_path);
void         qdv_encode(uint8_t* ambe_out,  const int16_t* pcm_in);   // 160 int16 -> 7 bytes
void         qdv_decode(const uint8_t* ambe_in, int16_t* pcm_out);    // 7 bytes -> 160 int16
void         qdv_encode_fec(uint8_t* ambe_out, const int16_t* pcm_in);
void         qdv_decode_fec(const uint8_t* ambe_in, int16_t* pcm_out);
void         qdv_shutdown(void);
```

## Build

Boost headers (1.57+) are required by dynarmic; set `BOOST_ROOT`. dynarmic and
its externals are fetched by CMake. No python3/unzip/xxd/curl are needed.

- Windows x64: `build_windows.bat` -> `build-windows/qso_dmr_vocoder.dll`
- Android arm64: `build_android.bat` (NDK 27, arm64-v8a) -> `build-android/libqso_dmr_vocoder.so`

## D-Star and P25 codecs: the helper's "dv" mode (2026-10-05)

The bundled helper executable also carries the D-Star (AMBE 3600x2400 with
D-Star FEC, 9-byte frames) and P25 Phase 1 (IMBE 7200x4400, 11-byte frames)
software voice codecs. They run ONLY when the helper is started with the single
argument `dv`, as a separate process; that mode never calls `qdv_*` and needs no
firmware. Protocol: `src/dv_mode.cpp`. Codecs: `third_party/dv_codecs/`
(mbelib ISC, Pavel Yazev IMBE GPL v3+, OP25 AMBE encoder GPL v3+, MMDVMHost
tables GPL v2+, DroidStar glue GPL v3+); our local changes, including the D-Star
loudness fix and the D-Star pitch fix, are listed in `NOTICE.md`.

Builds (every target links dv mode):
- Windows x64: `build_windows.bat` -> `build-windows/qso_dmr_vocoder_helper.exe`
  (+ `dv_test.exe`)
- Android arm64-v8a: `build_android.bat` -> `build-android/libqso_dmr_vocoder_helper.so`
- Android x86_64: `build_android_x64.bat` -> `build-android-x64/` (then
  `llvm-strip --strip-all`)
- Android armeabi-v7a: `build_android_v7a.bat` -> `build-android-v7a/` (the
  native 32-bit MD-380 helper from `v7a/`, plus dv mode)

Test: `dv_test <test/clips/male_harvard_8k.wav> <test/clips/female_harvard_8k.wav>`
(frame sizes, silence, decoded level tracks the input on normal / quiet / loud
speech with no clipping, reset, CPU per frame).

Note: build from a short path (for example `C:\Users\<you>\qdvb\`). From a deep
path the dynarmic checkout exceeds the Windows 260-character limit and the
configure fails.
