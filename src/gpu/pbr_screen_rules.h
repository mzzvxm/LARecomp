// MCLA GPU renderer -- the screen passes' side of the shared physically based model: the C++ twin of hlsl/pbr_screen_common.hlsli, and the switch's reading (PBR level 1C, perf\PBR.md "1C screen
// passes and guides").
//
// Everything here is pure, on top of PBR-1's twin of the contract (pbr_brdf_rules.h, namespace larecomp::gpu::pbr: the roughness, the metalness and the reflectance of a class, the split sum). The
// test (src/gpu/pbr_screen_test.cpp) compiles the .hlsli AND the contract's header as C++ and checks each function against this twin on random pixels, and against the definitions (a metal's weight is its
// albedo's split sum and tints; a dielectric's is grey; an emissive pixel reflects nothing; a pure metal takes no diffuse light; the calibration of the diffuse share ...).
//
// THE SWITCH. `mcla_gpu_pbr` (0 the passes' old maths, 1 the model; debug only, live, default 1) is PBR-1's setting (feat/pbr-light, 932277b5). The screen passes read it by NAME through a LiveText
// (pbr_screen_switch.h) and treat a missing setting as ON, so the accessor needs no rewiring whichever branch it is built with. EnabledOfText is the parse.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "gbuffer_types.h"
#include "pbr_brdf_rules.h"

namespace mcla::gpu::pbr_screen {

namespace pbr = larecomp::gpu::pbr;

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the switch
// ---------------------------------------------------------------------------------------------------------------------------------------------------

inline constexpr const char* kSettingName = "mcla_gpu_pbr";

// The setting's text as the registry gives it: empty (no such setting: a build without PBR-1's branch) is ON; "0", "false", "off" and "no" (any case) are OFF; anything else is ON.
inline bool EnabledOfText(std::string_view text) {
  if (text.empty()) return true;
  std::string lowered(text);
  for (char& c : lowered) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return !(lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the screen passes' functions (hlsl/pbr_screen_common.hlsli)
// ---------------------------------------------------------------------------------------------------------------------------------------------------

using Vec3 = pbr::V3;
using gbuffer::SurfaceClass;
constexpr bool Is(uint32_t cls, SurfaceClass c) { return cls == uint32_t(c); }

inline constexpr float kUnknownMetalAlbedo = 0.6f;

// PbrScreenMetalness.
inline float Metalness(uint32_t cls, bool metal_flag) { return pbr::MetalOfScreen(cls, metal_flag); }

// PbrScreenSurface (the normal is not part of the twin's weights; the contract's surface is made with the normal of the +z axis).
inline pbr::Surface MakeSurface(uint32_t cls, bool metal_flag, float smoothness, Vec3 albedo, bool have_albedo) {
  const Vec3 base = have_albedo ? albedo : Vec3{kUnknownMetalAlbedo, kUnknownMetalAlbedo, kUnknownMetalAlbedo};
  return pbr::SurfaceOfGbuffer(Vec3{0.0f, 0.0f, 1.0f}, base, smoothness, cls, metal_flag);
}

// PbrScreenReflectionWeight.
inline Vec3 ReflectionWeight(uint32_t cls, bool metal_flag, float smoothness, Vec3 albedo, bool have_albedo, float nv) {
  if (Is(cls, SurfaceClass::kEmissive)) return {0.0f, 0.0f, 0.0f};
  const Vec3 w = pbr::EnvSpecularWeight(MakeSurface(cls, metal_flag, smoothness, albedo, have_albedo), nv);
  return {pbr::Saturate(w.x), pbr::Saturate(w.y), pbr::Saturate(w.z)};
}

// PbrScreenDiffuseWeight.
inline Vec3 DiffuseWeight(uint32_t cls, bool metal_flag, float smoothness, Vec3 albedo, float nv) {
  return pbr::EnvDiffuseWeight(MakeSurface(cls, metal_flag, smoothness, albedo, true), nv);
}

// PbrScreenDiffuseShare.
inline float DiffuseShare(uint32_t cls, bool metal_flag, float smoothness, float nv) {
  const pbr::Surface s = MakeSurface(cls, metal_flag, smoothness, Vec3{1.0f, 1.0f, 1.0f}, true);
  const Vec3 w = pbr::EnvSpecularWeight(s, nv);
  const float spec = pbr::Saturate((w.x + w.y + w.z) * (1.0f / 3.0f));
  return (1.0f - s.metalness) * (1.0f - spec);
}

// PbrScreenDiffuseGuide.
inline Vec3 DiffuseGuide(uint32_t cls, bool metal_flag, Vec3 albedo, float floor_albedo) {
  const float k = 1.0f - Metalness(cls, metal_flag);
  return {std::max(albedo.x * k, floor_albedo), std::max(albedo.y * k, floor_albedo), std::max(albedo.z * k, floor_albedo)};
}

}  // namespace mcla::gpu::pbr_screen
