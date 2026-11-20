// MCLA GPU renderer -- volumetric light. See volumetrics.h.

#include "volumetrics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "gpu_class.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "scene_noise.h"
#include "shaders.h"
#include "texture_bytes.h"
#include "vol_lamp_list.h"

namespace mcla::gpu {

namespace vol = volumetrics;

namespace {

constexpr const char* kPrefix = "[volumetrics]";
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);
constexpr plume::RenderFormat kVolumeFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr plume::RenderTextureFlags kVolumeFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
constexpr uint32_t kMinRenderSize = 64;
// Frames without a volume before the images are given back (a clear noon, a menu: 2 s).
constexpr uint32_t kReleaseAfterFrames = 120;
constexpr uint32_t kIntervalFrames = 600;

// The registers read: VS c16 gLightPosDir (the direction the sunlight travels), PS c39 gLightColor (the sun colour), VS c43 gReflectionParams (x:
// the wetness), VS c12-c15 the view inverse.
constexpr uint32_t kVsSunDirection = 16;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsReflection = 43;
// The sun shadow collector's registers: PS c63 ShaderGlobals (z: the fade), c64-c79 the four light matrices, c80-c87 the planes and boxes; its
// atlas is the fetch constant 15 (DepthTextureSampler0, byte 60 of the shared constants' table).
constexpr uint32_t kPsShaderGlobals = 63;
constexpr uint32_t kPsCollectorFirst = 64;
constexpr uint32_t kPsCollectorCount = 24;
constexpr uint32_t kCollectorAtlasFetch = 15;
// The fog quad (xrage_postfx PSBlendedHeightFog*NoPFog), by pixel shader identity: the colour it draws is PS c64 (SuperFogMaxColor, w: opacity).
constexpr uint64_t kFogInFogPs = 0x0E5798C2CF01820Bull;
constexpr uint64_t kFogAboveFogPs = 0x51A47627B1B209BFull;
constexpr uint32_t kPsFogColour = 64;
// The vertex constants' view inverse is taken as the frame's own camera once it matched the anchor's (shafts.cpp, the same numbers).
constexpr double kVsMatch = 5e-4;
constexpr double kVsMoved = 5e-3;
// A camera step that starts the history again: a teleport, a shot change.
constexpr float kCutStepMetres = 25.0f;
constexpr float kCutTurnRadians = 0.7f;
// A frame this old at most is the collector's constants.
constexpr uint64_t kCollectorMaxAge = 3;

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

bool PoseSane(const temporal::Mat4& w) {
  if (!temporal::Finite(w)) {
    return false;
  }
  if (std::fabs(w.m[0][3]) > 1e-3 || std::fabs(w.m[1][3]) > 1e-3 || std::fabs(w.m[2][3]) > 1e-3 || std::fabs(w.m[3][3] - 1.0) > 1e-3) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] + w.m[r][2] * w.m[r][2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  return true;
}

// The text of the setting, read through LiveText (the pause menu's thread writes the cvar's string while this one reads it).
LiveText& Setting() {
  static LiveText text("mcla_gpu_volumetrics");
  return text;
}

vol::Choice ReadChoice() { return vol::Resolve(gpu_class::Current(), Setting().Get()); }

// A colour constant of the game: finite and not negative; a draw whose constants were never written (the first frame) reads any bit pattern, so a huge one is capped too.
float Usable(float x) { return std::isfinite(x) && x > 0.0f ? std::min(x, 64.0f) : 0.0f; }

vol::VolCamera CameraOf(const temporal::Camera& c) {
  vol::VolCamera cam;
  for (int i = 0; i < 3; ++i) {
    cam.pos[i] = float(c.position[i]);
    cam.right[i] = float(c.right[i]);
    cam.up[i] = float(c.up[i]);
    cam.forward[i] = float(c.forward[i]);
  }
  const temporal::Mat4& p = c.projection;
  cam.tan_x = float(1.0 / std::max(std::fabs(p.m[0][0]), 1e-9));
  cam.tan_y = float(1.0 / std::max(std::fabs(p.m[1][1]), 1e-9));
  return cam;
}

}  // namespace

Volumetrics::~Volumetrics() { Shutdown(); }

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

uint32_t Volumetrics::ApplyVariant(uint32_t host_samples) {
  return kVApplyBase + std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
}

std::string Volumetrics::VariantName(uint32_t variant) {
  switch (variant) {
    case kVInject: return "volumetrics inject";
    case kVIntegrate: return "volumetrics integrate";
    default: break;
  }
  return std::format("volumetrics apply {}x", 1u << (variant - kVApplyBase));
}

void Volumetrics::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram,
                             PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  shaders_failed_ = false;
  listed_ = false;
  built_ = false;
  // The layout, the shaders and the pipelines are made only where the setting resolves to something at the start (not on the Low class, where
  // `auto` is off): where it is off they would be compile work and memory for nothing. A live switch makes the layout and shaders in BeginFrame and
  // the pipelines in the background (HoldUntilBuilt).
  const vol::Choice choice = ReadChoice();
  if (vol::On(choice.level) && MakeLayoutAndShaders()) {
    ListPrecache(choice);
  } else {
    passes.precache().Note(std::format("volumetric light {}", vol::Describe(gpu_class::Current(), choice)), 0);
  }
}

bool Volumetrics::MakeLayoutAndShaders() {
  if (ready_) {
    return true;
  }
  if (shaders_failed_ || !device_ || !passes_) {
    return false;
  }
  cb_builder_.begin();
  cb_builder_.addByteAddressBuffer(0);  // t0, space 2: VolGpuConstants
  cb_builder_.end();
  uav_builder_.begin();
  uav_builder_.addReadWriteTexture(0);  // u0, space 3: the volume the pass writes
  uav_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 0, space 0: Texture2D<float4>[]
  builder.addDescriptorSet(heap_->sampler_set_builder());  // set 1, space 1: SamplerState[]
  builder.addDescriptorSet(cb_builder_);                   // set 2, space 2: the constants
  builder.addDescriptorSet(uav_builder_);                  // set 3, space 3: the output
  builder.addPushConstant(0, 4, sizeof(ComputePush), plume::RenderShaderStageFlag::COMPUTE);  // b0, space 4
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "volumetrics layout");
  bool ok = layout_ != nullptr;
  if (ok) {
    const HostShaderBlob inject = VolumetricsInjectShader();
    const HostShaderBlob integrate = VolumetricsIntegrateShader();
    inject_shader_ = device_->CreateShader(inject.data, inject.size, "main", inject.name);
    integrate_shader_ = device_->CreateShader(integrate.data, integrate.size, "main", integrate.name);
    for (uint32_t i = 0; i < 2; ++i) {
      const HostShaderBlob apply = VolumetricsApplyShader(i != 0);
      apply_shaders_[i] = passes_->CreateShader(apply.data, apply.size, apply.name);
    }
    ok = inject_shader_ && integrate_shader_ && apply_shaders_[0] && apply_shaders_[1];
  }
  if (!ok) {
    REXLOG_ERROR("{} the layout or a shader could not be made: no volumetric light this session", kPrefix);
    inject_shader_.reset();
    integrate_shader_.reset();
    for (auto& shader : apply_shaders_) {
      shader.reset();
    }
    layout_.reset();
    shaders_failed_ = true;
    return false;
  }
  ready_ = true;
  return true;
}

void Volumetrics::Shutdown() {
  // The host builder's functions name this object's shaders and layout: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (SunShafts::Shutdown).
  for (auto& image : scatter_) {
    image = HostImage{};
  }
  accum_ = HostImage{};
  for (auto& uav : uav_sets_) {
    uav = UavSet{};
  }
  for (auto& set : cb_set_) {
    set.reset();
  }
  for (auto& buffer : cb_) {
    buffer.reset();
  }
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  inject_shader_.reset();
  integrate_shader_.reset();
  for (auto& shader : apply_shaders_) {
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

std::vector<uint32_t> Volumetrics::Variants() const {
  // The compute passes and the composite for the play view's colour target at 1x, 2x and 4x (the host's MSAA level), 8x when the device has it,
  // so that a switch of the level waits for no pipeline.
  std::vector<uint32_t> variants = {kVInject, kVIntegrate, ApplyVariant(1), ApplyVariant(2), ApplyVariant(4)};
  if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
    variants.push_back(ApplyVariant(8));
  }
  return variants;
}

void Volumetrics::ListPrecache(const vol::Choice& choice) {
  HostPrecache& precache = passes_->precache();
  uint32_t added = 0;
  for (const uint32_t variant : Variants()) {
    added += precache.Add(HostPrecache::Key(HostPrecache::kVolumetrics, variant), VariantName(variant),
                          [this, variant] { return MakePipeline(variant); })
                 ? 1u
                 : 0u;
  }
  listed_ = true;
  precache.Note(std::format("volumetric light {}", vol::Describe(gpu_class::Current(), choice)), added);
}

void Volumetrics::HoldUntilBuilt(Level* level) {
  if (built_) {
    return;
  }
  HostPrecache& precache = passes_->precache();
  const std::vector<uint32_t> variants = Variants();
  if (!listed_) {
    std::vector<HostPrecache::Item> fresh;
    for (const uint32_t variant : variants) {
      fresh.push_back({HostPrecache::Key(HostPrecache::kVolumetrics, variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }});
    }
    listed_ = true;
    const uint32_t count = precache.AddLive("volumetric light", std::move(fresh));
    REXLOG_INFO("{} the volumetric light was off at the start: {} pipelines are made in the background, and the volume is drawn from the frame those are done",
                kPrefix, count);
  }
  // Not drawn until every pipeline is made (or has failed, which Pipeline says): a frame never waits for a compile.
  for (const uint32_t variant : variants) {
    if (!pipelines_[variant] && !pipeline_failed_[variant] && !precache.Ready(HostPrecache::Key(HostPrecache::kVolumetrics, variant))) {
      *level = Level::kOff;
      return;
    }
  }
  built_ = true;
}

std::unique_ptr<plume::RenderPipeline> Volumetrics::MakePipeline(uint32_t variant) {
  const std::string name = VariantName(variant);
  if (variant == kVInject) {
    return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), inject_shader_.get(), 8, 8, 1), name.c_str());
  }
  if (variant == kVIntegrate) {
    return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), integrate_shader_.get(), 8, 8, 1), name.c_str());
  }
  // The composite: into the scene colour (float16 RGBA) with the target's sample count, blend (ONE, SRC_ALPHA) on the colour, the alpha never written.
  const uint32_t samples = 1u << (variant - kVApplyBase);
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::ONE;
  blend.dstBlend = plume::RenderBlend::SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  return passes_->CreatePipeline(passes_->layout(), *apply_shaders_[samples > 1 ? 1 : 0], plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples,
                                 name.c_str());
}

plume::RenderPipeline* Volumetrics::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  // Made ahead when listed; a variant nobody listed (an 8x target) is made here.
  pipelines_[variant] = passes_->precache().Acquire(HostPrecache::Key(HostPrecache::kVolumetrics, variant), VariantName(variant),
                                                    [&] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no volumetric light this session in that configuration", kPrefix, VariantName(variant));
  }
  return pipelines_[variant].get();
}

void Volumetrics::BeginFrame() {
  census_every_ = uint32_t(std::max(0, larecomp::settings::mcla_gpu_vol_census.Get()));
  keep_cones_ = larecomp::settings::mcla_gpu_vol_keep_cones.Get();
  const vol::Choice choice = ReadChoice();
  Level level = choice.level;
  if (!ready_ && vol::On(level) && !shaders_failed_ && MakeLayoutAndShaders()) {
    REXLOG_INFO("{} the volumetric light was off at the start: its layout and shaders are made now", kPrefix);
  }
  if (!ready_ && vol::On(level)) {
    level = Level::kOff;  // the layout or a shader could not be made (MakeLayoutAndShaders said so)
  }
  if (uint32_t(level) != last_level_logged_) {
    last_level_logged_ = uint32_t(level);
    const vol::GridDesc grid = vol::GridFor(level);
    if (vol::On(level)) {
      REXLOG_INFO("{} volumetric light {} (mcla_gpu_volumetrics = {}: {}), live: {}x{}x{} froxels, {} lamps, {} headlights, {} m, history {}", kPrefix,
                  vol::Name(level), Setting().Get(), vol::Describe(gpu_class::Current(), choice), grid.nx, grid.ny, grid.nz, grid.lamps,
                  grid.headlights, grid.far_m, grid.temporal ? "on" : "off");
    } else {
      REXLOG_INFO("{} volumetric light off (mcla_gpu_volumetrics = {}: {}), live", kPrefix, Setting().Get(), vol::Describe(gpu_class::Current(), choice));
    }
  }
  if (vol::On(level)) {
    HoldUntilBuilt(&level);
  }
  level_ = level;
  if (!vol::On(level_)) {
    // The guest hook that copies the game's lamp list (vol_lamp_list.h) only works while the volume is on: the producer turns it on at its first Fill.
    vol_lamps::SetWanted(false);
  }
}

uint64_t Volumetrics::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* image : {&scatter_[0], &scatter_[1], &accum_}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  return bytes;
}

void Volumetrics::RetireUav(UavSet& uav) {
  if (uav.set && frames_) {
    frames_->Retire(std::move(uav.set));
  }
  uav = UavSet{};
}

void Volumetrics::ReleaseResources() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(scatter_[0]);
  passes_->ReleaseImage(scatter_[1]);
  passes_->ReleaseImage(accum_);
  for (auto& uav : uav_sets_) {
    RetireUav(uav);
  }
  made_for_ = vol::GridDesc{};
  history_.valid = false;
}

bool Volumetrics::EnsureUavSet(uint32_t index, const HostImage& image) {
  UavSet& uav = uav_sets_[index];
  if (uav.set && uav.texture == image.texture.get()) {
    return true;
  }
  RetireUav(uav);
  uav.set = device_->CreateDescriptorSet(uav_builder_, 1, 0, "volumetrics output set");
  if (!uav.set) {
    return false;
  }
  uav.set->setTexture(0, image.texture.get(), plume::RenderTextureLayout::GENERAL, image.view.get());
  uav.texture = image.texture.get();
  return true;
}

bool Volumetrics::EnsureConstantSet(uint32_t slot) {
  if (slot >= kFramesInFlight) {
    return false;
  }
  if (cb_set_[slot]) {
    return true;
  }
  const uint64_t size = (sizeof(vol::VolGpuConstants) + 255u) & ~uint64_t(255u);
  cb_[slot] = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(size, plume::RenderBufferFlag::STORAGE), "volumetrics constants");
  if (!cb_[slot]) {
    return false;
  }
  cb_set_[slot] = device_->CreateDescriptorSet(cb_builder_, 1, 0, "volumetrics constants set");
  if (!cb_set_[slot]) {
    cb_[slot].reset();
    return false;
  }
  cb_set_[slot]->setBuffer(0, cb_[slot].get(), size);
  return true;
}

bool Volumetrics::EnsureResources(const vol::GridDesc& grid) {
  const uint32_t w = grid.atlas_width();
  const uint32_t h = grid.atlas_height();
  const bool same = made_for_.nx == grid.nx && made_for_.ny == grid.ny && made_for_.nz == grid.nz && made_for_.tiles_x == grid.tiles_x;
  const bool ok = passes_->EnsureImage(scatter_[0], w, h, kVolumeFormat, "volumetrics scatter A", kVolumeFlags) &&
                  passes_->EnsureImage(scatter_[1], w, h, kVolumeFormat, "volumetrics scatter B", kVolumeFlags) &&
                  passes_->EnsureImage(accum_, w, h, kVolumeFormat, "volumetrics accumulated", kVolumeFlags);
  if (!ok) {
    ReleaseResources();
    return false;
  }
  if (!EnsureUavSet(0, scatter_[0]) || !EnsureUavSet(1, scatter_[1]) || !EnsureUavSet(2, accum_)) {
    ReleaseResources();
    return false;
  }
  if (!same) {
    made_for_ = grid;
    history_.valid = false;  // a new grid has no history
    REXLOG_INFO("{} volumes {}x{} ({}x{}x{} froxels, {}x{} tiles): scatter x2 and accumulated, RGBA16F, {:.1f} MiB", kPrefix, w, h, grid.nx, grid.ny, grid.nz,
                grid.tiles_x, grid.tiles_y, double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return true;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void Volumetrics::OnPreparedDraw(const ConstantBanks::Banks& banks, uint32_t ps_features, const uint8_t* shared_constants) {
  if ((ps_features & ShaderStore::FeatureBit(larecomp::shaderpack::kFeatureShadowFilter)) == 0 || !shared_constants) {
    return;
  }
  // A draw of the sun shadow collector (the pack's soft lookup variant exists only for these shaders). The first of the frame is read; its registers
  // are the same in every draw of a frame (GFX-UPGRADES-DESIGN.md 6B.2 item 1).
  if (collector_.valid && collector_.frame == frame_) {
    ++collector_.draws;
    return;
  }
  const uint8_t* ps = banks.ps.data();
  for (uint32_t r = 0; r < kPsCollectorCount; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      collector_.regs[r][c] = BankFloat(ps, kPsCollectorFirst + r, c);
    }
  }
  collector_.shader_globals_z = BankFloat(ps, kPsShaderGlobals, 2);
  uint32_t atlas = 0;
  std::memcpy(&atlas, shared_constants + 4 * kCollectorAtlasFetch, sizeof(atlas));
  atlas &= 0x7FFFFFFFu;  // the gamma flag (TFETCH_GAMMA_BIT)
  collector_.atlas_slot = atlas == kNullTexture2DSlot ? vol::kNoSlot : atlas;
  collector_.frame = frame_;
  collector_.draws = 1;
  collector_.valid = true;
}

void Volumetrics::OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks, uint64_t vs_identity, uint64_t ps_identity) {
  (void)vs_identity;
  if (!match) {
    return;
  }
  if (match.samples == temporal::kPauseSamples) {
    pause_view_ = true;  // the pause menu's, the race pause's, the garage's and the map's world (menu_world.h)
  } else {
    play_view_ = true;
  }
  if (ps_identity == kFogInFogPs || ps_identity == kFogAboveFogPs) {
    // The fog quad: its colour of this frame serves the next frame's haze (it is drawn after the first point).
    for (uint32_t c = 0; c < 4; ++c) {
      fog_colour_[c] = BankFloat(banks.ps.data(), kPsFogColour, c);
    }
    fog_seen_ = true;
    fog_frame_ = frame_;
  }
  if (anchor_seen_ || composite_seen_ || !match.color || first_point_seen_) {
    return;
  }
  // The light of the frame is the one most colour draws before the first point carry (a draw whose shaders do not use a register leaves what an earlier
  // one set).
  ++main_draws_;
  Seen now;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  for (uint32_t i = 0; i < 3; ++i) {
    now.dwords[i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
    now.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
  }
  now.dwords[6] = LoadBe32(vs + kVsReflection * 16);
  for (uint32_t i = 0; i < 16; ++i) {
    vs_view_inverse_[i] = LoadBe32(vs + 12 * 16 + i * 4);
  }
  ++vs_view_inverse_draws_;
  last_seen_ = now;
  for (uint32_t i = 0; i < seen_count_; ++i) {
    if (std::memcmp(seen_[i].dwords, now.dwords, sizeof(now.dwords)) == 0) {
      ++seen_[i].draws;
      return;
    }
  }
  if (seen_count_ < kSeenSlots) {
    seen_[seen_count_] = now;
    seen_[seen_count_].draws = 1;
    ++seen_count_;
  }
}

void Volumetrics::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  if (play_view_ == pause_view_) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  temporal::Camera camera;
  if (temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_, &camera) != temporal::Reject::kNone) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool same_kind = have_previous_ && previous_frame_ + 1 == frame_ && previous_pause_ == pause;
  // Whether this frame's vertex constants held this frame's view inverse: trusted once they did in a frame the camera moved in, dropped as soon as
  // they differ (ao.cpp, shafts.cpp).
  if (same_kind && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 vertex;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        vertex.m[r][c] = double(std::bit_cast<float>(vs_view_inverse_[r * 4 + c]));
      }
    }
    const double differs = temporal::MaxAbsDiff(vertex, camera.view_inverse);
    const double moved = temporal::MaxAbsDiff(camera.view_inverse, previous_.view_inverse);
    if (!(differs <= kVsMatch)) {
      vs_pose_trusted_ = false;
    } else if (moved >= kVsMoved) {
      vs_pose_trusted_ = true;
    }
  } else {
    vs_pose_trusted_ = false;
  }
  previous_ = camera;
  previous_pause_ = pause;
  have_previous_ = true;
  previous_frame_ = frame_;
}

