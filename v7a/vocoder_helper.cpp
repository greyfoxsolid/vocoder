// qso_dmr_vocoder 32-bit helper (armeabi-v7a).
//
// Ships in the APK as libqso_dmr_vocoder_helper.so, Process.start'd by the app
// from applicationInfo.nativeLibraryDir (the only exec-allowed location under
// Android W^X). It runs the genuine MD-380 firmware AMBE codec NATIVELY on the
// 32-bit ARM host (nostar/md380_vocoder lineage), loading the firmware the app
// fetched at runtime — the firmware is NEVER shipped in this binary.
//
// LOGGING IS LOAD-BEARING. The go/no-go (does this device permit the fixed
// mappings + PROT_EXEC) is answered in the field, not here, so every step
// self-reports to stderr WITH ITS ERRNO before the helper can die. The kernel's
// SELinux denial lands in logcat, which our bundles cannot see; these stderr
// lines are what the app folds into the bundle so a failed device is diagnosable
// with no follow-up and no logcat.
//
// IPC: length-framed request/response on stdin/stdout (the Flutter-practical
// equivalent of a socketpair — Dart's Process API exposes pipes, not an
// inheritable socketpair fd). Logs on stderr. See helper_protocol.md.
//
// GPL (nostar/md380_vocoder + md380tools lineage). No VoxDMR code.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "qso_dmr_vocoder.h" // qdv_status codes (QDV_OK, QDV_ERR_*)
#include "dv_mode.h"         // the separate "dv" mode (D-Star / P25), never qdv_*

// ── Fixed firmware layout ────────────────────────────────────────────────────
// Addresses VERIFIED against the shipping dynarmic module's tables.h (which runs
// D002.032.bin on arm64/Windows today). The firmware window is 1 MiB; the real
// D002.032.bin blob is 994,304 B and is copied into the low part.
static const uintptr_t kFwAddr = 0x0800C000u;
static const size_t kFwSize = 0x00100000u; // 1 MiB window
static const uintptr_t kSramAddr = 0x20000000u;
static const size_t kSramSize = 0x00020000u; // 128 KiB, exact (d02032-core.img)
static const uintptr_t kTcramAddr = 0x10000000u;
static const size_t kTcramSize = 0x00020000u; // defensive: dynarmic maps TCRAM,
                                              // nostar did not; map it rather
                                              // than discover on a tester's radio.

// Helper-local status extending qdv_status for the exec-protection failure the
// public header has no code for (12+). The app maps any nonzero -> unsupportedAbi
// with the logged step, so the exact number matters less than the stderr line.
static const int QDV_ERR_MAP_FIXED = 12;
static const int QDV_ERR_MPROTECT_EXEC = 13; // the most likely field denial
static const int QDV_ERR_SELFTEST = 14;

// ── nostar core (md380_vocoder.c), compiled alongside; ambe_* entry points are
//    resolved at LINK time via --defsym (see build_v7a.sh). The CODE symbols
//    carry the Thumb bit (odd address) so a native BLX enters Thumb; the SRAM
//    data symbols are even. This is the deliberate Thumb-bit handling. ──────────
extern "C" {
void md380_encode(uint8_t* ambe49, const int16_t* pcm);
void md380_decode(uint8_t* ambe49, int16_t* pcm);
void md380_encode_fec(uint8_t* ambe, const int16_t* pcm);
void md380_decode_fec(const uint8_t* ambe, int16_t* pcm);
}
int qdv_unwrap_md380_impl(const char* wrapped_path,
                          const char* unwrapped_path); // md380_unwrap.cpp

static bool g_init = false;

// ── Logging (load-bearing) ───────────────────────────────────────────────────
static void vlog(const char* step, const char* msg, int err) {
  if (err) {
    fprintf(stderr, "[vhelper] step=%s %s errno=%d(%s)\n", step, msg, err,
            strerror(err));
  } else {
    fprintf(stderr, "[vhelper] step=%s %s\n", step, msg);
  }
  fflush(stderr);
}

