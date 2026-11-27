// MCLA GPU renderer -- the probe field's owner. See gi_field.h.

#include "gi_field.h"
#include "gfx_preset_apply.h"
#include "gi_setting.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

namespace rules = gi_field;

constexpr const char* kPrefix = "[gi field]";
constexpr plume::RenderTextureFlags kTextureFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
constexpr plume::RenderFormat kIrradianceFormat = plume::RenderFormat::R16G16B16A16_FLOAT;  // gi::kIrradianceFormat
constexpr plume::RenderFormat kMomentsFormat = plume::RenderFormat::R16G16_FLOAT;           // gi::kMomentsFormat
constexpr plume::RenderFormat kWeightFormat = plume::RenderFormat::R8_UNORM;                // gi::kWeightFormat
static_assert(gi::TexelBytes(gi::kIrradianceFormat) == 8 && gi::TexelBytes(gi::kMomentsFormat) == 4 && gi::TexelBytes(gi::kWeightFormat) == 1, "the plume formats above are gi_frame.h's");
// The upload buffers' size: 320 bytes of rows and 400 of reset list, each rounded up to 512.
constexpr uint64_t kUploadBytes = 512;
static_assert(gi::kGpuFrameBytes <= kUploadBytes && rules::kResetListBytes <= kUploadBytes, "the rows and the reset list fit their upload buffers");
// A field that has nothing to show (the setting is off) is given back after this many frames (2 s at 60); one that idles for a long time (the pause menu, the garage) after this many (a minute).
constexpr uint32_t kReleaseAfterOffFrames = 120;
constexpr uint32_t kReleaseAfterIdleFrames = 3600;
// Retry the selected layout at this interval after an allocation failure.
constexpr uint64_t kRefitFrames = 30;
// Under this the structures stand too near for the field to mean anything (the same line city_rt_shadow.cpp draws).
constexpr float kMinSafeRangeM = 8.0f;
constexpr uint64_t kIntervalFrames = 600;

constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);

double Mib(uint64_t bytes) { return double(bytes) / double(rules::kMiB); }

}  // namespace

GiField::~GiField() { Shutdown(); }

const char* GiField::VariantName(uint32_t variant) { return variant == kVReset ? "gi field reset" : "gi field weights"; }

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void GiField::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, bool listed_at_start) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  ready_ = false;
  listed_ = false;
  shaders_failed_ = false;
  pipeline_failed_ = {};
  if (!MakeLayoutsAndShaders()) {
    passes.precache().Note("ray traced lighting's probe field: the layout or a shader could not be made (no field this session)", 0);
    return;
  }
  ready_ = true;
  if (listed_at_start) {
    uint32_t added = 0;
    for (uint32_t v = 0; v < kVariantCount; ++v) {
      added += passes.precache().Add(HostPrecache::Key(HostPrecache::kGiField, v), VariantName(v), [this, v] { return MakePipeline(v); }) ? 1u : 0u;
    }
    listed_ = true;
    passes.precache().Note("ray traced lighting's probe field: on where the setting says (auto: a strong ray tracing card)", added);
  } else {
    passes.precache().Note("ray traced lighting's probe field: made when first wanted", 0);
  }
}

bool GiField::MakeLayoutsAndShaders() {
  // The compute passes' set (space 0): the frame's rows, the reset list, the three textures and the state buffer. The binding numbers are unique inside the set (gi_field_rules.h).
  field_set_builder_.begin();
  field_rows_index_ = field_set_builder_.addByteAddressBuffer(rules::kBindRows);
  field_list_index_ = field_set_builder_.addByteAddressBuffer(rules::kBindResetList);
  field_irradiance_index_ = field_set_builder_.addReadWriteTexture(rules::kBindIrradiance);
  field_moments_index_ = field_set_builder_.addReadWriteTexture(rules::kBindMoments);
  field_weight_index_ = field_set_builder_.addReadWriteTexture(rules::kBindWeight);
  field_state_index_ = field_set_builder_.addReadWriteByteAddressBuffer(rules::kBindState);
  field_set_builder_.end();
  // The view's set (space 2): the rows and the state buffer, read only.
  view_set_builder_.begin();
  view_rows_index_ = view_set_builder_.addByteAddressBuffer(rules::kViewBindRows);
  view_state_index_ = view_set_builder_.addByteAddressBuffer(rules::kViewBindState);
  view_set_builder_.end();

  plume::RenderPipelineLayoutBuilder compute;
  compute.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  compute.addDescriptorSet(field_set_builder_);  // set 0, space 0
  compute.end();
  compute_layout_ = device_->CreatePipelineLayout(compute, "gi field compute layout");

  plume::RenderPipelineLayoutBuilder view;
  view.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  view.addDescriptorSet(heap_->texture_set_builder());  // set 0, space 0: Texture2D<float4>[]
  view.addDescriptorSet(heap_->sampler_set_builder());  // set 1, space 1: SamplerState[]
  view.addDescriptorSet(view_set_builder_);             // set 2, space 2
  view.addPushConstant(0, rules::kViewPushSpace, sizeof(ViewPush), plume::RenderShaderStageFlag::PIXEL);
  view.end();
  view_layout_ = device_->CreatePipelineLayout(view, "gi field view layout");

  const HostShaderBlob reset = GiFieldResetShader();
  const HostShaderBlob weights = GiFieldWeightsShader();
  const HostShaderBlob viewps = GiFieldViewShader();
  reset_shader_ = device_->CreateShader(reset.data, reset.size, "main", reset.name);
  weights_shader_ = device_->CreateShader(weights.data, weights.size, "main", weights.name);
  view_shader_ = device_->CreateShader(viewps.data, viewps.size, "main", viewps.name);
  if (!compute_layout_ || !reset_shader_ || !weights_shader_) {
    REXLOG_ERROR("{} the compute layout or a shader could not be made: no probe field this session", kPrefix);
    reset_shader_.reset();
    weights_shader_.reset();
    view_shader_.reset();
    compute_layout_.reset();
    view_layout_.reset();
    shaders_failed_ = true;
    return false;
  }
  if (!view_layout_ || !view_shader_) {
    // The field itself does not need the view: it is a debug tool.
    REXLOG_WARN("{} the debug view's layout or shader could not be made: no debug view this session", kPrefix);
    view_failed_ = true;
  }
  return true;
}

void GiField::Shutdown() {
  gi_setting::NoteResourcePressure(false);
  if (passes_) {
    passes_->precache().Stop();  // the host builder's functions name this object's layout and shaders: stop it first
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (SunShafts::Shutdown, Volumetrics::Shutdown).
  irradiance_ = HostImage{};
  moments_ = HostImage{};
  weight_ = HostImage{};
  state_buffer_.reset();
  for (Slot& s : slots_) {
    s = Slot{};
  }
  view_pipelines_.clear();
  for (auto& p : pipelines_) {
    p.reset();
  }
  reset_shader_.reset();
  weights_shader_.reset();
  view_shader_.reset();
  compute_layout_.reset();
  view_layout_.reset();
  state_.Stop();
  resources_made_ = false;
  active_ = usable_ = update_ready_ = published_ = false;
  published_frame_ = gi::GiFrame{};
  update_frame_ = gi::GiFrame{};
  ready_ = false;
  ++generation_;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> GiField::MakePipeline(uint32_t variant) {
  const char* name = VariantName(variant);
  if (variant == kVReset) {
    return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(compute_layout_.get(), reset_shader_.get(), rules::kResetGroupSize, rules::kResetGroupSize, 1), name);
  }
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(compute_layout_.get(), weights_shader_.get(), rules::kWeightsGroupSize, 1, 1), name);
}

