// MCLA GPU renderer -- the player's side mirrors show the real view behind, ray traced. See car_mirror_rt.h.

#include "car_mirror_rt.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_rt_scene.h"
#include "car_selfrefl_rules.h"
#include "city_refl_setting.h"
#include "descriptors.h"
#include "device.h"
#include "draw_decode.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "rt_scene_service.h"
#include "rt_tier.h"
#include "temporal_identities.h"

namespace mcla::gpu {

namespace cm = car_mirror;
namespace cmc = car_mirror_city;

namespace {

constexpr const char* kPrefix = "[car mirror]";
constexpr const char* kPipelineName = "car mirror rt";
constexpr plume::RenderFormat kApplyFormat = plume::RenderFormat::R16G16B16A16_FLOAT;  // the main colour as a host texture
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);
constexpr uint32_t kMinRenderSize = 64;
constexpr uint64_t kQuietLogFrames = 1800;  // the window's line when no pass ran in it: every half minute at 60 FPS
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

LiveText& Setting() {
  static LiveText text("mcla_gpu_car_mirror_rt");
  return text;
}

// MIRROR-CITY: what the mirror ray shows where it finds none of the cars (auto, env, city).
LiveText& WorldSetting() {
  static LiveText text("mcla_gpu_car_mirror_rt_world");
  return text;
}

// MIRROR-CITY: the player's RT CITY REFLECTIONS word (auto, off, high, ultra), which the mirror's `auto` follows. A string cvar the pause menu's thread writes: the render thread reads it through a LiveText
// (live_text.h), as city_refl.cpp does, never with the cvar's own Get().
LiveText& PaintWordSetting() {
  static LiveText text("mcla_gpu_rt_reflections");
  return text;
}

uint64_t PipelineKey() { return HostPrecache::Key(HostPrecache::kCarMirrorRt, 0); }
uint64_t ApplyKey(uint32_t index) { return HostPrecache::Key(HostPrecache::kCarMirrorRt, 1 + index); }

const char* ApplyName(uint32_t index) {
  static constexpr const char* kNames[4] = {"car mirror apply 1x", "car mirror apply 2x", "car mirror apply 4x", "car mirror apply 8x"};
  return index < 4 ? kNames[index] : "car mirror apply ?";
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

CarMirrorRt::~CarMirrorRt() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarMirrorRt::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, EdramTargets& edram, CarRtScene& scene) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  edram_ = &edram;
  scene_ = &scene;
  ready_ = false;
  active_ = false;
  listed_ = false;
  pipeline_failed_ = false;
  apply_listed_ = {};
  apply_failed_ = {};
  if (!scene.ready()) {
    passes.precache().Note("ray traced side mirrors: not on this graphics card or driver (the game's mirrors stay)", 0);
    return;
  }
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);  // u0: the image the pass writes
  output_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  const uint32_t texture_set = builder.addDescriptorSet(heap.texture_set_builder());  // space 0: Texture2D<float4>[] (the environment map, the materials' textures)
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);              // space 1: u0 the mirror image
  const uint32_t scene_set = builder.addDescriptorSet(scene.SetDesc());               // space 2: the car's structures (CAR_RT_SPACE)
  builder.addPushConstant(0, kPushSpace, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (texture_set != kTextureSet || output_set != kOutputSet || scene_set != kSceneSet) {
    REXLOG_ERROR("{} the layout's sets are not in the order the shader declares them", kPrefix);
    return;
  }
  layout_ = device.CreatePipelineLayout(builder, "car mirror rt layout");
  if (!layout_) {
    return;
  }
  const HostShaderBlob blob = CarMirrorRtShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  const HostShaderBlob apply_blob = CarMirrorApplyShader();
  apply_shader_ = passes.CreateShader(apply_blob.data, apply_blob.size, apply_blob.name);
  if (!shader_ || !apply_shader_) {
    shader_.reset();
    apply_shader_.reset();
    layout_.reset();
    return;
  }
  ready_ = true;
  const cm::Setting setting = cm::SettingOf(Setting().Get());
  if (cm::Wanted(setting, true, gpu_class::CurrentRtClass())) {
    // Wanted at the start: the trace's pipeline and the applies for the sample counts the main colour has in play (1x with an upscaler, 2x and 4x by the MSAA level) are made ahead.
    uint32_t added = passes.precache().Add(PipelineKey(), kPipelineName, [this] { return MakePipeline(); }) ? 1u : 0u;
    listed_ = added != 0;
    for (uint32_t i = 0; i < 3; ++i) {
      apply_listed_[i] = passes.precache().Add(ApplyKey(i), ApplyName(i), [this, i] { return MakeApplyPipeline(i); });
      added += apply_listed_[i] ? 1u : 0u;
    }
    passes.precache().Note("ray traced side mirrors: on (mcla_gpu_car_mirror_rt) where the car's ray tracing runs", added);
  } else {
    passes.precache().Note("ray traced side mirrors: made when first wanted", 0);
  }
}

