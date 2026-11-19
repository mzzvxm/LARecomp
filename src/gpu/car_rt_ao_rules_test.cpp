// Check of the ray traced occlusion and contact shadows' rules (car_rt_ao_rules.h): the setting and what it asks for, the rays and the reach, the
// cosine weighted pattern and the distribution of the rays' lengths (the expected occlusion of a blocker at a distance, and of a plane over the
// surface), the sun ray's cone, the car's box in view space (the camera's z whichever way it looks) and the pixel rectangle that holds it, the slab
// test, the image's codes and the flags. Given the src/gpu folder it also checks that the shaders repeat the header's numbers and flags. A program of
// its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_ao_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_ao_rules_test [<src/gpu dir>] [--print]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "car_rt_ao_rules.h"

namespace ra = mcla::gpu::car_rt_ao;
namespace rr = mcla::gpu::car_rt_rules;
namespace as = mcla::gpu::ao_stable;
namespace tp = mcla::gpu::temporal;

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
  char buf[320];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// (p, 1) * m.
void Apply(const tp::Mat4& m, const double p[3], double out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
  }
}
tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}
// A rotation about the vertical axis (row vectors: v' = v R).
tp::Mat4 RotationY(double angle) {
  tp::Mat4 m = tp::Identity();
  m.m[0][0] = std::cos(angle);
  m.m[0][2] = -std::sin(angle);
  m.m[2][0] = std::sin(angle);
  m.m[2][2] = std::cos(angle);
  return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------------------------------------------------
void TestSetting() {
  Check(ra::SettingOf("auto") == ra::Setting::kAuto, "auto is auto");
  Check(ra::SettingOf("off") == ra::Setting::kOff, "off is off");
  Check(ra::SettingOf("ao") == ra::Setting::kOcclusion, "ao is the occlusion only");
  Check(ra::SettingOf("contact") == ra::Setting::kContact, "contact is the sun ray only");
  Check(ra::SettingOf("") == ra::Setting::kAuto && ra::SettingOf("ultra") == ra::Setting::kAuto && ra::SettingOf("OFF") == ra::Setting::kAuto,
        "a word that is none of them is auto (the settings' rule)");

  // Wanted(setting, device, occlusion, bounce, contact)
  Check(!ra::Wanted(ra::Setting::kAuto, false, true, false, true).any(), "a device that cannot trace the car: nothing");
  Check(!ra::Wanted(ra::Setting::kOff, true, true, false, true).any(), "off: nothing");
  const ra::Terms both = ra::Wanted(ra::Setting::kAuto, true, true, false, true);
  Check(both.occlusion && both.contact, "auto with both effects on: both terms");
  const ra::Terms occlusion_only = ra::Wanted(ra::Setting::kAuto, true, true, false, false);
  Check(occlusion_only.occlusion && !occlusion_only.contact, "auto, the contact shadows off or gated: the occlusion term only");
  const ra::Terms contact_only = ra::Wanted(ra::Setting::kAuto, true, false, false, true);
  Check(!contact_only.occlusion && contact_only.contact, "auto, the occlusion off: the sun ray only");
  const ra::Terms bounce = ra::Wanted(ra::Setting::kAuto, true, true, true, true);
  Check(!bounce.occlusion && bounce.contact, "the bounce light's visibility pass has no traced term, the contact march does");
  const ra::Terms ao_setting = ra::Wanted(ra::Setting::kOcclusion, true, true, false, true);
  Check(ao_setting.occlusion && !ao_setting.contact, "ao: the occlusion term only even with the contact shadows on");
  const ra::Terms contact_setting = ra::Wanted(ra::Setting::kContact, true, true, false, true);
  Check(!contact_setting.occlusion && contact_setting.contact, "contact: the sun ray only even with the occlusion on");
  Check(!ra::Wanted(ra::Setting::kAuto, true, false, false, false).any(), "both effects off: nothing");
}

// ---------------------------------------------------------------------------------------------------------------------
// the rays
// ---------------------------------------------------------------------------------------------------------------------
void TestConstants() {
  Check(ra::OcclusionRays(false) == 2 && ra::OcclusionRays(true) == 4 && ra::kMaxRays == 4, "high 2 rays, ultra 4");
  Check(ra::ClampedReach(0.6f) == 0.6f && ra::ClampedReach(0.0f) == ra::kMinReach && ra::ClampedReach(50.0f) == ra::kMaxReach &&
            ra::ClampedReach(std::nanf("")) == ra::kDefaultReach,
        "the reach is kept in its range");
  Check(ra::kDefaultReach >= 0.5f && ra::kDefaultReach <= 1.0f, "the default reach is in the 0.5 to 1 m the design asks for");
  Check(Near(ra::SnappedLift(0.0f), 0.004, 1e-6) && Near(ra::SnappedLift(5.0f), 0.014, 1e-6), "the snapped lift is the reflection's (4 mm + 2 mm a metre)");
  Check(ra::DepthLift(5.0f, 0.0007f) > ra::kDepthLiftFloor && ra::DepthLift(10.0f, 0.0007f) > ra::DepthLift(5.0f, 0.0007f),
        "the depth lift grows with the distance");
  Check(ra::kRayParts == (rr::kMaskBody | rr::kMaskTyres), "the rays hit the body and the tyres");
  Check(ra::kRayParts == 0x05u, "the rays' mask is 5 (hlsl CAR_RT_MASK_BODY | CAR_RT_MASK_TYRES)");
  Check(ra::kGolden == as::kStepGolden, "the golden angle is the occlusion's step golden");
}

void TestPattern() {
  // Cosine weighted: the mean of cos(theta) is 2/3 and of cos^2 is 1/2; the pattern is unit vectors above the plane; no sideways drift.
  for (uint32_t rays : {1u, 2u, 4u}) {
    double sum_z = 0.0, sum_z2 = 0.0, sum_x = 0.0, sum_y = 0.0;
    int count = 0;
    bool unit = true, above = true;
    for (int i = 0; i < 64; ++i) {
      for (int j = 0; j < 64; ++j) {
        const float nx = (float(i) + 0.5f) / 64.0f;
        const float ny = (float(j) + 0.5f) / 64.0f;
        for (uint32_t k = 0; k < rays; ++k) {
          const ra::Vec3 v = ra::CosineLocal(k, rays, nx, ny);
          const double length = std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
          unit = unit && Near(length, 1.0, 1e-4);
          above = above && v.z >= 0.0f;
          sum_z += v.z;
          sum_z2 += double(v.z) * v.z;
          sum_x += v.x;
          sum_y += v.y;
          ++count;
        }
      }
    }
    Check(unit, Num("rays %.0f: every direction is a unit vector", rays));
    Check(above, Num("rays %.0f: every direction is above the surface", rays));
    Check(Near(sum_z / count, 2.0 / 3.0, 0.004), Num("rays %.0f: the mean cosine is 2/3 (%.4f)", rays, sum_z / count));
    Check(Near(sum_z2 / count, 0.5, 0.004), Num("rays %.0f: the mean squared cosine is 1/2 (%.4f)", rays, sum_z2 / count));
    Check(Near(sum_x / count, 0.0, 0.004) && Near(sum_y / count, 0.0, 0.004), Num("rays %.0f: no sideways drift (%.4f)", rays, sum_x / count));
  }
  // The rotation of the strata's pairing stays in range whatever the noise.
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u01(0.0f, 1.0f);
  bool in_range = true;
  for (int i = 0; i < 20000; ++i) {
    for (uint32_t rays : {1u, 2u, 4u}) {
      in_range = in_range && ra::LengthRotation(u01(rng), u01(rng), rays) < rays;
    }
  }
  Check(in_range, "the length strata's rotation is below the ray count");
}

// The share of rays at least `d` long, over every stratum, rotation and jitter.
double LongShare(uint32_t rays, float reach, float d) {
  int longer = 0, total = 0;
  for (uint32_t rotation = 0; rotation < rays; ++rotation) {
    for (uint32_t k = 0; k < rays; ++k) {
      for (int j = 0; j < 256; ++j) {
        const float u = ra::LengthShare(k, rays, rotation, (float(j) + 0.5f) / 256.0f);
        longer += ra::RayLength(reach, u) >= d ? 1 : 0;
        ++total;
      }
    }
  }
  return double(longer) / double(total);
}

void TestLengths() {
  const float reach = 0.6f;
  for (uint32_t rays : {1u, 2u, 4u}) {
    for (float d : {0.0f, 0.03f, 0.1f, 0.2f, 0.35f, 0.5f, 0.59f, 0.6f, 0.9f}) {
      const double expected = ra::BlockerWeight(d, reach);
      const double got = LongShare(rays, reach, d);
      Check(Near(got, expected, 0.01), Num("rays %.0f: a blocker at %.2f m is reached by %.4f of the rays, the weight says", rays, d, got) +
                                           Num(" %.4f", expected));
    }
  }
  Check(ra::BlockerWeight(0.0f, reach) == 1.0f && ra::BlockerWeight(reach, reach) == 0.0f && ra::BlockerWeight(2.0f * reach, reach) == 0.0f,
        "a blocker at the surface counts fully, at the reach and beyond it not at all");
  Check(ra::BlockerWeight(0.1f, reach) > ra::BlockerWeight(0.2f, reach) && ra::BlockerWeight(0.2f, reach) > ra::BlockerWeight(0.4f, reach),
        "the weight falls with the distance");
  Check(ra::RayLength(reach, 0.0f) == reach && ra::RayLength(reach, 1.0f) == 0.0f, "the shortest and the longest ray");
}

// The expected hit share of a plane parallel to the surface at height h (distance h / cos(theta) along a ray), for the estimator as the shader makes it
// (the direction's stratum, the length's stratum turned by the rotation, the minimum cosine above the surface), against the integral of the weights
// over the cosine weighted directions (the expectation the design promises).
void TestPlaneOverhead() {
  const float reach = 0.6f;
  struct Row {
    float h;
    double analytic;
    double got[3];
  };
  if (g_print) {
    std::printf("an overhead plane at height h over the surface (reach %.1f m): the share of the hemisphere's light it takes\n  h      integral   1 ray  2 rays 4 rays\n", reach);
  }
  for (float h : {0.02f, 0.05f, 0.1f, 0.2f, 0.3f, 0.45f}) {
    // the integral over the radius squared U of the weight at h / sqrt(1 - U), the direction below the minimum cosine lifted as the shader lifts it
    double analytic = 0.0;
    const int steps = 400000;
    for (int i = 0; i < steps; ++i) {
      const double u = (double(i) + 0.5) / steps;
      const double cosine = std::max(std::sqrt(1.0 - u), double(ra::kMinAbove));
      analytic += ra::BlockerWeight(float(h / cosine), reach);
    }
    analytic /= steps;
    double got[3] = {};
    int slot = 0;
    for (uint32_t rays : {1u, 2u, 4u}) {
      double hits = 0.0;
      long total = 0;
      for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 96; ++j) {
          const float nx = (float(i) + 0.5f) / 4.0f;
          const float ny = (float(j) + 0.5f) / 96.0f;
          for (uint32_t rotation = 0; rotation < rays; ++rotation) {
            for (int jj = 0; jj < 32; ++jj) {
              const float jitter = (float(jj) + 0.5f) / 32.0f;
              for (uint32_t k = 0; k < rays; ++k) {
                ra::Vec3 v = ra::CosineLocal(k, rays, nx, ny);
                if (v.z < ra::kMinAbove) {  // the shader's lift: d = normalize(d + (min - above) n)
                  v.z += ra::kMinAbove - v.z;
                  const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                  v = {v.x / l, v.y / l, v.z / l};
                }
                const float length = ra::RayLength(reach, ra::LengthShare(k, rays, rotation, jitter));
                hits += length >= h / v.z ? 1.0 : 0.0;
                ++total;
              }
            }
          }
        }
      }
      got[slot] = hits / double(total);
      Check(Near(got[slot], analytic, 0.012), Num("an overhead plane at %.2f m, ", h) + Num("%.0f rays: %.4f against the integral %.4f", rays, got[slot], analytic));
      ++slot;
    }
    if (g_print) {
      std::printf("  %.2f   %.4f     %.4f %.4f %.4f\n", h, analytic, got[0], got[1], got[2]);
    }
  }
}

void TestSunCone() {
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u01(0.0f, 1.0f);
  std::uniform_real_distribution<float> s11(-1.0f, 1.0f);
  const float cos_limit = float(1.0 / std::sqrt(1.0 + double(ra::kSunDiscTan) * ra::kSunDiscTan));
  bool unit = true, inside = true;
  double mean_dot = 0.0;
  int count = 0;
  for (int i = 0; i < 400; ++i) {
    ra::Vec3 s{s11(rng), s11(rng), s11(rng)};
    const float l = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    if (l < 0.1f) continue;
    s = {s.x / l, s.y / l, s.z / l};
    for (int k = 0; k < 40; ++k) {
      const ra::Vec3 d = ra::SunDirection(s, u01(rng), u01(rng), ra::kSunDiscTan);
      const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      unit = unit && Near(length, 1.0, 1e-4);
      const float dot = d.x * s.x + d.y * s.y + d.z * s.z;
      inside = inside && dot >= cos_limit - 1e-5f;
      mean_dot += dot;
      ++count;
    }
  }
  Check(unit, "the sun ray's directions are unit vectors (every direction of the sun)");
  Check(inside, "the sun ray stays inside the sun disc's cone");
  Check(mean_dot / count > cos_limit && mean_dot / count < 1.0, "the sun ray's spread is a cone, not a line");
  // radius 0: the sun's direction itself
  const ra::Vec3 up{0.0f, 0.6f, 0.8f};
  const ra::Vec3 d0 = ra::SunDirection(up, 0.3f, 0.7f, 0.0f);
  Check(Near(d0.x, 0.0, 1e-5) && Near(d0.y, 0.6, 1e-5) && Near(d0.z, 0.8, 1e-5), "a disc of radius 0 is the sun's direction");
}

// ---------------------------------------------------------------------------------------------------------------------
// the car's box and its pixels
// ---------------------------------------------------------------------------------------------------------------------
void TestViewBox() {
  // The game's own view looks down -z: the car at world z = -5 is at view z = -5, which the occlusion's view space makes +5.
  {
    const tp::Mat4 view = tp::Identity();
    const double position[3] = {0.0, 0.0, -5.0};
    const double lo[3] = {-1.0, 0.0, -7.0};
    const double hi[3] = {1.0, 1.5, -3.0};
    ra::Box box;
    Check(ra::ViewBoxFor(view, position, true, lo, hi, 0.0f, &box) && box.valid, "the game's own camera (z backward): a box");
    Check(Near(box.min[0], -1.0, 1e-5) && Near(box.max[0], 1.0, 1e-5) && Near(box.min[1], 0.0, 1e-5) && Near(box.max[1], 1.5, 1e-5), "x and y are the world's");
    Check(Near(box.min[2], 3.0, 1e-5) && Near(box.max[2], 7.0, 1e-5), "z is flipped: the car is in front of the camera (3 to 7)");
    ra::Box grown;
    ra::ViewBoxFor(view, position, true, lo, hi, 0.15f, &grown);
    Check(Near(grown.min[2], 2.85, 1e-5) && Near(grown.max[0], 1.15, 1e-5) && Near(grown.max[2], 7.15, 1e-5), "the margin grows every side");
  }
  // The occlusion's own view space (z forward already): used as it is.
  {
    const tp::Mat4 view = Translation(0.0, -1.0, 0.0);
    const double position[3] = {0.0, 0.0, 5.0};
    const double lo[3] = {-1.0, 0.0, 3.0};
    const double hi[3] = {1.0, 1.5, 7.0};
    ra::Box box;
    Check(ra::ViewBoxFor(view, position, true, lo, hi, 0.0f, &box), "a camera with z forward: a box");
    Check(Near(box.min[1], -1.0, 1e-5) && Near(box.max[1], 0.5, 1e-5) && Near(box.min[2], 3.0, 1e-5) && Near(box.max[2], 7.0, 1e-5),
          "the translation is applied and z stays");
  }
  // No box of the car: a cube of kFallbackRadius round its position.
  {
    const tp::Mat4 view = tp::Identity();
    const double position[3] = {2.0, 0.5, -10.0};
    ra::Box box;
    Check(ra::ViewBoxFor(view, position, false, nullptr, nullptr, 0.0f, &box), "no world box: the fallback");
    Check(Near(box.min[0], 2.0 - ra::kFallbackRadius, 1e-4) && Near(box.max[0], 2.0 + ra::kFallbackRadius, 1e-4) &&
              Near(box.min[2], 10.0 - ra::kFallbackRadius, 1e-4) && Near(box.max[2], 10.0 + ra::kFallbackRadius, 1e-4),
          "the fallback is a cube round the car, z flipped");
  }
  // A car at the camera's plane: no answer (the sign cannot be told).
  {
    const tp::Mat4 view = tp::Identity();
    const double position[3] = {0.0, 0.0, -0.2};
    ra::Box box;
    Check(!ra::ViewBoxFor(view, position, false, nullptr, nullptr, 0.0f, &box), "a car within half a metre of the camera's plane: no box");
  }
  // A turned and moved camera: every corner of the world box is in the view box, whichever way the camera's z points.
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> angle(-3.1, 3.1);
  std::uniform_real_distribution<double> metres(-20.0, 20.0);
  bool contained = true;
  int boxes = 0;
  for (int i = 0; i < 300; ++i) {
    const tp::Mat4 view = tp::Mul(Translation(metres(rng), metres(rng), metres(rng)), RotationY(angle(rng)));
    const double position[3] = {metres(rng), 0.0, metres(rng)};
    const double lo[3] = {position[0] - 2.3, position[1], position[2] - 1.0};
    const double hi[3] = {position[0] + 2.3, position[1] + 1.5, position[2] + 1.0};
    ra::Box box;
    if (!ra::ViewBoxFor(view, position, true, lo, hi, 0.0f, &box)) {
      continue;
    }
    ++boxes;
    const bool flip = rr::ViewZSign(view, position) < 0;
    for (int corner = 0; corner < 8; ++corner) {
      const double p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
      double v[3];
      Apply(view, p, v);
      if (flip) v[2] = -v[2];
      for (int c = 0; c < 3; ++c) {
        contained = contained && v[c] >= box.min[c] - 1e-4 && v[c] <= box.max[c] + 1e-4;
      }
    }
  }
  Check(boxes > 100 && contained, "every corner of the car's world box is inside the view box (turned and moved cameras)");
}

