#pragma once
// ===========================================================================
// MCLA GPU renderer -- the other cars the camera does not see, placed from what the scene remembers of them: the rules that need no renderer
// ===========================================================================
// THE LIMIT THIS REMOVES. The ray traced car scene (car_rt_scene.h) holds the cars driving alongside, but it learns of them
// from the DRAWS of the frame: CarFrame (car_frame.h) lists the draws of every stencil ID the game wrote into the main view, keeps the nearest few and gives
// each draw's mesh key and matrix to the instance list (car_rt_instances_rules.h). A car the camera does not see (beside the player and level with him, out
// of a chase camera's picture, behind a cockpit camera, a neighbour at the door) is culled by the game: it has no draw this frame, so no mesh key and no
// matrix, so no instance, and the player's door cannot show it. The car that should show most is the one nearest, and it is the one least often in the picture.
//
// WHAT THIS DOES. The game knows where every car is, drawn or not: the opponent manager's racers and the ambient manager's live cars (traffic and parked
// cars) each have a world matrix (car_mesh_read.h ReadOtherVehicles, the same lists cinematic/cars.cpp and the traffic probe read). Every frame a car that IS
// drawn is matched to its vehicle in those lists (by position: the mean of its draws' origins is about the vehicle's), and its draws are remembered as a KIT:
// for each draw the mesh key and the draw's matrix RELATIVE TO THE VEHICLE'S MATRIX,
//
//        rel = P * inverse(M)          (row vectors: world = (p, 1) * P; M the vehicle's world matrix when the draw was made)
//
// A later frame in which the vehicle is in the list, within the radius, and NOT drawn (no seen car is matched to it) places every draw of its kit again with
// the vehicle's matrix of that frame, P' = rel * M', and hands the draws to the instance list as the draws of a car (stencil ID kRememberedIdBase + slot,
// OtherCar::remembered). Nothing else changes: the same class caps (High 4 cars within 14 m, Mid 2 within 10 m), counted with the drawn cars by the distance
// to the player (not by the screen), the same instance budget, the same structures (one bottom level per mesh key: a car that was seen once keeps its meshes
// while it is placed), the same masks, boxes and rows.
//
// WHAT CAN BE PLACED WITHOUT DRAWS. Everything the kit holds: the body's parts, the glass, the wheels (their matrices are relative to the vehicle too: the
// wheel stands where it stood at the last sight, with the steering and the spin of that frame, and the suspension of that frame), the chrome. What cannot:
// a car never drawn while it was within the radius (no mesh key, no mesh: nothing, as the brief says), a part whose mesh the scene has given back
// (skipped; the car goes when under half of its kit is resident), the driver (skinned: not in the scene at all), the parts of the LOD the car was drawn at (a car
// seen only far away keeps that LOD's meshes), a vehicle the lists do not show (police cars: the police manager's matrix is not read here).
//
// WHEN A KIT GOES: its vehicle is not in the lists any more (or is beyond the scan radius, 1.5 x the class's radius plus 2 m), the model changed under the same
// slot (the ambient car's type pointer), the car jumped (a slot of the pool given to another car, a teleport), fewer than half of its draws' meshes are resident,
// or the store is full and it is the one drawn longest ago.
//
// Matrices are row vectors (v' = v M) as in temporal_math.h. This header includes nothing of the renderer. car_rt_memory_rules_test.cpp checks it.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_rt_memory {

namespace ci = car_rt_instances;
namespace tp = temporal;

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

