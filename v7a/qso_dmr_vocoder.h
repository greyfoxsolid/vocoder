/*
 * qso_dmr_vocoder - stable C ABI for the QSO One DMR (AMBE+2) vocoder module.
 *
 * This module is GPL (it incorporates md380_vocoder_dynarmic). It is a
 * standalone, source-available artifact that QSO One loads at runtime via
 * dart:ffi; QSO One's closed binary does not contain it. The MD380 firmware is
 * non-redistributable and is NOT part of this module: it is supplied to
 * qdv_init() as runtime file paths. See LICENSE and NOTICE.md.
 *
 * ABI contract: keep this header in lockstep with the module. Bump
 * QDV_ABI_VERSION on any breaking change so a consumer can refuse a
 * mismatched module via qdv_abi_version().
 */
#ifndef QSO_DMR_VOCODER_H
#define QSO_DMR_VOCODER_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(QDV_BUILDING)
#    define QDV_API __declspec(dllexport)
#  else
#    define QDV_API __declspec(dllimport)
#  endif
#else
#  define QDV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. Incremented on any breaking change to the surface below.
 * v2 added qdv_unwrap_md380(). A consumer that needs the unwrap must pin >= 2. */
#define QDV_ABI_VERSION 2

/* Return / error codes. 0 = success, nonzero = failure. */
typedef enum {
    QDV_OK                 = 0,
    QDV_ERR_ALREADY_INIT   = 1, /* qdv_init() called while already initialized  */
    QDV_ERR_NULL_ARG       = 2, /* a required path argument was NULL            */
    QDV_ERR_FIRMWARE_OPEN  = 3, /* firmware file could not be opened            */
    QDV_ERR_FIRMWARE_SIZE  = 4, /* firmware file size out of range              */
    QDV_ERR_FIRMWARE_READ  = 5, /* firmware file read failed                    */
    QDV_ERR_CORE_OPEN      = 6, /* core (SRAM) file could not be opened         */
    QDV_ERR_CORE_SIZE      = 7, /* core file is not exactly the required size   */
    QDV_ERR_CORE_READ      = 8, /* core file read failed                        */
    QDV_ERR_UNWRAP_OPEN    = 9, /* wrapped OEM firmware could not be opened/read */
    QDV_ERR_UNWRAP_FORMAT  = 10,/* wrapped OEM firmware header/size not valid   */
    QDV_ERR_UNWRAP_WRITE   = 11 /* unwrapped output could not be written        */
} qdv_status;

/* Returns QDV_ABI_VERSION the module was built with. Call before anything else
 * and refuse the module if it does not match the value this header declares. */
QDV_API int qdv_abi_version(void);

/* Human-readable build identifier (module version + incorporated commit refs).
 * Stable string, owned by the module; do not free. */
QDV_API const char* qdv_build_id(void);

/* Initialize the codec by loading firmware from files at runtime.
 *   firmware_path : MD380 application firmware (D002.032.bin, 994304 bytes),
 *                   mapped into guest memory at 0x0800C000.
 *   core_path     : SRAM core image (d02032-core.img, exactly 131072 bytes),
 *                   copied into guest SRAM at 0x20000000.
 * Returns QDV_OK on success or a nonzero qdv_status on failure. Fallible by
 * design: runtime file load can fail. Must succeed before any encode/decode.
 * Calling twice without qdv_shutdown() returns QDV_ERR_ALREADY_INIT. */
QDV_API int qdv_init(const char* firmware_path, const char* core_path);

/* Unwrap a wrapped MD380 OEM firmware image into the raw application image the
 * codec needs. Input is the OEM file (e.g. MD-380-D2.32(AD).bin extracted from
 * TYT's update zip); output is D002.032.bin. This GPL md380tools-derived
 * transform lives in the module by design (kept out of the closed caller). The
 * caller fetches/extracts the wrapped file (generic, non-GPL) and SHA-verifies
 * the output. Returns QDV_OK or a nonzero qdv_status (QDV_ERR_UNWRAP_*). */
QDV_API int qdv_unwrap_md380(const char* wrapped_path, const char* unwrapped_path);

/* One 20 ms voice frame (8 kHz). Data shapes match the upstream codec:
 *   encode: pcm_in  = 160 int16 samples  -> ambe_out = 7 bytes (49 bits, MSB-first)
 *   decode: ambe_in = 7 bytes            -> pcm_out  = 160 int16 samples
 * No-ops (leave output unmodified) if called before a successful qdv_init(). */
QDV_API void qdv_encode(uint8_t* ambe_out, const int16_t* pcm_in);
QDV_API void qdv_decode(const uint8_t* ambe_in, int16_t* pcm_out);

/* FEC variants (DMR on-air 49->96 bit framing handled internally). */
QDV_API void qdv_encode_fec(uint8_t* ambe_out, const int16_t* pcm_in);
QDV_API void qdv_decode_fec(const uint8_t* ambe_in, int16_t* pcm_out);

/* Free the emulator. Safe to call when not initialized. After this, qdv_init()
 * may be called again. */
QDV_API void qdv_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QSO_DMR_VOCODER_H */