bool Volumetrics::PoseFromVertexConstants(vol::VolCamera* out) const {
  if (!vs_pose_trusted_ || vs_view_inverse_draws_ == 0) {
    return false;
  }
  temporal::Mat4 world;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      world.m[r][c] = double(std::bit_cast<float>(vs_view_inverse_[r * 4 + c]));
    }
  }
  if (!PoseSane(world)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out->right[i] = float(world.m[0][i]);
    out->up[i] = float(world.m[1][i]);
    out->forward[i] = float(-world.m[2][i]);
    out->pos[i] = float(world.m[3][i]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// the frame's inputs
// ---------------------------------------------------------------------------

bool Volumetrics::ComputeLight(Light* out) const {
  *out = Light{};
  if (main_draws_ == 0) {
    return false;
  }
  Seen pick = last_seen_;
  if (seen_count_ != 0) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < seen_count_; ++i) {
      if (seen_[i].draws >= seen_[best].draws) {
        best = i;
      }
    }
    pick = seen_[best];
  }
  float dir[3];
  float colour[3];
  for (int i = 0; i < 3; ++i) {
    dir[i] = std::bit_cast<float>(pick.dwords[i]);
    colour[i] = Usable(std::bit_cast<float>(pick.dwords[3 + i]));
  }
  const float wet = std::bit_cast<float>(pick.dwords[6]);
  if (!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2])) {
    return false;
  }
  const double length = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] + double(dir[2]) * dir[2]);
  if (!(length > 0.9 && length < 1.1)) {
    return false;  // not a direction (no light constants in this draw)
  }
  // c16 is the direction the light travels: the sun is where it comes from.
  for (int i = 0; i < 3; ++i) {
    out->to_sun[i] = float(-double(dir[i]) / length);
    out->colour[i] = colour[i];
  }
  out->wetness = std::isfinite(wet) ? std::clamp(wet, 0.0f, 16.0f) : 0.0f;
  out->elevation = std::asin(std::clamp(double(out->to_sun[1]), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
  out->valid = true;
  return true;
}

void Volumetrics::FillCascades(vol::VolInputs* in) const {
  in->sun_valid = false;
  in->sun = vol::VolSun{};
  if (!collector_.valid || collector_.frame + kCollectorMaxAge < frame_ || collector_.atlas_slot == vol::kNoSlot) {
    return;
  }
  const float(*regs)[4] = collector_.regs;
  for (uint32_t r = 0; r < kPsCollectorCount; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      if (!std::isfinite(regs[r][c])) {
        return;
      }
    }
  }
  vol::VolSun& sun = in->sun;
  for (int i = 0; i < 3; ++i) {
    sun.right[i] = regs[85 - kPsCollectorFirst][i];
    sun.top[i] = regs[82 - kPsCollectorFirst][i];
  }
  for (uint32_t i = 0; i < vol::kMaxCascades; ++i) {
    vol::VolCascade& cascade = sun.cascades[i];
    for (uint32_t j = 0; j < 4; ++j) {
      for (uint32_t k = 0; k < 4; ++k) {
        cascade.rows[j][k] = regs[4 * i + j][k];
      }
    }
    cascade.box[0] = regs[86 - kPsCollectorFirst][i];
    cascade.box[1] = regs[87 - kPsCollectorFirst][i];
    cascade.box[2] = regs[83 - kPsCollectorFirst][i];
    cascade.box[3] = regs[84 - kPsCollectorFirst][i];
  }
  // A frame whose cascades are not drawn (night, a storm) may leave the registers as they were or empty: the boxes must have an extent.
  bool extent = false;
  for (uint32_t i = 0; i < vol::kMaxCascades; ++i) {
    extent = extent || (sun.cascades[i].box[1] > 0.0f && sun.cascades[i].box[3] > 0.0f);
  }
  if (!extent) {
    return;
  }
  sun.cascade_count = vol::kMaxCascades;
  sun.atlas_slot = collector_.atlas_slot;
  in->sun_valid = true;
}

