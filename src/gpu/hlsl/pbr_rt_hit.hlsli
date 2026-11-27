// MCLA GPU renderer -- the ray traced hit shading's thin layer over the shared physically based light model (PBR level 1, task PBR-1B; design perf\PBR.md "1B RT hit shading").
//
// hlsl/pbr_brdf.hlsli is THE CONTRACT (PbrSurface, PbrDirect, PbrEnvSpecularWeight, PbrEnvDiffuseWeight ...); this file only says how the hit shading's own units meet it, once, so that the city hit library
// (hlsl/city_hit_shade.hlsli) and the car hit shading (hlsl/car_rt_hit_shade.hlsli) agree. C++ twin: src/gpu/pbr_rt_hit_rules.h (src/gpu/pbr_rt_hit_rules_test.cpp checks the two against each other).
//
// UNITS. The hit shading works in the game's own light units: a surface of albedo a under a light of colour L at N.L = c shows a x L x c (the lit shaders have no 1/pi: the sun's colour is not an
// irradiance in the usual sense, a white wall facing it shows exactly that colour). The contract's PbrDirect is the usual BRDF, whose diffuse term has the 1/pi, so a light of the game's colour L is
// the radiance L x pi to it: then a Lambert surface shows what it always showed, (1 - F) x a x L x c, and the model's only changes are the energy split (1 - F), the specular lobe and the metals. The
// ambient and the other irradiance sums (the sky's hemisphere, the GI field's feedback) are in the game's units already and take PbrEnvDiffuseWeight, which has no 1/pi.
//
// Compiled as C++ by city_hit_shade_rules_test.cpp (hlsl_cpp_shim.h; the contract, hlsl/pbr_brdf.hlsli, compiles as C++ the same way): scalar and vector arithmetic only, no swizzles on scalars, no out parameters.

#ifndef MCLA_PBR_RT_HIT_HLSLI
#define MCLA_PBR_RT_HIT_HLSLI

#include "pbr_brdf.hlsli"

// The N.L under which PbrRtDirectIrradiance does not divide (a light that grazes the surface): the response to an irradiance E from a direction is E over N.L as radiance, and N.L is not allowed to be 0.
static const float kPbrRtMinNdl = 0.05;

// The response of the surface to a light of the game's colour `lightColour` (the sun's, times its visibility) arriving from the unit direction `l`, seen from the unit direction `v`: PbrDirect.
float3 PbrRtDirect(PbrSurface s, float3 v, float3 l, float3 lightColour)
{
    return PbrDirect(s, v, l, lightColour * kPbrPi);
}

// The same for an IRRADIANCE `irradiance` that has N.L in it already (a sum of lamps, the game's lamp term: colour x term x cone x N.L) and arrives from `l`, the brightness weighted mean direction of the lamps
// that made it: PbrDirect takes radiance and multiplies by N.L itself, so it is given the irradiance over N.L (kPbrRtMinNdl at least). Exact for one lamp; for several it is the usual "most representative
// light" of their sum.
float3 PbrRtDirectIrradiance(PbrSurface s, float3 v, float3 l, float3 irradiance)
{
    return PbrDirect(s, v, l, irradiance * (kPbrPi / max(dot(s.n, l), kPbrRtMinNdl)));
}

// The ambient and the irradiance sums that are already in the game's units (no direction): the diffuse colour times what the specular layer lets through.
float3 PbrRtDiffuse(PbrSurface s, float nv, float3 light)
{
    return PbrEnvDiffuseWeight(s, nv) * light;
}

// A reflection (the environment map, a ray's hit) seen from nv: its radiance times the split-sum weight.
float3 PbrRtReflection(PbrSurface s, float nv, float3 radiance)
{
    return PbrEnvSpecularWeight(s, nv) * radiance;
}

#endif  // MCLA_PBR_RT_HIT_HLSLI
