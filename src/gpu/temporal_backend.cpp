// MCLA GPU renderer -- the temporal upscaler backends: the player's settings and the names. See
// temporal_backend.h.

#include "temporal_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    mcla_gpu_upscaler, "off", "MCLA/GPU",
    "Upscaling of the 3D scene: off, fsr31 (AMD FSR 3.1) or fsr4 (AMD FSR 4, only where the "
    "graphics card and driver run it). The scene is moved by a different sub-pixel amount every "
    "frame and AMD's upscaler rebuilds a sharper, steadier picture from the frames; the menus, the "
    "HUD and the bloom, glow and tone map stay at full size. While one is on, FXAA and SMAA are "
    "held off, and a game started with one on runs without MSAA (your MSAA choice, "
    "mcla_gpu_msaa, is kept and applies again at a start with no upscaler: the game's MSAA is a "
    "start-up switch). A choice this PC cannot run is treated as off. Live, except that MSAA "
    "follows at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr31_mode, "native_aa", "MCLA/GPU",
    "FSR 3.1's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the render scale (mcla_gpu_resolution_scale). The scene's size is "
    "fixed when the game starts: a mode changed later applies at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr4_mode, "native_aa", "MCLA/GPU",
    "FSR 4's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the render scale (mcla_gpu_resolution_scale). The scene's size is "
    "fixed when the game starts: a mode changed later applies at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_upscaler_sharpness, 0.2, "MCLA/GPU",
    "How much the upscaler sharpens its picture (AMD's RCAS pass), 0 to 1: 0 adds no sharpening, "
    "1 the most. Live.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(std::string, mcla_gpu_msaa);  // edram.cpp

namespace mcla::gpu {

namespace {

constexpr ModeInfo kModes[size_t(UpscaleMode::kCount)] = {
    {"native_aa", "NATIVE AA", 100, 1.0},
    {"quality", "QUALITY", 67, 1.5},
    {"balanced", "BALANCED", 59, 1.7},
    {"performance", "PERFORMANCE", 50, 2.0},
    {"ultra_performance", "ULTRA PERFORMANCE", 33, 3.0},
};

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

// Packed for one atomic: bit 0 probed, 1 fsr31, 2 fsr4.
std::atomic<uint32_t> g_availability{0};
std::atomic<uint32_t> g_running{0};
std::atomic<uint8_t> g_guest_msaa{uint8_t(GuestMsaa::kUndecided)};

}  // namespace

const char* UpscalerCvarValue(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "fsr31";
    case Upscaler::kFsr4: return "fsr4";
    default: return "off";
  }
}

const char* UpscalerName(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "FSR 3.1";
    case Upscaler::kFsr4: return "FSR 4";
    default: return "OFF";
  }
}

bool ParseUpscaler(std::string_view text, Upscaler* out) {
  const std::string value = Lower(text);
  *out = Upscaler::kOff;
  if (value == "fsr31" || value == "fsr3.1" || value == "fsr 3.1") {
    *out = Upscaler::kFsr31;
    return true;
  }
  if (value == "fsr4" || value == "fsr 4") {
    *out = Upscaler::kFsr4;
    return true;
  }
  return value == "off" || value.empty();
}

const ModeInfo& ModeInfoOf(UpscaleMode mode) {
  return kModes[size_t(mode) < size_t(UpscaleMode::kCount) ? size_t(mode) : 0];
}

bool ParseUpscaleMode(std::string_view text, UpscaleMode* out) {
  const std::string value = Lower(text);
  *out = UpscaleMode::kNativeAa;
  for (size_t i = 0; i < size_t(UpscaleMode::kCount); ++i) {
    if (value == kModes[i].cvar) {
      *out = UpscaleMode(i);
      return true;
    }
  }
  return false;
}

bool ModeRuns(Upscaler upscaler, UpscaleMode mode) {
  // FSR 3.1 and FSR 4 both take all five of the vendor's modes (the scene's size is the start's
  // choice: GetStartUpscale).
  return upscaler != Upscaler::kOff && size_t(mode) < size_t(UpscaleMode::kCount);
}

