// Check of the car shadow map's rules (car_shadow_map_rules.h): the setting's words and sizes, when a frame draws, the car's frame, the
// estimate and the held centre and radius, the fit (the axes, the planes, the depth's direction, the car covered whatever the yaw and the
// sun), the matrices (the contact pass's rows from the AO's view space, the depth-only vertex shader's rows from a draw's model space, in
// float at a world position hundreds of metres from the origin), the pipeline variants and the debug view's square. A program of its own:
// it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_shadow_map_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_shadow_map_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "car_shadow_map_rules.h"

namespace cs = mcla::gpu::car_shadow_map;
namespace gc = mcla::gpu::gpu_class;
namespace tp = mcla::gpu::temporal;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kMapStabilityTolerance = 1e-9;  // Fraction of the map width, height or depth range.
double Rad(double degrees) { return degrees * kPi / 180.0; }

// The sun from an elevation and an azimuth (degrees): world y up.
void Sun(double elevation, double azimuth, double out[3]) {
  out[0] = std::cos(Rad(elevation)) * std::sin(Rad(azimuth));
  out[1] = std::sin(Rad(elevation));
  out[2] = std::cos(Rad(elevation)) * std::cos(Rad(azimuth));
}

// A car's root matrix: rows right (x), up (y), forward (z) turned by `yaw` about the world's up axis, row 3 the position. `shuffle`
// reorders the rows (a game whose root has its up axis first, say): the three source rows go to these places.
tp::Mat4 CarWorld(double yaw_degrees, double x, double y, double z, const int shuffle[3] = nullptr) {
  const double c = std::cos(Rad(yaw_degrees));
  const double s = std::sin(Rad(yaw_degrees));
  const double rows[3][3] = {{c, 0.0, -s}, {0.0, 1.0, 0.0}, {s, 0.0, c}};
  tp::Mat4 m = tp::Identity();
  for (int r = 0; r < 3; ++r) {
    const int source = shuffle ? shuffle[r] : r;
    for (int k = 0; k < 3; ++k) {
      m.m[r][k] = rows[source][k];
    }
  }
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// (p, 1) * M.
void Transform(const tp::Mat4& m, const double p[3], double out[3]) {
  for (int j = 0; j < 3; ++j) {
    out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
  }
}

// The car's box in model space (root on the ground at the middle): 4.6 m long (z), 1.9 wide (x), 1.3 high (y).
constexpr double kBox[2][3] = {{-0.95, 0.0, -2.3}, {0.95, 1.3, 2.3}};

// The world box of the car's box through `world`: what car_frame.h Frame::bounds is, as far as it can be.
void WorldBox(const tp::Mat4& world, double bmin[3], double bmax[3]) {
  for (int i = 0; i < 3; ++i) {
    bmin[i] = 1e30;
    bmax[i] = -1e30;
  }
  for (int corner = 0; corner < 8; ++corner) {
    const double p[3] = {kBox[corner & 1][0], kBox[(corner >> 1) & 1][1], kBox[(corner >> 2) & 1][2]};
    double w[3];
    Transform(world, p, w);
    for (int i = 0; i < 3; ++i) {
      bmin[i] = std::min(bmin[i], w[i]);
      bmax[i] = std::max(bmax[i], w[i]);
    }
  }
}

double Dist(const double a[3], const double b[3]) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The held values after the first frame of a car at `world`, its box exact.
cs::Held HeldFor(const tp::Mat4& world) {
  cs::Pose pose;
  cs::MakePose(world, &pose);
  double bmin[3], bmax[3];
  WorldBox(world, bmin, bmax);
  return cs::Hold(cs::Held{}, cs::EstimateOf(pose, true, bmin, bmax));
}

// ---------------------------------------------------------------------------------------------------------------------

void TestSetting() {
  // `auto`: the big map on a discrete card, the small one on an integrated one
  cs::Choice low = cs::Resolve(gc::GpuClass::kLow, "auto");
  cs::Choice mid = cs::Resolve(gc::GpuClass::kMid, "auto");
  cs::Choice high = cs::Resolve(gc::GpuClass::kHigh, "auto");
  Check(low.level == cs::Level::kUltra && low.size() == 2048 && low.automatic && !low.unknown, "legacy auto on Low uses Ultra, 2048");
  Check(mid.level == cs::Level::kUltra && mid.size() == 2048 && mid.automatic, "auto on the Mid class: ultra, 2048");
  Check(high.level == cs::Level::kUltra && high.size() == 2048 && high.automatic, "auto on the High class: ultra, 2048");
  // the words
  for (gc::GpuClass cls : {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh}) {
    const cs::Choice off = cs::Resolve(cls, "off");
    const cs::Choice h = cs::Resolve(cls, "high");
    const cs::Choice u = cs::Resolve(cls, "Ultra");
    Check(off.level == cs::Level::kOff && off.size() == 0 && !off.automatic, "off is off on every class");
    Check(h.level == cs::Level::kHigh && h.size() == 1024 && !h.automatic, "high is 1024 on every class, set");
    Check(u.level == cs::Level::kUltra && u.size() == 2048 && !u.automatic, "ultra (any case) is 2048 on every class, set");
  }
  // a word that is no value counts as auto (the low of an older larecomp.toml)
  const cs::Choice old = cs::Resolve(gc::GpuClass::kMid, "low");
  Check(old.automatic && old.unknown && old.level == cs::Level::kUltra, "\"low\" is no value: it runs as auto and says so");
  const cs::Choice empty = cs::Resolve(gc::GpuClass::kLow, "");
  Check(empty.automatic && empty.unknown && empty.level == cs::Level::kUltra, "an empty text runs as auto");
  Check(cs::Describe(high, gc::GpuClass::kHigh).find("2048x2048") != std::string::npos, "the words of the log line name the size");
  Check(cs::Describe(cs::Resolve(gc::GpuClass::kHigh, "off"), gc::GpuClass::kHigh).find("2048") == std::string::npos,
        "an off map names no size");
  // no level is called low
  Check(std::string(cs::LevelName(cs::Level::kHigh)) == "high" && std::string(cs::LevelName(cs::Level::kUltra)) == "ultra" &&
            std::string(cs::LevelName(cs::Level::kOff)) == "off",
        "the levels' words are off, high and ultra");
}

void TestDecide() {
  double noon[3], low_sun[3], below[3], empty[3] = {0, 0, 0};
  Sun(60, 0, noon);
  Sun(2.0, 0, low_sun);
  Sun(-10, 0, below);
  using W = cs::Why;
  const cs::Level on = cs::Level::kUltra;
  Check(cs::Decide(on, true, true, true, noon) == W::kRun, "everything on, a car, noon: drawn");
  Check(cs::Decide(cs::Level::kOff, true, true, true, noon) == W::kSettingOff, "the setting off");
  Check(cs::Decide(on, false, true, true, noon) == W::kContactOff, "the contact shadows off: nobody reads the map");
  Check(cs::Decide(on, true, false, true, noon) == W::kNotPlayView, "the pause menu's world: no map");
  Check(cs::Decide(on, true, true, false, noon) == W::kNoCar, "no car");
  Check(cs::Decide(on, true, true, true, empty) == W::kNoSun, "no direction to the sun");
  Check(cs::Decide(on, true, true, true, low_sun) == W::kSunLow, "the sun at 2 degrees: the contact shadows are gated");
  Check(cs::Decide(on, true, true, true, below) == W::kSunLow, "the sun below the horizon");
  double edge[3];
  Sun(3.5, 40, edge);
  Check(cs::Decide(on, true, true, true, edge) == W::kRun, "the sun at 3.5 degrees: drawn");
  double scaled[3] = {noon[0] * 5, noon[1] * 5, noon[2] * 5};
  Check(cs::Decide(on, true, true, true, scaled) == W::kRun, "a direction of any length over 0.5 is taken as a direction");
}

void TestPose() {
  cs::Pose pose;
  const tp::Mat4 world = CarWorld(30.0, -620.03, 17.80, -644.88);
  Check(cs::MakePose(world, &pose), "a root matrix makes a pose");
  Check(std::fabs(pose.position[0] + 620.03) < 1e-12 && std::fabs(pose.position[2] + 644.88) < 1e-12, "the position is row 3");
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      double dot = 0;
      for (int k = 0; k < 3; ++k) {
        dot += pose.axes[a][k] * pose.axes[b][k];
      }
      Check(std::fabs(dot - (a == b ? 1.0 : 0.0)) < 1e-12, "the axes are orthonormal");
    }
  }
  // a root with scale and shear still gives an exact frame
  tp::Mat4 sheared = world;
  for (int k = 0; k < 3; ++k) {
    sheared.m[0][k] *= 1.2;
    sheared.m[1][k] = sheared.m[1][k] * 0.9 + 0.05 * sheared.m[0][k];
  }
  Check(cs::MakePose(sheared, &pose), "a sheared root matrix makes a pose");
  double dot01 = 0;
  for (int k = 0; k < 3; ++k) {
    dot01 += pose.axes[0][k] * pose.axes[1][k];
  }
  Check(std::fabs(dot01) < 1e-12, "the shear is taken out");
  // degenerate and not finite
  tp::Mat4 flat = world;
  for (int k = 0; k < 3; ++k) {
    flat.m[2][k] = flat.m[0][k];
  }
  Check(!cs::MakePose(flat, &pose), "two equal rows: no pose");
  tp::Mat4 nan = world;
  nan.m[1][1] = std::nan("");
  Check(!cs::MakePose(nan, &pose), "a NaN: no pose");
  // the up axis, whichever row it is
  const int orders[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
  for (const auto& order : orders) {
    cs::Pose p;
    cs::MakePose(CarWorld(10.0, 0, 0, 0, order), &p);
    const int up = cs::UpIndex(p);
    Check(std::fabs(p.axes[up][1]) > 0.99, "UpIndex finds the axis that points up");
    const int side = cs::SideIndex(up);
    Check(side != up && std::fabs(p.axes[side][1]) < 1e-9, "SideIndex is a horizontal axis");
  }
}

void TestEstimateAndHold() {
  const tp::Mat4 world = CarWorld(25.0, -620.03, 17.80, -644.88);
  cs::Pose pose;
  cs::MakePose(world, &pose);
  double bmin[3], bmax[3];
  WorldBox(world, bmin, bmax);
  const cs::Estimate est = cs::EstimateOf(pose, true, bmin, bmax);
  Check(est.from_bounds, "the car's box is believed");
  // the centre: 0.65 m up the car's own up axis (row 1), nowhere else
  Check(std::fabs(est.offset[0]) < 1e-9 && std::fabs(est.offset[1] - 0.65) < 1e-9 && std::fabs(est.offset[2]) < 1e-9,
        "the centre is found in the car's own axes");
  const double diagonal = std::sqrt(1.9 * 1.9 + 1.3 * 1.3 + 4.6 * 4.6) * 0.5;  // the tight half diagonal
  Check(est.radius >= diagonal - 1e-9, "the radius of the world box holds the car");
  // no box, a huge box (stray triangles hundreds of metres away), a box not around the root: the fallback sphere
  const cs::Estimate none = cs::EstimateOf(pose, false, bmin, bmax);
  Check(!none.from_bounds && none.radius == cs::kFallbackRadius && none.offset[0] == 0 && none.offset[1] == 0, "no box: the fallback sphere on the root");
  double far_min[3] = {bmin[0], bmin[1], bmin[2]};
  double far_max[3] = {bmax[0] + 880.0, bmax[1], bmax[2]};
  Check(!cs::EstimateOf(pose, true, far_min, far_max).from_bounds, "a box 880 m wide is not a car's");
  double elsewhere_min[3] = {bmin[0] + 30, bmin[1], bmin[2]};
  double elsewhere_max[3] = {bmax[0] + 30, bmax[1], bmax[2]};
  Check(!cs::EstimateOf(pose, true, elsewhere_min, elsewhere_max).from_bounds, "a box that does not contain the root's place is not the car's");
  double bad_min[3] = {bmax[0], bmin[1], bmin[2]};
  double bad_max[3] = {bmin[0], bmax[1], bmax[2]};
  Check(!cs::EstimateOf(pose, true, bad_min, bad_max).from_bounds, "an inverted box is not believed");
  double nan_min[3] = {std::nan(""), bmin[1], bmin[2]};
  Check(!cs::EstimateOf(pose, true, nan_min, bmax).from_bounds, "a NaN box is not believed");

  // the first frame holds what it saw; the radius is a multiple of the quantum and holds the dead band too
  const cs::Held first = cs::Hold(cs::Held{}, est);
  Check(first.valid && std::fabs(first.offset[1] - 0.65) < 1e-9, "the first estimate is held");
  Check(std::fabs(first.radius / cs::kRadiusQuantum - std::round(first.radius / cs::kRadiusQuantum)) < 1e-9, "the radius is a multiple of the quantum");
  Check(first.radius >= est.radius + cs::kCentreDeadBand - 1e-9, "the radius holds the dead band: the held centre may be off by that much");
  // the car turns: the world box changes shape, its centre stays put and its radius wanders; the held values do not follow
  cs::Held held = first;
  int centre_moves = 0;
  int radius_steps = 0;
  for (int yaw = 0; yaw <= 360; yaw += 3) {
    const tp::Mat4 w = CarWorld(double(yaw), -620.03, 17.80, -644.88);
    cs::Pose p;
    cs::MakePose(w, &p);
    double lo[3], hi[3];
    WorldBox(w, lo, hi);
    // jitter the centre of the box by a few centimetres, as a union of rotated part boxes does
    lo[0] += 0.08 * std::sin(yaw * 0.3);
    hi[0] += 0.08 * std::sin(yaw * 0.3);
    const cs::Held next = cs::Hold(held, cs::EstimateOf(p, true, lo, hi));
    if (std::fabs(next.offset[1] - held.offset[1]) > 1e-12 || std::fabs(next.offset[0] - held.offset[0]) > 1e-12) {
      ++centre_moves;
    }
    if (next.radius != held.radius) {
      ++radius_steps;
    }
    Check(next.radius >= held.radius, "the held radius never shrinks");
    held = next;
  }
  std::printf("  a full turn of the car: the held centre moved %d times, the radius stepped %d times, ends at %.2f m\n", centre_moves,
              radius_steps, held.radius);
  Check(centre_moves == 0, "a full turn with the box's jitter does not move the held centre");
  Check(radius_steps <= 3 && held.radius <= 4.6, "the radius settles in a few steps and stays a car's size");
  // a jump says another car
  cs::Held jumped = held;
  cs::Estimate other = est;
  other.offset[1] += 2.0;
  other.radius = 2.0;
  jumped = cs::Hold(held, other);
  Check(std::fabs(jumped.offset[1] - (0.65 + 2.0)) < 1e-9, "a centre that jumps over kResetJump starts over");
  // a much smaller car: starts over, the radius shrinks
  cs::Estimate small = est;
  small.radius = 1.4;
  const cs::Held smaller = cs::Hold(held, small);
  Check(smaller.radius < held.radius, "a car half the size starts over with a smaller radius");
  // a frame with no believed box keeps what is held; with nothing held it takes the fallback
  Check(cs::Hold(held, none).radius == held.radius && cs::Hold(held, none).offset[1] == held.offset[1], "no box: the held fit stays");
  const cs::Held fallback = cs::Hold(cs::Held{}, none);
  Check(fallback.valid && fallback.radius >= cs::kFallbackRadius && fallback.radius <= cs::kFallbackRadius + cs::kRadiusQuantum,
        "no box and nothing held: the fallback sphere");
}

