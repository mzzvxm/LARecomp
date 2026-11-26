#include "vpl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <rex/logging/log_category.h>

#include "ao.h"
#include "descriptors.h"
#include "car_frame.h"
#include "car_rt_scene.h"
#include "device.h"
#include "gbuffer.h"
#include "gfx_preset_apply.h"
#include "gi_bounce_settings.h"
#include "gi_setting.h"
#include "headlight_rig.h"
#include "host_shaders.h"
#include "hlsl/interior_camera_rules.hlsli"
#include "light_manager.h"
#include "live_text.h"
#include "mc_engine/camera/menu_camera.h"
#include "menu_world.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "rt_world_frame.h"
#include "screen_shared.h"
#include "settings/larecomp_settings.h"
#include "temporal_identities.h"
#include "world_clock.h"

namespace mcla::gpu {
namespace {
namespace ll = larecomp::gpu::local_lights;
namespace lg = larecomp::gpu;
namespace st = larecomp::settings;
using Format = plume::RenderFormat;
using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;
namespace Access = plume::RenderBufferAccess;

constexpr uint32_t kNoSlot = screen::kNoSlot;
constexpr uint32_t kHistoryBytes = vpl::kMaxLights * 48;
constexpr Format kIrradianceFormat = Format::R16G16B16A16_FLOAT;

float Gain(double x) { return float(std::clamp(std::isfinite(x) ? x : 1.0, 0.0, 16.0)); }

bool NeonOn() {
  static LiveText neon("mcla_gpu_neon_light");
  return gpu_class::Resolve(gpu_class::Effect::kNeonLight, neon.Get()) == "on";
}

void MergeNeon(std::vector<lg::Light>& points, std::vector<lg::Light>& beams) {
  for (size_t i = 0; i < points.size();) {
    lg::Light beam = points[i++];
    float count = 1.0f;
    while (i < points.size() && points[i].car_key == beam.car_key) {
      const lg::Light& point = points[i++];
      ++count;
      for (uint32_t j = 0; j < 3; ++j) {
        beam.pos[j] += (point.pos[j] - beam.pos[j]) / count;
        beam.color[j] += point.color[j];
      }
    }
    beams.push_back(beam);
  }
}
}  // namespace

void VirtualBounce::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                               EdramTargets& edram, PassTiming& timing, LightManager& lights,
                               CarRtScene& rt, CarFrame& car, ScreenShared& screen,
                               AmbientOcclusion& ao, GBuffer& gbuffer, RtWorldFrame& world, RtSceneService& rt_service) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  timing_ = &timing;
  lights_ = &lights;
  rt_ = &rt;
  car_ = &car;
  screen_ = &screen;
  ao_ = &ao;
  gbuffer_ = &gbuffer;
  world_ = &world;
  rt_service_ = &rt_service;
}

void VirtualBounce::Shutdown() {
  if (passes_) passes_->ReleaseImage(irradiance_);
  if (lights_) lights_->WantBounce(false);
  for (Slot& slot : slots_) {
    slot.set.reset();
    slot.input.reset();
  }
  for (Programs& p : programs_) p = {};
  history_.reset();
  rt_layout_.reset();
  layout_.reset();
  passes_ = nullptr;
  history_frame_ = 0;
  history_ids_.fill(0);
  logged_.fill(false);
}

void VirtualBounce::BeginFrame(uint64_t frame) {
  frame_ = frame;
  recorded_ = false;
  cockpit_ = interior_camera::IsCockpit(CurrentRenderedCameraRig());
  static LiveText bounce("mcla_gpu_light_bounce");
  enabled_ = bounce.Get() == "on" && gi_bounce::AnyGroup(gi_bounce::GroupsNow());
  const float hour = world_clock::Hour();
  if (enabled_ && hour >= 0.0f && CurrentHeadlights().count == 0 &&
      rt_world::ScheduleAt(larecomp::citypack::LampHours{}, hour, 0.0f).fill <= 0.0f) enabled_ = false;
  rt_requested_ = enabled_ && rt_ && rt_->ready() &&
      (gfx_preset::NeedsRt(gfx_preset::Current()) || gi_setting::Current().level != gi::Level::kOff);
  if (rt_requested_ && rt_service_) {
    rt_scene::Needs needs;
    needs.city = city_rt::Level::kHigh;
    needs.hit = needs.city;
    rt_service_->Want(rt_scene::Consumer::kVirtualBounce, needs);
  }
  if (screen_) screen_->Want(screen::Consumer::kVirtualBounce, enabled_ ? screen::ProductBit(screen::kLinDepth) : 0);
  if (lights_) lights_->WantBounce(enabled_);
  if (enabled_ && car_) car_->RequestStencilCentres();
  if (++idle_ == 120 && passes_) passes_->ReleaseImage(irradiance_);
}

bool VirtualBounce::EnsureResources(uint32_t index) {
  if (!device_ || !heap_ || index >= slots_.size()) return false;
  if (!layout_) {
    set_desc_.begin();
    set_desc_.addByteAddressBuffer(0);
    set_desc_.addReadWriteByteAddressBuffer(1);
    set_desc_.addByteAddressBuffer(2);
    set_desc_.end();
    plume::RenderPipelineLayoutBuilder desc;
    desc.begin(false, false);
    for (uint32_t i = 0; i < 4; ++i) desc.addDescriptorSet(heap_->texture_set_builder());
    desc.addDescriptorSet(set_desc_);
    desc.end();
    layout_ = device_->CreatePipelineLayout(desc, "virtual bounce layout");
    if (!layout_) return false;
  }
  if (!history_) {
    history_ = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(
        kHistoryBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "virtual bounce hits");
    if (!history_) return false;
  }
  Slot& slot = slots_[index];
  if (!slot.set) {
    slot.input = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(
        sizeof(vpl::Frame), plume::RenderBufferFlag::STORAGE), "virtual bounce frame");
    if (!slot.input) return false;
    slot.set = device_->CreateDescriptorSet(set_desc_, 3, 0, "virtual bounce set");
    if (!slot.set) return false;
    slot.set->setBuffer(0, slot.input.get(), sizeof(vpl::Frame));
    slot.set->setBuffer(1, history_.get(), kHistoryBytes);
    slot.set->setBuffer(2, history_.get(), kHistoryBytes);
  }
  return true;
}

bool VirtualBounce::EnsurePrograms(Format format, uint32_t samples, bool ray) {
  if (!passes_ || !layout_ || !device_ || !heap_ || samples == 0 || samples > 8 || (samples & (samples - 1)) != 0) return false;
  Programs& p = programs_[samples == 8 ? 3 : samples == 4 ? 2 : samples == 2 ? 1 : 0];
  const auto shader = [this, samples](uint32_t stage) {
    const HostShaderBlob blob = VirtualBounceShader(stage, samples > 1);
    return passes_->CreateShader(blob.data, blob.size, blob.name);
  };
  if (ray && !rt_layout_ && rt_) {
    plume::RenderPipelineLayoutBuilder desc;
    desc.begin(false, false);
    for (uint32_t i = 0; i < 4; ++i) desc.addDescriptorSet(heap_->texture_set_builder());
    desc.addDescriptorSet(set_desc_);
    desc.addDescriptorSet(rt_->SetDesc());
    desc.end();
    rt_layout_ = device_->CreatePipelineLayout(desc, "virtual bounce ray layout");
  }
  auto& hit = ray ? p.hit_rt : p.hit;
  if (!hit && (!ray || rt_layout_)) {
    if (const auto cs = shader(ray ? 1u : 0u)) {
      hit = device_->CreateComputePipeline({ray ? rt_layout_.get() : layout_.get(), cs.get(), 16, 1, 1}, "vpl hit");
    }
  }
  if (!p.irradiance) {
    if (const auto ps = shader(2)) {
      p.irradiance = passes_->CreatePipeline(*layout_, *ps, kIrradianceFormat, "vpl irradiance");
    }
  }
  if (!p.composite || p.format != format) {
    if (const auto ps = shader(3)) {
      auto blend = plume::RenderBlendDesc::Copy();
      blend.blendEnabled = true;
      blend.srcBlend = plume::RenderBlend::ONE;
      blend.dstBlend = plume::RenderBlend::ONE;
      blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                    uint8_t(plume::RenderColorWriteEnable::BLUE);
      auto composite = passes_->CreatePipeline(*layout_, *ps, format, blend, samples, "vpl composite");
      if (!composite) return false;
      if (p.composite && frames_) frames_->Retire(std::move(p.composite));
      p.composite = std::move(composite);
      p.format = format;
    }
  }
  return hit && p.irradiance && p.composite && p.format == format;
}

float VirtualBounce::VirtualShare(const lg::Light& light, float field_share) const {
  const bool listed_static = field_share > 0.0f && world_ &&
      world_->EmitterMotionOf(light.id) == RtWorldFrame::EmitterMotion::kStatic;
  return gi_bounce::VirtualShare(listed_static, field_share);
}

void VirtualBounce::Gather(const float eye[3], float night, float field_share) {
  namespace re = rt_emitters;
  beams_.clear();
  candidates_.Clear();
  const uint32_t groups = gi_bounce::GroupsNow();
  const bool heads = (groups & re::kGroupHeadlights) != 0 && headlight_rig::SettingOn();
  constexpr ll::Tier tier = ll::Tier::kHigh;
  const float gain = Gain(st::mcla_gpu_local_lights_gain.Get());
  if (heads) {
    const auto& rig = CurrentHeadlights();
    for (uint32_t i = 0; i < rig.count; ++i) beams_.push_back(lg::LightFromHeadlight(rig.lights[i]));
  }
  if ((groups & re::kGroupNeon) != 0 && NeonOn()) {
    ll::NeonSourceInputs in;
    ll::nl::Global().Recent(&neon_);
    in.cars = &neon_;
    in.tier = ll::Tier::kUltra;  // four complete tubes, fixed on every adapter
    in.gain = Gain(st::mcla_gpu_neon_light_gain.Get());
    in.reach_scale = float(st::mcla_gpu_neon_light_reach.Get());
    in.tuned = st::mcla_gpu_neon_light_tuned.Get();
    std::copy_n(eye, 3, in.camera);
    ll::GatherNeonLights(in, &candidates_.neon);
    MergeNeon(candidates_.neon, beams_);
  }
  if (lights_ && lights_->active()) {
    const auto& cars = lights_->Cars();
    const double reference[3] = {eye[0], eye[1], eye[2]};
    lights_->SetCarReference(reference);
    ll::CarSourceInputs in;
    in.cars = &cars;
    in.frame = &lights_->Frame();
    in.tier = tier;
    in.gain = Gain(st::mcla_gpu_local_lights_car_gain.Get());
    std::copy_n(eye, 3, in.camera);
    in.sources = ((groups & re::kGroupRear) ? ll::kSourceRearLamps : 0) |
                 ((groups & re::kGroupOther) ? ll::kSourcePolice : 0);
    // The rig is the forward headlight producer. Only the debug light pass adds
    // the extra headlights that it does not cover.
    if (heads && st::mcla_gpu_local_lights.Get() &&
        (st::mcla_gpu_local_lights_sources.Get() & ll::kSourceHeadLamps)) in.sources |= ll::kSourceHeadLamps;
    ll::GatherCarLights(in, &candidates_);
    for (const auto& l : candidates_.head) beams_.push_back(l);
    for (auto& l : candidates_.rear) {
      for (uint32_t i = 0; i < cars.count; ++i) {
        const auto& c = cars.cars[i];
        if (c.lights.car_key != l.car_key) continue;
        l.dir[0] = float(c.axes.back[0]);
        l.dir[1] = -0.22f;
        l.dir[2] = float(c.axes.back[2]);
        // Turn and reverse lamps use their own front/back placement.
        if ((l.pos[0] - c.centre[0]) * c.axes.back[0] + (l.pos[2] - c.centre[2]) * c.axes.back[2] < 0.0) {
          l.dir[0] = -l.dir[0];
          l.dir[2] = -l.dir[2];
        }
        break;
      }
      beams_.push_back(l);
    }
    for (const auto& l : candidates_.police) beams_.push_back(l);
  }

  for (auto& light : beams_) {
    // A merged tube retains its first point ID, shared with the RT list
    // even if the direct light pass used a different number of points.
    const float scale = gain * (light.law == lg::LightLaw::kHeadlight ? 1.0f : night) * VirtualShare(light, field_share);
    for (float& c : light.color) c = std::isfinite(c) ? std::max(0.0f, c * scale) : 0.0f;
  }
  std::erase_if(beams_, [eye](const lg::Light& light) {
    return light.id == lg::kNoLightId || !gi_bounce::Bounces(light, 1.0f, 1.0f) ||
           !(ll::detail::Dist2(light.pos, eye) <= 70.0f * 70.0f);
  });
  ll::RankByScore(&beams_, eye, vpl::kMaxLights);
}

