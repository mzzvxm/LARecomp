// MCLA GPU renderer -- the player car's sun shadow map. See car_shadow_map.h and car_shadow_map_rules.h.

#include "car_shadow_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ao.h"
#include "car_frame.h"
#include "descriptors.h"
#include "device.h"
#include "draw_state.h"
#include "edram.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_plan.h"
#include "live_text.h"
#include "pass_timing.h"
#include "menu_world.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

// The generated blobs of this pass's shaders (src/gpu/CMakeLists.txt larecomp_gpu_host_shader), included here beside host_shaders.h's
// macros so that file is not touched: the arrays have internal linkage.
#if defined(LARECOMP_GPU_HAS_D3D12)
#include "car_shadow_map_vs.hlsl.dxil.h"
#include "car_shadow_matrix_ps.hlsl.dxil.h"
#include "car_shadow_view_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "car_shadow_map_vs.hlsl.spirv.h"
#include "car_shadow_matrix_ps.hlsl.spirv.h"
#include "car_shadow_view_ps.hlsl.spirv.h"
#endif
#include "host_shaders.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_car_shadow_map, "auto", "MCLA/GPU",
    "Car shadow map: your car casts a shadow of its real shape on itself and on the road beside it. The game's own sun shadows "
    "draw the car in a rough low-detail form into maps whose texels are 1 to 25 cm, so the wing, the mirrors, the sills and the "
    "wheels cast no shadow of their own; this draws the car again from the sun, at full detail, into a small map fitted to it "
    "(2048 by 2048 on ultra, 1024 by 1024 on high), and the contact shadows read it. It only runs while the contact shadows "
    "(mcla_gpu_contact_shadows) are on, in the game view, in daylight. off, high, ultra or auto (the default): ultra on a discrete "
    "graphics card, high on an integrated one. Costs a little graphics card time (the car's triangles drawn once more, depth only) "
    "and 8 or 32 MiB of video memory. Live.")
    .allowed({"auto", "off", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_car_shadow_map_view, 0, "MCLA/GPU",
    "Diagnostic: 1 draws the car shadow map in a square in the lower left corner of the picture (grey by depth, near the sun "
    "bright; dark blue where nothing of the car is), while the map is drawn. 0: off. Live.")
    .debug_only()
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

namespace rules = car_shadow_map;

constexpr const char* kPrefix = "[car shadow]";
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// The map's push block: a 4x4 matrix as rows (hlsl/car_shadow_map_vs.hlsl).
constexpr uint32_t kPushBytes = 16 * sizeof(float);
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
// Frames without a map recorded (the setting off, no car, a pause menu) before the images are given back: 4 s at 60 FPS.
constexpr uint32_t kReleaseAfterFrames = 240;
constexpr uint64_t kCensusFrames = 1800;
constexpr uint32_t kMaxWhyLines = 12;

// mcla_gpu_car_shadow_map and mcla_gpu_contact_shadows through LiveText (the pause menu's thread writes the cvars' strings while this
// one reads them; the compare key's flip of the contact shadows is in the second).
LiveText& Setting() {
  static LiveText text("mcla_gpu_car_shadow_map");
  return text;
}

LiveText& ContactSetting() {
  static LiveText text("mcla_gpu_contact_shadows");
  return text;
}

HostShaderBlob VertexBlob() { return MCLA_GPU_HOST_SHADER_BLOB(car_shadow_map_vs); }
HostShaderBlob MatrixBlob() { return MCLA_GPU_HOST_SHADER_BLOB(car_shadow_matrix_ps); }
HostShaderBlob ViewBlob() { return MCLA_GPU_HOST_SHADER_BLOB(car_shadow_view_ps); }

rules::PositionKind KindOf(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R32G32B32_FLOAT:
    case plume::RenderFormat::R32G32B32A32_FLOAT:
      return rules::PositionKind::kFloat3;  // a float4's fourth float is not read
    case plume::RenderFormat::R16G16B16A16_FLOAT:
      return rules::PositionKind::kHalf4;
    default:
      return rules::PositionKind::kNone;
  }
}

constexpr uint32_t TriangleList() { return uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST); }

bool IsTriangles(plume::RenderPrimitiveTopology topology) {
  return topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST || topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

}  // namespace

