#pragma once
// ===========================================================================
// The road reflection's mirror plane on the title screen: the rules
// ===========================================================================
// title_road_plane.cpp holds the hooks and the game calls; what they decide is here, as pure functions on plain numbers, so a test needs no game
// (title_road_plane_test.cpp).
//
// THE BUG (owner, 2026-11-25; the same on the stock Xenia renderer, so it is the game's own): in rain the title screen's puddles are large flat white
// blocks. The wet road's pixel shaders (xCityRoad, xCityTunnelRoad, the sidewalks, the windows, the water) mix their lit colour with ONE texture, the
// road reflection view: the game draws the city a second time from the main camera with every vertex mirrored about a plane by the vertex shader
// (VS c43.z, gReflectionParams.z) and stretches the 256 x 256 result over the screen. The plane is the street's height, which sub_822F3918 puts there
// every frame from the PLAYER's ground height (the matrix of player slot 0, +52). The title screen has no player: sub_822A39C0 says no view, and the
// function stores its default, the constant 1.0 (the float at 0x82001D14), which is a plane at sea level. The front-end's cameras look at the
// city from 40 to 150 metres up, so the roads in the picture lie tens of metres above a plane at y = 1, the mirrored city lands far below the
// picture, and the reflection the puddles mix in is the picture of nothing: in rain, where VS c43.y is 1.19 and the puddle mask is the whole
// road, that reads as flat blocks. (The pause menu, the map and the garage all have a player, so their plane is right: only the title does not.
// A dry night hides it: c43.y is 0.095 there and the mix is a tenth of the reflection.)
//
// THE FIX: when the game has no player view, the plane is the height of the ground the camera is looking at. The camera's final pose is published
// by the front-end camera hook (mc_engine/camera/final_camera.h); a few segments through the lower part of the picture are probed against the
// game's physics level (the segment probe the teleport snap and the Director's query.pick run: sub_82574418), the upward-facing hits are the
// ground, and the lower quartile of their heights is the plane (a roof or a car is above the road, never below it). The plane follows the shot
// smoothly, and snaps to a cut. When nothing is known the game's own value stays.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace larecomp::title_plane {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

// The final camera the renderer gets (the slots of sub_822C0320): the rows are right, up, BACK (the forward direction is minus the third row:
// mc_engine/cinematic/camera.cpp FromSlotRows), the position is the fourth row, Y is up.
struct Camera {
  Vec3 pos;
  Vec3 right, up, back;
  float fov_degrees = 50.0f;  // the vertical field of view
  bool valid = false;
};

struct Ray {
  Vec3 origin;
  Vec3 dir;  // unit
};

struct Hit {
  Vec3 pos;
  Vec3 normal;
};

inline constexpr float kAspect = 16.0f / 9.0f;       // only decides how wide the probes fan out: the rays need to land on the road, not on a pixel
inline constexpr float kRayLengthMetres = 600.0f;    // a segment's length: the front-end's cameras are 40 to 150 m up and look 50 to 500 m out
inline constexpr float kMinNormalY = 0.75f;          // a hit is ground when its normal points up this much (a wall, a pole, a car's side is not)
inline constexpr size_t kMinHits = 3;                // fewer ground hits than this say nothing about the road
inline constexpr float kCutMetres = 12.0f;           // the plane moved more than this between two probes: the camera cut, the plane snaps
inline constexpr float kTauSeconds = 0.35f;          // the plane's follow time
inline constexpr float kRefreshSeconds = 0.1f;       // the probes run this often (the plane is held between)
inline constexpr float kStaleSeconds = 0.5f;         // a camera pose older than this is not the picture's
inline constexpr uint32_t kWarmupCalls = 90;         // the plane function's calls before the first probe: the world streams in at the start of the front end

