#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The call shim for the game's script natives: see native_call.h for the convention. The call is
// rex::ppc::GuestToHostFunction<void>(wrapper, context), the way cutscene_gallery.cpp starts a script and the E4 test
// (test/cin-e4) called its natives.
//

#include "native_call.h"

#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "larecomp_log.h"
#include "larecomp_guest.h"

namespace mc::cinematic {

namespace {

constexpr uint32_t kGuestMallocFn = 0x82130528;  // the game's allocator: r3 = size, returns the block
constexpr uint32_t kCtxBytes = 128;              // the context, 16-byte aligned (a wrapper may store vector registers into it)
constexpr uint32_t kRetBytes = 16;
constexpr uint32_t kMaxArgs = 8;
constexpr uint32_t kVecEntries = 16;   // the ring of vectors a native may take by pointer
constexpr uint32_t kStrBytes = 512;    // ... and of strings
constexpr uint32_t kRetSentinel = 0xDEADC0DEu;

struct Block {
    uint32_t ctx = 0;
    uint32_t ret = 0;
    uint32_t args = 0;
    uint32_t vecs = 0;
    uint32_t strs = 0;
    uint32_t vec_next = 0;
    uint32_t str_next = 0;
    bool failed = false;
};
Block g_block;

uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

void Put32(uint8_t* base, uint32_t ea, uint32_t v) {
    base[ea + 0] = static_cast<uint8_t>(v >> 24);
    base[ea + 1] = static_cast<uint8_t>(v >> 16);
    base[ea + 2] = static_cast<uint8_t>(v >> 8);
    base[ea + 3] = static_cast<uint8_t>(v);
}

uint32_t Get32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = REX_RUNTIME();
    if (!rt) return nullptr;
    return rt->FindFunction(addr).value_or(nullptr);
}

bool EnsureBlock(uint8_t* base) {
    if (g_block.ctx) return true;
    if (g_block.failed) return false;
    PPCFunc* malloc_fn = GuestFn(kGuestMallocFn);
    const uint32_t total = kCtxBytes + kRetBytes + kMaxArgs * 4 + kVecEntries * 16 + kStrBytes;
    const uint32_t raw = malloc_fn ? larecomp::guest::Call<uint32_t>(malloc_fn, total + 16) : 0;
    if (!raw) {
        g_block.failed = true;
        LARECOMP_APP_WARN("[scene] native calls: the game's allocator gave no block; no native can be called");
        return false;
    }
    const uint32_t at = (raw + 15) & ~15u;
    std::memset(base + at, 0, total);
    g_block.ctx = at;
    g_block.ret = at + kCtxBytes;
    g_block.args = g_block.ret + kRetBytes;
    g_block.vecs = g_block.args + kMaxArgs * 4;
    g_block.strs = g_block.vecs + kVecEntries * 16;
    LARECOMP_APP_INFO("[scene] native calls: context block at 0x{:08X}", at);
    return true;
}

void PutF(uint8_t* base, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    Put32(base, ea, v);
}

float GetF(const uint8_t* base, uint32_t ea) {
    const uint32_t v = Get32(base, ea);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

}  // namespace

uint32_t GuestPutVec(float x, float y, float z, float w) {
    uint8_t* base = Membase();
    if (!base || !rex::kernel::ThreadState::Get() || !EnsureBlock(base)) return 0;
    const uint32_t at = g_block.vecs + (g_block.vec_next++ % kVecEntries) * 16;
    PutF(base, at, x);
    PutF(base, at + 4, y);
    PutF(base, at + 8, z);
    PutF(base, at + 12, w);
    return at;
}

uint32_t GuestPutString(const char* text) {
    uint8_t* base = Membase();
    if (!base || !text || !rex::kernel::ThreadState::Get() || !EnsureBlock(base)) return 0;
    const uint32_t need = static_cast<uint32_t>(std::strlen(text)) + 1;
    if (need > kStrBytes) return 0;
    if (g_block.str_next + need > kStrBytes) g_block.str_next = 0;  // wraps: an older string is overwritten
    const uint32_t at = g_block.strs + g_block.str_next;
    std::memcpy(base + at, text, need);
    g_block.str_next += (need + 3) & ~3u;
    return at;
}

NativeResult CallNative(uint32_t wrapper, std::initializer_list<uint32_t> args, bool flush_log) {
    NativeResult res;
    uint8_t* base = Membase();
    if (!base || args.size() > kMaxArgs) return res;
    if (!rex::kernel::ThreadState::Get()) return res;  // not a guest thread: GuestToHostFunction would do nothing
    PPCFunc* fn = GuestFn(wrapper);
    if (!fn || !EnsureBlock(base)) return res;

    std::memset(base + g_block.ctx, 0, kCtxBytes);
    std::memset(base + g_block.args, 0, kMaxArgs * 4);
    Put32(base, g_block.ret, kRetSentinel);
    Put32(base, g_block.ctx + 0, g_block.ret);
    Put32(base, g_block.ctx + 4, static_cast<uint32_t>(args.size()));
    Put32(base, g_block.ctx + 8, g_block.args);
    Put32(base, g_block.ctx + 12, 0);
    uint32_t i = 0;
    for (const uint32_t a : args) Put32(base, g_block.args + 4 * i++, a);

    if (flush_log) REX_LOGGER().Flush();  // a line written before the call is on disk if the call takes the game down
    const auto t0 = std::chrono::steady_clock::now();
    larecomp::guest::Call<void>(fn, g_block.ctx);
    const auto t1 = std::chrono::steady_clock::now();
    res.called = true;
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.ret = Get32(base, g_block.ret);
    res.ret_written = res.ret != kRetSentinel;
    res.vec_count = std::min<uint32_t>(Get32(base, g_block.ctx + 12), 4);
    for (uint32_t i = 0; i < res.vec_count; ++i) {
        for (uint32_t k = 0; k < 4; ++k) res.vec[i][k] = GetF(base, g_block.ctx + 32 + 16 * i + 4 * k);
    }
    return res;
}

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "native_call.h"

namespace mc::cinematic {
NativeResult CallNative(uint32_t, std::initializer_list<uint32_t>, bool) {
    return {};
}
uint32_t GuestPutVec(float, float, float, float) {
    return 0;
}
uint32_t GuestPutString(const char*) {
    return 0;
}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