CarShadowMap::CarShadowMap() = default;
CarShadowMap::~CarShadowMap() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarShadowMap::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram,
                              PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  base_listed_ = false;
  shaders_failed_ = false;
  const gpu_class::GpuClass cls = gpu_class::Current();
  const rules::Choice choice = rules::Resolve(cls, Setting().Get());
  const bool contact = host_plan::Resolve(host_plan::Pass::kContact, cls, true, ContactSetting().Get()).level != host_plan::Level::kOff;
  const std::string note = rules::Describe(choice, cls);
  // The shaders are small blobs and made at once; what costs is the pipelines (a driver compile each), listed only where the setting
  // and the contact shadows are on at the start.
  if (!MakeShaders()) {
    passes.precache().Note(note + ": the shaders or the layout could not be made, no map", 0);
    return;
  }
  if (choice.level != rules::Level::kOff && contact) {
    ListBase(true, note);
  } else {
    passes.precache().Note(note + (contact ? "" : ", the contact shadows are off"), 0);
  }
}

bool CarShadowMap::MakeShaders() {
  if (ready_) {
    return true;
  }
  if (shaders_failed_ || !device_ || !passes_) {
    return false;
  }
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/true);
  builder.addPushConstant(0, 0, kPushBytes, plume::RenderShaderStageFlag::VERTEX);  // b0, space0
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "car shadow map layout");
  const HostShaderBlob vs = VertexBlob();
  vertex_shader_ = device_->CreateShader(vs.data, vs.size, "main", vs.name);
  const HostShaderBlob matrix = MatrixBlob();
  matrix_shader_ = passes_->CreateShader(matrix.data, matrix.size, matrix.name);
  if (!layout_ || !vertex_shader_ || !matrix_shader_) {
    REXLOG_ERROR("{} a shader or the pipeline layout could not be made: no car shadow map this session", kPrefix);
    layout_.reset();
    vertex_shader_.reset();
    matrix_shader_.reset();
    shaders_failed_ = true;
    return false;
  }
  ready_ = true;
  return true;
}

void CarShadowMap::Shutdown() {
  // The host builder's functions name this object's shaders and layout: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (CarSelfRefl::Shutdown).
  map_ = Map{};
  matrix_ = HostImage{};
  items_.clear();
  pipelines_.clear();
  listed_.clear();
  failed_.clear();
  to_list_.clear();
  view_shader_.reset();
  matrix_shader_.reset();
  vertex_shader_.reset();
  layout_.reset();
  ready_ = false;
  view_ = CarShadowMapView{};
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

uint64_t CarShadowMap::PrecacheKey(uint32_t variant) {
  return HostPrecache::Key(HostPrecache::kCarShadowMap, variant);
}

std::string CarShadowMap::VariantName(uint32_t variant) const {
  if (variant == kMatrixVariant) {
    return "car shadow matrix";
  }
  if (variant < rules::VariantKey(rules::PositionKind::kFloat3, 0, 1)) {
    return std::format("car shadow view ({} samples)", variant - kViewVariantBase);
  }
  const uint32_t topology = rules::VariantTopology(variant);
  return std::format("car shadow depth ({} stride {}, {})",
                     rules::VariantKind(variant) == rules::PositionKind::kHalf4 ? "half4" : "float3", rules::VariantStride(variant),
                     topology == uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_STRIP) ? "strip" : "list");
}

std::unique_ptr<plume::RenderPipeline> CarShadowMap::MakeDepthPipeline(uint32_t variant) {
  const rules::PositionKind kind = rules::VariantKind(variant);
  const plume::RenderInputSlot slot(0, rules::VariantStride(variant));
  // The element is at offset 0: the vertex view is shifted by the position's offset when the draw is recorded.
  const plume::RenderInputElement element("POSITION", 0, 0,
                                          kind == rules::PositionKind::kHalf4 ? plume::RenderFormat::R16G16B16A16_FLOAT
                                                                              : plume::RenderFormat::R32G32B32_FLOAT,
                                          0, 0);
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = nullptr;  // depth only
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = &element;
  desc.inputElementsCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology(rules::VariantTopology(variant));
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.depthTargetFormat = kDepthFormat;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = true;
  desc.depthFunction = plume::RenderComparisonFunction::LESS;
  // Clipped, not clamped: a stray triangle of the mesh parked far from the car (the census saw draws 888 m away) must not land as
  // "nearest" on the near plane.
  desc.depthClipEnabled = true;
  // Both faces: a thin part (a wing, a mirror's stalk) casts whatever way its triangles face, and a guest strip's winding says nothing here.
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  // The slope scaled bias; the constant one is the reader's, in metres (the map is a D32 float, whose constant bias scales with the
  // depth's exponent).
  desc.depthBias = 0;
  desc.depthBiasClamp = 0.0f;
  desc.slopeScaledDepthBias = rules::kSlopeScaledDepthBias;
  desc.renderTargetCount = 0;
  const std::string name = VariantName(variant);
  return device_->CreateGraphicsPipeline(desc, name.c_str());
}

