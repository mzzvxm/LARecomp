#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "car_rt_opp_refl_rules.h"

namespace mcla::gpu::car_rt_refl {

using PaintRect = car_rt_opp_refl::TexelRect;
using PaintRects = std::vector<PaintRect>;
inline constexpr int32_t kPaintTile = 16;

inline bool CompleteBox(const car_rt_instances::ViewBox& box) {
  if (!box.valid || !box.complete) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(box.min[axis]) || !std::isfinite(box.max[axis]) || box.min[axis] > box.max[axis]) {
      return false;
    }
  }
  return true;
}

inline PaintRect PaintTilesForBox(const car_rt_instances::ViewBox& box, double tan_x, double tan_y, double render_w,
                                  double render_h, double divisor, PaintRect full) {
  if (!CompleteBox(box) || !std::isfinite(tan_x) || !std::isfinite(tan_y) || tan_x <= 0.0 || tan_y <= 0.0 ||
      (box.min[2] <= kMinDepth && box.max[2] >= 0.001f)) {
    return full;  // The primary ray can start closer than the box projection's near plane.
  }
  auto rect = car_rt_opp_refl::BoxRect(box, tan_x, tan_y, render_w, render_h, divisor, full.x1, full.y1);
  if (rect.Empty()) {
    return {};
  }
  rect.x0 = rect.x0 / kPaintTile * kPaintTile;
  rect.y0 = rect.y0 / kPaintTile * kPaintTile;
  rect.x1 = std::min(full.x1, (rect.x1 + kPaintTile - 1) / kPaintTile * kPaintTile);
  rect.y1 = std::min(full.y1, (rect.y1 + kPaintTile - 1) / kPaintTile * kPaintTile);
  return rect;
}

// A reused UAV still holds the previous footprint. Rerun it to zero texels
// vacated by a moving car. The first write initializes the entire allocation.
// Keep only this frame's potential writes for the next reuse of this image.
struct PaintCoverage {
  uint64_t serial = 0;
  PaintRects previous;
};

inline PaintRects PaintDispatch(PaintCoverage& coverage, uint64_t serial, const PaintRects& current, PaintRect full, bool tiled) {
  if (!tiled || coverage.serial != serial) {
    coverage.serial = serial;
    coverage.previous = tiled ? current : PaintRects{full};
    return {full};
  }
  PaintRects dirty = coverage.previous;
  dirty.insert(dirty.end(), current.begin(), current.end());
  coverage.previous = current;
  // Merge overlaps before recording dispatches. No pairwise UAV write hazards.
  return car_rt_opp_refl::MergeRects(std::move(dirty), std::numeric_limits<uint32_t>::max());
}

}  // namespace mcla::gpu::car_rt_refl
