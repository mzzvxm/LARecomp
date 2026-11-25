// MCLA GPU renderer -- ambient occlusion and contact shadows around the player's car, ray traced. See car_rt_ao.h.

#include "car_rt_ao.h"
#include "hlsl/cabin_shadow_rules.hlsli"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_frame.h"
#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "rt_tier.h"
#include "texture_bytes.h"


namespace mcla::gpu {

namespace {

namespace rules = car_rt_ao;

constexpr const char* kPrefix = "[car rt ao]";
constexpr const char* kPipelineName = "car rt ao";
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8
// The pipeline's place among the occlusion's: the same owner, a variant past ao.h's Variant numbers (those stay under 0x100).
constexpr uint32_t kPipelineVariant = 0x100;
// The image goes back after this many frames without the pass (2 s at 60).
constexpr uint32_t kReleaseAfterFrames = 120;
// The census line says the totals every this many passes.
constexpr uint64_t kLogEvery = 3600;

LiveText& Setting() {
  static LiveText text("mcla_gpu_ao_rt");
  return text;
}

rules::Setting SettingNow() {
  if (gpu_class::CurrentRtRung() == gpu_class::RtTier::kNone) return rules::Setting::kOff;
  return rules::SettingOf(Setting().Get());
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kAmbientOcclusion, kPipelineVariant); }

float ReadReach() { return rules::ClampedReach(float(larecomp::settings::mcla_gpu_ao_rt_length.Get())); }

uint32_t ReadRays(bool ultra) {
  const int32_t asked = larecomp::settings::mcla_gpu_ao_rt_rays.Get();
  return asked > 0 ? uint32_t(std::min<int32_t>(asked, int32_t(rules::kMaxRays))) : rules::OcclusionRays(ultra);
}

}  // namespace

CarRtAo::~CarRtAo() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarRtAo::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* scene,
                         const CarFrame* car_frame, bool listed_at_start) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  scene_ = scene;
  car_frame_ = car_frame;
  ready_ = false;
  listed_ = false;
  pipeline_failed_ = false;
  supported_ = scene != nullptr && scene->ready() && car_frame != nullptr &&
               CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops());
  if (!supported_) {
    passes.precache().Note("ray traced ambient occlusion and contact shadows: not on this graphics card or backend (the screen-space passes run)", 0);
    return;
  }
  if (!MakeLayoutAndShader(device, passes, *scene)) {
    supported_ = false;
    passes.precache().Note("ray traced ambient occlusion and contact shadows: the layout or the shader could not be made (the screen-space passes run)", 0);
    return;
  }
  ready_ = true;
  // Listed with the start's precache only where the occlusion or the contact shadows are on at the start and the setting is not off; else it is made
  // in the background when first wanted (PipelineReady).
  if (listed_at_start && SettingNow() != rules::Setting::kOff) {
    const uint32_t added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); }) ? 1u : 0u;
    listed_ = true;
    passes.precache().Note("ray traced ambient occlusion and contact shadows: on where occlusion or contact shadows are active", added);
  } else {
    passes.precache().Note("ray traced ambient occlusion and contact shadows: made when first wanted", 0);
  }
}

bool CarRtAo::MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene) {
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[] (the depth image)
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);                // space 1: the output
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                 // space 2: the car's structures (CAR_RT_SPACE)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  layout_ = device.CreatePipelineLayout(builder, "car rt ao layout");
  if (!layout_) {
    return false;
  }
  const HostShaderBlob blob = CarRtAoShader();  // the active backend's: DXIL, or SPIR-V 1.5 (the car RT passes' blobs, host_shaders.h)
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  return shader_ != nullptr;
}

void CarRtAo::Shutdown() {
  // The host builder's function names this object's layout and shader: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slot is not handed back (CarRtRefl::Shutdown).
  image_ = HostImage{};
  set_.reset();
  set_texture_ = nullptr;
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_ = false;
  wanted_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  scene_ = nullptr;
  car_frame_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipeline
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> CarRtAo::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, kGroup, 1), kPipelineName);
}