std::unique_ptr<plume::RenderPipeline> CarShadowMap::MakePipeline(uint32_t variant) {
  if (variant == kMatrixVariant) {
    return passes_->CreatePipeline(*matrix_shader_, plume::RenderFormat::R32G32B32A32_FLOAT, "car shadow matrix");
  }
  return MakeDepthPipeline(variant);
}

void CarShadowMap::ListBase(bool at_start, const std::string& note) {
  std::vector<uint32_t> variants = {kMatrixVariant};
  for (uint32_t stride : rules::kKnownStrides) {
    variants.push_back(rules::VariantKey(rules::PositionKind::kFloat3, TriangleList(), stride));
  }
  HostPrecache& precache = passes_->precache();
  if (at_start) {
    uint32_t added = 0;
    for (uint32_t variant : variants) {
      if (listed_.insert(variant).second) {
        added += precache.Add(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }) ? 1u : 0u;
      }
    }
    precache.Note(note, added);
  } else {
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t variant : variants) {
      if (listed_.insert(variant).second) {
        fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }});
      }
    }
    const uint32_t count = precache.AddLive("car shadow map", std::move(fresh));
    REXLOG_INFO("{} the car shadow map was off at the start: {} pipelines are made in the background, and the map is drawn from the "
                "frame those are done ({})",
                kPrefix, count, note);
  }
  base_listed_ = true;
}

void CarShadowMap::FlushListing() {
  if (to_list_.empty()) {
    return;
  }
  std::vector<HostPrecache::Item> fresh;
  for (uint32_t variant : to_list_) {
    fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }});
  }
  to_list_.clear();
  passes_->precache().AddLive("car shadow map", std::move(fresh));
}

plume::RenderPipeline* CarShadowMap::Pipeline(uint32_t variant, bool* pending) {
  const auto found = pipelines_.find(variant);
  if (found != pipelines_.end()) {
    return found->second.get();
  }
  if (failed_.count(variant) != 0) {
    return nullptr;
  }
  if (listed_.insert(variant).second) {
    // Nobody listed it (a stride the census did not see): the background builder makes it; no map until it is made.
    to_list_.push_back(variant);
    *pending = true;
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!precache.Ready(PrecacheKey(variant))) {
    *pending = true;
    return nullptr;
  }
  std::unique_ptr<plume::RenderPipeline> made =
      precache.Acquire(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!made) {
    failed_.insert(variant);
    REXLOG_ERROR("{} the pipeline '{}' could not be made: the draws that need it are left out of the map", kPrefix, VariantName(variant));
    return nullptr;
  }
  return (pipelines_[variant] = std::move(made)).get();
}

