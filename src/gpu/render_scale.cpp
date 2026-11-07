// MCLA GPU renderer -- the start's internal resolution scale from the cvars.
// See render_scale.h.

#include "render_scale.h"

#include <atomic>
#include <format>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "temporal_backend.h"

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_resolution_scale, 0.0, "MCLA/GPU",
    "Render scale: every render target that is an image of the screen (the scene, the UI, the "
    "post-processing sized from the screen) is this many times the guest's in each direction, from "
    "0.5 to 3 in steps of 0.05 (0.75 = 960x540, 1.5 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160 from "
    "720p; other values go to the nearest step, and outside 0.5 to 3 to the nearer end). 0, the "
    "default: the screen's -- the window's 16:9 area over the game's 1280x720, followed while the "
    "game runs (fullscreen, a resized window): the picture is drawn at the size of the window "
    "(\"Render Scale 100%\" in the pause menu); above it is supersampling. Not used while a "
    "temporal upscaler (mcla_gpu_upscaler) runs: its output is always the screen's, and the scene "
    "is drawn at its mode's share of that. Not used either while mcla_gpu_fsr is on with a preset "
    "(mcla_gpu_fsr_preset). Live: the images are made again at the new scale at a frame boundary, "
    "once the value has stood for a moment (and, while an upscaler runs, once its context for the "
    "new sizes is ready); a scale the video memory cannot hold is not made: the old one is kept and "
    "the log says so. The guest keeps seeing its own sizes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr_preset, "custom", "MCLA/GPU",
    "With mcla_gpu_fsr on: the internal resolution scale is the screen's (the window's 16:9 "
    "area over the game's 1280x720, followed while the game runs) divided by an FSR1 quality "
    "ratio -- ultra_quality 1.3, quality 1.5, balanced 1.7, performance 2, ultra_performance 3 -- "
    "on the 0.05 grid and at least 0.5, and FSR1 upscales it to the window. custom: "
    "mcla_gpu_resolution_scale (by default the screen's, which leaves FSR1 nothing to upscale: pick "
    "a preset). With mcla_gpu_fsr off it is not used, nor while a temporal upscaler "
    "(mcla_gpu_upscaler) runs. Live, like mcla_gpu_resolution_scale.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

// What the renderer's images of the screen are at, and what waits (GetRenderScaleStatus): two scales
// of twentieths packed in one word, running in the high half.
std::atomic<uint64_t> g_status{0};

// The preset text that was reported as not one (the live choice reads it at every frame boundary).
std::mutex g_reported_mutex;
std::string g_reported_preset;

// The preset the cvar names; custom, with an error line for a text that is none (`once`: only the
// first time the same text is seen).
FsrPreset ReadPreset(bool once) {
  const std::string preset_text = REXCVAR_GET(mcla_gpu_fsr_preset);
  FsrPreset preset = FsrPreset::kCustom;
  if (ParseFsrPreset(preset_text, &preset)) {
    return preset;
  }
  if (once) {
    std::lock_guard lock(g_reported_mutex);
    if (g_reported_preset == preset_text) {
      return FsrPreset::kCustom;
    }
    g_reported_preset = preset_text;
  }
  REXLOG_ERROR("[gpu] mcla_gpu_fsr_preset = '{}' is not custom, ultra_quality, quality, "
               "balanced, performance or ultra_performance; custom is used",
               preset_text);
  return FsrPreset::kCustom;
}

}  // namespace

RenderScaleChoice ChooseWantedRenderScale(uint32_t window_width, uint32_t window_height) {
  return ChooseRenderScale(REXCVAR_GET(mcla_gpu_resolution_scale), REXCVAR_GET(mcla_gpu_fsr),
                           ReadPreset(/*once=*/true), window_width, window_height);
}

std::string DescribeRenderScale(const RenderScaleChoice& c) {
  switch (c.source) {
    case RenderScaleChoice::Source::kAsked:
      if (c.invalid) {
        return std::format("mcla_gpu_resolution_scale = {} is not a scale: 1x", c.asked);
      }
      return std::format("mcla_gpu_resolution_scale = {}{}", c.asked, Adjusted(c));
    case RenderScaleChoice::Source::kScreen:
      return std::format("the screen's (mcla_gpu_resolution_scale = 0), {}{}", Window(c),
                         Adjusted(c));
    case RenderScaleChoice::Source::kPreset:
      return std::format("FSR1 {} (mcla_gpu_fsr_preset), {} / {} = {:.3g}x{}",
                         FsrPresetName(c.preset), Window(c), FsrPresetRatio(c.preset), c.wanted,
                         Adjusted(c));
    case RenderScaleChoice::Source::kUpscaler:
      return std::format("the output of the upscaler: the screen's, {}{}", Window(c), Adjusted(c));
  }
  return std::string();
}

