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

using namespace mcla::gpu::upscaler_math;

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

}  // namespace

int main() {
  TestHalton();
  TestPhases();
  TestOffsets();
  TestMipBias();
  if (g_failed != 0) {
    std::printf("upscaler math: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("upscaler math: all %d checks passed\n", g_checks);
  return 0;
}
