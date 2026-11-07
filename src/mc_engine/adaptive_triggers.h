#pragma once
//
// DualSense trigger effects: resistance (R2 is a light spring that firms up towards the end of
// its travel, L2 is firmer) and vibration (R2 while the driven wheels spin, L2 while the brakes
// lock the wheels). Both go slack whenever the car is not being driven. Each has its own setting,
// trigger_resistance and trigger_vibration. See adaptive_triggers.cpp.
//

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre). Does its work at most every 50 ms
// and nothing while both settings are off and no pad is held.
void Tick();

// Lets both triggers go and closes the pads. The process ends without further notice (the
// window's close, QUIT), and an effect stays in the pad until it is told otherwise. Safe to
// call more than once and from any thread.
void Shutdown();

// The checks the triggers use, for other code that wants the same answer (the audio gap
// counter, hooks/audio_mixer.cpp; the Xbox trigger rumble, impulse_triggers.cpp). Safe from any
// thread.
// IsGamePaused: the game's own paused flag is set (the pause menu holds the game).
// IsWindowInFront: the foreground window belongs to this process.
// IsDriving: the race HUD updated in the last 300 ms and the game is not paused.
bool IsGamePaused();
bool IsWindowInFront();
bool IsDriving();

}  // namespace larecomp::triggers
