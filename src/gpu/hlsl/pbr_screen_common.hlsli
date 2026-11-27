// MCLA GPU renderer -- the screen passes' side of the shared physically based model (PBR level 1C, perf\PBR.md "1C screen passes and guides").
//
// The contract is hlsl/pbr_brdf.hlsli (PBR-1 owns it). This file is what the screen passes add to it, and nothing else: the screen-space reflections' resolve and trace, the probe field's apply, the screen-space
// bounce light's apply, and the guides of DLSS Ray Reconstruction and AMD FSR Ray Regeneration all read the same things off a G-buffer pixel (RT2's albedo, RT3's smoothness, class and metal flag) and
// turn them into the contract's PbrSurface here (PbrSurfaceOfGbuffer: the roughness, the metalness and the reflectance of the class are the contract's), so a reflection's weight, a bounce light's diffuse
// share and a denoiser's albedo guides agree with each other and with the local lights.
//
// THE OLD PATH. Every pass keeps its previous maths behind `mcla_gpu_pbr 0` (a bit in the pass's own flags, set by pbr_screen::Enabled()): the functions here are only called with the bit on.
//
// This file keeps to the subset both languages read (src/gpu/pbr_screen_test.cpp compiles it as C++ with the contract itself, through the tests' shim of the HLSL vector types and
// pbr_screen_shim.h): scalar and float3 arithmetic, .x .y .z, no swizzles of more than one component, no `out` parameters. The twin is src/gpu/pbr_screen_rules.h.

#ifndef MCLA_PBR_SCREEN_COMMON_HLSLI
#define MCLA_PBR_SCREEN_COMMON_HLSLI

#include "gbuffer_common.hlsli"
#include "pbr_brdf.hlsli"

static const float kPbrScreenUnknownMetalAlbedo = 0.6f;     // a metal that the frame holds no albedo for (what the reflections always took)

// The metalness of a G-buffer pixel: RT3's metal flag (1) or the class's default (the contract's PbrMetalOfScreen: chrome 1, metal 0.85, every other class 0).
float PbrScreenMetalness(uint cls, bool metal_flag) {
  return PbrMetalOfScreen(cls, metal_flag);
}

// The contract's surface of a G-buffer pixel: `albedo` is RT2's albedo decoded (GbAlbedoDecode), `have_albedo` false where the frame holds none (a metal then takes kPbrScreenUnknownMetalAlbedo, as the
// reflections always did), `smoothness` RT3's (the wet road's, where the pass raised it), `metal_flag` RT3's, `n` the unit normal in the space of the pass's vectors.
PbrSurface PbrScreenSurface(uint cls, bool metal_flag, float smoothness, float3 albedo, bool have_albedo, float3 n) {
  const float3 base = have_albedo ? albedo : float3(kPbrScreenUnknownMetalAlbedo, kPbrScreenUnknownMetalAlbedo, kPbrScreenUnknownMetalAlbedo);
  return PbrSurfaceOfGbuffer(n, base, smoothness, cls, metal_flag);
}

// How strongly a reflection of this pixel is seen from the direction with the cosine `nv` to the normal: the contract's split sum (PbrEnvSpecularWeight), per channel, within 0 and 1 (the fit can overshoot
// 1 at exactly grazing). An emissive pixel reflects nothing (a lamp lens, a neon tube: the light is its own). Multiply the reflected radiance by it.
float3 PbrScreenReflectionWeight(uint cls, bool metal_flag, float smoothness, float3 albedo, bool have_albedo, float nv) {
  if (cls == uint(GB_CLASS_EMISSIVE)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const PbrSurface s = PbrScreenSurface(cls, metal_flag, smoothness, albedo, have_albedo, float3(0.0f, 0.0f, 1.0f));
  return saturate(PbrEnvSpecularWeight(s, nv));
}

// The weight of incoming diffuse light (the bounce light) at this pixel: the diffuse colour (a metal has none) times what the specular layer lets through (1 - the reflection's weight).
float3 PbrScreenDiffuseWeight(uint cls, bool metal_flag, float smoothness, float3 albedo, float nv) {
  const PbrSurface s = PbrScreenSurface(cls, metal_flag, smoothness, albedo, true, float3(0.0f, 0.0f, 1.0f));
  return PbrEnvDiffuseWeight(s, nv);
}

// The share of a pixel's OWN diffuse colour that takes incoming diffuse light (no albedo in it): (1 - metalness) * (1 - the reflection's weight, averaged over the channels). The screen-space bounce light's
// apply has the pixel's own colour as its multiplier (the framebuffer) and not an albedo, so what it needs from the model is this share. Near 1 for a rough dielectric seen head on, 0 for a pure metal.
float PbrScreenDiffuseShare(uint cls, bool metal_flag, float smoothness, float nv) {
  const PbrSurface s = PbrScreenSurface(cls, metal_flag, smoothness, float3(1.0f, 1.0f, 1.0f), true, float3(0.0f, 0.0f, 1.0f));
  const float3 w = PbrEnvSpecularWeight(s, nv);
  const float spec = saturate((w.x + w.y + w.z) * (1.0f / 3.0f));
  return (1.0f - s.metalness) * (1.0f - spec);
}

// DLSS Ray Reconstruction's diffuse albedo guide of a surface (rr_guides_common.hlsli): the base colour times (1 - metalness), never below `floor_albedo` ("never leave a valid pixel at 0"). A pure metal
// is the floor.
float3 PbrScreenDiffuseGuide(uint cls, bool metal_flag, float3 albedo, float floor_albedo) {
  const float k = 1.0f - PbrScreenMetalness(cls, metal_flag);
  return float3(max(albedo.x * k, floor_albedo), max(albedo.y * k, floor_albedo), max(albedo.z * k, floor_albedo));
}

#endif  // MCLA_PBR_SCREEN_COMMON_HLSLI
