#pragma once
//
// Xbox One and Xbox Series trigger rumble on Windows: the pads and their motors, through
// Windows.Gaming.Input. Used by the worker thread of impulse_triggers.cpp, which explains why this
// is the way in (see the top of that file). Windows only; every call is made from that worker
// (CloseAll also from Shutdown, under the worker's lock).
//

#include <cstdint>

namespace larecomp::impulse::wgi {

// Looks at the gamepads Windows.Gaming.Input lists: keeps the ones that can have trigger motors,
// forgets the ones that went away, and writes one log line per pad found or lost (and one when a
// pad cannot be driven, with the reason). The first call starts Windows.Gaming.Input on the
// calling thread; if it is not available the reason is logged once and nothing is kept.
void Scan();

// Whether a pad is kept that the triggers should be sent to.
bool HasPads();

// Sets the triggers of every kept pad to the two motor powers (0 to 65535), left and right. A
// pad's rumble is set as a whole (two motors and two triggers in one call), so `game_motors`, the
// two motor speeds the game last asked for (left in the high half, as VibrationDriven returns
// them), goes along and keeps its value. Both powers 0 lets the triggers go (once, with the
// motors as they are).
void Apply(uint16_t left, uint16_t right, uint32_t game_motors);

// Lets every trigger go, forgets the pads (they are found again by the next Scan).
void CloseAll(uint32_t game_motors);

}  // namespace larecomp::impulse::wgi