plume::RenderPipeline* GiField::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  pipelines_[variant] = passes_->precache().Acquire(HostPrecache::Key(HostPrecache::kGiField, variant), VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no probe field this session", kPrefix, VariantName(variant));
  }
  return pipelines_[variant].get();
}

// True once both compute pipelines are made (listed in the background when the setting was off at the start); false while they are being made or when one failed.
bool GiField::PipelinesReady() {
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t v = 0; v < kVariantCount; ++v) {
      fresh.push_back({HostPrecache::Key(HostPrecache::kGiField, v), VariantName(v), [this, v] { return MakePipeline(v); }});
    }
    listed_ = true;
    precache.AddLive("ray traced lighting's probe field", std::move(fresh));
    REXLOG_INFO("{} the field was off at the start: its two compute pipelines are made in the background, and it starts from the frame they are done", kPrefix);
  }
  for (uint32_t v = 0; v < kVariantCount; ++v) {
    if (pipeline_failed_[v]) {
      return false;
    }
    if (!pipelines_[v] && !precache.Ready(HostPrecache::Key(HostPrecache::kGiField, v))) {
      return false;
    }
  }
  return Pipeline(kVReset) != nullptr && Pipeline(kVWeights) != nullptr;
}

plume::RenderPipeline* GiField::ViewPipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (ViewPipeline& v : view_pipelines_) {
    if (v.format == format && v.samples == samples) {
      return v.pipeline.get();
    }
  }
  if (view_failed_ || !view_layout_ || !view_shader_) {
    return nullptr;
  }
  // Made here, when the view is first drawn (a debug tool: one compile, once, for each colour target format and sample count).
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
  blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  ViewPipeline v;
  v.format = format;
  v.samples = samples;
  v.pipeline = passes_->CreatePipeline(*view_layout_, *view_shader_, format, blend, samples, "gi field view");
  if (!v.pipeline) {
    view_failed_ = true;
    REXLOG_ERROR("{} the debug view's pipeline could not be made: no debug view this session", kPrefix);
    return nullptr;
  }
  plume::RenderPipeline* raw = v.pipeline.get();
  view_pipelines_.push_back(std::move(v));
  return raw;
}

// ---------------------------------------------------------------------------
// the fit and the textures
// ---------------------------------------------------------------------------

bool GiField::MakeResources(const rules::Fit& fit, const double camera[3]) {
  ReleaseResources(nullptr);
  const gi::Budget& b = fit.budget;
  const uint32_t aw = gi::AtlasWidth(b), ah = gi::AtlasHeight(b), ww = gi::WeightWidth(b), wh = gi::WeightHeight(b);
  bool ok = passes_->EnsureImage(irradiance_, aw, ah, kIrradianceFormat, "gi irradiance atlas", kTextureFlags) &&
            passes_->EnsureImage(moments_, aw, ah, kMomentsFormat, "gi moments atlas", kTextureFlags) &&
            passes_->EnsureImage(weight_, ww, wh, kWeightFormat, "gi probe weights", kTextureFlags);
  if (ok) {
    state_bytes_ = rules::StateBytes(rules::TableOf(b));
    state_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(state_bytes_, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "gi probe state");
    ok = state_buffer_ != nullptr;
  }
  if (!ok) {
    ReleaseResources(nullptr);
    ++resources_failed_;
    return false;
  }
  fit_ = fit;
  resources_made_ = true;
  ++generation_;
  state_.Start(b, camera);
  fade_.Reset();
  update_ready_ = false;
  REXLOG_INFO("{} probe field {} of {}: {} cascades of {} probes, {} m apart, atlases {}x{} (RGBA16F and RG16F) and a weight map {}x{}, state {} KiB, {:.1f} MiB held{}; the update, the apply and the "
              "per-pixel pass will add about {:.1f} MiB",
              kPrefix, rules::StepName(fit.step), gi::LayoutName(fit.wanted), b.cascade_count, b.cascade[0].dims.Probes(), b.cascade[0].spacing_m, aw, ah, ww, wh, state_bytes_ / 1024,
              Mib(HeldBytes()), fit.Reduced() ? " (less than the setting asks for: video memory is short; asked again later)" : "",
              Mib(fit.memory.update_reserve + fit.memory.apply_reserve + fit.memory.pixel_reserve));
  return true;
}

