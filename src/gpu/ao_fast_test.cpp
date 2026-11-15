// Checks of ao_fast.h: what the cheaper forms of the occlusion's and the bounce light's shader maths change, on the C++ twins of the
// passes (ao_halo.h Visibility, ao_stable_twin.h GiMain: no renderer, no ReXGlue, no GPU), and that the shaders and ao.cpp repeat the
// header's constants.
//
//   clang++ -std=c++23 -O2 -I src/gpu src/gpu/ao_fast_test.cpp -o ao_fast_test && ao_fast_test <src/gpu dir> [--print]
//
// Without the folder argument the checks of the files are skipped (the maths run alone). Exit 0 = passed.

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "ao_fast.h"
#include "ao_halo.h"
#include "ao_stable_twin.h"

namespace {

using namespace mcla::gpu;

int g_failed = 0;
int g_checks = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}
void Say(const std::string& line) {
  if (g_print) {
    std::printf("%s\n", line.c_str());
  }
}
template <typename... Args>
std::string Num(const char* fmt, Args... args) {
  char buf[1024];
  std::snprintf(buf, sizeof(buf), fmt, double(args)...);
  return buf;
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------
// the polynomial acos
// ---------------------------------------------------------------------------

struct AcosStat {
  double max_error = 0.0;
  double at = 0.0;
  double mean_signed = 0.0;
};

AcosStat ScanAcos() {
  AcosStat s;
  const int kN = 2000000;
  double sum = 0.0;
  for (int i = 0; i <= kN; ++i) {
    const float c = float(-1.0 + 2.0 * double(i) / double(kN));
    const double exact = std::acos(double(c));
    const double e = double(ao_fast::AcosFast(c)) - exact;
    sum += e;
    if (std::fabs(e) > s.max_error) {
      s.max_error = std::fabs(e);
      s.at = c;
    }
  }
  s.mean_signed = sum / double(kN + 1);
  return s;
}

void TestAcos() {
  const AcosStat s = ScanAcos();
  Say(Num("acos polynomial: largest error %.3g rad at cos %.4f (the paper: 6.7e-5), mean signed error %.3g rad", s.max_error, s.at,
          s.mean_signed));
  Check(s.max_error <= ao_fast::kAcosMaxErrorRadians, Num("AcosFast: largest error %.3g rad is within %.3g", s.max_error, ao_fast::kAcosMaxErrorRadians));
  Check(std::fabs(s.mean_signed) < 1e-5, Num("AcosFast: the error has no bias (mean %.3g rad)", s.mean_signed));
  Check(ao_fast::AcosFast(1.0f) == 0.0f, "AcosFast(1) = 0");
  Check(std::fabs(ao_fast::AcosFast(-1.0f) - ao_fast::kPi) < 1e-6f, "AcosFast(-1) = pi");
  Check(std::fabs(ao_fast::AcosFast(0.0f) - 1.5707288f) < 1e-6f, "AcosFast(0) = the first coefficient");
  // outside -1..1: kept within by the minimum, as acos(clamp)
  Check(ao_fast::AcosFast(2.5f) == 0.0f && std::fabs(ao_fast::AcosFast(-3.0f) - ao_fast::kPi) < 1e-6f, "AcosFast: a cosine outside -1..1 is kept within");
  Check(ao_fast::AcosFast(std::nanf("")) == 0.0f && ao_fast::AcosExact(std::nanf("")) == 0.0f, "AcosFast and AcosExact: a NaN counts as 1 (angle 0)");
  // monotone: a polynomial that bent back would put a horizon the wrong way round
  bool monotone = true;
  float last = ao_fast::AcosFast(-1.0f);
  for (int i = 1; i <= 200000; ++i) {
    const float c = -1.0f + 2.0f * float(i) / 200000.0f;
    const float a = ao_fast::AcosFast(c);
    if (a > last + 1e-7f) {
      monotone = false;
    }
    last = a;
  }
  Check(monotone, "AcosFast falls as the cosine rises over -1..1");
}

// ---------------------------------------------------------------------------
// what the polynomial does to a sector position (AoSectorPos): the largest error in sectors, over every arc and every horizon
// ---------------------------------------------------------------------------

struct SectorStat {
  double max_sectors = 0.0;
  double mean_signed_sectors = 0.0;
  double at_n = 0.0, at_cos = 0.0;
  int count = 0;
};

SectorStat ScanSectors() {
  SectorStat st;
  double sum = 0.0;
  for (int i = -400; i <= 400; ++i) {  // the slice's normal angle n, -pi/2 .. pi/2
    const float n = float(double(i) / 400.0 * 1.57079632679490);
    const float cos_n = std::fmax(std::cos(n), 0.0f);
    const float sin_n = std::sin(n);
    const float total = cos_n + n * sin_n;
    const float inv_total = 1.0f / std::max(total, 1e-3f);
    const float g_end = (2.0f * cos_n + (2.0f * n - ao_halo::kPi) * sin_n) * 0.25f;
    for (int side = -1; side <= 1; side += 2) {
      const float low = side > 0 ? -sin_n : sin_n;  // the surface's own tangent on that side
      for (int j = 0; j <= 1000; ++j) {
        const float cos_front = low + (1.0f - low) * float(j) / 1000.0f;  // from the tangent to straight at the viewer
        const float exact = ao_stable_twin::SectorPos(float(side) * ao_fast::AcosExact(cos_front), n, sin_n, cos_n, g_end, inv_total);
        const float fast = ao_stable_twin::SectorPos(float(side) * ao_fast::AcosFast(cos_front), n, sin_n, cos_n, g_end, inv_total);
        const double e = (double(fast) - double(exact)) * 32.0;
        sum += e;
        ++st.count;
        if (std::fabs(e) > st.max_sectors) {
          st.max_sectors = std::fabs(e);
          st.at_n = n;
          st.at_cos = cos_front;
        }
      }
    }
  }
  st.mean_signed_sectors = sum / double(st.count);
  return st;
}

void TestSectors() {
  const SectorStat s = ScanSectors();
  Say(Num("sector position: largest error %.5f of one of the 32 sectors (arc normal %.3f, cosine %.3f), mean signed %.2g", s.max_sectors,
          s.at_n, s.at_cos, s.mean_signed_sectors));
  // total >= 1 and the arc's density <= 1: the error in sectors is at most 32 x the angle's error
  Check(s.max_sectors <= 32.0 * ao_fast::kAcosMaxErrorRadians + 1e-3, Num("the polynomial moves a sector edge by at most %.5f sectors", s.max_sectors));
  Check(s.max_sectors < 0.0025, "the polynomial moves a sector edge by under 0.0025 of a sector");
  Check(std::fabs(s.mean_signed_sectors) < 2e-4, Num("the polynomial's sector error has no bias (mean %.2g sectors)", s.mean_signed_sectors));
}

// ---------------------------------------------------------------------------
// the blur's weight: one exponential for the two
// ---------------------------------------------------------------------------

void TestBlurWeight() {
  double worst_rel = 0.0, worst_abs = 0.0;
  double worst_rel_new = 0.0, worst_abs_new = 0.0;
  const float sigmas[] = {0.9f, 1.4f, 1.8f, 2.2f};  // ao_common.hlsli AoBlurSigma
  const int d2s[] = {0, 1, 2, 4, 5, 8, 9, 10, 13, 16, 18, 20, 25, 32};
  for (const float sigma : sigmas) {
    const float inv_sq = 1.0f / (sigma * sigma);
    for (const int d2 : d2s) {
      for (int k = 0; k <= 3000; ++k) {
        const float off = float(k) * 0.005f;  // the tap's distance from the plane in sigmas, 0 .. 15
        const double a = double(off) * double(off);
        const double b = double(d2) / (double(sigma) * double(sigma));
        const double ref = std::exp(-0.5 * a) * std::exp(-0.5 * b);
        const double w_new = double(ao_fast::BlurWeight(off, float(d2), inv_sq));
        const double w_old = double(ao_fast::BlurWeightTwoExp(off, float(d2), sigma));
        worst_abs_new = std::max(worst_abs_new, std::fabs(w_new - ref));
        worst_abs = std::max(worst_abs, std::fabs(w_old - ref));
        if (ref > 1e-12) {
          worst_rel_new = std::max(worst_rel_new, std::fabs(w_new - ref) / ref);
          worst_rel = std::max(worst_rel, std::fabs(w_old - ref) / ref);
        }
      }
    }
  }
  Say(Num("blur weight against the exact: two exponentials %.2g relative / %.2g absolute, one exponential %.2g relative / %.2g absolute",
          worst_rel, worst_abs, worst_rel_new, worst_abs_new));
  Check(worst_abs_new < 5e-7, Num("one exponential: largest weight error %.2g (under 5e-7)", worst_abs_new));
  Check(worst_rel_new < 2e-5, Num("one exponential: largest relative weight error %.2g (under 2e-5, down to weights of 1e-12)", worst_rel_new));
  Check(worst_abs_new < 4.0 * worst_abs + 1e-7, "one exponential is as exact as the two it replaces");

  // a whole blur: 7x7 taps of a random visibility and depth, the old and the new weights
  uint32_t seed = 12345u;
  const auto rnd = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  double worst_blur = 0.0;
  for (int trial = 0; trial < 20000; ++trial) {
    const int radius = 1 + trial % 3;
    const float sigma = radius == 1 ? 0.9f : (radius == 2 ? 1.4f : 1.8f);
    const float inv_sq = 1.0f / (sigma * sigma);
    float sum_old = 0, w_old_sum = 0, sum_new = 0, w_new_sum = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        const float off = rnd() * (trial % 5 == 0 ? 6.0f : 1.5f);
        const float v = rnd();
        const float d2 = float(dx * dx + dy * dy);
        const float wo = ao_fast::BlurWeightTwoExp(off, d2, sigma);
        const float wn = ao_fast::BlurWeight(off, d2, inv_sq);
        sum_old += v * wo;
        w_old_sum += wo;
        sum_new += v * wn;
        w_new_sum += wn;
      }
    }
    const float old_blur = sum_old / std::max(w_old_sum, 1e-8f);
    const float new_blur = sum_new / std::max(w_new_sum, 1e-8f);
    worst_blur = std::max(worst_blur, double(std::fabs(old_blur - new_blur)));
  }
  Say(Num("a whole blur (3x3 to 7x7, random planes): the largest difference of the result %.2g", worst_blur));
  Check(worst_blur < 2e-6, Num("a blur's result differs by %.2g at most (under 2e-6: 1/255 is 3.9e-3)", worst_blur));
}

