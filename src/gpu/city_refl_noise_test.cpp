// Check of the noise of a city hit's sun ray and lamp choice in the car reflections (hlsl/city_refl_noise.hlsli, city_refl.hlsli CityReflHit, city_refl_rules.h's flag, city_refl.cpp; NOISE-AUDIT,
// perf\NOISE-AUDIT.md). A program of its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_city_refl_noise_test, then run src/gpu/larecomp_city_refl_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// The first version's numbers were interleaved gradient noise of the texel (a lattice) plus the R2 shift by the float of the whole frame count plus a salt hashed from the ray's direction. Now the blue
// noise tile at the texel stepped along R2 by the sample number frame * 5 + the hit's number in the texel's cone. It compiles the .hlsli as C++ (hlsl_shim.h) and checks, with the shared measures of
// noise_resolve_check.h:
//   the numbers are uniform, change at every texel every frame, leave no gap over 0.25 in 16 frames, take one step at every texel (nothing slides); the cone's rays get different numbers; the numbers
//   stay uniform at frame counts over 2^30 (the first version's float product loses its fraction)
//   the error of a shaded sun edge after the history has no lattice where the first version's has one
//   the sources: the shader's flag is the header's bit 5 (clear of the others), the three callers reset the hit counter, the host sets the flag from mcla_gpu_noise_blue and the rows carry it

#include <cstdint>
#include <cstdio>
#include <string>

#include "city_refl_rules.h"
#include "hlsl_shim.h"
#include "noise_resolve_check.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/city_refl_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;
namespace cr = mcla::gpu::city_refl;

namespace {

nc::Checks g_check;

hs::float3 Dir(int x, int y) { return hs::float3(0.3f + 0.001f * float(x), 0.5f + 0.0007f * float(y), 0.8f); }

std::pair<float, float> Noise(int x, int y, uint32_t frame, uint32_t hit, bool blue) {
  const hs::float2 u = hs::CityReflNoise(x, y, frame, hit, Dir(x, y), blue);
  return {u.x, u.y};
}

// A shaded sun edge over the disc (a hit's shadow edge) of the two numbers, as the city hit's sun ray makes it.
double Edge(float u1, float u2) { return std::sqrt(double(u1)) * std::cos(6.2831853 * double(u2)) > 0.3 ? 1.0 : 0.0; }

void TestNumbers() {
  const auto blue = [](int x, int y, uint32_t f) { return Noise(x, y, f, 0, true); };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(blue, channel, 5000);
    if (g_check.print) {
      nc::Print(channel == 0 ? "hit number 1, blue" : "hit number 2, blue", p);
    }
    g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
            nc::Num("number %d: uniform (mean %.3f), changes at every texel every frame (%.3f), no gap over %.3f in 16 frames, one step at every texel (%.6f)", channel + 1, p.mean, p.changed, p.max_gap, p.step_spread));
  }
  // the cone's rays: five numbers in a frame, all different, the sequence of frame * 5 + hit
  bool differ = true, sequence = true;
  for (int y = 0; y < 16 && differ; ++y) {
    for (int x = 0; x < 16 && differ; ++x) {
      for (uint32_t h = 0; h < 4; ++h) {
        differ = differ && Noise(x, y, 77, h, true).first != Noise(x, y, 77, h + 1, true).first;
      }
      sequence = sequence && Noise(x, y, 9, 3, true) == Noise(x, y, 8, 3 + 5, true);
    }
  }
  g_check(differ && sequence, "the cone's rays of a texel get different numbers, and the hit h of frame f + 1 is the sample 5 f + 5 + h of one sequence");
  // long sessions: the product wraps exactly in integers; the first version's float product has lost its fraction by then
  double sum = 0.0, sum2 = 0.0;
  int n = 0, bad = 0;
  for (uint32_t base : {0x7FFFFF00u, 0xFFFFFF00u, 0x40000000u}) {
    for (int y = 0; y < 24; ++y) {
      for (int x = 0; x < 24; ++x) {
        const std::pair<float, float> u = Noise(x, y, base + 3u, 1, true), v = Noise(x, y, base + 4u, 1, true);
        sum += u.first;
        sum2 += double(u.first) * u.first;
        ++n;
        bad += (u.first == v.first || u.first < 0.0f || u.first >= 1.0f) ? 1 : 0;
      }
    }
  }
  const double mean = sum / n, var = sum2 / n - mean * mean;
  g_check(bad == 0 && std::fabs(mean - 0.5) < 0.02 && std::fabs(var - 1.0 / 12.0) < 0.006, nc::Num("frame counts over 2^30 stay uniform on [0, 1) (mean %.3f, variance %.4f) and keep stepping", mean, var));
}

void TestResolve() {
  const nc::Resolved rl = nc::Resolve([](int x, int y, uint32_t f) { return Noise(x, y, f + 5000, 0, false); }, Edge, 100, 128, 40, 0.9, false);
  const nc::Resolved rb = nc::Resolve([](int x, int y, uint32_t f) { return Noise(x, y, f + 5000, 0, true); }, Edge, 100, 128, 40, 0.9, false);
  if (g_check.print) {
    nc::Print("sun edge after the history, first version", rl);
    nc::Print("sun edge after the history, blue", rb);
  }
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue noise's error after the history has no frequency that stands out (%.4f) or direction (%.2f, under 3) and stays where it is (%.2f at (%d, %d))",
                                                             rb.shape.peak_share, rb.shape.wedge_ratio, rb.best_corr, rb.best_dx, rb.best_dy));
  g_check(rb.rms < 1.3 * rl.rms, nc::Num("... at an rms of %.4f against the first version's %.4f (a lattice share of %.3f, a direction %.1f)", rb.rms, rl.rms, rl.shape.peak_share, rl.shape.wedge_ratio));
}

void TestSources(const std::string& dir) {
  const std::string hlsli = nc::ReadFile(dir + "/hlsl/city_refl.hlsli");
  const std::string noise = nc::ReadFile(dir + "/hlsl/city_refl_noise.hlsli");
  const std::string cs = nc::ReadFile(dir + "/hlsl/car_rt_refl_cs.hlsl");
  const std::string glass = nc::ReadFile(dir + "/hlsl/car_rt_refl_glass.hlsli");
  const std::string opp = nc::ReadFile(dir + "/hlsl/car_rt_refl_opp.hlsli");
  const std::string host = nc::ReadFile(dir + "/city_refl.cpp");
  if (hlsli.empty() || noise.empty() || cs.empty() || glass.empty() || opp.empty() || host.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  const uint32_t others = cr::kFlagOn | cr::kFlagScreen | cr::kFlagStats | cr::kFlagCoat | cr::kFlagLamps;
  g_check(cr::kFlagBlueNoise == 32u && (cr::kFlagBlueNoise & others) == 0u && nc::Has(hlsli, "static const uint kCityFlagBlueNoise = 32u;"), "the rows' blue noise flag is bit 5, the shader's constant is the header's and no other flag overlaps it");
  g_check(nc::Has(hlsli, "#include \"city_refl_noise.hlsli\"") && nc::Has(hlsli, "CityReflNoise(int(gCityTexel.x), int(gCityTexel.y), c.serial, min(gCityHits++, kCityReflRaysPerFrame - 1u), direction, (c.flags & kCityFlagBlueNoise) != 0u)") &&
              !nc::Has(hlsli, "float CityReflIgn(") && !nc::Has(hlsli, "43758"),
          "CityReflHit takes the numbers from city_refl_noise.hlsli (the hit's number counted in the texel) and carries no gradient noise or salt of its own");
  g_check(nc::Has(cs, "gCityTexel = texel;  // the noise of the city hits' sun rays (city_refl.hlsli)\n    gCityHits = 0u;") && nc::Has(glass, "gCityHits = 0u;") && nc::Has(opp, "gCityHits = 0u;"),
          "the player's pass, the glass pass and the opponents' pass reset the hit counter with the texel");
  g_check(nc::Has(host, "in.blue_noise = larecomp::settings::mcla_gpu_noise_blue.Get();") && nc::Has(noise, "serial * kCityReflRaysPerFrame + hit"), "city_refl.cpp sets the flag from mcla_gpu_noise_blue (live: read at each frame's rows)");
  cr::RowsIn in;
  in.level = cr::Level::kHigh;
  in.blue_noise = true;
  uint32_t rows[cr::kRows][cr::kRowWords];
  cr::MakeRows(in, rows);
  const uint32_t first = rows[cr::kRowControl][0];
  in.blue_noise = false;
  cr::MakeRows(in, rows);
  g_check((first & cr::kFlagBlueNoise) != 0u && (rows[cr::kRowControl][0] & cr::kFlagBlueNoise) == 0u && (first & ~cr::kFlagBlueNoise) == rows[cr::kRowControl][0], "the rows carry the flag when asked and no other bit changes with it");
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
  TestNumbers();
  TestResolve();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
