#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared physically based light model, the C++ twin of hlsl/pbr_brdf.hlsli (PBR level 1, owner-approved 2026-11-22; design perf/PBR.md section 1)
// ===========================================================================
// What a surface answers to a light, for every consumer that lights with OUR lights: GGX / Trowbridge-Reitz normal distribution, height-correlated Smith visibility, Schlick Fresnel, the cheap
// multi-scatter compensation, Lambert diffuse with the (1 - F) energy split, metals with no diffuse and F0 = albedo, a spherical light's representative point, the split-sum weights of a
// reflection and of incoming diffuse light. Written here once, as pure functions on plain numbers, and held to the HLSL text by src/gpu/pbr_brdf_test.cpp (which compiles the .hlsli as C++),
// to the laws of physics (white furnace, reciprocity, the roughness limits, metal against dielectric: the same test) and to the first model's numbers (the calibration section).
//
// The conventions are the .hlsli's: unit vectors n (surface), v (to the eye), l (to the light); PERCEPTUAL roughness r (alpha = r * r), clamped at kMinRoughness; radiance in the PHYSICAL
// convention (a Lambert surface of albedo a lit at normal incidence by `radiance` returns a * radiance / pi; the first model's colour x scale is radiance / pi). Pure: no renderer, no guest, no SDK.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gbuffer_types.h"

namespace larecomp::gpu::pbr {

inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kMinRoughness = 0.045f;
inline constexpr float kDefaultF0 = 0.04f;
inline constexpr float kMetalChrome = 1.0f;
inline constexpr float kMetalMetal = 0.85f;
inline constexpr float kF0Water = 0.02f;
inline constexpr float kF0Skin = 0.028f;
inline constexpr float kRadiusHeadlight = 0.08f;
inline constexpr float kRadiusLamp = 0.20f;
inline constexpr float kRadiusNeon = 0.50f;
inline constexpr float kRadiusOther = 0.15f;
inline constexpr float kMinE = 0.2f;
inline constexpr float kMsMetal = 0.02f;  // the multi-scatter factor is applied above this metalness only, ramped in over the first 1 / kMsRamp
inline constexpr float kMsRamp = 8.0f;

struct V3 {
  float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(float s, V3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }
inline V3 Reflect(V3 i, V3 n) { return i - n * (2.0f * Dot(n, i)); }
inline float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline V3 Splat(float v) { return {v, v, v}; }

struct Surface {
  V3 n;
  V3 albedo;
  float roughness = 1.0f;
  float metalness = 0.0f;
  float f0_dielectric = kDefaultF0;
};

inline Surface MakeSurface(V3 n, V3 albedo, float roughness, float metalness, float f0_dielectric) {
  Surface s;
  s.n = n;
  s.albedo = albedo;
  s.roughness = std::min(std::max(roughness, kMinRoughness), 1.0f);
  s.metalness = Saturate(metalness);
  s.f0_dielectric = f0_dielectric;
  return s;
}

// ---- the material -------------------------------------------------------------------------------------------------------------------------------------------------------------

// The G-buffer's smoothness (RT3.r) to perceptual roughness: the byte is s = 1 - sqrt(alpha) (gbuffer_rules.h SmoothnessOfAlpha), so r = 1 - s and alpha_ggx = r^2.
inline float RoughnessFromSmoothness(float smoothness) { return std::min(std::max(1.0f - Saturate(smoothness), kMinRoughness), 1.0f); }
// A Blinn-Phong POWER n: alpha = sqrt(2 / (n + 2)), roughness = sqrt(alpha) (a pixel shader's exponent register is half the power: gbuffer_material.h GlossPowerScale).
inline float RoughnessFromBlinnPower(float n) {
  const float alpha = std::sqrt(2.0f / (std::max(n, 0.0f) + 2.0f));
  return std::min(std::max(std::sqrt(alpha), kMinRoughness), 1.0f);
}

inline float MetalOfClass(uint32_t cls) {
  if (cls == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kChrome)) return kMetalChrome;
  if (cls == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kMetal)) return kMetalMetal;
  return 0.0f;
}
inline float MetalOfScreen(uint32_t cls, bool metal_flag) { return metal_flag ? 1.0f : MetalOfClass(cls); }
inline float MetalOfWord(uint32_t cls, float metal_byte01) { return std::max(MetalOfClass(cls), Saturate(metal_byte01)); }
inline float F0OfClass(uint32_t cls) {
  if (cls == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kWater)) return kF0Water;
  if (cls == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kSkin)) return kF0Skin;
  return kDefaultF0;
}
inline Surface SurfaceOfGbuffer(V3 n, V3 albedo, float smoothness, uint32_t cls, bool metal_flag) {
  return MakeSurface(n, albedo, RoughnessFromSmoothness(smoothness), MetalOfScreen(cls, metal_flag), F0OfClass(cls));
}
inline V3 F0(const Surface& s) { return Lerp(Splat(s.f0_dielectric), s.albedo, s.metalness); }
inline V3 DiffuseColour(const Surface& s) { return s.albedo * (1.0f - s.metalness); }

// The radius of the source a light of this kind stands for (light_frame.h LightSource), metres.
inline float LightRadius(uint32_t source) {
  switch (source) {
    case 0: return kRadiusHeadlight;  // LightSource::kHeadlight
    case 1: return kRadiusLamp;       // kLamp
    case 2: return kRadiusNeon;       // kNeon
    default: return kRadiusOther;     // police, mod, other
  }
}

// ---- the lobes ----------------------------------------------------------------------------------------------------------------------------------------------------------------

inline V3 FresnelSchlick(V3 f0, float cos_theta) {
  const float m = 1.0f - Saturate(cos_theta);
  const float m2 = m * m;
  return f0 + (Splat(1.0f) - f0) * (m2 * m2 * m);
}
inline float DGgx(float nh, float alpha) {
  const float a2 = alpha * alpha;
  const float d = nh * nh * (a2 - 1.0f) + 1.0f;
  return a2 / std::max(kPi * d * d, 1.0e-20f);
}
inline float VSmithGgxCorrelated(float nv, float nl, float alpha) {
  const float a2 = alpha * alpha;
  const float gv = nl * std::sqrt(nv * nv * (1.0f - a2) + a2);
  const float gl = nv * std::sqrt(nl * nl * (1.0f - a2) + a2);
  return 0.5f / std::max(gv + gl, 1.0e-7f);
}
// E(roughness, N.V) = the integral of D V N.L of a white specular layer, and B = its part at F0 = 0 (the albedo of reflectance F0 is F0 * (E - B) + B): least squares fits of this lobe, the twin of
// the .hlsli's PbrDfgE / PbrDfgB (the test holds them to the exact integral).
inline float DfgE(float roughness, float nv) {
  const float x = std::sqrt(roughness);
  const float y = std::sqrt(std::min(std::max(nv, 0.04f), 1.0f));
  const float p0 = (((2.107673f) * y + (-1.726959f)) * y + (0.385717f)) * y + (0.868638f);
  const float p1 = (((-8.172101f) * y + (-0.226110f)) * y + (1.588044f)) * y + (1.353653f);
  const float p2 = (((6.440065f) * y + (17.661646f)) * y + (-0.243378f)) * y + (-8.244065f);
  const float p3 = ((-16.142164f) * y + (-16.899258f)) * y + (15.628299f);
  const float p4 = (14.536796f) * y + (-8.625241f);
  const float g = (((p4 * x + p3) * x + p2) * x + p1) * x + p0;
  return std::min(std::max(g, 0.0f), 1.0f);
}
inline float DfgB(float roughness, float nv) {
  const float x = std::sqrt(roughness);
  const float y = std::min(std::max(nv, 0.04f), 1.0f);
  const float p0 = (((6.852671f) * y + (7.361595f)) * y + (-3.673047f)) * y + (0.256724f);
  const float p1 = (((-4.659930f) * y + (-83.194467f)) * y + (22.290481f)) * y + (8.188444f);
  const float p2 = (((-1.864670f) * y + (166.076176f)) * y + (-2.968450f)) * y + (-29.168003f);
  const float p3 = ((-91.156636f) * y + (-58.213473f)) * y + (34.029022f);
  const float p4 = (42.980395f) * y + (-13.292207f);
  const float g = (((p4 * x + p3) * x + p2) * x + p1) * x + p0;
  const float m = 1.0f - y;
  return std::min(std::max(std::max(g, 0.0f) * (m * m * m * m * m), 0.0f), 1.0f);
}
// The split sum's (A, B): the albedo of reflectance F0 is F0 * A + B; A + B = E.
struct Ab {
  float a = 0, b = 0;
};
inline Ab DfgAnalytic(float roughness, float nv) {
  const float e = DfgE(roughness, nv);
  const float b = std::min(DfgB(roughness, nv), e);
  return {e - b, b};
}
inline V3 MultiScatter(V3 f0, float roughness, float nv) {
  const float e = std::min(std::max(DfgE(roughness, nv), kMinE), 1.0f);
  return Splat(1.0f) + f0 * (1.0f / e - 1.0f);
}
inline V3 EnvSpecularWeight(const Surface& s, float nv) {
  const Ab ab = DfgAnalytic(s.roughness, nv);
  const V3 f0 = F0(s);
  const V3 weight = f0 * ab.a + Splat(ab.b);
  if (s.metalness > kMsMetal) {
    const float e = std::min(std::max(ab.a + ab.b, kMinE), 1.0f);
    return weight * (Splat(1.0f) + f0 * ((1.0f / e - 1.0f) * Saturate(s.metalness * kMsRamp)));
  }
  return weight;
}
inline V3 EnvDiffuseWeight(const Surface& s, float nv) { return DiffuseColour(s) * (Splat(1.0f) - EnvSpecularWeight(s, nv)); }

