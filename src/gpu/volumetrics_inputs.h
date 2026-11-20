#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's light inputs: the game's street lamps and the cars' headlights (VOL-INPUTS)
// ===========================================================================
// The producer behind volumetrics_types.h's VolInputSource, plugged in with Volumetrics::SetInputSource (renderer.cpp). The core fills the sun, the
// cascades, the haze and the weather; this fills VolInputs::lamps (the street lamps) and VolInputs::headlights and the shadow views (the cars' lights),
// once a frame at the volume's first point.
//
// STREET LAMPS: the game's own per-frame list (guest 0x8288B8D8, mean 64 and at most 83 spot lamps a night frame, 6298 in the city), copied at the cone
// drawer's entry by a read-only guest hook (src/mc_engine/hooks/vol_lamps.cpp, vol_lamp_list.h) into a table, the newest copy taken here. volumetrics_lamps.h
// has the law (the game's units: colour x intensity x fade, the plateau as the near radius, the cone from the angle and the spread), the calibration and
// the pick (the top GridDesc::lamps by their contribution to the view, hysteresis and fades). Nothing is drawn by day: the list is empty then.
//
// HEADLIGHTS: the rig's HeadlightSet (headlight_types.h: CurrentHeadlights()) in the volume's units (volumetrics_headlights.h) and the replay's
// LocalShadowFrame with its atlas (LocalShadow::Frame() and AtlasInfo(), handed in with SetLocalShadow). The two headers are found with __has_include (both are
// in the tree on main; a tree without them simply does not read the headlights, the lamps and the core's test lights work alone).
//
// SETTINGS (debug only): mcla_gpu_vol_inputs (1 the lamps, 2 the headlights: the default reads both) and mcla_gpu_vol_lamp_limit (at most this many lamps;
// 0: the level's). mcla_gpu_vol_test (the core's test lights) turns the game's lamps (bit 1) and headlights (bit 2) off in favour of the test light of that kind.
//
// Threading: Fill on the render thread. The lamp table is filled by a guest thread (vol_lamp_list.h: a mutex).
// ===========================================================================

#include <cstdint>
#include <vector>

#include "volumetrics_lamps.h"
#include "volumetrics_headlights.h"
#include "volumetrics_types.h"

namespace mcla::gpu {

class LocalShadow;  // local_shadow.h

class VolumetricsInputs final : public volumetrics::VolInputSource {
 public:
  // The Fill of the frame (volumetrics_types.h): adds the lamps and the headlights to what the core put in `out`.
  void Fill(const volumetrics::VolFrameContext& frame, volumetrics::VolInputs* out) override;

  // The headlight shadow maps' producer (the Renderer's LocalShadow): its views and its atlas are what the headlights are shadowed by.
  // Null (the default): no shadows. It must outlive this.
  void SetLocalShadow(const LocalShadow* shadow) { local_shadow_ = shadow; }

 private:
  void FillLamps(const volumetrics::VolFrameContext& frame, volumetrics::VolInputs* out, uint32_t limit);
  void FillHeadlightInputs(volumetrics::VolInputs* out);
  void Census(const volumetrics::VolFrameContext& frame, const volumetrics::VolInputs& out, bool lamps_on, bool heads_on);

  const LocalShadow* local_shadow_ = nullptr;

  // the lamps
  volumetrics::LampPicker picker_;
  volumetrics::PickResult picked_;
  std::vector<vol_lamps::GameLamp> list_;  // the newest list the table gave
  uint64_t list_seq_ = 0;                  // its number
  bool list_stale_ = true;                 // the table had nothing recent at the last Fill
  float last_camera_[3] = {};
  bool have_camera_ = false;

  // the headlights, for the census
  uint32_t head_count_ = 0;
  uint32_t head_shadowed_ = 0;
  uint32_t atlas_slot_ = volumetrics::kNoSlot;
  uint32_t atlas_w_ = 0;
  uint32_t atlas_h_ = 0;
  bool shadows_seen_ = false;   // a shadow view has been handed over (logged once)
  bool headlights_seen_ = false;

  // the log
  uint64_t fills_ = 0;
  uint64_t fills_without_list_ = 0;  // Fills while the hook had never published (it is not in the build, or nothing is running)
  bool hook_warned_ = false;
  bool lamps_seen_ = false;
};

}  // namespace mcla::gpu
