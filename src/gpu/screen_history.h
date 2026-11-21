#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen products: the camera history, the one place the frames' cameras are kept (SS-1, pure, no GPU)
// ===========================================================================
// perf/LIGHT-UNIFY.md section 5.2 `history` and `camera`. Each frame's camera exists only at the game's motion blur draw (the anchor), which comes AFTER the mid-scene depth copy where every
// screen pass runs, so a pass at the trigger of frame N works with the camera of frame N-1 (its lens, its view-projection) and learns frame N's own at the anchor. What a consumer asks of the
// history is always one of four things, and this class answers them (they were CarSelfRefl's own bookkeeping, previous_, have_previous_, previous_frame_, previous_pause_, lens_used_ and the
// lens test of its OnAnchor, until SS-1 moved them here; the AO keeps its own cut logic until SS-2):
//
//   Previous(frame, pause)   the camera of the frame before `frame` when it was of the same kind of view (the play view's, or the pause menu's / garage's / map's 4x view, whose lenses differ by
//                            25 to 54 degrees: never mixed): the lens the passes at the trigger of `frame` linearise the depth with. Null: none (the frame before had no usable anchor, was of the
//                            other kind or is not the frame before: a cut, a skipped frame, a pause).
//   Before(frame, pause)     the camera of the frame before THAT one, when it too was the frame before it and of the same kind: with Previous, the pair a reprojection needs (the products'
//                            camera: view_projection and prev_view_projection).
//   Current(frame)           the camera of `frame`'s own anchor, after the anchor. Null before it or when the anchor gave none.
//   LensAgrees(frame)        whether `frame`'s own lens equals the one the trigger worked with (Previous) within kLensTolerance on the vertical field of view, the aspect, the near and the far
//                            plane: an image made at the trigger with that lens belongs to this camera only when they agree (a pause view's zoom, a sweep: it does not).
//
// A frame gives a camera to OnAnchor or none: a frame with draws of both kinds of view or of neither, a size c62 does not say, constants that are not a camera (temporal::Reject) pass null and
// the history starts again. The frame numbers are the caller's own count of guest presents; every consumer counts the same ones.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "screen_frame.h"
#include "temporal_camera.h"

namespace mcla::gpu::screen {

// How much a lens number may differ between the frame an image was linearised with and the frame it was made in (car_selfrefl.cpp's kLensTolerance, which this is).
inline constexpr double kLensTolerance = 0.05;

inline double RelativeDifference(double a, double b) {
  return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
}

// The four numbers of the lens, each within kLensTolerance (a is the frame's own, b the one the trigger worked with: the relative difference is taken against b).
inline bool LensesAgree(const temporal::Camera& a, const temporal::Camera& b) {
  return RelativeDifference(a.fov_y, b.fov_y) < kLensTolerance && RelativeDifference(a.aspect, b.aspect) < kLensTolerance &&
         RelativeDifference(a.near_plane, b.near_plane) < kLensTolerance && RelativeDifference(a.far_plane, b.far_plane) < kLensTolerance;
}

// The products' camera pair: `out` already holds the view-projection of `previous` (the AO fills it from the camera it worked with, converted to float); when that really is `previous`'s, the
// view-projection of `before` (the frame before it) goes into prev_view_projection and kCameraHasPrevious is set. False (nothing changed) when there is no pair or the camera `out` holds is
// another one (a pair of matrices from two different histories would reproject by the wrong motion).
inline bool FillPreviousViewProjection(const temporal::Camera* previous, const temporal::Camera* before, ScreenCamera* out) {
  if (!previous || !before || (out->flags & kCameraValid) == 0) {
    return false;
  }
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      if (float(previous->view_projection.m[row][col]) != out->view_projection[row * 4 + col]) {
        return false;
      }
    }
  }
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      out->prev_view_projection[row * 4 + col] = float(before->view_projection.m[row][col]);
    }
  }
  out->flags |= kCameraHasPrevious;
  return true;
}

class CameraHistory {
 public:
  // The anchor of `frame`, in a view of kind `pause` (true: the pause menu's, the garage's or the map's world). `camera` null: the frame gave none; the history starts again.
  void OnAnchor(uint64_t frame, bool pause, const temporal::Camera* camera) {
    lens_known_ = false;
    lens_agrees_ = false;
    if (!camera) {
      last_.valid = false;
      before_.valid = false;
      return;
    }
    const bool follows = last_.valid && last_.frame + 1 == frame && last_.pause == pause;
    // The trigger of this frame worked with the lens of last_ (Previous), so that is the lens this frame's own is held against.
    lens_known_ = follows;
    lens_agrees_ = follows && LensesAgree(*camera, last_.camera);
    lens_frame_ = frame;
    if (follows) {
      before_ = last_;
    } else {
      before_.valid = false;
    }
    last_.camera = *camera;
    last_.frame = frame;
    last_.pause = pause;
    last_.valid = true;
  }

  const temporal::Camera* Previous(uint64_t frame, bool pause) const {
    return last_.valid && last_.frame + 1 == frame && last_.pause == pause ? &last_.camera : nullptr;
  }
  const temporal::Camera* Before(uint64_t frame, bool pause) const {
    return Previous(frame, pause) && before_.valid ? &before_.camera : nullptr;
  }
  const temporal::Camera* Current(uint64_t frame) const { return last_.valid && last_.frame == frame ? &last_.camera : nullptr; }
  bool LensAgrees(uint64_t frame) const { return lens_known_ && lens_frame_ == frame && lens_agrees_; }

  // The newest anchor's camera whatever its frame, when it was of the pause kind (what the frame's world is classified by at the frame's end: menu_world.h); null otherwise.
  const temporal::Camera* LastPauseCamera() const { return last_.valid && last_.pause ? &last_.camera : nullptr; }
  // True once an anchor gave a camera and nothing has taken it back.
  bool has_camera() const { return last_.valid; }
  // Forgets everything (a consumer that was switched off and on again does not trust the cameras of its time away: the frames are not adjacent anyway).
  void Reset() {
    last_.valid = false;
    before_.valid = false;
    lens_known_ = false;
    lens_agrees_ = false;
  }

 private:
  struct Anchor {
    temporal::Camera camera;
    uint64_t frame = 0;
    bool pause = false;
    bool valid = false;
  };
  Anchor last_;
  Anchor before_;
  bool lens_known_ = false;
  bool lens_agrees_ = false;
  uint64_t lens_frame_ = 0;
};

}  // namespace mcla::gpu::screen
