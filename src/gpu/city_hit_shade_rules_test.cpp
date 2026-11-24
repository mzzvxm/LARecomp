// Check of the city hit library (CITY-RT M2 package H4): hlsl/city_hit_shade.hlsli and its C++ twin src/gpu/city_hit_shade_rules.h. A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -I src -I src/gpu src/gpu/city_hit_shade_rules_test.cpp src/citypack/city_hit_format.cpp src/citypack/lamp_merge.cpp src/citypack/lamp_table.cpp src/citypack/pack_format.cpp \
//           -o city_hit_shade_rules_test
//   city_hit_shade_rules_test <src dir>
//
// It checks the twin's own claims (the texture's level, the laws of the lamps and the estimator of the lamp choice, the sun ray's direction, the fresnel, the screen's weight, the mix, the lamp's
// GPU record), then compiles the shader library ITSELF as C++ (hlsl_cpp_shim.h: the HLSL types, the pool and the tables, a fake texture set, the ray queries replaced by deterministic fakes) and,
// on a synthetic city (a wall, a road, glass, a lamp lens, metal, a wet road, water, a neon sign, a lit window; sectors with and without corners, runs, a texture that is not resident, a sector that
// is not built) and thousands of random rays, hits, frames and parameters, compares CityHitShade and CityHitResolve with the twin: the colour, the albedo, the normal, the sun's visibility, the glow,
// the source, the rays that were cast (their origins, directions and lengths), the calibration's counters. The twin decodes with DecodeHit (src/citypack/city_hit_format.h), the shader with
// CityHitDecode: the decode is checked against the twin as well.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "city_hit_shade_rules.h"
#include "city_refl_rules.h"  // REFL-LAMPS: the reflection passes' lamp term in front of the library (ReflectionGlueChecks)
#include "gbuffer_types.h"
#include "hlsl_cpp_shim.h"
#include "rt_hit_calibration.h"
#include "rt_hit_frame.h"
#include "rt_hit_sample_ref.h"
#include "rt_world_frame_rules.h"

using namespace larecomp::citypack;
namespace cs = mcla::gpu::city_hit_shade;
namespace rh = mcla::gpu::rt_hit;
namespace hc = mcla::gpu::rt_hit_cal;
namespace gb = mcla::gpu::gbuffer;
namespace rw = mcla::gpu::rt_world;

// ---------------------------------------------------------------------------
// the shader library as C++: the shim, the resources, the fakes of what a GPU would do
// ---------------------------------------------------------------------------
namespace hlsl_shim {
ByteAddressBuffer gCityPool;
StructuredBuffer<uint4> gCitySectors;
StructuredBuffer<uint4> gCityMaterials;
StructuredBuffer<uint4> gCityTextures;
StructuredBuffer<uint4> g_frame;
StructuredBuffer<uint4> g_lamps;
TextureArray g_textures;

// what the fakes answer
struct RayCall {
  float origin[3], direction[3], reach;
};
std::vector<RayCall> gSunCalls, gLampCalls;
float gScreenDepth = 10.0f;   // the picture's depth everywhere
int gScreenRejectMod = 0;     // a pixel whose x is a multiple of this is rejected (0: none)
float gGain[16];
std::vector<uint32_t> gStats(48, 0);
int gAmbientCalls = 0, gFeedbackCalls = 0;

// the air: the colour fades to a pale blue with the distance
inline float3 HazeFake(float3 colour, float distance) {
  const float keep = std::exp(-0.004f * distance);
  return colour * keep + float3(0.3f, 0.35f, 0.4f) * (1.0f - keep);
}
inline bool FakeBlocked(float ox, float oy, float oz, float dx, float dy, float dz) {
  const float v = ox * 0.37f + oy * 0.61f + oz * 0.29f + dx * 1.7f + dy * 2.3f + dz * 3.1f;
  return (v - std::floor(v)) < 0.4f;
}
}  // namespace hlsl_shim

#define CITY_HIT_NO_RESOURCES
#define CITY_HIT_SHADE_NO_RAYQUERY
#define RT_HIT_NO_RAYQUERY
#define RT_HIT_UNROLL
#define CITY_HIT_BRANCH
#define CITY_HIT_LOOP
#define RT_HIT_FRAME_ROW(i) hlsl_shim::g_frame[i]
#define CITY_HIT_SCREEN_DEPTH(texel) hlsl_shim::gScreenDepth
#define CITY_HIT_SCREEN_COLOUR(uv, mip) hlsl_shim::float3((uv).x, (uv).y, 0.2f + 0.1f * (mip))
#define CITY_HIT_SCREEN_REJECT(texel) (hlsl_shim::gScreenRejectMod != 0 && (texel).x % hlsl_shim::gScreenRejectMod == 0 ? 1 : 0)
#define CITY_HIT_GAIN(cls) hlsl_shim::gGain[cls]
#define CITY_HIT_STAT_ADD(word, value) (hlsl_shim::gStats[(word)] += (value))
#define CITY_HIT_LAMP_LOAD(i) CityHitLampFromRows(hlsl_shim::g_lamps[(i) * 3u], hlsl_shim::g_lamps[(i) * 3u + 1u], hlsl_shim::g_lamps[(i) * 3u + 2u])
#define CITY_HIT_AMBIENT(P, N, ambient) (++hlsl_shim::gAmbientCalls, (ambient) * (1.0f + 0.05f * (P).x) + hlsl_shim::float3(0.01f, 0.02f, 0.03f) * (N).y)
#define CITY_HIT_FEEDBACK(P, N) (++hlsl_shim::gFeedbackCalls, hlsl_shim::float3(0.1f + 0.01f * (P).y, 0.2f, 0.05f * (N).x + 0.1f))
#define CITY_HIT_HAZE(colour, distance) hlsl_shim::HazeFake((colour), (distance))

namespace hlsl_shim {
namespace flat {  // R7: a namespace of its own (a using-directive below keeps every hlsl_shim:: name of this test), so that the second inclusion in hlsl_shim::grid is not found by argument dependent lookup
#include "hlsl/city_hit_common.hlsli"
#include "hlsl/rt_hit_common.hlsli"
inline bool CityHitSunRay(float3 origin, float3 direction, float reach, float footprint, float sunTan) {
  (void)footprint;
  (void)sunTan;
  gSunCalls.push_back({{origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, reach});
  return FakeBlocked(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
}
inline bool CityHitLampRay(float3 origin, float3 direction, float reach, float footprint) {
  (void)footprint;
  gLampCalls.push_back({{origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, reach});
  return FakeBlocked(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
}
#include "hlsl/city_hit_shade.hlsli"
}  // namespace flat
using namespace flat;
}  // namespace hlsl_shim

// R7: the same library again, in a namespace of its own, with the world frame's grid for its lamps and the world rows' schedule for its glow (city_hit_lamp_term_lib.inc; the checks are in
// city_hit_lamp_term_checks.inc)
#include "city_hit_lamp_term_lib.inc"

namespace {

int g_checks = 0;
int g_failed = 0;
void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond)
bool Near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }
std::string Num(const char* format, double a) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a);
  return text;
}
std::string Num2(const char* format, double a, double b) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a, b);
  return text;
}

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : (U32() >> 8) % n; }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

cs::V3 V(const hlsl_shim::float3& v) { return {v.x, v.y, v.z}; }
hlsl_shim::float3 F3(const cs::V3& v) { return hlsl_shim::float3(v.x, v.y, v.z); }

// ---------------------------------------------------------------------------
// the twin's own claims
// ---------------------------------------------------------------------------

void NumbersChecks() {
  // the shader's numbers are the twin's
  CHECK(CITY_HIT_MODE_REFLECT == cs::kModeReflect && CITY_HIT_MODE_GI == cs::kModeGi);
  CHECK(CITY_HIT_P_SUN_RAY == cs::kPSunRay && CITY_HIT_P_SUN_CASCADE == cs::kPSunCascade && CITY_HIT_P_LAMPS == cs::kPLamps && CITY_HIT_P_LAMP_SHADOW == cs::kPLampShadow &&
        CITY_HIT_P_EMISSIVE == cs::kPEmissive && CITY_HIT_P_CALIBRATE == cs::kPCalibrate && CITY_HIT_P_SCREEN == cs::kPScreen);
  CHECK(CITY_HIT_SOURCE_NONE == cs::kSourceNone && CITY_HIT_SOURCE_SCREEN == cs::kSourceScreen && CITY_HIT_SOURCE_SHADED == cs::kSourceShaded && CITY_HIT_SOURCE_BLEND == cs::kSourceBlend);
  CHECK(CITY_HIT_CLASS_COUNT == cs::kClassCount && CITY_HIT_CLASS_ROAD == cs::kClassRoad && CITY_HIT_CLASS_WET_ROAD == cs::kClassWetRoad && CITY_HIT_CLASS_SIDEWALK == cs::kClassSidewalk);
  CHECK(Near(hlsl_shim::kCityHitGrazingMin, cs::kGrazingMin) && Near(hlsl_shim::kCityHitSpecFrom, cs::kSpecFrom) && Near(hlsl_shim::kCityHitEnvLodSmooth, cs::kEnvLodSmooth) &&
        Near(hlsl_shim::kCityHitWetDarken, cs::kWetDarken) && Near(hlsl_shim::kCityHitDielectricF0, cs::kDielectricF0) && Near(hlsl_shim::kCityHitLiftFloor, cs::kLiftFloor) &&
        Near(hlsl_shim::kCityHitLiftPerMetre, cs::kLiftPerMetre) && Near(hlsl_shim::kCityHitLiftCap, cs::kLiftCap) && Near(hlsl_shim::kCityHitSunReach, cs::kSunReachDefault) &&
        Near(hlsl_shim::kCityHitMinAbove, cs::kMinAbove) && Near(hlsl_shim::kCityHitLampReach, cs::kLampReach) && Near(hlsl_shim::kCityHitLampEnd, cs::kLampEnd) &&
        Near(hlsl_shim::kCityHitMinDepth, cs::kMinDepth) && Near(hlsl_shim::kCityHitScreenTrust, cs::kScreenTrust) && Near(hlsl_shim::kCityHitCalFloor, cs::kCalFloor));
  CHECK(Near(hlsl_shim::kCityHitLogFloor, hc::kLogFloor) && Near(hlsl_shim::kCityHitLogBias, hc::kLogBias) && Near(hlsl_shim::kCityHitLogScale, hc::kLogScale) && Near(hlsl_shim::kCityHitLogMax, hc::kLogMax));
  bool tables = true;
  for (uint32_t c = 0; c < cs::kClassCount; ++c) tables = tables && Near(hlsl_shim::kCityHitDiffuseGain[c], cs::kDiffuseGain[c]) && Near(hlsl_shim::kCityHitSpecGain[c], cs::kSpecGain[c]);
  CHECK(tables);
  // the G-buffer's classes (gbuffer_types.h) are the library's: a hit's class means what the raster's does
  CHECK(uint32_t(gb::SurfaceClass::kNone) == CITY_HIT_CLASS_NONE && uint32_t(gb::SurfaceClass::kWorld) == CITY_HIT_CLASS_WORLD && uint32_t(gb::SurfaceClass::kRoad) == CITY_HIT_CLASS_ROAD &&
        uint32_t(gb::SurfaceClass::kWetRoad) == CITY_HIT_CLASS_WET_ROAD && uint32_t(gb::SurfaceClass::kSidewalk) == CITY_HIT_CLASS_SIDEWALK && uint32_t(gb::SurfaceClass::kTerrain) == CITY_HIT_CLASS_TERRAIN &&
        uint32_t(gb::SurfaceClass::kLod) == CITY_HIT_CLASS_LOD && uint32_t(gb::SurfaceClass::kFoliage) == CITY_HIT_CLASS_FOLIAGE && uint32_t(gb::SurfaceClass::kPaint) == CITY_HIT_CLASS_PAINT &&
        uint32_t(gb::SurfaceClass::kChrome) == CITY_HIT_CLASS_CHROME && uint32_t(gb::SurfaceClass::kMetal) == CITY_HIT_CLASS_METAL && uint32_t(gb::SurfaceClass::kGlass) == CITY_HIT_CLASS_GLASS &&
        uint32_t(gb::SurfaceClass::kSkin) == CITY_HIT_CLASS_SKIN && uint32_t(gb::SurfaceClass::kEmissive) == CITY_HIT_CLASS_EMISSIVE && uint32_t(gb::SurfaceClass::kInterior) == CITY_HIT_CLASS_INTERIOR &&
        uint32_t(gb::SurfaceClass::kWater) == CITY_HIT_CLASS_WATER);
}

