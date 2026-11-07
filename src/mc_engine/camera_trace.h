#pragma once
// cam_trace: a per-frame record of the gameplay camera and of when each frame
// reached the screen, for measuring camera judder. See camera_trace.cpp.

#include <cstdint>

namespace mc::camtrace {

// Guest render thread, at every D3DDevice_Swap after the frame limiter's hold
// (Patch_60FPS_Byte). `timed`: the engine timer ran since the previous swap.
void OnGuestSwap(bool timed);

// Update thread, when the game clock is about to read the timebase (the start
// of a game update; frame_pacing.cpp): `qpc` is that moment.
void NoteGameClock(int64_t qpc);

// Once at startup: hands the renderer the present observer.
void Install();

}  // namespace mc::camtrace
