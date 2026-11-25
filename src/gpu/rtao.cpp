#include "rtao.h"

#include <bit>
#include <cstring>
#include <format>
#include <vector>

#include <rex/logging/log_category.h>

#include "ao_stable.h"
#include "backend.h"
#include "car_rt_scene.h"
#include "city_rt_scene.h"
#include "denoiser_setting.h"
#include "descriptors.h"
#include "device.h"
#include "gpu_class.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "settings/larecomp_settings.h"

#include "rtao_compose.h"
#include "rtao_denoise.h"

namespace mcla::gpu {
namespace {

namespace settings = larecomp::settings;
using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;
namespace Access = plume::RenderBufferAccess;
using Alpha = city_rt_shadow::AlphaMode;
constexpr uint32_t kGroup = 8;
constexpr uint32_t kCounterBytes = 7 * sizeof(uint32_t);
constexpr uint32_t kOutputSet = 4, kSceneSet = 5, kPushSpace = 6;
constexpr uint64_t kProbeInterval = 120, kStatsInterval = 120, kReportInterval = 3600;
constexpr float kGovernorCapMs = 1.2f;
constexpr auto kStorage = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

constexpr uint64_t Key(uint32_t variant) {
  return HostPrecache::Key(HostPrecache::Owner::kRtAo, variant);
}
constexpr const char* Name(uint32_t variant) { return variant == 0 ? "rtao trace" : "rtao trace MS"; }

const char* AlphaText(Alpha alpha) {
  switch (alpha) {
    case Alpha::kExact: return "exact";
    case Alpha::kStochastic: return "stochastic";
    case Alpha::kOpaque: return "opaque";
    case Alpha::kExclude: return "exclude";
  }
  return "unknown";
}

}  // namespace

RtAo::RtAo() = default;
RtAo::~RtAo() = default;

void RtAo::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& timing,
                      CarRtScene& scene, CityRtScene* city, RtSceneService& service, bool listed_at_start) {
  if (!CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops()))
    return;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  timing_ = &timing;
  city_ = city;
  service_ = &service;

  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);
  output_builder_.addReadWriteTexture(1);
  output_builder_.addReadWriteTexture(2);
  output_builder_.addReadWriteTexture(3);
  output_builder_.addReadWriteByteAddressBuffer(4);
  output_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, false);
  for (uint32_t set = 0; set < kOutputSet; ++set)
    builder.addDescriptorSet(heap.texture_set_builder());
  builder.addDescriptorSet(output_builder_);
  builder.addDescriptorSet(scene.SetDesc());
  builder.addPushConstant(0, kPushSpace, sizeof(rtao::RtAoPush), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "rtao trace layout");
  if (!layout_)
    return;
  for (uint32_t variant = 0; variant < 2; ++variant) {
    const HostShaderBlob blob = RtAoTraceShader(variant != 0);
    shader_[variant] = passes.CreateShader(blob.data, blob.size, blob.name);
  }
  if (!shader_[0] || !shader_[1])
    return;
  denoise_ = std::make_unique<RtAoDenoise>();
  compose_ = std::make_unique<RtAoCompose>();
  denoise_->Initialize(device, frames, heap, passes);
  compose_->Initialize(device, frames, heap, passes);
  rtao_setting::NotePassBuilt(available());
  if (listed_at_start && available()) {
    uint32_t added = 0;
    for (uint32_t variant = 0; variant < 2; ++variant) {
      added += passes.precache().Add(Key(variant), Name(variant), [this, variant] {
        return device_->CreateComputePipeline({layout_.get(), shader_[variant].get(), kGroup, kGroup, 1}, Name(variant));
      });
    }
    passes.precache().Note("ray traced ambient occlusion", added);
    listed_ = true;
  }
}

