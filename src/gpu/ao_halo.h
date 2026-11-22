#pragma once
// ===========================================================================
// MCLA GPU renderer -- ambient occlusion: the road beside a car, and a road that nothing covers
// (pure maths, no GPU)
// ===========================================================================
// hlsl/ao_main_ps.hlsl works out the visibility, hlsl/ao_depth_ps.hlsl marks the vehicles' pixels,
// ao.cpp passes the setting. The numbers and rules that decide how far a car's darkening reaches on the
// road live HERE, with a program of their own that checks them (ao_halo_test.cpp: it needs no renderer,
// no ReXGlue and no GPU), and this file holds a C++ twin of the shader's visibility, `Visibility`, which
// the check runs over synthetic scenes. The shader repeats the rules and the constants (marked
// "ao_halo.h" in its comments: change them here first) and the check compares them.
//
// What the owner and Mz saw (2026-11-13, chase camera, dusk): with the occlusion on, the road beside and
// behind a car is darker than the car's footprint, a halo. Two things in the first version, both
// measured with the twin on a car-sized box on a road seen from 7 m behind and 2 m up, 3 slices and
// 3 steps, the look "default" (radius 3 m, power 3.6), 64 frames averaged (the occlusion's own
// accumulation):
//   1. A car is a wall at the road's edge to the depth, and the whole radius (3 m, full weight out to
//      1.15 m, then a ramp) counts for it: the road is 0.78 (22% dark) at 0.6 m from the car's rear,
//      0.87 at 0.8 m, 0.89 at 1 m, 0.93 at 1.25 m and still 0.95 at 1.5 m; the power of 3.6 turns the
//      few per cent of occlusion that a 1 m high wall makes at 2 m into a visible tail. A wall of a
//      building is the same maths and has to stay: the look wants it. A car is not a wall: it is small,
//      it moves, and the game shadows it itself, so what the eye wants of it is the dark line where it
//      meets the road. The depth pass now marks a vehicle's pixels (the stencil IDs 1 to 23, in the
//      lowest bit of the depth), and a sample that is a vehicle, seen from a pixel that is not, weighs
//      by the falloff over kDefaultCarReach (0.4) of the radius: 1.2 m, the ramp the same share of it.
//      Result: 0.30 at 5 cm, 0.83 at 0.4 m, 0.95 at 0.6 m, 0.98 at 0.8 m and nothing past 1 m. The car's own body
//      (a pixel that is a vehicle) keeps the whole radius: the creases under a spoiler and round the
//      rear window are as dark as before.
//   2. The sum over the slices was divided by the number of slices and clamped to 1. The sum is 1 for
//      a surface nothing blocks only on average over the slice angles; with two or three slices at a
//      grazing angle (a road seen from a car's height) a frame's value is above 1 half of the time and
//      the clamp cut it: the mean of an OPEN road came out 0.96 at 5 m, 0.95 at 10 m, 0.93 at 15 m and
//      0.91 at 40 m (the power made it worse, and the history averaged it). A road with nothing on it
//      was 4 to 9% dark, more with the distance. The sum is now divided by what it is for a surface
//      nothing blocks (the slices' own open arcs: a ratio), which is 1 exactly whatever the slice
//      angles: the open road comes out 0.99 at 5 to 10 m and 0.96 at 40 m, and a wall's foot is as dark
//      as before (0.27 at 10 cm from a tall wall).
// The pause menu's cars, the traffic and the AI's are vehicles like the player's.
// ===========================================================================

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>

#include "blue_noise.h"

