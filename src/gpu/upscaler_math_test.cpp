// Check of the temporal upscalers' pure arithmetic (upscaler_math.h): the jitter sequence, how many
// phases it has at a ratio, and the scene's mip bias. A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_upscaler_math_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_upscaler_math_test
//
// The expected numbers are the vendors' own (FSR's ffxFsr3GetJitterPhaseCount and
// ffxFsr3GetJitterOffset, its guide's mip bias) written out again on purpose, so a change of the
// arithmetic in the header alone fails here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "upscaler_math.h"

// With the SDK's include folder (FidelityFX Kits/FidelityFX/upscalers/include) on the path, the lever
// key numbers are also compared with the SDK's own enum; without it the test still builds.
#if __has_include(<ffx_upscale.h>)
#include <ffx_upscale.h>
#endif

using namespace mcla::gpu::upscaler_math;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_sdk_header_seen = false;

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

void CheckEq(int64_t got, int64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %lld, want %lld\n", what.c_str(), (long long)got, (long long)want);
  }
}

// FSR's own count for a display and a render width: (int32_t)(8 x ratio^2) with the ratio a float.
uint32_t FsrCount(uint32_t render_width, uint32_t display_width) {
  const float ratio = float(display_width) / float(render_width);
  return uint32_t(8.0f * std::pow(ratio, 2.0f));
}

void TestHalton() {
  // The first terms of the two bases (radical inverse of 1, 2, 3, ...).
  const double base2[] = {0.5, 0.25, 0.75, 0.125, 0.625, 0.375, 0.875, 0.0625};
  const double base3[] = {1.0 / 3, 2.0 / 3, 1.0 / 9, 4.0 / 9, 7.0 / 9, 2.0 / 9, 5.0 / 9, 8.0 / 9};
  for (uint32_t i = 0; i < 8; ++i) {
    CheckNear(Halton(i + 1, 2), base2[i], 1e-12, "Halton base 2 index " + std::to_string(i + 1));
    CheckNear(Halton(i + 1, 3), base3[i], 1e-12, "Halton base 3 index " + std::to_string(i + 1));
  }
  CheckNear(Halton(0, 2), 0.0, 0.0, "Halton of index 0");
  for (uint32_t i = 1; i < 400; ++i) {
    const double a = Halton(i, 2);
    const double b = Halton(i, 3);
    Check(a >= 0.0 && a < 1.0 && b >= 0.0 && b < 1.0, "Halton stays in [0, 1) at " + std::to_string(i));
  }
}

void TestPhases() {
  // The standard ratios (F4-DESIGN.md 3): 8 x ratio^2.
  CheckEq(JitterPhases(1.0, 0), 8, "ratio 1: 8 phases");
  CheckEq(JitterPhases(1.5, 0), 18, "ratio 1.5 (Quality): 18 phases");
  CheckEq(JitterPhases(2.0, 0), 32, "ratio 2 (Performance): 32 phases, not 33 by rounding noise");
  CheckEq(JitterPhases(3.0, 0), 72, "ratio 3 (Ultra Performance): 72 phases");
  CheckEq(JitterPhases(1.7, 0), 24, "ratio 1.7 (Balanced): 24 phases");
  // The runtime's number wins, within the cap.
  CheckEq(JitterPhases(2.0, 30), 30, "the runtime's 30 is taken");
  CheckEq(JitterPhases(1.0, 500), kMaxPhases, "a runtime count above the cap is cut to it");
  // Odd inputs stay in 1..kMaxPhases.
  CheckEq(JitterPhases(0.0, 0), 1, "ratio 0: one phase");
  CheckEq(JitterPhases(-3.0, 0), 1, "a negative ratio: one phase");
  CheckEq(JitterPhases(100.0, 0), kMaxPhases, "a huge ratio is capped");
  CheckEq(JitterPhases(std::nan(""), 0), 1, "a NaN ratio: one phase");
  // FSR's own answers for the sizes of a 1280x720 and a 1920x1080 window at the four modes: the
  // renderer follows them when the runtime gives them, and its own formula must not be far from them
  // (the formula rounds up, FSR down: one phase at most).
  const struct {
    uint32_t render;
    uint32_t display;
    uint32_t want;
  } kSizes[] = {
      {1280, 1920, 18},  // Quality at 1080p (ratio 1.5)
      {960, 1920, 32},   // Performance at 1080p, render 960x540
      {640, 1920, 72},   // Ultra Performance at 1080p
      {853, 1280, 18},   // Quality at 720p (render 853x480)
      {640, 1280, 32},   // Performance at 720p, render 640x360
      {427, 1280, 71},   // Ultra Performance at 720p (the scale grid does not make it: 427x240)
      {1856, 3648, 30},  // Performance at the owner's 4K window, the first line of its log
  };
  for (const auto& s : kSizes) {
    const std::string name = std::to_string(s.render) + " -> " + std::to_string(s.display);
    const uint32_t fsr = FsrCount(s.render, s.display);
    CheckEq(fsr, s.want, "FSR's count for " + name);
    const double ratio = double(s.display) / double(s.render);
    const uint32_t ours = JitterPhases(ratio, 0);
    Check(ours >= fsr && ours <= fsr + 1, "the fallback formula is within one phase of FSR's for " + name);
    CheckEq(JitterPhases(ratio, fsr), fsr, "the runtime's count is used for " + name);
  }
}

