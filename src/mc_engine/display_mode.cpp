// The game window's display mode. See display_mode.h.

#include "display_mode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
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

namespace {

// The size `resolution` asks for the window, in PHYSICAL pixels (the client area; see display_mode.h
// for the units). False when `resolution` is empty or no size: the window is then left as it is.
//
// window_width and window_height are not looked at here. They are the SDK's start-up size in LOGICAL
// pixels (kRequiresRestart; at 150% scaling 1280 is a window of 1920 pixels), they win over
// `resolution` where the SDK makes the window, and until this was written they won here too: a
// harness that starts the game with window_width=1280 had every later write of `resolution` answered
// with 1280x720, and the window then already was that size, so no write did anything.
bool AskedWindowedSize(Size* out) {
  Size asked;
  if (!ParseSize(rex::cvar::GetFlagByName("resolution"), &asked)) return false;
  *out = asked;
  return true;
}

}  // namespace

Size WantedWindowedSize() {
  Size asked;
  if (AskedWindowedSize(&asked)) return asked;
  // No size asked: the window's own, in physical pixels, for the row to show what is true. Its client
  // area while it is a window; while it is not (fullscreen on the way to a window), the size the SDK
  // keeps for the window state (its logical size, at the window's DPI).
  rex::ui::Window* window = g_window.load();
  if (window && !window->IsFullscreen()) {
    const Size own = OutputSize();
    if (!own.empty()) return own;
  }
  if (window) {
    const Size kept = {window->SizeToPhysical(window->GetDesiredLogicalWidth()),
                       window->SizeToPhysical(window->GetDesiredLogicalHeight())};
    if (!kept.empty()) return kept;
  }
  return {kMinWidth, kMinHeight};
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

// What the display lists. The list is asked of the driver (a few milliseconds), and the menu asks every
// frame it is open, so it is kept for a few seconds, per display (ListedModes).
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
  // The size of the display's work area (its own size less the taskbar), as the monitor said it while
  // the display was in its own mode. Empty when that was not asked.
  Size work;
  ULONGLONG when = 0;
};
constexpr ULONGLONG kInfoLifetimeMs = 5000;

