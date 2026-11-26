// MCLA GPU renderer -- the probe field's integration. See gi_integration.h.

#include "gi_integration.h"

#include "cpu_timing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "ao.h"
#include "city_rt_scene.h"
#include "gi_apply.h"
#include "gpu_class.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "temporal_identities.h"
#include "world_clock.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gi]";

// The game draws a sector at its full detail within 300 m times lod_city_scale of the camera (CITY-RT-SURVEY section 1.2; city_rt_shadow.cpp does the same): the safe range is asked with it.
float GameHiRadius() { return float(300.0 * gpu_class::CityLodScale(larecomp::settings::lod_city_scale.Get())); }

}  // namespace

void GiIntegration::Initialize(GiField* field, GiApply* apply, AmbientOcclusion* ao, CityRtScene* city) {
  field_ = field;
  apply_ = apply;
  ao_ = ao;
  city_ = city;
  trigger_frame_ = 0;
  world_frame_ = 0;
  city_view_ = false;
  idle_until_ = 0;
  cut_pending_ = false;
  relight_.Reset();
  relight_pending_ = false;
  have_camera_ = false;
  have_light_ = false;
  logged_any_ = false;
}

void GiIntegration::Shutdown() {
  gi_setting::NoteReplaced(false);
  gi_setting::NoteUpdateBuilt(false);
  update_ = nullptr;
  want_ = nullptr;
  field_ = nullptr;
  apply_ = nullptr;
  ao_ = nullptr;
  city_ = nullptr;
}

void GiIntegration::SetUpdate(UpdateFn update, WantFn want) {
  update_ = std::move(update);
  want_ = std::move(want);
  gi_setting::NoteUpdateBuilt(static_cast<bool>(update_));
}

void GiIntegration::LogDecision(const gi_setting::Decision& d) {
  if (logged_any_ && d.level == logged_level_ && d.layout == logged_layout_ && d.gate == logged_gate_) {
    return;
  }
  logged_any_ = true;
  logged_level_ = d.level;
  logged_layout_ = d.layout;
  logged_gate_ = d.gate;
  const char* why = d.gate == gi_setting::Gate::kRuns ? "" : gi_setting::RowSuffix(d.gate);
  REXLOG_INFO("{} ray traced bounce light: {} (layout {}, ray tracing {}{}{}); the screen-space bounce light is the source {}", kPrefix, gi::LevelName(d.level), gi::LayoutName(d.layout), gi::RungName(d.rung),
              *why ? ", the row is grey:" : "", why, d.level == gi::Level::kOff ? "everywhere" : "except where the field stands");
}

void GiIntegration::LatchPoseAndLight() {
  if (ao_ == nullptr) {
    return;
  }
  // This frame's own pose where the vertex constants are trusted for it, else the last frame's camera; the light of the frame's draws so far: the units the apply and the baseline are in (gi_apply.h G3h).
  float pose[16];
  bool own_pose = false;
  if (ao_->TriggerViewInverse(pose, &own_pose)) {
    for (int i = 0; i < 3; ++i) {
      camera_[i] = double(pose[12 + i]);
    }
    have_camera_ = true;
  }
  float ambient[3] = {}, sun[3] = {};
  double to_sun[3] = {};
  bool light_ok = false;
  if (ao_->FrameLight(ambient, sun, to_sun)) {
    for (int i = 0; i < 3; ++i) {
      sun_rgb_[i] = sun[i];
      ambient_rgb_[i] = ambient[i];
    }
    sun_sin_elevation_ = float(to_sun[1]);  // the world's y is up
    have_light_ = true;
    light_ok = true;
  }
  // The city lighting drives relights in play, pause and photo mode.
  if (relight_.Observe(frame_, mcla::gpu::world_clock::HourJumps(), light_ok, to_sun)) {
    relight_pending_ = true;
    REXLOG_INFO("{} frame {}: the world's hour jumped (the clock and the sun's direction both moved): the probes are relit", kPrefix, frame_);
  }
}

