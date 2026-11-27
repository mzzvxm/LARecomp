// MCLA GPU renderer -- the player's car windows reflecting the city, ray traced. See car_rt_glass_refl.h.

#include "car_rt_glass_refl.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_rt_hit_rules.h"
#include "car_rt_denoise_v2.h"
#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"

namespace mcla::gpu {

namespace glass = car_glass;

namespace {

constexpr const char* kPrefix = "[car glass]";
constexpr const char* kPipelineName = "car glass refl";
constexpr uint32_t kGroup = glass::kGroup;
constexpr plume::RenderFormat kApplyFormat = plume::RenderFormat::R16G16B16A16_FLOAT;  // the main colour as a host texture (ssr.cpp kFormat)
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 3); }
uint64_t ApplyKey(uint32_t index) { return HostPrecache::Key(HostPrecache::kCarRtRefl, 4 + index); }

const char* ApplyName(uint32_t index) {
  static constexpr const char* kNames[4] = {"car glass apply 1x", "car glass apply 2x", "car glass apply 4x", "car glass apply 8x"};
  return index < 4 ? kNames[index] : "car glass apply ?";
}

// The apply's pipeline for a target of `samples` host samples: 1, 2, 4 or 8 (0 to 3 here), else none.
bool ApplyIndexOf(uint32_t samples, uint32_t* index) {
  switch (samples) {
    case 1: *index = 0; return true;
    case 2: *index = 1; return true;
    case 4: *index = 2; return true;
    case 8: *index = 3; return true;
    default: return false;
  }
}

}  // namespace

CarRtGlassRefl::~CarRtGlassRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarRtGlassRefl::Initialize(Device& device, HostPasses& passes, BindlessHeap& heap, FrameRing& frames, PassTiming& pass_timing, CarRtScene& scene,
                                const plume::RenderDescriptorSetBuilder& output_builder, const plume::RenderDescriptorSetBuilder& city_builder, bool list_at_start) {
  device_ = &device;
  passes_ = &passes;
  heap_ = &heap;
  frames_ = &frames;
  pass_timing_ = &pass_timing;
  output_builder_ = &output_builder;
  ready_ = false;
  listed_ = false;
  pipeline_failed_ = false;
  apply_listed_ = {};
  apply_failed_ = {};

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap.texture_set_builder());  // space 0: Texture2D<float4>[] (the history)
  const uint32_t output_set = builder.addDescriptorSet(output_builder);                // space 1: u0 the glass image, u1 the aux image, u2 the counters
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());                // space 2: the car's structures (CAR_RT_SPACE)
  const uint32_t city_set = builder.addDescriptorSet(city_builder);                    // space 3: the city's rows and counters (city_refl.h)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet || city_set != kCitySet) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shader declares them", kPrefix);
    return;
  }
  layout_ = device.CreatePipelineLayout(builder, "car glass refl layout");
  if (!layout_) {
    return;
  }
  const HostShaderBlob blob = CarRtReflGlassShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  const HostShaderBlob apply_blob = CarGlassApplyShader();
  apply_shader_ = passes.CreateShader(apply_blob.data, apply_blob.size, apply_blob.name);
  if (!shader_ || !apply_shader_) {
    shader_.reset();
    apply_shader_.reset();
    layout_.reset();
    return;
  }
  ready_ = true;
  if (list_at_start) {
    listed_ = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
    apply_listed_[0] = passes.precache().Add(ApplyKey(0), ApplyName(0), [this] { return MakeApplyPipeline(0); });
    passes.precache().Note("ray traced reflections of the player's glass: on where the car self-reflection is ray traced", (listed_ ? 1u : 0u) + (apply_listed_[0] ? 1u : 0u));
  }
}

void CarRtGlassRefl::Shutdown() {
  // The host builder's function names this object's layout and shaders: CarRtRefl::Shutdown stopped it already (passes_->precache().Stop()).
  for (auto& pipeline : apply_) {
    pipeline.reset();
  }
  pipeline_.reset();
  set_.reset();
  set_image_ = nullptr;
  set_aux_ = nullptr;
  set_guide_ = nullptr;
  image_ = HostImage{};  // the GPU is idle and the heap goes later: the slot is not handed back (CarRtRefl::Shutdown)
  shader_.reset();
  apply_shader_.reset();
  layout_.reset();
  ready_ = false;
  traced_ = false;
  device_ = nullptr;
  passes_ = nullptr;
  heap_ = nullptr;
  frames_ = nullptr;
  pass_timing_ = nullptr;
  output_builder_ = nullptr;
}

std::unique_ptr<plume::RenderPipeline> CarRtGlassRefl::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, kGroup, 1), kPipelineName);
}

