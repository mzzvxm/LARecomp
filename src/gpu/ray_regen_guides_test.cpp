// Check of AMD FSR Ray Regeneration's guide conversion (ray_regen_guides_rules.h, hlsl/ray_regen_common.hlsli, hlsl/ray_regen_ssr_cs.hlsl, ray_regen_guides.h / ray_regen_guides.cpp, perf/RAY-REGEN.md,
// packages RG-2 and RG-3). A program of its own: no renderer, no Windows, no GPU, no AMD runtime; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ray_regen_guides_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ray_regen_guides_test [<src/gpu dir>]    (the folder defaults to the one this file is in)
//
// It checks:
//   THE TWIN     the .hlsli compiled AS C++ (through the shim of the HLSL types below) against the twin in ray_regen_guides_rules.h on random texels, function by function.
//   THE FORMATS  the octahedral normals against AMD's decode (a round trip over 20,000 directions, every axis, both hemispheres), the roughness clamp, the material fraction, the signal's floor
//                (a road's negative trace, NaN), the hit distance's floor, the depth sign and the depth outside the bounds of the dispatch for what is not traced.
//   THE MOTION   a static point seen by two cameras (the previous and the current, rotated and moved, with a jitter): the texel's motion is PreviousUV - CurrentUV of the unjittered pictures, and its
//                depth delta PreviousLinearDepth - CurrentLinearDepth in the signed convention (a camera that moves forward makes a point nearer: the delta is -1 for 1 unit).
//   THE CAMERA   BuildCamera: a point 10 units ahead is at view z -10 and clip w +10 for a rotated camera (and +10 / +10 with the mirrored convention), the signed depth of the rules agrees, the position
//                delta of a motion made from two poses is previous minus current, the bounds put the not-traced depth outside, the jitter at the trace's size.
//   THE FRAME    DecideRegen against a restatement of its order over every fact set; the outcome of a refused dispatch is the effect's own filter; the images' bytes and that the dispatch plan (ray_regen_rules.h
//                PlanDispatch) accepts exactly these images and refuses a wrong size or format.
//   THE SOURCES  the push block against the HLSL's struct and defines, the five UAVs' formats and spaces against SpecOf, the CMake lines and the blobs of the shader, the include that gives it the extra
//                words, the reflections' own push block untouched.
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
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_rules.h"
#include "ray_regen_guides_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: the shim of what the file uses (a namespace of its own so that the standard library's abs, min, max ... are not in the way)
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
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float abs(float v) { return std::fabs(v); }
// HLSL's min and max return the other operand where one is NaN (fmin / fmax do the same).
inline float min(float a, float b) { return std::fmin(a, b); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline float saturate(float v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }
#include "hlsl/ray_regen_common.hlsli"
}  // namespace hlsl_shim

namespace hs = hlsl_shim;
namespace rg = mcla::gpu::rayregen;
namespace gd = mcla::gpu::rayregen::guides;
namespace rr = mcla::gpu::rr;
namespace gb = mcla::gpu::gbuffer;
namespace ssr = mcla::gpu::ssr;

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

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

// A small deterministic random source.
struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return uint32_t(s >> 32);
  }
  float F() { return float(Next()) * (1.0f / 4294967296.0f); }
  float Range(float lo, float hi) { return lo + (hi - lo) * F(); }
};

