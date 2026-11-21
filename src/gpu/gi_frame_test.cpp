// Check of the probe field's frame interface (gi_frame.h and hlsl/gi_common.hlsli): CITY-RT M3 package G0. A program of its own: it needs no renderer, no game files and no GPU, and
// exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_frame_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_frame_test.cpp
//   run: larecomp_gi_frame_test [-v] [<hlsl/gi_common.hlsli>]   (without the path the .hlsli is found from this file's own place: hlsl/; -v prints every check with its numbers)
//
// It checks: the budgets' numbers against M3 section 6 and the cells of the (tier, level) table; the toroidal addressing (the slot of every coordinate, the tiles of a cascade
// tile the atlas without overlap, the boxes a move exposes are exactly the coordinates that entered the window, whatever the move); the octahedral map against the G-buffer's and the
// border's continuity; the integration of rays into a texel against closed forms (a uniform environment, an open ground, a sky); the ground baseline against an integrated open ground; the
// rows of a frame; and the lookup, on synthetic fields whose answer is known (a uniform field, a cosine lobe, a gradient, a wall between probes with and without the moments, the
// blend between cascades, a cascade whose probes are off, a field scrolled by the toroidal moves against one built from scratch, the range fade, the ambient). Then it compiles the .hlsli
// ITSELF as C++ (through a shim of the HLSL vector types and of the four resources it reads) and compares each of its functions with the twin on random frames and random points, and
// compares every numeric #define of the .hlsli with the C++ value.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_rules.h"
#include "gi_frame.h"

namespace gi = mcla::gpu::gi;
namespace gb = mcla::gpu::gbuffer;

// ---------------------------------------------------------------------------
// the textures of the field, on the CPU: a Source for gi_frame.h's lookup and the resources the .hlsli shim reads
// ---------------------------------------------------------------------------
struct Atlas {
  uint32_t w = 0, h = 0;    // the irradiance and moments atlases
  uint32_t ww = 0, wh = 0;  // the weight map
  std::vector<float> irr;   // 4 floats a texel
  std::vector<float> mom;   // 2 floats a texel
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
  // A bilinear sample with clamp addressing at the texture's 0..1 coordinates (texel centres at +0.5), in float.
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
  void SetTexel(uint32_t x, uint32_t y, gi::Rgba e, gi::Vec2 m) {
    float* a = &irr[(size_t(y) * w + x) * 4];
    a[0] = e.r;
    a[1] = e.g;
    a[2] = e.b;
    a[3] = e.a;
    float* b = &mom[(size_t(y) * w + x) * 2];
    b[0] = m.x;
    b[1] = m.y;
  }
};

const Atlas* g_atlas = nullptr;  // what the shim's macros read

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
// ---------------------------------------------------------------------------
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
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
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
#undef GI_FRAME_ROW
#undef GI_SAMPLE_IRRADIANCE
#undef GI_SAMPLE_MOMENTS
#undef GI_LOAD_PROBE_WEIGHT
}  // namespace hlsl_shim

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;  // -v: every check's line, with the measured numbers

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

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }  // 0 .. 1
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : (U32() >> 8) % n; }
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

bool Near(double a, double b, double abs_tol, double rel_tol = 0.0) { return std::fabs(a - b) <= abs_tol + rel_tol * std::max(std::fabs(a), std::fabs(b)); }
bool NearV(const gi::Vec3& a, const gi::Vec3& b, double abs_tol, double rel_tol = 0.0) {
  return Near(a.x, b.x, abs_tol, rel_tol) && Near(a.y, b.y, abs_tol, rel_tol) && Near(a.z, b.z, abs_tol, rel_tol);
}

// n rays spread uniformly over the sphere (the spherical Fibonacci set), unit
std::vector<gi::Vec3> FibonacciDirections(uint32_t n) {
  std::vector<gi::Vec3> out(n);
  const double golden = 0.5 * (1.0 + std::sqrt(5.0));
  for (uint32_t i = 0; i < n; ++i) {
    const double z = 1.0 - (2.0 * i + 1.0) / n;
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double phi = 2.0 * 3.14159265358979323846 * std::fmod(i / golden, 1.0);
    out[i] = {float(r * std::cos(phi)), float(z), float(r * std::sin(phi))};
  }
  return out;
}

// ---------------------------------------------------------------------------
// the budgets and the cells of the table
// ---------------------------------------------------------------------------
void BudgetChecks() {
  using gi::Layout;
  const gi::Budget& lean = gi::BudgetOf(Layout::kLean);
  const gi::Budget& standard = gi::BudgetOf(Layout::kStandard);
  const gi::Budget& extended = gi::BudgetOf(Layout::kExtended);
  const gi::Budget& none = gi::BudgetOf(Layout::kNone);
  // M3 section 6, and city_hit_format.h's HitBudget (gi_cascades, gi_rays_per_probe, gi_rays_per_frame, gi_pixel_pass)
  Check(none.cascade_count == 0 && gi::BudgetRaysPerFrame(none) == 0 && gi::AtlasBytes(none) == 0, "no layout: no cascades, no rays, no memory");
  Check(lean.cascade_count == 2 && standard.cascade_count == 3 && extended.cascade_count == 4, "2 / 3 / 4 cascades");
  Check(lean.cascade[0].rays_per_probe == 32 && lean.cascade[1].rays_per_probe == 32, "Mid: 32 rays a probe");
  for (uint32_t c = 0; c < 3; ++c) Check(standard.cascade[c].rays_per_probe == 64, "High: 64 rays a probe");
  for (uint32_t c = 0; c < 4; ++c) Check(extended.cascade[c].rays_per_probe == 64, "Ultra: 64 rays a probe");
  Check(gi::BudgetRaysPerFrame(lean) == 12288 && gi::BudgetRaysPerFrame(standard) == 458752 && gi::BudgetRaysPerFrame(extended) == 983040, "12 K, 459 K, 983 K rays a frame");
  Check(lean.cascade[0].interval_frames == 8 && lean.cascade[1].interval_frames == 16, "Mid refreshes every 8 and 16 frames");
  Check(standard.cascade[0].interval_frames == 4 && standard.cascade[1].interval_frames == 8 && standard.cascade[2].interval_frames == 16, "High: 4, 8, 16");
  Check(extended.cascade[0].interval_frames == 2 && extended.cascade[1].interval_frames == 4 && extended.cascade[2].interval_frames == 8 && extended.cascade[3].interval_frames == 16, "Ultra: 2, 4, 8, 16");
  Check(lean.cascade[0].dims == gi::Dims{16, 8, 16} && lean.cascade[0].spacing_m == 3.0f && lean.cascade[1].spacing_m == 6.0f, "Mid: 16 x 8 x 16 at 3 and 6 m");
  Check(standard.cascade[0].dims == gi::Dims{32, 16, 32} && standard.cascade[0].spacing_m == 2.0f && standard.cascade[1].spacing_m == 4.0f && standard.cascade[2].spacing_m == 8.0f,
        "High: 32 x 16 x 32 at 2, 4, 8 m");
  Check(extended.pixel_rays_per_pixel == 1 && standard.pixel_rays_per_pixel == 0 && lean.pixel_rays_per_pixel == 0, "the per-pixel pass only at Ultra");
  // memory: 1.2 KB a probe, 19.66 MB a High cascade
  Check(gi::kProbeAtlasBytes == 1200, "a probe is 1.2 KB of atlas");
  Check(Near(double(standard.cascade[0].dims.Probes()) * gi::kProbeAtlasBytes / 1e6, 19.66, 0.01), "a High cascade is 19.7 MB");
  Check(Near(double(gi::AtlasBytes(standard)) / 1e6, 59.0, 0.1) && Near(double(gi::AtlasBytes(extended)) / 1e6, 78.7, 0.1) && Near(double(gi::AtlasBytes(lean)) / 1e6, 4.9, 0.1),
        Num("the atlases: High %.1f MB (59), Ultra %.1f MB (79), Mid %.1f MB (5)", gi::AtlasBytes(standard) / 1e6, gi::AtlasBytes(extended) / 1e6, gi::AtlasBytes(lean) / 1e6));
  // every cascade of a budget: every tile of every cascade has a distinct place in the atlas, inside it
  for (Layout layout : {Layout::kLean, Layout::kStandard, Layout::kExtended}) {
    const gi::Budget& b = gi::BudgetOf(layout);
    const uint32_t aw = gi::AtlasWidth(b), ah = gi::AtlasHeight(b);
    std::vector<uint8_t> used(size_t(aw) * ah, 0);
    std::vector<uint8_t> wused(size_t(gi::WeightWidth(b)) * gi::WeightHeight(b), 0);
    bool overlap = false, outside = false, wover = false;
    for (uint32_t c = 0; c < b.cascade_count; ++c) {
      const gi::Dims d = b.cascade[c].dims;
      for (uint32_t z = 0; z < d.z; ++z)
        for (uint32_t y = 0; y < d.y; ++y)
          for (uint32_t x = 0; x < d.x; ++x) {
            const gi::Texel t = gi::TileOrigin({x, y, z}, d);
            const uint32_t x0 = t.x, y0 = gi::AtlasRow(b, c) + t.y;
            if (x0 + gi::kProbeTile > aw || y0 + gi::kProbeTile > ah) {
              outside = true;
              continue;
            }
            for (uint32_t j = 0; j < gi::kProbeTile; ++j)
              for (uint32_t i = 0; i < gi::kProbeTile; ++i) {
                uint8_t& u = used[size_t(y0 + j) * aw + x0 + i];
                if (u) overlap = true;
                u = 1;
              }
            const gi::Texel wt = gi::WeightTexel({x, y, z}, d);
            uint8_t& wu = wused[size_t(gi::WeightRow(b, c) + wt.y) * gi::WeightWidth(b) + wt.x];
            if (wu) wover = true;
            wu = 1;
          }
    }
    Check(!overlap && !outside && !wover, std::string("the tiles of every probe of every cascade are distinct and inside the atlas: ") + std::string(gi::LayoutName(layout)));
  }
  // the cells of the (tier, level) table, every one
  using gi::Level;
  using gi::Rung;
  const Layout expect[3][3] = {{Layout::kNone, Layout::kNone, Layout::kNone},          // none: off, high, ultra
                               {Layout::kNone, Layout::kLean, Layout::kStandard},      // weak
                               {Layout::kNone, Layout::kStandard, Layout::kExtended}};  // strong
  for (int r = 0; r < 3; ++r)
    for (int l = 0; l < 3; ++l)
      Check(gi::LayoutFor(Rung(r), Level(l)) == expect[r][l], std::string("the cell: ") + std::string(gi::RungName(Rung(r))) + " / " + std::string(gi::LevelName(Level(l))));
  Check(gi::AutoLevel(Rung::kStrong) == Level::kHigh && gi::AutoLevel(Rung::kWeak) == Level::kOff && gi::AutoLevel(Rung::kNone) == Level::kOff, "auto: HIGH on a strong card only (rt_tier.h kRtGi)");
  for (int r = 0; r < 3; ++r)
    for (int l = 0; l < 3; ++l) {
      const Layout layout = gi::LayoutFor(Rung(r), Level(l));
      Check(gi::PixelPassAllowed(Rung(r), layout) == (r == 2 && l == 2), "the per-pixel pass is the strong card's ULTRA layer only");
      Check(gi::ReplacesSsgi(layout) == (layout != Layout::kNone), "the field replaces the screen-space bounce light exactly where it runs");
    }
  // the words: no level called low, `auto` is not a level
  Level parsed = Level::kOff;
  Check(gi::ParseLevel("off", &parsed) && parsed == Level::kOff && gi::ParseLevel("high", &parsed) && parsed == Level::kHigh && gi::ParseLevel("ultra", &parsed) && parsed == Level::kUltra, "the level words");
  Check(!gi::ParseLevel("auto", &parsed) && !gi::ParseLevel("low", &parsed) && !gi::ParseLevel("medium", &parsed) && !gi::ParseLevel("", &parsed), "auto and low are not levels");
  Check(gi::LevelName(Level::kHigh) == "high" && gi::RungName(Rung::kWeak) == "weak" && gi::LayoutName(Layout::kExtended) == "extended", "the names");
  // the costs: positive where a layout exists, growing, the 3060's ultra marked not recommended
  Check(lean.cost.ms_5080_4k_lo > 0 && lean.cost.ms_5080_4k_lo <= lean.cost.ms_5080_4k_hi && standard.cost.ms_3060_1080p_lo > 0 && extended.cost.ms_3060_1080p_hi == 0.0f, "the estimates are filled, the 3060's ultra is 'not recommended' (0)");
}

// ---------------------------------------------------------------------------
// the addressing
// ---------------------------------------------------------------------------
void AddressingChecks() {
  Lcg rng;
  // Wrap is the mathematical mod
  bool wrap_ok = true;
  for (int32_t c = -200; c <= 200; ++c)
    for (uint32_t d : {1u, 2u, 7u, 16u, 32u}) {
      const int32_t m = ((c % int32_t(d)) + int32_t(d)) % int32_t(d);
      if (gi::Wrap(c, d) != uint32_t(m)) wrap_ok = false;
    }
  Check(wrap_ok, "Wrap is the mathematical mod for negative coordinates");
  // the slots of the window are a bijection onto the slot grid, for any origin (negative included)
  for (int trial = 0; trial < 60; ++trial) {
    const gi::Dims d = {1 + rng.Below(33), 1 + rng.Below(17), 1 + rng.Below(33)};
    const gi::Coord o = {int32_t(rng.Below(4000)) - 2000, int32_t(rng.Below(400)) - 200, int32_t(rng.Below(4000)) - 2000};
    std::vector<uint8_t> seen(d.Probes(), 0);
    bool ok = true;
    for (int32_t z = 0; z < int32_t(d.z); ++z)
      for (int32_t y = 0; y < int32_t(d.y); ++y)
        for (int32_t x = 0; x < int32_t(d.x); ++x) {
          const gi::Slot s = gi::SlotOf({o.x + x, o.y + y, o.z + z}, d);
          if (s.x >= d.x || s.y >= d.y || s.z >= d.z) ok = false;
          const uint32_t index = gi::ProbeIndex(s, d);
          if (index >= d.Probes() || seen[index]++) ok = false;
        }
    Check(ok, "the slots of a window are distinct and inside the grid");
  }
  // the origin: moves in whole cells, centred on the camera, the y centre 8 m under it
  const gi::CascadeSpec spec = gi::BudgetOf(gi::Layout::kStandard).cascade[0];
  {
    const double cam[3] = {100.3, 20.0, -55.7};
    const gi::Coord o = gi::OriginFor(cam, spec);
    Check(o.x == 50 - 16 && o.z == -28 - 16, "the origin is the camera's cell less half the window (x, z)");
    Check(o.y == int32_t(std::floor((20.0 - 8.0) / 2.0)) - 8, "... and the y centre is 8 m under the camera");
    const double cam2[3] = {101.9, 20.0, -55.7};  // same cell in x: 50.95 -> 50
    Check(gi::OriginFor(cam2, spec) == o, "a move inside a cell does not move the window");
    const double cam3[3] = {102.0, 20.0, -55.7};
    Check(gi::OriginFor(cam3, spec).x == o.x + 1, "crossing a cell boundary moves it by exactly one cell");
  }
  // Contains / ProbeWorld
  Check(gi::Contains({-5, 0, 0}, {8, 4, 8}, {-5, 0, 0}) && !gi::Contains({-5, 0, 0}, {8, 4, 8}, {3, 0, 0}) && gi::Contains({-5, 0, 0}, {8, 4, 8}, {2, 3, 7}) && !gi::Contains({-5, 0, 0}, {8, 4, 8}, {-6, 0, 0}),
        "Contains is the half open window");
  Check(Near(gi::ProbeWorld({-3, 4, 1}, 2.0f).x, -6.0, 1e-9) && Near(gi::ProbeWorld({-3, 4, 1}, 2.0f).y, 8.0, 1e-9), "a probe sits at its coordinate times the spacing");

  // the exposed boxes are exactly the coordinates of the new window that were not in the old one, each in one box
  int moves = 0;
  bool all_ok = true;
  uint64_t worst_count = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const gi::Dims d = {2 + rng.Below(10), 2 + rng.Below(6), 2 + rng.Below(10)};
    gi::Coord o = {int32_t(rng.Below(60)) - 30, int32_t(rng.Below(60)) - 30, int32_t(rng.Below(60)) - 30};
    gi::Coord n = o;
    const uint32_t kind = rng.Below(4);
    const int32_t reach = kind == 0 ? 1 : kind == 1 ? 3 : kind == 2 ? 12 : 40;  // a step, a few cells, up to a whole window, a cut
    n.x += int32_t(rng.Below(2 * reach + 1)) - reach;
    n.y += int32_t(rng.Below(2 * reach + 1)) - reach;
    n.z += int32_t(rng.Below(2 * reach + 1)) - reach;
    if (trial % 7 == 0) n = o;  // no move
    gi::Box boxes[3];
    const uint32_t count = gi::ExposedBoxes(o, n, d, boxes);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += boxes[i].Count();
    uint64_t want = 0;
    for (int32_t z = n.z - 5; z < n.z + int32_t(d.z) + 5; ++z)
      for (int32_t y = n.y - 5; y < n.y + int32_t(d.y) + 5; ++y)
        for (int32_t x = n.x - 5; x < n.x + int32_t(d.x) + 5; ++x) {
          const gi::Coord c = {x, y, z};
          const bool is_new = gi::Contains(n, d, c) && !gi::Contains(o, d, c);
          int in_boxes = 0;
          for (uint32_t i = 0; i < count; ++i) in_boxes += boxes[i].Has(c) ? 1 : 0;
          if (is_new) ++want;
          if (in_boxes != (is_new ? 1 : 0)) all_ok = false;
        }
    if (sum != want || count > 3 || gi::ExposedProbes(o, n, d) != want) all_ok = false;
    for (uint32_t i = 0; i < count; ++i)
      if (boxes[i].Empty()) all_ok = false;
    worst_count = std::max<uint64_t>(worst_count, count);
    ++moves;
  }
  Check(all_ok, Num("the boxes a move exposes are exactly the new coordinates, each in one box, at most three (%.0f moves)", moves));
  Check(gi::ExposedProbes({0, 0, 0}, {0, 0, 0}, {32, 16, 32}) == 0, "no move exposes nothing");
  Check(gi::ExposedProbes({0, 0, 0}, {1, 0, 0}, {32, 16, 32}) == 16 * 32, "one cell in x exposes a slab of 16 x 32 probes");
  Check(gi::ExposedProbes({0, 0, 0}, {1, 1, 1}, {32, 16, 32}) == 16 * 32 + 31 * 32 + 31 * 15, "a diagonal step exposes three slabs without overlap");
  Check(gi::ExposedProbes({0, 0, 0}, {40, 0, 0}, {32, 16, 32}) == 16384 && gi::ExposedProbes({0, 0, 0}, {0, -16, 0}, {32, 16, 32}) == 16384, "a move of a whole window or more is the whole window (a cut)");
}

// ---------------------------------------------------------------------------
// the octahedral map
// ---------------------------------------------------------------------------
void OctChecks() {
  Lcg rng;
  double worst = 0.0;
  int differs_from_gbuffer = 0, hlsl_differs = 0;
  for (int i = 0; i < 20000; ++i) {
    const gi::Vec3 d = rng.Direction();
    const gi::Vec2 e = gi::OctEncode(d);
    const gi::Vec3 back = gi::OctDecode(e);
    worst = std::max(worst, double(std::fabs(back.x - d.x) + std::fabs(back.y - d.y) + std::fabs(back.z - d.z)));
    const gb::Vec2 g = gb::OctEncode({d.x, d.y, d.z});
    if (g.x != e.x || g.y != e.y) ++differs_from_gbuffer;
    const hlsl_shim::float2 h = hlsl_shim::GiOctEncode(hlsl_shim::float3(d.x, d.y, d.z));
    const hlsl_shim::float3 hb = hlsl_shim::GiOctDecode(hlsl_shim::float2(e.x, e.y));
    if (h.x != e.x || h.y != e.y || hb.x != back.x || hb.y != back.y || hb.z != back.z) ++hlsl_differs;
    if (e.x < 0 || e.x > 1 || e.y < 0 || e.y > 1) ++differs_from_gbuffer;
  }
  Check(worst < 1e-5, Num("the octahedral map round-trips a direction (worst error %.1e)", worst));
  Check(differs_from_gbuffer == 0, "the map is the G-buffer's, bit for bit, and stays in 0..1");
  Check(hlsl_differs == 0, "the HLSL twin's map is the C++ one's, bit for bit");
  // the axes land where the tile's border logic expects them
  Check(Near(gi::OctEncode({0, 0, 1}).x, 0.5, 1e-6) && Near(gi::OctEncode({0, 0, 1}).y, 0.5, 1e-6), "the +z axis is the middle of the map");
  Check(Near(gi::OctEncode({1, 0, 0}).x, 1.0, 1e-6) && Near(gi::OctEncode({-1, 0, 0}).x, 0.0, 1e-6), "+x and -x are the middles of the left and right edges");

  // the border: a border texel's value is its source's, and the source is the direction the border's own position folds to (the map continues across the seam)
  bool border_ok = true;
  double worst_border = 0.0;
  for (uint32_t ty = 0; ty < gi::kProbeTile; ++ty)
    for (uint32_t tx = 0; tx < gi::kProbeTile; ++tx) {
      const bool interior = tx >= 1 && tx <= 8 && ty >= 1 && ty <= 8;
      const gi::Texel src = gi::OctBorderSource(tx, ty);
      if (src.x < 1 || src.x > 8 || src.y < 1 || src.y > 8) border_ok = false;
      if (interior) {
        if (!(src == gi::Texel{tx, ty})) border_ok = false;
        continue;
      }
      float u = (float(tx) - 1.0f + 0.5f) / 8.0f, v = (float(ty) - 1.0f + 0.5f) / 8.0f;  // outside 0..1
      if (u < 0.0f) { u = -u; v = 1.0f - v; } else if (u > 1.0f) { u = 2.0f - u; v = 1.0f - v; }
      if (v < 0.0f) { v = -v; u = 1.0f - u; } else if (v > 1.0f) { v = 2.0f - v; u = 1.0f - u; }
      const gi::Vec3 folded = gi::OctDecode({u, v});
      const gi::Vec3 source = gi::TexelDirection(src.x, src.y);
      const double err = std::fabs(folded.x - source.x) + std::fabs(folded.y - source.y) + std::fabs(folded.z - source.z);
      worst_border = std::max(worst_border, err);
      if (err > 1e-5) border_ok = false;
    }
  Check(border_ok, Num("every border texel copies the interior texel its position folds to (worst direction error %.1e)", worst_border));
  // the tile's interior texels: the directions are distinct and cover the sphere (each of the 64 texels is the centre of its own 1/64 of the map)
  std::vector<gi::Vec3> dirs;
  for (uint32_t ty = 1; ty <= 8; ++ty)
    for (uint32_t tx = 1; tx <= 8; ++tx) dirs.push_back(gi::TexelDirection(tx, ty));
  float closest = 4.0f;
  for (size_t a = 0; a < dirs.size(); ++a)
    for (size_t b = a + 1; b < dirs.size(); ++b) closest = std::min(closest, 1.0f - gi::Dot(dirs[a], dirs[b]));
  Check(closest > 0.01f, Num("the 64 texel directions are distinct (closest pair 1 - cos = %.4f)", closest));
}

