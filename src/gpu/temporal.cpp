// MCLA GPU renderer -- the groundwork of the temporal upscalers. See temporal.h.

#include "temporal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "backend.h"
#include "constants.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "pass_timing.h"
#include "records.h"
#include "render_scale.h"
#include "resolve.h"
#include "texture_bytes.h"
#include "textures.h"
#include "topology.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal:";
constexpr uint32_t kMaxResetLines = 16;
constexpr uint32_t kMaxLensLines = 24;
constexpr double kPi = 3.14159265358979323846;
// When the targets could not be made at a new scale, the change is asked again after this long, then
// after twice as long, up to kSwitchRetryMax (temporal.h retry_delay_).
constexpr std::chrono::seconds kSwitchRetry{3};
constexpr std::chrono::seconds kSwitchRetryMax{30};

double MillisSince(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

// "FSR 3.1 QUALITY (67%)", or "no upscaler" (the scene at the render scale).
std::string PathText(Upscaler upscaler, UpscaleMode mode) {
  if (upscaler == Upscaler::kOff) {
    return "no upscaler";
  }
  const ModeInfo& info = ModeInfoOf(upscaler, mode);
  return std::format("{} {} ({}%)", UpscalerName(upscaler), info.name, info.percent);
}

// The anchor's three textures, in fetch slots 0, 1, 2 (census 1.1: it has a fourth, a half-size
// colour texture, which is not checked).
constexpr uint32_t kAnchorFormats[3] = {
    uint32_t(xe::TextureFormat::k_16_16_16_16_EXPAND),  // the scene colour (as float)
    uint32_t(xe::TextureFormat::k_8_8_8_8),             // the stencil view of the depth resolve
    uint32_t(xe::TextureFormat::k_24_8_FLOAT),          // the depth, 1 near
};
constexpr const char* kAnchorFormatNames[3] = {"16_16_16_16_EXPAND", "8_8_8_8", "24_8_FLOAT"};

const char* StatusName(TemporalSystem::AnchorStatus status) {
  using S = TemporalSystem::AnchorStatus;
  switch (status) {
    case S::kNotSeen: return "no anchor draw";
    case S::kAccepted: return "accepted";
    case S::kNoMainView: return "no main view draw before it";
    case S::kView4x: return "the main view is at 4x samples";
    case S::kFetches: return "its fetches are not the scene colour, stencil view and depth";
    case S::kTarget: return "its target is not the half-size float image";
    case S::kFrameSize: return temporal::RejectName(temporal::Reject::kFrameSize);
    case S::kConstants: return temporal::RejectName(temporal::Reject::kConstants);
    case S::kViewProjection: return temporal::RejectName(temporal::Reject::kViewProjection);
    case S::kViewInverse: return temporal::RejectName(temporal::Reject::kViewInverse);
    case S::kLens: return temporal::RejectName(temporal::Reject::kLens);
    case S::kCount: break;
  }
  return "?";
}

TemporalSystem::AnchorStatus StatusOf(temporal::Reject reject) {
  using S = TemporalSystem::AnchorStatus;
  switch (reject) {
    case temporal::Reject::kFrameSize: return S::kFrameSize;
    case temporal::Reject::kConstants: return S::kConstants;
    case temporal::Reject::kViewProjection: return S::kViewProjection;
    case temporal::Reject::kViewInverse: return S::kViewInverse;
    case temporal::Reject::kLens: return S::kLens;
    default: return S::kAccepted;
  }
}

const char* ResetName(uint32_t index) {
  static const char* const kNames[TemporalSystem::kResetReasons] = {
      "no anchor in the frame before", "camera cut", "size changed", "gap over 100 ms",
      "no previous matrix"};
  return index < TemporalSystem::kResetReasons ? kNames[index] : "?";
}

std::string ResetText(uint32_t mask) {
  std::string text;
  for (uint32_t i = 0; i < TemporalSystem::kResetReasons; ++i) {
    if (mask & (1u << i)) {
      text += std::format("{}{}", text.empty() ? "" : ", ", ResetName(i));
    }
  }
  return text.empty() ? std::string("none") : text;
}

// A row-major 4x4 of row vectors as 16 floats, the layout of Streamline's float4x4.
void Flatten(const temporal::Mat4& m, float* out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(m.m[r][c]);
    }
  }
}

// The main view draws that draw colour: what says a frame has a 3D scene. Depth-only draws alone do
// not: without MSAA the UI draws one into the main depth key (temporal_identities.h), also in a
// frame with no 3D scene.
uint32_t Colored(const std::array<uint32_t, TemporalSystem::kKinds>& a) {
  return a[TemporalSystem::kColorDepth] + a[TemporalSystem::kColorOnly];
}

}  // namespace

// ---------------------------------------------------------------------------
// the switch and the frame
// ---------------------------------------------------------------------------

void TemporalSystem::SetEnabled(bool enabled) {
  enabled_ = enabled;
  ResetRun();
  window_ = Window{};
  window_start_ = Clock::now();
  shown_mode_ = Mode::kCount;
  pending_mode_ = Mode::kCount;
  pending_frames_ = 0;
  status_logged_.fill(false);
  reset_lines_ = 0;
  logged_lens_[0] = logged_lens_[1] = logged_lens_[2] = 0.0;
  lens_lines_ = 0;
  frame_number_ = 0;
  upscale_misses_ = 0;
  last_dispatch_frame_ = ~uint64_t(0);
  scene_colors_ = {};
  scene_color_scale_ = Scale::None();
  post_stand_in_reported_ = false;
  post_unserved_run_ = 0;
  post_unserved_reported_ = false;
  plain_logged_.fill(false);
  if (enabled && effective_ == Upscaler::kOff && vectors_on_) {
    REXLOG_INFO("{} ON for frame generation: the camera is read from the game's motion blur draw "
                "and each frame's depth and motion vectors are kept; nothing is moved or replaced. "
                "A summary is logged when the game exits",
                kPrefix);
  } else if (enabled) {
    REXLOG_INFO("{} ON for {}: the camera is read from the game's motion blur draw and the 3D "
                "scene is moved by a sub-pixel amount every frame once the upscaler is ready. A "
                "summary is logged when the game exits",
                kPrefix, UpscalerName(effective_));
  } else {
    ClearStandIn();
    motion_.Release();
    REXLOG_INFO("{} off", kPrefix);
  }
}

void TemporalSystem::SyncVectors() {
  if (vectors_wanted_ == vectors_on_) {
    return;
  }
  vectors_on_ = vectors_wanted_;
  if (vectors_on_ && !enabled_) {
    SetEnabled(true);
  } else if (!vectors_on_ && enabled_ && effective_ == Upscaler::kOff) {
    SetEnabled(false);
  }
}

void TemporalSystem::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                HostPasses& passes, PassTiming& pass_timing,
                                TextureCache& textures, ResolveCache& resolve,
                                EdramTargets& edram) {
  motion_.Initialize(device, heap, passes, pass_timing);
  edram_ = &edram;
  device_ = &device;
  frames_ = &frames;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  textures_ = &textures;
  resolve_ = &resolve;
  // The upscaler runtimes, if this build has them: FidelityFX and DLSS (Streamline). A worker
  // thread of each loads its runtime and asks it what this PC can do; the menu lists what they
  // find. Nothing here waits for them. DLSS loads Streamline only when it is the saved choice (or
  // when it is chosen later: temporal_dlss.h).
  ffx_ = MakeFfxBackend();
  if (ffx_) {
    ffx_->Probe(device, frames);
  }
  dlss_ = MakeDlssBackend();
  if (dlss_) {
    dlss_->Probe(device, frames);
  }
}

void TemporalSystem::Shutdown() {
  // The GPU is idle and the heap goes later: the output's slot is not handed back.
  if (textures_) {
    textures_->ClearOverride();
    textures_->DropOverrideViews();
  }
  stand_in_set_ = false;
  output_ = HostImage{};
  if (ffx_) {
    ffx_->Shutdown();
    ffx_.reset();
  }
  if (dlss_) {
    dlss_->Shutdown();
    dlss_.reset();
  }
  PublishRunningUpscaler(Upscaler::kOff);
  PublishKeptUpscaler(Upscaler::kOff);
  PublishUpscalerSwitching(false);
  PublishDlssRefusedModes(0);
  motion_.Shutdown();
  pending_ready_ = false;
  edram_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  textures_ = nullptr;
  resolve_ = nullptr;
}

bool TemporalSystem::RecordAnchorPasses(plume::RenderCommandList* list) {
  if (!pending_ready_) {
    DropPostStandIn();
    return false;
  }
  pending_ready_ = false;
  if (frame_.scale != frame_.output_scale) {
    // The anchor draws at the output's scale, and what it is bound to read is the scene's depth at
    // the render scale: a depth source made below 1x through a copy enlarged to the output's size.
    // The passes read texel for texel at the render size, so they take the product itself: the same
    // fetch bound at the render scale, which is the product's own.
    // CHECK: only a scene scale under 1x (Radeon at 1x, any mode; Ultra Performance at 2x) gets an
    // enlarged copy; above it the two bindings are the same slot.
    // CHECK: the colour and stencil fetches are no longer bound at the render scale here (only the
    // depth is read by the passes); the anchor draw binds all three itself.
    const TextureFetch fetch = ReadTextureFetch(frame_.anchor_textures.fetch[2]);
    if (fetch.is_texture) {
      const TextureCache::Binding bound = textures_->Bind2D(fetch, frame_.scale, list);
      if (bound.from_product) {
        pending_.depth_slot = bound.slot;
      }
    }
  }
  if (!motion_.Record(list, pending_)) {
    if (frame_.upscaler != Upscaler::kOff && frame_.backend_ready) {
      // The upscaler cannot be fed this session: the passes themselves could not be made.
      FailUpscaler("the depth and motion vector passes could not be recorded");
    }
    DropPostStandIn();
    return false;
  }
  RecordUpscale(list);
  DropPostStandIn();
  return true;
}

void TemporalSystem::DropPostStandIn() {
  // The reads before the anchor are over (the anchor draw is being prepared, BeforeTextures).
  CountPostStandIn();
  // An upscaled frame has the anchor's own stand-in by now (the same image, this frame's picture).
  if (frame_.post_on_previous && !frame_.upscaled) {
    ClearStandIn();
  }
  frame_.post_on_previous = false;
}