void TestFit() {
  const tp::Mat4 world = CarWorld(25.0, -620.03, 17.80, -644.88);
  cs::Pose pose;
  cs::MakePose(world, &pose);
  const cs::Held held = HeldFor(world);
  double sun[3];
  Sun(50, 130, sun);
  cs::Fit fit;
  Check(cs::MakeFit(sun, pose, held, 2048, &fit) && fit.valid, "a fit");
  // the axes
  const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  Check(std::fabs(dot(fit.right, fit.up)) < 1e-12 && std::fabs(dot(fit.right, fit.sun)) < 1e-12 && std::fabs(dot(fit.up, fit.sun)) < 1e-12,
        "right, up and sun are at right angles");
  Check(std::fabs(dot(fit.right, fit.right) - 1) < 1e-12 && std::fabs(dot(fit.up, fit.up) - 1) < 1e-12 && std::fabs(dot(fit.sun, fit.sun) - 1) < 1e-12,
        "... and unit");
  double cross[3] = {fit.right[1] * fit.up[2] - fit.right[2] * fit.up[1], fit.right[2] * fit.up[0] - fit.right[0] * fit.up[2],
                     fit.right[0] * fit.up[1] - fit.right[1] * fit.up[0]};
  Check(dot(cross, fit.sun) > 0.999999, "right x up = sun: a right handed frame");
  // the map's right axis is the car's horizontal axis, projected: turning the car turns it
  const int side = cs::SideIndex(cs::UpIndex(pose));
  const double along = dot(pose.axes[side], sun);
  double projected[3] = {pose.axes[side][0] - along * sun[0], pose.axes[side][1] - along * sun[1], pose.axes[side][2] - along * sun[2]};
  const double length = std::sqrt(dot(projected, projected));
  Check(std::fabs(dot(projected, fit.right) / length - 1.0) < 1e-12, "the map's right axis is the car's horizontal axis projected on the light plane");
  // sizes
  Check(std::fabs(fit.texel_m - 2.0 * fit.half_extent / 2048.0) < 1e-15 && std::fabs(fit.depth_m - 2.0 * fit.half_depth) < 1e-15,
        "texel_m and depth_m");
  Check(fit.half_extent > 2.5 && fit.half_extent < 4.5, "the map is a few metres across, not a pile");
  std::printf("  an R34 at 2048: half extent %.2f m, texel %.2f mm, depth range %.2f m; at 1024 the texel is %.2f mm\n", fit.half_extent,
              fit.texel_m * 1000.0, fit.depth_m, fit.texel_m * 2000.0);
  // the centre
  double u, v, d;
  cs::MapOfWorld(fit, fit.centre, &u, &v, &d);
  Check(std::fabs(u - 0.5) < 1e-9 && std::fabs(v - 0.5) < 1e-9 && std::fabs(d - 0.5) < 1e-9, "the centre is in the middle of the map and of its depth");
  // along the sun: (u, v) do not change, the depth does, nearer the sun is smaller
  double toward[3] = {fit.centre[0] + fit.sun[0] * 2.0, fit.centre[1] + fit.sun[1] * 2.0, fit.centre[2] + fit.sun[2] * 2.0};
  double u2, v2, d2;
  cs::MapOfWorld(fit, toward, &u2, &v2, &d2);
  Check(std::fabs(u2 - u) < 1e-12 && std::fabs(v2 - v) < 1e-12, "a point along the sun keeps its texel");
  Check(d2 < d && std::fabs((d - d2) - 2.0 / fit.depth_m) < 1e-12, "nearer the sun is a smaller depth, 1 / depth_m a metre");
  // v is down: a point higher in the map's up direction has a smaller v; u grows with right
  double upward[3] = {fit.centre[0] + fit.up[0], fit.centre[1] + fit.up[1], fit.centre[2] + fit.up[2]};
  cs::MapOfWorld(fit, upward, &u2, &v2, &d2);
  Check(v2 < v && std::fabs(u2 - u) < 1e-12, "v is down: up in the map is a smaller v");
  double rightward[3] = {fit.centre[0] + fit.right[0], fit.centre[1] + fit.right[1], fit.centre[2] + fit.right[2]};
  cs::MapOfWorld(fit, rightward, &u2, &v2, &d2);
  Check(u2 > u && std::fabs(v2 - v) < 1e-12, "u grows to the right");
  // the planes: a receiver behind the car (the road under it, further from the sun than the far plane) is beyond depth 1 and one between
  // the car and the sun is below 0
  double behind[3] = {fit.centre[0] - fit.sun[0] * (fit.half_depth + 0.2), fit.centre[1] - fit.sun[1] * (fit.half_depth + 0.2),
                      fit.centre[2] - fit.sun[2] * (fit.half_depth + 0.2)};
  cs::MapOfWorld(fit, behind, &u2, &v2, &d2);
  Check(d2 > 1.0, "a receiver beyond the far plane has a depth above 1: it is behind the car");
  double front[3] = {fit.centre[0] + fit.sun[0] * (fit.half_depth + 0.2), fit.centre[1] + fit.sun[1] * (fit.half_depth + 0.2),
                     fit.centre[2] + fit.sun[2] * (fit.half_depth + 0.2)};
  cs::MapOfWorld(fit, front, &u2, &v2, &d2);
  Check(d2 < 0.0, "a point between the car and the sun, past the near plane, is below 0");
  // refusals
  const cs::Held nothing;
  Check(!cs::MakeFit(sun, pose, nothing, 2048, &fit) && !fit.valid, "nothing held: no fit");
  Check(!cs::MakeFit(sun, pose, held, 8, &fit), "a map of 8 texels: no fit");
  double none[3] = {0, 0, 0};
  Check(!cs::MakeFit(none, pose, held, 2048, &fit), "no sun: no fit");
  // the sun straight overhead: the car's axis is at right angles to it still, the right axis is fine
  double overhead[3] = {0, 1, 0};
  Check(cs::MakeFit(overhead, pose, held, 2048, &fit) && std::fabs(dot(fit.right, fit.sun)) < 1e-12, "the sun overhead still fits");
  // the sun along the car's own horizontal axis (level with the horizon, down the car): the other horizontal axis takes over
  double along_car[3] = {pose.axes[side][0], pose.axes[side][1], pose.axes[side][2]};
  const int other = 3 - cs::UpIndex(pose) - side;
  Check(cs::MakeFit(along_car, pose, held, 2048, &fit) && std::fabs(dot(fit.right, fit.sun)) < 1e-9 &&
            std::fabs(dot(fit.right, pose.axes[other])) > 0.999,
        "the sun along the car's axis: the car's other horizontal axis takes over");
}

