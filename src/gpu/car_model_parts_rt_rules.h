#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced scene's walked cars: wheels, traffic and the kinds of what a car brings (RT-MODEL-PARTS): the rules that need no renderer
// ===========================================================================
// RT-MODEL-CARS (car_model_rt_rules.h) builds the body and the model set's parts of the racers the camera does not draw. This is the rest:
//
//   - THE WHEELS (car_model_wheel_rules.h: the manager's tyre and rim, the vehicle's four wheel matrices). WheelCalibration checks the hypotheses for where a wheel geometry
//     stands against the draws of the cars the camera sees (the player's wheels at least), kind by kind (tyre, rim), exactly as PoseCalibration does for the parts: a wheel of a
//     car the camera does not draw is placed only after a hypothesis fits the draws within kFitMetres. A wheel geometry is one mesh placed FOUR times (one instance per wheel the
//     vehicle draws) on one structure: the cars that wear the same wheel share it.
//   - THE TRAFFIC AND THE PARKED CARS (car_model_traffic_rules.h: the renderer's ambient client's items, one drawable a car, the bones in the item's block). TrafficCalibration does
//     the same for the three placement hypotheses of a bone against the draws of the traffic cars the camera sees.
//   - THE KINDS. Every Part the scene takes is one of body (the body shell and the model set's parts), wheel (a tyre or a rim) or model (a traffic car's one drawable); the driver is
//     none: it is skinned (four blended bones through the vehicle's palette) and the draw path leaves it out of the scene (car_rt_instances_rules.h OtherDraw), as does the walk
//     (PickGeometries takes rigid geometry only); Composition counts what a car brought and how many skinned in-use geometries it left out.
//   - WHICH CARS. The class's count (ci::Limits::cars) and radius rank the racers; the traffic has a count of its own (TrafficCarsFor): a traffic car's model is a fraction of a
//     racer's (one lod 1 drawable of a few thousand triangles against 40 to 90 thousand), so the same number is cheap; the build budget (the staged triangles and KiB a frame,
//     PlanBuilds) is the same one for all walked cars.
//
// Matrices are row vectors as everywhere (temporal_math.h).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "car_model_rt_rules.h"
#include "car_model_traffic_rules.h"
#include "car_model_wheel_rules.h"

namespace mcla::gpu::car_model_parts {

namespace cw = car_model_walk;
namespace crt = car_model_rt;
namespace tp = temporal;
namespace ci = car_rt_instances;

// ---------------------------------------------------------------------------
// kinds
// ---------------------------------------------------------------------------

enum class Kind : uint8_t { kBody, kWheel, kModel };

inline Kind KindOf(uint32_t part) {
  if (cw::IsWheelPart(part)) {
    return Kind::kWheel;
  }
  return part == cw::kModelPart ? Kind::kModel : Kind::kBody;
}

// What a walked car has in the top level (instances by kind).
struct Composition {
  uint32_t body = 0;              // the body shell and the model set's parts
  uint32_t tyres = 0, rims = 0;   // wheel instances (four to a wheel geometry)
  uint32_t model = 0;             // a traffic car's instances (the body and the wheels are one drawable)
  uint32_t driver = 0;            // never: the driver is skinned
  uint32_t skinned_left_out = 0;  // in-use skinned geometries of the walk (the driver's, a skinned seat's): not in the scene
  uint32_t wheels() const { return tyres + rims; }
  uint32_t total() const { return body + tyres + rims + model + driver; }
  bool operator==(const Composition&) const = default;
};

inline void Count(Composition* c, uint32_t part) {
  switch (KindOf(part)) {
    case Kind::kWheel:
      ++(cw::IsRimPart(part) ? c->rims : c->tyres);
      break;
    case Kind::kModel:
      ++c->model;
      break;
    default:
      ++c->body;
      break;
  }
}

inline uint32_t SkinnedInUse(const cw::CarWalk& walk) {
  uint32_t n = 0;
  for (const cw::Geometry& g : walk.geometries) {
    n += g.in_use && g.palette_count != 0;
  }
  return n;
}

inline std::string Describe(const Composition& c, bool traffic) {
  if (traffic) {
    return std::format("model {} (the body and the wheels are one drawable), driver {} (none: skinned geometry is left out, {} in use)", c.model, c.driver, c.skinned_left_out);
  }
  return std::format("body {}, wheels {} ({} tyre + {} rim instances), driver {} (skinned: left out of the scene as the draw path leaves it; {} skinned in-use geometries not taken)", c.body, c.wheels(),
                     c.tyres, c.rims, c.driver, c.skinned_left_out);
}

// ---------------------------------------------------------------------------
// the wheels: which hypothesis places them
// ---------------------------------------------------------------------------

// The calibration of the wheels' placement, one per kind (tyre = 0, rim = 1). A matched pair is (a wheel geometry of the car, the draw that is of it): the draw's own
// model-to-world against the four wheels' placements under each hypothesis (the best of the four counts: the draws of one wheel mesh are four, one per wheel, and which is
// which is not told). The same numbers as PoseCalibration: a mean error (the origins' distance plus the largest difference of the 3x3) over at least kMinSamples pairs and
// within kFitMetres.
class WheelCalibration {
 public:
  static constexpr uint32_t kMinSamples = 8;
  static constexpr double kFitMetres = crt::PoseCalibration::kFitMetres;
  using Tally = crt::PoseCalibration::Tally;