void TemporalSystem::CountPostStandIn() {
  FrameState& fr = frame_;
  if (!fr.post_on_previous || fr.post_counted || !textures_) {
    return;
  }
  fr.post_counted = true;
  const uint64_t served = textures_->override_binds() - fr.post_binds_at_set;
  window_.post_reads_served += served;
  if (served != 0) {
    ++window_.post_on_previous;
    post_unserved_run_ = 0;
    if (!post_stand_in_reported_) {
      post_stand_in_reported_ = true;
      REXLOG_INFO("{} the game's post chain before the anchor (the half-size downsample the bloom, "
                  "the exposure and the motion blur's half-size layer are made from) reads the "
                  "previous frame's upscaled picture, so the jitter cannot reach them; they are one "
                  "frame late. The scene is drawn at {}x{} for an output of {}x{}; the game "
                  "alternates {} scene colour products and the stand-in covers {} of them",
                  kPrefix, last_render_size_[0], last_render_size_[1], last_output_size_[0],
                  last_output_size_[1], kSceneColorProducts, fr.post_products);
    }
  } else if (++post_unserved_run_ >= kPostUnservedWarn && !post_unserved_reported_) {
    post_unserved_reported_ = true;
    REXLOG_WARN("{} the stand-in for the game's post chain was set in {} frames in a row and no "
                "read before the anchor was given it: the bloom still reads the jittered scene (the "
                "scene colour products it covers are not the ones that read)",
                kPrefix, post_unserved_run_);
  }
}

void TemporalSystem::NoteSceneColor(const void* product, uint32_t page, uint64_t frame) {
  // Newest first; a product that is in the list moves to the front, a new one pushes the oldest
  // out.
  size_t at = scene_colors_.size() - 1;
  for (size_t i = 0; i < scene_colors_.size(); ++i) {
    if (scene_colors_[i].product == product) {
      at = i;
      break;
    }
  }
  for (size_t i = at; i > 0; --i) {
    scene_colors_[i] = scene_colors_[i - 1];
  }
  scene_colors_[0] = SceneColor{product, page, frame};
}

uint64_t TemporalSystem::HeldBytes() const {
  uint64_t bytes = motion_.HeldBytes();
  if (output_.texture) {
    bytes += TextureBytes(output_.width, output_.height, 1, output_.format);
  }
  if (ffx_) {
    bytes += ffx_->Status().context_bytes;
  }
  if (dlss_) {
    bytes += dlss_->Status().context_bytes;
  }
  return bytes;
}

void TemporalSystem::PatchAnchor(SharedConstants& shared) const {
  if (frame_.upscaled) {
    // The anchor's own colour fetch (slot 0) was bound before the upscale existed.
    shared.texture_2d[0] = frame_.anchor_color_slot;
  }
}

bool TemporalSystem::HoldsAaOff(bool aa_asked) {
  const bool hold = effective_ != Upscaler::kOff;
  if (hold && aa_asked && !aa_hold_reported_) {
    aa_hold_reported_ = true;
    REXLOG_INFO("{} FXAA and SMAA (mcla_gpu_aa) are held off while {} is on", kPrefix,
                UpscalerName(effective_));
  }
  if (!hold) {
    aa_hold_reported_ = false;
  }
  return hold;
}

// ---------------------------------------------------------------------------
// the upscaler
// ---------------------------------------------------------------------------

bool TemporalSystem::CanRun(Upscaler upscaler, const UpscalerAvailability& available) const {
  return BackendFor(upscaler) &&
         ((upscaler == Upscaler::kFsr31 && available.fsr31) ||
          (upscaler == Upscaler::kFsr4 && available.fsr4) ||
          (upscaler == Upscaler::kDlss && available.dlss));
}

void TemporalSystem::ReadUpscalerChoice() {
  settings_ = ReadUpscalerSettings();
  const UpscalerAvailability available = GetUpscalerAvailability();
  Upscaler effective = Upscaler::kOff;
  const TemporalBackend* chosen_backend = BackendFor(settings_.upscaler);
  if (CanRun(settings_.upscaler, available)) {
    effective = settings_.upscaler;
  }
  const bool chosen_probed =
      settings_.upscaler == Upscaler::kDlss ? available.dlss_probed : available.probed;
  // DLSS chosen in a Vulkan game that was started without it: it runs from the next start. The
  // renderer keeps the path it runs until then (UpdatePath), it does not turn it off.
  restart_pending_ = effective == Upscaler::kOff && settings_.upscaler == Upscaler::kDlss &&
                     chosen_backend && chosen_probed && available.dlss_after_restart;
  // A line when what the choice comes to changes. A choice this PC cannot run is off (the saved
  // choice stays as it is, and comes back where it can run), except the one that waits for a
  // start, which leaves the running path alone.
  const uint32_t key = uint32_t(settings_.upscaler) | uint32_t(effective) << 2 |
                       uint32_t(chosen_probed) << 4 | uint32_t(chosen_backend != nullptr) << 5;
  if (!choice_reported_ || key != choice_key_) {
    choice_reported_ = true;
    choice_key_ = key;
    if (settings_.upscaler != Upscaler::kOff) {
      if (effective != Upscaler::kOff) {
        REXLOG_INFO("{} upscaling: {} ({} {}%), sharpness {:.2f}", kPrefix, UpscalerName(effective),
                    ModeInfoOf(effective, settings_.mode).name,
                    ModeInfoOf(effective, settings_.mode).percent,
                    effective == Upscaler::kDlss ? 0.0f : settings_.sharpness);
      } else if (!chosen_backend) {
        REXLOG_INFO("{} {} is chosen but this build has no runtime for it: treated as off", kPrefix,
                    UpscalerName(settings_.upscaler));
      } else if (chosen_probed) {
        if (restart_pending_) {
          // A Vulkan game started without DLSS: its device was not made through Streamline (that
          // costs seconds at every start, so it is done only when DLSS is chosen at the start). The
          // choice is saved; the pause menu says RESTART and offers it. What the game runs goes on:
          // UpdatePath says which path (the line "keeps ... until the restart").
        } else {
          REXLOG_INFO("{} {} is chosen but this PC cannot run it{}: treated as off (the choice is "
                      "kept)",
                      kPrefix, UpscalerName(settings_.upscaler),
                      settings_.upscaler == Upscaler::kDlss && available.dlss_needs_setup
                          ? " (its files are missing: it needs setup)"
                          : "");
        }
      }
    }
  }
  // What the frames follow changes only at a frame boundary, once the new path is ready (UpdatePath).
  wanted_ = effective;
  wanted_mode_ = effective != Upscaler::kOff ? settings_.mode : UpscaleMode::kNativeAa;
}

// ---------------------------------------------------------------------------
// the path: the scene's size, the upscaler and its mode, changed while the game runs
// ---------------------------------------------------------------------------

bool TemporalSystem::GuestFrameSize(uint32_t out[2], bool quiet) const {
  if (last_camera_.width != 0 && last_camera_.height != 0) {
    out[0] = last_camera_.width;
    out[1] = last_camera_.height;
    return true;
  }
  return edram_ && edram_->GuestScreen(&out[0], &out[1], quiet);
}

BackendRequest TemporalSystem::RequestFor(Upscaler upscaler, UpscaleMode mode,
                                          const uint32_t guest[2], Scale output) const {
  const double ratio = upscaler != Upscaler::kOff ? ModeInfoOf(upscaler, mode).ratio : 1.0;
  const Scale scene = ChooseSceneScale(output, ratio).scale;
  BackendRequest request;
  request.upscaler = upscaler;
  request.mode = mode;
  // The same two edges the anchor reports for a frame (OnAnchor: the main view at the scene's
  // scale, the anchor's own target at the images of the screen's), so that the context made before
  // the switch is the one the first frame asks for.
  // CHECK: that the sizes of the first frame after a switch are these ("live change ... ran its
  // first upscaled frame (frame N, WxH to WxH)" in the log): a different size makes BeginFrame ask
  // for another context, which is plain frames until it is made (the old behaviour of a size change).
  for (int axis = 0; axis < 2; ++axis) {
    request.render_size[axis] = scene.Edge(guest[axis]);
    request.output_size[axis] = output.Edge(guest[axis]);
  }
  return request;
}

ModeSizes TemporalSystem::DlssModeSizes(const uint32_t guest[2], Scale output) const {
  // The same sizes RequestFor makes a context for: the scene's render size per mode on the scale
  // grid, and the output's.
  ModeSizes sizes;
  for (int axis = 0; axis < 2; ++axis) {
    sizes.output[axis] = output.Edge(guest[axis]);
  }
  for (size_t mode = 0; mode < size_t(UpscaleMode::kCount); ++mode) {
    const Scale scene =
        ChooseSceneScale(output, ModeInfoOf(Upscaler::kDlss, UpscaleMode(mode)).ratio).scale;
    for (int axis = 0; axis < 2; ++axis) {
      sizes.render[mode][axis] = scene.Edge(guest[axis]);
    }
  }
  return sizes;
}

// CHECK: at 1280x720 with DLSS chosen: a start with ULTRA PERFORMANCE saved logs "ULTRA PERFORMANCE
// (33%) cannot run ... PERFORMANCE (50%) runs instead", DLSS stays available (no "DLSS is off for this
// session"), and the first upscaled frame is PERFORMANCE at 640x360 to 1280x720; picking ULTRA PERF.
// live from DLAA logs "... DLAA (100%) keeps running" and nothing switches; the same at 2560x1440
// (Performance there is 1280x720 to 2560x1440, so the scene targets do move at a start); 1920x1080 and
// 3840x2160 run ULTRA PERFORMANCE itself. A render scale change that leaves the running mode unable to
// run (ULTRA PERFORMANCE at 4K, then a window of another size) moves to PERFORMANCE at the new output.
UpscaleMode TemporalSystem::ResolveDlssMode(UpscaleMode wanted, const ModeSizes& sizes, bool* any) {
  *any = true;
  const ModeVerdicts verdicts = dlss_->Modes(sizes);
  const auto runs = [&verdicts](UpscaleMode mode) {
    return verdicts.verdict[size_t(mode)] != ModeVerdict::kRefused;
  };
  if (runs(wanted)) {
    dlss_refusal_key_ = 0;
    return wanted;
  }
  // The mode the settings ask for cannot run at this output. The DLSS mode that runs goes on if it
  // still can (a pick in the menu of a mode that cannot run changes nothing, as RENDER SCALE keeps the
  // scale that runs when a larger one is too big); a start or an output change that left nothing
  // running takes the nearest mode that can, toward higher quality first.
  UpscaleMode use = wanted;
  bool kept = false;
  bool found = false;
  if (effective_ == Upscaler::kDlss && runs(mode_in_use_)) {
    use = mode_in_use_;
    kept = true;
    found = true;
  } else {
    for (int m = int(wanted) - 1; m >= 0 && !found; --m) {
      if (runs(UpscaleMode(m))) {
        use = UpscaleMode(m);
        found = true;
      }
    }
    for (int m = int(wanted) + 1; m < int(UpscaleMode::kCount) && !found; ++m) {
      if (runs(UpscaleMode(m))) {
        use = UpscaleMode(m);
        found = true;
      }
    }
  }
  *any = found;
  const uint64_t key = 1u | uint64_t(wanted) << 4 | uint64_t(found ? use : wanted) << 8 |
                       uint64_t(found) << 12 | uint64_t(kept) << 13 |
                       uint64_t(sizes.output[0]) << 16 | uint64_t(sizes.output[1]) << 40;
  if (key != dlss_refusal_key_) {
    dlss_refusal_key_ = key;
    const std::string& why = verdicts.why[size_t(wanted)];
    const std::string tail =
        !found ? std::string("no DLSS mode takes this output, so nothing upscales the scene (the "
                             "setting is kept)")
               : std::format("{} {} (the setting stays {})", PathText(Upscaler::kDlss, use),
                             kept ? "keeps running" : "runs instead",
                             ModeInfoOf(Upscaler::kDlss, wanted).name);
    REXLOG_INFO("{} {} cannot run at an output of {}x{}: {}. {}", kPrefix,
                PathText(Upscaler::kDlss, wanted), sizes.output[0], sizes.output[1],
                why.empty() ? std::string("DLSS does not take its size here") : why, tail);
  }
  return use;
}

void TemporalSystem::AdoptStartPath() {
  // The scene's first targets are made for the upscaler as the start found it (gpu.h
  // GetStartUpscale, EdramTargets::DecideScene): that is the path the frames follow until the
  // settings ask for another one.
  const StartUpscale start = PeekStartUpscale();
  edram_->SceneScale();  // decides the scene's scale now if no scene identity has been used yet
  if (start.runs && BackendFor(start.upscaler)) {
    effective_ = start.upscaler;
    mode_in_use_ = start.mode;
    SetEnabled(true);
  }
}

bool TemporalSystem::UpscalerOwnsOutput() const {
  // DLSS chosen in a Vulkan game that was started without it waits for a restart: the path that runs
  // (FSR 3.1 or 4 as it was, or none) goes on, with the output it has.
  return restart_pending_ ? effective_ != Upscaler::kOff : wanted_ != Upscaler::kOff;
}

Scale TemporalSystem::UpscalerOutputScale(Scale fallback) const {
  // The window's size comes with any choice WantedRenderScale gives; the output is the screen's for it.
  const RenderScaleChoice window = WantedRenderScale();
  if (window.window_width == 0 || window.window_height == 0) {
    return fallback;
  }
  return ChooseUpscalerOutputScale(window.asked, window.fsr, window.preset, window.window_width,
                                   window.window_height)
      .scale;
}

// CHECK: not run (written without a build). In 720p, 1080p and 1440p windows at the default (Upscaling
// Off, mcla_gpu_resolution_scale 0) with each upscaler picked from the menu and back: the upscaled
// picture is the window's size ("live change: ...; upscaling to the window's 1920x1080" and the first
// upscaled frame "1280x720 to 1920x1080" for QUALITY at 1080p), one "making the new context" line per
// change (not one at the old output and another at the new), and going back to Off leaves the picture
// at the window's size. With a saved scale other than 0 (say 2) the same switch moves the output from
// that scale to the screen's and back in one step each way. A window resized while an upscaler runs
// moves the output after 0.4 s (DLSS: a new context of several seconds per settled size). A game
// started with DLSS chosen on a PC that cannot run it and a saved scale other than 0 starts at the
// screen's scale and moves to the saved one once at the first frame boundary (log line "live change").
void TemporalSystem::ReadRenderScale() {
  // The cvars and the window as they are now. The window's size is the swap chain's last one: while
  // none has been published (or a minimised window left none) there is nothing to measure the screen's
  // scale or a preset against, and what is wanted stays what it was.
  RenderScaleChoice choice = WantedRenderScale();
  if (choice.from_window() && (choice.window_width == 0 || choice.window_height == 0)) {
    return;
  }
  // While a temporal upscaler is the active choice its output is the screen's, the window's 16:9 area,
  // whatever mcla_gpu_resolution_scale and an FSR1 preset say; the scene is then drawn at the vendor's
  // ratio of it (SwitchPath, ChooseSceneScale). The settings' own scale comes back when it is off.
  // Without a window size (checked above only for a choice made from the window) the settings' scale
  // stays what is wanted.
  const bool upscaling = UpscalerOwnsOutput() && choice.window_width != 0 && choice.window_height != 0;
  if (upscaling) {
    choice = ChooseUpscalerOutputScale(choice.asked, choice.fsr, choice.preset, choice.window_width,
                                       choice.window_height);
  }
  if (choice.invalid && !render_invalid_logged_) {
    render_invalid_logged_ = true;
    REXLOG_ERROR("{} {}", kPrefix, DescribeRenderScale(choice));
  } else if (!choice.invalid) {
    render_invalid_logged_ = false;
  }
  render_choice_ = choice;
  const bool flipped = upscaling != render_upscaling_;
  render_upscaling_ = upscaling;
  if (choice.scale != render_wanted_) {
    render_wanted_ = choice.scale;
    render_wanted_since_ = Clock::now();
    // A scale that moves because an upscaler went on or off is one change with it, not a drag through
    // sizes: UpdatePath takes it without the wait, so that the upscaler's context (or the plain scene)
    // is made once, for the output it will have.
    render_wanted_at_once_ = flipped;
  }
}

void TemporalSystem::CancelStage(const char* why) {
  if (!stage_.active) {
    return;
  }
  if (TemporalBackend* backend = BackendFor(stage_.upscaler)) {
    backend->CancelPrepared();
  }
  if (why) {
    REXLOG_INFO("{} live change to {} dropped: {}", kPrefix,
                PathText(stage_.upscaler, stage_.mode), why);
  }
  stage_ = Stage{};
}

void TemporalSystem::UpdatePath(plume::RenderCommandList* list) {
  if (!edram_ || !device_) {
    return;
  }
  if (!path_adopted_) {
    // The start's decision is the game's (its renderer's creation, or the scene's first targets):
    // not forced here, where an idle frame of the boot screens may come long before the game.
    if (!PeekStartUpscale().decided) {
      return;
    }
    path_adopted_ = true;
    AdoptStartPath();
  }
  Upscaler to = wanted_;
  UpscaleMode to_mode = to == Upscaler::kOff ? UpscaleMode::kNativeAa : wanted_mode_;
  // The chosen upscaler runs only from the next start (restart_pending_): the path that runs stays,
  // upscaler and mode as the scene is drawn for them now, which is what the player saw when picking
  // it (the settings' own mode is the new upscaler's, not this path's). A path whose upscaler
  // cannot run any more (its backend failed) is not kept: that is the ordinary turn-off below.
  const bool keep_running = restart_pending_ && (effective_ == Upscaler::kOff ||
                                                 CanRun(effective_, GetUpscalerAvailability()));
  PublishKeptUpscaler(keep_running ? effective_ : Upscaler::kOff);
  if (keep_running) {
    to = effective_;
    to_mode = mode_in_use_;
    if (!hold_reported_) {
      hold_reported_ = true;
      REXLOG_INFO("{} DLSS is chosen, but this Vulkan game was started without it (its device is "
                  "made through Streamline only when DLSS is the saved choice at the start): the "
                  "choice is saved and applies at the next start; the game keeps {} until the "
                  "restart",
                  kPrefix, PathText(effective_, mode_in_use_));
    }
  } else {
    hold_reported_ = false;
  }
  const Clock::time_point now = Clock::now();
  // The images of the screen: the render scale the settings ask for (the screen's while an upscaler is
  // the active choice), once it has stood for a moment (kScaleSettle; not for a scale that moved with
  // the upscaler going on or off). Until then the scale that runs stays the one the path is made for,
  // so no target and no upscaler context is made for the steps of a window drag or of a row stepped
  // through.
  const Scale output_now = edram_->resolution_scale();
  Scale to_output = output_now;
  if (!render_wanted_.none() && render_wanted_ != output_now &&
      (render_wanted_at_once_ || now - render_wanted_since_ >= kScaleSettle)) {
    to_output = render_wanted_;
  }
  if (to_output == output_now) {
    PublishRenderScaleStatus(output_now, Scale::None());  // nothing is asked that could not be made
  }
  // DLSS takes only some render sizes per mode at an output size (temporal.h "Changing the path"):
  // the backend is told the sizes this path is made for, so that it finds out which modes cannot run
  // there (the pause menu marks them), and a mode that cannot run is not asked for: the one that
  // runs goes on, or the nearest one that can takes its place. Not for the DLSS that waits for a
  // restart (the path that runs stays).
  if (dlss_) {
    uint32_t fit_guest[2] = {};
    if (GuestFrameSize(fit_guest, /*quiet=*/true)) {
      const ModeSizes sizes = DlssModeSizes(fit_guest, to_output);
      dlss_->AskModes(sizes);
      if (to == Upscaler::kDlss && !keep_running) {
        bool any = true;
        to_mode = ResolveDlssMode(to_mode, sizes, &any);
        if (!any) {
          to = Upscaler::kOff;
          to_mode = UpscaleMode::kNativeAa;
        }
      } else {
        dlss_refusal_key_ = 0;  // DLSS is not what the path is for: the next time says it again
      }
    }
  }
  // What the frames are steered to, for the host MSAA level (UpscalerNext): its pipelines are made
  // while the path's context is, and the level changes once the path has settled (msaa_level.h).
  next_ = to;
  // The wait after a change that could not be made belongs to that change: another one (a smaller
  // scale picked after "too big", another mode) is tried at once.
  const uint64_t change_key = (uint64_t(1) << 63) | uint64_t(to) | uint64_t(to_mode) << 8 |
                              uint64_t(to_output.n) << 16;
  if (change_key != retry_key_) {
    retry_key_ = change_key;
    retry_after_ = Clock::time_point{};
    retry_delay_ = kSwitchRetry;
    switch_failure_logged_ = false;
  }
  if (to == effective_ && to_mode == mode_in_use_ && to_output == output_now) {
    CancelStage(keep_running ? "DLSS is chosen and waits for the restart; the running path stays"
                : dlss_refusal_key_ != 0 ? "DLSS does not take that mode at this output; the running path stays"
                                         : "the setting is back on the path that runs");
    change_pending_ = false;
    PublishUpscalerSwitching(false);
    return;
  }
  if (!change_pending_) {
    change_pending_ = true;
    change_seen_ = now;
  }
  // A change whose targets could not be made waits quietly for its next try: the menu's (WAIT) is for
  // a context that is being made.
  PublishUpscalerSwitching(now >= retry_after_);
  if (now < retry_after_) {
    return;
  }
  if (to == Upscaler::kOff) {
    // No context to wait for: the scene goes back to the images of the screen's scale now.
    CancelStage(nullptr);
    SwitchPath(list, Upscaler::kOff, UpscaleMode::kNativeAa, nullptr, to_output);
    PublishUpscalerSwitching(change_pending_ && Clock::now() >= retry_after_);
    return;
  }
  TemporalBackend* backend = BackendFor(to);
  uint32_t guest[2] = {};
  if (!backend || !GuestFrameSize(guest)) {
    return;  // the game has not set its screen size yet
  }
  const BackendRequest request = RequestFor(to, to_mode, guest, to_output);
  if (!stage_.active || stage_.upscaler != to || stage_.mode != to_mode ||
      stage_.output != to_output || !(stage_.request == request)) {
    CancelStage(nullptr);
    stage_.active = true;
    stage_.upscaler = to;
    stage_.mode = to_mode;
    stage_.output = to_output;
    stage_.request = request;
    REXLOG_INFO("{} live change: {} -> {}{}: making the new context ({}x{} to {}x{}) while {} goes "
                "on; the switch is made at the first frame boundary after it is ready",
                kPrefix, PathText(effective_, mode_in_use_), PathText(to, to_mode),
                to_output != output_now
                    ? std::format(", render scale {}x -> {}x", output_now, to_output)
                    : std::string(),
                request.render_size[0], request.render_size[1], request.output_size[0],
                request.output_size[1],
                effective_ == Upscaler::kOff ? "the plain scene" : "the old path");
  }
  // Every frame while it is wanted (the same request again does nothing; DLSS's first use starts
  // Streamline here, a step at a time).
  backend->Prepare(request);
  if (backend->PreparedReady(request)) {
    SwitchPath(list, to, to_mode, &request, to_output);
    PublishUpscalerSwitching(change_pending_ && Clock::now() >= retry_after_);
  }
}

