#pragma once

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// PSSeedRTNoZ uses the glow texture as a mask before multiplying by the
// sprite gain and vertex tint. BC1 stores neutral gradients with unequal red,
// green and blue precision. Estimate neutral intensity from the finer green
// channel while retaining texture colors outside the quantization range.
inline constexpr uint32_t kGlowNeutralVariantVersion = 1;
inline constexpr VariantShader kGlowNeutralSeed = {"xrage_postfx__PSSeedRTNoZ", 0x35F41762995C91B9ull};

// In 8-bit UNORM levels, allow one 5-bit step plus rounding. Fade the
// correction out over a second step so mildly colored edges stay smooth.
inline constexpr int kGlowNeutralFullLevels = 9;
inline constexpr int kGlowNeutralNoneLevels = 18;

VariantDef MakeGlowNeutralVariant();
VariantTransformResult TransformGlowNeutralHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack
