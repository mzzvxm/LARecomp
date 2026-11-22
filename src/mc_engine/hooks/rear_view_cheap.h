#pragma once
// ===========================================================================
// The rear view mirror's cheap options: the rules (perf/MIRRORS-SURVEY.md section 4, B to E)
// ===========================================================================
// rear_view.cpp holds the hooks; what each one decides is here, as pure functions on plain numbers, so a test needs no game (rear_view_cheap_test.cpp).
// Every option is a debug setting that is off by default (rearview_far, rearview_fov_scale, rearview_lod_distance, rearview_cars_max, rearview_cars_lod,
// rearview_every): the rules below return their input unchanged for the off value.
//
//   B  the mirror camera: the field of view scaled down (the stamp is 5.2:1, so the game's field makes it a very wide view), the far plane pulled in;
//   C  the city's hi detail radius while the mirror's phase runs (a sector beyond it is the coarse one-model-a-sector city);
//   D  the racers: the first N that the mirror's camera sees, and at least a given model level;
//   E  every Nth frame: the phase is not asked for on the others, unless the camera cut.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace larecomp::rearview_cheap {

inline constexpr float kMinFarMetres = 20.0f;        // a far plane under this would show nothing: the setting is raised to it
inline constexpr double kMinFovScale = 0.1;          // the setting's own range
inline constexpr float kCutMetres = 20.0f;           // the eye moved more than this between two frames: a camera cut
inline constexpr int kLodCoarsest = 2;               // the car's model levels are 0 (finest) to 2

// B. The far plane: the game's own (the main camera's), pulled in to `wanted` metres when that is set; never pushed out, never under kMinFarMetres.
// A stock value that is not a positive number is left alone (the register did not hold what this expects).
inline float FarPlane(float stock, double wanted) {
  if (!(wanted > 0.0) || !std::isfinite(wanted) || !(stock > 0.0f) || !std::isfinite(stock)) {
    return stock;
  }
  return std::min(stock, std::max(float(wanted), kMinFarMetres));
}

// B. The field of view: the game's, scaled by `scale` (0.1 to 1.0). 1.0 or more, and anything that is not a number, leave it: this only narrows.
inline float FieldOfView(float stock, double scale) {
  if (!(scale > 0.0) || !(scale < 1.0) || !std::isfinite(scale) || !(stock > 0.0f) || !std::isfinite(stock)) {
    return stock;
  }
  return stock * float(std::max(scale, kMinFovScale));
}

// C. The hi detail radius of a city sector: the game's, cut to `wanted` metres when that is set. Never raised.
inline float LodRadius(float stock, double wanted) {
  if (!(wanted > 0.0) || !std::isfinite(wanted) || !std::isfinite(stock)) {
    return stock;
  }
  return std::min(stock, float(wanted));
}

// D. A car's model level (0 finest, 2 coarsest) at least `at_least`: 0 leaves the game's choice; a level outside 0..2 (a car the game marked as not drawn)
// is left alone.
inline int CarLevel(int chosen, int at_least) {
  if (at_least <= 0 || chosen < 0 || chosen > kLodCoarsest) {
    return chosen;
  }
  return std::max(chosen, std::min(at_least, kLodCoarsest));
}

// D. The racers drawn in one run of the mirror's phase: Allow() says whether one more may be, at most `max` (0: no limit). Reset at the phase's start.
struct CarBudget {
  uint32_t drawn = 0;
  uint32_t turned_away = 0;
  void Reset() {
    drawn = 0;
    turned_away = 0;
  }
  bool Allow(int max) {
    if (max <= 0 || drawn < uint32_t(max)) {
      ++drawn;
      return true;
    }
    ++turned_away;
    return false;
  }
};

// E. The camera cut rule: the eye moved more than kCutMetres between two looks, or either look is not a position. No look before (the first) is a cut.
inline bool EyeJumped(const float before[3], const float now[3], bool have_before) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(now[i])) {
      return true;
    }
  }
  if (!have_before) {
    return true;
  }
  double sum = 0.0;
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(before[i])) {
      return true;
    }
    const double d = double(now[i]) - double(before[i]);
    sum += d * d;
  }
  return sum > double(kCutMetres) * double(kCutMetres);
}

// E. Every Nth frame. Decide() is asked once for every frame on which the mirror is up and answers whether to draw it; the frames in between are held.
// Reset() when the mirror was not up (the first frame after it is always drawn). A cut draws at once and the count starts again from it.
struct Cadence {
  uint32_t held = 0;       // frames held since the last drawn one
  bool fresh = true;       // nothing drawn since Reset
  uint64_t drawn = 0;      // totals, for the log
  uint64_t skipped = 0;
  uint64_t cut_draws = 0;  // drawn out of turn because the camera cut

  void Reset() {
    held = 0;
    fresh = true;
  }
  bool Decide(int every, bool cut) {
    const bool due = every <= 1 || fresh || held + 1 >= uint32_t(every);
    if (due || cut) {
      if (!due) {
        ++cut_draws;
      }
      held = 0;
      fresh = false;
      ++drawn;
      return true;
    }
    ++held;
    ++skipped;
    return false;
  }
};

}  // namespace larecomp::rearview_cheap
