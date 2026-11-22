// MCLA GPU renderer -- the deferred local light pass. See local_lights.h; the rules (the lights taken, the camera, the units, the scissor, the buffer) are in local_lights_rules.h and tested
// without a renderer, the pixel maths in hlsl/local_light.hlsli and hlsl/local_light_ps.hlsl.

#include "local_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "ao.h"
#include "descriptors.h"
#include "device.h"
#include "gbuffer.h"
#include "gpu_class.h"
#include "headlight_rig.h"
#include "lamp_shadows.h"
#include "host_shaders.h"
#include "light_manager.h"
#include "live_text.h"
#include "local_shadow_rules.h"
#include "mod_light_source.h"
#include "pass_timing.h"
#include "records.h"
#include "rt_lights.h"
#include "screen_shared.h"
#include "temporal_identities.h"

namespace mcla::gpu {

namespace ll = larecomp::gpu::local_lights;
namespace hp = larecomp::gpu::headlight_pass;
namespace mls = larecomp::gpu::mod_light_source;

namespace {

constexpr const char* kPrefix = "[local-lights]";
constexpr uint32_t kIntervalFrames = 600;
// The registers the units come from, as the translated lit shaders name them (xCityDefault__PSMultiLight: ShaderGlobals packoffset(c63), OcclusionInfo packoffset(c56)): PS float constants.
constexpr uint32_t kPsOcclusionInfo = 56;
constexpr uint32_t kPsShaderGlobals = 63;
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);

const char* SkipName(uint32_t why) {
  switch (why) {
    case 0: return "no G-buffer planes";
    case 1: return "no camera";
    case 2: return "no colour target";
    case 3: return "no lights";
    case 4: return "no pipeline";
    case 5: return "no buffer";
    default: return "recorded";
  }
}

// The budgets' tier: the light shadows' level (the pause menu's LIGHT SHADOWS, `auto` from the GPU class), by its word: local_lights_sources.h TierOfWord. The pass has no level of its own
// (LIGHT-UNIFY Q3: LIGHT SHADOWS covers headlights and lamps).
larecomp::gpu::local_lights::Tier TierNow() {
  static LiveText text("mcla_gpu_light_shadows");
  const local_shadow::Choice choice = local_shadow::Resolve(gpu_class::Current(), text.Get());
  return ll::TierOfWord(local_shadow::LevelName(choice.level));
}

// The empty frame the pass reads when the manager is not running (a LightFrame is about 12 KB: made once).
const larecomp::gpu::LightFrame& EmptyFrame() {
  static const larecomp::gpu::LightFrame frame{};
  return frame;
}

}  // namespace

LocalLights::~LocalLights() = default;

void LocalLights::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  resources_failed_ = false;
  pipelines_failed_ = false;
}

void LocalLights::Shutdown() {
  // The GPU is idle and the FrameRing drained: the framebuffers the draws used belong to the EDRAM's cache, the rest goes at once.
  pipelines_.clear();
  shaders_ = {};
  for (Slot& slot : slots_) {
    slot.set.reset();
    slot.buffer.reset();
  }
  layout_.reset();
  gbuffer_ = nullptr;
  screen_ = nullptr;
  ao_ = nullptr;
  manager_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  enabled_ = false;
  planes_wanted_ = false;
  route_ = hp::Route::kForward;
}

uint64_t LocalLights::HeldBytes() const {
  uint64_t bytes = 0;
  for (const Slot& slot : slots_) {
    bytes += slot.buffer ? ll::kBufferAlloc : 0u;
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

bool LocalLights::EnsureResources() {
  if (layout_ && shaders_[0] && shaders_[1]) {
    return true;
  }
  if (resources_failed_ || !device_ || !passes_ || !heap_) {
    return false;
  }
  // The host passes' four bindless texture sets (spaces 0-3: the planes and the depth, plain and multisampled), the lights' buffer (space 4), the push block (b0, space 5) for the pixel
  // stage: the layout of the fused resolve (resolve_memory.cpp), with this pass's set in place of the memory.
  set_builder_.begin();
  set_builder_.addByteAddressBuffer(0);  // t0, space 4: the header and the lights
  set_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap_->texture_set_builder());
  }
  builder.addDescriptorSet(set_builder_);
  builder.addPushConstant(0, 5, sizeof(ll::PassPush), plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "local lights layout");
  bool ok = layout_ != nullptr;
  for (uint32_t i = 0; ok && i < 2; ++i) {
    const HostShaderBlob blob = LocalLightPixelShader(i != 0);
    shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("{} the layout or a shader could not be made: no local light pass this session", kPrefix);
    shaders_ = {};
    layout_.reset();
    resources_failed_ = true;
    return false;
  }
  return true;
}

