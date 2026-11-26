// Check of the probe field's per-pixel detail pass (gi_pixel_rules.h, hlsl/gi_pixel_common.hlsli, hlsl/gi_pixel_trace_cs.hlsl, gi_pixel.h / .cpp, the apply's mix): CITY-RT M3 package G5. A program of its own: it needs
// no renderer, no game files and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_pixel_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_pixel_test.cpp
//   run: larecomp_gi_pixel_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check)
//
// It checks: the IDENTITY (every cell but the strong rung's ULTRA is "none", and a layout without the pass is never asked; the plan's order of reasons; a weight of 0 is the field's value bit for bit, the apply's
// net irradiance included); the governor (steady, down after a spell over the cap, back after a spell far under it, no flicker near the cap); the sizes; the ray (the noise is in 0..1 and the R2 steps cover the
// unit square, the direction is unit, above the floor and cosine weighted: the mean cosine is 2/3 and P(cos > c) = 1 - c^2, and an estimator over a half lit hemisphere converges to the integral); the
// estimator's pieces (the miss, the car, the clamp, the hit distance as Ray Reconstruction's guide wants it, the origin's lift as the library's); the mix and the weight; the reduced read's tap weight; the surface
// lights picked from a light manager frame; the denoiser's push block against the plain consumer's words; the camera's motion against a brute force; the layout's numbers against the apply's; the pixel row of the
// field's frame; then it compiles the .hlsli ITSELF as C++ (hlsl_cpp_shim.h) and compares each function with the twin and every #define with the header's number, and reads the shader, the host, the
// apply, the CMake lines, host_shaders.h, the frame order, the renderer and the Ray Reconstruction table for the wiring. CITY-RT G6 added: the raw mode (the plan, the weight, the history rule, the consumer
// rule, the row's raw bit, the shader's text, the host's raw branch, the apply's wiring, the switch).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gi_apply_rules.h"
#include "gi_frame.h"
#include "gi_pixel_rules.h"
#include "dlss_rr_rules.h"
#include "hlsl_cpp_shim.h"
#include "light_frame.h"
#include "rt_denoise_rules.h"

namespace gi = mcla::gpu::gi;
namespace gp = mcla::gpu::gi_pixel;
namespace ga = mcla::gpu::gi_apply;
namespace lf = larecomp::gpu;

namespace hlsl_shim {
#include "hlsl/gi_pixel_common.hlsli"
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
bool SameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

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

hlsl_shim::float3 H3(gi::Vec3 v) { return hlsl_shim::float3(v.x, v.y, v.z); }
gi::Vec3 C3(hlsl_shim::float3 v) { return {v.x, v.y, v.z}; }

// ---------------------------------------------------------------------------
// the cells, the identity, the plan
// ---------------------------------------------------------------------------

void CellChecks() {
  const gi::Rung rungs[3] = {gi::Rung::kNone, gi::Rung::kWeak, gi::Rung::kStrong};
  const gi::Level levels[3] = {gi::Level::kOff, gi::Level::kHigh, gi::Level::kUltra};
  int running = 0;
  for (gi::Rung rung : rungs) {
    for (gi::Level level : levels) {
      const gp::Cell c = gp::CellFor(rung, level);
      const bool expect = rung != gi::Rung::kNone && level == gi::Level::kUltra;
      Check(c.runs == expect, std::string("CELL: ") + std::string(gi::RungName(rung)) + " " + std::string(gi::LevelName(level)) + (expect ? " runs the pass" : " is the identity"));
      if (!c.runs) {
        Check(c.rays_per_pixel == 0 && c.reach_m == 0.0f && c.denoise_iterations == 0 && c.cap_ms == 0.0f, std::string("CELL: a cell without the pass holds no numbers: ") + std::string(gi::RungName(rung)) + " " + std::string(gi::LevelName(level)));
      }
      running += c.runs ? 1 : 0;
    }
  }
  Check(running == 2, "CELL: both RT tiers run the pass at Ultra");
  const gp::Cell u = gp::CellFor(gi::Rung::kStrong, gi::Level::kUltra);
  Check(u.rays_per_pixel == 1 && u.reach_m == 12.0f && u.denoise_iterations == 3 && u.cap_ms > u.est_ms_hi && u.est_ms_lo < u.est_ms_hi, "CELL: 1 ray a pixel, 12 m, 3 iterations, a cap over the estimate");
  Check(gi::BudgetOf(gi::Layout::kExtended).pixel_rays_per_pixel == u.rays_per_pixel && gi::BudgetOf(gi::Layout::kStandard).pixel_rays_per_pixel == 0 && gi::BudgetOf(gi::Layout::kLean).pixel_rays_per_pixel == 0,
        "CELL: the cell's rays are the field's budget's (gi_frame.h): only the extended layout has the pass");
  // the memory: the doc's 190 MiB at ULTRA holds the pass's images at a traced 1080 lines (2.07 M pixels) beside the field's 79
  const uint64_t bytes = gp::ImageBytes(1920, 1080);
  Check(gp::BytesPerPixel() == 60 && Near(double(bytes) / (1024.0 * 1024.0), 118.6, 0.2), Num("MEMORY: 60 bytes a pixel, %.1f MiB at a traced 1920 x 1080", double(bytes) / (1024.0 * 1024.0)));
  Check(double(bytes) / (1024.0 * 1024.0) + double(gi::AtlasBytes(gi::BudgetOf(gi::Layout::kExtended))) / (1024.0 * 1024.0) < double(gi::BudgetOf(gi::Layout::kExtended).cost.vram_mib) + 30.0,
        "MEMORY: the pass's images and the field's atlases are about the layout's estimate (190 MiB) at the traced 1080 lines");
}

void PlanChecks() {
  gp::Facts all;
  all.layout = gi::Layout::kExtended;
  all.field_present = all.scene_ok = all.world_ok = all.hit_data_ok = all.planes_ok = all.pipelines_ready = all.resources_ok = true;
  Check(gp::Decide(all).why == gp::Why::kRuns && gp::Decide(all).denoise && !gp::Decide(all).raw, "PLAN: everything stands: it runs, with the denoiser, not raw");
  // THE IDENTITY: a layout without the pass is never asked whatever else is true
  for (gi::Layout layout : {gi::Layout::kNone, gi::Layout::kLean, gi::Layout::kStandard}) {
    gp::Facts f = all;
    f.layout = layout;
    Check(gp::Decide(f).why == gp::Why::kNotWanted && !gp::Decide(f).runs(), std::string("PLAN: the ") + std::string(gi::LayoutName(layout)) + " layout has no per-pixel pass: not wanted");
  }
  // the order of the reasons: the first that applies
  struct Step {
    const char* name;
    void (*clear)(gp::Facts&);
    gp::Why why;
  };
  const Step steps[] = {
      {"the field", [](gp::Facts& f) { f.field_present = false; }, gp::Why::kNoField},
      {"the top level", [](gp::Facts& f) { f.scene_ok = false; }, gp::Why::kNoScene},
      {"the world rows", [](gp::Facts& f) { f.world_ok = false; }, gp::Why::kNoWorld},
      {"the hit data", [](gp::Facts& f) { f.hit_data_ok = false; }, gp::Why::kNoHitData},
      {"the planes", [](gp::Facts& f) { f.planes_ok = false; }, gp::Why::kNoPlanes},
      {"the pipelines", [](gp::Facts& f) { f.pipelines_ready = false; }, gp::Why::kPipelines},
      {"the resources", [](gp::Facts& f) { f.resources_ok = false; }, gp::Why::kResources},
  };
  for (const Step& s : steps) {
    gp::Facts f = all;
    s.clear(f);
    Check(gp::Decide(f).why == s.why && !gp::Decide(f).runs(), std::string("PLAN: without ") + s.name + " the answer is '" + std::string(gp::WhyName(s.why)) + "'");
  }
  gp::Facts two = all;
  two.field_present = false;
  two.resources_ok = false;
  Check(gp::Decide(two).why == gp::Why::kNoField, "PLAN: with two reasons the first one is the answer");
  gp::Facts off = all;
  off.debug = 0;
  Check(gp::Decide(off).why == gp::Why::kDebugOff, "PLAN: the debug switch at 0 turns the pass off");
  off.debug = 1;
  Check(gp::Decide(off).runs(), "PLAN: the debug switch at 1 leaves the layout's say");
  gp::Facts raw = all;
  raw.raw = true;
  const gp::Plan rp = gp::Decide(raw);
  Check(rp.runs() && rp.raw && !rp.denoise, "RAW: in a raw frame (Ray Reconstruction takes it) the pass runs and the denoiser does not");
  Check(!gp::Facts{}.raw, "RAW: the facts' default is not raw (only a frame Ray Reconstruction takes is: GiApply passes TemporalSystem::RawModes().RtGi())");
  for (uint32_t i = 0; i < uint32_t(gp::Why::kCount); ++i) Check(std::string(gp::WhyName(gp::Why(i))).size() > 3, "PLAN: every reason has a name");
}

// ---------------------------------------------------------------------------
// the governor, the sizes
// ---------------------------------------------------------------------------

void GovernorChecks() {
  const float cap = 2.2f;
  gp::Governor g;
  for (int i = 0; i < 1000; ++i) g.AddSample(1.5f, cap);
  Check(!g.reduced() && g.divisor() == 1, "GOVERNOR: a pass under its cap stays at the traced size");
  for (int i = 0; i < 29; ++i) g.AddSample(4.0f, cap);
  Check(!g.reduced(), "GOVERNOR: a short spell over the cap changes nothing");
  g.Reset();
  int frames = 0;
  for (; frames < 200 && !g.reduced(); ++frames) g.AddSample(4.0f, cap);
  Check(g.reduced() && g.divisor() == 2 && frames >= gp::Governor::kDownFrames && frames < 80, Num("GOVERNOR: a pass at %.1f ms against a cap of 2.2 goes to half the traced size after %.0f frames", 4.0, double(frames)));
  // at the reduced size the cost is about a quarter: 1 ms is 4 ms at full size, over the cap: stays
  for (int i = 0; i < 1000; ++i) g.AddSample(1.0f, cap);
  Check(g.reduced(), "GOVERNOR: at the reduced size a pass whose full size would still be over the cap stays reduced (hysteresis)");
  for (int i = 0; i < 300; ++i) g.AddSample(0.3f, cap);
  Check(!g.reduced(), "GOVERNOR: far under the cap (4 x the reduced cost under 0.9 of it) it goes back");
  // near the cap: no flicker
  gp::Governor h;
  uint32_t flips = 0;
  bool was = false;
  Lcg rng;
  for (int i = 0; i < 20000; ++i) {
    h.AddSample(cap * rng.Range(0.8f, 1.1f) * (h.reduced() ? 0.25f : 1.0f), cap);
    flips += h.reduced() != was ? 1u : 0u;
    was = h.reduced();
  }
  Check(flips <= 4, Num("GOVERNOR: a pass that hovers round its cap does not flicker between the sizes (%.0f changes in 20000 frames)", double(flips)));
  gp::Governor bad;
  bad.AddSample(-1.0f, cap);
  bad.AddSample(std::nanf(""), cap);
  bad.AddSample(1.0f, 0.0f);
  Check(!bad.reduced() && bad.ewma() == 0.0f, "GOVERNOR: a negative, a NaN or a missing cap is not a sample");
}

void SizeChecks() {
  Check(gp::PixelExtent(1920, 1) == 1920 && gp::PixelExtent(1920, 2) == 960 && gp::PixelExtent(1081, 2) == 541 && gp::PixelExtent(1, 2) == 1, "SIZE: the pass's extent is the traced one or half of it rounded up");
  Check(gp::TracedTexel(0, 2, 1920) == 0 && gp::TracedTexel(959, 2, 1920) == 1918 && gp::TracedTexel(540, 2, 1081) == 1080 && gp::TracedTexel(5, 1, 1920) == 5 && gp::TracedTexel(2000, 1, 1920) == 1919,
        "SIZE: the traced texel a pixel stands for is its top left one, clamped into the traced image");
  Check(gp::Groups(1920) == 240 && gp::Groups(1081) == 136 && gp::Groups(8) == 1 && gp::Groups(9) == 2, "SIZE: the groups are 8 x 8");
}

// ---------------------------------------------------------------------------
// the ray
// ---------------------------------------------------------------------------

void RayChecks() {
  // the noise: in 0..1 and the frames of one pixel cover the unit square evenly (the R2 sequence: a low discrepancy set)
  uint32_t bad = 0;
  for (uint32_t y = 0; y < 16; ++y) {
    for (uint32_t x = 0; x < 16; ++x) {
      for (uint32_t f = 0; f < 8; ++f) {
        const gi::Vec2 n = gp::Noise(x, y, f);
        bad += (n.x >= 0.0f && n.x < 1.0f && n.y >= 0.0f && n.y < 1.0f) ? 0u : 1u;
      }
    }
  }
  Check(bad == 0, "NOISE: every number is in 0..1");
  {
    // 64 frames of one pixel: each of the 8 x 8 cells of the unit square holds at least one and at most three points (R2's discrepancy; independent random points would leave holes)
    int cells[8][8] = {};
    for (uint32_t f = 0; f < 64; ++f) {
      const gi::Vec2 n = gp::Noise(37, 11, f);
      ++cells[std::min(int(n.y * 8.0f), 7)][std::min(int(n.x * 8.0f), 7)];
    }
    int lo = 99, hi = 0, empty = 0;
    for (auto& row : cells) {
      for (int c : row) {
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        empty += c == 0 ? 1 : 0;
      }
    }
    Check(empty <= 14 && hi <= 4, Num("NOISE: 64 frames of one pixel cover the unit square (%.0f of 64 cells empty, at most %.0f points in one)", double(empty), double(hi)));
  }
  {
    // neighbouring pixels do not share their numbers (the per-pixel offset): the mean of the pairs' difference is that of independent numbers (1/3)
    double sum = 0.0;
    for (uint32_t i = 0; i < 4000; ++i) sum += std::fabs(gp::Noise(i, 100, 5).x - gp::Noise(i + 1, 100, 5).x);
    Check(Near(sum / 4000.0, 1.0 / 3.0, 0.03), Num("NOISE: neighbouring pixels are decorrelated (mean difference %.3f)", sum / 4000.0));
  }
  // the direction: unit, above the floor, cosine weighted about any normal
  Lcg rng;
  double mean_cos = 0.0;
  int over_half = 0, over_9 = 0;
  constexpr int kN = 200000;
  bool unit_ok = true, floor_ok = true;
  for (int i = 0; i < kN; ++i) {
    const gi::Vec3 n = rng.Direction();
    const gi::Vec3 d = gp::CosineDirection(n, rng.Next(), rng.Next());
    unit_ok = unit_ok && Near(gi::Dot(d, d), 1.0, 1e-4);
    const float c = gi::Dot(d, n);
    floor_ok = floor_ok && c >= gp::kCosineFloor - 1e-4f;
    mean_cos += c;
    over_half += c > 0.5f ? 1 : 0;
    over_9 += c > 0.9f ? 1 : 0;
  }
  mean_cos /= kN;
  Check(unit_ok, "DIRECTION: unit length");
  Check(floor_ok, "DIRECTION: never under the floor above the tangent plane");
  Check(Near(mean_cos, 2.0 / 3.0, 0.004), Num("DIRECTION: the mean cosine of cosine weighted rays is 2/3 (%.4f)", mean_cos));
  Check(Near(double(over_half) / kN, 0.75, 0.005) && Near(double(over_9) / kN, 0.19, 0.005), Num("DIRECTION: P(cos > c) = 1 - c^2: 0.75 and 0.19 (%.4f, %.4f)", double(over_half) / kN, double(over_9) / kN));
  // the estimator: a uniform environment gives its radiance back, and a half lit hemisphere (radiance 1 where the ray is within 60 degrees of the normal) the integral 1 - cos^2(60) = 0.75
  double e_uniform = 0.0, e_cap = 0.0;
  for (int i = 0; i < kN; ++i) {
    const gi::Vec3 n = {0.0f, 1.0f, 0.0f};
    const gi::Vec3 d = gp::CosineDirection(n, rng.Next(), rng.Next());
    e_uniform += 1.0;
    e_cap += d.y > 0.5f ? 1.0 : 0.0;
  }
  Check(Near(e_uniform / kN, 1.0, 1e-9) && Near(e_cap / kN, 0.75, 0.005), Num("ESTIMATOR: the mean of the rays' radiance is the irradiance in the field's units (a cap of 60 degrees: %.4f)", e_cap / kN));
  // the same through the field's own definition (gi_frame.h IntegrateTexel: 4 / N x sum over a uniform sphere): the two estimators of a lit hemisphere agree
  {
    std::vector<gi::RayResult> sphere;
    for (int i = 0; i < 4096; ++i) {
      gi::RayResult r;
      r.dir = rng.Direction();
      r.escaped = false;
      r.radiance = r.dir.y > 0.5f ? gi::Vec3{1.0f, 1.0f, 1.0f} : gi::Vec3{0, 0, 0};
      r.distance = 5.0f;
      sphere.push_back(r);
    }
    const gi::TexelValue t = gi::IntegrateTexel({0.0f, 1.0f, 0.0f}, sphere.data(), uint32_t(sphere.size()), 24.0f);
    Check(Near(t.irradiance.x, e_cap / kN, 0.05), Num("ESTIMATOR: the probes' integration (uniform sphere, 4/N sum) and the pixel's (cosine rays, mean) give one irradiance: %.3f vs %.3f", t.irradiance.x, e_cap / kN));
  }
  // the basis is orthonormal
  bool basis_ok = true;
  for (int i = 0; i < 2000; ++i) {
    const gi::Vec3 n = rng.Direction();
    const gp::Basis b = gp::MakeBasis(n);
    basis_ok = basis_ok && Near(gi::Dot(b.t, b.t), 1.0, 1e-4) && Near(gi::Dot(b.b, b.b), 1.0, 1e-4) && Near(gi::Dot(b.t, b.b), 0.0, 1e-4) && Near(gi::Dot(b.t, n), 0.0, 1e-4) && Near(gi::Dot(b.b, n), 0.0, 1e-4);
  }
  Check(basis_ok, "DIRECTION: the frame about a normal is orthonormal");
}

// ---------------------------------------------------------------------------
// the estimator's pieces, the mix, the weight
// ---------------------------------------------------------------------------

void EstimatorChecks() {
  Check(NearV(gp::MissRadiance({0.5f, 2.0f, 40.0f}), {0.5f, 2.0f, 32.0f}, 1e-6) && NearV(gp::MissRadiance({-1.0f, std::nanf(""), 3.0f}), {0.0f, 0.0f, 3.0f}, 1e-6),
        "MISS: the field's irradiance clamped as a probe's ray is (a firefly's guard: 32; a NaN and a negative are 0)");
  Check(gp::kMissShare == 1.0f, "MISS: a ray with nothing in its reach carries the field's value in full");
  const gi::Vec3 amb = {0.2f, 0.3f, 0.4f}, sun = {2.0f, 1.9f, 1.5f};
  Check(NearV(gp::CarRadiance(amb, sun, 0.5f), {(0.2f + 1.0f) * 0.12f, (0.3f + 0.95f) * 0.12f, (0.4f + 0.75f) * 0.12f}, 1e-6) && NearV(gp::CarRadiance(amb, sun, -1.0f), amb * 0.12f, 1e-6),
        "CAR: a car's hit is a neutral diffuse in the ambient and the sun's N.L (no negative N.L)");
  Check(gp::HitDistance(false, true, 3.0f) == 0.0f && gp::HitDistance(true, false, 3.0f) == gp::kMissDistance && gp::HitDistance(true, true, 3.0f) == 3.0f && gp::HitDistance(true, true, 1e9f) == gp::kMissDistance &&
            gp::HitDistance(true, true, 0.0f) == 0.0f,
        "HIT DISTANCE: 0 not traced, the miss distance for a miss, the ray's length for a hit");
  Check(gp::kMissDistance == mcla::gpu::rr::kRawMissDistance, "HIT DISTANCE: the miss distance is Ray Reconstruction's raw one (dlss_rr_rules.h)");
  Lcg rng;
  bool same = true;
  for (int i = 0; i < 400; ++i) {
    const bool traced = rng.Next() > 0.2f, hit = rng.Next() > 0.4f;
    const float len = rng.Range(0.0f, 8000.0f);
    same = same && SameBits(gp::HitDistance(traced, hit, len), mcla::gpu::rr::RawHitDistance(traced, hit, len));
  }
  Check(same, "HIT DISTANCE: the same function as Ray Reconstruction's RawHitDistance on random inputs");
  Check(Near(gp::OriginLift(0.0f), 0.006, 1e-6) && Near(gp::OriginLift(10.0f), 0.036, 1e-6) && Near(gp::OriginLift(1000.0f), 0.4, 1e-6), "LIFT: the origin is lifted as the library's CityHitLift");
  Check(Near(gp::PixelFootprint(0.5f, 1080.0f, 10.0f), 2.0 * 0.5 / 1080.0 * 10.0, 1e-7) && gp::PixelFootprint(0.5f, 0.0f, 10.0f) == 0.0f, "FOOTPRINT: the pixel's width at a distance");
  // the weight and the mix
  Check(gp::Weight(1.0f, 5.0f) == 0.0f && gp::Weight(gp::kFullLength, 5.0f) == 1.0f && gp::Weight(100.0f, 5.0f) == 1.0f && gp::Weight(0.0f, 5.0f) == 0.0f && gp::Weight(2.5f, 0.0f) == 0.0f && gp::Weight(2.5f, 5.0f) == 0.5f,
        "WEIGHT: 0 on a fresh pixel, 1 from kFullLength frames, 0 where the pass did not trace the pixel (a depth guide of 0)");
  float last = -1.0f;
  bool monotone = true;
  for (float l = 0.0f; l <= 8.0f; l += 0.25f) {
    const float w = gp::Weight(l, 1.0f);
    monotone = monotone && w >= last && w >= 0.0f && w <= 1.0f;
    last = w;
  }
  Check(monotone, "WEIGHT: grows with the history's length, within 0..1");
  // THE IDENTITY of the mix: a weight of 0 is the field's value bit for bit, whatever the pixel term holds
  bool ident = true;
  for (int i = 0; i < 500; ++i) {
    const gi::Vec3 f = {rng.Range(0, 3), rng.Range(0, 3), rng.Range(0, 3)};
    const gi::Vec3 p = i % 3 == 0 ? gi::Vec3{INFINITY, std::nanf(""), 1e30f} : gi::Vec3{rng.Range(0, 3), rng.Range(0, 3), rng.Range(0, 3)};
    const gi::Vec3 m = gp::Mix(f, p, 0.0f);
    ident = ident && SameBits(m.x, f.x) && SameBits(m.y, f.y) && SameBits(m.z, f.z);
    const gi::Vec3 q = gp::Mix(f, {1.0f, 2.0f, 3.0f}, 1.0f);
    ident = ident && Near(q.x, 1.0, 1e-5) && Near(q.y, 2.0, 1e-5) && Near(q.z, 3.0, 1e-5);
    const gi::Vec3 h = gp::Mix(f, {1.0f, 2.0f, 3.0f}, 0.5f);
    ident = ident && Near(h.x, 0.5 * (f.x + 1.0), 1e-5) && Near(h.y, 0.5 * (f.y + 2.0), 1e-5);
  }
  Check(ident, "MIX: a weight of 0 gives the field's value bit for bit (a NaN pixel term included), 1 the pixel's, 0.5 the middle");
  // through the apply's net irradiance: with the weight 0 the lookup's result is the field's, with the pixel term it moves where the pixel's light differs
  gi::GiFrame frame;
  frame.sun_rgb[0] = 1.5f;
  frame.sun_rgb[1] = 1.4f;
  frame.sun_rgb[2] = 1.2f;
  frame.sun_sin_elevation = 0.6f;
  frame.ambient_up_rgb[0] = frame.ambient_up_rgb[1] = frame.ambient_up_rgb[2] = 0.3f;
  bool net_ok = true;
  for (int i = 0; i < 200; ++i) {
    const gi::Vec3 n = rng.Direction();
    const gi::Vec3 e = {rng.Range(0, 2), rng.Range(0, 2), rng.Range(0, 2)};
    const gi::Vec3 a = ga::NetIrradiance(frame, e, n, ga::kDeadBand);
    const gi::Vec3 b = ga::NetIrradiance(frame, gp::Mix(e, {9.0f, 9.0f, 9.0f}, 0.0f), n, ga::kDeadBand);
    const gi::Vec3 c = ga::NetIrradiance(frame, gp::Mix(e, {9.0f, 9.0f, 9.0f}, 1.0f), n, ga::kDeadBand);
    net_ok = net_ok && SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z) && c.x >= a.x && c.y >= a.y && c.z >= a.z;
  }
  Check(net_ok, "MIX: through the apply's net irradiance a weight of 0 changes nothing, and a brighter pixel term never lowers it");
  // the reduced read's tap
  Check(gp::TapWeight(0.25f, 0.0f, 1.0f, 1.0f) == 0.0f && Near(gp::TapWeight(0.25f, 5.0f, 0.5f, 0.5f), 0.0625, 1e-9) && Near(gp::TapWeight(0.5f, 5.0f, 0.0f, 1.0f), 0.5 * gp::kMinTapWeight, 1e-12) &&
            gp::kMinTapWeight == ga::kMinUpsampleWeight,
        "TAP: bilinear x depth x normal, floored as the apply's upsample is, 0 for a tap the pass did not trace");
}