UpscalerSettings ReadUpscalerSettings() {
  UpscalerSettings out;
  ParseUpscaler(REXCVAR_GET(mcla_gpu_upscaler), &out.upscaler);
  const std::string mode = out.upscaler == Upscaler::kFsr4 ? REXCVAR_GET(mcla_gpu_fsr4_mode)
                                                           : REXCVAR_GET(mcla_gpu_fsr31_mode);
  UpscaleMode parsed = UpscaleMode::kNativeAa;
  ParseUpscaleMode(mode, &parsed);
  out.mode = ModeRuns(out.upscaler, parsed) ? parsed : UpscaleMode::kNativeAa;
  out.sharpness = float(std::clamp(REXCVAR_GET(mcla_gpu_upscaler_sharpness), 0.0, 1.0));
  return out;
}

bool MayDrawSceneSmaller() {
  const UpscalerSettings s = ReadUpscalerSettings();
  return s.upscaler != Upscaler::kOff && ModeInfoOf(s.mode).ratio > 1.0;
}

void PublishUpscalerAvailability(bool probed, bool fsr31, bool fsr4) {
  g_availability.store(uint32_t(probed) | uint32_t(fsr31) << 1 | uint32_t(fsr4) << 2,
                       std::memory_order_relaxed);
}

void PublishRunningUpscaler(Upscaler running) {
  g_running.store(uint32_t(running), std::memory_order_relaxed);
}

UpscalerAvailability GetUpscalerAvailability() {
  const uint32_t v = g_availability.load(std::memory_order_relaxed);
  UpscalerAvailability a;
  a.probed = (v & 1u) != 0;
  a.fsr31 = (v & 2u) != 0;
  a.fsr4 = (v & 4u) != 0;
  a.running = Upscaler(g_running.load(std::memory_order_relaxed) & 3u);
  return a;
}

