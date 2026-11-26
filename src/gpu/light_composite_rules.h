#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "light_frame.h"

namespace mcla::gpu::light_composite {

constexpr bool Smooth(const larecomp::gpu::Light& light, bool in_capture) {
  using namespace larecomp::gpu;
  return !in_capture && light.shadow.kind == ShadowKind::kNone &&
         (light.flags & (kLightFlagGameLit | kLightFlagRtVisibility)) == 0 &&
         light.source != LightSource::kHeadlight && light.source != LightSource::kLamp &&
         (light.law == LightLaw::kOmni || light.law == LightLaw::kNeon);
}

constexpr bool WorldVirtual(bool street, bool screen_pair, float field_share) {
  return !screen_pair && (!street || field_share < 1.0f);
}

inline float TapWeight(float receiver, float guide, bool vehicle, float bilinear) {
  if (guide == 0.0f || (guide < 0.0f) != vehicle) return 0.0f;
  return bilinear * std::clamp(1.0f - std::abs(std::abs(guide) - receiver) /
                                     std::max(0.20f, receiver * 0.018f), 0.0f, 1.0f);
}

constexpr float Apply(float albedo, float lit, float direct, float bounce, float ao) {
  return (albedo * lit) * (direct + ao * bounce);
}

}  // namespace mcla::gpu::light_composite
