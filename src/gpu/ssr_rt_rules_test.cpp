// Check of the ray traced fallback of the screen-space reflections (ssr_rt_rules.h, hlsl/ssr_rt_common.hlsli, hlsl/ssr_rt_select_cs.hlsl, hlsl/ssr_rt_trace_cs.hlsl, and the sources that use them:
// ssr_rt.h / ssr_rt.cpp, ssr.cpp, host_passes, the build lists). A program of its own: no renderer, no ReXGlue, no GPU; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssr_rt_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssr_rt_rules_test [<src/gpu dir>] [--print]
//
// It checks: the class groups and the confidence target's codec (through the 8 bit quantisation of the target), the candidate rule, the cap (the share kept, the weight, the unbiasedness of the thinning, the
// tile order's turn), the ray's lift and the hit's fade against the city's own shadow pass (city_rt_shadow_rules.h Lift), the blend, the settings' decision at every combination of the effect's level, the
// debug word and the card's rung (the identity on a card that does not trace), the cells, the push block's and the buffers' words against the .hlsli's #defines, the governor (it steps down under a
// budget, holds, grows back only when the projected cost fits, does not oscillate), the counters' wrap, the census lines; it compiles hlsl/ssr_rt_common.hlsli ITSELF as C++ (through a shim of the HLSL
// types) and compares every function with the twin. Given the src/gpu folder it also reads the sources.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "city_rt_shadow_rules.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "noise_resolve_check.h"
#include "rt_hit_calibration.h"
#include "setting_source.h"
#include "ssr_rt_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++ (the shim of what ssr_common.hlsli and ssr_rt_common.hlsli use; gbuffer_common.hlsli and ao_gbuffer_normal.hlsli first, as the shaders include them)
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float3 sqrt(float3 v) { return float3(std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z)); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float3 normalize(float3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline int clamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline int min(int a, int b) { return std::min(a, b); }
inline int max(int a, int b) { return std::max(a, b); }
inline float floor(float v) { return std::floor(v); }
inline float frac(float v) { return v - std::floor(v); }
inline float exp2(float v) { return std::exp2(v); }
inline float log2(float v) { return std::log2(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
#include "hlsl/gbuffer_common.hlsli"
#include "hlsl/ao_gbuffer_normal.hlsli"
#include "hlsl/ssr_common.hlsli"
#include "hlsl/ssr_rt_common.hlsli"
}  // namespace hlsl_shim

namespace rt = mcla::gpu::ssr_rt;
namespace gb = mcla::gpu::gbuffer;
namespace gc = mcla::gpu::gpu_class;
namespace hs = hlsl_shim;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}
size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

// The value of `#define NAME value` in a text (unsigned; "u" suffix allowed), or ~0 when there is none.
uint64_t DefineOf(const std::string& text, const std::string& name) {
  const std::string key = "#define " + name + " ";
  size_t at = text.find(key);
  while (at != std::string::npos) {
    // a whole word: the character before is a line start
    if (at == 0 || text[at - 1] == '\n') {
      const size_t from = at + key.size();
      return std::strtoull(text.c_str() + from, nullptr, 0);
    }
    at = text.find(key, at + key.size());
  }
  return ~0ull;
}

struct Lcg {
  uint32_t state = 20261121u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

// ---------------------------------------------------------------------------
// the class groups and the confidence target
// ---------------------------------------------------------------------------

void TestGroups() {
  using SC = gb::SurfaceClass;
  uint32_t seen[3] = {};
  for (uint32_t c = 0; c < 16; ++c) {
    const uint32_t twin = rt::GroupOf(SC(c));
    const uint32_t shader = hs::SsrRtGroupOf(c);
    Check(twin == shader, Num("class %.0f: the shader's group %.0f is the twin's", c, shader));
    ++seen[twin];
  }
  Check(rt::GroupOf(SC::kGlass) == rt::kGroupBright && rt::GroupOf(SC::kMetal) == rt::kGroupBright && rt::GroupOf(SC::kWater) == rt::kGroupBright, "glass, metal and water are the bright group");
  Check(rt::GroupOf(SC::kRoad) == rt::kGroupGround && rt::GroupOf(SC::kWetRoad) == rt::kGroupGround && rt::GroupOf(SC::kSidewalk) == rt::kGroupGround && rt::GroupOf(SC::kWorld) == rt::kGroupGround,
        "road, wet road, sidewalk and the tiles the table flags are the ground group");
  Check(rt::GroupOf(SC::kPaint) == rt::kGroupNone && rt::GroupOf(SC::kChrome) == rt::kGroupNone && rt::GroupOf(SC::kSkin) == rt::kGroupNone && rt::GroupOf(SC::kFoliage) == rt::kGroupNone &&
            rt::GroupOf(SC::kNone) == rt::kGroupNone && rt::GroupOf(SC::kEmissive) == rt::kGroupNone,
        "a car's paint and chrome, skin, leaves, nothing and glowing surfaces are in no group: they keep their own reflections");
  Check(seen[1] == 3 && seen[2] == 4, "three classes in the bright group, four in the ground group");
  // the classes the SSR's own gate takes (ssr_common.hlsli SsrClassGain > 0) are exactly the ones with a group
  for (uint32_t c = 0; c < 16; ++c) {
    const bool gate = hs::SsrClassGain(c, uint32_t(GB_FLAG_SSR)) > 0.0f;
    Check(gate == (rt::GroupOf(SC(c)) != rt::kGroupNone), Num("class %.0f: the SSR class gate and the fallback's groups agree", c));
  }
}

void TestConfidenceCodec() {
  // the target is R8G8_UNORM: a value is rounded to 255 steps and back
  const auto unorm8 = [](float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f; };
  for (uint32_t g = 0; g < rt::kGroupCount; ++g) {
    Check(hs::SsrRtGroupIn(unorm8(hs::SsrRtGroupOut(g))) == g, Num("group %.0f survives the 8 bit target", g));
  }
  Check(hs::SsrRtGroupIn(0.0f) == 0 && hs::SsrRtGroupOut(0) == 0.0f, "a pixel nothing was traced for (cleared to 0) is group 0");
  for (uint32_t g = 0; g < rt::kGroupCount; ++g) {
    for (int bin = 0; bin <= 63; ++bin) {
      const float p = float(bin) / 63.0f;
      const float packed = unorm8(hs::SsrRtGroupOut(g, p));
      Check(hs::SsrRtGroupIn(packed) == g, "every ray-share bin preserves the group");
      Check(hs::SsrRtPickRay(packed, p) == 1u && (bin == 0 || hs::SsrRtPickRay(packed, p - 0.001f) == 0u),
            "the packed share is the boundary for selecting the first ray");
    }
  }
  // the candidate rule: every group, mask and confidence against the twin
  const float thresholds[3] = {0.5f, rt::kConfidenceThreshold, 1.0f};
  bool all = true;
  for (uint32_t g = 0; g < 3; ++g) {
    for (uint32_t mask = 0; mask < 16; ++mask) {
      for (float conf : {0.0f, 0.3f, 0.5f, 0.96f, 0.97f, 1.0f}) {
        for (float threshold : thresholds) {
          all = all && (hs::SsrRtCandidate(g, conf, mask, threshold) == rt::Candidate(g, conf, mask, threshold));
        }
      }
    }
  }
  Check(all, "the shader's candidate rule is the twin's over every group, mask, confidence and threshold");
  Check(!rt::Candidate(rt::kGroupNone, 0.0f, 15, 1.0f), "group 0 is never a candidate");
  Check(rt::Candidate(rt::kGroupBright, 0.0f, rt::GroupBit(rt::kGroupBright), rt::kConfidenceThreshold) && !rt::Candidate(rt::kGroupGround, 0.0f, rt::GroupBit(rt::kGroupBright), rt::kConfidenceThreshold),
        "a group the level does not trace is no candidate");
  Check(!rt::Candidate(rt::kGroupBright, 0.97f, 15, rt::kConfidenceThreshold) && rt::Candidate(rt::kGroupBright, 0.969f, 15, rt::kConfidenceThreshold), "the picture speaking for 97 percent or more is no candidate");
  Check(rt::ConfidenceImageBytes(960, 540) == 960ull * 540 * 2, "the confidence image is two bytes a pixel");
  Check(Near(hs::kSsrRtMinRamp, rt::kMinRamp, 0.0), "the ramp floor is the shader's");
}

void TestFallbackCoverage() {
  const auto pick = [](float first, float second, float noise) {
    return hs::SsrRtPickRay(hs::SsrRtGroupOut(2u, hs::SsrRtFirstRayShare(first, first + second, 2u)), noise);
  };
  Check(pick(1.0f, 0.0f, 0.0f) == 1u && pick(0.0f, 1.0f, 0.999f) == 0u, "a fully covered ray gives all work to the missing ray");
  Check(pick(0.25f, 0.75f, 0.70f) == 0u && pick(0.25f, 0.75f, 0.80f) == 1u, "partial coverage selects rays in proportion to their deficits");
  Check(hs::SsrRtFirstRayShare(1.0f, 2.0f, 2u) == 0.0f, "fully covered pixels encode a finite value");
  for (int i = 0; i <= 16; ++i) {
    for (int j = 0; j <= 16; ++j) {
      const float first = i / 16.0f;
      const float second = j / 16.0f;
      const float confidence = (first + second) * 0.5f;
      double combined = 0.0;
      for (int sample = 0; sample < 256; ++sample) {
        const float term = pick(first, second, (sample + 0.5f) / 256.0f) == 0u ? 1.0f : 4.0f;
        combined += rt::Blend(0.0f, confidence, term, 1.0f);
      }
      const double exact = ((1.0 - first) + 4.0 * (1.0 - second)) * 0.5;
      Check(Near(combined / 256.0, exact, 0.031), "single-ray mean matches the two per-ray deficits to quantization precision");
    }
  }
  Check(hs::SsrRtSkyWeight(-0.5f, 0.0f) == 0.0f && hs::SsrRtSkyWeight(0.1f, 0.0f) == 0.0f, "downward and horizon misses keep the planar view");
  Check(Near(hs::SsrRtSkyWeight(0.25f, 0.0f), 0.5, 1e-6) && Near(hs::SsrRtSkyWeight(0.7f, 0.25f), 0.75, 1e-6), "sky contribution fades at the horizon and at a distant hit");
  Check(hs::SsrRtSkyWeight(1.0f, 1.0f) == 0.0f && hs::SsrRtSkyWeight(1.0f, 0.0f) == 1.0f, "a full city hit has no sky contribution and a clear skyward miss has full coverage");
  Check(hs::SsrRtProxyWeight(0.5f) == 0.0f && hs::SsrRtProxyWeight(0.75f) == 0.5f && hs::SsrRtProxyWeight(1.0f) == 1.0f, "proxy recovery fades in above half raster confidence");
  Check(hs::SsrRtNoDataReason(true, true, true, true) == rt::buf::kNoDataLod && hs::SsrRtNoDataReason(true, true, false, true) == rt::buf::kNoDataDefault &&
            hs::SsrRtNoDataReason(false, true, false, true) == rt::buf::kNoDataMissing && hs::SsrRtNoDataReason(false, false, true, true) == rt::buf::kNoDataMissing,
        "the census separates deliberate ground LOD rejection, valid placeholders and unavailable shading");
}

void TestCarHits() {
  using SC = gb::SurfaceClass;
  const uint32_t cars = rt::push::kFlagCars;
  Check(hs::SsrRtCarReceiver(cars, uint32_t(SC::kRoad)), "enabled RT car hits shade on ordinary road material");
  Check(hs::SsrRtCarReceiver(cars | rt::push::kFlagStats, uint32_t(SC::kWetRoad)), "enabled RT car hits shade on wet road material on stats frames");
  Check(!hs::SsrRtCarReceiver(rt::push::kFlagStats, uint32_t(SC::kWetRoad)), "disabled car hits keep the original road reflection");
  Check(!hs::SsrRtCarReceiver(cars, uint32_t(SC::kSidewalk)) && !hs::SsrRtCarReceiver(cars, uint32_t(SC::kGlass)), "car shading is limited to road receivers");
  Check(!rt::CarHitsEnabled(rt::kUltraCell, false) && rt::CarHitsEnabled(rt::kUltraCell, true), "the road-capable cell respects the toggle");
  Check(rt::CarHitsEnabled(rt::kHighCell, true) && !rt::CarHitsEnabled(rt::kMidCell, true), "strong High shades ground hits; the weak cell only traces bright receivers");
  Check(!rt::NeedsFor(rt::Level::kUltra).cars && rt::NeedsFor(rt::Level::kUltra, true).cars && !rt::NeedsFor(rt::Level::kOff, true).any(), "active road car shading requests the meshes independently");
  Check(hs::kSsrRtCarLampVisits == 8u, "the car lighting has a fixed eight-lamp visit limit");
  Check(hs::SsrRtCarLampWeight(0.0f) == 1.0f && hs::SsrRtCarLampWeight(16.0f) == 1.0f, "nearby street lamps give their full light");
  Check(Near(hs::SsrRtCarLampWeight(18.0f), 0.84375, 1e-6) && Near(hs::SsrRtCarLampWeight(20.0f), 0.5, 1e-6), "lamps fade smoothly between 16 and 24 metres");
  Check(hs::SsrRtCarLampWeight(24.0f) == 0.0f && hs::SsrRtCarLampWeight(40.0f) == 0.0f, "lamps outside the car lookup do not contribute");
}

// ---------------------------------------------------------------------------
// the cap
// ---------------------------------------------------------------------------

void TestCap() {
  Check(Near(hs::kSsrRtMinKeep, rt::kMinKeep, 0.0) && hs::kSsrRtTileStride == rt::kTileStride && Near(hs::kSsrRtFadeStart, rt::kFadeStart, 0.0) && Near(hs::kSsrRtLiftFloor, rt::kLiftFloor, 0.0) &&
            Near(hs::kSsrRtLiftPixels, rt::kLiftPixels, 0.0) && Near(hs::kSsrRtLiftCap, rt::kLiftCap, 0.0),
        "the shader's constants are the header's");
  // the share kept
  Check(rt::Keep(1000, 0) == 1.0f && rt::Keep(1000, 1000) == 1.0f && rt::Keep(1000, 500) == 1.0f, "within the capacity everything is kept (and the first frame, with nothing counted, keeps all)");
  Check(Near(rt::Keep(1000, 2000), 0.5, 1e-6) && Near(rt::Keep(1000, 3000), 1.0 / 3.0, 1e-6), "over it the share is the capacity over the candidates");
  Check(rt::Keep(1000, 100000) == rt::kMinKeep, "never under a quarter: a survivor weighs at most four");
  Check(Near(rt::Weight(1.0f), 1.0, 0.0) && Near(rt::Weight(0.5f), 2.0, 0.0) && Near(rt::Weight(0.01f), 4.0, 0.0), "the weight is the inverse of the share, at most 4");
  Lcg rng;
  bool same = true;
  for (int i = 0; i < 2000; ++i) {
    const uint32_t cap = uint32_t(rng.Range(1.0f, 1.0e6f));
    const uint32_t total = uint32_t(rng.Range(0.0f, 4.0e6f));
    same = same && hs::SsrRtKeep(cap, total) == rt::Keep(cap, total) && hs::SsrRtWeight(rt::Keep(cap, total)) == rt::Weight(rt::Keep(cap, total));
  }
  Check(same, "the shader's share and weight are the twin's");
  // the thinning: a stable pattern per frame that keeps the share and, weighted, the whole
  for (float keep : {1.0f, 0.75f, 0.5f, 0.25f}) {
    uint32_t kept = 0;
    const int w = 512, h = 512;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) kept += hs::SsrRtKept(hs::SsrRtThinHash(x, y, 77u, false), keep) ? 1u : 0u;
    }
    const double share = double(kept) / double(w * h);
    Check(Near(share, keep, 0.02), Num("thinning at %.2f keeps a share of %.4f", keep, share));
    Check(Near(double(kept) * rt::Weight(keep), double(w * h), 0.03 * w * h), Num("the weighted survivors at %.2f are the whole (unbiased)", keep));
  }
  Check(hs::SsrRtKept(0.999f, 1.0f) && !hs::SsrRtKept(1.0f, 1.0f) == true, "a share of one keeps every hash under one");
  // the pattern turns with the frame and is spread: a pixel is kept in a share of the frames
  int differs = 0;
  uint32_t frames_kept = 0;
  for (uint32_t f = 0; f < 256; ++f) {
    frames_kept += hs::SsrRtKept(hs::SsrRtThinHash(100, 200, f, false), 0.5f) ? 1u : 0u;
    differs += hs::SsrRtThinHash(100, 200, f, false) != hs::SsrRtThinHash(100, 200, f + 1, false) ? 1 : 0;
  }
  Check(frames_kept > 90 && frames_kept < 166 && differs > 250, Num("one pixel is kept in about half the frames at 0.5 (%.0f of 256)", frames_kept));
  Check(hs::SsrRtThinHash(10, 20, 5u, false) == hs::SsrRtThinHash(10, 20, 5u, false) && hs::SsrRtThinHash(10, 20, 5u, false) != hs::SsrRtThinHash(10, 20, 5u + 256u, false) &&
            hs::SsrRtThinHash(10, 20, 5u, true) == hs::SsrRtThinHash(10, 20, 5u + 256u, true),
        "the hash is deterministic; the blue one steps along R2 and has no period of 256 frames (the first version's, with legacy true, has: the IGN's own)");
  // the tile order: turned by a stride a frame, a permutation of the tiles whatever the frame
  bool perm = true;
  bool moves = false;
  for (uint32_t tiles : {1u, 7u, 7919u, 15840u, 32400u}) {
    for (uint32_t frame : {0u, 1u, 2u, 1000u, 0xFFFFFFFFu}) {
      std::vector<uint8_t> hit(tiles, 0);
      const uint32_t tiles_x = tiles < 240 ? tiles : 240;
      for (uint32_t g = 0; g < tiles; ++g) {
        const uint32_t t = rt::TileOf(g % tiles_x, g / tiles_x, tiles_x, tiles, frame);
        perm = perm && t < tiles && hs::SsrRtTileOf(g % tiles_x, g / tiles_x, tiles_x, tiles, frame) == t;
        if (t < tiles) ++hit[t];
      }
      for (uint8_t n : hit) perm = perm && n == 1;
    }
    if (tiles > 1) moves = moves || rt::TileOf(0, 0, tiles, tiles, 0) != rt::TileOf(0, 0, tiles, tiles, 1);
  }
  Check(perm, "every frame's tile order is a permutation of the tiles, as the shader makes it");
  Check(moves, "the order turns from one frame to the next");
  Check(rt::TileOf(3, 4, 10, 0, 5) == 0, "no tiles (a zero count) does not divide by zero");
  // the capacity and the steps
  Check(rt::Capacity(rt::kHighCell, 0) == 250000 && rt::Capacity(rt::kUltraCell, 0) == 500000 && rt::Capacity(rt::kMidCell, 0) == 100000, "the capacity in pixels at the full step: the cap over the rays a pixel");
  Check(rt::RayCap(rt::kHighCell, 1) == 125000 && rt::RayCap(rt::kHighCell, 2) == 62500 && rt::RayCap(rt::kHighCell, 3) == 31250 && rt::RayCap(rt::kHighCell, 9) == 31250, "each step halves the cap, three at most");
  Check(rt::RayCap(rt::kMidCell, 3) == 12500 && rt::RayCap(rt::Cell{0, 5000, 1, 1.0f, 1.0f}, 3) == 5000 && rt::RayCap(rt::Cell{}, 0) == 0, "a cap is never under kMinRays (a small cell keeps its own), and an empty cell has none");
  Check(rt::GroupsFor(500000) == 7813 && rt::GroupsFor(64) == 1 && rt::GroupsFor(65) == 2 && rt::GroupsFor(0) == 0, "groups of 64");
  for (uint32_t step = 0; step <= rt::Governor::kMaxStep; ++step) {
    const auto rays = rt::RaysUsed(rt::kUltraCell, 2, true);
    const auto capacity = rt::CapacityFor(rt::kUltraCell, step, rays);
    Check(rays == 1 && capacity == 2 * rt::Capacity(rt::kUltraCell, step) && capacity * rays == rt::RayCap(rt::kUltraCell, step),
          "single-ray Ultra doubles pixel capacity at each step and preserves the ray cap");
  }
  Check(rt::RaysUsed(rt::kUltraCell, 2) == 2 && rt::RaysUsed(rt::kHighCell, 1, true) == 1, "default Ultra and one-ray High retain their sample counts");
  Check(rt::CapacityFor(rt::kUltraCell, 0, 1) == rt::kMaxListEntries && rt::kListBytes == 4000000, "the list holds one million pixels (4 MB)");
}

// ---------------------------------------------------------------------------
// the ray and the result
// ---------------------------------------------------------------------------

void TestRay() {
  // the lift is the city's shadow pass's
  bool lift = true;
  Lcg rng;
  for (int i = 0; i < 500; ++i) {
    const float t = rng.Range(0.5f, 800.0f);
    const float unit = rng.Range(0.0004f, 0.004f);
    lift = lift && Near(hs::SsrRtLift(t, unit), mcla::gpu::city_rt_shadow::Lift(t, unit), 1e-6) && Near(rt::Lift(t, unit), mcla::gpu::city_rt_shadow::Lift(t, unit), 1e-6);
  }
  Check(lift, "the ray's lift is the city shadow pass's (a few millimetres and three pixel footprints, at most 0.4 m)");
  // the fade
  Check(Near(hs::SsrRtReachFade(10.0f, 150.0f), 1.0, 0.0) && Near(hs::SsrRtReachFade(112.5f, 150.0f), 1.0, 1e-6) && Near(hs::SsrRtReachFade(150.0f, 150.0f), 0.0, 1e-6) && Near(hs::SsrRtReachFade(400.0f, 150.0f), 0.0, 0.0),
        "a hit's weight is 1 to three quarters of the reach and 0 at it");
  float last = 2.0f;
  bool monotone = true;
  bool same = true;
  for (float t = 0.0f; t <= 160.0f; t += 1.0f) {
    const float f = hs::SsrRtReachFade(t, 150.0f);
    monotone = monotone && f <= last + 1e-6f;
    last = f;
    same = same && Near(f, rt::ReachFade(t, 150.0f), 1e-6);
  }
  Check(monotone && same, "the fade falls smoothly and is the twin's");
  Check(hs::SsrRtHitCoverage(false, 0.0f) == 1.0f && hs::SsrRtHitCoverage(true, 0.0f) == 0.0f &&
            hs::SsrRtHitCoverage(true, 0.5f) == 0.0f && hs::SsrRtHitCoverage(true, 0.625f) == 0.15625f &&
            hs::SsrRtHitCoverage(true, 1.0f) == 1.0f,
        "ground requires strong screen support and fades in smoothly; other groups keep their full hit weight");
  Check(Near(hs::SsrRtFootprint(0.002f, 10.0f, 40.0f, 0.05f), rt::Footprint(0.002f, 10.0f, 40.0f, 0.05f), 1e-6) && Near(rt::Footprint(0.002f, 10.0f, 40.0f, 0.05f), 0.002 * 50.0 + 2.0 * 0.05 * 40.0, 1e-6),
        "the footprint: the pixel's angle over the unfolded path plus the lobe's two half widths");
  // the blend
  const hs::float3 trace(0.5f, 0.25f, 0.125f);
  const hs::float3 term(2.0f, 4.0f, 8.0f);
  const hs::float3 full = hs::SsrRtBlend(trace, 1.0f, term, 1.0f);
  const hs::float3 none = hs::SsrRtBlend(trace, 0.0f, term, 1.0f);
  const hs::float3 half = hs::SsrRtBlend(trace, 0.5f, term, 2.0f);
  Check(full.x == trace.x && full.y == trace.y && full.z == trace.z, "where the picture speaks in full the trace is left alone");
  Check(Near(none.x, 2.5, 1e-6) && Near(none.y, 4.25, 1e-6) && Near(none.z, 8.125, 1e-6), "where it speaks for nothing the traced term is added whole");
  Check(Near(half.x, 0.5 + 2.0, 1e-6) && Near(rt::Blend(0.5f, 0.5f, 2.0f, 2.0f), 2.5, 1e-6), "half the picture's share and a weight of two add the whole term");
  // the lerp it stands for: trace = c * (L - fallback) (the trace's own value), the result is lerp(R - fallback, L - fallback, c)
  const float c = 0.3f, L = 5.0f, R = 9.0f, fb = 1.5f;
  const float ssr_term = c * (L - fb);
  const float expect = (1.0f - c) * (R - fb) + c * (L - fb);
  Check(Near(rt::Blend(ssr_term, c, R - fb, 1.0f), expect, 1e-5), "trace + (1 - c) * term is lerp(ray traced, screen, c) for a trace that is c times the screen's term");
}

