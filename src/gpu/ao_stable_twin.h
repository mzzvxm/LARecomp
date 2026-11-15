#pragma once
// ===========================================================================
// MCLA GPU renderer -- the occlusion's and the bounce light's passes as a C++ twin (test only, no GPU)
// ===========================================================================
// ao_stable.h holds the rules that make the occlusion and the bounce light steady (pure maths the shaders and
// ao.cpp repeat); this file is what ao_stable_test.cpp measures them on: a C++ twin of the bounce light's main pass
// (hlsl/ao_main_ps.hlsl, AO_GI=1: the visibility bitmask, the near samples and the far ring), of the 3x3 .. 7x7
// bilateral blur (ao_common.hlsli AoBilateral4), of the accumulation over frames (ao_temporal_ps.hlsl, AO_GI) for
// a camera that stands still, of the a-trous filter (ao_atrous_ps.hlsl) and of the apply's gain (ssgi_light.h
// BounceFactor), over a scene made of a road, a wall and a car with a texture on them. Nothing here is the
// renderer's: it exists so that a change of one of the rules shows as a number (the temporal standard deviation of
// what the picture is multiplied by, with the camera still) before the game runs.
//
// It reuses ao_halo.h's scene geometry types (Scene, Params, the noise, the depth chain's maths).
// ===========================================================================

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "ao_fast.h"
#include "ao_halo.h"
#include "ao_stable.h"
#include "ssgi_light.h"
#include "ssgi_stable.h"

