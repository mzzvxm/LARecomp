// Frame pacing: every frame stays on screen for exactly the time step the game
// simulated it with.
//
// Why the camera jumped above 30 FPS (cam_trace, fps_limit 120, driving: about
// 95 FPS, frame times from 7.5 to 19 ms and more):
//
// The game's frame (default.xex):
//   sub_82131508, the main loop: sub_822C1FA8 updates the game -- the clock
//     (sub_821BDA90 on the game clock 0x827D7500), the physics in substeps of the
//     clock's step, the cameras -- then sub_82305D00 hands the frame over: it
//     waits for the render thread to finish the frame it was given last
//     (sub_823057E8, "render done"), swaps the frame buffers (sub_82305B38) and
//     signals the render thread to draw the update that just ran.
//   sub_82305E38, "[MC] Render Thread": waits for the hand-over, draws, ends with
//     D3DDevice_Swap, signals render done.
// So update k+1 runs while the render thread draws frame k.
//
// The time step of update k is the time between the clock reads that start
// updates k-1 and k (MCLAFrameDelta). When the game does not hold the FPS LIMIT
// -- the update itself is the slowest part -- frame k is swapped right after
// update k+1 starts, plus the render thread's time: it is on screen for about as
// long as update k took, but was simulated with the time update k-1 took.
// Driving, consecutive frame times differ by several ms, so the world moved by
// the wrong amount for the time each frame was shown, back and forth: the camera
// "jumping". Measured: 67-71% of frames within 1 ms of their step, 8-12 errors
// over 2 ms a second, 2.5 ms rms; at 70 m/s a 5 ms error is 35 cm.
//
// The pacer plans, when an update starts, the time its frame reaches the screen,
//
//     T(k) = max(T(k-1) + the FPS LIMIT period, now + L),
//
// has the update simulate exactly T(k) - T(k-1), and the Plume renderer presents
// the frame at T(k) (gpu.h SetNextPresentTarget). The renderer submits the frame's
// GPU work and releases the guest as before; only the swap chain's present waits,
// done by the renderer's own loop between the next frame's records when the time
// comes -- so neither the game's threads, nor the renderer's work, nor the GPU
// wait on it, and the GPU has usually finished the frame by then. Without an FPS LIMIT
// reached, T(k) - T(k-1) is the measured frame time, as before; what changes is
// that the frame is shown for exactly that long. L, the latency from an update's
// start to its frame on screen, follows how late frames arrived (ready after
// their time, or DXGI's frame statistics): up when one is late, slowly down while
// none is.
//
// Measured with cam_trace, the frame's time on screen against its step:
//   free-fly camera at 70 m/s, FPS LIMIT 120 held: within 1 ms 77% -> 98-99%,
//     errors over 2 ms 12.6/s -> 0.4-1/s, camera position error p99 51 -> 2-11 cm.
//   the same at FPS LIMIT 240, not reached (~130 FPS): 86-88% -> 97-99%,
//     6-9/s -> 0.6-1.8/s.
//   driving at FPS LIMIT 120, not reached (~95 FPS): 64-67% -> 97%, 14-15/s -> 1-2/s.
// The frame rate stayed within the run-to-run spread (+-5 FPS). Online sessions
// keep the game's own ten-frame average of the step (sub_822C1FA8 when
// sub_82387A18 is set), so there the step is the average of the planned ones.
//
// The plan travels with the game's own hand-over: the update that just ran is the
// one the render thread draws next, and sub_82305B38 runs between the render-done
// wait and the signal, while the render thread is idle -- so T(k) is handed over
// there and read back at that frame's swap (Patch_60FPS_Byte, frame_timing.cpp).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

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
#include "../logging.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DEFINE_INT32(frame_pacing, 1, "MCLA/Performance",
    "1: every frame stays on screen for exactly the time step the game simulated it with -- the "
    "renderer holds a frame that is ready early until its planned time, and the game steps by the "
    "planned interval. Stops the camera's back-and-forth jumps when the game does not hold the FPS "
    "LIMIT, for a few milliseconds of latency. 0: the step is the measured frame time and frames are "
    "shown when ready, as before. Needs the Plume renderer, and is idle with mcla_gpu_vsync.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, real_frame_delta);
