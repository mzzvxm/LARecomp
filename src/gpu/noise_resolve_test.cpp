// Check of NOISE-RESOLVE: the stochastic effects that held one pattern without a temporal upscaler now change their noise every frame and resolve it themselves (noise_resolve.h; perf\NOISE-AUDIT.md CHECK 1 and
// CHECK 2). A program of its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_noise_resolve_test, then run src/gpu/larecomp_noise_resolve_test [<src/gpu dir>] [--print] (about ten seconds).
//
// The shaders' noise functions are compiled as C++ (hlsl_shim.h) and the resolves are C++ twins of the passes (written from the shaders' text; the sources are checked below). For each effect, against a smooth
// truth, with the measures of noise_resolve_check.h:
//   per-frame variation   the number a pixel gets changes at (almost) every pixel every frame, the step is the same for every pixel (nothing slides), no gap over 0.25 in 16 frames
//   the error             rms after the history under a STILL camera and a MOVING one, against what the first fixes left (the fixed blue pattern with the filter alone); the error's lattice (one spatial frequency, a direction);
//                         the flicker (how much the resolved picture changes from frame to frame): the unresolved per-frame noise boils, the resolved one does not
//   the ghost             a shadow edge or a reflection ramp that moves: how far the history trails it
// and the sources: the call sites no longer gate on the upscaler alone, the switch, the flags, the passes and the images.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "blue_noise_spectrum.h"
#include "car_rt_refl_rules.h"
#include "hlsl_shim.h"
#include "noise_resolve.h"
#include "noise_resolve_check.h"
#include "scene_noise.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/car_rt_ao_noise.hlsli"
#include "hlsl/shafts_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;
namespace nr = mcla::gpu::noise_resolve;
namespace rt = mcla::gpu::car_rt_refl;
namespace sn = mcla::gpu::scene_noise;

namespace {

nc::Checks g_check;
constexpr double kPi = 3.14159265358979323846;
constexpr int kN = 64;  // the pictures are kN x kN texels (a power of two for the spectra)

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

struct Image {
  std::vector<float> v;
  bool periodic = true;
  Image() : v(size_t(kN) * size_t(kN), 0.0f) {}
  int Wrap(int i) const { return periodic ? ((i % kN) + kN) % kN : std::clamp(i, 0, kN - 1); }
  float& At(int x, int y) { return v[size_t(Wrap(y)) * size_t(kN) + size_t(Wrap(x))]; }
  float At(int x, int y) const { return v[size_t(Wrap(y)) * size_t(kN) + size_t(Wrap(x))]; }
  // bilinear read at a texel-centre position (x, y in texels, the centre of texel i is i)
  float Sample(double x, double y) const {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    return float((1 - fx) * (1 - fy) * At(x0, y0) + fx * (1 - fy) * At(x0 + 1, y0) + (1 - fx) * fy * At(x0, y0 + 1) + fx * fy * At(x0 + 1, y0 + 1));
  }
};

double Rms(const Image& a, const std::function<double(int, int)>& truth, double* mean_out = nullptr) {
  double sum = 0.0, sum2 = 0.0;
  for (int y = 0; y < kN; ++y) {
    for (int x = 0; x < kN; ++x) {
      const double e = double(a.At(x, y)) - truth(x, y);
      sum += e;
      sum2 += e * e;
    }
  }
  const double n = double(kN) * double(kN);
  if (mean_out) {
    *mean_out = sum / n;
  }
  return std::sqrt(sum2 / n);
}

// the error image with its mean taken out, as the spectra want it
blue_noise_spectrum::Shape ShapeOf(const Image& a, const std::function<double(int, int)>& truth) {
  std::vector<double> e(size_t(kN) * size_t(kN));
  for (int y = 0; y < kN; ++y) {
    for (int x = 0; x < kN; ++x) {
      e[size_t(y) * kN + size_t(x)] = double(a.At(x, y)) - truth(x, y);
    }
  }
  return blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(e, kN, true), kN);
}

double Flicker(const Image& a, const Image& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.v.size(); ++i) {
    const double d = double(a.v[i]) - double(b.v[i]);
    s += d * d;
  }
  return std::sqrt(s / double(a.v.size()));
}

// ---------------------------------------------------------------------------
// the contact image's resolve: the occlusion's filter (hlsl/ao_common.hlsli AoBilateralEdge) and temporal pass (hlsl/ao_temporal_ps.hlsl, the plain variant) over a plane
// ---------------------------------------------------------------------------

// The filter's 3 x 3 on a plane (no depth weight to speak of): the spatial weights (sigma 0.9) times the visibility's difference weight (sigma 0.2). The vehicle march's marks are not modelled (the filter
// takes them out before the history: ao.cpp RecordContactHistory runs over its plain result).
Image Filter(const Image& raw) {
  Image out;
  for (int y = 0; y < kN; ++y) {
    for (int x = 0; x < kN; ++x) {
      const double c = raw.At(x, y);
      double sum = c, weights = 1.0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          const double q = raw.At(x + dx, y + dy);
          const double w = std::exp(-0.5 * double(dx * dx + dy * dy) / (0.9 * 0.9)) * std::exp(-0.5 * (q - c) * (q - c) / (0.2 * 0.2));
          sum += q * w;
          weights += w;
        }
      }
      out.At(x, y) = float(sum / weights);
    }
  }
  return out;
}

// The temporal pass: the history read at (x + sx, y + sy) (the reprojection of a plane under a camera that moved by (sx, sy) texels: exact), clamped to the 3 x 3 range of this frame's filtered value plus kClampMargin
// (0.08), blended at `weight` (1 - 1 / min(age + 2, cap)). A pixel with no history (weight 0) takes the frame's own value (the contact image's flag mode: no wide blur).
constexpr float kAoClampMargin = 0.08f;
Image Accumulate(const Image& current, const Image* history, double weight, double sx, double sy) {
  Image out;
  for (int y = 0; y < kN; ++y) {
    for (int x = 0; x < kN; ++x) {
      const float c = current.At(x, y);
      if (!history || weight <= 0.0) {
        out.At(x, y) = c;
        continue;
      }
      float lo = c, hi = c;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          lo = std::min(lo, current.At(x + dx, y + dy));
          hi = std::max(hi, current.At(x + dx, y + dy));
        }
      }
      const float h = std::clamp(history->Sample(double(x) + sx, double(y) + sy), lo - kAoClampMargin, hi + kAoClampMargin);
      out.At(x, y) = float(std::clamp(double(c) + (double(h) - double(c)) * weight, 0.0, 1.0));
    }
  }
  return out;
}

// What the contact march and the traced sun ray make of their noise at a pixel and frame: a visibility 0 or 1.
using Visibility = std::function<float(int x, int y, uint32_t frame_term, double world_x, double world_y)>;

struct Run {
  Image last;
  Image before_last;
};

// What a noisy run leaves over the same pipeline fed the noise-free expectation: the NOISE residual (the pipeline's own bias, the filter's blur, the history's bilinear read, is in both and cancels), its mean
// taken out, its flicker (how much it changes from the frame before) and, for the periodic pictures, where its energy sits in the spectrum.
struct Metrics {
  double rms = 0.0;
  double flicker = 0.0;
  blue_noise_spectrum::Shape shape;
};

Metrics Compare(const Run& noisy, const Run& clean, bool spectrum) {
  Metrics m;
  std::vector<double> now(noisy.last.v.size()), before(noisy.last.v.size());
  double mean = 0.0;
  for (size_t i = 0; i < now.size(); ++i) {
    now[i] = double(noisy.last.v[i]) - double(clean.last.v[i]);
    before[i] = double(noisy.before_last.v[i]) - double(clean.before_last.v[i]);
    mean += now[i];
  }
  mean /= double(now.size());
  double s2 = 0.0, f2 = 0.0;
  for (size_t i = 0; i < now.size(); ++i) {
    s2 += (now[i] - mean) * (now[i] - mean);
    f2 += (now[i] - before[i]) * (now[i] - before[i]);
  }
  m.rms = std::sqrt(s2 / double(now.size()));
  m.flicker = std::sqrt(f2 / double(now.size()));
  if (spectrum) {
    m.shape = blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(now, kN, true), kN);
  }
  return m;
}

// Frames of the pass: the noise (`vary`: the frame term moves every frame; else frame 0), the filter, and with `history` the temporal pass. `noise_free` feeds the pipeline the expectation instead (the reference
// of Compare). The world's true visibility field shifts with the camera by (sx, sy) texels a frame (the world position of a texel at frame f is (x + sx f, y + sy f)).
Run RunContact(const Visibility& visibility, const std::function<double(double, double)>& truth, bool vary, bool history, bool noise_free, int frames, double sx, double sy, int cap = 12) {
  Run run;
  Image prev;
  bool have = false;
  int age = 0;
  for (int f = 0; f < frames; ++f) {
    Image raw;
    for (int y = 0; y < kN; ++y) {
      for (int x = 0; x < kN; ++x) {
        const double wx = double(x) + sx * double(f), wy = double(y) + sy * double(f);
        raw.At(x, y) = noise_free ? float(truth(wx, wy)) : visibility(x, y, vary ? uint32_t(100 + f) & 1023u : 0u, wx, wy);
      }
    }
    Image filtered = Filter(raw);
    Image result = filtered;
    if (history) {
      const double weight = have ? 1.0 - 1.0 / double(std::min(age + 2, cap)) : 0.0;
      result = Accumulate(filtered, have ? &prev : nullptr, weight, sx, sy);
      age = have ? std::min(age + 1, cap) : 0;
    }
    run.before_last = run.last;
    run.last = result;
    prev = result;
    have = true;
  }
  return run;
}

// The contact march: the ray's start is the noise inside its first step, a thin occluder of coverage c (the share of a step it covers) hits when the noise falls in its interval: visibility 1 where the noise
// is at or above c. The noise is the shader's: the blue tile (channel 0) plus the frame term times 0.75487766624669 (ao_contact_ps.hlsl).
bool g_uniform = false;  // the spectra want a stationary noise: a field with the same coverage everywhere (a smooth field modulates the noise's variance by p (1 - p), which shows as one frequency)
double Coverage(double wx, double wy) { return g_uniform ? 0.5 : 0.5 + 0.4 * std::sin(2.0 * kPi * (2.0 * wx + 1.0 * wy) / double(kN)); }
double MarchTruth(double wx, double wy) { return 1.0 - Coverage(wx, wy); }
float MarchVisibility(int x, int y, uint32_t frame_term, double wx, double wy) {
  const float noise = hs::frac(hs::BlueNoiseTileAt(x, y, 0u) + float(frame_term) * 0.75487766624669f);
  return noise >= float(Coverage(wx, wy)) ? 1.0f : 0.0f;
}
// The same with the occluder's soft edge (the step's ramp covers a quarter of a step: a thicker occluder, the common case of the world's pixels): a ramp, not a binary stipple. The truth is the ramp's mean over the noise.
double SoftTruth(double wx, double wy) {
  const double c = Coverage(wx, wy);
  double sum = 0.0;
  for (int i = 0; i < 256; ++i) {
    const double n = (double(i) + 0.5) / 256.0;
    sum += std::clamp((n - c) / 0.25 + 0.5, 0.0, 1.0);
  }
  return sum / 256.0;
}
float SoftVisibility(int x, int y, uint32_t frame_term, double wx, double wy) {
  const float noise = hs::frac(hs::BlueNoiseTileAt(x, y, 0u) + float(frame_term) * 0.75487766624669f);
  return std::clamp((noise - float(Coverage(wx, wy))) / 0.25f + 0.5f, 0.0f, 1.0f);
}

// The traced sun ray: the disc sample (radius sqrt(u1), angle 2 pi u2: two numbers of car_rt_ao_noise.hlsli) against an edge at offset d across the sun's disc: lit where the sample's offset along the edge's
// normal is at or beyond d. The truth is the share of the disc beyond d.
double SunOffset(double wx, double wy) { return g_uniform ? 0.0 : 1.1 * std::sin(2.0 * kPi * (1.0 * wx + 2.0 * wy) / double(kN)); }
double SunTruth(double wx, double wy) {
  static std::vector<double> table;  // 1 - P(offset < d), d from -1.2 to 1.2 in 241 steps
  if (table.empty()) {
    for (int i = 0; i <= 240; ++i) {
      const double d = -1.2 + 2.4 * double(i) / 240.0;
      int lit = 0, total = 0;
      for (int a = 0; a < 200; ++a) {
        for (int b = 0; b < 200; ++b) {
          const double u1 = (double(a) + 0.5) / 200.0, u2 = (double(b) + 0.5) / 200.0;
          lit += std::sqrt(u1) * std::cos(2.0 * kPi * u2) >= d ? 1 : 0;
          ++total;
        }
      }
      table.push_back(double(lit) / double(total));
    }
  }
  const double d = std::clamp(SunOffset(wx, wy), -1.2, 1.2);
  const double f = (d + 1.2) / 2.4 * 240.0;
  const int i = std::min(int(f), 239);
  return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * (f - double(i));
}
float SunVisibility(int x, int y, uint32_t frame_term, double wx, double wy) {
  const hs::float2 u = hs::CarRtSunNoise(x, y, true, frame_term);
  return std::sqrt(u.x) * std::cos(2.0f * float(kPi) * u.y) >= float(SunOffset(wx, wy)) ? 1.0f : 0.0f;
}

void TestVariation() {
  const auto march = [](int x, int y, uint32_t f) {
    return std::pair<float, float>(hs::frac(hs::BlueNoiseTileAt(x, y, 0u) + float((100u + f) & 1023u) * 0.75487766624669f), 0.0f);
  };
  const nc::PerFrame p = nc::PerFrameOf(march, 0);
  if (g_check.print) {
    nc::Print("contact march ray start, by frame", p);
  }
  // (the march's frame term is a float multiple, not the wrapped 32 bit fraction of the other effects: the step is the same for every pixel all the same, and the numbers stay uniform)
  g_check(p.changed > 0.99 && p.step_spread < 1e-3 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
          nc::Num("the contact march's start changes at every pixel every frame (%.3f), is uniform (mean %.3f, variance %.4f) and takes the same step at every pixel (%.6f): nothing slides", p.changed, p.mean, p.var, p.step_spread));
  const auto sun = [](int x, int y, uint32_t f) {
    const hs::float2 u = hs::CarRtSunNoise(x, y, true, (100u + f) & 1023u);
    return std::pair<float, float>(u.x, u.y);
  };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame q = nc::PerFrameOf(sun, channel);
    g_check(q.changed > 0.99 && q.max_gap < 0.25 && q.step_spread < 1e-4 && std::fabs(q.mean - 0.5) < 0.01,
            nc::Num("the traced sun ray's number %d changes at every pixel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes one step at every pixel (%.6f)", channel + 1, q.changed, q.max_gap, q.step_spread));
  }
  // the fixed pattern the first fixes kept (frame term 0) really is one pattern
  bool fixed = true;
  for (int y = 0; y < 16 && fixed; ++y) {
    for (int x = 0; x < 16 && fixed; ++x) {
      fixed = MarchVisibility(x, y, 0u, x, y) == MarchVisibility(x, y, 0u, x, y) && SunVisibility(x, y, 0u, 0.0, 0.0) == SunVisibility(x, y, 0u, 0.0, 0.0);
    }
  }
  g_check(fixed, "(the fixed pattern is the frame term 0: the A/B switch's old behaviour)");
}

void TestContact(const char* what, const Visibility& vis, const std::function<double(double, double)>& truth) {
  // still camera
  const Run clean_fixed = RunContact(vis, truth, false, false, true, 14, 0, 0);
  const Run clean_resolved = RunContact(vis, truth, true, true, true, 14, 0, 0);
  const Metrics fixed = Compare(RunContact(vis, truth, false, false, false, 14, 0, 0), clean_fixed, true);
  const Metrics vary_only = Compare(RunContact(vis, truth, true, false, false, 14, 0, 0), clean_fixed, true);
  const Metrics resolved = Compare(RunContact(vis, truth, true, true, false, 14, 0, 0), clean_resolved, false);
  g_uniform = true;
  const Metrics uniform = Compare(RunContact(vis, truth, true, true, false, 14, 0, 0), RunContact(vis, truth, true, true, true, 14, 0, 0), true);
  g_uniform = false;
  if (g_check.print) {
    std::printf("  %s, still: fixed pattern + filter: noise rms %.4f flicker %.4f | per-frame noise + filter only: rms %.4f flicker %.4f | + history: rms %.4f flicker %.4f, one frequency %.4f, direction %.2f (a uniform field)\n", what,
                fixed.rms, fixed.flicker, vary_only.rms, vary_only.flicker, resolved.rms, resolved.flicker, uniform.shape.peak_share, uniform.shape.wedge_ratio);
  }
  g_check(resolved.rms < 0.45 * fixed.rms,
          nc::Num("%s, still camera: after 14 frames the history leaves a noise rms of %.4f against the fixed pattern's %.4f (%.2f of it)", what, resolved.rms, fixed.rms, resolved.rms / fixed.rms));
  g_check(vary_only.flicker > 5.0 * resolved.flicker && fixed.flicker < 1e-6,
          nc::Num("%s: a per-frame noise nothing averages boils (flicker %.4f a frame); the history takes it to %.4f; the fixed pattern has none (%.4f)", what, vary_only.flicker, resolved.flicker, fixed.flicker));
  g_check(uniform.shape.peak_share < 0.012 && uniform.shape.wedge_ratio < 3.0,
          nc::Num("%s: the resolved noise of a uniform field has no lattice (%.4f of it in one frequency, a direction %.2f times the mean)", what, uniform.shape.peak_share, uniform.shape.wedge_ratio));
  // moving camera: the world's field moves by (0.5, 0.25) texels a frame, the history is read at the reprojected place (a plane: exact, the bilinear read blurs a little: the same in the reference)
  const Run clean_moving_fixed = RunContact(vis, truth, false, false, true, 14, 0.5, 0.25);
  const Run clean_moving = RunContact(vis, truth, true, true, true, 14, 0.5, 0.25);
  const Metrics moving_fixed = Compare(RunContact(vis, truth, false, false, false, 14, 0.5, 0.25), clean_moving_fixed, false);
  const Metrics moving = Compare(RunContact(vis, truth, true, true, false, 14, 0.5, 0.25), clean_moving, false);
  if (g_check.print) {
    std::printf("  %s, moving 0.5, 0.25 texels a frame: fixed noise rms %.4f | resolved rms %.4f flicker %.4f\n", what, moving_fixed.rms, moving.rms, moving.flicker);
  }
  g_check(moving.rms < 0.7 * moving_fixed.rms,
          nc::Num("%s, moving camera: the history's noise rms %.4f against the fixed pattern's %.4f (%.2f of it): the bilinear read of a moving history costs some of the gain, not all (the residual is carried with the world, as the occlusion's)",
                  what, moving.rms, moving_fixed.rms, moving.rms / moving_fixed.rms));
}

// A shadow edge that moves while the camera stands still (a car driving by, the sun's own move): the same clamp and weights as the occlusion's, so the same ghost.
void TestContactGhost() {
  const auto edge_visibility = [](int x, int, uint32_t, double, double) { return 0.0f * float(x); };  // unused: the edge image is made below
  (void)edge_visibility;
  Image prev;
  bool have = false;
  int age = 0;
  double worst_far = 0.0, worst_near = 0.0;
  for (int f = 0; f < 16; ++f) {
    Image raw;
    raw.periodic = false;
    const int edge = 20 + f;  // pixels left of the edge are in shadow
    for (int y = 0; y < kN; ++y) {
      for (int x = 0; x < kN; ++x) {
        raw.At(x, y) = x < edge ? 0.0f : 1.0f;
      }
    }
    Image filtered = Filter(raw);
    filtered.periodic = false;
    const double weight = have ? 1.0 - 1.0 / double(std::min(age + 2, 12)) : 0.0;
    Image result = Accumulate(filtered, have ? &prev : nullptr, weight, 0.0, 0.0);
    result.periodic = false;
    if (have) {
      for (int x = 0; x < kN; ++x) {
        const double ghost = std::fabs(double(result.At(x, 32)) - double(filtered.At(x, 32)));
        const int distance = std::abs(x - edge);
        (distance >= 3 ? worst_far : worst_near) = std::max(distance >= 3 ? worst_far : worst_near, ghost);
      }
    }
    prev = result;
    have = true;
    age = std::min(age + 1, 12);
  }
  if (g_check.print) {
    std::printf("  moving shadow edge (1 texel a frame, camera still): the history differs from the frame's own value by at most %.3f at 3 texels or more from the edge and %.3f within 2\n", worst_far, worst_near);
  }
  g_check(worst_far <= (1.0 - 1.0 / 12.0) * double(kAoClampMargin) + 0.01,
          nc::Num("a shadow edge that moves at a texel a frame leaves a ghost of at most %.3f beyond 2 texels of the edge: the clamp's margin %.2f times the weight %.3f (the occlusion's history leaves the same: one pass, one clamp)",
                  worst_far, double(kAoClampMargin), 1.0 - 1.0 / 12.0));
}

// ---------------------------------------------------------------------------
// the shafts: the radial blur's start noise, averaged with the last frame's image shifted by the sun's own move
// ---------------------------------------------------------------------------

constexpr int kShaftSteps = 32;
double Shine(double u, double v, double parallax_u) {  // the sky's brightness: sky above a skyline (smooth edge, as the bilinear read of the half size image)
  const double h = 0.52 + 0.1 * std::sin(2.0 * kPi * (2.0 * (u + parallax_u))) + 0.05 * std::sin(2.0 * kPi * (5.0 * (u + parallax_u) + 0.3));
  const double t = std::clamp((h - v) / 0.04 + 0.5, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// One frame of the blur: the sun at `sun`, the scene shifted by `scene` (u) and `parallax` (the layer that moves faster than the sun: trees beside the road); `expected` averages the start noise out.
float Blur(int x, int y, double sun_u, double sun_v, double scene, double parallax, uint32_t shift, bool expected) {
  const double u = (double(x) + 0.5) / kN, v = (double(y) + 0.5) / kN;
  const double du = (sun_u - u) * 0.9 / kShaftSteps, dv = (sun_v - v) * 0.9 / kShaftSteps;
  const int variants = expected ? 64 : 1;
  double total = 0.0;
  for (int k = 0; k < variants; ++k) {
    const double noise = expected ? (double(k) + 0.5) / 64.0 : double(hs::ShaftsStartNoise(x, y, hs::float2(float(x) + 0.5f, float(y) + 0.5f), true, shift));
    double sum = 0.0;
    for (int i = 0; i < kShaftSteps; ++i) {
      sum += Shine(u + du * (noise + double(i) + 1.0) + scene, v + dv * (noise + double(i) + 1.0), parallax);
    }
    total += sum / kShaftSteps;
  }
  return float(total / variants);
}

Run RunShafts(bool vary, bool history, bool noise_free, int frames, double turn_u, double parallax_rate) {
  Run run;
  Image prev;
  bool have = false;
  uint32_t held = 0;
  double sun_u = 0.5, sun_v = 0.18, prev_sun_u = 0.5, prev_sun_v = 0.18;
  for (int f = 0; f < frames; ++f) {
    sun_u = 0.5 - turn_u * f;  // the camera turns: the scene and the sun move left together
    Image current;
    current.periodic = false;
    for (int y = 0; y < kN; ++y) {
      for (int x = 0; x < kN; ++x) {
        current.At(x, y) = Blur(x, y, sun_u, sun_v, turn_u * f, parallax_rate * f, vary ? uint32_t(100 + f) & 1023u : 0u, noise_free);
      }
    }
    Image result = current;
    if (history && have) {
      // the history is read at the texel's place shifted by the sun's move (last sun - this sun), as the shader does; the weight, the clamp and the margin are noise_resolve.h's
      const double shift_u = (prev_sun_u - sun_u), shift_v = (prev_sun_v - sun_v);
      const bool keep = nr::ShaftsKeepsHistory(float(shift_u), float(shift_v), 1.0f);
      const double weight = keep ? double(nr::ShaftsHistoryWeight(held)) : 0.0;
      for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
          float lo = current.At(x, y), hi = lo;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              lo = std::min(lo, current.At(x + dx, y + dy));
              hi = std::max(hi, current.At(x + dx, y + dy));
            }
          }
          const double hx = double(x) + shift_u * kN, hy = double(y) + shift_v * kN;
          const bool inside = hx >= -0.5 && hx <= kN - 0.5 && hy >= -0.5 && hy <= kN - 0.5;
          if (keep && inside) {
            const float raw = prev.Sample(hx, hy);
            const float h = std::clamp(raw, lo - nr::kShaftsClampMargin, hi + nr::kShaftsClampMargin);
            const double kept = weight * double(nr::StaleKeep(std::fabs(raw - current.At(x, y)), nr::kShaftsStaleFrom, nr::kShaftsStaleTo));  // the shader's stale rule
            result.At(x, y) = float(double(current.At(x, y)) + (double(h) - double(current.At(x, y))) * kept);
          }
        }
      }
      held = keep ? std::min(held + 1u, 64u) : 0u;
    } else if (history) {
      held = 0;
    }
    run.before_last = run.last;
    run.last = result;
    prev = result;
    prev.periodic = false;
    have = true;
    prev_sun_u = sun_u;
    prev_sun_v = sun_v;
  }
  return run;
}

// The largest difference of two runs' last pictures away from the border.
double Worst(const Run& a, const Run& b) {
  double worst = 0.0;
  for (int y = 2; y < kN - 2; ++y) {
    for (int x = 2; x < kN - 2; ++x) {
      worst = std::max(worst, std::fabs(double(a.last.At(x, y)) - double(b.last.At(x, y))));
    }
  }
  return worst;
}

void TestShafts() {
  const auto start = [](int x, int y, uint32_t f) { return std::pair<float, float>(hs::ShaftsStartNoise(x, y, hs::float2(float(x) + 0.5f, float(y) + 0.5f), true, (100u + f) & 1023u), 0.0f); };
  const nc::PerFrame p = nc::PerFrameOf(start, 0);
  g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01,
          nc::Num("the shafts' ray start changes at every texel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes one step at every texel (%.6f)", p.changed, p.max_gap, p.step_spread));
  // still camera (the sun and the scene stand still)
  const Run clean_fixed = RunShafts(false, false, true, 14, 0.0, 0.0);
  const Run clean_resolved = RunShafts(true, true, true, 14, 0.0, 0.0);
  const Metrics fixed = Compare(RunShafts(false, false, false, 14, 0.0, 0.0), clean_fixed, false);
  const Metrics vary_only = Compare(RunShafts(true, false, false, 14, 0.0, 0.0), clean_fixed, false);
  const Metrics resolved = Compare(RunShafts(true, true, false, 14, 0.0, 0.0), clean_resolved, false);
  if (g_check.print) {
    std::printf("  shafts, still: fixed noise rms %.5f flicker %.5f | per-frame only rms %.5f flicker %.5f | + history rms %.5f flicker %.5f\n", fixed.rms, fixed.flicker, vary_only.rms, vary_only.flicker, resolved.rms,
                resolved.flicker);
  }
  g_check(resolved.rms < 0.65 * fixed.rms && resolved.rms < 0.65 * vary_only.rms,
          nc::Num("shafts, still camera: after 14 frames the history leaves a noise rms of %.5f against the fixed pattern's %.5f and one frame's per-frame noise %.5f", resolved.rms, fixed.rms, vary_only.rms));
  g_check(vary_only.flicker > 1.5 * resolved.flicker && fixed.flicker < 1e-6,
          nc::Num("shafts: a per-frame noise nothing averages boils (flicker %.5f a frame); the history takes it to %.5f", vary_only.flicker, resolved.flicker));
  // moving camera: the sun and the scene turn together by 0.007 of the picture a frame (0.5 degrees of a 70 degree picture), the history follows by the sun's move
  const Run clean_moving_fixed = RunShafts(false, false, true, 14, 0.007, 0.0);
  const Run clean_moving = RunShafts(true, true, true, 14, 0.007, 0.0);
  const Metrics moving_fixed = Compare(RunShafts(false, false, false, 14, 0.007, 0.0), clean_moving_fixed, false);
  const Metrics moving = Compare(RunShafts(true, true, false, 14, 0.007, 0.0), clean_moving, false);
  g_check(moving.rms < 0.75 * moving_fixed.rms,
          nc::Num("shafts, turning camera (0.007 of the picture a frame): the history's noise rms %.5f against the fixed pattern's %.5f (%.2f of it)", moving.rms, moving_fixed.rms, moving.rms / std::max(moving_fixed.rms, 1e-9)));
  // parallax: a skyline layer that moves 0.003 of the picture a frame faster than the sun: the history trails it; against the frame's own picture (what no history would show) the trail is bounded
  const Run own = RunShafts(true, false, true, 14, 0.007, 0.003);       // the frame's own expectation: what a history-free pass shows without its noise
  const Run trailed = RunShafts(true, true, true, 14, 0.007, 0.003);    // the same with the history: only the ghost differs
  const double ghost = Worst(trailed, own);
  if (g_check.print) {
    std::printf("  shafts, a layer moving 0.003 of the picture a frame faster than the sun: the history's ghost is at most %.4f of the brightness (the clamp's margin %.2f)\n", ghost, double(nr::kShaftsClampMargin));
  }
  g_check(ghost < 0.06, nc::Num("shafts, a layer moving faster than the sun by 0.003 of the picture a frame: the history trails it by at most %.4f of the brightness (the image's range is 0 to 1)", ghost));
  // a jump of the sun (a cut) keeps no history
  g_check(!nr::ShaftsKeepsHistory(0.2f, 0.0f, 1.0f) && nr::ShaftsKeepsHistory(0.007f, 0.0f, 1.8f) && nr::ShaftsHistoryWeight(0) == 0.0f && nr::ShaftsHistoryWeight(40) == nr::kShaftsHistoryWeight,
          "the shafts' history is dropped when the sun jumps by more than 15% of the image's height, and its weight ramps from the plain mean to its cap");
}

// REVIEW F1 (perf\LIGHT-STACK-REVIEW.md): the two history images are made again when the shafts' image changes size, and a new image holds nothing. This is the host's side (shafts.cpp EnsureImages +
// RecordTemporal) as a twin, written from the source (the statements are pinned in TestSources below): a frame at a new size must have weight 0, and the frame after it the plain mean of one frame
// (not a leftover of the old counters).
struct ShaftsHost {
  bool valid = false;        // hist_valid_
  uint64_t frame_of = 0;     // hist_frame_
  bool pause_of = false;     // hist_pause_
  float sun_of[2] = {};      // hist_sun_
  uint32_t frames = 0;       // hist_frames_
  uint32_t image[2] = {0, 0};  // the size of the history images
  uint64_t frame = 0;
  float weight_last = -1.0f;
  bool keep_last = false;
  // One frame at `w` x `h` with the sun at (sun_u, sun_v). `ensure_invalidates` is the fix: EnsureImages tells the history that it made the images again.
  void Frame(uint32_t w, uint32_t h, float sun_u, float sun_v, bool ensure_invalidates, bool pause = false, uint64_t skip = 0) {
    frame += 1 + skip;
    if (image[0] != w || image[1] != h) {
      image[0] = w;
      image[1] = h;  // HostPasses::EnsureImage made new textures: undefined contents, layout unknown (RecordTemporal's own size test compares the images with the plan: it can never differ here)
      if (ensure_invalidates) {
        valid = false;
        frames = 0;
      }
    }
    const float shift_u = sun_of[0] - sun_u;
    const float shift_v = sun_of[1] - sun_v;
    const float aspect = float(w) / float(h);
    const bool keep = valid && frame_of + 1 == frame && pause_of == pause && std::isfinite(shift_u) && std::isfinite(shift_v) && nr::ShaftsKeepsHistory(shift_u, shift_v, aspect);
    keep_last = keep;
    weight_last = keep ? nr::ShaftsHistoryWeight(frames) : 0.0f;  // push.sun[2]
    valid = true;
    frame_of = frame;
    pause_of = pause;
    sun_of[0] = sun_u;
    sun_of[1] = sun_v;
    frames = keep ? std::min(frames + 1u, 64u) : 1u;
  }
};

void TestShaftsHistoryGate() {
  // a steady run: no history on the first frame, then the plain mean of 1 frame (1/2), then of 2 (the cap)
  ShaftsHost host;
  host.Frame(960, 540, 0.5f, 0.2f, true);
  const bool first_none = !host.keep_last && host.weight_last == 0.0f;
  host.Frame(960, 540, 0.5f, 0.2f, true);
  const float second = host.weight_last;
  host.Frame(960, 540, 0.5f, 0.2f, true);
  const float third = host.weight_last;
  g_check(first_none && host.keep_last && std::fabs(second - 0.5f) < 1e-6f && std::fabs(third - nr::kShaftsHistoryWeight) < 1e-6f && host.frames == 3u,
          nc::Num("the shafts' history gate: no history on the first frame, weight %.2f on the second (the mean of two), %.2f on the third (the cap)", double(second), double(third)));
  // the images change size (render scale, window): the next frame has NO history (weight 0) and the one after it the mean of two frames again
  ShaftsHost sized;
  sized.Frame(960, 540, 0.5f, 0.2f, true);
  sized.Frame(960, 540, 0.5f, 0.2f, true);
  sized.Frame(960, 540, 0.5f, 0.2f, true);
  const bool had = sized.keep_last && sized.weight_last == nr::kShaftsHistoryWeight;
  sized.Frame(1280, 720, 0.5f, 0.2f, true);  // the images are made again at the new size
  const bool zero_after_size = !sized.keep_last && sized.weight_last == 0.0f;
  sized.Frame(1280, 720, 0.5f, 0.2f, true);
  const float after = sized.weight_last;
  g_check(had && zero_after_size && sized.keep_last && std::fabs(after - 0.5f) < 1e-6f,
          nc::Num("a size change of the shafts' image forces weight 0 on the next frame (the history had %.2f before), and the frame after it starts again at the mean of two (%.2f)", double(nr::kShaftsHistoryWeight), double(after)));
  // what the gate alone would do: the same sequence WITHOUT the invalidation reads the new, never-written image at the cap's weight (the bug the review found)
  ShaftsHost bug;
  bug.Frame(960, 540, 0.5f, 0.2f, false);
  bug.Frame(960, 540, 0.5f, 0.2f, false);
  bug.Frame(960, 540, 0.5f, 0.2f, false);
  bug.Frame(1280, 720, 0.5f, 0.2f, false);
  g_check(bug.keep_last && bug.weight_last == nr::kShaftsHistoryWeight,
          "without the invalidation the gate keeps the history across a size change at its full weight: this is why EnsureImages must clear it (the sources are pinned below)");
  // a second change right after the first, and a change back to the first size, still start from nothing each time
  ShaftsHost churn;
  churn.Frame(960, 540, 0.5f, 0.2f, true);
  churn.Frame(960, 540, 0.5f, 0.2f, true);
  churn.Frame(1280, 720, 0.5f, 0.2f, true);
  const bool a = !churn.keep_last && churn.weight_last == 0.0f;
  churn.Frame(960, 540, 0.5f, 0.2f, true);
  const bool b = !churn.keep_last && churn.weight_last == 0.0f;
  churn.Frame(960, 540, 0.5f, 0.2f, true);
  g_check(a && b && churn.keep_last && std::fabs(churn.weight_last - 0.5f) < 1e-6f, "a size change back and forth forces weight 0 at each change");
  // the other reasons to start again: a gap of a frame, another kind of view, a sun that jumped or is not a number
  ShaftsHost reasons;
  reasons.Frame(960, 540, 0.5f, 0.2f, true);
  reasons.Frame(960, 540, 0.5f, 0.2f, true, false, 1);  // a frame without the pass in between
  const bool gap = !reasons.keep_last;
  reasons.Frame(960, 540, 0.5f, 0.2f, true);
  reasons.Frame(960, 540, 0.5f, 0.2f, true, true);  // the pause view
  const bool pause = !reasons.keep_last;
  reasons.Frame(960, 540, 0.5f, 0.2f, true, true);
  reasons.Frame(960, 540, 0.9f, 0.2f, true, true);  // the sun jumped across the picture
  const bool jump = !reasons.keep_last;
  reasons.Frame(960, 540, 0.9f, 0.2f, true, true);
  const bool steady_again = reasons.keep_last;
  reasons.Frame(960, 540, std::nanf(""), 0.2f, true, true);
  const bool not_a_number = !reasons.keep_last && reasons.weight_last == 0.0f;
  g_check(gap && pause && jump && steady_again && not_a_number, "the history is not kept over a gap of a frame, a change of the kind of view, a jump of the sun or a sun that is no number");
  ShaftsHost capped;
  for (int f = 0; f < 100; ++f) {
    capped.Frame(960, 540, 0.5f, 0.2f, true);
  }
  g_check(capped.frames == 64u, "the frame count stops at 64");
}

// ---------------------------------------------------------------------------
// the cone: the finished image (two a-trous passes) averaged with the last frame's result, texel for texel, clamped to the 3 x 3 range
// ---------------------------------------------------------------------------

constexpr double kBand = 12.0;  // the cone's penumbra on the panel, in texels (a part half a metre off: about 8 to 12)

// The share of the cone's four rays that hit the part: ray k's offset across the edge is r cos(angle); the edge's offset at texel column x is d(x) = 2 (x - x0) / kBand - 1 (-1 .. 1 across the band).
double ConeOffsetD(double x, double x0) { return std::clamp(2.0 * (x - x0) / kBand - 1.0, -1.2, 1.2); }
float ConeWeight(int x, int y, uint32_t phase, double x0) {
  const float turn = rt::ConeRotation(uint32_t(x + 100), uint32_t(y), phase);
  int hits = 0;
  for (int ray = 0; ray < 4; ++ray) {
    const float angle = 6.283185307179586f * (turn + 0.25f * float(ray));
    hits += (rt::ConeRadius(ray) * std::cos(angle) > float(ConeOffsetD(x, x0))) ? 1 : 0;
  }
  return float(hits) / 4.0f;
}
double ConeTruth(double x, double x0) {
  double sum = 0.0;
  for (int t = 0; t < 512; ++t) {
    const double turn = (double(t) + 0.5) / 512.0;
    int hits = 0;
    for (int ray = 0; ray < 4; ++ray) {
      hits += (rt::ConeRadius(ray) * std::cos(2.0 * kPi * (turn + 0.25 * double(ray))) > ConeOffsetD(x, x0)) ? 1 : 0;
    }
    sum += double(hits) / 4.0;
  }
  return sum / 512.0;
}

// An a-trous pass of the denoise at full strength (the band is wider than the second pass's 10 texels): the kernel 1 2 1 / 2 4 2 / 1 2 1 with taps `step` texels apart, the edge clamped.
Image Atrous(const Image& in, int step) {
  Image out;
  out.periodic = false;
  for (int y = 0; y < kN; ++y) {
    for (int x = 0; x < kN; ++x) {
      double sum = 0.0, w = 0.0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const double k = (dx == 0 ? 2.0 : 1.0) * (dy == 0 ? 2.0 : 1.0);
          sum += k * double(in.At(x + dx * step, y + dy * step));
          w += k;
        }
      }
      out.At(x, y) = float(sum / w);
    }
  }
  return out;
}

Run RunCone(bool vary, bool history, bool noise_free, int frames, double speed) {
  Run run;
  Image prev;
  prev.periodic = false;
  bool have = false;
  uint32_t held = 0;
  for (int f = 0; f < frames; ++f) {
    const double x0 = 16.0 + speed * double(f);
    Image trace;
    trace.periodic = false;
    for (int y = 0; y < kN; ++y) {
      for (int x = 0; x < kN; ++x) {
        trace.At(x, y) = noise_free ? float(ConeTruth(double(x), x0)) : ConeWeight(x, y, vary ? uint32_t(f) & 7u : 0u, x0);
      }
    }
    Image finished = Atrous(Atrous(trace, 1), 2);
    finished.periodic = false;
    Image result = finished;
    if (history && have) {
      const double keep = double(nr::ConeHistoryWeight(held));
      for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
          float lo = finished.At(x, y), hi = lo;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              lo = std::min(lo, finished.At(x + dx, y + dy));
              hi = std::max(hi, finished.At(x + dx, y + dy));
            }
          }
          const float raw = prev.At(x, y);
          const float h = std::clamp(raw, lo - nr::kConeClampMargin, hi + nr::kConeClampMargin);
          const double kept = keep * double(nr::StaleKeep(std::fabs(raw - finished.At(x, y)), nr::kConeStaleFrom, nr::kConeStaleTo));  // the shader's stale rule
          result.At(x, y) = float(double(finished.At(x, y)) + (double(h) - double(finished.At(x, y))) * kept);
        }
      }
    }
    held = (history && have) ? std::min(held + 1u, 64u) : 0u;
    run.before_last = run.last;
    run.last = result;
    run.last.periodic = false;
    prev = result;
    prev.periodic = false;
    have = true;
  }
  return run;
}

// Where the ramp (along row 32) crosses 0.5: the history's trail shows as a shift of the crossing.
double Crossing(const Run& r) {
  for (int x = 1; x < kN; ++x) {
    const double a = double(r.last.At(x - 1, 32)), b = double(r.last.At(x, 32));
    if (a >= 0.5 && b < 0.5) {
      return double(x - 1) + (a - 0.5) / (a - b);
    }
  }
  return -1.0;
}

void TestCone() {
  const auto turn = [](int x, int y, uint32_t f) { return std::pair<float, float>(rt::ConeRotation(uint32_t(x + 100), uint32_t(y), f & 7u), 0.0f); };
  const nc::PerFrame p = nc::PerFrameOf(turn, 0, 104);
  g_check(p.changed > 0.99 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01,
          nc::Num("the cone's turn changes at every texel every frame (%.3f: the golden step of the phase) and takes the same step at every texel (%.6f)", p.changed, p.step_spread));
  // a still view
  const Run clean_fixed = RunCone(false, false, true, 14, 0.0);
  const Run clean_resolved = RunCone(true, true, true, 14, 0.0);
  const Metrics fixed = Compare(RunCone(false, false, false, 14, 0.0), clean_fixed, false);
  const Metrics vary_only = Compare(RunCone(true, false, false, 14, 0.0), clean_fixed, false);
  const Metrics resolved = Compare(RunCone(true, true, false, 14, 0.0), clean_resolved, false);
  if (g_check.print) {
    std::printf("  cone, still: fixed noise rms %.5f flicker %.5f | per-frame only rms %.5f flicker %.5f | + history rms %.5f flicker %.5f (of a weight of 1, after the two a-trous passes)\n", fixed.rms, fixed.flicker,
                vary_only.rms, vary_only.flicker, resolved.rms, resolved.flicker);
  }
  g_check(resolved.rms < 0.6 * fixed.rms,
          nc::Num("cone, still view: after 14 frames the history leaves a noise rms of %.5f against the fixed pattern's %.5f (%.2f of it) after the two a-trous passes", resolved.rms, fixed.rms, resolved.rms / fixed.rms));
  g_check(vary_only.flicker > 1.4 * resolved.flicker && fixed.flicker < 1e-6,
          nc::Num("cone: a per-frame pattern the a-trous passes alone resolve flickers by %.5f a frame; the history takes it to %.5f", vary_only.flicker, resolved.flicker));
  g_check(vary_only.rms < 0.05 && fixed.rms < 0.05, nc::Num("(the a-trous passes' residual is small either way: %.4f varying, %.4f fixed, of a weight of 1)", vary_only.rms, fixed.rms));
  // a moving view: the ramp moves by 1.2 texels a frame (0.5 degrees a frame against a 2.5 degree cone is a fifth of the half cone, about a texel and a half of this 12 texel band)
  const Run clean_moving_fixed = RunCone(false, false, true, 14, 1.2);
  const Run clean_moving = RunCone(true, true, true, 14, 1.2);
  const Metrics moving_fixed = Compare(RunCone(false, false, false, 14, 1.2), clean_moving_fixed, false);
  const Metrics moving = Compare(RunCone(true, true, false, 14, 1.2), clean_moving, false);
  const Run own = RunCone(true, false, true, 14, 1.2);  // the frame's own expectation: what a history-free pass shows without its noise
  const double ghost = Worst(clean_moving, own);
  const double trail = Crossing(clean_moving) - Crossing(own);
  if (g_check.print) {
    std::printf("  cone, ramp moving 1.2 texels a frame: fixed noise rms %.5f | history noise rms %.5f flicker %.5f | the history's trail: worst %.4f of the weight, the 50%% point %.2f texels behind (the ramp is %.0f wide)\n",
                moving_fixed.rms, moving.rms, moving.flicker, ghost, trail, kBand);
  }
  // moving, the history must not cost more than it saves: its total error against the frame's own expectation (noise and trail together) stays within the fixed pattern's noise
  const Run moving_noisy = RunCone(true, true, false, 14, 1.2);
  double total2 = 0.0, fixed2 = 0.0;
  const Run moving_fixed_run = RunCone(false, false, false, 14, 1.2);
  for (size_t i = 0; i < own.last.v.size(); ++i) {
    total2 += (double(moving_noisy.last.v[i]) - double(own.last.v[i])) * (double(moving_noisy.last.v[i]) - double(own.last.v[i]));
    fixed2 += (double(moving_fixed_run.last.v[i]) - double(own.last.v[i])) * (double(moving_fixed_run.last.v[i]) - double(own.last.v[i]));
  }
  const double total_rms = std::sqrt(total2 / double(own.last.v.size())), fixed_total = std::sqrt(fixed2 / double(own.last.v.size()));
  g_check(total_rms < 1.25 * fixed_total,
          nc::Num("cone, the ramp moving: the history's total error (its noise and its trail) is %.5f against the fixed pattern's noise alone %.5f (%.2f of it): the history is not worse than none while the view moves", total_rms, fixed_total,
                  total_rms / fixed_total));
  // the unclamped history would trail by weight / (1 - weight) frames of motion
  const double trail_unclamped = double(nr::kConeHistoryWeight) / (1.0 - double(nr::kConeHistoryWeight)) * 1.2;
  g_check(std::fabs(trail) < 0.4 * trail_unclamped && std::fabs(trail) < 0.15 * kBand && ghost < 0.2,
          nc::Num("cone, the ramp moving 1.2 texels a frame: the history trails it by %.2f texels (an unclamped history of this weight: %.1f; the ramp is %.0f wide) and its worst difference from the frame's own value is %.3f of the weight: "
                  "the clamp bounds the trail",
                  std::fabs(trail), trail_unclamped, kBand, ghost));
  g_check(std::fabs(nr::ConeHistoryWeight(0) - 0.5f) < 1e-6 && nr::ConeHistoryWeight(1) < nr::ConeHistoryWeight(2) + 1e-6 && nr::ConeHistoryWeight(50) == nr::kConeHistoryWeight,
          "the cone history's weight ramps from the plain mean of two frames to its cap");
}

// ---------------------------------------------------------------------------
// CHECK 2: the upscaler's jitter in a pinned still
// ---------------------------------------------------------------------------

void Frame(int64_t update) {
  sn::SetUpdate(update);
  sn::OnHandOver();
  sn::OnGuestSwap();
  sn::BeginFrame();
}

// temporal.cpp: jitter_index = Active ? NoiseIndex(fallback) % phases : fallback % phases (was Index(fallback) % phases)
uint32_t JitterIndex(bool noise_index, uint64_t fallback, uint32_t phases) {
  return uint32_t(sn::Active() ? (noise_index ? sn::NoiseIndex(fallback) : sn::Index(fallback)) % phases : fallback % phases);
}