// ---------------------------------------------------------------------------
// the integration of rays into texels
// ---------------------------------------------------------------------------
void IntegrationChecks() {
  const std::vector<gi::Vec3> dirs = FibonacciDirections(2048);
  const float max_d = 12.0f;
  std::vector<gi::RayResult> rays(dirs.size());

  // a uniform environment of radiance L that every ray hits: E = L everywhere, no sky
  const gi::Vec3 L = {0.5f, 0.25f, 1.0f};
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = {dirs[i], L, 5.0f, false};
  double worst = 0.0;
  for (uint32_t ty = 1; ty <= 8; ++ty)
    for (uint32_t tx = 1; tx <= 8; ++tx) {
      const gi::TexelValue v = gi::IntegrateTexel(gi::TexelDirection(tx, ty), rays.data(), uint32_t(rays.size()), max_d);
      worst = std::max({worst, double(std::fabs(v.irradiance.x - L.x)), double(std::fabs(v.irradiance.y - L.y) * 2), double(std::fabs(v.irradiance.z - L.z)), double(v.sky_visibility)});
      if (!Near(v.mean_distance, 5.0, 1e-3) || !Near(v.mean_distance_sq, 25.0, 1e-2)) worst = 9.0;
    }
  Check(worst < 0.01, Num("a closed uniform environment of radiance L gives E = L (the units have no pi), no sky, distance 5 (worst error %.4f)", worst));

  // every ray escapes: no irradiance, the whole lobe sees the sky, the distance is the clamp
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = {dirs[i], {0, 0, 0}, 0.0f, true};
  const gi::TexelValue open = gi::IntegrateTexel(gi::TexelDirection(3, 6), rays.data(), uint32_t(rays.size()), max_d);
  Check(open.irradiance.x == 0.0f && Near(open.sky_visibility, 1.0, 0.01) && Near(open.mean_distance, max_d, 1e-4) && Near(open.mean_distance_sq, max_d * max_d, 1e-2), "an open sky: E = 0, sky visibility 1, distance = the clamp");

  // an open ground: the rays that go down hit radiance G, the rest escape: E(N) = G (1 - N.y) / 2 and the sky is (1 + N.y) / 2
  const gi::Vec3 G = {0.8f, 0.6f, 0.2f};
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = dirs[i].y < 0 ? gi::RayResult{dirs[i], G, 4.0f / std::max(-dirs[i].y, 0.05f), false} : gi::RayResult{dirs[i], {0, 0, 0}, 0.0f, true};
  worst = 0.0;
  for (uint32_t ty = 1; ty <= 8; ++ty)
    for (uint32_t tx = 1; tx <= 8; ++tx) {
      const gi::Vec3 n = gi::TexelDirection(tx, ty);
      const gi::TexelValue v = gi::IntegrateTexel(n, rays.data(), uint32_t(rays.size()), max_d);
      const float share = 0.5f * (1.0f - n.y);
      worst = std::max({worst, double(std::fabs(v.irradiance.x - G.x * share)), double(std::fabs(v.irradiance.y - G.y * share)), double(std::fabs(v.sky_visibility - (1.0f - share)))});
    }
  Check(worst < 0.012, Num("an open ground: E(N) = G x (1 - N.y) / 2 and the sky is the rest (worst error %.4f)", worst));

  // the moments: a wall at 2 m on the +x side, 6 m the other way; the texel toward +x sees the wall, the one toward -x the far side; mean^2 <= mean of squares
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = {dirs[i], {0, 0, 0}, dirs[i].x > 0 ? 2.0f : 6.0f, false};
  const gi::TexelValue plus = gi::IntegrateTexel({1, 0, 0}, rays.data(), uint32_t(rays.size()), max_d);
  const gi::TexelValue minus = gi::IntegrateTexel({-1, 0, 0}, rays.data(), uint32_t(rays.size()), max_d);
  Check(plus.mean_distance < 2.7f && minus.mean_distance > 5.3f && plus.mean_distance_sq >= plus.mean_distance * plus.mean_distance - 1e-4f &&
            minus.mean_distance_sq >= minus.mean_distance * minus.mean_distance - 1e-4f,
        Num("the moments follow the surface: toward the wall %.2f, away %.2f", plus.mean_distance, minus.mean_distance));
  // a hit beyond the clamp counts as the clamp
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = {dirs[i], {0, 0, 0}, 500.0f, false};
  Check(Near(gi::IntegrateTexel({0, 1, 0}, rays.data(), uint32_t(rays.size()), max_d).mean_distance, max_d, 1e-3), "a hit farther than the clamp counts as the clamp");
  // no rays at all: an empty texel is open, and nothing is divided by zero
  const gi::TexelValue empty = gi::IntegrateTexel({0, 1, 0}, nullptr, 0, max_d);
  Check(empty.irradiance.x == 0.0f && empty.sky_visibility == 0.0f && empty.mean_distance == max_d && std::isfinite(empty.mean_distance_sq), "no rays: finite, open");
}

// ---------------------------------------------------------------------------
// a synthetic field
// ---------------------------------------------------------------------------
struct CascadeDef {
  gi::Dims dims;
  float spacing;
};

struct TestField {
  gi::GiFrame frame;
  Atlas atlas;