// The car covered: every point of the car's box (and the ground under its shadow's start) is inside the map for every yaw and sun.
void TestCovered() {
  int checked = 0;
  int outside = 0;
  double worst_margin = 1e30;
  for (int yaw = 0; yaw < 360; yaw += 15) {
    const tp::Mat4 world = CarWorld(double(yaw), -620.03, 17.80, -644.88);
    cs::Pose pose;
    cs::MakePose(world, &pose);
    // the held fit as it would be after a drive: grown over a turn
    cs::Held held = HeldFor(CarWorld(0.0, -620.03, 17.80, -644.88));
    for (int turn = 0; turn < 360; turn += 5) {
      const tp::Mat4 w = CarWorld(double(turn), -620.03, 17.80, -644.88);
      cs::Pose p;
      cs::MakePose(w, &p);
      double lo[3], hi[3];
      WorldBox(w, lo, hi);
      held = cs::Hold(held, cs::EstimateOf(p, true, lo, hi));
    }
    for (double elevation : {4.0, 10.0, 25.0, 45.0, 65.0, 89.0}) {
      for (int azimuth = 0; azimuth < 360; azimuth += 40) {
        double sun[3];
        Sun(elevation, azimuth, sun);
        cs::Fit fit;
        if (!cs::MakeFit(sun, pose, held, 1024, &fit)) {
          Check(false, "a fit for a yaw and a sun");
          continue;
        }
        for (int i = 0; i < 5; ++i) {
          for (int j = 0; j < 5; ++j) {
            for (int k = 0; k < 9; ++k) {
              const double p[3] = {kBox[0][0] + (kBox[1][0] - kBox[0][0]) * i / 4.0, kBox[0][1] + (kBox[1][1] - kBox[0][1]) * j / 4.0,
                                   kBox[0][2] + (kBox[1][2] - kBox[0][2]) * k / 8.0};
              double w[3];
              Transform(world, p, w);
              double u, v, d;
              cs::MapOfWorld(fit, w, &u, &v, &d);
              ++checked;
              const double margin = std::min({u, 1.0 - u, v, 1.0 - v, d, 1.0 - d});
              worst_margin = std::min(worst_margin, margin);
              if (margin <= 0.0) {
                ++outside;
              }
            }
          }
        }
      }
    }
  }
  std::printf("  %d points of the car, every yaw and sun: %d outside the map, the least margin %.3f of the map\n", checked, outside, worst_margin);
  Check(outside == 0, "every point of the car is inside the map, at every yaw and sun");
  Check(worst_margin > 0.02, "... with a margin");
}

