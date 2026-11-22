// Check of the ray traced car reflection's cone turn (hlsl/car_rt_refl_cs.hlsl ConeRotation, its twin car_rt_refl_rules.h ConeRotation, car_rt_refl.cpp's flag; NOISE-AUDIT, perf\NOISE-AUDIT.md). A program of
// its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_car_refl_noise_test, then run src/gpu/larecomp_car_refl_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// The cone's four rays are turned about the mirror direction by an angle that differs from texel to texel: the first version took it from interleaved gradient noise, a lattice, plus a golden step per phase
// (0 where no temporal upscaler runs, 0 .. 7 by the frame where one does). The denoise (two 3x3 a-trous passes) and the upscaler's history leave a lattice as thin diagonal lines on a panel. Now the
// texel's part is the blue noise tile (the golden step is unchanged: eight steps of 0.618 leave no gap over 0.15 of the circle, which is what a cycle of eight phases wants, and the same for every
// texel). It checks, with the shared measures of noise_resolve_check.h:
//   the turn: uniform, a new value at every texel, changes at every texel every frame with a step that is the same for every texel (nothing slides), no gap over 0.25 in 16 frames
//   the error of the cone's coverage (a glossy edge across the disc) after an upscaler's average over frames: the first version's has a lattice, the blue noise's has none and stays where it is
//   a still frame (phase 0, no upscaler) after the denoise's 3x3 blur: no lattice
//   the sources: the flag bit (17) does not overlap the phase, the rays or the stats bit; the shader's text is the twin's; the host sets the flag from mcla_gpu_noise_blue

#include <cstdint>
#include <cstdio>
#include <string>

#include "car_rt_hit_rules.h"
#include "car_rt_refl_rules.h"
#include "noise_resolve_check.h"

namespace nc = noise_check;
namespace rt = mcla::gpu::car_rt_refl;
namespace hit = mcla::gpu::car_rt_hit;

namespace {

nc::Checks g_check;

std::pair<float, float> Blue(int x, int y, uint32_t frame) { return {rt::ConeRotation(uint32_t(x + 100), uint32_t(y), frame & 7u), 0.0f}; }
std::pair<float, float> Legacy(int x, int y, uint32_t frame) { return {rt::ConeRotationLegacy(uint32_t(x + 100), uint32_t(y), frame & 7u), 0.0f}; }
std::pair<float, float> BlueStill(int x, int y, uint32_t) { return {rt::ConeRotation(uint32_t(x + 100), uint32_t(y), 0u), 0.0f}; }
std::pair<float, float> LegacyStill(int x, int y, uint32_t) { return {rt::ConeRotationLegacy(uint32_t(x + 100), uint32_t(y), 0u), 0.0f}; }

// What a texel's glossy edge makes of its turn: the four rays' directions (ConeDisc) against a straight edge across the disc, the share that hit (a function of the turn alone, periodic in it).
double Coverage(float turns, float) {
  int hits = 0;
  for (int ray = 0; ray < 4; ++ray) {
    const float angle = 6.283185307179586f * (turns + 0.25f * float(ray));
    const float r = rt::ConeRadius(ray);
    hits += (r * std::cos(angle) > 0.18f) ? 1 : 0;
  }
  return double(hits) / 4.0;
}

void TestTurn() {
  const nc::PerFrame p = nc::PerFrameOf(Blue, 0);
  if (g_check.print) {
    nc::Print("cone turn, blue, by frame", p);
  }
  g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
          nc::Num("with an upscaler the turn is uniform (mean %.3f, variance %.4f), changes at every texel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes the same step at every texel (%.6f)", p.mean,
                  p.var, p.changed, p.max_gap, p.step_spread));
  // a new value at (almost) every texel of a still picture, in [0, 1)
  int distinct = 0;
  float previous = -1.0f;
  bool inside = true;
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      const float t = rt::ConeRotation(x, y, 0);
      inside = inside && t >= 0.0f && t < 1.0f;
      distinct += t != previous ? 1 : 0;
      previous = t;
    }
  }
  g_check(inside && distinct > 3900, nc::Num("a still picture's turns are in [0, 1) and new at almost every texel (%d of 4096)", distinct));
  // the first version stays available as the A/B
  g_check(rt::ConeRotationLegacy(31, 17, 3) == rt::ConeRotationLegacy(31, 17, 3) && rt::ConeRotationLegacy(31, 17, 0) != rt::ConeRotation(31, 17, 0), "the first version's turn is kept for mcla_gpu_noise_blue false");
}

