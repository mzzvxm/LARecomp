// MCLA GPU renderer -- the ray traced visibility of the top K lights. See rt_lights.h; the rules are rt_lights_rules.h, the trace hlsl/rt_lights_trace_cs.hlsl, the reader hlsl/rt_mask_read.hlsli.

#include "rt_lights.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "car_rt_scene.h"
#include "city_rt_rules.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "lamp_shadows_rules.h"
#include "live_text.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "rt_world_frame.h"
#include "vol_lamp_list.h"

namespace mcla::gpu {

namespace {

namespace rt = larecomp::gpu::rt_lights;
namespace lg = larecomp::gpu;
namespace ll = larecomp::gpu::local_lights;
namespace la = lamp_shadows;

constexpr const char* kPrefix = "[rt lights]";
constexpr plume::RenderBufferFlags kStorageFlags = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS;
constexpr plume::RenderTextureFlags kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
// A copy of the game's lamp list this old is not the list any more (the hook is not running, a menu, a day with the hook idle): the lamp shadows' number.
constexpr std::chrono::milliseconds kListMaxAge{500};

using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;

LiveText& ModeSetting() {
  static LiveText text("mcla_gpu_rt_lights");
  return text;
}
LiveText& ShadowsSetting() {
  static LiveText text("mcla_gpu_rt_shadows");
  return text;
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kRtLights, 0); }

}  // namespace

RtLights::~RtLights() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void RtLights::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene& car_scene, RtWorldFrame& world, RtSceneService& service) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  car_scene_ = &car_scene;
  world_ = &world;
  service_ = &service;
  ready_ = false;
  pipeline_failed_ = false;
  listed_ = false;
  const bool supported = car_scene.ready() && world.initialized() && CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan);
  if (!supported) {
    passes.precache().Note("ray traced light visibility: not on this graphics card or backend (the shadow atlas alone shadows the lights)", 0);
    return;
  }
  if (!MakeLayoutAndShader(car_scene)) {
    passes.precache().Note("ray traced light visibility: a layout or a shader could not be made (the shadow atlas alone shadows the lights)", 0);
    return;
  }
  // THE DENOISER: one RtDenoise instance for each group of three lights (instances 2 to 4; the reflections' is 0 and the per-pixel bounce light's 1), the diffuse compile of the plain consumer's two stages.
  const HostShaderBlob spatial = RtDenoiseDiffuseShader(false);
  const HostShaderBlob temporal = RtDenoiseDiffuseShader(true);
  for (uint32_t g = 0; g < rt::kMaxImages; ++g) {
    denoise_[g].Initialize(device, frames, heap, passes, rt::kFirstDenoiseInstance + g, "rt lights", RtDenoise::Blob{spatial.data, spatial.size, spatial.name},
                           RtDenoise::Blob{temporal.data, temporal.size, temporal.name});
    if (!denoise_[g].available()) {
      passes.precache().Note("ray traced light visibility: the denoiser could not be made (the shadow atlas alone shadows the lights)", 0);
      return;
    }
  }
  stats_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(rt::kStatBytes, kStorageFlags), "rt lights counters");
  readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(rt::kStatBytes), "rt lights counters readback");
  if (!stats_ || !readback_) {
    passes.precache().Note("ray traced light visibility: the counters could not be made (the shadow atlas alone shadows the lights)", 0);
    stats_.reset();
    readback_.reset();
    return;
  }
  ready_ = true;
  passes.precache().Note("ray traced light visibility: made when first wanted (mcla_gpu_rt_lights)", 0);
}

bool RtLights::MakeLayoutAndShader(CarRtScene& scene) {
  // THE PASS SET: the rows, the three signal images, the three guides, the counters (registers unique inside the set: rt_lights_rules.h).
  pass_set_builder_.begin();
  rows_index_ = pass_set_builder_.addByteAddressBuffer(rt::kBindRows);
  signal_index_[0] = pass_set_builder_.addReadWriteTexture(rt::kBindSignal0);
  signal_index_[1] = pass_set_builder_.addReadWriteTexture(rt::kBindSignal1);
  signal_index_[2] = pass_set_builder_.addReadWriteTexture(rt::kBindSignal2);
  depth_index_ = pass_set_builder_.addReadWriteTexture(rt::kBindDepth);
  normal_index_ = pass_set_builder_.addReadWriteTexture(rt::kBindNormal);
  hit_index_ = pass_set_builder_.addReadWriteTexture(rt::kBindHit);
  stats_index_ = pass_set_builder_.addReadWriteByteAddressBuffer(rt::kBindStats);
  pass_set_builder_.end();

  // The trace: the bindless textures (the linear depth image), the pass set, the ray tracing set, the push block.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap_->texture_set_builder());
  const uint32_t pass_set = builder.addDescriptorSet(pass_set_builder_);
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());
  builder.addPushConstant(0, rt::kPushSpace, sizeof(rt::Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != rt::kTextureSet || pass_set != rt::kPassSet || scene_set != rt::kSceneSet) {
    REXLOG_ERROR("{} the trace layout's sets are not in the order the shader declares them", kPrefix);
    return false;
  }
  layout_ = device_->CreatePipelineLayout(builder, "rt lights trace layout");
  if (!layout_) {
    return false;
  }
  const HostShaderBlob blob = RtLightsTraceShader();
  shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  return shader_ != nullptr;
}

void RtLights::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();  // the host builder's functions name this object's layout and shader: stop it first
  }
  vol_lamps::SetWantedBy(vol_lamps::kWantedByRtLights, false);
  for (RtDenoise& d : denoise_) {
    d.Shutdown();
  }
  // The GPU is idle: nothing is retired, the objects just go.
  for (Slot& s : slots_) {
    s = Slot{};
  }
  for (HostImage& s : signal_) {
    s = HostImage{};
  }
  for (HostImage& d : depth_) {
    d = HostImage{};
  }
  normal_ = HostImage{};
  hit_ = HostImage{};
  stats_.reset();
  readback_.reset();
  readback_busy_ = false;
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_ = false;
  wanted_ = false;
  ++generation_;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  car_scene_ = nullptr;
  world_ = nullptr;
  service_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipeline (made in the background the first time the pass is wanted: a frame never waits for a compile)
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> RtLights::MakePipeline() {
  if (!layout_ || !shader_) {
    return nullptr;
  }
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), rt::kGroup, rt::kGroup, 1), "rt lights trace");
}

bool RtLights::PipelineReady() {
  if (pipeline_) {
    return true;
  }
  if (pipeline_failed_ || !passes_) {
    return false;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    listed_ = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({PipelineKey(), "rt lights trace", [this] { return MakePipeline(); }});
    precache.AddLive("ray traced light visibility", std::move(items));
    REXLOG_INFO("{} the pass was wanted: its pipeline is made in the background, and the pass starts from the frame it is done", kPrefix);
  }
  if (!precache.Ready(PipelineKey())) {
    return false;
  }
  pipeline_ = precache.Acquire(PipelineKey(), "rt lights trace", [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the pass's pipeline could not be made: no ray traced light visibility this session (the shadow atlas alone shadows the lights)", kPrefix);
  }
  return pipeline_ != nullptr;
}

// ---------------------------------------------------------------------------
// the images and the sets
// ---------------------------------------------------------------------------

void RtLights::RetireSets() {
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

void RtLights::ReleaseImages() {
  RetireSets();
  if (passes_) {
    for (HostImage& s : signal_) {
      if (s.texture) passes_->ReleaseImage(s);
    }
    for (HostImage& d : depth_) {
      if (d.texture) passes_->ReleaseImage(d);
    }
    if (normal_.texture) passes_->ReleaseImage(normal_);
    if (hit_.texture) passes_->ReleaseImage(hit_);
  }
  ++image_serial_;
  image_width_ = image_height_ = image_groups_ = 0;
  have_prev_ = false;
  history_frames_ = 0;
  for (RtDenoise& d : denoise_) {
    d.ResetHistory();
  }
}

bool RtLights::EnsureImages(uint32_t width, uint32_t height, uint32_t groups) {
  if (signal_[0].texture && image_width_ == width && image_height_ == height && image_groups_ >= groups) {
    return true;
  }
  ReleaseImages();
  bool ok = true;
  for (uint32_t g = 0; g < groups && ok; ++g) {
    ok = passes_->EnsureImage(signal_[g], width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "rt lights signal", kImageFlags) && signal_[g].slot <= 0xFFFEu;
  }
  // the denoiser's guides name their images by 16 bit slots (rt_denoise_rules.h plain::)
  ok = ok && passes_->EnsureImage(depth_[0], width, height, plume::RenderFormat::R32_FLOAT, "rt lights depth 0", kImageFlags) &&
       passes_->EnsureImage(depth_[1], width, height, plume::RenderFormat::R32_FLOAT, "rt lights depth 1", kImageFlags) &&
       passes_->EnsureImage(normal_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "rt lights normal", kImageFlags) &&
       passes_->EnsureImage(hit_, width, height, plume::RenderFormat::R16G16_FLOAT, "rt lights hit", kImageFlags);
  ok = ok && depth_[0].slot <= 0xFFFEu && depth_[1].slot <= 0xFFFEu && normal_.slot <= 0xFFFEu && hit_.slot <= 0xFFFEu;
  if (!ok) {
    ReleaseImages();
    return false;
  }
  image_width_ = width;
  image_height_ = height;
  image_groups_ = groups;
  ++image_serial_;
  REXLOG_INFO("{} the pass's images: {}x{}, {} group(s) of three lights, {:.1f} MiB with the denoisers' own four a group (the signals, two depth guides, the view normal, the hit image)", kPrefix, width, height, groups,
              double(rt::HeldBytes(width, height, groups)) / (1024.0 * 1024.0));
  return true;
}

bool RtLights::EnsureSlot(uint32_t frame_slot) {
  Slot& s = slots_[frame_slot];
  if (!s.rows) {
    s.rows = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(rt::kRowsAlloc, plume::RenderBufferFlag::STORAGE), "rt lights rows");
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
    s.set[p] = device_->CreateDescriptorSet(pass_set_builder_, rt::kSetViews, 0, "rt lights set");  // eight views: D3D12 sizes a set by this count (descriptor_count_test reads it)
    if (!s.set[p]) {
      return false;
    }
    s.set[p]->setBuffer(rows_index_, s.rows.get(), rt::kRowsAlloc);
    for (uint32_t g = 0; g < rt::kMaxImages; ++g) {
      const HostImage& image = g < image_groups_ ? signal_[g] : signal_[0];   // a group this frame does not write is bound to the first image (the shader never writes it)
      s.set[p]->setTexture(signal_index_[g], image.texture.get(), Layout::GENERAL, image.view.get());
    }
    s.set[p]->setTexture(depth_index_, depth_[p].texture.get(), Layout::GENERAL, depth_[p].view.get());
    s.set[p]->setTexture(normal_index_, normal_.texture.get(), Layout::GENERAL, normal_.view.get());
    s.set[p]->setTexture(hit_index_, hit_.texture.get(), Layout::GENERAL, hit_.view.get());
    s.set[p]->setBuffer(stats_index_, stats_.get(), rt::kStatBytes);
    s.set_key[p] = key;
  }
  return true;
}

uint64_t RtLights::HeldBytes() const {
  uint64_t bytes = 0;
  if (signal_[0].texture) {
    bytes += uint64_t(image_width_) * image_height_ * (uint64_t(image_groups_) * 8u + 28u);   // the signals 8 a group, two depth guides 8, the normal 8, the hit image 4
  }
  for (const RtDenoise& d : denoise_) {
    bytes += d.HeldBytes();
  }
  for (const Slot& s : slots_) {
    bytes += s.rows ? rt::kRowsAlloc : 0u;
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void RtLights::BeginFrame(bool pass_runs, bool ultra) {
  namespace st = larecomp::settings;
  ultra_ = ultra;
  mode_ = rt::ParseMode(ModeSetting().Get());
  k_setting_ = int(st::mcla_gpu_rt_lights_k.Get());
  radius_setting_ = st::mcla_gpu_rt_lights_radius.Get();
  rays_ = rt::ClampRays(int(st::mcla_gpu_rt_lights_rays.Get()));
  view_ = rt::ClampView(int(st::mcla_gpu_rt_lights_view.Get()));
  check_ = rt::ClampCheck(int(st::mcla_gpu_rt_lights_check.Get()));
  blue_ = st::mcla_gpu_noise_blue.Get();
  wanted_ = false;
  bool want = false;
  if (ready_ && service_ != nullptr && mode_ != rt::Mode::kOff) {
    // The structures at the level the sun shadows would take (the RT SHADOWS row's: a card with none, or the row off, is only a reason for auto not to run; "on" builds them at HIGH).
    city_rt::Level level = city_rt::LevelForRung(gpu_class::Current(), gpu_class::CurrentRtTier(), city_rt::ParseChoice(ShadowsSetting().Get()));
    const bool rt_shadows_on = level != city_rt::Level::kOff;
    want = pass_runs && rt::Wanted(mode_, rt_shadows_on);
    if (want) {
      if (level == city_rt::Level::kOff) {
        level = city_rt::Level::kHigh;
      }
      rt_scene::Needs needs;
      needs.cars = true;
      needs.city = level;
      needs.city_radius = float(st::mcla_gpu_city_rt_radius.Get());
      service_->Want(rt_scene::Consumer::kRtLights, needs);
      wanted_ = true;
    }
  }
  if (want != lamp_hook_on_) {
    lamp_hook_on_ = want;
    vol_lamps::SetWantedBy(vol_lamps::kWantedByRtLights, want);   // the game's lamp list hook runs while a reader wants the list
  }
}

uint32_t RtLights::AppendLampLights(const float position[3], std::vector<larecomp::gpu::Light>* extras) {
  if (!wanted_ || !ready_ || extras == nullptr) {
    return 0;
  }
  bool stale = false;
  if (vol_lamps::Global().Latest(&list_, &list_seq_, &stale, kListMaxAge)) {
    // a new copy of the list
  }
  if (stale) {
    list_.clear();
    list_seq_ = 0;
  }
  if (list_.empty()) {
    return 0;
  }
  const uint32_t k = rt::ClampK(k_setting_, ultra_);
  la::View view;
  view.have_eye = true;
  for (int i = 0; i < 3; ++i) {
    view.eye[i] = double(position[i]);
  }
  struct Scored {
    double score;
    uint64_t id;
    uint32_t index;
  };
  std::vector<Scored> scored;
  scored.reserve(list_.size());
  for (uint32_t i = 0; i < list_.size(); ++i) {
    const la::Lamp lamp = la::FromGameLamp(list_[i]);
    if (!la::Usable(lamp)) {
      continue;
    }
    const double score = la::Score(lamp, view);
    if (score > 0.0) {
      scored.push_back({score, lamp.id, i});
    }
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score != b.score ? a.score > b.score : a.id < b.id; });
  uint32_t added = 0;
  for (const Scored& s : scored) {
    if (added >= k) {
      break;
    }
    bool present = false;
    for (const lg::Light& l : *extras) {
      present = present || l.id == s.id;
    }
    if (present) {
      continue;   // the atlas's provider handed it over already (with its views)
    }
    const la::Lamp lamp = la::FromGameLamp(list_[s.index]);
    la::LampRef none;   // no views: the lamp has only its ray
    extras->push_back(la::MakePassLight(lamp, none, la::LampMode::kSubtract));
    ++added;
  }
  lamps_handed_ += added;
  return added;
}

void RtLights::Fail(rt::Why why, ll::Selection* selection) {
  why_ = why;
  CountWhy(why);
  masks_ = rt::MaskFrame{};
  result_.fill(nullptr);
  if (selection != nullptr) {
    ll::TrimSelection(selection);   // a lamp handed over for a ray that will not come has no source of visibility: it must not be added on top of the grid's
  }
}

bool RtLights::Plan(const Inputs& in, ll::Selection* selection) {
  masks_ = rt::MaskFrame{};
  result_.fill(nullptr);
  frame_ = in.frame;
  in_ = in;
  light_count_ = 0;
  if (selection == nullptr) {
    return false;
  }
  rt::Facts facts;
  facts.wanted = wanted_ && ready_;
  facts.device_ok = ready_;
  facts.scene_ok = car_scene_ != nullptr && car_scene_->View().scene_valid && car_scene_->View().set != nullptr;
  if (world_ != nullptr && world_->initialized() && world_->Valid()) {
    const RtWorldFrame::Current world = world_->current();
    const uint32_t needed = rt_world::kWorldValid | rt_world::kWorldCameraKnown;
    facts.world_ok = world.decoded.Valid() && (world.decoded.flags & needed) == needed;
  }
  facts.depth_ok = in.lin_slot != lg::kNoLightSlot && in.traced[0] != 0 && in.traced[1] != 0 && in.traced[0] <= 0xFFFFu && in.traced[1] <= 0xFFFFu && in.tan_x > 0.0f && in.tan_y > 0.0f &&
                   in.frame_slot < kFramesInFlight;
  facts.lights = 1;
  facts.pipelines_ready = true;
  facts.resources_ok = true;
  rt::Why why = rt::Decide(facts);
  if (why != rt::Why::kRuns) {
    Fail(why, selection);
    return false;
  }

  // THE PICK: the top K of the lights the pass is going to draw, a light that held a channel scoring more.
  k_ = rt::ClampK(k_setting_, ultra_);
  groups_ = rt::ImagesFor(k_);
  const float eye[3] = {in.position[0], in.position[1], in.position[2]};
  std::array<uint64_t, rt::kMaxLights> held = {};
  uint32_t held_count = 0;
  for (uint64_t id : channels_.id) {
    if (id != 0) {
      held[held_count++] = id;
    }
  }
  const rt::Pick pick = rt::SelectTopK(selection->lights.data(), selection->count, eye, held.data(), held_count, k_);
  facts.lights = pick.count;
  why = rt::Decide(facts);
  if (why != rt::Why::kRuns) {
    Fail(why, selection);
    return false;
  }
  // The pipelines are asked for only now (and listed with the host precache, the denoisers' two stages included), the images made after them: a frame never waits for a compile.
  facts.pipelines_ready = false;
  facts.resources_ok = false;
  if (rt::Decide(facts) == rt::Why::kPipelines) {
    bool ready = PipelineReady();
    for (uint32_t g = 0; g < groups_; ++g) {
      const bool spatial = denoise_[g].SpatialReady();
      const bool temporal = denoise_[g].TemporalReady();
      ready = ready && spatial && temporal;
    }
    facts.pipelines_ready = ready;
  }
  if (rt::Decide(facts) == rt::Why::kResources) {
    facts.resources_ok = EnsureImages(in.traced[0], in.traced[1], groups_) && EnsureSlot(in.frame_slot);
  }
  why = rt::Decide(facts);
  if (why != rt::Why::kRuns) {
    Fail(why, selection);
    return false;
  }
  why_ = rt::Why::kRuns;
  CountWhy(why_);

  // THE CHANNELS: a light keeps its channel while it is picked; a group whose owners changed starts its history again (RecordDenoise).
  const rt::Assigned assigned = rt::AssignChannels(channels_, pick.id.data(), pick.count, groups_ * rt::kChannelsPerImage);
  channels_ = assigned.now;
  pending_reset_ |= assigned.changed_groups;
  light_count_ = pick.count;
  for (uint32_t i = 0; i < pick.count; ++i) {
    const lg::Light& l = selection->lights[pick.index[i]];
    rows_[i] = rt::MakeGpuRtLight(l, assigned.channel_of_wanted[i], radius_setting_);
    classes_[i] = rt::ClassOf(l);
  }
  ll::AnnotateSelection(channels_, selection);
  ll::TrimSelection(selection);

  // THE CAMERA and the history: the last run's pose and lens, valid when that run was the previous frame's at this size and the camera did not cut.
  for (int a = 0; a < 3; ++a) {
    pose_.position[a] = double(in.position[a]);
    pose_.right[a] = double(in.right[a]);
    pose_.up[a] = double(in.up[a]);
    pose_.forward[a] = double(in.forward[a]);
  }
  tan_[0] = in.tan_x;
  tan_[1] = in.tan_y;
  history_ok_ = have_prev_ && prev_frame_ + 1 == in.frame && prev_size_[0] == image_width_ && prev_size_[1] == image_height_ && !rt::CameraCut(prev_pose_.position, pose_.position, prev_pose_.forward, pose_.forward);
  if (!history_ok_) {
    for (RtDenoise& d : denoise_) {
      d.ResetHistory();
    }
    history_frames_ = 0;
  }
  stats_frame_ = in.frame % rt::kStatsEvery == 0;

  // THE ROWS: the header and the lights, into this frame slot's upload buffer.
  Slot& slot = slots_[in.frame_slot];
  void* mapped = slot.rows->map();
  if (mapped == nullptr) {
    Fail(rt::Why::kResources, selection);
    return false;
  }
  rt::GpuRtHeader header{};
  header.width = image_width_;
  header.height = image_height_;
  header.frame = uint32_t(in.frame);
  header.flags = (stats_frame_ ? rt::kFlagStats : 0u) | (check_ == rt::kCheckVisible ? rt::kFlagCheckVisible : 0u) | (check_ == rt::kCheckBlocked ? rt::kFlagCheckBlocked : 0u) | (blue_ ? rt::kFlagBlue : 0u);
  header.light_count = light_count_;
  header.rays = rays_;
  header.tan_x = in.tan_x;
  header.tan_y = in.tan_y;
  header.max_range = rt::kMaxRangeMetres;
  std::array<uint8_t, rt::kRowsAlloc> bytes = {};
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + rt::kRowsHeaderBytes, rows_.data(), size_t(light_count_) * sizeof(rt::GpuRtLight));
  std::memcpy(mapped, bytes.data(), rt::kRowsAlloc);
  slot.rows->unmap();
  return true;
}

void RtLights::RecordTrace(plume::RenderCommandList* list) {
  if (why_ != rt::Why::kRuns || !list || light_count_ == 0 || frame_ != in_.frame) {
    return;
  }
  Slot& slot = slots_[in_.frame_slot];
  const CarRtView& view = car_scene_->View();
  if (!pipeline_ || !view.set) {
    return;
  }
  std::array<plume::RenderTextureBarrier, 6> writes;
  uint32_t write_count = 0;
  for (uint32_t g = 0; g < groups_; ++g) {
    writes[write_count++] = plume::RenderTextureBarrier(signal_[g].texture.get(), Layout::GENERAL);
    signal_[g].layout = Layout::GENERAL;
  }
  writes[write_count++] = plume::RenderTextureBarrier(depth_[parity_].texture.get(), Layout::GENERAL);
  writes[write_count++] = plume::RenderTextureBarrier(normal_.texture.get(), Layout::GENERAL);
  writes[write_count++] = plume::RenderTextureBarrier(hit_.texture.get(), Layout::GENERAL);
  const plume::RenderBufferBarrier buffers[1] = {{stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
  list->barriers(Stage::COMPUTE, buffers, 1, writes.data(), write_count);
  normal_.layout = hit_.layout = depth_[parity_].layout = Layout::GENERAL;
  rt::Push push = {};
  push.slots[0] = in_.lin_slot;
  push.slots[1] = groups_;
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), rt::kTextureSet);
  list->setComputeDescriptorSet(slot.set[parity_].get(), rt::kPassSet);
  list->setComputeDescriptorSet(view.set, rt::kSceneSet);
  list->setPipeline(pipeline_.get());
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(rt::Groups(image_width_), rt::Groups(image_height_), 1);
  ran_frame_ = frame_;
}

void RtLights::RecordDenoise(plume::RenderCommandList* list) {
  if (why_ != rt::Why::kRuns || !list || ran_frame_ != frame_) {
    return;
  }
  const uint32_t prev = parity_ ^ 1u;
  // the groups whose channels changed owner since the last run start their history again
  for (uint32_t g = 0; g < rt::kMaxImages; ++g) {
    if ((pending_reset_ & (1u << g)) != 0u) {
      denoise_[g].ResetHistory();
    }
  }
  pending_reset_ = 0;
  gi_pixel::DenoiseView v;
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
    gi_pixel::MotionRows(pose_, prev_pose_, v.motion);
  }
  bool all = true;
  bool read_history = false;
  for (uint32_t g = 0; g < groups_; ++g) {
    v.signal_slot = signal_[g].slot;
    v.length = gi_pixel::AssumedLength(history_frames_);
    RtDenoise::Frame stage;
    stage.list = list;
    stage.signal = &signal_[g];
    stage.reads = {&depth_[parity_], &normal_, &hit_, &depth_[prev], nullptr, nullptr, nullptr, nullptr};
    stage.width = image_width_;
    stage.height = image_height_;
    stage.iterations = rt::kDenoiseIterations;
    stage.push = gi_pixel::MakePush(v);
    stage.patch = rt_denoise::plain::PatchFor();
    stage.history_valid = history_ok_;
    const HostImage* accumulated = denoise_[g].RecordTemporal(stage);
    if (accumulated == nullptr) {
      all = false;
      continue;
    }
    read_history = read_history || denoise_[g].last_history_read();
    v.length = gi_pixel::AssumedLength(history_frames_ + (denoise_[g].last_history_read() ? 1u : 0u));
    stage.signal = const_cast<HostImage*>(accumulated);   // the class only reads it (RtDenoise::Frame holds non-const pointers for the images it moves to the read state)
    stage.push = gi_pixel::MakePush(v);
    result_[g] = denoise_[g].RecordSpatial(stage);
    all = all && result_[g] != nullptr;
  }
  history_frames_ = read_history ? std::min(history_frames_ + 1, 64u) : 0u;
  // The light pass's pixel stage reads the results (the denoiser's last pass made them readable by both stages), and the next frame's temporal stage the guides: the guides go to the read state of both stages
  // now, after the denoisers' own passes (a later compute-only barrier would take the pixel stage's state away on D3D12).
  const plume::RenderTextureBarrier reads[2] = {{depth_[parity_].texture.get(), Layout::SHADER_READ}, {normal_.texture.get(), Layout::SHADER_READ}};
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, reads, 2);
  depth_[parity_].layout = normal_.layout = Layout::SHADER_READ;
  if (all) {
    masks_.valid = true;
    masks_.images = groups_;
    masks_.width = image_width_;
    masks_.height = image_height_;
    for (uint32_t g = 0; g < groups_; ++g) {
      masks_.slot[g] = result_[g]->slot;
      masks_.raw_slot[g] = signal_[g].slot;
    }
    masks_.lin_slot = in_.lin_slot;
    masks_.view = view_;
  } else {
    masks_ = rt::MaskFrame{};
  }
  // This run is the next frame's history.
  have_prev_ = true;
  prev_frame_ = in_.frame;
  prev_pose_ = pose_;
  prev_tan_[0] = tan_[0];
  prev_tan_[1] = tan_[1];
  prev_size_[0] = image_width_;
  prev_size_[1] = image_height_;
  parity_ ^= 1u;
  ran_since_present_ = true;
  ++frames_run_;
  for (uint32_t i = 0; i < light_count_; ++i) {
    ++class_lights_[size_t(classes_[i])];
  }
  const uint32_t state = (light_count_ << 0) | (rays_ << 4) | (uint32_t(check_) << 8) | (uint32_t(view_) << 10) | (blue_ ? 1u << 12 : 0u) | (groups_ << 13);
  if (!first_logged_ || state != last_state_) {
    first_logged_ = true;
    last_state_ = state;
    REXLOG_INFO("{} frame {}: the pass runs: {}x{} pixels (the occlusion's traced size), K {} ({} group(s) of three), {} light(s) have a ray ({} headlight, {} neon, {} street lamp, {} other), {} ray(s) a light, "
                "disc radius {}, {} noise, RtDenoise diffuse profile, {} iterations; check {}, view {} (logged when it changes)",
                kPrefix, in_.frame, image_width_, image_height_, k_, groups_, light_count_, std::count(classes_.begin(), classes_.begin() + light_count_, rt::Class::kHeadlight),
                std::count(classes_.begin(), classes_.begin() + light_count_, rt::Class::kNeon), std::count(classes_.begin(), classes_.begin() + light_count_, rt::Class::kLamp),
                std::count(classes_.begin(), classes_.begin() + light_count_, rt::Class::kOther), rays_, radius_setting_ > 0.0 ? std::format("{:.2f} m", radius_setting_) : std::string("by class"),
                blue_ ? "blue" : "first versions'", rt::kDenoiseIterations, check_, view_);
  }
  // The counters, every interval: copied to the readback buffer (the next frame's first barrier takes the counters back to the storage state).
  if (in_.frame >= next_readback_ && !readback_busy_) {
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(stats_.get(), 0), rt::kStatBytes);
    readback_busy_ = true;
    next_readback_ = in_.frame + rt::kReadbackEveryFrames;
    frames_->RetireAction([this, frame = in_.frame, generation = generation_] { ProcessReadback(frame, generation); });
  }
}

