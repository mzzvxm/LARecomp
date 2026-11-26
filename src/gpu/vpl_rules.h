#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::vpl {

inline constexpr uint32_t kMaxLights = 12;
inline constexpr uint32_t kBeamSteps = 28;
inline constexpr float kHistorySeconds = 0.075f;
inline constexpr float kBounceFraction = 0.25f;
inline constexpr float kNearCeiling = 0.35f;
inline constexpr float kVehicleShare = 0.35f;

inline float HistoryWeight(double seconds) {
  return seconds >= 0.0 && seconds <= 0.25 ? float(-std::expm1(-seconds / kHistorySeconds)) : 1.0f;
}

inline float GroundDistance(float height, float down, float range) {
  return std::min(range, height / std::max(down, 0.001f));
}

inline float StepDistance(float range, uint32_t index) {
  const float fraction = float(index) / float(kBeamSteps);
  return std::max(0.04f, range * fraction * fraction);
}

inline bool WithinDepthThickness(float beam, float surface) {
  return beam >= surface && beam - surface <= std::max(0.14f, surface * 0.018f);
}

inline float Energy(float incident, float albedo, float footprint) {
  return std::max(incident, 0.0f) * std::clamp(albedo, 0.0f, 1.0f) *
         (kBounceFraction * footprint * footprint);
}

inline float Falloff(float distance_sq, float footprint) {
  const float size_sq = footprint * footprint;
  const float window = std::clamp(1.0f - distance_sq / (9.0f * size_sq), 0.0f, 1.0f);
  return window * window / (distance_sq + size_sq);
}

inline float ReceiverShare(uint32_t receiver, uint32_t source, bool cabin) {
  if (cabin || (receiver != 0 && receiver == source)) return 0.0f;
  return receiver != 0 ? kVehicleShare : 1.0f;
}

struct alignas(16) Beam {
  std::array<float, 4> origin_range;
  std::array<float, 4> aim_ground;
  std::array<float, 4> colour_law;
  std::array<uint32_t, 4> meta;  // persistent slot, reset flag, source stencil ID, light law
};

struct alignas(16) Frame {
  std::array<float, 4> right;
  std::array<float, 4> up;
  std::array<float, 4> forward;
  std::array<float, 4> eye;   // xyz camera position, w scene exposure
  std::array<float, 4> lens;  // tan half FOV, history weight, AO strength
  std::array<float, 4> cabin_min;
  std::array<float, 4> cabin_max;
  std::array<uint32_t, 4> sizes;
  std::array<uint32_t, 4> counts;  // traced width, traced height, active beams, guest sample zero
  std::array<uint32_t, 4> slots;   // linear depth 0-2, raw depth
  std::array<uint32_t, 4> guides;  // RT2, main colour, AO visibility, stencil
  std::array<uint32_t, 4> apply;   // irradiance, cockpit ID, cockpit flag, reserved
  std::array<float, 4> depth;  // reversed-depth linearisation
  std::array<Beam, kMaxLights> beams;
};

static_assert(sizeof(Beam) == 64 && sizeof(Frame) == 208 + kMaxLights * sizeof(Beam));

}  // namespace mcla::gpu::vpl