void TemporalSystem::PrewarmDlss() {
  // A PC that can run DLSS, whose game was not started with it: NGX's first-time work (Streamline's
  // own start on D3D12, then the first feature of each kind: seconds each, 1.2 s to 8.4 s measured)
  // is done now, in the background, with features that are freed at once, so that the first switch to
  // DLSS from the menu finds it done (temporal_dlss.cpp, "the pre-warm"). Nothing here waits for it.
  if (!dlss_ || !path_adopted_ || !edram_ || !dlss_->PrewarmPending()) {
    return;
  }
  // The settings ask for DLSS (or it runs): the backend ends the pre-warm itself at its Request or
  // Prepare, and the real context is the worker's first job.
  if (wanted_ == Upscaler::kDlss || effective_ == Upscaler::kDlss) {
    return;
  }
  uint32_t guest[2] = {};
  if (!GuestFrameSize(guest, /*quiet=*/true)) {
    return;  // the game has not set its screen size yet
  }
  const Clock::time_point now = Clock::now();
  if (prewarm_wait_since_ == Clock::time_point{}) {
    prewarm_wait_since_ = now;
  }
  if (now - prewarm_wait_since_ < kPrewarmDelay) {
    return;
  }
  UpscaleMode mode = UpscaleMode::kNativeAa;
  if (dlss_->PrewarmNext(&mode)) {
    // The context a live change to this mode would ask for (the same sizes), so that what the
    // pre-warm made is what the switch needs: at the output DLSS would have in this window (the
    // screen's), which the render scale is not unless the settings leave it there while no upscaler
    // is on.
    dlss_->Prewarm(
        RequestFor(Upscaler::kDlss, mode, guest, UpscalerOutputScale(edram_->resolution_scale())));
  }
}

bool TemporalSystem::SwitchPath(plume::RenderCommandList* list, Upscaler to, UpscaleMode mode,
                                const BackendRequest* request, Scale output) {
  const Clock::time_point began = Clock::now();
  const Upscaler from = effective_;
  const UpscaleMode from_mode = mode_in_use_;
  const Scale output_before = edram_->resolution_scale();
  const double ratio = to != Upscaler::kOff ? ModeInfoOf(to, mode).ratio : 1.0;
  const SceneScaleChoice choice = ChooseSceneScale(output, ratio);
  const Scale scene_before = edram_->SceneScale();
  // The targets first (the scene's, and with a new render scale every image of the screen): the one
  // step that can fail (video memory), and then nothing has changed and the old path runs on.
  // CHECK: the switch frame under the D3D12 debug layer and Vulkan validation (the new targets'
  // first clear at the frame's start, their layouts, the retired textures behind the frames, the
  // resolve products made again at the new scale), and that no frame between two paths shows a
  // hitch beyond the one that makes the textures.
  // CHECK: a render scale change with the upscaler running: the context was made for the new sizes
  // (the log line "live change ... making the new context (WxH to WxH)"), and the first frame after
  // the switch asks for the same ones (no "context is not ready" plain frames).
  if (!edram_->SetScales(output, choice.scale, list)) {
    // Asked again after a while, less often each time: a size the card cannot hold keeps failing.
    retry_delay_ = switch_failure_logged_ ? std::min(retry_delay_ * 2, kSwitchRetryMax)
                                          : kSwitchRetry;
    retry_after_ = began + retry_delay_;
    if (output != output_before) {
      PublishRenderScaleStatus(output_before, output);  // the menu says the scale is too big
    }
    if (!switch_failure_logged_) {
      switch_failure_logged_ = true;
      REXLOG_WARN("{} live change to {}{} not made: the targets could not be made at the new size "
                  "(images of the screen {}x -> {}x, 3D scene {}x -> {}x; video memory is most "
                  "likely short): the game stays at {}x and {}, and the change is asked again "
                  "after {} seconds, then less often",
                  kPrefix, PathText(to, mode),
                  output != output_before ? std::format(", render scale {}x", output)
                                          : std::string(),
                  output_before, output, scene_before, choice.scale, output_before,
                  PathText(from, from_mode), retry_delay_.count());
    }
    return false;
  }
  switch_failure_logged_ = false;
  retry_delay_ = kSwitchRetry;
  retry_after_ = Clock::time_point{};
  PublishRenderScaleStatus(output, Scale::None());
  const double targets_ms = MillisSince(began);

  TemporalBackend* backend = BackendFor(to);
  bool committed = true;
  if (to != Upscaler::kOff) {
    committed = backend && request && backend->Commit(*request);
    if (!committed) {
      // The prepared context went between the check and now (the backend failed meanwhile): the
      // upscaler is treated as off from the next frame on.
      REXLOG_WARN("{} the context made for {} was gone at the switch; the frame asks for it again",
                  kPrefix, PathText(to, mode));
    }
  }
  effective_ = to;
  mode_in_use_ = mode;
  stage_ = Stage{};
  change_pending_ = false;
  const bool enable = to != Upscaler::kOff || vectors_on_;
  if (enable != enabled_) {
    SetEnabled(enable);  // starts or ends the run (and says so)
  } else if (enable) {
    // One upscaler path to another with the system on: the run goes on (the camera, the cut rules)
    // and the new context starts its own history with its first dispatch.
    misses_ = 0;
    upscale_misses_ = 0;
    disarmed_ = false;
  }
  if (enable && request) {
    // The frame that follows asks for the same context (BeginFrame's Request) and is armed at once
    // when the run went on; the sizes are the ones this switch made the scene and the context for.
    request_ = *request;
    request_made_ = true;
    for (int axis = 0; axis < 2; ++axis) {
      last_render_size_[axis] = request->render_size[axis];
      last_output_size_[axis] = request->output_size[axis];
    }
    off_since_ = Clock::time_point{};
    // The other runtime holds nothing for this path (the context of an upscaler that was turned off
    // a moment ago is kept for a few seconds, so that turning it on again is instant).
    for (TemporalBackend* other : {ffx_.get(), dlss_.get()}) {
      if (other && other != backend) {
        other->Release();
      }
    }
  }

  // When the setting changed: a render scale was taken only after it had stood for kScaleSettle, so
  // its own clock is the earlier one.
  Clock::time_point asked = change_seen_;
  if (output != output_before && render_wanted_since_ != Clock::time_point{} &&
      render_wanted_since_ < asked) {
    asked = render_wanted_since_;
  }
  const double waited_ms = std::chrono::duration<double, std::milli>(began - asked).count();
  const double total_ms = MillisSince(began);
  const bool path_changed = from != to || from_mode != mode;
  std::string render_text;
  if (output != output_before) {
    // The render scale moved: from, to, and what the settings said (the screen's scale follows the
    // window, so the reason is the window's size).
    render_text = std::format(
        "; render scale {}x ({}x{}) -> {}x ({}x{}), {}", output_before,
        output_before.Edge(kAssumedScreenWidth), output_before.Edge(kAssumedScreenHeight), output,
        output.Edge(kAssumedScreenWidth), output.Edge(kAssumedScreenHeight),
        render_choice_.scale == output ? DescribeRenderScale(render_choice_)
                                       : std::string("by the settings"));
  }
  if (enable) {
    // What the upscaler makes its picture for: said at every switch that has one, which is once per
    // change of the upscaler, its mode or the output (a window of another size).
    render_text += std::format("; upscaling to {}",
                               DescribeUpscalerTarget(output, render_choice_.window_width,
                                                      render_choice_.window_height));
  }
  REXLOG_INFO("{} live change: {}{}: {}; the setting changed {:.0f} ms before the switch, the 3D "
              "scene {}, switched in {:.2f} ms ({:.2f} ms of it the targets) at the boundary "
              "before frame {}{}",
              kPrefix,
              path_changed ? std::format("{} -> {}", PathText(from, from_mode), PathText(to, mode))
                           : PathText(to, mode),
              render_text,
              committed ? "its context was ready" : "its context was missing",
              std::max(waited_ms, 0.0),
              scene_before == choice.scale
                  ? std::format("stays at {}x", choice.scale)
                  : std::format("moves from {}x to {}x (the output at {}x)", scene_before,
                                choice.scale, edram_->resolution_scale()),
              total_ms, targets_ms, frame_number_,
              enable ? "; the first upscaled frame is logged when it has run"
                     : "; nothing to wait for");
  switch_asked_ = asked;
  switch_log_pending_ = enable;
  return true;
}

void TemporalSystem::MakeRequest(BackendRequest* request) const {
  request->upscaler = effective_;
  request->mode = mode_in_use_;
  // What the last accepted frame had: the scene at the render size, the output at the size of the
  // images of the screen (one size at Native AA).
  request->render_size[0] = last_render_size_[0];
  request->render_size[1] = last_render_size_[1];
  request->output_size[0] = last_output_size_[0];
  request->output_size[1] = last_output_size_[1];
}

uint32_t TemporalSystem::JitterPhases(double ratio, uint32_t from_runtime) {
  if (from_runtime != 0) {
    return std::min(from_runtime, kMaxPhases);
  }
  const double wanted = std::ceil(double(kPhasesAtRatio1) * ratio * ratio - 1e-6);
  return uint32_t(std::clamp(wanted, 1.0, double(kMaxPhases)));
}

void TemporalSystem::ClearStandIn() {
  if (stand_in_set_ && textures_) {
    textures_->ClearOverride();
  }
  stand_in_set_ = false;
  frame_.upscaled = false;
}

