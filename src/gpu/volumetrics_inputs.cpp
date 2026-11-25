// MCLA GPU renderer -- the volumetric light's light inputs. See volumetrics_inputs.h.

#include "volumetrics_inputs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "light_manager.h"
#include "light_project.h"

// feat/headlights (the rig's set and the shadow replay) is not on every branch this is built on: its headers are the test.
#if __has_include("headlight_types.h")
#include "headlight_types.h"
#define MCLA_VOL_HAS_HEADLIGHTS 1
#else
#define MCLA_VOL_HAS_HEADLIGHTS 0
#endif
#if __has_include("local_shadow.h")
#include "local_shadow.h"
#define MCLA_VOL_HAS_SHADOW 1
#else
#define MCLA_VOL_HAS_SHADOW 0
#endif

namespace mcla::gpu {

namespace vol = volumetrics;
namespace vl = vol_lamps;

namespace {

constexpr const char* kPrefix = "[volumetrics]";
// A copy of the lamp list this old is not the game's list any more (the hook is not running, a menu, a day with the pass idle).
constexpr std::chrono::milliseconds kMaxListAge{500};
// A gap between two Fills this long starts the picks again (a menu, a cut scene): the lamps fade in with the world, not from the last place.
constexpr float kGapSeconds = 0.5f;
// Fills without a single list from the guest hook before the log says so (10 s at 60 FPS).
constexpr uint64_t kHookWarnFills = 600;

}  // namespace

void VolumetricsInputs::Fill(const vol::VolFrameContext& frame, vol::VolInputs* out) {
  ++fills_;
  const int inputs = larecomp::settings::mcla_gpu_vol_inputs.Get();
  const int test = larecomp::settings::mcla_gpu_vol_test.Get();
  // The core's test lights (mcla_gpu_vol_test) stand in for the game's lights of their kind while they are on.
  const bool lamps_on = (inputs & 1) != 0 && (test & 1) == 0;
  const bool heads_on = (inputs & 2) != 0 && (test & 2) == 0;
  const uint32_t lamps_before = out->lamp_count;  // the core's test lamp, if any: the game's lamps come after it
  // The hook only copies the list while somebody wants it (the light manager's pick reads the same table).
  vl::SetWanted(lamps_on);
  const int limit_setting = larecomp::settings::mcla_gpu_vol_lamp_limit.Get();
  const uint32_t limit = limit_setting > 0 ? uint32_t(limit_setting) : vol::kMaxLamps;
  // LU-4b: from the light manager's frame (mcla_gpu_light_frame bit 2) or from this producer's own code. The manager answers false when it is off or its frame is not open.
  const bool from_frame = light_manager_ != nullptr && light_manager_->Serving(light_project::kFlipVolume);
  if (from_frame != from_frame_) {
    // A live switch: the producer that was not running has stale picks (or none): it starts afresh, its lamps at full weight.
    from_frame_ = from_frame;
    picker_.Reset();
    picked_ = vol::PickResult{};
    have_camera_ = false;
  }
  if (from_frame) {
    const bool proof = light_manager_->Checking();
    vol::VolInputs legacy;  // the proof run: this producer's own result for the same entry, to compare the frame's with
    if (proof) {
      legacy = *out;
      FillLegacy(frame, &legacy, lamps_on, heads_on, limit);
    }
    light_manager_->FillVolumeInputs(frame, limit, lamps_on, heads_on, out, proof ? &list_ : nullptr);
    if (proof) {
      light_manager_->CheckVolumeBytes(*out, legacy);
    }
  } else {
    FillLegacy(frame, out, lamps_on, heads_on, limit);
  }
  if (light_manager_ != nullptr && light_manager_->active()) {
    light_manager_->NoteVolumeInputs(frame, *out, lamps_on, heads_on, lamps_before);  // the census looks at what the volume was given
  }
  const int every = larecomp::settings::mcla_gpu_vol_census.Get();
  if (every > 0 && (fills_ <= 3 || frame.frame % uint64_t(every) == 0)) {
    Census(frame, *out, lamps_on, heads_on, from_frame);
  }
}

// This producer's own code (what Fill was before LU-4b): the street lamps from the game's list through the volume's picker, the headlights from the rig's set and the replay's frame.
void VolumetricsInputs::FillLegacy(const vol::VolFrameContext& frame, vol::VolInputs* out, bool lamps_on, bool heads_on, uint32_t limit) {
  if (lamps_on) {
    FillLamps(frame, out, limit);
  } else {
    picker_.Reset();
    picked_ = vol::PickResult{};
    list_.clear();
    list_seq_ = 0;
    list_stale_ = true;
  }
  if (heads_on) {
    FillHeadlightInputs(out);
  } else {
    head_count_ = 0;
    head_shadowed_ = 0;
  }
}

void VolumetricsInputs::FillLamps(const vol::VolFrameContext& frame, vol::VolInputs* out, uint32_t limit) {
  // The newest list: a new copy replaces ours, none that is recent drops what we hold (the hook stopped: the lamps are gone, not frozen).
  bool stale = false;
  vl::Global().Latest(&list_, &list_seq_, &stale, kMaxListAge);
  if (stale) {
    list_.clear();
    list_seq_ = 0;
  }
  list_stale_ = stale;
  if (vl::Global().published() == 0) {
    ++fills_without_list_;
    if (!hook_warned_ && fills_without_list_ >= kHookWarnFills) {
      hook_warned_ = true;
      REXLOG_WARN("{} the game's lamp list has not come once in {} frames: the guest hook MCLAVolLampList (0x82302814, larecomp_config.toml) is not in this build "
                  "(a codegen run is needed), or the game never reaches the cone drawer: no street lamp is lit in the volume (mcla_gpu_vol_test=1 puts a test lamp in)",
                  kPrefix, fills_without_list_);
    }
  }

  // The camera's step and the time since the last pick: a cut or a gap starts the lamps again at full weight.
  bool reset = false;
  if (have_camera_) {
    float step2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float d = frame.camera.pos[i] - last_camera_[i];
      step2 += d * d;
    }
    reset = step2 > vol::kCutMetres * vol::kCutMetres;
  }
  reset = reset || frame.elapsed_s > kGapSeconds;
  for (int i = 0; i < 3; ++i) {
    last_camera_[i] = frame.camera.pos[i];
  }
  have_camera_ = true;