bool InRect(const ra::Rect& r, double px, double py) {
  return std::floor(px) >= double(r.x0) && std::floor(px) < double(r.x1) && std::floor(py) >= double(r.y0) && std::floor(py) < double(r.y1);
}

void TestScreenRect() {
  const float tan_x = 0.8f, tan_y = 0.45f;
  const uint32_t width = 1920, height = 1080;
  ra::Box box;
  box.valid = true;
  box.min[0] = -1.3f; box.max[0] = 1.3f;
  box.min[1] = -1.2f; box.max[1] = 0.5f;
  box.min[2] = 4.0f;  box.max[2] = 9.0f;
  const float to_sun[3] = {0.5f, 0.7f, -0.51f};  // normalised below
  const float sl = std::sqrt(to_sun[0] * to_sun[0] + to_sun[1] * to_sun[1] + to_sun[2] * to_sun[2]);
  const float sun[3] = {to_sun[0] / sl, to_sun[1] / sl, to_sun[2] / sl};
  const float none[3] = {0.0f, 0.0f, 0.0f};
  const float reach = 0.6f;

  const ra::Rect plain = ra::ScreenRect(box, reach, none, 0.0f, tan_x, tan_y, width, height);
  const ra::Rect with_sun = ra::ScreenRect(box, reach, sun, ra::kSunReach, tan_x, tan_y, width, height);
  Check(!plain.empty() && plain.x1 <= width && plain.y1 <= height, "a car in front of the camera has a rectangle inside the picture");
  Check(with_sun.x0 <= plain.x0 && with_sun.y0 <= plain.y0 && with_sun.x1 >= plain.x1 && with_sun.y1 >= plain.y1,
        "the shadow's extrusion only grows the rectangle");
  Check(with_sun.area() > plain.area(), "the shadow's extrusion toward the camera's side grows it");

  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u01(0.0f, 1.0f);
  bool occ_inside = true, sun_inside = true;
  for (int i = 0; i < 20000; ++i) {
    float p[3];
    for (int c = 0; c < 3; ++c) {
      p[c] = box.min[c] - reach + u01(rng) * (box.max[c] - box.min[c] + 2.0f * reach);
    }
    const double px = (double(p[0]) / (double(p[2]) * tan_x) * 0.5 + 0.5) * width;
    const double py = (0.5 - double(p[1]) / (double(p[2]) * tan_y) * 0.5) * height;
    // a point that is off the picture is clamped away: only the ones the picture has are tested
    if (px >= 0 && px < width && py >= 0 && py < height) {
      occ_inside = occ_inside && InRect(plain, px, py) && InRect(with_sun, px, py);
    }
    const float s = u01(rng) * ra::kSunReach;
    float q[3] = {p[0] - sun[0] * s, p[1] - sun[1] * s, p[2] - sun[2] * s};
    // the point in the box itself (not grown): its shadow region is the box moved away from the sun
    for (int c = 0; c < 3; ++c) {
      q[c] = box.min[c] + u01(rng) * (box.max[c] - box.min[c]) - sun[c] * s;
    }
    if (q[2] > ra::kNearZ) {
      const double qx = (double(q[0]) / (double(q[2]) * tan_x) * 0.5 + 0.5) * width;
      const double qy = (0.5 - double(q[1]) / (double(q[2]) * tan_y) * 0.5) * height;
      if (qx >= 0 && qx < width && qy >= 0 && qy < height) {
        sun_inside = sun_inside && InRect(with_sun, qx, qy);
      }
    }
  }
  Check(occ_inside, "every point within the reach of the box projects into the rectangle");
  Check(sun_inside, "every point of the box's shadow region that is on the picture projects into the rectangle");

  // near the camera or behind it: the whole picture
  ra::Box near_box = box;
  near_box.min[2] = 0.05f;
  const ra::Rect whole = ra::ScreenRect(near_box, reach, none, 0.0f, tan_x, tan_y, width, height);
  Check(whole.x0 == 0 && whole.y0 == 0 && whole.x1 == width && whole.y1 == height, "a box at the lens: the whole picture");
  // off the picture
  ra::Box left = box;
  left.min[0] = -60.0f; left.max[0] = -50.0f;
  Check(ra::ScreenRect(left, reach, none, 0.0f, tan_x, tan_y, width, height).empty(), "a box far off to the left: empty");
  Check(ra::ScreenRect(ra::Box{}, reach, none, 0.0f, tan_x, tan_y, width, height).empty(), "no box: empty");
  Check(ra::ScreenRect(box, reach, none, 0.0f, 0.0f, tan_y, width, height).empty() && ra::ScreenRect(box, reach, none, 0.0f, tan_x, tan_y, 0, height).empty(),
        "no lens or no image: empty");
  // clamped to the image
  ra::Box big = box;
  big.min[0] = -9.0f; big.max[0] = 9.0f;
  const ra::Rect clamped = ra::ScreenRect(big, reach, none, 0.0f, tan_x, tan_y, width, height);
  Check(clamped.x0 == 0 && clamped.x1 == width, "a box wider than the picture: clamped");
  // the share of the picture, for the design note
  if (g_print) {
    std::printf("rectangle of a 2.6 x 1.7 x 5 m box at 4 to 9 m: %ux%u at (%u, %u) = %.1f%% of the picture; with the sun's extrusion %ux%u = %.1f%%\n", plain.width(),
                plain.height(), plain.x0, plain.y0, 100.0 * double(plain.area()) / (double(width) * height), with_sun.width(), with_sun.height(),
                100.0 * double(with_sun.area()) / (double(width) * height));
  }
}

