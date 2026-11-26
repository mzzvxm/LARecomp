// Check of the masks' decisions and arithmetic that need no device (upscaler_mask_plan.h): which main
// views get second draws and with how many samples, and the mask pass as a function (the single sample
// path and the mean over the samples of a multisampled coverage image). A program of its own: it needs
// no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_upscaler_masks_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_upscaler_masks_test

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "upscaler_mask_plan.h"

using namespace mcla::gpu::upscaler_mask_plan;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckNear(double got, double want, double tolerance, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tolerance)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
  }
}

constexpr float kReactiveGain = 1.0f;       // UpscalerMasks::kReactiveGain
constexpr float kAddedLightGain = 1.0f;     // UpscalerMasks::kAddedLightGain
constexpr float kTransparencyGain = 4.0f;   // UpscalerMasks::kTransparencyGain

void TestViews() {
  // One sample: always (today's path, whatever the lever says).
  Check(ForView(1, false) == Verdict::kDraw, "1 sample, lever off: drawn");
  Check(ForView(1, true) == Verdict::kDraw, "1 sample, lever on: drawn");
  // Samples with the lever on: drawn into a coverage image of the same count.
  for (uint32_t n : {2u, 4u, 8u}) {
    Check(ForView(n, true) == Verdict::kDraw, std::to_string(n) + " samples, lever on: drawn");
    Check(ForView(n, false) == Verdict::kMultisampled,
          std::to_string(n) + " samples, lever off: left out as multisampled");
  }
  // A count no target has.
  for (uint32_t n : {0u, 3u, 5u, 6u, 7u, 16u}) {
    Check(ForView(n, true) == Verdict::kBadSamples, std::to_string(n) + " samples: not a count");
    Check(ForView(n, false) == Verdict::kBadSamples,
          std::to_string(n) + " samples, lever off: not a count");
  }
  // The framebuffer takes one count.
  Check(FramebufferAgrees(4, 4), "depth 4, coverage 4: agree");
  Check(FramebufferAgrees(1, 1), "depth 1, coverage 1: agree");
  Check(!FramebufferAgrees(4, 1), "depth 4, coverage 1: not (the old coverage image)");
  Check(!FramebufferAgrees(1, 4), "depth 1, coverage 4: not (the level just fell)");
  Check(!FramebufferAgrees(0, 0), "no depth: not");
}

