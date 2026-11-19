#pragma once
// ===========================================================================
// MCLA GPU renderer -- the other cars in the car ray tracing scene: the rules that need no renderer
// ===========================================================================
// The ray traced car effects (car_rt_scene.h) started with the player's car alone. The reflection of the paint and the chrome should also
// show the car driving alongside, so the top level structure holds the OTHER cars' draws too: opponents, traffic, parked cars. This header holds
// what needs no device, as pure functions on plain numbers, so that car_rt_instances_rules_test.cpp builds on its own:
//
//   - the limits by graphics class (how many cars, how far from the player, how many instances, how much work a frame may spend on a car that is
//     new) and the two debug overrides (Limits, LimitsFor);
//   - which cars are kept: the nearest few within a radius of the player's vehicle, decided draw by draw as the frame comes in (CarSelector);
//   - a car's body centre, the mean of the origins of its kept draws, which the shader samples the car's colour at (OtherCar, CarRow);
//   - the budgets of a car that is new: guest bytes fingerprinted, triangles read, video memory (MayFingerprint, MayStage, OthersMayAdd);
//   - the per-frame instance list: the nearest whole cars that fit the instance budget, each draw placed in the frame's view space, the rows of
//     the table the shaders read, the cars' rows after them (AppendOthers).
//
// WHICH CARS (CarSelector): the distance that ranks a car is its distance from the PLAYER'S VEHICLE (the guest's own position of it, which the car
// frame reads once a frame), not from the camera: every effect that traces against this scene starts its rays on the player's car (its reflection,
// its ambient occlusion, its shadows), so a car matters by how near it is to that car, and a car that is near the camera but far from the player
// (the traffic ahead of a chase camera's car) cannot be seen in it. The ray's own reach (the reflection's is 4 m from the panel) and the car's
// size (a car is 4.5 m long) give the radius: a car whose centre is more than 10 to 14 m from the player's cannot touch a reflection ray.
//
// Matrices are row vectors (v' = v M) as in temporal_math.h. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "car_dump_rules.h"
#include "car_rt_scene_rules.h"
#include "gpu_class.h"
#include "temporal_math.h"

namespace mcla::gpu::car_rt_instances {

namespace rr = car_rt_rules;

// ---------------------------------------------------------------------------
// the limits
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaxCars = 8;           // the most any setting may take
inline constexpr double kMaxRadius = 60.0;        // metres: the most a debug override may ask
// The car's own size, for the shader's sample of its colour (hlsl/car_rt_refl_cs.hlsl OtherCarColour): about half a car's length.
inline constexpr float kCarRadius = 1.6f;
// A car's body centre is the mean of its parts' origins, put this far up (the world's y) so that it is inside the body, not under it.
inline constexpr double kCentreRise = 0.6;

struct Limits {
  uint32_t cars = 0;             // the most other cars in the scene (0: none, nothing of this is collected)
  double radius = 0.0;           // metres from the player's vehicle: a car farther than this is not kept
  uint32_t instances = 0;        // the most instances of other cars in the top level (the player's are counted separately)
  uint32_t min_indices = 0;      // a draw with fewer indices (or vertices) than this is left out: the car's smallest parts show in no reflection
  uint32_t stage_triangles = 0;  // triangles of other cars' new meshes read from guest memory in one frame (the player's have a budget of their own)
  uint32_t content_bytes = 0;    // guest bytes fingerprinted for other cars' new mesh keys in one frame (the first mesh of a frame is always made)
  bool operator==(const Limits&) const = default;
  bool On() const { return cars != 0 && radius > 0.0; }
};

// The numbers by class. A car at full detail is about 195 rigid draws and 90,000 triangles (perf/CAR-RT-SPIKE.md), which is 8 to 10 MiB of
// structures and buffers and about 1 ms of the render thread's time on the frame it appears (the fingerprints of its meshes and the read
// of their triangles): the weaker the class the fewer cars and the slower a new car is taken in (a few frames, the car appears in parts).
//   Low   none: a graphics card of that class has no ray tracing worth the name (the self-reflection's own cell for it is `off`).
//   Mid   2 cars within 10 m, no part under 15 triangles, 30,000 triangles and 384 KiB of fingerprints a frame for a new car (3 to 4 frames).
//   High  4 cars within 14 m, every part, 60,000 triangles and 768 KiB a frame (about 2 frames).
inline Limits ClassLimits(gpu_class::GpuClass cls) {
  Limits limits;
  switch (cls) {
    case gpu_class::GpuClass::kHigh:
      limits.cars = 4;
      limits.radius = 14.0;
      limits.instances = 640;
      limits.min_indices = 0;
      limits.stage_triangles = 60000;
      limits.content_bytes = 768u * 1024u;
      break;
    case gpu_class::GpuClass::kMid:
      limits.cars = 2;
      limits.radius = 10.0;
      limits.instances = 320;
      limits.min_indices = 45;
      limits.stage_triangles = 30000;
      limits.content_bytes = 384u * 1024u;
      break;
    case gpu_class::GpuClass::kLow:
      break;
  }
  return limits;
}

// The limits with the two debug overrides applied: `cars_override` -1 keeps the class's, 0 turns the other cars off, N asks for N cars (at most kMaxCars;
// the instance budget grows to 160 a car, up to the top level's own cap, so that the cars asked for fit). Asked for by hand on a class that has none
// (the Low class), the High cell's numbers stand for the rest. `radius_override` 0 or less keeps the class's.
inline Limits LimitsFor(gpu_class::GpuClass cls, int cars_override, double radius_override) {
  Limits limits = ClassLimits(cls);
  if (cars_override >= 0) {
    if (limits.cars == 0 && cars_override > 0) {
      limits = ClassLimits(gpu_class::GpuClass::kHigh);
    }
    limits.cars = uint32_t(std::min<int>(cars_override, int(kMaxCars)));
    limits.instances = std::max<uint32_t>(limits.instances, limits.cars * 160u);
  }
  if (radius_override > 0.0) {
    limits.radius = std::min(radius_override, kMaxRadius);
  }
  limits.instances = std::min<uint32_t>(limits.instances, rr::kMaxInstances);
  return limits;
}

// ---------------------------------------------------------------------------
// which cars are kept
// ---------------------------------------------------------------------------
// The draws of a frame come in the game's order and a car's draws are in blocks (not always: its glass and lights come after the world). A car is
// known by its stencil ID, which the game hands out per frame in draw order. For every draw of an ID that is not the player's the selector gets the
// draw's distance from the player's vehicle (the draw's gWorld translation against the vehicle's position: about the car's root) and says whether
// the car is kept: a car is ranked by the NEAREST of its draws seen so far; it is kept while it is within the radius and there are fewer than
// `cars` kept, or it is nearer than the farthest kept car, which is then given up (its draws are dropped by the caller). At the end of the frame
// the kept cars are the nearest N of those within the radius, except that a car's draws that came before it was kept are lost for the frame:
// only a car whose first draws are farther than a nearer car's is affected, which is rare (a car's root draws come first and are at the root).
class CarSelector {
 public:
  static constexpr uint32_t kSlots = car_dump::kIdSlots;

  struct Verdict {
    bool keep = false;       // this draw's car is kept: its draw is to be listed
    uint32_t evicted = 0;    // the stencil ID of the car this draw's car took the place of (0: none); its listed draws are to be dropped
  };

  void Begin(const Limits& limits) {
    limits_ = limits;
    for (Entry& e : entries_) {
      e = Entry{};
    }
    kept_ = 0;
  }

  Verdict OnDraw(uint32_t id, double distance) {
    Verdict v;
    if (id == 0 || id >= kSlots || !limits_.On() || !std::isfinite(distance) || distance < 0.0) {
      return v;
    }
    Entry& e = entries_[id];
    e.best = std::min(e.best, distance);
    if (e.kept) {
      v.keep = true;
      return v;
    }
    if (!(e.best <= limits_.radius)) {
      return v;
    }
    if (kept_ < limits_.cars) {
      e.kept = true;
      ++kept_;
      v.keep = true;
      return v;
    }
    uint32_t farthest = 0;
    double farthest_best = -1.0;
    for (uint32_t other = 1; other < kSlots; ++other) {
      if (entries_[other].kept && entries_[other].best > farthest_best) {
        farthest = other;
        farthest_best = entries_[other].best;
      }
    }
    if (farthest != 0 && e.best < farthest_best) {
      entries_[farthest].kept = false;
      e.kept = true;
      v.keep = true;
      v.evicted = farthest;
    }
    return v;
  }

  bool Kept(uint32_t id) const { return id < kSlots && entries_[id].kept; }
  uint32_t kept() const { return kept_; }
  // The nearest distance seen of the car (infinity: none).
  double Best(uint32_t id) const { return id < kSlots ? entries_[id].best : std::numeric_limits<double>::infinity(); }
  const Limits& limits() const { return limits_; }

 private:
  struct Entry {
    double best = std::numeric_limits<double>::infinity();
    bool kept = false;
  };
  Limits limits_;
  std::array<Entry, kSlots> entries_{};
  uint32_t kept_ = 0;
};

// ---------------------------------------------------------------------------
// a car's draws and its body centre
// ---------------------------------------------------------------------------

// One draw of another car, as the car frame lists it: no host geometry (only the player's car is drawn again by the car shadow map), nothing
// skinned (the other cars' drivers are left out as the player's is). `mesh_key` is never 0 in a list (car_frame.h Draw::mesh_key: the same key
// = the same triangles in model space, so every instance of a model shares one bottom level structure).
struct OtherDraw {
  uint32_t stencil_id = 0;
  uint32_t order = 0;           // its place among the frame's main-view draws
  bool after_world = false;     // drawn after the frame's first world draw (glass and the translucent parts, often)
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint64_t mesh_key = 0;
  uint32_t indices = 0;         // the draw's index (or vertex) count
  temporal::Mat4 model_to_world;
};

// One kept car: its ID, its distance and the running sum of the origins of its draws.
struct OtherCar {
  uint32_t stencil_id = 0;
  double distance = std::numeric_limits<double>::infinity();  // metres from the player's vehicle, its nearest draw
  double sum[3] = {};
  uint32_t draws = 0;

  void Add(const temporal::Mat4& model_to_world, double draw_distance) {
    for (int c = 0; c < 3; ++c) {
      sum[c] += model_to_world.m[3][c];
    }
    ++draws;
    distance = std::min(distance, draw_distance);
  }
  // The mean of the origins of the car's draws, raised to be inside the body. False: no draw.
  bool Centre(double out[3]) const {
    if (draws == 0) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      out[c] = sum[c] / double(draws);
    }
    out[1] += kCentreRise;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
  }
};

// The row the shader reads for a car (hlsl/car_rt_common.hlsli CarRtCar): the body centre in the frame's view space and the car's radius. It sits in
// the same table as the instances' rows (the table's rows are uint4), after them; an instance of another car names it with `draw`.
struct CarRow {
  float centre[3] = {};
  float radius = 0.0f;
};
static_assert(sizeof(CarRow) == sizeof(rr::InstanceRow), "a car's row is one uint4, as an instance's");

