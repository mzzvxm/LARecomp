// Check of RtDenoise (rt_denoise_rules.h, ssr_denoise_rules.h, hlsl/rt_denoise_common.hlsli, hlsl/rt_denoise_engine.hlsli, the two consumers' headers and the shaders built from them, and the sources that
// use them: rt_denoise.h / rt_denoise.cpp, ssr.cpp, host_shaders.h, the build lists, the settings). A program of its own: no renderer, no ReXGlue, no GPU; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_rt_denoise_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_rt_denoise_test [<src/gpu dir>] [--print]
//
// It checks: the numbers and the functions of the .hlsli against the twins (the .hlsli is compiled ITSELF as C++ through a shim of the HLSL types and every function is compared on thousands of random
// inputs), the radius law (a near mirror is left alone, a rough surface with a far hit takes the widest kernel, a long history shrinks it), the iterations a radius takes, the weights, the virtual motion
// (the reprojection point against ssr_common.hlsli's own virtual image, and against the geometry of a mirror and a moving camera), the history's rules (the weight, the length, the variance clip against
// ssr_common.hlsli's), a CPU model of both stages on synthetic frames (noise falls, the mean stays, edges of depth, normal, roughness and hit distance are not crossed, a near mirror and a pixel with no share
// are not touched, a pixel that is no signal leaks nothing, a step change in the scene is clipped at once), the screen-space reflections' decision over every combination, the setting, the census, the push
// blocks' words against the .hlsli's #defines; given the src/gpu folder it also reads the sources.

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>  // global scope: pbr_screen_shim.h needs it and is included inside namespace hlsl_shim
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "rt_denoise_rules.h"
#include "ssr_denoise_rules.h"
#include "ssr_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++ (the shim of what ssr_common.hlsli and rt_denoise_common.hlsli use; gbuffer_common.hlsli and ao_gbuffer_normal.hlsli first, as the shaders include them)
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
inline float pow(float a, float b) { return std::pow(a, b); }
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
#include "pbr_screen_shim.h"   // what the contract (hlsl/pbr_brdf.hlsli, included by ssr_common.hlsli) needs and this shim does not have (PBR level 1C)
#include "hlsl/ao_gbuffer_normal.hlsli"
#include "hlsl/ssr_common.hlsli"
#include "hlsl/rt_denoise_common.hlsli"
}  // namespace hlsl_shim

namespace rd = mcla::gpu::rt_denoise;
namespace sd = mcla::gpu::ssr_denoise;
namespace ssr = mcla::gpu::ssr;
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
bool NearRel(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }

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
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t ia = At(text, a);
  const size_t ib = At(text, b);
  return ia != std::string::npos && ib != std::string::npos && ia < ib;
}
// The text of a function from its first line to the closing "\n}\n".
std::string FunctionText(const std::string& text, const std::string& head) {
  const size_t b = At(text, head);
  if (b == std::string::npos) return std::string();
  const size_t e = At(text.substr(b), "\n}\n");
  return e == std::string::npos ? std::string() : text.substr(b, e);
}
// The value of a "#define NAME value" line (a number), -1 where it is not there.
long DefineOf(const std::string& text, const std::string& name) {
  const std::string key = "#define " + name + " ";
  const size_t at = At(text, key);
  if (at == std::string::npos) return -1;
  return std::strtol(text.c_str() + at + key.size(), nullptr, 10);
}

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t Next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(s >> 33);
  }
  float Unit() { return float(Next() & 0xFFFFFF) * (1.0f / 16777216.0f); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
  // a roughly normal number (the sum of four units)
  float Gauss() { return (Unit() + Unit() + Unit() + Unit() - 2.0f) * 1.7320508f; }
};

// ---------------------------------------------------------------------------
// the numbers and the functions: the twins against the .hlsli
// ---------------------------------------------------------------------------

void TestNumbers() {
  Check(rd::kMaxRadius == hs::kRtDnMaxRadius && rd::kLobeScale == hs::kRtDnLobeScale && rd::kDiffuseRadius == hs::kRtDnDiffuseRadius && rd::kMatureLength == hs::kRtDnMatureLength &&
            rd::kMatureScale == hs::kRtDnMatureScale && rd::kStepReach == hs::kRtDnStepReach && rd::kMinShare == hs::kRtDnMinShare && rd::kClipGamma == hs::kRtDnClipGamma &&
            rd::kClipMargin == hs::kRtDnClipMargin && rd::kMaxLength == hs::kRtDnMaxLength && rd::kVirtualRough == hs::kRtDnVirtualRough && rd::kVirtualFull == hs::kRtDnVirtualFull,
        "the twin's numbers are the .hlsli's");
  Check(rd::kClipGamma == hs::kSsrClipGamma && rd::kClipMargin == hs::kSsrClipMargin, "the clip's numbers are the reflections' own (the same variance clip)");
  Check(RT_DN_PROFILE_SPECULAR == 0u && RT_DN_PROFILE_DIFFUSE == 1u && uint32_t(rd::Profile::kSpecular) == 0u && uint32_t(rd::Profile::kDiffuse) == 1u, "the profile numbers are the .hlsli's");
  Check(rd::kMaxIterations == 3 && rd::kMaxRadius <= 2.0f + 4.0f + 8.0f, "three iterations (steps 1, 2, 4) reach 14 pixels: the widest kernel (12) is inside it");
  for (int p = 0; p < 2; ++p) {
    const rd::Profile profile = rd::Profile(p);
    const uint32_t hp = uint32_t(p);
    Check(rd::DepthSigma(profile) == hs::RtDnDepthSigma(hp) && rd::NormalPower(profile) == hs::RtDnNormalPower(hp) && rd::RoughSigma(profile) == hs::RtDnRoughSigma(hp) && rd::LumaSigma(profile) == hs::RtDnLumaSigma(hp) &&
              rd::HitSigma(profile) == hs::RtDnHitSigma(hp),
          "the profile's weights' numbers are the .hlsli's (profile " + std::to_string(p) + ")");
  }
  Check(rd::HitSigma(rd::Profile::kDiffuse) == 0.0f && rd::HitSigma(rd::Profile::kSpecular) > 0.0f, "only the specular profile has the hit distance as an edge");
  // the B3 spline: one axis sums to 1, so five by five does
  float sum = 0.0f;
  for (int d = -2; d <= 2; ++d) sum += rd::Kernel1(d);
  Check(Near(sum, 1.0, 1e-6) && rd::Kernel1(0) == 0.375f && rd::Kernel1(1) == rd::Kernel1(-1) && rd::Kernel1(3) == 0.0f, "the kernel is the B3 spline 1 4 6 4 1 over 16 and sums to one");
}