bool LocalLights::EnsureSlot(uint32_t frame_slot) {
  if (frame_slot >= kFramesInFlight || !device_) {
    return false;
  }
  Slot& slot = slots_[frame_slot];
  if (slot.set) {
    return true;
  }
  slot.buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(ll::kBufferAlloc, plume::RenderBufferFlag::STORAGE), "local lights buffer");
  if (!slot.buffer) {
    return false;
  }
  slot.set = device_->CreateDescriptorSet(set_builder_, 1, 0, "local lights set");
  if (!slot.set) {
    slot.buffer.reset();
    return false;
  }
  slot.set->setBuffer(0, slot.buffer.get(), ll::kBufferAlloc);
  return true;
}

plume::RenderPipeline* LocalLights::PipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (PipelineEntry& entry : pipelines_) {
    if (entry.format == format && entry.samples == samples) {
      return entry.pipeline.get();
    }
  }
  if (pipelines_failed_ || !layout_ || !shaders_[samples > 1 ? 1 : 0]) {
    return nullptr;
  }
  // ADD (ONE, ONE) on the colour, the alpha never written; no depth. Made here, on first use (a short stutter: the first frame with the pass on).
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::ONE;
  blend.dstBlend = plume::RenderBlend::ONE;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  PipelineEntry entry;
  entry.format = format;
  entry.samples = samples;
  entry.pipeline = passes_->CreatePipeline(*layout_, *shaders_[samples > 1 ? 1 : 0], format, blend, samples, "local lights");
  if (!entry.pipeline) {
    pipelines_failed_ = true;
    REXLOG_ERROR("{} the pipeline could not be made ({} host samples): no local light pass this session (reported once)", kPrefix, samples);
    return nullptr;
  }
  plume::RenderPipeline* raw = entry.pipeline.get();
  pipelines_.push_back(std::move(entry));
  return raw;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void LocalLights::BeginFrame() {
  namespace st = larecomp::settings;
  // GB-H2: the headlight route (headlight_pass_rules.h): `pass` and `both` give this pass the frame's headlights and run it whatever mcla_gpu_local_lights says.
  const hp::Route route = hp::ParseRoute(st::mcla_gpu_headlight_pass.Get());
  // LAMP-SHADOWS-4: the street lamps' shadows are drawn by this pass. mcla_gpu_lamp_shadows -1 (auto, the default) follows the LIGHT SHADOWS row while the pass runs for anything else (the provider is told
  // whether it does), 1 runs the pass for the lamps (and wants the planes) whatever mcla_gpu_local_lights says, 0 is off.
  if (lamp_shadows_ != nullptr) {
    lamp_shadows_->SetPassRuns(hp::PassOn(route, st::mcla_gpu_local_lights.Get()));
  }
  const bool lamps_on = lamp_shadows_ != nullptr && lamp_shadows_->active();  // lamps are live now (enabled, by night): by day the pass and the G-buffer's planes cost nothing for them
  const bool setting_on = st::mcla_gpu_local_lights.Get() || lamps_on;
  const bool on = hp::PassOn(route, setting_on);
  sources_ = hp::SourcesFor(route, uint32_t(std::clamp<int>(int(st::mcla_gpu_local_lights_sources.Get()), 0, int(ll::kSourceMask))) & ll::kSourceMask);
  ab_ = ll::AbOf(int(st::mcla_gpu_local_lights_ab.Get()));
  gain_ = float(std::clamp(double(st::mcla_gpu_local_lights_gain.Get()), 0.0, 16.0));
  car_gain_ = float(std::clamp(double(st::mcla_gpu_local_lights_car_gain.Get()), 0.0, 16.0));
  tier_ = on ? TierNow() : ll::Tier::kMedium;
  ++frame_;
  if (on != enabled_) {
    enabled_ = on;
    if (on) {
      REXLOG_INFO("{} on (mcla_gpu_local_lights {}, headlight route {}; live): at the mid-scene depth copy, after the occlusion's apply, one scissored additive draw per light from the G-buffer's normal and albedo "
                  "and the main depth; sources {} (1 the frame's surface lights, 2 a test lamp, 4 police, 8 the cars' head lamps, 16 their rear lamps, 32 the mods' lamps outside their grid), "
                  "lamp A/B {} (0 grid, 1 nowhere, 2 pass), gain {:.2f}, the cars' look x {:.2f}, budget tier {} (frame {}, police {}, head lamps {}, rear lamps {}, mod lamps {}; 32 in all)",
                  kPrefix, setting_on ? "on" : "off", hp::RouteName(route), sources_, int(ab_), gain_, car_gain_, ll::TierName(tier_), ll::BudgetOf(tier_).frame, ll::BudgetOf(tier_).police,
                  ll::BudgetOf(tier_).head, ll::BudgetOf(tier_).rear, ll::BudgetOf(tier_).mod);
      EnsureResources();  // the pipelines are made on the first frame that records
    } else {
      REXLOG_INFO("{} off", kPrefix);
    }
  }
  if (route != route_) {
    route_ = route;
    REXLOG_INFO("{} headlight route {} (mcla_gpu_headlight_pass; live): the forward headlight stage (the lit shaders' variant) is {}, the pass {} the frame's headlights{}", kPrefix, hp::RouteName(route),
                hp::ForwardStageOn(route) ? "on" : "OFF", hp::PassLightsHeadlights(route) ? "lights" : "does not light",
                hp::PassLightsHeadlights(route) ? " with their shadows from the atlas (the one lookup)" : "");
  }
  // The wants (sticky; the G-buffer and the shared products read them at their own BeginFrame, which the Scene runs after this): with the pass off nothing asks for any plane or product,
  // so no sidecar is made and the AO's depth chain runs for nobody. A route that gives the pass the headlights (GB-H2) wants them only while HEADLIGHTS is on (a rig with no light has nothing for
  // the pass to add, and a car passing in and out of the rig's four must not make the sidecar again): the setting's own use wants them always, as before.
  planes_wanted_ = enabled_ && (setting_on || headlight_rig::SettingOn());
  if (gbuffer_ != nullptr) {
    gbuffer_->Want(gbuffer::Consumer::kLocalLights, planes_wanted_ ? gbuffer::ConsumerPlanes(gbuffer::Consumer::kLocalLights) : 0u);
  }
  if (screen_ != nullptr) {
    screen_->Want(screen::Consumer::kLocalLights, planes_wanted_ ? screen::ProductBit(screen::kLinDepth) : 0u);
  }
  if (manager_ != nullptr) {
    manager_->Want(planes_wanted_ && (sources_ & (ll::kSourceFrame | ll::kSourceCars)) != 0);
  }
  if (rt_lights_ != nullptr) {
    rt_lights_->BeginFrame(planes_wanted_, tier_ == ll::Tier::kUltra);   // RT-LIGHTS-D1: reads its settings and asks for the structures while it is wanted
  }
  votes_.Reset();
  trigger_seen_ = false;
}

