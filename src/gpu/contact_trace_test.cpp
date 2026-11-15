// Check of the contact shadows' trace rules (contact_trace.h) and of its C++ twin of the shader's march. A
// program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_contact_trace_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_contact_trace_test [<src/gpu/hlsl dir>]
//
// With the hlsl folder as its argument it also checks that the shader's constants are the header's (the shader repeats them).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "contact_trace.h"

using namespace mcla::gpu::contact_trace;

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[200];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}

// ---------------------------------------------------------------------------
// synthetic scenes: a ground plane (optionally inclined) and boxes, seen from the origin looking along +z
// ---------------------------------------------------------------------------

struct Box {
  float x0, x1, y0, y1, z0, z1;
  bool vehicle = false;  // the depth pass would mark its pixels as a vehicle's (the lowest bit of the depth)
};

struct Scene {
  Camera cam;
  float ground_y = -1.5f;  // the camera is 1.5 m above the ground
  float incline = 0.0f;    // y = ground_y + incline * z
  bool ground = true;
  std::vector<Box> boxes;

  // The linear depth of the pixel (x, y) with the vehicle bit, as the depth pass writes it: the nearer of
  // the ground and the boxes' first face, the lowest bit set when that face is a vehicle's.
  float DepthV(int x, int y) const {
    bool vehicle = false;
    const float d = Hit(x, y, &vehicle);
    return DepthWithVehicleBit(d, vehicle);
  }
  // The linear depth of the pixel (x, y): the nearer of the ground and the boxes' first face.
  float Depth(int x, int y) const {
    bool vehicle = false;
    return Hit(x, y, &vehicle);
  }
  float Hit(int x, int y, bool* vehicle) const {
    const Vec3 r = ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, 1.0f);  // (rx, ry, 1)
    float best = 60000.0f;
    *vehicle = false;
    if (ground) {
      const float den = r.y - incline;
      if (den < -1e-6f) {
        best = std::min(best, ground_y / den);
      }
    }
    for (const Box& b : boxes) {
      float tn = 0.0f, tf = 1e30f;
      const float o[3][2] = {{b.x0, b.x1}, {b.y0, b.y1}, {b.z0, b.z1}};
      const float d[3] = {r.x, r.y, r.z};
      bool miss = false;
      for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-9f) {
          miss = miss || o[a][0] > 0.0f || o[a][1] < 0.0f;
          continue;
        }
        const float t1 = o[a][0] / d[a], t2 = o[a][1] / d[a];
        tn = std::max(tn, std::min(t1, t2));
        tf = std::min(tf, std::max(t1, t2));
      }
      if (!miss && tn <= tf && tf > 0.0f && tn < best) {
        best = tn;
        *vehicle = b.vehicle;
      }
    }
    return best;
  }
};

Camera MakeCamera(int height) {
  Camera c;
  c.tan_y = 0.2217f;  // 25 degrees, the game's field of view
  c.tan_x = c.tan_y * 16.0f / 9.0f;
  c.height = height;
  c.width = height * 16 / 9;
  return c;
}

Vec3 Sun(double elevation_degrees, double azimuth_degrees) {
  const double pi = 3.14159265358979323846;
  const double e = elevation_degrees * pi / 180.0, a = azimuth_degrees * pi / 180.0;
  return {float(std::sin(a) * std::cos(e)), float(std::sin(e)), float(std::cos(a) * std::cos(e))};
}

// ---------------------------------------------------------------------------
// the rules
// ---------------------------------------------------------------------------

void RayLengthRule() {
  Check(std::fabs(LengthGrow(60.0) - 1.0) < 1e-9, "a high sun keeps the setting's length");
  Check(std::fabs(LengthGrow(18.7) - 2.07) < 0.01, Num("18.7 degrees grows the ray %.3f times (2.07)", LengthGrow(18.7)));
  Check(LengthGrow(5.0) == kLengthMaxScale && LengthGrow(0.0) == kLengthMaxScale, "a low sun is capped at 3 times");
  double last = 1e9;
  bool monotone = true;
  for (double e = 0.0; e <= 90.0; e += 0.5) {
    const double g = LengthGrow(e);
    monotone = monotone && g <= last + 1e-12 && g >= 1.0 && g <= kLengthMaxScale;
    last = g;
  }
  Check(monotone, "the growth never increases with the elevation and stays between 1 and 3");
}