// ---------------------------------------------------------------------------
// the decision, the cells
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// the ray is the trace's; the select pass as a model
// ---------------------------------------------------------------------------

void TestRayIsTheTraces() {
  Lcg rng;
  bool same = true;
  bool unit = true;
  for (int i = 0; i < 600; ++i) {
    hs::float3 n = hs::normalize(hs::float3(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f)));
    hs::float3 v = hs::normalize(hs::float3(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f)));
    if (hs::dot(n, v) < 0.05f) v = hs::normalize(v + n * 1.2f);   // the camera is on the normal's side
    const float alpha = rng.Range(0.005f, 1.0f);
    const int px = int(rng.Range(0.0f, 1920.0f));
    const int py = int(rng.Range(0.0f, 1080.0f));
    const uint32_t frame = uint32_t(rng.Range(0.0f, 100000.0f));
    const uint32_t ray = uint32_t(rng.Range(0.0f, 2.0f));
    // the trace's own sequence, as it stood before the function (hlsl/ssr_trace_ps.hlsl)
    hs::float3 old = hs::SsrSampleReflection(n, v, alpha, hs::SsrNoise1(px, py, frame, ray, 2), hs::SsrNoise2(px, py, frame, ray, 2));
    if (hs::dot(old, n) < 0.02f) old = hs::SsrReflect(n, v);
    const hs::float3 now = hs::SsrSampleRay(n, v, alpha, px, py, frame, ray, 2);
    same = same && old.x == now.x && old.y == now.y && old.z == now.z;
    unit = unit && Near(hs::dot(now, now), 1.0, 1e-3) && hs::dot(now, n) >= 0.019f;
  }
  Check(same, "SsrSampleRay is the sequence the trace walked before it was a function (so the fallback's ray is the trace's)");
  Check(unit, "the ray is a unit vector above the surface");
  // the ray differs from pixel to pixel, frame to frame and ray to ray (the noise is the trace's)
  const hs::float3 n(0.0f, 1.0f, 0.0f);
  const hs::float3 v = hs::normalize(hs::float3(0.0f, 0.6f, -0.8f));
  const hs::float3 a = hs::SsrSampleRay(n, v, 0.2f, 10, 10, 5, 0, 2);
  const hs::float3 b = hs::SsrSampleRay(n, v, 0.2f, 11, 10, 5, 0, 2);
  const hs::float3 c = hs::SsrSampleRay(n, v, 0.2f, 10, 10, 6, 0, 2);
  const hs::float3 d = hs::SsrSampleRay(n, v, 0.2f, 10, 10, 5, 1, 2);
  Check((a.x != b.x || a.z != b.z) && (a.x != c.x || a.z != c.z) && (a.x != d.x || a.z != d.z), "another pixel, frame or ray index is another ray");
}

// The select pass on the CPU, group by group as the shader's text has it (hlsl/ssr_rt_select_cs.hlsl), over a made-up confidence image: the counters' rotation, the share kept from the last frame's total, the
// group's one reservation, the tile order. The invariants are the ones the trace pass and the cap rely on.
struct ModelFrame {
  uint32_t total = 0;      // candidates counted
  uint32_t listed = 0;     // the counter's value (may exceed the capacity)
  std::vector<uint32_t> list;   // the entries written (index < capacity), 0xFFFFFFFF where nothing was written
};
// THE THINNING'S NOISE (NOISE-AUDIT, perf\NOISE-AUDIT.md): which candidate pixels are traced in a frame. The first version read interleaved gradient noise at a position moved by (7.7, 3.1) pixels a frame, a lattice
// that slid across the picture; now the blue noise tile stepped along the R2 sequence by the frame (hlsl/blue_noise_frame.hlsli). Over a pixel's frames the share kept is the keep; in one frame the
// kept pixels are a stipple with no lattice; the error of what the history makes of the weighted trace has no lattice and does not move.
void TestThinNoise() {
  namespace nc = noise_check;
  const auto blue = [](int x, int y, uint32_t f) { return std::pair<float, float>(hs::SsrRtThinHash(x, y, f, false), 0.0f); };
  const auto legacy = [](int x, int y, uint32_t f) { return std::pair<float, float>(hs::SsrRtThinHash(x, y, f, true), 0.0f); };
  const nc::PerFrame p = nc::PerFrameOf(blue, 0);
  Check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
        nc::Num("thinning noise: uniform (mean %.3f), changes at every pixel every frame (%.3f), no gap over %.3f in 16 frames, one step at every pixel (%.6f): the pattern moves round the circle and does not slide", p.mean,
            p.changed, p.max_gap, p.step_spread));
  for (const float keep : {0.5f, 0.25f}) {
    const auto response = [keep](float u, float) { return u < keep ? 1.0 / double(keep) : 0.0; };
    const nc::Resolved rl = nc::Resolve(legacy, response, 100, 128, 48, 0.9, false);
    const nc::Resolved rb = nc::Resolve(blue, response, 100, 128, 48, 0.9, false);
    std::printf("  keep %.2f: first version rms %.3f, one frequency %.4f, direction %.2f, match %.2f at (%d, %d); blue rms %.3f, one frequency %.4f, direction %.2f, match %.2f at (%d, %d)\n", keep, rl.rms,
                rl.shape.peak_share, rl.shape.wedge_ratio, rl.best_corr, rl.best_dx, rl.best_dy, rb.rms, rb.shape.peak_share, rb.shape.wedge_ratio, rb.best_corr, rb.best_dx, rb.best_dy);
    Check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb) && rb.rms < 0.5 * rl.rms + 0.05,
          nc::Num("keep %.2f: what the history leaves of the weighted trace has no frequency that stands out (%.4f) or direction (%.2f) and does not move; rms %.3f against the first version's %.3f", keep, rb.shape.peak_share,
              rb.shape.wedge_ratio, rb.rms, rl.rms));
    Check(nc::IsLattice(rl) || nc::Crawls(rl), nc::Num("(the first version's: %.4f in one frequency, a direction %.1f, best match with the next frame %.2f at (%d, %d))", rl.shape.peak_share, rl.shape.wedge_ratio, rl.best_corr, rl.best_dx, rl.best_dy));
  }
  // one frame's kept pixels: a stipple of the density asked for with no frequency that stands out
  {
    const int n = 128;
    const float keep = 0.25f;
    std::vector<double> mask(size_t(n) * size_t(n)), mask_old(size_t(n) * size_t(n));
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        mask[size_t(y) * n + x] = hs::SsrRtKept(hs::SsrRtThinHash(x, y, 40u, false), keep) ? 1.0 - keep : -keep;
        mask_old[size_t(y) * n + x] = hs::SsrRtKept(hs::SsrRtThinHash(x, y, 40u, true), keep) ? 1.0 - keep : -keep;
      }
    }
    const blue_noise_spectrum::Shape sb = blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(mask, n, true), n);
    const blue_noise_spectrum::Shape so = blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(mask_old, n, true), n);
    Check(sb.peak_share < 0.02 && so.peak_share > 0.03,
          nc::Num("one frame's kept pixels at 0.25: no frequency stands out in the blue stipple (%.4f of its energy in one), the first version's was a lattice (%.4f)", sb.peak_share, so.peak_share));
  }
}