void RtAo::Shutdown() {
  rtao_setting::NotePassBuilt(false);
  if (passes_)
    passes_->precache().Stop();
  if (compose_)
    compose_->Shutdown();
  if (denoise_)
    denoise_->Shutdown();
  ReleaseImages();
  compose_.reset();
  denoise_.reset();
  for (auto& p : pipeline_) p.reset();
  for (auto& s : shader_) s.reset();
  for (auto& q : stamps_) q.reset();
  layout_.reset();
  readback_.reset();
  zeros_.reset();
  stats_.reset();
  passes_ = nullptr;
}

bool RtAo::available() const {
  return layout_ && shader_[0] && shader_[1] && compose_ && denoise_;
}

bool RtAo::PipelineReady(bool ms) {
  const uint32_t variant = ms ? 1u : 0u;
  if (pipeline_[variant])
    return true;
  if (failed_[variant])
    return false;
  HostPrecache& precache = passes_->precache();
  const auto make = [this](uint32_t i) {
    return device_->CreateComputePipeline({layout_.get(), shader_[i].get(), kGroup, kGroup, 1}, Name(i));
  };
  if (!listed_) {
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t i = 0; i < 2; ++i)
      fresh.push_back({Key(i), Name(i), [make, i] { return make(i); }});
    precache.AddLive("ray traced ambient occlusion", std::move(fresh));
    listed_ = true;
  }
  if (!precache.Ready(Key(variant)))
    return false;
  pipeline_[variant] = precache.Acquire(Key(variant), Name(variant), [make, variant] { return make(variant); });
  failed_[variant] = !pipeline_[variant];
  return !failed_[variant];
}

void RtAo::BeginFrame(const rtao_setting::Decision& decision, bool play_view) {
  plan_ = {};
  raw_ = false;
  selected_ = decision.rt;
  play_view_ = play_view;
  wanted_ = selected_ && play_view;
  if (selected_) {
    budget_ = rtao::BudgetFor(decision.rung);
    const auto alpha_text = settings::mcla_gpu_rtao_alpha.Get();
    alpha_ = alpha_text == "auto" && budget_.exact_alpha ? Alpha::kExact : city_rt_shadow::ParseAlphaMode(alpha_text);
    const auto screen_text = settings::mcla_gpu_rtao_screen.Get();
    screen_ = screen_text == "off" ? rtao::Screen::kOff : screen_text == "on" ? rtao::Screen::kOn : rtao::Screen::kAuto;
    const auto interior_text = settings::mcla_gpu_rtao_interior.Get();
    interior_policy_ = interior_text == "screen" ? rtao::Interior::kScreen
                    : interior_text == "rt" ? rtao::Interior::kRt : rtao::Interior::kAuto;
    reach_override_ = float(settings::mcla_gpu_rtao_reach.Get());
    range_override_ = float(settings::mcla_gpu_rtao_range.Get());
    rays_ = uint32_t(settings::mcla_gpu_rtao_rays.Get());
    scale_ = uint32_t(settings::mcla_gpu_rtao_scale.Get());
    check_ = uint32_t(settings::mcla_gpu_rtao_check.Get());
    view_ = settings::mcla_gpu_rtao_view.Get();
    blend_car_ = settings::mcla_gpu_rtao_blend_car.Get() != 0;
    govern_ = decision.rung == gpu_class::RtTier::kStrong && scale_ == 0;
  }
  if (wanted_) {
    rt_scene::Needs needs;
    needs.cars = true;
    needs.city = city_rt::Level::kHigh;
    if (alpha_ == Alpha::kExact) {
      needs.hit = needs.city;
      needs.textures = TextureScope::kAlpha;
    }
    service_->Want(rt_scene::Consumer::kRtAo, needs);
  }
  denoiser::NoteConsumer(rr::Consumer::kRtAo, wanted_);
}

rtao::Plan RtAo::PlanFrame(const Inputs& in, const CarRtView& top_level, bool bounce_light_on) {
  top_level_ = &top_level;
  const bool interior = in.interior && top_level.player_box.valid;
  if (player_mesh_key_ != top_level.player_mesh_key) {
    player_mesh_key_ = top_level.player_mesh_key;
    ++mesh_epoch_;
    cabin_closed_ = false;
    probe_due_ = true;
  }
  probe_due_ = probe_due_ || (interior && (!interior_ || in.frame - last_probe_ >= kProbeInterval));
  interior_ = interior;
  cabin_screen_ = rtao::ScreenCabin(interior_policy_, interior, cabin_closed_);
  rtao::PlanInputs facts;
  facts.wanted = selected_;
  facts.play_view = play_view_;
  facts.cabin_screen = cabin_screen_;
  facts.bounce_light = bounce_light_on;
  facts.screen = screen_;
  facts.raw = raw_;
  facts.budget = budget_;
  facts.rays = rays_;
  facts.render[0] = in.render[0];
  facts.render[1] = in.render[1];
  facts.look_radius = in.look_radius;
  facts.reach_override = reach_override_;
  facts.scale = scale_ == 1 || scale_ == 2 || scale_ == 4 ? scale_ : governor_.reduced() ? 4u : 2u;
  if (facts.wanted && facts.play_view && available()) {
    const bool trace_ready = PipelineReady(in.stencil_ms);
    const bool compose_ready = compose_->available();
    if (raw_)
      facts.scale = rtao_denoise::RawScale();
    const bool filter_ready = raw_ || !rtao_denoise::Enabled() || denoise_->available();
    facts.pipelines = trace_ready && compose_ready && filter_ready;
    facts.top_level = top_level.scene_valid && top_level.set;
    if (facts.pipelines && facts.top_level && city_ && city_->pack_ready()) {
      const float game_hi_radius = float(300.0 * gpu_class::CityLodScale(settings::lod_city_scale.Get()));
      facts.range = std::min({rtao::kMaxRange, city_->SafeRange(game_hi_radius), in.fade_end});
      if (range_override_ > 0.0f)
        facts.range = std::min(facts.range, range_override_);
      facts.city = city_->stats().instances > 0 && facts.range > 0.0f;
      if (facts.city) {
        facts.images = true;
        const rtao::Plan size = rtao::Decide(facts);
        facts.images = EnsureImages(size.traced[0], size.traced[1]);
      }
    }
  }
  plan_ = rtao::Decide(facts);
  if (selected_ && !plan_.on)
    NoteFallback(in.frame, plan_.why);
  return plan_;
}

bool RtAo::EnsureImages(uint32_t width, uint32_t height) {
  using Format = plume::RenderFormat;
  if (output_[0] && output_[1] && signal_.width == width && signal_.height == height)
    return true;
  ReleaseImages();
  if (!stats_) {
    auto stats = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(kCounterBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS),
                                       "rtao counters");
    auto zeros = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(kCounterBytes), "rtao counter reset");
    auto readback = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kCounterBytes), "rtao counter readback");
    if (!stats || !zeros || !readback)
      return false;
    void* mapped = zeros->map();
    if (!mapped)
      return false;
    std::memset(mapped, 0, kCounterBytes);
    zeros->unmap();
    stats_ = std::move(stats);
    zeros_ = std::move(zeros);
    readback_ = std::move(readback);
  }
  const bool images = passes_->EnsureImage(signal_, width, height, Format::R16G16B16A16_FLOAT, "rtao signal", kStorage) &&
                      passes_->EnsureImage(depth_[0], width, height, Format::R32_FLOAT, "rtao depth 0", kStorage) &&
                      passes_->EnsureImage(depth_[1], width, height, Format::R32_FLOAT, "rtao depth 1", kStorage) &&
                      passes_->EnsureImage(normal_, width, height, Format::R16G16B16A16_FLOAT, "rtao normal", kStorage) &&
                      passes_->EnsureImage(id_share_, width, height, Format::R16G16_FLOAT, "rtao ID and share", kStorage);
  if (!images) {
    ReleaseImages();
    return false;
  }
  for (uint32_t parity = 0; parity < 2; ++parity) {
    output_[parity] = device_->CreateDescriptorSet(output_builder_, 5, 0, "rtao trace output");
    if (!output_[parity]) {
      ReleaseImages();
      return false;
    }
    output_[parity]->setTexture(0, signal_.texture.get(), Layout::GENERAL, signal_.view.get());
    output_[parity]->setTexture(1, depth_[parity].texture.get(), Layout::GENERAL, depth_[parity].view.get());
    output_[parity]->setTexture(2, normal_.texture.get(), Layout::GENERAL, normal_.view.get());
    output_[parity]->setTexture(3, id_share_.texture.get(), Layout::GENERAL, id_share_.view.get());
    output_[parity]->setBuffer(4, stats_.get(), kCounterBytes);
  }
  return true;
}