plume::RenderPipeline* CarShadowMap::ViewPipeline(uint32_t host_samples) {
  const uint32_t variant = kViewVariantBase + host_samples;
  const auto found = pipelines_.find(variant);
  if (found != pipelines_.end()) {
    return found->second.get();
  }
  if (failed_.count(variant) != 0) {
    return nullptr;
  }
  if (!view_shader_) {
    const HostShaderBlob blob = ViewBlob();
    view_shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  }
  std::unique_ptr<plume::RenderPipeline> made;
  if (view_shader_) {
    // Debug only: made here on the render thread at its first use (a hitch the player never sees), the scene colour's rgb replaced.
    plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
    blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                  uint8_t(plume::RenderColorWriteEnable::BLUE);
    made = passes_->CreatePipeline(passes_->layout(), *view_shader_, plume::RenderFormat::R16G16B16A16_FLOAT, blend, host_samples,
                                   "car shadow view");
  }
  if (!made) {
    failed_.insert(variant);
    return nullptr;
  }
  return (pipelines_[variant] = std::move(made)).get();
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

uint64_t CarShadowMap::HeldBytes() const {
  uint64_t bytes = 0;
  if (map_.texture) {
    bytes += TextureBytes(map_.size, map_.size, 1, kDepthFormat);
  }
  if (matrix_.texture) {
    bytes += TextureBytes(matrix_.width, matrix_.height, 1, matrix_.format);
  }
  return bytes;
}

bool CarShadowMap::EnsureMap(uint32_t size) {
  if (map_.texture && map_.size == size) {
    return true;
  }
  // Another size (a live change of the setting): the old map goes after the lists that name it.
  if (map_.texture) {
    if (map_.slot != kNoSlot) {
      heap_->ReleaseTexture(map_.slot, TextureDimension::k2D);
    }
    frames_->Retire(std::move(map_.framebuffer));
    frames_->Retire(std::move(map_.view));
    device_->NoteReleased(TextureBytes(map_.size, map_.size, 1, kDepthFormat));
    frames_->Retire(std::move(map_.texture));
  }
  map_ = Map{};
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(size, size, 1, kDepthFormat, plume::RenderTextureFlag::DEPTH_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(1);
  map_.texture = device_->CreateTexture(desc, "car shadow map");
  if (map_.texture) {
    // The texture's own format: Plume views a depth format through its depth plane (R32_FLOAT_X8X24 on D3D12, the DEPTH aspect on
    // Vulkan), as the EDRAM depth targets' sample views are made (edram.cpp SampleSlots).
    plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kDepthFormat);
    view.mipLevels = 1;
    map_.view = device_->CreateTextureView(*map_.texture, view, "car shadow map view");
    const plume::RenderTexture* none[1] = {nullptr};
    map_.framebuffer = device_->CreateFramebuffer(plume::RenderFramebufferDesc(none, 0, map_.texture.get()), "car shadow map");
  }
  if (map_.view) {
    map_.slot = heap_->AllocateTexture(*map_.texture, *map_.view);
  }
  if (!map_.texture || !map_.view || !map_.framebuffer || map_.slot == kInvalidBindlessSlot) {
    if (!view_failed_logged_) {
      view_failed_logged_ = true;
      REXLOG_ERROR("{} the {}x{} map could not be made (reported once): no car shadow map while that lasts", kPrefix, size, size);
    }
    if (map_.slot != kNoSlot && map_.slot != kInvalidBindlessSlot) {
      heap_->ReleaseTexture(map_.slot, TextureDimension::k2D);
    }
    map_ = Map{};
    return false;
  }
  map_.size = size;
  map_.layout = plume::RenderTextureLayout::UNKNOWN;
  return true;
}

bool CarShadowMap::EnsureMatrix() {
  return passes_->EnsureImage(matrix_, 4, 1, plume::RenderFormat::R32G32B32A32_FLOAT, "car shadow matrix");
}

void CarShadowMap::ReleaseImages() {
  if (!passes_ || !device_) {
    return;
  }
  if (map_.texture) {
    if (map_.slot != kNoSlot) {
      heap_->ReleaseTexture(map_.slot, TextureDimension::k2D);
    }
    frames_->Retire(std::move(map_.framebuffer));
    frames_->Retire(std::move(map_.view));
    device_->NoteReleased(TextureBytes(map_.size, map_.size, 1, kDepthFormat));
    frames_->Retire(std::move(map_.texture));
  }
  map_ = Map{};
  passes_->ReleaseImage(matrix_);
  size_logged_ = 0;  // the next map is logged again
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CarShadowMap::BeginFrame() {
  ++frame_;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
  view_ = CarShadowMapView{};
  if (!ready_) {
    wanted_ = false;
    return;
  }
  const gpu_class::GpuClass cls = gpu_class::Current();
  choice_ = rules::Resolve(cls, Setting().Get());
  contact_on_ =
      host_plan::Resolve(host_plan::Pass::kContact, cls, true, ContactSetting().Get()).level != host_plan::Level::kOff;
  view_mode_ = int(REXCVAR_GET(mcla_gpu_car_shadow_map_view));
  wanted_ = choice_.level != rules::Level::kOff && contact_on_ && car_frame_ != nullptr;
  const uint32_t state = uint32_t(choice_.level) * 2u + (contact_on_ ? 1u : 0u);
  if (state != last_level_logged_) {
    last_level_logged_ = state;
    REXLOG_INFO("{} {}; the contact shadows are {} (mcla_gpu_car_shadow_map = {}, live)", kPrefix, rules::Describe(choice_, cls),
                contact_on_ ? "on" : "off", Setting().Get());
  }
  if (wanted_) {
    if (!base_listed_) {
      ListBase(false, rules::Describe(choice_, cls));
    }
    car_frame_->Request();  // a request in this frame makes the next frame collect the car's draws
  }
  // No map for a while (the setting off, no car on the screen, a menu): the images go back.
  if (idle_frames_ < kReleaseAfterFrames && ++idle_frames_ == kReleaseAfterFrames) {
    ReleaseImages();
  }
  if (wanted_ && frame_ % kCensusFrames == 0) {
    Census();
  }
}

bool CarShadowMap::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge, car_selfrefl.cpp IsTrigger): the first copy of the main depth that is no
  // end-of-scene copy, into 078B0000, before the anchor. The play view's rectangle starts at the origin and stays inside the picture;
  // the pause view's (4 guest samples: the pause menu's, the garage's and the map's worlds) three tiles come in an order nobody has
  // looked at, so any rectangle inside the picture will do and the destination must be the whole picture (menu_world.h). The
  // contact shadows run in both, so the map is drawn in both (the owner's broken spoiler shadow was seen behind the pause menu).
  if (!summary.depth_copy || !summary.source || trigger_seen_) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  const bool pause = samples == temporal::kPauseSamples;
  if (!temporal::IsPlaySamples(samples) && !pause) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth || summary.dest_address != kTriggerDestination || anchor_seen_ ||
      composite_seen_) {
    return false;
  }
  const uint32_t width = std::min(summary.dest_width, summary.source->width);
  const uint32_t height = std::min(summary.dest_height, summary.source->height);
  if (pause) {
    return menu_world::PauseFirstPointGeometry(summary.x0, summary.y0, summary.x1, summary.y1, width, height, summary.source->width,
                                               summary.source->height, EdramTargets::kHeightAlignment);
  }
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 && uint32_t(summary.x1) <= width &&
         uint32_t(summary.y1) <= height;
}

