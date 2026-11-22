// MCLA GPU renderer -- car self-reflections (G8b prototype). See car_selfrefl.h.

#include "car_selfrefl.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_dump.h"
#include "car_rt_hit_rules.h"
#include "car_rt_refl.h"
#include "descriptors.h"
#include "draw_decode.h"
#include "gpu_class.h"
#include "host_plan.h"
#include "host_shaders.h"
#include "live_text.h"
#include "menu_world.h"
#include "pass_timing.h"
#include "scene_noise.h"
#include "screen_shared.h"
#include "texture_bytes.h"


namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car refl]";
constexpr uint32_t kMinRenderSize = 64;
// Frames without the effect on, or with no paint draw of the variant in them (the car is not on the screen), before the image is
// given back (2 s at 60 FPS).
constexpr uint32_t kReleaseAfterFrames = 120;
// The log's period while the test is on.
constexpr uint64_t kCensusFrames = 600;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

const char* ModeName(CarSelfRefl::Mode mode) {
  switch (mode) {
    case CarSelfRefl::Mode::kOff: return "off";
    case CarSelfRefl::Mode::kOcclusion: return "specular occlusion only (the test level)";
    case CarSelfRefl::Mode::kReflection: return "on (occlusion and reflection)";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------
// mcla_gpu_car_selfrefl with `auto` resolved for this graphics card (gpu_class.h: off on Low, on on Mid and High; a word that is
// no value counts as `auto` in host_plan.h Resolve, so only the debug override below reaches the occlusion-only mode), read
// through LiveText (the pause menu's thread writes the cvar's string while this one reads it; the compare key's flip is in it).
// The same rule the precache uses (host_plan.h), so the draws and the start's list cannot disagree about what is on.
static_assert(uint8_t(host_plan::Level::kOff) == uint8_t(car_selfrefl::Mode::kOff) &&
              uint8_t(host_plan::Level::kHigh) == uint8_t(car_selfrefl::Mode::kOcclusion) &&
              uint8_t(host_plan::Level::kUltra) == uint8_t(car_selfrefl::Mode::kReflection));

LiveText& Setting() {
  static LiveText text("mcla_gpu_car_selfrefl");
  return text;
}

host_plan::Choice CarReflChoice() {
  return host_plan::Resolve(host_plan::Pass::kCarRefl, gpu_class::Current(), true, Setting().Get());
}

// The debug override wins when it is above 0.
bool Forced() { return larecomp::settings::mcla_gpu_car_selfrefl_test.Get() > 0; }

CarSelfRefl::Mode ReadMode() {
  if (Forced()) {
    return car_selfrefl::ModeOfTest(int(larecomp::settings::mcla_gpu_car_selfrefl_test.Get()));
  }
  return CarSelfRefl::Mode(uint8_t(CarReflChoice().level));
}

// What the frame's BeginFrame decided, for the pipelines' per-draw choice of the paint's variant.
std::atomic<bool> g_selected{false};

car_selfrefl::Tuning ReadTuning() {
  car_selfrefl::Tuning t;
  t.length = float(larecomp::settings::mcla_gpu_car_selfrefl_length.Get());
  t.thickness = float(larecomp::settings::mcla_gpu_car_selfrefl_thickness.Get());
  t.strength = float(larecomp::settings::mcla_gpu_car_selfrefl_strength.Get());
  t.steps = int(larecomp::settings::mcla_gpu_car_selfrefl_steps.Get());
  t.view = int(larecomp::settings::mcla_gpu_car_selfrefl_view.Get());
  t.gain = float(larecomp::settings::mcla_gpu_car_selfrefl_gain.Get());
  t.full = float(larecomp::settings::mcla_gpu_car_selfrefl_full.Get());
  return car_selfrefl::Clamped(t);
}

}  // namespace

bool CarSelfRefl::Selected() { return g_selected.load(std::memory_order_relaxed); }

const char* CarSelfRefl::PassName(uint32_t pass) {
  switch (pass) {
    case kPassMs: return "car history (multisampled)";
    case kPassMsShared: return "car history (multisampled, shared colour)";
    default: return "car history";
  }
}

CarSelfRefl::~CarSelfRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarSelfRefl::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  shaders_failed_ = false;
  listed_ = false;
  built_ = false;
  // The shaders and the pipelines are made only where the effect is on at the start (not on the Low class, where `auto` is
  // off): where it is off they would be compile work and memory for nothing. A live switch to on makes the shaders in
  // BeginFrame and the pipelines in the background (HoldUntilBuilt).
  const host_plan::Choice choice = CarReflChoice();
  if (Forced()) {
    if (MakeShaders()) {
      ListPrecache("car self-reflections forced on (mcla_gpu_car_selfrefl_test)");
    }
  } else if (choice.level != host_plan::Level::kOff && MakeShaders()) {
    ListPrecache(host_plan::Describe(host_plan::Pass::kCarRefl, choice, gpu_class::Current()));
  } else {
    passes.precache().Note(host_plan::Describe(host_plan::Pass::kCarRefl, choice, gpu_class::Current()), 0);
  }
}

bool CarSelfRefl::MakeShaders() {
  if (ready_) {
    return true;
  }
  if (shaders_failed_ || !passes_) {
    return false;
  }
  bool ok = true;
  for (uint32_t i = 0; i < kPassCount && ok; ++i) {
    const HostShaderBlob blob = CarHistoryPixelShader(i);
    shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] car refl: a shader could not be made: no car self-reflection this session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    shaders_failed_ = true;
    return false;
  }
  ready_ = true;
  return true;
}

void CarSelfRefl::Shutdown() {
  // The host builder's functions name this object's shaders: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slot is not handed back (SunShafts::Shutdown).
  history_ = HostImage{};
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  ready_ = false;
  history_valid_ = false;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

void CarSelfRefl::ListPrecache(const std::string& note) {
  HostPrecache& precache = passes_->precache();
  uint32_t added = 0;
  for (uint32_t pass = 0; pass < kPassCount; ++pass) {
    added += precache.Add(HostPrecache::Key(HostPrecache::kCarSelfRefl, pass), PassName(pass),
                          [this, pass] { return MakePipeline(pass); })
                 ? 1u
                 : 0u;
  }
  listed_ = true;
  precache.Note(note, added);
}

void CarSelfRefl::HoldUntilBuilt(Mode* mode) {
  if (built_) {
    return;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_) {
    // Off at the start, on now: made on a worker of their own, in the order of the list.
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
      fresh.push_back({HostPrecache::Key(HostPrecache::kCarSelfRefl, pass), PassName(pass), [this, pass] { return MakePipeline(pass); }});
    }
    listed_ = true;
    const uint32_t count = precache.AddLive("car self-reflections", std::move(fresh));
    REXLOG_INFO("{} the car self-reflections were off at the start: {} pipelines are made in the background, and the "
                "effect is drawn from the frame those are done",
                kPrefix, count);
  }
  // Not drawn until every pipeline is made (or has failed, which Pipeline says): a frame never waits for a compile.
  for (uint32_t pass = 0; pass < kPassCount; ++pass) {
    if (!pipelines_[pass] && !pipeline_failed_[pass] &&
        !precache.Ready(HostPrecache::Key(HostPrecache::kCarSelfRefl, pass))) {
      *mode = Mode::kOff;
      return;
    }
  }
  built_ = true;
}

std::unique_ptr<plume::RenderPipeline> CarSelfRefl::MakePipeline(uint32_t pass) {
  return passes_->CreatePipeline(*shaders_[pass], plume::RenderFormat::R16G16B16A16_FLOAT, PassName(pass));
}

plume::RenderPipeline* CarSelfRefl::Pipeline(uint32_t pass) {
  if (pipelines_[pass]) {
    return pipelines_[pass].get();
  }
  if (pipeline_failed_[pass]) {
    return nullptr;
  }
  pipelines_[pass] = passes_->precache().Acquire(HostPrecache::Key(HostPrecache::kCarSelfRefl, pass), PassName(pass), [&] { return MakePipeline(pass); });
  if (!pipelines_[pass]) {
    pipeline_failed_[pass] = true;
    REXLOG_ERROR("{} the history pass's pipeline could not be made: no car self-reflection this session in that "
                 "configuration",
                 kPrefix);
  }
  return pipelines_[pass].get();
}

