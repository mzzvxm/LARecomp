// MCLA GPU renderer -- the scene above paper white: the tone map drawn again into a float image.
// See hdr_tonemap.h.

#include "hdr_tonemap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "draw_decode.h"
#include "pass_timing.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_hdr_highlights, 2.0, "MCLA/GPU",
    "HDR output: how far the game's lights, the sun, the sky at dusk and the headlights go above "
    "the game's white: 1 normal, 1.5 bright, 2 brighter, 3 brightest. It is the steepness of the "
    "HDR curve above where it leaves the game's, so the brightest light stays under the display's "
    "peak whatever it is. Changes at once; nothing happens while HDR output is off.")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr plume::RenderFormat kSceneFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr plume::RenderFormat kTargetFormat = plume::RenderFormat::R8G8B8A8_UNORM;

// The game's PS constants: Gamma.x is c70.x of the pixel bank (4 floats of 16 bytes per register).
constexpr uint32_t kGammaRegister = 70;

const char* CauseName(HdrToneMap::Cause cause) {
  switch (cause) {
    case HdrToneMap::Cause::kAa:
      return "mcla_gpu_aa is on (its passes run on the 8-bit scene)";
    case HdrToneMap::Cause::kTarget:
      return "the tone map's target is not a single-sampled R8G8B8A8 one";
    case HdrToneMap::Cause::kState:
      return "the tone map is drawn with blending, a mask without RGB, an alpha test or depth/stencil in use";
    case HdrToneMap::Cause::kCoverage:
      return "the tone map's scissor or viewport does not start at the origin";
    case HdrToneMap::Cause::kPipeline:
      return "a pipeline of the second draw could not be made";
    case HdrToneMap::Cause::kImage:
      return "the float image could not be made";
    case HdrToneMap::Cause::kUpload:
      return "no upload memory for the tone map's constants";
    case HdrToneMap::Cause::kAfterTarget:
      return "a draw after the tone map into its target cannot be drawn again (several colour "
             "outputs, samples, depth or stencil, or a pipeline that is not ready) and does more "
             "than add light";
    case HdrToneMap::Cause::kClear:
      return "a clear of the target after the tone map";
    case HdrToneMap::Cause::kMarker:
      return "the composite marker is not on the tone map's target at its size";
    case HdrToneMap::Cause::kCount:
      break;
  }
  return "?";
}

float LoadBeFloat(const uint8_t* p) {
  return std::bit_cast<float>(LoadBe32(p));
}

// A depth or stencil test that can reject pixels: the second draw has no depth target. A bound depth
// target the draw does not test (or tests with ALWAYS), writing or not, changes nothing a float
// image without one would show.
// Whether a depth or stencil test could leave pixels out. The float scene's draw has no depth/stencil
// target (VariantKey drops both), so only a test that rejects matters: the game's tone map has stencil
// on with ALWAYS front and back (it writes a stencil mark with REPLACE), which rejects nothing.
bool DepthInUse(const PipelineKey& key) {
  if (key.depth_format == 0) {
    return false;
  }
  constexpr auto kAlways = plume::RenderComparisonFunction::ALWAYS;
  if ((key.stencil & 1u) != 0 &&
      (plume::RenderComparisonFunction((key.stencil >> 1) & 0xFu) != kAlways ||
       plume::RenderComparisonFunction((key.stencil >> 17) & 0xFu) != kAlways)) {
    return true;
  }
  return (key.depth & 1u) != 0 &&
         plume::RenderComparisonFunction((key.depth >> 2) & 0xFu) != kAlways;
}

}  // namespace

bool HdrToneMap::AddsLight(uint32_t packed_blend) {
  const plume::RenderBlendDesc blend = UnpackBlend(packed_blend);
  return blend.blendEnabled && blend.blendOp == plume::RenderBlendOperation::ADD &&
         blend.dstBlend == plume::RenderBlend::ONE;
}

HdrToneMap::~HdrToneMap() {
  Shutdown();
}