void TestSelectModel() {
  const int w = 203, h = 117;   // not a multiple of the tile: the edge tiles are partial
  const uint32_t tiles_x = uint32_t(w + 7) / 8u, tiles_y = uint32_t(h + 7) / 8u, tile_count = tiles_x * tiles_y;
  // the confidence image: a block of the bright group (confidence 0.2), a block of the ground group (confidence 0.99: not a candidate), a block of the bright group at 0.97 (not a candidate)
  std::vector<std::array<float, 2>> conf(size_t(w) * h, {0.0f, 0.0f});
  uint32_t expected = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::array<float, 2>& c = conf[size_t(y) * w + x];
      if (x < 120 && y > 20 && y < 100) {
        c = {hs::SsrRtGroupOut(1), 0.2f};
        ++expected;
      } else if (x >= 150 && y < 60) {
        c = {hs::SsrRtGroupOut(2), 0.99f};
      } else if (x >= 130 && x < 150) {
        c = {hs::SsrRtGroupOut(1), 0.97f};
      }
    }
  }
  const uint32_t mask = rt::GroupBit(rt::kGroupBright);   // the HIGH cell's classes
  const uint32_t capacity = 3500;   // under a third of the candidates (9480): within the thinning's reach (a survivor weighs at most four)
  uint32_t counters[8] = {};   // [0..2] total, [3..5] listed
  uint32_t previous_total_seen[6] = {};
  std::vector<ModelFrame> frames;
  for (uint32_t rt_frame = 0; rt_frame < 6; ++rt_frame) {
    const uint32_t slot = rt_frame % 3, previous = (rt_frame + 2) % 3, next = (rt_frame + 1) % 3;
    ModelFrame mf;
    mf.list.assign(capacity, 0xFFFFFFFFu);
    counters[rt::buf::kCtlTotal + next] = 0;       // group (0, 0) clears the next frame's slot
    counters[rt::buf::kCtlListed + next] = 0;
    const float keep = hs::SsrRtKeep(capacity, counters[rt::buf::kCtlTotal + previous]);
    previous_total_seen[rt_frame] = counters[rt::buf::kCtlTotal + previous];
    for (uint32_t gy = 0; gy < tiles_y; ++gy) {
      for (uint32_t gx = 0; gx < tiles_x; ++gx) {
        const uint32_t tile = hs::SsrRtTileOf(gx, gy, tiles_x, tile_count, rt_frame);
        uint32_t group_total = 0, group_kept = 0;
        std::vector<uint32_t> mine;
        for (uint32_t gi = 0; gi < 64; ++gi) {
          const int px = int(tile % tiles_x) * 8 + int(gi & 7u);
          const int py = int(tile / tiles_x) * 8 + int(gi >> 3u);
          if (px >= w || py >= h) continue;
          const std::array<float, 2>& c = conf[size_t(py) * w + px];
          const bool candidate = hs::SsrRtCandidate(hs::SsrRtGroupIn(c[0]), c[1], mask, rt::kConfidenceThreshold);
          const bool kept = candidate && hs::SsrRtKept(hs::SsrRtThinHash(px, py, rt_frame, false), keep);
          group_total += candidate ? 1u : 0u;
          if (kept) {
            ++group_kept;
            mine.push_back(uint32_t(px) | (uint32_t(py) << 16));
          }
        }
        counters[rt::buf::kCtlTotal + slot] += group_total;
        const uint32_t base = counters[rt::buf::kCtlListed + slot];
        counters[rt::buf::kCtlListed + slot] += group_kept;
        for (uint32_t k = 0; k < mine.size(); ++k) {
          if (base + k < capacity) mf.list[base + k] = mine[k];
        }
      }
    }
    mf.total = counters[rt::buf::kCtlTotal + slot];
    mf.listed = counters[rt::buf::kCtlListed + slot];
    frames.push_back(std::move(mf));
  }
  Check(expected == 120u * 79u, "the made-up image has the candidates it was made with");
  const uint32_t first_expected = expected;
  Check(frames[0].total == first_expected && frames[0].listed == std::min(first_expected, first_expected) && previous_total_seen[0] == 0u,
        Num("frame 0 counts every candidate (%.0f of %.0f) with nothing known of the last frame: all are kept", frames[0].total, first_expected));
  bool counted = true, distinct = true, inside = true, within = true, contiguous = true;
  double weighted_sum = 0.0;
  for (uint32_t f = 0; f < frames.size(); ++f) {
    const ModelFrame& mf = frames[f];
    counted = counted && mf.total == first_expected;   // the candidates are counted before the thinning: the same every frame
    within = within && std::min(mf.listed, capacity) <= capacity;
    std::vector<uint8_t> seen(size_t(w) * h, 0);
    uint32_t written = 0;
    for (uint32_t i = 0; i < capacity; ++i) {
      if (mf.list[i] == 0xFFFFFFFFu) continue;
      ++written;
      const int px = int(mf.list[i] & 0xFFFFu), py = int(mf.list[i] >> 16);
      inside = inside && px >= 0 && px < w && py >= 0 && py < h && conf[size_t(py) * w + px][1] < rt::kConfidenceThreshold && conf[size_t(py) * w + px][0] > 0.0f;
      if (px >= 0 && px < w && py >= 0 && py < h) {
        distinct = distinct && seen[size_t(py) * w + px] == 0;
        seen[size_t(py) * w + px] = 1;
      }
    }
    // the entries of a tile are neighbours: walking the list, a tile's entries form one run
    std::vector<int> run_of_tile(tile_count, -1);
    int run = -1;
    int last_tile = -1;
    for (uint32_t i = 0; i < std::min(mf.listed, capacity); ++i) {
      if (mf.list[i] == 0xFFFFFFFFu) continue;
      const int px = int(mf.list[i] & 0xFFFFu), py = int(mf.list[i] >> 16);
      const int tile = (py / 8) * int(tiles_x) + px / 8;
      if (tile != last_tile) {
        ++run;
        contiguous = contiguous && run_of_tile[size_t(tile)] < 0;   // a tile never comes back after another's entries
        run_of_tile[size_t(tile)] = run;
        last_tile = tile;
      }
    }
    if (f >= 1) {
      // from the second frame on the share kept is the capacity over the last frame's total: the list is about full and, weighted, stands for the candidates
      const double keep = hs::SsrRtKeep(capacity, f >= 3 ? frames[f - 1].total : frames[f - 1].total);
      weighted_sum += double(written) * rt::Weight(float(keep)) / double(first_expected);
    }
  }
  Check(counted, "the candidates are counted before the thinning (the same total every frame: the cap does not hide the demand)");
  Check(inside && distinct && within, "every entry is a candidate pixel inside the image, no pixel is listed twice in a frame, the list never passes its capacity");
  Check(contiguous, "a tile's candidates are listed together (a wave's neighbours are the picture's neighbours)");
  Check(frames[0].listed > capacity, "frame 0 over the cap: the counter says more were kept than fit (the entries past the capacity are dropped)");
  Check(rt::Keep(1000, first_expected) == rt::kMinKeep && rt::Weight(rt::Keep(1000, first_expected)) * 1000.0 < double(first_expected), "past four times the cap the thinning stops at a quarter: the list is full and covers less than the demand (the documented limit)");
  const double average = weighted_sum / double(frames.size() - 1);
  Check(Near(average, 1.0, 0.12), Num("from frame 1 the weighted list stands for the candidates (mean %.3f of 1)", average));
  uint32_t frame1_written = 0;
  for (uint32_t i = 0; i < capacity; ++i) frame1_written += frames[1].list[i] != 0xFFFFFFFFu ? 1u : 0u;
  Check(frame1_written > capacity * 8 / 10 && frame1_written <= capacity, Num("frame 1 fills the list (%.0f of %.0f) and does not overflow it", frame1_written, capacity));
  // the order is turned: the first listed pixel is not the same in two frames
  Check(frames[1].list[0] != frames[2].list[0] || frames[2].list[0] != frames[3].list[0], "the order of the tiles turns from frame to frame");
}

void TestDecision() {
  const rt::Level levels[3] = {rt::Level::kOff, rt::Level::kHigh, rt::Level::kUltra};
  const rt::Choice choices[3] = {rt::Choice::kOff, rt::Choice::kHigh, rt::Choice::kUltra};
  const gc::RtTier rungs[3] = {gc::RtTier::kNone, gc::RtTier::kWeak, gc::RtTier::kStrong};
  bool identity = true;
  bool by_hand = true;
  for (rt::Level ssr : levels) {
    for (rt::Choice choice : choices) {
      for (gc::RtTier rung : rungs) {
        const rt::Resolved r = rt::Resolve({ssr, choice, rung});
        if (ssr == rt::Level::kOff || rung == gc::RtTier::kNone || choice == rt::Choice::kOff) {
          identity = identity && r.level == rt::Level::kOff && r.cell.ray_cap == 0 && r.why != rt::Why::kRuns;
          continue;
        }
        const rt::Level wanted = choice == rt::Choice::kHigh ? rt::Level::kHigh : rt::Level::kUltra;
        by_hand = by_hand && r.level == wanted && r.why == rt::Why::kRuns && r.cell.ray_cap == rt::CellFor(wanted, rung).ray_cap && r.cell.ray_cap != 0;
      }
    }
  }
  Check(identity, "the effect off, a card that does not trace and the debug word off all give no fallback at all (the effect is what it was)");
  Check(by_hand, "high and ultra by hand run on a strong card and on a weak one (a card that does not trace never)");
  Check(rt::Resolve({rt::Level::kHigh, rt::Choice::kUltra, gc::RtTier::kStrong}).cell.group_mask == rt::kUltraCell.group_mask, "the debug word can run ULTRA's cell under the effect's high");
  Check(rt::Resolve({rt::Level::kUltra, rt::Choice::kUltra, gc::RtTier::kWeak}).cell.ray_cap == rt::kUltraCell.ray_cap && rt::Resolve({rt::Level::kUltra, rt::Choice::kUltra, gc::RtTier::kWeak}).cell.reach == rt::kUltraCell.reach,
        "Ultra uses the same rays and reach across RT tiers");
  Check(rt::ParseChoice("auto") == rt::Choice::kOff && rt::ParseChoice("off") == rt::Choice::kOff && rt::ParseChoice("high") == rt::Choice::kHigh && rt::ParseChoice("ultra") == rt::Choice::kUltra &&
            rt::ParseChoice("") == rt::Choice::kOff && rt::ParseChoice("medium") == rt::Choice::kOff,
        "legacy Auto and unrecognised values keep the fallback off");
  // the cells
  Check(rt::kMidCell.group_mask == rt::GroupBit(rt::kGroupBright) && rt::kHighCell.group_mask == (rt::GroupBit(rt::kGroupBright) | rt::GroupBit(rt::kGroupGround)) &&
            rt::kUltraCell.group_mask == rt::kHighCell.group_mask,
        "strong High and Ultra trace glossy and ground holes; weak RT traces the bright group");
  Check(rt::kHighCell.ray_cap == 250000 && rt::kUltraCell.ray_cap == 1000000 && rt::kHighCell.rays == 1 && rt::kUltraCell.rays == 2 && rt::kHighCell.reach == 150.0f && rt::kUltraCell.reach == 300.0f, "the design's numbers: 0.25 M and 1.0 M rays, one and two a pixel, 150 and 300 m");
  Check(rt::kMidCell.ray_cap < rt::kHighCell.ray_cap && rt::kHighCell.ray_cap < rt::kUltraCell.ray_cap && rt::kMidCell.reach < rt::kHighCell.reach && rt::kHighCell.reach < rt::kUltraCell.reach &&
            rt::kMidCell.budget_ms == 0.30f && rt::kHighCell.budget_ms == 0.40f && rt::kUltraCell.budget_ms == 0.50f,
        "the cells rise: cap, reach, budget");
  Check(rt::kHighCell.rays == mcla::gpu::ssr::NumbersOf(mcla::gpu::ssr::Level::kHigh).rays && rt::kUltraCell.rays == mcla::gpu::ssr::NumbersOf(mcla::gpu::ssr::Level::kUltra).rays,
        "the cells' rays a pixel are the effect's own (the fallback regenerates the trace's rays)");
  Check(rt::CellFor(rt::Level::kOff, gc::RtTier::kStrong).ray_cap == 0 && rt::CellFor(rt::Level::kHigh, gc::RtTier::kNone).ray_cap == 0, "off and no ray tracing have an empty cell");
  // the tier tables agree: a strong card's rung from rt_tier.h
  Check(gc::RtRung(gc::GpuClass::kHigh, gc::RtTier::kStrong) == gc::RtTier::kStrong && gc::RtRung(gc::GpuClass::kLow, gc::RtTier::kStrong) == gc::RtTier::kStrong && gc::RtRung(gc::GpuClass::kMid, gc::RtTier::kStrong) == gc::RtTier::kStrong,
        "class does not change RT support");
  // the needs
  const mcla::gpu::rt_scene::Needs high = rt::NeedsFor(rt::Level::kHigh);
  const mcla::gpu::rt_scene::Needs ultra = rt::NeedsFor(rt::Level::kUltra);
  Check(high.city == mcla::gpu::city_rt::Level::kHigh && high.hit == mcla::gpu::city_rt::Level::kHigh && high.textures == mcla::gpu::TextureScope::kAll && !high.cars && ultra.city == mcla::gpu::city_rt::Level::kUltra &&
            ultra.hit == mcla::gpu::city_rt::Level::kUltra && !rt::NeedsFor(rt::Level::kOff).any(),
        "the scene service is asked for the structures, the hit data and every texture at the level's level, no cars; off asks nothing");
  // the frame's decision, in its order
  rt::FrameFacts f;
  f.resolved = rt::Resolve({rt::Level::kHigh, rt::Choice::kHigh, gc::RtTier::kStrong});
  f.pipelines_ready = f.scene_valid = f.images_ok = true;
  Check(rt::DecideFrame(f) == rt::Why::kRuns, "everything there: it runs");
  rt::FrameFacts g = f;
  g.scene_valid = false;
  Check(rt::DecideFrame(g) == rt::Why::kNoScene, "no top level: it does not");
  g.images_ok = false;
  Check(rt::DecideFrame(g) == rt::Why::kNoImages, "images before the scene");
  g.pipelines_ready = false;
  Check(rt::DecideFrame(g) == rt::Why::kNoPipelines, "pipelines before the images");
  g.stub = true;
  Check(rt::DecideFrame(g) == rt::Why::kStub, "the stub first of the frame's reasons");
  g.resolved = rt::Resolve({rt::Level::kHigh, rt::Choice::kOff, gc::RtTier::kStrong});
  Check(rt::DecideFrame(g) == rt::Why::kSetting, "the setting's reason before any of the frame's");
  bool named = true;
  for (uint32_t i = 0; i < uint32_t(rt::Why::kCount); ++i) named = named && std::string(rt::WhyName(rt::Why(i))) != "?";
  Check(named, "every reason has words");
}