std::string Read(const std::string& dir, const std::string& rel) {
  std::ifstream in(dir + "/" + rel, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// --- THE TWIN ---------------------------------------------------------------------------------------------------------------------------------------

void CheckTwin() {
  Rng rng;
  bool octa = true, normals = true, depth = true, motion = true, spec = true, signal = true;
  for (int i = 0; i < 4000; ++i) {
    // a direction (both hemispheres, the axes included now and then)
    rg::Vec3 n{rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    if (i % 50 == 0) n = {0, 0, (i % 100 == 0) ? 1.0f : -1.0f};
    if (i % 50 == 1) n = {(i % 100 == 1) ? 1.0f : -1.0f, 0, 0};
    if (i % 50 == 2) n = {0, 0.5f, -0.5f};
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len < 1e-3f) continue;
    n = {n.x / len, n.y / len, n.z / len};
    const rg::Vec2 uv = rg::OctahedronUv(n);
    const hs::float2 huv = hs::RgOctahedronUv(hs::float3(n.x, n.y, n.z));
    octa = octa && Near(uv.x, huv.x, 1e-6) && Near(uv.y, huv.y, 1e-6);
    const float rough = rng.Range(-0.5f, 1.5f);
    const uint32_t material = rng.Next() % 6;
    const rg::NormalsTexel t = rg::MakeNormalsTexel(n, rough, material);
    const hs::float4 h = hs::RgNormalsTexel(hs::float3(n.x, n.y, n.z), rough, material);
    normals = normals && Near(t.u, h.x, 1e-6) && Near(t.v, h.y, 1e-6) && Near(t.roughness, h.z, 1e-6) && Near(t.material, h.w, 1e-6);
    const bool traced = (rng.Next() & 1) != 0;
    const float distance = rng.Range(-10.0f, 70000.0f);
    const float sign = (rng.Next() & 1) ? 1.0f : -1.0f;
    depth = depth && gd::DepthTexel(traced, distance, sign) == hs::RgDepthTexel(traced, distance, sign);
    const float cu = rng.F(), cv = rng.F(), pu = rng.F(), pv = rng.F(), pz = rng.Range(0, 100), t_cur = rng.Range(0, 100);
    const bool valid = (rng.Next() & 3) != 0;
    const gd::Float4 m = gd::MotionTexel(cu, cv, pu, pv, pz, t_cur, valid, sign);
    const hs::float4 hm = hs::RgMotionTexel(hs::float2(cu, cv), pu, pv, pz, t_cur, valid, sign);
    motion = motion && Near(m.x, hm.x, 1e-6) && Near(m.y, hm.y, 1e-6) && Near(m.z, hm.z, 1e-4) && Near(m.w, hm.w, 1e-6);
    const float f0 = rng.F(), scale = rng.Range(0, 2), bias = rng.Range(-0.2f, 1.2f);
    const hs::float3 albedo = hs::RgSpecularAlbedo(hs::float3(f0, f0 * 0.5f, 0.04f), hs::float2(scale, bias));
    spec = spec && Near(gd::SpecularAlbedoChannel(f0, scale, bias), albedo.x, 1e-6) && Near(gd::SpecularAlbedoChannel(f0 * 0.5f, scale, bias), albedo.y, 1e-6) &&
           Near(gd::SpecularAlbedoChannel(0.04f, scale, bias), albedo.z, 1e-6);
    const float r = rng.Range(-2, 5), g = rng.Range(-2, 5), b = rng.Range(-2, 5), hit = rng.Range(-1, 5000);
    const gd::Float4 s = gd::SignalTexel(r, g, b, hit, traced);
    const hs::float4 hsg = hs::RgSignalTexel(hs::float3(r, g, b), hit, traced);
    signal = signal && Near(s.x, hsg.x, 1e-6) && Near(s.y, hsg.y, 1e-6) && Near(s.z, hsg.z, 1e-6) && Near(s.w, hsg.w, 1e-6);
  }
  Check(octa, "the octahedral map: the shader's text and the twin agree on 4,000 directions");
  Check(normals, "the normals texel: the shader's text and the twin agree (octahedral rg, clamped linear roughness, material fraction)");
  Check(depth, "the depth texel: the shader's text and the twin agree, for both signs");
  Check(motion, "the motion texel: the shader's text and the twin agree");
  Check(spec, "the specular albedo: the shader's text and the twin agree per channel");
  Check(signal, "the signal texel: the shader's text and the twin agree");
  Check(hs::kRgNoDepth == gd::kNoDepth && hs::kRgMinHit == gd::kMinHit && hs::kRgMaterialMax == 3.0f, "the shader's constants are the twin's (not-traced depth 60000, minimum hit distance 0.02, material types 0 to 3)");
  // NaN: the HLSL max keeps the other operand, so a NaN radiance reads 0 and a NaN hit distance reads the floor
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const hs::float4 hn = hs::RgSignalTexel(hs::float3(nan, 1.0f, -1.0f), nan, true);
  const gd::Float4 tn = gd::SignalTexel(nan, 1.0f, -1.0f, nan, true);
  Check(hn.x == 0.0f && hn.y == 1.0f && hn.z == 0.0f && hn.w == gd::kMinHit && tn.x == 0.0f && tn.y == 1.0f && tn.z == 0.0f && tn.w == gd::kMinHit, "a NaN radiance is 0 and a NaN hit distance is the floor, in both");
}

// --- THE FORMATS --------------------------------------------------------------------------------------------------------------------------------------

void CheckFormats() {
  Rng rng;
  // AMD's decode (denoising.md 5.3.2) round-trips what the encode made, in both hemispheres.
  double worst = 0.0;
  for (int i = 0; i < 20000; ++i) {
    rg::Vec3 n{rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len < 1e-3f) continue;
    n = {n.x / len, n.y / len, n.z / len};
    const hs::float2 uv = hs::RgOctahedronUv(hs::float3(n.x, n.y, n.z));
    const rg::Vec3 back = rg::OctahedronUvToNormal({uv.x, uv.y});
    worst = std::max(worst, double(std::fabs(back.x - n.x) + std::fabs(back.y - n.y) + std::fabs(back.z - n.z)));
  }
  Check(worst < 2e-5, "the octahedral map round-trips through AMD's decode (worst error " + std::to_string(worst) + ")");
  const hs::float2 up = hs::RgOctahedronUv(hs::float3(0, 0, 1));
  const hs::float2 down = hs::RgOctahedronUv(hs::float3(0, 0, -1));
  Check(Near(up.x, 0.5) && Near(up.y, 0.5), "the +z axis is the middle of the map");
  Check(Near(down.x, 1.0) && Near(down.y, 1.0), "the -z axis is the corner (sign taken as positive at 0, as the G-buffer's own code)");
  // the G-buffer's own code is the same map
  bool same = true;
  for (int i = 0; i < 2000; ++i) {
    gb::Vec3 n{rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len < 1e-3f) continue;
    n = {n.x / len, n.y / len, n.z / len};
    const auto code = gb::OctEncode(n);
    const hs::float2 uv = hs::RgOctahedronUv(hs::float3(n.x, n.y, n.z));
    same = same && Near(code.x, uv.x, 1e-5) && Near(code.y, uv.y, 1e-5);
  }
  Check(same, "the G-buffer's RT1 code (gbuffer::OctEncode) is the same octahedral map as the guide's");
  // the normals texel
  const hs::float4 t = hs::RgNormalsTexel(hs::float3(0, 1, 0), 0.75f, 0u);
  Check(Near(t.z, 0.75) && t.w == 0.0f, "the normals texel carries the roughness in b and the default material in a");
  Check(hs::RgNormalsTexel(hs::float3(0, 1, 0), 3.0f, 9u).z == 1.0f && hs::RgNormalsTexel(hs::float3(0, 1, 0), -3.0f, 9u).z == 0.0f, "the roughness is clamped to 0..1");
  Check(Near(hs::RgNormalsTexel(hs::float3(0, 1, 0), 0.5f, 1u).w, 1.0 / 3.0) && Near(hs::RgNormalsTexel(hs::float3(0, 1, 0), 0.5f, 2u).w, 2.0 / 3.0) &&
            hs::RgNormalsTexel(hs::float3(0, 1, 0), 0.5f, 3u).w == 1.0f && hs::RgNormalsTexel(hs::float3(0, 1, 0), 0.5f, 7u).w == 1.0f,
        "the material type is a fraction of 3 (denoising.md 5.4), and never above the last type");
  // the signal
  const hs::float4 road = hs::RgSignalTexel(hs::float3(-0.4f, 0.2f, -0.1f), 12.0f, true);
  Check(road.x == 0.0f && road.y == 0.2f && road.z == 0.0f && road.w == 12.0f, "a road's negative trace is floored at 0 per channel (the game's own reflection keeps the rest)");
  const hs::float4 none = hs::RgSignalTexel(hs::float3(5, 5, 5), 100.0f, false);
  Check(none.x == 0 && none.y == 0 && none.z == 0 && none.w == 0, "a texel nothing was traced for is all 0 (alpha 0 is how the resolve knows)");
  Check(hs::RgSignalTexel(hs::float3(1, 1, 1), 0.0f, true).w == hs::kRgMinHit, "a traced texel never has hit distance 0");
  Check(hs::RgSignalTexel(hs::float3(1, 1, 1), 4096.0f, true).w == 4096.0f, "a miss keeps the effect's own 'far away' distance (4096)");
  // the depth
  Check(hs::RgDepthTexel(true, 10.0f, -1.0f) == -10.0f && hs::RgDepthTexel(true, 10.0f, 1.0f) == 10.0f, "the signed depth: negative ahead for the game's view space, positive for the mirrored one");
  const rg::Bounds b = rg::LinearDepthBounds(5.0, 10000.0);
  Check(std::fabs(hs::RgDepthTexel(false, 10.0f, -1.0f)) > b.max && std::fabs(hs::RgDepthTexel(true, 70000.0f, -1.0f)) > b.max && std::fabs(hs::RgDepthTexel(true, -3.0f, -1.0f)) > b.max,
        "a texel that is not traced, or has no distance, is outside the bounds: the dispatch passes it through");
  Check(!rg::InBounds(std::fabs(hs::RgDepthTexel(false, 0.0f, -1.0f)), b) && rg::InBounds(std::fabs(hs::RgDepthTexel(true, 25.0f, -1.0f)), b), "InBounds agrees: traced depth is in, not-traced depth is out");
  Check(hs::RgDepthTexel(false, 0.0f, -1.0f) < 0.0f && hs::RgDepthTexel(false, 0.0f, 1.0f) > 0.0f, "the not-traced depth has the convention's sign too");
  Check(hs::kRgNoDepth < 65504.0f, "the not-traced depth fits a half float");
  Check(gd::DepthSign(false) == -1.0f && gd::DepthSign(true) == 1.0f && gd::DepthSign() == (rg::kLinearDepthPositiveAhead ? 1.0f : -1.0f), "the depth sign follows the convention constant");
  // the specular albedo is a plain reflectance
  const hs::float3 a = hs::RgSpecularAlbedo(hs::float3(0.04f, 0.04f, 0.04f), hs::float2(1.0f, 0.02f));
  Check(Near(a.x, 0.06) && Near(a.y, 0.06) && Near(a.z, 0.06), "F0 0.04 with scale 1 and bias 0.02 is 0.06");
  const hs::float3 over = hs::RgSpecularAlbedo(hs::float3(0.9f, 0.9f, 0.9f), hs::float2(1.5f, 0.3f));
  Check(over.x == 1.0f, "the reflectance is saturated at 1 (the BRDF table overshoots at grazing)");
}

// --- THE MOTION ---------------------------------------------------------------------------------------------------------------------------------------

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
  const double l = std::sqrt(Dot(a, a));
  return {a.x / l, a.y / l, a.z / l};
}

// A camera pose: position, and the axes a yaw / pitch give (right handed: right x up = back; the camera looks along `forward`).
struct Pose {
  V3 pos;
  V3 right, up, forward;
};
Pose MakePose(V3 pos, double yaw, double pitch) {
  Pose p;
  p.pos = pos;
  p.forward = Norm({std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)});
  const V3 world_up{0, 1, 0};
  p.right = Norm(Cross(p.forward, world_up));
  p.up = Cross(p.right, p.forward);
  return p;
}
// A point in the pose's AO view space (x right, y up, z forward: the distance) and back.
V3 ToView(const Pose& p, V3 world) {
  const V3 d = Sub(world, p.pos);
  return {Dot(d, p.right), Dot(d, p.up), Dot(d, p.forward)};
}
V3 FromView(const Pose& p, V3 v) { return {p.pos.x + p.right.x * v.x + p.up.x * v.y + p.forward.x * v.z, p.pos.y + p.right.y * v.x + p.up.y * v.y + p.forward.y * v.z, p.pos.z + p.right.z * v.x + p.up.z * v.y + p.forward.z * v.z}; }

// The camera motion the occlusion hands the reflections: the 3x4 that takes this frame's view space into the previous one's.
void MotionRows(const Pose& now, const Pose& before, float out[12]) {
  const V3 axes_now[3] = {now.right, now.up, now.forward};
  const V3 axes_before[3] = {before.right, before.up, before.forward};
  const V3 shift = Sub(now.pos, before.pos);
  for (int row = 0; row < 3; ++row) {
    out[row * 4 + 0] = float(Dot(axes_before[row], axes_now[0]));
    out[row * 4 + 1] = float(Dot(axes_before[row], axes_now[1]));
    out[row * 4 + 2] = float(Dot(axes_before[row], axes_now[2]));
    out[row * 4 + 3] = float(Dot(axes_before[row], shift));
  }
}

// ssr_common.hlsli SsrViewPos and SsrToPrevious, in C++ (the reflections' own test compiles them; the twin here keeps this test alone).
struct Prev {
  bool ok;
  float u, v, z;
};
Prev ToPrevious(const float m[12], V3 p, float tan_x, float tan_y) {
  const float x = float(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]);
  const float y = float(m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7]);
  const float z = float(m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
  Prev r{false, 0, 0, z};
  if (z > 1.0e-3f) {
    r.u = 0.5f + 0.5f * x / (tan_x * z);
    r.v = 0.5f - 0.5f * y / (tan_y * z);
    r.ok = r.u >= 0 && r.u <= 1 && r.v >= 0 && r.v <= 1;
  }
  return r;
}
V3 ViewPos(float cx, float cy, float w, float h, float t, float tan_x, float tan_y) {
  const float u = cx / w, v = cy / h;
  return {(u * 2.0f - 1.0f) * tan_x * t, (1.0f - v * 2.0f) * tan_y * t, t};
}

void CheckMotion() {
  const float tan_x = 0.85f, tan_y = 0.5f;
  const int w = 1280, h = 720;
  const Pose before = MakePose({100, 2, -400}, 0.30, 0.05);
  const Pose now = MakePose({100.6, 2.05, -401.2}, 0.34, 0.04);
  float rows[12];
  MotionRows(now, before, rows);
  bool all_ok = true;
  double worst_uv = 0.0, worst_z = 0.0;
  Rng rng;
  for (int i = 0; i < 500; ++i) {
    // a texel of the current picture and the surface it shows at t units, with this frame's jitter
    const int tx = int(rng.Next() % w), ty = int(rng.Next() % h);
    const float jx = rng.Range(-0.5f, 0.5f), jy = rng.Range(-0.5f, 0.5f);
    const float t = rng.Range(6.0f, 300.0f);
    const float px = float(tx) + 0.5f - jx, py = float(ty) + 0.5f - jy;   // the unjittered place of what the jittered texel shows
    const V3 view_now = ViewPos(px, py, float(w), float(h), t, tan_x, tan_y);
    const V3 world = FromView(now, view_now);
    // the truth: the same point seen by the previous camera
    const V3 view_before = ToView(before, world);
    if (view_before.z < 1.0) continue;
    const double u_true = 0.5 + 0.5 * view_before.x / (tan_x * view_before.z);
    const double v_true = 0.5 - 0.5 * view_before.y / (tan_y * view_before.z);
    // the shader's: the 3x4 applied to the unjittered view point, then the motion texel
    const Prev prev = ToPrevious(rows, view_now, tan_x, tan_y);
    const hs::float4 m = hs::RgMotionTexel(hs::float2(px / float(w), py / float(h)), prev.u, prev.v, prev.z, t, prev.z > 1.0e-3f, -1.0f);
    const double du = (u_true - px / double(w)), dv = (v_true - py / double(h));
    worst_uv = std::max(worst_uv, std::max(std::fabs(m.x - du), std::fabs(m.y - dv)));
    // the depth delta: previous minus current of the signed linear depths (-z_before - (-t) is wrong: both are negative ahead, so -z_before - (-t) = t - z_before; the AMD form is prev - cur)
    const double dz_true = (-view_before.z) - (-double(t));
    worst_z = std::max(worst_z, std::fabs(m.z - dz_true));
    all_ok = all_ok && std::fabs(m.w) == 0.0f;
  }
  Check(worst_uv < 2e-5, "the motion texel is PreviousUV - CurrentUV of the unjittered pictures (worst " + std::to_string(worst_uv) + ")");
  Check(worst_z < 2e-2, "the depth delta is the previous signed linear depth minus this frame's (worst " + std::to_string(worst_z) + ")");
  Check(all_ok, "the motion texel's alpha is 0");
  // a camera that moves forward by one unit makes a still point nearer: previous - current = -1 in the negative-ahead convention
  const Pose a = MakePose({0, 0, 0}, 0.0, 0.0);
  const Pose bnow = MakePose({0, 0, -1}, 0.0, 0.0);   // forward is -z here: one unit further along it
  float fwd_rows[12];
  MotionRows(bnow, a, fwd_rows);
  const V3 point_view{0, 0, 20.0};   // 20 ahead of the current camera: it was 21 ahead of the previous one
  const Prev p1 = ToPrevious(fwd_rows, point_view, tan_x, tan_y);
  const hs::float4 mv = hs::RgMotionTexel(hs::float2(0.5f, 0.5f), p1.u, p1.v, p1.z, 20.0f, true, -1.0f);
  Check(Near(p1.z, 21.0, 1e-4) && Near(mv.z, -1.0, 1e-4) && Near(mv.x, 0.0, 1e-6) && Near(mv.y, 0.0, 1e-6), "forward by 1: the point was 21 ahead and is 20, the delta is -1 (signed: -21 - (-20)), no 2D motion");
  const hs::float4 mirrored = hs::RgMotionTexel(hs::float2(0.5f, 0.5f), p1.u, p1.v, p1.z, 20.0f, true, 1.0f);
  Check(Near(mirrored.z, 1.0, 1e-4), "the mirrored convention flips the delta (+21 - (+20))");
  // a point the previous camera has no place for (behind it) gives no vector
  const hs::float4 behind = hs::RgMotionTexel(hs::float2(0.5f, 0.5f), 0.3f, 0.3f, -5.0f, 20.0f, false, -1.0f);
  Check(behind.x == 0 && behind.y == 0 && behind.z == 0, "a point behind the last camera has no vector (the history resets there)");
  // a camera that turns right moves a still point left in the picture: PreviousUV - CurrentUV is positive for a point the previous frame saw to the right of where it is now? (previous = more to the left)
  const Pose p0 = MakePose({0, 0, 0}, 0.00, 0.0);
  const Pose p1b = MakePose({0, 0, 0}, 0.05, 0.0);   // yawed to the right (forward rotates toward +x)
  float turn_rows[12];
  MotionRows(p1b, p0, turn_rows);
  const Prev pt = ToPrevious(turn_rows, ViewPos(640, 360, 1280, 720, 30.0f, tan_x, tan_y), tan_x, tan_y);
  Check(pt.u > 0.5f, "the camera has turned right since the last frame: a point in the middle now was right of the middle then (previous u above 0.5), so its vector is positive in x");
}

