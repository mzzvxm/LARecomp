// MCLA GPU renderer -- the player car's self-reflection, ray traced. See car_rt_refl.h.

#include "car_rt_refl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
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
#include "noise_resolve.h"
#include "pass_timing.h"
#include "rt_tier.h"
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

car_rt_refl::Setting SettingNow() {
  return car_rt_refl::SettingOf(Setting().Get());
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 0); }
uint64_t DenoiseKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 1); }
// (variant 2 is the opponents' trace, car_rt_opp_refl.cpp; 3 is the player's glass trace and 4 to 7 its apply, car_rt_glass_refl.cpp) RR-4b: the raw mode's hit distance pass. It had 3 too, so in a Ray
// Reconstruction frame the host precache handed it the glass trace's pipeline (the same key, listed first at the start): the hit pass dispatched the glass shader under the denoise's two-set layout (the Vulkan
// validation layer's VUID-vkCmdDispatch-None-08600) and the glass trace had to be made again on the render thread. One number each: host_precache.h lists them.
uint64_t HitKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 8); }
constexpr const char* kHitName = "car rt refl hit distance (Ray Reconstruction)";

// CarRtReflAvailable()'s answer: set once the layout and the shader exist and the car's structures can be built.
std::atomic<bool> g_available{false};

}  // namespace

// The paint row requires ray-query support and an initialized trace path.
bool CarRtReflAvailable() {
  return g_available.load(std::memory_order_relaxed) && gpu_class::CurrentRtTier() != gpu_class::RtTier::kNone;
}

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
  supported_ = scene != nullptr && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops());
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
  const bool paint_at_start = reflection_on_at_start &&
      SettingNow() == car_rt_refl::Setting::kOn;
  opp_.Initialize(device, passes, heap, pass_timing, *scene, output_builder_, city_.Builder(), paint_at_start);
  // The player's windows (car_rt_glass_refl.h): optional too, on the same layout of sets.
  glass_.Initialize(device, passes, heap, frames, pass_timing, *scene, output_builder_, city_.Builder(), paint_at_start);
  // Listed with the start's precache only where the car self-reflections are on at the start and this is not switched off; else it is
  // made in the background when first wanted (PipelineReady).
  if (paint_at_start) {
    uint32_t added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); }) ? 1u : 0u;
    if (denoise_ready_) {
      added += passes.precache().Add(DenoiseKey(), kDenoiseName, [this] { return MakeDenoisePipeline(); }) ? 1u : 0u;
    }
    listed_ = true;
    passes.precache().Note("ray traced car self-reflections: on where the car self-reflections are active", added);
  } else {
    passes.precache().Note("ray traced car self-reflections: made when first wanted", 0);
  }
}

bool CarRtRefl::MakeStatsBuffers(Device& device) {
  namespace census = car_rt_hit::census;
  stats_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(census::kBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "car rt refl stats");
  stats_zero_ = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(census::kBytes), "car rt refl stats zero");
  stats_readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(census::kBytes), "car rt refl stats readback");
  if (!stats_ || !stats_zero_ || !stats_readback_) {
    return false;
  }
  const plume::RenderRange no_read(0, 0);
  void* mapped = stats_zero_->map(0, &no_read);
  if (!mapped) {
    return false;
  }
  std::memset(mapped, 0, census::kBytes);
  stats_zero_->unmap(0, nullptr);
  return true;
}

bool CarRtRefl::MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene) {
  if (!MakeStatsBuffers(device)) {
    REXLOG_ERROR("{} the census's buffers could not be made", kPrefix);
    return false;
  }
  if (!city_.Initialize(device, *frames_)) {
    return false;  // the city's rows and counters could not be made: the layout would have no set 3 (the screen-space version runs)
  }
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  output_builder_.addReadWriteTexture(1);  // u1: the aux image (hit distance, instance, N.V) for the denoise
  output_builder_.addReadWriteByteAddressBuffer(2);  // u2: the census's counters (car_rt_hit_rules.h census::)
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[]
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);                // space 1: the outputs
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                 // space 2: the car's structures (CAR_RT_SPACE)
  const uint32_t city_set = builder.addDescriptorSet(city_.Builder());                  // space 3: the city's rows and counters (city_refl.h)
  builder.addPushConstant(0, kPushSpace, sizeof(TracePush), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet || city_set != kCitySet) {
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
  glass_.Shutdown();
  opp_.Shutdown();
  city_.Shutdown();
  // The GPU is idle and the heap goes later: the slot is not handed back (CarSelfRefl::Shutdown).
  image_ = HostImage{};
  aux_ = HostImage{};
  scratch_ = HostImage{};
  trace_set_ = OutputSet{};
  first_set_ = OutputSet{};
  final_set_ = OutputSet{};
  for (int i = 0; i < 2; ++i) {
    stable_[i] = HostImage{};
    stable_set_[i] = OutputSet{};
  }
  stable_valid_ = false;
  result_in_stable_ = false;
  trace_coverage_ = {};
  scratch_coverage_ = {};
  stable_coverage_[0] = {};
  stable_coverage_[1] = {};
  stats_.reset();
  stats_zero_.reset();
  stats_readback_.reset();
  stats_busy_ = false;
  ++stats_generation_;
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  hit_ = HostImage{};
  hit_set_ = OutputSet{};
  hit_pipeline_.reset();
  hit_shader_.reset();
  hit_ready_ = false;
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

std::unique_ptr<plume::RenderPipeline> CarRtRefl::MakeHitPipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(denoise_layout_.get(), hit_shader_.get(), kGroup, kGroup, 1), kHitName);
}

