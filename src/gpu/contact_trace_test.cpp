// Check of the contact shadows' trace rules (contact_trace.h) and of its C++ twin of the shader's march. A
// program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_contact_trace_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_contact_trace_test [<src/gpu/hlsl dir>]
//
// With the hlsl folder as its argument it also checks that the shader's constants are the header's (the shader repeats them).

#include <bit>
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
  for (uint32_t steps : {kStepsHigh, kStepsUltra}) {
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
  const Cost old4k = CostModel(3840, 2160, kStepsUltra, 2, false);
  const Cost new4k = CostModel(3840, 2160, kStepsUltra, 1, true);
  Check(std::fabs(old4k.march_ms - 0.232) < 0.002 && std::fabs(old4k.filter_ms - 0.2705) < 0.002,
        Num("the model gives back the measured 0.232 + 0.271 ms (%.3f + %.3f)", old4k.march_ms, old4k.filter_ms));
  Check(new4k.total_ms < old4k.total_ms,
        Num("the new total (%.3f ms) is under the old (%.3f ms) at 3840x2160", new4k.total_ms, old4k.total_ms));
  Check(new4k.total_ms < 0.45, "the new contact shadows cost under 0.45 ms at 3840x2160 on the RTX 5080 (model)");
  const Cost hd = CostModel(1920, 1080, kStepsUltra, 1, true);
  Check(std::fabs(hd.total_ms * 4.0 - new4k.total_ms) < 1e-9, "the cost scales with the pixels");
  const Cost high_setting = CostModel(1280, 720, kStepsHigh, 1, true);
  Check(high_setting.total_ms < 0.05, Num("the high setting at 1280x720 is %.3f ms (model)", high_setting.total_ms));
  Check(CostModel(3840, 2160, 8, 1, true).march_ms * 2.0 - new4k.march_ms < 1e-9, "the march scales with the steps");

  // The cheaper contact shadows (mcla_gpu_contact_shadows_fast): the adaptive steps and the filter inside the apply.
  const Cost fast4k = CostModelFast(3840, 2160, kStepsUltra, true);
  const Cost fast1080 = CostModelFast(1920, 1080, kStepsUltra, true);
  const Cost plain1080 = CostModel(1920, 1080, kStepsUltra, 1, true);
  std::printf("cost model (RTX 5080, not measured): 3840x2160 plain %.3f ms (march %.3f + filter %.3f), fast %.3f ms (march %.3f + filter %.3f); "
              "1920x1080 plain %.3f ms, fast %.3f ms\n",
              new4k.total_ms, new4k.march_ms, new4k.filter_ms, fast4k.total_ms, fast4k.march_ms, fast4k.filter_ms, plain1080.total_ms,
              fast1080.total_ms);
  Check(fast4k.total_ms < new4k.total_ms && fast1080.total_ms < plain1080.total_ms, "the cheaper path costs less than the plain one in the model");
  Check(fast4k.filter_ms < 0.2 * new4k.filter_ms && fast4k.filter_ms > 0.08 * new4k.filter_ms, "the filter inside the apply costs about 12% of its own pass (PLAY4: the apply rose 0.013 ms, the pass was 0.107)");
  Check(fast4k.march_ms <= new4k.march_ms && fast4k.march_ms > 0.95 * new4k.march_ms,
        "at 3840x2160 the adaptive march keeps nearly all its steps (rays are long on the screen)");
  Check(CostModelFast(1280, 720, kStepsUltra, true).march_ms / CostModel(1280, 720, kStepsUltra, 1, true).march_ms < 0.97,
        "at 1280x720 the adaptive march is cheaper in the model");
  Check(CostModelFast(1920, 1080, kStepsUltra, false).filter_ms == plain1080.filter_ms, "not folded (half size): one filter pass as before");
  Check(std::fabs(fast1080.total_ms * 4.0 - fast4k.total_ms) < 0.05 * fast4k.total_ms, "the cheaper path's cost scales with the pixels (the share of steps moves it a little)");
  Check(AdaptiveStepShare(500) == kAdaptShare720 && AdaptiveStepShare(4000) == 1.0 && AdaptiveStepShare(kAdaptMaxHeight + 1) == 1.0 &&
            AdaptiveStepShare(900) > kAdaptShare720 && AdaptiveStepShare(900) < kAdaptShare1080 && AdaptiveStepShare(kAdaptMaxHeight) == kAdaptShare1080,
        "the model's step share is flat below 720 lines, in between up to 1080, and 1 (the count is not used) above");
  Check(AdaptiveOnAt(720) && AdaptiveOnAt(1080) && !AdaptiveOnAt(1081) && !AdaptiveOnAt(1440) && !AdaptiveOnAt(2160),
        "the adaptive count is used up to 1080 lines of picture and not above");
}

// ---------------------------------------------------------------------------
// the adaptive step count: the rule
// ---------------------------------------------------------------------------