void TwinChecks() {
  Lcg rng;
  // the texture's level: log2 of the texels the cone covers, a grazing view covers more (not below kGrazingMin), the bias
  CHECK(Near(cs::TextureLevel(1.0f, 1.0f, 64, 64, 1.0f, 0.0f), 6.0, 1e-5));  // 1 m of 1 repeat a metre of a 64 texel texture: 64 texels: level 6
  CHECK(Near(cs::TextureLevel(0.25f, 1.0f, 64, 64, 1.0f, 0.0f), 4.0, 1e-5) && Near(cs::TextureLevel(1.0f, 4.0f, 64, 64, 1.0f, 0.0f), 8.0, 1e-5));
  CHECK(Near(cs::TextureLevel(1.0f, 1.0f, 64, 256, 1.0f, 0.0f), 7.0, 1e-5));  // the geometric mean of the sides: 128
  CHECK(Near(cs::TextureLevel(1.0f, 1.0f, 64, 64, 0.5f, 0.0f), 7.0, 1e-5) && Near(cs::TextureLevel(1.0f, 1.0f, 64, 64, 0.1f, 0.0f), 6.0 + std::log2(1.0 / 0.35), 1e-5));
  CHECK(Near(cs::TextureLevel(1.0f, 1.0f, 64, 64, 1.0f, 1.5f), 7.5, 1e-5) && std::isfinite(cs::TextureLevel(0.0f, 0.0f, 64, 64, 1.0f, 0.0f)));
  float previous = -100.0f;
  for (int i = 0; i < 100; ++i) {
    const float level = cs::TextureLevel(0.001f * std::pow(1.2f, float(i)), 0.7f, 128, 128, 0.8f, 0.0f);
    CHECK(level > previous);
    previous = level;
  }
  // the level in the resident chain and the share of the average colour beyond it
  const cs::ResidentRead a = cs::ResidentLevel(3.0f, 0, 7), b = cs::ResidentLevel(3.0f, 1, 7), c = cs::ResidentLevel(8.5f, 1, 5), d = cs::ResidentLevel(5.0f, 1, 5);
  CHECK(Near(a.level, 3.0) && Near(a.average, 0.0) && Near(b.level, 2.0) && Near(b.average, 0.0));
  CHECK(Near(c.level, 7.5) && Near(c.average, 1.0) && Near(d.level, 4.0) && Near(d.average, 0.0));
  CHECK(Near(cs::ResidentLevel(5.4f, 1, 5).average, 0.4, 1e-5));

  // the albedo: the texture times the tint, fading to the average; the occlusion
  const cs::V3 tint{0.5f, 1.0f, 2.0f}, average{0.2f, 0.4f, 0.6f}, texel{0.8f, 0.6f, 0.1f};
  cs::V3 al = cs::Albedo(true, texel, tint, average, 0.0f, false, 1.0f, 0.5f);
  CHECK(Near(al.x, 0.4) && Near(al.y, 0.6) && Near(al.z, 0.2));
  al = cs::Albedo(true, texel, tint, average, 1.0f, false, 1.0f, 0.5f);
  CHECK(Near(al.x, 0.2) && Near(al.y, 0.4) && Near(al.z, 0.6));
  al = cs::Albedo(false, texel, tint, average, 0.0f, false, 1.0f, 0.5f);
  CHECK(Near(al.x, 0.2) && Near(al.z, 0.6));
  al = cs::Albedo(false, texel, tint, average, 0.0f, true, 0.2f, 0.5f);  // occlusion 0.2 mixed by 0.5: x 0.6
  CHECK(Near(al.x, 0.2 * 0.6) && Near(al.y, 0.4 * 0.6));
  al = cs::Albedo(false, texel, tint, average, 0.0f, true, 0.2f, 0.0f);
  CHECK(Near(al.x, 0.2));
  // RT-GREEN-BAND: a road's diffuse is (grey, specular mask, nothing): its albedo is the red channel as a grey, not the colour (a road texel (0.08, 0.5, 0.08) is a grey road, not a green one)
  const cs::V3 road_texel{0.08f, 0.5f, 0.02f};
  const cs::V3 grey_texel = cs::TexelAlbedo(true, road_texel), plain_texel = cs::TexelAlbedo(false, road_texel);
  CHECK(Near(grey_texel.x, 0.08) && Near(grey_texel.y, 0.08) && Near(grey_texel.z, 0.08));
  CHECK(Near(plain_texel.x, 0.08) && Near(plain_texel.y, 0.5) && Near(plain_texel.z, 0.02));
  al = cs::Albedo(true, grey_texel, {1.0f, 1.0f, 1.0f}, average, 0.0f, false, 1.0f, 0.0f);  // the packed road lit as a grey: R = G = B, whatever the green channel holds
  CHECK(Near(al.x, 0.08) && Near(al.y, 0.08) && Near(al.z, 0.08));
  al = cs::Albedo(true, grey_texel, tint, average, 0.0f, false, 1.0f, 0.0f);  // ... times the tint
  CHECK(Near(al.x, 0.08 * 0.5) && Near(al.y, 0.08 * 1.0) && Near(al.z, 0.08 * 2.0));

  // the fresnel and the sky's share
  CHECK(Near(cs::Schlick(0.04f, 1.0f), 0.04) && Near(cs::Schlick(0.04f, 0.0f), 1.0, 1e-3) && Near(cs::Schlick(0.04f, 0.5f), 0.04 + 0.96 / 32.0, 1e-5));
  CHECK(Near(cs::SpecWeight(0.0f), 0.0) && Near(cs::SpecWeight(0.3f), 0.0) && Near(cs::SpecWeight(1.0f), 1.0) && cs::SpecWeight(0.8f) > cs::SpecWeight(0.6f) && Near(cs::SpecWeight(0.65f), 0.25, 1e-5));

  // the sun ray: sharp where the disc has no size, on the disc and above the surface otherwise
  const cs::V3 sun = cs::Normalize({0.3f, 0.8f, -0.2f}), normal = cs::Normalize({0.1f, 1.0f, 0.2f});
  const cs::V3 sharp = cs::SunDirection(sun, normal, 0.0f, 0.3f, 0.7f);
  CHECK(Near(sharp.x, sun.x) && Near(sharp.y, sun.y) && Near(sharp.z, sun.z));
  double worst_angle = 0.0;
  bool above = true;
  for (int i = 0; i < 4000; ++i) {
    const cs::V3 dir = cs::SunDirection(sun, normal, 0.0047f, rng.Next(), rng.Next());
    CHECK(Near(cs::Length(dir), 1.0, 1e-4));
    worst_angle = std::max(worst_angle, std::asin(double(std::min(1.0f, cs::Length(cs::Cross(dir, sun))))));  // the angle by the cross product: acos of a float near 1 is coarse
    above = above && cs::Dot(dir, normal) >= cs::kMinAbove * 0.95f;
  }
  Check(worst_angle < 0.0047 * 1.0005 && worst_angle > 0.0040, Num("the jittered sun ray stays on the disc (the widest angle %.5f rad of 0.0047)", worst_angle));
  CHECK(above);
  const cs::V3 grazing = cs::Normalize({0.0f, 1.0f, 0.0f}), low_sun = cs::Normalize({1.0f, 0.02f, 0.0f});
  for (int i = 0; i < 200; ++i) CHECK(cs::Dot(cs::SunDirection(low_sun, grazing, 0.0047f, rng.Next(), rng.Next()), grazing) >= cs::kMinAbove * 0.95f);  // pushed up to the limit (the shadow pass's rule: normalised after, so a hair under)
  // the disc's samples cover it evenly: the mean radius squared of the area sample is 1/2
  double radius2 = 0.0;
  for (int i = 0; i < 20000; ++i) {
    float s[2];
    cs::DiscSample(rng.Next(), rng.Next(), s);
    radius2 += s[0] * s[0] + s[1] * s[1];
  }
  Check(std::fabs(radius2 / 20000.0 - 0.5) < 0.02, Num("the disc sample is area uniform (mean r^2 %.3f)", radius2 / 20000.0));
  CHECK(Near(cs::Lift(0.0f), 0.006) && Near(cs::Lift(10.0f), 0.036) && Near(cs::Lift(1000.0f), 0.4));
  // a hit behind the camera has a negative view z: the lift follows its magnitude (a negative lift puts the ray's origin under the surface and the hit in its own shadow), in the library too
  CHECK(Near(cs::Lift(-10.0f), 0.036) && Near(cs::Lift(-1000.0f), 0.4) && Near(hlsl_shim::CityHitLift(-10.0f), 0.036) && Near(hlsl_shim::CityHitLift(-1000.0f), 0.4) && hlsl_shim::CityHitLift(-50.0f) > 0.0f);

  // the lamps: the game's laws
  CHECK(Near(cs::LampTerm(20.0f, 10.0f), 1.0) && Near(cs::LampTerm(20.0f, 20.0f), 1.0) && Near(cs::LampTerm(20.0f, 40.0f), 0.5) && Near(cs::LampTerm(55.0f, 5500.0f), 0.01));
  cs::Lamp lamp;
  lamp.position = {0, 10, 0};
  lamp.intensity = 20.0f;
  lamp.axis = {0, -1, 0};
  lamp.cos_outer = std::cos(70.0f * 3.14159265f / 180.0f);
  lamp.cos_inner = std::cos(55.0f * 3.14159265f / 180.0f);
  // directly under the lamp: to_lamp is +y (from the point to the lamp), the axis points down: full
  CHECK(Near(cs::LampCone(lamp, {0, 1, 0}), 1.0));
  cs::V3 to_side = cs::Normalize({1.0f, 1.0f, 0.0f});  // 45 degrees off the axis: inside the inner cone
  CHECK(Near(cs::LampCone(lamp, to_side), 1.0));
  to_side = cs::Normalize({1.0f, 0.2f, 0.0f});  // about 79 degrees: outside the outer
  CHECK(Near(cs::LampCone(lamp, to_side), 0.0));
  const float mid = 0.5f * (lamp.cos_outer + lamp.cos_inner);  // the ramp is linear in the cosine
  CHECK(Near(cs::LampCone(lamp, {std::sqrt(1 - mid * mid), mid, 0}), 0.5, 1e-3));  // half way in the ramp
  lamp.cos_outer = -2.0f;
  CHECK(Near(cs::LampCone(lamp, {1, 0, 0}), 1.0));  // an omni light
  // the term at a point: colour x term x cone x N.L x the schedule, nothing beyond sqrt(100 I)
  lamp.colour = {1.0f, 0.5f, 0.25f};
  const cs::LampEval near_eval = cs::LampAt(0.8f, lamp, {0, 0, 0}, {0, 1, 0});  // 10 m below: d2 = 100 > 20: term 0.2
  CHECK(Near(near_eval.radiance.x, 0.2 * 0.8) && Near(near_eval.radiance.y, 0.1 * 0.8) && Near(near_eval.distance, 10.0));
  const cs::LampEval facing_away = cs::LampAt(1.0f, lamp, {0, 0, 0}, {0, -1, 0});
  CHECK(Near(facing_away.radiance.x, 0.0));
  const cs::LampEval far_eval = cs::LampAt(1.0f, lamp, {0, -50, 0}, {0, 1, 0});  // 60 m: beyond 44.7 m
  CHECK(Near(far_eval.radiance.x, 0.0));
  const cs::LampEval reach_eval = cs::LampAt(1.0f, lamp, {0, 10.0f - 44.0f, 0}, {0, 1, 0});  // 44 m: just inside, term 20 / 1936
  CHECK(Near(reach_eval.radiance.x, 20.0 / 1936.0, 1e-5));

  // the lamp choice: its expectation over the noise is the sum of the lit lamps' light
  std::vector<cs::Lamp> lamps(7);
  for (cs::Lamp& l : lamps) {
    l.position = {rng.Range(-20, 20), rng.Range(4, 12), rng.Range(-20, 20)};
    l.intensity = rng.Range(20, 70);
    l.colour = {rng.Next(), rng.Next(), rng.Next()};
    l.cos_outer = rng.Below(2) ? -2.0f : std::cos(rng.Range(60, 80) * 3.14159265f / 180.0f);
    l.cos_inner = l.cos_outer < -1.5f ? -2.0f : l.cos_outer + 0.2f;
  }
  const cs::V3 position{1.0f, 0.0f, 2.0f}, n = cs::Normalize({0.2f, 1.0f, 0.1f});
  const auto lit_by_one = [&](cs::V3 origin, cs::V3 dir, float reach) {
    (void)origin;
    (void)reach;
    return dir.x > 0.3f;  // a lamp whose direction has x over 0.3 is hidden
  };
  const cs::V3 plain_sum = cs::LampsSum(0.9f, lamps.data(), uint32_t(lamps.size()), position, n);
  cs::V3 expected;
  for (const cs::Lamp& l : lamps) {
    const cs::LampEval e = cs::LampAt(0.9f, l, position, n);
    if (!lit_by_one(position, e.to_lamp, e.distance)) expected = expected + e.radiance;
  }
  cs::V3 mean;
  const int samples = 20000;
  for (int i = 0; i < samples; ++i) {
    const float noise = (float(i) + 0.5f) / float(samples);  // the noise's whole range, evenly
    mean = mean + cs::LampPick(0.9f, lamps.data(), uint32_t(lamps.size()), position, n, noise, 0.0f, lit_by_one);
  }
  mean = mean * (1.0f / float(samples));
  Check(Near(mean.x, expected.x, 3e-3) && Near(mean.y, expected.y, 3e-3) && Near(mean.z, expected.z, 3e-3),
        Num2("the one lamp's estimate averages to the sum of the lit lamps' light (%.4f against %.4f)", mean.x, expected.x));
  cs::V3 all_lit;
  for (int i = 0; i < samples; ++i) all_lit = all_lit + cs::LampPick(0.9f, lamps.data(), uint32_t(lamps.size()), position, n, (float(i) + 0.5f) / float(samples), 0.0f, [](cs::V3, cs::V3, float) { return false; });
  all_lit = all_lit * (1.0f / float(samples));
  Check(Near(all_lit.x, plain_sum.x, 3e-3) && Near(all_lit.y, plain_sum.y, 3e-3), Num2("with no shadow the estimate averages to the plain sum (%.4f against %.4f)", all_lit.x, plain_sum.x));
  // by day (the schedule 0) there is no light and no pick
  CHECK(cs::Luma(cs::LampsSum(0.0f, lamps.data(), 7, position, n)) == 0.0f && cs::Luma(cs::LampPick(0.0f, lamps.data(), 7, position, n, 0.5f, 0.0f, lit_by_one)) == 0.0f);
  // the shadow ray stops short of the lamp
  float seen_reach = -1.0f;
  cs::LampPick(1.0f, lamps.data(), 1, position, n, 0.5f, 5.0f, [&](cs::V3, cs::V3, float reach) {
    seen_reach = reach;
    return false;
  });
  const cs::LampEval one = cs::LampAt(1.0f, lamps[0], position, n);
  CHECK(cs::Luma(one.radiance) == 0.0f || Near(seen_reach, std::max(one.distance - cs::kLampEnd, 0.0f)));

  // the lamp's GPU record: the world's lamp in the frame's view space
  float axes[3][3];
  const float yaw = 0.7f, pitch = -0.3f;  // an orthonormal frame: the world's axes as the camera sees them
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  const float rows_[3][3] = {{cy, 0, sy}, {sy * sp, cp, -cy * sp}, {-sy * cp, sp, cy * cp}};
  std::memcpy(axes, rows_, sizeof(axes));
  const float camera[3] = {1000.0f, 20.0f, -300.0f};
  cs::WorldLamp wl;
  wl.position[0] = 1012.0f;
  wl.position[1] = 28.0f;
  wl.position[2] = -296.0f;
  wl.colour[0] = 0.9f;
  wl.axis[0] = 0.0f;
  wl.axis[1] = -2.0f;
  wl.axis[2] = 0.0f;
  wl.intensity = 55.0f;
  wl.cone_cos_outer = -0.3f;
  wl.cone_cos_inner = 0.1f;
  uint32_t rows[3][4];
  cs::MakeGpuLamp(wl, camera, axes, rows);
  const cs::Lamp packed = cs::UnpackGpuLamp(rows);
  float back[3];
  const float view[3] = {packed.position.x, packed.position.y, packed.position.z};
  rh::WorldPoint(camera, axes, view, back);
  CHECK(Near(back[0], wl.position[0], 1e-4) && Near(back[1], wl.position[1], 1e-4) && Near(back[2], wl.position[2], 1e-4));
  CHECK(Near(cs::Length(packed.axis), 1.0, 1e-5) && Near(packed.intensity, 55.0) && Near(packed.cos_outer, -0.3) && Near(packed.cos_inner, 0.1) && Near(packed.colour.x, 0.9));
  // the axis (down in the world) is the world's -y in view space
  CHECK(Near(packed.axis.x, -axes[1][0], 1e-5) && Near(packed.axis.y, -axes[1][1], 1e-5) && Near(packed.axis.z, -axes[1][2], 1e-5));
  // and what the shader's loader makes of the rows is the same lamp
  hlsl_shim::uint4 r0(rows[0][0], rows[0][1], rows[0][2], rows[0][3]), r1(rows[1][0], rows[1][1], rows[1][2], rows[1][3]), r2(rows[2][0], rows[2][1], rows[2][2], rows[2][3]);
  const hlsl_shim::CityHitLamp sl = hlsl_shim::CityHitLampFromRows(r0, r1, r2);
  CHECK(Near(sl.position.x, packed.position.x) && Near(sl.axis.y, packed.axis.y) && Near(sl.colour.x, 0.9) && Near(sl.intensity, 55.0) && Near(sl.cosOuter, -0.3) && Near(sl.cosInner, 0.1));

  // the screen's weight: seen, off the edge, not in front, the depth wrong, the surface turned away, rejected
  cs::Screen s;
  s.tan_x = 1.0f;
  s.tan_y = 0.5625f;
  s.width = 1920;
  s.height = 1080;
  const auto depth_is = [](float z) { return [z](int, int) { return z; }; };
  const cs::V3 facing_normal{0, 0, -1};
  cs::ScreenAt at = cs::ScreenWeight(s, {0.0f, 0.0f, 10.0f}, facing_normal, depth_is(10.0f), nullptr);
  CHECK(Near(at.weight, 1.0) && Near(at.uv[0], 0.5) && Near(at.uv[1], 0.5) && Near(at.pixel, 2.0f * 0.5625f * 10.0f / 1080.0f));
  at = cs::ScreenWeight(s, {5.0f, 2.0f, 10.0f}, facing_normal, depth_is(10.0f), nullptr);  // inside, away from the edge
  CHECK(at.weight > 0.9f && Near(at.uv[0], 0.75) && Near(at.uv[1], 0.5 - 2.0 / 10.0 / 0.5625 * 0.5, 1e-5));
  CHECK(cs::ScreenWeight(s, {10.0f, 0.0f, 10.0f}, facing_normal, depth_is(10.0f), nullptr).weight == 0.0f);   // on the edge
  CHECK(cs::ScreenWeight(s, {0.0f, 0.0f, -4.0f}, facing_normal, depth_is(10.0f), nullptr).weight == 0.0f);    // behind the camera
  CHECK(cs::ScreenWeight(s, {0.0f, 0.0f, 0.03f}, facing_normal, depth_is(10.0f), nullptr).weight == 0.0f);    // nearer than the minimum depth
  const float edge_in = cs::ScreenWeight(s, {9.8f, 0.0f, 10.0f}, facing_normal, depth_is(10.0f), nullptr).weight;  // inside the edge band: partly
  CHECK(edge_in > 0.0f && edge_in < 1.0f);
  CHECK(cs::ScreenWeight(s, {0.0f, 0.0f, 10.0f}, facing_normal, depth_is(14.0f), nullptr).weight == 0.0f);   // the picture shows something else there
  const float near_depth = cs::ScreenWeight(s, {0.0f, 0.0f, 10.0f}, facing_normal, depth_is(10.15f), nullptr).weight;  // a hair off: the tolerance
  CHECK(near_depth > 0.0f && near_depth < 1.0f + 1e-6f);
  CHECK(cs::ScreenWeight(s, {0.0f, 0.0f, 10.0f}, {0, 0, 1}, depth_is(10.0f), nullptr).weight == 0.0f);        // the surface faces away from the camera
  CHECK(cs::ScreenWeight(s, {0.0f, 0.0f, 10.0f}, facing_normal, depth_is(10.0f), [](int, int) { return true; }).weight == 0.0f);  // a rejected pixel (a vehicle's, for its own ray)
  CHECK(Near(cs::ScreenMip(0.0f, 0.01f), 0.0) && Near(cs::ScreenMip(0.04f, 0.01f), 2.0, 1e-5) && Near(cs::ScreenMip(0.005f, 0.01f), 0.0));
  const cs::V3 capped = cs::CapLuma({3.0f, 3.0f, 3.0f}, 1.0f);
  CHECK(Near(cs::Luma(capped), 1.0, 1e-5) && Near(capped.x, capped.y) && Near(cs::CapLuma({0.5f, 0.5f, 0.5f}, 1.0f).x, 0.5) && Near(cs::CapLuma({3.0f, 3.0f, 3.0f}, 0.0f).x, 3.0));

  // the mix: the picture in full, the model, both; no data
  cs::Shaded sh;
  sh.valid = true;
  sh.cls = 3;
  sh.colour = {1.0f, 0.5f, 0.25f};
  const cs::V3 seen{0.2f, 0.2f, 0.2f};
  cs::Resolved r = cs::Mix(1.0f, seen, sh, 2.0f);
  CHECK(r.source == cs::kSourceScreen && Near(r.colour.x, 0.2));
  r = cs::Mix(0.0f, seen, sh, 2.0f);
  CHECK(r.source == cs::kSourceShaded && Near(r.colour.x, 2.0) && Near(r.colour.z, 0.5) && r.cls == 3);
  r = cs::Mix(0.25f, seen, sh, 2.0f);
  CHECK(r.source == cs::kSourceBlend && Near(r.colour.x, 2.0 + (0.2 - 2.0) * 0.25));
  cs::Shaded none;
  CHECK(cs::Mix(0.0f, seen, none, 1.0f).source == cs::kSourceNone);
  CHECK(cs::Mix(0.7f, seen, none, 1.0f).source == cs::kSourceScreen && Near(cs::Mix(0.7f, seen, none, 1.0f).colour.x, 0.2));
  CHECK(cs::IsCalibrationSample(1.0f, 0.5f, 0.5f) && cs::IsCalibrationSample(0.98f, 0.5f, 0.5f) && !cs::IsCalibrationSample(0.97f, 0.5f, 0.5f) && !cs::IsCalibrationSample(1.0f, 0.02f, 0.5f) &&
        !cs::IsCalibrationSample(1.0f, 0.5f, 0.01f));
}

