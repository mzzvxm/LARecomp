// MCLA GPU renderer -- native window handle. See native_window.h.
//
// Design followed: reblue platform/native_window.cpp (BSD-3-Clause).

#include "native_window.h"

#include <rex/logging.h>
#include <rex/ui/window.h>

#if !defined(_WIN32) && defined(PLUME_SDL_VULKAN_ENABLED)
#include <SDL3/SDL_video.h>
#endif

namespace mcla::gpu {

#if defined(_WIN32)

bool GetRenderWindow(rex::ui::Window* window, plume::RenderWindow& out) {
  out = window ? static_cast<plume::RenderWindow>(window->GetNativeWindowHandle()) : nullptr;
  if (!out) {
    REXLOG_ERROR("[gpu] the window has no native HWND yet");
    return false;
  }
  return true;
}

#elif defined(PLUME_SDL_VULKAN_ENABLED)

bool GetRenderWindow(rex::ui::Window*, plume::RenderWindow& out) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  out = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!out) {
    REXLOG_ERROR("[gpu] no SDL window exists for the Vulkan surface");
    return false;
  }
  return true;
}

#else

bool GetRenderWindow(rex::ui::Window*, plume::RenderWindow&) {
  REXLOG_ERROR("[gpu] native window handles are not wired for this platform");
  return false;
}

#endif

}  // namespace mcla::gpu
