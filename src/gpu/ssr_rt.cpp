// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections. See ssr_rt.h and ssr_rt_rules.h.

#include "ssr_rt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "rt_tier.h"
#include "rt_world_frame.h"

namespace mcla::gpu {

namespace {

namespace rules = ssr_rt;

constexpr const char* kPrefix = "[ssr rt]";
// The pipelines' place among the effect's in the host precache: the same owner, variants past the graphics passes'.
constexpr uint32_t kPipelineVariantBase = 0x100;
// A frame without the fallback this many times in a row gives the output set back (2 s at 60).
constexpr uint32_t kReleaseAfterFrames = 120;
// The census line is logged at most this often (frames), at a stats frame.
constexpr uint64_t kLogEveryFrames = 120;

// mcla_gpu_ssr_rt (off, high, ultra), read through LiveText (the pause menu's thread writes the cvar's string while this one reads it).
LiveText& Setting() {
  static LiveText text("mcla_gpu_ssr_rt");
  return text;
}

uint64_t PipelineKey(uint32_t variant) { return HostPrecache::Key(HostPrecache::kScreenReflections, kPipelineVariantBase + variant); }

}  // namespace

// Two timestamps round the two passes, a ring of pools so a frame never waits for the GPU: the results are read by a FrameRing action once the list that wrote them has completed.
struct ScreenReflectionsRt::Timer {
  static constexpr uint32_t kRing = 4;
  std::array<std::unique_ptr<plume::RenderQueryPool>, kRing> pools;
  std::array<bool, kRing> busy{};
  uint32_t next = 0;
  bool ok = false;
};

ScreenReflectionsRt::ScreenReflectionsRt() = default;
ScreenReflectionsRt::~ScreenReflectionsRt() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void ScreenReflectionsRt::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* car_scene, RtSceneService* service,
                                     RtWorldFrame* world) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  car_scene_ = car_scene;
  service_ = service;
  world_ = world;
  ready_ = false;
  listed_.fill(false);
  failed_.fill(false);
  const bool supported = car_scene != nullptr && car_scene->ready() && service != nullptr && world != nullptr && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops());
  if (!supported) {
    return;   // no ray queries here (or no scene): the effect is what it was
  }
  if (!MakeLayoutAndShaders(device, passes, *car_scene)) {
    REXLOG_ERROR("{} the layout, a shader or a buffer could not be made: no ray traced fallback this session (the screen-space reflections are what they were)", kPrefix);
    for (auto& shader : shaders_) shader.reset();
    layout_.reset();
    ctl_.reset();
    list_.reset();
    readback_.reset();
    return;
  }
  timer_ = std::make_unique<Timer>();
  if (backend::TimestampsSupported(device.device())) {
    timer_->ok = true;
    for (auto& pool : timer_->pools) {
      pool = device.CreateQueryPool(2, "ssr rt timestamps");
      timer_->ok = timer_->ok && pool != nullptr;
    }
  }
  if (!timer_->ok) {
    REXLOG_INFO("{} no timestamps on this device: the governor has no signal and holds the cell's cap", kPrefix);
  }
  reset_pending_ = true;
  ready_ = true;
}

bool ScreenReflectionsRt::MakeLayoutAndShaders(Device& device, HostPasses& passes, CarRtScene& scene) {
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);              // u0: the trace image
  output_builder_.addReadWriteByteAddressBuffer(1);    // u1: the control and statistics words
  output_builder_.addReadWriteByteAddressBuffer(2);    // u2: the list
  output_builder_.end();
  layout_ = MakeLayout(output_builder_, scene, "ssr rt layout");
  if (!layout_) {
    return false;
  }
  bool ok = true;
  for (uint32_t i = 0; i < kVariantCount; ++i) {
    const HostShaderBlob blob = ScreenReflectionsRtShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = ok && shaders_[i] != nullptr;
  }
  ctl_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(ssr_rt::buf::kBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "ssr rt control");
  list_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(ssr_rt::kListBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "ssr rt list");
  readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(ssr_rt::buf::kBytes), "ssr rt readback");
  return ok && ctl_ && list_ && readback_;
}

