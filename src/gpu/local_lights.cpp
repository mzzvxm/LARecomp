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
namespace nl = larecomp::gpu::neon_light;

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

// GB-NEON: whether NEON LIGHT (mcla_gpu_neon_light, `auto` resolved for this graphics card: gpu_class.h Effect::kNeonLight) is on; a word that is no value counts as `auto`. Render thread.
bool NeonSettingOn() {
  static LiveText neon_text("mcla_gpu_neon_light");
  std::string mode = gpu_class::Resolve(gpu_class::Effect::kNeonLight, neon_text.Get());
  if (mode != "on" && mode != "off") {
    mode = std::string(gpu_class::Cell(gpu_class::Effect::kNeonLight, gpu_class::Current()));
  }
  return mode == "on";
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
  neon_on_ = false;
  neon_active_ = false;
  neon_hold_ = 0;
  nl::Want(false);  // the hook reads nothing for a pass that is gone
  nl::Global().Clear();
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

bool LocalLights::NeonLightReachesPicture() {
  if (!nl::Global().AnyLit(ll::kNeonMinColour)) {
    return false;  // no car with its neon lit (one lock, no copy): the usual frame of a player with no neon car about
  }
  // The camera of the last frame (the play view's: a pause or garage view, a cut, the first frame have none): a car is "in reach" when one of its lights passes the pass's own visibility test (the
  // scissor of its reach is not empty) against it, the lens widened (local_lights_rules.h kNeonProbeLensMargin). Without a camera a lit car alone asks, as before.
  const temporal::Camera* camera = screen_ != nullptr ? screen_->Cameras().Previous(screen_->frame_count(), false) : nullptr;
  ll::ProbeView view;
  if (camera == nullptr || !ll::ProbeViewOf(camera->right, camera->up, camera->forward, camera->position, camera->fov_y, camera->aspect, camera->width, camera->height, &view)) {
    return nl::LitNow(true, false, false);
  }
  nl::Global().Recent(&neon_cars_);
  return nl::LitNow(true, true, ll::NeonReachesView(neon_cars_, tier_, neon_gain_, view));
}

void LocalLights::BeginFrame() {
  namespace st = larecomp::settings;
  // GB-H2: the headlight route (headlight_pass_rules.h): `pass` and `both` give this pass the frame's headlights and run it whatever mcla_gpu_local_lights says.
  const hp::Route route = headlight_rig::CurrentRoute();  // read once for the frame by the Scene (headlight_rig::SampleRoute, through LiveText: the debug menu's thread writes the cvar)
  // LAMP-SHADOWS-4: the street lamps' shadows are drawn by this pass. mcla_gpu_lamp_shadows -1 (auto, the default) follows the LIGHT SHADOWS row while the pass runs for anything else (the provider is told
  // whether it does), 1 runs the pass for the lamps (and wants the planes) whatever mcla_gpu_local_lights says, 0 is off.
  if (lamp_shadows_ != nullptr) {
    lamp_shadows_->SetPassRuns(hp::PassOn(route, st::mcla_gpu_local_lights.Get()));
  }
  const bool lamps_on = lamp_shadows_ != nullptr && lamp_shadows_->active();  // lamps are live now (enabled, by night): by day the pass and the G-buffer's planes cost nothing for them
  const bool setting_on = st::mcla_gpu_local_lights.Get() || lamps_on;
  const bool on_for_lights = hp::PassOn(route, setting_on);
  // GB-NEON: NEON LIGHT runs the pass for the cars' underglow whatever the setting and the route say, and tells the hook whether anyone reads the game's neon (it reads nothing while off).
  const bool neon_now = NeonSettingOn();
  if (neon_now != neon_on_) {
    neon_on_ = neon_now;
    nl::Want(neon_on_);
    if (!neon_on_) {
      nl::Global().Clear();
      neon_hold_ = 0;
      neon_active_ = false;
    }
    REXLOG_INFO("{} neon light {} (mcla_gpu_neon_light; live): a car's neon underglow {} the ground, the walls and the other cars near it", kPrefix, neon_on_ ? "on" : "off",
                neon_on_ ? "lights" : "no longer lights");
  }
  on_for_lights_ = on_for_lights;
  const bool on = on_for_lights || neon_on_;
  sources_ = hp::SourcesFor(route, uint32_t(std::clamp<int>(int(st::mcla_gpu_local_lights_sources.Get()), 0, int(ll::kSourceMask))) & ll::kSourceMask);
  if (neon_on_) {
    sources_ |= ll::kSourceNeon;
  }
  neon_gain_ = float(std::clamp(double(st::mcla_gpu_neon_light_gain.Get()), 0.0, 16.0));
  ab_ = ll::AbOf(int(st::mcla_gpu_local_lights_ab.Get()));
  if (!on_for_lights) {
    // The pass runs for NEON LIGHT alone (its own setting is off and the headlight route is `forward`): the debug setting's other sources, its test lamp and the mod lamp A/B belong to
    // mcla_gpu_local_lights and the route, so that the row of every player does not light what a tester left in the settings of a pass that is off.
    sources_ &= ll::kSourceNeon;
    ab_ = ll::Ab::kGrid;
  }
  gain_ = float(std::clamp(double(st::mcla_gpu_local_lights_gain.Get()), 0.0, 16.0));
  car_gain_ = float(std::clamp(double(st::mcla_gpu_local_lights_car_gain.Get()), 0.0, 16.0));
  // GB-NEON-COST: the A/B of the cost savings (0: the reach test before the planes, one draw for a car's tube and the contact term's cap are on): the picture is the same either way (the cap
  // moves a little of the contact term's accuracy for its reads: local_lights_rules.h ContactDonors).
  const uint32_t cost_off = uint32_t(std::clamp<int>(int(st::mcla_gpu_local_lights_cost_off.Get()), 0, int(ll::kCostOffMask))) & ll::kCostOffMask;
  if (cost_off != cost_off_) {
    cost_off_ = cost_off;
    REXLOG_INFO("{} cost savings (mcla_gpu_local_lights_cost_off {}; live): the reach test before the planes are read {}, one draw for a car's neon tube {}, the contact term's cap (two marches a pixel) {}", kPrefix,
                cost_off_, (cost_off_ & ll::kCostOffReachFirst) != 0 ? "OFF" : "on", (cost_off_ & ll::kCostOffMerge) != 0 ? "OFF" : "on", (cost_off_ & ll::kCostOffContactCap) != 0 ? "OFF" : "on");
  }
  // GB-NEON-CONTACT: the contact term (a neon light does not shine through the car it lights, GB-NEON-BLEED), on by default; its A/B is live.
  const bool contact_on = st::mcla_gpu_neon_contact.Get();
  if (contact_on != contact_on_) {
    contact_on_ = contact_on;
    REXLOG_INFO("{} neon contact term {} (mcla_gpu_neon_contact; live): a neon light {} shut out of the car's own panels on a vehicle pixel", kPrefix, contact_on_ ? "on" : "off", contact_on_ ? "is" : "is not");
  }
  tier_ = on ? TierNow() : ll::Tier::kMedium;  // (`on` is the pass running at all: its own setting, the headlight route's, or NEON LIGHT's)
  ++frame_;
  if (on != enabled_) {
    enabled_ = on;
    if (on) {
      REXLOG_INFO("{} on (mcla_gpu_local_lights {}, headlight route {}; live): at the mid-scene depth copy, after the occlusion's apply, one scissored additive draw per light from the G-buffer's normal and albedo "
                  "and the main depth; sources {} (1 the frame's surface lights, 2 a test lamp, 4 police, 8 the cars' head lamps, 16 their rear lamps, 32 the mods' lamps outside their grid, 64 the cars' neon underglow), "
                  "lamp A/B {} (0 grid, 1 nowhere, 2 pass), gain {:.2f}, the cars' look x {:.2f}, neon light {} (x {:.2f}), budget tier {} (frame {}, police {}, head lamps {}, rear lamps {}, mod lamps {}, neon {} "
                  "= {} cars x {} lights; 32 in all)",
                  kPrefix, setting_on ? "on" : "off", hp::RouteName(route), sources_, int(ab_), gain_, car_gain_, neon_on_ ? "on" : "off", neon_gain_, ll::TierName(tier_), ll::BudgetOf(tier_).frame,
                  ll::BudgetOf(tier_).police, ll::BudgetOf(tier_).head, ll::BudgetOf(tier_).rear, ll::BudgetOf(tier_).mod, ll::BudgetOf(tier_).neon, ll::BudgetOf(tier_).neon_cars,
                  ll::BudgetOf(tier_).neon_points);
      if (on_for_lights) {
        EnsureResources();  // the pipelines are made on the first frame that records (for NEON LIGHT alone: when a lit neon car first asks, OnResolve makes them)
      }
    } else {
      REXLOG_INFO("{} off", kPrefix);
    }
  }
  // GB-H2-FOLLOW: in `pass` the forward stage stays on the draws the pass does not light unless mcla_gpu_headlight_pass_keep_forward is off (headlight_pass_rules.h).
  const bool keep_forward = st::mcla_gpu_headlight_pass_keep_forward.Get();
  if (route != route_ || keep_forward != keep_forward_) {
    route_ = route;
    keep_forward_ = keep_forward;
    const char* stage = !hp::ForwardStageOn(route, keep_forward) ? "OFF" : (route == hp::Route::kPass ? "kept on the draws the pass does not light (the neon, the store sign, the lamp glass, the draws with no planes)" : "on");
    REXLOG_INFO("{} headlight route {} (mcla_gpu_headlight_pass{}; live): the forward headlight stage (the lit shaders' variant) is {}, the pass {} the frame's headlights{}", kPrefix, hp::RouteName(route),
                route == hp::Route::kPass ? (keep_forward ? ", mcla_gpu_headlight_pass_keep_forward on" : ", mcla_gpu_headlight_pass_keep_forward off") : "", stage,
                hp::PassLightsHeadlights(route) ? "lights" : "does not light", hp::PassLightsHeadlights(route) ? " with their shadows from the atlas (the one lookup)" : "");
  }
  // The wants (sticky; the G-buffer and the shared products read them at their own BeginFrame, which the Scene runs after this): with the pass off nothing asks for any plane or product,
  // so no sidecar is made and the AO's depth chain runs for nobody. A route that gives the pass the headlights (GB-H2) wants them only while HEADLIGHTS is on (a rig with no light has nothing for
  // the pass to add, and a car passing in and out of the rig's four must not make the sidecar again): the setting's own use wants them always, as before.
  // GB-NEON: a lit neon car in the hook's table with a light that can reach a pixel of the picture (held for neon_light.h kHoldFrames frames after the last) wants them too; NEON LIGHT alone does not, so a
  // player with no neon car, or with only cars out of reach (100 m away, behind the camera), pays nothing. Every consumer has its own want: the reflections' and the others' stay whatever this says.
  // The camera history is kept while NEON LIGHT is on (the frame's own camera comes after the trigger: the probe uses the last frame's).
  if (screen_ != nullptr) {
    screen_->WantCamera(screen::Consumer::kLocalLights, neon_on_);
  }
  neon_hold_ = nl::NextHold(neon_on_, neon_on_ && NeonLightReachesPicture(), neon_hold_);
  neon_active_ = neon_on_ && neon_hold_ > 0;
  planes_wanted_ = enabled_ && (setting_on || (hp::PassLightsHeadlights(route) && headlight_rig::SettingOn()) || neon_active_);
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
  const auto unseen = [&](const larecomp::gpu::Light& l) {
    return ll::ScissorOfLight(l, rows, sf.camera.tan_half_fov[0], sf.camera.tan_half_fov[1], gb.width, gb.height).Empty();
  };
  if ((sources_ & ll::kSourceNeon) != 0) {
    // GB-NEON: the cars the hook saw in the last 100 ms, the nearest lit ones as whole tubes (the game's colour: pulse, fade and the 30 Hz hold are in it). A car whose tube is out of the picture (behind
    // the camera: the pack of racers behind the player) takes none of the car budget, as the other classes' lights that no pixel is inside the reach of take none of theirs.
    nl::Global().Recent(&neon_cars_);
    ll::NeonSourceInputs neon_in;
    neon_in.cars = &neon_cars_;
    neon_in.tier = tier_;
    neon_in.gain = neon_gain_;
    neon_in.can_be_seen = [&](const larecomp::gpu::Light& l) { return !unseen(l); };
    for (int i = 0; i < 3; ++i) {
      neon_in.camera[i] = rows.position[i];
    }
    ll::NeonReport neon_report;
    ll::GatherNeonLights(neon_in, &candidates_.neon, &neon_report);
    neon_lit_ += neon_report.lit;
    neon_unplaced_ += neon_report.unplaced;
    neon_far_ += neon_report.distant;
    neon_cut_ += neon_report.cut;
    neon_frames_ += neon_report.lit != 0 ? 1u : 0u;
    neon_space_ = neon_report.lit != 0 ? neon_report.space : neon_space_;
  }
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
  std::erase_if(candidates_.police, unseen);
  std::erase_if(candidates_.head, unseen);
  std::erase_if(candidates_.rear, unseen);
  std::erase_if(candidates_.mod, unseen);
  std::erase_if(candidates_.neon, unseen);
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
  // GB-NEON-COST: the draws (local_lights_rules.h PlanDraws): one for each light, but a car's neon tube (its lights share the car, the surface and nearly the rectangle) is one draw the pixel
  // shader sums; the flags tell the shader whether to test the reach before it reads the planes.
  // GB-NEON-CONTACT: and whether the contact term runs at all (mcla_gpu_neon_contact), and whether it marches for every light of a tube (its cap off).
  push.info[3] = ((cost_off_ & ll::kCostOffReachFirst) != 0 ? ll::kPushLateReach : 0u) | ((cost_off_ & ll::kCostOffContactCap) != 0 ? ll::kPushFullContact : 0u) | (contact_on_ ? 0u : ll::kPushNoContact);
  std::array<ll::PassDraw, ll::kMaxPassLights> plan;
  const uint32_t draws = ll::PlanDraws(selection, rects.data(), cost_off_, plan.data());
  uint32_t drawn = 0;
  for (uint32_t d = 0; d < draws; ++d) {
    const ll::PassDraw& draw = plan[d];
    const plume::RenderRect scissor(draw.rect.x0, draw.rect.y0, draw.rect.x1, draw.rect.y1);
    list->setScissors(&scissor, 1);
    push.info[0] = draw.first;
    push.info[1] = draw.count;
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
    for (uint32_t k = draw.first; k < draw.first + draw.count; ++k) {
      drawn += rects[k].Empty() ? 0u : 1u;
    }
    ++draws_;
    pixels_scissored_ += draw.rect.Area();
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
    REXLOG_INFO("{} frame {}: the first run: {} lights drawn of {} ({} from the frame, {} of them with views of the shadow atlas ({}); {} handed over: lamp A/B {} / test lamp {}; police {}, head lamps {}, rear lamps {}, mod lamps {}, neon {}), "
                "the picture {}x{} at {} host samples, G-buffer planes {} "
                "(RT3 {}), the camera {} (lens {:.3f} x {:.3f}), the units: night {:.3f}, height factor {}, exposure {:.1f}, gain {:.2f} ({} colour draws voted, {} distinct values)",
                kPrefix, frame_, drawn, selection.count, selection.from_frame, shadowed_lights_, shadow_table ? "the table is in the buffer" : "no table: no atlas or no views", selection.from_extra, int(ab_),
                (sources_ & ll::kSourceTest) != 0 ? "on" : "off",
                selection.from_class[size_t(ll::SourceClass::kPolice)], selection.from_class[size_t(ll::SourceClass::kHeadLamps)], selection.from_class[size_t(ll::SourceClass::kRearLamps)],
                selection.from_class[size_t(ll::SourceClass::kMod)], selection.from_class[size_t(ll::SourceClass::kNeon)], gb.width, gb.height,
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
    // With the pass on only for NEON LIGHT (the default) and no neon car in view all interval there is nothing to say: the line would be in every player's log every ten seconds.
    LogInterval(on_for_lights_ || lights_drawn_ != 0 || neon_lit_ != 0 || neon_unplaced_ != 0);
  }
}

void LocalLights::LogInterval(bool say) {
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
  if (say) {
    REXLOG_INFO("{} {} frames with the pass on: {} lights drawn ({}), {:.1f} MPixels scissored; the trigger frames: {}; the frame's own camera pose in {} frames; the manager held {:.1f} cars a frame; "
              "neon: {} cars lit in {} frames (the last tube read as {} space), {} not placed, {} too far, {} cut by the budget; "
              "budget tier {}; headlight route {}, {} lights drawn with the shadow lookup ({} views) in {} frames with the table; street lamps: {} lights drawn with the lamps' table in {} frames; buffers {} B; {} draw calls (cost savings off: {})",
              kPrefix, kIntervalFrames, lights_drawn_, classes.empty() ? std::string("none by class") : classes, double(pixels_scissored_) * 1.0e-6, why.empty() ? std::string("none") : why,
              frames_own_pose_, double(cars_seen_) / double(kIntervalFrames), neon_lit_, neon_frames_, nl::SpaceName(neon_space_), neon_unplaced_, neon_far_, neon_cut_, ll::TierName(tier_), hp::RouteName(route_), shadowed_lights_, shadow_views_, shadow_frames_, lamp_lights_, lamp_frames_, HeldBytes(), draws_, cost_off_);
  }
  counts_.fill(0);
  lights_drawn_ = 0;
  draws_ = 0;
  pixels_scissored_ = 0;
  frames_own_pose_ = 0;
  class_drawn_.fill(0);
  class_cut_.fill(0);
  cars_seen_ = 0;
  neon_lit_ = 0;
  neon_unplaced_ = 0;
  neon_far_ = 0;
  neon_cut_ = 0;
  neon_frames_ = 0;
  shadowed_lights_ = 0;
  shadow_views_ = 0;
  shadow_frames_ = 0;
  lamp_lights_ = 0;
  lamp_frames_ = 0;
}

}  // namespace mcla::gpu