void TestJitter() {
  sn::Rearm();
  sn::SetCountHeld(true);
  constexpr uint32_t kPhases = 8;
  bool seen_new[kPhases] = {}, seen_old[kPhases] = {};
  for (int k = 0; k < 16; ++k) {
    Frame(7);  // a pinned still: update 7 in every frame
    seen_new[JitterIndex(true, 9000 + uint64_t(k), kPhases)] = true;
    seen_old[JitterIndex(false, 9000 + uint64_t(k), kPhases)] = true;
    sn::OnPresent();
  }
  int visited_new = 0, visited_old = 0;
  for (uint32_t i = 0; i < kPhases; ++i) {
    visited_new += seen_new[i] ? 1 : 0;
    visited_old += seen_old[i] ? 1 : 0;
  }
  g_check(visited_new == int(kPhases) && visited_old == 1,
          nc::Num("a pinned still walks %d of the upscaler's %u jitter phases in 16 frames (the scene's number alone stood on %d: one sub-pixel offset for ever)", visited_new, kPhases, visited_old));
  // a running scene and a frame outside a scene are as they were
  bool running = true;
  for (int64_t u = 40; u < 52; ++u) {
    Frame(u);
    running = running && JitterIndex(true, 1, kPhases) == JitterIndex(false, 1, kPhases) && JitterIndex(true, 1, kPhases) == uint32_t(u % int64_t(kPhases));
    sn::OnPresent();
  }
  g_check(running, "a running scene (a new update every frame): the jitter is the update number mod the phases, as before");
  sn::SetUpdate(-1);
  g_check(JitterIndex(true, 77, kPhases) == 77u % kPhases, "outside a scene the jitter follows the renderer's own count, as before");
  // the switch: mcla_gpu_noise_held false gives the scene's number alone back
  sn::Rearm();
  Frame(7);
  sn::OnPresent();
  Frame(7);
  sn::SetCountHeld(false);
  const bool off = JitterIndex(true, 1, kPhases) == 7u % kPhases;
  sn::SetCountHeld(true);
  const bool on = JitterIndex(true, 1, kPhases) == 8u % kPhases;
  sn::OnPresent();
  sn::SetUpdate(-1);
  g_check(off && on, "mcla_gpu_noise_held false: the jitter is the scene's number alone again; on, it counts the held frames");
}

// ---------------------------------------------------------------------------
// the numbers the effects' memory rests on, and the sources
// ---------------------------------------------------------------------------

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

void TestMemory() {
  // 1080p: the occlusion is traced at the picture's size, the contact image is the picture's; 4K: the occlusion at half size (1920 x 1080), the contact image keeps the picture's size (Plan::SplitContact)
  const struct {
    const char* name;
    uint32_t w, h;
  } cases[] = {{"1080p", 1920, 1080}, {"4K", 3840, 2160}};
  if (g_check.print) {
    for (const auto& c : cases) {
      const uint32_t shafts_w = std::min(c.w / 2, 960u), shafts_h = std::min(c.h / 2, 540u);
      const uint32_t cone_w = c.w >= 2560 ? c.w / 2 : c.w, cone_h = c.w >= 2560 ? c.h / 2 : c.h;
      std::printf("  %s: contact history %.1f MiB (+ the filter's own image %.1f MiB where it was inside the apply), shafts %.2f MiB, cone %.1f MiB: %.1f MiB in all\n", c.name,
                  Mib(nr::ContactHistoryBytes(c.w, c.h)), Mib(nr::ContactFilterImageBytes(c.w, c.h)), Mib(nr::ShaftsHistoryBytes(shafts_w, shafts_h)), Mib(nr::ConeHistoryBytes(cone_w, cone_h)),
                  Mib(nr::ContactHistoryBytes(c.w, c.h) + nr::ContactFilterImageBytes(c.w, c.h) + nr::ShaftsHistoryBytes(shafts_w, shafts_h) + nr::ConeHistoryBytes(cone_w, cone_h)));
    }
  }
  g_check(nr::ContactHistoryBytes(1920, 1080) == 2ull * 1920 * 1080 * 4 && nr::ShaftsHistoryBytes(960, 540) == 2ull * 960 * 540 * 2 && nr::ConeHistoryBytes(1920, 1080) == 2ull * 1920 * 1080 * 8,
          "the memory model: R16G16_FLOAT x2, R16_FLOAT x2 and R16G16B16A16_FLOAT x2 at the images' sizes");
  g_check(Mib(nr::ShaftsHistoryBytes(960, 540)) < 2.1, nc::Num("the shafts' history is %.2f MiB at most (the image is capped at 960 x 540)", Mib(nr::ShaftsHistoryBytes(960, 540))));
}