REXCVAR_DECLARE(int32_t, fps_limit);

REX_EXTERN(__imp__rex_sub_821BDA90);  // clock update (vtable slot 2 of the clock objects)
REX_EXTERN(__imp__rex_sub_82305B38);  // the hand-over's frame-buffer swap

namespace {

constexpr uint32_t kGameClock = 0x827D7500;

// L, the latency from an update's start to its frame's time on screen.
//
// Each frame reports when it was ready (submitted) against its time; with the
// update's start that is the frame's natural latency. L is the 98th percentile
// of the last 128 of them plus a margin -- so about two frames in a hundred
// arrive late, and the renderer carries their lateness into the frames after
// them instead of jumping back (present_schedule.h) -- plus what the
// presentation's frame statistics show the GPU still adding after the hold.
//
// And L stays under the median latency plus one step. A frame held longer than
// that holds up the next one: the renderer presents the held frame before it
// starts the next present, and the guest waits at its swap for that one. The
// game would then run fewer frames per second, and its frames would measure
// later and push L further (measured: an L that only followed lateness ran up
// and cost 15% of the frames). Under that bound a held-up frame measures as
// late as L itself, so the bound comes back down instead.
constexpr double kLatencyStartMs = 20.0;
constexpr double kLatencyMinMs = 4.0;
constexpr double kLatencyMaxMs = 45.0;
constexpr double kNeedMarginMs = 0.5;       // on top of the 97th percentile
constexpr double kCouplingMarginMs = 1.5;   // under the median plus one step
constexpr double kRiseMsPerFrame = 1.0;     // L moves up at most this a frame
constexpr double kFallMsPerFrame = 0.1;     // and down at most this
constexpr double kStepAverage = 0.05;       // weight of a new step in the recent step's average
constexpr uint32_t kLatencySamples = 128;
constexpr uint32_t kLatencySamplesToDecide = 16;
constexpr double kGpuLateDecay = 0.995;     // per frame, the GPU's extra lateness fades

int64_t Qpc() {
#if defined(_WIN32)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
#else
    return 0;
#endif
}

double QpcPerMs() {
#if defined(_WIN32)
    static const double per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return per_ms;
#else
    return 1.0;
#endif
}

int32_t FpsLimit() {
    // The same override EnforceFrameLimit honours.
    static const int32_t env_limit = [] {
        if (const char* e = std::getenv("MCLA_FPS_CAP")) return std::atoi(e);
        return -1;
    }();
    return env_limit >= 0 ? env_limit : REXCVAR_GET(fps_limit);
}

bool Wanted() {
#if defined(LARECOMP_HAS_GPU)
    return REXCVAR_GET(frame_pacing) != 0 && REXCVAR_GET(real_frame_delta) &&
           mcla::gpu::SchedulesPresents();
#else
    return false;
#endif
}

// Update thread.
struct Plan {
    bool active = false;
    int64_t target = 0;   // T of the last frame planned
    double latency = 0;   // L, QPC ticks
    double step = 0;      // the recent step, averaged, QPC ticks
    // The frames planned lately: their time and their update's start, to turn
    // the renderer's reports into latencies.
    struct Planned {
        int64_t target = 0;
        int64_t start = 0;
    };
    Planned planned[64] = {};
    uint32_t planned_next = 0;
    // Natural latencies (ms), a ring.
    float samples[kLatencySamples] = {};
    uint32_t sample_count = 0;
    uint32_t sample_next = 0;
    double gpu_late_ms = 0.0;  // what the frame statistics show the GPU adding
    double need_ms = 0.0;      // the last decision, for the log
    double bound_ms = 0.0;
    int64_t last_log = 0;
};
Plan g_plan;
// The clock read being planned: its step in guest timebase ticks, for MCLAFrameDelta.
thread_local bool t_game_clock = false;
bool g_have_step = false;
uint64_t g_step_ticks = 0;
// T of the update in progress, then of the frame the render thread draws.
int64_t g_update_target = 0;
std::atomic<int64_t> g_render_target{0};
std::atomic<bool> g_active{false};

// The update that planned `target` started at; 0 when it is no longer known.
int64_t StartOf(int64_t target) {
    for (const Plan::Planned& p : g_plan.planned) {
        if (p.target == target) return p.start;
    }
    return 0;
}

// Reads the renderer's reports and moves L toward what the frames need.
void Learn() {
    const double ms = QpcPerMs();
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::PresentReport reports[64];
    const uint32_t n = mcla::gpu::TakePresentReports(reports, 64);
    for (uint32_t i = 0; i < n; ++i) {
        const mcla::gpu::PresentReport& r = reports[i];
        if (r.on_screen) {
            // Late on screen past the flip: the GPU had not finished at the hold's end.
            const double late_ms = static_cast<double>(r.late) / ms;
            if (late_ms > 0.5 && late_ms < 20.0) g_plan.gpu_late_ms = std::max(g_plan.gpu_late_ms, late_ms);
            continue;
        }
        const int64_t start = StartOf(r.target);
        if (start == 0) continue;
        const double latency_ms = static_cast<double>(r.target + r.late - start) / ms;
        if (latency_ms <= 0.0 || latency_ms > 250.0) continue;  // a load, not a frame
        g_plan.samples[g_plan.sample_next] = static_cast<float>(latency_ms);
        g_plan.sample_next = (g_plan.sample_next + 1) % kLatencySamples;
        if (g_plan.sample_count < kLatencySamples) ++g_plan.sample_count;
    }
#endif
    g_plan.gpu_late_ms *= kGpuLateDecay;
    if (g_plan.sample_count < kLatencySamplesToDecide) return;

    float sorted[kLatencySamples];
    std::copy(g_plan.samples, g_plan.samples + g_plan.sample_count, sorted);
    std::sort(sorted, sorted + g_plan.sample_count);
    const double p50 = sorted[g_plan.sample_count / 2];
    const double p98 = sorted[(g_plan.sample_count * 98) / 100];
    const double need = p98 + kNeedMarginMs + g_plan.gpu_late_ms;
    double bound = kLatencyMaxMs;
    if (g_plan.step > 0.0) bound = std::min(bound, p50 + g_plan.step / ms - kCouplingMarginMs);
    const double wanted = std::clamp(std::min(need, bound), kLatencyMinMs, kLatencyMaxMs) * ms;
    const double rise = kRiseMsPerFrame * ms;
    const double fall = kFallMsPerFrame * ms;
    g_plan.latency += std::clamp(wanted - g_plan.latency, -fall, rise);
    g_plan.need_ms = need;
    g_plan.bound_ms = bound;
}

// One line every half minute: what the plan settled on.
void LogPlan(int64_t now) {
    const double ms = QpcPerMs();
    if (g_plan.last_log != 0 && static_cast<double>(now - g_plan.last_log) < 30000.0 * ms) return;
    if (g_plan.last_log != 0) {
        MC_INFO("[pacing] latency {:.1f} ms (frames need {:.1f}, bound {:.1f}), step {:.2f} ms, GPU "
                "past the hold {:.2f} ms, {} latency samples",
                g_plan.latency / ms, g_plan.need_ms, g_plan.bound_ms, g_plan.step / ms,
                g_plan.gpu_late_ms, g_plan.sample_count);
    }
    g_plan.last_log = now;
}

// Plans the update about to start at `now`; returns `now`.
int64_t PlanFrame(int64_t now) {
    g_have_step = false;
    if (!Wanted()) {
        g_plan = {};
        g_update_target = 0;
        g_active.store(false, std::memory_order_relaxed);
        return now;
    }
    const double ms = QpcPerMs();
    if (g_plan.latency <= 0.0) g_plan.latency = kLatencyStartMs * ms;
    Learn();
    LogPlan(now);

    const int32_t limit = FpsLimit();
    // No limit: at least a millisecond apart.
    const int64_t period = static_cast<int64_t>(limit > 0 ? 1000.0 * ms / limit : ms);
    int64_t target = now + static_cast<int64_t>(g_plan.latency);
    if (g_plan.active) {
        // Ahead of the FPS LIMIT the frame simply waits for its turn: the renderer
        // holds it, and the game is held back by the renderer's own queue (its
        // next present waits for this one). Holding the update back here instead,
        // to start it only L before its turn, was measured worse: frames were
        // ready later and less evenly (p97 28 against 17 ms) and 15% fewer of them.
        target = std::max(target, g_plan.target + period);
        const double step = static_cast<double>(target - g_plan.target);
        // A load or a stall is not a frame time to bound the latency by.
        if (step < 250.0 * ms) {
            g_plan.step = g_plan.step > 0.0 ? g_plan.step + kStepAverage * (step - g_plan.step) : step;
        }
        const double step_ms = step / ms;
        // Into guest timebase ticks; MCLAFrameDelta's hitch clamp still applies.
        const double hz = static_cast<double>(rex::chrono::Clock::guest_tick_frequency());
        g_step_ticks = static_cast<uint64_t>(step_ms * 0.001 * hz + 0.5);
        g_have_step = g_step_ticks > 0;
    }
    g_plan.target = target;
    g_plan.active = true;
    g_plan.planned[g_plan.planned_next] = {target, now};
    g_plan.planned_next = (g_plan.planned_next + 1) % 64;
    g_update_target = target;
    g_active.store(true, std::memory_order_relaxed);
    return now;
}

}  // namespace