void RtLights::OnPresent() {
  for (RtDenoise& d : denoise_) {
    d.OnPresent();   // the denoisers' frame boundary: a frame without a stage breaks the history, and the images go back after a spell
  }
  if (ran_since_present_) {
    idle_frames_ = 0;
  } else if (signal_[0].texture && ++idle_frames_ >= rt::kReleaseAfterFrames) {
    ReleaseImages();   // the setting turned off, the pause menu, a day: the images go back (made again on the next run)
    idle_frames_ = 0;
  }
  ran_since_present_ = false;
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

void RtLights::ProcessReadback(uint64_t frame, uint64_t generation) {
  readback_busy_ = false;
  if (generation != generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the counters' readback of frame {} could not be mapped", kPrefix, frame);
    return;
  }
  std::array<uint32_t, rt::kStatWords> words;
  std::memcpy(words.data(), mapped, sizeof(uint32_t) * rt::kStatWords);
  readback_->unmap();
  // The counters are never cleared: the interval's words are the difference to the last readback.
  if (!have_last_words_) {
    last_words_ = words;
    have_last_words_ = true;
    return;
  }
  std::array<uint32_t, rt::kStatWords> delta;
  for (uint32_t i = 0; i < rt::kStatWords; ++i) {
    delta[i] = words[i] - last_words_[i];
  }
  last_words_ = words;
  LogInterval(frame, delta);
}

void RtLights::LogInterval(uint64_t frame, const std::array<uint32_t, rt::kStatWords>& d) {
  const double pixels = double(d[rt::kStatPixels]);
  const double world = double(d[rt::kStatWorld]);
  const double rays = double(d[rt::kStatRays]);
  const double pairs = double(d[rt::kStatPairs]);
  REXLOG_INFO("{} frame {}: census of every {} th frame in the last {}: {} pixels, {:.1f}% are surfaces to judge, {:.1f}% of those are reached by a light, {:.2f} lights reach such a pixel, {} rays, {:.2f}% blocked by the "
              "city and {:.2f}% by a car, {:.2f}% of the (pixel, light) pairs are under half visible; lights held a channel in {} frames, {} lamps handed over for a ray",
              kPrefix, frame, rt::kStatsEvery, rt::kReadbackEveryFrames, d[rt::kStatPixels], pixels > 0.0 ? 100.0 * world / pixels : 0.0,
              world > 0.0 ? 100.0 * double(d[rt::kStatCovered]) / world : 0.0, double(d[rt::kStatCovered]) > 0.0 ? pairs / double(d[rt::kStatCovered]) : 0.0, d[rt::kStatRays],
              rays > 0.0 ? 100.0 * double(d[rt::kStatBlockedCity]) / rays : 0.0, rays > 0.0 ? 100.0 * double(d[rt::kStatBlockedCar]) / rays : 0.0, pairs > 0.0 ? 100.0 * double(d[rt::kStatBlockedPairs]) / pairs : 0.0,
              frames_run_, lamps_handed_);
  if (pass_timing_ && pass_timing_->enabled()) {
    REXLOG_INFO("{} frame {}: timings: rt lights {:.3f} ms, rt lights denoise {:.3f} ms a frame", kPrefix, frame, pass_timing_->LabelMsPerFrame("rt lights"), pass_timing_->LabelMsPerFrame("rt lights denoise"));
  }
}

std::string RtLights::CensusText() const {
  if (!ready_) {
    return {};
  }
  std::string why;
  for (uint32_t i = 1; i < uint32_t(rt::Why::kCount); ++i) {
    if (why_counts_[i] != 0) {
      why += std::format("{}{} {}", why.empty() ? "" : ", ", rt::WhyName(rt::Why(i)), why_counts_[i]);
    }
  }
  return std::format("; ray traced light visibility: {} frames, lights with a channel by class (headlight {}, neon {}, lamp {}, other {}), {} lamps handed over, {:.1f} MiB held; frames without it: {}", frames_run_,
                     class_lights_[0], class_lights_[1], class_lights_[2], class_lights_[3], lamps_handed_, double(HeldBytes()) / (1024.0 * 1024.0), why.empty() ? std::string("none") : why);
}

}  // namespace mcla::gpu
