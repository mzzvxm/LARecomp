// MCLA GPU renderer -- native window handle. See native_window.h.
//
// Design followed: reblue platform/native_window.cpp (BSD-3-Clause).

#include "native_window.h"

#include <rex/logging/log_category.h>
#include <rex/platform/native_window.h>

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

#if defined(_WIN32)

bool GetRenderWindow(const rex::NativeWindow& window, plume::RenderWindow& out) {
  out = window.type == rex::NativeWindow::Type::kWin32 ? static_cast<plume::RenderWindow>(window.window) : nullptr;
  if (!out) {
    REXLOG_ERROR("[gpu] the window has no native HWND yet");
    return false;
  }
  return true;
}

void GetWindowPixelSize(const rex::NativeWindow& window, uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
  RECT rect{};
  if (window.type == rex::NativeWindow::Type::kWin32 && window.window &&
      GetClientRect(static_cast<HWND>(window.window), &rect)) {
    width = rect.right > rect.left ? uint32_t(rect.right - rect.left) : 0;
    height = rect.bottom > rect.top ? uint32_t(rect.bottom - rect.top) : 0;
  }
}

#elif defined(PLUME_SDL_VULKAN_ENABLED)

bool GetRenderWindow(const rex::NativeWindow& window, plume::RenderWindow& out) {
  out = static_cast<SDL_Window*>(window.sdl);
  if (!out) {
    REXLOG_ERROR("[gpu] the window has no SDL window for the Vulkan surface");
    return false;
  }
  return true;
}

void GetWindowPixelSize(const rex::NativeWindow& window, uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
  int w = 0;
  int h = 0;
  if (window.sdl && SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window.sdl), &w, &h)) {
    width = w > 0 ? uint32_t(w) : 0;
    height = h > 0 ? uint32_t(h) : 0;
  }
}

#else

bool GetRenderWindow(const rex::NativeWindow&, plume::RenderWindow&) {
  REXLOG_ERROR("[gpu] native window handles are not wired for this platform");
  return false;
}

void GetWindowPixelSize(const rex::NativeWindow&, uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
}

#endif

}  // namespace mcla::gpu