  void Add(bool rim, const cw::WheelMatrices& m, const bool present[cw::kWheelCount], uint32_t bone, const tp::Mat4& drawn) {
    bool any_present = false;
    for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
      any_present = any_present || present[w];
    }
    for (uint32_t h = 0; h < cw::kWheelPoseCount; ++h) {
      bool found = false;
      crt::PlacementError best;
      double best_total = 1.0e18;
      for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
        tp::Mat4 placed;
        if ((any_present && !present[w]) || !cw::PlaceWheel(m, w, cw::WheelPose(h), bone, &placed) || !crt::UsablePlacement(placed)) {
          continue;
        }
        const crt::PlacementError e = crt::ComparePlacements(placed, drawn);
        if (e.Total() < best_total) {
          best_total = e.Total();
          best = e;
          found = true;
        }
      }
      if (found) {
        Tally& t = tally_[rim ? 1 : 0][h];
        ++t.n;
        t.sum += best.translation;
        t.basis_sum += best.basis;
        t.worst = std::max(t.worst, best.translation);
      }
    }
  }
  // Decides one kind from what was added; true when a hypothesis was chosen.
  bool Decide(bool rim) {
    const uint32_t k = rim ? 1 : 0;
    uint32_t best = cw::kWheelPoseCount;
    double best_mean = 1.0e18;
    for (uint32_t h = 0; h < cw::kWheelPoseCount; ++h) {
      if (tally_[k][h].n >= kMinSamples && tally_[k][h].Mean() < best_mean) {
        best = h;
        best_mean = tally_[k][h].Mean();
      }
    }
    if (best == cw::kWheelPoseCount) {
      return false;
    }
    chosen_[k] = cw::WheelPose(best);
    fits_[k] = best_mean <= kFitMetres;
    decided_[k] = true;
    return true;
  }
  cw::WheelPose chosen(bool rim) const { return decided_[rim ? 1 : 0] ? chosen_[rim ? 1 : 0] : cw::DefaultWheelPose(rim); }
  bool decided(bool rim) const { return decided_[rim ? 1 : 0]; }
  bool fits(bool rim) const { return decided_[rim ? 1 : 0] && fits_[rim ? 1 : 0]; }
  const Tally& tally(bool rim, cw::WheelPose pose) const { return tally_[rim ? 1 : 0][size_t(pose)]; }
  uint32_t samples(bool rim) const {
    uint32_t n = 0;
    for (const Tally& t : tally_[rim ? 1 : 0]) {
      n = std::max(n, t.n);
    }
    return n;
  }
  void NewWindow() { tally_ = {}; }
  std::string Describe(bool rim) const {
    std::string out;
    for (uint32_t h = 0; h < cw::kWheelPoseCount; ++h) {
      const Tally& t = tally_[rim ? 1 : 0][h];
      out += std::format("{}{}: {}", out.empty() ? "" : "; ", cw::WheelPoseName(cw::WheelPose(h)),
                         t.n == 0 ? std::string("no pair") : std::format("{} pairs, mean {:.3f} m + basis {:.3f}, worst {:.3f} m", t.n, t.sum / double(t.n), t.basis_sum / double(t.n), t.worst));
    }
    return out;
  }

 private:
  std::array<std::array<Tally, cw::kWheelPoseCount>, 2> tally_{};
  std::array<cw::WheelPose, 2> chosen_{cw::DefaultWheelPose(false), cw::DefaultWheelPose(true)};
  std::array<bool, 2> decided_{};
  std::array<bool, 2> fits_{};
};

