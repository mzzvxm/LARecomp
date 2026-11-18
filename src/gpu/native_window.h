#pragma once
// ===========================================================================
// MCLA GPU renderer -- the window's native handle, in Plume's terms
// ===========================================================================
// rexglue 0.11 hands out the window it opened as a rex::NativeWindow (IUISystem::GetNativeWindow):
// Windows: the HWND (window).
// Linux:   the SDK's one SDL3 window (sdl), handed to Plume whole (PLUME_SDL_VULKAN_ENABLED) so the
//          surface follows whichever video driver SDL picked. NativeWindow::sdl is the SDL_Window of an
//          SDL-backed window on every platform; it is what SDL_GetWindows() gave before.
// macOS:   not wired. reblue builds an NSWindow + CAMetalLayer pair for MoltenVK
//          (platform/native_window.cpp); NativeWindow::layer carries the CAMetalLayer
//          now, so that lands with the Apple build (M7).
// ===========================================================================

#include <cstdint>

#include <plume_render_interface.h>

namespace rex {
struct NativeWindow;
}

namespace mcla::gpu {

// False (and logged) when the window has no native handle yet or the platform
// is not wired.
bool GetRenderWindow(const rex::NativeWindow& window, plume::RenderWindow& out);

// The window's client area in pixels, 0 x 0 when it has none (minimised) or cannot be asked.
void GetWindowPixelSize(const rex::NativeWindow& window, uint32_t& width, uint32_t& height);

}  // namespace mcla::gpu