void CarSelfRefl::BeginFrame() {
  Mode mode = ReadMode();
  if (!ready_ && mode != Mode::kOff && !shaders_failed_ && MakeShaders()) {
    // Off at the start, on now (a live change): the shaders were not made; they are made here (a short call) and the
    // pipelines in the background (HoldUntilBuilt).
    REXLOG_INFO("{} the car self-reflections were off at the start: their shaders are made now", kPrefix);
  }
  if (!ready_ && mode != Mode::kOff) {
    mode = Mode::kOff;  // the shaders could not be made (MakeShaders said so)
  }
  if (uint32_t(mode) != last_mode_logged_) {
    last_mode_logged_ = uint32_t(mode);
    REXLOG_INFO("{} car self-reflections {} (mcla_gpu_car_selfrefl = {}{}, live)", kPrefix, ModeName(mode),
                Setting().Get(), Forced() ? ", forced by mcla_gpu_car_selfrefl_test" : "");
  }
  if (mode != Mode::kOff) {
    HoldUntilBuilt(&mode);
  }
  mode_ = mode;
  g_selected.store(mode != Mode::kOff, std::memory_order_relaxed);
  tuning_ = ReadTuning();
  source_ = std::clamp(int(larecomp::settings::mcla_gpu_car_selfrefl_source.Get()), 0, 2);
  if (screen_) {
    // The shared screen products: this effect reads the frames' cameras while it is on, and (the source setting 2: the A/B) asks for colour_post to be made for it. In the automatic mode it
    // reads the capture where another effect made it and makes none of its own.
    screen_->WantCamera(screen::Consumer::kCarHistory, mode != Mode::kOff);
    screen_->Want(screen::Consumer::kCarHistory, mode != Mode::kOff && source_ == 2 ? screen::ProductBit(screen::kColourPost) : 0u);
  }
  if (rt_) {
    rt_->BeginFrame(mode != Mode::kOff);  // the ray traced image of this frame's trigger, where it is wanted and can be made
  }
}

uint64_t CarSelfRefl::HeldBytes() const {
  const uint64_t rt = rt_ ? rt_->HeldBytes() : 0;
  return rt + (history_.texture ? TextureBytes(history_.width, history_.height, 1, history_.format) : 0);
}

void CarSelfRefl::ReleaseImage() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(history_);
  if (rt_) {
    rt_->ReleaseImage();
  }
  history_valid_ = false;
  image_for_[0] = image_for_[1] = 0;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void CarSelfRefl::OnMainDraw(const temporal::MainMatch& match, uint32_t stencil_ids) {
  if (!match) {
    return;
  }
  if (match.samples == temporal::kPauseSamples) {
    pause_view_ = true;  // the pause menu's, the race pause's, the garage's and the map's world (menu_world.h)
  } else {
    play_view_ = true;
  }
  if (!anchor_seen_ && !composite_seen_) {
    stencil_ids_ |= stencil_ids;  // the anchor's constants hold the motion of those only
  }
}

void CarSelfRefl::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  const bool had_history = history_pending_;
  history_pending_ = false;
  motion_.valid = 0;
  // The frame's camera is the shared screen products' (screen_shared.h Cameras(), read at this same anchor by the owner just before this call): the play view's and the pause/map world's kept
  // apart as in ao.cpp (the lenses differ), a frame with draws of both kinds or of neither, a size c62 does not say or constants that are no camera give none, and the history starts again.
  if (play_view_ == pause_view_) {
    history_valid_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint64_t shared_frame = screen_ ? screen_->frame_count() : 0;
  const temporal::Camera* camera = screen_ ? screen_->Cameras().Current(shared_frame) : nullptr;
  if (!camera) {
    history_valid_ = false;
    return;
  }
  // The vehicles' own motion over this frame: the next frame repeats it (the constant velocity guess of ao.h). Not in the pause view: the game's slots of a 4x frame are not trusted there (a car
  // in the garage or under the menu is still).
  if (!pause) {
    temporal::ReadVehicleMotion(banks.ps.data(), stencil_ids_, &motion_);
    motion_frame_ = frame_;
  }
  // The history written at this frame's trigger belongs to this camera, when the lens it was linearised with (the previous frame's) is this frame's.
  if (had_history) {
    const bool lens_agrees = screen_->Cameras().LensAgrees(shared_frame);
    history_valid_ = lens_agrees;
    if (!lens_agrees) {
      ++lens_changed_;
    }
    history_camera_ = *camera;
    history_frame_ = frame_;
    history_pause_ = pause;
  } else {
    history_valid_ = false;  // none made this frame: the image of the frame before is a frame too old
  }
}

