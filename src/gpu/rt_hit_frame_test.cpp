// Check of what every hit shading shares (CITY-RT M2 package H4): src/gpu/rt_hit_frame.h and hlsl/rt_hit_frame.hlsli + hlsl/rt_hit_sample.hlsli (together rt_hit_common.hlsli), and the exposure
// calibration of src/gpu/rt_hit_calibration.h. A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -I src -I src/gpu src/gpu/rt_hit_frame_test.cpp -o rt_hit_frame_test && rt_hit_frame_test <src dir>
//
// It compiles the shader files THEMSELVES as C++ (hlsl_cpp_shim.h: the HLSL types and intrinsics, a fake texture set) and checks them against the C++ twin: the frame's rows as MakeFrameRows
// writes them are what RtHitLoadFrame reads back, the cascades' lookup is CascadeLit's over a fake atlas, the environment map's mapping is EnvUv's, a texture read by slot is a bilinear two
// level read with the game's gamma, the packing of a colour is PackRgb10's. It also holds the car files to the move: the car's hit shading reads the frame through the shared names. The calibration
// (rt_hit_calibration.h) is checked for its arithmetic, for the car's numbers (unchanged), and for the per class gains of the city's.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_hit_rules.h"
#include "hlsl_cpp_shim.h"
#include "rt_hit_calibration.h"
#include "rt_hit_frame.h"
#include "rt_hit_sample_ref.h"

namespace hlsl_shim {
StructuredBuffer<uint4> g_frame;
TextureArray g_textures;
#define RT_HIT_FRAME_ROW(i) g_frame[i]
#define RT_HIT_NO_RAYQUERY
#define RT_HIT_UNROLL
#include "hlsl/rt_hit_frame.hlsli"
#include "hlsl/rt_hit_sample.hlsli"
}  // namespace hlsl_shim

