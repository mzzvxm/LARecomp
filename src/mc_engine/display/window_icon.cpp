// The game window's icon. See window_icon.h.

#include "window_icon.h"

#if !defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <rex/platform/native_window.h>
#include <rex/ui/ui_system.h>

#include "mc_engine/core/logging.h"
#include "mc_engine/modloader/texture.h"  // DecodeImageRGBA: the stb decoder this program carries (the SDK's is private since 0.11)
#include "window_icon_data.h"

namespace mcla::display {

namespace {

// One decoded PNG and the SDL surface that looks at its pixels (SDL_CreateSurfaceFrom copies nothing, so the pixels live here).
struct IconImage {
  std::vector<uint8_t> rgba;
  int width = 0;
  int height = 0;
  SDL_Surface* surface = nullptr;
  IconImage() = default;
  IconImage(const IconImage&) = delete;
  IconImage& operator=(const IconImage&) = delete;
  ~IconImage() {
    if (surface) SDL_DestroySurface(surface);
  }

  bool Load(const unsigned char* png, size_t size) {
    rgba = mc::modloader::DecodeImageRGBA(png, size, width, height);
    if (rgba.empty() || width <= 0 || height <= 0) return false;
    surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, rgba.data(), width * 4);
    return surface != nullptr;
  }
};

}  // namespace

void SetWindowIcon(rex::ui::IUISystem& ui) {
  auto native = ui.GetNativeWindow();
  SDL_Window* sdl = native ? static_cast<SDL_Window*>(native->sdl) : nullptr;
  if (!sdl) {
    MC_INFO("[display] window icon: the window is not an SDL window here, so it keeps the default icon");
    return;
  }

  IconImage base;     // 64 px: the one X11 uses
  IconImage icon24;   // 24 px, for a title bar
  IconImage icon128;  // 128 px, for an Alt-Tab list
  if (!base.Load(icon_data::kPng64, sizeof(icon_data::kPng64))) {
    MC_WARN("[display] window icon: the embedded 64 px PNG could not be read");
    return;
  }
  // The alternates are for the compositors that take several sizes (Wayland); SDL ignores them where it takes one image.
  if (icon24.Load(icon_data::kPng24, sizeof(icon_data::kPng24))) SDL_AddSurfaceAlternateImage(base.surface, icon24.surface);
  if (icon128.Load(icon_data::kPng128, sizeof(icon_data::kPng128))) SDL_AddSurfaceAlternateImage(base.surface, icon128.surface);

  // SDL keeps its own copy of the pixels, and holds a reference to each alternate: the surfaces here can go as soon as the call returns.
  if (!SDL_SetWindowIcon(sdl, base.surface)) {
    MC_WARN("[display] window icon: SDL_SetWindowIcon failed: {}", SDL_GetError());
  }
}

}  // namespace mcla::display

#else  // Windows: the icon is a resource of the executable (larecomp.rc); the window class takes it.

namespace mcla::display {

void SetWindowIcon(rex::ui::IUISystem&) {}

}  // namespace mcla::display

#endif