namespace {

// The start's upscaler (gpu.h StartUpscale), packed for one atomic: bit 0 decided, 1 probed, 2
// runs, bits 4-5 the upscaler, bits 8-11 the mode.
std::atomic<uint32_t> g_start_upscale{0};

void DecideStartUpscale() {
  Upscaler chosen = Upscaler::kOff;
  ParseUpscaler(REXCVAR_GET(mcla_gpu_upscaler), &chosen);
  bool probed = false;
  bool runs = false;
  UpscaleMode mode = UpscaleMode::kNativeAa;
  if (chosen != Upscaler::kOff) {
    // Whether this PC can run it is known once the probe ends. The probe is started with the
    // renderer (before the game runs) and takes well under a second. A build with no FidelityFX
    // never starts one, and nothing is waited for.
    // CHECK: that the game makes its renderer after the renderer has started the probe (in a log,
    // the "[gpu] ffx: ready" line comes before "[gpu] msaa:" and "[gpu] upscaler at start:"); if
    // not, the hold is missed and logged.
#if defined(LARECOMP_HAS_FFX)
    constexpr auto kProbeWait = std::chrono::seconds(4);  // the game's thread, at its renderer's creation
    const auto give_up = std::chrono::steady_clock::now() + kProbeWait;
    while (!GetUpscalerAvailability().probed && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    const UpscalerAvailability a = GetUpscalerAvailability();
    probed = a.probed;
    runs = a.probed && ((chosen == Upscaler::kFsr31 && a.fsr31) ||
                        (chosen == Upscaler::kFsr4 && a.fsr4));
    mode = ReadUpscalerSettings().mode;
  }
  g_start_upscale.store(1u | uint32_t(probed) << 1 | uint32_t(runs) << 2 | uint32_t(chosen) << 4 |
                            uint32_t(runs ? mode : UpscaleMode::kNativeAa) << 8,
                        std::memory_order_release);
  if (chosen != Upscaler::kOff) {
    if (runs) {
      REXLOG_INFO("[gpu] upscaler at start: {} {} ({}% of the output per direction, ratio {:.2f}): the "
                  "3D scene is drawn at that share of the render scale from this start on (the "
                  "mode and the render scale apply at a start, not live)",
                  UpscalerName(chosen), ModeInfoOf(mode).name, ModeInfoOf(mode).percent,
                  ModeInfoOf(mode).ratio);
    } else {
      REXLOG_INFO("[gpu] upscaler at start: {} is chosen but {}: the scene is drawn at the render "
                  "scale",
                  UpscalerName(chosen),
                  probed ? "this PC cannot run it"
                         : "its probe has not ended (or this build has no upscaler runtime)");
    }
  }
}

void DecideGuestMsaa() {
  const std::string msaa = REXCVAR_GET(mcla_gpu_msaa);
  std::string choice = msaa;
  choice.erase(std::remove_if(choice.begin(), choice.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               choice.end());
  if (Lower(choice) == "off") {
    g_guest_msaa.store(uint8_t(GuestMsaa::kOffChosen), std::memory_order_relaxed);
    return;
  }
  Upscaler chosen = Upscaler::kOff;
  ParseUpscaler(REXCVAR_GET(mcla_gpu_upscaler), &chosen);
  if (chosen == Upscaler::kOff) {
    g_guest_msaa.store(uint8_t(GuestMsaa::kOn), std::memory_order_relaxed);
    return;
  }
  // An upscaler is chosen: whether this PC can run it is the start's decision (above).
  const StartUpscale start = GetStartUpscale();
  const bool runs = start.runs;
  if (runs) {
    g_guest_msaa.store(uint8_t(GuestMsaa::kOffForUpscaler), std::memory_order_relaxed);
    REXLOG_INFO("[gpu] msaa: the game starts without MSAA: {} is chosen and runs on this PC, and "
                "MSAA under it only costs time. mcla_gpu_msaa = '{}' is kept and applies again at "
                "a start with no upscaler (the game's MSAA is a start-up switch)",
                UpscalerName(chosen), msaa);
  } else {
    g_guest_msaa.store(uint8_t(GuestMsaa::kOn), std::memory_order_relaxed);
    REXLOG_INFO("[gpu] msaa: {} is chosen but {}: the game starts with the MSAA chosen "
                "(mcla_gpu_msaa = '{}')",
                UpscalerName(chosen),
                start.probed ? "this PC cannot run it"
                             : "its probe has not ended (or this build has no upscaler runtime)",
                msaa);
  }
}

}  // namespace

StartUpscale GetStartUpscale() {
  static std::once_flag once;
  std::call_once(once, DecideStartUpscale);
  return PeekStartUpscale();
}

StartUpscale PeekStartUpscale() {
  const uint32_t v = g_start_upscale.load(std::memory_order_acquire);
  StartUpscale s;
  s.decided = (v & 1u) != 0;
  s.probed = (v & 2u) != 0;
  s.runs = (v & 4u) != 0;
  s.upscaler = Upscaler((v >> 4) & 3u);
  const uint32_t mode = (v >> 8) & 15u;
  s.mode = mode < uint32_t(UpscaleMode::kCount) ? UpscaleMode(mode) : UpscaleMode::kNativeAa;
  s.ratio = s.runs ? ModeInfoOf(s.mode).ratio : 1.0;
  return s;
}

bool GuestMsaaOff() {
  static std::once_flag once;
  std::call_once(once, DecideGuestMsaa);
  return GetGuestMsaa() != GuestMsaa::kOn;
}

GuestMsaa GetGuestMsaa() {
  return GuestMsaa(g_guest_msaa.load(std::memory_order_relaxed));
}

#if !defined(LARECOMP_HAS_FFX)
// A build without LARECOMP_FFX_SDK_DIR: no backend, nothing listed, nothing loaded.
std::unique_ptr<TemporalBackend> MakeFfxBackend() {
  return nullptr;
}
#endif

}  // namespace mcla::gpu
