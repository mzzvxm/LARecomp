// The game's resource decompression (XMemDecompressStream, LZX) natively.
//
// What it is (read from the recompiled code, VERIFIED unless marked):
//  - the streamer (sub_821BC140) reads a compressed resource 32 KiB at a time into a stack buffer and
//    hands each piece to zlibInflater::InflateBegin (sub_821D5E10) together with the rest of the
//    current output segment as the destination. InflateBegin calls XMemDecompressStream
//    (sub_8244FF20 -> sub_82460420 -> sub_82461010, call site 0x821D5EB8) with (context, dst, &dst_size,
//    src, &src_size) and adds the two sizes it gets back to its cursors. This runs on the two
//    "[RAGE] pgStreamer::Worker" threads; the contexts are created once (sub_821D5D60 ->
//    XMemCreateDecompressionContext, LZX, 128 KiB window) and reset for every resource
//    (sub_821D5DC0 -> sub_8244FEF8 = XMemResetDecompressionContext -> sub_824603C0).
//  - so one resource is many calls and the decoder state lives in the context between them: the LZX
//    window, R0-R2, the block in progress, the tree lengths, a chunk that was cut by the end of the
//    caller's input, and decoded output the caller had no room for. A call can end in the middle of a
//    chunk; a destination can be smaller than a frame.
//
// The boundary: a call of sub_8244FF20 does nothing a single native function cannot do from the
// context's history, but the history is the state, and the context's own memory is the recompiled
// decoder's private layout (about 12 KiB of it). Two ways to keep a native decoder honest about that:
//   (a) translate the recompiled decoder's inner loops (sub_82462FF8 and friends) to run on that
//       memory: exact by construction, but it needs the Huffman table layout and every field the
//       loops touch, and it keeps the call overhead of the rest;
//   (b) keep a native shadow of the whole stream, keyed by the guest context pointer: the chunk layer
//       of sub_82461010 (lzx_stream.h Chunks, a line-by-line mirror, including the partial chunk and
//       the buffered output) and an LZX decoder (lzx_stream.h Stream). The guest context's memory is
//       then not updated, so a stream is native from its start to its end or not at all.
// This file does (b). It decides at the reset of a context (sub_8244FEF8), which is the start of a
// stream: the original reset runs, then the context is either armed as a native stream or left alone.
// A stream that is not armed runs on the recompiled code from its first call to its last, as before.
//
// Checking: a stream armed as CHECKED runs both decoders on every call, from its start. The native
// one decodes into a scratch copy of the destination first, then the recompiled function runs on the
// guest's memory (that result is what the game keeps), and the two are compared: the bytes written,
// the sizes stored back (*dst_size, *src_size) and the status in r3. Both keep their state, so a
// difference at any point means the recompiled decoder carries on for the rest of that stream, and the
// native decoder is switched off for every stream that starts after it. Streams are CHECKED until 64
// calls from at least 4 completed streams matched; after that they are NATIVE (no recompiled call at
// all). lzx_native_check_all keeps every stream CHECKED.
//
// A NATIVE stream cannot go back to the recompiled code mid-stream (the guest's decoder state was
// never built). If the native decoder meets a frame it cannot decode, it logs it once and gives 0
// bytes for that frame and the rest of the stream (the recompiled decoder also gives 0 bytes for a
// frame it rejects, but its later frames then decode from a damaged state; there is no useful
// equivalent). Valid streams, which are everything the game and the mod loader produce, do not hit
// this; the check phase decodes real streams and would show it.
//
// Threads: the streamer's worker threads each use their own contexts, and a context is reset and used
// by one thread at a time (the guest's own order). The slot table is looked up without a lock; a lock
// is taken only when a context is armed for the first time. No lock on the decode path.
//
// Memory: guest pointers become host pointers with the generated code's own mapping (REX_RAW_ADDR:
// addresses from 0xE0000000 up sit 0x1000 higher on Windows). A buffer that would straddle that
// address is not handled (nothing like it exists: the stack, the heap and the resource segments are
// all below it, and the physical mirror is above it in one piece).
//
// Time: lzx_stats's Pre/Post hooks (0x821D5EB4, 0x821D5EBC) bracket the call to sub_8244FF20, so they
// time whichever decoder ran, native or recompiled, and read the sizes from the same stack slots.
// A CHECKED call includes both decoders; compare lzx_stats with lzx_native=false and with the checks
// finished (or without them) for the native decoder's cost.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include "larecomp_log.h"
#include "lzx_stream.h"

REX_EXTERN(__imp__rex_sub_8244FF20);  // the recompiled XMemDecompressStream
REX_EXTERN(__imp__rex_sub_8244FEF8);  // the recompiled XMemResetDecompressionContext

REXCVAR_DEFINE_BOOL(lzx_native, true, "MCLA/Performance",
    "Decompress the game's streamed resources (LZX) with native code instead of the recompiled "
    "XMemDecompressStream.\n"
    "\n"
    "The first streams are decoded by both and compared; any difference switches the native decoder "
    "off for the rest of the session. Read at start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(lzx_native_check_all, false, "MCLA/Diagnostics",
    "Run the recompiled LZX decoder next to the native one for every stream, not only the first few, "
    "and compare every call. Slower than either alone: for validating a whole session. Read at start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

constexpr uint32_t kLowGuestLimit = 0xE0000000u;  // from here up the host address is 0x1000 higher
constexpr uint32_t kGuestFloor = 0x10000u;
constexpr int kTrustCalls = 64;      // checked calls that must match before streams run natively
constexpr int kTrustStreams = 4;     // checked streams that must have finished
constexpr uint32_t kMaxScratch = 256u << 20;  // a checked call with a larger output space is not checked
constexpr int kSlots = 8;            // contexts the game has: three streamer slots

enum class Mode : int { kInactive = 0, kChecked, kNative };

uint8_t* HostPtr(uint8_t* base, uint32_t addr) {
    return base + addr + (addr >= kLowGuestLimit ? 0x1000u : 0u);
}

uint32_t LoadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void StoreBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// A guest range [addr, addr + size) the native code can address: below the physical mirror or wholly in it.
bool RangeOk(uint32_t addr, uint32_t size) {
    if (size == 0) return true;
    if (addr < kGuestFloor) return false;
    const uint64_t end = static_cast<uint64_t>(addr) + size;
    if (end > 0x100000000ull) return false;
    return (addr < kLowGuestLimit) == (end <= kLowGuestLimit);
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Slot {
    std::atomic<uint32_t> key{0};  // the guest context pointer, 0 = free
    Mode mode = Mode::kInactive;
    std::unique_ptr<lzx_native::Chunks> chunks;
    int calls = 0;       // calls of the current stream so far
    bool clean = true;   // every checked call of the current stream matched
};

Slot g_slots[kSlots];
std::mutex g_slot_mutex;  // arming a context for the first time only

std::atomic<bool> g_off{false};   // a mismatch was seen: no new stream runs natively
std::atomic<int> g_checked_calls{0};
std::atomic<int> g_checked_streams{0};
std::atomic<bool> g_trusted{false};
std::atomic<bool> g_logged_start{false};
std::atomic<bool> g_logged_refusal{false};
std::atomic<uint64_t> g_check_native_ns{0};
std::atomic<uint64_t> g_check_original_ns{0};
std::atomic<uint64_t> g_native_calls{0};
std::atomic<uint64_t> g_native_ns{0};
std::atomic<uint64_t> g_native_in{0};
std::atomic<uint64_t> g_native_out{0};
std::atomic<uint64_t> g_streams_native{0};
std::atomic<uint64_t> g_streams_checked{0};
std::atomic<int64_t> g_last_stats_ns{0};

bool Enabled() {
    static const bool enabled = REXCVAR_GET(lzx_native);
    return enabled;
}
bool CheckAll() {
    static const bool check_all = REXCVAR_GET(lzx_native_check_all);
    return check_all;
}

Slot* FindSlot(uint32_t key) {
    if (key == 0) return nullptr;
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_acquire) == key) return &s;
    }
    return nullptr;
}

