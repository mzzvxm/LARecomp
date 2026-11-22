// Host versions of the game's two memcpys, memset, row writer and two D3D constant writers.
//
// See host_helpers.h for what each function is and what it declines. This file is only the glue: each
// override reads its arguments from the guest registers, asks the native function to do the work, and
// runs the recompiled function (__imp__<name>) when the native one declines or the helper is switched
// off. The hot list that picked them is perf\CPU-HOT-GUEST.md (guest self time, race window of the
// AB011 runs).
//
// What the overrides leave in the registers: r3 and every non-volatile register as the recompiled
// function leaves them (r3 is the destination for memcpy, the device for the others; none of the
// natives touches a register of the context). The volatile registers r4..r12, ctr, cr0, cr6, xer.ca,
// lr and the vector scratch registers keep the caller's values instead of the function's scratch
// values, and the stack below r1 is not written: all dead after a call by the ABI.
//
// The A/B switch is host_helpers_mask (debug only, read at start): bit 0 memcpy, 1 memset, 2 the
// row writer, 3 the shader-constant copy, 4 the constant-bank flush, 5 the C runtime's memcpy.
// 0 = all recompiled, 63 = all native (the default).

#if !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include <atomic>
#include <cstdint>

#include "host_helpers.h"
#include "larecomp_log.h"
#include "settings/larecomp_settings.h"

REX_EXTERN(__imp__rex_sub_8244D5C0);              // memcpy
REX_EXTERN(__imp__rex_sub_823D9890);              // memset
REX_EXTERN(__imp__rex_sub_8218A6E0);              // row writer
REX_EXTERN(__imp__rex_sub_82417678);              // shader constant copy
REX_EXTERN(__imp__D3DDevice_FlushConstantBank);   // constant bank flush
REX_EXTERN(__imp__rex_sub_823DA950);              // the C runtime's memcpy

namespace {

enum Helper : uint32_t { kMemcpy = 0, kMemset, kRowWrite, kShaderConstF, kFlushBank, kCrtMemcpy, kHelperCount };

const char* const kNames[kHelperCount] = {"memcpy", "memset", "row writer", "shader constant copy", "constant bank flush", "C runtime memcpy"};

std::atomic<uint32_t> g_declined[kHelperCount];

// The enabled helpers, read once when the first override runs; logs one line.
uint32_t Mask() {
    static const uint32_t mask = [] {
        const uint32_t m = static_cast<uint32_t>(larecomp::settings::host_helpers_mask.Get()) & ((1u << kHelperCount) - 1u);
        LARECOMP_APP_INFO("[HostHelpers] native versions of the game's helpers: mask {:#x} ({})", m,
                          m == 0 ? "all recompiled" : "bits set = native: 1 memcpy, 2 memset, 4 row writer, 8 shader constant copy, 16 constant bank flush, 32 C runtime memcpy");
        return m;
    }();
    return mask;
}

inline bool On(Helper h) { return (Mask() >> h) & 1u; }

// A decline is rare (overlap, a window address, a ring wrap): the first few of each kind are logged.
void Declined(Helper h) {
    const uint32_t n = g_declined[h].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 100 || n == 10000 || n == 1000000) {
        LARECOMP_APP_INFO("[HostHelpers] {}: {} call(s) so far went to the recompiled code", kNames[h], n);
    }
}

}  // namespace

extern "C" REX_FUNC(rex_sub_8244D5C0) {
    // r3 = dst, r4 = src, r5 = n; returns dst in r3 (which this leaves as it is).
    if (On(kMemcpy) && (ctx.r5.u64 >> 32) == 0) {
        if (host_helpers::Memcpy(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) return;
        Declined(kMemcpy);
    }
    __imp__rex_sub_8244D5C0(ctx, base);
}

extern "C" REX_FUNC(rex_sub_823DA950) {
    // The C runtime's memcpy: r3 = dst, r4 = src, r5 = n; returns dst in r3 (left as it is).
    if (On(kCrtMemcpy) && (ctx.r5.u64 >> 32) == 0) {
        if (host_helpers::CrtMemcpy(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) return;
        Declined(kCrtMemcpy);
    }
    __imp__rex_sub_823DA950(ctx, base);
}

extern "C" REX_FUNC(rex_sub_823D9890) {
    // r3 = dst, r4 = value, r5 = n; r3 is preserved.
    if (On(kMemset) && (ctx.r5.u64 >> 32) == 0) {
        if (host_helpers::Memset(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) return;
        Declined(kMemset);
    }
    __imp__rex_sub_823D9890(ctx, base);
}

extern "C" REX_FUNC(rex_sub_8218A6E0) {
    // r3 object, r4 row table pointer, r5 slot + 1, r6 source, r7 bytes per row, r8 rows.
    if (On(kRowWrite)) {
        if (host_helpers::RowWrite(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32)) return;
        Declined(kRowWrite);
    }
    __imp__rex_sub_8218A6E0(ctx, base);
}

extern "C" REX_FUNC(rex_sub_82417678) {
    // r3 device, r4 first register, r5 source, r6 count of float4, r7 the 64-bit dirty mask.
    if (On(kShaderConstF)) {
        if (host_helpers::ShaderConstF(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64)) return;
        Declined(kShaderConstF);
    }
    __imp__rex_sub_82417678(ctx, base);
}

extern "C" REX_FUNC(D3DDevice_FlushConstantBank) {
    // r3 device, r4 the 64-bit dirty mask, r5 first register, r6 bank base.
    if (On(kFlushBank)) {
        if (host_helpers::FlushBank(base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32)) return;
        Declined(kFlushBank);
    }
    __imp__D3DDevice_FlushConstantBank(ctx, base);
}
#endif  // !REXGLUE_HAS_XEO3_TARGET
