#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "env_map": the car reflections' environment map at its own scale
// ===========================================================================
// The environment map (the game's 512x256 dual paraboloid view of the world that the car paint reflects,
// side_view_env.cpp) can be made at 2x and 3x its size (mcla_gpu_env_resolution). Two of the game's pixel
// shaders read that map with taps that were written for the 512x256 one:
//
//   xrage_fastmipmap::PSFastMipMap  (identity 4AA33D052D8AAD13, in the executable)
//       the pass that makes mip levels 1 to 3: three draws a frame into the environment map's own target, each
//       taking the average of 4 taps that sit at +-0.5 TexelSize around the pixel's UV. TexelSize is a pixel
//       shader constant that the game sets in ITS texels. Its value is the guest's and neither the translated
//       text nor the census shows it [I]: 1 over the size of the level the pass reads (the four taps then sit
//       on the centres of the 2x2 texels that make one pixel of the next level) or of the level it makes. In
//       UV that is the same at any scale, which is S times as many host texels once the map is S times larger:
//       the game's kernel becomes S times as wide in the map's own texels. For the taps on the 2x2 texel
//       centres: at 2x each lands on a corner of the texels and the average is a 4x4 box (a level twice as
//       blurry as the game's, whatever the level), and at 3x on texel centres 1.5 away that skip the nearest
//       two (a dotted, shimmering level). The variant multiplies the offset by
//       g_InvResolutionScale (1 / the scale of the target the draw is made into, the environment map's own),
//       which keeps the game's kernel in host texels at any scale (the draw's implicit LOD is the same at
//       every scale: the map and its pixels grow together, so it reads the same level). At 1x it is the
//       game's own arithmetic.
//
//   xSpecMapGen::PS_Blit  (identity F92316724CFA62ED, in the RPF, cars)
//       the first of the three draws that make the 64x32 specular copy: ONE bilinear tap of the environment
//       map's base level per pixel of the copy, an 8:1 reduction (a pixel of the copy is 8 x 8 texels of the
//       map; the tap reads 2 x 2 of them, 6% of the block, and the two blurs after it smooth the aliasing).
//       Its UV comes from the vertex shader and its offset is (0, 0), so unlike the bloom's Gaussians it does
//       not depend on the size of the texture: at 2x and 3x it works, but the same tap reads 1.6% and 0.7% of
//       the block: the copy would sparkle (a lamp or the sun is 2 to 5 map texels wide). The variant takes
//       n x n bilinear taps, n = ceil(S) with S = the map's width / 512 (read from the texture: the copy's
//       own target is not scaled, so g_InvResolutionScale says nothing about the map), spaced so that the
//       footprint has the variance of the game's one tap (a tent one game texel wide: S^2 / 6 host texels^2):
//       d = sqrt(2 (S^2 - 1) / (n^2 - 1)), d = sqrt(2) = 1.41 host texels at every whole scale. At S = 1 it is
//       the game's tap. Explicit level 0 fetches: the copy's sampler reads the base map only, and no
//       gradient operation sits in the loops.
//
// The two blurs of the copy (xSpecMapGen::PS_BlurHoriz and PS_BlurVert) read the 64x32 copy itself through
// UV offsets that are pixel shader constants: not a size of the map, nothing to change.
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (ps_variant kEnvVariant) while mcla_gpu_env_resolution is
// above `game`. The edits (anchors that must each be found the number of times said, or the variant is not
// built):
//
//   PSFastMipMap:  `r1.xw = -TexelSize.xy * c255.yy + r0.xy;`  and  `r1.yz = TexelSize.yx * c255.yy + r0.yx;`
//                  (once each: the four corners of the pixel), the four taps with offset (0, 0) (exactly four)
//   PS_Blit:       the one `tfetch2D(CurrentFaceSampler_..., r0.xy, float2(0, 0))` (once), and the helper before
//                  `void main(`
//
// CHECKED offline: both edited texts (the translator's HLSL of the baseline tree with these edits) compile with dxc
// 1.9.2609 to DXIL (ps_6_0) and SPIR-V with the pack's arguments, no warning. CHECK in the pack step: the stamp says
// variants_env_map_shaders=4aa33d052d8aad13,f92316724cfa62ed. CHECK in a run at `high`: the three level draws and the
// reduction have no alpha test or alpha to coverage (a draw that has them keeps the game's shader: a variant is made
// for spec variant 0), so the picture of the levels and of the specular copy follows the scale.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_env_map_version).
inline constexpr uint32_t kEnvMapVariantVersion = 1;

// Identities on the retail game (the runtime's own: FNV-1a 64 of the microcode; the "hash" column of the
// shader report is another number). Names as the translator makes them: group and entry point.
inline constexpr VariantShader kEnvMipPass = {"xrage_fastmipmap__PSFastMipMap", 0x4AA33D052D8AAD13ull};
inline constexpr VariantShader kEnvReduce = {"xSpecMapGen__PS_Blit", 0xF92316724CFA62EDull};

VariantDef MakeEnvMapVariant();

// The text transform alone (pure; public for tests). `shader` is kEnvMipPass or kEnvReduce (by name).
VariantTransformResult TransformEnvMapHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack
