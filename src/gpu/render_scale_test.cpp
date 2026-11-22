// Check of the render scale's choice (render_scale.h): the number the renderer works at, which it asks again at
// every frame boundary while the game runs (temporal.cpp ReadRenderScale) and moves the images of the screen to
// between two frames (EdramTargets::SetScales). A program of its own: it needs no renderer, no ReXGlue and no GPU,
// and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_render_scale_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_render_scale_test
//
// What it holds: the four places a scale comes from (asked, the screen's, an FSR1 preset, the upscaler's output) and
// the order they win in; the limits and the grid (twentieths); a setting that is not a number; the scene's scale under
// a temporal upscaler (the vendor ratio of each mode) with its floor; and that a live change is a new call of the same
// pure function (the window's size moves the three window-following sources, not the asked one). The expected numbers
// are written out here again on purpose: a change of the rule in the header alone fails here.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "render_scale.h"

using namespace mcla::gpu;

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

void CheckN(uint32_t got, uint32_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %u, want %u\n", what.c_str(), got, want);
  }
}

constexpr auto kAsked = RenderScaleChoice::Source::kAsked;
constexpr auto kScreen = RenderScaleChoice::Source::kScreen;
constexpr auto kPreset = RenderScaleChoice::Source::kPreset;
constexpr auto kUpscaler = RenderScaleChoice::Source::kUpscaler;

// The scale in twentieths of the choice for `asked` (0 = the screen's) in a window.
RenderScaleChoice Pick(double asked, uint32_t w, uint32_t h) {
  return ChooseRenderScale(asked, false, FsrPreset::kCustom, w, h);
}

void PresetNames() {
  const FsrPreset all[] = {FsrPreset::kCustom,  FsrPreset::kUltraQuality, FsrPreset::kQuality,
                           FsrPreset::kBalanced, FsrPreset::kPerformance,  FsrPreset::kUltraPerformance};
  for (FsrPreset p : all) {
    FsrPreset back = FsrPreset::kUltraPerformance;
    Check(ParseFsrPreset(FsrPresetName(p), &back) && back == p, std::string("the preset ") + FsrPresetName(p) + " parses back");
  }
  FsrPreset p = FsrPreset::kBalanced;
  Check(ParseFsrPreset("Ultra-Quality", &p) && p == FsrPreset::kUltraQuality, "a preset in any case with '-' for '_'");
  Check(ParseFsrPreset("ULTRA QUALITY", &p) && p == FsrPreset::kUltraQuality, "... and with ' '");
  p = FsrPreset::kBalanced;
  Check(!ParseFsrPreset("ultra", &p) && p == FsrPreset::kBalanced, "a word that is no preset is refused and leaves *out as it was");
  Check(!ParseFsrPreset("", &p) && p == FsrPreset::kBalanced, "the empty word is refused");
  // AMD's ratios of output to render, per axis (FSR1's four and FSR 2's ultra performance).
  Check(FsrPresetRatio(FsrPreset::kCustom) == 1.0, "custom ratio 1");
  Check(FsrPresetRatio(FsrPreset::kUltraQuality) == 1.3, "ultra quality 1.3");
  Check(FsrPresetRatio(FsrPreset::kQuality) == 1.5, "quality 1.5");
  Check(FsrPresetRatio(FsrPreset::kBalanced) == 1.7, "balanced 1.7");
  Check(FsrPresetRatio(FsrPreset::kPerformance) == 2.0, "performance 2");
  Check(FsrPresetRatio(FsrPreset::kUltraPerformance) == 3.0, "ultra performance 3");
}

