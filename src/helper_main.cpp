// qso_dmr_vocoder helper — one IPC front end, three targets:
//   Android arm64-v8a / x86_64  -> libqso_dmr_vocoder_helper.so  (execve'd from
//                                  nativeLibraryDir; Android W^X)
//   Windows  x64                -> qso_dmr_vocoder_helper.exe     (bundled next to
//                                  the app .exe; Process.start'd)
//
// It runs the SAME dynarmic AMBE+2 codec the app used to dlopen in-process
// (qso_dmr_vocoder / md380_vocoder_dynarmic), but now in ITS OWN PROCESS, so
// nothing executable is downloaded and the GPL codec stays in its own binary
// rather than being linked into the closed app. On Windows this is what mirrors
// the Android shape: the Microsoft Store forbids a packaged app loading code it
// fetched from outside the package, and the GPL separation the compliance notes
// rest on requires the codec NOT be shipped in-process. A separate process
// satisfies both (see ../LICENSE, ../NOTICE.md, GPL_VOCODER_COMPLIANCE).
//
// This file is ONLY the IPC front end + crash/log instrumentation. The qdv_*
// codec implementation is provided by the linked-in dynarmic module
// (src/qdv_abi.cpp over third_party/md380_vocoder_dynarmic + dynarmic). The
// firmware is loaded by qdv_init() from the runtime file paths the app fetched;
// it is NEVER shipped in this binary.
//
// The wire protocol and the stderr `[vhelper] step=<name> ... errno=<n>` line
// format are IDENTICAL on every platform, so the existing VocoderHelperClient
// (spawn + framed stdin/stdout IPC + stderr parser) drives this unchanged — one
// client, one protocol, all architectures. The ONLY genuine platform forks are:
//   1. binary stdio on Windows (_setmode) — without it the CRT translates
//      0x0A<->0x0D0A and mangles every framed byte of the IPC. Load-bearing.
//   2. crash instrumentation: POSIX signals on Android, SEH on Windows.
//   3. the self-test HANG watchdog: an in-process alarm() on Android; on Windows
//      it is a no-op because the app-side VocoderHelperClient._kInitTimeout (12 s)
//      already turns a spinning codec into a reported failure.
//
// GPL: this executable statically links the GPL qso_dmr_vocoder codec. It is a
// separate, source-available binary run at arm's length; see ../LICENSE and
// ../NOTICE.md.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <cstdlib>    // _exit
#include <cmath>      // std::sin (MSVC has no __builtin_sin)
#include <string>     // std::wstring for the UTF-16 path/size probe
#include <io.h>       // _setmode, _fileno
#include <fcntl.h>    // _O_BINARY
#include <windows.h>  // SetUnhandledExceptionFilter (the crash line)
#else
#include <csignal>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "qso_dmr_vocoder.h" // qdv_* ABI (implemented by the linked dynarmic module)
#include "dv_mode.h"         // the separate "dv" mode (D-Star / P25), never qdv_*

// ── Logging (load-bearing; identical format on every platform) ───────────────
static void vlog(const char* step, const char* msg, int err) {
  if (err) {
    fprintf(stderr, "[vhelper] step=%s %s errno=%d(%s)\n", step, msg, err,
            strerror(err));
  } else {
    fprintf(stderr, "[vhelper] step=%s %s\n", step, msg);
  }
  fflush(stderr);
}

// ── Crash instrumentation (WE CANNOT DEBUG SILENCE) ──────────────────────────
// One async-safe line names the fault, the faulting address, the PC and the step
// we were in, so the FIRST bundle discriminates a policy denial / codec fault /
// hang. Shared across platforms: the STEP string and the tiny string builders.
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

#if defined(_WIN32)

