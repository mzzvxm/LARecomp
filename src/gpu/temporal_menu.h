#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal upscalers in the menu worlds: two rules (pure)
// ===========================================================================
// The owner's play build (2026-11-13, 4K, DLSS DLAA): "while the car is in the garage, it moves around slightly, like it's
// shaking". The garage is the pause view (the same 4x identities as the pause menu and the map, temporal.h "Views"), so
// the upscaler runs there on the pause view's own run, jittered, with the vectors of every frame. Nothing in a log of a
// garage with an upscaler exists (every garage run so far had the upscaler off), so these two rules are made from what
// the code does in a world it was never checked in, not from a measured cause: chain-garage.ps1 (perf\scripts) measures the
// car's frame-to-frame movement with each rule on and off (a CHECK, not a result).
//
// 1. THE GAME'S OWN VEHICLE MOTION (UseGameVehicleMotion). The vector pass takes, for a pixel of a vehicle, the vehicle's
//    own motion from the game's per-vehicle slots c(69 + 4 ID) of the anchor draw (vehicle_motion.h). The design said it
//    for play only (GHOSTING-DESIGN.md: "the pause and map view is not touched (4x samples are skipped)") and left one
//    open check for the garage: "the vehicle term must be the identity for a still car under an orbiting camera (CHECK
//    8 of 8)". PV4 then made the pause view a main view and the table came with it, unchecked. In a 4x frame the game
//    draws the world once for each of three tiles and hands out the stencil IDs in draw order (a garage frame shows up
//    to 6 IDs; the ambient occlusion's log counts them), and a slot the game did not fill for an ID may hold what an
//    earlier frame left there: a rigid, finite, sane matrix of some other car, which the vector pass only holds to a quarter of
//    the screen's short side from the camera's motion (CHECK: what the slots hold in a 4x frame has not been read from a
//    run; the stencil counts above are). A car that is still must get the camera's motion, which is what
//    the ambient occlusion already does in every 4x frame (ao.cpp: vehicles take the camera's motion like the world),
//    and what a car that turns slowly on its plate loses is under a pixel a frame at the frame rates this runs at. So
//    in the pause view (the menu, the garage, the map) the vehicles take the camera's motion; the debug cvar
//    mcla_gpu_pause_vehicle_vectors gives the game's slots back for an A/B.
//
// 2. A JITTER THE UPSCALER CANNOT RESOLVE (JitterHold). The scene is moved by a sub-pixel jitter in every armed frame; the
//    upscaler takes it out only when it accumulates the frames. The history restarts for a reason that is known only at
//    the anchor, after the jitter was applied (a camera cut, a size change, a gap, the game's own previous matrix not
//    usable), so a frame that restarts the history is jittered and then resolved from one frame: the picture is the raw
//    jittered one. One such frame in a long run is invisible, a world in which EVERY frame restarts it (a camera the cut
//    rule or the previous-matrix test cannot follow, which the garage may be) shakes by the jitter all the time. After
//    kHoldAfter frames in a row that restarted the history for a reason of its own (not the first frame of a spell or
//    the change of view, which are plain frames already) the jitter is held at zero: the frame is drawn still, as a
//    plain anti-aliased one, until a frame comes that keeps its history. mcla_gpu_temporal_jitter_hold (debug) is the
//    A/B.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::temporal {

// Whether the game's per-vehicle slots feed the vectors of a vehicle's pixels in this frame: always in the game view, in the
// pause view (the pause menu, the garage, the map) only when the debug cvar says so.
constexpr bool UseGameVehicleMotion(bool pause_view, bool pause_view_allowed) {
  return !pause_view || pause_view_allowed;
}

class JitterHold {
 public:
  // Frames in a row that restarted the history for their own reason before the jitter is held.
  static constexpr uint32_t kHoldAfter = 3;

  // Once for each frame whose anchor was accepted: whether the frame restarted the history for a reason of its own
  // (a cut, a size change, a gap, no usable previous matrix: not the first frame of a spell, not a change of view).
  // Returns true when the hold changed with this frame (began or ended).
  bool Observe(bool restarted) {
    const bool before = Held();
    if (restarted) {
      if (streak_ < kHoldAfter) {
        ++streak_;
      }
      ++restarted_total_;
    } else {
      streak_ = 0;
    }
    const bool now = Held();
    if (now && !before) {
      ++holds_;
    }
    return now != before;
  }

  // The jitter of the next frame is zero.
  bool Held() const { return streak_ >= kHoldAfter; }
  uint32_t streak() const { return streak_; }
  uint64_t holds() const { return holds_; }
  uint64_t restarted_total() const { return restarted_total_; }
  void Clear() { streak_ = 0; }

 private:
  uint32_t streak_ = 0;
  uint64_t holds_ = 0;           // how many times the hold began
  uint64_t restarted_total_ = 0;
};

// The jitter a frame takes: the sequence's, or zero while held (`enabled`: the debug cvar's A/B).
inline float HeldJitter(float jitter, bool held, bool enabled) {
  return (held && enabled) ? 0.0f : jitter;
}

}  // namespace mcla::gpu::temporal