// ---------------------------------------------------------------------------
// the denoiser's push block, the camera's motion, the layout, the field's row
// ---------------------------------------------------------------------------

void PushChecks() {
  namespace p = mcla::gpu::rt_denoise::plain;
  gp::DenoiseView v;
  v.signal_slot = 301;
  v.depth_slot = 302;
  v.normal_slot = 303;
  v.hit_slot = 304;
  v.history_depth_slot = 305;
  v.width = 1920;
  v.height = 1080;
  v.tan_x = 1.6f;
  v.tan_y = 0.9f;
  v.prev_tan_x = 1.5f;
  v.prev_tan_y = 0.85f;
  for (int i = 0; i < 12; ++i) v.motion[i] = float(i) * 0.5f;
  v.length = 6.0f;
  const auto w = gp::MakePush(v);
  const auto f = [&](uint32_t i) { return std::bit_cast<float>(w[i]); };
  Check((w[p::kSignal] & 0xFFFFu) == 301 && (w[p::kSignal] >> 16) == 302, "PUSH: word 0 is the signal and the depth guide");
  Check((w[p::kGuides] & 0xFFFFu) == 303 && (w[p::kGuides] >> 16) == 304, "PUSH: word 1 is the view normal and the hit image");
  Check((w[p::kHistory] & 0xFFFFu) == 0 && (w[p::kHistory] >> 16) == 305, "PUSH: word 2 holds the previous depth guide in its high half (the class patches the history image's slot into the low half)");
  Check((w[p::kSize] & 0xFFFFu) == 1920 && (w[p::kSize] >> 16) == 1080 && w[p::kParams] == 0, "PUSH: the size, and the params left to the class (iteration, has-history)");
  Check(f(p::kTanX) == 1.6f && f(p::kTanY) == 0.9f && f(p::kPrevTanX) == 1.5f && f(p::kPrevTanY) == 0.85f && f(p::kLength) == 6.0f, "PUSH: the lens, the last frame's lens and the assumed history length");
  bool motion_ok = true;
  for (uint32_t i = 0; i < 12; ++i) motion_ok = motion_ok && f(p::kMotion + i) == float(i) * 0.5f;
  Check(motion_ok, "PUSH: the 3 x 4 motion in words 9 to 20");
  Check(w[22] == 0 && w[31] == 0 && p::kLength == 21 && p::kMotion == 9 && gp::MakePush({})[p::kSize] == 0, "PUSH: the words after the length are zero");
}