void StepsAndFade() {
  for (uint32_t steps : {kStepsLow, kStepsHigh}) {
    for (float noise : {0.0f, 0.5f, 0.999f}) {
      bool increasing = Along(0, noise, steps) >= 0.0f;
      for (uint32_t i = 1; i < steps; ++i) {
        increasing = increasing && Along(i, noise, steps) > Along(i - 1, noise, steps);
      }
      Check(increasing && Along(steps - 1, noise, steps) < 1.0f, "the steps' distances increase and end inside the ray");
      // StepLength is the derivative of the distance: the real spacing is between it and it + 1/steps^2.
      bool spacing_ok = true;
      for (uint32_t i = 0; i + 1 < steps; ++i) {
        const float real = (Along(i + 1, noise, steps) - Along(i, noise, steps)) * 1.24f;
        const float rule = StepLength(i, noise, steps, 1.24f);
        spacing_ok = spacing_ok && real >= rule - 1e-5f && real <= rule + 1.24f / float(steps * steps) + 1e-5f;
      }
      Check(spacing_ok, "StepLength is the spacing of the squared steps");
    }
  }
  // Fine at the contact: the first spacing of 16 steps over 1.24 m is at most 1.5 cm (the old uniform one was 7.75 cm).
  float finest = 0.0f, coarsest = 0.0f;
  for (float noise : {0.0f, 0.5f, 0.999f}) {
    finest = std::max(finest, (Along(1, noise, 16) - Along(0, noise, 16)) * 1.24f);
    coarsest = std::max(coarsest, (Along(16, noise, 16) - Along(15, noise, 16)) * 1.24f);
  }
  Check(finest < 0.016f, Num("the first step is %.1f cm (old 7.75)", 100.0 * finest));
  Check(coarsest < 0.17f, Num("the last step is %.1f cm", 100.0 * coarsest));
  Check(Along(8, 0.0f, 16) < 0.26f, "half of the steps cover the first quarter of the ray");

  Check(EndFade(0.0f, kDefaultFadeStart) == 1.0f && EndFade(0.6f, kDefaultFadeStart) == 1.0f, "full darkness up to the fade's start");
  Check(EndFade(1.0f, kDefaultFadeStart) == 0.0f, "no shadow at the ray's end");
  Check(std::fabs(EndFade(0.8f, kDefaultFadeStart) - 0.5f) < 1e-6f, "half way through the fade it is half");
  bool monotone = true;
  float last = 2.0f;
  for (float a = 0.0f; a <= 1.0f; a += 0.01f) {
    monotone = monotone && EndFade(a, kDefaultFadeStart) <= last + 1e-6f;
    last = EndFade(a, kDefaultFadeStart);
  }
  Check(monotone, "the end fade never increases");
  Check(ClassicEndFade(0.5f) == 0.75f && EndFade(0.5f, kDefaultFadeStart) == 1.0f,
        "half way along the ray the old fade took a quarter of the shadow, the new one nothing");
  Check(EndFade(0.7f, 5.0f) == EndFade(0.7f, kFadeStartMax), "a fade start of 1 or more is kept below 1");
}

void ToleranceScalesWithResolution() {
  const float tan_y = 0.2217f;
  const float f720 = Footprint(10.0f, tan_y, 720.0f), f2160 = Footprint(10.0f, tan_y, 2160.0f);
  Check(std::fabs(f720 - 0.00616f) < 1e-4f && std::fabs(f2160 - 0.002053f) < 1e-4f, "a pixel at 10 m is 6.2 mm at 720p and 2.1 mm at 2160p");
  const float eps720 = HitEpsilon(f720), eps2160 = HitEpsilon(f2160);
  Check(eps2160 < 0.6f * eps720, Num("the tolerance at 2160p (%.1f mm) is well under 720p's (%.1f mm)", 1000.0 * eps2160, 1000.0 * eps720));
  Check(eps2160 < ClassicBias(10.0f) / 6.0f, Num("... and under a sixth of the old 6 cm bias at 10 m (%.1f mm)", 1000.0 * eps2160));
  const float ramp2160 = HitRamp(f2160, 0.05f, 0.5f);
  Check(ramp2160 < 0.025f, Num("the ramp at 2160p at 10 m is %.1f mm (the old one 60)", 1000.0 * ramp2160));
  Check(HitRamp(f2160, 0.05f, 0.0f) < HitRamp(f2160, 0.05f, 1.0f), "the ramp grows with the depth a step changes by");
  Check(RayLift(f2160) < RayLift(f720), "the lift of the ray's start is smaller with finer pixels");
  // The tolerance in pixels (footprints) is the same at every scale, apart from the floor.
  Check(eps2160 / f2160 > 1.5f && eps720 / f720 > 1.5f && eps720 / f720 < eps2160 / f2160, "the tolerance is a few footprints");
}

void FilterWeights() {
  Check(EdgeRangeWeight(0.0f) == 1.0f, "a neighbour of the same value counts fully");
  Check(EdgeRangeWeight(0.05f) > 0.96f, "noise of a few hundredths is averaged");
  Check(EdgeRangeWeight(1.0f) < 1e-4f && EdgeRangeWeight(-1.0f) < 1e-4f, "a whole step of the range is not averaged: the edge stays");
  Check(EdgeRangeWeight(0.5f) < 0.05f, "half the range is mostly kept apart too");
}

void CostRule() {
  const Cost old4k = CostModel(3840, 2160, kStepsHigh, 2, false);
  const Cost new4k = CostModel(3840, 2160, kStepsHigh, 1, true);
  Check(std::fabs(old4k.march_ms - 0.232) < 0.002 && std::fabs(old4k.filter_ms - 0.2705) < 0.002,
        Num("the model gives back the measured 0.232 + 0.271 ms (%.3f + %.3f)", old4k.march_ms, old4k.filter_ms));
  Check(new4k.total_ms < old4k.total_ms,
        Num("the new total (%.3f ms) is under the old (%.3f ms) at 3840x2160", new4k.total_ms, old4k.total_ms));
  Check(new4k.total_ms < 0.45, "the new contact shadows cost under 0.45 ms at 3840x2160 on the RTX 5080 (model)");
  const Cost hd = CostModel(1920, 1080, kStepsHigh, 1, true);
  Check(std::fabs(hd.total_ms * 4.0 - new4k.total_ms) < 1e-9, "the cost scales with the pixels");
  const Cost low = CostModel(1280, 720, kStepsLow, 1, true);
  Check(low.total_ms < 0.05, Num("the low setting at 1280x720 is %.3f ms (model)", low.total_ms));
  Check(CostModel(3840, 2160, 8, 1, true).march_ms * 2.0 - new4k.march_ms < 1e-9, "the march scales with the steps");
}

// ---------------------------------------------------------------------------
// the march over scenes
// ---------------------------------------------------------------------------

// A plane seen at any angle, lit from any side at any elevation, never shadows itself, at both scales.
void PlanesNeverShadowThemselves() {
  struct Plane {
    const char* name;
    float incline;
    bool ground;
    std::vector<Box> boxes;
  };
  const std::vector<Plane> planes = {
      {"flat ground", 0.0f, true, {}},
      {"road climbing 0.2", 0.2f, true, {}},
      {"road falling 0.15", -0.15f, true, {}},
      {"wall facing the camera", 0.0f, false, {{-100, 100, -100, 100, 8.0f, 8.5f}}},
      {"wall seen along its length", 0.0f, false, {{3.0f, 3.5f, -100, 100, 0.0f, 100}}},
  };
  int total = 0, bad = 0, bad_classic = 0;
  for (const Plane& pl : planes) {
    for (int height : {720, 2160}) {
      Scene s;
      s.cam = MakeCamera(height);
      s.incline = pl.incline;
      s.ground = pl.ground;
      s.boxes = pl.boxes;
      auto depth = [&](int x, int y) { return s.Depth(x, y); };
      for (double elevation : {5.0, 10.0, 20.0, 35.0, 60.0, 85.0}) {
        for (double azimuth = 0.0; azimuth < 360.0; azimuth += 45.0) {
          for (uint32_t steps : {kStepsLow, kStepsHigh}) {
            for (float noise : {0.0f, 0.37f, 0.99f}) {
              for (float length : {0.6f, 1.24f, 3.7f}) {
                Trace t;
                t.camera = s.cam;
                t.to_sun = Sun(elevation, azimuth);
                t.length = length;
                t.steps = steps;
                t.noise = noise;
                Trace classic = t;
                classic.classic = true;
                for (int y = 3; y < s.cam.height - 3; y += s.cam.height / 24 + 1) {
                  for (int x = 3; x < s.cam.width - 3; x += s.cam.width / 31 + 1) {
                    ++total;
                    if (March(t, depth, x, y) < 0.999f) {
                      ++bad;
                    }
                    if (March(classic, depth, x, y) < 0.999f) {
                      ++bad_classic;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  Check(bad == 0, Num("a plane at any angle and scale never shadows itself: %.0f of %.0f samples did", double(bad), double(total)));
  Check(bad_classic == 0, "the old march did not either (the scenes are fair)");
}

// A bollard 0.3 m wide, 0.3 m deep and 0.5 m high at 10 m, the sun low on its right (the shadow falls to its left).
struct Bollard {
  Box box{-0.15f, 0.15f, -1.5f, -1.0f, 10.0f, 10.3f};
  double elevation = 15.0;
  float length = 1.5f;
};

// The lifted ray's points inside the box inset (or outset) by `margin`, counted by sampling every millimetre up to `limit` metres.
double ChordInside(Vec3 start, Vec3 sun, double limit, const Box& b, float margin) {
  double inside = 0.0;
  for (double d = 0.0; d <= limit; d += 0.001) {
    const Vec3 p = start + sun * float(d);
    if (p.x > b.x0 + margin && p.x < b.x1 - margin && p.y > b.y0 + margin && p.y < b.y1 - margin && p.z > b.z0 + margin && p.z < b.z1 - margin) {
      inside += 0.001;
    }
  }
  return inside;
}

void KerbShadowAndNoFalseShadow() {
  Bollard bo;
  int shadowed = 0, shadowed_ok = 0, open = 0, open_ok = 0, open_classic_bad = 0;
  for (int height : {720, 2160}) {
    Scene s;
    s.cam = MakeCamera(height);
    s.boxes.push_back(bo.box);
    auto depth = [&](int x, int y) { return s.Depth(x, y); };
    const Vec3 sun = Sun(bo.elevation, 90.0);
    for (float noise : {0.1f, 0.5f, 0.9f}) {
      Trace t;
      t.camera = s.cam;
      t.to_sun = sun;
      t.length = bo.length;
      t.noise = noise;
      Trace classic = t;
      classic.classic = true;
      for (int y = height / 2; y < height - 2; y += 2) {
        for (int x = s.cam.width * 4 / 10; x < s.cam.width * 6 / 10; x += 3) {
          const float tc = depth(x, y);
          if (tc > 12.0f || tc < 8.0f) {
            continue;
          }
          const Vec3 pc = ViewPos(s.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
          // The pixel must be ground (not the box): its position is on y = -1.5.
          if (std::fabs(pc.y + 1.5f) > 1e-3f) {
            continue;
          }
          const Vec3 start = pc + Vec3{0.0f, 1.0f, 0.0f} * RayLift(FootprintUnit(s.cam.tan_y, float(height)) * tc);
          // Definitely shadowed: 12 cm of the ray, within the part that stays full (60% of the length), inside the box shrunk by 2 cm.
          if (ChordInside(start, sun, 0.55 * bo.length, bo.box, 0.02f) >= 0.12) {
            ++shadowed;
            shadowed_ok += March(t, depth, x, y) < 0.5f ? 1 : 0;
          } else if (ChordInside(start, sun, bo.length, bo.box, -0.65f) == 0.0) {
            // Definitely open: the whole ray is 65 cm or more from the box (a screen-space march cannot tell a point behind
            // the box's front face from one inside it, so the slab of the thickness, 0.3 m, and its fade, 0.3 m more, are
            // not "open").
            ++open;
            open_ok += March(t, depth, x, y) > 0.999f ? 1 : 0;
            open_classic_bad += March(classic, depth, x, y) < 0.999f ? 1 : 0;
          }
        }
      }
    }
  }
  Check(shadowed > 200, Num("the bollard scene has shadowed ground pixels (%.0f)", double(shadowed)));
  Check(shadowed_ok >= shadowed * 95 / 100, Num("the ground in the bollard's shadow is dark: %.0f of %.0f", double(shadowed_ok), double(shadowed)));
  Check(open > 1000, Num("the bollard scene has open ground pixels (%.0f)", double(open)));
  Check(open_ok == open, Num("no shadow where the ray is 65 cm from the box: %.0f of %.0f open", double(open_ok), double(open)));
  (void)open_classic_bad;
}

// The shadow does not fade along the ray: the ground whose ray meets the bollard half way along it is as dark
// as the ground at its foot (the old fade 1 - along^2 gave it a quarter of the light).
void NoGradientInsideAShadow() {
  Bollard bo;
  bo.length = 1.5f;
  Scene s;
  s.cam = MakeCamera(2160);
  s.boxes.push_back(bo.box);
  auto depth = [&](int x, int y) { return s.Depth(x, y); };
  const Vec3 sun = Sun(bo.elevation, 90.0);
  Trace t;
  t.camera = s.cam;
  t.to_sun = sun;
  t.length = bo.length;
  t.noise = 0.5f;
  Trace classic = t;
  classic.classic = true;
  // Ground pixels at world x = -0.15 - dx for dx = 0.1 and 0.72: the ray meets the box after dx / cos(15 degrees) = 0.10 and 0.75 m,
  // that is half way along the 1.5 m ray in the second case.
  float near_new = 0, far_new = 0, near_old = 0, far_old = 0;
  int counted = 0;
  for (int y = 1500; y < 2100; ++y) {
    for (int x = 0; x < s.cam.width; ++x) {
      const float tc = depth(x, y);
      if (tc < 10.12f || tc > 10.2f) {
        continue;  // the strip inside the bollard's depth, clear of its edges
      }
      const Vec3 pc = ViewPos(s.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      if (std::fabs(pc.y + 1.5f) > 1e-3f) {
        continue;
      }
      const float dx = -0.15f - pc.x;  // metres to the box along x
      const float d = dx / float(std::cos(bo.elevation * 3.14159265358979323846 / 180.0));
      if (std::fabs(dx - 0.10f) < 0.004f) { near_new += March(t, depth, x, y); near_old += March(classic, depth, x, y); ++counted; }
      if (std::fabs(dx - 0.72f) < 0.004f) { far_new += March(t, depth, x, y); far_old += March(classic, depth, x, y); }
      (void)d;
    }
  }
  Check(counted > 4, "found ground pixels beside the bollard");
  Check(far_new / float(std::max(counted, 1)) < 0.1f, Num("new: the shadow 0.72 m from the bollard (half way along the ray) is as dark as at its foot (visibility %.2f)", far_new / std::max(counted, 1)));
  Check(far_old / float(std::max(counted, 1)) > 0.2f, Num("old: it was a quarter lighter there (visibility %.2f)", far_old / std::max(counted, 1)));
  Check(near_new <= near_old + 1e-3f, "and the new shadow is not lighter anywhere near the foot");
}

// The width of the shadow's near edge in pixel rows, down a column: the rows whose visibility is neither open nor dark.
int EdgeRows(bool classic, int height, int column_from, int column_to) {
  Bollard bo;
  Scene s;
  s.cam = MakeCamera(height);
  s.boxes.push_back(bo.box);
  auto depth = [&](int x, int y) { return s.Depth(x, y); };
  Trace t;
  t.camera = s.cam;
  t.to_sun = Sun(bo.elevation, 90.0);
  t.length = bo.length;
  t.noise = 0.5f;
  t.classic = classic;
  int widest = 0;
  for (int x = column_from; x < column_to; ++x) {
    int rows = 0;
    for (int y = height / 2; y < height - 2; ++y) {
      const float tc = depth(x, y);
      if (tc < 9.9f || tc > 10.15f) {
        continue;  // the near edge of the strip (the bollard's front face is at 10.0 m), not the thickness's fade behind it
      }
      const float v = March(t, depth, x, y);
      if (v > 0.1f && v < 0.9f) {
        ++rows;
      }
    }
    widest = std::max(widest, rows);
  }
  return widest;
}

void EdgeIsNarrowAtEveryScale() {
  // The columns of the shadow strip left of the bollard (world x about -0.3 to -0.9 at 10 m).
  const auto columns = [](int height) {
    const int width = height * 16 / 9;
    const double per_metre = 0.5 * width / (10.0 * 0.2217 * 16.0 / 9.0);
    return std::pair<int, int>{int(width * 0.5 - 0.9 * per_metre), int(width * 0.5 - 0.3 * per_metre)};
  };
  const auto c720 = columns(720), c2160 = columns(2160);
  const int new720 = EdgeRows(false, 720, c720.first, c720.second);
  const int new2160 = EdgeRows(false, 2160, c2160.first, c2160.second);
  const int old720 = EdgeRows(true, 720, c720.first, c720.second);
  const int old2160 = EdgeRows(true, 2160, c2160.first, c2160.second);
  std::printf("edge rows (partial visibility, widest column): new 720p %d, new 2160p %d, old 720p %d, old 2160p %d\n", new720, new2160, old720, old2160);
  Check(old2160 >= 2 * old720 || old2160 >= 4, "the old edge got wider in pixels with the render scale");
  Check(new2160 <= 3, Num("the new edge at 2160p is at most 3 pixel rows (%.0f)", double(new2160)));
  Check(new720 <= 2, Num("the new edge at 720p is at most 2 pixel rows (%.0f)", double(new720)));
  Check(new2160 * 2 <= old2160 || old2160 <= 1, "the new edge at 2160p is at most half of the old one's in pixels");
}

// ---------------------------------------------------------------------------
// the ground under a vehicle (a traffic car: no sun shadow of its own)
// ---------------------------------------------------------------------------

// A car: 1.8 m wide, 4.5 m long, 1.4 m tall with 0.25 m under it, its rear face at 8 m, in front of a camera
// `cam_height` metres above the ground looking level along +z; the world's up is then (0, 1, 0) in view space.
Scene MakeCar(int height_px, float cam_height, bool vehicle) {
  Scene s;
  s.cam = MakeCamera(height_px);
  s.ground_y = -cam_height;
  Box car{-0.9f, 0.9f, -cam_height + 0.25f, -cam_height + 1.4f, 8.0f, 12.5f};
  car.vehicle = vehicle;
  s.boxes.push_back(car);
  return s;
}

// The world position of the ground the pixel shows (the pixel's ray meets the ground plane).
Vec3 GroundPoint(const Scene& s, int x, int y) {
  const float t = s.Depth(x, y);
  return ViewPos(s.cam, float(x) + 0.5f, float(y) + 0.5f, t);
}

struct Tally {
  int count = 0;
  int dark = 0;           // visibility under 0.15
  float worst_change = 0.0f;  // the largest |new - old|
  float sum_new = 0.0f, sum_old = 0.0f;
};

// Runs the march with and without the term over every ground pixel `select` picks (on the ground, not a vehicle).
template <class Select>
Tally RunScene(const Scene& s, const Vec3& up, float strength, Select&& select, int* loads_per_frame = nullptr,
               int* pixels = nullptr, float cast_distance = 0.0f) {
  const auto depth = [&](int x, int y) { return s.DepthV(x, y); };
  Trace old_t;
  old_t.camera = s.cam;
  old_t.to_sun = Sun(60.0, 180.0);  // high, behind the camera
  old_t.length = 0.6f;
  old_t.noise = 0.5f;
  Trace new_t = old_t;
  new_t.up = up;
  new_t.under = strength;
  new_t.cast_distance = cast_distance;
  Tally tally;
  int total_loads = 0, total_pixels = 0;
  for (int y = 0; y < s.cam.height; ++y) {
    for (int x = 0; x < s.cam.width; ++x) {
      new_t.under_loads = 0;
      const float v_new = March(new_t, depth, x, y);
      total_loads += new_t.under_loads;
      ++total_pixels;
      if (!select(x, y)) {
        continue;
      }
      const float v_old = March(old_t, depth, x, y);
      ++tally.count;
      tally.dark += v_new < 0.15f ? 1 : 0;
      tally.worst_change = std::max(tally.worst_change, std::fabs(v_new - v_old));
      tally.sum_new += v_new;
      tally.sum_old += v_old;
    }
  }
  if (loads_per_frame) {
    *loads_per_frame = total_loads;
  }
  if (pixels) {
    *pixels = total_pixels;
  }
  return tally;
}

void UnderAVehicle() {
  const Vec3 up{0.0f, 1.0f, 0.0f};
  for (int height : {720, 2160}) {
    const Scene car = MakeCar(height, 1.5f, true);
    const auto under = [&](int x, int y) {
      if (DepthIsVehicle(car.DepthV(x, y))) {
        return false;
      }
      const Vec3 g = GroundPoint(car, x, y);
      return car.Depth(x, y) < 1000.0f && std::fabs(g.y + 1.5f) < 1e-2f && std::fabs(g.x) < 0.75f && g.z > 8.1f && g.z < 12.4f;
    };
    int loads = 0, pixels = 0;
    const Tally t = RunScene(car, up, 1.0f, under, &loads, &pixels);
    std::printf("%dp: ground seen under the car: %d pixels, visibility old %.2f mean, new %.2f mean, %d of them dark with the term\n",
                height, t.count, t.sum_old / float(std::max(t.count, 1)), t.sum_new / float(std::max(t.count, 1)), t.dark);
    Check(t.count > (height == 720 ? 150 : 1500), Num("the scene shows the ground under the car (%.0f pixels)", double(t.count)));
    // The bug: the contact march alone leaves most of what is under the car lit.
    const Tally off = RunScene(car, up, 0.0f, under);
    Check(off.dark * 3 < off.count, Num("without the term most of the ground under the car stays lit (%.0f%% dark): the patchy look",
                                        100.0 * off.dark / std::max(off.count, 1)));
    Check(float(t.dark) > 0.97f * float(t.count), Num("with the term every pixel under the car is dark (%.1f%%)", 100.0 * t.dark / std::max(t.count, 1)));
    Check(t.sum_new / float(t.count) < 0.05f, Num("... mean visibility %.3f", t.sum_new / float(t.count)));
    // At most three depth reads a pixel, and none for most of the picture's pixels (the sky, the car, a surface that is not horizontal).
    Check(loads <= 3 * pixels, "the term reads at most three pixels a pixel");
    Check(double(loads) < 3.0 * double(pixels) * 0.85, Num("... fewer on average (%.2f reads a pixel)", double(loads) / double(pixels)));
  }
}

void NoShadowWhereThereIsNoCar() {
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const int height = 720;
  // In front of the car, beside it, and (from a camera high enough to see it) beyond it: the term adds nothing.
  struct Region {
    const char* what;
    float cam_height;
    float x0, x1, z0, z1;
  };
  const Region regions[] = {
      {"in front of the car", 1.5f, -3.0f, 3.0f, 3.0f, 7.4f},
      {"beside the car (0.2 m off its flank and farther)", 1.5f, 1.1f, 4.0f, 8.0f, 12.5f},
      {"beside the other flank", 1.5f, -4.0f, -1.1f, 8.0f, 12.5f},
      {"beyond the car, seen from 4 m up", 4.0f, -2.0f, 2.0f, 13.2f, 20.0f},
  };
  for (const Region& r : regions) {
    const Scene s = MakeCar(height, r.cam_height, true);
    const auto select = [&](int x, int y) {
      if (DepthIsVehicle(s.DepthV(x, y))) {
        return false;
      }
      const float d = s.Depth(x, y);
      if (d > 1000.0f) {
        return false;
      }
      const Vec3 g = ViewPos(s.cam, float(x) + 0.5f, float(y) + 0.5f, d);
      return std::fabs(g.y + r.cam_height) < 1e-2f && g.x >= r.x0 && g.x <= r.x1 && g.z >= r.z0 && g.z <= r.z1;
    };
    const Tally t = RunScene(s, up, 1.0f, select);
    Check(t.count > 100, std::string("the scene shows ground ") + r.what);
    Check(t.worst_change < 1e-6f, std::string("no shadow added ") + r.what + Num(" (largest change %.4f)", t.worst_change));
  }
}

void VehicleBitAndSwitches() {
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const int height = 720;
  const Scene car = MakeCar(height, 1.5f, true);
  const auto all = [](int, int) { return true; };
  // Off: strength 0, or no up: the picture is the old march's, pixel for pixel.
  Check(RunScene(car, up, 0.0f, all).worst_change < 1e-6f, "a strength of 0 gives the old march's picture");
  Check(RunScene(car, Vec3{}, 1.0f, all).worst_change < 1e-6f, "an unknown up gives the old march's picture");
  // The same shape that the depth pass did not mark as a vehicle (a table, a shelter): nothing changes.
  const Scene table = MakeCar(height, 1.5f, false);
  Check(RunScene(table, up, 1.0f, all).worst_change < 1e-6f, "a box that is not a vehicle gets no term (the contact march's own job)");
  // The vehicle's own pixels never change.
  const auto vehicle_px = [&](int x, int y) { return DepthIsVehicle(car.DepthV(x, y)); };
  const Tally own = RunScene(car, up, 1.0f, vehicle_px);
  Check(own.count > 1000 && own.worst_change < 1e-6f, "the car's own pixels are not touched");
  // A surface that is not horizontal (the car's rear face seen from a pixel-neighbour's normal is vehicle anyway): the ground
  // tilted by 40 degrees to the up has no term.
  Check(UnderUpness(0.5f) == 0.0f && UnderUpness(0.74f) == 0.0f && UnderUpness(1.0f) == 1.0f && UnderUpness(0.9f) == 1.0f,
        "a surface within about 40 degrees of horizontal is ground, one steeper than 41 is not");
  Check(UnderUpness(0.82f) > 0.0f && UnderUpness(0.82f) < 1.0f, "in between it fades");
}

void HitWeightRule() {
  const float fp = Footprint(10.0f, 0.2217f, 2160.0f);
  Check(UnderHitWeight(-1.0f, fp) == 0.0f && UnderHitWeight(0.0f, fp) == 0.0f, "a step in front of the shell or on it is no hit");
  Check(UnderHitWeight(UnderTolerance(fp) * 0.5f, fp) == 0.0f, "not within the tolerance");
  Check(UnderHitWeight(UnderTolerance(fp) * 2.0f, fp) == 1.0f, "fully at twice the tolerance");
  Check(UnderHitWeight(4.5f, fp) == 1.0f && UnderHitWeight(kUnderReach, fp) == 1.0f, "a car's length behind the shell is inside it");
  Check(UnderHitWeight(kUnderReach * 1.25f, fp) == 0.0f && UnderHitWeight(kUnderReach * 1.1f, fp) > 0.0f, "past the reach it fades out");
  float last = 2.0f;
  bool monotone = true;
  for (float dz = kUnderReach; dz <= kUnderReach * 1.3f; dz += 0.05f) {
    monotone = monotone && UnderHitWeight(dz, fp) <= last + 1e-6f;
    last = UnderHitWeight(dz, fp);
  }
  Check(monotone, "the weight never grows with the distance behind the shell");
  // The tolerance is in footprints, so 4K and 720p agree to within the floor.
  const float fp720 = Footprint(10.0f, 0.2217f, 720.0f);
  Check(UnderTolerance(fp) < UnderTolerance(fp720) && UnderTolerance(fp720) < 0.12f, Num("the tolerance at 10 m is %.1f cm at 720p", 100.0 * UnderTolerance(fp720)));
  // The vehicle bit round trip and its size: one unit in the last place.
  bool bit_ok = true;
  for (float d : {0.5f, 3.7f, 12.0f, 80.0f, 1234.5f}) {
    const float on = DepthWithVehicleBit(d, true), off = DepthWithVehicleBit(d, false);
    bit_ok = bit_ok && DepthIsVehicle(on) && !DepthIsVehicle(off) && std::fabs(on - d) < d * 2.4e-7f && std::fabs(off - d) < d * 2.4e-7f;
  }
  Check(bit_ok, "the vehicle bit is the lowest bit of the depth and changes it by one unit in the last place");
}

// A car the game draws into the sun shadow map (TRAFFIC SHADOWS) has its own shadow: the term leaves it out, by
// the distance of the car's shell from the camera, and keeps the cars beyond it (traffic_shadow.h).
void CastDistanceRule() {
  // The weight: 1 with no cast distance, 0 well inside it, 1 well beyond, half in the middle, never falling with distance.
  Check(UnderCastWeight(5.0f, 0.0f) == 1.0f && UnderCastWeight(500.0f, 0.0f) == 1.0f && UnderCastWeight(5.0f, -3.0f) == 1.0f,
        "no cast distance: every car keeps the term");
  Check(UnderCastWeight(10.0f, 40.0f) == 0.0f && UnderCastWeight(40.0f - kUnderCastBand, 40.0f) == 0.0f,
        "a car well within the cast distance gets none of the term");
  Check(UnderCastWeight(40.0f + kUnderCastBand, 40.0f) == 1.0f && UnderCastWeight(120.0f, 40.0f) == 1.0f,
        "a car beyond it keeps all of it");
  Check(std::fabs(UnderCastWeight(40.0f, 40.0f) - 0.5f) < 1e-6f, "half at the cast distance itself");
  float last = -1.0f;
  bool monotone = true;
  for (float d = 0.0f; d <= 60.0f; d += 0.25f) {
    const float w = UnderCastWeight(d, 40.0f);
    monotone = monotone && w >= last - 1e-6f && w >= 0.0f && w <= 1.0f;
    last = w;
  }
  Check(monotone, "the weight never falls with the shell's distance and stays in 0 to 1");
  // A cast distance nearer than the band's half width still works (a car at 2 m).
  Check(UnderCastWeight(0.5f, 2.0f) < 1.0f && UnderCastWeight(5.0f, 2.0f) == 1.0f, "a cast distance below the band's width");

  // On the car scene (the car's rear face at 8 m, its far end at 12.5 m, the camera on its axis). The ground under it is
  // dark with the term and stays lit without it (the bug of the patchy look, see UnderAVehicle).
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const int height = 720;
  const Scene car = MakeCar(height, 1.5f, true);
  const auto under = [&](int x, int y) {
    if (DepthIsVehicle(car.DepthV(x, y))) {
      return false;
    }
    const Vec3 g = GroundPoint(car, x, y);
    return car.Depth(x, y) < 1000.0f && std::fabs(g.y + 1.5f) < 1e-2f && std::fabs(g.x) < 0.75f && g.z > 8.1f && g.z < 12.4f;
  };
  int loads = 0, pixels = 0;
  const Tally on = RunScene(car, up, 1.0f, under, &loads, &pixels, 0.0f);
  const Tally near_cast = RunScene(car, up, 1.0f, under, &loads, &pixels, 30.0f);  // the car casts: 8 to 12.5 m, well inside 27 m
  const Tally far_cast = RunScene(car, up, 1.0f, under, &loads, &pixels, 4.0f);    // the car is beyond 4 + 3 m: it keeps the term
  Check(on.count > 150 && float(on.dark) > 0.97f * float(on.count), "the car's ground is dark with the term (no cast distance)");
  Check(near_cast.worst_change < 1e-6f, Num("a car within the cast distance: the term adds nothing (largest change %.4f), the game's shadow does the job",
                                              near_cast.worst_change));
  Check(far_cast.dark == on.dark && std::fabs(far_cast.sum_new - on.sum_new) < 1e-3f,
        "a car beyond the cast distance (and its band): the term is exactly as without a cast distance");
  // In the band the term is partly there: between the two (the shell is 8 to 12.5 m; a cast distance of 10.5 puts it astride).
  const Tally band = RunScene(car, up, 1.0f, under, &loads, &pixels, 10.5f);
  Check(band.sum_new >= far_cast.sum_new - 1e-4f && band.sum_new <= near_cast.sum_new + 1e-4f &&
            band.sum_new > far_cast.sum_new + 1.0f && band.sum_new < near_cast.sum_new - 1.0f,
        "in the band the visibility is strictly between the two (the car's shell is astride the cast distance)");
  // The same through the table: a non-vehicle (a table) is not touched by any cast distance.
  const Scene table = MakeCar(height, 1.5f, false);
  Check(RunScene(table, up, 1.0f, [](int, int) { return true; }, nullptr, nullptr, 30.0f).worst_change < 1e-6f,
        "a box that is not a vehicle is not touched, cast distance or not");
}

void UpFromTheView() {
  struct M {
    double v[4][4];
    const double* operator[](int i) const { return v[i]; }
  };
  const auto length = [](Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); };
  M identity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
  const Vec3 u0 = UpInView(identity);
  Check(std::fabs(u0.x) < 1e-6f && std::fabs(u0.y - 1.0f) < 1e-6f && std::fabs(u0.z) < 1e-6f, "a level camera: up is up");
  // Looking down by 30 degrees: the world's Y in the game's view space (z backward) is (0, cos, sin), in ours (z forward) (0, cos, -sin):
  // up points up and toward the camera.
  const double a = 30.0 * 3.14159265358979323846 / 180.0;
  M pitched = {{{1, 0, 0, 0}, {0, std::cos(a), std::sin(a), 0}, {0, -std::sin(a), std::cos(a), 0}, {0, 0, 0, 1}}};
  const Vec3 u1 = UpInView(pitched);
  Check(std::fabs(u1.y - 0.866f) < 1e-3f && std::fabs(u1.z + 0.5f) < 1e-3f && std::fabs(length(u1) - 1.0f) < 1e-5f,
        Num("a camera looking down 30 degrees: up is (0, %.3f, %.3f)", u1.y, u1.z));
  // A scaled row (a matrix with a scale): normalised. A rolled camera: up leans sideways.
  M scaled = {{{2, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 2, 0}, {0, 0, 0, 1}}};
  Check(std::fabs(length(UpInView(scaled)) - 1.0f) < 1e-6f, "the vector is a unit vector whatever the matrix's scale");
  M rolled = {{{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
  const Vec3 u2 = UpInView(rolled);
  Check(std::fabs(u2.x - 1.0f) < 1e-6f && std::fabs(u2.y) < 1e-6f, "a camera rolled a quarter turn: up is to the right");
  M zero = {{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}}};
  const Vec3 u3 = UpInView(zero);
  Check(u3.x == 0.0f && u3.y == 0.0f && u3.z == 0.0f, "an unusable matrix gives no vector (the term is then off)");
  M broken = {{{1, 0, 0, 0}, {std::nan(""), 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
  const Vec3 u4 = UpInView(broken);
  Check(u4.x == 0.0f && u4.y == 0.0f && u4.z == 0.0f, "a NaN gives no vector either");
}

// The shaders repeat the header's constants as `static const float kName = value;`: read them and compare.
bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}
bool ShaderConstant(const std::string& text, const std::string& name, double* value) {
  const std::string key = "static const float " + name + " = ";
  const size_t at = text.find(key);
  if (at == std::string::npos) {
    return false;
  }
  *value = std::strtod(text.c_str() + at + key.size(), nullptr);
  return true;
}
void ShaderConstantsMatch(const std::string& dir) {
  std::string contact, common;
  if (!ReadFile(dir + "/ao_contact_ps.hlsl", &contact) || !ReadFile(dir + "/ao_common.hlsli", &common)) {
    Check(false, "could not read the shaders in " + dir);
    return;
  }
  struct Row {
    const std::string* text;
    const char* name;
    double want;
  };
  const Row rows[] = {
      {&contact, "kEpsFloor", kEpsFloor},           {&contact, "kEpsPixels", kEpsPixels},
      {&contact, "kRampFloor", kRampFloor},         {&contact, "kRampPixels", kRampPixels},
      {&contact, "kRampStepShare", kRampStepShare}, {&contact, "kLiftFloor", kLiftFloor},
      {&contact, "kLiftPixels", kLiftPixels},       {&contact, "kSunFacingLow", kSunFacingLow},
      {&contact, "kSunFacingHigh", kSunFacingHigh}, {&contact, "kFadeStartMax", kFadeStartMax},
      {&contact, "kNearZ", kNearZ},                 {&common, "kEdgeRangeSigma", kEdgeRangeSigma},
      {&common, "kEdgeSpatialSigma", kEdgeSpatialSigma}, {&common, "kEdgePlanePixels", kEdgePlanePixels},
      {&common, "kEdgePlaneFloor", kEdgePlaneFloor},
      {&contact, "kUnderHeight", kUnderHeight},     {&contact, "kUnderStep0", kUnderStep0},
      {&contact, "kUnderStep1", kUnderStep1},       {&contact, "kUnderStep2", kUnderStep2},
      {&contact, "kUnderJitterLow", kUnderJitterLow}, {&contact, "kUnderJitterSpan", kUnderJitterSpan},
      {&contact, "kUnderReach", kUnderReach},       {&contact, "kUnderEpsFloor", kUnderEpsFloor},
      {&contact, "kUnderEpsPixels", kUnderEpsPixels}, {&contact, "kUnderUpLow", kUnderUpLow},
      {&contact, "kUnderUpHigh", kUnderUpHigh},     {&contact, "kUnderCastBand", kUnderCastBand},
  };
  for (const Row& r : rows) {
    double got = 0.0;
    const bool found = ShaderConstant(*r.text, r.name, &got);
    Check(found && std::fabs(got - r.want) < 1e-9 * std::max(1.0, std::fabs(r.want)) + 1e-7,
          std::string("the shader's ") + r.name + " is the header's" + (found ? "" : " (not found)"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  RayLengthRule();
  StepsAndFade();
  ToleranceScalesWithResolution();
  FilterWeights();
  CostRule();
  PlanesNeverShadowThemselves();
  KerbShadowAndNoFalseShadow();
  NoGradientInsideAShadow();
  EdgeIsNarrowAtEveryScale();
  UnderAVehicle();
  CastDistanceRule();
  NoShadowWhereThereIsNoCar();
  VehicleBitAndSwitches();
  HitWeightRule();
  UpFromTheView();
  if (argc > 1) {
    ShaderConstantsMatch(argv[1]);
  } else {
    std::printf("(no hlsl folder given: the shaders' constants were not compared)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
