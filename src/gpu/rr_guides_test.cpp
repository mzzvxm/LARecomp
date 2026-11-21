// Check of DLSS Ray Reconstruction's guide pass (rr_guides_rules.h, hlsl/rr_guides_common.hlsli, hlsl/rr_guides_cs.hlsl, hlsl/rr_guides_view_cs.hlsl, rr_guides.h / rr_guides.cpp, perf/DLSS-RR.md, package
// RR-2). A program of its own: it needs no renderer, no ReXGlue, no Streamline and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_rr_guides_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_rr_guides_test [<src/gpu dir>]     (the folder defaults to the one this file is in)
//
// It checks:
//   THE IMAGES   the five guides' formats are the ones dlss_rr_rules.h takes (rr::PlanTags over exactly these images is ok: the packed normals and roughness, the albedos, the hit distance in place of
//                a specular motion, the responsivity), their bytes a pixel and at 1080p and 4K, and that a wrong size or format of any is refused.
//   THE DECODE   the .hlsli compiled AS C++ (through the shim of the HLSL types below) against the twin in rr_guides_rules.h on random pixels (every output of every function), and against the
//                definitions: the normal is RT1's (a round trip through the octahedral code), the roughness is one minus the smoothness byte, the diffuse albedo is the albedo squared with a floor and 0.5
//                where RT2 holds none, F0 follows the class table for all 32 class numbers, an emissive surface reflects nothing, the sky is the guide's defaults, a surface with no data at all faces the
//                camera with roughness 1.
//   THE SPECULAR ALBEDO   NVIDIA's EnvBRDFApprox2 as the guide's text writes it (the matrices as matrices) against the multiplied-out form of the shader and the twin, and against a Monte Carlo
//                integral of the GGX lobe with Schlick's Fresnel and the height-correlated Smith term (the fit's own source): within the fit's accuracy over the whole range of N.V and roughness.
//   THE DEPTH     the linearisation of the stored depth against the projection of the game's form (near, far); the view direction through a pixel for a rotated camera; the normal of the depth's tangents on
//                tilted planes (exact), at a step in depth (it stays on the nearer surface's side), at the edge of the picture and next to the sky (one side), and none with the sky on both.
//   THE REST     hit distance (the sky's 65504, 0 on a surface, a writer's positive distance wins, NaN and infinity do not), responsivity, the debug views (colours of every view).
//   THE RULES    PassWanted: wanted exactly when Ray Reconstruction would run if its guides were there, or a view is on while an upscaler runs; the views and their numbers (the HLSL's defines, the
//                setting's range); the push blocks' sizes and offsets against the HLSL's structs; the layout's spaces, the formats the shaders declare for the five images, the pipelines' registration
//                (the CMake lines, the blobs, the precache owner's number).
//   THE SOURCES  the pass asks the G-buffer for the planes and uses the frame gate; the Scene's frame is not touched (the call sites are held by frame_order_test.cpp).
//
// Exit 0 = passed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dlss_rr_rules.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "rr_guides_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: the shim of what the files use (the same as the reflections' and the G-buffer's tests: a namespace of its own so that the standard library's abs, sqrt ... are not in the way)
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
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float exp2(float v) { return std::exp2(v); }
inline float log2(float v) { return std::log2(v); }
#include "hlsl/gbuffer_common.hlsli"
#include "hlsl/rr_guides_common.hlsli"
}  // namespace hlsl_shim

namespace rg = mcla::gpu::rr_guides;
namespace gb = mcla::gpu::gbuffer;
namespace rr = mcla::gpu::rr;
namespace hs = hlsl_shim;

namespace {

using uint = unsigned int;
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* format, double a = 0, double b = 0, double c = 0) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), format, a, b, c);
  return buffer;
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  out->erase(std::remove(out->begin(), out->end(), '\r'), out->end());
  return true;
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }

struct Rng {
  uint32_t state = 2463534242u;
  uint32_t Next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  float F() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
  float Range(float lo, float hi) { return lo + (hi - lo) * F(); }
};

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
bool Near3(hs::float3 a, rg::Vec3 b, float tol) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }
hs::float3 ToShim(rg::Vec3 v) { return hs::float3(v.x, v.y, v.z); }

