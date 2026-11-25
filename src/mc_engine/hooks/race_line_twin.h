#pragma once
//
// Host twins of the Race Line's two path functions, rex_sub_8216E408 (headings, curvature and speed limits of the path's samples) and rex_sub_8216F900 (the
// samples from the control points), with the helpers they call. perf\RACELINE-TWIN.md has the reading of the code, the design and what the first run has to check.
//
// The build writes race_line_twin_gen.h from the player's recompiled sources. It holds one line per instruction over a LOCAL register file.
// This header defines the memory and vector operations, the hook entry points and the context comparison:
//  - single-precision instructions are a double operation rounded to float once (race_line::S); fused multiply-adds are the CPU's instruction (race_line::Fma:
//    needs FMA3, callers check race_line::Fma3Available(), the baseline build's recompiled code calls std::fma);
//  - the vector instructions are the same simde operations, the dot products the SDK's own (rex::ppc::Vmsum3fp128 / Vmsum4fp128), the flush-to-zero mode is
//    switched where the recompiled code switches it (PPCFPSCRRegister, on a local copy of the thread's FPSCR that Load / Store carry in and out);
//  - memory is big-endian guest memory at base + address, read and written in the recompiled order but with plain accesses (the recompiled code's are volatile): fine
//    while the job's own threads are the only writers of the planner blocks (the brain joins the job before it touches the planner);
//  - the registers are members of a struct that never leaves the entry point (every function is inlined into it), so the compiler keeps them in host registers.
// Callers must pass plain guest RAM (race_line::PlainBlock on the blocks, the stack below the physical window): the twins do not apply the physical-window offset
// that the recompiled code's accesses carry for addresses at or above 0xE0000000.
//
// x86 only (the generated header needs FMA3 and the SDK's simde); elsewhere this header is empty and the recompiled functions run.
//
#include "race_line_native.h"

#if RL_HAS_X86 && !defined(RL_NO_TWIN) && __has_include(<race_line_twin_gen.h>)  // An absent build header leaves the recompiled functions active.
#define RL_HAS_TWIN 1

#include <rex/ppc/context.h>
#include <rex/ppc/vmx.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#define RLT_INL __attribute__((always_inline)) inline
#define RLT_FN __attribute__((target("fma"), always_inline)) inline
#define RLT_ENTRY __attribute__((target("fma"), noinline))

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace race_line::twin {

// Deliberate faults for the unit test (race_line_test.cpp compiled with -DRL_TWIN_MUTANTS): the test switches one on at a time and must see the results change.
#ifdef RL_TWIN_MUTANTS
inline int g_mutant = 0;
inline constexpr int kMutantCount = 12;
inline const char* MutantName(int m) {
    static const char* const names[kMutantCount] = {"vmulfp adds",           "vsubfp operands swapped",       "vmaxfp takes the minimum",     "vcmpgtfp is vcmpgefp",
                                                    "vnmsubfp keeps the sign", "vsel operands swapped",       "vmsum3fp sums four lanes",     "vmrghw operands swapped",
                                                    "vslw shifts right",       "stfs truncates (one ulp low)", "vspltisw sets one more", "fused multiply-add not fused"};
    return names[m - 1];
}
#define RLT_MUT(n) (g_mutant == (n))
#else
#define RLT_MUT(n) false
#endif

// The scalar helpers of race_line_native.h under the names the generated lines use (so that a mutant can reach them).
RLT_FN double Fma(double a, double b, double c) { return RLT_MUT(12) ? a * b + c : race_line::Fma(a, b, c); }
RLT_INL double S(double x) { return race_line::S(x); }
RLT_INL double Sqrt(double x) { return race_line::Sqrt(x); }

// ------------------------------------------------------------------------------------------ guest memory (plain: see the header)
RLT_INL uint8_t rl_ld8(const uint8_t* base, uint32_t a) { return base[a]; }
RLT_INL uint16_t rl_ld16(const uint8_t* base, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, base + a, 2);
    return __builtin_bswap16(v);
}
RLT_INL uint32_t rl_ld32(const uint8_t* base, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
}
RLT_INL uint64_t rl_ld64(const uint8_t* base, uint32_t a) {
    uint64_t v;
    std::memcpy(&v, base + a, 8);
    return __builtin_bswap64(v);
}
RLT_INL double rl_lds(const uint8_t* base, uint32_t a) {  // lfs: a float widened to double
    const uint32_t u = rl_ld32(base, a);
    float f;
    std::memcpy(&f, &u, 4);
    return static_cast<double>(f);
}
RLT_INL void rl_st8(uint8_t* base, uint32_t a, uint8_t v) { base[a] = v; }
RLT_INL void rl_st16(uint8_t* base, uint32_t a, uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(base + a, &v, 2);
}
RLT_INL void rl_st32(uint8_t* base, uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
}
RLT_INL void rl_st64(uint8_t* base, uint32_t a, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(base + a, &v, 8);
}
RLT_INL void rl_sts(uint8_t* base, uint32_t a, double d) {  // stfs: a double rounded to float
    float f = static_cast<float>(d);
    if (RLT_MUT(10)) f = std::nextafter(f, -INFINITY);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    rl_st32(base, a, u);
}
// the generated lines use these names, with the function's own `base` (undefined again at the end of race_line_twin_gen.h)
#define LD8(x) rl_ld8(base, static_cast<uint32_t>(x))
#define LD16(x) rl_ld16(base, static_cast<uint32_t>(x))
#define LD32(x) rl_ld32(base, static_cast<uint32_t>(x))
#define LD64(x) rl_ld64(base, static_cast<uint32_t>(x))
#define LDS(x) rl_lds(base, static_cast<uint32_t>(x))
#define ST8(x, y) rl_st8(base, static_cast<uint32_t>(x), (y))
#define ST16(x, y) rl_st16(base, static_cast<uint32_t>(x), (y))
#define ST32(x, y) rl_st32(base, static_cast<uint32_t>(x), (y))
#define ST64(x, y) rl_st64(base, static_cast<uint32_t>(x), (y))
#define STS(x, y) rl_sts(base, static_cast<uint32_t>(x), (y))

// ------------------------------------------------------------------------------------------ vector instructions (the recompiler's, with simde)
// d, a, b, c are registers of the local file; ea is a guest address already rounded down to 16 bytes.
RLT_INL void LVX(const uint8_t* base, PPCVRegister& d, uint32_t ea) {
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + ea)), simde_mm_load_si128((simde__m128i*)rex::ppc::simd::kVectorMaskL)));
}
RLT_INL void STVX(uint8_t* base, uint32_t ea, const PPCVRegister& s) {
    simde_mm_store_si128((simde__m128i*)(base + ea), simde_mm_shuffle_epi8(simde_mm_load_si128((const simde__m128i*)s.u8), simde_mm_load_si128((simde__m128i*)rex::ppc::simd::kVectorMaskL)));
}
RLT_INL void VMOV(PPCVRegister& d, const PPCVRegister& a) { simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_load_si128((const simde__m128i*)a.u8)); }
RLT_INL void VAND(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_and_si128(simde_mm_load_si128((const simde__m128i*)a.u8), simde_mm_load_si128((const simde__m128i*)b.u8)));
}
RLT_INL void VOR(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_or_si128(simde_mm_load_si128((const simde__m128i*)a.u8), simde_mm_load_si128((const simde__m128i*)b.u8)));
}
RLT_INL void VANDC(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {  // a & ~b
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_andnot_si128(simde_mm_load_si128((const simde__m128i*)b.u8), simde_mm_load_si128((const simde__m128i*)a.u8)));
}
RLT_INL void VSEL(PPCVRegister& d, const PPCVRegister& a_, const PPCVRegister& b_, const PPCVRegister& c) {  // (a & ~c) | (b & c)
    const PPCVRegister& a = RLT_MUT(6) ? b_ : a_;
    const PPCVRegister& b = RLT_MUT(6) ? a_ : b_;
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((const simde__m128i*)c.u8), simde_mm_load_si128((const simde__m128i*)a.u8)),
                                                                simde_mm_and_si128(simde_mm_load_si128((const simde__m128i*)c.u8), simde_mm_load_si128((const simde__m128i*)b.u8))));
}
RLT_INL void VZERO(PPCVRegister& d) { simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_setzero_si128()); }
RLT_INL void VSPLTISW(PPCVRegister& d, int k) { simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_set1_epi32(RLT_MUT(11) ? k + 1 : k)); }
#define VSHUF32(d, a, imm) simde_mm_store_si128((simde__m128i*)(d).u32, simde_mm_shuffle_epi32(simde_mm_load_si128((const simde__m128i*)(a).u32), imm))
RLT_INL void VUNPCKHI(PPCVRegister& d, const PPCVRegister& a_, const PPCVRegister& b_) {
    const PPCVRegister& a = RLT_MUT(8) ? b_ : a_;
    const PPCVRegister& b = RLT_MUT(8) ? a_ : b_;
    simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((const simde__m128i*)a.u32), simde_mm_load_si128((const simde__m128i*)b.u32)));
}
RLT_INL void VUNPCKLO(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((const simde__m128i*)a.u32), simde_mm_load_si128((const simde__m128i*)b.u32)));
}
RLT_INL void VCMPEQUW(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_si128((simde__m128i*)d.u8, simde_mm_cmpeq_epi32(simde_mm_load_si128((const simde__m128i*)a.u32), simde_mm_load_si128((const simde__m128i*)b.u32)));
}
RLT_INL void VSLW(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {  // each word of a shifted left by the low 5 bits of b's word (simde_mm_sllv_epi32 of the recompiled code)
    for (int i = 0; i < 4; ++i) d.u32[i] = RLT_MUT(9) ? a.u32[i] >> (b.u32[i] & 0x1Fu) : a.u32[i] << (b.u32[i] & 0x1Fu);
}
RLT_INL void VMULFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    if (RLT_MUT(1)) return simde_mm_store_ps(d.f32, simde_mm_add_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
    simde_mm_store_ps(d.f32, simde_mm_mul_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VSUBFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    if (RLT_MUT(2)) return simde_mm_store_ps(d.f32, simde_mm_sub_ps(simde_mm_load_ps(b.f32), simde_mm_load_ps(a.f32)));
    simde_mm_store_ps(d.f32, simde_mm_sub_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VADDFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_ps(d.f32, simde_mm_add_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VMAXFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    if (RLT_MUT(3)) return simde_mm_store_ps(d.f32, simde_mm_min_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
    simde_mm_store_ps(d.f32, simde_mm_max_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VMINFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_ps(d.f32, simde_mm_min_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VCMPGEFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_ps(d.f32, simde_mm_cmpge_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VCMPGTFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    if (RLT_MUT(4)) return simde_mm_store_ps(d.f32, simde_mm_cmpge_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
    simde_mm_store_ps(d.f32, simde_mm_cmpgt_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VCMPEQFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_ps(d.f32, simde_mm_cmpeq_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VMADDFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b, const PPCVRegister& c) {  // a * b + c, two roundings (not fused: vmaddfp)
    simde_mm_store_ps(d.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)), simde_mm_load_ps(c.f32)));
}
RLT_INL void VNMSUBFP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b, const PPCVRegister& c) {  // -(a * b - c)
    if (RLT_MUT(5)) return simde_mm_store_ps(d.f32, simde_mm_sub_ps(simde_mm_mul_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)), simde_mm_load_ps(c.f32)));
    simde_mm_store_ps(d.f32, simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)), simde_mm_load_ps(c.f32)),
                                             simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));
}
RLT_INL void VRSQRTEFP(PPCVRegister& d, const PPCVRegister& a) {  // the exact 1 / sqrt(a) (the recompiler does not emulate the estimate)
    simde_mm_store_ps(d.f32, simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_load_ps(a.f32))));
}
RLT_INL void VREFP(PPCVRegister& d, const PPCVRegister& a) { simde_mm_store_ps(d.f32, simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_load_ps(a.f32))); }
RLT_INL void VMSUM3FP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
    simde_mm_store_ps(d.f32, RLT_MUT(7) ? rex::ppc::Vmsum4fp128(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)) : rex::ppc::Vmsum3fp128(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
}
RLT_INL void VMSUM4FP(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) { simde_mm_store_ps(d.f32, rex::ppc::Vmsum4fp128(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32))); }
// vcmp...fp. with the record bit: cr6 from the comparison's lane mask
#define CR6_MASK(v, k) c.cr6.SetFromMask(simde_mm_load_ps((v).f32), k)