// The apply: the colour added to the main colour (ONE, ONE on the colour channels, alpha untouched), the target's sample count; ssr.cpp's apply pipeline is the model.
std::unique_ptr<plume::RenderPipeline> CarRtGlassRefl::MakeApplyPipeline(uint32_t index) {
  if (!passes_ || !apply_shader_ || index >= kApplyVariants) {
    return nullptr;
  }
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::ONE;
  blend.dstBlend = plume::RenderBlend::ONE;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  return passes_->CreatePipeline(passes_->layout(), *apply_shader_, kApplyFormat, blend, 1u << index, ApplyName(index));
}

// The trace's pipeline when it is made, else null: the frame goes on without the pass (never a wait, never a compile on the render thread).
plume::RenderPipeline* CarRtGlassRefl::Pipeline() {
  if (pipeline_) {
    return pipeline_.get();
  }
  if (pipeline_failed_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    // Off at the start, wanted now: made on a worker of their own.
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({PipelineKey(), kPipelineName, [this] { return MakePipeline(); }});
    listed_ = true;
    precache.AddLive("ray traced reflections of the player's glass", std::move(fresh));
    REXLOG_INFO("{} the trace's pipeline was not listed at the start: it is made in the background, and the pass runs from the frame it is done", kPrefix);
    return nullptr;
  }
  if (!precache.Ready(PipelineKey())) {
    return nullptr;
  }
  pipeline_ = precache.Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the trace's pipeline could not be made: the player's windows keep the game's reflection this session", kPrefix);
  }
  return pipeline_.get();
}

plume::RenderPipeline* CarRtGlassRefl::ApplyPipeline(uint32_t index) {
  if (index >= kApplyVariants) {
    return nullptr;
  }
  if (apply_[index]) {
    return apply_[index].get();
  }
  if (apply_failed_[index]) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!apply_listed_[index]) {
    std::vector<HostPrecache::Item> fresh;
    fresh.push_back({ApplyKey(index), ApplyName(index), [this, index] { return MakeApplyPipeline(index); }});
    apply_listed_[index] = true;
    precache.AddLive("ray traced reflections of the player's glass", std::move(fresh));
    REXLOG_INFO("{} the pipeline '{}' is wanted: it is made in the background, and the pass runs from the frame it is done", kPrefix, ApplyName(index));
    return nullptr;
  }
  if (!precache.Ready(ApplyKey(index))) {
    return nullptr;
  }
  apply_[index] = precache.Acquire(ApplyKey(index), ApplyName(index), [this, index] { return MakeApplyPipeline(index); });
  if (!apply_[index]) {
    apply_failed_[index] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no glass reflection in that configuration this session", kPrefix, ApplyName(index));
  }
  return apply_[index].get();
}

// ---------------------------------------------------------------------------
// the image
// ---------------------------------------------------------------------------

void CarRtGlassRefl::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_image_ = nullptr;
  set_aux_ = nullptr;
  set_guide_ = nullptr;
}

bool CarRtGlassRefl::EnsureImage(uint32_t width, uint32_t height) {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = image_.texture.get();
  if (!passes_->EnsureImage(image_, width, height, plume::RenderFormat::R16G16B16A16_FLOAT, "car glass refl image", kFlags)) {
    RetireSet();  // EnsureImage released the image and logged it
    return false;
  }
  if (image_.texture.get() != before) {
    RetireSet();  // a new texture is a new set for it (the old one goes after the lists that name it)
    if (before == nullptr) {
      REXLOG_INFO("{} image {}x{} R16G16B16A16_FLOAT ({:.1f} MiB) for the player's windows", kPrefix, width, height, double(glass::HeldBytes(width, height)) / (1024.0 * 1024.0));
    }
  }
  return true;
}

bool CarRtGlassRefl::EnsureSet(const HostImage& aux, const plume::RenderBuffer* stats, const HostImage& guide) {
  if (set_ && set_image_ == image_.texture.get() && set_aux_ == aux.texture.get() && set_guide_ == guide.texture.get()) {
    return true;
  }
  RetireSet();
  if (!device_ || !output_builder_) return false;
  set_ = device_->CreateDescriptorSet(*output_builder_, 4, 0, "car glass refl output set");
  if (!set_) {
    return false;
  }
  set_->setTexture(0, image_.texture.get(), plume::RenderTextureLayout::GENERAL, image_.view.get());
  set_->setTexture(1, aux.texture.get(), plume::RenderTextureLayout::GENERAL, aux.view.get());
  if (stats) {
    set_->setBuffer(2, stats, car_rt_hit::census::kBytes);
  }
  set_->setTexture(3, guide.texture.get(), plume::RenderTextureLayout::GENERAL, guide.view.get());
  set_image_ = image_.texture.get();
  set_aux_ = aux.texture.get();
  set_guide_ = guide.texture.get();
  return true;
}