void TestSlab() {
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  ra::Box box;
  box.valid = true;
  box.min[0] = -1.0f; box.max[0] = 1.0f;
  box.min[1] = 0.0f;  box.max[1] = 1.5f;
  box.min[2] = 3.0f;  box.max[2] = 7.0f;
  int met = 0;
  bool no_false_negative = true, interval_inside = true, ordered = true;
  for (int i = 0; i < 4000; ++i) {
    const float origin[3] = {u(rng) * 8.0f, u(rng) * 3.0f, 1.0f + std::fabs(u(rng)) * 12.0f};
    float d[3] = {u(rng), u(rng), u(rng)};
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 0.05f) continue;
    for (float& c : d) c /= l;
    const float t_max = 8.0f;
    float t_in = 0.0f, t_out = 0.0f;
    const bool hit = ra::RayMeetsBox(origin, d, box, 0.1f, t_max, &t_in, &t_out);
    // the reference: a dense walk along the ray
    bool walked = false;
    for (int s = 0; s <= 8000 && !walked; ++s) {
      const float t = t_max * float(s) / 8000.0f;
      bool inside = true;
      for (int c = 0; c < 3; ++c) {
        const float p = origin[c] + d[c] * t;
        inside = inside && p >= box.min[c] - 0.1f && p <= box.max[c] + 0.1f;
      }
      walked = inside;
    }
    if (walked && !hit) no_false_negative = false;
    if (hit) {
      ++met;
      ordered = ordered && t_in <= t_out + 1e-5f && t_out <= t_max + 1e-5f && t_in >= -1e-5f;
      const float mid = 0.5f * (t_in + t_out);
      for (int c = 0; c < 3; ++c) {
        const float p = origin[c] + d[c] * mid;
        interval_inside = interval_inside && p >= box.min[c] - 0.1f - 1e-3f && p <= box.max[c] + 0.1f + 1e-3f;
      }
    }
  }
  Check(met > 100, Num("the random rays meet the box often enough to test (%.0f)", met));
  Check(no_false_negative, "a ray that passes through the box is never missed");
  Check(interval_inside && ordered, "the entry and exit of a ray that meets the box are ordered, inside [0, t_max], and bound a point in the box");
  // an axis-parallel ray, a ray from inside, a ray that points away
  const float o1[3] = {0.0f, 0.5f, 0.0f}, d1[3] = {0.0f, 0.0f, 1.0f};
  Check(ra::RayMeetsBox(o1, d1, box, 0.0f, 10.0f, nullptr, nullptr), "a ray parallel to z through the box meets it");
  const float o2[3] = {5.0f, 0.5f, 0.0f};
  Check(!ra::RayMeetsBox(o2, d1, box, 0.0f, 10.0f, nullptr, nullptr), "a ray parallel to z beside the box misses it");
  const float o3[3] = {0.0f, 0.5f, 5.0f};
  Check(ra::RayMeetsBox(o3, d1, box, 0.0f, 0.1f, nullptr, nullptr), "a ray that starts inside the box meets it");
  const float d4[3] = {0.0f, 0.0f, -1.0f};
  Check(!ra::RayMeetsBox(o1, d4, box, 0.0f, 10.0f, nullptr, nullptr), "a ray that points away from the box misses it");
  const float p_in[3] = {0.0f, 0.5f, 5.0f}, p_out[3] = {0.0f, 0.5f, 7.5f};
  Check(ra::NearBox(p_in, box, 0.0f) && !ra::NearBox(p_out, box, 0.4f) && ra::NearBox(p_out, box, 0.6f), "NearBox is the box grown by the reach");
}

