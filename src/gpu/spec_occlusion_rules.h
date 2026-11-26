#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::spec_occlusion {

// CarSelfReflConstants::c[3]. The customizable paint's material smoothness is 0.85.
inline constexpr uint32_t kPaintFlag = 8u;
inline constexpr float kPaintRoughness = 0.15f * 0.15f;

// Lagarde and de Rousiers, Moving Frostbite to PBR, section 4.10.2. Roughness is GGX alpha.
// Visibility is 1 in the open and 0 when fully occluded. Missing AO is neutral.
inline float Visibility(float no_v, float ao, float roughness) {
  if (!(ao < 1.0f)) {
    return 1.0f;
  }
  if (ao <= 0.0f) {
    return 0.0f;
  }
  const float exponent = std::exp2(-16.0f * std::clamp(roughness, 0.0f, 1.0f) - 1.0f);
  const float lobe = std::pow(std::clamp(no_v, 0.0f, 1.0f) + ao, exponent);
  return std::clamp(lobe - 1.0f + ao, 0.0f, 1.0f);
}

}  // namespace mcla::gpu::spec_occlusion