// --- THE CAMERA ---------------------------------------------------------------------------------------------------------------------------------------

void CheckCamera() {
  const Pose pose = MakePose({2903, 12, -418}, 0.7, -0.1);
  gd::CameraInputs in;
  for (int i = 0; i < 3; ++i) {
    const V3 axes[3] = {pose.right, pose.up, pose.forward};
    in.right[i] = float((&axes[0].x)[i]);
    in.up[i] = float((&axes[1].x)[i]);
    in.forward[i] = float((&axes[2].x)[i]);
  }
  in.tan_half_fov[0] = 0.85f;
  in.tan_half_fov[1] = 0.48f;
  // The game's form: depth 0 at the near plane (view z -5) and 1 at the far plane (10,000), clip w = -z: P22 = f / (n - f), P23 = -1, P32 = n f / (n - f), P33 = 0.
  in.lin[0] = float(10000.0 / (5.0 - 10000.0));
  in.lin[1] = -1.0f;
  in.lin[2] = float(5.0 * 10000.0 / (5.0 - 10000.0));
  in.lin[3] = 0.0f;
  in.near_plane = 5.0f;
  in.far_plane = 10000.0f;
  const gd::RegenCamera cam = gd::BuildCamera(in, false);
  // a point 10 units ahead of the camera, a little to the right and up: its place relative to the camera (the view has no translation)
  const V3 offset{pose.forward.x * 10.0 + pose.right.x * 1.5 + pose.up.x * 0.8, pose.forward.y * 10.0 + pose.right.y * 1.5 + pose.up.y * 0.8, pose.forward.z * 10.0 + pose.right.z * 1.5 + pose.up.z * 0.8};
  double v[4] = {0, 0, 0, 0};
  const double row[4] = {offset.x, offset.y, offset.z, 1.0};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) v[c] += row[r] * double(cam.view[r * 4 + c]);
  }
  Check(Near(v[0], 1.5, 1e-3) && Near(v[1], 0.8, 1e-3) && Near(v[2], -10.0, 1e-3), "the view matrix puts a point 10 ahead (1.5 right, 0.8 up) at view (1.5, 0.8, -10): the game's own view space (rotated camera at (2903, 12, -418))");
  double clip[4] = {0, 0, 0, 0};
  const double vv[4] = {v[0], v[1], v[2], 1.0};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) clip[c] += vv[r] * double(cam.projection[r * 4 + c]);
  }
  Check(Near(clip[3], 10.0, 1e-3) && clip[3] > 0, "the projection gives clip w +10 for that point (in front)");
  Check(Near(clip[0] / clip[3], 1.5 / (10.0 * 0.85), 1e-4) && Near(clip[1] / clip[3], 0.8 / (10.0 * 0.48), 1e-4), "the projection puts it where the lens says (x over the tangent, y over the tangent)");
  // the rules' signed depth of the point's NDC depth is the view z (RG-1's convention test, seen from this camera)
  const double p[4][4] = {{1.0 / 0.85, 0, 0, 0}, {0, 1.0 / 0.48, 0, 0}, {0, 0, in.lin[0], in.lin[1]}, {0, 0, in.lin[2], in.lin[3]}};
  Check(clip[2] / clip[3] > 0.0 && clip[2] / clip[3] < 1.0, "its depth is inside 0..1 (0 at the near plane)");
  Check(Near(rg::ViewZOfDepth(clip[2] / clip[3], p), -10.0, 1e-3), "the rules' view z of that point's depth is -10 (the game's convention)");
  // the mirrored convention: positive z, positive w, the same x and y
  const gd::RegenCamera mir = gd::BuildCamera(in, true);
  double vm[4] = {0, 0, 0, 0};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) vm[c] += row[r] * double(mir.view[r * 4 + c]);
  }
  double cm[4] = {0, 0, 0, 0};
  const double vmm[4] = {vm[0], vm[1], vm[2], 1.0};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) cm[c] += vmm[r] * double(mir.projection[r * 4 + c]);
  }
  Check(Near(vm[2], 10.0, 1e-3) && Near(cm[3], 10.0, 1e-3) && Near(vm[0], 1.5, 1e-3) && Near(vm[1], 0.8, 1e-3) && Near(cm[0] / cm[3], clip[0] / clip[3], 1e-4) && Near(cm[1] / cm[3], clip[1] / clip[3], 1e-4),
        "the mirrored pair: the same point is at view z +10 with clip w +10 and the same screen place");
  // the view's rotation is orthonormal and a rotation (determinant +1 in the game's, -1 in the mirror)
  const auto det3 = [](const float m[16]) {
    return double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) - double(m[1]) * (double(m[4]) * m[10] - double(m[6]) * m[8]) + double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
  };
  Check(Near(det3(cam.view), 1.0, 1e-4) && Near(det3(mir.view), -1.0, 1e-4), "the view's rotation has determinant +1, the mirror's -1");
  Check(cam.view[12] == 0 && cam.view[13] == 0 && cam.view[14] == 0 && cam.view[15] == 1.0f, "the view has no translation (the position delta travels separately)");
  auto placed = in;
  placed.position[0] = float(pose.pos.x);
  placed.position[1] = float(pose.pos.y);
  placed.position[2] = float(pose.pos.z);
  for (bool mirrored : {false, true}) {
    const auto translated = gd::BuildCamera(placed, mirrored);
    double world_point[3] = {pose.pos.x + offset.x, pose.pos.y + offset.y, pose.pos.z + offset.z};
    double got[3] = {translated.view[12], translated.view[13], translated.view[14]};
    for (int a = 0; a < 3; ++a) {
      for (int r = 0; r < 3; ++r) got[a] += world_point[r] * translated.view[r * 4 + a];
    }
    Check(Near(got[0], 1.5, 1e-3) && Near(got[1], 0.8, 1e-3) && Near(got[2], mirrored ? 10.0 : -10.0, 1e-3),
          "GI's translated view matches its signed depth in both conventions");
  }
  // the bounds
  Check(cam.bounds.min == 5.0f && Near(cam.bounds.max, 9990.0, 0.01), "the bounds are the near plane and 0.999 of the far plane");
  // the position delta of a motion made from two poses is previous minus current
  const Pose before = MakePose({2900.5, 11.8, -415.2}, 0.62, -0.08);
  gd::CameraInputs moving = in;
  MotionRows(pose, before, moving.motion);
  moving.have_motion = true;
  const gd::RegenCamera with_motion = gd::BuildCamera(moving, false);
  Check(Near(with_motion.position_delta[0], before.pos.x - pose.pos.x, 2e-3) && Near(with_motion.position_delta[1], before.pos.y - pose.pos.y, 2e-3) &&
            Near(with_motion.position_delta[2], before.pos.z - pose.pos.z, 2e-3),
        "the camera's position delta is previous minus current, in world space (from two poses with a turn between them)");
  Check(with_motion.have_previous && !cam.have_previous && cam.position_delta[0] == 0 && cam.position_delta[1] == 0 && cam.position_delta[2] == 0, "with no motion there is no previous frame and no delta");
  // a camera that only turned has no delta
  const Pose turned = MakePose(pose.pos, 0.8, -0.05);
  gd::CameraInputs spin = in;
  MotionRows(turned, pose, spin.motion);
  spin.have_motion = true;
  const gd::RegenCamera spun = gd::BuildCamera(spin, false);
  Check(Near(spun.position_delta[0], 0, 1e-3) && Near(spun.position_delta[1], 0, 1e-3) && Near(spun.position_delta[2], 0, 1e-3), "a camera that only turned has a position delta of 0");
  // the jitter at the trace's size
  Check(Near(gd::JitterAtSize(0.4f, 1280, 640), 0.2) && Near(gd::JitterAtSize(-0.5f, 1281, 641), -0.25, 5e-3) && gd::JitterAtSize(0.4f, 0, 0) == 0.0f, "the jitter is scaled to the trace's size (half), and 0 with no size");
}

