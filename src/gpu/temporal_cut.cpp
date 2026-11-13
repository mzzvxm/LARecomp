// MCLA GPU renderer -- the camera cut rule. See temporal_camera.h for what it decides and why.
//
// Pure: nothing here touches the GPU, the guest or the log, so camera_cut_test.cpp builds it alone.

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "temporal_camera.h"

namespace mcla::gpu::temporal {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegrees = 180.0 / kPi;
// The lens when neither camera has one (the occlusion's hand-made camera has no near plane or
// axes): the game's chase camera, 25 degrees, 16:9.
constexpr double kDefaultFov = 25.0 / kDegrees;
constexpr double kDefaultAspect = 16.0 / 9.0;

double Dot(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Length(const double* a) { return std::sqrt(Dot(a, a)); }

// A camera made from a position and a forward vector only (the occlusion's) has no right or up.
bool HasAxes(const Camera& c) { return Length(c.right) > 0.5 && Length(c.up) > 0.5; }

// The angle between two cameras' orientations. With axes it is the whole rotation: the trace of
// R * R' of the rows (right, up, back) is 1 + 2 cos(angle), and back . back' = forward . forward',
// so a roll counts and the handedness does not matter. Without axes, the forward vectors' angle.
// `*forward_cosine` is the forward vectors' cosine either way.
double Rotation(const Camera& now, const Camera& before, double* forward_cosine) {
  const double length = Length(now.forward) * Length(before.forward);
  const double forward = length > 1e-30 ? Dot(now.forward, before.forward) / length : 1.0;
  *forward_cosine = std::clamp(forward, -1.0, 1.0);
  if (HasAxes(now) && HasAxes(before)) {
    const double trace = Dot(now.right, before.right) + Dot(now.up, before.up) + forward;
    return std::acos(std::clamp((trace - 1.0) * 0.5, -1.0, 1.0));
  }
  return std::acos(*forward_cosine);
}

// The least of |value - earlier| over the steps known; with none, the camera is predicted to have
// stood still, so the whole value is the deviation.
double ScalarDeviation(double value, const double* earlier, int count) {
  if (count <= 0) {
    return std::fabs(value);
  }
  double least = std::fabs(value - earlier[0]);
  for (int i = 1; i < count; ++i) {
    least = std::min(least, std::fabs(value - earlier[i]));
  }
  return least;
}

// The largest of the steps known. With `skip_cuts` the ones that were cuts are left out: what the
// camera was really doing, which a teleport must not widen.
double Largest(const double* values, const bool* was_cut, int count, bool skip_cuts) {
  double largest = 0.0;
  for (int i = 0; i < count; ++i) {
    if (!(skip_cuts && was_cut[i])) {
      largest = std::max(largest, std::fabs(values[i]));
    }
  }
  return largest;
}

}  // namespace

void CutMotion::Accept(const Cut& result) {
  for (int i = 0; i < 3; ++i) {
    step[1][i] = step[0][i];
    step[0][i] = result.step[i];
  }
  turn[1] = turn[0];
  turn[0] = result.turn;
  fov[1] = fov[0];
  fov[0] = result.fov_change;
  cut[1] = cut[0];
  cut[0] = result.fires;
  steps = std::min(steps + 1, 2);
}

Cut TestCut(const Camera& now, const Camera& before, const CutMotion& motion) {
  Cut cut;
  const int known = std::clamp(motion.steps, 0, 2);

  // The lens the step is judged against: the frame before's (the one the vectors reproject from).
  const double fov = before.fov_y > 0.0 ? before.fov_y : (now.fov_y > 0.0 ? now.fov_y : kDefaultFov);
  const double tan_half = std::tan(0.5 * fov);
  const double aspect = before.aspect > 0.0 ? before.aspect : kDefaultAspect;

  // JUMP: the position step against the nearest of the last steps.
  double distance2 = 0.0;
  for (int i = 0; i < 3; ++i) {
    cut.step[i] = now.position[i] - before.position[i];
    distance2 += cut.step[i] * cut.step[i];
  }
  cut.distance = std::sqrt(distance2);
  cut.deviation = cut.distance;  // predicted to stand still, unless a step before says otherwise
  cut.predicted = 0.0;
  double longest = 0.0;  // the longest of the steps before, cuts too
  double recent = 0.0;   // ... without the cuts: what the camera was doing
  for (int k = 0; k < known; ++k) {
    double away2 = 0.0;
    for (int i = 0; i < 3; ++i) {
      const double d = cut.step[i] - motion.step[k][i];
      away2 += d * d;
    }
    const double length = Length(motion.step[k]);
    longest = std::max(longest, length);
    if (!motion.cut[k]) {
      recent = std::max(recent, length);
    }
    const double away = std::sqrt(away2);
    if (k == 0 || away < cut.deviation) {
      cut.deviation = away;
      cut.predicted = length;
    }
  }
  const double depth = std::max({kCutDepth, before.near_plane, now.near_plane});
  cut.floor = kCutShift * 2.0 * tan_half * depth;
  cut.tolerance = cut.floor + kCutSpeedTolerance * recent;
  // A jump is a step longer than the camera has just taken (a camera that slows or stops after a
  // jump, or after a frame-time stall's long step, is not making another) and not one of the steps
  // before again (a camera that goes on moving the same way).
  cut.by_distance = cut.distance > cut.floor && cut.distance > longest && cut.deviation > cut.tolerance;

  // TURN: the whole rotation, against the nearest of the last turns.
  cut.turn = Rotation(now, before, &cut.cosine);
  const double fov_x = 2.0 * std::atan(aspect * tan_half);
  cut.turn_floor = std::clamp(kCutTurnFraction * fov_x, kCutTurnMin, kCutTurnMax);
  cut.turn_deviation = ScalarDeviation(cut.turn, motion.turn, known);
  cut.turn_tolerance = cut.turn_floor + kCutSpeedTolerance * Largest(motion.turn, motion.cut, known, true);
  cut.by_turn = cut.turn > cut.turn_floor && cut.turn > Largest(motion.turn, motion.cut, known, false) &&
                cut.turn_deviation > cut.turn_tolerance;

  // LENS: the relative change of the vertical field of view, against the nearest of the last ones.
  cut.fov_change = before.fov_y > 0.0 ? std::fabs(now.fov_y - before.fov_y) / before.fov_y : 0.0;
  cut.fov_floor = kCutFovFloor;
  cut.fov_deviation = ScalarDeviation(cut.fov_change, motion.fov, known);
  cut.fov_tolerance = cut.fov_floor + kCutSpeedTolerance * Largest(motion.fov, motion.cut, known, true);
  cut.by_fov = cut.fov_change > cut.fov_floor && cut.fov_change > Largest(motion.fov, motion.cut, known, false) &&
               cut.fov_deviation > cut.fov_tolerance;

  cut.fires = cut.by_distance || cut.by_turn || cut.by_fov;
  return cut;
}

Cut TestCut(const Camera& now, const Camera& before, const double* previous_step) {
  CutMotion motion;
  if (previous_step) {
    motion.steps = 1;
    for (int i = 0; i < 3; ++i) {
      motion.step[0][i] = previous_step[i];
    }
  }
  return TestCut(now, before, motion);
}

std::string CutReason(const Cut& cut) {
  std::string text;
  const auto add = [&text](std::string clause) {
    if (!text.empty()) {
      text += "; ";
    }
    text += clause;
  };
  if (cut.by_distance) {
    add(std::format("jump: moved {:.1f}, {:.1f} away from the {:.1f} the steps before predict "
                    "(allowed {:.1f}, floor {:.1f})",
                    cut.distance, cut.deviation, cut.predicted, cut.tolerance, cut.floor));
  }
  if (cut.by_turn) {
    add(std::format("turn: {:.1f} deg, {:.1f} beyond what the steps before explain (allowed "
                    "{:.1f}, floor {:.1f}; forward cosine {:.4f})",
                    cut.turn * kDegrees, cut.turn_deviation * kDegrees, cut.turn_tolerance * kDegrees,
                    cut.turn_floor * kDegrees, cut.cosine));
  }
  if (cut.by_fov) {
    add(std::format("lens: field of view changed {:.1f}%, {:.1f} beyond what the steps before "
                    "explain (allowed {:.1f}, floor {:.0f})",
                    100.0 * cut.fov_change, 100.0 * cut.fov_deviation, 100.0 * cut.fov_tolerance,
                    100.0 * cut.fov_floor));
  }
  return text.empty() ? std::string("no clause fired") : text;
}

}  // namespace mcla::gpu::temporal
