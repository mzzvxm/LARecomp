// Checkpoints and line triggers that a script asks about: "did the car cross this since I last looked".
//
// The game decides it with a swept segment. The race checkpoints are tested by sub_822653A8
// (checkpoint, end, start): a gate (a line checkpoint, +330) or a circle with a height band (+168
// radius, +180 / +184 heights) against the segment from `start` to `end`.
//  - CheckpointManager_HasHitCheckpoint (sub_82263930 -> sub_822633F0): end is the car's position
//    plus 2 m along its velocity, start is the racer's +400 (the position of the previous update).
//  - CheckpointManager_GetWhichHitCheckpoint (sub_822638F0 -> sub_82263600 -> sub_82263338, a loop
//    over the open checkpoints): end is the car's position, start is the same +400.
//  - mcTriggerLine::Check (sub_826881A8, the script's Trigger_CheckTrigger and, every update, the
//    trigger manager): the same segment, flattened, against the line.
// So the segment covers one update's travel. At 30 FPS the script runs in every update and sees every
// segment. Above 30 FPS a script thread looks only every 2nd to 4th update (see script_input.cpp), and
// the updates in between sweep a piece of road that no look ever tests: a gate or a small circle that
// the car crosses in one of those updates is never reported, however close the car was. HasHit's 2 m
// probe hides most of it (the car has to move more than 2 m between two looks); GetWhichHit's segment
// is one update long, so a gate is missed in about two looks out of three at 147 FPS.
//
// For every place in a script that asks (the running thread and its program counter, with the
// checkpoint or trigger, and the racer) it remembers where the segment ended at its previous look. A
// look that finds no hit and whose segment starts beyond that point tests the piece in between as
// well: the segment from the previous end to the new one, which contains the game's own. That is what
// the script would have been told at 30 FPS, where the previous look is the previous update. Nothing
// changes when the script looks in every update (its previous end is not behind the new start), and a
// car that jumped (a reset, a race start: more than kMaxGapM, or a previous look older than
// kWindowMs) starts afresh, so a teleport never sweeps through a gate.
//
// CHECK: that the racer's +400 is the position of the previous update is read from how the three tests
// use it (every one sweeps from it to the current position) and from the trigger manager running the
// line test once per update; the code that writes it was not found. That HasHit's end is 2 m ahead
// along the car's velocity (sub_822A8148's vector, normalised, times 2.0 at 0x82040E94) is read from
// the code, not seen in a running game. No game has run this fix.
//
// kWindowMs and kMaxGapM: a polling script's looks were 20 to 33 ms apart in the runs that measured it
// (script_input.cpp), so 100 ms is three times that; the fastest car covers about 100 m/s, i.e. 10 m in
// 100 ms, and 15 m leaves room for the car's own segment on top. A previous look older than that, or
// further away, is not the same drive.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "hooks_internal.h"
#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_822653A8);  // the recompiled checkpoint test

namespace {

// scrThread::Run (sub_82554E20) stores the thread it runs here (0 outside a script); the thread's +16
// is the program counter of the opcode being run. The same two words script_input.cpp reads.
constexpr uint32_t kCurrentScriptThread = 0x828DA3BC;
constexpr uint32_t kScriptThreadPc = 16;

constexpr int64_t kWindowMs = 100;
constexpr float kMaxGapM = 15.0f;

struct V3 {
    float x, y, z;
};

V3 ReadV3(const uint8_t* base, uint32_t addr) {
    return {ReadGuestF32(base, addr), ReadGuestF32(base, addr + 4), ReadGuestF32(base, addr + 8)};
}

void WriteV3(uint8_t* base, uint32_t addr, const V3& v) {
    WriteGuestF32(base, addr, v.x);
    WriteGuestF32(base, addr + 4, v.y);
    WriteGuestF32(base, addr + 8, v.z);
}

float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// A place that asks, and where its segment ended the last time.
struct Look {
    uint32_t thread = 0;
    uint32_t pc = 0;
    uint32_t subject = 0;  // the checkpoint or the trigger
    uint32_t racer = 0;    // 0 where the test does not say (the checkpoint test)
    V3 end{};
    int64_t at_ms = 0;  // 0 = unused
};
constexpr int kLooks = 256;

std::mutex g_mutex;
Look g_looks[kLooks];
int g_next_look = 0;
std::atomic<int> g_told[2];  // checkpoint, line trigger: outcomes changed or segments extended

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// This place's previous look, if it is recent and the segment [start, end] begins beyond it. Stores
// `end` as the place's new previous end either way. `gap_ms` is the time since the previous look.
bool PreviousEnd(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer, const V3& start,
                 const V3& end, V3& previous, int64_t& gap_ms) {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    Look* look = nullptr;
    for (Look& l : g_looks) {
        if (l.at_ms != 0 && l.thread == thread && l.pc == pc && l.subject == subject &&
            l.racer == racer) {
            look = &l;
            break;
        }
    }
    if (!look) {
        look = &g_looks[g_next_look];
        g_next_look = (g_next_look + 1) % kLooks;
        look->thread = thread;
        look->pc = pc;
        look->subject = subject;
        look->racer = racer;
        look->end = end;
        look->at_ms = now;
        return false;
    }
    previous = look->end;
    gap_ms = now - look->at_ms;
    look->end = end;
    look->at_ms = now;
    if (gap_ms > kWindowMs) return false;
    const V3 jump = Sub(previous, start);
    if (!(Dot(jump, jump) <= kMaxGapM * kMaxGapM)) return false;
    // Behind the game's own segment: the previous end is on the far side of `start`.
    return Dot(jump, Sub(end, start)) < 0.0f;
}

}  // namespace

