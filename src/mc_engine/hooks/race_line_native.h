#pragma once
//
// Host versions of the Race Line's two hottest functions (hooks/race_line.cpp, perf\RACE-LINE.md).
//
// What the Race Line is (read from the recompiled code; INFERENCE for the names): the AI racers' driving
// line. Each AI vehicle's brain (sub_822BE670, "the AI brain") keeps a planner (the object at +300) with a
// ring of 64 control points and, once per frame, queues the job "UpdateRaceLine" (sub_8216C7E0 -> pool
// "[RAGE] Race Line Task", two threads) whose body sub_82171078 improves the line: up to 100 rounds, each
// moving one control point sideways by +-step and keeping the move when the predicted time of the
// whole path gets shorter. The time of one trial path is computed by a fixed chain over the path's samples
// (at most 66 samples of 56 bytes at path + 1328; count at +5024, first at +5028):
//     8216F900 (samples from the control points), 8216DD38, 8216E408 (headings and curvature),
//     8216CBC0 (backward speed pass: what a sample can be braked to from the next one),
//     8216CDD0 (forward pass: engine, drag and grip along the samples; adds up the time at +5032).
// At 143 FPS two jobs of about 2.5 ms run every frame (perf\RACE-LINE.md has the profile).
//
// This file holds a native version of the last two. They are scalar single-precision code with long
// dependency chains (two reciprocal square roots per sample), run as recompiled code: every
// guest register is a field of the context in memory, every guest load swaps its bytes, and on the
// baseline build every fused multiply-add is a call through two pointers (hooks/fma_fast.cpp). Here the
// registers are locals, the fused operations are the CPU's, and the arithmetic is the same operation by
// operation, so the results are bit for bit the recompiled function's:
//  - single-precision instructions are done in double and rounded to float once (S()), as the
//    recompiler does; fmadds and friends are a double fma rounded to float (not a float fma);
//  - frsqrte / fres are the exact 1/sqrt and 1/x rounded to float (the recompiler's), then the same
//    Newton step in double;
//  - memory is read and written in the same order as the original (the speed pass feeds itself
//    through the samples in memory, the forward pass through the totals at +5032 / +5036 / +5356).
// The fused operations need FMA3 (every x86 CPU since 2013): callers check Fma3Available() and
// leave the call to the recompiled function otherwise. Nothing here knows the rest of the program
// (the standard library and the x86 intrinsics only), so race_line_test.cpp compiles it alone.
//
// Both functions take the guest memory base and guest addresses (r3 = the planner's tuning block, r4 = the
// vehicle's tuning block, r5 = the path) and read only: the tuning blocks, the path, the constants in the
// image (kConstAddr below) and the vehicle table at r4 + 4 * (index + 19 / 20). They decline (return
// false) when a block is not plain guest RAM; the caller then runs the recompiled function.
//
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define RL_HAS_X86 1
#else
#define RL_HAS_X86 0
#endif

#if RL_HAS_X86
#define RL_INL __attribute__((always_inline)) inline                         // plain SSE2 helper
#define RL_FN __attribute__((target("fma"), always_inline)) inline           // uses the fused instruction
#define RL_ENTRY __attribute__((target("fma"), noinline))                    // a pass: the caller checks Fma3Available() first
#endif

namespace race_line {

// ------------------------------------------------------------------------------------------ guest memory

inline constexpr uint32_t kMmioLow = 0x7F000000u;       // the GPU and APU register blocks sit at 0x7FC80000 / 0x7FEA0000
inline constexpr uint32_t kMmioHigh = 0x80000000u;
inline constexpr uint32_t kPhysicalWindow = 0xE0000000u;  // the recompiled code reaches the physical mirror from here through an offset

// A block the natives may touch: plain guest RAM, below the register blocks or between them and the physical
// window, and no wrap of the 32-bit address. Anything else goes to the recompiled function.
inline bool PlainBlock(uint32_t addr, uint32_t len) {
    if (addr < 0x10000u || len == 0) return false;
    const uint64_t end = static_cast<uint64_t>(addr) + len;
    return end <= kMmioLow || (addr >= kMmioHigh && end <= kPhysicalWindow);
}

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
}
inline double LdF(const uint8_t* base, uint32_t a) {  // lfs: a float widened to double
    const uint32_t u = Ld32(base, a);
    float f;
    std::memcpy(&f, &u, 4);
    return static_cast<double>(f);
}
inline double LdD(const uint8_t* base, uint32_t a) {  // lfd
    uint64_t v;
    std::memcpy(&v, base + a, 8);
    v = __builtin_bswap64(v);
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}
inline void StF(uint8_t* base, uint32_t a, double d) {  // stfs: a double rounded to float
    const float f = static_cast<float>(d);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    St32(base, a, u);
}

// The scalar constants of the two functions, at fixed addresses of the game's image (read-only data).
inline constexpr uint32_t kConstAddr[] = {0x82004288u,   // 0: 0.1f   (floor of a square root's argument)
                                          0x82000ED4u,   // 1: 0.0f
                                          0x82040E94u,   // 2: 2.0f
                                          0x82001D14u,   // 3: 1.0f
                                          0x82008554u,   // 4: 126.1f (the top of the table index)
                                          0x820022B0u,   // 5: -8388608.0f (-2^23)
                                          0x820022ACu,   // 6: +8388608.0f
                                          0x82003774u};  // 7: 1e-6f
inline constexpr uint32_t kConstDoubleAddr[] = {0x82007DE0u,   // 0: 0.5  (the Newton step)
                                                0x82005438u};  // 1: 1.0
struct Constants {
    double f[8];
    double d[2];
    static Constants Load(const uint8_t* base) {
        Constants c;
        for (int i = 0; i < 8; ++i) c.f[i] = LdF(base, kConstAddr[i]);
        for (int i = 0; i < 2; ++i) c.d[i] = LdD(base, kConstDoubleAddr[i]);
        return c;
    }
};

// The offsets the two functions use.
inline constexpr uint32_t kSampleBase = 1328;   // first sample, 56 bytes each
inline constexpr uint32_t kSampleSize = 56;
inline constexpr uint32_t kPathEnd = 5024;      // one past the last sample
inline constexpr uint32_t kPathBegin = 5028;    // first sample
inline constexpr uint32_t kPathTime = 5032;     // total predicted time
inline constexpr uint32_t kPathDist = 5036;     // total distance
inline constexpr uint32_t kPathWork = 5356;     // samples the forward pass has been through (the job's budget)

#if RL_HAS_X86
// FMA3 and the operating system's AVX state: the fused instructions below need both.
inline bool Fma3Available() {
    static const bool ok = [] {
#if defined(_MSC_VER) && !defined(__clang__)
        int r[4];
        __cpuid(r, 1);
        const unsigned ecx = static_cast<unsigned>(r[2]);
#else
        unsigned eax = 1, ebx = 0, ecx = 0, edx = 0;
        __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
#endif
        const bool fma = (ecx >> 12) & 1u, osxsave = (ecx >> 27) & 1u, avx = (ecx >> 28) & 1u;
        if (!fma || !osxsave || !avx) return false;
        unsigned xlo, xhi;
        __asm__ __volatile__("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
        (void)xhi;
        return (xlo & 6u) == 6u;
    }();
    return ok;
}

// ------------------------------------------------------------------------------------------ arithmetic
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// frsp: a double rounded to a float, kept as a double. The single-precision instructions of the console
// are, in the recompiled code, a double operation followed by this.
RL_INL double S(double x) { return static_cast<double>(static_cast<float>(x)); }
// fused a * b + c in double, one rounding (std::fma of the recompiler)
RL_FN double Fma(double a, double b, double c) { return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c))); }
RL_INL double Sqrt(double x) { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_set_sd(x), _mm_set_sd(x))); }
// fsel: a >= 0 ? b : c (a NaN selects c)
RL_INL double Sel(double a, double b, double c) { return a >= 0.0 ? b : c; }
// frsqrte as the recompiler emulates it: the exact 1/sqrt, rounded to float
RL_INL double Rsqrte(double x) { return S(1.0 / Sqrt(x)); }
// fctiwz: the truncated, saturated 32-bit integer of a double
RL_INL int32_t Ctiwz(double x) {
    if (std::isnan(x)) return INT32_MIN;
    if (x >= 2147483647.0) return INT32_MAX;
    return _mm_cvttsd_si32(_mm_set_sd(x));
}
// the reciprocal square root with its Newton step (frsqrte; fmul; fmul; fnmsub; fmadd; fmul; frsp),
// with half = 0.5 and one = 1.0 (kConstDoubleAddr)
RL_FN double SqrtOfNewton(double x, double half, double one) {
    const double y = Rsqrte(x);
    const double yy = y * y;
    const double yh = y * half;
    const double t = -Fma(yy, x, -one);
    const double u = Fma(t, yh, y);
    return S(u * x);
}
#endif  // RL_HAS_X86

// ------------------------------------------------------------------------------------------ 8216CBC0
#if RL_HAS_X86
// Backward speed pass (rex_sub_8216CBC0). r3 = planner tuning block, r4 = vehicle tuning block, r5 = path.
// Returns f1: the sum of the samples' +36 fields divided by (end - 1), as floats, when the path has more than
// one sample, else 0.0f. Writes the samples' +4 fields (limited by their +8 field and by the planner's caps) and path + 5036.
RL_ENTRY inline double BackwardPass(uint8_t* base, const Constants& k, uint32_t r3, uint32_t r4, uint32_t r5) {
    const double f0 = k.f[0];     // 0.1f
    const double f1z = k.f[1];    // 0.0f
    const double two = k.f[2];    // 2.0f  (f9 of the original)
    const double one_f = k.f[3];  // 1.0f  (f13 in the loop)
    const double half = k.d[0];   // f11
    const double one_d = k.d[1];  // f12

    double f13 = LdF(base, r3 + 25264);
    const uint32_t begin0 = Ld32(base, r5 + kPathBegin);
    double f12 = S(f0 - f13);
    const double f11 = Sel(f12, f0, f13);
    StF(base, begin0 * kSampleSize + r5 + 1328, f11);
    const uint32_t end = Ld32(base, r5 + kPathEnd);
    StF(base, r5 + kPathDist, f1z);
    const uint32_t begin1 = Ld32(base, r5 + kPathBegin);
    if (!(static_cast<int32_t>(end - begin1) > 1)) return f1z;

    // the last sample's +4 field: limited by its +8 field (when +4 is not negative), then by the planner's cap
    const uint32_t last = end * kSampleSize + r5;  // 1276(last) is the +4 field of sample end - 1
    f13 = LdF(base, last + 1276);
    f12 = LdF(base, last + 1280);
    double f10 = f1z;
    int32_t i = static_cast<int32_t>(end) - 1;
    {
        const double d = S(f12 - f13);
        const double f9 = Sel(d, f13, f12);
        const double f8 = Sel(f13, f9, f12);
        StF(base, last + 1276, f8);
        const double cap = LdF(base, r3 + 25268);
        const double f6 = S(cap - f8);
        const double f5 = Sel(f6, f8, cap);
        const double f4 = Sel(cap, f5, f8);
        StF(base, last + 1276, f4);
    }
    if (static_cast<int32_t>(Ld32(base, r5 + kPathBegin)) < i) {
        uint32_t p = static_cast<uint32_t>(i) * kSampleSize + r5 + 1360;  // sample i + 32
        int32_t n = i;
        do {
            double f8 = LdF(base, p - 16);
            --n;
            double f7 = LdF(base, r3 + 25312);
            double f6 = S(f8 * f7);
            const double f5 = LdF(base, p - 28);
            double f4 = LdF(base, p - 12);
            const double f3 = S(f5 * f5);
            double f2 = S(f4 * f5);
            f7 = LdF(base, p - 4);
            f8 = LdF(base, p + 0);
            f4 = LdF(base, r3 + 25396);
            double f31 = LdF(base, p - 8);
            double f30 = LdF(base, r4 + 16);
            double f29 = LdF(base, r3 + 25400);
            f31 = S(Fma(f30, f29, -f31));
            f30 = LdF(base, p + 4);
            f29 = LdF(base, p - 80);
            f6 = S(f6 * f5);
            const double f28 = LdF(base, p - 84);
            f2 = S(Fma(f2, f5, f7));
            f7 = S(-Fma(f6, f5, -one_f));
            f6 = S(f2 * f2);
            double g5 = Sel(f7, f7, f1z);
            f2 = S(g5 - one_f);
            f7 = Sel(f2, one_f, g5);
            g5 = S(f7 * f8);
            f4 = S(g5 * f4);
            f2 = S(Fma(f4, f4, -f6));
            double f8b = S(f0 - f2);
            f7 = Sel(f8b, f0, f2);
            f6 = SqrtOfNewton(f7, half, one_d);
            double f5b = S(f31 - f6);
            f4 = Sel(f5b, f6, f31);
            f2 = S(f4 * f30);
            f8b = S(Fma(f2, two, f3));
            f7 = S(f0 - f8b);
            f6 = Sel(f7, f0, f8b);
            f6 = SqrtOfNewton(f6, half, one_d);
            f5b = S(f29 - f6);
            f4 = Sel(f5b, f6, f29);
            const double f3b = S(f4 - f28);
            f2 = Sel(f3b, f28, f4);
            f8b = Sel(f28, f2, f4);
            StF(base, p - 84, f8b);
            f7 = LdF(base, r3 + 25268);
            f6 = S(f7 - f8b);
            f5b = Sel(f6, f8b, f7);
            f4 = Sel(f7, f5b, f8b);
            StF(base, p - 84, f4);
            const uint32_t b = Ld32(base, r5 + kPathBegin);
            const double f3c = LdF(base, p + 4);
            f10 = S(f10 + f3c);
            p -= kSampleSize;
            if (!(n > static_cast<int32_t>(b))) break;
        } while (true);
    }
    const double f12n = S(static_cast<double>(static_cast<int32_t>(i)));
    return S(f10 / f12n);
}

// ------------------------------------------------------------------------------------------ 8216CDD0
// Forward pass (rex_sub_8216CDD0). r1 = the guest stack pointer of the caller (the function spills its
// argument to r1 + 44), r3 / r4 / r5 as above, f1 = the argument (the backward pass's result).
// Writes each sample's +0 field, the path's totals at +5032 / +5036 and the work counter at +5356. Nothing is returned.
// The variables keep the names of the console registers they stand for, so the code can be read against the
// recompiled function; the comments say what each stage computes where that is clear.
RL_ENTRY inline void ForwardPass(uint8_t* base, const Constants& k, uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, double f1arg) {
    const double half = k.d[0];   // f4
    const double one_d = k.d[1];  // f5
    const double f12 = k.f[3];    // 1.0f
    const double f13 = k.f[1];    // 0.0f
    const double f9 = k.f[4];     // 126.1f
    const double kNeg = k.f[5];   // -2^23   (stack -192 of the original)
    const double kPos = k.f[6];   // +2^23   (stack -188)
    const double kTwo = k.f[2];   // 2.0f    (stack -200 inside the loop)
    const double kTiny = k.f[7];  // 1e-6f   (after the loop)

    StF(base, r1 + 44, f1arg);  // the original spills its argument to the caller's frame
    const uint32_t end = Ld32(base, r5 + kPathEnd);
    const uint32_t begin = Ld32(base, r5 + kPathBegin);
    if (!(static_cast<int32_t>(end - begin) > 1)) return;

    double f0 = LdF(base, r4 + 44);
    double f5 = LdF(base, r4 + 8);
    double f11 = LdF(base, r3 + 25288);
    double f10 = LdF(base, r3 + 25304);
    const uint32_t first = begin * kSampleSize + r5;
    double f4 = LdF(base, first + 1328);
    double f3 = S(f4 * f0);
    double f2 = S(f12 / f5);
    const double f14c = S(f2);  // 1 / the vehicle's +8 field, as the float the original spills and reloads (-208)
    double f6 = f13;
    double f8 = f13;
    double f7;
    f5 = -f3;
    f4 = Sel(f5, f13, f3);
    f3 = Sel(f4, f4, f13);
    f2 = S(f3 - f9);
    f5 = Sel(f2, f9, f3);
    f3 = Sel(f5, kNeg, kPos);
    f2 = S(f5 - f3);
    f0 = S(f2 + f3);
    f7 = S(f5 - f0);
    f5 = S(f0 - f12);
    f3 = Sel(f7, f0, f5);
    uint32_t r9 = static_cast<uint32_t>(Ctiwz(f3));  // the table index: the floor of the first sample's value
    f7 = S(f4 - f3);                                   // and its fraction
    double f1 = f1arg;
    const double f1f = S(f1arg);  // what the original reloads from the spill slot at the end of every round

    if (static_cast<int32_t>(begin + 1) < static_cast<int32_t>(end)) {
        uint32_t p = (begin + 1) * kSampleSize + r5 + 1348;  // sample + 20
        uint32_t n = end - (begin + 1);
        f0 = k.f[0];  // 0.1f
        do {
            double f31 = LdF(base, r3 + 25312);
            double f2n = -f11;
            f3 = LdF(base, p - 4);
            double f30 = -f10;
            f3 = S(f3 * f31);
            f31 = LdF(base, p - 76);
            double f29 = LdF(base, p + 0);
            const uint32_t i19 = (r9 + 19u) << 2;
            double f28 = S(f29 * f31);
            double f23 = LdF(base, r4 + 24);
            f29 = std::fabs(f29);
            double f24 = LdF(base, r4 + 28);
            const double f14 = f14c;
            const uint32_t i20 = (r9 + 20u) << 2;
            double f20 = LdF(base, r3 + 25280);
            double f26 = LdF(base, p + 8);
            double f22 = LdF(base, r4 + 20);
            const double f21 = S(f31 * f31);
            f2n = Sel(f2n, f13, f11);
            const double f19 = LdF(base, r3 + 25296);
            f30 = Sel(f30, f13, f10);
            const double f27a = LdF(base, r3 + 25392);
            f3 = S(f3 * f31);
            double f18 = LdF(base, i19 + r4);
            double f17 = LdF(base, i20 + r4);
            f17 = S(f17 - f18);
            const double f25 = LdF(base, p + 12);
            f24 = S(Fma(f29, f24, f23));
            const double f16 = LdF(base, p + 16);
            f29 = S(Fma(f29, f14, -f12));
            double f15 = LdF(base, r3 + 25420);
            f28 = S(Fma(f28, f31, f26));
            f26 = LdF(base, p + 4);
            f23 = LdF(base, p + 32);
            const double f1d = S(f16 - f1);
            f23 = Sel(f23, f15, f13);
            f15 = LdF(base, p - 16);
            f20 = S(f20 * f2n);
            double f14b = S(f30 * f30);
            f3 = S(-Fma(f3, f31, -f12));
            f18 = S(Fma(f17, f7, f18));
            f7 = kTwo;
            f31 = S(Fma(f24, f31, f22));
            f28 = S(f28 * f28);
            f2 = S(f20 * f2n);
            f30 = S(f14b * f30);
            f3 = Sel(f3, f3, f13);
            f24 = S(f0 - f18);
            f22 = -f31;
            f2 = S(Fma(f30, f19, f2));
            f30 = S(f3 - f12);
            f24 = Sel(f24, f0, f18);
            f31 = Sel(f22, f13, f31);
            f3 = Sel(f30, f12, f3);
            f2 = S(f2 + f24);
            f30 = S(f31 * f31);
            f3 = S(f3 * f27a);
            f2 = S(f2 + f26);
            f30 = S(f30 * f31);
            f3 = S(f3 * f25);
            f2 = S(-Fma(f30, f31, -f2));
            f3 = S(Fma(f3, f3, -f28));
            f31 = S(f0 - f3);
            f3 = Sel(f31, f0, f3);
            f3 = SqrtOfNewton(f3, half, one_d);
            f31 = S(f2 - f3);
            f2 = Sel(f31, f3, f2);
            f3 = S(f2 * f16);
            f2 = S(Fma(f3, f7, f21));
            f3 = S(f0 - f2);
            f2 = Sel(f3, f0, f2);
            const double f1abs = std::fabs(f1d);
            f30 = kPos;
            f31 = kNeg;
            const double f28n = -f29;
            f6 = S(f1abs + f6);
            f29 = Sel(f28n, f13, f29);
            f1 = SqrtOfNewton(f2, half, one_d);
            f3 = S(f1 - f15);
            f2 = Sel(f3, f15, f1);
            f1 = S(f2 - f23);
            f3 = S(f0 - f1);
            f2 = Sel(f3, f0, f1);
            StF(base, p - 20, f2);
            f1 = LdF(base, r4 + 44);
            const uint32_t work = Ld32(base, r5 + kPathWork);
            f3 = LdF(base, p - 76);
            const double f28d = LdF(base, r5 + kPathDist);
            const double f27 = LdF(base, r5 + kPathTime);
            f3 = S(f2 + f3);
            f2 = S(f2 * f1);
            f1 = S(f0 - f3);
            const double f26n = -f2;
            f3 = Sel(f1, f0, f3);
            f2 = Sel(f26n, f13, f2);
            f1 = S(1.0 / f3);  // fres
            f3 = Sel(f2, f2, f13);
            f1 = S(f1 * f16);
            const double f26c = S(f3 - f9);
            f7 = S(f1 * f7);
            f3 = Sel(f26c, f9, f3);
            f1 = S(f28d + f7);
            StF(base, r5 + kPathDist, f1);
            f1 = LdF(base, p + 28);
            f1 = S(Fma(f1, f7, f27));
            StF(base, r5 + kPathTime, f1);
            f31 = Sel(f3, f31, f30);
            f30 = LdF(base, p - 20);
            f11 = S(f11 - f7);
            f28 = LdF(base, r3 + 25348);
            f1 = S(Fma(f29, f28, f1));
            f29 = S(f3 - f31);
            StF(base, r5 + kPathTime, f1);
            f10 = S(f10 - f7);
            St32(base, r5 + kPathWork, work + 1u);
            f8 = S(Fma(f7, f30, f8));
            p += kSampleSize;
            f7 = S(f29 + f31);
            f3 = S(f3 - f7);
            f1 = S(f7 - f12);
            f7 = Sel(f3, f7, f1);
            r9 = static_cast<uint32_t>(Ctiwz(f7));
            f1 = f1f;
            f7 = S(f2 - f7);
        } while (--n != 0);
    }
    // after the loop: the average step, the mean speed term and the totals
    f3 = LdF(base, r5 + kPathDist);
    f0 = kTiny;
    double f11b = S(f0 - f3);
    double f10b = Sel(f11b, f0, f3);
    f3 = S(f8 / f10b);
    if (f3 > f0) {
        const double f11c = LdF(base, r3 + 25404);
        const double f10c = S(f11c / f3);
        const double f9c = LdF(base, r5 + kPathTime);
        const double f5c = S(static_cast<double>(static_cast<int32_t>(end - 1u)));
        const double f4c = S(f10c * f5c);
        StF(base, r5 + kPathTime, S(Fma(f4c, f1, f9c)));
    }
    {
        const double f13c = LdF(base, r3 + 25340);
        const double f11c = S(f13c * f6);
        const double f10c = LdF(base, r5 + kPathTime);
        const double f8c = S(static_cast<double>(static_cast<int32_t>(end - 2u)));
        const double f7c = S(f8c * f1);
        const double f5c = S(f0 - f7c);
        const double f4c = Sel(f5c, f0, f7c);
        const double f3c = S(f11c / f4c);
        const double f2c = S(f3c + f12);
        StF(base, r5 + kPathTime, S(f2c * f10c));
    }
}
#endif  // RL_HAS_X86

}  // namespace race_line
