#pragma once
// ===========================================================================
// MCLA GPU renderer -- which of the city's textures a consumer of the hit data reads (CITY-RT M2 H2 / M1c)
// ===========================================================================
// The city texture set (city_texture_set.h) keeps resident what the streaming ring's sectors can show. What that is depends on who reads it:
//
//   kAlpha  only the textures of the cut-out triangles (the pack's second geometry: fences, grates, neon cut-outs; the runs flagged kRunAlphaTested): the ray traced sun shadows read
//           their alpha in the ray query's candidate loop (hlsl/city_hit_alpha.hlsli), nothing else. A few dozen textures a sector at most.
//   kAll    every texture a hit can show: the reflections and the global illumination shade a hit from its diffuse (M2, M3).
//
// A consumer says its scope through the scene service (rt_scene_rules.h Needs::textures), the merge takes the widest. This header has the enum and nothing else, so that the scene rules
// and the texture rules can both include it.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu {

enum class TextureScope : uint8_t { kNone = 0, kAlpha = 1, kAll = 2 };

constexpr TextureScope MaxScope(TextureScope a, TextureScope b) { return uint8_t(a) >= uint8_t(b) ? a : b; }

constexpr const char* ScopeName(TextureScope scope) {
  switch (scope) {
    case TextureScope::kAlpha: return "cut-outs only";
    case TextureScope::kAll: return "all";
    default: return "none";
  }
}

}  // namespace mcla::gpu
