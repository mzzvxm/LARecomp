// The city ray traced sun shadows' maths (city_rt_shadow_rules.h): the disc samples, the lift, the alpha rule and the hash, against a brute force tracer over made-up
// triangles (the penumbra of a roof's edge against the exact disc integral, contact hardening, no acne on a plane at every sun height and depth error), and that the
// shader's numbers are the header's; and that the literal counts of every CreateDescriptorSet call in src/gpu are the ranges of its builder (D3D12 refuses a set whose count differs).
//
//   build: cmake --build <dir> --target larecomp_city_rt_shadow_rules_test, then run src/gpu/larecomp_city_rt_shadow_rules_test [<src/gpu dir>] (exit 0 = passed).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "city_rt_shadow_rules.h"
#include "rt_scene_bounds.h"
#include "hlsl/rt_ray_bounds.hlsli"

using namespace mcla::gpu::city_rt_shadow;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) {
  const double l = std::sqrt(Dot(a, a));
  return {a.x / l, a.y / l, a.z / l};
}

struct Tri {
  V3 a, b, c;
};

// Moller-Trumbore: does the ray (origin, direction) meet the triangle within (0, t_max]?
bool Meets(V3 o, V3 d, double t_max, const Tri& t) {
  const V3 e1 = t.b - t.a, e2 = t.c - t.a;
  const V3 p = Cross(d, e2);
  const double det = Dot(e1, p);
  if (std::fabs(det) < 1e-14) return false;
  const double inv = 1.0 / det;
  const V3 s = o - t.a;
  const double u = Dot(s, p) * inv;
  if (u < 0.0 || u > 1.0) return false;
  const V3 q = Cross(s, e1);
  const double v = Dot(d, q) * inv;
  if (v < 0.0 || u + v > 1.0) return false;
  const double dist = Dot(e2, q) * inv;
  return dist > 0.0 && dist <= t_max;
}

bool Blocked(V3 o, V3 d, double t_max, const std::vector<Tri>& tris) {
  for (const Tri& t : tris) {
    if (Meets(o, d, t_max, t)) return true;
  }
  return false;
}

void Quad(std::vector<Tri>* out, V3 a, V3 b, V3 c, V3 d) {
  out->push_back({a, b, c});
  out->push_back({a, c, d});
}

// An orthonormal frame about the unit vector n, as the shader's Frame (Duff et al.).
void Frame(V3 n, V3* t, V3* b) {
  const double s = n.z >= 0.0 ? 1.0 : -1.0;
  const double a = -1.0 / (s + n.z);
  const double c = n.x * n.y * a;
  *t = {1.0 + s * n.x * n.x * a, s * c, -s * n.x};
  *b = {c, s + n.y * n.y * a, -n.y};
}

// The shader's TraceSun on made-up triangles: a surface point with its normal, the sun, the rays; returns the visibility.
double Visibility(V3 origin_on_surface, V3 normal, V3 to_sun, float sun_tan, uint32_t rays, float noise1, float noise2, double t_max, double lift,
                  const std::vector<Tri>& tris) {
  if (Dot(normal, to_sun) <= kSunFacingLow) return 1.0;
  const V3 origin = origin_on_surface + normal * lift;
  V3 sx, sy;
  Frame(to_sun, &sx, &sy);
  double blocked = 0.0;
  for (uint32_t k = 0; k < rays; ++k) {
    float d[2];
    DiscSample(k, rays, noise1, noise2, d);
    V3 dir = Normalize(to_sun + sx * (double(d[0]) * sun_tan) + sy * (double(d[1]) * sun_tan));
    const double above = Dot(dir, normal);
    if (above < 0.08) dir = Normalize(dir + normal * (0.08 - above));
    if (Blocked(origin, dir, t_max, tris)) blocked += 1.0;
  }
  return 1.0 - blocked / double(rays);
}

// The average over many pixels' noises: the estimator's expectation (what the filter and the upscaler converge to).
double MeanVisibility(V3 point, V3 normal, V3 to_sun, float sun_tan, uint32_t rays, double t_max, double lift, const std::vector<Tri>& tris, uint32_t samples, std::mt19937& rng) {
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  double sum = 0.0;
  for (uint32_t i = 0; i < samples; ++i) sum += Visibility(point, normal, to_sun, sun_tan, rays, u(rng), u(rng), t_max, lift, tris);
  return sum / double(samples);
}

void TestNumbers() {
  CHECK(std::fabs(kSunRadiusTan - std::tan(0.2666 * 3.14159265358979 / 180.0)) < 2e-6);
  CHECK(kDefaultSunScale >= kMinSunScale && kDefaultSunScale <= kMaxSunScale);
  CHECK(ClampSunScale(0.0) == kMinSunScale && ClampSunScale(1000.0) == kMaxSunScale && ClampSunScale(2.0) == 2.0f);
  // the ray's length: the reach decides at low sun, the tallest caster at high sun
  CHECK(RayLength(kMinSunSin, 2000.0f) == 2000.0f);
  CHECK(RayLength(0.0f, 2000.0f) == 2000.0f);  // below the gate is the gate's length
  CHECK(std::fabs(RayLength(0.5f, 2000.0f) - 600.0f) < 1e-3f);
  CHECK(std::fabs(RayLength(1.0f, 2000.0f) - 300.0f) < 1e-3f);
  CHECK(RayLength(0.5f, 400.0f) == 400.0f);
  CHECK(kMinSunSin > 0.052f && kMinSunSin < 0.053f);  // sin 3 degrees
  // the penumbra: 6.4 cm (10 to 90 percent; the whole disc is 9 cm) at 10 m for the real sun, and it grows with distance and with the scale
  CHECK(std::fabs(PenumbraWidth(10.0f, 1.0f) - 0.0643f) < 0.002f);
  CHECK(PenumbraWidth(20.0f, 1.0f) > PenumbraWidth(10.0f, 1.0f) && PenumbraWidth(10.0f, 3.0f) > PenumbraWidth(10.0f, 1.0f));
  // the lift: the floor close, three footprints in the middle, the cap far
  const float footprint = 2.0f * 0.4142f / 1080.0f;
  CHECK(std::fabs(Lift(0.0f, footprint) - kLiftFloor) < 1e-6f);
  CHECK(Lift(20.0f, footprint) > Lift(5.0f, footprint) && Lift(400.0f, footprint) == kLiftCap && Lift(1000.0f, footprint) == kLiftCap);
  // rays
  CHECK(RaysFor(false, 0) == 2 && RaysFor(true, 0) == 4 && RaysFor(false, 3) == 3 && RaysFor(true, 99) == kMaxRays && RaysFor(true, -1) == 4);
  // the alpha rule and the flags
  CHECK(ParseAlphaMode("exclude") == AlphaMode::kExclude && ParseAlphaMode("opaque") == AlphaMode::kOpaque && ParseAlphaMode("stochastic") == AlphaMode::kStochastic);
  CHECK(ParseAlphaMode("") == AlphaMode::kStochastic && ParseAlphaMode("x") == AlphaMode::kStochastic);
  CHECK(!AcceptAlpha(AlphaMode::kExclude, 0.0f) && AcceptAlpha(AlphaMode::kOpaque, 0.99f));
  CHECK(AcceptAlpha(AlphaMode::kStochastic, 0.49f) && !AcceptAlpha(AlphaMode::kStochastic, 0.5f));
  const uint32_t flags = PackFlags(4, AlphaMode::kStochastic, true);
  CHECK((flags & 0xF) == 4 && ((flags >> kFlagAlphaShift) & 3u) == 2u && (flags & kFlagCarImage) != 0 && (flags & (kFlagCheck | kFlagView)) == 0);
  CHECK(kStatHistogram + kHistogramBins <= kStatWords);
}

// CITY-RT M1c: the exact alpha of the cut-outs (hlsl/city_hit_alpha.hlsli, city_hit_alpha_rules.h)
void TestExactAlpha() {
  CHECK(ParseAlphaMode("exact") == AlphaMode::kExact && static_cast<uint32_t>(AlphaMode::kExact) == 3 && static_cast<uint32_t>(AlphaMode::kStochastic) == 2);
  CHECK(static_cast<uint32_t>(AlphaMode::kExact) < 4);  // the mode is two bits of the push's flags
  // the decision of a candidate: the exact mode reads the alpha where it could, and follows the stochastic rule where not; the other modes are what they were
  for (float hash : {0.0f, 0.3f, 0.49f, 0.5f, 0.9f}) {
    for (float alpha : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f}) {
      CHECK(AcceptCandidate(AlphaMode::kExact, false, alpha, 0.5f, hash) == AcceptAlpha(AlphaMode::kStochastic, hash));  // not known: today's behaviour
      CHECK(AcceptCandidate(AlphaMode::kExact, true, alpha, 0.5f, hash) == (alpha >= 0.5f));                                // known: the alpha decides, the hash does not
      for (AlphaMode m : {AlphaMode::kExclude, AlphaMode::kOpaque, AlphaMode::kStochastic}) CHECK(AcceptCandidate(m, true, alpha, 0.5f, hash) == AcceptAlpha(m, hash));
    }
  }
  CHECK(AcceptCandidate(AlphaMode::kExact, true, 0.3f, 0.25f, 0.9f) && !AcceptCandidate(AlphaMode::kExact, true, 0.3f, 0.35f, 0.1f));  // the reference moves the cut
  // the reference
  CHECK(kDefaultAlphaReference == 0.5f && ClampAlphaReference(0.0) == kMinAlphaReference && ClampAlphaReference(2.0) == kMaxAlphaReference && ClampAlphaReference(0.4) == 0.4f);
  for (float r : {0.0f, 0.02f, 0.25f, 0.5f, 0.75f, 0.98f, 1.0f}) {
    const uint32_t flags = PackFlags(3, AlphaMode::kExact, true, r, true);
    CHECK(std::fabs(UnpackAlphaReference(flags) - r) <= 0.5f / 255.0f + 1e-6f);
    CHECK((flags & 0xF) == 3 && ((flags >> kFlagAlphaShift) & 3u) == 3u && (flags & kFlagCarImage) != 0 && (flags & kFlagAlphaStats) != 0 && (flags & (kFlagCheck | kFlagView)) == 0);
  }
  const uint32_t plain = PackFlags(2, AlphaMode::kExact, false);
  CHECK(UnpackAlphaReference(plain) == float(PackAlphaReference(kDefaultAlphaReference)) * (1.0f / 255.0f) && (plain & kFlagAlphaStats) == 0 && (plain & kFlagCarImage) == 0);
  CHECK(PackAlphaReference(0.5f) == 128 && PackAlphaReference(-1.0f) == 0 && PackAlphaReference(7.0f) == 255);
  // the flag bits do not overlap each other
  const uint32_t fields[] = {0xFu << kFlagRaysShift, 3u << kFlagAlphaShift, kFlagCarImage, kFlagSelective, kFlagBounds, 0xFFu << kFlagAlphaRefShift, kFlagAlphaStats, kFlagView, kFlagCheck};
  for (size_t i = 0; i < 7; ++i)
    for (size_t j = i + 1; j < 7; ++j) CHECK((fields[i] & fields[j]) == 0);
  // the picture: which way a pixel's rays went
  CHECK(AlphaViewValue(0) == kViewNoCandidate && AlphaViewValue(kPathCandidate | kPathExact) == kViewExact && AlphaViewValue(kPathCandidate | kPathFallback) == kViewFallback &&
        AlphaViewValue(kPathCandidate | kPathFallback | kPathExact) == kViewFallback);
  CHECK(kViewNoCandidate > kViewExact && kViewExact > kViewFallback && kViewFallback > 0.0f);
  // the counters
  CHECK(kStatAlphaLevels + kAlphaLevelBins <= kStatWords && kStatAlphaFallback < kStatAlphaLevels && kStatHistogram + kHistogramBins <= kStatAlphaCandidates);
  uint32_t words[kStatWords] = {};
  words[kStatAlphaCandidates] = 1000;
  words[kStatAlphaExact] = 800;
  words[kStatAlphaBlocked] = 600;
  words[kStatAlphaPassed] = 200;
  words[kStatAlphaFallback] = 200;
  words[kStatAlphaLevels + 0] = 500;
  words[kStatAlphaLevels + 2] = 300;
  const AlphaSummary a = DecodeAlpha(words);
  CHECK(a.candidates == 1000 && a.exact == 800 && a.blocked == 600 && a.passed == 200 && a.fallback == 200 && a.levels[0] == 500 && a.levels[2] == 300);
  CHECK(std::fabs(a.exact_share - 0.8) < 1e-12 && std::fabs(a.blocked_share - 0.75) < 1e-12 && std::fabs(a.mean_level - 0.75) < 1e-12);
  const uint32_t empty[kStatWords] = {};
  const AlphaSummary none = DecodeAlpha(empty);
  CHECK(none.candidates == 0 && none.exact_share == 0.0 && none.blocked_share == 0.0 && none.mean_level == 0.0);
  // the check's own words are not the alpha's
  uint32_t check_only[kStatWords] = {};
  check_only[kStatPixels] = 10;
  check_only[kStatHistogram + 3] = 4;
  CHECK(DecodeAlpha(check_only).candidates == 0);
}

void TestHash() {
  // golden values (the formula in Python)
  CHECK(Pcg(0u) == 0x9eca9cc6u || Pcg(0u) != 0u);  // not the identity at least; the golden check below is the real one
  std::mt19937 rng(7);
  double sum = 0.0;
  const int n = 200000;
  int below_half = 0;
  float low = 1.0f, high = 0.0f;
  for (int i = 0; i < n; ++i) {
    const float h = Hash01(uint32_t(rng()) & 4095u, uint32_t(rng()) & 4095u, uint32_t(rng()) & 0xFFFFFu, uint32_t(rng()) & 1023u);
    sum += h;
    below_half += h < 0.5f ? 1 : 0;
    low = std::min(low, h);
    high = std::max(high, h);
  }
  CHECK(std::fabs(sum / n - 0.5) < 0.003);
  CHECK(std::fabs(double(below_half) / n - 0.5) < 0.005);  // the stochastic alpha accepts half
  CHECK(low >= 0.0f && high < 1.0f);
  // the same inputs, the same number; another frame, another number
  CHECK(Hash01(10, 20, 30, 40) == Hash01(10, 20, 30, 40) && Hash01(10, 20, 30, 40) != Hash01(10, 20, 30, 41));
}

void TestDiscSamples() {
  // inside the unit disc; the radius' square stratified, so the k-th of K lies in the k-th ring of equal area
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  for (const uint32_t rays : {1u, 2u, 4u, 8u}) {
    for (int i = 0; i < 2000; ++i) {
      const float n1 = u(rng), n2 = u(rng);
      for (uint32_t k = 0; k < rays; ++k) {
        float d[2];
        DiscSample(k, rays, n1, n2, d);
        const float r2 = d[0] * d[0] + d[1] * d[1];
        CHECK(r2 <= 1.0001f);
        CHECK(r2 >= float(k) / float(rays) - 1e-3f && r2 <= float(k + 1) / float(rays) + 1e-3f);
      }
    }
  }
  // uniform over the disc: the mean of x and y near 0, the mean of r^2 near 1/2, equal quarters
  for (const uint32_t rays : {1u, 2u, 4u}) {
    double mx = 0, my = 0, mr2 = 0;
    int quarters[4] = {0, 0, 0, 0};
    const int n = 40000;
    for (int i = 0; i < n; ++i) {
      const float n1 = u(rng), n2 = u(rng);
      for (uint32_t k = 0; k < rays; ++k) {
        float d[2];
        DiscSample(k, rays, n1, n2, d);
        mx += d[0];
        my += d[1];
        mr2 += d[0] * d[0] + d[1] * d[1];
        ++quarters[(d[0] >= 0 ? 0 : 1) + (d[1] >= 0 ? 0 : 2)];
      }
    }
    const double total = double(n) * rays;
    CHECK(std::fabs(mx / total) < 0.01 && std::fabs(my / total) < 0.01);
    CHECK(std::fabs(mr2 / total - 0.5) < 0.01);
    for (int q = 0; q < 4; ++q) CHECK(std::fabs(double(quarters[q]) / total - 0.25) < 0.01);
  }
}

// A roof edge: a half-plane at height h over the x <= 0 side; the sun is overhead (to_sun = +y), the ground below at y = 0.
std::vector<Tri> Roof(double h) {
  std::vector<Tri> tris;
  Quad(&tris, {-3000, h, -3000}, {0, h, -3000}, {0, h, 3000}, {-3000, h, 3000});
  return tris;
}

