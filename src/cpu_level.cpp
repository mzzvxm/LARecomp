// The CPU level of this build, what the CPU reports, and the start-up check for a build made for
// x86-64-v3.
//
// CMake option LARECOMP_CPU_LEVEL=x86-64-v3 compiles the generated code and LARecomp's own sources
// with -march=x86-64-v3 (AVX2, FMA, BMI1/2, MOVBE, LZCNT, F16C). A CPU without them would stop at
// the first such instruction with an illegal-instruction crash, so a build for v3 checks the CPU
// before any of its code runs and, if a feature is missing, says so and exits.
//
// This file is built into its own object library (larecomp_cpuguard), without the level's -march
// flag, so nothing in it uses an instruction a baseline CPU lacks. It does not use the SDK, spdlog
// or the C++ runtime's heap in the check: none of them is up yet on every platform at that point.
//
// When the check runs, per platform (the v3 translation units' static initializers are the earliest
// v3 code that runs, so the check has to come before them):
//  - Windows, MSVC target: the function pointer sits in the section .CRT$XCAA. The C runtime
//    runs the .CRT$XC* table from .CRT$XCA to .CRT$XCZ in name order; C++ static initializers
//    are in .CRT$XCU (init_seg(compiler) is .CRT$XCC, init_seg(lib) .CRT$XCL), so .CRT$XCAA runs ahead of
//    all of them. Dynamic initializers of thread_local variables run after that table (the C
//    runtime calls them for the main thread once it is done, and the loader for later threads),
//    and main and WinMain after that.
//  - Windows, MinGW (and an x86-64 Mac): constructor priority 101, the lowest a program may use;
//    the default priority runs after it.
//  - Linux and other ELF: an entry in .preinit_array. The dynamic loader calls it after
//    relocation and before the initializers of any shared library and before .init_array.
// Checked in scratch builds of a test program (no game) with Clang 20 for Windows (lld-link, with
// /OPT:REF and /OPT:ICF), llvm-mingw Clang 20 and Clang 21 on Linux: the check ran first, ahead of
// static initializers (also init_seg(compiler)) and thread_local initializers, in either link order.
//
// A baseline build has no check and no section: nothing is registered.

#include "cpu_level.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
#define LARECOMP_CPUCHECK_X86 1
#include <cpuid.h>
#else
#define LARECOMP_CPUCHECK_X86 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace larecomp::cpu {

const char* BuiltLevel() {
#if defined(LARECOMP_CPU_LEVEL_V3)
    return "x86-64-v3";
#else
    return "baseline";
#endif
}

#if LARECOMP_CPUCHECK_X86

namespace {

struct Features {
    char vendor[13] = {};
    // CPUID leaf 1
    bool ssse3 = false, fma = false, cx16 = false, sse41 = false, sse42 = false, movbe = false,
         popcnt = false, avx = false, f16c = false;
    bool osxsave = false;
    // CPUID leaf 7
    bool bmi1 = false, avx2 = false, bmi2 = false;
    // CPUID leaf 0x80000001
    bool lahf = false, lzcnt = false;
    // XCR0: the OS saves and restores the SSE and AVX register state
    bool avx_state = false;
};

inline uint64_t ReadXcr0() {
    uint32_t lo = 0, hi = 0;
    // xgetbv with ecx = 0, as bytes so it assembles whatever the target features are.
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

Features Detect() {
    Features f;
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(0, 0, a, b, c, d);
    const unsigned max_leaf = a;
    std::memcpy(f.vendor + 0, &b, 4);
    std::memcpy(f.vendor + 4, &d, 4);
    std::memcpy(f.vendor + 8, &c, 4);
    f.vendor[12] = '\0';
    if (max_leaf >= 1) {
        __cpuid_count(1, 0, a, b, c, d);
        f.ssse3 = (c >> 9) & 1;
        f.fma = (c >> 12) & 1;
        f.cx16 = (c >> 13) & 1;
        f.sse41 = (c >> 19) & 1;
        f.sse42 = (c >> 20) & 1;
        f.movbe = (c >> 22) & 1;
        f.popcnt = (c >> 23) & 1;
        f.osxsave = (c >> 27) & 1;
        f.avx = (c >> 28) & 1;
        f.f16c = (c >> 29) & 1;
    }
    if (max_leaf >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        f.bmi1 = (b >> 3) & 1;
        f.avx2 = (b >> 5) & 1;
        f.bmi2 = (b >> 8) & 1;
    }
    __cpuid_count(0x80000000u, 0, a, b, c, d);
    if (a >= 0x80000001u) {
        __cpuid_count(0x80000001u, 0, a, b, c, d);
        f.lahf = (c >> 0) & 1;
        f.lzcnt = (c >> 5) & 1;
    }
    // xgetbv only exists when the OS says it uses XSAVE (OSXSAVE). Bits 1 and 2 are the SSE and
    // the AVX (YMM) state.
    if (f.osxsave) f.avx_state = (ReadXcr0() & 0x6) == 0x6;
    return f;
}

// The AVX family needs the OS to save the YMM registers as well as the CPU to have them.
inline bool Avx(const Features& f) { return f.avx && f.avx_state; }

// Appends ", name" to out (a fixed buffer: no heap, the check runs very early).
void AppendName(char* out, size_t cap, const char* name) {
    size_t n = std::strlen(out);
    const char* sep = n ? ", " : "";
    std::snprintf(out + n, cap - n, "%s%s", sep, name);
}

// What x86-64-v3 needs and this CPU lacks, as "AVX2, BMI2", or an empty string. The compiler's
// x86-64-v3 is the v2 features (SSSE3, SSE4.2, POPCNT, CMPXCHG16B, LAHF) plus these.
void MissingForV3(const Features& f, char* out, size_t cap) {
    out[0] = '\0';
    if (!f.avx) AppendName(out, cap, "AVX");
    else if (!f.avx_state) AppendName(out, cap, "AVX (not enabled by the operating system)");
    if (!f.avx2) AppendName(out, cap, "AVX2");
    if (!f.fma) AppendName(out, cap, "FMA");
    if (!f.f16c) AppendName(out, cap, "F16C");
    if (!f.bmi1) AppendName(out, cap, "BMI1");
    if (!f.bmi2) AppendName(out, cap, "BMI2");
    if (!f.movbe) AppendName(out, cap, "MOVBE");
    if (!f.lzcnt) AppendName(out, cap, "LZCNT");
    if (!f.popcnt) AppendName(out, cap, "POPCNT");
    if (!f.ssse3) AppendName(out, cap, "SSSE3");
    if (!f.sse42) AppendName(out, cap, "SSE4.2");
    if (!f.cx16) AppendName(out, cap, "CMPXCHG16B");
    if (!f.lahf) AppendName(out, cap, "LAHF/SAHF");
}

}  // namespace

std::string DescribeFeatures() {
    const Features f = Detect();
    std::string s = f.vendor[0] ? f.vendor : "unknown vendor";
    s += ";";
    auto add = [&](bool on, const char* name) {
        if (on) {
            s += ' ';
            s += name;
        }
    };
    add(f.sse41, "sse4.1");
    add(f.sse42, "sse4.2");
    add(Avx(f), "avx");
    add(f.avx2 && Avx(f), "avx2");
    add(f.fma && Avx(f), "fma");
    add(f.f16c && Avx(f), "f16c");
    add(f.bmi1, "bmi1");
    add(f.bmi2, "bmi2");
    add(f.movbe, "movbe");
    add(f.lzcnt, "lzcnt");
    add(f.popcnt, "popcnt");
    char missing[192];
    MissingForV3(f, missing, sizeof(missing));
    s += missing[0] ? "; x86-64-v3: no (lacks " : "; x86-64-v3: yes";
    if (missing[0]) {
        s += missing;
        s += ')';
    }
    return s;
}

#else  // not x86-64

std::string DescribeFeatures() { return "not an x86-64 CPU"; }

#endif

// ---- The start-up check (builds for x86-64-v3 only) ------------------------------------------

#if defined(LARECOMP_CPU_LEVEL_V3) && LARECOMP_CPUCHECK_X86

namespace {

// The exit code of a process that stopped here, so a launcher can tell it from a crash.
constexpr unsigned kExitCode = 0x4C43;  // "LC"

[[noreturn]] void Refuse(const char* missing) {
    char text[512];
    std::snprintf(text, sizeof(text),
                  "This copy of LARecomp was built for a newer processor than this one (it needs "
                  "x86-64-v3: AVX2, FMA, BMI2, MOVBE). Open the launcher and use Repair to build "
                  "it for this PC.\n\nThis processor lacks: %s.",
                  missing);
#if defined(_WIN32)
    // No logging exists yet; the debugger output is the only trace besides the message.
    OutputDebugStringA("[cpu] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    MessageBoxA(nullptr, text, "LARecomp", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    // Not ExitProcess: the process is still starting, and the DLLs that are loaded have
    // nothing to clean up.
    TerminateProcess(GetCurrentProcess(), kExitCode);
    ExitProcess(kExitCode);
#else
    std::fputs("[cpu] ", stderr);
    std::fputs(text, stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
    _exit(static_cast<int>(kExitCode & 0xFF));
#endif
}

void CheckThisCpu() {
    const Features f = Detect();
    char missing[192];
    MissingForV3(f, missing, sizeof(missing));
    if (missing[0]) Refuse(missing);
}

#if defined(_WIN32) && !defined(__MINGW32__)
// Section name sorts before .CRT$XCC, .CRT$XCL and .CRT$XCU. Read-only data like the compiler's own
// entries in .CRT$XCU.
__attribute__((section(".CRT$XCAA"), used)) void (*const g_check_entry)() = &CheckThisCpu;
#elif defined(__ELF__)
void CheckThisCpuPreinit(int, char**, char**) { CheckThisCpu(); }
__attribute__((section(".preinit_array"), used)) void (*g_check_entry)(int, char**, char**) =
    &CheckThisCpuPreinit;
#else  // MinGW, and Mach-O (an x86-64 Mac)
__attribute__((constructor(101), used)) void CheckThisCpuConstructor() { CheckThisCpu(); }
#endif

}  // namespace

#endif

}  // namespace larecomp::cpu
