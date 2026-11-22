// MCLA GPU renderer -- sun shafts. See shafts.h.

#include "shafts.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "device.h"
#include "noise_resolve.h"
#include "scene_noise.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "gpu_class.h"
#include "host_plan.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "texture_bytes.h"


namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[shafts]";
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                              uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);

// The registers read: VS c16 gLightPosDir (the direction the sunlight travels), PS c39 gLightColor
// (the sun colour), VS c43 gReflectionParams (x: the wetness), VS c12-c15 the view inverse.
constexpr uint32_t kVsSunDirection = 16;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsReflection = 43;

constexpr plume::RenderFormat kImageFormat = plume::RenderFormat::R16_FLOAT;
constexpr uint32_t kMinRenderSize = 64;
// Frames without a pass before the images are given back (a noon, a night, rain: 2 s).
constexpr uint32_t kReleaseAfterFrames = 120;
// Frames of a logging interval (the scene's own interval is 600) and the changes of anchor logged in all.
constexpr uint32_t kIntervalFrames = 600;
constexpr uint32_t kAnchorChangesLogged = 8;
constexpr uint32_t kMissingCopyLogs = 3;
// Below this strength nothing is recorded.
constexpr float kMinStrength = 1.0f / 256.0f;
// The built-in strength of the light: the brightness the blurred image holds (a ray through open sky
// gathers about a quarter of the sky's brightness) times this times the level's multiplier (shafts_level.h)
// times the debug intensity.
constexpr float kGain = 2.0f;
// The decay of one step is raised to 64 / steps so that the light a ray gathers is the same whatever
// the steps (the class's, gpu_class.h SunShaftSteps).
constexpr float kDecayAt64 = 0.96f;
// The sky image's brightness is kept under this (the sun's disc is far brighter than the rest).
constexpr float kShineLimit = 4.0f;
// The wetness (VS c43.x) at which the rain's gate starts and ends: 0.095 dry, 1.17 in rain.
constexpr double kWetStart = 0.2;
constexpr double kWetEnd = 0.7;
// The sky shaders that draw the main view's sky and sun disc are listed in shafts_sun.h (kSkyPixelShaders: the city's
// xSkyhat ps_main*, ps_sunonly, as the renderer identifies them). Every one reads the direction toward the sun at
// PS c65 `SunDirection`.
constexpr uint32_t kPsSkySunDirection = 65;
// A sky constant told this many frames ago or less is still used (the sky's draws come every frame of the play
// view: this only bridges a frame whose sky draw was not seen before the first point).
constexpr uint64_t kSkyMaxAge = 120;
// Frames in a row with a sun to place and no sky constant before the log says so (twice at most).
constexpr uint32_t kSkyMissingFramesToWarn = 90;
constexpr uint32_t kSkyLogsMax = 3;

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

// A light constant that is a usable amount (a draw whose shaders do not use the register can leave
// anything there).
float Usable(float x) {
  return std::isfinite(x) && x > 0.0f ? x : 0.0f;
}

// A matrix that can be a camera's view inverse: finite, the last column (0, 0, 0, 1), unit axes
// (as ao.cpp: the vertex constants hold a frame's own view only once they matched the anchor's).
bool PoseSane(const temporal::Mat4& w) {
  if (!temporal::Finite(w)) {
    return false;
  }
  if (std::fabs(w.m[0][3]) > 1e-3 || std::fabs(w.m[1][3]) > 1e-3 || std::fabs(w.m[2][3]) > 1e-3 ||
      std::fabs(w.m[3][3] - 1.0) > 1e-3) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] +
                                    w.m[r][2] * w.m[r][2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  return true;
}
// The vertex constants' view inverse is taken as the frame's own camera once it matched the anchor's
// within kVsMatch in a frame whose camera moved by at least kVsMoved (ao.cpp, the same numbers).
constexpr double kVsMatch = 5e-4;
constexpr double kVsMoved = 5e-3;

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------
// mcla_gpu_sun_shafts with `auto` resolved for this graphics card (gpu_class.h: off on Low, default on
// Mid and High). The old low and high read as default, anything else the list does not know (a
// hand-edited file) counts as `auto` (shafts_level.h has the rule, host_plan.h passes it on to the
// precache).

// The text of the setting, read through LiveText: the pause menu's thread writes the cvar's string while
// this one reads it (live_text.h).
LiveText& Setting() {
  static LiveText text("mcla_gpu_sun_shafts");
  return text;
}

host_plan::Choice ShaftsChoice() {
  return host_plan::Resolve(host_plan::Pass::kShafts, gpu_class::Current(), true, Setting().Get());
}

SunShafts::Level ReadLevel() {
  return ShaftsChoice().strength;
}

// The ray steps in force: the class's, or the debug cvar's (shafts_level.h).
uint32_t ReadSteps() {
  return shafts_level::Steps(gpu_class::Current(), larecomp::settings::mcla_gpu_sun_shafts_steps.Get());
}

}  // namespace

SunShafts::~SunShafts() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

uint32_t SunShafts::AddVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVAddBase + (debug ? 4u : 0u) + log2_samples;
}

std::string SunShafts::VariantName(uint32_t variant) {
  switch (variant) {
    case kVSky: return "shafts sky";
    case kVSkyMs: return "shafts sky (multisampled)";
    case kVBlur: return "shafts blur";
    case kVTemporal: return "shafts temporal";
    default: break;
  }
  const uint32_t index = variant - kVAddBase;
  return std::format("shafts {} {}x", (index & 4) ? "view" : "add", 1u << (index & 3));
}

void SunShafts::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  shaders_failed_ = false;
  // The shaders and the pipelines are made only where the sun shafts are on at the start (not on the Low
  // class, where `auto` is off): where they are off they would be compile work and memory for nothing.
  // A live switch to on makes the shaders in BeginFrame and the pipelines in the background
  // (HoldUntilBuilt).
  const host_plan::Choice choice = ShaftsChoice();
  listed_ = false;
  built_ = false;
  if (choice.level != host_plan::Level::kOff && MakeShaders()) {
    ListPrecache(choice);
  } else {
    passes.precache().Note(host_plan::Describe(host_plan::Pass::kShafts, choice, gpu_class::Current()), 0);
  }
}

bool SunShafts::MakeShaders() {
  if (ready_) {
    return true;
  }
  if (shaders_failed_ || !passes_) {
    return false;
  }
  bool ok = true;
  for (uint32_t i = 0; i < kPassCount && ok; ++i) {
    const HostShaderBlob blob = SunShaftsPixelShader(i);
    shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] shafts: a shader could not be made: no sun shafts this session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    shaders_failed_ = true;
    return false;
  }
  ready_ = true;
  return true;
}

void SunShafts::Shutdown() {
  // The host builder's functions name this object's shaders: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  sky_ = HostImage{};
  shaft_ = HostImage{};
  hist_[0] = HostImage{};
  hist_[1] = HostImage{};
  hist_valid_ = false;
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  ready_ = false;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

std::vector<uint32_t> SunShafts::Variants() const {
  // The add pass for the play view's colour target at 1x, 2x and 4x (the host's MSAA level), 8x
  // when the device has it, so that a switch of the level or the first dusk waits for no pipeline.
  std::vector<uint32_t> variants = {kVSky, kVSkyMs, kVBlur, kVTemporal, AddVariant(false, 1), AddVariant(false, 2),
                                    AddVariant(false, 4)};
  if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
    variants.push_back(AddVariant(false, 8));
  }
  return variants;
}

void SunShafts::ListPrecache(const host_plan::Choice& choice) {
  HostPrecache& precache = passes_->precache();
  uint32_t added = 0;
  for (const uint32_t variant : Variants()) {
    added += precache.Add(HostPrecache::Key(HostPrecache::kSunShafts, variant), VariantName(variant),
                          [this, variant] { return MakePipeline(variant); })
                 ? 1u
                 : 0u;
  }
  listed_ = true;
  precache.Note(host_plan::Describe(host_plan::Pass::kShafts, choice, gpu_class::Current()), added);
}