// Windows has no POSIX signals. An SEH filter writes the same `[vhelper] step=…
// CRASH …` line naming the exception code + faulting address, so a Windows tester
// bundle discriminates a codec fault the same way the Android one does.
static LONG WINAPI winCrashFilter(EXCEPTION_POINTERS* ep) {
  char buf[256];
  char* p = buf;
  ssAppendStr(p, "[vhelper] step=");
  ssAppendStr(p, (const char*)g_step);
  ssAppendStr(p, " CRASH code=");
  ssAppendHex(p, ep ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0);
  ssAppendStr(p, " addr=");
  ssAppendHex(p,
      ep ? (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0);
  *p++ = '\n';
  fwrite(buf, 1, (size_t)(p - buf), stderr);
  fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER; // let the process die with a nonzero code
}

static void installCrashHandlers() { SetUnhandledExceptionFilter(winCrashFilter); }

// The self-test HANG watchdog is app-side on Windows (VocoderHelperClient's 12 s
// init timeout → HelperInitResult.diedDetail), so the in-process alarm is a no-op.
static inline unsigned alarm(unsigned) { return 0; }

#else // POSIX (Android)

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
  unsigned long pc = 0, sp = 0, lr = 0;
  if (uctx) {
    ucontext_t* uc = (ucontext_t*)uctx;
#if defined(__aarch64__)
    pc = (unsigned long)uc->uc_mcontext.pc;
    sp = (unsigned long)uc->uc_mcontext.sp;
    lr = (unsigned long)uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    pc = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
    sp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__arm__)
    pc = (unsigned long)uc->uc_mcontext.arm_pc;
    sp = (unsigned long)uc->uc_mcontext.arm_sp;
    lr = (unsigned long)uc->uc_mcontext.arm_lr;
#endif
  }
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
  ssAppendStr(p, " sp=");
  ssAppendHex(p, sp);
  ssAppendStr(p, " lr=");
  ssAppendHex(p, lr);
  *p++ = '\n';
  ssize_t w = write(2, buf, (size_t)(p - buf));
  (void)w;
  signal(sig, SIG_DFL);
  raise(sig);
}

// Self-test watchdog: a codec that SPINS would hang the helper — and the parent —
// forever. alarm() turns that into a distinct, non-silent verdict.
static const unsigned kSelfTestTimeoutSec = 5;
static void alarmHandler(int) {
  static const char m[] =
      "[vhelper] step=selftest TIMEOUT (codec did not return)\n";
  ssize_t w = write(2, m, sizeof m - 1);
  (void)w;
  _exit(70); // distinct from any qdv_status rc
}

// A dedicated stack so the handler still runs even after a stack overflow. Fixed
// size — SIGSTKSZ is no longer a compile-time constant on newer bionic.
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

#endif // _WIN32

// Round-trip self-test AFTER qdv_init: encode a 1 kHz tone, decode it, confirm
// the output carries energy. Catches "the JIT built but produces garbage / cannot
// execmem" as a first-decode verdict in the very first bundle. Returns true if the
// decode carried real energy.
static bool selfTest() {
  int16_t pcm_in[160];
  for (int i = 0; i < 160; ++i) {
    double ph = 2.0 * 3.14159265358979 * 1000.0 * i / 8000.0;
#if defined(_WIN32)
    pcm_in[i] = (int16_t)(8000.0 * std::sin(ph)); // MSVC has no __builtin_sin
#else
    pcm_in[i] = (int16_t)(8000.0 * __builtin_sin(ph));
#endif
  }
  uint8_t ambe[7] = {0};
  vlog("selftest_enter", "encode+decode roundtrip begin", 0);
  g_step = "selftest_encode";
#if !defined(_WIN32)
  alarm(kSelfTestTimeoutSec); // POSIX in-process hang watchdog; no-op on Windows
#endif
  qdv_encode(ambe, pcm_in);
  int16_t pcm_out[160] = {0};
  g_step = "selftest_decode";
  qdv_decode(ambe, pcm_out);
#if !defined(_WIN32)
  alarm(0);
#endif
  g_step = "selftest_energy";
  long energy = 0;
  for (int i = 0; i < 160; ++i) energy += (long)pcm_out[i] * pcm_out[i];
  char m[64];
  snprintf(m, sizeof m, "roundtrip energy=%ld", energy);
  vlog("selftest", m, 0);
  // B3: PRIME the encoder with a few silence frames (output DISCARDED, never on
  // the wire) so its inter-frame state is converged before the FIRST real over.
  // Load-bearing now that the DMR auto-key sends header+terminator only (it no
  // longer warms the encoder), and it addresses the YSF/DMR cold-first-frame the
  // echo reflectors played back as a tone. Encode-only; the codec self-test above
  // already proved encode+decode work, so a bad frame here cannot mask that.
  g_step = "selftest_prime";
  int16_t sil[160] = {0};
  uint8_t sambe[7] = {0};
  for (int k = 0; k < 3; ++k) qdv_encode(sambe, sil);
  return energy > 100000;
}

// Helper-local status for a self-test failure (mapping/JIT succeeded but the codec
// produced silence). The app maps any nonzero init rc -> unsupportedAbi with the
// logged step, so the exact number matters less than the stderr line.
static const int QDV_ERR_SELFTEST = 14;