namespace mcla::gpu::ao_stable_twin {

using ao_halo::Cross;
using ao_halo::Dot;
using ao_halo::Frac;
using ao_halo::Length;
using ao_halo::Normalize;
using ao_halo::Params;
using ao_halo::Saturate;
using ao_halo::V3;
using ssgi_light::Rgb;

// ---------------------------------------------------------------------------
// the images the main pass reads: the depth and colour chains of one still picture
// ---------------------------------------------------------------------------

struct Chains {
  int width = 0;
  int height = 0;
  float tan_x = 0.0f;
  float tan_y = 0.0f;
  std::vector<float> depth[3];     // level 0, 1 (1/2), 2 (1/4): linear depth
  std::vector<Rgb> colour[3];      // the same levels of the carried colour
  std::vector<uint8_t> vehicle;    // level 0
  int w[3] = {0, 0, 0};
  int h[3] = {0, 0, 0};
};

struct ChainInput {
  int width = 1280;
  int height = 720;
  float tan_y = 0.5773503f;
  std::function<float(int, int)> depth;       // linear depth of the pixel (60000: sky)
  std::function<Rgb(int, int)> colour;        // the scene colour of the pixel at the trigger
  std::function<bool(int, int)> vehicle;
  float radiance_cap = 1.0f;                  // luma soft clip of the carried colour (ssgi_light::CapRadiance)
};

inline Chains BuildChains(const ChainInput& in, const Params& p) {
  Chains c;
  c.width = in.width;
  c.height = in.height;
  c.tan_y = in.tan_y;
  c.tan_x = in.tan_y * float(in.width) / float(in.height);
  c.w[0] = in.width;
  c.h[0] = in.height;
  c.w[1] = (in.width + 1) / 2;
  c.h[1] = (in.height + 1) / 2;
  c.w[2] = (c.w[1] + 1) / 2;
  c.h[2] = (c.h[1] + 1) / 2;
  for (int l = 0; l < 3; ++l) {
    c.depth[l].assign(size_t(c.w[l]) * size_t(c.h[l]), ao_halo::kSky);
    c.colour[l].assign(size_t(c.w[l]) * size_t(c.h[l]), Rgb{});
  }
  c.vehicle.assign(size_t(in.width) * size_t(in.height), 0);
  for (int y = 0; y < in.height; ++y) {
    for (int x = 0; x < in.width; ++x) {
      const size_t i = size_t(y) * size_t(in.width) + size_t(x);
      c.depth[0][i] = in.depth(x, y);
      Rgb col = in.colour(x, y);
      col = ssgi_light::CapRadiance({std::max(col.r, 0.0f), std::max(col.g, 0.0f), std::max(col.b, 0.0f)},
                                    in.radiance_cap);
      c.colour[0][i] = col;
      c.vehicle[i] = in.vehicle && in.vehicle(x, y) ? 1 : 0;
    }
  }
  const float range = std::max(0.75f * p.radius * p.ratio, 0.01f);
  for (int l = 1; l < 3; ++l) {
    for (int y = 0; y < c.h[l]; ++y) {
      for (int x = 0; x < c.w[l]; ++x) {
        float d[4];
        Rgb s[4];
        int k = 0;
        for (int j = 0; j < 2; ++j) {
          for (int i = 0; i < 2; ++i) {
            const int xx = std::min(x * 2 + i, c.w[l - 1] - 1);
            const int yy = std::min(y * 2 + j, c.h[l - 1] - 1);
            d[k] = c.depth[l - 1][size_t(yy) * size_t(c.w[l - 1]) + size_t(xx)];
            s[k] = c.colour[l - 1][size_t(yy) * size_t(c.w[l - 1]) + size_t(xx)];
            ++k;
          }
        }
        const float far_d = std::max(std::max(d[0], d[1]), std::max(d[2], d[3]));
        float sum = 0.0f;
        float weights = 0.0f;
        for (int q = 0; q < 4; ++q) {
          const float w = Saturate(1.0f - (far_d - d[q]) / range);
          sum += w * d[q];
          weights += w;
        }
        const size_t o = size_t(y) * size_t(c.w[l]) + size_t(x);
        c.depth[l][o] = std::min(sum / std::max(weights, 1e-6f), ao_halo::kSky);
        c.colour[l][o] = {(s[0].r + s[1].r + s[2].r + s[3].r) * 0.25f, (s[0].g + s[1].g + s[2].g + s[3].g) * 0.25f,
                          (s[0].b + s[1].b + s[2].b + s[3].b) * 0.25f};
      }
    }
  }
  return c;
}

inline float ChainDepthAt(const Chains& c, int level, int x, int y) {
  const int xx = std::clamp(x, 0, c.w[level] - 1);
  const int yy = std::clamp(y, 0, c.h[level] - 1);
  return c.depth[level][size_t(yy) * size_t(c.w[level]) + size_t(xx)];
}
inline Rgb ChainColourAt(const Chains& c, int level, int x, int y) {
  const int xx = std::clamp(x, 0, c.w[level] - 1);
  const int yy = std::clamp(y, 0, c.h[level] - 1);
  return c.colour[level][size_t(yy) * size_t(c.w[level]) + size_t(xx)];
}
inline float Depth0(const Chains& c, int x, int y) { return ChainDepthAt(c, 0, x, y); }
inline V3 ViewPosOf(const Chains& c, float cx, float cy, float t) {
  const float u = cx / float(c.width);
  const float v = cy / float(c.height);
  return {(u * 2.0f - 1.0f) * c.tan_x * t, (1.0f - v * 2.0f) * c.tan_y * t, t};
}
inline V3 NormalOf(const Chains& c, int px, int py) {
  const float tc = Depth0(c, px, py);
  const V3 pc = ViewPosOf(c, float(px) + 0.5f, float(py) + 0.5f, tc);
  const int xl = std::max(px - 1, 0), xr = std::min(px + 1, c.width - 1);
  const int yt = std::max(py - 1, 0), yb = std::min(py + 1, c.height - 1);
  const float tl = Depth0(c, xl, py), tr = Depth0(c, xr, py), tt = Depth0(c, px, yt), tb = Depth0(c, px, yb);
  const V3 h = std::fabs(tl - tc) < std::fabs(tr - tc) ? pc - ViewPosOf(c, float(xl) + 0.5f, float(py) + 0.5f, tl)
                                                       : ViewPosOf(c, float(xr) + 0.5f, float(py) + 0.5f, tr) - pc;
  const V3 v = std::fabs(tt - tc) < std::fabs(tb - tc) ? ViewPosOf(c, float(px) + 0.5f, float(yt) + 0.5f, tt) - pc
                                                       : pc - ViewPosOf(c, float(px) + 0.5f, float(yb) + 0.5f, tb);
  return Normalize(Cross(v, h));
}

// ---------------------------------------------------------------------------
// the main pass (hlsl/ao_main_ps.hlsl, AO_GI=1)
// ---------------------------------------------------------------------------

struct GiParams {
  Params ao;
  float thickness = 0.0f;
  float facing_weight = 1.0f;
  float far_reach = 9.0f;  // metres; 0: no ring
  int far_steps = 2;
  // The level (0, 1, 2) a sample at `length_px` reads: log2(length) - 3 clamped (the shader's rule), then `bias` more
  // levels where the chain has them (the twin has three).
  int level_bias = 0;
  // The frame's shift of the noise: 0 the additive recurrence of the R2 sequence (the first version), 13 the Fibonacci
  // lattice (n/13, 8n/13) (ao_stable.h NoiseShift).
  int noise_lattice = 0;
  // The shader's cheaper forms of the same arithmetic (ao_fast.h, "THE SAME WORK"): the reciprocal of the image's size and of
  // the distance in place of divisions, the surface's tangents as -sin n and sin n, the sample's level from comparing its length
  // in pixels with 16 and 32. False: the forms of before (what every other test of the twin uses).
  bool lossless = false;
  // The polynomial acos of the sectors (ao_fast.h AcosFast; hlsl AO_FAST_ACOS) in place of the exact one.
  bool fast_acos = false;
};

struct GiOut {
  float visibility = 1.0f;
  Rgb bounce;
};

inline int ClampLevel(float level_f, int lo, int hi) {
  return int(std::min(std::max(level_f, float(lo)), float(hi)));
}

// Where the signed angle `theta` falls in the slice's 32 sectors, 0 to 1 (AoSectorPos), with the arc's numbers of the slice.
inline float SectorPos(float theta, float n, float sin_n, float cos_n, float g_end, float inv_total) {
  const float t = n + std::clamp(theta - n, -ao_halo::kHalfPi, ao_halo::kHalfPi);
  const float g = (cos_n - std::cos(2.0f * t - n) + 2.0f * t * sin_n) * 0.25f;
  const float below = t < 0.0f ? g_end - g : g_end + g;
  return Saturate(below * inv_total);
}

// One sample of one side of a slice (AoGiSample). `lossless` and `fast_acos` as GiParams.
inline void GiSample(const Chains& c, float px, float py, int level, float ax, float ay, float side, float low, V3 pc,
                     V3 view_vec, V3 axis, float falloff_mul, float falloff_add, float n, float sin_n, float cos_n,
                     float g_end, float inv_total, float thickness, float facing_weight, bool limit_vehicles,
                     float vehicle_mul, float vehicle_add, uint32_t* occupied, Rgb* light, bool lossless = false,
                     bool fast_acos = false) {
  if (!(px >= 0.0f && py >= 0.0f && px < float(c.width) && py < float(c.height))) {
    return;
  }
  // AoViewPosInv: the centre times 2 / size (the clip space's scale, made once for the pass) where AoViewPos divides by the size.
  const float scale_x = 2.0f / float(c.width), scale_y = 2.0f / float(c.height);
  const auto view_pos = [&](float cx, float cy, float t) {
    if (!lossless) {
      return ViewPosOf(c, cx, cy, t);
    }
    return V3{(cx * scale_x - 1.0f) * c.tan_x * t, (1.0f - cy * scale_y) * c.tan_y * t, t};
  };
  const auto angle_of = [&](float cosine) {
    return fast_acos ? ao_fast::AcosFast(cosine) : ao_fast::AcosExact(cosine);
  };
  const int ix = int(std::floor(px)), iy = int(std::floor(py));
  const float td = ChainDepthAt(c, level, ix >> level, iy >> level);
  const V3 sp = view_pos(float(ix) + 0.5f, float(iy) + 0.5f, td);
  const V3 delta = sp - pc;
  const float dist = Length(delta);
  float w = Saturate(dist * falloff_mul + falloff_add);
  if (limit_vehicles && w > 0.0f && c.vehicle[size_t(iy) * size_t(c.width) + size_t(ix)] != 0) {
    w = std::min(w, Saturate(dist * vehicle_mul + vehicle_add));
  }
  const float inv_dist = 1.0f / std::max(dist, 1e-6f);
  const V3 dir = delta * inv_dist;
  const float cos_front = std::max(low + (Dot(dir, view_vec) - low) * w, low);
  const float u_front = SectorPos(side * angle_of(cos_front), n, sin_n, cos_n, g_end, inv_total);
  float u_back = side > 0.0f ? 1.0f : 0.0f;
  if (thickness > 0.0f) {
    const V3 behind = delta + sp * (thickness / std::max(Length(sp), 1e-6f));
    const float cos_back = std::max(low + (Dot(Normalize(behind), view_vec) - low) * w, low);
    u_back = SectorPos(side * angle_of(cos_back), n, sin_n, cos_n, g_end, inv_total);
  }
  const float lo = std::min(u_front, u_back), hi = std::max(u_front, u_back);
  const uint32_t start = uint32_t(lo * 32.0f + 0.5f);
  const uint32_t stop = uint32_t(hi * 32.0f + 0.5f);
  uint32_t width = stop > start ? stop - start : 0u;
  if (width == 0u && hi > lo + 1e-4f) {
    width = 1u;
  }
  uint32_t bits = 0;
  if (!(start >= 32u || width == 0u)) {
    width = std::min(width, 32u - start);
    bits = (width >= 32u ? 0xFFFFFFFFu : ((1u << width) - 1u)) << start;
  }
  const uint32_t fresh = bits & ~*occupied;
  *occupied |= bits;
  if (fresh == 0u) {
    return;
  }
  float facing = 1.0f;
  if (facing_weight > 0.0f) {
    const float p2x = px + ax * float(1 << level), p2y = py + ay * float(1 << level);
    const int i2x = int(std::floor(p2x)), i2y = int(std::floor(p2y));
    const float t2 = ChainDepthAt(c, level, i2x >> level, i2y >> level);
    const V3 tangent = view_pos(float(i2x) + 0.5f, float(i2y) + 0.5f, t2) - sp;
    if (t2 < ao_halo::kSky * 0.5f && Dot(tangent, tangent) > 1e-10f) {
      V3 normal_s = Cross(tangent, axis);
      const float length_n = Length(normal_s);
      if (length_n > 1e-6f) {
        normal_s = normal_s * (1.0f / length_n);
        if (Dot(normal_s, sp) > 0.0f) {
          normal_s = normal_s * -1.0f;
        }
        facing = 1.0f + (Saturate(4.0f * Dot(normal_s, dir * -1.0f)) - 1.0f) * facing_weight;
      }
    }
  }
  const Rgb col = ChainColourAt(c, level, ix >> level, iy >> level);
  const float k = float(std::popcount(fresh)) * (1.0f / 32.0f) * facing;
  light->r += k * col.r;
  light->g += k * col.g;
  light->b += k * col.b;
}

// The bounce light's main pass at pixel (px, py) in frame `frame`.
inline GiOut GiMain(const Chains& c, const GiParams& g, int px, int py, int frame) {
  const Params& p = g.ao;
  GiOut out;
  const float tc = Depth0(c, px, py);
  if (tc >= ao_halo::kSky * 0.5f) {
    return out;
  }
  const bool receiver_vehicle = c.vehicle[size_t(py) * size_t(c.width) + size_t(px)] != 0;
  const V3 pc = ViewPosOf(c, float(px) + 0.5f, float(py) + 0.5f, tc);
  const V3 normal = NormalOf(c, px, py);
  const V3 view_vec = Normalize(pc * -1.0f);
  const float radius = p.radius;
  const float pixels_per_unit = 0.5f * float(c.height) / (c.tan_y * tc);
  const float radius_px = std::min(radius * pixels_per_unit, ao_halo::kMaxRadiusShare * float(c.height));
  if (radius_px < ao_halo::kMinOffsetPixels + 0.5f) {
    return out;
  }
  const float ratio = std::max(p.ratio, 0.01f);
  const float falloff_mul = -1.0f / (ratio * radius);
  const float falloff_add = 1.0f / ratio;
  const float car_share = p.car_reach;
  const bool limit_vehicles = car_share > 0.001f && car_share < 0.999f && !receiver_vehicle;
  const float car_radius = std::max(car_share * radius, 0.05f);
  const float vehicle_mul = -1.0f / (ratio * car_radius);
  const float vehicle_add = 1.0f / ratio;

  const int slices = std::max(p.slices, 1);
  const int steps = std::max(p.steps, 1);
  const float spatial_x = ao_halo::Noise(float(px), float(py));
  const float spatial_y = ao_halo::Noise(float(py) + 31.0f, float(px) + 11.0f);
  float noise_x, noise_y;
  if (g.noise_lattice == 13) {
    const uint32_t m = uint32_t(frame) % 13u;
    noise_x = Frac(spatial_x + float(m) / 13.0f);
    noise_y = Frac(spatial_y + float((m * 8u) % 13u) / 13.0f);
  } else {
    const float fr = float(frame & 1023);
    noise_x = Frac(spatial_x + fr * 0.75487766624669f);
    noise_y = Frac(spatial_y + fr * 0.56984029099806f);
  }
  const float min_offset = ao_halo::kMinOffsetPixels / radius_px;

  const float far_reach = std::max(g.far_reach, 0.0f);
  const int far_steps = g.far_steps;
  const float near_end_px = radius_px * (1.0f + min_offset);
  const float far_px = std::min(far_reach * pixels_per_unit, ssgi_light::kFarMaxRadiusShare * float(c.height));
  const bool far_ring = far_steps > 0 && far_px > near_end_px * ssgi_light::kFarMinGrowth + ssgi_light::kFarMinExtraPx;
  const float far_falloff_mul = -1.0f / (ratio * std::max(far_reach, 1e-3f));

  float visibility = 0.0f;
  Rgb bounce;
  float open_sum = 0.0f;
  for (int slice = 0; slice < slices; ++slice) {
    const float phi = (float(slice) + noise_x) / float(slices) * ao_halo::kPi;
    const float dpx = std::cos(phi), dpy = std::sin(phi);
    const V3 dir_view{dpx, -dpy, 0.0f};
    const V3 ortho = dir_view - view_vec * Dot(dir_view, view_vec);
    const V3 axis = Normalize(Cross(ortho, view_vec));
    const V3 projected = normal - axis * Dot(normal, axis);
    const float projected_len = Length(projected);
    const float dd = Dot(ortho, projected);
    const float sign_n = dd > 0.0f ? 1.0f : (dd < 0.0f ? -1.0f : 0.0f);
    const float cos_n = Saturate(Dot(projected, view_vec) / std::max(projected_len, 1e-6f));
    const float n = sign_n * std::acos(cos_n);
    const float sin_n = std::sin(n);
    // The surface's tangent on each side: the cosine of n plus or minus a quarter turn (minus and plus the sine of n, the form of
    // the shader's cheaper compilation).
    const float low0 = g.lossless ? -sin_n : std::cos(n + ao_halo::kHalfPi);
    const float low1 = g.lossless ? sin_n : std::cos(n - ao_halo::kHalfPi);
    const float total = cos_n + n * sin_n;
    const float inv_total = 1.0f / std::max(total, 1e-3f);
    const float g_end = (2.0f * cos_n + (2.0f * n - ao_halo::kPi) * sin_n) * 0.25f;

    uint32_t occupied = 0;
    Rgb light;
    for (int s = 0; s < steps; ++s) {
      const float step_noise = Frac(noise_y + float(slice + s * slices) * 0.6180339887f);
      float share = (float(s) + step_noise) / float(steps);
      share = share * share + min_offset;
      const float ox = dpx * share * radius_px, oy = dpy * share * radius_px;
      int level;
      if (g.lossless && g.level_bias == 0) {
        // dir is a unit vector: the offset's length is the share of the radius; level 0 under 16 pixels, 1 under 32, 2 from there.
        const float length_px = share * radius_px;
        level = (length_px >= 16.0f ? 1 : 0) + (length_px >= 32.0f ? 1 : 0);
      } else {
        const float level_f = std::clamp(std::log2(std::max(std::hypot(ox, oy), 1.0f)) - 3.0f + float(g.level_bias), 0.0f, 2.0f);
        level = int(level_f);
      }
      GiSample(c, float(px) + 0.5f + ox, float(py) + 0.5f + oy, level, dpx, dpy, 1.0f, low0, pc, view_vec, axis,
               falloff_mul, falloff_add, n, sin_n, cos_n, g_end, inv_total, g.thickness, g.facing_weight,
               limit_vehicles, vehicle_mul, vehicle_add, &occupied, &light, g.lossless, g.fast_acos);
      GiSample(c, float(px) + 0.5f - ox, float(py) + 0.5f - oy, level, -dpx, -dpy, -1.0f, low1, pc, view_vec, axis,
               falloff_mul, falloff_add, n, sin_n, cos_n, g_end, inv_total, g.thickness, g.facing_weight,
               limit_vehicles, vehicle_mul, vehicle_add, &occupied, &light, g.lossless, g.fast_acos);
    }
    const uint32_t occupied_near = occupied;
    if (far_ring) {
      for (int s = 0; s < far_steps; ++s) {
        const float step_noise = Frac(noise_y + float(slice + (steps + s) * slices) * 0.6180339887f);
        float share = (float(s) + step_noise) / float(far_steps);
        share *= share;
        const float length_px = near_end_px + (far_px - near_end_px) * share;
        const float ox = dpx * length_px, oy = dpy * length_px;
        const int level = g.lossless && g.level_bias == 0
                              ? (length_px >= 32.0f ? 2 : 1)
                              : ClampLevel(std::log2(std::max(length_px, 1.0f)) - 3.0f + float(g.level_bias), 1, 2);
        GiSample(c, float(px) + 0.5f + ox, float(py) + 0.5f + oy, level, dpx, dpy, 1.0f, low0, pc, view_vec, axis,
                 far_falloff_mul, falloff_add, n, sin_n, cos_n, g_end, inv_total, g.thickness, g.facing_weight,
                 limit_vehicles, vehicle_mul, vehicle_add, &occupied, &light, g.lossless, g.fast_acos);
        GiSample(c, float(px) + 0.5f - ox, float(py) + 0.5f - oy, level, -dpx, -dpy, -1.0f, low1, pc, view_vec, axis,
                 far_falloff_mul, falloff_add, n, sin_n, cos_n, g_end, inv_total, g.thickness, g.facing_weight,
                 limit_vehicles, vehicle_mul, vehicle_add, &occupied, &light, g.lossless, g.fast_acos);
      }
    }
    const float weight = projected_len * total;
    visibility += weight * float(32 - std::popcount(occupied_near)) * (1.0f / 32.0f);
    bounce.r += weight * light.r;
    bounce.g += weight * light.g;
    bounce.b += weight * light.b;
    open_sum += weight;
  }
  const float inv_open = 1.0f / std::max(open_sum, 1e-3f);
  visibility = Saturate(visibility * inv_open);
  out.visibility = std::max(std::pow(visibility, p.power), p.floor);
  out.bounce = {std::min(std::max(bounce.r * inv_open, 0.0f), 60000.0f), std::min(std::max(bounce.g * inv_open, 0.0f), 60000.0f),
                std::min(std::max(bounce.b * inv_open, 0.0f), 60000.0f)};
  return out;
}


// ---------------------------------------------------------------------------
// the passes after the main pass, for a camera that stands still
// ---------------------------------------------------------------------------

struct Px4 {
  float v = 1.0f;  // the visibility
  Rgb b;           // the bounce
};

// A rectangle of the picture the passes work on (borders included: a pass reads its neighbours, an edge clamps).
struct Region {
  int x0 = 0;
  int y0 = 0;
  int w = 0;
  int h = 0;
  size_t Index(int x, int y) const {
    const int xx = std::clamp(x - x0, 0, w - 1);
    const int yy = std::clamp(y - y0, 0, h - 1);
    return size_t(yy) * size_t(w) + size_t(xx);
  }
  bool Contains(int x, int y) const { return x >= x0 && y >= y0 && x < x0 + w && y < y0 + h; }
  size_t Count() const { return size_t(w) * size_t(h); }
};

// Per pixel of the region: the distance, the position and the normal (what the bilateral and the a-trous read).
struct Geometry {
  Region region;
  std::vector<float> t;
  std::vector<V3> pos;
  std::vector<V3> normal;
};

inline Geometry BuildGeometry(const Chains& c, const Region& r) {
  Geometry g;
  g.region = r;
  g.t.resize(r.Count());
  g.pos.resize(r.Count());
  g.normal.resize(r.Count());
  for (int y = 0; y < r.h; ++y) {
    for (int x = 0; x < r.w; ++x) {
      const size_t i = size_t(y) * size_t(r.w) + size_t(x);
      const int px = r.x0 + x, py = r.y0 + y;
      g.t[i] = Depth0(c, px, py);
      g.pos[i] = ViewPosOf(c, float(px) + 0.5f, float(py) + 0.5f, g.t[i]);
      g.normal[i] = NormalOf(c, px, py);
    }
  }
  return g;
}

// ao_common.hlsli AoBilateral4: radius `radius`, the plane weight from the pixel's normal, the spatial sigma by `sigma`.
inline Px4 Bilateral4(const Geometry& g, const std::vector<Px4>& src, int px, int py, int radius, float sigma) {
  const Region& r = g.region;
  const size_t k = r.Index(px, py);
  const float plane_sigma = 0.012f * g.t[k] + 0.02f;
  float sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f, weights = 0.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const size_t q = r.Index(px + dx, py + dy);
      const float off = Dot(g.normal[k], g.pos[q] - g.pos[k]) / plane_sigma;
      const float w = std::exp(-0.5f * off * off) * std::exp(-0.5f * float(dx * dx + dy * dy) / (sigma * sigma));
      sv += src[q].v * w;
      sr += src[q].b.r * w;
      sg += src[q].b.g * w;
      sb += src[q].b.b * w;
      weights += w;
    }
  }
  const float inv = 1.0f / std::max(weights, 1e-8f);
  Px4 o;
  o.v = sv * inv;
  o.b = {sr * inv, sg * inv, sb * inv};
  return o;
}