// ---------------------------------------------------------------------------
// the level of the chain a sample reads: compare with 16 and 32 pixels instead of log2(length) - 3 cut to 0..2
// ---------------------------------------------------------------------------

void TestLevels() {
  int mismatches = 0;
  double worst_edge = 0.0;
  for (int i = 0; i <= 600000; ++i) {
    const float length = 0.5f + float(i) * 0.0005f;  // 0.5 .. 300.5 pixels
    const int old_level = int(std::clamp(std::log2(std::max(length, 1.0f)) - 3.0f, 0.0f, 2.0f));
    const int new_level = (length >= 16.0f ? 1 : 0) + (length >= 32.0f ? 1 : 0);
    if (old_level != new_level) {
      ++mismatches;
      worst_edge = std::max(worst_edge, double(std::min(std::fabs(length - 16.0f), std::fabs(length - 32.0f))) / double(length));
    }
  }
  Say(Num("level of the chain: %.0f of 600001 lengths differ, the farthest from an edge by %.2g of the length", double(mismatches), worst_edge));
  Check(mismatches == 0 || worst_edge < 1e-5, Num("the level's compare differs from log2 only within %.2g of 16 or 32 pixels", worst_edge));
  // the far ring's: 1 under 32, 2 from there
  int far_mismatches = 0;
  for (int i = 0; i <= 600000; ++i) {
    const float length = 0.5f + float(i) * 0.0005f;
    const int old_level = int(std::clamp(std::log2(std::max(length, 1.0f)) - 3.0f, 1.0f, 2.0f));
    const int new_level = length >= 32.0f ? 2 : 1;
    far_mismatches += old_level != new_level ? 1 : 0;
  }
  Check(far_mismatches < 5, Num("the far ring's level: %.0f of 600001 lengths differ (at the 32 pixel edge only)", double(far_mismatches)));
  // the tangents
  double worst_tangent = 0.0;
  for (int i = -2000; i <= 2000; ++i) {
    const float n = float(double(i) / 2000.0 * 1.57079632679490);
    worst_tangent = std::max(worst_tangent, double(std::fabs(std::cos(n + ao_halo::kHalfPi) + std::sin(n))));
    worst_tangent = std::max(worst_tangent, double(std::fabs(std::cos(n - ao_halo::kHalfPi) - std::sin(n))));
  }
  Say(Num("tangents: cos(n +- a quarter turn) against -+ sin(n): %.2g at most", worst_tangent));
  // (the float kHalfPi and the float sum n + kHalfPi are what the 1.8e-7 is: a float's own rounding at an angle of up to 3)
  Check(worst_tangent < 3e-7, "the surface's tangents -sin n and sin n are the cosines of n +- a quarter turn to 3e-7");
}

