// Check of the sun shadow filter's disk turn (src/shaderpack/variants/shadow_filter.cpp ShadowFilterNoise and its blue noise table, shadow_filter.h's mode bit and constants; NOISE-AUDIT,
// perf\NOISE-AUDIT.md). A program of its own: no renderer, no ReXGlue, no GPU, no game files.
//
//   cmake --build <dir> --target larecomp_shadow_filter_noise_test, then run src/gpu/larecomp_shadow_filter_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// While a temporal upscaler runs, the shadow lookup's Vogel disk is turned per host pixel (mode bit 4) so that the upscaler's history averages the sampling grain of a penumbra. The turn was
// interleaved gradient noise plus the R2 shift of the frame, a lattice: the spectral line of the noise at (0.44, 0.31) cycles a pixel that the owner's "dithering artifacts" of 2026-11-19 showed, which the
// history leaves as a diagonal pattern in a penumbra. With mode bit 5 the turn is the blue noise tile at the pixel plus a step along the R2 sequence by the frame, the same for every pixel (the table is
// the one of hlsl/blue_noise.hlsli, its words read here from the variant's own text and compared with the generator's). It checks, with the shared measures of noise_resolve_check.h and a C++ twin of the
// helper written from the variant's text:
//   the table is the blue noise tile's; the turn is uniform, changes at every pixel every frame, leaves no gap over 0.25 in 16 frames, takes one step at every pixel (nothing slides)
//   the error of a 16 tap penumbra's lit share after the upscaler's average over frames: the first version's has a lattice, the tile's has none and stays where it is
//   the host: mode bit 5 is clear of the other bits, set only with the noise (a temporal upscaler), by the setting; the variant's text and version

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "blue_noise.h"
#include "noise_resolve_check.h"
#include "shadow_filter.h"

namespace nc = noise_check;
namespace sf = mcla::gpu::shadow_filter;

