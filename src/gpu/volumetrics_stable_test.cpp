// Check of the volumetric light's stability rules (volumetrics_stable.h): the lamp's term averaged over a slice, the composite's samples, the history's weight, and
// a simulation of the passes (the inject's jitter and history, the integrate, the composite's lookup) for a camera driving at a street lamp, which puts a number on how
// much each cause of the flicker is (the ranking in volumetrics_stable.h). A program of its own: no renderer, no ReXGlue, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_volumetrics_stable_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_volumetrics_stable_test [<src/gpu dir>]
// By hand: clang++ -std=c++20 -I src/gpu src/gpu/volumetrics_stable_test.cpp -o volumetrics_stable_test.exe (about 3 s to run without -O).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "volumetrics_lamps.h"
#include "volumetrics_stable.h"

using namespace mcla::gpu::volumetrics;

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
void CheckNear(double got, double want, double eps, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= eps)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.6f, want %.6f\n", what.c_str(), got, want);
  }
}

// A deterministic stream of numbers in 0..1 (for the samples of the checks).
struct Rng {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  float Next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return float(double(state >> 11) / double(1ull << 53));
  }
};

// A street lamp as the game has it: 55, cone 125, spread -50.5, pointing down, warm white.
VolLamp StreetLamp(float x, float y, float z, float intensity = 55.0f) {
  mcla::gpu::vol_lamps::GameLamp g;
  g.pos[0] = x;
  g.pos[1] = y;
  g.pos[2] = z;
  g.colour[0] = 1.0f;
  g.colour[1] = 0.9f;
  g.colour[2] = 0.7f;
  g.dir[0] = 0.0f;
  g.dir[1] = -1.0f;
  g.dir[2] = 0.0f;
  g.intensity = intensity;
  g.cone_deg = 125.0f;
  g.spread_deg = -50.5f;
  g.scale = 1.0f;
  g.type = 1;
  g.on = 1;
  return MakeVolLamp(g);
}

void Normalise(float v[3]) {
  const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int i = 0; i < 3; ++i) {
    v[i] /= n;
  }
}

// --- the kernel ------------------------------------------------------------------------------------------------------------------------------

void CheckKernel() {
  Rng rng;
  for (int trial = 0; trial < 200; ++trial) {
    const float s0 = 1.0f + 150.0f * rng.Next();
    const float s1 = s0 + 0.1f + 15.0f * rng.Next();
    const float s_c = -20.0f + 190.0f * rng.Next();
    const float dmin = 12.0f * rng.Next() * rng.Next();
    const float near_sq = 4.0f + 80.0f * rng.Next();
    double sum = 0.0;
    const int steps = 4000;
    for (int i = 0; i < steps; ++i) {
      const double s = s0 + (double(i) + 0.5) / steps * (s1 - s0);
      sum += 1.0 / ((s - s_c) * (s - s_c) + double(dmin) * dmin + near_sq);
    }
    const double want = sum / steps;
    const double got = SliceAverageKernel(s0, s1, s_c, dmin * dmin, near_sq);
    Check(std::fabs(got - want) <= 1e-4 * want + 1e-9, "the kernel's average is the numeric integral's (trial " + std::to_string(trial) + ")");
  }
  // a stretch with no length is the point value
  CheckNear(SliceAverageKernel(10.0f, 10.0f, 12.0f, 9.0f, 16.0f), 1.0 / (4.0 + 9.0 + 16.0), 1e-6, "a stretch with no length is the point value");
  // the average never exceeds the peak of the kernel
  Check(SliceAverageKernel(0.0f, 30.0f, 15.0f, 0.0f, 36.0f) <= 1.0f / 36.0f, "the average is under the kernel's peak 1 / near^2");
}

// --- the lamp's term through a slice, against a point sample ----------------------------------------------------------------------------------

