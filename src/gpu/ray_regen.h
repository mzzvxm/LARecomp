#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD FSR Ray Regeneration through the FidelityFX API (perf/RAY-REGEN.md, package RG-1)
// ===========================================================================
// The runtime half: opens AMD's loader and denoiser DLL on a Radeon RX 9000 on Direct3D 12 (never anywhere else: ray_regen_rules.h DeviceOk is asked before any file is touched), asks the runtime for
// its providers, makes a trial context to learn that the card really takes it, publishes the answer (Facts) for the DENOISER row and the plan, makes the contexts the effects ask for on a worker thread,
// and records a dispatch. Everything it decides is in ray_regen_rules.h; this file is the calls.
//
// WHO CALLS WHAT (RG-2, RG-3). The plan (ray_regen_plan.h, TemporalSystem::UpdateDenoiserPlan once a frame) decides whether Ray Regeneration takes the frame and asks for the context (Request) while it
// does and for kContextHoldFrames after; the screen-space reflections' trigger (ssr.cpp, the first effect with a signal writer: ray_regen_rules.h kConsumers) asks Ready, records its guide and signal
// pass (ray_regen_guides.h) and Dispatch, and composes the denoised image with its own resolve, or runs its own filter that frame when the dispatch was refused. Probe is made at the start only where
// ProbeWanted says so (the row is not OURS, an effect has a signal writer, the debug cvar does not say off), and live by the plan when the row leaves OURS: it does nothing on any PC that is not an
// RX 9000 on Direct3D 12 with a build and files that have it.
//
// Without LARECOMP_HAS_RAY_REGEN (no denoiser header, or no Direct3D 12 backend) the same names exist as inline no-ops (the end of this file): Published says build_has false, every Ready and
// Dispatch is false, so the callers need no #if.
//
// THREADING: Probe, Request, Shutdown and the facts are thread safe; Dispatch is for the render thread (the one that records the frame). Contexts are made and destroyed on the worker; a replaced
// context goes to the worker through FrameRing::RetireAction, after the frames that may name it.
//
// Compiled only with LARECOMP_HAS_RAY_REGEN (src/gpu/CMakeLists.txt: the denoiser header is in the FidelityFX kit and the D3D12 backend is built); without it Facts says build_has false.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "ray_regen_rules.h"
#include "temporal_backend.h"

namespace mcla::gpu {
class Device;
class FrameRing;
}  // namespace mcla::gpu

namespace mcla::gpu::rayregen {

// What the runtime and the PC said, for the DENOISER row and the plan (any thread; a copy).
struct Published {
  Facts facts;
  uint32_t vendor_id = 0;    // the PCI vendor of the graphics card, set by the probe (the row's middle value is named by it: ray_regen_rules.h VendorOf)
  std::string provider;      // the version the contexts are pinned to ("1.2.1"), empty before the probe
  std::string failed_why;    // why Facts::failed, empty otherwise
  uint64_t context_bytes = 0;  // the runtime's own figure for the live context (0: none)
  uint32_t context_signals = 0;  // the signals of the live context
  bool context_making = false;  // a context is asked for and not ready
};
Published Current();

// One dispatch, in the conventions of ray_regen_rules.h (CONVENTIONS). The common images are at the render size and in the state `input_state` (the guide pass leaves its images in SHADER_READ at the
// compute stage), the signals' inputs likewise, their outputs in UNORDERED_ACCESS; states are FFX_API_RESOURCE_STATE_* bits (ffx_api_types.h).
struct SignalImages {
  TemporalImage input;
  TemporalImage output;
};
struct DispatchFrame {
  uint64_t game_frame = 0;      // the renderer's frame number: a gap resets the history (FrameClock)
  bool force_reset = false;     // a camera cut, a size change, a switch of the view
  uint32_t render_size[2] = {};
  TemporalImage linear_depth, motion, normals, specular_albedo, diffuse_albedo;
  std::array<SignalImages, kSignalCount> signals{};
  uint32_t dispatch_signals = 0;  // the signals above that are filled: exactly the context's
  float jitter[2] = {};           // the frame's own sub-pixel jitter in the dispatch's pixels (the draws' jitter at the dispatch's size: guides::JitterAtSize)
  float camera_position_delta[3] = {};  // previous minus current, world space
  float view[16] = {};            // world to view, row-major, row vectors (ray_regen_guides_rules.h BuildCamera: the game's view space, the camera's rotation)
  float projection[16] = {};      // the unjittered view to projection, the same layout
  Bounds depth_bounds;
  bool motion_in_pixels = false;  // false: the motion image is PreviousUV - CurrentUV already (motionVectorScale 1); true: render pixels (scale 1 / size)
  bool shared_linear_albedo = true;  // the albedo images are linear (the RR guides): kDispatchNonEncodedAlbedo
  uint32_t input_state = 0;      // 0: SHADER_READ at the compute stage (FFX_API_RESOURCE_STATE_COMPUTE_READ)
  uint32_t output_state = 0;     // 0: UNORDERED_ACCESS
};

#if defined(LARECOMP_HAS_RAY_REGEN)

class RayRegen {
 public:
  RayRegen();
  ~RayRegen();
  RayRegen(const RayRegen&) = delete;
  RayRegen& operator=(const RayRegen&) = delete;

  // Opens the runtime on a worker thread when this device can run it (DeviceOk); a no-op otherwise, and on every later call. `frames` retires replaced contexts.
  void Probe(Device& device, FrameRing& frames);
  // Asks for a context with these signals for renders up to `max_w` x `max_h` (made in the background). 0 signals: the context is released after a hold (the plan's ContextKeeper decides when).
  void Request(uint32_t signals, uint32_t max_w, uint32_t max_h);
  // The context with exactly these signals exists and was made for at least this size (the render thread adopts a context the worker made here and in Request / Dispatch).
  bool Ready(uint32_t signals, uint32_t w, uint32_t h);
  // Records the dispatch on the render thread's list. False: nothing was recorded (the log says why once; three failures in a row drop Ray Regeneration for the session: Facts::failed).
  bool Dispatch(plume::RenderCommandList* list, const DispatchFrame& frame);
  // The next frame must not reuse this one's history (the view changed, a camera cut that the dispatch's own flag does not see).
  void ForgetHistory();
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The one runtime of the process (the DLLs are opened once). StartIfWanted reads the debug switch mcla_gpu_ray_regen and the DENOISER row and calls Probe when ProbeWanted says so; StopAll shuts it down
// (the GPU is idle).
RayRegen& Instance();
void StartIfWanted(Device& device, FrameRing& frames);
// The plan wants the runtime now (the row left OURS in a game that started with it on OURS: live): opens it when it is not open (a no-op when it is, or on a PC that cannot run it).
void ProbeNow(Device& device, FrameRing& frames);
void StopAll();

#else  // no AMD denoiser header, or no Direct3D 12 backend: the same names, doing nothing

inline Published Current() {
  return Published{};
}
class RayRegen {
 public:
  void Probe(Device&, FrameRing&) {}
  void Request(uint32_t, uint32_t, uint32_t) {}
  bool Ready(uint32_t, uint32_t, uint32_t) { return false; }
  bool Dispatch(plume::RenderCommandList*, const DispatchFrame&) { return false; }
  void ForgetHistory() {}
  void Shutdown() {}
};
inline RayRegen& Instance() {
  static RayRegen runtime;
  return runtime;
}
inline void StartIfWanted(Device&, FrameRing&) {}
inline void ProbeNow(Device&, FrameRing&) {}
inline void StopAll() {}

#endif

}  // namespace mcla::gpu::rayregen
