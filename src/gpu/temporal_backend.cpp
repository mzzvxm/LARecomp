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
    "Upscaling of the 3D scene: off, fsr31 (AMD FSR 3.1), fsr4 (AMD FSR 4, only where the "
    "graphics card and driver run it) or dlss (NVIDIA DLSS, only on an NVIDIA RTX card, with "
    "NVIDIA's files next to the game). The scene is moved by a different sub-pixel amount every "
    "frame and the upscaler rebuilds a sharper, steadier picture from the frames; the menus, the "
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

REXCVAR_DEFINE_STRING(
    mcla_gpu_dlss_mode, "dlaa", "MCLA/GPU",
    "DLSS's mode, NVIDIA's names and the share of the output the scene is drawn at: dlaa (100%, "
    "only smoothed), quality (67%), balanced (58%), performance (50%) or ultra_performance (33%), "
    "per direction, of the render scale (mcla_gpu_resolution_scale). The scene's size is fixed "
    "when the game starts: a mode changed later applies at the next start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_upscaler_sharpness, 0.2, "MCLA/GPU",
    "How much the FSR upscalers sharpen their picture (AMD's RCAS pass), 0 to 1: 0 adds no "
    "sharpening, 1 the most. DLSS has no sharpening. Live.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(std::string, mcla_gpu_msaa);  // edram.cpp

namespace mcla::gpu {

namespace {

// AMD's names and ratios (FSR 3.1 and FSR 4).
constexpr ModeInfo kModes[size_t(UpscaleMode::kCount)] = {
    {"native_aa", "NATIVE AA", 100, 1.0},
    {"quality", "QUALITY", 67, 1.5},
    {"balanced", "BALANCED", 59, 1.7},
    {"performance", "PERFORMANCE", 50, 2.0},
    {"ultra_performance", "ULTRA PERFORMANCE", 33, 3.0},
};
// NVIDIA's: DLAA, Quality 66.7%, Balanced 58%, Performance 50%, Ultra Performance 33.3% of the
// output per direction (the sizes slDLSSGetOptimalSettings gives; the backend checks the scene's
// size against the range it reports).
constexpr ModeInfo kDlssModes[size_t(UpscaleMode::kCount)] = {
    {"dlaa", "DLAA", 100, 1.0},
    {"quality", "QUALITY", 67, 1.5},
    {"balanced", "BALANCED", 58, 1.0 / 0.58},
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
// DLSS: bit 0 probed, 1 dlss, 2 needs setup, 3 after restart.
std::atomic<uint32_t> g_dlss_availability{0};
std::atomic<uint32_t> g_running{0};
std::atomic<uint8_t> g_guest_msaa{uint8_t(GuestMsaa::kUndecided)};

}  // namespace

const char* UpscalerCvarValue(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "fsr31";
    case Upscaler::kFsr4: return "fsr4";
    case Upscaler::kDlss: return "dlss";
    default: return "off";
  }
}

const char* UpscalerName(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "FSR 3.1";
    case Upscaler::kFsr4: return "FSR 4";
    case Upscaler::kDlss: return "DLSS";
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
  if (value == "dlss") {
    *out = Upscaler::kDlss;
    return true;
  }
  return value == "off" || value.empty();
}

const ModeInfo& ModeInfoOf(Upscaler upscaler, UpscaleMode mode) {
  const ModeInfo* table = upscaler == Upscaler::kDlss ? kDlssModes : kModes;
  return table[size_t(mode) < size_t(UpscaleMode::kCount) ? size_t(mode) : 0];
}

bool ParseUpscaleMode(std::string_view text, UpscaleMode* out) {
  const std::string value = Lower(text);
  *out = UpscaleMode::kNativeAa;
  if (value == "dlaa") {  // DLSS's name for the first mode
    return true;
  }
  for (size_t i = 0; i < size_t(UpscaleMode::kCount); ++i) {
    if (value == kModes[i].cvar) {
      *out = UpscaleMode(i);
      return true;
    }
  }
  return false;
}

bool ModeRuns(Upscaler upscaler, UpscaleMode mode) {
  // FSR 3.1, FSR 4 and DLSS all take the five modes of their vendor (the scene's size is the
  // start's choice: GetStartUpscale).
  return upscaler != Upscaler::kOff && size_t(mode) < size_t(UpscaleMode::kCount);
}

UpscalerSettings ReadUpscalerSettings() {
  UpscalerSettings out;
  ParseUpscaler(REXCVAR_GET(mcla_gpu_upscaler), &out.upscaler);
  const std::string mode = out.upscaler == Upscaler::kFsr4   ? REXCVAR_GET(mcla_gpu_fsr4_mode)
                           : out.upscaler == Upscaler::kDlss ? REXCVAR_GET(mcla_gpu_dlss_mode)
                                                             : REXCVAR_GET(mcla_gpu_fsr31_mode);
  UpscaleMode parsed = UpscaleMode::kNativeAa;
  ParseUpscaleMode(mode, &parsed);
  out.mode = ModeRuns(out.upscaler, parsed) ? parsed : UpscaleMode::kNativeAa;
  out.sharpness = float(std::clamp(REXCVAR_GET(mcla_gpu_upscaler_sharpness), 0.0, 1.0));
  return out;
}

bool MayDrawSceneSmaller() {
  const UpscalerSettings s = ReadUpscalerSettings();
  return s.upscaler != Upscaler::kOff && ModeInfoOf(s.upscaler, s.mode).ratio > 1.0;
}

void PublishUpscalerAvailability(bool probed, bool fsr31, bool fsr4) {
  g_availability.store(uint32_t(probed) | uint32_t(fsr31) << 1 | uint32_t(fsr4) << 2,
                       std::memory_order_relaxed);
}

void PublishDlssAvailability(bool probed, bool dlss, bool needs_setup, bool after_restart) {
  g_dlss_availability.store(uint32_t(probed) | uint32_t(dlss) << 1 | uint32_t(needs_setup) << 2 |
                                uint32_t(after_restart) << 3,
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
  const uint32_t d = g_dlss_availability.load(std::memory_order_relaxed);
  a.dlss_probed = (d & 1u) != 0;
  a.dlss = (d & 2u) != 0;
  a.dlss_needs_setup = (d & 4u) != 0;
  a.dlss_after_restart = (d & 8u) != 0;
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
    // Whether this PC can run it is known once the probe of its backend ends. The probes are
    // started with the renderer (before the game runs) and take well under a second (DLSS, when it
    // is the choice, loads Streamline in its probe: about a second). A build without that backend
    // never starts one, and nothing is waited for.
    // CHECK: that the game makes its renderer after the renderer has started the probe (in a log,
    // the "[gpu] ffx: ready" or "[gpu] dlss: ready" line comes before "[gpu] msaa:" and "[gpu]
    // upscaler at start:"); if not, the hold is missed and logged.
    const auto ended = [chosen] {
      const UpscalerAvailability a = GetUpscalerAvailability();
      return chosen == Upscaler::kDlss ? a.dlss_probed : a.probed;
    };
#if defined(LARECOMP_HAS_FFX) || defined(LARECOMP_HAS_DLSS)
#if defined(LARECOMP_HAS_FFX)
    constexpr bool kHasFfx = true;
#else
    constexpr bool kHasFfx = false;
#endif
#if defined(LARECOMP_HAS_DLSS)
    constexpr bool kHasDlss = true;
#else
    constexpr bool kHasDlss = false;
#endif
    const bool has_backend = chosen == Upscaler::kDlss ? kHasDlss : kHasFfx;
    constexpr auto kProbeWait = std::chrono::seconds(4);  // the game's thread, at its renderer's creation
    const auto give_up = std::chrono::steady_clock::now() + kProbeWait;
    while (has_backend && !ended() && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    const UpscalerAvailability a = GetUpscalerAvailability();
    probed = ended();
    runs = probed && ((chosen == Upscaler::kFsr31 && a.fsr31) || (chosen == Upscaler::kFsr4 && a.fsr4) ||
                      (chosen == Upscaler::kDlss && a.dlss));
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
                  UpscalerName(chosen), ModeInfoOf(chosen, mode).name,
                  ModeInfoOf(chosen, mode).percent, ModeInfoOf(chosen, mode).ratio);
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
  s.ratio = s.runs ? ModeInfoOf(s.upscaler, s.mode).ratio : 1.0;
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

#if !defined(LARECOMP_HAS_DLSS)
// A build without the Streamline headers (cmake/FetchStreamline.cmake): no backend, nothing listed,
// nothing loaded.
std::unique_ptr<TemporalBackend> MakeDlssBackend() {
  return nullptr;
}
#endif

}  // namespace mcla::gpu
