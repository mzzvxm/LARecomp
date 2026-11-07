// MCLA GPU renderer -- HDR10 output: the settings and the display's levels. See hdr.h.
//
// The display is read as the ReXGlue fork of this game's HDR output reads it (DXGI
// IDXGIOutput6::GetDesc1 for the colour space and the peak, DisplayConfig's SDR white level for
// the SDR content brightness), found from the window's monitor over all adapters.

#include "hdr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#endif

REXCVAR_DEFINE_STRING(
    mcla_gpu_hdr, "off", "MCLA/GPU",
    "HDR output for an HDR10 display. off: the usual picture. auto: HDR while Windows has HDR "
    "turned on for the display the game is on, following Windows' HDR switch and the display the "
    "window is moved to. on: HDR even if Windows does not say its HDR is on; where the display or "
    "the driver cannot show it the usual picture is used and the log says why. The HUD and the "
    "menus are shown at your paper white setting; the game's lights, the sun, the sky at dusk and "
    "the headlights go above it (how far: mcla_gpu_hdr_highlights). Takes effect within a moment, "
    "at the end of a frame; the picture may flicker once while the window's swap chain is made "
    "again.")
    .allowed({"off", "auto", "on"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_hdr_paper_white_nits, 0.0, "MCLA/GPU",
    "How bright the game's white is in HDR, in nits (Windows' own slider covers 80 to 480). 0: "
    "the Windows SDR content brightness setting, or 200 when Windows does not report it. "
    "Changes at once.")
    .range(0.0, 10000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_hdr_peak_nits, 0.0, "MCLA/GPU",
    "The brightest level HDR output may reach, in nits (200 to 4000 covers most displays). 0: "
    "what the display reports, or 1000 when it reports nothing. Changes at once.")
    .range(0.0, 10000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_hdr_test_pattern, false, "MCLA/GPU",
    "In HDR, shows brightness bars instead of the game. The top row starts at 100 nits and "
    "doubles every 4 bars, to see where the display stops showing steps; the bottom left is "
    "paper white and the bottom right the same bars rolled off to the peak setting. It covers the "
    "whole picture, the menus too. The pause menu's TEST PATTERN row shows it only while that row "
    "is highlighted and never saves it; set on the command line it stays on for the run. Changes at "
    "once.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

enum class Mode : uint8_t { kOff, kAuto, kOn };

// What the poll found, as atomics: the render thread and the menu read them, the poll thread
// writes them.
struct DisplayState {
  std::atomic<bool> known{false};
  std::atomic<bool> hdr_enabled{false};
  std::atomic<float> peak_nits{0.0f};        // 0: not reported
  std::atomic<float> full_frame_nits{0.0f};  // 0: not reported
  std::atomic<float> min_nits{0.0f};
  std::atomic<float> sdr_white_nits{0.0f};   // 0: not reported
};

struct DisplayInfo {
  bool known = false;
  bool hdr_enabled = false;
  float peak_nits = 0.0f;
  float full_frame_nits = 0.0f;
  float min_nits = 0.0f;
  float sdr_white_nits = 0.0f;
};

// What AUTO falls back to when the display reports nothing usable.
constexpr float kFallbackPeakNits = 1000.0f;
constexpr float kFallbackPaperWhiteNits = 200.0f;

DisplayState g_display;
std::atomic<bool> g_active{false};
std::atomic<bool> g_decided{false};  // SetActive ran
// The setting, read by the render thread at each frame boundary (SyncMode) and by Start; the poll
// thread only asks whether it is off. `g_mode_text` is the cvar text it was parsed from.
std::atomic<Mode> g_mode{Mode::kOff};
std::string g_mode_text;
#if defined(_WIN32)
HWND g_window = nullptr;
#endif
std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;  // g_mutex
// Poll() runs on the poll thread each second and on the render thread when the setting leaves off;
// this guards the debounce below.
std::mutex g_poll_mutex;
// A change of what the display says (readable or not, HDR on or off) is published once two polls in
// a row agree (g_poll_mutex): a display that flickers through a mode change does not rebuild the
// swap chain twice.
bool g_candidate_seen = false;
bool g_candidate_known = false;
bool g_candidate_hdr = false;
// An attempt to change the output that the swap chain refused: not asked again until the setting or
// the display's state is another (render thread). The key is SwitchKey().
bool g_refused = false;
uint32_t g_refused_key = 0;

Mode ParseMode(const std::string& value) {
  if (value == "on") {
    return Mode::kOn;
  }
  return value == "auto" ? Mode::kAuto : Mode::kOff;
}

const char* ModeName(Mode mode) {
  return mode == Mode::kOn ? "on" : (mode == Mode::kAuto ? "auto" : "off");
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

// The SDR content brightness of the display whose GDI name is `device_name`, in nits; 0 when
// Windows does not report it. DisplayConfig gives it in thousandths of 80 nits.
// CHECK: DISPLAYCONFIG_SDR_WHITE_LEVEL is in the Windows 10 SDK headers only (wingdi.h, guarded by
// the target version); the fork's build had it, this one is not compiled here.
float ReadSdrWhiteNits(const wchar_t* device_name) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
      ERROR_SUCCESS) {
    return 0.0f;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return 0.0f;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        std::wcscmp(source.viewGdiDeviceName, device_name) != 0) {
      continue;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = paths[i].targetInfo.adapterId;
    white.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
      return float(white.SDRWhiteLevel) * (80.0f / 1000.0f);
    }
    return 0.0f;
  }
  return 0.0f;
}

