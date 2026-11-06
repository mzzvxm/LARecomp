#pragma once
// ===========================================================================
// MCLA GPU renderer -- the GPU vendor's latency reducer
// ===========================================================================
// NVIDIA Reflex (D3D12 through nvapi64.dll from the driver, Vulkan through
// VK_NV_low_latency2) and AMD Anti-Lag 2 (D3D12 through the driver's extension,
// Vulkan through VK_AMD_anti_lag). When the GPU is the limit the game runs
// ahead of it and the frames queue in front of the GPU, each one input lag; the
// reducer holds the game back at the start of a frame, just long enough that
// the frame meets a free GPU. Both are asked of the rendering device (the one
// Plume made, whichever GPU drives the display: the driver's answer decides,
// there is no special case for a render GPU that is not the display GPU).
//
// Support is found once, at renderer start, and never changes in a session: the
// graphics API, the GPU's vendor, the driver's entry points (nvapi64.dll,
// amdxc64.dll) and the device's extensions with their features all have to be
// there, and the driver has to take the neutral mode (NVIDIA) or the interface
// has to open (AMD). Where any of that is missing the feature is off, whatever
// mcla_gpu_low_latency says, the pause menu has no row for it, and the log says
// why in one line. It is also only ever asked of the driver while it is on:
// a player who leaves it off keeps their driver's own setting.
//
// What a frame is, and where the driver is told about it. The game updates on
// its main loop, records the frame on its own render thread ("[MC] Render
// Thread", one frame behind) and this renderer replays that on the render thread
// of its own, so one frame crosses three threads:
//
//   update thread                      guest render thread        host render thread
//   OnFrameStart (the engine timer's   draws are captured as      first scene record of the frame:
//   first instruction): the driver's   records                    RENDERSUBMIT_START
//   wait, then SIMULATION_START and    the swap: OnGuestSwap ->   the frame's last submission:
//   INPUT_SAMPLE                       SIMULATION_END             RENDERSUBMIT_END
//                                                                 the present: PRESENT_START / PRESENT_END
//
//  - The wait goes where the frame starts, before the engine timer reads the
//    clock and before the game polls its input: the timer is the first thing a
//    frame does, and the hook sits on its first instruction, so the driver's
//    wait is part of the time step the timer then measures and not a gap
//    between two steps (measured with the fork this comes from: a wait after the
//    clock read gives a less even step in every pair tried). INPUT_SAMPLE
//    follows at once: the game reads its pad in the update that follows, and
//    nothing in a frame comes before the timer.
//  - A frame is the pass through the engine timer to the swap that ends it. Its
//    id is the ordinal of that pass; the swap (guest side) and the replay (host
//    side) count the swaps they see against the passes that were started, in
//    the same order, so every marker of a frame carries the same id without a
//    queue between the threads. A swap with no pass to pair with (the logos,
//    loading screens, the overlay's idle presents) is not marked; a frame that
//    was started and never swapped is written off after a quarter of a second
//    (or when eight are waiting), so a lost frame cannot leave the ids shifted.
//  - Reflex's RENDERSUBMIT markers bracket the host render thread's work for
//    the frame (the records of its draws are submitted in the frame's last
//    command list), PRESENT_START/END the swap chain's present call, which on
//    Vulkan also carries the frame's id as its VkPresentIdKHR. Anti-Lag 2 is told
//    the same points where its API has them (Vulkan: the input stage before the
//    wait, the present stage before the present; D3D12: the end of the frame's
//    main rendering at the start of the present).
//
// The frame rate limit stays with LARecomp's own limiter (hooks/frame_timing.cpp),
// which waits after the frame's input; the vendors' own limits are left at none.
// Reflex can hold the cap instead, its wait before the input (the minimum interval
// of its sleep mode), but on an RTX 5080 at a 60 FPS cap that paced frames worse
// (1% low 55.9 against 58.8 FPS, 0.1% low 48.6 against 57.8) for no less latency
// (from the frame's start to the screen, 95th percentile 19.2 against 17.9 ms)
// than Reflex beside the game's limiter.
//
// Vsync is left to the driver: the reducers work with it, and the vsync wait
// comes after the markers, inside the present they bracket.
//
// Threading: OnFrameStart on the update thread only; OnGuestSwap on the guest
// render thread; the Render* and Present* calls on the host render thread;
// Initialize/Shutdown on the thread that owns the renderer's lifecycle, the
// latter after the render thread stopped. Everything else from any thread.
// ===========================================================================

#include <cstdint>
#include <memory>

namespace mcla::gpu {

class Device;
class SwapChain;

namespace latency {

// --- the pause menu --------------------------------------------------------

// Which vendor's reducer the renderer found on its device; kNone where there is
// none (no row, nothing happens).
enum class Kind : uint8_t { kNone, kReflex, kAntiLag };
Kind AvailableKind();
// What mcla_gpu_low_latency = auto comes to on this GPU.
bool AutoIsOn();
// kPaused: the driver kept the game waiting too long and the reducer is off for
// a few seconds;
// kFailed: the driver refused the mode, and it stays off until the next start.
enum class Health : uint8_t { kOk, kPaused, kFailed };
Health CurrentHealth();

// --- the guest's threads (hooks/frame_timing.cpp) -------------------------------

// The update thread, once per pass through the engine timer, before the timer
// reads the clock: the driver's wait, then the frame's SIMULATION_START and
// INPUT_SAMPLE.
void OnFrameStart();
// The guest render thread, at every swap, after the frame limiter's wait: the
// frame the swap ends has SIMULATION_END.
void OnGuestSwap();

// --- the renderer (renderer.cpp), render thread unless noted --------------------

// After the swap chain exists, before the render thread starts. Finds out
// whether the device has a reducer, logs the answer and, with one, arms
// the frame bookkeeping.
void Initialize(Device& device, SwapChain& swap_chain);
// After the render thread stopped, before the swap chain goes: the driver is
// told to leave the mode and the vendor's objects are released.
void Shutdown();

// A guest frame's command list was opened (its first scene record, or its
// present when it had none): RENDERSUBMIT_START.
void OnRenderFrameOpen();
// The frame's last command list was submitted: RENDERSUBMIT_END.
void OnRenderSubmitted();
// Just before and after the swap chain's present call of a guest frame.
void OnPresentBegin();
void OnPresentEnd();

// The presents the renderer makes by itself while the guest is not swapping (the
// overlays) are not guest frames; a list opened inside one is not a frame to mark.
class IdlePresentScope {
 public:
  explicit IdlePresentScope(bool idle);
  ~IdlePresentScope();
  IdlePresentScope(const IdlePresentScope&) = delete;
  IdlePresentScope& operator=(const IdlePresentScope&) = delete;

 private:
  bool idle_;
};

// Around SwapChain::Rebuild: Vulkan's swap chain handle changes there while the
// update thread may be calling the driver with it, and the sleep mode belongs to
// the chain, so it is told again to the new one. Nothing on D3D12.
class SwapChainChange {
 public:
  SwapChainChange();
  ~SwapChainChange();
  SwapChainChange(const SwapChainChange&) = delete;
  SwapChainChange& operator=(const SwapChainChange&) = delete;

 private:
  std::shared_ptr<void> reducer_;  // the Reducer, type-erased (latency_backend.h)
};

}  // namespace latency
}  // namespace mcla::gpu
