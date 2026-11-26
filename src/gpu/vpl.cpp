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
#include "gi_apply.h"
#include "gfx_preset_apply.h"
#include "gi_bounce_settings.h"
#include "gi_setting.h"
#include "headlight_rig.h"
#include "host_shaders.h"
#include "hlsl/interior_camera_rules.hlsli"
#include "light_manager.h"
#include "light_composite_rules.h"
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
  ReleaseImages();
  if (lights_) lights_->WantBounce(false);
  if (world_) world_->WantBounceEmitters(false);
  vol_lamps::SetWantedBy(vol_lamps::kWantedByVirtualBounce, false);
  street_lamps_.clear();
  street_sequence_ = 0;
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

void VirtualBounce::ReleaseImages() {
  if (!passes_) return;
  passes_->RetireFramebuffer(std::move(irradiance_mrt_));
  passes_->ReleaseImage(direct_irradiance_);
  passes_->ReleaseImage(irradiance_);
}

void VirtualBounce::BeginFrame(uint64_t frame) {
  frame_ = frame;
  recorded_ = false;
  hit_recorded_ = false;
  irradiance_recorded_ = false;
  composite_recorded_ = false;
  composite_enabled_ = st::mcla_gpu_light_composite.Get();
  cockpit_ = interior_camera::IsCockpit(CurrentRenderedCameraRig());
  static LiveText bounce("mcla_gpu_light_bounce");
  enabled_ = bounce.Get() == "on" && gi_bounce::AnyGroup(gi_bounce::GroupsNow());
  const float hour = world_clock::Hour();
  if (enabled_ && hour >= 0.0f && CurrentHeadlights().count == 0 &&
      rt_world::ScheduleAt(larecomp::citypack::LampHours{}, hour, 0.0f).fill <= 0.0f) enabled_ = false;
  rt_requested_ = enabled_ && rt_ && rt_->ready() &&
      (gfx_preset::NeedsRt(gfx_preset::Current()) || gi_setting::Current().level != gi::Level::kOff);
  if (world_) world_->WantBounceEmitters(composite_enabled_ && rt_requested_);
  vol_lamps::SetWantedBy(vol_lamps::kWantedByVirtualBounce, composite_enabled_ && enabled_);
  if (rt_requested_ && rt_service_) {
    rt_scene::Needs needs;
    needs.city = city_rt::Level::kHigh;
    needs.hit = needs.city;
    rt_service_->Want(rt_scene::Consumer::kVirtualBounce, needs);
  }
  if (screen_) screen_->Want(screen::Consumer::kVirtualBounce, enabled_ ? screen::ProductBit(screen::kLinDepth) : 0);
  if (lights_) lights_->WantBounce(enabled_);
  if (enabled_ && car_) car_->RequestStencilCentres();
  if (++idle_ == 120) ReleaseImages();
}

bool VirtualBounce::EnsureResources(uint32_t index) {
  if (!device_ || !heap_ || index >= slots_.size()) return false;
  if (!layout_) {
    set_desc_.begin();
    set_desc_.addByteAddressBuffer(0);
    set_desc_.addReadWriteByteAddressBuffer(1);
    set_desc_.addByteAddressBuffer(2);
    set_desc_.addByteAddressBuffer(3);
    set_desc_.addByteAddressBuffer(4);
    set_desc_.end();
    plume::RenderPipelineLayoutBuilder desc;
    desc.begin(false, false);
    for (uint32_t i = 0; i < 4; ++i) desc.addDescriptorSet(heap_->texture_set_builder());
    desc.addDescriptorSet(set_desc_);
    desc.addDescriptorSet(heap_->sampler_set_builder());
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
        sizeof(vpl::CompositeFrame), plume::RenderBufferFlag::STORAGE), "virtual bounce frame");
    if (!slot.input) return false;
    slot.set = device_->CreateDescriptorSet(set_desc_, 5, 0, "virtual bounce set");
    if (!slot.set) return false;
    slot.set->setBuffer(0, slot.input.get(), sizeof(vpl::CompositeFrame));
    slot.set->setBuffer(1, history_.get(), kHistoryBytes);
    slot.set->setBuffer(2, history_.get(), kHistoryBytes);
  }
  return true;
}