// How near the mean of a drawn car's draw origins (the draws' gWorld translation: the car's root, as the player's own draws are 0.00 m from his vehicle when
// parked) must be to a vehicle's position (metres) to be that vehicle, and so to make or refresh its kit. The draws come from a frame or more before the game's
// own record of the car is read, so a fast car is ahead of its draws by its speed times that lag (car_frame_rules.h IdentityTracker takes 5 m for the player at
// 250 km/h). A lane is 3.5 m wide and the pairs are assigned nearest first, so for cars driving side by side the true pair (the lag) is nearer than the wrong
// one (the lag and the lane): the tolerance is no risk to them. CHECK: the log's "worst match offset" and "the kits' prediction" lines.
inline constexpr double kMatchMetres = 3.0;
// A SECOND, looser match for what the strict one left: a vehicle that holds a kit and has a drawn car within this many metres is covered (not placed), but the
// car is not matched to it (no kit is made or refreshed from a loose match). It keeps a very fast car, whose draws lag its record by more than kMatchMetres, from
// being placed again ahead of itself as a second car; its price is that a drawn car of no list (a police car) within this of a culled car with a kit hides it.
inline constexpr double kLooseMatchMetres = 5.0;
// A vehicle within this of the player's own is the player's (the player's racer may be in the opponent manager's list).
inline constexpr double kOwnMetres = 1.0;
// The scan radius of the vehicle lists: the class's radius times this plus the extra. A kit whose vehicle leaves the scan is dropped.
inline constexpr double kScanFactor = 1.5;
inline constexpr double kScanExtraMetres = 2.0;
// A vehicle that moved more than kJumpBase + kJumpPerFrame x (frames since its last reading, at least 1) metres, at most kJumpMax, is another car.
inline constexpr double kJumpBaseMetres = 6.0;
inline constexpr double kJumpPerFrameMetres = 1.5;
inline constexpr double kJumpMaxMetres = 60.0;
// The kits held: the class's cars plus those that come and go (the most any setting asks for is ci::kMaxCars).
inline constexpr uint32_t kMaxKits = 8;
// A kit needs this many draws to be worth keeping (a car seen through a gap has a few parts and no body).
inline constexpr uint32_t kMinKitDraws = 4;
// A kit is placed while at least this share of its draws' meshes have a structure; under it the kit is dropped (the meshes were given back).
inline constexpr double kMinResidentShare = 0.5;
// A kit's draws are replaced by the next frame's draws of the same car when the new list has at least this share of the old one's draws (a car that
// appears in parts, or one drawn in a smaller LOD, does not shrink its kit), or when the kit is older than kRefreshFrames guest frames.
inline constexpr double kReplaceShare = 0.9;
inline constexpr uint64_t kRefreshFrames = 600;
// The stencil ID of a car placed from memory is this plus its kit's slot (the game's own IDs are under CarSelector::kSlots).
inline constexpr uint32_t kRememberedIdBase = ci::CarSelector::kSlots;
static_assert(kRememberedIdBase + kMaxKits <= ci::kIdSpace, "the instance list's slot table has room for the remembered cars' IDs");

// ---------------------------------------------------------------------------
// the game's own vehicles
// ---------------------------------------------------------------------------

enum class VehicleKind : uint8_t {
  kOpponent,  // the opponent manager's active racers (a race's opponents, the toolkit's scene cars)
  kTraffic,   // the ambient manager's live moving cars
  kParked,    // the ambient manager's live parked cars
};

inline const char* KindName(VehicleKind kind) {
  switch (kind) {
    case VehicleKind::kOpponent:
      return "opponent";
    case VehicleKind::kTraffic:
      return "traffic";
    case VehicleKind::kParked:
      return "parked";
  }
  return "?";
}

// One vehicle of the game's lists with its world matrix as the game holds it: rows right, up, back and position (row vectors; the fourth column is not
// read: the game leaves NaN there, cinematic/cars.cpp). `address` names the game's object and is stable while the car lives; `tag` names its model where
// the list says (the ambient car's type pointer; 0 for an opponent).
struct VehicleRead {
  uint32_t address = 0;
  uint32_t tag = 0;
  VehicleKind kind = VehicleKind::kOpponent;
  double rows[4][3] = {};

  // The world matrix with the fourth column made (0, 0, 0, 1).
  tp::Mat4 Matrix() const {
    tp::Mat4 m = tp::Identity();
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 3; ++c) {
        m.m[r][c] = rows[r][c];
      }
    }
    return m;
  }
  void Position(double out[3]) const {
    for (int c = 0; c < 3; ++c) {
      out[c] = rows[3][c];
    }
  }
};

// Whether the rows are a car's pose: finite, a position within a million metres, three axes about a metre long (the body's squash and stretch scale them a
// little) that span a volume. A garbage read (a freed object) fails it.
inline bool UsableRows(const double rows[4][3]) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(rows[r][c])) {
        return false;
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    if (std::fabs(rows[3][c]) > 1.0e6) {
      return false;
    }
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2]);
    if (!(length > 0.25 && length < 4.0)) {
      return false;
    }
  }
  const double det = rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1]) - rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0]) +
                     rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]);
  return std::fabs(det) > 0.02;
}

inline double Distance(const double a[3], const double b[3]) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How far from the player a vehicle may be to be read at all, for the class's radius.
inline double ScanRadius(double radius) { return radius * kScanFactor + kScanExtraMetres; }

// ---------------------------------------------------------------------------
// the cars the frame drew
// ---------------------------------------------------------------------------

// A stencil ID of the frame (not the player's) with the sum of its draws' gWorld translations: every draw of every other car counts, kept or not, so that a
// car the selection did not keep still covers its vehicle.
struct SeenCar {
  uint32_t stencil_id = 0;
  double sum[3] = {};
  uint32_t draws = 0;

  bool Centre(double out[3]) const {
    if (draws == 0) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      out[c] = sum[c] / double(draws);
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
  }
};

// Adds a draw of `id` with the translation of its gWorld. A translation that is not finite is not counted.
inline void NoteSeen(std::vector<SeenCar>* seen, uint32_t id, const double translation[3]) {
  if (!std::isfinite(translation[0]) || !std::isfinite(translation[1]) || !std::isfinite(translation[2])) {
    return;
  }
  SeenCar* car = nullptr;
  for (SeenCar& s : *seen) {
    if (s.stencil_id == id) {
      car = &s;
      break;
    }
  }
  if (!car) {
    seen->emplace_back();
    car = &seen->back();
    car->stencil_id = id;
  }
  for (int c = 0; c < 3; ++c) {
    car->sum[c] += translation[c];
  }
  ++car->draws;
}

// Matches the cars the frame drew to the vehicles: the nearest pairs first, each car and each vehicle once, within `tolerance`. `vehicle_of_seen[i]` is the
// index of the vehicle matched to seen[i] (-1: none), `taken[j]` whether vehicle j was matched. Returns the largest offset of a match (0: none).
inline double MatchSeen(const std::vector<SeenCar>& seen, const std::vector<VehicleRead>& vehicles, double tolerance, std::vector<int32_t>* vehicle_of_seen,
                        std::vector<uint8_t>* taken) {
  vehicle_of_seen->assign(seen.size(), -1);
  taken->assign(vehicles.size(), 0);
  struct Pair {
    double distance;
    uint32_t car;
    uint32_t vehicle;
  };
  std::vector<Pair> pairs;
  for (uint32_t i = 0; i < seen.size(); ++i) {
    double centre[3];
    if (!seen[i].Centre(centre)) {
      continue;
    }
    for (uint32_t j = 0; j < vehicles.size(); ++j) {
      double position[3];
      vehicles[j].Position(position);
      const double d = Distance(centre, position);
      if (d <= tolerance) {
        pairs.push_back(Pair{d, i, j});
      }
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
  double worst = 0.0;
  for (const Pair& p : pairs) {
    if ((*vehicle_of_seen)[p.car] >= 0 || (*taken)[p.vehicle] != 0) {
      continue;
    }
    (*vehicle_of_seen)[p.car] = int32_t(p.vehicle);
    (*taken)[p.vehicle] = 1;
    worst = std::max(worst, p.distance);
  }
  return worst;
}

// The second, looser pass over what MatchSeen left (see kLooseMatchMetres): the cars of `seen` with no vehicle (`vehicle_of_seen[i] < 0`) against the vehicles that
// are not `taken` and are `eligible` (the ones that hold a kit), nearest pairs first. Marks the vehicles taken; returns how many it took.
inline uint32_t MatchLoose(const std::vector<SeenCar>& seen, const std::vector<VehicleRead>& vehicles, double tolerance, const std::vector<int32_t>& vehicle_of_seen,
                           const std::vector<uint8_t>& eligible, std::vector<uint8_t>* taken) {
  struct Pair {
    double distance;
    uint32_t car;
    uint32_t vehicle;
  };
  std::vector<Pair> pairs;
  for (uint32_t i = 0; i < seen.size(); ++i) {
    double centre[3];
    if (vehicle_of_seen[i] >= 0 || !seen[i].Centre(centre)) {
      continue;
    }
    for (uint32_t j = 0; j < vehicles.size(); ++j) {
      if ((*taken)[j] != 0 || eligible[j] == 0) {
        continue;
      }
      double position[3];
      vehicles[j].Position(position);
      const double d = Distance(centre, position);
      if (d <= tolerance) {
        pairs.push_back(Pair{d, i, j});
      }
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
  std::vector<uint8_t> car_used(seen.size(), 0);
  uint32_t took = 0;
  for (const Pair& p : pairs) {
    if (car_used[p.car] != 0 || (*taken)[p.vehicle] != 0) {
      continue;
    }
    car_used[p.car] = 1;
    (*taken)[p.vehicle] = 1;
    ++took;
  }
  return took;
}

// ---------------------------------------------------------------------------
// the kit: a car's draws relative to its vehicle
// ---------------------------------------------------------------------------

struct KitDraw {
  uint64_t mesh_key = 0;
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint32_t indices = 0;
  bool after_world = false;
  float rel[4][3] = {};  // P * inverse(M): the draw's matrix in the vehicle's frame; the fourth column is (0, 0, 0, 1)
};

// rel = P * inverse(M), for a draw's model-to-world P. False (nothing written) when the result is not finite or P is not affine (a perspective column).
inline bool MakeRelative(const tp::Mat4& model_to_world, const tp::Mat4& vehicle_inverse, float out[4][3]) {
  const tp::Mat4 r = tp::Mul(model_to_world, vehicle_inverse);
  if (!tp::Finite(r)) {
    return false;
  }
  constexpr double kAffine = 1e-3;
  if (std::fabs(r.m[0][3]) > kAffine || std::fabs(r.m[1][3]) > kAffine || std::fabs(r.m[2][3]) > kAffine || std::fabs(r.m[3][3] - 1.0) > kAffine) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 3; ++j) {
      out[i][j] = float(r.m[i][j]);
    }
  }
  return true;
}

// P' = rel * M: the draw's matrix with the vehicle at `vehicle`.
inline tp::Mat4 Place(const float rel[4][3], const tp::Mat4& vehicle) {
  tp::Mat4 r = tp::Identity();
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[i][j] = double(rel[i][j]);
    }
  }
  return tp::Mul(r, vehicle);
}

struct Kit {
  uint32_t vehicle = 0;       // the game's object
  uint32_t tag = 0;
  VehicleKind kind = VehicleKind::kOpponent;
  uint32_t slot = 0;          // 0 .. kMaxKits - 1: the car's stable number (its stencil ID is kRememberedIdBase + slot)
  uint64_t recorded = 0;      // the guest frame the draws were made from
  uint64_t drawn = 0;         // the guest frame the car was last drawn and matched
  uint64_t updated = 0;       // the guest frame the vehicle's position was last read
  double position[3] = {};    // ... and what it was
  std::vector<KitDraw> draws;
};

// What a frame's update found, for the log and for the tests.
struct Stats {
  uint32_t vehicles = 0;        // the game's vehicles read near the player
  uint32_t seen = 0;            // stencil IDs of other cars the frame drew
  uint32_t matched = 0;         // ... matched to a vehicle
  uint32_t loose_covered = 0;   // vehicles with a kit that a drawn car within kLooseMatchMetres covers (a fast car whose draws lag its record)
  double worst_offset = 0.0;    // the largest match offset, metres
  uint32_t recorded = 0;        // kits made or replaced
  uint32_t record_refused = 0;  // a matched car whose draws could not be made relative (bad matrix, too few draws)
  // The proof of the placement maths on the game's own data: for a car drawn this frame whose kit (made in an earlier frame) lists the same meshes in the same
  // order, the kit placed at the vehicle's matrix of THIS frame against the draws' own origins: how far the prediction was from what the game drew.
  uint32_t predicted_draws = 0;
  double prediction_sum = 0.0;    // metres, over those draws
  double prediction_worst = 0.0;
  uint32_t kits = 0;            // kits held after the frame
  uint32_t covered = 0;         // kits whose car is drawn this frame (not placed: the game's draws stand)
  uint32_t out_of_range = 0;    // kits whose vehicle is beyond the radius (kept, not placed)
  uint32_t placed_cars = 0;     // cars placed from memory
  uint32_t placed_draws = 0;
  uint32_t not_resident = 0;    // draws of placed cars left out: their mesh has no structure
  uint32_t dropped_gone = 0;    // kits dropped: the vehicle is not in the lists
  uint32_t dropped_tag = 0;     // ... another model in the slot
  uint32_t dropped_jump = 0;    // ... the car moved too far to be the same
  uint32_t dropped_meshes = 0;  // ... under half of its meshes are resident
  uint32_t dropped_full = 0;    // ... the store was full: the one drawn longest ago went
};

struct Totals {
  uint64_t frames = 0;
  uint64_t frames_placed = 0;   // frames with at least one car placed from memory
  uint64_t cars_placed = 0;
  uint64_t draws_placed = 0;
  uint64_t recorded = 0;
  uint64_t record_refused = 0;
  uint64_t predicted_draws = 0;
  double prediction_sum = 0.0;
  double prediction_worst = 0.0;
  uint64_t dropped = 0;
  uint64_t not_resident = 0;
  uint32_t most_kits = 0;
  uint32_t most_placed = 0;
  uint32_t vehicles_max = 0;
  double worst_offset = 0.0;
  uint64_t no_player = 0;       // frames the vehicle lists could not be used
};

// ---------------------------------------------------------------------------
// the store and the update
// ---------------------------------------------------------------------------

class CarMemory {
 public:
  struct Input {
    uint64_t guest_frame = 0;
    const ci::Limits* limits = nullptr;
    double match_metres = 0.0;                     // the strict match tolerance (0: kMatchMetres); the loose one is this plus 2 m
    bool player_known = false;
    double player[3] = {};                         // the player's vehicle position
    bool vehicles_valid = false;                   // the lists were read this frame (false: nothing is placed and nothing is dropped)
    const std::vector<VehicleRead>* vehicles = nullptr;
    const std::vector<SeenCar>* seen = nullptr;
    const std::vector<ci::OtherCar>* cars = nullptr;    // the cars the selection kept (drawn)
    const std::vector<ci::OtherDraw>* draws = nullptr;  // their draws
  };
  struct Output {
    std::vector<ci::OtherCar> cars;    // the cars placed from memory, nearest first (OtherCar::remembered)
    std::vector<ci::OtherDraw> draws;  // their draws, placed in the world
    Stats stats;
    void Clear() {
      cars.clear();
      draws.clear();
      stats = Stats{};
    }
  };

  // The frame: records the kits of the cars that were drawn, drops what is stale, places the kits of the cars that were not. `resident(mesh_key)` says
  // whether the scene holds a bottom level structure for the mesh. Fills `out` (cleared first).
  template <typename Resident>
  void Update(const Input& in, Resident&& resident, Output* out) {
    out->Clear();
    ++totals_.frames;
    if (!in.limits || !in.limits->On() || !in.vehicles || !in.seen || !in.cars || !in.draws) {
      Clear();
      return;
    }
    if (!in.player_known || !in.vehicles_valid) {
      ++totals_.no_player;
      return;  // nothing known this frame: the kits stay as they are, nothing is placed
    }
    const std::vector<VehicleRead>& vehicles = *in.vehicles;
    Stats& stats = out->stats;
    stats.vehicles = uint32_t(vehicles.size());
    stats.seen = uint32_t(in.seen->size());
    totals_.vehicles_max = std::max(totals_.vehicles_max, stats.vehicles);

    // The cars the frame drew, matched to the vehicles.
    std::vector<int32_t> vehicle_of_seen;
    std::vector<uint8_t> taken;
    const double strict = in.match_metres > 0.0 ? in.match_metres : kMatchMetres;
    stats.worst_offset = MatchSeen(*in.seen, vehicles, strict, &vehicle_of_seen, &taken);
    totals_.worst_offset = std::max(totals_.worst_offset, stats.worst_offset);
    for (const int32_t v : vehicle_of_seen) {
      stats.matched += v >= 0 ? 1u : 0u;
    }
    // What the strict pass left: a drawn car within the loose tolerance of a vehicle that holds a kit covers it (no ghost of a fast car ahead of itself).
    {
      std::vector<uint8_t> eligible(vehicles.size(), 0);
      for (size_t j = 0; j < vehicles.size(); ++j) {
        for (const Kit& k : kits_) {
          if (k.vehicle == vehicles[j].address) {
            eligible[j] = 1;
            break;
          }
        }
      }
      stats.loose_covered = MatchLoose(*in.seen, vehicles, strict + (kLooseMatchMetres - kMatchMetres), vehicle_of_seen, eligible, &taken);
    }

    // The kits of the cars that were drawn and kept: their draws relative to their vehicles.
    for (const ci::OtherCar& car : *in.cars) {
      if (car.remembered || car.draws == 0) {
        continue;
      }
      int32_t vehicle = -1;
      for (uint32_t i = 0; i < in.seen->size(); ++i) {
        if ((*in.seen)[i].stencil_id == car.stencil_id) {
          vehicle = vehicle_of_seen[i];
          break;
        }
      }
      if (vehicle < 0) {
        continue;
      }
      Record(in, car, vehicles[size_t(vehicle)], &stats);
    }

    // What to do with every kit: drop, skip (drawn, out of range) or place.
    struct Candidate {
      uint32_t kit;
      double distance;
      int32_t vehicle;
    };
    std::vector<Candidate> candidates;
    for (size_t k = 0; k < kits_.size();) {
      Kit& kit = kits_[k];
      int32_t vehicle = -1;
      for (uint32_t j = 0; j < vehicles.size(); ++j) {
        if (vehicles[j].address == kit.vehicle) {
          vehicle = int32_t(j);
          break;
        }
      }
      if (vehicle < 0) {
        ++stats.dropped_gone;
        Erase(k);
        continue;
      }
      const VehicleRead& v = vehicles[size_t(vehicle)];
      if (v.tag != kit.tag) {
        ++stats.dropped_tag;
        Erase(k);
        continue;
      }
      double position[3];
      v.Position(position);
      const double since = double(std::max<uint64_t>(in.guest_frame > kit.updated ? in.guest_frame - kit.updated : 1, 1));
      const double allowed = std::min(kJumpBaseMetres + kJumpPerFrameMetres * since, kJumpMaxMetres);
      const bool jumped = Distance(position, kit.position) > allowed;
      for (int c = 0; c < 3; ++c) {
        kit.position[c] = position[c];
      }
      kit.updated = in.guest_frame;
      if (jumped) {
        ++stats.dropped_jump;
        Erase(k);
        continue;
      }
      ++k;
      if (kit.drawn == in.guest_frame) {
        ++stats.covered;
        continue;
      }
      if (taken[size_t(vehicle)] != 0) {
        ++stats.covered;  // a car the frame drew is this vehicle's (its kit could not be made from the draws, or is older than this frame's)
        continue;
      }
      const double distance = Distance(position, in.player);
      if (!(distance <= in.limits->radius)) {
        ++stats.out_of_range;
        continue;
      }
      candidates.push_back(Candidate{uint32_t(k - 1), distance, vehicle});
    }

    // The nearest cars within the class's count: whole kits whose meshes are resident.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    std::vector<uint32_t> drop;  // kit indices to drop after the loop (the indices of `candidates` stay valid meanwhile)
    for (const Candidate& c : candidates) {
      if (out->cars.size() >= in.limits->cars) {
        break;
      }
      const Kit& kit = kits_[c.kit];
      uint32_t resident_draws = 0;
      for (const KitDraw& d : kit.draws) {
        resident_draws += resident(d.mesh_key) ? 1u : 0u;
      }
      if (resident_draws == 0 || double(resident_draws) < kMinResidentShare * double(kit.draws.size())) {
        drop.push_back(c.kit);
        continue;
      }
      const tp::Mat4 matrix = vehicles[size_t(c.vehicle)].Matrix();
      ci::OtherCar car;
      car.stencil_id = kRememberedIdBase + kit.slot;
      car.remembered = true;
      uint32_t order = 0;
      for (const KitDraw& d : kit.draws) {
        if (!resident(d.mesh_key)) {
          ++stats.not_resident;
          continue;
        }
        ci::OtherDraw draw;
        draw.stencil_id = car.stencil_id;
        draw.order = order++;
        draw.after_world = d.after_world;
        draw.vs = d.vs;
        draw.ps = d.ps;
        draw.mesh_key = d.mesh_key;
        draw.indices = d.indices;
        draw.model_to_world = Place(d.rel, matrix);
        car.Add(draw.model_to_world, c.distance);
        out->draws.push_back(draw);
        ++stats.placed_draws;
      }
      out->cars.push_back(car);
      ++stats.placed_cars;
    }
    // Drop the kits whose meshes are gone, last index first (the erase moves the tail down).
    std::sort(drop.begin(), drop.end(), std::greater<uint32_t>());
    for (const uint32_t k : drop) {
      ++stats.dropped_meshes;
      Erase(k);
    }
    stats.kits = uint32_t(kits_.size());

    totals_.frames_placed += stats.placed_cars != 0 ? 1u : 0u;
    totals_.cars_placed += stats.placed_cars;
    totals_.draws_placed += stats.placed_draws;
    totals_.recorded += stats.recorded;
    totals_.record_refused += stats.record_refused;
    totals_.predicted_draws += stats.predicted_draws;
    totals_.prediction_sum += stats.prediction_sum;
    totals_.prediction_worst = std::max(totals_.prediction_worst, stats.prediction_worst);
    totals_.dropped += uint64_t(stats.dropped_gone) + stats.dropped_tag + stats.dropped_jump + stats.dropped_meshes + stats.dropped_full;
    totals_.not_resident += stats.not_resident;
    totals_.most_kits = std::max(totals_.most_kits, stats.kits);
    totals_.most_placed = std::max(totals_.most_placed, stats.placed_cars);
  }