// The spatial sigma of the blur of radius r (ao_stable.h BlurSigma: 0.9 for 1, 1.4 for 2, 1.8 for 3, 2.2 for 4).
inline float BlurSigma(int radius) { return ao_stable::BlurSigma(radius); }

// The a-trous iteration of ao_atrous_ps.hlsl: the bounce filtered, the visibility untouched.
inline Px4 Atrous(const Geometry& g, const std::vector<Px4>& src, int px, int py, int step) {
  static const float kTap[3] = {0.375f, 0.25f, 0.0625f};
  const Region& r = g.region;
  const size_t k = r.Index(px, py);
  const float plane_sigma = 0.006f * g.t[k] + 0.01f;
  Rgb sum = {src[k].b.r * kTap[0] * kTap[0], src[k].b.g * kTap[0] * kTap[0], src[k].b.b * kTap[0] * kTap[0]};
  float weights = kTap[0] * kTap[0];
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int qx = px + dx * step, qy = py + dy * step;
      if (!r.Contains(qx, qy)) {
        continue;
      }
      const size_t q = r.Index(qx, qy);
      if (g.t[q] >= ao_halo::kSky * 0.5f) {
        continue;
      }
      const float off = Dot(g.normal[k], g.pos[q] - g.pos[k]) / plane_sigma;
      const float w = kTap[std::abs(dx)] * kTap[std::abs(dy)] * std::exp(-0.5f * off * off);
      sum.r += src[q].b.r * w;
      sum.g += src[q].b.g * w;
      sum.b += src[q].b.b * w;
      weights += w;
    }
  }
  Px4 o;
  o.v = src[k].v;
  o.b = {sum.r / weights, sum.g / weights, sum.b / weights};
  return o;
}

