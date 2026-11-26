#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car-only sun shadow map: the rules that need no renderer
// ===========================================================================
// The renderer's half is car_shadow_map.h. This header holds what needs no Plume, no ReXGlue and no game, as pure functions on plain
// numbers, so that car_shadow_map_rules_test.cpp builds on its own:
//
//   - the setting: mcla_gpu_car_shadow_map's words and the map's size per GPU class (Resolve), and when a frame draws a map at all
//     (Decide);
//   - the car's pose out of its root matrix (MakePose: an orthonormal frame, the one the map is attached to);
//   - the fit: an orthographic view along the sun, square, centred on the car and turned with it (MakeFit), and the held centre and
//     radius that keep that view the same from frame to frame (Estimate, Hold);
//   - the matrices: world -> (u, v, depth) of the map, the same from the AO's VIEW space (ViewToMap: the rows the contact pass reads),
//     and a draw's model space -> the map's clip space (ModelToClip: the rows the depth-only vertex shader multiplies by);
//   - the vertex-position variants a pipeline is made for (VariantKey) and the debug view's rectangle (InsetFor).
//
// WHY THE MAP IS ATTACHED TO THE CAR, AND HAS NO WORLD TEXEL SNAP. A cascaded shadow map is snapped to whole texels of the WORLD so a
// static scene keeps its raster while the camera moves. The car is not static: it is the one thing in this map, and it moves with
// the map's centre, so a world snap would slide the texel grid across the car as it drives and make its own self-shadows crawl.
// The centre here is a fixed point of the CAR (a held offset from its root along its own axes), and the two axes of the map are
// the car's own horizontal axis projected on the plane at right angles to the sun, so with the sun fixed in the car's frame (a
// straight drive) every triangle lands on the same texel positions in every frame, bit for bit. A turn rotates the sun in the car's
// frame, which moves the shadow smoothly, as it should. The size of the map (its half extent) is a held number too, in steps of
// kRadiusQuantum, so the texel size never creeps.
//
// DEPTH. The stored depth is 0 at the map's near plane (nearest the sun) and 1 at its far plane, 1 also where no part of the car is
// (the clear value). The planes are a fixed distance either side of the centre (kDepthMargin beyond the held radius), so a point of
// the road behind the car has a depth above 1 and a point between the car and the sun one below 0: a receiver beyond the far plane is
// BEHIND the car, whatever the texel holds. A reader must treat a stored 1 as "no part of the car here" before it compares
// (stored < depth - bias) -- else every point with a depth above 1 would be shadowed in an empty texel.
//
// Matrices are row vectors (v' = v M) as in temporal_math.h. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "temporal_math.h"

