// MCLA GPU renderer -- the headlight shadow maps. See local_shadow.h and local_shadow_rules.h.

#include "local_shadow.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "cycles.h"
#include "descriptors.h"
#include "device.h"
#include "draw_state.h"
#include "edram.h"
#include "frame.h"
#include "gpu_class.h"
#include "headlight_rig.h"
#include "layout.h"
#include "light_id.h"
#include "light_manager.h"
#include "light_project.h"
#include "light_text.h"
#include "live_text.h"
#include "menu_world.h"
#include "pass_timing.h"
#include "shadow_atlas.h"
#include "texture_bytes.h"

// The generated blobs of the debug view's pixel shader (src/gpu/CMakeLists.txt larecomp_gpu_host_shader), included here beside
// host_shaders.h's macros so that file is not touched: the arrays have internal linkage.
#if defined(LARECOMP_GPU_HAS_D3D12)
#include "local_shadow_view_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "local_shadow_view_ps.hlsl.spirv.h"
#endif
#include "host_shaders.h"

namespace mcla::gpu {

namespace {

namespace rules = local_shadow;
namespace tp = ::mcla::gpu::temporal;

constexpr const char* kPrefix = "[local-shadow]";
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
constexpr uint64_t kConstantAlignment = 256;
constexpr uint32_t kBankFloats = guest::kConstantBankBytes / sizeof(float);  // 1024: a register bank, 256 registers of four floats
constexpr uint32_t kCameraTries = 48;   // distinct banks looked at for the frame's camera
// Frames without a map recorded (the setting off, no lamp lit) before the atlas is given back: 4 s at 60 FPS.
constexpr uint32_t kReleaseAfterFrames = 240;
constexpr uint64_t kCensusFrames = 1800;
constexpr uint32_t kFlushGapFrames = 15;  // pipelines first seen are handed to the builder at most this often
constexpr uint32_t kLitGraceFrames = 3;   // frames a map is still collected for after the rig's last lit light
constexpr uint32_t kViewChangeLogs = 8;   // the first changes of which cars hold a view are logged one by one, the rest only counted
constexpr uint32_t kListEveryFrames = 8;  // with no lit headlight the main view's draws are looked at one frame in this many, to list their pipelines

LiveText& Setting() {
  static LiveText text("mcla_gpu_light_shadows");
  return text;
}

HostShaderBlob ViewBlob() { return MCLA_GPU_HOST_SHADER_BLOB(local_shadow_view_ps); }

bool IsTriangles(plume::RenderPrimitiveTopology topology) {
  return topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST || topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
}

rules::Cull GameCull(uint32_t raster) {
  switch (plume::RenderCullMode(raster & 0xF)) {
    case plume::RenderCullMode::FRONT: return rules::Cull::kFront;
    case plume::RenderCullMode::BACK: return rules::Cull::kBack;
    default: return rules::Cull::kNone;
  }
}

plume::RenderCullMode HostCull(uint8_t cull) {
  switch (rules::Cull(cull)) {
    case rules::Cull::kFront: return plume::RenderCullMode::FRONT;
    case rules::Cull::kBack: return plume::RenderCullMode::BACK;
    default: return plume::RenderCullMode::NONE;
  }
}

bool SameViews(const plume::RenderVertexBufferView* a, const plume::RenderVertexBufferView* b, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (a[i].buffer.ref != b[i].buffer.ref || a[i].buffer.offset != b[i].buffer.offset || a[i].size != b[i].size) {
      return false;
    }
  }
  return true;
}

double MilliSeconds(uint64_t ns) { return double(ns) / 1e6; }

}  // namespace

// What one trigger's replay works from: the views, the atlas and the signs of the pass's NDC.
struct LocalShadow::Plan {
  struct View {
    rules::ViewSlot slot;     // the lights the view covers (a lamp's, or the car's) and whose view it is (local_shadow_rules.h PlanViews)
    rules::ViewSpec spec;
    tp::Mat4 clip;
    tp::Mat4 replay;
    bool skirt_ok = false;    // the vehicle parts are drawn a second time: the skirt (local_shadow_rules.h "the ground skirt")
    float uv[4] = {};
    rules::TileRect tile;
    std::vector<uint32_t> keep;  // the casters this view draws, in draw order
  };
  rules::AtlasLayout atlas;
  rules::Light lights[4];
  uint32_t light_count = 0;
  uint32_t views = 0;
  uint32_t lamp_views = 0;    // views made from a headlamp (the others are a car's)
  View view[rules::kMaxViews];
  double sx = 1.0;
  double sy = 1.0;
  rules::Camera camera;
};

LocalShadow::LocalShadow() = default;
LocalShadow::~LocalShadow() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void LocalShadow::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing,
                             UploadRing& upload, ShaderStore& shaders, GuestLayout& layout) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  upload_ = &upload;
  shaders_ = &shaders;
  guest_layout_ = &layout;
  ready_ = true;
  // The pipelines are made when a draw of their shape is first seen (they depend on the game's shaders and vertex declarations, which are
  // not known until it draws): nothing is listed here.
  const gpu_class::GpuClass cls = gpu_class::Current();
  const rules::Choice choice = rules::Choice(rules::Resolve(cls, Setting().Get()));
  passes.precache().Note(rules::Describe(choice, cls) + ": its pipelines are made in the background as the game's draws are first seen", 0);
}

void LocalShadow::Shutdown() {
  // The host builder's functions name this object's members: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  if (shadow_atlas_ != nullptr) {
    shadow_atlas_->RemoveProvider(this);  // the atlas outlives this object (the renderer shuts it down after)
  }
  // The GPU is idle and the heap goes later: the slot is not handed back (CarShadowMap::Shutdown).
  atlas_ = Atlas{};
  view_pipelines_.clear();
  view_failed_.clear();
  view_shader_.reset();
  pipes_.clear();
  pipe_ids_.clear();
  to_list_.clear();
  casters_.clear();
  published_ = larecomp::gpu::LocalShadowFrame{};
  atlas_info_ = LocalShadowAtlas{};
  HandOver();
  use_atlas_ = false;
  replay_pool_ = -1;
  ready_ = false;
  wanted_ = false;
  offering_ = false;
  collecting_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  upload_ = nullptr;
  shaders_ = nullptr;
  guest_layout_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

uint64_t LocalShadow::PrecacheKey(uint32_t id) { return HostPrecache::Key(HostPrecache::kLocalShadow, id); }

bool LocalShadow::VsSkinned(const ShaderStore::Entry& vs) {
  const auto found = vs_skinned_.find(vs.key);
  if (found != vs_skinned_.end()) {
    return found->second;
  }
  bool skinned = false;
  shaders_->GetVertexElements(vs, elements_);
  for (const ShaderStore::VertexElement& e : elements_) {
    skinned |= e.usage == 2;  // D3DDECLUSAGE_BLENDINDICES (car_mesh_read.h kUsageBlendIndices)
  }
  vs_skinned_.emplace(vs.key, skinned);
  return skinned;
}

std::unique_ptr<plume::RenderPipeline> LocalShadow::MakePipeline(const rules::PipeKey& key, const ShaderStore::Entry* vs, const ShaderStore::Entry* ps,
                                                                 const InputLayout* layout) {
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  plume::RenderShader* vertex = vs ? shaders_->GetShader(*vs, key.vs_variant, &vs_result) : nullptr;
  plume::RenderShader* pixel = (key.ps != 0 && ps) ? shaders_->GetShader(*ps, key.ps_variant, &ps_result) : nullptr;
  if (!vertex || (key.ps != 0 && !pixel) || !layout) {
    REXLOG_ERROR("{} pipeline for vs {:016X}/{} ps {:016X}/{} not created (vertex shader {}, pixel shader {})", kPrefix, key.vs, key.vs_variant, key.ps,
                 key.ps_variant, ShaderStore::ResultName(vs_result), ShaderStore::ResultName(ps_result));
    return nullptr;
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = guest_layout_->layout();
  desc.vertexShader = vertex;
  desc.pixelShader = pixel;  // null: depth only
  desc.inputSlots = layout->slots.data();
  desc.inputSlotsCount = uint32_t(layout->slots.size());
  desc.inputElements = layout->elements.data();
  desc.inputElementsCount = uint32_t(layout->elements.size());
  desc.primitiveTopology = plume::RenderPrimitiveTopology(key.topology);
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(1));
  desc.renderTargetCount = 0;  // no colour target: the alpha tested draws' pixel shader is cut down to the alpha test by the driver
  desc.depthTargetFormat = kDepthFormat;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = true;
  desc.depthFunction = plume::RenderComparisonFunction::LESS;
  desc.cullMode = HostCull(key.cull);
  desc.frontFace = key.front_ccw ? plume::RenderFrontFace::COUNTER_CLOCKWISE : plume::RenderFrontFace::CLOCKWISE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  // Clipped, not clamped: the near plane drops what is within 1.5 m of the lamps (the lamps' own car), the far plane what is beyond their range.
  desc.depthClipEnabled = true;
  desc.alphaToCoverageEnabled = false;  // the shader's (contract v6)
  desc.dynamicDepthBiasEnabled = false;
  desc.depthBias = 0;
  desc.slopeScaledDepthBias = rules::kSlopeScaledDepthBias;
  const std::string name = std::format("local shadow vs {:016X}/{} layout {} {}{}", key.vs, key.vs_variant, key.layout, key.ps != 0 ? "alpha " : "",
                                       key.cull == uint8_t(rules::Cull::kFront) ? "cull front" : key.cull == uint8_t(rules::Cull::kBack) ? "cull back" : "two sided");
  return device_->CreateGraphicsPipeline(desc, name.c_str());
}