void TestFunctionsAgainstHlsl() {
  Rng rng;
  bool all = true;
  int bad_radius = 0, bad_weights = 0, bad_temporal = 0;
  for (int i = 0; i < 20000; ++i) {
    const float len = rng.Range(0.0f, 40.0f);
    const float cone = rng.Range(0.0f, 0.5f);
    const float hit = rng.Unit() < 0.1f ? 0.0f : rng.Range(0.0f, 300.0f);
    const float z = rng.Range(0.5f, 200.0f);
    const float angle = rng.Range(0.0003f, 0.004f);
    all = all && rd::LengthScale(len) == hs::RtDnLengthScale(len) && rd::LobePixels(cone, hit, z, angle) == hs::RtDnLobePixels(cone, hit, z, angle);
    for (int p = 0; p < 2; ++p) {
      const float a = rd::Radius(rd::Profile(p), cone, hit, z, angle, len);
      const float b = hs::RtDnRadius(uint32_t(p), cone, hit, z, angle, len);
      if (a != b) ++bad_radius;
      for (uint32_t step : {1u, 2u, 4u}) {
        if (rd::StepTaken(a, step) != hs::RtDnStepTaken(a, step)) ++bad_radius;
      }
    }
    const float share = rng.Range(-0.1f, 1.2f);
    all = all && rd::Mix(share) == hs::RtDnMix(share);
    // the weights
    const float plane = rng.Range(0.0f, 5.0f), sigma = rng.Range(0.0f, 0.2f), ndot = rng.Range(-1.0f, 1.0f), power = rng.Range(1.0f, 40.0f);
    const float r0 = rng.Unit(), r1 = rng.Unit(), l0 = rng.Range(-3.0f, 30.0f), l1 = rng.Range(-3.0f, 30.0f), var = rng.Range(-0.1f, 50.0f), ls = rng.Range(0.5f, 6.0f);
    const float h0 = rng.Unit() < 0.1f ? 0.0f : rng.Range(0.0f, 4096.0f), h1 = rng.Unit() < 0.1f ? 0.0f : rng.Range(0.0f, 4096.0f), hsig = rng.Unit() < 0.2f ? 0.0f : rng.Range(0.2f, 2.0f);
    if (rd::DepthWeight(plane, z, sigma) != hs::RtDnDepthWeight(plane, z, sigma)) ++bad_weights;
    if (rd::NormalWeight(ndot, power) != hs::RtDnNormalWeight(ndot, power)) ++bad_weights;
    if (rd::RoughWeight(r0, r1, sigma) != hs::RtDnRoughWeight(r0, r1, sigma)) ++bad_weights;
    if (rd::LumaWeight(l0, l1, var, ls) != hs::RtDnLumaWeight(l0, l1, var, ls)) ++bad_weights;
    if (rd::HitWeight(h0, h1, hsig) != hs::RtDnHitWeight(h0, h1, hsig)) ++bad_weights;
    const int d = int(rng.Next() % 7) - 3;
    if (rd::Kernel1(d) != hs::RtDnKernel1(d)) ++bad_weights;
    const float cnt = float(rng.Next() % 12);
    if (rd::Variance(l0, l1 * l1, cnt) != hs::RtDnVariance(l0, l1 * l1, cnt)) ++bad_weights;
    if (rd::Luma(l0, l1, r0) != hs::RtDnLuma(l0, l1, r0)) ++bad_weights;
    // the temporal rules
    const float rough = rng.Unit();
    const hs::float3 receiver(rng.Range(-30.0f, 30.0f), rng.Range(-10.0f, 10.0f), rng.Range(0.5f, 100.0f));
    const hs::float3 mine = hs::RtDnReprojectionPoint(receiver, hit, rough);
    const rd::Vec3 twin = rd::ReprojectionPoint({receiver.x, receiver.y, receiver.z}, hit, rough);
    if (mine.x != twin.x || mine.y != twin.y || mine.z != twin.z) ++bad_temporal;
    if (rd::VirtualShare(rough, hit) != hs::RtDnVirtualShare(rough, hit)) ++bad_temporal;
    const float exp_z = z * rng.Range(0.8f, 1.2f);
    if (rd::SameSurface(z, exp_z) != hs::RtDnSameSurface(z, exp_z)) ++bad_temporal;
    if (rd::HistoryWeight(len) != hs::RtDnHistoryWeight(len)) ++bad_temporal;
    const bool used = (rng.Next() & 1u) != 0;
    if (rd::NextLength(len, used) != hs::RtDnNextLength(len, used)) ++bad_temporal;
    const float mean = rng.Range(-2.0f, 5.0f), sg = rng.Range(0.0f, 2.0f), lo = mean - rng.Range(0.0f, 3.0f), hi = mean + rng.Range(0.0f, 3.0f);
    if (rd::BoxLo(mean, sg, lo, hi) != hs::RtDnBoxLo(mean, sg, lo, hi) || rd::BoxHi(mean, sg, lo, hi) != hs::RtDnBoxHi(mean, sg, lo, hi)) ++bad_temporal;
    const rd::Vec3 v{rng.Range(-4.0f, 8.0f), rng.Range(-4.0f, 8.0f), rng.Range(-4.0f, 8.0f)};
    const rd::Vec3 bl{rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f)};
    const rd::Vec3 bh{bl.x + rng.Range(0.0f, 3.0f), bl.y + rng.Range(0.0f, 3.0f), bl.z + rng.Range(0.0f, 3.0f)};
    const rd::Vec3 c0 = rd::ClipToBox(v, bl, bh);
    const hs::float3 c1 = hs::RtDnClipToBox(hs::float3(v.x, v.y, v.z), hs::float3(bl.x, bl.y, bl.z), hs::float3(bh.x, bh.y, bh.z));
    if (c0.x != c1.x || c0.y != c1.y || c0.z != c1.z) ++bad_temporal;
  }
  Check(all, "LengthScale, LobePixels, Mix: the twins are the .hlsli's on 20,000 inputs");
  Check(bad_radius == 0, "Radius (both profiles) and StepTaken: the twins are the .hlsli's on 20,000 inputs (" + std::to_string(bad_radius) + " differ)");
  Check(bad_weights == 0, "the weights, the kernel, the variance, the luminance: the twins are the .hlsli's on 20,000 inputs (" + std::to_string(bad_weights) + " differ)");
  Check(bad_temporal == 0, "the temporal rules (the reprojection point, the share, the surface test, the history's weight and length, the box and the clip): the twins are the .hlsli's (" + std::to_string(bad_temporal) + " differ)");
  // numbers only: every NaN (either sign, any payload), infinity and ordinary float
  int bad_number = 0;
  const auto from_bits = [](uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  };
  std::vector<float> specials = {from_bits(0x7FC00000u), from_bits(0xFFC00000u), from_bits(0x7F800001u), from_bits(0xFF800001u), from_bits(0x7FFFFFFFu), from_bits(0xFFFFFFFFu), from_bits(0x7F800000u),
                                 from_bits(0xFF800000u), from_bits(0x7F7FFFFFu), from_bits(0xFF7FFFFFu), from_bits(0x00000001u), from_bits(0x80000000u), 0.0f, -1.0f, 1.0e30f};
  for (int i = 0; i < 20000; ++i) specials.push_back(from_bits(rng.Next() ^ (rng.Next() << 16)));
  for (const float v : specials) {
    if (rd::IsNumber(v) != hs::RtDnIsNumber(v) || rd::IsNumber(v) != !(std::isnan(v) || std::isinf(v))) ++bad_number;
    const hs::float3 c = hs::RtDnSane(hs::float3(v, 1.0f, 2.0f));
    if (rd::IsNumber(v) ? (c.x != v || c.y != 1.0f || c.z != 2.0f) : (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f)) ++bad_number;  // one channel that is not a number makes the colour black
  }
  Check(bad_number == 0, "RtDnIsNumber and RtDnSane: NaN (either sign, any payload) and infinity are not numbers, a colour with one is black, every other value is kept (" + std::to_string(bad_number) + " differ)");
  // the guides: a pixel is usable when ok and every guide is a number
  int bad_guides = 0;
  for (int i = 0; i < 4000; ++i) {
    hs::RtDnPixel px;
    px.ok = (rng.Next() & 7u) != 0;
    float g[13];
    for (float& f : g) f = rng.Unit() < 0.1f ? specials[rng.Next() % 15] : rng.Range(-2.0f, 2.0f);
    px.colour = hs::float3(g[0], g[1], g[2]);
    px.z = g[3];
    px.pos = hs::float3(g[4], g[5], g[6]);
    px.n = hs::float3(g[7], g[8], g[9]);
    px.rough = g[10];
    px.hit = g[11];
    px.share = g[12];
    px.length = rng.Range(0.0f, 32.0f);
    bool expect = px.ok;
    for (int k = 3; k < 13; ++k) expect = expect && rd::IsNumber(g[k]);  // the colour (g[0..2]) is looked at apart
    if (hs::RtDnGuidesOk(px) != expect) ++bad_guides;
    hs::RtDnPixel bad_length = px;
    bad_length.length = from_bits(0x7FC00000u);
    if (hs::RtDnGuidesOk(bad_length)) ++bad_guides;
  }
  Check(bad_guides == 0, "RtDnGuidesOk: ok and every guide a number (the colour is looked at apart): a NaN or infinite depth, position, normal, roughness, hit distance, share or length is no signal (" + std::to_string(bad_guides) + " differ)");
}

// ---------------------------------------------------------------------------
// the radius law and the iterations
// ---------------------------------------------------------------------------

void TestRadius() {
  const float angle = 2.0f * 0.4f / 540.0f;   // 1080 lines at half size, a 0.4 tangent
  const auto alpha_of = [](float perceptual_rough) { return perceptual_rough * perceptual_rough; };
  // no hit, no kernel
  Check(rd::LobePixels(0.3f, 0.0f, 10.0f, angle) == 0.0f && rd::LobePixels(0.3f, -1.0f, 10.0f, angle) == 0.0f && rd::LobePixels(0.3f, 5.0f, 0.0f, angle) == 0.0f && rd::LobePixels(0.3f, 5.0f, 10.0f, 0.0f) == 0.0f,
        "a reflection with no hit distance (or no depth, or no pixel size) has no lobe to follow: the radius is 0");
  // the lobe's width: the hit far away is the angle of the lobe over the pixel's angle; a hit at the receiver is nothing
  const float far_hit = rd::LobePixels(0.25f, 1.0e6f, 10.0f, angle);
  Check(Near(far_hit, 0.25f / angle, 0.5), Num("a hit at infinity: the lobe's angle in pixels (%.1f against %.1f)", far_hit, 0.25f / angle));
  Check(rd::LobePixels(0.25f, 0.01f, 10.0f, angle) < 0.75f && rd::IterationsTaken(rd::LobePixels(0.25f, 0.01f, 10.0f, angle)) == 0, "a hit a centimetre from the surface (even of a rough lobe): the kernel is under 0.75 pixel, no iteration");
  bool monotonic = true;
  float last = -1.0f;
  for (float hit = 0.5f; hit < 500.0f; hit *= 1.5f) {
    const float r = rd::LobePixels(0.25f, hit, 10.0f, angle);
    monotonic = monotonic && r > last;
    last = r;
  }
  Check(monotonic, "a farther hit widens the kernel (strictly)");
  monotonic = true;
  last = -1.0f;
  for (float rough = 0.05f; rough <= 1.0f; rough += 0.05f) {
    const float r = rd::LobePixels(alpha_of(rough), 20.0f, 10.0f, angle);
    monotonic = monotonic && r > last;
    last = r;
  }
  Check(monotonic, "a rougher surface widens the kernel (strictly)");
  // the cases the design names
  const float near_mirror = rd::Radius(rd::Profile::kSpecular, alpha_of(0.05f), 5.0f, 10.0f, angle, 1.0f);
  Check(near_mirror < 0.75f && rd::IterationsTaken(near_mirror) == 0, Num("a near mirror (roughness 0.05, a hit 5 m away): radius %.2f, under 0.75: it takes no iteration", near_mirror));
  const float wet_near = rd::Radius(rd::Profile::kSpecular, alpha_of(0.08f), 5.0f, 12.0f, angle, 1.0f);   // a wet road: smoothness 0.92, the car behind it 5 m away
  const float wet_far = rd::Radius(rd::Profile::kSpecular, alpha_of(0.08f), 30.0f, 12.0f, angle, 1.0f);   // ... and a building 30 m away
  Check(wet_near >= 0.75f && wet_near < 1.5f && rd::IterationsTaken(wet_near) == 1 && wet_far >= 3.0f && wet_far < 3.5f && rd::IterationsTaken(wet_far) == 3,
        Num("a wet road (smoothness 0.92): a hit 5 m away takes one iteration (%.2f pixels), a hit 30 m away three (%.2f): the lobe's own width", wet_near, wet_far));
  const float rough_far = rd::Radius(rd::Profile::kSpecular, alpha_of(0.6f), 40.0f, 10.0f, angle, 1.0f);
  Check(rough_far == rd::kMaxRadius && rd::IterationsTaken(rough_far) == 3, "a rough surface with a far hit: the widest kernel, all three iterations");
  // the history shrinks it, gently
  const float fresh = rd::Radius(rd::Profile::kSpecular, 0.02f, 10.0f, 10.0f, angle, 1.0f);
  const float mature = rd::Radius(rd::Profile::kSpecular, 0.02f, 10.0f, 10.0f, angle, 12.0f);
  Check(fresh > 0.0f && Near(mature / fresh, rd::kMatureScale, 1e-4) && rd::LengthScale(1.0f) == 1.0f && Near(rd::LengthScale(12.0f), rd::kMatureScale, 1e-6) && Near(rd::LengthScale(100.0f), rd::kMatureScale, 1e-6) &&
            rd::LengthScale(0.0f) == 1.0f,
        "a history of 12 frames or more shrinks the kernel to 0.4 of a fresh pixel's; a fresh one keeps all of it");
  // the diffuse profile: a fixed radius, no hit distance
  Check(rd::Radius(rd::Profile::kDiffuse, 0.0f, 0.0f, 10.0f, angle, 1.0f) == rd::kDiffuseRadius && Near(rd::Radius(rd::Profile::kDiffuse, 0.5f, 99.0f, 3.0f, angle, 12.0f), rd::kDiffuseRadius * rd::kMatureScale, 1e-5),
        "the diffuse profile: six pixels whatever the hit, shortened by the history");
  // the iterations
  Check(!rd::StepTaken(0.74f, 1) && rd::StepTaken(0.75f, 1) && !rd::StepTaken(1.49f, 2) && rd::StepTaken(1.5f, 2) && !rd::StepTaken(2.99f, 4) && rd::StepTaken(3.0f, 4), "a pixel takes the iteration of step s from a radius of 0.75 s");
  Check(rd::IterationsTaken(0.0f) == 0 && rd::IterationsTaken(0.74f) == 0 && rd::IterationsTaken(0.75f) == 1 && rd::IterationsTaken(1.49f) == 1 && rd::IterationsTaken(1.5f) == 2 && rd::IterationsTaken(2.99f) == 2 &&
            rd::IterationsTaken(3.0f) == 3 && rd::IterationsTaken(12.0f) == 3,
        "the iterations a radius takes: 0, 1, 2, 3");
  // the share
  Check(rd::Mix(0.0f) == 0.0f && rd::Mix(0.049f) == 0.0f && rd::Mix(0.05f) == 0.05f && rd::Mix(0.5f) == 0.5f && rd::Mix(1.0f) == 1.0f && rd::Mix(2.0f) == 1.0f && rd::Mix(-1.0f) == 0.0f, "the share: nothing under 0.05, itself up to 1");
  // the group count of the host
  Check(rd::Groups(1) == 1 && rd::Groups(8) == 1 && rd::Groups(9) == 2 && rd::Groups(960) == 120 && rd::Groups(540) == 68, "the dispatch's groups of 8 round up");
  Check(rd::ImageBytes(960, 540) == 960ull * 540 * 8, "an RGBA16F image's bytes");
  // RT-LIGHTS-D1 part 2: the format a consumer may ask for (RGBA8 for the ray traced light visibility): the default is RGBA16F, so nothing of the other consumers changes
  Check(rd::ImageBytes(960, 540, rd::Format::kRgba16f) == rd::ImageBytes(960, 540) && rd::ImageBytes(960, 540, rd::Format::kRgba8) == 960ull * 540 * 4 && rd::BytesPerPixel(rd::Format::kRgba8) == 4 && rd::BytesPerPixel(rd::Format::kRgba16f) == 8 &&
            int(rd::Format::kRgba16f) == 0,
        "the result images' formats: RGBA16F (the default, 8 bytes) and RGBA8 (4 bytes)");
}

void TestWeights() {
  // each weight is 1 for the same surface and the same reflection, and falls monotonically
  const float z = 10.0f;
  Check(rd::DepthWeight(0.0f, z, 0.04f) == 1.0f && rd::NormalWeight(1.0f, 24.0f) == 1.0f && rd::RoughWeight(0.4f, 0.4f, 0.1f) == 1.0f && Near(rd::LumaWeight(2.0f, 2.0f, 0.5f, 3.0f), 1.0, 1e-6) &&
            rd::HitWeight(20.0f, 20.0f, 1.0f) == 1.0f,
        "every weight is 1 for the same surface and the same reflection");
  bool mono = true;
  float last = 2.0f;
  for (float d = 0.0f; d < 3.0f; d += 0.05f) {
    const float w = rd::DepthWeight(d, z, 0.04f);
    mono = mono && w <= last && w >= 0.0f && w <= 1.0f;
    last = w;
  }
  Check(mono && Near(rd::DepthWeight(0.4f, z, 0.04f), 0.5, 1e-6), "the depth weight falls with the plane distance and is a half at one width (4% of the depth)");
  mono = true;
  last = 2.0f;
  for (float c = 1.0f; c >= -1.0f; c -= 0.05f) {
    const float w = rd::NormalWeight(c, 24.0f);
    mono = mono && w <= last && w >= 0.0f;
    last = w;
  }
  Check(mono && rd::NormalWeight(0.0f, 24.0f) == 0.0f && rd::NormalWeight(-1.0f, 24.0f) == 0.0f && rd::NormalWeight(0.9f, 24.0f) < 0.1f, "the normal weight falls with the angle, is 0 at a right angle and for a back face, and 24 is tight (0.9 gives under a tenth)");
  Check(Near(rd::RoughWeight(0.2f, 0.4f, 0.1f), 0.2, 1e-6) && rd::RoughWeight(0.4f, 0.2f, 0.1f) == rd::RoughWeight(0.2f, 0.4f, 0.1f), "the roughness weight is symmetric, 1 / (1 + (d / sigma)^2)");
  Check(rd::LumaWeight(1.0f, 4.0f, 0.0f, 3.0f) < 0.01f && rd::LumaWeight(1.0f, 4.0f, 4.0f, 3.0f) > 0.5f, "the luminance weight: a flat neighbourhood stops a jump, a noisy one (deviation 2) lets a difference of 3 through");
  Check(Near(rd::HitWeight(5.0f, 5.0f * 2.0f + 1.0f, 1.0f), 0.5, 1e-6) && rd::HitWeight(5.0f, 0.0f, 1.0f) == 0.0f && rd::HitWeight(0.0f, 0.0f, 1.0f) == 1.0f && rd::HitWeight(5.0f, 50.0f, 0.0f) == 1.0f,
        "the hit weight: an octave apart is a half, a hit against no hit 0, two with none 1, and sigma 0 (the diffuse profile) turns it off");
  Check(Near(rd::Variance(6.0f, 14.0f, 3.0f), 14.0 / 3.0 - 4.0, 1e-5) && rd::Variance(0.0f, 0.0f, 0.0f) == 0.0f && rd::Variance(3.0f, 2.0f, 3.0f) == 0.0f, "the variance: E[x^2] - E[x]^2, never negative, 0 for an empty set");
}

// ---------------------------------------------------------------------------
// the temporal rules
// ---------------------------------------------------------------------------

void TestTemporalRules() {
  // the history's weight is the running mean's while it is young
  Check(rd::HistoryWeight(0.0f) == 0.0f && rd::HistoryWeight(1.0f) == 0.5f && Near(rd::HistoryWeight(3.0f), 0.75, 1e-6) && Near(rd::HistoryWeight(31.0f), 31.0 / 32.0, 1e-6) && Near(rd::HistoryWeight(1000.0f), 32.0 / 33.0, 1e-6) &&
            rd::HistoryWeight(-4.0f) == 0.0f,
        "the history's weight: n / (n + 1), n at most 32 (the floor of the blend is 1 / 33), nothing for none");
  Check(rd::NextLength(0.0f, false) == 1.0f && rd::NextLength(7.0f, false) == 1.0f && rd::NextLength(7.0f, true) == 8.0f && rd::NextLength(31.5f, true) == 32.0f && rd::NextLength(100.0f, true) == 32.0f &&
            rd::NextLength(-3.0f, true) == 1.0f,
        "the length: one more than the history's (up to 32), 1 where no history was used");
  // the running mean: blending with n / (n + 1) over the first frames is exactly the mean of the samples
  {
    Rng rng;
    float colour = 0.0f, len = 0.0f, sum = 0.0f;
    bool exact = true;
    for (int n = 1; n <= 30; ++n) {
      const float sample = rng.Range(0.0f, 2.0f);
      sum += sample;
      const bool used = n > 1;
      colour = used ? rd::Lerp(sample, colour, rd::HistoryWeight(len)) : sample;
      len = rd::NextLength(len, used);
      exact = exact && Near(colour, sum / float(n), 1e-4);
    }
    Check(exact, "blended by n / (n + 1), the history is the mean of every sample so far (30 frames)");
  }
  // the surface test
  Check(rd::SameSurface(10.0f, 10.0f) && rd::SameSurface(10.0f, 10.8f) && !rd::SameSurface(10.0f, 11.0f) && rd::SameSurface(0.0f, 0.04f) && !rd::SameSurface(0.0f, 0.2f), "the same surface: within 8% of the depth and 5 cm");
  Rng rng;
  bool same_as_ssr = true;
  for (int i = 0; i < 5000; ++i) {
    const float a = rng.Range(0.1f, 100.0f), b = a * rng.Range(0.7f, 1.3f);
    same_as_ssr = same_as_ssr && rd::SameSurface(a, b) == hs::SsrSameSurface(a, b);
  }
  Check(same_as_ssr, "the surface test is the reflections' own (SsrSameSurface)");
  // the clip is the reflections' own: the box and the point
  bool box_ok = true;
  bool inside = true;
  for (int i = 0; i < 5000; ++i) {
    const float mean = rng.Range(0.0f, 3.0f), sg = rng.Range(0.0f, 1.0f), lo = mean - rng.Range(0.0f, 2.0f), hi = mean + rng.Range(0.0f, 2.0f);
    box_ok = box_ok && rd::BoxLo(mean, sg, lo, hi) == hs::SsrBoxLoChannel(mean, sg, lo, hi) && rd::BoxHi(mean, sg, lo, hi) == hs::SsrBoxHiChannel(mean, sg, lo, hi);
    const rd::Vec3 v{rng.Range(-3.0f, 6.0f), rng.Range(-3.0f, 6.0f), rng.Range(-3.0f, 6.0f)};
    const rd::Vec3 bl{rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f)};
    const rd::Vec3 bh{bl.x + rng.Range(0.1f, 2.0f), bl.y + rng.Range(0.1f, 2.0f), bl.z + rng.Range(0.1f, 2.0f)};
    const rd::Vec3 mine = rd::ClipToBox(v, bl, bh);
    const hs::float3 theirs = hs::SsrClipToBox(hs::float3(v.x, v.y, v.z), hs::float3(bl.x, bl.y, bl.z), hs::float3(bh.x, bh.y, bh.z));
    box_ok = box_ok && Near(mine.x, theirs.x, 1e-5) && Near(mine.y, theirs.y, 1e-5) && Near(mine.z, theirs.z, 1e-5);
    inside = inside && mine.x >= bl.x - 1e-4f && mine.x <= bh.x + 1e-4f && mine.y >= bl.y - 1e-4f && mine.y <= bh.y + 1e-4f && mine.z >= bl.z - 1e-4f && mine.z <= bh.z + 1e-4f;
    // a value inside the box is left alone
    const rd::Vec3 in_box{(bl.x + bh.x) * 0.5f, (bl.y + bh.y) * 0.5f, (bl.z + bh.z) * 0.5f};
    const rd::Vec3 same = rd::ClipToBox(in_box, bl, bh);
    inside = inside && same.x == in_box.x && same.y == in_box.y && same.z == in_box.z;
  }
  Check(box_ok, "the variance clip's box and the point it brings the history to are the reflections' own (SsrBoxLo/HiChannel, SsrClipToBox)");
  Check(inside, "the clipped history is inside the box, and a value that was inside is not moved");
}

// The virtual motion: the geometry of a mirror and a moving camera.
void TestVirtualMotion() {
  // the reprojection point is the reflections' own virtual image for a mirror
  Rng rng;
  bool same_as_ssr = true;
  for (int i = 0; i < 2000; ++i) {
    const hs::float3 receiver(rng.Range(-20.0f, 20.0f), rng.Range(-10.0f, 10.0f), rng.Range(1.0f, 80.0f));
    const float hit = rng.Range(0.1f, 500.0f);
    const rd::Vec3 mine = rd::ReprojectionPoint({receiver.x, receiver.y, receiver.z}, hit, 0.0f);
    const hs::float3 theirs = hs::SsrVirtualPoint(receiver, hit);
    same_as_ssr = same_as_ssr && Near(mine.x, theirs.x, 1e-3) && Near(mine.y, theirs.y, 1e-3) && Near(mine.z, theirs.z, 1e-3);
  }
  Check(same_as_ssr, "for a mirror the reprojection point is the reflections' virtual image (SsrVirtualPoint)");
  // the share by roughness: whole under 0.3, none over 0.6, between linear; none without a hit
  Check(rd::VirtualShare(0.0f, 10.0f) == 1.0f && rd::VirtualShare(0.3f, 10.0f) == 1.0f && rd::VirtualShare(0.6f, 10.0f) == 0.0f && rd::VirtualShare(1.0f, 10.0f) == 0.0f && Near(rd::VirtualShare(0.45f, 10.0f), 0.5, 1e-6) &&
            rd::VirtualShare(0.0f, 0.0f) == 0.0f && rd::VirtualShare(0.0f, -1.0f) == 0.0f,
        "the virtual image's share: whole for a mirror, none for a rough surface, linear between 0.3 and 0.6, none without a hit");
  // the geometry: the camera moves one metre to the right; a mirror 5 m ahead shows an object whose image stands 10 m behind the mirror's surface (15 m from the eye). In the last frame the image stood
  // at (1, 0, 15) in the old view space; the surface point at (1, 0, 5). The pixel that looks at (0, 0, 5) now must read the history where the IMAGE was, not where the wall was.
  const float tan_x = 0.5f;
  const auto u_of = [&](const rd::Vec3& p) { return 0.5f + 0.5f * p.x / (tan_x * p.z); };
  const rd::Vec3 receiver{0.0f, 0.0f, 5.0f};
  const rd::Vec3 motion{1.0f, 0.0f, 0.0f};   // this frame's view space into the last frame's: a translation by the camera's step
  const auto moved = [&](rd::Vec3 p) { return rd::Vec3{p.x + motion.x, p.y + motion.y, p.z + motion.z}; };
  const rd::Vec3 image_prev{1.0f, 0.0f, 15.0f};   // where the reflected object's image was in the last frame
  const float u_true = u_of(image_prev);
  const float u_mirror = u_of(moved(rd::ReprojectionPoint(receiver, 10.0f, 0.0f)));
  const float u_surface = u_of(moved(receiver));
  const float u_rough = u_of(moved(rd::ReprojectionPoint(receiver, 10.0f, 0.9f)));
  Check(Near(u_mirror, u_true, 1e-6), Num("a mirror: the history is read at the image's own place (u %.5f against %.5f)", u_mirror, u_true));
  Check(std::fabs(u_surface - u_true) > 0.05f, Num("the surface's own motion would read the wrong place (u %.4f against %.4f: a wall that slides with the road)", u_surface, u_true));
  Check(Near(u_rough, u_surface, 1e-6), "a rough surface follows the surface");
  // between: the point is on the segment from the surface to the image, in the share the roughness says
  const rd::Vec3 half = rd::ReprojectionPoint(receiver, 10.0f, 0.45f);
  Check(Near(half.x, 0.0, 1e-6) && Near(half.z, 5.0 + 0.5 * 10.0, 1e-4), "between the two roughnesses the point is half way along the view ray to the image");
}

// ---------------------------------------------------------------------------
// the CPU model of the two stages on synthetic frames
// ---------------------------------------------------------------------------

struct Px {
  bool ok = true;
  float c[3] = {1.0f, 1.0f, 1.0f};
  float z = 10.0f;
  rd::Vec3 pos;
  rd::Vec3 n{0.0f, 0.0f, -1.0f};
  float rough = 0.6f;
  float hit = 20.0f;
  float share = 1.0f;
  float length = 1.0f;
};
struct Frame {
  int w = 0, h = 0;
  std::vector<Px> p;
  Px& at(int x, int y) { return p[size_t(y) * size_t(w) + size_t(x)]; }
  const Px& at(int x, int y) const { return p[size_t(y) * size_t(w) + size_t(x)]; }
};
constexpr float kTan = 0.4f;
rd::Vec3 PosOf(int x, int y, int w, int h, float z) { return {((float(x) + 0.5f) / float(w) * 2.0f - 1.0f) * kTan * z, (1.0f - (float(y) + 0.5f) / float(h) * 2.0f) * kTan * z, z}; }
float PixelAngle(int h) { return 2.0f * kTan / float(h); }
Frame Flat(int w, int h, float mean, float noise, Rng& rng) {
  Frame f;
  f.w = w;
  f.h = h;
  f.p.resize(size_t(w) * size_t(h));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Px& q = f.at(x, y);
      q.pos = PosOf(x, y, w, h, q.z);
      const float v = mean + noise * rng.Gauss();
      q.c[0] = q.c[1] = q.c[2] = v;
    }
  }
  return f;
}
bool ColourIsNumber(const Px& q) { return rd::IsNumber(q.c[0]) && rd::IsNumber(q.c[1]) && rd::IsNumber(q.c[2]); }
// hlsl RtDnGuidesOk: ok, and every guide a number
bool GuidesOk(const Px& q) {
  return q.ok && rd::IsNumber(q.z) && rd::IsNumber(q.pos.x) && rd::IsNumber(q.pos.y) && rd::IsNumber(q.pos.z) && rd::IsNumber(q.n.x) && rd::IsNumber(q.n.y) && rd::IsNumber(q.n.z) && rd::IsNumber(q.rough) &&
         rd::IsNumber(q.hit) && rd::IsNumber(q.share) && rd::IsNumber(q.length);
}
// One iteration of the spatial pass: hlsl/rt_denoise_engine.hlsli RtDnSpatialMain, pixel by pixel, from the twins.
Frame Iterate(const Frame& in, uint32_t iteration, rd::Profile profile) {
  Frame out = in;
  const uint32_t step = 1u << iteration;
  for (int y = 0; y < in.h; ++y) {
    for (int x = 0; x < in.w; ++x) {
      Px c = in.at(x, y);
      Px& out_px = out.at(x, y);
      // a colour that is not a number is black (RtDnSane), in the pass-through as well
      if (!rd::IsNumber(c.c[0]) || !rd::IsNumber(c.c[1]) || !rd::IsNumber(c.c[2])) c.c[0] = c.c[1] = c.c[2] = 0.0f;
      out_px.c[0] = c.c[0];
      out_px.c[1] = c.c[1];
      out_px.c[2] = c.c[2];
      const float amount = rd::Mix(c.share);
      if (!GuidesOk(c) || amount <= 0.0f) continue;
      const float alpha = c.rough * c.rough;
      const float radius = rd::Radius(profile, alpha, c.hit, c.z, PixelAngle(in.h), c.length);
      if (!rd::StepTaken(radius, step)) continue;
      float sum = 0.0f, sum2 = 0.0f, count = 0.0f;
      for (int vy = -1; vy <= 1; ++vy) {
        for (int vx = -1; vx <= 1; ++vx) {
          const int qx = std::clamp(x + vx * int(step), 0, in.w - 1), qy = std::clamp(y + vy * int(step), 0, in.h - 1);
          const Px& t = in.at(qx, qy);
          if (t.ok && ColourIsNumber(t)) {
            const float l = rd::Luma(t.c[0], t.c[1], t.c[2]);
            sum += l;
            sum2 += l * l;
            count += 1.0f;
          }
        }
      }
      const float variance = rd::Variance(sum, sum2, count);
      const float luma_c = rd::Luma(c.c[0], c.c[1], c.c[2]);
      float total[3] = {0, 0, 0};
      float weights = 0.0f;
      for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
          const int qx = x + dx * int(step), qy = y + dy * int(step);
          if (qx < 0 || qy < 0 || qx >= in.w || qy >= in.h) continue;
          float w = rd::Kernel1(dx) * rd::Kernel1(dy);
          const Px* t = &c;
          if (dx != 0 || dy != 0) {
            t = &in.at(qx, qy);
            if (!t->ok) continue;
            if (!GuidesOk(*t) || !ColourIsNumber(*t)) continue;  // the tap is left out
            const rd::Vec3 d{t->pos.x - c.pos.x, t->pos.y - c.pos.y, t->pos.z - c.pos.z};
            const float plane = std::fabs(c.n.x * d.x + c.n.y * d.y + c.n.z * d.z);
            const float ndot = c.n.x * t->n.x + c.n.y * t->n.y + c.n.z * t->n.z;
            w *= rd::DepthWeight(plane, c.z, rd::DepthSigma(profile)) * rd::NormalWeight(ndot, rd::NormalPower(profile)) * rd::RoughWeight(c.rough, t->rough, rd::RoughSigma(profile)) *
                 rd::LumaWeight(luma_c, rd::Luma(t->c[0], t->c[1], t->c[2]), variance, rd::LumaSigma(profile)) * rd::HitWeight(c.hit, t->hit, rd::HitSigma(profile));
          }
          for (int k = 0; k < 3; ++k) total[k] += t->c[k] * w;
          weights += w;
        }
      }
      Px& o = out_px;
      for (int k = 0; k < 3; ++k) {
        const float filtered = weights > 1.0e-6f ? total[k] / weights : c.c[k];
        o.c[k] = rd::Lerp(c.c[k], filtered, amount);
      }
    }
  }
  return out;
}
Frame Run(const Frame& in, uint32_t iterations, rd::Profile profile = rd::Profile::kSpecular) {
  Frame f = in;
  for (uint32_t i = 0; i < iterations; ++i) f = Iterate(f, i, profile);
  return f;
}
double VarianceOf(const Frame& f, int border) {
  double sum = 0.0, sum2 = 0.0;
  int n = 0;
  for (int y = border; y < f.h - border; ++y) {
    for (int x = border; x < f.w - border; ++x) {
      const double v = f.at(x, y).c[0];
      sum += v;
      sum2 += v * v;
      ++n;
    }
  }
  const double mean = sum / n;
  return sum2 / n - mean * mean;
}
double MeanOf(const Frame& f, int border) {
  double sum = 0.0;
  int n = 0;
  for (int y = border; y < f.h - border; ++y) {
    for (int x = border; x < f.w - border; ++x) {
      sum += f.at(x, y).c[0];
      ++n;
    }
  }
  return sum / n;
}