// ---------------------------------------------------------------------------
// the passes' twins over a scene: a road, a wall and a car
// ---------------------------------------------------------------------------

struct Hit {
  float t = ao_halo::kSky;
  int what = 0;  // 0 sky, 1 road, 2 wall, 3 car
};

Hit CastPixel(int width, int height, float tan_x, float tan_y, int x, int y) {
  const double u = (double(x) + 0.5) / double(width), v = (double(y) + 0.5) / double(height);
  const double dx = (2.0 * u - 1.0) * double(tan_x), dy = (1.0 - 2.0 * v) * double(tan_y);
  const double cam_y = 1.4;
  Hit best;
  const auto take = [&](double t, int what) {
    if (t > 0.1 && t < double(best.t)) {
      best.t = float(t);
      best.what = what;
    }
  };
  if (dy < 0.0) {
    take(cam_y / -dy, 1);  // the road, y = 0
  }
  if (dx < 0.0) {
    const double t = -4.0 / dx;  // the wall, x = -4, up to 6 m
    const double y_hit = cam_y + t * dy;
    if (y_hit >= 0.0 && y_hit <= 6.0) {
      take(t, 2);
    }
  }
  // the car: a box x -0.95..0.95, y 0.15..1.25, z 6..10.4
  const double lo[3] = {-0.95, 0.15 - cam_y, 6.0}, hi[3] = {0.95, 1.25 - cam_y, 10.4};
  const double d[3] = {dx, dy, 1.0};
  double t0 = 0.0, t1 = 1e9;
  bool hit = true;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(d[a]) < 1e-12) {
      if (0.0 < lo[a] || 0.0 > hi[a]) {
        hit = false;
      }
      continue;
    }
    double ta = lo[a] / d[a], tb = hi[a] / d[a];
    if (ta > tb) {
      std::swap(ta, tb);
    }
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
  }
  if (hit && t0 <= t1) {
    take(t0, 3);
  }
  return best;
}

ao_stable_twin::Chains MakeChains(int width, int height, const ao_halo::Params& p) {
  ao_stable_twin::ChainInput in;
  in.width = width;
  in.height = height;
  in.tan_y = 0.5773503f;
  const float tan_x = in.tan_y * float(width) / float(height);
  const float tan_y = in.tan_y;
  in.depth = [=](int x, int y) { return CastPixel(width, height, tan_x, tan_y, x, y).t; };
  in.colour = [=](int x, int y) {
    const Hit h = CastPixel(width, height, tan_x, tan_y, x, y);
    const float k = 0.7f + 0.3f * std::sin(float(x) * 0.043f + float(y) * 0.017f);
    switch (h.what) {
      case 1: return ssgi_light::Rgb{0.33f * k, 0.31f * k, 0.28f * k};
      case 2: return ssgi_light::Rgb{0.75f * k, 0.62f * k, 0.5f * k};
      case 3: return ssgi_light::Rgb{0.7f * k, 0.08f * k, 0.07f * k};
      default: return ssgi_light::Rgb{0.4f, 0.6f, 1.0f};
    }
  };
  in.vehicle = [=](int x, int y) { return CastPixel(width, height, tan_x, tan_y, x, y).what == 3; };
  return ao_stable_twin::BuildChains(in, p);
}

struct DiffStat {
  int pixels = 0;
  int differ = 0;            // the visibility differs by more than 1e-4
  int differ_bounce = 0;     // a bounce channel differs by more than 1e-4 of the larger of the two (and of 1e-4)
  double max_vis = 0.0;
  double mean_vis = 0.0;
  double mean_signed_vis = 0.0;
  double max_bounce_abs = 0.0;  // in the colour's own units (the chain's colours are at most 1)
  double mean_bounce_abs = 0.0;
  double mean_signed_bounce = 0.0;
};

// What one sector of one slice is worth, the most a rounding error that lands on a sector's edge can change a pixel by: a slice
// holds at most 1 / open_sum of the sum, a sector is 1/32 of its arc: the visibility's final power (3.6) makes that 3.6 / 32 =
// 0.1125 at the most; the bounce's is a sector's colour (at most 1) over 32.
inline constexpr double kOneSectorVisibility = 3.6 / 32.0;
inline constexpr double kOneSectorBounce = 1.0 / 32.0;

ao_halo::Params AoParams() {
  ao_halo::Params p;
  p.ratio_sum = true;
  p.car_reach = ao_halo::kDefaultCarReach;
  p.slices = 3;
  p.steps = 3;
  return p;
}

