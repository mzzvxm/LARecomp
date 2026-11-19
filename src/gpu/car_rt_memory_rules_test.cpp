// Check of the rules of the cars the camera does not see (car_rt_memory_rules.h): the game's vehicle poses, the match of the cars a frame drew to the game's
// vehicles, a car's draws kept relative to its vehicle and placed again at the vehicle's matrix of a later frame (the placement maths), which kits are placed
// (the class's cars, nearest first by the distance to the player, within the radius, not covered by a drawn car, with their meshes resident) and when a kit goes,
// and the instance list the placed cars end up in (car_rt_instances_rules.h AppendOthers with the cars of both kinds). A program of its own: it needs no
// renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_memory_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_memory_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "car_rt_memory_rules.h"

namespace cm = mcla::gpu::car_rt_memory;
namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace tp = mcla::gpu::temporal;
using mcla::gpu::gpu_class::GpuClass;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// A turn about the vertical axis, row vectors (v' = v M).
tp::Mat4 Yaw(double angle) {
  tp::Mat4 m = tp::Identity();
  m.m[0][0] = std::cos(angle);
  m.m[0][2] = -std::sin(angle);
  m.m[2][0] = std::sin(angle);
  m.m[2][2] = std::cos(angle);
  return m;
}

// A vehicle's pose: its heading and its position, as the rows right, up, back, position.
cm::VehicleRead Vehicle(uint32_t address, double x, double y, double z, double heading = 0.0, uint32_t tag = 0, cm::VehicleKind kind = cm::VehicleKind::kOpponent) {
  const tp::Mat4 m = tp::Mul(Yaw(heading), Translation(x, y, z));
  cm::VehicleRead v;
  v.address = address;
  v.tag = tag;
  v.kind = kind;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      v.rows[r][c] = m.m[r][c];
    }
  }
  return v;
}

// A car's parts: offsets in the vehicle's frame (x right, y up, z back) with their mesh keys.
struct Part {
  uint64_t key;
  double dx, dy, dz;
  uint64_t ps = 0;
};

std::vector<Part> BodyParts(uint64_t first_key, uint32_t count) {
  std::vector<Part> parts;
  for (uint32_t i = 0; i < count; ++i) {
    // a body, then wheels at the four corners, then small parts along the roof
    if (i == 0) {
      parts.push_back({first_key + i, 0.0, 0.5, 0.0});
    } else if (i <= 4) {
      parts.push_back({first_key + i, (i & 1) ? 0.8 : -0.8, 0.3, (i & 2) ? 1.4 : -1.4});
    } else {
      parts.push_back({first_key + i, 0.0, 1.1, -1.0 + 0.1 * double(i)});
    }
  }
  return parts;
}

// One simulated frame's inputs.
struct Frame {
  uint64_t guest_frame = 1;
  std::vector<cm::VehicleRead> vehicles;
  std::vector<cm::SeenCar> seen;
  std::vector<ci::OtherCar> cars;
  std::vector<ci::OtherDraw> draws;
  double player[3] = {0, 0, 0};
  bool vehicles_valid = true;
  bool player_known = true;

  // The game draws `parts` of the car whose vehicle is `v` (with the stencil ID `id`); `kept` lists it among the cars the selection kept.
  void Draw(uint32_t id, const cm::VehicleRead& v, const std::vector<Part>& parts, bool kept = true, double offset_x = 0.0) {
    const tp::Mat4 m = v.Matrix();
    ci::OtherCar car;
    car.stencil_id = id;
    double position[3];
    v.Position(position);
    const double distance = cm::Distance(position, player);
    for (const Part& p : parts) {
      ci::OtherDraw d;
      d.stencil_id = id;
      d.mesh_key = p.key;
      d.ps = p.ps;
      d.vs = 7;
      d.indices = 300;
      d.after_world = p.key % 7 == 0;
      d.model_to_world = tp::Mul(Translation(p.dx + offset_x, p.dy, p.dz), m);
      cm::NoteSeen(&seen, id, d.model_to_world.m[3]);
      if (kept) {
        draws.push_back(d);
        car.Add(d.model_to_world, distance);
      }
    }
    if (kept) {
      cars.push_back(car);
    }
  }
};

cm::CarMemory::Input InputOf(const Frame& f, const ci::Limits& limits) {
  cm::CarMemory::Input in;
  in.guest_frame = f.guest_frame;
  in.limits = &limits;
  in.player_known = f.player_known;
  for (int c = 0; c < 3; ++c) {
    in.player[c] = f.player[c];
  }
  in.vehicles_valid = f.vehicles_valid;
  in.vehicles = &f.vehicles;
  in.seen = &f.seen;
  in.cars = &f.cars;
  in.draws = &f.draws;
  return in;
}

ci::Limits High() { return ci::ClassLimits(GpuClass::kHigh); }

// All meshes resident, except those in `missing`.
struct Resident {
  std::set<uint64_t> missing;
  bool operator()(uint64_t key) const { return !missing.contains(key); }
};