uint32_t LocalShadow::PipelineFor(const MainDraw& draw, const rules::PipeKey& key) {
  const auto found = pipe_ids_.find(key);
  if (found != pipe_ids_.end()) {
    return found->second;
  }
  if (pipes_.size() >= rules::kMaxPipelines) {
    if (!pipe_cap_logged_) {
      pipe_cap_logged_ = true;
      REXLOG_WARN("{} {} pipelines of the game's vertex shaders are listed already: no new shape is listed (a draw of a shape never seen is left out of the maps)", kPrefix,
                  pipes_.size());
    }
    return rules::kNoPipeline;
  }
  const uint32_t id = uint32_t(pipes_.size());
  PipeEntry entry;
  entry.key = key;
  entry.vs = draw.vs;
  entry.ps = key.ps != 0 ? draw.ps : nullptr;
  entry.layout = draw.input_layout;
  entry.queued = true;
  pipes_.push_back(std::move(entry));
  pipe_ids_.emplace(key, id);
  to_list_.push_back(id);
  return id;
}

void LocalShadow::FlushListing() {
  if (to_list_.empty() || frame_ < next_flush_ || !passes_) {
    return;
  }
  std::vector<HostPrecache::Item> fresh;
  fresh.reserve(to_list_.size());
  for (uint32_t id : to_list_) {
    PipeEntry& e = pipes_[id];
    e.flushed = true;
    const rules::PipeKey key = e.key;
    const ShaderStore::Entry* vs = e.vs;
    const ShaderStore::Entry* ps = e.ps;
    const InputLayout* layout = e.layout;
    fresh.push_back({PrecacheKey(id), std::format("local shadow {}", id), [this, key, vs, ps, layout] { return MakePipeline(key, vs, ps, layout); }});
  }
  const size_t count = to_list_.size();
  to_list_.clear();
  next_flush_ = frame_ + kFlushGapFrames;
  const uint32_t started = passes_->precache().AddLive("local shadow", std::move(fresh));
  if (frame_ < 3600 || started != count) {
    REXLOG_INFO("{} {} pipelines of the game's vertex shaders listed for the background builder ({} new); {} known in all", kPrefix, count, started,
                pipes_.size());
  }
}

plume::RenderPipeline* LocalShadow::Pipeline(uint32_t i, bool* pending) {
  PipeEntry& e = pipes_[i];
  if (e.pipeline) {
    return e.pipeline.get();
  }
  if (e.failed) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!e.flushed || !precache.Ready(PrecacheKey(i))) {
    *pending = true;  // not handed to the builder yet, or being made: never made here on the render thread
    return nullptr;
  }
  // Ready: made, failed, or taken. A pipeline the builder made is taken with no wait; Acquire's own creation is for a key nobody listed, which
  // this one is not.
  e.pipeline = precache.Acquire(PrecacheKey(i), std::format("local shadow {}", i), [this, key = e.key, vs = e.vs, ps = e.ps, layout = e.layout] {
    return MakePipeline(key, vs, ps, layout);
  });
  if (!e.pipeline) {
    e.failed = true;
    return nullptr;
  }
  return e.pipeline.get();
}

// ---------------------------------------------------------------------------
// the atlas
// ---------------------------------------------------------------------------

uint64_t LocalShadow::HeldBytes() const {
  return (atlas_.texture ? TextureBytes(atlas_.width, atlas_.height, 1, kDepthFormat) : 0) + (shadow_atlas_ != nullptr ? shadow_atlas_->HeldBytes() : 0);
}

void LocalShadow::SetShadowAtlas(ShadowAtlas* atlas) {
  if (shadow_atlas_ != nullptr) {
    shadow_atlas_->RemoveProvider(this);
  }
  shadow_atlas_ = atlas;
  if (shadow_atlas_ != nullptr) {
    shadow_atlas_->AddProvider(this);
  }
}

// The atlas the frame records into: the shadow atlas object with the flip (LU-5), else the replay's own.
uint32_t LocalShadow::AtlasSlot() const { return use_atlas_ && shadow_atlas_ != nullptr ? shadow_atlas_->slot() : atlas_.slot; }
uint32_t LocalShadow::AtlasWidth() const { return use_atlas_ && shadow_atlas_ != nullptr ? shadow_atlas_->width() : atlas_.width; }
uint32_t LocalShadow::AtlasHeight() const { return use_atlas_ && shadow_atlas_ != nullptr ? shadow_atlas_->height() : atlas_.height; }

// ShadowProvider: the replay's pool is one tile size, as many tiles as the level's views (local_shadow_rules.h AtlasFor: the same numbers). Nothing while the setting is off.
void LocalShadow::RequestPools(shadow_atlas::AtlasSpec* spec) const {
  if (!wanted_ || choice_.level == rules::Level::kOff) {
    return;
  }
  spec->Add({shadow_atlas::ProviderId::kReplay, shadow_atlas::PoolRole::kSingle, rules::TileSize(choice_.level), rules::MaxViews(choice_.level, views_override_), false});
}

void LocalShadow::OnLayout(const shadow_atlas::Layout& layout) {
  atlas_layout_ = layout;
  replay_pool_ = layout.FindPool(shadow_atlas::ProviderId::kReplay, shadow_atlas::PoolRole::kSingle);
}

// ShadowProvider: the views of the last map, each with the id of its light (a car's lamp id from the car's key: light_id.h; the rig's index named it in the map), kSingle: one entry for each
// headlamp's view or the car's. Nothing without a published map.
void LocalShadow::CollectViews(shadow_atlas::ShadowFrameBuilder* builder) const {
  if (!atlas_info_.valid) {
    return;
  }
  for (uint32_t v = 0; v < published_.count && v < published_.views.size(); ++v) {
    const larecomp::gpu::LocalShadowView& view = published_.views[v];
    const uint64_t id = view.light_index < published_ids_.size() ? published_ids_[view.light_index] : larecomp::gpu::kNoLightId;
    builder->AddView(view, id);
  }
}

bool LocalShadow::EnsureAtlas(const rules::AtlasLayout& layout) {
  if (use_atlas_ && shadow_atlas_ != nullptr) {
    return shadow_atlas_->Ensure();  // LU-5: the layout of the pools was planned for this frame (RecordMaps: ShadowAtlas::Plan); the texture is the atlas's
  }
  const uint32_t width = layout.width();
  const uint32_t height = layout.height();
  if (atlas_.texture && atlas_.width == width && atlas_.height == height) {
    return true;
  }
  // Another size (a live change of the setting): the old atlas goes after the lists that name it.
  ReleaseImages();
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, kDepthFormat, plume::RenderTextureFlag::DEPTH_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(1);
  Atlas made;
  made.texture = device_->CreateTexture(desc, "headlight shadow atlas");
  if (made.texture) {
    // The texture's own format: Plume views a depth format through its depth plane (R32_FLOAT_X8X24 on D3D12, the DEPTH aspect on Vulkan).
    plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kDepthFormat);
    view.mipLevels = 1;
    made.view = device_->CreateTextureView(*made.texture, view, "headlight shadow atlas view");
    const plume::RenderTexture* none[1] = {nullptr};
    made.framebuffer = device_->CreateFramebuffer(plume::RenderFramebufferDesc(none, 0, made.texture.get()), "headlight shadow atlas");
  }
  if (made.view) {
    made.slot = heap_->AllocateTexture(*made.texture, *made.view);
  }
  if (!made.texture || !made.view || !made.framebuffer || made.slot == kInvalidBindlessSlot) {
    static bool reported = false;  // render thread only
    if (!reported) {
      reported = true;
      REXLOG_ERROR("{} the {}x{} atlas could not be made (reported once): no headlight shadows while that lasts", kPrefix, width, height);
    }
    if (made.slot != kNoSlot && made.slot != kInvalidBindlessSlot) {
      heap_->ReleaseTexture(made.slot, TextureDimension::k2D);
    }
    return false;
  }
  made.width = width;
  made.height = height;
  made.layout = plume::RenderTextureLayout::UNKNOWN;
  atlas_ = std::move(made);
  return true;
}

void LocalShadow::ReleaseImages() {
  // LU-5: the shared atlas, when the replay recorded into it (it holds nothing otherwise, and then nothing changes here).
  const bool shared = shadow_atlas_ != nullptr && shadow_atlas_->has_texture();
  if (shared) {
    shadow_atlas_->Release();
  }
  if (!device_ || !atlas_.texture) {
    atlas_ = Atlas{};
    if (shared) {
      atlas_info_ = LocalShadowAtlas{};
      published_ = larecomp::gpu::LocalShadowFrame{};
      atlas_logged_ = 0;
      HandOver();
    }
    return;
  }
  if (atlas_.slot != kNoSlot) {
    heap_->ReleaseTexture(atlas_.slot, TextureDimension::k2D);
  }
  frames_->Retire(std::move(atlas_.framebuffer));
  frames_->Retire(std::move(atlas_.view));
  device_->NoteReleased(TextureBytes(atlas_.width, atlas_.height, 1, kDepthFormat));
  frames_->Retire(std::move(atlas_.texture));
  atlas_ = Atlas{};
  atlas_info_ = LocalShadowAtlas{};
  published_ = larecomp::gpu::LocalShadowFrame{};
  atlas_logged_ = 0;  // the next atlas is logged again
  HandOver();
}

// ---------------------------------------------------------------------------
// the headlights' side
// ---------------------------------------------------------------------------

void LocalShadow::HandOver() {
  // The views and the atlas the lit shaders read, as of now: the pointed-to frame changes in place, the rig copies it. The sampler is the fixed
  // point/clamp one (the shader compares four taps itself; a depth plane is not filtered). With no map (no valid atlas or no view) the rig's block
  // has no atlas slot and its shaders skip the lookup (every light is lit), so no fallback texture is read.
  const bool have = atlas_info_.valid && published_.count != 0;
  headlight_rig::SetLocalShadows(have ? &published_ : nullptr, have ? atlas_info_.slot : kInvalidBindlessSlot, kSamplerPointClamp);
  // LU-5: the same maps in the atlas's published form (the views with their lights' ids), for the light manager (mcla_gpu_light_frame bit 8).
  if (use_atlas_ && shadow_atlas_ != nullptr) {
    shadow_atlas_->Collect(kSamplerPointClamp);
  }
}

// The lights of the rig's set as the replay's rules read them: each through MakeLight (finite, a direction that can be normalised, a cone with an angle), named for its place in the set, at
// most four (the groups take four lights: BuildGroups). What PickViews always made; the light manager's frame makes the same ones (light_project.h ProjectShadowLights).
static uint32_t LightsOfRigSet(rules::Light out[4]) {
  const larecomp::gpu::HeadlightSet& set = larecomp::gpu::CurrentHeadlights();
  uint32_t count = 0;
  for (uint32_t i = 0; i < set.count && i < set.lights.size() && count < 4; ++i) {
    const larecomp::gpu::Headlight& h = set.lights[i];
    if (rules::MakeLight(h.pos, h.dir, h.cos_outer, h.range, h.car_key, h.is_player, i, &out[count], h.lamp_offset)) {
      ++count;
    }
  }
  return count;
}

// The frame's lights, and which cars get a view: made once when the frame opens, so that the cone cull (which drops draws no view can see) and the replay
// (which draws into the views) work from the same cars. The set does not change until the next guest present. The hysteresis (local_shadow_rules.h ViewPicker)
// lives across frames.
void LocalShadow::PickViews() {
  frame_light_count_ = 0;
  frame_cars_ = 0;
  frame_views_ = 0;
  frame_plan_ = rules::ViewPlan{};
  // The lights: the rig's set, or (mcla_gpu_light_frame bit 4, LIGHT-UNIFY U4c) the light manager's frame, the same lights bit for bit. With the census on both are made and compared.
  if (light_manager_ != nullptr && light_manager_->Serving(light_project::kFlipShadowLights)) {
    frame_light_count_ = light_manager_->ShadowLights(frame_lights_);
    if (light_manager_->Checking()) {
      rules::Light legacy[4];
      const uint32_t legacy_count = LightsOfRigSet(legacy);
      light_manager_->CheckShadowLights(legacy, legacy_count, frame_lights_, frame_light_count_);
    }
  } else {
    frame_light_count_ = LightsOfRigSet(frame_lights_);
  }
  rules::PickContext context;
  if (last_camera_.valid) {
    // The distance from the camera and whether a car's lamps are on the screen, as the last frame had them.
    context.have_reference = true;
    context.have_screen = true;
    std::memcpy(context.reference, last_camera_.eye, sizeof(context.reference));
    context.view_projection = last_camera_.view_projection;
    // HL-EDGE-FADE: the camera's frustum, so that a car is ranked by how much of its beam is in the view (the rig's ranking, mcla_gpu_headlight_edge, the same switch).
    if (larecomp::settings::mcla_gpu_headlight_edge.Get()) {
      light_influence::MakeViewFrustum(last_camera_.view_projection.m, last_camera_.eye, &context.frustum);
    }
  }
  // The cars that get views (the level's count, the player's first), then the views of each: one for each headlamp of the first cars the level gives lamps to, one for the car
  // for the others (local_shadow_rules.h PlanViews). A car's views come and go together: the picking is of cars, and the plan keeps both of a car's or neither.
  picker_.SetMode(light_pick::ParseMode(light_text::LightPick().Get()));  // the A/B of the one picker (light_pick.h): unified, or the ViewPicker's own first code
  // The budget (light_budget.h): the manager's, noted from this level at the frame's start; the same numbers from the table when no manager's picker is attached.
  const light_budget::LightBudget budget = light_picker_ != nullptr ? light_picker_->budget
                                                                      : light_budget::BudgetFor(true, choice_.level, views_override_, light_budget::Level::kOff, rules::kLevelTable);
  const std::string record_path = light_text::LightPickRecord().Get();
  if (record_path != record_path_) {
    record_path_ = record_path;
    record_file_.close();
    if (!record_path.empty()) {
      record_file_.open(record_path + ".views.txt", std::ios::binary | std::ios::trunc);
      REXLOG_INFO("{} recording the view pick's inputs to {}.views.txt {} (mcla_gpu_light_pick_record; live)", kPrefix, record_path,
                  record_file_.is_open() ? "" : "(the file could not be opened)");
    }
  }
  if (record_file_.is_open()) {
    light_pick::record::ViewFrame recorded;
    recorded.frame = frame_;
    recorded.max_cars = budget.shadow_cars;
    recorded.lights.assign(frame_lights_, frame_lights_ + frame_light_count_);
    recorded.context = context;
    record_file_ << light_pick::record::FormatView(recorded);
  }
  frame_cars_ = picker_.Pick(frame_lights_, frame_light_count_, budget.shadow_cars, context, frame_groups_);
  rules::PlanViews(frame_lights_, frame_groups_, frame_cars_, budget.shadow_lamp_cars, budget.shadow_views, &frame_plan_);
  frame_cars_ = frame_plan_.cars;
  frame_views_ = frame_plan_.count;
  // The log: which cars hold the views, when that changes (the first few times in full, the rest as a count in the census line).
  bool changed = frame_cars_ != last_view_count_;
  for (uint32_t v = 0; v < frame_cars_ && !changed; ++v) {
    changed = frame_groups_[v].car_key != last_view_keys_[v];
  }
  if (changed) {
    ++view_changes_;
    if (view_changes_ <= kViewChangeLogs) {
      std::string cars;
      for (uint32_t v = 0; v < frame_cars_; ++v) {
        cars += std::format("{}{} (key {}, {} view{})", v ? ", " : "", frame_groups_[v].is_player ? "the player's car" : "a car", frame_groups_[v].car_key,
                            frame_plan_.view_count[v], frame_plan_.view_count[v] == 1 ? "" : "s");
      }
      REXLOG_INFO("{} views now: {} view(s) for {} of {} lit car(s), of at most {} car(s) and {} view(s): {} (change {})", kPrefix, frame_views_, frame_cars_, frame_light_count_,
                  budget.shadow_cars, budget.shadow_views, cars.empty() ? std::string("none") : cars, view_changes_);
    }
    last_view_count_ = frame_cars_;
    for (uint32_t v = 0; v < frame_cars_; ++v) {
      last_view_keys_[v] = frame_groups_[v].car_key;
    }
  }
}

// The cone cull's views: one for each car PickViews chose, holding all of the car's views (a car's two lamps are culled together: one test for a draw, not two), as the lights
// alone say them.
void LocalShadow::PlanEarlyViews() {
  early_views_ = 0;
  if (frame_views_ == 0) {
    return;  // no lamp lit: the test light (made at the camera) is not planned here
  }
  for (uint32_t c = 0; c < frame_plan_.cars; ++c) {
    if (!rules::FitCar(frame_plan_, c, &early_specs_[c])) {
      return;  // the replay will refuse this frame too
    }
  }
  early_views_ = frame_plan_.cars;
}

void LocalShadow::EvaluatePlace(const ConstantBanks::Banks& banks) {
  float wvp[16];
  const uint8_t* source = banks.vs.data() + size_t(rules::kWvpRegister) * 16;
  for (uint32_t i = 0; i < 16; ++i) {
    wvp[i] = std::bit_cast<float>(LoadBe32(source + size_t(i) * 4));
  }
  const rules::WvpFacts facts = rules::CheckWvp(wvp, early_camera_);
  eval_have_ = true;
  eval_version_ = banks.vs_version;
  eval_ok_ = facts.ok;
  std::memcpy(eval_origin_, facts.origin, sizeof(eval_origin_));
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void LocalShadow::BeginFrame() {
  ++frame_;
  // LU-5: whether this frame records into the one shadow atlas (mcla_gpu_light_frame bit 8; the manager answers false when it is off or its frame is not open: the replay's own atlas then).
  // A change (live) gives the images of the other code up: one frame without maps, the same as a level change.
  {
    const bool from_atlas = shadow_atlas_ != nullptr && light_manager_ != nullptr && light_manager_->Serving(light_project::kFlipAtlas);
    if (from_atlas != use_atlas_) {
      ReleaseImages();
      use_atlas_ = from_atlas;
      REXLOG_INFO("{} the headlight shadow maps are recorded into {} (mcla_gpu_light_frame bit 8, live)", kPrefix, use_atlas_ ? "the shared shadow atlas (shadow_atlas.h)" : "the replay's own atlas");
    }
  }
  const bool had_trigger = trigger_seen_;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
  casters_.clear();
  banks_.clear();
  shared_.clear();
  have_bank_ = false;
  collected_ = 0;
  collect_cycles_ = 0;
  std::memset(counts_, 0, sizeof(counts_));
  vehicle_census_.Clear();
  early_views_ = 0;
  early_camera_ = rules::Camera{};
  early_tries_ = 0;
  eval_have_ = false;
  if (!had_trigger) {
    // The frame before had no trigger (a menu, a movie): its map is not the world the draws of this frame are of.
    const bool had_map = published_.count != 0 || atlas_info_.valid;
    published_ = larecomp::gpu::LocalShadowFrame{};
    atlas_info_.valid = false;
    if (had_map) {
      HandOver();
    }
  }
  if (!ready_) {
    wanted_ = false;
    offering_ = false;
    collecting_ = false;
    return;
  }
  const gpu_class::GpuClass cls = gpu_class::Current();
  choice_ = rules::Resolve(cls, Setting().Get());
  view_mode_ = int(larecomp::settings::mcla_gpu_headlight_shadow_view.Get());
  skip_mask_ = int(larecomp::settings::mcla_gpu_headlight_shadow_skip.Get());
  cull_policy_ = int(larecomp::settings::mcla_gpu_headlight_shadow_cull.Get());
  views_override_ = int(larecomp::settings::mcla_gpu_headlight_shadow_views.Get());
  test_mode_ = int(larecomp::settings::mcla_gpu_headlight_shadow_test.Get());
  bias_scale_ = float(larecomp::settings::mcla_gpu_headlight_shadow_bias.Get());
  cone_cull_ = int(larecomp::settings::mcla_gpu_headlight_shadow_cone.Get());
  skirt_ = int(larecomp::settings::mcla_gpu_headlight_shadow_skirt.Get());
  wanted_ = choice_.level != rules::Level::kOff;
  // A headlight is lit: the rig's set has one, or the light manager's frame (the same set) when the replay reads it (LU-4c).
  const bool from_frame = light_manager_ != nullptr && light_manager_->Serving(light_project::kFlipShadowLights);
  const uint32_t lit_lights = from_frame ? light_manager_->RigLightCount() : larecomp::gpu::CurrentHeadlights().count;
  if (lit_lights > 0) {
    lit_frames_left_ = kLitGraceFrames;
  } else if (lit_frames_left_ > 0) {
    --lit_frames_left_;
  }
  collecting_ = wanted_ && (lit_frames_left_ > 0 || test_mode_ != 0);
  // LU-5: the level the video memory allows (the atlas is only made when a map is recorded: nothing is asked by day). The plan below is for that level.
  if (use_atlas_ && collecting_) {
    choice_.level = shadow_atlas_->FitLevel(choice_.level, views_override_, frame_);
  }
  // By day (nothing lit) the draws are only looked at to list their pipelines, so that the night's are made: one frame in a few is enough, a shape that is
  // seen late is left out of a map for the moment (pending) and listed then.
  offering_ = wanted_ && (collecting_ || frame_ % kListEveryFrames == 0);
  frame_light_count_ = 0;
  frame_cars_ = 0;
  frame_views_ = 0;
  frame_plan_ = rules::ViewPlan{};
  if (light_picker_ != nullptr) {
    light_picker_->NoteShadows(choice_.level, views_override_, rules::kLevelTable);  // the one budget (light_budget.h): the level resolved to, noted every frame
  }
  if (collecting_) {
    PickViews();
  } else {
    picker_.Forget();  // the lights have been off for a while: the next night's pick starts afresh
  }
  if (collecting_ && cone_cull_ != 0) {
    PlanEarlyViews();
  }
  const uint32_t state = (uint32_t(choice_.level) * 16u + rules::MaxViews(choice_.level, views_override_)) * 4u + uint32_t(views_override_ & 3);
  if (state != last_choice_logged_) {
    last_choice_logged_ = state;
    REXLOG_INFO("{} {} (mcla_gpu_light_shadows = {}, live); {} view(s) at most; collecting only while a headlight is lit", kPrefix,
                rules::Describe(choice_, cls, views_override_), Setting().Get(), rules::MaxViews(choice_.level, views_override_));
  }
  if (wanted_) {
    FlushListing();
  }
  // No map for a while (the setting off, no lit headlight, a menu): the atlas goes back.
  if (idle_frames_ < kReleaseAfterFrames && ++idle_frames_ == kReleaseAfterFrames) {
    ReleaseImages();
  }
  if (wanted_ && frame_ % kCensusFrames == 0) {
    Census();
  }
}

void LocalShadow::OnMainDraw(const MainDraw& d) {
  if (anchor_seen_ || composite_seen_ || trigger_seen_ || !d.key || !d.vs || !d.geometry || !d.banks || !d.shared || !d.input_layout) {
    return;
  }
  const uint64_t began_cycles = cycles::Now();
  Collect(d);
  collect_cycles_ += cycles::Now() - began_cycles;
}

void LocalShadow::Collect(const MainDraw& d) {
  const DrawGeometry& g = *d.geometry;
  rules::DrawFacts f;
  f.color_pass = d.match && d.match.color && d.match.depth && temporal::IsPlaySamples(d.match.samples);
  f.color_count = d.key->color_count;
  f.depth_test = (d.key->depth & 1u) != 0;
  f.depth_write = (d.key->depth & 2u) != 0;
  f.blend0 = d.key->color_count > 0 && UnpackBlend(d.key->blend[0]).blendEnabled;
  f.has_ps = d.ps != nullptr;
  f.ps_writes_depth = d.ps && (d.ps->ps_outputs & ShaderStore::kOutputDepth) != 0;
  f.triangles = g.count != 0 && g.slot_count != 0 && IsTriangles(g.topology) && (!g.indexed || g.index_view.buffer.ref != nullptr);
  f.alpha_tested = d.key->ps_variant == 1;
  f.vs_computed_fetch = (d.vs->flags & ShaderStore::kComputedFetchIndex) != 0;
  f.vs_skinned = VsSkinned(*d.vs);
  f.vehicle = rules::WritesVehicleId(d.stencil_ids);
  f.vs_key = d.vs->key;
  rules::Kind kind = rules::Kind::kOpaque;
  const rules::Skip verdict = rules::ClassifyDraw(f, uint32_t(skip_mask_), &kind);
  // What became of the draw, for the frame's counters and, for a vehicle's part, the census by vertex shader (the log's "vehicle draws" line).
  const bool census = f.vehicle && collecting_;  // by day nothing is stored, and nothing is counted by shader either
  const auto dropped = [&](rules::Skip why) {
    ++counts_[size_t(why)];
    if (census) {
      vehicle_census_.Note(f.vs_key, why);
    }
  };
  ++counts_[size_t(verdict)];
  if (verdict != rules::Skip::kNone) {
    if (census) {
      vehicle_census_.Note(f.vs_key, verdict);
    }
    return;
  }
  rules::PipeKey key;
  key.vs = d.vs->key;
  key.vs_variant = d.key->vs_variant;
  key.layout = d.input_layout->id;
  key.topology = uint32_t(g.topology);
  key.front_ccw = uint8_t((d.key->raster >> 4) & 1u);
  key.cull = uint8_t(rules::ResolveCasterCull(cull_policy_, GameCull(d.key->raster), f.vehicle));
  if (f.alpha_tested && d.ps) {
    key.ps = d.ps->key;
    key.ps_variant = 1;
  }
  // Every shape of draw the game's main view has is listed with the builder while the setting is on, lit headlights or not: by night the
  // pipelines are made.
  const uint32_t pipe = PipelineFor(d, key);
  if (pipe == rules::kNoPipeline) {
    dropped(rules::Skip::kPipeline);
    return;
  }
  if (!collecting_) {
    return;
  }
  if (casters_.capacity() == 0) {
    // The first collecting frame: the frame's stores are made once (about 6 MiB of banks for a night street's 1500 distinct ones, 1 MiB of casters).
    casters_.reserve(2048);
    banks_.reserve(size_t(kBankFloats) * 1536);
    shared_.reserve(64);
  }
  // The cone cull: once the frame's camera is known (the first banks give it, as at the trigger), a draw whose place is outside every view's cone and
  // reach, with its margin, is dropped here, before its bank is copied. The place is c8-c11 x the main view-projection's inverse: 16 floats read, no copy.
  if (early_views_ != 0 && early_camera_.valid) {
    if (!eval_have_ || eval_version_ != d.banks->vs_version) {
      EvaluatePlace(*d.banks);
    }
    if (!eval_ok_) {
      dropped(rules::Skip::kNotWvp);
      return;
    }
    const double reach = rules::CasterReach(f.vehicle);
    bool in_view = false;
    for (uint32_t v = 0; v < early_views_ && !in_view; ++v) {
      in_view = rules::SphereInCarView(early_specs_[v], eval_origin_, reach);
    }
    if (!in_view) {
      dropped(rules::Skip::kOutOfView);
      return;
    }
  }
  Caster c;
  c.pipe = pipe;
  c.kind = uint8_t(kind);
  c.vehicle = f.vehicle;
  c.geometry = g;
  c.layout = d.input_layout;
  c.ps_constants = d.ps_constants;
  // The vertex constants: a host copy of the bank, once per bank version (a draw of the same object shares it).
  if (have_bank_ && d.banks->vs_version == last_bank_version_) {
    c.bank = last_bank_index_;
  } else {
    c.bank = uint32_t(banks_.size() / kBankFloats);
    banks_.resize(banks_.size() + kBankFloats);
    SwapBankToHost(d.banks->vs.data(), reinterpret_cast<uint8_t*>(banks_.data() + size_t(c.bank) * kBankFloats));
    last_bank_version_ = d.banks->vs_version;
    last_bank_index_ = c.bank;
    have_bank_ = true;
    if (early_views_ != 0 && !early_camera_.valid && early_tries_ < kCameraTries) {
      // The same banks, in the same order, the trigger looks at for the camera (OnResolve): the first that makes a usable one.
      ++early_tries_;
      early_camera_ = rules::DeriveCamera(banks_.data() + size_t(c.bank) * kBankFloats);
    }
  }
  // The shared constants: once per distinct block (consecutive draws almost always repeat it).
  if (shared_.empty() || std::memcmp(&shared_.back(), d.shared, sizeof(SharedConstants)) != 0) {
    shared_.push_back(*d.shared);
  }
  c.shared = uint32_t(shared_.size() - 1);
  casters_.push_back(std::move(c));
  ++collected_;
  if (census) {
    vehicle_census_.Note(f.vs_key, rules::Skip::kNone);
  }
}

bool LocalShadow::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (car_shadow_map.cpp IsTrigger, ao.cpp Judge): the first copy of the main depth that is no end-of-scene copy,
  // into 078B0000, before the anchor, in the game view (the pause view's world has no headlights lit by this rig).
  if (!summary.depth_copy || !summary.source || trigger_seen_) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (!temporal::IsPlaySamples(samples)) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth || summary.dest_address != kTriggerDestination || anchor_seen_ || composite_seen_) {
    return false;
  }
  const uint32_t width = std::min(summary.dest_width, summary.source->width);
  const uint32_t height = std::min(summary.dest_height, summary.source->height);
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 && uint32_t(summary.x1) <= width &&
         uint32_t(summary.y1) <= height;
}