void GiField::RetireSets() {
  for (Slot& s : slots_) {
    if (frames_) {
      frames_->Retire(std::move(s.field_set));
      frames_->Retire(std::move(s.view_set));
    }
    s.field_set.reset();
    s.view_set.reset();
    s.sets_generation = 0;
  }
}

void GiField::ReleaseResources(const char* why) {
  if (!passes_) {
    return;
  }
  const bool had = resources_made_ || irradiance_.texture || state_buffer_;
  passes_->ReleaseImage(irradiance_);
  passes_->ReleaseImage(moments_);
  passes_->ReleaseImage(weight_);
  if (state_buffer_ && frames_) {
    frames_->Retire(std::move(state_buffer_));
  }
  state_buffer_.reset();
  state_bytes_ = 0;
  RetireSets();
  state_.Stop();
  fade_.Reset();
  resources_made_ = false;
  update_ready_ = false;
  if (had) {
    ++generation_;
    if (why) {
      REXLOG_INFO("{} probe field given back ({})", kPrefix, why);
    }
  }
}

uint64_t GiField::HeldBytes() const {
  uint64_t bytes = state_buffer_ ? state_bytes_ : 0;
  for (const HostImage* image : {&irradiance_, &moments_, &weight_}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  for (const Slot& s : slots_) {
    bytes += (s.rows ? kUploadBytes : 0) + (s.list ? kUploadBytes : 0);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void GiField::BeginFrame(const Inputs& in) {
  ++frame_count_;
  inputs_ = in;
  active_ = usable_ = update_ready_ = published_ = false;
  move_ = rules::MoveReport{};
  update_frame_ = gi::GiFrame{};
  published_frame_ = gi::GiFrame{};
  gpu_frame_ = gi::GpuGiFrame{};
  if (!ready_) {
    return;
  }
  const double dt = have_time_ ? std::clamp(in.time_s - last_time_s_, 0.0, 0.25) : 0.0;
  last_time_s_ = in.time_s;
  have_time_ = true;
  if (frame_count_ - interval_start_frame_ >= kIntervalFrames) {
    LogInterval();
  }

  const gi::Layout wanted = gi::LayoutFor(in.rung, in.level);
  if (wanted == gi::Layout::kNone) {
    gi_setting::NoteResourcePressure(false);
    memory_warned_ = false;
    // The setting is off (or the card has no ray tracing): a field that was on fades out (the screen-space bounce light crossfades in), then goes back after a while.
    if (!resources_made_) {
      fit_ = rules::FitAt(gi::Layout::kNone, rules::Step::kNone);
      return;
    }
    const float fade = fade_.Step(dt, false, 0.0f);
    off_frames_ = fade > 0.0f ? 0u : off_frames_ + 1u;
    if (!gfx_preset::retain_scene_resources.load(std::memory_order_relaxed) && off_frames_ >= kReleaseAfterOffFrames) {
      ReleaseResources("the setting is off");
      return;
    }
    active_ = fade > 0.0f;
  } else {
    off_frames_ = 0;
    if (!PipelinesReady()) {
      return;  // the pipelines are made in the background: the field starts from the frame they are done
    }
    const bool layout_changed = resources_made_ && fit_.wanted != wanted;
    if (resources_made_ && !layout_changed && memory_warned_ && frame_count_ >= next_refit_frame_ && device_) {
      next_refit_frame_ = frame_count_ + kRefitFrames;
      const auto memory = device_->QueryVideoMemory();
      memory_warned_ = rules::FixedFitPressure(fit_, HeldBytes(), memory.used, memory.budget);
      gi_setting::NoteResourcePressure(memory_warned_);
    }
    if (layout_changed || (!resources_made_ && frame_count_ >= next_refit_frame_)) {
      next_refit_frame_ = frame_count_ + kRefitFrames;
      const rules::Fit fit = rules::RequestedFit(wanted);
      if (!device_) return;
      const auto memory = device_->QueryVideoMemory();
      const bool pressure = rules::FixedFitPressure(fit, HeldBytes(), memory.used, memory.budget);
      if (pressure && !memory_warned_) {
        memory_warned_ = true;
        REXLOG_WARN("{} video memory is nearly full ({} of {} MiB in use); the {} probe field needs about {:.1f} MiB",
                    kPrefix, memory.used >> 20, memory.budget >> 20, gi::LayoutName(wanted), Mib(fit.memory.Total()));
      }
      const bool made = MakeResources(fit, in.camera);
      gi_setting::NoteResourcePressure(pressure || !made);
      if (made) {
        ++refits_;
        memory_warned_ = pressure;
      } else if (layout_changed || last_none_log_ == 0 || frame_count_ - last_none_log_ >= 1800) {
        last_none_log_ = frame_count_;
        REXLOG_WARN("{} the {} probe field could not be allocated ({:.1f} MiB); retrying the same layout",
                    kPrefix, gi::LayoutName(wanted), Mib(fit.memory.Total()));
      }
      // CHECK: force an allocation failure and confirm the selected layout is retried on both APIs.
    }
    if (!resources_made_) {
      return;
    }
    usable_ = in.world_ok && in.safe_range_m >= kMinSafeRangeM;
    if (usable_) {
      idle_frames_ = 0;
      safe_range_kept_ = in.safe_range_m;
      shown_level_ = in.level;
      if (in.cut != rules::CutKind::kNone) {
        state_.RequestCut(in.cut);
      }
      move_ = state_.Move(in.camera);
      if (move_.fresh || move_.cut || move_.relight) {
        fade_.Reset();  // an hour jump cleared the probes like a cut: the field shows nothing until it is traced again (the screen-space bounce light meanwhile)
      }
      ++moves_;
      cuts_ += move_.cut ? 1u : 0u;
      relights_ += move_.relight ? 1u : 0u;
      exposed_ += move_.exposed;
    } else if (++idle_frames_ >= kReleaseAfterIdleFrames) {
      ReleaseResources("the world has had no field for a minute");
      return;
    }
    const float fade = fade_.Step(dt, usable_, state_.DirtyShare(0));
    active_ = usable_ || fade > 0.0f;
  }
  if (!active_) {
    return;
  }

  // The frame the update and the readers take.
  gi::GiFrame& f = update_frame_;
  f = state_.Frame(in.camera);
  f.generation = generation_;
  f.frame_serial = uint32_t(in.frame);
  f.level = usable_ ? in.level : shown_level_;
  f.layout = fit_.wanted;
  f.atlas_width = gi::AtlasWidth(fit_.budget);
  f.atlas_height = gi::AtlasHeight(fit_.budget);
  f.slot[gi::kIrradiance] = irradiance_.slot;
  f.slot[gi::kMoments] = moments_.slot;
  f.slot[gi::kProbeWeight] = weight_.slot;
  f.valid = gi::kFieldProducts;
  f.fade = fade_.fade;
  f.safe_range_m = usable_ ? in.safe_range_m : safe_range_kept_;
  f.strength = in.strength;
  f.bounce_weight = in.bounce_weight;
  for (int c = 0; c < 3; ++c) {
    f.sun_rgb[c] = in.sun_rgb[c];
    f.ambient_up_rgb[c] = in.ambient_up_rgb[c];
    f.ambient_down_rgb[c] = in.ambient_down_rgb[c];
  }
  f.sun_sin_elevation = in.sun_sin_elevation;
  if (fit_.Reduced() || move_.cut || move_.relight || move_.fresh) {
    Log(move_.cut ? "cut" : move_.relight ? "relight" : move_.fresh ? "fresh" : "reduced");
  }
}

void GiField::Log(const char* what) {
  const uint32_t key = (uint32_t(fit_.wanted) << 8) | uint32_t(fit_.step) | (move_.cut ? 0x10000u : 0u) | (move_.relight ? 0x20000u : 0u);
  if (key == last_logged_fit_) {
    return;
  }
  last_logged_fit_ = key;
  REXLOG_INFO("{} frame {}: {} (layout {}, {}, {} cascades, generation {}, fade {:.2f}, safe range {:.0f} m, dirty {} of {} probes in the finest cascade)", kPrefix, inputs_.frame, what,
              gi::LayoutName(fit_.wanted), rules::StepName(fit_.step), fit_.budget.cascade_count, generation_, fade_.fade, update_frame_.safe_range_m, state_.DirtyCount(0),
              fit_.budget.cascade[0].dims.Probes());
}

void GiField::LogInterval() {
  const uint64_t frames = frame_count_ - interval_start_frame_;
  interval_start_frame_ = frame_count_;
  if (!resources_made_) {
    return;
  }
  REXLOG_INFO("{} {} frames: windows moved on {} of them ({} probes cleared, {} cuts, {} relights), published {}, reset passes {}, finest cascade {:.1f}% dirty, fade {:.2f}, {:.1f} MiB held, "
              "debug view drawn {} times",
              kPrefix, frames, moves_, exposed_, cuts_, relights_, published_frames_, reset_runs_, state_.DirtyShare(0) * 100.0f, fade_.fade, Mib(HeldBytes()), view_runs_);
  moves_ = cuts_ = relights_ = exposed_ = published_frames_ = reset_runs_ = view_runs_ = 0;
}

std::string GiField::CensusText() const {
  if (!ready_) {
    return {};
  }
  if (!resources_made_) {
    return std::format("; probe field: none (layout {})", gi::LayoutName(fit_.wanted));
  }
  return std::format("; probe field: {} of {} ({} cascades, {:.1f} MiB, fade {:.2f}, finest cascade {:.1f}% dirty, {} refits, {} creations failed)", rules::StepName(fit_.step),
                     gi::LayoutName(fit_.wanted), fit_.budget.cascade_count, Mib(HeldBytes()), fade_.fade, state_.DirtyShare(0) * 100.0f, refits_, resources_failed_);
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

bool GiField::EnsureSlot(uint32_t frame_slot) {
  if (frame_slot >= kFramesInFlight || !resources_made_) {
    return false;
  }
  Slot& s = slots_[frame_slot];
  if (!s.rows) {
    s.rows = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(kUploadBytes, plume::RenderBufferFlag::STORAGE), "gi frame rows");
    s.list = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(kUploadBytes, plume::RenderBufferFlag::STORAGE), "gi reset list");
    if (!s.rows || !s.list) {
      s.rows.reset();
      s.list.reset();
      return false;
    }
  }
  if (!s.field_set || (view_layout_ && !s.view_set) || s.sets_generation != generation_) {
    if (frames_) {
      frames_->Retire(std::move(s.field_set));
      frames_->Retire(std::move(s.view_set));
    }
    s.field_set = device_->CreateDescriptorSet(field_set_builder_, 6, 0, "gi field set");  // six views: D3D12 sizes the set by this count, not by the ranges (the same fix as the city shadow's output set). CHECK (run): the D3D12 debug layer accepts the set
    if (!s.field_set) {
      return false;
    }
    s.field_set->setBuffer(field_rows_index_, s.rows.get(), kUploadBytes);
    s.field_set->setBuffer(field_list_index_, s.list.get(), kUploadBytes);
    s.field_set->setTexture(field_irradiance_index_, irradiance_.texture.get(), plume::RenderTextureLayout::GENERAL, irradiance_.view.get());
    s.field_set->setTexture(field_moments_index_, moments_.texture.get(), plume::RenderTextureLayout::GENERAL, moments_.view.get());
    s.field_set->setTexture(field_weight_index_, weight_.texture.get(), plume::RenderTextureLayout::GENERAL, weight_.view.get());
    s.field_set->setBuffer(field_state_index_, state_buffer_.get(), state_bytes_);
    if (view_layout_) {
      s.view_set = device_->CreateDescriptorSet(view_set_builder_, 2, 0, "gi field view set");  // two views
      if (s.view_set) {
        s.view_set->setBuffer(view_rows_index_, s.rows.get(), kUploadBytes);
        s.view_set->setBuffer(view_state_index_, state_buffer_.get(), state_bytes_);
      }
    }
    s.sets_generation = generation_;
  }
  return true;
}

bool GiField::RecordMaintenance(plume::RenderCommandList* list, uint32_t frame_slot) {
  update_ready_ = false;
  if (!list || !ready_ || !active_ || !resources_made_ || !EnsureSlot(frame_slot)) {
    return false;
  }
  plume::RenderPipeline* reset = Pipeline(kVReset);
  if (!reset) {
    return false;
  }
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  Slot& s = slots_[frame_slot];

  // The rows: this frame's table (the weights pass and the debug view read them).
  gpu_frame_ = gi::MakeGpuFrame(update_frame_);
  void* mapped = s.rows->map();
  if (!mapped) {
    return false;
  }
  std::memcpy(mapped, gpu_frame_.words.data(), gi::kGpuFrameBytes);
  s.rows->unmap();

  // The textures were left readable by the pixel and compute passes of the last frame that used them (or are new): storage images now, and the state buffer's last writer is done.
  // CHECK: the first frame's transition from the undefined layout of new textures and a new buffer (D3D12: the common state) to the storage state in one barrier call, and the UAV barrier
  // that the same call makes when a texture is already in it (Plume's D3D12 barriers emit one when the state does not change).
  const plume::RenderTextureBarrier textures[3] = {{irradiance_.texture.get(), Layout::GENERAL}, {moments_.texture.get(), Layout::GENERAL}, {weight_.texture.get(), Layout::GENERAL}};
  const plume::RenderBufferBarrier buffers[1] = {{state_buffer_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
  list->barriers(Stage::COMPUTE, buffers, 1, textures, 3);
  irradiance_.layout = moments_.layout = weight_.layout = Layout::GENERAL;
  ++maintenance_runs_;

  // The exposed probes are cleared: one group of 10 x 10 threads for each.
  if (move_.exposure_count > 0) {
    const rules::ResetList reset_list = move_.Reset();
    if (reset_list.total > 0) {
      void* list_mapped = s.list->map();
      if (list_mapped) {
        std::memcpy(list_mapped, reset_list.words.data(), rules::kResetListBytes);
        s.list->unmap();
        pass_timing_->Mark(list, "gi reset");
        list->setComputePipelineLayout(compute_layout_.get());
        list->setComputeDescriptorSet(s.field_set.get(), 0);
        list->setPipeline(reset);
        list->dispatch(rules::kResetGroupsX, reset_list.GroupsY(), 1);
        // The update (G2) writes the same textures and the state words: its first pass finds the clears done.
        list->barriers(Stage::COMPUTE, buffers, 1, textures, 3);
        ++reset_runs_;
      }
    }
  }
  update_ready_ = true;
  return true;
}

bool GiField::RecordFinish(plume::RenderCommandList* list, uint32_t frame_slot) {
  if (!list || !update_ready_ || frame_slot >= kFramesInFlight) {
    return false;
  }
  plume::RenderPipeline* weights = Pipeline(kVWeights);
  if (!weights) {
    return false;
  }
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  Slot& s = slots_[frame_slot];
  // The update's writes are done before the weights pass reads the state words, and the weight map's last writer (the reset) before it writes it.
  {
    const plume::RenderTextureBarrier textures[3] = {{irradiance_.texture.get(), Layout::GENERAL}, {moments_.texture.get(), Layout::GENERAL}, {weight_.texture.get(), Layout::GENERAL}};
    const plume::RenderBufferBarrier buffers[1] = {{state_buffer_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE}};
    list->barriers(Stage::COMPUTE, buffers, 1, textures, 3);
  }
  pass_timing_->Mark(list, "gi weights");
  list->setComputePipelineLayout(compute_layout_.get());
  list->setComputeDescriptorSet(s.field_set.get(), 0);
  list->setPipeline(weights);
  list->dispatch(rules::WeightsGroups(state_.table().Total()), 1, 1);
  // The readers sample the three textures (pixel shaders: the apply; compute shaders: the hit shading, the per-pixel pass).
  // CHECK: that GRAPHICS_AND_COMPUTE covers both kinds of reader on Vulkan (the destination stages of the layout change).
  {
    const plume::RenderTextureBarrier textures[3] = {{irradiance_.texture.get(), Layout::SHADER_READ}, {moments_.texture.get(), Layout::SHADER_READ}, {weight_.texture.get(), Layout::SHADER_READ}};
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, textures, 3);
    irradiance_.layout = moments_.layout = weight_.layout = Layout::SHADER_READ;
  }
  published_frame_ = update_frame_;
  gpu_frame_ = gi::MakeGpuFrame(published_frame_);
  published_ = true;
  update_ready_ = false;
  ++published_frames_;
  return true;
}

bool GiField::RecordDebugView(plume::RenderCommandList* list, uint32_t frame_slot, const ViewTarget& target, const ViewInputs& in) {
  if (!list || !published_ || !published_frame_.FieldPresent() || !rules::IsOwnView(in.view) || !target.framebuffer || target.width == 0 || target.height == 0 || frame_slot >= kFramesInFlight) {
    return false;
  }
  Slot& s = slots_[frame_slot];
  if (!s.view_set) {
    return false;
  }
  plume::RenderPipeline* pipeline = ViewPipelineFor(target.format, target.samples);
  if (!pipeline) {
    return false;
  }
  ViewPush push = {};
  for (int i = 0; i < 3; ++i) {
    push.pos_tan_x[i] = in.camera.position[i];
    push.right_tan_y[i] = in.camera.right[i];
    push.up_gain[i] = in.camera.up[i];
    push.forward_max[i] = in.camera.forward[i];
  }
  push.pos_tan_x[3] = in.camera.tan_x;
  push.right_tan_y[3] = in.camera.tan_y;
  push.up_gain[3] = in.gain;
  push.forward_max[3] = std::max(in.max_distance_m, 1.0f);
  const bool have_depth = in.depth && in.depth->texture && in.depth->slot != kNoSlot;
  push.info[0] = in.view;
  push.info[1] = std::min(in.cascade, std::max(published_frame_.cascade_count, 1u) - 1u);
  push.info[2] = have_depth ? in.depth->slot : kNoSlot;
  push.info[3] = have_depth ? in.depth->width : 0u;
  push.size[0] = target.width;
  push.size[1] = target.height;
  push.size[2] = have_depth ? in.depth->height : 0u;
  push.size[3] = std::bit_cast<uint32_t>(rules::kSphereRadiusSpacings);

  // The state words were written as storage; the pixel shader reads them.
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderBufferBarrier(state_buffer_.get(), plume::RenderBufferAccess::READ));
  pass_timing_->Mark(list, "gi field view");
  list->setFramebuffer(target.framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(target.width), float(target.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(target.width), int32_t(target.height));
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(view_layout_.get());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 1);
  list->setGraphicsDescriptorSet(s.view_set.get(), 2);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  ++view_runs_;
  return true;
}

}  // namespace mcla::gpu
