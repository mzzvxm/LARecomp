// MCLA GPU renderer -- the start's internal resolution scale from the cvars.
// See render_scale.h.

#include "render_scale.h"

#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_resolution_scale, 1.0, "MCLA/GPU",
    "Internal resolution scale: every render target that is an image of the screen (the scene, "
    "the UI, the post-processing sized from the screen) is this many times the guest's in each "
    "direction, from 0.5 to 3 in steps of 0.05 (0.75 = 960x540, 1.5 = 1920x1080, 2 = 2560x1440, "
    "3 = 3840x2160 from 720p; other values go to the nearest step, and outside 0.5 to 3 to the "
    "nearer end). 0: the screen's -- the window's 16:9 area at start over the game's 1280x720. "
    "Not used while mcla_gpu_fsr is on with a preset (mcla_gpu_fsr_preset). The guest keeps "
    "seeing its own sizes.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr_preset, "custom", "MCLA/GPU",
    "With mcla_gpu_fsr on: the internal resolution scale is the screen's (the window's 16:9 "
    "area at start over the game's 1280x720) divided by an FSR1 quality ratio -- ultra_quality "
    "1.3, quality 1.5, balanced 1.7, performance 2, ultra_performance 3 -- on the 0.05 grid and "
    "at least 0.5, and FSR1 upscales it to the window. custom: mcla_gpu_resolution_scale. With "
    "mcla_gpu_fsr off it is not used.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DECLARE(bool, mcla_gpu_fsr);  // present.cpp

namespace mcla::gpu {

namespace {

// What a choice renders, for the logs: "1.5x, 1920x1080 of the game's 1280x720".
std::string Rendered(Scale scale) {
  return std::format("{}x, {}x{} of the game's {}x{}", scale, scale.Edge(kAssumedScreenWidth),
                     scale.Edge(kAssumedScreenHeight), kAssumedScreenWidth, kAssumedScreenHeight);
}

// How the window measured: "the window 1366x768, its 16:9 area 1365x768 (1.067x)".
std::string Window(const RenderScaleChoice& c) {
  const bool whole_area = c.area_width == c.window_width && c.area_height == c.window_height;
  return whole_area ? std::format("the window {}x{} ({:.3g}x)", c.window_width, c.window_height,
                                  c.screen)
                    : std::format("the window {}x{}, its 16:9 area {}x{} ({:.3g}x)",
                                  c.window_width, c.window_height, c.area_width, c.area_height,
                                  c.screen);
}

// The step the wanted scale was brought to, if any: ", limited to 0.5x".
std::string Adjusted(const RenderScaleChoice& c) {
  if (c.limited) {
    return std::format(", limited to {}x", c.scale);
  }
  if (c.rounded()) {
    return std::format(", nearest step {}x", c.scale);
  }
  return std::string();
}

}  // namespace

RenderScaleChoice ChooseStartRenderScale(uint32_t window_width, uint32_t window_height) {
  const double asked = REXCVAR_GET(mcla_gpu_resolution_scale);
  const bool fsr = REXCVAR_GET(mcla_gpu_fsr);
  const std::string preset_text = REXCVAR_GET(mcla_gpu_fsr_preset);
  FsrPreset preset = FsrPreset::kCustom;
  if (!ParseFsrPreset(preset_text, &preset)) {
    REXLOG_ERROR("[gpu] mcla_gpu_fsr_preset = '{}' is not custom, ultra_quality, quality, "
                 "balanced, performance or ultra_performance; custom is used",
                 preset_text);
  }
  const RenderScaleChoice c =
      ChooseRenderScale(asked, fsr, preset, window_width, window_height);
  switch (c.source) {
    case RenderScaleChoice::Source::kAsked:
      if (c.invalid) {
        REXLOG_ERROR("[gpu] mcla_gpu_resolution_scale = {} is not a scale (0 = the screen's, or "
                     "0.5 to 3); the render scale is {}",
                     asked, Rendered(c.scale));
      } else {
        REXLOG_INFO("[gpu] render scale {}: mcla_gpu_resolution_scale = {}{}", Rendered(c.scale),
                    asked, Adjusted(c));
      }
      break;
    case RenderScaleChoice::Source::kScreen:
      REXLOG_INFO("[gpu] render scale {}: the screen's (mcla_gpu_resolution_scale = 0), {}{}",
                  Rendered(c.scale), Window(c), Adjusted(c));
      break;
    case RenderScaleChoice::Source::kPreset:
      REXLOG_INFO("[gpu] render scale {}: FSR1 {} (mcla_gpu_fsr_preset), {} / {} = {:.3g}x{}; "
                  "mcla_gpu_resolution_scale = {} is not used",
                  Rendered(c.scale), FsrPresetName(preset), Window(c), FsrPresetRatio(preset),
                  c.wanted, Adjusted(c), asked);
      break;
  }
  if (!fsr && preset != FsrPreset::kCustom) {
    REXLOG_INFO("[gpu] mcla_gpu_fsr_preset = {} waits for mcla_gpu_fsr: FSR1 is off",
                FsrPresetName(preset));
  }
  return c;
}

void NoteRenderScaleWindow(const RenderScaleChoice& start, uint32_t window_width,
                           uint32_t window_height, Scale* noted) {
  if (!start.from_window() || window_width == 0 || window_height == 0) {
    return;
  }
  const RenderScaleChoice now =
      ChooseRenderScale(start.asked, start.fsr, start.preset, window_width, window_height);
  // Back at the start's scale, or the one already reported (a resize drags
  // through many sizes of the same step).
  const Scale previous = *noted;
  *noted = now.scale;
  if (now.scale == start.scale || now.scale == previous) {
    return;
  }
  REXLOG_INFO("[gpu] render scale: {} would give {} for {}; the render stays at {}x until the "
              "next start",
              Window(now), Rendered(now.scale),
              start.source == RenderScaleChoice::Source::kScreen
                  ? std::string("the screen's")
                  : std::format("FSR1 {}", FsrPresetName(start.preset)),
              start.scale);
}

}  // namespace mcla::gpu