// A car that drives straight (the sun fixed in its frame) is rasterised onto the same texels in every frame; a turn moves the shadow smoothly.
void TestStable() {
  double sun[3];
  Sun(40, 70, sun);
  const cs::Held held = HeldFor(CarWorld(0.0, 0, 0, 0));
  const double points[4][3] = {{0.9, 1.2, -2.2}, {-0.9, 0.3, 2.3}, {0.0, 1.3, 0.0}, {0.5, 0.0, 1.0}};
  cs::Fit first;
  {
    cs::Pose p;
    cs::MakePose(CarWorld(33.0, -620.03, 17.80, -644.88), &p);
    cs::MakeFit(sun, p, held, 2048, &first);
  }
  double worst = 0.0;
  for (int step = 1; step < 40; ++step) {
    // the same yaw, the car somewhere else (and the sun with it, as a fixed sun in the car's frame needs a turn; here the car moves only)
    const tp::Mat4 world = CarWorld(33.0, -620.03 + step * 1.37, 17.80 + step * 0.01, -644.88 - step * 2.91);
    cs::Pose p;
    cs::MakePose(world, &p);
    cs::Fit fit;
    cs::MakeFit(sun, p, held, 2048, &fit);
    for (const auto& point : points) {
      double w0[3], w1[3];
      Transform(CarWorld(33.0, -620.03, 17.80, -644.88), point, w0);
      Transform(world, point, w1);
      double u0, v0, d0, u1, v1, d1;
      cs::MapOfWorld(first, w0, &u0, &v0, &d0);
      cs::MapOfWorld(fit, w1, &u1, &v1, &d1);
      worst = std::max({worst, std::fabs(u0 - u1), std::fabs(v0 - v1), std::fabs(d0 - d1)});
    }
  }
  std::printf("  the car driven 40 frames straight: the largest change of a model point's (u, v, depth) %.3g (a texel is %.3g)\n", worst, 1.0 / 2048.0);
  Check(worst < kMapStabilityTolerance, "a car driven straight lands on the same texels in every frame (no shimmer)");
  // a turn of half a degree: the shadow moves smoothly (a point 2.5 m from the centre moves 2.2 cm: under 0.01 of the map)
  double worst_step = 0.0;
  cs::Fit previous;
  for (int i = 0; i <= 720; ++i) {
    cs::Pose p;
    cs::MakePose(CarWorld(0.5 * i, -620.03, 17.80, -644.88), &p);
    cs::Fit fit;
    cs::MakeFit(sun, p, held, 2048, &fit);
    if (i > 0) {
      for (const auto& point : points) {
        double wp[3], wq[3];
        Transform(CarWorld(0.5 * (i - 1), -620.03, 17.80, -644.88), point, wp);
        Transform(CarWorld(0.5 * i, -620.03, 17.80, -644.88), point, wq);
        double up, vp, dp, uq, vq, dq;
        cs::MapOfWorld(previous, wp, &up, &vp, &dp);
        cs::MapOfWorld(fit, wq, &uq, &vq, &dq);
        worst_step = std::max({worst_step, std::fabs(up - uq), std::fabs(vp - vq), std::fabs(dp - dq)});
      }
    }
    previous = fit;
  }
  std::printf("  a full turn in half degrees: the largest step of a model point's (u, v, depth) %.4f of the map\n", worst_step);
  Check(worst_step < 0.012, "a turn moves the shadow smoothly, no jump of the map");
}