// ------------------------------------------------------------------------------------------ comparison helpers (the generated FirstRegDifference)
RLT_INL bool SameF64(uint64_t a, uint64_t b) {
    if (a == b) return true;
    double x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    return std::isnan(x) && std::isnan(y);
}
RLT_INL bool SameV128(const uint32_t* a, const uint32_t* b) {  // four lanes; two NaN lanes are equal
    for (int i = 0; i < 4; ++i) {
        if (a[i] == b[i]) continue;
        float x, y;
        std::memcpy(&x, &a[i], 4);
        std::memcpy(&y, &b[i], 4);
        if (!(std::isnan(x) && std::isnan(y))) return false;
    }
    return true;
}

}  // namespace race_line::twin

#include <race_line_twin_gen.h>

#undef VSHUF32
#undef CR6_MASK

namespace race_line::twin {
// The twins as the hook calls them. Both take the thread's context and the guest memory base (r3..r6 and f1 as the recompiled function's callers set them) and leave
// the context's registers and the guest memory as the recompiled function does.
//   E408: r3 = the planner's tuning block, r4 = the vehicle's tuning block, r5 = the path, r6 = the job's scratch block.
//   F900: r3 = tuning block, r4 = path, r5 = scratch block, r6 = 0, f1 = the path's total time (read by the first call of a job).
inline void RunE408(PPCContext& ctx, uint8_t* base) { gen::e408::Run(ctx, base); }
inline void RunF900(PPCContext& ctx, uint8_t* base) { gen::f900::Run(ctx, base); }
inline const char* DiffE408(const PPCContext& a, const PPCContext& b) { return gen::e408::FirstRegDifference(a, b); }
inline const char* DiffF900(const PPCContext& a, const PPCContext& b) { return gen::f900::FirstRegDifference(a, b); }
}  // namespace race_line::twin

#else
#define RL_HAS_TWIN 0
#endif
