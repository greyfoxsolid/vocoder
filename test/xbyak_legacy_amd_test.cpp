// Runs xbyak's Xbyak::util::Cpu constructor (what dynarmic calls in
// GetHostFeatures() on the first qdv_init) against SIMULATED CPUID tables, so
// the WX4TAZ crash (AMD A6-3650 "Llano", qdv_init CRASH code=0xc0000094, an
// integer divide by zero in setCacheHierarchy) can be reproduced and proven
// fixed on any x64 Windows PC, not only on an old AMD machine.
//
// How: xbyak calls the MSVC intrinsics __cpuid / __cpuidex. This file includes
// <intrin.h> first, then redirects those two names to a table lookup before
// including xbyak_util.h, so every CPUID the Cpu class reads comes from the
// table below. The CPU the test runs on does not matter.
//
// Build (from a VS x64 developer prompt), against the header under test:
//   cl /nologo /EHsc /std:c++20 /O2 /I <dir holding xbyak/xbyak_util.h>
//      test\xbyak_legacy_amd_test.cpp /Fe:xbyak_legacy_amd_test.exe
// Exit code 0 = every profile constructed without a crash and with sane
// values. A crash is caught (SEH) and reported as "CRASH code=0x...".
//
// Llano values: AMD family 12h (base F + ext 3), 4 cores, no SMT, max standard
// leaf 6 (so no leaf 0xB), max extended leaf 0x8000001B (so no 0x8000001D),
// 64 KB L1d, 1 MB L2, no L3. Taken from the AMD family 12h BKDG CPUID layout.

#include <intrin.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

namespace fake {
struct Leaf { uint32_t a, b, c, d; };
static std::map<std::pair<uint32_t, uint32_t>, Leaf> g_table;  // (leaf, subleaf)
static std::map<uint32_t, Leaf> g_any_sub;                      // leaf, any subleaf

static void lookup(int out[4], uint32_t leaf, uint32_t sub) {
  Leaf r{0, 0, 0, 0};
  auto it = g_table.find({leaf, sub});
  if (it != g_table.end()) {
    r = it->second;
  } else {
    auto it2 = g_any_sub.find(leaf);
    if (it2 != g_any_sub.end()) r = it2->second;
  }
  out[0] = (int)r.a; out[1] = (int)r.b; out[2] = (int)r.c; out[3] = (int)r.d;
}
static void cpuid(int out[4], int leaf) { lookup(out, (uint32_t)leaf, 0); }
static void cpuidex(int out[4], int leaf, int sub) { lookup(out, (uint32_t)leaf, (uint32_t)sub); }
}  // namespace fake

#define __cpuid fake::cpuid
#define __cpuidex fake::cpuidex
#include <xbyak/xbyak_util.h>
#undef __cpuid
#undef __cpuidex

static const uint32_t kAuth = 0x68747541, kEnti = 0x69746E65, kCAMD = 0x444D4163;
static const uint32_t kGenu = 0x756E6547, kIneI = 0x49656E69, kNtel = 0x6C65746E;

static void loadLlano() {
  using fake::Leaf;
  fake::g_table.clear(); fake::g_any_sub.clear();
  fake::g_any_sub[0x0] = Leaf{0x6, kAuth, kCAMD, kEnti};
  // family F + ext 3 = 12h, model 1; EBX[23:16] = 4 logical; EDX bit 28 HTT.
  fake::g_any_sub[0x1] = Leaf{0x00300F10, 0x00040800, 0x00802009, 0x178BFBFF};
  fake::g_any_sub[0x80000000] = Leaf{0x8000001B, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x80000001] = Leaf{0x00300F10, 0, 0x000037FF, 0xEFD3FBFF};
  fake::g_any_sub[0x80000005] = Leaf{0, 0, 0x40020140, 0x40020140};  // 64 KB L1d
  fake::g_any_sub[0x80000006] = Leaf{0, 0, 0x04006140, 0};           // 1 MB L2, no L3
  fake::g_any_sub[0x80000008] = Leaf{0x00003030, 0, 0x00002003, 0};  // NC = 3 -> 4 cores
  // Leaves 0xB and 0x8000001D are above the maximums: all zero, like the hardware.
}

// AMD family 14h "Bobcat" (Brazos E-350 / E1 / E2 APUs, Radeon HD 6310 / 7310):
// KM4FEQ and WB4TSN hit the same qdv_init 0xc0000094. 2 cores, no SMT, max
// standard leaf 6, SSSE3 but no SSE4.1, 32 KB L1d, 512 KB L2, no L3.
static void loadBobcat() {
  using fake::Leaf;
  fake::g_table.clear(); fake::g_any_sub.clear();
  fake::g_any_sub[0x0] = Leaf{0x6, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x1] = Leaf{0x00500F20, 0x00020800, 0x00802209, 0x178BFBFF};
  fake::g_any_sub[0x80000000] = Leaf{0x8000001B, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x80000001] = Leaf{0x00500F20, 0, 0x000035FF, 0x2FD3FBFF};
  fake::g_any_sub[0x80000005] = Leaf{0, 0, 0x20020140, 0x20020140};  // 32 KB L1d
  fake::g_any_sub[0x80000006] = Leaf{0, 0, 0x02008140, 0};           // 512 KB L2, no L3
  fake::g_any_sub[0x80000008] = Leaf{0x00003024, 0, 0x00001001, 0};  // NC = 1 -> 2 cores
}