void TemporalSystem::StandInForPostChain() {
  // The post chain's first reads of the scene colour come before the anchor, so before this frame's
  // upscale (temporal.h, "The post chain before the anchor"). The upscale needs this frame's camera
  // and the anchor's own fetches, and the camera is known only at the anchor's draw (nothing before
  // it carries one in a form that has been verified); so those reads are given what the previous
  // frame's upscale made: the output image still holds it, and the stand-in the anchor sets later
  // is the same image with this frame's picture in it. Nothing before the anchor writes the image,
  // so the draws recorded before and after it see the frame before's and this frame's picture each.
  // Which product is this frame's is not known yet either (the anchor's fetch comes later), and
  // it is not the last frame's: the game draws the scene into one of a pair of products and
  // alternates them (the log showed the downsample reading base 07fe0000 and 07c48000 in turn,
  // 194 reads a second, none on the stand-in while it covered the other one). The stand-in covers
  // the last two products the anchor read, scene_colors_: this frame's is the one of two frames
  // ago.
  // CHECK: that the half-size downsample (sub_8260DA68's first sub_8260D678 call, dofObj+92 into
  // dofObj+12, read in the recompiled guest code, not seen in a draw log) is the only reader of
  // the scene colour product before the anchor; any other one is redirected the same way and
  // shows in the cause of a flicker that stays.
  FrameState& fr = frame_;
  if (!fr.armed || fr.upscaler == Upscaler::kOff || !textures_ || stand_in_set_) {
    return;
  }
  // The frame before was upscaled (so the image holds its picture), by a gap the history rule
  // accepts. A camera cut cannot be known yet: the cut frame's bloom and exposure show the view
  // before it, for that one frame.
  if (last_dispatch_frame_ == ~uint64_t(0) || last_dispatch_frame_ + 1 != fr.frame ||
      std::chrono::duration<double, std::milli>(Clock::now() - last_anchor_time_).count() >
          kMaxGapMs) {
    return;
  }
  // The scene drawn at the output's size (Native AA, DLAA) has no sub-pixel glint the jitter
  // drops: its bloom is as steady as without the jitter (measured: 0.2% of bright pixels flicker
  // against 3% at Quality), and the lag would buy nothing.
  if (last_render_size_[0] == last_output_size_[0] && last_render_size_[1] == last_output_size_[1]) {
    return;
  }
  if (!output_.texture || output_.layout != plume::RenderTextureLayout::SHADER_READ ||
      output_.width != last_output_size_[0] || output_.height != last_output_size_[1]) {
    return;
  }
  // The products the anchor read in the frames just before (the newest is the frame before's).
  // An older one is not trusted: a frame in between that was not upscaled, or a size change.
  static_assert(kSceneColorProducts == 2, "TextureCache::Override holds a pair of products");
  TextureCache::Override stand_in;
  uint32_t products = 0;
  for (const SceneColor& c : scene_colors_) {
    if (!c.product || c.frame + kSceneColorMaxAge < fr.frame) {
      continue;
    }
    if (products == 0) {
      stand_in.product = c.product;
      stand_in.page = c.page;
    } else {
      stand_in.other_product = c.product;
      stand_in.other_page = c.page;
    }
    ++products;
  }
  if (products == 0) {
    return;
  }
  stand_in.texture = output_.texture.get();
  stand_in.format = output_.format;
  stand_in.scale = scene_color_scale_;
  textures_->SetOverride(stand_in);
  stand_in_set_ = true;
  fr.post_on_previous = true;
  fr.post_products = products;
  fr.post_binds_at_set = textures_->override_binds();
  ++window_.post_stand_in_set;
}

void TemporalSystem::ReleaseUpscaler() {
  // No upscaler for a while: the context and the output image go (the GPU may still be reading
  // them: the backend retires the context behind the frames, the image goes through the ring).
  if (ffx_) {
    ffx_->Release();
  }
  if (dlss_) {
    dlss_->Release();
  }
  if (textures_) {
    textures_->ReleaseOverrideViews();
  }
  if (passes_) {
    passes_->ReleaseImage(output_);
  }
  if (request_made_) {
    REXLOG_INFO("{} the upscaler has been off for a while: its context and output are released",
                kPrefix);
  }
  request_made_ = false;
  off_since_ = Clock::time_point{};
}

void TemporalSystem::FailUpscaler(const std::string& why) {
  if (TemporalBackend* backend = BackendFor(effective_)) {
    backend->Fail(why);
  }
}

