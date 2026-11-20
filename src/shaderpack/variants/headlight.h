#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "headlight": the lit shaders add the headlights of the cars as spot lights
// ===========================================================================
// perf/LOCAL-SHADOWS-SURVEY.md finding 1 and section 5.1: in the original game a car's headlights light nothing. The city's lit shaders (the
// 120 pixel shaders named ...PSMultiLight, ...PSMultiLightEnv, ...PSMultiLightInterior and the debug ones, in the RPF: every city material, the props
// and the traffic cars' body, xAmbientVehicle) add up their light as `lights + gLightAmbient`, and then multiply the sum by what the surface is
// (or add it to the car's colour): the grid's four lamps per pixel, the three dynamic slots, then the ambient. This variant adds one more term to that
// sum, the sum of up to four spot lights the host hands over for the frame (src/gpu/headlight_rig.h: the player's car, the nearest opponents and
// traffic cars): the light is on a car's headlights, lights what is in the cone, in front of the car, and is not any of the game's lights.
//
// What the edit changes, in every shader that has the shapes it knows:
//   1. `float4 hlLight = HeadlightLight(<the pixel's world position>);` at the start of main (after the registers are declared). The world position is the
//      interpolant the shader's own light grid lookup is made with: the grid is indexed by `LightSortScaleBias.xy * (world x, world z) + .zw`, and the
//      line that does it (`rD.zw = rS.xz * LightSortScaleBias.xy + LightSortScaleBias.zw;`) names the register; a register that nothing writes before is an
//      interpolant (`float4 rS = iTexCoordK;`), and one that is made by copies (`rD.y = max(rS.x, rS.x);`, the road's and the sidewalk's) leads to the
//      interpolant by the copies. The pixel's height is the interpolant's remaining component.
//   2. `+ hlLight.<swizzle>` appended to the statement(s) that add the ambient colour (`rD.xyz = rS.xyz + gLightAmbient.xyz;`, with the swizzle of the
//      ambient and of the sum permuted as the translator does, or `ps = gLightAmbient.x + rS.w;` one channel at a time): a term of the sum, so the shader
//      multiplies it by its albedo (or adds it to the car's colour) as it does for every lamp. A channel of the term follows its swizzle: the sum's lanes
//      are in the order the ambient's swizzle says.
//   3. The helper block before main: HeadlightLight() and its shadow hook HeadlightShadow(), and the constants' declarations (a 672 byte block after the
//      608 byte SharedConstants: src/gpu/headlight_rig.h HeadlightConstants, c38 to c79 of the block; the SPIR-V side reads them from the shared
//      constants' address).
//
// The helper (HeadlightLight): the pixel's normal is the geometric one from the derivatives of its world position, turned to face the camera (the
// camera's position is in the block); a box test against the box all the lights reach (the host's early-out, so that a pixel outside every cone pays for
// the derivatives, the normal and 3 compares); then, per light (at most four): the distance and the reach window, the cone (a smooth step between the
// outer and the inner cosine), the inverse square fall-off 1 / (1 + (d/D)^2), a wrapped surface term (saturate(w + (1 - w) N.L): a road seen at a grazing
// angle keeps a share), a low beam's cut-off (nothing a little above the lamp), and a fade near the lamp. The numbers are
// src/gpu/headlight_rig_rules.h's (LampScale is the CPU twin; the variant's test checks that the `kHl...` constants here are those).
// The shadow hook: a light with a valid local shadow view (src/gpu/local_shadow_types.h) multiplies its term by a 4 tap compare in the atlas
// (the views are in the block; the view of a light is in g_HlShadowOf; no atlas bound: lit).
//
// What the transform leaves out (it says `skip`, with the reason, and the renderer draws the game's own shader for it): a shader that reads no light grid
// (the pedestrians, the road studs, the foliage's trunks and impostors, the glass ...), one whose world position is not an interpolant or a copy of one,
// and one whose ambient is not added in a statement of the shapes above (the interior shaders pick it with a flag: `select(flag, constant, gLightAmbient)`).
// On the retail game the census is in headlight_test.cpp.
//
// Alpha test twin: the variant has one (alpha_test_id): a draw with the alpha test or alpha to coverage takes the same text compiled with the shader's
// own spec mask (the main pack's spec variant 1).
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (ps_variant kVariantHeadlight, or the twin for a draw whose key is spec variant 1) while the rig has a light
// (headlight_rig.h VariantSelected); src/gpu/scene.cpp uploads the constants (Scene::UploadHeadlightConstants).
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_headlight_version).
// 2: the beam's calibration (peak 2.0, half distance 26 m: the road is lit out to 40 m; HEADLIGHT-FIX-2), a new helper text: the packs are built again.
inline constexpr uint32_t kHeadlightVariantVersion = 2;

// The pixel shaders, found by name: every one of the lit family.
inline constexpr std::string_view kHeadlightSuffixes[] = {"__PSMultiLight", "__PSMultiLightEnv", "__PSMultiLightInterior", "__PSMultiLightDebug",
                                                          "__PSMultiLightNFLDebug"};

VariantDef MakeHeadlightVariant();

// The text transform alone (pure; public for tests). Returns `skip` for a shader whose shapes the edits do not know (with the reasons below), an
// error only for a text that came out wrong after the edits.
VariantTransformResult TransformHeadlightHlsl(const VariantShader& shader, std::string_view hlsl);

// The reasons a shader is left out (also what the tests look for).
inline constexpr std::string_view kHeadlightSkipNoGrid = "it reads no light grid";
inline constexpr std::string_view kHeadlightSkipPosition = "its world position is not an interpolant or a copy of one";
inline constexpr std::string_view kHeadlightSkipAmbient = "its ambient light is not added in a statement of a shape these edits know";

// The HLSL of the helper block (the constants' declarations are separate), for tests: the text of every `static const float kHl...` line is in it.
std::string_view HeadlightHelperText();

}  // namespace larecomp::shaderpack
