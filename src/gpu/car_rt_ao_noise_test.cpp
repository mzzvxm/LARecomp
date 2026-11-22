// Check of the ray traced car occlusion's and sun ray's noise (hlsl/car_rt_ao_noise.hlsli, car_rt_ao_cs.hlsl, ao.cpp's noise code for it; NOISE-AUDIT, perf\NOISE-AUDIT.md). A program of its own: no renderer,
// no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_car_rt_ao_noise_test, then run src/gpu/larecomp_car_rt_ao_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// It compiles hlsl/car_rt_ao_noise.hlsli as C++ (hlsl_shim.h) and checks, with the shared measures of noise_resolve_check.h:
//   the occlusion's numbers: uniform, change at every pixel every frame (the 13 frame lattice of ao_stable.h), every pixel takes the same step so nothing slides, no lattice in the error the history of 12
//   frames leaves (the first version's gradient noise: a lattice), the same numbers as the screen-space occlusion's (hlsl/ao_common.hlsli AoSpatialNoise + AoNoiseShift: the tile's two channels plus the shift)
//   the sun ray's numbers: with a temporal upscaler they step along R2 every frame, without one they are the fixed blue pattern; uncorrelated with the occlusion's; a still frame's pattern has no
//   lattice after the contact filter's 3 x 3 blur, where the first version's gradient noise had one
//   the sources: the shader includes the noise file and no longer carries gradient noise, its flag is ao_stable.h's, ao.cpp hands the blue flag and the scene's frame number to the traced pass

#include <cstdint>
#include <cstdio>
#include <string>

#include "ao_stable.h"
#include "hlsl_shim.h"
#include "noise_resolve_check.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/car_rt_ao_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;
namespace as = mcla::gpu::ao_stable;

namespace {

nc::Checks g_check;

hs::float2 ShiftOf(uint32_t frame, bool blue) {
  const as::Shift s = as::DecodeShift(as::NoiseCode(frame, true, blue));
  return hs::float2(s.x, s.y);
}

std::pair<float, float> Occlusion(int x, int y, uint32_t frame, bool blue) {
  const hs::float2 u = hs::CarRtOcclusionNoise(x, y, blue, ShiftOf(frame, blue));
  return {u.x, u.y};
}
std::pair<float, float> Sun(int x, int y, uint32_t sun_frame, bool blue) {
  const hs::float2 u = hs::CarRtSunNoise(x, y, blue, sun_frame);
  return {u.x, u.y};
}

// What the occlusion makes of its two numbers: the pixel's open share against the slice's rotation and the step offsets (smooth and periodic).
double Response(float u1, float u2) { return std::cos(6.2831853 * double(u1) + 0.7) + 0.6 * std::cos(6.2831853 * double(u2)); }

void TestOcclusion() {
  const auto blue = [](int x, int y, uint32_t f) { return Occlusion(x, y, f, true); };
  const auto legacy = [](int x, int y, uint32_t f) { return Occlusion(x, y, f, false); };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(blue, channel);
    if (g_check.print) {
      nc::Print(channel == 0 ? "occlusion, number 1" : "occlusion, number 2", p);
    }
    g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
            nc::Num("occlusion number %d: uniform (mean %.3f), changes at every pixel every frame (%.3f), widest gap in 16 frames %.3f, the same step at every pixel (%.6f): nothing slides", channel + 1, p.mean,
                    p.changed, p.max_gap, p.step_spread));
  }
  // the same numbers as the screen-space occlusion's: the tile's channels at the pixel plus the shift (ao_common.hlsli AoSpatialNoise, AoNoiseShift)
  bool same = true;
  for (int y = 0; y < 40 && same; y += 7) {
    for (int x = 0; x < 40 && same; x += 5) {
      const hs::float2 shift = ShiftOf(9, true), t = hs::BlueNoiseTile2At(x, y);
      const std::pair<float, float> u = blue(x, y, 9);
      same = u.first == hs::frac(t.x + shift.x) && u.second == hs::frac(t.y + shift.y);
    }
  }
  g_check(same, "the traced occlusion's numbers are the screen-space occlusion's: the tile's two channels plus the frame's shift, modulo 1");
  // the history of 12 frames (weight 11/12) leaves no lattice, and the first version's noise does
  const nc::Resolved rl = nc::Resolve(legacy, Response, 100, 128, 48, 11.0 / 12.0, false);
  const nc::Resolved rb = nc::Resolve(blue, Response, 100, 128, 48, 11.0 / 12.0, false);
  if (g_check.print) {
    nc::Print("occlusion, first version", rl);
    nc::Print("occlusion, blue", rb);
  }
  g_check(nc::IsLattice(rl), nc::Num("the first version's error after the history is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue noise's has no frequency that stands out (%.4f) and no direction (%.2f, under 3), and it stays where it is (best match with the next frame at (%d, %d))",
                                                             rb.shape.peak_share, rb.shape.wedge_ratio, rb.best_dx, rb.best_dy));
  g_check(nc::Crawls(rl), nc::Num("(the first version's error moves: the next frame matches this one moved by (%d, %d) at %.2f)", rl.best_dx, rl.best_dy, rl.best_corr));
  g_check(rb.rms < 1.3 * rl.rms, nc::Num("... at an error within 1.3 times the first version's (%.4f against %.4f)", rb.rms, rl.rms));
}

