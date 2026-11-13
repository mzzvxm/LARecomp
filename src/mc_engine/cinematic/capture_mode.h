#pragma once
//
// Capture mode of the trailer toolkit (work package M2a): a fixed time step, a frame gate and one clock for the algorithms that
// read the wall time. The maths is in capture_core.h; this is the glue (capture_mode.cpp) and the interface the hooks and the
// renderer call.
//
//   fixed step   every update advances the game's clock by exactly 1 / fps, whatever the frame took to render. Armed by a take
//                (below), or alone by the debug setting mcla_fixed_step_fps (a repeatability test, a slow-motion recording that
//                is made some other way). The FPS limit, the step cap and frame pacing stand aside while it runs.
//   take         a scene's `capture fps N` (or the debug setting mcla_capture_fps) starts a take at the next update: the game
//                steps by 1 / N, every frame is read back (gpu/capture_readback.cpp) and handed to a sink (nothing, ffmpeg or a
//                raw file), and the game does not start update k + 1 until frame k has been read back: the frame gate. The take
//                ends when the scene says so (`capture off`, its `end`) and exactly one picture exists for every update of it.
//   scene clock  during a take the upscaler's history rule (gpu/temporal.cpp) and the UI movies' pacing (hooks/ui.cpp,
//                hooks/ui_smooth.cpp) read frames x step instead of the wall clock, so a slow capture (render scale 6, a slow
//                disk) is not a stall to them. Wall time at any other moment.
//
// Nothing here does anything unless a scene's capture key or one of the debug settings asks: the hooks cost a few atomic loads.
// capture_mode.cpp lists the settings; documentation/DEBUG.md describes the whole.
//

#include <cstdint>

namespace mc::cinematic {

inline constexpr uint64_t kNoFrame = ~0ull;

// ---------------------------------------------------------------------------------------------------------------- game thread

// The engine timer's first instruction (hooks/frame_timing.cpp MCLAFrameStart, once per update): counts the update; starts a take
// that was asked for; during a take waits until the frames handed over have been read back, and ends the take that was
// switched off.
void CaptureGate();

// The hand-over of a frame to the render thread (hooks/frame_pacing.cpp, rex_sub_82305B38; the render thread is idle): stamps the
// frame with the update it shows and whether it belongs to the take. The swap takes the stamp.
void CaptureOnHandOver();

// The guest's swap (hooks/frame_timing.cpp Patch_60FPS_Byte, before D3DDevice_Swap writes the present record): gives the stamp to
// the frame's present record (gpu.h SetNextPresentTag).
void CaptureOnSwap();

// A take is running (the clock, the readback and the gate follow it).
bool CaptureTakeActive();

// The scene's request (the capture applier): a take at this many frames a second, or 0 for none.
void CaptureSceneRequest(double fps);

// ---------------------------------------------------------------------------------------------------------------- fixed step

// The step the engine timer is to be given every update, in the game's own ticks (49.875 MHz), or 0 when none is set: the
// running take's, else mcla_fixed_step_fps's. Game thread (hooks/frame_timing.cpp MCLAFrameDelta).
uint64_t FixedStepTicks();
// A fixed step is in force: the FPS limiter, the step cap and frame pacing stand aside. Any thread.
bool FixedStepActive();

// ---------------------------------------------------------------------------------------------------------------- scene clock

// Milliseconds on the scene clock: the steady clock, or, during a take (with mcla_capture_virtual_clock on), the take's frames
// times its step, started where the steady clock was when the take began, so a value kept across the start of a take stays
// comparable. `frame`: the caller's own frame counter (the renderer's guest frame); kNoFrame: the game thread's update counter.
// Any thread.
double SceneClockNowMs(uint64_t frame = kNoFrame);
int64_t SceneClockNowNs();
// Changes whenever the clock switches between the steady and the virtual one (a take starts or ends): what a pacing schedule
// built on SceneClockNowNs has to start again at.
uint32_t SceneClockEpoch();
// The upscaler's history rule, once per accepted anchor (gpu/temporal.cpp): `reset` is the frame's reset mask, `time_bit` the bit
// that means "more than kMaxGapMs since the anchor before". Counted during a take; the summary logs it, so the clock's effect
// (a restart on every frame without it, none with it) is read from one line.
void SceneClockNoteAnchor(uint32_t reset, uint32_t time_bit);

// ---------------------------------------------------------------------------------------------------------------- render thread

struct FrameFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    bool bgra = true;  // the swap chain's order (B8G8R8A8), else R8G8B8A8
};
// What the render thread measured for one captured frame, milliseconds.
struct FrameCosts {
    uint64_t update = 0;   // the frame's tag
    double record_ms = 0;  // recording the 1:1 draw and the copy (CPU)
    double gpu_ms = 0;     // the render thread waited this long for the frame's fence (lock-step only)
    double copy_ms = 0;    // map, copy rows into the host buffer, unmap
    double stall_ms = 0;   // waited for a free host buffer (the writer is slower than the render)
    double vram_mb = 0;    // the renderer's video memory in use, MiB (0: not sampled)
};
// A host buffer of the ring.
struct HostBuffer {
    uint8_t* data = nullptr;
    uint32_t slot = 0;
    uint64_t bytes = 0;
};
// Render thread, at the first frame of a take (and after, cheap): opens the sink for this frame size. False: no take is running
// or no sink could be made (logged); the frame is then reported lost, so the gate does not wait for it.
bool CaptureSinkOpen(const FrameFormat& format);
// Render thread: a free host buffer of width x height x 4 bytes, packed rows. Blocks while every buffer waits for the writer
// (counted in `stall_ms`). data == nullptr when the sink is not open.
HostBuffer CaptureAcquire(double* stall_ms);
// Render thread: the buffer holds the frame; the writer takes it and the gate counts the frame as read back.
void CaptureFrameDone(const HostBuffer& buffer, const FrameCosts& costs);
// Render thread: the frame could not be read back (counted, so the gate does not wait for it).
void CaptureFrameLost(uint64_t update, const char* why);
// The ring's lag setting: 0 = the game waits for each frame's readback before the next update (lock-step).
int CaptureLag();
// Extra milliseconds the render thread sleeps per captured frame (emulates a slow capture: mcla_capture_delay_ms).
double CaptureDelayMs();

}  // namespace mc::cinematic
