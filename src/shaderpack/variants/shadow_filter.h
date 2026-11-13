#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "shadow_filter": the sun shadow collector's soft lookup
// ===========================================================================
// The sun's shadows are drawn in two steps (feature-parity/GFX-UPGRADES-DESIGN.md 3.1): the cascades fill a
// 1280x1280 depth atlas, and the collector (phase 0x1000) draws every receiver into a screen sized mask with
// its material's PS_ShadowBlend / PS_ShadowBlendAlpha: 159 pixel shaders in the game's RPF. Most of them are
// the same lookup with other register numbers. It picks the cascade (4 minus the number of cascade boxes that
// hold the point), turns the point into an atlas uv and a receiver depth with that cascade's light matrix,
// and compares FOUR taps, a cross of radii (1, 0.75, 0.5, 0.25) x count / 1280 uv (count = 4 at the nearest
// cascade down to 1, so up to 4 atlas texels) turned by a per-pixel angle, with the game's own `>=`, and
// averages them with 0.25 each. Four binary taps give five levels: a grainy shadow edge.
//
// What the variant changes, in every shader that has that lookup (the four texture fetches of the atlas at
// float2(0, 0) offsets, a compare of the four and an average with 0.25):
//
//   - the average is replaced by ShadowFilterSoft(): N taps (8 on Low, 16 otherwise, the host's constants)
//     on a Vogel disk (golden angle spiral, sqrt radius) of the game's own radius round the game's own centre,
//     turned by interleaved gradient noise of the HOST pixel position (SV_Position: the game's angle comes
//     from the guest's pixels, so at a render scale S its pattern is S x S pixel blocks) plus the R2 shift of
//     the frame number while a temporal upscaler runs (the host sets the frame; 0 keeps the pattern fixed),
//     compared with the game's `>=` and weighted by a smooth disk weight (1 - r^2 / 2). The edge is as wide
//     as the game's (10 to 90 percent: 1.36 radii for the game's four taps, 1.36 for this disk at gain 1.05).
//   - the centre and the radius are not recomputed: the game's four tap coordinates are copied just before
//     the fetches (they are overwritten by the fetched depths afterwards) and the receiver depth just before
//     the compare, and the helper takes the centre and the radius from them: tap 1 is centre + 1.0 R u and
//     tap 3 centre - 0.5 R u (u a unit vector, R the radius), so centre = (tap1 + 2 tap3) / 3 and
//     R = |tap1 - tap3| / 1.5. The anchors check the constants that make it so (c255, c253.y).
//   - the lookup is either the guest's bilinear sampler (a tap compares an interpolated depth, as the game's
//     do: the game's bias constants were tuned for it) or Gather (each tap is a bilinear weighted compare of
//     the four texels: a true PCF); the host picks (the constants' `mode`, mcla_gpu_shadow_filter_pcf).
//
// The four fetches and the compare stay in the text and are dead code the compiler removes. The game's other
// lookups are not touched: the car paint, the skinned metal and the glass (5 shaders) already filter
// with the bilinear weights of a 2x2 block and an exponential falloff (getWeights2D), and 7 shaders (the
// shadow imposter collector, the plastic and visor characters, the pond water and the store interior) read no
// atlas at all; the transform says `skip` for them (variant_pack.h). On the retail game that leaves 147 of
// the 159: 101 + 44 + 2 in four texts, so four compiles.
//
// Extra constants: 16 bytes after the SharedConstants, `float4 g_ShadowFilter` at byte 608 (c38 of the block;
// src/gpu/shadow_filter.h ShadowFilterConstants): x = taps, y = radius scale (1; G1a sets 1 / the atlas
// scale), z = the noise frame, w = mode bits (1: Gather taps; 2: contact hardening, G1c, not read yet).
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (ps_variant kVariantShadowFilter) while
// mcla_gpu_shadow_filter resolves to soft; src/gpu/scene.cpp uploads the constants. A variant is built for
// spec variant 0 only, so a collector draw that has the alpha test or alpha to coverage on keeps the game's
// shader (CHECK in the report: whether the alpha shaders are drawn that way).
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line
// variants_shadow_filter_version).
inline constexpr uint32_t kShadowFilterVariantVersion = 1;

// The collector's pixel shaders, found by name (the entry points of the .updb path; the identities differ
// per material and the pack records each one's own).
inline constexpr std::string_view kShadowBlendSuffix = "__PS_ShadowBlend";
inline constexpr std::string_view kShadowBlendAlphaSuffix = "__PS_ShadowBlendAlpha";

VariantDef MakeShadowFilterVariant();

// The text transform alone (pure; public for tests). `shader` is only used in messages. Returns `skip` for
// a shader that reads no atlas or has a filter of its own, an error for a shader that reads the atlas in
// a way these edits were not written for.
VariantTransformResult TransformShadowFilterHlsl(const VariantShader& shader, std::string_view hlsl);

// The reasons a shader is left out (also what the tests look for).
inline constexpr std::string_view kShadowSkipNoLookup = "it reads no shadow map";
inline constexpr std::string_view kShadowSkipOwnFilter = "its own filter (bilinear weights and an exponential falloff)";

}  // namespace larecomp::shaderpack
