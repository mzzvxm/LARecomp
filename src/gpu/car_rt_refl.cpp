// MCLA GPU renderer -- the player car's self-reflection, ray traced. See car_rt_refl.h.

#include "car_rt_refl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"
#include "live_text.h"
#include "pass_timing.h"
#include "texture_bytes.h"

// The compute shader's blob: built for D3D12 only (larecomp_gpu_host_shader_rt: cs_6_5 DXIL), the one backend that traces the car.
#if defined(LARECOMP_GPU_HAS_D3D12)
#include "car_rt_refl_cs.hlsl.dxil.h"  // CHECK: the name and variable (g_car_rt_refl_cs_dxil) larecomp_gpu_host_shader_rt gives, as the other host shaders'
#define MCLA_CAR_RT_REFL_HAS_SHADER 1
#else
#define MCLA_CAR_RT_REFL_HAS_SHADER 0
#endif

REXCVAR_DEFINE_STRING(
    mcla_gpu_car_selfrefl_rt, "auto", "MCLA/GPU",
    "Ray traced car self-reflections: where the graphics card can ray trace (Direct3D 12 with ray tracing 1.1), your car's reflection of "
    "its own body comes from its real shape, not from the picture of the car on the screen: a part the camera cannot see (the back of the "
    "wing, a mirror's far side) shows in the paint as it does in a mirror, and thin parts are exact. auto (the default) is on there while the "
    "car self-reflections are on; off keeps the screen-space version everywhere. A graphics card that cannot ray trace uses the "
    "screen-space version whatever this says. Live.")
    .allowed({"auto", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_selfrefl_rt_length, 4.0, "MCLA/GPU",
    "Diagnostic: how far the ray traced car self-reflection's ray reaches, in the game's world units (about metres); what it finds weighs "
    "less from 0.6 of it and nothing at it. Live.")
    .debug_only()
    .range(0.5, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_car_selfrefl_rt_parts, 7, "MCLA/GPU",
    "Diagnostic: the parts of your car the ray traced self-reflection's ray can hit, a sum of: 1 the body, 2 the glass, 4 the tyres, 8 the "
    "driver. 7 (the default): the body, the glass and the tyres. Live.")
    .debug_only()
    .range(1, 15)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car rt refl]";
constexpr const char* kPipelineName = "car rt refl";
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8

LiveText& Setting() {
  static LiveText text("mcla_gpu_car_selfrefl_rt");
  return text;
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 0); }

}  // namespace

CarRtRefl::~CarRtRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarRtRefl::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing,
                           CarRtScene* scene, bool reflection_on_at_start) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  scene_ = scene;
  ready_ = false;
  listed_ = false;
  pipeline_failed_ = false;
  supported_ = scene != nullptr && MCLA_CAR_RT_REFL_HAS_SHADER != 0 &&
               CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan);
  if (!supported_) {
    passes.precache().Note("ray traced car self-reflections: not on this graphics card or backend (the screen-space version runs)", 0);
    return;
  }
  if (!MakeLayoutAndShader(device, passes, *scene)) {
    supported_ = false;
    passes.precache().Note("ray traced car self-reflections: the layout or the shader could not be made (the screen-space version runs)", 0);
    return;
  }
  ready_ = true;
  // Listed with the start's precache only where the car self-reflections are on at the start and this is not switched off; else it is
  // made in the background when first wanted (PipelineReady).
  if (reflection_on_at_start && car_rt_refl::SettingOf(Setting().Get()) == car_rt_refl::Setting::kAuto) {
    const bool added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
    listed_ = true;
    passes.precache().Note("ray traced car self-reflections: on (auto) where the car self-reflections are", added ? 1u : 0u);
  } else {
    passes.precache().Note("ray traced car self-reflections: made when first wanted", 0);
  }
}

bool CarRtRefl::MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene) {
#if MCLA_CAR_RT_REFL_HAS_SHADER
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[]
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);                // space 1: the output
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                 // space 2: the car's structures (CAR_RT_SPACE)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  layout_ = device.CreatePipelineLayout(builder, "car rt refl layout");
  if (!layout_) {
    return false;
  }
  shader_ = passes.CreateShader(g_car_rt_refl_cs_dxil, sizeof(g_car_rt_refl_cs_dxil), "car_rt_refl_cs");
  return shader_ != nullptr;
#else
  (void)device;
  (void)passes;
  (void)scene;
  return false;
#endif
}

void CarRtRefl::Shutdown() {
  // The host builder's function names this object's layout and shader: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slot is not handed back (CarSelfRefl::Shutdown).
  image_ = HostImage{};
  output_set_.reset();
  output_set_texture_ = nullptr;
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_ = false;
  recorded_ = false;
  active_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  scene_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipeline
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> CarRtRefl::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, kGroup, 1), kPipelineName);
}

plume::RenderPipeline* CarRtRefl::Pipeline() {
  if (pipeline_) {
    return pipeline_.get();
  }
  if (pipeline_failed_) {
    return nullptr;
  }
  pipeline_ = passes_->precache().Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the pass's pipeline could not be made: the screen-space car self-reflection this session", kPrefix);
  }
  return pipeline_.get();
}

bool CarRtRefl::PipelineReady() {
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
    precache.AddLive("ray traced car self-reflections", std::move(fresh));
    REXLOG_INFO("{} ray traced car self-reflections were not listed at the start: the pipeline is made in the background, and the pass runs "
                "from the frame it is done",
                kPrefix);
  }
  return precache.Ready(PipelineKey());
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CarRtRefl::BeginFrame(bool reflection_on) {
  active_ = false;
  if (!ready_ || !scene_) {
    return;
  }
  const bool wanted = car_rt_refl::Wanted(car_rt_refl::SettingOf(Setting().Get()), supported_, reflection_on);
  if (!wanted) {
    return;
  }
  if (!PipelineReady()) {
    ++not_ready_;
    return;
  }
  active_ = true;
  // The scene builds a frame's structures only for a frame that asked in the one before (as CarFrame): ask in every frame this is on.
  // CHECK: the name of CarRtScene's per-frame request (written as CarFrame::Request, car_frame.h).
  scene_->Request();
}