// The checkpoint test, called by the three natives above (and by the capture-the-flag natives, which
// test the same way). r3 = the checkpoint, r4 = the end of the segment, r5 = its start (16-byte
// vectors in guest memory); the answer is r3 = 0 or 1. The game's own test runs first; a look that
// finds nothing tests the piece of road since the script's previous look as well.
extern "C" REX_FUNC(rex_sub_822653A8) {
    const uint32_t checkpoint = ctx.r3.u32;
    const uint32_t end_ptr = ctx.r4.u32;
    const uint32_t start_ptr = ctx.r5.u32;
    const auto lr = ctx.lr;
    __imp__rex_sub_822653A8(ctx, base);

    const uint32_t thread = ReadGuestU32(base, kCurrentScriptThread);
    if (!thread || end_ptr < 0x10000u || start_ptr < 0x10000u) return;
    const uint32_t pc = ReadGuestU32(base, thread + kScriptThreadPc);
    const V3 start = ReadV3(base, start_ptr);
    const V3 end = ReadV3(base, end_ptr);
    V3 previous{};
    int64_t gap_ms = -1;
    const bool behind = PreviousEnd(thread, pc, checkpoint, 0, start, end, previous, gap_ms);
    if (!behind || (ctx.r3.u32 & 0xFFu) != 0) return;

    // The same test on [previous, end]. The scratch vector sits just under the caller's frame, and the
    // call runs a little lower still, so its frame does not reach it (the guest's stack below r1 is
    // free; the original does the same).
    const uint32_t sp = ctx.r1.u32;
    const uint32_t scratch = (sp - 0x80u) & ~15u;
    for (uint32_t i = 0; i < 16; ++i) base[scratch + i] = base[start_ptr + i];
    WriteV3(base, scratch, previous);
    ctx.r1.u32 = sp - 0x100u;
    ctx.lr = lr;
    ctx.r3.u64 = checkpoint;
    ctx.r4.u64 = end_ptr;
    ctx.r5.u64 = scratch;
    __imp__rex_sub_822653A8(ctx, base);
    ctx.r1.u32 = sp;
    ctx.lr = lr;
    if ((ctx.r3.u32 & 0xFFu) != 0) {
        ctx.r3.u64 = 1;
        if (g_told[0].fetch_add(1) < 4) {
            LARECOMP_APP_INFO("[CheckpointSweep] checkpoint {:08X}: the car crossed it between two looks of "
                              "the script ({} ms apart, {:.1f} m): it is told it was hit",
                              checkpoint, gap_ms, std::sqrt(Dot(Sub(previous, start), Sub(previous, start))));
        }
    } else {
        ctx.r3.u64 = 0;
    }
}

// 0x82688200 in mcTriggerLine::Check (sub_826881A8; r27 = the racer, r31 = the trigger), before the
// `beq` that follows the two vector stores: the racer's previous position is on the stack at r1 + 128
// (the segment's start) and the car's position at r1 + 144 (its end). A script's look (not the trigger
// manager's own, which runs in every update outside any script) that finds the start beyond its
// previous end starts the segment there instead, which contains the game's.
void Hook_TriggerLineSweep(PPCRegister& r1, PPCRegister& r31, PPCRegister& r27) {
    uint8_t* const base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t thread = ReadGuestU32(base, kCurrentScriptThread);
    if (!thread) return;
    const uint32_t pc = ReadGuestU32(base, thread + kScriptThreadPc);
    const uint32_t start_ptr = r1.u32 + 128;
    const uint32_t end_ptr = r1.u32 + 144;
    const V3 start = ReadV3(base, start_ptr);
    const V3 end = ReadV3(base, end_ptr);
    V3 previous{};
    int64_t gap_ms = -1;
    if (!PreviousEnd(thread, pc, r31.u32, r27.u32, start, end, previous, gap_ms)) return;
    WriteV3(base, start_ptr, previous);
    if (g_told[1].fetch_add(1) < 4) {
        // CHECK: not seen in a running game; whether the longer segment changed the answer is not known here.
        LARECOMP_APP_INFO("[CheckpointSweep] line trigger {:08X}: the script looked {} ms after its "
                          "previous look; the line is tested from where the previous look ended "
                          "({:.1f} m before the car's last position)",
                          r31.u32, gap_ms, std::sqrt(Dot(Sub(previous, start), Sub(previous, start))));
    }
}
#endif  // REXGLUE_HAS_XEO3_TARGET
