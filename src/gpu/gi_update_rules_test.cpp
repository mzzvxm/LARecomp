// Check of the probe update's rules (gi_update_rules.h and hlsl/gi_update_common.hlsli): CITY-RT M3 package G2. A program of its own: it needs no renderer, no game files and no GPU, and exits 0 when every
// check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_update_rules_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_update_rules_test.cpp
//   run: larecomp_gi_update_rules_test [-v] [<src/gpu dir>]   (without the path the shaders and the sources are found from this file's own place; -v prints every check with its numbers)
//
// It checks: the .hlsli compiled as C++ (through hlsl_cpp_shim.h) against the twin on random inputs (the hash, the seed, the Fibonacci set, the rotation, the probe word, the ray record, the octahedral
// texels, the integration of a texel against gi_frame.h's IntegrateTexel, the classification, the hysteresis, the blend, the histogram) and every #define against the header; the half precision
// conversion by its definition; the ray set (uniform over the sphere, a closed environment integrates to its radiance, the rotation is a rotation and new every update); the classification on analytic
// scenes (a probe under the ground, in a box, above the ground, outside a box, close to a surface); the blend (the running mean while a probe is young, the step response with and without the jump, noise
// that is not a jump, a real change that is); the scheduler on the field's real windows and dirty sets (the warm-up, the steady state's coverage, the governor's scales, the safe range, the stale boxes, a move's
// exposed probes, the order); the governor; the budgets of the cells; the counters' decode and the four census lines against the chain's regexes; and, given the source folder, the shaders' registers, the
// CMake lines, host_shaders.h and the wiring in the renderer, the integration and the city scene.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gi_field_rules.h"
#include "gi_frame.h"
#include "gi_update_rules.h"
#include "hlsl_cpp_shim.h"

namespace gi = mcla::gpu::gi;
namespace gf = mcla::gpu::gi_field;
namespace gu = mcla::gpu::gi_update;

// ---------------------------------------------------------------------------
// the .hlsli as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
inline uint f32tof16(float v) { return mcla::gpu::gi_update::FloatToHalf(v); }
inline uint asuint(float v) { return std::bit_cast<uint>(v); }
float3 g_dir[64];
float3 g_radiance[64];
float g_distance[64];
bool g_escaped[64];
#define GIU_RAY_DIR(i) (hlsl_shim::g_dir[(i)])
#define GIU_RAY_RADIANCE(i) (hlsl_shim::g_radiance[(i)])
#define GIU_RAY_DISTANCE(i) (hlsl_shim::g_distance[(i)])
#define GIU_RAY_ESCAPED(i) (hlsl_shim::g_escaped[(i)])
#include "hlsl/gi_update_common.hlsli"
}  // namespace hlsl_shim

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  } else if (g_verbose) {
    std::printf("  ok: %s\n", what.c_str());
  }
}

std::string Num(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), fmt, a, b, c, d);
  return buffer;
}

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 12345) {}
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double Uniform() { return double(Next() >> 11) / double(1ull << 53); }
  double Range(double lo, double hi) { return lo + (hi - lo) * Uniform(); }
  int32_t Int(int32_t lo, int32_t hi) { return lo + int32_t(Next() % uint64_t(hi - lo + 1)); }
  uint32_t U32() { return uint32_t(Next() >> 16); }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }
int Count(const std::string& text, const std::string& needle) {
  int n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

gi::Vec3 V(hlsl_shim::float3 v) { return {v.x, v.y, v.z}; }
float Dist(gi::Vec3 a, gi::Vec3 b) { return std::sqrt(gi::Dot(a - b, a - b)); }
float Len(gi::Vec3 a) { return std::sqrt(gi::Dot(a, a)); }

// ---------------------------------------------------------------------------
// the numbers: every #define against the header
// ---------------------------------------------------------------------------
void TestNumbers() {
  Check(GIU_FORMAT_VERSION == int(gu::kFormatVersion), "the format version");
  Check(uint32_t(GIU_TRACE_GROUP) == gu::kTraceGroup && uint32_t(GIU_GROUPS_X) == gu::kGroupsX && uint32_t(GIU_RAY_BYTES) == gu::kRayBytes && uint32_t(GIU_RAY_WORDS) * 4 == gu::kRayBytes, "the groups and the record");
  Check(float(GIU_MAX_RADIANCE) == gu::kMaxRadiance && float(GIU_TWO_PI) == gu::kTwoPi && float(GIU_GOLDEN) == gu::kGolden, "the radiance clamp, two pi, the golden ratio");
  Check(float(GIU_MOMENTS_LOBE_POWER) == gi::kMomentsLobePower && float(GIU_RAY_SCALE) == gi::kRayScale, "the field's lobe power and ray scale are gi_frame.h's");
  Check(uint32_t(GIU_RAY_HIT) == gu::kRayHit && uint32_t(GIU_RAY_BACK) == gu::kRayBack && uint32_t(GIU_RAY_LOD) == gu::kRayLod && uint32_t(GIU_RAY_SKIPPED) == gu::kRaySkipped, "the ray flags");
  Check(uint32_t(GIU_PROBE_DIRTY) == gu::kProbeDirty && uint32_t(GIU_PROBE_WORD_FLAGS_SHIFT) == gu::kProbeFlagsShift && uint32_t(GIU_PROBE_WORD_INDEX_SHIFT) == gu::kProbeIndexShift, "the probe word's fields");
  Check(float(GIU_INSIDE_BACK_SHARE) == gu::kInsideBackShare && float(GIU_INSIDE_NEAREST_SPACINGS) == gu::kInsideNearestSpacings && uint32_t(GIU_INSIDE_RETEST_MASK) == gu::kInsideRetestMask, "the classification's numbers");
  Check(float(GIU_JUMP_RELATIVE) == gu::kJumpRelative && float(GIU_JUMP_FLOOR) == gu::kJumpFloor && float(GIU_JUMP_SIGMAS) == gu::kJumpSigmas && float(GIU_CHANGED_STAT) == gu::kChangedStat, "the jump's numbers");
  Check(uint32_t(GIU_CONVERGED_UPDATES) == gu::kConvergedUpdates && float(GIU_FIXED_SCALE) == gu::kFixedScale && float(GIU_RAY_FIXED) == gu::kRayFixed && uint32_t(GIU_STAT_HIST_BINS) == gu::kHistBins,
        "the census' numbers");
  // the fixed point sums cannot overflow an unsigned word: 64 rays at the radiance clamp, 64 texels at it
  const double ray_sum = double(gu::kMaxRadiance) * gu::kRayFixed * 64.0;
  const double ray_sq = double(gu::kMaxRadiance) * gu::kMaxRadiance * gu::kRayFixed * 64.0;
  const double texel_sum = double(gu::kMaxRadiance) * gu::kFixedScale * 64.0;
  Check(ray_sum < 4.0e9 && ray_sq < 4.0e9 && texel_sum < 4.0e9, Num("the group's fixed point sums fit 32 bits (%.0f, %.0f, %.0f of 4.29e9)", ray_sum, ray_sq, texel_sum));
  // the field's state layout the blend writes: the frame's low 14 bits go in the bits 18..31 the reset clears
  const uint32_t state = gu::StateAfterBlend(gf::MakeState(5, false, false, 0), false, 77, 0x12345);
  Check(gf::StateUpdates(state) == 6 && !gf::StateInside(state) && !gf::StateNew(state) && gf::StateBack(state) == 77 && (state >> 18) == (0x12345u & 0x3FFu), "the state after a blend: count up, back share, frame bits");
  const uint32_t inside = gu::StateAfterBlend(gf::MakeState(5, false, true, 0), true, 200, 9);
  Check(gf::StateUpdates(inside) == 5 && gf::StateInside(inside) && gf::StateNew(inside) && gf::StateBack(inside) == 200, "the state after an inside classification: count and new bit kept, inside bit, back share");
  const uint32_t again = gu::StateAfterBlend(inside, false, 10, 10);
  Check(gf::StateUpdates(again) == 6 && !gf::StateInside(again) && !gf::StateNew(again), "an inside probe that is blended again is counted and loses the new bit");
}

// ---------------------------------------------------------------------------
// the half precision conversion, by its definition
// ---------------------------------------------------------------------------
void TestHalf() {
  // every finite half round-trips through the float and back
  int bad = 0;
  for (uint32_t h = 0; h < 0x10000u; ++h) {
    if (((h >> 10) & 31u) == 31u) continue;
    const float f = hlsl_shim::f16tof32(h);
    if (gu::HalfToFloat(h) != f) ++bad;
    if (h != 0x8000u && gu::FloatToHalf(f) != h) ++bad;
  }
  Check(bad == 0, "every finite half converts to the float of its definition and back to itself");
  Rng rng(2);
  double worst = 0;
  for (int i = 0; i < 100000; ++i) {
    const float x = float(rng.Range(0.0, 32.0)) * float(std::pow(10.0, -rng.Range(0, 5)));
    const float y = gu::HalfToFloat(gu::FloatToHalf(x));
    const double allowed = double(x) * std::ldexp(1.0, -11) + std::ldexp(1.0, -25);
    worst = std::max(worst, std::fabs(double(y) - double(x)) / allowed);
  }
  Check(worst <= 1.0001, Num("a float's half is within half an ulp of it (the worst is %.3f of it)", worst));
  Check(gu::FloatToHalf(65520.0f) == 0x7C00u && gu::FloatToHalf(0.0f) == 0 && gu::FloatToHalf(1.0f) == 0x3C00u, "overflow goes to infinity; zero and one");
}

// ---------------------------------------------------------------------------
// the .hlsli against the twin
// ---------------------------------------------------------------------------
void TestHlslTwin() {
  using namespace hlsl_shim;
  Rng rng(3);
  bool hash_ok = true, seed_ok = true, unit_ok = true;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t x = rng.U32() * 2654435761u + uint32_t(i);
    if (GiuHash(x) != gu::Hash(x)) hash_ok = false;
    if (GiuUnit(x) != gu::Unit(x)) unit_ok = false;
    const uint32_t f = rng.U32(), c = rng.U32() & 3u, p = rng.U32() & 0xFFFFu;
    if (GiuSeed(f, c, p) != gu::Seed(f, c, p)) seed_ok = false;
  }
  Check(hash_ok && unit_ok && seed_ok, "the hash, its unit float and the probe's seed are the twin's on 20,000 inputs");
  float worst_dir = 0, worst_rot = 0;
  bool unit_len = true;
  for (int i = 0; i < 3000; ++i) {
    const uint32_t n = (i & 1) ? 64u : 32u;
    const uint32_t ray = rng.U32() % n;
    const uint32_t seed = rng.U32();
    const float3 a = GiuFibonacci(ray, n);
    const gi::Vec3 b = gu::Fibonacci(ray, n);
    worst_dir = std::max(worst_dir, Dist(V(a), b));
    const float4 q = GiuRotationQuat(seed);
    const gu::Quat r = gu::RotationQuat(seed);
    worst_rot = std::max(worst_rot, std::max(std::max(std::fabs(q.x - r.x), std::fabs(q.y - r.y)), std::max(std::fabs(q.z - r.z), std::fabs(q.w - r.w))));
    const float3 d = GiuRayDirection(ray, n, q);
    const gi::Vec3 e = gu::RayDirection(ray, n, r);
    worst_dir = std::max(worst_dir, Dist(V(d), e));
    if (std::fabs(Len(V(d)) - 1.0f) > 1.0e-4f) unit_len = false;
  }
  Check(worst_dir < 1.0e-5f && worst_rot < 1.0e-5f, Num("the Fibonacci directions, the rotation quaternion and the rotated rays are the twin's (worst %.2e, %.2e)", worst_dir, worst_rot));
  Check(unit_len, "a rotated ray is a unit vector");

  bool word_ok = true;
  for (int i = 0; i < 5000; ++i) {
    const uint32_t c = rng.U32() & 3u, f = rng.U32() & 15u, idx = rng.U32() & 0xFFFFFu;
    const uint32_t w = GiuProbeWord(c, f, idx);
    if (w != gu::ProbeWord(c, f, idx) || GiuProbeCascade(w) != c || GiuProbeFlags(w) != f || GiuProbeIndex(w) != idx) word_ok = false;
  }
  Check(word_ok, "the probe word packs and unpacks as the twin's");

  bool ray_ok = true;
  for (int i = 0; i < 5000; ++i) {
    gu::Ray r;
    r.radiance = {float(rng.Range(-1, 40)), float(rng.Range(0, 5)), float(rng.Range(0, 0.01))};
    r.distance = float(rng.Range(0, 1000));
    r.flags = rng.U32() & 15u;
    r.extra = rng.U32();
    const gu::RayWords tw = gu::PackRay(r);
    const uint4 hw = GiuPackRay(float3(r.radiance.x, r.radiance.y, r.radiance.z), r.distance, r.flags, r.extra);
    if (hw.x != tw.w[0] || hw.y != tw.w[1] || hw.z != tw.w[2] || hw.w != tw.w[3]) ray_ok = false;
    const GiuRay u = GiuUnpackRay(hw);
    const gu::Ray t = gu::UnpackRay(tw);
    if (u.flags != r.flags || u.distance != r.distance || u.extra != r.extra || Dist(V(u.radiance), t.radiance) > 1.0e-6f) ray_ok = false;
    // the clamp and the half's precision
    const float wanted = std::min(std::max(r.radiance.x, 0.0f), gu::kMaxRadiance);
    if (std::fabs(t.radiance.x - wanted) > wanted * 1.0e-3f + 1.0e-6f) ray_ok = false;
  }
  Check(ray_ok, "the ray record packs as the twin's, unpacks to the clamped radiance within half precision, and keeps the flags, the distance and the extra word");
  // A channel that is not a number is 0 in the record and infinity is the cap (the shader decides it by the bits: Vulkan leaves fmax(NaN, 0) open), whatever the NaN's sign or payload
  bool nan_ok = true;
  const float inf = std::numeric_limits<float>::infinity();
  const float nans[] = {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(), std::bit_cast<float>(0x7F800001u), std::bit_cast<float>(0xFFC12345u)};
  for (const float nan : nans) {
    if (GiuRadianceChannel(nan) != 0.0f || gu::RadianceChannel(nan) != 0.0f) nan_ok = false;
    const uint4 packed = GiuPackRay(float3(nan, 1.0f, nan), 3.0f, 1u, 0u);
    const GiuRay unpacked = GiuUnpackRay(packed);
    if (unpacked.radiance.x != 0.0f || unpacked.radiance.y != 1.0f || unpacked.radiance.z != 0.0f) nan_ok = false;
  }
  if (GiuRadianceChannel(inf) != gu::kMaxRadiance || GiuRadianceChannel(-inf) != 0.0f || GiuRadianceChannel(-3.0f) != 0.0f || GiuRadianceChannel(5.0f) != 5.0f) nan_ok = false;
  Check(nan_ok, "a radiance channel that is NaN (either sign, any payload) is 0 in the ray record, infinity is the cap, in the shader and in the twin");

  bool border_ok = true;
  float worst_texel = 0;
  for (uint32_t ty = 0; ty < gi::kProbeTile; ++ty) {
    for (uint32_t tx = 0; tx < gi::kProbeTile; ++tx) {
      const GiuTexelCoord a = GiuBorderSource(tx, ty);
      const gi::Texel b = gi::OctBorderSource(tx, ty);
      if (a.x != b.x || a.y != b.y) border_ok = false;
      if (tx >= 1 && tx <= 8 && ty >= 1 && ty <= 8) worst_texel = std::max(worst_texel, Dist(V(GiuTexelDirection(tx, ty)), gi::TexelDirection(tx, ty)));
    }
  }
  Check(border_ok && worst_texel < 1.0e-6f, "the border's source and the interior texels' directions are gi_frame.h's, all 100 texels");

  // the integration against gi_frame.h's IntegrateTexel (the field's definition of the units)
  float worst_irr = 0, worst_sky = 0, worst_mom = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const uint32_t n = (trial & 1) ? 64u : 32u;
    std::vector<gi::RayResult> ref(n);
    const gu::Quat q = gu::RotationQuat(rng.U32());
    for (uint32_t i = 0; i < n; ++i) {
      const gi::Vec3 d = gu::RayDirection(i, n, q);
      const bool escaped = rng.Uniform() < 0.4;
      const gi::Vec3 radiance = escaped ? gi::Vec3{} : gi::Vec3{float(rng.Range(0, 3)), float(rng.Range(0, 3)), float(rng.Range(0, 3))};
      const float dist = float(rng.Range(0.05, 30));
      ref[i] = {d, radiance, dist, escaped};
      g_dir[i] = float3(d.x, d.y, d.z);
      g_radiance[i] = float3(radiance.x, radiance.y, radiance.z);
      g_distance[i] = dist;
      g_escaped[i] = escaped;
    }
    const float max_distance = float(rng.Range(2, 40));
    for (int k = 0; k < 6; ++k) {
      const uint32_t tx = 1 + rng.U32() % 8, ty = 1 + rng.U32() % 8;
      const gi::Vec3 dir = gi::TexelDirection(tx, ty);
      const GiuTexel h = GiuIntegrateTexel(float3(dir.x, dir.y, dir.z), n, max_distance);
      const gi::TexelValue t = gi::IntegrateTexel(dir, ref.data(), n, max_distance);
      worst_irr = std::max(worst_irr, Dist(V(h.irradiance), t.irradiance));
      worst_sky = std::max(worst_sky, std::fabs(h.sky - t.sky_visibility));
      worst_mom = std::max(worst_mom, std::max(std::fabs(h.meanDistance - t.mean_distance), std::fabs(h.meanDistanceSq - t.mean_distance_sq) / (max_distance * max_distance)));
    }
  }
  Check(worst_irr < 1.0e-4f && worst_sky < 1.0e-4f && worst_mom < 1.0e-3f, Num("the .hlsli's integration is gi_frame.h's IntegrateTexel (worst irradiance %.2e, sky %.2e, moments %.2e)", worst_irr, worst_sky, worst_mom));

  bool class_ok = true, blend_ok = true, hist_ok = true;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t back = rng.U32() % 65, rays = (rng.U32() & 1) ? 64u : 32u;
    const float nearest = float(rng.Range(0, 5)), spacing = float(rng.Range(1, 16));
    if (GiuProbeInside(back, rays, nearest, spacing) != gu::ProbeInside(back, rays, nearest, spacing)) class_ok = false;
    if (GiuBackByte(back, rays) != gu::BackByte(back, rays)) class_ok = false;
    const uint32_t frame = rng.U32(), probe = rng.U32() & 0xFFFFu;
    const bool ins = rng.U32() & 1;
    if (GiuSkipInside(ins, frame, probe) != gu::SkipInside(ins, frame, probe)) class_ok = false;
    const float om = float(rng.Range(0, 3)), nm = float(rng.Range(0, 3)), se = float(rng.Range(0, 1));
    if (std::fabs(GiuRelativeChange(om, nm) - gu::RelativeChange(om, nm)) > 1e-6f) blend_ok = false;
    if (GiuMeanJumped(om, nm, se) != gu::MeanJumped(om, nm, se)) blend_ok = false;
    if (std::fabs(GiuStandardError(om * 64, nm * 64, 64) - gu::StandardError(om * 64, nm * 64, 64)) > 1e-5f) blend_ok = false;
    const uint32_t updates = rng.U32() % 300;
    const bool is_new = rng.U32() & 1, dirty = rng.U32() & 1, jumped = rng.U32() & 1;
    if (std::fabs(GiuHysteresis(updates, is_new, dirty, jumped, gi::kHysteresis, gi::kHysteresisJump) - gu::Hysteresis(updates, is_new, dirty, jumped)) > 1e-7f) blend_ok = false;
    const float h = float(rng.Uniform());
    if (Dist(V(GiuBlend3(float3(om, nm, se), float3(nm, se, om), h)), gu::Blend(gi::Vec3{om, nm, se}, gi::Vec3{nm, se, om}, h)) > 1e-6f) blend_ok = false;
    if (std::fabs(GiuBlend1(om, nm, h) - gu::Blend(om, nm, h)) > 1e-6f) blend_ok = false;
    const float m = std::pow(10.0f, float(rng.Range(-8, 3)));
    if (GiuHistBin(m) != gu::HistBin(m)) hist_ok = false;
  }
  Check(class_ok, "the classification, the back face byte and the inside retest are the twin's on 20,000 inputs");
  Check(blend_ok, "the relative change, the jump, the standard error, the hysteresis and the blends are the twin's on 20,000 inputs");
  Check(hist_ok, "the histogram's bin is the twin's");
  // A value that is not a number is not carried by the hysteresis: NaN * 0 and infinity * 0 are NaN
  bool blend_nan_ok = true;
  const float bad_values[] = {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
  for (const float bad : bad_values) {
    for (const float h : {0.0f, 0.5f, 0.97f, 1.0f}) {
      if (GiuBlend1(bad, 2.0f, h) != 2.0f || gu::Blend(bad, 2.0f, h) != 2.0f) blend_nan_ok = false;  // a history that is not a number is replaced
      if (GiuBlend1(3.0f, bad, h) != 3.0f || gu::Blend(3.0f, bad, h) != 3.0f) blend_nan_ok = false;  // a sample that is not one leaves the history
      if (GiuBlend1(bad, bad, h) != 0.0f || gu::Blend(bad, bad, h) != 0.0f) blend_nan_ok = false;
      const float3 mixed = GiuBlend3(float3(bad, 1.0f, 2.0f), float3(4.0f, bad, 6.0f), h);
      const gi::Vec3 twin_mixed = gu::Blend(gi::Vec3{bad, 1.0f, 2.0f}, gi::Vec3{4.0f, bad, 6.0f}, h);
      if (mixed.x != 4.0f || mixed.y != 1.0f || std::fabs(mixed.z - (2.0f * h + 6.0f * (1.0f - h))) > 1e-6f || Dist(V(mixed), twin_mixed) > 1e-6f) blend_nan_ok = false;
    }
  }
  Check(blend_nan_ok, "a history or a sample that is NaN or infinite is dropped by the blend (never carried), in the shader and in the twin");
  Check(std::fabs(GiuLuma(float3(1, 1, 1)) - 1.0f) < 1e-6f && std::fabs(gu::Luma({1, 1, 1}) - 1.0f) < 1e-6f, "the luminance weights sum to one");
}

// ---------------------------------------------------------------------------
// the ray set
// ---------------------------------------------------------------------------
void TestRaySet() {
  for (const uint32_t n : {32u, 64u}) {
    std::vector<gi::Vec3> d(n);
    gi::Vec3 mean = {0, 0, 0};
    double xx = 0, yy = 0, zz = 0;
    float min_angle = 10.0f;
    for (uint32_t i = 0; i < n; ++i) {
      d[i] = gu::Fibonacci(i, n);
      mean = mean + d[i];
      xx += double(d[i].x) * d[i].x;
      yy += double(d[i].y) * d[i].y;
      zz += double(d[i].z) * d[i].z;
    }
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = i + 1; j < n; ++j) min_angle = std::min(min_angle, std::acos(std::min(1.0f, gi::Dot(d[i], d[j]))));
    const double ideal = std::sqrt(4.0 * 3.14159265358979 / double(n));  // the spacing of n points on a sphere
    Check(Len(mean) / float(n) < 0.02f && std::fabs(xx / n - 1.0 / 3.0) < 0.05 && std::fabs(yy / n - 1.0 / 3.0) < 0.02 && std::fabs(zz / n - 1.0 / 3.0) < 0.05,
          Num("%.0f spherical Fibonacci rays: the mean direction is 0 and the second moments 1/3 (mean %.4f, y moment %.3f)", n, Len(mean) / float(n), yy / n));
    Check(min_angle > 0.55f * float(ideal), Num("%.0f rays: no two closer than 0.55 of the even spacing (%.3f rad against %.3f)", n, min_angle, ideal));
  }
  // the rotation is a rotation, uniform, and new every update
  Rng rng(5);
  bool length_ok = true, isometry_ok = true;
  for (int i = 0; i < 2000; ++i) {
    const gu::Quat q = gu::RotationQuat(rng.U32());
    if (std::fabs(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) - 1.0f) > 1.0e-5f) length_ok = false;
    const gi::Vec3 a = gu::Fibonacci(rng.U32() % 64, 64), b = gu::Fibonacci(rng.U32() % 64, 64);
    if (std::fabs(gi::Dot(gu::Rotate(q, a), gu::Rotate(q, b)) - gi::Dot(a, b)) > 2.0e-5f) isometry_ok = false;
  }
  Check(length_ok && isometry_ok, "the rotation quaternion is a unit and the rotation keeps the angle between two rays");
  double mx = 0, my = 0, mz = 0, yy = 0;
  const int samples = 8000;
  for (int i = 0; i < samples; ++i) {
    const gi::Vec3 r = gu::Rotate(gu::RotationQuat(gu::Seed(uint32_t(i), 1, 17)), gi::Vec3{0, 1, 0});
    mx += r.x;
    my += r.y;
    mz += r.z;
    yy += double(r.y) * r.y;
  }
  Check(std::fabs(mx / samples) < 0.03 && std::fabs(my / samples) < 0.03 && std::fabs(mz / samples) < 0.03 && std::fabs(yy / samples - 1.0 / 3.0) < 0.02,
        Num("the rotated axis is uniform over the sphere (mean %.3f %.3f, y moment %.3f)", mx / samples, my / samples, yy / samples));
  int changed = 0;
  for (uint32_t f = 0; f < 1000; ++f) {
    const gi::Vec3 a = gu::RayDirection(0, 64, gu::RotationQuat(gu::Seed(f, 0, 5))), b = gu::RayDirection(0, 64, gu::RotationQuat(gu::Seed(f + 1, 0, 5)));
    if (Dist(a, b) > 1.0e-3f) ++changed;
  }
  Check(changed >= 990, Num("a probe's rays are new every frame (%.0f of 1000 consecutive frames moved ray 0)", changed));
  // a different probe in the same frame has another rotation
  int distinct = 0;
  for (uint32_t i = 0; i < 1000; ++i) {
    if (gu::Seed(7, 0, i) != gu::Seed(7, 0, i + 1)) ++distinct;
  }
  Check(distinct == 1000, "neighbouring probes of a frame have different seeds");

  // a closed environment of uniform radiance L integrates to L in every texel (the field's units: gi_frame.h IntegrateTexel)
  for (const uint32_t n : {32u, 64u}) {
    double mean_err = 0, worst = 0;
    int cnt = 0;
    for (int trial = 0; trial < 300; ++trial) {
      const gu::Quat q = gu::RotationQuat(rng.U32());
      std::vector<gi::RayResult> rays(n);
      for (uint32_t i = 0; i < n; ++i) rays[i] = {gu::RayDirection(i, n, q), {0.7f, 0.7f, 0.7f}, 10.0f, false};
      for (uint32_t ty = 1; ty <= 8; ty += 3)
        for (uint32_t tx = 1; tx <= 8; tx += 3) {
          const gi::TexelValue t = gi::IntegrateTexel(gi::TexelDirection(tx, ty), rays.data(), n, 30.0f);
          const double err = std::fabs(double(t.irradiance.x) - 0.7) / 0.7;
          mean_err += err;
          worst = std::max(worst, err);
          ++cnt;
        }
    }
    Check(mean_err / cnt < 0.02 && worst < (n == 64 ? 0.12 : 0.25), Num("%.0f rays in a closed uniform environment: the irradiance is its radiance (mean error %.3f, worst %.3f)", n, mean_err / cnt, worst));
  }

  // a hit with no data is a miss, not a black occluder (CITY-RT-REVIEW M5): the record of the twin's HitRecord, integrated as the blend does
  {
    const gu::Ray miss = gu::HitRecord(false, false, 0.0f, 120.0f, {}, false, false, 0);
    const gu::Ray nodata = gu::HitRecord(true, false, 7.0f, 120.0f, {}, false, true, 3);
    const gu::Ray good = gu::HitRecord(true, true, 7.0f, 120.0f, {0.5f, 0.4f, 0.3f}, true, true, 3);
    Check(gu::PackRay(nodata) == gu::PackRay(miss) && (nodata.flags & gu::kRayHit) == 0 && nodata.distance == 120.0f && nodata.radiance.x == 0.0f && nodata.extra == 0,
          "a hit with no data is recorded as a miss: no hit flag, the ray's reach, no light");
    Check((good.flags & gu::kRayHit) != 0 && (good.flags & gu::kRayBack) != 0 && (good.flags & gu::kRayLod) != 0 && good.distance == 7.0f && good.radiance.x == 0.5f && good.extra == 3,
          "a hit whose data decoded keeps its flags, distance, light and class");
    const uint32_t n = 64;
    const gu::Quat q = gu::RotationQuat(0x1234u);
    const auto result = [&](const gu::Ray& r, uint32_t i) {
      gi::RayResult out;
      out.dir = gu::RayDirection(i, n, q);
      out.radiance = r.radiance;
      out.distance = r.distance;
      out.escaped = (r.flags & gu::kRayHit) == 0;
      return out;
    };
    // a probe by a wall with no data: a quarter of its rays hit it, the rest are open; the texels see the wall's rays as open ones, where a black hit took the sky of its direction away
    std::vector<gi::RayResult> as_miss(n), as_nodata(n), as_black(n);
    for (uint32_t i = 0; i < n; ++i) {
      const bool wall = i % 4 == 0;
      as_miss[i] = result(miss, i);
      as_nodata[i] = result(wall ? nodata : miss, i);
      gu::Ray black;  // what the trace recorded before: a hit flag, the hit's distance, black
      black.flags = gu::kRayHit;
      black.distance = 7.0f;
      as_black[i] = result(wall ? black : miss, i);
    }
    double worst_sky = 0.0, worst_irr = 0.0, black_sky_loss = 0.0;
    for (uint32_t ty = 1; ty <= 8; ++ty) {
      for (uint32_t tx = 1; tx <= 8; ++tx) {
        const gi::Vec3 d = gi::TexelDirection(tx, ty);
        const gi::TexelValue a = gi::IntegrateTexel(d, as_nodata.data(), n, 30.0f);
        const gi::TexelValue m = gi::IntegrateTexel(d, as_miss.data(), n, 30.0f);
        const gi::TexelValue k = gi::IntegrateTexel(d, as_black.data(), n, 30.0f);
        worst_sky = std::max(worst_sky, double(std::fabs(a.sky_visibility - m.sky_visibility)));
        worst_irr = std::max(worst_irr, double(std::fabs(a.irradiance.x - m.irradiance.x)));
        black_sky_loss = std::max(black_sky_loss, double(m.sky_visibility - k.sky_visibility));
      }
    }
    Check(worst_sky < 1.0e-6 && worst_irr < 1.0e-6 && black_sky_loss > 0.05,
          Num("rays that hit a sector with no data integrate as open rays; a black hit took the sky of its direction away (%.0e from the open, a black hit lost up to %.2f)", worst_sky, black_sky_loss));
  }
}

// ---------------------------------------------------------------------------
// the classification on analytic scenes
// ---------------------------------------------------------------------------
struct Plane {
  gi::Vec3 point, normal;  // the front is the side the normal points to
};
struct Hit {
  bool hit = false;
  bool back = false;
  float t = 0;
};
Hit TracePlanes(gi::Vec3 origin, gi::Vec3 dir, const std::vector<Plane>& planes, float half_extent) {
  Hit best;
  best.t = 1.0e30f;
  for (const Plane& p : planes) {
    const float denom = gi::Dot(p.normal, dir);
    if (std::fabs(denom) < 1.0e-6f) continue;
    const float t = gi::Dot(p.normal, p.point - origin) / denom;
    if (t <= 0.0f || t >= best.t) continue;
    const gi::Vec3 at = origin + dir * t;
    if (std::fabs(at.x - p.point.x) > half_extent && std::fabs(p.normal.x) < 0.5f) continue;
    if (std::fabs(at.y - p.point.y) > half_extent && std::fabs(p.normal.y) < 0.5f) continue;
    if (std::fabs(at.z - p.point.z) > half_extent && std::fabs(p.normal.z) < 0.5f) continue;
    best = {true, denom > 0.0f, t};
  }
  return best;
}
struct Verdict {
  uint32_t back = 0, hits = 0;
  float nearest = 1.0e30f;
  bool inside = false;
};
Verdict Classify(gi::Vec3 probe, const std::vector<Plane>& planes, float half_extent, float spacing, uint32_t seed_base, uint32_t rays = 64) {
  Verdict v;
  const gu::Quat q = gu::RotationQuat(seed_base);
  for (uint32_t i = 0; i < rays; ++i) {
    const Hit h = TracePlanes(probe, gu::RayDirection(i, rays, q), planes, half_extent);
    if (!h.hit) continue;
    ++v.hits;
    if (h.back) ++v.back;
    v.nearest = std::min(v.nearest, h.t);
  }
  v.inside = gu::ProbeInside(v.back, rays, v.nearest, spacing);
  return v;
}