// --- THE FRAME ----------------------------------------------------------------------------------------------------------------------------------------

void CheckFrame() {
  // the order of the reasons, restated, over every fact set
  int sets = 0;
  bool all = true;
  for (int bits = 0; bits < 64; ++bits) {
    for (uint32_t samples : {1u, 2u, 4u}) {
      gd::RegenFacts f;
      f.asked = (bits & 1) != 0;
      f.debug_view = (bits & 2) != 0;
      f.stub = (bits & 4) != 0;
      f.pipelines_ready = (bits & 8) != 0;
      f.images_ok = (bits & 16) != 0;
      f.host_samples = samples;
      gd::RegenWhy want = gd::RegenWhy::kRegen;
      if (!f.asked) want = gd::RegenWhy::kNotAsked;
      else if (f.debug_view) want = gd::RegenWhy::kDebugView;
      else if (f.stub) want = gd::RegenWhy::kStub;
      else if (samples != 1) want = gd::RegenWhy::kMultisampled;
      else if (!f.pipelines_ready) want = gd::RegenWhy::kPipelines;
      else if (!f.images_ok) want = gd::RegenWhy::kImages;
      all = all && gd::DecideRegen(f) == want;
      all = all && gd::RegenPossible(f) == (f.asked && !f.debug_view && !f.stub && samples == 1);
      ++sets;
    }
  }
  Check(all, "DecideRegen follows its order over all " + std::to_string(sets) + " fact sets, and RegenPossible is everything but the pipelines and the images");
  gd::RegenFacts ok;
  ok.asked = true;
  ok.pipelines_ready = true;
  Check(gd::DecideRegen(ok) == gd::RegenWhy::kRegen, "a frame that is asked, plain and ready is Ray Regeneration's");
  Check(gd::OutcomeOf(true) == gd::Outcome::kDenoised && gd::OutcomeOf(false) == gd::Outcome::kOwnFilter, "a refused dispatch ends the frame on the effect's own filter, never on the denoiser's image");
  for (int w = 0; w < int(gd::RegenWhy::kCount); ++w) {
    Check(std::string(gd::RegenWhyName(gd::RegenWhy(w))) != "?" && std::strlen(gd::RegenWhyName(gd::RegenWhy(w))) > 10, "every reason has a sentence");
  }
  // the images
  Check(gd::kImageCount == 6 && gd::kPassImageCount == 5, "six images, five written by the pass");
  Check(gd::BytesPerTexel() == 44, "44 bytes a texel (RGBA16F x 5 and R32F)");
  Check(gd::BytesOf(960, 540) == uint64_t(960) * 540 * 44, "the images at the 1080p trace's size");
  Check(gd::BytesOf(960, 540) < 24ull * 1024 * 1024, "under 24 MiB at 1080p (the context's own memory is on top of it: ray_regen_rules.h EstimateBytes)");
  // the dispatch plan accepts exactly these images
  rg::Inputs in;
  const uint32_t w = 960, h = 540;
  in.linear_depth = gd::PlanImage(gd::Image::kDepth, w, h);
  in.motion = gd::PlanImage(gd::Image::kMotion, w, h);
  in.normals = gd::PlanImage(gd::Image::kNormals, w, h);
  in.specular_albedo = gd::PlanImage(gd::Image::kAlbedo, w, h);
  in.signal_in[size_t(rg::Signal::kIndirectSpecular)] = gd::PlanImage(gd::Image::kSignal, w, h);
  in.signal_out[size_t(rg::Signal::kIndirectSpecular)] = gd::PlanImage(gd::Image::kDenoised, w, h);
  in.render_size[0] = w;
  in.render_size[1] = h;
  in.context_max_size[0] = w;
  in.context_max_size[1] = h;
  in.context_signals = rg::FlagOf(rg::Signal::kIndirectSpecular);
  in.dispatch_signals = in.context_signals;
  Check(rg::PlanDispatch(in).verdict == rg::Verdict::kOk, "the dispatch plan accepts the six images (no diffuse albedo: the signal is specular only)");
  rg::Inputs bad = in;
  bad.signal_in[size_t(rg::Signal::kIndirectSpecular)].width = w / 2;
  Check(rg::PlanDispatch(bad).verdict == rg::Verdict::kBadSize, "a signal of another size is refused");
  bad = in;
  bad.motion.format = rr::Format::kRg16Float;
  Check(rg::PlanDispatch(bad).verdict == rg::Verdict::kBadFormat, "a motion image of a format the runtime does not take (RG16F) is refused: the guide is RGBA16F");
  bad = in;
  bad.linear_depth.present = false;
  Check(rg::PlanDispatch(bad).verdict == rg::Verdict::kMissingInput, "a missing depth is refused");
}

// --- THE SOURCES --------------------------------------------------------------------------------------------------------------------------------------

std::string FormatOfUav(const std::string& hlsl, const std::string& var) {
  const size_t at = hlsl.find(var + " : register(");
  if (at == std::string::npos) return "";
  const size_t from = hlsl.rfind("vk::image_format(\"", at);
  if (from == std::string::npos || at - from > 120) return "";
  const size_t q0 = from + std::strlen("vk::image_format(\"");
  return hlsl.substr(q0, hlsl.find('"', q0) - q0);
}