void CheckLampSlice() {
  const VolLamp lamp = StreetLamp(4.0f, 8.0f, 60.0f);
  const float cam[3] = {0.0f, 0.0f, 0.0f};
  Rng rng;
  // a thin slice is the point term at its middle
  for (int trial = 0; trial < 100; ++trial) {
    float ray[3] = {-0.4f + 0.8f * rng.Next(), -0.2f + 0.4f * rng.Next(), 1.0f};
    Normalise(ray);
    const float s = 10.0f + 80.0f * rng.Next();
    const float p[3] = {cam[0] + ray[0] * s, cam[1] + ray[1] * s, cam[2] + ray[2] * s};
    const float thin = LampSliceFactor(lamp, cam, ray, s - 1e-3f, s + 1e-3f, 0.3f);
    const float point = LampPointFactor(lamp, p, ray, 0.3f);
    Check(std::fabs(thin - point) <= 2e-3f * std::max(point, 1e-6f) + 1e-9f, "a thin slice gives the point term (trial " + std::to_string(trial) + ")");
  }
  // no light beyond the reach, whatever the slice
  float away[3] = {0.0f, 0.0f, -1.0f};
  Check(LampSliceFactor(lamp, cam, away, 5.0f, 9.0f, 0.3f) == 0.0f, "a ray that points away from a lamp that is out of reach gets nothing");
  // the slice average equals the mean of many point samples inside the slice where the pool is wide against the slice (a near slice): the same energy
  {
    float ray[3] = {0.05f, 0.1f, 1.0f};
    Normalise(ray);
    const float s0 = 30.0f;
    const float s1 = 33.8f;
    double mean = 0.0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      const float s = s0 + (float(i) + 0.5f) / float(n) * (s1 - s0);
      const float p[3] = {ray[0] * s, ray[1] * s, ray[2] * s};
      mean += LampPointFactor(lamp, p, ray, 0.3f);
    }
    mean /= n;
    const double slice = LampSliceFactor(lamp, cam, ray, s0, s1, 0.3f);
    Check(std::fabs(slice - mean) <= 0.03 * mean, "the slice average carries the energy the jittered samples carry on average (a near slice: " + std::to_string(slice) + " against " + std::to_string(mean) + ")");
  }
  // continuity: the lamp moving across a slice boundary changes the factor smoothly (the first version's point sample jumped with the sample's place)
  {
    float ray[3] = {0.0f, 0.1f, 1.0f};
    Normalise(ray);
    std::vector<double> f;
    for (int i = 0; i <= 400; ++i) {
      const VolLamp l = StreetLamp(0.0f, 7.0f, 40.0f + 0.05f * float(i));  // the lamp slides 20 m along the axis through a fixed slice [45, 50.7]
      f.push_back(LampSliceFactor(l, cam, ray, 45.0f, 50.7f, 0.3f));
    }
    double peak = 0.0;
    for (double x : f) {
      peak = std::max(peak, x);
    }
    double worst = 0.0;
    for (size_t i = 1; i < f.size(); ++i) {
      worst = std::max(worst, std::fabs(f[i] - f[i - 1]) / peak);
    }
    Check(peak > 0.0 && worst < 0.03, "the factor of a slice changes by under 3 per cent of its peak for a lamp that moves 5 cm (worst " + std::to_string(worst) + ")");
  }
}

// --- the composite's depths --------------------------------------------------------------------------------------------------------------------