// The placement among the four wheels' under `pose` that is nearest a draw's model-to-world (which wheel a draw is of is not told: the draws of one wheel mesh are four):
// the proof's instance of a wheel geometry. False when no wheel could be placed.
inline bool BestWheelPlacement(const cw::WheelMatrices& m, const bool present[cw::kWheelCount], uint32_t bone, cw::WheelPose pose, const tp::Mat4& drawn, tp::Mat4* out) {
  bool any_present = false;
  for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
    any_present = any_present || present[w];
  }
  bool found = false;
  double best = 1.0e18;
  for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
    tp::Mat4 placed;
    if ((any_present && !present[w]) || !cw::PlaceWheel(m, w, pose, bone, &placed) || !crt::UsablePlacement(placed)) {
      continue;
    }
    const double total = crt::ComparePlacements(placed, drawn).Total();
    if (total < best) {
      best = total;
      *out = placed;
      found = true;
    }
  }
  return found;
}

// The parts a wheel geometry brings: one placement per wheel the vehicle draws (`present`), under the pose the calibration chose for its kind. A wheel whose matrix was not
// readable or is not a part's size is left out.
inline std::vector<tp::Mat4> WheelPlacements(const cw::WheelMatrices& m, const bool present[cw::kWheelCount], bool rim, uint32_t bone, cw::WheelPose pose) {
  bool any_present = false;
  for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
    any_present = any_present || present[w];
  }
  std::vector<tp::Mat4> out;
  for (uint32_t w = 0; w < cw::kWheelCount; ++w) {
    tp::Mat4 placed;
    if ((any_present && !present[w]) || !cw::PlaceWheel(m, w, pose, bone, &placed) || !crt::UsablePlacement(placed)) {
      continue;
    }
    out.push_back(placed);
  }
  (void)rim;
  return out;
}

// ---------------------------------------------------------------------------
// the traffic: which hypothesis places a bone
// ---------------------------------------------------------------------------

class TrafficCalibration {
 public:
  static constexpr uint32_t kMinSamples = 12;
  static constexpr double kFitMetres = crt::PoseCalibration::kFitMetres;
  using Tally = crt::PoseCalibration::Tally;

