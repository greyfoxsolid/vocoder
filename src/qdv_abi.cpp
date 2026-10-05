/*
 * qdv_abi.cpp - stable extern "C" ABI over the GPL md380 codec.
 *
 * This is the clean C boundary QSO One calls across via dart:ffi. It does not
 * touch the codec math; it loads firmware at runtime (firmware_runtime.cpp),
 * drives the upstream md380_* functions, and manages init/teardown lifecycle.
 *
 * GPL: part of the qso_dmr_vocoder module. See ../LICENSE and ../NOTICE.md.
 */

#define QDV_BUILDING 1
#include "qso_dmr_vocoder.h"

#include "md380_vocoder.h" // upstream GPL codec (vendored, third_party/)

// Defined in firmware_runtime.cpp.
int qdv_runtime_load(const char* firmware_path, const char* core_path);
// Defined in md380_unwrap.cpp.
int qdv_unwrap_md380_impl(const char* wrapped_path, const char* unwrapped_path);

namespace {
bool g_initialized = false;

const char kBuildId[] =
    "qso_dmr_vocoder/1.1.0 abi2 "
    "(md380_vocoder_dynarmic@55c9a2c GPL; dynarmic@yuzu-mirror/9d45823 0BSD; "
    "xbyak-amd-legacy-fix@53be499)";
} // namespace

extern "C" {

int qdv_abi_version(void) {
    return QDV_ABI_VERSION;
}

const char* qdv_build_id(void) {
    return kBuildId;
}

int qdv_unwrap_md380(const char* wrapped_path, const char* unwrapped_path) {
    if (!wrapped_path || !unwrapped_path) return QDV_ERR_NULL_ARG;
    return qdv_unwrap_md380_impl(wrapped_path, unwrapped_path);
}

int qdv_init(const char* firmware_path, const char* core_path) {
    if (g_initialized) return QDV_ERR_ALREADY_INIT;
    if (!firmware_path || !core_path) return QDV_ERR_NULL_ARG;

    int rc = qdv_runtime_load(firmware_path, core_path);
    if (rc != QDV_OK) return rc; // firmware/sram buffers untouched on failure path semantics

    md380_init(); // builds the emulator over the now-loaded firmware[]/sram[]
    g_initialized = true;
    return QDV_OK;
}

void qdv_encode(uint8_t* ambe_out, const int16_t* pcm_in) {
    if (!g_initialized || !ambe_out || !pcm_in) return;
    md380_encode(ambe_out, const_cast<int16_t*>(pcm_in));
}

void qdv_decode(const uint8_t* ambe_in, int16_t* pcm_out) {
    if (!g_initialized || !ambe_in || !pcm_out) return;
    md380_decode(const_cast<uint8_t*>(ambe_in), pcm_out);
}

void qdv_encode_fec(uint8_t* ambe_out, const int16_t* pcm_in) {
    if (!g_initialized || !ambe_out || !pcm_in) return;
    md380_encode_fec(ambe_out, const_cast<int16_t*>(pcm_in));
}

void qdv_decode_fec(const uint8_t* ambe_in, int16_t* pcm_out) {
    if (!g_initialized || !ambe_in || !pcm_out) return;
    md380_decode_fec(const_cast<uint8_t*>(ambe_in), pcm_out);
}

void qdv_shutdown(void) {
    if (!g_initialized) return;
    md380_deinit();
    g_initialized = false;
}

} // extern "C"