Slot* FindOrAddSlot(uint32_t key) {
    if (Slot* s = FindSlot(key)) return s;
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    if (Slot* s = FindSlot(key)) return s;
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_relaxed) == 0) {
            s.mode = Mode::kInactive;
            s.chunks = std::make_unique<lzx_native::Chunks>();
            s.key.store(key, std::memory_order_release);
            return &s;
        }
    }
    return nullptr;
}

void LogStats(const char* why) {
    const uint64_t calls = g_native_calls.load(std::memory_order_relaxed);
    LARECOMP_APP_INFO("[LZX] {}: {} native calls in {} streams, {} KB in, {} KB out, {} us in the native decoder "
                      "(checked calls not counted)",
                      why, calls, g_streams_native.load(std::memory_order_relaxed),
                      g_native_in.load(std::memory_order_relaxed) / 1024, g_native_out.load(std::memory_order_relaxed) / 1024,
                      g_native_ns.load(std::memory_order_relaxed) / 1000);
}

// The recompiled decoder is the one that decides: it runs on the guest's memory; the native result is
// only compared with it, and a difference drops the stream to the recompiled code. Returns true if the
// recompiled function ran (the result is in the guest's memory and r3); false if the call is one the
// native code does not take, in which case nothing ran, the stream is dropped and the caller runs the
// recompiled function.
bool CheckedCall(Slot& s, PPCContext& ctx, uint8_t* base) {
    const uint32_t dst = ctx.r4.u32, p_dst = ctx.r5.u32, src = ctx.r6.u32, p_src = ctx.r7.u32;
    if (!RangeOk(p_dst, 4) || !RangeOk(p_src, 4)) {
        s.mode = Mode::kInactive;
        return false;
    }
    const uint32_t dst_cap = LoadBe32(HostPtr(base, p_dst));
    const uint32_t src_len = LoadBe32(HostPtr(base, p_src));
    if (!RangeOk(dst, dst_cap) || !RangeOk(src, src_len) || dst_cap > kMaxScratch) {
        s.mode = Mode::kInactive;  // the recompiled code takes this stream from here, its state is intact
        return false;
    }
    std::unique_ptr<uint8_t[]> scratch(new uint8_t[dst_cap ? dst_cap : 1]);
    uint32_t n_written = 0, n_consumed = 0;
    const int64_t t0 = NowNs();
    s.chunks->Step(HostPtr(base, src), src_len, scratch.get(), dst_cap, n_written, n_consumed);
    const int64_t t1 = NowNs();
    __imp__rex_sub_8244FF20(ctx, base);  // r4..r7 are not changed by the native step
    const int64_t t2 = NowNs();
    const uint32_t status = ctx.r3.u32;
    const uint32_t g_written = LoadBe32(HostPtr(base, p_dst));
    const uint32_t g_consumed = LoadBe32(HostPtr(base, p_src));
    g_check_native_ns.fetch_add(static_cast<uint64_t>(t1 - t0), std::memory_order_relaxed);
    g_check_original_ns.fetch_add(static_cast<uint64_t>(t2 - t1), std::memory_order_relaxed);

    const uint8_t* const guest_out = HostPtr(base, dst);
    bool same = status == 0 && g_written == n_written && g_consumed == n_consumed && n_written <= dst_cap;
    uint32_t first_diff = 0;
    if (same && n_written != 0 && std::memcmp(guest_out, scratch.get(), n_written) != 0) {
        same = false;
        while (first_diff < n_written && guest_out[first_diff] == scratch[first_diff]) ++first_diff;
    }
    if (!same) {
        g_off.store(true);
        s.mode = Mode::kInactive;
        LARECOMP_APP_ERROR("[LZX] MISMATCH: the native decoder differs from the recompiled one (call {} of the stream, "
                           "{} bytes in, {} bytes of room; recompiled: status {:#x}, {} written, {} consumed; native: {} "
                           "written, {} consumed; first differing output byte {}). The native decoder is switched off; "
                           "the recompiled code does the rest of this stream and all later ones",
                           s.calls + 1, src_len, dst_cap, status, g_written, g_consumed, n_written, n_consumed, first_diff);
        return true;
    }
    ++s.calls;
    const int done = g_checked_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (done <= 3) {
        LARECOMP_APP_INFO("[LZX] call checked against the recompiled code ({}): {} bytes in, {} bytes of room, {} written, "
                          "{} consumed, identical; native {} us, recompiled {} us",
                          done, src_len, dst_cap, n_written, n_consumed, (t1 - t0) / 1000, (t2 - t1) / 1000);
    }
    if (!CheckAll() && !g_trusted.load(std::memory_order_relaxed) && done >= kTrustCalls &&
        g_checked_streams.load(std::memory_order_relaxed) >= kTrustStreams) {
        if (!g_trusted.exchange(true)) {
            LARECOMP_APP_INFO("[LZX] {} calls from {} finished streams matched the recompiled decoder byte for byte "
                              "(native {} us, recompiled {} us in those calls); streams that start now run natively",
                              done, g_checked_streams.load(std::memory_order_relaxed),
                              g_check_native_ns.load(std::memory_order_relaxed) / 1000,
                              g_check_original_ns.load(std::memory_order_relaxed) / 1000);
        }
    }
    return true;
}

}  // namespace

