#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::rtao_denoise {

inline constexpr float kMinTapWeight = 0.25f;
inline constexpr float kMinNormalDot = 0.8f;
inline constexpr float kBoxFloor = 0.2f;
inline constexpr uint32_t kDefaultCap = 32;
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

constexpr uint32_t HistoryCap(int setting) {
  return uint32_t(std::clamp(setting, 1, 64));
}

constexpr bool CanReadHistory(bool guides_valid, bool recorded, uint64_t frame, uint64_t previous_frame, uint32_t cap, uint32_t previous_cap) {
  return guides_valid && recorded && frame > previous_frame && frame - previous_frame == 1 && cap == previous_cap;
}

// Word 23 uses the low byte alongside the 24-bit ID mask. Word 24 carries
// the rest of the slot since the bindless heap has 32768 entries.
constexpr uint32_t VehicleWord(uint32_t slot, uint32_t ids) {
  return (slot & 0xFFu) | (ids << 8u);
}
constexpr uint32_t VehicleSlot(uint32_t word, uint32_t high) {
  return (word & 0xFFu) | (high << 8u);
}

inline bool AcceptTap(float depth, float expected, bool same_vehicle, float normal_dot) {
  return same_vehicle && normal_dot >= kMinNormalDot && std::abs(depth - expected) <= 0.04f * expected + 0.02f;
}
inline float MotionCap(float speed, float user_cap) {
  return std::min(user_cap, std::clamp(32.0f / (1.0f + 1.2f * speed), 4.0f, 32.0f));
}
inline float HistoryLength(float weighted_length, float weights, float cap) {
  if (weights < kMinTapWeight) return 0.0f;
  return std::min(weighted_length / weights * std::min(1.0f, 2.0f * weights), cap);
}
inline float Clip(float history, float mean, float sigma, float lo, float hi) {
  const float radius = std::max(1.25f * sigma, kBoxFloor);
  const float padding = (hi - lo) * 0.1f;
  return std::clamp(history, std::max(mean - radius, lo - padding), std::min(mean + radius, hi + padding));
}
constexpr float Blend(float history, float current, float length) {
  return std::lerp(history, current, 1.0f / (length + 1.0f));
}
constexpr float NextLength(float length, float cap) {
  return std::min(length + 1.0f, cap);
}
inline float UpsampleWeight(float bilinear, float depth, float receiver, bool same_surface) {
  if (!same_surface) return 0.0f;
  const float dz = (depth - receiver) / std::max(0.02f * receiver, 0.05f);
  return bilinear * std::exp(-dz * dz);
}
inline float Term(float open, float coverage, float screen, float power, float floor, bool blend_screen) {
  const float rt = std::max(std::pow(std::clamp(open, 0.0f, 1.0f), power), floor);
  return std::lerp(screen, blend_screen ? std::min(rt, screen) : rt, coverage);
}

}  // namespace mcla::gpu::rtao_denoise
