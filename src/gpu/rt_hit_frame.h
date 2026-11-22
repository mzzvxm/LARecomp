#pragma once
// ===========================================================================
// MCLA GPU renderer -- what every ray traced hit shading shares: the frame's rows, the sun's cascades, the environment map, the packing (CITY-RT M2 package H4)
// ===========================================================================
// WHERE THIS CAME FROM. The car hit shading (car_rt_material_rules.h, RT-HIT-SHADING) held, beside what is about a car, the pieces every hit shading needs: the frame's 32 rows of
// lighting and the game's sun cascades (what the camera's draws saw: the sun, the ambient, the environment map's slots, the sun's shadow map of the world), the environment map's
// paraboloid, the lookup into the cascades, the packing of colours and half floats, and the numbers of the shading's shared terms (the sky's fill, the emissive gain, the cone's
// mip scale). The city hit library (hlsl/city_hit_shade.hlsli, city_hit_shade_rules.h) shades a hit on the city from the SAME frame, so those pieces moved here, BY NAME and unchanged
// (the plan of src/citypack/city_hit_format.h "THE rt_hit_common SPLIT"); car_rt_material_rules.h names them again in its own namespace (using declarations), so every car file, and every
// car test, reads exactly what it read before. The HLSL twin is hlsl/rt_hit_common.hlsli (CarMat* became RtHit*, bodies unchanged: tools/check_rt_hit_identity.py compiles the car
// shaders from the commit before the move and from this one and compares the DXIL and the SPIR-V without names); rt_hit_frame_test.cpp checks the pieces and that hlsl/rt_hit_common.hlsli
// holds their numbers.
//
// The frame's rows (binding 5 of the car scene's set: kFrameRows rows of four words before the instances' rows; any other pass of a pure city consumer has them in a buffer of its own,
// hlsl/rt_hit_common.hlsli RT_HIT_FRAME_ROW):
//   0 xyz the direction to the sun in VIEW space (unit), w flags (kFrameShade, kFrameShadow, kFrameEnv, kFrameSun, kFrameCascade)
//   1 xyz the sun's colour (PS c39 of the car's own draws, in the paint's units), w the sun shadow ray's reach in metres
//   2 xyz the ambient (PS c26), w its gain
//   3 x the environment map's slot (bit 31 the gamma bit), y the blurred map's slot, z the night factor (from the sun's height where the frame knows the sun, from the sun's colour where not:
//     RT-HIT-SHADING-5), w the specular gain
//   4 xyz the sky ambient (PS c57 ambYPos), w the overall gain
//   5, 6, 7 the WORLD x, y and z axes in view space (the environment map is in world axes; the passes work in view space), row 5's w the hit's detail (the car's: RT-HIT-SHADING-5; the city's
//     shading ignores it)
//   8..31 the cascades (below)
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcla::gpu::rt_hit {

// ---------------------------------------------------------------------------
// the numbers every hit shading shares
// ---------------------------------------------------------------------------

// What the shading adds to the cone's footprint when it picks the environment map's level: log2(1 + kConeLodScale x tan(half angle)) levels.
inline constexpr float kConeLodScale = 20.0f;
// The reach of the sun's visibility ray, metres: a car's own shadow on itself and on its neighbour needs a few metres.
inline constexpr float kSunReach = 6.0f;
// The brightness of the sun's colour (its mean) between which the night factor falls from 1 to 0 (the lamps and the lit interior glow at night).
inline constexpr float kNightLow = 0.02f;
inline constexpr float kNightHigh = 0.25f;
// The sky's fill (RT-HIT-SHADING-3, car_rt_hit_rules.h): the ambient light of a hit is at least this share of the sun's colour (in the sun's hue), times the hemisphere term. The cars' draws'
// c26 was (0.000 0.004 0.004) in a daylight run: no red, so a flank in shadow was black under an orange paint. A clear sky lights a shaded flank at about a fifth of what the sun does.
inline constexpr float kFillShare = 0.2f;
// The glow of a lamp (and of a lit interior) at night, against the paint's units.
inline constexpr float kEmissiveGain = 3.0f;
// The largest value a packed colour holds (R10G10B10 over 0..kTintMax).
inline constexpr float kTintMax = 2.0f;
// A diffuse slot of "none" in a record.
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
inline constexpr uint32_t kGammaBit = 0x80000000u;  // descriptors.h kTextureGammaBit

// ---------------------------------------------------------------------------
// packing: half floats, colours, float bits
// ---------------------------------------------------------------------------

// The nearest half float (ties to even), saturating at the largest finite half; a NaN is 0. The texture coordinates are stored as halves, which the shader reads with f16tof32.
inline uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent == 0xFFu) {
    return mantissa != 0 ? uint16_t(0) : uint16_t(sign | 0x7BFFu);
  }
  const int32_t e = int32_t(exponent) - 127 + 15;
  if (e >= 31) {
    return uint16_t(sign | 0x7BFFu);
  }
  if (e <= 0) {
    if (e < -10) {
      return uint16_t(sign);
    }
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - e);
    uint32_t half = mantissa >> shift;
    const uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) {
      ++half;
    }
    return uint16_t(sign | half);
  }
  uint32_t half = (uint32_t(e) << 10) | (mantissa >> 13);
  const uint32_t remainder = mantissa & 0x1FFFu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) {
    ++half;
  }
  if (half >= 0x7C00u) {
    half = 0x7BFFu;
  }
  return uint16_t(sign | half);
}


inline uint32_t PackRgb10(const float rgb[3]) {
  uint32_t out = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    const float v = std::clamp(std::isfinite(rgb[c]) ? rgb[c] : 0.0f, 0.0f, kTintMax) / kTintMax;
    out |= uint32_t(std::lround(v * 1023.0f)) << (10u * c);
  }
  return out;
}
inline void UnpackRgb10(uint32_t packed, float rgb[3]) {
  for (uint32_t c = 0; c < 3; ++c) {
    rgb[c] = float((packed >> (10u * c)) & 1023u) / 1023.0f * kTintMax;
  }
}

inline uint32_t Bits(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  return b;
}
inline float FromBits(uint32_t b) {
  float v;
  std::memcpy(&v, &b, 4);
  return v;
}

// ---------------------------------------------------------------------------
// the frame's rows
// ---------------------------------------------------------------------------

// The frame's rows: 0 to 7 the lighting (below), then the sun's cascades (SunCascades: the game's own shadow map of the world, so that the reflected car sits in the scene's light):
//   8   x the atlas's bindless slot (kNoSlot: none), y the cascade count, z the depth bias (float bits), w the sun's fade (float bits: the visibility is lerp(lit, 1, fade))
//   9   xyz the CAMERA's position in the game's world (the cascades' matrices take world points), w 0
//   10  xyz the cascade boxes' right axis (the collector's c85), 11 xyz the top axis (c82)
//   12 + 5 i .. 12 + 5 i + 3   cascade i's four matrix rows (c64 + 4 i .. c67 + 4 i), 12 + 5 i + 4 its box (c86.i, c87.i, c83.i, c84.i)
inline constexpr uint32_t kMaxCascades = 4;
inline constexpr uint32_t kCascadeHeaderRow = 8;
inline constexpr uint32_t kCameraRow = 9;
inline constexpr uint32_t kCascadeRightRow = 10;
inline constexpr uint32_t kCascadeTopRow = 11;
inline constexpr uint32_t kCascadeFirstRow = 12;
inline constexpr uint32_t kCascadeRowsEach = 5;
inline constexpr uint32_t kFrameRows = kCascadeFirstRow + kMaxCascades * kCascadeRowsEach;  // 32

inline constexpr uint32_t kFrameShade = 1;    // hit shading is on
inline constexpr uint32_t kFrameShadow = 2;   // the sun's visibility ray is cast
inline constexpr uint32_t kFrameEnv = 4;      // the environment map's slot is valid
inline constexpr uint32_t kFrameSun = 8;      // the direction to the sun is known
inline constexpr uint32_t kFrameCascade = 16; // the game's sun cascades and the camera's world position are in the rows: the sun's visibility at a hit is read from them

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Tuning {
  bool enabled = true;
  bool shadow = true;
  bool cascade = true;   // the sun's visibility also reads the game's cascades (where the frame has them)
  float gain = 1.0f;     // everything the shading adds, against the history's units
  float ambient = 1.0f;  // the ambient's share
  float spec = 1.0f;     // the environment reflection's share
  float detail = 1.0f;   // RT-HIT-SHADING-5: the hit's own detail (the coat's glints, the sky's openness, the contact darkening, the class's ambient); 0 is the shading as it was, above 1 stronger glints
};

