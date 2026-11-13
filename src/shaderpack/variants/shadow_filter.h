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
// scale), z = the noise frame, w = mode bits (1: Gather taps; 2: contact hardening; bits 8 to 15: the sun's
// size for it, in sixteenths of the real sun's angular radius, 0.27 degrees).
//
// Contact hardening (PCSS, package G1c; mode bit 2, the same shader and pipeline as soft: one uniform branch):
//   - a blocker search first: 8 bilinear taps over a disc of twice the soft radius (the game's radius times
//     the atlas scale: the cap below), a tap nearer to the sun than the receiver (stored < depth, the game's
//     own compare turned round) is a blocker. None: lit, all 8: in shadow, and the 16 taps are not taken at
//     all (the pixels away from an edge cost 8 taps, half of what soft takes everywhere).
//   - the penumbra: the distance from the receiver to the average blocker along the sun's rays times
//     tan(the sun's angular radius) is the radius of the sun's disc seen through the blockers' plane, i.e.
//     of the PCF's disc in the world. World units come from the cascade's light matrix (the game's own
//     address register a0 picks its rows: gLightMatrixArr(a0 .. a0 + 3), which the collector's PS c64-c79
//     hold, the same in every draw of a frame: 6B.2 item 1): rows 0 to 2 are the gradients of u, v and the
//     depth along the world's x, y, z and are at right angles (a rotation and a scale: on the census constants
//     of gc3-day-s1 the cosines between the three are under 2e-6 in all four cascades, and the u and v axes
//     are the planes' right and top axes of c85 and c82), so the lengths of (row0.x, row1.x, row2.x)
//     and (row0.y, row1.y, row2.y) are the atlas uv per metre across the rays in u and in v, and the length
//     of (row0.z, row1.z, row2.z) times row 3's w the depth per metre along them. The kernel is that radius in
//     uv, per axis: an ellipse, because the atlas texels are taller than wide.
//   - clamped to [0.75 atlas texels, twice the soft filter's radius] per axis: the map cannot show an edge
//     sharper than about a texel, and the cap keeps the PCF's 16 taps dense enough (it follows the atlas scale
//     with the soft radius: constants.y). With the real sun the penumbra is under a texel for every caster
//     lower than about 3.5 m at the nearest cascade, so the sun's size is a setting (mcla_gpu_shadow_filter_sun).
//   - a Low class never gets the search (the host leaves the bit out): LOW is soft.
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (ps_variant kVariantShadowFilter) while
// mcla_gpu_shadow_filter resolves to soft or pcss; src/gpu/scene.cpp uploads the constants. A variant is built for
// spec variant 0 only, so a collector draw that has the alpha test or alpha to coverage on keeps the game's
// shader (CHECK in the report: whether the alpha shaders are drawn that way).
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line
// variants_shadow_filter_version).
inline constexpr uint32_t kShadowFilterVariantVersion = 2;  // 2: contact hardening (mode bit 2), a0 passed to the helper

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