// ---------------------------------------------------------------------------
// the push block and the buffers
// ---------------------------------------------------------------------------

void TestWords() {
  Check(rt::push::kConf == SSR_W_RT_CONF && rt::push::kCap == SSR_W_RT_CAP && rt::push::kReach == SSR_W_RT_REACH && rt::push::kThreshold == SSR_W_RT_THRESHOLD && rt::push::kFrame == SSR_W_RT_FRAME,
        "the push words are the shader's");
  Check(rt::push::kMaskShift == SSR_RT_CONF_MASK_SHIFT && rt::push::kRaysShift == SSR_RT_RAYS_SHIFT && rt::push::kFlagsShift == SSR_RT_FLAGS_SHIFT && rt::push::kFlagStats == SSR_RT_FLAG_STATS &&
            rt::push::kFlagReset == SSR_RT_FLAG_RESET && rt::push::kFlagScreen == SSR_RT_FLAG_SCREEN && rt::push::kFlagSingleRay == SSR_RT_FLAG_SINGLE_RAY &&
            rt::push::kFlagSkyMiss == SSR_RT_FLAG_SKY_MISS && rt::push::kFlagScreenProxy == SSR_RT_FLAG_SCREEN_PROXY && rt::push::kFlagProbeMisses == SSR_RT_FLAG_PROBE_MISSES &&
            rt::push::kFlagCars == SSR_RT_FLAG_CARS && (rt::PackConf(0, 0, 1, rt::push::kFlagCars) >> 24) == 128u,
        "the conf word's fields are the shader's");
  // the compute passes take the words the SSR passes read for their second meaning: none is one the compute passes read for the first
  const uint32_t reused[5] = {SSR_W_RT_CONF, SSR_W_RT_CAP, SSR_W_RT_REACH, SSR_W_RT_THRESHOLD, SSR_W_RT_FRAME};
  const uint32_t read_by_compute[] = {SSR_W_DEPTH_RT1, SSR_W_RT2_RT3, SSR_W_SRC, SSR_W_HIZ01, SSR_W_COL01, SSR_W_COL23, SSR_W_RENDER, SSR_W_FLAGS, SSR_W_FRAME,
                                      SSR_W_LIN0, SSR_W_TAN_X, SSR_W_TAN_Y, SSR_W_LIN_X, SSR_W_LIN_Y, SSR_W_LIN_Z, SSR_W_LIN_W, SSR_W_RIGHT, SSR_W_RIGHT + 1, SSR_W_RIGHT + 2,
                                      SSR_W_UP, SSR_W_UP + 1, SSR_W_UP + 2, SSR_W_FALLBACK, SSR_W_MIN_SMOOTH, SSR_W_WET, SSR_W_CAP};
  bool clash = false;
  for (uint32_t a : reused) {
    for (uint32_t b : read_by_compute) clash = clash || a == b;
  }
  Check(!clash, "the words the fallback puts its numbers in are ones the compute passes read for nothing else");
  Check(SSR_W_RT_CONF == SSR_W_HIZ23 && SSR_W_RT_CAP == SSR_W_HIZ45 && SSR_W_RT_REACH == SSR_W_NEAR && SSR_W_RT_THRESHOLD == SSR_W_MAX_DIST && SSR_W_RT_FRAME == SSR_W_THICK,
        "they are the Hi-Z's upper levels' words, the near plane's, the longest ray's and the thickness: the walk's, which a compute pass does not make");
  // PackConf
  const uint32_t word = rt::PackConf(4321, 5, 2, rt::push::kFlagStats | rt::push::kFlagScreen);
  Check((word & 0xFFFF) == 4321 && ((word >> 16) & 15) == 5 && (((word >> 20) & 3) + 1) == 2 && (word >> 24) == 5, "the conf word packs the slot, the groups, the rays and the flags");
  Check(rt::PackConf(0xFFFFFFFFu, 3, 1, 0) == (0xFFFFu | (3u << 16)), "a slot that does not fit is 'none'");
  // the buffer
  Check(rt::buf::kCtlTotal == SSR_RT_CTL_TOTAL && rt::buf::kCtlListed == SSR_RT_CTL_LISTED && rt::buf::kCtlWords == SSR_RT_CTL_WORDS && rt::buf::kStatCal == SSR_RT_STAT_CAL &&
            rt::buf::kStatCensus == SSR_RT_STAT_CENSUS && rt::buf::kStatWords == SSR_RT_STAT_WORDS,
        "the control buffer's regions are the shader's");
  Check(rt::buf::kRays == SSR_RT_CENSUS_RAYS && rt::buf::kCity == SSR_RT_CENSUS_CITY && rt::buf::kCar == SSR_RT_CENSUS_CAR && rt::buf::kMiss == SSR_RT_CENSUS_MISS &&
            rt::buf::kNoData == SSR_RT_CENSUS_NO_DATA && rt::buf::kScreen == SSR_RT_CENSUS_SCREEN && rt::buf::kShaded == SSR_RT_CENSUS_SHADED && rt::buf::kFilled == SSR_RT_CENSUS_FILLED,
        "the census words are the shader's");
  Check(rt::buf::kMissProbed == SSR_RT_CENSUS_MISS_PROBED && rt::buf::kMissBeyond == SSR_RT_CENSUS_MISS_BEYOND && rt::buf::kMissOpenUp == SSR_RT_CENSUS_MISS_OPEN_UP &&
            rt::buf::kNoDataLod == SSR_RT_CENSUS_NO_DATA_LOD && rt::buf::kNoDataDefault == SSR_RT_CENSUS_NO_DATA_DEFAULT && rt::buf::kNoDataMissing == SSR_RT_CENSUS_NO_DATA_MISSING &&
            rt::buf::kProxyRecovered == SSR_RT_CENSUS_PROXY_RECOVERED && rt::buf::kEnvMiss == SSR_RT_CENSUS_ENV_MISS && rt::buf::kCarShaded == SSR_RT_CENSUS_CAR_SHADED,
        "the car fill, miss and no-data details use the same counters on the CPU and GPU");
  Check(rt::buf::kStatCensus - rt::buf::kStatCal == mcla::gpu::rt_hit_cal::kStatsWords, "the calibration's region is the library's counters' size");
  Check(rt::buf::kBytes == 292 && rt::HeldBytes(960, 540) == 960ull * 540 * 2 + 4000000 + 292, "the memory: the confidence image, the list, the control words");
}

// ---------------------------------------------------------------------------
// the governor
// ---------------------------------------------------------------------------

// A pass whose cost is work / 2^step milliseconds (the cost follows the rays), `samples` seconds of 60 FPS frames, the budget as given. Returns the steps taken at every second.
std::vector<uint32_t> Simulate(rt::Governor* g, double work_ms, double budget, double seconds, double t0 = 0.0) {
  std::vector<uint32_t> steps;
  double t = t0;
  for (int frame = 0; frame < int(seconds * 60.0); ++frame, t += 1.0 / 60.0) {
    g->Note(work_ms / double(1u << g->step()));
    g->Update(t, budget);
    if (frame % 60 == 59) steps.push_back(g->step());
  }
  return steps;
}

void TestGovernor() {
  using G = rt::Governor;
  {
    G g;
    const std::vector<uint32_t> steps = Simulate(&g, 0.4, 0.8, 10.0);
    Check(g.step() == 0 && g.changes() == 0, "under the budget nothing changes");
  }
  {
    G g;
    const std::vector<uint32_t> steps = Simulate(&g, 3.0, 0.8, 12.0);
    // 3.0 ms of work against 0.8: step 2 costs 0.75, fits; it must stop there
    Check(g.step() == 2, Num("a cost of 3.0 against a budget of 0.8 settles at step 2 (took %.0f)", g.step()));
    Check(steps.front() >= 1, "it reacts within the first second");
    Check(g.changes() == 2, Num("two steps, no oscillation (%.0f changes)", g.changes()));
  }
  {
    G g;
    Simulate(&g, 100.0, 0.8, 20.0);
    Check(g.step() == G::kMaxStep, "a hopeless cost stops at the last step (an eighth)");
  }
  {
    // grows back when the load goes away, one step at a time, and not before the hold after the last change is over
    G g;
    double t = 0.0;
    double last_change = 0.0;
    for (int frame = 0; frame < 60 * 8; ++frame, t += 1.0 / 60.0) {
      g.Note(3.0 / double(1u << g.step()));
      if (g.Update(t, 0.8)) last_change = t;
    }
    const uint32_t before = g.step();
    double first_regrow = -1.0;
    for (int frame = 0; frame < 60 * 12; ++frame, t += 1.0 / 60.0) {
      g.Note(0.3 / double(1u << g.step()));   // the load goes: 0.3 ms of work
      if (g.Update(t, 0.8) && first_regrow < 0.0) first_regrow = t;
    }
    Check(before == 2 && g.step() == 0, Num("it grows back to the full step when the load goes (from %.0f to %.0f)", before, g.step()));
    Check(first_regrow >= last_change + G::kHoldSeconds - 1e-9, Num("the first regrowth waits for the hold of two seconds after the last change (%.2f s after it)", first_regrow - last_change));
  }
  {
    // a change right before the load goes: the hold applies from the change
    G g;
    double t = 0.0;
    double drop = -1.0;
    for (int frame = 0; frame < 60 * 4 && drop < 0.0; ++frame, t += 1.0 / 60.0) {
      g.Note(1.6);
      if (g.Update(t, 0.8)) drop = t;
    }
    double regrow = -1.0;
    for (int frame = 0; frame < 60 * 6 && regrow < 0.0; ++frame, t += 1.0 / 60.0) {
      g.Note(0.1);
      if (g.Update(t, 0.8)) regrow = t;
    }
    Check(drop > 0.0 && regrow >= drop + 2.0 - 1e-9 && regrow < drop + 3.0, Num("dropped at %.2f s, grown back no sooner than 2 s later (%.2f s)", drop, regrow - drop));
  }
  {
    // a cost that would not fit one step up does not grow back (no oscillation at the edge)
    G g;
    Simulate(&g, 1.2, 0.8, 6.0);   // step 1 costs 0.6
    const uint32_t settled = g.step();
    const uint32_t changes = g.changes();
    Simulate(&g, 1.2, 0.8, 30.0, 6.0);
    Check(settled == 1 && g.step() == 1 && g.changes() == changes, "at the edge (the full step costs 1.2 against 0.8) it stays at step 1 and does not flap");
  }
  {
    // noise around the budget: the average decides, not one frame
    G g;
    Lcg rng;
    double t = 0.0;
    for (int frame = 0; frame < 60 * 20; ++frame, t += 1.0 / 60.0) {
      g.Note(0.4 + (rng.Next() < 0.02f ? 3.0 : 0.0));   // a spike now and then (one frame in fifty), the mean about 0.46
      g.Update(t, 0.8);
    }
    Check(g.step() == 0, "an occasional spike does not drop the cap");
  }
  {
    // one frame that took twenty times as long (a stall: a shader compile, a texture upload) is over the budget at one decision at most and the cap does not move
    G g;
    double t = 0.0;
    for (int frame = 0; frame < 60 * 8; ++frame, t += 1.0 / 60.0) {
      g.Note(frame == 200 ? 20.0 : 0.4);
      g.Update(t, 0.8);
    }
    Check(g.step() == 0 && g.changes() == 0, "a single stall does not drop the cap (two decisions over the budget are needed)");
  }
  {
    G g;
    g.Note(-1.0);
    g.Note(std::nan(""));
    Check(!g.has_samples(), "a bad sample is ignored");
    Check(!g.Update(5.0, 0.8), "no samples, no decision");
    g.Note(5.0);
    Check(!g.Update(5.0, 0.8), "too few samples, no decision");
    g.Reset();
    Check(g.step() == 0 && !g.has_samples() && g.changes() == 0, "reset forgets everything");
    for (int i = 0; i < 20; ++i) g.Note(2.0);
    Check(!g.Update(1.0, 0.0), "a budget of nothing decides nothing");
  }
}

// ---------------------------------------------------------------------------
// the counters and the census
// ---------------------------------------------------------------------------

void TestCensus() {
  const uint32_t now[3] = {5u, 0u, 10u};
  const uint32_t last[3] = {0xFFFFFFF0u, 0u, 4u};
  uint32_t d[3] = {};
  rt::Delta(now, last, 3, d);
  Check(d[0] == 21u && d[1] == 0u && d[2] == 6u, "the counters' difference wraps at 2^32");
  rt::FrameLine f;
  f.frame = 1200;
  f.level = rt::Level::kUltra;
  f.group_mask = rt::kUltraCell.group_mask;
  f.reach = 300.0f;
  f.capacity = 500000;
  f.step = 0;
  f.rays_per_pixel = 2;
  f.candidates = 120400;
  f.listed = 120400;
  f.keep = 1.0f;
  f.rays = 240800;
  f.city = 91000;
  f.car = 2100;
  f.miss = 147700;
  f.no_data = 0;
  f.screen = 30000;
  f.shaded = 61000;
  f.filled = 83000;
  f.ms = 0.42;
  f.budget_ms = 1.8f;
  const std::string line = rt::FormatFrameLine(f);
  if (g_print) std::printf("%s\n", line.c_str());
  Check(line.rfind("[ssr rt] frame 1200: level ultra, groups glass+metal+water+road+sidewalk+tunnels, reach 300 m, capacity 500000 px at step 0 (2 rays), candidates 120400 listed 120400 keep 1.00, rays 240800 city 91000 car 2100 miss 147700 no data 0 screen 30000 shaded 61000 filled 83000, 0.42 ms (budget 1.80)", 0) == 0,
        "the frame line the chains read: " + line);
  f.ms = -1.0;
  f.rays_per_pixel = 1;
  f.level = rt::Level::kHigh;
  f.group_mask = rt::kHighCell.group_mask;
  const std::string untimed = rt::FormatFrameLine(f);
  Check(Has(untimed, "level high, groups glass+metal+water+road+sidewalk+tunnels, ") && Has(untimed, "(1 ray)") && Has(untimed, "no timing (budget 1.80)"), "a line without timing says so; one ray is singular");
  rt::Totals t;
  Check(rt::FormatTotals(t).empty(), "a fallback that never was wanted adds nothing to the census");
  t.frames_run = 900;
  t.frames_not_run = 12;
  t.stats_frames = 45;
  t.candidates = 5000000;
  t.rays = 4000000;
  t.step = 1;
  t.step_changes = 3;
  t.ms = 0.31;
  t.last_why = rt::Why::kNoScene;
  const std::string text = rt::FormatTotals(t);
  Check(text.rfind("; ssr rt: 900 frames ran, 12 wanted and did not (last: no ray tracing top level in this frame), 45 stats frames (candidates 5000000, rays 4000000, ", 0) == 0 && Has(text, "governor step 1 (3 changes), 0.31 ms"),
        "the census text: " + text);
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

void TestShaderSources(const std::string& dir) {
  const std::string hl = dir + "/hlsl/";
  std::string common, select, trace, trace_ps, ssr_common;
  if (!Slurp(hl + "ssr_rt_common.hlsli", &common) || !Slurp(hl + "ssr_rt_select_cs.hlsl", &select) || !Slurp(hl + "ssr_rt_trace_cs.hlsl", &trace) || !Slurp(hl + "ssr_trace_ps.hlsl", &trace_ps) ||
      !Slurp(hl + "ssr_common.hlsli", &ssr_common)) {
    Check(false, "the fallback's shaders are in " + hl);
    return;
  }
  // the numbers the header claims are the .hlsli's #defines
  Check(DefineOf(common, "SSR_W_RT_CONF") == rt::push::kConf && DefineOf(common, "SSR_W_RT_CAP") == rt::push::kCap && DefineOf(common, "SSR_W_RT_REACH") == rt::push::kReach &&
            DefineOf(common, "SSR_W_RT_THRESHOLD") == rt::push::kThreshold && DefineOf(common, "SSR_W_RT_FRAME") == rt::push::kFrame && DefineOf(common, "SSR_RT_STAT_WORDS") == rt::buf::kStatWords,
        "the text of ssr_rt_common.hlsli holds the header's words");
  // one ray: the trace and the fallback take it from the same function
  Check(Count(ssr_common, "float3 SsrSampleRay(") == 1 && Has(trace_ps, "SsrSampleRay(n, to_camera, alpha, pix.x, pix.y, frame, ray, pattern_rays)") &&
            Has(trace, "pattern_rays = (stable || single_ray || handoff || classified) ? SsrRays() : rays") && Has(trace, "SsrSampleRay(r.n, r.to_camera, r.alpha, pix.x, pix.y, frame, k, pattern_rays)") &&
            Has(trace, "weight *= SsrRtHistoryEdge(confidence, SsrF(SSR_W_RT_THRESHOLD))"),
        "native fallback regenerates SSR rays even when it budgets fewer, and fades at the candidate cutoff");
  Check(!Has(trace_ps, "SsrSampleReflection("), "the trace has no copy of the ray's formula left");
  // the identity of the plain compilations: the trace's plain entry and everything the confidence adds is under SSR_RT
  Check(Has(trace_ps, "#ifndef SSR_RT\n#define SSR_RT 0\n#endif") && Has(trace_ps, "float4 main(float4 position : SV_Position) : SV_Target {\n  return SsrTraceBody(position);\n}") &&
            Has(trace_ps, "#if SSR_RT\n  float2 confidence : SV_Target1;\n#endif") && Has(trace_ps, "#if SSR_RT\n  o.confidence = g_confidence;\n#endif") &&
            Count(trace_ps, "g_confidence") == 3 && Count(trace_ps, "conf_sum") == 4 && Count(trace_ps, "rt_group") == 2,
        "the confidence target's code is all under #if SSR_RT (the plain compilations hold none of it)");
  Check(Has(trace_ps, "SV_Target0") && Has(trace_ps, "SV_Target1") &&
            Has(trace_ps, "SsrRtGroupOut(rt_group, SsrRtFirstRayShare(first_confidence, conf_sum, rays)), saturate(conf_sum / divisor)"),
        "the confidence target holds the group and first ray's residual share in R, with mean confidence in G");
  // the select pass
  Check(Has(select, "SSR_RT_FLAG_RESET") && Has(select, "SsrRtTileOf(") && Has(select, "SsrRtKeep(capacity") && Has(select, "SsrRtThinHash(pix.x, pix.y, rt_frame, (SsrW(SSR_W_FRAME) & SSR_FRAME_LEGACY_NOISE) != 0u)") && Has(select, "InterlockedAdd(gs_total") && Has(select, "InterlockedAdd(gs_kept") &&
            Has(select, "(rt_frame + 2u) % 3u") && Has(select, "(rt_frame + 1u) % 3u"),
        "the select pass: the reset frame, the tile order, the share kept from the last frame's total, the group's one reservation, the three rotating counters");
  Check(Has(select, "[numthreads(8, 8, 1)]") && Has(trace, "[numthreads(64, 1, 1)]") && int(rt::kTile) == 8 && int(rt::kThreads) == 64, "the groups' sizes are the host's");
  Check(Has(select, "if (index < capacity) {"), "an entry past the capacity is dropped, never written");
  // the trace pass
  Check(Has(trace, "#define CAR_RT_SPACE 6") && Has(trace, "#define RT_WORLD_SPACE 6") && Has(trace, "#define CITY_HIT_SPACE 6") && Has(trace, "register(u0, space5)") && Has(trace, "register(u1, space5)") &&
            Has(trace, "register(u2, space5)") && Has(select, "register(u1, space5)") && Has(select, "register(u2, space5)"),
        "the layout's spaces: the textures 0 to 3, the samplers and the push block 4, the output set 5, the ray tracing set 6");
  Check(Has(trace, "CityHitResolve(p, f, s, ray, hit.inst, ground)") && Has(trace, "CityHitTrace(origin, dir, 0.0f, reach, kSsrRtMask") && Has(trace, "RtWorldLoad()") && Has(trace, "RT_WORLD_VALID") && Has(trace, "CITY_HIT_GAIN(cls) RtWorldClassGain(cls)"),
        "the trace pass resolves its hits with the city hit library in the world frame's light, with the world frame's class gains");
  Check(Has(trace, "const bool ground = SsrRtGroupOf(r.cls) == kSsrRtGroupGround;") &&
            Has(trace, "if (ground && (flags & SSR_RT_FLAG_SCREEN) == 0u)") &&
            Has(trace, "if (res.source == CITY_HIT_SOURCE_NONE || coverage <= 0.0f) {\n      if (res.source == CITY_HIT_SOURCE_NONE) {") && Has(trace, "if (try_proxy && (lod || placeholder))") &&
            Has(trace, "SsrRtCount(stats, SSR_RT_CENSUS_NO_DATA);\n      continue;"),
        "ground hits require screen support or an opted-in raster proxy, and rejected rays leave the radiance and hit distance alone");
  Check(Has(trace, "SsrRtHitCoverage(ground, res.screen)") && Has(trace, "coverage * SsrRtReachFade(hit.t, reach)") &&
            Has(trace, "if (!ground || fade >= 0.5f)") && Has(trace, "rt_distance && nearest < kSsrMissLength"),
        "fade replaces both ground radiance and the matching planar share; weak hits keep the old distance");
  Check(Has(trace, "&& has_planar && (f.flags & RT_HIT_FRAME_ENV) != 0u") && Has(trace, "hit_coverage * rt_weight_scale > confidence"),
        "replacing the planar view needs both sources, and environment misses do not replace the hit distance");
  Check(Has(trace, "NgFinite3(current.xyz + sum * rt_weight_scale)") && Has(trace, "SsrPackHit(clamp(nearest, 0.02f, 60000.0f), false, 0u)"), "the result is the blend, and the hit distance goes into the trace's alpha packed as the trace's own");
  Check(Has(trace, "const bool shade_cars = SsrRtCarReceiver(flags, r.cls);") && Has(trace, "if (!CityHitIsCity(hit.inst.instanceId))") &&
            Has(trace, "SsrRtCount(stats, SSR_RT_CENSUS_CAR);") && Has(trace, "if (shade_cars)") && Has(trace, "SsrRtCount(stats, SSR_RT_CENSUS_CAR_SHADED);"),
        "car hits are counted and shade only on enabled road receivers");
  Check(Has(trace, "CarMatLoad(hit.instanceIndex)") && Has(trace, "m.cls == CAR_MAT_CLASS_NONE || m.cls > CAR_MAT_CLASS_OTHER") &&
            Has(trace, "CarRtHitNormal(hit.instanceIndex, hit.inst.primitiveIndex, hit.inst.barycentrics, transform, direction)") &&
            Has(trace, "CarMatAlbedo(m, hit.instanceIndex, hit.inst.primitiveIndex, hit.inst.barycentrics, footprint, nv)") &&
            Has(trace, "CarMatFresnel(m, nv) * kClassSpecGain[m.cls]") && Has(trace, "m.emissive * kRtHitEmissiveGain * CarMatGlowScale(m, w.lampFactor)"),
        "car shading uses the actual hit's normal, textured material, roughness class and lens radiance");
  Check(Has(trace, "RtHitCascadeVisible(f, position, normal)") && Has(trace, "i < kSsrRtCarLampVisits && cursor.done == 0u") &&
            Has(trace, "lamp.radiance * SsrRtCarLampWeight(lamp.distance)") && Has(trace, "colour = NgHeld3(result * p.units, kCityHitMaxColour);") &&
            Count(trace, "CityHitTrace(") == 2 && !Has(trace, "RayQuery<"),
        "car hits use a bounded lamp lookup and cascades without adding a ray query");
  Check(Has(trace, "if (fade > 0.0f && SsrRtShadeCar(") && Has(trace, "sum += (min(colour, float3(cap, cap, cap)) - fallback) * (fade * ray_share);") &&
            Has(trace, "rt_id = kReflHitRtCarShaded;"),
        "a shaded car uses the fallback fade, subtracts the planar share and enters the existing history distance path");
  Check(Has(trace, "register(u3, space5)") && Has(trace, "ReflHitPackHandoff(visible, rt_id >= kReflHitRtCity ? rt_id : rejected, screen_id, native, screen_fraction)"),
        "the capture pass stores discarded query classes separately from the visible source");
  Check(Has(trace, "CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE") && Has(trace, "CITY_HIT_P_CALIBRATE") && Has(trace, "CITY_HIT_STAT_ADD(word, value) g_ctl.InterlockedAdd((SSR_RT_STAT_CAL + (word)) * 4u, (value))"),
        "the hit's flags are the car reflections' (the sun's ray, glow) and a stats frame calibrates through the control buffer's region");
  Check(Has(trace, "SsrRtLift(r.pc.z,") && Has(trace, "SsrRtReachFade(hit.t, reach)") && Has(trace, "SsrRtFootprint(pixel_angle, receiver_distance, hit.t, cone_tan)"), "the ray's lift, the hit's fade and the footprint are the twins'");
}


void TestHostSources(const std::string& dir) {
  std::string rt_cpp, rt_h, ssr_cpp, ssr_h, passes_cpp, passes_h, cmake, shaders_h, renderer_cpp, settings, order_h;
  if (!Slurp(dir + "/ssr_rt.cpp", &rt_cpp) || !Slurp(dir + "/ssr_rt.h", &rt_h) || !Slurp(dir + "/ssr.cpp", &ssr_cpp) || !Slurp(dir + "/ssr.h", &ssr_h) || !Slurp(dir + "/host_passes.cpp", &passes_cpp) ||
      !Slurp(dir + "/host_passes.h", &passes_h) || !Slurp(dir + "/CMakeLists.txt", &cmake) || !Slurp(dir + "/host_shaders.h", &shaders_h) || !Slurp(dir + "/renderer.cpp", &renderer_cpp) ||
      !Slurp(dir + "/../settings/generated/settings_gpu.h", &settings) || !Slurp(dir + "/frame_order.h", &order_h)) {
    Check(false, "the host sources are in " + dir);
    return;
  }
  // the layout: the SSR host pass layout (four texture sets, the samplers), the output set at 5, the car scene's set at 6, the push block at (b0, space4) for the compute stage
  Check(Has(rt_cpp, "builder.addDescriptorSet(heap_->texture_set_builder());") && Has(rt_cpp, "for (uint32_t set = 0; set < 4; ++set)") && Has(rt_cpp, "sampler_set != 4 || output_set != 5 || scene_set != 6") &&
            Has(rt_cpp, "builder.addPushConstant(0, 4, sizeof(ssr::Push), plume::RenderShaderStageFlag::COMPUTE)") && Has(rt_cpp, "builder.addDescriptorSet(scene.SetDesc())"),
        "the compute layout: four texture sets, the samplers at 4, the output set at 5, the car scene's ray tracing set at 6, the push block at (b0, space4)");
  Check(Has(rt_cpp, "list->setComputeDescriptorSet(hit ? capture_set_.get() : set_.get(), 5);") && Has(rt_cpp, "list->setComputeDescriptorSet(view.set, 6);") && Has(rt_cpp, "list->setComputeDescriptorSet(heap_->sampler_set(), 4);"), "the sets are bound at the indices the shaders declare");
  // D3D12 sizes a descriptor set by its view count: the literal is the builder's ranges (u0, u1, u2)
  Check(Count(rt_cpp, "output_builder_.addReadWrite") == 3 && Has(rt_cpp, "CreateDescriptorSet(output_builder_, 3, 0, \"ssr rt output set\")"),
        "the output set is made with the three views its builder describes (D3D12 counts them)");
  Check(Count(rt_cpp, "capture_builder_.addReadWrite") == 4 && Has(rt_cpp, "CreateDescriptorSet(capture_builder_, 4, 0, \"ssr rt capture output\")") &&
            Has(rt_cpp, "capture_set_->setTexture(3, hit.texture.get()"), "the capture output set adds the hit ID image at u3");
  Check(Has(rt_cpp, "set_->setTexture(0, trace.texture.get(), plume::RenderTextureLayout::GENERAL, trace.view.get());") && Has(rt_cpp, "set_->setBuffer(1, ctl_.get()") && Has(rt_cpp, "set_->setBuffer(2, list_.get()"),
        "u0 is the trace image, u1 the control words, u2 the list");
  // the pass: the barriers and the order of the dispatches
  const size_t gen = At(rt_cpp, "const plume::RenderTextureBarrier write(in.trace->texture.get(), Layout::GENERAL);");
  const size_t select = At(rt_cpp, "list->setPipeline(select_pipeline);");
  const size_t first_dispatch = At(rt_cpp, "list->dispatch(tiles_x, tiles_y, 1);");
  const size_t trace_pipe = At(rt_cpp, "list->setPipeline(trace_pipeline);");
  const size_t trace_dispatch = At(rt_cpp, "list->dispatch(ssr_rt::GroupsFor(capacity), 1, 1);");
  const size_t back = At(rt_cpp, "const plume::RenderTextureBarrier read(in.trace->texture.get(), Layout::SHADER_READ);");
  Check(gen != std::string::npos && gen < select && select < first_dispatch && first_dispatch < trace_pipe && trace_pipe < trace_dispatch && trace_dispatch < back,
        "the trace image goes to a storage layout, the select pass runs, then the trace pass, then the image goes back to a shader read");
  Check(Count(rt_cpp, "list->barriers(Stage::COMPUTE, between, 2);") >= 2 && Has(rt_cpp, "list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, &read, 1);"),
        "the control words and the list are barriered between the passes and the trace image is readable by the graphics passes after");
  Check(Has(rt_cpp, "reset_pending_") && Has(rt_cpp, "pk::kFlagReset") && Has(rt_cpp, "ssr_rt::GroupsFor(capacity)") && Has(rt_cpp, "ssr_rt::CapacityFor(cell, 0, rays)") && Has(rt_cpp, "ssr_rt::RaysUsed(cell,"),
        "the first frame clears the control words; the dispatch is the capacity's, the capacity is the fixed level budget, the rays the trace's own");
  const size_t indirect = At(rt_cpp, "in.indirect->Dispatch(*list);");
  Check(select < indirect && indirect < trace_pipe && Has(rt_cpp, "reset_pending_ || in.indirect") &&
            Has(rt_cpp, "reset.Set(pk::kCap, 0);"),
        "the indirect list is selected before tracing and clears its counters even when empty");
  Check(!Has(rt_cpp, "governor_.Update(") && Has(rt_cpp, "ms_ = ms") && Has(rt_cpp, "list->writeTimestamp(timer_->pools[timer_slot].get(), 0);") && Has(rt_cpp, "list->writeTimestamp(timer_->pools[timer_slot].get(), 1);") &&
            Has(rt_cpp, "frames_->RetireAction("),
        "pass timings come from two timestamps round the passes, read once the list has completed");
  Check(Has(rt_cpp, "world_->AddCalibration(delta.data() + ssr_rt::buf::kStatCal)") && Has(rt_cpp, "world_->StatsFrame()"), "a stats frame's calibration counters go to the world frame's one window");
  Check(Has(rt_cpp, "service_->Want(rt_scene::Consumer::kSsrFallback, ssr_rt::NeedsFor(resolved_.level, shade_cars_))") && Has(rt_cpp, "if (wanted() && service_) {") &&
            Has(rt_cpp, "ssr_rt::CarHitsEnabled(now.cell, larecomp::settings::mcla_gpu_ssr_rt_cars.Get())") && Has(rt_cpp, "if (shade_cars_) flags |= pk::kFlagCars;"),
        "the frame's resolved car setting is shared by the scene request and trace flags");
  Check(Has(rt_cpp, "static LiveText text(\"mcla_gpu_ssr_rt\");") && Has(rt_cpp, "ssr_rt::ParseChoice(Setting().Get())") &&
            Has(rt_cpp, "ssr_rt::ResolveFixed(ssr_level, choice, ready_)") &&
            Has(rt_cpp, "ssr_rt::Resolve({ssr_level, choice, gpu_class::CurrentRtRung()})"),
        "the classified path resolves work from the chosen level and ray-query support");
  // the identity: the effect without the fallback
  Check(Has(rt_cpp, "if (!ready_) {\n    resolved_ = ssr_rt::Resolved{};") && Has(rt_cpp, "CarRtSupported(device.capabilities()"), "a device that cannot trace makes none of it and the effect asks for nothing");
  Check(Has(ssr_cpp, "if (!garage && rt_.wanted() && !decision.stub) {") && Has(ssr_cpp, "rt = trace_rt_pipeline != nullptr && rt_.PipelinesReady(ms);") && Has(ssr_cpp, "EnsureImages(width, height, rt, raw, sf.render[0], sf.render[1])") &&
            Has(ssr_cpp, "rt ? kTraceRtFlags : plume::RenderTextureFlag::RENDER_TARGET") && Has(ssr_cpp, "} else {\n    DrawPass(list, trace_, trace_pipeline, trace_push, classify);\n  }"),
        "without the fallback the images are the old ones (default flags, no confidence) and the pass is the plain one");
  Check(Has(ssr_cpp, "rt_.BeginFrame(garage ? ssr::Level::kOff : level_, classify_enabled_);") && Has(ssr_cpp, "rt_.OnPresent();") && Has(ssr_cpp, "rt_.Shutdown();") &&
            Has(ssr_cpp, "return bytes + classifier_.HeldBytes() + rt_.HeldBytes() + denoise_.HeldBytes();"),
        "the effect's frame start, present, shutdown and memory call the fallback");
  const size_t m_trace = At(ssr_cpp, "Mark(list, \"ssr trace\")");
  const size_t m_rt = At(ssr_cpp, "Mark(list, \"ssr rt\")");
  const size_t m_temporal = At(ssr_cpp, "Mark(list, \"ssr temporal\")");
  Check(m_trace < m_rt && m_rt < m_temporal && Count(ssr_cpp, "\"ssr rt\"") == 1 && Count(rt_cpp, "\"ssr rt\"") <= 2, "the label 'ssr rt' stands between the trace's and the accumulation's marks, written once");
  Check(Has(order_h, "{{\"ssr rt\", \"ssr temporal\", \"ssr denoise\", \"ssr apply\"}}") && Has(order_h, ".soft_reads = data::kCarStructures"), "the frame order table has the label and the soft read of the top level");
  // the MRT pipeline and its host pass
  Check(Has(passes_h, "const plume::RenderFormat* formats,") && Has(passes_cpp, "desc.renderTargetCount = count;") && Has(passes_cpp, "count < 2"), "the host passes make a pipeline over several targets");
  Check(Has(ssr_cpp, "const plume::RenderFormat formats[2] = {kFormat, kConfidenceFormat};") && Has(ssr_cpp, "kConfidenceFormat = plume::RenderFormat::R8G8_UNORM") && Has(ssr_cpp, "plume::RenderFramebufferDesc(attachments, 2)"),
        "the trace pass with two targets: RGBA16F and R8G8_UNORM, one framebuffer over both");
  Check(Has(ssr_cpp, "first_use[i] = images[i]->layout == Layout::UNKNOWN;") && Has(ssr_cpp, "if (first_use[i]) list->clearColor(i,"),
        "every trace target is cleared on its first use");
  // the build and the blobs
  Check(Has(cmake, "ssr_rt.cpp") && Has(cmake, "larecomp_gpu_host_shader(ssr_trace_rt_ps ps_6_0 SOURCE ssr_trace_ps DEFINES SSR_MS=0 SSR_RT=1)") && Has(cmake, "larecomp_gpu_host_shader(ssr_trace_rt_ms_ps ps_6_0 SOURCE ssr_trace_ps DEFINES SSR_MS=1 SSR_RT=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(ssr_rt_select_cs cs_6_0)") && Has(cmake, "larecomp_gpu_host_shader_rt(ssr_rt_trace_cs cs_6_5 DEFINES SSR_MS=0)") &&
            Has(cmake, "larecomp_gpu_host_shader_rt(ssr_rt_trace_ms_cs cs_6_5 SOURCE ssr_rt_trace_cs DEFINES SSR_MS=1)") && Has(cmake, "add_executable(larecomp_ssr_rt_rules_test EXCLUDE_FROM_ALL ssr_rt_rules_test.cpp)") &&
            Has(cmake, "larecomp_gpu_host_shader(ssr_trace_ps ps_6_0 DEFINES SSR_MS=0)"),
        "the build lists the fallback's source, its five shaders (the trace with SSR_RT 1 twice, the select pass, the trace pass twice) and the test; the plain trace's line is unchanged");
  Check(Has(shaders_h, "case 5: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ps);") && Has(shaders_h, "case 6: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ms_ps);") && Has(shaders_h, "case 0: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_ps);") &&
            Has(shaders_h, "ScreenReflectionsRtShader(uint32_t pass)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(ssr_rt_trace_ms_cs)") && Count(shaders_h, "ssr_rt_select_cs.hlsl.dxil.h") == 1 && Count(shaders_h, "ssr_rt_select_cs.hlsl.spirv.h") == 1 &&
            Count(shaders_h, "ssr_trace_rt_ms_ps.hlsl.dxil.h") == 1 && Count(shaders_h, "ssr_trace_rt_ps.hlsl.spirv.h") == 1,
        "the blobs: the plain numbering is unchanged, the new passes come after it");
  Check(Has(ssr_h, "kShaderTraceRt, kShaderTraceRtMs, kShaderApplyRaw, kShaderHit, kShaderCount") && Has(ssr_h, "kVTraceRt = kVApplyBase + 8, kVTraceRtMs, kVApplyRaw, kVHit, kVariantCount") && Has(ssr_h, "ScreenReflectionsRt rt_;") && Has(ssr_h, "InitializeRt(") && Has(ssr_h, "RtCensusText()"),
        "the effect's header has the new shaders, variants, the fallback and its two calls");
  // the renderer's wiring
  const size_t shadow_init = At(renderer_cpp, "city_shadow_.Initialize(device_, frames_, heap_, passes_, pass_timing_, &car_rt_, &city_rt_, &rt_scene_, /*listed_at_start=*/true);");
  const size_t rt_init = At(renderer_cpp, "ssr_.InitializeRt(device_, frames_, heap_, passes_, pass_timing_, &car_rt_, &rt_scene_, &rt_world_);");
  const size_t precache_start = At(renderer_cpp, "passes_.precache().Start();");
  Check(shadow_init != std::string::npos && shadow_init < rt_init && rt_init < precache_start && Has(renderer_cpp, "rt_world_.CensusText() + ssr_.RtCensusText()"),
        "the renderer starts the fallback after the car scene's, the scene service's and the world frame's start, before the precache, and prints its census with the city's");
  // the setting
  const auto rt_setting = setting_source::Find(dir, rt_cpp, "mcla_gpu_ssr_rt");
  Check(rt_setting.found && !rt_setting.debug_only && rt_setting.live && rt_setting.default_text == "\"off\"",
        "surface RT is a live graphics row and needs an explicit preset or row choice");
  for (const char* name : {"mcla_gpu_ssr_rt_cars", "mcla_gpu_ssr_rt_single_ray",
                           "mcla_gpu_ssr_rt_sky_miss", "mcla_gpu_ssr_rt_screen_proxy"}) {
    const auto setting = setting_source::Find(dir, rt_cpp, name);
    Check(setting.found && setting.debug_only && setting.live && setting.default_text == "true",
          std::string(name) + " is live, debug-only and on by default");
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string src_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--print") {
      g_print = true;
    } else if (a[0] != '-') {
      src_dir = a;
    }
  }
  TestGroups();
  TestConfidenceCodec();
  TestFallbackCoverage();
  TestCarHits();
  TestCap();
  TestRay();
  TestRayIsTheTraces();
  TestSelectModel();
  TestThinNoise();
  TestDecision();
  TestWords();
  TestGovernor();
  TestCensus();
  if (!src_dir.empty()) {
    TestShaderSources(src_dir);
    TestHostSources(src_dir);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  (void)Count;
  (void)At;
  return g_failed == 0 ? 0 : 1;
}