bool LocalLights::rt_lights_wanted() const {
  return rt_lights_ != nullptr && rt_lights_->wanted();
}

void LocalLights::SetRtRawMode(bool raw) {
  if (rt_lights_ != nullptr) {
    rt_lights_->SetRawMode(raw);
  }
}

void LocalLights::OnMainDraw(const ConstantBanks::Banks& banks, bool colour) {
  if (!planes_wanted_ || !colour || trigger_seen_) {
    return;
  }
  // The registers the units come from: every colour draw of the main view before the trigger votes with what its banks hold (a draw whose shader does not use them leaves what an earlier one set).
  const uint8_t* ps = banks.ps.data();
  votes_.Add({LoadBe32(ps + size_t(kPsShaderGlobals) * 16 + 12), LoadBe32(ps + size_t(kPsOcclusionInfo) * 16), LoadBe32(ps + size_t(kPsOcclusionInfo) * 16 + 4),
              LoadBe32(ps + size_t(kPsOcclusionInfo) * 16 + 8)});
}

bool LocalLights::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot) {
  (void)record;
  if (!planes_wanted_ || !list || trigger_seen_ || !gbuffer_published || gbuffer_ == nullptr || screen_ == nullptr || edram_ == nullptr || !summary.source) {
    return false;
  }
  trigger_seen_ = true;  // once a frame
  const gbuffer::GBufferFrame& gb = gbuffer_->frame();
  if (!gb.Has(gbuffer::kPlaneNormal) || !gb.Has(gbuffer::kPlaneAlbedo)) {
    Count(Skip::kNoPlanes);
    return false;
  }

  // The camera: the frame's own pose where the AO trusts the vertex constants for it, else the last frame's; the lens and the projection's depth rows from the shared products' camera.
  const screen::ScreenFrame& sf = screen_->Frame();
  if ((sf.camera.flags & screen::kCameraValid) == 0 || sf.frame_serial != gb.frame_serial || (sf.flags & screen::kFramePause) != 0 || !(sf.camera.tan_half_fov[0] > 0.0f) ||
      !(sf.camera.tan_half_fov[1] > 0.0f)) {
    Count(Skip::kNoCamera);
    return false;
  }
  float pose[16];
  bool own_pose = false;
  if (ao_ == nullptr || !ao_->TriggerViewInverse(pose, &own_pose)) {
    std::memcpy(pose, sf.camera.view_inverse, sizeof(pose));
  }
  ll::Rows rows;
  if (!ll::RowsFromViewInverse(pose, &rows)) {
    Count(Skip::kNoCamera);
    return false;
  }
  // The manager keeps the cars nearest to the camera (eight at most): this frame's camera is its reference at the present that follows (the cars the next frame's pass reads).
  if (manager_ != nullptr && (sources_ & ll::kSourceCars) != 0) {
    const double reference[3] = {double(rows.position[0]), double(rows.position[1]), double(rows.position[2])};
    manager_->SetCarReference(reference);
  }

  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply, gbuffer.cpp DrawView).
  EdramTargets::Target& depth = *summary.source;
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != gb.host_samples || depth.host_samples != gb.host_samples || found->scale.Edge(found->width) != gb.width ||
      found->scale.Edge(found->height) != gb.height) {
    Count(Skip::kNoTarget);
    return false;
  }

  // The lights: the frame's (when the sources say so), the lamp A/B's, the test lamp, then GB-H3's classes: the cars' police, head and rear lamps (the manager's last poll of the light points),
  // the mods' lamps the game's grid is not lighting. The classes' lights that no pixel of the picture is inside the reach of are left out BEFORE the budgets count them.
  extras_.clear();
  const std::shared_ptr<const mls::Set> active_mod = mls::Global().Current();
  ll::HandedOverLamps(ab_, active_mod.get(), rows.position, &extras_);
  if ((sources_ & ll::kSourceTest) != 0) {
    extras_.push_back(ll::MakeTestLamp(rows));
  }
  const uint32_t extras_before_lamps = uint32_t(extras_.size());
  if (lamp_shadows_ != nullptr) {
    lamp_shadows_->AppendPassLights(&extras_);  // the picked street lamps with views (this trigger's: the replay recorded them just before)
  }
  const uint32_t lamp_extras = uint32_t(extras_.size()) - extras_before_lamps;
  if (rt_lights_ != nullptr && rt_lights_->wanted()) {
    // RT-LIGHTS-D1: the nearest street lamps the atlas did not pick, as lights the game lights with no views: they are in the pass only to get a ray (a lamp not picked for a channel is trimmed below)
    rt_lights_->AppendLampLights(rows.position, &extras_);
  }
  const bool manager_on = manager_ != nullptr && manager_->active();
  const larecomp::gpu::LightFrame& frame = manager_on ? manager_->Frame() : EmptyFrame();
  candidates_.Clear();
  if (manager_on && (sources_ & ll::kSourceCars) != 0) {
    ll::CarSourceInputs cars_in;
    cars_in.cars = &manager_->Cars();
    cars_in.frame = &frame;
    cars_in.sources = sources_;
    cars_in.tier = tier_;
    cars_in.gain = car_gain_;
    for (int i = 0; i < 3; ++i) {
      cars_in.camera[i] = rows.position[i];
    }
    ll::GatherCarLights(cars_in, &candidates_);
    cars_seen_ += cars_in.cars->count;
  }
  if ((sources_ & ll::kSourceMod) != 0 && larecomp::settings::mod_lights.Get()) {
    ll::GatherModLamps(mls::Global().Built(), active_mod.get(), ab_, rows.position, &candidates_.mod);
  }
  const auto unseen = [&](const larecomp::gpu::Light& l) {
    return ll::ScissorOfLight(l, rows, sf.camera.tan_half_fov[0], sf.camera.tan_half_fov[1], gb.width, gb.height).Empty();
  };
  std::erase_if(candidates_.police, unseen);
  std::erase_if(candidates_.head, unseen);
  std::erase_if(candidates_.rear, unseen);
  std::erase_if(candidates_.mod, unseen);
  ll::Selection selection = ll::SelectPassLights(frame, sources_, extras_.data(), uint32_t(extras_.size()), candidates_, tier_);
  bool rt_ran = false;
  if (rt_lights_ != nullptr && rt_lights_->wanted()) {
    // RT-LIGHTS-D1: the top K of the lights the pass is going to draw get a channel of the ray traced visibility; the selection is annotated (the flag and the channel) and trimmed (a game's lamp with no source
    // of visibility is taken out: the pass would ADD it on top of the grid's). With the pass not running this frame the selection is only trimmed.
    RtLights::Inputs rin;
    rin.frame = frame_;
    rin.frame_slot = frame_slot;
    for (int i = 0; i < 3; ++i) {
      rin.position[i] = rows.position[i];
      rin.right[i] = rows.right[i];
      rin.up[i] = rows.up[i];
      rin.forward[i] = rows.forward[i];
    }
    rin.tan_x = sf.camera.tan_half_fov[0];
    rin.tan_y = sf.camera.tan_half_fov[1];
    rin.traced[0] = sf.traced[0];
    rin.traced[1] = sf.traced[1];
    rin.render[0] = gb.width;
    rin.render[1] = gb.height;
    rin.lin_slot = sf.lin_depth_slot[0];
    rt_ran = rt_lights_->Plan(rin, &selection);
  }
  if (selection.count == 0) {
    Count(Skip::kNoLights);
    return false;
  }
  std::array<ll::PixelRect, ll::kMaxPassLights> rects;
  uint32_t visible = 0;
  for (uint32_t i = 0; i < selection.count; ++i) {
    rects[i] = ll::ScissorOfLight(selection.lights[i], rows, sf.camera.tan_half_fov[0], sf.camera.tan_half_fov[1], gb.width, gb.height);
    visible += rects[i].Empty() ? 0u : 1u;
  }
  if (visible == 0) {
    Count(Skip::kNoLights);  // every light is off the picture
    return false;
  }

  plume::RenderPipeline* pipeline = EnsureResources() ? PipelineFor(found->format, gb.host_samples) : nullptr;
  if (!pipeline) {
    Count(Skip::kNoPipeline);
    if (!no_pipeline_logged_) {
      no_pipeline_logged_ = true;
      REXLOG_WARN("{} frame {}: no layout, shader or pipeline: the pass does not run (reported once)", kPrefix, frame_);
    }
    return false;
  }
  if (!EnsureSlot(frame_slot)) {
    Count(Skip::kNoBuffer);
    return false;
  }

  // RT-LIGHTS-D1: the ray traced visibility is traced and filtered now, outside any render pass (the colour is drawn into after it), so that the buffer below can name the images the pixel shader reads.
  if (rt_ran) {
    pass_timing_->Mark(list, "rt lights");
    rt_lights_->RecordTrace(list);
    pass_timing_->Mark(list, "rt lights denoise");
    rt_lights_->RecordDenoise(list);
    pass_timing_->Mark(list, "after rt lights");
  }

  // The units from the frame's vote and the buffer.
  std::array<uint32_t, 4> bits{};
  const bool voted = votes_.Best(&bits);
  ll::PassInputs in;
  in.rows = rows;
  in.tan_x = sf.camera.tan_half_fov[0];
  in.tan_y = sf.camera.tan_half_fov[1];
  for (int i = 0; i < 4; ++i) {
    in.lin[i] = sf.camera.lin[i];
  }
  in.width = gb.width;
  in.height = gb.height;
  in.units = voted ? ll::UnitsFromBits(bits, gain_) : ll::Units{};
  in.units.gain = gain_;
  in.units.exposure = (std::isfinite(sf.camera.exposure_bias) && sf.camera.exposure_bias > 0.0f) ? sf.camera.exposure_bias : 1.0f;
  in.flags = gb.Has(gbuffer::kPlaneMaterial) ? ll::kFlagMaterial : 0u;
  in.shadow = &frame.shadow;  // GB-H2: the replay's maps of this frame (recorded just before: scene.cpp OnResolve) and their views, which the frame's lights name
  if (lamp_shadows_ != nullptr) {
    in.lamp_shadow = &lamp_shadows_->Frame();  // LAMP-SHADOWS-4: the street lamps' views of this trigger, which the lamps' lights name (the combined entry space)
    in.debug = lamp_shadows_->debug_view();
  }
  in.rt_mask = (rt_ran && rt_lights_ != nullptr && rt_lights_->Masks().valid) ? &rt_lights_->Masks() : nullptr;
  const bool lamp_table = ll::LampTableNeeded(selection, in.lamp_shadow);
  const bool shadow_table = ll::ShadowTableNeeded(selection, in.shadow);
  ll::BuildBuffer(in, selection, scratch_.data());
  Slot& slot = slots_[frame_slot];
  void* mapped = slot.buffer->map();
  if (mapped == nullptr) {
    Count(Skip::kNoBuffer);
    return false;
  }
  std::memcpy(mapped, scratch_.data(), ll::kBufferAlloc);
  slot.buffer->unmap();

  // The colour is drawn into; the depth plane, the G-buffer's planes (published in the shader-read layout) are read. After the pass the game's next draws bind their own layouts again.
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    Count(Skip::kNoTarget);
    return false;
  }
  edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    Count(Skip::kNoTarget);
    return false;
  }
  pass_timing_->Mark(list, "local lights");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(gb.width), float(gb.height)};
  list->setViewports(&viewport, 1);
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(slot.set.get(), 4);
  list->setPipeline(pipeline);
  ll::PassPush push = {};
  push.slots[0] = summary.sample_slot;
  push.slots[1] = gb.slot[gbuffer::kPlaneNormal];
  push.slots[2] = gb.slot[gbuffer::kPlaneAlbedo];
  push.slots[3] = gb.Has(gbuffer::kPlaneMaterial) ? gb.slot[gbuffer::kPlaneMaterial] : larecomp::gpu::kNoLightSlot;
  push.info[2] = gb.sample0;
  uint32_t drawn = 0;
  for (uint32_t i = 0; i < selection.count; ++i) {
    if (rects[i].Empty()) {
      continue;
    }
    const plume::RenderRect scissor(rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1);
    list->setScissors(&scissor, 1);
    push.info[0] = i;
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
    ++drawn;
    pixels_scissored_ += rects[i].Area();
  }
  list->setFramebuffer(nullptr);
  pass_timing_->Mark(list, "after local lights");

  Count(Skip::kRecorded);
  lights_drawn_ += drawn;
  if (lamp_table) {
    ++lamp_frames_;
    lamp_lights_ += lamp_extras;
  }
  if (shadow_table) {
    ++shadow_frames_;
    for (uint32_t i = 0; i < selection.count; ++i) {
      const larecomp::gpu::ShadowRef& ref = selection.lights[i].shadow;
      if (!rects[i].Empty() && ref.kind == larecomp::gpu::ShadowKind::kAtlas && ref.view_count != 0 && uint32_t(ref.first_view) < frame.shadow.count) {
        ++shadowed_lights_;
        shadow_views_ += ref.view_count;
      }
    }
  }
  for (uint32_t c = 0; c < ll::kSourceClassCount; ++c) {
    class_drawn_[c] += selection.from_class[c];
    class_cut_[c] += selection.cut_class[c];
  }
  frames_own_pose_ += own_pose ? 1u : 0u;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} frame {}: the first run: {} lights drawn of {} ({} from the frame, {} of them with views of the shadow atlas ({}); {} handed over: lamp A/B {} / test lamp {}; police {}, head lamps {}, rear lamps {}, mod lamps {}), "
                "the picture {}x{} at {} host samples, G-buffer planes {} "
                "(RT3 {}), the camera {} (lens {:.3f} x {:.3f}), the units: night {:.3f}, height factor {}, exposure {:.1f}, gain {:.2f} ({} colour draws voted, {} distinct values)",
                kPrefix, frame_, drawn, selection.count, selection.from_frame, shadowed_lights_, shadow_table ? "the table is in the buffer" : "no table: no atlas or no views", selection.from_extra, int(ab_),
                (sources_ & ll::kSourceTest) != 0 ? "on" : "off",
                selection.from_class[size_t(ll::SourceClass::kPolice)], selection.from_class[size_t(ll::SourceClass::kHeadLamps)], selection.from_class[size_t(ll::SourceClass::kRearLamps)],
                selection.from_class[size_t(ll::SourceClass::kMod)], gb.width, gb.height,
                gb.host_samples, gb.valid, gb.Has(gbuffer::kPlaneMaterial) ? "there" : "not written yet", own_pose ? "the frame's own" : "the last frame's", in.tan_x, in.tan_y,
                in.units.night,
                in.units.occ[3] > 0.0f ? std::format("on (x {:.1f} y {:.4f} z {:.3f})", in.units.occ[0], in.units.occ[1], in.units.occ[2]) : std::string("not known"), in.units.exposure, in.units.gain,
                votes_.draws(), votes_.distinct());
  }
  if (voted && (!units_logged_ || bits != last_units_bits_)) {
    units_logged_ = true;
    last_units_bits_ = bits;
    REXLOG_INFO("{} frame {}: the units' vote: night light intensity (PS c63.w) {:.3f}, OcclusionInfo (PS c56) {:.4f} {:.5f} {:.4f}; {} colour draws, {} distinct values{} (logged when it changes)", kPrefix, frame_,
                ll::BitsToFloat(bits[0]), ll::BitsToFloat(bits[1]), ll::BitsToFloat(bits[2]), ll::BitsToFloat(bits[3]), votes_.draws(), votes_.distinct(),
                votes_.overflow() != 0 ? std::format(" ({} draws had values past the table)", votes_.overflow()) : std::string());
  }
  return true;
}