void TestSources(const std::string& dir) {
  const std::string ao = nc::ReadFile(dir + "/ao.cpp");
  const std::string ao_h = nc::ReadFile(dir + "/ao.h");
  const std::string temporal_cs = nc::ReadFile(dir + "/hlsl/ao_temporal_ps.hlsl");
  const std::string shafts = nc::ReadFile(dir + "/shafts.cpp");
  const std::string shafts_ps = nc::ReadFile(dir + "/hlsl/shafts_temporal_ps.hlsl");
  const std::string cone = nc::ReadFile(dir + "/car_rt_refl.cpp");
  const std::string denoise = nc::ReadFile(dir + "/hlsl/car_rt_refl_denoise_cs.hlsl");
  const std::string selfrefl = nc::ReadFile(dir + "/car_selfrefl.cpp");
  const std::string temporal = nc::ReadFile(dir + "/temporal.cpp");
  const std::string cmake = nc::ReadFile(dir + "/CMakeLists.txt");
  const std::string host_shaders = nc::ReadFile(dir + "/host_shaders.h");
  const std::string src = dir + "/..";
  const std::string settings = nc::ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = nc::ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = nc::ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string pages = nc::ReadFile(src + "/mc_engine/menus/debug_menu_pages.cpp");
  if (ao.empty() || shafts.empty() || cone.empty() || denoise.empty() || settings.empty() || cmake.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  const std::string decl = std::string("REX_") + "SETTING(bool, mcla_gpu_noise_resolve_no_upscaler, true,";
  g_check(nc::Has(settings, decl.c_str()) && nc::Has(settings, "inline auto& mcla_gpu_noise_resolve_no_upscaler = g_gpu.mcla_gpu_noise_resolve_no_upscaler;") && nc::Has(manifest, "\"mcla_gpu_noise_resolve_no_upscaler\": {") &&
              nc::Has(meta, "{\"mcla_gpu_noise_resolve_no_upscaler\", \"MCLA/GPU\"},") && nc::Has(pages, "NativeOnly(Bool(\"PM_RxDbgNoiseResolve\", \"mcla_gpu_noise_resolve_no_upscaler\""),
          "mcla_gpu_noise_resolve_no_upscaler: a live debug setting, on by default, in the settings header, its alias, the manifest, the meta list and the debug menu's FIXES page");
  // the contact march and the car's sun ray
  g_check(nc::Has(ao, "noise_resolve::Varies(plan.upscaler, plan.contact_history) ? base.slots2[1] : (base.slots2[1] & ao_stable::kBlueNoiseFlag)") &&
              nc::Has(ao, "in.sun_frame = noise_resolve::Varies(plan.upscaler, plan.contact_history) ? uint32_t(scene_noise::NoiseIndex(frame_) & 1023) : 0u;") && !nc::Has(ao, "plan.upscaler ? base.slots2[1]"),
          "ao.cpp: the contact march's and the car's sun ray's frame terms advance while an upscaler runs OR the contact history does (the old `plan.upscaler ? frame : 0` is gone)");
  g_check(nc::Has(ao, "plan.contact_history = contact && !upscaler_running && larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get() && larecomp::settings::mcla_gpu_ao_history.Get() >= 2 &&") &&
              nc::Has(ao_h, "bool ContactFilterInApply() const { return !contact_history && ao_size::ContactFilterInApply(contact.fast, SizeChoice()); }"),
          "the contact history runs only with no temporal upscaler, the switch on, the occlusion's history length 2 or more and no contact debug picture; the filter is a pass of its own then");
  g_check(nc::Has(ao, "uint32_t AmbientOcclusion::RecordContactHistory(") && nc::Has(ao, "plume::RenderPipeline* accumulate = Pipeline(stencil_ms_ ? kVTemporalMs : kVTemporal);") &&
              nc::Has(ao, "const uint32_t slot = RecordContactHistory(list, plan, base, contact_dn, contact_depth);") && nc::Has(ao, "constexpr uint32_t kContactHistoryFlag = 1u << 22;"),
          "the contact image's history is the occlusion's own temporal pass (the same pipeline, ao_temporal_ps) over the contact filter's result, in the flag mode of bit 22");
  g_check(nc::Has(temporal_cs, "bool AoContactHistory() { return (g_push.slots2.z & (1u << 22)) != 0u; }") && nc::Has(temporal_cs, "if (confidence < 1.0f && !AoContactHistory()) {") && nc::Has(temporal_cs, "static const float kClampMargin = 0.08f;") &&
              nc::Has(ao, "kHistoryFlag = 1u << 20;"),
          "the shader's flag mode skips the wide blur and keeps the occlusion's clamp margin (0.08), plane test and weight: the same ghost as the occlusion's");
  // the shafts
  g_check(nc::Has(shafts, "plan.history = !upscaler_running && larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get() && plan.view == 0;") && nc::Has(shafts, "const HostImage* resolved = plan.history ? RecordTemporal(list, plan) : nullptr;") &&
              nc::Has(shafts, "RecordAdd(list, *colour, plan, resolved ? *resolved : shaft_)") && nc::Has(shafts, "push.sun[2] = keep ? noise_resolve::ShaftsHistoryWeight(hist_frames_) : 0.0f;"),
          "shafts.cpp: the history runs with no upscaler and the switch on (not in a debug view), between the blur and the add pass, and the add pass reads its result");
  g_check(nc::Has(shafts_ps, "const float2 uv = (float2(pixel) + 0.5f) / size + g_push.tint.xy;") && nc::Has(shafts_ps, "const float held = clamp(history, lo - g_push.sun.w, hi + g_push.sun.w);") &&
              nc::Has(shafts_ps, "return float4(lerp(current, held, kept), 0.0f, 0.0f, 0.0f);") && nc::Has(cmake, "larecomp_gpu_host_shader(shafts_temporal_ps ps_6_0)") &&
              nc::Has(host_shaders, "default: return MCLA_GPU_HOST_SHADER_BLOB(shafts_temporal_ps);") && nc::Has(host_shaders, "#include \"shafts_temporal_ps.hlsl.dxil.h\"") && nc::Has(host_shaders, "#include \"shafts_temporal_ps.hlsl.spirv.h\""),
          "shafts_temporal_ps reads the history at the sun's shift, clamps it to the 3x3 range and blends at the weight; registered in CMake and host_shaders.h");
  // REVIEW F1: EnsureImages clears the history's counters when it makes an image again (or gives it back), before RecordTemporal decides, and the gate is the one TestShaftsHistoryGate's twin transcribes;
  // the pass never reads an image that has not been drawn into
  const size_t ensure_at = shafts.find("bool SunShafts::EnsureImages(");
  const size_t invalidate_at = shafts.find("if (hist_[0].texture.get() != hist_before[0] || hist_[1].texture.get() != hist_before[1]) {\n    hist_valid_ = false;\n    hist_frames_ = 0;\n  }");
  const size_t record_at = shafts.find("const HostImage* SunShafts::RecordTemporal(");
  g_check(nc::Has(shafts, "const plume::RenderTexture* hist_before[2] = {hist_[0].texture.get(), hist_[1].texture.get()};") && ensure_at != std::string::npos && invalidate_at != std::string::npos &&
              record_at != std::string::npos && ensure_at < invalidate_at && invalidate_at < record_at &&
              nc::Has(shafts, "const bool keep = hist_valid_ && hist_frame_ + 1 == frame_ && hist_pause_ == pause && std::isfinite(shift_u) && std::isfinite(shift_v) &&\n"
                              "                    noise_resolve::ShaftsKeepsHistory(shift_u, shift_v, aspect) && history_in.layout != plume::RenderTextureLayout::UNKNOWN;") &&
              nc::Has(shafts, "push.sun[2] = keep ? noise_resolve::ShaftsHistoryWeight(hist_frames_) : 0.0f;") && nc::Has(shafts, "hist_frames_ = keep ? std::min(hist_frames_ + 1u, 64u) : 1u;"),
          "shafts.cpp (review F1): EnsureImages compares the history images' textures before and after making them and clears the history's counters; RecordTemporal's gate is the twin's and never reads an image that has no layout yet");
  // the cone
  g_check(nc::Has(cone, "const bool resolve_history = denoise_passes > 0 && !info.upscaler && larecomp::settings::mcla_gpu_noise_resolve_no_upscaler.Get() && EnsureStableImages(history.width, history.height);") &&
              nc::Has(cone, "const uint32_t pattern_phase = noise_resolve::Varies(info.upscaler, resolve_history) ? info.pattern_phase : 0u;") && nc::Has(selfrefl, "info.pattern_phase = uint32_t(scene_noise::NoiseIndex(frame_) & 7u);"),
          "the cone: the pattern's phase advances while an upscaler runs or the denoise's own history does; car_selfrefl.cpp hands the phase over every frame and car_rt_refl.cpp says where it is used");
  g_check(nc::Has(cone, "DispatchDenoise(list, denoise, history.slot, finished, stable_set_[out], 2, history.width, history.height, float(info.tan_y), length, tan_half, keep, stable_[stable_index_].slot);") &&
              nc::Has(denoise, "if (g_push.slots.w == 2u)") && nc::Has(denoise, "static const float kTemporalMargin = 0.05;") && nc::Has(denoise, "const float4 last = g_textures[uint(g_push.image.w + 0.5)].Load(int3(centre, 0));") &&
              nc::Has(cone, "const HostImage& result = result_in_stable_ ? stable_[stable_index_] : (result_in_scratch_ ? scratch_ : image_);"),
          "the denoise shader's third pass averages the finished image with the last result (slot in image.w), clamped to its 3x3 neighbourhood; the paint reads the history's image");
  g_check(std::fabs(double(nr::kConeClampMargin) - 0.05) < 1e-9 && std::fabs(double(nr::kShaftsClampMargin) - 0.02) < 1e-9 && std::fabs(double(nr::kConeStaleFrom) - 0.02) < 1e-9 && std::fabs(double(nr::kConeStaleTo) - 0.05) < 1e-9 &&
              std::fabs(double(nr::kShaftsStaleFrom) - 0.02) < 1e-9 && std::fabs(double(nr::kShaftsStaleTo) - 0.05) < 1e-9 && nc::Has(denoise, "static const float kStaleFrom = 0.02;") && nc::Has(denoise, "static const float kStaleTo = 0.05;") &&
              nc::Has(denoise, "(1.0 - smoothstep(kStaleFrom, kStaleTo, stale))") && nc::Has(shafts_ps, "smoothstep(g_push.tint.z, g_push.tint.w, abs(history - current))") &&
              nc::Has(shafts, "push.tint[2] = noise_resolve::kShaftsStaleFrom;") && nc::Has(shafts, "push.tint[3] = noise_resolve::kShaftsStaleTo;"),
          "the shaders' margins and stale ranges are noise_resolve.h's (clamp 0.05, stale 0.02 to 0.05 for the cone; clamp 0.02, stale 0.02 to 0.05 for the shafts)");
  // the jitter
  g_check(nc::Has(temporal, "scene_noise::Active() ? scene_noise::NoiseIndex(jitter_index_) % frame_.phase_count : jitter_index_ % frame_.phase_count") && !nc::Has(temporal, "scene_noise::Index(jitter_index_)"),
          "temporal.cpp: the upscaler's jitter takes the noise index (the scene's number plus the frames it has been held): a pinned still walks the whole sequence");
  // what is NOT changed: the shadow filter keeps its fixed turn (shadow_filter_resolve_test.cpp has the numbers)
  const std::string sf = nc::ReadFile(dir + "/shadow_filter.h");
  g_check(nc::Has(sf, "constants.noise_frame = temporal_frame_valid ? static_cast<float>(frame & 1023u) : 0.0f;"), "the shadow filter's disk keeps its fixed turn without an upscaler (shadow_filter_resolve_test.cpp: why)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = nc::DirOf(__FILE__);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--print") {
      g_check.print = true;
    } else {
      dir = a;
    }
  }
  TestVariation();
  TestContact("contact march (thin occluder: a binary stipple)", MarchVisibility, MarchTruth);
  TestContact("contact march (soft edge)", SoftVisibility, SoftTruth);
  TestContact("traced sun ray", SunVisibility, SunTruth);
  TestContactGhost();
  TestShafts();
  TestShaftsHistoryGate();
  TestCone();
  TestJitter();
  TestMemory();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