// B3 (KM4FEQ): helper-local status for a GUARDED qdv_init crash. On some Windows
// machines qdv_init faults with 0xc0000094 (integer divide by zero) inside
// dynarmic's Jit construction (a host-CPU-feature-derived path, not our code and
// not a Dart-guardable input). Without a guard the whole helper process died and
// the app looped re-spawning it. guardedQdvInit() catches the SEH exception,
// logs the code, and returns this rc so the app reports a clean codec failure.
static const int QDV_ERR_INIT_CRASH = 15;

#if defined(_WIN32)
// SEH must live in its own function with no C++ unwindable locals. qdv_init has
// no fallback codec on Windows (the dynarmic MD380 IS the codec), so the win is
// turning a process-killing crash into a clean, bounded, accurately-messaged
// failure rather than the ~1 s respawn loop.
static int guardedQdvInit(const char* fw, const char* core) {
  __try {
    return qdv_init(fw, core);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    char m[64];
    snprintf(m, sizeof m, "qdv_init CRASH code=0x%lx (guarded -> reject)",
             (unsigned long)GetExceptionCode());
    vlog("qdv_init", m, 0);
    return QDV_ERR_INIT_CRASH;
  }
}

// UTF-16 file size probe (bytes) for the unwrap diagnostic — narrow stat/fopen
// can't see a non-ASCII path, which is the very case we are diagnosing.
static long utf8FileSize(const char* path_utf8) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, nullptr, 0);
  if (wlen <= 0) return -1;
  std::wstring wpath((size_t)wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, &wpath[0], wlen);
  FILE* f = _wfopen(wpath.c_str(), L"rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}
#endif

// ── IPC request loop (stdin cmd, stdout reply, stderr logs) ──────────────────
// Byte-for-byte the same framing/commands on every platform.
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
#if defined(_WIN32)
  // CRITICAL (Windows-only): binary stdio, or the CRT translates 0x0A<->0x0D0A on
  // stdin/stdout and mangles every framed byte of the binary IPC. The one line the
  // whole Windows subprocess path depends on.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  installCrashHandlers();
  // 2026-10-05: "dv" mode = the D-Star / P25 software codecs (src/dv_mode.cpp),
  // a SEPARATE process started with this one argument. It never reaches the
  // MD-380 loop below and never calls qdv_*; with no argument (how the app has
  // always started the DMR / YSF helper) nothing below changes.
  if (argc >= 2 && strcmp(argv[1], "dv") == 0) {
    return qdv_dv_main(&g_step);
  }
  vlog("start", qdv_build_id(), 0);
  vlog("start", "vhelper alive; entering request loop", 0);
  uint8_t cmd;
  while (readN(&cmd, 1)) {
    switch (cmd) {
      case 'I': { // init: <u16 len><fw_path><u16 len><core_path>  -> 1 status byte
        char fw[1024], core[1024];
        if (!readPath(fw, sizeof fw) || !readPath(core, sizeof core)) return 2;
        g_step = "qdv_init";
#if defined(_WIN32)
        int rc = guardedQdvInit(fw, core); // B3: SEH-guarded (0xc0000094)
#else
        int rc = qdv_init(fw, core);
#endif
        if (rc == QDV_OK && !selfTest()) {
          vlog("selftest", "FAILED (JIT ran but codec output silent) -> reject", 0);
          qdv_shutdown();
          rc = QDV_ERR_SELFTEST;
        }
        g_step = "loop";
        uint8_t b = (uint8_t)rc;
        writeN(&b, 1);
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
        // B3 (TA2IW / F4LZT): log the wrapped path + byte size so the NEXT
        // bundle distinguishes a non-ASCII-path open failure from a short
        // read. rc 9 = QDV_ERR_UNWRAP_OPEN covers both; only the path/size
        // tell them apart.
        {
          char m[1200];
#if defined(_WIN32)
          snprintf(m, sizeof m, "wrapped=\"%s\" bytes=%ld", w, utf8FileSize(w));
#else
          FILE* pf = fopen(w, "rb");
          long sz = -1;
          if (pf) { fseek(pf, 0, SEEK_END); sz = ftell(pf); fclose(pf); }
          snprintf(m, sizeof m, "wrapped=\"%s\" bytes=%ld", w, sz);
#endif
          vlog("unwrap", m, 0);
        }
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