void LocalLights::OnPresent() {
  if (rt_lights_ != nullptr) {
    rt_lights_->OnPresent();   // RT-LIGHTS-D1: the denoisers' frame boundary and the images' idle count (also while the pass is off: they go back)
  }
  if (!enabled_) {
    return;
  }
  ++frames_run_;
  if (frames_run_ % kIntervalFrames == 0) {
    LogInterval();
  }
}

void LocalLights::LogInterval() {
  std::string why;
  for (uint32_t i = 0; i < uint32_t(Skip::kCount); ++i) {
    if (counts_[i] != 0) {
      why += std::format("{}{} {}", why.empty() ? "" : ", ", SkipName(i), counts_[i]);
    }
  }
  std::string classes;
  for (uint32_t c = 0; c < ll::kSourceClassCount; ++c) {
    if (class_drawn_[c] != 0 || class_cut_[c] != 0) {
      classes += std::format("{}{} {} taken{}", classes.empty() ? "" : ", ", ll::SourceClassName(ll::SourceClass(c)), class_drawn_[c],
                             class_cut_[c] != 0 ? std::format(" ({} cut by the budget)", class_cut_[c]) : std::string());
    }
  }
  REXLOG_INFO("{} {} frames with the pass on: {} lights drawn ({}), {:.1f} MPixels scissored; the trigger frames: {}; the frame's own camera pose in {} frames; the manager held {:.1f} cars a frame; "
              "budget tier {}; headlight route {}, {} lights drawn with the shadow lookup ({} views) in {} frames with the table; street lamps: {} lights drawn with the lamps' table in {} frames; buffers {} B",
              kPrefix, kIntervalFrames, lights_drawn_, classes.empty() ? std::string("none by class") : classes, double(pixels_scissored_) * 1.0e-6, why.empty() ? std::string("none") : why,
              frames_own_pose_, double(cars_seen_) / double(kIntervalFrames), ll::TierName(tier_), hp::RouteName(route_), shadowed_lights_, shadow_views_, shadow_frames_, lamp_lights_, lamp_frames_,
              HeldBytes());
  counts_.fill(0);
  lights_drawn_ = 0;
  pixels_scissored_ = 0;
  frames_own_pose_ = 0;
  class_drawn_.fill(0);
  class_cut_.fill(0);
  cars_seen_ = 0;
  shadowed_lights_ = 0;
  shadow_views_ = 0;
  shadow_frames_ = 0;
  lamp_lights_ = 0;
  lamp_frames_ = 0;
}

}  // namespace mcla::gpu