namespace mc::pacing {

bool TakePlannedTicks(uint64_t& ticks) {
    if (!t_game_clock || !g_have_step) return false;
    ticks = g_step_ticks;
    return true;
}

bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

void OnGuestSwap(bool timed) {
#if defined(LARECOMP_HAS_GPU)
    if (!timed || !Active()) return;
    mcla::gpu::SetNextPresentTarget(g_render_target.load(std::memory_order_acquire));
#else
    (void)timed;
#endif
}

void TraceState(int64_t& target, double& latency_ms) {
    target = g_update_target;
    latency_ms = g_plan.latency / QpcPerMs();
}

}  // namespace mc::pacing

// The clock update. The game clock's call is the start of a game update: it is
// planned here, before the original reads the timebase, and MCLAFrameDelta takes
// the planned step from inside it.
extern "C" REX_FUNC(rex_sub_821BDA90) {
    if (ctx.r3.u32 != kGameClock) {
        __imp__rex_sub_821BDA90(ctx, base);
        return;
    }
    const int64_t now = PlanFrame(Qpc());
    mc::camtrace::NoteGameClock(now);
    t_game_clock = true;
    __imp__rex_sub_821BDA90(ctx, base);
    t_game_clock = false;
}

// The hand-over's frame-buffer swap: the render thread is idle and draws the
// update that just ran next.
extern "C" REX_FUNC(rex_sub_82305B38) {
    g_render_target.store(g_update_target, std::memory_order_release);
    __imp__rex_sub_82305B38(ctx, base);
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <cstdint>

namespace mc::pacing {
bool TakePlannedTicks(uint64_t&) { return false; }
bool Active() { return false; }
void OnGuestSwap(bool) {}
void TraceState(int64_t& target, double& latency_ms) {
    target = 0;
    latency_ms = 0;
}
}  // namespace mc::pacing

#endif  // REXGLUE_HAS_XEO3_TARGET