void Volumetrics::FillDefaults(const Light& light, const vol::VolFrameContext& frame, vol::VolInputs* in) {
  using larecomp::settings::mcla_gpu_vol_density;
  *in = vol::VolInputs{};
  vol::VolWorld& w = in->world;
  const float wet = vol::WetFactor(light.wetness);
  const float night = vol::NightFactor(float(light.elevation));
  const float fog_opacity = fog_seen_ ? std::clamp(fog_colour_[3], 0.0f, 1.0f) : 0.0f;
  float sigma = vol::BaseSigma(wet, fog_opacity) * float(std::clamp(mcla_gpu_vol_density.Get(), 0.0, 50.0));
  // The ground layer: the smoothed height of the ground under the camera (a hill above the layer is clear air), 40 m deep by default.
  const float ground = frame.camera.pos[1] - 1.4f;
  ground_y_ = ground_valid_ ? ground_y_ + (ground - ground_y_) * 0.02f : ground;
  ground_valid_ = true;
  w.density[0] = sigma;
  w.density[1] = ground_y_;
  w.density[2] = float(std::clamp(larecomp::settings::mcla_gpu_vol_ground_height.Get(), 0.0, 500.0));
  w.density[3] = float(std::clamp(larecomp::settings::mcla_gpu_vol_ground_strength.Get(), 0.0, 20.0));
  const float elapsed = std::clamp(frame.elapsed_s, 0.0f, 0.25f);
  const float wind = float(std::clamp(larecomp::settings::mcla_gpu_vol_wind.Get(), 0.0, 50.0));
  wind_[0] += wind * elapsed;
  wind_[1] += 0.3f * wind * elapsed;
  w.noise[0] = float(std::clamp(larecomp::settings::mcla_gpu_vol_noise.Get(), 0.0, 1.0));
  w.noise[1] = 0.02f;
  w.noise[2] = std::fmod(wind_[0], 4096.0f);
  w.noise[3] = std::fmod(wind_[1], 4096.0f);
  for (int i = 0; i < 3; ++i) {
    w.sun_dir[i] = light.to_sun[i];
  }
  w.sun_dir[3] = 0.0f;
  // The sun: its colour (the game's c39) times how much sun there is at this height; nothing under the horizon or at noon.
  const float sun_strength = vol::SunStrength(float(light.elevation));
  for (int i = 0; i < 3; ++i) {
    w.sun_colour[i] = light.colour[i] * sun_strength;
  }
  w.sun_colour[3] = float(std::clamp(larecomp::settings::mcla_gpu_vol_g_sun.Get(), 0.0, 0.95));
  // The haze takes the game's fog colour (the fog quad's PS c64) scaled down: it is what a thickened fog looks like, not a second fog.
  for (int i = 0; i < 3; ++i) {
    w.haze_colour[i] = Usable(fog_colour_[i]) * 0.5f;
  }
  w.haze_colour[3] = float(std::clamp(larecomp::settings::mcla_gpu_vol_far.Get(), 0.0, 1000.0));
  w.mix[0] = night;
  w.mix[1] = wet;
  w.mix[2] = float(std::clamp(larecomp::settings::mcla_gpu_vol_g_lamp.Get(), 0.0, 0.95));
  w.mix[3] = float(std::clamp(larecomp::settings::mcla_gpu_vol_g_head.Get(), 0.0, 0.95));
  w.scale[0] = float(std::clamp(larecomp::settings::mcla_gpu_vol_extinction.Get(), 0.0, 1.0));
  w.scale[1] = float(std::clamp(larecomp::settings::mcla_gpu_vol_lamp_strength.Get(), 0.0, 50.0));
  w.scale[2] = float(std::clamp(larecomp::settings::mcla_gpu_vol_head_strength.Get(), 0.0, 50.0));
  w.scale[3] = float(std::clamp(larecomp::settings::mcla_gpu_vol_sun_strength.Get(), 0.0, 50.0));
  FillCascades(in);
  if (in->sun_valid) {
    w.sun_dir[3] = std::clamp(collector_.shader_globals_z, 0.0f, 1.0f);  // the collector's own fade: visibility lerp(lit, 1, z)
  }
  // The test lights (mcla_gpu_vol_test: 1 a street lamp, 2 a headlight): they stand where the camera looks, to see the beams of the passes with
  // no producer behind them. A lamp 8 m ahead and 4.5 m up, pointing down; a headlight 1.2 m under and 1 m beside the camera, pointing forward.
  const int test = larecomp::settings::mcla_gpu_vol_test.Get();
  const vol::VolCamera& c = frame.camera;
  if ((test & 1) != 0) {
    vol::VolLamp& lamp = in->lamps[in->lamp_count++];
    for (int i = 0; i < 3; ++i) {
      lamp.pos[i] = c.pos[i] + c.forward[i] * 8.0f;
      lamp.colour[i] = i == 0 ? 4.0f : (i == 1 ? 3.0f : 1.7f);  // the game's own scale: an intensity of 4 and a warm white
    }
    lamp.pos[1] += 4.5f;
    lamp.dir[0] = 0.0f;
    lamp.dir[1] = -1.0f;
    lamp.dir[2] = 0.0f;
    lamp.range = 25.0f;
    lamp.cos_outer = std::cos(62.0f * vol::kPi / 180.0f);
    lamp.cos_inner = std::cos(30.0f * vol::kPi / 180.0f);
    lamp.shadow_view = -1;
  }
  if ((test & 2) != 0) {
    vol::VolLamp& lamp = in->headlights[in->headlight_count++];
    for (int i = 0; i < 3; ++i) {
      lamp.pos[i] = c.pos[i] + c.right[i] * 1.0f - c.up[i] * 1.2f;
      lamp.dir[i] = c.forward[i];
      lamp.colour[i] = i == 2 ? 1.6f : 2.0f;  // the game's own scale: its lit headlight points read 2.0
    }
    lamp.range = 70.0f;
    lamp.cos_outer = std::cos(38.0f * vol::kPi / 180.0f);
    lamp.cos_inner = std::cos(14.0f * vol::kPi / 180.0f);
    lamp.shadow_view = -1;
  }
  if (test != 0) {
    // A test light needs something to show in: at least a hazy night's medium.
    w.density[0] = std::max(w.density[0], 0.004f);
  }
  in->valid = true;
}

// ---------------------------------------------------------------------------
// the first point: the mid-scene depth copy
// ---------------------------------------------------------------------------

void Volumetrics::PictureSize(const ResolveCache::Summary& summary, uint32_t* width, uint32_t* height) {
  *width = summary.dest_width;
  *height = summary.dest_height;
  if (summary.source) {
    *width = std::min(*width, summary.source->width);
    *height = std::min(*height, summary.source->height);
  }
}

bool Volumetrics::IsFirstPoint(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge, shafts.cpp IsFirstPoint): the first copy of the main depth that is no end-of-scene copy, into
  // 078B0000, before the anchor, whose rectangle starts at the origin and stays inside the picture. Only the play view here.
  if (!summary.depth_copy || !summary.source || first_point_seen_) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  const bool pause = samples == temporal::kPauseSamples;
  if (!temporal::IsPlaySamples(samples) && !pause) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth || summary.dest_address != kTriggerDestination || anchor_seen_ || composite_seen_) {
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  PictureSize(summary, &width, &height);
  if (pause) {
    return menu_world::PauseFirstPointGeometry(summary.x0, summary.y0, summary.x1, summary.y1, width, height, summary.source->width, summary.source->height,
                                               EdramTargets::kHeightAlignment);
  }
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 && uint32_t(summary.x1) <= width &&
         uint32_t(summary.y1) <= height;
}

EdramTargets::Target* Volumetrics::TargetOf(const EdramKey& key, plume::RenderCommandList* list) {
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture) {
    return nullptr;
  }
  bool unset = false;
  return edram_->Get(key, found->height, found->scale, 0, list, &unset);
}