std::unique_ptr<plume::RenderPipelineLayout> ScreenReflectionsRt::MakeLayout(const plume::RenderDescriptorSetBuilder& output,
                                                                           const CarRtScene& scene, const char* name) {
  if (!device_ || !heap_) return nullptr;
  // The SSR host pass layout (ssr.cpp Initialize: the four texture sets, the samplers, the push block at (b0, space4)) with this pass's output set at 5 and the car scene's ray tracing set at 6.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap_->texture_set_builder());
  }
  const uint32_t sampler_set = builder.addDescriptorSet(heap_->sampler_set_builder());
  const uint32_t output_set = builder.addDescriptorSet(output);
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());
  builder.addPushConstant(0, 4, sizeof(ssr::Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (sampler_set != 4 || output_set != 5 || scene_set != 6) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shaders declare them", kPrefix);
    return nullptr;
  }
  return device_->CreatePipelineLayout(builder, name);
}

void ScreenReflectionsRt::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();  // the host builder's functions name this object's layout and shaders: stop it first
  }
  ++generation_;
  capture_set_.reset();
  capture_select_.reset();
  for (auto& pipeline : capture_trace_) pipeline.reset();
  capture_layout_.reset();
  set_.reset();
  set_texture_ = nullptr;
  for (auto& pipeline : pipelines_) pipeline.reset();
  for (auto& shader : shaders_) shader.reset();
  ctl_.reset();
  list_.reset();
  readback_.reset();
  readback_busy_ = false;
  timer_.reset();
  layout_.reset();
  ready_ = false;
  resolved_ = ssr_rt::Resolved{};
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  car_scene_ = nullptr;
  service_ = nullptr;
  world_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> ScreenReflectionsRt::MakePipeline(uint32_t variant) {
  if (!layout_ || !shaders_[variant]) {
    return nullptr;
  }
  const bool select = variant == kVSelect;
  const char* name = select ? "ssr rt select" : (variant == kVTraceMs ? "ssr rt trace (multisampled planes)" : "ssr rt trace");
  const uint32_t x = select ? ssr_rt::kTile : ssr_rt::kThreads;
  const uint32_t y = select ? ssr_rt::kTile : 1;
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shaders_[variant].get(), x, y, 1), name);
}

plume::RenderPipeline* ScreenReflectionsRt::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (failed_[variant] || !ready_ || !passes_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = PipelineKey(variant);
  if (!listed_[variant]) {
    listed_[variant] = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, variant == kVSelect ? "ssr rt select" : (variant == kVTraceMs ? "ssr rt trace (multisampled planes)" : "ssr rt trace"), [this, variant] { return MakePipeline(variant); }});
    precache.AddLive("screen reflections ray traced fallback", std::move(items));
    REXLOG_INFO("{} the pipeline {} is wanted: it is made in the background, and the fallback runs from the frame it is done", kPrefix, variant);
  }
  if (!precache.Ready(key)) {
    return nullptr;  // a frame never waits for a compile
  }
  pipelines_[variant] = precache.Acquire(key, "ssr rt", [this, variant] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline {} could not be made: no ray traced fallback in that configuration this session", kPrefix, variant);
  }
  return pipelines_[variant].get();
}