// ---------------------------------------------------------------------------------------------------------------------
// the image, the flags, the cost
// ---------------------------------------------------------------------------------------------------------------------
void TestCodesAndFlags() {
  Check(ra::Encode8(0.0f) == 0 && ra::Encode8(1.0f) == 255 && ra::Encode8(2.0f) == 255 && ra::Encode8(-1.0f) == 0, "the 8 bit codes clamp");
  bool round_trip = true;
  for (int i = 0; i <= 1000; ++i) {
    const float x = float(i) / 1000.0f;
    round_trip = round_trip && std::fabs(ra::Decode8(ra::Encode8(x)) - x) <= 1.0f / 510.0f + 1e-6f;
  }
  Check(round_trip, "an 8 bit code is within half a step");

  // the bits the passes' flags already use (ao.cpp FillPush, ao_main_ps.hlsl, ao_contact_ps.hlsl)
  const uint32_t main_used = 0xFFu | (0xFu << 8) | (0xFu << 12) | (1u << 16) | (7u << 17) | (7u << 26);
  Check((ra::kMainFlag & main_used) == 0 && ra::kMainFlag == (1u << 31), "the main pass's flag (bit 31) overlaps none of its flags");
  const uint32_t contact_used = (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 30);
  Check((ra::kContactFlag & contact_used) == 0 && (ra::kContactViewFlag & contact_used) == 0 && ra::kContactFlag != ra::kContactViewFlag,
        "the contact march's two flags overlap none of its flags");
  Check((ra::kPushOcclusion & ra::kPushSun) == 0 && ((ra::kPushOcclusion | ra::kPushSun) >> ra::kPushRaysShift) == 0 && ra::kPushRaysShift == 8,
        "the push flags and the rays' bits do not overlap");
  Check(((ra::kMaxRays << ra::kPushRaysShift) >> 12) == 0, "four rays fit the four bits");

  Check(ra::EstimatedRays(0, 4, true) == 0.0 && ra::EstimatedRays(2000, 4, true) > ra::EstimatedRays(2000, 2, true) &&
            ra::EstimatedRays(2000, 2, false) < ra::EstimatedRays(2000, 2, true),
        "the ray estimate grows with the rays and the sun");
}

