// MCLA GPU renderer -- the player car's self-reflection, ray traced. See car_rt_refl.h.

#include "car_rt_refl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_hit_rules.h"
#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "texture_bytes.h"

// pipelines.cpp: the chrome variant's own switch (the pass leaves another car's chrome alone where the variant is not drawn).

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car rt refl]";
constexpr const char* kPipelineName = "car rt refl";
constexpr const char* kDenoiseName = "car rt refl denoise";
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8

LiveText& Setting() {
  static LiveText text("mcla_gpu_car_selfrefl_rt");
  return text;
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 0); }
uint64_t DenoiseKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 1); }

// CarRtReflAvailable()'s answer: set once the layout and the shader exist and the car's structures can be built.
std::atomic<bool> g_available{false};

}  // namespace

bool CarRtReflAvailable() { return g_available.load(std::memory_order_relaxed); }

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
  g_available.store(false, std::memory_order_relaxed);
  listed_ = false;
  pipeline_failed_ = false;
  supported_ = scene != nullptr && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan);
  if (!supported_) {
    passes.precache().Note("ray traced car self-reflections: not on this graphics card or driver (the screen-space version runs)", 0);
    return;
  }
  if (!MakeLayoutAndShader(device, passes, *scene)) {
    supported_ = false;
    passes.precache().Note("ray traced car self-reflections: the layout or the shader could not be made (the screen-space version runs)", 0);
    return;
  }
  ready_ = true;
  g_available.store(scene->ready(), std::memory_order_relaxed);
  // The denoise is optional: the trace runs, and its raw image is read, where its layout or shader could not be made.
  denoise_ready_ = MakeDenoiseLayoutAndShader(device, passes);
  denoise_failed_ = false;
  if (!denoise_ready_) {
    passes.precache().Note("ray traced car self-reflections: the denoise's layout or shader could not be made (the raw trace is read)", 0);
  }
  // The opponents' cars' pass (car_rt_opp_refl.h): optional, listed beside the trace's pipeline where the reflection is on at the start.
  opp_.Initialize(device, passes, heap, pass_timing, *scene, output_builder_,
                  reflection_on_at_start && car_rt_refl::SettingOf(Setting().Get()) == car_rt_refl::Setting::kAuto);
  // Listed with the start's precache only where the car self-reflections are on at the start and this is not switched off; else it is
  // made in the background when first wanted (PipelineReady).
  if (reflection_on_at_start && car_rt_refl::SettingOf(Setting().Get()) == car_rt_refl::Setting::kAuto) {
    uint32_t added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); }) ? 1u : 0u;
    if (denoise_ready_) {
      added += passes.precache().Add(DenoiseKey(), kDenoiseName, [this] { return MakeDenoisePipeline(); }) ? 1u : 0u;
    }
    listed_ = true;
    passes.precache().Note("ray traced car self-reflections: on (auto) where the car self-reflections are", added);
  } else {
    passes.precache().Note("ray traced car self-reflections: made when first wanted", 0);
  }
}

bool CarRtRefl::MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene) {
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  output_builder_.addReadWriteTexture(1);  // u1: the aux image (hit distance, instance, N.V) for the denoise
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[]
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);                // space 1: the outputs
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
  const HostShaderBlob blob = CarRtReflShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  return shader_ != nullptr;
}

