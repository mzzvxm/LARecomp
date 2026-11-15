#pragma once
//
// DualSense trigger effects: resistance (R2 is a light spring that firms up towards the end of
// its travel, L2 is firmer) and vibration (R2 while the driven wheels spin, L2 while the brakes
// lock the wheels). Both go slack whenever the car is not being driven. Each has its own setting,
// trigger_resistance and trigger_vibration. See adaptive_triggers.cpp.
//

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre). Does its work at most every 50 ms:
// works out what the triggers should be and hands it on. On Windows a worker thread (started
// when the device watcher, device_watch.h, first sees a DualSense) opens the pads and writes the
// reports; no device I/O and no scan runs on the game's thread. Nothing at all while no DualSense
// is plugged in or both settings are off.
void Tick();

// Lets both triggers go and closes the pads. The process ends without further notice (the
// window's close, QUIT), and an effect stays in the pad until it is told otherwise. Safe to
// call more than once and from any thread. On Windows it asks the worker to do it and waits for
// that (up to 600 ms).
void Shutdown();

// The checks the triggers use, for other code that wants the same answer (the audio gap
// counter, hooks/audio_mixer.cpp; the Xbox trigger rumble, impulse_triggers.cpp). Safe from any
// thread.
// IsGamePaused: the game's own paused flag is set (the pause menu holds the game).
// IsWindowInFront: the foreground window belongs to this process (off Windows: the game window has
// the focus).
// IsDriving: the race HUD updated in the last 300 ms and the game is not paused.
bool IsGamePaused();
bool IsWindowInFront();
bool IsDriving();

}  // namespace larecomp::triggers