std::mutex g_info_mutex;
// The lists kept, by display, and the display this game has switched to another mode (empty: none).
// Both under g_info_mutex: the menu reads them from any thread, the UI thread changes the mode.
std::map<std::wstring, DisplayInfo> g_infos;
std::wstring g_switched_device;

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
  // The display's own mode is always one of its modes. The driver's list depends on the mode the display
  // is in (asked while this game had switched it to 1280x720, the list of a 3840x2160 display left its
  // own size out: the RESOLUTION row lost the size the player started at), so it is put in when the
  // list lacks it.
  if (info.desktop.width >= kMinWidth && info.desktop.height >= kMinHeight) {
    const bool listed = std::any_of(info.modes.begin(), info.modes.end(),
                                    [&](const ModeRecord& m) { return m.size == info.desktop; });
    if (!listed) info.modes.push_back({info.desktop, info.desktop_hz});
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

// What a display lists, as ReadDisplay saw it not long ago. The list is the display's own: it is asked
// of the driver only while the display is in its own mode. While this game has the display switched to
// another mode (FULLSCREEN at a lower size) the list from before the switch is kept and used, whatever
// its age: Windows answers for the mode the display is in, and a list asked then loses the sizes above
// it, among them the display's own (the owner's log of 2026-11-07: the RESOLUTION row listed 3840x2160
// for the 5 seconds the list from before the switch lived, and not after). SetSwitchedDevice drops it
// when the display's own mode is back, so the next ask is made in that mode again. `monitor`, when it
// is the display's, also gives the work area.
// CHECK: that the list in FULLSCREEN at 1280x720 stays what it was in BORDERLESS (the same sizes,
// 3840x2160 and every other one, however long the menu stays on the row), and comes back after the
// display's own mode is back; the log's "the display lists no ..." must not appear for a size the row
// offered.
DisplayInfo ListedModes(const std::wstring& device, const MONITORINFOEXW* monitor) {
  std::lock_guard lock(g_info_mutex);
  const auto kept = g_infos.find(device);
  const bool have = kept != g_infos.end();
  const bool switched = !g_switched_device.empty() && g_switched_device == device;
  const bool fresh = have && GetTickCount64() - kept->second.when <= kInfoLifetimeMs;
  if (!fresh && !(have && switched)) {
    DisplayInfo info = ReadDisplay(device);
    if (monitor && device == monitor->szDevice) {
      info.work = {uint32_t(monitor->rcWork.right - monitor->rcWork.left),
                   uint32_t(monitor->rcWork.bottom - monitor->rcWork.top)};
    } else if (have) {
      info.work = kept->second.work;
    }
    g_infos[device] = info;
    return info;
  }
  return kept->second;
}

// The display the window is on.
DisplayInfo CurrentDisplay() {
  MONITORINFOEXW monitor;
  if (!MonitorOf(NativeWindow(), &monitor)) return {};
  return ListedModes(monitor.szDevice, &monitor);
}

// This game has switched `device` to another mode, or (empty) has put the display's own mode back.
void SetSwitchedDevice(const std::wstring& device) {
  std::lock_guard lock(g_info_mutex);
  g_switched_device = device;
  if (device.empty()) {
    // The display is in its own mode: what it lists now is asked again at the next ask.
    for (auto& entry : g_infos) entry.second.when = 0;
  }
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

// How the process sees pixels: the window's coordinates (GetClientRect, SetWindowPos) are physical
// pixels only when it is DPI aware. SDL3 declares per-monitor awareness when it starts its video
// system (and the window's size at 150% scaling, 1920 pixels for 1280 logical, says it did); this asks
// instead of assuming, for the log line of a resize. 0 unaware, 1 system, 2 per monitor, -1 not known.
int DpiAwareness(HWND hwnd) {
  using GetContext = HANDLE(WINAPI*)(HWND);
  using GetAwareness = int(WINAPI*)(HANDLE);
  static const GetContext get_context = User32<GetContext>("GetWindowDpiAwarenessContext");
  static const GetAwareness get_awareness =
      User32<GetAwareness>("GetAwarenessFromDpiAwarenessContext");
  if (!get_context || !get_awareness) return -1;
  return get_awareness(get_context(hwnd));
}

const char* DpiAwarenessText(int awareness) {
  switch (awareness) {
    case 0:  return "DPI unaware: sizes are NOT physical pixels";
    case 1:  return "system DPI aware";
    case 2:  return "per-monitor DPI aware";
    default: return "DPI awareness not known";
  }
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
  // The list is asked of the driver again from now on, whether or not the display went back (a list
  // that is kept for good would be stale after any change the player makes in Windows).
  SetSwitchedDevice({});
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
  const DisplayInfo info = ListedModes(device, &monitor);
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
  SetSwitchedDevice(device);
  MC_INFO("[display] FULLSCREEN: the display is now {} at {} Hz (its own mode is {} at {} Hz)",
          SizeText(want), hz, SizeText(info.desktop), info.desktop_hz);
  FitFullscreenWindow(hwnd);
  return true;
}

// ── Changing the window's size (UI thread) ─────────────────────────────────────────────────────

// A size the window was asked and Windows would not give (a window larger than the display), and the
// size it kept instead: the same ask is not made again, and not logged again, while the window is still
// at that size. UI thread only.
Size g_refused_want;
Size g_refused_got;

// What ResizeWindow found.
enum class Resized {
  kAlready,  // the client area already is that size (or there is nothing to size: no window, minimised)
  kDone,     // the window was sized
  kNotYet,   // the window has no frame to size yet (SDL has not given it back since fullscreen)
};

// The client area of the window made `want` (PHYSICAL pixels: the units of GetClientRect, of
// SetWindowPos and of the swap chain), the whole window centred on the work area of its display. The
// frame is added at the window's own DPI (AdjustWindowRectExForDpi), so at 150% scaling a client area of
// 1600x900 is 1600x900 pixels plus the frame at that DPI, never 2400x1350. The result is read back and
// corrected once if Windows made the client area anything else (a frame that is not the one asked
// for), and a size it will not give is said in the log instead of being left silent.
// CHECK: SetWindowPos in the pixels SDL reports (the process is per-monitor DPI aware once SDL has
// started its video system; the SDK makes the first window with SizeToPhysical the same way). The first
// run (next3-window) shows it: 1280x720 asked at 150% scaling made a client area of exactly 1280x720
// ("the window 1280x720 (1x)" in the render scale line). The log line below says the DPI awareness and
// the scaling; "DPI unaware" there would mean every size in this file is virtualised.
Resized ResizeWindow(HWND hwnd, Size want) {
  if (!hwnd || want.empty() || IsIconic(hwnd)) return Resized::kAlready;
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (!(style & WS_CAPTION)) return Resized::kNotYet;
  if (style & WS_MAXIMIZE) {
    ShowWindow(hwnd, SW_RESTORE);
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  }
  RECT client = {};
  GetClientRect(hwnd, &client);
  if (uint32_t(client.right) == want.width && uint32_t(client.bottom) == want.height) {
    return Resized::kAlready;
  }
  const Size have = {uint32_t(client.right), uint32_t(client.bottom)};
  if (want == g_refused_want && have == g_refused_got) return Resized::kAlready;

  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  RECT outer = {0, 0, LONG(want.width), LONG(want.height)};
  AdjustForWindow(hwnd, &outer, DWORD(style), DWORD(ex_style));
  LONG outer_width = outer.right - outer.left;
  LONG outer_height = outer.bottom - outer.top;

  LONG x = 0;
  LONG y = 0;
  MONITORINFOEXW monitor;
  if (MonitorOf(hwnd, &monitor)) {
    const RECT& work = monitor.rcWork;
    x = std::max(work.left, work.left + ((work.right - work.left) - outer_width) / 2);
    y = std::max(work.top, work.top + ((work.bottom - work.top) - outer_height) / 2);
  }
  const UINT dpi = WindowDpi(hwnd);
  MC_INFO("[display] WINDOWED: the window's client area is now {}x{} physical pixels (was {}x{}; "
          "{}% scaling, {})",
          want.width, want.height, client.right, client.bottom, (dpi * 100 + 48) / 96,
          DpiAwarenessText(DpiAwareness(hwnd)));
  SetWindowPos(hwnd, nullptr, x, y, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);

  RECT now = {};
  GetClientRect(hwnd, &now);
  if (uint32_t(now.right) != want.width || uint32_t(now.bottom) != want.height) {
    outer_width += LONG(want.width) - now.right;
    outer_height += LONG(want.height) - now.bottom;
    SetWindowPos(hwnd, nullptr, x, y, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(hwnd, &now);
    if (uint32_t(now.right) != want.width || uint32_t(now.bottom) != want.height) {
      MC_WARN("[display] WINDOWED: asked for a client area of {}x{}, the window has {}x{} (a size "
              "the display or its frame does not allow)",
              want.width, want.height, now.right, now.bottom);
      g_refused_want = want;
      g_refused_got = {uint32_t(now.right), uint32_t(now.bottom)};
    }
  }
  return Resized::kDone;
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
  // The display's work area as it is in its own mode: while this game has the display switched to
  // another mode (the menu is on WINDOWED for a moment before the display is put back) the monitor
  // reports the smaller one, and the steps of a window would be cut to it.
  const Size own_work = ListedModes(monitor.szDevice, &monitor).work;
  const RECT& work = monitor.rcWork;
  const uint32_t work_width = own_work.empty() ? uint32_t(work.right - work.left) : own_work.width;
  const uint32_t work_height = own_work.empty() ? uint32_t(work.bottom - work.top) : own_work.height;
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

// How many times after the first the settings are looked at again while the window is still catching
// up (the passes stop at the first one that finds nothing to do).
constexpr int kLastPass = 3;

// UI thread: makes the window what the settings say. Idempotent; reads the settings when it runs, so a
// write that comes while one is queued is seen by it, and one that comes after it is queued again.
void ApplyNow(int pass) {
  rex::ui::Window* window = g_window.load();
  const HWND hwnd = NativeWindow();
  if (!window || !hwnd) return;
  bool changed = false;
  bool not_yet = false;
  switch (WantedMode()) {
    case Mode::kWindowed: {
      RestoreDesktopMode("the window is a window", false);
      if (window->IsFullscreen()) {
        window->SetFullscreen(false);
        changed = true;
      }
      // The size comes from `resolution` alone (see AskedWindowedSize); with none the window keeps
      // the size it has, which SDL also puts back when the window leaves fullscreen.
      Size asked;
      if (AskedWindowedSize(&asked)) {
        const Resized resized = ResizeWindow(hwnd, asked);
        changed |= resized == Resized::kDone;
        not_yet = resized == Resized::kNotYet;
      }
      break;
    }
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
  // leaves fullscreen, the size it takes for the display): the settings are looked at again, up to
  // kLastPass times, while something changed or the window had no frame to size yet.
  // CHECK: in the log, "[display] WINDOWED: the window's client area is now ..." comes from a later
  // look when SDL had not given the frame back by the first (leaving FULLSCREEN or BORDERLESS with a
  // `resolution` that is not the size SDL puts back), and a window that SDL moves or sizes after this
  // (WM_DPICHANGED on another display, the restore of its old rectangle) is brought back to the asked
  // size by the look that follows, not left at SDL's.
  if ((changed || not_yet) && pass < kLastPass) ScheduleApply(pass + 1);
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
  // The window the SDK made is the start of it: borderless for a saved `fullscreen` (FULLSCREEN is
  // ours to finish), and a window of `resolution` read as LOGICAL pixels, which at 150% scaling is 1.5
  // times as many physical ones as the RESOLUTION row says. One look now makes the window the physical
  // size the setting means, and does nothing where it already is (100% scaling, or no `resolution`).
  // CHECK: at a scaling other than 100% with a `resolution` saved (1600x900 at 150%), the window opens
  // at 2400x1350 and settles at 1600x900 in the first moments: the log has one "[display] WINDOWED: the
  // window's client area is now 1600x900 physical pixels (was 2400x1350; 150% scaling, ...)" and the
  // renderers one swap chain rebuild, before the game's first picture or just after it.
  ScheduleApply(0);
}

}  // namespace mcla::display