// The accumulation over frames (ao_temporal_ps.hlsl, AO_GI) with the camera still: the history is the same pixel, the
// confidence 1. `rule` picks the clip box of the bounce.
struct AccumRules {
  int vis_cap = 12;        // frames the visibility averages (mcla_gpu_ao_history)
  int bounce_cap = 16;     // frames the bounce averages (mcla_gpu_ssgi_test_history)
  float clip_width = 1.25f;       // the bounce's box: this many deviations of the 3x3 neighbourhood ...
  float clip_floor_share = 0.05f; // ... never under this share of its mean
  float clip_margin = 0.0f;       // ... plus this share of the history's own value (0: the shader's)
  float vis_margin = 0.08f;       // the visibility's clamp margin
  bool clip = true;
};

inline Px4 Accumulate(const Geometry& g, const std::vector<Px4>& current, const std::vector<Px4>* history, int age,
                      int px, int py, const AccumRules& rules, int wide_radius) {
  const Region& r = g.region;
  const size_t k = r.Index(px, py);
  const Px4& cur = current[k];
  if (!history) {
    return Bilateral4(g, current, px, py, wide_radius, BlurSigma(wide_radius));
  }
  Px4 h = (*history)[k];
  float lo = cur.v, hi = cur.v;
  Rgb m1, m2;
  float wsum = 0.0f;
  const float plane_sigma = 0.012f * g.t[k] + 0.02f;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const size_t q = r.Index(px + dx, py + dy);
      lo = std::min(lo, current[q].v);
      hi = std::max(hi, current[q].v);
      const float off = Dot(g.normal[k], g.pos[q] - g.pos[k]) / plane_sigma;
      const float w = std::exp(-0.5f * off * off);
      m1.r += current[q].b.r * w; m1.g += current[q].b.g * w; m1.b += current[q].b.b * w;
      m2.r += current[q].b.r * current[q].b.r * w; m2.g += current[q].b.g * current[q].b.g * w;
      m2.b += current[q].b.b * current[q].b.b * w;
      wsum += w;
    }
  }
  m1 = {m1.r / wsum, m1.g / wsum, m1.b / wsum};
  const Rgb spread = {std::sqrt(std::max(m2.r / wsum - m1.r * m1.r, 0.0f)), std::sqrt(std::max(m2.g / wsum - m1.g * m1.g, 0.0f)),
                      std::sqrt(std::max(m2.b / wsum - m1.b * m1.b, 0.0f))};
  h.v = std::clamp(h.v, lo - rules.vis_margin, hi + rules.vis_margin);
  if (rules.clip) {
    const auto clip1 = [&](float hist, float mean, float dev) {
      const float half = rules.clip_width * dev + rules.clip_floor_share * mean + rules.clip_margin * hist;
      return std::clamp(hist, std::max(mean - half, 0.0f), mean + half);
    };
    h.b.r = clip1(h.b.r, m1.r, spread.r);
    h.b.g = clip1(h.b.g, m1.g, spread.g);
    h.b.b = clip1(h.b.b, m1.b, spread.b);
  }
  const float wv = 1.0f - 1.0f / float(std::min(age + 2, rules.vis_cap));
  const float wb = 1.0f - 1.0f / float(std::min(age + 2, rules.bounce_cap));
  Px4 o;
  o.v = Saturate(cur.v + (h.v - cur.v) * wv);
  o.b = {cur.b.r + (h.b.r - cur.b.r) * wb, cur.b.g + (h.b.g - cur.b.g) * wb, cur.b.b + (h.b.b - cur.b.b) * wb};
  return o;
}


// ---------------------------------------------------------------------------
// the whole chain over a small patch of a still picture, for the rules one against another
// ---------------------------------------------------------------------------

// What a set of rules is: the blur's radius and the wide blur's, the frames the history averages, the clip box, the a-trous
// passes, and the noise's shift. FirstRules() is what the renderer did before ao_stable.h and ssgi_stable.h, StableRules(h) what it
// does for an image `h` lines high.
struct Rules {
  int radius = 1;
  int wide = 2;
  int vis_cap = 12;
  int bounce_cap = 16;
  float clip_width = 1.25f;
  float clip_floor = 0.05f;
  float clip_history = 0.0f;
  int atrous = 2;
  int noise_lattice = 0;
};

inline Rules FirstRules() { return Rules{}; }
inline Rules StableRules(uint32_t ao_height) {
  Rules r;
  r.radius = ao_stable::DenoiseRadius(ao_height);
  r.wide = ao_stable::WideRadius(r.radius);
  r.clip_width = ssgi_stable::kClipWidth;
  r.clip_history = ssgi_stable::kClipHistoryShare;
  r.atrous = 2;
  r.noise_lattice = 13;
  return r;
}

// The rules' reach in pixels from a patch: what the pipeline reads outside it.
inline int RulesBorder(const Rules& r, int atrous_passes) {
  int reach = 0;
  for (int i = 0; i < atrous_passes; ++i) {
    reach += 2 << i;
  }
  return std::max(r.radius, r.wide) + 1 + reach + 1;
}