uint32_t VirtualBounce::SourceStencil(const lg::Light& light) const {
  if (!car_) return 0;
  const auto& car = car_->Current();
  if ((light.flags & lg::kLightFlagPlayer) != 0) return car.valid ? car.player_id : 0;
  const float* source = light.pos;
  float centre[3];
  if (lights_ && lights_->active()) {
    const auto& cars = lights_->Cars();
    for (uint32_t i = 0; i < cars.count; ++i) {
      const auto& candidate = cars.cars[i];
      if (candidate.lights.car_key != light.car_key) continue;
      for (uint32_t j = 0; j < 3; ++j) centre[j] = float(candidate.centre[j]);
      source = centre;
      break;
    }
  }
  uint32_t stencil = 0;
  float best = 2.6f * 2.6f;
  for (const auto& seen : car.seen_cars) {
    double centre[3];
    if (!seen.Centre(centre)) continue;
    const float p[3] = {float(centre[0]), float(centre[1]), float(centre[2])};
    const float d = ll::detail::Dist2(source, p);
    if (d < best && seen.stencil_id != 0 && seen.stencil_id <= 23) {
      best = d;
      stencil = seen.stencil_id;
    }
  }
  return stencil;
}

uint32_t VirtualBounce::WriteBeams(vpl::Frame* input) {
  if (!input) return 0;
  std::array<uint64_t, vpl::kMaxLights> ids{};
  std::array<uint32_t, vpl::kMaxLights> indices{};
  indices.fill(vpl::kMaxLights);
  const uint32_t count = uint32_t(beams_.size());
  for (uint32_t i = 0; i < count; ++i) {
    const auto old = std::find(history_ids_.begin(), history_ids_.end(), beams_[i].id);
    if (old != history_ids_.end()) {
      indices[i] = uint32_t(old - history_ids_.begin());
      ids[indices[i]] = beams_[i].id;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    const lg::Light& light = beams_[i];
    const bool reset = indices[i] == vpl::kMaxLights;
    if (reset) {
      indices[i] = uint32_t(std::find(ids.begin(), ids.end(), 0) - ids.begin());
      ids[indices[i]] = light.id;
    }
    auto& beam = input->beams[i];
    const bool head = light.source == lg::LightSource::kHeadlight;
    const bool neon = light.source == lg::LightSource::kNeon;
    const bool rear = light.source == lg::LightSource::kOther;
    const bool down = neon || (!head && !rear);
    float height = neon ? 0.35f : head ? 0.65f : rear ? 0.55f : 1.55f;
    if (lights_ && lights_->active()) {
      const auto& cars = lights_->Cars();
      for (uint32_t j = 0; j < cars.count; ++j) {
        const auto& car = cars.cars[j];
        if (car.lights.car_key != light.car_key || car.axes.source == lg::car_lights::FrameSource::kNone) continue;
        const float above_ground = light.pos[1] - float(car.axes.origin[1]) + 0.35f;
        if (above_ground >= 0.10f && above_ground <= 2.0f) height = above_ground;
        break;
      }
    }
    beam.origin_range = {light.pos[0], light.pos[1], light.pos[2], light.range};
    beam.aim_ground = {down ? 0.0f : light.dir[0], down ? -1.0f : std::min(light.dir[1], -0.03f),
                       down ? 0.0f : light.dir[2], height};
    beam.colour_law = {light.color[0], light.color[1], light.color[2], light.law_a};
    beam.meta = {indices[i], reset ? 1u : 0u, SourceStencil(light), uint32_t(light.law) | (neon ? 4u : 0u)};
  }
  history_ids_ = ids;
  return count;
}

void VirtualBounce::Draw(plume::RenderCommandList* list, const Slot& slot, plume::RenderPipeline& program,
                         plume::RenderFramebuffer& target, uint32_t width, uint32_t height, bool clear) const {
  if (!list || !layout_ || !heap_ || !slot.set) return;
  list->setFramebuffer(&target);
  if (clear) list->clearColor(0, plume::RenderColor(0, 0, 0, 0));
  const plume::RenderViewport viewport{0, 0, float(width), float(height)};
  const plume::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
  list->setViewports(&viewport, 1);
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(heap_->texture_set(), i);
  list->setGraphicsDescriptorSet(slot.set.get(), 4);
  list->setPipeline(&program);
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
}

bool VirtualBounce::OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                              bool planes, uint32_t index, float field_share) {
  if (recorded_ || !enabled_ || !list || !screen_ || !ao_ || !edram_ || !passes_ || !timing_ || !heap_) return false;
  const auto& sf = screen_->Frame();
  if (sf.frame_serial != uint32_t(frame_) || !menu_world::RulesFor(ao_->FrameWorld()).bounce ||
      !(sf.camera.flags & screen::kCameraValid) || sf.lin_depth_slot[0] == kNoSlot ||
      sf.traced[0] == 0 || sf.traced[1] == 0 || sf.render[0] == 0 || sf.render[1] == 0 ||
      sf.camera.tan_half_fov[0] <= 0 || sf.camera.tan_half_fov[1] <= 0 ||
      !summary.source || !summary.source->texture || summary.sample_slot == kNoSlot) return false;
  AmbientOcclusion::FrameCameraOut axes;
  float view[16];
  bool own;
  if (!ao_->FrameCameraNow(&axes) || !axes.have_rows || !ao_->TriggerViewInverse(view, &own)) return false;

  vpl::Frame in{};
  for (uint32_t i = 0; i < 3; ++i) {
    in.right[i] = axes.right[i];
    in.up[i] = axes.up[i];
    in.forward[i] = axes.forward[i];
    in.eye[i] = view[12 + i];
  }
  in.eye[3] = std::clamp(sf.camera.exposure_bias, 1.0f, 16.0f);
  float ambient[3], sun[3];
  double sun_dir[3];
  const float night_hint = ao_->FrameLight(ambient, sun, sun_dir) ? rt_hit::NightFactor(sun) : -1.0f;
  const float night = rt_world::ScheduleAt(larecomp::citypack::LampHours{}, world_clock::Hour(), night_hint).fill;
  Gather(in.eye.data(), night, field_share);
  if (beams_.empty()) return false;

  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != sf.host_samples ||
      found->scale.Edge(found->width) != sf.render[0] || found->scale.Edge(found->height) != sf.render[1]) return false;
  bool unset = false;
  auto* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour || !colour->texture) return false;

  const uint32_t samples = sf.host_samples;
  const auto* top = rt_requested_ && rt_ && world_ && world_->Valid() ? &rt_->View() : nullptr;
  bool ray = top && top->scene_valid && top->set;
  if (!EnsureResources(index)) return false;
  if (!EnsurePrograms(colour->format, samples, ray)) {
    if (!ray || !EnsurePrograms(colour->format, samples, false)) return false;
    ray = false;
  }
  Programs& program = programs_[samples == 8 ? 3 : samples == 4 ? 2 : samples == 2 ? 1 : 0];
  const uint32_t width = (sf.render[0] + 1) / 2, height = (sf.render[1] + 1) / 2;
  if (!passes_->EnsureImage(irradiance_, width, height, kIrradianceFormat, "vpl irradiance") ||
      !irradiance_.framebuffer || !irradiance_.texture) return false;
  EdramTargets::Target* targets[] = {colour};
  auto* target = edram_->Framebuffer(targets, 1, nullptr);
  if (!target) return false;
  uint32_t main_sample = kNoSlot, unused = kNoSlot;
  if (!edram_->SampleSlots(*colour, &main_sample, &unused)) main_sample = kNoSlot;

  const double now = mc::cinematic::SceneClockNowMs(frame_) * 1.0e-3;
  const double elapsed = now - history_time_;
  if (frame_ != history_frame_ + 1 || generation_ != sf.generation ||
      !axes.have_motion || elapsed < 0.0 || elapsed > 0.25) history_ids_.fill(0);
  const uint32_t count = WriteBeams(&in);

  AmbientOcclusion::OcclusionView ov;
  const bool ao = ao_->FrameOcclusionNow(&ov) && ov.has_visibility;
  in.lens = {sf.camera.tan_half_fov[0], sf.camera.tan_half_fov[1], vpl::HistoryWeight(elapsed), ao ? ov.strength : 0.0f};
  if (cockpit_ && car_) {
    const auto& car = car_->Current();
    if (car.valid && car.bounds_valid) {
      for (uint32_t i = 0; i < 3; ++i) {
        in.cabin_min[i] = float(car.bounds_min[i]) - 0.15f;
        in.cabin_max[i] = float(car.bounds_max[i]) + 0.15f;
      }
      in.cabin_min[3] = 1.0f;
    }
    in.apply[1] = car.valid ? car.player_id : 0;
  }
  in.apply[0] = irradiance_.slot;
  in.apply[2] = cockpit_;
  in.sizes = {sf.render[0], sf.render[1], width, height};
  in.counts = {sf.traced[0], sf.traced[1], count, sf.sample0};
  in.slots = {sf.lin_depth_slot[0], sf.lin_depth_slot[1], sf.lin_depth_slot[2], summary.sample_slot};
  in.depth = {sf.camera.lin[0], sf.camera.lin[1], sf.camera.lin[2], sf.camera.lin[3]};
  uint32_t albedo = kNoSlot;
  if (planes && gbuffer_) {
    const auto& gb = gbuffer_->frame();
    if (gb.Has(gbuffer::kPlaneAlbedo) && gb.width == sf.render[0] && gb.height == sf.render[1] &&
        gb.host_samples == samples) albedo = gb.slot[gbuffer::kPlaneAlbedo];
  }
  in.guides = {albedo, main_sample, ao ? ov.visibility_slot : kNoSlot, summary.stencil_slot};
  const Slot& slot = slots_[index];
  if (!slot.input || !slot.set) return false;
  void* mapped = slot.input->map();
  if (!mapped) return false;
  std::memcpy(mapped, &in, sizeof(in));
  slot.input->unmap();

  // CHECK: confirm depth/colour and UAV handoffs on both APIs, including MSAA
  // sample zero in cockpit views and the world-frame transform on RT cuts.
  edram_->Transition(list, *summary.source, Layout::SHADER_READ);
  if (main_sample != kNoSlot) edram_->Transition(list, *colour, Layout::SHADER_READ);
  for (uint32_t level = 0; level < screen::kDepthLevels; ++level) {
    const auto& image = screen_->Depth(level);
    if (image.texture) {
      const plume::RenderTextureBarrier depth_read(image.texture.get(), Layout::SHADER_READ);
      list->barriers(Stage::GRAPHICS_AND_COMPUTE, &depth_read, 1);
    }
  }
  const plume::RenderBufferBarrier hit_write(history_.get(), Access::READ | Access::WRITE);
  list->barriers(Stage::COMPUTE, &hit_write, 1);
  timing_->Mark(list, "vpl hit");
  list->setComputePipelineLayout(ray ? rt_layout_.get() : layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setComputeDescriptorSet(heap_->texture_set(), i);
  list->setComputeDescriptorSet(slot.set.get(), 4);
  if (ray && top) list->setComputeDescriptorSet(top->set, 5);
  list->setPipeline(ray ? program.hit_rt.get() : program.hit.get());
  list->dispatch(1, 1, 1);

  const plume::RenderBufferBarrier hit_read(history_.get(), Access::READ);
  const plume::RenderTextureBarrier irradiance_write(irradiance_.texture.get(), Layout::COLOR_WRITE);
  list->barriers(Stage::GRAPHICS, &hit_read, 1, &irradiance_write, 1);
  timing_->Mark(list, "vpl irradiance");
  Draw(list, slot, *program.irradiance, *irradiance_.framebuffer, width, height, irradiance_.layout == Layout::UNKNOWN);
  const plume::RenderTextureBarrier irradiance_read(irradiance_.texture.get(), Layout::SHADER_READ);
  list->barriers(Stage::GRAPHICS, &irradiance_read, 1);
  irradiance_.layout = Layout::SHADER_READ;

  edram_->Transition(list, *colour, Layout::COLOR_WRITE);
  timing_->Mark(list, "vpl composite");
  Draw(list, slot, *program.composite, *target, sf.render[0], sf.render[1], false);
  timing_->Mark(list, "after vpl");
  history_frame_ = frame_;
  history_time_ = now;
  generation_ = sf.generation;
  idle_ = 0;
  recorded_ = true;
  const uint32_t path = ray ? 1u : 0u;
  if (!logged_[path]) {
    logged_[path] = true;
    REXLOG_INFO("[gpu] vpl: {} placement, {} beams, {}x{} irradiance, {} samples",
                ray ? "ray query" : "depth chain", count, width, height, samples);
  }
  return true;
}

uint64_t VirtualBounce::HeldBytes() const {
  return uint64_t(irradiance_.width) * irradiance_.height * 8 + (history_ ? kHistoryBytes : 0);
}

}  // namespace mcla::gpu