namespace mcla::gpu::car_shadow_map {

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

// mcla_gpu_car_shadow_map: auto, off, high or ultra. No level is called low (owner, 2026-11-14): two sizes are high and ultra.
enum class Level : uint8_t { kOff, kHigh, kUltra };

inline constexpr uint32_t kSizeHigh = 1024;
inline constexpr uint32_t kSizeUltra = 2048;

constexpr uint32_t SizeOf(Level level) {
  return level == Level::kUltra ? kSizeUltra : level == Level::kHigh ? kSizeHigh : 0u;
}

constexpr std::string_view LevelName(Level level) {
  return level == Level::kUltra ? "ultra" : level == Level::kHigh ? "high" : "off";
}

struct Choice {
  Level level = Level::kOff;
  bool automatic = false;  // `auto`: the class decided (also a text the setting does not know)
  bool unknown = false;    // the text is none of the setting's words (it then runs as `auto`)
  uint32_t size() const { return SizeOf(level); }
};

inline std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

// `auto` is the big map (2048) on a discrete graphics card and the small one (1024) on an integrated one (the Low class: shared
// memory, gpu_class.h). The contact shadows' own `auto` is off on that class, so there it only runs when the player turned the
// contact shadows on. A word that is no value (the low of an older larecomp.toml) counts as `auto`.
inline Level AutoLevel(gpu_class::GpuClass) { return Level::kUltra; }

inline Choice Resolve(gpu_class::GpuClass cls, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  if (text == "off") {
    choice.level = Level::kOff;
  } else if (text == "high") {
    choice.level = Level::kHigh;
  } else if (text == "ultra") {
    choice.level = Level::kUltra;
  } else {
    choice.automatic = true;
    choice.unknown = text != "auto";
    choice.level = AutoLevel(cls);
  }
  return choice;
}

// "car shadow map ultra, 2048x2048 (auto on the high class)".
inline std::string Describe(const Choice& choice, gpu_class::GpuClass cls) {
  std::string text = "car shadow map ";
  text += LevelName(choice.level);
  if (choice.level != Level::kOff) {
    text += ", " + std::to_string(choice.size()) + "x" + std::to_string(choice.size());
  }
  text += " (";
  if (choice.unknown) {
    text += "not a value of the setting: taken as auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else if (choice.automatic) {
    text += "auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else {
    text += "set";
  }
  text += ')';
  return text;
}

// ---------------------------------------------------------------------------
// when a frame draws a map
// ---------------------------------------------------------------------------

// The contact shadows are gated off below this sun height (ao.cpp kGateOffDegrees): a map nobody reads is not drawn. CHECK: keep in step.
inline constexpr double kMinSunDegrees = 3.0;

enum class Why : uint8_t {
  kRun,
  kSettingOff,    // mcla_gpu_car_shadow_map is off
  kContactOff,    // mcla_gpu_contact_shadows is off: nobody reads the map
  kNotPlayView,   // the pause menu's, the garage's and the map's worlds
  kNoCar,         // the player's car was not found in the frame (or has no usable draw)
  kNoSun,         // no direction to the sun
  kSunLow,        // the sun is below kMinSunDegrees: the contact shadows are gated
  kContactGated,  // the AO gated the contact shadows in the frame before (the sun's height or the wet road)
};

inline double SunSin(const double to_sun[3]) {
  const double length = std::sqrt(to_sun[0] * to_sun[0] + to_sun[1] * to_sun[1] + to_sun[2] * to_sun[2]);
  return length > 0.5 && std::isfinite(length) ? to_sun[1] / length : -2.0;  // world y is up
}

inline Why Decide(Level map, bool contact_on, bool play_view, bool car_found, const double to_sun[3]) {
  if (map == Level::kOff) {
    return Why::kSettingOff;
  }
  if (!contact_on) {
    return Why::kContactOff;
  }
  if (!play_view) {
    return Why::kNotPlayView;
  }
  if (!car_found) {
    return Why::kNoCar;
  }
  const double sin_elevation = SunSin(to_sun);
  if (sin_elevation < -1.5) {
    return Why::kNoSun;
  }
  constexpr double kPi = 3.14159265358979323846;
  return sin_elevation < std::sin(kMinSunDegrees * kPi / 180.0) ? Why::kSunLow : Why::kRun;
}

constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kRun: return "drawn";
    case Why::kSettingOff: return "the setting is off";
    case Why::kContactOff: return "the contact shadows are off";
    case Why::kNotPlayView: return "not the game view";
    case Why::kNoCar: return "no player car in the frame";
    case Why::kNoSun: return "no direction to the sun";
    case Why::kSunLow: return "the sun is too low (the contact shadows are gated)";
    case Why::kContactGated: return "the contact shadows are gated (a low sun or a wet road)";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// who casts the player's car's shadow on the world: the map, or the game's cascades
// ---------------------------------------------------------------------------
// The game draws the player's car into its four sun cascades at a forced low level of detail (Vehicle::Draw, the byte +594:
// traffic_shadow.h). Its shadow is then a blob that is bigger and rounder than the car, filtered by the game's four taps, and it
// lies under the car's own crisp shadow from this map: the edge of the blob pokes out round the crisp one with the game's grain
// (the owner, 2026-11-19: "we're also drawing the game's dither-prone shadow as well"). Where the map covers the car's shadow
// the cascades must not cast it. The map covers all of it: the map is a view along the sun, the car is inside the square, so every
// ground point in its shadow has its place in the square whatever the sun's height, and a point beyond the far plane under a
// texel that holds the car is shadowed (car_shadow_map.h). What the map shades is the sun's share through the contact shadows' apply
// (ao_apply_ps.hlsl: 1 - sun_share * (1 - cs) is the sun's term taken out where the game's own mask says lit), which is the same
// number the game's mask would have taken out, but only at the contact shadows' full strength: the strength is the setting times
// the sun's elevation gate (full from 12 degrees: ao.cpp kGateFullDegrees) times the wetness gate. Below it (a low sun, rain, a
// setting under 1) the cascades keep the car, as the game draws it, and the map's shadow goes over it.
//
// The guest's draw runs ahead of the renderer by a few frames, so the decision is the renderer's last word, published in an atomic
// the hook reads: it is taken when the map has been there for kOwnAfterFrames frames in a row with the contact shadows at full
// strength, and given up the moment a frame has no map (the player's car left the picture, a pipeline is being made, the
// setting went off) or the strength falls under kOwnKeepStrength (the hysteresis keeps a sun at 12 degrees from flipping it).
inline constexpr uint32_t kOwnAfterFrames = 3;
inline constexpr float kOwnTakeStrength = 0.995f;
inline constexpr float kOwnKeepStrength = 0.9f;

struct Owner {
  uint32_t valid_run = 0;  // frames in a row that drew a map with the strength at kOwnTakeStrength or more
  bool owns = false;
};

// The next state after a frame. `map_drawn`: the frame drew a map (CarShadowMap::View().valid); `strength`: the contact shadows'
// strength this frame (setting x sun gate x wetness gate, 0 when they are off or gated); `allowed`: the debug switch
// mcla_gpu_car_shadow_map_owns.
constexpr Owner NextOwner(Owner before, bool map_drawn, float strength, bool allowed) {
  Owner after;
  if (!allowed || !map_drawn) {
    return after;
  }
  after.valid_run = strength >= kOwnTakeStrength ? before.valid_run + 1 : 0u;
  after.owns = before.owns ? strength >= kOwnKeepStrength : after.valid_run >= kOwnAfterFrames;
  if (before.owns && strength >= kOwnKeepStrength && strength < kOwnTakeStrength) {
    after.valid_run = before.valid_run;  // inside the hysteresis band: held, not counted
  }
  return after;
}

// The cell the renderer writes and the guest's hook (hooks/traffic_shadow.cpp Patch_TrafficCascadeGate) reads, on other threads.
inline std::atomic<bool>& OwnerCell() {
  static std::atomic<bool> cell{false};
  return cell;
}
inline bool PlayerShadowIsTheMaps() { return OwnerCell().load(std::memory_order_relaxed); }
inline void PublishOwner(bool owns) { OwnerCell().store(owns, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// the car's frame
// ---------------------------------------------------------------------------

namespace detail {

inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double Length(const double a[3]) { return std::sqrt(Dot(a, a)); }
inline void Cross(const double a[3], const double b[3], double out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool Normalize(double a[3]) {
  const double length = Length(a);
  if (!(length > 1e-9) || !std::isfinite(length)) {
    return false;
  }
  a[0] /= length;
  a[1] /= length;
  a[2] /= length;
  return true;
}

}  // namespace detail

// The player's root matrix as an orthonormal frame: the position and three unit axes (the rows of the matrix, made orthogonal by
// Gram-Schmidt in row order: a car whose root matrix carries a little scale or shear still gives an exact frame).
struct Pose {
  double position[3] = {};
  double axes[3][3] = {};
};

inline bool MakePose(const temporal::Mat4& world, Pose* pose) {
  if (!temporal::Finite(world)) {
    return false;
  }
  double rows[3][3];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      rows[r][c] = world.m[r][c];
    }
    pose->position[r] = world.m[3][r];
  }
  for (int r = 0; r < 3; ++r) {
    double v[3] = {rows[r][0], rows[r][1], rows[r][2]};
    for (int k = 0; k < r; ++k) {
      const double d = detail::Dot(v, pose->axes[k]);
      for (int c = 0; c < 3; ++c) {
        v[c] -= d * pose->axes[k][c];
      }
    }
    if (!detail::Normalize(v)) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      pose->axes[r][c] = v[c];
    }
  }
  return true;
}

// Which axis of the car points up in the world (world y), and the horizontal axis the map is attached to: the lower numbered of the
// other two. The car's own model axes are not known here (which row is "forward" is the game's), and need not be: any horizontal axis
// of the body turns with the car.
inline int UpIndex(const Pose& pose) {
  int best = 0;
  for (int k = 1; k < 3; ++k) {
    if (std::fabs(pose.axes[k][1]) > std::fabs(pose.axes[best][1])) {
      best = k;
    }
  }
  return best;
}

inline int SideIndex(int up) { return up == 0 ? 1 : 0; }

// ---------------------------------------------------------------------------
// the car's size and centre: the estimate, and what is held
// ---------------------------------------------------------------------------

// The most a car's bounds may be and still be believed (the half of the box's diagonal, metres): a mesh with stray triangles parked
// hundreds of metres away (the census saw draws 888 m from the camera) must not blow the map up. A car's own half diagonal is about
// 2.6 m (an R34), a long one 3.2.
inline constexpr double kMinRadius = 1.0;
inline constexpr double kMaxRadius = 7.0;
// The sphere of the fallback, centred on the root: a root at the middle of the wheel base on the ground still covers a long car.
inline constexpr double kFallbackRadius = 3.4;
// How far the bounds' centre may wander from the held centre before the held one follows (the box of a car that turns changes with
// the turn: the world box of the meshes' boxes), and the jump that says it is another car (or a teleport) and starts over.
inline constexpr double kCentreDeadBand = 0.35;
inline constexpr double kResetJump = 1.2;
// The held radius is a multiple of this, so the map's scale changes in steps, never creeps.
inline constexpr double kRadiusQuantum = 0.25;
// The map reaches this far beyond the held radius sideways and in depth.
inline constexpr double kEdgeMargin = 0.25;
inline constexpr double kDepthMargin = 0.5;

inline double QuantizeUp(double value, double quantum) {
  return std::ceil(value / quantum - 1e-9) * quantum;
}

// The centre as metres along the car's own unit axes from its root, and the radius of a sphere that holds the car.
struct Estimate {
  double offset[3] = {};
  double radius = kFallbackRadius;
  bool from_bounds = false;  // the frame's box was believed (else the fallback sphere on the root)
};

inline Estimate EstimateOf(const Pose& pose, bool bounds_valid, const double bounds_min[3], const double bounds_max[3]) {
  Estimate est;
  if (!bounds_valid) {
    return est;
  }
  double centre[3];
  double diagonal[3];
  for (int i = 0; i < 3; ++i) {
    if (!(std::isfinite(bounds_min[i]) && std::isfinite(bounds_max[i]) && bounds_max[i] >= bounds_min[i])) {
      return est;
    }
    centre[i] = 0.5 * (bounds_min[i] + bounds_max[i]);
    diagonal[i] = bounds_max[i] - bounds_min[i];
  }
  const double radius = 0.5 * detail::Length(diagonal);
  double from_root[3] = {centre[0] - pose.position[0], centre[1] - pose.position[1], centre[2] - pose.position[2]};
  // The root of the car is inside its own box, or near it: a box far from the root belongs to something else.
  if (!(radius >= kMinRadius && radius <= kMaxRadius) || detail::Length(from_root) > radius) {
    return est;
  }
  for (int k = 0; k < 3; ++k) {
    est.offset[k] = detail::Dot(from_root, pose.axes[k]);
  }
  est.radius = radius;
  est.from_bounds = true;
  return est;
}

// What the map is fitted to, kept from frame to frame.
struct Held {
  bool valid = false;
  double offset[3] = {};  // the centre, metres along the car's unit axes from its root
  double radius = 0.0;    // a sphere around that centre that holds the car, a multiple of kRadiusQuantum
};

// The held values after one more frame's estimate. The centre only moves when the estimate is further than the dead band from it
// (the jitter of the box of a turning car must not move the map), the radius only grows (it takes the largest the box showed, plus
// the dead band the centre may be off by); a jump of the centre or a much smaller radius says another car and starts over. A frame
// with no believed box (the fallback sphere) changes nothing once something is held.
inline Held Hold(const Held& held, const Estimate& est) {
  const double needed = QuantizeUp(est.radius + (est.from_bounds ? kCentreDeadBand : 0.0), kRadiusQuantum);
  Held out = held;
  if (!held.valid) {
    out.valid = true;
    for (int k = 0; k < 3; ++k) {
      out.offset[k] = est.offset[k];
    }
    out.radius = needed;
    return out;
  }
  if (!est.from_bounds) {
    return out;
  }
  double delta[3];
  for (int k = 0; k < 3; ++k) {
    delta[k] = est.offset[k] - held.offset[k];
  }
  const double moved = detail::Length(delta);
  if (moved > kResetJump || est.radius * 2.0 < held.radius) {
    out.valid = true;
    for (int k = 0; k < 3; ++k) {
      out.offset[k] = est.offset[k];
    }
    out.radius = needed;
    return out;
  }
  if (moved > kCentreDeadBand) {
    for (int k = 0; k < 3; ++k) {
      out.offset[k] = est.offset[k];
    }
  }
  out.radius = std::max(held.radius, needed);
  return out;
}

// ---------------------------------------------------------------------------
// the fit
// ---------------------------------------------------------------------------

struct Fit {
  bool valid = false;
  double centre[3] = {};   // world
  double right[3] = {};    // the map's u axis: the car's horizontal axis projected on the plane at right angles to the sun
  double up[3] = {};       // the map's other axis, right x up = sun; v of the map grows against it (v is down, as an image)
  double sun[3] = {};      // unit, towards the sun
  double half_extent = 0;  // metres: the map covers [-half_extent, half_extent] along right and up
  double half_depth = 0;   // metres: and [-half_depth, half_depth] along the sun
  double texel_m = 0;      // a texel's side in metres
  double depth_m = 0;      // the depth range in metres
  // World -> the map: u = dot((p, 1), to_map[0]), v = dot((p, 1), to_map[1]), depth = dot((p, 1), to_map[2]); u, v in [0, 1] across
  // the map (v down), depth 0 at the near plane (towards the sun), 1 at the far plane.
  double to_map[3][4] = {};
};

inline void SetProjection(Fit& f) {
  const double cr = detail::Dot(f.centre, f.right);
  const double cu = detail::Dot(f.centre, f.up);
  const double cs = detail::Dot(f.centre, f.sun);
  for (int i = 0; i < 3; ++i) {
    f.to_map[0][i] = 0.5 / f.half_extent * f.right[i];
    f.to_map[1][i] = -0.5 / f.half_extent * f.up[i];
    f.to_map[2][i] = -0.5 / f.half_depth * f.sun[i];
  }
  f.to_map[0][3] = 0.5 - 0.5 * cr / f.half_extent;
  f.to_map[1][3] = 0.5 + 0.5 * cu / f.half_extent;
  f.to_map[2][3] = 0.5 + 0.5 * cs / f.half_depth;
}

// The sun's direction (any length over 0.5) and the car's frame, the held centre and radius, and the map's size in texels.
inline bool MakeFit(const double to_sun[3], const Pose& pose, const Held& held, uint32_t size, Fit* fit) {
  *fit = Fit{};
  if (!held.valid || size < 16 || !(held.radius > 0.0)) {
    return false;
  }
  double sun[3] = {to_sun[0], to_sun[1], to_sun[2]};
  if (!(detail::Length(sun) > 0.5) || !detail::Normalize(sun)) {
    return false;
  }
  // The map's right axis: the first of these that is not along the sun, projected on the plane at right angles to it.
  const int up_index = UpIndex(pose);
  const int side = SideIndex(up_index);
  const int other = 3 - up_index - side;
  const double world_x[3] = {1.0, 0.0, 0.0};
  const double world_z[3] = {0.0, 0.0, 1.0};
  const double* candidates[4] = {pose.axes[side], pose.axes[other], world_x, world_z};
  double right[3] = {};
  bool found = false;
  for (const double* candidate : candidates) {
    const double along = detail::Dot(candidate, sun);
    for (int i = 0; i < 3; ++i) {
      right[i] = candidate[i] - along * sun[i];
    }
    if (detail::Length(right) > 0.02) {
      found = detail::Normalize(right);
      break;
    }
  }
  if (!found) {
    return false;
  }
  double up[3];
  detail::Cross(sun, right, up);  // right x up = sun: a right handed frame
  if (!detail::Normalize(up)) {
    return false;
  }
  Fit f;
  for (int i = 0; i < 3; ++i) {
    f.centre[i] = pose.position[i] + held.offset[0] * pose.axes[0][i] + held.offset[1] * pose.axes[1][i] +
                  held.offset[2] * pose.axes[2][i];
    f.right[i] = right[i];
    f.up[i] = up[i];
    f.sun[i] = sun[i];
  }
  f.half_extent = held.radius + kEdgeMargin;
  f.half_depth = held.radius + kDepthMargin;
  f.texel_m = 2.0 * f.half_extent / double(size);
  f.depth_m = 2.0 * f.half_depth;
  SetProjection(f);
  f.valid = true;
  *fit = f;
  return true;
}

inline constexpr uint64_t kMapRefreshFrames = 4;
inline constexpr double kReuseAngleCosine = 0.9999996192282494;  // cos(0.05 degrees)

// Store the raster's axes and centre in car space. Reattach them to the current root before sampling the held map.
inline Fit RelativeFit(const Fit& fit, const Pose& pose) {
  Fit relative = fit;
  const double centre[3] = {fit.centre[0] - pose.position[0], fit.centre[1] - pose.position[1], fit.centre[2] - pose.position[2]};
  for (int i = 0; i < 3; ++i) {
    relative.centre[i] = detail::Dot(centre, pose.axes[i]);
    relative.right[i] = detail::Dot(fit.right, pose.axes[i]);
    relative.up[i] = detail::Dot(fit.up, pose.axes[i]);
    relative.sun[i] = detail::Dot(fit.sun, pose.axes[i]);
  }
  return relative;
}

inline Fit AttachFit(const Fit& relative, const Pose& pose) {
  Fit fit = relative;
  const auto world = [&](const double local[3], int i) {
    return local[0] * pose.axes[0][i] + local[1] * pose.axes[1][i] + local[2] * pose.axes[2][i];
  };
  for (int i = 0; i < 3; ++i) {
    fit.centre[i] = pose.position[i] + world(relative.centre, i);
    fit.right[i] = world(relative.right, i);
    fit.up[i] = world(relative.up, i);
    fit.sun[i] = world(relative.sun, i);
  }
  SetProjection(fit);
  return fit;
}

inline bool ReuseFit(const Fit& saved, const Fit& now, uint64_t drawn, uint64_t frame) {
  const double offset[3] = {now.centre[0] - saved.centre[0], now.centre[1] - saved.centre[1], now.centre[2] - saved.centre[2]};
  return saved.valid && now.valid && frame > drawn && frame - drawn < kMapRefreshFrames && saved.texel_m == now.texel_m &&
         saved.half_depth == now.half_depth && detail::Length(offset) < now.texel_m * 0.25 &&
         detail::Dot(saved.sun, now.sun) >= kReuseAngleCosine && detail::Dot(saved.right, now.right) >= kReuseAngleCosine;
}

struct PartKey {
  uint64_t mesh = 0;
  uint32_t variant = 0;
  uint32_t position_offset = 0;
  float clip[16] = {};
};

// Compare the caster in the reattached raster's coordinates. Bound the change anywhere in its model-space box.
inline bool SamePart(const PartKey& saved, const PartKey& now, const double extent[3], const Fit& fit) {
  if (saved.mesh == 0 || saved.mesh != now.mesh || saved.variant != now.variant || saved.position_offset != now.position_offset) {
    return false;
  }
  const double tolerance[3] = {0.25 * fit.texel_m / fit.half_extent, 0.25 * fit.texel_m / fit.half_extent, 0.25 * fit.texel_m / fit.depth_m};
  for (int c = 0; c < 3; ++c) {
    double delta = std::fabs(double(now.clip[12 + c]) - saved.clip[12 + c]);
    for (int r = 0; r < 3; ++r) {
      delta += extent[r] * std::fabs(double(now.clip[r * 4 + c]) - saved.clip[r * 4 + c]);
    }
    if (!(delta <= tolerance[c])) {
      return false;
    }
  }
  return true;
}

// A world point in the map.
inline void MapOfWorld(const Fit& fit, const double p[3], double* u, double* v, double* depth) {
  *u = fit.to_map[0][0] * p[0] + fit.to_map[0][1] * p[1] + fit.to_map[0][2] * p[2] + fit.to_map[0][3];
  *v = fit.to_map[1][0] * p[0] + fit.to_map[1][1] * p[1] + fit.to_map[1][2] * p[2] + fit.to_map[1][3];
  *depth = fit.to_map[2][0] * p[0] + fit.to_map[2][1] * p[1] + fit.to_map[2][2] * p[2] + fit.to_map[2][3];
}

// The contact pass's rows: VIEW space (the AO's, car_frame.h Frame::view takes a world point to it) -> the map. `view` is the
// frame's world -> view matrix; false when it cannot be inverted.
inline bool ViewToMap(const Fit& fit, const temporal::Mat4& view, float out[3][4]) {
  temporal::Mat4 inverse;
  if (!temporal::Inverse(view, &inverse)) {
    return false;
  }
  for (int k = 0; k < 3; ++k) {
    // p_world = (p_view, 1) * inverse, then the row of the fit on (p_world, 1): the coefficient of p_view[j] is the sum over i.
    const double w[4] = {fit.to_map[k][0], fit.to_map[k][1], fit.to_map[k][2], fit.to_map[k][3]};
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int i = 0; i < 4; ++i) {
        sum += inverse.m[j][i] * w[i];
      }
      out[k][j] = float(sum);
    }
  }
  return true;
}