DiffStat CompareGi(const ao_stable_twin::Chains& chains, const ao_stable_twin::GiParams& a, const ao_stable_twin::GiParams& b, int stride,
                   int frames) {
  DiffStat s;
  for (int frame = 0; frame < frames; ++frame) {
    for (int y = 1 + frame % stride; y < chains.height - 1; y += stride) {
      for (int x = 1 + (frame * 2) % stride; x < chains.width - 1; x += stride) {
        const ao_stable_twin::GiOut oa = ao_stable_twin::GiMain(chains, a, x, y, 3 + frame * 5);
        const ao_stable_twin::GiOut ob = ao_stable_twin::GiMain(chains, b, x, y, 3 + frame * 5);
        ++s.pixels;
        const double dv = double(ob.visibility) - double(oa.visibility);
        s.max_vis = std::max(s.max_vis, std::fabs(dv));
        s.mean_vis += std::fabs(dv);
        s.mean_signed_vis += dv;
        s.differ += std::fabs(dv) > 1e-4 ? 1 : 0;
        const float ca[3] = {oa.bounce.r, oa.bounce.g, oa.bounce.b};
        const float cb[3] = {ob.bounce.r, ob.bounce.g, ob.bounce.b};
        bool bounce_differs = false;
        for (int k = 0; k < 3; ++k) {
          const double d = double(cb[k]) - double(ca[k]);
          const double scale = std::max(1e-4, double(std::max(ca[k], cb[k])));
          s.max_bounce_abs = std::max(s.max_bounce_abs, std::fabs(d));
          s.mean_bounce_abs += std::fabs(d) / 3.0;
          s.mean_signed_bounce += d / 3.0;
          bounce_differs = bounce_differs || std::fabs(d) / scale > 1e-4;
        }
        s.differ_bounce += bounce_differs ? 1 : 0;
      }
    }
  }
  const double n = double(std::max(s.pixels, 1));
  s.mean_vis /= n;
  s.mean_signed_vis /= n;
  s.mean_bounce_abs /= n;
  s.mean_signed_bounce /= n;
  return s;
}

void TestTwinGi() {
  const ao_halo::Params p = AoParams();
  const ao_stable_twin::Chains chains = MakeChains(1280, 720, p);
  ao_stable_twin::GiParams base;
  base.ao = p;
  base.far_reach = 9.0f;
  base.far_steps = 2;
  base.noise_lattice = 13;
  ao_stable_twin::GiParams lossless = base;
  lossless.lossless = true;
  ao_stable_twin::GiParams fast = lossless;
  fast.fast_acos = true;

  // the same work: the counts, divisions, tangents and levels as the shader's cheaper compilation has them
  const DiffStat same = CompareGi(chains, base, lossless, 5, 4);
  Say(Num("bounce main pass, cheaper arithmetic against before (%.0f pixels): visibility differs by over 1e-4 in %.0f (largest %.4f, "
          "mean %.2g, signed mean %.2g); the bounce in %.0f (largest %.4f, mean %.2g, signed mean %.2g)",
          double(same.pixels), double(same.differ), same.max_vis, same.mean_vis, same.mean_signed_vis, double(same.differ_bounce),
          same.max_bounce_abs, same.mean_bounce_abs, same.mean_signed_bounce));
  // A rounding error of one ulp in a sample's position moves its sector edges by a thousandth of a sector or less, and a pixel whose
  // edge sits that near the rounding point to the sector flips one sector of one slice: the visibility and the bounce change by
  // that sector's share and nothing else. The same difference exists between two drivers' compilations of one shader.
  Check(double(same.differ) <= 0.0005 * double(same.pixels), Num("the same work: %.0f of %.0f pixels differ in the visibility", double(same.differ), double(same.pixels)));
  Check(same.max_vis < kOneSectorVisibility, Num("the same work: the largest visibility difference is %.4f (under one sector of one slice, 0.1125)", same.max_vis));
  Check(same.max_bounce_abs < kOneSectorBounce, Num("the same work: the largest bounce difference is %.4f (under one sector's colour, 1/32)", same.max_bounce_abs));
  Check(double(same.differ_bounce) <= 0.03 * double(same.pixels), Num("the same work: %.0f of %.0f pixels differ in the bounce", double(same.differ_bounce), double(same.pixels)));
  Check(std::fabs(same.mean_signed_vis) < 1e-5 && std::fabs(same.mean_signed_bounce) < 1e-5,
        Num("the same work: no bias (visibility %.2g, bounce %.2g)", same.mean_signed_vis, same.mean_signed_bounce));

  // the approximation
  const DiffStat approx = CompareGi(chains, lossless, fast, 5, 4);
  Say(Num("bounce main pass, polynomial acos against the exact (%.0f pixels): visibility differs by over 1e-4 in %.0f (largest %.4f, "
          "mean %.2g, signed mean %.2g); the bounce in %.0f (largest %.4f, mean %.2g, signed mean %.2g)",
          double(approx.pixels), double(approx.differ), approx.max_vis, approx.mean_vis, approx.mean_signed_vis, double(approx.differ_bounce),
          approx.max_bounce_abs, approx.mean_bounce_abs, approx.mean_signed_bounce));
  // a sector edge moved by at most 0.0014 of a sector flips the bit of the sample whose edge is that near the rounding point
  Check(double(approx.differ) < 0.005 * double(approx.pixels), Num("the approximation: %.0f of %.0f pixels differ in the visibility", double(approx.differ), double(approx.pixels)));
  Check(approx.max_vis < kOneSectorVisibility, Num("the approximation: the largest visibility difference is %.4f (under one sector of one slice, 0.1125)", approx.max_vis));
  Check(approx.max_bounce_abs < kOneSectorBounce, Num("the approximation: the largest bounce difference is %.4f (under one sector's colour, 1/32)", approx.max_bounce_abs));
  Check(std::fabs(approx.mean_signed_vis) < 5e-5 && std::fabs(approx.mean_signed_bounce) < 5e-5,
        Num("the approximation has no bias (visibility %.2g, bounce %.2g)", approx.mean_signed_vis, approx.mean_signed_bounce));
  Check(approx.mean_vis < 1e-4 && approx.mean_bounce_abs < 1e-4,
        Num("the approximation: the mean difference is %.2g in the visibility, %.2g in the bounce", approx.mean_vis, approx.mean_bounce_abs));
}