bool Volumetrics::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t frame_slot,
                            bool upscaler_running) {
  if (!list || !ready_ || !IsFirstPoint(record, summary)) {
    return false;
  }
  first_point_seen_ = true;
  ++triggers_;
  outcome_ = Outcome::kNoCamera;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  if (pause) {
    // The pause menu's, the race pause's and the garage's world and the map have no volume (survey V10): nothing is recorded.
    outcome_ = Outcome::kWorld;
    return false;
  }
  if (!have_previous_ || previous_frame_ + 1 != frame_ || previous_pause_) {
    ++skipped_no_camera_;
    return false;
  }
  outcome_ = Outcome::kNoLight;
  Light light;
  if (!ComputeLight(&light)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    census_sun_[i] = light.to_sun[i];
  }
  census_sun_[3] = float(light.elevation);
  census_sun_valid_ = true;

  vol::GridDesc grid = vol::GridFor(level_);
  if (!grid.valid()) {
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  PictureSize(summary, &width, &height);
  const Scale scale = summary.source->scale;
  Pending plan;
  plan.render[0] = scale.Edge(width);
  plan.render[1] = scale.Edge(height);
  if (plan.render[0] < kMinRenderSize || plan.render[1] < kMinRenderSize) {
    outcome_ = Outcome::kTooSmall;
    return false;
  }

  // The camera of the frame: its own where the vertex constants hold it, else the previous frame's (the volume is then one frame behind a moving
  // camera); the lens is always the previous frame's.
  vol::VolCamera camera = CameraOf(previous_);
  const bool current = PoseFromVertexConstants(&camera);

  vol::VolFrameContext context;
  context.frame = frame_;
  context.level = level_;
  context.grid = grid;
  context.camera = camera;
  context.camera_is_current = current;
  context.render_width = plan.render[0];
  context.render_height = plan.render[1];
  context.upscaler = upscaler_running;
  context.elapsed_s = last_fill_frame_ != 0 && frame_ > last_fill_frame_ ? float(frame_ - last_fill_frame_) / 60.0f : 0.0f;
  last_fill_frame_ = frame_;
  auto inputs = std::make_unique<vol::VolInputs>();
  FillDefaults(light, context, inputs.get());
  if (source_) {
    source_->Fill(context, inputs.get());
  }
  if (!inputs->valid) {
    outcome_ = Outcome::kNoLight;
    return false;
  }
  const float sun_energy = std::max({inputs->world.sun_colour[0], inputs->world.sun_colour[1], inputs->world.sun_colour[2]}) * inputs->world.scale[3];
  if (!vol::HasSomethingToShow(*inputs, sun_energy, 1e-4f)) {
    // A clear noon with no light on, a night with no lamp in the list: nothing is recorded.
    outcome_ = Outcome::kGated;
    ++gated_;
    return false;
  }
  const float far_override = float(std::clamp(larecomp::settings::mcla_gpu_vol_far.Get(), 0.0, 1000.0));
  if (far_override > 0.0f && inputs->world.haze_colour[3] == 0.0f) {
    inputs->world.haze_colour[3] = far_override;
  }

  // The volumes and the constants' buffer first (a new grid has no history).
  if (!EnsureResources(grid) || !EnsureConstantSet(frame_slot)) {
    outcome_ = Outcome::kNotRecorded;
    if (!resources_failed_logged_) {
      resources_failed_logged_ = true;
      REXLOG_ERROR("{} the volumes or their constants could not be made: no volumetric light while that lasts (asked again every frame)", kPrefix);
    }
    return false;
  }

  // The history: last frame's volume at the previous camera, if it is the frame before, the same grid and no cut.
  const bool history_wanted = larecomp::settings::mcla_gpu_vol_temporal.Get() && grid.temporal;
  float step = 0.0f;
  float turn = 0.0f;
  bool cut = false;
  if (history_.valid) {
    for (int i = 0; i < 3; ++i) {
      const float d = camera.pos[i] - history_.camera.pos[i];
      step += d * d;
    }
    step = std::sqrt(step);
    const float cosine = camera.forward[0] * history_.camera.forward[0] + camera.forward[1] * history_.camera.forward[1] +
                         camera.forward[2] * history_.camera.forward[2];
    turn = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    cut = step > kCutStepMetres || turn > kCutTurnRadians;
  }
  const bool history_ok = history_wanted && history_.valid && history_frame_ + 1 == frame_ && history_level_ == level_ && !cut;
  vol::HistoryUse use = history_;
  use.valid = history_ok;
  use.weight = vol::HistoryWeight(grid, history_ok, step, turn);
  vol::VolGpuConstants constants;
  vol::MakeGpuConstants(grid, *inputs, camera, use, uint32_t(frame_), &constants);
  plan.grid = grid;
  plan.far_m = constants.depth[1];
  plan.extinction_scale = inputs->world.scale[0];
  const temporal::Mat4& p = previous_.projection;
  plan.lin[0] = float(p.m[2][2]);
  plan.lin[1] = float(p.m[2][3]);
  plan.lin[2] = float(p.m[3][2]);
  plan.lin[3] = float(p.m[3][3]);
  plan.host_samples = summary.source->host_samples;
  plan.depth_sample = summary.host_sample;
  plan.depth = summary.source_key;
  plan.colour.base_tiles = temporal::kMainColorBase;
  plan.colour.pitch = temporal::kMainPitch;
  plan.colour.depth = 0;
  plan.colour.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  plan.colour.samples = summary.source_key.samples;
  // The pipelines of every pass first (a frame does not take the volume and then lose the composite).
  if (!Pipeline(kVInject) || !Pipeline(kVIntegrate) || !Pipeline(ApplyVariant(plan.host_samples))) {
    outcome_ = Outcome::kNotRecorded;
    return false;
  }
  const bool shadows = inputs->shadow_atlas_slot != vol::kNoSlot && inputs->shadow_view_count != 0;
  if (!RecordVolume(list, frame_slot, plan, constants, use.valid && use.weight > 0.0f, shadows)) {
    outcome_ = Outcome::kNotRecorded;
    return false;
  }
  recorded_this_frame_ = true;
  ++volumes_;
  if (current) {
    ++current_camera_;
  }
  history_.valid = true;
  history_.camera = camera;
  history_.far_m = plan.far_m;
  history_.near_m = grid.near_m;
  history_.weight = 0.0f;
  history_frame_ = frame_;
  history_level_ = level_;
  parity_ ^= 1u;
  pending_ = plan;
  pending_.valid = true;  // the composite is still to be recorded
  outcome_ = Outcome::kSkyOnly;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first volume: frame {}, level {}, {}x{}x{} froxels, far {:.0f} m, {} lamps, {} headlights, {} shadow views, sun {}, camera {}, history {}, picture {}x{} ({} host "
                "samples); composited at {}; the debug cvars mcla_gpu_vol_* tune the look",
                kPrefix, frame_, vol::Name(level_), grid.nx, grid.ny, grid.nz, plan.far_m, constants.counts[0], constants.counts[1], constants.counts[2],
                inputs->sun_valid ? "through the cascades" : "unshadowed", current ? "this frame's" : "the previous frame's",
                use.valid && use.weight > 0.0f ? "on" : "off", plan.render[0], plan.render[1], plan.host_samples,
                shafts_anchor::AnchorName(anchor_));
  }
  anchor_ = policy_.Choose(larecomp::settings::mcla_gpu_vol_early.Get());
  if (anchor_ == shafts_anchor::Anchor::kMidScene) {
    // Here, before the fog and the particles: the debug switch, or the fall-back of a build or a scene that gives no copy of the main colour at the
    // second point. The copy is still watched there.
    pending_.valid = false;
    composited_ = RecordComposite(list, plan);
    outcome_ = composited_ ? Outcome::kRecorded : Outcome::kNotRecorded;
  }
  return true;
}