// A draw's model space -> the map's clip space (x and y in [-1, 1] across the map, y up; z the depth in [0, 1]; w 1), as the rows of
// a row-vector matrix: clip = p.x m[0..3] + p.y m[4..7] + p.z m[8..11] + m[12..15]. `model_to_world` is the draw's matrix
// (car_frame.h Draw::model_to_world). The map's y is down in u, v terms and up in clip space: y = 1 - 2 v.
inline void ModelToClip(const Fit& fit, const temporal::Mat4& model_to_world, float out[16]) {
  temporal::Mat4 clip;  // (p_world, 1) -> clip, row-vector form
  for (int i = 0; i < 4; ++i) {
    const double mu = i < 3 ? fit.to_map[0][i] : 0.0;
    const double mv = i < 3 ? fit.to_map[1][i] : 0.0;
    const double md = i < 3 ? fit.to_map[2][i] : 0.0;
    clip.m[i][0] = 2.0 * mu;
    clip.m[i][1] = -2.0 * mv;
    clip.m[i][2] = md;
    clip.m[i][3] = 0.0;
  }
  clip.m[3][0] = 2.0 * fit.to_map[0][3] - 1.0;
  clip.m[3][1] = 1.0 - 2.0 * fit.to_map[1][3];
  clip.m[3][2] = fit.to_map[2][3];
  clip.m[3][3] = 1.0;
  const temporal::Mat4 product = temporal::Mul(model_to_world, clip);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(product.m[r][c]);
    }
  }
}