void TestTwinPlain() {
  ao_halo::Params p = AoParams();
  const int w = 1280, h = 720;
  ao_halo::Scene scene;
  scene.width = w;
  scene.height = h;
  scene.tan_y = 0.5773503f;
  scene.tan_x = scene.tan_y * float(w) / float(h);
  const float tan_x = scene.tan_x, tan_y = scene.tan_y;
  scene.depth = [=](int x, int y) { return CastPixel(w, h, tan_x, tan_y, x, y).t; };
  scene.vehicle = [=](int x, int y) { return CastPixel(w, h, tan_x, tan_y, x, y).what == 3; };
  ao_halo::Params q = p;
  q.lossless = true;
  int pixels = 0, differ = 0;
  double max_diff = 0.0, mean_signed = 0.0;
  for (int frame = 0; frame < 4; ++frame) {
    for (int y = 1 + frame % 5; y < h - 1; y += 5) {
      for (int x = 1 + (frame * 2) % 5; x < w - 1; x += 5) {
        const float a = ao_halo::Visibility(scene, p, x, y, 5 + frame * 7);
        const float b = ao_halo::Visibility(scene, q, x, y, 5 + frame * 7);
        ++pixels;
        const double d = double(b) - double(a);
        max_diff = std::max(max_diff, std::fabs(d));
        mean_signed += d;
        differ += std::fabs(d) > 1e-4 ? 1 : 0;
      }
    }
  }
  mean_signed /= double(pixels);
  Say(Num("plain main pass, cheaper arithmetic against before (%.0f pixels): %.0f differ by over 1e-4 (largest %.4g, signed mean %.2g)",
          double(pixels), double(differ), max_diff, mean_signed));
  Check(double(differ) <= 0.0005 * double(pixels), Num("plain pass, the same work: %.0f of %.0f pixels differ", double(differ), double(pixels)));
  Check(max_diff < 0.06, Num("plain pass, the same work: the largest difference is %.4g", max_diff));
  Check(std::fabs(mean_signed) < 1e-5, Num("plain pass, the same work: no bias (mean %.2g)", mean_signed));
}

// ---------------------------------------------------------------------------
// the variants
// ---------------------------------------------------------------------------

void TestVariantChoice() {
  using ao_fast::Counts;
  Check(ao_fast::PlainCounts(false) == Counts::kUltra && ao_fast::PlainCounts(true) == Counts::kHigh, "the plain pass: ultra is 3x3, high is 2x2");
  Check(ao_fast::GiCounts(false, 2) == Counts::kUltra, "the bitmask pass: ultra quality with 2 ring steps is the 3x3 variant");
  Check(ao_fast::GiCounts(true, 1) == Counts::kHigh, "the bitmask pass: the fewer (high) quality with 1 ring step is the 2x2 variant");
  Check(ao_fast::GiCounts(false, 0) == Counts::kGeneric && ao_fast::GiCounts(true, 0) == Counts::kGeneric, "no ring: the generic compilation");
  Check(ao_fast::GiCounts(false, 1) == Counts::kGeneric && ao_fast::GiCounts(true, 2) == Counts::kGeneric && ao_fast::GiCounts(false, 7) == Counts::kGeneric,
        "a ring's steps that no variant has (the debug override): the generic compilation");
  Check(ao_fast::kUltraSlices == 3 && ao_fast::kUltraSteps == 3 && ao_fast::kHighSlices == 2 && ao_fast::kHighSteps == 2, "the qualities' counts are 3x3 and 2x2");
  Check(ao_fast::kUltraFarSteps == 2 && ao_fast::kHighFarSteps == 1, "the ring's steps are 2 and 1");
}

