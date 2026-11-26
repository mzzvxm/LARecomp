// MCLA GPU renderer -- the probe update. See gi_update.h.

#include "gi_update.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "city_hit_scene.h"
#include "city_rt_rules.h"
#include "city_rt_scene.h"
#include "city_texture_scope.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gi_setting.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "rt_world_frame.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu {

namespace {

namespace rules = gi_update;

constexpr const char* kPrefix = "[rt gi]";
constexpr plume::RenderBufferFlags kStorageFlags = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS;
// The pass set's views: rows, probe list, rays, irradiance, moments, state, counters (gi_update_rules.h kSetViews; D3D12 sizes a set by this count, not by the ranges).
constexpr uint32_t kSetViews = rules::kSetViews;
// Under this the structures stand too near for the field to mean anything (gi_field.cpp kMinSafeRangeM).
constexpr uint32_t kUploadAlign = 256;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

gi::Vec3 VectorOf(const float (&v)[3]) { return {v[0], v[1], v[2]}; }

void CollectProbeLights(const RtWorldFrame::Current& world, std::vector<rules::ProbeLight>& lights) {
  lights.clear();
  if (world.lamps) {
    for (const auto& lamp : world.lamps->probe_lamps) {
      lights.push_back({lamp.key, VectorOf(lamp.position), lamp.reach});
    }
  }
  // The static prefix feeds the probe shader. Moving lights use LT-6's
  // per-frame virtual bounce and must not invalidate the slow field.
  const uint32_t base = world.decoded.emitter_row_base;
  constexpr uint32_t rows = rt_world::kEmitterRows;
  if (world.lamp_words && base <= world.lamp_rows) {
    const uint32_t count = std::min(world.decoded.emitter_static, (world.lamp_rows - base) / rows);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t* words = world.lamp_words + (base + i * rows) * 4u;
      uint64_t key = 14695981039346656037ull;
      // Ignore the surface shadow-atlas metadata in the last two words.
      for (uint32_t j = 0; j < rows * 4u - 2u; ++j) key = (key ^ words[j]) * 1099511628211ull;
      lights.push_back({key, {rt_hit::FromBits(words[0]), rt_hit::FromBits(words[1]), rt_hit::FromBits(words[2])}, rt_hit::FromBits(words[3])});
    }
  }
  std::sort(lights.begin(), lights.end());
}

}  // namespace

GiUpdate::~GiUpdate() = default;

const char* GiUpdate::VariantName(uint32_t variant) { return variant == kVTrace ? "gi update trace" : "gi update blend"; }

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void GiUpdate::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, GiField& field, CarRtScene& car_scene, CityRtScene& city_scene,
                          CityHitScene& hit_scene, RtSceneService& service, RtWorldFrame& world, bool listed_at_start) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  field_ = &field;
  car_scene_ = &car_scene;
  city_scene_ = &city_scene;
  hit_scene_ = &hit_scene;
  service_ = &service;
  world_ = &world;
  ready_ = false;
  listed_ = false;
  pipeline_failed_ = {};
  const bool supported = field.available() && car_scene.ready() && city_scene.initialized() && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops());
  if (!supported) {
    passes.precache().Note("ray traced bounce light's probe update: not on this graphics card or backend (the screen-space bounce light stays the source)", 0);
    return;
  }
  if (!MakeLayoutsAndShaders(car_scene)) {
    passes.precache().Note("ray traced bounce light's probe update: a layout or a shader could not be made (the screen-space bounce light stays the source)", 0);
    return;
  }
  stats_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(rules::kStatBytes, kStorageFlags), "gi update counters");
  readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(rules::kStatBytes), "gi update counters readback");
  if (!stats_ || !readback_) {
    passes.precache().Note("ray traced bounce light's probe update: the counters could not be made (the screen-space bounce light stays the source)", 0);
    stats_.reset();
    readback_.reset();
    return;
  }
  ready_ = true;
  if (listed_at_start) {
    uint32_t added = 0;
    for (uint32_t v = 0; v < kVariantCount; ++v) {
      added += passes.precache().Add(HostPrecache::Key(HostPrecache::kGiUpdate, v), VariantName(v), [this, v] { return MakePipeline(v); }) ? 1u : 0u;
    }
    listed_ = true;
    passes.precache().Note("ray traced bounce light's probe update: on where the setting says (auto: a strong ray tracing card)", added);
  } else {
    passes.precache().Note("ray traced bounce light's probe update: made when first wanted", 0);
  }
}

bool GiUpdate::MakeLayoutsAndShaders(CarRtScene& scene) {
  // THE PASS SET: rows, probe list, rays, the two atlases as storage images, the state words, the counters (registers unique inside the set: gi_update_rules.h).
  pass_set_builder_.begin();
  rows_index_ = pass_set_builder_.addByteAddressBuffer(rules::kBindRows);
  probes_index_ = pass_set_builder_.addByteAddressBuffer(rules::kBindProbes);
  rays_index_ = pass_set_builder_.addReadWriteByteAddressBuffer(rules::kBindRays);
  irradiance_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindIrradiance);
  moments_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindMoments);
  state_index_ = pass_set_builder_.addReadWriteByteAddressBuffer(rules::kBindState);
  stats_index_ = pass_set_builder_.addReadWriteByteAddressBuffer(rules::kBindStats);
  pass_set_builder_.end();

  // The trace: the bindless textures (the field's three, read by their slots), the samplers, the pass set, the ray tracing set, the push block.
  plume::RenderPipelineLayoutBuilder trace;
  trace.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = trace.addDescriptorSet(heap_->texture_set_builder());
  const uint32_t sampler_set = trace.addDescriptorSet(heap_->sampler_set_builder());
  const uint32_t pass_set = trace.addDescriptorSet(pass_set_builder_);
  const uint32_t scene_set = trace.addDescriptorSet(scene.SetDesc());
  trace.addPushConstant(0, rules::kTracePushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  trace.end();
  if (texture_set != rules::kTraceTextureSet || sampler_set != rules::kTraceSamplerSet || pass_set != rules::kTracePassSet || scene_set != rules::kTraceSceneSet) {
    REXLOG_ERROR("{} the trace layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  trace_layout_ = device_->CreatePipelineLayout(trace, "gi update trace layout");

  // The blend: the pass set alone and the push block.
  plume::RenderPipelineLayoutBuilder blend;
  blend.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t blend_set = blend.addDescriptorSet(pass_set_builder_);
  blend.addPushConstant(0, rules::kBlendPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  blend.end();
  if (blend_set != rules::kBlendPassSet) {
    REXLOG_ERROR("{} the blend layout's set is not the one the shader declares", kPrefix);
    return false;
  }
  blend_layout_ = device_->CreatePipelineLayout(blend, "gi update blend layout");
  if (!trace_layout_ || !blend_layout_) {
    return false;
  }
  const HostShaderBlob trace_blob = GiUpdateTraceShader();
  const HostShaderBlob blend_blob = GiUpdateBlendShader();
  trace_shader_ = passes_->CreateShader(trace_blob.data, trace_blob.size, trace_blob.name);
  blend_shader_ = passes_->CreateShader(blend_blob.data, blend_blob.size, blend_blob.name);
  return trace_shader_ != nullptr && blend_shader_ != nullptr;
}

void GiUpdate::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();  // the host builder's functions name this object's layouts and shaders: stop it first
  }
  // The GPU is idle: nothing is retired, the objects just go.
  for (Slot& s : slots_) {
    s = Slot{};
  }
  rays_.reset();
  ray_bytes_ = 0;
  stats_.reset();
  readback_.reset();
  readback_busy_ = false;
  for (auto& p : pipelines_) {
    p.reset();
  }
  trace_shader_.reset();
  blend_shader_.reset();
  trace_layout_.reset();
  blend_layout_.reset();
  ready_ = false;
  ++generation_;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  field_ = nullptr;
  car_scene_ = nullptr;
  city_scene_ = nullptr;
  hit_scene_ = nullptr;
  service_ = nullptr;
  world_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> GiUpdate::MakePipeline(uint32_t variant) {
  const char* name = VariantName(variant);
  if (variant == kVTrace) {
    return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(trace_layout_.get(), trace_shader_.get(), rules::kTraceGroup, 1, 1), name);
  }
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(blend_layout_.get(), blend_shader_.get(), rules::kBlendGroupSize, rules::kBlendGroupSize, 1), name);
}