std::string DescribeUpscalerTarget(Scale output, uint32_t window_width, uint32_t window_height) {
  const uint32_t width = output.Edge(kAssumedScreenWidth);
  const uint32_t height = output.Edge(kAssumedScreenHeight);
  if (window_width == 0 || window_height == 0) {
    return std::format("{}x{}", width, height);
  }
  if (width == window_width && height == window_height) {
    return std::format("the window's {}x{}", width, height);
  }
  // The window's 16:9 area, as ChooseRenderScale measures it; the output is that area on the grid.
  const double screen = std::min(double(window_width) / kAssumedScreenWidth,
                                 double(window_height) / kAssumedScreenHeight);
  const uint32_t area_width = std::max(uint32_t(std::lround(kAssumedScreenWidth * screen)), 1u);
  const uint32_t area_height = std::max(uint32_t(std::lround(kAssumedScreenHeight * screen)), 1u);
  if (width == area_width && height == area_height) {
    return std::format("{}x{}, the 16:9 area of the {}x{} window", width, height, window_width,
                       window_height);
  }
  if (output == kMaxRenderScale || output == kMinRenderScale) {
    return std::format("{}x{}, the {} output made, for the {}x{} window", width, height,
                       output == kMaxRenderScale ? "largest" : "smallest", window_width,
                       window_height);
  }
  return std::format("{}x{}, the step of the scale grid nearest the 16:9 area ({}x{}) of the {}x{} "
                     "window",
                     width, height, area_width, area_height, window_width, window_height);
}

void PublishRenderScaleStatus(Scale running, Scale blocked) {
  g_status.store(uint64_t(running.n) << 32 | blocked.n, std::memory_order_relaxed);
}

RenderScaleStatus GetRenderScaleStatus() {
  const uint64_t status = g_status.load(std::memory_order_relaxed);
  RenderScaleStatus out;
  out.running = Scale{uint32_t(status >> 32)};
  out.blocked = Scale{uint32_t(status)};
  return out;
}

RenderScaleChoice ChooseStartRenderScale(uint32_t window_width, uint32_t window_height) {
  const double asked = REXCVAR_GET(mcla_gpu_resolution_scale);
  const bool fsr = REXCVAR_GET(mcla_gpu_fsr);
  const FsrPreset preset = ReadPreset(/*once=*/false);
  // The temporal upscaler as chosen: whether this PC can run it is not known yet (the probe of its
  // backend ends later), so the choice counts. If it cannot run, the temporal system takes the render
  // scale back to the settings' own at the first frame boundary it can (ReadRenderScale).
  const Upscaler upscaler = ReadUpscalerSettings().upscaler;
  const RenderScaleChoice c =
      upscaler != Upscaler::kOff
          ? ChooseUpscalerOutputScale(asked, fsr, preset, window_width, window_height)
          : ChooseRenderScale(asked, fsr, preset, window_width, window_height);
  PublishRenderScaleStatus(c.scale, Scale::None());
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
    case RenderScaleChoice::Source::kUpscaler: {
      // The settings that the upscaler's output replaces, when they say something else.
      std::string unused;
      if (asked != 0.0) {
        unused = std::format("mcla_gpu_resolution_scale = {}", asked);
      }
      if (fsr && preset != FsrPreset::kCustom) {
        unused += std::format("{}FSR1 {} (mcla_gpu_fsr_preset)", unused.empty() ? "" : " and ",
                              FsrPresetName(preset));
      }
      REXLOG_INFO("[gpu] render scale {}: {} is chosen, so the output is the screen's, {}{}: "
                  "upscaling to {}{}{}",
                  Rendered(c.scale), UpscalerName(upscaler), Window(c), Adjusted(c),
                  DescribeUpscalerTarget(c.scale, window_width, window_height),
                  unused.empty() ? "" : "; not used while it runs: ", unused);
      break;
    }
  }
  if (!fsr && preset != FsrPreset::kCustom) {
    REXLOG_INFO("[gpu] mcla_gpu_fsr_preset = {} waits for mcla_gpu_fsr: FSR1 is off",
                FsrPresetName(preset));
  }
  return c;
}

}  // namespace mcla::gpu
