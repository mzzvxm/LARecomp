#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "temporal_math.h"

namespace mcla::gpu::car_rt_denoise_v2 {

inline constexpr uint32_t Iterations(bool glass) { return glass ? 2u : 3u; }
inline constexpr bool WritesTrace(uint32_t iteration, uint32_t count) { return ((iteration + count) & 1u) != 0u; }
inline constexpr uint32_t kTraceGuide = 1u << 30;
inline constexpr float kInstanceTravel = 2.0f;
inline constexpr float kInstanceMargin = 0.10f;

inline uint64_t Signature(std::initializer_list<uint32_t> values) {
  uint64_t hash = 14695981039346656037ull;
  for (uint32_t value : values) hash = (hash ^ value) * 1099511628211ull;
  return hash;
}

inline temporal::Mat4 InstanceMatrix(const float rows[3][4]) {
  temporal::Mat4 result = temporal::Identity();
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) result.m[row][col] = rows[col][row];
  }
  return result;
}

inline bool PreviousInstance(const temporal::Mat4& current, const temporal::Mat4& previous, temporal::Mat4& transform) {
  temporal::Mat4 inverse;
  if (!temporal::Inverse(current, &inverse)) return false;
  transform = temporal::Mul(inverse, previous);
  return temporal::Finite(transform);
}

inline bool UniqueMatch(float distance, float runner_up) {
  return distance <= kInstanceTravel && runner_up - distance >= kInstanceMargin;
}

// CPU twins of the signal-specific shader rules.
inline float HitWeight(float a, float b, bool glass) {
  if (a == 0.0f || b == 0.0f) return a == b ? 1.0f : glass ? 0.0f : 0.4f;
  const float tolerance = glass ? 0.03f + 0.025f * std::min(a, b) : 0.15f + 0.35f * std::min(a, b);
  const float difference = std::abs(a - b) / tolerance;
  return difference < 1.0f ? 1.0f - difference : 0.0f;
}

inline float SamplePixel(float texel, uint32_t divisor, uint32_t render) {
  return std::min(std::floor((texel + 0.5f) * divisor), float(render - 1)) + 0.5f;
}

inline float ImageTexel(float pixel, uint32_t divisor, uint32_t render) {
  const float last = float((render - 1) / divisor);
  const float last_pixel = SamplePixel(last, divisor, render);
  if (last_pixel < last * divisor + divisor / 2 + 0.5f && pixel > last_pixel - 1.0f) {
    return last + pixel - last_pixel;
  }
  return (pixel - (divisor / 2 + 0.5f)) / divisor;
}

inline bool IsConsecutive(uint64_t previous, uint64_t current, bool reset, bool same_signal) {
  return !reset && same_signal && current > previous && current - previous == 1;
}

}  // namespace mcla::gpu::car_rt_denoise_v2
