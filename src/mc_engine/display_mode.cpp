// The game window's display mode. See display_mode.h.

#include "display_mode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "logging.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(fullscreen_exclusive, false, "UI/Window",
    "With fullscreen on: exclusive fullscreen. The display is switched to the size of `resolution` "
    "(the nearest mode it lists; the display's own size when `resolution` is empty) while the game "
    "has the focus, and put back when the game loses it or closes. Off is borderless fullscreen: a "
    "window without a frame the size of the display, the display left as it is. Windows only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::display {

namespace {

// The game's own screen: no display mode or window below this is offered.
constexpr uint32_t kMinWidth = 1280;
constexpr uint32_t kMinHeight = 720;

std::atomic<rex::ui::Window*> g_window{nullptr};
// Set once, on the UI thread, with the window.
std::atomic<void*> g_native{nullptr};
// A change is queued for the UI thread (the settings that decide it are read when it runs).
std::atomic<bool> g_pending{false};

bool Flag(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }

int IntFlag(const char* name) {
  return int(std::strtol(rex::cvar::GetFlagByName(name).c_str(), nullptr, 10));
}

void ScheduleApply(int pass);

void SettingChanged(std::string_view, std::string_view) { ScheduleApply(0); }

}  // namespace

// ── Settings, sizes (every platform) ───────────────────────────────────────────────────────────

bool ExclusiveSupported() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool LiveResize() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

Mode WantedMode() {
  if (!Flag("fullscreen")) return Mode::kWindowed;
  return Flag("fullscreen_exclusive") && ExclusiveSupported() ? Mode::kExclusive : Mode::kBorderless;
}

bool ParseSize(std::string_view text, Size* out) {
  int32_t width = 0;
  int32_t height = 0;
  if (!rex::graphics::video_mode_util::TryParseResolutionPreset(text, width, height)) return false;
  if (width <= 0 || height <= 0) return false;
  out->width = uint32_t(width);
  out->height = uint32_t(height);
  return true;
}

std::string SizeText(Size size) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%ux%u", size.width, size.height);
  return buf;
}

std::string SizeLabel(Size size) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%uX%u", size.width, size.height);
  return buf;
}

Size WantedWindowedSize() {
  // As the SDK reads them when it makes the window (window_sdl.cpp ResolveWindowWidth): the window_*
  // settings first, each on its own, then the `resolution` text, then the app's 1280x720.
  Size preset;
  const bool have_preset = ParseSize(rex::cvar::GetFlagByName("resolution"), &preset);
  const int width = IntFlag("window_width");
  const int height = IntFlag("window_height");
  Size size;
  size.width = width > 0 ? uint32_t(width) : (have_preset ? preset.width : kMinWidth);
  size.height = height > 0 ? uint32_t(height) : (have_preset ? preset.height : kMinHeight);
  return size;
}

namespace {

// The listed size closest to `want` (the sum of the two differences); the first when `want` is empty.
Size Nearest(const std::vector<Size>& sizes, Size want) {
  Size best = sizes.empty() ? want : sizes.front();
  int64_t best_distance = INT64_MAX;
  for (const Size& s : sizes) {
    const int64_t distance =
        std::abs(int64_t(s.width) - int64_t(want.width)) + std::abs(int64_t(s.height) - int64_t(want.height));
    if (distance < best_distance) {
      best_distance = distance;
      best = s;
    }
  }
  return best;
}

}  // namespace

Size WantedExclusiveSize() {
  Size want;
  if (!ParseSize(rex::cvar::GetFlagByName("resolution"), &want)) want = DesktopSize();
  const std::vector<Size> sizes = ExclusiveSizes();
  return sizes.empty() ? want : Nearest(sizes, want);
}

#if defined(_WIN32)

// ── Windows ────────────────────────────────────────────────────────────────────────────────────

namespace {

// What the display lists, for the display the window is on. The list is asked of the driver (a few
// milliseconds), and the menu asks every frame it is open, so it is kept for a few seconds.
struct ModeRecord {
  Size size;
  uint32_t hz = 0;
};
struct DisplayInfo {
  std::wstring device;
  Size desktop;
  uint32_t desktop_hz = 0;
  std::vector<ModeRecord> modes;
  std::vector<Size> sizes;
  ULONGLONG when = 0;
};
constexpr ULONGLONG kInfoLifetimeMs = 5000;

std::mutex g_info_mutex;
DisplayInfo g_info;

// The display's own mode: what the player set in Windows, not one this game switched to.
bool DesktopMode(const wchar_t* device, DEVMODEW* dm) {
  *dm = {};
  dm->dmSize = sizeof(DEVMODEW);
  if (EnumDisplaySettingsExW(device, ENUM_REGISTRY_SETTINGS, dm, 0)) return true;
  *dm = {};
  dm->dmSize = sizeof(DEVMODEW);
  return EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, dm, 0) != 0;
}

DisplayInfo ReadDisplay(const std::wstring& device) {
  DisplayInfo info;
  info.device = device;
  DEVMODEW dm = {};
  if (DesktopMode(device.c_str(), &dm)) {
    info.desktop = {dm.dmPelsWidth, dm.dmPelsHeight};
    info.desktop_hz = dm.dmDisplayFrequency;
  }
  // Every mode the display lists (not the raw ones it can be driven at), 32 bit, not interlaced, from
  // the game's own screen up.
  for (DWORD i = 0;; ++i) {
    dm = {};
    dm.dmSize = sizeof(DEVMODEW);
    if (!EnumDisplaySettingsExW(device.c_str(), i, &dm, 0)) break;
    if (dm.dmBitsPerPel < 32) continue;
    if ((dm.dmFields & DM_DISPLAYFLAGS) && (dm.dmDisplayFlags & DM_INTERLACED)) continue;
    if (dm.dmPelsWidth < kMinWidth || dm.dmPelsHeight < kMinHeight) continue;
    info.modes.push_back({{dm.dmPelsWidth, dm.dmPelsHeight}, dm.dmDisplayFrequency});
  }
  for (const ModeRecord& m : info.modes) {
    if (std::find(info.sizes.begin(), info.sizes.end(), m.size) == info.sizes.end()) {
      info.sizes.push_back(m.size);
    }
  }
  std::sort(info.sizes.begin(), info.sizes.end(), [](const Size& a, const Size& b) {
    const uint64_t area_a = uint64_t(a.width) * a.height;
    const uint64_t area_b = uint64_t(b.width) * b.height;
    return area_a != area_b ? area_a < area_b : a.width < b.width;
  });
  info.when = GetTickCount64();
  return info;
}

HWND NativeWindow() { return static_cast<HWND>(g_native.load(std::memory_order_acquire)); }

bool MonitorOf(HWND hwnd, MONITORINFOEXW* info) {
  if (!hwnd) return false;
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return false;
  *info = {};
  info->cbSize = sizeof(MONITORINFOEXW);
  return GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(info)) != 0;
}

// The display the window is on, as ReadDisplay saw it not long ago.
DisplayInfo CurrentDisplay() {
  MONITORINFOEXW monitor;
  if (!MonitorOf(NativeWindow(), &monitor)) return {};
  std::lock_guard lock(g_info_mutex);
  if (g_info.device != monitor.szDevice || GetTickCount64() - g_info.when > kInfoLifetimeMs) {
    g_info = ReadDisplay(monitor.szDevice);
  }
  return g_info;
}

template <typename Fn>
Fn User32(const char* name) {
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  const FARPROC proc = user32 ? GetProcAddress(user32, name) : nullptr;
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
}

UINT WindowDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static const Fn fn = User32<Fn>("GetDpiForWindow");
  const UINT dpi = fn ? fn(hwnd) : 0;
  return dpi ? dpi : 96;
}

// The outer rectangle for a client rectangle, at the window's DPI where Windows can say it
// (Windows 10 1607 and later).
void AdjustForWindow(HWND hwnd, RECT* rc, DWORD style, DWORD ex_style) {
  using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
  static const Fn fn = User32<Fn>("AdjustWindowRectExForDpi");
  if (fn) {
    fn(rc, style, FALSE, ex_style, WindowDpi(hwnd));
  } else {
    AdjustWindowRectEx(rc, style, FALSE, ex_style);
  }
}