void RtAo::ReleaseImages() {
  if (!passes_)
    return;
  for (auto& set : output_) frames_->Retire(std::move(set));
  passes_->ReleaseImage(signal_);
  for (auto& depth : depth_) passes_->ReleaseImage(depth);
  passes_->ReleaseImage(normal_);
  passes_->ReleaseImage(id_share_);
  previous_ours_ = false;
}

bool RtAo::RecordTrace(plume::RenderCommandList* list, const Inputs& in) {
  if (!plan_.on || !list || !in.depth || !in.depth->texture)
    return false;
  frame_ = in.frame;
  tan_[0] = in.tan_x;
  tan_[1] = in.tan_y;
  depth_full_ = in.depth;
  normal_slot_ = in.normal_slot;
  stencil_slot_ = in.stencil_slot;
  stencil_sample_ = in.stencil_sample;
  stencil_ms_ = in.stencil_ms;
  render_[0] = in.render[0];
  render_[1] = in.render[1];
  parity_ = uint32_t(frame_ & 1u);
  const bool probe = !readback_busy_ && interior_ && probe_due_;
  const bool sample = !readback_busy_ && passes_run_ >= next_sample_;
  if (sample || probe) {
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), Access::WRITE));
    list->copyBufferRegion({stats_.get(), 0}, {zeros_.get(), 0}, kCounterBytes);
  }
  const plume::RenderTextureBarrier reads[] = {{in.depth->texture.get(), Layout::SHADER_READ}};
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, reads, 1);
  const plume::RenderTextureBarrier writes[] = {{signal_.texture.get(), Layout::GENERAL}, {depth_[parity_].texture.get(), Layout::GENERAL},
                                                {normal_.texture.get(), Layout::GENERAL}, {id_share_.texture.get(), Layout::GENERAL}};
  const plume::RenderBufferBarrier counters(stats_.get(), Access::READ | Access::WRITE);
  list->barriers(Stage::COMPUTE, &counters, 1, writes, 4);
  const auto& box = top_level_->player_box;
  rtao::RtAoPush push = {
      .slots = {in.depth->slot, in.normal_slot,
                in.stencil_slot == kNoSlot ? kNoSlot : in.stencil_slot | (in.stencil_sample << 24), plan_.rays},
      .sizes = {plan_.traced[0], plan_.traced[1], in.depth->width, in.depth->height},
      .trace = {uint32_t(in.noise_index & 0xFFFFF), rtao::kRayMask, in.noise_code, top_level_->box_row | (top_level_->box_count << 24)},
      .lens = {in.tan_x, in.tan_y, plan_.reach, plan_.range},
      .look = {in.look_power, in.look_floor, rtao::CarReach(plan_.reach, in.car_reach), rtao::kCabinPad},
      .box_min = {1.0f, 1.0f, 1.0f, std::bit_cast<float>(uint32_t(in.noise_index))},
      .box_max = {0.0f, 0.0f, 0.0f, cabin_screen_ ? 1.0f : 0.0f},
  };
  if (box.valid) {
    std::copy_n(box.min, 3, push.box_min);
    std::copy_n(box.max, 3, push.box_max);
  }
  push.slots[3] |= uint32_t(alpha_) << rtao::kFlagAlphaShift;
  push.slots[3] |= city_rt_shadow::PackAlphaReference(city_rt_shadow::kDefaultAlphaReference) << rtao::kFlagAlphaRefShift;
  push.slots[3] |= (in.stencil_ms ? rtao::kFlagMs : 0u) | (plan_.raw ? rtao::kFlagRaw : 0u) |
                   (check_ == 1 ? rtao::kFlagCheckOpen : 0u) | (check_ == 2 ? rtao::kFlagCheckClosed : 0u) |
                   (interior_ ? rtao::kFlagInterior : 0u) | ((in.noise_code & ao_stable::kBlueNoiseFlag) ? rtao::kFlagBlue : 0u) |
                   (in.depth->width != in.render[0] || in.depth->height != in.render[1] ? rtao::kFlagDepthHalf : 0u) |
                   (probe ? rtao::kFlagProbe : 0u) | (sample ? rtao::kFlagStats : 0u);
  timing_->Mark(list, "rtao trace");
  if (govern_ && !plan_.raw && backend::TimestampsSupported(device_->device())) {
    auto& stamp = stamps_[frames_->slot()];
    if (!stamp)
      stamp = device_->CreateQueryPool(2, "rtao timestamps");
    if (stamp) {
      pending_timing_ = stamp.get();
      list->resetQueryPool(stamp.get(), 0, 2);
      list->writeTimestamp(stamp.get(), 0);
    }
  }
  // CHECK: both backends' stencil plane views and typed UAV barriers in a play frame.
  list->setComputePipelineLayout(layout_.get());
  for (uint32_t set = 0; set < kOutputSet; ++set)
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  list->setComputeDescriptorSet(output_[parity_].get(), kOutputSet);
  list->setComputeDescriptorSet(top_level_->set, kSceneSet);
  list->setPipeline(pipeline_[in.stencil_ms].get());
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((signal_.width + kGroup - 1) / kGroup, (signal_.height + kGroup - 1) / kGroup, 1);
  const plume::RenderTextureBarrier outputs[] = {{signal_.texture.get(), Layout::SHADER_READ}, {depth_[parity_].texture.get(), Layout::SHADER_READ},
                                                 {normal_.texture.get(), Layout::SHADER_READ}, {id_share_.texture.get(), Layout::SHADER_READ}};
  list->barriers(Stage::COMPUTE, nullptr, 0, outputs, 4);
  signal_.layout = depth_[parity_].layout = normal_.layout = id_share_.layout = Layout::SHADER_READ;
  if (sample || probe) {
    list->barriers(Stage::COPY, plume::RenderBufferBarrier(stats_.get(), Access::READ));
    list->copyBufferRegion({readback_.get(), 0}, {stats_.get(), 0}, kCounterBytes);
    frames_->RetireAction([this, frame = in.frame, epoch = mesh_epoch_, probe] { ReadStats(frame, epoch, probe); });
    readback_busy_ = true;
    if (sample)
      next_sample_ = passes_run_ + kStatsInterval;
    if (probe) {
      last_probe_ = in.frame;
      probe_due_ = false;
    }
  }
  recorded_ = true;
  filtered_ = &signal_;
  ++passes_run_;
  // Streaming can move the range every frame. A change to the trace's configuration is what matters here.
  const std::array<uint32_t, 6> configuration = {signal_.width, signal_.height, plan_.rays,
                                                  std::bit_cast<uint32_t>(plan_.reach), uint32_t(alpha_), plan_.raw};
  if (last_state_ != configuration) {
    REXLOG_INFO("[rtao] {}: frame {}, traced {}x{} of render {}x{}, {} rays a pixel, reach {:.2f} m, range {:.1f} m, mask 0x{:X}, alpha {}, "
                "city level {} (the class's ring), filter {}, about {:.3f} million rays",
                passes_run_ == 1 ? "the first ray traced pass" : "trace changed", in.frame,
                signal_.width, signal_.height, in.render[0], in.render[1], plan_.rays, plan_.reach, plan_.range,
                rtao::kRayMask, AlphaText(alpha_), city_rt::LevelName(city_rt::Level::kHigh),
                plan_.raw ? "ray reconstruction raw" : "ours",
                rtao::EstimatedRays(signal_.width, signal_.height, plan_.rays, 0.0) / 1.0e6);
    last_state_ = configuration;
  }
  return true;
}

