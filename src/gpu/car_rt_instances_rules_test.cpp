// Check of the other cars' rules in the car ray tracing scene (car_rt_instances_rules.h): the limits by graphics class and the two overrides, which cars
// the selection keeps (the nearest few within a radius, whatever the order the draws come in), a car's body centre, the budgets of a car that is new,
// and the instance list (whole cars within the budget, the transform in the view space, the rows and the cars' rows after them). A program of its
// own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_instances_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_instances_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"

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

tp::Mat4 Multiply(const tp::Mat4& a, const tp::Mat4& b) { return tp::Mul(a, b); }

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// ---------------------------------------------------------------------------
// the limits
// ---------------------------------------------------------------------------

void TestLimits() {
  const ci::Limits low = ci::ClassLimits(GpuClass::kLow);
  const ci::Limits mid = ci::ClassLimits(GpuClass::kMid);
  const ci::Limits high = ci::ClassLimits(GpuClass::kHigh);
  Check(!low.On() && low.cars == 0, "the Low class has no other cars");
  Check(mid.On() && mid.cars == 2 && Near(mid.radius, 10.0), "the Mid class: 2 cars within 10 m");
  Check(high.On() && high.cars == 4 && Near(high.radius, 14.0), "the High class: 4 cars within 14 m");
  Check(high.cars > mid.cars && high.radius > mid.radius && high.instances > mid.instances && high.stage_triangles > mid.stage_triangles &&
            high.content_bytes > mid.content_bytes && high.min_indices < mid.min_indices,
        "the weaker the class the fewer cars, the nearer, the smaller the budgets, the more small parts left out");
  Check(mid.instances <= rr::kMaxInstances && high.instances <= rr::kMaxInstances, "the class budgets are inside the top level's cap");
  // The instance budget holds the cars the class asks for at about 140 draws a car.
  Check(mid.instances >= mid.cars * 140 && high.instances >= high.cars * 140, "the budget holds the cars at about 140 draws a car");

  ci::Limits l = ci::LimitsFor(GpuClass::kHigh, -1, 0.0);
  Check(l == high, "no override: the class's cell");
  l = ci::LimitsFor(GpuClass::kHigh, 0, 0.0);
  Check(!l.On(), "0 cars turns them off");
  l = ci::LimitsFor(GpuClass::kMid, 3, 0.0);
  Check(l.cars == 3 && l.instances >= 480 && Near(l.radius, 10.0) && l.min_indices == mid.min_indices, "3 cars on the Mid class: the instances grow to hold them, the rest stays");
  l = ci::LimitsFor(GpuClass::kHigh, 99, 0.0);
  Check(l.cars == ci::kMaxCars && l.instances <= rr::kMaxInstances, "the cars are clamped to the most, the instances to the top level's cap");
  l = ci::LimitsFor(GpuClass::kHigh, -1, 30.0);
  Check(Near(l.radius, 30.0) && l.cars == 4, "the radius override");
  l = ci::LimitsFor(GpuClass::kHigh, -1, 1000.0);
  Check(Near(l.radius, ci::kMaxRadius), "the radius is clamped");
  l = ci::LimitsFor(GpuClass::kLow, 2, 0.0);
  Check(l.On() && l.cars == 2 && Near(l.radius, 14.0), "asked for by hand on the Low class: the High cell's numbers");
  l = ci::LimitsFor(GpuClass::kLow, -1, 12.0);
  Check(!l.On() || l.cars == 0, "a radius alone does not turn the Low class on");
  l = ci::LimitsFor(GpuClass::kMid, -1, -5.0);
  Check(l == mid, "a radius of 0 or less keeps the class's");
  Check(ci::Describe(low).find("no other cars") != std::string::npos && ci::Describe(high).find("4 cars") != std::string::npos, "the words");
}

// ---------------------------------------------------------------------------
// which cars are kept
// ---------------------------------------------------------------------------

ci::Limits Limits(uint32_t cars, double radius) {
  ci::Limits l;
  l.cars = cars;
  l.radius = radius;
  l.instances = 640;
  return l;
}

void TestSelector() {
  {
    ci::CarSelector s;
    s.Begin(Limits(2, 10.0));
    Check(s.OnDraw(1, 4.0).keep && s.OnDraw(1, 4.2).keep, "a car within the radius is kept, every draw of it");
    Check(!s.OnDraw(2, 12.0).keep && !s.Kept(2), "a car beyond the radius is not");
    Check(s.OnDraw(3, 9.0).keep && s.kept() == 2, "the second place goes to the next car within the radius");
    ci::CarSelector::Verdict v = s.OnDraw(4, 9.5);
    Check(!v.keep && v.evicted == 0, "a car that is no nearer than the farthest kept one does not take its place");
    v = s.OnDraw(5, 2.0);
    Check(v.keep && v.evicted == 3 && s.Kept(5) && !s.Kept(3) && s.Kept(1) && s.kept() == 2, "a nearer car takes the farthest kept car's place and says which it gave up");
    Check(!s.OnDraw(3, 9.0).keep, "the car that was given up stays out");
    // A car that comes nearer later is kept from that draw on.
    Check(!s.OnDraw(6, 11.0).keep && Near(s.Best(6), 11.0), "a car beyond the radius ...");
    Check(!s.OnDraw(6, 9.0).keep, "... then within but no nearer than the farthest kept one (the first car's nearest is 4)");
    ci::CarSelector t;
    t.Begin(Limits(2, 10.0));
    Check(!t.OnDraw(7, 12.0).keep && t.OnDraw(7, 8.0).keep, "a car whose later draw is within the radius is kept from there on (its earlier draws are lost)");
    Check(Near(t.Best(7), 8.0), "its best distance is the nearest of its draws");
  }
  {
    ci::CarSelector s;
    s.Begin(Limits(4, 14.0));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Check(!s.OnDraw(1, nan).keep && !s.OnDraw(1, std::numeric_limits<double>::infinity()).keep && !s.OnDraw(1, -1.0).keep, "a distance that is no number, infinite or negative keeps nothing");
    Check(!s.OnDraw(0, 1.0).keep && !s.OnDraw(24, 1.0).keep && !s.OnDraw(99, 1.0).keep, "ID 0 and IDs past the table are no cars");
    Check(s.kept() == 0 && std::isinf(s.Best(1)) && std::isinf(s.Best(99)), "nothing was kept");
    s.Begin(ci::Limits{});
    Check(!s.OnDraw(1, 1.0).keep, "with the other cars off nothing is kept");
  }
  {
    // Begin forgets the last frame.
    ci::CarSelector s;
    s.Begin(Limits(1, 10.0));
    s.OnDraw(3, 1.0);
    s.Begin(Limits(1, 10.0));
    Check(s.kept() == 0 && !s.Kept(3) && std::isinf(s.Best(3)), "a new frame starts empty");
  }
  {
    // Whatever order the cars' first draws come in, the kept ones are the nearest N within the radius.
    const double distances[] = {3.5, 12.0, 6.0, 9.0, 4.5, 8.0, 15.0, 1.5, 9.9};
    const uint32_t n = uint32_t(sizeof(distances) / sizeof(distances[0]));
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    bool all_right = true;
    uint32_t shuffles = 0;
    std::mt19937 random(12345);
    for (; shuffles < 3000; ++shuffles) {
      std::shuffle(order.begin(), order.end(), random);
      ci::CarSelector s;
      s.Begin(Limits(3, 10.0));
      for (const uint32_t i : order) {
        s.OnDraw(i + 1, distances[i]);
      }
      std::set<uint32_t> kept;
      for (uint32_t id = 1; id <= n; ++id) {
        if (s.Kept(id)) {
          kept.insert(id);
        }
      }
      // The nearest three within 10 m: 1.5 (ID 8), 3.5 (ID 1), 4.5 (ID 5).
      all_right = all_right && kept == std::set<uint32_t>{1, 5, 8} && s.kept() == 3;
    }
    Check(all_right, "in any order of arrival the nearest N within the radius are the cars kept");
  }
  {
    // Fewer cars within the radius than the limit: all of them.
    ci::CarSelector s;
    s.Begin(Limits(4, 10.0));
    s.OnDraw(1, 3.0);
    s.OnDraw(2, 20.0);
    Check(s.kept() == 1 && s.Kept(1) && !s.Kept(2), "fewer cars than places: only those within the radius");
  }
}

// ---------------------------------------------------------------------------
// a car's centre, the rows
// ---------------------------------------------------------------------------