namespace mcla::gpu::ao_halo {

// ---------------------------------------------------------------------------
// the rules
// ---------------------------------------------------------------------------

// How far a vehicle's samples reach on a pixel that is not a vehicle: this share of the radius
// (mcla_gpu_ao_car_reach). At or below kReachOff and at or above kReachFull there is no limit.
inline constexpr float kDefaultCarReach = 0.4f;
inline constexpr float kReachOff = 0.001f;
inline constexpr float kReachFull = 0.999f;
// The smallest radius a vehicle's samples may get (a share near 0 would divide by it).
inline constexpr float kMinCarRadius = 0.05f;
// A vehicle's stencil ID is the stencil byte >> 2: 1 to 23 (the UI's draws use 24 and up).
inline constexpr uint32_t kFirstVehicleId = 1;
inline constexpr uint32_t kLastVehicleId = 23;
// What the sum over the slices is divided by at least (a pixel whose slices have no length at all).
inline constexpr float kMinOpenSum = 1e-3f;

inline float Saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

inline bool CarReachLimits(float share) { return share > kReachOff && share < kReachFull; }

// The weight of a sample `dist` away on the horizon (1 full, 0 none): full out to radius * (1 - ratio),
// then a ramp to 0 at the radius (the shaders' saturate(dist * mul + add), mul = -1 / (ratio * radius),
// add = 1 / ratio).
inline float Falloff(float dist, float radius, float ratio) {
  const float r = std::max(ratio, 0.01f);
  return Saturate(dist * (-1.0f / (r * radius)) + 1.0f / r);
}

// The same for a vehicle's sample: the ramp over `share` of the radius.
inline float VehicleFalloff(float dist, float radius, float ratio, float share) {
  return Falloff(dist, std::max(share * radius, kMinCarRadius), ratio);
}

// The weight a sample gets. The receiver being a vehicle (its own body) or the sample not being one: the
// world's falloff; a vehicle's sample on anything else: the smaller of the two.
inline float SampleWeight(float dist, float radius, float ratio, float share, bool sample_is_vehicle,
                          bool receiver_is_vehicle) {
  float w = Falloff(dist, radius, ratio);
  if (CarReachLimits(share) && !receiver_is_vehicle && sample_is_vehicle) {
    w = std::min(w, VehicleFalloff(dist, radius, ratio, share));
  }
  return w;
}

// The vehicle bit: the lowest bit of the level-0 depth (a float). One unit in the last place.
inline float SetVehicleBit(float depth, bool vehicle) {
  const uint32_t bits = std::bit_cast<uint32_t>(depth);
  return std::bit_cast<float>((bits & ~1u) | (vehicle ? 1u : 0u));
}
inline bool IsVehicle(float level0_depth) { return (std::bit_cast<uint32_t>(level0_depth) & 1u) != 0u; }

inline bool IsVehicleStencil(uint32_t stencil_byte) {
  const uint32_t id = stencil_byte >> 2;
  return id >= kFirstVehicleId && id <= kLastVehicleId;
}

// One slice's share of the sum of an open surface: the length of the normal's projection into the slice
// plane times the arc of the whole hemisphere, cos(n) + n sin(n) (n the normal's angle from the view
// vector in the slice).
inline float OpenSlice(float projected_len, float cos_n, float n) {
  return projected_len * (cos_n + n * std::sin(n));
}

// The visibility from the slices' sums: divided by what the sum is for a surface nothing blocks (the
// ratio), or, as the first version did, by the number of slices.
inline float SliceVisibility(float sum, float open_sum, int slices, bool ratio) {
  return Saturate(ratio ? sum / std::max(open_sum, kMinOpenSum) : sum / float(std::max(slices, 1)));
}

// ---------------------------------------------------------------------------
// the C++ twin of the main pass
// ---------------------------------------------------------------------------

// What the main pass looks at: the linear depth of level 0 and whether the pixel is a vehicle, at any
// pixel (the coarser levels are made from level 0 here the way ao_mip_ps makes them). Depth 60000 is the
// sky.
struct Scene {
  int width = 1280;
  int height = 720;
  float tan_x = 0.0f;  // tan of the half field of view, horizontal and vertical
  float tan_y = 0.0f;
  std::function<float(int, int)> depth;
  std::function<bool(int, int)> vehicle;
};

struct Params {
  float radius = 3.0f;
  float ratio = 0.615f;   // the falloff range as a share of the radius (ao.cpp kFalloffRatio)
  float power = 3.6f;
  float floor = 0.1f;
  float car_reach = 0.0f;  // 0: no limit
  int slices = 3;
  int steps = 3;
  bool ratio_sum = true;   // the slice sum as a ratio of what it is for an open surface
  // The shader's cheaper forms of the same arithmetic (ao_fast.h, "THE SAME WORK"): 1 / size and 1 / distance as factors, the
  // tangents as -sin n and sin n, the sample's level from comparing its length with 16 and 32 pixels. False: the forms of before.
  bool lossless = false;
  // The pixel's noise (hlsl/ao_common.hlsli AoSpatialNoise, AoNoiseShift). Without these two, the first version's: interleaved gradient noise and a
  // shift of the R2 sequence a frame. `noise_lattice` 13: the shift is the lattice of 13 frames (ao_stable.h, mcla_gpu_ao_stable). `blue`: the pixel's
  // own part is the blue noise tile's two channels (blue_noise.h, mcla_gpu_ao_blue_noise) in place of interleaved gradient noise.
  int noise_lattice = 0;
  const blue_noise::Tile* blue = nullptr;
};

inline constexpr float kSky = 60000.0f;
inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kHalfPi = 1.57079632679490f;
inline constexpr float kMinOffsetPixels = 1.3f;
inline constexpr float kMaxRadiusShare = 0.3f;

struct V3 {
  float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a) {
  const float l = Length(a);
  return l > 1e-20f ? a * (1.0f / l) : V3{0.0f, 0.0f, -1.0f};
}
inline float Frac(float x) { return x - std::floor(x); }

// The view-space position of the pixel centre (cx, cy) at distance t (ao_common.hlsli AoViewPos).
inline V3 ViewPos(const Scene& s, float cx, float cy, float t) {
  const float u = cx / float(s.width);
  const float v = cy / float(s.height);
  return {(u * 2.0f - 1.0f) * s.tan_x * t, (1.0f - v * 2.0f) * s.tan_y * t, t};
}

// The depth chain: level 0 is the scene; levels 1 and 2 are the weighted means ao_mip_ps makes (the
// weights favour the farthest of the four).
inline float ChainDepth(const Scene& s, const Params& p, int level, int x, int y) {
  if (level == 0) {
    return s.depth(std::clamp(x, 0, s.width - 1), std::clamp(y, 0, s.height - 1));
  }
  const int pw = level == 1 ? s.width : (s.width + 1) / 2;
  const int ph = level == 1 ? s.height : (s.height + 1) / 2;
  float d[4];
  int k = 0;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      d[k++] = ChainDepth(s, p, level - 1, std::min(x * 2 + i, pw - 1), std::min(y * 2 + j, ph - 1));
    }
  }
  const float far_d = std::max(std::max(d[0], d[1]), std::max(d[2], d[3]));
  const float range = std::max(0.75f * p.radius * p.ratio, 0.01f);
  float sum = 0.0f;
  float weights = 0.0f;
  for (int q = 0; q < 4; ++q) {
    const float w = Saturate(1.0f - (far_d - d[q]) / range);
    sum += w * d[q];
    weights += w;
  }
  return std::min(sum / std::max(weights, 1e-6f), kSky);
}

