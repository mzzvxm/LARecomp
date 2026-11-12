// The game's zlib inflate_fast (rex_sub_821E4B20), natively.
//
// What it is (read from the recompiled code and the game image; the generated function, line by
// line, is the loop of zlib 1.2.3's inffast.c):
//  - the image carries "1.2.3" at 0x82012CA0, which sub_82557A68 passes to rex_sub_821DECE0
//    (inflateInit_(strm, version, 56): 56 is sizeof(z_stream) on a 32-bit target);
//  - rex_sub_821DEE40 is inflate(strm, flush): its state machine switches on the mode at
//    state+0 with zlib 1.2.3's numbering (TYPE = 11, BAD = 27 are stored by inflate_fast), and
//    its LEN state, when 6 input and 258 output bytes are left, saves hold/bits into the state
//    and calls rex_sub_821E4B20(strm, out) (r4 = the avail_out inflate() was entered with);
//  - the z_stream offsets (next_in 0, avail_in 4, next_out 12, avail_out 16, msg 24, state 28)
//    and the inflate_state ones the function reads (wsize 40, whave 44, wnext 48, window 52,
//    hold 56, bits 60, lencode 76, distcode 80, lenbits 84, distbits 88) are zlib 1.2.3's;
//    the code table entries are {op u8, bits u8, val u16}, big-endian, as inftrees.h has them;
//  - the three error strings it stores are at 0x820168B0 (invalid literal/length code),
//    0x820168CC (invalid distance code) and 0x82016938 (invalid distance too far back), the
//    mode values 11 and 27 (end of block, error); there is no dmax check (INFLATE_STRICT is
//    off) and no state->sane (1.2.4+).
// rex_sub_821E4B20 has one caller, inflate (rex_sub_821DEE40, call site 821DFE5C). inflate is
// called from sub_82557A68 (three times per script, one z_stream, flush Z_SYNC_FLUSH, the three
// outputs one after the other: mcScript::Load) and from sub_821CC1E0 (a stream reader: 4096-byte
// input chunks, so one inflate_fast per chunk and block).
//
// Why this level and not inflate(): inflate_fast's contract is a pure function of the guest's
// memory (the stream, the state's first 112 bytes, the code tables and the window it reads, the
// input and output bytes) with a short list of writes. inflate() is a streaming state machine
// that the game feeds in pieces (the script loader splits one stream over three outputs), so a
// native replacement would have to keep zlib's state byte for byte for the next call. The fast
// loop is what the recompiled code spends its time in (about 10% of the main thread's samples
// of the first world entry, next to the script AES).
//
// What the override does: LoadCall (inflate_fast.h) reads the arguments and refuses anything it
// cannot reproduce exactly (outside the low guest range, an output buffer that overlaps what the
// loop reads, a window that is not a ring of wsize bytes, ...): those calls go to the recompiled
// function. Otherwise the same loop runs on host pointers and writes the same guest fields.
// Checking it:
//  - the first time the function is called, known-answer tests (inflate_fast_test.h) run;
//  - the first three calls of each kind (nothing in the window yet / the window full with wnext
//    == 0 / wnext != 0) are done by the native decoder on a copy of the output and by the
//    recompiled function on the guest's memory, and the z_stream, the first 112 bytes of the
//    inflate_state and the whole output area are compared; the recompiled result is what the
//    guest keeps. Only after a kind has passed three times are its calls done natively;
//  - any difference logs an error and switches the native decoder off for the rest of the run.
// The log says what was checked, how long each took and, now and then, how much the native
// decoder has done.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "inflate_fast.h"
#include "inflate_fast_test.h"
#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_821E4B20);  // the recompiled inflate_fast

namespace {

enum Mode : int { kUnset = 0, kOn, kOff };

constexpr int kChecksPerClass = 3;           // calls of each window kind compared with the recompiled function
constexpr uint32_t kMinCheckBytes = 512;     // a call with less input or output room than this is not worth a comparison
constexpr uint64_t kMaxCheckBytes = 16u << 20;  // a call whose output area is larger is not compared (it goes to the recompiled code)

std::atomic<int> g_mode{kUnset};

struct ClassState {
    std::atomic<int> tickets{0};  // checks started
    std::atomic<int> passed{0};   // checks that matched
};
ClassState g_class[3];

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_native_ns{0};

const char* ClassName(int c) {
    return c == 0 ? "empty window" : c == 1 ? "full window, wnext 0" : "wnext not 0";
}

const char* EndingName(inflate_fast::Ending e) {
    switch (e) {
        case inflate_fast::kEndLimit: return "out of room";
        case inflate_fast::kEndBlock: return "end of block";
        case inflate_fast::kEndBadLiteral: return "invalid literal/length code";
        case inflate_fast::kEndBadDistCode: return "invalid distance code";
        case inflate_fast::kEndTooFar: return "distance too far back";
    }
    return "?";
}

// Runs the known-answer tests once. A failure keeps the native decoder off.
Mode ChooseMode() {
    static std::once_flag once;
    std::call_once(once, [] {
        const auto t0 = std::chrono::steady_clock::now();
        const inflate_fast::test::KatResult kat = inflate_fast::test::RunKnownAnswerTests();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (kat.ok) {
            LARECOMP_APP_INFO("[Inflate] known-answer tests ok ({} groups in {} us: a zlib stream in stages through a "
                              "512-byte window with two table layouts, random programs against a plain oracle, the error "
                              "endings; paths taken {:#x}); the game's inflate_fast will be checked against the "
                              "recompiled code, then run natively",
                              kat.tests, us, kat.features);
            g_mode.store(kOn);
        } else {
            LARECOMP_APP_ERROR("[Inflate] a known-answer test FAILED after {} groups: {}. This is a bug in inflate_fast.h; "
                               "the recompiled inflate_fast does everything",
                               kat.tests, kat.why);
            g_mode.store(kOff);
        }
    });
    return static_cast<Mode>(g_mode.load());
}

}  // namespace