// ── Crash + hang instrumentation (WE CANNOT DEBUG SILENCE) ───────────────────
// The go/no-go this helper answers is "does the firmware EXECUTE on this device."
// Mapping succeeding then execution dying (Chris N9ICV, Unisoc W6+) leaves NO log
// line because a SIGSEGV/SIGILL/SIGBUS is asynchronous to the vlog() calls above.
// A signal handler emits ONE line — signal + faulting address + PC + the step we
// were in — so the FIRST bundle discriminates: a stray peripheral/system access
// the arm64 emulator absorbed but native silicon faults (addr in 0x4000_xxxx /
// 0xE000_xxxx / low / bit-band), a bad Thumb/entry (pc == the entry address), an
// M-profile-only instruction (decode pc), or a non-returning firmware (pc/lr
// garbage). A self-test alarm distinguishes a HANG (spin) from a CRASH.
//
// The handler is async-signal-safe: it formats with hand-rolled helpers and
// write(2), never fprintf/snprintf (not on the async-signal-safe list), then
// restores the default disposition and re-raises so the process dies with the
// right status for the parent to read.
static volatile const char* g_step = "init";

static void ssAppendStr(char*& p, const char* s) {
  while (*s) *p++ = *s++;
}
static void ssAppendDec(char*& p, long v) {
  if (v < 0) { *p++ = '-'; v = -v; }
  char tmp[24];
  int i = 0;
  if (v == 0) tmp[i++] = '0';
  while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
  while (i) *p++ = tmp[--i];
}
static void ssAppendHex(char*& p, unsigned long v) {
  *p++ = '0';
  *p++ = 'x';
  char tmp[16];
  int i = 0;
  if (v == 0) tmp[i++] = '0';
  while (v) { int d = (int)(v & 0xf); tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
  while (i) *p++ = tmp[--i];
}

static void crashHandler(int sig, siginfo_t* info, void* uctx) {
  const char* name = "?";
  switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGILL:  name = "SIGILL";  break;
    case SIGBUS:  name = "SIGBUS";  break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGFPE:  name = "SIGFPE";  break;
    default: break;
  }
  unsigned long pc = 0, lr = 0, sp = 0, fault = 0;
#if defined(__arm__)
  if (uctx) {
    ucontext_t* uc = (ucontext_t*)uctx;
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
    fault = uc->uc_mcontext.fault_address;
  }
#endif
  unsigned long addr = info ? (unsigned long)info->si_addr : 0;
  char buf[256];
  char* p = buf;
  ssAppendStr(p, "[vhelper] step=");
  ssAppendStr(p, (const char*)g_step);
  ssAppendStr(p, " CRASH signal=");
  ssAppendDec(p, sig);
  ssAppendStr(p, "(");
  ssAppendStr(p, name);
  ssAppendStr(p, ") code=");
  ssAppendDec(p, info ? info->si_code : 0);
  ssAppendStr(p, " addr=");
  ssAppendHex(p, addr);
  ssAppendStr(p, " pc=");
  ssAppendHex(p, pc);
  ssAppendStr(p, " lr=");
  ssAppendHex(p, lr);
  ssAppendStr(p, " sp=");
  ssAppendHex(p, sp);
  ssAppendStr(p, " fault=");
  ssAppendHex(p, fault);
  *p++ = '\n';
  ssize_t w = write(2, buf, (size_t)(p - buf));
  (void)w;
  // Restore default disposition + re-raise so the process dies with the right
  // status (dart:io reports the signal as a negative exitCode to the parent).
  signal(sig, SIG_DFL);
  raise(sig);
}

// Self-test watchdog: a firmware that SPINS (rather than faults) would hang the
// helper — and the parent — forever. alarm(kSelfTestTimeoutSec) turns that into a
// distinct, non-silent verdict.
static const unsigned kSelfTestTimeoutSec = 5;
static void alarmHandler(int) {
  static const char m[] =
      "[vhelper] step=selftest TIMEOUT (firmware did not return)\n";
  ssize_t w = write(2, m, sizeof m - 1);
  (void)w;
  _exit(70); // distinct from any qdv_status rc
}

