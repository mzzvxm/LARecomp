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

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "upscale_modes.h"
#include "upscaler_load_policy.h"

namespace mcla::gpu {

bool UpscalerDebugView(Upscaler upscaler) {
  return (upscaler == Upscaler::kFsr31 || upscaler == Upscaler::kFsr4) &&
         larecomp::settings::mcla_gpu_fsr_debug_view.Get();
}

namespace {

// Packed for one atomic: bit 0 probed, 1 fsr31, 2 fsr4.
std::atomic<uint32_t> g_availability{0};
// DLSS: bit 0 probed, 1 dlss, 2 needs setup, 3 after restart.
std::atomic<uint32_t> g_dlss_availability{0};
// DLSS: the modes that cannot run at the output size (bit i = UpscaleMode i).
std::atomic<uint32_t> g_dlss_refused{0};
std::atomic<uint32_t> g_running{0};
std::atomic<uint32_t> g_kept{0};
std::atomic<bool> g_switching{false};
std::atomic<bool> g_no_room{false};

}  // namespace

// The words, the mode tables and the parses are upscale_modes.h's (pure, upscale_modes_test.cpp); these keep the names the renderer uses.
const char* UpscalerCvarValue(Upscaler upscaler) {
  return upscale_modes::CvarValue(upscaler);
}

const char* UpscalerName(Upscaler upscaler) {
  return upscale_modes::Name(upscaler);
}

bool ParseUpscaler(std::string_view text, Upscaler* out) {
  return upscale_modes::ParseUpscaler(text, out);
}

const ModeInfo& ModeInfoOf(Upscaler upscaler, UpscaleMode mode) {
  return upscale_modes::InfoOf(upscaler, mode);
}

bool ParseUpscaleMode(std::string_view text, UpscaleMode* out) {
  return upscale_modes::ParseMode(text, out);
}

bool ModeRuns(Upscaler upscaler, UpscaleMode mode) {
  return upscale_modes::Runs(upscaler, mode);
}

UpscaleMode SavedUpscaleMode(Upscaler upscaler) {
  const std::string mode = upscaler == Upscaler::kFsr4   ? larecomp::settings::mcla_gpu_fsr4_mode.Get()
                           : upscaler == Upscaler::kDlss ? larecomp::settings::mcla_gpu_dlss_mode.Get()
                                                         : larecomp::settings::mcla_gpu_fsr31_mode.Get();
  UpscaleMode parsed = UpscaleMode::kNativeAa;
  ParseUpscaleMode(mode, &parsed);
  return ModeRuns(upscaler, parsed) ? parsed : UpscaleMode::kNativeAa;
}

UpscalerSettings ReadUpscalerSettings() {
  UpscalerSettings out;
  ParseUpscaler(larecomp::settings::mcla_gpu_upscaler.Get(), &out.upscaler);
  out.mode = SavedUpscaleMode(out.upscaler);
  out.sharpness = float(std::clamp(larecomp::settings::mcla_gpu_upscaler_sharpness.Get(), 0.0, 1.0));
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

void PublishUpscalerNoRoom(bool no_room) {
  g_no_room.store(no_room, std::memory_order_relaxed);
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
  a.no_room = g_no_room.load(std::memory_order_relaxed);
  return a;
}

namespace {
upscaler_load::Interest g_interest;
}  // namespace

bool UpscalerLazyLoad() {
  return larecomp::settings::mcla_gpu_upscaler_lazy_load.Get();
}

void NoteUpscalerInterest(const char* why) {
  if (g_interest.Noted()) {
    return;  // the common case: the pause menu asks on every draw of its row
  }
  if (g_interest.Note()) {
    REXLOG_INFO("[gpu] upscalers: {}: the DLSS warm-up (Streamline's start and one feature per mode, "
                "in the background; D3D12 on an RTX card only) is allowed to begin{}",
                why ? why : "interest noted",
                UpscalerLazyLoad() ? "" : " (mcla_gpu_upscaler_lazy_load is off: it began without "
                                          "waiting for this)");
  }
}

bool UpscalerInterestNoted() {
  return g_interest.Noted();
}

namespace {

// The start's upscaler (gpu.h StartUpscale), packed for one atomic: bit 0 decided, 1 probed, 2
// runs, bits 4-5 the upscaler, bits 8-11 the mode.
std::atomic<uint32_t> g_start_upscale{0};

void DecideStartUpscale() {
  Upscaler chosen = Upscaler::kOff;
  ParseUpscaler(larecomp::settings::mcla_gpu_upscaler.Get(), &chosen);
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