// Start of a stream.
extern "C" REX_FUNC(rex_sub_8244FEF8) {
    const uint32_t gctx = ctx.r3.u32;
    __imp__rex_sub_8244FEF8(ctx, base);
    const bool reset_ok = ctx.r3.u32 == 0;

    Slot* s = FindSlot(gctx);
    if (s && s->mode == Mode::kChecked && s->calls > 0 && s->clean) {  // the stream that just ended matched all the way
        g_checked_streams.fetch_add(1, std::memory_order_relaxed);
    }
    if (s) s->mode = Mode::kInactive;

    if (!Enabled()) {
        if (!g_logged_start.exchange(true)) {
            LARECOMP_APP_INFO("[LZX] native decoding is off (lzx_native = false): the recompiled XMemDecompressStream "
                              "does all resource decompression");
        }
        return;
    }
    if (!reset_ok || g_off.load(std::memory_order_relaxed)) return;

    // The context must be what the recompiled creation function builds for the game's 128 KiB LZX window: type 1 at
    // +4, the window size at +24 and its mask at +28 (read from sub_82461B98 / sub_82461F78, which use state + 4 and
    // state + 8 for them). Anything else is left to the recompiled code.
    if (!RangeOk(gctx, 64)) return;
    const uint8_t* g = HostPtr(base, gctx);
    if (LoadBe32(g + 4) != 1 || LoadBe32(g + 24) != lzx_native::kWindowSize || LoadBe32(g + 28) != lzx_native::kWindowMask) {
        if (!g_logged_refusal.exchange(true)) {
            LARECOMP_APP_ERROR("[LZX] a decompression context is not the one expected (type {}, window {:#x}, mask {:#x}): "
                               "its streams stay on the recompiled decoder",
                               LoadBe32(g + 4), LoadBe32(g + 24), LoadBe32(g + 28));
        }
        return;
    }

    s = FindOrAddSlot(gctx);
    if (!s) return;
    s->chunks->Reset();
    s->calls = 0;
    s->clean = true;
    const bool trusted = g_trusted.load(std::memory_order_relaxed) && !CheckAll();
    s->mode = trusted ? Mode::kNative : Mode::kChecked;
    if (trusted) {
        g_streams_native.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_streams_checked.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_logged_start.exchange(true)) {
        LARECOMP_APP_INFO("[LZX] native LZX decoder on: streams are checked against the recompiled decoder until {} calls "
                          "from {} finished streams match{}",
                          kTrustCalls, kTrustStreams, CheckAll() ? " (lzx_native_check_all: every stream stays checked)" : "");
    }
}

// XMemDecompressStream(context, dst, &dst_size, src, &src_size).
extern "C" REX_FUNC(rex_sub_8244FF20) {
    Slot* s = FindSlot(ctx.r3.u32);
    if (!s || s->mode == Mode::kInactive) {
        __imp__rex_sub_8244FF20(ctx, base);
        return;
    }

    if (s->mode == Mode::kChecked) {
        if (!CheckedCall(*s, ctx, base)) __imp__rex_sub_8244FF20(ctx, base);
        return;
    }

    // NATIVE: the guest's context memory is not touched.
    const uint32_t dst = ctx.r4.u32, p_dst = ctx.r5.u32, src = ctx.r6.u32, p_src = ctx.r7.u32;
    if (!RangeOk(p_dst, 4) || !RangeOk(p_src, 4)) {
        ctx.r3.u64 = 0x80004005u;  // E_FAIL: nothing like this is expected
        return;
    }
    const uint32_t dst_cap = LoadBe32(HostPtr(base, p_dst));
    const uint32_t src_len = LoadBe32(HostPtr(base, p_src));
    if (!RangeOk(dst, dst_cap) || !RangeOk(src, src_len)) {
        StoreBe32(HostPtr(base, p_dst), 0);
        StoreBe32(HostPtr(base, p_src), 0);
        ctx.r3.u64 = 0x80004005u;
        return;
    }
    uint32_t written = 0, consumed = 0;
    const int64_t t0 = NowNs();
    s->chunks->Step(HostPtr(base, src), src_len, HostPtr(base, dst), dst_cap, written, consumed);
    const int64_t t1 = NowNs();
    StoreBe32(HostPtr(base, p_dst), written);
    StoreBe32(HostPtr(base, p_src), consumed);
    ctx.r3.u64 = 0;  // S_OK, as sub_82460420 maps the decoder's 0

    g_native_ns.fetch_add(static_cast<uint64_t>(t1 - t0), std::memory_order_relaxed);
    g_native_in.fetch_add(consumed, std::memory_order_relaxed);
    g_native_out.fetch_add(written, std::memory_order_relaxed);
    const uint64_t calls = g_native_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (s->chunks->failed() && s->clean) {
        s->clean = false;
        LARECOMP_APP_ERROR("[LZX] a frame of a native stream could not be decoded (call {} of the stream, {} bytes in); "
                           "the rest of the stream gives no data",
                           s->calls + 1, src_len);
    }
    ++s->calls;
    if (calls == 1 || calls == 10 || calls == 100 || calls == 1000 || calls == 10000 || calls == 100000 ||
        calls == 1000000) {
        LogStats("native decoder");
    } else {
        int64_t last = g_last_stats_ns.load(std::memory_order_relaxed);
        if (t1 - last >= 120'000'000'000ll && g_last_stats_ns.compare_exchange_strong(last, t1)) {
            if (last != 0) LogStats("native decoder, periodic");
        }
    }
}
// CHECK: speed in the game: not measured here (this was written without a build). LZX decoding is about 7% of a
// core on the streamer worker while driving and about a whole core at the world entry (guest profile,
// 82462BD0); the native decoder is expected to cost a fraction of that, the checked streams cost both.
// CHECK: the project's clang build of lzx_stream.h / this file was not compiled when it was written.
// CHECK: the recompiled reset (sub_8244FEF8) leaves nothing the next stream's decode depends on from the stream
// before it (a fresh stream must not); the native stream starts from a fresh decoder, so a stream that
// the recompiled decoder would decode differently after a damaged predecessor is not reproduced.
// CHECK: pad byte after an odd-sized uncompressed block, E8 translation and the reset-interval behaviour
// of XCompress follow libmspack's rules, not the game's code; the checked streams are what tests them.
// CHECK: the window and mask offsets (ctx + 24, + 28) come from reading sub_82461B98 and sub_82461F78; a wrong
// reading keeps every stream on the recompiled decoder (logged once as "not the one expected").
#endif  // !REXGLUE_HAS_XEO3_TARGET