void SunShafts::HoldUntilBuilt(Level* level) {
  if (built_) {
    return;
  }
  HostPrecache& precache = passes_->precache();
  const std::vector<uint32_t> variants = Variants();
  if (!listed_) {
    // Off at the start, on now: made on a worker of their own, in the order of the list.
    std::vector<HostPrecache::Item> fresh;
    for (const uint32_t variant : variants) {
      fresh.push_back({HostPrecache::Key(HostPrecache::kSunShafts, variant), VariantName(variant),
                       [this, variant] { return MakePipeline(variant); }});
    }
    listed_ = true;
    const uint32_t count = precache.AddLive("sun shafts", std::move(fresh));
    REXLOG_INFO("[gpu] shafts: the sun shafts were off at the start: {} pipelines are made in the "
                "background, and the shafts are drawn from the frame those are done",
                count);
  }
  // Not drawn until every pipeline is made (or has failed, which Pipeline says): a frame never waits
  // for a compile.
  for (const uint32_t variant : variants) {
    if (!pipelines_[variant] && !pipeline_failed_[variant] &&
        !precache.Ready(HostPrecache::Key(HostPrecache::kSunShafts, variant))) {
      *level = Level::kOff;
      return;
    }
  }
  built_ = true;
}

std::unique_ptr<plume::RenderPipeline> SunShafts::MakePipeline(uint32_t variant) {
  const std::string name = VariantName(variant);
  switch (variant) {
    case kVSky:
      return passes_->CreatePipeline(*shaders_[kPassSky], kImageFormat, name.c_str());
    case kVSkyMs:
      return passes_->CreatePipeline(*shaders_[kPassSkyMs], kImageFormat, name.c_str());
    case kVBlur:
      return passes_->CreatePipeline(*shaders_[kPassBlur], kImageFormat, name.c_str());
    case kVTemporal:
      return passes_->CreatePipeline(*shaders_[kPassTemporal], kImageFormat, name.c_str());
    default: break;
  }
  // The add pass: into the scene colour (float16 RGBA) with the target's sample count. The light is
  // added (ONE, ONE) on the colour; a debug view replaces it. The alpha is never written.
  const uint32_t index = variant - kVAddBase;
  const bool debug = (index & 4) != 0;
  const uint32_t samples = 1u << (index & 3);
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  if (!debug) {
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::ONE;
    blend.dstBlend = plume::RenderBlend::ONE;
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  }
  blend.renderTargetWriteMask = kWriteRgb;
  return passes_->CreatePipeline(passes_->layout(), *shaders_[kPassAdd],
                                 plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples,
                                 name.c_str());
}

plume::RenderPipeline* SunShafts::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  // Made ahead when listed; a variant nobody listed (a debug view, an 8x target) is made here.
  pipelines_[variant] = passes_->precache().Acquire(
      HostPrecache::Key(HostPrecache::kSunShafts, variant), VariantName(variant),
      [&] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("[gpu] shafts: the pipeline '{}' could not be made: no sun shafts this session in "
                 "that configuration",
                 VariantName(variant));
  }
  return pipelines_[variant].get();
}

void SunShafts::BeginFrame() {
  census_every_ = uint32_t(std::max(0, larecomp::settings::mcla_gpu_sun_shafts_census.Get()));
  Level level = ReadLevel();
  if (!ready_ && level != Level::kOff && !shaders_failed_ && MakeShaders()) {
    // Off at the start, on now (a live change): the shaders were not made; they are made here (a short
    // call) and the pipelines in the background (HoldUntilBuilt).
    REXLOG_INFO("[gpu] shafts: the sun shafts were off at the start: their shaders are made now");
  }
  if (!ready_ && level != Level::kOff) {
    level = Level::kOff;  // the shaders could not be made (MakeShaders said so)
  }
  if (uint32_t(level) != last_level_logged_) {
    last_level_logged_ = uint32_t(level);
    // The strength, what the setting said, and the ray steps (the class's: the shafts' quality is not a setting).
    REXLOG_INFO("[gpu] shafts: sun shafts strength {} (mcla_gpu_sun_shafts = {}, {} steps on the {} class, live)",
                shafts_level::Describe(level), Setting().Get(), ReadSteps(),
                gpu_class::ClassName(gpu_class::Current()));
  }
  if (level != Level::kOff) {
    HoldUntilBuilt(&level);
  }
  level_ = level;
}

uint64_t SunShafts::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* image : {&sky_, &shaft_, &hist_[0], &hist_[1]}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  return bytes;
}

void SunShafts::ReleaseImages() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(sky_);
  passes_->ReleaseImage(shaft_);
  passes_->ReleaseImage(hist_[0]);
  passes_->ReleaseImage(hist_[1]);
  hist_valid_ = false;
  images_made_for_[0] = images_made_for_[1] = 0;
}

bool SunShafts::EnsureImages(const Pending& plan) {
  bool ok =
      passes_->EnsureImage(sky_, plan.image[0], plan.image[1], kImageFormat, "shafts sky") &&
      passes_->EnsureImage(shaft_, plan.image[0], plan.image[1], kImageFormat, "shafts blurred");
  // NOISE-RESOLVE: the history's two images while the frames are averaged here (no temporal upscaler); given back when an upscaler takes over or the setting goes off.
  const plume::RenderTexture* hist_before[2] = {hist_[0].texture.get(), hist_[1].texture.get()};
  if (ok && plan.history) {
    ok = passes_->EnsureImage(hist_[0], plan.image[0], plan.image[1], kImageFormat, "shafts history 0") &&
         passes_->EnsureImage(hist_[1], plan.image[0], plan.image[1], kImageFormat, "shafts history 1");
  } else if (hist_[0].texture || hist_[1].texture) {
    passes_->ReleaseImage(hist_[0]);
    passes_->ReleaseImage(hist_[1]);
    hist_valid_ = false;
  }
  // A history image that was made again (a new size: the render scale, the window, an upscaler switch; or one that could not be made) holds nothing, and the counters below still describe the old
  // images: RecordTemporal's size test cannot see it (the images already have the new size), so the next frame would read a never-written image with up to kShaftsHistoryWeight (review F1). The texture
  // pointer compare is what ssr.cpp's EnsureImages does.
  if (hist_[0].texture.get() != hist_before[0] || hist_[1].texture.get() != hist_before[1]) {
    hist_valid_ = false;
    hist_frames_ = 0;
  }
  if (ok && (images_made_for_[0] != plan.image[0] || images_made_for_[1] != plan.image[1])) {
    images_made_for_[0] = plan.image[0];
    images_made_for_[1] = plan.image[1];
    REXLOG_INFO("[gpu] shafts: images at {}x{} for a {}x{} picture: sky and blurred R16_FLOAT, "
                "{:.1f} MiB",
                plan.image[0], plan.image[1], plan.render[0], plan.render[1],
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return ok;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void SunShafts::NoteSkyDraw(uint64_t vs_identity, uint64_t ps_identity,
                            const ConstantBanks::Banks& banks) {
  const shafts_sun::SkyDraw kind = shafts_sun::ClassifySkyDraw(vs_identity, ps_identity);
  if (kind == shafts_sun::SkyDraw::kNone) {
    return;
  }
  if (kind == shafts_sun::SkyDraw::kUnknownPs) {
    // The dome's own vertex shader with a pixel shader the table does not have (another game build, a variant):
    // its c65 is not read, and the log says which shader it was, a few times.
    ++sky_unknown_draws_;
    if (sky_unknown_logged_ < kSkyLogsMax && ps_identity != sky_unknown_ps_) {
      ++sky_unknown_logged_;
      sky_unknown_ps_ = ps_identity;
      REXLOG_WARN("[gpu] shafts: frame {}: a draw of the sky dome ({} {:016x}) used the pixel shader {:016x}, "
                  "which is not one of the sky's in shafts_sun.h (kSkyPixelShaders): its sun constant is "
                  "not read (logged {} of at most {})",
                  frame_, shafts_sun::SkyVertexShaderName(vs_identity), vs_identity, ps_identity,
                  sky_unknown_logged_, kSkyLogsMax);
    }
    return;
  }
  const uint8_t* ps = banks.ps.data();
  float dir[3];
  for (uint32_t i = 0; i < 3; ++i) {
    dir[i] = BankFloat(ps, kPsSkySunDirection, i);
  }
  if (!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2])) {
    return;  // a draw that left garbage in the register: the last good one stands
  }
  std::memcpy(sky_dir_, dir, sizeof(dir));
  sky_have_ = true;
  sky_frame_ = frame_;
  sky_ps_ = ps_identity;
  ++sky_draws_;
  ++sky_draws_total_;
}

void SunShafts::OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks,
                           uint64_t vs_identity, uint64_t ps_identity) {
  if (!match) {
    return;
  }
  if (match.samples == temporal::kPauseSamples) {
    pause_view_ = true;  // the pause menu's, the race pause's, the garage's and the map's world (menu_world.h)
  } else {
    play_view_ = true;
  }
  if (anchor_seen_ || composite_seen_ || !match.color) {
    return;
  }
  // The sun disc's direction: the sky's draws tell it (kept from frame to frame; a sky draw after the first
  // point serves the next frame).
  NoteSkyDraw(vs_identity, ps_identity, banks);
  // The frame's first colour draw of the main view is the sky dome: what it was is in the log when no sky draw
  // ever told the sun (LogSkySun), the way to find the sky's real identity if the table is out of date.
  if (!first_colour_seen_) {
    first_colour_seen_ = true;
    first_colour_vs_ = vs_identity;
    first_colour_ps_ = ps_identity;
  }
  // The light of the frame is the one most colour draws before the first point carry (a draw whose
  // shaders do not use a register leaves what an earlier one set).
  if (first_point_seen_) {
    return;
  }
  ++main_draws_;
  Seen now;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  for (uint32_t i = 0; i < 3; ++i) {
    now.dwords[i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
    now.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
  }
  now.dwords[6] = LoadBe32(vs + kVsReflection * 16);
  for (uint32_t i = 0; i < 16; ++i) {
    vs_view_inverse_[i] = LoadBe32(vs + 12 * 16 + i * 4);
  }
  ++vs_view_inverse_draws_;
  last_seen_ = now;
  for (uint32_t i = 0; i < seen_count_; ++i) {
    if (std::memcmp(seen_[i].dwords, now.dwords, sizeof(now.dwords)) == 0) {
      ++seen_[i].draws;
      return;
    }
  }
  if (seen_count_ < kSeenSlots) {
    seen_[seen_count_] = now;
    seen_[seen_count_].draws = 1;
    ++seen_count_;
  } else {
    ++seen_overflow_;
  }
}

void SunShafts::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  lens_step_valid_ = false;
  // The camera of the frame's kind of view (the play view's or the pause/map world's, kept apart as in
  // ao.cpp: the lenses differ); a frame with draws of both kinds or of neither gives none, and the frame
  // after it has none to use.
  if (play_view_ == pause_view_) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself, as ao.cpp: a frame size that
  // is not the main view's is caught by the first point's rectangle.
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  temporal::Camera camera;
  if (temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_,
                              &camera) != temporal::Reject::kNone) {
    have_previous_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  // The lens the previous frame's camera gave the trigger against this frame's own, for the pause/map
  // view's rule that its lens must have been steady (the trigger works with the previous frame's lens).
  const bool same_kind = have_previous_ && previous_frame_ + 1 == frame_ && previous_pause_ == pause;
  if (same_kind) {
    const auto rel = [](double a, double b) {
      return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
    };
    lens_step_ = std::max({rel(camera.fov_y, previous_.fov_y), rel(camera.aspect, previous_.aspect),
                           rel(camera.near_plane, previous_.near_plane),
                           rel(camera.far_plane, previous_.far_plane)});
    lens_step_valid_ = std::isfinite(lens_step_);
  }
  // Whether the vertex constants of this frame's draws held this frame's view inverse: trusted once
  // they did in a frame the camera moved in, dropped as soon as they differ.
  if (same_kind && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 vertex;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        vertex.m[r][c] = double(std::bit_cast<float>(vs_view_inverse_[r * 4 + c]));
      }
    }
    const double differs = temporal::MaxAbsDiff(vertex, camera.view_inverse);
    const double moved = temporal::MaxAbsDiff(camera.view_inverse, previous_.view_inverse);
    if (!(differs <= kVsMatch)) {
      vs_pose_trusted_ = false;
    } else if (moved >= kVsMoved) {
      vs_pose_trusted_ = true;
    }
  } else {
    vs_pose_trusted_ = false;
  }
  previous_ = camera;
  previous_pause_ = pause;
  have_previous_ = true;
  previous_frame_ = frame_;
}

// ---------------------------------------------------------------------------
// the sun
// ---------------------------------------------------------------------------

bool SunShafts::ComputeSun(Sun* out) const {
  *out = Sun{};
  if (main_draws_ == 0) {
    return false;
  }
  // The commonest light constants of the draws before the first point (the last one's when none
  // repeats).
  Seen pick = last_seen_;
  if (seen_count_ != 0) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < seen_count_; ++i) {
      if (seen_[i].draws >= seen_[best].draws) {
        best = i;
      }
    }
    pick = seen_[best];
  }
  float dir[3];
  float colour[3];
  for (int i = 0; i < 3; ++i) {
    dir[i] = std::bit_cast<float>(pick.dwords[i]);
    colour[i] = std::bit_cast<float>(pick.dwords[3 + i]);
  }
  const float wet = std::bit_cast<float>(pick.dwords[6]);
  if (!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2])) {
    return false;
  }
  const double length = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] +
                                  double(dir[2]) * dir[2]);
  if (!(length > 0.9 && length < 1.1)) {
    return false;  // not a direction (no light constants in this draw)
  }
  out->wetness = std::isfinite(wet) ? std::clamp(double(wet), 0.0, 16.0) : 0.0;
  // c16 is the direction the light travels (y is -0.867 at noon): the sun is where it comes from.
  const double toward_light[3] = {-double(dir[0]) / length, -double(dir[1]) / length,
                                  -double(dir[2]) / length};
  out->elevation = shafts_sun::ElevationDegrees(toward_light);

  // The direction to the sun in view space (x right, y up, z forward): the game's view space looks
  // along -z, so z changes sign. The rotation is the frame's own view from the vertex constants once
  // they are trusted, else the previous frame's camera (the sun's image position then follows the
  // camera a frame late). v_view = v_world * view, view = the inverse of the view inverse: for the
  // rotation, its transpose.
  double rotation[3][3];
  bool vertex = false;
  if (vs_pose_trusted_ && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 world;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        world.m[r][c] = double(std::bit_cast<float>(vs_view_inverse_[r * 4 + c]));
      }
    }
    if (PoseSane(world)) {
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          rotation[i][j] = world.m[j][i];
        }
      }
      vertex = true;
    }
  }
  if (!vertex) {
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        rotation[i][j] = previous_.view.m[i][j];
      }
    }
  }
  out->vertex_pose = vertex;
  if (!shafts_sun::WorldToView(rotation, toward_light, out->to_sun)) {
    return false;
  }

  // The sun in the picture: the previous frame's lens (tan of the half field of view, x and y).
  const temporal::Mat4& p = previous_.projection;
  const double tan_x = 1.0 / std::max(std::fabs(p.m[0][0]), 1e-9);
  const double tan_y = 1.0 / std::max(std::fabs(p.m[1][1]), 1e-9);
  const shafts_sun::Placement light = shafts_sun::Place(out->to_sun, tan_x, tan_y);
  out->light_ndc[0] = light.ndc[0];
  out->light_ndc[1] = light.ndc[1];
  out->light_uv[0] = float(light.uv[0]);
  out->light_uv[1] = float(light.uv[1]);

  // The sky's own sun constant (the disc the player sees) places the sun when a sky draw has told it and it
  // is the sun of the light's axes (shafts_sun.h JudgeSky); else the light does.
  const bool sky_at_hand = sky_have_ && frame_ - sky_frame_ <= kSkyMaxAge;
  const double sky[3] = {double(sky_dir_[0]), double(sky_dir_[1]), double(sky_dir_[2])};
  shafts_sun::SkyJudgement judged = shafts_sun::JudgeSky(sky_at_hand, sky, toward_light);
  out->sky_age = sky_at_hand ? frame_ - sky_frame_ : 0;
  shafts_sun::Placement used = light;
  if (judged.state == shafts_sun::SkyState::kUsed ||
      judged.state == shafts_sun::SkyState::kFarFromLight) {
    if (shafts_sun::WorldToView(rotation, sky, out->sky_to_sun)) {
      const shafts_sun::Placement placed = shafts_sun::Place(out->sky_to_sun, tan_x, tan_y);
      out->sky_ndc[0] = placed.ndc[0];
      out->sky_ndc[1] = placed.ndc[1];
      out->sky_uv[0] = float(placed.uv[0]);
      out->sky_uv[1] = float(placed.uv[1]);
      if (judged.state == shafts_sun::SkyState::kUsed) {
        used = placed;
        out->from_sky = true;
      }
    } else {
      judged.state = shafts_sun::SkyState::kUnusable;
    }
  }
  out->sky_state = judged.state;
  out->sky_elevation = judged.elevation;
  out->sky_light_angle = judged.light_angle;
  out->ndc[0] = used.ndc[0];
  out->ndc[1] = used.ndc[1];
  out->uv[0] = float(used.uv[0]);
  out->uv[1] = float(used.uv[1]);

  out->fade_elevation = shafts_sun::ElevationFade(out->elevation, out->from_sky, judged.elevation);
  out->fade_wet = 1.0 - std::clamp((out->wetness - kWetStart) / (kWetEnd - kWetStart), 0.0, 1.0);
  out->fade_view = shafts_sun::ViewFade(used);
  out->strength = float(out->fade_elevation * out->fade_wet * out->fade_view);

  // The sun's colour, its largest channel 1 (the strength is the fades' and the setting's).
  float largest = 0.0f;
  for (int i = 0; i < 3; ++i) {
    colour[i] = Usable(colour[i]);
    largest = std::max(largest, colour[i]);
  }
  for (int i = 0; i < 3; ++i) {
    out->colour[i] = largest > 1e-4f ? colour[i] / largest : 1.0f;
  }
  out->valid = true;
  return true;
}

