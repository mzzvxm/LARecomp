#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's rules: the C++ twin of the shaders' math, and the constants builder
// ===========================================================================
// What hlsl/volumetrics_*.hlsl compute, written as plain functions so that a test can hold both sides to the same numbers
// (volumetrics_rules_test.cpp): the slice mapping of the log spaced grid and its 2D atlas, the phase function, the lamp's law, the
// extinction of the height layer, the cascade pick and the headlight shadow lookup, the reprojection of a voxel into last frame's grid,
// and MakeGpuConstants (VolInputs + the camera + the history -> VolGpuConstants, the buffer the compute passes read).
// Change one side, change the other. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "volumetrics_types.h"

namespace mcla::gpu::volumetrics {

inline constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// the grid: slices, the atlas
// ---------------------------------------------------------------------------

// ln(far / near), the constant of the slice law.
inline float LogRatio(float near_m, float far_m) { return std::log(far_m / near_m); }

// The depth (metres along the view's z) of slice boundary `k` (0 .. nz): near * (far / near)^(k / nz).
inline float SliceBoundary(float near_m, float far_m, uint32_t nz, float k) { return near_m * std::exp(LogRatio(near_m, far_m) * k / float(nz)); }
// The depth of the centre of slice `k` (what a texel stores): boundary k + 0.5.
inline float SliceCentre(float near_m, float far_m, uint32_t nz, uint32_t k) { return SliceBoundary(near_m, far_m, nz, float(k) + 0.5f); }
// The thickness of slice `k`.
inline float SliceThickness(float near_m, float far_m, uint32_t nz, uint32_t k) {
  return SliceBoundary(near_m, far_m, nz, float(k) + 1.0f) - SliceBoundary(near_m, far_m, nz, float(k));
}
// The continuous slice coordinate of a view depth z: nz * ln(z / near) / ln(far / near); the centre of slice k is at k + 0.5.
inline float SliceCoord(float near_m, float far_m, uint32_t nz, float z) { return float(nz) * std::log(std::max(z, near_m * 1e-3f) / near_m) / LogRatio(near_m, far_m); }

// The tile of slice `k` in the 2D atlas.
inline void TileOf(const GridDesc& g, uint32_t k, uint32_t* tx, uint32_t* ty) {
  *tx = k % g.tiles_x;
  *ty = k / g.tiles_x;
}
// The atlas texel of voxel (x, y, k).
inline void AtlasTexel(const GridDesc& g, uint32_t x, uint32_t y, uint32_t k, uint32_t* ax, uint32_t* ay) {
  uint32_t tx, ty;
  TileOf(g, k, &tx, &ty);
  *ax = tx * g.nx + x;
  *ay = ty * g.ny + y;
}
// The atlas uv of the point (u, v) in 0..1 of slice `k`'s tile, kept half a texel inside the tile so that the bilinear filter does not
// take its neighbour tile's texel.
inline void AtlasUv(const GridDesc& g, float u, float v, uint32_t k, float* au, float* av) {
  uint32_t tx, ty;
  TileOf(g, k, &tx, &ty);
  const float x = std::clamp(u * float(g.nx), 0.5f, float(g.nx) - 0.5f);
  const float y = std::clamp(v * float(g.ny), 0.5f, float(g.ny) - 0.5f);
  *au = (float(tx * g.nx) + x) / float(g.atlas_width());
  *av = (float(ty * g.ny) + y) / float(g.atlas_height());
}

// ---------------------------------------------------------------------------
// the camera
// ---------------------------------------------------------------------------

// The world position of the view ray through (u, v) (0..1, v down) at view depth z: pos + right x tan_x (2u - 1) z + up tan_y (1 - 2v) z + fwd z.
inline void ViewToWorld(const VolCamera& c, float u, float v, float z, float out[3]) {
  const float x = (2.0f * u - 1.0f) * c.tan_x * z;
  const float y = (1.0f - 2.0f * v) * c.tan_y * z;
  for (int i = 0; i < 3; ++i) {
    out[i] = c.pos[i] + c.right[i] * x + c.up[i] * y + c.forward[i] * z;
  }
}
// A world point in a camera's grid: (u, v) in 0..1 and the view depth z; false behind it.
inline bool WorldToView(const VolCamera& c, const float p[3], float* u, float* v, float* z) {
  const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
  const float x = d[0] * c.right[0] + d[1] * c.right[1] + d[2] * c.right[2];
  const float y = d[0] * c.up[0] + d[1] * c.up[1] + d[2] * c.up[2];
  const float f = d[0] * c.forward[0] + d[1] * c.forward[1] + d[2] * c.forward[2];
  if (!(f > 1e-4f)) {
    return false;
  }
  *u = 0.5f * (x / (f * c.tan_x) + 1.0f);
  *v = 0.5f * (1.0f - y / (f * c.tan_y));
  *z = f;
  return true;
}

// ---------------------------------------------------------------------------
// the medium and the lights
// ---------------------------------------------------------------------------

// The Henyey-Greenstein phase function: (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5). cos is between the light's direction of travel and
// the direction the light leaves toward the camera, so that it is 1 for light that goes on to the camera (a lamp or the sun in front of it).
inline float HenyeyGreenstein(float cos_theta, float g) {
  const float g2 = g * g;
  const float d = 1.0f + g2 - 2.0f * g * cos_theta;
  return (1.0f - g2) / (4.0f * kPi * d * std::sqrt(d));
}

// The extinction at height y with the noise value n in 0..1: sigma_t (1 + ground_k exp(-max(y - ground_y, 0) / ground_h)) (1 + noise_k (2n - 1)).
inline float Extinction(const VolWorld& w, float y, float n) {
  float layer = 1.0f;
  if (w.density[2] > 0.0f) {
    layer += w.density[3] * std::exp(-std::max(y - w.density[1], 0.0f) / w.density[2]);
  }
  return std::max(0.0f, w.density[0] * layer * (1.0f + w.noise[0] * (2.0f * n - 1.0f)));
}

// The cone of a spot light: 1 inside the inner angle, 0 outside the outer one, smooth between. `cos_axis` is the cosine between the lamp's
// axis and the direction to the point; cos_outer <= -1 is a point light (no cone).
inline float SpotFactor(float cos_axis, float cos_outer, float cos_inner) {
  if (cos_outer <= -0.999f) {
    return 1.0f;
  }
  const float t = std::clamp((cos_axis - cos_outer) / std::max(cos_inner - cos_outer, 1e-4f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
// The distance law of a lamp: 1 / (d^2 + near^2), windowed so that it is exactly 0 at the reach: saturate(1 - (d / range)^4)^2. The near radius is the
// lamp's own (volumetrics_lamps.h: the game's lamps saturate at the radius sqrt(intensity); the headlights' is the lit shaders' falloff distance), the
// calibration against the road's pool of light (V7); a lamp that says none (the test lights) has the 0.8 m below, which keeps the singularity out.
inline constexpr float kLampNear = 0.8f;
// The scene units of a light's colour (hlsl/volumetrics_inject_cs.hlsl kLightGain): the scattered light is the light's colour, in the game's units (the
// producers' MakeVolLamp and ApplyHeadlightUnits), times this, times the haze's extinction. 60 = pi x 19.1 (volumetrics_lamps.h kPhysicalGain and
// kVisibilityBoost: what the calibration says it comes to).
inline constexpr float kLightGain = 60.0f;
// The near radius squared of a lamp: its own (VolLamp::pad[0], volumetrics_lamps.h: the game's lamps saturate at their plateau's radius) or, when it says
// none (0, a test light), kLampNear squared.
inline float LampNearSqOrDefault(float near_sq) { return near_sq > 0.0f ? near_sq : kLampNear * kLampNear; }
inline float LampFalloff(float dist, float range, float near_sq) {
  if (!(dist < range)) {
    return 0.0f;
  }
  const float r = dist / range;
  const float w = std::clamp(1.0f - r * r * r * r, 0.0f, 1.0f);
  return w * w / (dist * dist + LampNearSqOrDefault(near_sq));
}
inline float LampFalloff(float dist, float range) { return LampFalloff(dist, range, 0.0f); }

// ---------------------------------------------------------------------------
// the sun's cascades (the collector's lookup) and the headlights' shadow maps
// ---------------------------------------------------------------------------

// The number of cascade boxes that hold the world point `p` (the collector's count) and so the cascade: 4 minus the count, as the game
// picks it. cascade_count (<= 4) is the number the frame has; a point in none is outside every cascade (-1: no shadow data).
inline int CascadeOf(const VolSun& s, const float p[3]) {
  const float r = p[0] * s.right[0] + p[1] * s.right[1] + p[2] * s.right[2];
  const float t = p[0] * s.top[0] + p[1] * s.top[1] + p[2] * s.top[2];
  int count = 0;
  for (uint32_t i = 0; i < s.cascade_count && i < kMaxCascades; ++i) {
    const VolCascade& c = s.cascades[i];
    if (std::fabs(r + c.box[0]) < c.box[1] && std::fabs(t + c.box[2]) < c.box[3]) {
      ++count;
    }
  }
  if (count == 0) {
    return -1;
  }
  return int(s.cascade_count) - count;
}
// The point in cascade `c`'s map: the atlas uv and the receiver depth (the collector's formulas, VolCascade).
inline void CascadePoint(const VolCascade& c, const float p[3], float* u, float* v, float* depth) {
  float o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = p[0] * c.rows[0][j] + p[1] * c.rows[1][j] + p[2] * c.rows[2][j] + c.rows[3][j];
  }
  *u = o[0];
  *v = o[1];
  *depth = o[2] * o[3];
}
// A headlight's shadow view: the atlas uv and the depth of the world point; false behind the light.
inline bool ShadowViewPoint(const VolShadowView& view, const float p[3], float* u, float* v, float* depth) {
  float o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = view.world_to_shadow[4 * j + 0] * p[0] + view.world_to_shadow[4 * j + 1] * p[1] + view.world_to_shadow[4 * j + 2] * p[2] +
           view.world_to_shadow[4 * j + 3];
  }
  if (!(o[3] > 0.0f)) {
    return false;
  }
  *u = o[0] / o[3];
  *v = o[1] / o[3];
  *depth = o[2] / o[3];
  return true;
}
// Whether a receiver at `depth` is lit by a map that stores `stored` there: stored 1 is "nothing drawn", a receiver outside 0..1 is outside the
// map's planes, else lit when stored >= depth - bias (the reader of local_shadow_rules.h).
inline bool ShadowLit(float stored, float depth, float bias) {
  if (!(depth > 0.0f && depth < 1.0f) || stored >= 1.0f) {
    return true;
  }
  return stored >= depth - bias;
}

// The fraction of a headlight's beam that reaches the world point `p` (1: all of it), as hlsl/volumetrics_inject_cs.hlsl HeadlightLit computes it: the
// published matrix, a point behind the light, outside the map's depth range or outside the view's tile (atlas_rect = u0 v0 u1 v1: the far corner, as
// local_shadow_types.h publishes it) is lit; else a bilinear weighting of the four texels' compares, the taps clamped into the tile's texel box so that a
// neighbouring view's texels are never read. `fetch(x, y)` gives the atlas's depth at a texel, the atlas is `width` x `height` texels.
template <class Fetch>
inline float ShadowShare(const VolShadowView& view, const float p[3], int width, int height, Fetch&& fetch) {
  float u, v, depth;
  if (view.valid == 0 || !ShadowViewPoint(view, p, &u, &v, &depth)) {
    return 1.0f;
  }
  const float* r = view.atlas_rect;
  if (!(depth > 0.0f && depth < 1.0f) || u < r[0] || v < r[1] || u > r[2] || v > r[3]) {
    return 1.0f;
  }
  const int lo_x = int(std::floor(r[0] * float(width) + 0.5f));
  const int lo_y = int(std::floor(r[1] * float(height) + 0.5f));
  const int hi_x = std::max(int(std::floor(r[2] * float(width) + 0.5f)) - 1, lo_x);
  const int hi_y = std::max(int(std::floor(r[3] * float(height) + 0.5f)) - 1, lo_y);
  const float tx = u * float(width) - 0.5f;
  const float ty = v * float(height) - 0.5f;
  const int x0 = int(std::floor(tx));
  const int y0 = int(std::floor(ty));
  const float fx = tx - float(x0);
  const float fy = ty - float(y0);
  const float ref = depth - view.depth_bias;
  auto lit = [&](int x, int y) {
    const float stored = fetch(std::clamp(x, lo_x, hi_x), std::clamp(y, lo_y, hi_y));
    return (stored >= 1.0f || stored >= ref) ? 1.0f : 0.0f;
  };
  const float a = lit(x0, y0);
  const float b = lit(x0 + 1, y0);
  const float c = lit(x0, y0 + 1);
  const float d = lit(x0 + 1, y0 + 1);
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// ---------------------------------------------------------------------------
// the weather and the hour: the core's defaults (VOL-INPUTS' producers replace them with the game's own weather)
// ---------------------------------------------------------------------------

inline float SmoothStep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
// How much of the sun there is for the sun term: 0 at and under -2 degrees, in full from 8 to 30 degrees, none from 55 (a noon sun gives no shafts
// worth a pass; the survey's gate), a smooth ramp between.
inline float SunStrength(float elevation_deg) {
  return SmoothStep(-2.0f, 8.0f, elevation_deg) * (1.0f - SmoothStep(30.0f, 55.0f, elevation_deg));
}
// The night factor from the sun's height: 1 at and under -4 degrees, 0 from 6 degrees up.
inline float NightFactor(float elevation_deg) { return 1.0f - SmoothStep(-4.0f, 6.0f, elevation_deg); }
// The wetness (VS c43.x, 0.095 dry, 1.17 in rain) as 0..1: the sun shafts' rain gate (shafts.cpp kWetStart / kWetEnd).
inline float WetFactor(float wetness) { return SmoothStep(0.2f, 0.7f, wetness); }
// ---------------------------------------------------------------------------
// the look: the numbers by weather and by hour (VOL-LOOK-TUNE; perf/VOLUMETRICS-TUNING/TUNING.md says why each one is what it is)
// ---------------------------------------------------------------------------
// The first look (VOL-ON-MAIN's run vol2) had ONE set of numbers for every weather: a clear night showed a grey veil over the sky, rain a white ball around the
// oncoming headlights, and the foggy night the clear night's density (the old fog test needed an opacity of 0.7, the game's foggy NIGHT is 0.56). The cause is the
// units: a lamp's or a headlight's colour is in the game's units (a headlight is 1352 = kPeak x D^2) and the scatter is sigma x kLightGain (60) x that, so how much
// light the air gives back is set by the gains, and the gains that make a lamp show in a clear night (halos a few metres wide) are far too much for a headlight (its
// phase function is forward and its beam covers the camera) and for a medium that is ten times thicker (rain, fog). So the look is a TABLE of three weathers (clear,
// fog, rain) by three hours (night, low sun, day), each cell the numbers below, mixed by weights that come from what the draws tell (the sun's height, the wetness
// VS c43.x, the fog quad's opacity), and a cap on the light the composite may add.

// What the frame's look is made of: how much of each weather and each hour it is (each triple sums to 1).
struct LookWeights {
  float clear = 1.0f, fog = 0.0f, rain = 0.0f;
  float night = 0.0f, low = 0.0f, day = 1.0f;
};

// One cell of the table.
struct VolLook {
  float sigma = 0.0f;         // the medium's extinction at and above the ground layer's floor, 1/m
  float ground_k = 0.0f;      // the ground layer's extra density at the ground, a multiplier (1: twice as thick)
  float ground_h = 40.0f;     // its scale height, m
  float g_sun = 0.6f;         // the phase function's anisotropy per light class (Henyey-Greenstein g)
  float g_lamp = 0.3f;
  float g_head = 0.45f;
  float sun_strength = 1.0f;  // a multiplier per light class on top of the shader's unit gain (kLightGain for lamps and headlights, 1 for the sun)
  float lamp_strength = 1.0f;
  float head_strength = 1.0f;
  float extinction = 0.1f;    // how much of the medium's own dimming the composite applies (1: all of it)
  float cap = 1.5f;           // the soft limit on the light the composite adds to a pixel (scene units, the largest channel); 0: none
};

inline constexpr int kLookFields = 11;
enum class LookWeather : int { kClear = 0, kFog = 1, kRain = 2 };
enum class LookHour : int { kNight = 0, kLow = 1, kDay = 2 };

// The table: [weather][hour][field]. Fields: sigma, ground_k, ground_h, g_sun, g_lamp, g_head, sun_strength, lamp_strength, head_strength, extinction, cap.
//  CLEAR  night: the air is only dust: a lamp shows a halo a few metres wide (lamp strength 4: the game's lamp colour is its intensity, 55 to 100, so its pool is
//         bright; 4 puts the halo about 0.25 scene units above the sky), a headlight a faint beam (0.09: its units are 25 times a lamp's and the camera sits in the
//         oncoming car's beam, so any more is a veil over the sky), the cap 1.5 is never reached by the look itself (only by the debug multipliers).
//         low sun: dusk and dawn mist, 2.3 times the night's thickness with a deeper ground layer, the sun's forward lobe tighter (g 0.72), almost no dimming (0.05:
//         the extinction of a bright sky is what made the first look a dim haze toward the sun).
//         day: nearly clear air; the sun term is only worth a pass while the sun is low (SunStrength is 0 from 55 degrees).
//  FOG    the game's foggy weather: 0.010 1/m (the first look used the clear 0.0012 there), a thinner ground layer (the fog is everywhere), headlights wider in the
//         phase (g 0.4: the beam reads as a cone from behind, not only as a glow when it faces the camera), lamps 3, headlights 0.16, the cap 6.
//  RAIN   wet air 0.008 1/m; headlights 0.12 (the first look's white ball: 43 scene units at the oncoming car, 3.7 now), lamps 3 (visible cones), the cap 5; the sun's
//         term is off (rain gates the shafts too).
inline constexpr float kLookTable[3][3][kLookFields] = {
    // clear
    {{0.0012f, 1.0f, 40.0f, 0.60f, 0.30f, 0.45f, 0.0f, 4.0f, 0.09f, 0.15f, 1.5f},   // night
     {0.0028f, 1.5f, 50.0f, 0.72f, 0.30f, 0.45f, 1.0f, 4.0f, 0.09f, 0.05f, 1.5f},   // low sun
     {0.0008f, 1.0f, 40.0f, 0.65f, 0.30f, 0.45f, 1.0f, 4.0f, 0.09f, 0.05f, 1.0f}},  // day
    // fog
    {{0.0100f, 0.5f, 40.0f, 0.60f, 0.35f, 0.40f, 0.0f, 3.0f, 0.16f, 0.20f, 6.0f},
     {0.0100f, 0.5f, 40.0f, 0.60f, 0.35f, 0.40f, 0.5f, 3.0f, 0.16f, 0.20f, 6.0f},
     {0.0080f, 0.5f, 40.0f, 0.60f, 0.35f, 0.40f, 0.5f, 3.0f, 0.16f, 0.20f, 4.0f}},
    // rain
    {{0.0080f, 0.5f, 40.0f, 0.60f, 0.35f, 0.50f, 0.0f, 3.0f, 0.12f, 0.20f, 5.0f},
     {0.0080f, 0.5f, 40.0f, 0.60f, 0.35f, 0.50f, 0.0f, 3.0f, 0.12f, 0.20f, 5.0f},
     {0.0060f, 0.5f, 40.0f, 0.60f, 0.35f, 0.50f, 0.0f, 3.0f, 0.12f, 0.20f, 4.0f}}};

// The game's foggy weather from the fog quad's opacity (PS c64.w of PSBlendedHeightFog*, the run's census): the foggy NIGHT is 0.56 (a clear night 0 to 0.13, the
// dusk 0.41, the clear day 0.4 to 0.67) and the foggy DAY 0.94; so the night's ramp is 0.35 to 0.52 and the day's 0.75 to 0.90, mixed by the night factor. A cloudy
// night (0.59) is read as fog too: it has the game's own thick fog there (CHECK: the guest's weather weights 0x827E1B30 would tell them apart).
inline float FogWeight(float opacity, float night) {
  const float at_night = SmoothStep(0.35f, 0.52f, opacity);
  const float by_day = SmoothStep(0.75f, 0.90f, opacity);
  return by_day + (at_night - by_day) * std::clamp(night, 0.0f, 1.0f);
}

// The weights of a frame from the sun's height in degrees, the wetness mapped to 0..1 (WetFactor) and the fog quad's opacity. The hours: night as NightFactor, day
// from 22 degrees (full at 40: the dusk of the chain, 17.2 h, is 25 degrees and still wants its mist), the low sun the rest. The weathers: rain is the wetness, fog the fog weight in what rain leaves.
inline LookWeights LookWeightsFor(float elevation_deg, float wet01, float fog_opacity) {
  LookWeights w;
  w.night = NightFactor(elevation_deg);
  w.day = SmoothStep(22.0f, 40.0f, elevation_deg);
  w.low = std::max(0.0f, 1.0f - w.night - w.day);
  const float sum = w.night + w.low + w.day;
  w.night /= sum;
  w.low /= sum;
  w.day /= sum;
  w.rain = std::clamp(wet01, 0.0f, 1.0f);
  w.fog = (1.0f - w.rain) * std::clamp(FogWeight(fog_opacity, w.night), 0.0f, 1.0f);
  w.clear = 1.0f - w.rain - w.fog;
  return w;
}

// The cell of the table.
inline VolLook LookCell(LookWeather weather, LookHour hour) {
  const float* c = kLookTable[int(weather)][int(hour)];
  VolLook l;
  l.sigma = c[0];
  l.ground_k = c[1];
  l.ground_h = c[2];
  l.g_sun = c[3];
  l.g_lamp = c[4];
  l.g_head = c[5];
  l.sun_strength = c[6];
  l.lamp_strength = c[7];
  l.head_strength = c[8];
  l.extinction = c[9];
  l.cap = c[10];
  return l;
}

// The look of a frame: the table's cells mixed by the weights (a weather change or the sun's setting blends, nothing jumps).
inline VolLook LookFor(const LookWeights& w) {
  const float weather[3] = {w.clear, w.fog, w.rain};
  const float hour[3] = {w.night, w.low, w.day};
  float sum[kLookFields] = {};
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      const float weight = weather[a] * hour[b];
      for (int i = 0; i < kLookFields; ++i) {
        sum[i] += weight * kLookTable[a][b][i];
      }
    }
  }
  VolLook l;
  l.sigma = sum[0];
  l.ground_k = sum[1];
  l.ground_h = sum[2];
  l.g_sun = sum[3];
  l.g_lamp = sum[4];
  l.g_head = sum[5];
  l.sun_strength = sum[6];
  l.lamp_strength = sum[7];
  l.head_strength = sum[8];
  l.extinction = sum[9];
  l.cap = sum[10];
  return l;
}
inline VolLook LookFor(float elevation_deg, float wet01, float fog_opacity) { return LookFor(LookWeightsFor(elevation_deg, wet01, fog_opacity)); }

// ---------------------------------------------------------------------------
// the clamps: what keeps a light from whiting out the picture
// ---------------------------------------------------------------------------

// The composite's soft limit on the light it adds (hlsl/volumetrics_apply_ps.hlsl SoftLimit is the twin): the largest channel is kept as it is up to the knee
// (half the cap), then rolls off exponentially toward the cap with the same slope at the knee, so that no pixel gets more than `cap` however many lights add up or
// however thick the air is, and the hue stays the light's (all three channels are scaled alike). cap <= 0: no limit.
inline constexpr float kSoftKnee = 0.5f;
inline float SoftLimit(float x, float cap) {
  if (!(x > 0.0f)) {
    return 0.0f;
  }
  if (!(cap > 0.0f)) {
    return x;
  }
  const float knee = cap * kSoftKnee;
  if (x <= knee) {
    return x;
  }
  const float room = cap - knee;
  return knee + room * (1.0f - std::exp(-(x - knee) / room));
}
inline void SoftLimitColour(float c[3], float cap) {
  const float m = std::max({c[0], c[1], c[2], 0.0f});
  if (!(cap > 0.0f) || !(m > 0.0f)) {
    return;
  }
  const float scale = SoftLimit(m, cap) / m;
  for (int i = 0; i < 3; ++i) {
    c[i] *= scale;
  }
}

// A car's headlights are low beams: the lit shaders (headlight_rig_rules.h LampScale: kCutLow, kCutHigh, kNearStart, kNearSpan) put no light above the lamp's own
// height by a ramp in the sine of the angle a point is above the lamp, and none in the lamp's first 0.8 m. The beam in the air is the same light: without it the
// cone lit the sky above the oncoming car and the air right at the nose (volumetrics_inputs_test.cpp holds these four numbers against the rig's own).
inline constexpr float kHeadCutLow = 0.10f;
inline constexpr float kHeadCutHigh = 0.38f;
inline constexpr float kHeadNearStart = 0.8f;
inline constexpr float kHeadNearSpan = 1.5f;
// `sin_above` is the y of the unit direction from the lamp to the point (the light's direction of travel).
inline float HeadlightCutoff(float sin_above) { return 1.0f - SmoothStep(kHeadCutLow, kHeadCutHigh, sin_above); }
inline float HeadlightNearFade(float dist) { return std::clamp((dist - kHeadNearStart) / kHeadNearSpan, 0.0f, 1.0f); }

// ---------------------------------------------------------------------------
// the frame's noise
// ---------------------------------------------------------------------------

// Interleaved gradient noise (Jimenez): a value in 0..1 per integer position, stable and cheap, shifted by `frame` along its diagonal so
// that the pattern moves from frame to frame.
inline float InterleavedGradient(float x, float y, uint32_t frame) {
  const float shift = 5.588238f * float(frame & 63u);
  const float v = 0.06711056f * (x + shift) + 0.00583715f * (y + shift);
  const float f = 52.9829189f * (v - std::floor(v));
  return f - std::floor(f);
}
// The radical inverse of `index` in `base` (a Halton term): the frame's slice offset.
inline float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float f = 1.0f;
  while (index > 0) {
    f /= float(base);
    result += f * float(index % base);
    index /= base;
  }
  return result;
}

