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
//     the table the shaders read, the cars' rows after them (AppendOthers);
//   - each kept car's box in the frame's view space, from its meshes' model-space bounds and its draws' transforms, which the ray traced
//     occlusion (car_rt_ao.h) takes as the region it traces in; the boxes' rows follow the cars' rows in the same table (BoundsOf, GrowBox).
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
// The IDs a listed car may have: the game's stencil IDs (CarSelector::kSlots) and, above them, the cars placed from memory (car_rt_memory_rules.h
// kRememberedIdBase = kSlots, at most kMaxCars of them at a time, with room to spare).
inline constexpr uint32_t kRememberedIds = 16;

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

// The IDs a listed car may have (AppendOthers' slot table): the game's, then those of the cars placed from memory.
inline constexpr uint32_t kIdSpace = CarSelector::kSlots + kRememberedIds;

// ---------------------------------------------------------------------------
// a car's draws and its body centre
// ---------------------------------------------------------------------------

// The identity of xChrome::PS_Chrome on the retail game (the runtime's own: FNV-1a 64 of the microcode; shaderpack/variants/car_chrome.h kChrome has the
// same number and car_chrome_rt_rules_test.cpp checks that it does): the pixel shader of the tailpipes, the badges and the chrome trim of EVERY car.
inline constexpr uint64_t kChromePs = 0x2D8CA0430FF88ED7ull;

// The mask of the instance of another car's draw: every part of another car has kMaskOthers (what the reflection's, the occlusion's and the
// check's rays hit); a draw of the chrome's shader also has kMaskOtherChrome, so that the reflection pass's primary ray can find another car's chrome at a
// pixel and the chrome's occlusion be traced for it (car_chrome_rt_rules.h). The player's own chrome is a body part (kMaskBody) as before.
inline uint32_t OtherMask(uint64_t ps_identity) {
  return rr::kMaskOthers | (ps_identity == kChromePs ? rr::kMaskOtherChrome : 0u);
}

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
  // The game's own stencil IDs are under CarSelector::kSlots; a car placed from memory (car_rt_memory_rules.h: the camera does not see it this frame, its
  // draws are its kit placed at the vehicle's matrix) has an ID from kRememberedIdBase on and `remembered` set.
  uint32_t stencil_id = 0;
  double distance = std::numeric_limits<double>::infinity();  // metres from the player's vehicle, its nearest draw (a remembered car's: its vehicle's)
  double sum[3] = {};
  uint32_t draws = 0;
  bool remembered = false;
  // A car built from its own model data (car_model_rt_rules.h; remembered is set too: the camera does not draw it, the history holds nothing of it).
  bool walked = false;
  // The car's paint colour where it is known (the game's customisation record: a walked car's, or a remembered car's matched to a walked car at the same place):
  // the hidden hits of the car show it instead of the fixed grey (hlsl/car_rt_refl_cs.hlsl OtherCarColour).
  bool has_paint = false;
  float paint[3] = {};

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
  // > 0: the car's colour is sampled from the history round `centre`; 0: no colour is known (the car's hidden hits are misses); < 0 (-kCarRadius): a car
  // placed from memory (OtherCar::remembered): the camera does not see it, the history holds nothing of it, and the shader shows it in a fixed dark shade
  // (hlsl/car_rt_refl_cs.hlsl OtherCarColour, car_rt_refl_rules.h kRememberedCarColour).
  float radius = 0.0f;
};
static_assert(sizeof(CarRow) == sizeof(rr::InstanceRow), "a car's row is one uint4, as an instance's");

// The second row of a car's record (hlsl/car_rt_common.hlsli CarRtCar::paint): its paint colour as float bits and a flags word (bit 0: the colour is known).
struct CarPaintRow {
  float rgb[3] = {};
  uint32_t flags = 0;
};
static_assert(sizeof(CarPaintRow) == sizeof(rr::InstanceRow), "a car's paint row is one uint4, as an instance's");
// A listed car's record is TWO rows (CarRow, then CarPaintRow); an instance of the car names the first.
inline constexpr uint32_t kCarRows = 2;
inline constexpr uint32_t kPaintKnown = 1;

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