void TestMatrices() {
  const tp::Mat4 world = CarWorld(-40.0, -620.03, 17.80, -644.88);
  cs::Pose pose;
  cs::MakePose(world, &pose);
  const cs::Held held = HeldFor(world);
  double sun[3];
  Sun(35, 200, sun);
  cs::Fit fit;
  cs::MakeFit(sun, pose, held, 2048, &fit);

  // a camera: world -> the AO's view space (x right, y up, z forward), about 9 m from the car, looking at it
  const double eye[3] = {-613.0, 20.5, -640.0};
  double forward[3] = {pose.position[0] - eye[0], pose.position[1] + 0.6 - eye[1], pose.position[2] - eye[2]};
  const double fl = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
  for (double& f : forward) {
    f /= fl;
  }
  const double world_up[3] = {0, 1, 0};
  // a left handed view space: x right, y up, z forward; right = up x forward, the camera's up = forward x right
  double right[3] = {world_up[1] * forward[2] - world_up[2] * forward[1], world_up[2] * forward[0] - world_up[0] * forward[2],
                     world_up[0] * forward[1] - world_up[1] * forward[0]};
  const double rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (double& r : right) {
    r /= rl;
  }
  const double up[3] = {forward[1] * right[2] - forward[2] * right[1], forward[2] * right[0] - forward[0] * right[2],
                        forward[0] * right[1] - forward[1] * right[0]};
  tp::Mat4 view = tp::Identity();  // (p, 1) * view = (right . (p - eye), up . (p - eye), forward . (p - eye), 1)
  for (int k = 0; k < 3; ++k) {
    view.m[k][0] = right[k];
    view.m[k][1] = up[k];
    view.m[k][2] = forward[k];
  }
  view.m[3][0] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
  view.m[3][1] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
  view.m[3][2] = -(forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2]);

  float rows[3][4];
  const bool have_rows = cs::ViewToMap(fit, view, rows);
  Check(have_rows, "the contact pass's rows");
  if (!have_rows) return;
  double worst = 0.0;
  for (const auto& corner : kBox) {
    for (int mask = 0; mask < 4; ++mask) {
      const double p[3] = {kBox[mask & 1][0], corner[1], kBox[(mask >> 1) & 1][2]};
      double w[3];
      Transform(world, p, w);
      // the point in the AO's view space, then through the rows the pass reads
      const double pv[3] = {w[0] * view.m[0][0] + w[1] * view.m[1][0] + w[2] * view.m[2][0] + view.m[3][0],
                            w[0] * view.m[0][1] + w[1] * view.m[1][1] + w[2] * view.m[2][1] + view.m[3][1],
                            w[0] * view.m[0][2] + w[1] * view.m[1][2] + w[2] * view.m[2][2] + view.m[3][2]};
      Check(pv[2] > 1.0 && pv[2] < 12.0, "the car is in front of the camera, a few metres away (z forward is the linear depth)");
      double u, v, d;
      cs::MapOfWorld(fit, w, &u, &v, &d);
      float out[3];
      for (int k = 0; k < 3; ++k) {
        out[k] = rows[k][0] * float(pv[0]) + rows[k][1] * float(pv[1]) + rows[k][2] * float(pv[2]) + rows[k][3];
      }
      worst = std::max({worst, std::fabs(double(out[0]) - u), std::fabs(double(out[1]) - v), std::fabs(double(out[2]) - d)});
    }
  }
  std::printf("  AO view space -> map, float rows, camera about 9 m away at world x -620: the largest error %.3g of the map (a texel is %.3g)\n", worst, 1.0 / 2048.0);
  Check(worst < 2e-5, "the rows take a view space point to the same (u, v, depth) as the world point does");

  // a singular view: no rows
  tp::Mat4 singular = view;
  for (int j = 0; j < 4; ++j) {
    singular.m[1][j] = singular.m[0][j];
  }
  float junk[3][4];
  Check(!cs::ViewToMap(fit, singular, junk), "a view that cannot be inverted gives no rows");

  // a draw's rows, in float as the shader multiplies them: model point -> clip
  double worst_clip = 0.0;
  const tp::Mat4 draws[3] = {world, tp::Mul(CarWorld(10.0, 0.3, 0.1, -0.2), world), CarWorld(-40.0, -620.03 + 0.4, 17.80 + 0.25, -644.88 + 1.1)};
  for (const tp::Mat4& m : draws) {
    float clip_rows[16];
    cs::ModelToClip(fit, m, clip_rows);
    for (int a = 0; a <= 4; ++a) {
      for (int b = 0; b <= 4; ++b) {
        for (int c = 0; c <= 4; ++c) {
          const double p[3] = {-0.9 + 0.45 * a, 0.0 + 0.3 * b, -2.2 + 1.1 * c};
          double w[3];
          Transform(m, p, w);
          double u, v, d;
          cs::MapOfWorld(fit, w, &u, &v, &d);
          float clip[4];
          for (int j = 0; j < 4; ++j) {
            clip[j] = float(p[0]) * clip_rows[j] + float(p[1]) * clip_rows[4 + j] + float(p[2]) * clip_rows[8 + j] + clip_rows[12 + j];
          }
          worst_clip = std::max({worst_clip, std::fabs(double(clip[0]) - (2.0 * u - 1.0)), std::fabs(double(clip[1]) - (1.0 - 2.0 * v)),
                                 std::fabs(double(clip[2]) - d), std::fabs(double(clip[3]) - 1.0)});
        }
      }
    }
  }
  std::printf("  model space -> clip in float at x = -620: the largest error %.3g (a texel is %.3g of the clip range)\n", worst_clip, 2.0 / 2048.0);
  Check(worst_clip < 1e-4, "a draw's rows take a model point to the map's clip space, y up, depth 0 to 1, in float, far from the origin");
}

void TestVariants() {
  using K = cs::PositionKind;
  const uint32_t list = 4, strip = 5;  // plume::RenderPrimitiveTopology's values
  const uint32_t key = cs::VariantKey(K::kFloat3, list, 28);
  Check(cs::VariantKind(key) == K::kFloat3 && cs::VariantTopology(key) == list && cs::VariantStride(key) == 28, "a variant key round trips");
  Check(cs::VariantKey(K::kHalf4, strip, 8) != cs::VariantKey(K::kFloat3, strip, 8), "the kind is in the key");
  Check(cs::VariantKey(K::kFloat3, list, 28) != cs::VariantKey(K::kFloat3, strip, 28), "the topology is in the key");
  Check(cs::VariantKey(K::kFloat3, list, 28) != cs::VariantKey(K::kFloat3, list, 32), "the stride is in the key");
  Check(key >= 256 && cs::VariantKey(K::kFloat3, 0, 1) >= 256, "a depth variant's key never collides with the small keys of the matrix and view passes");
  Check(cs::PositionUsable(K::kFloat3, 0, 28, 28 * 100), "float3 at 0 of 28");
  Check(cs::PositionUsable(K::kFloat3, 16, 28, 28 * 100), "float3 at 16 of 28");
  Check(!cs::PositionUsable(K::kFloat3, 20, 28, 28 * 100), "float3 at 20 of 28 does not fit");
  Check(cs::PositionUsable(K::kHalf4, 0, 8, 8) && !cs::PositionUsable(K::kHalf4, 0, 6, 600), "half4 needs 8 bytes of stride");
  Check(!cs::PositionUsable(K::kNone, 0, 28, 28 * 100), "no kind: not usable");
  Check(!cs::PositionUsable(K::kFloat3, 0, 0, 100), "stride 0: not usable");
  Check(!cs::PositionUsable(K::kFloat3, 8, 28, 16), "a view too short for the element");
  Check(cs::KindBytes(K::kFloat3) == 12 && cs::KindBytes(K::kHalf4) == 8, "the element sizes");
  bool known_in_range = true;
  for (uint32_t stride : cs::kKnownStrides) {
    known_in_range = known_in_range && stride >= 12 && stride % 4 == 0;
  }
  Check(known_in_range, "the known strides are whole dwords of at least a float3");
}

