#pragma once
// ===========================================================================
// MCLA GPU renderer -- the occlusion's and the bounce light's cheaper shader maths (pure maths, no GPU)
// ===========================================================================
// R5 of perf/GFX-COST-AUDIT.md: the main pass (hlsl/ao_main_ps.hlsl), the blurs (hlsl/ao_common.hlsli AoBilateral and
// AoBilateral4, ao_denoise_ps, ao_temporal_ps) and the a-trous filter (ao_atrous_ps) do the same work for less. What is the same
// work to a rounding error, and what is an approximation, is kept apart here, with a program that checks both
// (ao_fast_test.cpp: it needs no renderer, no ReXGlue and no GPU; given the src/gpu folder it also checks that the shaders'
// constants and the variants' compile-time counts are this file's). The shaders repeat the constants (marked "ao_fast.h" in
// their comments: change them here first).
//
// THE SAME WORK (no switch; the result is the same to a float's rounding):
//   - the counts. The slices, steps and the far ring's steps are compile-time constants of the pipeline for the two qualities
//     (3 slices of 3 steps, 2 of 2, with 2 and 1 ring steps a side): the loops over the steps (and the ring's) are unrolled and
//     everything that is a function of the counts is folded; the slice loop keeps its (constant) count as a loop, because the
//     bitmask pass unrolled over the slices is 15000 DXIL instructions (5000 with the loop: -DAO_UNROLL_SLICES=1 makes the other
//     one, for a GPU that measures it faster). A count that no variant has (a debug override of the ring's steps) runs the
//     compilation that reads the push block, as before. The variants are listed with the host precache where the pass is on.
//   - the divisions. dxc does not take a division out of a loop and folds a given 1 / size and the clip space's 2 back into a 2 / size
//     in the tap, so every tap of a blur, of the a-trous filter and of the main pass made two divisions for the position
//     (AoViewPos), one for the plane's sigma and one for the spatial sigma, and each of the 3 components of a sample's direction a
//     division by its length. They are now factors made once for the pass (AoViewPosInv with 2 / size; 1 / sigma) and one reciprocal
//     for the length: the main pass's step loop goes from 11 divisions to 3, the a-trous filter from 78 to 17, the bounce
//     blur's tap from 4 to none.
//   - the exponentials. A blur tap's weights exp(-0.5 a) exp(-0.5 b) are one exponential, exp(-0.5 (a + b)): a 7x7 blur makes 49
//     exponentials, not 98 (the bounce blur's tap: 2 exponentials and 4 divisions become 1 exponential).
//   - the logarithm and the cosines. The level of the depth chain a sample reads is 0 under 16 pixels, 1 under 32, 2 from there
//     (log2(length) - 3, cut to 0 to 2 and truncated): a compare of the length, without the square root and the logarithm; the
//     surface's tangent on each side is -sin(n) and sin(n), not the cosine of n plus or minus a quarter turn.
// ao_fast_test.cpp shows the largest difference of each on the C++ twins (ao_halo.h Visibility and ao_stable_twin.h GiMain with
// their `lossless` flag) and checks that the shaders have the forms the twins have; the shaders themselves, old and new, were
// run on the D3D12 software rasteriser (WARP) over a road, a wall and a car at 1280x720 and 3840x2160: the denoise passes differ
// by 4e-7 at the most, the a-trous filter by 4e-6, the plain pass by under 1e-4 in all but 1 pixel of 140,000 (a horizon step
// of one sample: 5 pixels of 8.3 million by more than 1/255), and the bitmask pass in 1 pixel of 13,000 to 17,000 by one sector of one
// slice (about 0.05 of the visibility), where a rounding error lands a sector's edge on the rounding point. The image's four edge
// lines are left out of that count: AoNormalFromImage has the same neighbour twice there, its normal is zero and the shader falls to a
// unit vector or to rounding noise, whatever the compilation.
//
// THE APPROXIMATION (debug cvar mcla_gpu_ao_fast_math, default true; false is the hardware's acos in the bounce light's sectors):
//   - the bounce light's sectors. Each sample of the bitmask pass takes the angle of its horizon's cosine (acos) to find its sector;
//     the polynomial of Abramowitz and Stegun 4.4.45 (a square root and three fused multiply-adds against the driver's longer
//     sequence for the hardware's acos) is 6.8e-5 radians at the most, which moves a sector's edge by 0.0014 of a sector at the
//     most (the arc's density is under 1 and its measure over 1: the angle's error times 32; ao_fast_test.cpp scans every arc and
//     horizon). An edge that was within that of a rounding point sets the neighbour's bit: on the WARP run, 1 pixel in 44,000
//     (720p) to 130,000 (4K) changes by one sector of one slice (visibility 0.05, the bounce 1/32 of a colour), as often up as down
//     (no bias: the mean of the error is 4e-8 radians), and the accumulation over frames averages it away.
// ===========================================================================

#include <cmath>
#include <cstdint>

#include "ssgi_light.h"

namespace mcla::gpu::ao_fast {

// ---------------------------------------------------------------------------
// the compile-time counts of the main pass's variants (ao.cpp FillPush: the occlusion's quality picks 3 slices of 3 steps, or 2 of
// 2; the bounce light's quality picks the same and the far ring's steps a side)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHighSlices = 3;
inline constexpr uint32_t kHighSteps = 3;
inline constexpr uint32_t kLowSlices = 2;
inline constexpr uint32_t kLowSteps = 2;
inline constexpr uint32_t kHighFarSteps = ssgi_light::kFarStepsHigh;  // 2
inline constexpr uint32_t kLowFarSteps = ssgi_light::kFarStepsLow;    // 1

// The variant a frame draws: the counts compiled in, or none that fits (the generic compilation, which reads them).
enum class Counts : uint8_t { kGeneric, kHigh, kLow };

// The plain pass: `low` is the occlusion's own quality (FillPush's ao_low with the bounce light off).
inline constexpr Counts PlainCounts(bool low) {
  return low ? Counts::kLow : Counts::kHigh;
}
// The bitmask pass: `fewer` is the bounce light's own quality (2 slices and 2 steps), `far_steps` what the push block says
// (0 when there is no ring, or a debug override).
inline constexpr Counts GiCounts(bool fewer, uint32_t far_steps) {
  if (!fewer && far_steps == kHighFarSteps) {
    return Counts::kHigh;
  }
  if (fewer && far_steps == kLowFarSteps) {
    return Counts::kLow;
  }
  return Counts::kGeneric;
}

// ---------------------------------------------------------------------------
// the exponential
// ---------------------------------------------------------------------------

// exp(-0.5 x) = exp2(x * kNegHalfLog2e) (hlsl/ao_common.hlsli kAoNegHalfLog2e).
inline constexpr float kNegHalfLog2e = -0.72134752044f;

// The weight of one tap of the blurs: the plane's weight times the spatial weight, one exponential for both.
//   off_plane   the tap's distance from the pixel's tangent plane over the plane's sigma
//   d2          dx * dx + dy * dy
//   inv_sq      1 / (spatial sigma)^2
inline float BlurWeight(float off_plane, float d2, float inv_sq) {
  return std::exp2(kNegHalfLog2e * (off_plane * off_plane + d2 * inv_sq));
}
// The first version's: two exponentials, one division by the sigma squared in the tap.
inline float BlurWeightTwoExp(float off_plane, float d2, float spatial_sigma) {
  const float w_depth = std::exp(-0.5f * off_plane * off_plane);
  const float w_space = std::exp(-0.5f * d2 / (spatial_sigma * spatial_sigma));
  return w_depth * w_space;
}

// ---------------------------------------------------------------------------
// the angle of a cosine
// ---------------------------------------------------------------------------

// Abramowitz and Stegun 4.4.45, for 0 <= c <= 1: acos(c) = sqrt(1 - c) (c0 + c1 c + c2 c^2 + c3 c^3), mirrored for a negative cosine
// (hlsl/ao_main_ps.hlsl AoAngleOf with AO_FAST_ACOS). The cosine is kept within -1 to 1 by the minimum, a NaN counts as 1.
inline constexpr float kAcosC0 = 1.5707288f;
inline constexpr float kAcosC1 = -0.2121144f;
inline constexpr float kAcosC2 = 0.0742610f;
inline constexpr float kAcosC3 = -0.0187293f;
inline constexpr float kPi = 3.14159265358979f;
// The bound the polynomial's error stays under, in radians (the paper's is 6.7e-5; the float coefficients measure 6.77e-5).
inline constexpr double kAcosMaxErrorRadians = 6.8e-5;

inline float AcosFast(float c) {
  // (std::fmin returns the non-NaN operand, as the shader's min does.)
  const float a = std::fmin(std::fabs(c), 1.0f);
  const float r = std::sqrt(1.0f - a) * (kAcosC0 + a * (kAcosC1 + a * (kAcosC2 + a * kAcosC3)));
  return c < 0.0f ? kPi - r : r;
}
// The hardware's, as the shader writes it without AO_FAST_ACOS: acos of the cosine kept within -1 to 1.
inline float AcosExact(float c) {
  return std::acos(std::fmax(std::fmin(c, 1.0f), -1.0f));
}

}  // namespace mcla::gpu::ao_fast
