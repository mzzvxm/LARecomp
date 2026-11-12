#pragma once
//
// Xbox One and Xbox Series trigger rumble (the impulse triggers): the right trigger shakes while
// the driven wheels spin under power, the left while the brakes lock the wheels, harder the more
// they slip. The DualSense has its own effects (adaptive_triggers.h). See impulse_triggers.cpp
// (and impulse_wgi.cpp, which reaches the pad on Windows).
//

namespace larecomp::impulse {

// Once, at start-up. Does nothing now: on Windows the trigger motors are reached through
// Windows.Gaming.Input (impulse_wgi.cpp), which needs no SDL hint, and off Windows the SDK's SDL
// is the one SDL and its HIDAPI drivers are on. Kept so the start-up code need not change.
void EarlyInit();

// Every frame, on the game's thread (Patch_DeltaTimePre). Starts the worker thread the first time
// impulse_triggers is on; nothing else.
void Tick();

// Stops the worker, lets both triggers go and closes the pads. The process ends without further
// notice, and a rumble stays in the pad until it is told otherwise. Safe to call more than once
// and from any thread.
void Shutdown();

}  // namespace larecomp::impulse