void CarMirrorRt::Shutdown() {
  // The host builder's functions name this object's layout and shaders: stop it first (safe again: every owner does).
  if (passes_) {
    passes_->precache().Stop();
  }
  for (auto& pipeline : apply_) {
    pipeline.reset();
  }
  pipeline_.reset();
  set_.reset();
  set_image_ = nullptr;
  image_ = HostImage{};  // the GPU is idle and the heap goes later: the slot is not handed back (as CarRtGlassRefl::Shutdown)
  shader_.reset();
  apply_shader_.reset();
  layout_.reset();
  ready_ = false;
  active_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  edram_ = nullptr;
  scene_ = nullptr;
  service_ = nullptr;
  world_ = cmc::Decision{};
  last_world_state_ = -1;
  city_seen_ = false;
  city_seen_frame_ = 0;
}

std::unique_ptr<plume::RenderPipeline> CarMirrorRt::MakePipeline() {
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), cm::kGroup, cm::kGroup, 1), kPipelineName);
}

// The apply: the colour blended over the main colour (SRC_ALPHA, INV_SRC_ALPHA on the colour channels, alpha untouched), the target's sample count.
std::unique_ptr<plume::RenderPipeline> CarMirrorRt::MakeApplyPipeline(uint32_t index) {
  if (!passes_ || !apply_shader_ || index >= kApplyVariants) {
    return nullptr;
  }
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
  blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  return passes_->CreatePipeline(passes_->layout(), *apply_shader_, kApplyFormat, blend, 1u << index, ApplyName(index));
}

// The trace's pipeline when it is made, else null: the frame goes on without the pass (never a wait, never a compile on the render thread).
plume::RenderPipeline* CarMirrorRt::Pipeline() {
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
    precache.AddLive("ray traced side mirrors", std::move(fresh));
    REXLOG_INFO("{} the trace's pipeline was not listed at the start: it is made in the background, and the pass runs from the frame it is done", kPrefix);
    return nullptr;
  }
  if (!precache.Ready(PipelineKey())) {
    return nullptr;
  }
  pipeline_ = precache.Acquire(PipelineKey(), kPipelineName, [this] { return MakePipeline(); });
  if (!pipeline_) {
    pipeline_failed_ = true;
    REXLOG_ERROR("{} the trace's pipeline could not be made: the game's side mirrors stay this session", kPrefix);
  }
  return pipeline_.get();
}

plume::RenderPipeline* CarMirrorRt::ApplyPipeline(uint32_t index) {
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
    precache.AddLive("ray traced side mirrors", std::move(fresh));
    REXLOG_INFO("{} the pipeline '{}' is wanted: it is made in the background, and the pass runs from the frame it is done", kPrefix, ApplyName(index));
    return nullptr;
  }
  if (!precache.Ready(ApplyKey(index))) {
    return nullptr;
  }
  apply_[index] = precache.Acquire(ApplyKey(index), ApplyName(index), [this, index] { return MakeApplyPipeline(index); });
  if (!apply_[index]) {
    apply_failed_[index] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no ray traced mirrors in that configuration this session", kPrefix, ApplyName(index));
  }
  return apply_[index].get();
}

// ---------------------------------------------------------------------------
// the image
// ---------------------------------------------------------------------------

void CarMirrorRt::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_image_ = nullptr;
}