// ---------------------------------------------------------------------------
// the draws' vertex positions: one pipeline per variant
// ---------------------------------------------------------------------------

// How a draw's POSITION is stored in its host vertex view (car_frame.h Draw::position_format): three floats (a float4 reads its
// first three), or four halves. Anything else is not drawn.
enum class PositionKind : uint8_t { kNone = 0, kFloat3 = 1, kHalf4 = 2 };

constexpr uint32_t KindBytes(PositionKind kind) { return kind == PositionKind::kFloat3 ? 12u : kind == PositionKind::kHalf4 ? 8u : 0u; }

// The pipeline's key: kind (2 bits), the host topology (6 bits, plume::RenderPrimitiveTopology's value) and the vertex stride (bytes).
// The position's offset in the vertex is NOT part of it: the vertex view is shifted by it (the element is then at offset 0).
constexpr uint32_t VariantKey(PositionKind kind, uint32_t topology, uint32_t stride) {
  return (stride << 8) | ((topology & 0x3Fu) << 2) | uint32_t(kind);
}
constexpr PositionKind VariantKind(uint32_t key) { return PositionKind(key & 3u); }
constexpr uint32_t VariantTopology(uint32_t key) { return (key >> 2) & 0x3Fu; }
constexpr uint32_t VariantStride(uint32_t key) { return key >> 8; }