// ---------------------------------------------------------------------------
// the replay
// ---------------------------------------------------------------------------

bool LocalShadow::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot) {
  const bool had_trigger = trigger_seen_;
  const bool recorded = RecordMaps(record, summary, list, slot);
  if (trigger_seen_ && !had_trigger) {
    HandOver();  // the views and the atlas of this frame (or none), for the lit draws that follow and the next frame's
  }
  return recorded;
}

bool LocalShadow::RecordMaps(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot) {
  if (!list || !ready_ || !wanted_ || !IsTrigger(record, summary)) {
    return false;
  }
  trigger_seen_ = true;
  ++triggers_;
  // Whatever happens below, the lit draws of the next frame see no map unless one is recorded here.
  published_ = larecomp::gpu::LocalShadowFrame{};
  atlas_info_.valid = false;
  if (!collecting_ || casters_.empty()) {
    return false;
  }
  const auto began = std::chrono::steady_clock::now();
  Plan plan;
  // The main camera of THIS frame, from the first stored bank that carries a usable one.
  const uint32_t bank_count = uint32_t(banks_.size() / kBankFloats);
  for (uint32_t b = 0; b < bank_count && b < kCameraTries && !plan.camera.valid; ++b) {
    plan.camera = rules::DeriveCamera(banks_.data() + size_t(b) * kBankFloats);
  }
  if (!plan.camera.valid) {
    ++no_map_[1];
    if (!camera_logged_) {
      camera_logged_ = true;
      REXLOG_WARN("{} no draw of the frame carries a main camera (view inverse c12-c15, gWorld c0-c3 and gWorldViewProj c8-c11 that make a projection): no map "
                  "(reported once; {} casters, {} banks)",
                  kPrefix, casters_.size(), bank_count);
    }
    return false;
  }
  // The lights and the cars that get a view: the frame's pick (BeginFrame), the same one the cone cull worked from.
  last_camera_ = plan.camera;  // the next frame's pick measures from this one's camera
  for (uint32_t i = 0; i < frame_light_count_; ++i) {
    plan.lights[i] = frame_lights_[i];
  }
  plan.light_count = frame_light_count_;
  rules::ViewPlan view_plan = frame_plan_;
  plan.views = view_plan.count;
  bool synthetic = false;
  if (plan.light_count == 0 && test_mode_ != 0) {
    // mcla_gpu_headlight_shadow_test: a light of its own at the camera (the map is drawn and can be looked at; nothing reads it).
    float pos[3], dir[3];
    for (int k = 0; k < 3; ++k) {
      pos[k] = float(plan.camera.eye[k]);
      dir[k] = float(plan.camera.forward[k]);
    }
    if (rules::MakeLight(pos, dir, float(std::cos(rules::Rad(35.0))), 40.0f, 0xFFFFFFFFu, true, 0, &plan.lights[0])) {
      plan.light_count = 1;
      synthetic = true;
      rules::Group groups[4];
      const uint32_t cars = rules::GroupLights(plan.lights, plan.light_count, 1, groups);
      rules::PlanViews(plan.lights, groups, cars, 0, 1, &view_plan);
      plan.views = view_plan.count;
    }
  }
  if (plan.views == 0) {
    ++no_map_[0];
    return false;
  }
  plan.sx = shared_.front().pos_scale[0] < 0.0f ? -1.0 : 1.0;
  plan.sy = shared_.front().pos_scale[1] < 0.0f ? -1.0 : 1.0;
  plan.atlas = rules::AtlasFor(choice_.level, views_override_);
  if (use_atlas_) {
    // LU-5: the atlas lays out the pools every provider asked for (the replay's is AtlasFor's tiles); the replay's tiles are the pool's. A pool that is not the plan's (it can only be if a
    // provider changed what it asked for between the plan and now) is no map for this frame, never a wrong one.
    const shadow_atlas::Layout& layout = shadow_atlas_->Plan();
    const bool pool_ok = replay_pool_ >= 0 && layout.pools[uint32_t(replay_pool_)].request.tile == plan.atlas.tile && layout.pools[uint32_t(replay_pool_)].request.slots >= plan.atlas.views;
    if (!pool_ok) {
      ++no_map_[3];
      static bool reported = false;  // render thread only
      if (!reported) {
        reported = true;
        REXLOG_WARN("{} the shadow atlas has no pool of {} tile(s) of {}x{} for the replay: no map (reported once)", kPrefix, plan.atlas.views, plan.atlas.tile, plan.atlas.tile);
      }
      return false;
    }
  }
  for (uint32_t v = 0; v < plan.views; ++v) {
    Plan::View& view = plan.view[v];
    view.slot = view_plan.views[v];
    if (!rules::FitView(view.slot.lights.data(), view.slot.group, &view.spec) || !rules::LightClip(view.spec, plan.camera.right_handed, &view.clip) ||
        !rules::ReplayMatrix(plan.camera, view.clip, &view.replay)) {
      ++no_map_[2];
      return false;
    }
    view.skirt_ok = skirt_ != 0;
    if (use_atlas_) {
      const shadow_atlas::Rect r = atlas_layout_.SlotRect(uint32_t(replay_pool_), v);  // the atlas's tile (bit for bit AtlasFor's: shadow_atlas_test.cpp)
      view.tile = rules::TileRect{r.x, r.y, r.size};
      shadow_atlas::UvOf(atlas_layout_, r, view.uv);
    } else {
      view.tile = rules::TileAt(plan.atlas, v);
      rules::UvRect(plan.atlas, v, view.uv);
    }
    plan.lamp_views += view.slot.side != rules::LampSide::kWhole ? 1u : 0u;
  }
  // Each caster's place in the world, from its own gWorldViewProj: also the guard against a vertex shader that is not of this kind.
  for (Caster& c : casters_) {
    const float* wvp = banks_.data() + size_t(c.bank) * kBankFloats + size_t(rules::kWvpRegister) * 4;
    const rules::WvpFacts facts = rules::CheckWvp(wvp, plan.camera);
    c.wvp_ok = facts.ok;
    std::memcpy(c.origin, facts.origin, sizeof(c.origin));
  }
  // The casters each view keeps: the own car and the budget (the near plane and the far plane are the pipeline's clip).
  // WHAT THE VIEWS OF A CAR SHARE. The casters were collected once (Collect: one cone test against the car's own union view, not one for each lamp), and what the draws
  // need to be replayed at all, which is the same for every view, is settled here once: a usable place (c8-c11 is a world matrix), the pixel constants of the guest
  // layout, a pipeline that is made. A view then only tests its own cone and takes its own budget. (Two lamps of a car cost two passes over the draws: the second view's
  // constants differ in c8-c11, a bank cannot be patched, and a viewport that changes at every draw would cost the commands that sharing the binds saves, so a car's two
  // views are drawn one after the other; local_shadow.h says why not one draw into both tiles.)
  uint32_t verdicts[size_t(rules::Skip::kCount)] = {};
  uint32_t shared_not_wvp = 0, shared_unusable = 0, shared_pipeline = 0;
  std::vector<uint32_t> usable;  // the casters every view may draw, in draw order
  usable.reserve(casters_.size());
  bool any_pending = false;
  for (uint32_t i = 0; i < casters_.size(); ++i) {
    const Caster& c = casters_[i];
    if (!c.wvp_ok) {
      ++shared_not_wvp;
      continue;
    }
    if (!c.ps_constants) {
      ++shared_unusable;  // every root descriptor of the guest layout is set for a draw
      continue;
    }
    bool pending = false;
    if (!Pipeline(c.pipe, &pending)) {
      ++shared_pipeline;
      any_pending |= pending;
      continue;
    }
    usable.push_back(i);
  }
  std::vector<rules::Candidate> candidates;
  candidates.reserve(usable.size());
  std::vector<uint32_t> keep(usable.size());
  std::vector<uint32_t> index_of;
  index_of.reserve(usable.size());
  for (uint32_t v = 0; v < plan.views; ++v) {
    Plan::View& view = plan.view[v];
    candidates.clear();
    index_of.clear();
    // The verdicts are counted for each view, as they always were (the log says "per view, once each").
    verdicts[size_t(rules::Skip::kNotWvp)] += shared_not_wvp;
    verdicts[size_t(rules::Skip::kUnusable)] += shared_unusable;
    verdicts[size_t(rules::Skip::kPipeline)] += shared_pipeline;
    for (const uint32_t i : usable) {
      const Caster& c = casters_[i];
      if (cone_cull_ != 0 && !rules::SphereInView(view.spec, c.origin, rules::CasterReach(c.vehicle))) {
        ++verdicts[size_t(rules::Skip::kOutOfView)];  // another view's, or nobody's
        continue;
      }
      rules::Candidate cand;
      cand.index = i;
      cand.vehicle = c.vehicle;
      cand.distance = rules::detail::Distance(c.origin, view.spec.eye);
      candidates.push_back(cand);
      index_of.push_back(i);
    }
    const uint32_t kept = rules::SelectForView(candidates.data(), uint32_t(candidates.size()), rules::kMaxDrawsPerView, keep.data(), verdicts);
    view.keep.assign(kept, 0);
    for (uint32_t k = 0; k < kept; ++k) {
      view.keep[k] = index_of[keep[k]];
    }
  }
  if (any_pending) {
    ++pending_draws_;
    if (!pending_logged_) {
      pending_logged_ = true;
      REXLOG_INFO("{} some pipelines of the game's vertex shaders are still being made in the background: their draws are left out of the map until they are "
                  "(reported once; they are listed from the first frame of the game view while the setting is on)",
                  kPrefix);
    }
  }
  if (!EnsureAtlas(plan.atlas)) {
    ++no_map_[3];
    return false;
  }
  if (atlas_logged_ != AtlasWidth() * 100000u + AtlasHeight()) {
    atlas_logged_ = AtlasWidth() * 100000u + AtlasHeight();
    REXLOG_INFO("{} atlas {}x{} D32 float + S8 ({:.1f} MiB), {} tile(s) of {}x{}{}", kPrefix, AtlasWidth(), AtlasHeight(), rules::Mib(HeldBytes()), plan.atlas.views, plan.atlas.tile,
                plan.atlas.tile, use_atlas_ ? " (the shared shadow atlas: " + shadow_atlas_->Describe() + ")" : std::string());
  }
  if (!RecordPass(plan, list, slot)) {
    ++no_map_[4];
    return false;
  }
  if (view_mode_ == 1) {
    RecordDebugView(summary, list, plan);
  }
  // Publish: one entry for each light a view covers (a car's lights that share a view share its matrix and tile; a lamp's view covers one light, the car's, which has two such
  // views: the shader takes the mean of the two). The test light is nobody's: not published.
  published_ = larecomp::gpu::LocalShadowFrame{};
  uint32_t drawn = 0;
  uint32_t kinds[size_t(rules::Kind::kCount)] = {};
  for (uint32_t v = 0; v < plan.views; ++v) {
    const Plan::View& view = plan.view[v];
    for (uint32_t m = 0; m < view.slot.group.count && published_.count < published_.views.size() && !synthetic; ++m) {
      larecomp::gpu::LocalShadowView& out = published_.views[published_.count++];
      rules::WorldToShadow(view.clip, view.uv, plan.sx, plan.sy, out.world_to_shadow);
      rules::PublishedRect(view.uv, out.atlas_rect);  // u0 v0 u1 v1, what the shader compares a point with
      out.depth_bias = rules::kDepthBias * bias_scale_;
      out.texel_size = 1.0f / float(plan.atlas.tile);
      out.light_index = view.slot.lights[view.slot.group.members[m]].index;
      out.valid = 1;
    }
    drawn += uint32_t(view.keep.size());
    for (uint32_t i : view.keep) {
      ++kinds[casters_[i].kind];
    }
  }
  // The id of the rig's light each view is of (a car's lamp id from the car's key), for the atlas's published form (CollectViews); the lights of the frame the map was made for.
  published_ids_.fill(larecomp::gpu::kNoLightId);
  for (uint32_t i = 0; i < plan.light_count; ++i) {
    if (plan.lights[i].index < published_ids_.size()) {
      published_ids_[plan.lights[i].index] = larecomp::gpu::light_id::CarLampId(plan.lights[i].car_key);
    }
  }
  atlas_info_.valid = published_.count != 0;
  atlas_info_.slot = AtlasSlot();
  atlas_info_.width = AtlasWidth();
  atlas_info_.height = AtlasHeight();
  atlas_info_.tile = plan.atlas.tile;
  ++maps_;
  views_total_ += plan.views;
  lamp_views_total_ += plan.lamp_views;
  draws_total_ += drawn;
  for (size_t k = 0; k < size_t(rules::Skip::kCount); ++k) {
    skipped_total_[k] += verdicts[k] + counts_[k];
  }
  last_views_ = plan.views;
  last_lamp_views_ = plan.lamp_views;
  last_draws_ = drawn;
  last_collected_ = collected_;
  for (size_t k = 0; k < size_t(rules::Kind::kCount); ++k) {
    last_kinds_[k] = kinds[k];
  }
  last_cpu_ns_ = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
  cpu_ns_total_ += last_cpu_ns_;
  last_collect_ns_ = uint64_t(cycles::ToNs(collect_cycles_));
  collect_ns_total_ += last_collect_ns_;
  idle_frames_ = 0;
  // The vehicles' draws by vertex shader (a car is some 200 draws of some 20 shaders): the first frame that has a car, then one line in 1800 frames. It says
  // which parts of a car are in the map and which are left out and why (the picture of 2026-11-20 had only the tyres).
  if (vehicle_census_.draws >= 40 && frame_ >= next_vehicle_log_ && vehicle_logs_ < 8) {
    ++vehicle_logs_;
    next_vehicle_log_ = frame_ + kCensusFrames;
    REXLOG_INFO("{} frame {}: {} vehicle draws by vertex shader (identity: collected, or why not):{}; the skirt copy of a vehicle's part is {}", kPrefix, frame_,
                vehicle_census_.draws, rules::DescribeVehicleCensus(vehicle_census_), skirt_ != 0 ? "on" : "off");
  }
  if (maps_ == 1) {
    // The first map: the line the run plan reads (views, draws replayed, skipped by kind).
    std::string skipped;
    for (size_t k = 1; k < size_t(rules::Skip::kCount); ++k) {
      const uint32_t n = verdicts[k] + counts_[k];
      if (n != 0) {
        skipped += std::format(" {} {},", n, rules::SkipName(rules::Skip(k)));
      }
    }
    REXLOG_INFO("{} first map: {} view(s) ({} from a headlamp) for {} light(s), {} draws replayed of {} collected (opaque {}, alpha tested {}, skinned {}, vehicle {}); skipped:{} "
                "(per view, once each); {} vertex constant banks written ({:.1f} KiB); render thread {:.2f} ms for the pass and {:.2f} ms collecting",
                kPrefix, plan.views, plan.lamp_views, plan.light_count, drawn, collected_, kinds[0], kinds[1], kinds[2], kinds[3], skipped.empty() ? " nothing" : skipped,
                last_banks_, double(last_banks_) * double(guest::kConstantBankBytes) / 1024.0, MilliSeconds(last_cpu_ns_), MilliSeconds(last_collect_ns_));
  }
  return true;
}