// A dedicated stack so the handler still runs (and emits its line) even if the
// firmware crash was a stack overflow (H2). Fixed size — SIGSTKSZ is no longer a
// compile-time constant on newer bionic.
static char g_altStack[65536];

static void installCrashHandlers() {
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = g_altStack;
  ss.ss_size = sizeof g_altStack;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crashHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);

  struct sigaction al;
  memset(&al, 0, sizeof al);
  al.sa_handler = alarmHandler;
  sigemptyset(&al.sa_mask);
  sigaction(SIGALRM, &al, nullptr);
}

// MAP_FIXED_NOREPLACE so an OCCUPIED range fails with EEXIST (tells us "something
// was already there") instead of silently clobbering. On kernels without the
// flag it degrades to MAP_FIXED; we then still verify the returned address.
static bool mapFixedRw(uintptr_t addr, size_t size, const char* step) {
  void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) {
    // errno == EEXIST here means the address was already occupied.
    char m[96];
    snprintf(m, sizeof m, "mmap MAP_FIXED 0x%08lx size=0x%lx failed%s",
             (unsigned long)addr, (unsigned long)size,
             errno == EEXIST ? " (address already occupied)" : "");
    vlog(step, m, errno);
    return false;
  }
  if (reinterpret_cast<uintptr_t>(p) != addr) {
    vlog(step, "mmap returned a DIFFERENT address (flag unsupported + occupied)",
         0);
    munmap(p, size);
    return false;
  }
  return true;
}

// Round-trip self-test: encode a 1 kHz tone, decode it, confirm the output
// carries energy. Catches "mapping succeeded but the firmware produced garbage"
// (a mapping win that is not a decode win). This is the "first decode attempt"
// the plan calls for, run inside init so the very first bundle reports it.
static bool selfTest() {
  int16_t pcm_in[160];
  for (int i = 0; i < 160; ++i) {
    // ~1 kHz at 8 kHz sample rate, moderate amplitude.
    double ph = 2.0 * 3.14159265358979 * 1000.0 * i / 8000.0;
    pcm_in[i] = (int16_t)(8000.0 * __builtin_sin(ph));
  }
  uint8_t ambe[7] = {0};
  // The FIRST firmware execution. Mark the step + arm the hang watchdog so a
  // crash names the exact call and a spin is not silent (see the crash handler).
  vlog("selftest_enter", "encode+decode roundtrip begin", 0);
  g_step = "selftest_encode";
  alarm(kSelfTestTimeoutSec);
  md380_encode(ambe, pcm_in);
  int16_t pcm_out[160] = {0};
  g_step = "selftest_decode";
  md380_decode(ambe, pcm_out);
  alarm(0);
  g_step = "selftest_energy";
  long energy = 0;
  for (int i = 0; i < 160; ++i) energy += (long)pcm_out[i] * pcm_out[i];
  char m[64];
  snprintf(m, sizeof m, "roundtrip energy=%ld", energy);
  vlog("selftest", m, 0);
  return energy > 100000; // any real speech energy clears this easily
}

// ── qdv ABI ──────────────────────────────────────────────────────────────────
extern "C" int qdv_abi_version(void) { return QDV_ABI_VERSION; } // 2

extern "C" const char* qdv_build_id(void) {
  return "qso_dmr_vocoder-v7a helper (nostar native md380, runtime-mapped)";
}

extern "C" int qdv_unwrap_md380(const char* w, const char* u) {
  return qdv_unwrap_md380_impl(w, u);
}