plume::RenderPipeline* GiUpdate::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  pipelines_[variant] = passes_->precache().Acquire(HostPrecache::Key(HostPrecache::kGiUpdate, variant), VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no probe update this session (the field stays empty and the screen-space bounce light stays the source)", kPrefix, VariantName(variant));
  }
  return pipelines_[variant].get();
}

// True once both pipelines are made (listed in the background when the setting was off at the start); false while they are being made or when one failed.
bool GiUpdate::PipelinesReady() {
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t v = 0; v < kVariantCount; ++v) {
      fresh.push_back({HostPrecache::Key(HostPrecache::kGiUpdate, v), VariantName(v), [this, v] { return MakePipeline(v); }});
    }
    listed_ = true;
    precache.AddLive("ray traced bounce light's probe update", std::move(fresh));
    REXLOG_INFO("{} the update was off at the start: its two compute pipelines are made in the background, and it starts from the frame they are done", kPrefix);
  }
  for (uint32_t v = 0; v < kVariantCount; ++v) {
    if (pipeline_failed_[v]) {
      return false;
    }
    if (!pipelines_[v] && !precache.Ready(HostPrecache::Key(HostPrecache::kGiUpdate, v))) {
      return false;
    }
  }
  return Pipeline(kVTrace) != nullptr && Pipeline(kVBlend) != nullptr;
}

// ---------------------------------------------------------------------------
// the buffers and the sets
// ---------------------------------------------------------------------------

void GiUpdate::RetireSets() {
  for (Slot& s : slots_) {
    if (frames_) {
      frames_->Retire(std::move(s.set));
    }
    s.set.reset();
    s.set_key = 0;
  }
}

void GiUpdate::ReleaseBuffers() {
  if (rays_ && frames_) {
    frames_->Retire(std::move(rays_));
  }
  rays_.reset();
  ray_bytes_ = 0;
  RetireSets();
  ++ray_serial_;
}

bool GiUpdate::EnsureBuffers(const rules::Budget& budget) {
  if (rays_ && ray_bytes_ == budget.ray_buffer_bytes) {
    return true;
  }
  ReleaseBuffers();
  rays_ = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(budget.ray_buffer_bytes, kStorageFlags), "gi update rays");
  if (!rays_) {
    return false;
  }
  ray_bytes_ = budget.ray_buffer_bytes;
  buffers_for_ = budget;
  REXLOG_INFO("{} the ray buffer: {} probes of {} rays, {:.1f} MiB (the field's update reserve, gi_field_rules.h); a frame's plan runs in up to {} chunks", kPrefix, budget.chunk_probes, budget.rays_per_probe,
              double(budget.ray_buffer_bytes) / (1024.0 * 1024.0), rules::kMaxChunks);
  return true;
}

uint64_t GiUpdate::SetKey() const { return (uint64_t(field_->generation()) << 32) | uint64_t(ray_serial_); }

