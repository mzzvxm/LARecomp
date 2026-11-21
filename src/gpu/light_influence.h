#pragma once
// ===========================================================================
// MCLA GPU renderer -- how much a light has to do with the picture: the view's frustum and a spot light's reach into it
// ===========================================================================
// HL-EDGE-FADE (owner, 2026-11-20): "as traffic cars get near the edge of the frame, their headlights seem to turn off, or at least get much weaker". The two places that
// pick cars for a light (the rig's Tracker: four lights; the shadow replay's ViewPicker: a few views) ranked a car by where its LAMP was on the screen and gave a car whose lamp
// was off it 0.35 of the score: a car at the edge of the frame lit the road ahead of it, in the picture, and was ranked as if it lit nothing. And the rig only knows a car while
// the game draws it (the game culls a car whose bounds leave the view), so the light of a car that left the frame was dropped within 8 frames although its beam still lit the
// road in the frame. What a light is worth to a picture is how much of its beam is in the view, not where its lamp is: this header measures that.
//
// Pure: plain numbers, no renderer, no guest. headlight_rig_test.cpp and local_shadow_rules_test.cpp check it.
//
//   ViewFrustum / MakeViewFrustum   the camera's side planes and its near plane from the game's world -> clip matrix (row vectors, the form of temporal_math.h:
//                                   clip = (p, 1) * M), in metres, with the eye.
//   SphereFactor                    how much of a sphere is in the frustum: 1 when its centre is inside, falling to 0 when it only touches from outside (as
//                                   volumetrics_lamps.h FrustumFactor, which takes the camera's tangents).
//   NdcExtent                       how far from the middle of the picture a point is, as a share of the half picture: 1 on the edge, more outside, infinity behind the camera.
//   BeamReach                       how much of a spot light's beam is in the view: the beam is five spheres along its axis (the cone's cross sections), each weighted by
//                                   the light's strength there (the shader's fall-off and window, headlight_rig_rules.h LampScale), the reach is the strongest of them in
//                                   the view: 1 for a lamp in the view, a little for a lamp whose far beam only grazes it, 0 for a lamp that lights nothing in the picture.
//   DistanceFade                    1 up to a near distance, 0 from a far one, smooth between: a light of a far car goes out gently, not at a swap of ranks.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace mcla::gpu::light_influence {

// A point nearer to the camera than this (view depth, clip w, metres) is behind the near plane: the camera's own near plane is about 0.1 to 0.5 m; the test is for "in front".
inline constexpr double kNearW = 0.1;

struct ViewFrustum {
  bool valid = false;
  double eye[3] = {};
  double clip[4][4] = {};     // world -> the game's clip, row vectors: clip = (p, 1) * clip
  double planes[5][4] = {};   // left, right, bottom, top, near: n . p + d >= 0 is inside, n a unit vector (the near plane is clip w >= kNearW)
};

// The frustum of a world -> clip matrix `m` (row vectors, D3D conventions: x and y in [-w, w], w the view depth) seen from `eye`. False (and `out->valid` false) when a number
// is not finite or a plane has no direction. The far plane and the depth range are left out: the lights that ask are reached by 70 m.
inline bool MakeViewFrustum(const double m[4][4], const double eye[3], ViewFrustum* out) {
  ViewFrustum f;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(m[r][c])) {
        *out = ViewFrustum{};
        return false;
      }
      f.clip[r][c] = m[r][c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(eye[c])) {
      *out = ViewFrustum{};
      return false;
    }
    f.eye[c] = eye[c];
  }
  // Column j of the matrix is the plane vector of clip_j: clip_j(p) = a p.x + b p.y + c p.z + d with (a, b, c, d) = (m[0][j], m[1][j], m[2][j], m[3][j]). The side planes are
  // w + x, w - x, w + y and w - y (>= 0); the near plane is w - kNearW.
  const int sides[4][2] = {{0, +1}, {0, -1}, {1, +1}, {1, -1}};  // the clip component and its sign
  for (int p = 0; p < 4; ++p) {
    for (int i = 0; i < 4; ++i) {
      f.planes[p][i] = m[i][3] + double(sides[p][1]) * m[i][sides[p][0]];
    }
  }
  for (int i = 0; i < 4; ++i) {
    f.planes[4][i] = m[i][3];
  }
  f.planes[4][3] -= kNearW;
  for (int p = 0; p < 5; ++p) {
    const double length = std::sqrt(f.planes[p][0] * f.planes[p][0] + f.planes[p][1] * f.planes[p][1] + f.planes[p][2] * f.planes[p][2]);
    if (!(length > 1e-9) || !std::isfinite(length)) {
      *out = ViewFrustum{};
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      f.planes[p][i] /= length;
    }
  }
  f.valid = true;
  *out = f;
  return true;
}