void TestClassification() {
  const std::vector<Plane> ground = {{{0, 0, 0}, {0, 1, 0}}};
  const float big = 1.0e9f;
  int below = 0, above = 0, close = 0, mid = 0;
  for (uint32_t s = 0; s < 400; ++s) {
    if (Classify({0, -1.0f, 0}, ground, big, 2.0f, s * 7 + 1).inside) ++below;
    if (!Classify({0, 1.0f, 0}, ground, big, 2.0f, s * 7 + 2).inside) ++above;
    if (Classify({0, 0.1f, 0}, ground, big, 2.0f, s * 7 + 3).inside) ++close;
    if (!Classify({0, 0.5f, 0}, ground, big, 2.0f, s * 7 + 4).inside) ++mid;
  }
  Check(below == 400, "a probe under the ground sheet sees its back (about half its rays) and is inside geometry, in every one of 400 rotations");
  Check(above == 400, "a probe a metre above the ground sees front faces only and is not inside geometry, in every rotation");
  Check(close == 400, "a probe 0.1 m above a surface of a 2 m lattice is inside (its nearest hit is under a tenth of a spacing: the 0.2 m rule)");
  Check(mid == 400, "a probe 0.5 m above it is not");
  // a closed box with outward normals, half size 5
  std::vector<Plane> box = {{{5, 0, 0}, {1, 0, 0}}, {{-5, 0, 0}, {-1, 0, 0}}, {{0, 5, 0}, {0, 1, 0}}, {{0, -5, 0}, {0, -1, 0}}, {{0, 0, 5}, {0, 0, 1}}, {{0, 0, -5}, {0, 0, -1}}};
  int in_box = 0, out_box = 0;
  float back_share = 0;
  for (uint32_t s = 0; s < 300; ++s) {
    const Verdict a = Classify({0.5f, 0.2f, -1.0f}, box, 5.0001f, 2.0f, s * 3 + 1);
    if (a.inside) ++in_box;
    back_share += float(a.back) / 64.0f;
    if (!Classify({12.0f, 0.3f, 1.0f}, box, 5.0001f, 2.0f, s * 3 + 2).inside) ++out_box;
  }
  Check(in_box == 300 && back_share / 300.0f > 0.99f, Num("a probe inside a closed box sees back faces with all its rays (%.2f) and is inside", back_share / 300.0f));
  Check(out_box == 300, "a probe outside the box sees its front faces and is not inside");
  // the retest: an inside probe is skipped at 7 visits of 8; one that is not inside never
  int skipped = 0, never = 0;
  for (uint32_t f = 0; f < 8000; ++f) {
    if (gu::SkipInside(true, f, 12345)) ++skipped;
    if (gu::SkipInside(false, f, 12345)) ++never;
  }
  Check(never == 0 && skipped > 6800 && skipped < 7200, Num("an inside probe is looked at at one visit in eight (%.0f of 8000 skipped)", skipped));
}

// ---------------------------------------------------------------------------
// the blend
// ---------------------------------------------------------------------------
void TestBlend() {
  // the first update has no history; the weight of the stored value is the running mean while the probe is young, at most 0.97
  Check(gu::Hysteresis(0, true, false, false) == 0.0f && gu::Hysteresis(0, false, false, false) == 0.0f, "the first update replaces: a new probe and a probe of no updates have no history");
  Check(std::fabs(gu::Hysteresis(1, false, false, false) - 0.5f) < 1e-7f && std::fabs(gu::Hysteresis(2, false, false, false) - 2.0f / 3.0f) < 1e-6f, "the second update averages 1:1, the third 2:1");
  Check(gu::Hysteresis(32, false, false, false) < gi::kHysteresis && gu::Hysteresis(33, false, false, false) == gi::kHysteresis && gu::Hysteresis(255, false, false, false) == gi::kHysteresis,
        "the running mean reaches the design's 0.97 at 33 updates and stays");
  Check(gu::Hysteresis(100, false, true, false) == gi::kHysteresisJump && gu::Hysteresis(100, false, false, true) == gi::kHysteresisJump && gu::Hysteresis(1, false, true, false) == 0.5f,
        "a dirty or jumped probe falls to 0.85 (a young one keeps its smaller weight); a new one still has none");
  // the running mean: the stored value after n updates is the mean of the samples while h = n / (n + 1)
  Rng rng(7);
  std::vector<float> samples;
  float stored = 0;
  bool mean_ok = true;
  for (uint32_t n = 0; n < 32; ++n) {
    const float s = float(rng.Range(0, 4));
    samples.push_back(s);
    stored = gu::Blend(stored, s, gu::Hysteresis(n, n == 0, false, false));
    float mean = 0;
    for (float x : samples) mean += x;
    mean /= float(samples.size());
    if (std::fabs(stored - mean) > 1.0e-5f) mean_ok = false;
  }
  Check(mean_ok, "while a probe is young the blend is the exact running mean of its updates (the first 32)");
  // the step response: a converged probe (updates 60) whose light changes from 1 to 4
  const auto steps_to = [&](float from, float to, bool jumped, float fraction) {
    float v = from;
    int n = 0;
    while (std::fabs(v - to) > (1.0f - fraction) * std::fabs(to - from) && n < 2000) {
      v = gu::Blend(v, to, gu::Hysteresis(60, false, false, jumped));
      ++n;
    }
    return n;
  };
  const int fast = steps_to(1.0f, 4.0f, true, 0.9f), slow = steps_to(1.0f, 4.0f, false, 0.9f);
  Check(fast >= 13 && fast <= 15, Num("after a jump the probe follows to 90%% in %.0f updates (0.85^14 = 10%%)", fast));
  Check(slow >= 70 && slow <= 80, Num("without one it takes %.0f updates (0.97: about 33 per time constant)", slow));
  // the jump rule: a real change is a jump, noise is not
  const float old_mean = 0.4f;
  Check(gu::MeanJumped(old_mean, 1.2f, 0.05f) && !gu::MeanJumped(old_mean, 0.45f, 0.01f) && !gu::MeanJumped(old_mean, 1.2f, 0.5f) && !gu::MeanJumped(0.001f, 0.01f, 0.0001f),
        "a mean that triples with a small error is a jump; a 12% move, a move inside the noise and a dark probe's flicker are not");
  // a probe that sees a small bright thing with a few rays: its new mean swings, and the swings are not jumps
  const auto env_mean = [&](double base, double patch, double fraction, int trials, double* jumps, double* swing) {
    int jumped = 0;
    double worst = 0;
    const double truth = base * (1 - fraction) + patch * fraction;
    for (int t = 0; t < trials; ++t) {
      const gu::Quat q = gu::RotationQuat(rng.U32());
      float sum = 0, sq = 0;
      for (uint32_t i = 0; i < 64; ++i) {
        const gi::Vec3 d = gu::RayDirection(i, 64, q);
        const bool in_patch = d.x > 1.0f - 2.0f * float(fraction);  // a cap of the sphere with the fraction of its area
        const float l = float(in_patch ? patch : base);
        sum += l;
        sq += l * l;
      }
      const float mean = sum / 64.0f;
      const float se = gu::StandardError(sum, sq, 64);
      if (gu::MeanJumped(float(truth), mean, se)) ++jumped;
      worst = std::max(worst, std::fabs(double(mean) - truth) / truth);
    }
    *jumps = double(jumped) / trials;
    *swing = worst;
    return truth;
  };
  double jumps = 0, swing = 0;
  env_mean(0.1, 10.0, 0.03, 4000, &jumps, &swing);
  Check(jumps < 0.002, Num("a probe that sees a bright patch (3%% of the sphere, 100x the rest) with a few rays: its mean swings by up to %.0f%% from the truth and %.2f%% of the updates call it a jump", swing * 100.0, jumps * 100.0));
  // a broad change is a jump at once: the whole environment from 0.1 to 0.4 with some spread
  int detected = 0;
  for (int t = 0; t < 2000; ++t) {
    const gu::Quat q = gu::RotationQuat(rng.U32());
    float sum = 0, sq = 0;
    for (uint32_t i = 0; i < 64; ++i) {
      const float l = 0.4f * float(0.7 + 0.6 * rng.Uniform());
      sum += l;
      sq += l * l;
    }
    (void)q;
    if (gu::MeanJumped(0.1f, sum / 64.0f, gu::StandardError(sum, sq, 64))) ++detected;
  }
  Check(detected == 2000, "a broad change of the light (0.1 to 0.4 over most rays) is a jump in every update");
  // the census' histogram
  Check(gu::HistBin(0.0f) == 0 && gu::HistBin(1.0f) == 20 && gu::HistBin(1000.0f) == 31 && gu::HistBin(std::exp2(-10.0f)) == 0 && std::fabs(gu::HistBinUpper(20) - std::sqrt(2.0f)) < 1e-5f, "the histogram: half octaves from 2^-10 to 2^6");
}

// ---------------------------------------------------------------------------
// the scheduler
// ---------------------------------------------------------------------------
struct Rig {
  gi::Budget budget = gi::BudgetOf(gi::Layout::kStandard);
  gf::FieldState state;
  gu::Scheduler sched;
  double camera[3] = {1000.0, 30.0, -2000.0};
  explicit Rig(gi::Layout layout = gi::Layout::kStandard) : budget(gi::BudgetOf(layout)) {
    state.Start(budget, camera);
    sched.Start(budget);
    state.Move(camera);  // fresh: every probe is new and dirty, the warm-up begins
  }
  // clears every dirty probe and runs the warm-up out
  void Settle() {
    for (uint32_t c = 0; c < budget.cascade_count; ++c)
      for (uint32_t i = 0; i < budget.cascade[c].dims.Probes(); ++i) state.NoteTraced(c, i);
    for (int i = 0; i < 14; ++i) state.Move(camera);
  }
  gu::PlanInputs Inputs(float safe_range = 400.0f) const {
    gu::PlanInputs in;
    for (int a = 0; a < 3; ++a) in.camera[a] = camera[a];
    in.safe_range_m = safe_range;
    in.warmup_scale = state.BudgetScale();
    return in;
  }
};

void TestScheduler() {
  const gu::Budget ub = gu::BudgetOf(gi::BudgetOf(gi::Layout::kStandard));
  {
    Rig rig;
    auto in = rig.Inputs();
    in.reuse_coarse = true;
    const gu::Plan warm = rig.sched.Make(rig.state, in, ub.max_probes);
    Check(warm.per_cascade[1] == 8192 && warm.per_cascade[2] == 4096, "reuse keeps the full warm-up quota");
    rig.Settle();
    in = rig.Inputs();
    in.reuse_coarse = true;
    const gu::Plan steady = rig.sched.Make(rig.state, in, ub.max_probes);
    Check(steady.per_cascade[0] == 4096 && steady.per_cascade[1] == 1024 && steady.per_cascade[2] == 512,
          "reuse halves the coarse quotas and keeps the finest quota");
    rig.camera[0] += 8.0;
    rig.state.Move(rig.camera);
    in = rig.Inputs();
    in.reuse_coarse = true;
    rig.sched.MarkAllStale();
    const gu::Plan priority = rig.sched.Make(rig.state, in, ub.max_probes);
    Check(priority.per_cascade[1] == 2048 && priority.dirty_taken[1] > 0 && priority.stale_taken[1] > 0 &&
              priority.dirty_taken[1] + priority.stale_taken[1] == priority.per_cascade[1] && priority.per_cascade[2] == 1024,
          "new and stale probes still take the full quota before round-robin reuse");
  }
  {
    Rig rig;
    Check(rig.state.BudgetScale() == gi::kCutWarmupBudgetScale && rig.state.TotalDirty() == 49152, "a fresh field: every probe is dirty and the warm-up budget is 4x");
    const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(), ub.max_probes);
    Check(plan.per_cascade[0] == 16384 && plan.per_cascade[1] == 8192 && plan.per_cascade[2] == 4096 && plan.Total() == 28672,
          Num("the warm-up plan: the whole finest cascade (%.0f), 8,192 and 4,096 of the others, four chunks of the layout's 7,168 (%.0f in all)", plan.per_cascade[0], plan.Total()));
    Check(plan.dirty_taken[0] == 16384 && plan.dirty_taken[1] == 8192 && plan.dirty_taken[2] == 4096, "all of them are the field's dirty probes");
    Check(rig.state.DirtyCount(0) == 0 && rig.state.DirtyCount(1) == 8192 && rig.state.DirtyCount(2) == 12288, "the dirty probes taken are no longer dirty; the rest wait for the next frames");
    std::set<uint64_t> seen;
    bool distinct = true;
    for (uint32_t w : plan.words) distinct = distinct && seen.insert((uint64_t(gu::ProbeCascade(w)) << 32) | gu::ProbeIndex(w)).second;
    Check(distinct, "no probe is planned twice in a frame");
    bool flags = true;
    for (uint32_t w : plan.words) flags = flags && gu::ProbeFlags(w) == gu::kProbeDirty;
    Check(flags, "a dirty probe carries the dirty flag (the blend's jump hysteresis; a new one has no history anyway)");
    const std::vector<gu::Chunk> chunks = gu::ChunksOf(plan.Total(), ub.chunk_probes);
    Check(chunks.size() == 4 && chunks[0].first == 0 && chunks[0].count == 7168 && chunks[3].first == 21504 && chunks[3].count == 7168, "the plan runs in four chunks of the ray buffer's capacity");
    const std::vector<gu::Chunk> small = gu::ChunksOf(7169, 7168);
    Check(small.size() == 2 && small[1].count == 1 && gu::ChunksOf(0, 7168).empty(), "a plan one probe over a chunk has a second chunk of one; an empty plan none");
  }
  {
    // the steady state: every probe once in `interval` frames
    Rig rig;
    rig.Settle();
    Check(rig.state.BudgetScale() == 1.0f && rig.state.TotalDirty() == 0, "after the warm-up the budget is 1x and nothing is dirty");
    std::vector<std::set<uint32_t>> covered(3);
    bool share_ok = true, distinct_ok = true;
    for (int frame = 0; frame < 16; ++frame) {
      const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(), ub.max_probes);
      share_ok = share_ok && plan.per_cascade[0] == 4096 && plan.per_cascade[1] == 2048 && plan.per_cascade[2] == 1024 && plan.Total() == 7168;
      std::set<uint32_t> frame_set[3];
      for (uint32_t w : plan.words) {
        distinct_ok = distinct_ok && frame_set[gu::ProbeCascade(w)].insert(gu::ProbeIndex(w)).second && gu::ProbeFlags(w) == 0;
        if (frame < gi::BudgetOf(gi::Layout::kStandard).cascade[gu::ProbeCascade(w)].interval_frames) covered[gu::ProbeCascade(w)].insert(gu::ProbeIndex(w));
      }
    }
    Check(share_ok, "in the steady state a frame traces the design's 4,096 + 2,048 + 1,024 probes (458,752 rays)");
    Check(distinct_ok, "a frame's probes are distinct and carry no flag");
    Check(covered[0].size() == 16384 && covered[1].size() == 16384 && covered[2].size() == 16384, "every probe of every cascade is traced once in its interval (4, 8 and 16 frames)");
  }
  {
    Check(gu::CascadeShare(gi::BudgetOf(gi::Layout::kStandard).cascade[2], 0.0f) == 1 && gu::CascadeShare(gi::BudgetOf(gi::Layout::kStandard).cascade[0], 100.0f) == 16384, "a share is at least one probe and at most the cascade");
  }
  {
    // the safe range: probes beyond it are not traced, dirty ones stay dirty
    Rig rig;
    const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(20.0f), ub.max_probes);
    bool in_range = true;
    for (uint32_t w : plan.words) {
      const uint32_t c = gu::ProbeCascade(w);
      const gi::Coord k = gf::CoordOfIndex(rig.state.origin(c), rig.budget.cascade[c].dims, gu::ProbeIndex(w));
      const gi::Position p = gi::ProbeWorld(k, rig.budget.cascade[c].spacing_m);
      const double dx = p.x - rig.camera[0], dy = p.y - rig.camera[1], dz = p.z - rig.camera[2];
      in_range = in_range && std::sqrt(dx * dx + dy * dy + dz * dz) <= 20.0001;
    }
    Check(in_range && plan.Total() > 0 && plan.Total() < 28672, Num("with a safe range of 20 m only the probes within it are planned (%.0f of the 28,672)", plan.Total()));
    Check(plan.left_dirty_out_of_range > 0 && rig.state.DirtyCount(0) > 0, "the dirty probes beyond the range are left dirty (their weight is 0; the range may grow)");
    const gu::Plan none = rig.sched.Make(rig.state, rig.Inputs(0.0f), ub.max_probes);
    Check(none.Total() == 0, "a safe range of 0 plans nothing");
  }
  {
    // stale boxes: traced first, with the dirty flag, and not in the field's dirty sets
    Rig rig;
    rig.Settle();
    const double lo[3] = {rig.camera[0] - 5, rig.camera[1] - 3, rig.camera[2] - 5}, hi[3] = {rig.camera[0] + 5, rig.camera[1] + 3, rig.camera[2] + 5};
    const uint32_t marked = rig.sched.MarkStaleBox(rig.state, lo, hi);
    uint32_t expected0 = 0;
    {
      const double m = 3.0 * 2.0;  // three spacings of the finest cascade
      const gi::Box box = {{gi::CellOf(lo[0] - m, 2.0f), gi::CellOf(lo[1] - m, 2.0f), gi::CellOf(lo[2] - m, 2.0f)}, {gi::CellOf(hi[0] + m, 2.0f) + 1, gi::CellOf(hi[1] + m, 2.0f) + 1, gi::CellOf(hi[2] + m, 2.0f) + 1}};
      const gi::Box win = rig.state.Window(0);
      const gi::Box b = {{std::max(box.lo.x, win.lo.x), std::max(box.lo.y, win.lo.y), std::max(box.lo.z, win.lo.z)}, {std::min(box.hi.x, win.hi.x), std::min(box.hi.y, win.hi.y), std::min(box.hi.z, win.hi.z)}};
      expected0 = uint32_t(b.Count());
    }
    Check(rig.sched.StaleCount(0) == expected0 && expected0 > 0 && marked >= expected0, Num("a built sector marks the finest cascade's probes within three spacings of its box (%.0f)", rig.sched.StaleCount(0)));
    Check(rig.state.TotalDirty() == 0, "the field's own dirty sets are untouched (they drive its fade)");
    const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(), ub.max_probes);
    bool first_ok = true;
    for (uint32_t i = 0; i < expected0 && i < plan.words.size(); ++i) first_ok = first_ok && gu::ProbeCascade(plan.words[i]) == 0 && gu::ProbeFlags(plan.words[i]) == gu::kProbeDirty;
    Check(first_ok && plan.stale_taken[0] == expected0 && rig.sched.StaleCount(0) == 0, "the stale probes come first in their cascade with the dirty flag, and are cleared once taken");
    Check(gu::ProbeFlags(plan.words[expected0]) == 0, "the round robin follows them");
    rig.sched.MarkAllStale();
    Check(rig.sched.StaleCount(0) == 16384 && rig.sched.StaleCount(2) == 16384, "every probe can be marked stale at once");
    const double far_lo[3] = {rig.camera[0] + 5000, 0, 0}, far_hi[3] = {rig.camera[0] + 5010, 10, 10};
    Rig other;
    other.Settle();
    Check(other.sched.MarkStaleBox(other.state, far_lo, far_hi) == 0, "a box far outside every window marks nothing");
  }
  {
    // a move exposes probes: they are dirty and come first, nearest the camera first
    Rig rig;
    rig.Settle();
    rig.camera[0] += 6.0;  // three cells of the finest cascade, one and a half of the second
    rig.state.Move(rig.camera);
    const uint32_t dirty0 = rig.state.DirtyCount(0);
    Check(dirty0 == 3u * 16u * 32u, Num("a 6 m move exposes three slabs of the finest cascade (%.0f probes)", dirty0));
    const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(), ub.max_probes);
    uint32_t taken0 = plan.dirty_taken[0];
    Check(taken0 == dirty0 && rig.state.DirtyCount(0) == 0, "the exposed probes are planned and no longer dirty");
    int32_t last = -1;
    bool sorted = true;
    const gi::Coord ref = {gi::CellOf(rig.camera[0], 2.0f), gi::CellOf(rig.camera[1], 2.0f), gi::CellOf(rig.camera[2], 2.0f)};
    for (uint32_t i = 0; i < taken0; ++i) {
      const gi::Coord k = gf::CoordOfIndex(rig.state.origin(0), rig.budget.cascade[0].dims, gu::ProbeIndex(plan.words[i]));
      const int32_t dist = std::max({std::abs(k.x - ref.x), std::abs(k.y - ref.y), std::abs(k.z - ref.z)});
      sorted = sorted && dist >= last;
      last = dist;
    }
    Check(sorted, "the exposed probes are taken nearest the camera first (Chebyshev distance in cells)");
  }
  {
    // the lean layout: 2 cascades, 32 rays, a chunk of 384
    Rig rig(gi::Layout::kLean);
    const gu::Budget lean = gu::BudgetOf(gi::BudgetOf(gi::Layout::kLean));
    const gu::Plan plan = rig.sched.Make(rig.state, rig.Inputs(300.0f), lean.max_probes);
    Check(plan.per_cascade[0] == 1024 && plan.per_cascade[1] == 512 && plan.Total() == lean.max_probes, "the lean layout's warm-up plan (256 + 128 probes a frame, 4x) is exactly its four chunks");
    Check(lean.max_probes == 1536 && lean.rays_per_probe == 32, "lean: 384 probes a chunk of 32 rays, four chunks");
  }
}