void TestInset() {
  const cs::Inset at_4k = cs::InsetFor(3840, 2160);
  Check(at_4k.size == 512 && at_4k.x == 16 && at_4k.y == 2160 - 512 - 16, "4K: the largest square, in the lower left");
  const cs::Inset at_720 = cs::InsetFor(1280, 720);
  Check(at_720.size == 240 && at_720.y == 720 - 240 - 16, "720p: a third of the height");
  const cs::Inset tiny = cs::InsetFor(200, 150);
  Check(tiny.size >= 16 && tiny.size <= 100 && tiny.x + tiny.size <= 200 && tiny.y >= 0, "a tiny picture still has a square that fits");
}

// The map covers ALL of the car's shadow on the world, at any sun: a point of the ground in the shadow is a point of the car moved away
// from the sun along the sun's ray, and a view along the sun gives both the same (u, v), so its place in the square is the car's own,
// and its depth is the car's plus the way it was moved (over 1 when it is beyond the far plane: the reader shadows it, car_shadow_map.h).
// This is why the game's cascades can leave the car out wherever the map is drawn.
void TestShadowOnTheGround() {
  const tp::Mat4 world = CarWorld(37.0, -620.03, 17.80, -644.88);
  cs::Pose pose;
  cs::MakePose(world, &pose);
  double lo[3], hi[3];
  WorldBox(world, lo, hi);
  const cs::Held held = cs::Hold(cs::Held{}, cs::EstimateOf(pose, true, lo, hi));
  double worst_uv = 0.0;
  double least_gain = 1e30;
  int points = 0;
  for (double elevation : {3.0, 5.0, 12.0, 30.0, 60.0, 89.0}) {
    for (int azimuth = 0; azimuth < 360; azimuth += 60) {
      double sun[3];
      Sun(elevation, azimuth, sun);
      cs::Fit fit;
      Check(cs::MakeFit(sun, pose, held, 2048, &fit), "a fit");
      for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < 9; ++k) {
          const double p[3] = {kBox[0][0] + (kBox[1][0] - kBox[0][0]) * i / 4.0, kBox[0][1], kBox[0][2] + (kBox[1][2] - kBox[0][2]) * k / 8.0};
          double car[3];
          Transform(world, p, car);
          // The ground under it along the sun's ray: the point where the ray from the car away from the sun meets the road's plane.
          const double drop = car[1] - 17.80;  // the car's root is on the road at y 17.80: the point's height above it
          if (!(sun[1] > 0.0) || drop < 0.0) {
            continue;
          }
          const double t = drop / sun[1];
          const double ground[3] = {car[0] - t * sun[0], car[1] - t * sun[1], car[2] - t * sun[2]};
          double uc, vc, dc, ug, vg, dg;
          cs::MapOfWorld(fit, car, &uc, &vc, &dc);
          cs::MapOfWorld(fit, ground, &ug, &vg, &dg);
          worst_uv = std::max(worst_uv, std::max(std::fabs(uc - ug), std::fabs(vc - vg)));
          least_gain = std::min(least_gain, dg - dc);
          ++points;
        }
      }
    }
  }
  std::printf("  %d points of the car's shadow on the road, sun 3 to 89 degrees: the largest (u, v) difference from the car's %.2e, the least depth gain %.3g\n",
              points, worst_uv, least_gain);
  Check(points > 100 && worst_uv < 1e-9, "a ground point in the car's shadow has the (u, v) of the car point that shades it, at any sun");
  Check(least_gain >= -1e-12, "and it is never nearer the sun than that point: the reader's compare shadows it (beyond the far plane too)");
}

// Who casts the player's car's shadow on the world: the map takes it after kOwnAfterFrames frames of a map at full contact strength,
// gives it up at once, and a sun that wanders round 12 degrees does not flip it.
void TestOwner() {
  cs::Owner o;
  for (uint32_t i = 1; i < cs::kOwnAfterFrames; ++i) {
    o = cs::NextOwner(o, true, 1.0f, true);
    Check(!o.owns, "the map has not been there long enough: the game's cascades keep the car (frame " + std::to_string(i) + ")");
  }
  o = cs::NextOwner(o, true, 1.0f, true);
  Check(o.owns, "after kOwnAfterFrames frames of a map at full strength the map owns the shadow");
  Check(cs::kOwnAfterFrames >= 2, "a frame or two of both is not enough to take it: the guest's draw runs ahead of the renderer");
  // The strength: just under the take level never takes, and a hysteresis band keeps it.
  cs::Owner low;
  for (int i = 0; i < 10; ++i) {
    low = cs::NextOwner(low, true, 0.99f, true);
  }
  Check(!low.owns, "a strength of 0.99 (a sun at 11 degrees) never takes it");
  cs::Owner held = o;
  held = cs::NextOwner(held, true, 0.95f, true);
  Check(held.owns, "owning, a strength of 0.95 keeps it (the hysteresis band)");
  held = cs::NextOwner(held, true, 0.85f, true);
  Check(!held.owns && held.valid_run == 0, "a strength under kOwnKeepStrength gives it up and the count starts again");
  held = cs::NextOwner(held, true, 1.0f, true);
  Check(!held.owns, "and it is taken again only after the frames in a row");
  // A frame without a map gives it up at once, whatever the strength.
  cs::Owner gone = cs::NextOwner(o, false, 1.0f, true);
  Check(!gone.owns && gone.valid_run == 0, "no map this frame: the game's cascades draw the car again at once");
  // The debug switch.
  cs::Owner off = o;
  off = cs::NextOwner(off, true, 1.0f, false);
  Check(!off.owns && off.valid_run == 0, "mcla_gpu_car_shadow_map_owns off: never");
  // The gates of the contact shadows (ao.cpp): full strength needs the sun at 12 degrees or more (kGateFullDegrees) and a dry road.
  Check(cs::kOwnTakeStrength <= 1.0f && cs::kOwnTakeStrength > cs::kOwnKeepStrength, "the take level is over the keep level and not over 1");
  // The cell the hook reads.
  cs::PublishOwner(true);
  Check(cs::PlayerShadowIsTheMaps(), "the published ownership is what the hook reads");
  cs::PublishOwner(false);
  Check(!cs::PlayerShadowIsTheMaps(), "and it is given up the same way");
}