plume::RenderPipeline* CarRtAo::Pipeline() {
  if (pipeline_) {
    return pipeline_.get();
  }
  if (pipeline_failed_) {
    return nullptr;
  }
  pipeline_ = passes_->precache().Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the pass's pipeline could not be made: the screen-space occlusion and contact shadows this session", kPrefix);
  }
  return pipeline_.get();
}

bool CarRtAo::PipelineReady() {
  if (pipeline_) {
    return true;
  }
  if (pipeline_failed_) {
    return false;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    // Off at the start, wanted now: made on a worker of their own.
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({PipelineKey(), kPipelineName, [this] { return MakePipeline(); }});
    listed_ = true;
    precache.AddLive("ray traced ambient occlusion and contact shadows", std::move(fresh));
    REXLOG_INFO("{} the ray traced pass was not listed at the start: the pipeline is made in the background, and the pass runs from the frame it is done",
                kPrefix);
  }
  return precache.Ready(PipelineKey());
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CarRtAo::BeginFrame(bool occlusion_on, bool contact_on) {
  wanted_ = false;
  if (!ready_ || !scene_) {
    return;
  }
  // The image goes back once the pass has not run for a while (Record resets the count).
  if (image_.texture && ++idle_frames_ >= kReleaseAfterFrames) {
    ReleaseImage();
    idle_frames_ = 0;
  }
  setting_ = SettingNow();
  // The coarse decision, from the effects' levels: the frame's own gates (the contact shadows' sun and rain, the bounce light) come with Record.
  const rules::Terms coarse = rules::Wanted(setting_, true, occlusion_on, contact_on);
  if (!coarse.any()) {
    return;
  }
  if (!PipelineReady()) {
    ++not_ready_;
    return;
  }
  wanted_ = true;
  // The scene builds a frame's structures only for a frame that asked in the one before (as CarFrame): ask in every frame this is on.
  scene_->Request();
}

CarRtAo::Result CarRtAo::Record(plume::RenderCommandList* list, const Inputs& in) {
  Result result;
  if (!list || !wanted_ || !ready_ || !in.depth || !in.depth->texture || in.depth->slot == kNoSlot) {
    return result;
  }
  const rules::Terms terms = rules::Wanted(setting_, true, in.occlusion, in.contact);
  if (!terms.any()) {
    return result;
  }
  const CarRtView& view = scene_->View();
  if (!view.valid || !view.set) {
    // The scene had no top level for this frame (the car is not found, the first frame after a request, a mesh not read).
    ++no_scene_;
    if (!no_scene_logged_) {
      no_scene_logged_ = true;
      REXLOG_INFO("{} no acceleration structures of the car at the first frame that wanted them (frame {}): the screen-space passes run there; counted "
                  "in the census (the structures are built before the occlusion's passes only in a frame the scene is asked for in the one before)",
                  kPrefix, in.frame);
    }
    return result;
  }
  // The region: the player's car's box in view space, as the top level's instances are placed, and the other cars' boxes (the scene's, from their
  // instances' transformed mesh boxes: car_rt_instances_rules.h), each grown a little.
  const car_frame::Frame& frame = car_frame_->Current();
  rules::Box box;
  const double position[3] = {frame.player_world.m[3][0], frame.player_world.m[3][1], frame.player_world.m[3][2]};
  if (!frame.valid ||
      !rules::ViewBoxFor(frame.view, position, frame.bounds_valid, frame.bounds_min, frame.bounds_max, rules::kBoxMargin, &box)) {
    ++no_box_;
    if (!no_box_logged_) {
      no_box_logged_ = true;
      REXLOG_INFO("{} the car's box in the camera's space is not known at the first frame that wanted it (frame {}: no car in the frame, or the car is within "
                  "half a metre of the camera's plane): the screen-space passes run there; counted in the census",
                  kPrefix, in.frame);
    }
    return result;
  }
  const bool have_sun = in.to_sun[0] * in.to_sun[0] + in.to_sun[1] * in.to_sun[1] + in.to_sun[2] * in.to_sun[2] > 0.5f;
  const bool sun = terms.contact && have_sun;
  const uint32_t rays = ReadRays(in.ultra);
  const float reach = ReadReach();
  std::array<rules::Box, rules::kMaxBoxes> region;
  uint32_t boxes = 0;
  region[boxes++] = box;
  const uint32_t others = view.boxes ? std::min(view.box_count, rules::kMaxOtherBoxes) : 0u;
  for (uint32_t i = 0; i < others; ++i) {
    region[boxes++] = rules::GrownBox(view.boxes[i], rules::kOtherBoxMargin);
  }
  const rules::Rect rect = rules::RegionRect(region.data(), boxes, terms.occlusion ? reach : 0.0f, in.to_sun, sun ? rules::kSunReach : 0.0f, in.tan_x,
                                             in.tan_y, in.depth->width, in.depth->height);
  if (rect.empty() || (!terms.occlusion && !sun)) {
    ++no_box_;
    return result;  // the car is off the picture
  }
  plume::RenderPipeline* pipeline = Pipeline();
  if (!pipeline || !EnsureOutput(in.depth->width, in.depth->height)) {
    return result;
  }

  Push push = {};
  push.slots[0] = in.depth->slot;
  push.slots[1] = in.depth->width;
  push.slots[2] = in.depth->height;
  push.slots[3] = (terms.occlusion ? rules::kPushOcclusion : 0u) | (sun ? rules::kPushSun : 0u) | (rays << rules::kPushRaysShift) |
                  (others << rules::kPushBoxesShift) | (sun && in.cabin_sun ? cabin_shadow::kCabinRtFlag : 0u);
  push.rect[0] = rect.x0;
  push.rect[1] = rect.y0;
  push.rect[2] = rect.x1;
  push.rect[3] = rect.y1;
  push.noise[0] = in.noise_code;
  push.noise[1] = rules::kRayPartsWithOthers;  // the player's body and tyres, and every part of another car
  push.noise[2] = in.sun_frame;
  push.noise[3] = view.box_row;  // the first row of the other cars' boxes in the car RT table (two rows a box)
  push.lens[0] = in.tan_x;
  push.lens[1] = in.tan_y;
  push.lens[2] = reach;
  push.lens[3] = in.fade_end;
  for (int c = 0; c < 3; ++c) {
    push.light[c] = have_sun ? in.to_sun[c] : 0.0f;
    push.box_min[c] = box.min[c];
    push.box_max[c] = box.max[c];
  }
  push.light[3] = rules::kSunDiscTan;
  push.box_min[3] = rules::kSunReach;

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  pass_timing_->Mark(list, "ao rt");
  // The depth image was left readable by the pixel pass that made it; a compute shader reads it here (a layout both can read, as the reflection
  // pass takes the history's). The output is written as a storage image and then read by the pixel passes that follow.
  {
    const plume::RenderTextureBarrier read(in.depth->texture.get(), Layout::SHADER_READ);
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, &read, 1);
    const plume::RenderTextureBarrier write(image_.texture.get(), Layout::GENERAL);
    list->barriers(Stage::COMPUTE, nullptr, 0, &write, 1);
    image_.layout = Layout::GENERAL;
  }
  // CHECK: CarRtScene leaves the top level built, and readable by a compute shader, when View().valid (a barrier after its build).
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(set_.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((image_.width + kGroup - 1) / kGroup, (image_.height + kGroup - 1) / kGroup, 1);
  {
    const plume::RenderTextureBarrier read(image_.texture.get(), Layout::SHADER_READ);
    list->barriers(in.compute_read ? Stage::GRAPHICS_AND_COMPUTE : Stage::GRAPHICS, nullptr, 0, &read, 1);
    image_.layout = Layout::SHADER_READ;
  }

  result.recorded = true;
  result.slot = image_.slot;
  result.occlusion = terms.occlusion;
  result.contact = sun;
  idle_frames_ = 0;
  ++passes_run_;
  rect_pixels_ += rect.area();
  rays_estimated_ += uint64_t(rules::EstimatedRays(rect.area(), terms.occlusion ? rays : 0u, sun));
  LogState(terms, in, rect, rays, reach, view.instance_count, box, others);
  return result;
}

void CarRtAo::LogState(const rules::Terms& terms, const Inputs& in, const rules::Rect& rect, uint32_t rays, float reach, uint32_t instances,
                       const rules::Box& box, uint32_t other_boxes) {
  const uint32_t state = (terms.occlusion ? 1u : 0u) | (terms.contact ? 2u : 0u) | (rays << 4) | (std::min(other_boxes, 1u) << 8);
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first ray traced pass: frame {}, image {}x{} (the occlusion's traced size), {} instances; occlusion {} ({} rays a pixel, reach {:.2f} m, "
                "the visibility pass takes the darker of its own and this one's), sun ray {} (the contact march takes the darker shadow); the car's box in "
                "view space ({:.2f} {:.2f} {:.2f}) to ({:.2f} {:.2f} {:.2f}) and {} other cars' boxes, the pixel rectangle {}x{} at ({}, {}), about {:.2f} "
                "million rays estimated (pass timing label \"ao rt\"; mcla_gpu_ao_rt off compares)",
                kPrefix, in.frame, in.depth->width, in.depth->height, instances, terms.occlusion ? "on" : "off", rays, reach, terms.contact ? "on" : "off",
                box.min[0], box.min[1], box.min[2], box.max[0], box.max[1], box.max[2], other_boxes, rect.width(), rect.height(), rect.x0, rect.y0,
                rules::EstimatedRays(rect.area(), terms.occlusion ? rays : 0u, terms.contact) / 1.0e6);
    last_state_ = state;
  } else if (state != last_state_) {
    last_state_ = state;
    REXLOG_INFO("{} frame {}: occlusion {} ({} rays a pixel), sun ray {}, {} (the settings, the effects' levels or the cars beside the player's changed)",
                kPrefix, in.frame, terms.occlusion ? "on" : "off", rays, terms.contact ? "on" : "off",
                other_boxes != 0 ? "other cars in the region" : "the player's car alone");
  }
  if (passes_run_ % kLogEvery == 0) {
    REXLOG_INFO("{} {} passes since the start, about {:.1f} million rays estimated, {} frames wanted without the car's structures, {} without its box, {} "
                "waiting for the pipeline",
                kPrefix, passes_run_, double(rays_estimated_) / 1.0e6, no_scene_, no_box_, not_ready_);
  }
}