bool Volumetrics::RecordVolume(plume::RenderCommandList* list, uint32_t frame_slot, const Pending& plan, const vol::VolGpuConstants& constants,
                               bool use_history, bool use_shadows) {
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  const vol::GridDesc& g = plan.grid;
  HostImage& write = scatter_[parity_];
  HostImage& read = scatter_[parity_ ^ 1u];
  // Moves images to their layouts in one barrier and notes it (the layout the next pass finds them in).
  auto transition = [&](plume::RenderBarrierStages stages, std::initializer_list<std::pair<HostImage*, Layout>> changes) {
    plume::RenderTextureBarrier barriers[3];
    uint32_t count = 0;
    for (const auto& change : changes) {
      barriers[count++] = plume::RenderTextureBarrier(change.first->texture.get(), change.second);
      change.first->layout = change.second;
    }
    list->barriers(stages, nullptr, 0, barriers, count);
  };

  // The constants: written into this frame slot's buffer (the slot's last use is finished: the FrameRing waited its fence).
  void* mapped = cb_[frame_slot]->map();
  if (!mapped) {
    return false;
  }
  std::memcpy(mapped, &constants, sizeof(constants));
  cb_[frame_slot]->unmap();

  pass_timing_->Mark(list, "vol inject");
  // The history is read by the compute shader (a layout it can read); the volumes it writes are storage images.
  if (use_history) {
    transition(Stage::COMPUTE, {{&read, Layout::SHADER_READ}});
  }
  transition(Stage::COMPUTE, {{&write, Layout::GENERAL}, {&accum_, Layout::GENERAL}});

  ComputePush push = {};
  push.slots[0] = use_history ? read.slot : 0u;
  push.slots[1] = write.slot;
  push.slots[2] = (use_history ? 1u : 0u) | (use_shadows ? 2u : 0u);
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), 0);
  list->setComputeDescriptorSet(heap_->sampler_set(), 1);
  list->setComputeDescriptorSet(cb_set_[frame_slot].get(), 2);
  list->setComputeDescriptorSet(uav_sets_[parity_].set.get(), 3);
  list->setPipeline(Pipeline(kVInject));
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((g.nx + 7) / 8, (g.ny + 7) / 8, g.nz);

  // The integrate reads what the inject wrote.
  transition(Stage::COMPUTE, {{&write, Layout::SHADER_READ}});
  pass_timing_->Mark(list, "vol integrate");
  list->setComputeDescriptorSet(uav_sets_[2].set.get(), 3);
  list->setPipeline(Pipeline(kVIntegrate));
  push.slots[1] = write.slot;
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((g.nx + 7) / 8, (g.ny + 7) / 8, 1);

  // The composite (a pixel shader) reads the accumulated volume.
  transition(Stage::GRAPHICS, {{&accum_, Layout::SHADER_READ}});
  pass_timing_->Mark(list, "after vol integrate");
  return true;
}

// ---------------------------------------------------------------------------
// the second point: the end-of-scene copy of the main colour
// ---------------------------------------------------------------------------

bool Volumetrics::ColourSourceKey(const ResolveRecord& r, EdramKey* key) {
  if (r.flags & (kEventSurfaceUnreadable | kEventStackUnreadable)) {
    return false;
  }
  if ((r.resolve_flags & 7) != 0) {
    return false;  // colour 1..3 or depth: not the main colour (colour 0)
  }
  // As ResolveCache::Execute reads the source of a copy (and shafts.cpp ColourSourceKey): pitch and samples from the device's RB_SURFACE_INFO, base and
  // format from the bound surface's header (+0x1C), else the registers.
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const bool bound = pre.DeviceDword(guest::kDevColorSurfaces) != 0;
  xreg::RB_SURFACE_INFO surface_info;
  surface_info.value = pre.Register(guest::reg::kRbSurfaceInfo);
  key->pitch = uint16_t(surface_info.surface_pitch);
  key->samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
  xreg::RB_COLOR_INFO color_info;
  color_info.value = bound ? LoadBe32(r.state.surfaces[0] + 0x1C) : pre.Register(guest::reg::kRbColorInfo);
  key->base_tiles = uint16_t(color_info.color_base | (color_info.color_base_bit_11 << 11));
  key->depth = 0;
  key->format = uint8_t(FoldColorFormat(uint32_t(color_info.color_format)));
  return true;
}

bool Volumetrics::BeforeResolve(const ResolveRecord& record, plume::RenderCommandList* list) {
  // Only after the first point of a frame whose volume was made, before the anchor draw, and until a copy of the main colour has come.
  if (!recorded_this_frame_ || copy_seen_ || !list || !ready_) {
    return false;
  }
  if (anchor_seen_ || composite_seen_) {
    return false;  // the scene is over: no copy of the main colour came in time
  }
  EdramKey key;
  const bool keyed = ColourSourceKey(record, &key);
  if (!keyed || !(key == pending_.colour) || (record.resolve_flags & 0x2000u) != 0) {
    return false;
  }
  copy_seen_ = true;
  if (!pending_.valid) {
    return false;  // the volume went in at the first point: the copy was only watched
  }
  const Pending plan = pending_;
  pending_.valid = false;  // once: the other tiles' copies find the picture with the light in it
  composited_ = RecordComposite(list, plan);
  outcome_ = composited_ ? Outcome::kRecorded : Outcome::kNotRecorded;
  return composited_;
}