// ---------------------------------------------------------------------------
// the constants
// ---------------------------------------------------------------------------

// How the frame's history is used: its camera and grid (the previous frame's, kept by the core), and the weight the history keeps (0: none).
struct HistoryUse {
  bool valid = false;
  VolCamera camera;
  float far_m = 0.0f;
  float near_m = 0.0f;
  float weight = 0.0f;
  // The headlights of the volume that is the history (as the constants gave them to the inject): a moving light is compared with where it was.
  std::array<VolLamp, kMaxHeadlights> headlights{};
  uint32_t headlight_count = 0;
};

// The weight of the history by level and by how far the camera moved: 0.9 where it is still, falling to 0.5 for a fast move
// (the history is blurrier than the frame then: a headlight sweeping a wall leaves no trail longer than a few frames).
inline float HistoryWeight(const GridDesc& g, bool valid, float camera_step_m, float camera_turn_rad) {
  if (!g.temporal || !valid) {
    return 0.0f;
  }
  const float move = std::clamp(camera_step_m / 3.0f + camera_turn_rad / 0.1f, 0.0f, 1.0f);
  return 0.9f - 0.4f * move;
}

inline void CopyCamera(const VolCamera& c, float pos[4], float right[4], float up[4], float fwd[4]) {
  for (int i = 0; i < 3; ++i) {
    pos[i] = c.pos[i];
    right[i] = c.right[i];
    up[i] = c.up[i];
    fwd[i] = c.forward[i];
  }
  pos[3] = c.tan_x;
  right[3] = c.tan_y;
  up[3] = 0.0f;
  fwd[3] = 0.0f;
}

// The far plane of the frame: the producer's request in VolWorld::far_m when it is a usable fraction of the level's, else the level's.
inline float FarOf(const GridDesc& g, const VolWorld& w) {
  const float asked = w.haze_colour[3];
  if (asked >= 30.0f && asked <= g.far_m) {
    return asked;
  }
  return g.far_m;
}

// The buffer of the compute passes: everything of `in` for the level's grid (lamps and headlights cut to the level's counts), the camera
// and its history, the frame's jitter. `frame` is the guest's frame number.
inline void MakeGpuConstants(const GridDesc& g, const VolInputs& in, const VolCamera& cam, const HistoryUse& history, uint32_t frame, VolGpuConstants* out) {
  std::memset(out, 0, sizeof(*out));
  const float far_m = FarOf(g, in.world);
  out->grid[0] = g.nx;
  out->grid[1] = g.ny;
  out->grid[2] = g.nz;
  out->grid[3] = g.tiles_x;
  const float ratio = LogRatio(g.near_m, far_m);
  out->depth[0] = g.near_m;
  out->depth[1] = far_m;
  out->depth[2] = ratio;
  out->depth[3] = 1.0f / ratio;
  const bool use_history = g.temporal && history.valid && history.weight > 0.0f;
  out->history[0] = use_history ? history.far_m : far_m;
  out->history[1] = use_history ? LogRatio(g.near_m, history.far_m) : ratio;
  out->history[2] = use_history ? std::clamp(history.weight, 0.0f, 0.98f) : 0.0f;
  // The frame's offset inside the voxel: the Halton terms of the frame's number (the shader adds the voxel's own blue noise). NOISE-FOLLOW: the number counts to 1024 like the other effects' frame terms (`& 1023`);
  // it was `frame % 16`, which repeated the offsets every 16 frames and left a quarter of their amplitude as a 3.75 Hz shimmer under a history of 0.9 (any 16 frames in a row still leave no gap over 0.16 of the voxel in any of the three terms: 0.15 before).
  out->jitter[0] = Halton((frame & 1023u) + 1u, 2);
  out->jitter[1] = Halton((frame & 1023u) + 1u, 3);
  out->jitter[2] = Halton((frame & 1023u) + 1u, 5);
  out->jitter[3] = float(frame & 0xFFFFu);
  CopyCamera(cam, out->cam_pos, out->cam_right, out->cam_up, out->cam_fwd);
  CopyCamera(use_history ? history.camera : cam, out->prev_pos, out->prev_right, out->prev_up, out->prev_fwd);
  out->world = in.world;
  out->world.haze_colour[3] = far_m;
  out->sun = in.sun;
  if (!in.sun_valid) {
    out->sun.cascade_count = 0;
    out->sun.atlas_slot = kNoSlot;
  }
  const uint32_t lamps = std::min({in.lamp_count, g.lamps, kMaxLamps});
  const uint32_t heads = std::min({in.headlight_count, g.headlights, kMaxHeadlights});
  out->counts[0] = lamps;
  out->counts[1] = heads;
  out->counts[2] = in.shadow_view_count;
  out->counts[3] = in.shadow_atlas_slot;
  out->shadow_atlas[0] = in.shadow_atlas_size[0];
  out->shadow_atlas[1] = in.shadow_atlas_size[1];
  for (uint32_t i = 0; i < lamps; ++i) {
    out->lamps[i] = in.lamps[i];
  }
  for (uint32_t i = 0; i < heads; ++i) {
    out->headlights[i] = in.headlights[i];
    // A headlight's shadow view beyond the level's view count (Medium reads two headlights) is not used.
    if (out->headlights[i].shadow_view >= int32_t(in.shadow_view_count)) {
      out->headlights[i].shadow_view = -1;
    }
    if (out->headlights[i].shadow_view_b >= int32_t(in.shadow_view_count)) {
      out->headlights[i].shadow_view_b = -1;
    }
  }
  for (uint32_t i = 0; i < in.shadow_view_count && i < kMaxShadowViews; ++i) {
    out->shadow_views[i] = in.shadow_views[i];
  }
  // the history's headlights (VOL-STABLE-2): only where the history is read
  if (use_history) {
    const uint32_t before = std::min({history.headlight_count, g.headlights, kMaxHeadlights});
    out->prev_counts[0] = before;
    for (uint32_t i = 0; i < before; ++i) {
      out->prev_headlights[i] = history.headlights[i];
    }
  }
}

// Whether a frame has anything for the volume to show: a medium dense enough to matter, a sun that lights it, or a lamp or a headlight.
// The pass is skipped when it has none (a clear noon with no light on: nothing is recorded). `sun_energy` is the largest channel of the sun's
// colour times its strength, `min_sigma` the floor under which the medium is no medium.
inline bool HasSomethingToShow(const VolInputs& in, float sun_energy, float min_sigma) {
  const bool medium = in.world.density[0] > min_sigma;
  const bool sun = sun_energy > 1e-3f && medium;
  const bool lights = in.lamp_count > 0 || in.headlight_count > 0;
  return medium && (sun || lights);
}

}  // namespace mcla::gpu::volumetrics
