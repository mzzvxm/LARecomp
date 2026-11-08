#pragma once

// MCLA GPU renderer -- the internal resolution scale the renderer works at
// (M6e.3), which is also the scale of the output: the images of the screen are
// made at it and the upscalers' output is one of them. It is
//   - while a temporal upscaler (FSR 3.1, FSR 4, DLSS) is the active choice: the
//     screen's, whatever mcla_gpu_resolution_scale and an FSR1 preset say; the
//     upscaler's output is the window's, and the scene is drawn at the vendor's
//     ratio of it (ChooseSceneScale below);
//   - otherwise the one an FSR1 quality preset derives from the screen
//     (mcla_gpu_fsr_preset, with mcla_gpu_fsr on),
//   - else the one asked for (mcla_gpu_resolution_scale; 0 is the screen's, which
//     is also the default: "Render Scale 100%").
// The choice is pure and lives here; render_scale.cpp reads the cvars and logs
// it. It is made at start and again at every frame boundary (temporal.cpp
// ReadRenderScale): the scale, the preset, the upscaler that is on and the
// window's size (fullscreen, a resize) are followed while the game runs, and the
// images of the screen move to the new scale between two frames (edram.h
// SetScales, temporal.h "Changing the path").
//
// "The screen" is the window's 16:9 area, the game's screen letterboxed in it
// the way the present fits it; its scale is that area over the game's screen.
// A preset divides it by AMD's ratio of output to render per axis (FSR1's
// four, and FSR 2's ultra performance). Every choice ends on the grid of
// Scale, from kMinRenderScale to kMaxRenderScale.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "scale.h"

namespace mcla::gpu {

inline constexpr Scale kMinRenderScale{10};  // 0.5x
inline constexpr Scale kMaxRenderScale{60};  // 3x

// The game's screen (grcDevice's size) in every session logged so far. The
// screen's scale and the presets measure the window against it before the game
// has set its own; the EDRAM checks it on its first read (ScreenSize).
inline constexpr uint32_t kAssumedScreenWidth = 1280;
inline constexpr uint32_t kAssumedScreenHeight = 720;

enum class FsrPreset : uint8_t {
  kCustom,  // the scale asked for
  kUltraQuality,
  kQuality,
  kBalanced,
  kPerformance,
  kUltraPerformance,
};

// The cvar's names, which are also what the logs print.
inline const char* FsrPresetName(FsrPreset preset) {
  switch (preset) {
    case FsrPreset::kCustom: return "custom";
    case FsrPreset::kUltraQuality: return "ultra_quality";
    case FsrPreset::kQuality: return "quality";
    case FsrPreset::kBalanced: return "balanced";
    case FsrPreset::kPerformance: return "performance";
    case FsrPreset::kUltraPerformance: return "ultra_performance";
  }
  return "custom";
}

// Output over render, per axis.
inline double FsrPresetRatio(FsrPreset preset) {
  switch (preset) {
    case FsrPreset::kCustom: return 1.0;
    case FsrPreset::kUltraQuality: return 1.3;
    case FsrPreset::kQuality: return 1.5;
    case FsrPreset::kBalanced: return 1.7;
    case FsrPreset::kPerformance: return 2.0;
    case FsrPreset::kUltraPerformance: return 3.0;
  }
  return 1.0;
}

// A name as FsrPresetName gives it, in any case, with ' ' or '-' for '_'.
// False for anything else, *out untouched.
inline bool ParseFsrPreset(std::string_view text, FsrPreset* out) {
  std::string name;
  name.reserve(text.size());
  for (char c : text) {
    if (c == ' ' || c == '-') {
      c = '_';
    } else if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
    name.push_back(c);
  }
  constexpr FsrPreset kAll[] = {FsrPreset::kCustom,      FsrPreset::kUltraQuality,
                                FsrPreset::kQuality,     FsrPreset::kBalanced,
                                FsrPreset::kPerformance, FsrPreset::kUltraPerformance};
  for (FsrPreset preset : kAll) {
    if (name == FsrPresetName(preset)) {
      *out = preset;
      return true;
    }
  }
  return false;
}

struct RenderScaleChoice {
  enum class Source : uint8_t {
    kAsked,     // mcla_gpu_resolution_scale
    kScreen,    // mcla_gpu_resolution_scale = 0
    kPreset,    // mcla_gpu_fsr on with a preset
    kUpscaler,  // a temporal upscaler is the active choice: its output is the screen's
  };

  // What the images of the screen are made at.
  Scale scale;
  Source source = Source::kAsked;
  // The inputs, kept for the logs (a window of another size is weighed with ChooseRenderScale again).
  // They are the settings' own: a choice that is `kUpscaler` did not use them.
  double asked = 0.0;
  bool fsr = false;
  FsrPreset preset = FsrPreset::kCustom;
  uint32_t window_width = 0;
  uint32_t window_height = 0;
  // The window's 16:9 area and its scale over the game's screen.
  uint32_t area_width = 0;
  uint32_t area_height = 0;
  double screen = 1.0;
  // The scale before the limits and the grid.
  double wanted = 1.0;
  // `wanted` was outside kMinRenderScale..kMaxRenderScale and was brought to
  // the nearer end.
  bool limited = false;
  // The scale asked for is not one (negative, not a number): 1x.
  bool invalid = false;

  bool from_window() const { return source != Source::kAsked; }
  // `scale` is not `wanted` (limits aside): the nearest step was taken.
  bool rounded() const {
    const double limited_to = std::clamp(wanted, double(kMinRenderScale.n) / Scale::kDenominator,
                                         double(kMaxRenderScale.n) / Scale::kDenominator);
    return !invalid && std::abs(double(scale.n) / Scale::kDenominator - limited_to) > 1e-9;
  }
};

// `asked` is mcla_gpu_resolution_scale; a preset counts only with `fsr` on.
// A window with no size measures as the game's screen (1x).
inline RenderScaleChoice ChooseRenderScale(double asked, bool fsr, FsrPreset preset,
                                           uint32_t window_width, uint32_t window_height) {
  RenderScaleChoice c;
  c.asked = asked;
  c.fsr = fsr;
  c.preset = preset;
  c.window_width = window_width;
  c.window_height = window_height;
  if (window_width != 0 && window_height != 0) {
    c.screen = std::min(double(window_width) / kAssumedScreenWidth,
                        double(window_height) / kAssumedScreenHeight);
  }
  c.area_width = std::max(uint32_t(std::lround(kAssumedScreenWidth * c.screen)), 1u);
  c.area_height = std::max(uint32_t(std::lround(kAssumedScreenHeight * c.screen)), 1u);

  if (fsr && preset != FsrPreset::kCustom) {
    c.source = RenderScaleChoice::Source::kPreset;
    c.wanted = c.screen / FsrPresetRatio(preset);
  } else if (asked == 0.0) {
    c.source = RenderScaleChoice::Source::kScreen;
    c.wanted = c.screen;
  } else if (!(asked > 0.0) || !std::isfinite(asked)) {
    c.source = RenderScaleChoice::Source::kAsked;
    c.wanted = asked;
    c.invalid = true;
    c.scale = Scale{};
    return c;
  } else {
    c.source = RenderScaleChoice::Source::kAsked;
    c.wanted = asked;
  }
  const double lo = double(kMinRenderScale.n) / Scale::kDenominator;
  const double hi = double(kMaxRenderScale.n) / Scale::kDenominator;
  const double within = std::clamp(c.wanted, lo, hi);
  c.limited = within != c.wanted;
  c.scale = Scale{uint32_t(std::lround(within * Scale::kDenominator))};
  return c;
}

// The choice while a temporal upscaler is the active choice: the screen's scale, for the window of
// that size, whatever `asked` and an FSR1 preset say (the settings' own values are kept in the result
// for the logs, and the two are not used). The upscaler's output is then the window's 16:9 area
// (on the grid of Scale, limited to kMaxRenderScale: a window above 4K is upscaled to 4K and stretched
// by the present), and ChooseSceneScale takes the scene's scale from it.
inline RenderScaleChoice ChooseUpscalerOutputScale(double asked, bool fsr, FsrPreset preset,
                                                   uint32_t window_width, uint32_t window_height) {
  RenderScaleChoice c = ChooseRenderScale(0.0, false, FsrPreset::kCustom, window_width, window_height);
  c.source = RenderScaleChoice::Source::kUpscaler;
  c.asked = asked;
  c.fsr = fsr;
  c.preset = preset;
  return c;
}

// The scale the 3D scene itself is drawn at while a temporal upscaler works on it (F4-DESIGN.md 4,
// stage U3): the output's scale (`output`, the render scale above) divided by the vendor's ratio
// of output to render, per axis (FSR 3.1 and 4: 1, 1.5, 1.7, 2, 3). The grid is Scale's twentieths,
// the one every resolve and encode shader works in, so the render size is the nearest step: the
// main view's 1280x720 comes out at multiples of 64x36 (1728x972 for Quality at 2x, where exactly
// 1/1.5 would be 1707x960). A ratio of 1 or less leaves it at `output`. Never below
// kMinSceneScale: nothing below 0.5x has been run through the fractional resolve paths.
inline constexpr Scale kMinSceneScale = kMinRenderScale;

struct SceneScaleChoice {
  Scale scale;           // what the main view's colour and depth are made at
  double ratio = 1.0;    // asked: output over render, per axis
  double wanted = 1.0;   // output / ratio, before the grid and the limit
  bool limited = false;  // `wanted` was below kMinSceneScale and was brought up to it
  // Output over render as the grid made it (1 when the scene is not drawn smaller).
  double actual_ratio(Scale output) const {
    return scale.n == 0 ? 1.0 : double(output.n) / double(scale.n);
  }
};

inline SceneScaleChoice ChooseSceneScale(Scale output, double ratio) {
  SceneScaleChoice c;
  c.scale = output;
  c.wanted = output.none() ? 1.0 : output.value();
  if (output.none() || !std::isfinite(ratio) || !(ratio > 1.0)) {
    return c;
  }
  c.ratio = ratio;
  c.wanted = output.value() / ratio;
  const double low = double(std::min(kMinSceneScale.n, output.n));
  const double step = std::round(double(output.n) / ratio);
  c.limited = step < low;
  c.scale = Scale{uint32_t(std::clamp(step, low, double(output.n)))};
  return c;
}

// render_scale.cpp. The start's choice from mcla_gpu_resolution_scale,
// mcla_gpu_fsr and mcla_gpu_fsr_preset for a window of that size, logged. A temporal upscaler chosen
// in mcla_gpu_upscaler counts as the active choice here: whether this PC can run it is known only
// later (its probe has not ended when the renderer starts), and the temporal system takes the render
// scale back to the settings' own (ReadRenderScale) once it finds that it cannot.
RenderScaleChoice ChooseStartRenderScale(uint32_t window_width, uint32_t window_height);
// The same choice for the settings as they are now, not logged (a live change reads it at every frame
// boundary): an mcla_gpu_fsr_preset that is not a preset is reported once, and custom is used. It does
// not know whether a temporal upscaler is on (the temporal system does): while one is, the caller takes
// ChooseUpscalerOutputScale of the window this choice carries instead.
RenderScaleChoice ChooseWantedRenderScale(uint32_t window_width, uint32_t window_height);
// What a choice came from and where it landed, for the log of a live change: "mcla_gpu_resolution_scale
// = 2", "the screen's (mcla_gpu_resolution_scale = 0), the window 2560x1440 (2x)", "FSR1 quality, the
// window 1920x1080 (1.5x) / 1.5 = 1x", "the output of the upscaler: the screen's, the window 1920x1080
// (1.5x)". Ends with ", nearest step 1.35x" or ", limited to 0.5x" when the grid or the limits moved it.
std::string DescribeRenderScale(const RenderScaleChoice& choice);
// Where a temporal upscaler's output goes, for the log: "the window's 1920x1080" when the output is
// the window's size, else the output's size and how it comes from the window ("2560x1440, the 16:9
// area of the 2560x1600 window"). `window_*` 0: the output's size alone.
std::string DescribeUpscalerTarget(Scale output, uint32_t window_width, uint32_t window_height);

// What the renderer's images of the screen are at now, for the pause menu (gpu.h GetRenderScaleStatus).
struct RenderScaleStatus {
  Scale running = Scale::None();  // the scale the images of the screen are made at; none before the renderer is up
  // The scale the settings ask for but that could not be made (video memory is short): the renderer
  // keeps `running` and tries again; none while nothing is waiting for that.
  Scale blocked = Scale::None();
};
// Written by the temporal system (render thread).
void PublishRenderScaleStatus(Scale running, Scale blocked);

}  // namespace mcla::gpu