void TestOffsets() {
  for (uint32_t phases : {8u, 18u, 30u, 32u, 72u}) {
    std::set<std::pair<float, float>> seen;
    double sx = 0.0;
    double sy = 0.0;
    bool inside = true;
    for (uint32_t i = 0; i < phases; ++i) {
      float o[2];
      JitterOffset(i, phases, o);
      inside = inside && o[0] > -0.5f && o[0] < 0.5f && o[1] > -0.5f && o[1] < 0.5f;
      seen.insert({o[0], o[1]});
      sx += o[0];
      sy += o[1];
    }
    const std::string p = std::to_string(phases) + " phases";
    Check(inside, p + ": every offset is inside the pixel, (-0.5, 0.5)");
    CheckEq(int64_t(seen.size()), int64_t(phases), p + ": every phase is a position of its own");
    // The sequence is centred: its mean is a small share of a pixel (a biased one would shift the
    // picture). Halton starts at index 1, so a short sequence leans a little (8 phases: -0.055 in x).
    const double lean = phases <= 8 ? 0.06 : 0.04;
    Check(std::fabs(sx / phases) < lean && std::fabs(sy / phases) < lean,
          p + ": the mean offset is within " + std::to_string(lean) + " of the centre");
    // The index wraps by itself: phase i and phase i + phases are the same position.
    float a[2];
    float b[2];
    JitterOffset(3, phases, a);
    JitterOffset(3 + phases * 5, phases, b);
    Check(a[0] == b[0] && a[1] == b[1], p + ": an index wraps modulo the count");
  }
  // The first position (index 0) is Halton(1) - 0.5: (0, -1/6) in render pixels.
  float first[2];
  JitterOffset(0, 32, first);
  CheckNear(first[0], 0.0, 1e-7, "phase 0 x is 0");
  CheckNear(first[1], 1.0 / 3.0 - 0.5, 1e-7, "phase 0 y is -1/6");
  // 0 phases is not a division by zero.
  float zero[2];
  JitterOffset(7, 0, zero);
  Check(std::isfinite(zero[0]) && std::isfinite(zero[1]), "0 phases still gives an offset");

  // What the jitter has to cover at Performance: the render pixel is 2x2 output pixels, and each of
  // those four quarters, and each of the sixteen cells of a 4x4 grid over the pixel, should be visited
  // by the 32 phases (a sequence that leaves a cell empty leaves that part of the picture to the
  // upscaler's guess).
  std::vector<int> quarter(4, 0);
  std::vector<int> cell(16, 0);
  for (uint32_t i = 0; i < 32; ++i) {
    float o[2];
    JitterOffset(i, 32, o);
    const int qx = o[0] < 0.0f ? 0 : 1;
    const int qy = o[1] < 0.0f ? 0 : 1;
    ++quarter[qy * 2 + qx];
    const int cx = std::clamp(int((o[0] + 0.5f) * 4.0f), 0, 3);
    const int cy = std::clamp(int((o[1] + 0.5f) * 4.0f), 0, 3);
    ++cell[cy * 4 + cx];
  }
  for (int q = 0; q < 4; ++q) {
    Check(quarter[q] >= 6, "32 phases visit every output pixel of a render pixel at 2x, quarter " + std::to_string(q));
  }
  for (int c = 0; c < 16; ++c) {
    Check(cell[c] >= 1, "32 phases visit every cell of a 4x4 grid, cell " + std::to_string(c));
  }
}