bool CarMirrorRt::EnsureImage() {
  constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
  const plume::RenderTexture* before = image_.texture.get();
  if (!passes_->EnsureImage(image_, cm::kImageWidth, cm::kImageHeight, plume::RenderFormat::R16G16B16A16_FLOAT, "car mirror rt image", kFlags)) {
    RetireSet();  // EnsureImage released the image and logged it
    return false;
  }
  if (image_.texture.get() != before) {
    RetireSet();  // a new texture is a new set for it (the old one goes after the lists that name it)
    if (before == nullptr) {
      REXLOG_INFO("{} image {}x{} R16G16B16A16_FLOAT ({:.1f} MiB) for the two mirrors", kPrefix, cm::kImageWidth, cm::kImageHeight, double(cm::HeldBytes()) / (1024.0 * 1024.0));
    }
  }
  if (set_ && set_image_ == image_.texture.get()) {
    return true;
  }
  RetireSet();
  set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, "car mirror rt output set");
  if (!set_) {
    return false;
  }
  set_->setTexture(0, image_.texture.get(), plume::RenderTextureLayout::GENERAL, image_.view.get());
  set_image_ = image_.texture.get();
  return true;
}

void CarMirrorRt::ReleaseImage() {
  idle_frames_ = 0;
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(image_);
  RetireSet();
}

uint64_t CarMirrorRt::HeldBytes() const { return image_.texture ? cm::HeldBytes() : 0u; }

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CarMirrorRt::BeginFrame() {
  active_ = false;
  recorded_this_frame_ = false;
  world_ = cmc::Decision{};  // the environment map, until the setting and the card say otherwise (an off frame asks for nothing of the city)
  if (!ready_ || !scene_) {
    return;
  }
  const cm::Setting setting = cm::SettingOf(Setting().Get());
  if (!cm::Wanted(setting, true, gpu_class::CurrentRtClass())) {
    ++window_.not_run[size_t(cm::Why::kSettingOff)];
    if (image_.texture) {
      ++idle_frames_;
    }
    return;
  }
  active_ = true;
  // The scene builds a frame's structures only for a frame that asked in the one before (as CarFrame): ask in every frame this is on.
  scene_->Request();
  UpdateWorld();
}

// MIRROR-CITY: what the mirror's world is this frame (car_mirror_city_rules.h Decide: the reflections' own level rule with this setting's word, the class's cell, the data's state), and the wants of the
// city's structures, hit data and textures that go out to the scene service whether or not the data stands yet (it is the want that makes the app build the data). Only the pass waits for the data: until it
// does the mirror shows the environment map, as it did before this package.
void CarMirrorRt::UpdateWorld() {
  const cmc::World word = cmc::WorldOf(WorldSetting().Get());
  world_ = cmc::Decide(word, PaintWordSetting().Get(), gpu_class::Current(), gpu_class::CurrentRtTier(), gpu_class::CurrentRtClass(), city_refl_setting::DataNow());
  const int state = int(world_.why) * 16 + int(world_.data) * 4 + int(world_.level);
  if (state != last_world_state_) {
    last_world_state_ = state;
    REXLOG_INFO("{} the mirror's world (mcla_gpu_car_mirror_rt_world = {}): {} (the city's data is {}); level {}, reach {:.0f} m", kPrefix, cmc::WorldName(word), cmc::WhyName(world_.why),
                std::string(city_refl_setting::DataName(world_.data)), std::string(city_rt::LevelName(world_.wanted)), double(world_.cell.reach));
  }
  // The wants only while a mirror big enough to show the city has been seen lately (cmc::CityHeld): no structures, hit data or textures are kept for a mirror of a few pixels.
  if (world_.wanted != cmc::Level::kOff && service_ != nullptr && cmc::CityHeld(frame_, city_seen_, city_seen_frame_)) {
    service_->Want(rt_scene::Consumer::kCityMirror, cmc::NeedsFor(world_));
  }
}

bool CarMirrorRt::NotRun(cm::Why why) {
  ++window_.not_run[size_t(why)];
  return false;
}

