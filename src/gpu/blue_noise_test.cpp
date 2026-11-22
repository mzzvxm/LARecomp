// Check of the blue noise tile the screen-space effects sample with (blue_noise.h, hlsl/blue_noise.hlsli). A program of its own: it needs no renderer, no ReXGlue
// and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_blue_noise_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_blue_noise_test [--print]
//   <build dir>/src/gpu/larecomp_blue_noise_test --emit > src/gpu/hlsl/blue_noise.hlsli     (writes the shader's table; the check below compares it with the tile)
//
// It checks: the tile is a permutation of ranks that keeps each of its 256 levels in four pixels (so the noise is uniform exactly), the same on every run, and BLUE
// (no energy at the low spatial frequencies and no peak: the spectrum of the tile, which is the spectrum of the picture the tile repeats into), its two channels are
// two noises; the shader's table (hlsl/blue_noise.hlsli, compiled here as C++) is the tile; and the noise field after the occlusion's 3x3 blur against interleaved
// gradient noise, the first version's. The occlusion's own use of it is checked by ao_blue_noise_test.cpp, the reflections' by ssr_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "blue_noise.h"
#include "blue_noise_spectrum.h"

namespace hlsl_shim {
typedef unsigned int uint;
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
#if __has_include("hlsl/blue_noise.hlsli")
#define HAVE_BLUE_NOISE_HLSLI 1
#include "hlsl/blue_noise.hlsli"
#endif
}  // namespace hlsl_shim

namespace bn = mcla::gpu::blue_noise;
using namespace blue_noise_spectrum;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[300];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

// ---------------------------------------------------------------------------
// the table the shader holds
// ---------------------------------------------------------------------------

// hlsl/blue_noise.hlsli, as `--emit` writes it.
void Emit(const bn::Tile& tile) {
  std::printf(
      "// MCLA GPU renderer -- the blue noise tile the screen-space effects sample with (blue_noise.h): a void-and-cluster tile of %d x %d pixels, one byte a pixel\n"
      "// (the top 8 bits of its rank: each of the %d levels in four pixels, so the noise (level + 0.5) / %d is uniform), four to a word, pixel 4 w in the low byte.\n"
      "// The second channel is the same tile read %d pixels right and %d down.\n"
      "//\n"
      "// WRITTEN by `larecomp_blue_noise_test --emit` from blue_noise.h and checked against it by the same test (src/gpu/blue_noise_test.cpp): do not edit by hand.\n"
      "// The lookup is plain enough to compile as C++ (the test does, and so does ssr_test.cpp).\n\n"
      "#ifndef MCLA_GPU_BLUE_NOISE_HLSLI\n#define MCLA_GPU_BLUE_NOISE_HLSLI\n\n"
      "#define BLUE_NOISE_SIZE %d\n\n"
      "static const uint kBlueNoiseTile[%d] = {\n",
      bn::kSize, bn::kSize, bn::kLevels, bn::kLevels, bn::kSecondX, bn::kSecondY, bn::kSize, bn::kCount / 4);
  for (int w = 0; w < bn::kCount / 4; ++w) {
    const uint32_t word = uint32_t(tile.level[4 * w]) | (uint32_t(tile.level[4 * w + 1]) << 8) | (uint32_t(tile.level[4 * w + 2]) << 16) | (uint32_t(tile.level[4 * w + 3]) << 24);
    std::printf("%s0x%08Xu,%s", (w % 8) == 0 ? "  " : " ", word, (w % 8) == 7 ? "\n" : "");
  }
  std::printf(
      "};\n\n"
      "// The tile at the pixel (x, y) (any integer: the tile repeats) for channel 0, at the pixel moved by (%d, %d) for channel 1: uniform on (0, 1).\n"
      "float BlueNoiseTileAt(int x, int y, uint channel) {\n"
      "  const uint i = (((uint(y) + channel * %du) & %du) << 5u) | ((uint(x) + channel * %du) & %du);\n"
      "  return (float((kBlueNoiseTile[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu) + 0.5f) * (1.0f / %d.0f);\n"
      "}\n"
      "// Both channels of the pixel.\n"
      "float2 BlueNoiseTile2At(int x, int y) {\n"
      "  return float2(BlueNoiseTileAt(x, y, 0u), BlueNoiseTileAt(x, y, 1u));\n"
      "}\n\n"
      "#endif  // MCLA_GPU_BLUE_NOISE_HLSLI\n",
      bn::kSecondX, bn::kSecondY, bn::kSecondY, bn::kSize - 1, bn::kSecondX, bn::kSize - 1, bn::kLevels);
}

// ---------------------------------------------------------------------------
// the tile
// ---------------------------------------------------------------------------

void CheckTile(const bn::Tile& tile) {
  // The generator's ranks are a permutation, and the tile keeps each of the 256 levels in four pixels: the noise is uniform exactly.
  const bn::Ranks ranks = bn::Generate(bn::kSize, bn::kSeed);
  std::vector<int> seen(size_t(bn::kCount), 0);
  bool in_range = true;
  for (int i = 0; i < bn::kCount; ++i) {
    if (ranks.rank[size_t(i)] >= bn::kCount) {
      in_range = false;
    } else {
      ++seen[ranks.rank[size_t(i)]];
    }
  }
  bool once = in_range;
  for (int c : seen) {
    once = once && c == 1;
  }
  Check(once, "the generator's ranks are a permutation of 0 to 1023");
  std::vector<int> levels(size_t(bn::kLevels), 0);
  for (int i = 0; i < bn::kCount; ++i) {
    ++levels[tile.level[i]];
  }
  bool four = true;
  for (int c : levels) {
    four = four && c == bn::kCount / bn::kLevels;
  }
  Check(four, "the tile holds each of the 256 levels in exactly four pixels: the noise is uniform exactly");
  // Deterministic: a second run makes the same tile (integers only, no platform in it); another seed makes another one.
  const bn::Tile again = bn::MakeTile();
  Check(std::memcmp(&tile, &again, sizeof(tile)) == 0, "the tile is the same on a second run");
  const bn::Ranks other = bn::Generate(bn::kSize, bn::kSeed + 1u);
  int equal = 0;
  for (int i = 0; i < bn::kCount; ++i) {
    equal += other.rank[size_t(i)] == ranks.rank[size_t(i)] ? 1 : 0;
  }
  Check(equal < 16, Num("another seed makes another tile (%.0f pixels equal)", double(equal)));
  Check(bn::Value(0) > 0.0f && bn::Value(255) < 1.0f, "the value of the first and the last level is inside (0, 1)");
  // The two channels are two noises: the correlation of channel 0 at a pixel with channel 1 at the pixels round it is small (the offset was chosen for that; the tile's
  // own correlation with its neighbours is 0.33: it is anti-correlated, not white).
  double worst = 0.0;
  for (int ly = -3; ly <= 3; ++ly) {
    for (int lx = -3; lx <= 3; ++lx) {
      double sum = 0.0, s1 = 0.0, s2 = 0.0;
      for (int y = 0; y < bn::kSize; ++y) {
        for (int x = 0; x < bn::kSize; ++x) {
          const double u = double(bn::Sample(tile, 0, x, y)) - 0.5, v = double(bn::Sample(tile, 1, x + lx, y + ly)) - 0.5;
          sum += u * v;
          s1 += u * u;
          s2 += v * v;
        }
      }
      worst = std::max(worst, std::fabs(sum / std::sqrt(s1 * s2)));
    }
  }
  if (g_print) {
    std::printf("the two channels: worst correlation over the 7 x 7 pixels round a pixel %.4f\n", worst);
  }
  Check(worst < 0.1, Num("channel 1 is not channel 0: their correlation over the 7 x 7 pixels round a pixel is at most %.4f (under 0.1)", worst));
}

// The spectrum of the tile: it IS the spectrum of the picture it repeats into (a periodic picture has energy only at the tile's own frequencies).
void CheckBlue(const bn::Tile& tile) {
  const int n = bn::kSize;
  for (int c = 0; c < bn::kChannels; ++c) {
    std::vector<double> img(size_t(n) * size_t(n));
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        img[size_t(y) * size_t(n) + size_t(x)] = double(bn::Sample(tile, c, x, y));
      }
    }
    const std::vector<double> p = Power(img, n, false);
    const Shape s = Analyse(p, n);
    // Blue: nothing at the low frequencies (white noise has kLowEnd^2 * pi of its energy there, 4.5%), and no single bin that stands out (white noise's
    // largest of 1023 bins takes about 0.7%; interleaved gradient noise's takes 4% to 13%).
    const double low_share = s.low_energy / s.total;
    Check(low_share < 0.0015, Num("channel %.0f: the energy under 0.12 cycles a pixel is %.4f of the tile's (white noise: 0.045; blue noise: nothing)", double(c), low_share));
    Check(s.peak_share < 0.012, Num("channel %.0f: the largest frequency of the tile takes %.4f of its energy (no peak: under 0.012)", double(c), s.peak_share));
    if (g_print) {
      std::printf("tile channel %d: low share %.5f, peak share %.4f, band share %.3f, wedge ratio %.2f\n", c, low_share, s.peak_share, s.band_share, s.wedge_ratio);
    }
  }
}

// What the shader reads is the tile.
void CheckTable(const bn::Tile& tile) {
#ifndef HAVE_BLUE_NOISE_HLSLI
  (void)tile;
  Check(false, "hlsl/blue_noise.hlsli exists: run  larecomp_blue_noise_test --emit > src/gpu/hlsl/blue_noise.hlsli");
#else
  using hlsl_shim::BlueNoiseTile2At;
  using hlsl_shim::BlueNoiseTileAt;
  Check(BLUE_NOISE_SIZE == bn::kSize, "blue_noise.hlsli: the tile's size");
  int wrong = 0;
  for (int y = -37; y < 70; ++y) {
    for (int x = -41; x < 75; ++x) {
      const float a = BlueNoiseTileAt(x, y, 0u), b = BlueNoiseTileAt(x, y, 1u);
      const hlsl_shim::float2 ab = BlueNoiseTile2At(x, y);
      if (a != bn::Sample(tile, 0, x, y) || b != bn::Sample(tile, 1, x, y) || ab.x != a || ab.y != b) {
        ++wrong;
      }
    }
  }
  Check(wrong == 0, Num("blue_noise.hlsli: the lookup gives the tile's value in both channels at every pixel, the negative ones and past the tile too (%.0f differ)", double(wrong)));
  // The whole table, word by word.
  int words = 0;
  for (int w = 0; w < bn::kCount / 4; ++w) {
    const uint32_t word = uint32_t(tile.level[4 * w]) | (uint32_t(tile.level[4 * w + 1]) << 8) | (uint32_t(tile.level[4 * w + 2]) << 16) | (uint32_t(tile.level[4 * w + 3]) << 24);
    words += hlsl_shim::kBlueNoiseTile[w] != word ? 1 : 0;
  }
  Check(words == 0, Num("blue_noise.hlsli: all 256 words of the table are the tile's (%.0f differ: run --emit)", double(words)));
#endif
}

// ---------------------------------------------------------------------------
// the noise field against interleaved gradient noise
// ---------------------------------------------------------------------------

void CheckField(const bn::Tile& tile) {
  const int n = 256;  // 8 tiles; the interleaved gradient noise's lattice is far longer than that
  std::vector<double> ign(size_t(n) * size_t(n)), ign_t(size_t(n) * size_t(n)), blue(size_t(n) * size_t(n)), white(size_t(n) * size_t(n));
  uint32_t s = 12345u;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      ign[size_t(y) * size_t(n) + size_t(x)] = Ign(x, y);
      ign_t[size_t(y) * size_t(n) + size_t(x)] = Ign(y + 31, x + 11);
      blue[size_t(y) * size_t(n) + size_t(x)] = double(bn::Sample(tile, 0, x, y));
      s = s * 1664525u + 1013904223u;
      white[size_t(y) * size_t(n) + size_t(x)] = double(s >> 8) / double(1 << 24);
    }
  }
  const Shape sign = Analyse(Power(Blur3(ign, n), n, false), n);
  const Shape sign_t = Analyse(Power(Blur3(ign_t, n), n, false), n);
  const Shape sblue = Analyse(Power(Blur3(blue, n), n, false), n);
  const Shape swhite = Analyse(Power(Blur3(white, n), n, false), n);
  const Shape rign = Analyse(Power(ign, n, false), n);
  const Shape rblue = Analyse(Power(blue, n, false), n);
  if (g_print) {
    std::printf("field, after the 3x3 blur: low-band energy ign %.5f ign^T %.5f blue %.5f white %.5f; peak share ign %.3f blue %.4f; wedge ratio ign %.2f blue %.2f\n",
                sign.low_energy / double(n * n), sign_t.low_energy / double(n * n), sblue.low_energy / double(n * n), swhite.low_energy / double(n * n), sign.peak_share,
                sblue.peak_share, sign.wedge_ratio, sblue.wedge_ratio);
    std::printf("field, raw: peak share ign %.3f blue %.4f\n", rign.peak_share, rblue.peak_share);
  }
  // What the blur leaves of the pattern at the low frequencies, the ones it cannot take out: the blue noise's is a small fraction of the gradient noise's
  // (and of white noise's, which is the other way to lose the lattice).
  Check(sblue.low_energy * 50.0 < sign.low_energy, Num("after the blur the blue noise leaves under 1/50 of the gradient noise's low-frequency energy (%.1f times less)", sign.low_energy / sblue.low_energy));
  Check(sblue.low_energy * 50.0 < sign_t.low_energy, "... and of the transposed gradient noise's (the second channel)");
  Check(sblue.low_energy * 1000.0 < swhite.low_energy, Num("... and under 1/1000 of white noise's (%.0f times less)", swhite.low_energy / sblue.low_energy));
  // A lattice is a handful of frequencies: the gradient noise's raw peak takes a few per cent of the energy in one bin, the blue noise's a tenth of a per cent.
  Check(rign.peak_share > 0.02, Num("the gradient noise is a lattice: one frequency takes %.3f of its energy (over 0.02)", rign.peak_share));
  Check(rblue.peak_share < 0.012, Num("the blue noise is not: its largest frequency takes %.4f (under 0.012)", rblue.peak_share));
  Check(sign.peak_share > 0.02, Num("after the blur the gradient noise is still a lattice (%.3f in one frequency, over 0.02)", sign.peak_share));
  // (The blur takes the high frequencies, so the blue noise's share of what is left in its largest bin rises: the amount in the bin is the measure.)
  Check(sblue.peak_abs * 5.0 < sign.peak_abs, Num("after the blur the gradient noise's one frequency holds %.1f times what the blue noise's largest does (5 or more: the tile's own period of 32 pixels is its strongest, and a few thousandths of the noise's value)", sign.peak_abs / std::max(sblue.peak_abs, 1e-12)));
}

}  // namespace

int main(int argc, char** argv) {
  bool emit = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else if (std::strcmp(argv[i], "--emit") == 0) {
      emit = true;
    }
  }
  const bn::Tile tile = bn::MakeTile();
  if (emit) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // the file is checked in with LF line ends
#endif
    Emit(tile);
    return 0;
  }
  CheckTile(tile);
  CheckBlue(tile);
  CheckTable(tile);
  CheckField(tile);
  std::printf("blue_noise_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