gb::Vec3 UnitRandom(Rng& rng) {
  for (;;) {
    gb::Vec3 v = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float l2 = gb::Dot(v, v);
    if (l2 > 0.01f && l2 <= 1.0f) return gb::Normalize(v);
  }
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------
void ImagesChecks() {
  // the formats are the ones the rules take, each under the buffer type the rules tag it as
  for (size_t i = 0; i < rg::kGuideCount; ++i) {
    const rg::GuideSpec spec = rg::SpecOf(rg::Guide(i));
    Check(spec.buffer != rr::Buffer::kCount && rr::FormatOk(spec.buffer, spec.format), std::string(spec.name) + ": its format is one the rules take for its buffer type");
    Check(rr::Channels(spec.format) != 0 && spec.bytes_per_pixel == (spec.format == rr::Format::kRgba16Float ? 8u : 2u), std::string(spec.name) + ": its bytes a pixel are its format's");
    Check(std::string(spec.name) == rr::InfoOf(spec.buffer).name, std::string(spec.name) + ": it is named as the rules name its buffer");
  }
  Check(rg::BytesPerPixel() == 28, "five images, 28 bytes a pixel (three RGBA16F and two R16F)");
  Check(rg::BytesOf(1920, 1080) == 58060800ull && rg::BytesOf(3840, 2160) == 232243200ull && rg::BytesOf(1280, 720) == 25804800ull, "58 MB at 1080p, 232 MB at 4K (DLAA), 26 MB at 720p");
  // the rules' tag plan over exactly these images: ok, the guides tagged as the rules say, the hit distance in place of a specular motion
  rr::Inputs in;
  in.render_size[0] = 1920;
  in.render_size[1] = 1080;
  in.output_size[0] = 3840;
  in.output_size[1] = 2160;
  in.color = {true, 1920, 1080, rr::Format::kRgba16Float};
  in.depth = {true, 1920, 1080, rr::Format::kR32Float};
  in.motion = {true, 1920, 1080, rr::Format::kRg16Float};
  in.output = {true, 3840, 2160, rr::Format::kRgba16Float};
  in.diffuse_albedo = rg::PlanImage(rg::Guide::kDiffuseAlbedo, 1920, 1080);
  in.specular_albedo = rg::PlanImage(rg::Guide::kSpecularAlbedo, 1920, 1080);
  in.normal_roughness = rg::PlanImage(rg::Guide::kNormalRoughness, 1920, 1080);
  in.specular_hit_distance = rg::PlanImage(rg::Guide::kHitDistance, 1920, 1080);
  in.responsivity = rg::PlanImage(rg::Guide::kResponsivity, 1920, 1080);
  in.have_view_matrices = true;
  rr::TagPlan plan = rr::PlanTags(in);
  Check(plan.verdict == rr::Verdict::kOk && plan.normal_roughness == rr::NormalRoughnessMode::kPacked && plan.specular == rr::SpecularSource::kHitDistance && plan.needs_view_matrices,
        "the tag plan over the five guides is ok: packed normals and roughness, the hit distance as the reflection's motion (with the camera matrices)");
  bool has_resp = false, has_hit = false, has_diffuse = false, has_spec = false, has_nr = false;
  for (uint32_t i = 0; i < plan.count; ++i) {
    has_resp = has_resp || plan.tags[i].buffer == rr::Buffer::kResponsivity;
    has_hit = has_hit || plan.tags[i].buffer == rr::Buffer::kSpecularHitDistance;
    has_diffuse = has_diffuse || plan.tags[i].buffer == rr::Buffer::kDiffuseAlbedo;
    has_spec = has_spec || plan.tags[i].buffer == rr::Buffer::kSpecularAlbedo;
    has_nr = has_nr || plan.tags[i].buffer == rr::Buffer::kNormalRoughness;
  }
  Check(has_resp && has_hit && has_diffuse && has_spec && has_nr, "all five guides are tagged");
  // each guide is needed or accepted as it is, and a wrong size or an unknown format is refused for any of them
  const rg::Guide required[] = {rg::Guide::kDiffuseAlbedo, rg::Guide::kSpecularAlbedo, rg::Guide::kNormalRoughness};
  for (rg::Guide g : required) {
    rr::Inputs bad = in;
    rr::Image* slot = g == rg::Guide::kDiffuseAlbedo ? &bad.diffuse_albedo : (g == rg::Guide::kSpecularAlbedo ? &bad.specular_albedo : &bad.normal_roughness);
    slot->width -= 1;
    Check(rr::PlanTags(bad).verdict == rr::Verdict::kBadSize, std::string(rg::SpecOf(g).name) + ": one pixel too narrow is refused");
    *slot = rg::PlanImage(g, 1920, 1080);
    slot->format = rr::Format::kRgba8Srgb;
    Check(rr::PlanTags(bad).verdict == rr::Verdict::kBadFormat, std::string(rg::SpecOf(g).name) + ": an sRGB format is refused");
  }
  {
    rr::Inputs bad = in;
    bad.specular_hit_distance.format = rr::Format::kR8Unorm;
    Check(rr::PlanTags(bad).verdict == rr::Verdict::kBadFormat, "the hit distance in 8 bits is refused");
    bad = in;
    bad.responsivity.format = rr::Format::kR8Unorm;
    Check(rr::PlanTags(bad).verdict == rr::Verdict::kBadFormat, "the responsivity as unsigned 8 bits is refused (signed or half float only)");
    bad = in;
    bad.have_view_matrices = false;
    Check(rr::PlanTags(bad).verdict == rr::Verdict::kNeedsViewMatrices, "the hit distance needs the camera matrices (RR-3 puts them in the options)");
  }
  // the group size and the pass's dispatch
  Check(rg::Groups(1) == 1 && rg::Groups(8) == 1 && rg::Groups(9) == 2 && rg::Groups(1920) == 240 && rg::Groups(1080) == 135 && rg::Groups(2160) == 270, "the dispatch covers the image in 8 x 8 groups");
}

// ---------------------------------------------------------------------------
// NVIDIA's specular albedo as the guide writes it
// ---------------------------------------------------------------------------
struct M2 {
  double m[2][2];
};
struct M3 {
  double m[3][3];
};
// EnvBRDFApprox2 with the matrices as matrices (the guide's text, one float after the other: mul(M, v) is the matrix times a column vector).
double GuideBias(double nov, double alpha) {
  const double X[4] = {1.0, nov, nov * nov, nov * nov * nov};
  const double Y[4] = {1.0, alpha, alpha * alpha, alpha * alpha * alpha};
  const M2 M1 = {{{0.99044, -1.28514}, {1.29678, -0.755907}}};
  const M3 M2m = {{{1.0, 2.92338, 59.4188}, {20.3225, -27.0302, 222.592}, {121.563, 626.13, 316.627}}};
  const double xy[2] = {X[0], X[1]};
  const double xyw[3] = {X[0], X[1], X[3]};
  const double yxy[2] = {Y[0], Y[1]};
  const double yxyw[3] = {Y[0], Y[1], Y[3]};
  double a[2], b[3];
  for (int r = 0; r < 2; ++r) a[r] = M1.m[r][0] * xy[0] + M1.m[r][1] * xy[1];
  for (int r = 0; r < 3; ++r) b[r] = M2m.m[r][0] * xyw[0] + M2m.m[r][1] * xyw[1] + M2m.m[r][2] * xyw[2];
  return (a[0] * yxy[0] + a[1] * yxy[1]) / (b[0] * yxyw[0] + b[1] * yxyw[1] + b[2] * yxyw[2]);
}
double GuideScale(double nov, double alpha) {
  const double X[4] = {1.0, nov, nov * nov, nov * nov * nov};
  const double Y[4] = {1.0, alpha, alpha * alpha, alpha * alpha * alpha};
  const M2 M3m = {{{0.0365463, 3.32707}, {9.0632, -9.04756}}};
  const M3 M4 = {{{1.0, 3.59685, -1.36772}, {9.04401, -16.3174, 9.22949}, {5.56589, 19.7886, -20.2123}}};
  const double xy[2] = {X[0], X[1]};
  const double xzw[3] = {X[0], X[2], X[3]};
  const double yxy[2] = {Y[0], Y[1]};
  const double yxyw[3] = {Y[0], Y[1], Y[3]};
  double a[2], b[3];
  for (int r = 0; r < 2; ++r) a[r] = M3m.m[r][0] * xy[0] + M3m.m[r][1] * xy[1];
  for (int r = 0; r < 3; ++r) b[r] = M4.m[r][0] * xzw[0] + M4.m[r][1] * xzw[1] + M4.m[r][2] * xzw[2];
  return (a[0] * yxy[0] + a[1] * yxy[1]) / (b[0] * yxyw[0] + b[1] * yxyw[1] + b[2] * yxyw[2]);
}
double GuideSpecular(double f0, double alpha, double nov) {
  nov = std::fabs(nov);
  double bias = GuideBias(nov, alpha);
  const double scale = GuideScale(nov, alpha);
  bias *= std::min(std::max(f0 * 50.0, 0.0), 1.0);
  return f0 * std::max(0.0, scale) + std::max(0.0, bias);
}

// The directional albedo of a GGX lobe with Schlick's Fresnel and the height-correlated Smith visibility (the importance sampled split sum of Karis 2013, which the fit approximates).
double IntegratedSpecular(double f0, double rough, double nov) {
  const double alpha = rough * rough;
  const double a2 = alpha * alpha;
  const double V[3] = {std::sqrt(std::max(0.0, 1.0 - nov * nov)), 0.0, nov};
  const int N = 16384;
  double sum_a = 0.0, sum_b = 0.0;
  for (int i = 0; i < N; ++i) {
    // Hammersley point
    double u = double(i) / double(N);
    uint32_t bits = uint32_t(i);
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    const double v = double(bits) * 2.3283064365386963e-10;
    const double phi = 2.0 * 3.14159265358979323846 * u;
    const double cos_t = std::sqrt((1.0 - v) / (1.0 + (a2 - 1.0) * v));
    const double sin_t = std::sqrt(std::max(0.0, 1.0 - cos_t * cos_t));
    const double H[3] = {sin_t * std::cos(phi), sin_t * std::sin(phi), cos_t};
    const double vo_h = V[0] * H[0] + V[1] * H[1] + V[2] * H[2];
    const double L[3] = {2.0 * vo_h * H[0] - V[0], 2.0 * vo_h * H[1] - V[1], 2.0 * vo_h * H[2] - V[2]};
    const double no_l = L[2];
    const double no_h = H[2];
    if (no_l <= 0.0 || vo_h <= 0.0 || no_h <= 0.0) continue;
    const double vis = 0.5 / (no_l * std::sqrt(nov * nov * (1.0 - a2) + a2) + nov * std::sqrt(no_l * no_l * (1.0 - a2) + a2));
    const double g_vis = vis * no_l * (4.0 * vo_h / no_h);
    const double fc = std::pow(1.0 - vo_h, 5.0);
    sum_a += (1.0 - fc) * g_vis;
    sum_b += fc * g_vis;
  }
  return f0 * sum_a / N + sum_b / N;
}

void SpecularAlbedoChecks() {
  // the multiplied-out form (shader and twin) is the guide's text, on a grid of N.V, alpha and F0
  double worst_form = 0.0;
  for (int i = 0; i <= 20; ++i) {
    for (int j = 0; j <= 20; ++j) {
      const double nov = std::max(0.0, double(i) / 20.0);
      const double alpha = double(j) / 20.0;
      const double bias_guide = GuideBias(nov, alpha);
      const double scale_guide = GuideScale(nov, alpha);
      worst_form = std::max(worst_form, std::fabs(bias_guide - double(hs::RrEnvBias(float(nov), float(alpha)))));
      worst_form = std::max(worst_form, std::fabs(scale_guide - double(hs::RrEnvScale(float(nov), float(alpha)))));
      worst_form = std::max(worst_form, std::fabs(double(rg::EnvBias(float(nov), float(alpha))) - double(hs::RrEnvBias(float(nov), float(alpha)))));
      worst_form = std::max(worst_form, std::fabs(double(rg::EnvScale(float(nov), float(alpha))) - double(hs::RrEnvScale(float(nov), float(alpha)))));
    }
  }
  Check(worst_form < 2.0e-3, Num("the multiplied-out bias and scale are the guide's matrices (worst difference %.2e)", worst_form));
  double worst_full = 0.0;
  for (int i = 1; i <= 10; ++i) {
    for (int j = 0; j <= 10; ++j) {
      for (double f0 : {0.0, 0.02, 0.04, 0.6, 1.0}) {
        const double nov = double(i) / 10.0;
        const double alpha = double(j) / 10.0;
        const hs::float3 shader = hs::RrSpecularAlbedo(hs::float3(float(f0), float(f0), float(f0)), float(alpha), float(nov));
        const rg::Vec3 twin = rg::SpecularAlbedo({float(f0), float(f0), float(f0)}, float(alpha), float(nov));
        const double guide = GuideSpecular(f0, alpha, nov);
        worst_full = std::max({worst_full, std::fabs(double(shader.x) - guide), std::fabs(double(twin.x) - guide), std::fabs(double(shader.y) - double(twin.y))});
      }
    }
  }
  Check(worst_full < 2.0e-3, Num("the shader's and the twin's specular albedo are the guide's function (worst difference %.2e)", worst_full));
  // the hack for a surface that does not reflect, the sign of N.V, the floor at 0
  {
    const hs::float3 none = hs::RrSpecularAlbedo(hs::float3(0, 0, 0), 0.3f, 0.7f);
    Check(none.x == 0.0f && none.y == 0.0f && none.z == 0.0f, "F0 = 0 reflects nothing (the bias is taken to 0 with it)");
    const hs::float3 a = hs::RrSpecularAlbedo(hs::float3(0.04f, 0.04f, 0.04f), 0.25f, 0.6f);
    const hs::float3 b = hs::RrSpecularAlbedo(hs::float3(0.04f, 0.04f, 0.04f), 0.25f, -0.6f);
    Check(a.x == b.x && a.y == b.y && a.z == b.z, "N.V is taken as an absolute value (a back face)");
    Check(a.x > 0.0f && a.x < 1.0f, "a dielectric's specular albedo is a small positive number");
    // a colour F0 keeps its channels apart
    const hs::float3 c = hs::RrSpecularAlbedo(hs::float3(1.0f, 0.5f, 0.1f), 0.2f, 0.9f);
    Check(c.x > c.y && c.y > c.z, "a coloured metal's specular albedo keeps the order of its channels");
  }
  // against the integral of the lobe. The fit is NVIDIA's (the guide prescribes it), made to a shadowing term and a Fresnel of its own that are not exactly the ones here, so this is a sanity bound,
  // not the function's definition: it is close for a dielectric (F0 0.04: the Fresnel term B is all there is, a few thousandths) and within about 0.13 for a metal at the rough end, where the scale
  // term is the whole answer and the fit is a few hundredths off (a throwaway probe on 2026-11-21 matched the fit to the correlated Smith term at low roughness and to a Schlick-like one at roughness 1). The
  // grazing end (N.V under 0.15) is where fits of this kind are worst and gets more room.
  double worst_dielectric = 0.0, worst_metal = 0.0, worst_grazing = 0.0, sum_abs = 0.0;
  int n = 0;
  for (int i = 1; i <= 10; ++i) {
    for (double rough : {0.15, 0.3, 0.5, 0.7, 0.9, 1.0}) {
      for (double f0 : {0.04, 0.5, 1.0}) {
        const double nov = double(i) / 10.0;
        const double fit = GuideSpecular(f0, rough * rough, nov);
        const double truth = IntegratedSpecular(f0, rough, nov);
        const double err = std::fabs(fit - truth);
        if (nov < 0.15) {
          worst_grazing = std::max(worst_grazing, err);
        } else if (f0 < 0.1) {
          worst_dielectric = std::max(worst_dielectric, err);
        } else {
          worst_metal = std::max(worst_metal, err);
        }
        sum_abs += err;
        ++n;
      }
    }
  }
  Check(worst_dielectric < 0.03, Num("a dielectric's specular albedo of the fit is within 0.03 of the lobe's integral for N.V from 0.2 (worst %.4f)", worst_dielectric));
  Check(worst_metal < 0.15, Num("a metal's is within 0.15 for N.V from 0.2 (worst %.4f)", worst_metal));
  Check(worst_grazing < 0.25, Num("... and within 0.25 at the grazing end (worst %.4f)", worst_grazing));
  Check(sum_abs / n < 0.03, Num("... and within 0.03 on average (%.4f)", sum_abs / n));
  // a mirror dielectric looking straight on reflects its F0 (0.04): the integral and the fit agree
  Check(std::fabs(GuideSpecular(0.04, 0.0, 1.0) - 0.04) < 0.003, Num("a smooth dielectric seen straight on has a specular albedo of F0 (%.4f)", GuideSpecular(0.04, 0.0, 1.0)));
}

// ---------------------------------------------------------------------------
// the decode against the twin and the definitions
// ---------------------------------------------------------------------------
void DecodeChecks() {
  Rng rng;
  // random pixels: the shader's text against the twin, every output
  int compared = 0, with_depth_normal = 0, valid_normals = 0, no_albedo = 0, emissive = 0, metal = 0;
  for (int trial = 0; trial < 20000; ++trial) {
    float rt1[2] = {0, 0};
    const bool normal_on = rng.F() < 0.7f;
    gb::Vec3 world = UnitRandom(rng);
    if (normal_on) {
      const gb::Rt1Texel t = gb::Rt1Pack(world);
      rt1[0] = gb::Rt1Load(t).x;
      rt1[1] = gb::Rt1Load(t).y;
      ++valid_normals;
    }
    float rt2[4] = {0, 0, 0, 0};
    const float albedo_kind = rng.F();
    if (albedo_kind < 0.15f) {
      ++no_albedo;  // all zero: no albedo
    } else {
      for (int k = 0; k < 4; ++k) rt2[k] = float(rng.Next() & 255u) / 255.0f;
      if (albedo_kind < 0.25f) rt2[0] = rt2[1] = rt2[2] = 0.0f;  // black with a lit weight
      if (albedo_kind > 0.9f) rt2[3] = 0.0f;                     // emissive: an albedo and no lit weight
    }
    const uint32_t cls = rng.Next() % 32u;
    const uint32_t flags = rng.Next() & 7u;
    float rt3[2] = {float(rng.Next() & 255u) / 255.0f, float((cls | (flags << 5u)) & 255u) / 255.0f};
    emissive += cls == uint32_t(gb::SurfaceClass::kEmissive);
    metal += cls == uint32_t(gb::SurfaceClass::kMetal) || cls == uint32_t(gb::SurfaceClass::kChrome);
    gb::Vec3 depth_normal = {0, 0, 0};
    if (rng.F() < 0.5f) {
      depth_normal = UnitRandom(rng);
      ++with_depth_normal;
    }
    const gb::Vec3 dir = UnitRandom(rng);
    const hs::RrPixel a = hs::RrSurfacePixel(hs::float2(rt1[0], rt1[1]), hs::float4(rt2[0], rt2[1], rt2[2], rt2[3]), hs::float2(rt3[0], rt3[1]), ToShim(depth_normal), ToShim(dir));
    const rg::Pixel b = rg::SurfacePixel(rt1, rt2, rt3, depth_normal, dir);
    const float tol = 1.0e-5f;
    const bool same = Near3(a.diffuse, b.diffuse, tol) && Near3(a.normal, b.normal, tol) && Near(a.roughness, b.roughness, tol) && Near3(a.specular, b.specular, tol) && a.hit == b.hit;
    Check(same, "a random surface pixel: the shader's text and the twin agree (trial " + std::to_string(trial) + ")");
    if (!same) break;
    ++compared;
    // the definitions
    if (normal_on) {
      const float dotn = a.normal.x * world.x + a.normal.y * world.y + a.normal.z * world.z;
      if (!(dotn > 0.99999f)) {
        Check(false, Num("a valid RT1 gives its own normal back (dot %.6f)", dotn));
        break;
      }
    }
    if (!(Near(a.roughness, 1.0f - rt3[0], 1.0e-6f) && a.hit == 0.0f)) {
      Check(false, "the roughness is one minus the smoothness, the hit distance 0 on a surface");
      break;
    }
    const float lenn = std::sqrt(a.normal.x * a.normal.x + a.normal.y * a.normal.y + a.normal.z * a.normal.z);
    if (!Near(lenn, 1.0f, 1.0e-4f)) {
      Check(false, "the normal is a unit vector");
      break;
    }
  }
  Check(compared == 20000 && valid_normals > 12000 && with_depth_normal > 8000 && no_albedo > 2000 && emissive > 300 && metal > 1000, "the random pixels cover the cases (normals, depth normals, no albedo, emissive, metals)");
  // the albedo: squared, floored, 0.5 where RT2 holds none (all four values 0), and an albedo of black with a lit weight is the floor
  {
    const float none[4] = {0, 0, 0, 0};
    const rg::Vec3 d0 = rg::DiffuseAlbedo(none);
    Check(d0.x == 0.5f && d0.y == 0.5f && d0.z == 0.5f && hs::RrDiffuseAlbedo(hs::float4(0, 0, 0, 0)).x == 0.5f, "RT2 holds nothing: the diffuse albedo is 0.5");
    const float black[4] = {0, 0, 0, 1.0f};
    const rg::Vec3 d1 = rg::DiffuseAlbedo(black);
    Check(d1.x == rg::kMinAlbedo && d1.y == rg::kMinAlbedo && hs::RrDiffuseAlbedo(hs::float4(0, 0, 0, 1)).z == hs::kRrMinAlbedo, "a black albedo with a lit weight is the floor, never 0");
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    Check(Near(rg::DiffuseAlbedo(grey).x, 0.25f, 1.0e-7f), "the stored sqrt(albedo) is squared back: 0.5 -> 0.25");
    const float emissive_albedo[4] = {0.8f, 0.4f, 0.2f, 0.0f};
    const rg::Vec3 e = rg::DiffuseAlbedo(emissive_albedo);
    Check(Near(e.x, 0.64f, 1.0e-6f) && Near(e.y, 0.16f, 1.0e-6f) && Near(e.z, 0.04f, 1.0e-6f), "an emissive pixel keeps its texture colour as its diffuse albedo (lit weight 0 is not a reason to drop it)");
    // every byte of the albedo has a diffuse albedo at or above the floor and at most 1
    bool range_ok = true;
    for (int byte = 0; byte < 256; ++byte) {
      const float v = float(byte) / 255.0f;
      const float one[4] = {v, v, v, 1.0f};
      const rg::Vec3 d = rg::DiffuseAlbedo(one);
      range_ok = range_ok && d.x >= rg::kMinAlbedo && d.x <= 1.0f && Near(d.x, std::max(v * v, rg::kMinAlbedo), 1.0e-7f);
    }
    Check(range_ok, "every stored albedo byte gives its square, not under the floor, not over 1");
  }
  // F0 by class: all 32 class numbers, with and without an albedo
  {
    const rg::Vec3 albedo = {0.2f, 0.5f, 0.8f};
    bool ok = true;
    for (uint32_t cls = 0; cls < 32; ++cls) {
      const rg::Vec3 with = rg::ClassF0(cls, albedo, true);
      const rg::Vec3 without = rg::ClassF0(cls, albedo, false);
      const hs::float3 shader_with = hs::RrClassF0(cls, hs::float3(0.2f, 0.5f, 0.8f), true);
      const hs::float3 shader_without = hs::RrClassF0(cls, hs::float3(0.2f, 0.5f, 0.8f), false);
      ok = ok && Near3(shader_with, with, 0.0f) && Near3(shader_without, without, 0.0f);
      if (cls == uint32_t(gb::SurfaceClass::kChrome) || cls == uint32_t(gb::SurfaceClass::kMetal)) {
        ok = ok && with.x == albedo.x && with.y == albedo.y && with.z == albedo.z && without.x == rg::kMetalF0 && without.z == rg::kMetalF0;
      } else if (cls == uint32_t(gb::SurfaceClass::kEmissive)) {
        ok = ok && with.x == 0.0f && with.y == 0.0f && without.z == 0.0f;
      } else {
        ok = ok && with.x == 0.04f && with.y == 0.04f && with.z == 0.04f && without.x == 0.04f;
      }
    }
    Check(ok, "F0: a dielectric 0.04, a metal or chrome its albedo (0.6 with none), an emissive surface 0, for all 32 class numbers, shader and twin alike");
    Check(GB_CLASS_EMISSIVE == int(gb::SurfaceClass::kEmissive) && GB_CLASS_METAL == int(gb::SurfaceClass::kMetal) && GB_CLASS_CHROME == int(gb::SurfaceClass::kChrome),
          "the class numbers the shader uses are the G-buffer's");
  }
  // the sky is the guide's defaults
  {
    const hs::RrPixel sky = hs::RrSkyPixel();
    const rg::Pixel twin = rg::SkyPixel();
    Check(sky.diffuse.x == 0.5f && sky.diffuse.y == 0.5f && sky.diffuse.z == 0.5f && sky.normal.x == 0 && sky.normal.y == 0 && sky.normal.z == 0 && sky.roughness == 0 && sky.specular.x == 0 &&
              sky.hit == 65504.0f,
          "the sky: diffuse albedo 0.5, no normal, roughness 0, specular albedo 0, hit distance 65504 (the integration guide's defaults)");
    Check(twin.diffuse.x == rr::kSkyDiffuseAlbedo && twin.roughness == rr::kSkyRoughness && twin.specular.x == rr::kSkySpecularAlbedo && twin.hit == rr::kSkyHitDistance && twin.hit == sky.hit,
          "... and they are dlss_rr_rules.h's constants");
  }
  // a surface the planes know nothing about, and the depth has no normal for: it faces the camera, roughness 1 (RT3 cleared), the dielectric's F0, the albedo's 0.5
  {
    const gb::Vec3 dir = gb::Normalize({0.2f, -0.1f, 1.0f});
    const float z2[2] = {0, 0};
    const float z4[4] = {0, 0, 0, 0};
    const rg::Pixel px = rg::SurfacePixel(z2, z4, z2, {0, 0, 0}, dir);
    Check(Near(px.normal.x, -dir.x, 1.0e-6f) && Near(px.normal.y, -dir.y, 1.0e-6f) && Near(px.normal.z, -dir.z, 1.0e-6f), "no data and no depth normal: the normal faces the camera");
    Check(px.roughness == 1.0f && px.diffuse.x == 0.5f && px.hit == 0.0f, "no data: roughness 1 (RT3 cleared is smoothness 0), diffuse albedo 0.5, hit distance 0");
    Check(px.specular.x > 0.0f && px.specular.x < 0.1f, "no data: a small specular albedo (dielectric, chalk)");
    // the depth's normal is used where RT1 holds none, turned to the camera
    const rg::Pixel away = rg::SurfacePixel(z2, z4, z2, {0, 0, 1}, dir);
    Check(away.normal.z < 0.0f && Near(std::sqrt(away.normal.x * away.normal.x + away.normal.y * away.normal.y + away.normal.z * away.normal.z), 1.0f, 1.0e-6f),
          "the depth's normal that points away from the camera is turned to face it");
    // RT1 wins over the depth's normal
    const gb::Rt1Texel t = gb::Rt1Pack({0, 1, 0});
    const float rt1[2] = {gb::Rt1Load(t).x, gb::Rt1Load(t).y};
    const rg::Pixel own = rg::SurfacePixel(rt1, z4, z2, {1, 0, 0}, dir);
    Check(own.normal.y > 0.999f, "RT1's normal wins over the depth's");
  }
  // the roughness of every smoothness byte, one minus it
  {
    bool ok = true;
    for (int byte = 0; byte < 256; ++byte) {
      const float s = float(byte) / 255.0f;
      ok = ok && Near(rg::LinearRoughness(s), 1.0f - s, 1.0e-7f) && Near(hs::RrLinearRoughness(s), 1.0f - s, 1.0e-7f) && Near(rr::LinearRoughnessOfSmoothness(s), rg::LinearRoughness(s), 0.0f);
    }
    Check(ok && rg::LinearRoughness(2.0f) == 0.0f && rg::LinearRoughness(-1.0f) == 1.0f, "the linear roughness is 1 - smoothness (clamped), as the rules' function says");
    // the GGX alpha of the G-buffer is the square of it: the specular albedo is made with alpha = roughness^2
    Check(Near(gb::AlphaOfSmoothness(0.3f), rg::LinearRoughness(0.3f) * rg::LinearRoughness(0.3f), 1.0e-7f), "alpha = roughness^2 is the G-buffer's GGX alpha (gbuffer_rules.h)");
  }
}

// ---------------------------------------------------------------------------
// the depth, the view and the tangents
// ---------------------------------------------------------------------------
struct Lens {
  double near_plane = 0.3;
  double far_plane = 5000.0;
  double tan_x = 0.6;
  double tan_y = 0.34;
};

// The game's projection (row vectors, the camera looking down -z, depth 0 at the near plane and 1 at the far one, the guest then stores 1 - ndc): the four depth numbers ao.cpp reads.
void LinOf(const Lens& l, float lin[4]) {
  const double n = l.near_plane, f = l.far_plane;
  lin[0] = float(f / (n - f));        // P22
  lin[1] = -1.0f;                     // P23
  lin[2] = float(n * f / (n - f));    // P32
  lin[3] = 0.0f;                      // P33
}
double StoredOf(const Lens& l, double t) {
  const double n = l.near_plane, f = l.far_plane;
  const double ndc = f * (t - n) / ((f - n) * t);
  return 1.0 - ndc;
}

void DepthChecks() {
  const Lens lens;
  float lin[4];
  LinOf(lens, lin);
  // the stored depth back to the distance, over the range of the game: shader, twin, and the definition
  {
    bool ok = true;
    double worst = 0.0;
    for (double t : {0.5, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0, 3000.0}) {
      const float stored = float(StoredOf(lens, t));
      const float a = hs::RrLinearDepth(stored, lin[0], lin[1], lin[2], lin[3]);
      const float b = rg::LinearDepth(stored, lin[0], lin[1], lin[2], lin[3]);
      worst = std::max(worst, std::fabs(double(a) - t) / t);
      ok = ok && a == b;
    }
    Check(ok && worst < 3.0e-3, Num("the linear depth of a stored depth is the distance it came from (worst relative error %.2e), shader and twin the same", worst));
    Check(hs::RrLinearDepth(0.0f, lin[0], lin[1], lin[2], lin[3]) == 60000.0f && rg::LinearDepth(0.0f, lin[0], lin[1], lin[2], lin[3]) == rg::kSky, "a stored depth of 0 is the sky: kSky");
    const float one = hs::RrLinearDepth(1.0f, lin[0], lin[1], lin[2], lin[3]);
    Check(Near(one, float(lens.near_plane), 1.0e-4f), "a stored depth of 1 is the near plane");
  }
  // the view direction through a pixel, for a rotated camera
  {
    // an orthonormal frame turned about all three axes
    const gb::Vec3 forward = gb::Normalize({0.3f, 0.2f, 0.9f});
    const gb::Vec3 right = gb::Normalize(gb::Cross({0.0f, 1.0f, 0.0f}, forward));
    const gb::Vec3 up = gb::Cross(forward, right);
    const float w = 1920.0f, h = 1080.0f;
    const rg::Vec3 centre = rg::ViewDir(w * 0.5f, h * 0.5f, w, h, float(lens.tan_x), float(lens.tan_y), right, up, forward);
    Check(Near(centre.x, forward.x, 1.0e-6f) && Near(centre.y, forward.y, 1.0e-6f) && Near(centre.z, forward.z, 1.0e-6f), "the centre of the picture looks along the camera's forward axis");
    // the right edge and the top edge are at the half angles; picture y is down
    const rg::Vec3 edge = rg::ViewDir(w, h * 0.5f, w, h, float(lens.tan_x), float(lens.tan_y), right, up, forward);
    const float cos_edge = edge.x * forward.x + edge.y * forward.y + edge.z * forward.z;
    Check(Near(cos_edge, float(1.0 / std::sqrt(1.0 + lens.tan_x * lens.tan_x)), 1.0e-5f), "the right edge is the half horizontal field of view off the axis");
    const float side = edge.x * right.x + edge.y * right.y + edge.z * right.z;
    Check(side > 0.0f, "... on the right-hand side");
    const rg::Vec3 top = rg::ViewDir(w * 0.5f, 0.0f, w, h, float(lens.tan_x), float(lens.tan_y), right, up, forward);
    Check(top.x * up.x + top.y * up.y + top.z * up.z > 0.0f, "the top of the picture is on the up side (picture y runs down)");
    bool same = true;
    Rng rng;
    for (int i = 0; i < 500; ++i) {
      const float cx = rng.Range(0, w), cy = rng.Range(0, h);
      const rg::Vec3 a = rg::ViewDir(cx, cy, w, h, float(lens.tan_x), float(lens.tan_y), right, up, forward);
      const hs::float3 b = hs::RrViewDir(cx, cy, w, h, float(lens.tan_x), float(lens.tan_y), ToShim(right), ToShim(up), ToShim(forward));
      same = same && Near3(b, a, 1.0e-6f) && Near(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z), 1.0f, 1.0e-5f);
    }
    Check(same, "the view direction: shader and twin agree, unit length, over random pixels");
  }
  // the tangents on a tilted plane: the normal is exact, with the camera on its front side
  {
    const float w = 64.0f, h = 36.0f;
    const double tx = lens.tan_x, ty = lens.tan_y;
    Rng rng;
    int ok_count = 0;
    for (int trial = 0; trial < 300; ++trial) {
      // facing the camera, not edge on: z from -1 to -0.6, the rest of the unit length in a random direction of the picture's plane
      const float nz = -rng.Range(0.6f, 1.0f);
      const float side = std::sqrt(1.0f - nz * nz);
      const float angle = rng.Range(0.0f, 6.2831853f);
      const gb::Vec3 n = {side * std::cos(angle), side * std::sin(angle), nz};
      const double d = double(rng.Range(5.0f, 80.0f));
      // t (the distance along z) of the pixel: the plane n . (x tan_x t, y tan_y t, t) = -d  =>  t = -d / (n . ray)
      auto depth_at = [&](int px, int py, float* z) {
        const double u = (double(px) + 0.5) / w, v = (double(py) + 0.5) / h;
        const double rx = (u * 2.0 - 1.0) * tx, ry = (1.0 - v * 2.0) * ty;
        const double denom = double(n.x) * rx + double(n.y) * ry + double(n.z);
        *z = float(-d / denom);
      };
      const int px = 10 + int(rng.Next() % 40u), py = 5 + int(rng.Next() % 25u);
      float zc, zl, zr, zu, zd;
      depth_at(px, py, &zc);
      depth_at(px - 1, py, &zl);
      depth_at(px + 1, py, &zr);
      depth_at(px, py - 1, &zu);
      depth_at(px, py + 1, &zd);
      auto point = [&](int qx, int qy, float z) { return rg::ViewPos(float(qx) + 0.5f, float(qy) + 0.5f, w, h, z, float(tx), float(ty)); };
      const rg::Vec3 pc = point(px, py, zc);
      const rg::Vec3 tanx = rg::Tangent(pc, zc, point(px - 1, py, zl), zl, point(px + 1, py, zr), zr);
      const rg::Vec3 tany = rg::Tangent(pc, zc, point(px, py - 1, zu), zu, point(px, py + 1, zd), zd);
      const rg::Vec3 got = rg::NormalOfTangents(tanx, tany);
      // the plane's normal, turned so that z is negative (towards the camera of a view looking along +z)
      const bool close = Near(got.x, n.x, 2.0e-3f) && Near(got.y, n.y, 2.0e-3f) && Near(got.z, n.z, 2.0e-3f);
      // the shader's text on the same points
      const hs::float3 stx = hs::RrTangent(ToShim(pc), zc, ToShim(point(px - 1, py, zl)), zl, ToShim(point(px + 1, py, zr)), zr);
      const hs::float3 sty = hs::RrTangent(ToShim(pc), zc, ToShim(point(px, py - 1, zu)), zu, ToShim(point(px, py + 1, zd)), zd);
      const hs::float3 sgot = hs::RrNormalOfTangents(stx, sty);
      if (close && Near3(sgot, got, 1.0e-5f)) ++ok_count;
    }
    Check(ok_count >= 290, Num("the normal of the depth's tangents on a tilted plane is the plane's normal (%.0f of 300 within 0.002)", ok_count));
  }
  // a step in depth: a fronto-parallel wall at 10 on the left half and one at 20 on the right: the pixels beside the step keep their own wall's normal (0, 0, -1)
  {
    const float w = 32.0f, h = 8.0f;
    const double tx = 0.6, ty = 0.34;
    auto depth_at = [&](int px) { return px < 16 ? 10.0f : 20.0f; };
    auto point = [&](int qx, int qy) { return rg::ViewPos(float(qx) + 0.5f, float(qy) + 0.5f, w, h, depth_at(qx), float(tx), float(ty)); };
    bool ok = true;
    bool shader_ok = true;
    for (int px : {14, 15, 16, 17}) {
      const int py = 4;
      const float zc = depth_at(px), zl = depth_at(px - 1), zr = depth_at(px + 1);
      const rg::Vec3 pc = point(px, py);
      const rg::Vec3 tanx = rg::Tangent(pc, zc, point(px - 1, py), zl, point(px + 1, py), zr);
      const rg::Vec3 tany = rg::Tangent(pc, zc, point(px, py - 1), zc, point(px, py + 1), zc);
      const rg::Vec3 n = rg::NormalOfTangents(tanx, tany);
      ok = ok && Near(n.x, 0.0f, 1.0e-4f) && Near(n.y, 0.0f, 1.0e-4f) && Near(n.z, -1.0f, 1.0e-4f);
      // the shader's text on the same points
      const hs::float3 stx = hs::RrTangent(ToShim(pc), zc, ToShim(point(px - 1, py)), zl, ToShim(point(px + 1, py)), zr);
      const hs::float3 sty = hs::RrTangent(ToShim(pc), zc, ToShim(point(px, py - 1)), zc, ToShim(point(px, py + 1)), zc);
      const hs::float3 sn = hs::RrNormalOfTangents(stx, sty);
      shader_ok = shader_ok && Near(sn.x, 0.0f, 1.0e-4f) && Near(sn.y, 0.0f, 1.0e-4f) && Near(sn.z, -1.0f, 1.0e-4f);
    }
    Check(ok, "a step in depth: the pixels beside it keep their own surface's normal, the tangent never crosses the step");
    Check(shader_ok, "... in the shader's text too");
  }
  // the sky as a neighbour: one side, and both sides sky: no normal
  {
    const float w = 16.0f, h = 16.0f;
    auto pt = [&](int qx, int qy, float z) { return rg::ViewPos(float(qx) + 0.5f, float(qy) + 0.5f, w, h, z, 0.6f, 0.34f); };
    const rg::Vec3 pc = pt(8, 8, 12.0f);
    const rg::Vec3 only_right = rg::Tangent(pc, 12.0f, pt(7, 8, rg::kSky), rg::kSky, pt(9, 8, 12.0f), 12.0f);
    const rg::Vec3 only_left = rg::Tangent(pc, 12.0f, pt(7, 8, 12.0f), 12.0f, pt(9, 8, rg::kSky), rg::kSky);
    const rg::Vec3 none = rg::Tangent(pc, 12.0f, pt(7, 8, rg::kSky), rg::kSky, pt(9, 8, rg::kSky), rg::kSky);
    Check(only_right.x > 0.0f && only_left.x > 0.0f && none.x == 0.0f && none.y == 0.0f && none.z == 0.0f, "a tangent takes the one side that is a surface (it points to increasing x either way) and is 0 with the sky on both");
    const rg::Vec3 nn = rg::NormalOfTangents(none, {0, 1, 0});
    Check(nn.x == 0.0f && nn.y == 0.0f && nn.z == 0.0f, "no tangent: no normal (the pixel faces the camera instead)");
    const rg::Vec3 parallel = rg::NormalOfTangents({1, 0, 0}, {2, 0, 0});
    Check(parallel.x == 0.0f && parallel.y == 0.0f && parallel.z == 0.0f, "two parallel tangents span no surface: no normal");
    const hs::float3 sn = hs::RrNormalOfTangents(hs::float3(0, 0, 0), hs::float3(0, 1, 0));
    Check(sn.x == 0.0f && sn.y == 0.0f && sn.z == 0.0f, "the shader's text: no tangent, no normal");
  }
  // facing
  {
    const rg::Vec3 a = rg::Facing({0, 0, 1}, {0, 0, 1});
    const rg::Vec3 b = rg::Facing({0, 0, -1}, {0, 0, 1});
    const hs::float3 c = hs::RrFacing(hs::float3(0, 0, 1), hs::float3(0, 0, 1));
    Check(a.z == -1.0f && b.z == -1.0f && c.z == -1.0f, "a normal that points away from the camera is turned to face it, one that faces it is kept");
  }
  // the world of a view vector
  {
    const rg::Vec3 w = rg::WorldOfView({1, 2, 3}, {1, 0, 0}, {0, 0, 1}, {0, -1, 0});
    const hs::float3 s = hs::RrWorldOfView(hs::float3(1, 2, 3), hs::float3(1, 0, 0), hs::float3(0, 0, 1), hs::float3(0, -1, 0));
    Check(w.x == 1 && w.y == -3 && w.z == 2 && Near3(s, w, 0.0f), "a view vector in the world is x along right, y along up, z along forward");
  }
}