extern "C" int qdv_init(const char* fw_path, const char* core_path) {
  if (g_init) return QDV_ERR_ALREADY_INIT;
  if (!fw_path || !core_path) return QDV_ERR_NULL_ARG;

  // 1) firmware window @ 0x0800C000 (map RW, copy, then flip to R+X — the W^X
  //    order: never map exec-and-writable at once).
  FILE* f = fopen(fw_path, "rb");
  if (!f) {
    vlog("fw_open", fw_path, errno);
    return QDV_ERR_FIRMWARE_OPEN;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || (size_t)n > kFwSize) {
    fclose(f);
    vlog("fw_size", "firmware size out of range", 0);
    return QDV_ERR_FIRMWARE_SIZE;
  }
  if (!mapFixedRw(kFwAddr, kFwSize, "map_fw")) {
    fclose(f);
    return QDV_ERR_MAP_FIXED;
  }
  size_t got = fread(reinterpret_cast<void*>(kFwAddr), 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    vlog("fw_read", "short read", errno);
    return QDV_ERR_FIRMWARE_READ;
  }
  if (mprotect(reinterpret_cast<void*>(kFwAddr), kFwSize,
               PROT_READ | PROT_EXEC) != 0) {
    // THE most likely field denial: SELinux execmem/execmod / W^X.
    vlog("mprotect_fw_rx", "PROT_EXEC on firmware DENIED (SELinux execmem?)",
         errno);
    return QDV_ERR_MPROTECT_EXEC;
  }
  vlog("map_fw", "firmware mapped + R+X", 0);

  // 2) SRAM @ 0x20000000 (RW), initialised from core.img (exactly 0x20000).
  FILE* c = fopen(core_path, "rb");
  if (!c) {
    vlog("core_open", core_path, errno);
    return QDV_ERR_CORE_OPEN;
  }
  fseek(c, 0, SEEK_END);
  long cn = ftell(c);
  fseek(c, 0, SEEK_SET);
  if ((size_t)cn != kSramSize) {
    fclose(c);
    vlog("core_size", "core.img not exactly 128 KiB", 0);
    return QDV_ERR_CORE_SIZE;
  }
  if (!mapFixedRw(kSramAddr, kSramSize, "map_sram")) {
    fclose(c);
    return QDV_ERR_MAP_FIXED;
  }
  size_t cgot = fread(reinterpret_cast<void*>(kSramAddr), 1, kSramSize, c);
  fclose(c);
  if (cgot != kSramSize) {
    vlog("core_read", "short read", errno);
    return QDV_ERR_CORE_READ;
  }
  vlog("map_sram", "SRAM mapped + loaded", 0);

  // 3) TCRAM @ 0x10000000 (RW, zeroed) — defensive.
  if (!mapFixedRw(kTcramAddr, kTcramSize, "map_tcram")) {
    return QDV_ERR_MAP_FIXED;
  }
  memset(reinterpret_cast<void*>(kTcramAddr), 0, kTcramSize);
  vlog("map_tcram", "TCRAM mapped + zeroed", 0);

  g_init = true;
  if (!selfTest()) {
    vlog("selftest", "FAILED (mapped but firmware output silent) -> reject", 0);
    g_init = false;
    return QDV_ERR_SELFTEST;
  }
  vlog("init_ok", "helper ready", 0);
  return QDV_OK;
}

extern "C" void qdv_encode(uint8_t* ambe_out, const int16_t* pcm_in) {
  if (!g_init) return;
  md380_encode(ambe_out, pcm_in);
}
extern "C" void qdv_decode(const uint8_t* ambe_in, int16_t* pcm_out) {
  if (!g_init) return;
  md380_decode(const_cast<uint8_t*>(ambe_in), pcm_out);
}
extern "C" void qdv_encode_fec(uint8_t* ambe_out, const int16_t* pcm_in) {
  if (!g_init) return;
  md380_encode_fec(ambe_out, pcm_in);
}
extern "C" void qdv_decode_fec(const uint8_t* ambe_in, int16_t* pcm_out) {
  if (!g_init) return;
  md380_decode_fec(ambe_in, pcm_out);
}
extern "C" void qdv_shutdown(void) {
  if (!g_init) return;
  munmap(reinterpret_cast<void*>(kFwAddr), kFwSize);
  munmap(reinterpret_cast<void*>(kSramAddr), kSramSize);
  munmap(reinterpret_cast<void*>(kTcramAddr), kTcramSize);
  g_init = false;
}
// nostar has no md380_deinit(); our qdv_shutdown owns teardown instead.