void TestRasterReuse() {
  double sun[3];
  Sun(34.0, 15.0, sun);
  cs::Held held{true, {0.12, 0.25, -0.15}, 3.25};
  const tp::Mat4 parked = CarWorld(10.0, 430.0, -5.0, 120.0);
  const tp::Mat4 moved = CarWorld(10.0, 510.0, -5.0, 140.0);
  cs::Pose parked_pose, moved_pose;
  cs::Fit parked_fit, moved_fit;
  Check(cs::MakePose(parked, &parked_pose) && cs::MakePose(moved, &moved_pose), "straight-drive poses");
  Check(cs::MakeFit(sun, parked_pose, held, 2048, &parked_fit) && cs::MakeFit(sun, moved_pose, held, 2048, &moved_fit), "straight-drive fits");
  const cs::Fit saved = cs::RelativeFit(parked_fit, parked_pose);
  const cs::Fit now = cs::RelativeFit(moved_fit, moved_pose);
  Check(cs::ReuseFit(saved, now, 5, 6) && cs::ReuseFit(saved, now, 5, 8), "translation holds the same raster for up to three intervening frames");
  Check(!cs::ReuseFit(saved, now, 5, 9) && !cs::ReuseFit(saved, now, 5, 4), "the age limit and serial rollback force a new map");
  cs::PartKey original{1, 256, 0}, translated{1, 256, 0};
  cs::ModelToClip(parked_fit, parked, original.clip);
  cs::ModelToClip(cs::AttachFit(saved, moved_pose), moved, translated.clip);
  // The car-space round trip can change roundoff near zero. Check the projected box at the same precision as a fresh fit.
  // CHECK: Confirm the projection bound with the pinned Linux compiler.
  for (const auto& corner : kBox) {
    for (int mask = 0; mask < 4; ++mask) {
      const double point[4] = {kBox[mask & 1][0], corner[1], kBox[(mask >> 1) & 1][2], 1.0};
      for (int c = 0; c < 4; ++c) {
        double delta = 0.0;
        for (int r = 0; r < 4; ++r) {
          delta += point[r] * (double(translated.clip[4 * r + c]) - original.clip[4 * r + c]);
        }
        const double tolerance = c < 2 ? 2.0 * kMapStabilityTolerance : kMapStabilityTolerance;
        Check(c == 3 ? delta == 0.0 : std::fabs(delta) < tolerance,
              "reattaching preserves the box's projected coordinates in a straight drive");
      }
    }
  }
  const double extent[3] = {1.0, 0.4, 0.6};
  Check(cs::SamePart(original, translated, extent, moved_fit), "unchanged body can reuse its depth");
  translated.clip[4] += 0.01f;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "steering rotates a part enough to refresh");
  translated = original;
  translated.clip[13] += 0.002f;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "suspension travel refreshes the raster");
  translated = original;
  translated.clip[14] += 0.001f;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "a depth-only part change also refreshes the raster");
  translated = original;
  translated.clip[12] += 0.00001f;
  Check(cs::SamePart(original, translated, extent, moved_fit), "a subtexel part change can be held");
  translated = original;
  translated.mesh++;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "a new mesh or LOD refreshes");
  translated = original;
  translated.variant++;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "a replay variant change refreshes");
  translated = original;
  translated.position_offset++;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "a position attribute change refreshes");
  translated = original;
  translated.mesh = original.mesh = 0;
  Check(!cs::SamePart(original, translated, extent, moved_fit), "unknown geometry never reuses");

  for (double turn : {0.01, 0.25, 8.0}) {
    cs::Pose turned;
    cs::Fit turned_fit;
    Check(cs::MakePose(CarWorld(10.0 + turn, 510.0, -5.0, 140.0), &turned) && cs::MakeFit(sun, turned, held, 2048, &turned_fit), "turned pose and fit");
    Check(cs::ReuseFit(saved, cs::RelativeFit(turned_fit, turned), 5, 6) == (turn < 0.05), "body yaw above the angular limit redraws immediately");
  }
  Sun(36.0, 15.0, sun);
  Check(cs::MakeFit(sun, moved_pose, held, 2048, &moved_fit) && !cs::ReuseFit(saved, cs::RelativeFit(moved_fit, moved_pose), 5, 6), "moving sun refreshes");
  cs::Fit changed = now;
  changed.texel_m *= 2.0;
  Check(!cs::ReuseFit(saved, changed, 5, 6), "resized texture refreshes");
  changed = now;
  changed.half_depth += 1.0;
  Check(!cs::ReuseFit(saved, changed, 5, 6), "changed depth range refreshes");
  changed = now;
  changed.centre[0] += 0.1;
  Check(!cs::ReuseFit(saved, changed, 5, 6), "moved fit centre refreshes");
  changed = now;
  changed.sun[0] = -changed.sun[0];
  Check(!cs::ReuseFit(saved, changed, 5, 6), "a changed sun axis alone refreshes");
  changed = now;
  changed.right[0] = -changed.right[0];
  Check(!cs::ReuseFit(saved, changed, 5, 6), "a map grid flip alone refreshes");
}

}  // namespace

int main() {
  TestSetting();
  TestDecide();
  TestPose();
  TestEstimateAndHold();
  TestFit();
  TestCovered();
  TestStable();
  TestRasterReuse();
  TestMatrices();
  TestVariants();
  TestInset();
  TestShadowOnTheGround();
  TestOwner();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