// ---------------------------------------------------------------------------------------------------------------------
// the shaders repeat the header
// ---------------------------------------------------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The value of `static const <type> <name> = <value>;` ("1u << N", an unsigned or a float with an optional f). False when it is not there.
bool Constant(const std::string& text, const std::string& name, double* value) {
  const std::string key = " " + name + " = ";
  size_t at = text.find(key);
  if (at == std::string::npos) return false;
  at += key.size();
  if (text.compare(at, 5, "1u <<") == 0) {
    *value = double(1ull << std::strtoul(text.c_str() + at + 5, nullptr, 10));
    return true;
  }
  *value = std::strtod(text.c_str() + at, nullptr);
  return true;
}

void CheckShader(const std::string& dir) {
  const std::string main_ps = ReadFile(dir + "/hlsl/ao_main_ps.hlsl");
  const std::string contact_ps = ReadFile(dir + "/hlsl/ao_contact_ps.hlsl");
  const std::string cs = ReadFile(dir + "/hlsl/car_rt_ao_cs.hlsl");
  Check(!main_ps.empty() && !contact_ps.empty() && !cs.empty(), "the three shaders are read");
  double v = 0.0;
  Check(Constant(main_ps, "kRtOcclusionFlag", &v) && uint32_t(v) == ra::kMainFlag, "ao_main_ps.hlsl kRtOcclusionFlag is the header's kMainFlag");
  Check(main_ps.find("(g_push.slots2.z & kRtOcclusionFlag) != 0u") != std::string::npos &&
            main_ps.find("g_textures[g_push.slots.x].Load(int3(pixel, 0)).x") != std::string::npos,
        "ao_main_ps.hlsl reads the traced image in slots.x when the flag is set");
  Check(Constant(contact_ps, "kRtContactFlag", &v) && uint32_t(v) == ra::kContactFlag, "ao_contact_ps.hlsl kRtContactFlag is the header's kContactFlag");
  Check(Constant(contact_ps, "kRtContactViewFlag", &v) && uint32_t(v) == ra::kContactViewFlag,
        "ao_contact_ps.hlsl kRtContactViewFlag is the header's kContactViewFlag");
  Check(Constant(contact_ps, "kSunFacingLow", &v) && Near(v, ra::kSunFacingLow, 1e-6), "the contact march's sun facing limit is the header's");

  Check(Constant(cs, "kPushOcclusion", &v) && uint32_t(v) == ra::kPushOcclusion, "car_rt_ao_cs.hlsl kPushOcclusion is the header's");
  Check(Constant(cs, "kPushSun", &v) && uint32_t(v) == ra::kPushSun, "car_rt_ao_cs.hlsl kPushSun is the header's");
  Check(Constant(cs, "kMinAbove", &v) && Near(v, ra::kMinAbove, 1e-6), "car_rt_ao_cs.hlsl kMinAbove is the header's");
  Check(Constant(cs, "kSelfCos", &v) && Near(v, ra::kSelfCos, 1e-6), "car_rt_ao_cs.hlsl kSelfCos is the header's");
  Check(Constant(cs, "kSunFacingLow", &v) && Near(v, ra::kSunFacingLow, 1e-6), "car_rt_ao_cs.hlsl kSunFacingLow is the header's");
  Check(Constant(cs, "kDepthLiftFloor", &v) && Near(v, ra::kDepthLiftFloor, 1e-6), "car_rt_ao_cs.hlsl kDepthLiftFloor is the header's");
  Check(Constant(cs, "kDepthLiftPixels", &v) && Near(v, ra::kDepthLiftPixels, 1e-6), "car_rt_ao_cs.hlsl kDepthLiftPixels is the header's");
  Check(Constant(cs, "kGolden", &v) && Near(v, ra::kGolden, 1e-6), "car_rt_ao_cs.hlsl kGolden is the header's");
  Check(Constant(cs, "kLatticeFlag", &v) && uint32_t(v) == as::kLatticeFlag, "car_rt_ao_cs.hlsl kLatticeFlag is the occlusion's (ao_stable.h)");
  Check(cs.find("lift = 0.004 + 0.002 * snapped_p.z;") != std::string::npos && Near(ra::SnappedLift(2.0f), 0.004 + 0.002 * 2.0, 1e-6),
        "car_rt_ao_cs.hlsl lifts a snapped pixel as SnappedLift");
  Check(cs.find("(flags >> 8) & 0xFu") != std::string::npos && ra::kPushRaysShift == 8, "car_rt_ao_cs.hlsl reads the rays a pixel from bits 8-11");
  Check(cs.find("frac(noise.x * 13.7 + noise.y * 5.3)") != std::string::npos, "car_rt_ao_cs.hlsl turns the length strata as LengthRotation does");
  Check(cs.find("(k * 3u + 1u + rotation) % rays") != std::string::npos, "car_rt_ao_cs.hlsl pairs the strata as LengthStratum does");
  Check(cs.find("reach * (1.0 - sqrt(u))") != std::string::npos, "car_rt_ao_cs.hlsl makes the ray's length as RayLength does");
  Check(cs.find("(float(k) + min(noise.y, 0.9999)) / float(rays)") != std::string::npos, "car_rt_ao_cs.hlsl stratifies the radius as CosineLocal does");
  Check(cs.find("kTwoPi * (noise.x + kGolden * float(k))") != std::string::npos, "car_rt_ao_cs.hlsl turns the pattern as CosineLocal does");
  Check(cs.find("struct CarRtAoPush") != std::string::npos && cs.find("uint4 slots;") != std::string::npos && cs.find("float4 box_max;") != std::string::npos,
        "car_rt_ao_cs.hlsl has the push block's seven vectors");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      src_dir = argv[i];
    }
  }
  TestSetting();
  TestConstants();
  TestPattern();
  TestLengths();
  TestPlaneOverhead();
  TestSunCone();
  TestViewBox();
  TestScreenRect();
  TestSlab();
  TestCodesAndFlags();
  if (!src_dir.empty()) {
    CheckShader(src_dir);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