void CarMirrorRt::OnPresent() {
  if (ready_ && image_.texture && !recorded_this_frame_ && ++idle_frames_ >= cm::kReleaseAfterFrames) {
    ReleaseImage();  // a pass wanted but with no mirror on the picture (or off) for a while gives its image back
  }
  recorded_this_frame_ = false;
  ++frame_;
  // A wanted pass that never ran says why now and then (a car with no mirror pair, a mirror off the picture): a line every half minute at 60 FPS, none while the setting is off.
  if (frame_ >= next_log_frame_) {
    next_log_frame_ = frame_ + kQuietLogFrames;
    uint64_t reasons = 0;
    for (size_t i = size_t(cm::Why::kNoStructures); i < size_t(cm::Why::kCount); ++i) {
      reasons += window_.not_run[i];
    }
    if (window_.passes != 0 || reasons != 0) {
      LogWindow();
    }
  }
}

void CarMirrorRt::LogWindow() {
  const Window& w = window_;
  std::string reasons;
  for (size_t i = size_t(cm::Why::kSettingOff); i < size_t(cm::Why::kCount); ++i) {
    if (w.not_run[i] != 0) {
      reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", cm::WhyName(cm::Why(i)), w.not_run[i]);
    }
  }
  std::string rects;
  for (size_t i = size_t(cm::Why::kOffScreen); i < size_t(cm::Why::kNotReady); ++i) {
    if (w.rect_skipped[i] != 0) {
      rects += std::format("{}{} {}", rects.empty() ? "" : ", ", cm::WhyName(cm::Why(i)), w.rect_skipped[i]);
    }
  }
  const double n = double(std::max<uint32_t>(w.passes, 1));
  REXLOG_INFO("{} frames {}..{}: {} passes ({} with the city, the rest with the environment map), {:.2f} mirrors a pass, {:.0f} pixels a pass; frames not run: {}; mirrors left to the game: {}", kPrefix,
              w.first_frame, frame_, w.passes, w.city_passes, double(w.mirrors) / n, double(w.pixels) / n, reasons.empty() ? "none" : reasons, rects.empty() ? "none" : rects);
  window_ = Window{};
  window_.first_frame = frame_;
  next_log_frame_ = frame_ + kQuietLogFrames;
}

