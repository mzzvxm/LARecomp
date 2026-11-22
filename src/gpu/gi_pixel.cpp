// MCLA GPU renderer -- the probe field's per-pixel detail pass. See gi_pixel.h; the rules are gi_pixel_rules.h, the trace hlsl/gi_pixel_trace_cs.hlsl.

#include "gi_pixel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "city_hit_scene.h"
#include "city_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "light_manager.h"
#include "pass_timing.h"
#include "rt_world_frame.h"

namespace mcla::gpu {

namespace {

namespace rules = gi_pixel;

constexpr const char* kPrefix = "[gi pixel]";
constexpr plume::RenderBufferFlags kStorageFlags = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS;
constexpr plume::RenderTextureFlags kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
constexpr uint32_t kReleaseAfterFrames = 120;       // images back after this many frames without a run (2 s at 60)
constexpr uint32_t kReadbackEveryFrames = 600;
constexpr uint32_t kStatsEvery = 10;                // a census frame: the counters are added to

using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;

}  // namespace

GiPixel::~GiPixel() = default;

const char* GiPixel::VariantName(uint32_t variant) { return variant == kVPlain ? "gi pixel trace" : "gi pixel trace ms"; }

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void GiPixel::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, GiField& field, CarRtScene& car_scene, CityRtScene& city_scene,
                         CityHitScene& hit_scene, RtWorldFrame& world, const LightManager* lights) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  field_ = &field;
  car_scene_ = &car_scene;
  city_scene_ = &city_scene;
  hit_scene_ = &hit_scene;
  world_ = &world;
  lights_ = lights;
  ready_ = false;
  pipeline_failed_ = {};
  listed_ = {};
  const bool supported = field.available() && car_scene.ready() && city_scene.initialized() && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops());
  if (!supported) {
    passes.precache().Note("ray traced bounce light's per-pixel pass: not on this graphics card or backend (the probe field alone is the bounce light)", 0);
    return;
  }
  if (!MakeLayoutsAndShaders(car_scene)) {
    passes.precache().Note("ray traced bounce light's per-pixel pass: a layout or a shader could not be made (the probe field alone is the bounce light)", 0);
    return;
  }
  // THE DENOISER: RtDenoise instance 1 (the reflections' fallback is 0), the diffuse compile of the plain consumer's two stages.
  const HostShaderBlob spatial = RtDenoiseDiffuseShader(false);
  const HostShaderBlob temporal = RtDenoiseDiffuseShader(true);
  denoise_.Initialize(device, frames, heap, passes, /*instance=*/1, "gi pixel", RtDenoise::Blob{spatial.data, spatial.size, spatial.name}, RtDenoise::Blob{temporal.data, temporal.size, temporal.name});
  if (!denoise_.available()) {
    passes.precache().Note("ray traced bounce light's per-pixel pass: the denoiser could not be made (the probe field alone is the bounce light)", 0);
    return;
  }
  stats_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(rules::kStatBytes, kStorageFlags), "gi pixel counters");
  readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(rules::kStatBytes), "gi pixel counters readback");
  if (!stats_ || !readback_) {
    passes.precache().Note("ray traced bounce light's per-pixel pass: the counters could not be made (the probe field alone is the bounce light)", 0);
    stats_.reset();
    readback_.reset();
    return;
  }
  // The timestamps of the governor: a pool of two a frame slot. Without them the pass stays at the traced size (said once in the log).
  stamps_ok_ = backend::TimestampsSupported(device.device());
  for (Slot& s : slots_) {
    if (stamps_ok_) {
      s.stamps = device.CreateQueryPool(2, "gi pixel timestamps");
      stamps_ok_ = s.stamps != nullptr;
    }
  }
  if (!stamps_ok_) {
    for (Slot& s : slots_) {
      s.stamps.reset();
    }
    REXLOG_INFO("{} no GPU timestamps: the per-pixel pass's governor holds the traced size", kPrefix);
  }
  ready_ = true;
  passes.precache().Note("ray traced bounce light's per-pixel pass: made when first wanted (ULTRA on a strong ray tracing card)", 0);
}

bool GiPixel::MakeLayoutsAndShaders(CarRtScene& scene) {
  // THE PASS SET: rows, the four images as storage, the counters (registers unique inside the set: gi_pixel_rules.h).
  pass_set_builder_.begin();
  rows_index_ = pass_set_builder_.addByteAddressBuffer(rules::kBindRows);
  signal_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindSignal);
  depth_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindDepth);
  normal_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindNormal);
  hit_index_ = pass_set_builder_.addReadWriteTexture(rules::kBindHit);
  stats_index_ = pass_set_builder_.addReadWriteByteAddressBuffer(rules::kBindStats);
  pass_set_builder_.end();

  // The trace: the bindless textures (plain, and as multisampled planes), the samplers, the pass set, the ray tracing set, the push block.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());
  const uint32_t texture_ms_set = builder.addDescriptorSet(heap_->texture_set_builder());
  const uint32_t sampler_set = builder.addDescriptorSet(heap_->sampler_set_builder());
  const uint32_t pass_set = builder.addDescriptorSet(pass_set_builder_);
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());
  builder.addPushConstant(0, rules::kPushSpace, sizeof(rules::Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != rules::kTextureSet || texture_ms_set != rules::kTextureMsSet || sampler_set != rules::kSamplerSet || pass_set != rules::kPassSet || scene_set != rules::kSceneSet) {
    REXLOG_ERROR("{} the trace layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  layout_ = device_->CreatePipelineLayout(builder, "gi pixel trace layout");
  if (!layout_) {
    return false;
  }
  for (uint32_t v = 0; v < kVariantCount; ++v) {
    const HostShaderBlob blob = GiPixelTraceShader(v == kVMs);
    shaders_[v] = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (!shaders_[v]) {
      return false;
    }
  }
  return true;
}

void GiPixel::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();  // the host builder's functions name this object's layout and shaders: stop it first
  }
  denoise_.Shutdown();
  // The GPU is idle: nothing is retired, the objects just go.
  for (Slot& s : slots_) {
    s = Slot{};
  }
  signal_ = HostImage{};
  for (HostImage& d : depth_) {
    d = HostImage{};
  }
  normal_ = HostImage{};
  hit_ = HostImage{};
  stats_.reset();
  readback_.reset();
  readback_busy_ = false;
  for (auto& p : pipelines_) {
    p.reset();
  }
  for (auto& s : shaders_) {
    s.reset();
  }
  layout_.reset();
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
  world_ = nullptr;
  lights_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines (made in the background the first time the pass is wanted: a frame never waits for a compile)
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> GiPixel::MakePipeline(uint32_t variant) {
  if (!layout_ || !shaders_[variant]) {
    return nullptr;
  }
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shaders_[variant].get(), rules::kGroup, rules::kGroup, 1), VariantName(variant));
}

bool GiPixel::PipelineReady(uint32_t variant) {
  if (pipelines_[variant]) {
    return true;
  }
  if (pipeline_failed_[variant] || !passes_) {
    return false;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kGiPixel, variant);
  if (!listed_[variant]) {
    listed_[variant] = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, VariantName(variant), [this, variant] { return MakePipeline(variant); }});
    precache.AddLive("ray traced bounce light's per-pixel pass", std::move(items));
    REXLOG_INFO("{} the pipeline '{}' is wanted: it is made in the background, and the pass starts from the frame it is done", kPrefix, VariantName(variant));
  }
  if (!precache.Ready(key)) {
    return false;
  }
  pipelines_[variant] = precache.Acquire(key, VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no per-pixel pass this session (the probe field alone is the bounce light)", kPrefix, VariantName(variant));
  }
  return pipelines_[variant] != nullptr;
}

// ---------------------------------------------------------------------------
// the images and the sets
// ---------------------------------------------------------------------------

void GiPixel::RetireSets() {
  for (Slot& s : slots_) {
    for (uint32_t p = 0; p < 2; ++p) {
      if (frames_) {
        frames_->Retire(std::move(s.set[p]));
      }
      s.set[p].reset();
      s.set_key[p] = 0;
    }
  }
}

void GiPixel::ReleaseImages() {
  RetireSets();
  if (passes_) {
    if (signal_.texture) passes_->ReleaseImage(signal_);
    for (HostImage& d : depth_) {
      if (d.texture) passes_->ReleaseImage(d);
    }
    if (normal_.texture) passes_->ReleaseImage(normal_);
    if (hit_.texture) passes_->ReleaseImage(hit_);
  }
  ++image_serial_;
  image_width_ = image_height_ = 0;
  have_prev_ = false;
  history_frames_ = 0;
  denoise_.ResetHistory();
}

bool GiPixel::EnsureImages(uint32_t width, uint32_t height) {
  if (signal_.texture && image_width_ == width && image_height_ == height) {
    return true;
  }
  ReleaseImages();
  bool ok = passes_->EnsureImage(signal_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "gi pixel signal", kImageFlags) &&
            passes_->EnsureImage(depth_[0], width, height, plume::RenderFormat::R32_FLOAT, "gi pixel depth 0", kImageFlags) &&
            passes_->EnsureImage(depth_[1], width, height, plume::RenderFormat::R32_FLOAT, "gi pixel depth 1", kImageFlags) &&
            passes_->EnsureImage(normal_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "gi pixel normal", kImageFlags) &&
            passes_->EnsureImage(hit_, width, height, plume::RenderFormat::R16G16_FLOAT, "gi pixel hit", kImageFlags);
  // the denoiser's guides name their images by 16 bit slots (rt_denoise_rules.h plain::)
  ok = ok && signal_.slot <= 0xFFFEu && depth_[0].slot <= 0xFFFEu && depth_[1].slot <= 0xFFFEu && normal_.slot <= 0xFFFEu && hit_.slot <= 0xFFFEu;
  if (!ok) {
    ReleaseImages();
    return false;
  }
  image_width_ = width;
  image_height_ = height;
  ++image_serial_;
  REXLOG_INFO("{} the pass's images: {}x{}, {:.1f} MiB (the signal, two depth guides, the view normal, the hit image; the denoiser's own four are counted by it)", kPrefix, width, height,
              double(uint64_t(width) * height * 28) / (1024.0 * 1024.0));
  return true;
}

bool GiPixel::EnsureSlot(uint32_t frame_slot) {
  Slot& s = slots_[frame_slot];
  if (!s.rows) {
    s.rows = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(rules::kBufferAlloc, plume::RenderBufferFlag::STORAGE), "gi pixel rows");
    if (!s.rows) {
      return false;
    }
  }
  for (uint32_t p = 0; p < 2; ++p) {
    const uint64_t key = (image_serial_ << 1) | p;
    if (s.set[p] && s.set_key[p] == key) {
      continue;
    }
    if (s.set[p] && frames_) {
      frames_->Retire(std::move(s.set[p]));
    }
    s.set[p] = device_->CreateDescriptorSet(pass_set_builder_, rules::kSetViews, 0, "gi pixel set");  // six views: D3D12 sizes a set by this count. CHECK (run): the debug layer accepts it
    if (!s.set[p]) {
      return false;
    }
    s.set[p]->setBuffer(rows_index_, s.rows.get(), rules::kBufferAlloc);
    s.set[p]->setTexture(signal_index_, signal_.texture.get(), Layout::GENERAL, signal_.view.get());
    s.set[p]->setTexture(depth_index_, depth_[p].texture.get(), Layout::GENERAL, depth_[p].view.get());
    s.set[p]->setTexture(normal_index_, normal_.texture.get(), Layout::GENERAL, normal_.view.get());
    s.set[p]->setTexture(hit_index_, hit_.texture.get(), Layout::GENERAL, hit_.view.get());
    s.set[p]->setBuffer(stats_index_, stats_.get(), rules::kStatBytes);
    s.set_key[p] = key;
  }
  return true;
}