// A pre-Zen AMD whose topology leaves a hypervisor zeroed out (every leaf the
// Cpu class uses for counting returns 0). Must still not divide by zero.
static void loadAmdZeroedTopology() {
  using fake::Leaf;
  fake::g_table.clear(); fake::g_any_sub.clear();
  fake::g_any_sub[0x0] = Leaf{0xD, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x1] = Leaf{0x00600F20, 0, 0x00802009, 0x178BFBFF};
  fake::g_any_sub[0x80000000] = Leaf{0x8000001E, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x80000001] = Leaf{0, 0, 0x000037FF, 0xEFD3FBFF};
  // 0xB present but zeroed; 0x8000001D reports an L1d with 0 sharing threads.
  fake::g_table[{0x8000001D, 0}] = Leaf{0x00000121, 0x01C0003F, 0x3F, 0};
}

// A modern AMD (Zen 3 style): leaf 0xB and 0x8000001D both present.
static void loadZen() {
  using fake::Leaf;
  fake::g_table.clear(); fake::g_any_sub.clear();
  fake::g_any_sub[0x0] = Leaf{0x10, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x1] = Leaf{0x00A20F10, 0x00100800, 0x7ED8320B, 0x178BFBFF};
  fake::g_table[{0xB, 0}] = Leaf{1, 2, 0x100, 0};   // SMT level: 2 threads
  fake::g_table[{0xB, 1}] = Leaf{4, 16, 0x201, 0};  // core level: 16 logical
  fake::g_any_sub[0x80000000] = Leaf{0x80000023, kAuth, kCAMD, kEnti};
  fake::g_any_sub[0x80000001] = Leaf{0, 0, 0x75C237FF, 0x2FD3FBFF};
  fake::g_any_sub[0x80000008] = Leaf{0x3030, 0, 0x0000400F, 0};
  fake::g_any_sub[0x8000001E] = Leaf{0, 0x00000100, 0, 0};
  fake::g_table[{0x8000001D, 0}] = Leaf{0x00004121, 0x01C0003F, 0x3F, 0};    // L1d, 2 share
  fake::g_table[{0x8000001D, 1}] = Leaf{0x00004122, 0x01C0003F, 0x3F, 0};    // L1i
  fake::g_table[{0x8000001D, 2}] = Leaf{0x00004143, 0x01C0003F, 0x3FF, 2};   // L2, 2 share
  fake::g_table[{0x8000001D, 3}] = Leaf{0x0003C163, 0x03C0003F, 0x7FFF, 1};  // L3, 16 share
}

// An Intel CPU without leaf 0xB (Core 2 era): the Intel path must keep working.
static void loadIntelNoLeafB() {
  using fake::Leaf;
  fake::g_table.clear(); fake::g_any_sub.clear();
  fake::g_any_sub[0x0] = Leaf{0xA, kGenu, kNtel, kIneI};
  fake::g_any_sub[0x1] = Leaf{0x0001067A, 0x00040800, 0x0008E3FD, 0xBFEBFBFF};
  fake::g_table[{0x4, 0}] = Leaf{0x0C000121, 0x01C0003F, 0x3F, 0};   // L1d
  fake::g_table[{0x4, 1}] = Leaf{0x0C000122, 0x01C0003F, 0x3F, 0};   // L1i
  fake::g_table[{0x4, 2}] = Leaf{0x0C004143, 0x05C0003F, 0xFFF, 1};  // L2, 2 share
  fake::g_any_sub[0x80000000] = Leaf{0x80000008, 0, 0, 0};
  fake::g_any_sub[0x80000001] = Leaf{0, 0, 0x1, 0x20100800};
}

struct Result {
  DWORD crash = 0;
  bool amd = false, intel = false, sse42 = false;
  uint32_t levels = 0, l1 = 0;
};

static Result construct() {
  Result r;
  __try {
    Xbyak::util::Cpu cpu;
    r.amd = cpu.has(Xbyak::util::Cpu::tAMD);
    r.intel = cpu.has(Xbyak::util::Cpu::tINTEL);
    r.sse42 = cpu.has(Xbyak::util::Cpu::tSSE42);
    r.levels = cpu.getDataCacheLevels();
    r.l1 = r.levels > 0 ? cpu.getDataCacheSize(0) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    r.crash = GetExceptionCode();
  }
  return r;
}

static int g_fail = 0;
static void expect(bool ok, const char* profile, const char* what) {
  std::printf("  %s %s: %s\n", ok ? "ok  " : "FAIL", profile, what);
  if (!ok) g_fail++;
}

static void run(const char* name, void (*load)(), bool wantAmd, uint32_t wantL1) {
  load();
  Result r = construct();
  if (r.crash) {
    std::printf("CRASH code=0x%08lx profile=%s\n", (unsigned long)r.crash, name);
    g_fail++;
    return;
  }
  std::printf("%s: amd=%d intel=%d sse42=%d cacheLevels=%u l1d=%u\n", name, r.amd,
              r.intel, r.sse42, r.levels, r.l1);
  expect(r.amd == wantAmd, name, "vendor detected");
  if (wantL1) expect(r.l1 == wantL1, name, "L1 data cache size");
}

int main() {
  run("llano_a6_3650", loadLlano, true, 64 * 1024);
  run("bobcat_e350", loadBobcat, true, 32 * 1024);
  run("amd_zeroed_topology", loadAmdZeroedTopology, true, 0);
  run("zen", loadZen, true, 32 * 1024);
  run("intel_no_leaf_b", loadIntelNoLeafB, false, 32 * 1024);
  std::printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
