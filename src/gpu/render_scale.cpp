// MCLA GPU renderer -- the start's internal resolution scale from the cvars.
// See render_scale.h.

#include "render_scale.h"

#include <atomic>
#include <format>
#include <functional>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "scale_cap.h"
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
    "the log says so. At the default (0) a window whose scale would need more video memory than the "
    "graphics card's budget allows (a 4K window on a card with 2 to 3 GB) is drawn at the largest "
    "scale that fits instead, never below 1, at the start and whenever the window changes, and the "
    "log says so; a scale set here is kept as chosen. The guest keeps seeing its own sizes.")
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

REXCVAR_DEFINE_UINT32(
    mcla_gpu_scale_cap_budget_mb, 0, "MCLA/GPU",
    "Diagnostic: pretend the graphics card is a discrete one with this many MiB of video memory budget "
    "when the render scale that follows the window is checked against it (the cap of "
    "mcla_gpu_resolution_scale = 0: a window whose scale needs more than 75% of the budget is "
    "lowered), to see what a small card would get. Only that check reads it; the video memory "
    "response and everything else use the card's real memory. 0: the real one. Read at the start and "
    "whenever the window's size changes.")
    .debug_only()
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
  if (c.capped) {
    return std::format(", capped by the video memory from {}x", c.uncapped);
  }
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

// The render scale cap (scale_cap.h): what the card says is read through the probe the renderer
// registers, once per window size (a window that changes is the moment the budget is looked at again;
// the choice is made at every frame boundary and must not ask the OS every time).
struct CapState {
  std::mutex mutex;
  std::function<scale_cap::Memory()> probe;
  bool have_memory = false;
  uint32_t width = 0;  // the window the memory was read for
  uint32_t height = 0;
  scale_cap::Memory memory;
  // What the last line was about (a live change logs only when this changes to or from a lowered scale).
  scale_cap::Outcome last_outcome = scale_cap::Outcome::kNotChecked;
  uint32_t last_scale = 0;
};
CapState g_cap;

// `start`: the first look, which always logs its verdict; a live look logs only a scale that is lowered
// or kept against the budget, and the return to one that fits.
RenderScaleChoice ApplyMemoryCap(RenderScaleChoice c, bool start) {
  std::lock_guard lock(g_cap.mutex);
  if (!g_cap.probe) {
    return c;  // no renderer of ours (the Xenia one): nothing to look at
  }
  if (start || !g_cap.have_memory || g_cap.width != c.window_width ||
      g_cap.height != c.window_height) {
    g_cap.memory = g_cap.probe();
    if (const uint32_t pretend = REXCVAR_GET(mcla_gpu_scale_cap_budget_mb)) {
      g_cap.memory.budget = uint64_t(pretend) * scale_cap::kMiB;
      g_cap.memory.dedicated = g_cap.memory.budget;
      g_cap.memory.shared = false;
    }
    g_cap.have_memory = true;
    g_cap.width = c.window_width;
    g_cap.height = c.window_height;
  }
  const scale_cap::Report r = scale_cap::Apply(&c, g_cap.memory);
  using scale_cap::Outcome;
  const auto lowered = [](Outcome o) { return o == Outcome::kCapped || o == Outcome::kKept; };
  const bool changed = r.outcome != g_cap.last_outcome || r.scale.n != g_cap.last_scale;
  if (start || (changed && (lowered(r.outcome) || lowered(g_cap.last_outcome)))) {
    const std::string line = scale_cap::Line(c, r);
    if (r.outcome == Outcome::kKept || (r.outcome == Outcome::kCapped && r.need_scale > r.plan)) {
      REXLOG_WARN("[gpu] {}", line);
    } else {
      REXLOG_INFO("[gpu] {}", line);
    }
  }
  g_cap.last_outcome = r.outcome;
  g_cap.last_scale = r.scale.n;
  return c;
}

}  // namespace

void scale_cap::SetMemoryProbe(std::function<Memory()> probe) {
  std::lock_guard lock(g_cap.mutex);
  g_cap.probe = std::move(probe);
  g_cap.have_memory = false;
  g_cap.last_outcome = Outcome::kNotChecked;
  g_cap.last_scale = 0;
}

RenderScaleChoice ChooseWantedRenderScale(uint32_t window_width, uint32_t window_height) {
  return ChooseRenderScale(REXCVAR_GET(mcla_gpu_resolution_scale), REXCVAR_GET(mcla_gpu_fsr),
                           ReadPreset(/*once=*/true), window_width, window_height);
}

RenderScaleChoice CapToVideoMemory(RenderScaleChoice choice) {
  return ApplyMemoryCap(std::move(choice), /*start=*/false);
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

// CHECK: not run (written without a build). A start with an upscaler chosen logs "[gpu] render scale 1.5x
// ...: DLSS is chosen, so the output is the screen's ...: upscaling to the window's 1920x1080" in a
// 1080p window (one line, before "[gpu] edram: the 3D scene ... is made at 1x"). A start at the default
// in a 4K window on a card whose budget cannot hold 3x (scale_cap.h; --mcla_gpu_scale_cap_budget_mb=2048
// pretends one) logs "[gpu] render scale lowered from 3x to ...x: ..." and makes the images of the screen
// at that scale; with a scale set in mcla_gpu_resolution_scale it logs the warning "... is kept as
// chosen" and makes the scale asked for.
RenderScaleChoice ChooseStartRenderScale(uint32_t window_width, uint32_t window_height) {
  const double asked = REXCVAR_GET(mcla_gpu_resolution_scale);
  const bool fsr = REXCVAR_GET(mcla_gpu_fsr);
  const FsrPreset preset = ReadPreset(/*once=*/false);
  // The temporal upscaler as chosen: whether this PC can run it is not known yet (the probe of its
  // backend ends later), so the choice counts. If it cannot run, the temporal system takes the render
  // scale back to the settings' own at the first frame boundary it can (ReadRenderScale).
  const Upscaler upscaler = ReadUpscalerSettings().upscaler;
  // The scale against the card's video memory: an automatic scale that does not fit is lowered, the
  // verdict is logged on its own line (scale_cap.h).
  const RenderScaleChoice c = ApplyMemoryCap(
      upscaler != Upscaler::kOff
          ? ChooseUpscalerOutputScale(asked, fsr, preset, window_width, window_height)
          : ChooseRenderScale(asked, fsr, preset, window_width, window_height),
      /*start=*/true);
  // A lowered scale is published like one that could not be made: the menu says "TOO BIG (NN%)".
  PublishRenderScaleStatus(c.scale, c.capped ? c.uncapped : Scale::None());
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
