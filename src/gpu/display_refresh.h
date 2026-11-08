#pragma once
// ===========================================================================
// MCLA GPU renderer -- the refresh rate of the display a window is on
// ===========================================================================
// Frame generation sizes its default frame rate cap from it (HALF DISPLAY).
//
// Windows: the current mode of the monitor the window is on (a variable refresh display reports its
//          highest rate).
// Linux:   the SDK's SDL3 window is the one Plume was given (native_window.h), so SDL's own answer for
//          the display it is on.
// Elsewhere, and whenever it cannot be told: 0.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(PLUME_SDL_VULKAN_ENABLED)
#include <SDL3/SDL_video.h>
#endif

namespace mcla::gpu {

inline uint32_t ReadDisplayRefreshHz(plume::RenderWindow window) {
#if defined(_WIN32)
  const HWND hwnd = window;
  const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    return 0;
  }
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    return 0;
  }
  // 0 and 1 mean the hardware's default.
  return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0u;
#elif defined(PLUME_SDL_VULKAN_ENABLED)
  // CHECK: not run on Linux. SDL3 reports the refresh rate of the display's current mode as a
  // float (59.94 for a 60 Hz mode that is 60000/1001); it is rounded to whole hertz. Under
  // Wayland a compositor may report 0, which counts as "cannot be told".
  if (!window) {
    return 0;
  }
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
  const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
  if (!mode || !(mode->refresh_rate > 1.0f)) {
    return 0;
  }
  return uint32_t(mode->refresh_rate + 0.5f);
#else
  (void)window;
  return 0;
#endif
}

}  // namespace mcla::gpu
