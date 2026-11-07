#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD's FSR 3.1 frame generation, through the FidelityFX API
// ===========================================================================
// The context and the three calls of frame generation in manual mode (F4-DESIGN.md 8.2): no swap
// chain wrapper, no callbacks; the renderer records the generation into its own list and presents
// the generated image itself (frame_generation.h). Compiled only with the FidelityFX SDK's frame
// generation header (CMake: LARECOMP_FFX_HAS_FRAMEGENERATION); otherwise MakeFfxFrameGeneration()
// returns null and the setting is not offered.
//
// Files. amd_fidelityfx_loader_<dx12|vk>.dll and amd_fidelityfx_framegeneration_<dx12|vk>.dll next
// to the exe, opened by their full path as the upscaler backend does (temporal_ffx.h): the loader
// reuses a module of that name that is already loaded (AMD's documentation, "Effect DLL lookup").
// The frame generation DLL carries two providers, FSR 3.1 (analytical, any GPU the upscaler runs
// on) and FSR 4 (machine learning, RDNA 4 only); the 3.1 one is always the one asked for, by its
// version id from the runtime's own query.
//
// Per generated frame (the order the runtime requires, recorded on the render thread, outside any
// render pass, into the list given):
//   ffxConfigure   the frame's ID, the generation rect, FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY
//   ffxDispatch    ffxDispatchDescFrameGenerationPrepareV2: depth, motion vectors, jitter, camera
//   ffxDispatch    ffxDispatchDescFrameGeneration: the current colour in, the interpolated frame out
// The runtime keeps the colour it was given for the next frame's interpolation itself.
//
// Threads. Probe() starts a worker that loads the runtime and asks it what it can do; contexts are
// made and destroyed on it (tens of milliseconds to half a second; hundreds of MB at 4K); Generate()
// and Request() are render thread only; Status() and Ready() any thread.
//
// Memory. The runtime makes its images with the device's own calls, not through Device::CreateTexture,
// so the device never hears of a context's creation failing or of its going. Three things stand in
// for that (frame_generation.h "Memory"): the size the runtime reports for a context is kept
// (Status().context_bytes, and EstimateContextBytes before the next one is made); a context that
// fails for want of memory is not a failure of the session (Status().no_memory); and a context that
// is destroyed is told to the device as video memory given back (Device::NoteReleased).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "temporal_backend.h"

namespace mcla::gpu {

class Device;
class FrameRing;

// What a context is made for.
struct FgContextRequest {
  uint32_t render_size[2] = {};      // the depth and the motion vectors
  uint32_t generation_size[2] = {};  // the colour in and the generated frame out
  bool operator==(const FgContextRequest& o) const {
    return render_size[0] == o.render_size[0] && render_size[1] == o.render_size[1] &&
           generation_size[0] == o.generation_size[0] && generation_size[1] == o.generation_size[1];
  }
};

// Everything one generation reads and writes. Conventions as TemporalInputs (temporal_backend.h):
// depth R32_FLOAT with 1 near / 0 far, vectors RG16_FLOAT in render pixels (previous minus current,
// +y down, jitter removed), the colour R16G16B16A16_FLOAT in the codes the pass wrote (SRGB transfer).
struct FgInputs {
  uint64_t frame_id = 0;           // one more than the frame generated before, else the history starts again
  bool reset = false;              // the history starts again with this frame
  uint32_t render_size[2] = {};
  uint32_t generation_size[2] = {};
  TemporalImage color;             // SHADER_READ at the compute stage
  TemporalImage depth;             // SHADER_READ at the compute stage
  TemporalImage motion;            // SHADER_READ at the compute stage
  TemporalImage output;            // GENERAL, unordered access
  float jitter[2] = {};            // render pixels, +y down: what this frame's draws were moved by
  float motion_scale[2] = {1.0f, 1.0f};
  float frame_time_ms = 16.667f;
  float camera_near = 0.1f;
  float camera_far = 1000.0f;
  float camera_fov_y = 1.0f;       // radians
  float camera_position[3] = {};
  float camera_up[3] = {};
  float camera_right[3] = {};
  float camera_forward[3] = {};
};

enum class FgBackendState : uint8_t {
  kUnknown,      // Probe has not run
  kProbing,      // the worker is loading the runtime
  kAvailable,    // FSR 3.1 frame generation can run
  kUnavailable,  // it cannot; `reason` says why
};

struct FgBackendStatus {
  FgBackendState state = FgBackendState::kUnknown;
  bool failed = false;       // a creation or a dispatch failed: off for the rest of the session
  // The runtime ran out of video memory making a context (not a failure: nothing is asked again
  // until Release, and the caller tries again later). `reason` says so.
  bool no_memory = false;
  std::string reason;        // kUnavailable, or why it failed: one plain sentence
  std::string provider;      // the provider's name once a context exists
  uint64_t context_bytes = 0;  // video memory the ready context holds, as the runtime reports it
};

class FrameGenerationBackend {
 public:
  virtual ~FrameGenerationBackend() = default;

  // Once, after the device exists. Cheap on the calling thread: the checks that need no runtime, then
  // a worker thread that loads it and asks it what it can do. Never blocks.
  virtual void Probe(Device& device, FrameRing& frames) = 0;
  // Any thread.
  virtual FgBackendStatus Status() const = 0;

  // Asks for a context for `request` (render thread, every frame it is wanted; the same request again
  // does nothing). A request that differs from the context's retires the old one behind the frames that
  // may use it and has the worker make the new one.
  virtual void Request(const FgContextRequest& request) = 0;
  // A context for exactly this request exists and nothing failed. Any thread.
  virtual bool Ready(const FgContextRequest& request) const = 0;
  // The video memory a context for `request` would take, from the last one the runtime made and
  // reported on (the probe's small trial counts), scaled by the size of the generated frame; 0 when
  // none was measured. The runtime has no way to say it without a context. Any thread.
  virtual uint64_t EstimateContextBytes(const FgContextRequest& request) const = 0;
  // No request any more: the context goes (retired behind the frames that may use it) and a
  // no_memory answer is forgotten, so that the next Request tries again. Render thread.
  virtual void Release() = 0;
  // The caller found it unusable (a generation's inputs never came right): off for the session.
  virtual void Fail(const std::string& reason) = 0;

  // Records the configuration, the preparation and the generation into `list`. False when nothing
  // usable was recorded (a failure: logged, Status().failed is set). Render thread.
  virtual bool Generate(plume::RenderCommandList* list, const FgInputs& inputs) = 0;

  // GPU idle: the worker stops and the context goes.
  virtual void Shutdown() = 0;
};

// The FidelityFX backend, or null in a build without the SDK's frame generation header.
std::unique_ptr<FrameGenerationBackend> MakeFfxFrameGeneration();

}  // namespace mcla::gpu