void TheScreensScale() {
  // 0 asks for "the screen's": the window's 16:9 area over the game's 1280x720, on the grid of twentieths.
  struct Case {
    uint32_t w, h;
    uint32_t n;  // twentieths
    bool limited;
    uint32_t area_w, area_h;
  };
  const Case cases[] = {
      {1280, 720, 20, false, 1280, 720},     // the game's own size: 1x
      {1920, 1080, 30, false, 1920, 1080},   // 1.5x
      {2560, 1440, 40, false, 2560, 1440},   // 2x
      {3840, 2160, 60, false, 3840, 2160},   // 3x, the top of the grid, not limited
      {5120, 2880, 60, true, 5120, 2880},    // 4x is brought down to 3x
      {640, 360, 10, false, 640, 360},       // 0.5x, the bottom, not limited
      {320, 180, 10, true, 320, 180},        // 0.25x is brought up to 0.5x
      {1366, 768, 21, false, 1365, 768},     // min(1.0672, 1.0667) = 1.0667: 21.33 twentieths, the nearest step
      {2560, 1080, 30, false, 1920, 1080},   // an ultrawide window: the height limits it (the 16:9 area inside)
      {1920, 1200, 30, false, 1920, 1080},   // a 16:10 window: the width limits it
  };
  for (const Case& c : cases) {
    const RenderScaleChoice r = Pick(0.0, c.w, c.h);
    const std::string at = std::to_string(c.w) + "x" + std::to_string(c.h);
    Check(r.source == kScreen, "the screen's scale is the source at " + at);
    Check(r.from_window(), "it follows the window at " + at);
    CheckN(r.scale.n, c.n, "the screen's scale at " + at);
    Check(r.limited == c.limited, "limited flag at " + at);
    CheckN(r.area_width, c.area_w, "the 16:9 area's width at " + at);
    CheckN(r.area_height, c.area_h, "the 16:9 area's height at " + at);
    Check(!r.invalid, "not invalid at " + at);
  }
  // No window yet (the first frames, or a headless run): the game's own screen, 1x.
  const RenderScaleChoice none = Pick(0.0, 0, 0);
  CheckN(none.scale.n, 20, "no window: 1x");
  CheckN(none.area_width, 1280, "no window: the game's width");
  const RenderScaleChoice half = Pick(0.0, 1920, 0);
  CheckN(half.scale.n, 20, "a window with no height counts as no window");
  Check(Pick(0.0, 1366, 768).rounded(), "21.33 twentieths was rounded to 21");
  Check(!Pick(0.0, 1920, 1080).rounded(), "an exact step is not rounded");
  Check(!Pick(0.0, 5120, 2880).rounded(), "a limit is not a rounding");
}

void TheAskedScale() {
  // mcla_gpu_resolution_scale as a number: used as it is, whatever the window is.
  const double asks[] = {0.5, 1.0, 1.35, 2.0, 3.0};
  const uint32_t want[] = {10, 20, 27, 40, 60};
  for (size_t i = 0; i < 5; ++i) {
    const RenderScaleChoice a = Pick(asks[i], 1920, 1080);
    const RenderScaleChoice b = Pick(asks[i], 3840, 2160);
    Check(a.source == kAsked && !a.from_window(), "an asked scale does not come from the window");
    CheckN(a.scale.n, want[i], "asked " + std::to_string(asks[i]) + " in a 1080p window");
    CheckN(b.scale.n, want[i], "asked " + std::to_string(asks[i]) + " in a 4K window: the window does not move it");
    Check(!a.limited && !a.rounded() && !a.invalid, "exact and in range: nothing said about " + std::to_string(asks[i]));
  }
  const RenderScaleChoice near = Pick(1.37, 1920, 1080);  // 27.4 twentieths
  CheckN(near.scale.n, 27, "1.37 takes the nearest step, 1.35");
  Check(near.rounded() && !near.limited, "... and says it rounded");
  const RenderScaleChoice big = Pick(5.0, 1920, 1080);
  CheckN(big.scale.n, 60, "5x is brought down to 3x");
  Check(big.limited, "... and says it was limited");
  const RenderScaleChoice small = Pick(0.1, 1920, 1080);
  CheckN(small.scale.n, 10, "0.1x is brought up to 0.5x");
  Check(small.limited, "... and says it was limited");
  // Not a scale at all: 1x, flagged, whatever the window.
  const double bad[] = {-1.0, -0.001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity()};
  for (double v : bad) {
    const RenderScaleChoice r = Pick(v, 2560, 1440);
    Check(r.invalid, "a scale that is no number is flagged (" + std::to_string(v) + ")");
    CheckN(r.scale.n, 20, "... and is 1x (" + std::to_string(v) + ")");
    Check(!r.rounded(), "an invalid one is not 'rounded'");
  }
}

