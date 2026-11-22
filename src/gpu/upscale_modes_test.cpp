// Check of the upscalers' words, mode tables and ratios (upscale_modes.h): the settings the temporal system reads at every frame boundary
// (mcla_gpu_upscaler and the three mode settings) and the scene's size each mode gives. A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_upscale_modes_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_upscale_modes_test
//
// The expected values are the vendors' own (AMD's ratios 1, 1.5, 1.7, 2, 3; NVIDIA's DLSS sizes: quality 66.7%, balanced 58%, performance
// 50%, ultra performance 33.3%), written out here again on purpose, so a change in the header alone fails here. The scene's size is
// render_scale.h's ChooseSceneScale of the output's scale and the mode's ratio, on the grid of twentieths with a floor of 0.5x; the
// rows below are what a live change of the mode moves the scene's images to at 720p, 1080p, 1440p and 4K.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "render_scale.h"
#include "upscale_modes.h"

using namespace mcla::gpu;
namespace um = mcla::gpu::upscale_modes;

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

void CheckN(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got, (unsigned long long)want);
  }
}

const UpscaleMode kModesInOrder[] = {UpscaleMode::kNativeAa, UpscaleMode::kQuality, UpscaleMode::kBalanced, UpscaleMode::kPerformance,
                                     UpscaleMode::kUltraPerformance};

void TheTables() {
  const char* words[] = {"native_aa", "quality", "balanced", "performance", "ultra_performance"};
  const char* dlss_words[] = {"dlaa", "quality", "balanced", "performance", "ultra_performance"};
  const char* names[] = {"NATIVE AA", "QUALITY", "BALANCED", "PERFORMANCE", "ULTRA PERFORMANCE"};
  const char* dlss_names[] = {"DLAA", "QUALITY", "BALANCED", "PERFORMANCE", "ULTRA PERFORMANCE"};
  const uint32_t percent[] = {100, 67, 59, 50, 33};
  const uint32_t dlss_percent[] = {100, 67, 58, 50, 33};
  const double ratio[] = {1.0, 1.5, 1.7, 2.0, 3.0};
  const double dlss_ratio[] = {1.0, 1.5, 1.0 / 0.58, 2.0, 3.0};
  for (size_t i = 0; i < 5; ++i) {
    const UpscaleMode m = kModesInOrder[i];
    const ModeInfo& f = um::InfoOf(Upscaler::kFsr31, m);
    const ModeInfo& f4 = um::InfoOf(Upscaler::kFsr4, m);
    const ModeInfo& off = um::InfoOf(Upscaler::kOff, m);
    const ModeInfo& d = um::InfoOf(Upscaler::kDlss, m);
    const std::string at = std::string(" for ") + words[i];
    Check(std::string(f.cvar) == words[i] && std::string(f.name) == names[i], "FSR 3.1 word and name" + at);
    CheckN(f.percent, percent[i], "FSR 3.1 percent" + at);
    Check(f.ratio == ratio[i], "FSR 3.1 ratio" + at);
    Check(&f4 == &f && &off == &f, "FSR 4 and `off` take FSR's table" + at);
    Check(std::string(d.cvar) == dlss_words[i] && std::string(d.name) == dlss_names[i], "DLSS word and name" + at);
    CheckN(d.percent, dlss_percent[i], "DLSS percent" + at);
    Check(std::abs(d.ratio - dlss_ratio[i]) < 1e-12, "DLSS ratio" + at);
    // The percent shown in the menu is the render size's share of the output, from the ratio.
    CheckN(uint64_t(std::lround(100.0 / f.ratio)), f.percent, "FSR 3.1 percent is 100 / ratio" + at);
    CheckN(uint64_t(std::lround(100.0 / d.ratio)), d.percent, "DLSS percent is 100 / ratio" + at);
  }
  Check(&um::InfoOf(Upscaler::kFsr31, UpscaleMode::kCount) == &um::InfoOf(Upscaler::kFsr31, UpscaleMode::kNativeAa),
        "a mode out of range reads the first row, not memory beyond the table");
  Check(&um::InfoOf(Upscaler::kDlss, UpscaleMode(200)) == &um::InfoOf(Upscaler::kDlss, UpscaleMode::kNativeAa),
        "... in DLSS's table too");
}

void TheUpscalerWords() {
  Upscaler u = Upscaler::kDlss;
  Check(um::ParseUpscaler("off", &u) && u == Upscaler::kOff, "off");
  u = Upscaler::kDlss;
  Check(um::ParseUpscaler("", &u) && u == Upscaler::kOff, "the empty word is off");
  Check(um::ParseUpscaler("OFF", &u) && u == Upscaler::kOff, "OFF");
  Check(um::ParseUpscaler("fsr31", &u) && u == Upscaler::kFsr31, "fsr31");
  Check(um::ParseUpscaler("FSR3.1", &u) && u == Upscaler::kFsr31, "FSR3.1");
  Check(um::ParseUpscaler("fsr 3.1", &u) && u == Upscaler::kFsr31, "fsr 3.1");
  Check(um::ParseUpscaler("fsr4", &u) && u == Upscaler::kFsr4, "fsr4");
  Check(um::ParseUpscaler("Fsr 4", &u) && u == Upscaler::kFsr4, "Fsr 4");
  Check(um::ParseUpscaler("dlss", &u) && u == Upscaler::kDlss, "dlss");
  Check(um::ParseUpscaler("DLSS", &u) && u == Upscaler::kDlss, "DLSS");
  const char* bad[] = {"fsr", "fsr3", "fsr 5", "xess", "on", "dlss4", "auto"};
  for (const char* w : bad) {
    u = Upscaler::kDlss;
    Check(!um::ParseUpscaler(w, &u) && u == Upscaler::kOff, std::string("'") + w + "' is refused and reads as off");
  }
  // The word a choice is saved as, and back.
  const Upscaler all[] = {Upscaler::kOff, Upscaler::kFsr31, Upscaler::kFsr4, Upscaler::kDlss};
  const char* shown[] = {"OFF", "FSR 3.1", "FSR 4", "DLSS"};
  for (size_t i = 0; i < 4; ++i) {
    Upscaler back = Upscaler::kFsr4;
    Check(um::ParseUpscaler(um::CvarValue(all[i]), &back) && back == all[i], std::string("the saved word of ") + shown[i] + " parses back");
    Check(std::string(um::Name(all[i])) == shown[i], std::string("the name of ") + shown[i]);
  }
}

void TheModeWords() {
  UpscaleMode m = UpscaleMode::kPerformance;
  Check(um::ParseMode("native_aa", &m) && m == UpscaleMode::kNativeAa, "native_aa");
  m = UpscaleMode::kPerformance;
  Check(um::ParseMode("dlaa", &m) && m == UpscaleMode::kNativeAa, "dlaa is DLSS's word for the first mode");
  Check(um::ParseMode("DLAA", &m) && m == UpscaleMode::kNativeAa, "DLAA");
  Check(um::ParseMode("Quality", &m) && m == UpscaleMode::kQuality, "Quality");
  Check(um::ParseMode("balanced", &m) && m == UpscaleMode::kBalanced, "balanced");
  Check(um::ParseMode("PERFORMANCE", &m) && m == UpscaleMode::kPerformance, "PERFORMANCE");
  Check(um::ParseMode("ultra_performance", &m) && m == UpscaleMode::kUltraPerformance, "ultra_performance");
  const char* bad[] = {"", "ultra performance", "ultra-performance", "ultra", "native", "auto", "max"};
  for (const char* w : bad) {
    m = UpscaleMode::kPerformance;
    Check(!um::ParseMode(w, &m) && m == UpscaleMode::kNativeAa, std::string("'") + w + "' is refused and reads as Native AA");
  }
  // Every row of FSR's table parses to its own mode: the setting's words are the table's.
  for (size_t i = 0; i < 5; ++i) {
    UpscaleMode back = UpscaleMode::kCount;
    Check(um::ParseMode(um::kFsrModes[i].cvar, &back) && back == kModesInOrder[i], std::string("the word '") + um::kFsrModes[i].cvar + "' parses to its row");
  }
}

void WhichModesRun() {
  for (UpscaleMode m : kModesInOrder) {
    Check(!um::Runs(Upscaler::kOff, m), "no upscaler: no mode runs");
    Check(um::Runs(Upscaler::kFsr31, m) && um::Runs(Upscaler::kFsr4, m) && um::Runs(Upscaler::kDlss, m), "each upscaler takes the five modes");
  }
  Check(!um::Runs(Upscaler::kFsr31, UpscaleMode::kCount), "a mode out of range does not run");
}

void TheSceneSizeOfEachMode() {
  // The scene is drawn at ChooseSceneScale(output, ratio): twentieths, nearest step, never below 0.5x.
  struct Output {
    const char* name;
    uint32_t n;
    uint32_t fsr[5];
    uint32_t dlss[5];
  };
  const Output outputs[] = {
      {"1080p (1.5x)", 30, {30, 20, 18, 15, 10}, {30, 20, 17, 15, 10}},
      {"1440p (2x)", 40, {40, 27, 24, 20, 13}, {40, 27, 23, 20, 13}},
      {"4K (3x)", 60, {60, 40, 35, 30, 20}, {60, 40, 35, 30, 20}},
      {"720p (1x)", 20, {20, 13, 12, 10, 10}, {20, 13, 12, 10, 10}},  // ultra performance 6.67 is below the floor: brought up to 10
  };
  for (const Output& o : outputs) {
    for (size_t i = 0; i < 5; ++i) {
      const SceneScaleChoice f = ChooseSceneScale(Scale{o.n}, um::InfoOf(Upscaler::kFsr31, kModesInOrder[i]).ratio);
      const SceneScaleChoice d = ChooseSceneScale(Scale{o.n}, um::InfoOf(Upscaler::kDlss, kModesInOrder[i]).ratio);
      CheckN(f.scale.n, o.fsr[i], std::string("FSR scene scale at ") + o.name + ", mode " + um::kFsrModes[i].cvar);
      CheckN(d.scale.n, o.dlss[i], std::string("DLSS scene scale at ") + o.name + ", mode " + um::kDlssModes[i].cvar);
    }
  }
  // Native AA draws the scene at the output's own size; every other mode draws it smaller.
  for (const Output& o : outputs) {
    Check(ChooseSceneScale(Scale{o.n}, um::kFsrModes[0].ratio).scale.n == o.n, std::string("native AA is the output's scale at ") + o.name);
  }
  // A live change from one mode to another is a move of the scene's images from one of these scales to the other (SwitchPath): the
  // scales are different for different modes, so the move is never a no-op, and going back lands on the same scale as the first time.
  const Scale quality = ChooseSceneScale(Scale{30}, um::kFsrModes[1].ratio).scale;
  const Scale perf = ChooseSceneScale(Scale{30}, um::kFsrModes[3].ratio).scale;
  Check(quality != perf, "quality and performance are different scales at 1080p");
  Check(ChooseSceneScale(Scale{30}, um::kFsrModes[1].ratio).scale == quality, "back to quality: the same scale");
  // The same mode on another output is another scale: the window or the render scale moving makes the scene move too.
  Check(ChooseSceneScale(Scale{40}, um::kFsrModes[1].ratio).scale != quality, "quality at 1440p is another scale than at 1080p");
}

}  // namespace

int main() {
  TheTables();
  TheUpscalerWords();
  TheModeWords();
  WhichModesRun();
  TheSceneSizeOfEachMode();
  if (g_failed != 0) {
    std::printf("upscale_modes_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("upscale_modes_test: %d checks passed\n", g_checks);
  return 0;
}