// ---------------------------------------------------------------------------
// the hit distance, the responsivity and the views
// ---------------------------------------------------------------------------
void RestChecks() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  Check(hs::RrHitDistance(true, 5.0f) == 65504.0f && rg::HitDistance(true, 5.0f) == 65504.0f, "the sky's hit distance is 65504 whatever a writer says");
  Check(hs::RrHitDistance(false, 0.0f) == 0.0f && rg::HitDistance(false, 0.0f) == 0.0f, "a surface with no writer: 0 (the reflection is at the surface)");
  Check(hs::RrHitDistance(false, 12.5f) == 12.5f && rg::HitDistance(false, 12.5f) == 12.5f, "a writer's positive distance wins on a surface");
  Check(hs::RrHitDistance(false, -3.0f) == 0.0f && hs::RrHitDistance(false, nan) == 0.0f && hs::RrHitDistance(false, inf) == 0.0f && hs::RrHitDistance(false, 65504.0f) == 0.0f &&
            rg::HitDistance(false, nan) == 0.0f && rg::HitDistance(false, inf) == 0.0f,
        "a negative, NaN, infinite or sky-sized writer's value is not a distance on a surface: 0");
  // RR-4: two writers (the screen-space reflections' image and the cars' ray traced one) for a pixel: the larger, a NaN is the other's, none is 0, and the sky stays the sky's
  Check(rg::CombineHitSources(0.0f, 0.0f) == 0.0f && rg::CombineHitSources(3.0f, 0.0f) == 3.0f && rg::CombineHitSources(0.0f, 2.5f) == 2.5f && rg::CombineHitSources(3.0f, 2.5f) == 3.0f &&
            rg::CombineHitSources(nan, 2.5f) == 2.5f && rg::CombineHitSources(3.0f, nan) == 3.0f,
        "RR-4: two hit distance writers for a pixel: the larger wins, a NaN is the other's, neither is 0");
  Check(rg::HitDistance(false, rg::CombineHitSources(4096.0f, 0.0f)) == 4096.0f && rg::HitDistance(true, rg::CombineHitSources(4096.0f, 1.5f)) == 65504.0f && rg::HitDistance(false, rg::CombineHitSources(nan, nan)) == 0.0f,
        "a writer's miss distance (4096) is a distance on a surface, the sky is the sky's whatever the writers say, two bad values are the surface's 0");
  // CITY-RT G6: the bounce light's ray length is the LAST choice of the hit distance: a reflection's positive distance keeps the pixel, else the bounce light's stands (0 keeps the surface's, a NaN is not a distance)
  {
    const float sources[] = {0.0f, 1.5f, 4096.0f, -2.0f, nan};
    const float bounces[] = {0.0f, 0.7f, 11.9f, 4096.0f, nan, -1.0f};
    bool same = true;
    for (float s : sources) {
      for (float b : bounces) {
        const float a = hs::RrWithBounce(s, b), t = rg::WithBounce(s, b);
        same = same && (std::isnan(a) ? std::isnan(t) : a == t);
      }
    }
    Check(same, "CITY-RT G6: the shader's RrWithBounce is the twin's WithBounce on every pair of sources and bounce values (NaN included)");
    Check(rg::WithBounce(3.0f, 9.0f) == 3.0f && rg::WithBounce(0.0f, 9.0f) == 9.0f && rg::WithBounce(0.0f, 0.0f) == 0.0f && rg::WithBounce(nan, 9.0f) == 9.0f,
          "CITY-RT G6: a reflection's distance keeps the pixel, else the bounce light's stands (0 where it traced nothing: the surface's)");
    Check(rg::HitDistance(false, rg::WithBounce(0.0f, 4096.0f)) == 4096.0f && rg::HitDistance(false, rg::WithBounce(0.0f, nan)) == 0.0f && rg::HitDistance(true, rg::WithBounce(0.0f, 7.0f)) == 65504.0f,
          "CITY-RT G6: the bounce light's miss distance (4096) is a distance on a surface, a bad value is the surface's 0, the sky stays the sky's");
    // the texel under a render pixel: the pixel scaled by the two sizes, clamped into the image; the identity at the same size
    bool texel_same = true, identity = true, monotone = true, in_range = true;
    for (uint32_t size : {1u, 7u, 960u, 1080u, 1920u, 2160u, 3840u}) {
      for (uint32_t hit : {1u, 3u, 480u, 540u, 960u, 1080u, 1920u}) {
        uint32_t last = 0;
        for (uint32_t p = 0; p < size; p += (size > 64 ? 13u : 1u)) {
          const uint32_t a = hs::RrBounceTexel(p, size, hit), t = rg::BounceTexel(p, size, hit);
          texel_same = texel_same && a == t;
          in_range = in_range && t < hit;
          monotone = monotone && t >= last;
          last = t;
          if (hit == size) identity = identity && t == p;
        }
        if (hit <= size) in_range = in_range && rg::BounceTexel(size - 1, size, hit) == hit - 1u;   // the last pixel reads the last texel (the pass's image is never bigger than the picture)
      }
    }
    Check(texel_same, "CITY-RT G6: the shader's RrBounceTexel is the twin's BounceTexel");
    // a pixel past the edge (the dispatch's partial groups are cut by the shader's own bounds test, but the function alone stays inside the image) reads the last texel
    bool clamped = true;
    for (uint32_t size : {1u, 960u, 1920u, 3840u}) {
      for (uint32_t hit : {1u, 480u, 960u, 1920u}) {
        for (uint32_t past : {0u, 1u, 7u, 4000u}) {
          const uint32_t p = size + past;
          clamped = clamped && rg::BounceTexel(p, size, hit) == hit - 1u && hs::RrBounceTexel(p, size, hit) == hit - 1u;
        }
      }
    }
    Check(clamped, "CITY-RT G6: a pixel at or past the edge of the picture reads the image's last texel (the clamp)");
    Check(identity && in_range && monotone, "CITY-RT G6: the texel under a pixel is the pixel itself at the same size, stays inside the image, never goes backwards, and the last pixel reads the last texel");
    Check(rg::BounceTexel(0, 1920, 960) == 0 && rg::BounceTexel(1, 1920, 960) == 0 && rg::BounceTexel(2, 1920, 960) == 1 && rg::BounceTexel(3839, 3840, 1920) == 1919 && rg::BounceTexel(5, 0, 7) == 6 &&
              rg::BounceTexel(5, 10, 0) == 0,
          "CITY-RT G6: two pixels a texel at half the size (the governor's reduced image), the traced size under a 4K picture, and no division by zero for an empty size");
  }
  Check(hs::RrResponsivity(0.0f, 0.0f) == 0.0f && hs::RrResponsivity(0.3f, 0.6f) == 0.6f && hs::RrResponsivity(0.9f, 0.1f) == 0.9f && hs::RrResponsivity(2.0f, 0.0f) == 1.0f &&
            rg::Responsivity(0.3f, 0.6f) == 0.6f && rg::Responsivity(-1.0f, -1.0f) == 0.0f,
        "the responsivity is the larger of the two masks, between 0 and 1");
  // the views
  const hs::float3 diffuse(0.1f, 0.2f, 0.3f), specular(0.04f, 0.05f, 0.06f);
  const hs::float4 nr(0.0f, 0.6f, 0.8f, 0.7f);
  const float nr_arr[4] = {0.0f, 0.6f, 0.8f, 0.7f};
  const rg::Vec3 diffuse_t = {0.1f, 0.2f, 0.3f}, specular_t = {0.04f, 0.05f, 0.06f};
  bool same = true;
  for (int view = 0; view < rg::kViewCount; ++view) {
    for (float hit : {0.0f, 3.0f, 250.0f, 65504.0f}) {
      for (float resp : {0.0f, 0.5f}) {
        const hs::float3 a = hs::RrViewColour(uint(view), diffuse, nr, specular, hit, resp);
        const rg::Vec3 b = rg::ViewColour(rg::View(view), diffuse_t, nr_arr, specular_t, hit, resp);
        same = same && Near3(a, b, 1.0e-6f);
      }
    }
  }
  Check(same, "the debug views' colours: shader and twin agree for every view");
  const rg::Vec3 n_view = rg::ViewColour(rg::View::kNormal, diffuse_t, nr_arr, specular_t, 0.0f, 0.0f);
  Check(Near(n_view.x, 0.5f, 1e-6f) && Near(n_view.y, 0.8f, 1e-6f) && Near(n_view.z, 0.9f, 1e-6f), "the normal view is 0.5 + 0.5 n");
  const float zero_n[4] = {0, 0, 0, 0};
  const rg::Vec3 n_sky = rg::ViewColour(rg::View::kNormal, diffuse_t, zero_n, specular_t, 0.0f, 0.0f);
  Check(n_sky.x == 0.0f && n_sky.y == 0.0f && n_sky.z == 0.0f, "... black where there is no normal (the sky)");
  const rg::Vec3 r_view = rg::ViewColour(rg::View::kRoughness, diffuse_t, nr_arr, specular_t, 0.0f, 0.0f);
  Check(r_view.x == 0.7f && r_view.y == 0.7f && r_view.z == 0.7f, "the roughness view is the alpha channel as a grey");
  const rg::Vec3 h_sky = rg::ViewColour(rg::View::kHitDistance, diffuse_t, nr_arr, specular_t, 65504.0f, 0.0f);
  const rg::Vec3 h_surface = rg::ViewColour(rg::View::kHitDistance, diffuse_t, nr_arr, specular_t, 0.0f, 0.0f);
  const rg::Vec3 h_near = rg::ViewColour(rg::View::kHitDistance, diffuse_t, nr_arr, specular_t, 3.0f, 0.0f);
  const rg::Vec3 h_far = rg::ViewColour(rg::View::kHitDistance, diffuse_t, nr_arr, specular_t, 250.0f, 0.0f);
  Check(h_sky.x == 0.0f && h_sky.z == 1.0f && h_surface.x == 0.0f && h_surface.z == 0.0f && h_near.x > 0.0f && h_far.x > h_near.x && h_far.x <= 1.0f, "the hit distance view: blue in the sky, black on a surface, brighter with distance");
  Check(rg::ViewColour(rg::View::kOff, diffuse_t, nr_arr, specular_t, 1.0f, 1.0f).x == 0.0f && rg::ViewColour(rg::View(250), diffuse_t, nr_arr, specular_t, 1.0f, 1.0f).x == 0.0f, "no view: black");
}

