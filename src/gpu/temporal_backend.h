#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal upscaler backends (F4 step 2)
// ===========================================================================
// feature-parity/F4-DESIGN.md sections 6.2, 7 and 14. The TemporalSystem (temporal.h) finds the
// anchor, jitters the scene, makes the depth and the motion vectors; a TemporalBackend turns
// colour + depth + vectors + jitter + camera into the picture the rest of the frame uses. This
// step has two backends: AMD's FidelityFX upscaler (FSR 3.1, and FSR 4 where its provider loads;
// temporal_ffx.h) and NVIDIA's DLSS Super Resolution through Streamline (temporal_dlss.h), each at
// its vendor's five ratios (render size = output size over 1, 1.5, 1.7, 2 or 3: "Native AA" or
// "DLAA" to "Ultra Performance").
//
// The player's settings (REXCVAR, read every frame, saved by the menu like any cvar; a change takes
// effect at the next frame boundary once the new upscaler context is ready, temporal.h):
//   mcla_gpu_upscaler            off | fsr31 | fsr4 | dlss   "Upscaling"
//   mcla_gpu_fsr31_mode          native_aa | quality | balanced | performance | ultra_performance
//                                                            "FSR 3.1 Mode": 100%, 67%, 59%, 50%, 33%
//                                                            (live: the scene follows at a frame boundary)
//   mcla_gpu_fsr4_mode           the same                    "FSR 4 Mode"
//   mcla_gpu_dlss_mode           dlaa | quality | balanced | performance | ultra_performance
//                                                            "DLSS Mode": 100%, 67%, 58%, 50%, 33%
//   mcla_gpu_upscaler_sharpness  0 .. 1                      FSR's sharpening (RCAS); 0 is none
//                                                            (DLSS has none)
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
//                   about 0.6 s and hundreds of MB at 4K for FSR, about 8 s for DLSS); Upscale() is
//                   render thread only; Status() and Ready() may be asked from any thread.
//   Contexts.       One is current (Request, Ready, Upscale). A second one can be prepared beside it
//                   (Prepare, PreparedReady) and take over between two frames (Commit): the
//                   player's change of upscaler or mode never leaves the picture without one.
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

const char* UpscalerCvarValue(Upscaler upscaler);  // off, fsr31, fsr4, dlss
// False (and kOff in `*out`) for anything else.
bool ParseUpscaler(std::string_view text, Upscaler* out);
bool ParseUpscaleMode(std::string_view text, UpscaleMode* out);
// Whether `mode` can run with `upscaler` (FSR 3.1, FSR 4 and DLSS: all five).
bool ModeRuns(Upscaler upscaler, UpscaleMode mode);