void CheckSources(const std::string& dir) {
  const std::string cs = Read(dir, "hlsl/ray_regen_ssr_cs.hlsl");
  const std::string pass = Read(dir, "hlsl/ssr_pass.hlsli");
  const std::string cmake = Read(dir, "CMakeLists.txt");
  const std::string shaders = Read(dir, "host_shaders.h");
  Check(!cs.empty() && !pass.empty() && !cmake.empty() && !shaders.empty(), "the sources are readable from " + dir);
  if (cs.empty() || pass.empty()) return;
  // the push block
  Check(Has(pass, "struct SsrPush {\n  uint4 r[8];") && Has(pass, "#if SSR_REGEN_EXTRA\n  uint4 x[4];") && Has(pass, "#ifndef SSR_REGEN_EXTRA\n#define SSR_REGEN_EXTRA 0"),
        "the reflections' push block keeps its 8 x uint4 and gains 4 x uint4 only under SSR_REGEN_EXTRA (every other compilation is unchanged)");
  Check(sizeof(gd::Push) == sizeof(ssr::Push) + 16 * sizeof(uint32_t) && sizeof(ssr::Push) == 128, "the host's push is the reflections' 128 bytes and 64 more");
  const auto define_value = [&](const std::string& name) -> int {
    const size_t at = cs.find("#define " + name + " ");
    if (at == std::string::npos) return -1;
    return std::atoi(cs.c_str() + at + std::strlen("#define ") + name.size() + 1);
  };
  Check(define_value("RG_X_MOTION") == int(gd::push::kMotion) && define_value("RG_X_JITTER_X") == int(gd::push::kJitterX) && define_value("RG_X_JITTER_Y") == int(gd::push::kJitterY) &&
            define_value("RG_X_DEPTH_SIGN") == int(gd::push::kDepthSign) && define_value("RG_X_FLAGS") == int(gd::push::kFlags),
        "the shader's RG_X_* defines are the host's push words");
  Check(gd::push::kMotion + 12 == gd::push::kJitterX && gd::push::kJitterX + 1 == gd::push::kJitterY && gd::push::kJitterY + 1 <= gd::push::kDepthSign && gd::push::kFlags + 1 == gd::push::kExtraWords && gd::push::kFlagHaveMotion == 1,
        "the extra words tile the 16: 12 motion floats, the jitter, the sign, the flags");
  Check(Has(cs, "(SsrXW(RG_X_FLAGS) & 1u) != 0u"), "the shader reads the have-motion bit the host sets");
  // the five UAVs
  const char* vars[5] = {"g_signal", "g_depth", "g_motion", "g_normals", "g_specular"};
  const char* want_formats[5] = {"rgba16f", "r32f", "rgba16f", "rgba16f", "rgba16f"};
  const gd::Image order[5] = {gd::Image::kSignal, gd::Image::kDepth, gd::Image::kMotion, gd::Image::kNormals, gd::Image::kAlbedo};
  bool formats = true, spaces = true;
  for (int i = 0; i < 5; ++i) {
    const std::string f = FormatOfUav(cs, vars[i]);
    formats = formats && f == want_formats[i];
    const rr::Format spec = gd::SpecOf(order[i]).format;
    formats = formats && ((f == "r32f") == (spec == rr::Format::kR32Float)) && ((f == "rgba16f") == (spec == rr::Format::kRgba16Float));
    spaces = spaces && Has(cs, std::string(vars[i]) + " : register(u" + std::to_string(i) + ", space5)");
  }
  Check(formats, "the five UAVs' Vulkan formats are the ones SpecOf names (RGBA16F, R32F, RGBA16F x 3)");
  Check(spaces, "the five UAVs are u0 to u4 in space 5 (the output set after the texture sets and the sampler set)");
  Check(Has(cs, "#define SSR_REGEN_EXTRA 1\n#include \"ssr_pass.hlsli\"\n#include \"ray_regen_common.hlsli\"") && Has(cs, "[numthreads(8, 8, 1)]") && gd::Groups(960) == 120 && gd::Groups(961) == 121,
        "the shader takes the extra words, runs 8 x 8 threads, and the host's groups round up");
  Check(Has(cs, "RgSignalTexel(trace.xyz, SsrHitDistance(trace.w), true)") && Has(cs, "RgDepthTexel(true, z_block, depth_sign)") && Has(cs, "RgNormalsTexel(n_world, roughness, 0u)") &&
            Has(cs, "RgSpecularAlbedo(f0, ab)") && Has(cs, "RgMotionTexel(cur, previous.u, previous.v, previous.z, t_texel, previous.z > 1.0e-3f, depth_sign)"),
        "the shader writes each image through its function of the common file");
  Check(Has(cs, "const bool traced = trace.w != 0.0f && z_block < kSsrSky * 0.5f;"), "a texel is traced where the trace's alpha is not 0 (the trace's own class gate is the answer)");
  Check(Has(cs, "RgDepthTexel(false, 0.0f, depth_sign)") && Has(cs, "g_signal[pix] = float4(0.0f, 0.0f, 0.0f, 0.0f);"), "a texel that is not traced is passed through: all 0 and a depth outside the bounds");
  Check(Has(cs, "SsrPickInBlock(SsrLinearAt(t0), SsrLinearAt(t1), SsrLinearAt(t2), SsrLinearAt(t3), z_block)"), "the receiver is the one the trace picks (the pixel of the 2x2 block whose depth is the Hi-Z's)");
  Check(Has(cs, "1.0f - smoothness") && Has(cs, "SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET))"), "the roughness is the one the trace used: the wet road's smoothness");
  Check(Has(cs, "SsrEnvBrdf(roughness, nov)") && Has(cs, "GB_CLASS_METAL"), "the specular albedo is the resolve's Fresnel: the environment BRDF at the roughness and N.V, a metal's F0 its albedo");
  Check(Has(cs, "pos = float2(texel) + 0.5f - jitter"), "the motion starts from the texel's unjittered place (the jitter taken out)");
  // the registration of the shader
  Check(Has(cmake, "\nlarecomp_gpu_host_shader(ray_regen_ssr_cs cs_6_0)\n"), "the CMake line makes the shader (plain Shader Model 6.0: the model AMD needs is for its own runtime)");
  Check(Has(shaders, "#include \"ray_regen_ssr_cs.hlsl.dxil.h\"") && Has(shaders, "#include \"ray_regen_ssr_cs.hlsl.spirv.h\"") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(ray_regen_ssr_cs)"),
        "host_shaders.h includes both blobs and hands the shader out");
  Check(Has(cmake, "ray_regen_guides.cpp"), "the pass's source is in the library");
  Check(Has(cmake, "larecomp_ray_regen_guides_test"), "the test is a target");
}