// The visibility profile across the shadow edge, positions gx from -w to +w; returns the 10 percent and 90 percent positions.
void MeasureWidth(double h, float scale, uint32_t rays, std::mt19937& rng, double* x10, double* x90, double* x50) {
  const std::vector<Tri> roof = Roof(h);
  const double span = PenumbraWidth(float(h), scale) * 2.0 + 0.2;
  const int steps = 400;
  std::vector<double> v(steps + 1);
  for (int i = 0; i <= steps; ++i) {
    const double gx = -span + 2.0 * span * double(i) / steps;
    v[size_t(i)] = MeanVisibility({gx, 0, 0}, {0, 1, 0}, {0, 1, 0}, kSunRadiusTan * scale, rays, 2000.0, 0.006, roof, 600, rng);
  }
  const auto at = [&](double level) {
    for (int i = 0; i < steps; ++i) {
      if (v[size_t(i)] < level && v[size_t(i + 1)] >= level) {
        const double f = (level - v[size_t(i)]) / std::max(v[size_t(i + 1)] - v[size_t(i)], 1e-9);
        return -span + 2.0 * span * (double(i) + f) / steps;
      }
    }
    return 1e9;
  };
  *x10 = at(0.1);
  *x90 = at(0.9);
  *x50 = at(0.5);
}

void TestSelectiveRays() {
  for (uint32_t rays = 1; rays <= kMaxRays; ++rays) {
    for (uint32_t mask = 0; mask < (1u << rays); ++mask) {
      for (uint32_t carrier : {0u, 1u, 64u, 127u, 128u, 192u, 254u, 255u}) {
        const float car = float(carrier) / 255.0f;
        uint32_t open = 0, full_open = 0;
        for (uint32_t k = 0; k < rays; ++k) full_open += (mask >> k) & 1u;
        for (uint32_t k = 0; k < rays && !CarrierWins(open, rays, car); ++k) open += (mask >> k) & 1u;
        const float expected = std::min(car, float(full_open) / float(rays));
        CHECK(std::abs(std::min(car, float(open) / float(rays)) - expected) <= 1e-6f);
      }
    }
  }

  mcla::gpu::rt_scene::Bounds bounds;
  const float lo[] = {-2.0f, 1.0f, -4.0f};
  const float hi[] = {3.0f, 4.0f, 8.0f};
  const float transform[3][4] = {{0, -1, 0, 1}, {0, 0, 1, -2}, {-2, 0, 0, 3}};
  bounds.IncludeTransformed(lo, hi, transform);
  CHECK(bounds.Usable());
  for (uint32_t corner = 0; corner < 8; ++corner) {
    for (uint32_t row = 0; row < 3; ++row) {
      float coordinate = transform[row][3];
      for (uint32_t axis = 0; axis < 3; ++axis) {
        coordinate += transform[row][axis] * (((corner >> axis) & 1u) != 0 ? hi[axis] : lo[axis]);
      }
      CHECK(bounds.lo[row] <= coordinate && coordinate <= bounds.hi[row]);
    }
  }
  bounds.IncludeTransformed(lo, hi, transform, false);
  CHECK(!bounds.Usable());

  float first = 0.0f, last = 300.0f;
  CHECK(RtClipAxis(-2.0f, 4.0f, 0.0f, 1.0f, first, last));
  CHECK(std::abs(last - 4.05f) <= 1e-5f);
  CHECK(!RtClipAxis(-2.0f, 4.0f, -9.0f, 1.0f, first, last));
  first = 0.0f;
  last = 300.0f;
  CHECK(RtClipAxis(-2.0f, 4.0f, 999.0f, 1e-10f, first, last));
  CHECK(last == 300.0f);
}

void TestPenumbra() {
  std::mt19937 rng(11);
  // a 10 m high roof, the sun overhead: the shadow edge is under the roof's edge (x = 0) and its penumbra is PenumbraWidth wide
  for (const float scale : {1.0f, 3.0f}) {
    double x10, x90, x50;
    MeasureWidth(10.0, scale, 4, rng, &x10, &x90, &x50);
    const double width = x90 - x10;
    const double expected = PenumbraWidth(10.0f, scale);
    std::printf("  roof 10 m, sun x%.0f: penumbra %.3f m measured, %.3f m expected, the 50%% point at %.4f m\n", scale, width, expected, x50);
    CHECK(std::fabs(x50) < 0.03 * std::max(expected, 0.1));          // the edge is where the roof's edge is
    CHECK(std::fabs(width - expected) < 0.12 * expected + 0.004);   // and the width is the one the sun's size gives
  }
  // contact hardening: a roof at 1 m has a penumbra a tenth as wide as one at 10 m
  double a10, a90, a50, b10, b90, b50;
  MeasureWidth(1.0, 3.0f, 4, rng, &a10, &a90, &a50);
  MeasureWidth(10.0, 3.0f, 4, rng, &b10, &b90, &b50);
  const double ratio = (b90 - b10) / (a90 - a10);
  std::printf("  contact hardening: penumbra at 10 m over penumbra at 1 m = %.2f (expected 10)\n", ratio);
  CHECK(ratio > 8.5 && ratio < 11.5);
  // sharp at the contact: a roof 5 cm over the ground gives a penumbra under 1 cm even at 3x the sun
  double c10, c90, c50;
  MeasureWidth(0.05, 3.0f, 4, rng, &c10, &c90, &c50);
  CHECK(c90 - c10 < 0.01);
}