inline V3 DiffuseLobe(const Surface& s, V3 v, V3 l) {
  const V3 vl = v + l;
  if (!(Dot(vl, vl) > 1.0e-12f)) return {};
  const V3 h = Normalize(vl);
  const float vh = Saturate(Dot(v, h));
  const V3 f = FresnelSchlick(F0(s), vh);
  return (Splat(1.0f) - f) * DiffuseColour(s) * (1.0f / kPi);
}
inline V3 SpecularLobe(const Surface& s, V3 v, V3 l) {
  const float nl = Dot(s.n, l);
  if (!(nl > 0.0f)) return {};
  const V3 vl = v + l;
  if (!(Dot(vl, vl) > 1.0e-12f)) return {};
  const float nv = std::max(Dot(s.n, v), 1.0e-4f);
  const V3 h = Normalize(vl);
  const float nh = Saturate(Dot(s.n, h));
  const float vh = Saturate(Dot(v, h));
  const float alpha = s.roughness * s.roughness;
  const V3 f0 = F0(s);
  const V3 f = FresnelSchlick(f0, vh);
  const float dv = DGgx(nh, alpha) * VSmithGgxCorrelated(nv, nl, alpha);
  V3 ms = Splat(1.0f);
  if (s.metalness > kMsMetal) ms = Splat(1.0f) + (MultiScatter(f0, s.roughness, nv) - Splat(1.0f)) * Saturate(s.metalness * kMsRamp);
  return f * ms * dv;
}
inline V3 Direct(const Surface& s, V3 v, V3 l, V3 radiance) {
  const float nl = Saturate(Dot(s.n, l));
  if (nl <= 0.0f) return {};
  return (DiffuseLobe(s, v, l) + SpecularLobe(s, v, l)) * radiance * nl;
}

// ---- a light with a size ---------------------------------------------------------------------------------------------------------------------------------------------------------

struct SphereLobes {
  V3 diffuse;      // the lobe at the centre's direction, WITHOUT its N.L
  float nl = 0.0f;  // the centre's N.L
  V3 specular;     // the lobe at the representative point x that point's N.L x (alpha / alpha')^2
};
inline SphereLobes SphereLightLobes(const Surface& s, V3 v, V3 to_light, float radius) {
  SphereLobes o;
  const float d2 = Dot(to_light, to_light);
  if (!(d2 > 1.0e-8f)) return o;
  const float dist = std::sqrt(d2);
  const V3 lc = to_light * (1.0f / dist);
  o.nl = Saturate(Dot(s.n, lc));
  o.diffuse = DiffuseLobe(s, v, lc);
  const V3 r = Reflect(-v, s.n);
  const V3 centre_to_ray = r * Dot(to_light, r) - to_light;
  const float c2 = Dot(centre_to_ray, centre_to_ray);
  const V3 closest = to_light + centre_to_ray * Saturate(radius / std::sqrt(std::max(c2, 1.0e-12f)));
  const V3 lr = Normalize(closest);
  const float nl_r = Saturate(Dot(s.n, lr));
  const float alpha = s.roughness * s.roughness;
  const float alpha_w = Saturate(alpha + radius / (2.0f * dist));
  const float norm = (alpha / alpha_w) * (alpha / alpha_w);
  o.specular = SpecularLobe(s, v, lr) * (nl_r * norm);
  return o;
}
inline V3 DirectSphere(const Surface& s, V3 v, V3 to_light, float radius, V3 radiance) {
  const SphereLobes o = SphereLightLobes(s, v, to_light, radius);
  return (o.diffuse * o.nl + o.specular) * radiance;
}

}  // namespace larecomp::gpu::pbr