  // The cascades stacked as in the real layout, centred on `camera` with the y centre `y_offset` from it. The slots are made up, the frame is valid, fade 1, a huge safe range.
  void Init(const std::vector<CascadeDef>& defs, const double camera[3], double y_offset) {
    frame = gi::GiFrame{};
    frame.cascade_count = uint32_t(defs.size());
    uint32_t aw = 0, ah = 0, ww = 0, wh = 0;
    for (size_t c = 0; c < defs.size(); ++c) {
      gi::CascadeFrame& k = frame.cascades[c];
      k.dims = defs[c].dims;
      k.spacing_m = defs[c].spacing;
      k.origin = gi::OriginFor(camera, {defs[c].dims, defs[c].spacing, 0, 1}, y_offset);
      k.atlas_y = ah;
      k.weight_y = wh;
      k.max_distance_m = gi::MaxDistanceFor(defs[c].spacing);
      aw = std::max(aw, defs[c].dims.x * defs[c].dims.y * gi::kProbeTile);
      ah += defs[c].dims.z * gi::kProbeTile;
      ww = std::max(ww, defs[c].dims.x * defs[c].dims.y);
      wh += defs[c].dims.z;
    }
    for (int a = 0; a < 3; ++a) frame.camera[a] = camera[a];
    frame.atlas_width = aw;
    frame.atlas_height = ah;
    frame.slot = {1, 2, 3, gi::kNoSlot};
    frame.valid = gi::kFieldProducts;
    frame.level = gi::Level::kHigh;
    frame.layout = gi::Layout::kStandard;
    frame.fade = 1.0f;
    frame.safe_range_m = 1.0e6f;
    frame.sun_rgb[0] = 1.2f;
    frame.sun_rgb[1] = 1.1f;
    frame.sun_rgb[2] = 0.9f;
    frame.sun_sin_elevation = 0.7f;
    frame.ambient_up_rgb[0] = 0.4f;
    frame.ambient_up_rgb[1] = 0.5f;
    frame.ambient_up_rgb[2] = 0.7f;
    frame.ambient_down_rgb[0] = 0.15f;
    frame.ambient_down_rgb[1] = 0.12f;
    frame.ambient_down_rgb[2] = 0.1f;
    atlas.Allocate(aw, ah, ww, wh);
  }
  using EFn = std::function<gi::Rgba(const gi::Vec3& dir, const gi::Coord& coord, uint32_t cascade)>;
  using MFn = std::function<gi::Vec2(const gi::Vec3& dir, const gi::Coord& coord, uint32_t cascade)>;
  // Writes one probe: its 10 x 10 tile in both atlases (the border copies its source texel's value, as G2's blend will) and its weight.
  void WriteProbe(uint32_t cascade, const gi::Coord& coord, const EFn& e, const MFn& m, float weight) {
    const gi::CascadeFrame& k = frame.cascades[cascade];
    const gi::Slot slot = gi::SlotOf(coord, k.dims);
    const gi::Texel tile = gi::TileOrigin(slot, k.dims);
    for (uint32_t ty = 0; ty < gi::kProbeTile; ++ty)
      for (uint32_t tx = 0; tx < gi::kProbeTile; ++tx) {
        const gi::Texel src = gi::OctBorderSource(tx, ty);
        const gi::Vec3 dir = gi::TexelDirection(src.x, src.y);
        atlas.SetTexel(tile.x + tx, k.atlas_y + tile.y + ty, e(dir, coord, cascade), m(dir, coord, cascade));
      }
    const gi::Texel wt = gi::WeightTexel(slot, k.dims);
    atlas.weight[size_t(k.weight_y + wt.y) * atlas.ww + wt.x] = uint8_t(std::lround(std::min(std::max(weight, 0.0f), 1.0f) * 255.0f));
  }
  void FillCascade(uint32_t cascade, const EFn& e, const MFn& m, float weight) {
    const gi::Box w = frame.cascades[cascade].Window();
    for (int32_t z = w.lo.z; z < w.hi.z; ++z)
      for (int32_t y = w.lo.y; y < w.hi.y; ++y)
        for (int32_t x = w.lo.x; x < w.hi.x; ++x) WriteProbe(cascade, {x, y, z}, e, m, weight);
  }
  gi::Sample Lookup(gi::Vec3 p, gi::Vec3 n) const { return gi::Lookup(frame, atlas, p, n); }
  // The field's own twin of the .hlsli's resources
  void Bind() const {
    g_atlas = &atlas;
    const gi::GpuGiFrame g = gi::MakeGpuFrame(frame);
    hlsl_shim::g_rows.assign(gi::kGpuFrameRows, hlsl_shim::uint4());
    for (uint32_t r = 0; r < gi::kGpuFrameRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(g.words[r * 4], g.words[r * 4 + 1], g.words[r * 4 + 2], g.words[r * 4 + 3]);
  }
};

// Moments of a probe in open space: the cascade's clamp, no variance.
TestField::MFn Open(const TestField& f) {
  return [&f](const gi::Vec3&, const gi::Coord&, uint32_t cascade) {
    const float m = f.frame.cascades[cascade].max_distance_m;
    return gi::Vec2{m, m * m};
  };
}
TestField::EFn Constant(gi::Rgba value) {
  return [value](const gi::Vec3&, const gi::Coord&, uint32_t) { return value; };
}

// the cascades of the small test fields: 3 nested windows, spacing 1, 2, 4, 12 x 12 x 12 probes, centred on the camera (no y offset): the camera is inside every cascade's full weight part
const std::vector<CascadeDef> kSmall = {{{12, 12, 12}, 1.0f}, {{12, 12, 12}, 2.0f}, {{12, 12, 12}, 4.0f}};

void LookupChecks() {
  Lcg rng;
  const double camera[3] = {37.5, 11.25, -64.5};
  const gi::Vec3 cam = {float(camera[0]), float(camera[1]), float(camera[2])};

  // --- a uniform field: every probe E = L, sky 0.5, open moments: the lookup is L everywhere in the window, with full confidence, whatever the normal
  {
    TestField f;
    f.Init(kSmall, camera, 0.0);
    const gi::Rgba value = {0.7f, 0.4f, 0.2f, 0.5f};
    for (uint32_t c = 0; c < 3; ++c) f.FillCascade(c, Constant(value), Open(f), 1.0f);
    double worst = 0.0, worst_conf = 0.0;
    for (int i = 0; i < 3000; ++i) {
      // near the camera, inside every cascade's full weight part
      const gi::Vec3 p = {cam.x + rng.Range(-1.5f, 1.5f), cam.y + rng.Range(-1.5f, 1.5f), cam.z + rng.Range(-1.5f, 1.5f)};
      const gi::Sample s = f.Lookup(p, rng.Direction());
      worst = std::max({worst, double(std::fabs(s.irradiance.x - 0.7f)), double(std::fabs(s.irradiance.y - 0.4f)), double(std::fabs(s.irradiance.z - 0.2f)), double(std::fabs(s.sky_visibility - 0.5f))});
      worst_conf = std::max(worst_conf, double(std::fabs(s.confidence - 1.0f)));
    }
    Check(worst < 2e-4 && worst_conf < 1e-4, Num("a uniform field reads as itself, confidence 1 (errors %.1e, %.1e)", worst, worst_conf));
    // beyond every window: nothing, and confidence 0
    const gi::Sample far = f.Lookup({cam.x + 500, cam.y, cam.z}, {0, 1, 0});
    Check(far.confidence == 0.0f && far.irradiance.x == 0.0f && far.sky_visibility == 0.0f, "outside every cascade: no field, confidence 0");
    // the coarsest cascade's edge: the confidence falls to 0 over its last two probes
    const gi::CascadeFrame& last = f.frame.cascades[2];
    const float edge_x = float(last.origin.x + int32_t(last.dims.x) - 1) * last.spacing_m;  // the last probe's x: the weight is 0 there
    const gi::Sample at_edge = f.Lookup({edge_x, cam.y, cam.z}, {0, 1, 0});
    const gi::Sample inside = f.Lookup({edge_x - 3.0f * last.spacing_m, cam.y, cam.z}, {0, 1, 0});
    Check(at_edge.confidence < 1e-3f && inside.confidence > 0.999f, Num("the coarsest cascade fades out over its last two probes (confidence %.4f at the edge, %.4f three probes in)", at_edge.confidence, inside.confidence));
  }

  // --- a lobe: every probe holds the same function of the direction: the lookup reads it back whatever the weights (they cancel: all probes agree), to the 8 x 8 map's resolution.
  // A smooth wrap lobe is held to 5.8% of its range at worst (what an 8 x 8 octahedral map with bilinear filtering does to a smooth function: measured on the map alone) and 1.3% on
  // average; the cosine's kink at the horizon (max(w . l, 0)) is smeared over a texel: 13% at the kink, 1.7% on average.
  for (int kinked = 0; kinked < 2; ++kinked) {
    TestField f;
    f.Init(kSmall, camera, 0.0);
    const gi::Vec3 l = {0.36f, 0.8f, -0.48f};  // unit
    const gi::Vec3 c = {1.0f, 0.8f, 0.5f};
    const auto lobe = [&](const gi::Vec3& dir, const gi::Coord&, uint32_t) {
      const float k = kinked ? std::max(gi::Dot(dir, l), 0.0f) : 0.5f + 0.5f * gi::Dot(dir, l);
      return gi::Rgba{c.x * k, c.y * k, c.z * k, 1.0f - 0.5f * k};
    };
    for (uint32_t k = 0; k < 3; ++k) f.FillCascade(k, lobe, Open(f), 1.0f);
    double worst = 0.0, sum_err = 0.0;
    const int samples = 3000;
    for (int i = 0; i < samples; ++i) {
      const gi::Vec3 p = {cam.x + rng.Range(-3, 3), cam.y + rng.Range(-2, 2), cam.z + rng.Range(-3, 3)};
      const gi::Vec3 n = rng.Direction();
      const gi::Sample s = f.Lookup(p, n);
      const double want = kinked ? std::max(gi::Dot(n, l), 0.0f) : 0.5 + 0.5 * gi::Dot(n, l);
      const double err = std::fabs(s.irradiance.x - want);
      worst = std::max(worst, err);
      sum_err += err;
    }
    const double worst_limit = kinked ? 0.15 : 0.07, mean_limit = kinked ? 0.03 : 0.02;
    Check(worst < worst_limit && sum_err / samples < mean_limit,
          Num(kinked ? "a cosine lobe (kinked at the horizon) in every probe is read back (worst %.4f, mean %.4f: the 8 x 8 map's bilinear error)"
                     : "a smooth wrap lobe in every probe is read back (worst %.4f, mean %.4f: the 8 x 8 map's bilinear error)",
              worst, sum_err / samples));
  }

  // --- a gradient across the probes: the weighted average stays inside the probes' range, is monotone along the gradient and is near the linear value
  {
    TestField f;
    f.Init(kSmall, camera, 0.0);
    const auto ramp = [&](const gi::Vec3&, const gi::Coord& coord, uint32_t cascade) {
      const float x = float(coord.x) * f.frame.cascades[cascade].spacing_m;  // the probe's world x
      return gi::Rgba{0.1f * x, 0.0f, 0.0f, 0.0f};
    };
    f.FillCascade(0, ramp, Open(f), 1.0f);
    f.FillCascade(1, ramp, Open(f), 1.0f);
    f.FillCascade(2, ramp, Open(f), 1.0f);
    double prev = -1e9, worst = 0.0;
    bool monotone = true;
    const gi::Vec3 n = {0, 1, 0};  // across the gradient: the back face term then weighs the probes by their height, not by their place along the ramp
    for (int i = 0; i <= 160; ++i) {
      const float x = cam.x - 1.6f + 0.02f * float(i);
      const gi::Sample s = f.Lookup({x, cam.y + 0.2f, cam.z + 0.3f}, n);
      if (s.irradiance.x < prev - 1e-5) monotone = false;
      prev = s.irradiance.x;
      worst = std::max(worst, double(std::fabs(s.irradiance.x - 0.1f * x)));
    }
    Check(monotone, "along a gradient the lookup is monotone");
    Check(worst < 0.5 * 0.1 * 1.0, Num("... and within half a cell of the linear value (worst %.4f of a 0.1 per metre ramp)", worst));
  }

  // --- a wall between probes: the lit side red, the dark side blue; the moments of the dark side's probes see the wall close: with them no blue leaks, without them it does
  {
    TestField f;
    const std::vector<CascadeDef> one = {{{8, 6, 8}, 1.0f}};
    f.Init(one, camera, 0.0);
    const float wall_z = std::floor(cam.z) + 0.5f;  // between two lattice planes
    const auto side = [&](const gi::Vec3&, const gi::Coord& coord, uint32_t) {
      return float(coord.z) > wall_z ? gi::Rgba{1.0f, 0.0f, 0.0f, 1.0f} : gi::Rgba{0.0f, 0.0f, 1.0f, 1.0f};
    };
    // the distance from a probe to the wall plane along a direction (the wall is a plane z = wall_z): the true moments
    const auto wall_moments = [&](const gi::Vec3& dir, const gi::Coord& coord, uint32_t) {
      const float pz = float(coord.z);
      const float max_d = gi::MaxDistanceFor(1.0f);
      float d = max_d;
      if ((pz > wall_z && dir.z < -1e-4f) || (pz < wall_z && dir.z > 1e-4f)) d = std::min((wall_z - pz) / dir.z, max_d);
      return gi::Vec2{d, d * d};
    };
    f.FillCascade(0, side, wall_moments, 1.0f);
    gi::Rgba lit_side = {};
    const gi::Vec3 p = {cam.x + 0.31f, cam.y + 0.27f, wall_z + 0.3f};
    const gi::Vec3 n = {1.0f, 0.0f, 0.0f};  // along the wall: the back face term does not separate the sides
    const gi::Sample with = f.Lookup(p, n);
    lit_side = {with.irradiance.x, with.irradiance.y, with.irradiance.z, 0};
    // the same field with the moments cleared (open): the leak
    TestField g = f;
    g.FillCascade(0, side, Open(g), 1.0f);
    const gi::Sample without = g.Lookup(p, n);
    Check(lit_side.b < 0.02f && lit_side.r > 0.98f, Num("with the moments, a point in front of a wall sees only the lit side's probes: blue %.4f", lit_side.b));
    Check(without.irradiance.z > 0.15f, Num("without them the dark side leaks in (blue %.3f): the test is sensitive", without.irradiance.z));
    // the other side of the wall: the lit side's probes are behind it, the dark side's blue stays
    const gi::Sample behind = f.Lookup({cam.x + 0.31f, cam.y + 0.27f, wall_z - 0.3f}, n);
    Check(behind.irradiance.x < 0.02f && behind.irradiance.z > 0.98f, "behind the wall the lit side does not leak either");
  }

  // --- the blend between cascades: a fine cascade of 1.0 inside a coarse one of 3.0: deep inside the fine value, past its band the coarse one, monotone, no jump
  {
    TestField f;
    f.Init({{{12, 12, 12}, 2.0f}, {{24, 12, 24}, 4.0f}}, camera, 0.0);
    f.FillCascade(0, Constant({1, 1, 1, 0.2f}), Open(f), 1.0f);
    f.FillCascade(1, Constant({3, 3, 3, 0.6f}), Open(f), 1.0f);
    const gi::CascadeFrame& k0 = f.frame.cascades[0];
    const float x_edge = float(k0.origin.x + int32_t(k0.dims.x) - 1) * k0.spacing_m;  // the fine cascade's weight reaches 0 here
    double prev = 0.0, worst_step = 0.0, beyond = 0.0, worst_conf = 0.0;
    bool monotone = true;
    for (int i = 0; i <= 200; ++i) {
      const float x = x_edge - 8.0f + 0.04f * float(i);  // from 4 probes inside to the edge
      const gi::Sample s = f.Lookup({x, cam.y, cam.z}, {0, 1, 0});
      if (i > 0 && s.irradiance.x < prev - 1e-5) monotone = false;
      if (i > 0) worst_step = std::max(worst_step, double(std::fabs(s.irradiance.x - prev)));
      prev = s.irradiance.x;
      worst_conf = std::max(worst_conf, double(std::fabs(s.confidence - 1.0f)));
    }
    beyond = f.Lookup({x_edge + 2.0f, cam.y, cam.z}, {0, 1, 0}).irradiance.x;
    Check(monotone && worst_step < 0.06 && worst_conf < 1e-3, Num("the fine cascade hands over to the coarse one smoothly: monotone, the largest step in 4 cm is %.4f of the 2.0 between them, confidence stays 1", worst_step));
    Check(Near(f.Lookup({x_edge - 3.0f * k0.spacing_m, cam.y, cam.z}, {0, 1, 0}).irradiance.x, 1.0, 1e-4), "deeper than the band, only the fine cascade");
    Check(Near(beyond, 3.0, 1e-3), Num("at the fine cascade's edge and beyond, only the coarse one (%.4f)", beyond));
    // the sky visibility blends the same way
    Check(Near(f.Lookup({x_edge - 3.0f * k0.spacing_m, cam.y, cam.z}, {0, 1, 0}).sky_visibility, 0.2, 1e-4) && Near(f.Lookup({x_edge + 0.5f, cam.y, cam.z}, {0, 1, 0}).sky_visibility, 0.6, 1e-3), "the sky visibility follows");
  }

  // --- probes that are off (weight 0): a cascade without valid probes hands the point to the next one; a half valid cell is half confident; nothing valid is nothing
  {
    TestField f;
    f.Init({{{12, 12, 12}, 2.0f}, {{12, 12, 12}, 4.0f}}, camera, 0.0);
    f.FillCascade(0, Constant({1, 1, 1, 0.2f}), Open(f), 0.0f);
    f.FillCascade(1, Constant({3, 3, 3, 0.6f}), Open(f), 1.0f);
    const gi::Sample handed = f.Lookup(cam, {0, 1, 0});
    Check(Near(handed.irradiance.x, 3.0, 1e-4) && Near(handed.confidence, 1.0, 1e-4), "a cascade whose probes are all off hands the point to the next one");
    f.FillCascade(0, Constant({1, 1, 1, 0.2f}), Open(f), 0.5f);
    const gi::Sample half = f.Lookup(cam, {0, 1, 0});
    Check(Near(half.irradiance.x, 2.0, 2e-2) && Near(half.confidence, 1.0, 1e-4), Num("a cascade trusted at 0.5 shares the point with the next: E = %.3f (2.0), confidence 1", half.irradiance.x));
    // only the half of the probes on one side valid: the lookup there extrapolates from the valid ones and the confidence says how much it is
    TestField g;
    g.Init({{{12, 12, 12}, 2.0f}}, camera, 0.0);
    const auto any_value = [&](const gi::Vec3&, const gi::Coord&, uint32_t) { return gi::Rgba{1, 0, 0, 0}; };
    for (int32_t z = g.frame.cascades[0].origin.z; z < g.frame.cascades[0].origin.z + 12; ++z)
      for (int32_t y = g.frame.cascades[0].origin.y; y < g.frame.cascades[0].origin.y + 12; ++y)
        for (int32_t x = g.frame.cascades[0].origin.x; x < g.frame.cascades[0].origin.x + 12; ++x) g.WriteProbe(0, {x, y, z}, any_value, Open(g), x <= 18 ? 1.0f : 0.0f);
    // the probe plane x = 18 (world 36) is the last valid one; a point between x = 17 and 18 has both corners valid, between 18 and 19 half, between 19 and 20 none
    const gi::Sample both = g.Lookup({35.0f, cam.y, cam.z}, {0, 1, 0});
    const gi::Sample mixed = g.Lookup({37.0f, cam.y, cam.z}, {0, 1, 0});
    const gi::Sample none = g.Lookup({39.5f, cam.y, cam.z}, {0, 1, 0});
    Check(both.confidence > 0.99f && mixed.confidence > 0.3f && mixed.confidence < 0.7f && none.confidence < 0.05f,
          Num("the confidence is the share of valid probes: %.2f, %.2f, %.2f", both.confidence, mixed.confidence, none.confidence));
    // a field with no valid probe anywhere: zero, finite, confidence 0
    TestField h;
    h.Init({{{8, 6, 8}, 2.0f}}, camera, 0.0);
    h.FillCascade(0, Constant({1, 1, 1, 1}), Open(h), 0.0f);
    const gi::Sample nothing = h.Lookup(cam, {0, 1, 0});
    Check(nothing.confidence == 0.0f && nothing.irradiance.x == 0.0f && std::isfinite(nothing.sky_visibility), "no valid probe: nothing, finite, confidence 0");
  }

  // --- the range fade and the frame's fade: the confidence falls to 0 at the safe range; AmbientAt blends to the hemisphere by confidence x fade
  {
    TestField f;
    f.Init(kSmall, camera, 0.0);
    for (uint32_t c = 0; c < 3; ++c) f.FillCascade(c, Constant({0.6f, 0.3f, 0.1f, 0.5f}), Open(f), 1.0f);
    f.frame.safe_range_m = 3.0f;
    const gi::Vec3 n = {0, 1, 0};
    const gi::Sample near_s = f.Lookup({cam.x + 1.0f, cam.y, cam.z}, n);            // 1 m: inside (1 - 0.15) x 3 = 2.55
    const gi::Sample mid_s = f.Lookup({cam.x + 2.775f, cam.y, cam.z}, n);           // halfway through the fade: 2.55 .. 3
    const gi::Sample out_s = f.Lookup({cam.x + 3.5f, cam.y, cam.z}, n);
    Check(near_s.confidence > 0.999f && Near(mid_s.confidence, 0.5, 0.02) && out_s.confidence == 0.0f, Num("the safe range fades the confidence: %.3f, %.3f, %.3f", near_s.confidence, mid_s.confidence, out_s.confidence));
    f.frame.safe_range_m = 0.0f;
    Check(f.Lookup(cam, n).confidence == 0.0f, "no safe range, no field");
    f.frame.safe_range_m = 1000.0f;

    // AmbientAt
    const gi::Vec3 up = {0, 1, 0}, side_n = {1, 0, 0};
    const gi::Vec3 hemi_up = gi::AmbientHemisphere(f.frame, up);
    Check(NearV(hemi_up, {0.4f, 0.5f, 0.7f}, 1e-6) && NearV(gi::AmbientHemisphere(f.frame, {0, -1, 0}), {0.15f, 0.12f, 0.1f}, 1e-6) &&
              NearV(gi::AmbientHemisphere(f.frame, side_n), {0.275f, 0.31f, 0.4f}, 1e-6),
          "the flat hemisphere: up colour on top, down colour below, the mean on the side");
    const gi::Vec3 field = gi::AmbientAt(f.frame, f.atlas, cam, up);
    Check(NearV(field, {0.6f + 0.4f * 0.5f, 0.3f + 0.5f * 0.5f, 0.1f + 0.7f * 0.5f}, 1e-3), "AmbientAt = the field's surfaces + the sky ambient x the sky visibility where the field stands");
    f.frame.fade = 0.0f;
    Check(NearV(gi::AmbientAt(f.frame, f.atlas, cam, up), hemi_up, 1e-6), "AmbientAt at fade 0 is the flat hemisphere");
    f.frame.fade = 0.5f;
    const gi::Vec3 half = gi::AmbientAt(f.frame, f.atlas, cam, up);
    Check(NearV(half, hemi_up + (field - hemi_up) * 0.5f, 1e-3), "... and halfway at fade 0.5");
    f.frame.fade = 1.0f;
    Check(NearV(gi::AmbientAt(f.frame, f.atlas, {cam.x + 900, cam.y, cam.z}, up), hemi_up, 1e-6), "AmbientAt where there is no field is the flat hemisphere");
  }
}

// A field scrolled by the toroidal moves equals one built from scratch.
void ToroidalChecks() {
  Lcg rng;
  // what a probe holds is a function of its coordinate and direction, so that a wrong slot or a missed probe shows
  const auto irr = [](const gi::Vec3& dir, const gi::Coord& c, uint32_t cascade) {
    const float a = 0.6f + 0.3f * std::sin(0.7f * float(c.x) + 0.3f * float(c.y) + float(cascade)) + 0.2f * dir.y;
    const float b = 0.5f + 0.25f * std::cos(0.4f * float(c.z) - 0.2f * float(c.x)) + 0.1f * dir.x;
    return gi::Rgba{a, b, 0.3f + 0.1f * dir.z + 0.002f * float(c.y), 0.5f + 0.4f * std::sin(0.3f * float(c.x + c.z))};
  };
  const auto mom = [](const gi::Vec3& dir, const gi::Coord& c, uint32_t cascade) {
    const float m = 1.0f + 0.5f * std::sin(0.5f * float(c.x + c.y + c.z)) + 0.3f * dir.z + float(cascade);
    return gi::Vec2{m, m * m + 0.1f};
  };
  const std::vector<CascadeDef> defs = {{{6, 4, 6}, 1.0f}, {{6, 4, 6}, 2.0f}};
  int moves_checked = 0;
  bool tiles_equal = true, lookups_equal = true, weights_equal = true;
  for (int trial = 0; trial < 24; ++trial) {
    const double c0[3] = {rng.Range(-900.0f, 900.0f), rng.Range(-20.0f, 60.0f), rng.Range(-900.0f, 900.0f)};
    // the second camera: a step, a few cells, or a cut
    const uint32_t kind = trial % 3;
    const float reach = kind == 0 ? 2.5f : kind == 1 ? 9.0f : 400.0f;
    const double c1[3] = {c0[0] + rng.Range(-reach, reach), c0[1] + rng.Range(-reach, reach) * 0.3, c0[2] + rng.Range(-reach, reach)};
    TestField scrolled;
    scrolled.Init(defs, c0, -2.0);
    for (uint32_t k = 0; k < 2; ++k) scrolled.FillCascade(k, irr, mom, 1.0f);
    // scroll: new origins, only the exposed boxes written, in place
    for (uint32_t k = 0; k < 2; ++k) {
      const gi::CascadeFrame old = scrolled.frame.cascades[k];
      const gi::Coord new_origin = gi::OriginFor(c1, {old.dims, old.spacing_m, 0, 1}, -2.0);
      gi::Box boxes[3];
      const uint32_t count = gi::ExposedBoxes(old.origin, new_origin, old.dims, boxes);
      scrolled.frame.cascades[k].origin = new_origin;
      for (uint32_t b = 0; b < count; ++b)
        for (int32_t z = boxes[b].lo.z; z < boxes[b].hi.z; ++z)
          for (int32_t y = boxes[b].lo.y; y < boxes[b].hi.y; ++y)
            for (int32_t x = boxes[b].lo.x; x < boxes[b].hi.x; ++x) scrolled.WriteProbe(k, {x, y, z}, irr, mom, 1.0f);
    }
    TestField fresh;
    fresh.Init(defs, c1, -2.0);
    for (uint32_t k = 0; k < 2; ++k) fresh.FillCascade(k, irr, mom, 1.0f);
    // every probe of the new window holds the same bytes in both
    for (uint32_t k = 0; k < 2; ++k) {
      if (!(scrolled.frame.cascades[k].origin == fresh.frame.cascades[k].origin)) tiles_equal = false;
      const gi::Box w = fresh.frame.cascades[k].Window();
      for (int32_t z = w.lo.z; z < w.hi.z; ++z)
        for (int32_t y = w.lo.y; y < w.hi.y; ++y)
          for (int32_t x = w.lo.x; x < w.hi.x; ++x) {
            const gi::CascadeFrame& kf = fresh.frame.cascades[k];
            const gi::Slot s = gi::SlotOf({x, y, z}, kf.dims);
            const gi::Texel t = gi::TileOrigin(s, kf.dims);
            for (uint32_t j = 0; j < gi::kProbeTile; ++j)
              for (uint32_t i = 0; i < gi::kProbeTile; ++i) {
                const size_t at = size_t(kf.atlas_y + t.y + j) * fresh.atlas.w + t.x + i;
                for (int q = 0; q < 4; ++q)
                  if (fresh.atlas.irr[at * 4 + q] != scrolled.atlas.irr[at * 4 + q]) tiles_equal = false;
                for (int q = 0; q < 2; ++q)
                  if (fresh.atlas.mom[at * 2 + q] != scrolled.atlas.mom[at * 2 + q]) tiles_equal = false;
              }
            const gi::Texel wt = gi::WeightTexel(s, kf.dims);
            if (fresh.atlas.weight[size_t(kf.weight_y + wt.y) * fresh.atlas.ww + wt.x] != scrolled.atlas.weight[size_t(kf.weight_y + wt.y) * scrolled.atlas.ww + wt.x]) weights_equal = false;
          }
    }
    // and the lookups agree
    for (int i = 0; i < 100; ++i) {
      const gi::Vec3 p = {float(c1[0]) + rng.Range(-5, 5), float(c1[1]) + rng.Range(-3, 3), float(c1[2]) + rng.Range(-5, 5)};
      const gi::Vec3 n = rng.Direction();
      const gi::Sample a = scrolled.Lookup(p, n), b = fresh.Lookup(p, n);
      if (a.irradiance.x != b.irradiance.x || a.irradiance.y != b.irradiance.y || a.irradiance.z != b.irradiance.z || a.confidence != b.confidence || a.sky_visibility != b.sky_visibility) lookups_equal = false;
    }
    ++moves_checked;
  }
  Check(tiles_equal && weights_equal, Num("a field scrolled by the toroidal moves holds exactly what one built from scratch does, in every probe of the window (%.0f moves: steps, jumps and cuts, negative coordinates)", moves_checked));
  Check(lookups_equal, "... and the lookups are bit identical");
}

// The real budgets' windows nest for any camera, with their y centres, and the camera is always in the finest cascade's full weight part.
void NestingChecks() {
  Lcg rng;
  bool nested = true, camera_full = true, covered = true;
  double worst_margin = 1e9;  // the least the finest cascade's full weight part reaches above the camera, metres
  for (gi::Layout layout : {gi::Layout::kLean, gi::Layout::kStandard, gi::Layout::kExtended}) {
    const gi::Budget& b = gi::BudgetOf(layout);
    for (int trial = 0; trial < 600; ++trial) {
      const double cam[3] = {rng.Range(-4000, 4000), rng.Range(-30, 400), rng.Range(-4000, 4000)};
      const gi::GiFrame f = gi::MakeFrame(b, cam);
      const auto full_lo = [](const gi::CascadeFrame& k, int a) {
        const int32_t o = a == 0 ? k.origin.x : a == 1 ? k.origin.y : k.origin.z;
        return (double(o) + k.blend_band) * k.spacing_m;
      };
      const auto full_hi = [](const gi::CascadeFrame& k, int a) {
        const int32_t o = a == 0 ? k.origin.x : a == 1 ? k.origin.y : k.origin.z;
        const uint32_t d = a == 0 ? k.dims.x : a == 1 ? k.dims.y : k.dims.z;
        return (double(o) + double(d) - 1.0 - k.blend_band) * k.spacing_m;
      };
      for (uint32_t c = 0; c + 1 < b.cascade_count; ++c) {
        // the finer cascade's full weight part lies inside the coarser one's, so that where the finer fades the coarser is whole
        for (int a = 0; a < 3; ++a)
          if (!(full_lo(f.cascades[c], a) >= full_lo(f.cascades[c + 1], a) && full_hi(f.cascades[c], a) <= full_hi(f.cascades[c + 1], a))) nested = false;
      }
      const gi::Vec3 p = {float(cam[0]), float(cam[1]), float(cam[2])};
      if (!(gi::CascadeBlend(f.cascades[0], p) > 0.9999f)) camera_full = false;
      worst_margin = std::min(worst_margin, full_hi(f.cascades[0], 1) - cam[1]);
      // the camera and the points round it are covered by the cascades together (the field is valid there)
      for (int i = 0; i < 8; ++i) {
        const gi::Vec3 q = {p.x + rng.Range(-10, 10), p.y + rng.Range(-3, 3), p.z + rng.Range(-10, 10)};
        float remaining = 1.0f;
        for (uint32_t c = 0; c < b.cascade_count; ++c) remaining *= 1.0f - gi::CascadeBlend(f.cascades[c], q);
        if (remaining > 1e-4f) covered = false;
      }
    }
  }
  Check(nested, "the full weight part of each cascade lies inside the next one's, for any camera (each budget's y centre included)");
  Check(camera_full, "the camera is always inside the finest cascade's full weight part");
  Check(covered, "the points within 10 m and 3 m of the camera are always covered by the cascades together");
  Check(worst_margin >= 0.0, Num("the finest cascade's full weight part reaches at least %.2f m above the camera (High: 0 to 2 m by the snap: G0b)", worst_margin));
}

// The baseline of an open ground against an integrated open ground; the field over it nets to ~0.
void BaselineChecks() {
  Lcg rng;
  const double camera[3] = {10.0, 5.0, 10.0};
  TestField f;
  f.Init(kSmall, camera, 0.0);
  f.frame.ground_albedo = gi::kGroundAlbedo;
  // the ground's radiance: albedo x (sun colour x sin(elevation) + up ambient)
  gi::Vec3 ground_radiance;
  {
    const float sun = std::max(f.frame.sun_sin_elevation, 0.0f);
    ground_radiance = {f.frame.ground_albedo * (f.frame.sun_rgb[0] * sun + f.frame.ambient_up_rgb[0]), f.frame.ground_albedo * (f.frame.sun_rgb[1] * sun + f.frame.ambient_up_rgb[1]),
                       f.frame.ground_albedo * (f.frame.sun_rgb[2] * sun + f.frame.ambient_up_rgb[2])};
  }
  const std::vector<gi::Vec3> dirs = FibonacciDirections(2048);
  std::vector<gi::RayResult> rays(dirs.size());
  for (size_t i = 0; i < rays.size(); ++i) rays[i] = dirs[i].y < 0 ? gi::RayResult{dirs[i], ground_radiance, 8.0f, false} : gi::RayResult{dirs[i], {0, 0, 0}, 0.0f, true};
  const auto irr = [&](const gi::Vec3& dir, const gi::Coord&, uint32_t) {
    const gi::TexelValue v = gi::IntegrateTexel(dir, rays.data(), uint32_t(rays.size()), 12.0f);
    return gi::Rgba{v.irradiance.x, v.irradiance.y, v.irradiance.z, v.sky_visibility};
  };
  for (uint32_t k = 0; k < 3; ++k) f.FillCascade(k, irr, Open(f), 1.0f);
  // E_surf of the open ground is the analytic baseline (the cosine weighted ground share)
  double worst = 0.0, sum_abs = 0.0, sum_net = 0.0, worst_net = 0.0;
  const double peak = ground_radiance.x;
  const int samples = 4000;
  for (int i = 0; i < samples; ++i) {
    const gi::Vec3 n = rng.Direction();
    const gi::Vec3 base = gi::GroundBaseline(f.frame, n);
    const gi::Vec3 p = {float(camera[0]) + rng.Range(-4, 4), float(camera[1]) + rng.Range(-2, 2), float(camera[2]) + rng.Range(-4, 4)};
    const gi::Sample s = f.Lookup(p, n);
    const gi::Vec3 net = gi::NetIrradiance(s.irradiance, base);
    const double err = std::fabs(s.irradiance.x - base.x);
    worst = std::max(worst, err);
    sum_abs += err;
    sum_net += net.x;
    worst_net = std::max(worst_net, double(net.x));
  }
  // The residue is the 8 x 8 map's bilinear error on (1 - N.y) / 2 (3.7% of the range at worst, 1.0% on average, scratch oct_error.cpp) and the 2,048 rays' noise: C6's "mean excess under
  // 3% of the pixel's colour" holds on average; the worst texel-resolution residue is what G3 may want a small dead band for (G0d).
  Check(worst < 0.05 * peak && sum_abs / samples < 0.02 * peak && sum_net / samples < 0.015 * peak,
        Num("an open ground gains almost nothing: against the analytic baseline the field is off by %.2f%% on average and %.2f%% at worst of the ground's radiance; the net gain %.2f%% on average and %.2f%% at worst",
            100.0 * sum_abs / samples / peak, 100.0 * worst / peak, 100.0 * sum_net / samples / peak, 100.0 * worst_net / peak));
  // the baseline's shape
  f.frame.sun_sin_elevation = -0.2f;  // the sun is down: only the ambient stays
  const gi::Vec3 night = gi::GroundBaseline(f.frame, {0, 0, 1});
  Check(NearV(night, {0.5f * f.frame.ground_albedo * f.frame.ambient_up_rgb[0], 0.5f * f.frame.ground_albedo * f.frame.ambient_up_rgb[1], 0.5f * f.frame.ground_albedo * f.frame.ambient_up_rgb[2]}, 1e-6),
        "at night the baseline is the ambient's (the sun term is 0)");
  f.frame.sun_sin_elevation = 0.7f;
  const gi::Vec3 up = gi::GroundBaseline(f.frame, {0, 1, 0}), down = gi::GroundBaseline(f.frame, {0, -1, 0});
  Check(up.x == 0.0f && up.y == 0.0f && up.z == 0.0f && down.x > 0.0f && down.x > gi::GroundBaseline(f.frame, {1, 0, 0}).x, "a surface looking up sees no ground, one looking down sees all of it");
  const gi::Vec3 net = gi::NetIrradiance({0.5f, 0.1f, 0.3f}, {0.2f, 0.2f, 0.3f});
  Check(net.x == 0.3f && net.y == 0.0f && net.z == 0.0f, "E_net is E_surf less the baseline, never negative");
  // a sunlit wall's bounce is a gain: a vertical surface beside a bright wall (E_surf above the baseline) keeps the difference
  const gi::Vec3 over = gi::NetIrradiance({0.9f, 0.9f, 0.9f}, gi::GroundBaseline(f.frame, {1, 0, 0}));
  Check(over.x > 0.5f, "where geometry sends more than the open ground would, the difference shows");
}

// What the flags and the rows say.
void FrameChecks() {
  gi::GiFrame f;
  Check(!f.ReplacesSsgi() && !f.FieldPresent() && !f.Has(gi::kIrradiance), "an empty frame has no field and replaces nothing");
  const double cam[3] = {0, 0, 0};
  f = gi::MakeFrame(gi::BudgetOf(gi::Layout::kStandard), cam);
  f.level = gi::Level::kHigh;
  f.fade = 1.0f;
  f.safe_range_m = 300.0f;
  f.valid = gi::kFieldProducts;
  f.slot = {7, 8, 9, gi::kNoSlot};
  Check(f.FieldPresent() && f.ReplacesSsgi() && !f.Has(gi::kPixelIrradiance), "a frame with the three textures replaces the screen-space bounce light");
  gi::GiFrame g = f;
  g.level = gi::Level::kOff;
  Check(!g.ReplacesSsgi(), "level off: no");
  g = f;
  g.fade = 0.0f;
  Check(!g.ReplacesSsgi() && g.FieldPresent(), "fade 0 (warming up after a cut): the screen-space bounce light stays the source");
  g = f;
  g.safe_range_m = 0.0f;
  Check(!g.ReplacesSsgi(), "no safe range: no");
  g = f;
  g.slot[gi::kMoments] = gi::kNoSlot;
  Check(!g.FieldPresent() && !g.ReplacesSsgi(), "a missing texture: no field");
  g = f;
  g.valid &= ~gi::ProductBit(gi::kProbeWeight);
  Check(!g.FieldPresent(), "a product that is not in `valid` is not there, whatever its slot");
  g = f;
  g.cascade_count = 0;
  Check(!g.FieldPresent(), "no cascades: no field");
  g = f;
  g.valid |= gi::ProductBit(gi::kPixelIrradiance);
  g.slot[gi::kPixelIrradiance] = 11;
  Check((gi::FrameFlags(g) & gi::kFlagPixelPass) != 0 && (gi::FrameFlags(f) & gi::kFlagPixelPass) == 0, "the per-pixel product's flag");
  Check((gi::FrameFlags(f) & gi::kFlagValid) && (gi::FrameFlags(f) & gi::kFlagReplacesSsgi), "the header flags");
  Check(gi::SsgiShare(0.0f, 1.0f) == 1.0f && gi::SsgiShare(1.0f, 1.0f) == 0.0f && gi::SsgiShare(1.0f, 0.5f) == 0.5f && gi::SsgiShare(2.0f, 1.0f) == 0.0f && gi::SsgiShare(-1.0f, 1.0f) == 1.0f,
        "the screen-space share of a pixel: all without the field, none with it, a crossfade between");

  // the rows: every field in its place, the HLSL accessors read them back
  Lcg rng;
  f.frame_serial = 77;
  f.generation = 3;
  f.fade = 0.625f;
  f.safe_range_m = 123.5f;
  f.strength = 1.75f;
  f.bounce_weight = 0.25f;
  f.camera[0] = -1234.5;
  f.camera[1] = 77.25;
  f.camera[2] = 4321.125;
  f.sun_rgb[0] = 1.5f; f.sun_rgb[1] = 1.25f; f.sun_rgb[2] = 0.75f;
  f.sun_sin_elevation = 0.3125f;
  f.ambient_up_rgb[0] = 0.5f; f.ambient_up_rgb[1] = 0.625f; f.ambient_up_rgb[2] = 0.875f;
  f.ambient_down_rgb[0] = 0.125f; f.ambient_down_rgb[1] = 0.1875f; f.ambient_down_rgb[2] = 0.0625f;
  f.ground_albedo = 0.25f;
  f.slot = {7, 8, 9, 11};
  f.valid = gi::kFieldProducts | gi::ProductBit(gi::kPixelIrradiance);
  for (uint32_t c = 0; c < f.cascade_count; ++c) {
    f.cascades[c].origin = {-500 + int32_t(c) * 37, 12 - int32_t(c), 900 + int32_t(c)};
    f.cascades[c].blend_band = 1.0f + 0.5f * float(c);
  }
  const gi::GpuGiFrame rows = gi::MakeGpuFrame(f);
  Check(rows.words.size() == 80 && sizeof(gi::GpuGiFrame) == 320, "20 rows of 16 bytes");
  hlsl_shim::g_rows.assign(gi::kGpuFrameRows, hlsl_shim::uint4());
  for (uint32_t r = 0; r < gi::kGpuFrameRows; ++r) hlsl_shim::g_rows[r] = hlsl_shim::uint4(rows.words[r * 4], rows.words[r * 4 + 1], rows.words[r * 4 + 2], rows.words[r * 4 + 3]);
  using namespace hlsl_shim;
  Check(GiFlags() == gi::FrameFlags(f) && GiCascadeCount() == 3 && GiFieldValid() && GiReplacesSsgi(), "the HLSL reads the flags and the count");
  Check(GiSlot(0) == 7 && GiSlot(1) == 8 && GiSlot(2) == 9 && GiSlot(3) == 11, "... the slots");
  Check(GiFade() == 0.625f && GiSafeRange() == 123.5f && GiStrength() == 1.75f && GiBounceWeight() == 0.25f && GiGroundAlbedo() == 0.25f, "... the scalars");
  Check(GiCamera().x == -1234.5f && GiCamera().y == 77.25f && GiCamera().z == 4321.125f, "... the camera");
  Check(GiSunColour().x == 1.5f && GiSunColour().y == 1.25f && GiSunColour().z == 0.75f && GiSunSinElevation() == 0.3125f, "... the sun");
  Check(GiAmbientUp().y == 0.625f && GiAmbientDown().z == 0.0625f, "... the ambient");
  bool cascades_ok = true;
  for (uint32_t c = 0; c < 3; ++c) {
    const GiCascade k = GiLoadCascade(c);
    const gi::CascadeFrame& w = f.cascades[c];
    if (k.originX != w.origin.x || k.originY != w.origin.y || k.originZ != w.origin.z || k.atlasY != w.atlas_y || k.weightY != w.weight_y || k.dimX != w.dims.x || k.dimY != w.dims.y || k.dimZ != w.dims.z ||
        k.spacing != w.spacing_m || k.invSpacing != 1.0f / w.spacing_m || k.maxDistance != w.max_distance_m || k.blendBand != w.blend_band)
      cascades_ok = false;
  }
  Check(cascades_ok, "... and every cascade's row, negative origins included");
  Check(GiLoadCascade(0).atlasY == 0 && GiLoadCascade(1).atlasY == 320 && GiLoadCascade(2).atlasY == 640 && GiLoadCascade(2).weightY == 64, "the cascades' rows in the atlases");
  // the reserved row is zero; a frame with fewer cascades leaves the others' rows zero
  Check(rows.words[gi::kRowReserved * 4] == 0 && rows.words[gi::kRowReserved * 4 + 3] == 0, "the reserved row is zero");
  const gi::GpuGiFrame lean = gi::MakeGpuFrame(gi::MakeFrame(gi::BudgetOf(gi::Layout::kLean), cam));
  bool zero_tail = true;
  for (uint32_t r = gi::kRowCascade0 + 2 * gi::kRowsPerCascade; r < gi::kGpuFrameRows; ++r)
    for (uint32_t w = 0; w < 4; ++w)
      if (lean.words[r * 4 + w] != 0) zero_tail = false;
  Check(zero_tail, "a lean frame leaves the unused cascades' rows zero");
  // the same frame, the same bytes
  Check(gi::MakeGpuFrame(f) == rows, "the rows are a pure function of the frame");
  (void)rng;
}

// ---------------------------------------------------------------------------
// the .hlsli against the C++ values
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void DefineChecks(const std::string& text) {
  std::map<std::string, double> defines;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("#define GI_", 0) != 0) continue;
    std::istringstream words(line);
    std::string define, name, value;
    words >> define >> name >> value;
    if (name.find('(') != std::string::npos || value.empty()) continue;  // a macro with arguments (none are defined here) or a flag
    while (!value.empty() && (value.back() == 'f' || value.back() == 'u' || value.back() == 'F' || value.back() == 'U') && value.rfind("0x", 0) != 0) value.pop_back();
    if (value.rfind("0x", 0) == 0) {
      while (!value.empty() && (value.back() == 'u' || value.back() == 'U')) value.pop_back();
      defines[name] = double(std::strtoull(value.c_str(), nullptr, 16));
    } else {
      char* end = nullptr;
      const double v = std::strtod(value.c_str(), &end);
      if (end != value.c_str() && *end == '\0') defines[name] = v;
    }
  }
  const std::map<std::string, double> want = {
      {"GI_FORMAT_VERSION", double(gi::kFormatVersion)},
      {"GI_MAX_CASCADES", double(gi::kMaxCascades)},
      {"GI_PROBE_INTERIOR", double(gi::kProbeInterior)},
      {"GI_PROBE_BORDER", double(gi::kProbeBorder)},
      {"GI_PROBE_TILE", double(gi::kProbeTile)},
      {"GI_NO_SLOT", double(gi::kNoSlot)},
      {"GI_FRAME_ROWS", double(gi::kGpuFrameRows)},
      {"GI_ROW_HEADER", double(gi::kRowHeader)},
      {"GI_ROW_SLOTS", double(gi::kRowSlots)},
      {"GI_ROW_FADE", double(gi::kRowFade)},
      {"GI_ROW_CAMERA", double(gi::kRowCamera)},
      {"GI_ROW_SUN", double(gi::kRowSun)},
      {"GI_ROW_AMBIENT_UP", double(gi::kRowAmbientUp)},
      {"GI_ROW_AMBIENT_DOWN", double(gi::kRowAmbientDown)},
      {"GI_ROW_RESERVED", double(gi::kRowReserved)},
      {"GI_ROW_CASCADE0", double(gi::kRowCascade0)},
      {"GI_ROWS_PER_CASCADE", double(gi::kRowsPerCascade)},
      {"GI_FLAG_VALID", double(gi::kFlagValid)},
      {"GI_FLAG_REPLACES_SSGI", double(gi::kFlagReplacesSsgi)},
      {"GI_FLAG_PIXEL_PASS", double(gi::kFlagPixelPass)},
      {"GI_PRODUCT_IRRADIANCE", double(gi::kIrradiance)},
      {"GI_PRODUCT_MOMENTS", double(gi::kMoments)},
      {"GI_PRODUCT_PROBE_WEIGHT", double(gi::kProbeWeight)},
      {"GI_PRODUCT_PIXEL_IRRADIANCE", double(gi::kPixelIrradiance)},
      {"GI_NORMAL_OFFSET_SPACINGS", double(gi::kNormalOffsetSpacings)},
      {"GI_BACKFACE_FLOOR", double(gi::kBackFaceFloor)},
      {"GI_CHEBYSHEV_FLOOR", double(gi::kChebyshevFloor)},
      {"GI_RANGE_FADE_SHARE", double(gi::kRangeFadeShare)},
      {"GI_MIN_WEIGHT_SUM", double(gi::kMinWeightSum)},
      {"GI_MIN_COVER", double(gi::kMinCover)},
      {"GI_REMAINING_STOP", double(gi::kRemainingStop)},
  };
  int wrong = 0;
  for (const auto& [name, value] : want) {
    const auto it = defines.find(name);
    if (it == defines.end()) {
      std::printf("  missing in the .hlsli: %s\n", name.c_str());
      ++wrong;
    } else if (!Near(it->second, value, 1e-12, 1e-6)) {
      std::printf("  %s: .hlsli %.9g, C++ %.9g\n", name.c_str(), it->second, value);
      ++wrong;
    }
  }
  for (const auto& [name, value] : defines)
    if (!want.count(name)) {
      std::printf("  the test does not know %s\n", name.c_str());
      ++wrong;
    }
  Check(wrong == 0, Num("the .hlsli's %.0f numeric defines are the header's (%.0f wrong)", double(defines.size()), double(wrong)));
  // the layout words the rows documentation and the HLSL agree on
  Check(text.find("GI_FRAME_ROW") != std::string::npos && text.find("GI_SAMPLE_IRRADIANCE") != std::string::npos && text.find("GI_SAMPLE_MOMENTS") != std::string::npos && text.find("GI_LOAD_PROBE_WEIGHT") != std::string::npos,
        "the .hlsli reads its four resources through the macros the includer defines");
}