// ── IPC request loop (stdin cmd, stdout reply, stderr logs) ──────────────────
static bool readN(void* buf, size_t n) {
  return fread(buf, 1, n, stdin) == n;
}
static void writeN(const void* buf, size_t n) {
  fwrite(buf, 1, n, stdout);
  fflush(stdout);
}
static bool readPath(char* out, size_t cap) {
  uint16_t len = 0;
  if (!readN(&len, 2)) return false;
  if (len >= cap) return false;
  if (!readN(out, len)) return false;
  out[len] = 0;
  return true;
}

int main(int argc, char** argv) {
  installCrashHandlers();
  // 2026-10-05: "dv" mode = the D-Star / P25 software codecs (../src/dv_mode.cpp),
  // a SEPARATE process started with this one argument. It never reaches the
  // MD-380 loop below and never calls qdv_*; with no argument (how the app has
  // always started the DMR / YSF helper) nothing below changes.
  if (argc >= 2 && strcmp(argv[1], "dv") == 0) {
    return qdv_dv_main(&g_step);
  }
  vlog("start", "vhelper alive; entering request loop", 0);
  uint8_t cmd;
  while (readN(&cmd, 1)) {
    switch (cmd) {
      case 'I': { // init: <u16 len><fw_path><u16 len><core_path>
        char fw[1024], core[1024];
        if (!readPath(fw, sizeof fw) || !readPath(core, sizeof core)) return 2;
        uint8_t rc = (uint8_t)qdv_init(fw, core);
        writeN(&rc, 1);
        break;
      }
      case 'D': { // decode: 7 AMBE -> 160 int16 PCM (320 bytes)
        uint8_t a[7];
        if (!readN(a, 7)) return 2;
        int16_t pcm[160] = {0};
        qdv_decode(a, pcm);
        writeN(pcm, 320);
        break;
      }
      case 'F': { // decode_fec: 9 AMBE -> 160 int16 PCM
        uint8_t a[9];
        if (!readN(a, 9)) return 2;
        int16_t pcm[160] = {0};
        qdv_decode_fec(a, pcm);
        writeN(pcm, 320);
        break;
      }
      case 'E': { // encode: 160 int16 PCM (320 bytes) -> 7 AMBE (49-bit, raw)
        int16_t pcm[160];
        if (!readN(pcm, 320)) return 2;
        uint8_t a[7] = {0};
        qdv_encode(a, pcm);
        writeN(a, 7);
        break;
      }
      case 'G': { // encode_fec: 160 int16 PCM (320 bytes) -> 9 AMBE (72-bit FEC)
        // The DMR TX path (Rewind + Homebrew) wants 72-bit FEC codewords, the
        // symmetric counterpart of the 'F' decode. md380_encode_fec = the proven
        // firmware md380_encode + pure-C Golay/Hamming FEC (the same table math
        // the working RX decode_fec relies on).
        int16_t pcm[160];
        if (!readN(pcm, 320)) return 2;
        uint8_t a[9] = {0};
        qdv_encode_fec(a, pcm);
        writeN(a, 9);
        break;
      }
      case 'U': { // unwrap: <wrapped><out> -> 1 status byte
        char w[1024], u[1024];
        if (!readPath(w, sizeof w) || !readPath(u, sizeof u)) return 2;
        uint8_t rc = (uint8_t)qdv_unwrap_md380(w, u);
        writeN(&rc, 1);
        break;
      }
      case 'Q':
        vlog("quit", "graceful shutdown requested", 0);
        qdv_shutdown();
        return 0;
      default:
        vlog("proto", "unknown command byte", 0);
        return 2;
    }
  }
  vlog("parent_gone", "stdin closed; exiting", 0);
  return 0;
}