  void Clear() {
    kits_.clear();
  }
  const std::vector<Kit>& kits() const { return kits_; }
  const Totals& totals() const { return totals_; }
  void ResetTotals() { totals_ = Totals{}; }

 private:
  void Erase(size_t index) {
    kits_.erase(kits_.begin() + std::ptrdiff_t(index));
  }

  // The kit placed at the vehicle's matrix of this frame against the draws the game made of the same car in this frame (same meshes, same order): the distance
  // between the prediction's origin and the draw's, per draw. A parked car should give about 0; a moving car shows the suspension and the lag.
  static void Predict(const Kit& kit, const VehicleRead& v, uint32_t id, const std::vector<ci::OtherDraw>& draws, Stats* stats) {
    const tp::Mat4 matrix = v.Matrix();
    size_t i = 0;
    double sum = 0.0, worst = 0.0;
    uint32_t n = 0;
    for (const ci::OtherDraw& d : draws) {
      if (d.stencil_id != id || d.mesh_key == 0) {
        continue;
      }
      if (i >= kit.draws.size() || kit.draws[i].mesh_key != d.mesh_key) {
        return;  // another set of parts: no comparison
      }
      const tp::Mat4 predicted = Place(kit.draws[i].rel, matrix);
      const double e = Distance(predicted.m[3], d.model_to_world.m[3]);
      if (std::isfinite(e)) {
        sum += e;
        worst = std::max(worst, e);
        ++n;
      }
      ++i;
    }
    stats->predicted_draws += n;
    stats->prediction_sum += sum;
    stats->prediction_worst = std::max(stats->prediction_worst, worst);
  }

  // The lowest slot number no kit holds.
  uint32_t FreeSlot() const {
    for (uint32_t slot = 0; slot < kMaxKits; ++slot) {
      bool used = false;
      for (const Kit& k : kits_) {
        used = used || k.slot == slot;
      }
      if (!used) {
        return slot;
      }
    }
    return kMaxKits - 1;
  }

