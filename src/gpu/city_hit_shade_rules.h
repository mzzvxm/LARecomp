#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city hit library's C++ twin: what a ray that hit the city shows (CITY-RT M2 package H4; the shader is hlsl/city_hit_shade.hlsli)
// ===========================================================================
// The owner's goal (2026-11-20) for the reflections is that you can clearly identify what you see in them: a wall, a road, a lamp head, a window, a sign. This header is the library's
// arithmetic in plain C++, written apart from the shader: city_hit_shade_rules_test.cpp compiles the .hlsli ITSELF as C++ (through a shim of the HLSL types, the pool, the tables and a fake
// texture set) and checks it against this header on thousands of random hits, and checks this header's own claims (the laws, the estimator of the lamp choice, the calibration).
//
// THE PIECES (each is a function of the shader, in the shader's order):
//   decode        src/citypack/city_hit_format.h DecodeHit (hlsl/city_hit_common.hlsli CityHitDecode): the triangle, its uv and vertex colour, the run's material
//   albedo        REFLECT: the diffuse texture at TextureLevel (log2 of the texels the ray cone covers: width x the triangle's repeats a metre x the texture's size; a grazing view covers more),
//                 read bilinear in two levels (a road's packed diffuse, (grey, specular mask), as its red channel alone: TexelAlbedo), times the tint; past the last stored level it fades to the
//                 average colour (ResidentLevel); GI, no texture, not resident: the average colour;
//                 the baked vertex occlusion (the corner colour's alpha) mixed in by aoMix. A wet road is darker by kWetDarken x the wetness.
//   light         the frame's ambient with its hemisphere term (rt_hit_frame.h: the same rows as the car's hit shading), the sun's colour x N.L x its visibility (the game's cascades where the
//                 frame has them and/or one ray toward a place on the sun's disc against the whole top level), the frame's lamps (the game's laws, below), the GI field's feedback, and, last, the air between the camera and the hit (a hook: the game's distance fog)
//   surface       colour = albedo x light x (the class's diffuse share x (1 - metal)); in REFLECT mode the sky: the environment map at the reflected direction x Schlick's fresnel x the
//                 smoothness's weight x the class's share, the diffuse taking the rest (1 - weight x fresnel); then the glow of what glows (albedo x emissive scale x the gain x the lamp schedule)
//   resolve       ResolveHit (M2 section 4): the picture's colour where the hit's point is seen (in front of the camera, inside the picture away from its edge, the picture's depth agreeing,
//                 the surface facing the camera: each fades, the weight is the product), the model's colour x the class's gain where it is not, both by the weight between; GI caps the picture's luminance
//   calibration   a hit the picture speaks for in full adds the log of its luminance and of the ungained model's to the class's counters (rt_hit_calibration.h), the host turns the windows
//                 into one gain a class
//
// R7 (CITY-RT M2 R7): the lamps of a hit come from the WORLD FRAME's grid (rt_world_frame_rules.h: LampLightAt walks the 64 m cells around the hit, the twin of the shader's CITY_HIT_LAMP_GRID path): the
// first kLampMax lamps of the lookup THAT REACH the hit (a lamp out of the game's reach takes no slot; the walk stops after kLampWalkMax lamps whatever they were), their light summed and capped in
// luminance at kLampSumCap (LampsCapped), or ONE lamp chosen with a shadow ray (LampPickCapped) with the same cap; and what a
// glowing material glows by follows its kind (a lamp lens the street lamps' schedule, neon, signs, lit windows the fill lights', a traffic signal always: Reads::glow_factor, the world rows' row 47).
//
// THE LAMPS (src/citypack/lamp_table.h, the game's own laws read from the lit shaders and sub_822FD2C8): term = colour x saturate(I / d^2) x cone x N.L x the schedule: a lamp lights with its
// full colour out to d = sqrt(I) and falls as I / d^2 beyond (a fill light of 20 reaches 44.7 m where the term is 1 percent); the cone is a ramp in the cosine, full at cos(inner) and zero at
// cos(outer) (LampConeCosines); an omni light has none (cosOuter -2). Without the shadow flag the light of a hit is the SUM of the lamps in the range; with it ONE lamp is chosen with the
// probability of its brightness and cast a shadow ray: the estimate's expectation is the sum of the lit lamps' light (LampPick is checked on that). The GPU record of a lamp (GpuLamp) is three rows
// of four words in VIEW space: MakeGpuLamp turns the table's world lamp into it with the frame's camera and axes.
//
// Nothing here includes the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

#include "citypack/city_hit_format.h"
#include "rt_hit_calibration.h"
#include "rt_hit_frame.h"

namespace mcla::gpu::city_hit_shade {

namespace ch = larecomp::citypack;

// ---------------------------------------------------------------------------
// vectors (a few operators: the arithmetic below reads like the shader's)
// ---------------------------------------------------------------------------

struct V3 {
  float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a) { return a * (1.0f / Length(a)); }
inline float Saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float Luma(V3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
inline V3 Max0(V3 a) { return {std::max(a.x, 0.0f), std::max(a.y, 0.0f), std::max(a.z, 0.0f)}; }
// A hit's colour in the picture's units (hlsl kCityHitMaxColour): at most what a half float keeps with room.
inline constexpr float kHitMaxColour = 60000.0f;
// hlsl NgHeld3 (nan_guard.hlsli): not negative, at most `hi`, a NaN 0, decided by the bits in the shader (max(NaN, 0) is 0 on D3D12 only).
inline float Held(float v, float hi) { return std::isnan(v) ? 0.0f : std::min(std::max(v, 0.0f), hi); }
inline V3 Held3(V3 a, float hi) { return {Held(a.x, hi), Held(a.y, hi), Held(a.z, hi)}; }
inline V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline float Smoothstep(float lo, float hi, float x) {
  const float t = Saturate((x - lo) / (hi - lo));
  return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// the numbers (the shader's CITY_HIT_* and kCityHit*; the test compares them)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kModeReflect = 0;
inline constexpr uint32_t kModeGi = 1;
inline constexpr uint32_t kPSunRay = 1, kPSunCascade = 2, kPLamps = 4, kPLampShadow = 8, kPEmissive = 16, kPCalibrate = 32, kPScreen = 64;
inline constexpr uint32_t kSourceNone = 0, kSourceScreen = 1, kSourceShaded = 2, kSourceBlend = 3;
inline constexpr uint32_t kClassCount = 16;  // gbuffer_types.h SurfaceClass: none, world, road, wet road, sidewalk, terrain, lod, foliage, paint, chrome, metal, glass, skin, emissive, interior, water
inline constexpr uint32_t kClassRoad = 2, kClassWetRoad = 3, kClassSidewalk = 4;

inline constexpr float kGrazingMin = 0.35f;
inline constexpr float kSpecFrom = 0.3f;
inline constexpr float kEnvLodSmooth = 3.0f;
inline constexpr float kWetDarken = 0.4f;
inline constexpr float kDielectricF0 = 0.04f;
inline constexpr float kLiftFloor = 0.006f;
inline constexpr float kLiftPerMetre = 0.003f;
inline constexpr float kLiftCap = 0.4f;
inline constexpr float kSunReachDefault = 2000.0f;
inline constexpr float kMinAbove = 0.08f;
inline constexpr float kLampReach = 100.0f;
inline constexpr float kLampEnd = 0.3f;
inline constexpr float kMinDepth = 0.05f;
inline constexpr float kScreenTrust = 0.98f;
inline constexpr float kCalFloor = 0.02f;
inline constexpr float kFacingSpan = 0.15f;
inline constexpr uint32_t kLampMax = 96;          // R7: the lamps of a lookup that reach one hit and are examined at most (the shader's kCityHitLampMax)
inline constexpr uint32_t kLampWalkMax = 3 * kLampMax;  // R7: the lamps the walk visits at most, reaching or not (the shader's kCityHitLampWalkMax: the bound on the time)
inline constexpr float kLampSumCap = 6.0f;        // R7: the lamp light's luminance is capped at this
inline constexpr float kLampLookup = 64.0f;       // R7: the lookup's radius where the world rows give none
inline constexpr uint32_t kGlowNight = ch::kGlowNight, kGlowLens = ch::kGlowLens, kGlowAlways = ch::kGlowAlways;
// the counters of the lamp term and the glow (the shader's CITY_HIT_CENSUS_*)
inline constexpr uint32_t kCensusLookups = 0, kCensusExamined = 1, kCensusLit = 2, kCensusCapCount = 3, kCensusCapSum = 4, kCensusGlowNight = 5, kCensusWords = 8;

// Per class: the diffuse's share (glass and chrome let most of the light go through or off) and the sky reflection's share.
inline constexpr float kDiffuseGain[kClassCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.1f, 0.5f, 0.25f, 1.0f, 1.0f, 1.0f, 0.15f};
inline constexpr float kSpecGain[kClassCount] = {0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.3f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.5f, 1.0f};

// ---------------------------------------------------------------------------
// the albedo
// ---------------------------------------------------------------------------

// The texture's level (counted from its full size) a ray cone of `footprint` metres reads at a hit whose triangle repeats the texture `density` times a metre.
inline float TextureLevel(float footprint, float density, uint32_t width, uint32_t height, float nv, float bias) {
  const float texels = footprint * density * std::sqrt(float(width) * float(height)) / std::max(nv, kGrazingMin);
  return std::log2(std::max(texels, 1e-4f)) + bias;
}

// The level counted from the FIRST RESIDENT one (what a bindless read is given) and the share of the average colour that replaces the picture beyond the last stored level.
struct ResidentRead {
  float level = 0;
  float average = 0;
};
inline ResidentRead ResidentLevel(float level, uint32_t first_mip, uint32_t mip_count) {
  ResidentRead r;
  r.level = level - float(first_mip);
  r.average = Saturate(level - float(first_mip + mip_count - 1u));
  return r;
}

// A texel as the material's albedo reads it (hlsl CityHitTexelAlbedo): the colour, or for a road's packed diffuse (`grey_in_red`: (grey, specular mask, nothing), citypack GroupGreyInRed) its red
// channel as a grey. Read as a colour that texture is green or orange (RT-GREEN-BAND: the lime band along the lower half of the ray traced car paint).
inline V3 TexelAlbedo(bool grey_in_red, V3 texel) { return grey_in_red ? V3{texel.x, texel.x, texel.x} : texel; }

// The albedo from what was read: `texel` the diffuse at the level (rgb, in the picture's linear units), `textured` whether the texture path applies, the tint, the average, the vertex
// colour's alpha (the baked occlusion; `has_colour` whether the run has one) mixed in by `ao_mix`.
inline V3 Albedo(bool textured, V3 texel, V3 tint, V3 average, float level_beyond, bool has_colour, float vertex_ao, float ao_mix) {
  V3 albedo = average;
  if (textured) {
    albedo = Lerp(texel * tint, average, level_beyond);
  }
  if (has_colour) {
    albedo = albedo * (1.0f + (vertex_ao - 1.0f) * ao_mix);
  }
  return albedo;
}

// ---------------------------------------------------------------------------
// the sun
// ---------------------------------------------------------------------------

struct Basis {
  V3 t, b;
};
inline Basis MakeBasis(V3 n) {
  const float s = n.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (s + n.z);
  const float c = n.x * n.y * a;
  return {{1.0f + s * n.x * n.x * a, s * c, -s * n.x}, {c, s + n.y * n.y * a, -n.y}};
}
// The place on the sun's disc for one ray (city_rt_shadow_rules.h DiscSample with one ray).
inline void DiscSample(float noise1, float noise2, float out[2]) {
  const float r = std::sqrt(std::min(noise1, 0.9999f));
  const float angle = 6.283185307179586f * (noise2 - std::floor(noise2));
  out[0] = r * std::cos(angle);
  out[1] = r * std::sin(angle);
}
// The direction of the sun ray: toward the sun, to a place on its disc (tan of its radius `sun_tan`), kept above the surface's tangent plane.
inline V3 SunDirection(V3 to_sun, V3 normal, float sun_tan, float noise1, float noise2) {
  if (!(sun_tan > 0.0f)) {
    return to_sun;
  }
  const Basis basis = MakeBasis(to_sun);
  float d[2];
  DiscSample(noise1, noise2, d);
  V3 direction = Normalize(to_sun + basis.t * (d[0] * sun_tan) + basis.b * (d[1] * sun_tan));
  const float above = Dot(direction, normal);
  if (above < kMinAbove) {
    direction = Normalize(direction + normal * (kMinAbove - above));
  }
  return direction;
}
// How far off the surface a ray for the hit's light starts (hlsl CityHitLift: the magnitude of the view z, which is negative for a hit behind the camera).
inline float Lift(float view_distance) { return std::min(kLiftFloor + kLiftPerMetre * std::fabs(view_distance), kLiftCap); }

// ---------------------------------------------------------------------------
// the lamps
// ---------------------------------------------------------------------------

// A lamp in the frame's view space, in the game's units (lamp_table.h Lamp turned into the frame by MakeGpuLamp).
struct Lamp {
  V3 position;
  float intensity = 0;
  V3 axis{0, -1, 0};
  float cos_outer = -2.0f;  // below -1.5: no cone
  V3 colour{1, 1, 1};
  float cos_inner = -2.0f;
};
inline float LampTerm(float intensity, float distance2) { return distance2 <= intensity ? 1.0f : intensity / distance2; }
inline float LampCone(const Lamp& l, V3 to_lamp) {
  if (l.cos_outer > -1.5f) {
    return Saturate((Dot(l.axis, -to_lamp) - l.cos_outer) / (l.cos_inner - l.cos_outer));
  }
  return 1.0f;
}
struct LampEval {
  V3 to_lamp;
  float distance = 0;
  V3 radiance;  // the unshadowed light the lamp sends to the point, N.L in it, the schedule in it
  bool reaches = false;  // in the game's reach of the point (d^2 < kLampReach x intensity): the lamps the lookup's cap counts
};
inline LampEval LampAt(float lamp_factor, const Lamp& l, V3 position, V3 normal) {
  LampEval e;
  const V3 d = l.position - position;
  const float d2 = std::max(Dot(d, d), 1e-6f);
  e.distance = std::sqrt(d2);
  e.to_lamp = d * (1.0f / e.distance);
  if (d2 < kLampReach * l.intensity) {
    e.reaches = true;
    e.radiance = l.colour * (LampTerm(l.intensity, d2) * LampCone(l, e.to_lamp) * Saturate(Dot(normal, e.to_lamp)) * lamp_factor);
  }
  return e;
}
// The sum of the lamps' light (no rays).
inline V3 LampsSum(float lamp_factor, const Lamp* lamps, uint32_t count, V3 position, V3 normal) {
  V3 sum;
  for (uint32_t i = 0; i < count; ++i) {
    sum = sum + LampAt(lamp_factor, lamps[i], position, normal).radiance;
  }
  return sum;
}
// ONE lamp chosen by its unshadowed brightness (`noise` over the sum of them), `blocked(origin direction reach)` the shadow ray toward it, its light times sum / its own; the estimate's expectation over
// the noise is the sum of the lamps' light that is not blocked.
inline V3 LampPick(float lamp_factor, const Lamp* lamps, uint32_t count, V3 position, V3 normal, float noise, float view_z, const std::function<bool(V3, V3, float)>& blocked) {
  float weight_sum = 0.0f;
  V3 sum;
  for (uint32_t i = 0; i < count; ++i) {
    const LampEval e = LampAt(lamp_factor, lamps[i], position, normal);
    sum = sum + e.radiance;
    weight_sum += Luma(e.radiance);
  }
  if (!(weight_sum > 0.0f)) {
    return sum;
  }
  const float pick = noise * weight_sum;
  float running = 0.0f;
  V3 chosen, to_chosen{0, 1, 0};
  float distance = 0.0f, weight = 0.0f;
  for (uint32_t j = 0; j < count && weight <= 0.0f; ++j) {
    const LampEval e = LampAt(lamp_factor, lamps[j], position, normal);
    const float w = Luma(e.radiance);
    running += w;
    if (w > 0.0f && (running >= pick || j + 1u == count)) {
      chosen = e.radiance;
      to_chosen = e.to_lamp;
      distance = e.distance;
      weight = w;
    }
  }
  if (!(weight > 0.0f)) {
    return V3{};
  }
  const V3 origin = position + normal * Lift(position.z);
  return blocked(origin, to_chosen, std::max(distance - kLampEnd, 0.0f)) ? V3{} : chosen * (weight_sum / weight);
}

// R7: the world frame's lookup's lamps in the cursor's order (hlsl CityHitLamps with CITY_HIT_LAMP_GRID): the walk takes the first kLampMax lamps that REACH the hit (a lamp out of the game's reach
// adds nothing and takes no slot, so a dense street's far lamps do not push the near ones out) and stops after kLampWalkMax lamps passed, reaching or not; their light is summed and capped in luminance at
// kLampSumCap. `count` is the lookup's whole list: the walk that stopped with lamps left is a hit that hit the cap. `examined` is the lamps that reach (the census' word), `walked` the lamps passed.
struct LampLight {
  V3 light;
  uint32_t examined = 0, lit = 0, walked = 0;
  bool cap_count = false, cap_sum = false;
};
inline float LampKeep(float weight_sum) { return weight_sum > kLampSumCap ? kLampSumCap / weight_sum : 1.0f; }
inline LampLight LampsCapped(float lamp_factor, const Lamp* lamps, uint32_t count, V3 position, V3 normal) {
  LampLight t;
  float weight_sum = 0.0f;
  V3 sum;
  while (t.walked < count && t.examined < kLampMax && t.walked < kLampWalkMax) {
    const LampEval e = LampAt(lamp_factor, lamps[t.walked], position, normal);
    const float luma = Luma(e.radiance);
    sum = sum + e.radiance;
    weight_sum += luma;
    t.lit += luma > 0.0f ? 1u : 0u;
    t.examined += e.reaches ? 1u : 0u;
    ++t.walked;
  }
  t.cap_count = t.walked < count;
  t.cap_sum = weight_sum > kLampSumCap;
  t.light = sum * LampKeep(weight_sum);
  return t;
}
// The same with one lamp chosen by its unshadowed brightness and a shadow ray (as LampPick), times the cap's factor; the counters are LampsCapped's.
inline LampLight LampPickCapped(float lamp_factor, const Lamp* lamps, uint32_t count, V3 position, V3 normal, float noise, float view_z, const std::function<bool(V3, V3, float)>& blocked) {
  LampLight t = LampsCapped(lamp_factor, lamps, count, position, normal);
  float weight_sum = 0.0f;
  for (uint32_t i = 0; i < t.walked; ++i) weight_sum += Luma(LampAt(lamp_factor, lamps[i], position, normal).radiance);
  if (!(weight_sum > 0.0f)) {
    return t;
  }
  const float keep = LampKeep(weight_sum);
  const float pick = noise * weight_sum;
  float running = 0.0f;
  V3 chosen, to_chosen{0, 1, 0};
  float distance = 0.0f, weight = 0.0f;
  uint32_t seen = 0;  // the lamps that reach, replayed
  for (uint32_t j = 0; j < t.walked && weight <= 0.0f; ++j) {
    const LampEval e = LampAt(lamp_factor, lamps[j], position, normal);
    const float w = Luma(e.radiance);
    running += w;
    seen += e.reaches ? 1u : 0u;
    if (w > 0.0f && (running >= pick || seen == t.examined)) {
      chosen = e.radiance;
      to_chosen = e.to_lamp;
      distance = e.distance;
      weight = w;
    }
  }
  if (!(weight > 0.0f)) {
    t.light = V3{};
    return t;
  }
  const V3 origin = position + normal * Lift(view_z);
  t.light = blocked(origin, to_chosen, std::max(distance - kLampEnd, 0.0f)) ? V3{} : chosen * (keep * weight_sum / weight);
  return t;
}
// What a glow kind glows by when the includer gives no schedule: a traffic signal always, the rest by the params' lamp factor (hlsl CityHitGlowFactor); a kind that is not one reads as the night curve.
inline uint32_t GlowKindOf(uint32_t kind) { return kind == kGlowLens ? kGlowLens : (kind == kGlowAlways ? kGlowAlways : kGlowNight); }
inline float GlowFactorDefault(uint32_t kind, float lamp_factor) { return GlowKindOf(kind) == kGlowAlways ? 1.0f : Saturate(lamp_factor); }

// The GPU record: three rows of four words. `camera` is the camera's place in the game's world, `axes` the world's axes in view space (rt_hit::FrameInput::world_axes_view): a world
// point (p) is in the view at sum_k (p_k - camera_k) axes[k], a world direction likewise without the camera. `cone_cos_*` are lamp_table.h LampConeCosines (-2 for no cone).
struct WorldLamp {
  float position[3] = {};
  float colour[3] = {1, 1, 1};
  float axis[3] = {0, -1, 0};  // the direction the light points along (down for a street lamp)
  float intensity = 0;
  float cone_cos_outer = -2.0f;
  float cone_cos_inner = -2.0f;
};
inline void MakeGpuLamp(const WorldLamp& l, const float camera[3], const float axes[3][3], uint32_t rows[3][4]) {
  float position[3] = {}, axis[3] = {};
  for (int k = 0; k < 3; ++k) {
    for (int c = 0; c < 3; ++c) {
      position[c] += (l.position[k] - camera[k]) * axes[k][c];
      axis[c] += l.axis[k] * axes[k][c];
    }
  }
  const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  for (int c = 0; c < 3; ++c) {
    axis[c] = length > 1e-6f ? axis[c] / length : 0.0f;
  }
  for (int c = 0; c < 3; ++c) {
    rows[0][c] = rt_hit::Bits(position[c]);
    rows[1][c] = rt_hit::Bits(axis[c]);
    rows[2][c] = rt_hit::Bits(l.colour[c]);
  }
  rows[0][3] = rt_hit::Bits(l.intensity);
  rows[1][3] = rt_hit::Bits(l.cone_cos_outer);
  rows[2][3] = rt_hit::Bits(l.cone_cos_inner);
}
inline Lamp UnpackGpuLamp(const uint32_t rows[3][4]) {
  Lamp l;
  l.position = {rt_hit::FromBits(rows[0][0]), rt_hit::FromBits(rows[0][1]), rt_hit::FromBits(rows[0][2])};
  l.intensity = rt_hit::FromBits(rows[0][3]);
  l.axis = {rt_hit::FromBits(rows[1][0]), rt_hit::FromBits(rows[1][1]), rt_hit::FromBits(rows[1][2])};
  l.cos_outer = rt_hit::FromBits(rows[1][3]);
  l.colour = {rt_hit::FromBits(rows[2][0]), rt_hit::FromBits(rows[2][1]), rt_hit::FromBits(rows[2][2])};
  l.cos_inner = rt_hit::FromBits(rows[2][3]);
  return l;
}

// ---------------------------------------------------------------------------
// the surface
// ---------------------------------------------------------------------------

inline float Schlick(float f0, float nv) { return f0 + (1.0f - f0) * std::pow(std::max(1.0f - nv, 1e-4f), 5.0f); }
// The share of the sky's reflection a surface of this smoothness has: nothing for a rough wall, all for glass and a wet road.
inline float SpecWeight(float smoothness) {
  const float s = Saturate((smoothness - kSpecFrom) / (1.0f - kSpecFrom));
  return s * s;
}

// What the shading reads besides the hit data: the pieces a test can stand in for.
struct Reads {
  // the diffuse texture of the material at (slot, uv, level counted from the first resident level, wrap): rgb in linear units
  std::function<V3(uint32_t, float, float, float, bool)> texture;
  // the environment map at a view-space unit direction and a level
  std::function<V3(V3, float)> environment;
  // the sun ray and a lamp's ray: is anything in the way (origin, direction, reach)
  std::function<bool(V3, V3, float)> sun_blocked;
  std::function<bool(V3, V3, float)> lamp_blocked;
  // the game's cascades' visibility at a hit (view-space point, unit normal): 1 where the frame has none
  std::function<float(V3, V3)> cascade_visible;
  // the GI field's hooks: the ambient at a point given the default, and the feedback irradiance (null: none)
  std::function<V3(V3, V3, V3)> ambient;
  std::function<V3(V3, V3)> feedback;
  // the air between the camera and the hit on a shaded colour (the picture's units) at a distance in metres (null: none)
  std::function<V3(V3, float)> haze;
  // R7: the world frame's lamp light at a hit (view-space point, unit normal, the lamp shadow flag, the choice noise): the shader's CITY_HIT_LAMP_GRID path. Null: Shade's flat list.
  std::function<V3(V3, V3, bool, float)> lamps_at;
  // R7: what a glow kind (kGlow*) glows by, 0..1 (the world rows' row 47). Null: GlowFactorDefault.
  std::function<float(uint32_t)> glow_factor;
  // R7: the counters (the census words kCensus*) of the pass: called with (word, value); null: not counted
  std::function<void(uint32_t, uint32_t)> census;
};

struct Params {
  uint32_t mode = kModeReflect;
  uint32_t flags = 0;
  float units = 1.0f;
  float sun_reach = 0.0f;
  float sun_tan = 0.0f;
  float lamp_factor = 0.0f;
  float wet = 0.0f;
  float ao_mix = 0.5f;
  float lod_bias = 0.0f;
  float bounce = 0.0f;
  uint32_t lamp_first = 0;
  uint32_t lamp_count = 0;
  float noise[3] = {0.5f, 0.5f, 0.5f};
};

struct Ray {
  V3 origin, direction;
  float t = 0;
  float footprint = 0;
};

struct Instance {
  V3 row[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

struct Shaded {
  bool valid = false;
  uint32_t cls = 0;
  V3 colour, albedo, normal{0, 1, 0};
  float ndl = 0, visible = 1, glow = 0;
};

// The hit's unit normal in view space, turned to face the ray (hlsl CityHitViewNormal).
inline V3 ViewNormal(const float world_normal[3], const Instance& inst, V3 direction) {
  const V3 w{world_normal[0], world_normal[1], world_normal[2]};
  V3 n{Dot(inst.row[0], w), Dot(inst.row[1], w), Dot(inst.row[2], w)};
  const float l2 = Dot(n, n);
  n = l2 > 1e-30f ? n * (1.0f / std::sqrt(l2)) : V3{0, 1, 0};
  return Dot(n, direction) > 0.0f ? -n : n;
}

// The colour of a decoded hit in the picture's units (hlsl CityHitShadeHit). `texture_row` is the material's row of the texture table (null: none); `lamps` the frame's list.
inline Shaded Shade(const Params& p, const rt_hit::FrameInput& frame, const Ray& ray, const Instance& inst, const ch::DecodedHit& h, const ch::HitMaterial& m, const ch::GpuTextureRow* texture_row,
                    const Reads& reads, const Lamp* lamps) {
  Shaded r;
  if (!frame.tuning.enabled || !frame.light.valid || !h.valid) {
    return r;
  }
  const V3 hit = ray.origin + ray.direction * ray.t;
  const V3 normal = ViewNormal(h.normal, inst, ray.direction);
  const float nv = Saturate(-Dot(normal, ray.direction));
  const uint32_t cls = std::min(ch::WordClass(m.word), kClassCount - 1u);
  const float smoothness = float(ch::WordSmoothness(m.word)) * (1.0f / 255.0f);
  const float metal = float(ch::WordMetal(m.word)) * (1.0f / 255.0f);
  const V3 tint{m.tint[0], m.tint[1], m.tint[2]}, average{m.average[0], m.average[1], m.average[2]};

  // the albedo
  const bool has_uv = (h.run_flags & ch::kRunHasUv) != 0;
  const bool has_sector = (h.sector_flags & (ch::kSectorRowRuns | ch::kSectorRowCorners)) == (ch::kSectorRowRuns | ch::kSectorRowCorners);
  const uint32_t t_mips = texture_row ? texture_row->mips & 255u : 0u;
  const uint32_t t_first = texture_row ? (texture_row->mips >> 8) & 255u : 0u;
  const uint32_t t_width = texture_row ? texture_row->size & 0xFFFFu : 0u;
  const uint32_t t_height = texture_row ? texture_row->size >> 16 : 0u;
  const bool textured = p.mode == kModeReflect && texture_row && texture_row->slot != ch::kNoSlot && t_mips != 0 && has_uv && has_sector;
  V3 texel;
  float beyond = 0.0f;
  if (textured) {
    const float density = std::sqrt(h.uv_area / std::max(h.world_area, 1e-12f));
    const ResidentRead read = ResidentLevel(TextureLevel(ray.footprint, density, t_width, t_height, nv, p.lod_bias), t_first, t_mips);
    texel = TexelAlbedo(m.grey_in_red, reads.texture(texture_row->slot, h.uv[0], h.uv[1], read.level, true));
    beyond = read.average;
  }
  V3 albedo = Albedo(textured, texel, tint, average, beyond, (h.run_flags & ch::kRunHasColour) != 0, h.colour[3], p.ao_mix);
  if (cls == kClassRoad || cls == kClassWetRoad || cls == kClassSidewalk) {
    albedo = albedo * (1.0f - kWetDarken * Saturate(p.wet));
  }

  // the light
  const V3 up{frame.world_axes_view[1][0], frame.world_axes_view[1][1], frame.world_axes_view[1][2]};
  const float hemisphere = std::clamp(0.6f + 0.4f * Dot(normal, up), 0.2f, 1.0f);
  const V3 sun{frame.light.sun[0], frame.light.sun[1], frame.light.sun[2]};
  const V3 ambient_colour{frame.light.ambient[0], frame.light.ambient[1], frame.light.ambient[2]};
  const V3 fill{std::max(ambient_colour.x, sun.x * rt_hit::kFillShare), std::max(ambient_colour.y, sun.y * rt_hit::kFillShare), std::max(ambient_colour.z, sun.z * rt_hit::kFillShare)};
  V3 light = fill * (frame.tuning.ambient * hemisphere);
  if (reads.ambient) {
    light = reads.ambient(hit, normal, light);
  }
  const V3 to_sun = frame.sun_known ? V3{frame.to_sun_view[0], frame.to_sun_view[1], frame.to_sun_view[2]} : up;
  const float ndl = Saturate(Dot(normal, to_sun));
  float visible = 1.0f;
  if (ndl > 0.0f) {
    if ((p.flags & kPSunCascade) != 0 && frame.tuning.cascade && rt_hit::CascadesUsable(frame.cascades) && frame.camera_known && reads.cascade_visible) {
      visible = reads.cascade_visible(hit, normal);
    }
    if (visible > 0.0f && (p.flags & kPSunRay) != 0) {
      const V3 origin = hit + normal * Lift(hit.z);
      const V3 direction = SunDirection(to_sun, normal, p.sun_tan, p.noise[0], p.noise[1]);
      const float reach = p.sun_reach > 0.0f ? p.sun_reach : kSunReachDefault;
      visible = std::min(visible, reads.sun_blocked(origin, direction, reach) ? 0.0f : 1.0f);
    }
  }
  light = light + sun * (ndl * visible);
  if (reads.lamps_at) {
    if ((p.flags & kPLamps) != 0 && p.lamp_factor > 0.0f) {
      light = light + reads.lamps_at(hit, normal, (p.flags & kPLampShadow) != 0, p.noise[2]);
    }
  } else if (lamps && (p.flags & kPLamps) != 0 && p.lamp_count != 0 && p.lamp_factor > 0.0f) {
    light = light + ((p.flags & kPLampShadow) != 0 ? LampPick(p.lamp_factor, lamps + p.lamp_first, p.lamp_count, hit, normal, p.noise[2], hit.z, reads.lamp_blocked)
                                                    : LampsSum(p.lamp_factor, lamps + p.lamp_first, p.lamp_count, hit, normal));
  }
  if (p.mode == kModeGi && p.bounce > 0.0f && reads.feedback) {
    light = light + reads.feedback(hit, normal) * p.bounce;
  }

  // the surface
  V3 result = albedo * light * (kDiffuseGain[cls] * (1.0f - metal));
  if (p.mode == kModeReflect) {
    const float weight = SpecWeight(smoothness) * kSpecGain[cls];
    if (weight > 0.0f) {
      const V3 reflected = ray.direction - normal * (2.0f * Dot(normal, ray.direction));
      const float lod = (1.0f - smoothness) * kEnvLodSmooth + std::log2(1.0f + rt_hit::kConeLodScale * ray.footprint / std::max(2.0f * ray.t, 1e-3f));
      V3 env = ambient_colour * (0.6f + 0.4f * Dot(reflected, up));
      if (frame.light.env_sharp != 0) {
        env = reads.environment(reflected, lod);
      }
      const V3 f0 = Lerp(V3{kDielectricF0, kDielectricF0, kDielectricF0}, albedo, metal);
      const V3 fresnel{Schlick(f0.x, nv), Schlick(f0.y, nv), Schlick(f0.z, nv)};
      const V3 keep{1.0f - weight * fresnel.x, 1.0f - weight * fresnel.y, 1.0f - weight * fresnel.z};
      result = result * keep + env * fresnel * (weight * frame.tuning.spec);
    }
  }
  float glow = 0.0f;
  if ((p.flags & kPEmissive) != 0 && m.emissive > 0.0f) {
    const uint32_t kind = GlowKindOf(m.glow_kind);
    glow = Saturate(reads.glow_factor ? reads.glow_factor(kind) : GlowFactorDefault(kind, p.lamp_factor));
    result = result + albedo * (m.emissive * rt_hit::kEmissiveGain * glow);
    if (reads.census && (p.flags & kPCalibrate) != 0) reads.census(kCensusGlowNight + kind, 1u);
  }
  V3 shaded = Held3(result * p.units, kHitMaxColour);   // (hlsl: a NaN black, an infinity the cap: it would make a NaN of the blend with the picture's colour)
  if (reads.haze) {
    shaded = reads.haze(shaded, Length(hit));
  }
  r.valid = true;
  r.cls = cls;
  r.colour = shaded;
  r.albedo = albedo;
  r.normal = normal;
  r.ndl = ndl;
  r.visible = visible;
  r.glow = glow;
  return r;
}

// ---------------------------------------------------------------------------
// ResolveHit
// ---------------------------------------------------------------------------

struct Screen {
  float tan_x = 1, tan_y = 1;  // tan of half the horizontal and the vertical field of view
  float width = 1920, height = 1080;
  float tolerance_m = 0.05f, tolerance_rel = 0.01f, edge_band = 0.04f, min_facing = 0.05f;
  float cap = 0.0f;
};
struct ScreenAt {
  float weight = 0;
  float uv[2] = {0, 0};
  float pixel = 1;
};
// How much of the hit the picture can speak for (hlsl CityHitScreenWeight). `depth_at(x, y)` is the picture's linear view depth at a pixel; `rejected(x, y)` a pixel that must not be the answer.
inline ScreenAt ScreenWeight(const Screen& s, V3 hit, V3 normal, const std::function<float(int, int)>& depth_at, const std::function<bool(int, int)>& rejected) {
  ScreenAt a;
  a.pixel = 1.0f;
  if (!(hit.z > kMinDepth)) {
    return a;
  }
  const float ndc_x = hit.x / (hit.z * s.tan_x), ndc_y = hit.y / (hit.z * s.tan_y);
  const float edge = std::max(std::max(std::fabs(ndc_x), std::fabs(ndc_y)), 0.0f);
  if (!(edge < 1.0f)) {
    return a;
  }
  a.uv[0] = ndc_x * 0.5f + 0.5f;
  a.uv[1] = 0.5f - ndc_y * 0.5f;
  a.pixel = 2.0f * s.tan_y * hit.z / std::max(s.height, 1.0f);
  const int tx = std::min(int(a.uv[0] * s.width), int(s.width) - 1), ty = std::min(int(a.uv[1] * s.height), int(s.height) - 1);
  if (rejected && rejected(tx, ty)) {
    return a;
  }
  const float depth = depth_at(tx, ty);
  const float facing = Dot(normal, -Normalize(hit));
  const float slope = std::sqrt(std::max(1.0f - facing * facing, 0.0f)) / std::max(facing, 0.2f);
  const float tolerance = s.tolerance_m + s.tolerance_rel * hit.z + a.pixel * slope;
  const float edge_weight = Saturate((1.0f - edge) / std::max(s.edge_band, 1e-4f));
  const float depth_weight = 1.0f - Smoothstep(tolerance, 2.0f * tolerance, std::fabs(depth - hit.z));
  const float face_weight = Saturate((facing - s.min_facing) / kFacingSpan);
  a.weight = edge_weight * depth_weight * face_weight;
  return a;
}
// The mip level of the picture's colour for a cone: log2 of the footprint in pixels, at least 0.
inline float ScreenMip(float footprint, float pixel) { return std::log2(std::max(footprint / std::max(pixel, 1e-6f), 1.0f)); }
// GI caps the picture's luminance (a lamp or a neon sign is a bounded source).
inline V3 CapLuma(V3 c, float cap) {
  if (!(cap > 0.0f)) {
    return c;
  }
  const float luma = Luma(c);
  return luma > cap ? c * (cap / luma) : c;
}

struct Resolved {
  uint32_t source = kSourceNone;
  uint32_t cls = 0;
  V3 colour;
  float screen = 0;
};
// The decision of the resolve given the picture's weight and colour and the shaded result (hlsl CityHitResolve's tail): `gain` the class's. A shaded.valid false with no picture weight is "none".
inline Resolved Mix(float weight, V3 seen, const Shaded& shaded, float gain) {
  Resolved r;
  r.cls = shaded.cls;
  r.screen = weight;
  if (!shaded.valid && weight <= 0.0f) {
    return r;
  }
  const V3 model = shaded.colour * gain;
  if (!shaded.valid || weight >= 1.0f) {
    r.colour = seen;
    r.source = kSourceScreen;
  } else if (weight <= 0.0f) {
    r.colour = model;
    r.source = kSourceShaded;
  } else {
    r.colour = Lerp(model, seen, weight);
    r.source = kSourceBlend;
  }
  return r;
}
// Is a hit a calibration's sample: the picture speaks for it in full and both luminances are over the floor.
inline bool IsCalibrationSample(float weight, float seen_luma, float model_luma) { return weight >= kScreenTrust && seen_luma > kCalFloor && model_luma > kCalFloor; }

}  // namespace mcla::gpu::city_hit_shade
