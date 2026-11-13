#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The call shim for the game's script natives: see native_call.h for the convention. The call is
// rex::ppc::GuestToHostFunction<void>(wrapper, context), the way cutscene_gallery.cpp starts a script and the E4 test
// (test/cin-e4) called its natives.
//

#include "native_call.h"

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

#include <chrono>
#include <cstdint>
#include <cstring>

#include "larecomp_log.h"

namespace mc::cinematic {

namespace {

constexpr uint32_t kGuestMallocFn = 0x82130528;  // the game's allocator: r3 = size, returns the block
constexpr uint32_t kCtxBytes = 128;              // the context, 16-byte aligned (a wrapper may store vector registers into it)
constexpr uint32_t kRetBytes = 16;
constexpr uint32_t kMaxArgs = 8;
constexpr uint32_t kRetSentinel = 0xDEADC0DEu;

struct Block {
    uint32_t ctx = 0;
    uint32_t ret = 0;
    uint32_t args = 0;
    bool failed = false;
};
Block g_block;

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
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
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

bool EnsureBlock(uint8_t* base) {
    if (g_block.ctx) return true;
    if (g_block.failed) return false;
    PPCFunc* malloc_fn = GuestFn(kGuestMallocFn);
    const uint32_t total = kCtxBytes + kRetBytes + kMaxArgs * 4;
    const uint32_t raw = malloc_fn ? rex::ppc::GuestToHostFunction<uint32_t>(malloc_fn, total + 16) : 0;
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
    LARECOMP_APP_INFO("[scene] native calls: context block at 0x{:08X}", at);
    return true;
}

}  // namespace

NativeResult CallNative(uint32_t wrapper, std::initializer_list<uint32_t> args) {
    NativeResult res;
    uint8_t* base = Membase();
    if (!base || args.size() > kMaxArgs) return res;
    if (!rex::runtime::ThreadState::Get()) return res;  // not a guest thread: GuestToHostFunction would do nothing
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

    rex::FlushLogging();  // a line written before the call is on disk if the call takes the game down
    const auto t0 = std::chrono::steady_clock::now();
    rex::ppc::GuestToHostFunction<void>(fn, g_block.ctx);
    const auto t1 = std::chrono::steady_clock::now();
    res.called = true;
    res.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.ret = Get32(base, g_block.ret);
    res.ret_written = res.ret != kRetSentinel;
    return res;
}

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "native_call.h"

namespace mc::cinematic {
NativeResult CallNative(uint32_t, std::initializer_list<uint32_t>) {
    return {};
}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
