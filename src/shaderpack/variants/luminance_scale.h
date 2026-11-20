#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "luminance_scale": the auto-exposure's sample at a render scale
// ===========================================================================
// The game's auto-exposure is a chain of small passes (feature-parity/GFX-UPGRADES-DESIGN.md 6B.2 item 4):
//
//   P6  xrage_postfx::PSSampleAverageLuminance   128x128   reads the post chain's 320x180 image
//   P7-P9  xrage_postfx::PSReSampleAverageLuminance(Exp)   64x64 .. 1x1   4 x 4 averages of the 128x128 result
//   then PSCalculateAdaptedLuminance (the 1x1 result adapts over time, the tone map divides by it).
//
// The chain from P7 on, and the 128x128 target of P6, are the guest's size at every render scale. The image P6
// reads is not: it is the bloom chain's last image, a screen image, and the host makes it at the render scale S
// (320 S x 180 S: 960x540 at 3x). P6 reads it with nine POINT taps at the immediate offsets -1, 0, 1 texel in x and
// y, and the translated fetch divides an immediate offset by the host texture's own size, so the nine taps are a
// 3 x 3 texel window whatever S is:
//
//     the game's 1x      one 128x128 pixel stands for 2.50 x 1.41 texels of the 320x180 image; the window is 3 x 3
//                        texels: it overlaps its neighbours' windows (120% x 213%), every texel is read, complete.
//     at 3x              one pixel stands for 7.50 x 4.22 texels of the 960x540 image; the window is still 3 x 3
//                        texels: 40% x 71% of the pixel's area, 28% of the image is read at all.
//     (2x: 60% x 100%; 1.5x: 80% x 142%; 4x would be 16%.)
//
// What each tap reads is also finer at S: a texel of the image is 4 x 4 pixels of the scene at any scale, so at 3x
// a tap sees one ninth of the screen area it saw at 1x. Nothing is wrong with the average of the picture, but its
// sample is sparse and regular: a small bright light (a lamp, a lit window, a car's tail light at night) moving
// into and out of one of those few texels moves the log average by a step, and the exposure (the tone map divides
// by it) can pump. The bloom's Gaussians (bloom_gauss.h) had the same cause, an immediate offset in texels of an
// image that is S times larger; this is the same fix.
//
// The variant of PSSampleAverageLuminance replaces each of the nine calls
//
//     tfetch2D(ToneMapSampler_Texture2DDescriptorIndex, ToneMapSampler_SamplerDescriptorIndex, r0.xy, float2(a, b))
//
// by a call of a helper that returns the AVERAGE COLOUR of the screen area the game's tap stood for, which is
// what one texel of the 1x image is (a texel there is already the average of a 4 x 4 pixel block of the scene).
// The area is the S x S texels (per axis: Sx x Sy) centred at the game's offset times the scale; it is read with
// nx x ny point sub-taps, nx = ceil(Sx), ny = ceil(Sy), spaced S / n texels, so that at a whole scale every texel
// of the 3 x 3 x S x S window is read once (81 fetches at 3x). The helper returns a colour, and the game's own
// code goes on as before: luminance of each tap, log of each, the mean of the nine: the same statistic as at 1x,
// of the same screen area, at any scale.
//
// The scale is not a constant of the draw: P6's own target is the guest's size, so g_InvResolutionScale says 1.
// The helper reads it from the texture, like the environment map's reduction (env_map.h): Sx = the texture's
// width / 320 and Sy = its height / 180 (the game's screen is 1280x720 at every aspect: an ultrawide picture is
// drawn squeezed and stretched by the present, so the guest's chain is always 320x180). An axis whose scale is
// within 1% of 1, or below it, keeps the game's tap and the game's offset: when both are, the helper makes the
// game's own call (tfetch2D, implicit level), so at render scale 1 the picture and the shader's arithmetic are
// the game's, bit for bit; below 1 the 3 x 3 window already covers the pixel's area several times over, and the
// game's taps stay. The sub-taps are explicit level 0 fetches (the image has one level, and no gradient operation
// sits in a loop). No extra constants.
//
// The edits (anchors that must match, or the variant is not built):
//
//   1. the nine calls  tfetch2D(ToneMapSampler_Texture2DDescriptorIndex, ToneMapSampler_SamplerDescriptorIndex,
//                       r0.xy, float2(  ->  LuminanceTap(...        (exactly nine; offsets whole numbers -1..1)
//   2. the helper, before `void main(`
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (the feature kFeatureLuminanceScale) while the setting
// mcla_gpu_luminance_scale_fix is on.
//
// In the pack step's log: "luminance_scale (1 shader)" in the variant pack's summary line, and in its stamp
// variants_luminance_scale_version=1 and variants_luminance_scale_shaders=3cec0bb4ccd2bbbe.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line
// variants_luminance_scale_version).
inline constexpr uint32_t kLuminanceScaleVariantVersion = 1;

// The shader's identity on the retail game (the runtime's own: FNV-1a 64 of the pixel shader's microcode; the
// "hash" column of the shader report is another number, 533A9017221F8249).
inline constexpr VariantShader kLuminanceSample = {"xrage_postfx__PSSampleAverageLuminance",
                                                   0x3CEC0BB4CCD2BBBEull};

VariantDef MakeLuminanceScaleVariant();

// The text transform alone (pure; public for tests). `shader` is kLuminanceSample (by name).
VariantTransformResult TransformLuminanceScaleHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack
