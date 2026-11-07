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
    "HDR output for an HDR10 display. off: the usual picture. auto: HDR when Windows has HDR "
    "turned on for the display the game is on. on: HDR even if Windows does not say its HDR is "
    "on; where the display or the driver cannot show it the usual picture is used and the log "
    "says why. The HUD and the menus are shown at your paper white setting; the game's lights, "
    "the sun, the sky at dusk and the headlights go above it (how far: mcla_gpu_hdr_highlights). "
    "Takes effect at the next start.")
    .allowed({"off", "auto", "on"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
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
Mode g_mode = Mode::kOff;            // lifecycle thread; the poll thread starts after it is set
#if defined(_WIN32)
HWND g_window = nullptr;
#endif
std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;  // g_mutex
// The display's HDR state the poll last reported after the swap chain was made (poll thread).
bool g_reported_hdr_after_start = false;

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

// Reads the display and publishes it; logs what changed.
void Poll() {
  DisplayInfo info;
#if defined(_WIN32)
  if (g_window) {
    ReadDisplay(g_window, &info);
  }
#endif
  const bool changed = info.known != g_display.known.load() ||
                       info.hdr_enabled != g_display.hdr_enabled.load() ||
                       info.peak_nits != g_display.peak_nits.load() ||
                       info.sdr_white_nits != g_display.sdr_white_nits.load();
  g_display.known.store(info.known);
  g_display.hdr_enabled.store(info.hdr_enabled);
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
                info.hdr_enabled ? "on" : "off", info.peak_nits, info.full_frame_nits,
                info.min_nits, info.sdr_white_nits);
  } else {
    REXLOG_INFO("[gpu] hdr: the window's display could not be read; AUTO levels use {:.0f} nits "
                "paper white and {:.0f} nits peak",
                kFallbackPaperWhiteNits, kFallbackPeakNits);
  }
  // Windows' HDR switch moved after the swap chain was made: the chain keeps its format until
  // the next start.
  if (g_decided.load() && info.known && info.hdr_enabled != g_reported_hdr_after_start) {
    g_reported_hdr_after_start = info.hdr_enabled;
    const bool active = g_active.load();
    if (active && !info.hdr_enabled) {
      REXLOG_WARN("[gpu] hdr: Windows HDR was turned off for this display while the game shows "
                  "HDR10; the picture looks wrong until the game is restarted");
    } else if (!active && info.hdr_enabled && g_mode == Mode::kAuto) {
      REXLOG_INFO("[gpu] hdr: Windows HDR was turned on for this display; the game shows HDR "
                  "from the next start (mcla_gpu_hdr = auto)");
    }
  }
}

void PollLoop() {
  std::unique_lock<std::mutex> lock(g_mutex);
  while (!g_stop) {
    lock.unlock();
    Poll();
    lock.lock();
    // Windows does not reliably announce a change of the SDR content brightness: ask each second.
    g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop; });
  }
}

}  // namespace

namespace hdr {

void Start(plume::RenderWindow window) {
  Stop();
  g_active.store(false);
  g_decided.store(false);
  g_mode = ParseMode(REXCVAR_GET(mcla_gpu_hdr));
  if (g_mode == Mode::kOff) {
    return;
  }
#if defined(_WIN32)
  g_window = window;
#else
  (void)window;
  REXLOG_INFO("[gpu] hdr: the display's levels are only read on Windows; AUTO levels use the "
              "fallbacks");
#endif
  Poll();  // the first answer is there before the swap chain asks
  g_reported_hdr_after_start = g_display.hdr_enabled.load();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
  }
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
  if (g_mode == Mode::kOff) {
    return false;
  }
  if (g_mode == Mode::kOn) {
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
  if (g_mode == Mode::kOff) {
    return;
  }
  const HdrLevels levels = Levels();
  REXLOG_INFO("[gpu] hdr: {} (mcla_gpu_hdr = {}), paper white {:.0f} nits, peak {:.0f} nits",
              active ? "HDR10 output on" : "SDR output", ModeName(g_mode), levels.white,
              levels.peak);
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
