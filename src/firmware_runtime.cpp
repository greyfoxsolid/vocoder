/*
 * firmware_runtime.cpp - runtime firmware loader for qso_dmr_vocoder.
 *
 * Replaces the upstream xxd build-time embed (firmware.c / sram.c). The
 * upstream codec (md380_vocoder.cpp, via tables.h) expects two global symbols:
 *
 *   extern unsigned char firmware[];   // MD380 application firmware @ guest 0x0800C000
 *   extern unsigned char sram[];        // SRAM core image, copied to guest 0x20000000
 *
 * Here those are zero-initialized BSS buffers (NO firmware bytes in source or
 * artifact). They are filled at runtime from caller-supplied files by
 * qdv_runtime_load(). The mapping reproduces the spike's baked-in layout
 * byte-for-byte: firmware bytes at offset 0 of the firmware buffer (the codec
 * indexes firmware[vaddr - 0x0800C000]); core bytes are the 0x20000 region the
 * emulator copies into guest SRAM.
 *
 * GPL: part of the qso_dmr_vocoder module. See ../LICENSE and ../NOTICE.md.
 */

#include <cstdio>
#include <cstring>

#include "qso_dmr_vocoder.h"

// B3 (TA2IW / F4LZT): open UTF-8 paths via the UTF-16 API on Windows so a
// non-ASCII %APPDATA% profile path (Turkish/French usernames) resolves. A narrow
// fopen() uses the ANSI code page and fails on such paths -> QDV_ERR_FIRMWARE_OPEN
// / QDV_ERR_CORE_OPEN. POSIX fopen is already UTF-8. Mirrors md380_unwrap.cpp.
#if defined(_WIN32)
#include <windows.h>
#include <string>
static FILE* qdv_fopen_utf8(const char* path_utf8, const char* mode_ascii) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, &wpath[0], wlen);
    wchar_t wmode[8] = {0};
    for (int i = 0; mode_ascii[i] && i < 7; ++i) wmode[i] = (wchar_t)mode_ascii[i];
    return _wfopen(wpath.c_str(), wmode);
}
#else
static FILE* qdv_fopen_utf8(const char* path, const char* mode) {
    return fopen(path, mode);
}
#endif

// Guest firmware window is 0x100000 (1 MiB) per the emulator's MemoryRead8
// bounds. The real MD380 firmware (D002.032.bin) is 994304 bytes; we size the
// buffer to the full window so any in-window access stays in bounds.
static const size_t kFirmwareCapacity = 0x100000;  // 1 MiB guest window
// The emulator copies exactly 0x20000 bytes from the core image into SRAM, so
// the core file must be exactly this size.
static const size_t kCoreSize = 0x20000;            // 131072 bytes
// Lower sanity bound for the firmware file (guards obviously-wrong inputs).
static const size_t kFirmwareMinSize = 0x80000;     // 512 KiB

// Symbols the upstream codec links against (tables.h externs). BSS: no bytes
// committed, zero-filled at load.
unsigned char firmware[kFirmwareCapacity];
unsigned char sram[kCoreSize];
unsigned int firmware_len = 0;
unsigned int sram_len = 0;

// Returns the file size via ftell, or (size_t)-1 on error. Leaves f rewound.
static size_t file_size(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return (size_t)-1;
    long n = ftell(f);
    if (n < 0) return (size_t)-1;
    if (fseek(f, 0, SEEK_SET) != 0) return (size_t)-1;
    return (size_t)n;
}

// Loads firmware + core files into the guest buffers. Returns a qdv_status.
// Declared in qdv_abi.cpp (internal linkage boundary).
int qdv_runtime_load(const char* firmware_path, const char* core_path) {
    // --- firmware ---
    FILE* ff = qdv_fopen_utf8(firmware_path, "rb");
    if (!ff) return QDV_ERR_FIRMWARE_OPEN;
    size_t fsz = file_size(ff);
    if (fsz == (size_t)-1 || fsz < kFirmwareMinSize || fsz > kFirmwareCapacity) {
        fclose(ff);
        return QDV_ERR_FIRMWARE_SIZE;
    }
    memset(firmware, 0, kFirmwareCapacity);
    size_t got = fread(firmware, 1, fsz, ff);
    fclose(ff);
    if (got != fsz) return QDV_ERR_FIRMWARE_READ;
    firmware_len = (unsigned int)fsz;

    // --- core (SRAM image) ---
    FILE* cf = qdv_fopen_utf8(core_path, "rb");
    if (!cf) return QDV_ERR_CORE_OPEN;
    size_t csz = file_size(cf);
    if (csz != kCoreSize) {
        fclose(cf);
        return QDV_ERR_CORE_SIZE;
    }
    memset(sram, 0, kCoreSize);
    got = fread(sram, 1, kCoreSize, cf);
    fclose(cf);
    if (got != kCoreSize) return QDV_ERR_CORE_READ;
    sram_len = (unsigned int)kCoreSize;

    return QDV_OK;
}