void TestSpatialModel() {
  Rng rng;
  // noise falls, the mean stays
  const Frame noisy = Flat(64, 64, 1.0f, 0.3f, rng);
  const double v0 = VarianceOf(noisy, 10);
  const Frame one = Run(noisy, 1), two = Run(noisy, 2), three = Run(noisy, 3);
  const double v1 = VarianceOf(one, 10), v2 = VarianceOf(two, 10), v3 = VarianceOf(three, 10);
  Check(v1 < v0 * 0.5 && v2 < v1 * 0.5 && v3 < v2 * 0.5, Num("noise falls with every iteration (variance %.4f, then %.4f, %.4f, %.4f)", v0, v1, v2, v3));
  Check(v3 < v0 * 0.06, Num("three iterations take the variance of white noise down by 16 or more (%.1fx)", v0 / std::max(v3, 1e-12)));
  Check(Near(MeanOf(three, 10), MeanOf(noisy, 10), 0.02), Num("the mean is kept (%.4f against %.4f)", MeanOf(three, 10), MeanOf(noisy, 10)));
  // a flat image stays flat, for both profiles
  Frame flat = Flat(40, 40, 2.5f, 0.0f, rng);
  double worst = 0.0;
  for (const rd::Profile profile : {rd::Profile::kSpecular, rd::Profile::kDiffuse}) {
    const Frame f = Run(flat, 3, profile);
    for (const Px& q : f.p) worst = std::max(worst, double(std::fabs(q.c[0] - 2.5f)));
  }
  Check(worst < 1e-5, Num("a flat image stays flat through three iterations of both profiles (largest change %.2e)", worst));
  // the output of a pixel is a weighted mean of its inputs: inside their range
  {
    Frame f = Flat(32, 32, 1.0f, 0.5f, rng);
    const Frame out = Run(f, 3);
    float lo = 1e9f, hi = -1e9f;
    for (const Px& q : f.p) {
      lo = std::min(lo, q.c[0]);
      hi = std::max(hi, q.c[0]);
    }
    bool in_range = true;
    for (const Px& q : out.p) in_range = in_range && q.c[0] >= lo - 1e-4f && q.c[0] <= hi + 1e-4f;
    Check(in_range, "every filtered pixel is a mean of the frame's own values: nothing is made up (inside the input's range)");
  }
  // edges: the left half and the right half differ in one guide; the pixels beside the edge keep their side's value
  const auto edge_test = [&](const std::string& what, const auto& make_right, double tolerance) {
    Frame f = Flat(48, 48, 1.0f, 0.02f, rng);
    for (int y = 0; y < f.h; ++y) {
      for (int x = f.w / 2; x < f.w; ++x) {
        Px& q = f.at(x, y);
        const float v = 3.0f + 0.02f * rng.Gauss();
        q.c[0] = q.c[1] = q.c[2] = v;
        make_right(q, x, y, f.w, f.h);
      }
    }
    const Frame out = Run(f, 3);
    double left_err = 0.0, right_err = 0.0;
    for (int y = 8; y < f.h - 8; ++y) {
      for (int x = f.w / 2 - 6; x < f.w / 2; ++x) left_err = std::max(left_err, double(std::fabs(out.at(x, y).c[0] - 1.0f)));
      for (int x = f.w / 2; x < f.w / 2 + 6; ++x) right_err = std::max(right_err, double(std::fabs(out.at(x, y).c[0] - 3.0f)));
    }
    Check(left_err < tolerance && right_err < tolerance, "an edge of " + what + Num(" is not crossed (the pixels beside it moved %.3f and %.3f)", left_err, right_err));
  };
  edge_test("depth (a plane 4 m behind)", [](Px& q, int x, int y, int w, int h) {
    q.z = 14.0f;
    q.pos = PosOf(x, y, w, h, q.z);
  }, 0.12);
  edge_test("the normal (53 degrees)", [](Px& q, int, int, int, int) { q.n = {0.8f, 0.0f, -0.6f}; }, 0.12);
  edge_test("the roughness (0.6 and 0.9)", [](Px& q, int, int, int, int) { q.rough = 0.9f; }, 0.2);
  edge_test("the hit distance (20 m and 2000 m)", [](Px& q, int, int, int, int) { q.hit = 2000.0f; }, 0.2);
  // the diffuse profile does not stop at the hit distance (it has none)
  {
    Frame f = Flat(48, 48, 1.0f, 0.0f, rng);
    for (int y = 0; y < f.h; ++y) {
      for (int x = f.w / 2; x < f.w; ++x) {
        f.at(x, y).c[0] = f.at(x, y).c[1] = f.at(x, y).c[2] = 3.0f;
        f.at(x, y).hit = 2000.0f;
      }
    }
    const Frame spec = Run(f, 3, rd::Profile::kSpecular);
    const Frame diff = Run(f, 3, rd::Profile::kDiffuse);
    const double s = std::fabs(spec.at(f.w / 2 - 1, 24).c[0] - 1.0f);
    const double d = std::fabs(diff.at(f.w / 2 - 1, 24).c[0] - 1.0f);
    Check(s < 0.2 && d > s, Num("the hit distance is an edge for a reflection (%.3f) and not for the diffuse profile (%.3f)", s, d));
  }
  // a near mirror is not touched
  {
    Frame f = Flat(40, 40, 1.0f, 0.3f, rng);
    for (Px& q : f.p) {
      q.rough = 0.05f;
      q.hit = 5.0f;
    }
    const Frame out = Run(f, 3);
    bool same = true;
    for (size_t i = 0; i < f.p.size(); ++i) same = same && out.p[i].c[0] == f.p[i].c[0];
    Check(same, "a near mirror (radius under 0.75) is copied through all three iterations: not a pixel changed");
  }
  // the share: 0 leaves a pixel alone, a half is half way
  {
    Frame f = Flat(40, 40, 1.0f, 0.3f, rng);
    Frame none = f, half = f;
    for (Px& q : none.p) q.share = 0.0f;
    for (Px& q : half.p) q.share = 0.5f;
    const Frame full = Run(f, 1), out_none = Run(none, 3), out_half = Run(half, 1);
    bool a = true, b = true;
    for (size_t i = 0; i < f.p.size(); ++i) {
      a = a && out_none.p[i].c[0] == f.p[i].c[0];
      b = b && Near(out_half.p[i].c[0], f.p[i].c[0] + 0.5f * (full.p[i].c[0] - f.p[i].c[0]), 1e-5);
    }
    Check(a, "a pixel whose share is 0 is copied: the picture's own reflections are not filtered");
    Check(b, "a share of a half takes half the way to the filtered value");
    Frame under = f;
    for (Px& q : under.p) q.share = 0.04f;
    const Frame out_under = Run(under, 3);
    bool c = true;
    for (size_t i = 0; i < f.p.size(); ++i) c = c && out_under.p[i].c[0] == f.p[i].c[0];
    Check(c, "a share under 0.05 is copied as well");
  }
  // a pixel that is no signal leaks nothing and is not changed
  {
    Frame a = Flat(33, 33, 1.0f, 0.3f, rng);
    Frame b = a;
    a.at(16, 16).ok = false;
    a.at(16, 16).c[0] = a.at(16, 16).c[1] = a.at(16, 16).c[2] = 1000.0f;
    b.at(16, 16).ok = false;
    b.at(16, 16).c[0] = b.at(16, 16).c[1] = b.at(16, 16).c[2] = -5.0f;
    const Frame oa = Run(a, 3), ob = Run(b, 3);
    bool same = true;
    for (size_t i = 0; i < a.p.size(); ++i) {
      if (i == size_t(16) * 33 + 16) continue;
      same = same && oa.p[i].c[0] == ob.p[i].c[0];
    }
    Check(same && oa.at(16, 16).c[0] == 1000.0f && ob.at(16, 16).c[0] == -5.0f, "a pixel that is no signal is passed through and its value is never read by its neighbours");
  }
  // a pixel whose colour is not a number (NaN, either sign, or infinity) is not spread: its neighbours come out as if it were no signal, and the result is a number everywhere
  {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    bool neighbours_same = true, all_numbers = true, centre_black = true;
    for (const float bad : {nan, -nan, inf, -inf}) {
      Frame a = Flat(33, 33, 1.0f, 0.3f, rng);
      Frame b = a;
      for (int k = 0; k < 3; ++k) a.at(16, 16).c[k] = bad;
      b.at(16, 16).ok = false;
      a.at(10, 12).c[1] = bad;  // one channel only
      b.at(10, 12).ok = false;
      const Frame oa = Run(a, 3), ob = Run(b, 3);
      for (size_t i = 0; i < a.p.size(); ++i) {
        for (int k = 0; k < 3; ++k) all_numbers = all_numbers && rd::IsNumber(oa.p[i].c[k]);
      }
      // the first iteration only: the second reads the first's result, where the bad pixel (a signal) has been filtered into a number and the other frame's (not a signal) has not
      const Frame first_a = Run(a, 1), first_b = Run(b, 1);
      for (size_t i = 0; i < a.p.size(); ++i) {
        if (i == size_t(16) * 33 + 16 || i == size_t(12) * 33 + 10) continue;
        neighbours_same = neighbours_same && first_a.p[i].c[0] == first_b.p[i].c[0] && first_a.p[i].c[1] == first_b.p[i].c[1] && first_a.p[i].c[2] == first_b.p[i].c[2];
      }
      // the pixel itself is filtered from its neighbours when it is signal: black in, a mean of the others out (not NaN, and not left black by a bad weight)
      centre_black = centre_black && rd::IsNumber(oa.at(16, 16).c[0]) && oa.at(16, 16).c[0] >= 0.0f;
    }
    Check(neighbours_same, "a pixel whose colour is NaN or infinite leaks nothing: every other pixel is the same as with that pixel not a signal (the tap is left out, the variance does not count it)");
    Check(all_numbers && centre_black, "every pixel of the result is a number, the bad pixel's own value included (black where it is not filtered, a mean of its neighbours where it is)");
    // a guide that is not a number: the pixel is no signal (passed through) and never a tap
    bool guides_same = true;
    for (int which = 0; which < 6; ++which) {
      Frame a = Flat(33, 33, 1.0f, 0.3f, rng);
      Px& q = a.at(16, 16);
      q.c[0] = q.c[1] = q.c[2] = 1000.0f;  // a colour that would show in a neighbour's value if the tap were used
      switch (which) {
        case 0: q.z = nan; break;
        case 1: q.pos.x = inf; break;
        case 2: q.n.y = nan; break;
        case 3: q.rough = nan; break;
        case 4: q.hit = inf; break;
        default: q.share = nan; break;
      }
      const Frame oa = Run(a, 3);
      for (size_t i = 0; i < a.p.size(); ++i) {
        for (int k = 0; k < 3; ++k) guides_same = guides_same && rd::IsNumber(oa.p[i].c[k]);
        if (i == size_t(16) * 33 + 16) continue;
        guides_same = guides_same && oa.p[i].c[0] < 3.0f;  // the 1000 never reaches a neighbour (the frame's own values are 1 +- 0.9)
      }
      guides_same = guides_same && oa.at(16, 16).c[0] == 1000.0f;  // passed through as it was (a number)
    }
    Check(guides_same, "a pixel whose guide (depth, position, normal, roughness, hit distance or share) is not a number is no signal: passed through, never a tap, every pixel of the result a number");
  }
}

// One pixel of the temporal pass over a static scene: hlsl RtDnTemporalMain with the identity reprojection. `samples` are the 3 x 3 of the current frame (index 4 the pixel's own).
struct Temporal {
  float history = 0.0f;
  float length = 0.0f;
  bool has = false;
};
float TemporalStep(Temporal& t, const float samples[9]) {
  // a colour that is not a number is black (RtDnSane), a history that is not one is no history (RtDnTemporalMain)
  const float current = rd::IsNumber(samples[4]) ? samples[4] : 0.0f;
  float colour = current;
  const bool used = t.has && rd::IsNumber(t.history) && rd::IsNumber(t.length) && t.length > 0.0f;
  if (used) {
    float lo = current, hi = current, sum = 0.0f, sum2 = 0.0f, count = 0.0f;
    for (int i = 0; i < 9; ++i) {
      if (!rd::IsNumber(samples[i])) continue;  // a neighbour that is not a number is left out of the box
      lo = std::min(lo, samples[i]);
      hi = std::max(hi, samples[i]);
      sum += samples[i];
      sum2 += samples[i] * samples[i];
      count += 1.0f;
    }
    const float n = std::max(count, 1.0f);
    const float mean = sum / n;
    const float sigma = std::sqrt(std::max(sum2 / n - mean * mean, 0.0f));
    const float box_lo = rd::BoxLo(mean, sigma, lo, hi), box_hi = rd::BoxHi(mean, sigma, lo, hi);
    const rd::Vec3 clipped = rd::ClipToBox({t.history, t.history, t.history}, {box_lo, box_lo, box_lo}, {box_hi, box_hi, box_hi});
    colour = rd::Lerp(current, clipped.x, rd::HistoryWeight(t.length));
  }
  t.length = rd::NextLength(t.length, used);
  t.history = colour;
  t.has = true;
  return colour;
}