bool TemporalSystem::RecordUpscale(plume::RenderCommandList* list) {
  FrameState& fr = frame_;
  TemporalBackend* backend = BackendFor(effective_);
  if (effective_ == Upscaler::kOff || !backend) {
    return false;
  }
  // A frame the upscaler does not take is plain: counted, the first of each kind logged. Armed
  // frames that keep missing end it (EndFrame).
  const auto plain = [&](uint64_t& counter, size_t kind, const std::string& why) {
    ++counter;
    if (!plain_logged_[kind]) {
      plain_logged_[kind] = true;
      REXLOG_INFO("{} frame {} is not upscaled: {} (the first of its kind; the summaries count them)",
                  kPrefix, fr.frame, why);
    }
    return false;
  };
  if (!fr.backend_ready || !backend->Ready(request_)) {
    return plain(window_.upscale_plain_not_ready, 0, "the upscaler's context is not ready");
  }
  // The scene at the render size (the vectors, the depth and the colour the anchor reads are made
  // at it); the output at the output's (F4-DESIGN.md 4).
  const uint32_t width = fr.render_size[0];
  const uint32_t height = fr.render_size[1];
  const uint32_t out_width = fr.output_size[0];
  const uint32_t out_height = fr.output_size[1];
  if (out_width == 0 || out_height == 0 || out_width < width || out_height < height) {
    return plain(window_.upscale_plain_no_color, 2,
                 std::format("the output {}x{} is not at least the render size {}x{}", out_width,
                             out_height, width, height));
  }
  const TemporalMotion::Kept kept = motion_.kept();
  if (!kept.valid || kept.frame != fr.frame || !kept.depth || !kept.motion ||
      !kept.depth->texture || !kept.motion->texture || kept.depth->width != width ||
      kept.depth->height != height || kept.motion->width != width || kept.motion->height != height) {
    return plain(window_.upscale_plain_no_vectors, 1,
                 "the depth and the motion vectors of this frame are not there");
  }

  // The colour the anchor reads: the scene's resolve product (R16G16B16A16_FLOAT, linear).
  const TextureFetch fetch = ReadTextureFetch(fr.anchor_textures.fetch[0]);
  ResolveCache::Product* product =
      fetch.is_texture ? resolve_->Find(fetch.key, fetch.image_base_page) : nullptr;
  if (!product || !product->texture) {
    return plain(window_.upscale_plain_no_color, 2, "the anchor's colour is not a resolve product");
  }
  if (product->format != plume::RenderFormat::R16G16B16A16_FLOAT || product->key.levels() != 1 ||
      product->scale.Edge(product->key.width) != width ||
      product->scale.Edge(product->key.height) != height ||
      product->layout != plume::RenderTextureLayout::SHADER_READ) {
    return plain(window_.upscale_plain_no_color, 2,
                 std::format("the anchor's colour product is {} {}x{}, {} level(s), layout {} "
                             "(wanted R16G16B16A16_FLOAT {}x{}, one level, readable)",
                             format::PlumeFormatName(product->format),
                             product->scale.Edge(product->key.width),
                             product->scale.Edge(product->key.height), product->key.levels(),
                             int(product->layout), width, height));
  }

  // The output.
  constexpr plume::RenderFormat kOutputFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
  constexpr plume::RenderTextureFlags kOutputFlags =
      plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  if (output_.texture && (output_.width != out_width || output_.height != out_height)) {
    textures_->ReleaseOverrideViews();  // before the texture they name goes
  }
  if (!passes_->EnsureImage(output_, out_width, out_height, kOutputFormat,
                            "temporal upscaler output", kOutputFlags)) {
    FailUpscaler(std::format("its {}x{} output image could not be made", out_width, out_height));
    return false;
  }

  TemporalInputs in;
  in.frame = fr.frame;
  in.render_size[0] = width;
  in.render_size[1] = height;
  in.output_size[0] = out_width;
  in.output_size[1] = out_height;
  in.color = {product->texture.get(), width, height, kOutputFormat, plume::RenderTextureFlag::NONE};
  in.depth = {kept.depth->texture.get(), kept.depth->width, kept.depth->height, kept.depth->format,
              kept.depth->flags};
  in.motion = {kept.motion->texture.get(), kept.motion->width, kept.motion->height,
               kept.motion->format, kept.motion->flags};
  in.output = {output_.texture.get(), out_width, out_height, kOutputFormat, kOutputFlags};
  in.jitter[0] = fr.jitter[0];
  in.jitter[1] = fr.jitter[1];
  in.frame_time_ms = fr.frame_time_ms;
  in.reset = fr.reset != 0 || last_dispatch_frame_ == ~uint64_t(0) ||
             last_dispatch_frame_ + 1 != fr.frame;
  in.camera_near = float(fr.camera.near_plane);
  in.camera_far = float(fr.camera.far_plane);
  in.camera_fov_y = float(fr.camera.fov_y);
  in.sharpness = effective_ == Upscaler::kDlss ? 0.0f : settings_.sharpness;
  {
    // The camera for the backends that read matrices (DLSS): row vectors, no jitter. The frame
    // before's clip space is this frame's own when the history starts again (the same rule as the
    // motion vectors': pending_.previous).
    const temporal::Camera& cam = fr.camera;
    temporal::Mat4 to_prev = temporal::Identity();
    if (pending_.previous) {
      to_prev = temporal::Mul(cam.view_projection_inverse, pending_.previous->view_projection);
    }
    temporal::Mat4 from_prev = temporal::Identity();
    if (!temporal::Inverse(to_prev, &from_prev)) {
      to_prev = from_prev = temporal::Identity();
    }
    temporal::Mat4 clip_to_view = temporal::Identity();
    if (!temporal::Inverse(cam.projection, &clip_to_view)) {
      clip_to_view = temporal::Identity();
    }
    Flatten(cam.projection, in.view_to_clip);
    Flatten(clip_to_view, in.clip_to_view);
    Flatten(to_prev, in.clip_to_prev_clip);
    Flatten(from_prev, in.prev_clip_to_clip);
    for (int i = 0; i < 3; ++i) {
      in.camera_position[i] = float(cam.position[i]);
      in.camera_up[i] = float(cam.up[i]);
      in.camera_right[i] = float(cam.right[i]);
      in.camera_forward[i] = float(cam.forward[i]);
    }
    in.camera_aspect = float(cam.aspect);
  }

  // Outside a render pass (the Scene ended it). The inputs go to the compute stage's read state
  // (D3D12 NON_PIXEL_SHADER_RESOURCE, Vulkan SHADER_READ_ONLY_OPTIMAL), the output to general; the
  // runtime takes them back to those states; they then go back to what the draws and the passes
  // read them in.
  namespace Stage = plume::RenderBarrierStage;
  const plume::RenderBarrierStages graphics_and_compute = Stage::GRAPHICS_AND_COMPUTE;
  pass_timing_->Mark(list, "temporal upscale");
  plume::RenderTextureBarrier to_read[3] = {
      plume::RenderTextureBarrier(product->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::COMPUTE, to_read, 3);
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(output_.texture.get(),
                                                              plume::RenderTextureLayout::GENERAL));
  const bool ok = backend->Upscale(list, in);
  backend::InvalidateAfterExternalWork(list);
  list->barriers(graphics_and_compute,
                 plume::RenderTextureBarrier(product->texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  plume::RenderTextureBarrier back[2] = {
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::GRAPHICS, back, 2);
  list->barriers(graphics_and_compute,
                 plume::RenderTextureBarrier(output_.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  output_.layout = plume::RenderTextureLayout::SHADER_READ;
  if (!ok) {
    // A failure was logged by the backend, which is off for the session; one that was busy (it got
    // occupied after the frame began, TemporalBackend::Busy) only skipped this frame.
    if (!backend->Status().failed) {
      ++window_.upscale_plain_not_ready;
    }
    return false;  // this frame is plain
  }

  // Every fetch of the scene colour from here on reads the output.
  TextureCache::Override stand_in;
  stand_in.product = product;
  stand_in.page = fetch.key.base_page ? fetch.key.base_page : fetch.key.mip_page;
  stand_in.texture = output_.texture.get();
  stand_in.format = kOutputFormat;
  stand_in.scale = fr.output_scale;  // the output's size, however small the scene was drawn
  textures_->SetOverride(stand_in);
  stand_in_set_ = true;
  fr.upscaled = true;
  // What the next frames' post chains are given before their anchors (StandInForPostChain).
  NoteSceneColor(stand_in.product, stand_in.page, fr.frame);
  scene_color_scale_ = stand_in.scale;
  // The anchor's own colour fetch: the binding the stand-in gives this fetch's swizzle (the anchor
  // draws at the output's scale).
  fr.anchor_color_slot = textures_->Bind2D(fetch, fr.output_scale, list).slot;
  last_dispatch_frame_ = fr.frame;
  ++window_.upscaled;
  window_.upscale_resets += in.reset;
  if (switch_log_pending_) {
    switch_log_pending_ = false;
    REXLOG_INFO("{} live change to {}: the new path ran its first upscaled frame (frame {}, {}x{} "
                "to {}x{}) {:.0f} ms after the setting changed", kPrefix,
                PathText(effective_, mode_in_use_), fr.frame, width, height, out_width, out_height,
                MillisSince(switch_asked_));
  }
  return true;
}

void TemporalSystem::ResetRun() {
  ran_last_ = false;
  last_step_known_ = false;
  pending_ready_ = false;
  jitter_index_ = 0;
  misses_ = 0;
  disarmed_ = false;
  frame_open_ = false;
  frame_ = FrameState{};
}

void TemporalSystem::BeginFrame(uint32_t slot, plume::RenderCommandList* list) {
  ReadUpscalerChoice();
  ReadRenderScale();
  if (!frame_open_) {
    // A frame is jittered whole or not at all, and drawn at one scale: the path changes only
    // between frames, here, before the frame's first draw.
    UpdatePath(list);
    PrewarmDlss();
    SyncVectors();
  }
  // The context and the output image of an upscaler that has been off for a while go (10 s); not
  // while another one is being made (the stage).
  if (effective_ != Upscaler::kOff || stage_.active) {
    off_since_ = Clock::time_point{};
  } else if (request_made_ && !frame_open_) {
    const Clock::time_point now = Clock::now();
    if (off_since_ == Clock::time_point{}) {
      off_since_ = now;
    } else if (now - off_since_ > std::chrono::seconds(10)) {
      ReleaseUpscaler();
    }
  }
  if (!enabled_) {
    return;
  }
  if (frame_open_) {
    // A frame the Renderer opened again (an idle present opens a list too, an abandoned frame
    // starts a new one): the state of the frame stands.
    frame_.slot = slot;
    return;
  }
  frame_open_ = true;
  frame_ = FrameState{};
  frame_.frame = frame_number_;
  frame_.slot = slot;
  // The upscaler's context, for the sizes the last accepted frame had (the scene at the render
  // size, the output at the images of the screen's): asked for while the upscaler is chosen and can
  // run; the jitter waits for it, so a frame it cannot take never shakes.
  frame_.upscaler = effective_;
  bool ready = false;
  bool context_ready = false;
  TemporalBackend* backend = BackendFor(effective_);
  if (effective_ != Upscaler::kOff && backend && ran_last_ && last_render_size_[0] != 0 &&
      last_output_size_[0] != 0) {
    BackendRequest request;
    MakeRequest(&request);
    backend->Request(request);
    request_ = request;
    request_made_ = true;
    context_ready = backend->Ready(request);
    // A runtime occupied making another context (DLSS to DLSS) cannot take this frame: plain and not
    // jittered, as before a context is ready.
    const bool busy = context_ready && backend->Busy();
    if (busy != busy_reported_) {
      busy_reported_ = busy;
      if (busy) {
        REXLOG_INFO("{} {}'s runtime is occupied making the next context: frames are plain (not "
                    "jittered) at the old scale until it is done",
                    kPrefix, UpscalerName(effective_));
      } else {
        REXLOG_INFO("{} {}'s runtime is free again: upscaling goes on", kPrefix,
                    UpscalerName(effective_));
      }
    }
    ready = context_ready && !busy;
  }
  frame_.backend_ready = ready;
  if (context_ready != ready_reported_) {
    ready_reported_ = context_ready;
    if (context_ready) {
      REXLOG_INFO("{} {} is ready for {}x{} to {}x{} ({} {}%): the jitter starts and the scene "
                  "colour is replaced by its output",
                  kPrefix, UpscalerName(effective_), request_.render_size[0],
                  request_.render_size[1], request_.output_size[0], request_.output_size[1],
                  ModeInfoOf(request_.upscaler, request_.mode).name,
                  ModeInfoOf(request_.upscaler, request_.mode).percent);
    } else if (effective_ != Upscaler::kOff) {
      REXLOG_INFO("{} the upscaler's context is not ready: frames are plain until it is", kPrefix);
    }
  }
  // Armed only after a frame in which the anchor was accepted: a frame without one never shakes
  // (F4-DESIGN.md 2.5); with an upscaler chosen, only while its context is ready.
  frame_.armed = ran_last_ && !disarmed_ && !VectorsOnly() && (effective_ == Upscaler::kOff || ready);
  if (frame_.armed) {
    // The sequence's length follows the ratio of the last accepted frame (8 at 1, 18 at 1.5, 32 at
    // 2, 72 at 3): the upscaler's own count for its context when it has one.
    frame_.ratio = (last_render_size_[0] != 0 && last_output_size_[0] != 0)
                       ? double(last_output_size_[0]) / double(last_render_size_[0])
                       : 1.0;
    frame_.phase_count = JitterPhases(frame_.ratio, ready && backend ? backend->JitterPhaseCount() : 0u);
    frame_.jitter_index = jitter_index_ % frame_.phase_count;
    // Halton(2,3) from index 1, minus a half: in (-0.5, 0.5) render pixels, +y down.
    frame_.jitter[0] = float(temporal::Halton(frame_.jitter_index + 1, 2) - 0.5);
    frame_.jitter[1] = float(temporal::Halton(frame_.jitter_index + 1, 3) - 0.5);
  }
}

void TemporalSystem::OnComposite() {
  if (enabled_ && frame_open_) {
    frame_.composite_seen = true;
  }
}

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

bool TemporalSystem::OnDraw(const DrawFacts& facts, SharedConstants& shared) {
  if (!frame_open_) {
    return false;
  }
  const DrawRecord& r = facts.view->record();
  if (facts.ps && r.ps_identity == temporal::kAnchorPs && r.vs_identity == temporal::kAnchorVs) {
    return OnAnchor(facts, shared);
  }
  const temporal::MainMatch match = temporal::MatchMainView(facts.color0, facts.depth);
  if (match) {
    OnMainView(facts, match, shared);
  }
  return false;
}

uint32_t TemporalSystem::EvaluateGate(const XenosState& s, const DrawRecord& r,
                                      const ViewportInfo& vp, Scale scale) {
  uint32_t failed = 0;
  if (s.clip.clip_disable != 0) {
    failed |= kGateClipOff;
  }
  if (!(s.vte.vport_x_scale_ena && s.vte.vport_x_offset_ena && s.vte.vport_y_scale_ena &&
        s.vte.vport_y_offset_ena && s.vte.vport_z_scale_ena && s.vte.vport_z_offset_ena)) {
    failed |= kGateNoViewport;
  }
  if (r.primitive == uint32_t(xe::PrimitiveType::kRectangleList)) {
    failed |= kGateRectList;
  }
  // The target is the screen's pitch wide (the match says so); a world draw covers all of it.
  if (vp.viewport.width + 0.5f < float(scale.Edge(temporal::kMainPitch)) ||
      !(vp.viewport.height >= 1.0f)) {
    failed |= kGateNarrow;
  }
  return failed;
}

void TemporalSystem::OnMainView(const DrawFacts& f, const temporal::MainMatch& match,
                                SharedConstants& shared) {
  FrameState& fr = frame_;
  if (!temporal::IsPlaySamples(match.samples)) {
    ++fr.view4x_draws;  // the pause menu, the map: known, never moved
    return;
  }
  if (fr.anchor_seen || fr.composite_seen) {
    // The UI phase draws into a depth key that equals the main view's when the game runs without
    // MSAA (one depth-only draw a frame, census round 2); the anchor and the marker are what
    // separate them (census 3.4).
    return;
  }
  if (fr.samples == 0) {
    fr.samples = match.samples;
  }
  // CHECK: without MSAA the UI's depth-only draw, in the first frame with no 3D scene after one with
  // it (the frame is armed, there is no anchor), is moved by the jitter like a main view draw: a
  // sub-pixel shift of a stencil mask for one frame, counted as a miss only when colour was drawn.
  // The colour-only draws (no depth test and no stencil: the sky and effects drawn without depth)
  // and the depth-only draws (the occlusion draws at dusk and night, 256 a frame) are the same
  // world as the rest, so they are moved when they pass the gate below: a world draw left still
  // would sit at a different place than everything the temporal pass will treat as jittered.
  // CHECK: that no colour-only draw is a screen-space effect with clipping on (the census could
  // not tell them apart from the world; the counts per kind are in the summary), and that the 256
  // depth-only draws are the occlusion queries (census finding 12, an inference).
  const Kind kind = match.color ? (match.depth ? kColorDepth : kColorOnly) : kDepthOnly;
  if (fr.scale.none()) {
    fr.scale = f.scale;
  }
  ++fr.main_draws[kind];
  if (kind != kDepthOnly) {
    // The first draw that draws colour into the main view: this frame has a 3D scene, and the post
    // chain that reads its colour comes later.
    if (!fr.post_checked) {
      fr.post_checked = true;
      StandInForPostChain();
    }
  }

  if (EvaluateGate(*f.state, f.view->record(), *f.viewport, f.scale) != 0) {
    return;  // not a world draw: kept still
  }
  if (!fr.armed) {
    return;
  }
  // Every gated draw of the frame is moved by the same offset, so the depth-only draws, the world
  // and the colour-only draws agree. The viewport is in host pixels; NDC spans 2 of them, +y up
  // (ComputeViewport negated y), so a shift down by jy pixels is -2 jy / height.
  shared.pos_offset[0] += 2.0f * fr.jitter[0] / f.viewport->viewport.width;
  shared.pos_offset[1] -= 2.0f * fr.jitter[1] / f.viewport->viewport.height;
  ++fr.jittered[kind];
}

TemporalSystem::AnchorStatus TemporalSystem::CheckAnchor(const DrawFacts& f, uint32_t* width,
                                                         uint32_t* height,
                                                         std::string* why) const {
  const FrameState& fr = frame_;
  if (fr.view4x_draws != 0) {
    return AnchorStatus::kView4x;
  }
  if (Colored(fr.main_draws) == 0) {
    return AnchorStatus::kNoMainView;
  }
  // Fetches 0, 1, 2: 2D textures of one size, the colour, the stencil view and the depth.
  const uint32_t mask = f.ps->texture_fetch_mask;
  if ((mask & 7u) != 7u) {
    *why = std::format("the pixel shader's fetch mask is {:#x}", mask);
    return AnchorStatus::kFetches;
  }
  uint32_t sizes[3][2] = {};
  for (uint32_t slot = 0; slot < 3; ++slot) {
    uint32_t dwords[6];
    f.view->FetchGroup(slot, dwords);
    xe::xe_gpu_texture_fetch_t t;
    std::memcpy(&t, dwords, sizeof(t));
    sizes[slot][0] = uint32_t(t.size_2d.width) + 1;
    sizes[slot][1] = uint32_t(t.size_2d.height) + 1;
    if (t.type != xe::FetchConstantType::kTexture ||
        uint32_t(t.dimension) != uint32_t(xe::DataDimension::k2DOrStacked) ||
        uint32_t(t.format) != kAnchorFormats[slot]) {
      *why = std::format("fetch {} should be a 2D texture of format {}: it is type {}, "
                         "dimension {}, format {}",
                         slot, kAnchorFormatNames[slot], uint32_t(t.type), uint32_t(t.dimension),
                         uint32_t(t.format));
      return AnchorStatus::kFetches;
    }
  }
  if (sizes[1][0] != sizes[0][0] || sizes[2][0] != sizes[0][0] || sizes[1][1] != sizes[0][1] ||
      sizes[2][1] != sizes[0][1]) {
    *why = std::format("the fetches' sizes differ: {}x{}, {}x{}, {}x{}", sizes[0][0], sizes[0][1],
                       sizes[1][0], sizes[1][1], sizes[2][0], sizes[2][1]);
    return AnchorStatus::kFetches;
  }
  *width = sizes[0][0];
  *height = sizes[0][1];
  // Its target: the half-size float image. The target is allocated a little taller than the
  // picture (640x384 for 640x360, census 1.1), so the viewport, not the allocation, is compared.
  const uint32_t half_w = f.scale.Edge(*width / 2);
  const uint32_t half_h = f.scale.Edge(*height / 2);
  if (!f.color0 ||
      f.color0->format != uint8_t(xe::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) ||
      std::fabs(f.viewport->viewport.width - float(half_w)) >= 0.5f ||
      std::fabs(f.viewport->viewport.height - float(half_h)) >= 0.5f) {
    *why = std::format("its viewport is {:.0f}x{:.0f}, half the frame is {}x{} at scale {}, "
                       "colour format {}",
                       f.viewport->viewport.width, f.viewport->viewport.height, half_w, half_h,
                       f.scale.Text(), f.color0 ? int(f.color0->format) : -1);
    return AnchorStatus::kTarget;
  }
  return AnchorStatus::kAccepted;
}

bool TemporalSystem::OnAnchor(const DrawFacts& f, const SharedConstants& shared) {
  FrameState& fr = frame_;
  fr.anchor_seen = true;  // the main view's draws are done: no more jitter
  if (fr.anchors++ != 0) {
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  std::string why;
  AnchorStatus status = CheckAnchor(f, &width, &height, &why);
  temporal::Camera camera;
  if (status == AnchorStatus::kAccepted) {
    const temporal::Reject reject = temporal::ExtractCamera(
        f.banks->ps.data(), width, height, fr.frame, &camera);
    if (reject != temporal::Reject::kNone) {
      status = StatusOf(reject);
      why = temporal::RejectName(reject);
    }
  }
  fr.anchor_status = status;
  if (status != AnchorStatus::kAccepted) {
    if (status != AnchorStatus::kView4x && !status_logged_[size_t(status)]) {
      status_logged_[size_t(status)] = true;
      REXLOG_INFO("{} the anchor draw of frame {} is not taken: {}{}{} (the first of its kind; "
                  "the summaries count them)",
                  kPrefix, fr.frame, StatusName(status), why.empty() ? "" : ": ", why);
    }
    DropPostStandIn();  // the post chain was given the previous picture: the rest reads this frame's
    return false;
  }

  // Accepted. What the frame's camera is, and whether its history starts again.
  fr.anchor_ok = true;
  fr.camera = camera;
  fr.guest_size[0] = camera.width;
  fr.guest_size[1] = camera.height;
  fr.render_size[0] = fr.scale.Edge(camera.width);
  fr.render_size[1] = fr.scale.Edge(camera.height);
  // The anchor draws into an image of the screen (checked: its viewport is half the frame at its
  // scale), so its scale is the output's, however much smaller the main view was drawn.
  fr.output_scale = f.scale;
  fr.output_size[0] = f.scale.Edge(camera.width);
  fr.output_size[1] = f.scale.Edge(camera.height);
  fr.ratio = fr.render_size[0] != 0 ? double(fr.output_size[0]) / double(fr.render_size[0]) : 1.0;
  for (uint32_t i = 0; i < 3; ++i) {
    f.view->FetchGroup(i, fr.anchor_textures.fetch[i]);
    fr.anchor_textures.table[i] = shared.texture_2d[i];
  }
  fr.anchor_textures.bound = true;

  const Clock::time_point now = Clock::now();
  uint32_t reset = 0;
  temporal::Cut cut;
  if (!ran_last_) {
    reset |= kResetFirst;
  } else {
    const double gap_ms = std::chrono::duration<double, std::milli>(now - last_anchor_time_).count();
    fr.frame_time_ms = float(std::clamp(gap_ms, 1.0, kMaxGapMs));
    if (gap_ms > kMaxGapMs) {
      reset |= kResetTime;
    }
    if (last_camera_.width != camera.width || last_camera_.height != camera.height ||
        last_scale_ != fr.scale || last_output_size_[0] != fr.output_size[0] ||
        last_output_size_[1] != fr.output_size[1]) {
      reset |= kResetSize;
    }
    cut = temporal::TestCut(camera, last_camera_, last_step_known_ ? last_step_ : nullptr);
    fr.cut = cut;
    if (cut.fires) {
      reset |= kResetCut;
    }
  }
  if (!camera.previous_valid) {
    reset |= kResetNoPrevious;
  }
  last_anchor_time_ = now;
  fr.reset = reset;

  // The lens (field of view, near, far): a line when it changes, not only in the summaries (the
  // census logged it in its first two frames only and saw 25 degrees in play, 53.75 in the pause).
  const auto changed = [](double a, double b) { return std::fabs(a - b) > 0.01 * std::fabs(b); };
  if (lens_lines_ < kMaxLensLines &&
      (changed(camera.fov_y, logged_lens_[0]) || changed(camera.near_plane, logged_lens_[1]) ||
       changed(camera.far_plane, logged_lens_[2]))) {
    ++lens_lines_;
    logged_lens_[0] = camera.fov_y;
    logged_lens_[1] = camera.near_plane;
    logged_lens_[2] = camera.far_plane;
    REXLOG_INFO("{} lens in frame {}: field of view {:.3f} deg, aspect {:.4f}, near {:.3f}, far "
                "{:.1f}, camera at {:.2f}, {:.2f}, {:.2f}, looking along {:.3f}, {:.3f}, {:.3f}{}",
                kPrefix, fr.frame, camera.fov_y * 180.0 / kPi, camera.aspect, camera.near_plane,
                camera.far_plane, camera.position[0], camera.position[1], camera.position[2],
                camera.forward[0], camera.forward[1], camera.forward[2],
                lens_lines_ == kMaxLensLines ? " (the last of these lines)" : "");
  }

  if (reset != 0 && reset_lines_ < kMaxResetLines) {
    ++reset_lines_;
    REXLOG_INFO("{} history starts again in frame {}: {}{} (camera at {:.1f}, {:.1f}, {:.1f}, field "
                "of view {:.2f} deg){}",
                kPrefix, fr.frame, ResetText(reset),
                (reset & kResetCut)
                    ? std::format(" ({}{}{}: moved {:.1f}, {:.1f} away from the {:.1f} the step "
                                  "before predicts (allowed {:.1f}); turn cosine {:.4f}; field of "
                                  "view changed {:.1f}%)",
                                  cut.by_distance ? "distance" : "",
                                  cut.by_turn ? (cut.by_distance ? " + turn" : "turn") : "",
                                  cut.by_fov ? (cut.by_distance || cut.by_turn ? " + field of view"
                                                                               : "field of view")
                                             : "",
                                  cut.distance, cut.deviation,
                                  last_step_known_ ? std::sqrt(last_step_[0] * last_step_[0] +
                                                               last_step_[1] * last_step_[1] +
                                                               last_step_[2] * last_step_[2])
                                                   : 0.0,
                                  cut.tolerance, cut.cosine, 100.0 * cut.fov_change)
                    : std::string(),
                camera.position[0], camera.position[1], camera.position[2],
                camera.fov_y * 180.0 / kPi,
                reset_lines_ == kMaxResetLines ? " (the last of these lines; the summaries count)"
                                               : "");
  }

  // What the passes of step 1b need, for the Scene's call of RecordAnchorPasses right after this
  // draw's constants are known: the anchor's own depth fetch, the sizes, the jitter and the
  // cameras. The vectors use the frame before's camera only when it follows this one without a
  // restart (a cut, a size change or a gap leave them zero).
  pending_ = TemporalMotion::Frame{};
  pending_.frame = fr.frame;
  pending_.depth_slot = fr.anchor_textures.table[2];
  for (int i = 0; i < 2; ++i) {
    pending_.render_size[i] = fr.render_size[i];
    pending_.guest_size[i] = fr.guest_size[i];
    pending_.jitter[i] = fr.jitter[i];
  }
  pending_.camera = &fr.camera;
  pending_.previous =
      (ran_last_ && (reset & ~uint32_t(kResetNoPrevious)) == 0) ? &last_camera_ : nullptr;
  pending_ready_ = motion_.available();
  if (!pending_ready_) {
    DropPostStandIn();  // no passes: RecordAnchorPasses is not called
  }
  return pending_ready_;
}

// ---------------------------------------------------------------------------
// the end of the frame, the state machine and the log
// ---------------------------------------------------------------------------

TemporalSystem::Mode TemporalSystem::ModeOf(const FrameState& fr) {
  if (Colored(fr.main_draws) == 0 && fr.view4x_draws == 0) {
    return Mode::kNoScene;
  }
  if (fr.anchor_ok) {
    return Mode::kRunning;
  }
  if (fr.view4x_draws != 0) {
    return Mode::kView4x;
  }
  return Mode::kNoAnchor;
}

void TemporalSystem::EndFrame() {
  // The scene colour product is the product again for the next frame's first draws.
  const bool upscaled = frame_.upscaled;
  const Upscaler running = frame_.upscaler;
  CountPostStandIn();  // a frame whose anchor draw never came
  ClearStandIn();
  PublishRunningUpscaler(upscaled ? running : Upscaler::kOff);
  if (!enabled_) {
    return;
  }
  if (!frame_open_) {
    // A present with no scene record before it (a loading screen, a movie): no 3D scene.
    frame_ = FrameState{};
    frame_.frame = frame_number_;
  }
  const FrameState& fr = frame_;
  const bool ran = fr.anchor_ok;

  // The safety net: a frame that was jittered, had the main view and no accepted anchor.
  if (fr.armed && Colored(fr.jittered) != 0) {
    if (ran) {
      misses_ = 0;
    } else {
      if (++misses_ >= kDisarmAfterMisses && !disarmed_) {
        disarmed_ = true;
        REXLOG_WARN("{} {} frames in a row were jittered and had no usable anchor ({}): the "
                    "jitter is off until the upscaler is turned off and on",
                    kPrefix, misses_, StatusName(fr.anchor_status));
      }
    }
  }

  // The upscaler: a frame that was jittered and not upscaled is a wrong frame (the raw jittered
  // scene); a few in a row and it is over for the session. Frames that were not jittered, or whose
  // 3D scene was not the main view (the pause menu's 4x), are plain and right.
  if (fr.upscaler != Upscaler::kOff && fr.armed && Colored(fr.jittered) != 0 && !upscaled) {
    if (++upscale_misses_ >= kDisarmAfterMisses) {
      upscale_misses_ = 0;
      FailUpscaler(std::format("{} jittered frames in a row could not be upscaled ({})",
                               kDisarmAfterMisses, StatusName(fr.anchor_status)));
    }
  } else {
    upscale_misses_ = 0;
  }

  // The run for the next frame. The camera's step first: it needs the camera of the frame before.
  if (ran && ran_last_ && !(fr.reset & kResetTime)) {
    for (int i = 0; i < 3; ++i) {
      last_step_[i] = fr.camera.position[i] - last_camera_.position[i];
    }
    last_step_known_ = true;
  } else {
    last_step_known_ = false;
  }
  ran_last_ = ran;
  if (ran) {
    last_camera_ = fr.camera;
    last_scale_ = fr.scale;
    last_render_size_[0] = fr.render_size[0];
    last_render_size_[1] = fr.render_size[1];
    last_output_size_[0] = fr.output_size[0];
    last_output_size_[1] = fr.output_size[1];
    ++jitter_index_;  // advances in the frames the pass ran in only
  }

  // The window.
  Window& w = window_;
  ++w.frames;
  w.armed += fr.armed;
  const Mode mode = ModeOf(fr);
  ++w.modes[size_t(mode)];
  ++w.anchor_status[size_t(fr.anchor_status)];
  if (ran) {
    if (fr.reset != 0) {
      ++w.resets;
      for (uint32_t i = 0; i < kResetReasons; ++i) {
        w.reset_reasons[i] += (fr.reset >> i) & 1u;
      }
      if (fr.reset & kResetCut) {
        w.cut_distance += fr.cut.by_distance;
        w.cut_turn += fr.cut.by_turn;
        w.cut_fov += fr.cut.by_fov;
      }
    }
    w.have_camera = true;
    w.camera = fr.camera;
    w.render_size[0] = fr.render_size[0];
    w.render_size[1] = fr.render_size[1];
  }

  NoteMode(mode, fr);
  ++frame_number_;
  frame_open_ = false;
}

void TemporalSystem::NoteMode(Mode mode, const FrameState& fr) {
  if (shown_mode_ != Mode::kCount && mode == shown_mode_) {
    pending_mode_ = mode;
    pending_frames_ = 0;
    return;
  }
  if (shown_mode_ != Mode::kCount) {
    // A change counts once it has lasted a few frames (a pause menu opens through single frames).
    pending_frames_ = mode == pending_mode_ ? pending_frames_ + 1 : 1;
    pending_mode_ = mode;
    if (pending_frames_ < kModeDebounce) {
      return;
    }
  }
  shown_mode_ = mode;
  pending_mode_ = mode;
  pending_frames_ = 0;
  switch (mode) {
    case Mode::kNoScene:
      REXLOG_INFO("{} frame {}: no 3D scene (a menu, loading, a movie): nothing is moved, no camera",
                  kPrefix, fr.frame);
      break;
    case Mode::kView4x:
      REXLOG_INFO("{} frame {}: the 3D scene is at 4x samples (the pause menu, the map): nothing is "
                  "moved, no camera", kPrefix, fr.frame);
      break;
    case Mode::kNoAnchor:
      REXLOG_INFO("{} frame {}: the 3D scene is there but its anchor draw is not taken ({}): the "
                  "frames are not temporal",
                  kPrefix, fr.frame, StatusName(fr.anchor_status));
      break;
    case Mode::kRunning:
      REXLOG_INFO("{} frame {}: running: the 3D scene at {}x samples, the anchor taken (camera at "
                  "{:.1f}, {:.1f}, {:.1f}, field of view {:.2f} deg, near {:.2f}, far {:.0f}, "
                  "frame {}x{}, render {}x{}), jitter {}",
                  kPrefix, fr.frame, fr.samples, fr.camera.position[0], fr.camera.position[1],
                  fr.camera.position[2], fr.camera.fov_y * 180.0 / kPi,
                  fr.camera.near_plane, fr.camera.far_plane, fr.guest_size[0], fr.guest_size[1],
                  fr.render_size[0], fr.render_size[1],
                  fr.armed ? std::format("{:+.3f}, {:+.3f} px (phase {} of {})", fr.jitter[0],
                                         fr.jitter[1], fr.jitter_index, fr.phase_count)
                           : std::string("starts with the next frame"));
      break;
    case Mode::kCount:
      break;
  }
}

void TemporalSystem::Report() {
  const Window& w = window_;
  const double seconds = std::chrono::duration<double>(Clock::now() - window_start_).count();
  const uint64_t running = w.modes[size_t(Mode::kRunning)];
  const uint64_t scene = running + w.modes[size_t(Mode::kNoAnchor)];

  std::string refused;
  for (size_t i = size_t(AnchorStatus::kNoMainView); i < size_t(AnchorStatus::kCount); ++i) {
    if (w.anchor_status[i] != 0 && i != size_t(AnchorStatus::kView4x)) {
      refused += std::format("{}{} x{}", refused.empty() ? "" : "; ", StatusName(AnchorStatus(i)),
                             w.anchor_status[i]);
    }
  }
  std::string resets;
  for (uint32_t i = 0; i < kResetReasons; ++i) {
    if (w.reset_reasons[i] != 0) {
      resets += std::format("{}{} {}", resets.empty() ? "" : ", ", ResetName(i), w.reset_reasons[i]);
      if (i == 1) {
        resets += std::format(" (by jump {}, turn {}, field of view {})", w.cut_distance, w.cut_turn,
                              w.cut_fov);
      }
    }
  }
  std::string camera = "none";
  if (w.have_camera) {
    camera = std::format("({:.1f}, {:.1f}, {:.1f}), field of view {:.2f} deg, near {:.2f}, far "
                         "{:.0f}, frame {}x{}, render {}x{}",
                         w.camera.position[0], w.camera.position[1], w.camera.position[2],
                         w.camera.fov_y * 180.0 / kPi, w.camera.near_plane, w.camera.far_plane,
                         w.camera.width, w.camera.height, w.render_size[0], w.render_size[1]);
  }
  REXLOG_INFO("{} summary: {} frames in {:.1f} s: 3D scene {} (anchor taken {}, at 4x samples {}), "
              "no scene {}, jitter armed {} | refused anchors: {} | history restarts: {} ({}) | "
              "camera {}",
              kPrefix, w.frames, seconds, scene, running, w.modes[size_t(Mode::kView4x)],
              w.modes[size_t(Mode::kNoScene)], w.armed, refused.empty() ? "none" : refused,
              w.resets, resets.empty() ? "none" : resets, camera);
  if (effective_ != Upscaler::kOff || w.upscaled != 0) {
    const TemporalBackend* running_backend = BackendFor(effective_);
    const BackendStatus status = running_backend ? running_backend->Status() : BackendStatus{};
    REXLOG_INFO("[gpu] temporal upscaler: {} ({}{}) in {} frames: upscaled {} ({} of them with "
                "the history started again); the game's post chain before the anchor was offered "
                "the previous frame's picture in {} frames and read it in {} of them ({} reads "
                "served by the stand-in before the anchor; about one a frame when it works, none "
                "when the reads name a product it does not cover), plain: context not ready {}, "
                "no vectors {}, colour not an input {}{}",
                UpscalerName(effective_), ModeInfoOf(effective_, mode_in_use_).name,
                status.provider.empty() ? std::string() : ", provider " + status.provider, w.frames,
                w.upscaled, w.upscale_resets, w.post_stand_in_set, w.post_on_previous,
                w.post_reads_served, w.upscale_plain_not_ready, w.upscale_plain_no_vectors,
                w.upscale_plain_no_color,
                status.failed ? std::format(" | FAILED: {}", status.reason) : std::string());
  }
}

void TemporalSystem::Finish() {
  if (enabled_ && window_.frames != 0) {
    Report();
  }
}

}  // namespace mcla::gpu