void CarShadowMap::NoteSkipped(rules::Why why) {
  ++skipped_[size_t(why)];
  if (uint32_t(why) != last_why_logged_) {
    last_why_logged_ = uint32_t(why);
    static uint32_t lines = 0;  // render thread only
    if (lines < kMaxWhyLines) {
      ++lines;
      REXLOG_INFO("{} no map now: {} (logged when the reason changes)", kPrefix, rules::WhyName(why));
    }
  }
}

bool CarShadowMap::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (!list || !ready_ || !wanted_ || !IsTrigger(record, summary)) {
    return false;
  }
  trigger_seen_ = true;
  ++triggers_;
  const car_frame::Frame& frame = car_frame_->Current();
  // The world's sun (the AO's light of the frame); the car's own light only when the AO has none.
  double to_sun[3] = {frame.to_sun[0], frame.to_sun[1], frame.to_sun[2]};
  if (ao_) {
    double world[3];
    if (ao_->SunDirection(world)) {
      to_sun[0] = world[0];
      to_sun[1] = world[1];
      to_sun[2] = world[2];
    }
  }
  rules::Why why = rules::Decide(choice_.level, contact_on_, /*play_view=*/true, frame.valid && !frame.draws.empty(), to_sun);
  if (why == rules::Why::kRun && ao_ && ao_->ContactGated()) {
    why = rules::Why::kContactGated;  // the rain: nobody reads the map
  }
  if (why != rules::Why::kRun) {
    NoteSkipped(why);
    return false;
  }
  // The fit: the car's frame, its held centre and radius, the sun.
  rules::Pose pose;
  if (!rules::MakePose(frame.player_world, &pose)) {
    NoteSkipped(rules::Why::kNoCar);
    return false;
  }
  const rules::Estimate estimate = rules::EstimateOf(pose, frame.bounds_valid, frame.bounds_min, frame.bounds_max);
  const double radius_before = held_.radius;
  held_ = rules::Hold(held_, estimate);
  const uint32_t size = choice_.size();
  rules::Fit fit;
  if (!rules::MakeFit(to_sun, pose, held_, size, &fit)) {
    NoteSkipped(rules::Why::kNoSun);
    return false;
  }
  if (held_.radius != radius_before) {
    REXLOG_INFO("{} fit: the car's centre is ({:.2f}, {:.2f}, {:.2f}) m along its axes from its root, radius {:.2f} m ({}), so the map "
                "covers {:.2f} m a side and a texel is {:.1f} mm at {}x{} (logged when the radius changes)",
                kPrefix, held_.offset[0], held_.offset[1], held_.offset[2], held_.radius,
                estimate.from_bounds ? "from the frame's bounds" : "the fallback sphere: no usable bounds", 2.0 * fit.half_extent,
                fit.texel_m * 1000.0, size, size);
  }
  // The contact pass's rows: the AO's view space -> the map.
  float to_map[3][4];
  if (!rules::ViewToMap(fit, frame.view, to_map)) {
    NoteSkipped(rules::Why::kNoCar);
    return false;
  }

  // The draws: the rigid parts, each with the pipeline of its position variant. The skinned driver is left out.
  items_.clear();
  bool pending = false;
  Pipeline(kMatrixVariant, &pending);
  uint32_t skinned = 0;
  uint32_t unusable = 0;
  for (const car_frame::Draw& d : frame.draws) {
    if (d.part != car_frame::Part::kBody) {
      ++skinned;
      continue;
    }
    const DrawGeometry& g = d.geometry;
    const rules::PositionKind kind = KindOf(d.position_format);
    if (!IsTriangles(g.topology) || g.count == 0 || d.position_slot >= g.slot_count || d.position_slot >= DrawGeometry::kMaxSlots ||
        (g.indexed && !g.index_view.buffer.ref)) {
      ++unusable;
      continue;
    }
    const plume::RenderVertexBufferView& vb = g.vertex_views[d.position_slot];
    if (!vb.buffer.ref || !rules::PositionUsable(kind, d.position_offset, d.position_stride, vb.size)) {
      ++unusable;
      continue;
    }
    plume::RenderPipeline* pipeline = Pipeline(rules::VariantKey(kind, uint32_t(g.topology), d.position_stride), &pending);
    if (!pipeline) {
      ++unusable;  // pending or failed: the frame is held below, a failed one is left out
      continue;
    }
    items_.push_back({&d, pipeline});
  }
  FlushListing();
  draws_skinned_ += skinned;
  last_skinned_ = skinned;
  draws_unusable_ += unusable;
  if (pending) {
    // A pipeline is listed or being made: no map this frame (a part missing from it would let the sun through), and no wait.
    ++pending_frames_;
    if (!pending_logged_) {
      pending_logged_ = true;
      REXLOG_INFO("{} a pipeline for the car's vertex format is still being made in the background: no map until it is (reported once)",
                  kPrefix);
    }
    return false;
  }
  if (items_.empty()) {
    if (!unusable_logged_) {
      unusable_logged_ = true;
      REXLOG_WARN("{} the car's draws have no usable position (not a float3, float4 or half4 vertex view of triangles): no map "
                  "(reported once; {} draws, {} skinned left out)",
                  kPrefix, frame.draws.size(), skinned);
    }
    NoteSkipped(rules::Why::kNoCar);
    return false;
  }
  if (!EnsureMap(size) || !EnsureMatrix()) {
    return false;
  }
  plume::RenderPipeline* matrix_pipeline = Pipeline(kMatrixVariant, &pending);
  if (!matrix_pipeline) {
    return false;
  }
  if (size_logged_ != size) {
    size_logged_ = size;
    REXLOG_INFO("{} map {}x{} D32 float + S8 ({:.1f} MiB), a texel {:.1f} mm, depth range {:.2f} m, {} draws of the car's {} ({} skinned left "
                "out, {} unusable) (logged when the size changes)",
                kPrefix, size, size, Mib(HeldBytes()), fit.texel_m * 1000.0, fit.depth_m, items_.size(), frame.draws.size(), skinned,
                unusable);
  }
  RecordMap(fit, list);
  float matrix_rows[16];
  for (int k = 0; k < 3; ++k) {
    for (int j = 0; j < 4; ++j) {
      matrix_rows[k * 4 + j] = to_map[k][j];
    }
  }
  matrix_rows[12] = float(fit.texel_m);
  matrix_rows[13] = float(fit.depth_m);
  matrix_rows[14] = float(size);
  matrix_rows[15] = 1.0f;
  passes_->DrawInto(list, matrix_, matrix_pipeline, matrix_rows, sizeof(matrix_rows));
  if (view_mode_ == 1) {
    RecordView(summary, list);
  }
  ++maps_;
  draws_ += items_.size();
  last_draw_count_ = uint32_t(items_.size());
  idle_frames_ = 0;
  view_.valid = true;
  view_.slot = map_.slot;
  view_.size = size;
  std::memcpy(view_.to_map, to_map, sizeof(to_map));
  view_.matrix_slot = matrix_.slot;
  view_.texel_m = float(fit.texel_m);
  view_.depth_m = float(fit.depth_m);
  return true;
}