// The display the window is mostly on, found by its monitor over every adapter's outputs: the
// swap chain's own adapter may not be the one the display hangs off (cross-adapter presentation).
bool ReadDisplay(HWND window, DisplayInfo* out) {
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor) {
    return false;
  }
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc = {};
      if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) ||
          desc.Monitor != monitor) {
        continue;
      }
      out->known = true;
      out->hdr_enabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      out->peak_nits = desc.MaxLuminance;
      out->full_frame_nits = desc.MaxFullFrameLuminance;
      out->min_nits = desc.MinLuminance;
      out->sdr_white_nits = ReadSdrWhiteNits(desc.DeviceName);
      return true;
    }
  }
  return false;
}

#endif  // _WIN32

// Whether the setting stands for HDR when the display says `display_hdr`.
bool WantsHdr(Mode mode, bool display_known, bool display_hdr) {
  return mode == Mode::kOn || (mode == Mode::kAuto && display_known && display_hdr);
}

// Reads the display and publishes it; logs what changed. `immediate`: what it says is published at
// once (the first answer, or a setting that has just left off); otherwise a change of its state
// waits for the next poll to say the same, a second later.
void Poll(bool immediate) {
  DisplayInfo info;
#if defined(_WIN32)
  if (g_window) {
    ReadDisplay(g_window, &info);
  }
#endif
  std::lock_guard<std::mutex> lock(g_poll_mutex);
  const bool known_before = g_display.known.load();
  const bool hdr_before = g_display.hdr_enabled.load();
  const bool hdr_enabled = info.hdr_enabled;
  if (!immediate && known_before && (!info.known || hdr_enabled != hdr_before)) {
    // A display that was readable now says another thing: believed at the second read in a row.
    if (!(g_candidate_seen && g_candidate_known == info.known && g_candidate_hdr == hdr_enabled)) {
      g_candidate_seen = true;
      g_candidate_known = info.known;
      g_candidate_hdr = hdr_enabled;
      return;
    }
  }
  g_candidate_seen = false;
  const bool changed = info.known != known_before || hdr_enabled != hdr_before ||
                       info.peak_nits != g_display.peak_nits.load() ||
                       info.sdr_white_nits != g_display.sdr_white_nits.load();
  g_display.known.store(info.known);
  g_display.hdr_enabled.store(hdr_enabled);
  g_display.peak_nits.store(info.peak_nits);
  g_display.full_frame_nits.store(info.full_frame_nits);
  g_display.min_nits.store(info.min_nits);
  g_display.sdr_white_nits.store(info.sdr_white_nits);
  if (!changed) {
    return;
  }
  if (info.known) {
    REXLOG_INFO("[gpu] hdr: the window's display reports HDR {}, peak {:.0f} nits, full frame "
                "{:.0f} nits, black {:.4f} nits, SDR white {:.0f} nits",
                hdr_enabled ? "on" : "off", info.peak_nits, info.full_frame_nits, info.min_nits,
                info.sdr_white_nits);
  } else {
    REXLOG_INFO("[gpu] hdr: the window's display could not be read; AUTO levels use {:.0f} nits "
                "paper white and {:.0f} nits peak",
                kFallbackPaperWhiteNits, kFallbackPeakNits);
  }
  // Windows' HDR switch moved after the swap chain was made. With auto the renderer follows it at
  // its next frame boundary (SwitchWanted); on keeps what it was asked for.
  if (g_decided.load() && info.known && hdr_enabled != hdr_before &&
      g_mode.load() == Mode::kOn && g_active.load() && !hdr_enabled) {
    REXLOG_INFO("[gpu] hdr: Windows HDR was turned off for this display; mcla_gpu_hdr = on keeps "
                "the HDR10 output (auto follows Windows)");
  }
}