void AdaptiveStepRule() {
  Check(AdaptiveSteps(100.0f, 16, 0.0f) == 16 && AdaptiveSteps(2.0f, 16, 0.0f) == 16, "an end gap of 0 (the lever is off): every step");
  Check(AdaptiveSteps(-1.0f, 16, kAdaptEndPixels) == 16 && AdaptiveSteps(std::nanf(""), 16, kAdaptEndPixels) == 16,
        "a length that is not known (a ray through the near plane, NaN): every step");
  Check(AdaptiveSteps(0.0f, 16, kAdaptEndPixels) == kAdaptMinSteps && AdaptiveSteps(3.0f, 16, kAdaptEndPixels) == kAdaptMinSteps,
        "a ray of a few pixels takes the floor");
  Check(AdaptiveSteps(32.0f, 16, 8.0f) == 8 && AdaptiveSteps(64.0f, 16, 8.0f) == 16 && AdaptiveSteps(1000.0f, 16, 8.0f) == 16,
        "a 32 pixel ray takes 8 steps (a gap of 8 pixels at the end), a 64 pixel one or longer all 16");
  Check(AdaptiveSteps(64.0f, 8, 8.0f) == 8 && AdaptiveSteps(20.0f, 8, 8.0f) == 5 && AdaptiveSteps(10.0f, 3, 8.0f) == 3,
        "never more than the setting's steps (the high setting's 8), and a setting below the floor keeps its own");
  bool ok = true;
  uint32_t last = 0;
  for (float length = 0.0f; length <= 200.0f; length += 0.25f) {
    const uint32_t n = AdaptiveSteps(length, kStepsUltra, kAdaptEndPixels);
    ok = ok && n >= last && n >= kAdaptMinSteps && n <= kStepsUltra;
    // The gap between the last two steps, 2 L / n pixels, is the end gap or less unless every step is already taken.
    ok = ok && (n == kStepsUltra || 2.0f * length / float(n) <= kAdaptEndPixels + 1e-4f);
    last = n;
  }
  Check(ok, "the count grows with the ray's length, stays within the floor and the setting's, and keeps the end gap");

  // The ray's length on the screen: 1 m across the view at 10 m is 720 / (2 tan 10) pixels at 720p.
  const Camera c720 = MakeCamera(720), c2160 = MakeCamera(2160);
  const float across720 = RayPixelLength(c720, {0.0f, 0.0f, 10.0f}, {1.0f, 0.0f, 10.0f});
  const float across2160 = RayPixelLength(c2160, {0.0f, 0.0f, 10.0f}, {1.0f, 0.0f, 10.0f});
  const float per_metre = 0.5f * 720.0f / (10.0f * c720.tan_y);
  Check(std::fabs(across720 - per_metre) < 0.01f * per_metre, Num("1 m across the view at 10 m is %.1f pixels at 720p (%.1f)", across720, per_metre));
  Check(std::fabs(across2160 - 3.0f * across720) < 0.01f * across2160, "and three times that at 2160p");
  Check(RayPixelLength(c720, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.04f}) < 0.0f && RayPixelLength(c720, {0.0f, 0.0f, 0.01f}, {1.0f, 0.0f, 10.0f}) < 0.0f,
        "a ray that reaches the near plane has no length (every step)");
  // The same world ray covers fewer pixels and gets fewer steps the farther it is: never more.
  uint32_t previous = 1000;
  bool far_ok = true;
  for (float z = 3.0f; z <= 80.0f; z += 1.0f) {
    const float len = RayPixelLength(c720, {0.0f, -1.5f, z}, {0.5f, -1.5f + 0.5f, z});
    const uint32_t n = AdaptiveSteps(len, kStepsUltra, kAdaptEndPixels);
    far_ok = far_ok && n <= previous;
    previous = n;
  }
  Check(far_ok && previous < kStepsUltra, "a 0.7 m ray takes fewer steps the farther it is (720p) and fewer than 16 at 80 m");
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
  int total = 0, bad = 0, bad_classic = 0, bad_adaptive = 0;
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
          for (uint32_t steps : {kStepsHigh, kStepsUltra}) {
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
                Trace adaptive = t;  // the cheaper path's march (mcla_gpu_contact_shadows_fast)
                adaptive.adapt_pixels = kAdaptEndPixels;
                for (int y = 3; y < s.cam.height - 3; y += s.cam.height / 24 + 1) {
                  for (int x = 3; x < s.cam.width - 3; x += s.cam.width / 31 + 1) {
                    ++total;
                    if (March(t, depth, x, y) < 0.999f) {
                      ++bad;
                    }
                    if (March(classic, depth, x, y) < 0.999f) {
                      ++bad_classic;
                    }
                    if (March(adaptive, depth, x, y) < 0.999f) {
                      ++bad_adaptive;
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
  Check(bad_adaptive == 0, Num("nor does the adaptive march (fewer steps keep the lift and the tolerance): %.0f of %.0f samples did",
                               double(bad_adaptive), double(total)));
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

// ---------------------------------------------------------------------------
// the cheaper contact shadows: the adaptive march against the full one, and the filter inside the apply
// ---------------------------------------------------------------------------

// The shader's start offset noise for a pixel (ao_common.hlsli AoNoise).
float PixelNoise(int x, int y) {
  const float d = float(x) * 0.06711056f + float(y) * 0.00583715f;
  const float f = d - std::floor(d);
  const float g = 52.9829189f * f;
  return g - std::floor(g);
}

// A scene's linear depth image, rendered once (the march reads it many times a pixel).
struct DepthImage {
  Camera cam;
  std::vector<float> d;
  float At(int x, int y) const { return d[size_t(y) * size_t(cam.width) + size_t(x)]; }
};
DepthImage RenderDepth(const Scene& s) {
  DepthImage im;
  im.cam = s.cam;
  im.d.resize(size_t(s.cam.width) * size_t(s.cam.height));
  for (int y = 0; y < s.cam.height; ++y) {
    for (int x = 0; x < s.cam.width; ++x) {
      im.d[size_t(y) * size_t(s.cam.width) + size_t(x)] = s.Depth(x, y);
    }
  }
  return im;
}

struct NamedScene {
  const char* name;
  bool wide;  // also run at 3840x2160 (its depth image is 8 million pixels)
  std::vector<Box> boxes;
};
// The scenes, a camera 1.5 m above the ground looking along +z (the ground is y = -1.5): a bollard grid (0.3 x 0.3 x 0.5 m
// from 5 to 70 m), a 15 cm kerb with a wall behind it and a wall on the other side, a row of cars (non-vehicle boxes) on
// both sides from 6 to 77 m, and a street with all of them.
std::vector<NamedScene> AdaptScenes() {
  std::vector<NamedScene> scenes;
  scenes.push_back({"ground", false, {}});
  {
    NamedScene s{"bollards", true, {}};
    for (float z : {5.0f, 8.0f, 12.0f, 18.0f, 25.0f, 35.0f, 50.0f, 70.0f}) {
      for (float x : {-3.2f, -1.2f, 0.4f, 2.5f}) {
        s.boxes.push_back({x, x + 0.3f, -1.5f, -1.0f, z, z + 0.3f});
      }
    }
    scenes.push_back(s);
  }
  {
    NamedScene s{"kerb and walls", false, {}};
    s.boxes.push_back({3.0f, 12.0f, -1.5f, -1.35f, 0.0f, 100.0f});
    s.boxes.push_back({12.0f, 12.5f, -1.5f, 4.0f, 0.0f, 100.0f});
    s.boxes.push_back({-9.0f, -8.5f, -1.5f, 4.0f, 0.0f, 100.0f});
    s.boxes.push_back({-8.5f, 12.0f, -1.5f, 6.0f, 95.0f, 96.0f});
    scenes.push_back(s);
  }
  {
    NamedScene s{"cars", false, {}};
    for (float z : {6.0f, 14.0f, 22.0f, 31.0f, 41.0f, 55.0f, 70.0f}) {
      s.boxes.push_back({-3.4f, -1.6f, -1.25f, -0.1f, z, z + 4.5f});
      s.boxes.push_back({1.8f, 3.6f, -1.25f, -0.1f, z + 2.0f, z + 6.5f});
    }
    scenes.push_back(s);
  }
  {
    NamedScene s{"street", true, {}};
    s.boxes.push_back({4.0f, 9.0f, -1.5f, -1.35f, 0.0f, 100.0f});
    s.boxes.push_back({9.0f, 9.5f, -1.5f, 5.0f, 0.0f, 100.0f});
    s.boxes.push_back({-9.0f, -8.5f, -1.5f, 5.0f, 0.0f, 100.0f});
    for (float z : {7.0f, 16.0f, 27.0f, 40.0f, 60.0f}) {
      s.boxes.push_back({-3.4f, -1.6f, -1.25f, -0.1f, z, z + 4.5f});
      s.boxes.push_back({1.2f, 3.0f, -1.25f, -0.1f, z + 3.0f, z + 7.5f});
    }
    for (float z : {5.0f, 11.0f, 20.0f, 33.0f, 50.0f}) {
      s.boxes.push_back({4.2f, 4.5f, -1.5f, -0.9f, z, z + 0.3f});
    }
    s.boxes.push_back({-8.5f, 9.0f, -1.5f, 8.0f, 95.0f, 96.0f});
    scenes.push_back(s);
  }
  return scenes;
}

struct AdaptTally {
  double marched = 0.0;      // pixels that run the loop (not sky, not turned from the sun, within 80 m)
  double steps = 0.0;        // the adaptive march's steps over them
  double sad = 0.0;          // sum of |adaptive - full|
  double over = 0.0;         // |adaptive - full| > 0.2
  double unexplained = 0.0;  // ... and the full march has no value within 0.1 of the adaptive one in the 5 x 5 round the pixel
  double min_used = 1e9, max_used = 0.0;
  double visibility_min = 1.0;  // the lowest value either march gave (a plane must stay open)
};

// Both marches over the image's pixels (every `stride`-th, jittered), the sun at four heights and six directions
// (the ray 0.6 m times LengthGrow: 1.8, 1.24, 0.6, 0.6 m), the start offset noise of the shader's per-pixel pattern.
void CompareAdaptive(const DepthImage& im, int stride, AdaptTally* t) {
  const auto depth = [&](int x, int y) { return im.At(x, y); };
  const Camera& cam = im.cam;
  for (double elevation : {10.0, 18.7, 35.0, 65.0}) {
    for (double azimuth : {0.0, 60.0, 120.0, 180.0, 240.0, 300.0}) {
      Trace full;
      full.camera = cam;
      full.to_sun = Sun(elevation, azimuth);
      full.length = float(0.6 * LengthGrow(elevation));
      full.steps = kStepsUltra;
      Trace adaptive = full;
      adaptive.adapt_pixels = kAdaptEndPixels;
      for (int y = 1; y < cam.height - 1; y += stride) {
        for (int x = 1 + (y * 7) % stride; x < cam.width - 1; x += stride) {
          full.noise = adaptive.noise = PixelNoise(x, y);
          const float a = March(full, depth, x, y);
          if (full.steps_used == 0) {
            continue;
          }
          const float b = March(adaptive, depth, x, y);
          t->marched += 1.0;
          t->steps += adaptive.steps_used;
          t->min_used = std::min(t->min_used, double(adaptive.steps_used));
          t->max_used = std::max(t->max_used, double(adaptive.steps_used));
          t->visibility_min = std::min(t->visibility_min, double(std::min(a, b)));
          const double difference = std::fabs(double(a) - double(b));
          t->sad += difference;
          if (difference > 0.2) {
            t->over += 1.0;
            // A shadow edge that moved by 1 or 2 pixels: the full march has the adaptive value near this pixel.
            bool explained = false;
            Trace neighbour = full;
            for (int oy = -2; oy <= 2 && !explained; ++oy) {
              for (int ox = -2; ox <= 2 && !explained; ++ox) {
                const int qx = std::clamp(x + ox, 1, cam.width - 2), qy = std::clamp(y + oy, 1, cam.height - 2);
                neighbour.noise = PixelNoise(qx, qy);
                explained = std::fabs(March(neighbour, depth, qx, qy) - b) < 0.1f;
              }
            }
            t->unexplained += explained ? 0.0 : 1.0;
          }
        }
      }
    }
  }
}

// The adaptive march's tolerance, stated: over every scene and size below (a plane at every angle is the check
// PlanesNeverShadowThemselves), the contact visibility of the cheaper march differs from the full march's
//   by 0.002 or less on average (observed up to 0.0008),
//   by more than 0.2 in at most 0.3% of the pixels that march (observed up to 0.12%: the near edges of shadows move by a
//   fraction of a step, and the shader's per-pixel noise lands a thin shadow on other pixels), and
//   by more than 0.2 in at most 0.03% of them without the full march having that value within 2 pixels (observed up to
//   0.016%: one pixel in six thousand at 720p, none visible, and the filter's range weight keeps such a pixel alone).
void AdaptiveStepsMatchTheFullMarch() {
  constexpr double kMeanTolerance = 0.002;
  constexpr double kOverTolerance = 0.003;
  constexpr double kUnexplainedTolerance = 0.0003;
  const std::vector<NamedScene> scenes = AdaptScenes();
  struct Size {
    int height;
    int stride;
  };
  const Size sizes[] = {{720, 7}, {1080, 9}, {2160, 13}, {1440, 11}};
  double street_share[4] = {};
  double all_steps[4] = {}, all_marched[4] = {};
  for (size_t si = 0; si < 4; ++si) {
    for (const NamedScene& ns : scenes) {
      if (sizes[si].height >= 1440 && !ns.wide) {
        continue;
      }
      Scene s;
      s.cam = MakeCamera(sizes[si].height);
      s.boxes = ns.boxes;
      const DepthImage im = RenderDepth(s);
      AdaptTally t;
      CompareAdaptive(im, sizes[si].stride, &t);
      const double n = std::max(t.marched, 1.0);
      std::printf("  adaptive march, %4dp %-14s %9.0f pixels, %5.2f of 16 steps, mean |change| %.5f, over 0.2 in %.4f%%, not an edge move %.4f%%\n",
                  sizes[si].height, ns.name, t.marched, t.steps / n, t.sad / n, 100.0 * t.over / n, 100.0 * t.unexplained / n);
      const std::string row = std::string(ns.name) + Num(" at %.0fp", double(sizes[si].height));
      Check(t.marched > 50000.0, row + " has pixels to march");
      Check(t.sad / n <= kMeanTolerance, row + Num(": mean |change| %.5f within 0.002", t.sad / n));
      Check(t.over / n <= kOverTolerance, row + Num(": %.4f%% of the pixels change by more than 0.2 (within 0.3%%)", 100.0 * t.over / n));
      Check(t.unexplained / n <= kUnexplainedTolerance,
            row + Num(": %.4f%% change by more than 0.2 without an edge moving (within 0.03%%)", 100.0 * t.unexplained / n));
      Check(t.min_used >= double(kAdaptMinSteps) && t.max_used <= double(kStepsUltra), row + ": the step count stays between the floor and the setting's");
      if (std::string(ns.name) == "ground") {
        Check(t.visibility_min > 0.999 && t.sad == 0.0, row + ": the open ground stays open and the two marches agree exactly");
      }
      all_steps[si] += t.steps;
      all_marched[si] += t.marched;
      if (std::string(ns.name) == "street") {
        street_share[si] = t.steps / n / double(kStepsUltra);
      }
    }
  }
  std::printf("adaptive march: average steps of 16 over all the scenes: 720p %.2f, 1080p %.2f, 2160p %.2f (street alone: %.2f, %.2f, %.2f)\n",
              all_steps[0] / all_marched[0], all_steps[1] / all_marched[1], all_steps[2] / all_marched[2],
              street_share[0] * 16.0, street_share[1] * 16.0, street_share[2] * 16.0);
  // The cost model's shares are this street's average.
  Check(std::fabs(street_share[0] - kAdaptShare720) < 0.015 && std::fabs(street_share[1] - kAdaptShare1080) < 0.015 &&
            std::fabs(street_share[2] - kAdaptShare2160) < 0.015,
        Num("the model's step shares (%.2f, %.2f, %.2f) are the street's average", kAdaptShare720, kAdaptShare1080, kAdaptShare2160));
  Check(all_steps[2] / all_marched[2] > 15.5, "at 2160p the rays are long on the screen: nearly all 16 steps stay");
  // Why the count is used up to 1080 lines and not above (kAdaptMaxHeight): its 38 instructions against a step's 79 want more than half a step
  // saved on average.
  std::printf("adaptive march: steps saved of 16 (all scenes): 720p %.2f, 1080p %.2f, 1440p %.2f, 2160p %.2f; the count costs %.2f of a step\n",
              16.0 - all_steps[0] / all_marched[0], 16.0 - all_steps[1] / all_marched[1], 16.0 - all_steps[3] / all_marched[3],
              16.0 - all_steps[2] / all_marched[2], kInstrAdaptive / kInstrStep);
  Check(16.0 - all_steps[1] / all_marched[1] > kInstrAdaptive / kInstrStep * 2.0 && 16.0 - all_steps[3] / all_marched[3] < kInstrAdaptive / kInstrStep * 1.5 &&
            16.0 - all_steps[2] / all_marched[2] < kInstrAdaptive / kInstrStep,
        "the count saves over twice what it costs up to 1080 lines and about what it costs (1.5 times at most) from 1440 up, less at 2160: kAdaptMaxHeight");

  // With the lever off (an end gap of 0) the march is the full march, step for step.
  Scene s;
  s.cam = MakeCamera(720);
  s.boxes = scenes.back().boxes;
  const DepthImage im = RenderDepth(s);
  const auto depth = [&](int x, int y) { return im.At(x, y); };
  Trace full;
  full.camera = s.cam;
  full.to_sun = Sun(25.0, 100.0);
  full.length = 1.0f;
  Trace off = full;
  off.adapt_pixels = 0.0f;
  bool same = true;
  for (int y = 1; y < s.cam.height - 1; y += 5) {
    for (int x = 1; x < s.cam.width - 1; x += 5) {
      full.noise = off.noise = PixelNoise(x, y);
      const float a = March(full, depth, x, y);
      const float b = March(off, depth, x, y);
      same = same && a == b && (off.steps_used == 0 || off.steps_used == kStepsUltra);
    }
  }
  Check(same, "an end gap of 0 (the lever off) is the full march exactly, 16 steps");
}

// The filter inside the apply pass (AoBilateralEdgeFolded) is the denoise pass's filter (AoBilateralEdge): the same
// weights over the same 3 x 3, the four neighbours' depths coming from the apply's own reads for its normal.
void FilterInTheApply() {
  Scene s;
  s.cam = MakeCamera(720);
  s.boxes = AdaptScenes().back().boxes;
  const DepthImage im = RenderDepth(s);
  const Camera& cam = s.cam;
  const auto depth = [&](int x, int y) { return im.At(x, y); };
  // The march's image: the visibility at every pixel (raw: the noise of the ray's start is in it).
  Trace t;
  t.camera = cam;
  t.to_sun = Sun(25.0, 100.0);
  t.length = 1.0f;
  t.adapt_pixels = kAdaptEndPixels;
  std::vector<float> vis(im.d.size(), 1.0f);
  for (int y = 0; y < cam.height; ++y) {
    for (int x = 0; x < cam.width; ++x) {
      t.noise = PixelNoise(x, y);
      vis[size_t(y) * size_t(cam.width) + size_t(x)] = March(t, depth, x, y);
    }
  }
  const auto vis_at = [&](int x, int y) { return vis[size_t(y) * size_t(cam.width) + size_t(x)]; };
  int pixels = 0, differing = 0, changed = 0;
  double sum_change = 0.0;
  float worst_skipped = 0.0f;
  for (int y = 0; y < cam.height; y += (y < 3 || y > cam.height - 4) ? 1 : 3) {
    for (int x = 0; x < cam.width; x += (x < 3 || x > cam.width - 4) ? 1 : 3) {  // every pixel of the border rows and columns
      const float tc = depth(x, y);
      if (tc >= 60000.0f * 0.5f) {
        continue;
      }
      const Vec3 pc = ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      Vec3 normal = NormalFromDepth(cam, depth, x, y, tc, pc);
      if (Dot(normal, pc) > 0.0f) {
        normal = normal * -1.0f;
      }
      // The apply's reads: left, right, top, bottom, clamped into the image (LinearAt(pixel + offset, last)).
      const float cross[4] = {depth(std::max(x - 1, 0), y), depth(std::min(x + 1, cam.width - 1), y), depth(x, std::max(y - 1, 0)),
                              depth(x, std::min(y + 1, cam.height - 1))};
      const float denoise_pass = EdgeFilter(cam, depth, vis_at, x, y, tc, pc, normal);
      const float in_apply = EdgeFilter(cam, depth, vis_at, x, y, tc, pc, normal, cross);
      ++pixels;
      differing += denoise_pass == in_apply ? 0 : 1;
      const float change = std::fabs(denoise_pass - vis_at(x, y));
      changed += change > 0.01f ? 1 : 0;
      sum_change += change;
      // The skip: where the contact term moves the factor by under kFoldedFilterSkip the filter is left out; the factor is
      // 1 - weight (1 - cs), so the picture moves by at most the weight.
      for (float weight : {0.0f, 0.5f * kFoldedFilterSkip, kFoldedFilterSkip}) {
        worst_skipped = std::max(worst_skipped, std::fabs((1.0f - weight * (1.0f - denoise_pass)) - (1.0f - weight * (1.0f - 1.0f))));
      }
    }
  }
  Check(pixels > 5000 && differing == 0,
        Num("the filter in the apply is the denoise pass's, bit for bit, over the image and its border: %.0f of %.0f pixels differ", double(differing), double(pixels)));
  Check(changed > 0 && sum_change / pixels < 0.05, Num("and it filters (mean change %.4f)", sum_change / pixels));
  Check(worst_skipped <= kFoldedFilterSkip + 1e-7f, Num("a pixel left out of the filter (weight %.4f or less) is off by at most %.4f", kFoldedFilterSkip, worst_skipped));
}

// ---------------------------------------------------------------------------
// the skip under the game's own shadow: the twin of the march's skip against the twin of the apply
// ---------------------------------------------------------------------------

// The game's sun shadow mask as the resolve product holds it: 8-bit, the picture's size (the resolve is made at the render scale: a
// 3840x2160 mask at 3x; the check also runs the old assumption of a 1280x720 mask at every picture size), the stored value 0 in a shadow
// and up to 0.5 (128/255) in the sun (the apply's gain 2 turns it into a visibility of 1).
struct MaskImage {
  int w = 1280, h = 720;
  std::vector<uint8_t> v;
  float At(int x, int y) const { return float(v[size_t(y) * size_t(w) + size_t(x)]) / 255.0f; }
};
enum class MaskKind { kHalf, kDiagonal, kStripe, kSpecks, kBlocks, kFloor, kAllLit, kAllShadow };
const char* MaskName(MaskKind k) {
  switch (k) {
    case MaskKind::kHalf: return "half in shade";
    case MaskKind::kDiagonal: return "soft diagonal";
    case MaskKind::kStripe: return "lit stripe";
    case MaskKind::kSpecks: return "lit specks";
    case MaskKind::kBlocks: return "ragged blocks";
    case MaskKind::kFloor: return "shade at 2/255";
    case MaskKind::kAllLit: return "all lit";
    case MaskKind::kAllShadow: return "all shade";
  }
  return "?";
}
uint32_t Hash2(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return h;
}
MaskImage MakeMask(MaskKind kind, int width = 1280, int height = 720) {
  MaskImage m;
  m.w = width;
  m.h = height;
  m.v.assign(size_t(m.w) * size_t(m.h), 0);
  const uint8_t lit = 128;  // 0.5: the sun
  for (int y = 0; y < m.h; ++y) {
    for (int x = 0; x < m.w; ++x) {
      const float u = (float(x) + 0.5f) / float(m.w), v = (float(y) + 0.5f) / float(m.h);
      uint8_t s = 0;
      switch (kind) {
        case MaskKind::kHalf: s = u >= 0.5f ? lit : 0; break;  // 50% in shade
        case MaskKind::kDiagonal: {  // the shade is the bottom left corner under a slanted edge (a penumbra of 3 texels): about 30% of the ground
          const float ramp = Saturate((0.8f * u + 0.6f * (1.0f - v) - 0.39f) / 0.0024f);
          s = uint8_t(std::lround(ramp * float(lit)));
          break;
        }
        case MaskKind::kStripe: {  // a 5 texel stripe of sun in the shade
          const int at = int(0.3984f * float(m.w));
          s = (x >= at && x <= at + 4) ? lit : 0;
          break;
        }
        case MaskKind::kSpecks: {  // single lit texels in the shade
          const auto at = [&](float fx, float fy) { return x == int(fx * float(m.w)) && y == int(fy * float(m.h)); };
          s = (at(0.5469f, 0.5556f) || at(0.0781f, 0.8333f) || at(0.7813f, 0.2778f) || at(0.5f, 0.5f)) ? lit : 0;
          break;
        }
        case MaskKind::kBlocks: s = (Hash2(uint32_t(x / 40), uint32_t(y / 30)) % 100u) < 40u ? 0 : lit; break;  // 40% in shade, ragged
        case MaskKind::kFloor: s = u >= 0.5f ? lit : 2; break;  // the shade stores 2/255, not 0
        case MaskKind::kAllLit: s = lit; break;
        case MaskKind::kAllShadow: s = 0; break;
      }
      m.v[size_t(y) * size_t(m.w) + size_t(x)] = s;
    }
  }
  return m;
}

// The apply's bilinear clamp sample of the mask at (u, v) (SampleLevel with the linear clamp sampler). quant 0: exact; 1 and 2:
// the sampler's fixed point (8 fractional bits) in the worst direction (truncated) and rounded, the weights quantised the same.
float SampleMask(const MaskImage& m, float u, float v, int quant) {
  float tx = u * float(m.w) - 0.5f, ty = v * float(m.h) - 0.5f;
  if (quant == 1) {
    tx = std::floor(tx * 256.0f) / 256.0f;
    ty = std::floor(ty * 256.0f) / 256.0f;
  } else if (quant == 2) {
    tx = std::floor(tx * 256.0f + 0.5f) / 256.0f;
    ty = std::floor(ty * 256.0f + 0.5f) / 256.0f;
  }
  const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
  const int x0 = int(std::floor(tx)), y0 = int(std::floor(ty));
  const auto at = [&](int x, int y) { return m.At(std::clamp(x, 0, m.w - 1), std::clamp(y, 0, m.h - 1)); };
  const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
  const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1.0f - fy) + bottom * fy;
}

// The apply's numbers (the frame's light, the setting's strength and gain, the contact fade's range in metres).
struct ApplyConfig {
  float ambient = 0.35f, sun = 1.0f, strength = 1.0f, gain = 2.0f;
  float fade_start = 40.0f, fade_end = 80.0f;  // ao.cpp kContactFadeStart, kContactFadeEnd
};

// The weight of the contact term at (x, y) in the apply (ao_apply_ps.hlsl): strength * sun_share * fade.
template <class DepthAt>
float ApplyWeight(const Camera& cam, DepthAt&& depth, const MaskImage& mask, int quant, const ApplyConfig& a, Vec3 to_sun,
                  int x, int y, float* tc_out, Vec3* pc_out, Vec3* normal_out) {
  const float tc = depth(x, y);
  *tc_out = tc;
  if (tc >= a.fade_end) {
    return 0.0f;  // the apply discards past the contact fade's end
  }
  const Vec3 pc = ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, tc);
  Vec3 normal = NormalFromDepth(cam, depth, x, y, tc, pc);
  if (Dot(normal, pc) > 0.0f) {
    normal = normal * -1.0f;
  }
  *pc_out = pc;
  *normal_out = normal;
  const float n_dot_l = Saturate(Dot(normal, to_sun));
  const float sun = a.sun * n_dot_l;
  const float sun_vis = Saturate(a.gain * SampleMask(mask, (float(x) + 0.5f) / float(cam.width), (float(y) + 0.5f) / float(cam.height), quant));
  const float sun_lit = sun * sun_vis;
  const float sun_share = (a.ambient + sun_lit) > 1e-6f ? sun_lit / (a.ambient + sun_lit) : 0.0f;
  const float contact_fade = 1.0f - Smoothstep(a.fade_start, a.fade_end, tc);
  return a.strength * sun_share * contact_fade;
}

// The factor the apply gives the pixel (x, y) with the filter inside it: the weight, the filter where the weight is above its
// threshold (the march's image is read at the 3 x 3), 1 - weight (1 - cs).
template <class DepthAt, class VisAt>
float ApplyFactor(const Camera& cam, DepthAt&& depth, VisAt&& vis, const MaskImage& mask, int quant, const ApplyConfig& a, Vec3 to_sun,
                  int x, int y, bool* filtered = nullptr) {
  float tc;
  Vec3 pc, normal;
  const float weight = ApplyWeight(cam, depth, mask, quant, a, to_sun, x, y, &tc, &pc, &normal);
  float cs = 1.0f;
  if (filtered) {
    *filtered = false;
  }
  if (weight > kFoldedFilterSkip) {
    const float cross[4] = {depth(std::max(x - 1, 0), y), depth(std::min(x + 1, cam.width - 1), y), depth(x, std::max(y - 1, 0)),
                            depth(x, std::min(y + 1, cam.height - 1))};
    cs = EdgeFilter(cam, depth, vis, x, y, tc, pc, normal, cross);
    if (filtered) {
      *filtered = true;
    }
  }
  return 1.0f - Saturate(weight * (1.0f - cs));
}

// One picture size of the street scene, the march's image made on demand (every pixel once).
struct SkipFrame {
  Camera cam;
  DepthImage im;
  Trace trace;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the pixels the factors are compared over
  std::vector<float> vis;              // the march's value, NaN until made
  std::vector<uint8_t> would;          // 1: the march runs its loop at this pixel (not sky, not far, facing the sun)
  float Depth(int x, int y) const { return im.At(x, y); }
  float Vis(int x, int y) {
    float& slot = vis[size_t(y) * size_t(cam.width) + size_t(x)];
    if (std::isnan(slot)) {
      trace.noise = PixelNoise(x, y);
      slot = March(trace, [&](int a, int b) { return im.At(a, b); }, x, y);
    }
    return slot;
  }
  bool Would(int x, int y) const { return would[size_t(y) * size_t(cam.width) + size_t(x)] != 0; }
};
SkipFrame MakeSkipFrame(int height, bool window) {
  SkipFrame f;
  Scene s;
  s.cam = MakeCamera(height);
  s.boxes = AdaptScenes().back().boxes;  // the street
  f.cam = s.cam;
  f.im = RenderDepth(s);
  f.trace.camera = f.cam;
  f.trace.to_sun = Sun(25.0, 100.0);
  f.trace.length = 1.0f;
  f.trace.adapt_pixels = kAdaptEndPixels;
  f.vis.assign(f.im.d.size(), std::nanf(""));
  f.would.assign(f.im.d.size(), 0);
  const auto depth = [&](int a, int b) { return f.im.At(a, b); };
  for (int y = 0; y < f.cam.height; ++y) {
    for (int x = 0; x < f.cam.width; ++x) {
      const float tc = depth(x, y);
      if (tc >= f.trace.sky * 0.5f || tc >= f.trace.max_distance) {
        continue;
      }
      const Vec3 pc = ViewPos(f.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      Vec3 normal = NormalFromDepth(f.cam, depth, x, y, tc, pc);
      if (Dot(normal, pc) > 0.0f) {
        normal = normal * -1.0f;
      }
      f.would[size_t(y) * size_t(f.cam.width) + size_t(x)] = Dot(normal, f.trace.to_sun) > 0.0f ? 1 : 0;
    }
  }
  // 3840x2160 compares a window of the picture (20% to 55% across, 50% to 90% down: the street's ground and the edges of the
  // half, the diagonal and the blocks are in it); the other sizes compare every pixel. The skip map and the statistics
  // are over the whole picture.
  f.x0 = window ? f.cam.width * 20 / 100 : 0;
  f.x1 = window ? f.cam.width * 55 / 100 : f.cam.width;
  f.y0 = window ? f.cam.height * 50 / 100 : 0;
  f.y1 = window ? f.cam.height * 90 / 100 : f.cam.height;
  return f;
}

struct SkipResult {
  long marched = 0;                 // pixels whose march runs its loop (whole picture)
  long shadowed = 0;                // ... of them, in the game's shadow: the mask is 0 at the pixel's own sample
  long skipped = 0;                 // ... the skip leaves out
  long reference_skipped = 0;       // ... the exact range (the reference, texel by texel) would leave out
  long beyond_reference = 0;        // ... the test skips that the reference does not: must be none (the blocks hold the range)
  long fetches = 0;                 // texture fetches the test made over the marching pixels (1 in the sun)
  long shaded_fetches = 0;          // ... over the pixels in the game's shadow
  long tile_saved = 0;              // ... in whole 8 x 4 tiles in which every marching pixel is skipped (what a warp saves)
  long compared = 0;                // pixels whose factor was computed with and without the skip (a neighbour skipped with a value other than 1)
  long differing = 0;               // ... whose factors differ in any bit
  long picture_differing = 0;       // ... whose picture (the apply discards a factor of 0.999 or more) differs
  long filtered_next_to_skipped = 0;  // pixels that run the apply's filter with a skipped pixel among the 8 neighbours or itself
  long region = 0;                  // pixels compared over
};

// The march with the skip against the march without it, through the apply's twin, over one frame and one mask.
// reach: 1 the rule, 0 the control (the pixel's own footprint only, no dilation by the filter's neighbours).
SkipResult RunSkip(SkipFrame& f, const MaskImage& mask, const ApplyConfig& a, int quant, int reach) {
  SkipResult r;
  const Camera& cam = f.cam;
  const auto depth = [&](int x, int y) { return f.im.At(x, y); };
  const auto mask_at = [&](int tx, int ty) { return mask.At(tx, ty); };
  const float limit = SkipLimit(a.ambient, a.sun, a.strength, a.gain);
  const size_t n = f.im.d.size();
  std::vector<uint8_t> skip(n, 0);
  // reach 1 is the shader's test (the block pass's image and the block test), whose result is checked against the reference range; reach
  // 0 is the control, the pixel's own footprint with the reference's loop.
  const SkipBlockImage blocks = MakeSkipBlocks(mask_at, mask.w, mask.h, limit);
  for (int y = 0; y < cam.height; ++y) {
    for (int x = 0; x < cam.width; ++x) {
      if (!f.Would(x, y)) {
        continue;
      }
      bool skipped = false;
      if (reach == 1) {
        SkipTestCost cost;
        skipped = limit >= 0.0f && MaskCannotLightBlocks(blocks, mask.w, mask.h, cam.width, cam.height, x, y, &cost);
        const bool reference = MaskCannotLight(mask_at, mask.w, mask.h, cam.width, cam.height, x, y, limit, 1);
        r.fetches += cost.fetches;
        r.reference_skipped += reference ? 1 : 0;
        r.beyond_reference += (skipped && !reference) ? 1 : 0;
        if (SampleMask(mask, (float(x) + 0.5f) / float(cam.width), (float(y) + 0.5f) / float(cam.height), 0) == 0.0f) {
          r.shaded_fetches += cost.fetches;
        }
      } else {
        skipped = MaskCannotLight(mask_at, mask.w, mask.h, cam.width, cam.height, x, y, limit, reach);
      }
      if (skipped) {
        skip[size_t(y) * size_t(cam.width) + size_t(x)] = 1;
      }
    }
  }
  const auto skipped = [&](int x, int y) { return skip[size_t(y) * size_t(cam.width) + size_t(x)] != 0; };
  // The statistics: the pixels that march, the share in the game's shadow, the share skipped, whole tiles.
  for (int ty = 0; ty < cam.height; ty += 4) {
    for (int tx = 0; tx < cam.width; tx += 8) {
      long in_tile = 0;
      bool all = true;
      for (int y = ty; y < std::min(ty + 4, cam.height); ++y) {
        for (int x = tx; x < std::min(tx + 8, cam.width); ++x) {
          if (!f.Would(x, y)) {
            continue;
          }
          ++in_tile;
          r.marched += 1;
          r.skipped += skipped(x, y) ? 1 : 0;
          all = all && skipped(x, y);
          const float own = SampleMask(mask, (float(x) + 0.5f) / float(cam.width), (float(y) + 0.5f) / float(cam.height), 0);
          r.shadowed += own == 0.0f ? 1 : 0;
        }
      }
      r.tile_saved += all ? in_tile : 0;
    }
  }
  // The factors. vis_skipped is what the apply reads from the march's image with the skip: 1 where the march was left out.
  const auto vis_full = [&](int x, int y) { return f.Vis(x, y); };
  const auto vis_skipped = [&](int x, int y) { return skipped(x, y) ? 1.0f : f.Vis(x, y); };
  for (int y = f.y0; y < f.y1; ++y) {
    for (int x = f.x0; x < f.x1; ++x) {
      ++r.region;
      bool any_skipped = false, any_changed = false;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int qx = std::clamp(x + dx, 0, cam.width - 1), qy = std::clamp(y + dy, 0, cam.height - 1);
          if (skipped(qx, qy)) {
            any_skipped = true;
            any_changed = any_changed || vis_full(qx, qy) != 1.0f;
          }
        }
      }
      if (!any_skipped) {
        continue;
      }
      bool filtered = false;
      const float with_skip = ApplyFactor(cam, depth, vis_skipped, mask, quant, a, f.trace.to_sun, x, y, &filtered);
      r.filtered_next_to_skipped += filtered ? 1 : 0;
      if (!any_changed) {
        continue;  // every skipped pixel in reach holds the 1 its march would have given: the same inputs, the same factor
      }
      const float without = ApplyFactor(cam, depth, vis_full, mask, quant, a, f.trace.to_sun, x, y);
      ++r.compared;
      if (std::bit_cast<uint32_t>(with_skip) != std::bit_cast<uint32_t>(without)) {
        ++r.differing;
        if ((with_skip >= 0.999f ? 1.0f : with_skip) != (without >= 0.999f ? 1.0f : without)) {
          ++r.picture_differing;
        }
      }
    }
  }
  return r;
}

void SkipRules() {
  // The limit: the largest stored value at which strength * sun v / (ambient + sun v) stays under kSkipWeight.
  Check(kSkipWeight == 0.5f * kFoldedFilterSkip, "the skip's weight bound is half of the apply's filter threshold");
  Check(SkipLimit(0.35f, 1.0f, 1.0f, 2.0f) >= 0.0f && SkipLimit(0.35f, 1.0f, 1.0f, 2.0f) < 1.0f / 255.0f,
        Num("with the usual light (ambient 0.35, sun 1) only a stored 0 is skipped (limit %.6f of 1/255 = %.6f)", SkipLimit(0.35f, 1.0f, 1.0f, 2.0f), 1.0 / 255.0));
  Check(SkipLimit(-0.1f, 1.0f, 1.0f, 2.0f) < 0.0f && SkipLimit(0.3f, 0.0f, 1.0f, 2.0f) < 0.0f && SkipLimit(0.3f, 1.0f, 1.0f, 0.0f) < 0.0f &&
            SkipLimit(0.3f, 1.0f, 0.0005f, 2.0f) < 0.0f && SkipLimit(std::nanf(""), 1.0f, 1.0f, 2.0f) < 0.0f && SkipLimit(0.3f, std::nanf(""), 1.0f, 2.0f) < 0.0f &&
            SkipLimit(0.3f, 1.0f, std::nanf(""), 2.0f) < 0.0f,
        "inputs the bound is not proven for (negative ambient, no sun, no gain, a strength under the bound, NaN) never skip");
  Check(SkipLimit(0.0f, 1.0f, 1.0f, 2.0f) == 0.0f, "with no ambient only a stored 0 is skipped (any light on it is all of the weight)");
  bool ok = true;
  const float lights[][4] = {{0.35f, 1.0f, 1.0f, 2.0f}, {3.0f, 0.5f, 0.2f, 2.0f}, {0.05f, 2.0f, 1.5f, 0.5f}, {1.0f, 1.0f, 0.01f, 4.0f}, {20.0f, 0.2f, 0.05f, 1.0f}};
  for (const auto& l : lights) {
    const float limit = SkipLimit(l[0], l[1], l[2], l[3]);
    const auto bound = [&](float stored) {
      const float v = Saturate(l[3] * stored);
      const float s = l[1] * v;
      return (l[0] + s) > 1e-6f ? l[2] * s / (l[0] + s) : 0.0f;
    };
    if (limit < 3.0e38f) {
      ok = ok && bound(limit * 0.999f) <= kSkipWeight * 1.001f && bound(limit * 1.01f + 1e-6f) > kSkipWeight;
    } else {
      ok = ok && bound(1.0f) <= kSkipWeight * 1.001f;
    }
  }
  Check(ok, "SkipLimit is where the weight bound crosses kSkipWeight, for lights from dim ambient to bright, strengths 0.01 to 1.5");
  Check(SkipLimit(20.0f, 0.2f, 0.05f, 1.0f) > 1.0f, "a sun that is dim against the ambient and a small strength skip whatever the mask holds");

  // The texels: every texel the bilinear samples (the sampler's fixed point, truncated, rounded or exact) of the 3 pixels
  // reach is in the range, for the scales the mask and the picture have.
  bool within = true;
  long worst_span = 0;
  const int images[] = {640, 1280, 1920, 2560, 3840};
  for (int image : images) {
    const int mask = 1280;
    for (int p = 0; p < image; ++p) {
      int first, last;
      SkipTexelRange(p, image, mask, &first, &last);
      worst_span = std::max<long>(worst_span, last - first);
      for (int q = std::max(p - 1, 0); q <= std::min(p + 1, image - 1); ++q) {
        for (int quant = 0; quant < 3; ++quant) {
          float t = (float(q) + 0.5f) / float(image) * float(mask) - 0.5f;
          if (quant == 1) t = std::floor(t * 256.0f) / 256.0f;
          if (quant == 2) t = std::floor(t * 256.0f + 0.5f) / 256.0f;
          const int a = std::clamp(int(std::floor(t)), 0, mask - 1), b = std::clamp(int(std::floor(t)) + 1, 0, mask - 1);
          within = within && a >= first && b <= last;
        }
      }
    }
  }
  Check(within, "the texel range holds every bilinear tap of the 3 pixels, at the sampler's fixed point, truncated or rounded, 640 to 3840 wide");
  Check(float(worst_span) < kSkipMaxTexels, Num("and it stays under the widest span tested (%.0f of %.0f texels) for pictures from half the mask's size up", double(worst_span), double(kSkipMaxTexels)));
  int first, last;
  SkipTexelRange(0, 3840, 1280, &first, &last);
  Check(first == 0, "at the picture's edge the range is clamped into the mask");
  SkipTexelRange(3839, 3840, 1280, &first, &last);
  Check(last == 1279, "and at the far edge");

  // The switch.
  Check(MaskSkipOn(true, true, true, false, 0.0f, true), "the skip is on with the lever, the filter in the apply, a mask, a limit that is proven and no debug picture");
  Check(!MaskSkipOn(false, true, true, false, 0.0f, true) && !MaskSkipOn(true, false, true, false, 0.0f, true) && !MaskSkipOn(true, true, false, false, 0.0f, true) &&
            !MaskSkipOn(true, true, true, true, 0.0f, true) && !MaskSkipOn(true, true, true, false, -1.0f, true) && !MaskSkipOn(true, true, true, false, 0.0f, false),
        "and off with the lever off, at half size, in a frame without the mask (6 h, 18 h, rain), on a debug picture, with a limit that is not proven (no strength) or a mask too large");
  Check(SkipScaleOk(1280, 720, 1280, 720) && SkipScaleOk(3840, 2160, 3840, 2160) && SkipScaleOk(1280, 720, 3840, 2160) && SkipScaleOk(1280, 720, 640, 360) &&
            !SkipScaleOk(3840, 2160, 1280, 720) && !SkipScaleOk(0, 720, 1280, 720) && SkipScaleOk(3000, 2160, 1280, 1080) && !SkipScaleOk(3200, 720, 1280, 720),
        "the skip is used for a mask up to 2.4 times the picture's size either way and not for a larger one");

  // A frame without the mask skips nothing, and a lever that is off skips nothing: the shader reads the flag.
  Check(kSkipFlag == (1u << 24) && kStatFlag == (1u << 26) && kSideFlag == (1u << 27) && (kSkipFlag & kStatFlag) == 0 && (kStatFlag & kSideFlag) == 0,
        "the skip's flag is bit 24, the statistics' 26 and the block pass's 27");
  const auto no_mask = [](int, int) { return 0.0f; };
  Check(!MaskCannotLight(no_mask, 1280, 720, 1280, 720, 100, 100, -1.0f), "a limit of -1 (not provable) skips nothing even over a mask of zeros");
}

void SkipUnderTheGamesShadow() {
  struct Size {
    int height;
    bool window;
  };
  const Size sizes[] = {{720, false}, {1080, false}, {2160, true}};
  const MaskKind kinds[] = {MaskKind::kHalf, MaskKind::kDiagonal, MaskKind::kStripe, MaskKind::kSpecks, MaskKind::kBlocks,
                            MaskKind::kFloor, MaskKind::kAllLit, MaskKind::kAllShadow};
  const ApplyConfig usual;                                              // ambient 0.35, sun 1, strength 1, gain 2
  ApplyConfig bright_ambient;                                           // a dim sun in a bright ambient with a small strength: the limit is above 1/255
  bright_ambient.ambient = 3.0f;
  bright_ambient.sun = 0.5f;
  bright_ambient.strength = 0.2f;
  double catch_sum[3] = {};
  int catch_count[3] = {};
  long control_violations = 0;
  long control_differing = 0;
  for (size_t si = 0; si < 3; ++si) {
    SkipFrame f = MakeSkipFrame(sizes[si].height, sizes[si].window);
    std::printf("  skip under the game's shadow, %4dp (compared over %d x %d pixels of %d x %d):\n", sizes[si].height, f.x1 - f.x0, f.y1 - f.y0,
                f.cam.width, f.cam.height);
    for (MaskKind kind : kinds) {
      const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
      const SkipResult r = RunSkip(f, mask, usual, 0, 1);
      const double marched = std::max<double>(double(r.marched), 1.0);
      std::printf("    %-15s marched %8ld: in the game's shade %5.1f%%, skipped %5.1f%% (%5.1f%% of the shade; the exact range %5.1f%%), whole 8x4 tiles %5.1f%%; "
                  "test fetches %.2f a pixel (%.2f in shade); compared %7ld with a skipped pixel in reach, differing %ld, filtering beside a skipped one %ld\n",
                  MaskName(kind), r.marched, 100.0 * double(r.shadowed) / marched, 100.0 * double(r.skipped) / marched,
                  100.0 * double(r.skipped) / std::max<double>(double(r.shadowed), 1.0), 100.0 * double(r.reference_skipped) / std::max<double>(double(r.shadowed), 1.0),
                  100.0 * double(r.tile_saved) / marched, double(r.fetches) / marched, double(r.shaded_fetches) / std::max<double>(double(r.shadowed), 1.0), r.compared,
                  r.differing, r.filtered_next_to_skipped);
      Check(r.beyond_reference == 0, std::string(MaskName(kind)) + Num(" at %.0fp", double(sizes[si].height)) + Num(": the test skips nothing the exact range would not (%.0f)", double(r.beyond_reference)));
      const std::string row = std::string(MaskName(kind)) + Num(" at %.0fp", double(sizes[si].height));
      Check(r.marched > 100000, row + " has pixels to march");
      Check(r.differing == 0 && r.picture_differing == 0,
            row + Num(": the factor with the skip is the factor without it, to the bit (%.0f of %.0f differ)", double(r.differing), double(r.compared)));
      Check(r.filtered_next_to_skipped == 0, row + Num(": no pixel that filters has a skipped pixel in reach (%.0f)", double(r.filtered_next_to_skipped)));
      if (kind == MaskKind::kAllLit) {
        Check(r.skipped == 0, row + ": nothing is skipped where the sun is everywhere");
      }
      if (kind == MaskKind::kAllShadow) {
        Check(r.skipped == r.marched, row + ": everything is skipped where all is shade");
      }
      if (kind == MaskKind::kFloor) {
        Check(r.skipped == 0, row + ": a shade that stores 2/255 is not skipped with the usual light (the weight there is above the bound)");
      }
      if (kind == MaskKind::kHalf || kind == MaskKind::kDiagonal || kind == MaskKind::kBlocks) {
        // (the soft diagonal's edge at 2160p crosses open ground in the window, where every march is 1: nothing to compare there)
        Check(r.skipped > 0 && (r.compared > 0 || (kind == MaskKind::kDiagonal && sizes[si].height == 2160)),
              row + ": skips pixels, some of them whose march would not have been 1");
        catch_sum[si] += double(r.tile_saved) / std::max<double>(double(r.shadowed), 1.0);
        catch_count[si] += 1;
      }
    }
    // A mask at another scale than the picture's (the first version of the check had a 1280x720 mask at every size): a third of the picture's
    // size at 2160p, two thirds at 1080p.
    if (sizes[si].height > 720) {
      for (MaskKind kind : {MaskKind::kHalf, MaskKind::kDiagonal, MaskKind::kBlocks, MaskKind::kStripe, MaskKind::kSpecks}) {
        const MaskImage mask = MakeMask(kind, 1280, 720);
        const SkipResult r = RunSkip(f, mask, usual, 0, 1);
        const std::string row = std::string(MaskName(kind)) + Num(", a 1280x720 mask at %.0fp", double(sizes[si].height));
        std::printf("    1280x720 mask, %-15s skipped %5.1f%% of the shade (the exact range %5.1f%%), test fetches %.2f a pixel; compared %ld, differing %ld\n",
                    MaskName(kind), 100.0 * double(r.skipped) / std::max<double>(double(r.shadowed), 1.0),
                    100.0 * double(r.reference_skipped) / std::max<double>(double(r.shadowed), 1.0), double(r.fetches) / std::max<double>(double(r.marched), 1.0),
                    r.compared, r.differing);
        Check(r.differing == 0 && r.picture_differing == 0 && r.filtered_next_to_skipped == 0 && r.beyond_reference == 0,
              row + Num(": exact (%.0f of %.0f differ)", double(r.differing), double(r.compared)));
      }
    }
    // The control: the pixel's own footprint only (no dilation by the filter's neighbours) is NOT exact, so the check has teeth.
    for (MaskKind kind : {MaskKind::kHalf, MaskKind::kDiagonal, MaskKind::kBlocks}) {
      const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
      const SkipResult r = RunSkip(f, mask, usual, 0, 0);
      control_violations += r.filtered_next_to_skipped;
      control_differing += r.differing;
      std::printf("    control (no dilation) %-15s: filtering beside a skipped pixel %ld, factors that differ %ld of %ld\n", MaskName(kind),
                  r.filtered_next_to_skipped, r.differing, r.compared);
    }
    // The other light (a limit above 1/255: the shade at 2/255 is skipped now, and still nothing changes) and the sampler's two roundings.
    for (MaskKind kind : {MaskKind::kFloor, MaskKind::kDiagonal, MaskKind::kBlocks}) {
      const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
      const SkipResult r = RunSkip(f, mask, bright_ambient, 0, 1);
      std::printf("    dim sun in a bright ambient, %-15s: skipped %5.1f%% of %ld, compared %ld, differing %ld, filtering beside a skipped one %ld\n",
                  MaskName(kind), 100.0 * double(r.skipped) / std::max<double>(double(r.marched), 1.0), r.marched, r.compared, r.differing,
                  r.filtered_next_to_skipped);
      const std::string row = std::string("bright ambient ") + MaskName(kind) + Num(" at %.0fp", double(sizes[si].height));
      Check(r.differing == 0 && r.filtered_next_to_skipped == 0, row + ": exact too");
      if (kind == MaskKind::kFloor) {
        Check(r.skipped > 0, row + ": the shade of 2/255 is skipped when the weight bound allows it");
      }
    }
    if (sizes[si].height <= 1080) {
      for (int quant : {1, 2}) {
        for (MaskKind kind : {MaskKind::kDiagonal, MaskKind::kBlocks, MaskKind::kHalf}) {
          const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
          const SkipResult r = RunSkip(f, mask, usual, quant, 1);
          const std::string row = std::string(MaskName(kind)) + Num(" at %.0fp", double(sizes[si].height)) + (quant == 1 ? ", sampler truncating" : ", sampler rounding");
          Check(r.differing == 0 && r.filtered_next_to_skipped == 0,
                row + Num(": exact (%.0f of %.0f differ)", double(r.differing), double(r.compared)));
        }
      }
    }
  }
  Check(control_violations > 0 && control_differing > 0,
        Num("the control (no dilation) does change the picture: %.0f filtering pixels beside a skipped one, %.0f factors differ", double(control_violations), double(control_differing)));
  std::printf("skip under the game's shadow: share of the shade skipped in whole 8x4 tiles (half, diagonal, blocks): 720p %.3f, 1080p %.3f, 2160p %.3f (the model's: %.2f, %.2f, %.2f)\n",
              catch_sum[0] / catch_count[0], catch_sum[1] / catch_count[1], catch_sum[2] / catch_count[2], kSkipCatch720, kSkipCatch1080,
              kSkipCatch2160);
  for (size_t si = 0; si < 3; ++si) {
    Check(std::fabs(catch_sum[si] / catch_count[si] - SkipCatch(uint32_t(sizes[si].height))) < 0.03,
          Num("the model's catch (%.2f) is what the check's scenes see", SkipCatch(uint32_t(sizes[si].height))) + Num(" (%.3f) at %.0fp", catch_sum[si] / catch_count[si], double(sizes[si].height)));
  }
}

// The blocks hold the range: for a lattice of pixels at several picture and mask sizes, a lone block that contains a texel of the reference
// range (the corners and the centre of it) is never skipped past: the test returns false (marched) for that block alone being 1.
void SkipBlocksHoldTheRange() {
  struct Case {
    int image_w, image_h, mask_w, mask_h;
  };
  const Case cases[] = {{3840, 2160, 3840, 2160}, {2560, 1440, 2560, 1440}, {1920, 1080, 1920, 1080}, {1280, 720, 1280, 720}, {1920, 1080, 1280, 720},
                        {3840, 2160, 1280, 720},  {1280, 720, 1920, 1080}, {640, 360, 1280, 720},     {1000, 700, 2400, 1680}, {1366, 768, 1366, 768}};
  long tested = 0, missed = 0, skipped_all_zero = 0, not_skipped_all_zero = 0;
  for (const Case& c : cases) {
    SkipBlockImage zero;
    zero.w = SkipBlockCount(c.mask_w);
    zero.h = SkipBlockCount(c.mask_h);
    zero.bit.assign(size_t(zero.w) * size_t(zero.h), 0);
    const int stride_x = std::max(c.image_w / 220, 1), stride_y = std::max(c.image_h / 150, 1);
    for (int y = 0; y < c.image_h; y += (y < 4 || y > c.image_h - 6) ? 1 : stride_y) {
      for (int x = 0; x < c.image_w; x += (x < 4 || x > c.image_w - 6) ? 1 : stride_x) {
        // All blocks 0: skipped (the test says the footprint is in shade), whatever the pixel.
        const bool all_zero_skipped = MaskCannotLightBlocks(zero, c.mask_w, c.mask_h, c.image_w, c.image_h, x, y);
        skipped_all_zero += all_zero_skipped ? 1 : 0;
        not_skipped_all_zero += all_zero_skipped ? 0 : 1;
        int x0, x1, y0, y1;
        SkipTexelRange(x, c.image_w, c.mask_w, &x0, &x1);
        SkipTexelRange(y, c.image_h, c.mask_h, &y0, &y1);
        const int texels[][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}, {(x0 + x1) / 2, (y0 + y1) / 2}, {x0, (y0 + y1) / 2}, {x1, (y0 + y1) / 2}};
        for (const auto& tx : texels) {
          SkipBlockImage one = zero;
          one.bit[size_t(tx[1] / int(kSkipBlock)) * size_t(one.w) + size_t(tx[0] / int(kSkipBlock))] = 1;
          ++tested;
          missed += MaskCannotLightBlocks(one, c.mask_w, c.mask_h, c.image_w, c.image_h, x, y) ? 1 : 0;
        }
      }
    }
  }
  Check(tested > 100000 && missed == 0, Num("a lone block that holds a texel of the pixel's range stops the skip, at 10 picture and mask sizes (%.0f of %.0f missed)", double(missed), double(tested)));
  Check(not_skipped_all_zero == 0 && skipped_all_zero > 100000, Num("with every block 0 every pixel is skipped (%.0f not)", double(not_skipped_all_zero)));
  // The block pass's own image: a block is 1 where any of its 4 x 4 texels (clamped into the mask) is above the limit.
  MaskImage m;
  m.w = 10;
  m.h = 7;
  m.v.assign(size_t(m.w) * size_t(m.h), 0);
  m.v[size_t(6) * size_t(m.w) + 9] = 100;  // the last texel: block (2, 1) of a 3 x 2 image
  const SkipBlockImage image = MakeSkipBlocks([&](int tx, int ty) { return m.At(tx, ty); }, m.w, m.h, 0.0f);
  Check(image.w == 3 && image.h == 2 && image.bit.size() == 6 && image.At(2, 1) == 1 && image.At(0, 0) == 0 && image.At(1, 0) == 0 && image.At(2, 0) == 0 &&
            image.At(0, 1) == 0 && image.At(1, 1) == 0,
        "the block image of a 10 x 7 mask is 3 x 2 and holds the lone texel in its last block");
  Check(MakeSkipBlocks([&](int tx, int ty) { return m.At(tx, ty); }, m.w, m.h, 0.4f).At(2, 1) == 0 &&
            MakeSkipBlocks([&](int tx, int ty) { return m.At(tx, ty); }, m.w, m.h, 0.39f).At(2, 1) == 1,
        "a texel at the limit does not make its block 1 (above it does)");
}