bool GiUpdate::EnsureSlot(uint32_t frame_slot, const rules::Budget& budget) {
  Slot& s = slots_[frame_slot];
  if (!s.rows) {
    s.rows = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(rules::kUploadBytes, plume::RenderBufferFlag::STORAGE), "gi update rows");
    if (!s.rows) {
      return false;
    }
  }
  const uint64_t list_bytes = AlignUp(std::max<uint64_t>(budget.list_bytes, 16), kUploadAlign);
  if (!s.list || s.list_bytes != list_bytes) {
    if (s.list && frames_) {
      frames_->Retire(std::move(s.list));
      frames_->Retire(std::move(s.set));
      s.set_key = 0;
    }
    s.list = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(list_bytes, plume::RenderBufferFlag::STORAGE), "gi update probe list");
    if (!s.list) {
      s.list_bytes = 0;
      return false;
    }
    s.list_bytes = list_bytes;
  }
  const uint64_t key = SetKey();
  if (!s.set || s.set_key != key) {
    if (frames_) {
      frames_->Retire(std::move(s.set));
    }
    s.set = device_->CreateDescriptorSet(pass_set_builder_, kSetViews, 0, "gi update set");  // seven views: D3D12 sizes the set by this count, not by the ranges. CHECK (run): the D3D12 debug layer accepts it
    if (!s.set) {
      return false;
    }
    s.set->setBuffer(rows_index_, s.rows.get(), rules::kUploadBytes);
    s.set->setBuffer(probes_index_, s.list.get(), s.list_bytes);
    s.set->setBuffer(rays_index_, rays_.get(), ray_bytes_);
    s.set->setTexture(irradiance_index_, field_->irradiance().texture.get(), plume::RenderTextureLayout::GENERAL, field_->irradiance().view.get());
    s.set->setTexture(moments_index_, field_->moments().texture.get(), plume::RenderTextureLayout::GENERAL, field_->moments().view.get());
    s.set->setBuffer(state_index_, field_->state_buffer(), field_->state_bytes());
    s.set->setBuffer(stats_index_, stats_.get(), rules::kStatBytes);
    s.set_key = key;
  }
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void GiUpdate::OnWant(gi::Level level) {
  if (world_ != nullptr) {
    // GI-BOUNCE-RT: the cars' lamps (neon, headlights, tail lamps ...) are lit surfaces' light too: the world frame makes the emitters while the bounce light runs, at the tier its level and rung say.
    world_->WantEmitters(ready_ && service_ != nullptr && level != gi::Level::kOff, level == gi::Level::kUltra, false);
  }
  if (!ready_ || !service_ || level == gi::Level::kOff) {
    return;
  }
  // The field needs the city's structures (its rays, its safe range) and the hit data (the hit shading: material, class, average colour). No textures (GI mode shades with the average colour), no cars (they
  // receive the field's light, they are not in its rays; the sun and lamp rays of the hit shading see whatever else put them in the top level).
  rt_scene::Needs needs;
  needs.city = level == gi::Level::kUltra ? city_rt::Level::kUltra : city_rt::Level::kHigh;
  needs.hit = needs.city;
  needs.textures = TextureScope::kNone;
  service_->Want(rt_scene::Consumer::kCityGi, needs);
}

void GiUpdate::DrainSectorChanges(bool scheduled) {
  if (!city_scene_ || !field_) return;
  std::vector<CityRtScene::ChangedBox> changes;
  uint32_t dropped = 0;
  city_scene_->DrainChanges(&changes, &dropped);
  if (dropped > 0) {
    // more builds than the log keeps (a cut, a long stall): every probe may have met new geometry
    scheduler_.MarkAllStale();
  }
  for (const CityRtScene::ChangedBox& b : changes) {
    if (!scheduled && b.evicted) continue;
    stale_marked_ += scheduler_.MarkStaleBox(field_->scheduler(), b.lo, b.hi);
    ++sector_changes_seen_;
  }
  sector_changes_seen_ += dropped;
}