void TestCentre() {
  ci::OtherCar car;
  car.stencil_id = 3;
  double c[3];
  Check(!car.Centre(c), "a car with no draw has no centre");
  car.Add(Translation(10.0, 0.0, 4.0), 6.0);
  car.Add(Translation(12.0, 0.0, 6.0), 5.0);
  car.Add(Translation(11.0, 1.0, 5.0), 7.0);
  Check(car.draws == 3 && Near(car.distance, 5.0), "the nearest draw's distance is the car's");
  Check(car.Centre(c) && Near(c[0], 11.0) && Near(c[1], 1.0 / 3.0 + ci::kCentreRise) && Near(c[2], 5.0), "the centre is the mean of the origins, raised");
  ci::OtherCar bad;
  bad.Add(Translation(std::numeric_limits<double>::quiet_NaN(), 0, 0), 1.0);
  Check(!bad.Centre(c), "a centre that is no number is none");

  float v[3];
  const double p[3] = {1.0, 2.0, -5.0};
  Check(ci::ViewPoint(tp::Identity(), false, p, v) && Near(v[2], -5.0), "the view point as it is");
  Check(ci::ViewPoint(tp::Identity(), true, p, v) && Near(v[0], 1.0) && Near(v[1], 2.0) && Near(v[2], 5.0), "with the z of the game's own view flipped");
  Check(ci::ViewPoint(Translation(0, 0, 20), true, p, v) && Near(v[2], -15.0), "a view matrix moves it first, the flip comes after");
  const double nan3[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
  Check(!ci::ViewPoint(tp::Identity(), false, nan3, v), "a point that is no number is none");
  static_assert(sizeof(ci::CarRow) == 16 && sizeof(rr::InstanceRow) == 16);
}

// ---------------------------------------------------------------------------
// the budgets
// ---------------------------------------------------------------------------

void TestBudgets() {
  Check(ci::MayFingerprint(0, 1u << 30, 1000), "the first fingerprint of a frame is always made, however large");
  Check(ci::MayFingerprint(900, 100, 1000) && !ci::MayFingerprint(900, 101, 1000), "within the budget, not beyond it");
  Check(ci::MayFingerprint(5000, 5000, 0), "no budget: no limit (the player's car)");
  Check(ci::MayStage(0, 99999, 100) && ci::MayStage(60, 40, 100) && !ci::MayStage(60, 41, 100), "the read budget, with the first read always allowed");
  const uint64_t cap = rr::kMemoryCapBytes;
  Check(ci::OthersMayAdd(0, 0, 10ull << 20), "an empty scene has room for another car");
  Check(ci::OthersMayAdd(30ull << 20, 5ull << 20, 10ull << 20) && !ci::OthersMayAdd(30ull << 20, 5ull << 20, 16ull << 20),
        "the other cars stop short of the cap by the player's reserve");
  Check(ci::OthersMayAdd(cap - ci::kPlayerReserveBytes, 0, 0) && !ci::OthersMayAdd(cap - ci::kPlayerReserveBytes, 0, 1), "exactly at the limit");
  Check(!ci::OthersMayAdd(0, 0, 1, ci::kPlayerReserveBytes), "a cap no larger than the reserve leaves the other cars nothing");
  Check(ci::EstimateMeshBytes(0, 96) == 0 && ci::EstimateMeshBytes(3000, 96) == 1000ull * (36 + 12 + 96) && ci::EstimateMeshBytes(6000, 96) > ci::EstimateMeshBytes(3000, 96),
        "the estimate follows the triangles");
}

// ---------------------------------------------------------------------------
// the instance list
// ---------------------------------------------------------------------------

ci::OtherDraw Draw(uint32_t id, uint64_t key, double x, double y, double z, bool after_world = false) {
  ci::OtherDraw d;
  d.stencil_id = id;
  d.mesh_key = key;
  d.after_world = after_world;
  d.model_to_world = Translation(x, y, z);
  d.indices = 300;
  return d;
}

ci::OtherCar MakeCar(uint32_t id, double distance, uint32_t draws, double x = 0.0, double z = 0.0) {
  ci::OtherCar c;
  c.stencil_id = id;
  c.distance = distance;
  c.draws = draws;
  c.sum[0] = x * draws;
  c.sum[2] = z * draws;
  return c;
}

void TestPlan() {
  std::vector<ci::OtherCar> cars = {MakeCar(2, 9.0, 100), MakeCar(3, 4.0, 150), MakeCar(4, 6.0, 80), MakeCar(5, 1.0, 0)};
  uint32_t cut = 0;
  std::vector<ci::OtherCar> none;
  Check(ci::PlanCars(none, 500, &cut).empty() && cut == 0, "no cars, no plan");
  std::vector<uint32_t> plan = ci::PlanCars(cars, 500, &cut);
  Check(plan == std::vector<uint32_t>({1, 2, 0}) && cut == 0, "nearest first (the car with no draw is not listed)");
  plan = ci::PlanCars(cars, 240, &cut);
  Check(plan == std::vector<uint32_t>({1, 2}) && cut == 1, "the farthest car goes when the draws do not fit");
  plan = ci::PlanCars(cars, 200, &cut);
  Check(plan == std::vector<uint32_t>({1}) && cut == 2, "a car that does not fit is cut whole, and one nearer that does fit stays");
  cars[0].draws = 40;  // the farthest is small: it fits after a nearer one was cut
  plan = ci::PlanCars(cars, 200, &cut);
  Check(plan == std::vector<uint32_t>({1, 0}) && cut == 1, "a small car farther away still fits where a larger one did not");
  plan = ci::PlanCars(cars, 0, &cut);
  Check(plan.empty() && cut == 3, "no room, no cars");
  // The count of cars (the class's, with the cars placed from memory among the drawn ones): the nearest N of all, by distance.
  cars[0].draws = 100;
  plan = ci::PlanCars(cars, 500, &cut, 2);
  Check(plan == std::vector<uint32_t>({1, 2}) && cut == 1, "a count of two keeps the nearest two");
  plan = ci::PlanCars(cars, 500, &cut, 0);
  Check(plan.size() == 3 && cut == 0, "a count of 0 is no count");
  plan = ci::PlanCars(cars, 500, &cut, 9);
  Check(plan.size() == 3 && cut == 0, "a count above the cars there are changes nothing");
  // A remembered car (an ID above the game's) counts like any other and keeps its place by distance.
  cars.push_back(MakeCar(ci::CarSelector::kSlots + 1, 2.0, 60));
  cars.back().remembered = true;
  plan = ci::PlanCars(cars, 500, &cut, 2);
  Check(plan == std::vector<uint32_t>({4, 1}) && cut == 2, "a remembered car nearer than the drawn ones takes a place of the count");
}

void TestAppend() {
  // The camera: the game's view, looking down -z, at the origin (flipped into z forward).
  const tp::Mat4 view = tp::Identity();
  std::map<uint64_t, ci::MeshRef> meshes;
  meshes[100] = ci::MeshRef{0, 0};
  meshes[101] = ci::MeshRef{300, 120};
  meshes[102] = ci::MeshRef{900, 400};
  const auto find = [&](uint64_t key, ci::MeshRef* ref) {
    const auto it = meshes.find(key);
    if (it == meshes.end()) {
      return false;
    }
    *ref = it->second;
    return true;
  };

  ci::List list;
  // The player's two instances are in the list already.
  for (int i = 0; i < 2; ++i) {
    ci::Item item;
    item.mesh_key = 100;
    item.mask = rr::kMaskBody;
    list.items.push_back(item);
    list.rows.push_back(rr::MakeRow(0, 0, rr::kMaskBody, false, uint32_t(i)));
  }
  list.stats.player_instances = 2;

  std::vector<ci::OtherCar> cars = {MakeCar(2, 8.0, 3, 3.0, -9.0), MakeCar(3, 5.0, 2, -3.0, -6.0)};
  // Car 2 (the farther) draws first; car 3's glass comes after the world; one draw names a mesh with no structure yet.
  std::vector<ci::OtherDraw> draws = {Draw(2, 100, 3.0, 0.0, -9.0), Draw(2, 101, 3.0, 0.0, -9.0), Draw(3, 100, -3.0, 0.0, -6.0),
                                      Draw(2, 555, 3.0, 0.0, -9.0), Draw(3, 102, -3.0, 0.0, -6.0, true)};
  ci::AppendOthers(&list, draws, cars, view, /*flip_z=*/true, 640, rr::kMaxInstances, find);
  // The rows: the player's two, the four instances, TWO rows for each car (its record, then its paint), then two rows (minimum, maximum) for the box of each car that has an instance.
  Check(list.items.size() == 2 + 4 && list.rows.size() == 2 + 4 + 2 * ci::kCarRows + 2 * 2,
        "the player's two, four instances of the other cars (one had no structure), two rows for each car, two rows for each car's box");
  Check(list.box_row == 2 + 4 + 2 * ci::kCarRows && list.boxes.size() == 2, "the boxes' rows start after the cars' rows, one box a car");
  Check(list.stats.other_instances == 4 && list.stats.cars == 2 && list.stats.missing_mesh == 1 && list.stats.bad_matrix == 0 && list.stats.cars_cut == 0,
        "the numbers of the list");
  // The instances keep the draws' order; every one has the others' mask.
  bool masks = true;
  for (size_t i = 2; i < list.items.size(); ++i) {
    masks = masks && list.items[i].mask == rr::kMaskOthers && (list.rows[i].flags & 0xFFu) == rr::kMaskOthers;
  }
  Check(masks, "every instance of another car has the others' mask, in the item and in the row");
  Check(list.items[2].mesh_key == 100 && list.items[3].mesh_key == 101 && list.items[4].mesh_key == 100 && list.items[5].mesh_key == 102, "in the draws' order");
  Check(list.rows[3].first_index == 300 && list.rows[3].first_vertex == 120 && list.rows[5].first_index == 900 && list.rows[5].first_vertex == 400,
        "the rows say where each mesh is in the shared buffers");
  Check((list.rows[5].flags & rr::kFlagAfterWorld) != 0 && (list.rows[2].flags & rr::kFlagAfterWorld) == 0, "the after-world flag follows the draw");
  // The cars' records follow the instances, two rows each: the nearest car (ID 3) is rows 6 and 7, the farther (ID 2) rows 8 and 9.
  Check(list.rows[2].draw == 8 && list.rows[3].draw == 8 && list.rows[4].draw == 6 && list.rows[5].draw == 6, "an instance names its car's first row");
  Check(list.rows[0].draw == 0 && list.rows[1].draw == 1, "the player's rows are as they were");
  ci::CarRow row3, row2;
  std::memcpy(&row3, &list.rows[6], sizeof(row3));
  std::memcpy(&row2, &list.rows[8], sizeof(row2));
  Check(row3.radius > 0.0f && row2.radius > 0.0f, "the cars' rows hold their cars (not the boxes')");
  // Car 3's origin at (-3, 0, -6) raised by kCentreRise; with the flipped view its z is +6.
  Check(Near(row3.centre[0], -3.0) && Near(row3.centre[1], ci::kCentreRise) && Near(row3.centre[2], 6.0) && Near(row3.radius, ci::kCarRadius), "a car's row: the centre in view space and the radius");
  Check(Near(row2.centre[0], 3.0) && Near(row2.centre[2], 9.0), "the other car's row");
  // the second row of each record is the paint: none here (no car had a colour)
  ci::CarPaintRow paint3;
  std::memcpy(&paint3, &list.rows[7], sizeof(paint3));
  Check(paint3.flags == 0 && list.stats.painted_cars == 0, "a car with no paint colour has a paint row with the known flag clear");
  // The instance's transform is the draw's model through the view, flipped.
  float expected[3][4];
  Check(rr::PackInstanceTransform(Translation(-3.0, 0.0, -6.0), view, true, expected), "the reference transform");
  Check(std::memcmp(list.items[4].transform, expected, sizeof(expected)) == 0 && Near(list.items[4].transform[2][3], 6.0), "an instance's transform is the player's instance rule's");

  // Sharing: mesh 100 is named by the player's two and by two other draws.
  ci::CountSharing(&list);
  Check(list.stats.distinct_meshes == 3 && list.stats.shared_meshes == 1, "three distinct meshes, one of them shared by several instances");
}

void TestAppendLimits() {
  const tp::Mat4 view = tp::Identity();
  const auto find = [](uint64_t, ci::MeshRef* ref) {
    *ref = ci::MeshRef{};
    return true;
  };
  // Three cars of 3 draws each; a budget of 7 takes two whole cars.
  std::vector<ci::OtherCar> cars = {MakeCar(2, 3.0, 3), MakeCar(3, 5.0, 3), MakeCar(4, 7.0, 3)};
  std::vector<ci::OtherDraw> draws;
  for (uint32_t id = 2; id <= 4; ++id) {
    for (int k = 0; k < 3; ++k) {
      draws.push_back(Draw(id, 100 + k, 0.0, 0.0, -double(id)));
    }
  }
  ci::List list;
  ci::AppendOthers(&list, draws, cars, view, true, 7, rr::kMaxInstances, find);
  Check(list.items.size() == 6 && list.stats.cars == 2 && list.stats.cars_cut == 1, "the instance budget takes whole cars, nearest first");
  bool ids_ok = true;
  for (const ci::Item& item : list.items) {
    ids_ok = ids_ok && (std::fabs(item.transform[2][3] - 2.0f) < 1e-4f || std::fabs(item.transform[2][3] - 3.0f) < 1e-4f);  // cars 2 and 3 (flipped z), not car 4's
  }
  Check(ids_ok, "the cars that were taken are the nearer two"); 
  // The top level's own cap counts the player's instances.
  ci::List full;
  for (uint32_t i = 0; i < rr::kMaxInstances - 4; ++i) {
    full.items.push_back(ci::Item{});
    full.rows.push_back(rr::InstanceRow{});
  }
  ci::AppendOthers(&full, draws, cars, view, true, 640, rr::kMaxInstances, find);
  Check(full.items.size() <= rr::kMaxInstances && full.stats.other_instances <= 4, "the top level's cap holds, the player's instances counted");
  // A matrix that cannot be an instance is counted and left out.
  ci::List bad;
  std::vector<ci::OtherDraw> bad_draws = {Draw(2, 100, 0, 0, -3)};
  bad_draws[0].model_to_world.m[0][0] = bad_draws[0].model_to_world.m[1][1] = bad_draws[0].model_to_world.m[2][2] = 0.0;  // no volume
  std::vector<ci::OtherCar> one = {MakeCar(2, 3.0, 1)};
  ci::AppendOthers(&bad, bad_draws, one, view, true, 640, rr::kMaxInstances, find);
  Check(bad.items.empty() && bad.stats.bad_matrix == 1 && bad.stats.cars == 1 && bad.rows.size() == ci::kCarRows, "a matrix with no volume is no instance; the car keeps its row");
  // No car, no draw: nothing appended.
  ci::List nothing;
  ci::AppendOthers(&nothing, draws, {}, view, true, 640, rr::kMaxInstances, find);
  Check(nothing.items.empty() && nothing.rows.empty(), "no cars, nothing appended");
  // A car with no centre gets a row with no radius, which the shader reads as "no colour".
  ci::List nocentre;
  std::vector<ci::OtherCar> weird = {MakeCar(2, 3.0, 1)};
  weird[0].sum[0] = std::numeric_limits<double>::quiet_NaN();
  ci::AppendOthers(&nocentre, {Draw(2, 100, 0, 0, -3)}, weird, view, true, 640, rr::kMaxInstances, find);
  ci::CarRow row;
  std::memcpy(&row, &nocentre.rows[nocentre.items.size()], sizeof(row));  // the car's row follows the instance's (the box's rows follow that)
  Check(nocentre.items.size() == 1 && row.radius == 0.0f, "a centre that is no number: radius 0");
}

// ---------------------------------------------------------------------------
// the cars' boxes
// ---------------------------------------------------------------------------

tp::Mat4 RotationY(double angle) {
  tp::Mat4 m = tp::Identity();
  m.m[0][0] = std::cos(angle);
  m.m[0][2] = -std::sin(angle);
  m.m[2][0] = std::sin(angle);
  m.m[2][2] = std::cos(angle);
  return m;
}

void TestBounds() {
  // The box of a mesh's positions.
  std::vector<float> positions = {-1.0f, 0.0f, -2.2f, 1.0f, 0.5f, 2.0f, 0.0f, 1.3f, 0.3f};
  float lo[3] = {9, 9, 9}, hi[3] = {-9, -9, -9};
  Check(ci::BoundsOf(positions, lo, hi) && lo[0] == -1.0f && lo[1] == 0.0f && lo[2] == -2.2f && hi[0] == 1.0f && hi[1] == 1.3f && hi[2] == 2.0f,
        "the box of a mesh's positions");
  float lo2[3] = {7, 7, 7}, hi2[3] = {7, 7, 7};
  Check(!ci::BoundsOf({}, lo2, hi2) && lo2[0] == 7.0f, "no vertices: no box, nothing written");
  std::vector<float> bad = {0.0f, 0.0f, 0.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
  Check(!ci::BoundsOf(bad, lo2, hi2) && lo2[0] == 7.0f, "a coordinate that is not finite: no box");
  std::vector<float> one = {3.0f, 4.0f, 5.0f};
  Check(ci::BoundsOf(one, lo2, hi2) && lo2[1] == 4.0f && hi2[2] == 5.0f, "one vertex is a box of a point");

  // A view box grows with the points added.
  ci::ViewBox box;
  Check(!box.valid, "an empty box is not valid");
  const float a[3] = {1.0f, 2.0f, 3.0f}, b[3] = {-1.0f, 5.0f, 2.0f};
  box.Add(a);
  Check(box.valid && box.min[0] == 1.0f && box.max[2] == 3.0f, "the first point makes the box");
  box.Add(b);
  Check(box.min[0] == -1.0f && box.max[0] == 1.0f && box.min[1] == 2.0f && box.max[1] == 5.0f && box.min[2] == 2.0f && box.max[2] == 3.0f, "the next grows it");

  // An instance of a mesh with a box: its eight corners through the transform.
  ci::MeshRef ref;
  ref.has_bounds = true;
  ref.bounds_min[0] = -1.0f; ref.bounds_min[1] = 0.0f; ref.bounds_min[2] = -2.2f;
  ref.bounds_max[0] = 1.0f;  ref.bounds_max[1] = 1.3f; ref.bounds_max[2] = 2.2f;
  float transform[3][4];
  Check(rr::PackInstanceTransform(Translation(3.0, 0.0, -9.0), tp::Identity(), true, transform), "the reference transform");
  ci::ViewBox placed;
  ci::GrowBox(&placed, transform, ref);
  // flip_z: the world z -9 is the view's z +9, and the mesh's z runs the other way: [-2.2, 2.2] about 9.
  Check(Near(placed.min[0], 2.0) && Near(placed.max[0], 4.0) && Near(placed.min[1], 0.0) && Near(placed.max[1], 1.3) && Near(placed.min[2], 6.8) &&
            Near(placed.max[2], 11.2),
        "an instance's box is the mesh's box placed in view space");
  // A turn of a quarter about the vertical axis swaps the car's length and width.
  float turned[3][4];
  Check(rr::PackInstanceTransform(Multiply(RotationY(1.5707963267948966), Translation(3.0, 0.0, -9.0)), tp::Identity(), true, turned), "the turned transform");
  ci::ViewBox quarter;
  ci::GrowBox(&quarter, turned, ref);
  Check(Near(quarter.max[0] - quarter.min[0], 4.4, 1e-4) && Near(quarter.max[2] - quarter.min[2], 2.0, 1e-4), "turned a quarter, the box is as wide as the car is long");
  Check(Near((quarter.min[0] + quarter.max[0]) * 0.5, 3.0, 1e-4) && Near((quarter.min[2] + quarter.max[2]) * 0.5, 9.0, 1e-4), "... about the same centre");
  // Several instances (a body and a wheel apart from it): the union.
  ci::ViewBox car;
  ci::GrowBox(&car, transform, ref);
  float wheel_transform[3][4];
  Check(rr::PackInstanceTransform(Translation(4.5, 0.0, -9.5), tp::Identity(), true, wheel_transform), "the wheel's transform");
  ci::GrowBox(&car, wheel_transform, ref);
  Check(car.max[0] > placed.max[0] && car.min[0] == placed.min[0] && car.max[2] > placed.max[2] - 1e-6f, "the car's box is the union of its instances' boxes");
  // A mesh whose box is not known puts its origin in the box.
  ci::MeshRef unknown;
  ci::ViewBox origin;
  ci::GrowBox(&origin, transform, unknown);
  Check(origin.valid && Near(origin.min[0], 3.0) && origin.min[0] == origin.max[0] && Near(origin.min[2], 9.0), "no mesh box: the instance's origin");
}

void TestCarBoxes() {
  const tp::Mat4 view = tp::Identity();
  std::map<uint64_t, ci::MeshRef> meshes;
  ci::MeshRef body;
  body.first_index = 0;
  body.first_vertex = 0;
  body.has_bounds = true;
  body.bounds_min[0] = -0.9f; body.bounds_min[1] = 0.0f; body.bounds_min[2] = -2.2f;
  body.bounds_max[0] = 0.9f;  body.bounds_max[1] = 1.3f; body.bounds_max[2] = 2.2f;
  meshes[100] = body;
  ci::MeshRef wheel = body;
  wheel.first_index = 300;
  wheel.bounds_min[0] = -0.15f; wheel.bounds_min[1] = -0.3f; wheel.bounds_min[2] = -0.3f;
  wheel.bounds_max[0] = 0.15f;  wheel.bounds_max[1] = 0.3f;  wheel.bounds_max[2] = 0.3f;
  meshes[101] = wheel;
  const auto find = [&](uint64_t key, ci::MeshRef* ref) {
    const auto it = meshes.find(key);
    if (it == meshes.end()) {
      return false;
    }
    *ref = it->second;
    return true;
  };

  // Two cars: ID 3 nearer, 4 m to the left; ID 2 farther, 14 m ahead to the right; a body and a wheel each. A third kept car has no draw that lists.
  std::vector<ci::OtherCar> cars = {MakeCar(2, 14.0, 2, 3.0, -14.0), MakeCar(3, 4.0, 2, -4.0, -6.0), MakeCar(5, 8.0, 1, 0.0, -3.0)};
  std::vector<ci::OtherDraw> draws = {Draw(2, 100, 3.0, 0.0, -14.0), Draw(3, 100, -4.0, 0.0, -6.0), Draw(2, 101, 4.0, 0.0, -14.0),
                                      Draw(3, 101, -4.0, 0.0, -5.0), Draw(5, 999, 0.0, 0.0, -3.0)};  // 999 has no structure
  ci::List list;
  ci::AppendOthers(&list, draws, cars, view, /*flip_z=*/true, 640, rr::kMaxInstances, find);
  Check(list.stats.cars == 3 && list.items.size() == 4 && list.stats.missing_mesh == 1, "three kept cars, four instances, one draw with no structure");
  Check(list.boxes.size() == 2, "a box for each car that has an instance (the third has none)");
  // The nearest car first, as the cars' rows: ID 3 (4 m) then ID 2 (14 m) then ID 5 (no box).
  const ci::ViewBox& near_box = list.boxes[0];
  const ci::ViewBox& far_box = list.boxes[1];
  Check(near_box.valid && far_box.valid, "both boxes are valid");
  Check(near_box.min[0] < -4.5 && near_box.max[0] > -3.2 && Near(near_box.min[2], 3.8) && near_box.max[2] > 8.1,
        "the nearer car's box holds its body (about x -4.9 to -3.1, z 3.8 to 8.2) and its wheel");
  Check(far_box.min[2] > 11.0 && far_box.max[2] < 16.5 && far_box.min[0] > 1.5, "the farther car's box is 14 m out, to the right");
  Check(near_box.max[0] < far_box.min[0], "the two boxes are apart: a box round both would hold the road between them");
  // The rows: [items 0..3][cars' rows 4..6][boxes' rows 7..10]; each box two rows, min then max, as float bits.
  Check(list.box_row == 4 + 3 * ci::kCarRows && list.rows.size() == 4 + 3 * ci::kCarRows + 4, "the boxes' rows follow the three cars' records");
  bool rows_match = true;
  for (size_t k = 0; k < list.boxes.size(); ++k) {
    float lo[4], hi[4];
    std::memcpy(lo, &list.rows[list.box_row + 2 * k], sizeof(lo));
    std::memcpy(hi, &list.rows[list.box_row + 2 * k + 1], sizeof(hi));
    for (int c = 0; c < 3; ++c) {
      rows_match = rows_match && lo[c] == list.boxes[k].min[c] && hi[c] == list.boxes[k].max[c];
    }
    uint32_t pad_lo, pad_hi;
    std::memcpy(&pad_lo, &list.rows[list.box_row + 2 * k].draw, 4);
    std::memcpy(&pad_hi, &list.rows[list.box_row + 2 * k + 1].draw, 4);
    rows_match = rows_match && pad_lo == 0 && pad_hi == 0;
  }
  Check(rows_match, "the rows hold each box's minimum and maximum as the shader reads them");
  // An instance still names its car's row (the boxes' rows came after them).
  Check(list.rows[0].draw >= 4 && list.rows[0].draw <= 8 && list.rows[0].draw % 2 == 0, "an instance names its car's first row");
  Check(list.rows[2].draw >= 4 && list.rows[2].draw <= 8 && list.rows[2].draw % 2 == 0, "... which is before the boxes' rows");

  // The player's own box (the check's scope): grown instance by instance as CarRtInstances::Build does, tight to the car's parts, empty after Clear.
  Check(!list.player_box.valid, "no player box until an instance of the player's car is grown into it");
  float to_view[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 8}};  // the car 8 m ahead of the camera
  ci::GrowBox(&list.player_box, to_view, body);
  ci::GrowBox(&list.player_box, to_view, wheel);
  Check(list.player_box.valid && Near(list.player_box.min[0], -0.9) && Near(list.player_box.max[0], 0.9) && Near(list.player_box.min[2], 5.8) &&
            Near(list.player_box.max[2], 10.2) && Near(list.player_box.max[1], 1.3) && Near(list.player_box.min[1], -0.3),
        "the player's box holds the body and the wheel (x -0.9 to 0.9, y -0.3 to 1.3, z 5.8 to 10.2)");

  // Clear forgets the boxes too.
  list.Clear();
  Check(list.boxes.empty() && list.box_row == 0 && list.rows.empty(), "Clear empties the boxes and their row");
  Check(!list.player_box.valid, "Clear empties the player's box");
}

}  // namespace

