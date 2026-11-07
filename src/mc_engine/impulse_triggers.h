#pragma once
//
// Xbox One and Xbox Series trigger rumble (the impulse triggers): the right trigger shakes while
// the driven wheels spin under power, the left while the brakes lock the wheels, harder the more
// they slip. The DualSense has its own effects (adaptive_triggers.h). See impulse_triggers.cpp.
//

namespace larecomp::impulse {

// Once, at start-up, before anything starts this executable's SDL joystick layer (the wheel
// thread, the menu pad layer): asks it for the GameInput driver, the only one of its Windows
// drivers that reaches the trigger motors.
void EarlyInit();

// Every frame, on the game's thread (Patch_DeltaTimePre). Starts the worker thread the first time
// impulse_triggers is on; nothing else.
void Tick();

// Stops the worker, lets both triggers go and closes the pads. The process ends without further
// notice, and a rumble stays in the pad until it is told otherwise. Safe to call more than once
// and from any thread.
void Shutdown();

}  // namespace larecomp::impulse
