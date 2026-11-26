#include "ao_rate_rules.h"
#include "hlsl_shim.h"

#include <cstdio>

namespace hlsl_shim {
#include "hlsl/rtao_noise.hlsli"
}

int main() {
  namespace rate = mcla::gpu::ao_rate;
  using namespace hlsl_shim;
  int checks = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* what) {
    ++checks;
    if (!ok && ++failed <= 12) {
      std::printf("FAIL: %s\n", what);
    }
  };
  for (unsigned frame = 0; frame < 13; ++frame) {
    unsigned corners = 0;
    for (unsigned f = frame; f < frame + 4; ++f) {
      const unsigned phase = rate::AoSamplePhase(f);
      corners |= 1u << phase;
      check(phase == f % 4, "rotating depth takes the same phase on both axes");
      check(rate::AoSourceAxis(4, phase & 1, 8) == 8, "odd edge clamps to the only available texel");
      check(rate::AoSourceAxis(3, phase >> 1, 8) == 6 + (phase >> 1), "interior uses the requested subpixel");
    }
    check(corners == 15, "each four-frame window visits all four corners");
  }
  check(rate::AoSamplePhase(0) == 0, "first sample after a cut is top left");
  check(rate::AoGatherIndex(0, 0) == 3 && rate::AoGatherIndex(1, 0) == 2 &&
        rate::AoGatherIndex(0, 1) == 0 && rate::AoGatherIndex(1, 1) == 1, "gather taps follow API ordering");
  for (unsigned id = 0; id <= 23; ++id) {
    for (unsigned age = 0; age <= rate::kMaxAge; ++age) {
      const float state = float(rate::AoHistoryState(age, id));
      check(rate::AoHistoryAge(state) == age && rate::AoHistoryId(state) == id, "history stores separate surface ID and age");
      check(rate::AoNextAge(float(age), 12) == (age < 12 ? age + 1 : 12), "history age increments and respects cap");
      check(rate::AoScreenSlices(age, 0.0f, 0.5f) == (age < 8 ? 3 : 2), "screen trace reduces after convergence");
      check(rate::AoRayProbability(age, 0.0f) == (age < 4 ? 1.0f : 0.25f), "RT trace fills new and disoccluded pixels");
      check(rate::AoRayProbability(age, 0.012f) == (age < 4 ? 1.0f : 0.5f), "RT trace probes moderate variance");
    }
  }
  check(rate::AoRateSurface(10.0f, 10.08f) && !rate::AoRateSurface(10.0f, 10.11f) &&
        !rate::AoRateSurface(10.0f, 9.89f), "depth disagreement rejects both sides of a disocclusion");
  check(rate::AoScreenSlices(31, 0.02f, 0.5f) == 3, "screen tracing rechecks noisy pixels");
  check(rate::AoRayProbability(31, 0.07f) == 1.0f, "RT tracing rechecks noisy pixels");
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      int reduced_screen = 0;
      int sampled_rt = 0;
      for (unsigned frame = 0; frame < 13; ++frame) {
        const float screen = BlueNoiseFrame(x, y, 0u, frame, kBlueNoiseR2StepB);
        const float rt = BlueNoiseFrame(x, y, 1u, RtAoNoiseSample(frame, 6u), kBlueNoiseR2StepB);
        reduced_screen += rate::AoScreenSlices(12, 0.0f, screen) == 2;
        sampled_rt += rt < rate::AoRayProbability(12, 0.0f);
      }
      check(reduced_screen >= 8 && reduced_screen <= 12, "screen rate has a rotating full-rate probe");
      check(sampled_rt >= 1 && sampled_rt <= 6, "RT confidence is sampled across frames");
    }
  }
  std::printf("ao_rate_test: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
