#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "hlsl_shim.h"

namespace hlsl_shim {
#include "hlsl/rtao_noise.hlsli"
}

int main() {
  using namespace hlsl_shim;
  int checks = 0, failed = 0;
  const auto check = [&](bool ok, const char* what) {
    ++checks;
    if (!ok && ++failed <= 12) std::printf("FAIL: %s\n", what);
  };
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      std::array<int, 4> corner{};
      const auto first = RtAoRayNoise(x, y, 0, 0);
      for (unsigned f = 0; f < 16; ++f) {
        const auto subpixel = RtAoSubpixel(x, y, f);
        check(subpixel < corner.size(), "subpixel index");
        if (subpixel < corner.size()) ++corner[subpixel];
        const auto noise = RtAoRayNoise(x, y, f, 0);
        check(noise.x >= 0 && noise.x < 1 && noise.y >= 0 && noise.y < 1, "uniform domain");
        if (f != 0) check(noise.x != first.x && noise.y != first.y, "noise varies with frame");
      }
      for (int count : corner) check(count >= 2 && count <= 6, "16 frames cover the four corners");
      for (unsigned period : {64u, 128u, 256u, 512u, 1024u, 4096u}) {
        const auto later = RtAoRayNoise(x, y, period, 0);
        check(later.x != first.x && later.y != first.y, "no short period");
      }
      for (unsigned r = 1; r < 4; ++r) {
        const auto other = RtAoRayNoise(x, y, 0, r);
        check(other.x != first.x && other.y != first.y, "ray lanes are distinct");
      }
    }
  }
  check(RtAoNoiseSample(1, 3) == 11 && RtAoNoiseSample(0x100000, 3) == 3, "eight lanes in a 20-bit frame");
  // Low-frequency peaks would draw broad stripes. Test both channels in two
  // frames, over the noise's unit-circle embedding, where it is periodic in value.
  for (const unsigned frame : {0u, 1237u}) {
    for (int channel = 0; channel < 2; ++channel) {
      for (int ky = -4; ky <= 4; ++ky) {
        for (int kx = -4; kx <= 4; ++kx) {
          if (kx == 0 && ky == 0) continue;
          double real = 0.0, imaginary = 0.0;
          for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
              const auto v = RtAoRayNoise(x, y, frame, 0);
              const double phase = 2.0 * std::numbers::pi * ((channel == 0 ? v.x : v.y) - (kx * x + ky * y) / 32.0);
              real += std::cos(phase);
              imaginary += std::sin(phase);
            }
          }
          check(std::hypot(real, imaginary) < 90.0, "no prominent low-frequency lattice");
        }
      }
    }
  }
  std::printf("rtao_noise_test: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