// ---------------------------------------------------------------------------
// the history: the game's mid-scene depth copy
// ---------------------------------------------------------------------------

bool CarSelfRefl::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge, shafts.cpp IsFirstPoint): the first copy of the main depth that is no end-of-scene copy, into 078B0000, before the anchor. The rule is the
  // shared screen products' (ScreenShared::IsTrigger: this class's own text moved there so that every pass at the trigger asks one place), with this class's flags of what the frame has seen.
  return ScreenShared::IsTrigger(record, summary, anchor_seen_, composite_seen_, trigger_seen_);
}

EdramTargets::Target* CarSelfRefl::ColourTarget(const EdramKey& key, plume::RenderCommandList* list) {
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture) {
    return nullptr;
  }
  bool unset = false;
  return edram_->Get(key, found->height, found->scale, 0, list, &unset);
}

bool CarSelfRefl::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                            plume::RenderCommandList* list) {
  if (!list || !ready_ || !IsTrigger(record, summary)) {
    return false;
  }
  trigger_seen_ = true;
  ++triggers_;
  // The history is for the NEXT frame's paint draws, which a frame whose own draws had no paint of the variant (no car on the
  // screen, or the paint's pipeline not yet made) is not going to have either: no pass, no image work, no history. The car
  // coming into view costs one frame of the game's own paint.
  if (frame_paint_draws_ == 0) {
    ++skipped_no_car_;
    history_valid_ = false;
    return false;
  }
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  // The lens of the history is the previous frame's (this frame's camera comes at its anchor, after this): a frame that does not follow a frame with a camera of the same kind of view has none
  // (the shared screen products' camera history says: Previous).
  const temporal::Camera* previous = screen_ ? screen_->Cameras().Previous(screen_->frame_count(), pause) : nullptr;
  if (!previous) {
    ++no_camera_;
    history_valid_ = false;
    return false;
  }
  trigger_camera_ = *previous;
  // The world of the frame by that camera, and whether the effect runs in it: the game view, the pause menu's world and the
  // garage, not the map (menu_world.h).
  trigger_world_ = pause ? menu_world::Classify(true, menu_world::LensOf(&trigger_camera_), menu_world::GarageLighting())
                         : menu_world::World::kPlay;
  if (!menu_world::RulesFor(trigger_world_).car_refl) {
    ++world_skipped_;
    history_valid_ = false;
    return false;
  }
  if (!RecordHistory(summary, list)) {
    history_valid_ = false;
    return false;
  }
  history_pending_ = true;
  ++passes_run_;
  return true;
}

bool CarSelfRefl::RecordHistory(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  EdramTargets::Target& depth = *summary.source;
  const uint32_t width = std::min(summary.dest_width, depth.width);
  const uint32_t height = std::min(summary.dest_height, depth.height);
  const uint32_t render_w = depth.scale.Edge(width);
  const uint32_t render_h = depth.scale.Edge(height);
  if (render_w < kMinRenderSize || render_h < kMinRenderSize) {
    return false;
  }
  // The image is the render size over a divisor (car_selfrefl_rules.h HistoryDivisor: half for the occlusion-only test level and
  // from 2560 across, a quarter of the memory), the pass reads the plane's pixel at the lower right of each block.
  const uint32_t divisor = car_selfrefl::HistoryDivisor(mode_, render_w, int(larecomp::settings::mcla_gpu_car_selfrefl_history.Get()));
  const uint32_t image_w = car_selfrefl::HistoryEdge(render_w, divisor);
  const uint32_t image_h = car_selfrefl::HistoryEdge(render_h, divisor);
  // The main colour's identity beside the depth's, at the same samples (shafts.cpp RecordSky).
  EdramKey colour_key;
  colour_key.base_tiles = temporal::kMainColorBase;
  colour_key.pitch = temporal::kMainPitch;
  colour_key.depth = 0;
  colour_key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  colour_key.samples = summary.source_key.samples;
  EdramTargets::Target* colour = ColourTarget(colour_key, list);
  if (!colour || colour->host_samples != depth.host_samples || colour->scale != depth.scale) {
    if (!failed_logged_) {
      failed_logged_ = true;
      REXLOG_ERROR("{} the main view's colour target {} {} (depth: {} host samples at {}x): no history (reported once)",
                   kPrefix, DescribeEdramKey(colour_key),
                   !colour ? "does not exist" : "differs from the depth's samples or scale", depth.host_samples,
                   depth.scale);
    }
    return false;
  }
  // The colour: the shared screen products' colour_post where the frame has it at the picture's size (CapturePost made each texel with this pass's own arithmetic, the mean of the host samples in the
  // same order and the same clamp, so the image below is the same bytes: the source setting's A/B), else the main colour target's planes. Only a multisampled scene has a pass of its own for
  // it: a plain one reads the same single sample either way, from the shared image (a half float holds what the colour target holds).
  const HostImage* shared = SharedColour(render_w, render_h);
  const bool ms = depth.host_samples > 1;
  plume::RenderPipeline* pipeline = Pipeline(ms ? (shared ? kPassMsShared : kPassMs) : kPassPlain);
  if (!pipeline) {
    return false;
  }
  if (!passes_->EnsureImage(history_, image_w, image_h, plume::RenderFormat::R16G16B16A16_FLOAT, "car history")) {
    if (!failed_logged_) {
      failed_logged_ = true;
      REXLOG_ERROR("{} the history image could not be made: no car self-reflection while that lasts", kPrefix);
    }
    return false;
  }
  if (image_for_[0] != image_w || image_for_[1] != image_h) {
    image_for_[0] = image_w;
    image_for_[1] = image_h;
    REXLOG_INFO("{} history image {}x{} R16G16B16A16_FLOAT ({:.1f} MiB, {} of the render size) for a {}x{} picture of {} host "
                "samples",
                kPrefix, image_w, image_h, double(HeldBytes()) / (1024.0 * 1024.0), divisor == 1 ? "the full size" : "half",
                render_w, render_h, depth.host_samples);
  }
  uint32_t colour_slot = 0;
  uint32_t colour_stencil = 0;
  if (!edram_->SampleSlots(*colour, &colour_slot, &colour_stencil)) {
    return false;
  }
  if (summary.stencil_slot == kNoSlot && !no_stencil_logged_) {
    no_stencil_logged_ = true;
    REXLOG_ERROR("{} the main depth target has no stencil plane view: the history holds no car pixels and nothing is "
                 "reflected (reported once)",
                 kPrefix);
  }
  // Both planes are read as textures (after the copy the depth is already there: the call records a barrier only when it
  // is elsewhere); the game's next draws move them back themselves.
  edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);
  edram_->Transition(list, *colour, plume::RenderTextureLayout::SHADER_READ);

  Push push = {};
  push.slots[0] = summary.sample_slot;
  push.slots[1] = shared ? shared->slot : colour_slot;
  push.slots[2] = summary.stencil_slot;
  push.slots[3] = depth.host_samples;
  push.slots2[0] = summary.host_sample;
  push.sizes[0] = float(render_w);
  push.sizes[1] = float(render_h);
  push.sizes[2] = float(divisor);
  // The previous frame's lens (view space to clip, row vectors): the rows the linearisation reads.
  const temporal::Mat4& p = trigger_camera_.projection;
  push.lin[0] = float(p.m[2][2]);
  push.lin[1] = float(p.m[2][3]);
  push.lin[2] = float(p.m[3][2]);
  push.lin[3] = float(p.m[3][3]);
  pass_timing_->Mark(list, "car history");
  passes_->DrawInto(list, history_, pipeline, &push, sizeof(push));
  if (shared) {
    ++passes_shared_;
  }
  if (dump_) {
    dump_->OnHistoryImage(list, history_, divisor, p);  // the debug car dump reads this frame's image back (car_dump.h)
  }
  if (rt_) {
    // The ray traced reflection of the same texels (car_rt_refl.h): after the history, with the lens it was linearised with.
    CarRtRefl::HistoryInfo info;
    info.image = &history_;
    info.divisor = divisor;
    info.render_width = render_w;
    info.render_height = render_h;
    info.tan_x = 1.0 / std::max(std::fabs(p.m[0][0]), 1e-9);
    info.tan_y = 1.0 / std::max(std::fabs(p.m[1][1]), 1e-9);
    info.frame = frame_;
    info.upscaler = upscaler_running_;
    // While a scene runs the phase follows the scene's noise index like the other noise (scene_noise.h): the same scene frame gets the same ray pattern. It is the pattern's step for this frame; whether it is used
    // (an upscaler averages it, or the pass's own history does: NOISE-RESOLVE) is CarRtRefl::OnHistory's to say, which sets the pattern still where nothing averages it.
    info.pattern_phase = uint32_t(scene_noise::NoiseIndex(frame_) & 7u);
    info.exp_bias = exp_bias_;
    rt_->OnHistory(list, info);
  }
  return true;
}

// CHECK (not run): the history pass from the shared colour_post is the same image as from the main colour's planes (mcla_gpu_car_selfrefl_source 1 against 2 at 1080 lines and below, MSAA 1, 2, 4 and 8,
// the play view and the pause menu's: mcla_gpu_car_selfrefl_view 2, the colour the ray sees, and the car dump's history check, identical); and that nothing changes with the setting at 0 and no
// effect making colour_post (the frame has none: the old pass runs with the old slot). Above 1080 lines colour_post is half size, the pass keeps reading the planes.
const HostImage* CarSelfRefl::SharedColour(uint32_t render_width, uint32_t render_height) const {
  if (source_ == 1 || !screen_ || !screen_->Frame().Has(screen::kColourPost)) {
    return nullptr;
  }
  const HostImage* image = screen_->Image(screen::kColourPost, 0);
  if (!image || !image->texture || image->slot == kNoSlot || image->width != render_width || image->height != render_height) {
    return nullptr;
  }
  return image;
}

// ---------------------------------------------------------------------------
// the draws of the paint
// ---------------------------------------------------------------------------

void CarSelfRefl::FillConstants(const XenosState& state, bool upscaler_running,
                                car_selfrefl::CarSelfReflConstants* out) {
  *out = car_selfrefl::OffConstants();
  upscaler_running_ = upscaler_running;  // the ray traced pass's pattern turns with the frame while an upscaler works (car_rt_refl.h)
  ++paint_draws_;
  ++frame_paint_draws_;
  const bool one_view = play_view_ != pause_view_;
  if (!first_paint_logged_) {
    first_paint_logged_ = true;
    REXLOG_INFO("{} the first draw of the paint through the variant: frame {}, {} view, mode {}, history {}", kPrefix,
                frame_, pause_view_ ? "pause/map" : "play", ModeName(mode_), history_valid_ ? "valid" : "none yet");
  }
  if (mode_ == Mode::kOff || !ready_ || !history_valid_ || history_frame_ + 1 != frame_ || !one_view ||
      history_pause_ != pause_view_ || !history_.texture || history_.slot == kNoSlot) {
    return;
  }
  // The vehicle this draw belongs to: the ID its stencil write names (4 ID + {0, 1}).
  const uint32_t ids = temporal::StencilIdsWritten(state) & ~1u & ((1u << temporal::kVehicleIds) - 1u);
  if (ids == 0) {
    ++paint_no_id_;
    return;
  }
  const uint32_t id = uint32_t(std::countr_zero(ids));
  // The play view: the vehicle's own motion of the last anchor. The pause view's worlds: the car is still (OnAnchor says why).
  const temporal::Mat4* motion = nullptr;
  if (!pause_view_ && (motion_.valid & (1u << id)) != 0 && motion_frame_ == history_frame_) {
    motion = &motion_.world[id];
    ++paint_motion_;
  }
  float matrix[16];
  car_selfrefl::HistoryMatrix(history_camera_.view_projection, motion, matrix);
  const int32_t exp_bias = state.color[0].exp_bias;
  exp_bias_ = exp_bias;  // the ray traced pass of the next frame's trigger takes it (CarRtRefl::HistoryInfo::exp_bias)
  if (!bias_logged_) {
    bias_logged_ = true;
    REXLOG_INFO("{} the first draw of the paint with a history: stencil ID {}, RT0 exponent bias {} (colour gain {}), "
                "vehicle motion {}",
                kPrefix, id, exp_bias, car_selfrefl::ExpBiasGain(exp_bias) * tuning_.gain,
                motion ? "known" : pause_view_ ? "not used in this view (the car is taken as still)"
                                               : "unknown (the car is taken as still)");
  }
  if (pause_view_) {
    ++paint_pause_;
    if (!pause_paint_logged_) {
      pause_paint_logged_ = true;
      REXLOG_INFO("{} the first draw of the paint with a history in the pause view's worlds: {} (stencil ID {}), the car "
                  "is taken as still",
                  kPrefix, menu_world::Name(trigger_world_), id);
    }
  }
  // The ray traced image of the same history, if the pass made one (car_rt_refl.h): the variant reads it where it has data.
  const uint32_t rt_slot = rt_ ? rt_->SlotFor(history_frame_) : car_selfrefl::kNoRtImage;
  const bool coat_layer = larecomp::settings::mcla_gpu_car_rt_coat_layer.Get();
  *out = car_selfrefl::MakeConstants(mode_, tuning_, matrix, history_.slot, kSamplerLinearClamp,
                                     car_selfrefl::ExpBiasGain(exp_bias), rt_slot, coat_layer);
  ++paint_with_history_;
  if (rt_slot != car_selfrefl::kNoRtImage) {
    ++paint_with_rt_;
    if (!coat_layer_logged_) {
      coat_layer_logged_ = true;
      REXLOG_INFO("{} the first paint draw with a ray traced image: the clear coat's layer over the reflection of another car is {} (mcla_gpu_car_rt_coat_layer; fresnel {:.2f} at N.V 0.5, {:.2f} at 0.3, {:.2f} at 0.2)",
                  kPrefix, coat_layer ? "on" : "off", car_rt_hit::CoatLayerFresnel(0.5f), car_rt_hit::CoatLayerFresnel(0.3f), car_rt_hit::CoatLayerFresnel(0.2f));
    }
  }
}

// ---------------------------------------------------------------------------
// the frame boundary
// ---------------------------------------------------------------------------

menu_world::World CarSelfRefl::FrameWorld() const {
  if (play_view_ == pause_view_) {
    return menu_world::World::kNone;  // no main view in the frame, or both kinds
  }
  if (play_view_) {
    return menu_world::World::kPlay;
  }
  return menu_world::Classify(true, menu_world::LensOf(screen_ ? screen_->Cameras().LastPauseCamera() : nullptr), menu_world::GarageLighting());
}

const char* CarSelfRefl::Level() const {
  switch (mode_) {
    case Mode::kOff: return "off";
    case Mode::kOcclusion: return "occlusion";
    case Mode::kReflection: return "on";
  }
  return "off";
}

void CarSelfRefl::OnPresent() {
  if (mode_ != Mode::kOff && (play_view_ || pause_view_) && frame_ % kCensusFrames == 0) {
    Census();
  }
  // Off for a couple of seconds, or on with no car on the screen for as long (no draw of the paint variant): the image goes back
  // (the first trigger with a car after makes it again).
  const bool idle = mode_ == Mode::kOff || frame_paint_draws_ == 0;
  if (idle) {
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames) {
      ReleaseImage();
    }
  } else {
    off_frames_ = 0;
  }
  if (mode_ == Mode::kOff) {
    history_valid_ = false;
  }
  frame_paint_draws_ = 0;
  history_pending_ = false;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  stencil_ids_ = 0;
  trigger_world_ = menu_world::World::kNone;
  ++frame_;
}

void CarSelfRefl::Census() {
  // The numbers of the interval since the last line come first: the totals alone read as "none" for as long as the car is
  // not on the screen (the title and the loading frames), and in a run's first lines that is all there is.
  REXLOG_INFO("{} frame {}: {} view, mode {}, history {}; paint draws in the last {} frames: {}, {} with a history ({} of "
              "them in the pause view's worlds); since the start: {} triggers ({} history passes, {} of them from the shared colour, {} with no paint draw in "
              "the frame, {} without the previous frame's camera, {} lens changes, {} in a world with no effect), {} paint draws, {} with a history ({} with "
              "the vehicle's motion, {} without a stencil ID, {} with the ray traced image); {:.1f} MiB{}",
              kPrefix, frame_, pause_view_ ? "pause/map" : "play", ModeName(mode_), history_valid_ ? "valid" : "none",
              kCensusFrames, paint_draws_ - census_paint_, paint_with_history_ - census_with_history_,
              paint_pause_ - census_pause_, triggers_, passes_run_, passes_shared_, skipped_no_car_, no_camera_, lens_changed_, world_skipped_,
              paint_draws_, paint_with_history_, paint_motion_, paint_no_id_, paint_with_rt_, double(HeldBytes()) / (1024.0 * 1024.0),
              rt_ ? rt_->CensusText() : std::string());
  census_paint_ = paint_draws_;
  census_with_history_ = paint_with_history_;
  census_pause_ = paint_pause_;
}

}  // namespace mcla::gpu