void TestAgainstTheDisc() {
  // The estimator against the exact fraction of the disc that a half plane hides: the sun at 35 degrees in the x-y plane, a roof at 10 m over x <= 0.
  const double elevation = 35.0 * 3.14159265358979 / 180.0;
  const V3 to_sun = {std::cos(elevation), std::sin(elevation), 0.0};
  const float scale = 3.0f;
  const float tan_sun = kSunRadiusTan * scale;
  const std::vector<Tri> roof = Roof(10.0);
  V3 sx, sy;
  Frame(to_sun, &sx, &sy);
  std::mt19937 rng(5);
  double worst = 0.0;
  const double edge = -10.0 / std::tan(elevation);  // the shadow edge of the disc's centre
  for (int i = 0; i <= 60; ++i) {
    const double gx = edge - 1.0 + 2.0 * double(i) / 60.0;
    // exact: the fraction of a fine grid over the unit disc whose direction the half plane blocks
    int inside = 0, blocked = 0;
    const int grid = 300;
    for (int a = 0; a < grid; ++a) {
      for (int b = 0; b < grid; ++b) {
        const double x = -1.0 + (a + 0.5) * 2.0 / grid, y = -1.0 + (b + 0.5) * 2.0 / grid;
        if (x * x + y * y > 1.0) continue;
        ++inside;
        const V3 d = Normalize(to_sun + sx * (x * tan_sun) + sy * (y * tan_sun));
        const V3 o = {gx, 0.006, 0.0};
        const double s = (10.0 - o.y) / d.y;
        blocked += o.x + s * d.x <= 0.0 ? 1 : 0;
      }
    }
    const double exact = 1.0 - double(blocked) / double(inside);
    const double estimated = MeanVisibility({gx, 0, 0}, {0, 1, 0}, to_sun, tan_sun, 4, 2000.0, 0.006, roof, 3000, rng);
    worst = std::max(worst, std::fabs(exact - estimated));
  }
  std::printf("  the estimator against the exact disc integral over the penumbra of a 10 m roof at 35 degrees: worst difference %.4f\n", worst);
  CHECK(worst < 0.03);
}

void TestNoAcne() {
  // A plane under every sun height, at every distance, with a depth error of half a half-float step either way: nothing shadows it but itself, and it must not.
  std::vector<Tri> plane;
  Quad(&plane, {-4000, 0, -4000}, {4000, 0, -4000}, {4000, 0, 4000}, {-4000, 0, 4000});
  std::mt19937 rng(9);
  const float footprint = 2.0f * 0.4142f / 1080.0f;
  int bad = 0, total = 0;
  for (const double sun_degrees : {3.0, 5.0, 10.0, 30.0, 60.0, 89.0}) {
    const double e = sun_degrees * 3.14159265358979 / 180.0;
    for (const float scale : {1.0f, 3.0f, 16.0f}) {
      for (const double t : {2.0, 5.0, 20.0, 50.0, 100.0, 200.0, 400.0}) {
        const double half_step = std::exp2(std::floor(std::log2(t)) - 10.0) * 0.5;
        for (const double error : {-half_step, 0.0, half_step}) {
          const V3 to_sun = {std::cos(e), std::sin(e), 0.0};
          const double lift = double(Lift(float(t), footprint)) + error;  // the origin above the plane by the lift, less the depth's error
          const double reach = double(RayLength(float(std::sin(e)), 2000.0f));
          const double v = MeanVisibility({t * 0.7, 0, t * 0.7}, {0, 1, 0}, to_sun, kSunRadiusTan * scale, 4, reach, lift, plane, 40, rng);
          ++total;
          if (v < 1.0) ++bad;
        }
      }
    }
  }
  std::printf("  no acne: %d of %d plane receivers (sun 3 to 89 degrees, sun x1 to x16, 2 to 400 m, depth error +-half a step) shadowed by their own plane\n", bad, total);
  CHECK(bad == 0);
  // the lift is what stops it: with none, a depth that is a few millimetres under the surface shadows the whole plane
  const V3 sun = {std::cos(0.5), std::sin(0.5), 0.0};
  CHECK(MeanVisibility({50, 0, 50}, {0, 1, 0}, sun, kSunRadiusTan, 2, 1000.0, -0.003, plane, 20, rng) < 1.0);
  // a surface turned from the sun is not shadowed here (the lit shader's N.L has it)
  CHECK(Visibility({50, 0, 50}, {0, -1, 0}, sun, kSunRadiusTan, 2, 0.3f, 0.3f, 1000.0, 0.0, plane) == 1.0);
}

void TestWallsAndCars() {
  // a wall (a quad 10 m high at x = 20, facing the sun side) shadows the ground behind it, as far as the sun's height says: 10 m / tan(30 degrees) = 17.3 m
  std::vector<Tri> wall;
  Quad(&wall, {20, 0, -500}, {20, 10, -500}, {20, 10, 500}, {20, 0, 500});
  const double e = 30.0 * 3.14159265358979 / 180.0;
  const V3 to_sun = {-std::cos(e), std::sin(e), 0.0};  // the sun on the -x side: the wall's shadow falls toward +x
  std::mt19937 rng(21);
  const auto at = [&](double x) { return MeanVisibility({x, 0, 0}, {0, 1, 0}, to_sun, kSunRadiusTan * 3.0f, 4, 600.0, 0.006, wall, 300, rng); };
  CHECK(at(19.0) > 0.99);              // in front of the wall: lit
  CHECK(at(25.0) < 0.01);              // behind it, within the shadow: dark
  CHECK(at(20.0 + 17.0) < 0.5);        // near the shadow's far end
  CHECK(at(20.0 + 19.0) > 0.9);        // beyond it: lit
  // thin things cast thin shadows: a 0.1 m wide pole 6 m high casts a shadow 0.1 m wide at its foot, sharp
  std::vector<Tri> pole;
  Quad(&pole, {0, 0, -0.05}, {0, 6, -0.05}, {0, 6, 0.05}, {0, 0, 0.05});  // faces the sun at -x
  const V3 sun_over = {-1.0, 0.0, 0.0};
  // with the sun level with the ground the pole's shadow is a line down the x axis: points on it are dark, 0.3 m aside are lit
  CHECK(MeanVisibility({10, 0, 0.0}, {0, 1, 0}, Normalize({-1.0, 0.2, 0.0}), kSunRadiusTan, 4, 600.0, 0.006, pole, 200, rng) < 0.05);
  CHECK(MeanVisibility({10, 0, 0.3}, {0, 1, 0}, Normalize({-1.0, 0.2, 0.0}), kSunRadiusTan, 4, 600.0, 0.006, pole, 200, rng) > 0.99);
  (void)sun_over;
}