extern "C" REX_FUNC(rex_sub_821E4B20) {
    // CHECK: the volatile registers and the stack below r1 that the recompiled function leaves
    // changed are dead after the call (the ABI; inflate's call site reloads what it needs from the
    // stream and the state, and keeps its values in r14-r31, which the function restores).
    inflate_fast::GuestCall call;
    if (g_mode.load(std::memory_order_relaxed) == kOff ||
        !inflate_fast::LoadCall(base, ctx.r3.u32, ctx.r4.u32, call) || ChooseMode() == kOff) {
        __imp__rex_sub_821E4B20(ctx, base);
        return;
    }

    ClassState& cs = g_class[call.window_class];
    if (cs.passed.load(std::memory_order_acquire) >= kChecksPerClass) {
        const auto t0 = std::chrono::steady_clock::now();
        inflate_fast::RunNative(base, call);
        const auto t1 = std::chrono::steady_clock::now();
        g_native_ns.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
            std::memory_order_relaxed);
        const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (calls == 1 || calls == 10 || calls == 100 || calls == 1000 || calls == 10000 || calls == 100000) {
            LARECOMP_APP_INFO("[Inflate] {} native calls so far, {} us in the native decoder (checked calls not counted)",
                              calls, g_native_ns.load(std::memory_order_relaxed) / 1000);
        }
        return;
    }

    const uint64_t area = static_cast<uint64_t>(call.produced) + call.avail_out;
    if (area <= kMaxCheckBytes && call.avail_in >= kMinCheckBytes && call.avail_out >= kMinCheckBytes &&
        cs.tickets.load(std::memory_order_relaxed) < kChecksPerClass && cs.tickets.fetch_add(1) < kChecksPerClass) {
        const inflate_fast::CheckReport rep =
            inflate_fast::RunChecked(base, call, [&] { __imp__rex_sub_821E4B20(ctx, base); });
        if (!rep.ok) {
            g_mode.store(kOff);
            LARECOMP_APP_ERROR("[Inflate] MISMATCH: the native inflate_fast differs from the recompiled one ({}; window "
                               "kind '{}', {} bytes in, {} out, native ending '{}'). The native decoder is switched off; "
                               "the recompiled code does all inflate_fast calls from now on",
                               rep.what, ClassName(call.window_class), call.avail_in, call.avail_out,
                               EndingName(rep.result.ending));
            return;
        }
        if (rep.skipped) return;  // no scratch memory: nothing was compared
        const int passed = cs.passed.fetch_add(1) + 1;
        LARECOMP_APP_INFO("[Inflate] call checked against the recompiled code ({}, check {} of {}): {} bytes in, {} out "
                          "area, ended '{}' after {} in / {} out, identical; native {} us, recompiled {} us; paths {:#x}{}",
                          ClassName(call.window_class), passed, kChecksPerClass, call.avail_in, area,
                          EndingName(rep.result.ending), rep.result.in_used, rep.result.out_written,
                          rep.native_ns / 1000, rep.original_ns / 1000, rep.result.features,
                          passed == kChecksPerClass ? "; calls of this kind now run natively" : "");
        return;
    }

    // The checks of this kind are still running on other threads, or the call is too small or too big to compare.
    __imp__rex_sub_821E4B20(ctx, base);
}
// CHECK: speed in the game: not measured. A standalone harness on the dev PC (Ryzen 7 9800X3D, MSVC /O2
// for both sides) ran the generated rex_sub_821E4B20, translated verbatim, against inflate_fast.h inside a
// zlib-1.2.3-style inflate() driver on real deflate streams (text, floats, ints, random, game image slices
// at levels 1/6/9 and other strategies, whole input / 4096-byte chunks / random chunks): the native loop
// was 1.7x to 2.3x faster (8 MB of output: 22 ms against 36 ms, about 370 MB/s against 220 MB/s), with
// every one of 115k calls identical, corrupted streams included. The saving is therefore about 40% of the
// recompiled function's ~2.7 to 4.8 ms in the script load's 48 ms stall (NOTES-opt.md, STUT2 (a)), i.e.
// 1 to 2 ms per world entry, plus the same share of the streaming threads' archive reads.
// CHECK: the project's clang build of this file was not compiled here (only MSVC in the harness).
// CHECK: the guest ranges: input, output, window and tables must lie below 0xE0000000 (the physical mirror
// is left to the recompiled code, as in the script AES override). The stream reader's 4096-byte input chunk
// is a guest stack buffer; stacks are assumed to lie below that limit; a call that does not simply goes to
// the recompiled function.
// CHECK: write watches: the output bytes are stored by the host (memcpy, byte stores) where the recompiled
// code uses byte stores; both are plain host stores, so a watched page faults either way; not observed.
// CHECK: several guest threads can be inside inflate_fast at once (the script loader on the main thread,
// streaming workers): Run() has no shared state; the counters are atomics; a kind whose three checks are
// still running on another thread sends its calls to the recompiled function.
#endif  // !REXGLUE_HAS_XEO3_TARGET