void TestSun() {
  // a temporal upscaler averages the frames: the numbers step along R2 every frame; without one the frame is 0 and the pattern is the tile moved by (17, 5), the same every frame
  const auto stepping = [](int x, int y, uint32_t f) { return Sun(x, y, f, true); };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(stepping, channel);
    if (g_check.print) {
      nc::Print(channel == 0 ? "sun ray, number 1" : "sun ray, number 2", p);
    }
    g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01,
            nc::Num("sun ray number %d with an upscaler: uniform, changes every frame at every pixel (%.3f), no gap over %.3f in 16 frames, the same step at every pixel (%.6f)", channel + 1, p.changed, p.max_gap,
                    p.step_spread));
  }
  bool fixed = true;
  for (int y = 0; y < 40 && fixed; y += 3) {
    for (int x = 0; x < 40 && fixed; x += 3) {
      const hs::float2 t = hs::BlueNoiseTile2At(x + 17, y + 5);
      const std::pair<float, float> u = Sun(x, y, 0, true);
      fixed = u.first == hs::frac(t.x + 0.0f) && u.second == hs::frac(t.y + 0.0f);
    }
  }
  g_check(fixed, "without an upscaler (frame 0) the sun ray's numbers are the blue tile at the pixel moved by (17, 5): a fixed pattern, as the contact march's");
  // uncorrelated with the occlusion's numbers of the same pixel
  double sxy = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
  int m = 0;
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const double a = Occlusion(x, y, 4, true).first, b = Sun(x, y, 4, true).first;
      sx += a;
      sy += b;
      sxx += a * a;
      syy += b * b;
      sxy += a * b;
      ++m;
    }
  }
  const double c = (sxy / m - sx / m * sy / m) / std::sqrt((sxx / m - sx / m * sx / m) * (syy / m - sy / m * sy / m));
  g_check(std::fabs(c) < 0.1, nc::Num("the sun ray's first number is uncorrelated with the occlusion's (%.3f)", c));
  // a still picture's pattern after the contact filter's 3 x 3 blur, for the response the march has: a shadow edge across the sun's disc (a step of the radius and the angle together). The first version's
  // gradient noise leaves its low spatial frequencies there, the tile has none.
  const auto edge = [](float u1, float u2) { return std::sqrt(double(u1)) * std::cos(6.2831853 * double(u2)) > 0.3 ? 1.0 : 0.0; };
  const auto fixed_blue = [](int x, int y, uint32_t) { return Sun(x, y, 0, true); };
  const auto fixed_legacy = [](int x, int y, uint32_t) { return Sun(x, y, 0, false); };
  const nc::Resolved rb = nc::Resolve(fixed_blue, edge, 100, 128, 1, 0.9, true);
  const nc::Resolved rl = nc::Resolve(fixed_legacy, edge, 100, 128, 1, 0.9, true);
  if (g_check.print) {
    nc::Print("fixed sun pattern, first version", rl);
    nc::Print("fixed sun pattern, blue", rb);
  }
  g_check(nc::IsLattice(rl) && nc::NoLattice(rb, 3.0) && rb.rms < 1.3 * rl.rms,
          nc::Num("a fixed sun pattern after the 3x3 blur, edge response: the first version's has a lattice (%.3f of its energy in one frequency, a direction %.1f times the mean), the blue tile's none (%.4f, %.1f), at an rms of "
                  "%.3f against %.3f",
                  rl.shape.peak_share, rl.shape.wedge_ratio, rb.shape.peak_share, rb.shape.wedge_ratio, rb.rms, rl.rms));
}

void TestSources(const std::string& dir) {
  const std::string cs = nc::ReadFile(dir + "/hlsl/car_rt_ao_cs.hlsl");
  const std::string noise = nc::ReadFile(dir + "/hlsl/car_rt_ao_noise.hlsli");
  const std::string ao = nc::ReadFile(dir + "/ao.cpp");
  if (cs.empty() || noise.empty() || ao.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  g_check(nc::Has(cs, "#include \"car_rt_ao_noise.hlsli\"") && nc::Has(cs, "CarRtOcclusionNoise(") && nc::Has(cs, "CarRtSunNoise(") && !nc::Has(cs, "52.9829189") && !nc::Has(cs, "float Ign("),
          "car_rt_ao_cs.hlsl takes both noises from car_rt_ao_noise.hlsli and carries no gradient noise of its own");
  g_check(nc::Has(cs, "static const uint kBlueNoiseFlag = 1u << 29;") && as::kBlueNoiseFlag == (1u << 29), "its blue noise flag is bit 29, ao_stable.h's");
  g_check(nc::Has(cs, "float(m), float((m * 8u) % 13u)) * (1.0 / 13.0)") && as::kLatticePeriod == 13 && as::kLatticeStepY == 8, "its frame shift is the occlusion's 13 frame lattice");
  g_check(nc::Has(ao, "in.noise_code = ao_stable::NoiseCode(scene_noise::NoiseIndex(frame_), larecomp::settings::mcla_gpu_ao_stable.Get(), larecomp::settings::mcla_gpu_ao_blue_noise.Get());") &&
              nc::Has(ao, "in.sun_frame = plan.upscaler ? uint32_t(scene_noise::NoiseIndex(frame_) & 1023) : 0u;"),
          "ao.cpp gives the traced pass the blue flag (mcla_gpu_ao_blue_noise) and the scene's noise index (NoiseIndex: its number plus the frames a held scene has stood on it), like the main pass");
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
  TestOcclusion();
  TestSun();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