void GiUpdate::Record(plume::RenderCommandList* list, uint32_t frame_slot, const gi::GiFrame& uf) {
  if (!ready_ || !list || frame_slot >= kFramesInFlight || !field_ || !car_scene_ || !city_scene_ || !world_ || !hit_scene_ || !frames_) {
    return;
  }
  frame_ = uf.frame_serial;
  if (!field_->update_allowed() || !uf.FieldPresent()) {
    ++not_usable_;
    return;
  }
  // The top level of this frame (the city alone, or with the cars) and the world's rows (the light, the camera, the lamps): both are made before the trigger reaches here (scene.cpp).
  const CarRtView& view = car_scene_->View();
  if (!view.scene_valid || !view.set) {
    ++no_scene_;
    return;
  }
  const RtWorldFrame::Current world = world_->current();
  const uint32_t needed = rt_world::kWorldValid | rt_world::kWorldCameraKnown | rt_world::kWorldLight;
  if (!world_->Valid() || !world.rows || (world.decoded.flags & needed) != needed || (world.rows[3] & rt_hit::kFrameShade) == 0) {
    ++no_world_;
    return;
  }
  // The hit data's pool: without it a hit has no material and is shaded black (CityHitDecode answers invalid), and the field would stand as zeros in place of the screen-space bounce light.
  if (!hit_scene_->ready() || hit_scene_->stats().committed == 0) {
    ++no_hit_data_;
    return;
  }
  if (!PipelinesReady()) {
    ++not_ready_;
    return;
  }
  const rules::Budget budget = rules::BudgetOf(field_->fit().budget);
  if (budget.chunk_probes == 0 || budget.rays_per_probe == 0 || !EnsureBuffers(budget) || !EnsureSlot(frame_slot, budget)) {
    ++no_resources_;
    return;
  }
  Slot& slot = slots_[frame_slot];
  // Map before committing the plan. A failed upload must leave dirty probes
  // and the next phase pending.
  void* mapped = slot.rows->map();
  if (!mapped) {
    ++no_resources_;
    return;
  }
  const gi::GpuGiFrame rows = gi::MakeGpuFrame(uf);
  std::memcpy(mapped, rows.words.data(), gi::kGpuFrameBytes);
  slot.rows->unmap();
  void* list_mapped = slot.list->map();
  if (!list_mapped) {
    ++no_resources_;
    return;
  }

  // A new field (a layout, a size, the memory): the scheduler starts again.
  const bool new_field = field_->generation() != scheduler_generation_;
  const bool scheduled = larecomp::settings::mcla_gpu_rt_gi_field_schedule.Get();
  const gi_field::MoveReport& move = field_->move();
  if (new_field || (scheduled && (move.cut || move.relight))) {
    scheduler_.Start(field_->fit().budget);
    view_motion_ = {};
    lighting_.reset();
    previous_lights_.clear();
  }
  if (new_field) {
    scheduler_generation_ = field_->generation();
    have_last_words_ = false;
    interval_frames_ = 0;
    first_logged_ = false;
  }
  DrainSectorChanges(scheduled);
  // A cut: the field's own windows moved a whole window, a teleport or an hour jump was asked for, or this is the first move.
  // Z components of the three world axes in view space give camera forward.
  const gi::Vec3 forward = {rt_hit::FromBits(world.rows[5 * 4 + 2]), rt_hit::FromBits(world.rows[6 * 4 + 2]), rt_hit::FromBits(world.rows[7 * 4 + 2])};
  const bool camera_moving = view_motion_.Observe({float(uf.camera[0]), float(uf.camera[1]), float(uf.camera[2])}, forward);
  if (move.cut || move.relight) {
    ++cuts_;
    double step = 0.0;
    if (have_camera_) {
      const double dx = uf.camera[0] - last_camera_[0], dy = uf.camera[1] - last_camera_[1], dz = uf.camera[2] - last_camera_[2];
      step = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    REXLOG_INFO("{} {}", kPrefix, rules::CutLine(uf.frame_serial, rules::CutReason(move.relight, false, step), move.exposed, gi::kCutWarmupBudgetScale, gi::kCutWarmupFrames));
  }
  for (int a = 0; a < 3; ++a) {
    last_camera_[a] = uf.camera[a];
  }
  have_camera_ = true;

  if (scheduled) {
    const rules::Lighting light = {VectorOf(uf.sun_rgb), VectorOf(uf.ambient_up_rgb), VectorOf(uf.ambient_down_rgb), VectorOf(world.decoded.to_sun_world),
                                  world.decoded.street, world.decoded.fill, world.decoded.weather};
    const bool relight = lighting_ && rules::LightingChanged(*lighting_, light);
    if (!lighting_ || relight) lighting_ = light;
    CollectProbeLights(world, current_lights_);
    if (relight) {
      scheduler_.MarkAllStale();
    } else if (!new_field && !move.cut && !move.relight) {
      rules::VisitChangedLights(previous_lights_, current_lights_, [&](const rules::ProbeLight& changed) {
        const double r = changed.reach;
        if (!(r > 0.0) || !std::isfinite(r) || !std::isfinite(changed.centre.x) || !std::isfinite(changed.centre.y) || !std::isfinite(changed.centre.z)) return;
        const double lo[3] = {changed.centre.x - r, changed.centre.y - r, changed.centre.z - r};
        const double hi[3] = {changed.centre.x + r, changed.centre.y + r, changed.centre.z + r};
        stale_marked_ += scheduler_.MarkStaleBox(field_->scheduler(), lo, hi);
      });
    }
    previous_lights_.swap(current_lights_);
  } else {
    lighting_.reset();
    previous_lights_.clear();
  }

  // THE PLAN.
  rules::PlanInputs in;
  for (int a = 0; a < 3; ++a) {
    in.camera[a] = uf.camera[a];
  }
  in.safe_range_m = uf.safe_range_m;
  in.governor_scale = 1.0f;
  in.warmup_scale = field_->budget_scale();
  // CHECK: compare trace/blend, lamp and bounce recovery on both APIs across a
  // fast turn, passing lit traffic, a sector boundary and the dusk sweep.
  in.reuse_coarse = larecomp::settings::mcla_gpu_rt_gi_reuse_coarse_probes.Get();
  in.phased = scheduled;
  in.camera_moving = camera_moving;
  rules::Plan plan = scheduler_.Make(field_->scheduler(), in, budget.max_probes);
  if (plan.Total() == 0) {
    slot.list->unmap();
    return;
  }
  // CHECK: compare secondary-bounce noise and probe convergence during day/night
  // transitions and streaming. Preserve the nominal ray cone for alpha cut-outs.
  uint32_t full_count = plan.Total();
  if (larecomp::settings::mcla_gpu_rt_gi_short_refresh_rays.Get() && budget.rays_per_probe == 64u && in.warmup_scale == 1.0f) {
    const auto split = std::stable_partition(plan.words.begin(), plan.words.end(), [&](uint32_t word) {
      return rules::RefreshRays(word, budget.rays_per_probe, in.warmup_scale, true) == budget.rays_per_probe;
    });
    full_count = uint32_t(split - plan.words.begin());
  }

  std::memcpy(list_mapped, plan.words.data(), size_t(plan.Total()) * sizeof(uint32_t));
  slot.list->unmap();

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  plume::RenderTexture* irradiance = field_->irradiance().texture.get();
  plume::RenderTexture* moments = field_->moments().texture.get();
  plume::RenderTexture* weight = field_->weight().texture.get();
  const plume::RenderTextureBarrier to_read[3] = {{irradiance, Layout::SHADER_READ}, {moments, Layout::SHADER_READ}, {weight, Layout::SHADER_READ}};
  const plume::RenderTextureBarrier to_write[3] = {{irradiance, Layout::GENERAL}, {moments, Layout::GENERAL}, {weight, Layout::GENERAL}};
  const plume::RenderBufferBarrier buffers[3] = {{rays_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE},
                                                  {field_->state_buffer(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE},
                                                  {stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
  plume::RenderPipeline* trace = Pipeline(kVTrace);
  plume::RenderPipeline* blend = Pipeline(kVBlend);

  Push push = {};
  push.flags[0] = 1;
  push.flags[1] = scheduled;
  push.flags[2] = scheduled && in.warmup_scale == 1.0f;
  push.tune[0] = uf.bounce_weight;
  push.tune[1] = rules::RayReachM(uf.safe_range_m);
  push.tune[2] = gi::kHysteresis;
  push.tune[3] = gi::kHysteresisJump;
  push.tune2[0] = rules::RayConeTanHalf(budget.rays_per_probe);
  push.control[3] = uf.frame_serial;

  const auto record_batch = [&](uint32_t first, uint32_t count, uint32_t rays_per_probe) {
    push.control[1] = rays_per_probe;
    for (const rules::Chunk& chunk : rules::ChunksOf(count, budget.chunk_probes)) {
      push.control[0] = chunk.count;
      push.control[2] = first + chunk.first;
      // Trace reads the field after the preceding chunk's blend and writes the ray records.
      list->barriers(Stage::COMPUTE, buffers, 3, to_read, 3);
      pass_timing_->Mark(list, "rt gi trace");
      list->setComputePipelineLayout(trace_layout_.get());
      list->setComputeDescriptorSet(heap_->texture_set(), rules::kTraceTextureSet);
      list->setComputeDescriptorSet(heap_->sampler_set(), rules::kTraceSamplerSet);
      list->setComputeDescriptorSet(slot.set.get(), rules::kTracePassSet);
      list->setComputeDescriptorSet(view.set, rules::kTraceSceneSet);
      list->setPipeline(trace);
      list->setComputePushConstants(0, &push, 0, sizeof(push));
      const rules::Dispatch trace_groups = rules::DispatchFor(rules::TraceGroups(chunk.count, rays_per_probe));
      list->dispatch(trace_groups.x, trace_groups.y, 1);
      // Blend reads the ray records and writes the atlases and state.
      list->barriers(Stage::COMPUTE, buffers, 3, to_write, 3);
      pass_timing_->Mark(list, "rt gi blend");
      list->setComputePipelineLayout(blend_layout_.get());
      list->setComputeDescriptorSet(slot.set.get(), rules::kBlendPassSet);
      list->setPipeline(blend);
      list->setComputePushConstants(0, &push, 0, sizeof(push));
      const rules::Dispatch blend_groups = rules::DispatchFor(chunk.count);
      list->dispatch(blend_groups.x, blend_groups.y, 1);
      ++chunks_total_;
    }
  };
  record_batch(0, full_count, budget.rays_per_probe);
  record_batch(full_count, plan.Total() - full_count, 32u);
  // The blend's writes are done before the weights pass reads the state words and the readers sample the textures (RecordFinish).
  list->barriers(Stage::COMPUTE, buffers, 3, to_write, 3);
  recorded_frame_ = uf.frame_serial;
  ++frames_run_;
  const uint64_t planned_rays = uint64_t(full_count) * budget.rays_per_probe + uint64_t(plan.Total() - full_count) * 32u;
  rays_planned_ += planned_rays;
  ++interval_frames_;

  if (!first_logged_) {
    first_logged_ = true;
    const CityRtScene::Stats city = city_scene_->stats();
    REXLOG_INFO("{} {}", kPrefix,
                rules::FirstLine(uf.frame_serial, uf.layout, uf.cascade_count, budget.rays_per_probe, city.instances, city.hi_resident, city.lod_resident, uf.bounce_weight, gi::kHysteresis, uf.strength));
  }
  // The counters, every interval: copied to the readback buffer (the next frame's first barrier takes the counters back to the storage state).
  if (uf.frame_serial >= next_readback_ && !readback_busy_) {
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(stats_.get(), 0), rules::kStatBytes);
    readback_busy_ = true;
    next_readback_ = uf.frame_serial + rules::kReadbackEveryFrames;
    frames_->RetireAction([this, frame = uf.frame_serial, generation = generation_, frames = std::exchange(interval_frames_, 0u), field_generation = scheduler_generation_] {
      ProcessReadback(frame, generation, frames, field_generation);
    });
  }
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

void GiUpdate::ProcessReadback(uint64_t frame, uint64_t generation, uint32_t frames, uint32_t field_generation) {
  readback_busy_ = false;
  if (generation != generation_ || field_generation != scheduler_generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    have_last_words_ = false;
    REXLOG_ERROR("{} the counters' readback of frame {} could not be mapped", kPrefix, frame);
    return;
  }
  std::array<uint32_t, rules::kStatWords> words;
  std::memcpy(words.data(), mapped, sizeof(uint32_t) * rules::kStatWords);
  readback_->unmap();
  // The counters are never cleared: the interval's words are the difference to the last readback (the first readback has no baseline and says only that it is one).
  if (!have_last_words_) {
    last_words_ = words;
    have_last_words_ = true;
    return;
  }
  std::array<uint32_t, rules::kStatWords> delta;
  rules::StatDelta(words.data(), last_words_.data(), delta.data());
  last_words_ = words;
  LogInterval(frame, rules::DecodeStats(delta.data()), frames);
}

void GiUpdate::LogInterval(uint64_t frame, const rules::StatSummary& stats, uint32_t frames) {
  rules::IntervalInfo info;
  info.frame = frame;
  info.frames = frames;
  std::copy(stats.probes.begin(), stats.probes.end(), info.probes.begin());
  info.cascades = field_ ? field_->fit().budget.cascade_count : 0;
  info.cuts = cuts_;
  info.budget_scale = 1.0f;
  info.finest_dirty_share = field_ ? double(field_->scheduler().DirtyShare(0)) : 0.0;
  REXLOG_INFO("{} {}", kPrefix, rules::IntervalLine(info, stats));
  REXLOG_INFO("{} frame {}: {} settled far probes slept, {} inside probes skipped", kPrefix, frame, stats.probes_sleeping, stats.probes_skipped);
  if (pass_timing_ && pass_timing_->enabled() && frames > 0) {
    const double trace_ms = pass_timing_->LabelMsPerFrame("rt gi trace");
    const double blend_ms = pass_timing_->LabelMsPerFrame("rt gi blend");
    if (trace_ms > 0.0 || blend_ms > 0.0) {
      REXLOG_INFO("{} {}", kPrefix, rules::RateLine(frame, double(stats.rays) / double(frames), trace_ms, blend_ms));
    }
  }
}

uint64_t GiUpdate::HeldBytes() const {
  uint64_t bytes = rays_ ? ray_bytes_ : 0;
  bytes += stats_ ? rules::kStatBytes : 0;
  bytes += readback_ ? rules::kStatBytes : 0;
  for (const Slot& s : slots_) {
    bytes += (s.rows ? rules::kUploadBytes : 0) + (s.list ? s.list_bytes : 0);
  }
  return bytes;
}

std::string GiUpdate::CensusText() const {
  if (!ready_) {
    return {};
  }
  return std::format("; probe update: {} frames ({} chunks), {:.1f} M rays planned, {:.1f} MiB held, {} probes marked stale, {} sector changes; frames without it: {} not usable, {} without the top "
                     "level, {} without the world's rows, {} without the hit data's pool, {} waiting for the pipelines, {} without resources",
                     frames_run_, chunks_total_, double(rays_planned_) / 1.0e6, double(HeldBytes()) / (1024.0 * 1024.0), stale_marked_,
                     sector_changes_seen_, not_usable_, no_scene_, no_world_, no_hit_data_, not_ready_, no_resources_);
}

}  // namespace mcla::gpu