  void Add(const cw::TrafficPoseData& pose, uint32_t bone, const tp::Mat4& drawn) {
    for (uint32_t h = 0; h < cw::kTrafficPoseCount; ++h) {
      tp::Mat4 placed;
      if (!cw::PlaceTraffic(pose, bone, cw::TrafficPose(h), &placed) || !crt::UsablePlacement(placed)) {
        continue;
      }
      const crt::PlacementError e = crt::ComparePlacements(placed, drawn);
      Tally& t = tally_[h];
      ++t.n;
      t.sum += e.translation;
      t.basis_sum += e.basis;
      t.worst = std::max(t.worst, e.translation);
    }
  }
  bool Decide() {
    uint32_t best = cw::kTrafficPoseCount;
    double best_mean = 1.0e18;
    for (uint32_t h = 0; h < cw::kTrafficPoseCount; ++h) {
      if (tally_[h].n >= kMinSamples && tally_[h].Mean() < best_mean) {
        best = h;
        best_mean = tally_[h].Mean();
      }
    }
    if (best == cw::kTrafficPoseCount) {
      return false;
    }
    chosen_ = cw::TrafficPose(best);
    fits_ = best_mean <= kFitMetres;
    decided_ = true;
    return true;
  }
  cw::TrafficPose chosen() const { return decided_ ? chosen_ : cw::kDefaultTrafficPose; }
  bool decided() const { return decided_; }
  bool fits() const { return decided_ && fits_; }
  uint32_t samples() const {
    uint32_t n = 0;
    for (const Tally& t : tally_) {
      n = std::max(n, t.n);
    }
    return n;
  }
  void NewWindow() { tally_ = {}; }
  std::string Describe() const {
    std::string out;
    for (uint32_t h = 0; h < cw::kTrafficPoseCount; ++h) {
      const Tally& t = tally_[h];
      out += std::format("{}{}: {}", out.empty() ? "" : "; ", cw::TrafficPoseName(cw::TrafficPose(h)),
                         t.n == 0 ? std::string("no pair") : std::format("{} pairs, mean {:.3f} m + basis {:.3f}, worst {:.3f} m", t.n, t.sum / double(t.n), t.basis_sum / double(t.n), t.worst));
    }
    return out;
  }

 private:
  std::array<Tally, cw::kTrafficPoseCount> tally_{};
  cw::TrafficPose chosen_ = cw::kDefaultTrafficPose;
  bool decided_ = false;
  bool fits_ = false;
};

// ---------------------------------------------------------------------------
// which cars
// ---------------------------------------------------------------------------

// The traffic's count: the class's count by default (the debug override -1), 0 turns the traffic off, N asks for N (at most ci::kMaxCars); none where the other cars are
// off. A traffic car costs a fraction of a racer's (see the header), so the class's number is the same count with room to spare; this is the one place to give it a number
// of its own.
inline uint32_t TrafficCarsFor(const ci::Limits& limits, int override_cars) {
  if (!limits.On()) {
    return 0;
  }
  if (override_cars < 0) {
    return limits.cars;
  }
  return uint32_t(std::min<int>(override_cars, int(ci::kMaxCars)));
}

// The racers' cars and the traffic's, each within its own count, merged nearest first (at most crt::kMaxWalkedCars: the stencil IDs of the walked cars).
inline std::vector<uint32_t> ChoosePlacedAll(const std::vector<crt::CarInfo>& cars, const ci::Limits& limits, crt::Mode mode, const std::vector<ci::OtherCar>& remembered,
                                             uint32_t traffic_cars) {
  std::vector<uint32_t> order = crt::ChoosePlaced(cars, limits, mode, remembered, false, 0);
  if (traffic_cars != 0) {
    const std::vector<uint32_t> traffic = crt::ChoosePlaced(cars, limits, mode, remembered, true, traffic_cars);
    order.insert(order.end(), traffic.begin(), traffic.end());
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cars[a].distance < cars[b].distance; });
  if (order.size() > crt::kMaxWalkedCars) {
    order.resize(crt::kMaxWalkedCars);
  }
  return order;
}

// The cars a frame stages (reads the triangles of), by the same two counts and a spare each.
inline std::vector<uint32_t> ChooseStagedAll(const std::vector<crt::CarInfo>& cars, const ci::Limits& limits, uint32_t traffic_cars) {
  std::vector<uint32_t> order = crt::ChooseStaged(cars, limits, false, 0);
  if (traffic_cars != 0) {
    const std::vector<uint32_t> traffic = crt::ChooseStaged(cars, limits, true, traffic_cars);
    order.insert(order.end(), traffic.begin(), traffic.end());
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cars[a].distance < cars[b].distance; });
  return order;
}

}  // namespace mcla::gpu::car_model_parts