void TestMipBias() {
  // Performance (render half of the output): log2(1/2) = -1, and the guide's -1 on top.
  CheckEq(SceneMipBias(0.5, 1.0, kRecommendedExtraBias), -64, "Performance: -2 levels");
  CheckEq(SceneMipBias(0.75, 1.5, kRecommendedExtraBias), -64, "Performance, 1080p output (the grid's 0.75 over 1.5): -2 levels");
  CheckEq(SceneMipBias(1.0 / 1.5, 1.0, kRecommendedExtraBias), -51, "Quality: -1.585 levels");
  CheckEq(SceneMipBias(1.0 / 3.0, 1.0, kRecommendedExtraBias), -83, "Ultra Performance: -2.585 levels");
  CheckEq(SceneMipBias(1.0 / 1.7, 1.0, kRecommendedExtraBias), -56, "Balanced: -1.766 levels");
  // The levers: no extra, half a level, and one more than the guide.
  CheckEq(SceneMipBias(0.5, 1.0, 0.0), -32, "Performance with no extra bias: -1 level");
  CheckEq(SceneMipBias(0.5, 1.0, -0.5), -48, "Performance with -0.5 extra: -1.5 levels");
  CheckEq(SceneMipBias(0.5, 1.0, -2.0), -96, "Performance with -2 extra: -3 levels");
  // Only the ratio counts, not the units: 640/1280 and 960/1920 and 0.5/1 are one bias.
  CheckEq(SceneMipBias(640.0, 1280.0, -1.0), SceneMipBias(960.0, 1920.0, -1.0), "the bias depends on the ratio only");
  // The sampler's range: +-3.9 levels (an int8 of 1/32 levels holds +-3.97), whatever is asked.
  CheckEq(SceneMipBias(0.001, 1.0, -1.0), -125, "a tiny scene is cut at -3.9 levels");
  CheckEq(SceneMipBias(1.0, 0.001, 1.0), 125, "a tiny output is cut at +3.9 levels");
  for (double render : {0.25, 0.33, 0.5, 0.667, 0.75, 1.0, 1.5, 3.0}) {
    for (double output : {1.0, 1.5, 2.0, 3.0}) {
      for (double extra : {-3.0, -1.0, 0.0, 1.0}) {
        const int32_t bias = SceneMipBias(render, output, extra);
        Check(bias >= -127 && bias <= 127, "the bias fits the sampler's int8");
      }
    }
  }
  // A scene bigger than its output (it is not drawn that way) is a positive bias, not a crash.
  Check(SceneMipBias(2.0, 1.0, 0.0) == 32, "a scene twice the output: +1 level");
  // Bad inputs give no bias.
  CheckEq(SceneMipBias(0.0, 1.0, -1.0), 0, "no scene: 0");
  CheckEq(SceneMipBias(0.5, 0.0, -1.0), 0, "no output: 0");
  CheckEq(SceneMipBias(-0.5, 1.0, -1.0), 0, "a negative scene: 0");
  CheckEq(SceneMipBias(0.5, 1.0, std::nan("")), 0, "a NaN extra: 0");
  // The vendors' formula, spelled out for each standard ratio: log2(render / display) - 1.
  for (double ratio : {1.33, 1.5, 1.7, 2.0, 3.0}) {
    const double want = std::log2(1.0 / ratio) - 1.0;
    CheckNear(double(SceneMipBias(1.0 / ratio, 1.0, kRecommendedExtraBias)) / 32.0, want, 1.0 / 64.0 + 1e-9,
              "the guide's log2(render / display) - 1 at ratio " + std::to_string(ratio));
  }
}

