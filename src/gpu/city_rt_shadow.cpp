// MCLA GPU renderer -- the city's ray traced sun shadows. See city_rt_shadow.h.

#include "city_rt_shadow.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "city_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

namespace rules = city_rt_shadow;

constexpr const char* kPrefix = "[city rt shadow]";
constexpr const char* kPipelineName = "city rt shadow";
constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8
// The pipeline's place among the occlusion's: the same owner, a variant past the car ray traced pass's 0x100.
constexpr uint32_t kPipelineVariant = 0x101;
// The image goes back after this many frames without the pass (2 s at 60).
constexpr uint32_t kReleaseAfterFrames = 120;
// The check's counters are read back this often (frames).
constexpr uint64_t kReadbackEveryFrames = 120;
// A shadow is never applied beyond this (metres): past it the pixel's depth is too coarse for a ray's origin and the lod is the surface.
constexpr float kMaxRange = 700.0f;
// Under this the pass is not worth a dispatch (nothing is built near the camera yet).
constexpr float kMinRange = 8.0f;
// The occlusion's own gate for rain is the contact march's; the sun's gate is here.
LiveText& Setting() {
  static LiveText text("mcla_gpu_rt_shadows");
  return text;
}
LiveText& AlphaSetting() {
  static LiveText text("mcla_gpu_city_rt_alpha");
  return text;
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kAmbientOcclusion, kPipelineVariant); }

// CityRtShadowsAvailable()'s answer: set once the device can trace and the pass's layout and shader exist (Initialize), false again at Shutdown.
std::atomic<bool> g_available{false};

float GameHiRadius() {
  // The game draws a sector at its full detail within 300 m times lod_city_scale of the camera (CITY-RT-SURVEY section 1.2); gpu_class resolves 0 (auto) for this GPU.
  return float(300.0 * gpu_class::CityLodScale(larecomp::settings::lod_city_scale.Get()));
}

}  // namespace

bool CityRtShadowsAvailable() { return g_available.load(std::memory_order_relaxed); }

CityRtShadow::~CityRtShadow() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CityRtShadow::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* car_scene, CityRtScene* city_scene,
                              bool listed_at_start) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  car_scene_ = car_scene;
  city_scene_ = city_scene;
  ready_ = false;
  g_available.store(false, std::memory_order_relaxed);
  listed_ = false;
  pipeline_failed_ = false;
  const bool supported = car_scene != nullptr && car_scene->ready() && city_scene != nullptr && city_scene->initialized() &&
                         CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan);
  if (!supported) {
    passes.precache().Note("ray traced city shadows: not on this graphics card or backend (the game's shadows stand)", 0);
    return;
  }
  if (!MakeLayoutAndShader(device, passes, *car_scene)) {
    passes.precache().Note("ray traced city shadows: the layout or the shader could not be made (the game's shadows stand)", 0);
    return;
  }
  ready_ = true;
  g_available.store(true, std::memory_order_relaxed);
  if (listed_at_start && city_rt::LevelFor(gpu_class::Current(), city_rt::ParseChoice(Setting().Get())) != city_rt::Level::kOff) {
    const uint32_t added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); }) ? 1u : 0u;
    listed_ = true;
    passes.precache().Note("ray traced city shadows: on where the setting says (auto: the High class)", added);
  } else {
    passes.precache().Note("ray traced city shadows: made when first wanted", 0);
  }
}

bool CityRtShadow::MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene) {
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);              // u0: the image the pass writes
  output_builder_.addReadWriteByteAddressBuffer(1);    // u1: the check's counters
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());  // space 0: Texture2D<float4>[] (the depth image and the car image)
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);                // space 1: the output
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                 // space 2: the structures (CAR_RT_SPACE)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  layout_ = device.CreatePipelineLayout(builder, "city rt shadow layout");
  if (!layout_) {
    return false;
  }
  const HostShaderBlob blob = CityRtShadowShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  counters_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(kCounterBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS),
                                  "city rt shadow check counters");
  readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kCounterBytes), "city rt shadow check readback");
  return shader_ != nullptr && counters_ && readback_;
}

void CityRtShadow::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();  // the host builder's function names this object's layout and shader: stop it first
  }
  image_ = HostImage{};
  set_.reset();
  set_texture_ = nullptr;
  counters_.reset();
  readback_.reset();
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_ = false;
  g_available.store(false, std::memory_order_relaxed);
  wanted_ = false;
  ++generation_;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  car_scene_ = nullptr;
  city_scene_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipeline
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> CityRtShadow::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, kGroup, 1), kPipelineName);
}

