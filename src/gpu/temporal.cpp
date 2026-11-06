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
#include "resolve.h"
#include "textures.h"
#include "topology.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_temporal, false, "MCLA/GPU",
    "Development switch for the groundwork of the temporal upscaler (the player's setting is "
    "mcla_gpu_upscaler): moves the 3D scene by a different sub-pixel amount every frame (the world "
    "only: the menus and the HUD keep their positions), reads the game's camera, and makes the "
    "motion vectors. With no upscaler running, nothing smooths the movement out, so the scene "
    "shimmers a little. Logs '[gpu] temporal:' lines. Read every frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(uint32_t, mcla_gpu_temporal_debug);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal:";
constexpr uint32_t kMaxResetLines = 16;
constexpr uint32_t kMaxStillLines = 12;
constexpr uint32_t kMaxLensLines = 24;
constexpr double kPi = 3.14159265358979323846;

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

void TemporalSystem::SetEnabled(bool enabled, bool development) {
  enabled_ = enabled;
  ResetRun();
  window_ = Window{};
  window_start_ = Clock::now();
  shown_mode_ = Mode::kCount;
  pending_mode_ = Mode::kCount;
  pending_frames_ = 0;
  status_logged_.fill(false);
  reset_lines_ = 0;
  rigid_lines_ = 0;
  still_logged_.clear();
  logged_lens_[0] = logged_lens_[1] = logged_lens_[2] = 0.0;
  lens_lines_ = 0;
  frame_number_ = 0;
  upscale_misses_ = 0;
  last_dispatch_frame_ = ~uint64_t(0);
  plain_logged_.fill(false);
  if (enabled) {
    if (development) {
      REXLOG_WARN("{} ON (mcla_gpu_temporal, development): the 3D scene is moved by a sub-pixel "
                  "amount that changes every frame (the world only: the UI keeps still), and the "
                  "game's camera is read from its motion blur draw; with no upscaler running "
                  "nothing smooths the movement out. A summary every {:.0f} s",
                  kPrefix, kReportSeconds);
    } else {
      REXLOG_INFO("{} ON for {}: the camera is read from the game's motion blur draw and the 3D "
                  "scene is moved by a sub-pixel amount every frame once the upscaler is ready. A "
                  "summary every {:.0f} s",
                  kPrefix, UpscalerName(effective_), kReportSeconds);
    }
  } else {
    ClearStandIn();
    motion_.Release();
    REXLOG_INFO("{} off", kPrefix);
  }
}

