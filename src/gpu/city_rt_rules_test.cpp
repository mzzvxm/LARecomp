// The city ray tracing's streaming rules (city_rt_rules.h): the setting, the class budgets, the mask against the car scene's, the instance id, the distance to a
// sector's box, the streaming plan on a synthetic city (rings, hysteresis, the lod before the hi detail, the memory cap and what may be traded for room, a camera
// driving through for ten minutes) and the frame's build schedule.
//
//   build: cmake --build <dir> --target larecomp_city_rt_rules_test, then run src/gpu/larecomp_city_rt_rules_test (exit 0 = passed).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "car_rt_scene_rules.h"
#include "city_rt_rules.h"

using namespace mcla::gpu;
using namespace mcla::gpu::city_rt;
using mcla::gpu::gpu_class::GpuClass;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

// A grid of sectors, each `size` metres square, `n` x `n`, hi detail of `hi` triangles and lod of `lod`; the grid is centred on the origin.
std::vector<SectorInfo> Grid(int n, float size, uint32_t hi, uint32_t lod) {
  std::vector<SectorInfo> sectors;
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      SectorInfo s;
      s.box_min[0] = (float(x) - float(n) / 2) * size;
      s.box_max[0] = s.box_min[0] + size;
      s.box_min[2] = (float(z) - float(n) / 2) * size;
      s.box_max[2] = s.box_min[2] + size;
      s.box_min[1] = -10;
      s.box_max[1] = 60;
      s.hi_triangles = hi;
      s.lod_triangles = lod;
      sectors.push_back(s);
    }
  }
  return sectors;
}

// A stand-in for the scene: applies a plan at once (a read takes `latency` plans to arrive) and keeps the states.
struct Sim {
  std::vector<SectorInfo> sectors;
  std::vector<SectorState> states;
  Budget budget;
  uint32_t loading = 0;
  struct Pending { Request request; int left; };
  std::vector<Pending> pending;
  int latency = 0;
  uint64_t frame = 0;
  uint32_t loads_total = 0, evictions_total = 0;

  Sim(std::vector<SectorInfo> s, Budget b, int latency_frames = 0) : sectors(std::move(s)), states(sectors.size()), budget(b), latency(latency_frames) {}

  uint64_t BytesOf(const SectorInfo& s, Detail d) const { return EstimateStructureBytes(d == Detail::kHi ? s.hi_triangles : s.lod_triangles); }

