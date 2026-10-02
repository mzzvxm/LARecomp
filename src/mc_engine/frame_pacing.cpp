// Frame pacing for the unlocked frame rate.
//
// The policy itself, and why the measured frame time is the wrong step to
// simulate, is in frame_pacing_policy.h. This file wires it into the game:
//
// - sub_821BDA90 (the clock update) is wrapped so the game's own clock can be
//   told apart from the other clock objects the same code updates, and so the
//   frame limiter can sleep right before the clock reads the timebase instead
//   of right after it. Sleeping after the read pushed the sleep into the next
//   frame's measurement and made every step carry the frame-to-frame variation
//   of the work.
// - MCLAFrameDelta hands the measured ticks to AdjustFrameTicks, which returns
//   the step to simulate.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "frame_pacing.h"

#include <cstdint>
#include <cstdlib>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "frame_pacing_policy.h"
#include "logging.h"

REXCVAR_DEFINE_INT32(frame_pacing, 1, "MCLA/Performance",
    "How the game's time step follows the frame rate. 1: while the game holds its "
    "rate (one vblank, or the FPS LIMIT period) every frame advances by exactly that period; "
    "when it does not, by a smoothed frame time. Simulation time stays on the wall clock "
    "either way. 0: every frame advances by its own measured time, as before -- a frame that "
    "misses a vblank then shows up three frames later as a double step, which reads as the car "
    "lurching back and forth.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, real_frame_delta);
REXCVAR_DECLARE(int32_t, fps_limit);

// hooks.cpp
void EnforceFrameLimit();

REX_EXTERN(__imp__rex_sub_821BDA90);

namespace {

// The game's clock object: sub_822C1FA8 updates it through its vtable slot 2.
constexpr uint32_t kGameClock = 0x827D7500;

// Only the frame limiter and the game clock's own update run on this flag.
thread_local bool t_in_game_clock = false;

mc::pacing::Policy g_policy;

int32_t FpsLimit() {
    // Same override EnforceFrameLimit honours.
    static const int32_t env_limit = [] {
        if (const char* e = std::getenv("MCLA_FPS_CAP")) return std::atoi(e);
        return -1;
    }();
    return env_limit >= 0 ? env_limit : REXCVAR_GET(fps_limit);
}

}  // namespace

namespace mc::pacing {

bool LimiterRunsBeforeClock() { return REXCVAR_GET(frame_pacing) != 0; }

uint64_t AdjustFrameTicks(uint64_t elapsed_ticks) {
    if (!t_in_game_clock) return elapsed_ticks;

    const double freq = static_cast<double>(rex::chrono::Clock::guest_tick_frequency());
    const int32_t limit = FpsLimit();
    const double limiter_period = (limit > 0 && freq > 0.0) ? freq / limit : 0.0;
    // One guest vblank, but only while the runtime's vsync actually paces the
    // swaps. larecomp_app.h turns vsync off unless MCLA_VSYNC says otherwise,
    // and then the vblank worker ticks every millisecond: no cadence to snap to.
    // Read every frame, since the pause menu can flip it.
    const bool vsync_on = rex::cvar::GetFlagByName("vsync") == "true";
    const double vblank_period = (vsync_on && freq > 0.0) ? freq / 60.0 : 0.0;

    const int mode = REXCVAR_GET(frame_pacing);
    uint64_t step = elapsed_ticks;
    if (mode != 0 && REXCVAR_GET(real_frame_delta) && freq > 0.0) {
        const double dt = g_policy.Next(static_cast<double>(elapsed_ticks), limiter_period,
                                        vblank_period, 0.1 * freq);
        step = dt >= 1.0 ? static_cast<uint64_t>(dt + 0.5) : 1;
    } else {
        g_policy.Reset();
    }

    return step;
}

}  // namespace mc::pacing

// sub_821BDA90: the clock update. r3 is the clock object.
extern "C" REX_FUNC(rex_sub_821BDA90) {
    const bool game_clock = ctx.r3.u32 == kGameClock;
    if (game_clock) {
        if (mc::pacing::LimiterRunsBeforeClock())
            EnforceFrameLimit();
        t_in_game_clock = true;
    }
    __imp__rex_sub_821BDA90(ctx, base);
    if (game_clock) t_in_game_clock = false;
}

#endif  // REXGLUE_HAS_XEO3_TARGET