// True when the 3D scene may be drawn smaller than the output during this run: an upscaler runtime
// is built in, and the player can pick a mode below 100% at any time (the scene follows it live,
// temporal.h). The pipelines a smaller scene needs (the reduced copies and the fractional depth
// resolve, a handful) are listed for it before the start's own decision exists (resolve.cpp
// ListPrecache), so that a live change finds them made; a PC that cannot run an upscaler only makes
// that list longer than it had to be.
bool MayDrawSceneSmaller();

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
  bool dlss = false;      // DLSS can run on this PC (the DLSS backend; its own query)
  bool failed = false;    // a creation or a dispatch failed: off for the rest of the session
  std::string reason;     // kUnavailable, or why it failed
  std::string provider;   // the running provider's name once a context exists
  uint64_t context_bytes = 0;  // video memory the ready context holds, as the runtime reports it
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
  // The camera as Streamline's constants want it (DLSS; FSR does not read these): row-major
  // matrices of row vectors (v' = v M, as the game's constants are), without the jitter. The previous
  // clip space is this frame's own when the history starts again (clip_to_prev_clip is the identity).
  float camera_aspect = 1.0f;
  float view_to_clip[16] = {};         // the projection
  float clip_to_view[16] = {};
  float clip_to_prev_clip[16] = {};    // this frame's clip space to the frame before's
  float prev_clip_to_clip[16] = {};
  float camera_position[3] = {};       // world space
  float camera_up[3] = {};
  float camera_right[3] = {};
  float camera_forward[3] = {};
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
  // behind the frames that may use it: the FrameRing given to Probe) and the worker make the new one:
  // the frames in between are plain. The path that is running is asked for this way; a path that is
  // coming (a live change) is Prepare'd and Commit'ted, so that the old one works until the new one
  // is made.
  virtual void Request(const BackendRequest& request) = 0;
  // A context for exactly this request exists and nothing failed. Any thread.
  virtual bool Ready(const BackendRequest& request) const = 0;
  // The length of the jitter sequence the runtime wants for the ready context's sizes (FFX: its own
  // query, 8 x ratio^2), 0 when there is no context or the runtime has no say. Any thread.
  virtual uint32_t JitterPhaseCount() const { return 0; }
  // Make before break (a live change of upscaler or mode: temporal.h "Changing the path"). Asks for a
  // second context, for `next`, while the current one (Request's) goes on dispatching: the worker
  // makes it in a slot of its own, so nothing the frames use changes. The same request again does
  // nothing; a different one replaces the prepared one (what the worker makes for the old one is
  // freed when it is done). A request the current context already is, or is being made for, is not
  // made twice. Render thread, every frame it is wanted.
  virtual void Prepare(const BackendRequest& next) = 0;
  // A context for exactly this request exists, the current one or the prepared one. Any thread.
  virtual bool PreparedReady(const BackendRequest& next) const = 0;
  // The prepared context becomes the current one (the old one is retired behind the frames that
  // may use it): from now on Ready(next) holds and Upscale() uses it. False, nothing changed, when
  // there is no ready context for `next`. Render thread, between two frames.
  virtual bool Commit(const BackendRequest& next) = 0;
  // The prepared request is not wanted any more: its context, made or being made, is freed. Render
  // thread.
  virtual void CancelPrepared() = 0;
  // No request any more: the context goes, as when a request changes, and the prepared one with
  // it. Render thread.
  virtual void Release() = 0;

  // The caller found the backend unusable (its inputs never came right): the same as a failed
  // dispatch. Render thread.
  virtual void Fail(const std::string& reason) = 0;

  // Records the upscale into `list`. False when nothing was recorded: a failure (logged, and
  // Status().failed is set: the caller makes the frame plain and stops asking), or a runtime that
  // was busy (Busy: Status().failed stays clear, the frame is plain and the caller goes on).
  // Render thread.
  virtual bool Upscale(plume::RenderCommandList* list, const TemporalInputs& inputs) = 0;
  // A dispatch now would have to wait for the runtime, which is occupied making a context: DLSS's
  // Streamline serialises its API (slEvaluateFeature and slGetNewFrameToken are not thread safe) and
  // the first evaluation of a viewport, which makes the feature, holds it for the seconds NGX takes.
  // The frame is then plain and not jittered, and Upscale does not wait (it returns false at once if
  // the runtime got busy after this was asked). Only a context made while another one works can make
  // this true: a live change from one DLSS mode to another. FidelityFX never is. Any thread.
  virtual bool Busy() const { return false; }

  // GPU idle: the worker stops and every context goes.
  virtual void Shutdown() = 0;
};

// The FidelityFX backend, or null in a build without LARECOMP_FFX_SDK_DIR.
std::unique_ptr<TemporalBackend> MakeFfxBackend();
// The DLSS (Streamline) backend, or null in a build without the Streamline headers
// (LARECOMP_HAS_DLSS, cmake/FetchStreamline.cmake).
std::unique_ptr<TemporalBackend> MakeDlssBackend();

// --- what the app's menu asks (gpu.h GetUpscalerAvailability) -----------------------------------
// The backends publish what can run (at the end of their probes and when something fails), the
// temporal system what is running.
void PublishUpscalerAvailability(bool probed, bool fsr31, bool fsr4);  // FidelityFX
void PublishDlssAvailability(bool probed, bool dlss, bool needs_setup, bool after_restart);
void PublishRunningUpscaler(Upscaler running);
// The settings ask for another path than the one running and its context is being made.
void PublishUpscalerSwitching(bool switching);

}  // namespace mcla::gpu