void ThePresets() {
  struct Case {
    FsrPreset preset;
    uint32_t w, h;
    uint32_t n;
  };
  const Case cases[] = {
      {FsrPreset::kUltraQuality, 1920, 1080, 23},   // 1.5 / 1.3 = 1.154: 23.08 twentieths
      {FsrPreset::kQuality, 1920, 1080, 20},        // 1.5 / 1.5 = 1x: the game's own 1280x720 for a 1080p output
      {FsrPreset::kBalanced, 1920, 1080, 18},       // 1.5 / 1.7 = 0.882: 17.65
      {FsrPreset::kPerformance, 1920, 1080, 15},    // 0.75x
      {FsrPreset::kUltraPerformance, 1920, 1080, 10},  // 0.5x
      {FsrPreset::kQuality, 3840, 2160, 40},        // 3 / 1.5 = 2x
      {FsrPreset::kPerformance, 3840, 2160, 30},    // 1.5x
      {FsrPreset::kUltraPerformance, 3840, 2160, 20},  // 1x
      {FsrPreset::kUltraPerformance, 1280, 720, 10},   // 1 / 3 = 0.333: below the bottom, brought up to 0.5x
  };
  for (const Case& c : cases) {
    const RenderScaleChoice r = ChooseRenderScale(0.0, true, c.preset, c.w, c.h);
    const std::string at = std::string(FsrPresetName(c.preset)) + " at " + std::to_string(c.w) + "x" + std::to_string(c.h);
    Check(r.source == kPreset && r.from_window(), "FSR1 " + at + " comes from the preset");
    CheckN(r.scale.n, c.n, "FSR1 " + at);
  }
  Check(ChooseRenderScale(0.0, true, FsrPreset::kUltraPerformance, 1280, 720).limited, "a preset below the bottom is limited");
  // The preset wins over an asked scale, but only while FSR1 is on and it names a ratio.
  const RenderScaleChoice wins = ChooseRenderScale(2.0, true, FsrPreset::kQuality, 1920, 1080);
  Check(wins.source == kPreset, "the preset wins over the asked scale");
  CheckN(wins.scale.n, 20, "... and the asked 2x is not used");
  CheckN(wins.asked == 2.0 ? 1u : 0u, 1, "... but is kept for the log");
  const RenderScaleChoice off = ChooseRenderScale(2.0, false, FsrPreset::kQuality, 1920, 1080);
  Check(off.source == kAsked, "FSR1 off: the preset waits (the setting is kept)");
  CheckN(off.scale.n, 40, "... and the asked scale stands");
  const RenderScaleChoice off0 = ChooseRenderScale(0.0, false, FsrPreset::kQuality, 1920, 1080);
  Check(off0.source == kScreen, "FSR1 off with the screen's scale: the screen's, the preset still waits");
  const RenderScaleChoice custom = ChooseRenderScale(0.0, true, FsrPreset::kCustom, 1920, 1080);
  Check(custom.source == kScreen, "FSR1 on with the preset `custom`: the render scale decides");
}

void TheUpscalersOutput() {
  // A temporal upscaler on: its output is the screen's scale, whatever was asked, and the settings are kept for the logs.
  const RenderScaleChoice a = ChooseUpscalerOutputScale(2.0, true, FsrPreset::kPerformance, 1920, 1080);
  Check(a.source == kUpscaler && a.from_window(), "the upscaler's output follows the window");
  CheckN(a.scale.n, 30, "the output at 1080p is 1.5x, not the asked 2x or the preset's");
  Check(a.asked == 2.0 && a.fsr && a.preset == FsrPreset::kPerformance, "the settings' own values are kept for the log");
  const RenderScaleChoice b = ChooseUpscalerOutputScale(0.5, false, FsrPreset::kCustom, 3840, 2160);
  CheckN(b.scale.n, 60, "the output at 4K is 3x");
  const RenderScaleChoice c = ChooseUpscalerOutputScale(1.0, false, FsrPreset::kCustom, 7680, 4320);
  CheckN(c.scale.n, 60, "above 4K the output stops at 3x (the present stretches it)");
}

