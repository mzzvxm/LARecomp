#pragma once

// Frame pacing for the unlocked frame rate: the time step the game simulates,
// where the frame limiter sleeps, and a per-frame trace to measure both. See
// frame_pacing_policy.h for why the measured frame time is the wrong step.

#include <cstdint>

namespace mc::pacing {

// From MCLAFrameDelta, with the elapsed guest timebase ticks sub_821BDA90 just
// measured (after the hitch clamp). Returns the ticks the game should simulate.
// Only the game's own clock (0x827D7500) is touched; any other clock object the
// same code updates passes through unchanged.
uint64_t AdjustFrameTicks(uint64_t elapsed_ticks);

// True when the frame limiter is run by the clock wrapper, right before the
// game's clock reads the timebase, instead of by MCLAFrameDelta after it.
bool LimiterRunsBeforeClock();

// From the GPU interrupt probe. source 0 = vblank, 1 = PM4 interrupt.
void OnGuestInterrupt(uint32_t source);

}  // namespace mc::pacing
