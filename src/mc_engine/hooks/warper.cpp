// The warper: a far-away "go to garage" (or any cinematic warp) that never ends.
//
// Picking the garage from the pause menu while far from it plays a camera flight
// over the city, then the game is meant to cut inside. Sometimes it does not: the
// picture stays on the garage facade forever, the frame keeps ticking and no input
// does anything. It happens in the emulators too, and almost every time from far
// enough away.
//
// RE map (default.xex):
//
//   *(0x8286D8D4) + 0x51240   mcWarper. Its Update, sub_8221BE98(warper, dt), runs
//                             at the very end of mcUIManager::Update (sub_821FC588),
//                             after the UI state machine has stepped the frame's
//                             transitions.
//   warper+68                 current warp params; +108 the camera transition
//   warper+288                state: 26 starts the transition, 19 / 20 wait for it
//                             (20 when params+3, the cinematic warps), 29 is idle
//   warper+296 / +300         the transition being waited on; TRANSITION_COMPLETE
//                             clears +300 when it arrives
//   warper+256                id of the TRANSITION_COMPLETE event the warper listens to
//   warper+276                "the wait is over"
//   sub_82219678              picks the transition by distance: more than 2 km away
//                             is Warp_LowAltitude_FarAway_Cinematically (5.3 s),
//                             high up is Warp_HighAltitude_Cinematically
//   transition +16 & 0x80     active (vfunc 92); +76 duration (vfunc 592), +96 time
//                             (vfunc 608). SetDone (sub_82228858) deactivates it in
//                             the same step it ends, and its OnExit (sub_8222F5B8)
//                             sends TRANSITION_COMPLETE right away (sub_8268B770
//                             dispatches synchronously).
//   sub_8221B8C8              the TRANSITION_COMPLETE handler: releases +300 and the
//                             warp camera, and finalizes the warp (sub_8221B790:
//                             state 29, params back to the pool, next queued warp)
//                             -- but only when +276 is already set.
//   *(0x8286D8F0) + 293       mcCineScript's GPS-transition interlock; states 19 and
//                             20 clear it when they finish.
//
// State 20 only finishes while the transition is still active, and only when
//
//     time + dt + 0.0001 >= duration
//
// -- it predicts, one frame early, that the next step will end the transition, and
// sets +276 so the TRANSITION_COMPLETE that follows finalizes the warp. That holds
// on a fixed 1/30 s step, which is also how the duration is computed (sub_82228180
// steps the transition at 1/30 s until it is done). On real frame deltas it misses
// two ways: the step that ends the transition can be longer than the dt the warper
// predicted with, and the transition can end a few ms before the stored duration.
// Either way the warper sees it active and one frame short, the UI then ends and
// deactivates it before the warper looks again, TRANSITION_COMPLETE arrives with
// +276 still clear, the handler skips the finalize, and the warper waits in state 20
// for good. Measured on the 5.25 s flight from 7 km away: the transition steps by
// exactly the warper's dt, that dt ran from 6.5 ms to 100 ms (jumps of 16 -> 31 ms
// and 41 -> 98 ms while the new area streams in), and a natural miss ended it at
// 5.2502 s against a stored 5.2524 s, with the last check at 5.2293 + 0.0196. State
// 19 has the same shape with its own timer and half a second of slack, so it only
// needs a far longer stall to miss.
//
// Fix: after the game's own update, a state 19 / 20 transition that was running and
// is not anymore has ended. Do what the missed frame would have done -- clear the
// interlock, set +276 and state 29 -- and then finalize the way the handler would
// have: sub_8221B790 when TRANSITION_COMPLETE already came (+300 cleared), or the
// handler itself when it never did. A transition that never starts at all is given
// its duration plus a few seconds before the same is done.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "../logging.h"

