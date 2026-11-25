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
//
// The app id (Linux). A Wayland desktop (GNOME for one, which has no protocol for a window's own icon) shows the icon of the menu entry that the window's
// app_id names (the entry's file name without ".desktop"); an X11 window manager does the same with WM_CLASS and the entry's StartupWMClass. SDL fills both
// from its app id (SDL_APP_ID; without one it uses a name of its own), which is no menu entry's name. The launcher's menu entry is
// larecomp.desktop with Icon=larecomp (launcher repository, Core\Os\Linux\LinuxAppId.cs and LinuxDesktop.cs), so kAppId is "larecomp": the launcher passes it in
// SDL_APP_ID when it starts the game, and this file sets the same in the environment at process start for a game that was started another way (a terminal,
// a Steam shortcut to the program), unless SDL_APP_ID already holds something (a player's own choice stays).
// At process start, not at the window's creation: SDL reads the app id when it makes the window, which the SDK does inside Init (its SDL starts before OnInit:
// wheel_input.cpp explains the same for SDL_GAMECONTROLLER_IGNORE_DEVICES), so a static initialiser of the executable is the early, sure place.

namespace rex::ui {
class IUISystem;
}

namespace mcla::display {

// The name of the launcher's menu entry without ".desktop", and of its icon.
inline constexpr const char* kAppId = "larecomp";

// UI thread, once the window is open (LarecompApp::StartOverlays). Logs one line if the icon could not be set; never fails the start.
void SetWindowIcon(rex::ui::IUISystem& ui);

}  // namespace mcla::display