// ---------------------------------------------------------------------------
// the dispatches and the budgets of the cells
// ---------------------------------------------------------------------------
void TestPhasedScheduler() {
  const uint32_t settled = gu::StateAfterBlend(gf::MakeState(40, false, false), false, 77, 1023u, gu::kStableVisits);
  Check(gu::StableVisits(settled) == 4 && gu::StableVisits(gf::kStateAfterReset) == 0 && gf::StateBack(settled) == 77, "settled streak and existing state fields have distinct bits");
  uint32_t streak = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t state = streak << gu::kStableShift;
    streak = gu::NextStableVisits(state, false, 0.02f, 0.1f, 1.0f);
    Check(streak == std::min(i + 1u, 4u) && streak == hlsl_shim::GiuNextStableVisits(state, false, 0.02f, 0.1f, 1.0f), "steady history saturates after four visits in the CPU and shader");
  }
  Check(gu::NextStableVisits(settled, true, 0.0f, 0.0f, 1.0f) == 0 &&
        gu::NextStableVisits(settled, false, 0.1f, 0.0f, 1.0f) == 0 &&
        gu::NextStableVisits(settled, false, 0.0f, 0.16f, 1.0f) == 0, "dirty history, irradiance change and noisy rays each wake the probe");
  for (uint32_t flags = 0; flags < 8; ++flags) {
    for (const bool inside : {false, true}) {
      for (const uint32_t state : {0u, settled}) {
        Check(hlsl_shim::GiuScheduledSkip(inside, state, flags) == gu::ScheduledSkip(inside, state, flags), "the shader and scheduler twin agree on every skip reason");
      }
    }
  }
  Check(gu::ScheduledSkip(false, settled, gu::kProbeMaySleep) == gu::kRaySleeping &&
        gu::ScheduledSkip(false, 0, gu::kProbeMaySleep) == 0 &&
        gu::ScheduledSkip(true, settled, 0) == gu::kRaySkipped &&
        gu::ScheduledSkip(true, settled, gu::kProbeRetestInside) == 0 &&
        gu::ScheduledSkip(true, settled, gu::kProbeDirty) == 0, "only settled far probes sleep and dirty work bypasses inside skips");

  for (const gi::Layout layout : {gi::Layout::kStandard, gi::Layout::kExtended}) {
    Rig rig(layout);
    const gu::Budget budget = gu::BudgetOf(rig.budget);
    auto in = rig.Inputs();
    in.phased = true;
    in.reuse_coarse = true;
    const gu::Plan warm = rig.sched.Make(rig.state, in, budget.max_probes);
    Check(warm.dirty_taken[0] == rig.budget.cascade[0].dims.Probes() && warm.Total() <= budget.max_probes &&
          std::all_of(warm.words.begin(), warm.words.end(), [](uint32_t w) { return gu::ProbeFlags(w) == gu::kProbeDirty; }), "warm-up keeps its full priority budget and nominal ray count");
    rig.Settle();
    in = rig.Inputs();
    in.phased = true;
    in.reuse_coarse = true;
    bool coverage = true, balanced = true, phase_bounds = true;
    for (uint32_t c = 0; c < rig.budget.cascade_count; ++c) {
      const auto& spec = rig.budget.cascade[c];
      const uint32_t period = spec.interval_frames * (c != 0 ? 2u : 1u);
      std::vector<uint32_t> seen(spec.dims.Probes());
      for (uint32_t phase = 0; phase < period; ++phase) {
        for (uint32_t block = 0; block * period < spec.dims.Probes(); ++block) ++seen[gu::CohortIndex(block, phase, period)];
      }
      for (uint32_t i = 0; i < seen.size(); ++i) {
        coverage &= seen[i] == 1;
        uint32_t traces = 0, inside_retests = 0;
        for (uint32_t cycle = 0; cycle < 4; ++cycle) {
          const uint32_t flags = gu::VisitFlags(cycle, i, true);
          traces += gu::ScheduledSkip(false, settled, flags) == 0;
          inside_retests += gu::ScheduledSkip(true, settled, flags) == 0;
          phase_bounds &= (gu::VisitFlags(cycle, i, false) & gu::kProbeMaySleep) == 0;
          phase_bounds &= gu::ScheduledSkip(false, settled, flags) == 0 ||
                          gu::ScheduledSkip(false, settled, gu::VisitFlags(cycle + 1, i, true)) == 0;
        }
        balanced &= traces == 2 && inside_retests == 1;
      }
    }
    Check(coverage && balanced && phase_bounds, "all slots have a fixed visit phase, one inside retest per four visits and a far age bound of two visits");

    const gu::Plan held = rig.sched.Make(rig.state, in, budget.max_probes);
    Check(std::any_of(held.words.begin(), held.words.end(), [](uint32_t w) { return (gu::ProbeFlags(w) & gu::kProbeMaySleep) != 0; }), "a held view can sleep its stable far probes");
    in.camera_moving = true;
    const gu::Plan turning = rig.sched.Make(rig.state, in, budget.max_probes);
    Check(std::none_of(turning.words.begin(), turning.words.end(), [](uint32_t w) { return (gu::ProbeFlags(w) & gu::kProbeMaySleep) != 0; }), "camera motion wakes the regular cohort");
    Rig busy(layout);
    busy.Settle();
    bool retained = true, bounded = true, unique = true;
    const uint32_t period = busy.budget.cascade[0].interval_frames;
    std::set<uint32_t> covered;
    for (uint32_t phase = 0; phase < period; ++phase) {
      busy.sched.MarkAllStale();
      const gu::Plan plan = busy.sched.Make(busy.state, in, budget.max_probes);
      std::set<uint32_t> frame;
      for (uint32_t w : plan.words) {
        if (gu::ProbeCascade(w) != 0) continue;
        unique &= frame.insert(gu::ProbeIndex(w)).second;
        covered.insert(gu::ProbeIndex(w));
      }
      retained &= plan.stale_taken[0] > 0;
      bounded &= plan.Total() <= 2u * budget.probes_per_frame;
    }
    Check(retained && bounded && unique && covered.size() == busy.budget.cascade[0].dims.Probes(), "priority saturation cannot starve, duplicate or overflow the regular cohort");

    rig.camera[0] += 8.0;
    rig.state.Move(rig.camera);
    in = rig.Inputs(34.0f);
    in.phased = true;
    const gu::Plan offset = rig.sched.Make(rig.state, in, budget.max_probes);
    Check(offset.dirty_taken[0] > 0 && offset.left_dirty_out_of_range > 0 && rig.state.DirtyCount(0) > 0, "small offsets wake entering probes without consuming out-of-range slots");
    rig.camera[0] += 400.0;
    Check(rig.state.Move(rig.camera).cut, "teleport resets the field");
    rig.sched.Start(rig.budget);
    in = rig.Inputs();
    in.phased = true;
    const gu::Plan cut = rig.sched.Make(rig.state, in, budget.max_probes);
    Check(cut.dirty_taken == warm.dirty_taken && cut.Total() == warm.Total(), "teleport restarts bounded full-rate warm-up");
    rig.state.RequestCut(gf::CutKind::kRelight);
    Check(rig.state.Move(rig.camera).relight && rig.state.TotalDirty() == gi::BudgetProbes(rig.budget), "hour jump clears probes for full relight");
  }

  gu::ViewMotion motion;
  Check(!motion.Observe({}, {0, 0, 1}) && !motion.Observe({0.2f, 0, 0}, {0, 0, 1}) &&
        motion.Observe({0.4f, 0, 0}, {0, 0, 1}), "small camera steps accumulate until the field wakes");
  Check(motion.Observe({0.4f, 0, 0}, {0, 0, 1}) && motion.Observe({0.4f, 0, 0}, {0, 0, 1}) &&
        motion.Observe({0.4f, 0, 0}, {0, 0, 1}) && !motion.Observe({0.4f, 0, 0}, {0, 0, 1}) &&
        motion.Observe({0.4f, 0, 0}, {0.06f, 0, 0.9981984f}), "motion wake lasts four updates and can start again on a turn");
  gu::Lighting a, b;
  a.to_sun = b.to_sun = {0, 1, 0};
  b.street = 0.03f;
  Check(!gu::LightingChanged(a, b), "minor lamp changes can accumulate against the accepted lighting");
  b.street = 0.07f;
  Check(gu::LightingChanged(a, b), "a street lamp schedule step wakes the field");
  b = a;
  b.sun = {0.12f, 0.07f, 0.01f};
  Check(gu::LightingChanged(a, b), "a meaningful irradiance change wakes the field");
  b = a;
  ++b.weather;
  Check(gu::LightingChanged(a, b), "weather changes wake the field");
  std::vector<gu::ProbeLight> old_lights = {{7, {7, 0, 0}, 4}, {1, {1, 0, 0}, 4}};
  std::vector<gu::ProbeLight> new_lights = {{9, {9, 0, 0}, 6}, {7, {7, 0, 0}, 4}};
  std::sort(old_lights.begin(), old_lights.end());
  std::sort(new_lights.begin(), new_lights.end());
  std::vector<uint64_t> wakes;
  gu::VisitChangedLights(old_lights, new_lights, [&](const gu::ProbeLight& light) { wakes.push_back(light.key); });
  Check(wakes == std::vector<uint64_t>{1, 9}, "turning a light off and another on wakes both footprints independent of list order");
}

