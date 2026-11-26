// Check of the ray traced city sun shadows' disc noise (hlsl/city_rt_shadow_noise.hlsli, city_rt_shadow_cs.hlsl, ao.cpp's noise word for it; NOISE-AUDIT, perf\NOISE-AUDIT.md). A program of its own: no
// renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_city_rt_shadow_noise_test, then run src/gpu/larecomp_city_rt_shadow_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// Each pixel's sun rays are spread over the sun's disc by two numbers. They were interleaved gradient noise plus the R2 shift of the frame (a lattice), a fixed one while no temporal upscaler averages the
// frames; now the blue noise tile at the pixel stepped along the R2 sequence by the frame. It compiles the .hlsli as C++ (hlsl_shim.h) and checks, with the shared measures of noise_resolve_check.h:
//   with an upscaler (frame in the low 10 bits of the word): uniform, changes at every pixel every frame, no gap over 0.25 in 16 frames, one step at every pixel; the error of a sun edge's lit share after
//   the average over frames has no lattice, the first version's has
//   without one (frame 0): the fixed pattern is the tile; after the contact filter's 3 x 3 blur it has no lattice where the first version's had one
//   the sources: the shader includes the noise file and carries no gradient noise, the word's flag is ao_stable.h's bit 29 and ao.cpp sets it (mcla_gpu_ao_blue_noise) with the scene's frame number

#include <cstdint>
#include <cstdio>
#include <string>

#include "ao_stable.h"
#include "hlsl_shim.h"
#include "noise_resolve_check.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/city_rt_shadow_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;
namespace as = mcla::gpu::ao_stable;

namespace {

nc::Checks g_check;

std::pair<float, float> Noise(int x, int y, uint32_t frame, bool blue) {
  const hs::float2 u = hs::CityRtShadowNoise(x, y, (frame & 1023u) | (blue ? as::kBlueNoiseFlag : 0u));
  return {u.x, u.y};
}

// A shadow edge across the sun's disc: lit where the ray's offset (radius from the first number, angle from the second) is past the edge.
double Edge(float u1, float u2) { return std::sqrt(double(u1)) * std::cos(6.2831853 * double(u2)) > 0.3 ? 1.0 : 0.0; }

void TestUpscaler() {
  const auto blue = [](int x, int y, uint32_t f) { return Noise(x, y, f, true); };
  const auto legacy = [](int x, int y, uint32_t f) { return Noise(x, y, f, false); };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(blue, channel);
    if (g_check.print) {
      nc::Print(channel == 0 ? "disc number 1, blue" : "disc number 2, blue", p);
    }
    g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
            nc::Num("number %d with an upscaler: uniform (mean %.3f), changes at every pixel every frame (%.3f), no gap over %.3f in 16 frames, one step at every pixel (%.6f)", channel + 1, p.mean, p.changed, p.max_gap,
                    p.step_spread));
  }
  const nc::Resolved rl = nc::Resolve(legacy, Edge, 100, 128, 40, 0.9, false);
  const nc::Resolved rb = nc::Resolve(blue, Edge, 100, 128, 40, 0.9, false);
  if (g_check.print) {
    nc::Print("sun edge after the average, first version", rl);
    nc::Print("sun edge after the average, blue", rb);
  }
  g_check(nc::IsLattice(rl), nc::Num("the first version's error after the average is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue noise's has none (%.4f in one frequency, a direction %.2f, under 3) and stays where it is", rb.shape.peak_share, rb.shape.wedge_ratio));
}

void TestStill() {
  bool tile = true;
  for (int y = 0; y < 40 && tile; y += 3) {
    for (int x = 0; x < 40 && tile; x += 3) {
      const hs::float2 t = hs::BlueNoiseTile2At(x, y);
      const std::pair<float, float> u = Noise(x, y, 0, true);
      tile = u.first == t.x && u.second == t.y;
    }
  }
  g_check(tile, "without an upscaler (frame 0) the numbers are the blue tile's two channels at the pixel: a fixed pattern");
  const auto blue = [](int x, int y, uint32_t) { return Noise(x, y, 0, true); };
  const auto legacy = [](int x, int y, uint32_t) { return Noise(x, y, 0, false); };
  const nc::Resolved rb = nc::Resolve(blue, Edge, 100, 128, 1, 0.9, true);
  const nc::Resolved rl = nc::Resolve(legacy, Edge, 100, 128, 1, 0.9, true);
  if (g_check.print) {
    nc::Print("still sun pattern, 3x3 blur, first version", rl);
    nc::Print("still sun pattern, 3x3 blur, blue", rb);
  }
  g_check(nc::IsLattice(rl) && nc::NoLattice(rb, 3.0) && rb.rms < 1.3 * rl.rms,
          nc::Num("a fixed pattern after the contact filter's 3x3 blur: the first version's has a lattice (%.3f in one frequency, a direction %.1f), the blue tile's none (%.4f, %.1f), at an rms of %.3f against %.3f",
                  rl.shape.peak_share, rl.shape.wedge_ratio, rb.shape.peak_share, rb.shape.wedge_ratio, rb.rms, rl.rms));
}

void TestSources(const std::string& dir) {
  const std::string cs = nc::ReadFile(dir + "/hlsl/city_rt_shadow_cs.hlsl");
  const std::string noise = nc::ReadFile(dir + "/hlsl/city_rt_shadow_noise.hlsli");
  const std::string ao = nc::ReadFile(dir + "/ao.cpp");
  if (cs.empty() || noise.empty() || ao.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  g_check(nc::Has(cs, "#include \"city_rt_shadow_noise.hlsli\"") && nc::Has(cs, "CityRtShadowNoise(int(pixel.x), int(pixel.y), g_push.noise.x)") && !nc::Has(cs, "52.9829189") && !nc::Has(cs, "float Ign("),
          "city_rt_shadow_cs.hlsl takes the disc noise from city_rt_shadow_noise.hlsli and carries no gradient noise of its own");
  g_check(nc::Has(noise, "static const uint kCityNoiseBlueFlag = 1u << 29;") && as::kBlueNoiseFlag == (1u << 29), "the word's blue flag is bit 29, ao_stable.h's");
  g_check(nc::Has(ao, "const bool denoise_visibility = RtDenoise::V2Enabled();") &&
              nc::Has(ao, "city_in.sun_frame = (noise_resolve::Varies(plan.upscaler, plan.contact_history || denoise_visibility) ? uint32_t(scene_noise::NoiseIndex(frame_) & 1023) : 0u) | (larecomp::settings::mcla_gpu_ao_blue_noise.Get() ? ao_stable::kBlueNoiseFlag : 0u);"),
          "ao.cpp sets the city shadow's blue flag and advances its phase for the upscaler, contact history or v2 visibility denoiser");
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