void CarRtGlassRefl::ReleaseImage() {
  traced_ = false;
  idle_frames_ = 0;
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  RetireSet();
}

uint64_t CarRtGlassRefl::HeldBytes() const { return image_.texture ? glass::HeldBytes(image_.width, image_.height) : 0u; }

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CarRtGlassRefl::NoteNotRun(glass::Why why, uint64_t frame) {
  window_.not_run[uint32_t(why)] += 1;
  // A pass that is wanted but cannot run, or is off, for a while gives its image back.
  if (image_.texture && ++idle_frames_ >= glass::kReleaseAfterFrames) {
    ReleaseImage();
  }
  (void)frame;
}

void CarRtGlassRefl::LogWindow(uint64_t frame) {
  REXLOG_INFO("{}", glass::WindowText(window_, frame));
  window_ = glass::Window{};
}

bool CarRtGlassRefl::Trace(const Frame& f) {
  traced_ = false;
  if (!ready_ || !f.list || !f.view || !f.base_push || !f.aux || !f.aux->texture || !f.city ||
      !f.unused_guide || !f.unused_guide->texture) {
    return false;
  }
  if (f.base_push_bytes != kBasePushBytes) {
    if (!first_logged_) {
      first_logged_ = true;
      REXLOG_ERROR("{} the player's push block is {} bytes, not the {} this pass copies (CarRtRefl::Push changed): the pass is off", kPrefix, f.base_push_bytes, kBasePushBytes);
    }
    return false;
  }
  if (!f.wanted) {
    if (image_.texture) {
      NoteNotRun(glass::Why::kSettingOff, f.frame);
    }
    return false;
  }
  const CarRtView& view = *f.view;
  const car_rt_opp_refl::TexelRect rect = glass::RectFor(view.player_box, f.tan_x, f.tan_y, double(f.render_width), double(f.render_height), double(std::max<uint32_t>(f.divisor, 1)),
                                                         int32_t(f.image_width), int32_t(f.image_height));
  // The frame's facts, every one of them (the decision's `box` was left out of this list until CITY-M2-PICS: Decide() then said kNoBox for every frame and the pass never ran; the 11-21 run's
  // pictures had no window reflection at all, glass_rules_test HostFacts).
  glass::HostInputs inputs;
  inputs.wanted = f.wanted;
  inputs.city_set = f.city->set != nullptr;
  inputs.city_on = f.city->on;
  inputs.screen = f.city->screen;
  inputs.rect_empty = rect.Empty();
  inputs.view_valid = view.valid;
  inputs.view_set = view.set != nullptr;
  const glass::Why basic = glass::Decide(glass::HostFacts(inputs));
  if (basic != glass::Why::kUsed) {
    NoteNotRun(basic, f.frame);
    return false;
  }
  uint32_t apply_index = 0;
  plume::RenderPipeline* pipeline = Pipeline();
  plume::RenderPipeline* apply = ApplyIndexOf(f.colour_samples, &apply_index) ? ApplyPipeline(apply_index) : nullptr;
  if (!pipeline || !apply) {
    NoteNotRun(glass::Why::kNotReady, f.frame);
    return false;
  }
  if (!EnsureImage(f.image_width, f.image_height)) {
    NoteNotRun(glass::Why::kNoImage, f.frame);
    return false;
  }
  bool filtered = f.denoiser && f.denoiser->PrepareGlass(image_, rect);
  if (filtered && !EnsureSet(*f.aux, f.stats, f.denoiser->Guide())) filtered = false;
  HostImage& guide = filtered ? f.denoiser->Guide() : *f.unused_guide;
  if (!EnsureSet(*f.aux, f.stats, guide)) {
    NoteNotRun(glass::Why::kNoImage, f.frame);
    return false;
  }

  Push push = {};
  std::memcpy(&push, f.base_push, kBasePushBytes);
  push.slots[2] = glass::kPrimaryParts;
  push.slots[3] &= ~(car_rt_hit::census::kStatsFlag | car_rt_denoise_v2::kTraceGuide);
  if (filtered) push.slots[3] |= car_rt_denoise_v2::kTraceGuide;
  const float strength = glass::kStrength;
  std::memcpy(&push.glass[2], &strength, sizeof(strength));
  push.glass[0] = uint32_t(rect.x0);
  push.glass[1] = uint32_t(rect.y0);

  plume::RenderCommandList* list = f.list;
  const plume::RenderTextureBarrier barrier(image_.texture.get(), plume::RenderTextureLayout::GENERAL);
  pass_timing_->Mark(list, "car glass refl");
  list->barriers(plume::RenderBarrierStage::COMPUTE, nullptr, 0, &barrier, 1);
  image_.layout = plume::RenderTextureLayout::GENERAL;
  if (filtered || guide.layout != plume::RenderTextureLayout::GENERAL) {
    list->barriers(plume::RenderBarrierStage::COMPUTE,
                   plume::RenderTextureBarrier(guide.texture.get(), plume::RenderTextureLayout::GENERAL));
    guide.layout = plume::RenderTextureLayout::GENERAL;
  }
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(set_.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setComputeDescriptorSet(f.city->set, kCitySet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(car_rt_opp_refl::GroupsX(rect, kGroup), car_rt_opp_refl::GroupsY(rect, kGroup), 1);
  if (filtered) {
    pass_timing_->Mark(list, "car rt glass denoise v2");
    f.denoiser->RecordGlass(list, image_, rect);
  }

  traced_ = true;
  traced_frame_ = f.frame;
  rect_ = rect;
  divisor_ = std::max<uint32_t>(f.divisor, 1);
  idle_frames_ = 0;
  ++passes_run_;
  window_.texels += rect.Area();
  if (window_.passes == 0) {
    window_.first_frame = f.frame;
  }
  ++window_.passes;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first pass, frame {}: the rectangle {}x{} of the {}x{} history image (texels {}..{} x {}..{}), the glass's Fresnel F0 {:.2f} up to {:.2f}, the mirror ray {} m, the picture's depth from lin_depth; timed "
                "as 'car glass refl' and 'car glass apply'; the glass image {:.1f} MiB",
                kPrefix, f.frame, rect.Width(), rect.Height(), f.image_width, f.image_height, rect.x0, rect.x1, rect.y0, rect.y1, double(glass::kF0), double(glass::kMaxFresnel), double(push.lens[3]),
                double(glass::HeldBytes(f.image_width, f.image_height)) / (1024.0 * 1024.0));
  }
  if (window_.passes >= glass::kLogEveryPasses) {
    LogWindow(f.frame);
  }
  return true;
}