void CheckDepths() {
  const GridDesc g = GridFor(Level::kHigh);
  const float ln_ratio = LogRatio(g.near_m, g.far_m);
  const float tol = DepthAgreementTolerance(g.nz, ln_ratio);
  CheckNear(tol, 0.5 * (std::exp(ln_ratio / g.nz) - 1.0), 1e-7, "the tolerance is half a slice's relative thickness");
  Check(tol > 0.05f && tol < 0.07f, "half a slice is about 6 per cent on high");
  const float same[4] = {40.0f, 40.5f, 41.0f, 40.2f};
  Check(DepthsAgree(same, 4, tol), "samples within half a slice agree");
  const float edge[4] = {40.0f, 40.0f, 120.0f, 120.0f};
  Check(!DepthsAgree(edge, 4, tol), "samples on both sides of an edge do not");
  // the lookup count: one where they agree, n where they do not
  int calls = 0;
  AverageOverSamples(same, 4, tol, [&](float) { ++calls; return 1.0f; });
  Check(calls == 1, "one lookup where the samples agree");
  calls = 0;
  AverageOverSamples(edge, 4, tol, [&](float) { ++calls; return 1.0f; });
  Check(calls == 4, "four lookups at an edge");
  // an edge moving through a pixel: the old pixel took sample 0's depth for all, the new one the mean of the four. The geometry covers the pixel by `c`; a
  // stratified 4x pattern (sample 0 first) sees c as 0, 1/4 .. 1. The true light is c * near + (1 - c) * far.
  const float near_light = 0.2f;
  const float far_light = 1.0f;
  auto light = [&](float t) { return t < 60.0f ? near_light : far_light; };
  // sample positions along the edge's axis in a pixel: 0.125, 0.375, 0.625, 0.875 with sample 0 first in the ORDER (a real pattern's sample 0 is anywhere): rotate
  double err_old = 0.0;
  double err_new = 0.0;
  int n = 0;
  for (int rot = 0; rot < 4; ++rot) {
    const float pos[4] = {0.125f + 0.25f * float((0 + rot) % 4), 0.125f + 0.25f * float((1 + rot) % 4), 0.125f + 0.25f * float((2 + rot) % 4),
                          0.125f + 0.25f * float((3 + rot) % 4)};
    for (int i = 0; i < 200; ++i) {
      const float edge_at = float(i) / 200.0f;  // the geometry covers the samples whose position is under the edge
      const float c = edge_at;
      float t[4];
      for (int k = 0; k < 4; ++k) {
        t[k] = pos[k] < edge_at ? 40.0f : 120.0f;
      }
      const double truth = c * near_light + (1.0 - c) * far_light;
      const double old_value = light(t[0]);
      const double new_value = AverageOverSamples(t, 4, tol, light);
      err_old += (old_value - truth) * (old_value - truth);
      err_new += (new_value - truth) * (new_value - truth);
      ++n;
    }
  }
  const double rms_old = std::sqrt(err_old / n);
  const double rms_new = std::sqrt(err_new / n);
  std::printf("  composite at an edge moving through a pixel (4 samples, light 0.2 against 1.0): rms error of the pixel's light: sample 0 only %.3f, every sample %.3f (x%.1f)\n", rms_old, rms_new,
              rms_old / rms_new);
  Check(rms_new < 0.5 * rms_old, "averaging the four samples at least halves the error of an edge pixel");
}

// --- the history's weight ----------------------------------------------------------------------------------------------------------------------

void CheckHistory() {
  const GridDesc g = GridFor(Level::kHigh);
  CheckNear(HistoryWeightFor(g, true, 1.0f / 60.0f, 0.0f), 0.9, 2e-3, "at rest and 60 FPS the weight is the first version's 0.9");
  Check(HistoryWeightFor(GridFor(Level::kMedium), true, 1.0f / 60.0f, 0.0f) == 0.0f, "medium has no history");
  Check(HistoryWeightFor(g, false, 1.0f / 60.0f, 0.0f) == 0.0f, "no history to read: no weight");
  // a time constant: four frames at 240 FPS keep what one at 60 does
  const double w60 = HistoryWeightFor(g, true, 1.0f / 60.0f, 0.0f);
  const double w240 = HistoryWeightFor(g, true, 1.0f / 240.0f, 0.0f);
  CheckNear(std::pow(w240, 4.0), w60, 5e-3, "four frames at 240 FPS keep the history that one frame at 60 does");
  const double w30 = HistoryWeightFor(g, true, 1.0f / 30.0f, 0.0f);
  CheckNear(w30, w60 * w60, 5e-3, "one frame at 30 FPS keeps what two at 60 do");
  // the move lowers it, never under the floor
  const double moving = HistoryWeightFor(g, true, 1.0f / 60.0f, 1.0f);
  Check(moving < w60 && moving >= kHistoryMinWeight, "a fast move lowers the weight, to no less than the floor");
  CheckNear(moving, 0.75, 0.02, "a fast move at 60 FPS keeps 0.75");
  // the move does not pump: one frame of a bump in the camera (raw 1) moves the smoothed move by 13 per cent at 60 FPS, the weight by about a per cent
  const float dt = 1.0f / 60.0f;
  const float bumped = HistoryMotion(0.0f, 1.0f, dt);
  const double w_bump = HistoryWeightFor(g, true, dt, bumped);
  Check(w60 - w_bump < 0.015, "one bumped frame changes the history's weight by under 1.5 per cent (new)");
  const double old_bump = HistoryWeight(g, true, 3.0f, 0.0f);  // the first version: a step of 3 m in one frame goes to full move
  Check(w60 - old_bump > 0.35, "the first version's weight dropped by 0.4 for such a frame");
  const double noise_old = (1.0 - old_bump) / (1.0 + old_bump);
  const double noise_still = (1.0 - w60) / (1.0 + w60);
  std::printf("  history: the noise variance of a frame (1 - w) / (1 + w): still %.3f; the first version's frame of a fast bump %.3f (x%.1f); the new one's %.3f (x%.2f)\n", noise_still, noise_old,
              noise_old / noise_still, (1.0 - w_bump) / (1.0 + w_bump), ((1.0 - w_bump) / (1.0 + w_bump)) / noise_still);
  // the smoothed move converges to the raw one and never overshoots
  float m = 0.0f;
  for (int i = 0; i < 600; ++i) {
    m = HistoryMotion(m, 0.6f, dt);
    Check(m <= 0.6f + 1e-6f, "the smoothed move never overshoots");
  }
  CheckNear(m, 0.6, 1e-3, "and converges to the raw one");
  // RawMotion: the first version's scale at 60 FPS (3 m a frame = full)
  CheckNear(RawMotion(3.0f / 60.0f * 60.0f / 60.0f, 0.0f, 1.0f / 60.0f), 3.0 / 180.0 * 60.0 / 60.0 * 1.0, 1e-3, "a step of 0.05 m a frame at 60 FPS (3 m/s) is 3/180 of full");
  Check(RawMotion(10.0f, 0.0f, 1.0f / 60.0f) == 1.0f, "a 10 m step in a frame is full move");
  Check(ClampFrameSeconds(0.0f) == kMinFrameSeconds && ClampFrameSeconds(5.0f) == kMaxFrameSeconds, "the frame time is clamped");
}

// --- the flags ---------------------------------------------------------------------------------------------------------------------------------

void CheckFlags() {
  Check(ApplyWord(3, false, 0) == 3u, "the composite's word keeps the guest sample in its low byte");
  Check((ApplyWord(3, true, 0) & kApplyFlagAllSamples) != 0, "the all samples bit");
  Check(((ApplyWord(1, true, 2) >> kApplyViewShift) & kApplyViewMask) == 2u, "the debug view");
  Check((ApplyWord(0xFFu, false, 3) & 0xFFu) == 0xFFu, "the sample is eight bits");
}

// --- the passes, simulated for a camera driving at a street lamp ------------------------------------------------------------------------------
// The vertical plane through the camera's axis and a lamp above the road: the grid's column at the centre (x), the picture's rows (y) and the slices (z), as the inject
// makes them and the integrate and the composite read them, with the first version's jitter and history or the stable ones. No shadows, no haze noise, no sun: only
// the lamp's own term in a uniform haze, which is the thing the owner sees pulse.

struct Plane {
  uint32_t ny = 0;
  uint32_t nz = 0;
  std::vector<float> v;
  float& at(uint32_t y, uint32_t k) { return v[size_t(k) * ny + y]; }
  float at(uint32_t y, uint32_t k) const { return v[size_t(k) * ny + y]; }
};

struct Setup {
  bool slice_lamps = false;       // the lamp's term averaged over the slice (the lamps take no z jitter)
  bool stable_history = false;    // the time constant and the smoothed move
  float fps = 60.0f;
  float speed = 25.0f;            // m/s along the axis
  float haze = 0.0012f;           // sigma, 1/m
  float strength = 4.0f;          // the look's lamp strength (clear night)
};

float JitterUnit(float x) { return x - std::floor(x); }

void InjectPlane(const Setup& setup, const GridDesc& g, const VolCamera& cam, const VolCamera& hcam, const Plane* history, float weight, uint32_t frame, const VolLamp& lamp,
                 Plane* out) {
  const uint32_t xc = g.nx / 2;
  const float ratio = LogRatio(g.near_m, g.far_m);
  for (uint32_t k = 0; k < g.nz; ++k) {
    for (uint32_t y = 0; y < g.ny; ++y) {
      const float n0 = InterleavedGradient(float(xc) + 37.0f * float(k), float(y) + 17.0f * float(k), frame);
      const float jx = JitterUnit(Halton(frame % 16u + 1u, 2) + n0) - 0.5f;
      (void)jx;
      const float jy = JitterUnit(Halton(frame % 16u + 1u, 3) + JitterUnit(n0 * 7.31f)) - 0.5f;
      const float jz = JitterUnit(Halton(frame % 16u + 1u, 5) + JitterUnit(n0 * 3.77f)) - 0.5f;
      const float v = (float(y) + 0.5f + jy * 0.9f) / float(g.ny);
      float value = 0.0f;
      if (setup.slice_lamps) {
        float dir[3];
        ViewToWorld(cam, 0.5f, v, 1.0f, dir);
        for (int i = 0; i < 3; ++i) {
          dir[i] -= cam.pos[i];
        }
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        float ray[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
        float s0, s1;
        SliceBoundsAlongRay(g, g.far_m, k, len, &s0, &s1);
        value = LampSliceFactor(lamp, cam.pos, ray, s0, s1, 0.3f);
      } else {
        const float slice = float(k) + 0.5f + jz * 0.9f;
        const float z = g.near_m * std::exp(ratio * slice / float(g.nz));
        float p[3];
        ViewToWorld(cam, 0.5f, v, z, p);
        float ray[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
        Normalise(ray);
        value = LampPointFactor(lamp, p, ray, 0.3f);
      }
      float now = value * lamp.colour[1] * setup.strength * setup.haze * kLightGain;
      if (history && weight > 0.0f) {
        const float zc = SliceCentre(g.near_m, g.far_m, g.nz, k);
        float pc[3];
        ViewToWorld(cam, 0.5f, (float(y) + 0.5f) / float(g.ny), zc, pc);
        float pu, pv, pz;
        if (WorldToView(hcam, pc, &pu, &pv, &pz) && pz > g.near_m) {
          const float s = SliceCoord(g.near_m, g.far_m, g.nz, pz) - 0.5f;
          if (pv > 0.0f && pv < 1.0f && s > -0.5f && s < float(g.nz) - 0.5f) {
            const float ty = std::clamp(pv * float(g.ny), 0.5f, float(g.ny) - 0.5f) - 0.5f;
            const uint32_t y0 = uint32_t(std::floor(ty));
            const uint32_t y1 = std::min(y0 + 1, g.ny - 1);
            const float fy = ty - float(y0);
            const uint32_t s0 = uint32_t(std::clamp(std::floor(s), 0.0f, float(g.nz) - 1.0f));
            const uint32_t s1 = std::min(s0 + 1, g.nz - 1);
            const float fs = std::clamp(s - float(s0), 0.0f, 1.0f);
            auto row = [&](uint32_t kk) { return history->at(y0, kk) * (1.0f - fy) + history->at(y1, kk) * fy; };
            const float old = row(s0) * (1.0f - fs) + row(s1) * fs;
            now = now + (old - now) * weight;
          }
        }
      }
      out->at(y, k) = now;
    }
  }
}

// The integrate pass on one column: S at each slice's centre, the energy conserving march with sigma constant.
void IntegratePlane(const GridDesc& g, float sigma, const Plane& scatter, Plane* accum) {
  const float ratio = LogRatio(g.near_m, g.far_m);
  for (uint32_t y = 0; y < g.ny; ++y) {
    float s = 0.0f;
    float t = 1.0f;
    float d0 = g.near_m;
    for (uint32_t k = 0; k < g.nz; ++k) {
      const float dc = g.near_m * std::exp(ratio * (float(k) + 0.5f) / float(g.nz));
      const float d1 = g.near_m * std::exp(ratio * float(k + 1) / float(g.nz));
      const float sv = scatter.at(y, k);
      float tz = std::exp(-sigma * (dc - d0));
      s += t * (sv - sv * tz) / sigma;
      t *= tz;
      accum->at(y, k) = s;
      tz = std::exp(-sigma * (d1 - dc));
      s += t * (sv - sv * tz) / sigma;
      t *= tz;
      d0 = d1;
    }
  }
}

// The composite's lookup at the picture row v and the view depth t: two bilinear fetches and the slice blend.
float Lookup(const GridDesc& g, const Plane& accum, float v, float t) {
  const float s = float(g.nz) * std::log(std::max(t, g.near_m) / g.near_m) / LogRatio(g.near_m, g.far_m) - 0.5f;
  const float sf = std::floor(s);
  const float blend = std::clamp(s - sf, 0.0f, 1.0f);
  const uint32_t k0 = uint32_t(std::clamp(sf, 0.0f, float(g.nz) - 1.0f));
  const uint32_t k1 = std::min(uint32_t(std::clamp(sf + 1.0f, 0.0f, float(g.nz) - 1.0f)), g.nz - 1);
  const float ty = std::clamp(v * float(g.ny), 0.5f, float(g.ny) - 0.5f) - 0.5f;
  const uint32_t y0 = uint32_t(std::floor(ty));
  const uint32_t y1 = std::min(y0 + 1, g.ny - 1);
  const float fy = ty - float(y0);
  auto at = [&](uint32_t k) { return accum.at(y0, k) * (1.0f - fy) + accum.at(y1, k) * fy; };
  return at(k0) * (1.0f - blend) + at(k1) * blend;
}

struct Series {
  std::vector<double> value;   // the composite's S at the pixel that images the lamp, at the pole's depth and 15 m behind it
  std::vector<double> value_far;
  std::vector<double> truth;
  std::vector<double> truth_far;
  std::vector<double> distance;
};

// The exact S the pixel should get: the dense integral of the first version's term along the ray through the lamp (in the integrate's own convention, the view's z as the length).
double TrueScatter(const Setup& setup, const VolCamera& cam, const VolLamp& lamp, float v, float t_end) {
  const int steps = 6000;
  double sum = 0.0;
  for (int i = 0; i < steps; ++i) {
    const float z = (float(i) + 0.5f) / float(steps) * t_end;
    float p[3];
    ViewToWorld(cam, 0.5f, v, z, p);
    float ray[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
    Normalise(ray);
    sum += double(LampPointFactor(lamp, p, ray, 0.3f)) * double(lamp.colour[1]) * setup.strength * setup.haze * kLightGain * std::exp(-double(setup.haze) * z) * (t_end / steps);
  }
  return sum;  // with the haze's own transmittance, as the integrate pass has it
}

Series Drive(const Setup& setup) {
  GridDesc g = GridFor(Level::kHigh);
  const float dt = 1.0f / setup.fps;
  const VolLamp lamp = StreetLamp(0.0f, 7.5f, 110.0f);
  Plane a, b, accum;
  a.ny = b.ny = accum.ny = g.ny;
  a.nz = b.nz = accum.nz = g.nz;
  a.v.assign(size_t(g.ny) * g.nz, 0.0f);
  b.v = a.v;
  accum.v = a.v;
  Plane* write = &a;
  Plane* read = &b;
  VolCamera cam;
  cam.right[0] = 1.0f;
  cam.up[1] = 1.0f;
  cam.forward[2] = 1.0f;
  cam.tan_x = 0.92f;
  cam.tan_y = 0.52f;
  VolCamera previous = cam;
  Series out;
  float motion = 0.0f;
  float z_cam = 0.0f;
  bool have_history = false;
  for (uint32_t frame = 0; frame < 4000; ++frame) {
    z_cam = setup.speed * float(frame) * dt;
    cam.pos[2] = z_cam;
    const float step = setup.speed * dt;
    float weight = 0.0f;
    if (have_history) {
      if (setup.stable_history) {
        motion = HistoryMotion(motion, RawMotion(step, 0.0f, dt), dt);
        weight = HistoryWeightFor(g, true, dt, motion);
      } else {
        weight = HistoryWeight(g, true, step, 0.0f);
      }
    }
    InjectPlane(setup, g, cam, previous, have_history ? read : nullptr, weight, frame, lamp, write);
    IntegratePlane(g, setup.haze, *write, &accum);
    const float dist = 110.0f - z_cam;
    if (dist < 18.0f) {
      break;
    }
    // the pixel that images the pole 4.5 m under the lamp's head (the rays that pass under a street lamp are the ones its downward cone lights: a ray at the head itself
    // never enters it): the pole's depth, and the street 15 m behind it
    const float target[3] = {0.0f, 3.0f, 110.0f};
    float u, v, z;
    WorldToView(cam, target, &u, &v, &z);
    const float t1 = z - 0.1f;
    const float t2 = z + 15.0f;
    out.value.push_back(double(Lookup(g, accum, v, t1)));
    out.value_far.push_back(double(Lookup(g, accum, v, t2)));
    out.truth.push_back(TrueScatter(setup, cam, lamp, v, t1));
    out.truth_far.push_back(TrueScatter(setup, cam, lamp, v, t2));
    out.distance.push_back(dist);
    std::swap(write, read);
    previous = cam;
    have_history = true;
  }
  return out;
}

struct Flicker {
  double rms_error = 0.0;   // the series' error against the truth, as a share of the truth's mean
  double hf = 0.0;          // the frame to frame jitter: rms of x(t) - (x(t-1) + x(t+1)) / 2 of the error, as a share of the truth's mean
  double peak = 0.0;        // the largest error as a share of the truth's mean
  double pulse = 0.0;       // the error's variation over 12 frames or less (rms of the error less its 25 frame moving average): the pulsing of a slice that passes the lamp
  int frames = 0;
};

Flicker Measure(const std::vector<double>& value, const std::vector<double>& truth, const std::vector<double>& distance, double from, double to) {
  std::vector<double> error;
  double mean_truth = 0.0;
  for (size_t i = 0; i < value.size(); ++i) {
    if (distance[i] <= to && distance[i] >= from) {
      error.push_back(value[i] - truth[i]);
      mean_truth += truth[i];
    }
  }
  Flicker f;
  f.frames = int(error.size());
  if (error.size() < 8) {
    return f;
  }
  mean_truth /= double(error.size());
  if (!(mean_truth > 0.0)) {
    f.frames = 0;
    return f;
  }
  double se = 0.0;
  double sh = 0.0;
  int nh = 0;
  for (size_t i = 0; i < error.size(); ++i) {
    se += error[i] * error[i];
    f.peak = std::max(f.peak, std::fabs(error[i]));
    if (i > 0 && i + 1 < error.size()) {
      const double d = error[i] - 0.5 * (error[i - 1] + error[i + 1]);
      sh += d * d;
      ++nh;
    }
  }
  // the pulse: the error less its moving average over 25 frames
  double sp = 0.0;
  int np = 0;
  for (size_t i = 0; i < error.size(); ++i) {
    const size_t lo = i >= 12 ? i - 12 : 0;
    const size_t hi = std::min(error.size() - 1, i + 12);
    double avg = 0.0;
    for (size_t j = lo; j <= hi; ++j) {
      avg += error[j];
    }
    avg /= double(hi - lo + 1);
    sp += (error[i] - avg) * (error[i] - avg);
    ++np;
  }
  f.pulse = std::sqrt(sp / double(std::max(np, 1))) / mean_truth;
  f.rms_error = std::sqrt(se / double(error.size())) / mean_truth;
  f.hf = std::sqrt(sh / double(std::max(nh, 1))) / mean_truth;
  f.peak /= mean_truth;
  return f;
}

void CheckDrive() {
  struct Row {
    const char* name;
    Setup setup;
  };
  Setup first;
  Setup slice = first;
  slice.slice_lamps = true;
  Setup full = slice;
  full.stable_history = true;
  Setup first144 = first;
  first144.fps = 144.0f;
  Setup history144 = first144;
  history144.stable_history = true;
  Setup full144 = full;
  full144.fps = 144.0f;
  const Row rows[] = {{"first version, 60 FPS", first},          {"slice average, 60 FPS", slice},           {"slice average + time history, 60 FPS", full},
                      {"first version, 144 FPS", first144},      {"time history only, 144 FPS", history144}, {"all of it, 144 FPS", full144}};
  std::printf("  a camera drives at a street lamp at 25 m/s from 85 m to 18 m (lamp 55, pool %0.1f m, high grid): the composite of the pixel that images the lamp's pole (at the lamp's\n"
              "  depth) and of the street 15 m behind it, against the exact integral; the error as a share of the mean light, over the 85 to 35 m stretch where the pool is narrower\n"
              "  than a slice (rms of the error / its pulse over 12 frames or less / frame to frame jitter / the largest error)\n",
              std::sqrt(double(LampNearSq(StreetLamp(0, 0, 0))) ));
  double hf_first = 0.0;
  double hf_slice = 0.0;
  double hf_first_far = 0.0;
  double hf_slice_far = 0.0;
  double rms_first = 0.0;
  double rms_slice = 0.0;
  double hf_first144 = 0.0;
  double hf_full144 = 0.0;
  double hf_history144 = 0.0;
  double pulse_first = 0.0;
  double pulse_slice = 0.0;
  for (const Row& row : rows) {
    const Series s = Drive(row.setup);
    const Flicker near_f = Measure(s.value, s.truth, s.distance, 35.0, 85.0);
    const Flicker far_f = Measure(s.value_far, s.truth_far, s.distance, 35.0, 85.0);
    std::printf("    %-40s pole: %5.1f%% / %4.1f%% / %4.1f%% / %5.1f%%   behind: %5.1f%% / %4.1f%% / %4.1f%% / %5.1f%%   (%d frames)\n", row.name, 100.0 * near_f.rms_error, 100.0 * near_f.pulse,
                100.0 * near_f.hf, 100.0 * near_f.peak, 100.0 * far_f.rms_error, 100.0 * far_f.pulse, 100.0 * far_f.hf, 100.0 * far_f.peak, near_f.frames);
    if (&row == &rows[0]) {
      hf_first = near_f.hf;
      hf_first_far = far_f.hf;
      rms_first = near_f.rms_error;
      pulse_first = near_f.pulse;
    } else if (&row == &rows[1]) {
      hf_slice = near_f.hf;
      hf_slice_far = far_f.hf;
      rms_slice = near_f.rms_error;
      pulse_slice = near_f.pulse;
    } else if (&row == &rows[3]) {
      hf_first144 = near_f.hf;
    } else if (&row == &rows[4]) {
      hf_history144 = near_f.hf;
    } else if (&row == &rows[5]) {
      hf_full144 = near_f.hf;
    }
  }
  Check(hf_first > 0.0, "the first version's pole pixel has a flicker to measure");
  Check(hf_slice < 0.5 * hf_first, "averaging the lamp's term over the slice at least halves the frame to frame jitter of the pole's pixel");
  Check(hf_slice_far < 0.6 * hf_first_far, "and of the street behind it");
  Check(rms_slice < 0.6 * rms_first, "and the error against the exact integral");
  Check(pulse_slice < 0.6 * pulse_first, "and its pulse over 12 frames");
  Check(hf_history144 < 0.9 * hf_first144, "a time constant alone makes the jitter smaller at 144 FPS (more frames are averaged in the same time)");
  Check(hf_full144 < 0.2 * hf_first144, "all of the fixes together are at least five times steadier than the first version at 144 FPS");
}

}  // namespace

int main(int argc, char** argv) {
  CheckKernel();
  CheckLampSlice();
  CheckDepths();
  CheckHistory();
  CheckFlags();
  CheckDrive();
  if (argc > 1) {
    // the shaders say what the header says (a constant here, a constant there)
    const std::string folder = argv[1];
    auto read = [&](const std::string& name) {
      std::ifstream in(folder + "/hlsl/" + name, std::ios::binary);
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    };
    const std::string inject = read("volumetrics_inject_cs.hlsl");
    const std::string apply = read("volumetrics_apply_ps.hlsl");
    Check(!inject.empty() && !apply.empty(), "the shaders are readable in " + folder);
    Check(inject.find("static const uint kInjectFlagSliceLamps = " + std::to_string(kInjectFlagSliceLamps) + "u;") != std::string::npos, "the inject's slice lamps flag is the header's");
    Check(apply.find("static const uint kApplyFlagAllSamples = 0x100u;") != std::string::npos, "the composite's all samples flag is the header's");
    Check(apply.find("static const uint kApplyViewShift = " + std::to_string(kApplyViewShift) + "u;") != std::string::npos, "the composite's debug view shift is the header's");
    Check(apply.find("static const float kApplyViewGain = 8.0;") != std::string::npos, "the composite's debug view gain is the header's");
    Check(inject.find("LampSlice(") != std::string::npos, "the inject has the slice average for the lamps");
  }
  if (g_failed != 0) {
    std::printf("volumetrics_stable_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("volumetrics_stable_test: %d checks passed%s\n", g_checks, argc > 1 ? " (with the shaders)" : "");
  return 0;
}