bool RtAo::RecordFilter(plume::RenderCommandList* list, const FilterInputs& in) {
  if (!recorded_)
    return false;
  const bool keep = rtao::KeepHistory(previous_ours_, previous_frame_ + 1 == frame_,
                                     previous_size_[0] == signal_.width && previous_size_[1] == signal_.height,
                                     in.camera.have_motion || in.vehicles.ids != 0, in.cut);
  if (!keep || plan_.raw || !in.ours)
    denoise_->ResetHistory();
  previous_ours_ = false;
  if (!plan_.raw && in.ours) {
    RtAoDenoise::Frame frame = {
        .list = list, .signal = &signal_, .depth = &depth_[parity_], .depth_prev = &depth_[parity_ ^ 1u],
        .normal = &normal_, .id_share = &id_share_, .traced = {signal_.width, signal_.height},
        .tan_x = tan_[0], .tan_y = tan_[1], .prev_tan_x = previous_tan_[0], .prev_tan_y = previous_tan_[1],
        .vehicle_table_slot = in.vehicles.slot, .vehicle_ids = in.vehicles.ids, .history_valid = keep, .frame = frame_,
    };
    if (in.camera.have_motion && !in.camera.motion_still)
      std::copy_n(in.camera.motion, 12, frame.motion);
    // Everything recorded from here to the closing mark is the denoiser's: the temporal and the spatial passes (a raw frame has no such span).
    timing_->Mark(list, "rtao denoise");
    if (const HostImage* filtered = denoise_->Record(frame)) {
      filtered_ = filtered;
      previous_ours_ = true;
    }
  }
  previous_frame_ = frame_;
  std::copy_n(tan_, 2, previous_tan_);
  std::copy_n(plan_.traced, 2, previous_size_);
  FinishTraceTiming(list);
  CloseSpan(list);
  return previous_ours_;
}