// The apply pass's inputs for a pixel: its colour as the chain carries it, the sun's term at it (sun x N.L x the shadow mask).
struct ApplyPixel {
  Rgb colour;
  float sun_term = 0.0f;
};

struct ApplyLook {
  float ambient = 0.35f;
  float sun = 1.0f;
  float strength = 2.0f;  // the bounce level DEFAULT (ssgi_level.h): strength 2, limit 4.5, cap 2
  float limit = 4.5f;
  float cap = 2.0f;
  float ao_strength = 1.0f;
  float ao_direct = 0.375f;
};

// The patch's series: for each pixel of the P x P patch and each frame, the numbers the tests look at.
struct PatchSeries {
  int p = 0;
  int frames = 0;
  std::vector<std::vector<double>> final_luma;  // luma of the picture's pixel after the occlusion and the bounce
  std::vector<std::vector<double>> bounce_add;  // luma the bounce adds to it
  std::vector<std::vector<double>> ao_factor;   // the occlusion's factor
  std::vector<std::vector<double>> denoised;    // the visibility after the blur, before the history
  std::vector<std::vector<double>> accumulated; // the visibility after the history
  std::vector<std::vector<double>> bounce_b;    // the bounce's luma after the history and the a-trous filter
};

// Per frame the raw (visibility, bounce) of the region, from GiMain.
using RawFrames = std::vector<std::vector<Px4>>;

inline RawFrames ComputeRaw(const Chains& c, const GiParams& gp, const Region& reg, int frames, int lattice) {
  GiParams g = gp;
  g.noise_lattice = lattice;
  RawFrames raw(size_t(frames), std::vector<Px4>(reg.Count()));
  for (int n = 0; n < frames; ++n) {
    for (int y = 0; y < reg.h; ++y) {
      for (int x = 0; x < reg.w; ++x) {
        const GiOut o = GiMain(c, g, reg.x0 + x, reg.y0 + y, n);
        raw[size_t(n)][size_t(y) * size_t(reg.w) + size_t(x)] = {o.visibility, o.bounce};
      }
    }
  }
  return raw;
}

// Runs the blur, the accumulation and the a-trous passes of `rules` over the frames of `raw` and records the patch (its top left
// at (cx - p / 2, cy - p / 2)). `atrous_passes` is the number of a-trous iterations (the rules' base plus what the image's
// height adds, ssgi_stable.h). `pixel_of` gives the apply's inputs; `history_frames`: cap both histories to this many frames
// (0: the rules' own; 1: no history at all, every frame stands alone).
inline PatchSeries RunPatch(const Geometry& geo, const RawFrames& raw, const Rules& rules, int atrous_passes, int cx, int cy, int p,
                            const std::function<ApplyPixel(int, int)>& pixel_of, const ApplyLook& look, int history_frames = 0) {
  const Region& reg = geo.region;
  PatchSeries out;
  out.p = p;
  out.frames = int(raw.size());
  const size_t count = size_t(p) * size_t(p);
  out.final_luma.assign(count, {});
  out.bounce_add.assign(count, {});
  out.ao_factor.assign(count, {});
  out.denoised.assign(count, {});
  out.accumulated.assign(count, {});
  out.bounce_b.assign(count, {});
  AccumRules ar;
  ar.vis_cap = history_frames > 0 ? history_frames : rules.vis_cap;
  ar.bounce_cap = history_frames > 0 ? history_frames : rules.bounce_cap;
  ar.clip_width = rules.clip_width;
  ar.clip_floor_share = rules.clip_floor;
  ar.clip_margin = rules.clip_history;
  std::vector<Px4> history;
  bool have = false;
  int age = 0;
  for (size_t n = 0; n < raw.size(); ++n) {
    std::vector<Px4> den(reg.Count());
    for (int y = 0; y < reg.h; ++y) {
      for (int x = 0; x < reg.w; ++x) {
        den[size_t(y) * size_t(reg.w) + size_t(x)] = Bilateral4(geo, raw[n], reg.x0 + x, reg.y0 + y, rules.radius, BlurSigma(rules.radius));
      }
    }
    std::vector<Px4> acc(reg.Count());
    for (int y = 0; y < reg.h; ++y) {
      for (int x = 0; x < reg.w; ++x) {
        acc[size_t(y) * size_t(reg.w) + size_t(x)] = Accumulate(geo, den, have ? &history : nullptr, age, reg.x0 + x, reg.y0 + y, ar, rules.wide);
      }
    }
    history = acc;
    have = true;
    age = std::min(age + 1, std::max(ar.vis_cap, ar.bounce_cap));
    std::vector<Px4> img = acc;
    for (int it = 0; it < atrous_passes; ++it) {
      std::vector<Px4> next(reg.Count());
      for (int y = 0; y < reg.h; ++y) {
        for (int x = 0; x < reg.w; ++x) {
          next[size_t(y) * size_t(reg.w) + size_t(x)] = Atrous(geo, img, reg.x0 + x, reg.y0 + y, 1 << it);
        }
      }
      img = next;
    }
    for (int j = 0; j < p; ++j) {
      for (int i = 0; i < p; ++i) {
        const int px = cx - p / 2 + i, py = cy - p / 2 + j;
        const size_t k = reg.Index(px, py);
        const size_t q = size_t(j) * size_t(p) + size_t(i);
        const ApplyPixel ap = pixel_of(px, py);
        ssgi_light::ApplyIn ai;
        ai.bounce_radiance = img[k].b;
        ai.colour = ap.colour;
        ai.ambient = look.ambient;
        ai.sun = look.sun;
        ai.sun_term = ap.sun_term;
        ai.strength = look.strength;
        ai.limit = look.limit;
        ai.cap = look.cap;
        const float share0 = look.ambient / std::max(look.ambient + ap.sun_term, 1e-6f);
        const float share = Saturate(share0 + (1.0f - share0) * look.ao_direct);
        const float aof = 1.0f + (img[k].v - 1.0f) * Saturate(look.ao_strength * share);
        ai.ao_part = aof;
        const Rgb bf = ssgi_light::BounceFactor(ai);
        const Rgb fin = {ap.colour.r * (aof + bf.r), ap.colour.g * (aof + bf.g), ap.colour.b * (aof + bf.b)};
        const Rgb add = {ap.colour.r * bf.r, ap.colour.g * bf.g, ap.colour.b * bf.b};
        out.final_luma[q].push_back(ssgi_light::Luma(fin));
        out.bounce_add[q].push_back(ssgi_light::Luma(add));
        out.ao_factor[q].push_back(aof);
        out.denoised[q].push_back(den[k].v);
        out.accumulated[q].push_back(acc[k].v);
        out.bounce_b[q].push_back(ssgi_light::Luma(img[k].b));
      }
    }
  }
  return out;
}

