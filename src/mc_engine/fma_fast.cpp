// Fused multiply-adds on a CPU without FMA3.
//
// The recompiler turns the console's scalar fused multiply-adds (fmadd, fmsub, fnmadd,
// fnmsub and their single-precision forms; about 9,100 sites in the generated code) into
// std::fma(double, double, double). The SDK compiles for x86-64 with -msse4.1 only, so each
// one is a call to the C runtime's fma (a thunk into ucrtbase.dll). The runtime runs one
// vfmadd213sd when the CPU has FMA3 and a long software routine when it has not (Intel
// before Haswell, AMD before Piledriver): 157 to 165 cycles a call on a Ryzen 7 9800X3D
// with the runtime's FMA3 path switched off, against 6 to 13 with it. The audio mixer alone
// makes tens of thousands of these calls a round.
//
// Most of them take operands that are floats held in doubles (a single-precision
// instruction's inputs are single-precision values). The product of two floats has at most
// 48 significant bits, so it is exact in a double, and the fused result is then just one
// rounding of (a * b) + c: a double multiply and a double add give the same bits as the
// fused instruction, in every rounding mode. This file defines the fma symbol the generated
// code links against, ahead of the runtime's import. When the CPU has no FMA3 (the runtime's
// own flag says so), operands that are floats take the multiply and add; everything else,
// and everything on a CPU with FMA3, is passed on to the runtime's fma as before.
// fma_float_fast = false passes everything on. Takes effect at the next start.

#if defined(_WIN32) && defined(__x86_64__)
#include <rex/cvar.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(fma_float_fast, true, "MCLA/Performance",
    "On a CPU without FMA3 (Intel before Haswell, AMD before Piledriver), fused multiply-adds "
    "whose operands are single-precision values are computed with a double multiply and add, "
    "which gives the same bits, instead of the C runtime's software fma (157 to 165 cycles a "
    "call). Nothing changes on a CPU with FMA3. Off passes every call to the runtime. Takes "
    "effect at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using FmaFn = double (*)(double, double, double);

constexpr int kUnset = 0;
constexpr int kRuntime = 1;  // the runtime's fma: CPUs with FMA3, and the off switch
constexpr int kExact = 2;    // float-valued operands without a call, the rest to the runtime

std::atomic<int> g_mode{kUnset};
std::atomic<FmaFn> g_runtime{nullptr};

inline bool Finite(double x) {
    uint64_t u;
    std::memcpy(&u, &x, sizeof(u));
    return ((u >> 52) & 0x7FF) != 0x7FF;
}

// x is exactly a single-precision value (and finite). A float denormal held in a double is
// a normal double.
inline bool FloatValued(double x) {
    return Finite(x) && static_cast<double>(static_cast<float>(x)) == x;
}

// a * b + c, for a and b that are floats: the product is exact, so this is the one rounding of the
// fused operation. _mm_mul_sd and _mm_add_sd, so the compiler cannot contract or reassociate them.
inline double MulAdd(double a, double b, double c) {
    const __m128d p = _mm_mul_sd(_mm_set_sd(a), _mm_set_sd(b));
    return _mm_cvtsd_f64(_mm_add_sd(p, _mm_set_sd(c)));
}

int InitMode() {
    if (!g_runtime.load(std::memory_order_relaxed)) {
        HMODULE m = GetModuleHandleW(L"ucrtbase.dll");
        if (!m) m = LoadLibraryW(L"ucrtbase.dll");
        FmaFn fn = m ? reinterpret_cast<FmaFn>(GetProcAddress(m, "fma")) : nullptr;
        if (!fn) __builtin_trap();  // the runtime has no fma to fall back to
        g_runtime.store(fn, std::memory_order_relaxed);
    }
    const bool fma3 = _get_FMA3_enable() != 0;
    int mode = (!fma3 && REXCVAR_GET(fma_float_fast)) ? kExact : kRuntime;
    // Set before anything is logged, in case logging reaches fma itself; only the thread
    // that sets it says so.
    int expected = kUnset;
    if (!g_mode.compare_exchange_strong(expected, mode, std::memory_order_acq_rel)) return expected;
    LARECOMP_APP_INFO("[FMA] the C runtime's fma {}; {}",
                      fma3 ? "uses FMA3 (this CPU has it)" : "runs its software routine (no FMA3)",
                      mode == kExact ? "single-precision operands skip it (fma_float_fast)"
                                     : (fma3 ? "nothing to change" : "fma_float_fast is off"));
    return mode;
}

}  // namespace

// The symbol the generated code's std::fma resolves to: a definition in the executable wins
// over the import library's thunk. Under another C++ name with an assembler label, so the
// runtime headers' declaration of fma cannot clash with it.
extern "C" double larecomp_fma(double a, double b, double c) __asm__("fma");
extern "C" double larecomp_fma(double a, double b, double c) {
    int mode = g_mode.load(std::memory_order_acquire);
    if (mode == kUnset) mode = InitMode();
    if (mode == kExact && FloatValued(a) && FloatValued(b) && Finite(c)) return MulAdd(a, b, c);
    return g_runtime.load(std::memory_order_relaxed)(a, b, c);
}
#endif  // _WIN32 && __x86_64__