void CarShadowMap::RecordMap(const car_shadow_map::Fit& fit, plume::RenderCommandList* list) {
  pass_timing_->Mark(list, "car shadow map");
  if (map_.layout != plume::RenderTextureLayout::DEPTH_WRITE) {
    const plume::RenderTextureBarrier barrier(map_.texture.get(), plume::RenderTextureLayout::DEPTH_WRITE);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, barrier);
    map_.layout = plume::RenderTextureLayout::DEPTH_WRITE;
  }
  list->setFramebuffer(map_.framebuffer.get());
  list->clearDepthStencil(true, true, 1.0f, 0);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(map_.size), float(map_.size)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(map_.size), int32_t(map_.size));
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(layout_.get());
  const plume::RenderPipeline* bound = nullptr;
  float rows[16];
  for (const Item& item : items_) {
    const car_frame::Draw& d = *item.draw;
    const DrawGeometry& g = d.geometry;
    if (item.pipeline != bound) {
      list->setPipeline(item.pipeline);
      bound = item.pipeline;
    }
    rules::ModelToClip(fit, d.model_to_world, rows);
    list->setGraphicsPushConstants(0, rows, 0, kPushBytes);
    // The draw's own vertex view, shifted to its POSITION element (checked usable above), at the stride its main-view draw read.
    plume::RenderVertexBufferView view = g.vertex_views[d.position_slot];
    view.buffer.offset += d.position_offset;
    view.size -= d.position_offset;
    const plume::RenderInputSlot slot(0, d.position_stride);
    list->setVertexBuffers(0, &view, 1, &slot);
    if (g.indexed) {
      list->setIndexBuffer(&g.index_view);
      list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
    } else {
      list->drawInstanced(g.count, 1, g.first, 0);
    }
  }
  list->setFramebuffer(nullptr);
  // The contact pass (and the debug view) read it as a texture.
  const plume::RenderTextureBarrier read(map_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, read);
  map_.layout = plume::RenderTextureLayout::SHADER_READ;
}