plume::RenderPipeline* CarRtRefl::HitPipeline() {
  if (hit_pipeline_) {
    return hit_pipeline_.get();
  }
  if (!denoise_ready_ || hit_failed_ || !passes_) {
    return nullptr;  // the denoise's layout is the hit pass's: without it there is no raw mode
  }
  if (!hit_shader_) {
    const HostShaderBlob blob = CarRtReflHitShader();
    hit_shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (!hit_shader_) {
      hit_failed_ = true;
      REXLOG_ERROR("{} the hit distance pass's shader could not be made: no raw mode this session", kPrefix);
      return nullptr;
    }
  }
  HostPrecache& precache = passes_->precache();
  if (!hit_listed_) {
    hit_listed_ = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({HitKey(), kHitName, [this] { return MakeHitPipeline(); }});
    precache.AddLive("ray traced car self-reflections: hit distance for Ray Reconstruction", std::move(items));
    REXLOG_INFO("{} the hit distance pass was asked for (DLSS Ray Reconstruction takes the frame): its pipeline is made in the background, and the raw mode runs from the frame it is done", kPrefix);
  }
  if (!precache.Ready(HitKey())) {
    return nullptr;  // a frame never waits for a compile
  }
  hit_pipeline_ = precache.Acquire(HitKey(), kHitName, [this] { return MakeHitPipeline(); });
  if (!hit_pipeline_) {
    hit_failed_ = true;
    REXLOG_ERROR("{} the hit distance pass's pipeline could not be made: no raw mode this session", kPrefix);
  }
  return hit_pipeline_.get();
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

void CarRtRefl::BeginFrame(bool reflection_on, bool garage) {
  active_ = false;
  one_ray_ = larecomp::settings::mcla_gpu_car_rt_paint_one_ray.Get();
  glass_.NewFrame();
  city_.BeginFrame(false);  // the city's want and cell are this frame's: reset until the pass is known to run
  hit_ready_ = false;
  if (!ready_ || !scene_) {
    return;
  }
  const bool wanted = car_rt_refl::Wanted(SettingNow(), supported_, reflection_on);
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
  scene_->Request(one_ray_);
  if (!garage) city_.BeginFrame(true, one_ray_);  // CITY-RT M2: the city in the reflections (the setting's cell; the scene service's wants)
}

// RT-PARKED-PAINT: see car_rt_refl.h. The view is the one OnHistory will trace against (the scene built it before the trigger).
std::vector<car_rt_parked_paint::Mark> CarRtRefl::ParkedMarks(const HistoryInfo& info) {
  if (!active_ || !ready_ || !scene_ || !info.image || !info.image->texture || info.image->slot == kNoSlot) {
    return {};
  }
  CarRtOppRefl::MarkInput input;
  input.view = &scene_->View();
  input.one_ray = one_ray_;
  input.render_width = info.render_width;
  input.render_height = info.render_height;
  input.divisor = info.divisor;
  input.image_width = info.image->width;
  input.image_height = info.image->height;
  input.tan_x = info.tan_x;
  input.tan_y = info.tan_y;
  return opp_.ParkedMarks(input);
}

bool CarRtRefl::OnHistory(plume::RenderCommandList* list, const HistoryInfo& info) {
  recorded_ = false;
  if (!list || !active_ || !ready_ || !scene_ || !info.image || !info.image->texture || info.image->slot == kNoSlot) {
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
  scene_->SetHitStops(HitStops());  // the next frame's material rows carry the exposure
  const float self_reach = car_rt_refl::ClampedLength(float(larecomp::settings::mcla_gpu_car_selfrefl_rt_length.Get()));
  const uint32_t parts = uint32_t(std::clamp<int32_t>(larecomp::settings::mcla_gpu_car_selfrefl_rt_parts.Get(), 1, int32_t(car_rt_refl::kPartAll)));
  const float gloss = car_rt_refl::ClampedGloss(float(larecomp::settings::mcla_gpu_car_selfrefl_rt_gloss.Get()));
  const float tan_half = car_rt_refl::ConeTan(gloss);
  // RT-HIT-SHADING part two: the class's level (the reach to another car, the cone's rays, the clear coat), the two debug overrides on it. The ray is cast as far as the longer reach where the
  // parts it may hit include another car; a hit's weight falls over the reach of the part it is on (the shader's coat.x and coat.y).
  const bool one_ray = one_ray_;
  const car_rt_hit::Level base_level = one_ray ? car_rt_hit::PaintLevel() : car_rt_hit::LevelFor(gpu_class::Current());
  const car_rt_hit::Level level = car_rt_hit::LevelWith(base_level, double(larecomp::settings::mcla_gpu_car_rt_hit_reach.Get()),
                                                        double(larecomp::settings::mcla_gpu_car_rt_clearcoat.Get()));
  const float others_reach = level.others_reach > 0.0f ? car_rt_refl::ClampedLength(level.others_reach) : self_reach;
  // CITY-RT M2: the city in the reflections. A stats frame (car_rt_hit_rules.h census::) is decided first: the city's counters ride the same frames. Prepare writes this frame slot's rows and gives the
  // set the layout's set 3 takes (an off frame: zero rows, the pass as it was); the reflection ray's parts have the city's bit and its length the cell's reach where the city is on.
  const bool stats_frame = stats_ && stats_readback_ && !stats_busy_ && car_rt_hit::census::StatsDue(info.frame, stats_last_frame_, stats_ever_);
  const CityRefl::Frame city = city_.Prepare(frames_->slot(), info.screen, info.frame, stats_frame);
  if (!city.set) {
    return false;  // the city's rows could not be made (logged): the march answers
  }
  const uint32_t reflection_parts = city_refl::ReflectionParts(parts, city.on ? city.cell.level : city_refl::Level::kOff);
  const float car_length = car_rt_hit::CastReach(parts, self_reach, others_reach);  // the cars' reach
  const float length = city.on ? city_refl::CastLength(reflection_parts, car_length, city.cell.reach) : car_length;  // what is cast, and what the aux image and the denoise decode distances by
  const uint32_t cone_rays = city.on ? std::min(level.cone_rays, city.cell.cone_rays) : level.cone_rays;

  // The denoise (car_rt_refl.h DENOISE): the passes this frame, where the setting asks for them, the cone has width, the pipeline is made and
  // the scratch image is there; else the raw trace is what the paint reads.
  // RR-4b: in a frame Ray Reconstruction takes it is the denoiser: no filter, and the hit distance pass after the trace. Only where the pass can run (its pipeline made, its image made); a frame
  // that is asked and cannot is a plain one, as always (logged when the state changes). The pipeline is listed with the host precache only when a frame asks: with the plan not RR it never is.
  bool raw = false;
  if (info.raw) {
    raw_ever_asked_ = true;
    raw = HitPipeline() != nullptr && info.render_width != 0 && info.render_height != 0 && EnsureHitImage(info.render_width, info.render_height);
  }
  if (raw_ever_asked_ && raw_state_logged_ != (raw ? 0u : (info.raw ? 1u : 2u))) {
    raw_state_logged_ = raw ? 0u : (info.raw ? 1u : 2u);
    REXLOG_INFO("{} raw mode (DLSS Ray Reconstruction takes the frame): {} (logged when it changes)", kPrefix,
                raw ? "on: the trace is read as it is (no filter) and its hit distance is written for the guide pass"
                    : (info.raw ? "asked, not possible yet (the hit distance pass's pipeline or image is not ready): the filter runs as usual" : "not asked: the filter runs as usual"));
  }
  int denoise_passes = car_rt_refl::DenoisePassesFor(raw, int(larecomp::settings::mcla_gpu_car_selfrefl_rt_denoise.Get()), tan_half);
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
  const bool resolve_paint = denoise_passes > 0 && larecomp::settings::mcla_gpu_car_selfrefl_rt_resolve.Get();
  // NOISE-RESOLVE: with no temporal upscaler the cone's pattern changes every frame and the finished image is averaged with the last frame's result (the denoise's third pass); that needs the denoise
  // (it makes the image to average), its two images and the switch. Else the pattern stands still on the screen as the first fixes left it, and what the a-trous passes leave is all there is.
  const bool resolve_history = denoise_passes > 0 && (one_ray || !info.upscaler) && noise_resolve::ResolveOn(larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get(), larecomp::settings::mcla_gpu_noise_resolve_cone.Get()) && EnsureStableImages(history.width, history.height);
  // NOISE-FOLLOW: the images stay over the frames a temporal upscaler takes (the plain frames of an upscaler session, a pause toggle, would make them again); `stable_valid_` / `stable_frame_` say what they hold is stale
  // (have_last below), and they go back after kHistoryKeepFrames frames of the cone's passes in a row without the resolve, with the setting off, or at another size.
  const bool stable_held = stable_[0].texture || stable_[1].texture;
  const bool stable_size_ok = stable_[0].width == history.width && stable_[0].height == history.height && stable_[1].width == history.width && stable_[1].height == history.height;
  // CHECK: not run on a device: with DLSS Ray Reconstruction on, a pause toggle should not make and free the two images.
  // (RR-4b: in a frame DLSS Ray Reconstruction takes the denoise passes are 0 because Ray Reconstruction is the denoiser, not because the setting turned them off: the setting's own count says the history would be wanted.)
  const bool stable_gap = car_rt_refl::DenoisePassCount(int(larecomp::settings::mcla_gpu_car_selfrefl_rt_denoise.Get()), tan_half) > 0 && info.upscaler && noise_resolve::ResolveOn(larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get(), larecomp::settings::mcla_gpu_noise_resolve_cone.Get());
  const noise_resolve::HistoryImages stable_keep = stable_life_.Frame(resolve_history, stable_gap, stable_held, stable_size_ok);
  if (!resolve_history && stable_held && stable_keep != noise_resolve::HistoryImages::kKeep) {
    ReleaseStable();
  }
  const uint32_t pattern_phase = (one_ray || noise_resolve::Varies(info.upscaler, resolve_history)) ? info.pattern_phase : 0u;

  // Another car's chrome (car_chrome_rt_rules.h): found by the primary ray too, and its probes traced, where the chrome variant is drawn at all and the switch for
  // this is on; else the primary ray is the player's body alone, as it was (no instance of the scene has the chrome's bit then that a ray here would take).
  const bool chrome_others = larecomp::settings::mcla_gpu_car_chrome_occlusion.Get() && larecomp::settings::mcla_gpu_car_chrome_rt_others.Get();
  Push push = {};
  push.slots[0] = history.slot;
  push.slots[1] = reflection_parts;
  push.slots[2] = chrome_others ? car_rt_refl::kPrimaryTraceParts : car_rt_refl::kPrimaryParts;
  // A stats frame (car_rt_hit_rules.h census::): the shader counts what became of every hit, and the host reads the counters back (ProcessStats).
  // The cone's turn is the blue noise tile's (the first version's gradient noise is a lattice the denoise leaves as thin diagonal lines: car_rt_refl_rules.h ConeRotation) unless
  // mcla_gpu_noise_blue is off, for the A/B.
  push.slots[3] = car_rt_hit::census::PackWithStats(car_rt_hit::PackPhaseRays(pattern_phase, cone_rays), stats_frame) |
                  (larecomp::settings::mcla_gpu_noise_blue.Get() ? car_rt_refl::kConeBlueFlag : 0u) |
                  (resolve_paint ? car_rt_refl::kFullConeFlag : 0u) |
                  (one_ray ? car_rt_refl::kSingleRayFlag : 0u);
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
  if (city.stats) {
    city_.StatsBegin(list);  // the city's counters, zeroed (the same frame as the cars')
  }
  if (city.screen) {
    CityRefl::BarrierScreen(list, info.screen);  // lin_depth and colour_post's levels in the layout the pass reads them in
  }
  if (stats_frame) {
    // The counters start at zero (the buffer's last state is the compute stage's, see below).
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::WRITE));
    list->copyBufferRegion(plume::RenderBufferReference(stats_.get(), 0), plume::RenderBufferReference(stats_zero_.get(), 0), car_rt_hit::census::kBytes);
    list->barriers(Stage::COMPUTE, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  }
  // The history is read by a compute shader here (and by the paint's pixel shaders later): a layout both can read. The images are written
  // as storage images; the one the paint reads is read by its pixel shaders afterwards.
  transition(Stage::GRAPHICS_AND_COMPUTE, {{&history, Layout::SHADER_READ}});
  transition(Stage::COMPUTE, {{&image_, Layout::GENERAL}, {&aux_, Layout::GENERAL}});
  // CHECK: CarRtScene leaves the top level built, and readable by a compute shader, when View().valid (a UAV barrier after its build).
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(trace_set_.set.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setComputeDescriptorSet(city.set, kCitySet);
  list->setPipeline(pipeline);
  namespace paint = car_rt_refl;
  const paint::PaintRect full{0, 0, int32_t(history.width), int32_t(history.height)};
  paint::PaintRects current;
  if (one_ray) {
    const auto add_box = [&](const car_rt_instances::ViewBox& box) {
      current.push_back(paint::PaintTilesForBox(box, info.tan_x, info.tan_y, info.render_width, info.render_height,
                                               std::max(info.divisor, 1u), full));
    };
    add_box(view.player_box);
    if (view.boxes) {
      for (uint32_t i = 0; i < view.box_count; ++i) {
        if (view.boxes[i].paint || (chrome_others && view.boxes[i].chrome)) {
          add_box(view.boxes[i]);
        }
      }
    } else if (view.box_count != 0) {
      current = {full};
    }
    current = car_rt_opp_refl::MergeRects(std::move(current), std::numeric_limits<uint32_t>::max());
  }
  // CHECK: compare masked and full coverage in chase, cockpit and parked traffic,
  // including resize, camera cuts, disappearing receivers, and both backends.
  const auto trace_regions = paint::PaintDispatch(trace_coverage_, image_.serial, current, full, one_ray);
  for (const paint::PaintRect& rect : trace_regions) {
    const TracePush trace_push{push, {uint32_t(rect.x0), uint32_t(rect.y0), info.tile_slot + 1u, 0u}};
    list->setComputePushConstants(0, &trace_push, 0, sizeof(trace_push));
    list->dispatch(car_rt_opp_refl::GroupsX(rect, kGroup), car_rt_opp_refl::GroupsY(rect, kGroup), 1);
  }

  // The opponents' cars (car_rt_opp_refl.h): the same pass for the paint of the cars beside the player, a second dispatch into the same two images (nothing is recorded where no
  // other car's paint is in the scene).
  {
    CarRtOppRefl::Frame opponents;
    opponents.list = list;
    opponents.view = &view;
    opponents.base_push = &push;
    opponents.base_push_bytes = sizeof(push);
    if (!info.parked_history) opponents.tile_slot = info.tile_slot;
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
    opponents.city = &city;  // the city's set and cell: the opponents' paint reflects it within the cell's budget (CITY-RT M2 R2)
    opp_.Record(opponents);
  }
  // The player's windows (car_rt_glass_refl.h, CITY-RT M2 R3b): a third dispatch of the trace's file after the opponents', into an image of its own; the additive pass that adds it to the main colour is
  // recorded by CarSelfRefl::RecordHistory (it has the colour target) through ApplyGlass. In the same stats frame (the city's counters count its rays too).
  {
    CarRtGlassRefl::Frame windows;
    windows.list = list;
    windows.view = &view;
    windows.base_push = &push;
    windows.base_push_bytes = sizeof(push);
    windows.aux = &aux_;
    windows.stats = stats_.get();
    windows.city = &city;
    windows.wanted = city_.glass_wanted();
    windows.colour_samples = info.colour_samples;
    windows.image_width = history.width;
    windows.image_height = history.height;
    windows.render_width = info.render_width;
    windows.render_height = info.render_height;
    windows.divisor = std::max<uint32_t>(info.divisor, 1);
    windows.tan_x = info.tan_x;
    windows.tan_y = info.tan_y;
    windows.frame = info.frame;
    glass_.Trace(windows);
  }
  if (city.stats) {
    city_.StatsEnd(list, info.frame);  // the city's counters to the readback once both dispatches are done
  }
  if (stats_frame) {
    // The counters go to the readback once both dispatches are done; the buffer goes back to the state the pass's descriptor names it in.
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(stats_readback_.get(), 0), plume::RenderBufferReference(stats_.get(), 0), car_rt_hit::census::kBytes);
    list->barriers(Stage::COMPUTE, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
    stats_busy_ = true;
    stats_ever_ = true;
    stats_last_frame_ = info.frame;
    frames_->RetireAction([this, frame = info.frame, generation = stats_generation_] { ProcessStats(frame, generation); });
  }

  result_in_scratch_ = false;
  result_in_stable_ = false;
  if (denoise_passes == 0) {
    transition(Stage::GRAPHICS, {{&image_, Layout::SHADER_READ}});
  } else {
    // First pass: the trace's image (and the aux image) are read, the scratch image written. Second pass: the scratch is read, the trace's image
    // written again. One pass leaves its result in the scratch image.
    pass_timing_->Mark(list, "car rt denoise");
    transition(Stage::COMPUTE, {{&image_, Layout::SHADER_READ}, {&aux_, Layout::SHADER_READ}, {&scratch_, Layout::GENERAL}});
    const auto scratch_regions = paint::PaintDispatch(scratch_coverage_, scratch_.serial, current, full, one_ray);
    DispatchDenoise(list, denoise, history.slot, image_, first_set_, 0, history.width, history.height, float(info.tan_y), length, tan_half, scratch_regions, resolve_paint || one_ray);
    if (denoise_passes >= 2) {
      transition(Stage::COMPUTE, {{&scratch_, Layout::SHADER_READ}, {&image_, Layout::GENERAL}});
      DispatchDenoise(list, denoise, history.slot, scratch_, final_set_, 1, history.width, history.height, float(info.tan_y), length, tan_half, trace_regions);
      transition(Stage::GRAPHICS, {{&image_, Layout::SHADER_READ}});
    } else {
      result_in_scratch_ = true;
      transition(Stage::GRAPHICS, {{&scratch_, Layout::SHADER_READ}});
    }
    if (resolve_history) {
      // The third pass: the finished image (the trace's after two passes, the scratch after one) averaged with the last frame's result into the other stable image, which the paint reads. The history is the
      // frame before's (consecutive guest frames, the same size); without one the pass copies the finished image (weight 0) and the next frame has a history.
      const car_rt_refl::ConePose pose{view.camera_view, view.player_world, view.player_mesh_key};
      const bool have_last = (!one_ray || !info.reset_history) && stable_valid_ && stable_resolve_paint_ == resolve_paint && stable_one_ray_ == one_ray &&
                             stable_city_ == city.on && stable_length_ == length && stable_spread_ == tan_half &&
                             stable_coat_ == level.coat && stable_exp_bias_ == info.exp_bias && stable_frame_ + 1 == info.frame &&
                             stable_[0].width == history.width && stable_[0].height == history.height &&
                             car_rt_refl::ConeHistoryMatches(stable_garage_, info.garage, stable_pose_, pose);
      // CHECK: confirm history restarts on a garage transition and a vehicle swap.
      stable_pose_ = pose;
      stable_garage_ = info.garage;
      const float keep = have_last ? noise_resolve::ConeHistoryWeight(stable_frames_) : 0.0f;
      const uint32_t out = 1 - stable_index_;
      HostImage& finished = result_in_scratch_ ? scratch_ : image_;
      pass_timing_->Mark(list, "car rt temporal");
      transition(Stage::COMPUTE, {{&finished, Layout::SHADER_READ}, {&stable_[stable_index_], Layout::SHADER_READ}, {&stable_[out], Layout::GENERAL}});
      const auto stable_regions = paint::PaintDispatch(stable_coverage_[out], stable_[out].serial, current, full, one_ray);
      DispatchDenoise(list, denoise, history.slot, finished, stable_set_[out], 2, history.width, history.height, float(info.tan_y), length, tan_half,
                      stable_regions, false, keep, stable_[stable_index_].slot, one_ray);
      transition(Stage::GRAPHICS, {{&stable_[out], Layout::SHADER_READ}});
      stable_index_ = out;
      stable_valid_ = true;
      stable_resolve_paint_ = resolve_paint;
      stable_one_ray_ = one_ray;
      stable_city_ = city.on;
      stable_length_ = length;
      stable_spread_ = tan_half;
      stable_coat_ = level.coat;
      stable_exp_bias_ = info.exp_bias;
      stable_frame_ = info.frame;
      stable_frames_ = have_last ? std::min(stable_frames_ + 1u, 64u) : 0u;
      result_in_stable_ = true;
      ++stable_runs_;
      if (!stable_logged_ && have_last) {
        stable_logged_ = true;
        REXLOG_INFO("{} the first frame that averages the finished image over a history (no temporal upscaler: the cone's turn changes every frame, mcla_gpu_noise_resolve_no_upscaler): weight {:.2f} at most, texel for "
                    "texel, clamped to the 3x3 neighbourhood of the finished image, two images {}x{} R16G16B16A16_FLOAT ({:.1f} MiB) (reported once)",
                    kPrefix, noise_resolve::kConeHistoryWeight, history.width, history.height,
                    double(TextureBytes(stable_[0].width, stable_[0].height, 1, stable_[0].format) * 2) / (1024.0 * 1024.0));
      }
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
  if (raw) {
    // RR-4b: how far each reflection went, into the image the guide pass reads as the hit distance (hlsl/car_rt_refl_hit_cs.hlsl): from the aux image the trace wrote (this frame's, the opponents'
    // cars included: they wrote into it too).
    HitPush hit_push = {};
    hit_push.slots[0] = aux_.slot;
    hit_push.image[0] = float(info.render_width);
    hit_push.image[1] = float(info.render_height);
    hit_push.image[2] = float(history.width);
    hit_push.image[3] = float(history.height);
    hit_push.lens[0] = length;
    hit_push.lens[1] = float(std::max<uint32_t>(info.divisor, 1));
    hit_push.lens[2] = car_rt_refl::kRawMissDistance;
    pass_timing_->Mark(list, "car rt hit");
    transition(Stage::COMPUTE, {{&aux_, Layout::SHADER_READ}, {&hit_, Layout::GENERAL}});
    list->setComputePipelineLayout(denoise_layout_.get());
    list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
    list->setComputeDescriptorSet(hit_set_.set.get(), kOutputSet);
    list->setPipeline(hit_pipeline_.get());
    list->setComputePushConstants(0, &hit_push, 0, sizeof(hit_push));
    list->dispatch((info.render_width + kGroup - 1) / kGroup, (info.render_height + kGroup - 1) / kGroup, 1);
    transition(Stage::GRAPHICS_AND_COMPUTE, {{&hit_, Layout::SHADER_READ}});
    hit_ready_ = true;
    plain_streak_ = 0;
    if (++raw_passes_ == 1) {
      REXLOG_INFO("{} the first raw pass (DLSS Ray Reconstruction takes the frame): frame {}, no filter on the {}x{} trace image, the hit distance of its reflections in an R16_FLOAT image of {}x{} "
                  "({:.1f} MiB) for the guide pass",
                  kPrefix, info.frame, history.width, history.height, info.render_width, info.render_height,
                  double(TextureBytes(hit_.width, hit_.height, 1, hit_.format)) / (1024.0 * 1024.0));
    }
  } else if (plain_streak_ < car_rt_refl::kRawReleaseAfter) {
    ++plain_streak_;
  } else if (hit_.texture) {
    passes_->ReleaseImage(hit_);  // the raw mode has not been used for a while: the image goes back
    RetireSet(hit_set_);
  }
  // What follows is not this pass's: closes its timing segment (PassTiming labels everything up to the next mark).
  pass_timing_->Mark(list, "after car rt refl");

  recorded_ = true;
  recorded_frame_ = info.frame;
  ++passes_run_;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first ray traced pass: frame {}, image {}x{} for a {}x{} picture (divisor {}), {} instances, reflection ray {} m (the player's own parts reach {} m, "
                "another car {} m from {:.0f}% of it), parts {}, {} rays over a cone of {} degrees{}; the clear-coat ray: {}{}; the sun's shadow at a shaded hit: {}; the other cars' chrome: {}; "
                "the colours the pass makes up from a paint constant are multiplied by {} (the history's units over the paint's, the main target's exponent bias {})",
                kPrefix, info.frame, history.width, history.height, info.render_width, info.render_height, info.divisor,
                view.instance_count, length, self_reach, others_reach, 100.0 * double(level.others_fade_from), parts,
                gloss > 0.0f ? cone_rays : 1u, gloss,
                info.upscaler ? " (the pattern turns with the frame: a temporal upscaler runs)" : (resolve_history ? " (the pattern turns with the frame: the pass's own history averages it)" : ""),
                level.coat > 0.0f && gloss > 0.0f && (one_ray || cone_rays >= 4)
                    ? std::format("on, strength {:.2f} (mcla_gpu_car_rt_clearcoat)", level.coat) : std::string("off"),
                one_ray ? std::string(" (one reflection sample, shared by the coat and cone)")
                    : level.coat > 0.0f && gloss > 0.0f && cone_rays >= 4
                    ? std::format(" (another car in the reflection: the mirror is at least {:.0f}% of the texel, the cone's hits on it are dropped where the mirror missed it; the paint's clear coat layer: {})",
                                  100.0 * double(car_rt_hit::kOthersSharpShare), larecomp::settings::mcla_gpu_car_rt_coat_layer.Get() ? "on" : "off (mcla_gpu_car_rt_coat_layer)")
                    : std::string(" (a level with no mirror ray: the cone alone for another car too)"),
                larecomp::settings::mcla_gpu_car_rt_hit_cascade.Get() ? "the game's cascades with the cars' ray (when the volumetric light has the cascades)" : "the cars' ray alone",
                chrome_others ? "traced (mcla_gpu_car_chrome_rt_others)" : "left to the marched version", push.up[3], info.exp_bias);
  }
  return true;
}

void CarRtRefl::DispatchDenoise(plume::RenderCommandList* list, plume::RenderPipeline* pipeline, uint32_t history_slot, const HostImage& input,
                                const OutputSet& output, uint32_t pass, uint32_t width, uint32_t height, float tan_y, float length,
                                float tan_half, const car_rt_refl::PaintRects& regions, bool resolve_paint, float keep, uint32_t previous_slot, bool one_ray) {
  DenoisePush push = {};
  push.slots[0] = history_slot;
  push.slots[1] = input.slot;
  push.slots[2] = aux_.slot;
  push.slots[3] = pass;
  push.image[0] = float(width);
  push.image[1] = float(height);
  push.image[2] = keep;                  // pass 2: the weight the history keeps
  push.image[3] = float(previous_slot);  // pass 2: the last result's bindless slot (a slot is far below 2^24: exact in a float)
  push.lens[0] = tan_y;
  push.lens[1] = length;
  push.lens[2] = tan_half;
  const auto first_range = car_rt_refl::FirstDenoiseRange(resolve_paint);
  push.tune[0] = first_range.from;
  push.tune[1] = first_range.to;
  push.tune[2] = car_rt_refl::kDenoiseSecondFrom;
  push.tune[3] = car_rt_refl::kDenoiseSecondTo;
  list->setComputePipelineLayout(denoise_layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(output.set.get(), kOutputSet);
  list->setPipeline(pipeline);
  push.region[2] = one_ray ? 1u : 0u;
  for (const auto& rect : regions) {
    push.region[0] = uint32_t(rect.x0);
    push.region[1] = uint32_t(rect.y0);
    list->setComputePushConstants(0, &push, 0, sizeof(push));
    list->dispatch(car_rt_opp_refl::GroupsX(rect, kGroup), car_rt_opp_refl::GroupsY(rect, kGroup), 1);
  }
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
  if (!EnsureSet(trace_set_, output_builder_, image_, &aux_, "car rt refl output set", stats_.get())) {
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

bool CarRtRefl::EnsureHitImage(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = hit_.texture.get();
  if (!passes_->EnsureImage(hit_, width, height, plume::RenderFormat::R16_FLOAT, "car rt refl hit distance", kFlags)) {
    RetireSet(hit_set_);
    return false;
  }
  if (hit_.texture.get() != before) {
    RetireSet(hit_set_);  // a new texture is a new set for it (the old one goes after the lists that name it)
  }
  return EnsureSet(hit_set_, denoise_builder_, hit_, nullptr, "car rt refl hit set");
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

bool CarRtRefl::EnsureStableImages(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  for (int i = 0; i < 2; ++i) {
    const plume::RenderTexture* before = stable_[i].texture.get();
    if (!passes_->EnsureImage(stable_[i], width, height, plume::RenderFormat::R16G16B16A16_FLOAT, i == 0 ? "car rt refl stable 0" : "car rt refl stable 1", kFlags)) {
      ReleaseStable();
      return false;
    }
    if (stable_[i].texture.get() != before) {
      RetireSet(stable_set_[i]);
      stable_valid_ = false;  // a new image holds nothing
    }
    if (!EnsureSet(stable_set_[i], denoise_builder_, stable_[i], nullptr, i == 0 ? "car rt refl stable set 0" : "car rt refl stable set 1")) {
      ReleaseStable();
      return false;
    }
  }
  return true;
}

void CarRtRefl::ReleaseStable() {
  stable_valid_ = false;
  result_in_stable_ = false;
  if (!passes_) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    passes_->ReleaseImage(stable_[i]);
    RetireSet(stable_set_[i]);
  }
}

bool CarRtRefl::EnsureSet(OutputSet& out, plume::RenderDescriptorSetBuilder& builder, const HostImage& first, const HostImage* second,
                          const char* name, const plume::RenderBuffer* stats) {
  const plume::RenderTexture* first_texture = first.texture.get();
  const plume::RenderTexture* second_texture = second ? second->texture.get() : nullptr;
  if (out.set && out.first == first_texture && out.second == second_texture) {
    return true;
  }
  RetireSet(out);
  out.set = device_->CreateDescriptorSet(builder, stats ? 3u : (second ? 2u : 1u), 0, name);
  if (!out.set) {
    return false;
  }
  out.set->setTexture(0, first_texture, plume::RenderTextureLayout::GENERAL, first.view.get());
  if (second) {
    out.set->setTexture(1, second_texture, plume::RenderTextureLayout::GENERAL, second->view.get());
  }
  if (stats) {
    out.set->setBuffer(2, stats, car_rt_hit::census::kBytes);
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

const HostImage* CarRtRefl::ImageFor(uint64_t history_frame) const {
  const HostImage& result = result_in_stable_ ? stable_[stable_index_] : (result_in_scratch_ ? scratch_ : image_);
  if (!recorded_ || recorded_frame_ != history_frame || !result.texture || result.slot == kNoSlot) {
    return nullptr;
  }
  return &result;
}

uint32_t CarRtRefl::SlotFor(uint64_t history_frame) const {
  const auto* image = ImageFor(history_frame);
  return image ? image->slot : car_selfrefl::kNoRtImage;
}

void CarRtRefl::ReleaseImage() {
  recorded_ = false;
  result_in_scratch_ = false;
  result_in_stable_ = false;
  stable_valid_ = false;
  if (!passes_) {
    return;
  }
  glass_.ReleaseImage();
  passes_->ReleaseImage(stable_[0]);
  passes_->ReleaseImage(stable_[1]);
  RetireSet(stable_set_[0]);
  RetireSet(stable_set_[1]);
  passes_->ReleaseImage(image_);
  passes_->ReleaseImage(aux_);
  passes_->ReleaseImage(scratch_);
  passes_->ReleaseImage(hit_);
  hit_ready_ = false;
  RetireSet(hit_set_);
  RetireSet(trace_set_);
  RetireSet(first_set_);
  RetireSet(final_set_);
  image_for_[0] = image_for_[1] = 0;
}

uint64_t CarRtRefl::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* held : {&image_, &aux_, &scratch_, &hit_, &stable_[0], &stable_[1]}) {
    bytes += held->texture ? TextureBytes(held->width, held->height, 1, held->format) : 0;
  }
  return bytes + glass_.HeldBytes();
}

float CarRtRefl::HitStops() const { return larecomp::settings::mcla_gpu_car_rt_hit_calibrate.Get() ? hit_stops_ : 0.0f; }

// A stats frame's counters (car_rt_hit_rules.h census::): added to the window, the exposure moved when enough paint samples have come, a log line every kLogEveryStats frames (and for the
// first). The counters are one frame's (zeroed before it), read back after the GPU finished it.
void CarRtRefl::ProcessStats(uint64_t frame, uint64_t generation) {
  namespace census = car_rt_hit::census;
  stats_busy_ = false;
  if (generation != stats_generation_ || !stats_readback_) {
    return;
  }
  const void* mapped = stats_readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the census of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  uint32_t words[census::kWords];
  std::memcpy(words, mapped, sizeof(words));
  stats_readback_->unmap();
  ++stats_frames_;
  census::Add(&window_, words);
  ++window_frames_;
  const bool calibrating = larecomp::settings::mcla_gpu_car_rt_hit_calibrate.Get();
  // The exposure: the paint samples of the frames since it last moved; when there are enough, the gain moves to cancel the difference the model (with the gain in force) had to the history.
  cal_count_ += words[census::kCalCount];
  cal_seen_ += words[census::kCalSeen];
  cal_model_ += words[census::kCalModel];
  float residual = 0.0f;
  if (census::CalibrationStops(cal_count_, cal_seen_, cal_model_, &residual)) {
    if (calibrating) {
      hit_stops_ = census::NextStops(hit_stops_, residual);
    }
    cal_count_ = cal_seen_ = cal_model_ = 0;
  }
  if (stats_frames_ == 1 || stats_frames_ % census::kLogEveryStats == 0) {
    REXLOG_INFO("{} hits, frame {}: {}", kPrefix, frame, census::Text(window_, window_frames_, hit_stops_, calibrating));
    window_ = census::Totals{};
    window_frames_ = 0;
  }
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
         (raw_ever_asked_ ? std::format("; {} raw (Ray Reconstruction)", raw_passes_) : std::string()) + opp_.CensusText() + glass_.CensusText();
}

}  // namespace mcla::gpu