void PollLoop() {
  std::unique_lock<std::mutex> lock(g_mutex);
  while (!g_stop) {
    lock.unlock();
    // Nothing to follow while the setting is off and the output is SDR.
    if (g_mode.load() != Mode::kOff || g_active.load()) {
      Poll(false);
    }
    lock.lock();
    // Windows does not reliably announce a change of the SDR content brightness: ask each second.
    g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop; });
  }
}

// The setting the render thread sees now, and the display read the moment it leaves off.
void SyncMode() {
  const std::string& text = REXCVAR_GET(mcla_gpu_hdr);
  if (text == g_mode_text) {
    return;
  }
  g_mode_text = text;
  const Mode before = g_mode.load();
  const Mode after = ParseMode(text);
  g_mode.store(after);
  if (before == Mode::kOff && after != Mode::kOff) {
    Poll(true);  // the display's state is wanted now, not at the poll's next second
  }
}

// What a refusal is remembered under: the setting, what Windows says and what was asked for.
uint32_t SwitchKey(Mode mode, bool to_hdr) {
  return uint32_t(mode) | (g_display.known.load() ? 4u : 0u) |
         (g_display.hdr_enabled.load() ? 8u : 0u) | (to_hdr ? 16u : 0u);
}

}  // namespace

namespace hdr {

void Start(plume::RenderWindow window) {
  Stop();
  g_active.store(false);
  g_decided.store(false);
  g_refused = false;
  g_mode_text = REXCVAR_GET(mcla_gpu_hdr);
  g_mode.store(ParseMode(g_mode_text));
#if defined(_WIN32)
  g_window = window;
#else
  (void)window;
  if (g_mode.load() != Mode::kOff) {
    REXLOG_INFO("[gpu] hdr: the display's levels are only read on Windows; AUTO levels use the "
                "fallbacks");
  }
#endif
  if (g_mode.load() != Mode::kOff) {
    Poll(true);  // the first answer is there before the swap chain asks
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
  }
  // Started whatever the setting is: it can be turned on later, and the thread does nothing while
  // it is off.
  g_thread = std::thread(PollLoop);
}

void Stop() {
  if (!g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  g_thread.join();
}

bool Wanted() {
  const Mode mode = g_mode.load();
  if (mode == Mode::kOff) {
    return false;
  }
  if (mode == Mode::kOn) {
    return true;
  }
  if (!g_display.known.load()) {
    REXLOG_INFO("[gpu] hdr: auto: the window's display state is not known; SDR");
    return false;
  }
  if (!g_display.hdr_enabled.load()) {
    REXLOG_INFO("[gpu] hdr: auto: Windows HDR is off for the display the window is on; SDR");
    return false;
  }
  return true;
}

void SetActive(bool active) {
  g_active.store(active);
  g_decided.store(true);
  const Mode mode = g_mode.load();
  if (mode == Mode::kOff) {
    return;
  }
  // The swap chain was asked for HDR10 and would not: not asked again for this state of the
  // setting and the display.
  if (!active && WantsHdr(mode, g_display.known.load(), g_display.hdr_enabled.load())) {
    g_refused = true;
    g_refused_key = SwitchKey(mode, true);
  }
  const HdrLevels levels = Levels();
  REXLOG_INFO("[gpu] hdr: {} (mcla_gpu_hdr = {}), paper white {:.0f} nits, peak {:.0f} nits",
              active ? "HDR10 output on" : "SDR output", ModeName(mode), levels.white,
              levels.peak);
}

bool SwitchWanted(bool current_hdr, bool* to_hdr, std::string* why) {
  SyncMode();
  const Mode mode = g_mode.load();
  const bool known = g_display.known.load();
  const bool display_hdr = g_display.hdr_enabled.load();
  const bool want = WantsHdr(mode, known, display_hdr);
  if (want == current_hdr) {
    return false;
  }
  if (g_refused && g_refused_key == SwitchKey(mode, want)) {
    return false;
  }
  *to_hdr = want;
  if (why) {
    *why = std::string("mcla_gpu_hdr = ") + ModeName(mode);
    if (mode == Mode::kAuto) {
      *why += want ? ", Windows HDR is on for the display" : ", Windows HDR is off for the display";
    }
  }
  return true;
}

void SwitchDone(bool wanted_hdr, bool now_hdr) {
  g_active.store(now_hdr, std::memory_order_release);
  g_decided.store(true);
  if (wanted_hdr != now_hdr) {
    g_refused = true;
    g_refused_key = SwitchKey(g_mode.load(), wanted_hdr);
  } else {
    g_refused = false;
  }
}

bool Active() {
  return g_active.load(std::memory_order_acquire);
}

AutoLevels Auto() {
  AutoLevels levels;
  const float peak = g_display.peak_nits.load();
  const float white = g_display.sdr_white_nits.load();
  levels.from_display = g_display.known.load();
  if (peak >= 80.0f) {
    levels.peak = peak;
  }
  if (white >= 40.0f) {
    levels.paper_white = white;
  }
  return levels;
}

HdrLevels Levels() {
  const AutoLevels automatic = Auto();
  const double peak_setting = REXCVAR_GET(mcla_gpu_hdr_peak_nits);
  const double white_setting = REXCVAR_GET(mcla_gpu_hdr_paper_white_nits);
  HdrLevels levels;
  levels.peak = peak_setting > 0.0 ? std::clamp(float(peak_setting), 80.0f, 10000.0f)
                                   : automatic.peak;
  levels.white = white_setting > 0.0 ? std::clamp(float(white_setting), 20.0f, 10000.0f)
                                     : automatic.paper_white;
  // White above the peak would only be the roll-off's work.
  levels.white = std::min(levels.white, levels.peak);
  levels.knee = 0.75f * levels.peak;
  levels.flags = REXCVAR_GET(mcla_gpu_hdr_test_pattern) ? kHdrFlagTestPattern : 0u;
  return levels;
}

plume::RenderHdrMetadata Metadata() {
  const HdrLevels levels = Levels();
  const float full_frame = g_display.full_frame_nits.load();
  plume::RenderHdrMetadata metadata;
  metadata.maxMasteringLuminance = levels.peak;
  metadata.minMasteringLuminance = g_display.min_nits.load();
  metadata.maxContentLightLevel = levels.peak;
  metadata.maxFrameAverageLightLevel = full_frame > 0.0f ? std::min(full_frame, levels.peak) : 0.0f;
  return metadata;
}

}  // namespace hdr

// gpu.h: for the pause menu.
bool HdrOutputActive() {
  return hdr::Active();
}

float HdrAutoPaperWhiteNits() {
  return hdr::Auto().paper_white;
}

float HdrAutoPeakNits() {
  return hdr::Auto().peak;
}

}  // namespace mcla::gpu