// The lighting of the frame as the car's own draws saw it (PS c26 ambient, c39 sun colour, c57 sky ambient; the environment map's slots in the fetch constants 0 and 1).
struct FrameLight {
  bool valid = false;
  float sun[3] = {};
  float ambient[3] = {};
  float sky[3] = {};
  uint32_t env_sharp = 0;  // the bindless slot (bit 31 the gamma bit) of the 512x256 map as a paint, chrome, glass or light draw of the frame bound it; 0: none seen
  uint32_t env_blur = 0;
};

// The game's sun shadow map of the world as the collector's draws told it (volumetrics.h SunCascadesForOthers): four nested cascades, each a light matrix and a box in the
// collector's (right, top) axes, and the atlas the four share. The lookup is the collector's own (CascadeLit; hlsl/car_rt_hit_shade.hlsli CarMatCascadeLit is its twin).
struct SunCascades {
  bool valid = false;
  uint32_t atlas_slot = kNoSlot;
  uint32_t count = 0;
  float fade = 0.0f;        // the collector's own fade: the visibility is lerp(lit, 1, fade)
  float depth_bias = 0.0f;  // added to the receiver's depth before the compare
  float right[3] = {};
  float top[3] = {};
  float rows[kMaxCascades][4][4] = {};  // cascade i's matrix rows, as the collector's constants hold them
  float box[kMaxCascades][4] = {};
};

struct FrameInput {
  FrameLight light;
  SunCascades cascades;
  bool camera_known = false;
  float camera_world[3] = {};  // the camera's place in the game's world: a hit's world point is this plus the view vector turned by the world's axes
  bool sun_known = false;
  float to_sun_view[3] = {0.0f, 1.0f, 0.0f};     // unit, in the frame's view space
  float world_axes_view[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // the world's x, y and z axes in the view space
  Tuning tuning;
  float shadow_reach = kSunReach;
};

// 0 in the day, 1 at night, from the sun's colour (its mean). The colour of the cars' draws (c39) does not follow the hour (0.97 at 14 h and at 23 h, rthit1), so a frame whose sun
// direction is known takes NightFromSunHeight instead; this is the fallback for a frame that has none.
inline float NightFactor(const float sun[3]) {
  const float mean = (sun[0] + sun[1] + sun[2]) / 3.0f;
  const float t = std::clamp((mean - kNightLow) / (kNightHigh - kNightLow), 0.0f, 1.0f);
  const float s = t * t * (3.0f - 2.0f * t);
  return 1.0f - s;
}

// The night factor from the sun's height (RT-HIT-SHADING-5): `sin_elevation` is the sine of the sun's angle over the horizon (the direction to the sun dotted with the world's up). 1 at and under
// kNightSunLow degrees, 0 from kNightSunHigh degrees up, a smoothstep between: the volumetric light's own rule (volumetrics_rules.h NightFactor(elevation): -4 to 6 degrees, which the test
// holds this to), so that the lamps glow and the sun's light is gone at the hours the volume says it is night. The sun's light stops at night and the street lamps' stand in (the glints).
inline constexpr float kNightSunLow = -4.0f;
inline constexpr float kNightSunHigh = 6.0f;
inline float NightFromSunHeight(float sin_elevation) {
  const float degrees = std::asin(std::clamp(sin_elevation, -1.0f, 1.0f)) * (180.0f / 3.14159265358979323846f);
  const float t = std::clamp((degrees - kNightSunLow) / (kNightSunHigh - kNightSunLow), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The cascades are worth reading: a slot, at least one cascade, finite numbers, and a box with an extent. The same judgement as Volumetrics::FillSun's (an empty or stale collector is
// no sun shadow: the visibility is then the cars' ray alone).
inline bool CascadesUsable(const SunCascades& s) {
  if (!s.valid || s.atlas_slot == kNoSlot || s.count == 0 || s.count > kMaxCascades || !std::isfinite(s.fade) || !std::isfinite(s.depth_bias)) {
    return false;
  }
  bool extent = false;
  for (uint32_t i = 0; i < s.count; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(s.rows[i][j][c])) {
          return false;
        }
      }
      if (!std::isfinite(s.box[i][j])) {
        return false;
      }
    }
    extent = extent || (s.box[i][1] > 0.0f && s.box[i][3] > 0.0f);
  }
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(s.right[c]) || !std::isfinite(s.top[c])) {
      return false;
    }
  }
  return extent;
}

inline void MakeFrameRows(const FrameInput& in, uint32_t rows[kFrameRows][4]) {
  std::memset(rows, 0, sizeof(uint32_t) * 4 * kFrameRows);
  uint32_t flags = 0;
  if (in.tuning.enabled && in.light.valid) {
    flags |= kFrameShade;
  }
  if (in.tuning.shadow) {
    flags |= kFrameShadow;
  }
  if (in.light.env_sharp != 0) {
    flags |= kFrameEnv;
  }
  if (in.sun_known) {
    flags |= kFrameSun;
  }
  if (in.tuning.cascade && CascadesUsable(in.cascades) && in.camera_known) {
    flags |= kFrameCascade;
    rows[kCascadeHeaderRow][0] = in.cascades.atlas_slot;
    rows[kCascadeHeaderRow][1] = std::min<uint32_t>(in.cascades.count, kMaxCascades);
    rows[kCascadeHeaderRow][2] = Bits(in.cascades.depth_bias);
    rows[kCascadeHeaderRow][3] = Bits(std::clamp(in.cascades.fade, 0.0f, 1.0f));
    for (int c = 0; c < 3; ++c) {
      rows[kCameraRow][c] = Bits(in.camera_world[c]);
      rows[kCascadeRightRow][c] = Bits(in.cascades.right[c]);
      rows[kCascadeTopRow][c] = Bits(in.cascades.top[c]);
    }
    for (uint32_t i = 0; i < kMaxCascades; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        for (uint32_t c = 0; c < 4; ++c) {
          rows[kCascadeFirstRow + i * kCascadeRowsEach + j][c] = Bits(in.cascades.rows[i][j][c]);
        }
      }
      for (uint32_t c = 0; c < 4; ++c) {
        rows[kCascadeFirstRow + i * kCascadeRowsEach + 4][c] = Bits(in.cascades.box[i][c]);
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    rows[0][c] = Bits(in.to_sun_view[c]);
    rows[1][c] = Bits(in.light.sun[c]);
    rows[2][c] = Bits(in.light.ambient[c]);
    rows[4][c] = Bits(in.light.sky[c]);
    for (int axis = 0; axis < 3; ++axis) {
      rows[5 + axis][c] = Bits(in.world_axes_view[axis][c]);
    }
  }
  rows[0][3] = flags;
  rows[1][3] = Bits(in.shadow_reach);
  rows[2][3] = Bits(in.tuning.ambient);
  rows[3][0] = in.light.env_sharp;
  rows[3][1] = in.light.env_blur;
  // the night factor: the sun's height where the frame knows the sun's direction (and the world's up), the colour's rule where it does not
  const float night = in.sun_known ? NightFromSunHeight(Dot3(in.to_sun_view, in.world_axes_view[1])) : NightFactor(in.light.sun);
  rows[3][2] = Bits(night);
  rows[3][3] = Bits(in.tuning.spec);
  rows[4][3] = Bits(in.tuning.gain);
  rows[5][3] = Bits(std::clamp(std::isfinite(in.tuning.detail) ? in.tuning.detail : 0.0f, 0.0f, 8.0f));
}

// ---------------------------------------------------------------------------
// the game's environment map (a 512x256 dual paraboloid, four levels)
// ---------------------------------------------------------------------------

// The (u, v) of the map for a unit WORLD direction `r` (the reflection's, away from the surface), decoded from xChrome::PS_Chrome (the paint's sharp read is the same mapping):
// with (a, b, c) = (-r.y, -r.z, -r.x), a hemisphere with b > 0 fills the left half of the image and the rest the right half,
//   left  u = 0.25 - c / (4 + 4b), v = 0.5 + a / (2 + 2b)       right u = 0.75 + c / (4 - 4b), v = 0.5 + a / (2 - 2b).
inline void EnvUv(const float r[3], float* u, float* v) {
  const float a = -r[1], b = -r[2], c = -r[0];
  if (b > 0.0f) {
    *u = 0.25f - c / (4.0f + 4.0f * b);
    *v = 0.5f + a / (2.0f + 2.0f * b);
  } else {
    *u = 0.75f + c / (4.0f - 4.0f * b);
    *v = 0.5f + a / (2.0f - 2.0f * b);
  }
}

// The direction EnvUv maps to (u, v) (the inverse, for the test): false outside the discs.
inline bool EnvDirection(float u, float v, float r[3]) {
  const bool left = u < 0.5f;
  const float lu = left ? 0.25f - u : u - 0.75f;
  const float lv = v - 0.5f;
  const float k = 4.0f * lv * lv + 16.0f * lu * lu;
  if (k > 1.0f + 1e-6f) {
    return false;
  }
  const float depth = (1.0f - k) / (1.0f + k);  // |b|, the axis component's size
  const float scale = 1.0f + depth;             // 2 + 2 |b| over 2
  const float a = lv * 2.0f * scale;
  const float c = lu * 4.0f * scale;
  const float b = left ? depth : -depth;
  r[0] = -c;
  r[1] = -a;
  r[2] = -b;
  return true;
}

// ---------------------------------------------------------------------------
// the sun's visibility at a hit: the game's cascades and the cars' ray
// ---------------------------------------------------------------------------

// The distance along the surface's normal the cascade lookup is made at (hlsl/car_rt_hit_shade.hlsli kCascadeNormalOffset), metres: the cascades' texels are 1 to 25 cm and a point on
// the surface itself would shadow itself at its own depth.
inline constexpr float kCascadeNormalOffset = 0.03f;

// A view-space vector `view` in the game's world: `camera` is the camera's place there, `axes` the world's x, y and z axes in view space (FrameInput::world_axes_view; unit and
// orthogonal: the view matrix is a rigid motion), so the world coordinate along axis k is the vector's dot with it.
inline void WorldPoint(const float camera[3], const float axes[3][3], const float view[3], float out[3]) {
  for (int k = 0; k < 3; ++k) {
    out[k] = camera[k] + Dot3(view, axes[k]);
  }
}

// The fraction of the sun's light that reaches the world point `p` through the cascades: the collector's own lookup (the cascade is `count` minus the number of boxes that hold the
// point, so the finest box that holds it; the atlas coordinates and the depth from that cascade's matrix; a bilinear weighted compare of the four depth texels around it, lit
// where the stored depth is at least the receiver's), 1 where the frame has no cascades or the point is in none of the boxes. `load(x, y)` is the atlas's depth texel (x, y)
// clamped to the atlas `w` x `h`. The shader's CarMatCascadeLit does this arithmetic (and volumetrics_inject_cs.hlsl CascadeLit, which it copies, the same).
template <class LoadDepth>
inline float CascadeLit(const SunCascades& s, const float p[3], uint32_t w, uint32_t h, LoadDepth&& load) {
  if (!s.valid || s.count == 0 || s.atlas_slot == kNoSlot || w == 0 || h == 0) {
    return 1.0f;
  }
  const float r = Dot3(p, s.right);
  const float t = Dot3(p, s.top);
  uint32_t inside = 0;
  for (uint32_t i = 0; i < kMaxCascades; ++i) {
    if (i < s.count) {
      inside += (std::fabs(r + s.box[i][0]) < s.box[i][1] && std::fabs(t + s.box[i][2]) < s.box[i][3]) ? 1u : 0u;
    }
  }
  if (inside == 0) {
    return 1.0f;
  }
  const uint32_t index = s.count - inside;
  const float(*m)[4] = s.rows[index];
  const float u = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0];
  const float v = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1];
  const float depth = (p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2] + m[3][2]) * (p[0] * m[0][3] + p[1] * m[1][3] + p[2] * m[2][3] + m[3][3]) + s.depth_bias;
  const float tx = u * float(w) - 0.5f;
  const float ty = v * float(h) - 0.5f;
  const int x0 = int(std::floor(tx));
  const int y0 = int(std::floor(ty));
  const float fx = tx - float(x0);
  const float fy = ty - float(y0);
  const auto lit = [&](int x, int y) { return load(std::clamp(x, 0, int(w) - 1), std::clamp(y, 0, int(h) - 1)) >= depth ? 1.0f : 0.0f; };
  const float s00 = lit(x0, y0);
  const float s10 = lit(x0 + 1, y0);
  const float s01 = lit(x0, y0 + 1);
  const float s11 = lit(x0 + 1, y0 + 1);
  const float a = s00 + (s10 - s00) * fx;
  const float b = s01 + (s11 - s01) * fx;
  return a + (b - a) * fy;
}

// What the shader makes of the two kinds of shadow (the cascades' fraction with the sun's fade, the cars' ray): the cars' own shadow falls on themselves and on each other where the
// cascades have none (they hold the player's car at a low level of detail and no other car), and the world's shadows fall where the ray cannot see them. The darker of the two.
inline float SunVisibility(float cascade_lit, float fade, float ray_lit) {
  const float world = cascade_lit + (1.0f - cascade_lit) * std::clamp(fade, 0.0f, 1.0f);
  return std::min(world, ray_lit);
}

}  // namespace mcla::gpu::rt_hit
