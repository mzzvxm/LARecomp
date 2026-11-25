// Check of the sun shafts' ray-start noise (hlsl/shafts_noise.hlsli, shafts_blur_ps.hlsl, shafts.cpp's flag; NOISE-AUDIT, perf\NOISE-AUDIT.md). A program of its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_shafts_noise_test, then run src/gpu/larecomp_shafts_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// The ray of every texel of the shafts' half size image starts a fraction of one step along its way, so that the steps do not show as bands. The first version's fraction was interleaved gradient noise
// (a lattice), plus a golden step per frame while a temporal upscaler runs and none otherwise; now it is the blue noise tile, stepped along the R2 sequence. It compiles the .hlsli as C++
// (hlsl_shim.h) and checks, with the shared measures of noise_resolve_check.h:
//   with an upscaler (shift = the frame, 0 to 1023): uniform, changes at every texel every frame, no gap over 0.25 in 16 frames, one step for every texel (nothing slides), and the error of a brightness
//   that is linear in the start (a sky edge along the ray) after the upscaler's average over frames has no lattice where the first version's had one
//   without one (shift 0): the fixed blue pattern is the tile itself, and after the 3 x 3 blur (the bilinear read of the add pass is of that order) it keeps a thousandth of the first version's low
//   frequency energy (the first version's lattice is the pattern the owner saw as thin diagonal lines)
//   the sources: the shader includes the noise file and carries no gradient noise, the flag is slots.z bit 0 and the host sets it from mcla_gpu_noise_blue

#include <cstdint>
#include <cstdio>
#include <string>

#include "hlsl_shim.h"
#include "noise_resolve_check.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/shafts_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;

namespace {

nc::Checks g_check;

float Start(int x, int y, uint32_t shift, bool blue) { return hs::ShaftsStartNoise(x, y, hs::float2(float(x) + 0.5f, float(y) + 0.5f), blue, shift & 1023u); }

// What the ray makes of its start: a brightness that is linear in it (a sky edge along the ray changes the sum in proportion to where the first step lands), the mean taken out.
double Linear(float u, float) { return double(u) - 0.5; }

void TestUpscaler() {
  const auto blue = [](int x, int y, uint32_t f) { return std::pair<float, float>(Start(x, y, f, true), 0.0f); };
  const auto legacy = [](int x, int y, uint32_t f) { return std::pair<float, float>(Start(x, y, f, false), 0.0f); };
  const nc::PerFrame p = nc::PerFrameOf(blue, 0);
  if (g_check.print) {
    nc::Print("ray start, blue, by frame", p);
  }
  g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
          nc::Num("with an upscaler the start is uniform (mean %.3f, variance %.4f), changes at every texel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes one step at every texel (%.6f)", p.mean, p.var,
                  p.changed, p.max_gap, p.step_spread));
  const nc::Resolved rl = nc::Resolve(legacy, Linear, 100, 128, 40, 0.875, false);
  const nc::Resolved rb = nc::Resolve(blue, Linear, 100, 128, 40, 0.875, false);
  if (g_check.print) {
    nc::Print("after the upscaler's average, first version", rl);
    nc::Print("after the upscaler's average, blue", rb);
  }
  g_check(nc::IsLattice(rl), nc::Num("the first version's error after the upscaler's average is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue noise's has none (%.4f in one frequency, a direction %.2f, under 3) and stays where it is", rb.shape.peak_share, rb.shape.wedge_ratio));
}

void TestStill() {
  // without an upscaler the shift is 0: the tile itself
  bool tile = true;
  for (int y = 0; y < 40 && tile; y += 3) {
    for (int x = 0; x < 40 && tile; x += 3) {
      tile = Start(x, y, 0, true) == hs::BlueNoiseTileAt(x, y, 0u);
    }
  }
  g_check(tile, "without an upscaler (shift 0) the start is the blue tile at the texel: a fixed pattern");
  const auto blue = [](int x, int y, uint32_t) { return std::pair<float, float>(Start(x, y, 0, true), 0.0f); };
  const auto legacy = [](int x, int y, uint32_t) { return std::pair<float, float>(Start(x, y, 0, false), 0.0f); };
  const nc::Resolved rb = nc::Resolve(blue, Linear, 100, 128, 1, 0.9, true);
  const nc::Resolved rl = nc::Resolve(legacy, Linear, 100, 128, 1, 0.9, true);
  if (g_check.print) {
    nc::Print("still, 3x3 blur, first version", rl);
    nc::Print("still, 3x3 blur, blue", rb);
  }
  g_check(rb.shape.low_energy < 0.1 * rl.shape.low_energy,
          nc::Num("a fixed pattern after the 3x3 blur: the blue tile keeps %.2f%% of the first version's low frequency energy (under 10%%; %.2g against %.2g)", 100.0 * rb.shape.low_energy / rl.shape.low_energy,
                  rb.shape.low_energy, rl.shape.low_energy));
}

void TestSources(const std::string& dir) {
  const std::string blur = nc::ReadFile(dir + "/hlsl/shafts_blur_ps.hlsl");
  const std::string noise = nc::ReadFile(dir + "/hlsl/shafts_noise.hlsli");
  const std::string host = nc::ReadFile(dir + "/shafts.cpp");
  if (blur.empty() || noise.empty() || host.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  g_check(nc::Has(blur, "#include \"shafts_noise.hlsli\"") && nc::Has(blur, "ShaftsStartNoise(int(position.x), int(position.y), position.xy, (g_push.slots.z & kBlueNoiseFlag) != 0u, uint(g_push.tint.w))") &&
              nc::Has(blur, "static const uint kBlueNoiseFlag = 1u;") && !nc::Has(blur, "52.9829189"),
          "shafts_blur_ps.hlsl takes the start from shafts_noise.hlsli with the flag of slots.z bit 0 and carries no gradient noise of its own");
  g_check(nc::Has(host, "push.slots[2] = larecomp::settings::mcla_gpu_noise_blue.Get() ? 1u : 0u;") && nc::Has(host, "plan.noise_shift = float(noise_resolve::FrameTerm(upscaler_running, plan.history, scene_noise::NoiseIndex(frame_), 1023u));"),
          "shafts.cpp sets the flag from mcla_gpu_noise_blue (live) and shifts the noise by the scene's noise index (NoiseIndex: a held scene keeps counting) while an upscaler runs or the blurred image's own history does (NOISE-RESOLVE)");
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
  TestUpscaler();
  TestStill();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