// The world position of a placed draw of mesh `key` in `out`, from its matrix's translation; false when absent.
bool PlacedAt(const cm::CarMemory::Output& out, uint64_t key, double position[3]) {
  for (const ci::OtherDraw& d : out.draws) {
    if (d.mesh_key == key) {
      for (int c = 0; c < 3; ++c) {
        position[c] = d.model_to_world.m[3][c];
      }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// poses and matching
// ---------------------------------------------------------------------------

void TestPose() {
  cm::VehicleRead v = Vehicle(1, 10.0, 2.0, -5.0, 0.7);
  Check(cm::UsableRows(v.rows), "a car's pose is usable");
  const tp::Mat4 m = v.Matrix();
  Check(m.m[0][3] == 0.0 && m.m[1][3] == 0.0 && m.m[2][3] == 0.0 && m.m[3][3] == 1.0, "the matrix's fourth column is made (0, 0, 0, 1)");
  tp::Mat4 inverse;
  Check(tp::Inverse(m, &inverse), "the pose is invertible");
  double p[3];
  v.Position(p);
  Check(Near(p[0], 10.0) && Near(p[1], 2.0) && Near(p[2], -5.0), "the position is the last row");

  cm::VehicleRead bad = v;
  bad.rows[3][1] = std::numeric_limits<double>::quiet_NaN();
  Check(!cm::UsableRows(bad.rows), "a position that is not a number is no pose");
  bad = v;
  bad.rows[3][0] = 5.0e6;
  Check(!cm::UsableRows(bad.rows), "a position a million metres away is no pose");
  bad = v;
  for (int c = 0; c < 3; ++c) {
    bad.rows[0][c] *= 0.01;
  }
  Check(!cm::UsableRows(bad.rows), "an axis of a centimetre is no pose");
  bad = v;
  for (int c = 0; c < 3; ++c) {
    bad.rows[1][c] = bad.rows[0][c];
  }
  Check(!cm::UsableRows(bad.rows), "two equal axes span no volume");
  bad = v;
  for (int c = 0; c < 3; ++c) {
    bad.rows[2][c] *= 3.0;
  }
  Check(cm::UsableRows(bad.rows), "a part scaled by 3 (the squash and stretch is far less) is still taken");
  const cm::VehicleRead zero{};
  Check(!cm::UsableRows(zero.rows), "an all zero matrix (a freed object) is no pose");
  Check(Near(cm::ScanRadius(14.0), 23.0) && Near(cm::ScanRadius(10.0), 17.0), "the scan radius is 1.5 x the class's radius + 2 m");
}

void TestSeenAndMatch() {
  // NoteSeen sums the translations of every draw of an ID.
  std::vector<cm::SeenCar> seen;
  const double a[3] = {1.0, 0.0, 0.0}, b[3] = {3.0, 0.0, 2.0};
  cm::NoteSeen(&seen, 5, a);
  cm::NoteSeen(&seen, 5, b);
  cm::NoteSeen(&seen, 9, a);
  const double nan3[3] = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
  cm::NoteSeen(&seen, 9, nan3);
  Check(seen.size() == 2 && seen[0].draws == 2 && seen[1].draws == 1, "two IDs; a translation that is no number is not counted");
  double centre[3];
  Check(seen[0].Centre(centre) && Near(centre[0], 2.0) && Near(centre[2], 1.0), "the centre is the mean of the origins");

  // Two cars side by side (3.35 m, as the toolkit's neighbours) and the vehicles near them.
  std::vector<cm::VehicleRead> vehicles = {Vehicle(10, 3.35, 0, 0), Vehicle(11, 0.0, 0, 0), Vehicle(12, 40.0, 0, 0)};
  std::vector<cm::SeenCar> two;
  const double near0[3] = {0.3, 0.0, 0.2}, near1[3] = {3.1, 0.0, -0.3};
  cm::NoteSeen(&two, 1, near0);
  cm::NoteSeen(&two, 2, near1);
  std::vector<int32_t> of;
  std::vector<uint8_t> taken;
  const double worst = cm::MatchSeen(two, vehicles, cm::kMatchMetres, &of, &taken);
  Check(of[0] == 1 && of[1] == 0, "each car takes its own vehicle, though the other is within 3.5 m");
  Check(taken[0] == 1 && taken[1] == 1 && taken[2] == 0, "the vehicle far away is not taken");
  Check(worst > 0.3 && worst < 0.5, "the worst offset is the larger of the two matches");

  // A car with no vehicle near it matches nothing; a vehicle is matched once, to the nearer car.
  std::vector<cm::SeenCar> three;
  const double far_car[3] = {-20.0, 0.0, 0.0}, left[3] = {0.5, 0.0, 0.0}, right[3] = {0.9, 0.0, 0.0};
  cm::NoteSeen(&three, 1, far_car);
  cm::NoteSeen(&three, 2, left);
  cm::NoteSeen(&three, 3, right);
  cm::MatchSeen(three, vehicles, cm::kMatchMetres, &of, &taken);
  Check(of[0] == -1, "a car with no vehicle within the tolerance matches none");
  Check(of[1] == 1 && of[2] == -1, "two cars near one vehicle: the nearer takes it, the other none");

  // A tolerance that is too small matches nothing.
  cm::MatchSeen(two, vehicles, 0.1, &of, &taken);
  Check(of[0] == -1 && of[1] == -1, "a tight tolerance matches nothing");
}

// ---------------------------------------------------------------------------
// the placement maths
// ---------------------------------------------------------------------------

void TestPlacement() {
  const cm::VehicleRead v0 = Vehicle(1, 10.0, 2.0, -5.0, 0.7);
  tp::Mat4 inverse;
  Check(tp::Inverse(v0.Matrix(), &inverse), "the vehicle's inverse");
  // A part's matrix in the world: offset in the vehicle's frame, then the vehicle's pose.
  const tp::Mat4 part = tp::Mul(Translation(0.8, 0.3, 1.4), v0.Matrix());
  float rel[4][3];
  Check(cm::MakeRelative(part, inverse, rel), "a part's matrix is made relative to its vehicle");
  Check(Near(rel[3][0], 0.8, 1e-5) && Near(rel[3][1], 0.3, 1e-5) && Near(rel[3][2], 1.4, 1e-5), "the relative translation is the part's offset in the vehicle's frame");
  Check(Near(rel[0][0], 1.0, 1e-5) && Near(rel[1][1], 1.0, 1e-5) && Near(rel[2][2], 1.0, 1e-5) && std::fabs(rel[0][1]) < 1e-5, "its rotation is the identity");

  // The same vehicle: the part comes back.
  const tp::Mat4 same = cm::Place(rel, v0.Matrix());
  Check(tp::MaxAbsDiff(same, part) < 1e-4, "placed at the vehicle it was recorded at, the part is where it was");
  // The vehicle moved by (5, 0, -3) without turning: every part moves by that.
  const cm::VehicleRead v1 = Vehicle(1, 15.0, 2.0, -8.0, 0.7);
  const tp::Mat4 moved = cm::Place(rel, v1.Matrix());
  Check(Near(moved.m[3][0], part.m[3][0] + 5.0, 1e-4) && Near(moved.m[3][1], part.m[3][1], 1e-4) && Near(moved.m[3][2], part.m[3][2] - 3.0, 1e-4),
        "the vehicle moved: the part moved with it");
  Check(Near(moved.m[0][0], part.m[0][0], 1e-4) && Near(moved.m[2][1], part.m[2][1], 1e-4), "... and kept its heading");
  // The vehicle turned a quarter about its own origin: the part goes round it.
  const cm::VehicleRead v2 = Vehicle(1, 10.0, 2.0, -5.0, 0.7 + 1.5707963267948966);
  const tp::Mat4 turned = cm::Place(rel, v2.Matrix());
  const double offset_x = part.m[3][0] - 10.0, offset_z = part.m[3][2] + 5.0;  // the part's offset from the vehicle's origin, before the turn
  // Yaw(a): x' = x cos a + z sin a, z' = -x sin a + z cos a for row vectors (as Yaw builds it): a quarter turn takes (x, z) to (z, -x).
  Check(Near(turned.m[3][0] - 10.0, offset_z, 1e-4) && Near(turned.m[3][2] + 5.0, -offset_x, 1e-4) && Near(turned.m[3][1], part.m[3][1], 1e-4),
        "the vehicle turned a quarter about itself: the part's offset turned with it");
  // A part's own rotation (a wheel turned by the steering) rides along: the relative matrix holds it.
  const tp::Mat4 wheel = tp::Mul(tp::Mul(Yaw(0.3), Translation(-0.8, 0.3, -1.4)), v0.Matrix());
  float wheel_rel[4][3];
  Check(cm::MakeRelative(wheel, inverse, wheel_rel), "a wheel turned by the steering");
  const tp::Mat4 wheel_moved = cm::Place(wheel_rel, v1.Matrix());
  Check(Near(wheel_moved.m[0][0], wheel.m[0][0], 1e-4) && Near(wheel_moved.m[0][2], wheel.m[0][2], 1e-4), "the steering of the wheel rides with the car");
  // A matrix that is not affine, or not finite, is refused.
  tp::Mat4 perspective = part;
  perspective.m[0][3] = 0.5;
  float junk[4][3];
  Check(!cm::MakeRelative(perspective, inverse, junk), "a part with a perspective column is refused");
  tp::Mat4 nan_part = part;
  nan_part.m[1][1] = std::numeric_limits<double>::quiet_NaN();
  Check(!cm::MakeRelative(nan_part, inverse, junk), "a part with a NaN is refused");
}

// ---------------------------------------------------------------------------
// the update
// ---------------------------------------------------------------------------

// The R8 beside the player: drawn in frame 1, culled from frame 2 on.
void TestPlacedWhenCulled() {
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  const ci::Limits limits = High();
  const cm::VehicleRead r8 = Vehicle(100, 3.35, 0.3, 0.0, 0.0);
  const std::vector<Part> parts = BodyParts(1000, 12);
  Resident resident;

  // Frame 1: the car is drawn (ID 4).
  Frame f1;
  f1.guest_frame = 100;
  f1.vehicles = {r8};
  f1.Draw(4, r8, parts);
  memory.Update(InputOf(f1, limits), resident, &out);
  Check(out.stats.matched == 1 && out.stats.recorded == 1 && out.stats.kits == 1, "frame 1: the drawn car is matched to its vehicle and its kit is made");
  Check(out.cars.empty() && out.draws.empty() && out.stats.covered == 1, "frame 1: a drawn car is not placed from memory (its own draws stand)");
  Check(memory.kits().size() == 1 && memory.kits()[0].draws.size() == 12, "the kit holds the car's twelve draws");

  // Frame 2: the car is culled (no draws) and the vehicle moved on a little.
  Frame f2;
  f2.guest_frame = 101;
  const cm::VehicleRead r8_later = Vehicle(100, 3.35, 0.3, 0.4, 0.0);
  f2.vehicles = {r8_later};
  memory.Update(InputOf(f2, limits), resident, &out);
  Check(out.cars.size() == 1 && out.draws.size() == 12, "frame 2: the culled car is placed from memory with its twelve draws");
  Check(out.cars[0].remembered && out.cars[0].stencil_id >= cm::kRememberedIdBase && out.cars[0].stencil_id < ci::kIdSpace, "a remembered car has an ID above the game's");
  Check(Near(out.cars[0].distance, std::sqrt(3.35 * 3.35 + 0.3 * 0.3 + 0.4 * 0.4), 1e-6), "its distance is its vehicle's from the player");
  double body[3];
  Check(PlacedAt(out, 1000, body) && Near(body[0], 3.35, 1e-4) && Near(body[1], 0.8, 1e-4) && Near(body[2], 0.4, 1e-4), "the body is placed at the vehicle's new position");
  // Part 1001 is a wheel at (0.8, 0.3, -1.4) in the vehicle's frame: the vehicle is at (3.35, 0.3, 0.4) heading 0.
  double wheel[3];
  Check(PlacedAt(out, 1001, wheel) && Near(wheel[0], 3.35 + 0.8, 1e-4) && Near(wheel[1], 0.3 + 0.3, 1e-4) && Near(wheel[2], 0.4 - 1.4, 1e-4),
        "the wheel is placed at its offset from the vehicle's new position");
  // The draws keep their kit's flags.
  bool flags_ok = true;
  for (const ci::OtherDraw& d : out.draws) {
    flags_ok = flags_ok && d.after_world == (d.mesh_key % 7 == 0) && d.indices == 300 && d.vs == 7 && d.stencil_id == out.cars[0].stencil_id;
  }
  Check(flags_ok, "each placed draw has its kit's flags, shaders and ID");
  Check(out.stats.placed_cars == 1 && out.stats.placed_draws == 12 && out.stats.not_resident == 0, "the stats count them");

  // The car turns away (a quarter) at frame 3: the whole kit turns about the vehicle.
  Frame f3;
  f3.guest_frame = 102;
  f3.vehicles = {Vehicle(100, 3.35, 0.3, 0.4, 1.5707963267948966)};
  memory.Update(InputOf(f3, limits), resident, &out);
  double wheel3[3];
  Check(PlacedAt(out, 1001, wheel3) && out.cars.size() == 1, "frame 3: placed again");
  // The wheel's offset from the vehicle was (0.8, 0.3, 1.4)? (i = 1: dx 0.8, dz -1.4) rotated by a quarter turn: (x, z) -> (z, -x).
  Check(Near(wheel3[0] - 3.35, -1.4, 1e-4) && Near(wheel3[2] - 0.4, -0.8, 1e-4), "frame 3: the wheel went round the car with it");

  // Frame 4: the car is on screen again: drawn, not placed, the kit refreshed.
  Frame f4;
  f4.guest_frame = 103;
  f4.vehicles = {Vehicle(100, 3.35, 0.3, 0.8, 1.5707963267948966)};
  f4.Draw(6, f4.vehicles[0], parts);
  memory.Update(InputOf(f4, limits), resident, &out);
  Check(out.cars.empty() && out.stats.covered == 1 && out.stats.recorded == 1, "frame 4: drawn again: no duplicate, the kit refreshed");
}

// The kit placed at the vehicle's matrix against the draws of the same frame: the proof of the placement maths on the game's own data.
void TestPrediction() {
  const ci::Limits limits = High();
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  const std::vector<Part> parts = BodyParts(1000, 10);
  const cm::VehicleRead v1 = Vehicle(100, 3.35, 0.3, 0.0, 0.4);
  Frame f1;
  f1.guest_frame = 10;
  f1.vehicles = {v1};
  f1.Draw(2, v1, parts);
  memory.Update(InputOf(f1, limits), Resident{}, &out);
  Check(out.stats.predicted_draws == 0, "the first sight has no kit to compare");
  // The car drove 1.1 m and turned a little; the game draws the same parts at the same places in its frame: the prediction is right to float precision.
  const cm::VehicleRead v2 = Vehicle(100, 3.9, 0.3, 0.9, 0.45);
  Frame f2;
  f2.guest_frame = 11;
  f2.vehicles = {v2};
  f2.Draw(7, v2, parts);
  memory.Update(InputOf(f2, limits), Resident{}, &out);
  Check(out.stats.predicted_draws == 10 && out.stats.prediction_worst < 1e-4 && out.stats.prediction_sum < 1e-3, "a rigid car: the kit predicts the game's draws to a tenth of a millimetre");
  // The suspension compressed 5 cm at one wheel in the game's frame: that draw is off by 5 cm, the rest are right.
  const cm::VehicleRead v3 = Vehicle(100, 4.4, 0.3, 1.4, 0.5);
  Frame f3;
  f3.guest_frame = 12;
  f3.vehicles = {v3};
  std::vector<Part> sunk = parts;
  sunk[2].dy -= 0.05;
  f3.Draw(7, v3, sunk);
  memory.Update(InputOf(f3, limits), Resident{}, &out);
  Check(out.stats.predicted_draws == 10 && Near(out.stats.prediction_worst, 0.05, 1e-3) && Near(out.stats.prediction_sum, 0.05, 1e-3),
        "a wheel 5 cm lower than the kit has it: the worst error is 5 cm");
  // Another set of parts (a car that changed LOD): no comparison.
  const cm::VehicleRead v4 = Vehicle(100, 4.9, 0.3, 1.9, 0.5);
  Frame f4;
  f4.guest_frame = 13;
  f4.vehicles = {v4};
  f4.Draw(7, v4, BodyParts(1000, 9));
  memory.Update(InputOf(f4, limits), Resident{}, &out);
  Check(out.stats.predicted_draws == 0, "a different number of parts: nothing compared");
  Check(!cm::Describe(out.stats).empty() && cm::Describe(out.stats).find("no kit was compared") != std::string::npos, "the log's words say so");
  const cm::Totals& totals = memory.totals();
  Check(totals.predicted_draws == 20 && totals.prediction_worst > 0.04, "the totals add up the frames");
}

void TestNeverSeen() {
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  const ci::Limits limits = High();
  Frame f;
  f.vehicles = {Vehicle(100, 3.35, 0.3, 0.0), Vehicle(101, -3.35, 0.3, 0.0)};
  memory.Update(InputOf(f, limits), Resident{}, &out);
  Check(out.cars.empty() && out.draws.empty() && out.stats.kits == 0 && out.stats.vehicles == 2, "two cars in the game's list, never drawn: nothing is placed");
}

void TestCoverage() {
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  const ci::Limits limits = High();
  const cm::VehicleRead a = Vehicle(100, 3.35, 0.3, 0.0), b = Vehicle(101, -3.35, 0.3, 0.0);
  const std::vector<Part> parts = BodyParts(1000, 8);
  Frame f1;
  f1.guest_frame = 10;
  f1.vehicles = {a, b};
  f1.Draw(2, a, parts);
  f1.Draw(3, b, BodyParts(2000, 8));
  memory.Update(InputOf(f1, limits), Resident{}, &out);
  Check(out.stats.kits == 2 && out.stats.recorded == 2, "two cars drawn: two kits");

  // Frame 2: a is drawn (and kept), b is culled: only b is placed.
  Frame f2;
  f2.guest_frame = 11;
  f2.vehicles = {a, b};
  f2.Draw(2, a, parts);
  memory.Update(InputOf(f2, limits), Resident{}, &out);
  Check(out.cars.size() == 1 && out.stats.covered == 1, "a drawn, b culled: b alone is placed");
  double p[3];
  Check(PlacedAt(out, 2000, p) && Near(p[0], -3.35, 1e-3) && !PlacedAt(out, 1000, p), "the placed car is b's (its meshes), not a's");

  // Frame 3: a is drawn but the selection did not keep it (a car beyond the count): it still covers its vehicle.
  Frame f3;
  f3.guest_frame = 12;
  f3.vehicles = {a, b};
  f3.Draw(2, a, parts, /*kept=*/false);
  memory.Update(InputOf(f3, limits), Resident{}, &out);
  Check(out.cars.size() == 1 && out.stats.covered == 1, "a is drawn but not kept: it still covers its vehicle (no ghost on top of it)");
  Check(PlacedAt(out, 2000, p) && !PlacedAt(out, 1000, p), "... and only b is placed");
}

void TestClassCaps() {
  const ci::Limits mid = ci::ClassLimits(GpuClass::kMid);
  Check(mid.cars == 2 && Near(mid.radius, 10.0), "the Mid class: 2 cars within 10 m");
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  // Four parked cars at 3, 5, 7 and 12 m from the player, all drawn once, then all culled.
  const double distances[4] = {7.0, 3.0, 12.0, 5.0};
  Frame f1;
  f1.guest_frame = 1;
  std::vector<cm::VehicleRead> vehicles;
  for (uint32_t i = 0; i < 4; ++i) {
    vehicles.push_back(Vehicle(200 + i, distances[i], 0.0, 0.0));
  }
  f1.vehicles = vehicles;
  for (uint32_t i = 0; i < 4; ++i) {
    f1.Draw(2 + i, vehicles[i], BodyParts(1000 * (i + 1), 8), /*kept=*/true);
  }
  memory.Update(InputOf(f1, ci::ClassLimits(GpuClass::kHigh)), Resident{}, &out);
  Check(out.stats.kits == 4, "four kits");

  Frame f2;
  f2.guest_frame = 2;
  f2.vehicles = vehicles;
  memory.Update(InputOf(f2, mid), Resident{}, &out);
  Check(out.cars.size() == 2, "the Mid class places two");
  Check(Near(out.cars[0].distance, 3.0) && Near(out.cars[1].distance, 5.0), "the nearest two, nearest first, by the distance to the player");
  Check(out.stats.out_of_range == 1, "the car at 12 m is beyond the Mid class's 10 m: kept, not placed");
  Frame f3;
  f3.guest_frame = 3;
  f3.vehicles = vehicles;
  memory.Update(InputOf(f3, High()), Resident{}, &out);
  Check(out.cars.size() == 4 && Near(out.cars[3].distance, 12.0), "the High class places all four (the 12 m car is within its 14 m)");
  ci::Limits two = High();
  two.cars = 3;
  Frame f4;
  f4.guest_frame = 4;
  f4.vehicles = vehicles;
  memory.Update(InputOf(f4, two), Resident{}, &out);
  Check(out.cars.size() == 3, "a count of three places three");
  Frame f5;
  f5.guest_frame = 5;
  f5.vehicles = vehicles;
  ci::Limits off{};
  memory.Update(InputOf(f5, off), Resident{}, &out);
  Check(out.cars.empty() && memory.kits().empty(), "the other cars off: nothing is placed and the kits are given up");
}

void TestDrops() {
  const ci::Limits limits = High();
  const std::vector<Part> parts = BodyParts(1000, 10);
  const auto drawn_once = [&](cm::CarMemory* memory, cm::VehicleRead v, cm::CarMemory::Output* out) {
    Frame f;
    f.guest_frame = 50;
    f.vehicles = {v};
    f.Draw(2, v, parts);
    memory->Update(InputOf(f, limits), Resident{}, out);
  };
  cm::CarMemory::Output out;
  {
    // The vehicle leaves the list: the kit goes.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0), &out);
    Frame f;
    f.guest_frame = 51;
    memory.Update(InputOf(f, limits), Resident{}, &out);
    Check(out.stats.dropped_gone == 1 && memory.kits().empty() && out.cars.empty(), "the vehicle is gone from the lists: the kit is dropped");
  }
  {
    // The pool slot got another model.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0, 0.0, /*tag=*/0xA0), &out);
    Frame f;
    f.guest_frame = 51;
    f.vehicles = {Vehicle(100, 4.0, 0.0, 0.0, 0.0, /*tag=*/0xB0)};
    memory.Update(InputOf(f, limits), Resident{}, &out);
    Check(out.stats.dropped_tag == 1 && out.cars.empty(), "the same slot holds another model: the kit is dropped");
  }
  {
    // The car jumped (a recycled slot, a teleport): 100 m in a frame.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0), &out);
    Frame f;
    f.guest_frame = 51;
    f.vehicles = {Vehicle(100, 4.0, 0.0, 12.0)};  // within the player's radius, but 12 m in one frame: over the 7.5 m allowed
    memory.Update(InputOf(f, limits), Resident{}, &out);
    Check(out.stats.dropped_jump == 1 && out.cars.empty(), "a jump of 12 m in one frame: another car, the kit is dropped");
  }
  {
    // A car that moves by a car's speed is the same car: 1.2 m a frame (72 m/s at 60 FPS: a hard case).
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, -3.0), &out);
    for (uint64_t k = 1; k <= 5; ++k) {
      Frame f;
      f.guest_frame = 50 + k;
      f.vehicles = {Vehicle(100, 4.0, 0.0, -3.0 + 1.2 * double(k))};
      memory.Update(InputOf(f, limits), Resident{}, &out);
    }
    Check(out.cars.size() == 1 && memory.kits().size() == 1, "a car moving 1.2 m a frame for five frames stays the same car");
    // After a hitch (10 frames missed by the scene) the allowance grows with the frames.
    Frame hitch;
    hitch.guest_frame = 65;
    hitch.vehicles = {Vehicle(100, 4.0, 0.0, 3.0 + 12.0)};
    memory.Update(InputOf(hitch, limits), Resident{}, &out);
    Check(out.stats.dropped_jump == 0, "ten frames later 12 m further is within the allowance");
  }
  {
    // The meshes were given back: under half resident: the kit is dropped; all given back: dropped.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0), &out);
    Frame f;
    f.guest_frame = 51;
    f.vehicles = {Vehicle(100, 4.0, 0.0, 0.0)};
    Resident most;  // 6 of 10 resident
    for (uint64_t k = 1006; k < 1010; ++k) {
      most.missing.insert(k);
    }
    memory.Update(InputOf(f, limits), most, &out);
    Check(out.cars.size() == 1 && out.draws.size() == 6 && out.stats.not_resident == 4, "six of ten meshes resident: the car is placed with those six");
    Frame g;
    g.guest_frame = 52;
    g.vehicles = {Vehicle(100, 4.0, 0.0, 0.0)};
    Resident few;  // 4 of 10 resident
    for (uint64_t k = 1004; k < 1010; ++k) {
      few.missing.insert(k);
    }
    memory.Update(InputOf(g, limits), few, &out);
    Check(out.cars.empty() && out.stats.dropped_meshes == 1 && memory.kits().empty(), "four of ten resident: under half: the kit is dropped");
  }
  {
    // No vehicle list this frame (not readable): nothing is placed and nothing is dropped.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0), &out);
    Frame f;
    f.guest_frame = 51;
    f.vehicles_valid = false;
    memory.Update(InputOf(f, limits), Resident{}, &out);
    Check(out.cars.empty() && memory.kits().size() == 1 && out.stats.dropped_gone == 0, "a list that could not be read: nothing placed, the kit stays");
    Frame g;
    g.guest_frame = 52;
    g.player_known = false;
    g.vehicles = {Vehicle(100, 4.0, 0.0, 0.0)};
    memory.Update(InputOf(g, limits), Resident{}, &out);
    Check(out.cars.empty() && memory.kits().size() == 1, "no player's vehicle: nothing placed, the kit stays");
    Frame h;
    h.guest_frame = 53;
    h.vehicles = {Vehicle(100, 4.0, 0.0, 0.0)};
    memory.Update(InputOf(h, limits), Resident{}, &out);
    Check(out.cars.size() == 1, "the list is back: the car is placed again");
  }
  {
    // A vehicle beyond the scan: not in the list at all (the reader leaves it out) -> dropped; inside the scan but beyond the radius -> kept, not placed.
    cm::CarMemory memory;
    drawn_once(&memory, Vehicle(100, 4.0, 0.0, 0.0), &out);
    Frame f;
    f.guest_frame = 51;
    f.vehicles = {Vehicle(100, 4.0, 0.0, 5.0)};
    f.player[0] = -12.0;  // the player drove off: the car is 16.8 m from him, beyond 14 m, within the scan 23 m
    memory.Update(InputOf(f, limits), Resident{}, &out);
    Check(out.cars.empty() && out.stats.out_of_range == 1 && memory.kits().size() == 1, "beyond the radius, within the scan: kept, not placed");
    Frame g;
    g.guest_frame = 52;
    g.vehicles = {Vehicle(100, 4.0, 0.0, 5.0)};
    g.player[0] = -2.0;
    memory.Update(InputOf(g, limits), Resident{}, &out);
    Check(out.cars.size() == 1, "back within the radius: placed again");
  }
}

void TestKitPolicy() {
  const ci::Limits limits = High();
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  const cm::VehicleRead v = Vehicle(100, 4.0, 0.0, 0.0);
  Frame f1;
  f1.guest_frame = 10;
  f1.vehicles = {v};
  f1.Draw(2, v, BodyParts(1000, 20));
  memory.Update(InputOf(f1, limits), Resident{}, &out);
  Check(memory.kits()[0].draws.size() == 20, "a kit of twenty draws");
  // The car appears in a smaller LOD (fewer draws): the kit keeps the bigger set.
  Frame f2;
  f2.guest_frame = 11;
  f2.vehicles = {v};
  f2.Draw(2, v, BodyParts(1000, 10));
  memory.Update(InputOf(f2, limits), Resident{}, &out);
  Check(memory.kits()[0].draws.size() == 20 && out.stats.recorded == 0 && out.stats.covered == 1, "a listing under 90 percent of the kit does not replace it (the car is still drawn, not placed)");
  // ... unless the kit is old.
  Frame f3;
  f3.guest_frame = 10 + cm::kRefreshFrames + 5;
  f3.vehicles = {v};
  f3.Draw(2, v, BodyParts(1000, 10));
  memory.Update(InputOf(f3, limits), Resident{}, &out);
  Check(memory.kits()[0].draws.size() == 10 && out.stats.recorded == 1, "an old kit is replaced by whatever the car draws now");
  // A listing as large as the kit replaces it (the pose is current).
  Frame f4;
  f4.guest_frame = f3.guest_frame + 1;
  f4.vehicles = {v};
  f4.Draw(2, v, BodyParts(3000, 10));
  memory.Update(InputOf(f4, limits), Resident{}, &out);
  Check(memory.kits()[0].draws[0].mesh_key == 3000 && out.stats.recorded == 1, "an equal listing replaces the kit");
  // A listing whose matrices are all garbage is refused and the old kit stays as it was.
  Frame f4b;
  f4b.guest_frame = f4.guest_frame + 1;
  f4b.vehicles = {v};
  f4b.Draw(2, v, BodyParts(5000, 10));
  for (ci::OtherDraw& d : f4b.draws) {
    d.model_to_world.m[1][1] = std::numeric_limits<double>::quiet_NaN();
  }
  memory.Update(InputOf(f4b, limits), Resident{}, &out);
  Check(out.stats.record_refused == 1 && out.stats.recorded == 0 && memory.kits()[0].draws.size() == 10 && memory.kits()[0].draws[0].mesh_key == 3000,
        "a listing with no usable matrix is refused: the old kit stays");
  Check(out.stats.covered == 1 && out.cars.empty(), "... and the drawn car is still not placed on top of its own draws");
  // Too few draws: no kit.
  cm::CarMemory few;
  Frame f5;
  f5.guest_frame = 1;
  f5.vehicles = {v};
  f5.Draw(2, v, BodyParts(1000, 3));
  few.Update(InputOf(f5, limits), Resident{}, &out);
  Check(few.kits().empty() && out.stats.record_refused == 1, "a car seen through a gap (three parts) makes no kit");
  // The player's own car and a drawn car with no vehicle (a police car): no kit, no placement.
  cm::CarMemory police;
  Frame f6;
  f6.guest_frame = 1;
  f6.vehicles = {};
  f6.Draw(2, Vehicle(100, 4.0, 0.0, 0.0), BodyParts(1000, 10));
  police.Update(InputOf(f6, limits), Resident{}, &out);
  Check(police.kits().empty() && out.stats.matched == 0 && out.stats.seen == 1, "a drawn car the list does not know makes no kit");
  // A vehicle with a pose that is garbage when the kit would be made.
  cm::CarMemory garbage;
  cm::VehicleRead bad = v;
  bad.rows[1][1] = std::numeric_limits<double>::quiet_NaN();
  Frame f7;
  f7.guest_frame = 1;
  f7.vehicles = {bad};
  f7.Draw(2, v, BodyParts(1000, 10));
  garbage.Update(InputOf(f7, limits), Resident{}, &out);
  Check(garbage.kits().empty() && out.stats.record_refused == 1, "a vehicle with a pose that is no pose: no kit");
}

void TestStoreFull() {
  const ci::Limits limits = ci::LimitsFor(GpuClass::kHigh, 8, 0.0);
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  // Nine cars drawn one after another, a frame apiece, the store holds eight.
  std::vector<cm::VehicleRead> all_vehicles;
  for (uint32_t i = 0; i < 9; ++i) {
    Frame f;
    f.guest_frame = 10 + i;
    const cm::VehicleRead v = Vehicle(300 + i, 4.0 + 3.0 * double(i), 0.0, 0.0);  // 3 m apart: each car is matched to its own vehicle
    all_vehicles.push_back(v);
    f.vehicles = all_vehicles;
    f.Draw(2, v, BodyParts(1000 * (i + 1), 8));
    memory.Update(InputOf(f, limits), Resident{}, &out);
  }
  Check(memory.kits().size() == cm::kMaxKits, "the store holds eight kits");
  bool first_gone = true, last_there = false;
  std::set<uint32_t> slots;
  for (const cm::Kit& k : memory.kits()) {
    first_gone = first_gone && k.vehicle != 300;
    last_there = last_there || k.vehicle == 308;
    slots.insert(k.slot);
  }
  Check(first_gone && last_there, "the ninth car took the place of the one drawn longest ago");
  Check(slots.size() == cm::kMaxKits && *slots.rbegin() == cm::kMaxKits - 1, "every kit has a slot of its own, 0 to 7");
}

// ---------------------------------------------------------------------------
// the instance list
// ---------------------------------------------------------------------------

void TestInstanceList() {
  const tp::Mat4 view = tp::Identity();
  const ci::Limits limits = High();
  const auto find = [](uint64_t, ci::MeshRef* ref) {
    *ref = ci::MeshRef{};
    return true;
  };
  cm::CarMemory memory;
  cm::CarMemory::Output out;
  // Two cars drawn at 9 and 11 m, three remembered cars at 2, 3 and 4 m (drawn first, culled now).
  std::vector<cm::VehicleRead> vehicles;
  for (uint32_t i = 0; i < 3; ++i) {
    vehicles.push_back(Vehicle(400 + i, 2.0 + double(i), 0.0, 0.0));
  }
  Frame f1;
  f1.guest_frame = 1;
  f1.vehicles = vehicles;
  for (uint32_t i = 0; i < 3; ++i) {
    f1.Draw(2 + i, vehicles[i], BodyParts(1000 * (i + 1), 6));
  }
  memory.Update(InputOf(f1, limits), Resident{}, &out);

  Frame f2;
  f2.guest_frame = 2;
  const cm::VehicleRead drawn_a = Vehicle(500, 9.0, 0.0, 0.0), drawn_b = Vehicle(501, 11.0, 0.0, 0.0);
  f2.vehicles = {vehicles[0], vehicles[1], vehicles[2], drawn_a, drawn_b};
  f2.Draw(5, drawn_a, BodyParts(7000, 6));
  f2.Draw(6, drawn_b, BodyParts(8000, 6));
  memory.Update(InputOf(f2, limits), Resident{}, &out);
  Check(out.cars.size() == 3 && out.draws.size() == 18, "three remembered cars, six draws each");

  // The instance list: 5 cars of 6 draws with a count of 4: the nearest four of all, by distance (2, 3, 4 and 9 m).
  std::vector<ci::OtherCar> cars = f2.cars;
  cars.insert(cars.end(), out.cars.begin(), out.cars.end());
  std::vector<ci::OtherDraw> draws = f2.draws;
  draws.insert(draws.end(), out.draws.begin(), out.draws.end());
  ci::List list;
  ci::AppendOthers(&list, draws, cars, view, /*flip_z=*/true, 640, rr::kMaxInstances, find, /*max_cars=*/4);
  Check(list.stats.cars == 4 && list.stats.cars_cut == 1 && list.items.size() == 24, "the plan keeps the nearest four of the five cars: the 11 m car is cut");
  Check(list.stats.remembered_cars == 3 && list.stats.remembered_instances == 18 && list.stats.other_instances == 24, "three of the four are remembered, 18 of the 24 instances");
  // The cars' rows: a remembered car's has a negative radius, a drawn car's a positive one.
  const uint32_t car_rows = uint32_t(list.items.size());
  uint32_t negative = 0, positive = 0;
  for (uint32_t k = 0; k < list.stats.cars; ++k) {
    ci::CarRow row;
    std::memcpy(&row, &list.rows[car_rows + k], sizeof(row));
    negative += row.radius < 0.0f ? 1u : 0u;
    positive += row.radius > 0.0f ? 1u : 0u;
    Check(Near(std::fabs(row.radius), ci::kCarRadius, 1e-6), "the radius's size is the car's radius");
  }
  Check(negative == 3 && positive == 1, "the remembered cars' rows say so (a negative radius), the drawn car's does not");
  // An instance names its car's row (the remembered ones too): the draws' `draw` fields point into the cars' rows.
  bool rows_ok = true;
  for (uint32_t i = 0; i < list.items.size(); ++i) {
    rows_ok = rows_ok && list.rows[i].draw >= car_rows && list.rows[i].draw < car_rows + list.stats.cars;
  }
  Check(rows_ok, "every instance names a car's row");
  // The boxes: one a car with an instance.
  Check(list.boxes.size() == 4, "a box for each of the four cars");

  // Without a count (0) all five are listed, as before (the drawn cars alone behave as they did).
  ci::List all;
  ci::AppendOthers(&all, draws, cars, view, true, 640, rr::kMaxInstances, find);
  Check(all.stats.cars == 5 && all.items.size() == 30, "no count: all five cars");
  ci::List drawn_only;
  ci::AppendOthers(&drawn_only, f2.draws, f2.cars, view, true, 640, rr::kMaxInstances, find, 4);
  Check(drawn_only.stats.cars == 2 && drawn_only.stats.remembered_cars == 0, "drawn cars alone: nothing remembered in the stats");

  // The IDs of the remembered cars do not collide with the game's: the placed draws of the first remembered car are not mistaken for a drawn car's.
  std::set<uint32_t> ids;
  for (const ci::OtherCar& c : cars) {
    ids.insert(c.stencil_id);
  }
  Check(ids.size() == 5, "five cars, five IDs");
  // A draw whose ID is outside the slot table is ignored, not a crash.
  std::vector<ci::OtherDraw> stray = {draws[0]};
  stray[0].stencil_id = 200;
  ci::List ignore;
  std::vector<ci::OtherCar> one = {cars[0]};
  ci::AppendOthers(&ignore, stray, one, view, true, 640, rr::kMaxInstances, find);
  Check(ignore.items.empty(), "a draw with an ID outside the table is left out");
}

}  // namespace

int main() {
  TestPose();
  TestSeenAndMatch();
  TestPlacement();
  TestPlacedWhenCulled();
  TestPrediction();
  TestNeverSeen();
  TestCoverage();
  TestClassCaps();
  TestDrops();
  TestKitPolicy();
  TestStoreFull();
  TestInstanceList();
  std::printf("car_rt_memory_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
