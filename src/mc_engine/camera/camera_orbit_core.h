#pragma once
// The pure part of mouse look on the gameplay cameras (camBoomCS): no SDK, no guest memory, so larecomp_camera_orbit_test
// (camera_orbit_test.cpp) runs it without the game, on synthetic cases and on a recording of real frames (cam_orbit_record).
// The runtime half is camera_look.cpp (Hook_CameraBoomLook).
//
// ---------------------------------------------------------------------------------------------------------------------------------------
// WHY THE OLD LOOK TURNED ABOUT THE BONNET OR THE BOOT
//   The look used to turn the finished view matrix at sub_8220AA68. That matrix carries no pivot, so the point it orbited was guessed:
//   7.5 units along the view line from the camera (cam_freelook_orbit). The chase cameras do not sit 7.5 units from the car: each boom
//   (near, far, the speed pull-back) has its own distance, and the view line points at a look-at target ahead of and above the car, not at its
//   middle. So the guessed centre landed in front of the car (the bonnet) or behind it (the boot), and changed with the boom and the speed.
//   The bumper camera was worse: it is mounted on the car, and a rig number list decided whether it orbited, so it could swing 7.5 units out.
//
// WHAT RUNS NOW
//   camBoomCS::Update (sub_82320298) builds the camera position and its look-at target in the car's own frame, runs its own lookaround
//   (cs+452/456, the right stick) and then a rate limit (cs+656, at most tune+128 units/s per axis against last frame's cs+416/432). Right
//   after the limiter has stored both points (0x82321274) they are still in v127 and v62, and the game's pivot is one load away: the car's
//   middle (*(car+368)+32) for the chase booms, the camera itself when the boom is mounted (cs+668). The mouse turns both points there, so:
//     - the chase cameras orbit the car's own middle, at the boom's own distance, whatever it is this frame;
//     - the mounted cameras (bumper, cockpit) turn in place, like a head;
//     - the turn never reaches the limiter's history, so it follows the mouse at once instead of crawling there on a straight line
//       through the car (what the first version, turning before the limiter at 0x82320EA0, did);
//     - the view matrix (sub_82202EC0) and the camera collision (sub_8231FD60, cs+660) are still the game's, so the turned camera does
//       not pass through walls.
//
// THE ROTATIONS
//   Yaw turns about world Y, with the same sign as before (RotateY: x' = x cos + z sin, z' = -x sin + z cos), so the mouse turns the same way.
//   Pitch, orbiting: about the horizontal axis perpendicular to the arm pivot -> camera, so it never rolls the view; positive raises the camera
//   (the view looks further down at the car). In place: positive tips the view down. Both match the old look's signs.
//   The elevation limits only ever stop the mouse: a base camera that already sits outside them is never pushed, and yaw = pitch = 0 leaves
//   both points exactly as the game made them.
// ---------------------------------------------------------------------------------------------------------------------------------------

#include <algorithm>
#include <cmath>

namespace larecomp::camorbit {

struct V3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Scale(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline float HorizLen(const V3& a) { return std::sqrt(a.x * a.x + a.z * a.z); }
inline bool Finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// About world Y. The old look's convention (camera_look.cpp RotateY), so the mouse turns the view the same way it always did.
inline V3 RotY(const V3& v, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

// About a unit axis (Rodrigues).
inline V3 RotAxis(const V3& v, const V3& k, float a) {
  const float c = std::cos(a), s = std::sin(a);
  const float d = Dot(k, v);
  const V3 x{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
  return {v.x * c + x.x * s + k.x * d * (1.0f - c), v.y * c + x.y * s + k.y * d * (1.0f - c), v.z * c + x.z * s + k.z * d * (1.0f - c)};
}

// The horizontal axis about which a positive angle RAISES `arm` (d/dt of the y component is |arm horizontal| > 0). Zero when the arm is vertical.
inline bool RaiseAxis(const V3& arm, V3& axis) {
  const float h = HorizLen(arm);
  if (!(h > 1.0e-4f)) return false;
  axis = {-arm.z / h, 0.0f, arm.x / h};
  return true;
}

// Elevation of `v` above the horizontal, in radians.
inline float Elevation(const V3& v) { return std::atan2(v.y, HorizLen(v)); }

// How far `delta` may move an elevation e0 inside [lo, hi]: never past a limit, and never further out if e0 is already outside one.
inline float LimitedDelta(float e0, float delta, float lo, float hi) {
  if (hi < lo) hi = lo;
  if (delta > 0.0f) return std::max(0.0f, std::min(e0 + delta, hi) - e0);
  if (delta < 0.0f) return std::min(0.0f, std::max(e0 + delta, lo) - e0);
  return 0.0f;
}

struct Params {
  float min_elev = -0.08f;      // orbit: lowest elevation of the camera above the pivot (cam_freelook_min_elev)
  float max_elev = 1.3f;        // orbit: highest (cam_freelook_max_elev)
  float view_pitch_limit = 1.3f;  // in place: how far the view may tip up or down from level
  float inplace_dist = 3.0f;    // a boom whose camera is closer than this to the pivot, horizontally, is mounted on the car
  float max_pivot_dist = 60.0f;  // a pivot further than this from the camera is not the car; orbit the look-at target instead
};

struct In {
  V3 pos;           // camera position, as the boom built it this frame
  V3 look;          // the look-at target
  V3 pivot;         // the game's pivot: the car's position (*(car+368)+32)
  bool mounted_flag = false;  // cs+668: the game's own "this boom is mounted" (its pivot is then the camera itself)
};

enum class Mode : int { Orbit = 0, InPlace = 1, OrbitLookAt = 2, Skipped = 3 };

struct Out {
  V3 pos;
  V3 look;
  V3 pivot_used;
  Mode mode = Mode::Skipped;
  float pitch_used = 0.0f;
};

inline Mode Classify(const In& in, const Params& p) {
  if (!Finite(in.pos) || !Finite(in.look)) return Mode::Skipped;
  if (in.mounted_flag) return Mode::InPlace;
  if (!Finite(in.pivot) || Len(Sub(in.pos, in.pivot)) > p.max_pivot_dist) return Mode::OrbitLookAt;
  if (HorizLen(Sub(in.pos, in.pivot)) < p.inplace_dist) return Mode::InPlace;
  return Mode::Orbit;
}

// Turns the camera and its look-at target about `pivot`: yaw about world Y, then pitch about the horizontal axis across the arm.
inline void OrbitAbout(const V3& pivot, V3& pos, V3& look, float yaw, float pitch, const Params& p, float& pitch_used) {
  V3 rel = Sub(pos, pivot), lrel = Sub(look, pivot);
  if (yaw != 0.0f) {
    rel = RotY(rel, yaw);
    lrel = RotY(lrel, yaw);
  }
  pitch_used = 0.0f;
  V3 axis;
  if (pitch != 0.0f && RaiseAxis(rel, axis)) {
    pitch_used = LimitedDelta(Elevation(rel), pitch, p.min_elev, p.max_elev);
    if (pitch_used != 0.0f) {
      rel = RotAxis(rel, axis, pitch_used);
      lrel = RotAxis(lrel, axis, pitch_used);
    }
  }
  pos = Add(pivot, rel);
  look = Add(pivot, lrel);
}

// Turns the view about the camera itself: the position stands, the look-at target swings round it. Positive pitch tips the view down.
inline void TurnInPlace(const V3& pos, V3& look, float yaw, float pitch, const Params& p, float& pitch_used) {
  V3 v = Sub(look, pos);
  if (yaw != 0.0f) v = RotY(v, yaw);
  pitch_used = 0.0f;
  V3 axis;
  if (pitch != 0.0f && RaiseAxis(v, axis)) {
    const float lim = std::fabs(p.view_pitch_limit);
    pitch_used = -LimitedDelta(Elevation(v), -pitch, -lim, lim);
    if (pitch_used != 0.0f) v = RotAxis(v, axis, -pitch_used);
  }
  look = Add(pos, v);
}

inline Out Apply(const In& in, float yaw, float pitch, const Params& p) {
  Out o;
  o.pos = in.pos;
  o.look = in.look;
  o.mode = Classify(in, p);
  switch (o.mode) {
    case Mode::Skipped:
      break;
    case Mode::InPlace:
      o.pivot_used = in.pos;
      TurnInPlace(in.pos, o.look, yaw, pitch, p, o.pitch_used);
      break;
    case Mode::Orbit:
      o.pivot_used = in.pivot;
      OrbitAbout(in.pivot, o.pos, o.look, yaw, pitch, p, o.pitch_used);
      break;
    case Mode::OrbitLookAt:
      o.pivot_used = in.look;
      OrbitAbout(in.look, o.pos, o.look, yaw, pitch, p, o.pitch_used);
      break;
  }
  return o;
}

// The point the OLD look orbited, rebuilt from the same frame: `orbit` units from the camera along its view line (cam_freelook_orbit, 7.5).
// Only for the replay's comparison: how far from the car's middle the old "zero" sat on real frames.
inline V3 OldOrbitCentre(const V3& pos, const V3& look, float orbit) {
  const V3 v = Sub(look, pos);
  const float l = Len(v);
  if (!(l > 1.0e-4f)) return pos;
  return Add(pos, Scale(v, std::fabs(orbit) / l));
}

inline const char* ModeName(Mode m) {
  switch (m) {
    case Mode::Orbit: return "orbit";
    case Mode::InPlace: return "in-place";
    case Mode::OrbitLookAt: return "orbit-lookat";
    case Mode::Skipped: return "skipped";
  }
  return "?";
}

}  // namespace larecomp::camorbit
