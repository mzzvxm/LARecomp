// MCLA GPU renderer -- screen-space ambient occlusion. See ao.h.

#include "ao.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_ao, "off", "MCLA/GPU",
    "Ambient occlusion: darkens creases, kerbs, the ground under cars and where objects meet, "
    "from the picture's depth, only on the sky light and not on the sun's (the game's own "
    "constants say how much of a surface's light is which). off, low (half resolution, 2 slices) "
    "or high (full resolution, 3 slices). Costs graphics card time, high more than low. Also drawn "
    "behind the pause menu and on the map, so a change made there shows at once. Live.")
    .allowed({"off", "low", "high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ao_intensity, "default", "MCLA/GPU",
    "How strong ambient occlusion looks: light, default, strong or max. Each is a set of four "
    "numbers: how strongly it darkens, how far it looks for things that block the sky light, how "
    "hard the measure is pushed, and how much it also darkens the part of a surface the sun lights; "
    "the stronger ones reach farther and make every crease, corner and under-car shadow darker. "
    "Only used while mcla_gpu_ao is not off (the pause menu's AO INTENSITY row). Live.")
    .allowed({"light", "default", "strong", "max"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_fade_end, 160.0, "MCLA/GPU",
    "The distance, in the game's world units (about metres), where ambient occlusion has faded "
    "out entirely; it starts to fade at half of it. Fog, rain and particles are not darkened "
    "either way: they are drawn over the picture afterwards. Live.")
    .range(20.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the ambient occlusion as a grey picture instead of applying it "
    "(only while it is on): 1 the factor the picture is multiplied by (white: none), 2 the "
    "visibility, 3 the share of the light that is ambient, 4 the surface normals, 5 the distance "
    "(a log scale), 6 how much of the last frames each pixel kept (white: all it can, black: none, "
    "so it is new, a vehicle, or has no history yet; the history itself goes on as usual). 0: "
    "off. Live.")
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_history, 12, "MCLA/GPU",
    "Diagnostic: how many frames ambient occlusion averages to take its sampling noise out, 1 to "
    "64. Each frame's occlusion is found again in the next by the camera's motion and kept only "
    "where it is the same surface; more frames is smoother and slower to follow a moving shadow "
    "(a car's), fewer is noisier. 1: no averaging (two blur passes on every frame instead). Live.")
    .range(1, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_census, 0, "MCLA/GPU",
    "Diagnostic: every Nth guest frame (and the first three frames with the main view), one block "
    "of log lines with the main view's draws around the game's mid-scene depth resolve, that "
    "resolve (where it is, how big, inside EndTiling or not), the light constants the occlusion "
    "uses (PS c26 ambient, PS c39 sun colour, VS c16 sun direction), the depth clear value, the "
    "lens the game's motion blur draw gives, what the history did and a statistic of the linear "
    "depth (read back from the graphics card, so the first three frames and every Nth one run the "
    "depth passes even with the occlusion off). Frames where the main view does not have exactly "
    "one qualifying resolve are logged as they come (the first 40). 0: off. Live.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ao census]";
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                              uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);

// The registers the occlusion reads: PS c26 gLightAmbient, PS c39 gLightColor, VS c16 gLightPosDir
// (the direction the sunlight travels: at noon y is -0.867).
constexpr uint32_t kPsAmbient = 26;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsSunDirection = 16;

// The settings the passes use that no cvar sets: the effect's falloff range as a share of the
// radius, the floor of the final visibility (no black blotches). The power of the visibility is
// part of the look (mcla_gpu_ao_intensity).
constexpr float kFalloffRatio = 0.615f;
constexpr float kMinVisibility = 0.1f;
// The frames the occlusion has to stay off before its images are given back.
constexpr uint32_t kReleaseAfterFrames = 60;
// Sizes below this (host pixels, either way) are not worth the passes.
constexpr uint32_t kMinRenderSize = 64;
// The history's flag in the push block's flags (hlsl/ao_temporal_ps.hlsl AoHistoryOn).
constexpr uint32_t kHistoryFlag = 1u << 20;
// The vertex constants' view inverse is taken as the frame's own camera once it matched the
// anchor's within kVsMatch in a frame whose camera moved by at least kVsMoved (an entry of the
// view inverse: a step of that size shows in it; a stale copy of the last frame's would not match).
constexpr double kVsMatch = 5e-4;
constexpr double kVsMoved = 5e-3;
// The sky and anything the linearisation could not place (hlsl/ao_common.hlsli kAoSky).
constexpr float kSkyDistance = 60000.0f;

// The looks of mcla_gpu_ao_intensity (the pause menu's AO INTENSITY row): the darkening's strength,
// the radius in the game's world units, the power of the visibility and the direct share (how much
// of the sun-lit part is darkened too). "default" is the setting's default.
struct LookDef {
  const char* name;
  float strength;
  float radius;
  float power;
  float direct;
};
constexpr LookDef kLooks[] = {
    {"light", 1.0f, 2.5f, 3.2f, 0.25f},
    {"default", 1.0f, 3.0f, 3.6f, 0.375f},
    {"strong", 1.0f, 3.5f, 4.0f, 0.5f},
    {"max", 1.0f, 3.75f, 4.5f, 0.75f},
};
constexpr uint32_t kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);
constexpr uint32_t kDefaultLook = 1;

// The index of mcla_gpu_ao_intensity's look; anything the list does not know is the default.
uint32_t ReadLookIndex() {
  std::string value = REXCVAR_GET(mcla_gpu_ao_intensity);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (uint32_t i = 0; i < kLookCount; ++i) {
    if (value == kLooks[i].name) {
      return i;
    }
  }
  return kDefaultLook;
}

// A matrix that can be a camera's view inverse: finite, the last column (0, 0, 0, 1), unit axes.
bool PoseSane(const temporal::Mat4& w) {
  if (!temporal::Finite(w)) {
    return false;
  }
  if (std::fabs(w.m[0][3]) > 1e-3 || std::fabs(w.m[1][3]) > 1e-3 || std::fabs(w.m[2][3]) > 1e-3 ||
      std::fabs(w.m[3][3] - 1.0) > 1e-3) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] +
                                    w.m[r][2] * w.m[r][2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  return true;
}

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

double Length3(const float v[3]) {
  return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}

std::string Vec3(const float v[3]) {
  return std::format("({:.4g}, {:.4g}, {:.4g})", v[0], v[1], v[2]);
}

std::string RefusalNames(uint32_t refused) {
  static const char* const kNames[] = {
      "not a depth copy",
      "not the main view's depth",
      "inside EndTiling",
      "not the first qualifying one of the frame",
      "after the anchor or the composite marker",
      "not into 078B0000",
      "the same call clears the depth",
      "the rectangle does not start at the origin or leaves the picture",
      "another tile of the picture the trigger covered whole",
      "the destination is smaller than the main target (a 4x view's tile texture)"};
  std::string out;
  for (uint32_t i = 0; i < 10; ++i) {
    if (refused & (1u << i)) {
      out += std::format("{}{}", out.empty() ? "" : ", ", kNames[i]);
    }
  }
  return out;
}

const char* ModeName(AmbientOcclusion::Mode mode) {
  switch (mode) {
    case AmbientOcclusion::Mode::kOff: return "off";
    case AmbientOcclusion::Mode::kLow: return "low";
    case AmbientOcclusion::Mode::kHigh: return "high";
  }
  return "?";
}

AmbientOcclusion::Mode ReadMode() {
  std::string value = REXCVAR_GET(mcla_gpu_ao);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (value == "low") {
    return AmbientOcclusion::Mode::kLow;
  }
  if (value == "high") {
    return AmbientOcclusion::Mode::kHigh;
  }
  return AmbientOcclusion::Mode::kOff;
}

// A binary16 as a float.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

// A light constant that is a usable amount (a draw whose shaders do not use the register can leave
// anything there).
float Usable(float x) {
  return std::isfinite(x) && x > 0.0f ? x : 0.0f;
}

float Luma(const float c[3]) {
  return 0.2126f * Usable(c[0]) + 0.7152f * Usable(c[1]) + 0.0722f * Usable(c[2]);
}

}  // namespace

bool AmbientOcclusion::LightRaw::operator==(const LightRaw& o) const {
  return std::memcmp(dwords, o.dwords, sizeof(dwords)) == 0;
}

void AmbientOcclusion::DecodeLight(const LightRaw& raw, float ambient[3], float sun[3],
                                   float dir[3]) {
  for (int i = 0; i < 3; ++i) {
    ambient[i] = std::bit_cast<float>(raw.dwords[i]);
    sun[i] = std::bit_cast<float>(raw.dwords[3 + i]);
    dir[i] = std::bit_cast<float>(raw.dwords[6 + i]);
  }
}