bool CarRtRefl::MakeDenoiseLayoutAndShader(Device& device, HostPasses& passes) {
  denoise_builder_.begin();
  denoise_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  denoise_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[] (the history, the images)
  const uint32_t output_set = builder.addDescriptorSet(denoise_builder_);               // space 1: the output
  builder.addPushConstant(0, kDenoisePushSpace, sizeof(DenoisePush), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet) {
    REXLOG_ERROR("{} the denoise layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  denoise_layout_ = device.CreatePipelineLayout(builder, "car rt refl denoise layout");
  if (!denoise_layout_) {
    return false;
  }
  const HostShaderBlob blob = CarRtReflDenoiseShader();
  denoise_shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  return denoise_shader_ != nullptr;
}

void CarRtRefl::Shutdown() {
  // The host builder's function names this object's layout and shader: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  opp_.Shutdown();
  // The GPU is idle and the heap goes later: the slot is not handed back (CarSelfRefl::Shutdown).
  image_ = HostImage{};
  aux_ = HostImage{};
  scratch_ = HostImage{};
  trace_set_ = OutputSet{};
  first_set_ = OutputSet{};
  final_set_ = OutputSet{};
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  denoise_pipeline_.reset();
  denoise_shader_.reset();
  denoise_layout_.reset();
  denoise_ready_ = false;
  ready_ = false;
  g_available.store(false, std::memory_order_relaxed);
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

std::unique_ptr<plume::RenderPipeline> CarRtRefl::MakeDenoisePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(denoise_layout_.get(), denoise_shader_.get(), kGroup, kGroup, 1),
                                        kDenoiseName);
}

plume::RenderPipeline* CarRtRefl::DenoisePipeline() {
  if (denoise_pipeline_) {
    return denoise_pipeline_.get();
  }
  if (!denoise_ready_ || denoise_failed_ || !passes_->precache().Ready(DenoiseKey())) {
    return nullptr;  // not made yet (or never): this frame goes on with the raw image
  }
  denoise_pipeline_ = passes_->precache().Acquire(DenoiseKey(), kDenoiseName, [this] { return MakeDenoisePipeline(); });
  if (!denoise_pipeline_) {
    denoise_failed_ = true;
    REXLOG_ERROR("{} the denoise's pipeline could not be made: the raw trace is read this session", kPrefix);
  }
  return denoise_pipeline_.get();
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
    if (denoise_ready_) {
      fresh.push_back({DenoiseKey(), kDenoiseName, [this] { return MakeDenoisePipeline(); }});
    }
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
  const float self_reach = car_rt_refl::ClampedLength(float(larecomp::settings::mcla_gpu_car_selfrefl_rt_length.Get()));
  const uint32_t parts = uint32_t(std::clamp<int32_t>(larecomp::settings::mcla_gpu_car_selfrefl_rt_parts.Get(), 1, int32_t(car_rt_refl::kPartAll)));
  const float gloss = car_rt_refl::ClampedGloss(float(larecomp::settings::mcla_gpu_car_selfrefl_rt_gloss.Get()));
  const float tan_half = car_rt_refl::ConeTan(gloss);
  // RT-HIT-SHADING part two: the class's level (the reach to another car, the cone's rays, the clear coat), the two debug overrides on it. The ray is cast as far as the longer reach where the
  // parts it may hit include another car; a hit's weight falls over the reach of the part it is on (the shader's coat.x and coat.y).
  const car_rt_hit::Level level = car_rt_hit::LevelWith(car_rt_hit::LevelFor(gpu_class::Current()), double(larecomp::settings::mcla_gpu_car_rt_hit_reach.Get()),
                                                        double(larecomp::settings::mcla_gpu_car_rt_clearcoat.Get()));
  const float others_reach = level.others_reach > 0.0f ? car_rt_refl::ClampedLength(level.others_reach) : self_reach;
  const float length = car_rt_hit::CastReach(parts, self_reach, others_reach);  // what is cast, and what the aux image and the denoise decode distances by

  // The denoise (car_rt_refl.h DENOISE): the passes this frame, where the setting asks for them, the cone has width, the pipeline is made and
  // the scratch image is there; else the raw trace is what the paint reads.
  int denoise_passes = car_rt_refl::DenoisePassCount(int(larecomp::settings::mcla_gpu_car_selfrefl_rt_denoise.Get()), tan_half);
  plume::RenderPipeline* denoise = denoise_passes > 0 ? DenoisePipeline() : nullptr;
  if (denoise && !EnsureDenoiseImages(history.width, history.height)) {
    denoise = nullptr;
  }
  if (!denoise) {
    denoise_passes = 0;
    if (scratch_.texture) {
      ReleaseScratch();  // not needed while the denoise is off or cannot run
    }
  }

  // Another car's chrome (car_chrome_rt_rules.h): found by the primary ray too, and its probes traced, where the chrome variant is drawn at all and the switch for
  // this is on; else the primary ray is the player's body alone, as it was (no instance of the scene has the chrome's bit then that a ray here would take).
  const bool chrome_others = larecomp::settings::mcla_gpu_car_chrome_occlusion.Get() && larecomp::settings::mcla_gpu_car_chrome_rt_others.Get();
  Push push = {};
  push.slots[0] = history.slot;
  push.slots[1] = parts;
  push.slots[2] = chrome_others ? car_rt_refl::kPrimaryTraceParts : car_rt_refl::kPrimaryParts;
  push.slots[3] = car_rt_hit::PackPhaseRays(info.pattern_phase, level.cone_rays);
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
  push.tune[3] = tan_half;
  push.up[0] = view.up[0];
  push.up[1] = view.up[1];
  push.up[2] = view.up[2];
  push.up[3] = car_selfrefl::ExpBiasUnits(info.exp_bias);
  push.coat[0] = self_reach;
  push.coat[1] = others_reach;
  push.coat[2] = level.others_fade_from;
  push.coat[3] = level.coat;

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  // Moves up to four images to their layouts in one barrier and notes it (the layout the next pass finds them in).
  auto transition = [&](plume::RenderBarrierStages stages, std::initializer_list<std::pair<HostImage*, Layout>> changes) {
    plume::RenderTextureBarrier barriers[4];
    uint32_t count = 0;
    for (const auto& change : changes) {
      barriers[count++] = plume::RenderTextureBarrier(change.first->texture.get(), change.second);
      change.first->layout = change.second;
    }
    list->barriers(stages, nullptr, 0, barriers, count);
  };

  pass_timing_->Mark(list, "car rt refl");
  // The history is read by a compute shader here (and by the paint's pixel shaders later): a layout both can read. The images are written
  // as storage images; the one the paint reads is read by its pixel shaders afterwards.
  transition(Stage::GRAPHICS_AND_COMPUTE, {{&history, Layout::SHADER_READ}});
  transition(Stage::COMPUTE, {{&image_, Layout::GENERAL}, {&aux_, Layout::GENERAL}});
  // CHECK: CarRtScene leaves the top level built, and readable by a compute shader, when View().valid (a UAV barrier after its build).
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(trace_set_.set.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((history.width + kGroup - 1) / kGroup, (history.height + kGroup - 1) / kGroup, 1);

  // The opponents' cars (car_rt_opp_refl.h): the same pass for the paint of the cars beside the player, a second dispatch into the same two images (nothing is recorded where no
  // other car's paint is in the scene).
  {
    CarRtOppRefl::Frame opponents;
    opponents.list = list;
    opponents.view = &view;
    opponents.base_push = &push;
    opponents.base_push_bytes = sizeof(push);
    opponents.output_set = trace_set_.set.get();
    opponents.image = &image_;
    opponents.aux = &aux_;
    opponents.image_width = history.width;
    opponents.image_height = history.height;
    opponents.render_width = info.render_width;
    opponents.render_height = info.render_height;
    opponents.divisor = std::max<uint32_t>(info.divisor, 1);
    opponents.tan_x = info.tan_x;
    opponents.tan_y = info.tan_y;
    opponents.frame = info.frame;
    opp_.Record(opponents);
  }

  result_in_scratch_ = false;
  if (denoise_passes == 0) {
    transition(Stage::GRAPHICS, {{&image_, Layout::SHADER_READ}});
  } else {
    // First pass: the trace's image (and the aux image) are read, the scratch image written. Second pass: the scratch is read, the trace's image
    // written again. One pass leaves its result in the scratch image.
    pass_timing_->Mark(list, "car rt denoise");
    transition(Stage::COMPUTE, {{&image_, Layout::SHADER_READ}, {&aux_, Layout::SHADER_READ}, {&scratch_, Layout::GENERAL}});
    DispatchDenoise(list, denoise, history.slot, image_, first_set_, 0, history.width, history.height, float(info.tan_y), length, tan_half);
    if (denoise_passes >= 2) {
      transition(Stage::COMPUTE, {{&scratch_, Layout::SHADER_READ}, {&image_, Layout::GENERAL}});
      DispatchDenoise(list, denoise, history.slot, scratch_, final_set_, 1, history.width, history.height, float(info.tan_y), length, tan_half);
      transition(Stage::GRAPHICS, {{&image_, Layout::SHADER_READ}});
    } else {
      result_in_scratch_ = true;
      transition(Stage::GRAPHICS, {{&scratch_, Layout::SHADER_READ}});
    }
    ++denoise_runs_;
    if (!denoise_logged_) {
      denoise_logged_ = true;
      REXLOG_INFO("{} the first denoise: frame {}, {} pass{} over the {}x{} image (the cone's grain: car_rt_refl_rules.h), scratch image {:.1f} MiB, "
                  "aux image {:.1f} MiB",
                  kPrefix, info.frame, denoise_passes, denoise_passes == 1 ? "" : "es", history.width, history.height,
                  double(TextureBytes(scratch_.width, scratch_.height, 1, scratch_.format)) / (1024.0 * 1024.0),
                  double(TextureBytes(aux_.width, aux_.height, 1, aux_.format)) / (1024.0 * 1024.0));
    }
  }
  // What follows is not this pass's: closes its timing segment (PassTiming labels everything up to the next mark).
  pass_timing_->Mark(list, "after car rt refl");

  recorded_ = true;
  recorded_frame_ = info.frame;
  ++passes_run_;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first ray traced pass: frame {}, image {}x{} for a {}x{} picture (divisor {}), {} instances, reflection ray {} m (the player's own parts reach {} m, "
                "another car {} m from {:.0f}% of it), parts {}, {} rays over a cone of {} degrees{}; the clear-coat ray: {}; the sun's shadow at a shaded hit: {}; the other cars' chrome: {}; "
                "the colours the pass makes up from a paint constant are multiplied by {} (the history's units over the paint's, the main target's exponent bias {})",
                kPrefix, info.frame, history.width, history.height, info.render_width, info.render_height, info.divisor,
                view.instance_count, length, self_reach, others_reach, 100.0 * double(level.others_fade_from), parts,
                gloss > 0.0f ? std::min<uint32_t>(level.cone_rays, car_rt_refl::kConeRays) : 1, gloss,
                info.pattern_phase != 0 || info.upscaler ? " (the pattern turns with the frame: a temporal upscaler runs)" : "",
                level.coat > 0.0f && gloss > 0.0f && level.cone_rays >= 4 ? std::format("on, strength {:.2f} (mcla_gpu_car_rt_clearcoat)", level.coat) : std::string("off"),
                larecomp::settings::mcla_gpu_car_rt_hit_cascade.Get() ? "the game's cascades with the cars' ray (when the volumetric light has the cascades)" : "the cars' ray alone",
                chrome_others ? "traced (mcla_gpu_car_chrome_rt_others)" : "left to the marched version", push.up[3], info.exp_bias);
  }
  return true;
}

void CarRtRefl::DispatchDenoise(plume::RenderCommandList* list, plume::RenderPipeline* pipeline, uint32_t history_slot, const HostImage& input,
                                const OutputSet& output, uint32_t pass, uint32_t width, uint32_t height, float tan_y, float length,
                                float tan_half) {
  DenoisePush push = {};
  push.slots[0] = history_slot;
  push.slots[1] = input.slot;
  push.slots[2] = aux_.slot;
  push.slots[3] = pass;
  push.image[0] = float(width);
  push.image[1] = float(height);
  push.lens[0] = tan_y;
  push.lens[1] = length;
  push.lens[2] = tan_half;
  push.tune[0] = car_rt_refl::kDenoiseFootprintFrom;
  push.tune[1] = car_rt_refl::kDenoiseFootprintTo;
  push.tune[2] = car_rt_refl::kDenoiseSecondFrom;
  push.tune[3] = car_rt_refl::kDenoiseSecondTo;
  list->setComputePipelineLayout(denoise_layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(output.set.get(), kOutputSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((width + kGroup - 1) / kGroup, (height + kGroup - 1) / kGroup, 1);
}

bool CarRtRefl::EnsureOutput(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* image_before = image_.texture.get();
  const plume::RenderTexture* aux_before = aux_.texture.get();
  // CHECK: the aux image is R8G8B8A8_UNORM with STORAGE | UNORDERED_ACCESS: written by the trace as a typed UAV (u1) and read by the denoise through
  // the bindless Texture2D<float4> heap (HostPasses::EnsureImage gives it a slot and a view of its own format).
  if (!passes_->EnsureImage(image_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "car rt refl image", kFlags) ||
      !passes_->EnsureImage(aux_, width, height, plume::RenderFormat::R8G8B8A8_UNORM, "car rt refl aux", kFlags)) {
    // EnsureImage released the one that failed and logged it; the other goes too (the pass needs both).
    ReleaseImage();
    return false;
  }
  // A new texture is a new set for it (the old one goes after the lists that name it); the denoise's last pass writes the image too.
  if (image_.texture.get() != image_before) {
    RetireSet(final_set_);
    RetireSet(trace_set_);
  }
  if (aux_.texture.get() != aux_before) {
    RetireSet(trace_set_);
  }
  if (!EnsureSet(trace_set_, output_builder_, image_, &aux_, "car rt refl output set")) {
    ReleaseImage();
    return false;
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} image {}x{} R16G16B16A16_FLOAT and aux image R8G8B8A8_UNORM ({:.1f} MiB, with the denoise's scratch image when it runs)", kPrefix,
                width, height, double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return true;
}

bool CarRtRefl::EnsureDenoiseImages(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = scratch_.texture.get();
  if (!passes_->EnsureImage(scratch_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "car rt refl scratch", kFlags)) {
    RetireSet(first_set_);
    return false;
  }
  if (scratch_.texture.get() != before) {
    RetireSet(first_set_);
  }
  if (!EnsureSet(first_set_, denoise_builder_, scratch_, nullptr, "car rt refl denoise first set") ||
      !EnsureSet(final_set_, denoise_builder_, image_, nullptr, "car rt refl denoise final set")) {
    ReleaseScratch();
    return false;
  }
  return true;
}

bool CarRtRefl::EnsureSet(OutputSet& out, plume::RenderDescriptorSetBuilder& builder, const HostImage& first, const HostImage* second,
                          const char* name) {
  const plume::RenderTexture* first_texture = first.texture.get();
  const plume::RenderTexture* second_texture = second ? second->texture.get() : nullptr;
  if (out.set && out.first == first_texture && out.second == second_texture) {
    return true;
  }
  RetireSet(out);
  out.set = device_->CreateDescriptorSet(builder, second ? 2u : 1u, 0, name);
  if (!out.set) {
    return false;
  }
  out.set->setTexture(0, first_texture, plume::RenderTextureLayout::GENERAL, first.view.get());
  if (second) {
    out.set->setTexture(1, second_texture, plume::RenderTextureLayout::GENERAL, second->view.get());
  }
  out.first = first_texture;
  out.second = second_texture;
  return true;
}

void CarRtRefl::RetireSet(OutputSet& out) {
  if (out.set && frames_) {
    frames_->Retire(std::move(out.set));
  }
  out = OutputSet{};
}

void CarRtRefl::ReleaseScratch() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(scratch_);
  RetireSet(first_set_);
}

uint32_t CarRtRefl::SlotFor(uint64_t history_frame) const {
  const HostImage& result = result_in_scratch_ ? scratch_ : image_;
  if (!recorded_ || recorded_frame_ != history_frame || !result.texture || result.slot == kNoSlot) {
    return car_selfrefl::kNoRtImage;
  }
  return result.slot;
}

void CarRtRefl::ReleaseImage() {
  recorded_ = false;
  result_in_scratch_ = false;
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  passes_->ReleaseImage(aux_);
  passes_->ReleaseImage(scratch_);
  RetireSet(trace_set_);
  RetireSet(first_set_);
  RetireSet(final_set_);
  image_for_[0] = image_for_[1] = 0;
}

uint64_t CarRtRefl::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* held : {&image_, &aux_, &scratch_}) {
    bytes += held->texture ? TextureBytes(held->width, held->height, 1, held->format) : 0;
  }
  return bytes;
}

std::string CarRtRefl::CensusText() {
  if (!ready_) {
    return {};
  }
  const uint64_t interval = passes_run_ - census_passes_;
  census_passes_ = passes_run_;
  return std::format("; ray traced: {} passes since the start ({} in the interval, {} denoised), {} frames wanted but without the car's structures, {} "
                     "waiting for the pipeline",
                     passes_run_, interval, denoise_runs_, no_scene_, not_ready_) +
         opp_.CensusText();
}

}  // namespace mcla::gpu
