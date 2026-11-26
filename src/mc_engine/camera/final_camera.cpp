#ifndef REXGLUE_HAS_XEO3_TARGET
// The final camera's latest pose: see final_camera.h.

#include "final_camera.h"

#include <chrono>
#include <mutex>

#include "settings/immortal.h"

namespace mc::final_camera {

namespace {

struct State {
  std::mutex mutex;
  Pose pose;
  std::chrono::steady_clock::time_point at;
  bool valid = false;
};

State& S() {
  static State& state = larecomp::Leak<State>();
  return state;
}

}  // namespace

void Publish(const Pose& pose) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pose = pose;
  s.at = std::chrono::steady_clock::now();
  s.valid = true;
}

bool Latest(Pose* pose, double* age_seconds) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.valid) return false;
  *pose = s.pose;
  *age_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.at).count();
  return true;
}

}  // namespace mc::final_camera
#endif  // REXGLUE_HAS_XEO3_TARGET