AmbientOcclusion::~AmbientOcclusion() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::ApplyVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVApplyBase + (debug ? 4u : 0u) + log2_samples;
}

std::string AmbientOcclusion::VariantName(uint32_t variant) {
  switch (variant) {
    case kVDepth: return "ao depth";
    case kVDepthMs: return "ao depth (multisampled)";
    case kVMip: return "ao depth level";
    case kVMain: return "ao visibility";
    case kVDenoise: return "ao denoise";
    case kVTemporal: return "ao temporal";
    case kVTemporalMs: return "ao temporal (multisampled)";
    default: break;
  }
  const uint32_t index = variant - kVApplyBase;
  return std::format("ao {} {}x", (index & 4) ? "view" : "apply", 1u << (index & 3));
}

void AmbientOcclusion::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                  HostPasses& passes, EdramTargets& edram,
                                  PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  // The host pass layout (host_passes.h) with a push block of 128 bytes instead of 48: the four
  // texture sets, the sampler set, the push range at (b0, space4), for both stages as there.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addPushConstant(0, 4, sizeof(Push),
                          plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "ao layout");
  bool ok = layout_ != nullptr;
  for (uint32_t i = 0; i < kPassCount && ok; ++i) {
    const HostShaderBlob blob = AmbientOcclusionPixelShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] ao: the layout or a shader could not be made: no ambient occlusion this "
                 "session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    layout_.reset();
    return;
  }
  ready_ = true;
  ListPrecache();
}

void AmbientOcclusion::Shutdown() {
  // The host builder's functions name this object's layout and shaders: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  ++generation_;
  probe_buffer_.reset();
  probe_busy_ = false;
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  raw_ = HostImage{};
  denoised_ = HostImage{};
  for (HostImage& image : history_) {
    image = HostImage{};
  }
  weight_ = HostImage{};
  history_valid_ = false;
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  layout_.reset();
  ready_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

void AmbientOcclusion::ListPrecache() {
  HostPrecache& precache = passes_->precache();
  // The apply pass for the play view's colour target (1x, 2x) and the pause/map view's (4x), so
  // that opening the pause menu with the occlusion on waits for no pipeline.
  std::vector<uint32_t> listed = {kVDepth,           kVDepthMs,          kVMip,
                                  kVMain,            kVDenoise,          kVTemporal,
                                  kVTemporalMs,      ApplyVariant(false, 1), ApplyVariant(false, 2),
                                  ApplyVariant(false, 4)};
  // The host's MSAA level changes while the game runs (msaa_level.h) and gives the main view's
  // colour target the new level's samples: the apply pass for 8 samples is listed whatever the
  // level is at the start, so a switch to 8x finds it made instead of making it on the render
  // thread in the first frame at that level (the guest's own pipelines are made ahead for the new
  // level by Pipelines::PrebuildForLevel; this pass is not one of them). 2x and 4x are listed above
  // (the pause and map view's colour target has the game view's samples, or 1 without MSAA). A level
  // the device cannot do is not listed.
  if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
    listed.push_back(ApplyVariant(false, 8));
  }
  for (const uint32_t variant : listed) {
    precache.Add(HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant),
                 VariantName(variant), [this, variant] { return MakePipeline(variant); });
  }
}