namespace {

namespace rh = mcla::gpu::rt_hit;
namespace hc = mcla::gpu::rt_hit_cal;
namespace hr = mcla::gpu::car_rt_hit;
using rt_hit_ref::Rgba;
using rt_hit_ref::RefPwl;
using rt_hit_ref::RefSample;

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

// ---------------------------------------------------------------------------
// the frame's rows
// ---------------------------------------------------------------------------

rh::FrameInput RandomFrame(Lcg& rng, bool with_cascades) {
  rh::FrameInput in;
  in.light.valid = true;
  for (int c = 0; c < 3; ++c) {
    in.light.sun[c] = rng.Range(0.0f, 1.5f);
    in.light.ambient[c] = rng.Range(0.0f, 0.5f);
    in.light.sky[c] = rng.Range(0.0f, 0.5f);
  }
  in.light.env_sharp = rng.Below(2) ? (0x80000000u | (100 + rng.Below(5))) : 0u;
  in.light.env_blur = rng.Below(8);
  in.sun_known = rng.Below(4) != 0;
  const float x = rng.Range(-1, 1), y = rng.Range(-1, 1), z = rng.Range(-1, 1);
  const float len = std::sqrt(x * x + y * y + z * z) + 1e-3f;
  in.to_sun_view[0] = x / len;
  in.to_sun_view[1] = y / len;
  in.to_sun_view[2] = z / len;
  for (int a = 0; a < 3; ++a) {
    for (int c = 0; c < 3; ++c) in.world_axes_view[a][c] = rng.Range(-1, 1);
  }
  in.tuning.gain = rng.Range(0.5f, 2.0f);
  in.tuning.ambient = rng.Range(0.5f, 1.5f);
  in.tuning.spec = rng.Range(0.5f, 1.5f);
  in.tuning.shadow = rng.Below(2) != 0;
  in.shadow_reach = rng.Range(2, 10);
  if (with_cascades) {
    in.camera_known = true;
    for (int c = 0; c < 3; ++c) in.camera_world[c] = rng.Range(-500, 500);
    in.cascades.valid = true;
    in.cascades.atlas_slot = 7;
    in.cascades.count = 1 + rng.Below(4);
    in.cascades.fade = rng.Range(0, 1);
    in.cascades.depth_bias = rng.Range(-0.01f, 0.01f);
    for (int c = 0; c < 3; ++c) {
      in.cascades.right[c] = rng.Range(-1, 1);
      in.cascades.top[c] = rng.Range(-1, 1);
    }
    for (uint32_t i = 0; i < in.cascades.count; ++i) {
      for (int j = 0; j < 4; ++j)
        for (int c = 0; c < 4; ++c) in.cascades.rows[i][j][c] = rng.Range(-0.1f, 0.1f);
      in.cascades.box[i][0] = rng.Range(-20, 20);
      in.cascades.box[i][1] = rng.Range(10, 400);
      in.cascades.box[i][2] = rng.Range(-20, 20);
      in.cascades.box[i][3] = rng.Range(10, 400);
    }
  }
  return in;
}

void FrameChecks() {
  Lcg rng;
  // the layout and the numbers
  CHECK(hlsl_shim::kRtHitFrameRows == rh::kFrameRows && hlsl_shim::kRtHitCascadeHeaderRow == rh::kCascadeHeaderRow && hlsl_shim::kRtHitCameraRow == rh::kCameraRow &&
        hlsl_shim::kRtHitCascadeRightRow == rh::kCascadeRightRow && hlsl_shim::kRtHitCascadeTopRow == rh::kCascadeTopRow && hlsl_shim::kRtHitCascadeFirstRow == rh::kCascadeFirstRow &&
        hlsl_shim::kRtHitCascadeRowsEach == rh::kCascadeRowsEach && hlsl_shim::kRtHitMaxCascades == rh::kMaxCascades);
  CHECK(RT_HIT_FRAME_SHADE == rh::kFrameShade && RT_HIT_FRAME_SHADOW == rh::kFrameShadow && RT_HIT_FRAME_ENV == rh::kFrameEnv && RT_HIT_FRAME_SUN == rh::kFrameSun &&
        RT_HIT_FRAME_CASCADE == rh::kFrameCascade);
  CHECK(Near(hlsl_shim::kRtHitCascadeNormalOffset, rh::kCascadeNormalOffset) && Near(hlsl_shim::kRtHitTintMax, rh::kTintMax) && Near(hlsl_shim::kRtHitConeLodScale, rh::kConeLodScale) &&
        Near(hlsl_shim::kRtHitEmissiveGain, rh::kEmissiveGain) && Near(hlsl_shim::kRtHitFillShare, rh::kFillShare) && hlsl_shim::kRtHitNoSlot == rh::kNoSlot);

  // MakeFrameRows -> RtHitLoadFrame
  int wrong = 0;
  for (int round = 0; round < 400; ++round) {
    const rh::FrameInput in = RandomFrame(rng, round % 2 == 0);
    uint32_t rows[rh::kFrameRows][4];
    rh::MakeFrameRows(in, rows);
    std::vector<hlsl_shim::uint4> buffer(rh::kFrameRows);
    for (uint32_t i = 0; i < rh::kFrameRows; ++i) buffer[i] = hlsl_shim::uint4(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
    hlsl_shim::g_frame.data = buffer.data();
    hlsl_shim::g_frame.count = buffer.size();
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    const bool shade = in.tuning.enabled && in.light.valid;
    const bool cascade = in.tuning.cascade && rh::CascadesUsable(in.cascades) && in.camera_known;
    if (((f.flags & RT_HIT_FRAME_SHADE) != 0) != shade) ++wrong;
    if (((f.flags & RT_HIT_FRAME_SHADOW) != 0) != in.tuning.shadow) ++wrong;
    if (((f.flags & RT_HIT_FRAME_ENV) != 0) != (in.light.env_sharp != 0)) ++wrong;
    if (((f.flags & RT_HIT_FRAME_SUN) != 0) != in.sun_known) ++wrong;
    if (((f.flags & RT_HIT_FRAME_CASCADE) != 0) != cascade) ++wrong;
    if (!Near(f.toSun.x, in.to_sun_view[0], 1e-6) || !Near(f.toSun.y, in.to_sun_view[1], 1e-6) || !Near(f.toSun.z, in.to_sun_view[2], 1e-6)) ++wrong;
    if (!Near(f.sun.x, in.light.sun[0], 1e-6) || !Near(f.sun.y, in.light.sun[1], 1e-6) || !Near(f.sun.z, in.light.sun[2], 1e-6) || !Near(f.shadowReach, in.shadow_reach, 1e-6)) ++wrong;
    if (!Near(f.ambient.x, in.light.ambient[0], 1e-6) || !Near(f.ambient.z, in.light.ambient[2], 1e-6) || !Near(f.ambientGain, in.tuning.ambient, 1e-6)) ++wrong;
    if (f.envSharp != in.light.env_sharp || f.envBlur != in.light.env_blur || !Near(f.night, rh::NightFactor(in.light.sun), 1e-6) || !Near(f.specGain, in.tuning.spec, 1e-6)) ++wrong;
    if (!Near(f.sky.y, in.light.sky[1], 1e-6) || !Near(f.gain, in.tuning.gain, 1e-6)) ++wrong;
    if (!Near(f.axisX.x, in.world_axes_view[0][0], 1e-6) || !Near(f.axisY.y, in.world_axes_view[1][1], 1e-6) || !Near(f.axisZ.z, in.world_axes_view[2][2], 1e-6) ||
        !Near(f.axisZ.x, in.world_axes_view[2][0], 1e-6))
      ++wrong;
  }
  Check(wrong == 0, Num("the frame's rows as MakeFrameRows writes them are what RtHitLoadFrame reads (%.0f wrong)", wrong));

  // the hemisphere and the ambient (ShadeReference's terms)
  wrong = 0;
  for (int round = 0; round < 400; ++round) {
    const rh::FrameInput in = RandomFrame(rng, false);
    uint32_t rows[rh::kFrameRows][4];
    rh::MakeFrameRows(in, rows);
    std::vector<hlsl_shim::uint4> buffer(rh::kFrameRows);
    for (uint32_t i = 0; i < rh::kFrameRows; ++i) buffer[i] = hlsl_shim::uint4(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
    hlsl_shim::g_frame.data = buffer.data();
    hlsl_shim::g_frame.count = buffer.size();
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    float n[3] = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (float& c : n) c /= nl;
    const float up[3] = {in.world_axes_view[1][0], in.world_axes_view[1][1], in.world_axes_view[1][2]};
    const float hemisphere = std::clamp(0.6f + 0.4f * rh::Dot3(n, up), 0.2f, 1.0f);
    const float h = hlsl_shim::RtHitHemisphere(f, hlsl_shim::float3(n[0], n[1], n[2]));
    if (!Near(h, hemisphere, 1e-6)) ++wrong;
    const hlsl_shim::float3 ambient = hlsl_shim::RtHitAmbientLight(f, h);
    for (int c = 0; c < 3; ++c) {
      const float fill = std::max(in.light.ambient[c], in.light.sun[c] * rh::kFillShare);
      const float want = fill * in.tuning.ambient * hemisphere;
      const float got = c == 0 ? ambient.x : (c == 1 ? ambient.y : ambient.z);
      if (!Near(got, want, 1e-5)) ++wrong;
    }
  }
  Check(wrong == 0, Num("the hemisphere term and the ambient light are ShadeReference's (the fill is at least a fifth of the sun, the gain, the hemisphere) (%.0f wrong)", wrong));

  // the packed colour
  wrong = 0;
  for (int round = 0; round < 2000; ++round) {
    const float rgb[3] = {rng.Range(0, 2), rng.Range(0, 2), rng.Range(0, 2)};
    const uint32_t packed = rh::PackRgb10(rgb);
    float back[3];
    rh::UnpackRgb10(packed, back);
    const hlsl_shim::float3 s = hlsl_shim::RtHitRgb10(packed);
    if (!Near(s.x, back[0], 1e-6) || !Near(s.y, back[1], 1e-6) || !Near(s.z, back[2], 1e-6)) ++wrong;
  }
  Check(wrong == 0, Num("RtHitRgb10 unpacks a colour as PackRgb10 packs it (%.0f wrong)", wrong));
}

// ---------------------------------------------------------------------------
// the environment map
// ---------------------------------------------------------------------------

void EnvChecks() {
  Lcg rng;
  int wrong = 0;
  for (int round = 0; round < 5000; ++round) {
    float r[3] = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (l < 1e-3f) continue;
    for (float& c : r) c /= l;
    float u, v;
    rh::EnvUv(r, &u, &v);
    const hlsl_shim::float2 s = hlsl_shim::RtHitEnvUv(hlsl_shim::float3(r[0], r[1], r[2]));
    if (!Near(s.x, u, 1e-5) || !Near(s.y, v, 1e-5)) ++wrong;
    float back[3];
    if (!rh::EnvDirection(u, v, back) || std::fabs(back[0] - r[0]) > 2e-3f || std::fabs(back[1] - r[1]) > 2e-3f || std::fabs(back[2] - r[2]) > 2e-3f) ++wrong;
  }
  Check(wrong == 0, Num("the shader's dual paraboloid mapping is EnvUv's, and EnvDirection inverts it (%.0f wrong)", wrong));
}

// ---------------------------------------------------------------------------
// the cascades
// ---------------------------------------------------------------------------

void CascadeChecks() {
  Lcg rng;
  const uint32_t w = 64, h = 32;
  hlsl_shim::FakeTextures().clear();
  hlsl_shim::FakeTextures().resize(8);
  hlsl_shim::FakeTexture& atlas = hlsl_shim::FakeTextures()[7];
  atlas.width = w;
  atlas.height = h;
  atlas.levels.resize(1);
  atlas.levels[0].resize(size_t(w) * h);
  for (uint32_t i = 0; i < w * h; ++i) atlas.levels[0][i] = hlsl_shim::float4(rng.Range(0.0f, 1.0f), 0, 0, 0);
  hlsl_shim::FakeErrors() = 0;
  int wrong = 0, lit_mixed = 0, tested = 0;
  double worst = 0.0;
  for (int round = 0; round < 600; ++round) {
    rh::FrameInput in = RandomFrame(rng, true);
    // matrices that map the world around the camera into the atlas: u, v in 0..1, depth near the stored values
    for (uint32_t i = 0; i < in.cascades.count; ++i) {
      for (int j = 0; j < 4; ++j)
        for (int c = 0; c < 4; ++c) in.cascades.rows[i][j][c] = 0.0f;
      in.cascades.rows[i][0][0] = rng.Range(0.0005f, 0.004f);
      in.cascades.rows[i][1][1] = rng.Range(0.0005f, 0.004f);
      in.cascades.rows[i][3][0] = rng.Range(0.2f, 0.8f);
      in.cascades.rows[i][3][1] = rng.Range(0.2f, 0.8f);
      in.cascades.rows[i][2][2] = rng.Range(0.0005f, 0.003f);
      in.cascades.rows[i][3][2] = rng.Range(0.1f, 0.9f);
      in.cascades.rows[i][3][3] = 1.0f;
      in.cascades.box[i][0] = 0.0f;
      in.cascades.box[i][1] = rng.Range(100, 400);
      in.cascades.box[i][2] = 0.0f;
      in.cascades.box[i][3] = rng.Range(100, 400);
    }
    in.cascades.right[0] = 1;
    in.cascades.right[1] = 0;
    in.cascades.right[2] = 0;
    in.cascades.top[0] = 0;
    in.cascades.top[1] = 0;
    in.cascades.top[2] = 1;
    uint32_t rows[rh::kFrameRows][4];
    rh::MakeFrameRows(in, rows);
    std::vector<hlsl_shim::uint4> buffer(rh::kFrameRows);
    for (uint32_t i = 0; i < rh::kFrameRows; ++i) buffer[i] = hlsl_shim::uint4(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
    hlsl_shim::g_frame.data = buffer.data();
    hlsl_shim::g_frame.count = buffer.size();
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtHitLoadFrame();
    if ((f.flags & RT_HIT_FRAME_CASCADE) == 0) continue;
    for (int point = 0; point < 12; ++point) {
      const float p[3] = {rng.Range(-300, 300), rng.Range(-20, 40), rng.Range(-300, 300)};
      const float twin = rh::CascadeLit(in.cascades, p, w, h, [&](int x, int y) { return atlas.levels[0][size_t(y) * w + size_t(x)].x; });
      const float shader = hlsl_shim::RtHitCascadeLit(hlsl_shim::float3(p[0], p[1], p[2]));
      worst = std::max(worst, double(std::fabs(twin - shader)));
      ++tested;
      if (!Near(twin, shader, 1e-5)) ++wrong;
      if (twin > 0.05f && twin < 0.95f) ++lit_mixed;
      // the visibility at a hit: the point lifted along the normal, into the world, the collector's fade
      float view[3] = {rng.Range(-50, 50), rng.Range(-5, 20), rng.Range(1, 80)};
      float n[3] = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
      const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (float& c : n) c /= nl;
      float lifted[3], world[3];
      for (int c = 0; c < 3; ++c) lifted[c] = view[c] + n[c] * rh::kCascadeNormalOffset;
      rh::WorldPoint(in.camera_world, in.world_axes_view, lifted, world);
      const float lit = rh::CascadeLit(in.cascades, world, w, h, [&](int x, int y) { return atlas.levels[0][size_t(y) * w + size_t(x)].x; });
      const float want = rh::SunVisibility(lit, in.cascades.fade, 1.0f);
      const float got = hlsl_shim::RtHitCascadeVisible(f, hlsl_shim::float3(view[0], view[1], view[2]), hlsl_shim::float3(n[0], n[1], n[2]));
      if (!Near(got, want, 1e-4)) ++wrong;
    }
  }
  Check(tested > 1000 && lit_mixed > 50, Num("the cascade lookup was tried on %.0f points, with mixed lit values among them", tested));
  Check(wrong == 0, Num("RtHitCascadeLit is CascadeLit's, and RtHitCascadeVisible is the visibility at the lifted world point with the collector's fade (%.0f wrong)", wrong));
  Check(hlsl_shim::FakeErrors() == 0, "no atlas read outside the atlas");
}

// ---------------------------------------------------------------------------
// a texture read by slot
// ---------------------------------------------------------------------------

void SampleChecks() {
  Lcg rng;
  // the game's gamma: the curve's end points, its joins and its steps
  CHECK(Near(hlsl_shim::RtHitPwl(0.0f), 0.0f) && Near(hlsl_shim::RtHitPwl(1.0f), 1.0f, 1e-6));
  CHECK(Near(hlsl_shim::RtHitPwl(128.0f / 255.0f), 257.0f / 1023.0f, 1e-5));  // 128 of 255: 128 x 1024 x 4/1024 - 256 = 256, plus 1
  bool monotone = true, continuous = true;
  float previous = 0.0f;
  for (int i = 0; i <= 255 * 8; ++i) {
    const float g = float(i) / (255.0f * 8.0f);
    const float v = hlsl_shim::RtHitPwl(g);
    monotone = monotone && v + 1e-6f >= previous;
    continuous = continuous && std::fabs(v - previous) < 0.02f;
    previous = v;
    if (!Near(v, RefPwl(g), 1e-6)) monotone = false;
  }
  CHECK(monotone && continuous);

  // a fake texture set: a coloured noise chain, three sizes, one non square
  hlsl_shim::FakeTextures().clear();
  const uint32_t sizes[3][2] = {{32, 32}, {16, 8}, {1, 1}};
  for (const auto& size : sizes) {
    hlsl_shim::FakeTexture t;
    t.width = size[0];
    t.height = size[1];
    for (uint32_t l = 0;; ++l) {
      const uint32_t lw = std::max(size[0] >> l, 1u), lh = std::max(size[1] >> l, 1u);
      std::vector<hlsl_shim::float4> level(size_t(lw) * lh);
      for (auto& v : level) v = hlsl_shim::float4(rng.Next(), rng.Next(), rng.Next(), rng.Next());
      t.levels.push_back(level);
      if (lw == 1 && lh == 1) break;
    }
    hlsl_shim::FakeTextures().push_back(t);
  }
  hlsl_shim::FakeErrors() = 0;
  int wrong = 0;
  double worst = 0.0;
  for (int round = 0; round < 6000; ++round) {
    const uint32_t slot = rng.Below(3);
    const bool gamma = rng.Below(2) != 0, wrap = rng.Below(2) != 0;
    const float u = rng.Range(-2.5f, 3.5f), v = rng.Range(-2.5f, 3.5f), lod = rng.Range(-1.0f, 8.0f);
    const hlsl_shim::FakeTexture& t = hlsl_shim::FakeTextures()[slot];
    const Rgba want = RefSample(t, u, v, lod, wrap, gamma);
    const hlsl_shim::float4 got = hlsl_shim::RtHitSample(slot | (gamma ? 0x80000000u : 0u), hlsl_shim::float2(u, v), lod, wrap);
    const float g[4] = {got.x, got.y, got.z, got.w};
    for (int k = 0; k < 4; ++k) {
      worst = std::max(worst, double(std::fabs(g[k] - want.c[k])));
      if (std::fabs(g[k] - want.c[k]) > 1e-4f) ++wrong;
    }
  }
  Check(wrong == 0, Num("RtHitSample is a bilinear two level read with wrap or clamp and the game's gamma (worst difference %.2e)", worst));
  Check(hlsl_shim::FakeErrors() == 0, "no texel read outside its level");
}

// ---------------------------------------------------------------------------
// the text: the car's hit shading reads the frame through the shared names
// ---------------------------------------------------------------------------

void TextChecks(const std::string& src) {
  const std::string car = ReadFile(src + "/gpu/hlsl/car_rt_hit_shade.hlsli");
  const std::string frame = ReadFile(src + "/gpu/hlsl/rt_hit_frame.hlsli");
  const std::string sample = ReadFile(src + "/gpu/hlsl/rt_hit_sample.hlsli");
  const std::string common = ReadFile(src + "/gpu/hlsl/rt_hit_common.hlsli");
  if (car.empty() || frame.empty() || sample.empty()) {
    std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    return;
  }
  const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
  CHECK(has(car, "#define RT_HIT_FRAME_ROW(i) gCarRtMaterials[i]") && has(car, "#define RT_HIT_TLAS gCarRtTlas") && has(car, "#include \"rt_hit_frame.hlsli\"") && has(car, "#include \"rt_hit_sample.hlsli\""));
  CHECK(has(common, "#include \"rt_hit_frame.hlsli\"") && has(common, "#include \"rt_hit_sample.hlsli\""));
  CHECK(has(sample, "#include \"rt_hit_frame.hlsli\""));
  CHECK(!has(car, "CarMatSample") && !has(car, "CarMatCascadeLit") && !has(car, "CarMatFrame") && !has(car, "CAR_MAT_FRAME_"));  // nothing of the moved code is left behind under its old names
  // the moved functions are in their new files, once
  for (const char* name : {"RtHitLoadFrame(", "RtHitRgb10(", "RtHitHemisphere(", "RtHitAmbientLight("}) CHECK(has(frame, name) && !has(sample, name));
  for (const char* name : {"RtHitPwl(", "RtHitTexel(", "RtHitBilinear(", "RtHitSample(", "RtHitEnvUv(", "RtHitLoadDepth(", "RtHitCascadeLit(", "RtHitSunVisible(", "RtHitCascadeVisible(", "RtHitEnvDirection(",
                           "RtHitEnvRead(", "RtHitEnvAt("})
    CHECK(has(sample, name) && !has(frame, name));
}

// ---------------------------------------------------------------------------
// the calibration
// ---------------------------------------------------------------------------

void CalibrationChecks() {
  // the car's numbers are unchanged and its census forwards to the shared arithmetic
  CHECK(hr::census::kLogFloor == 1e-4f && hr::census::kLogBias == 16.0f && hr::census::kLogScale == 64.0f && hr::census::kLogMax == 2047u && hr::census::kCalFloor == 0.02f &&
        hr::census::kCalMinSamples == 48u && hr::census::kCalMinStops == -3.0f && hr::census::kCalMaxStops == 3.0f && hr::census::kCalRate == 0.5f);
  CHECK(hr::census::kLuma[0] == 0.2126f && hr::census::kLuma[1] == 0.7152f && hr::census::kLuma[2] == 0.0722f);
  Lcg rng;
  int wrong = 0;
  for (int i = 0; i < 4000; ++i) {
    const float l = std::exp2(rng.Range(-20.0f, 6.0f));
    if (hr::census::FixedLog2(l) != hc::FixedLog2(l)) ++wrong;
    const uint64_t count = rng.Below(200), seen = uint64_t(rng.Below(2047)) * count, model = uint64_t(rng.Below(2047)) * count;
    float a = 99, b = 99;
    const bool ra = hr::census::CalibrationStops(count, seen, model, &a), rb = hc::Stops(hc::kCarParams, count, seen, model, &b);
    if (ra != rb || (ra && a != b)) ++wrong;
    const float prev = rng.Range(-3, 3), res = rng.Range(-8, 8);
    if (hr::census::NextStops(prev, res) != hc::NextStops(hc::kCarParams, prev, res) || hr::census::GainOf(prev) != hc::GainOf(hc::kCarParams, prev)) ++wrong;
  }
  Check(wrong == 0, Num("the car's census gives what the shared calibration gives with the car's parameters (%.0f wrong)", wrong));

  // the fixed point log: a stop is 64 steps, the floor and the top
  CHECK(hc::FixedLog2(1.0f) == 16u * 64u && hc::FixedLog2(2.0f) == 17u * 64u && hc::FixedLog2(0.5f) == 15u * 64u && hc::FixedLog2(0.0f) == hc::FixedLog2(1e-4f) && hc::FixedLog2(1e9f) == hc::kLogMax);
  CHECK(Near(hc::FromFixedLog2(float(hc::FixedLog2(0.25f))), 0.25, 1e-3));
  const float rgb[3] = {1, 0, 0};
  CHECK(Near(hc::Luminance(rgb), 0.2126));

  // the gain's range: the car's 1/8 to 8, the city's 0.5 to 3
  CHECK(Near(hc::GainOf(hc::kCarParams, 5.0f), 8.0) && Near(hc::GainOf(hc::kCarParams, -5.0f), 0.125));
  CHECK(Near(hc::GainOf(hc::kCityParams, 5.0f), 3.0, 1e-4) && Near(hc::GainOf(hc::kCityParams, -5.0f), 0.5, 1e-4) && Near(hc::GainOf(hc::kCityParams, 0.0f), 1.0));
  float stops = 0;
  CHECK(!hc::Stops(hc::kCityParams, 47, 47 * 1000, 47 * 900, &stops) && hc::Stops(hc::kCityParams, 48, 48 * 1000, 48 * 900, &stops));
  CHECK(Near(stops, 100.0 / 64.0, 1e-5));  // the picture is 100/64 stops over the model

  // per class gains: a model too dark by a stop in one class and right in another; the gains settle where the residual is 0, each in its own class
  hc::ClassGains gains;
  CHECK(Near(gains.Gain(1), 1.0) && Near(gains.Gain(15), 1.0) && Near(gains.Gain(99), 1.0));
  const uint32_t dark_class = 2, right_class = 5, few_class = 11;
  float model_stops[hc::kClasses];  // the model's error in stops (picture over model BEFORE any gain)
  for (float& m : model_stops) m = 0.0f;
  model_stops[dark_class] = 1.0f;
  model_stops[few_class] = 1.2f;
  for (int window = 0; window < 40; ++window) {
    uint32_t words[hc::kStatsWords] = {};
    for (uint32_t c = 0; c < hc::kClasses; ++c) {
      const uint32_t samples = (c == dark_class || c == right_class) ? 300 : (c == few_class ? 10 : 0);
      // the shader shades the samples with the gain in force: what is left to correct is the model's error minus the gain, in stops
      const float residual = model_stops[c] - std::log2(gains.Gain(c));
      for (uint32_t s = 0; s < samples; ++s) {
        const uint32_t model_log = hc::FixedLog2(0.2f);
        const uint32_t seen_log = uint32_t(std::lround(float(model_log) + residual * hc::kLogScale));
        words[hc::StatsWord(c, hc::kCountWord)] += 1;
        words[hc::StatsWord(c, hc::kSeenWord)] += seen_log;
        words[hc::StatsWord(c, hc::kModelWord)] += model_log;
      }
    }
    gains.Add(words);
    gains.Close();
  }
  CHECK(Near(std::log2(gains.Gain(dark_class)), 1.0, 0.02));
  CHECK(Near(gains.Gain(right_class), 1.0, 0.02));
  CHECK(Near(gains.Gain(few_class), 1.0) && Near(gains.Gain(0), 1.0));  // 10 samples a window: never enough: the gain stays
  // an error beyond the range is clamped, not chased
  hc::ClassGains clamped;
  model_stops[dark_class] = 4.0f;
  for (int window = 0; window < 60; ++window) {
    uint32_t words[hc::kStatsWords] = {};
    const float residual = model_stops[dark_class] - std::log2(clamped.Gain(dark_class));
    for (uint32_t s = 0; s < 100; ++s) {
      const uint32_t model_log = hc::FixedLog2(0.2f);
      words[hc::StatsWord(dark_class, hc::kCountWord)] += 1;
      words[hc::StatsWord(dark_class, hc::kSeenWord)] += uint32_t(std::lround(float(model_log) + residual * hc::kLogScale));
      words[hc::StatsWord(dark_class, hc::kModelWord)] += model_log;
    }
    clamped.Add(words);
    clamped.Close();
  }
  CHECK(Near(clamped.Gain(dark_class), 3.0, 1e-3));
  float table[hc::kClasses];
  gains.Table(table);
  CHECK(Near(table[dark_class], gains.Gain(dark_class)) && Near(table[0], 1.0));
  CHECK(hc::kStatsWords == 48 && hc::kStatsBytes == 192 && hc::StatsWord(3, hc::kSeenWord) == 10 && hc::StatsWord(99, hc::kCountWord) == 45);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "src";
  FrameChecks();
  EnvChecks();
  CascadeChecks();
  SampleChecks();
  TextChecks(src);
  CalibrationChecks();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