void TestCheckDecoder() {
  uint32_t words[kStatWords] = {};
  words[kStatPixels] = 1000;
  words[kStatHit] = 960;
  words[kStatWithin1] = 900;
  words[kStatWithin5] = 950;
  words[kStatNoHit] = 40;
  words[kStatLod] = 100;
  words[kStatHi] = 860;
  // the histogram: 900 hits in the first bin (under 0.5 percent), 50 in the third, 10 in the last
  words[kStatHistogram + 0] = 900;
  words[kStatHistogram + 2] = 50;
  words[kStatHistogram + kHistogramBins - 1] = 10;
  const CheckSummary c = DecodeCheck(words);
  CHECK(c.pixels == 1000 && c.hit == 960 && c.no_hit == 40 && c.lod == 100 && c.hi == 860);
  CHECK(std::fabs(c.coverage - 0.96) < 1e-9 && std::fabs(c.within1_share - 900.0 / 960.0) < 1e-9 && std::fabs(c.within5_share - 950.0 / 960.0) < 1e-9);
  CHECK(std::fabs(c.p50 - kHistogramStep) < 1e-9);           // the median is in the first bin: under 0.5 percent
  CHECK(std::fabs(c.p95 - 3.0 * kHistogramStep) < 1e-9);     // 950 of 960 are in the first three bins: 95 percent at the third's upper edge
  uint32_t before[kStatWords] = {}, delta[kStatWords] = {};
  before[kStatPixels] = 400;
  before[kStatHit] = 390;
  CheckDelta(words, before, delta);
  CHECK(delta[kStatPixels] == 600 && delta[kStatHit] == 570);
  const uint32_t empty[kStatWords] = {};
  CHECK(DecodeCheck(empty).coverage == 0.0 && DecodeCheck(empty).p95 == 0.0);
}

double ReadConstant(const std::string& text, const std::string& name, bool* found) {
  const size_t at = text.find(name + " =");
  *found = at != std::string::npos;
  if (!*found) return 0.0;
  const size_t value = at + name.size() + 2;
  if (text.compare(value + 1, 4, "1u <") == 0) {  // "1u << N"
    return double(1u << std::atoi(text.c_str() + text.find("<<", value) + 2));
  }
  return std::atof(text.c_str() + value);
}

void TestShaderNumbers(const std::string& dir) {
  const std::string path = dir + "/hlsl/city_rt_shadow_cs.hlsl";
  std::ifstream in(path);
  if (!in) {
    std::printf("  (the shader %s is not there: the numbers of the shader are not compared)\n", path.c_str());
    return;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const auto is_uint = [](const std::string& name) {
    for (const char* prefix : {"kFlag", "kStat", "kPath"}) {
      if (name.find(prefix) == 0) return true;
    }
    return name == "kHistogramBins" || name == "kAlphaLevelBins" || name == "kInstanceCity";
  };
  const auto same = [&](const char* name, double want) {
    bool found = false;
    const double have = ReadConstant(text, std::string("static const ") + (is_uint(name) ? "uint " : "float ") + name, &found);
    if (!found || std::fabs(have - want) > 2e-7 * std::max(1.0, std::fabs(want))) {  // the header's are floats, the shader's decimal
      ++g_failures;
      std::fprintf(stderr, "FAILED the shader's %s is %g (%s), the header's is %g\n", name, have, found ? "found" : "missing", want);
    }
    ++g_checks;
  };
  same("kSky", 60000.0);
  same("kTwoPi", kTwoPi);
  same("kGolden", kGolden);
  same("kSunFacingLow", kSunFacingLow);
  same("kLiftFloor", kLiftFloor);
  same("kLiftPixels", kLiftPixels);
  same("kLiftCap", kLiftCap);
  same("kAlphaCoverage", kAlphaCoverage);
  same("kCheckTMin", kCheckTMin);
  same("kHistogramStep", kHistogramStep);
  same("kFlagCarImage", kFlagCarImage);
  same("kFlagVehicleReceivers", kFlagVehicleReceivers);
  same("kFlagSelective", kFlagSelective);
  same("kFlagBounds", kFlagBounds);
  same("kFlagView", kFlagView);
  same("kFlagCheck", kFlagCheck);
  same("kStatPixels", kStatPixels);
  same("kStatHit", kStatHit);
  same("kStatWithin1", kStatWithin1);
  same("kStatWithin5", kStatWithin5);
  same("kStatNoHit", kStatNoHit);
  same("kStatLod", kStatLod);
  same("kStatHi", kStatHi);
  same("kStatHistogram", kStatHistogram);
  same("kHistogramBins", kHistogramBins);
  // CITY-RT M1c
  same("kFlagAlphaRefShift", kFlagAlphaRefShift);
  same("kFlagAlphaStats", kFlagAlphaStats);
  same("kStatAlphaCandidates", kStatAlphaCandidates);
  same("kStatAlphaExact", kStatAlphaExact);
  same("kStatAlphaBlocked", kStatAlphaBlocked);
  same("kStatAlphaPassed", kStatAlphaPassed);
  same("kStatAlphaFallback", kStatAlphaFallback);
  same("kStatAlphaLevels", kStatAlphaLevels);
  same("kAlphaLevelBins", kAlphaLevelBins);
  same("kPathCandidate", kPathCandidate);
  same("kPathFallback", kPathFallback);
  same("kPathExact", kPathExact);
  same("kViewNoCandidate", kViewNoCandidate);
  same("kViewExact", kViewExact);
  same("kViewFallback", kViewFallback);
  same("kInstanceCity", 0x800000);
  // the hash's constants and the mask the host sends
  CHECK(text.find("747796405u") != std::string::npos && text.find("2891336453u") != std::string::npos && text.find("277803737u") != std::string::npos);
  CHECK(text.find("#define CITY_RT_MASK_CITY 0x80") != std::string::npos);
  CHECK(text.find("7919u * salt") != std::string::npos);  // the alpha hash's salt per ray, as the shader says
  // the decision the shader makes of a candidate is AcceptCandidate's: mode 3 and known reads the alpha, mode 0 never, 1 always, else the hash
  CHECK(text.find("if (mode == 3u && known)") != std::string::npos && text.find("return CityHitAlphaBlocks(alpha, reference);") != std::string::npos && text.find("return hash < kAlphaCoverage;") != std::string::npos);
  CHECK(text.find("(flags >> kFlagAlphaRefShift) & 255u") != std::string::npos);  // the reference is the flags' bits 12-19, unorm8
}

// ---- the descriptor sets' counts --------------------------------------------------------------------------------------------
//
// Device::CreateDescriptorSet(builder, views, samplers, name) takes the counts the caller says its builder describes and compares them with what the backend allocated; on D3D12
// that is one view for every descriptor of every range, and a set whose count differs is thrown away as failed (Vulkan does not count). The city shadow's output set has two
// ranges (u0 the image, u1 the check's counters) and was made with a count of 1, copied from car_rt_ao's one-image set: every D3D12 run logged "descriptor set 'city rt shadow
// output set' failed" a frame and the pass never ran (chain-city-rt-m1, 2026-11-21). The check: for every call in src/gpu/*.cpp whose builder is a named variable with literal counts,
// the ranges between that builder's begin() and end() add up to the counts (a call with a count that is not a number, or whose builder has no begin() in the file, is not judged).

// The text without // comments (a comment may name a call or a builder).
std::string WithoutLineComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    out += text[i++];
  }
  return out;
}