std::unique_ptr<plume::RenderPipeline> AmbientOcclusion::MakePipeline(uint32_t variant) {
  const std::string name = VariantName(variant);
  const plume::RenderBlendDesc copy = plume::RenderBlendDesc::Copy();
  const auto make = [&](Pass pass, plume::RenderFormat format, const plume::RenderBlendDesc& blend,
                        uint32_t samples) {
    return passes_->CreatePipeline(*layout_, *shaders_[pass], format, blend, samples, name.c_str());
  };
  switch (variant) {
    case kVDepth: return make(kPassDepth, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVDepthMs: return make(kPassDepthMs, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVMip: return make(kPassMip, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMain: return make(kPassMain, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVDenoise: return make(kPassDenoise, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVTemporal: return make(kPassTemporal, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    case kVTemporalMs: return make(kPassTemporalMs, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    default: break;
  }
  // The apply pass: into the scene colour (float16 RGBA) with the target's sample count. The
  // multiply is dst.rgb * src.rgb (the shader writes the factor); a debug view replaces the rgb.
  // Alpha is never written.
  const uint32_t index = variant - kVApplyBase;
  const bool debug = (index & 4) != 0;
  const uint32_t samples = 1u << (index & 3);
  plume::RenderBlendDesc blend = copy;
  if (!debug) {
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::ZERO;
    blend.dstBlend = plume::RenderBlend::SRC_COLOR;
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  }
  blend.renderTargetWriteMask = kWriteRgb;
  return make(samples > 1 ? kPassApplyMs : kPassApply,
              plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples);
}

plume::RenderPipeline* AmbientOcclusion::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  // Made ahead when listed; a variant nobody listed (a debug view, a 4x target) is made here.
  pipelines_[variant] = passes_->precache().Acquire(
      HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant), VariantName(variant),
      [&] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("[gpu] ao: the pipeline '{}' could not be made: no ambient occlusion this "
                 "session in that configuration",
                 VariantName(variant));
  }
  return pipelines_[variant].get();
}

void AmbientOcclusion::BeginFrame() {
  census_every_ = uint32_t(std::max(0, REXCVAR_GET(mcla_gpu_ao_census)));
  Mode mode = ReadMode();
  if (!ready_ && mode != Mode::kOff) {
    mode = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(mode) != last_mode_logged_) {
    last_mode_logged_ = uint32_t(mode);
    REXLOG_INFO("[gpu] ao: ambient occlusion {} (mcla_gpu_ao, live)", ModeName(mode));
  }
  mode_ = mode;
}

uint64_t AmbientOcclusion::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : depth_) {
    add(image);
  }
  add(raw_);
  add(denoised_);
  for (const HostImage& image : history_) {
    add(image);
  }
  add(weight_);
  return bytes;
}

void AmbientOcclusion::ReleaseImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(raw_);
  passes_->ReleaseImage(denoised_);
  for (HostImage& image : history_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(weight_);
  history_valid_ = false;
  history_cut_ = false;
  ++generation_;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks) {
  frame_had_main_view_ = true;
  if (draw.match.samples == temporal::kPauseSamples) {
    ++view4x_draws_;  // the pause menu's, the race pause's and the map's world
    pause_view_ = true;
  } else {
    play_view_ = true;
  }
  if (anchor_seen_ || composite_seen_) {
    ++draws_at_anchor_;
    return;
  }
  if (qualifying_resolves_ != 0) {
    ++draws_after_;
    if (draw.match.depth && draw.writes_depth) {
      ++depth_writes_after_;
    }
    return;
  }
  ++draws_before_;
  if (!draw.match.color) {
    return;
  }
  ++color_draws_before_;
  // The registers the banks of this draw hold (a draw whose shaders do not use them leaves what an
  // earlier one set: the value most colour draws carry is the frame's).
  LightRaw light;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  for (uint32_t i = 0; i < 3; ++i) {
    light.dwords[i] = LoadBe32(ps + kPsAmbient * 16 + i * 4);
    light.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
    light.dwords[6 + i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
  }
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      vs_view_inverse_[r * 4 + c] = BankFloat(vs, 12 + r, c);
    }
  }
  ++vs_view_inverse_draws_;
  if (light_draws_ == 0) {
    first_light_ = light;
  }
  last_light_ = light;
  ++light_draws_;
  for (uint32_t i = 0; i < light_count_; ++i) {
    if (lights_[i].raw == light) {
      ++lights_[i].draws;
      return;
    }
  }
  if (light_count_ < kLightSlots) {
    lights_[light_count_].raw = light;
    lights_[light_count_].draws = 1;
    ++light_count_;
  } else {
    ++light_overflow_;
  }
}

bool AmbientOcclusion::PickLight(LightRaw* out, uint32_t* draws_with_it) const {
  if (light_draws_ == 0) {
    return false;
  }
  if (light_count_ == 0) {
    *out = last_light_;
    *draws_with_it = 0;
    return true;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < light_count_; ++i) {
    if (lights_[i].draws >= lights_[best].draws) {
      best = i;
    }
  }
  *out = lights_[best].raw;
  *draws_with_it = lights_[best].draws;
  return true;
}

void AmbientOcclusion::OnDepthClear(const EdramKey& key, float z) {
  if (temporal::MainDepthSamples(key) == 0) {
    return;
  }
  ++depth_clears_;
  depth_clear_z_ = z;
}

void AmbientOcclusion::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  have_camera_ = false;
  lens_step_valid_ = false;
  vs_compare_valid_ = false;
  // The play view's camera and the pause/map world's (4x samples) are kept apart: the lens differs
  // (a field of view of 25 to 54 degrees), and the anchor draw runs in the 4x frames too with a
  // valid camera (918 of 918 frames in a recorded pause and map session). A frame with draws of
  // both kinds or of neither (the anchor without a main view) gives no camera; the frame after it
  // has none to use.
  if (play_view_ == pause_view_) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself here: only the temporal
  // system knows the anchor's fetches, and a frame size that is not the main view's is caught by
  // the rectangle of the trigger (ao.cpp Judge).
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    reject_ = temporal::Reject::kFrameSize;
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  temporal::Camera camera;
  reject_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_,
                                    &camera);
  if (reject_ != temporal::Reject::kNone) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    if (rejects_logged_ < 8) {
      ++rejects_logged_;
      REXLOG_INFO("[gpu] ao: the anchor's constants gave no camera: {} (frame {}, logged {} of 8)",
                  temporal::RejectName(reject_), frame_, rejects_logged_);
    }
    return;
  }
  // The lens the previous frame's camera gave the trigger, against this frame's own (the trigger
  // works with the previous frame's lens, which is this frame's outside cuts).
  lens_delta_valid_ = have_previous_ && previous_frame_ + 1 == frame_ && previous_pause_ == pause;
  if (lens_delta_valid_) {
    const auto rel = [](double a, double b) {
      return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
    };
    lens_delta_[0] = rel(camera.fov_y, previous_.fov_y);
    lens_delta_[1] = rel(camera.aspect, previous_.aspect);
    lens_delta_[2] = rel(camera.near_plane, previous_.near_plane);
    lens_delta_[3] = rel(camera.far_plane, previous_.far_plane);
    double dx = 0.0;
    double dot = 0.0;
    for (int i = 0; i < 3; ++i) {
      const double d = camera.position[i] - previous_.position[i];
      dx += d * d;
      dot += camera.forward[i] * previous_.forward[i];
    }
    lens_delta_[4] = std::sqrt(dx);
    lens_delta_[5] = dot;
    // The next frame's trigger uses this camera's lens: how fast the lens was moving is the best
    // guess of how far it is from that frame's own (a pause/map view only: see OnResolve).
    lens_step_ = std::max({lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3]});
    lens_step_valid_ = std::isfinite(lens_step_);
  }
  // The camera's motion from the frame before to this one, for the occlusion's history in the next
  // frame (FrameMotion): that frame is guessed to move the same way. A cut starts the history
  // again, and the step of a cut is not a motion to repeat.
  if (lens_delta_valid_) {
    const temporal::Cut cut =
        temporal::TestCut(camera, previous_, have_last_step_ ? last_step_ : nullptr);
    if (cut.fires) {
      history_valid_ = false;
      history_cut_ = true;
      have_last_step_ = false;
      last_motion_valid_ = false;
    } else {
      last_motion_ = temporal::Mul(previous_.view_inverse, camera.view);
      last_motion_valid_ = temporal::Finite(last_motion_);
      last_motion_frame_ = frame_;
      for (int i = 0; i < 3; ++i) {
        last_step_[i] = cut.step[i];
      }
      have_last_step_ = true;
    }
    // Whether the vertex constants of this frame's draws held this frame's view inverse (what the
    // census compares below): trusted once they did in a frame the camera moved in, dropped as
    // soon as they differ.
    if (vs_view_inverse_draws_ != 0) {
      temporal::Mat4 vertex;
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          vertex.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
        }
      }
      const double differs = temporal::MaxAbsDiff(vertex, camera.view_inverse);
      const double moved = temporal::MaxAbsDiff(camera.view_inverse, previous_.view_inverse);
      if (!(differs <= kVsMatch)) {
        vs_pose_trusted_ = false;
      } else if (moved >= kVsMoved) {
        vs_pose_trusted_ = true;
      }
    }
  } else {
    have_last_step_ = false;
    last_motion_valid_ = false;
    vs_pose_trusted_ = false;
  }
  camera_ = camera;
  have_camera_ = true;
  previous_ = camera;
  previous_pause_ = pause;
  have_previous_ = true;
  previous_frame_ = frame_;

  // The main view's vertex bank against the camera just read, for the census. If c12-c15 of a
  // vertex bank are the view inverse, the frame's own lens exists before the anchor.
  if (vs_view_inverse_draws_ != 0) {
    double worst = 0.0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const double d =
            std::fabs(double(vs_view_inverse_[r * 4 + c]) - camera.view_inverse.m[r][c]);
        worst = std::isfinite(d) ? std::max(worst, d) : HUGE_VAL;
      }
    }
    vs_compare_valid_ = true;
    vs_compare_diff_ = worst;
    ++vs_compared_[pause ? 1 : 0];
    if (worst <= 1e-4) {
      ++vs_equal_[pause ? 1 : 0];
    }
  }
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::Judge(const ResolveRecord& record,
                                 const ResolveCache::Summary& summary) const {
  if (!summary.depth_copy) {
    return kRefusedNotDepthCopy;
  }
  uint32_t refused = 0;
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (samples == 0) {
    refused |= kRefusedNotMainDepth;
  }
  const bool pause = samples == temporal::kPauseSamples;
  if (record.flags & kEventInsideEndTiling) {
    refused |= kRefusedEndTiling;
  }
  // The game resolves the picture in tiles. The first is the trigger, which covers the whole
  // picture (the frame is drawn once: the depth target is complete); the others of the same
  // picture are not triggers, whatever their rectangle.
  const bool later_tile = IsLaterTile(summary);
  if (later_tile) {
    refused |= kRefusedLaterTile;
  } else if (qualifying_resolves_ != 0) {
    refused |= kRefusedNotFirst;
  }
  if (anchor_seen_ || composite_seen_) {
    refused |= kRefusedAfterAnchor;
  }
  if (summary.dest_address != kTriggerDestination) {
    refused |= kRefusedDestination;
  }
  if (summary.clears_depth) {
    refused |= kRefusedClearsDepth;
  }
  if (!later_tile) {
    uint32_t width = 0;
    uint32_t height = 0;
    PictureSize(summary, &width, &height);
    // The play view's first tile starts at the origin (the rows 0-512 of two). The pause/map
    // view's three tiles come in an order nobody has looked at: any rectangle inside the picture
    // will do for it, the occlusion covers the whole picture whichever tile triggers.
    const bool origin = summary.x0 == 0 && summary.y0 == 0;
    if ((!pause && !origin) || summary.x0 < 0 || summary.y0 < 0 || summary.x1 <= summary.x0 ||
        summary.y1 <= summary.y0 || uint32_t(summary.x1) > width || uint32_t(summary.y1) > height) {
      refused |= kRefusedRectangle;
    }
    // The pause/map view's destination must be the whole picture (1280x720 within the 1280x736
    // target), not a tile's own texture: the passes cover what the destination's size says.
    // (mcla_gpu_ao_census prints every depth copy.)
    if (pause && summary.source &&
        (width != summary.source->width ||
         height + EdramTargets::kHeightAlignment <= summary.source->height)) {
      refused |= kRefusedNotWholePicture;
    }
  }
  return refused;
}

void AmbientOcclusion::PictureSize(const ResolveCache::Summary& summary, uint32_t* width,
                                   uint32_t* height) {
  *width = summary.dest_width;
  *height = summary.dest_height;
  if (summary.source) {
    *width = std::min(*width, summary.source->width);
    *height = std::min(*height, summary.source->height);
  }
}

bool AmbientOcclusion::IsLaterTile(const ResolveCache::Summary& summary) const {
  // The same picture (source, destination texture) as the frame's first qualifying resolve, and a
  // rectangle of its own: the same rectangle again is a second copy, not a tile.
  return qualifying_resolves_ != 0 && summary.source_key == trigger_source_ &&
         summary.dest_address == trigger_destination_ && summary.dest_format == trigger_format_ &&
         summary.dest_width == trigger_size_[0] && summary.dest_height == trigger_size_[1] &&
         (summary.x0 != trigger_rect_[0] || summary.y0 != trigger_rect_[1] ||
          summary.x1 != trigger_rect_[2] || summary.y1 != trigger_rect_[3]);
}