void GiIntegration::BeginFrame(uint64_t frame) {
  frame_ = frame;
  if (field_ == nullptr) {
    return;
  }
  // What the last frame left: the published frame still stands until the field's own BeginFrame empties it. The BOUNCE LIGHT row reads it from the menu's thread.
  gi_setting::NoteReplaced(gi_setting::ReplacedBy(field_->frame()));

  // The watchdog: the field stood and the apply could not draw its light for a run of frames (a cutscene with no G-buffer planes, a pipeline that failed): the screen-space bounce light was skipped for
  // nothing. Idle the field for a while; it fades out over 0.5 s and the screen-space bounce light crossfades back.
  if (apply_ != nullptr && apply_->Starved()) {
    apply_->ClearStarved();
    idle_until_ = frame + kStarvedHoldFrames;
    ++watchdog_trips_;
    REXLOG_WARN("{} frame {}: the probe field stood but its bounce light could not be drawn for {} frames in a row (no G-buffer planes, no pipeline, no camera or light: see the '[gi apply]' lines): the field "
                "idles for {} frames and the screen-space bounce light takes over meanwhile ({} times so far)",
                kPrefix, frame, uint32_t(GiApply::kStarvedFrames), kStarvedHoldFrames, watchdog_trips_);
  }

  // The gate the row and the setting read: the update is built (G2 gave SetUpdate a function), or a test says to run the field without it (mcla_gpu_rt_gi_dry_run: the field is made and maintained, nothing
  // traces into it, so it never becomes ready and the screen-space bounce light stays the source).
  gi_setting::NoteUpdateBuilt(static_cast<bool>(update_) || larecomp::settings::mcla_gpu_rt_gi_dry_run.Get());
  const gi_setting::Decision decision = gi_setting::Current();
  LogDecision(decision);
  if (want_) {
    want_(decision.level);  // G2: the structures and the hit data the field's rays need (rt_scene_service.h)
  }

  GiField::Inputs in;
  in.rung = decision.rung;
  in.level = decision.level;
  in.world_ok = city_view_ && frame >= world_frame_ && frame - world_frame_ <= kWorldStaleFrames && have_camera_ && have_light_ && frame >= idle_until_;
  for (int i = 0; i < 3; ++i) {
    in.camera[i] = camera_[i];
    in.sun_rgb[i] = sun_rgb_[i];
    in.ambient_up_rgb[i] = ambient_rgb_[i];
    in.ambient_down_rgb[i] = ambient_rgb_[i];  // CHECK G4d
  }
  in.sun_sin_elevation = sun_sin_elevation_;
  in.time_s = mc::cinematic::SceneClockNowMs(frame) * 1.0e-3;
  in.safe_range_m = city_ != nullptr && city_->initialized() ? city_->SafeRange(GameHiRadius()) : 0.0f;
  in.strength = GiApply::LevelStrength();
  in.bounce_weight = gi_setting::BounceWeightNow();
  in.cut = gi_field::CutFor(cut_pending_, relight_pending_);  // G4f: a cut the game asked for, else an hour jump (G4g); the camera's own step is GiField::Move's
  cut_pending_ = false;
  if (in.world_ok) {
    relight_pending_ = false;
  }
  in.frame = frame;
  field_->BeginFrame(in);
}

bool GiIntegration::OnResolve(const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t frame_slot,
                              gbuffer::PauseFxView view, bool has_planes) {
  const cpu_timing::Scope cpu_scope(cpu_timing::Section::kGiField);
  if (field_ == nullptr || list == nullptr || !summary.depth_copy || summary.source == nullptr) {
    return false;
  }
  // The trigger: the first copy of the MAIN view's depth in a frame (the mid-scene copy). Another depth (a shadow map, a side view) is not it.
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (samples == 0 || trigger_frame_ == frame_) {
    return false;
  }
  trigger_frame_ = frame_;
  world_frame_ = frame_;
  city_view_ = gbuffer::SupportsCityEffects(samples == temporal::kPauseSamples, view);
  if (!city_view_) {
    have_camera_ = false;
    have_light_ = false;
    return false;
  }
  LatchPoseAndLight();

  // CHECK: Confirm that entering photo mode never suppresses SSGI while its sidecar is unavailable.
  if (!has_planes || !field_->running()) {
    return false;
  }
  // RecordMaintenance, the probe update, RecordFinish (gi_field.h THE FRAME): outside any render pass (the caller closed it), BEFORE the occlusion plans its frame and asks for the screen-space bounce light's share.
  if (!field_->RecordMaintenance(list, frame_slot) || !field_->update_ready()) {
    return false;  // no field this frame (the world is not the play view's, the safe range is 0, the pipelines are not made yet): nothing was recorded
  }
  if (update_) {
    update_(list, frame_slot, field_->update_frame());
  }
  return field_->RecordFinish(list, frame_slot);
}

}  // namespace mcla::gpu