void TheSceneScale() {
  // The 3D scene under an upscaler: the output over the vendor's ratio, on the grid, never below 0.5x.
  struct Case {
    uint32_t output;  // twentieths
    double ratio;
    uint32_t scene;
    bool limited;
  };
  const Case cases[] = {
      {30, 1.0, 30, false},    // native AA / DLAA: drawn at the output's size
      {30, 1.5, 20, false},    // quality at 1080p: 1280x720
      {30, 1.7, 18, false},    // balanced: 17.65
      {30, 2.0, 15, false},    // performance
      {30, 3.0, 10, false},    // ultra performance at 1080p: 0.5x exactly
      {60, 1.5, 40, false},    // quality at 4K
      {60, 1.7, 35, false},    // 35.29
      {60, 2.0, 30, false},
      {60, 3.0, 20, false},
      {60, 1.0 / 0.58, 35, false},  // DLSS balanced (58%): 34.8
      {20, 3.0, 10, true},     // 1x output at ultra performance: 6.67, brought up to 10
      {20, 2.0, 10, false},    // exactly 10
      {40, 0.9, 40, false},    // a ratio at or below 1 leaves the output's scale
      {40, 1.0, 40, false},
      {8, 2.0, 8, true},       // an output already below the floor is not raised above itself (limited: 4 would be below it)
  };
  for (const Case& c : cases) {
    const SceneScaleChoice r = ChooseSceneScale(Scale{c.output}, c.ratio);
    const std::string at = "output " + std::to_string(c.output) + " ratio " + std::to_string(c.ratio);
    CheckN(r.scale.n, c.scene, "scene scale at " + at);
    Check(r.limited == c.limited, "scene scale limited flag at " + at);
  }
  const double odd[] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 0.0, -2.0};
  for (double v : odd) {
    CheckN(ChooseSceneScale(Scale{40}, v).scale.n, 40, "a ratio that is no ratio (" + std::to_string(v) + ") leaves the output's scale");
  }
  const SceneScaleChoice none = ChooseSceneScale(Scale::None(), 2.0);
  Check(none.scale.none() && none.wanted == 1.0, "no output scale: nothing to divide");
  // output over the grid's render size, as the log prints it.
  const SceneScaleChoice q = ChooseSceneScale(Scale{60}, 1.7);
  Check(std::abs(q.actual_ratio(Scale{60}) - 60.0 / 35.0) < 1e-12, "the ratio the grid made: 60 / 35");
  Check(ChooseSceneScale(Scale{30}, 1.0).actual_ratio(Scale{30}) == 1.0, "no division: ratio 1");
  CheckN(SceneScaleChoice{}.scale.n, 20, "a default choice has the unit scale");
  CheckN(kMinSceneScale.n, kMinRenderScale.n, "the scene's floor is the render scale's");
}

void ALiveChangeIsAnotherCall() {
  // The frame boundary asks again with the settings and the window as they are now. The window moves the sources that follow it;
  // a scale that was asked does not move; a change of the setting moves its own source at once.
  // 1080p -> 1440p (a window resize or a fullscreen switch):
  CheckN(Pick(0.0, 1920, 1080).scale.n, 30, "the screen's scale at 1080p");
  CheckN(Pick(0.0, 2560, 1440).scale.n, 40, "... follows the window to 1440p");
  CheckN(ChooseRenderScale(0.0, true, FsrPreset::kQuality, 1920, 1080).scale.n, 20, "FSR1 quality at 1080p");
  CheckN(ChooseRenderScale(0.0, true, FsrPreset::kQuality, 2560, 1440).scale.n, 27, "... follows the window (1.5x of 2560x1440 is 1.333x: 26.67)");
  CheckN(Pick(1.5, 1920, 1080).scale.n, 30, "an asked 1.5x at 1080p");
  CheckN(Pick(1.5, 2560, 1440).scale.n, 30, "... stays 1.5x at 1440p");
  // The row steps from 1x to the screen's to FSR1 and back: each step is a different pure answer, with no state between them.
  const RenderScaleChoice steps[] = {Pick(1.0, 1920, 1080), Pick(0.0, 1920, 1080), ChooseRenderScale(0.0, true, FsrPreset::kBalanced, 1920, 1080),
                                     Pick(1.0, 1920, 1080)};
  CheckN(steps[0].scale.n, 20, "step 1x");
  CheckN(steps[1].scale.n, 30, "step the screen's");
  CheckN(steps[2].scale.n, 18, "step FSR1 balanced");
  CheckN(steps[3].scale.n, steps[0].scale.n, "and back to 1x: the same answer as the first time");
  Check(steps[0].scale != steps[1].scale && steps[1].scale != steps[2].scale, "each step is a scale the frame boundary has to move to");
}

}  // namespace

int main() {
  PresetNames();
  TheScreensScale();
  TheAskedScale();
  ThePresets();
  TheUpscalersOutput();
  TheSceneScale();
  ALiveChangeIsAnotherCall();
  if (g_failed != 0) {
    std::printf("render_scale_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("render_scale_test: %d checks passed\n", g_checks);
  return 0;
}
