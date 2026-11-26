#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mcla::gpu::rt_scene {

struct Bounds {
  std::array<float, 3> lo{};
  std::array<float, 3> hi{};
  bool valid = false;
  bool complete = true;

  bool Usable() const { return valid && complete; }

  void Include(const float min_point[3], const float max_point[3], bool known = true) {
    if (!complete) return;
    if (!known || !min_point || !max_point) {
      complete = false;
      return;
    }
    for (int axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(min_point[axis]) || !std::isfinite(max_point[axis]) || min_point[axis] > max_point[axis]) {
        complete = false;
        return;
      }
      lo[axis] = valid ? std::min(lo[axis], min_point[axis]) : min_point[axis];
      hi[axis] = valid ? std::max(hi[axis], max_point[axis]) : max_point[axis];
    }
    valid = true;
  }

  void IncludeTransformed(const float min_point[3], const float max_point[3], const float transform[3][4],
                          bool known = true) {
    if (!complete) return;
    if (!known || !min_point || !max_point || !transform) {
      complete = false;
      return;
    }
    float view_lo[3], view_hi[3];
    for (int row = 0; row < 3; ++row) {
      double lower = transform[row][3];
      double upper = lower;
      for (int axis = 0; axis < 3; ++axis) {
        const double a = double(transform[row][axis]) * min_point[axis];
        const double b = double(transform[row][axis]) * max_point[axis];
        lower += std::min(a, b);
        upper += std::max(a, b);
      }
      view_lo[row] = float(lower);
      view_hi[row] = float(upper);
    }
    Include(view_lo, view_hi);
  }
};

}  // namespace mcla::gpu::rt_scene
