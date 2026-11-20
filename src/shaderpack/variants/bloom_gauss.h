#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "bloom_gauss": the bloom's Gaussians at a render scale
// ===========================================================================
// The bloom (the post chain's: scene -> PSInitialScaleBuffer 640x360 -> PSScaleBuffer 320x180 ->
// PSBrightPass -> PSGaussX and PSGaussY, 9 taps each -> the tone map adds it) blurs with immediate texel
// offsets, -4..4 of a 320x180 image. The translated shader divides an immediate offset by the host
// texture's own size (shader_common.h tfetch2D), and the host makes that image at the render scale S
// (it is a screen image, or at the output scale under an upscaler), so the taps are 1 / S as far apart
// in UV: the glow is 1 / S as wide on the screen (1080p: 0.67, 4K: 0.33), the console's by a third at 4K
// (feature-parity/GFX-UPGRADES-DESIGN.md 7.1; measured: a window's glow 24 px at 1x, 9 px at 3x).
//
// The variant of the two Gaussians (xrage_postfx__PSGaussX and PSGaussY) replaces each of the nine
// tfetch2D calls by a call of a helper that is the tap as an area average at the scale, S = 1 /
// g_InvResolutionScale of the shared constants (one scale per axis; the target's own, contract v7):
//
//     n = ceil(S)                                  the sub-taps of a tap
//     offset_j = (a, b) * S + dir * (j - (n - 1) / 2) * (S / n),   j = 0..n-1,   weight 1 / n
//
// where (a, b) is the game's offset and dir (1, 0) for PSGaussX, (0, 1) for PSGaussY. Scaling the offsets
// alone (n = 1) is not enough: the image under the Gaussian is S times finer, taps S texels apart with a
// one texel bilinear footprint skip S - 1 texels between them, and a small bright light (a lamp is one or
// two texels wide at 3x) is seen by one tap in S, so its glow breaks into a comb of dots. With the
// sub-taps a tap covers the S texels it stood for and the glow has the console's size at any scale.
// At S = 1 (within 1%) the helper makes the game's own call with the game's own offset, so the picture is
// the game's, bit for bit; at S < 1 (n = 1) the tap is the game's with its offset times S (the console's
// size at a render scale below 1).
// At S = 3 (4K; within 0.001, and for a texture without the gamma bit) the three sub-taps are exactly one
// texel apart, and together they weigh four texels (1 - f, 1, 1, f) / 3 along the axis (f the fraction of the
// tap's position: frac(position in texels - 0.5)). Two bilinear fetches give the same weighted sum, for every f
// and at the image edge under the sampler's clamp (the identity is on the clamped texel values):
//     first   over texels i-1, i    at i - 1 + 1 / (2 - f)    weight (2 - f) / 3
//     second  over texels i+1, i+2  at i + 1 + f / (1 + f)    weight (1 + f) / 3     (i = floor(position - 0.5))
// so 18 fetches per axis instead of 27 (bloom_gauss_test.cpp proves it on a model of the sampler at S = 2 and 3;
// S = 2 is two sub-taps and stays two fetches). Other scales keep their n sub-taps.
//
// The helper uses tfetch2D (implicit LOD) for those single taps and tfetch2DLod (level 0, the image has one
// level) for the sub-taps, so that no gradient operation sits in a loop. No extra constants:
// g_InvResolutionScale is in the shared block of every draw. The edits (anchors that must match, or the
// variant is not built):
//
//   1. the nine calls  tfetch2D(RenderMapBilinearSampler_Texture2DDescriptorIndex,
//                       RenderMapBilinearSampler_SamplerDescriptorIndex, r0.xy, float2(  ->  GaussTap(dir, ...
//      (exactly nine in each shader, each along its axis: the offset's other component is 0)
//   2. the helper, before `void main(`
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (the feature kFeatureBloomGauss) while the setting
// mcla_gpu_bloom_scale is on.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line
// variants_bloom_gauss_version).
//   1  the Gaussians' taps as ceil(S) sub-taps
//   2  at S = 3 the three sub-taps as two bilinear fetches (the same sum)
inline constexpr uint32_t kBloomGaussVariantVersion = 2;

// The Gaussians' identities on the retail game (the runtime's own: FNV-1a 64 of the microcode; the
// "hash" column of the shader report is another number).
inline constexpr VariantShader kBloomGaussX = {"xrage_postfx__PSGaussX", 0xBF2ABD24C5329903ull};
inline constexpr VariantShader kBloomGaussY = {"xrage_postfx__PSGaussY", 0x80ED62377E87264Aull};

VariantDef MakeBloomGaussVariant();

// The text transform alone (pure; public for tests). `shader` is kBloomGaussX or kBloomGaussY (by name).
VariantTransformResult TransformBloomGaussHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack
