// Check of the car chrome's rules (car_chrome_rules.h): the numbers of the two short probes of shaderpack/variants/car_chrome.h, and a
// reference march of them over small scenes made of boxes seen by a pinhole camera: a tailpipe's top under a lip of the bumper, a tube
// with a pipe's housing in front of it, the same tube in the open, and a plane at every slope and distance. A program of its own: it needs
// no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_chrome_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_chrome_rules_test
//
// The march is the variant helper's (CarChrProbe: a chain of segments through the history, the outline of a part found by halving), made
// with the functions of car_selfrefl_rules.h and car_chrome_rules.h; the history is a depth image the camera would see (a box's front
// face, in half float as the history stores it), with no vehicle pixel where nothing is.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "car_chrome_rules.h"

namespace cc = mcla::gpu::car_chrome;
namespace cr = mcla::gpu::car_selfrefl;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

// ---------------------------------------------------------------------------------------------------------------------
// A depth image of boxes
// ---------------------------------------------------------------------------------------------------------------------
struct Box {
  float lo[3];
  float hi[3];
};

// What a half float stores of x (10 bits of mantissa, 11 with the implicit one): the history's alpha is RGBA16F.
float ToHalf(float x) {
  if (x == 0.0f) return 0.0f;
  int e = 0;
  const float m = std::frexp(x, &e);                       // x = m 2^e, m in [0.5, 1)
  return std::ldexp(std::round(m * 2048.0f) / 2048.0f, e);  // 11 significant bits
}

struct Camera {
  float height = 1.23f;    // the camera's y; it looks along +z, x right, y up (view space with the camera moved to the origin below)
  int width = 3840;
  int rows = 2160;
  float focal_px = 4712.0f;  // 4K at 6.7 m: the picture the dump was measured with (1.4 mm a pixel)
};

struct Scene {
  Camera cam;
  std::vector<Box> boxes;
  float noise = 0.0f;  // an offset the history's depth carries (metres), the game's depth noise stood in by a constant
  // The view distance (z) of the first box the ray through pixel (px, py) hits; 0 where there is none.
  float Depth(int px, int py) const {
    const float dx = (float(px) + 0.5f - 0.5f * float(cam.width)) / cam.focal_px;
    const float dy = -(float(py) + 0.5f - 0.5f * float(cam.rows)) / cam.focal_px;
    float best = 0.0f;
    for (const Box& b : boxes) {
      float t0 = 0.0f, t1 = 1e9f;
      bool ok = true;
      const float o[3] = {0.0f, cam.height, 0.0f};
      const float d[3] = {dx, dy, 1.0f};
      for (int k = 0; k < 3 && ok; ++k) {
        if (std::fabs(d[k]) < 1e-9f) {
          ok = o[k] >= b.lo[k] && o[k] <= b.hi[k];
        } else {
          float a = (b.lo[k] - o[k]) / d[k];
          float c = (b.hi[k] - o[k]) / d[k];
          if (a > c) std::swap(a, c);
          t0 = std::max(t0, a);
          t1 = std::min(t1, c);
          ok = t0 <= t1;
        }
      }
      if (ok && t0 > 0.0f && (best == 0.0f || t0 < best)) best = t0;  // d.z = 1: the ray parameter is z
    }
    return best > 0.0f ? ToHalf(best + noise) : 0.0f;
  }
  // The pixel a world point (x, y, z with the camera at (0, height, 0)) lands in, and its view distance.
  void Project(const float p[3], float* px, float* py, float* z) const {
    *z = p[2];
    *px = 0.5f * float(cam.width) + cam.focal_px * p[0] / p[2];
    *py = 0.5f * float(cam.rows) - cam.focal_px * (p[1] - cam.height) / p[2];
  }
};

// The march of the variant's CarChrProbe, with the numbers of car_chrome_rules.h. Returns the hit weight before the self-test's agreement.
float March(const Scene& scene, const float p0[3], const float dir[3], int steps, float length, float nv) {
  float z0, px0, py0;
  scene.Project(p0, &px0, &py0, &z0);
  const float focal = 2.0f * scene.cam.focal_px / float(scene.cam.rows);  // 1 / tan(half the vertical field of view)
  const float slack = cc::SurfaceSlack(z0, float(scene.cam.rows), focal, nv);
  const cr::WindowShape shape = cc::MakeWindowShape(z0, cc::Thickness(z0), slack);
  struct Point {
    float t = 0.0f;
    cr::SegmentEnd end;
  };
  auto sample = [&](float t, Point* out, bool* inside) {
    const float q[3] = {p0[0] + dir[0] * t, p0[1] + dir[1] * t, p0[2] + dir[2] * t};
    float px, py, z;
    scene.Project(q, &px, &py, &z);
    *inside = z > 0.1f && px >= 0.0f && py >= 0.0f && px < float(scene.cam.width) && py < float(scene.cam.rows);
    if (!*inside) return;
    const float stored = scene.Depth(int(std::floor(px)), int(std::floor(py)));
    out->t = t;
    out->end.on = stored > 0.0f;
    out->end.stored = stored;
    out->end.diff = z - stored;
    out->end.t = t;
  };
  cr::HitSum sum;
  auto add = [&](const Point& a, const Point& b) {
    if (cr::HasOutline(a.end, b.end, shape)) {
      const bool a_in = cr::InWindow(a.end, shape);
      const Point in_end = a_in ? a : b;
      const Point other_end = a_in ? b : a;
      Point inner = in_end;
      Point outer = other_end;
      for (int k = 0; k < cr::kOutlineSteps; ++k) {
        Point m;
        bool inside = false;
        sample(0.5f * (inner.t + outer.t), &m, &inside);
        if (!inside) m = outer;
        if (cr::InWindow(m.end, shape)) {
          inner = m;
        } else {
          outer = m;
        }
      }
      const float outline = 0.5f * (inner.t + outer.t);
      const bool in_first = in_end.t < inner.t;
      const Point& first = in_first ? in_end : inner;
      const Point& second = in_first ? inner : in_end;
      cr::Segment s = cr::SegmentOf(first.end, second.end, shape);
      s.cover *= cr::OutlineShare(in_end.t, inner.t, outer.t, other_end.t);
      sum.Add(s, std::min(in_end.t, outline), std::max(in_end.t, outline), length, 1.0f, 0.0f, 0.0f);
    } else if (cr::MayCover(a.end, b.end, shape)) {
      sum.Add(cr::SegmentOf(a.end, b.end, shape), a.t, b.t, length, 1.0f, 0.0f, 0.0f);
    }
  };
  Point previous;
  previous.end.on = true;  // the shaded point itself: on the vehicle, diff 0
  previous.end.stored = z0;
  const float full = cr::FullPoints(cc::kFull, steps);
  for (int i = 0; i < steps; ++i) {
    Point point;
    bool inside = false;
    sample(cc::RayDistance(i, steps, length), &point, &inside);
    if (!inside) break;
    add(previous, point);
    previous = point;
    if (sum.Occupied() >= full) break;
  }
  return sum.Weight(cc::kFull, steps);
}

// The agreement of the shaded point with the history at its own pixel (the setup's self-test).
float AgreementAt(const Scene& scene, const float p[3], float nv) {
  float px, py, z;
  scene.Project(p, &px, &py, &z);
  const float stored = scene.Depth(int(std::floor(px)), int(std::floor(py)));
  if (stored <= 0.0f) return 0.0f;
  const float focal = 2.0f * scene.cam.focal_px / float(scene.cam.rows);
  return cc::Agreement(stored, z, cc::SurfaceSlack(z, float(scene.cam.rows), focal, nv));
}

// The self-test with the history's texel read `dx`, `dy` pixels from the point's own pixel (what the guest's sample 0 does at a rim: the texel under the
// pixel is another's), and the same with the rim's search of the neighbours (the variant's CarChrSilhouetteAgreement, car_chrome_rules.h).
float AgreementShifted(const Scene& scene, const float p[3], float nv, int dx, int dy, bool search) {
  float px, py, z;
  scene.Project(p, &px, &py, &z);
  const int ix = int(std::floor(px)) + dx, iy = int(std::floor(py)) + dy;
  const float focal = 2.0f * scene.cam.focal_px / float(scene.cam.rows);
  const float slack = cc::SurfaceSlack(z, float(scene.cam.rows), focal, nv);
  const float own = cc::TexelAgreement(scene.Depth(ix, iy), z, slack);
  if (!search) return own;
  float stored[cc::kSilhouetteTaps];
  for (int k = 0; k < cc::kSilhouetteTaps; ++k) {
    stored[k] = scene.Depth(ix + cc::kSilhouetteOffsets[k][0], iy + cc::kSilhouetteOffsets[k][1]);
  }
  return cc::SilhouetteAgreement(own, stored, z, slack);
}

void Normalize(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int k = 0; k < 3; ++k) v[k] /= l;
}

// The reflected view vector at point p with normal n, as the variant does it: 2 (N.V) N - V, V towards the camera.
void Reflect(const Scene& scene, const float p[3], const float n[3], float out[3], float* nv) {
  float v[3] = {-p[0], scene.cam.height - p[1], -p[2]};
  v[1] = scene.cam.height - p[1];
  Normalize(v);
  *nv = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
  for (int k = 0; k < 3; ++k) out[k] = 2.0f * *nv * n[k] - v[k];
}

}  // namespace

int main() {
  // ---- the numbers ---------------------------------------------------------------------------------------------------
  // The points: first at least kStartMin, the last at the length, in increasing order, closer together near the shaded point.
  for (const int steps : {cc::kReflectSteps, cc::kSkySteps}) {
    const float length = steps == cc::kReflectSteps ? cc::kReflectLength : cc::kSkyLength;
    float previous = 0.0f;
    bool increasing = true;
    for (int i = 0; i < steps; ++i) {
      const float d = cc::RayDistance(i, steps, length);
      increasing = increasing && d > previous;
      previous = d;
    }
    Check(increasing, "the points of a probe of " + std::to_string(steps) + " steps are increasing");
    Check(Near(cc::RayDistance(steps - 1, steps, length), length), "the last point is at the length");
    Check(cc::RayDistance(0, steps, length) >= cc::kStartMin - 1e-6f && cc::RayDistance(0, steps, length) < 0.02f,
          "the first point is 8 mm to 2 cm from the shaded point (the paint's was 4 cm)");
    Check(cc::RayDistance(1, steps, length) - cc::RayDistance(0, steps, length) <
              cc::RayDistance(steps - 1, steps, length) - cc::RayDistance(steps - 2, steps, length),
          "the points are closer together near the shaded point");
  }
  // The window: its ramps never overlap and the bias is under half the thickness, at every distance and slack the shader can meet.
  bool shapes_ok = true;
  for (float depth = 0.5f; depth <= 200.0f; depth *= 1.5f) {
    for (float slack = 0.0f; slack <= 0.03f; slack += 0.0075f) {
      const cr::WindowShape w = cc::MakeWindowShape(depth, cc::Thickness(depth), slack);
      shapes_ok = shapes_ok && w.bias >= 0.0f && w.ramp > 0.0f && w.bias + w.ramp <= w.fall + 1e-6f && w.fall < w.thickness &&
                  Near(w.fall, 0.5f * w.thickness);
    }
  }
  Check(shapes_ok, "the window's rise ends before its fall starts, at every distance from 0.5 m to 200 m and every slack");
  {
    const cr::WindowShape at_dump = cc::MakeWindowShape(6.7f, cc::Thickness(6.7f), 0.0f);
    const cr::WindowShape paint = cr::MakeWindowShape(6.7f, 0.3f, 0.0f);
    Check(at_dump.bias < 0.5f * paint.bias + 1e-4f && at_dump.bias > 0.010f && at_dump.bias < 0.020f,
          "at 6.7 m the window opens 1 to 2 cm behind a vehicle pixel, under half the paint's");
    Check(Near(cc::Thickness(6.7f), 0.1202f, 1e-3) && cc::Thickness(30.0f) > cc::Thickness(6.7f), "the thickness is 12 cm at 6.7 m and grows with the distance");
  }
  // The agreement: 1 at the right distance, 0 past the tolerance, between in a smooth fall.
  Check(Near(cc::Agreement(6.7f, 6.7f, 0.0f), 1.0f), "a point found at its own distance agrees fully");
  Check(cc::Agreement(6.7f + 0.2f, 6.7f, 0.0f) == 0.0f, "a vehicle pixel 20 cm away is not the shaded point");
  {
    const float paint_tolerance = 0.06f + 0.02f * 6.7f;  // the paint variant's self-test (car_selfrefl.cpp CarReflTrace)
    const float paint_agreement = 1.0f - cr::SmoothStep(0.5f * paint_tolerance, paint_tolerance, 0.05f);
    Check(cc::Agreement(6.7f + 0.05f, 6.7f, 0.0f) == 0.0f && paint_agreement == 1.0f,
          "the lip 5 cm in front of a tube is not the tube (the paint's tolerance of 19 cm would take it for the tube)");
  }
  Check(cc::Agreement(6.7f + 0.02f, 6.7f, 0.0f) == 1.0f, "2 cm is inside the tolerance's inner half");
  Check(cc::Agreement(6.7f + 0.0364f, 6.7f, 0.0f) > 0.3f && cc::Agreement(6.7f + 0.0364f, 6.7f, 0.0f) < 0.7f, "3.6 cm is half in, half out");
  Check(cc::Agreement(6.7f + 0.004f, 6.7f, 0.0f) == 1.0f, "4 mm (the history's half float step at 6.7 m) is no disagreement");

  // The rim of a part (the thin bright line round a tip's top edge, version 3): the history's texel under a pixel at the silhouette is what is behind the tip,
  // the self-test says 0 and the game's bright chrome stays; the texels round it hold the tip.
  {
    // The offsets: twelve, all different, none the texel itself, the eight round it first, the others two steps away along an axis.
    bool distinct = true, ring = true, near = true;
    for (int a = 0; a < cc::kSilhouetteTaps; ++a) {
      const int ax = cc::kSilhouetteOffsets[a][0], ay = cc::kSilhouetteOffsets[a][1];
      near = near && (ax != 0 || ay != 0) && std::abs(ax) <= 2 && std::abs(ay) <= 2;
      ring = ring && (a < 8 ? (std::abs(ax) <= 1 && std::abs(ay) <= 1) : (std::abs(ax) + std::abs(ay) == 2 && (ax == 0 || ay == 0)));
      for (int b = a + 1; b < cc::kSilhouetteTaps; ++b) {
        distinct = distinct && (ax != cc::kSilhouetteOffsets[b][0] || ay != cc::kSilhouetteOffsets[b][1]);
      }
    }
    Check(cc::kSilhouetteTaps == 12 && distinct && near && ring,
          "the rim's twelve offsets are different, are not the texel itself, and are within two steps (eight round it, four two steps along an axis)");

    // The rule alone: never lower than the own agreement, the own agreement where it is 1, 0 where no texel agrees, a texel that holds no vehicle never agrees.
    float none[cc::kSilhouetteTaps] = {};
    float behind[cc::kSilhouetteTaps], right[cc::kSilhouetteTaps], mixed[cc::kSilhouetteTaps];
    for (int k = 0; k < cc::kSilhouetteTaps; ++k) {
      behind[k] = 6.7f + 0.06f;  // the bumper's cavity 6 cm further
      right[k] = 6.7f;
      mixed[k] = k == 5 ? 6.7f + 0.01f : 0.0f;
    }
    Check(cc::SilhouetteAgreement(1.0f, behind, 6.7f, 0.0f) == 1.0f && cc::SilhouetteAgreement(1.0f, none, 6.7f, 0.0f) == 1.0f, "a pixel that agrees fully keeps it");
    Check(cc::SilhouetteAgreement(0.4f, behind, 6.7f, 0.0f) == 0.4f && cc::SilhouetteAgreement(0.4f, right, 6.7f, 0.0f) == 1.0f,
          "never lower than the own agreement; a neighbour that holds the point's surface lifts it");
    Check(cc::SilhouetteAgreement(0.0f, none, 6.7f, 0.0f) == 0.0f && cc::SilhouetteAgreement(0.0f, behind, 6.7f, 0.0f) == 0.0f,
          "no texel round it holds a vehicle, or all hold the cavity behind: 0 stays 0");
    Check(cc::SilhouetteAgreement(0.0f, mixed, 6.7f, 0.0f) == 1.0f, "a single neighbour (the diagonal one) that agrees is enough");
    Check(cc::TexelAgreement(0.0f, 0.1f, 0.0f) == 0.0f && cc::TexelAgreement(6.7f, 6.7f, 0.0f) == 1.0f && cc::TexelAgreement(6.7f + 0.2f, 6.7f, 0.0f) == 0.0f,
          "a texel with no vehicle never agrees, one at the right distance does, one 20 cm off does not");

    // A tip's top (a 14 cm by 10 cm face, 6.7 m from the camera) in front of the bumper's cavity 15 cm behind it; the shaded pixel at the tip's left edge, its
    // history texel read one to two pixels to the left (the sample that falls outside the tip).
    Scene scene;
    scene.boxes.push_back({{-0.07f, -0.06f, 6.70f}, {0.07f, 0.0f, 6.80f}});    // the tube's top
    scene.boxes.push_back({{-0.50f, -0.30f, 6.85f}, {0.50f, 0.30f, 7.00f}});   // the bumper's cavity behind it
    const float rim[3] = {-0.0693f, 0.0f, 6.72f};
    const float centre[3] = {0.0f, 0.0f, 6.72f};
    const float nv = 0.9f;
    const float inside = AgreementShifted(scene, centre, nv, 0, 0, false);
    Check(inside > 0.99f, "the tip's middle finds itself in the history at its own pixel: " + std::to_string(inside));
    const float own_ok = AgreementShifted(scene, rim, nv, 0, 0, false);
    Check(own_ok > 0.99f, "the rim's own texel is the tip's when the sample is inside it: " + std::to_string(own_ok));
    for (const int dx : {-1, -2}) {
      const float before = AgreementShifted(scene, rim, nv, dx, 0, false);
      const float after = AgreementShifted(scene, rim, nv, dx, 0, true);
      Check(before == 0.0f, "the rim's texel " + std::to_string(-dx) + " pixel(s) to the left is the cavity 15 cm behind: the old self-test says " + std::to_string(before));
      Check(after > 0.99f, "the rim's search finds the tip " + std::to_string(-dx) + " step(s) away: " + std::to_string(after));
    }
    // The cavity right up to the tip (10 cm behind it) and the sample that falls on the diagonal.
    scene.boxes[1] = {{-0.50f, -0.30f, 6.80f}, {0.50f, 0.30f, 7.00f}};
    {
      const float before = AgreementShifted(scene, rim, nv, -1, -1, false);
      const float after = AgreementShifted(scene, rim, nv, -1, -1, true);
      Check(before < 0.05f && after > 0.99f, "the diagonal: " + std::to_string(before) + " before, " + std::to_string(after) + " with the search");
    }
    // Where there is no vehicle behind the tip at all (the open street): the texel holds nothing, the old self-test said 0, the search finds the tip.
    Scene open;
    open.boxes.push_back({{-0.07f, -0.06f, 6.70f}, {0.07f, 0.0f, 6.80f}});
    {
      const float before = AgreementShifted(open, rim, nv, -1, 0, false);
      const float after = AgreementShifted(open, rim, nv, -1, 0, true);
      Check(before == 0.0f && after > 0.99f, "no vehicle behind the tip: " + std::to_string(before) + " before, " + std::to_string(after) + " with the search");
    }
    // What must stay left to the game: a point whose history is somewhere else (a wrong guess of the car's motion: 10 pixels off), a point that is 22 cm
    // nearer than anything in the history. And the cavity's own point finds itself.
    {
      const float off = AgreementShifted(scene, rim, nv, -10, 0, true);
      Check(off == 0.0f, "a history 10 pixels off is no better for the search: " + std::to_string(off));
      const float cavity_point[3] = {-0.0715f, 0.0f, 6.80f};  // on the cavity's face just left of the tip's edge
      const float cavity = AgreementShifted(scene, cavity_point, nv, 0, 0, true);
      Check(cavity > 0.99f, "a point on the cavity finds the cavity (its own texel): " + std::to_string(cavity));
      const float wrong[3] = {-0.0693f, 0.0f, 6.50f};  // 22 cm nearer than the tip: nothing in the history is at its distance
      const float lip = AgreementShifted(scene, wrong, nv, -1, 0, true);
      Check(lip == 0.0f, "a point 22 cm nearer than anything in the history stays at 0: " + std::to_string(lip));
    }
    // At 1080p (a 2.8 mm pixel).
    Scene lo = scene;
    lo.cam.width = 1920;
    lo.cam.rows = 1080;
    lo.cam.focal_px = 2356.0f;
    {
      const float before = AgreementShifted(lo, rim, nv, -1, 0, false);
      const float after = AgreementShifted(lo, rim, nv, -1, 0, true);
      Check(after >= before && after > 0.99f, "at 1080p: " + std::to_string(before) + " before, " + std::to_string(after) + " with the search");
    }
  }

  // The sky probe's direction.
  {
    const cc::Dir up = cc::SkyDirection(0.0f, 1.0f, 0.0f);
    Check(Near(up.y, 1.0f) && Near(up.x, 0.0f) && Near(up.z, 0.0f), "a normal that points up: the probe goes up");
    const cc::Dir side = cc::SkyDirection(1.0f, 0.0f, 0.0f);
    Check(Near(side.x, std::sqrt(0.5f)) && Near(side.y, std::sqrt(0.5f)), "a normal that points sideways: the probe goes up at 45 degrees");
    const cc::Dir down = cc::SkyDirection(0.0f, -1.0f, 0.0f);
    Check(Near(down.y, 1.0f), "a normal that points straight down: no sky to look at, the probe takes up");
    const cc::Dir back = cc::SkyDirection(0.3f, -0.95f, 0.0f);
    Check(std::fabs(back.x * back.x + back.y * back.y + back.z * back.z - 1.0f) < 1e-5f, "the probe's direction is a unit vector");
  }

  // The colour: the game's own arithmetic with `env` and the ambient occluded.
  {
    const float game = 0.8f * 1.0f * (0.6f + (0.2f - 0.6f) * 0.3f);  // ao * sun * mix(env, amb, blend)
    Check(Near(cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.3f, 0.0f, 0.0f, 1.0f), game), "no hit: the game's chrome, bit for bit");
    const float dark_env = cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.0f, 1.0f, 0.0f, 1.0f);
    Check(Near(dark_env, 0.8f * 0.6f * (1.0f - cc::kDark)), "a full reflection hit leaves a tenth of `env` where the blend is 0");
    const float dark_amb = cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f);
    Check(Near(dark_amb, 0.8f * 0.2f * (1.0f - cc::kDark)), "a full sky hit leaves a tenth of the ambient where the blend is 1 (a thin tube far away)");
    Check(Near(cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.3f, 1.0f, 1.0f, 0.0f), game), "strength 0: the game's chrome");
    Check(cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.0f, 0.5f, 0.0f, 1.0f) < cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.0f, 0.25f, 0.0f, 1.0f) &&
              cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.0f, 0.25f, 0.0f, 1.0f) < game + 1.0f,
          "a heavier hit is darker");
    Check(cc::Occlude(1.0f, 7.0f, 7.0f) >= 1.0f - cc::kDark - 1e-6f && cc::Occlude(1.0f, -3.0f, 1.0f) <= 1.0f + 1e-6f,
          "the weight is clamped: never under a tenth of the colour, never brighter than it");
  }

  // ---- the probes over boxes ----------------------------------------------------------------------------------------
  // A tailpipe's top (a 14 cm by 10 cm face at y = 0, the camera 1.23 m above it and 6.7 m away) with the lip of the bumper 2 cm above
  // it whose front face is 4 cm nearer the camera, as on the R34 of the car dump.
  {
    Scene scene;
    scene.boxes.push_back({{-0.07f, -0.06f, 6.70f}, {0.07f, 0.0f, 6.80f}});   // the tube's top
    scene.boxes.push_back({{-0.50f, 0.02f, 6.66f}, {0.50f, 0.30f, 6.80f}});   // the lip over it
    const float p[3] = {0.0f, 0.0f, 6.72f};
    const float n[3] = {0.0f, 1.0f, 0.0f};
    const cc::Dir sky = cc::SkyDirection(n[0], n[1], n[2]);
    const float sky_dir[3] = {sky.x, sky.y, sky.z};
    float nv = 0.0f;
    float reflected[3];
    Reflect(scene, p, n, reflected, &nv);
    const float agreement = AgreementAt(scene, p, nv);
    const float up_weight = March(scene, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv);
    Check(agreement > 0.9f, "the tube's top is found in the history at its own distance: agreement " + std::to_string(agreement));
    Check(up_weight > 0.5f, "the sky probe finds the lip 2 cm above the tube: weight " + std::to_string(up_weight));
    std::printf("  tube under a lip: sky probe weight %.3f, agreement %.3f, N.V %.3f\n", up_weight, agreement, nv);

    // The same tube in the open: no lip.
    Scene open;
    open.boxes.push_back({{-0.07f, -0.06f, 6.70f}, {0.07f, 0.0f, 6.80f}});
    const float open_weight = March(open, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv);
    Check(open_weight < 0.02f, "the same tube with nothing above it: the sky probe finds nothing: " + std::to_string(open_weight));
    const float open_reflect = March(open, p, reflected, cc::kReflectSteps, cc::kReflectLength, nv);
    Check(open_reflect < 0.02f, "and neither does its reflection probe: " + std::to_string(open_reflect));

    // A pipe's housing 4 cm in front of the tube's top (further from the camera): the reflection goes into it.
    Scene housed = open;
    housed.boxes.push_back({{-0.30f, -0.08f, 6.76f}, {0.30f, 0.10f, 6.95f}});
    const float p2[3] = {0.0f, 0.0f, 6.70f};
    Reflect(housed, p2, n, reflected, &nv);
    const float housed_weight = March(housed, p2, reflected, cc::kReflectSteps, cc::kReflectLength, nv);
    const float housed_agree = AgreementAt(housed, p2, nv);
    Check(housed_weight > 0.5f, "the reflection of a tube whose top looks into a housing 6 cm ahead hits it: " + std::to_string(housed_weight));
    Check(housed_agree > 0.9f, "the tube's top is itself found at its own distance beside the housing: " + std::to_string(housed_agree));
    std::printf("  tube beside a housing: reflection probe weight %.3f, open %.3f\n", housed_weight, open_reflect);

    // The history's noise: a few millimetres of constant offset in the depth do not take away the lip, and do not make one.
    for (const float noise : {-0.006f, 0.006f}) {
      Scene noisy = scene;
      noisy.noise = noise;
      Scene noisy_open = open;
      noisy_open.noise = noise;
      Check(March(noisy, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv) > 0.3f,
            "6 mm of depth offset (" + std::to_string(noise) + "): the lip is still found");
      Check(March(noisy_open, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv) < 0.05f,
            "6 mm of depth offset (" + std::to_string(noise) + "): an open tube still reads open");
    }
    // The same at 1080p (a 2.8 mm pixel).
    Scene lo = scene;
    lo.cam.width = 1920;
    lo.cam.rows = 1080;
    lo.cam.focal_px = 2356.0f;
    Scene lo_open = open;
    lo_open.cam = lo.cam;
    const float lo_weight = March(lo, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv);
    const float lo_open_weight = March(lo_open, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv);
    Check(lo_weight > 0.5f && lo_open_weight < 0.02f, "at 1080p: the lip is found (" + std::to_string(lo_weight) + ") and the open tube is not (" +
                                                          std::to_string(lo_open_weight) + ")");
  }

  // A plane of every slope at several distances: nothing is there but the plane itself, so no probe of any direction may find a hit (the
  // slack of a slanted surface and the bias of the window are what keep a surface from being found behind its own pixels). The plane is
  // y = s (z - depth), a staircase of boxes 1 mm deep (below the history's own step) whose normal (0, 1, -s) turns from the sky to the camera
  // as s grows; the shaded point is on it at (0, 0, depth).
  {
    float worst = 0.0f;
    float worst_slope = 0.0f;
    float worst_depth = 0.0f;
    int cases = 0;
    for (const float depth : {3.0f, 6.7f, 12.0f, 25.0f}) {
      for (const float slope : {0.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f}) {
        Scene scene;
        const float dz = 0.001f;
        const float span = 0.5f;
        for (float z = depth - span; z < depth + span; z += dz) {
          const float top = slope * (z + 0.5f * dz - depth);
          scene.boxes.push_back({{-0.5f, top - 0.2f, z}, {0.5f, top, z + dz}});
        }
        const float p[3] = {0.0f, 0.0f, depth};
        float nrm[3] = {0.0f, 1.0f, -slope};
        Normalize(nrm);
        float reflected[3];
        float nv = 0.0f;
        Reflect(scene, p, nrm, reflected, &nv);
        if (nv < 0.05f) continue;
        const cc::Dir sky = cc::SkyDirection(nrm[0], nrm[1], nrm[2]);
        const float sky_dir[3] = {sky.x, sky.y, sky.z};
        const float w = std::max({March(scene, p, sky_dir, cc::kSkySteps, cc::kSkyLength, nv),
                                  March(scene, p, reflected, cc::kReflectSteps, cc::kReflectLength, nv),
                                  March(scene, p, nrm, cc::kSkySteps, cc::kSkyLength, nv)});
        ++cases;
        if (w > worst) {
          worst = w;
          worst_slope = slope;
          worst_depth = depth;
        }
      }
    }
    Check(cases >= 20 && worst < 0.05f,
          "a bare surface at 3 to 25 m and slopes 0 to 4: no probe of any of the three directions finds a hit (worst " +
              std::to_string(worst) + " at slope " + std::to_string(worst_slope) + ", " + std::to_string(worst_depth) + " m, " +
              std::to_string(cases) + " cases)");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