// "larecomp_gpu_host_shader(<stem> ps_6_0 SOURCE <source> DEFINES A=1 B=2)" -> the defines of the stem, or false
bool DefinesOf(const std::string& cmake, const std::string& stem, std::string* source, std::vector<std::string>* defines) {
  const std::string head = "larecomp_gpu_host_shader(" + stem + " ";
  const size_t at = cmake.find(head);
  if (at == std::string::npos) {
    return false;
  }
  const size_t end = cmake.find(')', at);
  std::string body = cmake.substr(at + head.size(), end - at - head.size());
  defines->clear();
  source->clear();
  std::vector<std::string> words;
  size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
      ++i;
    }
    size_t j = i;
    while (j < body.size() && !std::isspace(static_cast<unsigned char>(body[j]))) {
      ++j;
    }
    if (j > i) {
      words.push_back(body.substr(i, j - i));
    }
    i = j;
  }
  bool in_defines = false;
  for (size_t k = 0; k < words.size(); ++k) {
    if (words[k] == "SOURCE" && k + 1 < words.size()) {
      *source = words[k + 1];
      ++k;
    } else if (words[k] == "DEFINES") {
      in_defines = true;
    } else if (in_defines) {
      defines->push_back(words[k]);
    }
  }
  return true;
}

bool HasDefine(const std::vector<std::string>& defines, const std::string& d) {
  return std::find(defines.begin(), defines.end(), d) != defines.end();
}