bool LocalShadow::RecordPass(Plan& plan, plume::RenderCommandList* list, uint32_t slot) {
  // The shared constants of the frame's blocks, with the pass's NDC transform: the main view's viewport maps are not the atlas tile's. One copy a
  // block, made when a view first needs it.
  std::vector<UploadSpan> shared_spans(shared_.size());
  const auto shared_span = [&](uint32_t i) -> const UploadSpan& {
    UploadSpan& span = shared_spans[i];
    if (!span) {
      span = upload_->Allocate(slot, sizeof(SharedConstants), kConstantAlignment);
      if (span) {
        SharedConstants s = shared_[i];
        s.pos_scale[0] = float(plan.sx);
        s.pos_scale[1] = float(plan.sy);
        s.pos_scale[2] = 1.0f;
        s.pos_scale[3] = 1.0f;
        s.pos_offset[0] = s.pos_offset[1] = s.pos_offset[2] = s.pos_offset[3] = 0.0f;
        // One sample, no 2 x 2 dither: a draw that has alpha to mask on gets the solid offsets (local_shadow_rules.h kAtlasAlphaToMask: the game's pattern would be one missing texel in two).
        s.alpha_to_mask = rules::AtlasAlphaToMask(s.alpha_to_mask);
        std::memcpy(span.cpu, &s, sizeof(s));
      }
    }
    return span;
  };
  pass_timing_->Mark(list, "headlight shadows");
  if (use_atlas_) {
    // LU-5: the shared atlas: the barrier to depth write, the target and the clear are its (the same commands, in the same order, as the lines below).
    if (!shadow_atlas_->BeginRecord(list)) {
      return false;
    }
  } else {
    if (atlas_.layout != plume::RenderTextureLayout::DEPTH_WRITE) {
      const plume::RenderTextureBarrier barrier(atlas_.texture.get(), plume::RenderTextureLayout::DEPTH_WRITE);
      list->barriers(plume::RenderBarrierStage::GRAPHICS, barrier);
      atlas_.layout = plume::RenderTextureLayout::DEPTH_WRITE;
    }
    list->setFramebuffer(atlas_.framebuffer.get());
    list->clearDepthStencil(true, true, 1.0f, 0);
  }
  list->setGraphicsPipelineLayout(guest_layout_->layout());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 1);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 2);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 3);
  const bool d3d12 = ActiveBackend() == Backend::kD3D12;
  const plume::RenderPipeline* bound_pipeline = nullptr;
  plume::RenderBufferReference bound_constants[3];
  bool constants_bound = false;
  uint32_t bound_slot_count = 0;
  std::array<plume::RenderVertexBufferView, DrawGeometry::kMaxSlots> bound_views{};
  const InputLayout* bound_layout = nullptr;
  plume::RenderIndexBufferView bound_index;
  bool index_bound = false;
  std::vector<UploadSpan> bank_spans;
  std::vector<UploadSpan> skirt_spans;
  // What a view draws: each kept caster, and for a vehicle's part its skirt copy right after it (pass 1, local_shadow_rules.h "the ground skirt").
  struct DrawItem {
    uint32_t index;
    uint32_t pass;
  };
  std::vector<DrawItem> items;
  float rows[16];
  bool upload_failed = false;
  uint32_t banks_written = 0;
  for (uint32_t v = 0; v < plan.views; ++v) {
    Plan::View& view = plan.view[v];
    if (use_atlas_) {
      shadow_atlas_->SetTile(list, shadow_atlas::Rect{view.tile.x, view.tile.y, view.tile.size});  // the tile's viewport and scissor (the lines below, in the atlas)
    } else {
      const plume::RenderViewport viewport(float(view.tile.x), float(view.tile.y), float(view.tile.size), float(view.tile.size), 0.0f, 1.0f);
      list->setViewports(&viewport, 1);
      const plume::RenderRect scissor(int32_t(view.tile.x), int32_t(view.tile.y), int32_t(view.tile.x + view.tile.size), int32_t(view.tile.y + view.tile.size));
      list->setScissors(&scissor, 1);
    }
    // The replaced vertex constants of a bank version (and of its skirt copy): made when the view first draws a caster that has it.
    bank_spans.assign(banks_.size() / kBankFloats, UploadSpan{});
    skirt_spans.assign(view.skirt_ok ? banks_.size() / kBankFloats : 0, UploadSpan{});
    items.clear();
    items.reserve(view.keep.size() + view.keep.size() / 4);
    for (uint32_t index : view.keep) {
      items.push_back({index, 0});
      if (view.skirt_ok && casters_[index].vehicle) {
        items.push_back({index, 1});
      }
    }
    for (const DrawItem& item : items) {
      const Caster& c = casters_[item.index];
      UploadSpan& vs_span = item.pass == 0 ? bank_spans[c.bank] : skirt_spans[c.bank];
      if (!vs_span) {
        vs_span = upload_->Allocate(slot, guest::kConstantBankBytes, kConstantAlignment);
        if (!vs_span) {
          upload_failed = true;
          break;
        }
        const float* bank = banks_.data() + size_t(c.bank) * kBankFloats;
        // The skirt's copy of a bank: moved along the way the light travels to this car, not along the view's axis (a bank's draws share their place).
        tp::Mat4 skirt;
        const tp::Mat4* replay = &view.replay;
        if (item.pass != 0) {
          double dir[3], shift[3];
          rules::SkirtDirection(view.spec.eye, c.origin, view.spec.dir, dir);
          rules::SkirtShift(dir, shift);
          if (rules::SkirtReplayMatrix(plan.camera, view.clip, shift, &skirt)) {
            replay = &skirt;
          }
        }
        rules::ReplaceWvp(bank + size_t(rules::kWvpRegister) * 4, *replay, rows);
        // The bank in three pieces around c8-c11, each byte written once (the span is write-combined memory: a second write to the same line costs).
        constexpr size_t kBefore = size_t(rules::kWvpRegister) * 16;
        constexpr size_t kAfter = kBefore + sizeof(float) * 16;
        std::memcpy(vs_span.cpu, bank, kBefore);
        std::memcpy(vs_span.cpu + kBefore, rows, sizeof(rows));
        std::memcpy(vs_span.cpu + kAfter, reinterpret_cast<const uint8_t*>(bank) + kAfter, guest::kConstantBankBytes - kAfter);
        ++banks_written;
      }
      const UploadSpan& shared = shared_span(c.shared);
      if (!shared) {
        upload_failed = true;
        break;
      }
      bool pending = false;
      plume::RenderPipeline* pipeline = Pipeline(c.pipe, &pending);
      if (!pipeline) {
        continue;  // checked when the views were planned; made nowhere else
      }
      if (bound_pipeline != pipeline) {
        list->setPipeline(pipeline);
        bound_pipeline = pipeline;
      }
      const plume::RenderBufferReference constants[3] = {vs_span.reference(), c.ps_constants.reference(), shared.reference()};
      bool changed = !constants_bound;
      for (uint32_t k = 0; k < 3 && !changed; ++k) {
        changed = constants[k].ref != bound_constants[k].ref || constants[k].offset != bound_constants[k].offset;
      }
      if (changed) {
        if (d3d12) {
          static constexpr uint32_t kRoots[3] = {GuestLayout::kVertexConstants, GuestLayout::kPixelConstants, GuestLayout::kSharedConstants};
          for (uint32_t k = 0; k < 3; ++k) {
            if (!constants_bound || constants[k].ref != bound_constants[k].ref || constants[k].offset != bound_constants[k].offset) {
              list->setGraphicsRootDescriptor(constants[k], kRoots[k]);
            }
          }
        } else {
          const uint64_t addresses[3] = {vs_span.device_address, c.ps_constants.device_address, shared.device_address};
          list->setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
        }
        for (uint32_t k = 0; k < 3; ++k) {
          bound_constants[k] = constants[k];
        }
        constants_bound = true;
      }
      const DrawGeometry& g = c.geometry;
      if (g.slot_count && (bound_layout != c.layout || bound_slot_count != g.slot_count || !SameViews(bound_views.data(), g.vertex_views.data(), g.slot_count))) {
        list->setVertexBuffers(0, g.vertex_views.data(), g.slot_count, c.layout->slots.data());
        bound_slot_count = g.slot_count;
        bound_layout = c.layout;
        for (uint32_t k = 0; k < g.slot_count; ++k) {
          bound_views[k] = g.vertex_views[k];
        }
      }
      if (g.indexed) {
        if (!index_bound || bound_index.buffer.ref != g.index_view.buffer.ref || bound_index.buffer.offset != g.index_view.buffer.offset ||
            bound_index.size != g.index_view.size || bound_index.format != g.index_view.format) {
          list->setIndexBuffer(&g.index_view);
          bound_index = g.index_view;
          index_bound = true;
        }
        list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
      } else {
        list->drawInstanced(g.count, 1, g.first, 0);
      }
    }
    if (upload_failed) {
      break;
    }
  }
  if (!use_atlas_) {
    list->setFramebuffer(nullptr);
  }
  last_banks_ = banks_written;
  banks_total_ += banks_written;
  // The lit draws of the next frame (and the translucent ones after this) read it as a texture, and the volumetric light's inject (a compute pass, later in
  // this frame: volumetrics_headlights.h) reads it too: both stages (D3D12: ALL_SHADER_RESOURCE, a compute shader may not read PIXEL_SHADER_RESOURCE;
  // Vulkan: fragment and compute). The pixel readers are unchanged.
  if (use_atlas_) {
    shadow_atlas_->EndRecord(list);  // LU-5: leaves the target and makes THE ONE barrier: shader read for the pixel and the compute stage (shadow_atlas_rules.h UseTracker)
  } else {
    const plume::RenderTextureBarrier read(atlas_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), read);
    atlas_.layout = plume::RenderTextureLayout::SHADER_READ;
  }
  if (upload_failed) {
    static bool reported = false;  // render thread only
    if (!reported) {
      reported = true;
      REXLOG_ERROR("{} the upload ring had no room for the replay's constants: the map of this frame is partial (reported once)", kPrefix);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// the debug view
// ---------------------------------------------------------------------------

plume::RenderPipeline* LocalShadow::DebugPipeline(uint32_t host_samples) {
  const auto found = view_pipelines_.find(host_samples);
  if (found != view_pipelines_.end()) {
    return found->second.get();
  }
  if (view_failed_.count(host_samples) != 0) {
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
    made = passes_->CreatePipeline(passes_->layout(), *view_shader_, plume::RenderFormat::R16G16B16A16_FLOAT, blend, host_samples, "local shadow view");
  }
  if (!made) {
    view_failed_.insert(host_samples);
    return nullptr;
  }
  return (view_pipelines_[host_samples] = std::move(made)).get();
}

bool LocalShadow::RecordDebugView(const ResolveCache::Summary& summary, plume::RenderCommandList* list, const Plan& plan) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (car_shadow_map.cpp RecordView).
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
  plume::RenderPipeline* pipeline = DebugPipeline(depth.host_samples);
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
  // A rectangle of the atlas' aspect in the lower right corner, a third of the picture's height at most, 16 pixels from the edges.
  const uint32_t render_w = depth.scale.Edge(std::min(summary.dest_width, depth.width));
  const uint32_t render_h = depth.scale.Edge(std::min(summary.dest_height, depth.height));
  const float aspect = float(AtlasWidth()) / float(std::max(1u, AtlasHeight()));
  float h = std::clamp(float(render_h) / 3.0f, 96.0f, 512.0f);
  float w = h * aspect;
  const float max_w = std::max(32.0f, float(render_w) * 0.5f);
  if (w > max_w) {
    w = max_w;
    h = w / aspect;
  }
  const float x = std::max(0.0f, float(render_w) - w - 16.0f);
  const float y = std::max(0.0f, float(render_h) - h - 16.0f);
  struct ViewPush {
    uint32_t slots[4];
    float rect[4];
    float depth[4];
  } push = {{AtlasSlot(), AtlasWidth(), AtlasHeight(), 0}, {x, y, w, h}, {float(plan.view[0].spec.near_m), float(plan.view[0].spec.far_m), float(plan.atlas.tile), 0.0f}};
  static_assert(sizeof(ViewPush) <= HostPasses::kPushConstantBytes);
  pass_timing_->Mark(list, "headlight shadow view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{x, y, w, h};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the log
// ---------------------------------------------------------------------------

void LocalShadow::Census() {
  std::string skipped;
  for (size_t k = 1; k < size_t(rules::Skip::kCount); ++k) {
    if (skipped_total_[k] != 0) {
      skipped += std::format(" {} {},", skipped_total_[k], rules::SkipName(rules::Skip(k)));
    }
  }
  REXLOG_INFO("{} frame {}: {} ({}x{}); since the start {} triggers, {} maps ({} views, {} from a headlamp, {} draws replayed, the cars holding the views changed {} times; the last map: {} view(s) ({} from a headlamp), {} draws of {} collected "
              "(opaque {}, alpha tested {}, skinned {}, vehicle {}), {} banks written, render thread {:.2f} ms for the pass and {:.2f} ms collecting; {:.2f} + {:.2f} ms a map on average, {:.0f} banks); no map: {} no lamp, {} no camera, {} view, "
              "{} atlas, {} pass; {} frames with pipelines still being made; {} pipelines; skipped (all frames, all views):{} ; {:.1f} MiB",
              kPrefix, frame_, rules::Describe(choice_, gpu_class::Current(), views_override_), AtlasWidth(), AtlasHeight(), triggers_, maps_, views_total_, lamp_views_total_,
              draws_total_, view_changes_, last_views_, last_lamp_views_, last_draws_, last_collected_, last_kinds_[0], last_kinds_[1], last_kinds_[2], last_kinds_[3], last_banks_, MilliSeconds(last_cpu_ns_),
              MilliSeconds(last_collect_ns_), maps_ ? MilliSeconds(cpu_ns_total_) / double(maps_) : 0.0,
              maps_ ? MilliSeconds(collect_ns_total_) / double(maps_) : 0.0, maps_ ? double(banks_total_) / double(maps_) : 0.0, no_map_[0], no_map_[1], no_map_[2], no_map_[3], no_map_[4], pending_draws_,
              pipes_.size(), skipped.empty() ? " nothing" : skipped, rules::Mib(HeldBytes()));
}

}  // namespace mcla::gpu