// The wiring: where the reflections, the scene, the temporal system, the menu and the settings hold the parts together (text checks, as the reflections' own test does).
void CheckBounce(const std::string& dir) {
  constexpr gd::BounceHistoryKey prev{18, 480, 270, 1919, 1079, 4};
  gd::BounceHistoryKey now{19, 480, 270, 1919, 1079, 4};
  Check(gd::KeepBounceHistory(prev, now, false), "bounce history accepts consecutive dispatches of the same footprint");
  Check(!gd::KeepBounceHistory(prev, now, true), "camera cut or relight resets bounce history");
  Check(!gd::KeepBounceHistory({}, now, false), "first bounce dispatch resets history");
  Check(!gd::KeepBounceHistory({18, 480, 270, 1919, 1079, 0}, {19, 480, 270, 1919, 1079, 0}, false),
        "an invalid footprint can never carry history");
  for (auto member : {&gd::BounceHistoryKey::width, &gd::BounceHistoryKey::height, &gd::BounceHistoryKey::render_width, &gd::BounceHistoryKey::render_height, &gd::BounceHistoryKey::step}) {
    auto changed = now;
    ++(changed.*member);
    Check(!gd::KeepBounceHistory(prev, changed, false), "changing any part of the footprint resets bounce history");
  }
  ++now.frame;
  Check(!gd::KeepBounceHistory(prev, now, false), "a missed bounce dispatch resets history");
  Check(sizeof(gd::BouncePush) == 128 && offsetof(gd::BouncePush, depth_sign) == 44 && offsetof(gd::BouncePush, motion) == 64 &&
            offsetof(gd::BouncePush, jitter) == 112 && offsetof(gd::BouncePush, have_motion) == 120, "GI guide push matches the HLSL packing");

  rr::Image signal = gd::PlanImage(gd::Image::kSignal, 480, 270);
  const auto diffuse = rg::Signal::kIndirectDiffuse;
  rg::Inputs in;
  in.render_size[0] = 480;
  in.render_size[1] = 270;
  in.context_max_size[0] = 960;
  in.context_max_size[1] = 540;
  in.dispatch_signals = in.context_signals = rg::FlagOf(diffuse);
  in.linear_depth = gd::PlanImage(gd::Image::kDepth, 480, 270);
  in.motion = gd::PlanImage(gd::Image::kMotion, 480, 270);
  in.normals = gd::PlanImage(gd::Image::kNormals, 480, 270);
  in.diffuse_albedo = gd::PlanImage(gd::Image::kAlbedo, 480, 270);
  in.signal_in[size_t(diffuse)] = in.signal_out[size_t(diffuse)] = signal;
  Check(rg::PlanDispatch(in).verdict == rg::Verdict::kOk, "bounce guides and RGBA irradiance/hit signal meet the diffuse-only context's contract");
  in.diffuse_albedo = {};
  Check(rg::PlanDispatch(in).verdict == rg::Verdict::kMissingInput, "indirect diffuse requires its albedo even with a valid radiance signal");

  const std::string shader = Read(dir, "hlsl/ray_regen_bounce_cs.hlsl");
  const std::string gi = Read(dir, "gi_pixel.cpp");
  const std::string apply = Read(dir, "gi_apply.cpp");
  const std::string runtime = Read(dir, "ray_regen.cpp");
  const std::string ssr = Read(dir, "ssr.cpp");
  Check(Has(shader, "traced = hit.y > 0.0f && t > 0.0f && t < kRgNoDepth") &&
            Has(shader, "g_depth[id.xy] = RgDepthTexel(traced, t, g_push.depth_sign)") && Has(shader, "g_signal[id.xy] = 0.0f"),
        "untraced bounce texels carry a zero signal and an out-of-range signed depth");
  Check(Has(shader, "Load(tap).xyz, hit.x, true") && Has(shader, "cls == GB_CLASS_METAL ? float3(0.0f, 0.0f, 0.0f) : albedo"),
        "irradiance carries hit distance in alpha, with diffuse albedo zero on metal");
  Check(Has(shader, "min(id.xy * g_push.render_step.z, render - 1u)") && Has(shader, "float2(pixel) + 0.5f - g_push.jitter") &&
            Has(shader, "RgMotionTexel(uv, prev_uv.x, prev_uv.y, prev.z, t, true, g_push.depth_sign)"),
        "the guide uses the trace footprint and unjittered UV/depth motion");
  Check(FormatOfUav(shader, "g_diffuse") == "rgba16f" && Has(shader, "g_diffuse : register(u4, space5)") &&
            Has(Read(dir, "CMakeLists.txt"), "larecomp_gpu_host_shader(ray_regen_bounce_cs cs_6_0)"), "GI binds the diffuse guide at the shared albedo slot and registers the shader");
  Check(Has(gi, "if (in_.regen && !plan_.raw && !in_.multisampled)") && Has(apply, "regen_jitter && view_ == 0 && own_pose && gb.Has(gbuffer::kPlaneMaterial)") &&
            Has(gi, "RecordBounce(list, push, signal_, depth_[parity_], hit_)"),
        "AMD GI requires a plain play frame with material and camera guides and consumes the current trace");
  Check(Has(gi, "frame.diffuse_albedo = regen_pass_.Tagged(rgd::Image::kAlbedo);") &&
            Has(gi, "rayregen::Instance().Dispatch(list, frame, site);") && Has(gi, "if (!dispatched) return nullptr;"),
        "GI dispatch sends the diffuse guide and only returns the vendor image on success");
  Check(Has(gi, "if (regen_ok) {\n    params_.raw = true;") && Has(gi, "} else if (plan_.denoise) {") &&
            Has(gi, "have_prev_ = rules::KeepsHistory(plan_.raw) && !regen_ok;"),
        "the existing depth-mask compose reads successful AMD frames and the local filter keeps its fallback");
  Check(Has(runtime, "ContextSlot& slot = i.sites[size_t(site)];") && Has(runtime, "context->clock.Next(frame.game_frame, frame.force_reset)") &&
            Has(runtime, "!stop && next->generation == generation"), "contexts, clocks and worker completions are site-local");
  Check(Has(ssr, "regen && rayregen::ReflectionSignalWanted(ask.asked, ask.city, rt_recorded)") &&
            Has(ssr, "if (regen_city_ != rt_recorded) regen_key_.valid = false;") &&
            ssr.find("rt_recorded = rt_.Record(rt_frame)") < ssr.find("regen_ok = RecordRegen(list, push, sf,"),
        "the reflection dispatch reads the shared trace after the city had a chance to write it");
}