void TestFiles(const std::string& gpu_dir) {
  std::string main_ps, common, atrous, temporal, cmake, ao_cpp, host_shaders;
  if (!Slurp(gpu_dir + "/hlsl/ao_main_ps.hlsl", &main_ps) || !Slurp(gpu_dir + "/hlsl/ao_common.hlsli", &common) ||
      !Slurp(gpu_dir + "/hlsl/ao_atrous_ps.hlsl", &atrous) || !Slurp(gpu_dir + "/hlsl/ao_temporal_ps.hlsl", &temporal) ||
      !Slurp(gpu_dir + "/CMakeLists.txt", &cmake) || !Slurp(gpu_dir + "/ao.cpp", &ao_cpp) || !Slurp(gpu_dir + "/host_shaders.h", &host_shaders)) {
    Check(false, "the files of src/gpu could not be read (" + gpu_dir + ")");
    return;
  }
  // the constants the shaders repeat
  Check(Has(common, "kAoNegHalfLog2e = -0.72134752044f;"), "ao_common.hlsli: the exponent's factor is the header's");
  Check(ao_fast::kNegHalfLog2e == -0.72134752044f, "the header's exponent factor is -0.5 / ln 2");
  Check(std::fabs(double(ao_fast::kNegHalfLog2e) + 0.5 / std::log(2.0)) < 1e-7, "-0.72134752044 is -0.5 / ln 2 (to a float's rounding)");
  Check(std::fabs(-0.72134752044 + 0.5 / std::log(2.0)) < 1e-10, "-0.72134752044 is -0.5 / ln 2 to 11 digits");
  Check(Has(main_ps, "1.5707288f + a * (-0.2121144f + a * (0.0742610f + a * -0.0187293f))"), "ao_main_ps.hlsl: the acos polynomial's coefficients are the header's");
  Check(ao_fast::kAcosC0 == 1.5707288f && ao_fast::kAcosC1 == -0.2121144f && ao_fast::kAcosC2 == 0.0742610f && ao_fast::kAcosC3 == -0.0187293f,
        "the header's coefficients are Abramowitz and Stegun 4.4.45's");
  Check(Has(main_ps, "const float low0 = -sin_n;") && Has(main_ps, "const float low1 = sin_n;"), "ao_main_ps.hlsl: the bitmask pass's tangents are -sin n and sin n");
  Check(Has(main_ps, "(length_px >= 16.0f ? 1u : 0u) + (length_px >= 32.0f ? 1u : 0u)") && Has(main_ps, "length_px >= 32.0f ? 2u : 1u"),
        "ao_main_ps.hlsl: the chain's level is the compare with 16 and 32 pixels");
  Check(Has(main_ps, "AoViewPosInv(floor(p) + 0.5f, clip_scale, ChainDepth(p, level))"), "ao_main_ps.hlsl: a sample position takes 2 / size");
  Check(Has(common, "exp2(kAoNegHalfLog2e * (off_plane * off_plane + float(dx * dx + dy * dy) * inv_spatial_sq))"),
        "ao_common.hlsli: a blur tap's two weights are one exponential");
  Check(!Has(common, "const float w_space = exp(-0.5f * float(dx * dx + dy * dy) / (spatial_sigma * spatial_sigma));"),
        "ao_common.hlsli: the blur has no second exponential for the spatial weight");
  Check(Has(atrous, "exp2(kAoNegHalfLog2e * off_plane * off_plane)") && Has(atrous, "AoViewPosInv"), "ao_atrous_ps.hlsl: one exp2 and 2 / size for the tap");
  Check(Has(temporal, "exp2(kAoNegHalfLog2e * off_plane * off_plane)"), "ao_temporal_ps.hlsl: the clip box's neighbours' weights are one exp2");

  // the variants: the same source with the counts defined; the qualities' counts are the header's
  struct Variant {
    const char* stem;
    uint32_t slices, steps, far_steps;  // far_steps 0: not defined
    bool gi, fast;
  };
  const Variant variants[] = {
      {"ao_main_3x3_ps", ao_fast::kUltraSlices, ao_fast::kUltraSteps, 0, false, false},
      {"ao_main_2x2_ps", ao_fast::kHighSlices, ao_fast::kHighSteps, 0, false, false},
      {"ao_main_gi_3x3_far2_ps", ao_fast::kUltraSlices, ao_fast::kUltraSteps, ao_fast::kUltraFarSteps, true, true},
      {"ao_main_gi_2x2_far1_ps", ao_fast::kHighSlices, ao_fast::kHighSteps, ao_fast::kHighFarSteps, true, true},
      {"ao_main_gi_3x3_far2_exact_ps", ao_fast::kUltraSlices, ao_fast::kUltraSteps, ao_fast::kUltraFarSteps, true, false},
      {"ao_main_gi_2x2_far1_exact_ps", ao_fast::kHighSlices, ao_fast::kHighSteps, ao_fast::kHighFarSteps, true, false},
  };
  for (const Variant& v : variants) {
    std::string source;
    std::vector<std::string> defines;
    if (!DefinesOf(cmake, v.stem, &source, &defines)) {
      Check(false, std::string("CMakeLists.txt: no larecomp_gpu_host_shader for ") + v.stem);
      continue;
    }
    Check(source == "ao_main_ps", std::string(v.stem) + ": compiled from ao_main_ps");
    Check(HasDefine(defines, "AO_SLICES=" + std::to_string(v.slices)) && HasDefine(defines, "AO_STEPS=" + std::to_string(v.steps)),
          std::string(v.stem) + ": the slices and steps defined are the quality's");
    Check(v.far_steps == 0 ? std::none_of(defines.begin(), defines.end(), [](const std::string& d) { return d.rfind("AO_FAR_STEPS", 0) == 0; })
                           : HasDefine(defines, "AO_FAR_STEPS=" + std::to_string(v.far_steps)),
          std::string(v.stem) + ": the ring's steps defined are the quality's");
    Check(HasDefine(defines, "AO_GI=1") == v.gi, std::string(v.stem) + ": the bitmask pass is compiled with AO_GI");
    Check(HasDefine(defines, "AO_FAST_ACOS=1") == v.fast, std::string(v.stem) + (v.fast ? ": the polynomial acos is compiled in" : ": the exact acos stays"));
    Check(Has(host_shaders, std::string("\"") + v.stem + ".hlsl.dxil.h\"") && Has(host_shaders, std::string("\"") + v.stem + ".hlsl.spirv.h\""),
          std::string(v.stem) + ": host_shaders.h includes both blobs");
    Check(Has(host_shaders, std::string("MCLA_GPU_HOST_SHADER_BLOB(") + v.stem + ")"), std::string(v.stem) + ": host_shaders.h hands the blob out");
  }
  // ao.cpp sends the counts the variants have compiled in
  Check(Has(ao_cpp, "ao_fewer ? ao_fast::kHighSlices : ao_fast::kUltraSlices") && Has(ao_cpp, "ao_fewer ? ao_fast::kHighSteps : ao_fast::kUltraSteps"),
        "ao.cpp: FillPush sends the slices and steps of ao_fast.h");
  Check(Has(ao_cpp, "ao_fast::PlainCounts(") && Has(ao_cpp, "ao_fast::GiCounts("), "ao.cpp: the pipelines are picked with ao_fast.h's rule");
  Check(Has(ao_cpp, "mcla_gpu_ao_fast_math"), "ao.cpp: the cvar mcla_gpu_ao_fast_math exists");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      gpu_dir = argv[i];
    }
  }
  TestAcos();
  TestSectors();
  TestBlurWeight();
  TestLevels();
  TestVariantChoice();
  TestTwinPlain();
  TestTwinGi();
  if (!gpu_dir.empty()) {
    TestFiles(gpu_dir);
  }
  std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "ao_fast_test PASS" : "ao_fast_test FAIL", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
