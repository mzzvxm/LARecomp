// Check of the probe field's apply (gi_apply_rules.h and hlsl/gi_apply_common.hlsli, hlsl/gi_apply_ps.hlsl): CITY-RT M3 package G3. A program of its own: it needs no renderer, no game files and no GPU,
// and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_apply_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_apply_test.cpp
//   run: larecomp_gi_apply_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check with its numbers)
//
// It checks: the IDENTITY (an empty field, a field of zeros, a field that is the open ground, a frame whose fade, strength or lit weight is 0, a pixel with no field in its cascade: the apply adds
// exactly nothing, so the picture is the game's bit for bit); that the bounce never darkens and never fills a pixel past the ceiling; that it is SSGI's own maths at the same strength where
// the ceiling does not bind (the energy parity, C8 in its structural part); the vehicle share, the lit weight (source rejection) and the cap; the SSGI switch (the share the colour chain runs
// with, the field's share, the two never adding up to more than one); the upsample's weights; the buffer's rows; the leak test's number on a synthetic wall; and then it compiles the .hlsli
// ITSELF as C++ (through a shim of the HLSL types and the field's three resources) and compares each of its functions with the twin on random inputs, and compares every #define of the
// .hlsli with the C++ value, the camera's two functions with local_lights_rules.h's, and reads the shader's registers, the push block, the CMake lines and the settings.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gi_apply_rules.h"
#include "gi_field_rules.h"
#include "gi_frame.h"
#include "local_lights_rules.h"
#include "ssgi_light.h"

namespace gi = mcla::gpu::gi;
namespace ga = mcla::gpu::gi_apply;
namespace sl = mcla::gpu::ssgi_light;
namespace ll = larecomp::gpu::local_lights;

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
    std::printf("ok: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
bool Near(double a, double b, double abs_tol, double rel_tol = 0.0) { return std::fabs(a - b) <= abs_tol + rel_tol * std::max(std::fabs(a), std::fabs(b)); }
bool NearV(const gi::Vec3& a, const gi::Vec3& b, double abs_tol, double rel_tol = 0.0) { return Near(a.x, b.x, abs_tol, rel_tol) && Near(a.y, b.y, abs_tol, rel_tol) && Near(a.z, b.z, abs_tol, rel_tol); }

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  gi::Vec3 Direction() {
    for (;;) {
      const gi::Vec3 v = {Range(-1, 1), Range(-1, 1), Range(-1, 1)};
      const float l2 = gi::Dot(v, v);
      if (l2 > 0.01f && l2 <= 1.0f) {
        const float il = 1.0f / std::sqrt(l2);
        return {v.x * il, v.y * il, v.z * il};
      }
    }
  }
};

