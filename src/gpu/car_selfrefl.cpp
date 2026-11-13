// MCLA GPU renderer -- car self-reflections (G8b prototype). See car_selfrefl.h.

#include "car_selfrefl.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "draw_decode.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_selfrefl_test, 0, "MCLA/GPU",
    "Diagnostic, a prototype (package G8b): the player's car paint reflects its own car, which the game's paint does not (no "
    "wing in the trunk lid, no mirror in the door). 0: the game's paint, nothing of this runs. 1: the reflection is darkened "
    "where a short ray along it passes just behind a part of the car (specular occlusion). 2: the same, and the car's own "
    "colour of the previous frame is reflected there. Needs the shader pack's car_selfrefl variant and the motion blur "
    "draw's camera; costs the graphics card time while on (a full-screen pass a frame and the ray in the paint). Live.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_car_selfrefl_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the car self-reflection instead of the paint (only while mcla_gpu_car_selfrefl_test is on): "
    "1 the weight of the hit, 2 the colour the ray found, 3 the paint's baked vertex occlusion, 4 the state of the trace "
    "(red: the point is not found in the previous frame, blue: the ray left the picture, green: no hit, yellow: a hit). 0: "
    "off. Live.")
    .debug_only()
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_selfrefl_length, 2.0, "MCLA/GPU",
    "Diagnostic: how far the car self-reflection's ray goes, in the game's world units (about metres). Live.")
    .debug_only()
    .range(0.2, 6.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_selfrefl_thickness, 0.25, "MCLA/GPU",
    "Diagnostic: how far behind a part of the car's surface (as the previous frame saw it) a ray point still hits it, "
    "in the game's world units. Live.")
    .debug_only()
    .range(0.02, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_selfrefl_strength, 1.0, "MCLA/GPU",
    "Diagnostic: the most of the reflection the car self-reflection takes over, 0 to 1. Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_car_selfrefl_steps, 12, "MCLA/GPU",
    "Diagnostic: the points on the car self-reflection's ray, 4 to 24 (the cost is one texture read each). Live.")
    .debug_only()
    .range(4, 24)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_car_selfrefl_gain, 1.0, "MCLA/GPU",
    "Diagnostic: a multiplier on the brightness of the car's own colour in its reflection (1: the target's exponent bias "
    "taken out, which is the paint's own units; unverified: raise it if the reflection of the car looks too dark). Live.")
    .debug_only()
    .range(0.0, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car refl]";
constexpr uint32_t kMinRenderSize = 64;
// Frames without the test on before the image is given back (2 s).
constexpr uint32_t kReleaseAfterFrames = 120;
// How much a lens number may differ between the frame the history was linearised with and the frame it was made in.
constexpr double kLensTolerance = 0.05;
// The log's period while the test is on.
constexpr uint64_t kCensusFrames = 600;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

double Relative(double a, double b) {
  return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
}

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

const char* ModeName(CarSelfRefl::Mode mode) {
  switch (mode) {
    case CarSelfRefl::Mode::kOff: return "off";
    case CarSelfRefl::Mode::kOcclusion: return "occlusion";
    case CarSelfRefl::Mode::kReflection: return "occlusion and reflection";
  }
  return "?";
}

car_selfrefl::Tuning ReadTuning() {
  car_selfrefl::Tuning t;
  t.length = float(REXCVAR_GET(mcla_gpu_car_selfrefl_length));
  t.thickness = float(REXCVAR_GET(mcla_gpu_car_selfrefl_thickness));
  t.strength = float(REXCVAR_GET(mcla_gpu_car_selfrefl_strength));
  t.steps = int(REXCVAR_GET(mcla_gpu_car_selfrefl_steps));
  t.view = int(REXCVAR_GET(mcla_gpu_car_selfrefl_view));
  t.gain = float(REXCVAR_GET(mcla_gpu_car_selfrefl_gain));
  return car_selfrefl::Clamped(t);
}

}  // namespace

bool CarSelfRefl::Selected() { return REXCVAR_GET(mcla_gpu_car_selfrefl_test) > 0; }

CarSelfRefl::~CarSelfRefl() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void CarSelfRefl::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  bool ok = true;
  for (uint32_t i = 0; i < kPassCount && ok; ++i) {
    const HostShaderBlob blob = CarHistoryPixelShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] car refl: a shader could not be made: no car self-reflection this session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    return;
  }
  ready_ = true;
  ListPrecache();
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

void CarSelfRefl::ListPrecache() {
  HostPrecache& precache = passes_->precache();
  for (uint32_t pass = 0; pass < kPassCount; ++pass) {
    precache.Add(HostPrecache::Key(HostPrecache::kCarSelfRefl, pass),
                 pass == kPassMs ? "car history (multisampled)" : "car history",
                 [this, pass] { return MakePipeline(pass); });
  }
}

std::unique_ptr<plume::RenderPipeline> CarSelfRefl::MakePipeline(uint32_t pass) {
  return passes_->CreatePipeline(*shaders_[pass], plume::RenderFormat::R16G16B16A16_FLOAT,
                                 pass == kPassMs ? "car history (multisampled)" : "car history");
}

plume::RenderPipeline* CarSelfRefl::Pipeline(uint32_t pass) {
  if (pipelines_[pass]) {
    return pipelines_[pass].get();
  }
  if (pipeline_failed_[pass]) {
    return nullptr;
  }
  pipelines_[pass] = passes_->precache().Acquire(
      HostPrecache::Key(HostPrecache::kCarSelfRefl, pass),
      pass == kPassMs ? "car history (multisampled)" : "car history", [&] { return MakePipeline(pass); });
  if (!pipelines_[pass]) {
    pipeline_failed_[pass] = true;
    REXLOG_ERROR("{} the history pass's pipeline could not be made: no car self-reflection this session in that "
                 "configuration",
                 kPrefix);
  }
  return pipelines_[pass].get();
}

void CarSelfRefl::BeginFrame() {
  Mode mode = car_selfrefl::ModeOfTest(int(REXCVAR_GET(mcla_gpu_car_selfrefl_test)));
  if (!ready_ && mode != Mode::kOff) {
    mode = Mode::kOff;  // the shaders could not be made (Initialize said so)
  }
  if (uint32_t(mode) != last_mode_logged_) {
    last_mode_logged_ = uint32_t(mode);
    REXLOG_INFO("{} car self-reflection test {} (mcla_gpu_car_selfrefl_test = {}, live)", kPrefix, ModeName(mode),
                REXCVAR_GET(mcla_gpu_car_selfrefl_test));
  }
  mode_ = mode;
  tuning_ = ReadTuning();
}

uint64_t CarSelfRefl::HeldBytes() const {
  return history_.texture ? TextureBytes(history_.width, history_.height, 1, history_.format) : 0;
}

void CarSelfRefl::ReleaseImage() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(history_);
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
    pause_view_ = true;  // the pause menu's, the race pause's and the map's world: no history there
    return;
  }
  play_view_ = true;
  if (!anchor_seen_ && !composite_seen_) {
    stencil_ids_ |= stencil_ids;  // the anchor's constants hold the motion of those only
  }
}