  // The room: the level's lamps less the ones already in (the core's test lamp), and at most the limit.
  const uint32_t room_all = std::min(frame.grid.lamps, vol::kMaxLamps);
  const uint32_t have = out->lamp_count;
  const uint32_t room = std::min(limit, room_all > have ? room_all - have : 0u);
  vol::PickParams params;
  params.budget = room;
  params.dt = frame.elapsed_s;
  params.near_m = frame.grid.near_m;
  params.far_m = frame.grid.far_m;
  params.reset = reset;
  picker_.Update(list_, frame.camera, params, &picked_);
  uint32_t n = have;
  for (uint32_t i = 0; i < picked_.count && n < vol::kMaxLamps; ++i) {
    out->lamps[n++] = picked_.lamps[i];
  }
  out->lamp_count = n;
  if (picked_.count != 0 && !lamps_seen_) {
    lamps_seen_ = true;
    REXLOG_INFO("{} the first street lamps from the game's list: {} records, {} lit and in view, {} picked of a budget of {} (the game's units: colour x intensity x "
                "fade, the plateau as the near radius, the cone from the angle and the spread)",
                kPrefix, picked_.listed, picked_.usable, picked_.wanted + picked_.fading, room);
  }
}

void VolumetricsInputs::FillHeadlightInputs(vol::VolInputs* out) {
  head_count_ = 0;
  head_shadowed_ = 0;
#if MCLA_VOL_HAS_HEADLIGHTS
  // The rig's set of this frame (the player's first); an empty set leaves what is in `out`.
  const HeadlightSet& set = CurrentHeadlights();
  if (set.count == 0) {
    return;
  }
  vol::FillHeadlights(set, out);
  for (uint32_t i = 0; i < out->headlight_count; ++i) {
    vol::ApplyHeadlightUnits(&out->headlights[i]);
  }
  head_count_ = out->headlight_count;
#if MCLA_VOL_HAS_SHADOW
  // The replay's maps of this frame (OnResolve ran before the volume's first point) and the atlas they are in.
  if (local_shadow_) {
    const LocalShadowAtlas& atlas = local_shadow_->AtlasInfo();
    const vol::ShadowAtlasState state{atlas.valid, atlas.slot, atlas.width, atlas.height};
    vol::FillShadows(&local_shadow_->Frame(), state, out);
    atlas_slot_ = out->shadow_atlas_slot;
    atlas_w_ = atlas.width;
    atlas_h_ = atlas.height;
    for (uint32_t i = 0; i < out->headlight_count; ++i) {
      head_shadowed_ += out->headlights[i].shadow_view >= 0 ? 1u : 0u;
    }
    if (head_shadowed_ != 0 && !shadows_seen_) {
      shadows_seen_ = true;
      REXLOG_INFO("{} the first headlight shadow maps: {} of {} lights read a view, {} views in a {}x{} atlas (bindless slot {}); the inject reads it as a depth plane "
                  "in compute (the replay's last barrier must name the compute stage: volumetrics_headlights.h)",
                  kPrefix, head_shadowed_, out->headlight_count, out->shadow_view_count, atlas.width, atlas.height, out->shadow_atlas_slot);
    }
  }
#endif
  if (head_count_ != 0 && !headlights_seen_) {
    headlights_seen_ = true;
    REXLOG_INFO("{} the first headlights from the rig: {} (the player's first), in the lit shaders' units (colour x {:.1f} x {:.0f}, near radius {:.0f} m)", kPrefix,
                head_count_, vol::kHeadlightPeak, vol::kHeadlightFalloffDistance * vol::kHeadlightFalloffDistance, vol::kHeadlightFalloffDistance);
  }
#else
  (void)out;
#endif
}