// The class shares of the street at 720p, 1080p and 2160p that the cost model uses (the sky or past 80 m, turned from the sun, marching).
void StreetMix() {
  for (int height : {720, 1080, 2160}) {
    SkipFrame f = MakeSkipFrame(height, false);
    long far_n = 0, back = 0, march = 0;
    const auto depth = [&](int a, int b) { return f.im.At(a, b); };
    for (int y = 0; y < f.cam.height; ++y) {
      for (int x = 0; x < f.cam.width; ++x) {
        const float tc = depth(x, y);
        if (tc >= f.trace.sky * 0.5f || tc >= f.trace.max_distance) {
          ++far_n;
        } else if (f.Would(x, y)) {
          ++march;
        } else {
          ++back;
        }
      }
    }
    const double n = double(f.im.d.size());
    std::printf("street mix at %dp: far %.3f, turned from the sun %.3f, marching %.3f of all pixels\n", height, double(far_n) / n, double(back) / n, double(march) / n);
    if (height == 2160) {
      Check(std::fabs(double(far_n) / n - kMixFar) < 0.03 && std::fabs(double(back) / n - kMixBack) < 0.03 && std::fabs(double(march) / n - kMixMarch) < 0.03,
            Num("the model's mix (far %.2f, back %.2f, march %.2f) is the street's at 2160p", kMixFar, kMixBack, kMixMarch));
    }
  }
}

// The cost model with the shadowed share (the skip under the game's own shadow).
void SkipCost() {
  const Cost plain = CostModel(3840, 2160, kStepsUltra, 1, true);
  const Cost none = CostModelFast(3840, 2160, kStepsUltra, true);
  const Cost lit = CostModelFast(3840, 2160, kStepsUltra, true, 0.0);
  const Cost third = CostModelFast(3840, 2160, kStepsUltra, true, 0.3);
  const Cost half = CostModelFast(3840, 2160, kStepsUltra, true, 0.5);
  const Cost all = CostModelFast(3840, 2160, kStepsUltra, true, 1.0);
  std::printf("cost model with the skip (RTX 5080, 3840x2160, not measured; the plain march %.3f ms): no mask %.3f ms (march %.3f + filter %.3f); mask, 0%% in shade %.3f (march %.3f + block pass %.3f); "
              "30%% %.3f (march %.3f, %.0f%% of the pixels skipped); 50%% %.3f (march %.3f, %.0f%% skipped); 100%% %.3f (march %.3f)\n",
              plain.march_ms, none.total_ms, none.march_ms, none.filter_ms, lit.total_ms, lit.march_ms, lit.block_ms, third.total_ms, third.march_ms,
              100.0 * third.skipped_share, half.total_ms, half.march_ms, 100.0 * half.skipped_share, all.total_ms, all.march_ms);
  Check(none.skipped_share == 0.0 && none.total_ms == CostModelFast(3840, 2160, kStepsUltra, true, -1.0).total_ms, "no mask (the default): the model as before");
  Check(none.march_ms == plain.march_ms, "at 2160p without the mask the march is the plain march exactly (the adaptive count is off there)");
  Check(lit.march_ms > plain.march_ms && lit.march_ms < 1.015 * plain.march_ms,
        Num("a frame with the mask and no shade pays the 17 instructions of the test only: the march %.2f%% more", 100.0 * (lit.march_ms / plain.march_ms - 1.0)));
  Check(third.march_ms < plain.march_ms && half.march_ms < third.march_ms && all.march_ms < half.march_ms, "the more of the picture in the game's shade, the cheaper");
  Check(third.march_ms < 0.9 * plain.march_ms && half.march_ms < 0.8 * plain.march_ms, "with 30% of the pixels in shade the march is under 90% of the plain one's, with half under 80%");
  Check(third.total_ms < plain.total_ms && lit.total_ms - lit.march_ms - lit.block_ms < 0.2 * plain.total_ms, "the filter in the apply and the block pass are small");
  // The first version's skip test in the same model (its instructions: 92 on a pixel in the sun, 356 on a pixel in shade at the 2160p mask's size,
  // the adaptive count on): worse than the plain march at the street's mix, with and without shade.
  {
    const double plain_instr = AverageInstructions(2160, kStepsUltra, false, 0.0, false);
    const auto first = [&](double shade) {
      const double skipped = shade * kSkipCatch2160;
      const double march_first = kInstrMarchFixed + 16.0 * kInstrStep + kInstrAdaptive;
      return kMixFar * kInstrFar + (kMixBack + kMixMarch) * skipped * kInstrSkippedFirst +
             (1.0 - skipped) * (kMixBack * (kInstrBack + kInstrTestLitFirst) + kMixMarch * (march_first + kInstrTestLitFirst));
    };
    const auto second = [&](double shade) { return AverageInstructions(2160, kStepsUltra, true, shade * kSkipCatch2160, true); };
    std::printf("instructions a pixel of the street executes against the plain march, at 2160p (first version of the test, this one): "
                "0%% shade %+.1f%% %+.1f%%, 30%% %+.1f%% %+.1f%%, 50%% %+.1f%% %+.1f%%\n",
                100.0 * (first(0.0) / plain_instr - 1.0), 100.0 * (second(0.0) / plain_instr - 1.0), 100.0 * (first(0.3) / plain_instr - 1.0),
                100.0 * (second(0.3) / plain_instr - 1.0), 100.0 * (first(0.5) / plain_instr - 1.0), 100.0 * (second(0.5) / plain_instr - 1.0));
    // PLAY4 measured the first version at +7.4% (0.306 against 0.285 ms) in a view that looked half in shade: the model gives +9% for a skip that
    // fires on nothing (the shade not stored as 0?), and a gain for a skip that fires on 30% of the pixels; the statistics line says which it was.
    Check(first(0.0) > plain_instr * 1.05 && first(0.0) < plain_instr * 1.12, "the first version's test, with nothing skipped, costs the street 5% to 12% more than the plain march (PLAY4: +7.4%)");
    Check(second(0.0) < plain_instr * 1.015 && second(0.3) < plain_instr * 0.9 && second(0.5) < plain_instr * 0.8, "this version costs under 1.5% more with no shade and 10% / 20% less with 30% / 50% of it in shade");
  }
  Check(CostModelFast(1920, 1080, kStepsUltra, false, 0.5).total_ms == CostModelFast(1920, 1080, kStepsUltra, false).total_ms,
        "at half size (the filter pass stays) the skip is not used and the model ignores the share");
  Check(SkipCatch(500) == kSkipCatch720 && SkipCatch(4000) == kSkipCatch2160 && SkipCatch(900) >= kSkipCatch720 && SkipCatch(900) <= kSkipCatch1080,
        "the catch is flat outside 720 to 2160 lines and in between inside");
  Check(std::fabs(kMixFar + kMixBack + kMixMarch - 1.0) < 1e-9, "the street's class shares add up to 1");
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
  std::string contact, common, apply;
  if (!ReadFile(dir + "/ao_contact_ps.hlsl", &contact) || !ReadFile(dir + "/ao_common.hlsli", &common) ||
      !ReadFile(dir + "/ao_apply_ps.hlsl", &apply)) {
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
      {&contact, "kAdaptMinSteps", double(kAdaptMinSteps)}, {&apply, "kFoldedFilterSkip", kFoldedFilterSkip},
      {&contact, "kSkipWeight", kSkipWeight},       {&contact, "kSkipTexelMargin", kSkipTexelMargin},
      {&contact, "kSkipMaxTexels", kSkipMaxTexels},  {&contact, "kSkipBlock", kSkipBlock},
  };
  for (const Row& r : rows) {
    double got = 0.0;
    const bool found = ShaderConstant(*r.text, r.name, &got);
    Check(found && std::fabs(got - r.want) < 1e-9 * std::max(1.0, std::fabs(r.want)) + 1e-7,
          std::string("the shader's ") + r.name + " is the header's" + (found ? "" : " (not found)"));
  }
  // The flags are integer constants: bits 24, 26 and 27, in the shader and in the header.
  Check(contact.find("static const uint kSkipFlag = 1u << 24;") != std::string::npos && kSkipFlag == (1u << 24) &&
            contact.find("static const uint kStatFlag = 1u << 26;") != std::string::npos && kStatFlag == (1u << 26) &&
            contact.find("static const uint kSideFlag = 1u << 27;") != std::string::npos && kSideFlag == (1u << 27),
        "the shader's skip, statistics and block pass flags are the header's (bits 24, 26, 27)");
  // The shader's skip is the twin's, line for line where it matters: the block of the centre, the range in blocks, the groups.
  Check(contact.find("g_textures[slot].Load(int3(int2(position * scale), 0)).x > 0.5f") != std::string::npos &&
            contact.find("const float2 lo = (position - 1.0f) * scale - (0.5f + kSkipTexelMargin) * (1.0f / kSkipBlock);") != std::string::npos &&
            contact.find("const float2 end = floor(lo + (2.0f * scale + (2.0f * kSkipTexelMargin + 1.0f) * (1.0f / kSkipBlock)));") != std::string::npos &&
            contact.find("for (float y = first.y; y <= end.y; y += 2.0f)") != std::string::npos &&
            contact.find("(float2(x, y) + 1.0f) * inv_block") != std::string::npos && contact.find("if (any(group > 0.5f))") != std::string::npos,
        "the shader's skip test is written as the twin's (MaskCannotLightBlocks)");
  Check(contact.find("const float2 corner = floor(position) * kSkipBlock + 1.0f;") != std::string::npos &&
            contact.find("(corner + float2(2.0f, 2.0f)) * inv_mask") != std::string::npos && contact.find("> g_push.shape.w ? 1.0f : 0.0f") != std::string::npos,
        "the shader's block pass is the twin's (four gathers at the corners of a 4 x 4 block, above the limit: 1)");
}

}  // namespace

int main(int argc, char** argv) {
  RayLengthRule();
  StepsAndFade();
  ToleranceScalesWithResolution();
  FilterWeights();
  CostRule();
  AdaptiveStepRule();
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
  AdaptiveStepsMatchTheFullMarch();
  FilterInTheApply();
  SkipRules();
  SkipBlocksHoldTheRange();
  StreetMix();
  SkipCost();
  SkipUnderTheGamesShadow();
  if (argc > 1) {
    ShaderConstantsMatch(argv[1]);
  } else {
    std::printf("(no hlsl folder given: the shaders' constants were not compared)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