bool CarShadowMap::RecordView(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples || found->scale != depth.scale) {
    return false;
  }
  plume::RenderPipeline* pipeline = ViewPipeline(depth.host_samples);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  const uint32_t render_w = depth.scale.Edge(std::min(summary.dest_width, depth.width));
  const uint32_t render_h = depth.scale.Edge(std::min(summary.dest_height, depth.height));
  const rules::Inset inset = rules::InsetFor(render_w, render_h);
  struct ViewPush {
    uint32_t slots[4];
    float rect[4];
  } push = {{map_.slot, map_.size, 0, 0}, {float(inset.x), float(inset.y), float(inset.size), float(inset.size)}};
  static_assert(sizeof(ViewPush) <= HostPasses::kPushConstantBytes);
  pass_timing_->Mark(list, "car shadow map view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{float(inset.x), float(inset.y), float(inset.size), float(inset.size)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(inset.x, inset.y, inset.x + inset.size, inset.y + inset.size);
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

void CarShadowMap::Census() {
  REXLOG_INFO("{} frame {}: {} ({}x{}); since the start {} triggers, {} maps recorded ({} draws, the last {}; {} skinned draws left out "
              "in the last, {} unusable in all), {} frames held for a pipeline being made; no map: {} no car, {} sun, {} sun too low, "
              "{} not the game view; {:.1f} MiB",
              kPrefix, frame_, rules::Describe(choice_, gpu_class::Current()), choice_.size(), choice_.size(), triggers_, maps_, draws_,
              last_draw_count_, last_skinned_, draws_unusable_, pending_frames_, skipped_[size_t(rules::Why::kNoCar)],
              skipped_[size_t(rules::Why::kNoSun)], skipped_[size_t(rules::Why::kSunLow)], skipped_[size_t(rules::Why::kNotPlayView)],
              Mib(HeldBytes()));
}

}  // namespace mcla::gpu