// mcla_gpu_upscaler_zero_jitter: the one jitter value of a frame, taken to the draws' projection offset
// and to the upscaler, is 0 for both, and unchanged with the lever off.
void TestZeroJitter() {
  for (uint32_t phases : {8u, 18u, 32u, 72u}) {
    for (uint32_t i = 0; i < phases; ++i) {
      float jitter[2];
      JitterOffset(i, phases, jitter);
      const float original[2] = {jitter[0], jitter[1]};

      // Off: nothing changes (today's behaviour), and the projection moves by the formula the draws
      // always used (2 j / size, y flipped).
      float off[2] = {jitter[0], jitter[1]};
      ApplyZeroJitter(false, off);
      Check(off[0] == original[0] && off[1] == original[1], "zero jitter off leaves the jitter as it was");
      float ndc_off[2];
      JitterNdcOffset(off, 1920.0f, 1080.0f, ndc_off);
      Check(ndc_off[0] == 2.0f * original[0] / 1920.0f && ndc_off[1] == -(2.0f * original[1] / 1080.0f),
            "the projection offset is 2 j / size with y flipped");

      // On: the value the upscaler is told and the value the projection is moved by are both 0.
      float on[2] = {jitter[0], jitter[1]};
      ApplyZeroJitter(true, on);
      float ndc_on[2];
      JitterNdcOffset(on, 1920.0f, 1080.0f, ndc_on);
      Check(on[0] == 0.0f && on[1] == 0.0f && !std::signbit(on[0]) && !std::signbit(on[1]),
            "zero jitter on: the upscaler's jitterOffset is exactly +0, +0");
      Check(ndc_on[0] == 0.0f && ndc_on[1] == 0.0f,
            "zero jitter on: the projection offset is 0 too, the two agree");
    }
  }
  // A jitter that is already 0 (a held or plain frame) stays 0 either way.
  float held[2] = {0.0f, 0.0f};
  ApplyZeroJitter(true, held);
  Check(held[0] == 0.0f && held[1] == 0.0f, "a frame with no jitter stays at 0");
  // The offset is linear in the jitter and in 1 / size (a viewport twice as wide halves it).
  const float j[2] = {0.25f, -0.375f};
  float a[2];
  float b[2];
  JitterNdcOffset(j, 1000.0f, 500.0f, a);
  JitterNdcOffset(j, 2000.0f, 1000.0f, b);
  CheckNear(a[0], 2.0 * 0.25 / 1000.0, 1e-9, "x offset: 2 jx / width");
  CheckNear(a[1], -(2.0 * -0.375 / 500.0), 1e-9, "y offset: -2 jy / height (+y down becomes NDC +y up)");
  CheckNear(b[0], a[0] / 2.0, 1e-9, "twice the width, half the offset");
  CheckNear(b[1], a[1] / 2.0, 1e-9, "twice the height, half the offset");
}

// mcla_gpu_upscaler_post_reads_output: the post chain's stand-in is wanted at every ratio above 1, at
// ratio 1 only with the lever.
void TestPostChainReadsOutput() {
  struct Sizes {
    uint32_t rw, rh, ow, oh;
    const char* name;
  };
  const Sizes kScaled[] = {{1280, 720, 1920, 1080, "Quality 1080p"},  {960, 540, 1920, 1080, "Performance 1080p"},
                           {2427, 1365, 3640, 2048, "Quality 4K"},    {640, 360, 1920, 1080, "Ultra Performance 1080p"},
                           {1919, 1080, 1920, 1080, "one pixel short in x"}, {1920, 1079, 1920, 1080, "one pixel short in y"}};
  for (const Sizes& s : kScaled) {
    Check(PostChainReadsOutput(s.rw, s.rh, s.ow, s.oh, false), std::string("below ratio 1, lever off: stand-in (") + s.name + ")");
    Check(PostChainReadsOutput(s.rw, s.rh, s.ow, s.oh, true), std::string("below ratio 1, lever on: stand-in (") + s.name + ")");
  }
  const Sizes kNative[] = {{1920, 1080, 1920, 1080, "Native AA / DLAA 1080p"}, {3640, 2048, 3640, 2048, "4K"}, {1280, 720, 1280, 720, "720p"}};
  for (const Sizes& s : kNative) {
    Check(!PostChainReadsOutput(s.rw, s.rh, s.ow, s.oh, false), std::string("ratio 1, lever off: today's early return (") + s.name + ")");
    Check(PostChainReadsOutput(s.rw, s.rh, s.ow, s.oh, true), std::string("ratio 1, lever on: stand-in (") + s.name + ")");
  }
  // No sizes known yet (0 and 0) count as equal, as the early return always read them.
  Check(!PostChainReadsOutput(0, 0, 0, 0, false), "no sizes, lever off: returns early as before");
  Check(PostChainReadsOutput(0, 0, 0, 0, true), "no sizes, lever on: not the early return");
}

