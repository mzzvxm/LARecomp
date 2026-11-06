#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal upscaler backends (F4 step 2)
// ===========================================================================
// feature-parity/F4-DESIGN.md sections 6.2, 7 and 14. The TemporalSystem (temporal.h) finds the
// anchor, jitters the scene, makes the depth and the motion vectors; a TemporalBackend turns
// colour + depth + vectors + jitter + camera into the picture the rest of the frame uses. This
// step has one backend, AMD's FidelityFX upscaler (FSR 3.1, and FSR 4 where its provider loads),
// at ratio 1 (render size = output size: "Native AA"); the interface is what DLSS and the other
// ratios plug into later.
//
// The player's settings (REXCVAR, read every frame, saved by the menu like any cvar):
//   mcla_gpu_upscaler            off | fsr31 | fsr4          "Upscaling"
//   mcla_gpu_fsr31_mode          native_aa                   "FSR 3.1 Mode" (this step: only Native AA, 100%)
//   mcla_gpu_fsr4_mode           native_aa                   "FSR 4 Mode"
//   mcla_gpu_upscaler_sharpness  0 .. 1                      FSR's sharpening (RCAS); 0 is none
// A choice this PC cannot run is treated as Off and kept as it was saved (the menu does not list it).
//
// Rules of every backend (6.2):
//   When it runs.   Upscale() at the anchor, on the render thread, inside the frame's list and outside
//                   any render pass: the inputs are in the states the caller transitioned them to,
//                   the output is written, and the caller tells Plume that the list's cached state
//                   is stale (backend.h InvalidateAfterExternalWork).
//   Output.         The caller makes `output` and, when Upscale returns true, the texture cache's
//                   override makes every later fetch of the scene colour read it.
//   Failure.        Anything not ready or failing gives a plain frame: no override, the jitter off
//                   from the next frame, the backend unavailable for the session, one log line.
//   Threads.        Probe() starts a worker; contexts are created and destroyed on it (they take
//                   about 0.6 s and hundreds of MB at 4K); Upscale() is render thread only;
//                   Status() and Ready() may be asked from any thread.
//   Availability.   The pause menu lists what Status() says can run; unsupported or broken is not
//                   listed at all.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <plume_render_interface.h>

#include "gpu.h"

namespace mcla::gpu {

class Device;
class FrameRing;

const char* UpscalerCvarValue(Upscaler upscaler);  // off, fsr31, fsr4
// False (and kOff in `*out`) for anything else.
bool ParseUpscaler(std::string_view text, Upscaler* out);
bool ParseUpscaleMode(std::string_view text, UpscaleMode* out);
// Whether `mode` can run with `upscaler` now (this step: Native AA only).
bool ModeRuns(Upscaler upscaler, UpscaleMode mode);

// What the settings say now (cvars read; a mode that cannot run is Native AA).
struct UpscalerSettings {
  Upscaler upscaler = Upscaler::kOff;  // as saved, before the PC's availability is applied
  UpscaleMode mode = UpscaleMode::kNativeAa;
  float sharpness = 0.2f;              // 0 .. 1
};
UpscalerSettings ReadUpscalerSettings();

enum class BackendState : uint8_t {
  kUnknown,      // Probe has not run (or no backend is built in)
  kProbing,      // the worker is loading the runtime and asking it what it can do
  kAvailable,    // at least one upscaler can run
  kUnavailable,  // none can; `reason` says why
};

struct BackendStatus {
  BackendState state = BackendState::kUnknown;
  bool fsr31 = false;     // the FSR 3.1 provider can run on this PC
  bool fsr4 = false;      // the FSR 4 provider really loads (its own query, not a GPU list)
  bool failed = false;    // a creation or a dispatch failed: off for the rest of the session
  std::string reason;     // kUnavailable, or why it failed
  std::string provider;   // the running provider's name once a context exists
};

// One image of a dispatch.
struct TemporalImage {
  const plume::RenderTexture* texture = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureFlags flags = plume::RenderTextureFlag::NONE;
};

// Everything a frame's dispatch reads (F4-DESIGN.md 6.1, in the conventions the groundwork keeps).
struct TemporalInputs {
  uint64_t frame = 0;
  uint32_t render_size[2] = {};
  uint32_t output_size[2] = {};
  // R16G16B16A16_FLOAT, linear, scene referred, jittered; the depth: R32_FLOAT, 1 near / 0 far;
  // the vectors: RG16_FLOAT, render pixels, previous minus current, +y down, jitter removed. All
  // in SHADER_READ at the COMPUTE stage. The output: R16G16B16A16_FLOAT, GENERAL (unordered access).
  TemporalImage color;
  TemporalImage depth;
  TemporalImage motion;
  TemporalImage output;
  float jitter[2] = {};        // render pixels, +y down: what this frame's draws were moved by
  float motion_scale[2] = {1.0f, 1.0f};  // the vectors times this are render pixels
  float frame_time_ms = 16.667f;
  bool reset = false;          // the history starts again with this frame
  float camera_near = 0.1f;
  float camera_far = 1000.0f;
  float camera_fov_y = 1.0f;   // radians
  float sharpness = 0.0f;      // 0 .. 1; 0 sends no sharpening pass
};

// What a context is made for.
struct BackendRequest {
  Upscaler upscaler = Upscaler::kOff;
  UpscaleMode mode = UpscaleMode::kNativeAa;
  uint32_t render_size[2] = {};
  uint32_t output_size[2] = {};
  bool operator==(const BackendRequest& o) const {
    return upscaler == o.upscaler && mode == o.mode && render_size[0] == o.render_size[0] &&
           render_size[1] == o.render_size[1] && output_size[0] == o.output_size[0] &&
           output_size[1] == o.output_size[1];
  }
};

class TemporalBackend {
 public:
  virtual ~TemporalBackend() = default;

  virtual const char* Name() const = 0;

  // Once, after the device exists. Cheap on the calling thread: the checks that need no runtime,
  // then a worker thread that loads it and asks it what it can do. Never throws, never blocks.
  virtual void Probe(Device& device, FrameRing& frames) = 0;
  // Any thread.
  virtual BackendStatus Status() const = 0;

  // Asks for a context for `request` (render thread, every frame it is wanted; the same request
  // again does nothing). A request that differs from the context's makes the old one go (retired
  // behind the frames that may use it: the FrameRing given to Probe) and the worker make the new one.
  virtual void Request(const BackendRequest& request) = 0;
  // A context for exactly this request exists and nothing failed. Any thread.
  virtual bool Ready(const BackendRequest& request) const = 0;
  // No request any more: the context goes, as when a request changes. Render thread.
  virtual void Release() = 0;

  // The caller found the backend unusable (its inputs never came right): the same as a failed
  // dispatch. Render thread.
  virtual void Fail(const std::string& reason) = 0;

  // Records the upscale into `list`. False on a failure (logged, and Status().failed is set: the
  // caller makes the frame plain and stops asking). Render thread.
  virtual bool Upscale(plume::RenderCommandList* list, const TemporalInputs& inputs) = 0;

  // GPU idle: the worker stops and every context goes.
  virtual void Shutdown() = 0;
};

// The FidelityFX backend, or null in a build without LARECOMP_FFX_SDK_DIR.
std::unique_ptr<TemporalBackend> MakeFfxBackend();

// --- what the app's menu asks (gpu.h GetUpscalerAvailability) -----------------------------------
// The backend publishes what can run (at the end of its probe and when something fails), the
// temporal system what is running.
void PublishUpscalerAvailability(bool probed, bool fsr31, bool fsr4);
void PublishRunningUpscaler(Upscaler running);

}  // namespace mcla::gpu