// ---------------------------------------------------------------------------
// the field on the CPU: a Source for gi_frame.h's lookup and the resources the .hlsli shim reads (the same as gi_frame_test.cpp's, shorter)
// ---------------------------------------------------------------------------
struct Atlas {
  uint32_t w = 0, h = 0, ww = 0, wh = 0;
  std::vector<float> irr, mom;
  std::vector<uint8_t> weight;
  void Allocate(uint32_t atlas_w, uint32_t atlas_h, uint32_t weight_w, uint32_t weight_h) {
    w = atlas_w;
    h = atlas_h;
    ww = weight_w;
    wh = weight_h;
    irr.assign(size_t(w) * h * 4, 0.0f);
    mom.assign(size_t(w) * h * 2, 0.0f);
    weight.assign(size_t(ww) * wh, 0);
  }
  template <int N>
  void Bilinear(const std::vector<float>& data, float u, float v, float* out) const {
    const float x = u * float(w) - 0.5f, y = v * float(h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const auto clampi = [](int i, int n) { return std::min(std::max(i, 0), n - 1); };
    const int x0 = clampi(int(fx), int(w)), x1 = clampi(int(fx) + 1, int(w));
    const int y0 = clampi(int(fy), int(h)), y1 = clampi(int(fy) + 1, int(h));
    for (int c = 0; c < N; ++c) {
      const float a = data[(size_t(y0) * w + x0) * N + c], b = data[(size_t(y0) * w + x1) * N + c];
      const float d = data[(size_t(y1) * w + x0) * N + c], e = data[(size_t(y1) * w + x1) * N + c];
      out[c] = (a + (b - a) * tx) + ((d + (e - d) * tx) - (a + (b - a) * tx)) * ty;
    }
  }
  gi::Rgba SampleIrradiance(float u, float v) const {
    float o[4];
    Bilinear<4>(irr, u, v, o);
    return {o[0], o[1], o[2], o[3]};
  }
  gi::Vec2 SampleMoments(float u, float v) const {
    float o[2];
    Bilinear<2>(mom, u, v, o);
    return {o[0], o[1]};
  }
  float LoadWeight(uint32_t x, uint32_t y) const { return x < ww && y < wh ? float(weight[size_t(y) * ww + x]) / 255.0f : 0.0f; }
};
const Atlas* g_atlas = nullptr;

namespace hlsl_shim {
typedef unsigned int uint;
struct uint4 {
  uint x = 0, y = 0, z = 0, w = 0;
  uint4() = default;
  uint4(uint a, uint b, uint c, uint d) : x(a), y(b), z(c), w(d) {}
};
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
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float exp(float v) { return std::exp(v); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float floor(float v) { return std::floor(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
inline int asint(uint v) { return std::bit_cast<int>(v); }

std::vector<uint4> g_rows;
inline float4 ShimIrradiance(float2 uv) {
  const gi::Rgba c = g_atlas->SampleIrradiance(uv.x, uv.y);
  return float4(c.r, c.g, c.b, c.a);
}
inline float2 ShimMoments(float2 uv) {
  const gi::Vec2 m = g_atlas->SampleMoments(uv.x, uv.y);
  return float2(m.x, m.y);
}
inline float ShimWeight(uint x, uint y) { return g_atlas->LoadWeight(x, y); }

#define GI_FRAME_ROW(i) (g_rows[(i)])
#define GI_SAMPLE_IRRADIANCE(uv) ShimIrradiance(uv)
#define GI_SAMPLE_MOMENTS(uv) ShimMoments(uv)
#define GI_LOAD_PROBE_WEIGHT(x, y) ShimWeight((x), (y))
#include "hlsl/gi_common.hlsli"
#include "hlsl/gi_apply_common.hlsli"
#undef GI_FRAME_ROW
#undef GI_SAMPLE_IRRADIANCE
#undef GI_SAMPLE_MOMENTS
#undef GI_LOAD_PROBE_WEIGHT
}  // namespace hlsl_shim

hlsl_shim::float3 H3(gi::Vec3 v) { return hlsl_shim::float3(v.x, v.y, v.z); }
gi::Vec3 C3(hlsl_shim::float3 v) { return {v.x, v.y, v.z}; }

// ---------------------------------------------------------------------------
// synthetic fields
// ---------------------------------------------------------------------------
struct Field {
  gi::GiFrame frame;
  Atlas atlas;
};

// A standard-layout field round `camera`: every probe on (weight 1), the irradiance of direction w given by `irr(w)` (rgb and sky), the moments those of an open scene (a far wall).
template <class F>
Field MakeField(gi::Layout layout, const double camera[3], F irr, float moment_distance = 1000.0f) {
  Field f;
  const gi::Budget& b = gi::BudgetOf(layout);
  f.frame = gi::MakeFrame(b, camera);
  f.frame.valid = gi::kFieldProducts;
  f.frame.slot = {1, 2, 3, gi::kNoSlot};
  f.frame.level = gi::Level::kHigh;
  f.frame.fade = 1.0f;
  f.frame.safe_range_m = 300.0f;
  f.frame.strength = 2.0f;
  f.frame.sun_rgb[0] = 1.2f;
  f.frame.sun_rgb[1] = 1.1f;
  f.frame.sun_rgb[2] = 0.9f;
  f.frame.sun_sin_elevation = 0.8f;
  f.frame.ambient_up_rgb[0] = 0.4f;
  f.frame.ambient_up_rgb[1] = 0.45f;
  f.frame.ambient_up_rgb[2] = 0.6f;
  f.frame.ambient_down_rgb[0] = 0.2f;
  f.frame.ambient_down_rgb[1] = 0.18f;
  f.frame.ambient_down_rgb[2] = 0.15f;
  f.atlas.Allocate(gi::AtlasWidth(b), gi::AtlasHeight(b), gi::WeightWidth(b), gi::WeightHeight(b));
  for (uint32_t c = 0; c < b.cascade_count; ++c) {
    const gi::Dims d = b.cascade[c].dims;
    for (uint32_t z = 0; z < d.z; ++z)
      for (uint32_t y = 0; y < d.y; ++y)
        for (uint32_t x = 0; x < d.x; ++x) {
          const gi::Texel tile = gi::TileOrigin({x, y, z}, d);
          const gi::Texel wt = gi::WeightTexel({x, y, z}, d);
          f.atlas.weight[size_t(gi::WeightRow(b, c) + wt.y) * f.atlas.ww + wt.x] = 255;
          for (uint32_t ty = 0; ty < gi::kProbeTile; ++ty)
            for (uint32_t tx = 0; tx < gi::kProbeTile; ++tx) {
              const gi::Texel src = gi::OctBorderSource(tx, ty);
              const gi::Vec3 dir = gi::TexelDirection(src.x, src.y);
              const gi::Rgba e = irr(dir);
              const size_t at = (size_t(gi::AtlasRow(b, c) + tile.y + ty) * f.atlas.w + (tile.x + tx));
              f.atlas.irr[at * 4 + 0] = e.r;
              f.atlas.irr[at * 4 + 1] = e.g;
              f.atlas.irr[at * 4 + 2] = e.b;
              f.atlas.irr[at * 4 + 3] = e.a;
              f.atlas.mom[at * 2 + 0] = moment_distance;
              f.atlas.mom[at * 2 + 1] = moment_distance * moment_distance;
            }
        }
  }
  return f;
}

ga::Frame MakeApplyFrame(const gi::GiFrame& f) {
  ga::Frame fr;
  fr.tan_x = 0.9f;
  fr.tan_y = 0.5f;
  fr.lin[0] = -1.0f;
  fr.lin[1] = 0.0f;
  fr.lin[2] = 0.1f;
  fr.lin[3] = 0.0f;
  fr.render[0] = 3840.0f;
  fr.render[1] = 2160.0f;
  fr.traced[0] = 1920.0f;
  fr.traced[1] = 1080.0f;
  fr.sun_dir[0] = 0.3f;
  fr.sun_dir[1] = 0.8f;
  fr.sun_dir[2] = 0.52f;
  const float il = 1.0f / std::sqrt(0.09f + 0.64f + 0.2704f);
  fr.sun_dir[0] *= il;
  fr.sun_dir[1] *= il;
  fr.sun_dir[2] *= il;
  fr.ambient = 0.45f;
  fr.sun = 1.15f;
  fr.cap = 2.0f;
  fr.field_share = 1.0f;
  fr.strength = f.strength;
  fr.ao_strength = 1.0f;
  fr.ao_fade_end = 100.0f;
  fr.paint_power = 0.3125f;
  return fr;
}

// ---------------------------------------------------------------------------
// the identity
// ---------------------------------------------------------------------------
gi::Vec3 AddAt(const gi::GiFrame& f, const Atlas& atlas, const ga::Frame& fr, gi::Vec3 p, gi::Vec3 n, gi::Vec3 albedo, float lit, float vehicle, float ao) {
  const gi::Sample s = gi::Lookup(f, atlas, p, n);
  ga::PixelIn in;
  in.albedo = albedo;
  in.lit = lit;
  in.e_net = ga::LookupNet(f, s, n, fr.dead_band);
  in.strength = fr.strength;
  in.field_share = fr.field_share;
  in.vehicle = vehicle;
  in.ao_term = ga::AoTerm(ao, vehicle, 20.0f, fr.ao_strength, fr.ao_fade_end, fr.paint_power);
  in.ambient = fr.ambient;
  in.sun = fr.sun;
  in.n_dot_l = std::max(0.0f, n.x * fr.sun_dir[0] + n.y * fr.sun_dir[1] + n.z * fr.sun_dir[2]);
  in.cap = fr.cap;
  return ga::BounceAdd(in);
}

void IdentityChecks() {
  Lcg rng;
  const double camera[3] = {100.0, 20.0, -50.0};
  const auto zero = [](gi::Vec3) { return gi::Rgba{0, 0, 0, 0.5f}; };
  Field empty = MakeField(gi::Layout::kStandard, camera, zero);
  std::fill(empty.atlas.weight.begin(), empty.atlas.weight.end(), uint8_t(0));  // no probe is valid: a field that is still being built, or a world with none
  Field zeros = MakeField(gi::Layout::kStandard, camera, zero);                 // every probe on, and nothing in them
  const gi::GiFrame& base = zeros.frame;
  Field ground = MakeField(gi::Layout::kStandard, camera, [&](gi::Vec3 w) {
    const gi::Vec3 e = gi::GroundBaseline(base, w);  // an open plane's irradiance in every direction
    return gi::Rgba{e.x, e.y, e.z, 1.0f - 0.5f * (1.0f - w.y)};
  });
  const ga::Frame fr = MakeApplyFrame(base);
  int pixels = 0, empty_nonzero = 0, zero_nonzero = 0, ground_nonzero = 0;
  double ground_peak = 0.0;
  for (int i = 0; i < 4000; ++i) {
    const gi::Vec3 p = {float(camera[0]) + rng.Range(-30, 30), float(camera[1]) + rng.Range(-14, 4), float(camera[2]) + rng.Range(-30, 30)};
    const gi::Vec3 n = rng.Direction();
    const gi::Vec3 albedo = {rng.Range(0.02f, 0.95f), rng.Range(0.02f, 0.95f), rng.Range(0.02f, 0.95f)};
    ++pixels;
    const gi::Vec3 a = AddAt(empty.frame, empty.atlas, fr, p, n, albedo, 1.0f, 0.0f, 1.0f);
    const gi::Vec3 b = AddAt(zeros.frame, zeros.atlas, fr, p, n, albedo, 1.0f, 0.0f, 1.0f);
    const gi::Vec3 c = AddAt(ground.frame, ground.atlas, fr, p, n, albedo, 1.0f, 0.0f, 1.0f);
    empty_nonzero += (a.x != 0.0f || a.y != 0.0f || a.z != 0.0f) ? 1 : 0;
    zero_nonzero += (b.x != 0.0f || b.y != 0.0f || b.z != 0.0f) ? 1 : 0;
    ground_nonzero += (c.x > 0.0f || c.y > 0.0f || c.z > 0.0f) ? 1 : 0;
    ground_peak = std::max(ground_peak, double(std::max(c.x, std::max(c.y, c.z))));
  }
  Check(empty_nonzero == 0, Num("IDENTITY: an empty field (no probe valid) adds exactly 0 to all %.0f pixels", pixels));
  Check(zero_nonzero == 0, "IDENTITY: a field of zeros adds exactly 0");
  // the octahedral map's bilinear error on the open ground's smooth function is up to 3.4% of its range (gi_frame.h C6): the dead band (2% of the ground's radiance) takes most of it; what is left is
  // a faint glow far under 1% of the pixel's colour
  Check(ground_peak < 0.01, Num("IDENTITY: a field that is the open ground adds at most %.5f anywhere (0.01 is under 1%% of a white pixel in the sun)", ground_peak));
  Check(ground_nonzero * 8 < pixels, Num("IDENTITY: ... and the open ground's rounding touches only %.1f%% of the pixels (the map's interpolation, under the band almost everywhere)", 100.0 * ground_nonzero / pixels));
  // the exact form: E_surf equal to E_base is a net of 0 whatever the direction
  bool net_zero = true;
  for (int i = 0; i < 2000; ++i) {
    const gi::Vec3 n = rng.Direction();
    const gi::Vec3 e = gi::GroundBaseline(base, n);
    const gi::Vec3 net = ga::NetIrradiance(base, e, n, fr.dead_band);
    net_zero = net_zero && net.x == 0.0f && net.y == 0.0f && net.z == 0.0f;
  }
  Check(net_zero, "IDENTITY: E_surf = E_base gives E_net = 0 in every direction, exactly");
  // the frame's own switches
  {
    ga::PixelIn in;
    in.albedo = {0.6f, 0.5f, 0.4f};
    in.e_net = {0.4f, 0.3f, 0.2f};
    in.ambient = fr.ambient;
    in.sun = fr.sun;
    in.n_dot_l = 0.2f;
    const gi::Vec3 on = ga::BounceAdd(in);
    Check(on.x > 0.0f && on.y > 0.0f && on.z > 0.0f, "a lit pixel with a net irradiance takes light");
    ga::PixelIn off = in;
    off.field_share = 0.0f;
    Check(ga::Luma(ga::BounceAdd(off)) == 0.0f, "IDENTITY: field share 0 (no field this frame, or the screen-space bounce light had the frame) adds exactly 0");
    off = in;
    off.strength = 0.0f;
    Check(ga::Luma(ga::BounceAdd(off)) == 0.0f, "IDENTITY: strength 0 adds exactly 0");
    off = in;
    off.lit = 0.0f;
    Check(ga::Luma(ga::BounceAdd(off)) == 0.0f, "IDENTITY (source rejection): an emissive surface (lit weight 0) takes nothing");
    off = in;
    off.e_net = {0, 0, 0};
    Check(ga::Luma(ga::BounceAdd(off)) == 0.0f, "IDENTITY: no net irradiance adds exactly 0");
    off = in;
    off.albedo = {0, 0, 0};
    Check(ga::Luma(ga::BounceAdd(off)) == 0.0f, "a pixel with no albedo (RT2 = 0) takes nothing");
  }
  // a frame with no field in it: the frame says no, and the SSGI share is 1 (the colour chain runs as it always did)
  {
    gi::GiFrame none;
    Check(!none.FieldPresent() && ga::FieldShare(none, 0.0f) == 0.0f && ga::SsgiShareFor(none) == 1.0f && !ga::SsgiSkipped(ga::SsgiShareFor(none)),
          "IDENTITY: no field in the frame: field share 0, the screen-space bounce light runs at its full strength, nothing is skipped");
  }
}

// ---------------------------------------------------------------------------
// the rules: never darkens, the ceiling, the cap, the vehicle share, the parity with SSGI
// ---------------------------------------------------------------------------
void RuleChecks() {
  Lcg rng;
  int negative = 0, over_ceiling = 0, over_cap = 0, nonfinite = 0;
  double worst_over = 0.0;
  for (int i = 0; i < 20000; ++i) {
    ga::PixelIn in;
    in.albedo = {rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f)};
    in.lit = rng.Range(0.0f, 1.0f);
    in.e_net = {rng.Range(0.0f, 6.0f), rng.Range(0.0f, 6.0f), rng.Range(0.0f, 6.0f)};
    in.strength = rng.Range(0.0f, 7.0f);
    in.field_share = rng.Range(0.0f, 1.0f);
    in.vehicle = rng.Range(0.0f, 1.0f);
    in.ao_term = rng.Range(0.0f, 1.0f);
    in.ambient = rng.Range(0.0f, 1.0f);
    in.sun = rng.Range(0.0f, 2.0f);
    in.n_dot_l = rng.Range(0.0f, 1.0f);
    in.cap = rng.Range(0.5f, 3.5f);
    const gi::Vec3 add = ga::BounceAdd(in);
    if (!std::isfinite(add.x) || !std::isfinite(add.y) || !std::isfinite(add.z)) ++nonfinite;
    if (add.x < 0.0f || add.y < 0.0f || add.z < 0.0f) ++negative;
    const float ceiling = sl::RadianceCap(in.cap, in.ambient, in.sun);
    const float room = std::max(ceiling - ga::Luma(in.albedo) * std::max(in.ambient + in.sun * in.n_dot_l, 1e-3f), 0.0f);
    const double over = double(ga::Luma(add)) - double(room);
    worst_over = std::max(worst_over, over);
    if (over > 1e-4) ++over_ceiling;
    if (ga::Luma(add) > ceiling * 1.0001f) ++over_cap;
  }
  Check(nonfinite == 0, "the bounce is always finite");
  Check(negative == 0, "NEVER DARKENS: the light added is never negative, in 20,000 random pixels");
  Check(over_ceiling == 0, Num("CEILING: the light added never goes over the room under the ceiling (worst excess %.2e)", worst_over));
  Check(over_cap == 0, "CEILING: and never over `cap` x the frame's light, so nothing gets whiter than the sun-lit white of the frame");

  // the parity with SSGI's own rule: with the pixel's colour = albedo x T, the source 0, no limit and a ceiling that does not bind, the pass adds exactly what SSGI's factor would
  int parity_bad = 0;
  double parity_worst = 0.0;
  for (int i = 0; i < 4000; ++i) {
    ga::PixelIn in;
    in.albedo = {rng.Range(0.05f, 0.9f), rng.Range(0.05f, 0.9f), rng.Range(0.05f, 0.9f)};
    in.lit = 1.0f;
    in.e_net = {rng.Range(0.0f, 0.3f), rng.Range(0.0f, 0.3f), rng.Range(0.0f, 0.3f)};
    in.strength = rng.Range(0.5f, 2.0f);
    in.field_share = 1.0f;
    in.vehicle = 0.0f;
    in.ao_term = 1.0f;
    in.ambient = rng.Range(0.3f, 0.6f);
    in.sun = rng.Range(0.8f, 1.5f);
    in.n_dot_l = rng.Range(0.0f, 1.0f);
    in.cap = 3.5f;  // the cap does not cut an irradiance of 0.3 (3.5 x 1.1 at least)
    const gi::Vec3 ours = ga::BounceAdd(in);
    sl::ApplyIn s;
    s.bounce_radiance = {in.e_net.x, in.e_net.y, in.e_net.z};
    const float t = std::max(in.ambient + in.sun * in.n_dot_l, 1e-3f);
    s.colour = {in.albedo.x * t, in.albedo.y * t, in.albedo.z * t};
    s.source = 0.0f;
    s.ambient = in.ambient;
    s.sun = in.sun;
    s.sun_term = in.sun * in.n_dot_l;
    s.ao_part = 1.0f;
    s.strength = in.strength;
    s.vehicle = 0.0f;
    s.fade = 1.0f;
    s.limit = sl::kLimitMax;  // the gain limit is the one rule this pass does not have: the biggest it can be
    s.cap = in.cap;
    const sl::Rgb factor = sl::BounceFactor(s);
    const gi::Vec3 theirs = {s.colour.r * factor.r, s.colour.g * factor.g, s.colour.b * factor.b};
    const double d = std::max({std::fabs(ours.x - theirs.x), std::fabs(ours.y - theirs.y), std::fabs(ours.z - theirs.z)});
    parity_worst = std::max(parity_worst, d);
    if (d > 1e-5) ++parity_bad;
  }
  Check(parity_bad == 0, Num("ENERGY PARITY (C8, structural): at the same strength the pass adds albedo x B x strength, the number SSGI's factor times the colour comes to (worst difference %.2e)", parity_worst));

  // the vehicle share: a car's pixels take 0.35 of the strength, at most 0.8
  {
    ga::PixelIn in;
    in.albedo = {0.3f, 0.3f, 0.3f};
    in.e_net = {0.05f, 0.05f, 0.05f};
    in.ambient = 0.5f;
    in.sun = 1.0f;
    in.n_dot_l = 0.5f;
    in.cap = 3.5f;
    in.strength = 2.0f;
    in.vehicle = 1.0f;
    const gi::Vec3 car = ga::BounceAdd(in);
    in.vehicle = 0.0f;
    in.strength = 0.7f;  // 0.35 x 2
    const gi::Vec3 world = ga::BounceAdd(in);
    Check(NearV(car, world, 1e-7), "VEHICLE: a car's pixel at strength 2 takes what the world takes at 0.7 (kVehicleShare 0.35)");
    in.strength = 7.0f;
    in.vehicle = 1.0f;
    const gi::Vec3 strongest = ga::BounceAdd(in);
    in.strength = 0.8f;
    in.vehicle = 0.0f;
    Check(NearV(strongest, ga::BounceAdd(in), 1e-7), "VEHICLE: ... and never more than 0.8 whatever the level (the strongest)");
    in.vehicle = 0.5f;
    in.strength = 2.0f;
    const gi::Vec3 half = ga::BounceAdd(in);
    in.vehicle = 1.0f;
    const gi::Vec3 full = ga::BounceAdd(in);
    in.vehicle = 0.0f;
    const gi::Vec3 none = ga::BounceAdd(in);
    Check(half.x > full.x && half.x < none.x, "VEHICLE: a silhouette's pixel (half its neighbourhood a car) is between the two");
  }
  // the cap: the net irradiance's luma is softly clipped at cap x the frame's light, the hue stays
  {
    ga::PixelIn in;
    in.albedo = {0.5f, 0.5f, 0.5f};
    in.ambient = 0.05f;  // a night: the cap's floor, 0.5
    in.sun = 0.0f;
    in.n_dot_l = 0.0f;
    in.strength = 1.0f;
    in.cap = 2.0f;
    in.e_net = {40.0f, 20.0f, 10.0f};  // a lamp pool's irradiance far over the cap
    const gi::Vec3 e = ga::CapRadiance(in.e_net, sl::RadianceCap(in.cap, in.ambient, in.sun));
    Check(ga::Luma(e) <= sl::kCapFloor + 1e-5f && Near(e.x / e.y, 2.0, 1e-4) && Near(e.y / e.z, 2.0, 1e-4), "CAP: a lamp's irradiance is cut to the floor (0.5) at night and keeps its hue");
    const gi::Vec3 add = ga::BounceAdd(in);
    Check(ga::Luma(add) < sl::kCapFloor && ga::Luma(add) > 0.0f, "CAP: so the pool lights a facade with at most the cap's light, never whiter");
  }
  // the camera's functions against local_lights_rules.h's
  {
    float pose[16] = {0.8f, 0.0f, 0.6f, 0, 0.0f, 1.0f, 0.0f, 0, -0.6f, 0.0f, 0.8f, 0, 12.5f, 3.0f, -40.0f, 1};
    ga::Frame a;
    ll::Rows b;
    Check(ga::CameraFromViewInverse(pose, &a) && ll::RowsFromViewInverse(pose, &b), "the camera's axes are read from a view inverse");
    bool same = true;
    for (int c = 0; c < 3; ++c) same = same && a.right[c] == b.right[c] && a.up[c] == b.up[c] && a.forward[c] == b.forward[c] && a.position[c] == b.position[c];
    Check(same, "the camera's rows are local_lights_rules.h's (right, up, forward = -row 2, position)");
    float bad[16];
    std::memcpy(bad, pose, sizeof(bad));
    bad[0] = 2.0f;
    ga::Frame c;
    Check(!ga::CameraFromViewInverse(bad, &c) && !ll::RowsFromViewInverse(bad, &b), "a matrix that is not a camera's is refused by both");
    const float lin[4] = {-1.0f, 0.0f, 0.1f, 0.0f};
    float worst = 0.0f;
    Lcg r2;
    for (int i = 0; i < 1000; ++i) {
      const float stored = r2.Range(0.0f, 1.0f);
      worst = std::max(worst, std::fabs(ga::LinearDepth(stored, lin) - ll::LinearDepth(stored, lin)));
    }
    Check(worst == 0.0f && ga::LinearDepth(0.0f, lin) == ga::kSky, "the linear depth is the light pass's, and the sky is 60000");
    ga::Frame fr = MakeApplyFrame(gi::GiFrame{});
    std::memcpy(fr.right, a.right, sizeof(fr.right));
    std::memcpy(fr.up, a.up, sizeof(fr.up));
    std::memcpy(fr.forward, a.forward, sizeof(fr.forward));
    std::memcpy(fr.position, a.position, sizeof(fr.position));
    double worst_pos = 0.0;
    for (int i = 0; i < 1000; ++i) {
      const float cx = r2.Range(0.0f, 3840.0f), cy = r2.Range(0.0f, 2160.0f), t = r2.Range(1.0f, 900.0f);
      float out[3];
      ll::WorldPosition(b, fr.tan_x, fr.tan_y, cx, cy, 3840.0f, 2160.0f, t, out);
      const gi::Vec3 p = ga::WorldPosition(fr, cx, cy, 3840.0f, 2160.0f, t);
      worst_pos = std::max({worst_pos, double(std::fabs(p.x - out[0])), double(std::fabs(p.y - out[1])), double(std::fabs(p.z - out[2]))});
    }
    Check(worst_pos < 1e-3, Num("the world position is the light pass's (worst difference %.2e m)", worst_pos));
  }
}

// ---------------------------------------------------------------------------
// the AO's term, the upsample
// ---------------------------------------------------------------------------
void AoAndUpsampleChecks() {
  // the term: 1 with no occlusion strength, the visibility itself near with a strength of 1, fading to 1 at the fade's end
  Check(ga::AoTerm(0.3f, 0.0f, 10.0f, 0.0f, 100.0f, 0.3f) == 1.0f, "AO: with the occlusion's strength at 0 the term is 1 (nothing multiplies the bounce)");
  Check(Near(ga::AoTerm(0.3f, 0.0f, 10.0f, 1.0f, 100.0f, 0.3f), 0.3, 1e-6), "AO: near, with strength 1 the term is the visibility: a bounce into a crease is occluded");
  Check(ga::AoTerm(0.3f, 0.0f, 100.0f, 1.0f, 100.0f, 0.3f) == 1.0f && ga::AoTerm(0.3f, 0.0f, 300.0f, 1.0f, 100.0f, 0.3f) == 1.0f, "AO: at and past the fade's end the term is 1 (as the occlusion's own factor)");
  Check(ga::AoTerm(0.3f, 0.0f, 75.0f, 1.0f, 100.0f, 0.3f) > 0.3f && ga::AoTerm(0.3f, 0.0f, 75.0f, 1.0f, 100.0f, 0.3f) < 1.0f, "AO: in between the fade is smooth");
  Check(ga::AoTerm(0.5f, 1.0f, 10.0f, 1.0f, 100.0f, 0.3125f) > ga::AoTerm(0.5f, 0.0f, 10.0f, 1.0f, 100.0f, 0.3125f), "AO: a vehicle's pixel takes the paint's lighter visibility (ao to the paint power)");
  Check(ga::AoTerm(0.5f, 0.4f, 10.0f, 1.0f, 100.0f, 0.3125f) == ga::AoTerm(0.5f, 0.0f, 10.0f, 1.0f, 100.0f, 0.3125f), "AO: ... only where over half of the neighbourhood is a vehicle (the road beside a tyre keeps the contact line)");
  float prev = 0.0f;
  bool monotone = true;
  for (int i = 0; i <= 100; ++i) {
    const float v = ga::AoTerm(float(i) / 100.0f, 0.0f, 10.0f, 0.8f, 100.0f, 0.3125f);
    monotone = monotone && v >= prev;
    prev = v;
  }
  Check(monotone, "AO: the more visible, the less the term takes away");
  // the upsample: equal depth and normal give the bilinear weights back; a depth step or a turned normal drops a texel
  Check(ga::DepthWeight(10.0f, 10.0f) == 1.0f && ga::DepthWeight(10.0f, 10.1f) > 0.1f && ga::DepthWeight(10.0f, 14.0f) <= 2e-4f, "UPSAMPLE: a texel at the same depth counts, one 4 m deeper at 10 m does not (the occlusion's rule)");
  Check(ga::NormalWeight({0, 1, 0}, {0, 1, 0}) == 1.0f && ga::NormalWeight({0, 1, 0}, {1, 0, 0}) == 0.0f && ga::NormalWeight({0, 1, 0}, {0, -1, 0}) == 0.0f, "UPSAMPLE: a texel with the same normal counts, one turned a right angle or round does not");
  const gi::Vec3 tilted = {0.0f, 0.9689f, 0.2474f};
  Check(ga::NormalWeight({0, 1, 0}, tilted) > 0.8f, "UPSAMPLE: a gently curved surface keeps its neighbours");
}

void DaylightChecks() {
  gi::GiFrame f{};
  f.sun_rgb[0] = f.sun_rgb[1] = f.sun_rgb[2] = 1.0f;
  f.sun_sin_elevation = 1.0f;
  f.ground_albedo = 0.2f;
  const gi::Vec3 wall{1, 0, 0}, ground{0, 1, 0}, ceiling{0, -1, 0};
  ga::Frame fr{};
  fr.scene_units = 16.0f;
  fr.daylight_recovery = 0.0625f;
  const auto run = [&](gi::Sample s, gi::Vec3 n) {
    const ga::Buffer buffer = ga::MakeBuffer(gi::MakeGpuFrame(f), fr);
    hlsl_shim::g_rows.resize(gi::kGpuFrameRows + ga::kApplyRows);
    for (size_t i = 0; i < hlsl_shim::g_rows.size(); ++i) {
      hlsl_shim::g_rows[i] = {buffer[i * 4], buffer[i * 4 + 1], buffer[i * 4 + 2], buffer[i * 4 + 3]};
    }
    const hlsl_shim::GiSample hs{H3(s.irradiance), s.sky_visibility, s.confidence};
    const gi::Vec3 expected = ga::DaylightNet(f, s, n, fr);
    Check(NearV(C3(hlsl_shim::GiaLookupNet(hs, H3(n), hlsl_shim::GiaLoadFrame())), expected, 1e-6), "daylight lookup matches shader");
    return expected;
  };
  const gi::Sample shade{{0.084f, 0.044f, 0.024f}, 0.05f, 0.5f};
  Check(NearV(run(shade, wall), {0.040f, 0.020f, 0.010f}, 1e-6), "shaded wall recovers colour removed by the baseline and keeps confidence and dead band");
  gi::Sample s = shade;
  s.sky_visibility = 0.20f;
  Check(NearV(run(s, wall), {0.020f, 0.010f, 0.005f}, 1e-6), "sky deficit ramp has a half-weight midpoint");
  s.sky_visibility = 0.36f;
  Check(NearV(run(s, wall), {}, 0), "small sky deficit leaves the original wall residual");
  s.sky_visibility = 0.275f;
  Check(NearV(run(s, wall), {0.00625f, 0.003125f, 0.0015625f}, 1e-6), "sky deficit uses cubic smoothing away from the midpoint");
  s = shade;
  s.irradiance = {0.124f, 0.044f, 0.024f};
  Check(NearV(run(s, wall), {0.210f, 0.020f, 0.010f}, 1e-6), "scene units amplify the surviving residual while recovering the bounded fraction");
  fr.daylight_recovery = 0.0f;
  Check(NearV(run(s, wall), {0.160f, 0, 0}, 1e-6), "units-only mode leaves the baseline subtracted");
  fr.daylight_recovery = -1.0f;
  Check(NearV(run(s, wall), ga::LookupNet(f, s, wall, fr.dead_band), 0), "legacy mode exactly matches the old apply");
  fr.daylight_recovery = 0.0625f;
  s = {{0.024f, 0.014f, 0.009f}, 0.50f, 0.5f};
  Check(NearV(run(s, ground), {0.16f, 0.08f, 0.04f}, 1e-6), "covered ground can gain from valid probes");
  s.sky_visibility = 1.0f;
  Check(NearV(run(s, ground), ga::LookupNet(f, s, ground, fr.dead_band), 0), "open ground receives no extra gain even with a positive residual");
  for (const gi::Vec3 n : {wall, ground, ceiling}) {
    s = {gi::GroundBaseline(f, n), 0.5f * (1.0f + n.y), 1.0f};
    Check(NearV(run(s, n), {}, 0), "an ideal open plane adds zero at every tested normal");
  }
  s = shade;
  s.confidence = 0.0f;
  Check(NearV(run(s, wall), {}, 0), "zero confidence rejects recovered light too");
  s = shade;
  s.irradiance = {};
  Check(NearV(run(s, wall), {}, 0), "a covered receiver with no incoming irradiance stays dark");
  f.sun_sin_elevation = 0.20f;
  fr.daylight_recovery = 0.0f;
  Check(NearV(run(shade, ground), ga::LookupNet(f, shade, ground, fr.dead_band) * 8.5f, 1e-6), "daylight correction is half strength at 0.20 sun elevation sine");
  f.sun_sin_elevation = -0.1f;
  fr.daylight_recovery = 0.0625f;
  Check(NearV(run(shade, wall), ga::LookupNet(f, shade, wall, fr.dead_band), 0), "night lamps use the existing apply");
}

// ---------------------------------------------------------------------------
// the SSGI switch
// ---------------------------------------------------------------------------
void SwitchChecks() {
  gi::GiFrame f;
  f.valid = gi::kFieldProducts;
  f.slot = {1, 2, 3, gi::kNoSlot};
  f.cascade_count = 3;
  f.level = gi::Level::kHigh;
  f.safe_range_m = 300.0f;
  f.fade = 0.0f;
  Check(!f.ReplacesSsgi() && ga::SsgiShareFor(f) == 1.0f, "SWITCH: a field with fade 0 (just cut, warming up) leaves the screen-space bounce light whole");
  f.fade = 0.25f;
  Check(f.ReplacesSsgi() && Near(ga::SsgiShareFor(f), 0.75, 1e-6) && !ga::SsgiSkipped(ga::SsgiShareFor(f)), "SWITCH: a quarter in, the screen-space bounce light runs at three quarters of its strength: a crossfade");
  f.fade = 1.0f;
  Check(ga::SsgiShareFor(f) == 0.0f && ga::SsgiSkipped(ga::SsgiShareFor(f)), "SWITCH: once the field stands the colour chain is not built (share 0)");
  gi::GiFrame off = f;
  off.level = gi::Level::kOff;
  Check(ga::SsgiShareFor(off) == 1.0f, "SWITCH: the setting off: SSGI as ever");
  off = f;
  off.safe_range_m = 0.0f;
  Check(ga::SsgiShareFor(off) == 1.0f, "SWITCH: no safe range (nothing built, the pause menu's world): SSGI as ever");
  off = f;
  off.valid = 0;
  Check(ga::SsgiShareFor(off) == 1.0f && ga::FieldShare(off, 0.0f) == 0.0f, "SWITCH: a frame that did not publish the field: SSGI as ever, the field adds nothing");
  off = f;
  off.slot[gi::kMoments] = gi::kNoSlot;
  Check(ga::SsgiShareFor(off) == 1.0f && ga::FieldShare(off, 0.0f) == 0.0f, "SWITCH: a frame missing one of the three textures: SSGI as ever");
  // the field's share: never more than what SSGI left
  bool sum_ok = true;
  for (int i = 0; i <= 20; ++i) {
    f.fade = float(i) / 20.0f;
    const float share = ga::SsgiShareFor(f);
    sum_ok = sum_ok && Near(ga::FieldShare(f, share) + share, 1.0, 1e-6);
    sum_ok = sum_ok && ga::FieldShare(f, 1.0f) == 0.0f;  // the occlusion planned the frame before the field was in it: SSGI ran whole: no field, no double bounce
    sum_ok = sum_ok && ga::FieldShare(f, 0.0f) == f.fade;  // SSGI did not run at all (the occlusion is off): the field takes its whole fade
  }
  Check(sum_ok, "SWITCH: the field's share and SSGI's make exactly one bounce through the fade; SSGI at full strength means no field; no SSGI means the field's whole fade");
  // the pixel-level crossfade of gi_frame.h and the frame-level one agree where the confidence is 1
  Check(gi::SsgiShare(1.0f, 0.4f) == 1.0f - 0.4f && Near(ga::SsgiShareFor([&] {
                                                              gi::GiFrame g = f;
                                                              g.fade = 0.4f;
                                                              return g;
                                                            }()),
                                                            gi::SsgiShare(1.0f, 0.4f), 1e-6),
        "SWITCH: the frame's share is gi_frame.h's SsgiShare at confidence 1");
}

// ---------------------------------------------------------------------------
// the buffer, the push, the views
// ---------------------------------------------------------------------------
void LayoutChecks() {
  const double camera[3] = {10.0, 5.0, 10.0};
  Field fld = MakeField(gi::Layout::kLean, camera, [](gi::Vec3) { return gi::Rgba{0, 0, 0, 0}; });
  ga::Frame fr = MakeApplyFrame(fld.frame);
  fr.right[0] = 0.8f;
  fr.right[2] = 0.6f;
  fr.position[0] = 12.5f;
  fr.position[1] = 3.0f;
  fr.position[2] = -40.0f;
  fr.ao_strength = 0.75f;
  fr.dead_band = 0.04f;
  fr.scene_units = 16.0f;
  fr.daylight_recovery = 0.0625f;
  const gi::GpuGiFrame gpu = gi::MakeGpuFrame(fld.frame);
  const ga::Buffer buf = ga::MakeBuffer(gpu, fr);
  Check(sizeof(buf) == 464 && ga::kBufferBytes == 464 && ga::kRowsOffsetBytes == 320 && ga::kBufferAlloc == 512, "BUFFER: the field's 20 rows then 9 more: 464 bytes in a 512 byte upload buffer");
  Check(std::memcmp(buf.data(), gpu.words.data(), 320) == 0, "BUFFER: the first 320 bytes are the field's rows, unchanged");
  hlsl_shim::g_rows.assign(gi::kGpuFrameRows + ga::kApplyRows, hlsl_shim::uint4());
  for (uint32_t r = 0; r < gi::kGpuFrameRows + ga::kApplyRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(buf[r * 4], buf[r * 4 + 1], buf[r * 4 + 2], buf[r * 4 + 3]);
  const hlsl_shim::GiaFrame h = hlsl_shim::GiaLoadFrame();
  const bool ok = h.right.x == fr.right[0] && h.right.z == fr.right[2] && h.tanX == fr.tan_x && h.up.y == fr.up[1] && h.tanY == fr.tan_y && h.forward.z == fr.forward[2] && h.fieldShare == fr.field_share &&
                  h.position.x == fr.position[0] && h.position.y == fr.position[1] && h.position.z == fr.position[2] && h.strength == fr.strength && h.lin.z == fr.lin[2] && h.render.x == fr.render[0] &&
                  h.render.y == fr.render[1] && h.traced.x == fr.traced[0] && h.traced.y == fr.traced[1] && h.sunDir.x == fr.sun_dir[0] && h.sunDir.y == fr.sun_dir[1] && h.sunDir.z == fr.sun_dir[2] &&
                  h.ambient == fr.ambient && h.sunLuma == fr.sun && h.cap == fr.cap && h.aoStrength == fr.ao_strength && h.aoFadeEnd == fr.ao_fade_end && h.deadBand == fr.dead_band &&
                  h.paintPower == fr.paint_power && h.sceneUnits == fr.scene_units && h.daylightRecovery == fr.daylight_recovery;
  Check(ok, "BUFFER: the shader's GiaLoadFrame reads back every number the host wrote");
  Check(hlsl_shim::GiFade() == fld.frame.fade && hlsl_shim::GiCascadeCount() == 2, "BUFFER: and the field's own rows are where gi_common.hlsli reads them");
  // the push block's last word
  Check(ga::PackInfoW(ga::kFlagLowTraced | ga::kFlagVisibility, ga::kViewBounce) == (3u | (9u << 8)) && ((ga::PackInfoW(0, 11) & ga::kViewMask) >> ga::kViewShift) == 11u, "PUSH: the flags and the view share the last word");
  // the views
  Check(ga::IsApplyView(1) && ga::IsApplyView(6) && ga::IsApplyView(11) && !ga::IsApplyView(0) && !ga::IsApplyView(12), "VIEWS: this pass draws 1, 2, 3, 5, 6, 9, 10 and 11");
  bool disjoint = true;
  for (uint32_t v = 0; v < 32; ++v) disjoint = disjoint && !(ga::IsApplyView(v) && mcla::gpu::gi_field::IsOwnView(v));
  Check(disjoint, "VIEWS: none of them is one the field owner draws (4, 7, 8: the probes as spheres)");
  Check(ga::kViewIrradiance == uint32_t(gi::View::kIrradiance) && ga::kViewValidity == uint32_t(gi::View::kValidity) && ga::kViewSkyVisibility == uint32_t(gi::View::kSkyVisibility) &&
            ga::kViewBaseline == uint32_t(gi::View::kBaseline) && ga::kViewLeak == uint32_t(gi::View::kLeak),
        "VIEWS: 1, 2, 3, 5 and 6 are gi_frame.h's View values");
  Check(std::string(ga::SkipName(ga::Skip::kRecorded)) == "recorded" && std::string(ga::SkipName(ga::Skip::kCount)) == "?", "the skips have names");
}

// ---------------------------------------------------------------------------
// the leak test on a synthetic wall
// ---------------------------------------------------------------------------
void LeakChecks() {
  const double camera[3] = {0.5, 4.5, 0.5};
  // A wall: the probes right of x = 3 (their cell) sit behind it; their moments say the wall is 0.5 m from them in every direction towards it, and the probes left of it see it at the
  // distance to it. Built directly: moments of a probe = the distance to the plane x = 3 along the map's direction (or far if it escapes).
  Field fld = MakeField(gi::Layout::kStandard, camera, [](gi::Vec3) { return gi::Rgba{0.5f, 0.5f, 0.5f, 0.5f}; });
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kStandard);
  const gi::CascadeFrame& k = fld.frame.cascades[0];
  const float wall_x = 3.0f;
  for (uint32_t z = 0; z < k.dims.z; ++z)
    for (uint32_t y = 0; y < k.dims.y; ++y)
      for (uint32_t x = 0; x < k.dims.x; ++x) {
        const gi::Coord c = {k.origin.x + int32_t(x), k.origin.y + int32_t(y), k.origin.z + int32_t(z)};
        const gi::Slot s = gi::SlotOf(c, k.dims);
        const gi::Texel tile = gi::TileOrigin(s, k.dims);
        const float px = float(c.x) * k.spacing_m;
        for (uint32_t ty = 0; ty < gi::kProbeTile; ++ty)
          for (uint32_t tx = 0; tx < gi::kProbeTile; ++tx) {
            const gi::Texel src = gi::OctBorderSource(tx, ty);
            const gi::Vec3 dir = gi::TexelDirection(src.x, src.y);
            float dist = gi::MaxDistanceFor(k.spacing_m);
            if (px < wall_x && dir.x > 1e-3f) dist = std::min(dist, (wall_x - px) / dir.x);
            if (px >= wall_x && dir.x < -1e-3f) dist = std::min(dist, (wall_x - px) / dir.x);
            const size_t at = size_t(gi::AtlasRow(b, 0) + tile.y + ty) * fld.atlas.w + (tile.x + tx);
            fld.atlas.mom[at * 2 + 0] = dist;
            fld.atlas.mom[at * 2 + 1] = dist * dist;
          }
      }
  // a point in the open, left of the wall, looking at it from 1.1 m and from 3 m away
  const gi::Vec3 n = {1.0f, 0.0f, 0.0f};  // a surface facing the wall: the probes behind the wall are in front of the surface's plane, only the visibility test can cut them
  const gi::Vec3 near_wall = {wall_x - 0.4f, 4.0f, 0.5f};
  const gi::Vec3 open = {-18.0f, 4.0f, 0.5f};
  const float g_wall = ga::GuardShare(fld.frame, fld.atlas, near_wall, n);
  const float g_open = ga::GuardShare(fld.frame, fld.atlas, open, n);
  Check(g_wall > 0.05f && g_wall > 4.0f * g_open + 0.01f, Num("LEAK TEST: the visibility test takes %.0f%% of the probes' weight at a thin wall and %.0f%% in the open", 100.0 * g_wall, 100.0 * g_open));
  Check(g_open < 0.02f, "LEAK TEST: ... almost none in the open");
  Check(ga::GuardShare(fld.frame, fld.atlas, {5000.0f, 5000.0f, 5000.0f}, n) == 0.0f, "LEAK TEST: a point outside every cascade reports 0");
  // the hlsli's version of the same on a random set of points
  hlsl_shim::g_rows.assign(gi::kGpuFrameRows + ga::kApplyRows, hlsl_shim::uint4());
  const gi::GpuGiFrame gpu = gi::MakeGpuFrame(fld.frame);
  const ga::Buffer buf = ga::MakeBuffer(gpu, MakeApplyFrame(fld.frame));
  for (uint32_t r = 0; r < gi::kGpuFrameRows + ga::kApplyRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(buf[r * 4], buf[r * 4 + 1], buf[r * 4 + 2], buf[r * 4 + 3]);
  g_atlas = &fld.atlas;
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 600; ++i) {
    const gi::Vec3 p = {rng.Range(-20.0f, 20.0f), rng.Range(-4.0f, 12.0f), rng.Range(-20.0f, 20.0f)};
    const gi::Vec3 nn = rng.Direction();
    if (!Near(hlsl_shim::GiaGuardShare(H3(p), H3(nn)), ga::GuardShare(fld.frame, fld.atlas, p, nn), 1e-5)) ++bad;
  }
  g_atlas = nullptr;
  Check(bad == 0, "LEAK TEST: the .hlsli's guard share is the twin's on 600 random points");
}

// ---------------------------------------------------------------------------
// the .hlsli as C++ against the twin
// ---------------------------------------------------------------------------
void TwinChecks() {
  Lcg rng;
  int bad = 0;
  const auto check = [&](bool ok) { bad += ok ? 0 : 1; };
  for (int i = 0; i < 6000; ++i) {
    const gi::Vec3 e = {rng.Range(0.0f, 8.0f), rng.Range(0.0f, 8.0f), rng.Range(0.0f, 8.0f)};
    check(Near(hlsl_shim::GiaLuma(H3(e)), ga::Luma(e), 1e-6));
    const float limit = rng.Range(-0.2f, 5.0f), x = rng.Range(0.0f, 9.0f);
    check(Near(hlsl_shim::GiaSoftClip(x, limit), sl::SoftClip(x, limit), 1e-6, 1e-6));
    const float cap = rng.Range(0.2f, 6.0f);
    check(NearV(C3(hlsl_shim::GiaCapRadiance(H3(e), cap)), ga::CapRadiance(e, cap), 1e-5, 1e-5));
    const float amb = rng.Range(0.0f, 1.0f), sun = rng.Range(0.0f, 2.0f), cm = rng.Range(0.25f, 4.0f);
    check(Near(hlsl_shim::GiaRadianceCap(cm, amb, sun), sl::RadianceCap(cm, amb, sun), 1e-6));
    const float s = rng.Range(0.0f, 7.0f), v = rng.Range(-0.2f, 1.2f);
    check(Near(hlsl_shim::GiaStrengthFor(s, v), ga::StrengthFor(s, v), 1e-6));
    const float ao = rng.Range(0.0f, 1.0f), veh = rng.Range(0.0f, 1.0f), tc = rng.Range(0.5f, 400.0f), as = rng.Range(0.0f, 1.5f), fe = rng.Range(20.0f, 1000.0f), pp = rng.Range(0.1f, 1.0f);
    check(Near(hlsl_shim::GiaAoTerm(ao, veh, tc, as, fe, pp), ga::AoTerm(ao, veh, tc, as, fe, pp), 1e-5, 1e-5));
    const float t1 = rng.Range(0.5f, 500.0f), t2 = rng.Range(0.5f, 500.0f);
    check(Near(hlsl_shim::GiaDepthWeight(t1, t2), ga::DepthWeight(t1, t2), 1e-6, 1e-5));
    const gi::Vec3 n1 = rng.Direction(), n2 = rng.Direction();
    check(Near(hlsl_shim::GiaNormalWeight(H3(n1), H3(n2)), ga::NormalWeight(n1, n2), 1e-6));
    ga::PixelIn in;
    in.albedo = {rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f), rng.Range(0.0f, 1.0f)};
    in.lit = rng.Range(-0.1f, 1.1f);
    in.e_net = e;
    in.strength = s;
    in.field_share = rng.Range(0.0f, 1.0f);
    in.vehicle = veh;
    in.ao_term = ao;
    in.ambient = amb;
    in.sun = sun;
    in.n_dot_l = rng.Range(0.0f, 1.0f);
    in.cap = cm;
    check(NearV(C3(hlsl_shim::GiaBounceAdd(H3(in.albedo), in.lit, H3(in.e_net), in.strength, in.field_share, in.vehicle, in.ao_term, in.ambient, in.sun, in.n_dot_l, in.cap)), ga::BounceAdd(in), 1e-5, 1e-5));
  }
  Check(bad == 0, Num("TWIN: the .hlsli's maths (luma, soft clip, cap, vehicle strength, AO term, weights, the whole add) is the twin's on 6,000 random inputs: %.0f mismatches", bad));

  // the lookup's net irradiance and the camera on a real synthetic field
  const double camera[3] = {40.0, 10.0, 40.0};
  Field fld = MakeField(gi::Layout::kStandard, camera, [](gi::Vec3 w) { return gi::Rgba{0.2f + 0.3f * std::max(w.x, 0.0f), 0.1f + 0.2f * std::max(w.y, 0.0f), 0.15f, 0.5f + 0.5f * w.y}; });
  ga::Frame fr = MakeApplyFrame(fld.frame);
  const gi::GpuGiFrame gpu = gi::MakeGpuFrame(fld.frame);
  const ga::Buffer buf = ga::MakeBuffer(gpu, fr);
  hlsl_shim::g_rows.assign(gi::kGpuFrameRows + ga::kApplyRows, hlsl_shim::uint4());
  for (uint32_t r = 0; r < gi::kGpuFrameRows + ga::kApplyRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(buf[r * 4], buf[r * 4 + 1], buf[r * 4 + 2], buf[r * 4 + 3]);
  g_atlas = &fld.atlas;
  int net_bad = 0;
  for (int i = 0; i < 1500; ++i) {
    const gi::Vec3 p = {float(camera[0]) + rng.Range(-40, 40), float(camera[1]) + rng.Range(-12, 6), float(camera[2]) + rng.Range(-40, 40)};
    const gi::Vec3 n = rng.Direction();
    const gi::Sample s = gi::Lookup(fld.frame, fld.atlas, p, n);
    const hlsl_shim::GiSample hs = hlsl_shim::GiLookup(H3(p), H3(n));
    hlsl_shim::GiaFrame h = hlsl_shim::GiaLoadFrame();
    h.sceneUnits = fr.scene_units = rng.Range(1.0f, 16.0f);
    h.daylightRecovery = fr.daylight_recovery = rng.Range(-0.0625f, 0.125f);
    const gi::Vec3 twin = ga::DaylightNet(fld.frame, s, n, fr);
    const gi::Vec3 hlsl = C3(hlsl_shim::GiaLookupNet(hs, H3(n), h));
    if (!NearV(twin, hlsl, 1e-4, 1e-4)) ++net_bad;
    const hlsl_shim::float3 net = hlsl_shim::GiaNetIrradiance(H3(s.irradiance), H3(n), fr.dead_band);
    if (!NearV(C3(net), ga::NetIrradiance(fld.frame, s.irradiance, n, fr.dead_band), 1e-5, 1e-5)) ++net_bad;
  }
  Check(net_bad == 0, "TWIN: the lookup's net irradiance (baseline, dead band, confidence) is the twin's on 1,500 random points");
  // the camera and the depth
  ga::Frame cam = fr;
  cam.right[0] = 0.8f;
  cam.right[1] = 0.0f;
  cam.right[2] = 0.6f;
  cam.forward[0] = -0.6f;
  cam.forward[1] = 0.0f;
  cam.forward[2] = 0.8f;
  cam.position[0] = 5.0f;
  cam.position[1] = 2.0f;
  cam.position[2] = -7.0f;
  const ga::Buffer buf2 = ga::MakeBuffer(gpu, cam);
  for (uint32_t r = 0; r < gi::kGpuFrameRows + ga::kApplyRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(buf2[r * 4], buf2[r * 4 + 1], buf2[r * 4 + 2], buf2[r * 4 + 3]);
  const hlsl_shim::GiaFrame hf = hlsl_shim::GiaLoadFrame();
  int cam_bad = 0;
  for (int i = 0; i < 1000; ++i) {
    const float stored = rng.Range(0.0f, 1.0f);
    if (hlsl_shim::GiaLinearDepth(stored, hlsl_shim::float4(cam.lin[0], cam.lin[1], cam.lin[2], cam.lin[3])) != ga::LinearDepth(stored, cam.lin)) ++cam_bad;
    const float cx = rng.Range(0.0f, 3840.0f), cy = rng.Range(0.0f, 2160.0f), t = rng.Range(1.0f, 900.0f);
    const gi::Vec3 a = C3(hlsl_shim::GiaWorldPosition(hf, cx, cy, 3840.0f, 2160.0f, t));
    const gi::Vec3 b = ga::WorldPosition(cam, cx, cy, 3840.0f, 2160.0f, t);
    if (!NearV(a, b, 1e-3, 1e-5)) ++cam_bad;
  }
  Check(cam_bad == 0, "TWIN: the .hlsli's linear depth and world position are the twin's");
  g_atlas = nullptr;
}

// ---------------------------------------------------------------------------
// the text of the shader, the CMake, the settings
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

void TextChecks(const std::string& dir) {
  const std::string common = ReadFile(dir + "/hlsl/gi_apply_common.hlsli");
  const std::string ps = ReadFile(dir + "/hlsl/gi_apply_ps.hlsl");
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  const std::string host = ReadFile(dir + "/host_shaders.h");
  Check(!common.empty() && !ps.empty() && !cmake.empty() && !host.empty(), "the sources are found");
  // every number #define of the .hlsli against the C++ value
  const std::map<std::string, double> expected = {
      {"GIA_FORMAT_VERSION", double(ga::kFormatVersion)}, {"GIA_ROWS", double(ga::kApplyRows)},
      {"GIA_SKY", double(ga::kSky)},                     {"GIA_CAP_FLOOR", double(ga::kCapFloor)},
      {"GIA_KNEE", double(ga::kKnee)},                   {"GIA_VEHICLE_SHARE", double(ga::kVehicleShare)},
      {"GIA_VEHICLE_MAX", double(ga::kVehicleStrengthMax)}, {"GIA_DEPTH_TOLERANCE", double(ga::kDepthTolerance)},
      {"GIA_DEPTH_TOLERANCE_FLOOR", double(ga::kDepthToleranceFloor)}, {"GIA_MIN_UPSAMPLE_WEIGHT", double(ga::kMinUpsampleWeight)},
      {"GIA_PAINT_FROM", double(ga::kPaintFrom)},        {"GIA_VIEW_SCALE", double(ga::kViewScale)},
      {"GIA_FLAG_LOW_TRACED", double(ga::kFlagLowTraced)}, {"GIA_FLAG_VISIBILITY", double(ga::kFlagVisibility)},
      {"GIA_NO_SLOT", double(0xFFFFFFFFu)}};
  int defined = 0, wrong = 0;
  std::istringstream lines(common);
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("#define GIA_", 0) != 0) continue;
    std::istringstream words(line);
    std::string define, name, value;
    words >> define >> name >> value;
    const auto it = expected.find(name);
    if (it == expected.end()) continue;  // GIA_ROW(i) is a function-like macro
    ++defined;
    while (!value.empty() && (value.back() == 'f' || value.back() == 'u' || value.back() == 'F' || value.back() == 'U') && value.rfind("0x", 0) != 0) value.pop_back();
    if (value.rfind("0x", 0) == 0) {
      while (!value.empty() && (value.back() == 'u' || value.back() == 'U')) value.pop_back();
    }
    const double v = value.rfind("0x", 0) == 0 ? double(std::strtoull(value.c_str(), nullptr, 16)) : std::strtod(value.c_str(), nullptr);
    if (!Near(v, it->second, 1e-12, 1e-6)) {
      ++wrong;
      std::printf("  %s: .hlsli %.9g, C++ %.9g\n", name.c_str(), v, it->second);
    }
  }
  Check(defined == int(expected.size()) && wrong == 0, Num("the .hlsli's #defines are gi_apply_rules.h's: %.0f of them compared, %.0f differ", defined, wrong));
  // the gi_common.hlsli row numbers the apply's rows follow
  Check(Has(common, "#define GIA_ROW(i) GI_FRAME_ROW(GI_FRAME_ROWS + (i))") && Has(common, "#define GIA_ROWS 9u"), "the apply's rows follow the field's GI_FRAME_ROWS");
  // the shader's registers: bindless planes, multisampled planes, samplers, the buffer, the push block
  Check(Has(ps, "Texture2D<float4> g_textures[] : register(t0, space0)") && Has(ps, "Texture2DMS<float4> g_textures_ms[] : register(t0, space1)") && Has(ps, "SamplerState g_samplers[] : register(s0, space2)") &&
            Has(ps, "ByteAddressBuffer g_rows : register(t0, space3)") && Has(ps, "register(b0, space4)"),
        "the shader's layout: textures space0, multisampled space1, samplers space2, the rows' buffer space3, the push block (b0, space4)");
  Check(ga::kPushSpace == 4 && Has(ps, "ConstantBuffer<GiaPush> g_push : register(b0, space4)"), "the push block's space is the host's (4)");
  const size_t push_at = ps.find("struct GiaPush");
  const size_t push_end = ps.find("};", push_at);
  Check(push_at != std::string::npos && push_end != std::string::npos && Count(ps.substr(push_at, push_end - push_at), "uint4 ") == 2, "the push block is two uint4 (32 bytes, as Push)");
  Check(Has(ps, "#include \"gi_common.hlsli\"") && Has(ps, "#include \"gi_apply_common.hlsli\"") && Has(ps, "#include \"gbuffer_common.hlsli\""), "the shader reads the field's decode, the apply's maths and the G-buffer's decode");
  Check(Has(ps, "#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)"), "the rows are read from the buffer, 16 bytes a row");
  Check(Has(ps, "GIA_PASS == 0") && Has(ps, "#if GIA_UP") && Has(ps, "#if GIA_MS"), "the three switches: the pass, the upsample, MSAA");
  // the CMake: six shaders from the one source, the test, the source file
  for (const char* line : {"larecomp_gpu_host_shader(gi_apply_lookup_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=0 GIA_MS=0 GIA_UP=0)", "larecomp_gpu_host_shader(gi_apply_lookup_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=0 GIA_MS=1 GIA_UP=0)",
                           "larecomp_gpu_host_shader(gi_apply_ps ps_6_0 DEFINES GIA_PASS=1 GIA_MS=0 GIA_UP=0)", "larecomp_gpu_host_shader(gi_apply_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=1 GIA_UP=0)",
                           "larecomp_gpu_host_shader(gi_apply_up_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=0 GIA_UP=1)", "larecomp_gpu_host_shader(gi_apply_up_ms_ps ps_6_0 SOURCE gi_apply_ps DEFINES GIA_PASS=1 GIA_MS=1 GIA_UP=1)"}) {
    Check(Has(cmake, line), std::string("CMake: ") + line);
  }
  Check(Has(cmake, "gi_apply.cpp") && Has(cmake, "add_executable(larecomp_gi_apply_test EXCLUDE_FROM_ALL gi_apply_test.cpp)"), "CMake: the source and the test are listed");
  // host_shaders.h: the six blobs for both backends and the accessor
  int blobs = 0;
  for (const char* name : {"gi_apply_lookup_ps", "gi_apply_lookup_ms_ps", "gi_apply_ps", "gi_apply_ms_ps", "gi_apply_up_ps", "gi_apply_up_ms_ps"}) {
    blobs += Has(host, std::string("#include \"") + name + ".hlsl.dxil.h\"") ? 1 : 0;
    blobs += Has(host, std::string("#include \"") + name + ".hlsl.spirv.h\"") ? 1 : 0;
  }
  Check(blobs == 12 && Has(host, "GiApplyShader("), "host_shaders.h includes the six shaders for both backends and has the accessor");
  // THE ORDER at the trigger (scene.cpp): the occlusion's apply first (it multiplies the colour), then this pass (adds the bounce, multiplies its own light by the occlusion's visibility), then the
  // deferred lights (their direct light is multiplied by neither); the frame's start tells the pass before the G-buffer and the shared products read the wants it makes
  const std::string scene = ReadFile(dir + "/scene.cpp");
  const size_t at_ao = scene.find("ao_->OnResolve(record, summary, list, UpscalerRunning());");
  const size_t at_gi = scene.find("gi_apply_->OnTrigger(summary, list, gbuffer_published, slot_, raw_gi);");
  const size_t at_ll = scene.find("local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_);");
  Check(at_ao != std::string::npos && at_gi != std::string::npos && at_ll != std::string::npos && at_ao < at_gi && at_gi < at_ll,
        "ORDER: the occlusion's apply, then the probe field's apply, then the deferred lights (scene.cpp OnResolve)");
  const size_t begin_gi = scene.find("gi_apply_->BeginFrame();");
  const size_t begin_gb = scene.find("gbuffer_->BeginFrame(frame_serial_);");
  Check(begin_gi != std::string::npos && begin_gb != std::string::npos && begin_gi < begin_gb, "ORDER: the pass is told of the frame's start before the G-buffer reads the wants it makes");
  // THE SSGI SWITCH in the occlusion's plan: asked once, not under the debug override, the strength scaled, nothing built at share 0
  const std::string ao = ReadFile(dir + "/ao.cpp");
  const size_t at_take = ao.find("gi_apply_->TakeSsgiShare()");
  const size_t at_plan = ao.find("void AmbientOcclusion::PlanGi(Plan* plan) {");
  Check(at_take != std::string::npos && at_plan != std::string::npos && at_plan < at_take && Has(ao, "if (gi_apply_ && !gi_test_) {") && Has(ao, "if (gi_apply::SsgiSkipped(share)) {") && Has(ao, "g.strength *= share;") &&
            Has(ao, "const float share = gi_apply_->TakeSsgiShare();"),
        "SWITCH: PlanGi asks the share once (not under the debug override), returns the plain occlusion at share 0 and scales the strength otherwise");
  Check(Has(ao, "gi_apply_->NoteSsgiRan(0.0f);") && Has(ao, "bool AmbientOcclusion::FrameOcclusionNow(OcclusionView* out) const {") && Has(ao, "bool AmbientOcclusion::FrameLight(float ambient[3], float sun[3], double to_sun[3]) const {"),
        "SWITCH: a bounce light that could not be made tells the apply; the occlusion publishes its visibility and its light for it");
  // the apply's own wants: RT1 and RT2 of the G-buffer, the depth chain of the shared products
  const std::string cpp = ReadFile(dir + "/gi_apply.cpp");
  Check(Has(cpp, "gbuffer::Consumer::kRtBounce") && Has(cpp, "screen::Consumer::kRtBounce") && Has(cpp, "field_->running()") && Has(cpp, "if (!gf.FieldPresent()) {") && Has(cpp, "ga::FieldShare(gf, ssgi_ran_share_)"),
        "IDENTITY: the pass wants planes only while the field runs, records nothing for a frame the field is not in, and takes the share the occlusion left");
  // the picture's size and whether the traced images are half of it come from the shared frame: the G-buffer's planes are the guest's padded colour target (1280x736 for a 1280x720 picture, 612e6dee), so a trace
  // at the picture's own size (1080 lines and below, the occlusion or the contact shadows at ultra) was taken for a half size one and the upsample read the visibility and the vehicle mark at half the coordinates
  Check(Has(cpp, "const bool up = (sf.flags & screen::kFrameHalfTraced) != 0;") && !Has(cpp, "sf.traced[0] != gb.width") && !Has(cpp, "sf.traced[1] != gb.height") && Has(cpp, "fr.render[0] = float(sf.render[0]);") &&
            Has(cpp, "fr.render[1] = float(sf.render[1]);") && Has(cpp, "pixel_in.render[0] = sf.render[0];") && Has(cpp, "pixel_in.render[1] = sf.render[1];") && !Has(cpp, "fr.render[0] = float(gb.width);"),
        "PADDING: the picture's size and the half size trace are the shared frame's, not the padded planes' (a full size trace is not upsampled)");
  // the trigger is the copy of the MAIN view's depth: the scene calls the apply at every resolve and a depth copy of the sun's shadow cascades (day only) came first and used the frame's one run while the field
  // was not published (the chain's noon runs: 'the apply pass never ran'; night has no cascades)
  {
    const size_t at_gate = cpp.find("if (!summary.depth_copy || temporal::MainDepthSamples(summary.source_key) == 0) {");
    const size_t at_seen = cpp.find("trigger_seen_ = true;  // once a frame");
    Check(at_gate != std::string::npos && at_seen != std::string::npos && at_gate < at_seen,
          "TRIGGER: the apply's run is taken by the main view's depth copy only (a shadow cascade's copy before it does not use the frame's one run)");
  }
  // the settings: the debug view
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  Check(Has(manifest, "\"mcla_gpu_rt_gi_view\"") && Has(ReadFile(dir + "/../settings/generated/settings_gpu.h"), "mcla_gpu_rt_gi_view") && Has(ReadFile(dir + "/../settings/generated/settings_meta.inc"), "mcla_gpu_rt_gi_view"),
        "the debug view's setting (mcla_gpu_rt_gi_view) is in the manifest, the header and the meta list");
  Check(Has(cpp, "fr.scene_units = std::clamp(sf.camera.exposure_bias, 1.0f, 16.0f);") && Has(cpp, "fr.daylight_recovery = float(larecomp::settings::mcla_gpu_rt_gi_daylight_recovery.Get());"),
        "DAYLIGHT: the live setting and the resolved main colour target's exposure reach the apply rows");
  Check(Has(ps, "GiaLookupNet(s, n, fr)") && Has(ps, "GiaLookupNetMixed(s, n, fr, GiaPixelTerm(p, t, n, fr))") && Has(ps, "GiaLookupNetMixed(s, n, fr, GiaPixelTerm(pixel, t, n, fr))"),
        "DAYLIGHT: inline, half-size and mixed Ultra lookups use the same rules");
}

std::string DirOfThisFile() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = DirOfThisFile();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v") {
      g_verbose = true;
    } else {
      dir = a;
    }
  }
  IdentityChecks();
  RuleChecks();
  AoAndUpsampleChecks();
  DaylightChecks();
  SwitchChecks();
  LayoutChecks();
  LeakChecks();
  TwinChecks();
  TextChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