void RtAo::FinishTraceTiming(plume::RenderCommandList* list) {
  if (!pending_timing_)
    return;
  auto* pool = pending_timing_;
  pending_timing_ = nullptr;
  list->writeTimestamp(pool, 1);
  frames_->RetireAction([this, pool, reduced = governor_.reduced()] {
    if (!govern_ || reduced != governor_.reduced())
      return;
    pool->queryResults();
    const uint64_t* ticks = pool->getResults();
    if (ticks[1] >= ticks[0])
      governor_.AddSample(float(double(ticks[1] - ticks[0]) / 1.0e6), kGovernorCapMs);
  });
}

uint32_t RtAo::RecordCompose(plume::RenderCommandList* list, const ComposeInputs& in) {
  if (!recorded_)
    return kNoSlot;
  timing_->Mark(list, "rtao compose");
  const RtAoCompose::Frame frame = {
      .list = list, .filtered = filtered_, .depth_guide = &depth_[parity_], .depth_full = depth_full_,
      .normal_slot = normal_slot_, .screen_term_slot = check_ == 1 ? kNoSlot : in.screen_term_slot,
      .bounce_slot = in.bounce_slot, .traced = {signal_.width, signal_.height}, .plan_ao = {in.plan_ao[0], in.plan_ao[1]},
      .power = in.power, .floor_ = in.floor, .blend_screen = screen_ == rtao::Screen::kOn && check_ != 1, .view = in.view,
  };
  // The compose's optional inputs: the trace's own raw signal (coverage, the debug views), the vehicle IDs of the traced pixels, the depth target's stencil plane (the full size picture's
  // vehicle IDs, so a car's edge takes only the car's taps) and the car pass's occlusion image of the A/B arm.
  const RtAoCompose::Guides guides = {
      .raw = &signal_, .id_share = &id_share_, .stencil_slot = stencil_slot_, .stencil_sample = stencil_sample_, .stencil_ms = stencil_ms_,
      .render = {render_[0], render_[1]}, .car_term_slot = in.car_term_slot,
  };
  const uint32_t slot = compose_->Record(frame, guides);
  CloseSpan(list);
  if (slot == kNoSlot) {
    // The compose could not make its term image (or an input did not match): the AO's own term stands, and the Ultra frame is recorded after it (ao.cpp).
    NoteFallback(frame_, rtao::Fallback::kImagesFailed);
  }
  return slot;
}