// A walked car's record carries its paint in the second row, and walked / painted cars are counted.
void TestPaintRows() {
  const tp::Mat4 view = tp::Identity();
  const auto find = [&](uint64_t key, ci::MeshRef* ref) {
    *ref = ci::MeshRef{uint32_t(key) * 10u, uint32_t(key) * 4u};
    return key != 0;
  };
  ci::List list;
  ci::OtherCar a = MakeCar(ci::CarSelector::kSlots + 8, 3.0, 2, 2.0, -5.0);  // a walked car with its paint
  a.remembered = true;
  a.walked = true;
  a.has_paint = true;
  a.paint[0] = 0.25f;
  a.paint[1] = 0.5f;
  a.paint[2] = 0.75f;
  ci::OtherCar b = MakeCar(ci::CarSelector::kSlots + 0, 6.0, 1, -2.0, -8.0);  // a remembered car with none
  b.remembered = true;
  std::vector<ci::OtherCar> cars = {a, b};
  std::vector<ci::OtherDraw> draws = {Draw(a.stencil_id, 7, 2.0, 0.0, -5.0), Draw(a.stencil_id, 8, 2.0, 0.0, -5.0), Draw(b.stencil_id, 9, -2.0, 0.0, -8.0)};
  ci::AppendOthers(&list, draws, cars, view, true, 640, rr::kMaxInstances, find);
  Check(list.items.size() == 3 && list.stats.cars == 2 && list.stats.walked_cars == 1 && list.stats.walked_instances == 2 && list.stats.painted_cars == 1,
        "two cars, one walked (two instances) and one painted");
  // the records: car a (nearer) rows 3 and 4, car b rows 5 and 6, the boxes after
  Check(list.rows[0].draw == 3 && list.rows[1].draw == 3 && list.rows[2].draw == 5, "the instances name the first row of their car's record");
  ci::CarRow ra, rb;
  ci::CarPaintRow pa, pb;
  std::memcpy(&ra, &list.rows[3], sizeof(ra));
  std::memcpy(&pa, &list.rows[4], sizeof(pa));
  std::memcpy(&rb, &list.rows[5], sizeof(rb));
  std::memcpy(&pb, &list.rows[6], sizeof(pb));
  Check(ra.radius < 0.0f && rb.radius < 0.0f, "both cars are remembered: no history colour (a negative radius)");
  Check((pa.flags & ci::kPaintKnown) != 0 && Near(pa.rgb[0], 0.25) && Near(pa.rgb[1], 0.5) && Near(pa.rgb[2], 0.75), "the walked car's paint is in its second row");
  Check(pb.flags == 0 && pb.rgb[0] == 0.0f, "a car with no paint: the flag is clear");
  Check(list.box_row == 3 + 2 * ci::kCarRows, "the boxes' rows follow the two records");
  // the log of the records (RT-MODEL-CARS-4): the first row of the records and what each holds, in the order of the rows, for the probe to compare with the shader's reads
  Check(list.car_row == 3 && list.records.size() == 2 && list.records[0].slot == 0 && list.records[1].slot == 1, "the records' first row and a log entry a car, in the rows' order");
  Check(list.records[0].has_paint && Near(list.records[0].paint[0], 0.25) && Near(list.records[0].paint[2], 0.75) && list.records[0].walked && list.records[0].remembered &&
            list.records[0].radius == ra.radius && list.records[0].stencil_id == a.stencil_id,
        "the log holds the paint, the radius and the flags written for the nearer car");
  Check(!list.records[1].has_paint && list.records[1].paint[0] == 0.0f && !list.records[1].walked && list.records[1].radius == rb.radius, "a car with no paint: none logged, zeros");
  Check(list.car_row + list.stats.cars * ci::kCarRows == list.box_row, "the records end where the boxes begin");
  list.Clear();
  Check(list.records.empty() && list.car_row == 0, "Clear empties the log of the records");
}

int main() {
  TestLimits();
  TestSelector();
  TestCentre();
  TestBudgets();
  TestPlan();
  TestAppend();
  TestAppendLimits();
  TestBounds();
  TestCarBoxes();
  TestPaintRows();
  std::printf("car_rt_instances_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