bool CarRtRefl::OnHistory(plume::RenderCommandList* list, const HistoryInfo& info) {
  recorded_ = false;
  if (!list || !active_ || !ready_ || !info.image || !info.image->texture || info.image->slot == kNoSlot) {
    return false;
  }
  HostImage& history = *info.image;
  const CarRtView& view = scene_->View();
  if (!view.valid || !view.set) {
    // The scene had no top level for this frame (the car is not found, the first frame after a request, a mesh not read): the march.
    ++no_scene_;
    if (!no_scene_logged_) {
      no_scene_logged_ = true;
      REXLOG_INFO("{} no acceleration structures of the car at the first trigger that wanted them (frame {}): the screen-space version "
                  "runs there; counted in the census",
                  kPrefix, info.frame);
    }
    return false;
  }
  plume::RenderPipeline* pipeline = Pipeline();
  if (!pipeline || !EnsureOutput(history.width, history.height)) {
    return false;
  }
  const float length = car_rt_refl::ClampedLength(float(REXCVAR_GET(mcla_gpu_car_selfrefl_rt_length)));
  const uint32_t parts = uint32_t(std::clamp<int32_t>(REXCVAR_GET(mcla_gpu_car_selfrefl_rt_parts), 1, int32_t(car_rt_refl::kPartAll)));

  Push push = {};
  push.slots[0] = history.slot;
  push.slots[1] = parts;
  push.slots[2] = car_rt_refl::kPrimaryParts;
  push.image[0] = float(history.width);
  push.image[1] = float(history.height);
  push.image[2] = float(info.render_width);
  push.image[3] = float(info.render_height);
  push.lens[0] = float(info.tan_x);
  push.lens[1] = float(info.tan_y);
  push.lens[2] = float(std::max<uint32_t>(info.divisor, 1));
  push.lens[3] = length;
  push.tune[0] = car_rt_refl::kShade;
  push.tune[1] = car_rt_refl::kHiddenWeight;
  push.tune[2] = car_rt_refl::kFadeFrom;

  pass_timing_->Mark(list, "car rt refl");
  // The history is read by a compute shader here (and by the paint's pixel shaders later): a layout both can read. The image is written
  // as a storage image, and read by the paint's pixel shaders afterwards.
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                 plume::RenderTextureBarrier(history.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  history.layout = plume::RenderTextureLayout::SHADER_READ;
  list->barriers(plume::RenderBarrierStage::COMPUTE,
                 plume::RenderTextureBarrier(image_.texture.get(), plume::RenderTextureLayout::GENERAL));
  image_.layout = plume::RenderTextureLayout::GENERAL;
  // CHECK: CarRtScene leaves the top level built, and readable by a compute shader, when View().valid (a UAV barrier after its build).
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(output_set_.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((history.width + kGroup - 1) / kGroup, (history.height + kGroup - 1) / kGroup, 1);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image_.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image_.layout = plume::RenderTextureLayout::SHADER_READ;

  recorded_ = true;
  recorded_frame_ = info.frame;
  ++passes_run_;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first ray traced pass: frame {}, image {}x{} for a {}x{} picture (divisor {}), {} instances, reflection ray {} m, parts {}",
                kPrefix, info.frame, history.width, history.height, info.render_width, info.render_height, info.divisor,
                view.instance_count, length, parts);
  }
  return true;
}

bool CarRtRefl::EnsureOutput(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  if (!passes_->EnsureImage(image_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "car rt refl image", kFlags)) {
    return false;
  }
  if (!output_set_ || output_set_texture_ != image_.texture.get()) {
    // A new texture: a new set for it; the old one goes after the lists that name it.
    if (output_set_) {
      frames_->Retire(std::move(output_set_));
    }
    output_set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, "car rt refl output set");
    if (!output_set_) {
      passes_->ReleaseImage(image_);
      output_set_texture_ = nullptr;
      return false;
    }
    output_set_->setTexture(0, image_.texture.get(), plume::RenderTextureLayout::GENERAL, image_.view.get());
    output_set_texture_ = image_.texture.get();
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} image {}x{} R16G16B16A16_FLOAT ({:.1f} MiB)", kPrefix, width, height, double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return true;
}

uint32_t CarRtRefl::SlotFor(uint64_t history_frame) const {
  if (!recorded_ || recorded_frame_ != history_frame || !image_.texture || image_.slot == kNoSlot) {
    return car_selfrefl::kNoRtImage;
  }
  return image_.slot;
}

void CarRtRefl::ReleaseImage() {
  recorded_ = false;
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  if (output_set_ && frames_) {
    frames_->Retire(std::move(output_set_));
  }
  output_set_.reset();
  output_set_texture_ = nullptr;
  image_for_[0] = image_for_[1] = 0;
}

uint64_t CarRtRefl::HeldBytes() const {
  return image_.texture ? TextureBytes(image_.width, image_.height, 1, image_.format) : 0;
}

std::string CarRtRefl::CensusText() {
  if (!ready_) {
    return {};
  }
  const uint64_t interval = passes_run_ - census_passes_;
  census_passes_ = passes_run_;
  return std::format("; ray traced: {} passes since the start ({} in the interval), {} frames wanted but without the car's structures, {} "
                     "waiting for the pipeline",
                     passes_run_, interval, no_scene_, not_ready_);
}

}  // namespace mcla::gpu