std::string SunShafts::SkyText(const Sun& sun) {
  switch (sun.sky_state) {
    case shafts_sun::SkyState::kNotSeen:
      return "the sky's sun constant (PS c65): not told by any sky draw";
    case shafts_sun::SkyState::kUnusable:
      return "the sky's sun constant (PS c65): not a direction";
    case shafts_sun::SkyState::kFarFromLight:
    case shafts_sun::SkyState::kUsed:
      break;
  }
  return std::format(
      "the sky's sun constant (PS c65, told {} frames ago): height {:.1f} deg, {:.1f} deg from the "
      "light's sun, in view space ({:+.3f}, {:+.3f}, {:+.3f}), in the picture at ({:.2f}, {:.2f}) "
      "(ndc {:+.2f}, {:+.2f}){}",
      sun.sky_age, sun.sky_elevation, sun.sky_light_angle, sun.sky_to_sun[0], sun.sky_to_sun[1],
      sun.sky_to_sun[2], sun.sky_uv[0], sun.sky_uv[1], sun.sky_ndc[0], sun.sky_ndc[1],
      sun.sky_state == shafts_sun::SkyState::kUsed
          ? ""
          : std::format(", NOT USED (further than {:.0f} deg from the light's sun)",
                        shafts_sun::kMaxSkyLightAngle));
}