bool CarRtAo::EnsureOutput(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = image_.texture.get();
  if (!passes_->EnsureImage(image_, width, height, plume::RenderFormat::R8G8B8A8_UNORM, "car rt ao image", kFlags)) {
    RetireSet();
    return false;
  }
  if (image_.texture.get() != before) {
    RetireSet();  // a new texture is a new set for it (the old one goes after the lists that name it)
  }
  if (!set_) {
    set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, "car rt ao output set");
    if (!set_) {
      return false;
    }
    set_->setTexture(0, image_.texture.get(), plume::RenderTextureLayout::GENERAL, image_.view.get());
    set_texture_ = image_.texture.get();
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} image {}x{} R8G8B8A8_UNORM ({:.1f} MiB), given back after {} frames without the pass", kPrefix, width, height,
                double(HeldBytes()) / (1024.0 * 1024.0), kReleaseAfterFrames);
  }
  return true;
}

void CarRtAo::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_texture_ = nullptr;
}

void CarRtAo::ReleaseImage() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  RetireSet();
  image_for_[0] = image_for_[1] = 0;
}

uint64_t CarRtAo::HeldBytes() const {
  return image_.texture ? TextureBytes(image_.width, image_.height, 1, image_.format) : 0;
}

std::string CarRtAo::CensusText() const {
  if (!ready_) {
    return {};
  }
  return std::format("; ray traced occlusion and contact: {} passes since the start, {} frames without the car's structures, {} without its box, {} waiting "
                     "for the pipeline",
                     passes_run_, no_scene_, no_box_, not_ready_);
}

}  // namespace mcla::gpu