bool HdrToneMap::Initialize(HostPasses& passes, ShaderStore& shaders, FrameLayers& layers,
                            PostAa& post, PassTiming& pass_timing) {
  passes_ = &passes;
  shaders_ = &shaders;
  layers_ = &layers;
  post_ = &post;
  pass_timing_ = &pass_timing;
  for (uint32_t i = 0; i < 257; ++i) {
    ramp_[i] = float(std::min(i, 255u)) / 255.0f;
  }
  initialized_ = true;
  REXLOG_INFO("[gpu] hdr scene: set up (the tone map is drawn again into a float image once the HDR "
              "variants of the game's shaders are loaded: {}; highlights {}, knee {})",
              shaders.has_hdr_variants() ? "they are" : "not yet",
              REXCVAR_GET(mcla_gpu_hdr_highlights), kKnee);
  return true;
}

void HdrToneMap::Shutdown() {
  // The GPU is idle and the heap goes later: the slot is not handed back.
  scene_ = HostImage{};
  initialized_ = false;
  passes_ = nullptr;
  shaders_ = nullptr;
  layers_ = nullptr;
  post_ = nullptr;
  pass_timing_ = nullptr;
}

void HdrToneMap::SetDisplay(const HdrLevels& levels, const float* ramp) {
  levels_ = levels;
  have_ramp_ = ramp != nullptr;
  for (uint32_t i = 0; i < 256; ++i) {
    ramp_[i] = ramp ? ramp[i] : float(i) / 255.0f;
  }
  ramp_[256] = ramp_[255];
}

void HdrToneMap::BeginFrame() {
  phase_ = Phase::kIdle;
  ready_this_frame_ = false;
  candidate_valid_ = false;
}

// ---------------------------------------------------------------------------
// the viewing transfer and the peak
// ---------------------------------------------------------------------------

float HdrToneMap::ViewingTransfer(float game_linear, float gamma) const {
  // The game's encode (as the tone map ends: 1.055 x^gamma - 0.055, 12.92 x near black), the display's
  // table between its entries and along its last step above 1 (present_ps.hlsl Ramp), the sRGB
  // decode (hdr_output.hlsli HdrSdrToLinear): light relative to paper white.
  const float code =
      game_linear > 0.00304f ? 1.055f * std::pow(game_linear, gamma) - 0.055f : game_linear * 12.92f;
  const float position = std::max(code, 0.0f) * 255.0f;
  const uint32_t index = uint32_t(std::min(std::floor(position), 254.0f));
  const float low = ramp_[index];
  const float high = ramp_[index + 1];
  const float shown = std::max(low + (high - low) * (position - float(index)), 0.0f);
  return shown > 0.04045f ? std::pow((shown + 0.055f) / 1.055f, 2.4f) : shown / 12.92f;
}