void SunShafts::LogSkySun(const Sun& sun) {
  switch (sun.sky_state) {
    case shafts_sun::SkyState::kUsed:
      sky_missing_frames_ = 0;
      if (!sky_used_logged_) {
        sky_used_logged_ = true;
        const char* sky_name = shafts_sun::SkyPixelShaderName(sky_ps_);
        REXLOG_INFO("[gpu] shafts: the sun's place in the picture comes from the sky's own sun "
                    "constant (PS c65 of the sky shader {} {:016x}, {} sky draws told it so far): {}; "
                    "the light's sun (VS c16, height {:.1f} deg) would be at ({:.2f}, {:.2f}) of the "
                    "picture",
                    sky_name ? sky_name : "?", sky_ps_, sky_draws_total_, SkyText(sun), sun.elevation,
                    sun.light_uv[0], sun.light_uv[1]);
      }
      break;
    case shafts_sun::SkyState::kFarFromLight:
      sky_missing_frames_ = 0;
      if (sky_far_logged_ < kSkyLogsMax) {
        ++sky_far_logged_;
        REXLOG_WARN("[gpu] shafts: frame {}: {}; the light's place ({:.2f}, {:.2f}) is used "
                    "(logged {} of at most {})",
                    frame_, SkyText(sun), sun.light_uv[0], sun.light_uv[1], sky_far_logged_,
                    kSkyLogsMax);
      }
      break;
    case shafts_sun::SkyState::kUnusable:
    case shafts_sun::SkyState::kNotSeen:
      if (++sky_missing_frames_ == kSkyMissingFramesToWarn &&
          sky_missing_logged_ < kSkyLogsMax - 1) {
        ++sky_missing_logged_;
        const char* first_vs = shafts_sun::SkyVertexShaderName(first_colour_vs_);
        const char* first_ps = shafts_sun::SkyPixelShaderName(first_colour_ps_);
        REXLOG_WARN("[gpu] shafts: frame {}: {} for {} frames in a row ({} sky draws seen in all, {} dome "
                    "draws with a pixel shader the table does not list); the main view's first colour "
                    "draw (the dome, when there is one) used VS {:016x} ({}) and PS {:016x} ({}); the "
                    "light's place ({:.2f}, {:.2f}) is used",
                    frame_, SkyText(sun), kSkyMissingFramesToWarn, sky_draws_total_,
                    sky_unknown_draws_, first_colour_vs_, first_vs ? first_vs : "not a sky vertex shader",
                    first_colour_ps_, first_ps ? first_ps : "not a sky pixel shader",
                    sun.light_uv[0], sun.light_uv[1]);
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// the first point: the mid-scene depth copy
// ---------------------------------------------------------------------------

void SunShafts::PictureSize(const ResolveCache::Summary& summary, uint32_t* width,
                            uint32_t* height) {
  *width = summary.dest_width;
  *height = summary.dest_height;
  if (summary.source) {
    *width = std::min(*width, summary.source->width);
    *height = std::min(*height, summary.source->height);
  }
}

bool SunShafts::IsFirstPoint(const ResolveRecord& record,
                             const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge) for the play view: the first copy of the main
  // depth that is no end-of-scene copy, into 078B0000, before the anchor, whose rectangle starts at
  // the origin and stays inside the picture (the game copies the picture in two tiles; the target
  // is whole at the first).
  if (!summary.depth_copy || !summary.source || first_point_seen_) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  const bool pause = samples == temporal::kPauseSamples;
  if (!temporal::IsPlaySamples(samples) && !pause) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth ||
      summary.dest_address != kTriggerDestination || anchor_seen_ || composite_seen_) {
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  PictureSize(summary, &width, &height);
  if (pause) {
    // The 4x view's three tiles come in an order nobody has looked at: any rectangle inside the picture,
    // whose destination is the whole picture (ao.cpp Judge says the same for the occlusion's trigger).
    return menu_world::PauseFirstPointGeometry(summary.x0, summary.y0, summary.x1, summary.y1, width,
                                               height, summary.source->width, summary.source->height,
                                               EdramTargets::kHeightAlignment);
  }
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 &&
         summary.y1 > summary.y0 && uint32_t(summary.x1) <= width && uint32_t(summary.y1) <= height;
}

EdramTargets::Target* SunShafts::ColourTarget(const EdramKey& key,
                                              plume::RenderCommandList* list) {
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture) {
    return nullptr;
  }
  bool unset = false;
  return edram_->Get(key, found->height, found->scale, 0, list, &unset);
}

bool SunShafts::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                          plume::RenderCommandList* list, bool upscaler_running) {
  if (!list || !ready_ || !IsFirstPoint(record, summary)) {
    return false;
  }
  first_point_seen_ = true;
  ++triggers_;
  outcome_ = Outcome::kNoCamera;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  // The previous frame's camera, of the same kind of view; a pause/map view's also needs a lens that has
  // been steady (its second frame, the pause camera's swing and the map's zoom have none).
  if (!have_previous_ || previous_frame_ + 1 != frame_ || previous_pause_ != pause ||
      (pause && !menu_world::LensSteady(lens_step_valid_, lens_step_))) {
    ++skipped_no_camera_;
    return false;
  }
  // The world of the frame by that camera, and whether it has shafts: the pause menu's world and the garage
  // when a sky draw of the frame told where the sun is, the map never.
  trigger_world_ = pause ? menu_world::Classify(true, menu_world::LensOf(&previous_),
                                                menu_world::GarageLighting())
                         : menu_world::World::kPlay;
  if (!menu_world::ShaftsRun(trigger_world_, sky_draws_ != 0)) {
    outcome_ = Outcome::kWorld;
    return false;
  }
  outcome_ = Outcome::kNoLight;
  Sun sun;
  if (!ComputeSun(&sun)) {
    return false;
  }
  sun_ = sun;
  LogSkySun(sun);
  const Level level = level_;
  const float intensity = float(std::clamp(larecomp::settings::mcla_gpu_sun_shafts_intensity.Get(), 0.0, 8.0));
  const float strength = sun.strength * shafts_level::Multiplier(level) * intensity;
  if (!(strength * kGain > kMinStrength)) {
    // The sun gives no shafts now (noon, night, rain, behind the camera): nothing is recorded.
    outcome_ = Outcome::kGated;
    ++gated_;
    return false;
  }

  Pending plan;
  uint32_t width = 0;
  uint32_t height = 0;
  PictureSize(summary, &width, &height);
  const Scale scale = summary.source->scale;
  plan.render[0] = scale.Edge(width);
  plan.render[1] = scale.Edge(height);
  if (plan.render[0] < kMinRenderSize || plan.render[1] < kMinRenderSize) {
    outcome_ = Outcome::kTooSmall;
    return false;
  }
  // The working images: half the picture, capped at a 960x540 box (shafts_image.h; the cvar is read every frame, so
  // a switch re-makes the images at the next sun frame). The sky pass spreads its four taps over a texel's footprint.
  const shafts_image::Size image =
      shafts_image::ImageSize(plan.render[0], plan.render[1], larecomp::settings::mcla_gpu_sun_shafts_cap.Get());
  plan.image[0] = image.width;
  plan.image[1] = image.height;
  plan.footprint[0] = shafts_image::Footprint(plan.render[0], plan.image[0]);
  plan.footprint[1] = shafts_image::Footprint(plan.render[1], plan.image[1]);
  plan.host_samples = summary.source->host_samples;
  // The main colour's identity beside the depth's, at the same samples (ao.cpp RecordApply).
  plan.colour.base_tiles = temporal::kMainColorBase;
  plan.colour.pitch = temporal::kMainPitch;
  plan.colour.depth = 0;
  plan.colour.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  plan.colour.samples = summary.source_key.samples;
  plan.sun_uv[0] = sun.uv[0];
  plan.sun_uv[1] = sun.uv[1];
  plan.sun_from_sky = sun.from_sky;
  for (int i = 0; i < 3; ++i) {
    plan.tint[i] = sun.colour[i] * strength * kGain;
  }
  plan.level = level;
  plan.steps = ReadSteps();
  plan.density = float(std::clamp(larecomp::settings::mcla_gpu_sun_shafts_density.Get(), 0.3, 1.0));
  plan.decay = std::pow(kDecayAt64, 64.0f / float(plan.steps));
  plan.view = uint32_t(std::clamp(larecomp::settings::mcla_gpu_sun_shafts_view.Get(), 0, 3));
  // The noise moves every frame while something averages it: a temporal upscaler, or (NOISE-RESOLVE) the history of the blurred image (RecordTemporal) while none runs; else a fixed pattern.
  plan.history = !upscaler_running && larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get() && plan.view == 0;
  plan.noise_shift = float(noise_resolve::FrameTerm(upscaler_running, plan.history, scene_noise::NoiseIndex(frame_), 1023u));

  if (!RecordSky(summary, list, plan)) {
    outcome_ = Outcome::kNotRecorded;
    return false;
  }
  recorded_this_frame_ = true;
  sky_taken_ = true;
  pending_ = plan;
  pending_.valid = true;  // the shafts are still to be added
  outcome_ = Outcome::kSkyOnly;
  // The menu worlds add the shafts here, at the first point, always: their end-of-scene copy (three tiles) was
  // never looked at, and what they teach must not reach the play view's policy (EndSkyFrame).
  anchor_ = trigger_world_ != menu_world::World::kPlay
                ? shafts_anchor::Anchor::kMidScene
                : policy_.Choose(larecomp::settings::mcla_gpu_sun_shafts_early.Get());
  if (anchor_ == shafts_anchor::Anchor::kMidScene) {
    // Here, before the fog and the particles: the debug switch, or the fall-back of a build or a scene
    // that gives no copy of the main colour at the second point. The copy is still watched there.
    pending_.valid = false;
    composited_ = RecordShafts(list, plan);
    outcome_ = composited_ ? Outcome::kRecorded : Outcome::kNotRecorded;
  }
  return true;
}