// A world-space point in the frame's view space (the AO's: z forward): `view` is the row-vector world-to-view matrix, `flip_z` the game's own
// view's negated z (car_rt_scene_rules.h PackInstanceTransform does the same to an instance).
inline bool ViewPoint(const temporal::Mat4& view, bool flip_z, const double p[3], float out[3]) {
  double v[3];
  for (int c = 0; c < 3; ++c) {
    v[c] = p[0] * view.m[0][c] + p[1] * view.m[1][c] + p[2] * view.m[2][c] + view.m[3][c];
  }
  if (flip_z) {
    v[2] = -v[2];
  }
  if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = float(v[c]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// the budgets of a car that is new
// ---------------------------------------------------------------------------

// Whether a fingerprint of `bytes` of guest memory may be made now for another car's draw: `spent` is what this frame has spent on them. The
// first one of a frame is always made, so that a mesh larger than the budget is not left out for ever. 0 for `budget`: no limit (the player's car).
inline bool MayFingerprint(uint64_t spent, uint64_t bytes, uint64_t budget) {
  return budget == 0 || spent == 0 || spent + bytes <= budget;
}

// Whether another car's mesh of about `triangles` may be read from guest memory now (`staged`: what this frame has read for other cars); the
// first of a frame always may.
inline bool MayStage(uint64_t staged, uint64_t triangles, uint64_t budget) {
  return staged == 0 || staged + triangles <= budget;
}

// The video memory other cars may take: a new mesh of theirs is read only while the resident bytes, the ones staged and its own estimate stay
// under the cap minus a reserve that the player's own car (8 to 10 MiB, 14 at the most) always finds.
inline constexpr uint64_t kPlayerReserveBytes = 14ull << 20;
inline bool OthersMayAdd(uint64_t resident_bytes, uint64_t staged_bytes, uint64_t estimate_bytes, uint64_t cap_bytes = rr::kMemoryCapBytes) {
  const uint64_t limit = cap_bytes > kPlayerReserveBytes ? cap_bytes - kPlayerReserveBytes : 0;
  return resident_bytes + staged_bytes + estimate_bytes <= limit;
}

// What a mesh of `index_count` indices is guessed to hold, before it is read: its triangles (a list's third of the indices), a vertex a triangle
// and a half in the shared buffers (24 bytes: position and normal), the indices (4), and the structure per triangle.
inline uint64_t EstimateMeshBytes(uint32_t index_count, uint64_t structure_bytes_per_triangle) {
  const uint64_t triangles = index_count / 3;
  return triangles * (36 + 12 + structure_bytes_per_triangle);
}

// ---------------------------------------------------------------------------
// the instance list
// ---------------------------------------------------------------------------

// The mesh a draw names, as the scene knows it when its structure is built: where its indices and vertices are in the shared buffers.
struct MeshRef {
  uint32_t first_index = 0;
  uint32_t first_vertex = 0;
};

// One instance of the top level: the mesh to instance, its transform in the frame's view space and its mask (its row of the table is List::rows[i]).
struct Item {
  uint64_t mesh_key = 0;
  float transform[3][4] = {};
  uint32_t mask = 0;
};

struct ListStats {
  uint32_t player_instances = 0;
  uint32_t other_instances = 0;
  uint32_t cars = 0;             // other cars in the list
  uint32_t cars_cut = 0;         // kept cars left out: their draws did not fit the instance budget
  uint32_t missing_mesh = 0;     // draws left out: their mesh has no structure yet
  uint32_t bad_matrix = 0;
  uint32_t distinct_meshes = 0;  // distinct mesh keys over all instances (one structure each, shared by every instance of it)
  uint32_t shared_meshes = 0;    // ... of those named by more than one instance
};

// The frame's instances and the rows of the table. rows[i] belongs to items[i]; after the items come the other cars' rows (CarRow), which an
// instance of another car names by its `draw` field.
struct List {
  std::vector<Item> items;
  std::vector<rr::InstanceRow> rows;
  ListStats stats;
  void Clear() {
    items.clear();
    rows.clear();
    stats = ListStats{};
  }
};

// The kept cars, nearest first, as whole cars while their draws fit `budget` instances; the rest are cut. Returns the ones kept as indices into
// `cars`, in the order they are to be listed.
inline std::vector<uint32_t> PlanCars(const std::vector<OtherCar>& cars, uint32_t budget, uint32_t* cut = nullptr) {
  std::vector<uint32_t> order;
  order.reserve(cars.size());
  for (uint32_t i = 0; i < cars.size(); ++i) {
    if (cars[i].draws != 0) {
      order.push_back(i);
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cars[a].distance < cars[b].distance; });
  std::vector<uint32_t> kept;
  uint32_t used = 0, dropped = 0;
  for (const uint32_t i : order) {
    if (used + cars[i].draws <= budget) {
      kept.push_back(i);
      used += cars[i].draws;
    } else {
      ++dropped;
    }
  }
  if (cut) {
    *cut = dropped;
  }
  return kept;
}

// Appends the other cars to `list` (which already holds the player's instances): the nearest whole cars within `budget_instances`, each of their
// draws whose mesh `find(mesh_key, &ref)` knows, in the draws' order, placed in the frame's view space with the mask kMaskOthers; then a row for
// every car listed. `max_total` is the top level's own cap on instances (the player's and these). The instance of a car's draw keeps the car's row in
// its `draw` field.
template <typename Find>
inline void AppendOthers(List* list, const std::vector<OtherDraw>& draws, const std::vector<OtherCar>& cars, const temporal::Mat4& view,
                         bool flip_z, uint32_t budget_instances, uint32_t max_total, Find&& find) {
  const uint32_t have = uint32_t(list->items.size());
  const uint32_t room = max_total > have ? max_total - have : 0;
  uint32_t cut = 0;
  const std::vector<uint32_t> plan = PlanCars(cars, std::min(budget_instances, room), &cut);
  list->stats.cars_cut += cut;
  if (plan.empty()) {
    return;
  }
  // The slot of a car in the list: its place in the plan; the row of the car comes after all the items.
  std::array<int32_t, CarSelector::kSlots> slot_of;
  slot_of.fill(-1);
  for (uint32_t k = 0; k < plan.size(); ++k) {
    slot_of[cars[plan[k]].stencil_id % CarSelector::kSlots] = int32_t(k);
  }
  const size_t first_other = list->items.size();
  for (const OtherDraw& draw : draws) {
    const int32_t slot = draw.stencil_id < CarSelector::kSlots ? slot_of[draw.stencil_id] : -1;
    if (slot < 0 || list->items.size() >= max_total) {
      continue;
    }
    MeshRef ref;
    if (draw.mesh_key == 0 || !find(draw.mesh_key, &ref)) {
      ++list->stats.missing_mesh;
      continue;
    }
    Item item;
    item.mesh_key = draw.mesh_key;
    item.mask = rr::kMaskOthers;
    if (!rr::PackInstanceTransform(draw.model_to_world, view, flip_z, item.transform)) {
      ++list->stats.bad_matrix;
      continue;
    }
    list->items.push_back(item);
    list->rows.push_back(rr::MakeRow(ref.first_index, ref.first_vertex, rr::kMaskOthers, draw.after_world, uint32_t(slot)));
    ++list->stats.other_instances;
  }
  // The cars' rows follow the instances' (the table's rows are all uint4); the instances name them by row.
  const uint32_t car_rows = uint32_t(list->items.size());
  for (size_t i = first_other; i < list->rows.size(); ++i) {
    list->rows[i].draw += car_rows;
  }
  for (const uint32_t index : plan) {
    CarRow row;
    row.radius = kCarRadius;
    double centre[3];
    if (!cars[index].Centre(centre) || !ViewPoint(view, flip_z, centre, row.centre)) {
      row.radius = 0.0f;  // no centre: the shader finds no colour for this car (its hidden hits are misses)
    }
    rr::InstanceRow raw;
    static_assert(sizeof(raw) == sizeof(row));
    std::memcpy(&raw, &row, sizeof(raw));
    list->rows.push_back(raw);
    ++list->stats.cars;
  }
}

// How many distinct meshes a list's items name and how many of those more than one instance names.
inline void CountSharing(List* list) {
  std::vector<uint64_t> keys;
  keys.reserve(list->items.size());
  for (const Item& item : list->items) {
    keys.push_back(item.mesh_key);
  }
  std::sort(keys.begin(), keys.end());
  uint32_t distinct = 0, shared = 0;
  for (size_t i = 0; i < keys.size();) {
    size_t j = i;
    while (j < keys.size() && keys[j] == keys[i]) {
      ++j;
    }
    ++distinct;
    shared += (j - i) > 1 ? 1 : 0;
    i = j;
  }
  list->stats.distinct_meshes = distinct;
  list->stats.shared_meshes = shared;
}

// The words the logs use for a class's limits.
inline std::string Describe(const Limits& limits) {
  if (!limits.On()) {
    return "no other cars";
  }
  return std::to_string(limits.cars) + " cars within " + std::to_string(int(limits.radius + 0.5)) + " m of the player, at most " +
         std::to_string(limits.instances) + " instances, parts under " + std::to_string(limits.min_indices) + " indices left out, " +
         std::to_string(limits.stage_triangles) + " triangles and " + std::to_string(limits.content_bytes / 1024u) +
         " KiB of fingerprints a frame for a new car";
}

}  // namespace mcla::gpu::car_rt_instances
