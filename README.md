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