bool CarRtGlassRefl::Apply(plume::RenderCommandList* list, EdramTargets& edram, EdramTargets::Target& colour, uint32_t render_width, uint32_t render_height) {
  if (!traced_ || !list || !image_.texture || image_.slot == 0xFFFFFFFFu) {
    return false;
  }
  traced_ = false;
  uint32_t index = 0;
  if (!ApplyIndexOf(colour.host_samples, &index)) {
    return false;
  }
  plume::RenderPipeline* pipeline = ApplyPipeline(index);
  if (!pipeline || !colour.texture) {
    NoteNotRun(glass::Why::kNotReady, traced_frame_);
    return false;
  }
  EdramTargets::Target* only[1] = {&colour};
  // The glass image: written by the trace (storage), read here as a texture; the main colour: a colour target in the middle of the frame (the occlusion's and the reflections' way: no depth, the next draw
  // of the game moves things back itself).
  const plume::RenderTextureBarrier read(image_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, &read, 1);
  image_.layout = plume::RenderTextureLayout::SHADER_READ;
  edram.Transition(list, colour, plume::RenderTextureLayout::COLOR_WRITE);
  plume::RenderFramebuffer* framebuffer = edram.Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  glass::ApplyPush push = {};
  push.image[0] = image_.slot;
  push.image[1] = image_.width;
  push.image[2] = image_.height;
  push.image[3] = divisor_;  // the picture over the image, 1 or 2 (the trace's)
  push.rect[0] = uint32_t(rect_.x0);
  push.rect[1] = uint32_t(rect_.y0);
  push.rect[2] = uint32_t(rect_.x1);
  push.rect[3] = uint32_t(rect_.y1);
  push.sizes[0] = float(render_width);
  push.sizes[1] = float(render_height);
  pass_timing_->Mark(list, "car glass apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(render_width), float(render_height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(render_width), int32_t(render_height));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  // The closing mark (PassTiming: a mark's time runs to the next mark): what is recorded next (the debug views, the game's draws) is not the apply's.
  pass_timing_->Mark(list, "after car glass");
  ++applies_run_;
  return true;
}

std::string CarRtGlassRefl::CensusText() {
  if (!ready_ || passes_run_ == 0) {
    return {};
  }
  const uint64_t interval = passes_run_ - census_passes_;
  census_passes_ = passes_run_;
  return std::format("; glass: {} passes since the start ({} in the interval, {} applied)", passes_run_, interval, applies_run_);
}

}  // namespace mcla::gpu