bool ScreenReflectionsRt::PipelinesReady(bool multisampled) {
  if (!wanted()) {
    return false;
  }
  // Both are asked (and so listed) before either's answer is used: the compiles go side by side.
  plume::RenderPipeline* select = Pipeline(kVSelect);
  plume::RenderPipeline* trace = Pipeline(multisampled ? kVTraceMs : kVTrace);
  return select != nullptr && trace != nullptr;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void ScreenReflectionsRt::BeginFrame(ssr::Level ssr_level) {
  ran_ = false;
  if (!ready_) {
    resolved_ = ssr_rt::Resolved{};
    shade_cars_ = false;
    return;
  }
  const ssr_rt::Resolved now = ssr_rt::Resolve({ssr_level, ssr_rt::ParseChoice(Setting().Get()), gpu_class::CurrentRtRung()});
  if (now.level != resolved_.level) {
    governor_.Reset();   // the cell changed: what the old one cost says nothing about the new
    ms_ = -1.0;
    logged_step_ = 0;
  }
  resolved_ = now;
  shade_cars_ = ssr_rt::CarHitsEnabled(now.cell, larecomp::settings::mcla_gpu_ssr_rt_cars.Get());
  if (!logged_level_once_ || now.level != logged_level_) {
    logged_level_once_ = true;
    logged_level_ = now.level;
    if (now.level == ssr_rt::Level::kOff) {
      REXLOG_INFO("{} ray traced fallback off: {} (mcla_gpu_ssr_rt {}, live)", kPrefix, ssr_rt::WhyName(now.why), Setting().Get());
    } else {
      REXLOG_INFO("{} ray traced fallback {} (mcla_gpu_ssr_rt {}, live): classes {}{}{}, at most {} rays a frame ({} a pixel), up to {:.0f} m, the governor holds its time to {:.2f} ms", kPrefix,
                  ssr::LevelName(now.level), Setting().Get(), (now.cell.group_mask & ssr_rt::GroupBit(ssr_rt::kGroupBright)) != 0 ? ssr_rt::GroupName(ssr_rt::kGroupBright) : "",
                  now.cell.group_mask == (ssr_rt::GroupBit(ssr_rt::kGroupBright) | ssr_rt::GroupBit(ssr_rt::kGroupGround)) ? ", " : "",
                  (now.cell.group_mask & ssr_rt::GroupBit(ssr_rt::kGroupGround)) != 0 ? ssr_rt::GroupName(ssr_rt::kGroupGround) : "", now.cell.ray_cap, now.cell.rays, double(now.cell.reach),
                  double(now.cell.budget_ms));
    }
  }
  if (wanted() && service_) {
    // The structures, the hit data and every texture at the level's city level, in EVERY frame the fallback is on (a want counts for its frame and the next: the top level of the trigger is built from the
    // wants of the frame before and this one), while the pipelines are still being made.
    service_->Want(rt_scene::Consumer::kSsrFallback, ssr_rt::NeedsFor(resolved_.level, shade_cars_));
  }
}

bool ScreenReflectionsRt::EnsureSet(const HostImage& trace) {
  if (set_ && set_texture_ == trace.texture.get()) {
    return true;
  }
  RetireSet();
  // Three views: u0 the trace image, u1 the control words and u2 the list (output_builder_). Device::CreateDescriptorSet compares this count with what the backend allocated, and D3D12 allocates one view
  // for each descriptor (the city shadow pass's set learned that on 2026-11-21); Vulkan does not count. ssr_rt_rules_test reads every CreateDescriptorSet call's literal count against its builder.
  set_ = device_->CreateDescriptorSet(output_builder_, 3, 0, "ssr rt output set");
  if (!set_) {
    return false;
  }
  set_->setTexture(0, trace.texture.get(), plume::RenderTextureLayout::GENERAL, trace.view.get());
  set_->setBuffer(1, ctl_.get(), ssr_rt::buf::kBytes);
  set_->setBuffer(2, list_.get(), ssr_rt::kListBytes);
  set_texture_ = trace.texture.get();
  return true;
}

void ScreenReflectionsRt::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_texture_ = nullptr;
}