void TestResolve() {
  // an upscaler's history averages the eight phases: the pattern left is static, lattice-free for the blue tile, a lattice for the gradient noise
  const nc::Resolved rl = nc::Resolve(Legacy, Coverage, 104, 128, 40, 0.875, false);
  const nc::Resolved rb = nc::Resolve(Blue, Coverage, 104, 128, 40, 0.875, false);
  if (g_check.print) {
    nc::Print("with an upscaler, first version", rl);
    nc::Print("with an upscaler, blue", rb);
  }
  g_check(nc::IsLattice(rl), nc::Num("the first version's error after the upscaler's average is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue tile's has none (%.4f in one frequency, a direction %.2f, under 3) and stays where it is", rb.shape.peak_share, rb.shape.wedge_ratio));
  // a still frame: nothing averages, the denoise's 3x3 passes are all that is left
  const nc::Resolved sl = nc::Resolve(LegacyStill, Coverage, 100, 128, 1, 0.9, true);
  const nc::Resolved sb = nc::Resolve(BlueStill, Coverage, 100, 128, 1, 0.9, true);
  if (g_check.print) {
    nc::Print("still, 3x3 blur, first version", sl);
    nc::Print("still, 3x3 blur, blue", sb);
  }
  g_check(nc::NoLattice(sb, 3.0) && sb.rms < 1.5 * sl.rms,
          nc::Num("a still frame after the 3x3 blur: the blue tile's pattern has no lattice (%.4f in one frequency, a direction %.2f) at an rms of %.3f against the first version's %.3f (%.4f, %.2f)", sb.shape.peak_share,
                  sb.shape.wedge_ratio, sb.rms, sl.rms, sl.shape.peak_share, sl.shape.wedge_ratio));
}

void TestSources(const std::string& dir) {
  const std::string cs = nc::ReadFile(dir + "/hlsl/car_rt_refl_cs.hlsl");
  const std::string host = nc::ReadFile(dir + "/car_rt_refl.cpp");
  if (cs.empty() || host.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  const uint32_t flag = rt::kConeBlueFlag;
  g_check(flag == (1u << 17) && nc::Has(cs, "static const uint kConeBlueFlag = 131072u;") && (flag & (7u | (7u << 8) | hit::census::kStatsFlag)) == 0u &&
              (hit::PackPhaseRays(7u, 4u) & flag) == 0u,
          "the cone's blue flag is slots.w bit 17: the shader's constant is the header's and no phase, rays or stats bit overlaps it");
  g_check(nc::Has(cs, "#include \"blue_noise.hlsli\"") && nc::Has(cs, "BlueNoiseTileAt(int(texel.x), int(texel.y), 0u)") && nc::Has(cs, "return frac(noise + 0.6180339887 * float(phase));") &&
              nc::Has(cs, "ConeRotation(texel, g_push.slots.w & 7u, (g_push.slots.w & kConeBlueFlag) != 0u)"),
          "the shader's turn is the tile at the texel plus the golden step per phase, as the twin's, with the flag from slots.w");
  g_check(nc::Has(host, "(larecomp::settings::mcla_gpu_noise_blue.Get() ? car_rt_refl::kConeBlueFlag : 0u)"), "car_rt_refl.cpp sets the flag from mcla_gpu_noise_blue (live: read at each frame's trace)");
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
  TestTurn();
  TestResolve();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