// ---------------------------------------------------------------------------
// the synthetic city
// ---------------------------------------------------------------------------

struct Sector {
  MeshData mesh;
  HitBlob blob;
  bool runs = true, corners = true, built = true;
  PoolBlockPlan plan;
  uint64_t base = 0;
};

// Materials: 0 default; 1 wall (texture 0); 2 road (texture 1, gamma, resident from level 1); 3 glass; 4 lamp lens (emissive); 5 metal (texture 2, not resident); 6 wet road (texture 0);
// 7 water; 8 neon sign (emissive, texture 0); 9 lit window (interior, emissive)
struct City {
  std::vector<Sector> sectors;
  std::vector<uint8_t> pool;
  std::vector<uint32_t> slots;
  std::vector<uint32_t> material_words;
  std::vector<GpuTextureRow> texture_rows;
  std::vector<HitTexture> textures;
  std::vector<HitMaterial> materials;
  std::vector<hlsl_shim::uint4> slot_rows, material_rows, texture_table;
  HitView view;
  int blobs_round_tripped = 0, blobs_with_corners = 0;  // the hit blobs written and read back by the format's own encode and decode before they fill the pool
};

void MakeFakeTexture(Lcg& rng, uint32_t slot, uint32_t width, uint32_t height, uint32_t levels) {
  auto& all = hlsl_shim::FakeTextures();
  if (all.size() <= slot) all.resize(slot + 1);
  hlsl_shim::FakeTexture& t = all[slot];
  t.width = width;
  t.height = height;
  t.levels.clear();
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t w = std::max(width >> l, 1u), h = std::max(height >> l, 1u);
    std::vector<hlsl_shim::float4> level(size_t(w) * h);
    for (auto& v : level) v = hlsl_shim::float4(rng.Range(0.05f, 1.0f), rng.Range(0.05f, 1.0f), rng.Range(0.05f, 1.0f), 1.0f);
    t.levels.push_back(level);
  }
}

Sector MakeSector(Lcg& rng, uint32_t material, bool runs, bool corners, bool built, uint32_t run_flags) {
  Sector s;
  s.runs = runs;
  s.corners = corners && runs;
  s.built = built;
  const float x = rng.Range(-30, 30), z = rng.Range(-30, 30), w = rng.Range(2, 8), h = rng.Range(2, 6);
  s.mesh.positions = {x, 0, z, x + w, 0, z, x + w, h, z + 0.5f * w, x, h, z - 0.5f * w};
  s.mesh.indices = {0, 1, 2, 0, 2, 3};
  s.mesh.opaque_triangles = 2;
  s.blob.triangle_count = 2;
  s.blob.opaque_triangles = 2;
  s.blob.flags = s.corners ? kHitBlobHasCorners : 0;
  HitRun run;
  run.first_triangle = 0;
  run.triangle_count = 2;
  run.material = material;
  run.flags = run_flags;
  const float uv_scale = rng.Range(0.1f, 2.5f);
  ChooseUvTransform(0.0f, uv_scale * w, 0.0f, uv_scale * h, &run);
  s.blob.runs.push_back(run);
  if (s.corners) {
    const float uv[2][3][2] = {{{0, 0}, {uv_scale * w, 0}, {uv_scale * w, uv_scale * h}}, {{0, 0}, {uv_scale * w, uv_scale * h}, {0, uv_scale * h}}};
    for (int t = 0; t < 2; ++t)
      for (int k = 0; k < 3; ++k) {
        s.blob.corners.push_back(EncodeCornerUv(run, uv[t][k][0], uv[t][k][1]));
        s.blob.corners.push_back(PackCornerColour(rng.Below(256), rng.Below(256), rng.Below(256), 80 + rng.Below(176)));
      }
  }
  return s;
}

City BuildCity(Lcg& rng) {
  City c;
  // fake textures (the bindless slots): 1 the wall's (64 x 64, 7 levels), 2 the road's (resident from level 1 of 6: 16 x 16, 5 levels), 3 not used; the atlas 7; the environment map 100.. (16 x 8, 4 levels)
  MakeFakeTexture(rng, 1, 64, 64, 7);
  MakeFakeTexture(rng, 2, 16, 16, 5);
  MakeFakeTexture(rng, 7, 64, 32, 1);
  for (uint32_t i = 0; i < 5; ++i) MakeFakeTexture(rng, 100 + i, 16, 8, 4);
  HitTexture wall, road;
  wall.width = wall.height = 64;
  wall.mip_count = 7;
  wall.format = HitTextureFormat::kBc1;
  road.width = road.height = 32;
  road.mip_count = 6;
  road.flags = kTextureGamma;
  road.format = HitTextureFormat::kBc3;
  HitTexture gone = wall;
  c.textures = {wall, road, gone};
  c.texture_rows.push_back(MakeTextureRow(wall, 1, 0));
  c.texture_rows.push_back(MakeTextureRow(road, 2, 1));
  c.texture_rows.push_back(MakeTextureRow(gone, kNoSlot, 0));

  const auto material = [&](uint32_t cls, uint32_t smooth, uint32_t metal, uint16_t texture, const float tint[3], const float average[3], float emissive) {
    HitMaterial m;
    m.word = PackMaterialWord(smooth, cls, 0, 255, metal);
    m.texture = texture;
    for (int k = 0; k < 3; ++k) {
      m.tint[k] = tint[k];
      m.average[k] = average[k];
    }
    m.emissive = emissive;
    return m;
  };
  const float white[3] = {1, 1, 1}, warm[3] = {0.9f, 0.8f, 0.7f}, grey[3] = {0.5f, 0.5f, 0.5f}, dark[3] = {0.05f, 0.07f, 0.09f}, lamp[3] = {1.0f, 0.9f, 0.6f}, avg1[3] = {0.45f, 0.4f, 0.35f},
              avg2[3] = {0.3f, 0.3f, 0.32f};
  c.materials.resize(10);
  c.materials[0] = HitMaterial{};
  c.materials[1] = material(1, 38, 0, 0, warm, avg1, 0.0f);
  c.materials[2] = material(2, 60, 0, 1, warm, avg2, 0.0f);  // (a tint that is not white: the packed road's grey is multiplied by the tint per channel)
  c.materials[3] = material(11, 230, 0, kNoTexture, dark, dark, 0.0f);
  c.materials[4] = material(13, 100, 0, kNoTexture, lamp, lamp, 1.0f);
  c.materials[5] = material(10, 200, 220, 2, white, grey, 0.0f);
  c.materials[6] = material(3, 220, 0, 0, white, avg1, 0.0f);
  c.materials[7] = material(15, 255, 0, kNoTexture, dark, dark, 0.0f);
  c.materials[8] = material(13, 90, 0, 0, white, avg1, 2.5f);
  c.materials[9] = material(14, 80, 0, kNoTexture, warm, warm, 1.5f);
  c.materials[2].grey_in_red = true;  // RT-GREEN-BAND: the road's diffuse is packed (grey, specular mask): its albedo is the red channel as a grey (CityRoad, CityTunnelRoad); the other textured materials read colours
  c.material_words.resize(c.materials.size() * 8);
  for (size_t m = 0; m < c.materials.size(); ++m) c.materials[m].Pack(&c.material_words[m * 8]);

  const uint32_t uv_colour = kRunHasUv | kRunHasColour;
  c.sectors.push_back(MakeSector(rng, 1, true, true, true, uv_colour));        // 0 wall
  c.sectors.push_back(MakeSector(rng, 2, true, true, true, uv_colour));        // 1 road (packed: the grey is in the red channel)
  c.sectors.push_back(MakeSector(rng, 3, true, true, true, uv_colour));        // 2 glass
  c.sectors.push_back(MakeSector(rng, 4, true, true, true, uv_colour));        // 3 lamp lens
  c.sectors.push_back(MakeSector(rng, 5, true, true, true, uv_colour));        // 4 metal, texture not resident
  c.sectors.push_back(MakeSector(rng, 6, true, true, true, uv_colour));        // 5 wet road
  c.sectors.push_back(MakeSector(rng, 1, true, false, true, uv_colour));       // 6 runs but no corners
  c.sectors.push_back(MakeSector(rng, 1, false, false, true, 0));              // 7 geometry only
  c.sectors.push_back(MakeSector(rng, 8, true, true, true, uv_colour));        // 8 neon sign
  c.sectors.push_back(MakeSector(rng, 9, true, true, true, kRunHasColour));    // 9 lit window, no uv
  c.sectors.push_back(MakeSector(rng, 7, true, true, true, kRunHasUv));        // 10 water, no vertex colour
  c.sectors.push_back(MakeSector(rng, 1, true, true, false, uv_colour));       // 11 not built
  // the hit file's way: every blob is encoded (the delta coded corner stream) and decoded by the format before it fills the pool, as the reader does for the file's blobs
  for (Sector& s : c.sectors) {
    if (!s.runs) continue;
    const std::vector<uint8_t> bytes = EncodeHitBlob(s.blob, CornerEncoding::kDelta);
    HitBlob decoded;
    std::string error;
    if (DecodeHitBlob(bytes.data(), bytes.size(), &decoded, &error) && decoded.corners == s.blob.corners && decoded.runs.size() == s.blob.runs.size() && decoded.flags == s.blob.flags) {
      ++c.blobs_round_tripped;
      s.blob = decoded;
    }
    c.blobs_with_corners += s.corners ? 1 : 0;
  }
  uint64_t end = 64;
  for (Sector& s : c.sectors) {
    s.plan = PlanPoolBlock(s.mesh.vertex_count(), s.mesh.triangle_count(), s.runs ? uint32_t(s.blob.runs.size()) : 0, s.runs, s.corners);
    s.base = end;
    end += s.plan.total_bytes;
    end = (end + 15) / 16 * 16;
  }
  c.pool.assign(size_t(end), 0xA5);
  c.slots.assign(c.sectors.size() * 2 * 8, 0);
  for (size_t i = 0; i < c.sectors.size(); ++i) {
    Sector& s = c.sectors[i];
    std::string error;
    if (!FillPoolBlock(s.plan, s.mesh, s.runs ? &s.blob : nullptr, c.pool.data() + s.base, size_t(s.plan.total_bytes), &error)) std::printf("FillPoolBlock: %s\n", error.c_str());
    GpuSectorRow row;
    MakeSectorRow(s.plan, s.base, s.mesh.triangle_count(), s.mesh.opaque_triangles, 0, &row);
    if (!s.built) row = GpuSectorRow{};
    std::memcpy(&c.slots[size_t(SectorRowOf(uint32_t(i), false)) * 8], &row, sizeof(row));
  }
  c.slot_rows.resize(c.slots.size() / 4);
  for (size_t i = 0; i < c.slot_rows.size(); ++i) c.slot_rows[i] = hlsl_shim::uint4(c.slots[4 * i], c.slots[4 * i + 1], c.slots[4 * i + 2], c.slots[4 * i + 3]);
  c.material_rows.resize(c.material_words.size() / 4);
  for (size_t i = 0; i < c.material_rows.size(); ++i)
    c.material_rows[i] = hlsl_shim::uint4(c.material_words[4 * i], c.material_words[4 * i + 1], c.material_words[4 * i + 2], c.material_words[4 * i + 3]);
  c.texture_table.resize(c.texture_rows.size());
  for (size_t i = 0; i < c.texture_rows.size(); ++i) c.texture_table[i] = hlsl_shim::uint4(c.texture_rows[i].slot, c.texture_rows[i].size, c.texture_rows[i].mips, c.texture_rows[i].reserved);
  hlsl_shim::gCityPool.data = c.pool.data();
  hlsl_shim::gCityPool.size = c.pool.size();
  hlsl_shim::gCitySectors.data = c.slot_rows.data();
  hlsl_shim::gCitySectors.count = c.slot_rows.size();
  hlsl_shim::gCityMaterials.data = c.material_rows.data();
  hlsl_shim::gCityMaterials.count = c.material_rows.size();
  hlsl_shim::gCityTextures.data = c.texture_table.data();
  hlsl_shim::gCityTextures.count = c.texture_table.size();
  c.view.pool = c.pool.data();
  c.view.pool_size = c.pool.size();
  c.view.sectors = c.slots.data();
  c.view.sector_slots = c.slots.size() / 8;
  c.view.materials = c.material_words.data();
  c.view.material_count = c.material_words.size() / 8;
  return c;
}

// ---------------------------------------------------------------------------
// a random hit, the frame, the parameters
// ---------------------------------------------------------------------------

rh::FrameInput RandomFrame(Lcg& rng, bool cascades) {
  rh::FrameInput in;
  in.light.valid = rng.Below(20) != 0;
  for (int c = 0; c < 3; ++c) {
    in.light.sun[c] = rng.Range(0.0f, 1.5f);
    in.light.ambient[c] = rng.Range(0.0f, 0.3f);
  }
  in.light.env_sharp = rng.Below(3) ? (0x80000000u | (100 + rng.Below(5))) : 0u;
  in.sun_known = rng.Below(5) != 0;
  const float x = rng.Range(-1, 1), y = rng.Range(-0.2f, 1), z = rng.Range(-1, 1);
  const float len = std::sqrt(x * x + y * y + z * z) + 1e-3f;
  in.to_sun_view[0] = x / len;
  in.to_sun_view[1] = y / len;
  in.to_sun_view[2] = z / len;
  const float yaw = rng.Range(0, 6.28f), pitch = rng.Range(-0.5f, 0.5f);
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  const float axes[3][3] = {{cy, 0, sy}, {sy * sp, cp, -cy * sp}, {-sy * cp, sp, cy * cp}};
  std::memcpy(in.world_axes_view, axes, sizeof(axes));
  in.tuning.ambient = rng.Range(0.5f, 1.5f);
  in.tuning.spec = rng.Range(0.5f, 1.5f);
  if (cascades) {
    in.camera_known = true;
    for (int c = 0; c < 3; ++c) in.camera_world[c] = rng.Range(-500, 500);
    in.cascades.valid = true;
    in.cascades.atlas_slot = 7;
    in.cascades.count = 1 + rng.Below(4);
    in.cascades.fade = rng.Range(0, 1);
    in.cascades.depth_bias = rng.Range(-0.01f, 0.01f);
    in.cascades.right[0] = 1;
    in.cascades.top[2] = 1;
    for (uint32_t i = 0; i < in.cascades.count; ++i) {
      in.cascades.rows[i][0][0] = rng.Range(0.0005f, 0.004f);
      in.cascades.rows[i][1][1] = rng.Range(0.0005f, 0.004f);
      in.cascades.rows[i][3][0] = rng.Range(0.2f, 0.8f);
      in.cascades.rows[i][3][1] = rng.Range(0.2f, 0.8f);
      in.cascades.rows[i][2][2] = rng.Range(0.0005f, 0.003f);
      in.cascades.rows[i][3][2] = rng.Range(0.1f, 0.9f);
      in.cascades.rows[i][3][3] = 1.0f;
      in.cascades.box[i][1] = rng.Range(100, 600);
      in.cascades.box[i][3] = rng.Range(100, 600);
    }
  }
  return in;
}

struct Case {
  rh::FrameInput frame;
  cs::Params p;
  cs::Ray ray;
  cs::Instance inst;
  uint32_t sector = 0;
  float b1 = 0, b2 = 0;
  uint32_t primitive = 0;
  std::vector<uint32_t> lamp_rows;  // 12 words a lamp
  std::vector<cs::Lamp> lamps;
};

Case RandomCase(Lcg& rng, uint32_t sectors) {
  Case k;
  k.frame = RandomFrame(rng, rng.Below(2) != 0);
  k.p.mode = rng.Below(4) == 0 ? cs::kModeGi : cs::kModeReflect;
  k.p.flags = rng.U32() & 0x7F;
  k.p.units = rng.Range(1.0f, 16.0f);
  k.p.sun_reach = rng.Below(2) ? 0.0f : rng.Range(10, 500);
  k.p.sun_tan = rng.Below(2) ? 0.0f : 0.0047f;
  k.p.lamp_factor = rng.Below(3) == 0 ? 0.0f : rng.Range(0.1f, 1.0f);
  k.p.wet = rng.Range(0, 1);
  k.p.ao_mix = rng.Range(0, 1);
  k.p.lod_bias = rng.Range(-1, 1);
  k.p.bounce = rng.Below(2) ? 0.0f : rng.Range(0.1f, 0.9f);
  k.p.noise[0] = rng.Next();
  k.p.noise[1] = rng.Next();
  k.p.noise[2] = rng.Next();
  const uint32_t lamps = 4 + rng.Below(5);
  k.p.lamp_first = rng.Below(2);
  k.p.lamp_count = lamps - k.p.lamp_first - rng.Below(2);
  const float yaw = rng.Range(0, 6.28f), pitch = rng.Range(-0.4f, 0.4f);
  k.ray.direction = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
  k.ray.origin = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
  k.ray.t = rng.Range(1.0f, 150.0f);
  k.ray.footprint = rng.Range(0.001f, 2.0f);
  // the instance's 3x3: a rotation, a mirror now and then
  const float a = rng.Range(0, 6.28f), b = rng.Range(0, 6.28f);
  const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
  k.inst.row[0] = {ca, -sa, 0};
  k.inst.row[1] = {sa * cb, ca * cb, -sb};
  k.inst.row[2] = {sa * sb, ca * sb, cb};
  if (rng.Below(4) == 0) k.inst.row[0] = k.inst.row[0] * -1.0f;
  k.sector = rng.Below(sectors);
  k.primitive = rng.Below(2);
  k.b1 = rng.Range(0.0f, 0.5f);
  k.b2 = rng.Range(0.0f, 0.5f);
  // the lamps near the hit, in the frame's view space, as the GPU record holds them
  const cs::V3 hit = k.ray.origin + k.ray.direction * k.ray.t;
  k.lamp_rows.resize(size_t(lamps) * 12);
  for (uint32_t i = 0; i < lamps; ++i) {
    cs::WorldLamp wl;
    const float camera[3] = {0, 0, 0};
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    wl.position[0] = hit.x + rng.Range(-30, 30);
    wl.position[1] = hit.y + rng.Range(3, 14);
    wl.position[2] = hit.z + rng.Range(-30, 30);
    wl.colour[0] = rng.Range(0.3f, 1);
    wl.colour[1] = rng.Range(0.3f, 1);
    wl.colour[2] = rng.Range(0.3f, 1);
    wl.intensity = rng.Range(15, 80);
    if (rng.Below(2)) {
      wl.cone_cos_outer = std::cos(rng.Range(55, 80) * 3.14159265f / 180.0f);
      wl.cone_cos_inner = wl.cone_cos_outer + rng.Range(0.05f, 0.3f);
    }
    uint32_t rows[3][4];
    cs::MakeGpuLamp(wl, camera, axes, rows);
    std::memcpy(&k.lamp_rows[size_t(i) * 12], rows, sizeof(rows));
    k.lamps.push_back(cs::UnpackGpuLamp(rows));
  }
  return k;
}

void BindFrame(const rh::FrameInput& in, std::vector<hlsl_shim::uint4>* buffer) {
  uint32_t rows[rh::kFrameRows][4];
  rh::MakeFrameRows(in, rows);
  buffer->resize(rh::kFrameRows);
  for (uint32_t i = 0; i < rh::kFrameRows; ++i) (*buffer)[i] = hlsl_shim::uint4(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
  hlsl_shim::g_frame.data = buffer->data();
  hlsl_shim::g_frame.count = buffer->size();
}

void BindLamps(const Case& k, std::vector<hlsl_shim::uint4>* buffer) {
  buffer->resize(k.lamp_rows.size() / 4);
  for (size_t i = 0; i < buffer->size(); ++i) (*buffer)[i] = hlsl_shim::uint4(k.lamp_rows[4 * i], k.lamp_rows[4 * i + 1], k.lamp_rows[4 * i + 2], k.lamp_rows[4 * i + 3]);
  hlsl_shim::g_lamps.data = buffer->data();
  hlsl_shim::g_lamps.count = buffer->size();
}

hlsl_shim::CityHitParams ShaderParams(const Case& k) {
  hlsl_shim::CityHitParams p;
  p.mode = k.p.mode;
  p.flags = k.p.flags;
  p.units = k.p.units;
  p.sunReach = k.p.sun_reach;
  p.sunTan = k.p.sun_tan;
  p.lampFactor = k.p.lamp_factor;
  p.wet = k.p.wet;
  p.aoMix = k.p.ao_mix;
  p.lodBias = k.p.lod_bias;
  p.bounce = k.p.bounce;
  p.lampFirst = k.p.lamp_first;
  p.lampCount = k.p.lamp_count;
  p.noise = hlsl_shim::float3(k.p.noise[0], k.p.noise[1], k.p.noise[2]);
  return p;
}
hlsl_shim::CityHitRay ShaderRay(const Case& k) {
  hlsl_shim::CityHitRay r;
  r.origin = F3(k.ray.origin);
  r.direction = F3(k.ray.direction);
  r.t = k.ray.t;
  r.footprint = k.ray.footprint;
  return r;
}
hlsl_shim::CityHitInstance ShaderInstance(const Case& k) {
  hlsl_shim::CityHitInstance i;
  i.instanceId = kInstanceIdFlag | (k.sector << 1);
  i.geometryIndex = 0;
  i.primitiveIndex = k.primitive;
  i.barycentrics = hlsl_shim::float2(k.b1, k.b2);
  i.row0 = F3(k.inst.row[0]);
  i.row1 = F3(k.inst.row[1]);
  i.row2 = F3(k.inst.row[2]);
  return i;
}

// ---------------------------------------------------------------------------
// the twin's reads: the same fake world the shader's reads go to
// ---------------------------------------------------------------------------

std::vector<hlsl_shim::RayCall> gTwinSun, gTwinLamp;
int gTwinAmbient = 0, gTwinFeedback = 0;

cs::Reads MakeReads(const Case& k) {
  cs::Reads r;
  const rh::FrameInput frame = k.frame;
  r.texture = [](uint32_t slot, float u, float v, float lod, bool wrap) {
    const hlsl_shim::FakeTexture& t = hlsl_shim::FakeTextures()[slot & 0x7FFFFFFFu];
    const rt_hit_ref::Rgba c = rt_hit_ref::RefSample(t, u, v, lod, wrap, (slot & 0x80000000u) != 0);
    return cs::V3{c.c[0], c.c[1], c.c[2]};
  };
  r.environment = [frame](cs::V3 reflected, float lod) {
    const float ray[3] = {reflected.x, reflected.y, reflected.z};
    const float world[3] = {rh::Dot3(frame.world_axes_view[0], ray), rh::Dot3(frame.world_axes_view[1], ray), rh::Dot3(frame.world_axes_view[2], ray)};
    float u, v;
    rh::EnvUv(world, &u, &v);
    const uint32_t slot = frame.light.env_sharp;
    const hlsl_shim::FakeTexture& t = hlsl_shim::FakeTextures()[slot & 0x7FFFFFFFu];
    const rt_hit_ref::Rgba c = rt_hit_ref::RefSample(t, u, v, lod, false, (slot & 0x80000000u) != 0);
    return cs::V3{c.c[0], c.c[1], c.c[2]};
  };
  r.sun_blocked = [](cs::V3 o, cs::V3 d, float reach) {
    gTwinSun.push_back({{o.x, o.y, o.z}, {d.x, d.y, d.z}, reach});
    return hlsl_shim::FakeBlocked(o.x, o.y, o.z, d.x, d.y, d.z);
  };
  r.lamp_blocked = [](cs::V3 o, cs::V3 d, float reach) {
    gTwinLamp.push_back({{o.x, o.y, o.z}, {d.x, d.y, d.z}, reach});
    return hlsl_shim::FakeBlocked(o.x, o.y, o.z, d.x, d.y, d.z);
  };
  r.cascade_visible = [frame](cs::V3 hit, cs::V3 n) {
    const float lifted[3] = {hit.x + n.x * rh::kCascadeNormalOffset, hit.y + n.y * rh::kCascadeNormalOffset, hit.z + n.z * rh::kCascadeNormalOffset};
    float world[3];
    rh::WorldPoint(frame.camera_world, frame.world_axes_view, lifted, world);
    const hlsl_shim::FakeTexture& atlas = hlsl_shim::FakeTextures()[frame.cascades.atlas_slot];
    const float lit = rh::CascadeLit(frame.cascades, world, atlas.width, atlas.height, [&](int x, int y) { return atlas.levels[0][size_t(y) * atlas.width + size_t(x)].x; });
    return rh::SunVisibility(lit, frame.cascades.fade, 1.0f);
  };
  r.ambient = [](cs::V3 p, cs::V3 n, cs::V3 ambient) {
    ++gTwinAmbient;
    return ambient * (1.0f + 0.05f * p.x) + cs::V3{0.01f, 0.02f, 0.03f} * n.y;
  };
  r.feedback = [](cs::V3 p, cs::V3 n) {
    ++gTwinFeedback;
    return cs::V3{0.1f + 0.01f * p.y, 0.2f, 0.05f * n.x + 0.1f};
  };
  r.haze = [](cs::V3 colour, float distance) {
    const float keep = std::exp(-0.004f * distance);
    return colour * keep + cs::V3{0.3f, 0.35f, 0.4f} * (1.0f - keep);
  };
  return r;
}

bool CallsAgree(const std::vector<hlsl_shim::RayCall>& a, const std::vector<hlsl_shim::RayCall>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    for (int c = 0; c < 3; ++c) {
      if (!Near(a[i].origin[c], b[i].origin[c], 2e-4) || !Near(a[i].direction[c], b[i].direction[c], 2e-4)) return false;
    }
    if (!Near(a[i].reach, b[i].reach, 2e-4)) return false;
  }
  return true;
}

bool Close(const hlsl_shim::float3& a, const cs::V3& b, double tol = 3e-4) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