// ---------------------------------------------------------------------------
// who runs the pass, the views, the push blocks, the registrations
// ---------------------------------------------------------------------------
void RulesChecks(const std::string& gpu) {
  // PassWanted: wanted exactly when RR would run if the guides were there, with no view; with a view, whenever an upscaler runs
  {
    long long cases = 0, wanted_cases = 0;
    bool ok = true;
    for (int debug = 0; debug < 4; ++debug) {
      for (int setting = 0; setting < 3; ++setting) {
        for (int mask = 0; mask < 256; ++mask) {
          rr::PlanInputs in;
          in.debug = rr::DebugSwitch(debug);
          in.setting = rr::Denoiser(setting);
          in.facts.build_has_rr = mask & 1;
          in.facts.files_ok = mask & 2;
          in.facts.streamline_running = mask & 4;
          in.facts.supported = mask & 8;
          in.upscaler_is_dlss = mask & 16;
          in.consumers_active = mask & 32;
          in.guides_ready = mask & 64;
          in.facts.asked = mask & 128;
          const rr::Plan plan = rr::SelectDenoiser(in);
          rr::PlanInputs with_guides = in;
          with_guides.guides_ready = true;
          const bool would_run = rr::SelectDenoiser(with_guides).use_rr;
          ++cases;
          wanted_cases += would_run;
          ok = ok && rg::PassWanted(plan, rg::View::kOff, false) == would_run && rg::PassWanted(plan, rg::View::kOff, true) == would_run;
          // a view with an upscaler running always wants it; a view with none does not add to it
          ok = ok && rg::PassWanted(plan, rg::View::kNormal, true) && rg::PassWanted(plan, rg::View::kNormal, false) == would_run;
        }
      }
    }
    Check(ok && cases == 4 * 3 * 256 && wanted_cases > 0 && wanted_cases < cases, "the guide pass is wanted exactly when Ray Reconstruction would run if its guides were there (the whole table of the rules' inputs), and for a view while an upscaler runs");
    // RR-3: the pass and the RR context are wanted by the same two reasons (dlss_rr_rules.h ContextWanted), and a dropped RR or a card with no room wants neither
    {
      bool same = true;
      for (int w = 0; w <= int(rr::Why::kNotLoaded); ++w) {
        rr::Plan plan;
        plan.use_rr = w == int(rr::Why::kRrOn);
        plan.why = rr::Why(w);
        same = same && rg::PassWanted(plan, rg::View::kOff, true) == rr::ContextWanted(plan.why);
      }
      Check(same, "RR-3: the guide pass is wanted for exactly the reasons the RR context is wanted for (no view)");
      rr::PlanInputs failed;
      failed.debug = rr::DebugSwitch::kOn;
      failed.setting = rr::Denoiser::kRr;
      failed.facts = {true, true, true, true, true};  // build, files, started, supported, failed
      failed.facts.asked = true;
      failed.consumers_any = true;
      failed.upscaler_is_dlss = true;
      failed.guides_ready = false;
      Check(rg::PassWanted(rr::SelectDenoiser(failed), rg::View::kOff, true) == false, "RR-3: a session that dropped RR does not run the guide pass");
      failed.facts.failed = false;
      Check(rg::PassWanted(rr::SelectDenoiser(failed), rg::View::kOff, true), "... and the same inputs without the failure do");
      failed.room_ok = false;
      Check(rg::PassWanted(rr::SelectDenoiser(failed), rg::View::kOff, true) == false, "RR-3: no room in video memory for the RR context: the guide pass (and its 28 bytes a pixel) is not wanted either");
    }
    // the plain defaults: nothing on, nothing wanted
    rr::PlanInputs idle;
    Check(!rg::PassWanted(rr::SelectDenoiser(idle), rg::View::kOff, true), "with the debug switch off and no view the pass is not wanted, even with an upscaler running");
  }
  // the views
  {
    Check(rg::ViewOf(0) == rg::View::kOff && rg::ViewOf(1) == rg::View::kDiffuseAlbedo && rg::ViewOf(6) == rg::View::kResponsivity && rg::ViewOf(7) == rg::View::kOff && rg::ViewOf(-1) == rg::View::kOff &&
              rg::ViewOf(1000) == rg::View::kOff && rg::kViewCount == 7,
          "the views: 0 off, 1 to 6 one each, anything else off");
    std::set<std::string> names;
    for (int v = 0; v < rg::kViewCount; ++v) names.insert(rg::ViewName(rg::View(v)));
    Check(names.size() == size_t(rg::kViewCount) && std::string(rg::ViewName(rg::View::kCount)) == "?", "each view has its own name");
    std::string hlsli;
    Check(Slurp(gpu + "/hlsl/rr_guides_common.hlsli", &hlsli), "hlsl/rr_guides_common.hlsli is readable");
    Check(RR_VIEW_OFF == int(rg::View::kOff) && RR_VIEW_DIFFUSE == int(rg::View::kDiffuseAlbedo) && RR_VIEW_NORMAL == int(rg::View::kNormal) && RR_VIEW_ROUGHNESS == int(rg::View::kRoughness) &&
              RR_VIEW_SPECULAR == int(rg::View::kSpecularAlbedo) && RR_VIEW_HIT == int(rg::View::kHitDistance) && RR_VIEW_RESPONSIVITY == int(rg::View::kResponsivity),
          "the shader's view numbers are the twin's");
    // the setting: defined in the three generated files, a debug-only integer 0 to the last view
    std::string manifest, settings_h, meta;
    const bool read = Slurp(gpu + "/../settings/generated/manifest.json", &manifest) && Slurp(gpu + "/../settings/generated/settings_gpu.h", &settings_h) && Slurp(gpu + "/../settings/generated/settings_meta.inc", &meta);
    Check(read, "the generated settings files are readable");
    if (read) {
      const std::string range = "rex::Range{0, " + std::to_string(rg::kViewCount - 1) + "}";
      const size_t at = At(manifest, "\"mcla_gpu_dlss_rr_view\"");
      Check(at != std::string::npos && At(manifest, "\"type\": \"i32\"", at) < At(manifest, "}", at) && At(manifest, "\"default\": \"0\"", at) < At(manifest, "}", at) && At(manifest, "rex::kDebugOnly", at) < At(manifest, "}", at) &&
                At(manifest, range, at) < At(manifest, "}", at),
            "the manifest has mcla_gpu_dlss_rr_view: an integer, default 0, debug only, " + range);
      const size_t decl = At(settings_h, std::string("REX_SETTING(i32, ") + "mcla_gpu_dlss_rr_view, 0,");
      Check(decl != std::string::npos && At(settings_h, "rex::kDebugOnly, " + range, decl) < At(settings_h, std::string("REX_SETTING") + "(", decl + 10) && Has(settings_h, "inline auto& mcla_gpu_dlss_rr_view = g_gpu.mcla_gpu_dlss_rr_view;"),
            "settings_gpu.h defines mcla_gpu_dlss_rr_view (debug only, " + range + ") and its alias");
      Check(Has(meta, "{\"mcla_gpu_dlss_rr_view\", \"MCLA/GPU\"},"), "settings_meta.inc lists it");
    }
  }
  // the push blocks: the C++ structs' layout and the HLSL structs' members, in the same order
  {
    Check(sizeof(rg::GuidesPush) == 112 && offsetof(rg::GuidesPush, slots2) == 16 && offsetof(rg::GuidesPush, info) == 32 && offsetof(rg::GuidesPush, right) == 48 && offsetof(rg::GuidesPush, up) == 64 &&
              offsetof(rg::GuidesPush, forward) == 80 && offsetof(rg::GuidesPush, lin) == 96,
          "the guide pass's push block is seven rows of 16 bytes at the offsets the HLSL's rows have");
    Check(sizeof(rg::ViewPush) == 48 && offsetof(rg::ViewPush, slots2) == 16 && offsetof(rg::ViewPush, sizes) == 32, "the view pass's push block is three rows");
    auto members = [&](const std::string& text, const std::string& name) {
      std::vector<std::string> out;
      const size_t at = At(text, "struct " + name);
      if (at == std::string::npos) return out;
      const size_t open = At(text, "{", at);
      const size_t close = At(text, "};", open);
      std::istringstream body(text.substr(open + 1, close - open - 1));
      std::string line;
      std::string statements;
      while (std::getline(body, line)) {
        const size_t slash = line.find("//");
        if (slash != std::string::npos) line = line.substr(0, slash);
        statements += line + " ";
      }
      // every statement (up to a semicolon) is a type and a member, whatever the lines
      std::istringstream all(statements);
      std::string statement;
      while (std::getline(all, statement, ';')) {
        std::istringstream words(statement);
        std::string type, member, extra;
        if (words >> type >> member) out.push_back(type + " " + member + ((words >> extra) ? " " + extra : std::string()));
      }
      return out;
    };
    std::string cs, view_cs;
    const bool read = Slurp(gpu + "/hlsl/rr_guides_cs.hlsl", &cs) && Slurp(gpu + "/hlsl/rr_guides_view_cs.hlsl", &view_cs);
    Check(read, "the compute shaders are readable");
    if (read) {
      const std::vector<std::string> want_guides = {"uint4 slots", "uint4 slots2", "uint4 info", "float4 right", "float4 up", "float4 forward", "float4 lin"};
      const std::vector<std::string> want_view = {"uint4 slots", "uint4 slots2", "uint4 sizes"};
      Check(members(cs, "RrGuidesPush") == want_guides, "rr_guides_cs.hlsl's RrGuidesPush has the C++ struct's seven rows in order");
      Check(members(view_cs, "RrGuidesViewPush") == want_view, "rr_guides_view_cs.hlsl's RrGuidesViewPush has the C++ struct's three rows in order");
      // the register spaces the layout declares: the four texture sets at 0 to 3 (host_pass_common.hlsli), the outputs at 4, the push at 5
      std::string header, source;
      Check(Slurp(gpu + "/rr_guides.h", &header) && Slurp(gpu + "/rr_guides.cpp", &source), "rr_guides.h and rr_guides.cpp are readable");
      Check(Has(header, "static constexpr uint32_t kOutputSet = 4;") && Has(header, "static constexpr uint32_t kPushSpace = 5;"), "the layouts put the outputs at set 4 and the push block at space 5");
      Check(Has(cs, "register(b0, space5)") && Has(view_cs, "register(b0, space5)") && Has(cs, "register(u0, space4)") && Has(cs, "register(u4, space4)") && Has(view_cs, "register(u0, space4)") &&
                !Has(cs, "register(u5,"),
            "the shaders declare the push block at space 5 and the five outputs u0 to u4 at space 4 (the view's one at u0)");
      Check(Has(source, "for (uint32_t set = 0; set < 4; ++set)") && Has(source, "builder.addPushConstant(0, kPushSpace, push_bytes, plume::RenderShaderStageFlag::COMPUTE);") &&
                Has(source, "guides_builder_.addReadWriteTexture(i);"),
            "the layout is the host pass's four texture sets, then the outputs, then the push block");
      // the formats the shaders declare for each image: u0..u4 in Guide order
      const struct {
        const char* name;
        rg::Guide guide;
      } outs[] = {{"g_diffuse", rg::Guide::kDiffuseAlbedo}, {"g_specular", rg::Guide::kSpecularAlbedo}, {"g_normal", rg::Guide::kNormalRoughness}, {"g_hit", rg::Guide::kHitDistance},
                  {"g_responsivity", rg::Guide::kResponsivity}};
      bool formats = true;
      for (size_t i = 0; i < 5; ++i) {
        const size_t at = At(cs, std::string("RWTexture2D<") + (rg::SpecOf(outs[i].guide).format == rr::Format::kRgba16Float ? "float4" : "float") + "> " + outs[i].name + " : register(u" + std::to_string(i) + ", space4)");
        const size_t fmt = rg::SpecOf(outs[i].guide).format == rr::Format::kRgba16Float ? At(cs, "[[vk::image_format(\"rgba16f\")]]\n#endif\nRWTexture2D<float4> " + std::string(outs[i].name))
                                                                                         : At(cs, "[[vk::image_format(\"r16f\")]]\n#endif\nRWTexture2D<float> " + std::string(outs[i].name));
        formats = formats && at != std::string::npos && fmt != std::string::npos && size_t(outs[i].guide) == i;
      }
      Check(formats, "the five outputs are u0..u4 in the twin's guide order, each declared as the format of its image (rgba16f or r16f for Vulkan)");
      Check(Has(cs, "g_responsivity[p] = sky ? 0.0f") && Has(cs, "g_hit[p] = RrHitDistance(sky, source)") && Has(cs, "const bool sky = !(stored > 0.0f);") && Has(cs, "g_normal[p] = float4(px.normal, px.roughness)") &&
                Has(cs, "g_diffuse[p] = float4(px.diffuse, 1.0f)") && Has(cs, "g_specular[p] = float4(px.specular, 1.0f)"),
            "the pass writes every guide in every pixel: the sky is stored depth 0, the normal's alpha is the roughness, the albedos' alpha is 1");
      Check(Has(cs, "if (g_push.slots2.z != kNone)") && Has(cs, "source = g_textures[g_push.slots2.z].Load(int3(p, 0)).x;") && Has(cs, "if (g_push.slots2.w != kNone)") && Has(cs, "source = max(source, g_textures[g_push.slots2.w].Load(int3(p, 0)).x);") &&
                Has(source, "push.slots2[2] = hit_used[0] ? hit_used[0]->slot : rr_guides::kNoSlot;") && Has(source, "push.slots2[3] = hit_used[1] ? hit_used[1]->slot : rr_guides::kNoSlot;") &&
                Has(source, "plume::RenderTextureBarrier reads[9];") && Has(source, "hit->format == plume::RenderFormat::R16_FLOAT") && Has(source, "hit->width == width && hit->height == height") &&
                Has(header, "const HostImage* hit[2] = {nullptr, nullptr};") && !Has(header, "hit_slot") && !Has(source, "hit_slot"),
            "RR-4: the pass reads up to two hit distance writers' images (R16_FLOAT at the render size, each in the read state, the larger distance wins), and the old single slot is gone");
      Check(Has(cs, "if (g_push.info.w != kNone && !(source > 0.0f))") && Has(cs, "g_textures[g_push.info.w].GetDimensions(hitWidth, hitHeight);") && Has(cs, "RrBounceTexel(uint(p.x), uint(size.x), hitWidth)") &&
                Has(cs, "RrBounceTexel(uint(p.y), uint(size.y), hitHeight)") && Has(cs, "source = RrWithBounce(source, g_textures[g_push.info.w].Load(int3(q, 0)).x);") &&
                Has(source, "push.info[3] = bounce_used ? bounce_used->slot : rr_guides::kNoSlot;") && Has(source, "in.bounce_hit->format == plume::RenderFormat::R16G16_FLOAT") && Has(source, "in.bounce_hit->width != 0 && in.bounce_hit->height != 0") &&
                Has(source, "reads[reads_count++] = plume::RenderTextureBarrier(bounce_used->texture.get(), Layout::SHADER_READ);") && Has(source, "bounce_hits_used_ += bounce_used ? 1u : 0u;") &&
                Has(header, "const HostImage* bounce_hit = nullptr;"),
            "CITY-RT G6: the bounce light's hit image (RG16F, any size above 0) is the LAST choice of the hit distance, read at the texel under the pixel, only where no reflection wrote one; the slot goes in info.w (none: kNone), it is put in the read state and counted");
      Check(Has(cs, "g_hit[p] = RrHitDistance(sky, source)") && At(cs, "source = RrWithBounce(") > At(cs, "source = max(source, g_textures[g_push.slots2.w]") && At(cs, "source = RrWithBounce(") < At(cs, "g_hit[p] = RrHitDistance(sky, source)"),
            "CITY-RT G6: the bounce light's distance comes after both reflection writers' and before the guide's value is made of the source");
      Check(Has(source, "push.info[3] = ") && !Has(source, "push.info[3] = 0;"), "CITY-RT G6: info.w is no longer unused (the old 0 would read as slot 0)");
      Check(Has(cs, "g_textures_ms[slot].Load(p, g_push.info.z)") && Has(cs, "#if RR_MS"), "the multisampled variant reads the host sample that is the guest's sample 0 (info.z)");
      Check(Has(source, "push.info[2] = frame.sample0;") && Has(source, "frame.host_samples > 1"), "the host picks the variant by the planes' sample count and passes the guest's sample 0");
      // the pass is Want-driven and gated
      Check(Has(source, "gbuffer_->Want(gbuffer::Consumer::kRayReconstruction, wanted_ ? rr_guides::kWantedPlanes : 0u);") && rg::kWantedPlanes == 7, "BeginFrame asks the G-buffer for all three planes while wanted, none otherwise");
      Check(Has(source, "rr::FrameGate(facts)") && Has(source, "facts.planes_valid = frame.valid;") && Has(source, "if (!wanted_ || !list || !gbuffer_ || !device_) {"),
            "Record runs only while wanted and goes through the frame gate (no G-buffer, a plane missing, the wrong size, the pause view)");
      Check(Has(source, "ready_ = true;") && Has(source, "guides_.specular_hit_distance") && Has(source, "guides_.normal_roughness") && Has(source, "guides_.responsivity") && Has(source, "guides_.diffuse_albedo") &&
                Has(source, "guides_.specular_albedo"),
            "Record fills the TemporalGuides RR-3 tags: all five guides");
      Check(!Has(At(source, "bool RrGuides::Record") != std::string::npos ? source.substr(At(source, "bool RrGuides::Record")) : std::string(), "NoteRrGuides"), "the guide pass does not hand its images to the plan itself (RR-3's one call): the gate of perf/DLSS-RR.md");
    }
  }
  // the pipelines' registrations: CMake lines, the blobs, the precache owner (a number of its own)
  {
    std::string cmake, shaders, precache;
    const bool read = Slurp(gpu + "/CMakeLists.txt", &cmake) && Slurp(gpu + "/host_shaders.h", &shaders) && Slurp(gpu + "/host_precache.h", &precache);
    Check(read, "CMakeLists.txt, host_shaders.h and host_precache.h are readable");
    if (read) {
      Check(Has(cmake, "larecomp_gpu_host_shader(rr_guides_cs cs_6_0 DEFINES RR_MS=0)") && Has(cmake, "larecomp_gpu_host_shader(rr_guides_ms_cs cs_6_0 SOURCE rr_guides_cs DEFINES RR_MS=1)") &&
                Has(cmake, "larecomp_gpu_host_shader(rr_guides_view_cs cs_6_0)") && Has(cmake, "    rr_guides.cpp\n") && Has(cmake, "add_executable(larecomp_rr_guides_test EXCLUDE_FROM_ALL rr_guides_test.cpp)"),
            "CMake builds the three shaders (single-sample and multisampled planes, the view), the pass's source and this test");
      bool blobs = true;
      for (const char* stem : {"rr_guides_cs", "rr_guides_ms_cs", "rr_guides_view_cs"}) {
        blobs = blobs && Has(shaders, std::string("#include \"") + stem + ".hlsl.dxil.h\"") && Has(shaders, std::string("#include \"") + stem + ".hlsl.spirv.h\"") && Has(shaders, std::string("MCLA_GPU_HOST_SHADER_BLOB(") + stem + ")");
      }
      Check(blobs && Has(shaders, "inline HostShaderBlob RrGuidesShader(bool multisampled)") && Has(shaders, "inline HostShaderBlob RrGuidesViewShader()"), "host_shaders.h includes both backends' blobs of the three shaders and hands them out");
      // the owners of the precache have numbers of their own
      std::map<int, int> owners;
      const size_t first = At(precache, "enum Owner : uint32_t {");
      const size_t last = At(precache, "};", first);
      std::istringstream body(precache.substr(first, last - first));
      std::string line;
      int rr_owner = -1;
      while (std::getline(body, line)) {
        const size_t eq = line.find(" = ");
        const size_t comma = line.find(",", eq == std::string::npos ? 0 : eq);
        if (eq != std::string::npos && comma != std::string::npos) {
          const int value = std::atoi(line.substr(eq + 3, comma - eq - 3).c_str());
          ++owners[value];
          if (line.find("kRrGuides") != std::string::npos) rr_owner = value;
        }
      }
      bool unique = !owners.empty();
      for (const auto& entry : owners) unique = unique && entry.second == 1;
      Check(unique && rr_owner == 31 && owners.size() >= 12, "every precache owner has a number of its own; the guide pass's is 31");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') gpu = argv[i];
  }
  if (gpu.empty()) gpu = std::filesystem::path(__FILE__).parent_path().string();
  ImagesChecks();
  SpecularAlbedoChecks();
  DecodeChecks();
  DepthChecks();
  RestChecks();
  RulesChecks(gpu);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