bool AmbientOcclusion::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                                 plume::RenderCommandList* list, bool upscaler_running) {
  if (!summary.depth_copy) {
    ++other_resolves_;
    return false;
  }
  const uint32_t refused = Judge(record, summary);
  const bool qualifies = (refused & ~uint32_t(kRefusedNotFirst)) == 0;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  if (census_every_ != 0 && depth_resolves_.size() < 16) {
    DepthResolveNote note;
    note.caller = record.caller;
    note.flags = record.flags;
    note.source = summary.source_key;
    note.dest_address = summary.dest_address;
    note.dest_format = summary.dest_format;
    note.dest_width = summary.dest_width;
    note.dest_height = summary.dest_height;
    note.rect[0] = summary.x0;
    note.rect[1] = summary.y0;
    note.rect[2] = summary.x1;
    note.rect[3] = summary.y1;
    note.host_sample = summary.host_sample;
    note.host_samples = summary.source ? summary.source->host_samples : 0;
    note.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
    note.clears_depth = summary.clears_depth;
    note.refused = refused;
    note.draws_before = draws_before_ + draws_after_;
    note.anchor_before = anchor_seen_;
    depth_resolves_.push_back(note);
  }
  // A pause/map view's copy of the main depth that fails only on its destination: a build that
  // copies it elsewhere has no occlusion in that view, and the log says so once.
  if (pause && qualifying_resolves_ == 0 && refused == kRefusedDestination &&
      !pause_destination_logged_) {
    pause_destination_logged_ = true;
    REXLOG_INFO("[gpu] ao: a pause/map frame copies its main depth into {:08x} (format {}, {}x{}), "
                "not into {:08x} where the play view's mid-scene copy goes: no occlusion in that "
                "view (reported once; mcla_gpu_ao_census prints every copy)",
                summary.dest_address, summary.dest_format, summary.dest_width, summary.dest_height,
                kTriggerDestination);
  }
  if (qualifies) {
    if (qualifying_resolves_ == 0) {
      trigger_source_ = summary.source_key;
      trigger_destination_ = summary.dest_address;
      trigger_format_ = summary.dest_format;
      trigger_size_[0] = summary.dest_width;
      trigger_size_[1] = summary.dest_height;
      trigger_rect_[0] = summary.x0;
      trigger_rect_[1] = summary.y0;
      trigger_rect_[2] = summary.x1;
      trigger_rect_[3] = summary.y1;
    }
    ++qualifying_resolves_;
  } else if (refused & kRefusedLaterTile) {
    ++later_tiles_;
  }
  if (refused != 0 || !list || !ready_ || !summary.source) {
    return false;
  }

  // The trigger. The depth chain also runs, with the occlusion off, at the census's frames: its
  // statistic shows whether the depth convention holds (1 near, 0 far).
  trigger_pause_ = pause;
  trigger_camera_valid_ = false;
  outcome_ = Outcome::kIdle;
  Plan plan;
  plan.mode = mode_;
  plan.probe = census_every_ != 0 && (probes_done_ < 3 || frame_ % census_every_ == 0);
  if (mode_ == Mode::kOff) {
    if (!plan.probe) {
      return false;
    }
    plan.mode = Mode::kLow;  // the cheaper size is enough for a statistic
    plan.apply = false;
  }
  plan.upscaler = upscaler_running;
  plan.view = mode_ == Mode::kOff ? 0u : uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_view), 0, 6));
  // The lens is the previous frame's (the camera exists at the anchor, after this), of the same
  // kind of view: none, no occlusion this frame (a cut's first frame, the first frame after
  // turning it on, the first frame of a pause spell: the play view's lens is not the pause
  // camera's).
  if (!have_previous_ || previous_frame_ + 1 != frame_ || previous_pause_ != pause) {
    ++skipped_no_lens_;
    outcome_ = Outcome::kNoCamera;
    if (pause) {
      ++pause_skipped_no_lens_;
      if (!no_lens_pause_logged_) {
        no_lens_pause_logged_ = true;
        REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame that has no previous pause/map "
                    "frame's camera (the first frame of a pause spell, a frame after one whose "
                    "anchor draw gave no camera); counted, logged once");
      }
    } else if (!no_lens_logged_) {
      no_lens_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a frame that has no previous frame's camera (the "
                  "first frame, a cut, a frame whose anchor draw gave no camera); counted, "
                  "logged once");
    }
    return false;
  }
  // A pause/map view whose lens was changing fast between the last two frames (the pause camera's
  // swing when the menu opens, the map's zoom), or whose rate of change is not known yet (the
  // second frame of a spell, the second after the setting was turned on): this frame's lens may be
  // as far from the previous one's, and a wrong lens is worse than none for a frame, so no
  // occlusion until it is known to be steady. The play view keeps its rule: its lens moves by
  // about 1e-4 a frame.
  trigger_lens_step_ = lens_step_valid_ ? lens_step_ : 0.0;
  if (pause && (!lens_step_valid_ || lens_step_ > temporal::kCutFov)) {
    ++pause_skipped_lens_;
    outcome_ = lens_step_valid_ ? Outcome::kLensChanging : Outcome::kLensUnknown;
    if (!lens_step_logged_) {
      lens_step_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame whose lens is not known to be "
                  "steady (the second frame of a spell, or a change of more than {:.0f}% between "
                  "the last two frames: the pause camera's swing, the map); counted, logged once",
                  100.0 * temporal::kCutFov);
    }
    return false;
  }
  trigger_camera_ = previous_;
  trigger_camera_valid_ = true;
  // The whole picture, not the tile this resolve copies (the game resolves it in two: the rows
  // 0-512 and 512-720), at the source's own host scale (the render scale, an upscaler's render
  // size): the target is complete now, so the occlusion covers all of it and the lens's field of
  // view spans the rows it was made for.
  uint32_t picture_width = 0;
  uint32_t picture_height = 0;
  PictureSize(summary, &picture_width, &picture_height);
  const Scale scale = summary.source->scale;
  plan.render[0] = scale.Edge(picture_width);
  plan.render[1] = scale.Edge(picture_height);
  if (plan.render[0] < kMinRenderSize || plan.render[1] < kMinRenderSize) {
    outcome_ = Outcome::kTooSmall;
    return false;
  }
  const bool low = plan.mode == Mode::kLow;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    plan.ao[axis] = low ? (plan.render[axis] + 1) / 2 : plan.render[axis];
    plan.level[0][axis] = (plan.ao[axis] + 1) / 2;
    plan.level[1][axis] = (plan.ao[axis] + 3) / 4;
  }
  const bool recorded = Run(summary, list, plan);
  outcome_ = recorded ? Outcome::kRecorded : Outcome::kNotRecorded;
  return recorded;
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

