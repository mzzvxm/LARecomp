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
//                steps by 1 / N, every frame is read back (gpu/capture_readback.cpp) and handed to a sink (nothing, ffmpeg's FFV1
//                video, PNG pictures or a raw file), and the game does not start update k + 1 until frame k has been read back: the
//                frame gate. The take ends when the scene says so (`capture off`, its `end`) and exactly one picture exists for
//                every update of it.
//   take folder  (M2b) every take is a folder of its own (take_folder.h): take.json (the scene, the exe, the cvars, the size, the rate,
//                the frame count, the update map, the dropped and repeated counts), the video or the pictures, frames.csv, the scene's
//                copy and, when the scene asks, state.csv. A take never overwrites another.
//   memory       (M2b) what a take holds is bounded: a plan made at its first frame (CaptureMemoryPlan) fits a hard cap and the free
//                memory, the GPU readback buffers and the host ring are made once at that size, a slow writer makes the render thread
//                wait (the frame gate holds the game), and a take that cannot fit, whose writer stops, or that sees the PC run out of
//                memory ends with an error.
//   scene clock  during a take the upscaler's history rule (gpu/temporal.cpp) and the UI movies' pacing (hooks/ui.cpp,
//                hooks/ui_smooth.cpp) read frames x step instead of the wall clock, so a slow capture (render scale 6, a slow
//                disk) is not a stall to them. Wall time at any other moment.
//
// Nothing here does anything unless a scene's capture key or one of the debug settings asks: the hooks cost a few atomic loads.
// capture_mode.cpp lists the settings; documentation/DEBUG.md describes the whole.
//

#include <cstdint>
#include <string>

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

// What the scene's `capture fps` key asked for besides the rate (M2b), read when a take starts: the take's folder name, the sink (none,
// ffmpeg, png, raw), and whether state.csv is written. Empty / false: the debug settings (mcla_capture_name, mcla_capture_sink,
// mcla_capture_statelog) and the scene's own name decide.
struct CaptureSpec {
    std::string name;
    std::string sink;
    bool state_log = false;
};
void CaptureSceneSpec(const CaptureSpec& spec);

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
// What a take may use for frames of this size (M2b). Render thread, before the readback buffers are made, at every frame of a take (the
// first call makes the plan, the rest return it): the number of GPU readback buffers and of host ring buffers that fit the memory cap and
// the free memory (take_folder.h PlanMemory). `ok` false: the plan does not fit even at `gpu_min` + 2 buffers, or the size changed during
// the take, or no take is running; the take has failed (the log says why) and the frame is to be reported lost.
struct MemoryGrant {
    bool ok = false;
    uint32_t gpu_buffers = 0;
    uint32_t ring = 0;
};
MemoryGrant CaptureMemoryPlan(uint32_t width, uint32_t height, uint32_t gpu_max, uint32_t gpu_min);
// Render thread, at the first frame of a take (and after, cheap): opens the sink for this frame size. False: no take is running
// or no sink could be made (logged); the frame is then reported lost, so the gate does not wait for it.
bool CaptureSinkOpen(const FrameFormat& format);
// Render thread: a free host buffer of width x height x 4 bytes, packed rows. Blocks while every buffer waits for the writer (the
// back-pressure; counted in `stall_ms`), and gives up only when the writer has finished nothing for mcla_capture_gate_timeout_ms (the take
// then fails). data == nullptr when the sink is not open or the writer is stuck.
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
