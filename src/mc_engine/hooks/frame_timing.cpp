// The 60 FPS clock: the measured frame delta fed to the simulation instead of
// the engine's fixed 30 Hz step, the hitch clamp, the frame rate limiter, the
// swap interval and the guest's D3D fence spin.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>
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
#include "../logging.h"

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

REXCVAR_DEFINE_BOOL(fps_limit_auto, false, "MCLA/Performance",
    "Pace frames at the highest rate the machine holds steadily, chosen from the display's "
    "refresh divided by a whole number (144 Hz: 72, 48, 36...). Drops a step when frames "
    "run long and climbs back only after ~2 s of headroom, so frame times stay even and moving "
    "things do not judder. fps_limit (or MCLA_FPS_CAP), when above 0, is the highest rate it "
    "may pick. Pointless on a variable refresh display.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(fps_limit_auto_refresh_hz, 0, "MCLA/Performance",
    "Display refresh rate for fps_limit_auto. 0 = ask Windows for the primary display's "
    "rate once at startup (other platforms assume 60).")
    .range(0, 500)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_delta_after_limit, true, "MCLA/Performance",
    "Measure the frame delta after the frame limiter's wait instead of before it, so the "
    "simulated step matches the interval frames are shown at. Off feeds the guest's own "
    "delta, which it reads before the limiter sleeps. Has no effect when uncapped.")
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

// Swap-interval patch at 0x82419AA0 ("li r11, 2"): the game always requests
// D3D interval TWO (30 FPS); replacing with 1 requests 60 Hz. Note the current
// RexGlue command processor ignores the guest swap interval (host vblank is a
// fixed 60 Hz timer), so this is kept only for correctness of the swap packet.
bool Patch_60FPS_Byte(PPCRegister& r11) {
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

// fps_limit_auto: the refresh period times a whole number of refreshes per
// frame. Every frame then lasts the same number of refreshes and simulates the
// same step, which is what a steady 30 FPS cap gets right and an uneven 40-60
// gets wrong: each frame moves the world by the previous frame's length, so
// frame times jumping between 17 and 34 ms make everything lurch.
#if defined(_WIN32)
// The primary display's refresh rate. QueryDisplayConfig gives it as a
// fraction (59.94 Hz is 60000/1001); EnumDisplaySettingsW rounds it down to
// whole hertz, and pacing at 59 on a 59.94 Hz display with vsync on shows one
// frame twice about every second. 0 if neither answers.
static double PrimaryDisplayRefreshHz() {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) ==
        ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                               modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < path_count; ++i) {
                const UINT32 src = paths[i].sourceInfo.modeInfoIdx;
                if (src >= mode_count ||
                    modes[src].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    continue;
                }
                // The primary display is the one at the desktop origin.
                const POINTL pos = modes[src].sourceMode.position;
                const DISPLAYCONFIG_RATIONAL rate = paths[i].targetInfo.refreshRate;
                if (pos.x == 0 && pos.y == 0 && rate.Numerator != 0 && rate.Denominator != 0) {
                    return static_cast<double>(rate.Numerator) / rate.Denominator;
                }
            }
        }
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        return static_cast<double>(dm.dmDisplayFrequency);
    }
    return 0.0;
}
#endif

static double RefreshPeriodUs() {
    const int32_t forced = REXCVAR_GET(fps_limit_auto_refresh_hz);
    if (forced > 0) return 1000000.0 / forced;
    // Read once: a refresh rate change while the game runs is not picked up.
    static const double detected = [] {
        double hz = 0.0;
#if defined(_WIN32)
        hz = PrimaryDisplayRefreshHz();
#endif
        if (hz < 1.0) {
            hz = 60.0;
            MC_INFO("[frame-timing] fps_limit_auto: display refresh unknown, assuming 60 Hz; "
                    "set fps_limit_auto_refresh_hz");
        } else {
            MC_INFO("[frame-timing] fps_limit_auto: display refresh {:.3f} Hz", hz);
        }
        return 1000000.0 / hz;
    }();
    return detected;
}

// Called only from EnforceFrameLimit, after its owner-thread check, so the
// state below belongs to one thread. `work_us`: how long the frame that just
// reached the limiter took, release to arrival, or 0 if there was no previous
// release to measure from. `min_period_us`: fps_limit's period, or 0.
static double AutoPacePeriod(uint64_t work_us, double min_period_us) {
    constexpr uint32_t kMaxRefreshesPerFrame = 64;
    static uint32_t n = 0;              // refreshes per frame
    static uint32_t overrun_mask = 0;   // last 10 frames, 1 = ran long
    static uint32_t headroom_frames = 0;
    // The game thread's own time says little about the renderer: at the faster
    // step the renderer is what runs out and the game thread only then waits on
    // it. So a step up that falls back within 5 s makes the next try at that
    // rate wait four times longer (2 s, 8 s, 32 s, then 60 s). Indexed by the
    // refreshes per frame being climbed to, minus one.
    static std::array<double, kMaxRefreshesPerFrame> climb_wait_us = [] {
        std::array<double, kMaxRefreshesPerFrame> w;
        w.fill(2e6);
        return w;
    }();
    static uint64_t frames_since_climb = ~0ull;  // ~0: no climb to blame
    const double base = RefreshPeriodUs();
    uint32_t n_min = 1;
    while (n_min * base < min_period_us * 0.999) ++n_min;
    // The slowest step it drops to is about 10 fps, unless fps_limit asks for
    // slower still. Clamping n also covers fps_limit rising while n sits above
    // the new n_min.
    const uint32_t n_max = std::max(
        n_min, std::clamp(static_cast<uint32_t>(100000.0 / base), 1u, kMaxRefreshesPerFrame));
    n = std::clamp(n, n_min, n_max);
    if (work_us == 0) return n * base;
    // A frame too long for even the slowest step says nothing about which step
    // fits: a load or streaming stall. Counted as an overrun, a few of those in
    // a row would drop the rate and leave it to climb back at 2 s per step.
    if (work_us > n_max * base) {
        headroom_frames = 0;
        return n * base;
    }
    const bool overran = work_us > n * base * 1.02;
    overrun_mask = ((overrun_mask << 1) | (overran ? 1u : 0u)) & 0x3FFu;
    if (frames_since_climb != ~0ull) ++frames_since_climb;
    if (std::popcount(overrun_mask) >= 3 && n < n_max) {
        if (frames_since_climb != ~0ull && frames_since_climb * n * base < 5e6) {
            climb_wait_us[n - 1] = std::min(climb_wait_us[n - 1] * 4.0, 60e6);
        }
        frames_since_climb = ~0ull;
        ++n;
        overrun_mask = 0;
        headroom_frames = 0;
        MC_INFO("[frame-timing] fps_limit_auto: down to {:.1f} fps", 1000000.0 / (n * base));
    } else if (n > n_min && work_us < (n - 1) * base * 0.75) {
        // Frames that would also fit the faster step, for long enough.
        if (++headroom_frames * n * base >= climb_wait_us[n - 2]) {
            --n;
            frames_since_climb = 0;
            overrun_mask = 0;
            headroom_frames = 0;
            MC_INFO("[frame-timing] fps_limit_auto: up to {:.1f} fps", 1000000.0 / (n * base));
        }
    } else {
        headroom_frames = 0;
    }
    return n * base;
}

// Returns the wall time, in microseconds, between the moments the limiter
// released the previous frame and this one. 0 when it did not pace this frame:
// uncapped, another thread, or the first frame after the cap turned on.
static uint64_t EnforceFrameLimit() {
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
    // When the previous paced frame was released. Only read once next_us is
    // set, and set together with it, so it cannot go stale across an
    // UNCAPPED period either.
    static uint64_t last_release_us = 0;

    const int32_t limit = (env_limit >= 0) ? env_limit : REXCVAR_GET(fps_limit);
    const bool auto_pace = REXCVAR_GET(fps_limit_auto);
    if (limit <= 0 && !auto_pace) {
        next_us = 0;
        return 0;
    }

    double period_us = limit > 0 ? 1000000.0 / static_cast<double>(limit) : 0.0;

    // sub_821BDA90 has multiple callers across threads; bind limiter to main thread
    static const std::thread::id owner = std::this_thread::get_id();
    if (std::this_thread::get_id() != owner) return 0;

    auto now_us = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };

    uint64_t now = now_us();
    if (auto_pace) {
        static double last_period_us = 0.0;
        period_us = AutoPacePeriod(next_us != 0 ? now - last_release_us : 0, period_us);
        // A new step re-plans the current frame's deadline from its release.
        // The period is truncated to whole microseconds here and below, which
        // keeps the pacer a fraction of a microsecond per frame ahead of the
        // display rather than behind it.
        if (next_us != 0 && period_us != last_period_us) {
            next_us = last_release_us + static_cast<uint64_t>(period_us);
        }
        last_period_us = period_us;
    }
    if (next_us == 0) {
        next_us = now + static_cast<uint64_t>(period_us);
        last_release_us = now;
        return 0;
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
    const uint64_t interval_us = after - last_release_us;
    last_release_us = after;
    return interval_us;
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
    //
    // The guest read its timer at the top of sub_821BDA90, before the limiter
    // sleeps, so its delta is period + this frame's work - the previous
    // frame's work. Frames reach the screen evenly, but each one simulates a
    // step that is off by however much the workload changed, and everything
    // moving shuffles back and forth. Measured at a 30 FPS cap the machine
    // holds easily: steps of 27-40 ms. The interval between the limiter's
    // releases is the interval the frames are shown at, so that goes to the
    // guest instead. Over many frames both sum to the same wall time; only
    // the per-frame split differs. Uncapped there is no wait to measure
    // around, and the guest's own delta stays.
    const uint64_t released_us = EnforceFrameLimit();
    if (released_us != 0 && REXCVAR_GET(frame_delta_after_limit)) {
        uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
        if (hz == 0) hz = 50000000;
        const uint64_t ticks = released_us * hz / 1000000u;
        r8.u64 = ticks > cap ? cap : ticks;
    }
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