void CheckWiring(const std::string& dir) {
  const std::string ssr_cpp = Read(dir, "ssr.cpp");
  const std::string ssr_h = Read(dir, "ssr.h");
  const std::string scene = Read(dir, "scene.cpp");
  const std::string temporal = Read(dir, "temporal.cpp");
  const std::string temporal_h = Read(dir, "temporal.h");
  const std::string renderer = Read(dir, "renderer.cpp");
  const std::string runtime = Read(dir, "ray_regen.cpp");
  const std::string runtime_h = Read(dir, "ray_regen.h");
  const std::string plan = Read(dir, "ray_regen_plan.cpp");
  const std::string menu = Read(dir, "../mc_engine/menus/upscaling_menu.cpp");
  const std::string manifest = Read(dir, "../settings/generated/manifest.json");
  const std::string header = Read(dir, "../settings/generated/settings_gpu.h");
  const std::string meta = Read(dir, "../settings/generated/settings_meta.inc");
  Check(!ssr_cpp.empty() && !scene.empty() && !temporal.empty() && !menu.empty() && !manifest.empty() && !plan.empty() && !runtime.empty(), "the wired sources are readable");
  // the reflections' frame: the decision, the dispatch, the fallback, the resolve
  const size_t trace_at = ssr_cpp.find("DrawPass(list, trace_, trace_pipeline, trace_push, classify);");
  const size_t rt_at = ssr_cpp.find("rt_recorded = rt_.Record(rt_frame);");
  const size_t regen_at = ssr_cpp.find("regen_ok = RecordRegen(list, push, sf,");
  const size_t denoise_at = ssr_cpp.find("filtered_v2 = denoise_.RecordV2(stage);");
  const size_t temporal_at = ssr_cpp.find("pass_timing_->Mark(list, \"ssr temporal\");");
  Check(temporal_at != std::string::npos && trace_at < rt_at && rt_at < regen_at && regen_at < denoise_at && denoise_at < temporal_at &&
            Has(ssr_cpp, "DrawTraceMrt(list, capture_pipeline ? capture_pipeline : trace_rt_pipeline, trace_push, true, capture_pipeline != nullptr);"),
        "the surface and RT traces precede Ray Regeneration, which precedes both internal denoisers");
  Check(Has(ssr_cpp, "regen_facts.asked = rayregen::ReflectionSignalWanted(ask.asked, ask.city, rt) && !raw && !garage;") &&
            Has(ssr_cpp, "if (rgd::RegenPossible(regen_facts)) {\n    regen_apply_pipeline = Pipeline(kVApplyRaw);\n    regen_facts.pipelines_ready = regen_pass_.Available() && regen_pass_.PipelineReady() && regen_apply_pipeline != nullptr;") &&
            Has(ssr_cpp, "const bool regen = regen_why == rgd::RegenWhy::kRegen;") && Has(ssr_cpp, "rgd::DecideRegen(regen_facts)") &&
            Has(ssr_cpp, "if (regen && rayregen::ReflectionSignalWanted(ask.asked, ask.city, rt_recorded)) {"),
        "the decision excludes raw and garage frames, gates pipeline requests and requires a recorded trace for a city-only request");
  Check(Has(ssr_cpp, "if (!raw && !regen_ok && rt_recorded && denoise_setting != 0 && RtDenoise::V2Enabled()) {") &&
            Has(ssr_cpp, "if (!raw && !regen_ok && !filtered_v2) {\n    // The accumulation.") &&
            Has(ssr_cpp, "if (!regen_ok) {\n      ++regen_fallbacks_;") &&
            Has(ssr_cpp, "const HostImage* resolve_source = filtered_v2 ? filtered_v2 : &accum_[write_];"),
        "a refused dispatch is counted, then the internal v2 filter or the legacy accumulation supplies the resolve");
  Check(Has(ssr_cpp, "apply.Set(pk::kSrc, ssr::PackSlots(denoised_slot, specular_ao ? trace_.slot : denoised_slot));") && Has(ssr_cpp, "regen_pass_.image(rgd::Image::kDenoised).slot") && Has(ssr_cpp, "(regen_ok ? regen_apply_pipeline : apply_pipeline)"),
        "the resolve reads the DENOISED image only after a dispatch that was recorded (the signed trace supplies specular occlusion)");
  Check(Has(ssr_cpp, "regen_key_ = now;") && Has(ssr_cpp, "regen_key_.valid = false;   // the next regen frame starts the runtime's history again") && Has(ssr_cpp, "ssr::HistoryUsable(regen_key_, now, motion_usable)"),
        "the runtime's history is kept only across consecutive regen frames of one size with a known camera motion; anything else resets it");
  Check(Has(ssr_cpp, "const bool dispatched = rayregen::Instance().Dispatch(list, frame);") && Has(ssr_cpp, "if (!dispatched) {\n    return false;") && Has(ssr_cpp, "regen_pass_.ReadDenoised(list);") && Has(ssr_cpp, "regen_pass_.PrepareDenoised(list);"),
        "the output is made writable before the dispatch and readable after it, and a refused dispatch is reported as no regen");
  Check(Has(ssr_cpp, "frame.motion_in_pixels = false;") && Has(ssr_cpp, "frame.shared_linear_albedo = true;") && Has(ssr_cpp, "rgd::JitterAtSize(ask.jitter[0], sf.render[0], width)") && Has(ssr_cpp, "rgd::BuildCamera(camera)"),
        "the dispatch hands over UV motion, linear albedo, the jitter at the trace's size and the camera the rules build");
  Check(Has(ssr_cpp, "rayregen::NoteTraced(rayregen::Site::kReflections, width, height);") && Has(ssr_cpp, "regen_pass_.Initialize(device, *frames, heap, passes, pass_timing);") && Has(ssr_cpp, "regen_pass_.Shutdown();") && Has(ssr_cpp, "uint64_t bytes = regen_pass_.HeldBytes();"),
        "the pass is made with the effect, shut down with it, counted in its held bytes, and the size the context is made for is reported");
  Check(Has(ssr_h, "const RegenAsk& regen = RegenAsk{}") && Has(ssr_h, "RayRegenRadiancePass regen_pass_;") && Has(ssr_h, "FrameRing* frames = nullptr"), "the header: the ask, the pass, the frames that retire its sets");
  // the scene, the temporal system, the renderer
  Check(Has(scene, "temporal_->RegenModes()") && Has(scene, "temporal_->FrameJitter(regen_ask.jitter);") && Has(scene, "ssr_->OnTrigger(summary, list, raw_ssr, UpscalerRunning(), regen_ask)"), "the scene asks the temporal system and hands the effect the ask");
  Check(Has(temporal, "denoiser::Publish(snapshot);\n  // RG-3") && Has(temporal, "regen_.Update(regen_in);") && Has(temporal, "regen_in.dlss_plan = rr_plan_;") && Has(temporal, "rayregen::NoteDevice(uint32_t(device.description().vendor), ActiveBackend() == Backend::kD3D12, device.description().name, WindowsBuildNumber());") &&
            Has(temporal, "rayregen::StopAll();") && Has(temporal_h, "rayregen::Planner regen_;") && Has(temporal_h, "rayregen::SignalModes RegenModes();"),
        "the plan is made at the frame boundary beside DLSS RR's, the vendor is noted at the start, the runtime stops with the system");
  Check(Has(renderer, "ssr_.Initialize(device_, heap_, passes_, edram_, pass_timing_, &frames_);") && Has(renderer, "rayregen::Current().context_bytes != 0 ? \", AMD FSR Ray Regeneration's context \""),
        "the renderer makes the pass with the effect and the existing VRAM line reports the runtime's context");
  Check(Has(runtime, "frame.motion_in_pixels ? MotionScaleFor(frame.render_size[0], frame.render_size[1]) : MotionScale{}") && Has(runtime, "p.vendor_id = in.vendor_id;") && Has(runtime, "p.context_signals |= (*context)->signals;") &&
            Has(runtime, "void ProbeNow(Device& device, FrameRing& frames)") && Has(runtime_h, "#else  // no AMD denoiser header, or no Direct3D 12 backend: the same names, doing nothing"),
        "the runtime: UV motion has scale 1, the vendor and the signals are published, a row can open it live, and a build without it has the same names as no-ops");
  Check(Has(plan, "site.keeper.Update(site.active)") && Has(plan, "Instance().Request(0, 0, 0, Site(i));") && Has(plan, "ProbeNow(*in.device, *in.frames);") && Has(plan, "RowListed() && in.device && in.frames") && Has(plan, "SelectRayRegen(pi)") &&
            Has(plan, "SignalModeOf(f, rr::Consumer::kSsr, switch_ssr)") && Has(plan, "f.context_ready = ready(Site::kBounce);"),
        "the planner: the plan is the rules', the context is made while it is on and freed after the hold, the runtime is opened only where the Radeon row can be listed, the modes are the rules'");
  Check(Has(plan, "if (!RowListed()) return {};"), "a previously published AMD runtime verdict cannot enable a hidden denoiser");
  const std::string ui_rules = Read(dir, "../settings/ui_rules.toml");
  Check(Has(menu, "rg::GateOf(rg::MenuFacts(), rg::ReadDebugSwitch(), mcla::gpu::denoiser::ConsumersOn(), rg::ContextMaking())") &&
        Has(menu, "gpu::denoiser::RowFor(gpu::rayregen::RowListed(), gpu::denoiser::RtxAdapter())") &&
        Has(ui_rules, "\"AMD FSR RAY REGENERATION\"") && Has(ui_rules, "\" (D3D12 ONLY)\""),
        "the Radeon denoiser row uses the AMD gate and declares its label and API reason");
  // the settings (the three generated files)
  Check(Has(manifest, "\"mcla_gpu_ray_regen_ssr\": {") && Has(header, "i32, mcla_gpu_ray_regen_ssr, 1, \"Debug (A/B, AMD FSR Ray Regeneration") && Has(header, "inline auto& mcla_gpu_ray_regen_ssr = g_gpu.mcla_gpu_ray_regen_ssr;") && Has(meta, "{\"mcla_gpu_ray_regen_ssr\", \"MCLA/GPU\"},"),
        "the A/B setting mcla_gpu_ray_regen_ssr is in the settings header, its alias, the manifest and the meta list");
  Check(Has(header, "AMD FSR Ray Regeneration (Radeon RX 9000 series cards, Direct3D 12)") && Has(header, "AMD FSR RAY REGENERATION row (a Radeon RX 9000 series card on Windows 11)"), "the settings' own descriptions name AMD FSR Ray Regeneration and the menu row");
  Check(Has(Read(dir, "CMakeLists.txt"), "ray_regen_plan.cpp"), "the planner is in the library");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = std::filesystem::path(__FILE__).parent_path().string();
  if (argc > 1) dir = argv[1];
  CheckTwin();
  CheckFormats();
  CheckMotion();
  CheckCamera();
  CheckFrame();
  CheckSources(dir);
  CheckBounce(dir);
  CheckWiring(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