void TestMask() {
  // An opaque layer, all samples covered: 1 at any count, in both layers.
  const Layers opaque{0, 0, 0, 1};
  for (uint32_t n : {1u, 2u, 4u, 8u}) {
    Layers all[8];
    for (uint32_t i = 0; i < n; ++i) all[i] = opaque;
    CheckNear(MeanMask(all, n, kReactiveGain, kAddedLightGain), 1.0, 1e-7,
              "opaque layer, " + std::to_string(n) + " samples: 1");
  }
  // Nothing drawn: 0.
  Layers none[8];
  CheckNear(MeanMask(none, 8, kReactiveGain, kAddedLightGain), 0.0, 0.0, "nothing: 0");
  // The single sample path is the old shader: max(a * gain, brightest * color_gain), saturated.
  {
    const Layers l{0.2f, 0.5f, 0.1f, 0.3f};
    CheckNear(MeanMask(&l, 1, kReactiveGain, kAddedLightGain), 0.5, 1e-7, "1 sample: the colour wins");
    const Layers g{0.0f, 0.0f, 0.0f, 0.3f};
    CheckNear(MeanMask(&g, 1, kTransparencyGain, kAddedLightGain), 1.0, 1e-7,
              "1 sample: glass of alpha 0.3 at gain 4 saturates");
    const Layers f{0.0f, 0.0f, 0.0f, 0.1f};
    CheckNear(MeanMask(&f, 1, kTransparencyGain, kAddedLightGain), 0.4, 1e-7,
              "1 sample: glass of alpha 0.1 at gain 4 is 0.4");
  }
  // The same layer in every sample is the single sample result (a layer inside a polygon).
  {
    const Layers l{0.0f, 0.0f, 0.0f, 0.1f};
    Layers s[8];
    for (uint32_t i = 0; i < 8; ++i) s[i] = l;
    for (uint32_t n : {2u, 4u, 8u}) {
      CheckNear(MeanMask(s, n, kTransparencyGain, kAddedLightGain),
                MeanMask(&l, 1, kTransparencyGain, kAddedLightGain), 1e-7,
                "uniform glass, " + std::to_string(n) + " samples = the 1 sample mask");
    }
  }
  // An edge: half the samples covered by glass of alpha 0.3. The mask is the share of samples (0.5);
  // the mask of the mean alpha (0.15 * 4) would be 0.6.
  {
    Layers s[4] = {{0, 0, 0, 0.3f}, {0, 0, 0, 0.3f}, {}, {}};
    CheckNear(MeanMask(s, 4, kTransparencyGain, kAddedLightGain), 0.5, 1e-7,
              "glass edge, 2 of 4 samples: 0.5");
    CheckNear(MaskOfMean(s, 4, kTransparencyGain, kAddedLightGain), 0.6, 1e-6,
              "the mean first would give 0.6 (the reason the pass does not)");
  }
  // A particle edge: one sample of 8 covered by an opaque particle.
  {
    Layers s[8] = {opaque};
    CheckNear(MeanMask(s, 8, kReactiveGain, kAddedLightGain), 0.125, 1e-7,
              "particle edge, 1 of 8 samples: 0.125");
  }
  // The added colour counts per sample too, and rgb of a sample is the larger channel.
  {
    Layers s[2] = {{0.8f, 0.2f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    CheckNear(MeanMask(s, 2, kReactiveGain, kAddedLightGain), 0.4, 1e-7,
              "added light in 1 of 2 samples: 0.4");
  }
  // Values beyond 1 (a gain over 1) saturate per sample and the mean never exceeds 1.
  {
    Layers s[4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0.9f}};
    CheckNear(MeanMask(s, 4, kTransparencyGain, kAddedLightGain), 1.0, 1e-7,
              "saturated samples: 1, not more");
  }
  // A sample count of 0 reads as 1 (the pass divides by max(count, 1)).
  {
    const Layers l{0, 0, 0, 0.5f};
    CheckNear(MeanMask(&l, 0, kReactiveGain, kAddedLightGain), 0.5, 1e-7, "0 samples read as 1");
  }
}

void TestResolvePlan() {
  for (uint32_t draws = 0; draws < 4; ++draws) {
    for (uint32_t sizes = 0; sizes < 4; ++sizes) {
      for (uint32_t forced = 0; forced < 4; ++forced) {
        const uint32_t expected = (draws & sizes) | forced;
        bool resolves[2];
        for (uint32_t layer = 0; layer < 2; ++layer) {
          const uint32_t bit = 1u << layer;
          const auto source = SourceFor((draws & bit) != 0, (sizes & bit) != 0,
                                        (forced & bit) != 0);
          resolves[layer] = Resolves(source);
          Check(resolves[layer] == ((expected & bit) != 0), "resolve only a valid layer");
          Check((source == ResolveSource::kOnes) == ((forced & bit) != 0),
                "the diagnostic overrides coverage and size");
          Check((source == ResolveSource::kWrongSize) ==
                    ((draws & ~sizes & ~forced & bit) != 0),
                "a drawn layer of the wrong size is rejected");
        }
        Check(FuseResolves(true, resolves[0], resolves[1]) == (expected == 3),
              "MRT only when both layers resolve");
        Check(!FuseResolves(false, resolves[0], resolves[1]), "A/B off uses separate resolves");
      }
    }
  }
}

void TestPairArithmetic() {
  // The dual resolve has independent inputs, including different sample counts on a transition or
  // when one layer is forced. Compare the pair with the old single-layer formula bit for bit.
  const Layers reactive[8] = {{0.9f, 0.1f, 0.3f, 0.1f}, {0.1f, 0.8f, 0.0f, 0.1f},
                              {}, {}, {0.3f, 0.1f, 0.85f, 0.2f}};
  const Layers transparency[8] = {{0.1f, 0.2f, 0.0f, 0.15f}, {0.3f, 0.1f, 0.2f, 0.3f},
                                  {}, {}, {0.1f, 0.4f, 0.0f, 0.2f}};
  const auto separate = [](const Layers* samples, uint32_t count, float gain) {
    if (count == 0) {
      return 1.0f;
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
      const Layers& l = samples[i];
      sum += std::clamp(std::max({l.a * gain, l.r, l.g, l.b}), 0.0f, 1.0f);
    }
    return sum / float(count);
  };
  for (uint32_t r : {0u, 1u, 2u, 4u, 8u}) {
    for (uint32_t t : {0u, 1u, 2u, 4u, 8u}) {
      const float first = r ? MeanMask(reactive, r, kReactiveGain, kAddedLightGain) : 1.0f;
      const float second =
          t ? MeanMask(transparency, t, kTransparencyGain, kAddedLightGain) : 1.0f;
      Check(std::bit_cast<uint32_t>(first) ==
                std::bit_cast<uint32_t>(separate(reactive, r, kReactiveGain)),
            "reactive target keeps its own arithmetic");
      Check(std::bit_cast<uint32_t>(second) ==
                std::bit_cast<uint32_t>(separate(transparency, t, kTransparencyGain)),
            "transparency target keeps its own arithmetic");
    }
  }
  // Recording only each draw's brightest channel loses the order of max and sum. Two additions
  // in different channels are a concrete counterexample to packing one scalar per coverage layer.
  const Layers channel_sum{0.6f, 0.6f, 0.0f, 0.0f};
  Check(MaskOf(channel_sum, 1.0f, 1.0f) == 0.6f && Saturate(0.6f + 0.6f) == 1.0f,
        "independent RGB sums cannot be replaced with a sum of per-draw maxima");
}

}  // namespace