// The tuning-constant levers: the key numbers, what asks for a lever, the clamps and the plan for a context.
void TestFsrLevers() {
  using mcla::gpu::upscaler_math::FsrLever;
  // The keys of FidelityFX's FfxApiConfigureUpscaleKey (ffx_upscale.h): 0 velocity factor, 2 shading
  // change scale, 4 minimum disocclusion accumulation. Written out here on purpose; with the SDK's
  // header on the include path they are compared with its enum too.
  CheckEq(int64_t(FsrLeverKey(FsrLever::kVelocityFactor)), 0, "key of fVelocityFactor");
  CheckEq(int64_t(FsrLeverKey(FsrLever::kShadingChangeScale)), 2, "key of fShadingChangeScale");
  CheckEq(int64_t(FsrLeverKey(FsrLever::kMinDisocclusionAccumulation)), 4, "key of fMinDisocclusionAccumulation");
#if __has_include(<ffx_upscale.h>)
  CheckEq(int64_t(FsrLeverKey(FsrLever::kVelocityFactor)), int64_t(FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR),
          "the SDK's FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR");
  CheckEq(int64_t(FsrLeverKey(FsrLever::kShadingChangeScale)), int64_t(FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE),
          "the SDK's FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE");
  CheckEq(int64_t(FsrLeverKey(FsrLever::kMinDisocclusionAccumulation)),
          int64_t(FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION),
          "the SDK's FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION");
  g_sdk_header_seen = true;
#endif
  // The three keys are distinct and none is the ones the levers leave alone (1 reactiveness scale, 3
  // accumulation added per frame).
  Check(FsrLeverKey(FsrLever(0)) != FsrLeverKey(FsrLever(1)) && FsrLeverKey(FsrLever(1)) != FsrLeverKey(FsrLever(2)) &&
            FsrLeverKey(FsrLever(0)) != FsrLeverKey(FsrLever(2)),
        "the three keys differ");
  for (uint32_t k = 0; k < kFsrLeverCount; ++k) {
    const uint64_t key = FsrLeverKey(FsrLever(k));
    Check(key != 1 && key != 3, "no lever touches reactiveness scale or accumulation added per frame");
  }
  // The cvar names (the test chain is written against them).
  Check(std::string(FsrLeverCvar(FsrLever::kVelocityFactor)) == "mcla_gpu_fsr_velocity_factor", "cvar of lever 0");
  Check(std::string(FsrLeverCvar(FsrLever::kShadingChangeScale)) == "mcla_gpu_fsr_shading_change_scale", "cvar of lever 1");
  Check(std::string(FsrLeverCvar(FsrLever::kMinDisocclusionAccumulation)) == "mcla_gpu_fsr_min_disocclusion_accumulation",
        "cvar of lever 2");
  // FSR's own defaults.
  CheckNear(FsrLeverDefault(FsrLever::kVelocityFactor), 1.0, 0.0, "default fVelocityFactor 1");
  CheckNear(FsrLeverDefault(FsrLever::kShadingChangeScale), 1.0, 0.0, "default fShadingChangeScale 1");
  CheckNear(FsrLeverDefault(FsrLever::kMinDisocclusionAccumulation), -1.0 / 3.0, 1e-7, "default fMinDisocclusionAccumulation -1/3");

  // What asks for a lever: 0 or more. -1 (the default) and anything else negative or not a number: no.
  Check(!FsrLeverAsked(-1.0), "-1 is not set");
  Check(!FsrLeverAsked(-0.0001), "any negative value is not set");
  Check(FsrLeverAsked(0.0), "0 is set");
  Check(FsrLeverAsked(0.5), "0.5 is set");
  Check(!FsrLeverAsked(std::nan("")), "NaN is not set");
  Check(!FsrLeverAsked(HUGE_VAL), "infinity is not set");
  // The clamps FSR applies anyway ([0, 1], [0, inf), [-1, 1]).
  CheckNear(FsrLeverValue(FsrLever::kVelocityFactor, 0.0), 0.0, 0.0, "velocity factor 0 passes as 0");
  CheckNear(FsrLeverValue(FsrLever::kVelocityFactor, 0.3), 0.3, 1e-7, "velocity factor 0.3");
  CheckNear(FsrLeverValue(FsrLever::kVelocityFactor, 7.0), 1.0, 0.0, "velocity factor above 1 is cut to 1");
  CheckNear(FsrLeverValue(FsrLever::kShadingChangeScale, 4.0), 4.0, 0.0, "shading change scale 4");
  CheckNear(FsrLeverValue(FsrLever::kShadingChangeScale, 100.0), 100.0, 0.0, "shading change scale has no upper bound");
  CheckNear(FsrLeverValue(FsrLever::kMinDisocclusionAccumulation, 0.25), 0.25, 0.0, "minimum disocclusion accumulation 0.25");
  CheckNear(FsrLeverValue(FsrLever::kMinDisocclusionAccumulation, 3.0), 1.0, 0.0, "minimum disocclusion accumulation above 1 is cut to 1");

  // The plan for a context. Nothing asked: nothing sent, whatever the number of frames.
  FsrLeverState state;
  const double none[3] = {-1.0, -1.0, -1.0};
  for (int frame = 0; frame < 5; ++frame) {
    CheckEq(PlanFsrLevers(none, &state).mask, 0, "nothing asked: nothing sent");
  }
  // One lever on: sent once, then not again while it stands.
  const double velocity0[3] = {0.0, -1.0, -1.0};
  FsrLeverPlan plan = PlanFsrLevers(velocity0, &state);
  CheckEq(plan.mask, 1, "velocity factor 0 asked: only lever 0 sent");
  CheckNear(plan.value[0], 0.0, 0.0, "... with the value 0");
  for (int frame = 0; frame < 5; ++frame) {
    CheckEq(PlanFsrLevers(velocity0, &state).mask, 0, "the same value is not sent again");
  }
  // A change of value sends it again; another lever added sends only that one.
  const double velocity5[3] = {0.5, -1.0, -1.0};
  plan = PlanFsrLevers(velocity5, &state);
  CheckEq(plan.mask, 1, "velocity factor 0 -> 0.5: lever 0 sent again");
  CheckNear(plan.value[0], 0.5, 0.0, "... with the value 0.5");
  const double both[3] = {0.5, 2.0, -1.0};
  plan = PlanFsrLevers(both, &state);
  CheckEq(plan.mask, 2, "shading change scale added: only lever 1 sent");
  CheckNear(plan.value[1], 2.0, 0.0, "... with the value 2");
  const double all[3] = {0.5, 2.0, 0.25};
  plan = PlanFsrLevers(all, &state);
  CheckEq(plan.mask, 4, "minimum disocclusion accumulation added: only lever 2 sent");
  CheckNear(plan.value[2], 0.25, 0.0, "... with the value 0.25");
  // A value cut to the same clamped number is not a change (velocity factor 3 is 1; 5 is 1 too).
  const double over1[3] = {3.0, 2.0, 0.25};
  CheckEq(PlanFsrLevers(over1, &state).mask, 1, "velocity factor 0.5 -> 3 (cut to 1): sent");
  const double over2[3] = {5.0, 2.0, 0.25};
  CheckEq(PlanFsrLevers(over2, &state).mask, 0, "velocity factor 3 -> 5 (both cut to 1): not sent again");
  // Back to "not set": FSR's own default goes to the context, once.
  const double back[3] = {-1.0, 2.0, -1.0};
  plan = PlanFsrLevers(back, &state);
  CheckEq(plan.mask, 1 | 4, "levers 0 and 2 back to not set: both are given FSR's default");
  CheckNear(plan.value[0], 1.0, 0.0, "... the velocity factor's 1");
  CheckNear(plan.value[2], -1.0 / 3.0, 1e-7, "... the minimum disocclusion accumulation's -1/3");
  CheckEq(PlanFsrLevers(back, &state).mask, 0, "and not sent again");
  // A new context (state of its own) with levers standing gets them at its first dispatch.
  FsrLeverState fresh;
  plan = PlanFsrLevers(all, &fresh);
  CheckEq(plan.mask, 7, "a new context is given every lever that is set");
  FsrLeverState fresh_none;
  CheckEq(PlanFsrLevers(none, &fresh_none).mask, 0, "a new context with none set is left at FSR's defaults");
}

}  // namespace

int main() {
  TestHalton();
  TestPhases();
  TestOffsets();
  TestMipBias();
  TestZeroJitter();
  TestPostChainReadsOutput();
  TestFsrLevers();
  std::printf("upscaler math: the SDK's ffx_upscale.h on the include path: %s\n",
              g_sdk_header_seen ? "yes (the lever keys were compared with its enum)"
                                : "no (the lever keys were checked against literals only)");
  if (g_failed != 0) {
    std::printf("upscaler math: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("upscaler math: all %d checks passed\n", g_checks);
  return 0;
}