struct Coverage {
  int valid = 0, invalid = 0, textured_cases = 0, sun_rays = 0, lamp_rays = 0, cascades = 0, gi = 0, glow = 0, feedback = 0, reflect = 0;
  int grey_cases = 0;  // RT-GREEN-BAND: textured reflect hits on a packed road (the texel read as its red channel)
  int by_class[16] = {};
};

// The decode the twin does for a case: the triangle, the material, its texture's row.
struct Decoded {
  DecodedHit hit;
  HitMaterial material;
  const GpuTextureRow* texture = nullptr;
};
Decoded DecodeCase(const City& city, const Case& k) {
  Decoded d;
  d.hit = DecodeHit(city.view, kInstanceIdFlag | (k.sector << 1), 0, k.primitive, k.b1, k.b2);
  if (d.hit.valid) {
    d.material = HitMaterial::Unpack(&city.material_words[size_t(d.hit.material) * 8]);
    d.texture = d.material.texture != kNoTexture && d.material.texture < city.texture_rows.size() ? &city.texture_rows[d.material.texture] : nullptr;
  }
  return d;
}

// CityHitShade against Shade, on random cases.
void ShadeChecks(const City& city, Lcg& rng, int rounds, Coverage* cov) {
  int wrong_valid = 0, wrong_colour = 0, wrong_albedo = 0, wrong_normal = 0, wrong_light = 0, wrong_rays = 0, wrong_hooks = 0, wrong_class = 0;
  double worst = 0.0;
  hlsl_shim::FakeErrors() = 0;
  hlsl_shim::gCityPool.errors = hlsl_shim::gCitySectors.errors = hlsl_shim::gCityMaterials.errors = hlsl_shim::gCityTextures.errors = 0;
  std::vector<hlsl_shim::uint4> frame_rows, lamp_rows;
  for (int round = 0; round < rounds; ++round) {
    const Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    BindFrame(k.frame, &frame_rows);
    BindLamps(k, &lamp_rows);
    hlsl_shim::gSunCalls.clear();
    hlsl_shim::gLampCalls.clear();
    gTwinSun.clear();
    gTwinLamp.clear();
    hlsl_shim::gAmbientCalls = hlsl_shim::gFeedbackCalls = 0;
    gTwinAmbient = gTwinFeedback = 0;
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    const hlsl_shim::CityHitShaded s = hlsl_shim::CityHitShade(ShaderParams(k), f, ShaderRay(k), ShaderInstance(k));
    const Decoded d = DecodeCase(city, k);
    const cs::Shaded t = cs::Shade(k.p, k.frame, k.ray, k.inst, d.hit, d.material, d.texture, MakeReads(k), k.lamps.data());
    if ((s.valid != 0) != t.valid) {
      ++wrong_valid;
      continue;
    }
    if (!CallsAgree(hlsl_shim::gSunCalls, gTwinSun) || !CallsAgree(hlsl_shim::gLampCalls, gTwinLamp)) ++wrong_rays;
    if (hlsl_shim::gAmbientCalls != gTwinAmbient || hlsl_shim::gFeedbackCalls != gTwinFeedback) ++wrong_hooks;
    if (!t.valid) {
      ++cov->invalid;
      continue;
    }
    ++cov->valid;
    ++cov->by_class[t.cls];
    cov->textured_cases += d.texture && d.texture->slot != kNoSlot && k.p.mode == cs::kModeReflect;
    cov->grey_cases += d.material.grey_in_red && d.texture && d.texture->slot != kNoSlot && k.p.mode == cs::kModeReflect;
    cov->sun_rays += int(hlsl_shim::gSunCalls.size());
    cov->lamp_rays += int(hlsl_shim::gLampCalls.size());
    cov->cascades += (k.p.flags & cs::kPSunCascade) != 0 && (k.frame.tuning.cascade && rh::CascadesUsable(k.frame.cascades) && k.frame.camera_known);
    cov->gi += k.p.mode == cs::kModeGi;
    cov->reflect += k.p.mode == cs::kModeReflect;
    cov->glow += t.glow > 0.0f;
    cov->feedback += hlsl_shim::gFeedbackCalls;
    if (s.cls != t.cls) ++wrong_class;
    if (!Close(s.colour, t.colour)) ++wrong_colour;
    worst = std::max(worst, double(std::fabs(s.colour.x - t.colour.x) / std::max(1.0f, t.colour.x)));
    if (!Close(s.albedo, t.albedo)) ++wrong_albedo;
    if (!Close(s.normal, t.normal, 1e-4)) ++wrong_normal;
    if (!Near(s.ndl, t.ndl, 1e-4) || !Near(s.visible, t.visible, 1e-4) || !Near(s.glow, t.glow, 1e-5)) ++wrong_light;
  }
  Check(wrong_valid == 0, Num("CityHitShade has data for the hits the twin decodes, and none for the rest (%.0f differ)", wrong_valid));
  Check(wrong_class == 0, Num("the class of the hit is the material word's (%.0f wrong)", wrong_class));
  Check(wrong_colour == 0, Num2("the shaded colour is the twin's (%.0f wrong, the worst difference %.1e)", wrong_colour, worst));
  Check(wrong_albedo == 0, Num("the albedo is the twin's: the texture at its level, the tint, the average, the occlusion, the wet road (%.0f wrong)", wrong_albedo));
  Check(wrong_normal == 0, Num("the normal is the geometric normal through the instance's 3x3, facing the ray (%.0f wrong)", wrong_normal));
  Check(wrong_light == 0, Num("N.L of the sun, its visibility and the glow are the twin's (%.0f wrong)", wrong_light));
  Check(wrong_rays == 0, Num("the sun's and the lamps' rays are cast from the same points, in the same directions, as far (%.0f cases differ)", wrong_rays));
  Check(wrong_hooks == 0, Num("the ambient and the feedback hooks are asked as often as the twin asks them (%.0f cases differ)", wrong_hooks));
  Check(hlsl_shim::FakeErrors() == 0, "no texel read outside its level, or from a slot that is not there");
  Check(hlsl_shim::gCityPool.errors == 0 && hlsl_shim::gCitySectors.errors == 0 && hlsl_shim::gCityMaterials.errors == 0 && hlsl_shim::gCityTextures.errors == 0,
        "no misaligned or out of range load on the pool or the tables");
}

// The hit's colour is always a number in [0, the half float's cap], whatever the units are (hlsl NgHeld3 in CityHitShadeHit): a NaN or an infinite factor must not put a NaN or an infinity in it (an infinity times
// the blend's weight of 0 is NaN, and a NaN is spread by every consumer's filter); D3D12's max drops a NaN and Vulkan's leaves it open, so the shader decides it by the bits.
void NanChecks(const City& city, Lcg& rng) {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float units_list[] = {nan, -nan, inf, -inf, 1.0e30f, 0.0f, 1.0f, 0.125f, std::bit_cast<float>(0x7F800001u)};
  const int kinds = int(sizeof(units_list) / sizeof(units_list[0]));
  int shaded = 0, not_numbers = 0, outside = 0, differ = 0, nan_not_black = 0, capped = 0;
  std::vector<hlsl_shim::uint4> frame_rows, lamp_rows;
  for (int round = 0; round < 1800; ++round) {
    Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    k.p.units = units_list[round % kinds];
    BindFrame(k.frame, &frame_rows);
    BindLamps(k, &lamp_rows);
    hlsl_shim::gSunCalls.clear();
    hlsl_shim::gLampCalls.clear();
    gTwinSun.clear();
    gTwinLamp.clear();
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    const hlsl_shim::CityHitShaded s = hlsl_shim::CityHitShade(ShaderParams(k), f, ShaderRay(k), ShaderInstance(k));
    const Decoded d = DecodeCase(city, k);
    const cs::Reads reads = MakeReads(k);
    const cs::Shaded t = cs::Shade(k.p, k.frame, k.ray, k.inst, d.hit, d.material, d.texture, reads, k.lamps.data());
    if (!t.valid || s.valid == 0) continue;
    ++shaded;
    // the hit's colour goes through the air (the test's haze: it adds the fog's colour), so black and the cap are what the haze makes of them
    const float distance = cs::Length(k.ray.origin + k.ray.direction * k.ray.t);
    const cs::V3 black = reads.haze({0.0f, 0.0f, 0.0f}, distance), at_cap = reads.haze({cs::kHitMaxColour, cs::kHitMaxColour, cs::kHitMaxColour}, distance);
    const float channels[3] = {s.colour.x, s.colour.y, s.colour.z};
    const float black_of[3] = {black.x, black.y, black.z}, cap_of[3] = {at_cap.x, at_cap.y, at_cap.z};
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(channels[c])) ++not_numbers;
      else if (channels[c] < 0.0f || channels[c] > cap_of[c] * (1.0f + 1e-5f)) ++outside;
      if (std::fabs(channels[c] - cap_of[c]) <= 1e-4f * cap_of[c]) ++capped;
      if (std::isnan(k.p.units) && std::fabs(channels[c] - black_of[c]) > 1e-5f) ++nan_not_black;
    }
    if (!Close(s.colour, t.colour)) ++differ;
  }
  Check(shaded > 800, Num("the NaN units cases are mostly shaded (%.0f shaded)", shaded));
  Check(not_numbers == 0, Num("the shaded colour is a number whatever the units (NaN, infinite, huge, zero): %.0f channels were not", not_numbers));
  Check(outside == 0, Num("the shaded colour is not negative and at most what a half float keeps with room (%.0f channels outside)", outside));
  Check(nan_not_black == 0, Num("units that are NaN make the colour black (%.0f channels were not)", nan_not_black));
  Check(capped > 0, "units that are infinite or huge cap the colour (the cap is reached)");
  Check(differ == 0, Num("the shaded colour is the twin's with NaN and infinite units too (%.0f cases differ)", differ));
}

// CityHitResolve against the twin's resolve.
void ResolveChecks(const City& city, Lcg& rng, int rounds, int* sources_seen) {
  int wrong_source = 0, wrong_colour = 0, wrong_weight = 0, wrong_fields = 0, wrong_stats = 0, calibrated = 0;
  std::vector<hlsl_shim::uint4> frame_rows, lamp_rows;
  for (int round = 0; round < rounds; ++round) {
    Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    k.p.flags |= cs::kPScreen;
    if (rng.Below(3) == 0) k.p.flags |= cs::kPCalibrate;
    BindFrame(k.frame, &frame_rows);
    BindLamps(k, &lamp_rows);
    for (float& g : hlsl_shim::gGain) g = rng.Range(0.5f, 3.0f);
    const cs::V3 hit = k.ray.origin + k.ray.direction * k.ray.t;
    // the picture: a depth that agrees, nearly agrees or does not with the hit's; some pixels are rejected
    const uint32_t kind = rng.Below(4);
    hlsl_shim::gScreenDepth = kind == 0 ? hit.z : (kind == 1 ? hit.z + rng.Range(-0.2f, 0.2f) : (kind == 2 ? hit.z + rng.Range(-3.0f, 3.0f) : hit.z * rng.Range(0.5f, 1.5f)));
    hlsl_shim::gScreenRejectMod = rng.Below(6) == 0 ? 3 : 0;
    cs::Screen sc;
    sc.tan_x = rng.Range(0.5f, 1.5f);
    sc.tan_y = rng.Range(0.3f, 1.0f);
    sc.width = float(640 + rng.Below(1400));
    sc.height = float(360 + rng.Below(800));
    sc.tolerance_m = rng.Range(0.02f, 0.3f);
    sc.tolerance_rel = rng.Range(0.0f, 0.03f);
    sc.edge_band = rng.Range(0.01f, 0.2f);
    sc.min_facing = rng.Range(0.0f, 0.3f);
    sc.cap = k.p.mode == cs::kModeGi && rng.Below(2) ? rng.Range(0.2f, 2.0f) : 0.0f;
    hlsl_shim::CityHitScreen hs;
    hs.lens = hlsl_shim::float4(sc.tan_x, sc.tan_y, sc.width, sc.height);
    hs.tolerance = hlsl_shim::float4(sc.tolerance_m, sc.tolerance_rel, sc.edge_band, sc.min_facing);
    hs.cap = sc.cap;
    std::fill(hlsl_shim::gStats.begin(), hlsl_shim::gStats.end(), 0u);
    hlsl_shim::gSunCalls.clear();
    hlsl_shim::gLampCalls.clear();
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    const hlsl_shim::CityHitResolved s = hlsl_shim::CityHitResolve(ShaderParams(k), f, hs, ShaderRay(k), ShaderInstance(k));

    // the twin
    const Decoded d = DecodeCase(city, k);
    cs::Resolved tr;
    float weight = 0.0f;
    std::vector<uint32_t> stats(48, 0);
    cs::Shaded shaded;
    if (d.hit.valid) {
      cs::V3 seen;
      const cs::V3 normal = cs::ViewNormal(d.hit.normal, k.inst, k.ray.direction);
      cs::ScreenAt at;
      if ((k.p.flags & cs::kPScreen) != 0) {
        at = cs::ScreenWeight(
            sc, hit, normal, [&](int, int) { return hlsl_shim::gScreenDepth; }, [&](int x, int) { return hlsl_shim::gScreenRejectMod != 0 && x % hlsl_shim::gScreenRejectMod == 0; });
      }
      weight = at.weight;
      if (weight > 0.0f) {
        seen = cs::CapLuma(cs::V3{at.uv[0], at.uv[1], 0.2f + 0.1f * cs::ScreenMip(k.ray.footprint, at.pixel)}, sc.cap);
      }
      if (weight < 1.0f || (k.p.flags & cs::kPCalibrate) != 0) {
        shaded = cs::Shade(k.p, k.frame, k.ray, k.inst, d.hit, d.material, d.texture, MakeReads(k), k.lamps.data());
      }
      if (shaded.valid && (k.p.flags & cs::kPCalibrate) != 0 && cs::IsCalibrationSample(weight, cs::Luma(seen), cs::Luma(shaded.colour))) {
        const uint32_t cls = std::min(shaded.cls, 15u);
        stats[hc::StatsWord(cls, hc::kCountWord)] += 1;
        stats[hc::StatsWord(cls, hc::kSeenWord)] += hc::FixedLog2(cs::Luma(seen));
        stats[hc::StatsWord(cls, hc::kModelWord)] += hc::FixedLog2(cs::Luma(shaded.colour));
        ++calibrated;
      }
      tr = cs::Mix(weight, seen, shaded, shaded.valid ? hlsl_shim::gGain[std::min(shaded.cls, 15u)] : 1.0f);
    }
    if (s.source != tr.source) {
      ++wrong_source;
      continue;
    }
    ++sources_seen[s.source];
    if (s.source == cs::kSourceNone) continue;
    if (!Close(s.colour, tr.colour, 5e-4)) ++wrong_colour;
    if (!Near(s.screen, tr.screen, 2e-4)) ++wrong_weight;
    if (shaded.valid && (s.cls != shaded.cls || !Near(s.ndl, shaded.ndl, 1e-4) || !Near(s.visible, shaded.visible, 1e-4) || !Close(s.normal, shaded.normal, 1e-4))) ++wrong_fields;
    for (size_t i = 0; i < stats.size(); ++i) {
      if (stats[i] != hlsl_shim::gStats[i]) {
        ++wrong_stats;
        break;
      }
    }
  }
  Check(wrong_source == 0, Num("the source of the resolve (none, the picture, the model, both) is the twin's (%.0f wrong)", wrong_source));
  Check(wrong_colour == 0, Num("the resolved colour is the twin's: the picture at the cone's mip, the model times its class's gain, the mix, the cap (%.0f wrong)", wrong_colour));
  Check(wrong_weight == 0, Num("the picture's weight is the twin's: the point's place, the edge, the depth, the facing (%.0f wrong)", wrong_weight));
  Check(wrong_fields == 0, Num("the class, the sun and the normal of a shaded hit come through the resolve (%.0f wrong)", wrong_fields));
  Check(wrong_stats == 0, Num("the calibration's counters are the twin's: the class's count, the picture's summed log, the model's (%.0f cases differ)", wrong_stats));
  Check(calibrated > 40, Num("the calibration was exercised (%.0f samples)", calibrated));
}

// the text of the library: what the host and the other passes rely on
void TextChecks(const std::string& src) {
  const std::string lib = ReadFile(src + "/gpu/hlsl/city_hit_shade.hlsli");
  const std::string check = ReadFile(src + "/gpu/hlsl/city_hit_shade_check_cs.hlsl");
  if (lib.empty() || check.empty()) {
    std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    return;
  }
  const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
  CHECK(has(lib, "#include \"city_hit_common.hlsli\"") && has(lib, "#include \"rt_hit_common.hlsli\"") && has(lib, "#include \"city_hit_alpha.hlsli\""));
  CHECK(has(lib, "CityHitResolved CityHitResolve(") && has(lib, "CityHitShaded CityHitShade(") && has(lib, "CityHitTraceResult CityHitTrace("));
  // the library samples through the shared code (one path with the car's hit shading)
  CHECK(has(lib, "RtHitSample(t.slot, h.uv, level - float(t.firstMip), true)") && has(lib, "RtHitHemisphere(f, normal)") && has(lib, "RtHitAmbientLight(f, hemisphere)") && has(lib, "RtHitCascadeVisible(f, hit, normal)") &&
        has(lib, "RtHitEnvAt(f, reflected, lod)"));
  // the library does not use the cars' frame gain, the cars' night factor or the car's classes
  CHECK(!has(lib, "f.gain") && !has(lib, "f.night") && !has(lib, "CAR_MAT_") && !has(lib, "gCarRt"));
  // the reference consumer defines what the library asks of it
  CHECK(has(check, "#define RT_HIT_FRAME_ROW(i) g_frame[i]") && has(check, "#define RT_HIT_TLAS gCarRtTlas") && has(check, "#define CITY_HIT_GAIN(cls) g_gains[cls]") && has(check, "#define CITY_HIT_STAT_ADD(word, value)") &&
        has(check, "#define CITY_HIT_LAMP_LOAD(i)") && has(check, "#include \"city_hit_shade.hlsli\"") && has(check, "CityHitTrace(") && has(check, "CityHitResolve("));
  // no identifier that is an HLSL keyword (dxc would refuse it): point, out, target (the code, not the comments)
  std::string code;
  {
    std::istringstream lines(lib);
    std::string line;
    while (std::getline(lines, line)) code += line.substr(0, line.find("//")) + " ";
  }
  for (const char* word : {" point,", " point)", " point;", "float3 out ", " out = ", "const float target"}) CHECK(!has(code, word));
}

#include "city_hit_lamp_term_checks.inc"

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "src";
  NumbersChecks();
  TwinChecks();
  Lcg rng;
  const City city = BuildCity(rng);
  Check(city.blobs_round_tripped == 11 && city.blobs_with_corners == 10, Num2("the hit blobs of the synthetic city went through the format's encode and decode before they filled the pool (%.0f round trips, %.0f with corners)", city.blobs_round_tripped, city.blobs_with_corners));
  Coverage cov;
  ShadeChecks(city, rng, 6000, &cov);
  NanChecks(city, rng);
  Check(cov.valid > 2500 && cov.invalid > 100, Num2("the random hits are mostly shaded and some are not (%.0f shaded, %.0f without data)", cov.valid, cov.invalid));
  int classes_hit = 0;
  for (int c = 0; c < 16; ++c) classes_hit += cov.by_class[c] > 0;
  Check(classes_hit >= 8, Num("the random hits cover the classes of the synthetic city (%.0f classes)", classes_hit));
  Check(cov.grey_cases > 30, Num("the packed road's textured hits were exercised (%.0f cases: the shader and the twin read its texel as the red channel alone)", cov.grey_cases));
  Check(cov.textured_cases > 500 && cov.sun_rays > 1000 && cov.lamp_rays > 300 && cov.cascades > 300 && cov.gi > 600 && cov.reflect > 1800 && cov.glow > 200 && cov.feedback > 100,
        Num2("textured, sun-ray, lamp-ray, cascade, GI, glow and feedback cases were all exercised (textured %.0f, sun rays %.0f)", cov.textured_cases, cov.sun_rays));
  int sources[4] = {};
  ResolveChecks(city, rng, 6000, sources);
  Check(sources[cs::kSourceScreen] > 400 && sources[cs::kSourceShaded] > 400 && sources[cs::kSourceBlend] > 100 && sources[cs::kSourceNone] > 100,
        Num2("the resolve ended in the picture, the model, both and nothing (picture %.0f, model %.0f)", sources[cs::kSourceScreen], sources[cs::kSourceShaded]));
  LampTermChecks(city, rng, src);
  TextChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