// How the mask images are stored: 8 bits on D3D12, half floats on Vulkan (Streamline's Vulkan back end has no format of its own for R8_UNORM and logs "Cannot have undefined format" four times a frame
// for the reactive mask it is tagged with). The mask's values are in [0, 1] and a half float holds them to 11 bits near 1: the same mask to within the 8 bit step.
void TestStorage() {
  Check(StorageFor(true) == Storage::kR16Float, "Vulkan: the masks are half floats");
  Check(StorageFor(false) == Storage::kR8Unorm, "D3D12: the masks stay 8 bits");
  // A half float (1 sign, 5 exponent, 10 mantissa bits) keeps the 8 bit mask's 256 levels apart: the step of the 8 bit form is 1/255, a half float's spacing in [0.5, 1] is 2^-11.
  const double eight_bit_step = 1.0 / 255.0;
  const double half_step_near_one = 1.0 / 2048.0;
  Check(half_step_near_one < eight_bit_step / 2.0, "a half float resolves finer than the 8 bit step everywhere in [0, 1]");
  // The mean mask over 8 samples is a multiple of 1/8: exact in both forms.
  Layers s[8] = {};
  for (int i = 0; i < 3; ++i) s[i] = Layers{0, 0, 0, 1.0f};
  CheckNear(MeanMask(s, 8, kTransparencyGain, kAddedLightGain), 0.375, 1e-7, "3 of 8 samples: 0.375, a value a half float holds exactly");
}

int main() {
  TestViews();
  TestMask();
  TestResolvePlan();
  TestPairArithmetic();
  TestStorage();
  if (g_failed != 0) {
    std::printf("upscaler masks: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("upscaler masks: all %d checks passed\n", g_checks);
  return 0;
}