// The signed distance of a point to the nearest plane of the frustum, in metres: positive inside, negative outside (the worst plane's).
inline double InsideDistance(const ViewFrustum& f, const double p[3]) {
  double s = std::numeric_limits<double>::infinity();
  for (int i = 0; i < 5; ++i) {
    s = std::min(s, f.planes[i][0] * p[0] + f.planes[i][1] * p[1] + f.planes[i][2] * p[2] + f.planes[i][3]);
  }
  return s;
}

// How much of a sphere is in the frustum (0 .. 1): 1 when its centre is inside, then a straight fall to 0 when the sphere only touches the frustum from outside.
inline double SphereFactor(const ViewFrustum& f, const double centre[3], double radius) {
  const double s = InsideDistance(f, centre);
  if (!(s < 0.0)) {
    return 1.0;
  }
  return std::clamp(1.0 + s / std::max(radius, 1.0e-3), 0.0, 1.0);
}

// max(|x|, |y|) / w of a point: 1 on the edge of the picture, more outside, 0 in the middle; +infinity at or behind the camera's plane.
inline double NdcExtent(const ViewFrustum& f, const double p[3]) {
  double clip[4];
  for (int c = 0; c < 4; ++c) {
    clip[c] = p[0] * f.clip[0][c] + p[1] * f.clip[1][c] + p[2] * f.clip[2][c] + f.clip[3][c];
  }
  if (!(clip[3] > 1.0e-6)) {
    return std::numeric_limits<double>::infinity();
  }
  return std::max(std::fabs(clip[0]), std::fabs(clip[1])) / clip[3];
}

// ---------------------------------------------------------------------------
// a spot light's reach into the view
// ---------------------------------------------------------------------------

// The beam is a cone: the light's strength at a distance t along its axis is the shader's fall-off and window (headlight_rig_rules.h: 1 / (1 + (t / D)^2) and the window
// (1 - (t / range)^2)^2; headlight_rig_test.cpp checks that D is the rig's kFalloffDistance), and the cone's cross section there is a disc of radius t * tan(outer half angle).
inline constexpr double kBeamFalloffDistance = 26.0;
inline constexpr double kBeamMinRadius = 1.5;                 // m: the cross section at the lamp (the lamp's own surroundings, the car's nose)
inline constexpr double kBeamSamples[5] = {0.0, 0.1, 0.2, 0.35, 0.55};  // the places along the axis, as shares of the reach

inline double BeamStrengthAt(double t, double range, double falloff_distance = kBeamFalloffDistance) {
  if (!(t < range) || !(range > 0.0)) {
    return 0.0;
  }
  const double window = 1.0 - (t / range) * (t / range);
  return window * window / (1.0 + (t / falloff_distance) * (t / falloff_distance));
}

// How much of a spot light's beam is in the view (0 .. 1): the strongest, over the five places along the axis, of (the beam's strength there, 1 at the lamp) x (how much of the
// cross section's sphere is in the frustum). A lamp inside the view is 1 whatever way it points; a lamp outside it whose beam crosses the picture 14 m on is about 0.6; one whose
// beam only reaches the picture with its far end is 0.1 to 0.2; one that points away from the picture is 0. `axis` is a unit vector, `outer_radians` the cone's half angle.
inline double BeamReach(const ViewFrustum& f, const double pos[3], const double axis[3], double range, double outer_radians,
                        double falloff_distance = kBeamFalloffDistance) {
  if (!f.valid || !(range > 0.0)) {
    return 0.0;
  }
  const double tan_outer = std::tan(std::clamp(outer_radians, 0.01, 1.5));
  double best = 0.0;
  for (const double share : kBeamSamples) {
    const double t = share * range;
    const double weight = BeamStrengthAt(t, range, falloff_distance);
    if (!(weight > best)) {
      continue;  // a sample no stronger than what is found already cannot raise the answer
    }
    const double centre[3] = {pos[0] + axis[0] * t, pos[1] + axis[1] * t, pos[2] + axis[2] * t};
    const double radius = std::max(kBeamMinRadius, t * tan_outer);
    best = std::max(best, weight * SphereFactor(f, centre, radius));
  }
  return best;
}

// ---------------------------------------------------------------------------
// the importance a reach gives, and the distance fade
// ---------------------------------------------------------------------------

inline double SmoothStep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// A light that lights nothing in the picture is still worth this share of one that does (the rig's old "off the screen" weight): it is a car near the view, and a car that can come in.
inline constexpr double kReachFloor = 0.35;
// The reach at which a light counts in full.
inline constexpr double kReachFull = 0.45;

// The weight a reach gives a car's importance: kReachFloor with no reach, 1 from kReachFull, smooth between.
inline double ReachWeight(double reach) { return kReachFloor + (1.0 - kReachFloor) * SmoothStep(0.0, kReachFull, reach); }

inline constexpr double kFadeNearMetres = 90.0;   // a light is in full up to this far from the camera
inline constexpr double kFadeFarMetres = 140.0;   // ... and gone from this far

inline double DistanceFade(double distance, double near_m = kFadeNearMetres, double far_m = kFadeFarMetres) {
  return 1.0 - SmoothStep(near_m, far_m, distance);
}

}  // namespace mcla::gpu::light_influence