float HdrToneMap::PeakInGameUnits(float gamma) const {
  const float relative = levels_.peak / std::max(levels_.white, 1.0f);
  float low = 0.0f;
  float high = 64.0f;
  for (uint32_t i = 0; i < 32; ++i) {
    const float middle = (low + high) * 0.5f;
    (ViewingTransfer(middle, gamma) > relative ? high : low) = middle;
  }
  return low;
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

HdrToneMap::Action HdrToneMap::Classify(const DrawFacts& f) {
  if (!active() || !f.key || f.key->color_count == 0) {
    return Action::kNone;
  }
  // The tone map (also while a frame's float scene is under way: a second one starts again).
  if (f.ps && f.ps->hdr_variant && f.vs) {
    const ShaderStore::HdrVariant* variant = shaders_->FindHdrVariant(f.ps->key);
    if (variant && variant->DrawnWith(f.vs->key)) {
      return ClassifyToneMap(f);
    }
    if (variant && !vs_logged_) {
      // The tone map's pixel shader with a vertex shader the variant was not built for: its
      // outputs may not be the ones the variant reads (the pack builder's TARGETS names them).
      vs_logged_ = true;
      REXLOG_INFO("[gpu] hdr scene: the game's tone map pixel shader {:016X} is drawn with vertex shader "
                  "{:016X}, which the HDR pack does not name; its frames keep the scene at paper white",
                  f.ps->key, f.vs->key);
    }
  }
  if (phase_ != Phase::kPending || !(f.color0 == target_)) {
    return Action::kNone;
  }
  // A draw into the tone map's target before the marker.
  const PipelineKey& key = *f.key;
  const bool reproducible = key.color_count == 1 && key.sample_count == 1 && f.host_samples == 1 &&
                            key.color_format[0] == uint32_t(kTargetFormat) && !DepthInUse(key);
  NoteAfter(f, reproducible);
  if (!reproducible) {
    Failed(Action::kAfter, Cause::kAfterTarget, false, AddsLight(key.blend[0]));
    return Action::kNone;
  }
  return Action::kAfter;
}

HdrToneMap::Action HdrToneMap::ClassifyToneMap(const DrawFacts& f) {
  const PipelineKey& key = *f.key;
  const XenosState& s = *f.state;
  candidate_valid_ = false;
  // Anything but a clean tone map ends a float scene that was under way on this target: the target's
  // content is the game's again.
  const auto refuse = [&](Cause cause, const std::string& detail) {
    Fail(cause, detail);
    return Action::kNone;
  };
  if (!f.aa_held_off && post_->CurrentMode() != PostAa::Mode::kOff) {
    return refuse(Cause::kAa, std::string());
  }
  if (key.color_count != 1 || key.sample_count != 1 || f.host_samples != 1 ||
      key.color_format[0] != uint32_t(kTargetFormat) || f.color0.samples != 1) {
    return refuse(Cause::kTarget, DescribeEdramKey(f.color0));
  }
  const plume::RenderBlendDesc blend = UnpackBlend(key.blend[0]);
  const uint8_t rgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                      uint8_t(plume::RenderColorWriteEnable::GREEN) |
                      uint8_t(plume::RenderColorWriteEnable::BLUE);
  if (blend.blendEnabled || (blend.renderTargetWriteMask & rgb) != rgb || key.ps_variant != 0 ||
      DepthInUse(key)) {
    return refuse(Cause::kState, std::format("blend {:#010x}, depth {:#x}, stencil {:#x}, "
                                             "variant {}",
                                             key.blend[0], key.depth, key.stencil, key.ps_variant));
  }
  // Coverage: the region the draw writes is the scissor and the viewport's overlap, from the
  // origin (the game's tone map is a full-screen quad with the scissor at (0,0)-(1280,720) and the
  // viewport 640/640 by -360/360, which the host has as 0,0 1280x720 times the scale). The float
  // image is that region, and the marker has to find the layers' region the same size.
  const uint32_t tl_x = s.scissor_tl & 0x7FFFu;
  const uint32_t tl_y = (s.scissor_tl >> 16) & 0x7FFFu;
  const uint32_t br_x = std::min<uint32_t>(s.scissor_br & 0x7FFFu, s.surface_pitch);
  const uint32_t br_y = (s.scissor_br >> 16) & 0x7FFFu;
  const plume::RenderViewport& vp = *f.viewport;
  const float slack = std::max(1.0f, f.scale.value());
  const uint32_t host_w = std::min(f.scale.Edge(br_x), uint32_t(std::max(vp.x + vp.width, 0.0f) + 0.5f));
  const uint32_t host_h = std::min(f.scale.Edge(br_y), uint32_t(std::max(vp.y + vp.height, 0.0f) + 0.5f));
  if (tl_x != 0 || tl_y != 0 || br_x == 0 || br_y == 0 || vp.x > slack || vp.y > slack ||
      host_w == 0 || host_h == 0) {
    return refuse(Cause::kCoverage,
                  std::format("scissor ({},{})-({},{}), viewport {:.1f},{:.1f} {:.1f}x{:.1f}, pitch {}",
                              tl_x, tl_y, br_x, br_y, vp.x, vp.y, vp.width, vp.height,
                              s.surface_pitch));
  }
  // The shoulder's constants. The game's encode exponent is Gamma.x of its pixel bank.
  float gamma = f.ps_bank ? LoadBeFloat(f.ps_bank + kGammaRegister * 16) : 0.0f;
  if (!(gamma > 0.2f && gamma < 1.0f)) {
    gamma = 1.0f / 2.2f;
  }
  const float peak = PeakInGameUnits(gamma);
  constants_.knee = kKnee;
  constants_.highlights = float(REXCVAR_GET(mcla_gpu_hdr_highlights));
  constants_.peak = peak;
  constants_.reserved = 0.0f;
  candidate_valid_ = true;
  candidate_target_ = f.color0;
  candidate_guest_width_ = br_x;
  candidate_guest_height_ = br_y;
  candidate_host_width_ = host_w;
  candidate_host_height_ = host_h;
  if (!recognised_logged_) {
    recognised_logged_ = true;
    REXLOG_INFO("[gpu] hdr scene: the game's tone map recognised (PS {:016X}, VS {:016X}), {}, "
                "{}x{} guest pixels ({}x{} host); the display's peak is {:.2f} in the game's "
                "units (peak {:.0f} nits over paper white {:.0f}, gamma {:.4f}, {} display "
                "table), knee {}, highlights {}",
                f.ps->key, f.vs->key, DescribeEdramKey(f.color0), br_x, br_y, host_w, host_h, peak,
                levels_.peak, levels_.white, gamma, have_ramp_ ? "the game's" : "no",
                kKnee, constants_.highlights);
  }
  return Action::kToneMap;
}

void HdrToneMap::NoteAfter(const DrawFacts& f, bool reproducible) {
  // What the game draws between the tone map and the marker: one line per kind of pixel shader and
  // blend, the first 16 (the census HDR-PLAN.md asks for, with what is done about each).
  const PipelineKey& key = *f.key;
  const uint64_t id = (f.ps ? f.ps->key : 0) ^ (uint64_t(key.blend[0]) * 0x9E3779B97F4A7C15ull) ^
                      uint64_t(reproducible);
  if (after_logged_.size() >= 16 || !after_logged_.insert(id).second) {
    return;
  }
  REXLOG_INFO("[gpu] hdr scene: a draw after the tone map (PS {:016X}, VS {:016X}): blend {:#010x} "
              "({}), {} colour output(s), {} sample(s), depth {:#x} stencil {:#x}: {}",
              f.ps ? f.ps->key : 0, f.vs ? f.vs->key : 0, key.blend[0],
              AddsLight(key.blend[0]) ? "adds light" : (UnpackBlend(key.blend[0]).blendEnabled
                                                             ? "blended, not additive"
                                                             : "blending off"),
              key.color_count, key.sample_count, key.depth, key.stencil,
              reproducible ? "drawn again"
                           : (AddsLight(key.blend[0]) ? "left out of the float scene"
                                                      : "ends the float scene"));
}

void HdrToneMap::VariantKey(Action action, const PipelineKey& key, PipelineKey* out) const {
  *out = key;
  out->color_count = 1;
  out->color_format[0] = uint32_t(kSceneFormat);
  for (uint32_t i = 1; i < kColorTargetCount; ++i) {
    out->color_format[i] = 0;
    out->blend[i] = 0;
  }
  out->depth_format = 0;
  out->depth = 0;
  out->stencil = 0;
  out->stencil_masks = 0;
  out->depth_bias = 0;
  out->slope_scaled_depth_bias = 0.0f;
  out->sample_count = 1;
  if (action == Action::kToneMap) {
    out->ps_variant = ShaderStore::kHdrVariant;
  }
}

HostImage* HdrToneMap::Image(Action action) {
  if (action == Action::kToneMap) {
    if (!candidate_valid_) {
      return nullptr;
    }
    const plume::RenderTexture* before = scene_.texture.get();
    if (!passes_->EnsureImage(scene_, candidate_host_width_, candidate_host_height_, kSceneFormat,
                              "hdr scene")) {
      Fail(Cause::kImage, std::format("{}x{}", candidate_host_width_, candidate_host_height_));
      return nullptr;
    }
    if (scene_.texture.get() != before) {
      ++epoch_;  // another image: the frames that named the old one lose their scene
    }
    return &scene_;
  }
  return phase_ == Phase::kPending && scene_.texture ? &scene_ : nullptr;
}

void HdrToneMap::Duplicated(Action action) {
  if (action == Action::kToneMap) {
    // A float scene starts (again) with this draw.
    phase_ = Phase::kPending;
    target_ = candidate_target_;
    guest_width_ = candidate_guest_width_;
    guest_height_ = candidate_guest_height_;
    host_width_ = candidate_host_width_;
    host_height_ = candidate_host_height_;
    ++epoch_;
    ++stats_.tone_maps;
  } else {
    ++stats_.after;
  }
}

void HdrToneMap::Failed(Action action, Cause cause, bool waiting, bool adds_light) {
  if (action == Action::kToneMap) {
    // Nothing started. A pipeline still being made is the usual first frames after a start: counted,
    // not logged. A scene under way on the target is the game's again.
    if (phase_ == Phase::kPending) {
      phase_ = Phase::kFailed;
    }
    if (waiting) {
      ++stats_.waiting;
    } else {
      Fail(cause, std::string());
    }
    return;
  }
  if (phase_ != Phase::kPending) {
    return;
  }
  if (adds_light) {
    // The glow is missing from the float scene; the rest of it stands.
    ++stats_.skipped_glows;
    if (!glow_logged_) {
      glow_logged_ = true;
      REXLOG_INFO("[gpu] hdr scene: a draw after the tone map that only adds light could not be drawn "
                  "again ({}); it is missing from the float scene (counted, logged once)",
                  CauseName(cause));
    }
    return;
  }
  if (waiting) {
    // Its pipeline is being made: the frame has no float scene, the next ones will.
    ++stats_.waiting;
    phase_ = Phase::kFailed;
    return;
  }
  Fail(cause, std::string());
}

void HdrToneMap::OnColorClear(const EdramKey& key) {
  if (phase_ == Phase::kPending && key == target_) {
    Fail(Cause::kClear, std::string());
  }
}

void HdrToneMap::OnComposite(plume::RenderCommandList* list, const EdramKey& target,
                             uint32_t width, uint32_t height) {
  if (phase_ != Phase::kPending) {
    return;
  }
  if (!(target == target_) || width != host_width_ || height != host_height_ ||
      !scene_.texture) {
    Fail(Cause::kMarker,
         std::format("marker on {} at {}x{}, tone map on {} at {}x{}", DescribeEdramKey(target),
                     width, height, DescribeEdramKey(target_), host_width_, host_height_));
    return;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(scene_.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  scene_.layout = plume::RenderTextureLayout::SHADER_READ;
  phase_ = Phase::kReady;
  ready_this_frame_ = true;
  ++stats_.ready;
}

const HostImage* HdrToneMap::FinishedScene(uint32_t width, uint32_t height, uint32_t* epoch) {
  if (phase_ != Phase::kReady || !scene_.texture || scene_.width != width ||
      scene_.height != height) {
    return nullptr;
  }
  *epoch = epoch_;
  ++stats_.captured;
  return &scene_;
}

void HdrToneMap::OnGuestPresent() {
  if (ready_this_frame_) {
    idle_frames_ = 0;
  } else if (scene_.texture && ++idle_frames_ == kReleaseAfterFrames) {
    passes_->ReleaseImage(scene_);
    ++epoch_;
  }
}

// ---------------------------------------------------------------------------
// reports
// ---------------------------------------------------------------------------

void HdrToneMap::Fail(Cause cause, const std::string& detail) {
  phase_ = Phase::kFailed;
  ++stats_.failed[size_t(cause)];
  if (cause_logged_[size_t(cause)]) {
    return;
  }
  cause_logged_[size_t(cause)] = true;
  REXLOG_INFO("[gpu] hdr scene: a frame keeps the scene at paper white: {}{}{} (counted; logged once "
              "per reason)",
              CauseName(cause), detail.empty() ? "" : ": ", detail);
}

uint64_t HdrToneMap::HeldBytes() const {
  return scene_.texture ? TextureBytes(scene_.width, scene_.height, 1, scene_.format) : 0;
}

std::string HdrToneMap::TakeSummary() {
  std::string failures;
  for (size_t i = 0; i < stats_.failed.size(); ++i) {
    if (stats_.failed[i] != 0) {
      failures += std::format("{}{} {}", failures.empty() ? "" : ", ", CauseName(Cause(i)),
                              stats_.failed[i]);
    }
  }
  std::string text = std::format(
      "hdr tone map: {} tone maps drawn again, {} later draws, {} glows left out, {} frames reached "
      "the marker ({} taken by the layers), {} waited for a pipeline | no float scene: {} | "
      "{:.1f} MiB",
      stats_.tone_maps, stats_.after, stats_.skipped_glows, stats_.ready, stats_.captured,
      stats_.waiting, failures.empty() ? "none" : failures,
      double(HeldBytes()) / double(1u << 20));
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