// The one closing mark of the RTAO span: what is recorded after it is not "rtao trace", "rtao denoise" or "rtao compose" (frame_order_test holds the three marks and this one to the table).
void RtAo::CloseSpan(plume::RenderCommandList* list) {
  timing_->Mark(list, "after rtao");
}

void RtAo::ReadStats(uint64_t frame, uint64_t epoch, bool probe) {
  readback_busy_ = false;
  const void* mapped = readback_->map();
  if (!mapped) {
    probe_due_ = probe_due_ || probe;
    return;
  }
  uint32_t data[7];
  std::memcpy(data, mapped, sizeof(data));
  readback_->unmap();
  for (uint32_t i = 0; i < counters_.size(); ++i)
    counters_[i] += data[i];
  if (probe && epoch == mesh_epoch_) {
    // CHECK: confirm the hit shares for the actual cockpit shells on both backends.
    cabin_closed_ = rtao::CabinClosed(data[5], data[4]);
    closed_probes_ += cabin_closed_;
    open_probes_ += !cabin_closed_;
    REXLOG_INFO("[rtao] frame {}: cabin probe {}, {} of {} rays hit", frame, cabin_closed_ ? "closed" : "not closed", data[5], data[4]);
  }
}

void RtAo::NoteFallback(uint64_t frame, rtao::Fallback why) {
  if (++fallback_[size_t(why)] == 1)
    REXLOG_INFO("[rtao] frame {}: back to the screen-space term: {}", frame, rtao::FallbackText(why));
}

const HostImage* RtAo::ComposedImage(uint32_t slot) const {
  return compose_ ? compose_->ImageAt(slot) : nullptr;
}

void RtAo::OnPresent() {
  if (denoise_)
    denoise_->OnPresent();
  if (recorded_) {
    idle_ = 0;
    if (passes_run_ % kReportInterval == 0) {
      REXLOG_INFO("[rtao] frame {}: {} passes, sampled {} rays, {} hits, {} alpha candidates, {} texture reads, {} uncovered pixels; "
                  "cabin probes {} closed, {} open; fallback counts play {}, pipeline {}, top level {}, city {}, images {}; "
                  "history length mean {:.2f}, governor {} ({:.3f} ms)",
                  frame_, passes_run_, counters_[0], counters_[1], counters_[2], counters_[3], counters_[6], closed_probes_, open_probes_,
                  fallback_[2], fallback_[3], fallback_[4], fallback_[5], fallback_[6], denoise_->LastMeanLength(),
                  governor_.reduced() ? "reduced" : "full", governor_.ewma());
      counters_.fill(0);
    }
  } else if (signal_.texture && ++idle_ >= rtao::kReleaseAfterFrames) {
    ReleaseImages();
    compose_->ReleaseImage();
    governor_.Reset();
  }
  recorded_ = false;
  filtered_ = depth_full_ = nullptr;
}

uint64_t RtAo::HeldBytes() const {
  return uint64_t(signal_.width) * signal_.height * (rtao::kBytesPerTracedPixel - 32) +
         (denoise_ ? denoise_->HeldBytes() : 0) + (compose_ ? compose_->HeldBytes() : 0);
}

std::string RtAo::CensusText() const {
  if (passes_run_ == 0)
    return {};
  return std::format("; rtao: {} passes, {} closed cabin probes, {} open, {:.1f} MiB held",
                     passes_run_, closed_probes_, open_probes_, double(HeldBytes()) / (1024.0 * 1024.0));
}

}  // namespace mcla::gpu