uint64_t GiPixel::HeldBytes() const {
  uint64_t bytes = 0;
  if (signal_.texture) {
    bytes += uint64_t(image_width_) * image_height_ * 28u;   // signal 8, two depth guides 8, normal 8, hit 4
  }
  bytes += denoise_.HeldBytes();
  for (const Slot& s : slots_) {
    bytes += s.rows ? rules::kBufferAlloc : 0u;
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void GiPixel::Harvest(Slot& s) {
  if (!s.stamps_pending || !s.stamps) {
    return;
  }
  s.stamps_pending = false;
  s.stamps->queryResults();
  const uint64_t* t = s.stamps->getResults();
  if (t[1] >= t[0]) {
    governor_.AddSample(float(double(t[1] - t[0]) / 1.0e6), rules::CellFor(gi::Rung::kStrong, gi::Level::kUltra).cap_ms);
  }
}

void GiPixel::OnSubmitted(uint32_t frame_slot) {
  if (frame_slot >= kFramesInFlight) {
    return;
  }
  Slot& s = slots_[frame_slot];
  s.stamps_pending = s.stamps_written;
  s.stamps_written = false;
}

bool GiPixel::Begin(const Inputs& in) {
  plan_ = {};
  result_ = nullptr;
  frame_ = in.frame;
  if (!ready_ || !in.field || !in.rows || !in.camera || in.frame_slot >= kFramesInFlight) {
    return false;
  }
  in_ = in;
  gi_pixel::Facts facts;
  facts.layout = in.field->layout;
  facts.field_present = in.field->FieldPresent();
  facts.debug = -1;
  facts.raw = in.raw;   // G6: Ray Reconstruction takes the frame and this effect's switch is on (GiApply: TemporalSystem::RawModes().RtGi(); false with the plan not RR)
  // The top level of this frame (the city alone, or with the cars) and the world's rows (the light, the camera, the lamps): both are made before the trigger reaches here (scene.cpp).
  const CarRtView& view = car_scene_->View();
  facts.scene_ok = view.scene_valid && view.set != nullptr;
  const RtWorldFrame::Current world = world_->current();
  const uint32_t needed = rt_world::kWorldValid | rt_world::kWorldCameraKnown | rt_world::kWorldLight;
  facts.world_ok = world_->Valid() && world.rows && (world.decoded.flags & needed) == needed && (world.rows[3] & rt_hit::kFrameShade) != 0;
  facts.hit_data_ok = hit_scene_->ready() && hit_scene_->stats().committed != 0;
  facts.planes_ok = in.depth_slot != gi::kNoSlot && in.normal_slot != gi::kNoSlot && in.albedo_slot != gi::kNoSlot && in.lin_slot != gi::kNoSlot && in.traced[0] != 0 && in.traced[1] != 0;
  facts.pipelines_ready = false;
  facts.resources_ok = true;
  // Decide stops at the first reason that applies: only when everything before the pipelines holds are they asked for (and listed, the denoiser's two stages included), then the images are made.
  facts.pipelines_ready = true;
  gi_pixel::Plan plan = gi_pixel::Decide(facts);
  if (plan.runs()) {
    const bool trace_ready = PipelineReady(in.multisampled ? kVMs : kVPlain);
    const bool spatial_ready = denoise_.SpatialReady();
    const bool temporal_ready = denoise_.TemporalReady();
    facts.pipelines_ready = trace_ready && spatial_ready && temporal_ready;
    plan = gi_pixel::Decide(facts);
  }
  if (plan.runs()) {
    // the size: the traced size, or half of it on each axis where the governor has taken the pass down
    divisor_ = governor_.divisor();
    const uint32_t width = rules::PixelExtent(in.traced[0], divisor_);
    const uint32_t height = rules::PixelExtent(in.traced[1], divisor_);
    facts.resources_ok = width <= 0xFFFFu && height <= 0xFFFFu && EnsureImages(width, height) && EnsureSlot(in.frame_slot);
    plan = gi_pixel::Decide(facts);
  }
  plan_ = plan;
  CountWhy(plan.why);
  if (!plan.runs()) {
    return false;
  }
  Slot& slot = slots_[in.frame_slot];
  Harvest(slot);

  // THE CAMERA and the history: the last run's pose and lens, valid when that run was the previous frame's at this size and the field did not cut.
  const gi_apply::Frame& cam = *in.camera;
  for (int a = 0; a < 3; ++a) {
    pose_.position[a] = double(cam.position[a]);
    pose_.right[a] = double(cam.right[a]);
    pose_.up[a] = double(cam.up[a]);
    pose_.forward[a] = double(cam.forward[a]);
  }
  tan_[0] = cam.tan_x;
  tan_[1] = cam.tan_y;
  const gi_field::MoveReport& move = field_->move();
  const bool cut = move.cut || move.relight;
  history_ok_ = have_prev_ && prev_frame_ + 1 == in.frame && prev_size_[0] == image_width_ && prev_size_[1] == image_height_ && !cut;
  if (!history_ok_) {
    denoise_.ResetHistory();
    history_frames_ = 0;
  }
  params_ = {};
  params_.depth_slot = depth_[parity_].slot;
  params_.normal_slot = normal_.slot;
  params_.width = image_width_;
  params_.height = image_height_;
  params_.divisor = divisor_;
  params_.raw = plan.raw;   // the apply weighs a raw frame's image by RawWeight (its alpha is no history length)
  stats_frame_ = in.frame % kStatsEvery == 0;

  // THE ROWS: the apply's buffer, the pixel rows, the frame's surface lights.
  std::array<larecomp::gpu::GpuLight, rules::kLightsMax> lights = {};
  lights_in_rows_ = lights_ ? rules::PickLights(lights_->Frame(), rules::kLightsMax, lights.data()) : 0u;
  uint32_t words[rules::kBufferAlloc / 4] = {};
  std::memcpy(words, in.rows->data(), sizeof(*in.rows));
  uint32_t* px = words + rules::kRowsOffsetBytes / 4;
  const uint32_t scale = (in.traced[0] != in.render[0] || in.traced[1] != in.render[1]) ? 2u : 1u;
  px[0] = image_width_;
  px[1] = image_height_;
  px[2] = divisor_;
  px[3] = scale;
  px[4] = uint32_t(in.frame);
  px[5] = stats_frame_ ? rules::kFlagStats : 0u;
  px[6] = lights_in_rows_;
  px[7] = in.multisampled ? 1u : 0u;
  const float pixel_angle = in.render[1] > 0 ? 2.0f * cam.tan_y / float(in.render[1]) * float(scale * divisor_) : 0.0f;
  px[8] = std::bit_cast<uint32_t>(rules::kReachM);
  px[9] = std::bit_cast<uint32_t>(in.field->bounce_weight);
  px[10] = std::bit_cast<uint32_t>(rules::kHeadlightGain);
  px[11] = std::bit_cast<uint32_t>(pixel_angle);
  if (lights_in_rows_ != 0) {
    std::memcpy(px + 12, lights.data(), size_t(lights_in_rows_) * sizeof(larecomp::gpu::GpuLight));
  }
  void* mapped = slot.rows->map();
  if (!mapped) {
    plan_.why = gi_pixel::Why::kResources;
    CountWhy(plan_.why);
    return false;
  }
  std::memcpy(mapped, words, rules::kBufferBytes);
  slot.rows->unmap();
  return true;
}

void GiPixel::RecordTrace(plume::RenderCommandList* list) {
  if (!plan_.runs() || !list) {
    return;
  }
  Slot& slot = slots_[in_.frame_slot];
  plume::RenderPipeline* trace = pipelines_[in_.multisampled ? kVMs : kVPlain].get();
  const CarRtView& view = car_scene_->View();
  if (!trace || !view.set) {
    plan_.why = gi_pixel::Why::kPipelines;
    return;
  }
  const plume::RenderTextureBarrier writes[4] = {{signal_.texture.get(), Layout::GENERAL}, {depth_[parity_].texture.get(), Layout::GENERAL}, {normal_.texture.get(), Layout::GENERAL}, {hit_.texture.get(), Layout::GENERAL}};
  const plume::RenderBufferBarrier buffers[1] = {{stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
  list->barriers(Stage::COMPUTE, buffers, 1, writes, 4);
  signal_.layout = normal_.layout = hit_.layout = depth_[parity_].layout = Layout::GENERAL;
  if (stamps_ok_ && slot.stamps) {
    list->resetQueryPool(slot.stamps.get(), 0, 2);
    list->writeTimestamp(slot.stamps.get(), 0);
    slot.stamps_written = true;
  }
  rules::Push push = {};
  push.slots[0] = in_.depth_slot;
  push.slots[1] = in_.normal_slot;
  push.slots[2] = in_.albedo_slot;
  push.slots[3] = in_.lin_slot;
  push.info[2] = in_.sample0;
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), rules::kTextureSet);
  list->setComputeDescriptorSet(heap_->texture_set(), rules::kTextureMsSet);
  list->setComputeDescriptorSet(heap_->sampler_set(), rules::kSamplerSet);
  list->setComputeDescriptorSet(slot.set[parity_].get(), rules::kPassSet);
  list->setComputeDescriptorSet(view.set, rules::kSceneSet);
  list->setPipeline(trace);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(rules::Groups(image_width_), rules::Groups(image_height_), 1);
  ran_frame_ = frame_;
}

void GiPixel::RecordDenoise(plume::RenderCommandList* list) {
  if (!plan_.runs() || !list || ran_frame_ != frame_) {
    return;
  }
  Slot& slot = slots_[in_.frame_slot];
  const uint32_t prev = parity_ ^ 1u;
  if (plan_.denoise) {
    rules::DenoiseView v;
    v.signal_slot = signal_.slot;
    v.depth_slot = depth_[parity_].slot;
    v.normal_slot = normal_.slot;
    v.hit_slot = hit_.slot;
    v.history_depth_slot = depth_[prev].texture ? depth_[prev].slot : depth_[parity_].slot;
    v.width = image_width_;
    v.height = image_height_;
    v.tan_x = tan_[0];
    v.tan_y = tan_[1];
    v.prev_tan_x = have_prev_ ? prev_tan_[0] : tan_[0];
    v.prev_tan_y = have_prev_ ? prev_tan_[1] : tan_[1];
    if (have_prev_) {
      rules::MotionRows(pose_, prev_pose_, v.motion);
    }
    v.length = rules::AssumedLength(history_frames_);
    RtDenoise::Frame stage;
    stage.list = list;
    stage.signal = &signal_;
    stage.reads = {&depth_[parity_], &normal_, &hit_, &depth_[prev], nullptr, nullptr, nullptr, nullptr};
    stage.width = image_width_;
    stage.height = image_height_;
    stage.iterations = rules::CellFor(gi::Rung::kStrong, gi::Level::kUltra).denoise_iterations;
    stage.push = rules::MakePush(v);
    stage.patch = rt_denoise::plain::PatchFor();
    stage.history_valid = history_ok_;
    const HostImage* accumulated = denoise_.RecordTemporal(stage);
    if (accumulated) {
      history_frames_ = denoise_.last_history_read() ? std::min(history_frames_ + 1, 64u) : 0u;
      v.length = rules::AssumedLength(history_frames_);
      stage.signal = const_cast<HostImage*>(accumulated);   // the class only reads it (RtDenoise::Frame holds non-const pointers for the images it moves to the read state)
      stage.push = rules::MakePush(v);
      result_ = denoise_.RecordSpatial(stage);
    }
  } else if (plan_.raw) {
    // G6: a RAW frame (Ray Reconstruction takes it and is the denoiser): neither stage runs. The apply reads the 1 spp signal itself, and the guide pass reads the hit distance image (as the last choice of its
    // hit distance input): both go to the read state of the stages that read them (the pixel stage, the guide pass's compute stage).
    result_ = &signal_;
    const plume::RenderTextureBarrier raw_reads[2] = {{signal_.texture.get(), Layout::SHADER_READ}, {hit_.texture.get(), Layout::SHADER_READ}};
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, raw_reads, 2);
    signal_.layout = hit_.layout = Layout::SHADER_READ;
  }
  // The apply's pixel stage reads the result (the denoiser's last pass made it readable by both stages) and the guides of a reduced image: the guides are moved to the read state of both stages now, after the
  // denoiser's own passes (a later compute-only barrier would take the pixel stage's state away on D3D12).
  const plume::RenderTextureBarrier reads[2] = {{depth_[parity_].texture.get(), Layout::SHADER_READ}, {normal_.texture.get(), Layout::SHADER_READ}};
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, reads, 2);
  depth_[parity_].layout = normal_.layout = Layout::SHADER_READ;
  if (stamps_ok_ && slot.stamps && slot.stamps_written) {
    list->writeTimestamp(slot.stamps.get(), 1);
  }
  // This run is the next frame's history, unless it was a raw one: the denoiser's own history image was not written in it, so the next frame (raw or plain) starts the denoiser again.
  have_prev_ = rules::KeepsHistory(plan_.raw);
  prev_frame_ = in_.frame;
  prev_pose_ = pose_;
  prev_tan_[0] = tan_[0];
  prev_tan_[1] = tan_[1];
  prev_size_[0] = image_width_;
  prev_size_[1] = image_height_;
  parity_ ^= 1u;
  ran_since_present_ = true;
  ++frames_run_;
  frames_raw_ += plan_.raw ? 1u : 0u;
  frames_reduced_ += divisor_ > 1 ? 1u : 0u;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} {}", kPrefix, rules::FirstLine(uint32_t(in_.frame), image_width_, image_height_, divisor_, lights_in_rows_, rules::kReachM));
  }
  if (plan_.raw != raw_logged_) {
    raw_logged_ = plan_.raw;
    REXLOG_INFO("{} frame {}: {} (logged when it changes)", kPrefix, in_.frame, rules::RawLine(plan_.raw));
  }
  // The counters, every interval: copied to the readback buffer (the next frame's first barrier takes the counters back to the storage state).
  if (in_.frame >= next_readback_ && !readback_busy_) {
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(stats_.get(), 0), rules::kStatBytes);
    readback_busy_ = true;
    next_readback_ = in_.frame + kReadbackEveryFrames;
    frames_->RetireAction([this, frame = in_.frame, generation = generation_] { ProcessReadback(frame, generation); });
  }
}

bool GiPixel::Publish(gi::GiFrame* frame) const {
  if (!frame || !result_ || !result_->texture || result_->slot > 0xFFFEu || !plan_.runs() || ran_frame_ != frame_) {
    return false;
  }
  frame->slot[gi::kPixelIrradiance] = result_->slot;
  frame->valid |= gi::ProductBit(gi::kPixelIrradiance);
  frame->pixel = params_;
  return true;
}

// The signal and the hit distance image of a frame the pass ran RAW (Ray Reconstruction took it): the apply reads the signal through Publish's slot, the guide pass the hit image through GiApply::RawHitImage.
GiPixel::RawOutput GiPixel::raw_output() const {
  RawOutput out;
  if (plan_.runs() && plan_.raw && ran_frame_ == frame_) {
    out.valid = true;
    out.frame = frame_;
    out.signal = &signal_;
    out.hit = &hit_;
    out.width = image_width_;
    out.height = image_height_;
  }
  return out;
}

void GiPixel::OnPresent() {
  if (ran_since_present_) {
    idle_frames_ = 0;
  } else if (signal_.texture && ++idle_frames_ >= kReleaseAfterFrames) {
    ReleaseImages();   // ULTRA turned down, the pause menu, the garage: the images go back (made again on the next run)
    idle_frames_ = 0;
  }
  ran_since_present_ = false;
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

void GiPixel::ProcessReadback(uint64_t frame, uint64_t generation) {
  readback_busy_ = false;
  if (generation != generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the counters' readback of frame {} could not be mapped", kPrefix, frame);
    return;
  }
  std::array<uint32_t, rules::kStatWords> words;
  std::memcpy(words.data(), mapped, sizeof(uint32_t) * rules::kStatWords);
  readback_->unmap();
  // The counters are never cleared: the interval's words are the difference to the last readback.
  if (!have_last_words_) {
    last_words_ = words;
    have_last_words_ = true;
    return;
  }
  std::array<uint32_t, rules::kStatWords> delta;
  for (uint32_t i = 0; i < rules::kStatWords; ++i) {
    delta[i] = words[i] - last_words_[i];
  }
  last_words_ = words;
  LogInterval(frame, delta, kReadbackEveryFrames);
}

void GiPixel::LogInterval(uint64_t frame, const std::array<uint32_t, rules::kStatWords>& d, uint32_t frames) {
  const double traced = double(d[rules::kStatPixels]);
  const auto share = [&](uint32_t word) { return traced > 0.0 ? 100.0 * double(d[word]) / traced : 0.0; };
  REXLOG_INFO("{} frame {}: census of every {} th frame in the last {}: {} pixels traced ({:.1f}% hit the city, {:.1f}% a car, {:.1f}% found nothing within {:.0f} m, {} hits without data), {} pixels left alone; the governor {} ({:.2f} ms)",
              kPrefix, frame, kStatsEvery, frames, d[rules::kStatPixels], share(rules::kStatHits), share(rules::kStatCarHits), share(rules::kStatMisses), double(rules::kReachM), d[rules::kStatNoData],
              d[rules::kStatSkipped], governor_.reduced() ? "at half the traced size" : "at the traced size", double(governor_.ewma()));
  if (pass_timing_ && pass_timing_->enabled()) {
    REXLOG_INFO("{} frame {}: timings: rt gi pixel {:.3f} ms, rt gi denoise {:.3f} ms a frame", kPrefix, frame, pass_timing_->LabelMsPerFrame("rt gi pixel"), pass_timing_->LabelMsPerFrame("rt gi denoise"));
  }
}

std::string GiPixel::CensusText() const {
  if (!ready_) {
    return {};
  }
  std::string why;
  for (uint32_t i = 1; i < uint32_t(rules::Why::kCount); ++i) {
    if (why_counts_[i] != 0) {
      why += std::format("{}{} {}", why.empty() ? "" : ", ", rules::WhyName(rules::Why(i)), why_counts_[i]);
    }
  }
  return std::format("; per-pixel pass: {} frames ({} reduced, {} raw), {:.1f} MiB held, governor {}{}; frames without it: {}", frames_run_, frames_reduced_, frames_raw_, double(HeldBytes()) / (1024.0 * 1024.0),
                     governor_.reduced() ? "reduced" : "full", stamps_ok_ ? "" : " (no timestamps)", why.empty() ? std::string("none") : why);
}

}  // namespace mcla::gpu