bool AmbientOcclusion::EnsureImages(const Plan& plan) {
  bool ok =
      passes_->EnsureImage(depth_[0], plan.ao[0], plan.ao[1], plume::RenderFormat::R32_FLOAT,
                           "ao depth") &&
      passes_->EnsureImage(depth_[1], plan.level[0][0], plan.level[0][1],
                           plume::RenderFormat::R16_FLOAT, "ao depth 1/2") &&
      passes_->EnsureImage(depth_[2], plan.level[1][0], plan.level[1][1],
                           plume::RenderFormat::R16_FLOAT, "ao depth 1/4");
  if (ok && plan.apply) {
    ok = passes_->EnsureImage(raw_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao visibility") &&
         passes_->EnsureImage(denoised_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao denoised visibility") &&
         passes_->EnsureImage(history_[0], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 0") &&
         passes_->EnsureImage(history_[1], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 1");
  }
  // The picture of debug view 6, made only while that view is shown.
  if (ok && plan.apply && plan.view == 6u) {
    ok = passes_->EnsureImage(weight_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16G16_FLOAT,
                              "ao history weight");
  } else if (weight_.texture) {
    passes_->ReleaseImage(weight_);
  }
  if (ok && (images_made_for_[0] != plan.ao[0] || images_made_for_[1] != plan.ao[1])) {
    images_made_for_[0] = plan.ao[0];
    images_made_for_[1] = plan.ao[1];
    REXLOG_INFO("[gpu] ao: images at {}x{} for a {}x{} picture ({}): linear depth R32_FLOAT with "
                "two coarser levels, visibility R16_FLOAT x2, history R16G16_FLOAT x2, {:.1f} MiB",
                plan.ao[0], plan.ao[1], plan.render[0], plan.render[1],
                plan.apply ? ModeName(plan.mode) : "census probe only",
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return ok;
}

void AmbientOcclusion::Bind(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

void AmbientOcclusion::DrawPass(plume::RenderCommandList* list, HostImage& image,
                                const plume::RenderPipeline* pipeline, const Push& push) {
  // The same sequence as HostPasses::DrawInto, with this pass's own layout: 128 bytes of push
  // constants for the vertex and the pixel stage on both backends (the temporal motion pass uses
  // 80, the host passes' layout 48; Vulkan guarantees 128), and the host layout's texture sets and
  // sampler set bound again by Bind.
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    // A new colour target is initialised before it is drawn into (D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

void AmbientOcclusion::UpdateLook() {
  const uint32_t index = ReadLookIndex();
  const LookDef& def = kLooks[index];
  look_.strength = def.strength;
  look_.radius = def.radius;
  look_.power = def.power;
  look_.direct = def.direct;
  if (index != look_logged_index_) {
    look_logged_index_ = index;
    REXLOG_INFO("[gpu] ao: intensity {} (mcla_gpu_ao_intensity, live): strength {:.3g}, radius {:.3g} "
                "m, power {:.3g}, direct {:.3g}",
                def.name, look_.strength, look_.radius, look_.power, look_.direct);
  }
}

void AmbientOcclusion::FillPush(Push* push, const ResolveCache::Summary& summary, const Plan& plan,
                                const LightRaw* light) const {
  std::memset(push, 0, sizeof(*push));
  push->slots[0] = summary.sample_slot;
  push->slots[1] = summary.host_sample;
  const bool low = plan.mode == Mode::kLow;
  const uint32_t slices = low ? 2 : 3;
  const uint32_t steps = low ? 2 : 3;
  // The noise changes every frame (hlsl/ao_main_ps.hlsl); the denoise is a 3x3 each time.
  const uint32_t denoise_radius = 1;
  push->slots2[1] = uint32_t(frame_ & 1023);
  push->slots2[2] = plan.view | (slices << 8) | (steps << 12) | ((low ? 1u : 0u) << 16) |
                    (denoise_radius << 17);

  // The lens: the previous frame's projection (row vectors, view space to clip, the camera looking
  // down -z): tan of the half field of view from its x and y scales, the depth rows for the
  // linearisation (t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored depth).
  const temporal::Mat4& p = previous_.projection;
  const float radius = look_.radius;
  const float fade_end = float(std::clamp(REXCVAR_GET(mcla_gpu_ao_fade_end), 20.0, 1000.0));
  push->lens[0] = float(1.0 / std::max(std::fabs(p.m[0][0]), 1e-9));
  push->lens[1] = float(1.0 / std::max(std::fabs(p.m[1][1]), 1e-9));
  push->lens[2] = radius;
  push->lens[3] = kFalloffRatio;
  push->lin[0] = float(p.m[2][2]);
  push->lin[1] = float(p.m[2][3]);
  push->lin[2] = float(p.m[3][2]);
  push->lin[3] = float(p.m[3][3]);
  push->shape[0] = look_.power;
  push->shape[1] = kMinVisibility;
  push->shape[2] = look_.strength;
  push->shape[3] = 0.5f * fade_end;
  push->extra[1] = fade_end;
  push->extra[2] = look_.direct;

  // The light: the ambient and the sun as lumas, and the direction to the sun in this renderer's
  // view space (x right, y up, z forward). The game's view space looks along -z, so z changes
  // sign. c16 is the direction the light travels (at noon y is -0.867: the sun is overhead), so the
  // direction to the sun is -c16. No light constants or no direction: no sun, the whole light is
  // ambient.
  if (light) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(*light, ambient, sun, dir);
    push->light[3] = Luma(ambient);
    push->extra[0] = Luma(sun);
    double view[3] = {};
    for (int j = 0; j < 3; ++j) {
      for (int i = 0; i < 3; ++i) {
        view[j] += double(std::isfinite(dir[i]) ? dir[i] : 0.0f) * previous_.view.m[i][j];
      }
    }
    view[2] = -view[2];
    const double length = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (std::isfinite(length) && length > 1e-6) {
      for (int i = 0; i < 3; ++i) {
        push->light[i] = float(-view[i] / length);
      }
    }
  }
}

const char* AmbientOcclusion::HistoryWhyName(HistoryWhy why) {
  switch (why) {
    case HistoryWhy::kUsed: return "used";
    case HistoryWhy::kFirst:
      return "none yet (the first frame, the occlusion just turned on)";
    case HistoryWhy::kGap: return "not the frame after the one that made it";
    case HistoryWhy::kView: return "the other kind of view (play against pause/map)";
    case HistoryWhy::kSize: return "another image size";
    case HistoryWhy::kCut: return "a cut of the camera";
    case HistoryWhy::kCap: return "mcla_gpu_ao_history is 1";
    case HistoryWhy::kCount: break;
  }
  return "?";
}

const char* AmbientOcclusion::PoseName(Pose pose) {
  switch (pose) {
    case Pose::kStill: return "standing still (no step measured)";
    case Pose::kPredicted: return "the last step repeated";
    case Pose::kVertexBank: return "the frame's own view inverse from the vertex constants";
  }
  return "?";
}

bool AmbientOcclusion::FrameMotion(temporal::Mat4* motion, Pose* source, bool* cut) const {
  *cut = false;
  // The frame's own camera, from the vertex constants of the draws before the trigger, once it is
  // known that they hold it (OnAnchor): the motion is exact then, as in any renderer with a
  // camera of its own. v_history = v_world * V_history, v_world = v_now * W_now.
  if (vs_pose_trusted_ && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 world;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        world.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
      }
    }
    if (PoseSane(world)) {
      *motion = temporal::Mul(world, previous_.view);
      *source = Pose::kVertexBank;
      temporal::Camera now;
      for (int i = 0; i < 3; ++i) {
        now.position[i] = world.m[3][i];
        now.forward[i] = -world.m[2][i];
      }
      now.fov_y = previous_.fov_y;
      *cut = temporal::TestCut(now, previous_, have_last_step_ ? last_step_ : nullptr).fires;
      return temporal::Finite(*motion);
    }
  }
  // The last step repeated: T took the view space of the frame before the previous one to the
  // previous one's, and the frame now is guessed to take the previous one's to its own, so what
  // moves a point of this frame's view space back into the previous one's is the inverse of T.
  if (last_motion_valid_ && last_motion_frame_ == previous_frame_) {
    temporal::Mat4 inverse;
    if (temporal::Inverse(last_motion_, &inverse)) {
      *motion = inverse;
      *source = Pose::kPredicted;
      return true;
    }
  }
  *motion = temporal::Identity();
  *source = Pose::kStill;
  return true;
}

AmbientOcclusion::HistoryUse AmbientOcclusion::ChooseHistory(const Plan& plan) const {
  HistoryUse use;
  use.cap = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_history), 1, 64));
  use.motion = temporal::Identity();
  if (use.cap < 2) {
    use.why = HistoryWhy::kCap;
  } else if (!history_valid_) {
    use.why = history_cut_ ? HistoryWhy::kCut : HistoryWhy::kFirst;
  } else if (history_frame_ + 1 != frame_) {
    use.why = HistoryWhy::kGap;
  } else if (history_pause_ != trigger_pause_) {
    use.why = HistoryWhy::kView;
  } else if (history_size_[0] != plan.ao[0] || history_size_[1] != plan.ao[1]) {
    use.why = HistoryWhy::kSize;
  } else {
    bool cut = false;
    if (!FrameMotion(&use.motion, &use.pose, &cut)) {
      use.why = HistoryWhy::kFirst;
      use.pose = Pose::kStill;
      use.motion = temporal::Identity();
    } else if (cut) {
      use.why = HistoryWhy::kCut;
    } else {
      use.on = true;
      use.why = HistoryWhy::kUsed;
      use.age = history_age_;
    }
  }
  return use;
}

void AmbientOcclusion::NoteHistory(const HistoryUse& use) {
  history_why_ = use.why;
  history_pose_ = use.pose;
  if (use.on) {
    ++history_used_;
    ++pose_frames_[size_t(use.pose)];
    if (!history_logged_) {
      history_logged_ = true;
      REXLOG_INFO("[gpu] ao: the first frame that averages over a history: {} frames at most "
                  "(mcla_gpu_ao_history), the camera's motion into the last frame from {}; one "
                  "denoise pass instead of two, the wide blur only where the history does not "
                  "hold (reported once)",
                  use.cap, PoseName(use.pose));
    }
  } else {
    ++history_resets_[size_t(use.why)];
  }
}

bool AmbientOcclusion::Run(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                           const Plan& plan) {
  EdramTargets::Target& depth_target = *summary.source;
  const bool ms = depth_target.host_samples > 1;
  // Everything that can fail before anything is recorded.
  plume::RenderPipeline* depth_pipeline = Pipeline(ms ? kVDepthMs : kVDepth);
  plume::RenderPipeline* mip_pipeline = Pipeline(kVMip);
  plume::RenderPipeline* main_pipeline = nullptr;
  plume::RenderPipeline* denoise_pipeline = nullptr;
  plume::RenderPipeline* temporal_pipeline = nullptr;
  if (plan.apply) {
    main_pipeline = Pipeline(kVMain);
    denoise_pipeline = Pipeline(kVDenoise);
    temporal_pipeline = Pipeline(ms ? kVTemporalMs : kVTemporal);
  }
  if (!depth_pipeline || !mip_pipeline ||
      (plan.apply && (!main_pipeline || !denoise_pipeline || !temporal_pipeline))) {
    return false;
  }
  if (!EnsureImages(plan)) {
    if (!images_failed_logged_) {
      images_failed_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the images could not be made: no occlusion while that lasts (asked "
                   "again every frame)");
    }
    return false;
  }
  LightRaw light;
  uint32_t with_it = 0;
  const bool have_light = PickLight(&light, &with_it);
  UpdateLook();
  Push base;
  FillPush(&base, summary, plan, have_light ? &light : nullptr);

  // The depth plane is read as a texture by every pass and by the apply (after the resolve it is
  // already there: the call records a barrier only when it is elsewhere).
  edram_->Transition(list, depth_target, plume::RenderTextureLayout::SHADER_READ);

  Push push = base;
  pass_timing_->Mark(list, "ao depth");
  push.sizes[0] = float(plan.ao[0]);
  push.sizes[1] = float(plan.ao[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  DrawPass(list, depth_[0], depth_pipeline, push);
  // The coarser levels.
  push.slots[2] = depth_[0].slot;
  push.sizes[0] = float(plan.level[0][0]);
  push.sizes[1] = float(plan.level[0][1]);
  push.sizes[2] = float(plan.ao[0]);
  push.sizes[3] = float(plan.ao[1]);
  DrawPass(list, depth_[1], mip_pipeline, push);
  push.slots[2] = depth_[1].slot;
  push.sizes[0] = float(plan.level[1][0]);
  push.sizes[1] = float(plan.level[1][1]);
  push.sizes[2] = float(plan.level[0][0]);
  push.sizes[3] = float(plan.level[0][1]);
  DrawPass(list, depth_[2], mip_pipeline, push);

  bool applied = false;
  if (plan.apply) {
    // The visibility from the chain.
    pass_timing_->Mark(list, "ao visibility");
    push = base;
    push.slots[2] = depth_[0].slot;
    push.slots[3] = depth_[1].slot;
    push.slots2[0] = depth_[2].slot;
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    DrawPass(list, raw_, main_pipeline, push);
    // The history this frame can lean on, and the camera's motion into it.
    const HistoryUse use = ChooseHistory(plan);
    NoteHistory(use);
    // The denoise: once with a history to average into, twice (the second pass back into the first
    // one's input image) without. It is never left out, whatever the quality or the upscaler.
    pass_timing_->Mark(list, "ao denoise");
    push.slots[2] = raw_.slot;
    push.slots[3] = depth_[0].slot;
    DrawPass(list, denoised_, denoise_pipeline, push);
    HostImage* visibility = &denoised_;
    if (!use.on) {
      push.slots[2] = denoised_.slot;
      DrawPass(list, raw_, denoise_pipeline, push);
      visibility = &raw_;
    }
    // The accumulation over frames: this frame's visibility with the history's into the other
    // history image, which is the next frame's history and what the apply reads.
    // The two R16G16_FLOAT history images are render targets and shader resources in turn
    // (DrawPass's barriers and first use clear); the pass also reads the stencil plane view of the
    // depth target (Texture2D<uint4> / Texture2DMS<uint4>) in the depth target's SHADER_READ
    // layout. The census line "history:" counts the frames that used a history against the resets
    // and where the camera's motion came from.
    pass_timing_->Mark(list, "ao temporal");
    const uint32_t out_index = 1 - history_index_;
    HostImage& history_in = history_[history_index_];
    HostImage& history_out = history_[out_index];
    push = base;
    push.slots[2] = visibility->slot;
    push.slots[3] = depth_[0].slot;
    // Without a history the slot is any readable image: the shader does not read it.
    push.slots2[0] = use.on ? history_in.slot : depth_[0].slot;
    push.slots2[2] |= use.on ? kHistoryFlag : 0u;
    push.slots2[3] = summary.stencil_slot;
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    // The motion in this renderer's view space (x right, y up, z forward): the game's looks along
    // -z, so the matrix is S G S with S = diag(1, 1, -1, 1). The shader takes the columns as rows.
    const auto motion = [&use](int i, int j) {
      const double sign = ((i == 2) != (j == 2)) ? -1.0 : 1.0;
      return float(sign * use.motion.m[i][j]);
    };
    for (int k = 0; k < 4; ++k) {
      push.lin[k] = motion(k, 0);
      push.shape[k] = motion(k, 1);
      push.light[k] = motion(k, 2);
    }
    push.extra[0] = history_tan_[0];
    push.extra[2] = history_tan_[1];
    push.extra[3] = use.on ? float(1.0 - 1.0 / double(std::min(use.age + 2, use.cap))) : 0.0f;
    // Debug view 6 shows how much history each pixel kept: the pass runs once more into an image of
    // its own, so that the history that goes on to the next frame is the normal one whatever the
    // view.
    if (plan.view == 6u) {
      DrawPass(list, weight_, temporal_pipeline, push);
    }
    push.slots2[2] &= ~0xFFu;  // the frame's own run writes no debug picture
    DrawPass(list, history_out, temporal_pipeline, push);
    history_index_ = out_index;
    history_valid_ = true;
    history_cut_ = false;
    history_frame_ = frame_;
    history_pause_ = trigger_pause_;
    history_size_[0] = plan.ao[0];
    history_size_[1] = plan.ao[1];
    history_tan_[0] = base.lens[0];
    history_tan_[1] = base.lens[1];
    history_age_ = use.on ? std::min(use.age + 1, use.cap) : 0u;
    // The apply into the scene colour.
    push = base;
    push.slots[2] = (plan.view == 6u ? weight_ : history_out).slot;
    push.slots[3] = depth_[0].slot;
    push.sizes[0] = float(plan.render[0]);
    push.sizes[1] = float(plan.render[1]);
    push.sizes[2] = float(plan.ao[0]);
    push.sizes[3] = float(plan.ao[1]);
    applied = RecordApply(list, summary, plan, push);
    applied_this_frame_ = applied;
  }
  if (plan.probe) {
    RecordProbe(list, plan);
  }
  ran_this_frame_ = true;
  if (plan.apply && applied) {
    if (runs_ == 0) {
      REXLOG_INFO("[gpu] ao: the first occlusion pass: {} at {}x{} (visibility {}x{}), {} depth, "
                  "{}{}",
                  ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                  ms ? "multisampled" : "single-sampled",
                  plan.upscaler ? "noise rotating every frame, a temporal upscaler running too"
                                : "noise rotating every frame",
                  plan.view ? ", debug view" : "");
    }
    ++runs_;
    if (trigger_pause_) {
      if (pause_runs_ == 0) {
        const temporal::Camera& c = previous_;
        REXLOG_INFO("[gpu] ao: the first occlusion pass in a pause/map view: {} at {}x{} "
                    "(visibility {}x{}), {} host samples of the depth (guest sample 0 = host "
                    "sample {}), the lens of the previous pause/map frame's anchor draw: vertical "
                    "field of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}",
                    ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                    depth_target.host_samples, summary.host_sample,
                    c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane);
      }
      ++pause_runs_;
    }
  }
  return true;
}

bool AmbientOcclusion::RecordApply(plume::RenderCommandList* list,
                                   const ResolveCache::Summary& summary, const Plan& plan,
                                   const Push& push) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and
  // scale (the depth key's layer is 0 while the scene phase is open, the colour's is always 0).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples ||
      found->scale != depth.scale) {
    if (!refused_target_logged_) {
      refused_target_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the main view's colour target {} {} (depth: {} host samples at {}x): "
                   "no occlusion applied (reported once)",
                   DescribeEdramKey(key),
                   !found ? "does not exist" : (!found->texture ? "has no texture" : "differs"),
                   depth.host_samples, depth.scale);
    }
    return false;
  }
  plume::RenderPipeline* pipeline = Pipeline(ApplyVariant(plan.view != 0, depth.host_samples));
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  // The scene's own sequence for a target it draws into: the layout, a framebuffer of that target
  // alone (no depth: nothing here tests or writes it). It is a colour-only render pass on the main
  // target in the middle of the frame: the depth plane stays in SHADER_READ, the colour goes to
  // COLOR_WRITE as for any draw, and the next draw of the game moves the depth back to DEPTH_WRITE
  // itself.
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  pass_timing_->Mark(list, "ao apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(plan.render[0]), float(plan.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(plan.render[0]), int32_t(plan.render[1]));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the census's depth statistic
// ---------------------------------------------------------------------------

void AmbientOcclusion::RecordProbe(plume::RenderCommandList* list, const Plan&) {
  if (probe_busy_) {
    return;
  }
  HostImage& level = depth_[2];
  const uint32_t width = level.width;
  const uint32_t height = level.height;
  const uint32_t row_bytes = (width * 2 + 255) / 256 * 256;  // R16_FLOAT, rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!probe_buffer_ || probe_buffer_bytes_ < bytes) {
    probe_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes),
                                          "ao census readback");
    probe_buffer_bytes_ = probe_buffer_ ? bytes : 0;
    if (!probe_buffer_) {
      return;
    }
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(level.texture.get(),
                                             plume::RenderTextureLayout::COPY_SOURCE));
  level.layout = plume::RenderTextureLayout::COPY_SOURCE;  // the next DrawPass moves it on
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(probe_buffer_.get(),
                                                        plume::RenderFormat::R16_FLOAT, width,
                                                        height, 1, row_bytes / 2, 0),
      plume::RenderTextureCopyLocation::Subresource(level.texture.get()));
  probe_busy_ = true;
  ++probes_done_;
  const uint64_t frame = frame_;
  const uint64_t generation = generation_;
  const uint32_t row_texels = row_bytes / 2;
  frames_->RetireAction([this, frame, generation, width, height, row_texels] {
    ProcessProbe(frame, generation, width, height, row_texels, 2);
  });
}

