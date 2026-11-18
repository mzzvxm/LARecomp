#pragma once
//
// How much the player's wheels spin under power and how much the brakes lock them, for the
// trigger effects: the DualSense's R2 and L2 vibration (adaptive_triggers.cpp) and the Xbox
// impulse triggers (impulse_triggers.cpp) both read it from here. See wheel_slip.cpp.
//

#include <cstdint>

namespace larecomp::slip {

struct Amounts {
    float wheelspin = 0.0f;  // 0 (the tyres grip) to 1 (spinning hard): R trigger
    float lockup = 0.0f;     // 0 (rolling) to 1 (locked): L trigger
};

// The newest amounts for the player's car, the larger of its wheels. Both are 0 while no car is
// driven and when the wheels have not been updated for a moment. Safe from any thread.
Amounts Current();

// From the race HUD update (Hook_TriggersHud, once a frame while a car is driven): `player` is
// the HUD's player object, whose +8 is the car it shows.
void NotePlayer(uint32_t player);

}  // namespace larecomp::slip
