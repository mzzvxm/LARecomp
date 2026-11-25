#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's street lamp pick as a driver of its own (perf/LIGHT-UNIFY.md 7.1 step U4b, package LU-4)
// ===========================================================================
// VolumetricsInputs::FillLamps does three things with the game's lamp list: it decides whether the pick starts again (the camera jumped, or a long gap since the last fill), works out how
// many lamps there is room for (the level's, less the ones already in the inputs, at most the debug limit) and runs the LampPicker. With LU-4b the LightManager picks the lamps for the
// frame it makes (the volume then reads them from it), so the same three things are here as a class that takes the list and the frame's context and nothing else: no guest, no renderer,
// no table of the hook. The legacy FillLamps is left as it was (it runs beside this one while the census compares the two, and alone while the flip is off); both give the same lamps
// from the same inputs, which light_project_test.cpp holds over a drive.
//
// A lamp that is picked is one of the LampPicker's tracks: PickResult::records and weights say which record of the game's list it is and the weight folded into its colour, the lamp itself
// is the manager's Light (light_convert.h LightFromGameLamp, Light::vol_weight) and its VolLamp is the projection ToVolLamp makes of it.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <vector>

#include "vol_lamp_list.h"
#include "volumetrics_lamps.h"
#include "volumetrics_types.h"

namespace mcla::gpu::volumetrics {

// A gap between two fills this long starts the picks again (a menu, a cut scene): the lamps fade in with the world, not from the last place (volumetrics_inputs.cpp kGapSeconds).
inline constexpr float kPickGapSeconds = 0.5f;

class VolumeLampPick {
 public:
  void Reset() {
    picker_.Reset();
    picked_ = PickResult{};
    have_camera_ = false;
  }

  // The pick of one fill. `list`: the newest copy of the game's lamp list (empty when none is recent); `frame`: the volume's context (the camera, the grid's lamps and planes, the seconds
  // since the last fill); `have`: the lamps already in the volume's inputs (the core's test lamp); `limit`: at most this many (0 or more than kMaxLamps: the level's). The result's
  // lamps are what FillLamps appends after the `have` that are there.
  const PickResult& Update(const std::vector<vol_lamps::GameLamp>& list, const VolFrameContext& frame, uint32_t have, uint32_t limit) {
    // The camera's step and the time since the last pick: a cut or a gap starts the lamps again at full weight.
    bool reset = false;
    if (have_camera_) {
      float step2 = 0.0f;
      for (int i = 0; i < 3; ++i) {
        const float d = frame.camera.pos[i] - last_camera_[i];
        step2 += d * d;
      }
      reset = step2 > kCutMetres * kCutMetres;
    }
    reset = reset || frame.elapsed_s > kPickGapSeconds;
    for (int i = 0; i < 3; ++i) {
      last_camera_[i] = frame.camera.pos[i];
    }
    have_camera_ = true;
    // The room: the level's lamps less the ones already in (the core's test lamp), and at most the limit.
    const uint32_t cap = limit > 0 ? limit : kMaxLamps;
    const uint32_t room_all = std::min(frame.grid.lamps, kMaxLamps);
    const uint32_t room = std::min(cap, room_all > have ? room_all - have : 0u);
    PickParams params;
    params.budget = room;
    params.dt = frame.elapsed_s;
    params.near_m = frame.grid.near_m;
    params.far_m = frame.grid.far_m;
    params.reset = reset;
    picker_.Update(list, frame.camera, params, &picked_);
    room_ = room;
    return picked_;
  }

  const PickResult& picked() const { return picked_; }
  uint32_t room() const { return room_; }  // the room of the last pick, for the census

 private:
  LampPicker picker_;
  PickResult picked_;
  float last_camera_[3] = {};
  bool have_camera_ = false;
  uint32_t room_ = 0;
};

}  // namespace mcla::gpu::volumetrics