inline float Noise(float px, float py) {
  return Frac(52.9829189f * Frac(px * 0.06711056f + py * 0.00583715f));
}

// The visibility at pixel (px, py) in frame `frame`, as hlsl/ao_main_ps.hlsl (the plain pass) makes it.
inline float Visibility(const Scene& s, const Params& p, int px, int py, int frame) {
  const float tc = s.depth(px, py);
  if (tc >= kSky * 0.5f) {
    return 1.0f;
  }
  const bool receiver_vehicle = s.vehicle(px, py);
  const V3 pc = ViewPos(s, float(px) + 0.5f, float(py) + 0.5f, tc);
  // The normal from the neighbours whose depth is nearer to the pixel's (AoNormalFromImage).
  const int xl = std::max(px - 1, 0), xr = std::min(px + 1, s.width - 1);
  const int yt = std::max(py - 1, 0), yb = std::min(py + 1, s.height - 1);
  const float tl = s.depth(xl, py), tr = s.depth(xr, py), tt = s.depth(px, yt), tb = s.depth(px, yb);
  const V3 h = std::fabs(tl - tc) < std::fabs(tr - tc) ? pc - ViewPos(s, float(xl) + 0.5f, float(py) + 0.5f, tl)
                                                       : ViewPos(s, float(xr) + 0.5f, float(py) + 0.5f, tr) - pc;
  const V3 v = std::fabs(tt - tc) < std::fabs(tb - tc) ? ViewPos(s, float(px) + 0.5f, float(yt) + 0.5f, tt) - pc
                                                       : pc - ViewPos(s, float(px) + 0.5f, float(yb) + 0.5f, tb);
  const V3 normal = Normalize(Cross(v, h));
  const V3 view_vec = Normalize(pc * -1.0f);

  const float pixels_per_unit = 0.5f * float(s.height) / (s.tan_y * tc);
  const float radius_px = std::min(p.radius * pixels_per_unit, kMaxRadiusShare * float(s.height));
  if (radius_px < kMinOffsetPixels + 0.5f) {
    return 1.0f;
  }
  const float ratio = std::max(p.ratio, 0.01f);
  const float spatial_x = p.blue ? blue_noise::Sample(*p.blue, 0, px, py) : Noise(float(px), float(py));
  const float spatial_y = p.blue ? blue_noise::Sample(*p.blue, 1, px, py) : Noise(float(py) + 31.0f, float(px) + 11.0f);
  float shift_x, shift_y;
  if (p.noise_lattice == 13) {
    const int m = ((frame % 13) + 13) % 13;
    shift_x = float(m) / 13.0f;
    shift_y = float((m * 8) % 13) / 13.0f;
  } else {
    const float fr = float(frame & 1023);
    shift_x = fr * 0.75487766624669f;
    shift_y = fr * 0.56984029099806f;
  }
  const float noise_x = Frac(spatial_x + shift_x);
  const float noise_y = Frac(spatial_y + shift_y);
  const float min_offset = kMinOffsetPixels / radius_px;
  const int slices = std::max(p.slices, 1);
  const int steps = std::max(p.steps, 1);

  float visibility = 0.0f;
  float open_sum = 0.0f;
  for (int slice = 0; slice < slices; ++slice) {
    const float phi = (float(slice) + noise_x) / float(slices) * kPi;
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
    const float low0 = p.lossless ? -std::sin(n) : std::cos(n + kHalfPi);
    const float low1 = p.lossless ? std::sin(n) : std::cos(n - kHalfPi);
    float hcos0 = low0, hcos1 = low1;
    for (int st = 0; st < steps; ++st) {
      const float step_noise = Frac(noise_y + float(slice + st * slices) * 0.6180339887f);
      float share = (float(st) + step_noise) / float(steps);
      share = share * share + min_offset;
      const float ox = dpx * share * radius_px, oy = dpy * share * radius_px;
      int level;
      if (p.lossless) {
        const float length_px = share * radius_px;  // dir is a unit vector
        level = (length_px >= 16.0f ? 1 : 0) + (length_px >= 32.0f ? 1 : 0);
      } else {
        const float level_f = std::clamp(std::log2(std::max(std::hypot(ox, oy), 1.0f)) - 3.0f, 0.0f, 2.0f);
        level = int(level_f);
      }
      for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? float(px) + 0.5f + ox : float(px) + 0.5f - ox;
        const float sy = side == 0 ? float(py) + 0.5f + oy : float(py) + 0.5f - oy;
        if (!(sx >= 0.0f && sy >= 0.0f && sx < float(s.width) && sy < float(s.height))) {
          continue;
        }
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        const float td = ChainDepth(s, p, level, ix >> level, iy >> level);
        V3 sp = ViewPos(s, float(ix) + 0.5f, float(iy) + 0.5f, td);
        if (p.lossless) {
          // AoViewPosInv: the centre times 2 / size (the clip space's scale, made once for the pass).
          const float scale_x = 2.0f / float(s.width), scale_y = 2.0f / float(s.height);
          sp = {((float(ix) + 0.5f) * scale_x - 1.0f) * s.tan_x * td, (1.0f - (float(iy) + 0.5f) * scale_y) * s.tan_y * td, td};
        }
        const V3 delta = sp - pc;
        const float dist = Length(delta);
        const float cos_sample = p.lossless ? Dot(delta, view_vec) * (1.0f / std::max(dist, 1e-6f))
                                            : Dot(delta * (1.0f / std::max(dist, 1e-6f)), view_vec);
        const float low = side == 0 ? low0 : low1;
        float& hc = side == 0 ? hcos0 : hcos1;
        float w = Falloff(dist, p.radius, ratio);
        float pull = low + (cos_sample - low) * w;
        if (CarReachLimits(p.car_reach) && !receiver_vehicle && pull > hc && s.vehicle(ix, iy)) {
          w = std::min(w, VehicleFalloff(dist, p.radius, ratio, p.car_reach));
          pull = low + (cos_sample - low) * w;
        }
        hc = std::max(hc, pull);
      }
    }
    float h0 = -std::acos(std::clamp(hcos1, -1.0f, 1.0f));
    float h1 = std::acos(std::clamp(hcos0, -1.0f, 1.0f));
    h0 = n + std::clamp(h0 - n, -kHalfPi, kHalfPi);
    h1 = n + std::clamp(h1 - n, -kHalfPi, kHalfPi);
    const float sin_n = std::sin(n);
    const float arc0 = (cos_n + 2.0f * h0 * sin_n - std::cos(2.0f * h0 - n)) * 0.25f;
    const float arc1 = (cos_n + 2.0f * h1 * sin_n - std::cos(2.0f * h1 - n)) * 0.25f;
    visibility += projected_len * (arc0 + arc1);
    open_sum += OpenSlice(projected_len, cos_n, n);
  }
  visibility = SliceVisibility(visibility, open_sum, slices, p.ratio_sum);
  return std::max(std::pow(visibility, p.power), p.floor);
}

}  // namespace mcla::gpu::ao_halo