plume::RenderPipeline* CityRtShadow::Pipeline() {
  if (pipeline_) {
    return pipeline_.get();
  }
  if (pipeline_failed_) {
    return nullptr;
  }
  pipeline_ = passes_->precache().Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the pass's pipeline could not be made: the game's shadows stand this session", kPrefix);
  }
  return pipeline_.get();
}

bool CityRtShadow::PipelineReady() {
  if (pipeline_) {
    return true;
  }
  if (pipeline_failed_) {
    return false;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({PipelineKey(), kPipelineName, [this] { return MakePipeline(); }});
    listed_ = true;
    precache.AddLive("ray traced city shadows", std::move(fresh));
    REXLOG_INFO("{} the pass was not listed at the start: the pipeline is made in the background, and the pass runs from the frame it is done", kPrefix);
  }
  return precache.Ready(PipelineKey());
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CityRtShadow::BeginFrame(bool contact_on) {
  wanted_ = false;
  level_ = city_rt::Level::kOff;
  if (!ready_ || !city_scene_ || !car_scene_) {
    return;
  }
  // The image goes back once the pass has not run for a while (Record resets the count).
  if (image_.texture && ++idle_frames_ >= kReleaseAfterFrames) {
    ReleaseImage();
    idle_frames_ = 0;
  }
  level_ = city_rt::LevelFor(gpu_class::Current(), city_rt::ParseChoice(Setting().Get()));
  // The structures (and their frame counter and idle release) see every frame; they build only for a level that is on.
  city_scene_->BeginFrame(contact_on ? level_ : city_rt::Level::kOff, float(larecomp::settings::mcla_gpu_city_rt_radius.Get()));
  if (level_ == city_rt::Level::kOff || !contact_on) {
    return;  // the contact march is the consumer: without it nothing would read the image
  }
  if (!PipelineReady()) {
    ++not_ready_;
    return;
  }
  wanted_ = true;
  car_scene_->Request();  // the top level is built only for a frame that asked in the one before: ask in every frame this is on
}

CityRtShadow::Result CityRtShadow::Record(plume::RenderCommandList* list, const Inputs& in) {
  Result result;
  if (!list || !wanted_ || !ready_ || !in.depth || !in.depth->texture || in.depth->slot == kNoSlot) {
    return result;
  }
  const CarRtView& view = car_scene_->View();
  const CityRtScene::Stats city = city_scene_->stats();
  if (!view.valid || !view.set || city.instances == 0) {
    ++no_scene_;
    if (!no_scene_logged_ && (view.valid || city_scene_->pack_ready())) {
      no_scene_logged_ = true;
      REXLOG_INFO("{} no city in the top level at the first frame that wanted it (frame {}: the car's structures {}, the pack {}, {} sectors built so far): the game's shadows stand "
                  "there; counted in the census",
                  kPrefix, in.frame, view.valid ? "exist" : "do not exist (the car is not found)", city_scene_->pack_ready() ? "is open" : "is not open", city.hi_resident + city.lod_resident);
    }
    return result;
  }
  // The sun: a direction in view space, and above the gate (3 degrees: where the contact shadows' own gate puts the sun's share of the light at nothing).
  const float sun_length = in.to_sun[0] * in.to_sun[0] + in.to_sun[1] * in.to_sun[1] + in.to_sun[2] * in.to_sun[2];
  const float sun_sin = in.to_sun[0] * view.up[0] + in.to_sun[1] * view.up[1] + in.to_sun[2] * view.up[2];
  if (!(sun_length > 0.5f) || sun_sin < rules::kMinSunSin) {
    ++no_sun_;
    return result;
  }
  // How far the answer holds: where the structures are the hi detail the game drew.
  float range = std::min(city_scene_->SafeRange(GameHiRadius()), kMaxRange);
  const double override_range = larecomp::settings::mcla_gpu_city_rt_range.Get();
  if (override_range > 0.0) {
    range = std::min(range, float(override_range));
  }
  fade_range_ = range;
  if (range < kMinRange) {
    ++no_range_;
    return result;
  }
  plume::RenderPipeline* pipeline = Pipeline();
  if (!pipeline || !EnsureOutput(in.depth->width, in.depth->height)) {
    return result;
  }

  const bool check = larecomp::settings::mcla_gpu_city_rt_check.Get();
  const bool check_view = check && larecomp::settings::mcla_gpu_city_rt_view.Get();
  const uint32_t rays = rules::RaysFor(level_ == city_rt::Level::kUltra, larecomp::settings::mcla_gpu_city_rt_rays.Get());
  const rules::AlphaMode alpha = rules::ParseAlphaMode(AlphaSetting().Get());
  const float sun_scale = rules::ClampSunScale(larecomp::settings::mcla_gpu_city_rt_sun.Get());
  const float ray_length = rules::RayLength(sun_sin, city_scene_->FarRadius());
  const bool have_car = in.car_slot != kNoSlot;

  Push push = {};
  push.slots[0] = in.depth->slot;
  push.slots[1] = in.depth->width;
  push.slots[2] = in.depth->height;
  push.slots[3] = rules::PackFlags(rays, alpha, have_car) | (check ? rules::kFlagCheck : 0u) | (check_view ? rules::kFlagView : 0u);
  push.noise[0] = in.sun_frame;
  push.noise[1] = city_rt::kMaskCity | car_rt_rules::kMaskBody | car_rt_rules::kMaskTyres | car_rt_rules::kMaskOthers;
  push.noise[2] = uint32_t(in.frame);
  push.noise[3] = have_car ? in.car_slot : 0u;
  push.lens[0] = in.tan_x;
  push.lens[1] = in.tan_y;
  push.lens[2] = ray_length;
  push.lens[3] = range;
  for (int c = 0; c < 3; ++c) {
    push.light[c] = in.to_sun[c] / std::sqrt(sun_length);
  }
  push.light[3] = rules::kSunRadiusTan * sun_scale;

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  pass_timing_->Mark(list, check ? "city rt check" : "city rt shadow");
  // The depth image was left readable by the pixel pass that made it, and the car's image by its compute pass; this compute shader reads them (a layout both can read) and
  // writes the output as a storage image that the pixel passes after it read.
  {
    const plume::RenderTextureBarrier read(in.depth->texture.get(), Layout::SHADER_READ);
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, &read, 1);
    const plume::RenderTextureBarrier write(image_.texture.get(), Layout::GENERAL);
    list->barriers(Stage::COMPUTE, nullptr, 0, &write, 1);
    image_.layout = Layout::GENERAL;
    list->barriers(Stage::COMPUTE, plume::RenderBufferBarrier(counters_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  }
  // CHECK: CarRtScene leaves the top level built, and readable by a compute shader, when View().valid (a barrier after its build): the city's structures are built in the same list
  // before it (CityRtScene::Prepare ends with a barrier that makes them readable by the build).
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(set_.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((image_.width + kGroup - 1) / kGroup, (image_.height + kGroup - 1) / kGroup, 1);
  {
    const plume::RenderTextureBarrier read(image_.texture.get(), Layout::SHADER_READ);
    list->barriers(Stage::GRAPHICS, nullptr, 0, &read, 1);
    image_.layout = Layout::SHADER_READ;
  }
  if (check && !readback_busy_ && in.frame >= next_readback_) {
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(counters_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(counters_.get(), 0), kCounterBytes);
    readback_busy_ = true;
    next_readback_ = in.frame + kReadbackEveryFrames;
    frames_->RetireAction([this, frame = in.frame, generation = generation_, width = in.depth->width, height = in.depth->height, instances = city.instances] {
      ProcessReadback(frame, generation, width, height, instances);
    });
  }

  idle_frames_ = 0;
  ++passes_run_;
  rays_estimated_ += uint64_t(in.depth->width) * in.depth->height * (check ? 1u : rays);
  // The check's picture stands in for the shadow only with the view on (the contact shadow view 7 shows it); else the check records no result for the march.
  if (!check || check_view) {
    result.recorded = true;
    result.slot = image_.slot;
  }
  const uint32_t state = (rays << 0) | (uint32_t(alpha) << 8) | (uint32_t(level_) << 12) | (check ? 1u << 16 : 0u) | (have_car ? 1u << 17 : 0u);
  if (!first_logged_ || state != last_state_) {
    first_logged_ = true;
    last_state_ = state;
    REXLOG_INFO("{} frame {}: image {}x{} (the occlusion's traced size), level {}, {} rays a pixel, sun x{:.1f}, alpha tested triangles {}, applied within {:.0f} m of the camera, rays up to "
                "{:.0f} m, {} instances in the top level ({} hi detail and {} lod sectors), the car's ray traced image {}{} (pass timing label \"city rt shadow\"; mcla_gpu_rt_shadows off compares; "
                "mcla_gpu_contact_shadows_view 7 shows the term)",
                kPrefix, in.frame, in.depth->width, in.depth->height, city_rt::LevelName(level_), rays, sun_scale,
                alpha == rules::AlphaMode::kExclude ? "left out" : alpha == rules::AlphaMode::kOpaque ? "opaque" : "stochastic (half)", range, ray_length, city.instances, city.hi_resident,
                city.lod_resident, have_car ? "is combined" : "is not there", check ? "; THE CHECK IS ON: primary rays against the depth, the sun's rays are not traced" : "");
  }
  return result;
}

bool CityRtShadow::EnsureOutput(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = image_.texture.get();
  if (!passes_->EnsureImage(image_, width, height, plume::RenderFormat::R8G8B8A8_UNORM, "city rt shadow image", kFlags)) {
    RetireSet();
    return false;
  }
  if (image_.texture.get() != before) {
    RetireSet();  // a new texture is a new set for it (the old one goes after the lists that name it)
  }
  if (!set_) {
    set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, "city rt shadow output set");
    if (!set_) {
      return false;
    }
    set_->setTexture(0, image_.texture.get(), plume::RenderTextureLayout::GENERAL, image_.view.get());
    set_->setBuffer(1, counters_.get(), kCounterBytes);
    set_texture_ = image_.texture.get();
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} image {}x{} R8G8B8A8_UNORM ({:.1f} MiB), given back after {} frames without the pass", kPrefix, width, height, double(HeldBytes()) / (1024.0 * 1024.0),
                kReleaseAfterFrames);
  }
  return true;
}

