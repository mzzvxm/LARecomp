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
    "HUD and the bloom, glow and tone map stay at full size. The picture it makes is the size of "
    "the window (its 16:9 area), whatever mcla_gpu_resolution_scale says, and the scene is drawn "
    "at the mode's share of that. While one is on, FXAA and SMAA are "
    "held off and MSAA is 1 sample (your MSAA choice, mcla_gpu_msaa, is kept and comes back when "
    "the upscaler is off). A choice this PC cannot run is treated as off. Live: turning an "
    "upscaler on, off or to another one is made at a frame boundary once the new one is ready, "
    "while the old one keeps working (FSR in about half a "
    "second, DLSS in several seconds). The exception is DLSS on Vulkan: that graphics device is "
    "made through NVIDIA's Streamline only when dlss is the saved choice at the start (it adds "
    "several seconds to every start), so choosing dlss in a Vulkan game started with something "
    "else is saved and applies at the next start, and the menu offers a restart. Until then the "
    "game keeps what it runs (the upscaler and its mode as they were, or none), it is not turned "
    "off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr31_mode, "native_aa", "MCLA/GPU",
    "FSR 3.1's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the output (the window's 16:9 area, whatever "
    "mcla_gpu_resolution_scale says). Live: the scene is drawn at the new size from a frame "
    "boundary once the upscaler is ready for it, and the old mode works until then.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_gpu_fsr4_mode, "native_aa", "MCLA/GPU",
    "FSR 4's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the output (the window's 16:9 area, whatever "
    "mcla_gpu_resolution_scale says). Live: the scene is drawn at the new size from a frame "
    "boundary once the upscaler is ready for it, and the old mode works until then.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_gpu_dlss_mode, "dlaa", "MCLA/GPU",
    "DLSS's mode, NVIDIA's names and the share of the output the scene is drawn at: dlaa (100%, "
    "only smoothed), quality (67%), balanced (58%), performance (50%) or ultra_performance (33%), "
    "per direction, of the output (the window's 16:9 area, whatever mcla_gpu_resolution_scale "
    "says). Live: the scene is drawn at the new size from a frame boundary once the upscaler is "
    "ready for it (several seconds for DLSS), and the old mode works until then.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_upscaler_sharpness, 0.2, "MCLA/GPU",
    "How much the FSR upscalers sharpen their picture (AMD's RCAS pass), 0 to 1: 0 adds no "
    "sharpening, 1 the most. DLSS has no sharpening. Live.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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
// DLSS: the modes that cannot run at the output size (bit i = UpscaleMode i).
std::atomic<uint32_t> g_dlss_refused{0};
std::atomic<uint32_t> g_running{0};
std::atomic<uint32_t> g_kept{0};
std::atomic<bool> g_switching{false};

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
  // start's choice: GetStartUpscale). DLSS can refuse a mode at some output sizes (ModeVerdicts):
  // that is not decided here but by the backend, for the sizes the renderer works at.
  return upscaler != Upscaler::kOff && size_t(mode) < size_t(UpscaleMode::kCount);
}

UpscaleMode SavedUpscaleMode(Upscaler upscaler) {
  const std::string mode = upscaler == Upscaler::kFsr4   ? REXCVAR_GET(mcla_gpu_fsr4_mode)
                           : upscaler == Upscaler::kDlss ? REXCVAR_GET(mcla_gpu_dlss_mode)
                                                         : REXCVAR_GET(mcla_gpu_fsr31_mode);
  UpscaleMode parsed = UpscaleMode::kNativeAa;
  ParseUpscaleMode(mode, &parsed);
  return ModeRuns(upscaler, parsed) ? parsed : UpscaleMode::kNativeAa;
}

UpscalerSettings ReadUpscalerSettings() {
  UpscalerSettings out;
  ParseUpscaler(REXCVAR_GET(mcla_gpu_upscaler), &out.upscaler);
  out.mode = SavedUpscaleMode(out.upscaler);
  out.sharpness = float(std::clamp(REXCVAR_GET(mcla_gpu_upscaler_sharpness), 0.0, 1.0));
  return out;
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

void PublishDlssRefusedModes(uint32_t refused) {
  g_dlss_refused.store(refused, std::memory_order_relaxed);
}

void PublishRunningUpscaler(Upscaler running) {
  g_running.store(uint32_t(running), std::memory_order_relaxed);
}

void PublishKeptUpscaler(Upscaler kept) {
  g_kept.store(uint32_t(kept), std::memory_order_relaxed);
}

void PublishUpscalerSwitching(bool switching) {
  g_switching.store(switching, std::memory_order_relaxed);
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
  a.dlss_modes_refused = g_dlss_refused.load(std::memory_order_relaxed);
  a.running = Upscaler(g_running.load(std::memory_order_relaxed) & 3u);
  a.kept = Upscaler(g_kept.load(std::memory_order_relaxed) & 3u);
  a.switching = g_switching.load(std::memory_order_relaxed);
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
    // started with the renderer (before the game runs) and take well under a second. DLSS's
    // answer does not wait for Streamline's own start (slInit and NGX: from 2 s to over 10 s on a
    // cold start): an RTX card whose files are in place and signed counts as able to run it as soon
    // as the worker has looked at them (temporal_dlss.cpp PublishEarlyVerdict), and the rest of the
    // start finishes in the background; if it fails, the backend says so plainly and the game
    // goes on without DLSS. So a slow Streamline start never changes the player's mode here. A
    // build without that backend never starts a probe, and nothing is waited for.
    // CHECK: that the game makes its renderer after the renderer has started the probe (in a log,
    // the "[gpu] ffx: ready" line, or for DLSS the "[gpu] dlss: ... counts as able to run" line,
    // comes before "[gpu] upscaler at start:"); if not, the start is decided as not able to run
    // and logged so.
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
                  "3D scene starts at that share of the output (the window's, the screen's scale: "
                  "see the \"render scale\" line; the upscaler and its mode can be changed while "
                  "the game runs, at a frame boundary)",
                  UpscalerName(chosen), ModeInfoOf(chosen, mode).name,
                  ModeInfoOf(chosen, mode).percent, ModeInfoOf(chosen, mode).ratio);
    } else {
      REXLOG_INFO("[gpu] upscaler at start: {} is chosen but {}: the scene is drawn at the render "
                  "scale",
                  UpscalerName(chosen),
                  probed ? "this PC cannot run it"
                         : chosen == Upscaler::kDlss
                               ? "Streamline did not say within the wait whether this card can run "
                                 "it (this is not an RTX card by its name, or its start is very slow)"
                               : "its probe has not ended (or this build has no upscaler runtime)");
    }
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