bool ScreenReflectionsRt::PrepareCaptureHit(const HostImage& trace, const HostImage& hit, bool multisampled) {
  if (!ready_ || !device_ || !car_scene_ || !passes_ || !ctl_ || !list_ || !shaders_[kVSelect] ||
      !trace.texture || !trace.view || !hit.texture || !hit.view) return false;
  // Keep the normal output layout and its descriptor count on the regular path.
  if (!capture_layout_) {
    capture_builder_.begin();
    capture_builder_.addReadWriteTexture(0);
    capture_builder_.addReadWriteByteAddressBuffer(1);
    capture_builder_.addReadWriteByteAddressBuffer(2);
    capture_builder_.addReadWriteTexture(3);
    capture_builder_.end();
    capture_layout_ = MakeLayout(capture_builder_, *car_scene_, "ssr rt capture layout");
    if (!capture_layout_) return false;
  }
  if (!capture_select_) {
    capture_select_ = device_->CreateComputePipeline(
        plume::RenderComputePipelineDesc(capture_layout_.get(), shaders_[kVSelect].get(), ssr_rt::kTile, ssr_rt::kTile, 1),
        "ssr rt capture select");
  }
  auto& pipeline = capture_trace_[multisampled];
  if (!pipeline) {
    const auto blob = ScreenReflectionsRtHitShader(multisampled);
    const auto shader = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (shader) pipeline = device_->CreateComputePipeline(
        plume::RenderComputePipelineDesc(capture_layout_.get(), shader.get(), ssr_rt::kThreads, 1, 1), blob.name);
  }
  if (!capture_select_ || !pipeline) return false;
  const std::array serials{trace.serial, hit.serial};
  if (capture_set_ && capture_serials_ == serials) return true;
  ReleaseCaptureSet();
  capture_set_ = device_->CreateDescriptorSet(capture_builder_, 4, 0, "ssr rt capture output");
  if (!capture_set_) return false;
  capture_set_->setTexture(0, trace.texture.get(), plume::RenderTextureLayout::GENERAL, trace.view.get());
  capture_set_->setBuffer(1, ctl_.get(), ssr_rt::buf::kBytes);
  capture_set_->setBuffer(2, list_.get(), ssr_rt::kListBytes);
  capture_set_->setTexture(3, hit.texture.get(), plume::RenderTextureLayout::GENERAL, hit.view.get());
  capture_serials_ = serials;
  return true;
}

void ScreenReflectionsRt::ReleaseCaptureSet() {
  if (capture_set_ && frames_) frames_->Retire(std::move(capture_set_));
  capture_set_.reset();
}