// Statistics of a series over the frames [from, to): the mean over the patch of each pixel's standard deviation in time, over its mean (a
// coefficient of variation), and of the mean frame-to-frame change over the mean.
struct SeriesStat {
  double cv = 0.0;
  double change = 0.0;
  double mean = 0.0;
};

inline SeriesStat Stat(const std::vector<std::vector<double>>& series, int from, int to) {
  SeriesStat s;
  double sd = 0.0, chg = 0.0, mean = 0.0;
  for (const auto& v : series) {
    double m = 0.0;
    for (int i = from; i < to; ++i) {
      m += v[size_t(i)];
    }
    m /= double(to - from);
    double q = 0.0, c = 0.0;
    for (int i = from; i < to; ++i) {
      q += (v[size_t(i)] - m) * (v[size_t(i)] - m);
      if (i > from) {
        c += std::fabs(v[size_t(i)] - v[size_t(i - 1)]);
      }
    }
    sd += std::sqrt(q / double(to - from));
    chg += c / double(std::max(to - from - 1, 1));
    mean += m;
  }
  mean /= double(series.size());
  s.mean = mean;
  s.cv = sd / double(series.size()) / std::max(mean, 1e-9);
  s.change = chg / double(series.size()) / std::max(mean, 1e-9);
  return s;
}

}  // namespace mcla::gpu::ao_stable_twin
