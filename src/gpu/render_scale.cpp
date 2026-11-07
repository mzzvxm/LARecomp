// MCLA GPU renderer -- the start's internal resolution scale from the cvars.
// See render_scale.h.

#include "render_scale.h"

#include <atomic>
#include <format>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_resolution_scale, 1.0, "MCLA/GPU",
    "Internal resolution scale: every render target that is an image of the screen (the scene, "
    "the UI, the post-processing sized from the screen) is this many times the guest's in each "
    "direction, from 0.5 to 3 in steps of 0.05 (0.75 = 960x540, 1.5 = 1920x1080, 2 = 2560x1440, "
    "3 = 3840x2160 from 720p; other values go to the nearest step, and outside 0.5 to 3 to the "
    "nearer end). 0: the screen's -- the window's 16:9 area over the game's 1280x720, followed "
    "while the game runs (fullscreen, a resized window). Not used while mcla_gpu_fsr is on with a "
    "preset (mcla_gpu_fsr_preset). Live: the images are made again at the new scale at a frame "
    "boundary, once the value has stood for a moment (and, while an upscaler runs, once its "
    "context for the new sizes is ready); a scale the video memory cannot hold is not made: the "
    "old one is kept and the log says so. The guest keeps seeing its own sizes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr_preset, "custom", "MCLA/GPU",
    "With mcla_gpu_fsr on: the internal resolution scale is the screen's (the window's 16:9 "
    "area over the game's 1280x720, followed while the game runs) divided by an FSR1 quality "
    "ratio -- ultra_quality 1.3, quality 1.5, balanced 1.7, performance 2, ultra_performance 3 -- "
    "on the 0.05 grid and at least 0.5, and FSR1 upscales it to the window. custom: "
    "mcla_gpu_resolution_scale. With mcla_gpu_fsr off it is not used. Live, like "
    "mcla_gpu_resolution_scale.")
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
  }
  return std::string();
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
  const RenderScaleChoice c =
      ChooseRenderScale(asked, fsr, preset, window_width, window_height);
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
  }
  if (!fsr && preset != FsrPreset::kCustom) {
    REXLOG_INFO("[gpu] mcla_gpu_fsr_preset = {} waits for mcla_gpu_fsr: FSR1 is off",
                FsrPresetName(preset));
  }
  return c;
}

}  // namespace mcla::gpu