bool ScreenReflectionsRt::Record(const Frame& in) {
  if (!ready_ || !in.list || !in.trace || !in.trace->texture || !in.confidence || !in.confidence->texture || !car_scene_ || !world_ || !wanted()) {
    return false;
  }
  plume::RenderCommandList* list = in.list;
  const ssr_rt::Cell& cell = resolved_.cell;
  const CarRtView& view = car_scene_->View();
  HostImage* hit = in.capture_hit;
  if (hit && !hit->texture) return false;
  plume::RenderPipeline* select_pipeline = hit ? capture_select_.get() : Pipeline(kVSelect);
  plume::RenderPipeline* trace_pipeline = hit ? capture_trace_[in.multisampled].get() : Pipeline(in.multisampled ? kVTraceMs : kVTrace);

  ssr_rt::FrameFacts facts;
  facts.resolved = resolved_;
  facts.stub = false;   // ScreenReflections does not call this for the stub
  facts.pipelines_ready = select_pipeline != nullptr && trace_pipeline != nullptr;
  facts.scene_valid = view.scene_valid && view.set != nullptr;
  facts.images_ok = hit ? capture_set_ != nullptr : EnsureSet(*in.trace);
  const ssr_rt::Why why = ssr_rt::DecideFrame(facts);
  last_why_ = why;
  if (why != ssr_rt::Why::kRuns) {
    ++totals_.frames_not_run;
    totals_.last_why = why;
    if (uint32_t(why) != why_logged_) {
      why_logged_ = uint32_t(why);
      REXLOG_INFO("{} frame {}: the fallback is wanted and does not run: {} (logged when it changes)", kPrefix, in.frame, ssr_rt::WhyName(why));
    }
    return false;
  }
  if (why_logged_ != uint32_t(ssr_rt::Why::kRuns)) {
    why_logged_ = uint32_t(ssr_rt::Why::kRuns);
    REXLOG_INFO("{} frame {}: the fallback runs", kPrefix, in.frame);
  }

  // The governor: the cap's step for this frame (the passes' time of the frames before it, measured by the timestamps below).
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_start_).count();
  if (governor_.Update(now, double(cell.budget_ms))) {
    totals_.step_changes = governor_.changes();
    REXLOG_INFO("{} the governor moves the cap to step {} of {} ({:.2f} ms average against {:.2f} ms): {} rays a frame", kPrefix, governor_.step(), ssr_rt::Governor::kMaxStep, governor_.ewma_ms(),
                double(cell.budget_ms), ssr_rt::RayCap(cell, governor_.step()));
  }
  const bool single_ray = larecomp::settings::mcla_gpu_ssr_rt_single_ray.Get();
  const uint32_t rays = ssr_rt::RaysUsed(cell, std::max<uint32_t>(in.ssr_rays, 1), single_ray);
  const uint32_t capacity = ssr_rt::CapacityFor(cell, governor_.step(), rays);
  if (capacity == 0) {
    return false;
  }
  const bool stats = world_->StatsFrame() && world_->Valid() && !readback_busy_;

  ssr::Push push = in.push;
  namespace pk = ssr_rt::push;
  uint32_t flags = (stats ? pk::kFlagStats : 0u) | (in.screen ? pk::kFlagScreen : 0u);
  if (single_ray) flags |= pk::kFlagSingleRay;
  if (larecomp::settings::mcla_gpu_ssr_rt_sky_miss.Get()) flags |= pk::kFlagSkyMiss;
  if (larecomp::settings::mcla_gpu_ssr_rt_screen_proxy.Get()) flags |= pk::kFlagScreenProxy;
  if (stats && larecomp::settings::mcla_gpu_ssr_rt_probe_misses.Get()) flags |= pk::kFlagProbeMisses;
  if (shade_cars_) flags |= pk::kFlagCars;
  push.Set(pk::kConf, ssr_rt::PackConf(in.confidence->slot, cell.group_mask, rays, flags));
  push.Set(pk::kCap, capacity);
  push.SetFloat(pk::kReach, cell.reach);
  push.SetFloat(pk::kThreshold, ssr_rt::kConfidenceThreshold);
  push.Set(pk::kFrame, rt_frame_);

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  int timer_slot = -1;
  if (timer_ && timer_->ok && !timer_->busy[timer_->next]) {
    timer_slot = int(timer_->next);
    list->resetQueryPool(timer_->pools[timer_slot].get(), 0, 2);
    list->writeTimestamp(timer_->pools[timer_slot].get(), 0);
    timer_->busy[timer_slot] = true;
  }
  {
    // The trace image becomes a storage image for the two passes; the control words and the list are read and written by both.
    const plume::RenderTextureBarrier write(in.trace->texture.get(), Layout::GENERAL);
    list->barriers(Stage::COMPUTE, nullptr, 0, &write, 1);
    in.trace->layout = Layout::GENERAL;
    // CHECK: validate the capture MRT -> storage image -> readback on both APIs.
    if (hit) {
      const plume::RenderTextureBarrier write_hit(hit->texture.get(), Layout::GENERAL);
      list->barriers(Stage::COMPUTE, nullptr, 0, &write_hit, 1);
      hit->layout = Layout::GENERAL;
    }
    const plume::RenderBufferBarrier buffers[2] = {{ctl_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}, {list_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
    list->barriers(Stage::COMPUTE, buffers, 2);
  }
  list->setComputePipelineLayout(hit ? capture_layout_.get() : layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(heap_->sampler_set(), 4);
  list->setComputeDescriptorSet(hit ? capture_set_.get() : set_.get(), 5);
  list->setComputeDescriptorSet(view.set, 6);

  const uint32_t tiles_x = (in.width + ssr_rt::kTile - 1) / ssr_rt::kTile;
  const uint32_t tiles_y = (in.height + ssr_rt::kTile - 1) / ssr_rt::kTile;
  const plume::RenderBufferBarrier between[2] = {{ctl_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}, {list_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
  list->setPipeline(select_pipeline);
  if (reset_pending_) {
    // The first frame: the control words are not known, so a pass of its own clears them (the groups all return but the first, which writes zeros).
    ssr::Push reset = push;
    reset.Set(pk::kConf, ssr_rt::PackConf(in.confidence->slot, cell.group_mask, rays, flags | pk::kFlagReset));
    list->setComputePushConstants(0, &reset, 0, sizeof(reset));
    list->dispatch(tiles_x, tiles_y, 1);
    list->barriers(Stage::COMPUTE, between, 2);
    reset_pending_ = false;
  }
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(tiles_x, tiles_y, 1);
  list->barriers(Stage::COMPUTE, between, 2);
  list->setPipeline(trace_pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(ssr_rt::GroupsFor(capacity), 1, 1);
  {
    const plume::RenderTextureBarrier read(in.trace->texture.get(), Layout::SHADER_READ);
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, &read, 1);
    in.trace->layout = Layout::SHADER_READ;
    if (hit) {
      const plume::RenderTextureBarrier read_hit(hit->texture.get(), Layout::SHADER_READ);
      list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, &read_hit, 1);
      hit->layout = Layout::SHADER_READ;
    }
  }
  if (timer_slot >= 0) {
    list->writeTimestamp(timer_->pools[timer_slot].get(), 1);
    timer_->next = (timer_->next + 1) % Timer::kRing;
    frames_->RetireAction([this, slot = uint32_t(timer_slot), generation = generation_] {
      if (generation != generation_ || !timer_) {
        return;
      }
      timer_->busy[slot] = false;
      plume::RenderQueryPool* pool = timer_->pools[slot].get();
      if (!pool) {
        return;
      }
      pool->queryResults();
      const uint64_t* t = pool->getResults();
      if (t[1] >= t[0]) {
        const double ms = double(t[1] - t[0]) * 1.0e-6;
        if (ms < 1000.0) {
          governor_.Note(ms);
          ms_ = governor_.ewma_ms();
          totals_.ms = ms_;
        }
      }
    });
  }

  ssr_rt::FrameLine line;
  line.frame = in.frame;
  line.level = resolved_.level;
  line.group_mask = cell.group_mask;
  line.reach = cell.reach;
  line.capacity = capacity;
  line.step = governor_.step();
  line.rays_per_pixel = rays;
  line.budget_ms = cell.budget_ms;
  line.ms = ms_;
  line.keep = 1.0f;
  if (stats) {
    // The census's counters of this stats frame (and the library's calibration counters): read back once the list has completed.
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(ctl_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(ctl_.get(), 0), ssr_rt::buf::kBytes);
    readback_busy_ = true;
    frames_->RetireAction([this, generation = generation_, rt_frame = rt_frame_, frame = in.frame, line] { ProcessReadback(generation, rt_frame, frame, line, true); });
  }
  ++rt_frame_;
  ++totals_.frames_run;
  totals_.step = governor_.step();
  ran_ = true;
  idle_frames_ = 0;
  return true;
}

void ScreenReflectionsRt::ProcessReadback(uint64_t generation, uint32_t rt_frame, uint64_t frame, const ssr_rt::FrameLine& base, bool) {
  readback_busy_ = false;
  if (generation != generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  std::array<uint32_t, ssr_rt::buf::kStatWords> words;
  std::memcpy(words.data(), mapped, sizeof(uint32_t) * words.size());
  readback_->unmap();
  // The counters are never cleared by the host: the interval's words are the difference to the last readback (the first readback only takes the baseline).
  if (!have_last_words_) {
    last_words_ = words;
    have_last_words_ = true;
    return;
  }
  std::array<uint32_t, ssr_rt::buf::kStatWords> delta;
  ssr_rt::Delta(words.data(), last_words_.data(), ssr_rt::buf::kStatWords, delta.data());
  last_words_ = words;
  // the calibration's window, one for every consumer of the world frame
  if (world_) {
    world_->AddCalibration(delta.data() + ssr_rt::buf::kStatCal);
  }
  ssr_rt::FrameLine line = base;
  const uint32_t slot = rt_frame % 3;
  line.candidates = words[ssr_rt::buf::kCtlTotal + slot];
  line.listed = std::min(words[ssr_rt::buf::kCtlListed + slot], line.capacity);
  line.keep = ssr_rt::Keep(line.capacity, line.candidates);
  const uint32_t* census = delta.data() + ssr_rt::buf::kStatCensus;
  line.rays = census[ssr_rt::buf::kRays];
  line.city = census[ssr_rt::buf::kCity];
  line.car = census[ssr_rt::buf::kCar];
  line.miss = census[ssr_rt::buf::kMiss];
  line.no_data = census[ssr_rt::buf::kNoData];
  line.screen = census[ssr_rt::buf::kScreen];
  line.shaded = census[ssr_rt::buf::kShaded];
  line.filled = census[ssr_rt::buf::kFilled];
  line.ms = ms_;
  ++totals_.stats_frames;
  totals_.candidates += line.candidates;
  totals_.listed += line.listed;
  totals_.rays += line.rays;
  totals_.city += line.city;
  totals_.car += line.car;
  totals_.miss += line.miss;
  totals_.no_data += line.no_data;
  totals_.screen += line.screen;
  totals_.shaded += line.shaded;
  totals_.filled += line.filled;
  if (!have_line_ || frame >= last_log_frame_ + kLogEveryFrames) {
    last_log_frame_ = frame;
    have_line_ = true;
    REXLOG_INFO("{}", ssr_rt::FormatFrameLine(line));
    using namespace ssr_rt::buf;
    REXLOG_INFO("{} frame {} detail: car hits shaded {}; no data LOD {} placeholder {} unavailable {}; raster proxy fills {} environment misses {}; "
                "miss probes {} beyond reach to {:.0f} m {} open upward {}",
                kPrefix, frame, census[kCarShaded], census[kNoDataLod], census[kNoDataDefault], census[kNoDataMissing], census[kProxyRecovered], census[kEnvMiss], census[kMissProbed], 2.0f * line.reach,
                census[kMissBeyond], census[kMissOpenUp]);
  }
  last_line_ = line;
}

void ScreenReflectionsRt::OnPresent() {
  if (ran_) {
    idle_frames_ = 0;
  } else if (set_ && ++idle_frames_ >= kReleaseAfterFrames) {
    RetireSet();   // the trace image may be gone by now (ScreenReflections gives its images back after a pause): the set is made again when it is wanted
    idle_frames_ = 0;
  }
  ran_ = false;
}

uint64_t ScreenReflectionsRt::HeldBytes() const {
  return ready_ ? ssr_rt::kListBytes + ssr_rt::buf::kBytes : 0;
}

std::string ScreenReflectionsRt::CensusText() const {
  ssr_rt::Totals totals = totals_;
  totals.step = governor_.step();
  totals.step_changes = governor_.changes();
  totals.ms = ms_;
  totals.last_why = last_why_;
  return ssr_rt::FormatTotals(totals);
}

}  // namespace mcla::gpu