void TestTemporalModel() {
  Rng rng;
  // a static scene: the variance falls with the frames (the running mean), over many pixels
  const int pixels = 3000;
  std::vector<Temporal> state(static_cast<size_t>(pixels));
  std::vector<float> out(static_cast<size_t>(pixels));
  std::vector<double> var_by_frame;
  for (int frame = 1; frame <= 24; ++frame) {
    for (int i = 0; i < pixels; ++i) {
      float samples[9];
      for (float& s : samples) s = 1.0f + 0.3f * rng.Gauss();
      out[size_t(i)] = TemporalStep(state[size_t(i)], samples);
    }
    double sum = 0.0, sum2 = 0.0;
    for (const float v : out) {
      sum += v;
      sum2 += double(v) * v;
    }
    const double mean = sum / pixels;
    var_by_frame.push_back(sum2 / pixels - mean * mean);
    if (frame == 24) Check(Near(mean, 1.0, 0.02), Num("a static scene: the mean stays (%.4f)", mean));
  }
  Check(var_by_frame[0] > 0.07 && var_by_frame[7] < var_by_frame[0] * 0.3 && var_by_frame[23] < var_by_frame[0] * 0.12,
        Num("a static scene: the variance falls with the frames (%.4f, %.4f after 8, %.4f after 24)", var_by_frame[0], var_by_frame[7], var_by_frame[23]));
  // the scene changes at frame 13 (1.0 -> 3.0): the variance clip pulls the history to the new neighbourhood at once; the plain running mean would still be near 1.15
  std::vector<Temporal> st(static_cast<size_t>(pixels));
  double after_jump = 0.0, plain = 0.0;
  for (int frame = 1; frame <= 14; ++frame) {
    const float truth = frame <= 12 ? 1.0f : 3.0f;
    double sum = 0.0;
    for (int i = 0; i < pixels; ++i) {
      float samples[9];
      for (float& s : samples) s = truth + 0.3f * rng.Gauss();
      sum += TemporalStep(st[size_t(i)], samples);
    }
    if (frame == 13) {
      after_jump = sum / pixels;
      plain = (12.0 * 1.0 + 3.0) / 13.0;
    }
  }
  Check(after_jump > 2.3 && plain < 1.3, Num("the scene changes: the first frame after reads %.2f (the clip), a plain running mean would read %.2f", after_jump, plain));
  // a reset (no history) takes the current frame whole
  Temporal fresh;
  const float s9[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  Check(TemporalStep(fresh, s9) == 5.0f && fresh.length == 1.0f, "no history: the pixel takes the current frame whole and its length is 1");
  // A NaN or an infinity in the signal or in the history never stays: the result is a number every frame, and a poisoned history starts again from the next frame's signal
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  bool finite_always = true, restarts = true;
  for (const float bad : {nan, -nan, inf, -inf}) {
    Temporal run;
    for (int frame = 0; frame < 40; ++frame) {
      float samples[9];
      for (float& v : samples) v = 2.0f + 0.1f * rng.Gauss();
      if (frame == 10) samples[4] = bad;               // the pixel itself, once
      if (frame == 20) samples[(rng.Next() % 8 + 5) % 9] = bad;  // a neighbour (never the centre: index 4 is the pixel)
      if (frame >= 30 && frame < 32) samples[4] = bad;  // two frames running
      const float out_value = TemporalStep(run, samples);
      finite_always = finite_always && rd::IsNumber(out_value) && rd::IsNumber(run.history) && rd::IsNumber(run.length);
    }
    // the history itself poisoned (a NaN texel the bilinear read touched): the next result is the current frame, the length restarts at 1
    Temporal poisoned;
    poisoned.has = true;
    poisoned.history = bad;
    poisoned.length = 12.0f;
    const float s_now[9] = {2, 2, 2, 2, 2.5f, 2, 2, 2, 2};
    const float after = TemporalStep(poisoned, s_now);
    restarts = restarts && after == 2.5f && poisoned.length == 1.0f && rd::IsNumber(poisoned.history);
    Temporal poisoned_length;
    poisoned_length.has = true;
    poisoned_length.history = 2.0f;
    poisoned_length.length = bad;
    restarts = restarts && TemporalStep(poisoned_length, s_now) == 2.5f && poisoned_length.length == 1.0f;
  }
  Check(finite_always, "temporal: a NaN or an infinity in the pixel, in a neighbour (the variance box) or two frames running never reaches the result, the history or its length");
  Check(restarts, "temporal: a history (colour or length) that is not a number is no history: the pixel starts again from the current frame with a length of 1; the current frame's own NaN is black");
}

// ---------------------------------------------------------------------------
// the screen-space reflections' use
// ---------------------------------------------------------------------------

void TestDecision() {
  using sd::Why;
  Check(sd::IterationsFor(ssr::Level::kOff, -1) == 0 && sd::IterationsFor(ssr::Level::kHigh, -1) == 2 && sd::IterationsFor(ssr::Level::kUltra, -1) == 3, "auto: high takes two iterations, ultra three, an effect that is off none");
  Check(sd::IterationsFor(ssr::Level::kHigh, 0) == 0 && sd::IterationsFor(ssr::Level::kUltra, 0) == 0 && sd::IterationsFor(ssr::Level::kHigh, 1) == 1 && sd::IterationsFor(ssr::Level::kHigh, 3) == 3 &&
            sd::IterationsFor(ssr::Level::kHigh, 9) == 3 && sd::IterationsFor(ssr::Level::kOff, 3) == 0 && sd::IterationsFor(ssr::Level::kUltra, -7) == 3,
        "the setting: 0 is off, 1 to 3 force that many (at most three), the effect being off wins, a number under -1 is auto");
  // the whole table: 3 levels x 9 settings x 2^5 booleans, against a plain model of the order of the reasons
  int rows = 0, mismatches = 0;
  for (int level = 0; level < 3; ++level) {
    for (int setting = -2; setting <= 6; ++setting) {
      for (int bits = 0; bits < 32; ++bits) {
        sd::Facts f;
        f.level = ssr::Level(level);
        f.setting = setting;
        f.raw = (bits & 1) != 0;
        f.fallback_followed = (bits & 2) != 0;
        f.multisampled = (bits & 4) != 0;
        f.pipelines_ready = (bits & 8) != 0;
        f.images_ok = (bits & 16) != 0;
        const sd::Plan plan = sd::Decide(f);
        const uint32_t iterations = sd::IterationsFor(f.level, f.setting);
        Why expect = Why::kRuns;
        if (iterations == 0) expect = Why::kOff;
        else if (f.raw) expect = Why::kRawFrame;
        else if (!f.fallback_followed) expect = Why::kNoFallback;
        else if (f.multisampled) expect = Why::kMultisampled;
        else if (!f.pipelines_ready) expect = Why::kPipelines;
        else if (!f.images_ok) expect = Why::kImages;
        ++rows;
        if (plan.why != expect || plan.iterations != (expect == Why::kRuns ? iterations : 0u) || plan.runs() != (expect == Why::kRuns)) ++mismatches;
      }
    }
  }
  Check(mismatches == 0, "the decision's order (off, raw frame, no fallback, multisampled, pipelines, images) over " + std::to_string(rows) + " combinations (" + std::to_string(mismatches) + " differ)");
  // the cases the design names
  sd::Facts ok;
  ok.level = ssr::Level::kHigh;
  ok.fallback_followed = ok.pipelines_ready = ok.images_ok = true;
  Check(sd::Decide(ok).runs() && sd::Decide(ok).iterations == 2, "high, the fallback followed, everything made: two iterations");
  ok.raw = true;
  Check(sd::Decide(ok).why == Why::kRawFrame && !sd::Decide(ok).runs(), "a raw frame (Ray Reconstruction takes it): RtDenoise is skipped, whatever else is ready");
  ok.raw = false;
  ok.fallback_followed = false;
  Check(sd::Decide(ok).why == Why::kNoFallback, "a frame the fallback did not follow: nothing to filter (the identity of a card that does not trace)");
  Check(sd::Decide(sd::Facts{}).why == Why::kOff && sd::Decide(sd::Facts{}).iterations == 0, "the default facts (the effect off): off");
  // the words
  for (int w = 0; w < int(Why::kCount); ++w) Check(std::string(sd::WhyName(Why(w))).size() > 3 && std::string(sd::WhyName(Why(w))) != "?", "every reason has a name (" + std::to_string(w) + ")");
  Check(sd::HistoryLength(true) == 12.0f && sd::HistoryLength(false) == 1.0f && sd::HistoryLength(true) >= rd::kMatureLength, "the accumulation's length: a mature signal with a history, a fresh one without");
  // the census
  sd::Totals t;
  Check(sd::FormatTotals(t).empty(), "the census line is empty when the stage was never wanted");
  t.wanted_once = true;
  t.frames_run = 90;
  t.iterations = 180;
  t.frames_raw = 7;
  t.frames_other = 3;
  t.last_iterations = 2;
  t.last_why = Why::kRuns;
  const std::string line = sd::FormatTotals(t);
  Check(Has(line, "90 frames") && Has(line, "180 iterations") && Has(line, "7 left to Ray Reconstruction") && Has(line, "3 could not run") && Has(line, "it runs") && line[0] == ';', "the census line says the frames, the iterations, the raw frames and the others: " + line);
}

// ---------------------------------------------------------------------------
// the push blocks' words against the .hlsli
// ---------------------------------------------------------------------------

void TestWords(const std::string& dir) {
  std::string ssr_common, consumer, plain, engine;
  if (!Slurp(dir + "/hlsl/ssr_common.hlsli", &ssr_common) || !Slurp(dir + "/hlsl/ssr_denoise_consumer.hlsli", &consumer) || !Slurp(dir + "/hlsl/rt_denoise_plain_consumer.hlsli", &plain) ||
      !Slurp(dir + "/hlsl/rt_denoise_engine.hlsli", &engine)) {
    return;
  }
  Check(DefineOf(ssr_common, "SSR_W_SRC") == long(sd::kWordSrc) && DefineOf(ssr_common, "SSR_W_COL01") == long(sd::kWordConf) && DefineOf(ssr_common, "SSR_W_COL23") == long(sd::kWordIteration) &&
            DefineOf(ssr_common, "SSR_W_THICK") == long(sd::kWordLength),
        "the reflections' words: the signal's and the trace's slots, the confidence image, the iteration and the history length are the SSR block's words the host sets");
  Check(Has(consumer, "#define SSRDN_W_CONF SSR_W_COL01") && Has(consumer, "#define SSRDN_W_ITERATION SSR_W_COL23") && Has(consumer, "#define SSRDN_W_LENGTH SSR_W_THICK") && Has(consumer, "SsrW(SSR_W_SRC)"),
        "the consumer names the same words");
  Check(Has(consumer, "SSR_W_TAN_X") && Has(consumer, "SSR_W_TAN_Y") && Has(consumer, "SSR_W_WET") && !Has(consumer, "SSR_W_MOTION") && !Has(consumer, "SSR_W_FRAME"), "the consumer reads the lens and the wetness and not the temporal pass's words");
  const rd::Patch p = sd::PatchFor();
  Check(p.src_word == sd::kWordSrc && p.iteration_word == sd::kWordIteration && p.history_word == rd::kNoWord && p.flags_word == rd::kNoWord, "the screen-space reflections' patch: the signal's slot and the iteration; no history (the accumulation is the temporal stage)");
  // the plain consumer
  struct Pair {
    const char* name;
    uint32_t value;
  };
  const Pair words[] = {{"RTDNP_W_SIGNAL", rd::plain::kSignal}, {"RTDNP_W_GUIDES", rd::plain::kGuides}, {"RTDNP_W_HISTORY", rd::plain::kHistory}, {"RTDNP_W_SIZE", rd::plain::kSize}, {"RTDNP_W_PARAMS", rd::plain::kParams},
                        {"RTDNP_W_TAN_X", rd::plain::kTanX}, {"RTDNP_W_TAN_Y", rd::plain::kTanY}, {"RTDNP_W_PREV_TAN_X", rd::plain::kPrevTanX}, {"RTDNP_W_PREV_TAN_Y", rd::plain::kPrevTanY},
                        {"RTDNP_W_MOTION", rd::plain::kMotion}, {"RTDNP_W_LENGTH", rd::plain::kLength}, {"RTDNP_W_FRAME", rd::plain::kFrame}};
  bool plain_ok = true;
  for (const Pair& w : words) plain_ok = plain_ok && DefineOf(plain, w.name) == long(w.value);
  Check(plain_ok, "the plain consumer's words are the twin's (signal, guides, history, size, params, lens, the last frame's lens, motion, length)");
  Check(rd::plain::kMotion + 12 <= rd::plain::kLength && rd::plain::kLength < rd::kPushWords && rd::plain::kFrame == rd::plain::kLength + 1 && rd::plain::kFrame < rd::kPushWords,
        "the motion's 12 floats fit before the length and everything fits in 32 words (the frame's number, RT_DN_VIS only, is the word after the length)");
  const rd::Patch pp = rd::plain::PatchFor();
  Check(pp.src_word == rd::plain::kSignal && pp.iteration_word == rd::plain::kParams && pp.history_word == rd::plain::kHistory && pp.flags_word == rd::plain::kParams && pp.has_history_bit == rd::plain::kHasHistoryBit &&
            Has(plain, "((RtDnW(RTDNP_W_PARAMS) >> 8u) & 1u)") && rd::plain::kHasHistoryBit == 8,
        "the plain consumer's patch: the signal's slot and the iteration in the first and fourth words, the history's slot, the has-history bit 8 of the parameters");
  Check(rd::kPushWords == 32 && Has(plain, "uint4 r[8]") && Has(consumer, "ssr_pass.hlsli"), "both consumers' blocks are 32 words (8 x uint4)");
  Check(rd::kTextureSets == 4 && rd::kSamplerSet == 4 && rd::kOutputSet == 5 && rd::kPushSpace == 4 && Has(engine, "register(u0, space5)") && Has(plain, "register(b0, space4)") && rd::kThreads == 8,
        "the layout: four texture sets, the samplers at 4, the output at 5 (u0, space5), the push block at (b0, space4), 8 x 8 threads");
  for (const char* hook : {"int2 RtDnSize()", "uint RtDnIteration()", "float RtDnPixelAngle()", "float4 RtDnSignal(int2 p)", "float4 RtDnColourAt(int2 p)", "RtDnPixel RtDnLoad(int2 pix)"}) {
    Check(Has(consumer, hook) && Has(plain, hook), std::string("both consumers define ") + hook);
  }
  for (const char* hook : {"bool RtDnHasHistory()", "RtDnPrevious RtDnToPrevious(float3 p)", "float RtDnHistoryDepthAt(int2 p)", "float4 RtDnHistoryAt(float2 uv)"}) {
    Check(Has(plain, hook), std::string("the plain consumer defines the temporal hook ") + hook);
  }
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

void TestSources(const std::string& dir) {
  std::string engine, spatial, plain_spatial, plain_temporal, host_h, host_cpp, ssr_cpp, ssr_h, cmake, shaders_h, precache, settings, manifest, meta, common, order_h;
  const bool read = Slurp(dir + "/hlsl/rt_denoise_engine.hlsli", &engine) && Slurp(dir + "/hlsl/ssr_denoise_spatial_cs.hlsl", &spatial) && Slurp(dir + "/hlsl/rt_denoise_plain_spatial_cs.hlsl", &plain_spatial) &&
                    Slurp(dir + "/hlsl/rt_denoise_plain_temporal_cs.hlsl", &plain_temporal) && Slurp(dir + "/rt_denoise.h", &host_h) && Slurp(dir + "/rt_denoise.cpp", &host_cpp) &&
                    Slurp(dir + "/ssr.cpp", &ssr_cpp) && Slurp(dir + "/ssr.h", &ssr_h) && Slurp(dir + "/CMakeLists.txt", &cmake) && Slurp(dir + "/host_shaders.h", &shaders_h) &&
                    Slurp(dir + "/host_precache.h", &precache) && Slurp(dir + "/../settings/generated/settings_gpu.h", &settings) && Slurp(dir + "/../settings/generated/manifest.json", &manifest) &&
                    Slurp(dir + "/../settings/generated/settings_meta.inc", &meta) && Slurp(dir + "/hlsl/rt_denoise_common.hlsli", &common) && Slurp(dir + "/frame_order.h", &order_h);
  if (!read) {
    std::printf("(sources not read: %s)\n", dir.c_str());
    return;
  }
  // the shaders
  Check(Has(spatial, "#define RT_DN_PASS_SPATIAL 1") && Has(spatial, "#include \"ssr_denoise_consumer.hlsli\"") && Has(spatial, "RtDnSpatialMain(id)") && Has(spatial, "[numthreads(8, 8, 1)]"),
        "the reflections' spatial shader: the spatial body over the reflections' consumer, 8 x 8");
  Check(Has(plain_spatial, "#define RT_DN_PASS_SPATIAL 1") && Has(plain_spatial, "rt_denoise_plain_consumer.hlsli") && Has(plain_temporal, "#define RT_DN_PASS_TEMPORAL 1") && Has(plain_temporal, "RtDnTemporalMain(id)"),
        "the plain shaders: one spatial, one temporal, over the plain consumer");
  Check(Has(engine, "#if defined(RT_DN_PASS_SPATIAL)") && Has(engine, "#elif defined(RT_DN_PASS_TEMPORAL)") && Has(engine, "[[vk::image_format(\"rgba16f\")]]") && Has(engine, "RWTexture2D<float4> g_dn_out : register(u0, space5);"),
        "the engine: both bodies behind their pass's define, one RGBA16F storage image at (u0, space5) with the SPIR-V format named");
  const std::string sp = FunctionText(engine, "void RtDnSpatialMain(uint3 id) {");
  Check(!sp.empty() && Before(sp, "if (!RtDnGuidesOk(c) || amount <= 0.0f) {", "const float radius") && Before(sp, "if (!RtDnStepTaken(radius, step)) {", "float sum = 0.0f;") && Count(sp, "g_dn_out[p] = own;") == 2 &&
            Has(sp, "if (!t.ok) {\n          continue;") && Has(sp, "g_dn_out[p] = float4(lerp(own.xyz, filtered, amount), own.w);"),
        "the spatial body: the pass-through for a pixel that is no signal or has no share comes first, then the step's test; a tap that is no signal is skipped; the alpha is kept");
  Check(Has(sp, "RtDnDepthWeight(plane") && Has(sp, "RtDnNormalWeight(dot(c.n, t.n)") && Has(sp, "RtDnRoughWeight(c.rough, t.rough") && Has(sp, "RtDnLumaWeight(luma_centre") && Has(sp, "RtDnHitWeight(c.hit, t.hit"),
        "the spatial body weighs every tap by depth, normal, roughness, luminance and hit distance");
  Check(Has(sp, "RtDnRadius(RT_DN_PROFILE, alpha, c.hit, c.z, RtDnPixelAngle(), c.length)") && Has(sp, "const float alpha = c.rough * c.rough;"), "the radius follows the reflection: the lobe's alpha (roughness squared), the hit, the depth, the pixel's size and the history");
  // numbers only: a signal, a guide or a history that is a NaN (or infinite) is dealt with where it is read, by the bits (Vulkan leaves max / min of a NaN open)
  Check(Has(common, "bool RtDnIsNumber(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }") && Has(common, "float3 RtDnSane(float3 c) { return RtDnIsNumber3(c) ? c : float3(0.0f, 0.0f, 0.0f); }") &&
            Has(common, "bool RtDnGuidesOk(RtDnPixel p) {") && !Has(common, "isnan(") && !Has(engine, "isnan("),
        "the library decides a number by the bits (RtDnIsNumber), a colour that is not one is black (RtDnSane) and a pixel's guides are checked together (RtDnGuidesOk)");
  Check(Has(sp, "const float4 own = float4(RtDnSane(raw_signal.xyz), raw_signal.w);") && Has(sp, "c.colour = RtDnSane(c.colour);") && Has(sp, "if (t.w > 0.0f && RtDnIsNumber3(t.xyz)) {") &&
            Has(sp, "if (!RtDnGuidesOk(t) || !RtDnIsNumber3(t.colour)) {\n          continue;") && Before(sp, "if (!t.ok) {\n          continue;", "if (!RtDnGuidesOk(t) || !RtDnIsNumber3(t.colour)) {"),
        "the spatial body: the signal's own colour and the centre's are black where not a number (in the pass-through too), the variance leaves such a pixel out, a tap with guides or a colour that are not numbers is left out");
  const std::string tp = FunctionText(engine, "void RtDnTemporalMain(uint3 id) {");
  Check(!tp.empty() && Has(tp, "const float4 current = float4(RtDnSane(raw_signal.xyz), raw_signal.w);") && Has(tp, "if (!RtDnGuidesOk(c)) {") && Has(tp, "if (RtDnIsNumber3(h.xyz) && RtDnIsNumber(h.w)) {\n          history = h.xyz;\n          length = h.w;\n          used = length > 0.0f;") &&
            Has(tp, "if (t.w > 0.0f && RtDnIsNumber3(t.xyz)) {\n          lo = min(lo, t.xyz);"),
        "the temporal body: the current colour is black where not a number, a pixel with guides that are not numbers writes nothing, a history (colour or length) that is not a number is no history, the variance box leaves such a neighbour out");
  Check(!tp.empty() && Has(tp, "RtDnToPrevious(RtDnReprojectionPoint(c.pos, c.hit, c.rough))") && Has(tp, "RtDnToPrevious(c.pos)") && Has(tp, "RtDnSameSurface(stored, plain.z)") && Has(tp, "RtDnClipToBox(history, box_lo, box_hi)") &&
            Has(tp, "RtDnHistoryWeight(length)") && Has(tp, "RtDnNextLength(length, used)"),
        "the temporal body: the history at the reflection's virtual point, accepted where the plain reprojection is the same surface, clipped to the variance box, blended by its length, the new length in the alpha");
  Check(!Has(engine, "NonUniformResourceIndex") && !Has(common, "NonUniformResourceIndex"), "no non uniform resource index (the slots are values of the push block)");
  // the build
  Check(Has(cmake, "larecomp_gpu_host_shader(ssr_denoise_spatial_cs cs_6_0 DEFINES SSR_MS=0)") && Has(cmake, "larecomp_gpu_host_shader(rt_denoise_plain_spatial_cs cs_6_0)") &&
            Has(cmake, "larecomp_gpu_host_shader(rt_denoise_plain_temporal_cs cs_6_0)") && Has(cmake, "    rt_denoise.cpp\n") && Has(cmake, "add_executable(larecomp_rt_denoise_test EXCLUDE_FROM_ALL rt_denoise_test.cpp)"),
        "the build lists the three shaders, the host's source and this test");
  Check(Count(shaders_h, "ssr_denoise_spatial_cs.hlsl.dxil.h") == 1 && Count(shaders_h, "ssr_denoise_spatial_cs.hlsl.spirv.h") == 1 && Count(shaders_h, "rt_denoise_plain_temporal_cs.hlsl.dxil.h") == 1 &&
            Count(shaders_h, "rt_denoise_plain_spatial_cs.hlsl.spirv.h") == 1 && Has(shaders_h, "SsrDenoiseSpatialShader()") && Has(shaders_h, "RtDenoisePlainShader(bool temporal)"),
        "host_shaders.h has the blobs and the two accessors");
  Check(Has(precache, "kRtDenoise = 32") && Has(host_cpp, "HostPrecache::Key(HostPrecache::kRtDenoise, (instance_ << 4) | stage)"), "the host's pipelines have their own precache owner, the consumer's number and the stage");
  // the host class
  Check(Has(host_cpp, "output_builder_.addReadWriteTexture(0);") && Has(host_cpp, "device_->CreateDescriptorSet(output_builder_, 1, 0, \"rt denoise output set\")") &&
            Has(host_cpp, "output_v2_builder_.addReadWriteTexture(0);") && Has(host_cpp, "output_v2_builder_.addReadWriteTexture(1);") &&
            Has(host_cpp, "device_->CreateDescriptorSet(output_v2_builder_, 2, 0, \"rt denoise v2 temporal outputs\")"),
        "the colour output has one UAV and the V2 temporal layout has a second UAV for features");
  Check(Has(host_cpp, "STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS") && Has(host_cpp, "plume::RenderFormat::R16G16B16A16_FLOAT") && !Has(host_cpp, "RENDER_TARGET"), "the result images are RGBA16F storage images (no render target, no typed load is used)");
  Check(Has(host_cpp, "const uint32_t iterations = std::clamp<uint32_t>(in.iterations, 1, rt_denoise::kMaxIterations);") && Has(host_cpp, "const uint32_t out = (i & 1u) == 0 ? kSpatialA : kSpatialB;") &&
            Has(host_cpp, "Dispatch(in.list, kSpatial, out, push, in.width, in.height, i + 1 == iterations);"),
        "the spatial stage records one to three iterations, ping-ponging between two images, the last one's result in the final read state");
  Check(Has(host_cpp, "final_pass ? Stage::GRAPHICS_AND_COMPUTE : Stage::COMPUTE") && Has(host_cpp, "Layout::GENERAL") && Has(host_cpp, "list->setComputePushConstants(0, push.data(), 0, sizeof(uint32_t) * rt_denoise::kPushWords);"),
        "each dispatch: the result image to the storage layout at the compute stage, the 32 words, then to the read state (both stages after the last)");
  Check(Has(host_cpp, "history_valid_ = false;   // a frame without a stage breaks the history") &&
            Has(host_cpp, "if (!v2_ran_) {\n    v2_history_valid_ = false;") &&
            Has(host_cpp, "if (image.width != width || image.height != height) {\n    RetireSet(slot);") &&
            Has(host_cpp, "ResetHistory();"),
        "a gap or resize invalidates history, and replacement images get fresh descriptors");
  Check(Has(host_h, "const HostImage* RecordSpatial(const Frame& frame);") && Has(host_h, "const HostImage* RecordTemporal(const Frame& frame);") && Has(host_h, "void ResetHistory()"), "the host's API: RecordSpatial, RecordTemporal, ResetHistory");
  // the screen-space reflections' use
  const std::string trigger = FunctionText(ssr_cpp, "bool ScreenReflections::OnTrigger(");
  const size_t legacy_temporal = trigger.find("pass_timing_->Mark(list, \"ssr temporal\");");
  const size_t legacy_denoise = trigger.rfind("pass_timing_->Mark(list, \"ssr denoise\");");
  Check(!trigger.empty() && Before(trigger, "pass_timing_->Mark(list, \"ssr denoise\");", "filtered_v2 = denoise_.RecordV2(stage);") &&
            Before(trigger, "filtered_v2 = denoise_.RecordV2(stage);", "pass_timing_->Mark(list, \"ssr temporal\");") &&
            legacy_temporal != std::string::npos && legacy_denoise != std::string::npos && legacy_temporal < legacy_denoise &&
            legacy_denoise < trigger.find("pass_timing_->Mark(list, \"ssr apply\");") && Count(ssr_cpp, "\"ssr denoise\"") == 2,
        "both denoiser paths have timing markers, with legacy accumulation preceding legacy spatial filtering");
  Check(Has(trigger, "denoise_facts.raw = raw;") && Has(trigger, "denoise_facts.fallback_followed = rt_recorded;") && Has(trigger, "rt_recorded = rt_.Record(rt_frame);") && Has(trigger, "denoise_facts.multisampled = ms;") && Has(trigger, "ssr_denoise::Plan denoise = ssr_denoise::Decide(denoise_facts);") &&
            Has(trigger, "if (denoise.runs() && !filtered_v2) {\n    pass_timing_->Mark(list, \"ssr denoise\");") && Has(trigger, "denoise_.RecordSpatial(stage)") &&
            Has(trigger, "if (!raw && !regen_ok && rt_recorded && denoise_setting != 0 && RtDenoise::V2Enabled())"),
        "both denoisers respect the vendor bypass, and the legacy spatial pass is used as a fallback");
  Check(Has(trigger, "denoise_facts.pipelines_ready = denoise_.SpatialReady();") && Before(trigger, "denoise_facts.pipelines_ready = true;", "denoise_facts.pipelines_ready = denoise_.SpatialReady();"),
        "the pipeline is asked for only when everything else says go: with the effect off, the fallback off or the setting 0 nothing is listed with the precache");
  Check(Has(trigger, "apply.Set(pk::kSrc, ssr::PackSlots(resolve_source->slot, trace_.slot));") &&
            Has(trigger, "const HostImage* resolve_source = filtered_v2 ? filtered_v2 : &accum_[write_];") &&
            Has(trigger, "resolve_source = filtered;") && Has(trigger, "filtered_v2 ? denoise_.V2Confidence() : nullptr"),
        "the resolve reads the filtered image where there is one, the accumulation otherwise");
  Check(Has(trigger, "last_key_ = now;") && !Has(trigger, "last_key_ = denoise") && Before(trigger, "write_ ^= 1u;", "return true;"), "the accumulation's history is the next frame's as before: the filter's image is not fed back");
  Check(Has(trigger, "stage.signal = &accum_[write_];") && Has(trigger, "stage.reads = {&trace_, &confidence_};") && Has(trigger, "pass.SetFloat(ssr_denoise::kWordLength, ssr_denoise::HistoryLength(use_history));") &&
            Has(trigger, "pass.Set(ssr_denoise::kWordConf, confidence_.slot);"),
        "the stage's inputs: the accumulated image as the signal, the trace and the confidence image read, the history's length from the accumulation's own use of one");
  Check(Has(ssr_cpp, "denoise_.Initialize(device, frames, heap, passes, /*instance=*/0, \"ssr\", RtDenoise::Blob{spatial.data, spatial.size, spatial.name}, RtDenoise::Blob{});") &&
            Before(ssr_cpp, "if (rt_.available()) {\n    const HostShaderBlob spatial", "denoise_.Initialize(") && Has(ssr_cpp, "denoise_.Shutdown();") && Has(ssr_cpp, "denoise_.OnPresent();") && Has(ssr_cpp, "denoise_.HeldBytes()"),
        "the host is made only where the fallback can run, shut down with the effect, ticked at the present and counted in its memory");
  // the merge of the raw mode and the fallback: one frame can be both
  Check(Has(trigger, "if (!garage && rt_.wanted() && !decision.stub) {") && !Has(trigger, "!decision.stub && !raw") && Before(trigger, "ssr::RawWhy raw_why = ssr::DecideRaw(raw_facts);", "if (!garage && rt_.wanted() && !decision.stub) {"),
        "R6 merge: a raw frame follows the ray traced fallback too (its one ray a pixel is what Ray Reconstruction takes): the fallback's condition does not look at the raw decision");
  Check(Has(trigger, "bool images_ok = EnsureImages(width, height, rt, raw, sf.render[0], sf.render[1]);") && Has(trigger, "images_ok = EnsureImages(width, height, false, raw, sf.render[0], sf.render[1]);") &&
            Has(trigger, "images_ok = EnsureImages(width, height, false, false);") && Before(trigger, "    rt = false;     // the confidence image or its framebuffer could not be made", "    raw_why = ssr::RawWhy::kImages;"),
        "R6 merge: the images are made for both modes at once, and when they cannot be the fallback goes first, then the raw mode");
  Check(Has(ssr_cpp, "if (raw) {\n    ok = ok && passes_->EnsureImage(hit_, render_width, render_height, kHitFormat, \"ssr hit distance\");\n  } else {\n    ok = ok && EnsureHistory(width, height);") &&
            Has(ssr_cpp, "passes_->EnsureImage(trace_, width, height, kFormat, \"ssr trace\", trace_flags)"),
        "R6 merge: a raw frame has the hit image and no accumulation; the trace image has the storage flags in any frame that follows the fallback, raw or not");
  Check(Has(ssr_cpp, "ScreenReflectionsPixelShader(i)") && Has(ssr_h, "kShaderTraceRt, kShaderTraceRtMs, kShaderApplyRaw, kShaderHit, kShaderCount") && Has(shaders_h, "case 7: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_raw_ps);") &&
            Has(shaders_h, "case 5: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ps);") && Has(shaders_h, "case 6: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_rt_ms_ps);"),
        "R6 merge: the pixel shaders' numbers: the fallback's two traces are 5 and 6, the raw resolve 7 and the hit pass 8");
  Check(Has(ssr_h, "RtDenoise denoise_;") && Has(ssr_h, "ssr_denoise::FormatTotals(denoise_totals_)") && Has(ssr_h, "#include \"rt_denoise.h\""), "ssr.h owns the host and adds the census");
  // the setting
  Check(Has(settings, "REX_SETTING(i32, " "mcla_gpu_rt_denoise, -1,") && Has(settings, "rex::kDebugOnly, rex::Range{-1, 3});") && Has(settings, "inline auto& mcla_gpu_rt_denoise = g_gpu.mcla_gpu_rt_denoise;") &&
            Has(manifest, "\"mcla_gpu_rt_denoise\": {") && Has(meta, "{\"mcla_gpu_rt_denoise\", \"MCLA/GPU\"},"),
        "mcla_gpu_rt_denoise is a live debug-only setting, -1 (auto) to 3, in the three generated files");
  Check(Has(ssr_cpp, "larecomp::settings::mcla_gpu_rt_denoise.Get()"), "the effect reads it every frame (live)");
  Check(Has(order_h, "\"ssr denoise\"") && Has(order_h, "std::array<const char*, 4> more_labels{};"), "the frame order table lists the label");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = ".";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      dir = argv[i];
    }
  }
  TestNumbers();
  TestFunctionsAgainstHlsl();
  TestRadius();
  TestWeights();
  TestTemporalRules();
  TestVirtualMotion();
  TestSpatialModel();
  TestTemporalModel();
  TestDecision();
  TestWords(dir);
  TestSources(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