// A vertex view that can be drawn from: the element fits in the stride, the offset in the view.
constexpr bool PositionUsable(PositionKind kind, uint32_t offset, uint32_t stride, uint64_t view_bytes) {
  return kind != PositionKind::kNone && stride >= KindBytes(kind) && stride < (1u << 20) && offset + KindBytes(kind) <= stride &&
         view_bytes >= uint64_t(offset) + KindBytes(kind);
}

// The strides of the player's car meshes the census saw (28 most, 12, 24, 32, 36): pipelines for these are made at the start.
inline constexpr uint32_t kKnownStrides[] = {12, 24, 28, 32, 36};

// ---------------------------------------------------------------------------
// the depth state
// ---------------------------------------------------------------------------

// The slope scaled bias of the depth only pipeline (a constant bias is the reader's, in metres: a stored depth difference times
// depth_m). Both backends take a float slope factor; the map is D32 float, where a constant bias would scale with the depth's exponent.
inline constexpr float kSlopeScaledDepthBias = 2.0f;

// ---------------------------------------------------------------------------
// the debug view
// ---------------------------------------------------------------------------

// mcla_gpu_car_shadow_map_view 1: a square in the lower left corner of the picture, a third of its height (96 to 512 pixels), 16
// pixels from the edges.
struct Inset {
  int x = 0;
  int y = 0;
  int size = 0;
};

inline Inset InsetFor(uint32_t render_width, uint32_t render_height) {
  Inset inset;
  const int margin = 16;
  int size = std::clamp(int(render_height / 3), 96, 512);
  size = std::min(size, std::max(16, int(render_width / 2)));
  size = std::min(size, std::max(16, int(render_height) - 2 * margin));
  inset.size = size;
  inset.x = margin;
  inset.y = std::max(0, int(render_height) - size - margin);
  return inset;
}

}  // namespace mcla::gpu::car_shadow_map