  // The drawn, kept car `car` is the vehicle `v`: makes or replaces its kit.
  void Record(const Input& in, const ci::OtherCar& car, const VehicleRead& v, Stats* stats) {
    if (!UsableRows(v.rows)) {
      ++stats->record_refused;
      return;
    }
    Kit* kit = nullptr;
    for (Kit& k : kits_) {
      if (k.vehicle == v.address) {
        kit = &k;
        break;
      }
    }
    if (kit && kit->tag != v.tag) {
      // Another model in the slot: what was kept is not this car's.
      for (size_t i = 0; i < kits_.size(); ++i) {
        if (&kits_[i] == kit) {
          ++stats->dropped_tag;
          Erase(i);
          break;
        }
      }
      kit = nullptr;
    }
    uint32_t count = 0;
    for (const ci::OtherDraw& d : *in.draws) {
      count += d.stencil_id == car.stencil_id ? 1u : 0u;
    }
    if (count < kMinKitDraws) {
      ++stats->record_refused;
      if (kit) {
        kit->drawn = in.guest_frame;  // the car is drawn: it is not placed on top of its own draws
      }
      return;
    }
    if (kit && kit->draws.size() == count && count != 0) {
      Predict(*kit, v, car.stencil_id, *in.draws, stats);
    }
    const bool replace = !kit || double(count) >= kReplaceShare * double(kit->draws.size()) || in.guest_frame > kit->recorded + kRefreshFrames;
    if (kit && !replace) {
      kit->drawn = in.guest_frame;
      return;
    }
    tp::Mat4 inverse;
    if (!tp::Inverse(v.Matrix(), &inverse)) {
      ++stats->record_refused;
      return;
    }
    // The new draws are made in a scratch vector (its memory is reused from frame to frame) and swapped in when there are enough of them: a refused listing
    // leaves the old kit as it was.
    std::vector<KitDraw>& draws = scratch_;
    draws.clear();
    draws.reserve(count);
    for (const ci::OtherDraw& d : *in.draws) {
      if (d.stencil_id != car.stencil_id || d.mesh_key == 0) {
        continue;
      }
      KitDraw k;
      if (!MakeRelative(d.model_to_world, inverse, k.rel)) {
        continue;
      }
      k.mesh_key = d.mesh_key;
      k.vs = d.vs;
      k.ps = d.ps;
      k.indices = d.indices;
      k.after_world = d.after_world;
      draws.push_back(k);
    }
    if (draws.size() < kMinKitDraws) {
      ++stats->record_refused;
      if (kit) {
        kit->drawn = in.guest_frame;  // the old kit stays; the car is drawn, so it is not placed on top of its own draws
      }
      return;
    }
    if (!kit) {
      if (kits_.size() >= kMaxKits) {
        size_t oldest = 0;
        for (size_t i = 1; i < kits_.size(); ++i) {
          if (kits_[i].drawn < kits_[oldest].drawn) {
            oldest = i;
          }
        }
        ++stats->dropped_full;
        Erase(oldest);
      }
      const uint32_t slot = FreeSlot();  // before the new kit is in the store: its own default slot would count as taken
      kits_.emplace_back();
      kit = &kits_.back();
      kit->slot = slot;
      kit->vehicle = v.address;
    }
    kit->tag = v.tag;
    kit->kind = v.kind;
    kit->recorded = in.guest_frame;
    kit->drawn = in.guest_frame;
    kit->updated = in.guest_frame;
    v.Position(kit->position);
    kit->draws.swap(draws);  // the scratch now holds the old draws' memory
    ++stats->recorded;
  }

  std::vector<Kit> kits_;
  std::vector<KitDraw> scratch_;
  Totals totals_;
};

// ---------------------------------------------------------------------------
// the log's words
// ---------------------------------------------------------------------------

inline std::string Describe(const Stats& s) {
  const std::string prediction = s.predicted_draws != 0
                                     ? std::format("kits placed at this frame's vehicles against the game's own draws: {} draws, mean error {:.3f} m, worst {:.3f} m",
                                                   s.predicted_draws, s.prediction_sum / double(s.predicted_draws), s.prediction_worst)
                                     : std::string("no kit was compared with the game's draws this frame");
  return prediction + "; " + std::format("{} vehicles near the player, {} other cars drawn of which {} matched to a vehicle (worst offset {:.2f} m, {} more covered loosely); {} kits held ({} made or replaced, {} refused); "
                     "{} placed from memory with {} draws ({} left out: no structure), {} kits covered by a drawn car, {} beyond the radius; dropped: {} vehicle gone, {} another model, "
                     "{} jumped, {} meshes given back, {} store full",
                     s.vehicles, s.seen, s.matched, s.worst_offset, s.loose_covered, s.kits, s.recorded, s.record_refused, s.placed_cars, s.placed_draws, s.not_resident, s.covered,
                     s.out_of_range, s.dropped_gone, s.dropped_tag, s.dropped_jump, s.dropped_meshes, s.dropped_full);
}

}  // namespace mcla::gpu::car_rt_memory
