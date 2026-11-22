// The game window's icon. See window_icon.h.

#include "window_icon.h"

#if !defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

// SDL_APP_ID in the environment before the SDK's SDL is first called (see window_icon.h, "The app id"): SDL then names the Wayland window's app_id and the X11 window's
// class "larecomp", which is the launcher's menu entry, whose icon the desktop shows. A value that is already there stays (the launcher gives the same one; a player may
// have another). An environment variable and not SDL_SetHint: the hint cannot reach an SDL that is not this executable's own, and the variable is read by both.
constexpr const char* kAppIdVariable = "SDL_APP_ID";

struct AppIdAtProcessStart {
  AppIdAtProcessStart() {
    const char* now = std::getenv(kAppIdVariable);
    if (!now || !*now) {
      setenv(kAppIdVariable, kAppId, 1);
    }
  }
};
const AppIdAtProcessStart g_app_id_at_process_start;

}  // namespace

void SetWindowIcon(rex::ui::IUISystem& ui) {
  // One line saying what SDL was given (the window is open: the app id went into its app_id / class when it was made).
  const char* app_id = std::getenv(kAppIdVariable);
  MC_INFO("[display] window app id: {} (the desktop shows the icon of {}.desktop for it)", app_id && *app_id ? app_id : "(none)", kAppId);

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