// What the window's frame takes from the display's work area, for the window SDL makes (a titled,
// sizable window).
Size FrameOf(HWND hwnd) {
  RECT rc = {0, 0, 0, 0};
  AdjustForWindow(hwnd, &rc, WS_OVERLAPPEDWINDOW, 0);
  return {uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top)};
}

// ── Changing the display's mode (UI thread) ────────────────────────────────────────────────────

bool g_mode_changed = false;     // this game switched the display to g_changed_to
std::wstring g_changed_device;   // the display it did that on
Size g_changed_to;

// The frameless window of a fullscreen state covers the display. SDL follows a change of the display's
// mode by itself (it listens for WM_DISPLAYCHANGE); this only makes sure of it.
// CHECK: that SDL resizes its borderless fullscreen window to the new desktop size after
// ChangeDisplaySettingsEx (SDL_windowsevents.c WM_DISPLAYCHANGE -> WIN_RefreshDisplays); if it does
// not, this call does it and the window's size in SDL is as wrong as the log's "window moved" says.
void FitFullscreenWindow(HWND hwnd) {
  MONITORINFOEXW monitor;
  if (!MonitorOf(hwnd, &monitor)) return;
  RECT now = {};
  GetWindowRect(hwnd, &now);
  const RECT& area = monitor.rcMonitor;
  if (now.left == area.left && now.top == area.top && now.right == area.right &&
      now.bottom == area.bottom) {
    return;
  }
  MC_INFO("[display] the fullscreen window ({}x{} at {},{}) did not follow the display ({}x{}): moved",
          now.right - now.left, now.bottom - now.top, now.left, now.top, area.right - area.left,
          area.bottom - area.top);
  SetWindowPos(hwnd, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

// Puts the display's own mode back. True when there was one to put back.
bool RestoreDesktopMode(const char* why, bool fit_window) {
  if (!g_mode_changed) return false;
  g_mode_changed = false;
  const LONG result = ChangeDisplaySettingsExW(g_changed_device.c_str(), nullptr, nullptr, 0, nullptr);
  if (result != DISP_CHANGE_SUCCESSFUL) {
    MC_WARN("[display] the display's own mode could not be put back ({}): code {}", why, result);
    return true;
  }
  MC_INFO("[display] the display's own mode is back ({})", why);
  rex::ui::Window* window = g_window.load();
  if (fit_window && window && window->IsFullscreen()) FitFullscreenWindow(NativeWindow());
  return true;
}

// Switches the display to `want` (a listed size; the display's own size means no switch). True when
// something changed.
// CHECK: ChangeDisplaySettingsExW with CDS_FULLSCREEN on the display of the window while SDL has the
// window in its borderless fullscreen; the mode reverts with the process (docs: "temporary in
// nature"), and RestoreDesktopMode runs at every exit this game has (display_mode.h).
bool EnterDesktopMode(HWND hwnd, Size want) {
  MONITORINFOEXW monitor;
  if (!MonitorOf(hwnd, &monitor)) return false;
  // The display this game already switched stays the one, wherever the window is now.
  const std::wstring device = g_mode_changed ? g_changed_device : std::wstring(monitor.szDevice);
  const DisplayInfo info = ReadDisplay(device);
  if (want.empty()) want = info.desktop;
  if (want == info.desktop) {
    return RestoreDesktopMode("the display's own size is the choice", true);
  }
  if (g_mode_changed && g_changed_to == want) return false;

  // Its refresh rate: the display's own when the size has it, else the highest the size lists. The
  // row has no rate of its own; the FPS limit and V-SYNC say how fast the game runs.
  uint32_t hz = 0;
  for (const ModeRecord& m : info.modes) {
    if (!(m.size == want)) continue;
    if (m.hz == info.desktop_hz) {
      hz = m.hz;
      break;
    }
    hz = std::max(hz, m.hz);
  }
  if (hz == 0) {
    MC_WARN("[display] FULLSCREEN: the display lists no {}; staying borderless", SizeText(want));
    return RestoreDesktopMode("no such mode", true);
  }

  DEVMODEW dm = {};
  dm.dmSize = sizeof(DEVMODEW);
  dm.dmPelsWidth = want.width;
  dm.dmPelsHeight = want.height;
  dm.dmBitsPerPel = 32;
  dm.dmDisplayFrequency = hz;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  const LONG result = ChangeDisplaySettingsExW(device.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
  if (result != DISP_CHANGE_SUCCESSFUL) {
    MC_WARN("[display] FULLSCREEN: the display would not switch to {} at {} Hz (code {}); staying "
            "borderless",
            SizeText(want), hz, result);
    return RestoreDesktopMode("the switch failed", true);
  }
  g_mode_changed = true;
  g_changed_device = device;
  g_changed_to = want;
  MC_INFO("[display] FULLSCREEN: the display is now {} at {} Hz (its own mode is {} at {} Hz)",
          SizeText(want), hz, SizeText(info.desktop), info.desktop_hz);
  FitFullscreenWindow(hwnd);
  return true;
}

// ── Changing the window's size (UI thread) ─────────────────────────────────────────────────────

// The client area of the window made `want`, centred on the work area of its display. False when it
// already is, or when the window has no frame to size (SDL has not given it back yet).
// CHECK: SetWindowPos in the pixels SDL reports (the process is per-monitor DPI aware once SDL has
// started its video system; the SDK makes the first window with SizeToPhysical the same way).
bool ResizeWindow(HWND hwnd, Size want) {
  if (!hwnd || want.empty() || IsIconic(hwnd)) return false;
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (!(style & WS_CAPTION)) return false;
  if (style & WS_MAXIMIZE) {
    ShowWindow(hwnd, SW_RESTORE);
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  }
  RECT client = {};
  GetClientRect(hwnd, &client);
  if (uint32_t(client.right) == want.width && uint32_t(client.bottom) == want.height) return false;

  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  RECT outer = {0, 0, LONG(want.width), LONG(want.height)};
  AdjustForWindow(hwnd, &outer, DWORD(style), DWORD(ex_style));
  const LONG outer_width = outer.right - outer.left;
  const LONG outer_height = outer.bottom - outer.top;

  LONG x = 0;
  LONG y = 0;
  MONITORINFOEXW monitor;
  if (MonitorOf(hwnd, &monitor)) {
    const RECT& work = monitor.rcWork;
    x = std::max(work.left, work.left + ((work.right - work.left) - outer_width) / 2);
    y = std::max(work.top, work.top + ((work.bottom - work.top) - outer_height) / 2);
  }
  MC_INFO("[display] WINDOWED: the window is now {}x{} (was {}x{})", want.width, want.height,
          client.right, client.bottom);
  SetWindowPos(hwnd, nullptr, x, y, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);
  return true;
}

}  // namespace

Size OutputSize() {
  const HWND hwnd = NativeWindow();
  RECT rc = {};
  if (!hwnd || !GetClientRect(hwnd, &rc)) return {};
  return {uint32_t(rc.right), uint32_t(rc.bottom)};
}

Size DesktopSize() { return CurrentDisplay().desktop; }

std::vector<Size> ExclusiveSizes() { return CurrentDisplay().sizes; }

std::vector<Size> WindowedSizes() {
  const HWND hwnd = NativeWindow();
  std::vector<Size> sizes;
  MONITORINFOEXW monitor;
  if (!MonitorOf(hwnd, &monitor)) return sizes;
  const Size frame = FrameOf(hwnd);
  const RECT& work = monitor.rcWork;
  const uint32_t work_width = uint32_t(work.right - work.left);
  const uint32_t work_height = uint32_t(work.bottom - work.top);
  constexpr Size kSteps[] = {{1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440},
                             {3200, 1800}, {3840, 2160}, {5120, 2880}, {7680, 4320}};
  for (const Size& s : kSteps) {
    if (s.width + frame.width <= work_width && s.height + frame.height <= work_height) {
      sizes.push_back(s);
    }
  }
  // A display too small for the game's own screen still gets the smallest window.
  if (sizes.empty()) sizes.push_back(kSteps[0]);
  return sizes;
}

namespace {

// UI thread: makes the window what the settings say. Idempotent; reads the settings when it runs.
void ApplyNow(int pass) {
  rex::ui::Window* window = g_window.load();
  const HWND hwnd = NativeWindow();
  if (!window || !hwnd) return;
  bool changed = false;
  switch (WantedMode()) {
    case Mode::kWindowed:
      RestoreDesktopMode("the window is a window", false);
      if (window->IsFullscreen()) {
        window->SetFullscreen(false);
        changed = true;
      }
      changed |= ResizeWindow(hwnd, WantedWindowedSize());
      break;
    case Mode::kBorderless:
      changed |= RestoreDesktopMode("BORDERLESS", false);
      if (!window->IsFullscreen()) {
        window->SetFullscreen(true);
        changed = true;
      }
      break;
    case Mode::kExclusive:
      if (!window->IsFullscreen()) {
        window->SetFullscreen(true);
        changed = true;
      }
      changed |= EnterDesktopMode(hwnd, WantedExclusiveSize());
      break;
  }
  // SDL finishes some of this with the events that follow (the frame it gives back when the window
  // leaves fullscreen, the size it takes for the display): the settings are looked at once more.
  // CHECK: in the log, "[display] WINDOWED: the window is now ..." comes from the second look only
  // when SDL had not given the frame back by the first.
  if (changed && pass == 0) ScheduleApply(1);
}

}  // namespace

void Shutdown() {
  RestoreDesktopMode("the game is closing", false);
}

// CHECK: Alt+Tab out of FULLSCREEN and back: the display has its own mode while another window has the
// focus and the chosen one again afterwards, once each way (the mode change itself must not cause
// focus events that switch it back and forth); the log has one "[display] ... is back (the window
// lost the focus)" and one "[display] FULLSCREEN: the display is now ..." per round trip.
void OnFocusChanged(bool focused) {
  if (WantedMode() != Mode::kExclusive) return;
  if (focused) {
    ScheduleApply(0);
  } else {
    // Another window has the focus: the display gets its own mode back so that window is usable.
    RestoreDesktopMode("the window lost the focus", true);
  }
}

#else  // not Windows

// ── Elsewhere: the window's own fullscreen switch only ─────────────────────────────────────────

Size OutputSize() {
  rex::ui::Window* window = g_window.load();
  return window ? Size{window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight()} : Size{};
}

Size DesktopSize() { return {}; }
std::vector<Size> ExclusiveSizes() { return {}; }
std::vector<Size> WindowedSizes() { return {}; }

namespace {

void ApplyNow(int) {
  rex::ui::Window* window = g_window.load();
  if (!window) return;
  const bool fullscreen = WantedMode() != Mode::kWindowed;
  if (window->IsFullscreen() != fullscreen) window->SetFullscreen(fullscreen);
}

}  // namespace

void Shutdown() {}
void OnFocusChanged(bool) {}

#endif  // _WIN32

namespace {

void ScheduleApply(int pass) {
  rex::ui::Window* window = g_window.load();
  if (!window) return;
  if (g_pending.exchange(true)) return;  // one is queued: it reads the settings when it runs
  const bool queued = window->app_context().CallInUIThreadDeferred([pass]() {
    g_pending.store(false);
    ApplyNow(pass);
  });
  if (!queued) g_pending.store(false);
}

}  // namespace

void Initialize(rex::ui::Window* window) {
  if (!window) return;
  g_window.store(window);
#if defined(_WIN32)
  g_native.store(window->GetNativeWindowHandle(), std::memory_order_release);
#endif
  // The SDK's callback for `fullscreen` (and the app's before this) calls Window::SetFullscreen on the
  // thread that set the setting, with the settings registry locked: the pause menu's thread then
  // waited for the UI thread, which waited for that lock, and the game hung for good. These only
  // queue the change for the UI thread, where it runs once the lock is released; the SDK registers
  // no other callback for the name.
  rex::cvar::UnregisterChangeCallbacks("fullscreen");
  rex::cvar::RegisterChangeCallback("fullscreen", SettingChanged);
  rex::cvar::RegisterChangeCallback("fullscreen_exclusive", SettingChanged);
  rex::cvar::RegisterChangeCallback("resolution", SettingChanged);
  // The SDK opened the window borderless for a saved `fullscreen`; FULLSCREEN is ours to finish.
  if (WantedMode() == Mode::kExclusive) ScheduleApply(0);
}

}  // namespace mcla::display
