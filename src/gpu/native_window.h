#pragma once
// ===========================================================================
// MCLA GPU renderer -- the window's native handle, in Plume's terms
// ===========================================================================
// Windows: the HWND behind rex::ui::Window (GetNativeWindowHandle).
// Linux:   the SDK's one SDL3 window, handed to Plume whole
//          (PLUME_SDL_VULKAN_ENABLED) so the surface follows whichever video
//          driver SDL picked -- the SDK exposes no SDL_Window accessor and
//          GetNativeWindowHandle is null off Windows.
// macOS:   not wired. reblue builds an NSWindow + CAMetalLayer pair for
//          MoltenVK (platform/native_window.cpp); that lands with the Apple
//          build (M7).
// ===========================================================================

#include <plume_render_interface.h>

namespace rex::ui {
class Window;
}

namespace mcla::gpu {

// False (and logged) when the window has no native handle yet or the platform
// is not wired.
bool GetRenderWindow(rex::ui::Window* window, plume::RenderWindow& out);

}  // namespace mcla::gpu