void TemporalSystem::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                HostPasses& passes, PassTiming& pass_timing,
                                plume::RenderFormat target_format, TextureCache& textures,
                                ResolveCache& resolve) {
  motion_.Initialize(device, frames, heap, passes, pass_timing, target_format);
  device_ = &device;
  frames_ = &frames;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  textures_ = &textures;
  resolve_ = &resolve;
  // The upscaler runtime, if this build has one: a worker thread loads it and asks it what this PC
  // can do; the menu lists what it finds. Nothing here waits for it.
  backend_ = MakeFfxBackend();
  if (backend_) {
    backend_->Probe(device, frames);
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
  if (backend_) {
    backend_->Shutdown();
    backend_.reset();
  }
  PublishRunningUpscaler(Upscaler::kOff);
  motion_.Shutdown();
  pending_ready_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  textures_ = nullptr;
  resolve_ = nullptr;
}

bool TemporalSystem::RecordAnchorPasses(plume::RenderCommandList* list) {
  if (!pending_ready_) {
    return false;
  }
  pending_ready_ = false;
  if (!motion_.Record(list, pending_)) {
    ++window_.mv_missed;
    if (frame_.upscaler != Upscaler::kOff && frame_.backend_ready) {
      // The upscaler cannot be fed this session: the passes themselves could not be made.
      FailUpscaler("the depth and motion vector passes could not be recorded");
    }
    return false;
  }
  RecordUpscale(list);
  return true;
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

void TemporalSystem::ReadUpscalerChoice() {
  settings_ = ReadUpscalerSettings();
  const UpscalerAvailability available = GetUpscalerAvailability();
  Upscaler effective = Upscaler::kOff;
  if (backend_ && ((settings_.upscaler == Upscaler::kFsr31 && available.fsr31) ||
                   (settings_.upscaler == Upscaler::kFsr4 && available.fsr4))) {
    effective = settings_.upscaler;
  }
  // A line when what the choice comes to changes. A choice this PC cannot run is off (the saved
  // choice stays as it is, and comes back where it can run).
  const uint32_t key = uint32_t(settings_.upscaler) | uint32_t(effective) << 2 |
                       uint32_t(available.probed) << 4 | uint32_t(backend_ != nullptr) << 5;
  if (!choice_reported_ || key != choice_key_) {
    choice_reported_ = true;
    choice_key_ = key;
    if (settings_.upscaler != Upscaler::kOff) {
      if (effective != Upscaler::kOff) {
        REXLOG_INFO("{} upscaling: {} ({} {}%), sharpness {:.2f}", kPrefix, UpscalerName(effective),
                    ModeInfoOf(settings_.mode).name, ModeInfoOf(settings_.mode).percent,
                    settings_.sharpness);
      } else if (!backend_) {
        REXLOG_INFO("{} {} is chosen but this build has no FidelityFX: treated as off", kPrefix,
                    UpscalerName(settings_.upscaler));
      } else if (available.probed) {
        REXLOG_INFO("{} {} is chosen but this PC cannot run it: treated as off (the choice is "
                    "kept)",
                    kPrefix, UpscalerName(settings_.upscaler));
      }
    }
  }
  effective_ = effective;
  NoteMsaa();
}

void TemporalSystem::NoteMsaa() {
  // What the upscaler comes to for the game's MSAA (gpu.h GuestMsaa). The game's start decided it
  // and logged it ("[gpu] msaa:"); what is said here is when the upscaler's state has since
  // changed: the cases where the game's MSAA and the upscaler do not agree until the next start.
  const GuestMsaa msaa = GetGuestMsaa();
  const bool upscaling = effective_ != Upscaler::kOff;
  const uint32_t key = uint32_t(msaa) | uint32_t(upscaling) << 4;
  if (key == msaa_key_) {
    return;
  }
  msaa_key_ = key;
  if (upscaling && msaa == GuestMsaa::kOn) {
    REXLOG_INFO("{} the game's MSAA is running under {}: the game was started with MSAA, before "
                "the upscaler was on. It works with it; a start with the upscaler on runs without "
                "MSAA (mcla_gpu_msaa is kept)",
                kPrefix, UpscalerName(effective_));
  } else if (!upscaling && msaa == GuestMsaa::kOffForUpscaler) {
    REXLOG_INFO("{} the upscaler is off, but the game was started without MSAA for it: no MSAA "
                "until the next start (mcla_gpu_msaa applies then)",
                kPrefix);
  }
}

void TemporalSystem::MakeRequest(BackendRequest* request) const {
  request->upscaler = effective_;
  request->mode = settings_.mode;
  // Ratio 1: the scene is drawn at the size it is shown at.
  request->render_size[0] = request->output_size[0] = last_render_size_[0];
  request->render_size[1] = request->output_size[1] = last_render_size_[1];
}

void TemporalSystem::ClearStandIn() {
  if (stand_in_set_ && textures_) {
    textures_->ClearOverride();
  }
  stand_in_set_ = false;
  frame_.upscaled = false;
}

void TemporalSystem::ReleaseUpscaler() {
  // No upscaler for a while: the context and the output image go (the GPU may still be reading
  // them: the backend retires the context behind the frames, the image goes through the ring).
  if (backend_) {
    backend_->Release();
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
  if (backend_) {
    backend_->Fail(why);
  }
}

bool TemporalSystem::RecordUpscale(plume::RenderCommandList* list) {
  FrameState& fr = frame_;
  if (effective_ == Upscaler::kOff || !backend_) {
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
  if (!fr.backend_ready || !backend_->Ready(request_)) {
    return plain(window_.upscale_plain_not_ready, 0, "the upscaler's context is not ready");
  }
  const uint32_t width = fr.render_size[0];
  const uint32_t height = fr.render_size[1];
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
  if (output_.texture && (output_.width != width || output_.height != height)) {
    textures_->ReleaseOverrideViews();  // before the texture they name goes
  }
  if (!passes_->EnsureImage(output_, width, height, kOutputFormat, "temporal upscaler output",
                            kOutputFlags)) {
    FailUpscaler(std::format("its {}x{} output image could not be made", width, height));
    return false;
  }

  TemporalInputs in;
  in.frame = fr.frame;
  in.render_size[0] = in.output_size[0] = width;
  in.render_size[1] = in.output_size[1] = height;
  in.color = {product->texture.get(), width, height, kOutputFormat, plume::RenderTextureFlag::NONE};
  in.depth = {kept.depth->texture.get(), kept.depth->width, kept.depth->height, kept.depth->format,
              kept.depth->flags};
  in.motion = {kept.motion->texture.get(), kept.motion->width, kept.motion->height,
               kept.motion->format, kept.motion->flags};
  in.output = {output_.texture.get(), width, height, kOutputFormat, kOutputFlags};
  in.jitter[0] = fr.jitter[0];
  in.jitter[1] = fr.jitter[1];
  // Development: the upscaler is told the jitter flipped (the convention check); the scene was
  // moved as it was.
  if (debug_bits_ & 4u) {
    in.jitter[0] = -in.jitter[0];
  }
  if (debug_bits_ & 8u) {
    in.jitter[1] = -in.jitter[1];
  }
  in.frame_time_ms = fr.frame_time_ms;
  in.reset = fr.reset != 0 || last_dispatch_frame_ == ~uint64_t(0) ||
             last_dispatch_frame_ + 1 != fr.frame;
  in.camera_near = float(fr.camera.near_plane);
  in.camera_far = float(fr.camera.far_plane);
  in.camera_fov_y = float(fr.camera.fov_y);
  in.sharpness = settings_.sharpness;

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
  const bool ok = backend_->Upscale(list, in);
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
    return false;  // the backend logged it and is off for the session; this frame is plain
  }

  // Every fetch of the scene colour from here on reads the output.
  TextureCache::Override stand_in;
  stand_in.product = product;
  stand_in.page = fetch.key.base_page ? fetch.key.base_page : fetch.key.mip_page;
  stand_in.texture = output_.texture.get();
  stand_in.format = kOutputFormat;
  stand_in.scale = fr.scale;
  textures_->SetOverride(stand_in);
  stand_in_set_ = true;
  fr.upscaled = true;
  // The anchor's own colour fetch: the binding the stand-in gives this fetch's swizzle.
  fr.anchor_color_slot = textures_->Bind2D(fetch, fr.scale, list).slot;
  last_dispatch_frame_ = fr.frame;
  ++window_.upscaled;
  window_.upscale_resets += in.reset;
  return true;
}

void TemporalSystem::DrawDebug(plume::RenderCommandList* list, uint32_t width, uint32_t height) {
  // The last frame had an accepted anchor, so its vectors were made (the view is a development tool).
  if (enabled_ && frame_.anchor_ok) {
    motion_.DrawDebug(list, width, height);
  }
}

void TemporalSystem::ResetRun() {
  ran_last_ = false;
  last_step_known_ = false;
  last_jitter_[0] = last_jitter_[1] = 0.0f;
  pending_ready_ = false;
  jitter_index_ = 0;
  misses_ = 0;
  disarmed_ = false;
  frame_open_ = false;
  frame_ = FrameState{};
}

void TemporalSystem::BeginFrame(uint32_t slot) {
  development_ = REXCVAR_GET(mcla_gpu_temporal);
  debug_bits_ = REXCVAR_GET(mcla_gpu_temporal_debug);
  ReadUpscalerChoice();
  const bool want = development_ || effective_ != Upscaler::kOff;
  if (want != enabled_ && !frame_open_) {
    SetEnabled(want, development_);  // only between frames: a frame is jittered whole or not at all
  }
  // The context and the output image of an upscaler that has been off for a while go (10 s).
  if (effective_ != Upscaler::kOff) {
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
  // The upscaler's context, for the size the last accepted frame had (ratio 1): asked for while the
  // upscaler is chosen and can run; the jitter waits for it, so a frame it cannot take never shakes.
  frame_.upscaler = effective_;
  bool ready = false;
  if (effective_ != Upscaler::kOff && backend_ && ran_last_ && last_render_size_[0] != 0) {
    BackendRequest request;
    MakeRequest(&request);
    backend_->Request(request);
    request_ = request;
    request_made_ = true;
    ready = backend_->Ready(request);
  }
  frame_.backend_ready = ready;
  if (ready != ready_reported_) {
    ready_reported_ = ready;
    if (ready) {
      REXLOG_INFO("{} {} is ready for {}x{} ({} {}%): the jitter starts and the scene colour is "
                  "replaced by its output",
                  kPrefix, UpscalerName(effective_), request_.render_size[0],
                  request_.render_size[1], ModeInfoOf(request_.mode).name,
                  ModeInfoOf(request_.mode).percent);
    } else if (effective_ != Upscaler::kOff) {
      REXLOG_INFO("{} the upscaler's context is not ready: frames are plain until it is", kPrefix);
    }
  }
  // Armed only after a frame in which the anchor was accepted: a frame without one never shakes
  // (F4-DESIGN.md 2.5); with an upscaler chosen, only while its context is ready.
  frame_.armed = ran_last_ && !disarmed_ && (effective_ == Upscaler::kOff || ready);
  if (frame_.armed) {
    frame_.phase_count = kPhasesAtRatio1;  // 8 x ratio^2, ratio 1
    frame_.jitter_index = jitter_index_ % frame_.phase_count;
    // Halton(2,3) from index 1, minus a half: in (-0.5, 0.5) render pixels, +y down. (Development,
    // bit 2: the scene is not moved.)
    if (!(debug_bits_ & 2u)) {
      frame_.jitter[0] = float(temporal::Halton(frame_.jitter_index + 1, 2) - 0.5);
      frame_.jitter[1] = float(temporal::Halton(frame_.jitter_index + 1, 3) - 0.5);
    }
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

void TemporalSystem::NoteStencil(const XenosState& s) {
  // The vehicle IDs: a draw that REPLACEs the stencil with a reference of 4k + {0, 1} writes ID k
  // (census 1.1). Either face may do it.
  if (!s.depth_bound || !s.depth_control.stencil_enable) {
    return;
  }
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  if ((s.stencil.stencilwritemask & 0xFFu) != 0 && dc.stencilzpass == xe::StencilOp::kReplace) {
    frame_.stencil_ids |= uint64_t(1) << ((s.stencil.stencilref & 0xFFu) >> 2);
  }
  if (dc.backface_enable && (s.stencil_bf.stencilwritemask & 0xFFu) != 0 &&
      dc.stencilzpass_bf == xe::StencilOp::kReplace) {
    frame_.stencil_ids |= uint64_t(1) << ((s.stencil_bf.stencilref & 0xFFu) >> 2);
  }
}

void TemporalSystem::NoteStill(const DrawFacts& f, uint32_t failed) {
  if (still_logged_.size() >= kMaxStillLines) {
    return;
  }
  const DrawRecord& r = f.view->record();
  const uint64_t key = r.vs_identity * 0x9E3779B97F4A7C15ull ^ r.ps_identity ^ (uint64_t(failed) << 60);
  if (!still_logged_.insert(key).second) {
    return;
  }
  std::string reasons;
  static const char* const kNames[kGateReasons] = {"clipping off", "no viewport transform",
                                                   "rectangle list", "narrow viewport"};
  for (uint32_t i = 0; i < kGateReasons; ++i) {
    if (failed & (1u << i)) {
      reasons += std::format("{}{}", reasons.empty() ? "" : ", ", kNames[i]);
    }
  }
  REXLOG_INFO("{} a main view draw is kept still ({}): VS {:016X} PS {:016X}, {} {} (the first {} "
              "kinds; the summaries count them)",
              kPrefix, reasons, r.vs_identity, r.ps_identity, r.count, PrimitiveName(r.primitive),
              kMaxStillLines);
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
    ++fr.late_draws;
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
  NoteStencil(*f.state);

  const uint32_t failed = EvaluateGate(*f.state, f.view->record(), *f.viewport, f.scale);
  if (failed != 0) {
    ++fr.gate_rejected;
    for (uint32_t i = 0; i < kGateReasons; ++i) {
      fr.gate_failed[i] += (failed >> i) & 1u;
    }
    NoteStill(f, failed);
    return;
  }
  // A viewport whose depth is not mirrored is a forward-Z draw (the scene's depth is 1 near, 0
  // far: ComputeViewport mirrors the guest's reversed range); a few draws a frame are.
  fr.forward_z_draws += f.viewport->ndc_scale[2] >= 0.0f;
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
  ++window_.anchors;
  fr.anchor_seen = true;  // the main view's draws are done: no more jitter
  if (fr.anchors++ != 0) {
    ++window_.second_anchors;
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  std::string why;
  AnchorStatus status = CheckAnchor(f, &width, &height, &why);
  temporal::Camera camera;
  if (status == AnchorStatus::kAccepted) {
    const temporal::Reject reject = temporal::ExtractCamera(
        f.banks->ps.data(), width, height, fr.frame, fr.stencil_ids, &camera);
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
    return false;
  }

  // Accepted. What the frame's camera is, and whether its history starts again.
  fr.anchor_ok = true;
  fr.camera = camera;
  fr.guest_size[0] = camera.width;
  fr.guest_size[1] = camera.height;
  fr.render_size[0] = fr.scale.Edge(camera.width);
  fr.render_size[1] = fr.scale.Edge(camera.height);
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
        last_scale_ != fr.scale) {
      reset |= kResetSize;
    }
    cut = temporal::TestCut(camera, last_camera_, last_step_known_ ? last_step_ : nullptr);
    fr.cut = cut;
    if (cut.fires) {
      reset |= kResetCut;
    }
  }
  if (!(camera.previous_valid & 1u)) {
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
                "{:.1f}, camera at {:.2f}, {:.2f}, {:.2f}, looking along {:.3f}, {:.3f}, {:.3f}, "
                "c64-c67 {} the inverse view-projection{}",
                kPrefix, fr.frame, camera.fov_y * 180.0 / kPi, camera.aspect, camera.near_plane,
                camera.far_plane, camera.position[0], camera.position[1], camera.position[2],
                camera.forward[0], camera.forward[1], camera.forward[2],
                camera.pixel_to_world_agrees ? "agree with" : "DO NOT agree with",
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

  // c69 against the view-projection this renderer saw a frame ago: is it the previous frame's, and
  // in which pixel layout (the census could not tell: the camera never moved).
  if (ran_last_ && reset == 0) {
    const temporal::History h = temporal::CompareToHistory(camera, last_camera_);
    ++window_.history_checks;
    window_.history_flip += h.flip < temporal::kHistoryTolerance;
    window_.history_noflip += h.noflip < temporal::kHistoryTolerance;
  }
  NoteRigid(camera, fr.frame);

  // What the passes of step 1b need, for the Scene's call of RecordAnchorPasses right after this
  // draw's constants are known: the anchor's own fetches (the depth, the stencil view, the colour),
  // the sizes, both jitters and the cameras. The vectors use the frame before's camera only when it
  // follows this one without a restart (a cut, a size change or a gap leave them zero).
  pending_ = TemporalMotion::Frame{};
  pending_.frame = fr.frame;
  pending_.color_slot = fr.anchor_textures.table[0];
  pending_.stencil_slot = fr.anchor_textures.table[1];
  std::memcpy(pending_.stencil_fetch, fr.anchor_textures.fetch[1], sizeof(pending_.stencil_fetch));
  pending_.depth_slot = fr.anchor_textures.table[2];
  for (int i = 0; i < 2; ++i) {
    pending_.render_size[i] = fr.render_size[i];
    pending_.guest_size[i] = fr.guest_size[i];
    pending_.jitter[i] = fr.jitter[i];
    pending_.previous_jitter[i] = last_jitter_[i];
  }
  pending_.camera = &fr.camera;
  pending_.previous =
      (ran_last_ && (reset & ~uint32_t(kResetNoPrevious)) == 0) ? &last_camera_ : nullptr;
  pending_ready_ = motion_.available();
  return pending_ready_;
}

void TemporalSystem::NoteRigid(const temporal::Camera& camera, uint64_t frame) {
  // The census could not say what the vehicle slots c69 + 4k mean (the world's turned out not to be
  // the previous camera). If a slot is the vehicle's world to the previous frame's pixels, then
  // c(69+4k) * inverse(c69) is the vehicle's own motion over one frame, a rigid transform: a
  // rotation block of determinant 1, last column 0 0 0 1, a translation of about speed times
  // frame time (0.5 units at 28 units a second). Counted always, the first lines of moving ones.
  constexpr double kRigidTolerance = 1e-3;
  constexpr double kMoving = 1e-3;
  constexpr uint32_t kMaxRigidLines = 24;
  for (uint32_t k = 1; k < temporal::kVehicleMatrices; ++k) {
    const temporal::Rigid& r = camera.rigid[k];
    if (!(camera.previous_valid & (1u << k)) || !r.valid) {
      continue;
    }
    ++window_.rigid_checked;
    const bool rigid = std::fabs(r.determinant - 1.0) <= kRigidTolerance &&
                       r.orthonormal <= kRigidTolerance && r.last_column <= kRigidTolerance;
    window_.rigid_ok += rigid;
    const bool moving = r.translation > kMoving;
    window_.rigid_moving += moving;
    if (moving && rigid_lines_ < kMaxRigidLines) {
      ++rigid_lines_;
      REXLOG_INFO("[gpu] temporal mv: vehicle ID {} in frame {}: c(69+4k) * inverse(c69) has "
                  "determinant {:.6f}, orthonormality error {:.2e}, last column error {:.2e}, "
                  "moves {:.4f} units: {}{}",
                  k, frame, r.determinant, r.orthonormal, r.last_column, r.translation,
                  rigid ? "a rigid transform" : "NOT a rigid transform",
                  rigid_lines_ == kMaxRigidLines ? " (the last of these lines; the summaries count)"
                                                 : "");
    }
  }
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
      ++window_.misses;
      if (++misses_ >= kDisarmAfterMisses && !disarmed_) {
        disarmed_ = true;
        REXLOG_WARN("{} {} frames in a row were jittered and had no usable anchor ({}): the "
                    "jitter is off until mcla_gpu_temporal is turned off and on",
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
    last_jitter_[0] = fr.jitter[0];
    last_jitter_[1] = fr.jitter[1];
    last_scale_ = fr.scale;
    last_render_size_[0] = fr.render_size[0];
    last_render_size_[1] = fr.render_size[1];
    ++jitter_index_;  // advances in the frames the pass ran in only
  }

  // The window.
  Window& w = window_;
  ++w.frames;
  w.armed += fr.armed;
  const Mode mode = ModeOf(fr);
  ++w.modes[size_t(mode)];
  ++w.anchor_status[size_t(fr.anchor_status)];
  for (uint32_t i = 0; i < kKinds; ++i) {
    w.main_draws[i] += fr.main_draws[i];
    w.jittered[i] += fr.jittered[i];
  }
  for (uint32_t i = 0; i < kGateReasons; ++i) {
    w.gate_failed[i] += fr.gate_failed[i];
  }
  w.gate_rejected += fr.gate_rejected;
  w.forward_z_draws += fr.forward_z_draws;
  w.view4x_draws += fr.view4x_draws;
  w.late_draws += fr.late_draws;
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
    w.vehicle_matrices += uint64_t(std::popcount(fr.camera.previous_valid));
    w.pixel_to_world_agrees += fr.camera.pixel_to_world_agrees;
    w.have_camera = true;
    w.camera = fr.camera;
    w.render_size[0] = fr.render_size[0];
    w.render_size[1] = fr.render_size[1];
  }

  NoteMode(mode, fr);
  ++frame_number_;
  frame_open_ = false;
  if (std::chrono::duration<double>(Clock::now() - window_start_).count() >= kReportSeconds) {
    Report("interval");
  }
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

void TemporalSystem::Report(const char* why) {
  const Window& w = window_;
  const double seconds = std::chrono::duration<double>(Clock::now() - window_start_).count();
  const uint64_t running = w.modes[size_t(Mode::kRunning)];
  const uint64_t scene = running + w.modes[size_t(Mode::kNoAnchor)];
  const double per_scene = scene != 0 ? 1.0 / double(scene) : 0.0;

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
  REXLOG_INFO("{} ({}) {} frames in {:.1f} s: 3D scene {} (anchor taken {}, at 4x samples {}), no "
              "scene {}, jitter armed {} | refused anchors: {} | history restarts: {} ({}) | camera "
              "{}",
              kPrefix, why, w.frames, seconds, scene, running, w.modes[size_t(Mode::kView4x)],
              w.modes[size_t(Mode::kNoScene)], w.armed, refused.empty() ? "none" : refused,
              w.resets, resets.empty() ? "none" : resets, camera);

  std::string gate;
  static const char* const kGateNames[kGateReasons] = {"clipping off", "no viewport transform",
                                                       "rectangle list", "narrow viewport"};
  for (uint32_t i = 0; i < kGateReasons; ++i) {
    if (w.gate_failed[i] != 0) {
      gate += std::format("{}{} {:.2f}", gate.empty() ? "" : ", ", kGateNames[i],
                          double(w.gate_failed[i]) * per_scene);
    }
  }
  const uint64_t drawn =
      w.main_draws[kColorDepth] + w.main_draws[kColorOnly] + w.main_draws[kDepthOnly];
  const uint64_t moved =
      w.jittered[kColorDepth] + w.jittered[kColorOnly] + w.jittered[kDepthOnly];
  REXLOG_INFO("{} ({}) per 3D scene frame: main view draws {:.1f} (colour+depth {:.1f}, colour only "
              "{:.1f}, depth only {:.1f}), moved {:.1f} (colour+depth {:.1f}, colour only {:.1f}, "
              "depth only {:.1f}), kept still {:.2f} ({}), forward-Z {:.2f}, after the anchor "
              "{:.2f}, second anchors {} | c69 is the previous view-projection times the pixel matrix in {} of {} checks "
              "(with y down: {}) | usable previous matrices {:.2f} a frame | c64-c67 agree with "
              "the inverse view-projection in {} of {}",
              kPrefix, why, double(drawn) * per_scene, double(w.main_draws[kColorDepth]) * per_scene,
              double(w.main_draws[kColorOnly]) * per_scene,
              double(w.main_draws[kDepthOnly]) * per_scene, double(moved) * per_scene,
              double(w.jittered[kColorDepth]) * per_scene,
              double(w.jittered[kColorOnly]) * per_scene,
              double(w.jittered[kDepthOnly]) * per_scene, double(w.gate_rejected) * per_scene,
              gate.empty() ? "none" : gate, double(w.forward_z_draws) * per_scene,
              double(w.late_draws) * per_scene, w.second_anchors,
              w.history_flip, w.history_checks, w.history_noflip,
              running != 0 ? double(w.vehicle_matrices) / double(running) : 0.0,
              w.pixel_to_world_agrees, running);
  const TemporalMotion::Counters mv = motion_.TakeCounters();
  REXLOG_INFO("[gpu] temporal mv: ({}) vectors in {} frames ({} zero after a history restart, {} "
              "anchors whose passes could not be recorded, {} images failed), vehicle ID passes {}, "
              "checks judged {} | vehicle slots measured as rigid transforms: {} checked, {} rigid "
              "within 1e-3, {} moving",
              why, mv.frames, mv.zero_frames, w.mv_missed, mv.failed, mv.vehicle_draws, mv.checks,
              w.rigid_checked, w.rigid_ok, w.rigid_moving);
  if (effective_ != Upscaler::kOff || w.upscaled != 0) {
    const BackendStatus status = backend_ ? backend_->Status() : BackendStatus{};
    REXLOG_INFO("[gpu] temporal upscaler: ({}) {} ({}{}) in {} frames: upscaled {} ({} of them with "
                "the history started again), plain: context not ready {}, no vectors {}, colour not "
                "an input {}{}",
                why, UpscalerName(effective_), ModeInfoOf(settings_.mode).name,
                status.provider.empty() ? std::string() : ", provider " + status.provider, w.frames,
                w.upscaled, w.upscale_resets, w.upscale_plain_not_ready, w.upscale_plain_no_vectors,
                w.upscale_plain_no_color,
                status.failed ? std::format(" | FAILED: {}", status.reason) : std::string());
  }
  window_ = Window{};
  window_start_ = Clock::now();
}

void TemporalSystem::Finish() {
  if (enabled_ && window_.frames != 0) {
    Report("final");
  }
}

}  // namespace mcla::gpu