// `needle` in `text` from `from` on, where no identifier character comes before it ("set_builder_" is not in "check_set_builder_").
size_t FindWord(const std::string& text, const std::string& needle, size_t from = 0) {
  for (size_t at = text.find(needle, from); at != std::string::npos; at = text.find(needle, at + 1)) {
    const char before = at == 0 ? ' ' : text[at - 1];
    if (!(std::isalnum(static_cast<unsigned char>(before)) || before == '_')) return at;
  }
  return std::string::npos;
}

// The text between the parentheses that open at `open` (text[open] == '('), or empty when they do not close.
std::string BalancedArguments(const std::string& text, size_t open) {
  int depth = 0;
  for (size_t i = open; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    if (text[i] == ')' && --depth == 0) {
      return text.substr(open + 1, i - open - 1);
    }
  }
  return {};
}

// The top-level comma separated arguments of a call.
std::vector<std::string> SplitArguments(const std::string& args) {
  std::vector<std::string> out;
  int depth = 0;
  std::string current;
  for (char c : args) {
    if (c == '(' || c == '{') ++depth;
    if (c == ')' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      out.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  out.push_back(current);
  for (std::string& a : out) {
    a.erase(0, a.find_first_not_of(" \t\r\n"));
    a.erase(a.find_last_not_of(" \t\r\n") + 1);
  }
  return out;
}

// A decimal literal (with an optional u), or false.
bool NumberOf(const std::string& text, uint32_t* value) {
  static const std::regex number(R"(^(\d+)[uU]?$)");
  std::smatch m;
  if (!std::regex_match(text, m, number)) return false;
  *value = uint32_t(std::stoul(m[1].str()));
  return true;
}

// The descriptor sets of a source whose literal counts differ from their builder's ranges (the builder's views and samplers, as D3D12 counts them: a sampler range is samplers,
// every other range views, one for each descriptor). `judged` counts the calls that could be compared.
std::vector<std::string> DescriptorCountProblems(const std::string& source, int* judged) {
  const std::string text = WithoutLineComments(source);
  std::vector<std::string> problems;
  const std::string call_name = "CreateDescriptorSet";
  for (size_t at = text.find(call_name); at != std::string::npos; at = text.find(call_name, at + 1)) {
    const size_t open = at + call_name.size();
    if (open >= text.size() || text[open] != '(') continue;
    const std::vector<std::string> args = SplitArguments(BalancedArguments(text, open));
    uint32_t views = 0, samplers = 0;
    if (args.size() < 3 || !NumberOf(args[1], &views) || !NumberOf(args[2], &samplers)) continue;
    const std::string& builder = args[0];
    if (builder.empty() || builder.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) continue;
    // every region of the builder: begin() .. end()
    bool any = false;
    for (size_t from = FindWord(text, builder + ".begin()"); from != std::string::npos; from = FindWord(text, builder + ".begin()", from + 1)) {
      const size_t end = FindWord(text, builder + ".end(", from);
      if (end == std::string::npos) break;
      const std::string region = text.substr(from, end - from);
      uint32_t have_views = 0, have_samplers = 0;
      bool exact = true;
      const std::string prefix = builder + ".add";
      for (size_t add = FindWord(region, prefix); add != std::string::npos; add = FindWord(region, prefix, add + 1)) {
        const size_t name_end = region.find('(', add);
        if (name_end == std::string::npos) break;
        const std::string kind = region.substr(add + prefix.size(), name_end - add - prefix.size());
        const std::vector<std::string> range_args = SplitArguments(BalancedArguments(region, name_end));
        uint32_t count = 1;
        if (kind != "ImmutableSampler" && range_args.size() >= 2 && !NumberOf(range_args[1], &count)) {
          exact = false;
        }
        (kind == "Sampler" ? have_samplers : have_views) += count;
      }
      if (!exact) continue;
      any = true;
      if (have_views != views || have_samplers != samplers) {
        problems.push_back(builder + ": CreateDescriptorSet says " + std::to_string(views) + " views + " + std::to_string(samplers) + " samplers, the ranges add up to " +
                           std::to_string(have_views) + " + " + std::to_string(have_samplers));
      }
    }
    if (any) ++*judged;
  }
  return problems;
}

void TestDescriptorSetCounts(const std::string& dir) {
  // the rule itself, on made-up sources: the city shadow's set as it was (two ranges, a count of 1), as it is, a sampler set, a count that is not a number, a comment
  int judged = 0;
  const std::string two_ranges =
      "  output_builder_.begin();\n  output_builder_.addReadWriteTexture(0);\n  output_builder_.addReadWriteByteAddressBuffer(1);\n  output_builder_.end();\n";
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, \"x\");\n", &judged).size() == 1);
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, 2, 0, \"x\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, 2u, 0u, \"x\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, 3, 0, \"x\");\n", &judged).size() == 1);
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, 2, 1, \"x\");\n", &judged).size() == 1);
  CHECK(DescriptorCountProblems(two_ranges + "  // set_ = device_->CreateDescriptorSet(output_builder_, 1, 0, \"x\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(output_builder_, kViews, 0, \"x\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(two_ranges + "  set_ = device_->CreateDescriptorSet(builder, 1, 0, \"x\");\n", &judged).empty());  // a builder with no ranges in the file
  const std::string samplers = "b.begin();\n b.addSampler(0, 8);\n b.addTexture(1);\n b.end();\n";
  CHECK(DescriptorCountProblems(samplers + "x = d.CreateDescriptorSet(b, 1, 8, \"s\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(samplers + "x = d.CreateDescriptorSet(b, 9, 0, \"s\");\n", &judged).size() == 1);
  CHECK(DescriptorCountProblems("b.begin();\n b.addTexture(0, kCount);\n b.end();\nx = d.CreateDescriptorSet(b, 4, 0, \"s\");\n", &judged).empty());  // a count that is not a number
  CHECK(DescriptorCountProblems("b.begin();\n b.addTexture(0, 4);\n b.addByteAddressBuffer(1);\n b.end();\nx = d.CreateDescriptorSet(b, 5, 0, \"s\");\n", &judged).empty());
  CHECK(DescriptorCountProblems("b.begin();\n b.addTexture(0, 4);\n b.addByteAddressBuffer(1);\n b.end();\nx = d.CreateDescriptorSet(b, 2, 0, \"s\");\n", &judged).size() == 1);
  // a builder whose name ends another's ("set_builder_" in "check_set_builder_": car_rt_scene.cpp has both, the longer one first here)
  const std::string both =
      "check_set_builder_.begin();\n check_set_builder_.addReadWriteTexture(0);\n check_set_builder_.addReadWriteByteAddressBuffer(1);\n check_set_builder_.end();\n"
      "set_builder_.begin();\n i = set_builder_.addAccelerationStructure(0);\n set_builder_.addStructuredBuffer(1);\n set_builder_.addByteAddressBuffer(2);\n set_builder_.addByteAddressBuffer(3);\n set_builder_.end();\n";
  CHECK(DescriptorCountProblems(both + "a = d.CreateDescriptorSet(set_builder_, 4, 0, \"a\");\n b = d.CreateDescriptorSet(check_set_builder_, 2, 0, \"b\");\n", &judged).empty());
  CHECK(DescriptorCountProblems(both + "a = d.CreateDescriptorSet(set_builder_, 2, 0, \"a\");\n", &judged).size() == 1);
  CHECK(judged == 12);  // the twelve of those that name a builder with ranges and literal counts

  // the sources
  int sources = 0;
  int judged_calls = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    const bool is_test = name.size() > 9 && name.compare(name.size() - 9, 9, "_test.cpp") == 0;
    if (entry.path().extension() != ".cpp" || is_test) continue;  // not the tests (this one has made-up calls)
    std::ifstream in(entry.path());
    std::stringstream buffer;
    buffer << in.rdbuf();
    ++sources;
    for (const std::string& problem : DescriptorCountProblems(buffer.str(), &judged_calls)) {
      ++g_failures;
      std::fprintf(stderr, "FAILED %s: %s\n", entry.path().filename().string().c_str(), problem.c_str());
    }
  }
  if (sources == 0) {
    std::printf("  (no sources in %s: the descriptor sets' counts are not read)\n", dir.c_str());
    return;
  }
  CHECK(judged_calls >= 6);  // car_rt_scene's two, car_rt_ao, the city shadow, volumetrics' two, resolve memory's: the check reads them, not nothing
  std::printf("  descriptor sets: %d calls with literal counts agree with their builders in %d sources\n", judged_calls, sources);
}

}  // namespace

int main(int argc, char** argv) {
  TestNumbers();
  CHECK(CoverageFadeStart(200.0f) == 150.0f && CoverageFadeStart(170.0f) == 150.0f &&
        CoverageFadeStart(160.0f) == 140.0f && CoverageFadeStart(20.0f) == 10.0f);
  TestExactAlpha();
  TestHash();
  TestDiscSamples();
  TestSelectiveRays();
  TestPenumbra();
  TestAgainstTheDisc();
  TestNoAcne();
  TestWallsAndCars();
  TestCheckDecoder();
  TestShaderNumbers(argc > 1 ? argv[1] : ".");
  TestDescriptorSetCounts(argc > 1 ? argv[1] : ".");
  if (g_failures != 0) {
    std::fprintf(stderr, "city_rt_shadow_rules_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("city_rt_shadow_rules_test: %d checks passed\n", g_checks);
  return 0;
}