namespace {

nc::Checks g_check;
std::vector<uint32_t> g_table;  // the words of kShadowFilterTile, read from the variant's text

// The words of `static const uint <name>[256] = { ... };` in a text.
std::vector<uint32_t> Words(const std::string& text, const char* name) {
  std::vector<uint32_t> out;
  const size_t at = text.find(std::string("static const uint ") + name + "[256] = {");
  if (at == std::string::npos) {
    return out;
  }
  size_t pos = text.find('{', at) + 1;
  const size_t end = text.find("};", pos);
  while (pos < end) {
    const size_t x = text.find("0x", pos);
    if (x == std::string::npos || x >= end) {
      break;
    }
    out.push_back(uint32_t(std::strtoul(text.c_str() + x + 2, nullptr, 16)));
    pos = x + 10;
  }
  return out;
}

// The variant's ShadowFilterTile and ShadowFilterNoise (shaderpack/variants/shadow_filter.cpp), as C++.
float Tile(float px, float py) {
  const int x = int(std::floor(px)), y = int(std::floor(py));
  const uint32_t i = ((uint32_t(y) & 31u) << 5u) | (uint32_t(x) & 31u);
  return (float((g_table[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu) + 0.5f) * (1.0f / 256.0f);
}
float Noise(float px, float py, float frame, bool blue) {
  if (blue) {
    const float v = Tile(px, py) + float(uint32_t(frame) * 3242174889u) * (1.0f / 4294967296.0f);
    return v - std::floor(v);
  }
  float inner = 0.06711056f * std::floor(px) + 0.00583715f * std::floor(py);
  inner -= std::floor(inner);
  float g = 52.9829189f * inner;  // the first version: frac(52.98 * frac(dot(floor(pixel), ...)))
  g -= std::floor(g);
  const float v = g + frame * 0.75487766624669f;
  return v - std::floor(v);
}

std::pair<float, float> Blue(int x, int y, uint32_t frame) { return {Noise(float(x) + 0.5f, float(y) + 0.5f, float(frame & 1023u), true), 0.0f}; }
std::pair<float, float> Legacy(int x, int y, uint32_t frame) { return {Noise(float(x) + 0.5f, float(y) + 0.5f, float(frame & 1023u), false), 0.0f}; }

// The lit share of a straight penumbra for a Vogel disk of 16 taps turned by `turns` (the variant's disk: sqrt radius, golden angle), the edge 0.2 radii from the centre.
double Lit(float turns, float) {
  const double golden = 2.399963229728653;
  int lit = 0;
  for (int i = 0; i < 16; ++i) {
    const double r = std::sqrt((double(i) + 0.5) / 16.0), a = golden * double(i) + 6.283185307179586 * double(turns);
    lit += (r * std::cos(a) > 0.2) ? 1 : 0;
  }
  return double(lit) / 16.0;
}

void TestTable(const std::string& dir, const std::string& variant) {
  const std::vector<uint32_t> hlsli = Words(nc::ReadFile(dir + "/hlsl/blue_noise.hlsli"), "kBlueNoiseTile");
  g_table = Words(variant, "kShadowFilterTile");
  g_check(g_table.size() == 256 && g_table == hlsli, nc::Num("the variant's table is hlsl/blue_noise.hlsli's: 256 words (%zu read), the same", g_table.size()));
  const mcla::gpu::blue_noise::Tile tile = mcla::gpu::blue_noise::MakeTile();
  bool same = g_table.size() == 256;
  for (int i = 0; same && i < mcla::gpu::blue_noise::kCount; ++i) {
    same = ((g_table[size_t(i >> 2)] >> ((i & 3) * 8)) & 0xFFu) == tile.level[i];
  }
  g_check(same, "... and the generator's tile (blue_noise.h) byte for byte");
}

void TestTurn() {
  if (g_table.size() != 256) {
    g_check(false, "no table to test the turn with");
    return;
  }
  const nc::PerFrame p = nc::PerFrameOf(Blue, 0);
  if (g_check.print) {
    nc::Print("disk turn, blue, by frame", p);
  }
  g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004,
          nc::Num("with an upscaler the turn is uniform (mean %.3f, variance %.4f), changes at every pixel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes one step at every pixel (%.6f)", p.mean, p.var,
                  p.changed, p.max_gap, p.step_spread));
  const nc::Resolved rl = nc::Resolve(Legacy, Lit, 100, 128, 40, 0.9, false);
  const nc::Resolved rb = nc::Resolve(Blue, Lit, 100, 128, 40, 0.9, false);
  if (g_check.print) {
    nc::Print("penumbra after the history, first version", rl);
    nc::Print("penumbra after the history, blue", rb);
  }
  g_check(nc::IsLattice(rl), nc::Num("the first version's error in a penumbra after the upscaler's history is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb), nc::Num("the blue tile's has none (%.4f in one frequency, a direction %.2f, under 3) and stays where it is", rb.shape.peak_share, rb.shape.wedge_ratio));
  g_check(rb.rms < 1.3 * rl.rms, nc::Num("... at an rms of %.4f against the first version's %.4f", rb.rms, rl.rms));
}

void TestHost(const std::string& dir, const std::string& variant) {
  const uint32_t bits = sf::kModeGather | sf::kModePcss | sf::kModeNoise | sf::kModeEarlyOut | (255u << sf::kSunShift);
  g_check(sf::kModeBlueNoise == 32u && (sf::kModeBlueNoise & bits) == 0u, "mode bit 5 is the blue noise's: clear of the gather, contact hardening, noise and sun size bits");
  const sf::ShadowFilterConstants with = sf::MakeConstants(0, 1.0, 1.0f, false, true, 77, false, 4.0, true);
  const sf::ShadowFilterConstants without = sf::MakeConstants(0, 1.0, 1.0f, false, true, 77, false, 4.0, false);
  const sf::ShadowFilterConstants still = sf::MakeConstants(0, 1.0, 1.0f, false, false, 77, false, 4.0, true);
  g_check(uint32_t(with.mode) == (sf::kModeNoise | sf::kModeBlueNoise) && uint32_t(without.mode) == sf::kModeNoise && with.noise_frame == 77.0f,
          "with an upscaler the mode has the noise bit, and the blue bit by the setting; the frame is the shift's");
  g_check(uint32_t(still.mode) == 0u && still.noise_frame == 0.0f, "without an upscaler there is no noise and no blue bit: every pixel takes the same disk, as before");
  const std::string host = nc::ReadFile(dir + "/shadow_filter.cpp");
  g_check(nc::Has(host, "larecomp::settings::mcla_gpu_noise_blue.Get(),"), "shadow_filter.cpp passes mcla_gpu_noise_blue to the constants (read at every draw: live)");
  g_check(nc::Has(variant, "(mode & 4u) != 0u ? 6.28318530718 * ShadowFilterNoise(pixel, constants.z, (mode & 32u) != 0u) : kShadowFilterFixedAngle") &&
              nc::Has(variant, "float(uint(frame) * 3242174889u) * (1.0 / 4294967296.0)") && nc::Has(variant, "float ShadowFilterNoise(float2 pixel, float frame, bool blue)"),
          "the variant's text turns the disk by the tile and R2 step with mode bit 5, by the first version's noise without it, and by nothing without bit 4");
  const std::string header = nc::ReadFile(dir + "/../shaderpack/variants/shadow_filter.h");
  g_check(nc::Has(header, "kShadowFilterVariantVersion = 5;"), "version 5 rebuilds the pack for the PCSS probes");
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
  const std::string variant = nc::ReadFile(dir + "/../shaderpack/variants/shadow_filter.cpp");
  if (variant.empty()) {
    std::printf("FAIL: the variant's source could not be read (run the test with the src/gpu folder as its argument)\n");
    return 1;
  }
  TestTable(dir, variant);
  TestTurn();
  TestHost(dir, variant);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