bool CarMirrorRt::OnResolve(const ResolveCache::Summary& summary, plume::RenderCommandList* list, int32_t exp_bias) {
  if (!ready_ || !list || !active_ || !scene_ || !summary.depth_copy || !summary.source) {
    return false;
  }
  const CarRtView& view = scene_->View();
  if (!view.valid || !view.set) {
    return NotRun(cm::Why::kNoStructures);
  }
  if (view.mirror.count != cm::kMaxMirrors) {
    return NotRun(cm::Why::kNoMirror);
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (!temporal::IsPlaySamples(samples)) {
    return NotRun(cm::Why::kPauseView);  // the pause menu's world and the garage: the game's mirrors
  }
  if (!(view.tan_x > 0.0) || !(view.tan_y > 0.0)) {
    return NotRun(cm::Why::kNoLens);
  }
  EdramTargets::Target& depth = *summary.source;
  const uint32_t width = std::min(summary.dest_width, depth.width);
  const uint32_t height = std::min(summary.dest_height, depth.height);
  const uint32_t render_w = depth.scale.Edge(width);
  const uint32_t render_h = depth.scale.Edge(height);
  if (render_w < kMinRenderSize || render_h < kMinRenderSize) {
    return NotRun(cm::Why::kOffScreen);
  }

  // where the mirrors are on the picture
  const cm::Limits limits = cm::LimitsFor(gpu_class::CurrentRtClass(), int(larecomp::settings::mcla_gpu_car_mirror_rt_pixels.Get()));
  cm::RectResult rects[cm::kMaxMirrors];
  uint32_t used = 0;
  cm::Why first_reason = cm::Why::kOffScreen;
  for (uint32_t k = 0; k < cm::kMaxMirrors; ++k) {
    rects[k] = cm::RectFor(view.mirror_box[k], view.tan_x, view.tan_y, double(render_w), double(render_h), limits);
    if (rects[k].why == cm::Why::kUsed) {
      ++used;
    } else {
      if (used == 0 && k == 0) {
        first_reason = rects[k].why;
      }
      ++window_.rect_skipped[size_t(rects[k].why)];
    }
  }
  if (used == 0) {
    return NotRun(first_reason);
  }
  // MIRROR-CITY: is the larger mirror big enough to show the city? Then the city is traced in this pass and its wants are kept for the next kCityHoldFrames frames (UpdateWorld).
  uint64_t largest = 0;
  for (uint32_t k = 0; k < cm::kMaxMirrors; ++k) {
    if (rects[k].why == cm::Why::kUsed) {
      largest = std::max<uint64_t>(largest, rects[k].rect.Area());
    }
  }
  const bool big_enough = cmc::BigEnough(largest);
  if (big_enough) {
    city_seen_ = true;
    city_seen_frame_ = frame_;
  }

  // the pipelines and the targets
  uint32_t apply_index = 0;
  plume::RenderPipeline* pipeline = Pipeline();
  plume::RenderPipeline* apply = ApplyIndexOf(depth.host_samples, &apply_index) ? ApplyPipeline(apply_index) : nullptr;
  if (!pipeline || !apply) {
    return NotRun(cm::Why::kNotReady);
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply, car_selfrefl.cpp ColourTarget).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples || found->scale != depth.scale) {
    return NotRun(cm::Why::kNoImage);
  }
  if (!EnsureImage()) {
    return NotRun(cm::Why::kNoImage);
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour || image_.slot == kNoSlot) {
    return NotRun(cm::Why::kNoImage);
  }
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = nullptr;  // made below, after the trace: the colour is only touched by the apply

  // THE TRACE
  Push push = {};
  push.image[2] = cm::kPrimaryMask;
  push.image[3] = cm::kReflectionMask;
  push.lens[0] = float(view.tan_x);
  push.lens[1] = float(view.tan_y);
  push.lens[2] = float(render_w);
  push.lens[3] = float(render_h);
  push.tune[0] = float(std::clamp(larecomp::settings::mcla_gpu_car_mirror_rt_strength.Get(), 0.0, 1.0));
  push.tune[1] = limits.reach;
  push.tune[2] = cm::kFadeFrom;
  push.tune[3] = car_selfrefl::ExpBiasUnits(exp_bias);
  // MIRROR-CITY: the city is traced where this frame's world says so; the lamp term and the world frame's light follow the reflections' own two debug switches (the same city hit, lit the same way)
  push.city[0] = cmc::FlagsFor(world_, big_enough, larecomp::settings::mcla_gpu_city_refl_lamps.Get(), larecomp::settings::mcla_gpu_city_refl_world_light.Get());
  std::memcpy(&push.city[1], &world_.cell.reach, sizeof(float));
  std::memcpy(&push.city[2], &world_.cell.fade_from, sizeof(float));
  const plume::RenderTextureBarrier to_write(image_.texture.get(), plume::RenderTextureLayout::GENERAL);
  pass_timing_->Mark(list, "car mirror rt");
  list->barriers(plume::RenderBarrierStage::COMPUTE, nullptr, 0, &to_write, 1);
  image_.layout = plume::RenderTextureLayout::GENERAL;
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), kTextureSet);
  list->setComputeDescriptorSet(set_.get(), kOutputSet);
  list->setComputeDescriptorSet(view.set, kSceneSet);
  list->setPipeline(pipeline);
  uint64_t pixels = 0;
  for (uint32_t k = 0; k < cm::kMaxMirrors; ++k) {
    if (rects[k].why != cm::Why::kUsed) {
      continue;
    }
    const car_rt_opp_refl::TexelRect& r = rects[k].rect;
    push.rect[0] = uint32_t(r.x0);
    push.rect[1] = uint32_t(r.y0);
    push.rect[2] = uint32_t(r.Width());
    push.rect[3] = uint32_t(r.Height());
    push.image[0] = cm::ImageX(k);
    push.image[1] = 0;
    list->setComputePushConstants(0, &push, 0, sizeof(push));
    list->dispatch(car_rt_opp_refl::GroupsX(r, cm::kGroup), car_rt_opp_refl::GroupsY(r, cm::kGroup), 1);
    pixels += r.Area();
  }

  // THE APPLY: the image is written by the trace (storage) and read here as a texture; the main colour is a colour attachment in the middle of the frame (the occlusion's and the glass
  // reflections' way: no depth, the next draw of the game moves things back itself).
  const plume::RenderTextureBarrier read(image_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, &read, 1);
  image_.layout = plume::RenderTextureLayout::SHADER_READ;
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    ++window_.not_run[size_t(cm::Why::kNoImage)];
    return true;  // the trace was recorded: the caller's list state is gone
  }
  pass_timing_->Mark(list, "car mirror apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(render_w), float(render_h)};
  list->setViewports(&viewport, 1);
  passes_->Bind(list);
  list->setPipeline(apply);
  for (uint32_t k = 0; k < cm::kMaxMirrors; ++k) {
    if (rects[k].why != cm::Why::kUsed) {
      continue;
    }
    const car_rt_opp_refl::TexelRect& r = rects[k].rect;
    const plume::RenderRect scissor(r.x0, r.y0, r.x1, r.y1);
    list->setScissors(&scissor, 1);
    ApplyPush apply_push = {};
    apply_push.image[0] = image_.slot;
    apply_push.image[1] = cm::ImageX(k);
    apply_push.image[2] = 0;
    apply_push.rect[0] = uint32_t(r.x0);
    apply_push.rect[1] = uint32_t(r.y0);
    apply_push.rect[2] = uint32_t(r.x1);
    apply_push.rect[3] = uint32_t(r.y1);
    list->setGraphicsPushConstants(0, &apply_push, 0, sizeof(apply_push));
    list->drawInstanced(3, 1, 0, 0);
  }
  list->setFramebuffer(nullptr);
  // The closing mark (PassTiming: a mark's time runs to the next mark): what is recorded next is not the apply's.
  pass_timing_->Mark(list, "after car mirror");

  // the totals and the log
  recorded_this_frame_ = true;
  idle_frames_ = 0;
  ++passes_run_;
  ++window_.passes;
  window_.mirrors += used;
  window_.pixels += pixels;
  window_.city_passes += push.city[0] != 0 ? 1u : 0u;
  if (!pair_logged_) {
    pair_logged_ = true;
    const car_rt_instances::ViewBox& a = view.mirror_box[0];
    const car_rt_instances::ViewBox& b = view.mirror_box[1];
    REXLOG_INFO("{} the car's mirror glasses are instances {} and {} of the top level (boxes in view space: ({:.2f} {:.2f} {:.2f}) to ({:.2f} {:.2f} {:.2f}) and ({:.2f} {:.2f} {:.2f}) to "
                "({:.2f} {:.2f} {:.2f}); the pair's score {:.2f}: two small flat patches across the car, the same size and height, facing the same way and tilted towards each other)",
                kPrefix, view.mirror.item[0], view.mirror.item[1], double(a.min[0]), double(a.min[1]), double(a.min[2]), double(a.max[0]), double(a.max[1]), double(a.max[2]),
                double(b.min[0]), double(b.min[1]), double(b.min[2]), double(b.max[0]), double(b.max[1]), double(b.max[2]), double(view.mirror.score));
  }
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first pass, frame {}: {} of the 2 mirrors drawn, rectangles ({}x{} and {}x{} pixels) of the {}x{} picture, the cars' reach {:.0f} m, strength {:.2f}, cap {} pixels a mirror, "
                "main colour of {} host samples, the world {}; timed as 'car mirror rt' and 'car mirror apply'",
                kPrefix, frame_, used, rects[0].rect.Width(), rects[0].rect.Height(), rects[1].rect.Width(), rects[1].rect.Height(), render_w, render_h, double(limits.reach),
                double(push.tune[0]), limits.max_pixels, depth.host_samples, push.city[0] != 0 ? "the city" : "the environment map (the city: not asked, not ready or the mirror too small)");
  }
  if (window_.passes >= cm::kLogEveryPasses) {
    LogWindow();
  }
  return true;
}

}  // namespace mcla::gpu
