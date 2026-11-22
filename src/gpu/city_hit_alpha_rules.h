#pragma once
// ===========================================================================
// MCLA GPU renderer -- the exact alpha of a cut-out triangle: the C++ twin of hlsl/city_hit_alpha.hlsli (CITY-RT M1c)
// ===========================================================================
// The ray traced sun shadows read the alpha of the texture a cut-out candidate (fence, grate, neon cut-out) has, at the candidate's own texture coordinates, to decide whether the ray is
// blocked (city_rt_shadow_rules.h AlphaMode::kExact, hlsl/city_rt_shadow_cs.hlsl). This header is the same arithmetic in plain C++, so that a test can check the shader's text (compiled as C++
// by city_hit_alpha_rules_test.cpp, through a shim of the HLSL types) against it, and so that the numbers have one explanation:
//
//   THE FOOTPRINT  a shadow ray's width at its hit: the pixel it started from (footprint_unit x the receiver's distance, metres) plus the sun disc's spread over the distance travelled (the
//                  disc's diameter, 2 tan(radius), times the distance). In texels: that width x the triangle's texture repeats a metre (the square root of its uv area over its world area) x
//                  the texture's size (the geometric mean of its sides).
//   THE LEVEL      log2 of the texels, rounded; never coarser than the lowest level that keeps the shape of a cut-out: a level with fewer than 2^kShapeMinLog2 = 8 texels on its short side has
//                  averaged the wires of a fence and the bars of a grate to a grey that a reference cuts all or nothing, so the limit is floor(log2(short side)) - 3 levels down from the full
//                  size (0 for a texture of 8 texels or fewer on a side). The texture the GPU holds starts at the class's cap (its first resident level): the level read is counted from there
//                  (a level above it reads the first, a level beyond the stored chain reads the last).
//   THE ALPHA      the level's alpha bilinear at the uv (texture repeats), the texel coordinates wrapped: the way the game's own sampler reads a tiled cut-out. The ray is blocked where it is at
//                  or above the reference (city_rt_shadow_rules.h kDefaultAlphaReference).
//
// Nothing here includes the renderer: the test links nothing but this header.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace mcla::gpu::city_hit_alpha {

// hlsl/city_hit_alpha.hlsli CITY_HIT_ALPHA_SHAPE_MIN_LOG2.
inline constexpr int kShapeMinLog2 = 3;

// The width, in metres, of the shadow ray at a hit `distance_m` along it: the pixel it started from (`pixel_m`, footprint_unit x the receiver's distance) and the sun disc's diameter over the
// distance. `sun_tan` is the tangent of the disc's radius times the setting's scale (the push's light.w).
inline float ConeWidth(float pixel_m, float distance_m, float sun_tan) { return pixel_m + 2.0f * distance_m * sun_tan; }

// Texels of the ray's footprint on a texture of width x height: cone x repeats a metre x the geometric mean of the sides (hlsl CityHitAlphaTexels; `repeats_per_metre` is CityHitUvDensity).
inline float Texels(float cone_m, float repeats_per_metre, uint32_t width, uint32_t height) { return cone_m * repeats_per_metre * std::sqrt(float(width) * float(height)); }

// The coarsest level, counted from the texture's full size, that keeps a cut-out's shape (hlsl CityHitAlphaShapeLimit).
inline int ShapeLimit(uint32_t width, uint32_t height) {
  const int short_side = int(std::min(width, height));
  const int log2_short = int(std::floor(std::log2(float(std::max(short_side, 1)))));
  return std::max(log2_short - kShapeMinLog2, 0);
}

// The level to read counted from the first resident one (hlsl CityHitAlphaLevel).
inline uint32_t Level(uint32_t width, uint32_t height, uint32_t mip_count, uint32_t first_mip, float texels) {
  const int wanted = std::min(int(std::floor(std::log2(std::max(texels, 1.0f)) + 0.5f)), ShapeLimit(width, height));
  const int resident = std::max(int(mip_count), 1);
  return uint32_t(std::clamp(wanted - int(first_mip), 0, resident - 1));
}

// The alpha of one texel: `texel(x, y)` of the level, already inside it. Bilinear at `uv` over the level's size (hlsl CityHitAlphaBilinear); the wrap is the same for every texel.
inline float Bilinear(const std::function<float(int, int)>& texel, float u, float v, uint32_t width, uint32_t height, uint32_t first_mip, uint32_t level) {
  const int at = int(first_mip) + int(level);
  const int w = std::max(int(width) >> at, 1);
  const int h = std::max(int(height) >> at, 1);
  const float px = u * float(w) - 0.5f;
  const float py = v * float(h) - 0.5f;
  const float fx = std::floor(px);
  const float fy = std::floor(py);
  const float tx = px - fx;
  const float ty = py - fy;
  const int x0 = int(fx);
  const int y0 = int(fy);
  const auto wrap = [](int c, int size) { return ((c % size) + size) % size; };
  const auto read = [&](int x, int y) { return texel(wrap(x, w), wrap(y, h)); };
  const float a = read(x0, y0), b = read(x0 + 1, y0), c = read(x0, y0 + 1), d = read(x0 + 1, y0 + 1);
  const auto lerp = [](float p, float q, float t) { return p + (q - p) * t; };
  return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

// The ray is a blocker at this alpha when it reaches the reference (hlsl CityHitAlphaBlocks).
inline bool Blocks(float alpha, float reference) { return alpha >= reference; }

}  // namespace mcla::gpu::city_hit_alpha
