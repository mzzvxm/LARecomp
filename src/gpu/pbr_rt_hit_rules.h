#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced hit shading's physically based layer, in plain C++ (PBR level 1, task PBR-1B; the shader is hlsl/pbr_rt_hit.hlsli)
// ===========================================================================
// Two things:
//
//   namespace contract   the CONTRACT's arithmetic (hlsl/pbr_brdf.hlsli): an alias of PBR-1's twin (src/gpu/pbr_brdf_rules.h, larecomp::gpu::pbr), so that the hit shading's twins
//                        (city_hit_shade_rules.h, car_rt_material_rules.h) are written against the header that ships (the integration of feat/pbr-light dropped the copy this branch started with).
//   the layer            what hlsl/pbr_rt_hit.hlsli adds: the game's light units meet the contract's radiance (a light of the game's colour is that times pi), the irradiance form for a sum
//                        of lamps, the ambient and the reflection weights.
//
// The vector type is the contract twin's (V3); the twins convert at their call sites. Nothing here includes the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "pbr_brdf_rules.h"

namespace mcla::gpu::pbr_rt {

// The debug switch of the whole model (PBR-1's setting) and the parse of its text as the registry gives it (pbr_rt_hit_switch.h reads it): no such setting (empty: a build without PBR-1's branch) is ON, "0", "false",
// "off" and "no" (any case) are OFF, anything else is ON.
inline constexpr const char* kSettingName = "mcla_gpu_pbr";
inline bool SwitchOn(std::string_view text) {
  if (text.empty()) return true;
  std::string lowered(text);
  for (char& c : lowered) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return !(lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no");
}

// PBR-1's twin (src/gpu/pbr_brdf_rules.h, the C++ twin of hlsl/pbr_brdf.hlsli) is the contract: this file's own copy of the base header's arithmetic is gone (integration of feat/pbr-light), so the
// hit shading's twins are written against the header that ships. Same names, same order; pbr_brdf_test.cpp holds the twin to the header's text and to the laws of physics.
namespace contract = larecomp::gpu::pbr;
using V3 = contract::V3;
using contract::Dot;
using contract::Lerp;
using contract::Normalize;
using contract::Saturate;

// ---------------------------------------------------------------------------
// the layer (hlsl/pbr_rt_hit.hlsli)
// ---------------------------------------------------------------------------

using Surface = contract::Surface;
inline constexpr float kMinNdl = 0.05f;   // hlsl kPbrRtMinNdl

// A light of the game's colour (the sun's times its visibility) from `l`, seen from `v`: PbrDirect with the colour times pi (the game's lit shaders have no 1/pi).
inline V3 Direct(const Surface& s, V3 v, V3 l, V3 light_colour) { return contract::Direct(s, v, l, light_colour * contract::kPi); }
// An irradiance with N.L in it already (a sum of lamps) from the mean direction `l`.
inline V3 DirectIrradiance(const Surface& s, V3 v, V3 l, V3 irradiance) {
  return contract::Direct(s, v, l, irradiance * (contract::kPi / std::max(Dot(s.n, l), kMinNdl)));
}
// The ambient and the other irradiance sums in the game's units.
inline V3 Diffuse(const Surface& s, float nv, V3 light) { return contract::EnvDiffuseWeight(s, nv) * light; }
// A reflection's radiance times the split-sum weight.
inline V3 Reflection(const Surface& s, float nv, V3 radiance) { return contract::EnvSpecularWeight(s, nv) * radiance; }

}  // namespace mcla::gpu::pbr_rt
