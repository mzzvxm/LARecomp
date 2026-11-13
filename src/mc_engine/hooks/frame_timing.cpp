// The 60 FPS clock: the measured frame delta fed to the simulation instead of
// the engine's fixed 30 Hz step, the hitch clamp, the frame rate limiter, the
// swap interval and the guest's D3D fence spin.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
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
#include "../camera_trace.h"
#include "../guest_profiler.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#include "gpu/latency.h"
#endif

// BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
REXCVAR_DEFINE_BOOL(real_frame_delta, true, "MCLA/Patches",
    "Feeds the simulation the measured frame time instead of the engine's fixed "
    "30 Hz timestep, and unlocks presentation from every-other-vblank. Required "
    "for correct physics, camera and traffic above 30 FPS. This is NOT a frame "
    "rate cap - see fps_limit for that.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// A number of milliseconds as written in frame_step_cap or MCLA_MAX_FRAME_MS: plain digits, a
// positive finite number, blanks around it allowed. Not clamped here (see kStepCapMinFixedMs).
static bool ParseStepCapMs(std::string_view text, double& ms) {
    const std::string copy(text);
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str()) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    if (!std::isfinite(v) || !(v > 0.0)) return false;
    ms = v;
    return true;
}

static bool IsAdaptiveStepCap(std::string_view text) {
    constexpr std::string_view kWord = "adaptive";
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.size() != kWord.size()) return false;
    for (size_t i = 0; i < kWord.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != kWord[i]) return false;
    }
    return true;
}

// How far one frame may advance the game's clock. See the step cap block below for the rule.
REXCVAR_DEFINE_STRING(frame_step_cap, "adaptive", "MCLA/Patches",
    "How far one frame may advance the game's clock after a stall (a texture-streaming hitch, a "
    "long load): the step is cut to this and the game plays that instant in slow motion, as the "
    "console did, instead of jumping ahead. adaptive (the default) is 1.5 times "
    "the recent median frame time, never below 34 ms or above 125 ms: about 34 ms at 60 FPS, 50 ms "
    "at a steady 30 FPS (so a slow PC never runs in slow motion). Or a number of milliseconds for a "
    "fixed cap, 16 to 1000 (125 was the old fixed cap). The cap cannot be turned off, and the "
    "MCLA_MAX_FRAME_MS environment variable, when set, wins (as a fixed number).")
    .validator([](std::string_view v) {
        double ms = 0.0;
        return IsAdaptiveStepCap(v) || ParseStepCapMs(v, ms);
    })
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Default to 60 FPS: with vsync=false for low input lag and fast pacing,
// the precision frame limiter caps to 60 FPS out-of-the-box.
REXCVAR_DEFINE_INT32(fps_limit, 60, "MCLA/Performance",
    "Frame rate cap in FPS (0 = uncapped). The pause menu offers 30, 40, 45, 50, 60, 120 and 144. "
    "While frame generation runs the cap counts the frames the game renders (twice as many are "
    "shown) and mcla_gpu_frame_generation_fps_limit is the cap instead.")
    .range(0, 360)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_limit_spin, true, "MCLA/Performance",
    "Hit the frame-limit deadline with a PAUSE spin. Off restores the sleep-then-yield wait, "
    "which gives the core back instead of burning it.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vsync_fast_poll, true, "MCLA/Performance",
    "Drive the SDK's gpu_vsync_fast_poll. With vsync off the guest vblank interval is 1 ms, so "
    "fast poll keeps the GPU VSync worker spinning a whole core. Off restores the 1 ms sleep.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(intro_original_speed, true, "MCLA/Patches",
    "Present the start-up legal screens and logos 30 times a second, as the console does. "
    "They are drawn by a loop that never runs the engine timer, so fps_limit never reached "
    "them and they played as fast as the host could present. The screens play before the "
    "pause menu can be opened, so a change made there is seen at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Where the frame limiter waits. The game updates on its main loop
// (sub_82131508) and presents from "[MC] Render Thread" (sub_82306238), one
// frame behind. Waiting before the update leaves the present wherever the
// render work ends it, so frames reach the screen at uneven intervals; waiting
// just before the swap presents them evenly, and the queue between the two
// threads holds the update back to the same interval. The cost is latency: the
// update then runs up to a frame ahead of what is on screen.
REXCVAR_DEFINE_BOOL(pace_at_swap, true, "MCLA/Performance",
    "Hold each frame to the FPS limit right before it is presented instead of before the "
    "game's update: even present intervals (best on a variable refresh display), at the cost "
    "of up to one more frame of input latency.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(fence_spin_throttle, true, "MCLA/Performance",
    "Yield the host CPU inside the guest's D3D fence poll (sub_82412F98) instead of running "
    "its 32 pipeline-throttle NOPs. Off restores the stock guest spin.")
    .debug_only()
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

// Set by every pass through the engine timer (sub_821BDA90, MCLAFrameDelta),
// taken by every swap.
static std::atomic<bool> g_timer_since_swap{false};

// Swaps the engine timer never saw. The legal screens and logos are drawn by a
// loop of their own (the timer's only callers are sub_822611B0 and
// sub_822C1FA8, the game loop), paced on the console by the swap interval of
// two vblanks alone, which the runtime does not honour. Such swaps are held to
// 30 a second -- from the second one in a row only: uncapped, the update and
// present threads drift, and about one swap in forty finds no timer pass before
// it while the next finds two; holding that lone swap would stutter play.
// Swap thread only. `timed`: the engine timer ran since the previous swap.
static void HoldUntimedSwap(bool timed) {
    using Clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::microseconds(1000000 / 30);
    static Clock::time_point last_swap{};
    static bool last_untimed = false;

    const bool untimed = !timed;
    if (untimed && last_untimed && REXCVAR_GET(intro_original_speed) &&
        last_swap != Clock::time_point{}) {
        const Clock::time_point due = last_swap + kPeriod;
        // Sleep to a millisecond short of it, then yield the rest away: the
        // sleep alone overshoots by up to a timer tick.
        if (due - Clock::now() > std::chrono::microseconds(1500)) {
            std::this_thread::sleep_until(due - std::chrono::milliseconds(1));
        }
        while (Clock::now() < due) {
#if defined(_WIN32)
            YieldProcessor();
#else
            std::this_thread::yield();
#endif
        }
    }
    last_untimed = untimed;
    last_swap = Clock::now();
}

// Swap-interval patch at 0x82419AA0 ("li r11, 2"): the game always requests
// D3D interval TWO (30 FPS); replacing with 1 requests 60 Hz. Note the current
// RexGlue command processor ignores the guest swap interval (host vblank is a
// fixed 60 Hz timer), so this is kept only for correctness of the swap packet.
// It runs once per swap (sub_824199B0), which makes it where the swaps the
// engine timer never saw are paced, and, with pace_at_swap, the game's frames.
static void EnforceFrameLimit();
bool Patch_60FPS_Byte(PPCRegister& r11) {
    const bool timed = g_timer_since_swap.exchange(false, std::memory_order_relaxed);
    // frame_pacing: the renderer holds the frame to its planned time instead,
    // which keeps the FPS limit too.
    const bool paced = mc::pacing::Active();
    if (timed && REXCVAR_GET(pace_at_swap) && !paced) {
        // A game frame: held to the FPS limit here instead of before its update.
        // Bound to the first thread that presents one, the render thread.
        static const std::thread::id owner = std::this_thread::get_id();
        if (std::this_thread::get_id() == owner) {
            EnforceFrameLimit();
        }
    }
#if defined(LARECOMP_HAS_GPU)
    // The swap ends a frame for the GPU vendor's latency reducer (SIMULATION_END), after the
    // limiter's wait above. Every swap, timed or not: the reducer pairs swaps with the passes
    // through the engine timer itself, and a swap it has no pass for is not marked.
    mcla::gpu::latency::OnGuestSwap();
#endif
    HoldUntimedSwap(timed);
    mc::pacing::OnGuestSwap(timed);
    mc::camtrace::OnGuestSwap(timed);
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

    int32_t limit = (env_limit >= 0) ? env_limit : REXCVAR_GET(fps_limit);
#if defined(LARECOMP_HAS_GPU)
    // With frame generation running the cap is its own, in rendered frames (the frames shown are twice
    // as many): HALF DISPLAY by default. MCLA_FPS_CAP stays the override of everything.
    if (env_limit < 0) {
        limit = mcla::gpu::FrameGenerationFpsLimit(limit);
        // DLSS frame generation's cap (the same setting; HALF DISPLAY is the display's rate over its
        // multiplier). Only one of the two runs.
        limit = mcla::gpu::DlssFrameGenerationFpsLimit(limit);
    }
#endif
    if (limit <= 0) {
        next_us = 0;
        return;
    }

    const double period_us = 1000000.0 / static_cast<double>(limit);

    // One thread per call site: the callers bind to theirs (MCLAFrameDelta,
    // PaceAtSwap), so the deadline is only ever advanced from one place at a time.

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
#if defined(_WIN32)
        // A high-resolution waitable timer (Windows 10 1803 and later) wakes a fraction of a
        // millisecond after its time, where sleep_for, on the 1 ms system timer, can be 1.5 ms late.
        // So it sleeps closer to the deadline and leaves a shorter spin (or yield) to finish on.
        // How early it wakes adapts to the PC: the slack follows the lateness seen (a slowly
        // decaying maximum, plus a margin), between 0.25 and 1.2 ms.
        static thread_local HANDLE hr_timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        static thread_local double hr_late_us = 500.0;
        if (hr_timer) {
            const uint64_t slack_us = std::clamp<uint64_t>(static_cast<uint64_t>(hr_late_us) + 150, 250, 1200);
            if (remaining > slack_us + 100) {
                const uint64_t target = next_us - slack_us;
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((remaining - slack_us) * 10);  // 100 ns units, relative
                if (SetWaitableTimerEx(hr_timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
                    WaitForSingleObject(hr_timer, 50) == WAIT_OBJECT_0) {
                    const uint64_t woke = now_us();
                    const double late = woke > target ? static_cast<double>(woke - target) : 0.0;
                    hr_late_us = std::max(late, hr_late_us * 0.98);
                }
            }
        } else
#endif
        {
#if defined(_WIN32)
            // The 1 ms system timer wakes sleep_for up to 1.5 ms late: sleep to well before the
            // deadline and finish on the spin below.
            const uint64_t slack_us = REXCVAR_GET(frame_limit_spin) ? 1200 : 1500;
#else
            // nanosleep wakes within about 0.1 ms of its time, so the sleep goes to 0.3 ms before
            // the deadline (0.6 without the spin) and the core is only burned for the last stretch.
            // CHECK: not run on Linux (frame times at a 60 FPS cap, and the CPU use, against 1.2 ms).
            const uint64_t slack_us = REXCVAR_GET(frame_limit_spin) ? 300 : 600;
#endif
            if (remaining > slack_us + 500) {
                std::this_thread::sleep_for(std::chrono::microseconds(remaining - slack_us));
            }
        }
        // Sub-millisecond spin with YieldProcessor() (x86 PAUSE) to hit the exact
        // microsecond deadline. Replacing std::this_thread::yield() eliminates
        // Windows scheduler stalls that cause 1-2 ms wake-up jitter.
        const uint64_t spin_from = now_us();
        const bool spin = REXCVAR_GET(frame_limit_spin);
        while (now_us() < next_us) {
#if defined(_WIN32)
            if (spin) { YieldProcessor(); continue; }
#elif defined(__x86_64__)
            if (spin) { _mm_pause(); continue; }
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
//
// The step cap: the upper bound on a single frame's delta, in guest timebase ticks.
//
// A texture-streaming stall holds the game for 70 to 230 ms and the next frame's delta is then the
// whole stall. The chase camera is tuned for the console's fixed 33 ms step and lags behind the car
// when one step is that long, so with the old fixed cap of 125 ms the camera dipped for a frame or
// two after a stall and came back (a 60 FPS capture, frame by frame, showed each dip right after a
// stall, and capping the step near 34 ms removed them). A cap that low would make a slow PC (a
// steady 30 FPS, a Steam Deck) run in slow motion, though, so the cap follows the frame rate:
//
//   adaptive (the default)   min(125 ms, max(34 ms, 1.5 x the median of the last 30 frames' deltas))
//   a number of ms           that fixed cap (125 is what this used to be), clamped to 16..1000
//
// At 60 FPS the median is 16.7 ms and the cap its floor, 34 ms, the value the stall frames needed. At
// a steady 30 FPS it is about 50 ms, so no frame is cut and nothing runs slow; at 12 FPS or less it
// is the old 125 ms. An isolated stall is cut to the cap and played in slow motion, as on the
// console. The median is over the UNCAPPED deltas, so the stalls themselves do not raise it (it takes
// more than half the window being slow to move it), and it needs 8 deltas before it is trusted: until
// then the cap is the 125 ms.
//
// The cap is set by frame_step_cap; MCLA_MAX_FRAME_MS (a number of ms) wins over it, as it did when it
// was the only way. Either way the cap is unconditional (see MCLAFrameDelta).
constexpr double kStepCapFloorMs = 34.0;
constexpr double kStepCapCeilingMs = 125.0;
constexpr double kStepCapMedianFactor = 1.5;
constexpr size_t kStepCapWindow = 30;
constexpr size_t kStepCapMinSamples = 8;
// A fixed cap is held to this range, in ms.
constexpr double kStepCapMinFixedMs = 16.0;
constexpr double kStepCapMaxFixedMs = 1000.0;

static double GuestTicksPerMs() {
    static const double per_ms = [] {
        uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
        if (hz == 0) hz = 50000000;
        return static_cast<double>(hz) * 0.001;
    }();
    return per_ms;
}

// The cap in force after the last frame the game thread timed, in guest ticks; 0 before the first.
// The other threads that pass through the engine timer take theirs from here.
static std::atomic<uint64_t> g_step_cap_ticks{0};

// Game thread only: takes this frame's uncapped delta (guest ticks) and returns the cap, in ticks,
// to apply to it.
static uint64_t StepCapForFrame(uint64_t measured) {
    // The deltas of the last frames, in a ring.
    static uint64_t ring[kStepCapWindow] = {};
    static size_t ring_head = 0;
    static size_t ring_count = 0;

    // MCLA_MAX_FRAME_MS: a fixed cap in ms that wins over the cvar (0: not set). Read once;
    // environment variables cannot change after the process starts.
    static const double env_ms = [] {
        double ms = 0.0;
        if (const char* e = std::getenv("MCLA_MAX_FRAME_MS")) {
            if (!ParseStepCapMs(e, ms)) ms = 0.0;
        }
        return ms;
    }();

    // The mode, worked out again whenever the cvar's text changes (and logged then).
    static bool mode_known = false;
    static std::string mode_text;
    static bool adaptive = true;
    static double fixed_ms = kStepCapCeilingMs;

    const std::string& text = REXCVAR_GET(frame_step_cap);
    if (!mode_known || text != mode_text) {
        mode_known = true;
        mode_text = text;
        double requested_ms = 0.0;
        if (env_ms > 0.0) {
            adaptive = false;
            requested_ms = env_ms;
        } else if (IsAdaptiveStepCap(text)) {
            adaptive = true;
        } else if (ParseStepCapMs(text, requested_ms)) {
            adaptive = false;
        } else {
            // The validator keeps this out of the cvar; a value that got in anyway is not a reason
            // to run without a cap.
            adaptive = true;
            REXLOG_WARN("Frame step cap: '{}' is neither adaptive nor a number of ms, using adaptive",
                        text);
        }
        const char* source = env_ms > 0.0 ? "MCLA_MAX_FRAME_MS" : "frame_step_cap";
        if (adaptive) {
            REXLOG_INFO("Frame step cap: adaptive (between {:.0f} and {:.0f} ms, {:.1f} x the recent "
                        "median frame time)",
                        kStepCapFloorMs, kStepCapCeilingMs, kStepCapMedianFactor);
        } else {
            fixed_ms = std::clamp(requested_ms, kStepCapMinFixedMs, kStepCapMaxFixedMs);
            if (fixed_ms != requested_ms) {
                REXLOG_INFO("Frame step cap: fixed {:.0f} ms (from {}; {} ms is outside {:.0f}..{:.0f})",
                            fixed_ms, source, requested_ms, kStepCapMinFixedMs, kStepCapMaxFixedMs);
            } else {
                REXLOG_INFO("Frame step cap: fixed {:.0f} ms (from {})", fixed_ms, source);
            }
        }
    }

    // A zero delta is not a frame time (it would pull the median down for nothing).
    if (measured > 0) {
        ring[ring_head] = measured;
        ring_head = (ring_head + 1) % kStepCapWindow;
        if (ring_count < kStepCapWindow) ++ring_count;
    }

    double cap_ms = fixed_ms;
    if (adaptive) {
        cap_ms = kStepCapCeilingMs;
        if (ring_count >= kStepCapMinSamples) {
            uint64_t sorted[kStepCapWindow];
            std::copy(ring, ring + ring_count, sorted);
            uint64_t* middle = sorted + ring_count / 2;
            std::nth_element(sorted, middle, sorted + ring_count);
            const double median_ms = static_cast<double>(*middle) / GuestTicksPerMs();
            cap_ms = std::clamp(kStepCapMedianFactor * median_ms, kStepCapFloorMs, kStepCapCeilingMs);
        }
    }
    const uint64_t cap = static_cast<uint64_t>(cap_ms * GuestTicksPerMs());
    g_step_cap_ticks.store(cap, std::memory_order_relaxed);
    return cap;
}

// A stall that was cut, for the debug log: one line, then at most one more a second, saying how
// many were cut in between. Game thread only.
static void LogCappedStep(uint64_t measured, uint64_t cap) {
    static uint64_t last_log_us = 0;
    static uint32_t skipped = 0;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (last_log_us != 0 && now - last_log_us < 1000000) {
        ++skipped;
        return;
    }
    REXLOG_DEBUG("Frame step capped: {:.1f} ms measured, {:.1f} ms applied ({} more cut since the last "
                 "line)",
                 static_cast<double>(measured) / GuestTicksPerMs(),
                 static_cast<double>(cap) / GuestTicksPerMs(), skipped);
    last_log_us = now;
    skipped = 0;
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

// 0x821BDA90, the engine timer's first instruction: where a frame starts for the GPU vendor's
// latency reducer (NVIDIA Reflex, AMD Anti-Lag 2; gpu/latency.h). Its wait comes here, before the
// timer reads the clock (so the time step the timer then measures is the one after the wait, not
// a gap between two steps) and before anything in the frame reads the input; SIMULATION_START and
// INPUT_SAMPLE follow it. sub_821BDA90 has callers on more than one thread: the frame is the
// first one's, the game loop's, the same thread MCLAFrameDelta binds its limiter to. The timer's
// own retry (it jumps back to this address when the low half of the time it read was 0, about
// once in 2^32 reads) is the same frame: a pass less than 100 us after the last does nothing.
void MCLAFrameStart() {
#if defined(LARECOMP_HAS_GPU)
    static const std::thread::id frame_thread = std::this_thread::get_id();
    static uint64_t last_start_us = 0;  // the frame thread's
    if (std::this_thread::get_id() != frame_thread) {
        return;
    }
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - last_start_us < 100) {
        return;
    }
    mcla::gpu::latency::OnFrameStart();
    // After the driver's wait.
    last_start_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

void MCLAFrameDelta(PPCRegister& r8) {
    // frame_pacing: the game clock steps by the interval its frame will be on
    // screen for (frame_pacing.cpp), not by the time since the last read.
    uint64_t planned = 0;
    if (mc::pacing::TakePlannedTicks(planned)) {
        r8.u64 = planned;
    }

    // The hitch clamp runs UNCONDITIONALLY, before any cvar check.
    //
    // It is the last line of defence against an unbounded delta reaching the
    // physics and audio clocks after a streaming stall. In the midnightclub
    // fork, removing it produced a very loud audio blowout, which is why it is
    // deliberately not switchable there. Gating it behind real_frame_delta
    // meant turning that option off also silently removed crash protection -
    // the clamp is correct at 30 Hz too, since the guest's own fixed timestep
    // never exceeds it. How high it is set is the step cap's business (above);
    // frame_step_cap can move it between 16 ms and 1 s, never remove it.
    //
    // sub_821BDA90 has callers on more than one thread: the limiter and the
    // step cap's history are bound to the first, the game loop. The others use
    // the cap it last worked out.
    static const std::thread::id owner = std::this_thread::get_id();
    const bool game_thread = std::this_thread::get_id() == owner;
    uint64_t cap;
    if (game_thread) {
        cap = StepCapForFrame(r8.u64);
    } else {
        cap = g_step_cap_ticks.load(std::memory_order_relaxed);
        if (cap == 0) cap = static_cast<uint64_t>(kStepCapCeilingMs * GuestTicksPerMs());
    }
    if (r8.u64 > cap) {
        if (game_thread) LogCappedStep(r8.u64, cap);
        r8.u64 = cap;
    }

    // EnforceFrameLimit and UpdateCityLODMemory are NOT gated on
    // real_frame_delta: they are independent settings that happen to be driven
    // from this per-frame hook. Gating them meant turning REAL FRAME DELTA off
    // also silently disabled the FPS LIMIT row and the CITY LOD slider, which
    // is the cross-setting confusion this option was renamed to avoid. The
    // midnightclub fork calls both unconditionally for the same reason.
    // With pace_at_swap the limiter waits at the swap instead (Patch_60FPS_Byte).
    if (!REXCVAR_GET(pace_at_swap) && !mc::pacing::Active() && game_thread) {
        EnforceFrameLimit();
    }
    g_timer_since_swap.store(true, std::memory_order_relaxed);  // see HoldUntimedSwap
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