void VolumetricsInputs::Census(const vol::VolFrameContext& frame, const vol::VolInputs& out, bool lamps_on, bool heads_on, bool from_frame) {
  if (from_frame) {
    // LU-4b: this producer's own picks are not what the volume was given (the light manager's frame is; its census line has the pick's numbers).
    REXLOG_INFO("{} inputs, frame {}: from the light manager's frame (mcla_gpu_light_frame): {} street lamps (budget {}), {} headlights, {} shadow views, atlas slot {}", kPrefix, frame.frame,
                lamps_on ? out.lamp_count : 0u, frame.grid.lamps, heads_on ? out.headlight_count : 0u, out.shadow_view_count,
                out.shadow_atlas_slot == vol::kNoSlot ? std::string("none") : std::to_string(out.shadow_atlas_slot));
    return;
  }
  std::string lamps = "off";
  if (lamps_on) {
    // The nearest picked lamp (where a pole stands: the toolkit scenes of the beam through a pole are placed by it), its distance and the game's intensity.
    std::string nearest = "none";
    float best = 1e30f;
    for (uint32_t i = 0; i < picked_.count; ++i) {
      const vol::VolLamp& l = picked_.lamps[i];
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float d = l.pos[k] - frame.camera.pos[k];
        d2 += d * d;
      }
      if (d2 < best) {
        best = d2;
        nearest = std::format("{:.1f} m at ({:.2f}, {:.2f}, {:.2f}), intensity {:.0f}", std::sqrt(d2), l.pos[0], l.pos[1], l.pos[2], vol::LampNearSq(l) / vol::kLampNearShare);
      }
    }
    lamps = std::format("list {} (copy #{}{}, {} published), {} lit and in view, picked {} + {} fading, best score {:.3f}, {} in the volume (budget {}){}, nearest {}",
                        picked_.listed, list_seq_, list_stale_ ? ", none recent" : "", vl::Global().published(), picked_.usable, picked_.wanted, picked_.fading,
                        picked_.best_score, out.lamp_count, frame.grid.lamps,
                        picked_.dropped != 0 ? std::format(", {} dropped at once", picked_.dropped) : std::string(), nearest);
  }
  std::string heads = "off";
  if (heads_on) {
#if MCLA_VOL_HAS_HEADLIGHTS
    heads = std::format("{} from the rig, {} reading a shadow view, {} views, atlas {}x{} slot {}", head_count_, head_shadowed_, out.shadow_view_count, atlas_w_, atlas_h_,
                        out.shadow_atlas_slot == vol::kNoSlot ? std::string("none") : std::to_string(out.shadow_atlas_slot));
#else
    heads = "not on this branch (feat/headlights)";
#endif
  }
  REXLOG_INFO("{} inputs, frame {}: street lamps: {}; headlights: {}", kPrefix, frame.frame, lamps, heads);
}

}  // namespace mcla::gpu