// ---------------------------------------------------------------------------
// the .hlsli as C++ against the twin, on random frames and points
// ---------------------------------------------------------------------------
void TwinChecks() {
  Lcg rng;
  // a random field on the lean layout (and a small three cascade one): random atlas contents, so that any difference in the arithmetic shows
  int frames = 0, mismatched = 0, points = 0, in_blend = 0, partial = 0;
  double worst = 0.0;
  for (int trial = 0; trial < 10; ++trial) {
    TestField f;
    const double cam[3] = {rng.Range(-3000, 3000), rng.Range(-50, 300), rng.Range(-3000, 3000)};
    float y_shift = 0.0f;
    if (trial % 2 == 0) {
      const gi::Budget& b = gi::BudgetOf(gi::Layout::kLean);
      f.Init({{b.cascade[0].dims, b.cascade[0].spacing_m}, {b.cascade[1].dims, b.cascade[1].spacing_m}}, cam, b.y_centre_offset_m);
      for (uint32_t c = 0; c < 2; ++c) f.frame.cascades[c].blend_band = b.cascade[c].blend_band;
      y_shift = float(b.y_centre_offset_m);
    } else {
      f.Init(kSmall, cam, -1.0);
    }
    for (size_t i = 0; i < f.atlas.irr.size(); i += 4) {
      f.atlas.irr[i + 0] = rng.Range(0.0f, 3.0f);
      f.atlas.irr[i + 1] = rng.Range(0.0f, 3.0f);
      f.atlas.irr[i + 2] = rng.Range(0.0f, 3.0f);
      f.atlas.irr[i + 3] = rng.Range(0.0f, 1.0f);
    }
    for (size_t i = 0; i < f.atlas.mom.size(); i += 2) {
      const float mean = rng.Range(0.0f, 24.0f);
      f.atlas.mom[i] = mean;
      f.atlas.mom[i + 1] = mean * mean + rng.Range(-0.2f, 6.0f);  // a negative variance now and then: the lookup takes the absolute value
    }
    for (auto& w : f.atlas.weight) w = rng.Below(4) == 0 ? 0 : uint8_t(rng.Below(256));
    f.frame.fade = rng.Range(0.2f, 1.0f);
    f.frame.safe_range_m = rng.Range(40.0f, 400.0f);
    f.frame.sun_rgb[0] = rng.Range(0.0f, 2.0f);
    f.frame.sun_rgb[1] = rng.Range(0.0f, 2.0f);
    f.frame.sun_rgb[2] = rng.Range(0.0f, 2.0f);
    f.frame.sun_sin_elevation = rng.Range(-0.3f, 1.0f);
    f.frame.ambient_up_rgb[0] = rng.Range(0.0f, 1.0f);
    f.frame.ambient_up_rgb[1] = rng.Range(0.0f, 1.0f);
    f.frame.ambient_up_rgb[2] = rng.Range(0.0f, 1.0f);
    f.frame.ambient_down_rgb[0] = rng.Range(0.0f, 0.5f);
    f.frame.ambient_down_rgb[1] = rng.Range(0.0f, 0.5f);
    f.frame.ambient_down_rgb[2] = rng.Range(0.0f, 0.5f);
    f.frame.ground_albedo = rng.Range(0.05f, 0.5f);
    f.Bind();
    ++frames;
    double span = 0.0;
    for (uint32_t c = 0; c < f.frame.cascade_count; ++c) span = std::max(span, double(f.frame.cascades[c].dims.x) * f.frame.cascades[c].spacing_m);
    for (int i = 0; i < 1500; ++i) {
      const gi::Vec3 p = {float(cam[0]) + rng.Range(-0.6f, 0.6f) * float(span), float(cam[1]) + rng.Range(-0.6f, 0.6f) * float(span) * 0.5f + y_shift,
                          float(cam[2]) + rng.Range(-0.6f, 0.6f) * float(span)};
      const gi::Vec3 n = rng.Direction();
      const gi::Sample a = gi::Lookup(f.frame, f.atlas, p, n);
      const hlsl_shim::GiSample b = hlsl_shim::GiLookup(hlsl_shim::float3(p.x, p.y, p.z), hlsl_shim::float3(n.x, n.y, n.z));
      const double e = std::max({std::fabs(a.irradiance.x - b.irradiance.x), std::fabs(a.irradiance.y - b.irradiance.y), std::fabs(a.irradiance.z - b.irradiance.z), std::fabs(a.sky_visibility - b.skyVisibility),
                                 std::fabs(a.confidence - b.confidence)});
      worst = std::max(worst, e);
      if (e > 2e-5 * (1.0 + std::fabs(a.irradiance.x))) ++mismatched;
      // the ambient, the baseline, the net
      const gi::Vec3 am = gi::AmbientAt(f.frame, f.atlas, p, n);
      const hlsl_shim::float3 hm = hlsl_shim::GiAmbientAt(hlsl_shim::float3(p.x, p.y, p.z), hlsl_shim::float3(n.x, n.y, n.z));
      if (!NearV(am, {hm.x, hm.y, hm.z}, 2e-5, 2e-5)) ++mismatched;
      const gi::Vec3 base = gi::GroundBaseline(f.frame, n);
      const hlsl_shim::float3 hbase = hlsl_shim::GiGroundBaseline(hlsl_shim::float3(n.x, n.y, n.z));
      if (!NearV(base, {hbase.x, hbase.y, hbase.z}, 1e-6, 1e-6)) ++mismatched;
      const gi::Vec3 net = gi::NetIrradiance(a.irradiance, base);
      const hlsl_shim::float3 hnet = hlsl_shim::GiNetIrradiance(hlsl_shim::float3(b.irradiance.x, b.irradiance.y, b.irradiance.z), hbase);
      if (!NearV(net, {hnet.x, hnet.y, hnet.z}, 2e-5, 2e-5)) ++mismatched;
      const gi::Vec3 hemi = gi::AmbientHemisphere(f.frame, n);
      const hlsl_shim::float3 hh = hlsl_shim::GiAmbientHemisphere(hlsl_shim::float3(n.x, n.y, n.z));
      if (!NearV(hemi, {hh.x, hh.y, hh.z}, 1e-6, 1e-6)) ++mismatched;
      if (hlsl_shim::GiSsgiShare(a.confidence, f.frame.fade) != gi::SsgiShare(a.confidence, f.frame.fade) && !Near(hlsl_shim::GiSsgiShare(a.confidence, f.frame.fade), gi::SsgiShare(a.confidence, f.frame.fade), 1e-6)) ++mismatched;
      ++points;
      if (a.confidence > 0.02f && a.confidence < 0.98f) ++partial;
      for (uint32_t c = 0; c < f.frame.cascade_count; ++c) {
        const float w = gi::CascadeBlend(f.frame.cascades[c], p);
        if (w > 0.0f && w < 1.0f) ++in_blend;
        const hlsl_shim::GiCascade k = hlsl_shim::GiLoadCascade(c);
        if (!Near(hlsl_shim::GiCascadeBlend(k, hlsl_shim::float3(p.x, p.y, p.z)), w, 1e-6)) ++mismatched;
      }
      // the range fade and the octahedral uv
      const float dist = float(i) * 0.37f;
      if (!Near(hlsl_shim::GiRangeFade(dist, f.frame.safe_range_m), gi::RangeFade(dist, f.frame.safe_range_m), 1e-6)) ++mismatched;
      const uint32_t cas = uint32_t(rng.Below(f.frame.cascade_count));
      const hlsl_shim::GiCascade k = hlsl_shim::GiLoadCascade(cas);
      const gi::Vec2 oct = gi::OctEncode(n);
      const uint32_t sx = rng.Below(k.dimX), sy = rng.Below(k.dimY), sz = rng.Below(k.dimZ);
      const gi::Vec2 uv = gi::ProbeUv(f.frame.cascades[cas], f.frame.atlas_width, f.frame.atlas_height, sx, sy, sz, oct);
      const hlsl_shim::float2 huv = hlsl_shim::GiProbeUv(k, 1.0f / float(f.frame.atlas_width), 1.0f / float(f.frame.atlas_height), sx, sy, sz, hlsl_shim::float2(oct.x, oct.y));
      if (uv.x != huv.x || uv.y != huv.y) ++mismatched;
      // the toroidal slot
      const int32_t coord = int32_t(rng.Below(20000)) - 10000;
      if (hlsl_shim::GiWrap(coord, k.dimX) != gi::Wrap(coord, k.dimX) || hlsl_shim::GiWrap(coord, k.dimZ) != gi::Wrap(coord, k.dimZ)) ++mismatched;
    }
  }
  Check(mismatched == 0, Num("the HLSL lookup, ambient, baseline, net, blend, range fade, uv and wrap equal the twin on %.0f random points of %.0f random frames (worst difference %.1e)", points, frames, worst));
  Check(in_blend > 500 && partial > 500, Num("the random points exercise the blend bands (%.0f) and partial confidence (%.0f)", in_blend, partial));
}

}  // namespace

int main(int argc, char** argv) {
  std::string hlsli;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-v") {
      g_verbose = true;
    } else {
      hlsli = argv[i];
    }
  }
  if (hlsli.empty()) {
    const std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    hlsli = (slash == std::string::npos ? std::string() : here.substr(0, slash + 1)) + "hlsl/gi_common.hlsli";
  }
  const std::string text = ReadFile(hlsli);
  Check(!text.empty(), "the .hlsli is readable: " + hlsli);

  BudgetChecks();
  AddressingChecks();
  OctChecks();
  IntegrationChecks();
  NestingChecks();
  FrameChecks();
  LookupChecks();
  ToroidalChecks();
  BaselineChecks();
  if (!text.empty()) DefineChecks(text);
  TwinChecks();

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
