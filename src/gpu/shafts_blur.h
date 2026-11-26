#pragma once

#include <algorithm>
#include <cmath>

namespace mcla::gpu::shafts_blur {

inline constexpr unsigned kTaps = 8;
inline constexpr float kLegacyDecay = 0.96f;
inline constexpr float kLegacyTaps = 64.0f;

// Each successive segment covers the same fraction of the remaining ray.
inline float SegmentRatio(float density) {
  return std::pow(std::max(1.0f - density, 1.0f / kLegacyTaps), 1.0f / kTaps);
}

// Integrate the legacy exponential over each segment. Normalize to the old
// discrete sum so fully open sky keeps the same gain.
inline float SegmentWeight(float from, float to, float density) {
  return (std::pow(kLegacyDecay, from * kLegacyTaps / density) -
          std::pow(kLegacyDecay, to * kLegacyTaps / density)) /
         (kLegacyTaps * (1.0f - kLegacyDecay));
}

}  // namespace mcla::gpu::shafts_blur
