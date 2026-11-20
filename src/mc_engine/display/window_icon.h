#pragma once
//
// The game window's icon.
//
// Windows: nothing to do here. The SDK's SDL registers the window class with the executable's first icon resource (larecomp.rc,
// `1 ICON "larecomp.ico"`), which gives the window, the taskbar button and Alt-Tab the same icon as Explorer; SetWindowIcon does nothing.
//
// Linux (every platform that is not Windows): the SDK sets no window icon, so this gives the window's SDL_Window the app icon
// (SDL_SetWindowIcon, from PNGs embedded in window_icon_data.h). X11 takes the first image (_NET_WM_ICON); a Wayland compositor chooses among
// the alternates.

namespace rex::ui {
class IUISystem;
}

namespace mcla::display {

// UI thread, once the window is open (LarecompApp::StartOverlays). Logs one line if the icon could not be set; never fails the start.
void SetWindowIcon(rex::ui::IUISystem& ui);

}  // namespace mcla::display