void AmbientOcclusion::ProcessProbe(uint64_t frame, uint64_t generation, uint32_t width,
                                    uint32_t height, uint32_t row_texels, uint32_t level_shift) {
  probe_busy_ = false;
  if (generation != generation_ || !probe_buffer_ || width == 0 || height == 0) {
    return;
  }
  const void* mapped = probe_buffer_->map();
  if (!mapped) {
    REXLOG_ERROR("{} depth statistic of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  std::vector<float> values(size_t(width) * height);
  const uint16_t* source = static_cast<const uint16_t*>(mapped);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      values[size_t(y) * width + x] = HalfToFloat(source[size_t(y) * row_texels + x]);
    }
  }
  probe_buffer_->unmap();

  // Sky and distances over the whole picture, and over its thirds: outdoors the road at the bottom
  // of the picture is nearer than the middle (the guest's 1 near / 0 far, linearised with the
  // lens, gives that), and the sky is at the top.
  struct Third {
    uint64_t count = 0;
    uint64_t sky = 0;
    double sum = 0.0;
  };
  Third thirds[3];
  std::vector<float> sample;
  sample.reserve(16384);
  const size_t stride = std::max<size_t>(values.size() / 16384, 1);
  uint64_t sky = 0;
  for (uint32_t y = 0; y < height; ++y) {
    Third& third = thirds[std::min(3u * y / height, 2u)];
    for (uint32_t x = 0; x < width; ++x) {
      const float t = values[size_t(y) * width + x];
      ++third.count;
      if (!(t < 0.5f * kSkyDistance)) {
        ++third.sky;
        ++sky;
      } else {
        third.sum += double(t);
        if ((size_t(y) * width + x) % stride == 0) {
          sample.push_back(t);
        }
      }
    }
  }
  const auto share = [](uint64_t part, uint64_t whole) {
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
  };
  const auto mean = [](const Third& t) {
    return t.count > t.sky ? t.sum / double(t.count - t.sky) : 0.0;
  };
  float p[4] = {};  // min, median, 90th percentile, max
  if (!sample.empty()) {
    std::sort(sample.begin(), sample.end());
    p[0] = sample.front();
    p[1] = sample[sample.size() / 2];
    p[2] = sample[sample.size() * 9 / 10];
    p[3] = sample.back();
  }
  const double middle = mean(thirds[1]);
  const double bottom = mean(thirds[2]);
  const char* verdict =
      thirds[1].count == thirds[1].sky || thirds[2].count == thirds[2].sky
          ? "no ground to compare (an all-sky third)"
      : bottom < middle ? "the bottom of the picture is nearer than the middle: the guest's "
                          "1 near / 0 far holds"
                        : "the bottom of the picture is NOT nearer than the middle: look at the "
                          "convention (or this is not a street view)";
  REXLOG_INFO("{} depth statistic of frame {} (the linear depth chain's 1/{} level, {}x{}): sky "
              "{:.1f}%, distance min {:.2f} median {:.2f} 90th percentile {:.1f} max {:.1f}; top "
              "third {:.0f}% sky, mean distance of the middle third {:.1f}, of the bottom third "
              "{:.1f}: {}",
              kPrefix, frame, 1u << level_shift, width, height, share(sky, values.size()), p[0],
              p[1], p[2], p[3], share(thirds[0].sky, thirds[0].count), middle, bottom, verdict);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnPresent() {
  pause_spell_ = pause_view_ && !play_view_ ? pause_spell_ + 1 : 0;
  if (census_every_ != 0 && frame_had_main_view_) {
    Census();
  }
  if (mode_ == Mode::kOff) {
    // Off for a second: the images go back (they are made again by the first trigger after).
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames &&
        census_every_ == 0) {
      ReleaseImages();
      images_made_for_[0] = images_made_for_[1] = 0;
    }
  } else {
    off_frames_ = 0;
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  frame_had_main_view_ = false;
  qualifying_resolves_ = 0;
  later_tiles_ = 0;
  draws_before_ = 0;
  color_draws_before_ = 0;
  draws_after_ = 0;
  depth_writes_after_ = 0;
  draws_at_anchor_ = 0;
  view4x_draws_ = 0;
  light_count_ = 0;
  light_overflow_ = 0;
  light_draws_ = 0;
  depth_clears_ = 0;
  depth_resolves_.clear();
  other_resolves_ = 0;
  lens_delta_valid_ = false;
  vs_view_inverse_draws_ = 0;
  vs_compare_valid_ = false;
  have_camera_ = false;
  trigger_camera_valid_ = false;
  trigger_pause_ = false;
  outcome_ = Outcome::kNoTrigger;
  ran_this_frame_ = false;
  applied_this_frame_ = false;
  ++frame_;
}

const char* AmbientOcclusion::ViewName() const {
  if (play_view_ && pause_view_) {
    return "play + pause/map (mixed)";
  }
  return pause_view_ ? "pause/map (4x)" : play_view_ ? "play" : "no main";
}

std::string AmbientOcclusion::TriggerText() const {
  const char* kind = trigger_pause_ ? "pause/map" : "play";
  switch (outcome_) {
    case Outcome::kNoTrigger:
      return "no resolve of the frame was the trigger: no occlusion";
    case Outcome::kIdle:
      return "a trigger, with the occlusion off and no depth statistic due: nothing recorded";
    case Outcome::kNoCamera:
      return std::format("a {} trigger, but no previous frame's camera of a {} view (the frame "
                         "before was not a consecutive {} frame with a camera): no occlusion",
                         kind, kind, kind);
    case Outcome::kLensUnknown:
      return "a pause/map trigger in the second frame of a spell (or of the setting being on): the "
             "lens' rate of change is not known yet, no occlusion for this frame";
    case Outcome::kLensChanging:
      return std::format("a pause/map trigger, but the lens changed by {:.1f}% between the last "
                         "two frames (limit {:.0f}%): no occlusion until it settles",
                         100.0 * trigger_lens_step_, 100.0 * temporal::kCutFov);
    case Outcome::kTooSmall:
      return std::format("a {} trigger, but the picture is smaller than {} pixels: no occlusion",
                         kind, kMinRenderSize);
    case Outcome::kNotRecorded:
      return std::format("a {} trigger, but a pipeline or an image could not be made (see the "
                         "errors above): no occlusion", kind);
    case Outcome::kRecorded:
      break;
  }
  const char* done = applied_this_frame_ ? "passes recorded and applied to the main view's colour"
                     : mode_ == Mode::kOff   ? "the depth passes only (the census probe), nothing "
                                               "applied"
                                             : "passes recorded but the apply pass could not be "
                                               "drawn (see the errors above)";
  if (!trigger_camera_valid_) {
    return std::format("a {} trigger, {}", kind, done);
  }
  const temporal::Camera& c = trigger_camera_;
  return std::format("a {} trigger, {} with the lens of the PREVIOUS frame's motion blur draw "
                     "(frame {}, a {} view, the step from the frame before {:.2e}): vertical field "
                     "of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}, camera at "
                     "({:.1f}, {:.1f}, {:.1f}); the sun direction uses that frame's view rotation",
                     kind, done, c.frame, kind, trigger_lens_step_,
                     c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane,
                     c.position[0], c.position[1], c.position[2]);
}

void AmbientOcclusion::Census() {
  const bool main_view = play_view_ || pause_view_;
  const bool anomaly = main_view && qualifying_resolves_ != 1 && census_anomalies_logged_ < 40;
  // The first four frames of every pause/map spell are logged whatever the interval: a spell is
  // short, and its first frames are where its camera comes from.
  const bool pause_entry =
      pause_view_ && !play_view_ && pause_spell_ <= 4 && census_pause_logged_ < 40;
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!anomaly && !periodic && !pause_entry) {
    return;
  }
  if (anomaly) {
    ++census_anomalies_logged_;
  }
  if (pause_entry) {
    ++census_pause_logged_;
  }
  ++census_frames_logged_;
  const std::string spell = pause_view_ && !play_view_
                                ? std::format(" (frame {} of a pause/map spell)", pause_spell_)
                                : std::string();
  REXLOG_INFO("{} frame {}{}{}: {} view, main view draws {} before the first qualifying depth "
              "resolve ({} with colour), {} after it before the anchor ({} of them write the main "
              "depth), {} after the anchor, 4x (pause/map) draws {}; qualifying resolves {} "
              "(expect exactly 1), later tiles of its picture {}; anchor draw {}; clears of the "
              "main depth {} (last z {}); resolves of other kinds {}",
              kPrefix, frame_, anomaly ? " (UNEXPECTED trigger count)" : "", spell, ViewName(),
              draws_before_, color_draws_before_, draws_after_, depth_writes_after_,
              draws_at_anchor_, view4x_draws_, qualifying_resolves_, later_tiles_,
              anchor_seen_ ? "seen" : "NOT seen", depth_clears_,
              depth_clears_ ? std::format("{}", depth_clear_z_) : std::string("none"),
              other_resolves_);
  uint32_t index = 0;
  for (const DepthResolveNote& n : depth_resolves_) {
    REXLOG_INFO("{}   depth resolve {}: caller {:#010x}, source {}, host sample {} of {}, "
                "destination {:08x} format {} {}x{}, rect ({}, {})-({}, {}), {}, {}, after {} main "
                "view draws, anchor {}: {}",
                kPrefix, index++, n.caller, DescribeEdramKey(n.source), n.host_sample,
                n.host_samples, n.dest_address, n.dest_format, n.dest_width, n.dest_height,
                n.rect[0], n.rect[1], n.rect[2], n.rect[3],
                n.inside_end_tiling ? "inside EndTiling" : "plain resolve",
                n.clears_depth ? "clears the depth after" : "no clear", n.draws_before,
                n.anchor_before ? "already drawn" : "not yet",
                n.refused == 0 ? "THE TRIGGER"
                               : std::format("not the trigger ({})", RefusalNames(n.refused)));
  }
  LightRaw picked;
  uint32_t with_it = 0;
  if (PickLight(&picked, &with_it)) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(picked, ambient, sun, dir);
    float first_a[3], first_s[3], first_d[3], last_a[3], last_s[3], last_d[3];
    DecodeLight(first_light_, first_a, first_s, first_d);
    DecodeLight(last_light_, last_a, last_s, last_d);
    REXLOG_INFO("{}   light constants: {} distinct values over {} colour draws before the trigger "
                "({} draws past the table); the commonest ({} draws): PS c26 ambient {}, PS c39 sun "
                "colour {}, VS c16 the direction the light travels {} (length {:.4f}); first draw: "
                "{} {} {}; last draw: {} {} {}",
                kPrefix, light_count_ + (light_overflow_ ? 1 : 0), light_draws_, light_overflow_,
                with_it, Vec3(ambient), Vec3(sun), Vec3(dir), Length3(dir), Vec3(first_a),
                Vec3(first_s), Vec3(first_d), Vec3(last_a), Vec3(last_s), Vec3(last_d));
    if (have_camera_) {
      // The direction to the sun, -c16, through this frame's view (row vectors, v' = v M, the
      // game's view space looks along -z): at noon the sun is overhead, so it is up in the world
      // (y > 0) when c16 is the direction the light travels.
      double view[3] = {};
      for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
          view[j] -= double(dir[i]) * camera_.view.m[i][j];
        }
      }
      const double ambient_luma = Luma(ambient);
      const double sun_luma = Luma(sun);
      REXLOG_INFO("{}   sun: c16 is the direction the light travels, y {:+.3f}, so the direction to "
                  "the sun (-c16) has world y {:+.3f} (+ is up: a sun overhead at noon gives +) and, "
                  "in the game's view space, ({:+.3f}, {:+.3f}, {:+.3f}); luma ambient {:.4g}, "
                  "sun {:.4g}: ambient share for a surface facing the sun {:.3f}, a surface in "
                  "shade 1.000",
                  kPrefix, double(dir[1]), -double(dir[1]), view[0], view[1], view[2],
                  ambient_luma, sun_luma,
                  ambient_luma + sun_luma > 1e-9 ? ambient_luma / (ambient_luma + sun_luma) : 1.0);
    }
  } else {
    REXLOG_INFO("{}   light constants: no colour draw of the main view before the trigger", kPrefix);
  }
  if (have_camera_) {
    const temporal::Camera& c = camera_;
    REXLOG_INFO("{}   lens (the game's motion blur draw, this frame): vertical field of view {:.3f} "
                "deg, aspect {:.4f}, near {:.4f}, far {:.1f}, c62 {}x{}, camera at ({:.1f}, {:.1f}, "
                "{:.1f}) looking ({:+.3f}, {:+.3f}, {:+.3f})",
                kPrefix, c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane,
                c.far_plane, c.width, c.height, c.position[0], c.position[1], c.position[2],
                c.forward[0], c.forward[1], c.forward[2]);
    if (lens_delta_valid_) {
      REXLOG_INFO("{}   the previous frame's lens (what the trigger uses) against this one's: "
                  "relative change fov {:.2e}, aspect {:.2e}, near {:.2e}, far {:.2e}; camera moved "
                  "{:.3f}, cosine of the turn {:.6f}",
                  kPrefix, lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3],
                  lens_delta_[4], lens_delta_[5]);
    } else {
      REXLOG_INFO("{}   no previous frame's camera of this kind of view (the frame before was "
                  "not a consecutive {} frame with a camera): the trigger gives no occlusion this "
                  "frame",
                  kPrefix, pause_view_ ? "pause/map" : "play");
    }
  } else {
    REXLOG_INFO("{}   no camera this frame ({})", kPrefix,
                anchor_seen_ ? temporal::RejectName(reject_) : "no anchor draw");
  }
  if (vs_compare_valid_) {
    REXLOG_INFO("{}   vertex bank: VS c12-c15 of the last colour draw before the trigger "
                "against the view inverse the anchor reads from PS c12-c15: largest entry "
                "difference {:.3e} ({}); frames compared so far: play {} of {} equal, pause/map {} "
                "of {} equal (where they agree, the frame's own camera exists in the vertex bank "
                "before the anchor)",
                kPrefix, vs_compare_diff_,
                vs_compare_diff_ <= 1e-4 ? "equal" : "DIFFERENT", vs_equal_[0], vs_compared_[0],
                vs_equal_[1], vs_compared_[1]);
  }
  REXLOG_INFO("{}   trigger: {}", kPrefix, TriggerText());
  REXLOG_INFO("{}   history: this frame's trigger {}{}; frames that used a history {}, did not: first {}, "
              "gap {}, view {}, size {}, cut {}, cap {}; motion from the vertex constants {}, the "
              "last step {}, standing still {}; the vertex constants {} as the frame's camera",
              kPrefix,
              history_why_ == HistoryWhy::kUsed
                  ? std::format("used the history, camera motion from {}", PoseName(history_pose_))
                  : std::format("had no history: {}", HistoryWhyName(history_why_)),
              ran_this_frame_ ? "" : " (no passes recorded this frame)", history_used_,
              history_resets_[size_t(HistoryWhy::kFirst)], history_resets_[size_t(HistoryWhy::kGap)],
              history_resets_[size_t(HistoryWhy::kView)], history_resets_[size_t(HistoryWhy::kSize)],
              history_resets_[size_t(HistoryWhy::kCut)], history_resets_[size_t(HistoryWhy::kCap)],
              pose_frames_[size_t(Pose::kVertexBank)], pose_frames_[size_t(Pose::kPredicted)],
              pose_frames_[size_t(Pose::kStill)],
              vs_pose_trusted_ ? "are trusted" : "are not trusted yet");
  REXLOG_INFO("{}   occlusion: {} (mcla_gpu_ao), passes {} this frame, {} runs in all, {} frames "
              "without a previous frame's camera; pause/map view: {} runs, {} triggers without "
              "a pause/map camera, {} held back for the lens (second frame or a sweep); the depth "
              "statistic follows when the GPU has run its copy",
              kPrefix, ModeName(mode_), ran_this_frame_ ? "recorded" : "not recorded", runs_,
              skipped_no_lens_, pause_runs_, pause_skipped_no_lens_, pause_skipped_lens_);
}

}  // namespace mcla::gpu