void CarSelfRefl::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  const bool had_history = history_pending_;
  history_pending_ = false;
  motion_.valid = 0;
  // The play view's camera only; a frame with draws of both kinds or of neither gives none, and leaves no history.
  if (!play_view_ || pause_view_) {
    have_previous_ = false;
    history_valid_ = false;
    return;
  }
  const uint8_t* ps = banks.ps.data();
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    have_previous_ = false;
    history_valid_ = false;
    return;
  }
  temporal::Camera camera;
  if (temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_, &camera) !=
      temporal::Reject::kNone) {
    have_previous_ = false;
    history_valid_ = false;
    return;
  }
  // The vehicles' own motion over this frame: the next frame repeats it (the constant velocity guess of ao.h).
  temporal::ReadVehicleMotion(ps, stencil_ids_, &motion_);
  motion_frame_ = frame_;
  // The history written at this frame's trigger belongs to this camera, when the lens it was linearised with (the
  // previous frame's) is this frame's.
  if (had_history) {
    const bool lens_agrees = Relative(camera.fov_y, lens_used_.fov_y) < kLensTolerance &&
                             Relative(camera.aspect, lens_used_.aspect) < kLensTolerance &&
                             Relative(camera.near_plane, lens_used_.near_plane) < kLensTolerance &&
                             Relative(camera.far_plane, lens_used_.far_plane) < kLensTolerance;
    history_valid_ = lens_agrees;
    if (!lens_agrees) {
      ++lens_changed_;
    }
    history_camera_ = camera;
    history_frame_ = frame_;
  } else {
    history_valid_ = false;  // none made this frame: the image of the frame before is a frame too old
  }
  previous_ = camera;
  have_previous_ = true;
  previous_frame_ = frame_;
}

// ---------------------------------------------------------------------------
// the history: the game's mid-scene depth copy
// ---------------------------------------------------------------------------

bool CarSelfRefl::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge, shafts.cpp IsFirstPoint) for the play view: the first copy of the main
  // depth that is no end-of-scene copy, into 078B0000, before the anchor, whose rectangle starts at the origin and stays
  // inside the picture (the game copies the picture in two tiles; the target is whole at the first).
  if (!summary.depth_copy || !summary.source || trigger_seen_) {
    return false;
  }
  if (!temporal::IsPlaySamples(temporal::MainDepthSamples(summary.source_key))) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth ||
      summary.dest_address != kTriggerDestination || anchor_seen_ || composite_seen_) {
    return false;
  }
  uint32_t width = std::min(summary.dest_width, summary.source->width);
  uint32_t height = std::min(summary.dest_height, summary.source->height);
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 &&
         uint32_t(summary.x1) <= width && uint32_t(summary.y1) <= height;
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
  // The lens of the history is the previous frame's (this frame's camera comes at its anchor, after this): a frame that
  // does not follow a frame with a camera has none.
  if (!have_previous_ || previous_frame_ + 1 != frame_) {
    ++no_camera_;
    history_valid_ = false;
    return false;
  }
  if (!RecordHistory(summary, list)) {
    history_valid_ = false;
    return false;
  }
  history_pending_ = true;
  lens_used_ = previous_;
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
  const bool ms = depth.host_samples > 1;
  plume::RenderPipeline* pipeline = Pipeline(ms ? kPassMs : kPassPlain);
  if (!pipeline) {
    return false;
  }
  if (!passes_->EnsureImage(history_, render_w, render_h, plume::RenderFormat::R16G16B16A16_FLOAT, "car history")) {
    if (!failed_logged_) {
      failed_logged_ = true;
      REXLOG_ERROR("{} the history image could not be made: no car self-reflection while that lasts", kPrefix);
    }
    return false;
  }
  if (image_for_[0] != render_w || image_for_[1] != render_h) {
    image_for_[0] = render_w;
    image_for_[1] = render_h;
    REXLOG_INFO("{} history image {}x{} R16G16B16A16_FLOAT ({:.1f} MiB) for a {}x{} picture of {} host samples", kPrefix,
                render_w, render_h, double(HeldBytes()) / (1024.0 * 1024.0), render_w, render_h, depth.host_samples);
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
  push.slots[1] = colour_slot;
  push.slots[2] = summary.stencil_slot;
  push.slots[3] = depth.host_samples;
  push.slots2[0] = summary.host_sample;
  push.sizes[0] = float(render_w);
  push.sizes[1] = float(render_h);
  // The previous frame's lens (view space to clip, row vectors): the rows the linearisation reads.
  const temporal::Mat4& p = previous_.projection;
  push.lin[0] = float(p.m[2][2]);
  push.lin[1] = float(p.m[2][3]);
  push.lin[2] = float(p.m[3][2]);
  push.lin[3] = float(p.m[3][3]);
  pass_timing_->Mark(list, "car history");
  passes_->DrawInto(list, history_, pipeline, &push, sizeof(push));
  return true;
}

// ---------------------------------------------------------------------------
// the draws of the paint
// ---------------------------------------------------------------------------

void CarSelfRefl::FillConstants(const XenosState& state, bool upscaler_running,
                                car_selfrefl::CarSelfReflConstants* out) {
  *out = car_selfrefl::OffConstants();
  ++paint_draws_;
  if (mode_ == Mode::kOff || !ready_ || !history_valid_ || history_frame_ + 1 != frame_ || !play_view_ || pause_view_ ||
      !history_.texture || history_.slot == kNoSlot) {
    return;
  }
  // The vehicle this draw belongs to: the ID its stencil write names (4 ID + {0, 1}).
  const uint32_t ids = temporal::StencilIdsWritten(state) & ~1u & ((1u << temporal::kVehicleIds) - 1u);
  if (ids == 0) {
    ++paint_no_id_;
    return;
  }
  const uint32_t id = uint32_t(std::countr_zero(ids));
  const temporal::Mat4* motion = nullptr;
  if ((motion_.valid & (1u << id)) != 0 && motion_frame_ == history_frame_) {
    motion = &motion_.world[id];
    ++paint_motion_;
  }
  float matrix[16];
  car_selfrefl::HistoryMatrix(history_camera_.view_projection, motion, matrix);
  const int32_t exp_bias = state.color[0].exp_bias;
  if (!bias_logged_) {
    bias_logged_ = true;
    REXLOG_INFO("{} the first draw of the paint with a history: stencil ID {}, RT0 exponent bias {} (colour gain {}), "
                "vehicle motion {}",
                kPrefix, id, exp_bias, car_selfrefl::ExpBiasGain(exp_bias) * tuning_.gain,
                motion ? "known" : "unknown (the car is taken as still)");
  }
  *out = car_selfrefl::MakeConstants(mode_, tuning_, matrix, history_.slot, kSamplerLinearClamp,
                                     car_selfrefl::ExpBiasGain(exp_bias),
                                     upscaler_running ? float(frame_ & 1023u) : 0.0f);
  ++paint_with_history_;
}

// ---------------------------------------------------------------------------
// the frame boundary
// ---------------------------------------------------------------------------

void CarSelfRefl::OnPresent() {
  if (mode_ != Mode::kOff && (play_view_ || pause_view_) && frame_ % kCensusFrames == 0) {
    Census();
  }
  if (mode_ == Mode::kOff) {
    // Off for a couple of seconds: the image goes back (the first trigger after makes it again).
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames) {
      ReleaseImage();
    }
    history_valid_ = false;
  } else {
    off_frames_ = 0;
  }
  history_pending_ = false;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  stencil_ids_ = 0;
  ++frame_;
}

void CarSelfRefl::Census() {
  REXLOG_INFO("{} frame {}: {} view, mode {}, history {}; {} triggers ({} history passes, {} without the previous "
              "frame's camera, {} lens changes); {} paint draws, {} with a history ({} with the vehicle's motion, {} "
              "without a stencil ID); {:.1f} MiB",
              kPrefix, frame_, pause_view_ ? "pause/map" : "play", ModeName(mode_),
              history_valid_ ? "valid" : "none", triggers_, passes_run_, no_camera_, lens_changed_, paint_draws_,
              paint_with_history_, paint_motion_, paint_no_id_, double(HeldBytes()) / (1024.0 * 1024.0));
}

}  // namespace mcla::gpu