// The picture's sample points (0..1 across, 0..1 down from the top): the road is in the lower part of the front-end's pictures, so the rows
// start under the middle and the lowest row is the nearest ground.
inline constexpr std::array<float, 5> kColumns = {0.15f, 0.32f, 0.5f, 0.68f, 0.85f};
inline constexpr std::array<float, 4> kRows = {0.55f, 0.68f, 0.82f, 0.94f};
inline constexpr size_t kRayCount = kColumns.size() * kRows.size();

inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// A camera that can be used: finite numbers, rows that are not zero, a field of view that makes a picture.
inline bool Usable(const Camera& c) {
  if (!c.valid || !Finite(c.pos) || !Finite(c.right) || !Finite(c.up) || !Finite(c.back)) return false;
  if (!(Length(c.back) > 0.5f) || !(Length(c.right) > 0.5f) || !(Length(c.up) > 0.5f)) return false;
  return std::isfinite(c.fov_degrees) && c.fov_degrees > 1.0f && c.fov_degrees < 170.0f;
}

// The ray through the picture's point (u, v): forward + right * x + up * y. Not a camera of this world's (a zero row) gives a ray of length 0.
inline Ray RayThrough(const Camera& c, float u, float v) {
  const float tan_v = std::tan(c.fov_degrees * 0.5f * 3.14159265358979f / 180.0f);
  const float tan_h = tan_v * kAspect;
  const Vec3 forward = Scale(c.back, -1.0f);
  Vec3 d = Add(forward, Add(Scale(c.right, (2.0f * u - 1.0f) * tan_h), Scale(c.up, (1.0f - 2.0f * v) * tan_v)));
  const float len = Length(d);
  Ray r;
  r.origin = c.pos;
  r.dir = len > 1e-6f ? Scale(d, 1.0f / len) : Vec3{};
  return r;
}

// The i-th probe of the grid (row-major, kRows x kColumns).
inline Ray ProbeRay(const Camera& c, size_t i) {
  return RayThrough(c, kColumns[i % kColumns.size()], kRows[(i / kColumns.size()) % kRows.size()]);
}

// A hit that may be the ground: finite, facing up, and not above the camera (a road under the picture's eye, whatever its height).
inline bool IsGround(const Hit& h, float camera_y) {
  if (!Finite(h.pos) || !Finite(h.normal)) return false;
  return h.normal.y >= kMinNormalY && h.pos.y <= camera_y;
}

// The plane the hits say: the lower quartile of the ground hits' heights (the roofs and the cars above the road are the tail the quartile drops;
// a hit under the road, a bridge's underside seen from below, faces down and is not ground). False when fewer than kMinHits are ground.
inline bool PlaneFromHits(const std::vector<Hit>& hits, float camera_y, float* plane) {
  std::vector<float> ys;
  ys.reserve(hits.size());
  for (const Hit& h : hits) {
    if (IsGround(h, camera_y)) ys.push_back(h.pos.y);
  }
  if (ys.size() < kMinHits) return false;
  std::sort(ys.begin(), ys.end());
  *plane = ys[(ys.size() - 1) / 4];
  return std::isfinite(*plane);
}

// The plane that follows the probes: a first value and a cut (the new plane is kCutMetres or more away) are taken as they are; anything else
// approaches the probe's with the follow time (seconds, not frames: the same at 30 and 240 FPS).
inline float Follow(bool have_previous, float previous, float target, float dt_seconds) {
  if (!have_previous || !std::isfinite(previous) || std::fabs(target - previous) >= kCutMetres) return target;
  const float dt = std::isfinite(dt_seconds) ? std::clamp(dt_seconds, 0.0f, 0.25f) : 0.0f;
  return previous + (target - previous) * (1.0f - std::exp(-dt / kTauSeconds));
}

// Whether the camera's pose may be used at `age_seconds` old.
inline bool Fresh(double age_seconds) { return age_seconds >= 0.0 && age_seconds <= double(kStaleSeconds); }

// What the plane function stores for the plane (VS c43.z) when the game has no player: the plane found, else the game's own value.
inline float Chosen(bool known, float plane, float game_default) { return known && std::isfinite(plane) ? plane : game_default; }

}  // namespace larecomp::title_plane