bool Volumetrics::RecordComposite(plume::RenderCommandList* list, const Pending& plan) {
  EdramTargets::Target* colour = TargetOf(plan.colour, list);
  EdramTargets::Target* depth = TargetOf(plan.depth, list);
  if (!colour || !depth || colour->host_samples != plan.host_samples || depth->host_samples != plan.host_samples || !accum_.texture) {
    return false;
  }
  plume::RenderPipeline* pipeline = Pipeline(ApplyVariant(colour->host_samples));
  if (!pipeline) {
    return false;
  }
  uint32_t depth_slot = 0;
  uint32_t stencil_slot = 0;
  if (!edram_->SampleSlots(*depth, &depth_slot, &stencil_slot)) {
    return false;
  }
  // The depth plane is read as a texture; the colour is drawn into. After the pass the game's next draws bind their own layouts again.
  edram_->Transition(list, *depth, plume::RenderTextureLayout::SHADER_READ);
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  const vol::GridDesc& g = plan.grid;
  ApplyPush push = {};
  push.slots[0] = depth_slot;
  push.slots[1] = accum_.slot;
  push.slots[2] = g.nx | (g.ny << 8) | (g.nz << 16) | (g.tiles_x << 24);
  push.slots[3] = plan.depth_sample;
  push.sizes[0] = float(plan.render[0]);
  push.sizes[1] = float(plan.render[1]);
  push.sizes[2] = plan.extinction_scale;
  push.depth[0] = g.near_m;
  push.depth[1] = plan.far_m;
  push.depth[2] = 1.0f / vol::LogRatio(g.near_m, plan.far_m);
  for (int i = 0; i < 4; ++i) {
    push.lin[i] = plan.lin[i];
  }
  pass_timing_->Mark(list, "vol apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(plan.render[0]), float(plan.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(plan.render[0]), int32_t(plan.render[1]));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  pass_timing_->Mark(list, "after vol apply");
  ++runs_;
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

const char* Volumetrics::OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNoTrigger: return "no resolve of the frame was the first point (the mid-scene depth copy)";
    case Outcome::kWorld: return "the world has none (the pause menu's, the map, the garage): nothing recorded";
    case Outcome::kNoCamera: return "no previous frame's camera";
    case Outcome::kNoLight: return "no usable light constants in the main view's draws, or no inputs";
    case Outcome::kGated: return "nothing to show now (a medium too thin, no sun, no lamp, no headlight): nothing recorded";
    case Outcome::kTooSmall: return "the picture is smaller than the passes are worth";
    case Outcome::kNotRecorded: return "a pipeline, an image or a buffer could not be made (see the errors above)";
    case Outcome::kSkyOnly: return "the volume was made, no copy of the main colour came after it before the anchor draw: nothing composited";
    case Outcome::kRecorded: return "the volume made and composited";
  }
  return "?";
}

menu_world::World Volumetrics::FrameWorld() const {
  if (play_view_ == pause_view_) {
    return menu_world::World::kNone;  // no main view in the frame, or both kinds
  }
  return play_view_ ? menu_world::World::kPlay : menu_world::World::kNone;
}

std::string Volumetrics::LevelName() const { return std::string(vol::Name(level_)); }

void Volumetrics::EndFrame() {
  if (recorded_this_frame_) {
    const bool was_fallen_back = policy_.fallen_back();
    policy_.EndOfFrame(anchor_, copy_seen_);
    if (!composited_) {
      ++lost_;
    }
    if (was_fallen_back != policy_.fallen_back()) {
      if (policy_.fallen_back()) {
        REXLOG_WARN("{} frame {}: no copy of the main colour came after the first point in {} frames in a row: the volume is composited at {} from now on, "
                    "before the fog; the copy is still watched",
                    kPrefix, frame_, shafts_anchor::Policy::kMissesToFallBack, shafts_anchor::AnchorName(shafts_anchor::Anchor::kMidScene));
      } else {
        REXLOG_INFO("{} frame {}: the copy of the main colour came again: the volume is composited at {} again", kPrefix, frame_,
                    shafts_anchor::AnchorName(shafts_anchor::Anchor::kEndOfScene));
      }
    }
  }
  if (recorded_this_frame_ && SkipsLightCones()) {
    // counted by the Scene's skip; nothing here
  }
}

void Volumetrics::OnPresent() {
  if (level_ != Level::kOff) {
    EndFrame();
  }
  if (census_every_ != 0 && (play_view_ || pause_view_)) {
    Census();
  }
  if (level_ == Level::kOff) {
    // Off for a second: the images go back (they are made again by the first volume after).
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames) {
      ReleaseResources();
    }
    unused_frames_ = 0;
  } else {
    off_frames_ = 0;
    // A noon or a menu gives no volume for long: the images go back after a while.
    if (recorded_this_frame_) {
      unused_frames_ = 0;
    } else if (unused_frames_ < kReleaseAfterFrames && ++unused_frames_ == kReleaseAfterFrames) {
      ReleaseResources();
    }
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  first_point_seen_ = false;
  main_draws_ = 0;
  seen_count_ = 0;
  vs_view_inverse_draws_ = 0;
  pending_ = Pending{};
  outcome_ = Outcome::kNoTrigger;
  recorded_this_frame_ = false;
  composited_ = false;
  copy_seen_ = false;
  ++frame_;
}

void Volumetrics::Census() {
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!periodic) {
    return;
  }
  ++census_frames_logged_;
  REXLOG_INFO("{} frame {}: {} view, {} (level {}); {} first points, {} gated, {} without a camera, {} lost, {} composites; collector: {} (atlas slot {}, "
              "age {} frames), fog quad: {}; history {}; camera this frame's in {} of {} volumes; sun {}",
              kPrefix, frame_, pause_view_ ? "pause/map" : "play", OutcomeName(outcome_), vol::Name(level_), triggers_, gated_, skipped_no_camera_, lost_,
              runs_, collector_.valid ? std::format("{} draws", collector_.draws) : std::string("none"),
              collector_.valid && collector_.atlas_slot != vol::kNoSlot ? std::to_string(collector_.atlas_slot) : std::string("none"),
              collector_.valid ? frame_ - collector_.frame : 0, fog_seen_ ? std::format("colour ({:.3f}, {:.3f}, {:.3f}), opacity {:.2f}", fog_colour_[0], fog_colour_[1], fog_colour_[2], fog_colour_[3]) : std::string("not seen"),
              history_.valid ? "valid" : "none", current_camera_, volumes_,
              census_sun_valid_ ? std::format("toward ({:.3f}, {:.3f}, {:.3f}), elevation {:.1f} degrees", census_sun_[0], census_sun_[1], census_sun_[2], census_sun_[3])
                                : std::string("not read"));
}

}  // namespace mcla::gpu