// A world-space DIRECTION (no translation) in the frame's view space, as ViewPoint places a point: the chrome's sky probe needs the world's up in
// view space (the camera pitches; the sky does not). Normalised; false where the result is not a usable vector.
inline bool ViewVector(const temporal::Mat4& view, bool flip_z, const double v[3], float out[3]) {
  double r[3];
  for (int c = 0; c < 3; ++c) {
    r[c] = v[0] * view.m[0][c] + v[1] * view.m[1][c] + v[2] * view.m[2][c];
  }
  if (flip_z) {
    r[2] = -r[2];
  }
  const double length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (!std::isfinite(length) || !(length > 1e-9)) {
    return false;
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = float(r[c] / length);
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
  // The mesh's box in its model space (BoundsOf of its positions); `has_bounds` false: the scene does not know it (the car's box then holds the
  // draw's origin alone).
  float bounds_min[3] = {};
  float bounds_max[3] = {};
  bool has_bounds = false;
};

// A box in the frame's view space (x right, y up, z forward: the occlusion's own). `valid` is false until a point has been added.
struct ViewBox {
  float min[3] = {0, 0, 0};
  float max[3] = {0, 0, 0};
  bool valid = false;
  void Add(const float p[3]) {
    for (int c = 0; c < 3; ++c) {
      if (!valid) {
        min[c] = max[c] = p[c];
      } else {
        min[c] = std::min(min[c], p[c]);
        max[c] = std::max(max[c], p[c]);
      }
    }
    valid = true;
  }
};

// The box of a mesh's positions (float3 each, model space). False (nothing written) for no vertices or a coordinate that is not finite.
inline bool BoundsOf(const std::vector<float>& positions, float lo[3], float hi[3]) {
  const size_t vertices = positions.size() / 3;
  if (vertices == 0) {
    return false;
  }
  float a[3] = {positions[0], positions[1], positions[2]};
  float b[3] = {a[0], a[1], a[2]};
  for (size_t v = 0; v < vertices; ++v) {
    for (int c = 0; c < 3; ++c) {
      const float x = positions[v * 3 + c];
      if (!std::isfinite(x)) {
        return false;
      }
      a[c] = std::min(a[c], x);
      b[c] = std::max(b[c], x);
    }
  }
  for (int c = 0; c < 3; ++c) {
    lo[c] = a[c];
    hi[c] = b[c];
  }
  return true;
}

// Grows `box` by an instance of the mesh `ref` placed by `transform` (the instance's 3x4 in view space, the translation in column 3): the eight
// corners of the mesh's box, or the origin alone where the mesh's box is not known.
inline void GrowBox(ViewBox* box, const float transform[3][4], const MeshRef& ref) {
  if (!ref.has_bounds) {
    const float origin[3] = {transform[0][3], transform[1][3], transform[2][3]};
    box->Add(origin);
    return;
  }
  for (int corner = 0; corner < 8; ++corner) {
    const float p[3] = {(corner & 1) ? ref.bounds_max[0] : ref.bounds_min[0], (corner & 2) ? ref.bounds_max[1] : ref.bounds_min[1],
                        (corner & 4) ? ref.bounds_max[2] : ref.bounds_min[2]};
    float q[3];
    for (int r = 0; r < 3; ++r) {
      q[r] = transform[r][0] * p[0] + transform[r][1] * p[1] + transform[r][2] * p[2] + transform[r][3];
    }
    box->Add(q);
  }
}

// One instance of the top level: the mesh to instance, its transform in the frame's view space and its mask (its row of the table is List::rows[i]).
struct Item {
  uint64_t mesh_key = 0;
  float transform[3][4] = {};
  uint32_t mask = 0;
};

struct ListStats {
  uint32_t player_instances = 0;
  uint32_t other_instances = 0;
  uint32_t other_chrome = 0;     // ... of which draws of the chrome's shader (they also carry kMaskOtherChrome)
  uint32_t cars = 0;             // other cars in the list
  uint32_t cars_cut = 0;         // kept cars left out: their draws did not fit the instance budget (or the class's count of cars)
  uint32_t remembered_cars = 0;       // ... of `cars`: placed from memory, not drawn this frame (car_rt_memory_rules.h)
  uint32_t remembered_instances = 0;  // ... and their instances (counted in other_instances too)
  uint32_t walked_cars = 0;           // ... of `cars` (and of the remembered ones): built from the car's own model data (car_model_rt_rules.h)
  uint32_t walked_instances = 0;
  uint32_t painted_cars = 0;          // cars whose record carries a paint colour
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
  // The other cars' boxes in the frame's view space, one a listed car that has any instance (the nearest first), and the first row of them in
  // `rows`: two rows a box, its minimum and its maximum as float bits (x, y, z, 0), after the cars' rows. The ray traced occlusion traces
  // inside these and the player's own box (hlsl/car_rt_ao_cs.hlsl).
  std::vector<ViewBox> boxes;
  uint32_t box_row = 0;
  // The player's own car's box in the frame's view space: its listed instances' transformed mesh boxes (the same kind of box as `boxes`, tight to what
  // the scene holds of the car). Not valid when no listed instance has a mesh box. The check's scope (car_rt_scene_rules.h) places a pixel in it.
  ViewBox player_box;
  ListStats stats;
  void Clear() {
    items.clear();
    rows.clear();
    boxes.clear();
    box_row = 0;
    player_box = ViewBox{};
    stats = ListStats{};
  }
};

// The kept cars, nearest first, as whole cars while their draws fit `budget` instances and there are at most `max_cars` of them (0: no count: the drawn cars'
// own selection already holds the class's count; with the cars placed from memory among them the plan keeps the nearest `max_cars` of all, by the distance
// to the player and not by the screen); the rest are cut. Returns the ones kept as indices into `cars`, in the order they are to be listed.
inline std::vector<uint32_t> PlanCars(const std::vector<OtherCar>& cars, uint32_t budget, uint32_t* cut = nullptr, uint32_t max_cars = 0) {
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
    if ((max_cars == 0 || kept.size() < max_cars) && used + cars[i].draws <= budget) {
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
// draws whose mesh `find(mesh_key, &ref)` knows, in the draws' order, placed in the frame's view space with the mask OtherMask(draw.ps) (kMaskOthers, and kMaskOtherChrome for a draw of the chrome's shader); then a row for
// every car listed. `max_total` is the top level's own cap on instances (the player's and these). The instance of a car's draw keeps the car's row in
// its `draw` field.
template <typename Find>
inline void AppendOthers(List* list, const std::vector<OtherDraw>& draws, const std::vector<OtherCar>& cars, const temporal::Mat4& view,
                         bool flip_z, uint32_t budget_instances, uint32_t max_total, Find&& find, uint32_t max_cars = 0) {
  const uint32_t have = uint32_t(list->items.size());
  const uint32_t room = max_total > have ? max_total - have : 0;
  uint32_t cut = 0;
  const std::vector<uint32_t> plan = PlanCars(cars, std::min(budget_instances, room), &cut, max_cars);
  list->stats.cars_cut += cut;
  if (plan.empty()) {
    return;
  }
  // The slot of a car in the list: its place in the plan; the row of the car comes after all the items. The IDs are the game's (under CarSelector::kSlots) and,
  // for the cars placed from memory, kRememberedIdBase (= kSlots) up to kSlots + kRememberedIds.
  std::array<int32_t, kIdSpace> slot_of;
  slot_of.fill(-1);
  for (uint32_t k = 0; k < plan.size(); ++k) {
    if (cars[plan[k]].stencil_id < kIdSpace) {
      slot_of[cars[plan[k]].stencil_id] = int32_t(k);
    }
  }
  const size_t first_other = list->items.size();
  std::vector<ViewBox> car_boxes(plan.size());
  for (const OtherDraw& draw : draws) {
    const int32_t slot = draw.stencil_id < kIdSpace ? slot_of[draw.stencil_id] : -1;
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
    item.mask = OtherMask(draw.ps);
    if (!rr::PackInstanceTransform(draw.model_to_world, view, flip_z, item.transform)) {
      ++list->stats.bad_matrix;
      continue;
    }
    list->items.push_back(item);
    list->rows.push_back(rr::MakeRow(ref.first_index, ref.first_vertex, item.mask, draw.after_world, uint32_t(slot) * kCarRows));
    GrowBox(&car_boxes[size_t(slot)], item.transform, ref);
    ++list->stats.other_instances;
    list->stats.other_chrome += (item.mask & rr::kMaskOtherChrome) != 0 ? 1u : 0u;
    list->stats.remembered_instances += cars[plan[size_t(slot)]].remembered ? 1u : 0u;
    list->stats.walked_instances += cars[plan[size_t(slot)]].walked ? 1u : 0u;
  }
  // The cars' records follow the instances' (the table's rows are all uint4: two a car, CarRow then CarPaintRow); the instances name the first by row.
  const uint32_t car_rows = uint32_t(list->items.size());
  for (size_t i = first_other; i < list->rows.size(); ++i) {
    list->rows[i].draw += car_rows;
  }
  for (const uint32_t index : plan) {
    CarRow row;
    row.radius = cars[index].remembered ? -kCarRadius : kCarRadius;
    double centre[3];
    if (!cars[index].Centre(centre) || !ViewPoint(view, flip_z, centre, row.centre)) {
      row.radius = 0.0f;  // no centre: the shader finds no colour for this car (its hidden hits are misses)
    }
    list->stats.remembered_cars += cars[index].remembered ? 1u : 0u;
    list->stats.walked_cars += cars[index].walked ? 1u : 0u;
    rr::InstanceRow raw;
    static_assert(sizeof(raw) == sizeof(row));
    std::memcpy(&raw, &row, sizeof(raw));
    list->rows.push_back(raw);
    CarPaintRow paint;
    if (cars[index].has_paint) {
      for (int c = 0; c < 3; ++c) {
        paint.rgb[c] = cars[index].paint[c];
      }
      paint.flags = kPaintKnown;
      ++list->stats.painted_cars;
    }
    static_assert(sizeof(raw) == sizeof(paint));
    std::memcpy(&raw, &paint, sizeof(raw));
    list->rows.push_back(raw);
    ++list->stats.cars;
  }
  // The boxes' rows after the cars' (a car none of whose draws is listed has no box and no rows).
  list->box_row = uint32_t(list->rows.size());
  for (const ViewBox& box : car_boxes) {
    if (!box.valid) {
      continue;
    }
    list->boxes.push_back(box);
    for (const float* corner : {box.min, box.max}) {
      uint32_t words[4];
      std::memcpy(words, corner, sizeof(float) * 3);
      words[3] = 0;
      rr::InstanceRow raw;
      static_assert(sizeof(raw) == sizeof(words));
      std::memcpy(&raw, words, sizeof(raw));
      list->rows.push_back(raw);
    }
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
