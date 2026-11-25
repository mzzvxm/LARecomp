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
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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
  // The linear depth of the pixel (x, y): the nearer of the ground and the boxes' first face. The vehicle bit is clear (the march reads
  // it: a vehicle's own pixels march a short ray), whatever the float's last bit would be by itself; DepthV is the one with the bit.
  float Depth(int x, int y) const {
    bool vehicle = false;
    return DepthWithVehicleBit(Hit(x, y, &vehicle), false);
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
    // The ground under the car is dark, but for the strip along the car's rear edge that the 60 degree sun reaches under the car's rear
    // (the sun's column is behind the rear face there: 0.25 m of clearance over tan 60 degrees is 14 cm, and the steps' jitter takes a bit
    // more): 93% here, 97% before the sun's column.
    Check(float(t.dark) > 0.9f * float(t.count), Num("with the term the ground under the car is dark (%.1f%%)", 100.0 * t.dark / std::max(t.count, 1)));
    Check(t.sum_new / float(t.count) < 0.1f, Num("... mean visibility %.3f", t.sum_new / float(t.count)));
    // At most four depth reads a pixel (two columns of two), the second column only where the first found a car, and none for most of the
    // picture's pixels (the sky, the car, a surface that is not horizontal).
    Check(loads <= 4 * pixels, "the term reads at most four pixels a pixel");
    Check(double(loads) < 2.0 * double(pixels), Num("... fewer than two on average (%.2f reads a pixel)", double(loads) / double(pixels)));
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
  Check(on.count > 150 && float(on.dark) > 0.9f * float(on.count), "the car's ground is dark with the term (no cast distance)");
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
  bool car_sees_mask = false;                  // true: the apply before the comb fix (a vehicle's pixel takes the mask like any other: the check's control)
  // The game's car sun image in the frame (CarAwareSunVisibility): a vehicle's pixel takes its value at the pixel's place; without one it takes 1.
  bool have_car_image = false;
  CarSunLookup car_lookup;
  std::function<float(float, float)> car_image;  // the image's bilinear .x at (u, v)
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
  const float mask_vis = Saturate(a.gain * SampleMask(mask, (float(x) + 0.5f) / float(cam.width), (float(y) + 0.5f) / float(cam.height), quant));
  // The game's mask is made before the cars are drawn: a vehicle's pixel takes the game's car sun image's value at its place, or 1 in a frame without the
  // image (CarAwareSunVisibility; ao_apply_ps.hlsl).
  const float sun_vis = a.car_sees_mask ? mask_vis
                                        : CarAwareSunVisibility(DepthIsVehicle(tc), mask_vis, a.have_car_image, a.car_lookup, pc,
                                                                [&a](float u, float v) { return a.car_image ? a.car_image(u, v) : 1.0f; });
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
      slot = MarchStored(trace, [&](int a, int b) { return im.At(a, b); }, x, y);
    }
    return slot;
  }
  bool Would(int x, int y) const { return would[size_t(y) * size_t(cam.width) + size_t(x)] != 0; }
};
SkipFrame MakeSkipFrameOf(const Camera& cam, DepthImage im, Vec3 to_sun, bool window) {
  SkipFrame f;
  f.cam = cam;
  f.im = std::move(im);
  f.trace.camera = f.cam;
  f.trace.to_sun = to_sun;
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
SkipFrame MakeSkipFrame(int height, bool window) {
  Scene s;
  s.cam = MakeCamera(height);
  s.boxes = AdaptScenes().back().boxes;  // the street
  return MakeSkipFrameOf(s.cam, RenderDepth(s), Sun(25.0, 100.0), window);
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
SkipResult RunSkip(SkipFrame& f, const MaskImage& mask, const ApplyConfig& a, int quant, int reach, bool exempt_vehicles = true) {
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
      // A vehicle's pixel and the pixels beside it are marched whatever the mask says (VehicleInReach: the apply gives a vehicle's pixel a sun
      // visibility of 1, and its filter reads the 8 neighbours).
      if (skipped && exempt_vehicles && VehicleInReach(depth, cam.width, cam.height, x, y)) {
        skipped = false;
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

// ---------------------------------------------------------------------------
// the thin parts of a car and the ground seen past a car (owner, 2026-11-18: three false shadows with the sun low)
// ---------------------------------------------------------------------------
//
// THE RIG. The scenes above look level along +z from the origin, which cannot show a chase camera looking down at a car's trunk or a
// pause menu's camera looking across it. This rig has world coordinates (y up, z forward, the road at y = 0), a camera at `eye` aimed at a
// target, boxes (the same Box, `vehicle` marks a car's parts) and cylinders (a tyre, axis along x), and it knows the TRUTH, which no
// screen-space march does: whether the sun's ray from a point of a surface meets anything, by exact ray tests. The depth image is the
// view-space z of the first hit with the vehicle bit, as the depth pass writes it.
struct Cyl {
  Vec3 centre;
  float radius = 0.33f, half_length = 0.14f;  // the axis is along x
  bool vehicle = true;
};

struct Rig {
  Camera cam;
  Vec3 eye;
  float yaw = 0.0f, pitch = 0.0f;  // radians; forward is (sin yaw cos pitch, -sin pitch, cos yaw cos pitch)
  std::vector<Box> boxes;
  std::vector<Cyl> cyls;

  void Aim(Vec3 target) {
    const Vec3 d = target - eye;
    yaw = std::atan2(d.x, d.z);
    pitch = std::atan2(-d.y, std::sqrt(d.x * d.x + d.z * d.z));
  }
  Vec3 Right() const { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }
  Vec3 Up() const { return {std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch)}; }
  Vec3 Forward() const { return {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)}; }
  Vec3 ToWorld(Vec3 v) const { return Right() * v.x + Up() * v.y + Forward() * v.z; }  // a direction, view space to world
  Vec3 ToView(Vec3 w) const { return {Dot(w, Right()), Dot(w, Up()), Dot(w, Forward())}; }

  static bool HitBox(Vec3 o, Vec3 d, const Box& b, float* t) {
    const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    float tn = 0.0f, tf = 1e30f;
    for (int a = 0; a < 3; ++a) {
      if (std::fabs(dd[a]) < 1e-9f) {
        if (oo[a] < lo[a] || oo[a] > hi[a]) {
          return false;
        }
        continue;
      }
      const float t1 = (lo[a] - oo[a]) / dd[a], t2 = (hi[a] - oo[a]) / dd[a];
      tn = std::max(tn, std::min(t1, t2));
      tf = std::min(tf, std::max(t1, t2));
    }
    *t = tn;
    return tn <= tf;
  }
  // A finite cylinder along x: the side and the two caps.
  static bool HitCyl(Vec3 o, Vec3 d, const Cyl& c, float tmin, float* t) {
    const float oy = o.y - c.centre.y, oz = o.z - c.centre.z, ox = o.x - c.centre.x;
    float best = 1e30f;
    const float a = d.y * d.y + d.z * d.z, b = 2.0f * (oy * d.y + oz * d.z), cc = oy * oy + oz * oz - c.radius * c.radius;
    if (a > 1e-12f) {
      const float disc = b * b - 4.0f * a * cc;
      if (disc >= 0.0f) {
        const float s = std::sqrt(disc);
        for (const float ts : {(-b - s) / (2.0f * a), (-b + s) / (2.0f * a)}) {
          if (ts > tmin && ts < best && std::fabs(ox + ts * d.x) <= c.half_length) {
            best = ts;
          }
        }
      }
    }
    if (std::fabs(d.x) > 1e-9f) {
      for (const float side : {-1.0f, 1.0f}) {
        const float ts = (side * c.half_length - ox) / d.x;
        const float py = oy + ts * d.y, pz = oz + ts * d.z;
        if (ts > tmin && ts < best && py * py + pz * pz <= c.radius * c.radius) {
          best = ts;
        }
      }
    }
    *t = best;
    return best < 1e29f;
  }
  // The nearest hit of o + t d, tmin < t < tmax (the road included): its t, or -1; `vehicle` says whether it is a car's part.
  float Nearest(Vec3 o, Vec3 d, float tmin, float tmax, bool* vehicle = nullptr, bool* road = nullptr) const {
    float best = tmax;
    bool hit = false, veh = false, on_road = false;
    if (d.y < -1e-9f) {
      const float t = -o.y / d.y;
      if (t > tmin && t < best) {
        best = t;
        hit = true;
        on_road = true;
      }
    }
    for (const Box& b : boxes) {
      float t;
      if (HitBox(o, d, b, &t) && t > tmin && t < best) {
        best = t;
        hit = true;
        veh = b.vehicle;
        on_road = false;
      }
    }
    for (const Cyl& c : cyls) {
      float t;
      if (HitCyl(o, d, c, tmin, &t) && t < best) {
        best = t;
        hit = true;
        veh = c.vehicle;
        on_road = false;
      }
    }
    if (vehicle) {
      *vehicle = hit && veh;
    }
    if (road) {
      *road = hit && on_road;
    }
    return hit ? best : -1.0f;
  }
  Vec3 PixelDirection(int x, int y) const { return ToWorld(ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, 1.0f)); }
};

struct RigImage {
  DepthImage depth;
  std::vector<uint8_t> road;  // 1: the pixel shows the road
};
RigImage RenderRig(const Rig& rig) {
  RigImage im;
  im.depth.cam = rig.cam;
  im.depth.d.resize(size_t(rig.cam.width) * size_t(rig.cam.height));
  im.road.assign(im.depth.d.size(), 0);
  for (int y = 0; y < rig.cam.height; ++y) {
    for (int x = 0; x < rig.cam.width; ++x) {
      bool vehicle = false, road = false;
      const float t = rig.Nearest(rig.eye, rig.PixelDirection(x, y), 0.0f, 60000.0f, &vehicle, &road);
      const size_t at = size_t(y) * size_t(rig.cam.width) + size_t(x);
      im.depth.d[at] = DepthWithVehicleBit(t < 0.0f ? 60000.0f : t, vehicle);
      im.road[at] = road ? 1 : 0;
    }
  }
  return im;
}

// The world position of the surface a pixel shows.
Vec3 RigPoint(const Rig& rig, const RigImage& im, int x, int y) {
  return rig.eye + rig.PixelDirection(x, y) * im.depth.At(x, y);
}

// What a march does over a rig's picture, against the truth: `false_dark` the pixels it darkens (visibility under 0.5) whose sun ray
// meets nothing within the ray's own length (`base.length`); `real` the pixels whose ray meets something within `real_length` (the
// contact shadows that must stay) and `real_dark` how many of those it darkens. Only pixels that face the sun, and of the receivers `which`.
struct RigScore {
  int pixels = 0, false_dark = 0, real = 0, real_dark = 0;
};
enum class Receiver { kAny, kVehicle, kOther };
template <class MarchFn>
RigScore ScoreRig(const Rig& rig, const RigImage& im, Vec3 sun_world, const Trace& base, Receiver which, float real_length, MarchFn&& march) {
  RigScore s;
  Trace t = base;
  t.camera = rig.cam;
  t.to_sun = rig.ToView(sun_world);
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  for (int y = 0; y < rig.cam.height; ++y) {
    for (int x = 0; x < rig.cam.width; ++x) {
      const float tc = im.depth.At(x, y);
      if (tc >= t.sky * 0.5f) {
        continue;
      }
      const bool vehicle = DepthIsVehicle(tc);
      if ((which == Receiver::kVehicle && !vehicle) || (which == Receiver::kOther && vehicle)) {
        continue;
      }
      const Vec3 pc = ViewPos(rig.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      Vec3 normal = NormalFromDepth(rig.cam, depth, x, y, tc, pc);
      if (Dot(normal, pc) > 0.0f) {
        normal = normal * -1.0f;
      }
      if (Dot(normal, t.to_sun) < 0.1f) {
        continue;  // turned from the sun: nothing to shadow
      }
      t.noise = PixelNoise(x, y);
      const float v = march(t, depth, x, y);
      const Vec3 p = RigPoint(rig, im, x, y) + sun_world * 0.012f;
      ++s.pixels;
      const bool dark = v < 0.5f;
      if (dark && rig.Nearest(p, sun_world, 0.0f, base.length) < 0.0f) {
        ++s.false_dark;
      }
      if (rig.Nearest(p, sun_world, 0.0f, real_length) >= 0.0f) {
        ++s.real;
        s.real_dark += dark ? 1 : 0;
      }
    }
  }
  return s;
}

// A Trace as the game's low sun makes it: the setting's 0.6 m grown by 0.7 / tan(elevation), 16 steps, thickness 0.3 m.
Trace LowSunTrace(double elevation_degrees) {
  Trace t;
  t.length = 0.6f * float(LengthGrow(elevation_degrees));
  t.thickness = 0.3f;
  t.steps = kStepsUltra;
  return t;
}
struct PlainMarch {
  template <class DepthAt>
  float operator()(const Trace& t, DepthAt&& depth, int x, int y) const {
    return March(t, depth, x, y);
  }
};

// The trunk of a car with a wing: a lid 1.5 m wide and 1.1 m long at 1 m (3 cm thick), the car's body behind it to z = 2.5, a plate
// (1.6 m wide, 30 cm chord, 4 cm thick) 28 cm above the lid on two 6 cm posts, the rear edge of the lid at z = 0. The Skyline's
// wing in the owner's pictures is about this.
Rig MakeWingRig(Vec3 eye, Vec3 target) {
  Rig r;
  r.cam = MakeCamera(720);
  r.eye = eye;
  r.boxes.push_back({-0.75f, 0.75f, 0.97f, 1.0f, 0.0f, 1.1f, true});   // the lid
  r.boxes.push_back({-0.8f, 0.8f, 0.35f, 0.97f, 0.0f, 2.5f, true});    // the body below it and the rear window's deck
  r.boxes.push_back({-0.8f, 0.8f, 1.28f, 1.32f, 0.15f, 0.45f, true});  // the plate
  for (const float x : {-0.5f, 0.5f}) {
    r.boxes.push_back({x - 0.03f, x + 0.03f, 1.0f, 1.28f, 0.25f, 0.31f, true});  // the posts
  }
  r.Aim(target);
  return r;
}

// A pixel of a car marches at most kVehicleReceiverSteps steps over at most kVehicleReceiverLength; any other pixel as the setting says.
void VehiclePixelsMarchShort() {
  Scene s;
  s.cam = MakeCamera(720);
  const auto plain = [&](int x, int y) { return DepthWithVehicleBit(s.Depth(x, y), false); };
  const auto car = [&](int x, int y) { return DepthWithVehicleBit(s.Depth(x, y), true); };
  Trace t;
  t.camera = s.cam;
  t.to_sun = Sun(18.0, 90.0);
  t.length = 1.3f;
  t.steps = kStepsUltra;
  t.noise = 0.5f;
  March(t, plain, 640, 600);
  const uint32_t steps_plain = t.steps_used;
  March(t, car, 640, 600);
  const uint32_t steps_car = t.steps_used;
  Check(steps_plain == kStepsUltra && steps_car == kVehicleReceiverSteps, Num("a car's pixel marches %.0f steps, any other %.0f", double(steps_car), double(steps_plain)));
  // The adaptive count measures the short ray, not the setting's: a pixel 39 m away, where a 0.3 m ray is 12 pixels long and the setting's 54,
  // keeps 4 steps (the floor) where the road's pixel keeps 14.
  t.adapt_pixels = kAdaptEndPixels;
  March(t, car, 640, 422);
  const uint32_t far_car = t.steps_used;
  March(t, plain, 640, 422);
  const uint32_t far_plain = t.steps_used;
  Check(far_car < far_plain && far_car >= kAdaptMinSteps, Num("far away the adaptive count reads the short ray: %.0f steps for a car's pixel, %.0f for the rest", double(far_car), double(far_plain)));
  // The ray stops at 0.3 m: a wall 0.5 m along the ray is not seen by a car's pixel (the ground in front of a wall facing the camera, the sun
  // behind the wall), and is by the road's.
  Scene w;
  w.cam = MakeCamera(720);
  w.boxes.push_back({-3.0f, 3.0f, -1.5f, 1.5f, 10.5f, 11.0f});
  Trace u;
  u.camera = w.cam;
  u.to_sun = Sun(45.0, 0.0);  // up and away from the camera, toward the wall
  u.length = 1.3f;
  u.steps = kStepsUltra;
  u.noise = 0.5f;
  const auto wall_plain = [&](int x, int y) { return DepthWithVehicleBit(w.Depth(x, y), false); };
  const auto wall_car = [&](int x, int y) { return DepthWithVehicleBit(w.Depth(x, y), true); };
  int seen_plain = 0, seen_car = 0;
  for (int y = 500; y < 700; y += 2) {
    for (int x = 400; x < 880; x += 3) {
      const float d = w.Depth(x, y);
      const Vec3 p = ViewPos(w.cam, float(x) + 0.5f, float(y) + 0.5f, d);
      if (std::fabs(p.y + 1.5f) > 1e-3f || p.z < 9.9f || p.z > 10.3f) {
        continue;  // the ground 0.2 to 0.6 m in front of the wall: its ray (45 degrees) meets the wall after 0.28 to 0.85 m
      }
      seen_plain += March(u, wall_plain, x, y) < 0.5f ? 1 : 0;
      seen_car += March(u, wall_car, x, y) < 0.5f ? 1 : 0;
    }
  }
  std::printf("vehicle pixels: steps %u against %u; far away (adaptive) %u against %u; a wall 0.3 to 0.9 m along the ray darkens %d road pixels, %d car pixels\n",
              steps_car, steps_plain, far_car, far_plain, seen_plain, seen_car);
  Check(seen_plain > 100 && seen_car * 4 < seen_plain, Num("a wall 0.3 to 0.9 m along the ray: dark for the road's pixels %.0f, for a car's %.0f", double(seen_plain), double(seen_car)));
}

// CASE 1: the sun low behind the camera, a chase camera behind and above the car: a broad black band across the trunk lid under the wing
// (the owner's pictures; with the sun behind the camera the wing's shadow falls forward onto the rear window, not on the lid).
//   The mechanism, on this rig: the lid pixel's ray runs toward the sun, which is toward the camera and up, and climbs on the screen. Its
//   late steps (0.7 to 1.1 m along a 1.3 m ray) are at the plate's height and just in front of the plate's far edge, 0.2 to 0.5 m beyond the
//   plate along the line of sight, where the camera sees the plate's rear face and top (it is looking down at 7 to 10 degrees) and the plate
//   hides the step on the screen: a step hidden by a surface that is nearer to the camera by less than the thickness (0.3 m) is "inside" it.
//   The plate is 4 cm thick, and 0.3 m along a view ray that grazes a horizontal surface is 3.7 cm of height: the thickness rule takes the
//   surface for a slab extruded along the view ray, which is right for a kerb and wrong for a thin plate, whose slab is empty air beyond its edge.
void WingBandOnTheTrunk() {
  const Vec3 sun = Sun(17.7, 180.0);  // behind the camera
  int old_false = 0, new_false = 0, old_real = 0, new_real = 0, real = 0;
  for (const Vec3 eye : {Vec3{0.0f, 1.55f, -3.5f}, Vec3{0.0f, 1.8f, -5.0f}, Vec3{0.0f, 2.1f, -5.0f}}) {
    const Rig rig = MakeWingRig(eye, {0.0f, 1.28f, 0.0f});
    const RigImage im = RenderRig(rig);
    Trace nw = LowSunTrace(17.7);
    Trace od = nw;
    od.vehicle_rule = false;
    const RigScore a = ScoreRig(rig, im, sun, od, Receiver::kVehicle, 0.15f, PlainMarch{});
    const RigScore b = ScoreRig(rig, im, sun, nw, Receiver::kVehicle, 0.15f, PlainMarch{});
    std::printf("wing from behind, camera %.2f m up %.1f m back: car pixels darkened with nothing blocking the sun within the ray: old %d, new %d; "
                "pixels with a shadow within 15 cm: %d, darkened old %d, new %d\n", double(eye.y), double(-eye.z), a.false_dark, b.false_dark, b.real, a.real_dark, b.real_dark);
    old_false += a.false_dark;
    new_false += b.false_dark;
    old_real += a.real_dark;
    new_real += b.real_dark;
    real += b.real;
  }
  Check(old_false > 1500, Num("the old march darkens the trunk under the wing: %.0f pixels with nothing blocking the sun", double(old_false)));
  Check(new_false * 50 <= old_false, Num("the new march leaves it lit: %.0f of the old %.0f", double(new_false), double(old_false)));
  Check(real > 100 && new_real * 10 >= old_real * 9, Num("the contact shadows on the car's own pixels within 15 cm stay: %.0f of the old %.0f", double(new_real), double(old_real)));
}

// CASE 2: the camera at the side of the car (the pause menu's), dashed streaks along the wing's edge and dark blobs at the bases of the
// uprights; right from some angles and broken from others. Mechanism: the same, from the side: a post (6 cm deep along the view) that stands
// 0.1 to 0.3 m nearer to the camera than the ray hides the step on the screen and counts as being in front of it by less than the thickness.
void WingFromTheSide() {
  int old_false = 0, new_false = 0, old_real_toward = 0, new_real_toward = 0, real_toward = 0;
  for (const float side : {3.0f, 4.5f}) {
    const Rig rig = MakeWingRig({-side, 1.5f, 0.4f}, {0.0f, 1.28f, 0.4f});
    const RigImage im = RenderRig(rig);
    int row_old = 0, row_new = 0;
    for (int az = 0; az < 360; az += 45) {
      const Vec3 sun = Sun(17.7, double(az));
      Trace nw = LowSunTrace(17.7);
      Trace od = nw;
      od.vehicle_rule = false;
      const RigScore a = ScoreRig(rig, im, sun, od, Receiver::kVehicle, 0.15f, PlainMarch{});
      const RigScore b = ScoreRig(rig, im, sun, nw, Receiver::kVehicle, 0.15f, PlainMarch{});
      row_old += a.false_dark;
      row_new += b.false_dark;
      if (az == 270) {  // the sun on the camera's side of the car, shining along the plate's length at the posts: their real shadows
        old_real_toward += a.real_dark;
        new_real_toward += b.real_dark;
        real_toward += b.real;
      }
    }
    std::printf("wing from the side, %.1f m away, the sun at 17.7 degrees from 8 directions: false shadow on the car's pixels: old %d, new %d\n", double(side), row_old, row_new);
    old_false += row_old;
    new_false += row_new;
  }
  Check(old_false > 20000, Num("the old march streaks the wing and blobs its posts from the side: %.0f false pixels over 16 views", double(old_false)));
  Check(new_false * 8 <= old_false, Num("the new march: %.0f of the old %.0f", double(new_false), double(old_false)));
  Check(real_toward > 500 && new_real_toward * 10 >= old_real_toward * 9,
        Num("the posts' own shadows on the lid, the sun shining along the plate at them, stay: %.0f of the old %.0f", double(new_real_toward), double(old_real_toward)));
}

// The small shadows that must stay: a mirror on a door (a vehicle receiver, 14 cm proud of the door), and a car's tyres and sills on the road
// (a vehicle's shadow on a receiver that is not one: the vehicle rule leaves those pixels as they were, to the bit).
Rig MakeMirrorRig() {
  Rig r;
  r.cam = MakeCamera(720);
  r.eye = {-3.0f, 1.3f, 0.2f};
  r.boxes.push_back({-0.9f, 0.9f, 0.22f, 1.2f, 0.0f, 4.5f, true});      // the car
  r.boxes.push_back({-1.04f, -0.9f, 0.95f, 1.15f, 1.3f, 1.5f, true});   // the mirror on its left side
  r.Aim({-0.9f, 1.0f, 1.4f});
  return r;
}
Rig MakeTyreRig() {
  Rig r;
  r.cam = MakeCamera(720);
  r.eye = {3.0f, 1.0f, -6.0f};
  r.boxes.push_back({-0.9f, 0.9f, 0.22f, 1.2f, 0.0f, 4.5f, true});
  for (const float x : {-0.78f, 0.78f}) {
    for (const float z : {0.7f, 3.8f}) {
      r.cyls.push_back({{x, 0.33f, z}, 0.33f, 0.14f, true});
    }
  }
  r.Aim({0.0f, 0.5f, 3.0f});
  return r;
}

void SmallShadowsStay() {
  // The mirror: the sun low and high, from behind and from the side of the camera.
  {
    const Rig rig = MakeMirrorRig();
    const RigImage im = RenderRig(rig);
    int old_real = 0, new_real = 0, real = 0, old_false = 0, new_false = 0;
    for (const double elevation : {20.0, 40.0}) {
      for (const double az : {135.0, 270.0}) {
        const Vec3 sun = Sun(elevation, az);
        Trace nw = LowSunTrace(elevation);
        Trace od = nw;
        od.vehicle_rule = false;
        const RigScore a = ScoreRig(rig, im, sun, od, Receiver::kVehicle, 0.15f, PlainMarch{});
        const RigScore b = ScoreRig(rig, im, sun, nw, Receiver::kVehicle, 0.15f, PlainMarch{});
        old_real += a.real_dark;
        new_real += b.real_dark;
        real += b.real;
        old_false += a.false_dark;
        new_false += b.false_dark;
      }
    }
    std::printf("mirror on a door, 4 suns: shadow within 15 cm: %d pixels, darkened old %d, new %d; false: old %d, new %d\n", real, old_real, new_real, old_false, new_false);
    Check(real > 2000, Num("the mirror scene has shadows within 15 cm (%.0f pixels)", double(real)));
    Check(new_real * 100 >= old_real * 90, Num("the mirror's shadow on the door stays: %.0f of the old %.0f pixels", double(new_real), double(old_real)));
    Check(new_false <= old_false + old_false / 20, "and the mirror scene gets no new false shadow");
  }
  // The tyres and the sills on the road: every road pixel is exactly what it was, and the shadows within 15 cm are dark.
  {
    const Rig rig = MakeTyreRig();
    const RigImage im = RenderRig(rig);
    int differing = 0, real = 0, real_dark = 0, pixels = 0;
    for (const double elevation : {15.0, 30.0}) {
      for (const double az : {0.0, 90.0, 180.0, 270.0}) {
        const Vec3 sun = Sun(elevation, az);
        Trace nw = LowSunTrace(elevation);
        Trace od = nw;
        od.vehicle_rule = false;
        const RigScore b = ScoreRig(rig, im, sun, nw, Receiver::kOther, 0.15f, PlainMarch{});
        real += b.real;
        real_dark += b.real_dark;
        pixels += b.pixels;
        nw.camera = od.camera = rig.cam;
        nw.to_sun = od.to_sun = rig.ToView(sun);
        const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
        for (int y = 0; y < rig.cam.height; y += 2) {
          for (int x = 0; x < rig.cam.width; x += 2) {
            if (DepthIsVehicle(im.depth.At(x, y))) {
              continue;
            }
            nw.noise = od.noise = PixelNoise(x, y);
            differing += March(nw, depth, x, y) != March(od, depth, x, y) ? 1 : 0;
          }
        }
      }
    }
    std::printf("car and tyres on the road, 8 suns: %d road pixels, shadow within 15 cm of the car: %d, dark %d\n", pixels, real, real_dark);
    Check(differing == 0, Num("the road around a car is exactly as it was: %.0f pixels differ", double(differing)));
    Check(real > 3000 && real_dark * 100 >= real * 90, Num("the shadows of the tyres and sills on the road stay dark: %.0f of %.0f", double(real_dark), double(real)));
  }
}

// CASE 3: the ground under a car, and the ground beyond it. The owner's photo (an Audi R8, the camera in front of the car on the left and
// low, the sun behind the car) and a pause-menu picture of the player's R34 (the same set-up): a dark slab rises from the sunlit road right
// behind the rear tyre. It is gone with contact shadows off and with only the under-a-vehicle term off; the march does not make it.
//   The mechanism: a ground pixel looks up the column above it and, when the pixel the column falls on is a car's and the point is behind
//   that shell by less than kUnderReach, calls itself inside the car. A road pixel that the camera sees PAST the car (beyond its far end or
//   through the gap under it, from a low camera) is behind the shell too, and so is every ground pixel beyond the car seen from high above;
//   a depth image cannot tell "under the car" from "beyond the car" (the underside is not in it), so the column alone says "under" for both.
//   What tells them apart is the sun: the ground is under the car when the sun's ray from it is blocked by the car, and a ray that runs
//   away from the car is not. The second column follows that ray (the same heights up it): both must find the car. That takes the false
//   shadow away where the sun is not at the car's end (and everywhere with a camera above the car), at the price of the strip along the
//   sun's edge of the car that the low sun really does reach under it. What is left is the strip of road past the car's far end, seen
//   through the gap, when the sun is straight behind the car: for the PLAYER's car the game's own shadow map has the right answer and the
//   term stands aside (kPlayerCarCastDistance: the host passes at least that distance), for a far traffic car the strip is small.
struct UnderScore {
  int pixels = 0, dark = 0, dark_blocked = 0, dark_open = 0, blocked = 0;
};
// The term over the road pixels of a rig against the truth: `blocked` the road pixels whose sun ray meets a car within 12 m, `dark` those
// the term darkens (over 0.5), `dark_open` the ones of those whose ray is free.
UnderScore ScoreUnder(const Rig& rig, const RigImage& im, Vec3 sun_world, bool sun_column, float cast_distance) {
  UnderScore s;
  Trace t;
  t.camera = rig.cam;
  t.to_sun = rig.ToView(sun_world);
  t.up = rig.ToView({0.0f, 1.0f, 0.0f});
  t.under = 1.0f;
  t.sun_column = sun_column;
  t.cast_distance = cast_distance;
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  for (int y = 0; y < rig.cam.height; ++y) {
    for (int x = 0; x < rig.cam.width; ++x) {
      if (!im.road[size_t(y) * size_t(rig.cam.width) + size_t(x)]) {
        continue;
      }
      const float tc = im.depth.At(x, y);
      const Vec3 pc = ViewPos(rig.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      Vec3 normal = NormalFromDepth(rig.cam, depth, x, y, tc, pc);
      if (Dot(normal, pc) > 0.0f) {
        normal = normal * -1.0f;
      }
      t.noise = PixelNoise(x, y);
      const bool dark = UnderVehicle(t, depth, tc, pc, normal) > 0.5f;
      bool vehicle = false;
      const bool blocked = rig.Nearest(RigPoint(rig, im, x, y) + sun_world * 0.012f, sun_world, 0.0f, 12.0f, &vehicle) >= 0.0f && vehicle;
      ++s.pixels;
      s.blocked += blocked ? 1 : 0;
      s.dark += dark ? 1 : 0;
      s.dark_blocked += dark && blocked ? 1 : 0;
      s.dark_open += dark && !blocked ? 1 : 0;
    }
  }
  return s;
}

// A car 1.8 m wide and 4.5 m long, 25 cm off the road to 1.4 m, with four tyres, its near end at z = `z0`.
Rig MakeCarRig(Vec3 eye, Vec3 target, float z0, bool tyres) {
  Rig r;
  r.cam = MakeCamera(720);
  r.eye = eye;
  r.boxes.push_back({-0.9f, 0.9f, 0.25f, 1.4f, z0, z0 + 4.5f, true});
  if (tyres) {
    for (const float x : {-0.78f, 0.78f}) {
      for (const float z : {z0 + 0.7f, z0 + 3.8f}) {
        r.cyls.push_back({{x, 0.33f, z}, 0.33f, 0.14f, true});
      }
    }
  }
  r.Aim(target);
  return r;
}

void GroundBeyondACar() {
  // (1) From above and behind, 4 m up, the ground past the car's far end is on the screen above its roof: the vertical column alone calls it
  //     under the car for a low sun (9000 and 11000 pixels at 30 and 18 degrees, where the sun's ray reaches nothing).
  {
    const Rig rig = MakeCarRig({0.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 10.0f}, 8.0f, false);
    const RigImage im = RenderRig(rig);
    int old_open = 0, new_open = 0;
    for (const double elevation : {30.0, 18.0}) {
      const Vec3 sun = Sun(elevation, 180.0);
      const UnderScore a = ScoreUnder(rig, im, sun, false, 0.0f);
      const UnderScore b = ScoreUnder(rig, im, sun, true, 0.0f);
      std::printf("camera 4 m up behind the car, sun %.0f degrees behind the camera: ground called under the car with the sun free: column alone %d, with the sun's column %d; "
                  "ground the car really shades %d, called under: column alone %d, with the sun's column %d\n",
                  elevation, a.dark_open, b.dark_open, b.blocked, a.dark_blocked, b.dark_blocked);
      old_open += a.dark_open;
      new_open += b.dark_open;
    }
    Check(old_open > 8000, Num("from above, the vertical column darkens the open ground beyond the car: %.0f pixels", double(old_open)));
    Check(new_open * 50 <= old_open, Num("the sun's column leaves it lit: %.0f of %.0f", double(new_open), double(old_open)));
  }
  // (2) The owner's set-up: the camera in front of the car on the left and low, the sun behind the car 30 degrees off its axis (as the sheets'
  //     front-wheel picture is), at 18 degrees: the road beside and past the car is lit.
  {
    const Rig rig = MakeCarRig({3.0f, 1.0f, 3.0f}, {0.0f, 0.6f, 10.0f}, 8.0f, true);
    const RigImage im = RenderRig(rig);
    int old_open = 0, new_open = 0, old_true = 0, new_true = 0;
    for (const double az : {30.0, 60.0}) {
      const Vec3 sun = Sun(18.0, az);
      const UnderScore a = ScoreUnder(rig, im, sun, false, 0.0f);
      const UnderScore b = ScoreUnder(rig, im, sun, true, 0.0f);
      std::printf("camera at the front left, low, sun 18 degrees at %.0f degrees to the car's axis: ground called under the car with the sun free: column alone %d, with the sun's column %d; "
                  "ground really under the car and called so: column alone %d, with the sun's column %d\n",
                  az, a.dark_open, b.dark_open, a.dark_blocked, b.dark_blocked);
      old_open += a.dark_open;
      new_open += b.dark_open;
      old_true += a.dark_blocked;
      new_true += b.dark_blocked;
    }
    Check(old_open > 5000, Num("the column alone darkens the road beside the car: %.0f pixels", double(old_open)));
    Check(new_open * 20 <= old_open, Num("the sun's column leaves it lit: %.0f of %.0f", double(new_open), double(old_open)));
    Check(new_true * 2 >= old_true, Num("and keeps the ground under the car (%.0f of the column alone's %.0f pixels)", double(new_true), double(old_true)));
  }
  // (3) The sun straight behind the car, the same camera: the slab. The car is the player's (4 to 11 m from the camera, within kPlayerCarCastDistance
  //     plus its band): the term stands aside and nothing is darkened. A traffic car 20 m off keeps it (and its strip past the far end is small).
  {
    const Vec3 sun = Sun(18.0, 0.0);
    const Rig near_rig = MakeCarRig({3.0f, 1.0f, 3.0f}, {0.0f, 0.6f, 10.0f}, 5.0f, true);
    const RigImage near_im = RenderRig(near_rig);
    const UnderScore with = ScoreUnder(near_rig, near_im, sun, true, 0.0f);
    const UnderScore floor = ScoreUnder(near_rig, near_im, sun, true, UnderCastDistance(0.0f));
    std::printf("camera at the front left, low, the sun straight behind the car, the car 4 to 11 m off: term on %d pixels (%d with the sun free), with the player's car left out %d\n",
                with.dark, with.dark_open, floor.dark);
    Check(with.dark_open > 1000, Num("without the floor the slab is there: %.0f road pixels darkened with the sun free", double(with.dark_open)));
    Check(floor.dark == 0 && floor.dark_open == 0, Num("with the player's car left out nothing is darkened (%.0f)", double(floor.dark)));
    const Rig far_rig = MakeCarRig({3.0f, 1.0f, -14.0f}, {0.0f, 0.6f, 4.0f}, 6.0f, true);
    const RigImage far_im = RenderRig(far_rig);
    const UnderScore traffic = ScoreUnder(far_rig, far_im, sun, true, UnderCastDistance(0.0f));
    std::printf("the same car 20 to 29 m off, left in by the floor: term on %d pixels, %d of them under the car, %d with the sun free\n", traffic.dark, traffic.dark_blocked, traffic.dark_open);
    Check(traffic.dark_blocked > 400, Num("a traffic car beyond the floor keeps its dark ground under it (%.0f pixels)", double(traffic.dark_blocked)));
  }
  // (4) The floor itself.
  Check(UnderCastDistance(0.0f) == kPlayerCarCastDistance && UnderCastDistance(5.0f) == kPlayerCarCastDistance && UnderCastDistance(40.0f) == 40.0f,
        "the distance the host passes is the traffic setting's, but never under the player's car's");
  Check(UnderCastWeight(8.0f, UnderCastDistance(0.0f)) == 0.0f && UnderCastWeight(25.0f, UnderCastDistance(0.0f)) == 1.0f,
        "the player's car (8 m) is left out of the term and a car at 25 m keeps it");
}

// ---------------------------------------------------------------------------
// the player's car's own sun shadow map (contact_trace.h CarMapShadow, and the march with a map)
// ---------------------------------------------------------------------------
//
// The map is made here the way car_shadow_map.cpp makes it: the parts of a rig marked `vehicle` (and its cylinders) rendered from the sun, depth
// only, both faces, into a square window fitted to the car and a margin round it, 0 at the sun's end of the car's range and 1 where nothing is,
// and with the pipeline's slope scaled bias (the stored depth pushed away from the sun by kCarMapProducerSlope times the larger of the surface's
// depth changes per texel along u and v: the same number as car_shadow_map_rules.h kSlopeScaledDepthBias). The twin then answers for every pixel
// of the rig's picture, and the truth is an exact ray test against the car's parts. A pixel is an INTERIOR one when no pixel of the picture that
// is within a few texels of it in space (and within the sun's penumbra at the farthest blocker the map holds) has another truth: the edges of a
// shadow are not judged here, what is inside a shadow and what is in the light is.

struct CarHit {
  float t = -1.0f;
  Vec3 normal;  // of the surface the ray enters, facing the ray's origin
};

// The nearest part of the car along o + t d (the road is not in the map), with the normal of the face it enters.
CarHit NearestCarHit(const Rig& rig, Vec3 o, Vec3 d) {
  CarHit best;
  float best_t = 1e30f;
  for (const Box& b : rig.boxes) {
    if (!b.vehicle) {
      continue;
    }
    const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    float tn = 0.0f, tf = 1e30f;
    int axis = -1;
    bool miss = false;
    for (int a = 0; a < 3; ++a) {
      if (std::fabs(dd[a]) < 1e-9f) {
        miss = miss || oo[a] < lo[a] || oo[a] > hi[a];
        continue;
      }
      const float t1 = (lo[a] - oo[a]) / dd[a], t2 = (hi[a] - oo[a]) / dd[a];
      if (std::min(t1, t2) > tn) {
        tn = std::min(t1, t2);
        axis = a;
      }
      tf = std::min(tf, std::max(t1, t2));
    }
    if (!miss && tn <= tf && tn < best_t) {
      best_t = tn;
      float n[3] = {0.0f, 0.0f, 0.0f};
      if (axis >= 0) {
        n[axis] = dd[axis] > 0.0f ? -1.0f : 1.0f;
      } else {
        n[0] = -dd[0];  // the origin is inside the box: a hit at 0 (a point on a surface's plane that is inside a part of the car)
        n[1] = -dd[1];
        n[2] = -dd[2];
      }
      best.t = tn;
      best.normal = {n[0], n[1], n[2]};
    }
  }
  for (const Cyl& c : rig.cyls) {
    float t;
    if (c.vehicle && Rig::HitCyl(o, d, c, 0.0f, &t) && t >= 0.0f && t < best_t) {
      best_t = t;
      const Vec3 p = o + d * t;
      best.t = t;
      if (std::fabs(p.x - c.centre.x) >= c.half_length - 1e-4f) {
        best.normal = {p.x > c.centre.x ? 1.0f : -1.0f, 0.0f, 0.0f};
      } else {
        const float ny = p.y - c.centre.y, nz = p.z - c.centre.z;
        const float len = std::sqrt(ny * ny + nz * nz);
        best.normal = {0.0f, ny / len, nz / len};
      }
    }
  }
  return best;
}

// The distance to the nearest part of the car along o + t d, or -1.
float NearestCar(const Rig& rig, Vec3 o, Vec3 d) { return NearestCarHit(rig, o, d).t; }

struct CarMapRig {
  CarMapView view;
  std::vector<float> texels;
  Vec3 sun;  // world, unit, toward the sun
};

// The map of the rig's car from `sun` (world): a window of the car's extent plus `margin` m a side, `size` texels, the depth range the car's
// extent along the sun plus 0.4 m. The matrix is in the rig's VIEW space, as the contract says. `producer_slope`: the pipeline's slope scaled bias.
CarMapRig BuildCarMap(const Rig& rig, Vec3 sun, int size, float margin, float producer_slope = kCarMapProducerSlope) {
  CarMapRig m;
  m.sun = sun;
  const Vec3 ref = std::fabs(sun.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
  Vec3 right = Cross(ref, sun);
  right = right * (1.0f / std::sqrt(Dot(right, right)));
  const Vec3 down_axis = Cross(sun, right);  // unit: the window's second axis (v runs against it)
  float rmin = 1e30f, rmax = -1e30f, umin = 1e30f, umax = -1e30f, lmin = 1e30f, lmax = -1e30f;
  const auto add = [&](Vec3 p) {
    rmin = std::min(rmin, Dot(p, right));
    rmax = std::max(rmax, Dot(p, right));
    umin = std::min(umin, Dot(p, down_axis));
    umax = std::max(umax, Dot(p, down_axis));
    lmin = std::min(lmin, Dot(p, sun));
    lmax = std::max(lmax, Dot(p, sun));
  };
  for (const Box& b : rig.boxes) {
    if (b.vehicle) {
      for (int i = 0; i < 8; ++i) {
        add({(i & 1) ? b.x1 : b.x0, (i & 2) ? b.y1 : b.y0, (i & 4) ? b.z1 : b.z0});
      }
    }
  }
  for (const Cyl& c : rig.cyls) {
    if (c.vehicle) {
      for (int i = 0; i < 8; ++i) {
        add({c.centre.x + ((i & 1) ? c.half_length : -c.half_length), c.centre.y + ((i & 2) ? c.radius : -c.radius),
             c.centre.z + ((i & 4) ? c.radius : -c.radius)});
      }
    }
  }
  const float side = std::max(rmax - rmin, umax - umin) + 2.0f * margin;
  const float r0 = 0.5f * (rmin + rmax) - 0.5f * side, u1 = 0.5f * (umin + umax) + 0.5f * side;
  const float range = (lmax - lmin) + 0.4f, l0 = lmax + 0.2f;
  const float texel = side / float(size);
  m.texels.assign(size_t(size) * size_t(size), 1.0f);
  for (int ty = 0; ty < size; ++ty) {
    for (int tx = 0; tx < size; ++tx) {
      const float r = r0 + (float(tx) + 0.5f) * texel, uu = u1 - (float(ty) + 0.5f) * texel;
      const Vec3 o = right * r + down_axis * uu + sun * (l0 + 0.5f);
      const CarHit h = NearestCarHit(rig, o, sun * -1.0f);
      if (h.t < 0.0f) {
        continue;
      }
      // The surface's depth change per texel along u (right) and v (against down_axis): texel * (n . axis) / (n . sun); the pipeline's slope scaled
      // bias pushes the stored depth deeper by `producer_slope` times the larger of the two.
      const float facing = std::max(Dot(h.normal, sun), 1e-3f);
      const float slope = texel * std::max(std::fabs(Dot(h.normal, right)), std::fabs(Dot(h.normal, down_axis))) / facing;
      m.texels[size_t(ty) * size_t(size) + size_t(tx)] = (h.t - 0.5f + producer_slope * slope) / range;
    }
  }
  // u = (p_world . right - r0) / side, v = (u1 - p_world . down_axis) / side, depth = (l0 - p_world . sun) / range, p_world = eye + ToWorld(p_view).
  const auto row = [&](Vec3 axis, float scale, float constant, float* out) {
    const Vec3 a = rig.ToView(axis);
    out[0] = a.x * scale;
    out[1] = a.y * scale;
    out[2] = a.z * scale;
    out[3] = constant;
  };
  row(right, 1.0f / side, (Dot(rig.eye, right) - r0) / side, m.view.to_map[0]);
  row(down_axis, -1.0f / side, (u1 - Dot(rig.eye, down_axis)) / side, m.view.to_map[1]);
  row(sun, -1.0f / range, (l0 - Dot(rig.eye, sun)) / range, m.view.to_map[2]);
  m.view.texel_m = texel;
  m.view.depth_m = range;
  m.view.size = size;
  m.view.texels = m.texels.data();
  return m;
}

// What the map says at a world point `p` of a surface with the world normal `n` (the camera's footprint there given).
CarMapResult MapAtWorld(const Rig& rig, const CarMapRig& map, Vec3 p, Vec3 n, float footprint, float noise) {
  const Vec3 pv = rig.ToView(p - rig.eye);
  return CarMapShadow(map.view, pv, rig.ToView(n), footprint, noise);
}

struct CarMapTally {
  long pixels = 0, edge = 0;
  long lit = 0, acne = 0, acne_strong = 0, false_dark = 0;  // interior pixels of the light: the term over 0.02, over 0.3, over 0.5
  long shadow = 0, missed = 0, near = 0, near_missed = 0;   // interior pixels of a shadow: the term under 0.5; `near` those whose blocker is within 15 cm
  long vehicle = 0, vehicle_covered = 0, other_covered = 0;
  long evals = 0, gathers = 0;
  int worst_gathers = 0;
  double truth_area = 0.0, map_area = 0.0;  // over the judged pixels: the shadow's size by the truth and by the term
};
CarMapTally& operator+=(CarMapTally& a, const CarMapTally& b) {
  a.pixels += b.pixels;
  a.edge += b.edge;
  a.lit += b.lit;
  a.acne += b.acne;
  a.acne_strong += b.acne_strong;
  a.false_dark += b.false_dark;
  a.shadow += b.shadow;
  a.missed += b.missed;
  a.near += b.near;
  a.near_missed += b.near_missed;
  a.vehicle += b.vehicle;
  a.vehicle_covered += b.vehicle_covered;
  a.other_covered += b.other_covered;
  a.evals += b.evals;
  a.gathers += b.gathers;
  a.worst_gathers = std::max(a.worst_gathers, b.worst_gathers);
  a.truth_area += b.truth_area;
  a.map_area += b.map_area;
  return a;
}

// The shadow term over a rig's picture against the truth: every second pixel in each direction that faces the sun by at least 0.1 and falls inside the
// map's window (14 texels in from its border: the fade, the filter's ring and the search's). `term(x, y, tc, pc, normal, footprint, &covered, &gathers)` is the value judged, 0 lit to 1 in shadow.
// `only_vehicle`: judge the car's own pixels alone. `fixed_edge_m` > 0: the zone round a change of the truth that is not judged, in metres (else the
// map's own: three texels, a centimetre and the sun's penumbra at the farthest blocker).
template <class TermFn>
void ScoreShadow(const Rig& rig, const RigImage& im, const CarMapRig& map, TermFn&& term, CarMapTally* tally, bool only_vehicle = false, float fixed_edge_m = 0.0f) {
  const Camera& cam = rig.cam;
  const Vec3 sun_world = map.sun;
  const Vec3 sun_view = rig.ToView(sun_world);
  const int sw = (cam.width + 1) / 2, sh = (cam.height + 1) / 2;
  const size_t n = size_t(sw) * size_t(sh);
  std::vector<float> value(n, -1.0f), footprint(n, 0.0f), facing_of(n, 0.0f), blocker(n, -1.0f);
  std::vector<uint8_t> truth(n, 0);
  std::vector<Vec3> where(n), normals(n);
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  const float guard = 14.0f / float(map.view.size);
  for (int sy = 0; sy < sh; ++sy) {
    for (int sx = 0; sx < sw; ++sx) {
      const int x = sx * 2, y = sy * 2;
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f || (only_vehicle && !DepthIsVehicle(tc))) {
        continue;
      }
      const Vec3 pc = ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, tc);
      Vec3 normal = NormalFromDepth(cam, depth, x, y, tc, pc);
      if (Dot(normal, pc) > 0.0f) {
        normal = normal * -1.0f;
      }
      if (Dot(normal, sun_view) < 0.1f) {
        continue;
      }
      const float u = pc.x * map.view.to_map[0][0] + pc.y * map.view.to_map[0][1] + pc.z * map.view.to_map[0][2] + map.view.to_map[0][3];
      const float v = pc.x * map.view.to_map[1][0] + pc.y * map.view.to_map[1][1] + pc.z * map.view.to_map[1][2] + map.view.to_map[1][3];
      if (u < guard || v < guard || u > 1.0f - guard || v > 1.0f - guard) {
        continue;
      }
      const size_t at = size_t(sy) * size_t(sw) + size_t(sx);
      const float fp = FootprintUnit(cam.tan_y, float(cam.height)) * tc;
      bool covered = false;
      int gathers = 0;
      value[at] = term(x, y, tc, pc, normal, fp, &covered, &gathers);
      where[at] = RigPoint(rig, im, x, y);
      normals[at] = normal;
      blocker[at] = NearestCar(rig, where[at] + sun_world * 0.003f, sun_world);
      truth[at] = blocker[at] >= 0.0f ? 1 : 0;
      footprint[at] = fp;
      facing_of[at] = Dot(normal, sun_view);
      ++tally->evals;
      tally->gathers += gathers;
      tally->worst_gathers = std::max(tally->worst_gathers, gathers);
      if (DepthIsVehicle(tc)) {
        ++tally->vehicle;
        tally->vehicle_covered += covered ? 1 : 0;
      } else {
        tally->other_covered += covered ? 1 : 0;
      }
    }
  }
  for (int sy = 0; sy < sh; ++sy) {
    for (int sx = 0; sx < sw; ++sx) {
      const size_t at = size_t(sy) * size_t(sw) + size_t(sx);
      if (value[at] < 0.0f) {
        continue;
      }
      ++tally->pixels;
      tally->truth_area += truth[at];
      tally->map_area += value[at];
      // The zone round a change of the truth that is not judged: three texels and a centimetre, and the sun's whole penumbra at the farthest
      // blocker the map holds (the sun's diameter 0.0088 times the depth range, stretched over a surface turned from the sun by 1 / cos).
      const float edge_m = fixed_edge_m > 0.0f ? fixed_edge_m : 3.0f * map.view.texel_m + 0.01f + 0.0088f * map.view.depth_m / std::max(facing_of[at], 0.1f);
      // An edge is a change of the truth anywhere on the pixel's own surface plane (extended past the face: a step of the geometry is an edge too)
      // within edge_m: the truth is asked at three rings of eight points round the pixel, on the plane of its normal.
      bool edge = false;
      {
        const Vec3 nw = rig.ToWorld(normals[at]);
        const Vec3 ref = std::fabs(nw.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        Vec3 t1 = Cross(nw, ref);
        t1 = t1 * (1.0f / std::sqrt(Dot(t1, t1)));
        const Vec3 t2 = Cross(nw, t1);
        for (int ring = 1; ring <= 3 && !edge; ++ring) {
          for (int k = 0; k < 8 && !edge; ++k) {
            const float angle = float(k) * 0.78539816339f;
            const Vec3 q = where[at] + (t1 * std::cos(angle) + t2 * std::sin(angle)) * (edge_m * float(ring) / 3.0f);
            const bool shadowed = NearestCar(rig, q + sun_world * 0.003f, sun_world) >= 0.0f;
            edge = shadowed != (truth[at] != 0);
          }
        }
      }
      if (edge) {
        ++tally->edge;
      } else if (truth[at]) {
        ++tally->shadow;
        tally->missed += value[at] < 0.5f ? 1 : 0;
        if (blocker[at] <= 0.15f) {
          ++tally->near;
          tally->near_missed += value[at] < 0.5f ? 1 : 0;
        }
      } else {
        ++tally->lit;
        tally->acne += value[at] > 0.02f ? 1 : 0;
        tally->acne_strong += value[at] > 0.3f ? 1 : 0;
        tally->false_dark += value[at] > 0.5f ? 1 : 0;
      }
    }
  }
}

// The map's own term over a rig's picture (the twin of the shader's CarMapShadow, no march).
void ScoreCarMap(const Rig& rig, const RigImage& im, const CarMapRig& map, CarMapTally* tally) {
  ScoreShadow(rig, im, map, [&](int x, int y, float, Vec3 pc, Vec3 normal, float fp, bool* covered, int* gathers) {
    const CarMapResult r = CarMapShadow(map.view, pc, normal, fp, PixelNoise(x, y));
    *covered = r.covered;
    *gathers = r.gathers;
    return r.shadow;
  }, tally);
}

// THE FILTER FOLLOWS THE WORLD'S (contact_trace.h): on a straight edge of a flat blocker (the map's left half holds it at depth 0.5, the right half is empty)
// the soft disk is a smooth ramp of the world's width, the pcss disk grows with the blocker's distance, neither turns without a temporal upscaler, and a
// pixel far from every blocker costs the search alone. The map has 1 cm texels here (a soft radius of 3 texels).
void CarMapWorldFilter() {
  std::vector<float> texels(64 * 64, 1.0f);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 32; ++x) {
      texels[size_t(y) * 64 + size_t(x)] = 0.5f;
    }
  }
  CarMapView m;
  m.size = 64;
  m.texel_m = 0.01f;
  m.depth_m = 1.0f;
  m.texels = texels.data();
  m.to_map[0][0] = 1.0f / (64.0f * 0.01f);
  m.to_map[1][1] = -1.0f / (64.0f * 0.01f);
  m.to_map[1][3] = 1.0f;
  m.to_map[2][2] = 1.0f;
  const float side = 0.64f;
  const Vec3 n{0.0f, 0.0f, -1.0f};
  // The edge is at u = 0.5 (x = 0.32 m). Walk the receiver across it in quarter texels; `depth` is the receiver's z, the blocker's is 0.5.
  const auto ramp = [&](CarMapView view, float depth, float noise, std::vector<float>* out) {
    out->clear();
    for (int i = -32; i <= 32; ++i) {
      const float x = 0.5f * side + float(i) * 0.0025f;  // quarter texels
      out->push_back(CarMapShadow(view, {x, 0.5f * side, depth}, n, 0.0005f, noise).shadow);
    }
  };
  const auto width = [&](const std::vector<float>& r) {  // the 10 to 90 percent width, in texels
    int first = -1, last = -1;
    for (size_t i = 0; i < r.size(); ++i) {
      if (first < 0 && r[i] < 0.9f) first = int(i);
      if (r[i] > 0.1f) last = int(i);
    }
    return first < 0 || last < first ? 0.0f : float(last - first) * 0.25f;
  };
  std::vector<float> a, b;
  // soft: a smooth, monotonic ramp about 1.36 radii wide (3 texels each) at any blocker distance.
  CarMapView soft = m;
  soft.filter_mode = kCarMapFilterSoft;
  ramp(soft, 0.6f, 0.37f, &a);
  bool monotonic = true;
  for (size_t i = 1; i < a.size(); ++i) {
    monotonic = monotonic && a[i] <= a[i - 1] + 1e-4f;
  }
  Check(monotonic && a.front() > 0.97f && a.back() < 0.03f, Num("soft: a monotonic ramp from shadow to light across the edge (%.3f to %.3f)", double(a.front()), double(a.back())));
  const float soft_width = width(a);
  Check(soft_width > 2.5f && soft_width < 5.5f, Num("soft: the 10 to 90 percent width is about 1.4 radii of 3 texels: %.2f texels", double(soft_width)));
  float steepest = 0.0f;
  for (size_t i = 1; i < a.size(); ++i) {
    steepest = std::max(steepest, a[i - 1] - a[i]);
  }
  Check(steepest < 0.12f, Num("soft: no step of the ramp over a quarter texel is bigger than 0.12 (16 gathers: %.3f)", double(steepest)));
  ramp(soft, 0.95f, 0.37f, &b);
  Check(std::fabs(width(b) - soft_width) < 0.6f, Num("soft: the width does not depend on the blocker's distance (%.2f against %.2f texels)", double(width(b)), double(soft_width)));
  // Without a temporal upscaler the disk does not turn: the noise changes nothing. With one it does.
  std::vector<float> c, d;
  ramp(soft, 0.6f, 0.05f, &c);
  ramp(soft, 0.6f, 0.83f, &d);
  Check(c == d, "soft: no upscaler: the pixel's noise changes nothing (a fixed disk)");
  CarMapView turning = soft;
  turning.noise_turns = true;
  ramp(turning, 0.6f, 0.05f, &c);
  ramp(turning, 0.6f, 0.83f, &d);
  float most = 0.0f;
  for (size_t i = 0; i < c.size(); ++i) {
    most = std::max(most, std::fabs(c[i] - d[i]));
  }
  Check(most > 0.01f, Num("soft: with a temporal upscaler the noise turns the disk (the ramps differ by %.3f)", double(most)));
  // pcss: the penumbra grows with the blocker's distance (the sun 15 times the real one here, texels of 1 cm).
  CarMapView pcss = m;
  pcss.filter_mode = kCarMapFilterPcss;
  pcss.sun_times = 15.0f;
  ramp(pcss, 0.6f, 0.37f, &a);   // 10 cm behind the blocker
  ramp(pcss, 0.95f, 0.37f, &b);  // 45 cm behind it
  const float near_width = width(a), far_width = width(b);
  Check(far_width > near_width + 1.0f && near_width < 2.5f,
        Num("pcss: the penumbra grows with the blocker's distance: %.2f texels at 10 cm, %.2f at 45 cm", double(near_width), double(far_width)));
  Check(far_width <= 1.4f * 2.0f * (kCarMapSoftRadiusMetres / m.texel_m) + 0.5f, "pcss: and never wider than twice the soft disk");
  pcss.sun_times = 4.0f;
  ramp(pcss, 0.95f, 0.37f, &c);
  Check(width(c) < far_width, "pcss: a smaller sun gives a narrower penumbra");
  // The cost: a pixel with no blocker in reach makes the centre's gather and the search's 8 (9), one at an edge 9 + 16.
  std::vector<float> empty(64 * 64, 1.0f);
  CarMapView none = soft;
  none.texels = empty.data();
  const CarMapResult nothing = CarMapShadow(none, {0.3f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  Check(nothing.shadow == 0.0f && nothing.gathers == 1 + kCarMapSearchTaps, Num("soft: no blocker: the centre and the search alone (%.0f gathers)", double(nothing.gathers)));
  const CarMapResult edge = CarMapShadow(soft, {0.5f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  Check(edge.gathers == 1 + kCarMapSearchTaps + kCarMapFilterTaps && edge.shadow > 0.2f && edge.shadow < 0.8f,
        Num("soft: at the edge: 9 gathers and the 16 taps (%.0f), a partial shadow %.2f", double(edge.gathers), double(edge.shadow)));
  // `game` is the map's own filter, to the bit: a ring of five taps turned by the noise at the real sun's penumbra.
  const CarMapResult game = CarMapShadow(m, {0.5f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  Check(m.filter_mode == kCarMapFilterGame && game.gathers == 5 + 4, Num("game: the map's own ring (%.0f gathers)", double(game.gathers)));
  // The matrix texture's last component.
  Check(CarMapFilterModeOf(CarMapFilterCode(kCarMapFilterGame, 64)) == kCarMapFilterGame && CarMapFilterCode(kCarMapFilterSoft, 64) == 256.0f &&
            CarMapFilterModeOf(CarMapFilterCode(kCarMapFilterPcss, 64)) == kCarMapFilterPcss && CarMapFilterSunOf(CarMapFilterCode(kCarMapFilterPcss, 64)) == 4.0f &&
            CarMapFilterSunOf(CarMapFilterCode(kCarMapFilterPcss, 240)) == 15.0f && CarMapFilterCode(kCarMapFilterGame, 64) == 0.0f,
        "the matrix texture's filter code: mode * 256 + the sun in sixteenths (pcss only), read back exactly");
  Check(kCarMapSunTan == 0.0047123890f && std::fabs(kCarMapSunTan - std::tan(0.27 * 3.14159265358979323846 / 180.0)) < 1e-5 &&
            kCarMapFilterTaps == 16 && kCarMapFilterMaxRadii == 2.0f,
        "the car map's sun is the world's (tan 0.27 degrees), its disk 16 taps, its cap twice the soft radius");
}

void CarMapRules() {
  // The penumbra's half width is the blocker distance times the sun's angular radius, at least a texel's worth and at most kCarMapMaxFilterTexels.
  const float texel = 0.005f;
  Check(std::fabs(CarMapFilterTexels(1.0f, texel) - 1.0f * kCarMapSunRadius / texel) < 1e-6f && CarMapFilterTexels(1.0f, texel) > kCarMapMinFilterTexels,
        Num("a blocker 1 m away: a filter %.2f texels wide (blocker x 0.0044 / texel)", double(CarMapFilterTexels(1.0f, texel))));
  Check(std::fabs(CarMapFilterTexels(2.0f, texel) - 2.0f * CarMapFilterTexels(1.0f, texel)) < 1e-5f, "the filter doubles with the blocker distance");
  Check(CarMapFilterTexels(0.02f, texel) == kCarMapMinFilterTexels, "a blocker a few centimetres away: the filter is its minimum");
  Check(CarMapFilterTexels(100.0f, texel) == kCarMapMaxFilterTexels, "a very far blocker: the filter is held to its maximum");
  Check(std::fabs(kCarMapSunRadius - std::tan(0.25 * 3.14159265358979323846 / 180.0)) < 1e-4, "the sun's angular radius is a quarter of a degree");
  Check(CarMapSearchTexels(0.0005f) == kCarMapMaxFilterTexels && CarMapSearchTexels(0.1f) == kCarMapSearchMinTexels, "the blocker search radius is held between its limits");
  Check(kCarMapBiasFloor >= 0.01f && kCarMapBiasFloor <= 0.02f && CarMapBiasMetres(0.004f, 0.002f) > kCarMapBiasFloor && CarMapBiasMetres(0.004f, 0.002f) < 0.02f,
        Num("the constant bias is the producer's 1 to 2 cm with a few millimetres more: %.1f mm", double(CarMapBiasMetres(0.004f, 0.002f)) * 1000.0));
  Check(kCarMapProducerSlope == 2.0f, "the map's slope scaled bias is the producer's (car_shadow_map_rules.h kSlopeScaledDepthBias, 2)");
  Check((kCarMapFlag & kCarMapViewFlag) == 0 && (kCarMapFlag & kCarMapKeepMarchFlag) == 0 && (kCarMapViewFlag & kCarMapKeepMarchFlag) == 0 &&
            ((kCarMapFlag | kCarMapViewFlag | kCarMapKeepMarchFlag) & (kSkipFlag | kStatFlag | kSideFlag | (1u << 25))) == 0,
        "the map's flag bits (28 to 30) are apart from each other and from the skip's, the statistics' and the block pass's (24, 26, 27) and the contact filter's (25)");
  // The map is not read outside itself: a point past the border has no term, a point inside the border's fade a part of it.
  std::vector<float> texels(64 * 64, 0.5f);
  CarMapView m;
  m.size = 64;
  m.texel_m = 0.01f;
  m.depth_m = 1.0f;
  m.texels = texels.data();
  m.to_map[0][0] = 1.0f / (64.0f * 0.01f);
  m.to_map[1][1] = -1.0f / (64.0f * 0.01f);
  m.to_map[1][3] = 1.0f;
  m.to_map[2][2] = 1.0f;  // depth 0..1 along view z: the "sun" shines along -z toward the camera
  const float side = 0.64f;
  // A flat blocker at depth 0.5 everywhere; a receiver at z = 0.52 facing the sun (normal toward -z): in its shadow inside the window.
  const Vec3 n{0.0f, 0.0f, -1.0f};
  const CarMapResult inside = CarMapShadow(m, {0.3f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  const CarMapResult outside = CarMapShadow(m, {1.2f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  const CarMapResult at_border = CarMapShadow(m, {0.995f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  Check(inside.shadow > 0.99f && inside.covered, Num("a point 10 cm behind a flat blocker, inside the map: shadow %.3f", double(inside.shadow)));
  Check(outside.shadow == 0.0f && !outside.covered, "a point outside the map has no term and is not covered");
  Check(at_border.shadow > 0.0f && at_border.shadow < inside.shadow, Num("a point 0.3 texels from the border: the term fades (%.3f)", double(at_border.shadow)));
  const CarMapResult before = CarMapShadow(m, {0.3f * side, 0.5f * side, 0.45f}, n, 0.0005f, 0.5f);
  Check(before.shadow == 0.0f && !before.covered, "a point nearer to the sun than the blocker is lit and is not covered (the car's part is behind it)");
  const CarMapResult thin = CarMapShadow(m, {0.3f * side, 0.5f * side, 0.505f}, n, 0.0005f, 0.5f);
  Check(thin.shadow == 0.0f && thin.covered, "a point 5 mm behind the stored surface (inside the bias) is lit, and covered: it is the surface itself");
  // The receiver beyond the far plane (depth above 1) under an EMPTY texel is lit; under a texel that holds a part of the car it is in shadow.
  std::vector<float> empty(64 * 64, 1.0f);
  m.texels = empty.data();
  const CarMapResult none = CarMapShadow(m, {0.3f * side, 0.5f * side, 0.6f}, n, 0.0005f, 0.5f);
  const CarMapResult beyond = CarMapShadow(m, {0.3f * side, 0.5f * side, 1.4f}, n, 0.0005f, 0.5f);
  Check(none.shadow == 0.0f && !none.covered && none.gathers == 5, Num("an empty map: no shadow, not covered, %.0f gathers (the search alone)", double(none.gathers)));
  Check(beyond.shadow == 0.0f && !beyond.covered, "a point beyond the far plane under an empty texel is not shadowed (the stored 1 is tested before the depths are compared)");
  m.texels = texels.data();
  const CarMapResult behind = CarMapShadow(m, {0.3f * side, 0.5f * side, 1.4f}, n, 0.0005f, 0.5f);
  Check(behind.shadow > 0.99f, "a point beyond the far plane under a texel that holds the car is behind it, and shadowed");
}

// Every sun, on the lit side of every face of the rigs' cars: no acne. The maps are 8 m a side, as the producer's for an R34: 7.8 mm a texel at 1024.
void CarMapNoAcne() {
  struct RigCase {
    const char* name;
    Rig rig;
  };
  std::vector<RigCase> cases;
  cases.push_back({"wing from behind and above", MakeWingRig({0.0f, 2.1f, -5.0f}, {0.0f, 1.2f, 0.4f})});
  cases.push_back({"wing from the side", MakeWingRig({-3.0f, 1.5f, 0.4f}, {0.0f, 1.28f, 0.4f})});
  cases.push_back({"car and tyres on the road", MakeCarRig({3.0f, 1.6f, -5.0f}, {0.0f, 0.6f, 2.2f}, 0.0f, true)});
  cases.push_back({"mirror on a door", MakeMirrorRig()});
  CarMapTally all;
  for (const RigCase& c : cases) {
    const RigImage im = RenderRig(c.rig);
    CarMapTally t;
    int suns = 0;
    for (const double elevation : {10.0, 22.0, 38.0, 60.0, 85.0}) {
      for (const double azimuth : {20.0, 110.0, 200.0, 290.0}) {
        const Vec3 sun = Sun(elevation, azimuth);
        const CarMapRig map = BuildCarMap(c.rig, sun, 1024, 2.0f);
        CarMapTally one;
        ScoreCarMap(c.rig, im, map, &one);
        t += one;
        ++suns;
      }
    }
    std::printf("car map, %s, %d suns: %ld pixels judged (%ld on an edge), lit %ld: acne %ld (over 0.3: %ld, over 0.5: %ld); in shadow %ld: missed %ld; shadow area truth %.0f, map %.0f; "
                "vehicle pixels covered %ld of %ld, others %ld; gathers %.2f a pixel (worst %d)\n",
                c.name, suns, t.pixels, t.edge, t.lit, t.acne, t.acne_strong, t.false_dark, t.shadow, t.missed, t.truth_area, t.map_area, t.vehicle_covered, t.vehicle,
                t.other_covered, t.evals ? double(t.gathers) / double(t.evals) : 0.0, t.worst_gathers);
    Check(t.lit > 20000 && t.shadow > 1000, std::string(c.name) + ": the sample is large enough (lit " + std::to_string(t.lit) + ", shadowed " + std::to_string(t.shadow) + ")");
    // No acne: no pixel of the interior of the light is darkened over 0.3, none over 0.02 but a pixel in 200,000 (a shadow's far tail crossing the map's border).
    Check(t.acne * 200000 <= t.lit && t.acne_strong == 0 && t.false_dark == 0,
          std::string(c.name) + ": no acne, no pixel of a lit face the map darkens: " + std::to_string(t.acne) + " of " + std::to_string(t.lit));
    all += t;
  }
  Check(all.missed * 100 <= all.shadow, Num("the interior of the shadows is found: %.0f of %.0f missed (at most 1%%)", double(all.missed), double(all.shadow)));
  Check(std::fabs(all.map_area - all.truth_area) <= 0.04 * all.truth_area,
        Num("the shadows' size is the truth's to 4%%: map %.0f, truth %.0f pixels", all.map_area, all.truth_area));
  // The producer's ultra size: 2048 texels over the same window (3.7 mm a texel), three suns.
  {
    const Rig rig = MakeWingRig({-3.0f, 1.5f, 0.4f}, {0.0f, 1.28f, 0.4f});
    const RigImage im = RenderRig(rig);
    CarMapTally t;
    for (const double elevation : {10.0, 38.0, 85.0}) {
      const CarMapRig map = BuildCarMap(rig, Sun(elevation, 200.0), 2048, 2.0f);
      ScoreCarMap(rig, im, map, &t);
    }
    std::printf("car map at 2048 texels (the wing from the side, 3 suns): %ld lit pixels judged: acne %ld (over 0.3: %ld); in shadow %ld: missed %ld\n", t.lit, t.acne, t.acne_strong, t.shadow, t.missed);
    Check(t.lit > 20000 && t.acne * 200000 <= t.lit && t.acne_strong == 0 && t.false_dark == 0 && t.missed * 100 <= t.shadow, "at 2048 texels: no acne, and the shadows are found");
  }
  // The reader does not lean on the producer's slope scaled bias, and the check has teeth: on maps made WITHOUT that bias (producer_slope 0) the reader above
  // (the receiver's own plane, a constant 1 cm) still has no acne; a plain reader (the nearest texel, a constant bias of 1 cm) shadows the lit faces.
  {
    const Rig rig = MakeWingRig({0.0f, 2.1f, -5.0f}, {0.0f, 1.2f, 0.4f});
    const RigImage im = RenderRig(rig);
    CarMapTally plane, plain;
    for (const double elevation : {10.0, 22.0, 38.0}) {
      for (const double azimuth : {20.0, 200.0}) {
        const CarMapRig map = BuildCarMap(rig, Sun(elevation, azimuth), 1024, 2.0f, 0.0f);
        ScoreCarMap(rig, im, map, &plane);
        ScoreShadow(rig, im, map, [&](int, int, float, Vec3 pc, Vec3, float, bool* covered, int* gathers) {
          const CarMapView& m = map.view;
          const float u = pc.x * m.to_map[0][0] + pc.y * m.to_map[0][1] + pc.z * m.to_map[0][2] + m.to_map[0][3];
          const float v = pc.x * m.to_map[1][0] + pc.y * m.to_map[1][1] + pc.z * m.to_map[1][2] + m.to_map[1][3];
          const float d = pc.x * m.to_map[2][0] + pc.y * m.to_map[2][1] + pc.z * m.to_map[2][2] + m.to_map[2][3];
          const float s = m.At(int(u * float(m.size)), int(v * float(m.size)));
          *covered = false;
          *gathers = 1;
          return (s < kCarMapEmpty && (d - s) * m.depth_m > 0.01f) ? 1.0f : 0.0f;
        }, &plain);
      }
    }
    std::printf("maps without the producer's slope bias (the wing from behind, 6 suns): the reader above has acne on %ld of %ld lit pixels; a plain reader (nearest texel, constant 1 cm) on %ld of %ld\n",
                plane.acne, plane.lit, plain.acne, plain.lit);
    Check(plane.acne == 0 && plain.acne > 200, "without the producer's slope bias the reader still has no acne, where a plain constant bias shadows the lit faces (the check sees it)");
  }
}

// The wing's shadow on the lid and the posts' (the sun high enough for it to fall on the lid), and the body's on the road: where the geometry says.
void CarMapShadowsLandWhereGeometrySays() {
  {
    const Rig rig = MakeWingRig({0.0f, 2.4f, -4.5f}, {0.0f, 1.1f, 0.5f});
    const RigImage im = RenderRig(rig);
    // The sun behind the camera at 55 degrees: the plate (28 cm over the lid, 30 cm chord) throws its shadow onto the lid, displaced
    // 0.28 / tan(55) = 0.196 m away from the camera along z.
    const Vec3 sun = Sun(55.0, 180.0);
    const CarMapRig map = BuildCarMap(rig, sun, 1024, 2.0f);
    int expected = 0, found = 0, wrong = 0, lid_pixels = 0;
    for (int y = 0; y < rig.cam.height; y += 2) {
      for (int x = 0; x < rig.cam.width; x += 2) {
        const float tc = im.depth.At(x, y);
        if (tc >= 30000.0f) {
          continue;
        }
        const Vec3 p = RigPoint(rig, im, x, y);
        if (std::fabs(p.y - 1.0f) > 0.004f || p.z < 0.02f || p.z > 1.08f || std::fabs(p.x) > 0.7f) {
          continue;  // the lid's top face
        }
        ++lid_pixels;
        // Where the plate's shadow is: the plate's footprint along the sun's ray at the lid's height.
        const float tan55 = std::tan(55.0f * 3.14159265f / 180.0f);
        const float z_lo = 0.15f + 0.28f / tan55, z_hi = 0.45f + 0.32f / tan55;  // the underside's and the top's far corner: the shadow's extent on the lid
        const bool geometry = p.z > z_lo + 0.02f && p.z < z_hi - 0.02f;  // 2 cm inside the plate's shadow
        const bool open = p.z < z_lo - 0.02f || p.z > z_hi + 0.02f;
        const bool post = std::fabs(std::fabs(p.x) - 0.5f) < 0.05f && p.z > 0.23f && p.z < 0.56f;  // the posts' own shadows are on the lid too
        const Vec3 pv = rig.ToView(p - rig.eye);
        const Vec3 normal = rig.ToView({0.0f, 1.0f, 0.0f});
        const float fp = FootprintUnit(rig.cam.tan_y, float(rig.cam.height)) * tc;
        const CarMapResult r = CarMapShadow(map.view, pv, normal, fp, PixelNoise(x, y));
        const bool shaded = r.shadow > 0.5f;
        if (geometry) {
          ++expected;
          found += shaded ? 1 : 0;
        }
        if (open && !post) {
          wrong += shaded ? 1 : 0;
        }
      }
    }
    std::printf("wing's shadow on the lid, sun 55 degrees behind the camera: %d lid pixels, %d inside the plate's shadow by geometry, %d found; %d wrongly shaded outside it\n",
                lid_pixels, expected, found, wrong);
    Check(expected > 500 && found * 100 >= expected * 98, Num("the plate's shadow lands on the lid where the geometry says: %.0f of %.0f", double(found), double(expected)));
    Check(wrong == 0, Num("no lid pixel outside the plate's shadow (and the posts') is shaded: %.0f", double(wrong)));
  }
  {
    // The body on the road: the shadow of a car 25 cm off the road, the sun at 30 degrees from the side.
    const Rig rig = MakeCarRig({-1.2f, 6.5f, -2.5f}, {-1.5f, 0.0f, 2.2f}, 0.0f, false);
    const Vec3 sun = Sun(30.0, 90.0);  // from +x: the shadow falls toward -x (the camera's side)
    const CarMapRig map = BuildCarMap(rig, sun, 1024, 3.0f);
    const float tan30 = std::tan(30.0f * 3.14159265f / 180.0f);
    const float far_edge = -0.9f - 1.4f / tan30;   // the roof's edge's shadow on the road: -3.325
    const float near_edge = 0.9f - 0.25f / tan30;  // the sun side edge of the underside's shadow on the road: 0.467; the road beyond it is lit
    const RigImage im = RenderRig(rig);
    int inside = 0, inside_shaded = 0, outside = 0, outside_shaded = 0;
    for (int y = 0; y < rig.cam.height; y += 2) {
      for (int x = 0; x < rig.cam.width; x += 2) {
        if (!im.road[size_t(y) * size_t(rig.cam.width) + size_t(x)]) {
          continue;
        }
        const Vec3 p = RigPoint(rig, im, x, y);
        if (p.z < 0.3f || p.z > 4.2f) {
          continue;
        }
        const float tc = im.depth.At(x, y);
        const float fp = FootprintUnit(rig.cam.tan_y, float(rig.cam.height)) * tc;
        const CarMapResult r = MapAtWorld(rig, map, p, {0.0f, 1.0f, 0.0f}, fp, PixelNoise(x, y));
        const bool shaded = r.shadow > 0.5f;
        if (p.x < near_edge - 0.03f && p.x > far_edge + 0.03f) {
          ++inside;
          inside_shaded += shaded ? 1 : 0;
        } else if (p.x < far_edge - 0.03f) {
          ++outside;
          outside_shaded += shaded ? 1 : 0;
        }
      }
    }
    std::printf("body's shadow on the road, sun 30 degrees from the side: road pixels between x = %.2f and %.2f m: %d, shaded %d; beyond its far edge: %d, shaded %d\n",
                double(far_edge), double(near_edge), inside, inside_shaded, outside, outside_shaded);
    Check(inside > 1000 && inside_shaded * 100 >= inside * 99, Num("the road inside the body's shadow is shaded: %.0f of %.0f", double(inside_shaded), double(inside)));
    Check(outside > 1000 && outside_shaded * 100 <= outside, Num("the road outside it is not: %.0f of %.0f", double(outside_shaded), double(outside)));
  }
}

// The penumbra: a plate over the road, the width of the shadow's edge across the road grows with the blocker's distance as the sun's half degree says.
void CarMapPenumbra() {
  const double pi = 3.14159265358979323846;
  const double elevation = 30.0;
  const double tan_e = std::tan(elevation * pi / 180.0), sin_e = std::sin(elevation * pi / 180.0);
  // The 90% to 10% width of the term across the edge of the plate's shadow on the road (metres along the road's z axis), the plate `height` m up.
  const auto edge_width = [&](float height) {
    Rig rig;
    rig.cam = MakeCamera(720);
    rig.eye = {0.0f, 3.0f, -6.0f};
    rig.boxes.push_back({-1.0f, 1.0f, height, height + 0.04f, 0.0f, 2.0f, true});  // a plate 2 m square, 4 cm thick
    rig.Aim({0.0f, 0.0f, 1.0f});
    const Vec3 sun = Sun(elevation, 0.0);  // toward +z and up: the shadow falls toward -z
    const CarMapRig map = BuildCarMap(rig, sun, 1024, 0.5f);
    // The plate's far edge (z = 2, its underside) throws the shadow's edge onto the road at z = 2 - height / tan(elevation).
    const double centre_z = 2.0 - double(height) / tan_e;
    bool have_deep = false, have_faint = false;
    double deep = 0.0, faint = 0.0;
    const int steps = 800;
    for (int i = 0; i <= steps; ++i) {
      const double z = centre_z - 0.3 + 0.6 * double(i) / double(steps);
      const CarMapResult r = MapAtWorld(rig, map, {0.0f, 0.0f, float(z)}, {0.0f, 1.0f, 0.0f}, 0.001f, 0.5f);
      if (r.shadow >= 0.9f) {
        deep = z;  // the largest z whose term is at least 90%
        have_deep = true;
      }
      if (r.shadow >= 0.1f) {
        faint = z;  // the largest z whose term is at least 10%
        have_faint = true;
      }
    }
    return (have_deep && have_faint) ? faint - deep : 0.0;
  };
  const double e_near = edge_width(0.28f), e_mid = edge_width(0.9f), e_far = edge_width(2.0f);
  // The sun's own penumbra on the road: 0.0088 rad times the blocker's distance along the ray (height / sin) across the ray, over sin along the road
  // (0 to 100%), 0.8 of it between 10% and 90%.
  const auto sun_width = [&](double height) { return 0.8 * 0.0088 * height / (sin_e * sin_e); };
  std::printf("penumbra on the road, the sun at 30 degrees (the term's 90%% to 10%% width; the sun's own in brackets): plate 0.28 m up %.1f mm (%.1f), 0.9 m %.1f mm (%.1f), 2 m %.1f mm (%.1f)\n",
              e_near * 1000.0, sun_width(0.28) * 1000.0, e_mid * 1000.0, sun_width(0.9) * 1000.0, e_far * 1000.0, sun_width(2.0) * 1000.0);
  Check(e_far > e_mid && e_mid > e_near && e_near > 0.0, "the edge is wider the farther the blocker");
  Check(e_far > 1.5 * e_mid && e_far < 3.2 * e_mid, Num("2 m against 0.9 m: the widths' ratio is about the distances' 2.2: %.2f", e_far / std::max(e_mid, 1e-9)));
  Check(e_mid > 0.4 * sun_width(0.9) && e_mid < 2.5 * sun_width(0.9), Num("the edge under a plate 0.9 m up is within a factor of 2.5 of the sun's own: %.1f mm of %.1f", e_mid * 1000.0, sun_width(0.9) * 1000.0));
  Check(e_far > 0.4 * sun_width(2.0) && e_far < 2.5 * sun_width(2.0), Num("the edge under a plate 2 m up is within a factor of 2.5 of the sun's own: %.1f mm of %.1f", e_far * 1000.0, sun_width(2.0) * 1000.0));
  // A close blocker: the sun's penumbra is a few millimetres and the filter's minimum, about a texel and a half across, is what the edge is.
  const double texel = 3.0 / 1024.0;  // the window: the plate's 2 m plus the margin 0.5 m a side
  Check(e_near < (4.0 * double(kCarMapMinFilterTexels) * texel) / sin_e,
        Num("a blocker 28 cm up: the edge stays narrow, within about 4 filter minimums along the road (%.1f mm)", e_near * 1000.0));
}

// A map in the frame changes nothing for the pixels it does not reach, and a pixel of a car the map does not hold (traffic) marches as before.
void CarMapLeavesTheRestAlone() {
  Rig rig = MakeWingRig({-5.0f, 3.0f, -5.0f}, {0.0f, 0.6f, -0.5f});
  const Rig player = rig;  // the map holds the player's car alone
  // Traffic: another car in front of it and inside the map's window, not in the map.
  rig.boxes.push_back({-1.6f, 0.4f, 0.0f, 1.0f, -3.8f, -1.2f, true});
  rig.Aim({0.0f, 0.6f, -0.5f});
  const RigImage im = RenderRig(rig);
  const Vec3 sun = Sun(35.0, 250.0);
  const CarMapRig map = BuildCarMap(player, sun, 1024, 4.0f);
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  Trace plain = LowSunTrace(35.0);
  plain.camera = rig.cam;
  plain.to_sun = rig.ToView(sun);
  Trace with_map = plain;
  with_map.car_map = &map.view;
  long same = 0, differ = 0, differ_allowed = 0, traffic = 0, traffic_covered = 0, traffic_same = 0, outside_px = 0, outside_same = 0;
  long covered_vehicle = 0, steps_dropped = 0, player_px = 0;
  for (int y = 0; y < rig.cam.height; y += 2) {
    for (int x = 0; x < rig.cam.width; x += 2) {
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f) {
        continue;
      }
      plain.noise = with_map.noise = PixelNoise(x, y);
      const float a = March(plain, depth, x, y);
      const uint32_t steps_a = plain.steps_used;
      const float b = March(with_map, depth, x, y);
      const uint32_t steps_b = with_map.steps_used;
      const CarMapResult r = with_map.car_result;
      const Vec3 p = RigPoint(rig, im, x, y);
      const bool is_traffic = p.z < -1.1f && DepthIsVehicle(tc);
      if (is_traffic) {
        ++traffic;
        traffic_covered += r.covered ? 1 : 0;
        // A traffic car's pixel marches as before, and where the player's car throws no shadow on it the result is the same to the bit.
        traffic_same += (steps_a == steps_b && (r.shadow > 0.0f || a == b)) ? 1 : 0;
      } else if (DepthIsVehicle(tc)) {
        ++player_px;
      }
      const bool reached = r.covered || r.shadow > 0.0f;
      if (!reached) {
        ++outside_px;
        outside_same += (a == b && steps_a == steps_b) ? 1 : 0;
      }
      if (a == b) {
        ++same;
      } else {
        ++differ;
        // A pixel may differ only where the map shadows it, or where it covers a car's pixel (no march steps: the map's own result).
        differ_allowed += (r.shadow > 0.0f || (r.covered && DepthIsVehicle(tc))) ? 1 : 0;
      }
      if (r.covered && DepthIsVehicle(tc) && !is_traffic) {
        ++covered_vehicle;
        steps_dropped += (steps_b == 0 && steps_a > 0) ? 1 : 0;
      }
    }
  }
  std::printf("map in the frame, the player's car and a traffic car in front of it: %ld pixels as without the map, %ld differ (all %ld allowed); the pixels the map does not reach: %ld, "
              "identical %ld; traffic car's pixels %ld, covered %ld, marching as before %ld; the player's car's pixels %ld, covered %ld, march dropped %ld\n",
              same, differ, differ_allowed, outside_px, outside_same, traffic, traffic_covered, traffic_same, player_px, covered_vehicle, steps_dropped);
  Check(outside_px > 10000 && outside_same == outside_px, Num("a pixel the map does not reach is the march's, to the bit: %.0f of %.0f", double(outside_same), double(outside_px)));
  Check(differ == differ_allowed, Num("a pixel differs only where the map shadows it or takes the vehicle march's place: %.0f of %.0f", double(differ_allowed), double(differ)));
  Check(traffic > 500 && traffic_covered * 20 <= traffic, Num("the traffic car's pixels are not covered by the player's map (%.0f of %.0f)", double(traffic_covered), double(traffic)));
  Check(traffic_same * 100 >= traffic * 97, Num("the traffic car marches as it did: %.0f of %.0f", double(traffic_same), double(traffic)));
  Check(covered_vehicle * 10 >= player_px * 9 && player_px > 3000, Num("nearly every pixel of the player's car is covered by its map: %.0f of %.0f", double(covered_vehicle), double(player_px)));
  Check(steps_dropped == covered_vehicle, Num("every player's car pixel the map covers takes no march steps: %.0f of %.0f", double(steps_dropped), double(covered_vehicle)));
  // The A/B switch: the vehicle march kept on the covered pixels takes its steps again, and the pixel is the darker of the two.
  Trace keep = with_map;
  keep.car_map_keep_march = true;
  long kept_steps = 0, kept_darker = 0, kept_total = 0;
  for (int y = 0; y < rig.cam.height; y += 4) {
    for (int x = 0; x < rig.cam.width; x += 4) {
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f || !DepthIsVehicle(tc)) {
        continue;
      }
      plain.noise = with_map.noise = keep.noise = PixelNoise(x, y);
      const float a = March(plain, depth, x, y);
      const float k = March(keep, depth, x, y);
      if (keep.car_result.covered) {
        ++kept_total;
        kept_steps += keep.steps_used == plain.steps_used ? 1 : 0;
        kept_darker += k <= a ? 1 : 0;
      }
    }
  }
  Check(kept_total > 500 && kept_steps == kept_total && kept_darker == kept_total,
        Num("with the vehicle march kept: same steps and never lighter than without the map: %.0f of %.0f", double(kept_darker), double(kept_total)));
  // The debug view: the map's term alone; the facing weight is in it; no steps.
  Trace view = with_map;
  view.car_map_only = true;
  long view_ok = 0, view_total = 0;
  for (int y = 0; y < rig.cam.height; y += 5) {
    for (int x = 0; x < rig.cam.width; x += 5) {
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f) {
        continue;
      }
      with_map.noise = view.noise = PixelNoise(x, y);
      const float v = March(view, depth, x, y);
      ++view_total;
      view_ok += (v >= 0.0f && v <= 1.0f && view.steps_used == 0) ? 1 : 0;
    }
  }
  Check(view_total > 1000 && view_ok == view_total, "the debug view is the map's term alone: in 0 to 1, no march steps");
}

// The wing from behind and from the side, the owner's cases of 2026-11-18: against the exact truth the false shadow on the car's own pixels goes (the map
// replaces the vehicle march where it covers) and the real shadows of the posts and the plate stay. Judged on the interior of the shadows and of the light.
void CarMapFalseShadowOnTheWing() {
  CarMapTally march_only, with_map, march_tight, map_tight;
  const auto run = [&](const Rig& rig, const RigImage& im, Vec3 sun) {
    const CarMapRig map = BuildCarMap(rig, sun, 1024, 2.0f);
    Trace base = LowSunTrace(17.7);
    base.camera = rig.cam;
    base.to_sun = rig.ToView(sun);
    const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
    const auto march = [&](bool use_map) {
      return [&, use_map](int x, int y, float, Vec3, Vec3, float, bool* covered, int* gathers) {
        Trace t = base;
        t.noise = PixelNoise(x, y);
        t.car_map = use_map ? &map.view : nullptr;
        const float visibility = March(t, depth, x, y);
        *covered = t.car_result.covered;
        *gathers = t.car_result.gathers;
        return 1.0f - visibility;
      };
    };
    ScoreShadow(rig, im, map, march(false), &march_only, true);
    ScoreShadow(rig, im, map, march(true), &with_map, true);
    ScoreShadow(rig, im, map, march(false), &march_tight, true, 0.03f);
    ScoreShadow(rig, im, map, march(true), &map_tight, true, 0.03f);
  };
  for (const Vec3 eye : {Vec3{0.0f, 1.55f, -3.5f}, Vec3{0.0f, 2.1f, -5.0f}}) {
    const Rig rig = MakeWingRig(eye, {0.0f, 1.28f, 0.0f});
    run(rig, RenderRig(rig), Sun(17.7, 180.0));
  }
  for (const float side : {3.0f, 4.5f}) {
    const Rig rig = MakeWingRig({-side, 1.5f, 0.4f}, {0.0f, 1.28f, 0.4f});
    const RigImage im = RenderRig(rig);
    for (int az = 0; az < 360; az += 45) {
      run(rig, im, Sun(17.7, double(az)));
    }
  }
  std::printf("the wing, 2 views from behind and 16 from the side (the sun at 17.7 degrees), the car's own pixels, the interior of the light: darkened (over 0.5) with nothing in the way: "
              "the vehicle march alone %ld of %ld, with the map %ld of %ld; the interior of the shadows: %ld pixels, darkened by the march alone %ld, with the map %ld; "
              "of those within 15 cm of their blocker (%ld): march alone %ld, with the map %ld\n",
              march_only.false_dark, march_only.lit, with_map.false_dark, with_map.lit, with_map.shadow, with_map.shadow - march_only.missed, with_map.shadow - with_map.missed,
              with_map.near, with_map.near - march_only.near_missed, with_map.near - with_map.near_missed);
  std::printf("  (a zone of 3 cm round the truth's edges only: in the light, darkened with nothing in the way: march alone %ld of %ld, with the map %ld of %ld; in shadow, left light: march alone %ld of %ld, with the map %ld)\n", march_tight.false_dark, march_tight.lit, map_tight.false_dark, map_tight.lit, march_tight.missed, march_tight.shadow, map_tight.missed);
  Check(march_tight.shadow > 3000 && march_tight.missed * 2 > march_tight.shadow,
        Num("the vehicle march alone leaves most of the car's own shadow pixels light: %.0f of %.0f (its ray is 30 cm long)", double(march_tight.missed), double(march_tight.shadow)));
  Check(with_map.false_dark == 0 && map_tight.false_dark <= march_tight.false_dark,
        Num("with the map no pixel of the car's own light is darkened (3 cm zone: %.0f against the march's %.0f)", double(map_tight.false_dark), double(march_tight.false_dark)));
  Check(with_map.shadow > 3000 && with_map.missed * 50 <= with_map.shadow, Num("the interior of the car's own shadows is found: %.0f of %.0f missed (at most 2%%)", double(with_map.missed), double(with_map.shadow)));
  Check(with_map.shadow - with_map.missed > 10 * (march_only.shadow - march_only.missed),
        Num("the map finds ten times the car's own shadow pixels the vehicle march does: %.0f against %.0f", double(with_map.shadow - with_map.missed), double(march_only.shadow - march_only.missed)));
  Check(with_map.near > 300 && (with_map.near - with_map.near_missed) * 100 >= (march_only.near - march_only.near_missed) * 98,
        Num("the small shadows (the blocker within 15 cm) the march found stay: %.0f of %.0f", double(with_map.near - with_map.near_missed), double(march_only.near - march_only.near_missed)));
}

// ---------------------------------------------------------------------------
// the game's sun shadow mask is not a car's: the comb on the trunk lid under the wing (owner, 2026-11-19; contact_trace.h has the measurements)
// ---------------------------------------------------------------------------

// The mask as the owner's pause picture has it over a car's pixels: the iron fence behind the car, columns of shadow 3 pixels wide every 26 pixels,
// lit between them (the stored 0.5 the sun gives; the apply's gain 2 makes it a visibility of 1).
MaskImage MakeCombMask(int width, int height) {
  MaskImage m;
  m.w = width;
  m.h = height;
  m.v.assign(size_t(m.w) * size_t(m.h), 128);
  for (int y = 0; y < m.h; ++y) {
    for (int x = 0; x < m.w; ++x) {
      const int phase = x % 26;
      if (phase >= 10 && phase < 13) {
        m.v[size_t(y) * size_t(m.w) + size_t(x)] = 0;
      }
    }
  }
  return m;
}

// The apply's factor over the lid under the wing at the pause camera's kind of view, the sun at 55 degrees behind the camera: the plate's shadow
// falls on the lid, 0.196 m away from the camera. The march is the twin's (with the car's map: the lid is the map's, every pixel covered), the apply
// is ApplyFactor over it (the filter, the weight, the mask at the pixel).
struct LidResult {
  int shadow = 0;              // lid pixels inside the plate's shadow by geometry (2 cm in from its edges, away from the posts)
  int bright = 0;              // ... whose factor is above 0.5 (the lines)
  int bright_columns = 0;      // image columns holding at least one of them
  float lo = 1.0f, hi = 0.0f;  // the range of their factors
  int open = 0;                // lid pixels clearly outside the shadow
  int open_dark = 0;           // ... whose factor is under 0.9
};
LidResult LidFactors(const MaskImage& mask, bool car_sees_mask) {
  LidResult r;
  const Rig rig = MakeWingRig({0.0f, 2.4f, -4.5f}, {0.0f, 1.1f, 0.5f});
  const RigImage im = RenderRig(rig);
  const Vec3 sun = Sun(55.0, 180.0);
  const CarMapRig map = BuildCarMap(rig, sun, 1024, 2.0f);
  Trace base = LowSunTrace(55.0);
  base.camera = rig.cam;
  base.to_sun = rig.ToView(sun);
  base.car_map = &map.view;
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  const auto vis = [&](int x, int y) {
    Trace t = base;
    t.noise = PixelNoise(x, y);
    return March(t, depth, x, y);
  };
  ApplyConfig a;
  a.car_sees_mask = car_sees_mask;
  const float tan55 = std::tan(55.0f * 3.14159265f / 180.0f);
  const float z_lo = 0.15f + 0.28f / tan55, z_hi = 0.45f + 0.32f / tan55;
  std::vector<uint8_t> column(size_t(rig.cam.width), 0);
  for (int y = 0; y < rig.cam.height; ++y) {
    for (int x = 0; x < rig.cam.width; ++x) {
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f || !DepthIsVehicle(tc)) {
        continue;
      }
      const Vec3 p = RigPoint(rig, im, x, y);
      if (std::fabs(p.y - 1.0f) > 0.004f || p.z < 0.02f || p.z > 1.08f || std::fabs(p.x) > 0.7f) {
        continue;  // the lid's top face
      }
      const bool post = std::fabs(std::fabs(p.x) - 0.5f) < 0.1f && p.z > 0.2f && p.z < 0.62f;  // the posts' own shadows, and their penumbrae, are on the lid too
      const bool geometry = p.z > z_lo + 0.02f && p.z < z_hi - 0.02f && !post;
      const bool open = (p.z < z_lo - 0.03f || p.z > z_hi + 0.03f) && !post;
      if (!geometry && !open) {
        continue;
      }
      const float f = ApplyFactor(rig.cam, depth, vis, mask, 0, a, base.to_sun, x, y);
      if (geometry) {
        ++r.shadow;
        r.lo = std::min(r.lo, f);
        r.hi = std::max(r.hi, f);
        if (f > 0.5f) {
          ++r.bright;
          column[size_t(x)] = 1;
        }
      } else {
        ++r.open;
        r.open_dark += f < 0.9f ? 1 : 0;
      }
    }
  }
  for (uint8_t c : column) {
    r.bright_columns += c;
  }
  return r;
}

void GameMaskIsNotACars() {
  Check(CarSunVisibility(true, 0.0f) == 1.0f && CarSunVisibility(true, 0.4f) == 1.0f && CarSunVisibility(false, 0.0f) == 0.0f &&
            CarSunVisibility(false, 0.4f) == 0.4f && CarSunVisibility(false, 1.0f) == 1.0f,
        "a vehicle's pixel takes a sun visibility of 1 whatever the mask holds; any other pixel takes the mask's");
  const MaskImage comb = MakeCombMask(1280, 720);
  const MaskImage lit = MakeMask(MaskKind::kAllLit, 1280, 720);
  const LidResult with_comb_old = LidFactors(comb, true);
  const LidResult with_comb_new = LidFactors(comb, false);
  const LidResult lit_old = LidFactors(lit, true);
  const LidResult lit_new = LidFactors(lit, false);
  std::printf("the lid under the wing, the sun at 55 degrees behind the camera: %d pixels in the plate's shadow, %d outside it. In the shadow, the apply's factor:\n"
              "  mask lit everywhere:               range %.3f to %.3f, above 0.5 on %d (old rule), range %.3f to %.3f, %d (new rule)\n"
              "  mask with the fence's columns:     range %.3f to %.3f, above 0.5 on %d pixels in %d columns (old rule); range %.3f to %.3f, %d (new rule)\n"
              "  outside the shadow, darkened under 0.9: old %d of %d, new %d of %d\n",
              with_comb_new.shadow, with_comb_new.open, double(lit_old.lo), double(lit_old.hi), lit_old.bright, double(lit_new.lo), double(lit_new.hi), lit_new.bright,
              double(with_comb_old.lo), double(with_comb_old.hi), with_comb_old.bright, with_comb_old.bright_columns, double(with_comb_new.lo), double(with_comb_new.hi),
              with_comb_new.bright, with_comb_old.open_dark, with_comb_old.open, with_comb_new.open_dark, with_comb_new.open);
  Check(with_comb_new.shadow > 1500 && with_comb_new.open > 1500, Num("the lid has pixels to judge: %.0f in the plate's shadow, %.0f outside it", double(with_comb_new.shadow), double(with_comb_new.open)));
  // The map is not the source: with the mask lit everywhere the shadow on the lid is one flat darkness, whichever rule.
  Check(lit_new.bright == 0 && lit_new.hi - lit_new.lo < 0.02f && lit_old.bright == 0 && lit_old.hi - lit_old.lo < 0.02f,
        Num("with the mask lit everywhere the lid under the wing is one flat darkness (%.3f to %.3f): the map and the march make no lines", double(lit_new.lo), double(lit_new.hi)));
  // The comb is the mask's: the old rule shows the fence's columns as bright lines in the shadow, about 3 of every 26 columns.
  Check(with_comb_old.bright * 12 > with_comb_old.shadow && with_comb_old.bright_columns > 20 && with_comb_old.hi > 0.99f,
        Num("with the fence's columns in the mask the old rule cuts the wing's shadow with bright lines: %.0f of %.0f pixels in %.0f columns", double(with_comb_old.bright),
            double(with_comb_old.shadow), double(with_comb_old.bright_columns)));
  // The fix: a vehicle's pixel takes the mask as lit, so the shadow is as flat as under the lit mask, the same range to the bit.
  Check(with_comb_new.bright == 0 && with_comb_new.lo == lit_new.lo && with_comb_new.hi == lit_new.hi,
        Num("the new rule: no line (%.0f pixels above 0.5), the lid's factor %.3f to %.3f as under the lit mask", double(with_comb_new.bright), double(with_comb_new.lo), double(with_comb_new.hi)));
  // And nothing else changes on the lid: outside the plate's shadow the lit lid stays lit, under both rules.
  Check(with_comb_new.open_dark == 0 && with_comb_old.open_dark == 0, "outside the plate's shadow the lid is not darkened, under either rule");
}

// The skip leaves a vehicle's pixels and their neighbours to the march: the factor with the skip is the factor without it, to the bit, in a frame with
// cars; the control (the skip as it was, on a frame in which the apply gives a vehicle's pixel a sun visibility of 1) is not.
void SkipLeavesACarsPixels() {
  const Rig rig = MakeWingRig({0.0f, 2.4f, -4.5f}, {0.0f, 1.1f, 0.5f});
  SkipFrame f = MakeSkipFrameOf(rig.cam, RenderRig(rig).depth, rig.ToView(Sun(55.0, 180.0)), false);
  const ApplyConfig usual;
  long vehicle_pixels = 0;
  for (int y = 0; y < rig.cam.height; ++y) {
    for (int x = 0; x < rig.cam.width; ++x) {
      vehicle_pixels += DepthIsVehicle(f.Depth(x, y)) ? 1 : 0;
    }
  }
  Check(!VehicleInReach([](int, int) { return DepthWithVehicleBit(5.0f, false); }, 8, 8, 4, 4) &&
            VehicleInReach([](int x, int y) { return DepthWithVehicleBit(5.0f, x == 5 && y == 5); }, 8, 8, 4, 4) &&
            !VehicleInReach([](int x, int y) { return DepthWithVehicleBit(5.0f, x == 6 && y == 4); }, 8, 8, 4, 4) &&
            VehicleInReach([](int x, int y) { return DepthWithVehicleBit(5.0f, x == 0 && y == 0); }, 8, 8, 0, 0),
        "VehicleInReach is the pixel and its 8 neighbours (clamped at the border), no more");
  Check(vehicle_pixels > 5000, Num("the frame has cars: %.0f vehicle pixels", double(vehicle_pixels)));
  for (MaskKind kind : {MaskKind::kAllShadow, MaskKind::kHalf, MaskKind::kBlocks, MaskKind::kStripe}) {
    const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
    const SkipResult r = RunSkip(f, mask, usual, 0, 1);
    const SkipResult control = RunSkip(f, mask, usual, 0, 1, false);
    std::printf("skip with cars, %-15s: %ld vehicle pixels; marched %ld, skipped %ld (without the exemption %ld); compared %ld, differing %ld, filtering beside a skipped one %ld "
                "(control: filtering beside a skipped one %ld, differing %ld)\n",
                MaskName(kind), vehicle_pixels, r.marched, r.skipped, control.skipped, r.compared, r.differing, r.filtered_next_to_skipped, control.filtered_next_to_skipped,
                control.differing);
    const std::string row = std::string("a frame with cars, ") + MaskName(kind);
    Check(r.differing == 0 && r.picture_differing == 0 && r.filtered_next_to_skipped == 0,
          row + Num(": the factor with the skip is the factor without it, to the bit (%.0f of %.0f differ; %.0f pixels filter beside a skipped one)", double(r.differing),
                    double(r.compared), double(r.filtered_next_to_skipped)));
    if (kind == MaskKind::kAllShadow) {
      Check(r.skipped < control.skipped && control.filtered_next_to_skipped > 0,
            row + Num(": the exemption keeps the cars' pixels out of the skip (skipped %.0f against %.0f without it), and without it a vehicle's pixel filters beside a skipped one (%.0f)",
                      double(r.skipped), double(control.skipped), double(control.filtered_next_to_skipped)));
    }
  }
}

constexpr double kShadeStored = 0.2;  // a stored value under the floor of 0.25 (51 of 255): a car sun visibility of exactly 0

// ---------------------------------------------------------------------------
// a car's own sun visibility: the game's car sun image (CAR-SUNVIS, 2026-11-19; contact_trace.h "a car's own sun visibility")
// ---------------------------------------------------------------------------

// The game's image as the car shaders fetch it: 256 x 256 texels of one metre, UNORM 8, the centre at the truncated camera, bilinear and clamped. `stored(x, z)` is
// the value the game would have written for the ground at the world position (x, z) (a texel's centre).
struct CarSunImage {
  std::vector<uint8_t> texel;  // row v, column u
  float Texel(int i, int j) const {
    i = std::clamp(i, 0, 255);
    j = std::clamp(j, 0, 255);
    return float(texel[size_t(j) * 256u + size_t(i)]) / 255.0f;
  }
  float At(float u, float v) const {
    const float tu = u * 256.0f - 0.5f, tv = v * 256.0f - 0.5f;
    const int i0 = int(std::floor(tu)), j0 = int(std::floor(tv));
    const float fu = tu - float(i0), fv = tv - float(j0);
    const float top = Texel(i0, j0) * (1.0f - fu) + Texel(i0 + 1, j0) * fu;
    const float bottom = Texel(i0, j0 + 1) * (1.0f - fu) + Texel(i0 + 1, j0 + 1) * fu;
    return top * (1.0f - fv) + bottom * fv;
  }
};
template <class Stored>
CarSunImage MakeCarSunImage(const double camera[3], Stored&& stored) {
  CarSunImage im;
  im.texel.resize(256u * 256u);
  const double cx = std::trunc(camera[0]), cz = std::trunc(camera[2]);
  for (int j = 0; j < 256; ++j) {
    for (int i = 0; i < 256; ++i) {
      const double wx = cx + double(i) + 0.5 - 128.0, wz = cz + double(j) + 0.5 - 128.0;
      im.texel[size_t(j) * 256u + size_t(i)] = uint8_t(std::lround(std::clamp(stored(wx, wz), 0.0, 1.0) * 255.0));
    }
  }
  return im;
}

// A camera pose with some yaw and pitch, the game's way (the rows of the view inverse), in a world far from the origin and with negative coordinates (trunc goes
// towards 0, not down): the lookup against what the game's shader works out from the world position.
void CarSunLookupIsTheGames() {
  const double positions[][3] = {{817.62, 17.6, -808.4}, {-812.34, 20.0, 791.77}, {0.4, 3.0, -0.9}, {-0.2, 1.0, 0.3}, {1234.99, 5.0, -1234.01}};
  double worst = 0.0, worst_world = 0.0;
  int points = 0;
  for (const auto& position : positions) {
    for (int k = 0; k < 7; ++k) {
      Rig rig;
      rig.eye = {float(position[0]), float(position[1]), float(position[2])};
      rig.yaw = 0.9f * float(k) - 2.7f;
      rig.pitch = 0.11f * float(k) - 0.3f;
      const double right[3] = {rig.Right().x, rig.Right().y, rig.Right().z};
      const double up[3] = {rig.Up().x, rig.Up().y, rig.Up().z};
      const double forward[3] = {rig.Forward().x, rig.Forward().y, rig.Forward().z};
      const CarSunLookup l = MakeCarSunLookup(right, up, forward, position);
      for (const Vec3 pc : {Vec3{0.0f, 0.0f, 5.0f}, Vec3{-3.0f, 1.2f, 12.0f}, Vec3{4.0f, -2.0f, 30.0f}, Vec3{10.0f, 3.0f, 79.0f}, Vec3{-20.0f, -5.0f, 60.0f}}) {
        // the game's own: world = position + x right + y up + z forward (the view inverse's rows; forward is minus row 2)
        const double world[3] = {position[0] + double(pc.x) * right[0] + double(pc.y) * up[0] + double(pc.z) * forward[0],
                                 position[1] + double(pc.x) * right[1] + double(pc.y) * up[1] + double(pc.z) * forward[1],
                                 position[2] + double(pc.x) * right[2] + double(pc.y) * up[2] + double(pc.z) * forward[2]};
        float u, v, gu, gv;
        CarSunUv(l, pc, &u, &v);
        CarSunUvOfWorld(world, position, &gu, &gv);
        worst = std::max(worst, double(std::max(std::fabs(u - gu), std::fabs(v - gv))));
        // and in texels (one metre each): the lookup is within a hundredth of a texel everywhere
        worst_world = std::max(worst_world, 256.0 * double(std::max(std::fabs(u - gu), std::fabs(v - gv))));
        ++points;
      }
    }
  }
  std::printf("the car sun image's lookup against the game's formula: %d points over 5 positions (negative and far ones) and 7 aimings: worst %.2e in uv, %.4f texel\n", points, worst, worst_world);
  Check(points == 175 && worst_world < 0.01, Num("the lookup's u and v are the game's (world - trunc(camera)) / 256 + 0.5 to %.4f of a texel", worst_world));
  // the truncation is towards 0: at x = -812.34 the image's centre is -812, not -813
  double minus[3] = {-812.34, 0.0, 791.77};
  float u, v;
  const double at_centre[3] = {-812.0, 0.0, 791.0};
  CarSunUvOfWorld(at_centre, minus, &u, &v);
  Check(std::fabs(u - 0.5f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f, "the world point at trunc(camera) is the image's centre, for a negative camera too");
  // the stored value the car shaders turn into the sun's visibility: 1 lit (0.75 over the floor, times 4/3), 0 at the floor and below
  Check(std::fabs(CarSunFromImage(1.0f) - 1.0f) < 1e-5f && CarSunFromImage(0.25f) == 0.0f && CarSunFromImage(0.0f) == 0.0f && CarSunFromImage(0.4f) > 0.19f && CarSunFromImage(0.4f) < 0.21f &&
            CarSunFromImage(0.5f) > 0.33f && CarSunFromImage(0.5f) < 0.34f && std::fabs(CarSunFromImage(7.0f) - 1.0f) < 1e-5f,
        "the car's sun term is saturate(s - 0.25) * 4/3: 1 (to 1e-5) at a stored 1, 0 at 0.25 and below");
  float last = -1.0f;
  bool monotone = true;
  for (int i = 0; i <= 255; ++i) {
    const float f = CarSunFromImage(float(i) / 255.0f);
    monotone = monotone && f >= last && f >= 0.0f && f <= 1.0f;
    last = f;
  }
  Check(monotone, "the car's sun term never falls as the stored value rises, and stays in 0..1");
  // the pixels of the world are the mask's, whatever the image says; a vehicle's pixel takes the image's, or 1 where the frame has none
  const CarSunLookup unit;
  const auto lit = [](float, float) { return 1.0f; };
  const auto shade = [](float, float) { return 0.2f; };
  bool world_ok = true;
  for (float m : {0.0f, 0.3f, 1.0f}) {
    world_ok = world_ok && CarAwareSunVisibility(false, m, true, unit, Vec3{0.0f, 0.0f, 10.0f}, shade) == m && CarAwareSunVisibility(false, m, false, unit, Vec3{}, lit) == m;
  }
  Check(world_ok, "a pixel of the world takes the mask's visibility, with an image in the frame or without");
  Check(CarAwareSunVisibility(true, 0.0f, false, unit, Vec3{}, shade) == 1.0f && CarAwareSunVisibility(true, 0.0f, true, unit, Vec3{0.0f, 0.0f, 10.0f}, shade) == 0.0f &&
            std::fabs(CarAwareSunVisibility(true, 0.0f, true, unit, Vec3{0.0f, 0.0f, 10.0f}, lit) - 1.0f) < 1e-5f,
        "a vehicle's pixel takes the image's visibility (1 lit, 0 in shade), and 1 in a frame without an image, whatever the mask holds");
}

// The lid under the R34's wing (the rig of GameMaskIsNotACars), the pause camera's kind of view, the sun at 55 degrees behind the camera, with the march made
// once and the apply run over it for any mask and any image.
struct LidCase {
  Rig rig;
  RigImage im;
  Vec3 sun;
  CarMapRig map;
  Trace base;
  std::vector<float> vis;
  struct Px {
    int x, y;
    Vec3 p;
    bool geometry, open;
  };
  std::vector<Px> pixels;
  double position[3];  // the rig's eye in the game's world: the rig's own coordinates plus where the car stands
  LidCase(const LidCase&) = delete;
  LidCase& operator=(const LidCase&) = delete;
  LidCase() {
    rig = MakeWingRig({0.0f, 2.4f, -4.5f}, {0.0f, 1.1f, 0.5f});
    im = RenderRig(rig);
    sun = Sun(55.0, 180.0);
    map = BuildCarMap(rig, sun, 1024, 2.0f);
    base = LowSunTrace(55.0);
    base.camera = rig.cam;
    base.to_sun = rig.ToView(sun);
    base.car_map = &map.view;
    vis.assign(im.depth.d.size(), std::nanf(""));
    position[0] = 817.6 + double(rig.eye.x);
    position[1] = 17.6 + double(rig.eye.y);
    position[2] = -808.4 + double(rig.eye.z);
    const float tan55 = std::tan(55.0f * 3.14159265f / 180.0f);
    const float z_lo = 0.15f + 0.28f / tan55, z_hi = 0.45f + 0.32f / tan55;
    for (int y = 0; y < rig.cam.height; ++y) {
      for (int x = 0; x < rig.cam.width; ++x) {
        const float tc = im.depth.At(x, y);
        if (tc >= 30000.0f || !DepthIsVehicle(tc)) {
          continue;
        }
        const Vec3 p = RigPoint(rig, im, x, y);
        if (std::fabs(p.y - 1.0f) > 0.004f || p.z < 0.02f || p.z > 1.08f || std::fabs(p.x) > 0.7f) {
          continue;  // the lid's top face
        }
        const bool post = std::fabs(std::fabs(p.x) - 0.5f) < 0.1f && p.z > 0.2f && p.z < 0.62f;
        const bool geometry = p.z > z_lo + 0.02f && p.z < z_hi - 0.02f && !post;
        const bool open = (p.z < z_lo - 0.03f || p.z > z_hi + 0.03f) && !post;
        if (geometry || open) {
          pixels.push_back({x, y, p, geometry, open});
        }
      }
    }
  }
  float Vis(int x, int y) {
    float& slot = vis[size_t(y) * size_t(rig.cam.width) + size_t(x)];
    if (std::isnan(slot)) {
      Trace t = base;
      t.noise = PixelNoise(x, y);
      slot = March(t, [&](int a, int b) { return im.depth.At(a, b); }, x, y);
    }
    return slot;
  }
  // The lookup the host would give the apply for this camera (MakeCarSunLookup of the rig's pose in the game's world).
  CarSunLookup Lookup() const {
    const double right[3] = {rig.Right().x, rig.Right().y, rig.Right().z};
    const double up[3] = {rig.Up().x, rig.Up().y, rig.Up().z};
    const double forward[3] = {rig.Forward().x, rig.Forward().y, rig.Forward().z};
    return MakeCarSunLookup(right, up, forward, position);
  }
  // A lid pixel's place in the game's world (the rig's point plus where the car stands).
  double WorldX(const Px& px) const { return double(px.p.x) + 817.6; }
};

struct LidStats {
  int shadow = 0, bright = 0, bright_columns = 0;
  float lo = 1.0f, hi = 0.0f;  // the range of the factors in the plate's shadow
  int open = 0, open_dark = 0;
};
LidStats MeasureLid(LidCase& c, const MaskImage& mask, const ApplyConfig& a) {
  LidStats r;
  const auto depth = [&](int x, int y) { return c.im.depth.At(x, y); };
  const auto vis = [&](int x, int y) { return c.Vis(x, y); };
  std::vector<uint8_t> column(size_t(c.rig.cam.width), 0);
  for (const LidCase::Px& px : c.pixels) {
    const float f = ApplyFactor(c.rig.cam, depth, vis, mask, 0, a, c.base.to_sun, px.x, px.y);
    if (px.geometry) {
      ++r.shadow;
      r.lo = std::min(r.lo, f);
      r.hi = std::max(r.hi, f);
      if (f > 0.5f) {
        ++r.bright;
        column[size_t(px.x)] = 1;
      }
    } else {
      ++r.open;
      r.open_dark += f < 0.9f ? 1 : 0;
    }
  }
  for (uint8_t col : column) {
    r.bright_columns += col;
  }
  return r;
}

// The case the owner asked for, three ways (the rig, the sun at 55 degrees behind the camera, the plate's shadow on the lid):
//   1. a lit car under its own wing: the car sun image says lit (1) over the car: the wing's shadow is there, flat, as the first fix had it (to the bit);
//   2. a car in a building's shade: the image says shade over the whole car: the sun's share is 0, the contact term adds nothing (the factor is 1 on every pixel of
//      the lid): the first fix (visibility 1 everywhere) darkened the plate's shadow there as for a lit car; a frame without the image still does;
//   3. the fence's comb: the mask has the iron fence's columns (zero 3 pixels in 26), the image is lit: no line (as the first fix), and with a shade edge across the car no line either
//      (the lit part is flat, the shaded part is 1).
void CarSunVisibilityOnTheLid() {
  auto lid = std::make_unique<LidCase>();
  LidCase& c = *lid;
  const MaskImage lit_mask = MakeMask(MaskKind::kAllLit, 1280, 720);
  const MaskImage comb = MakeCombMask(1280, 720);
  const MaskImage shade_mask = MakeMask(MaskKind::kAllShadow, 1280, 720);
  const CarSunLookup lookup = c.Lookup();

  // the lookup is right for this rig: a lid pixel's world position from the lookup is the rig's point plus where the car stands (x and z; the lookup has no y)
  {
    const auto depth = [&](int x, int y) { return c.im.depth.At(x, y); };
    double worst = 0.0;
    int n = 0;
    for (size_t i = 0; i < c.pixels.size(); i += 37) {
      const LidCase::Px& px = c.pixels[i];
      const Vec3 pc = ViewPos(c.rig.cam, float(px.x) + 0.5f, float(px.y) + 0.5f, depth(px.x, px.y));
      float u, v;
      CarSunUv(lookup, pc, &u, &v);
      const double wx = double(px.p.x) + 817.6, wz = double(px.p.z) - 808.4;
      float gu, gv;
      const double world[3] = {wx, 0.0, wz};
      CarSunUvOfWorld(world, c.position, &gu, &gv);
      worst = std::max(worst, 256.0 * double(std::max(std::fabs(u - gu), std::fabs(v - gv))));
      ++n;
    }
    Check(n > 50 && worst < 0.05, Num("the lookup puts the lid's pixels where the rig's world has them: %.0f pixels, worst %.4f of a texel", double(n), worst));
  }

  const CarSunImage lit_image = MakeCarSunImage(c.position, [](double, double) { return 1.0; });
  const CarSunImage shade_image = MakeCarSunImage(c.position, [](double, double) { return kShadeStored; });
  ApplyConfig none;  // the frame has no image: a vehicle's pixel takes 1
  const auto with_image = [&](const CarSunImage& image) {
    ApplyConfig a;
    a.have_car_image = true;
    a.car_lookup = lookup;
    a.car_image = [&image](float u, float v) { return image.At(u, v); };
    return a;
  };
  const LidStats first_fix = MeasureLid(c, lit_mask, none);
  const LidStats lit_car = MeasureLid(c, lit_mask, with_image(lit_image));
  const LidStats shaded_car = MeasureLid(c, lit_mask, with_image(shade_image));
  const LidStats shaded_car_shade_mask = MeasureLid(c, shade_mask, with_image(shade_image));
  std::printf("the lid under the wing, the plate's shadow (%d pixels) and the open lid (%d):\n"
              "  no image in the frame (visibility 1, the first fix):  factor %.3f to %.3f in the shadow; %d of the open pixels under 0.9\n"
              "  lit car (image 1):                                    factor %.3f to %.3f; %d\n"
              "  car in a building's shade (image 0.2):               factor %.3f to %.3f; %d\n",
              first_fix.shadow, first_fix.open, double(first_fix.lo), double(first_fix.hi), first_fix.open_dark, double(lit_car.lo), double(lit_car.hi), lit_car.open_dark,
              double(shaded_car.lo), double(shaded_car.hi), shaded_car.open_dark);
  Check(first_fix.shadow > 1500 && first_fix.open > 1500, Num("the lid has pixels to judge: %.0f in the plate's shadow, %.0f outside it", double(first_fix.shadow), double(first_fix.open)));
  // 1. a lit car under its own wing keeps the wing's shadow, to the bit
  Check(std::fabs(lit_car.lo - first_fix.lo) < 1e-4f && std::fabs(lit_car.hi - first_fix.hi) < 1e-4f && lit_car.bright == 0 && lit_car.lo < 0.5f && lit_car.hi < 0.5f,
        Num("a lit car under its wing: the plate's shadow is there (%.3f to %.3f, flat, none above 0.5) and is what the first fix gave (to 1e-4: the image's bilinear 1)", double(lit_car.lo), double(lit_car.hi)));
  Check(lit_car.open_dark == 0, "a lit car: the open lid is not darkened");
  // 2. a car in a building's shade gets no sun contact darkening: the apply's factor is exactly 1 on every pixel of the lid, whichever mask is in the frame
  Check(shaded_car.lo == 1.0f && shaded_car.hi == 1.0f && shaded_car.open_dark == 0,
        Num("a car in a building's shade: no sun contact darkening on the lid (factor %.3f to %.3f; the first fix put it at %.3f)", double(shaded_car.lo), double(shaded_car.hi), double(first_fix.lo)));
  Check(shaded_car_shade_mask.lo == 1.0f && shaded_car_shade_mask.hi == 1.0f, "... and the same under a mask that says shade as well: the car never reads the screen mask");

  // 3. the fence: the mask's columns do not reach the car, with or without the image (lit)
  const LidStats fence_lit = MeasureLid(c, comb, with_image(lit_image));
  const LidStats fence_none = MeasureLid(c, comb, none);
  const LidStats fence_shade = MeasureLid(c, comb, with_image(shade_image));
  std::printf("  the fence's columns in the mask: lit image: %.3f to %.3f, %d above 0.5 in %d columns; no image: %.3f to %.3f, %d; shade image: %.3f to %.3f\n", double(fence_lit.lo),
              double(fence_lit.hi), fence_lit.bright, fence_lit.bright_columns, double(fence_none.lo), double(fence_none.hi), fence_none.bright, double(fence_shade.lo),
              double(fence_shade.hi));
  Check(fence_lit.bright == 0 && fence_lit.lo == lit_car.lo && fence_lit.hi == lit_car.hi && fence_none.bright == 0 && fence_none.lo == first_fix.lo && fence_none.hi == first_fix.hi,
        "the fence's columns in the mask make no comb on the lid, with a lit image and without an image: the factor is the lit mask's (to the bit: a vehicle's pixel never reads the mask)");
  Check(fence_shade.lo == 1.0f && fence_shade.hi == 1.0f, "a car in shade with the fence's columns in the mask: still no line (the factor is 1)");

  // a shade edge across the car: the image's texels are a metre, the car (the lid, 1.5 m wide) stands at x 816.85 to 818.35, texel centres at 816.5, 817.5, 818.5:
  // shade at and under 817.5, lit from 818.5, the bilinear sample between them
  const CarSunImage edge_image = MakeCarSunImage(c.position, [](double x, double) { return x < 817.9 ? kShadeStored : 1.0; });
  // by x: the plate's shadow darkness (1 - factor) of the pixels in columns of the world, and the sun's visibility the apply took (the image's value at the pixel)
  const auto depth = [&](int x, int y) { return c.im.depth.At(x, y); };
  const auto vis = [&](int x, int y) { return c.Vis(x, y); };
  const ApplyConfig edge_config = with_image(edge_image);
  double dark_left = 0.0, dark_right = 0.0;
  int n_left = 0, n_right = 0;
  float worst_left = 1.0f;
  bool monotone = true;
  std::vector<std::pair<double, double>> by_x;  // world x, darkness
  for (const LidCase::Px& px : c.pixels) {
    if (!px.geometry) {
      continue;
    }
    const float f = ApplyFactor(c.rig.cam, depth, vis, comb, 0, edge_config, c.base.to_sun, px.x, px.y);
    const double wx = c.WorldX(px);
    if (wx < 817.45) {
      dark_left += double(1.0f - f);
      worst_left = std::min(worst_left, f);
      ++n_left;
    } else if (wx > 818.1) {
      dark_right += double(1.0f - f);
      ++n_right;
    }
    by_x.push_back({wx, double(1.0f - f)});
  }
  // the darkness grows with the sun's visibility across the car: the mean darkness of x bins of 0.15 m never falls by more than a little
  std::sort(by_x.begin(), by_x.end());
  double previous = -1.0;
  for (size_t first = 0; first < by_x.size();) {
    size_t last = first;
    double sum = 0.0;
    while (last < by_x.size() && by_x[last].first < by_x[first].first + 0.15) {
      sum += by_x[last].second;
      ++last;
    }
    const double mean = sum / double(last - first);
    if (previous >= 0.0 && mean < previous - 0.03) {
      monotone = false;
    }
    previous = mean;
    first = last;
  }
  std::printf("  a shade edge across the car (the image: shade left of x 817.9, lit right of it; the comb in the mask): %d lid pixels in the shade (mean darkness %.3f, worst factor %.3f),"
              " %d on the lit side (mean darkness %.3f)\n",
              n_left, n_left ? dark_left / n_left : 0.0, double(worst_left), n_right, n_right ? dark_right / n_right : 0.0);
  Check(n_left > 100 && n_right > 20 && worst_left == 1.0f,
        Num("the part of the car over shaded ground has no sun contact darkening: %.0f pixels, the worst factor %.3f", double(n_left), double(worst_left)));
  Check(n_right > 20 && dark_right / n_right > 0.3, Num("the part over lit ground has the plate's shadow: mean darkness %.3f", n_right ? dark_right / n_right : 0.0));
  Check(monotone, "across the shade edge the darkness grows with the sun's visibility, no steps back (no comb at the fence's pitch either)");
}

// The skip leaves a vehicle's pixels to the march whatever the image says: the factor with the skip is the factor without it, to the bit, in a frame with cars
// and with a car sun image that is lit where the mask is 0 and shaded where the mask is lit (the cases the exemption is for).
void SkipWithACarSunImage() {
  const Rig rig = MakeWingRig({0.0f, 2.4f, -4.5f}, {0.0f, 1.1f, 0.5f});
  SkipFrame f = MakeSkipFrameOf(rig.cam, RenderRig(rig).depth, rig.ToView(Sun(55.0, 180.0)), false);
  const double position[3] = {817.6 + double(rig.eye.x), 17.6 + double(rig.eye.y), -808.4 + double(rig.eye.z)};
  const double right[3] = {rig.Right().x, rig.Right().y, rig.Right().z};
  const double up[3] = {rig.Up().x, rig.Up().y, rig.Up().z};
  const double forward[3] = {rig.Forward().x, rig.Forward().y, rig.Forward().z};
  const CarSunImage lit = MakeCarSunImage(position, [](double, double) { return 1.0; });
  const CarSunImage half = MakeCarSunImage(position, [](double x, double) { return x < 817.9 ? kShadeStored : 1.0; });
  for (const MaskKind kind : {MaskKind::kAllShadow, MaskKind::kBlocks}) {
    for (int which = 0; which < 2; ++which) {
      const CarSunImage& image = which == 0 ? lit : half;
      ApplyConfig a;
      a.have_car_image = true;
      a.car_lookup = MakeCarSunLookup(right, up, forward, position);
      a.car_image = [&image](float u, float v) { return image.At(u, v); };
      const MaskImage mask = MakeMask(kind, f.cam.width, f.cam.height);
      const SkipResult r = RunSkip(f, mask, a, 0, 1);
      std::printf("skip with cars and a car sun image (%s), %-15s: marched %ld, skipped %ld; compared %ld, differing %ld, filtering beside a skipped one %ld\n",
                  which == 0 ? "lit" : "shade edge", MaskName(kind), r.marched, r.skipped, r.compared, r.differing, r.filtered_next_to_skipped);
      Check(r.differing == 0 && r.picture_differing == 0 && r.filtered_next_to_skipped == 0,
            std::string("a frame with cars, ") + MaskName(kind) + (which == 0 ? ", a lit car sun image" : ", a car sun image with a shade edge") +
                Num(": the factor with the skip is the factor without it, to the bit (%.0f of %.0f differ)", double(r.differing), double(r.compared)));
    }
  }
}

// ---------------------------------------------------------------------------
// the vehicle march's stipple (contact_trace.h "the vehicle march's stipple")
// ---------------------------------------------------------------------------

// A value through the image's R16_FLOAT: the nearest half (ties to even) as a float, for 2^-24 and up.
float HalfRound(float v) {
  if (v == 0.0f || !std::isfinite(v)) {
    return v;
  }
  int e = 0;
  std::frexp(std::fabs(v), &e);  // |v| = m 2^e, m in [0.5, 1): a half keeps 11 significant bits, and nothing below 2^-24
  const float step = std::ldexp(1.0f, std::max(e - 11, -24));
  return std::copysign(std::nearbyint(std::fabs(v) / step) * step, v);
}

// The wing rig of MakeWingRig with the plate `plate` metres above the lid, at `height` lines.
Rig MakeWingRigAt(Vec3 eye, Vec3 target, float plate, int height) {
  Rig r;
  r.cam = MakeCamera(height);
  r.eye = eye;
  r.boxes.push_back({-0.75f, 0.75f, 0.97f, 1.0f, 0.0f, 1.1f, true});                  // the lid
  r.boxes.push_back({-0.8f, 0.8f, 0.35f, 0.97f, 0.0f, 2.5f, true});                   // the body below it and the rear window's deck
  r.boxes.push_back({-0.8f, 0.8f, 1.0f + plate, 1.04f + plate, 0.15f, 0.45f, true});  // the plate
  for (const float x : {-0.5f, 0.5f}) {
    r.boxes.push_back({x - 0.03f, x + 0.03f, 1.0f, 1.0f + plate, 0.25f, 0.31f, true});  // the posts
  }
  r.Aim(target);
  return r;
}

// The march's picture of a rig over a box of pixels, as the pass stores it (through the half), without and with the mark, and the picture the
// noise stands for: the mean of 16 starts of the ray (the noise 1/32, 3/32, ...), which is what a history of many frames would show.
struct StipplePictures {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1, w = 0;
  std::vector<float> plain, marked, ref;
  float At(const std::vector<float>& image, int x, int y) const { return image[size_t(y) * size_t(w) + size_t(x)]; }
};
StipplePictures MakeStipplePictures(const Rig& rig, const RigImage& im, const Trace& base, int lid_x0, int lid_y0, int lid_x1, int lid_y1, bool with_ref) {
  StipplePictures p;
  p.w = rig.cam.width;
  p.x0 = std::max(lid_x0 - 3, 2);
  p.y0 = std::max(lid_y0 - 3, 2);
  p.x1 = std::min(lid_x1 + 3, rig.cam.width - 3);
  p.y1 = std::min(lid_y1 + 3, rig.cam.height - 3);
  const size_t n = size_t(rig.cam.width) * size_t(rig.cam.height);
  p.plain.assign(n, 1.0f);
  p.marked.assign(n, 1.0f);
  p.ref.assign(n, 1.0f);
  const auto depth = [&](int x, int y) { return im.depth.At(x, y); };
  for (int y = p.y0 - 1; y <= p.y1 + 1; ++y) {
    for (int x = p.x0 - 1; x <= p.x1 + 1; ++x) {
      if (im.depth.At(x, y) >= 30000.0f) {
        continue;
      }
      Trace t = base;
      t.noise = PixelNoise(x, y);
      const size_t at = size_t(y) * size_t(rig.cam.width) + size_t(x);
      p.plain[at] = HalfRound(March(t, depth, x, y));
      p.marked[at] = HalfRound(MarchStored(t, depth, x, y));
      if (with_ref) {
        double sum = 0.0;
        for (int k = 0; k < 16; ++k) {
          t.noise = (float(k) + 0.5f) / 16.0f;
          sum += double(March(t, depth, x, y));
        }
        p.ref[at] = float(sum / 16.0);
      }
    }
  }
  return p;
}
// The filter of the apply (the twin) over one of the pictures at (x, y).
float FilterOf(const Rig& rig, const RigImage& im, const std::vector<float>& image, int x, int y) {
  const auto depth = [&](int a, int b) { return im.depth.At(a, b); };
  const auto vis = [&](int a, int b) { return image[size_t(b) * size_t(rig.cam.width) + size_t(a)]; };
  const float tc = im.depth.At(x, y);
  const Vec3 pc = ViewPos(rig.cam, float(x) + 0.5f, float(y) + 0.5f, tc);
  Vec3 normal = NormalFromDepth(rig.cam, depth, x, y, tc, pc);
  if (Dot(normal, pc) > 0.0f) {
    normal = normal * -1.0f;
  }
  return EdgeFilter(rig.cam, depth, vis, x, y, tc, pc, normal);
}

struct StippleTally {
  double energy_old = 0.0, energy_new = 0.0, energy_ref = 0.0;  // sums of |a pixel minus the mean of its four neighbours| over the lid's pixels
  double err_old = 0.0, err_new = 0.0;                          // sums of |the filtered pixel minus the reference|
  double dark_old = 0.0, dark_new = 0.0, dark_ref = 0.0;        // sums of 1 - the pixel
  long pixels = 0, scenarios = 0;
  double worst_old = 0.0, worst_new = 0.0, worst_ref = 0.0;     // the largest mean energy of one scenario
};

// The lid of a wing rig: its pixels and their box.
struct LidPixels {
  std::vector<uint8_t> lid;
  int x0, y0, x1, y1;
};
LidPixels FindLid(const Rig& rig, const RigImage& im) {
  LidPixels r;
  const int w = rig.cam.width, h = rig.cam.height;
  r.lid.assign(size_t(w) * size_t(h), 0);
  r.x0 = w;
  r.x1 = -1;
  r.y0 = h;
  r.y1 = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float tc = im.depth.At(x, y);
      if (tc >= 30000.0f || !DepthIsVehicle(tc)) {
        continue;
      }
      const Vec3 p = RigPoint(rig, im, x, y);
      if (std::fabs(p.y - 1.0f) > 0.004f || p.z < 0.02f || p.z > 1.08f || std::fabs(p.x) > 0.7f) {
        continue;  // the lid's top face
      }
      r.lid[size_t(y) * size_t(w) + size_t(x)] = 1;
      r.x0 = std::min(r.x0, x);
      r.x1 = std::max(r.x1, x);
      r.y0 = std::min(r.y0, y);
      r.y1 = std::max(r.y1, y);
    }
  }
  return r;
}

void VehicleStippleIsFiltered() {
  // 1. The mark itself: a marked value is outside 0 to 1 whatever the visibility, comes back through the half to within half a unit of its 2^-10, and an
  //    unmarked value is never read as marked.
  {
    int unmarked = 0, wrongly_marked = 0;
    double worst = 0.0;
    for (int i = 0; i <= 4096; ++i) {
      const float v = float(i) / 4096.0f;
      const float stored = HalfRound(MarkedVisibility(v));
      unmarked += IsMarked(stored) ? 0 : 1;
      worst = std::max(worst, double(std::fabs(UnmarkedVisibility(stored) - v)));
      wrongly_marked += IsMarked(HalfRound(v)) ? 1 : 0;
    }
    Check(unmarked == 0 && wrongly_marked == 0 && worst <= 0.0005,
          Num("the mark survives the R16_FLOAT image: %.0f marked values read as unmarked, %.0f plain values read as marked, the value back to within %.6f", double(unmarked),
              double(wrongly_marked), worst));
  }
  // 2. The lid under a wing seen from three places with the sun at two heights and three sides, the plate 28 and 18 cm above the lid, the car's map off (a player
  //    who chose CAR SHADOWS = OFF with CONTACT SHADOWS on): every pixel of the lid is a vehicle's and marches the short ray with the noise of its start and no
  //    history. The filter before this change kept the stipple (the visibility's weight is 0.2 wide: a pixel the ray hit and the next one it missed do not mix);
  //    the marked pixels mix (weight 1 between two of them), and the 3 x 3 of the start's noise is the smooth answer a history of frames would give.
  StippleTally t;
  long marked_pixels = 0, lid_pixels = 0;
  struct Eye {
    Vec3 eye, target;
  };
  const Eye eyes[] = {{{-3.0f, 1.6f, -3.0f}, {0.0f, 1.0f, 0.5f}}, {{-4.0f, 1.4f, 0.4f}, {0.0f, 1.1f, 0.4f}}, {{0.0f, 2.2f, -4.5f}, {0.0f, 1.1f, 0.5f}}};
  for (const float plate : {0.28f, 0.18f}) {
    for (const Eye& e : eyes) {
      const Rig rig = MakeWingRigAt(e.eye, e.target, plate, 1080);
      const RigImage im = RenderRig(rig);
      const int w = rig.cam.width, h = rig.cam.height;
      const LidPixels l = FindLid(rig, im);
      Check(l.x1 > l.x0 && l.y1 > l.y0, "the wing rig shows the lid");
      for (const double elevation : {55.0, 70.0}) {
        for (const double azimuth : {0.0, 120.0, 240.0}) {
          const Vec3 sun = Sun(elevation, azimuth);
          Trace base = LowSunTrace(elevation);
          base.camera = rig.cam;
          base.to_sun = rig.ToView(sun);
          const StipplePictures p = MakeStipplePictures(rig, im, base, l.x0, l.y0, l.x1, l.y1, true);
          double ref_dark = 0.0;
          for (int y = p.y0; y <= p.y1; ++y) {
            for (int x = p.x0; x <= p.x1; ++x) {
              ref_dark += l.lid[size_t(y) * size_t(w) + size_t(x)] ? 1.0 - double(p.At(p.ref, x, y)) : 0.0;
            }
          }
          if (ref_dark < 20.0) {
            continue;  // no shadow of the plate within the ray on this lid from this sun: nothing to smooth
          }
          std::vector<float> f_old(size_t(w) * size_t(h), 1.0f), f_new(size_t(w) * size_t(h), 1.0f);
          for (int y = p.y0; y <= p.y1; ++y) {
            for (int x = p.x0; x <= p.x1; ++x) {
              if (im.depth.At(x, y) < 30000.0f) {
                f_old[size_t(y) * size_t(w) + size_t(x)] = FilterOf(rig, im, p.plain, x, y);
                f_new[size_t(y) * size_t(w) + size_t(x)] = FilterOf(rig, im, p.marked, x, y);
              }
            }
          }
          StippleTally one;
          const auto lap = [&](const std::vector<float>& a, size_t i) {
            return double(a[i]) - 0.25 * (double(a[i - 1]) + double(a[i + 1]) + double(a[i - size_t(w)]) + double(a[i + size_t(w)]));
          };
          for (int y = p.y0 + 1; y < p.y1; ++y) {
            for (int x = p.x0 + 1; x < p.x1; ++x) {
              const size_t i = size_t(y) * size_t(w) + size_t(x);
              if (!l.lid[i]) {
                continue;
              }
              ++lid_pixels;
              marked_pixels += IsMarked(p.marked[i]) ? 1 : 0;
              if (!l.lid[i - 1] || !l.lid[i + 1] || !l.lid[i - size_t(w)] || !l.lid[i + size_t(w)]) {
                continue;
              }
              one.energy_old += std::fabs(lap(f_old, i));
              one.energy_new += std::fabs(lap(f_new, i));
              one.energy_ref += std::fabs(lap(p.ref, i));
              one.err_old += std::fabs(double(f_old[i]) - double(p.ref[i]));
              one.err_new += std::fabs(double(f_new[i]) - double(p.ref[i]));
              one.dark_old += 1.0 - double(f_old[i]);
              one.dark_new += 1.0 - double(f_new[i]);
              one.dark_ref += 1.0 - double(p.ref[i]);
              ++one.pixels;
            }
          }
          if (one.pixels == 0) {
            continue;
          }
          t.energy_old += one.energy_old;
          t.energy_new += one.energy_new;
          t.energy_ref += one.energy_ref;
          t.err_old += one.err_old;
          t.err_new += one.err_new;
          t.dark_old += one.dark_old;
          t.dark_new += one.dark_new;
          t.dark_ref += one.dark_ref;
          t.pixels += one.pixels;
          ++t.scenarios;
          t.worst_old = std::max(t.worst_old, one.energy_old / double(one.pixels));
          t.worst_new = std::max(t.worst_new, one.energy_new / double(one.pixels));
          t.worst_ref = std::max(t.worst_ref, one.energy_ref / double(one.pixels));
        }
      }
    }
  }
  const double n = double(std::max(t.pixels, 1L));
  std::printf("the lid under a wing, car map off, %ld scenarios, %ld pixels (%ld of the lid's %ld marked): stipple energy (mean |pixel - mean of its 4 neighbours|): "
              "old filter %.5f, marked filter %.5f, the noise's mean (what a history would show) %.5f; worst scenario %.5f, %.5f, %.5f; "
              "mean |filtered - mean| %.5f, %.5f; mean darkness %.5f, %.5f, mean's %.5f\n",
              t.scenarios, t.pixels, marked_pixels, lid_pixels, t.energy_old / n, t.energy_new / n, t.energy_ref / n, t.worst_old, t.worst_new, t.worst_ref, t.err_old / n,
              t.err_new / n, t.dark_old / n, t.dark_new / n, t.dark_ref / n);
  Check(t.scenarios >= 20 && t.pixels > 500000, Num("the sample is large enough (%.0f scenarios, %.0f lid pixels)", double(t.scenarios), double(t.pixels)));
  Check(marked_pixels > 0 && marked_pixels * 10 >= lid_pixels * 9, Num("the lid's pixels march with no map and are marked (%.0f of %.0f)", double(marked_pixels), double(lid_pixels)));
  Check(t.energy_old > 5.0 * t.energy_ref, Num("the old filter leaves the stipple: its energy is %.1f times the noise's mean's", t.energy_old / std::max(t.energy_ref, 1e-12)));
  Check(t.energy_new * 3.0 <= t.energy_old, Num("the marked filter takes the stipple out: its energy is %.2f of the old one's", t.energy_new / std::max(t.energy_old, 1e-12)));
  Check(t.energy_new <= 1.5 * t.energy_ref, Num("and leaves no more roughness than the noise's mean has by itself (%.2f times)", t.energy_new / std::max(t.energy_ref, 1e-12)));
  Check(t.worst_new * 4.0 <= t.worst_old, Num("in the worst scenario too (%.4f against %.4f)", t.worst_new, t.worst_old));
  Check(t.err_new * 2.0 <= t.err_old, Num("the filtered picture is nearer to the noise's mean than it was: %.5f against %.5f", t.err_new / n, t.err_old / n));
  Check(std::fabs(t.dark_new - t.dark_old) <= 0.03 * t.dark_old, Num("the shadow's total darkness stays (%.5f against %.5f a pixel)", t.dark_new / n, t.dark_old / n));
  // 3. With the car's map in the frame the lid's pixels are covered by it and take no steps: nothing is marked, and the filter is the old filter to the half's
  //    last unit, so the map's crisp shadow keeps its edge (a filter without the visibility's weight on every vehicle pixel would widen the map's edges to twice
  //    their width, which is why the mark is there).
  {
    long lid_n = 0, marked_n = 0, soft_old = 0, soft_new = 0, soft_all = 0, changed = 0;
    double worst = 0.0;
    const Rig rig = MakeWingRigAt(eyes[0].eye, eyes[0].target, 0.28f, 1080);
    const RigImage im = RenderRig(rig);
    const int w = rig.cam.width;
    const LidPixels l = FindLid(rig, im);
    for (const double elevation : {40.0, 55.0, 70.0}) {
      for (const double azimuth : {0.0, 90.0, 180.0, 270.0}) {
        const Vec3 sun = Sun(elevation, azimuth);
        const CarMapRig map = BuildCarMap(rig, sun, 1024, 2.0f);
        Trace base = LowSunTrace(elevation);
        base.camera = rig.cam;
        base.to_sun = rig.ToView(sun);
        base.car_map = &map.view;
        const StipplePictures p = MakeStipplePictures(rig, im, base, l.x0, l.y0, l.x1, l.y1, false);
        // The control: every vehicle pixel marked, as if the filter took the visibility's weight away between all of a car's pixels.
        std::vector<float> all = p.plain;
        for (size_t i = 0; i < all.size(); ++i) {
          all[i] = DepthIsVehicle(im.depth.d[i]) && im.depth.d[i] < 30000.0f ? HalfRound(MarkedVisibility(p.plain[i])) : p.plain[i];
        }
        for (int y = p.y0; y <= p.y1; ++y) {
          for (int x = p.x0; x <= p.x1; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (!l.lid[i]) {
              continue;
            }
            ++lid_n;
            const float c = FilterOf(rig, im, all, x, y);
            soft_all += (c > 0.05f && c < 0.95f) ? 1 : 0;
            marked_n += IsMarked(p.marked[i]) ? 1 : 0;
            const float a = FilterOf(rig, im, p.plain, x, y), b = FilterOf(rig, im, p.marked, x, y);
            soft_old += (a > 0.05f && a < 0.95f) ? 1 : 0;
            soft_new += (b > 0.05f && b < 0.95f) ? 1 : 0;
            worst = std::max(worst, double(std::fabs(a - b)));
            changed += std::fabs(a - b) > 0.002f ? 1 : 0;
          }
        }
      }
    }
    std::printf("the lid with the car's map in the frame, 12 suns: %ld lid pixels, %ld marked, soft pixels (0.05 to 0.95) old %ld, new %ld, with every vehicle pixel marked %ld; pixels the filter changes by more than 0.002: %ld, the most %.5f\n",
                lid_n, marked_n, soft_old, soft_new, soft_all, changed, worst);
    Check(lid_n > 500000 && marked_n * 200 <= lid_n, Num("with the map the lid takes no march steps and is not marked (%.0f of %.0f)", double(marked_n), double(lid_n)));
    Check(changed * 500 <= lid_n && soft_new <= soft_old + soft_old / 50,
          Num("and the map's shadow keeps its edge: %.0f of the lid's pixels change by more than 0.002, the soft pixels are %.0f against %.0f", double(changed), double(soft_new),
              double(soft_old)));
    Check(soft_all * 2 >= soft_old * 3, Num("(control) with every vehicle pixel marked the map's edges are %.2f times as wide", double(soft_all) / double(std::max(soft_old, 1L))));
  }
  // 4. Everything that is not a vehicle's pixel is filtered as it was: the road around a car (the tyres' and sills' shadows on it) reads the car's marked pixels
  //    beside it and takes them back out, so it differs only by the half's last unit.
  {
    const Rig rig = MakeTyreRig();
    const RigImage im = RenderRig(rig);
    long road = 0, differing = 0;
    double worst = 0.0;
    for (const double elevation : {15.0, 30.0}) {
      for (const double azimuth : {0.0, 90.0, 180.0, 270.0}) {
        const Vec3 sun = Sun(elevation, azimuth);
        Trace base = LowSunTrace(elevation);
        base.camera = rig.cam;
        base.to_sun = rig.ToView(sun);
        const StipplePictures p = MakeStipplePictures(rig, im, base, 2, 2, rig.cam.width - 3, rig.cam.height - 3, false);
        for (int y = 4; y < rig.cam.height - 4; y += 2) {
          for (int x = 4; x < rig.cam.width - 4; x += 2) {
            if (im.depth.At(x, y) >= 30000.0f || DepthIsVehicle(im.depth.At(x, y))) {
              continue;
            }
            ++road;
            const double d = std::fabs(double(FilterOf(rig, im, p.plain, x, y)) - double(FilterOf(rig, im, p.marked, x, y)));
            worst = std::max(worst, d);
            differing += d > 0.001 ? 1 : 0;
          }
        }
      }
    }
    std::printf("the road around a car, 8 suns: %ld pixels filtered, the most the marks change one: %.6f\n", road, worst);
    Check(road > 100000 && differing == 0, Num("a pixel that is not a vehicle's is filtered as it was, to the half's last unit (%.0f of %.0f differ by more than 0.001)", double(differing), double(road)));
  }
  // 5. A small shadow that must stay (the mirror on a door, a vehicle's pixels marching with the map off): the filter spreads it by a pixel but keeps it dark.
  {
    const Rig rig = MakeMirrorRig();
    const RigImage im = RenderRig(rig);
    long real = 0, dark_old = 0, dark_new = 0;
    double sum_old = 0.0, sum_new = 0.0;
    for (const double elevation : {20.0, 40.0}) {
      for (const double azimuth : {135.0, 270.0}) {
        const Vec3 sun = Sun(elevation, azimuth);
        Trace base = LowSunTrace(elevation);
        base.camera = rig.cam;
        base.to_sun = rig.ToView(sun);
        const StipplePictures p = MakeStipplePictures(rig, im, base, 2, 2, rig.cam.width - 3, rig.cam.height - 3, false);
        for (int y = 4; y < rig.cam.height - 4; ++y) {
          for (int x = 4; x < rig.cam.width - 4; ++x) {
            if (im.depth.At(x, y) >= 30000.0f || !DepthIsVehicle(im.depth.At(x, y))) {
              continue;
            }
            const Vec3 q = RigPoint(rig, im, x, y) + sun * 0.012f;
            if (rig.Nearest(q, sun, 0.0f, 0.15f) < 0.0f) {
              continue;
            }
            ++real;
            const float a = FilterOf(rig, im, p.plain, x, y), b = FilterOf(rig, im, p.marked, x, y);
            dark_old += a < 0.5f ? 1 : 0;
            dark_new += b < 0.5f ? 1 : 0;
            sum_old += 1.0 - double(a);
            sum_new += 1.0 - double(b);
          }
        }
      }
    }
    std::printf("the mirror on a door, 4 suns: %ld pixels with a shadow within 15 cm; filtered darker than 0.5: old %ld, new %ld; total darkness old %.0f, new %.0f\n", real, dark_old, dark_new,
                sum_old, sum_new);
    Check(real > 2000 && sum_new * 100.0 >= sum_old * 95.0, Num("the mirror's shadow on the door keeps its darkness (%.0f of %.0f)", sum_new, sum_old));
  }
  // 6. THE MARK AND THE RAY TRACED TERM (the ray tracing line, play build 11): the lid under the wing with the car's map off and the traced sun ray in the frame. The
  //    ray's answer is made here as the pass makes it, a ray from the pixel toward the sun against the rig's own boxes (exact, 0 or 1). A pixel the ray holds in the
  //    plate's shadow is stored plain (0), the lit ones that march are marked; the control marks every vehicle pixel that marches, as the march did before the rule.
  {
    long lid_n = 0, shadowed = 0, shadowed_marked = 0, lit_marching = 0, lit_marked = 0, soft_rule = 0, soft_control = 0, soft_plain = 0;
    double energy_rule = 0.0, energy_plain = 0.0, energy_control = 0.0, lit_energy_rule = 0.0, lit_energy_plain = 0.0;
    long lit_energy_n = 0;
    const Rig rig = MakeWingRigAt(eyes[0].eye, eyes[0].target, 0.28f, 1080);
    const RigImage im = RenderRig(rig);
    const int w = rig.cam.width, h = rig.cam.height;
    const LidPixels l = FindLid(rig, im);
    for (const double elevation : {40.0, 55.0, 70.0}) {
      for (const double azimuth : {0.0, 90.0, 180.0, 270.0}) {
        const Vec3 sun = Sun(elevation, azimuth);
        Trace base = LowSunTrace(elevation);
        base.camera = rig.cam;
        base.to_sun = rig.ToView(sun);
        const int x0 = std::max(l.x0 - 3, 2), y0 = std::max(l.y0 - 3, 2), x1 = std::min(l.x1 + 3, w - 3), y1 = std::min(l.y1 + 3, h - 3);
        const size_t n = size_t(w) * size_t(h);
        std::vector<float> rule(n, 1.0f), plain(n, 1.0f), control(n, 1.0f);  // the rule, the pictures with no mark at all, every marching vehicle pixel marked
        std::vector<uint8_t> in_shadow(n, 0);
        const auto depth = [&](int a, int b) { return im.depth.At(a, b); };
        for (int y = y0 - 1; y <= y1 + 1; ++y) {
          for (int x = x0 - 1; x <= x1 + 1; ++x) {
            if (im.depth.At(x, y) >= 30000.0f) {
              continue;
            }
            const size_t at = size_t(y) * size_t(w) + size_t(x);
            Trace t = base;
            t.noise = PixelNoise(x, y);
            // The traced sun ray: from the pixel's point, lifted a little, toward the sun, as long as the rig is tall.
            const Vec3 q = RigPoint(rig, im, x, y) + sun * 0.012f;
            t.traced_shadow = rig.Nearest(q, sun, 0.0f, 3.0f) >= 0.0f ? 1.0f : 0.0f;
            in_shadow[at] = t.traced_shadow >= kRtUnmarkedShadow ? 1 : 0;
            rule[at] = HalfRound(MarchStored(t, depth, x, y));
            const bool marked_by_rule = t.marked;
            const float visibility = March(t, depth, x, y);
            plain[at] = HalfRound(visibility);
            control[at] = HalfRound((DepthIsVehicle(im.depth.At(x, y)) && t.steps_used > 0) ? MarkedVisibility(visibility) : visibility);
            if (l.lid[at]) {
              ++lid_n;
              if (in_shadow[at]) {
                ++shadowed;
                shadowed_marked += marked_by_rule ? 1 : 0;
              } else {
                ++lit_marching;
                lit_marked += marked_by_rule ? 1 : 0;
              }
            }
          }
        }
        const auto lap = [&](const std::vector<float>& a, size_t i) {
          return double(a[i]) - 0.25 * (double(a[i - 1]) + double(a[i + 1]) + double(a[i - size_t(w)]) + double(a[i + size_t(w)]));
        };
        std::vector<float> f_rule(n, 1.0f), f_plain(n, 1.0f), f_control(n, 1.0f);
        const auto filter_all = [&](const std::vector<float>& src, std::vector<float>& dst) {
          for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
              if (im.depth.At(x, y) < 30000.0f) {
                dst[size_t(y) * size_t(w) + size_t(x)] = FilterOf(rig, im, src, x, y);
              }
            }
          }
        };
        filter_all(rule, f_rule);
        filter_all(plain, f_plain);
        filter_all(control, f_control);
        for (int y = y0 + 1; y < y1; ++y) {
          for (int x = x0 + 1; x < x1; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (!l.lid[i]) {
              continue;
            }
            soft_rule += (f_rule[i] > 0.05f && f_rule[i] < 0.95f) ? 1 : 0;
            soft_plain += (f_plain[i] > 0.05f && f_plain[i] < 0.95f) ? 1 : 0;
            soft_control += (f_control[i] > 0.05f && f_control[i] < 0.95f) ? 1 : 0;
            if (!l.lid[i - 1] || !l.lid[i + 1] || !l.lid[i - size_t(w)] || !l.lid[i + size_t(w)]) {
              continue;
            }
            energy_rule += std::fabs(lap(f_rule, i));
            energy_plain += std::fabs(lap(f_plain, i));
            energy_control += std::fabs(lap(f_control, i));
            // The lit lid pixels alone (not next to the traced shadow): where the stipple of a false hit would show.
            if (!in_shadow[i] && !in_shadow[i - 1] && !in_shadow[i + 1] && !in_shadow[i - size_t(w)] && !in_shadow[i + size_t(w)]) {
              lit_energy_rule += std::fabs(lap(f_rule, i));
              lit_energy_plain += std::fabs(lap(f_plain, i));
              ++lit_energy_n;
            }
          }
        }
      }
    }
    std::printf("the lid with the ray traced sun term (exact, 0 or 1), the car's map off, 12 suns: %ld lid pixels, %ld in the traced shadow (%ld of them marked), %ld lit (%ld marked); "
                "soft pixels (0.05 to 0.95) after the filter: with the rule %ld, no mark at all %ld, every marching vehicle pixel marked %ld; roughness %.1f / %.1f / %.1f; "
                "roughness of the lit pixels away from the shadow, rule %.3f, no mark %.3f (%ld pixels)\n",
                lid_n, shadowed, shadowed_marked, lit_marching, lit_marked, soft_rule, soft_plain, soft_control, energy_rule, energy_plain, energy_control, lit_energy_rule,
                lit_energy_plain, lit_energy_n);
    Check(lid_n > 500000 && shadowed * 20 >= lid_n && lit_marching * 4 >= lid_n,
          Num("the sample has the plate's traced shadow on the lid and lit pixels (%.0f lid pixels, %.0f in the shadow, %.0f lit)", double(lid_n), double(shadowed), double(lit_marching)));
    Check(shadowed_marked == 0, Num("a pixel the traced sun ray holds in a shadow is stored plain, not marked (%.0f of %.0f marked)", double(shadowed_marked), double(shadowed)));
    Check(lit_marked * 100 >= lit_marching * 99, Num("the vehicle pixels the ray leaves lit still march and are marked (%.0f of %.0f)", double(lit_marked), double(lit_marching)));
    Check(soft_control >= soft_plain * 5, Num("(control) marking every marching vehicle pixel widens the traced shadow's edge: %.2f times the soft pixels of the unmarked picture", double(soft_control) / double(std::max(soft_plain, 1L))));
    Check(soft_rule * 100 <= soft_plain * 125, Num("with the rule the traced shadow keeps its edge: %.0f soft pixels against %.0f without any mark (the lit pixels beside it still average among themselves)", double(soft_rule), double(soft_plain)));
    Check(lit_energy_n > 100000 && lit_energy_rule * 2.0 <= lit_energy_plain + 1e-9,
          Num("and the lit pixels' stipple is still smoothed by the mark: roughness %.3f against %.3f without it", lit_energy_rule, lit_energy_plain));
  }
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
      {&apply, "kChromaScale", kChromaScale},
      {&common, "kEdgeSpatialSigma", kEdgeSpatialSigma}, {&common, "kEdgePlanePixels", kEdgePlanePixels},
      {&common, "kEdgePlaneFloor", kEdgePlaneFloor},
      {&contact, "kUnderHeight", kUnderHeight},     {&contact, "kUnderStepHigh", kUnderStepHigh},
      {&contact, "kUnderStepLow", kUnderStepLow},   {&contact, "kUnderSunSinMin", kUnderSunSinMin},
      {&contact, "kVehicleLength", kVehicleReceiverLength}, {&contact, "kVehicleThickness", kVehicleReceiverThickness},
      {&contact, "kVehicleSteps", double(kVehicleReceiverSteps)},
      {&contact, "kUnderJitterLow", kUnderJitterLow}, {&contact, "kUnderJitterSpan", kUnderJitterSpan},
      {&contact, "kUnderReach", kUnderReach},       {&contact, "kUnderEpsFloor", kUnderEpsFloor},
      {&contact, "kUnderEpsPixels", kUnderEpsPixels}, {&contact, "kUnderUpLow", kUnderUpLow},
      {&contact, "kUnderUpHigh", kUnderUpHigh},     {&contact, "kUnderCastBand", kUnderCastBand},
      {&contact, "kAdaptMinSteps", double(kAdaptMinSteps)}, {&apply, "kFoldedFilterSkip", kFoldedFilterSkip},
      {&contact, "kSkipWeight", kSkipWeight},       {&contact, "kSkipTexelMargin", kSkipTexelMargin},
      {&contact, "kSkipMaxTexels", kSkipMaxTexels},  {&contact, "kSkipBlock", kSkipBlock},
      {&contact, "kCarMapSunRadius", kCarMapSunRadius}, {&contact, "kCarMapMinFilterTexels", kCarMapMinFilterTexels},
      {&contact, "kCarMapMaxFilterTexels", kCarMapMaxFilterTexels}, {&contact, "kCarMapSearchMetres", kCarMapSearchMetres},
      {&contact, "kCarMapSearchMinTexels", kCarMapSearchMinTexels}, {&contact, "kCarMapNormalOffsetTexels", kCarMapNormalOffsetTexels},
      {&contact, "kCarMapBiasFloor", kCarMapBiasFloor}, {&contact, "kCarMapBiasTexels", kCarMapBiasTexels},
      {&contact, "kCarMapFootprintBias", kCarMapFootprintBias}, {&contact, "kCarMapProducerSlope", kCarMapProducerSlope},
      {&contact, "kCarMapMaxTan", kCarMapMaxTan}, {&contact, "kCarMapEdgeTexels", kCarMapEdgeTexels},
      {&contact, "kCarMapEmpty", kCarMapEmpty},       {&common, "kVehicleMarkOffset", kVehicleMarkOffset},
      {&contact, "kRtUnmarkedShadow", kRtUnmarkedShadow},
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
  // The car's shadow map: the flags (bits 28 to 30) and the lookup written as the twin's (CarMapGather, CarMapShadow, March).
  Check(contact.find("static const uint kCarMapFlag = 1u << 28;") != std::string::npos && kCarMapFlag == (1u << 28) &&
            contact.find("static const uint kCarMapViewFlag = 1u << 29;") != std::string::npos && kCarMapViewFlag == (1u << 29) &&
            contact.find("static const uint kCarMapKeepMarchFlag = 1u << 30;") != std::string::npos && kCarMapKeepMarchFlag == (1u << 30),
        "the shader's car map flags are the header's (bits 28, 29, 30)");
  Check(contact.find("float4(base.x, base.x + 1.0f, base.x + 1.0f, base.x) + 0.5f - centre.x") != std::string::npos &&
            contact.find("float4(base.y + 1.0f, base.y + 1.0f, base.y, base.y) + 0.5f - centre.y") != std::string::npos &&
            contact.find("weight = float4((1.0f - f.x) * f.y, f.x * f.y, f.x * (1.0f - f.y), (1.0f - f.x) * (1.0f - f.y));") != std::string::npos &&
            contact.find("(base + 1.0f) * inv_size") != std::string::npos && contact.find("const float2 f = tex - 0.5f - base;") != std::string::npos,
        "the shader's gather is the twin's (the quad of the texel position, the gather's order (i0 j1) (i1 j1) (i1 j0) (i0 j0), the plane at each texel)");
  Check(contact.find("const float facing_map = max(-dot(normal, axis_d), 1.0f / kCarMapMaxTan);") != std::string::npos &&
            contact.find("const float3 p = pc + normal * (kCarMapNormalOffsetTexels * texel_m * sin_t);") != std::string::npos &&
            contact.find("const float2 gradient = clamp(texel_m * float2(dot(normal, axis_u), dot(normal, axis_v)) / facing_map, -limit, limit);") != std::string::npos &&
            contact.find("const float bias = kCarMapBiasFloor + kCarMapBiasTexels * texel_m + kCarMapFootprintBias * footprint;") != std::string::npos &&
            contact.find("const float cover = bias + kCarMapProducerSlope * max(abs(gradient.x), abs(gradient.y));") != std::string::npos &&
            contact.find("covered = dot(occupied * (float4)(over > -cover), 1.0f) > 0.5f;") != std::string::npos,
        "the shader's lookup is the twin's (the map's own axes, the normal offset, the receiver plane, the bias, the cover)");
  Check(contact.find("const float r0 = clamp(kCarMapSearchMetres / texel_m, kCarMapSearchMinTexels, kCarMapMaxFilterTexels);") != std::string::npos &&
            contact.find("const float2 ring[4] = {d0, d1, -d0, -d1};") != std::string::npos &&
            contact.find("const float r1 = clamp(blocker_sum / blocker_count * kCarMapSunRadius / texel_m, kCarMapMinFilterTexels, kCarMapMaxFilterTexels);") != std::string::npos &&
            contact.find("const float2 a45 = (d0 + d1) * 0.70710678118f;") != std::string::npos &&
            contact.find("return saturate(total * 0.2f) * saturate(edge / kCarMapEdgeTexels);") != std::string::npos &&
            contact.find("if (blocker_count < 0.5f) {") != std::string::npos,
        "the shader's blocker search and filter are the twin's (a ring at the search radius, the penumbra's half width, a ring an eighth turned, five taps)");
  // The world's filter on the map (CarMapWorldFilter): the same numbers and the same loops as the twin's.
  Check(contact.find("static const float kCarMapSoftRadiusMetres = 0.03f;") != std::string::npos && kCarMapSoftRadiusMetres == 0.03f &&
            contact.find("static const float kCarMapSunTan = 0.0047123890f;") != std::string::npos && kCarMapSunTan == 0.0047123890f &&
            contact.find("static const float kCarMapFilterMaxRadii = 2.0f;") != std::string::npos && kCarMapFilterMaxRadii == 2.0f &&
            contact.find("static const int kCarMapFilterTaps = 16;") != std::string::npos && kCarMapFilterTaps == 16 &&
            contact.find("static const int kCarMapSearchTaps = 8;") != std::string::npos && kCarMapSearchTaps == 8 &&
            contact.find("static const float kCarMapFixedAngle = 2.399963f;") != std::string::npos && kCarMapFixedAngle == 2.399963f &&
            contact.find("static const float kCarMapGoldenCos = -0.7373688780783197f;") != std::string::npos && kCarMapGoldenCos == -0.7373688780783197f &&
            contact.find("static const float kCarMapGoldenSin = 0.6754902942615238f;") != std::string::npos && kCarMapGoldenSin == 0.6754902942615238f &&
            contact.find("static const uint kCarMapFilterPcss = 2u;") != std::string::npos && kCarMapFilterPcss == 2u,
        "the shader's world filter constants are the header's");
  Check(contact.find("const uint filter_code = uint(info.w + 0.5f);") != std::string::npos &&
            contact.find("const uint filter_mode = filter_code >> 8;") != std::string::npos &&
            contact.find("const float sun_times = float(filter_code & 255u) * (1.0f / 16.0f);") != std::string::npos &&
            contact.find("const float reach = filter_mode == kCarMapFilterPcss ? kCarMapFilterMaxRadii * soft : soft;") != std::string::npos &&
            contact.find("sincos(((g_push.slots2.y & ~kAoNoiseBlueFlag) != 0u) ? noise * 6.28318530718f : kCarMapFixedAngle, direction.y, direction.x);") != std::string::npos &&
            contact.find("radius = clamp(blocker_sum / blocker_count * kCarMapSunTan * sun_times / texel_m, kCarMapMinFilterTexels, reach);") != std::string::npos &&
            contact.find("const float w = 1.0f - 0.5f * at;") != std::string::npos &&
            contact.find("return saturate(sum / weights) * saturate(edge / kCarMapEdgeTexels);") != std::string::npos,
        "the shader's world filter is the twin's (the matrix's last component, the reach, the turn only with a frame, the penumbra, the weights)");
  Check(contact.find("if (facing > kSunFacingLow) {") != std::string::npos &&
            contact.find("if (covered && AoIsVehicle(tc) && (flags & kCarMapKeepMarchFlag) == 0u) {") != std::string::npos &&
            contact.find("steps = 0u;") != std::string::npos && contact.find("shadow = max(shadow, car_shadow);") != std::string::npos &&
            contact.find("const bool car_map = (flags & kCarMapFlag) != 0u && !skip_car_map;") != std::string::npos &&
            contact.find("CabinSkipMap(cabin_mode, asuint(tc), (flags & kCarMapViewFlag) != 0u)") != std::string::npos &&
            contact.find("return float4(1.0f - car_shadow * smoothstep(kSunFacingLow, kSunFacingHigh, facing), 0.0f, 0.0f, 0.0f);") != std::string::npos,
        "the shader's use of the term is the twin's (not read at a grazing angle, no vehicle march where covered, the darker of the two, the debug view)");
  // The vehicle rule and the under term's two columns are written as the twin's (UnderVehicle, UnderProbe, March).
  Check(contact.find("length_units = min(length_units, kVehicleLength);") != std::string::npos &&
            contact.find("thickness = min(thickness, kVehicleThickness);") != std::string::npos &&
            contact.find("steps = min(steps, uint(kVehicleSteps));") != std::string::npos && contact.find("if (AoIsVehicle(tc)) {") != std::string::npos,
        "the shader's vehicle rule is the twin's (a car's pixel: the length, the thickness and the steps clamped)");
  // The mark (contact_trace.h "the vehicle march's stipple"): the march marks a vehicle pixel that took steps and stores nothing marked otherwise; both filters
  // read it the same way (decode, and no visibility weight between two marked pixels).
  {
    size_t filters = 0, at = 0;
    const std::string weight = "const float off_range = (centre_marked && marked) ? 0.0f : (value - centre) / kEdgeRangeSigma;";
    while ((at = common.find(weight, at)) != std::string::npos) {
      ++filters;
      at += weight.size();
    }
    Check(contact.find("const bool marked = AoIsVehicle(tc) && steps > 0u && traced_shadow < kRtUnmarkedShadow;") != std::string::npos &&
              contact.find("return float4(marked ? visibility - kVehicleMarkOffset : visibility, 0.0f, 0.0f, 0.0f);") != std::string::npos &&
              common.find("bool AoIsMarked(float stored) { return stored < -0.5f; }") != std::string::npos &&
              common.find("float AoUnmarked(float stored) { return AoIsMarked(stored) ? stored + kVehicleMarkOffset : stored; }") != std::string::npos &&
              filters == 2 && common.find("const bool centre_marked = AoIsMarked(centre_stored);") != std::string::npos &&
              common.find("const float centre = AoUnmarked(centre_stored);") != std::string::npos &&
              common.find("const float value = AoUnmarked(stored);") != std::string::npos,
          "the shaders' mark is the twin's (the march marks a vehicle pixel that took steps and that the ray traced sun term does not hold in a shadow; the filter and its folded twin decode, and weigh two marked pixels by position and depth alone)");
  }
  // The comb fix (contact_trace.h "the game's sun shadow mask is not a car's"): the apply takes a vehicle's sun visibility as 1 in both its uses of
  // the mask, and the march's skip (and the statistics' twin of it) is not allowed on a vehicle's pixel or beside one.
  Check(apply.find("float CarAwareSunVisibility(float mask_vis, bool vehicle, float3 pc) {") != std::string::npos &&
            apply.find("    return mask_vis;") != std::string::npos && apply.find("return CarSunOn() ? CarSunVisibility(pc) : 1.0f;") != std::string::npos &&
            apply.find("const float sun_lit = sun * sun_vis_used;") != std::string::npos &&
            apply.find("return AoIsVehicle(AoLoadLinear(ContactDepthSlot(), pixel));") != std::string::npos,
        "the apply's sun visibility is the twin's (CarAwareSunVisibility: the world takes the mask, a vehicle's pixel the car sun image or 1, for the contact share and for the bounce light's T)");
  Check(contact.find("if ((flags & kSkipFlag) != 0u && MaskCannotLight(position.xy) && !VehicleInReach(depth_slot, pixel, isize, tc)) {") != std::string::npos &&
            contact.find("MaskCannotLight(position) && !VehicleInReach(depth_slot, pixel, int2(size), tc)") != std::string::npos &&
            contact.find("(dx != 0 || dy != 0) &&") != std::string::npos &&
            contact.find("AoIsVehicle(AoLoadLinear(depth_slot, clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1)))") != std::string::npos,
        "the shader's skip is the twin's (MarchWithSkip: the mask test, and then not a vehicle's pixel or one beside it)");
  // The car sun image (contact_trace.h "a car's own sun visibility"): the apply reads it for a vehicle's pixel with the game's own formula, and takes the
  // slots from the high halves of slots.x and slots.y (ao.cpp kCarSunOnFlag, kCarSunSlotShift).
  Check(apply.find("const float2 uv = float2(dot(a.xyz, pc) + a.w, dot(b.xyz, pc) + b.w) * (1.0f / 256.0f) + 0.5f;") != std::string::npos &&
            apply.find("return min(saturate(s - 0.25f) * 1.3333325f, 1.0f);") != std::string::npos &&
            apply.find("g_textures[g_push.slots.x >> 16].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).x;") != std::string::npos &&
            apply.find("const uint lookup = g_push.slots.y >> 16;") != std::string::npos &&
            apply.find("g_textures[lookup].Load(int3(0, 0, 0))") != std::string::npos && apply.find("g_textures[lookup].Load(int3(1, 0, 0))") != std::string::npos &&
            apply.find("bool CarSunOn() { return (g_push.slots.y & 0x100u) != 0u; }") != std::string::npos &&
            apply.find("return CarSunOn() ? CarSunVisibility(pc) : 1.0f;") != std::string::npos &&
            apply.find("sun_vis_used = CarAwareSunVisibility(sun_vis_used, PixelIsVehicle(pixel), pc);") != std::string::npos &&
            apply.find("gi_sun_vis = CarAwareSunVisibility(gi_sun_vis, PixelIsVehicle(pixel), pc);") != std::string::npos &&
            apply.find("g_textures_ms[g_push.slots.x & 0xFFFFu].Load(p, g_push.slots.y & 0xFFu).r") != std::string::npos &&
            apply.find("g_textures[g_push.slots.x & 0xFFFFu].Load(int3(p, 0)).r") != std::string::npos &&
            apply.find("} else if (view == 13u) {") != std::string::npos,
        "the apply's car sun image lookup is the twin's (CarSunUv, CarSunFromImage, CarAwareSunVisibility), in the contact share and in the bounce light's T, with the slots' packing");
  Check(kCarSunWindow == 256.0f && kCarSunFloor == 0.25f && Num("%.7f", double(kCarSunGain)) == "1.3333325",
        "the header's constants are the literals the shader repeats (a window of 256 m, a floor of 0.25, a gain of 1.3333325 = 0x3FAAAA8F)");
  {
    std::string host;
    if (!ReadFile(dir + "/../ao.cpp", &host)) {
      Check(false, "could not read ao.cpp beside the shaders");
    } else {
      Check(host.find("constexpr uint32_t kCarSunOnFlag = 1u << 8;") != std::string::npos && host.find("constexpr uint32_t kCarSunSlotShift = 16;") != std::string::npos &&
                host.find("constexpr uint32_t kCarSunPage = 0x0329E000u >> 12;") != std::string::npos &&
                host.find("push.slots[0] = base.slots[0] & 0xFFFFu;") != std::string::npos && host.find("push.slots[1] = base.slots[1] & 0xFFu;") != std::string::npos &&
                host.find("push.slots[0] |= car_sun_slot << kCarSunSlotShift;") != std::string::npos &&
                host.find("push.slots[1] |= kCarSunOnFlag | (car_sun_lookup_slot << kCarSunSlotShift);") != std::string::npos,
            "the host's packing of the car sun image's slots is the shader's (bit 8 of slots.y, the high halves of slots.x and slots.y, the depth slot and the host sample masked)");
    }
  }
  Check(contact.find("const float along = 1.0f / max(sun_up, kUnderSunSinMin);") != std::string::npos &&
            contact.find("pc + to_sun * (high * along)") != std::string::npos && contact.find("pc + to_sun * (low * along)") != std::string::npos &&
            contact.find("hit = min(hit, sun);") != std::string::npos && contact.find("if (hit > 0.0f) {") != std::string::npos &&
            contact.find("if (!(sun_up > 0.0f)) {") != std::string::npos,
        "the shader's under term is the twin's (a column up and a column along the sun's ray, the weaker one counts, the second read only after a hit)");
}

}  // namespace

// ---------------------------------------------------------------------------
// THE CONTACT FACTOR PER COLOUR CHANNEL (contact_trace.h: a shadow takes the sun's light away, in the sun's colour)
// ---------------------------------------------------------------------------

// What the apply pass made of the contact factor before the fix: the share of the LUMAS, the same for all three channels.
float OldLumaShare(float ambient_luma, float sun_luma, float lit) {
  const float sunlit = sun_luma * lit;
  return (ambient_luma + sunlit) > 1e-6f ? sunlit / (ambient_luma + sunlit) : 0.0f;
}

void ContactFactorPerChannel() {
  struct Light {
    const char* name;
    float ambient[3];
    float sun[3];
  };
  // The noon of the shdith1 runs: ambient luma 0.114 and sun luma 0.903 (PS c26 / c39); the colours are a cool sky's and a warm sun's, then the cases that must not move.
  const Light lights[] = {
      {"noon, cool ambient and warm sun", {0.080f, 0.112f, 0.160f}, {0.980f, 0.900f, 0.700f}},
      {"dusk, orange sun, deep blue ambient", {0.050f, 0.090f, 0.200f}, {1.000f, 0.600f, 0.250f}},
      {"overcast, near grey", {0.200f, 0.210f, 0.230f}, {0.520f, 0.520f, 0.540f}},
      {"grey light", {0.114f, 0.114f, 0.114f}, {0.903f, 0.903f, 0.903f}},
      {"night, dim and blue", {0.020f, 0.028f, 0.050f}, {0.040f, 0.044f, 0.070f}},
  };
  double worst_ambient_error = 0.0;     // the shadow's colour against albedo * ambient (relative, per channel)
  double worst_grey_difference = 0.0;   // the grey light against the old luma share
  double worst_old_error = 0.0;         // the OLD formula's: the bug, kept as a measured number
  int code_ok = 0;
  for (const Light& l : lights) {
    const float la = LumaOf(l.ambient), ls = LumaOf(l.sun);
    const uint32_t code = PackChroma(l.ambient, l.sun);
    code_ok += ((code & 0xF0000000u) == kChromaMarker) ? 1 : 0;
    // the float the GPU reads is never a denormal, an infinity or a NaN
    const float as_float = std::bit_cast<float>(code);
    Check(std::isnormal(as_float), Num("the code of the light is a normal float (%.3g)", double(as_float)) + std::string(" ") + l.name);
    for (const float lit : {0.2f, 0.55f, 0.87f, 1.0f}) {
      const Rgb share = SunShareRgb(la, ls, code, lit);
      const float old_share = OldLumaShare(la, ls, lit);
      // the pixel's lit colour is albedo * (A + S * lit) per channel; the shadow (cs 0, full weight) must leave albedo * A: the factor is A / (A + S * lit)
      const float factor[3] = {1.0f - share.r, 1.0f - share.g, 1.0f - share.b};
      for (int c = 0; c < 3; ++c) {
        const float a = l.ambient[c], s_c = l.sun[c];
        const float want = a / (a + s_c * lit);
        worst_ambient_error = std::max(worst_ambient_error, double(std::fabs(factor[c] - want) / want));
        const float old_factor = 1.0f - old_share;
        worst_old_error = std::max(worst_old_error, double(std::fabs(old_factor - want) / want));
      }
      if (std::string(l.name) == "grey light") {
        worst_grey_difference = std::max({worst_grey_difference, double(std::fabs(share.r - old_share)), double(std::fabs(share.g - old_share)), double(std::fabs(share.b - old_share))});
      }
    }
  }
  Check(code_ok == int(sizeof(lights) / sizeof(lights[0])), "every code carries the marker bits");
  Check(worst_ambient_error < 0.03, Num("a full shadow leaves albedo * ambient in the ambient's own colour: worst relative error per channel %.4f (the codes are 7 bits)", worst_ambient_error));
  Check(worst_grey_difference < 1e-6, Num("a grey light gives the old luma share, bit for bit: worst difference %.3g", worst_grey_difference));
  Check(worst_old_error > 0.15, Num("the OLD luma share was wrong for the coloured lights (this is the bug: the shadow kept the sun's hue): worst relative error %.3f", worst_old_error));
  std::printf("contact factor per channel: the full shadow's colour error %.4f (old luma share %.3f), grey light difference %.2g\n", worst_ambient_error, worst_old_error, worst_grey_difference);

  // the noon numbers of the runs: the share of the blue channel is smaller than the red's (the sun is warm), so the shadow is bluer than the road
  const float ambient[3] = {0.080f, 0.112f, 0.160f}, sun[3] = {0.980f, 0.900f, 0.700f};
  const Rgb share = SunShareRgb(LumaOf(ambient), LumaOf(sun), PackChroma(ambient, sun), 0.87f);
  Check(share.r > share.g && share.g > share.b, Num("noon: the sun's share falls from red to blue (%.3f, %.3f)", double(share.r), double(share.b)));
  const Rgb full = ContactFactorRgb(share, 1.0f, 0.0f);
  Check(full.b > full.r, Num("noon: a full contact shadow leaves more blue than red (%.3f against %.3f)", double(full.b), double(full.r)));
  // the rest of the rules: open (cs 1) or no weight leaves the picture alone, the weight scales the removal, nothing goes negative or past 1
  const Rgb open = ContactFactorRgb(share, 1.0f, 1.0f);
  const Rgb weightless = ContactFactorRgb(share, 0.0f, 0.0f);
  Check(open.r == 1.0f && open.g == 1.0f && open.b == 1.0f, "cs 1: factor 1 in every channel");
  Check(weightless.r == 1.0f && weightless.g == 1.0f && weightless.b == 1.0f, "weight 0: factor 1 in every channel");
  const Rgb half = ContactFactorRgb(share, 0.5f, 0.0f);
  Check(std::fabs((1.0f - half.r) - 0.5f * (1.0f - full.r)) < 1e-6f, "the weight scales what is removed");
  for (const float w : {-1.0f, 0.3f, 7.0f}) {
    for (const float cs : {-0.5f, 0.0f, 0.4f, 1.0f, 3.0f}) {
      const Rgb f = ContactFactorRgb(share, w, cs);
      Check(f.r >= 0.0f && f.r <= 1.0f && f.g >= 0.0f && f.g <= 1.0f && f.b >= 0.0f && f.b <= 1.0f, "the factor stays in 0 to 1");
    }
  }
  // a pixel the sun does not light (lit 0: the game's own shadow, a surface turned from the sun) has no share and no contact shadow
  const Rgb none = SunShareRgb(0.114f, 0.903f, PackChroma(ambient, sun), 0.0f);
  Check(none.r == 0.0f && none.g == 0.0f && none.b == 0.0f, "lit 0: no share");
  // the neutral code is grey, and the chroma of a light with no luma is grey
  const Rgb grey = SunShareRgb(0.114f, 0.903f, kNeutralChroma, 0.87f);
  Check(std::fabs(grey.r - OldLumaShare(0.114f, 0.903f, 0.87f)) < 1e-6f && std::fabs(grey.b - grey.r) < 1e-6f, "the neutral code is the luma share in all channels");
  const float black[3] = {0.0f, 0.0f, 0.0f};
  Check(PackChroma(black, black) == kNeutralChroma, "a light with no luma packs as grey");
  // the round trip: the chroma comes back within the quantisation (1/32, half of it at most) and the luma weights of the three channels still add up to 1
  double worst_round = 0.0, worst_luma = 0.0;
  for (const Light& l : lights) {
    const float la = LumaOf(l.ambient);
    const uint32_t code = PackChroma(l.ambient, l.sun);
    const Rgb a = ChromaRgb(code & 0x7Fu, (code >> 7) & 0x7Fu);
    worst_round = std::max({worst_round, double(std::fabs(a.r - l.ambient[0] / la)), double(std::fabs(a.b - l.ambient[2] / la))});
    worst_luma = std::max(worst_luma, double(std::fabs(kLumaRed * a.r + kLumaGreen * a.g + kLumaBlue * a.b - 1.0f)));
  }
  Check(worst_round <= 0.5 / kChromaScale + 1e-4, Num("the chroma round trip is within half a step: worst %.4f", worst_round));
  Check(worst_luma < 1e-5, Num("red, green (derived) and blue chroma keep the luma at 1: worst %.2g", worst_luma));
}

void ChromaShaderText(const std::string& dir) {
  std::string apply;
  if (!ReadFile(dir + "/ao_apply_ps.hlsl", &apply)) {
    Check(false, "could not read ao_apply_ps.hlsl in " + dir);
    return;
  }
  // the shader unpacks what the header packs: 7 bits at shifts 0, 7, 14, 21, scale 64, the fade's start made from extra.y, the picture taking the per channel factor
  for (const char* text : {"0x7Fu", "(code >> 7) & 0x7Fu", "(code >> 14) & 0x7Fu", "(code >> 21) & 0x7Fu", "asuint(g_push.shape.w)", "smoothstep(0.5f * fade_end, fade_end, tc)",
                           "contact_factor3 = 1.0f - saturate(", "return float4(factor3, 1.0f);", "factor3 + bounce"}) {
    Check(apply.find(text) != std::string::npos, std::string("ao_apply_ps.hlsl has \"") + text + "\"");
  }
  Check(apply.find("g_push.shape.w, fade_end") == std::string::npos, "ao_apply_ps.hlsl no longer reads shape.w as the fade's start");
  std::string host;
  if (ReadFile(dir + "/../ao.cpp", &host)) {
    Check(host.find("push.shape[3] = std::bit_cast<float>(") != std::string::npos && host.find("contact_trace::PackChroma(ambient, sun)") != std::string::npos,
          "ao.cpp packs the lights' chroma into the apply's shape.w");
  }
}

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[2]) == "chroma") {  // only the per channel contact factor and the shaders' text
    ContactFactorPerChannel();
    ChromaShaderText(argv[1]);
    ShaderConstantsMatch(argv[1]);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[2]) == "carmap") {  // only the car's shadow map checks (a second argument: quicker to run while working on them)
    CarMapRules();
    CarMapWorldFilter();
    CarMapNoAcne();
    CarMapShadowsLandWhereGeometrySays();
    CarMapPenumbra();
    CarMapLeavesTheRestAlone();
    CarMapFalseShadowOnTheWing();
    GameMaskIsNotACars();
    SkipLeavesACarsPixels();
    CarSunLookupIsTheGames();
    CarSunVisibilityOnTheLid();
    SkipWithACarSunImage();
    ShaderConstantsMatch(argv[1]);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[2]) == "stipple") {  // only the vehicle march's stipple checks and the shaders' text
    VehicleStippleIsFiltered();
    ShaderConstantsMatch(argv[1]);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[2]) == "carsun") {  // only the car sun image's checks and the shaders' text (the quickest to run while working on them)
    CarSunLookupIsTheGames();
    CarSunVisibilityOnTheLid();
    SkipWithACarSunImage();
    ShaderConstantsMatch(argv[1]);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
  }
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
  VehiclePixelsMarchShort();
  WingBandOnTheTrunk();
  WingFromTheSide();
  SmallShadowsStay();
  CarMapRules();
  CarMapWorldFilter();
  CarMapNoAcne();
  CarMapShadowsLandWhereGeometrySays();
  CarMapPenumbra();
  CarMapLeavesTheRestAlone();
  CarMapFalseShadowOnTheWing();
  GameMaskIsNotACars();
  GroundBeyondACar();
  AdaptiveStepsMatchTheFullMarch();
  FilterInTheApply();
  SkipRules();
  SkipBlocksHoldTheRange();
  StreetMix();
  SkipCost();
  SkipUnderTheGamesShadow();
  SkipLeavesACarsPixels();
  CarSunLookupIsTheGames();
  CarSunVisibilityOnTheLid();
  SkipWithACarSunImage();
  VehicleStippleIsFiltered();
  ContactFactorPerChannel();
  if (argc > 1) {
    ChromaShaderText(argv[1]);
    ShaderConstantsMatch(argv[1]);
  } else {
    std::printf("(no hlsl folder given: the shaders' constants were not compared)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