REXCVAR_DEFINE_BOOL(warp_transition_fix, true, "MCLA/Patches",
    "Finish a cinematic warp (pause menu -> garage from far away) whose camera "
    "flight ended between two frames. Without it the game can sit on the garage "
    "facade forever: the warper only notices the flight's end by predicting it one "
    "frame early from a duration measured at a fixed 1/30 s step, and real, varying "
    "frame times slip past that prediction.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(warp_transition_trace, false, "MCLA/Diagnostics",
    "Log every frame the warper spends waiting on a cinematic warp transition: the "
    "transition's time, duration, active flag and the frame delta.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(warp_transition_test_miss, false, "MCLA/Diagnostics",
    "Testing aid: hide the running transition from the warper's own end check, so "
    "every cinematic warp ends the way the stuck ones do -- the transition finishes "
    "and reports it while the warper is not looking. With warp_transition_fix off "
    "this reproduces the stuck garage facade on demand.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_8221BE98);  // mcWarper::Update
REX_EXTERN(__imp__rex_sub_8221B790);  // mcWarper finalize
REX_EXTERN(__imp__rex_sub_8221B8C8);  // mcWarper TRANSITION_COMPLETE handler

namespace {

// Guest memory is big-endian.
uint32_t ReadGuestU32(const uint8_t* base, uint32_t addr) {
    return (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
           (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
}

void WriteGuestU32(uint8_t* base, uint32_t addr, uint32_t val) {
    base[addr + 0] = uint8_t(val >> 24);
    base[addr + 1] = uint8_t(val >> 16);
    base[addr + 2] = uint8_t(val >> 8);
    base[addr + 3] = uint8_t(val);
}

float ReadGuestF32(const uint8_t* base, uint32_t addr) {
    const uint32_t bits = ReadGuestU32(base, addr);
    float val;
    std::memcpy(&val, &bits, sizeof(val));
    return val;
}

constexpr uint32_t kCineScriptPtr     = 0x8286D8F0;
constexpr uint32_t kCineGpsInterlock  = 293;

constexpr uint32_t kWarperEventId     = 256;
constexpr uint32_t kWarperDone        = 276;
constexpr uint32_t kWarperState       = 288;
constexpr uint32_t kWarperTransition  = 296;
constexpr uint32_t kWarperPending     = 300;
constexpr uint32_t kStateWaitTimed    = 19;
constexpr uint32_t kStateWaitCinematic = 20;
constexpr uint32_t kStateIdle         = 29;

constexpr uint32_t kTransitionFlags   = 16;
constexpr uint32_t kTransitionName    = 20;
constexpr uint32_t kTransitionDur     = 76;
constexpr uint32_t kTransitionTime    = 96;
constexpr uint32_t kTransitionActive  = 0x80;

// Grace for a transition that never became active in state 19 / 20.
constexpr float kNeverStartedGrace    = 3.0f;

struct Watch {
    uint32_t transition = 0;
    bool seen_active = false;
    float waited = 0.0f;
    float last_time = 0.0f;
    float last_dt = 0.0f;
    uint32_t frames = 0;
};
Watch g_watch;

std::string TransitionName(const uint8_t* base, uint32_t transition) {
    const uint32_t p = ReadGuestU32(base, transition + kTransitionName);
    if (!p) return "<unnamed>";
    const char* s = reinterpret_cast<const char*>(base + p);
    return std::string(s, strnlen(s, 64));
}

void FinishMissedWarp(PPCContext& ctx, uint8_t* base, uint32_t warper, uint32_t transition,
                      bool never_started) {
    const float dur = ReadGuestF32(base, transition + kTransitionDur);
    const uint32_t pending = ReadGuestU32(base, warper + kWarperPending);
    if (never_started)
        MC_WARN("[warper] '{}' never started; waited {:.2f}s (duration {:.3f}s) -- finishing the warp",
                TransitionName(base, transition), g_watch.waited, dur);
    else
        MC_WARN("[warper] '{}' ended between frames: last seen at {:.3f}s of {:.3f}s with dt {:.4f} "
                "-- finishing the warp the game missed ({})",
                TransitionName(base, transition), g_watch.last_time, dur, g_watch.last_dt,
                pending ? "TRANSITION_COMPLETE not seen" : "after TRANSITION_COMPLETE");

    // What states 19 and 20 do when their check fires.
    if (const uint32_t cine = ReadGuestU32(base, kCineScriptPtr)) base[cine + kCineGpsInterlock] = 0;
    base[warper + kWarperDone] = 1;
    WriteGuestU32(base, warper + kWarperState, kStateIdle);

    // What TRANSITION_COMPLETE then does with +276 set.
    if (pending == transition) {
        ctx.r3.u64 = warper;
        ctx.r4.u64 = ReadGuestU32(base, warper + kWarperEventId);
        ctx.r5.u64 = transition;
        __imp__rex_sub_8221B8C8(ctx, base);
    } else {
        ctx.r3.u64 = warper;
        __imp__rex_sub_8221B790(ctx, base);
    }
}

bool IsWaitState(uint32_t state) { return state == kStateWaitTimed || state == kStateWaitCinematic; }

void WatchWarper(PPCContext& ctx, uint8_t* base, uint32_t warper, float dt) {
    if (!IsWaitState(ReadGuestU32(base, warper + kWarperState))) {
        if (g_watch.transition && g_watch.frames)
            MC_INFO("[warper] '{}' finished normally after {} frames ({:.2f}s)",
                    TransitionName(base, g_watch.transition), g_watch.frames, g_watch.waited);
        g_watch = {};
        return;
    }
    const uint32_t transition = ReadGuestU32(base, warper + kWarperTransition);
    if (!transition) return;
    if (transition != g_watch.transition) {
        g_watch = {};
        g_watch.transition = transition;
        MC_INFO("[warper] waiting on '{}' ({:.3f}s)", TransitionName(base, transition),
                ReadGuestF32(base, transition + kTransitionDur));
    }

    const bool active = (ReadGuestU32(base, transition + kTransitionFlags) & kTransitionActive) != 0;
    const float time = ReadGuestF32(base, transition + kTransitionTime);
    const float dur = ReadGuestF32(base, transition + kTransitionDur);
    ++g_watch.frames;
    g_watch.waited += dt;
    if (REXCVAR_GET(warp_transition_trace))
        MC_INFO("[warper] frame {} dt {:.4f} active {} t {:.4f} / {:.4f}", g_watch.frames, dt, active ? 1 : 0,
                time, dur);
    if (active) {
        g_watch.seen_active = true;
        g_watch.last_time = time;
        g_watch.last_dt = dt;
        return;
    }

    const bool never_started = !g_watch.seen_active && g_watch.waited > std::max(dur, 0.0f) + kNeverStartedGrace;
    if (!g_watch.seen_active && !never_started) return;
    if (!REXCVAR_GET(warp_transition_fix)) {
        if (g_watch.frames % 600 == 1)
            MC_WARN("[warper] stuck on '{}' (warp_transition_fix off)", TransitionName(base, transition));
        return;
    }
    FinishMissedWarp(ctx, base, warper, transition, never_started);
    g_watch = {};
}

}  // namespace

// mcWarper::Update. The game's own state machine runs first; the watch looks at
// what it left behind.
extern "C" REX_FUNC(rex_sub_8221BE98) {
    const uint32_t warper = ctx.r3.u32;
    const float dt = static_cast<float>(ctx.f1.f64);
    uint32_t hidden = 0;
    if (warper && REXCVAR_GET(warp_transition_test_miss) &&
        IsWaitState(ReadGuestU32(base, warper + kWarperState))) {
        const uint32_t transition = ReadGuestU32(base, warper + kWarperTransition);
        const uint32_t flags = transition ? ReadGuestU32(base, transition + kTransitionFlags) : 0;
        if (flags & kTransitionActive) {
            WriteGuestU32(base, transition + kTransitionFlags, flags & ~kTransitionActive);
            hidden = transition;
        }
    }
    __imp__rex_sub_8221BE98(ctx, base);
    if (hidden)
        WriteGuestU32(base, hidden + kTransitionFlags,
                      ReadGuestU32(base, hidden + kTransitionFlags) | kTransitionActive);
    if (warper) WatchWarper(ctx, base, warper, dt);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