void CityRtShadow::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_texture_ = nullptr;
}

void CityRtShadow::ReleaseImage() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  RetireSet();
  image_for_[0] = image_for_[1] = 0;
}

void CityRtShadow::ProcessReadback(uint64_t frame, uint64_t generation, uint32_t width, uint32_t height, uint32_t instances) {
  readback_busy_ = false;
  if (generation != generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} check of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  uint32_t words[rules::kStatWords];
  std::memcpy(words, mapped, sizeof(words));
  readback_->unmap();
  // The counters are never cleared: the interval's words are the difference to the last readback (the first readback has no baseline and says only that it is one).
  if (!have_last_words_) {
    std::memcpy(last_words_, words, sizeof(last_words_));
    have_last_words_ = true;
    REXLOG_INFO("{} check of frame {}: the counters' baseline is taken ({} instances in the top level); the next line is the first with numbers", kPrefix, frame, instances);
    return;
  }
  uint32_t delta[rules::kStatWords];
  rules::CheckDelta(words, last_words_, delta);
  std::memcpy(last_words_, words, sizeof(last_words_));
  const rules::CheckSummary s = rules::DecodeCheck(delta);
  if (s.pixels == 0) {
    REXLOG_INFO("{} check of frame {}: no city pixel in the {}x{} depth image in the last interval (the camera sees no city surface, or the sun-less structures are not built)", kPrefix, frame, width, height);
    return;
  }
  // The line the chain reads (perf/CITY-RT-M1-RUNPLAN.md R2): coverage, how many hits are within 1 and 5 percent, the error's median and 95th percentile (the upper edge of the
  // histogram's bin that holds it).
  REXLOG_INFO("{} check of frame {}: {} city pixels over the last interval ({}x{} image, {} instances), {:.1f}% hit a triangle of the city's structures ({} on the hi detail, {} on the lod), "
              "{:.1f}% of the hits are within 1% of the depth image's distance and {:.1f}% within 5%, the error's median is under {:.2f}% and its 95th percentile under {:.2f}%; {} pixels met "
              "nothing",
              kPrefix, frame, s.pixels, width, height, instances, s.coverage * 100.0, s.hi, s.lod, s.within1_share * 100.0, s.within5_share * 100.0, s.p50 * 100.0, s.p95 * 100.0, s.no_hit);
}

uint64_t CityRtShadow::HeldBytes() const { return image_.texture ? TextureBytes(image_.width, image_.height, 1, image_.format) : 0; }

std::string CityRtShadow::CensusText() const {
  if (!ready_) {
    return {};
  }
  return std::format("; ray traced city shadows: {} passes since the start, {} frames without the city in the top level, {} without a safe range, {} without the sun, {} waiting for the pipeline",
                     passes_run_, no_scene_, no_range_, no_sun_, not_ready_);
}

}  // namespace mcla::gpu
