#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "car_chrome_city": the car's chrome reflects the CITY (CITY-RT M2 R3a)
// ===========================================================================
// WHAT WAS THERE. The chrome variant (car_chrome.h, feature 7) reads the ray traced image of the car's reflection (src/gpu/car_rt_refl.h) for ONE thing: the weight, the share of the
// reflection cone that hits anything, which it takes for "a part of the car covers this chrome" and darkens `env` by (a tailpipe under its bumper). The image holds more: the COLOUR of what
// the cone found, and since CITY-RT M2 R1 (src/gpu/city_refl.h) what it finds is also the city, the lamp heads, the signs, the cars beside the player, and the paint's variant reads that
// colour. A chrome badge or a tailpipe in the open sun beside a building therefore had its weight read as an occluder (the reflection darkened by the share of the city in the cone) and
// the colour thrown away: the opposite of a mirror.
//
// WHAT THIS DOES (one edit, one helper): where the image answers for the player's own chrome texel (the chrome variant's state 5) and the self-reflection is at its reflection level (mode 2,
// as the paint's own replacement), the texel's cone is split by the image's SIGN (alpha 1 + w a hit on the car's own parts, -(1 + w) a hit mostly on ANOTHER CAR or the city: the paint's
// clear-coat rule, car_rt_hit_rules.h, which city hits take with the coat flag on, the default):
//     whole = w x the self-test's agreement             the cone's share that hit anything (what the chrome variant reads as `reflectHit`)
//     city  = whole x (the share of the weight with the other-car sign)
//     own   = whole - city
//     env'  = lerp(env x (1 - 0.9 x own x strength), colour x gain, city x strength)
// The car's own parts still darken the sky's reflection exactly as they did (a tip under a bumper), and the city and the cars beside replace it with what they look like. `colour` is the mean of the
// hits' colours in the history's units and `gain` the paint's constants' colour gain (g_CarReflB.z), as the paint's replacement takes it (car_selfrefl.cpp CarReflRtLookup). The chrome's `sun`,
// its AO and its blend with the sky ambient are the game's, as before. Everything else keeps the chrome variant's own line: the march (no image, or no data at the point), another car's chrome
// (state 6), a grazing view, a point the history does not hold, and the occlusion-only level.
//
// IDENTITY. This is a stage of its own, not a new version of the chrome's, so that the chrome's words in the pack stay byte for byte what they were (variant version 3, the golden's hashes):
//   - feature 13 in variant_chain.h composes with the chrome (7) and the G-buffer (10); pipelines.cpp wants it only while the chrome is wanted AND the city is in the reflections
//     (mcla_gpu_rt_reflections not off, a ray traced pass this frame, mcla_gpu_city_refl_chrome): a setting that is off draws the chrome's own words, which this stage never touched;
//   - the transform applies after the chrome's (the chain's order: ascending index), rewrites the one line the chrome stage wrote (`r1.xyz = CarChromeEnv(r1.xyz, carChrome);`) and adds a
//     helper before main; it declares no constants (it reads the chrome's block, g_CarReflA..C);
//   - in a text without the chrome stage (feature 13 alone, or with the G-buffer's alone) there is nothing to rewrite: the pack builds that entry as the game's own text, which only the
//     compositions need to exist (variant_pack.cpp builds a composition from the stages that were built alone) and which nothing draws;
//   - where the image has no city and no other car in the texel (the share `other` is 0: a frame whose city is off, a texel whose cone found only the car's own parts) the helper's
//     arithmetic is the chrome variant's own, exactly (own = whole, city = 0: env x (1 - 0.9 x w x strength)); the rules header's test (src/gpu/car_chrome_rt_rules.h ChromeCityEnv) checks it.
//
// The pure numbers and the arithmetic are mirrored in src/gpu/car_chrome_rt_rules.h (ReadForChromeCity, ChromeCityEnv: the same function as plain code, with its test, which also checks
// that this text holds each of them). Change one, change the other.
//
// CHECK (not run in a game): both edited texts compile with dxc (ps_6_0 DXIL and SPIR-V, the pack's arguments; the retail text was compiled and spirv-val checked by the package's author:
// car_chrome_city_test writes it with a second folder argument); that the texel's sign is what the image says in a picture (the debug view 4 of the chrome shows blue where the image
// answers; the chrome beside a building should show the building, beside the sky the sky); the look of the badge and the tips in the open sun against the game's chrome (A/B:
// mcla_gpu_city_refl_chrome); the cost: a chrome pixel does 18 more point reads (the image's nine texels and the history's nine again), a few hundred pixels at 4K.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/car_chrome.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_car_chrome_city_version).
inline constexpr uint32_t kCarChromeCityVariantVersion = 1;  // 1: the chrome reads the image's colour for the city and the cars beside (CITY-RT M2 R3a)

VariantDef MakeCarChromeCityVariant();

// The text transform alone (pure; public for tests). `ctx.features` says whether the chrome's stage is in the text: without it the text comes back as the game wrote it.
VariantTransformResult TransformCarChromeCityHlsl(const VariantShader& shader, std::string_view hlsl,
                                                  const StageContext& ctx = SoloContext(kFeatureCarChromeCity));

}  // namespace larecomp::shaderpack