void TestBudgets() {
  const uint32_t clean = gu::ProbeWord(0, 0, 170);
  const uint32_t dirty = gu::ProbeWord(1, gu::kProbeDirty, 170);
  Check(gu::RefreshRays(clean, 64, 1.0f, true) == 32, "steady probes use 32 rays");
  Check(gu::RefreshRays(gu::ProbeWord(0, gu::kProbeRetestInside | gu::kProbeMaySleep, 1), 64, 1.0f, true) == 32, "regular schedule flags keep the established 32-ray refresh");
  Check(gu::RefreshRays(dirty, 64, 1.0f, true) == 64, "new and stale probes use 64 rays");
  Check(gu::RefreshRays(clean, 64, 4.0f, true) == 64, "warm-up keeps the full ray count");
  Check(gu::RefreshRays(clean, 64, 1.0f, false) == 64, "the A/B keeps the full ray count");
  Check(gu::RefreshRays(clean, 32, 1.0f, true) == 32, "the lean field keeps its original ray count");
  const gu::Dispatch d = gu::DispatchFor(61440);
  Check(d.x == 4096 && d.y == 15 && uint64_t(d.x) * d.y >= 61440, "the largest plan's blend dispatch: 4096 x 15 groups");
  Check(gu::TraceGroups(61440, 64) == 61440 && gu::TraceGroups(1536, 32) == 768, "the trace dispatch: a group a probe at 64 rays, two probes a group at 32");
  Check(gu::DispatchFor(61440).y <= 65535 && gu::DispatchFor(65536 * 4).y <= 65535, "no dispatch dimension passes 65,535");
  const gu::Budget lean = gu::BudgetOf(gi::BudgetOf(gi::Layout::kLean)), standard = gu::BudgetOf(gi::BudgetOf(gi::Layout::kStandard)), extended = gu::BudgetOf(gi::BudgetOf(gi::Layout::kExtended));
  Check(lean.rays_per_probe == 32 && standard.rays_per_probe == 64 && extended.rays_per_probe == 64, "rays a probe: 32, 64, 64");
  Check(lean.probes_per_frame == 384 && standard.probes_per_frame == 7168 && extended.probes_per_frame == 15360, "probes a frame: 384, 7,168, 15,360");
  Check(lean.rays_per_frame == 12288 && standard.rays_per_frame == 458752 && extended.rays_per_frame == 983040, "rays a frame: 12 K, 459 K, 983 K (M3 section 6)");
  Check(lean.chunk_probes == 384 && standard.chunk_probes == 7168 && extended.chunk_probes == 15360, "a chunk is the layout's probes a frame");
  Check(lean.ray_buffer_bytes == 196608 && standard.ray_buffer_bytes == 7340032 && extended.ray_buffer_bytes == 15728640, "the ray buffers: 0.19, 7.0 and 15.0 MiB, exactly the field owner's update reserve");
  Check(extended.max_probes == 61440 && uint64_t(extended.max_probes) * 4 == extended.list_bytes, "the probe list holds four chunks of one uint a probe");
  Check(lean.cap_ms == 0.45f && standard.cap_ms == 0.9f && extended.cap_ms == 1.8f && gu::CapMs(gi::Layout::kNone) == 0.0f, "the governor's caps by layout");
  // a smaller field (the order of loss) has a smaller update
  const gu::Budget reduced = gu::BudgetOf(gf::FitAt(gi::Layout::kStandard, gf::Step::kNoThirdCascade).budget);
  Check(reduced.cascades == 2 && reduced.probes_per_frame == 6144 && reduced.chunk_probes == 6144 && reduced.ray_buffer_bytes < standard.ray_buffer_bytes, "a field without its third cascade updates 6,144 probes a frame");
  Check(gu::BudgetOf(gi::Budget{}).chunk_probes == 0, "no field, no update");
  // every tier / level cell: the layout and the update's rays
  for (const gi::Rung rung : {gi::Rung::kNone, gi::Rung::kWeak, gi::Rung::kStrong}) {
    for (const gi::Level level : {gi::Level::kOff, gi::Level::kHigh, gi::Level::kUltra}) {
      const gi::Layout layout = gi::LayoutFor(rung, level);
      const gu::Budget b = gu::BudgetOf(gi::BudgetOf(layout));
      if (g_verbose) std::printf("    %-6s %-5s %-8s: %5u probes, %2u rays a probe, %8u rays a frame, ray buffer %6.2f MiB, cap %.2f ms\n", gi::RungName(rung).data(), gi::LevelName(level).data(),
                                 gi::LayoutName(layout).data(), b.probes_per_frame, b.rays_per_probe, b.rays_per_frame, double(b.ray_buffer_bytes) / (1024.0 * 1024.0), double(b.cap_ms));
      Check((layout == gi::Layout::kNone) == (b.chunk_probes == 0) && b.rays_per_frame == gi::BudgetRaysPerFrame(gi::BudgetOf(layout)), std::string("cell ") + gi::RungName(rung).data() + "/" + gi::LevelName(level).data() + " runs the layout's rays");
    }
  }
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------
double Field(const std::string& line, const char* pattern) {
  std::smatch m;
  const std::regex re(pattern);
  return std::regex_search(line, m, re) ? std::stod(m[1].str()) : -1.0;
}

void TestCensus() {
  uint32_t d[gu::kStatWords] = {};
  d[gu::kStatRays] = 100000;
  d[gu::kStatHit] = 62000;
  d[gu::kStatEscaped] = 38000;
  d[gu::kStatBack] = 2000;
  d[gu::kStatLod] = 5000;
  d[gu::kStatBlended] = 1000;
  d[gu::kStatInside] = 300;
  d[gu::kStatChanged] = 40;
  d[gu::kStatRelit] = 7;
  d[gu::kStatConverged] = 950;
  d[gu::kStatLumaSum] = uint32_t(0.25 * 1000 * gu::kFixedScale);
  d[gu::kStatSkySum] = uint32_t(0.5 * 1000 * gu::kFixedScale);
  d[gu::kStatHist0 + gu::HistBin(0.05f)] = 100;
  d[gu::kStatHist0 + gu::HistBin(0.25f)] = 800;
  d[gu::kStatHist0 + gu::HistBin(2.0f)] = 100;
  const gu::StatSummary s = gu::DecodeStats(d);
  Check(s.rays == 100000 && std::fabs(s.hit_share - 0.62) < 1e-9 && std::fabs(s.escaped_share - 0.38) < 1e-9 && std::fabs(s.back_share - 0.02) < 1e-9 && std::fabs(s.lod_share - 0.05) < 1e-9, "the shares of the rays");
  Check(s.probes_blended == 1000 && s.probes_inside == 300 && std::fabs(s.changed_share - 0.04) < 1e-9 && std::fabs(s.converged_share - 0.95) < 1e-9 && s.probes_relit == 7, "the probes' counts and shares");
  Check(std::fabs(s.mean_irradiance - 0.25) < 1e-3 && std::fabs(s.mean_sky - 0.5) < 1e-3, Num("the means (irradiance %.3f, sky %.3f)", s.mean_irradiance, s.mean_sky));
  Check(s.p5 <= gu::HistBinUpper(gu::HistBin(0.05f)) * 1.0001f && s.p5 >= 0.05f && s.p95 >= 2.0f * 0.99f && s.p95 <= gu::HistBinUpper(gu::HistBin(2.0f)) * 1.0001f && s.p5 < s.p95,
        Num("the percentiles are the upper edges of the bins (p5 %.3f, p95 %.3f)", s.p5, s.p95));
  // a counter that wrapped
  uint32_t now[gu::kStatWords] = {}, last[gu::kStatWords] = {}, out[gu::kStatWords];
  last[gu::kStatRays] = 0xFFFFFF00u;
  now[gu::kStatRays] = 0x100u;
  gu::StatDelta(now, last, out);
  Check(out[gu::kStatRays] == 0x200u, "the difference of two readbacks is right across a wrap");
  Check(gu::DecodeStats(out).converged_share == 0.0 && gu::DecodeStats(out).p95 == 0.0, "an interval with no blended probe decodes to zeros");

  // the lines, against the chain's regexes (perf/scripts/city_rt_m3_report.py)
  gu::IntervalInfo info;
  info.frame = 240;
  info.frames = 120;
  info.probes = {4096 * 30, 2048 * 30, 1024 * 30, 0};
  info.cascades = 3;
  info.cuts = 2;
  info.budget_scale = 1.0f;
  info.finest_dirty_share = 0.01;
  const std::string line = gu::IntervalLine(info, s);
  if (g_verbose) std::printf("    [rt gi] %s\n", line.c_str());
  std::smatch m;
  Check(std::regex_match(line, m, std::regex("frame (\\d+): (.*)")) && m[1].str() == "240", "the interval line is 'frame N: ...' (the report's RT_GI regex)");
  Check(Field(line, "([0-9.]+)% escaped") == 38.0 && Field(line, "([0-9.]+)% back faces") == 2.0 && Field(line, "(\\d+) probes inside geometry") == 300 && Field(line, "([0-9.]+)% converged") == 95.0 &&
            Field(line, "([0-9.]+)% changed") == 4.0 && Field(line, "\\(([0-9.]+)% hit") == 62.0,
        "the report's fields: escaped, back faces, probes inside geometry, converged, changed, hit");
  Check(Has(line, "probes updated over the last 120 frames (122880 in cascade 0, 61440 in cascade 1, 30720 in cascade 2)") && Has(line, "100000 rays") && Has(line, "(7 relit)") && Has(line, "2 cuts") &&
            Has(line, "finest cascade 1.0% dirty") && Has(line, "irradiance mean 0.250 (p5 ") && Has(line, "sky visibility mean 0.50, budget scale 1.00"),
        "the interval line's words and order are the README's");
  const std::string first = gu::FirstLine(1, gi::Layout::kStandard, 3, 64, 1200, 90, 400, 0.5f, 0.97f, 1.0f);
  Check(Has(first, "frame 1: first update: layout standard (3 cascades), 64 rays a probe, hit shading GI mode, 1200 instances in the top level (90 hi detail and 400 lod sectors), bounce weight 0.50, hysteresis 0.97, strength 1.00"),
        "the first line is the README's");
  const std::string cut = gu::CutLine(900, gu::CutReason(false, false, 398.0), 49152, 1.0f * gi::kCutWarmupBudgetScale, gi::kCutWarmupFrames);
  std::smatch cm;
  Check(std::regex_match(cut, cm, std::regex("cut at frame (\\d+): (.*?): (\\d+) probes re-addressed, budget x(.*) for (\\d+) frames, fade from 0")) && cm[2].str() == "camera step 398 m" && cm[3].str() == "49152",
        "the cut line matches the report's RT_CUT regex");
  Check(gu::CutReason(true, false, 0) == "hour jump" && gu::CutReason(false, true, 0) == "warp", "the cut reasons");
  const std::string rate = gu::RateLine(240, 458752.0, 0.5, 0.12);
  Check(Has(rate, "frame 240: rays 917.5 M/s, trace 0.500 ms, blend 0.120 ms"), "the rate line is the README's (rays a second of trace time)");
}

// ---------------------------------------------------------------------------
// the sources: the shaders' registers, the CMake lines, the wiring
// ---------------------------------------------------------------------------
std::string Reg(const char* kind, uint32_t n, uint32_t space) { return std::string("register(") + kind + std::to_string(n) + ", space" + std::to_string(space) + ")"; }

void TestSources(const std::string& dir) {
  const std::string trace = ReadFile(dir + "/hlsl/gi_update_trace_cs.hlsl");
  const std::string blend = ReadFile(dir + "/hlsl/gi_update_blend_cs.hlsl");
  const std::string common = ReadFile(dir + "/hlsl/gi_update_common.hlsli");
  if (trace.empty() || blend.empty() || common.empty()) {
    Check(false, "the shaders were not found in " + dir + "/hlsl");
    return;
  }
  // the trace: set 0 the textures, 1 the samplers, 2 the pass set, 3 the ray tracing set, the push block at space 4
  Check(Has(trace, Reg("t", gu::kBindRows, gu::kTracePassSet)) && Has(trace, Reg("t", gu::kBindProbes, gu::kTracePassSet)) && Has(trace, Reg("u", gu::kBindRays, gu::kTracePassSet)) &&
            Has(trace, Reg("u", gu::kBindState, gu::kTracePassSet)) && Has(trace, Reg("u", gu::kBindStats, gu::kTracePassSet)),
        "the trace shader's pass set registers are the header's (t0, t1, u2, u5, u6 in space 2)");
  Check(Has(trace, "register(t0, space0)") && Has(trace, "register(s0, space1)") && Has(trace, "#define CAR_RT_SPACE 3") && Has(trace, "#define RT_WORLD_SPACE 3") && Has(trace, "#define CITY_HIT_SPACE 3") &&
            Has(trace, "register(b0, space4)"),
        "the trace shader: the bindless textures at space 0, the samplers at 1, the ray tracing set at 3, the push block at (b0, space4)");
  Check(gu::kTraceTextureSet == 0 && gu::kTraceSamplerSet == 1 && gu::kTracePassSet == 2 && gu::kTraceSceneSet == 3 && gu::kTracePushSpace == 4, "the trace layout's sets in the host's numbers");
  Check(Has(blend, Reg("t", gu::kBindRows, gu::kBlendPassSet)) && Has(blend, Reg("t", gu::kBindProbes, gu::kBlendPassSet)) && Has(blend, Reg("u", gu::kBindRays, gu::kBlendPassSet)) &&
            Has(blend, Reg("u", gu::kBindIrradiance, gu::kBlendPassSet)) && Has(blend, Reg("u", gu::kBindMoments, gu::kBlendPassSet)) && Has(blend, Reg("u", gu::kBindState, gu::kBlendPassSet)) &&
            Has(blend, Reg("u", gu::kBindStats, gu::kBlendPassSet)) && Has(blend, "register(b0, space1)") && gu::kBlendPushSpace == 1,
        "the blend shader's registers are the header's (t0, t1, u2 .. u6 in space 0, the push block at (b0, space1))");
  Check(Has(blend, "[numthreads(10, 10, 1)]") && Has(trace, "[numthreads(64, 1, 1)]") && gu::kBlendGroupSize == 10 && gu::kTraceGroup == 64, "the thread groups: 10 x 10 for the blend, 64 for the trace");
  Check(Has(blend, "image_format(\"rgba16f\")") && Has(blend, "image_format(\"rg16f\")"), "the blend gives the storage image formats to SPIR-V");
  for (const std::string* t : {&trace, &blend}) {
    Check(Has(*t, "uint4 control;") && Has(*t, "uint4 flags;") && Has(*t, "float4 tune;") && Has(*t, "float4 tune2;"), "a shader's push block is four rows of 16 bytes (64)");
  }
  Check(gu::kStatWords * 4 <= gu::kStatBytes && Has(trace, "4u * 4u, tracedSum") && Has(trace, "8u * 4u, lodSum") && Has(blend, "Count(9u,") && Has(blend, "Count(skipped == GIU_RAY_SLEEPING ? 17u : 16u, 1u)") && Has(blend, "Count(18u + GiuHistBin(newMean), 1u)"),
        "the shaders add to the counters' words the header names (4 .. 8 in the trace, 0 .. 3, 9 .. 16 and the histogram from 18 in the blend)");
  // the shaders include the library and the field's files they need
  Check(Has(trace, "#include \"city_hit_shade.hlsli\"") && Has(trace, "CITY_HIT_MODE_GI") && Has(trace, "CITY_HIT_P_SUN_RAY") && Has(trace, "CITY_HIT_P_EMISSIVE") && Has(trace, "CITY_HIT_P_LAMP_SHADOW") &&
            Has(trace, "#define CITY_HIT_FEEDBACK(P, N)") && Has(trace, "p.units = 1.0f") && Has(trace, "0x80u"),
        "the trace shades in the library's GI mode: the sun ray, the glow, one stochastic lamp, the field's feedback, the paint's units, the city's instance mask only");
  Check(!Has(trace, "CITY_HIT_AMBIENT") && Has(trace, "GiLookup(") && !Has(trace, "GiAmbientAt("), "the probes' own hits take the field's surfaces only, never AmbientAt (gi_frame.h)");
  // a hit with no data is a miss (CITY-RT-REVIEW M5): the hit flag, the distance and the LOD flag are set only after CityHitDecode said the data is valid
  {
    const size_t decode = At(trace, "const CityHit decoded = CityHitDecode(");
    const size_t valid = At(trace, "if (decoded.valid != 0u)");
    const size_t flag = At(trace, "flags = GIU_RAY_HIT;");
    const size_t dist = At(trace, "distance = hit.t;");
    const size_t lod = At(trace, "lods = (hit.inst.instanceId & 1u) != 0u ? 1u : 0u;");
    Check(decode != std::string::npos && decode < valid && valid < flag && flag < dist && dist < lod && Count(trace, "flags = GIU_RAY_HIT;") == 1 && Count(trace, "distance = hit.t;") == 1 &&
              Count(trace, "decoded.valid") == 1,
          "the trace sets the hit flag, the hit's distance and the LOD flag only inside the decoded.valid block: a hit with no data stays a miss");
  }

  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "larecomp_gpu_host_shader_rt(gi_update_trace_cs cs_6_5)") && Has(cmake, "larecomp_gpu_host_shader(gi_update_blend_cs cs_6_0)") && Has(cmake, "    gi_update.cpp\n") &&
            Has(cmake, "add_executable(larecomp_gi_update_rules_test EXCLUDE_FROM_ALL gi_update_rules_test.cpp)"),
        "CMake builds the two shaders (the trace as a ray tracing shader), gi_update.cpp and this test");
  const std::string shaders = ReadFile(dir + "/host_shaders.h");
  Check(Has(shaders, "GiUpdateTraceShader()") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(gi_update_trace_cs)") && Has(shaders, "GiUpdateBlendShader()") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(gi_update_blend_cs)") &&
            Count(shaders, "gi_update_trace_cs.hlsl.") == 2 && Count(shaders, "gi_update_blend_cs.hlsl.") == 2,
        "host_shaders.h has both shaders' blobs for both backends");
  const std::string precache = ReadFile(dir + "/host_precache.h");
  Check(Has(precache, "kGiUpdate = 33,"), "the precache has the update's owner key");

  const std::string cpp = ReadFile(dir + "/gi_update.cpp");
  Check(Count(cpp, "pass_set_builder_.add") == int(gu::kSetViews) && Has(cpp, "CreateDescriptorSet(pass_set_builder_, kSetViews, 0,") && gu::kSetViews == 7,
        "the pass set has seven views and is created with that count (D3D12 counts them: the two mistakes of 2026-11-21)");
  Check(Has(cpp, "Mark(list, \"rt gi trace\")") && Has(cpp, "Mark(list, \"rt gi blend\")") && At(cpp, "Mark(list, \"rt gi trace\")") < At(cpp, "Mark(list, \"rt gi blend\")"), "the passes' pass-timing labels are `rt gi trace` then `rt gi blend`");
  Check(Has(cpp, "to_read") && Has(cpp, "Layout::SHADER_READ") && Has(cpp, "to_write") && Has(cpp, "Layout::GENERAL"), "the field's textures are read by the trace and written by the blend");
  Check(Has(cpp, "kPrefix = \"[rt gi]\""), "the census lines carry the prefix the report reads");
  Check(Has(cpp, "hit_scene_->ready() || hit_scene_->stats().committed == 0") || Has(cpp, "!hit_scene_->ready() || hit_scene_->stats().committed == 0"), "the update waits for the hit data's pool (a field of zeros must never replace the screen-space bounce light)");
  Check(Has(cpp, "scheduler_.Make(field_->scheduler()") && Has(cpp, "DrainSectorChanges(scheduled);") && Has(cpp, "MarkStaleBox(") && !Has(cpp, "in.governor_scale") && !Has(cpp, "governor_.AddSample("), "the update uses the fixed probe budget and wakes sectors that changed");
  const std::string renderer = ReadFile(dir + "/renderer.cpp");
  const std::string renderer_h = ReadFile(dir + "/renderer.h");
  Check(Has(renderer_h, "GiUpdate gi_update_;") && Has(renderer_h, "#include \"gi_update.h\""), "renderer.h owns the update");
  Check(Has(renderer, "gi_update_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_field_, car_rt_, city_rt_, city_hit_, rt_scene_, rt_world_, update_listed);") && Has(renderer, "gi_integration_.SetUpdate(") &&
            At(renderer, "gi_update_.Initialize(") > At(renderer, "gi_integration_.Initialize(&gi_field_, &gi_apply_, &ao_, &city_rt_);") && At(renderer, "gi_update_.Initialize(") < At(renderer, "passes_.precache().Start();"),
        "the renderer makes the update after the integration and the world frame, before the precache starts, and gives it to the integration only where it exists");
  Check(Count(renderer, "gi_update_.Shutdown();") == 2 && Has(renderer, "gi_update_.CensusText()") && Has(renderer, "AddHolder(\"probe update\""),
        "the update is shut down on both paths, in the census and in the video memory holders");
  Check(Has(renderer, "gi_integration_.Shutdown();  // it holds pointers to the field, the apply, the AO and the city scene: first\n    gi_update_.Shutdown();") ||
            Has(renderer, "gi_integration_.Shutdown();  // it holds pointers to the field, the apply, the AO and the city scene: first\n  gi_update_.Shutdown();"),
        "the update is shut down right after the integration, before the field");
  const std::string integ = ReadFile(dir + "/gi_integration.cpp");
  Check(Has(integ, "want_(decision.level);") && At(integ, "want_(decision.level);") < At(integ, "field_->BeginFrame(in);") && Has(integ, "update_(list, frame_slot, field_->update_frame());"),
        "the integration asks the update for its structures before the field's frame begins, and calls the update between the maintenance and the weights");
  const std::string city = ReadFile(dir + "/city_rt_scene.h") + ReadFile(dir + "/city_rt_scene.cpp");
  Check(Has(city, "NoteSectorChange(sector, true);") && Has(city, "NoteSectorChange(s.sector, false);"), "both evicting and building a sector wake the affected probes");
  const std::string timing = ReadFile(dir + "/pass_timing.h") + ReadFile(dir + "/pass_timing.cpp");
  Check(Has(timing, "double LabelMsPerFrame(std::string_view label) const;") && Has(timing, "name.starts_with(\"rt gi\")"), "the pass timing reads a label's time and lists the update's labels");
  // the library files the update uses are not edited (they are H4's and R7's)
  Check(Has(ReadFile(dir + "/hlsl/city_hit_shade.hlsli"), "#ifdef CITY_HIT_FEEDBACK"), "the library's feedback hook is the one the trace defines");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-v") {
      g_verbose = true;
    } else {
      dir = argv[i];
    }
  }
  if (dir.empty()) {
    std::string file = __FILE__;
    const size_t slash = file.find_last_of("/\\");
    dir = slash == std::string::npos ? "." : file.substr(0, slash);
  }
  TestNumbers();
  TestHalf();
  TestHlslTwin();
  TestRaySet();
  TestClassification();
  TestBlend();
  TestScheduler();
  TestPhasedScheduler();
  TestBudgets();
  TestCensus();
  TestSources(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
