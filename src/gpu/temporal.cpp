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
#include "texture_bytes.h"
#include "textures.h"
#include "topology.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal:";
constexpr uint32_t kMaxResetLines = 16;
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
  plain_logged_.fill(false);
  if (enabled) {
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

void TemporalSystem::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                HostPasses& passes, PassTiming& pass_timing,
                                TextureCache& textures, ResolveCache& resolve) {
  motion_.Initialize(device, heap, passes, pass_timing);
  tone_map_.Initialize(passes, pass_timing);
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
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
  motion_.Shutdown();
  tone_map_.Shutdown();
  pending_ready_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
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
    return false;
  }
  RecordUpscale(list);
  return true;
}

uint64_t TemporalSystem::HeldBytes() const {
  uint64_t bytes = motion_.HeldBytes() + tone_map_.HeldBytes();
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

void TemporalSystem::ReadUpscalerChoice() {
  settings_ = ReadUpscalerSettings();
  // The scene was made at the size the start chose (gpu.h GetStartUpscale; it cannot change while
  // the game runs): the mode in use is the one it was drawn for, whatever the setting says now.
  const StartUpscale start = PeekStartUpscale();
  mode_in_use_ = start.decided && start.runs ? start.mode : UpscaleMode::kNativeAa;
  const UpscalerAvailability available = GetUpscalerAvailability();
  Upscaler effective = Upscaler::kOff;
  const TemporalBackend* chosen_backend = BackendFor(settings_.upscaler);
  if (chosen_backend && ((settings_.upscaler == Upscaler::kFsr31 && available.fsr31) ||
                         (settings_.upscaler == Upscaler::kFsr4 && available.fsr4) ||
                         (settings_.upscaler == Upscaler::kDlss && available.dlss))) {
    effective = settings_.upscaler;
  }
  const bool chosen_probed =
      settings_.upscaler == Upscaler::kDlss ? available.dlss_probed : available.probed;
  // A line when what the choice comes to changes. A choice this PC cannot run is off (the saved
  // choice stays as it is, and comes back where it can run).
  const uint32_t key = uint32_t(settings_.upscaler) | uint32_t(effective) << 2 |
                       uint32_t(chosen_probed) << 4 | uint32_t(chosen_backend != nullptr) << 5;
  if (!choice_reported_ || key != choice_key_) {
    choice_reported_ = true;
    choice_key_ = key;
    if (settings_.upscaler != Upscaler::kOff) {
      if (effective != Upscaler::kOff) {
        REXLOG_INFO("{} upscaling: {} ({} {}%), sharpness {:.2f}", kPrefix, UpscalerName(effective),
                    ModeInfoOf(effective, mode_in_use_).name,
                    ModeInfoOf(effective, mode_in_use_).percent,
                    effective == Upscaler::kDlss ? 0.0f : settings_.sharpness);
      } else if (!chosen_backend) {
        REXLOG_INFO("{} {} is chosen but this build has no runtime for it: treated as off", kPrefix,
                    UpscalerName(settings_.upscaler));
      } else if (chosen_probed) {
        REXLOG_INFO("{} {} is chosen but this PC cannot run it{}: treated as off (the choice is "
                    "kept)",
                    kPrefix, UpscalerName(settings_.upscaler),
                    settings_.upscaler == Upscaler::kDlss && available.dlss_needs_setup
                        ? " (its files are missing: it needs setup)"
                        : settings_.upscaler == Upscaler::kDlss && available.dlss_after_restart
                              ? " until the game is started again"
                              : "");
      }
    }
  }
  if (effective_ != effective) {
    // From one upscaler to another: the one that stops holds no context any more.
    TemporalBackend* old_backend = BackendFor(effective_);
    if (old_backend && old_backend != BackendFor(effective)) {
      old_backend->Release();
    }
  }
  effective_ = effective;
  // What the mode comes to now against what the scene was drawn for: a line when that changes.
  // The scene's size is the start's (restart only), so a mode picked since, and an upscaler
  // that is off or cannot run while the scene is drawn smaller, are said once.
  const bool smaller = start.decided && start.runs && start.ratio > 1.0;
  const bool mode_differs =
      start.decided && effective != Upscaler::kOff && settings_.mode != mode_in_use_;
  const bool soft = smaller && effective == Upscaler::kOff;
  const uint32_t note = uint32_t(settings_.mode) | uint32_t(mode_in_use_) << 4 |
                        uint32_t(mode_differs) << 8 | uint32_t(soft) << 9;
  if (note != mode_note_key_) {
    mode_note_key_ = note;
    if (mode_differs) {
      REXLOG_INFO("{} {} {} ({}%) is chosen, but this game was started with the 3D scene drawn "
                  "for {} ({}%): the scene's size is fixed at the start, so the new mode applies "
                  "at the next start",
                  kPrefix, UpscalerName(effective), ModeInfoOf(effective, settings_.mode).name,
                  ModeInfoOf(effective, settings_.mode).percent,
                  ModeInfoOf(start.upscaler, mode_in_use_).name,
                  ModeInfoOf(start.upscaler, mode_in_use_).percent);
    }
    if (soft) {
      REXLOG_INFO("{} no upscaler is running, but this game was started with the 3D scene drawn "
                  "for {} ({}% of the output): the scene is soft until the upscaler is on again "
                  "(or the game is started again with none)",
                  kPrefix, ModeInfoOf(start.upscaler, mode_in_use_).name,
                  ModeInfoOf(start.upscaler, mode_in_use_).percent);
    }
  }
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
  tone_map_.Release();
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

  // TEST ONLY (mcla_gpu_upscaler_tonemap): off, the upscaler reads the product and writes the
  // output image itself, as it did before the tone map.
  const bool tone_map = UpscalerToneMapEnabled();
  // The compression reads the product at the render size, texel for texel, through a view of its
  // own with no swizzle: the stand-in is read through each fetch's swizzle later, once.
  uint32_t color_slot = kInvalidBindlessSlot;
  if (tone_map) {
    color_slot = product->views.Slot(*device_, *heap_, *product->texture, product->format, 1,
                                     format::kHostSwizzleRGBA, "temporal upscaler input view");
    if (color_slot == kInvalidBindlessSlot) {
      return plain(window_.upscale_plain_no_color, 2,
                   "the anchor's colour product has no view for the compression");
    }
  }

  // The output: the picture the rest of the frame reads in place of the scene colour. The expansion
  // draws it (a render target); the upscaler itself writes the tone map's own image
  // (temporal_tonemap.h). With the tone map off the upscaler writes it (a compute pass's image).
  constexpr plume::RenderFormat kOutputFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
  const plume::RenderTextureFlags output_flags =
      tone_map ? plume::RenderTextureFlags(plume::RenderTextureFlag::RENDER_TARGET)
               : plume::RenderTextureFlags(plume::RenderTextureFlag::STORAGE |
                                           plume::RenderTextureFlag::UNORDERED_ACCESS);
  if (output_.texture && (output_.width != out_width || output_.height != out_height)) {
    textures_->ReleaseOverrideViews();  // before the texture they name goes
  }
  if (!passes_->EnsureImage(output_, out_width, out_height, kOutputFormat,
                            "temporal upscaler output", output_flags)) {
    FailUpscaler(std::format("its {}x{} output image could not be made", out_width, out_height));
    return false;
  }
  if (tone_map && !tone_map_.Prepare(width, height, out_width, out_height)) {
    FailUpscaler(std::format("the tone map around it could not be made (passes, or its {}x{} and "
                             "{}x{} images)",
                             width, height, out_width, out_height));
    return false;
  }
  // What the upscaler reads and writes (empty images when the tone map is off).
  HostImage& compressed = tone_map_.compressed();
  HostImage& upscaled = tone_map_.upscaled();
  HostImage& upscaler_out = tone_map ? upscaled : output_;

  TemporalInputs in;
  in.frame = fr.frame;
  in.render_size[0] = width;
  in.render_size[1] = height;
  in.output_size[0] = out_width;
  in.output_size[1] = out_height;
  in.color = tone_map ? TemporalImage{compressed.texture.get(), width, height, compressed.format,
                                      compressed.flags}
                      : TemporalImage{product->texture.get(), width, height, kOutputFormat,
                                      plume::RenderTextureFlag::NONE};
  in.depth = {kept.depth->texture.get(), kept.depth->width, kept.depth->height, kept.depth->format,
              kept.depth->flags};
  in.motion = {kept.motion->texture.get(), kept.motion->width, kept.motion->height,
               kept.motion->format, kept.motion->flags};
  in.output = {upscaler_out.texture.get(), out_width, out_height, upscaler_out.format,
               upscaler_out.flags};
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

  // Outside a render pass (the Scene ended it). The scene colour is compressed into the upscaler's
  // input first (a pixel shader reads the product; it stays readable by both stages). The inputs
  // then go to the compute stage's read state (D3D12 NON_PIXEL_SHADER_RESOURCE, Vulkan
  // SHADER_READ_ONLY_OPTIMAL), the output to general; the runtime takes them back to those states;
  // the depth and the vectors go back to what the passes read them in, the output to what the
  // expansion's pixel shader reads, and the expansion draws the image the rest of the frame reads.
  namespace Stage = plume::RenderBarrierStage;
  const plume::RenderBarrierStages graphics_and_compute = Stage::GRAPHICS_AND_COMPUTE;
  if (tone_map) {
    list->barriers(graphics_and_compute,
                   plume::RenderTextureBarrier(product->texture.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
    tone_map_.RecordCompress(list, color_slot);
  }
  pass_timing_->Mark(list, "temporal upscale");
  plume::RenderTextureBarrier to_read[3] = {
      plume::RenderTextureBarrier(tone_map ? compressed.texture.get() : product->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::COMPUTE, to_read, 3);
  list->barriers(Stage::COMPUTE,
                 plume::RenderTextureBarrier(upscaler_out.texture.get(),
                                             plume::RenderTextureLayout::GENERAL));
  const bool ok = backend->Upscale(list, in);
  backend::InvalidateAfterExternalWork(list);
  if (!tone_map) {
    list->barriers(graphics_and_compute,
                   plume::RenderTextureBarrier(product->texture.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
  }
  plume::RenderTextureBarrier back[2] = {
      plume::RenderTextureBarrier(kept.depth->texture.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(kept.motion->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(Stage::GRAPHICS, back, 2);
  if (tone_map) {
    list->barriers(Stage::GRAPHICS, plume::RenderTextureBarrier(
                                        upscaled.texture.get(),
                                        plume::RenderTextureLayout::SHADER_READ));
    upscaled.layout = plume::RenderTextureLayout::SHADER_READ;
  } else {
    list->barriers(graphics_and_compute,
                   plume::RenderTextureBarrier(output_.texture.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
    output_.layout = plume::RenderTextureLayout::SHADER_READ;
  }
  if (!ok) {
    return false;  // the backend logged it and is off for the session; this frame is plain
  }
  if (tone_map) {
    tone_map_.RecordExpand(list, output_);
    list->barriers(graphics_and_compute,
                   plume::RenderTextureBarrier(output_.texture.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
    output_.layout = plume::RenderTextureLayout::SHADER_READ;
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
  // The anchor's own colour fetch: the binding the stand-in gives this fetch's swizzle (the anchor
  // draws at the output's scale).
  fr.anchor_color_slot = textures_->Bind2D(fetch, fr.output_scale, list).slot;
  last_dispatch_frame_ = fr.frame;
  ++window_.upscaled;
  window_.upscale_resets += in.reset;
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

void TemporalSystem::BeginFrame(uint32_t slot) {
  ReadUpscalerChoice();
  const bool want = effective_ != Upscaler::kOff;
  if (want != enabled_ && !frame_open_) {
    SetEnabled(want);  // only between frames: a frame is jittered whole or not at all
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
  // The upscaler's context, for the sizes the last accepted frame had (the scene at the render
  // size, the output at the images of the screen's): asked for while the upscaler is chosen and can
  // run; the jitter waits for it, so a frame it cannot take never shakes.
  frame_.upscaler = effective_;
  bool ready = false;
  TemporalBackend* backend = BackendFor(effective_);
  if (effective_ != Upscaler::kOff && backend && ran_last_ && last_render_size_[0] != 0 &&
      last_output_size_[0] != 0) {
    BackendRequest request;
    MakeRequest(&request);
    backend->Request(request);
    request_ = request;
    request_made_ = true;
    ready = backend->Ready(request);
  }
  frame_.backend_ready = ready;
  if (ready != ready_reported_) {
    ready_reported_ = ready;
    if (ready) {
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
  frame_.armed = ran_last_ && !disarmed_ && (effective_ == Upscaler::kOff || ready);
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
                "the history started again), plain: context not ready {}, no vectors {}, colour not "
                "an input {}{}",
                UpscalerName(effective_), ModeInfoOf(effective_, settings_.mode).name,
                status.provider.empty() ? std::string() : ", provider " + status.provider, w.frames,
                w.upscaled, w.upscale_resets, w.upscale_plain_not_ready, w.upscale_plain_no_vectors,
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