bool VirtualBounce::EnsurePrograms(Format format, uint32_t samples, bool ray, bool beams, bool direct, bool combined) {
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
  if (beams && !hit && (!ray || rt_layout_)) {
    if (const auto cs = shader(ray ? 1u : 0u)) {
      hit = device_->CreateComputePipeline({ray ? rt_layout_.get() : layout_.get(), cs.get(), 16, 1, 1}, "vpl hit");
    }
  }
  if (beams && !direct && !p.irradiance) {
    if (const auto ps = shader(2)) {
      p.irradiance = passes_->CreatePipeline(*layout_, *ps, kIrradianceFormat, "vpl irradiance");
    }
  }
  if (direct && !p.combined_irradiance) {
    if (const auto ps = shader(4)) {
      const Format formats[] = {kIrradianceFormat, kIrradianceFormat};
      p.combined_irradiance = passes_->CreateMrtPipeline(*layout_, *ps, formats, 2, "light irradiance");
    }
  }
  auto& pipeline = combined ? p.combined_composite : p.composite;
  auto& pipeline_format = combined ? p.combined_format : p.format;
  if (!pipeline || pipeline_format != format) {
    if (const auto ps = shader(combined ? 5u : 3u)) {
      auto blend = plume::RenderBlendDesc::Copy();
      blend.blendEnabled = true;
      blend.srcBlend = plume::RenderBlend::ONE;
      blend.dstBlend = plume::RenderBlend::ONE;
      blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                    uint8_t(plume::RenderColorWriteEnable::BLUE);
      auto result = passes_->CreatePipeline(*layout_, *ps, format, blend, samples, combined ? "light composite" : "vpl composite");
      if (!result) return false;
      if (pipeline && frames_) frames_->Retire(std::move(pipeline));
      pipeline = std::move(result);
      pipeline_format = format;
    }
  }
  return (!beams || (hit && (direct || p.irradiance))) &&
         (!direct || p.combined_irradiance) && pipeline && pipeline_format == format;
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
  if (composite_enabled_) GatherWorld(eye, night, field_share);
}

void VirtualBounce::GatherWorld(const float eye[3], float night, float field_share) {
  namespace re = rt_emitters;
  if (beams_.size() >= vpl::kMaxLights || !(night > 0.0f)) return;
  const bool pair = screen_ && screen_->lights_wanted();
  const bool with_streets = light_composite::WorldVirtual(true, pair, field_share);
  const bool with_decor = light_composite::WorldVirtual(false, pair, field_share) &&
                          (gi_bounce::GroupsNow() & re::kGroupOther) != 0;
  if (!with_streets && !with_decor) return;

  world_beams_.clear();
  if (with_streets) {
    bool stale = false;
    vol_lamps::Global().Latest(&street_lamps_, &street_sequence_, &stale, std::chrono::milliseconds(500));
    if (stale) {
      street_lamps_.clear();
      street_sequence_ = 0;
    }
    for (const auto& lamp : street_lamps_) {
      if (!lamp.on || !vol_lamps::Plausible(lamp)) continue;
      auto light = lg::LightFromGameLamp(lamp);
      light.range = std::min(light.range, 38.0f);
      world_beams_.push_back(light);
    }
  }
  if (with_decor) {
    if (lights_ && lights_->active()) {
      const auto& frame = lights_->Frame();
      for (uint32_t i = 0; i < frame.count && i < frame.lights.size(); ++i) {
        const auto& light = frame.lights[i];
        if (!re::IsVehicleLight(light) && light.source != lg::LightSource::kLamp) world_beams_.push_back(light);
      }
    }
    if (st::mod_lights.Get()) {
      const auto mods = re::mls::Global().Current();
      ll::GatherModLamps(re::mls::Global().Built(), mods.get(), ll::Ab::kGrid, eye, &world_beams_);
    }
  }
  const float gain = Gain(st::mcla_gpu_local_lights_gain.Get());
  for (auto& light : world_beams_) {
    const float share = light.source == lg::LightSource::kLamp ? 1.0f - field_share : VirtualShare(light, field_share);
    for (float& colour : light.color) colour = std::isfinite(colour) ? std::max(0.0f, colour * gain * night * share) : 0.0f;
  }
  std::erase_if(world_beams_, [eye](const auto& light) {
    return light.id == lg::kNoLightId || !gi_bounce::Bounces(light, 1.0f, 1.0f) ||
           ll::detail::Dist2(light.pos, eye) > 70.0f * 70.0f;
  });
  ll::RankByScore(&world_beams_, eye, uint32_t(world_beams_.size()));
  // CHECK: inspect street and decorative landing points, including a moving
  // car crossing a street pool and both RT and screen-space day/night fades.
  uint32_t streets = 0;
  for (const auto& light : world_beams_) {
    if (beams_.size() >= vpl::kMaxLights) break;
    const bool street = light.source == lg::LightSource::kLamp;
    if (street && streets == 8) continue;
    if (std::any_of(beams_.begin(), beams_.end(), [&](const auto& placed) { return placed.id == light.id; })) continue;
    beams_.push_back(light);
    streets += street;
  }
}

uint32_t VirtualBounce::SourceStencil(const lg::Light& light) const {
  if (!car_ || !rt_emitters::IsVehicleLight(light)) return 0;
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
    const bool rear = light.source == lg::LightSource::kOther && light.car_key != 0;
    const bool world = !rt_emitters::IsVehicleLight(light);
    const bool world_spot = world && light.law == lg::LightLaw::kGameLamp;
    const bool down = neon || (!head && !rear && !world_spot);
    float height = neon ? 0.35f : head ? 0.65f : rear ? 0.55f : world ? 5.0f : 1.55f;
    if (!world && lights_ && lights_->active()) {
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
    beam.meta = {indices[i], reset ? 1u : 0u, SourceStencil(light), uint32_t(light.law) | (neon ? 4u : 0u) | (world ? 8u : 0u)};
  }
  history_ids_ = ids;
  return count;
}

void VirtualBounce::Draw(plume::RenderCommandList* list, const Slot& slot, plume::RenderPipeline& program,
                         plume::RenderFramebuffer& target, uint32_t width, uint32_t height, uint32_t clear_targets) const {
  if (!list || !layout_ || !heap_ || !slot.set) return;
  list->setFramebuffer(&target);
  for (uint32_t i = 0; i < clear_targets; ++i) list->clearColor(i, plume::RenderColor(0, 0, 0, 0));
  const plume::RenderViewport viewport{0, 0, float(width), float(height)};
  const plume::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
  list->setViewports(&viewport, 1);
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(heap_->texture_set(), i);
  list->setGraphicsDescriptorSet(slot.set.get(), 4);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 5);
  list->setPipeline(&program);
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
}

bool VirtualBounce::OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                              bool planes, uint32_t index, float field_share, GiApply* gi, const Direct* direct) {
  if (composite_recorded_ || recorded_) return false;
  if (Record(summary, list, planes, index, field_share, composite_enabled_, gi, direct)) return true;
  if (composite_enabled_) Record(summary, list, planes, index, field_share, false, nullptr, nullptr);
  return false;
}