  Plan Step(double x, double z) {
    ++frame;
    // reads that arrived
    for (auto it = pending.begin(); it != pending.end();) {
      if (--it->left <= 0) {
        SectorState& st = states[it->request.sector];
        (it->request.detail == Detail::kHi ? st.hi : st.lod) = Residency::kResident;
        --loading;
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
    PlanInput in;
    in.camera[0] = x;
    in.camera[1] = 5;
    in.camera[2] = z;
    in.frame = frame;
    in.budget = budget;
    in.loading = loading;
    Plan plan = PlanStreaming(in, sectors, states);
    for (const Request& e : plan.evictions) {
      SectorState& st = states[e.sector];
      if (e.detail == Detail::kHi) { st.hi = Residency::kNone; st.hi_bytes = 0; } else { st.lod = Residency::kNone; st.lod_bytes = 0; }
      ++evictions_total;
    }
    for (const Request& r : plan.loads) {
      SectorState& st = states[r.sector];
      (r.detail == Detail::kHi ? st.hi : st.lod) = Residency::kLoading;
      (r.detail == Detail::kHi ? st.hi_bytes : st.lod_bytes) = BytesOf(sectors[r.sector], r.detail);
      pending.push_back({r, latency});
      ++loading;
      ++loads_total;
    }
    return plan;
  }

  uint64_t Resident() const {
    uint64_t total = 0;
    for (const SectorState& s : states) {
      if (s.hi != Residency::kNone && s.hi != Residency::kFailed) total += s.hi_bytes;
      if (s.lod != Residency::kNone && s.lod != Residency::kFailed) total += s.lod_bytes;
    }
    return total;
  }
  uint32_t Count(bool hi, Residency r) const {
    uint32_t n = 0;
    for (const SectorState& s : states) n += (hi ? s.hi : s.lod) == r ? 1u : 0u;
    return n;
  }
};

void TestSettingAndBudgets() {
  CHECK(ParseChoice("off") == Choice::kOff && ParseChoice("high") == Choice::kHigh && ParseChoice("ultra") == Choice::kUltra);
  CHECK(ParseChoice("auto") == Choice::kAuto && ParseChoice("") == Choice::kAuto && ParseChoice("low") == Choice::kAuto && ParseChoice("HIGH") == Choice::kAuto);
  // auto is the class table's cell: High only
  CHECK(LevelFor(GpuClass::kLow, Choice::kAuto) == Level::kOff);
  CHECK(LevelFor(GpuClass::kMid, Choice::kAuto) == Level::kOff);
  CHECK(LevelFor(GpuClass::kHigh, Choice::kAuto) == Level::kHigh);
  // by hand: anywhere but Low
  CHECK(LevelFor(GpuClass::kMid, Choice::kHigh) == Level::kHigh && LevelFor(GpuClass::kMid, Choice::kUltra) == Level::kUltra);
  CHECK(LevelFor(GpuClass::kHigh, Choice::kUltra) == Level::kUltra && LevelFor(GpuClass::kHigh, Choice::kOff) == Level::kOff);
  CHECK(LevelFor(GpuClass::kLow, Choice::kHigh) == Level::kOff && LevelFor(GpuClass::kLow, Choice::kUltra) == Level::kOff);
  CHECK(LevelName(Level::kOff) == "off" && LevelName(Level::kHigh) == "high" && LevelName(Level::kUltra) == "ultra");

  CHECK(!BudgetFor(GpuClass::kHigh, Level::kOff).on() && !BudgetFor(GpuClass::kLow, Level::kUltra).on());
  const Budget mid = BudgetFor(GpuClass::kMid, Level::kHigh);
  const Budget high = BudgetFor(GpuClass::kHigh, Level::kHigh);
  const Budget ultra = BudgetFor(GpuClass::kHigh, Level::kUltra);
  CHECK(mid.on() && high.on() && ultra.on());
  CHECK(mid.hi_radius == 150.0f && high.hi_radius == 300.0f && ultra.hi_radius == 600.0f);  // the survey's rings
  CHECK(mid.memory_cap < high.memory_cap && high.memory_cap < ultra.memory_cap);
  CHECK(mid.build_triangles < high.build_triangles && high.build_triangles < ultra.build_triangles);
  CHECK(mid.far_radius > mid.hi_radius * 4 && high.far_radius > high.hi_radius * 4 && ultra.far_radius > ultra.hi_radius * 4);
  // the caps hold the survey's worst case (the ring's p90 triangles + the whole far lod) with room: Mid 0.5 M, High 1.5 M, Ultra 2.9 M hi triangles + 0.383 M lod
  CHECK(EstimateStructureBytes(500000 + 383000) < mid.memory_cap);
  CHECK(EstimateStructureBytes(1500000 + 383000) < high.memory_cap);
  CHECK(EstimateStructureBytes(2900000 + 383000) < ultra.memory_cap);
  CHECK(EstimateStructureBytes(50000) == 50000 * 96ull);
  // a Mid card asked for ultra by hand gets the High class's ring with a smaller cap
  const Budget mid_ultra = BudgetFor(GpuClass::kMid, Level::kUltra);
  CHECK(mid_ultra.hi_radius == 300.0f && mid_ultra.memory_cap < high.memory_cap);
  // the debug radius
  const Budget r = WithRadius(high, 100.0f);
  CHECK(r.hi_radius == 100.0f && r.far_radius >= 400.0f && r.far_radius >= high.far_radius && r.hysteresis >= 15.0f);
  CHECK(WithRadius(high, 0.0f).hi_radius == 300.0f && WithRadius(high, -5.0f).hi_radius == 300.0f);
  CHECK(WithRadius(high, 5.0f).hi_radius == 25.0f && WithRadius(high, 99999.0f).hi_radius == 1500.0f);
  CHECK(!WithRadius(Budget{}, 100.0f).on());  // off stays off
}

void TestMaskAndId() {
  using namespace car_rt_rules;
  constexpr uint32_t kCarMasks = kMaskBody | kMaskGlass | kMaskTyres | kMaskDriver | kMaskOthers | kMaskOtherChrome | kMaskOtherPaint;
  static_assert((kMaskCity & kCarMasks) == 0, "the city's mask bit is not one the car scene uses");
  static_assert((kMaskCity & (kMaskCity - 1)) == 0 && kMaskCity <= 0xFF, "one bit of the instance mask's eight");
  CHECK((kMaskCity | kCarMasks) == 0xFFu);  // every bit of the eight is spoken for
  for (const uint32_t sector : {0u, 1u, 438u, 100000u}) {
    for (const Detail d : {Detail::kHi, Detail::kLod}) {
      const uint32_t id = InstanceId(sector, d);
      CHECK(IsCityInstance(id) && SectorOf(id) == sector && DetailOf(id) == d && id < (1u << 24));
    }
  }
  CHECK(!IsCityInstance(0) && !IsCityInstance(1023));  // a car's table row is a small number
  CHECK(InstanceId(5, Detail::kHi) != InstanceId(5, Detail::kLod));
}

void TestDistance() {
  const float lo[3] = {0, 0, 0}, hi[3] = {10, 10, 10};
  const double inside[3] = {5, 5, 5};
  CHECK(DistanceToBox(inside, lo, hi) == 0.0f);
  const double on_edge[3] = {10, 5, 5};
  CHECK(DistanceToBox(on_edge, lo, hi) == 0.0f);
  const double face[3] = {15, 5, 5};
  CHECK(std::fabs(DistanceToBox(face, lo, hi) - 5.0f) < 1e-5f);
  const double edge[3] = {13, 14, 5};
  CHECK(std::fabs(DistanceToBox(edge, lo, hi) - 5.0f) < 1e-5f);
  const double corner[3] = {-3, -4, 10};
  CHECK(std::fabs(DistanceToBox(corner, lo, hi) - 5.0f) < 1e-5f);
  const double below[3] = {5, -7, 5};
  CHECK(std::fabs(DistanceToBox(below, lo, hi) - 7.0f) < 1e-5f);
}

void TestPlanBasics() {
  Budget b = BudgetFor(GpuClass::kHigh, Level::kHigh);
  // 400 sectors of 200 m: the ring of 300 m (hi) and 2000 m (far) round the origin
  Sim sim(Grid(20, 200.0f, 50000, 1000), b);
  // the first plan: only reads, the lod first, the nearest first, as many as the budget has slots
  const Plan first = sim.Step(0, 0);
  CHECK(first.evictions.empty());
  CHECK(first.loads.size() == b.loads_in_flight);
  CHECK(!first.loads.empty() && first.loads[0].detail == Detail::kLod);
  for (size_t i = 1; i < first.loads.size(); ++i) CHECK(first.loads[i - 1].distance <= first.loads[i].distance);
  CHECK(first.wanted_hi > 0 && first.wanted_lod > first.wanted_hi);

  // run it to a standstill
  for (int i = 0; i < 2000; ++i) sim.Step(0, 0);
  const Plan quiet = sim.Step(0, 0);
  CHECK(quiet.loads.empty() && quiet.evictions.empty());
  // every sector within the ring is resident at hi detail and within the far ring at lod; nothing outside
  PlanInput in;
  in.camera[0] = 0; in.camera[1] = 5; in.camera[2] = 0;
  uint32_t hi_in = 0, lod_in = 0;
  for (uint32_t i = 0; i < sim.sectors.size(); ++i) {
    const float d = DistanceToBox(in.camera, sim.sectors[i].box_min, sim.sectors[i].box_max);
    const bool want_hi = d <= b.hi_radius, want_lod = d <= b.far_radius;
    CHECK((sim.states[i].hi == Residency::kResident) == want_hi);
    CHECK((sim.states[i].lod == Residency::kResident) == want_lod);
    hi_in += want_hi ? 1u : 0u;
    lod_in += want_lod ? 1u : 0u;
  }
  CHECK(hi_in > 0 && lod_in > hi_in);
  CHECK(sim.Resident() <= b.memory_cap);
}

void TestHysteresis() {
  Budget b = BudgetFor(GpuClass::kHigh, Level::kHigh);
  Sim sim(Grid(20, 200.0f, 50000, 1000), b);
  for (int i = 0; i < 2000; ++i) sim.Step(0, 0);
  const uint32_t built = sim.Count(true, Residency::kResident);
  // the camera goes 50 m east: the sectors that just left the ring (within hi_radius + hysteresis) stay, the new ones come
  uint32_t evicted_before = sim.evictions_total;
  for (int i = 0; i < 300; ++i) sim.Step(50, 0);
  CHECK(sim.Count(true, Residency::kResident) >= built);  // more come than go
  // a camera that jitters across a ring's edge by a few metres neither builds nor gives back
  const uint32_t loads_before = sim.loads_total;
  evicted_before = sim.evictions_total;
  for (int i = 0; i < 600; ++i) sim.Step(50 + (i % 2 ? 4.0 : -4.0), 0);
  CHECK(sim.loads_total - loads_before <= 4);  // the first steps may still be arriving
  for (int i = 0; i < 600; ++i) sim.Step(50 + (i % 2 ? 4.0 : -4.0), 0);
  const uint32_t loads_mid = sim.loads_total;
  const uint32_t evictions_mid = sim.evictions_total;
  for (int i = 0; i < 600; ++i) sim.Step(50 + (i % 2 ? 4.0 : -4.0), 0);
  CHECK(sim.loads_total == loads_mid && sim.evictions_total == evictions_mid);  // steady: no flapping at all
  (void)evicted_before;

  // an unbuilt sector just beyond the radius is not asked for; one just inside is
  Sim edge(Grid(1, 200.0f, 50000, 0), b);  // one sector, x from -100 to 100
  PlanInput in;
  in.budget = b;
  in.frame = 1;
  in.camera[1] = 5;
  in.camera[0] = 100 + b.hi_radius + 1.0;  // 301 m from the box: out
  CHECK(PlanStreaming(in, edge.sectors, edge.states).loads.empty());
  in.camera[0] = 100 + b.hi_radius - 1.0;  // in
  CHECK(PlanStreaming(in, edge.sectors, edge.states).loads.size() == 1);
  // a built one stays until radius + hysteresis
  edge.states[0].hi = Residency::kResident;
  edge.states[0].hi_bytes = 1;
  in.camera[0] = 100 + b.hi_radius + b.hysteresis - 1.0;
  CHECK(PlanStreaming(in, edge.sectors, edge.states).evictions.empty());
  in.camera[0] = 100 + b.hi_radius + b.hysteresis + 1.0;
  const Plan gone = PlanStreaming(in, edge.sectors, edge.states);
  CHECK(gone.evictions.size() == 1 && gone.evictions[0].sector == 0 && gone.evictions[0].detail == Detail::kHi);
}

void TestPlanRules() {
  Budget b = BudgetFor(GpuClass::kHigh, Level::kHigh);
  PlanInput in;
  in.budget = b;
  in.frame = 100;
  in.camera[1] = 5;

  // a sector with no hi mesh is never asked for at hi detail, nor one with no lod at lod
  std::vector<SectorInfo> sectors = Grid(3, 200.0f, 50000, 1000);
  sectors[4].hi_triangles = 0;  // the middle one
  sectors[0].lod_triangles = 0;
  std::vector<SectorState> states(sectors.size());
  in.budget.loads_in_flight = 100;
  const Plan p = PlanStreaming(in, sectors, states);
  for (const Request& r : p.loads) {
    CHECK(!(r.sector == 4 && r.detail == Detail::kHi));
    CHECK(!(r.sector == 0 && r.detail == Detail::kLod));
  }
  // the lod reads all come before the hi reads
  bool seen_hi = false;
  for (const Request& r : p.loads) {
    if (r.detail == Detail::kHi) seen_hi = true;
    CHECK(!(seen_hi && r.detail == Detail::kLod));
  }

  // reads in flight count against the slots, and a loading sector is not asked for again
  in.budget.loads_in_flight = 2;
  std::vector<SectorState> loading_states(sectors.size());
  loading_states[3].lod = Residency::kLoading;
  loading_states[3].lod_bytes = 1;
  in.loading = 1;
  const Plan q = PlanStreaming(in, sectors, loading_states);
  CHECK(q.loads.size() == 1);
  for (const Request& r : q.loads) CHECK(!(r.sector == 3 && r.detail == Detail::kLod));
  in.loading = 2;
  CHECK(PlanStreaming(in, sectors, loading_states).loads.empty());
  in.loading = 0;

  // a failed read is not asked for again before its retry frame
  std::vector<SectorState> failed(sectors.size());
  for (SectorState& s : failed) {
    s.lod = Residency::kFailed;
    s.lod_retry_frame = 150;
    s.hi = Residency::kFailed;
    s.hi_retry_frame = 150;
  }
  in.budget.loads_in_flight = 100;
  in.frame = 120;
  CHECK(PlanStreaming(in, sectors, failed).loads.empty());
  in.frame = 150;
  CHECK(!PlanStreaming(in, sectors, failed).loads.empty());

  // off: everything built goes back, nothing is asked for
  std::vector<SectorState> built(sectors.size());
  built[2].hi = Residency::kResident;
  built[2].lod = Residency::kResident;
  built[5].lod = Residency::kResident;
  in.budget = Budget{};
  const Plan off = PlanStreaming(in, sectors, built);
  CHECK(off.loads.empty() && off.evictions.size() == 3);

  // the pick: hi once built, else lod, never both
  SectorState st;
  Detail d = Detail::kHi;
  CHECK(!PickInstance(st, &d));
  st.lod = Residency::kResident;
  CHECK(PickInstance(st, &d) && d == Detail::kLod);
  st.hi = Residency::kLoading;
  CHECK(PickInstance(st, &d) && d == Detail::kLod);  // the hi detail is on its way: the lod stays in the top level
  st.hi = Residency::kResident;
  CHECK(PickInstance(st, &d) && d == Detail::kHi);
  st.lod = Residency::kNone;
  CHECK(PickInstance(st, &d) && d == Detail::kHi);
}

void TestMemoryCap() {
  // sectors of 100,000 hi triangles (9.6 MB) and a cap of 40 MB: four fit
  Budget b = BudgetFor(GpuClass::kHigh, Level::kHigh);
  b.memory_cap = 40 * kMiB;
  b.loads_in_flight = 8;
  b.hi_radius = 1000;  // the whole row is wanted
  b.far_radius = 2000;
  std::vector<SectorInfo> sectors = Grid(1, 200.0f, 100000, 0);
  sectors.clear();
  for (int i = 0; i < 10; ++i) {
    SectorInfo s;
    s.box_min[0] = float(i) * 200.0f;
    s.box_max[0] = s.box_min[0] + 200.0f;
    s.box_min[1] = 0; s.box_max[1] = 10; s.box_min[2] = 0; s.box_max[2] = 200;
    s.hi_triangles = 100000;
    sectors.push_back(s);
  }
  PlanInput in;
  in.budget = b;
  in.frame = 1;
  in.camera[0] = 0;
  in.camera[1] = 5;
  in.camera[2] = 100;
  std::vector<SectorState> states(sectors.size());
  const Plan p = PlanStreaming(in, sectors, states);
  CHECK(p.loads.size() == 4);  // 4 x 9.6 MB < 40 MB < 5 x 9.6 MB
  CHECK(p.over_cap);
  for (size_t i = 0; i < p.loads.size(); ++i) CHECK(p.loads[i].sector == i);  // the nearest four

  // the camera moves to the far end: the four far sectors are now the nearest; built nearer-to-the-old-camera ones are traded for them, farthest first, never a nearer one
  for (size_t i = 0; i < 4; ++i) {
    states[i].hi = Residency::kResident;
    states[i].hi_bytes = EstimateStructureBytes(100000);
  }
  in.camera[0] = 2000;
  in.budget.hi_radius = 5000;  // everything wanted
  in.budget.far_radius = 6000;
  const Plan moved = PlanStreaming(in, sectors, states);
  CHECK(!moved.loads.empty());
  CHECK(moved.loads[0].sector == 9);  // the nearest to the new camera first
  CHECK(!moved.evictions.empty());
  for (const Request& e : moved.evictions) {
    CHECK(e.sector < 4);
    for (const Request& l : moved.loads) CHECK(e.distance > l.distance);  // never a nearer sector traded for a farther one
  }
  // the evictions go farthest first
  for (size_t i = 1; i < moved.evictions.size(); ++i) CHECK(moved.evictions[i - 1].distance >= moved.evictions[i].distance);
  // the lod is never traded: a sector whose lod is built keeps it while its hi detail is evicted
  std::vector<SectorState> with_lod = states;
  for (size_t i = 0; i < 4; ++i) {
    with_lod[i].lod = Residency::kResident;
    with_lod[i].lod_bytes = 1;
  }
  std::vector<SectorInfo> with_lod_sectors = sectors;
  for (size_t i = 0; i < 4; ++i) with_lod_sectors[i].lod_triangles = 10;
  const Plan keep = PlanStreaming(in, with_lod_sectors, with_lod);
  for (const Request& e : keep.evictions) CHECK(e.detail == Detail::kHi);
}

void TestDrive() {
  // a camera driving 36 m/s through a 6 km city for ten minutes, 60 frames a second, a read taking 6 frames: the memory never goes over the cap, the work per second
  // stays small (a sector in and a sector out every few seconds, as the survey estimates), and the ring is built round the camera when it is steady.
  for (const GpuClass cls : {GpuClass::kMid, GpuClass::kHigh}) {
    const Budget b = BudgetFor(cls, cls == GpuClass::kMid ? Level::kHigh : Level::kUltra);
    Sim sim(Grid(30, 200.0f, 50000, 1000), b, 6);
    uint64_t worst = 0;
    double x = -2500, z = -2500;
    double direction = 1.0;  // there and back along the diagonal (no jump: a teleport reads the whole ring at once, which no drive does)
    uint32_t loads_second = 0, worst_loads_second = 0;
    for (int frame = 0; frame < 600 * 60; ++frame) {
      x += direction * 36.0 / 60.0 * 0.8;  // along a diagonal
      z += direction * 36.0 / 60.0 * 0.6;
      if (x > 2500) direction = -1.0;
      if (x < -2500) direction = 1.0;
      const uint32_t before = sim.loads_total;
      sim.Step(x, z);
      loads_second += sim.loads_total - before;
      if (frame % 60 == 59) {
        worst_loads_second = std::max(worst_loads_second, loads_second);
        loads_second = 0;
      }
      worst = std::max(worst, sim.Resident());
    }
    CHECK(worst <= b.memory_cap);
    // a second of driving needs at most a handful of reads (the ring's circumference over 200 m sectors at 36 m/s, lod and hi)
    std::printf("  drive %s: worst %u reads in a second, resident at most %.1f MiB of %.1f\n", std::string(gpu_class::ClassName(cls)).c_str(), worst_loads_second, double(worst) / double(kMiB), double(b.memory_cap) / double(kMiB));
    // the sustained rate is what the ring's sweep needs (a few reads a second); a burst is the reads in flight at their limit, never more
    const double per_second = double(sim.loads_total) / 600.0;
    std::printf("  drive %s: %.1f reads and %.1f evictions a second on average\n", std::string(gpu_class::ClassName(cls)).c_str(), per_second, double(sim.evictions_total) / 600.0);
    CHECK(per_second <= 8.0);
    CHECK(worst_loads_second <= b.loads_in_flight * 60 / 6 + 1);  // a read takes 6 frames in this model
    // parked, the ring fills and everything wanted is built
    for (int i = 0; i < 3000; ++i) sim.Step(x, z);
    const Plan idle = sim.Step(x, z);
    CHECK(idle.loads.empty() && idle.evictions.empty());
    CHECK(sim.Count(true, Residency::kResident) >= 1 && sim.Count(false, Residency::kResident) > sim.Count(true, Residency::kResident));
  }
}

void TestBuildPlan() {
  const std::vector<BuildItem> items = {{50000, 4 << 20, 300.0f}, {50000, 4 << 20, 100.0f}, {50000, 4 << 20, 200.0f}, {10000, 1 << 20, 400.0f}};
  // a budget of 110,000: the nearest two (100 m, 200 m) fit, the third (300 m) would make 150,000; the small one far behind still fits (110,000 exactly... 50+50+10)
  std::vector<uint8_t> built = PlanBuilds(items, 110000, 100ull << 20);
  CHECK(built.size() == 4 && built[1] == 1 && built[2] == 1 && built[0] == 0 && built[3] == 1);
  // the bytes cap counts too
  built = PlanBuilds(items, 1000000, 9ull << 20);
  CHECK(built[1] == 1 && built[2] == 1 && built[0] == 0 && built[3] == 1);  // 8 MiB used, the 300 m one (4 more) would be 12; the small one at 400 m (1 more) makes 9, which fits
  // the nearest one is built even over both budgets: a big sector must not starve
  const std::vector<BuildItem> big = {{262000, 20 << 20, 50.0f}, {1000, 1 << 10, 60.0f}};
  built = PlanBuilds(big, 60000, 6 << 20);
  CHECK(built[0] == 1 && built[1] == 0);
  CHECK(PlanBuilds({}, 1, 1).empty());
  // equal distances keep the input's order
  const std::vector<BuildItem> same = {{100, 1, 10.0f}, {100, 1, 10.0f}, {100, 1, 10.0f}};
  built = PlanBuilds(same, 200, 100);
  CHECK(built[0] == 1 && built[1] == 1 && built[2] == 0);
}

}  // namespace

int main() {
  TestSettingAndBudgets();
  TestMaskAndId();
  TestDistance();
  TestPlanBasics();
  TestHysteresis();
  TestPlanRules();
  TestMemoryCap();
  TestDrive();
  TestBuildPlan();
  if (g_failures != 0) {
    std::fprintf(stderr, "city_rt_rules_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("city_rt_rules_test: %d checks passed\n", g_checks);
  return 0;
}