void MotionChecks() {
  Lcg rng;
  const auto pose = [&rng]() {
    gp::CameraPose p;
    // a random rotation from two random orthogonal vectors
    gi::Vec3 f = rng.Direction();
    gi::Vec3 helper = std::fabs(f.y) < 0.9f ? gi::Vec3{0, 1, 0} : gi::Vec3{1, 0, 0};
    gi::Vec3 r = {f.y * helper.z - f.z * helper.y, f.z * helper.x - f.x * helper.z, f.x * helper.y - f.y * helper.x};
    const float rl = std::sqrt(gi::Dot(r, r));
    r = r * (1.0f / rl);
    gi::Vec3 u = {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
    const float ul = std::sqrt(gi::Dot(u, u));
    u = u * (1.0f / ul);
    const float vals[4][3] = {{rng.Range(-500, 500), rng.Range(0, 80), rng.Range(-500, 500)}, {r.x, r.y, r.z}, {u.x, u.y, u.z}, {f.x, f.y, f.z}};
    for (int a = 0; a < 3; ++a) {
      p.position[a] = vals[0][a];
      p.right[a] = vals[1][a];
      p.up[a] = vals[2][a];
      p.forward[a] = vals[3][a];
    }
    return p;
  };
  // the same pose: the identity
  {
    const gp::CameraPose a = pose();
    float m[12];
    gp::MotionRows(a, a, m);
    const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    bool ok = true;
    for (int i = 0; i < 12; ++i) ok = ok && Near(m[i], id[i], 1e-5);
    Check(ok, "MOTION: the same pose moves nothing");
  }
  // random poses against a brute force: world point from the current view, then into the last view
  bool ok = true;
  for (int t = 0; t < 400 && ok; ++t) {
    const gp::CameraPose cur = pose();
    gp::CameraPose prev = cur;
    for (int a = 0; a < 3; ++a) prev.position[a] += rng.Range(-3, 3);   // a step of the camera: the common case; then also a different pose
    if (t % 2 == 1) prev = pose();
    float m[12];
    gp::MotionRows(cur, prev, m);
    const double q[3] = {rng.Range(-20, 20), rng.Range(-10, 10), rng.Range(0.5f, 100)};
    double world[3], want[3];
    for (int a = 0; a < 3; ++a) world[a] = cur.position[a] + q[0] * cur.right[a] + q[1] * cur.up[a] + q[2] * cur.forward[a];
    const double* axes[3] = {prev.right, prev.up, prev.forward};
    for (int r = 0; r < 3; ++r) {
      want[r] = 0.0;
      for (int a = 0; a < 3; ++a) want[r] += axes[r][a] * (world[a] - prev.position[a]);
      const double got = double(m[r * 4]) * q[0] + double(m[r * 4 + 1]) * q[1] + double(m[r * 4 + 2]) * q[2] + double(m[r * 4 + 3]);
      ok = ok && Near(got, want[r], 2e-3 + 2e-5 * std::fabs(want[r]));
    }
  }
  Check(ok, "MOTION: the 3 x 4 takes a point of this view into the last one (400 random pairs of poses, against the world point)");
  // a pure forward step of the camera: a point straight ahead gets nearer by the step in the OLD view... (new camera 2 m ahead of the old: a point 10 m ahead of the new one is 12 m ahead of the old one)
  gp::CameraPose a, b;
  b.position[2] = 2.0;
  float m[12];
  gp::MotionRows(b, a, m);
  Check(Near(m[2 * 4 + 3], 2.0, 1e-6) && Near(m[2 * 4 + 2], 1.0, 1e-6), "MOTION: a camera that stepped 2 m forward: what is 10 m ahead of it was 12 m ahead of the last pose");
  Check(gp::AssumedLength(0) == 1.0f && gp::AssumedLength(5) == 6.0f && gp::AssumedLength(1000) == mcla::gpu::rt_denoise::kMaxLength, "LENGTH: the history length the spatial pass assumes is frames + 1, at most kMaxLength");
}

void LayoutChecks() {
  Check(gp::kRowsOffsetBytes == ga::kBufferBytes && gp::kBufferBytes == ga::kBufferBytes + gp::kPixelRows * 16 && gp::kBufferAlloc >= gp::kBufferBytes && gp::kBufferAlloc % 256 == 0,
        "LAYOUT: the pixel rows follow the apply's buffer (the field's 20 rows and its 9) and fit the upload buffer");
  Check(gp::kPixelRows == 3 && gp::kBufferBytes == 512 && gp::kBufferAlloc == 512, "LAYOUT: three rows after the apply's 29 (the surface lights' rows are gone: the world frame's emitters are the cars' lamps)");
  Check(sizeof(gp::Push) == sizeof(ga::Push) && sizeof(gp::Push) == 32 && gp::kPushSpace == 5, "LAYOUT: the push block is the apply's shape, 32 bytes, at space 5");
  const std::set<uint32_t> binds = {gp::kBindRows, gp::kBindSignal, gp::kBindDepth, gp::kBindNormal, gp::kBindHit, gp::kBindStats};
  Check(binds.size() == 6 && gp::kSetViews == 6, "LAYOUT: six distinct registers in the pass set, and D3D12 is told six views");
  Check(gp::kTextureSet == 0 && gp::kTextureMsSet == 1 && gp::kSamplerSet == 2 && gp::kPassSet == 3 && gp::kSceneSet == 4, "LAYOUT: the sets are in the order the shader declares them");
  const std::set<uint32_t> stats = {gp::kStatPixels, gp::kStatHits, gp::kStatCarHits, gp::kStatMisses, gp::kStatSkipped, gp::kStatNoData};
  Check(stats.size() == 6 && *stats.rbegin() < gp::kStatWords && gp::kStatBytes == gp::kStatWords * 4, "LAYOUT: the counters are distinct words inside the counter buffer");
  // the field's frame: row 7 is zero without the pixel product (the frame's rows are bit for bit what they were) and carries the guides with it
  const double camera[3] = {10.0, 2.0, -5.0};
  gi::GiFrame fr = gi::MakeFrame(gi::BudgetOf(gi::Layout::kExtended), camera);
  fr.slot = {10, 11, 12, gi::kNoSlot};
  fr.valid = gi::kFieldProducts;
  const gi::GpuGiFrame before = gi::MakeGpuFrame(fr);
  Check(before.words[gi::kRowPixel * 4] == 0 && before.words[gi::kRowPixel * 4 + 1] == 0 && before.words[gi::kRowPixel * 4 + 2] == 0 && before.words[gi::kRowPixel * 4 + 3] == 0 && (before.words[0] & gi::kFlagPixelPass) == 0,
        "FRAME: without the product row 7 is zero and the flag is clear");
  gi::GiFrame shown = fr;
  shown.slot[gi::kPixelIrradiance] = 77;
  shown.valid |= gi::ProductBit(gi::kPixelIrradiance);
  shown.pixel.depth_slot = 78;
  shown.pixel.normal_slot = 79;
  shown.pixel.width = 960;
  shown.pixel.height = 540;
  shown.pixel.divisor = 2;
  const gi::GpuGiFrame after = gi::MakeGpuFrame(shown);
  Check((after.words[0] & gi::kFlagPixelPass) != 0 && after.words[gi::kRowSlots * 4 + 3] == 77, "FRAME: with it the flag is set and the image's slot is in row 1");
  Check((after.words[gi::kRowPixel * 4] & 0xFFFFu) == 78 && (after.words[gi::kRowPixel * 4] >> 16) == 79 && (after.words[gi::kRowPixel * 4 + 1] & 0xFFFFu) == 960 && (after.words[gi::kRowPixel * 4 + 1] >> 16) == 540 &&
            after.words[gi::kRowPixel * 4 + 2] == 2 && after.words[gi::kRowPixel * 4 + 3] == 0,
        "FRAME: and row 7 holds the guides' slots, the image's size and the divisor");
  bool rest_same = true;
  for (uint32_t i = 0; i < gi::kGpuFrameRows * 4; ++i) {
    const bool pixel_word = i < 4 /*flags*/ || (i >= gi::kRowSlots * 4 && i < gi::kRowSlots * 4 + 4) || (i >= gi::kRowPixel * 4 && i < gi::kRowPixel * 4 + 4);
    rest_same = rest_same && (pixel_word || before.words[i] == after.words[i]);
  }
  Check(rest_same, "FRAME: nothing else of the rows changes");
  // the apply's own rows follow the field's: the pixel pass reads the same buffer
  Check(gi::kGpuFrameRows == 20 && ga::kApplyRows == 9 && GIP_ROW_BASE == gi::kGpuFrameRows + ga::kApplyRows, "FRAME: GIP_ROW_BASE is the field's 20 rows and the apply's 9");
}

// ---------------------------------------------------------------------------
// the .hlsli compiled as C++ against the twin
// ---------------------------------------------------------------------------

// CITY-RT G6: the raw mode. The plan (the denoiser skipped, the pass still running, every earlier reason first), the weight (1 where traced, whatever the history), the history rule, the consumer rule, the row's raw bit and
// the mix a raw frame makes.
void RawChecks() {
  gp::Facts all;
  all.layout = gi::Layout::kExtended;
  all.field_present = all.scene_ok = all.world_ok = all.hit_data_ok = all.planes_ok = all.pipelines_ready = all.resources_ok = true;
  // the plan over both raw values: raw changes nothing but the denoiser, and only where the pass runs
  for (bool raw : {false, true}) {
    gp::Facts f = all;
    f.raw = raw;
    const gp::Plan p = gp::Decide(f);
    Check(p.runs() && p.raw == raw && p.denoise == !raw, std::string("RAW: the pass runs in a ") + (raw ? "raw" : "plain") + " frame, the denoiser only in the plain one");
  }
  bool never_raw_when_not_running = true;
  for (gi::Layout layout : {gi::Layout::kNone, gi::Layout::kLean, gi::Layout::kStandard}) {
    gp::Facts f = all;
    f.layout = layout;
    f.raw = true;
    const gp::Plan p = gp::Decide(f);
    never_raw_when_not_running = never_raw_when_not_running && !p.runs() && !p.raw && !p.denoise;
  }
  Check(never_raw_when_not_running, "RAW: a layout without the pass is not asked even in a frame Ray Reconstruction takes (the identity holds)");
  {
    gp::Facts f = all;
    f.raw = true;
    f.pipelines_ready = false;
    Check(!gp::Decide(f).runs() && !gp::Decide(f).raw && gp::Decide(f).why == gp::Why::kPipelines, "RAW: with the pipelines still being made nothing runs, raw or not");
    f = all;
    f.raw = true;
    f.debug = 0;
    Check(!gp::Decide(f).runs() && !gp::Decide(f).raw, "RAW: the pass's own switch at 0 stops it, raw or not");
  }
  // the weight: the whole where traced, nothing where not, and it does not depend on the history the filter would have made
  Check(gp::kRawWeight == 1.0f && gp::RawWeight(12.5f) == 1.0f && gp::RawWeight(0.0f) == 0.0f && gp::RawWeight(-1.0f) == 0.0f && gp::RawWeight(std::nanf("")) == 0.0f && gp::RawWeight(0.001f) == 1.0f,
        "RAW: the weight of the 1 spp term is 1 wherever the depth guide holds a distance and 0 where the pass traced nothing (a NaN is nothing)");
  Check(gp::Weight(1.0f, 10.0f) == 0.0f && gp::RawWeight(10.0f) == 1.0f && gp::Weight(0.0f, 10.0f) == 0.0f, "RAW: a plain frame's weight of a fresh pixel is 0 and a raw frame's is 1 (the signal's alpha is 0 and means no length)");
  {
    const gi::Vec3 field = {0.2f, 0.3f, 0.4f}, pixel = {1.5f, 0.0f, 3.0f};
    const gi::Vec3 traced = gp::Mix(field, pixel, gp::RawWeight(7.0f)), untraced = gp::Mix(field, pixel, gp::RawWeight(0.0f));
    Check(NearV(traced, pixel, 1e-6) && SameBits(untraced.x, field.x) && SameBits(untraced.y, field.y) && SameBits(untraced.z, field.z),
          "RAW: a raw frame's mix is the sample itself where the pass traced and the field's bits where it did not");
  }
  // the history: a raw frame is not the next frame's history
  Check(gp::KeepsHistory(false) && !gp::KeepsHistory(true), "RAW: a plain frame is the next frame's history for the denoiser, a raw one is not (the denoiser starts again after it)");
  // the consumer: only the extended layout has the pass; the pass's cell and the consumer's word are one
  {
    bool one_word = true;
    int wanted = 0;
    for (gi::Rung rung : {gi::Rung::kNone, gi::Rung::kWeak, gi::Rung::kStrong}) {
      for (gi::Level level : {gi::Level::kOff, gi::Level::kHigh, gi::Level::kUltra}) {
        const gi::Layout layout = gi::LayoutFor(rung, level);
        one_word = one_word && gp::ConsumerWanted(layout) == gp::CellFor(rung, level).runs;
        wanted += gp::ConsumerWanted(layout) ? 1 : 0;
      }
    }
    Check(one_word && wanted == 2, "RAW: the consumer is wanted exactly where the pass's cell runs (both RT tiers at Ultra), whatever the rest of the frame is");
    Check(!gp::ConsumerWanted(gi::Layout::kNone) && !gp::ConsumerWanted(gi::Layout::kLean) && !gp::ConsumerWanted(gi::Layout::kStandard) && gp::ConsumerWanted(gi::Layout::kExtended),
          "RAW: only the extended layout makes the bounce light a consumer of the DENOISER row (HIGH and OFF never are)");
  }
  // the row's raw bit (gi_frame.h row 7, word w): zero for a plain frame (the identity), one for a raw one, and nothing else of the frame moves
  {
    gi::GiFrame shown;
    shown.valid = gi::ProductBit(gi::kIrradiance) | gi::ProductBit(gi::kMoments) | gi::ProductBit(gi::kProbeWeight) | gi::ProductBit(gi::kPixelIrradiance);
    shown.slot[gi::kPixelIrradiance] = 77;
    shown.pixel.depth_slot = 78;
    shown.pixel.normal_slot = 79;
    shown.pixel.width = 960;
    shown.pixel.height = 540;
    shown.pixel.divisor = 2;
    const gi::GpuGiFrame plain = gi::MakeGpuFrame(shown);
    shown.pixel.raw = true;
    const gi::GpuGiFrame raw = gi::MakeGpuFrame(shown);
    bool only_the_bit = true;
    for (size_t i = 0; i < plain.words.size(); ++i) {
      if (i == size_t(gi::kRowPixel) * 4 + 3) continue;
      only_the_bit = only_the_bit && plain.words[i] == raw.words[i];
    }
    Check(plain.words[gi::kRowPixel * 4 + 3] == 0 && raw.words[gi::kRowPixel * 4 + 3] == 1 && only_the_bit, "RAW: row 7's last word is the raw bit (0 in a plain frame, as before G6), and nothing else of the frame's rows moves");
    gi::GiFrame none;
    none.pixel.raw = true;   // a frame without the pixel image never writes row 7 (raw or not)
    const gi::GpuGiFrame empty = gi::MakeGpuFrame(none);
    Check(empty.words[gi::kRowPixel * 4 + 3] == 0 && gi::PixelParams{}.raw == false, "RAW: a frame without the pixel image has a zero row 7 even if the flag was left set, and the default is not raw");
  }
  // the log line
  Check(gp::RawLine(true) != gp::RawLine(false) && gp::RawLine(true).find("raw") != std::string::npos && gp::RawLine(false).find("plain") != std::string::npos, "RAW: the log line says which state the pass is in");
}

void TwinChecks() {
  Check(GIP_FORMAT_VERSION == int(gp::kFormatVersion) && GIP_REACH_M == gp::kReachM && GIP_MAX_RADIANCE == gp::kMaxRadiance && GIP_CAR_ALBEDO == gp::kCarAlbedo && GIP_MISS_SHARE == gp::kMissShare &&
            GIP_FULL_LENGTH == gp::kFullLength && GIP_MAX_WEIGHT == gp::kMaxWeight && GIP_RAW_WEIGHT == gp::kRawWeight && GIP_MISS_DISTANCE == gp::kMissDistance && GIP_COSINE_FLOOR == gp::kCosineFloor && GIP_R2_A1 == gp::kR2A1 && GIP_R2_A2 == gp::kR2A2 &&
            GIP_TWO_PI == gp::kTwoPi && GIP_FLAG_STATS == gp::kFlagStats && GIP_MIN_TAP_WEIGHT == gp::kMinTapWeight && GIP_STAT_PIXELS == gp::kStatPixels &&
            GIP_STAT_HITS == gp::kStatHits && GIP_STAT_CAR_HITS == gp::kStatCarHits && GIP_STAT_MISSES == gp::kStatMisses && GIP_STAT_SKIPPED == gp::kStatSkipped && GIP_STAT_NO_DATA == gp::kStatNoData,
        "TWIN: every #define of the .hlsli is the header's number");
  Lcg rng;
  int bad = 0;
  const auto check = [&bad](bool ok) { bad += ok ? 0 : 1; };
  for (int i = 0; i < 4000; ++i) {
    const uint32_t x = rng.U32();
    check(hlsl_shim::GipHash(x) == gp::Hash(x));
    check(SameBits(hlsl_shim::GipUnit(x), gp::Unit(x)));
  }
  Check(bad == 0, "TWIN: the hash and the unit number are the same bits on random inputs");
  bad = 0;
  for (int i = 0; i < 4000; ++i) {
    const uint32_t x = rng.U32() & 0x7FFu, y = rng.U32() & 0x7FFu, f = rng.U32();
    const gi::Vec2 a = gp::Noise(x, y, f);
    const hlsl_shim::float2 b = hlsl_shim::GipNoise(x, y, f);
    check(SameBits(a.x, b.x) && SameBits(a.y, b.y));
  }
  Check(bad == 0, "TWIN: the pixel's noise is the same bits");
  bad = 0;
  for (int i = 0; i < 4000; ++i) {
    const gi::Vec3 n = rng.Direction();
    const float u1 = rng.Next(), u2 = rng.Next();
    check(NearV(C3(hlsl_shim::GipCosineDirection(H3(n), u1, u2)), gp::CosineDirection(n, u1, u2), 1e-5));
    const gp::Basis tb = gp::MakeBasis(n);
    const hlsl_shim::GipBasis hb = hlsl_shim::GipMakeBasis(H3(n));
    check(NearV(C3(hb.t), tb.t, 1e-6) && NearV(C3(hb.b), tb.b, 1e-6));
  }
  Check(bad == 0, "TWIN: the cosine direction and the frame about a normal");
  bad = 0;
  for (int i = 0; i < 4000; ++i) {
    const gi::Vec3 c = {rng.Range(-2, 50), rng.Range(-2, 50), rng.Range(-2, 50)};
    check(NearV(C3(hlsl_shim::GipClampRadiance(H3(c))), gp::ClampRadiance(c), 1e-6));
    check(NearV(C3(hlsl_shim::GipMissRadiance(H3(c))), gp::MissRadiance(c), 1e-6));
    const gi::Vec3 a = {rng.Range(0, 1), rng.Range(0, 1), rng.Range(0, 1)}, s = {rng.Range(0, 3), rng.Range(0, 3), rng.Range(0, 3)};
    const float ndl = rng.Range(-1, 1);
    check(NearV(C3(hlsl_shim::GipCarRadiance(H3(a), H3(s), ndl)), gp::CarRadiance(a, s, ndl), 1e-6));
    const bool traced = rng.Next() > 0.2f, hit = rng.Next() > 0.4f;
    const float len = rng.Range(0, 9000);
    check(SameBits(hlsl_shim::GipHitDistance(traced, hit, len), gp::HitDistance(traced, hit, len)));
    check(SameBits(hlsl_shim::GipOriginLift(len), gp::OriginLift(len)));
    const float length = rng.Range(0, 20), depth = rng.Next() > 0.2f ? rng.Range(0.1f, 100) : 0.0f;
    check(SameBits(hlsl_shim::GipWeight(length, depth), gp::Weight(length, depth)));
    check(SameBits(hlsl_shim::GipRawWeight(depth), gp::RawWeight(depth)));
    const gi::Vec3 p = {rng.Range(0, 9), rng.Range(0, 9), rng.Range(0, 9)};
    const float w = rng.Next() > 0.3f ? rng.Next() : 0.0f;
    check(NearV(C3(hlsl_shim::GipMix(H3(c), H3(p), w)), gp::Mix(c, p, w), 1e-6, 1e-6));
    const hlsl_shim::float3 junk = hlsl_shim::float3(std::nanf(""), INFINITY, 1e30f);
    const hlsl_shim::float3 kept = hlsl_shim::GipMix(H3(c), junk, 0.0f);   // THE IDENTITY in the shader's own text: a weight of 0 is the field's bits, whatever the pixel term holds
    check(SameBits(kept.x, c.x) && SameBits(kept.y, c.y) && SameBits(kept.z, c.z));
    const float bil = rng.Next(), dw = rng.Next(), nw = rng.Next(), td = rng.Next() > 0.3f ? rng.Range(0.1f, 50) : 0.0f;
    check(SameBits(hlsl_shim::GipTapWeight(bil, td, dw, nw), gp::TapWeight(bil, td, dw, nw)));
  }
  Check(bad == 0, "TWIN: the clamps, the miss, the car, the hit distance, the lift, the weight (plain and raw), the mix and the tap");
}

// ---------------------------------------------------------------------------
// the sources: the shader, the host, the apply, the build, the order, the wiring
// ---------------------------------------------------------------------------

std::string g_dir;

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
size_t CountOf(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

void SourceChecks() {
  std::string trace, common, apply_ps, apply_cpp, apply_h, host_h, host_cpp, cmake, shaders_h, order_h, renderer_cpp, renderer_h, precache_h, rr_rules, gi_common, frame_h, rules_h, denoise_ps, denoise_pt;
  const bool read = Slurp(g_dir + "/hlsl/gi_pixel_trace_cs.hlsl", &trace) && Slurp(g_dir + "/hlsl/gi_pixel_common.hlsli", &common) && Slurp(g_dir + "/hlsl/gi_apply_ps.hlsl", &apply_ps) &&
                    Slurp(g_dir + "/gi_apply.cpp", &apply_cpp) && Slurp(g_dir + "/gi_apply.h", &apply_h) && Slurp(g_dir + "/gi_pixel.h", &host_h) && Slurp(g_dir + "/gi_pixel.cpp", &host_cpp) &&
                    Slurp(g_dir + "/CMakeLists.txt", &cmake) && Slurp(g_dir + "/host_shaders.h", &shaders_h) && Slurp(g_dir + "/frame_order.h", &order_h) && Slurp(g_dir + "/renderer.cpp", &renderer_cpp) &&
                    Slurp(g_dir + "/renderer.h", &renderer_h) && Slurp(g_dir + "/host_precache.h", &precache_h) && Slurp(g_dir + "/dlss_rr_rules.h", &rr_rules) && Slurp(g_dir + "/hlsl/gi_common.hlsli", &gi_common) &&
                    Slurp(g_dir + "/gi_frame.h", &frame_h) && Slurp(g_dir + "/gi_pixel_rules.h", &rules_h) && Slurp(g_dir + "/hlsl/rt_denoise_plain_spatial_cs.hlsl", &denoise_ps) &&
                    Slurp(g_dir + "/hlsl/rt_denoise_plain_temporal_cs.hlsl", &denoise_pt);
  Check(read, "SOURCES: every file the checks read could be read (run with the src/gpu folder as the argument)");
  if (!read) return;

  // the shader
  Check(Has(trace, "register(t0, space3)") && Has(trace, "register(u1, space3)") && Has(trace, "register(u2, space3)") && Has(trace, "register(u3, space3)") && Has(trace, "register(u4, space3)") &&
            Has(trace, "register(u5, space3)") && Has(trace, "register(t0, space0)") && Has(trace, "register(t0, space1)") && Has(trace, "register(s0, space2)") && Has(trace, "register(b0, space5)") &&
            Has(trace, "#define CAR_RT_SPACE 4") && Has(trace, "#define RT_WORLD_SPACE 4") && Has(trace, "#define CITY_HIT_SPACE 4"),
        "SHADER: the registers are the layout's (the bindless set, its multisampled twin, the samplers, the pass set 3, the ray tracing set 4, the push block at space 5)");
  Check(Has(trace, "[numthreads(8, 8, 1)]") && Has(trace, "0x80u | CAR_RT_MASK_ALL") && Has(trace, "#define CITY_HIT_LAMP_GRID") && Has(trace, "CityHitShadeHit(params, f, ray, hit.inst, decoded)") &&
            Has(trace, "CITY_HIT_MODE_GI") && Has(trace, "CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE") && Has(trace, "CITY_HIT_P_LAMPS | CITY_HIT_P_LAMP_SHADOW") && Has(trace, "#define CITY_HIT_FEEDBACK(P, N) GipFeedback((P), (N))") &&
            Has(trace, "#define CITY_HIT_EMITTER_COUNT (g_world.emitterCount)") && Has(trace, "#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)"),
        "SHADER: one cosine ray into the city and the cars, shaded by the library's GI mode (the sun, one lamp of the world frame's grid with its shadow ray, the glow, the world frame's emitters, the field's second bounce), as the probes' rays are");
  Check(Has(trace, "#include \"light_laws.hlsli\"") && Has(trace, "#include \"rt_emitters.hlsli\"") && Has(trace, "params.flags = params.flags | CITY_HIT_P_EMITTERS | CITY_HIT_P_EMITTER_SHADOW;") && !Has(trace, "GipLights(") &&
            !Has(trace, "GIP_ROW(3u + 4u * i)") && !Has(trace, "g_lightCount"),
        "SHADER: the cars' lamps are the world frame's emitters through the laws with one shadow ray (the pass's own headlight rows are gone)");
  Check(Has(trace, "(asuint(g_textures[g_push.slots.w].Load(int3(tex, 0)).x) & 1u) == 0u") && Has(trace, "GbRt1Valid(rt1)") && Has(trace, "rt2.w > 0.0f") && Has(trace, "GiaLinearDepth(stored, fr.lin)"),
        "SHADER: a pixel is signal where the apply would light it (a normal, an albedo, a lit weight, geometry) and is not a vehicle's");
  Check(Has(trace, "const GiSample s = GiLookup(pos, n);") && Has(trace, "GipMissRadiance(s.irradiance)") && Has(trace, "GipCarRadiance(") && Has(trace, "CarRtHitNormal("),
        "SHADER: a miss carries the field's irradiance, a car's hit a neutral diffuse");
  Check(Has(trace, "g_signal[p] = float4(radiance, 0.0f)") && Has(trace, "g_depth[p] = t;") && Has(trace, "g_normal[p] = float4(nv, 1.0f)") && Has(trace, "g_hit[p] = float2(GipHitDistance(true, anyHit, hit.t), 1.0f)"),
        "SHADER: the signal and the three guides are written (roughness 1, the hit distance as Ray Reconstruction's raw mode wants it, the share 1)");
  Check(!Has(trace, "CityHitResolve(") && !Has(trace, "CITY_HIT_P_SCREEN") && Has(rules_h, "G5-c"), "SHADER: the picture's colour (ResolveHit's screen term) is not read: the checked limitation G5-c");

  // the apply
  Check(Has(apply_ps, "#include \"gi_pixel_common.hlsli\"") && CountOf(apply_ps, "GiaPixelTerm(") == 3 && Has(apply_ps, "GiaLookupNetMixed(s, n, fr, GiaPixelTerm(p, t, n, fr))") &&
            Has(apply_ps, "GiaLookupNetMixed(s, n, fr, GiaPixelTerm(pixel, t, n, fr))") && Has(apply_ps, "if (!GiPixelPresent())") && Has(apply_ps, "GipTapWeight(bilinear, d, GiaDepthWeight(t, d), GiaNormalWeight(nv, gn))"),
        "APPLY: the lookup (at the traced size and at the picture's) mixes the pass's term in through GipMix, reading nothing where the frame has no pixel image; the reduced image is read by four taps");
  Check(Has(common, "GipMix(float3 eField, float3 ePixel, float w)") && Has(apply_ps, "GipMix(s.irradiance,"), "APPLY: the mix is the twin's");
  const size_t m_pixel = At(apply_cpp, "Mark(list, \"rt gi pixel\")"), m_dn = At(apply_cpp, "Mark(list, \"rt gi denoise\")"), m_look = At(apply_cpp, "Mark(list, \"gi lookup\")"), m_after = At(apply_cpp, "Mark(list, \"after gi apply\")");
  Check(m_pixel != std::string::npos && m_dn > m_pixel && m_look > m_dn && m_after > m_look, "APPLY: the marks are 'rt gi pixel', 'rt gi denoise', then the lookup's and the closing one, in that order");
  Check(Has(apply_cpp, "pixel_ != nullptr && pixel_->available() && gi::BudgetOf(gf.layout).pixel_rays_per_pixel != 0") && Has(apply_cpp, "pixel_ran = pixel_->Publish(&shown)") &&
            Has(apply_cpp, "const ga::Buffer buffer = pixel_ran ? ga::MakeBuffer(gi::MakeGpuFrame(shown), fr) : base_rows;") && Has(apply_h, "void SetGiPixel(GiPixel* pixel)"),
        "APPLY: the pass is asked only where the frame's layout has one; a frame without its image is the rows as they were");
  const size_t at_pixel_call = At(apply_cpp, "pixel_->Begin(pixel_in)"), at_depth = At(apply_cpp, "edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);"), at_push = At(apply_cpp, "ga::Push push = {};");
  Check(at_depth != std::string::npos && at_pixel_call > at_depth && at_push > at_pixel_call, "APPLY: the main depth is in the shader-read layout before the pixel pass, and the lookup's push block comes after it");

  // the host
  Check(Has(host_cpp, "facts.raw = in.raw;") && !Has(host_cpp, "facts.raw = false;") && !Has(host_cpp, "NoteConsumer(") && !Has(host_cpp, "kRtGi") && Has(host_h, "bool raw = false;") && Has(host_h, "RawModes().RtGi()"),
        "RAW: the host takes the frame's raw flag from its caller (GiApply) and does not register the consumer itself");
  Check(Has(rr_rules, "{\"ray traced bounce light\", true, false}") && !Has(rr_rules, "{\"ray traced bounce light\", false"), "RAW: the Ray Reconstruction table's bounce light has raw_mode_built = true and AUTO does not trust RR with it (CITY-RT G6)");
  {
    // the host's raw branch: the signal is the result, both images go to the read state of the stages that read them, the denoiser's history is not kept, the params carry the flag, a change is logged
    const size_t at_else = At(host_cpp, "} else if (plan_.raw) {");
    const size_t at_signal = At(host_cpp, "result_ = &signal_;");
    const size_t at_barrier = At(host_cpp, "const plume::RenderTextureBarrier raw_reads[2] = {{signal_.texture.get(), Layout::SHADER_READ}, {hit_.texture.get(), Layout::SHADER_READ}};");
    const size_t at_graphics = At(host_cpp, "list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, raw_reads, 2);");
    const size_t at_guides = At(host_cpp, "const plume::RenderTextureBarrier reads[2]");
    Check(at_else != std::string::npos && at_signal > at_else && at_barrier > at_signal && at_graphics > at_barrier && at_guides > at_graphics && At(host_cpp, "if (plan_.denoise) {") < at_else,
          "RAW: a raw frame (the branch after the denoiser's own) publishes the 1 spp signal and puts it and the hit image in the read state of both stages, before the guides' barrier");
    Check(Has(host_cpp, "signal_.layout = hit_.layout = Layout::SHADER_READ;") && Has(host_cpp, "params_.raw = plan.raw;") && Has(host_cpp, "have_prev_ = rules::KeepsHistory(plan_.raw) && !regen_ok;") && Has(host_cpp, "out.frame = frame_;") &&
              Has(host_cpp, "if (plan_.raw != raw_logged_) {") && Has(host_cpp, "rules::RawLine(plan_.raw)") && Has(host_h, "uint64_t frame = 0;") && Has(host_h, "bool raw_logged_ = false;"),
          "RAW: the images' layouts are tracked, the apply's row carries the flag, the denoiser starts again after a raw frame, the output names its frame, and a change of state is one log line");
    Check(CountOf(host_cpp, "denoise_.RecordTemporal(") == 1 && CountOf(host_cpp, "denoise_.RecordSpatial(") == 1 && At(host_cpp, "denoise_.RecordTemporal(") < at_else && At(host_cpp, "denoise_.RecordSpatial(") < at_else,
          "RAW: the denoiser's two stages are recorded only inside the plain branch (a raw frame records neither)");
  }
  {
    // the apply: the raw flag of the row picks the weight, in both reads; the plain weight is untouched
    Check(Has(apply_ps, "row.raw != 0u ? GipRawWeight(d) : GipWeight(e.w, d)") && Has(apply_ps, "(row.raw != 0u ? GipRawWeight(1.0f) : GipWeight(lengthSum / wSum, 1.0f)) * saturate(cover)") &&
              Has(common, "float GipRawWeight(float depthGuide)") && Has(common, "#define GIP_RAW_WEIGHT 1.0f") && Has(gi_common, "p.raw = r.w & 1u;") && Has(gi_common, "    uint raw;\n};"),
          "RAW: the apply's two reads (nearest, and the four depth and normal weighted taps) take GipRawWeight where the row says raw, the history weight otherwise; the row's last word is the flag");
    Check(Has(apply_cpp, "pixel_in.raw = raw_requested;") && Has(apply_cpp, "bool GiApply::OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot, bool raw_requested,") &&
              Has(apply_cpp, "const HostImage* GiApply::RawHitImage() const {") && Has(apply_cpp, "return out.valid && out.frame == frame_ ? out.hit : nullptr;") && Has(apply_h, "bool pixel_pass_wanted() const { return pixel_wanted_; }") &&
              Has(apply_h, "const HostImage* RawHitImage() const;") && Has(apply_h, "bool raw_requested = false,"),
          "RAW: GiApply hands the frame's raw flag to the pass, answers the guide pass with the hit image of THIS frame only (none where the pass did not run raw), and tells the Scene whether the pass is wanted");
    Check(Has(apply_cpp, "denoiser::NoteConsumer(rr::Consumer::kRtGi, pixel_wanted_);") && Has(apply_cpp, "gi_pixel::ConsumerWanted(field_->fit().wanted)") && Has(apply_cpp, "#include \"denoiser_setting.h\""),
          "RAW: the consumer is registered by GiApply::BeginFrame from the rules' word (the denoiser_setting test reads the order inside the function)");
  }
  {
    // the Scene, the temporal system, the guide pass and the setting
    std::string scene_cpp, temporal_cpp, temporal_h, settings_h, manifest, meta, guides_h, guides_cpp;
    const bool read2 = Slurp(g_dir + "/scene.cpp", &scene_cpp) && Slurp(g_dir + "/temporal.cpp", &temporal_cpp) && Slurp(g_dir + "/temporal.h", &temporal_h) && Slurp(g_dir + "/../settings/generated/settings_gpu.h", &settings_h) &&
                       Slurp(g_dir + "/../settings/generated/manifest.json", &manifest) && Slurp(g_dir + "/../settings/generated/settings_meta.inc", &meta) && Slurp(g_dir + "/rr_guides.h", &guides_h) &&
                       Slurp(g_dir + "/rr_guides.cpp", &guides_cpp);
    Check(read2, "RAW: the Scene, the temporal system, the guide pass and the three generated settings files could be read");
    if (read2) {
      Check(Has(scene_cpp, "const bool raw_gi = gi_apply_->pixel_pass_wanted() && temporal_ && temporal_->RawModes().RtGi();") && Has(scene_cpp, "gi_apply_->OnTrigger(summary, list, gbuffer_published, slot_, raw_gi,") &&
                Has(scene_cpp, "temporal_->NoteRrBounceHit(gi_apply_ && rr::BounceHitWanted(int(larecomp::settings::mcla_gpu_dlss_rr_rt_gi.Get())) ? gi_apply_->RawHitImage() : nullptr);") &&
                At(scene_cpp, "const bool raw_gi") < At(scene_cpp, "temporal_->NoteRrBounceHit(") && At(scene_cpp, "temporal_->NoteRrHit(") < At(scene_cpp, "temporal_->NoteRrBounceHit("),
            "RAW: the Scene asks the temporal system for a raw frame only while the pass is wanted, calls the apply with it, and hands the hit image over after the trigger's passes (the switch's value 2 leaves it out)");
      Check(Has(temporal_cpp, "in.bounce_hit = rr_bounce_hit_;") && Has(temporal_cpp, "rr_bounce_hit_ = nullptr;           // ... nor the bounce light's (CITY-RT G6)") && Has(temporal_h, "void NoteRrBounceHit(const HostImage* bounce) { rr_bounce_hit_ = bounce; }") &&
                Has(temporal_cpp, "rr::RawSwitchOn(int(larecomp::settings::mcla_gpu_dlss_rr_rt_gi.Get()))") && Has(temporal_cpp, "window_.rr_raw_frames[size_t(rr::RawEffect::kRtGi)] += modes.RtGi() ? 1 : 0;") &&
                Has(temporal_cpp, "frame_.raw_any = frame_.raw_any || modes.Ssr() || modes.CarRefl() || modes.RtGi() || modes.RtLights();"),   // RT-LIGHTS-D1 part 2 added the fourth
            "RAW: the temporal system hands the guide pass the image and forgets it at the next boundary, reads the third switch, counts the frames asked and counts a raw GI frame among the ones Super Resolution may have upscaled after all");
      bool setting_ok = false;
      {
        const std::string decl_key = std::string("REX_") + "SETTING(i32, mcla_gpu_dlss_rr_rt_gi, 1,";
        const size_t decl = settings_h.find(decl_key);
        const size_t end = decl == std::string::npos ? std::string::npos : settings_h.find(");\n", decl);
        const std::string body = decl == std::string::npos || end == std::string::npos ? std::string() : settings_h.substr(decl, end - decl);
        setting_ok = !body.empty() && Has(body, "rex::kDebugOnly, rex::Range{0, 2}") && !Has(body, "kRestart") && Has(settings_h, "inline auto& mcla_gpu_dlss_rr_rt_gi = g_gpu.mcla_gpu_dlss_rr_rt_gi;") &&
                     Has(manifest, "\"mcla_gpu_dlss_rr_rt_gi\": {") && Has(meta, "{\"mcla_gpu_dlss_rr_rt_gi\", \"MCLA/GPU\"},");
      }
      Check(setting_ok, "RAW: the A/B switch mcla_gpu_dlss_rr_rt_gi is debug-only, live, 0 to 2, on (1) by default, in the settings header, its alias, the manifest and the meta list");
      Check(Has(guides_h, "const HostImage* bounce_hit = nullptr;") && Has(guides_cpp, "push.info[3] = bounce_used ? bounce_used->slot : rr_guides::kNoSlot;"), "RAW: the guide pass takes the image (its own test reads the shader)");
    }
  }
  Check(Has(host_h, "RawOutput raw_output() const;") && Has(host_cpp, "out.signal = &signal_;") && Has(host_cpp, "out.hit = &hit_;") && Has(host_cpp, "plan_.raw"), "RAW: the signal and the hit distance image are handed out for a raw frame");
  Check(Has(host_cpp, "denoise_.Initialize(device, frames, heap, passes, /*instance=*/1, \"gi pixel\"") && Has(host_cpp, "RtDenoiseDiffuseShader(false)") && Has(host_cpp, "RtDenoiseDiffuseShader(true)") && Has(host_cpp, "denoise_.RecordTemporal(stage)") &&
            Has(host_cpp, "denoise_.RecordSpatial(stage)") && Has(host_cpp, "rt_denoise::plain::PatchFor()"), "DENOISER: RtDenoise instance 1 with the diffuse compile of the plain consumer, the temporal stage then the spatial one");
  const size_t at_spatial = At(host_cpp, "denoise_.RecordSpatial(stage)"), at_reads = At(host_cpp, "const plume::RenderTextureBarrier reads[2]");
  Check(at_spatial != std::string::npos && at_reads != std::string::npos && at_spatial < at_reads && Has(host_cpp, "list->barriers(Stage::GRAPHICS_AND_COMPUTE, nullptr, 0, reads, 2);"),
        "DENOISER: the guides get the read state of both stages AFTER the denoiser's own compute-only barriers (D3D12 keeps the last state)");
  Check(!Has(host_cpp, "PickLights") && !Has(host_cpp, "lights_") && Has(host_cpp, "world_->current().decoded.emitter_count") && Has(host_cpp, "HostPrecache::kGiPixel") && Has(host_cpp, "AddLive(") && Has(host_cpp, "history_ok_ = have_prev_ && prev_frame_ + 1 == in.frame") &&
            Has(host_cpp, "move.cut || move.relight") && Has(host_cpp, "const uint32_t scale = (in.traced[0] != in.render[0] || in.traced[1] != in.render[1]) ? 2u : 1u;"),
        "HOST: no surface lights of the manager's frame (the emitters), the pipelines made in the background, the history only from the previous frame without a cut, the traced-to-picture scale");
  Check(Has(host_cpp, "rules::kSetViews, 0, \"gi pixel set\"") && Has(host_cpp, "plume::RenderFormat::R32_FLOAT") && Has(host_cpp, "plume::RenderFormat::R16G16_FLOAT") && Has(host_cpp, "signal_.slot <= 0xFFFEu") && CountOf(host_cpp, "->setTexture(") == 4,
        "HOST: six views in the pass set, the four image formats the shader declares, 16 bit slots for the denoiser's words");
  Check(CountOf(host_cpp, "Mark(") == 2 && Has(host_cpp, "Mark(list, \"ray regen bounce denoise\")") && Has(host_cpp, "Mark(list, \"rt gi denoise\")"),
        "HOST: the vendor dispatch has a nested timing span, then returns to the GI denoise label");

  // the build
  Check(Has(cmake, "    gi_pixel.cpp\n") && Has(cmake, "larecomp_gpu_host_shader_rt(gi_pixel_trace_cs cs_6_5 DEFINES GIP_MS=0)") && Has(cmake, "larecomp_gpu_host_shader_rt(gi_pixel_trace_ms_cs cs_6_5 SOURCE gi_pixel_trace_cs DEFINES GIP_MS=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(rt_denoise_diffuse_spatial_cs cs_6_0 SOURCE rt_denoise_plain_spatial_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE)") &&
            Has(cmake, "larecomp_gpu_host_shader(rt_denoise_diffuse_temporal_cs cs_6_0 SOURCE rt_denoise_plain_temporal_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE)") && Has(cmake, "add_executable(larecomp_gi_pixel_test EXCLUDE_FROM_ALL gi_pixel_test.cpp)"),
        "BUILD: the host, the two trace compiles (shader model 6.5), the two diffuse denoiser compiles and the test are in CMakeLists.txt");
  Check(Has(denoise_ps, "-D RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE") && Has(denoise_pt, "RT_DN_PASS_TEMPORAL"), "BUILD: the plain stages' own words name the diffuse compile");
  for (const char* stem : {"gi_pixel_trace_cs", "gi_pixel_trace_ms_cs", "rt_denoise_diffuse_spatial_cs", "rt_denoise_diffuse_temporal_cs"}) {
    Check(CountOf(shaders_h, std::string("#include \"") + stem + ".hlsl.dxil.h\"") == 1 && CountOf(shaders_h, std::string("#include \"") + stem + ".hlsl.spirv.h\"") == 1, std::string("BUILD: host_shaders.h includes ") + stem + " once for each backend");
  }
  Check(Has(shaders_h, "GiPixelTraceShader(bool multisampled)") && Has(shaders_h, "RtDenoiseDiffuseShader(bool temporal)") && Has(shaders_h, "RtDenoisePlainShader(bool temporal)"), "BUILD: the accessors, the specular ones kept");

  // the key, the order, the wiring
  {
    const size_t begin = At(precache_h, "enum Owner : uint32_t {"), end = At(precache_h, "};", begin);
    bool unique = begin != std::string::npos && end != std::string::npos;
    std::set<int> values;
    for (size_t at = precache_h.find(" = ", begin); at != std::string::npos && at < end; at = precache_h.find(" = ", at + 3)) {
      const int v = std::atoi(precache_h.c_str() + at + 3);
      unique = unique && values.insert(v).second;
    }
    Check(unique && values.count(34) == 1 && Has(precache_h, "kGiPixel = 34,"), Num("KEY: the per-pixel pass's owner number 34 is unique among the host precache's %.0f owners", double(values.size())));
  }
  Check(Has(order_h, ".label = \"rt gi pixel\", .more_labels = {{\"rt gi denoise\", \"gi lookup\"}}, .closing_label = \"after gi apply\"") && Has(order_h, "data::kCarStructures | data::kLights"),
        "ORDER: the frame order's row for the apply starts with 'rt gi pixel', then 'rt gi denoise' and 'gi lookup', and soft-reads the car structures and the lights");
  Check(Has(renderer_cpp, "gi_pixel_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_field_, car_rt_, city_rt_, city_hit_, rt_world_);") && Has(renderer_cpp, "gi_apply_.SetGiPixel(&gi_pixel_);") &&
            Has(renderer_h, "GiPixel gi_pixel_;") && CountOf(renderer_cpp, "gi_pixel_.OnSubmitted(slot);") == CountOf(renderer_cpp, "gi_update_.OnSubmitted(slot);") && Has(renderer_cpp, "gi_pixel_.CensusText()") &&
            Has(renderer_cpp, "vram_.AddHolder(\"per-pixel bounce light\""),
        "RENDERER: made after the probe update, given to the apply, submitted where the update is, counted in the census and the memory holders");
  {
    size_t from = 0;
    int ordered = 0;
    while (true) {
      const size_t px = renderer_cpp.find("gi_pixel_.Shutdown();", from);
      if (px == std::string::npos) break;
      const size_t ap = renderer_cpp.find("gi_apply_.Shutdown();", px);
      const size_t fd = renderer_cpp.find("gi_field_.Shutdown();", px);
      const size_t cs = renderer_cpp.find("rt_world_.Shutdown();", px);   // the world frame it holds a pointer to goes after it
      ordered += (ap != std::string::npos && fd != std::string::npos && cs != std::string::npos && px < ap && ap < fd && px < cs) ? 1 : 0;
      from = px + 1;
    }
    Check(ordered == 2, "RENDERER: both shutdown paths stop the pass before the apply that holds it, the field and the world frame it holds pointers to");
  }
  Check(Has(frame_h, "f.Has(kPixelIrradiance) && f.pixel.width != 0") && Has(apply_ps, "row.width == 0u || row.height == 0u"), "FRAME: row 7 is written only where the image has a size, and the apply reads nothing of an image that has none");
  Check(Has(frame_h, "struct PixelParams {") && Has(frame_h, "kRowPixel = 7") && Has(gi_common, "#define GI_ROW_PIXEL 7u") && Has(gi_common, "GiPixelRow GiLoadPixelRow()") && Has(gi_common, "bool GiPixelPresent()"),
        "FRAME: the field's frame carries the pass's guides in the reserved row 7 (gi_frame.h and gi_common.hlsli)");
}

}  // namespace

int main(int argc, char** argv) {
  g_dir = "";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v") {
      g_verbose = true;
    } else {
      g_dir = a;
    }
  }
  if (g_dir.empty()) {
    std::string self = __FILE__;
    const size_t cut = self.find_last_of("/\\");
    g_dir = cut == std::string::npos ? "." : self.substr(0, cut);
  }
  CellChecks();
  PlanChecks();
  GovernorChecks();
  SizeChecks();
  RayChecks();
  EstimatorChecks();
  PushChecks();
  MotionChecks();
  LayoutChecks();
  TwinChecks();
  RawChecks();
  SourceChecks();
  std::printf("gi_pixel_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