bool VirtualBounce::Record(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                           bool planes, uint32_t index, float field_share, bool combined,
                           GiApply* gi, const Direct* direct) {
  const auto* gi_push = combined && gi ? gi->CompositePush() : nullptr;
  auto* gi_buffer = gi_push && gi ? gi->CompositeBuffer() : nullptr;
  const bool has_direct = combined && direct && direct->buffer && direct->frame.info[0] != 0;
  if (composite_recorded_ || (!enabled_ && !has_direct && !gi_buffer) ||
      !list || !screen_ || !ao_ || !edram_ || !passes_ || !timing_ || !heap_) return false;
  const auto& sf = screen_->Frame();
  if (sf.frame_serial != uint32_t(frame_) ||
      !(sf.camera.flags & screen::kCameraValid) || sf.lin_depth_slot[0] == kNoSlot ||
      sf.traced[0] == 0 || sf.traced[1] == 0 || sf.render[0] == 0 || sf.render[1] == 0 ||
      sf.camera.tan_half_fov[0] <= 0 || sf.camera.tan_half_fov[1] <= 0 ||
      !summary.source || !summary.source->texture || summary.sample_slot == kNoSlot) return false;
  AmbientOcclusion::FrameCameraOut axes;
  float view[16];
  bool own;
  const bool bounce = enabled_ && menu_world::RulesFor(ao_->FrameWorld()).bounce &&
                      ao_->FrameCameraNow(&axes) && axes.have_rows && ao_->TriggerViewInverse(view, &own);
  if (!bounce && !has_direct && !gi_buffer) return false;
  if (!bounce) {
    std::memcpy(view, sf.camera.view_inverse, sizeof(view));
    ll::Rows rows;
    if (!ll::RowsFromViewInverse(view, &rows)) return false;
    std::copy_n(rows.right, 3, axes.right);
    std::copy_n(rows.up, 3, axes.up);
    std::copy_n(rows.forward, 3, axes.forward);
  }

  vpl::CompositeFrame input{};
  vpl::Frame& in = input.bounce;
  for (uint32_t i = 0; i < 3; ++i) {
    in.right[i] = axes.right[i];
    in.up[i] = axes.up[i];
    in.forward[i] = axes.forward[i];
    in.eye[i] = view[12 + i];
  }
  in.eye[3] = std::clamp(sf.camera.exposure_bias, 1.0f, 16.0f);
  beams_.clear();
  if (bounce) {
    float ambient[3], sun[3];
    double sun_dir[3];
    const float night_hint = ao_->FrameLight(ambient, sun, sun_dir) ? rt_hit::NightFactor(sun) : -1.0f;
    const float night = rt_world::ScheduleAt(larecomp::citypack::LampHours{}, world_clock::Hour(), night_hint).fill;
    Gather(in.eye.data(), night, field_share);
  }
  const bool has_beams = !beams_.empty();
  if (!has_beams && !has_direct && !gi_buffer) return false;

  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != sf.host_samples ||
      found->scale.Edge(found->width) < sf.render[0] || found->scale.Edge(found->height) < sf.render[1]) return false;
  const uint32_t target_width = found->scale.Edge(found->width);
  const uint32_t target_height = found->scale.Edge(found->height);
  bool unset = false;
  auto* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour || !colour->texture) return false;

  const uint32_t samples = sf.host_samples;
  const auto* top = rt_requested_ && rt_ && world_ && world_->Valid() ? &rt_->View() : nullptr;
  bool ray = has_beams && top && top->scene_valid && top->set;
  if (!EnsureResources(index)) return false;
  if (!EnsurePrograms(colour->format, samples, ray, has_beams, has_direct, combined)) {
    if (!ray || !EnsurePrograms(colour->format, samples, false, has_beams, has_direct, combined)) return false;
    ray = false;
  }
  Programs& program = programs_[samples == 8 ? 3 : samples == 4 ? 2 : samples == 2 ? 1 : 0];
  const uint32_t width = (sf.render[0] + 1) / 2, height = (sf.render[1] + 1) / 2;
  if (irradiance_.width != width || irradiance_.height != height) ReleaseImages();
  if ((has_beams || has_direct) &&
      (!passes_->EnsureImage(irradiance_, width, height, kIrradianceFormat, "vpl irradiance") ||
       !irradiance_.framebuffer || !irradiance_.texture)) return false;
  if (has_direct) {
    if (!passes_->EnsureImage(direct_irradiance_, width, height, kIrradianceFormat, "direct irradiance") ||
        !direct_irradiance_.texture) return false;
    if (!irradiance_mrt_) {
      const plume::RenderTexture* attachments[] = {irradiance_.texture.get(), direct_irradiance_.texture.get()};
      irradiance_mrt_ = passes_->CreateMrtFramebuffer(attachments, 2, "light irradiance");
      if (!irradiance_mrt_) return false;
    }
  }
  EdramTargets::Target* targets[] = {colour};
  auto* target = edram_->Framebuffer(targets, 1, nullptr);
  if (!target) return false;
  uint32_t main_sample = kNoSlot, unused = kNoSlot;
  if (has_beams && !edram_->SampleSlots(*colour, &main_sample, &unused)) main_sample = kNoSlot;

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
  in.apply[3] = has_direct ? direct_irradiance_.slot : kNoSlot;
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
  if (has_direct) input.direct = direct->frame;
  input.gi_push.fill(kNoSlot);
  if (gi_buffer && gi_push) std::memcpy(input.gi_push.data(), gi_push, sizeof(*gi_push));
  slot.set->setBuffer(3, has_direct ? direct->buffer : slot.input.get(),
                      has_direct ? direct->size : sizeof(input));
  slot.set->setBuffer(4, gi_buffer ? gi_buffer : slot.input.get(),
                      gi_buffer ? gi_apply::kBufferAlloc : sizeof(input));
  void* mapped = slot.input->map();
  if (!mapped) return false;
  std::memcpy(mapped, &input, sizeof(input));
  slot.input->unmap();

  // CHECK: confirm depth/colour and UAV handoffs on both APIs, including MSAA
  // sample zero in cockpit views and the world-frame transform on RT cuts.
  edram_->Transition(list, *summary.source, Layout::SHADER_READ);
  if (main_sample != kNoSlot) edram_->Transition(list, *colour, Layout::SHADER_READ);
  if (has_beams) {
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
    list->barriers(Stage::GRAPHICS, &hit_read, 1);
    hit_recorded_ = true;
    history_frame_ = frame_;
    history_time_ = now;
    generation_ = sf.generation;
  }
  if (has_beams || has_direct) {
    // CHECK: verify both attachment transitions with Vulkan validation and
    // D3D12's debug layer, including live toggles and a padded render target.
    plume::RenderTextureBarrier barriers[] = {
        {irradiance_.texture.get(), Layout::COLOR_WRITE},
        {direct_irradiance_.texture.get(), Layout::COLOR_WRITE},
    };
    list->barriers(Stage::GRAPHICS, barriers, has_direct ? 2u : 1u);
    if (combined) timing_->Mark(list, "light irradiance");
    else timing_->Mark(list, "vpl irradiance");
    const bool clear = irradiance_.layout == Layout::UNKNOWN ||
                       (has_direct && direct_irradiance_.layout == Layout::UNKNOWN);
    if (has_direct) {
      Draw(list, slot, *program.combined_irradiance, *irradiance_mrt_, width, height, clear ? 2u : 0u);
    } else {
      Draw(list, slot, *program.irradiance, *irradiance_.framebuffer, width, height, clear);
    }
    const plume::RenderTextureBarrier reads[] = {
        {irradiance_.texture.get(), Layout::SHADER_READ},
        {direct_irradiance_.texture.get(), Layout::SHADER_READ},
    };
    list->barriers(Stage::GRAPHICS, reads, has_direct ? 2u : 1u);
    irradiance_.layout = Layout::SHADER_READ;
    if (has_direct) direct_irradiance_.layout = Layout::SHADER_READ;
    irradiance_recorded_ = combined;
    idle_ = 0;
  }

  edram_->Transition(list, *colour, Layout::COLOR_WRITE);
  if (combined) timing_->Mark(list, "light composite");
  else timing_->Mark(list, "vpl composite");
  auto* composite = combined ? program.combined_composite.get() : program.composite.get();
  Draw(list, slot, *composite, *target, combined ? target_width : sf.render[0],
       combined ? target_height : sf.render[1], false);
  if (combined) timing_->Mark(list, "after light composite");
  else timing_->Mark(list, "after vpl");
  recorded_ = has_beams;
  composite_recorded_ = combined;
  const uint32_t path = ray ? 1u : 0u;
  if (has_beams && !logged_[path]) {
    logged_[path] = true;
    REXLOG_INFO("[gpu] vpl: {} placement, {} beams, {}x{} irradiance, {} samples",
                ray ? "ray query" : "depth chain", count, width, height, samples);
  }
  return true;
}

uint64_t VirtualBounce::HeldBytes() const {
  return uint64_t(irradiance_.width) * irradiance_.height * 8 + uint64_t(direct_irradiance_.width) * direct_irradiance_.height * 8 +
         (history_ ? kHistoryBytes : 0);
}

}  // namespace mcla::gpu