bool SunShafts::RecordSky(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                          const Pending& plan) {
  EdramTargets::Target& depth = *summary.source;
  EdramTargets::Target* colour = ColourTarget(plan.colour, list);
  if (!colour || colour->host_samples != depth.host_samples || colour->scale != depth.scale) {
    if (!images_failed_logged_) {
      images_failed_logged_ = true;
      REXLOG_ERROR("[gpu] shafts: the main view's colour target {} {} (depth: {} host samples at "
                   "{}x): no sun shafts (reported once)",
                   DescribeEdramKey(plan.colour),
                   !colour ? "does not exist" : "differs from the depth's samples or scale",
                   depth.host_samples, depth.scale);
    }
    return false;
  }
  const bool ms = depth.host_samples > 1;
  // Everything that can fail before anything is recorded; the second point's pipelines too, so that
  // a frame does not take the sky and then lose the shafts.
  plume::RenderPipeline* sky = Pipeline(ms ? kVSkyMs : kVSky);
  plume::RenderPipeline* blur = Pipeline(kVBlur);
  plume::RenderPipeline* add = Pipeline(AddVariant(plan.view != 0, depth.host_samples));
  if (!sky || !blur || !add) {
    return false;
  }
  if (!EnsureImages(plan)) {
    if (!images_failed_logged_) {
      images_failed_logged_ = true;
      REXLOG_ERROR("[gpu] shafts: the images could not be made: no sun shafts while that lasts "
                   "(asked again every frame)");
    }
    return false;
  }
  uint32_t colour_slot = 0;
  uint32_t colour_stencil = 0;
  if (!edram_->SampleSlots(*colour, &colour_slot, &colour_stencil)) {
    return false;
  }
  // Both planes are read as textures (after the copy the depth is already there: the call records a
  // barrier only when it is elsewhere); the game's next draws move them back themselves.
  edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);
  edram_->Transition(list, *colour, plume::RenderTextureLayout::SHADER_READ);

  Push push = {};
  push.slots[0] = summary.sample_slot;
  push.slots[1] = colour_slot;
  push.slots[2] = depth.host_samples;
  push.sizes[0] = float(plan.image[0]);
  push.sizes[1] = float(plan.image[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  push.sun[0] = float(std::clamp(larecomp::settings::mcla_gpu_sun_shafts_sky_depth.Get(), 0.0, 0.01));
  push.sun[1] = kShineLimit;
  push.tint[0] = plan.footprint[0];
  push.tint[1] = plan.footprint[1];
  push.tint[3] = float(std::clamp(larecomp::settings::mcla_gpu_sun_shafts_threshold.Get(), 0.0, 4.0));
  pass_timing_->Mark(list, "shafts sky");
  passes_->DrawInto(list, sky_, sky, &push, sizeof(push));
  return true;
}

// ---------------------------------------------------------------------------
// the second point: the end-of-scene copy of the main colour
// ---------------------------------------------------------------------------

bool SunShafts::ColourSourceKey(const ResolveRecord& r, EdramKey* key) {
  if (r.flags & (kEventSurfaceUnreadable | kEventStackUnreadable)) {
    return false;
  }
  if ((r.resolve_flags & 7) != 0) {
    return false;  // colour 1..3 or depth: not the main colour (colour 0)
  }
  // As ResolveCache::Execute reads the source of a copy (and FrameLayers::ResolveSourceKey): pitch
  // and samples from the device's RB_SURFACE_INFO, base and format from the bound surface's header
  // (+0x1C), else the registers.
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const bool bound = pre.DeviceDword(guest::kDevColorSurfaces) != 0;
  xreg::RB_SURFACE_INFO surface_info;
  surface_info.value = pre.Register(guest::reg::kRbSurfaceInfo);
  key->pitch = uint16_t(surface_info.surface_pitch);
  key->samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
  xreg::RB_COLOR_INFO color_info;
  color_info.value =
      bound ? LoadBe32(r.state.surfaces[0] + 0x1C) : pre.Register(guest::reg::kRbColorInfo);
  key->base_tiles = uint16_t(color_info.color_base | (color_info.color_base_bit_11 << 11));
  key->depth = 0;
  key->format = uint8_t(FoldColorFormat(uint32_t(color_info.color_format)));
  return true;
}

bool SunShafts::BeforeResolve(const ResolveRecord& record, plume::RenderCommandList* list) {
  // Only after the first point of a frame whose sky was taken, before the anchor draw, and until a copy
  // of the main colour has come (the first is the trigger: the second tile's finds the light in it).
  if (!sky_taken_ || copy_seen_ || !list || !ready_) {
    return false;
  }
  if (anchor_seen_ || composite_seen_) {
    return false;  // the scene is over: no copy of the main colour came in time
  }
  // The second point: a copy (not a clear alone) of the main colour at the samples the sky was taken
  // at. Where the game issues it from does not matter: its tile copies of the main depth are plain
  // Resolve calls (the AO census), and the first version of this rule asked for EndTiling's and never
  // found a copy.
  EdramKey key;
  const bool keyed = ColourSourceKey(record, &key);
  if (!keyed || !(key == pending_.colour) || (record.resolve_flags & 0x2000u) != 0) {
    NoteResolve(record, keyed ? &key : nullptr);
    return false;
  }
  copy_seen_ = true;
  if (!pending_.valid) {
    return false;  // the shafts went in at the first point: the copy was only watched
  }
  const Pending plan = pending_;
  pending_.valid = false;  // once: the other tiles' copies find the picture with the light in it
  if (census_every_ != 0 && census_end_logged_ < 8) {
    ++census_end_logged_;
    TextureKey destination;
    uint32_t d[6];
    bool have = false;
    if ((record.flags & kEventHasDest) && !(record.flags & kEventDestUnreadable)) {
      for (uint32_t i = 0; i < 6; ++i) {
        d[i] = LoadBe32(record.dest_header + guest::kTextureFetchOffset + 4 * i);
      }
      const TextureFetch fetch = ReadTextureFetch(d);
      if (fetch.is_texture) {
        destination = fetch.key;
        have = true;
      }
    }
    REXLOG_INFO("{} frame {}: the end-of-scene copy that triggers the second point: caller "
                "{:#010x}, {}, source {}, destination {}, after {} other resolves since the first "
                "point",
                kPrefix, frame_, record.caller,
                (record.flags & kEventInsideEndTiling) ? "inside EndTiling" : "plain resolve",
                DescribeEdramKey(key), have ? destination.Describe() : std::string("unreadable"),
                seen_resolve_count_);
  }
  composited_ = RecordShafts(list, plan);
  outcome_ = composited_ ? Outcome::kRecorded : Outcome::kNotRecorded;
  return composited_;
}

void SunShafts::NoteResolve(const ResolveRecord& record, const EdramKey* key) {
  if (seen_resolve_count_ < kSeenResolves) {
    SeenResolve& seen = seen_resolves_[seen_resolve_count_];
    seen.caller = record.caller;
    seen.flags = record.flags;
    seen.resolve_flags = record.resolve_flags;
    seen.keyed = key != nullptr;
    seen.key = key ? *key : EdramKey{};
  }
  ++seen_resolve_count_;
}

void SunShafts::LogMissingCopy() {
  std::string text;
  const uint32_t shown = std::min<uint32_t>(seen_resolve_count_, kSeenResolves);
  for (uint32_t i = 0; i < shown; ++i) {
    const SeenResolve& seen = seen_resolves_[i];
    text += std::format("{}[caller {:#010x}, {}, resolve flags {:#x}, {}]", i ? " " : "", seen.caller,
                        (seen.flags & kEventInsideEndTiling) ? "inside EndTiling" : "plain",
                        seen.resolve_flags,
                        seen.keyed ? DescribeEdramKey(seen.key) : std::string("not a colour 0 copy"));
  }
  REXLOG_WARN("[gpu] shafts: frame {}: the sky was taken at the mid-scene depth copy but no copy of "
              "the main colour ({}) came before the anchor draw; {} other resolves between the two"
              "{}{} (logged {} of at most {}; after {} frames in a row the shafts are added at the "
              "mid-scene point instead)",
              frame_, DescribeEdramKey(pending_.colour), seen_resolve_count_,
              shown ? ": " : "", text, missing_copy_logged_, kMissingCopyLogs,
              shafts_anchor::Policy::kMissesToFallBack);
}

bool SunShafts::RecordShafts(plume::RenderCommandList* list, const Pending& plan) {
  EdramTargets::Target* colour = ColourTarget(plan.colour, list);
  if (!colour || colour->host_samples != plan.host_samples) {
    return false;
  }
  plume::RenderPipeline* blur = Pipeline(kVBlur);
  if (!blur || !sky_.texture || !shaft_.texture || sky_.width != plan.image[0] ||
      sky_.height != plan.image[1]) {
    return false;
  }
  // The radial blur of the sky image toward the sun.
  Push push = {};
  push.slots[1] = sky_.slot;
  push.slots[2] = larecomp::settings::mcla_gpu_noise_blue.Get() ? 1u : 0u;  // the ray starts take the blue noise tile (hlsl/shafts_noise.hlsli), else the first version's gradient noise
  push.slots[3] = plan.steps;
  push.sizes[0] = float(plan.image[0]);
  push.sizes[1] = float(plan.image[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  push.sun[0] = plan.sun_uv[0];
  push.sun[1] = plan.sun_uv[1];
  push.sun[2] = plan.density;
  push.sun[3] = plan.decay;
  push.tint[3] = plan.noise_shift;
  pass_timing_->Mark(list, "shafts blur");
  passes_->DrawInto(list, shaft_, blur, &push, sizeof(push));
  // NOISE-RESOLVE: the blurred image averaged over frames (the add pass reads the result); without the pass or the images the blurred image itself.
  const HostImage* resolved = plan.history ? RecordTemporal(list, plan) : nullptr;
  pass_timing_->Mark(list, "shafts add");
  if (!RecordAdd(list, *colour, plan, resolved ? *resolved : shaft_)) {
    return false;
  }
  if (!first_run_logged_) {
    first_run_logged_ = true;
    REXLOG_INFO("[gpu] shafts: the first sun shafts pass: strength {} at {}x{} (images {}x{}), {} steps, {} "
                "host samples, the sun at ({:.2f}, {:.2f}) of the picture (from {}), {}, at {} (the "
                "debug cvars mcla_gpu_sun_shafts_intensity, _density, _threshold tune the look)",
                shafts_level::Describe(plan.level), plan.render[0], plan.render[1], plan.image[0], plan.image[1],
                plan.steps, plan.host_samples, plan.sun_uv[0], plan.sun_uv[1],
                plan.sun_from_sky ? "the sky's sun constant" : "the light's direction",
                plan.view != 0 ? "a debug view" : "added to the colour",
                shafts_anchor::AnchorName(anchor_));
  }
  if (trigger_world_ != menu_world::World::kPlay) {
    // A menu world's run is not the play view's (its own count, not the anchor policy's).
    ++runs_world_;
    if (!first_world_run_logged_) {
      first_world_run_logged_ = true;
      REXLOG_INFO("[gpu] shafts: the first sun shafts pass in the {}: {} at {}x{}, {} steps, {} host "
                  "samples, the sun at ({:.2f}, {:.2f}) of the picture (from {}), added at {} (the "
                  "world has a sky drawn, a steady lens and the same settings as the game view)",
                  menu_world::Name(trigger_world_), shafts_level::Name(level_), plan.render[0], plan.render[1],
                  plan.steps, plan.host_samples, plan.sun_uv[0], plan.sun_uv[1],
                  plan.sun_from_sky ? "the sky's sun constant" : "the light's direction",
                  shafts_anchor::AnchorName(anchor_));
    }
    return true;
  }
  ++runs_;
  if (anchor_ == shafts_anchor::Anchor::kEndOfScene) {
    ++runs_end_;
  } else {
    ++runs_mid_;
  }
  return true;
}

const HostImage* SunShafts::RecordTemporal(plume::RenderCommandList* list, const Pending& plan) {
  plume::RenderPipeline* temporal = Pipeline(kVTemporal);
  if (!temporal || !hist_[0].texture || !hist_[1].texture || hist_[0].width != plan.image[0] || hist_[0].height != plan.image[1]) {
    hist_valid_ = false;
    return nullptr;
  }
  // The history is the frame before's (the same kind of view, the same size) and the sun has not moved too far for the picture to be the same one. The image the pass would read must also have been
  // drawn into since it was made (its layout is no longer unknown): the belt to EnsureImages' braces (review F1).
  const bool pause = pause_view_;
  const float shift_u = hist_sun_[0] - plan.sun_uv[0];
  const float shift_v = hist_sun_[1] - plan.sun_uv[1];
  const float aspect = float(plan.image[0]) / float(std::max(plan.image[1], 1u));
  const uint32_t out_index = 1 - hist_index_;
  HostImage& history_in = hist_[hist_index_];
  HostImage& history_out = hist_[out_index];
  const bool keep = hist_valid_ && hist_frame_ + 1 == frame_ && hist_pause_ == pause && std::isfinite(shift_u) && std::isfinite(shift_v) &&
                    noise_resolve::ShaftsKeepsHistory(shift_u, shift_v, aspect) && history_in.layout != plume::RenderTextureLayout::UNKNOWN;
  Push push = {};
  push.slots[0] = keep ? history_in.slot : shaft_.slot;  // without a history any readable image: the shader does not read it
  push.slots[1] = shaft_.slot;
  push.slots[2] = keep ? 1u : 0u;
  push.sizes[0] = float(plan.image[0]);
  push.sizes[1] = float(plan.image[1]);
  push.sun[2] = keep ? noise_resolve::ShaftsHistoryWeight(hist_frames_) : 0.0f;
  push.sun[3] = noise_resolve::kShaftsClampMargin;
  push.tint[0] = shift_u;
  push.tint[1] = shift_v;
  push.tint[2] = noise_resolve::kShaftsStaleFrom;
  push.tint[3] = noise_resolve::kShaftsStaleTo;
  pass_timing_->Mark(list, "shafts temporal");
  passes_->DrawInto(list, history_out, temporal, &push, sizeof(push));
  hist_index_ = out_index;
  hist_valid_ = true;
  hist_frame_ = frame_;
  hist_pause_ = pause;
  hist_sun_[0] = plan.sun_uv[0];
  hist_sun_[1] = plan.sun_uv[1];
  // A frame that does not keep the history still writes one frame into it: the next frame has 1 frame of history (weight 1/2), not 0.
  hist_frames_ = keep ? std::min(hist_frames_ + 1u, 64u) : 1u;
  if (keep) {
    ++hist_used_;
    if (!hist_logged_) {
      hist_logged_ = true;
      REXLOG_INFO("[gpu] shafts: the first frame that averages the blurred image over a history (no temporal upscaler: the ray starts' noise changes every frame, mcla_gpu_noise_resolve_no_upscaler): "
                  "weight {:.2f} at most, the history read shifted by the sun's own move, clamped to the 3x3 neighbourhood +- {:.2f} and dropped where it differs from the frame by more than the noise does, images {}x{} R16_FLOAT x2 (reported once)",
                  noise_resolve::kShaftsHistoryWeight, noise_resolve::kShaftsClampMargin, plan.image[0], plan.image[1]);
    }
  }
  return &history_out;
}

bool SunShafts::RecordAdd(plume::RenderCommandList* list, EdramTargets::Target& colour,
                          const Pending& plan, const HostImage& shine) {
  plume::RenderPipeline* pipeline = Pipeline(AddVariant(plan.view != 0, colour.host_samples));
  if (!pipeline) {
    return false;
  }
  // The scene's own sequence for a target it draws into (ao.cpp RecordApply): the layout, a
  // framebuffer of that target alone (no depth: nothing here tests or writes it). It is a colour-only
  // render pass in the middle of the frame; the next draw of the game binds its own again.
  edram_->Transition(list, colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {&colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  Push push = {};
  push.slots[1] = plan.view == 1 ? sky_.slot : shine.slot;
  push.slots[3] = plan.view;
  push.sizes[0] = float(plan.render[0]);
  push.sizes[1] = float(plan.render[1]);
  push.sizes[2] = float(plan.image[0]);
  push.sizes[3] = float(plan.image[1]);
  push.tint[0] = plan.tint[0];
  push.tint[1] = plan.tint[1];
  push.tint[2] = plan.tint[2];
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(plan.render[0]), float(plan.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(plan.render[0]), int32_t(plan.render[1]));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

const char* SunShafts::OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNoTrigger:
      return "no resolve of the frame was the first point (the mid-scene depth copy)";
    case Outcome::kNoCamera: return "no previous frame's camera";
    case Outcome::kNoLight: return "no usable light constants in the main view's draws";
    case Outcome::kGated:
      return "the sun gives no shafts now (its height, the weather or its place): nothing recorded";
    case Outcome::kWorld:
      return "the world has none (the map, or no sky drawn in an interior): nothing recorded";
    case Outcome::kTooSmall: return "the picture is smaller than the passes are worth";
    case Outcome::kNotRecorded:
      return "a pipeline or an image could not be made (see the errors above)";
    case Outcome::kSkyOnly:
      return "the sky was taken, no copy of the main colour came after it before the anchor draw: "
             "nothing added";
    case Outcome::kRecorded: return "all three passes recorded";
  }
  return "?";
}

void SunShafts::EndSkyFrame() {
  // A frame in which the sky was taken is over: what became of its shafts teaches the policy where to
  // add them, and the interval's tally. The menu worlds' frames add at the first point whatever the policy says
  // and teach it nothing.
  if (trigger_world_ != menu_world::World::kPlay) {
    if (!composited_) {
      ++lost_;
    }
    return;
  }
  if (!copy_seen_ && missing_copy_logged_ < kMissingCopyLogs) {
    ++missing_copy_logged_;
    LogMissingCopy();
  }
  const bool was_fallen_back = policy_.fallen_back();
  policy_.EndOfFrame(anchor_, copy_seen_);
  interval_.Sky(anchor_, composited_, copy_seen_);
  interval_.SunPlacedBy(pending_.sun_from_sky);
  if (!composited_) {
    ++lost_;
  }
  if (was_fallen_back != policy_.fallen_back()) {
    const uint32_t changes = policy_.fallbacks() + policy_.returns();
    if (changes <= kAnchorChangesLogged) {
      if (policy_.fallen_back()) {
        REXLOG_WARN("[gpu] shafts: frame {}: no copy of the main colour came after the first point in {} "
                    "frames in a row: the sun shafts are added at {} from now on, where the fog covers "
                    "them at distance; the copy is still watched and the end of the scene is used again "
                    "after {} frames with it",
                    frame_, shafts_anchor::Policy::kMissesToFallBack,
                    shafts_anchor::AnchorName(shafts_anchor::Anchor::kMidScene),
                    shafts_anchor::Policy::kHitsToReturn);
      } else {
        REXLOG_INFO("[gpu] shafts: frame {}: the copy of the main colour came in {} frames in a row: "
                    "the sun shafts are added at {} again",
                    frame_, shafts_anchor::Policy::kHitsToReturn,
                    shafts_anchor::AnchorName(shafts_anchor::Anchor::kEndOfScene));
      }
    }
  }
}

void SunShafts::LogInterval() {
  // One line per interval, and only when the sun gave shafts in it (a noon, a night or a rain says
  // nothing): where they went in and how often.
  if (!interval_.Worth()) {
    return;
  }
  REXLOG_INFO("[gpu] shafts (interval): {} frames, the sun gave shafts in {}: added {} times at {}, {} "
              "at {}, {} not at all; the sun's place came from the sky's sun constant in {} of them "
              "and from the light's direction in {}; no copy of the main colour came after the first "
              "point in {}; now adding at {}{} ({} falls back and {} returns so far; {} frames with "
              "shafts in all, {} at the end of the scene, {} at the mid-scene point, {} lost)",
              interval_.frames, interval_.sky, interval_.end_of_scene,
              shafts_anchor::AnchorName(shafts_anchor::Anchor::kEndOfScene), interval_.mid_scene,
              shafts_anchor::AnchorName(shafts_anchor::Anchor::kMidScene), interval_.lost,
              interval_.sun_by_sky, interval_.sun_by_light,
              interval_.copies_missing, shafts_anchor::AnchorName(policy_.Choose(
                  larecomp::settings::mcla_gpu_sun_shafts_early.Get())),
              larecomp::settings::mcla_gpu_sun_shafts_early.Get()
                  ? " (forced by mcla_gpu_sun_shafts_early)"
                  : (policy_.fallen_back() ? " (the fall-back)" : ""),
              policy_.fallbacks(), policy_.returns(), runs_ + lost_, runs_end_, runs_mid_, lost_);
}

menu_world::World SunShafts::FrameWorld() const {
  if (play_view_ == pause_view_) {
    return menu_world::World::kNone;  // no main view in the frame, or both kinds
  }
  if (play_view_) {
    return menu_world::World::kPlay;
  }
  return menu_world::Classify(true, menu_world::LensOf(have_previous_ && previous_pause_ ? &previous_ : nullptr),
                              menu_world::GarageLighting());
}

std::string SunShafts::LevelName() const {
  return std::string(shafts_level::Name(level_));
}

void SunShafts::OnPresent() {
  if (level_ != Level::kOff) {
    if (sky_taken_) {
      EndSkyFrame();
    }
    interval_.Frame();
    if (interval_.frames >= kIntervalFrames) {
      LogInterval();
      interval_.Clear();
    }
  } else {
    interval_.Clear();
  }
  if (census_every_ != 0 && (play_view_ || pause_view_)) {
    Census();
  }
  if (level_ == Level::kOff) {
    // Off for a second: the images go back (they are made again by the first pass after).
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames) {
      ReleaseImages();
    }
    unused_frames_ = 0;
  } else {
    off_frames_ = 0;
    // A noon, a night or a rain gives no shafts for long: the images go back after a while.
    if (recorded_this_frame_) {
      unused_frames_ = 0;
    } else if (unused_frames_ < kReleaseAfterFrames && ++unused_frames_ == kReleaseAfterFrames) {
      ReleaseImages();
    }
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  first_colour_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  first_point_seen_ = false;
  main_draws_ = 0;
  seen_count_ = 0;
  seen_overflow_ = 0;
  vs_view_inverse_draws_ = 0;
  pending_ = Pending{};
  sun_ = Sun{};
  sky_draws_ = 0;
  trigger_world_ = menu_world::World::kNone;
  outcome_ = Outcome::kNoTrigger;
  recorded_this_frame_ = false;
  sky_taken_ = false;
  copy_seen_ = false;
  composited_ = false;
  seen_resolve_count_ = 0;
  ++frame_;
}

void SunShafts::Census() {
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!periodic) {
    return;
  }
  ++census_frames_logged_;
  if (!sun_.valid) {
    REXLOG_INFO("{} frame {}: {} view, {} (mcla_gpu_sun_shafts = {}); main view colour draws {} "
                "before the first point; {} first points in all, {} without a camera; {} sky draws "
                "told the sun disc's direction (this frame {})",
                kPrefix, frame_, pause_view_ ? "pause/map" : "play", OutcomeName(outcome_),
                shafts_level::Name(level_), main_draws_, triggers_, skipped_no_camera_, sky_draws_total_,
                sky_draws_);
    return;
  }
  REXLOG_INFO("{} frame {}: {}; sun height {:.1f} deg (the light's, VS c16), wetness {:.3f}, in view "
              "space ({:+.3f}, {:+.3f}, {:+.3f}), in the picture at ({:.2f}, {:.2f}) (ndc {:+.2f}, "
              "{:+.2f}) by the light; {}; the sun's place used: ({:.2f}, {:.2f}) from {}; the "
              "rotation from {}; fades: height {:.2f}, rain {:.2f}, view {:.2f}, strength {:.3f}; "
              "{} first points, {} gated, {} lost, {} runs ({} at the end of the scene, {} at the "
              "mid-scene point); anchor of this frame: {}{}, copy of the main colour {}; {} sky draws "
              "told the sun disc's direction in all (this frame {})",
              kPrefix, frame_, OutcomeName(outcome_), sun_.elevation, sun_.wetness,
              sun_.to_sun[0], sun_.to_sun[1], sun_.to_sun[2], sun_.light_uv[0], sun_.light_uv[1],
              sun_.light_ndc[0], sun_.light_ndc[1], SkyText(sun_), sun_.uv[0], sun_.uv[1],
              sun_.from_sky ? "the sky's sun constant" : "the light's direction",
              sun_.vertex_pose ? "the vertex constants" : "the previous camera",
              sun_.fade_elevation, sun_.fade_wet, sun_.fade_view, sun_.strength, triggers_,
              gated_, lost_, runs_, runs_end_, runs_mid_,
              sky_taken_ ? shafts_anchor::AnchorName(anchor_) : "none (the sky was not taken)",
              policy_.fallen_back() ? " (the fall-back)" : "",
              sky_taken_ ? (copy_seen_ ? "seen" : "NOT seen") : "not watched", sky_draws_total_,
              sky_draws_);
}

}  // namespace mcla::gpu
