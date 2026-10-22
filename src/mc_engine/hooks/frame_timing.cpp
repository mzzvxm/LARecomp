// The 60 FPS clock: the measured frame delta fed to the simulation instead of
// the engine's fixed 30 Hz step, the hitch clamp, the frame rate limiter, the
// swap interval and the guest's D3D fence spin.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "hooks.h"
#include "hooks_internal.h"
#include "../guest_profiler.h"

// BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
REXCVAR_DEFINE_BOOL(real_frame_delta, true, "MCLA/Patches",
    "Feeds the simulation the measured frame time instead of the engine's fixed "
    "30 Hz timestep, and unlocks presentation from every-other-vblank. Required "
    "for correct physics, camera and traffic above 30 FPS. This is NOT a frame "
    "rate cap - see fps_limit for that.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Default to 60 FPS: with vsync=false for low input lag and fast pacing,
// the precision frame limiter caps to 60 FPS out-of-the-box.
REXCVAR_DEFINE_INT32(fps_limit, 60, "MCLA/Performance",
    "Frame rate cap (0 = uncapped, 60 = 60 FPS, 120 = 120 FPS, 144 = 144 FPS).")
    .range(0, 360)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(intro_original_speed, true, "MCLA/Patches",
    "Hold the start-up legal screens and logos to their original 30 presents per "
    "second. They are drawn in a loop that never reaches the engine timer, so the "
    "frame limiter does not pace them. Off lets them run as fast as the host presents.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_limit_spin, true, "MCLA/Performance",
    "Hit the frame-limit deadline with a PAUSE spin. Off restores the sleep-then-yield wait, "
    "which gives the core back instead of burning it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vsync_fast_poll, true, "MCLA/Performance",
    "Drive the SDK's gpu_vsync_fast_poll. With vsync off the guest vblank interval is 1 ms, so "
    "fast poll keeps the GPU VSync worker spinning a whole core. Off restores the 1 ms sleep.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(fence_spin_throttle, true, "MCLA/Performance",
    "Yield the host CPU inside the guest's D3D fence poll (sub_82412F98) instead of running "
    "its 32 pipeline-throttle NOPs. Off restores the stock guest spin.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Throttle the D3D poll predicate / fence spin-wait.
//
// In sub_82412F98 (called by D3DDevice_BlockOnFence at 0x82411F34 and sub_82411180),
// the guest executes a tight loop of 32 `mr r31, r31` instructions meant to pause
// PowerPC in-order hardware execution pipelines.
// On host x86, those NOPs compile to an unconstrained busy-wait loop that burns ~40%
// of the Render Thread (XThread4918 at ~98% CPU) while waiting for GPU Commands.
//
// We replace the spin with host CPU yielding:
// - Low spin counts: YieldProcessor() (x86 PAUSE) to keep wake latency minimal.
// - Extended spin: SwitchToThread() yields the CPU quantum directly to GPU Commands.
// Returning true jumps to 0x82412FD8, bypassing the 32 NOP instructions.
// Instrumentation for the claim above. The hook sits INSIDE the 4-iteration NOP
// loop, so "how often does the guest reach this fence poll" and "how much wall
// time does the yield itself cost" are the only two numbers that decide whether
// this patch pays for itself. Both are published per second by the timing log.
std::atomic<uint64_t> g_fence_hook_calls{0};
std::atomic<uint64_t> g_fence_switches{0};
std::atomic<uint64_t> g_fence_switch_us{0};

bool Patch_FenceSpinThrottle() {
    // The guest reaches this ~10 million times a second, so the counters below
    // need their own switch, separate from MCLA_TIMING_LOG: an unconditional
    // atomic RMW at that rate costs about 1.7 fps on its own, which is the
    // same size as the effects the timing log is there to measure. Arming them
    // with the log made every A/B pay the instrumentation and compare against
    // a binary that did not. MCLA_SPIN_COUNTERS=1 turns them on deliberately.
    static const bool kCount = [] {
        const char* e = std::getenv("MCLA_SPIN_COUNTERS");
        return e && *e == '1';
    }();
    if (!REXCVAR_GET(fence_spin_throttle)) {
        // Still count the reaches, so the OFF run reports the same call rate
        // and the two runs are comparable.
        if (kCount) g_fence_hook_calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#if defined(_WIN32)
    static thread_local uint32_t s_spin_count = 0;
    if (kCount) g_fence_hook_calls.fetch_add(1, std::memory_order_relaxed);
    YieldProcessor();
    if (++s_spin_count >= 16) {
        LARGE_INTEGER a, b;
        if (kCount) QueryPerformanceCounter(&a);
        SwitchToThread();
        if (kCount) QueryPerformanceCounter(&b);
        static const double kUsPerTick = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            return 1000000.0 / double(f.QuadPart);
        }();
        if (kCount) {
            g_fence_switches.fetch_add(1, std::memory_order_relaxed);
            g_fence_switch_us.fetch_add(
                uint64_t((b.QuadPart - a.QuadPart) * kUsPerTick), std::memory_order_relaxed);
        }
        s_spin_count = 0;
    }
#endif
    return true;
}

// Set by MCLAFrameDelta each time the engine timer (sub_821BDA90) runs, cleared
// by each swap. See PaceUntimedSwap.
static std::atomic<bool> g_engine_timer_ran_since_swap{false};

// Intro pacing.
//
// The start-up legal screens and publisher logos are not Bink video. They are
// drawn in a loop that never calls the engine timer, so MCLAFrameDelta and its
// frame limiter never run while they are on screen. On the console that loop
// was paced only by the present interval of two vblanks, which the runtime
// ignores, so the sequence runs as fast as the host presents. That is why
// fps_limit at 30, 45 or 60 made no difference to its speed.
//
// A swap that follows another swap with no pass through the engine timer in
// between is therefore held to the original 30 per second. Gameplay frames
// always pass through the timer first and are left to fps_limit.
static void PaceUntimedSwap() {
    static std::atomic<uint64_t> last_swap_us{0};
    constexpr uint64_t kPeriodUs = 1000000ull / 30;
    auto now_us = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    const bool timed = g_engine_timer_ran_since_swap.exchange(false, std::memory_order_relaxed) ||
                       !REXCVAR_GET(intro_original_speed);
    const uint64_t last = last_swap_us.load(std::memory_order_relaxed);
    uint64_t now = now_us();
    if (!timed && last && now - last < kPeriodUs) {
        std::this_thread::sleep_for(std::chrono::microseconds(kPeriodUs - (now - last)));
        now = now_us();
    }
    last_swap_us.store(now, std::memory_order_relaxed);
}

// Swap-interval patch at 0x82419AA0 ("li r11, 2"): the game always requests
// D3D interval TWO (30 FPS); replacing with 1 requests 60 Hz. Note the current
// RexGlue command processor ignores the guest swap interval (host vblank is a
// fixed 60 Hz timer), so this is kept only for correctness of the swap packet.
bool Patch_60FPS_Byte(PPCRegister& r11) {
    // This instruction runs once per swap, which makes it the one place that
    // sees the frames the engine timer does not.
    PaceUntimedSwap();
    if (REXCVAR_GET(real_frame_delta)) {
        r11.u64 = 1; // Replaces the original value with 1 (li r11, 1)
        return true; // Skips the original instruction
    }
    return false;
}

// BadassBaboon's Recomp Adjustments: Rock-solid thread-pinned frame rate limiter
// Wall time the frame limiter spends in its PAUSE spin instead of sleeping.
std::atomic<uint64_t> g_limiter_spin_us{0};
std::atomic<uint64_t> g_limiter_spins{0};

static void EnforceFrameLimit() {
    // MCLA_FPS_CAP overrides the cvar. Read once: environment variables cannot
    // change after process start, and this runs on every single frame.
    static const int32_t env_limit = [] {
        if (const char* e = std::getenv("MCLA_FPS_CAP")) return std::atoi(e);
        return -1;
    }();
    // The pacing deadline. File-scope-static across calls, so it has to be
    // reset when pacing is off - otherwise switching FPS LIMIT to UNCAPPED and
    // back leaves a deadline minutes in the past. The catch-up clause at the
    // bottom does recover from that in one frame, but relying on it means the
    // stale value is load-bearing; clearing it here keeps the invariant simple.
    static uint64_t next_us = 0;

    const int32_t limit = (env_limit >= 0) ? env_limit : REXCVAR_GET(fps_limit);
    if (limit <= 0) {
        next_us = 0;
        return;
    }

    const double period_us = 1000000.0 / static_cast<double>(limit);

    // sub_821BDA90 has multiple callers across threads; bind limiter to main thread
    static const std::thread::id owner = std::this_thread::get_id();
    if (std::this_thread::get_id() != owner) return;

    auto now_us = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };

    uint64_t now = now_us();
    if (next_us == 0) {
        next_us = now + static_cast<uint64_t>(period_us);
        return;
    }

    if (now < next_us) {
        uint64_t remaining = next_us - now;
        const uint64_t slack_us = REXCVAR_GET(frame_limit_spin) ? 1200 : 1500;
        if (remaining > slack_us + 500) {
            std::this_thread::sleep_for(std::chrono::microseconds(remaining - slack_us));
        }
        // Sub-millisecond spin with YieldProcessor() (x86 PAUSE) to hit the exact
        // microsecond deadline. Replacing std::this_thread::yield() eliminates
        // Windows scheduler stalls that cause 1-2 ms wake-up jitter.
        const uint64_t spin_from = now_us();
        const bool spin = REXCVAR_GET(frame_limit_spin);
        while (now_us() < next_us) {
#if defined(_WIN32)
            if (spin) { YieldProcessor(); continue; }
#endif
            std::this_thread::yield();
        }
        // How much wall time this thread spent burning a core to hit the
        // deadline, as opposed to sleeping through it.
        g_limiter_spin_us.fetch_add(now_us() - spin_from, std::memory_order_relaxed);
        g_limiter_spins.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t after = now_us();
    next_us += static_cast<uint64_t>(period_us);
    if (next_us < after) next_us = after + static_cast<uint64_t>(period_us);
}

// BadassBaboon's Recomp Adjustments: Core 60 FPS Clock Delta Pipeline
// 0x821BDAB0: runs after subf r8,r10,r11 in sub_821BDA90.
// Clamps max ticks and runs precision limiter.
// Upper bound on a single frame's delta, in guest timebase ticks.
//
// Tunable via MCLA_MAX_FRAME_MS, clamped to [16, 1000] ms. Previously hardcoded
// to 125 ms while the env var was advertised in effective_config.txt but never
// read.
static uint64_t MaxFrameTicks() {
    static const uint64_t ticks = [] {
        double ms = 125.0;
        if (const char* e = std::getenv("MCLA_MAX_FRAME_MS")) {
            double v = std::atof(e);
            if (v > 0.0) ms = v;
        }
        if (ms < 16.0) ms = 16.0;
        if (ms > 1000.0) ms = 1000.0;
        uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
        if (hz == 0) hz = 50000000;
        return static_cast<uint64_t>(ms * 0.001 * static_cast<double>(hz));
    }();
    return ticks;
}

// Feeds the sampling profiler the real wall-clock frame time. Called after the
// limiter has slept, so the value is the frame the player actually saw. Costs
// one already-resolved bool test when MCLA_PROFILE is not set.
static void TickGuestProfiler() {
    if (!mc::profiler::Enabled()) return;
    static uint64_t last = 0;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (last != 0) mc::profiler::Tick(double(now - last) / 1000.0);
    last = now;
}

void MCLAFrameDelta(PPCRegister& r8) {
    // The hitch clamp runs UNCONDITIONALLY, before any cvar check.
    //
    // It is the last line of defence against an unbounded delta reaching the
    // physics and audio clocks after a streaming stall. In the midnightclub
    // fork, removing it produced a very loud audio blowout, which is why it is
    // deliberately not switchable there. Gating it behind real_frame_delta
    // meant turning that option off also silently removed crash protection -
    // the clamp is correct at 30 Hz too, since the guest's own fixed timestep
    // never exceeds it.
    const uint64_t cap = MaxFrameTicks();
    if (r8.u64 > cap) {
        r8.u64 = cap;
    }

    // EnforceFrameLimit and UpdateCityLODMemory are NOT gated on
    // real_frame_delta: they are independent settings that happen to be driven
    // from this per-frame hook. Gating them meant turning REAL FRAME DELTA off
    // also silently disabled the FPS LIMIT row and the CITY LOD slider, which
    // is the cross-setting confusion this option was renamed to avoid. The
    // midnightclub fork calls both unconditionally for the same reason.
    EnforceFrameLimit();
    g_engine_timer_ran_since_swap.store(true, std::memory_order_relaxed);
    UpdateCityLODMemory();
    RecordFrameTime();
    TickGuestProfiler();
}

// BadassBaboon's Recomp Adjustments: real delta instead of the fixed timestep.
//
// By 0x821BDAF8 sub_821BDA90 has already stored the measured unscaled delta at
// [r3+0x58] and the scaled one at [r3+0x08]. Two separate blocks downstream then
// throw that away and substitute the fixed timestep at [r3+0x20]; both have to
// be handled, and which one runs depends on [r3+0x3A] / [r3+0x3C]:
//
//   loc_821BDB58  reached when both are zero, after the +0x14 / +0x18
//                 accumulator updates. Loads [r3+0x20], and if the real delta is
//                 at least that big writes the FIXED value over +0x58 and +0x08
//                 (0x821BDB84/0x821BDB88). Skipped wholesale by jumping to
//                 loc_821BDC34 -- the accumulators are already updated by then,
//                 so nothing else is lost.
//   loc_821BDB90  reached from 0x821BDB1C / 0x821BDB28 when either flag is set.
//                 Not covered by the jump above, since it sits before it in the
//                 flow. Does the same substitution out of f11, so f11 is
//                 rewritten with the real delta instead.
//
// Returns true to take the jump. Baboon's build jumped unconditionally; gating
// it on real_frame_delta is the only change, and it makes the cvar actually turn the whole
// thing off instead of leaving half of it live.
bool MCLAUseRealDelta() {
    return REXCVAR_GET(real_frame_delta);
}

// 0x821BDB90, after `lfs f11, 0x20(r3)` has loaded the fixed timestep. f11 feeds
// both `stfs f11, 0x58(r3)` and `fmuls f0, f11, f13` -> `stfs f0, 8(r3)`, so
// replacing it with [r3+0x58] (the measured unscaled delta stored at 0x821BDAF8)
// publishes the real frame time down this path too.
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {
    if (!REXCVAR_GET(real_frame_delta)) return;
    if (TimingLogEnabled())
        g_fixedstep_hits.fetch_add(1, std::memory_order_relaxed);
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    f11.f64 = static_cast<double>(ReadGuestF32(base, static_cast<uint32_t>(r3.u64) + 0x58));
}

// Loop-entry anchor. r24 must NOT be modified: the game divides game_dt by it
// on its own (sub_821BD910) and r24 = 0 would clear the sub-tick gate at
// 0x827D754C and freeze physics.
void Patch_DeltaTime(PPCRegister& r24) {
    // Read-only: r24 must NOT be modified (see above). Publishing it costs
    // nothing and lets the timing log report the substep pass count, which is
    // the rate-invariance check - the value must not change with fps_limit.
    if (TimingLogEnabled())
        g_substep_last.store(static_cast<int32_t>(r24.s32), std::memory_order_relaxed);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_FenceSpinThrottle() { return false; }
bool Patch_60FPS_Byte(PPCRegister& r11) { return false; }
void Patch_DeltaTime(PPCRegister& r24) {}
void MCLAFrameDelta(PPCRegister& r8) {}
bool MCLAUseRealDelta() { return false; }
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {}
#endif // REXGLUE_HAS_XEO3_TARGET
