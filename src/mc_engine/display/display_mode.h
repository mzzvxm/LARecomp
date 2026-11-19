#pragma once
//
// The game window's display mode: windowed, borderless fullscreen or exclusive fullscreen, and the
// size each one has. The DISPLAY page's DISPLAY MODE and RESOLUTION rows (display_menu.h) are the
// way to change it while the game runs, and what both renderers follow: the window is the SDK's one
// SDL window, and its size is the output the picture is made at and the HUD is drawn in.
//
// The settings (the old ones keep their meaning, so a larecomp.toml from before still works):
//
//   fullscreen            the SDK's: off is a window, on is fullscreen
//   fullscreen_exclusive  new, with `fullscreen` on: off is BORDERLESS (the SDK's fullscreen: a window
//                         without a frame the size of the display, the display left as it is), on is
//                         FULLSCREEN (exclusive: the display is switched to `fullscreen_resolution`)
//   resolution            the SDK's text ("720p", "1080p", "1440p", "4k", "1920x1080" or empty): the
//                         window's size in WINDOWED; not used in BORDERLESS (the display's own size)
//                         nor in FULLSCREEN. Empty: a window keeps the size it has.
//   fullscreen_resolution new: the display mode FULLSCREEN switches to, the same text. Empty, the default:
//                         the display's own size, so no switch. It is not `resolution` because that is the
//                         window's size: a player who had only ever used a 1280x720 window (the default
//                         size) found the display switched to 1280x720 on entering FULLSCREEN, and the
//                         list of sizes was no way back to the display's own (Mz, 2026-11-16).
//
// Units. Every size here, in the RESOLUTION row, in `resolution` and in the log is in PHYSICAL pixels:
// the window's client area as the swap chain and the picture see it (what GetClientRect says), the
// pixels of a display mode. The window's frame is on top of it, at the window's DPI. The SDK reads the
// same `resolution` text, and window_width / window_height, as LOGICAL pixels when it makes the first
// window (the window is then that times the display's scaling: 1280x720 at 150% is 1920x1080 pixels),
// and the first look at start-up (Initialize) puts the window right, so a window and the row never
// disagree for long. Once the game runs, `resolution` is the only setting of the window's size:
// window_width and window_height are start-up values (kRequiresRestart) and are not read here. A
// window with no `resolution` keeps the size it has, and the row shows that size.
//
// What the SDK's window can and cannot do (LAR-RXSDK src/ui/window_sdl.cpp): it is an SDL window
// whose only live change is SDL_SetWindowFullscreen(on/off) (borderless: no display mode is ever
// given to SDL), and it has no call to resize.
//
// Windows: its SDL is not the one this executable carries (two copies of SDL run: the window's is
// inside rexruntime.dll), so nothing here can call SDL on it. Windows is therefore driven through the
// window's HWND (Window::GetNativeWindowHandle):
//   - WINDOWED <-> BORDERLESS: Window::SetFullscreen, live, as the old FULLSCREEN row did;
//   - the window's size in WINDOWED: SetWindowPos on the HWND, live (SDL follows the size from the
//     window messages and the renderers rebuild their swap chain as for any resize);
//   - FULLSCREEN: SetFullscreen(true) for the frameless window, plus ChangeDisplaySettingsEx with the
//     mode (CDS_FULLSCREEN: reverts by itself when the process ends), put back when the mode is left,
//     when the window loses focus, and when the game closes.
//
// Linux (every platform that is not Windows): there is no HWND, but the renderer already reaches the
// SDK's window through SDL itself (gpu/native_window.cpp hands Plume SDL_GetWindows()'s window for the
// Vulkan surface), so this executable and the SDK share one SDL there and the window's size is changed
// the same way, with SDL_SetWindowSize: no SDK method is needed. Initialize finds the window by its
// title and says so in the log; when it is not found (a second copy of SDL after all) the size is read
// at the next start and the row says (RESTART), as before.
//   - WINDOWED <-> BORDERLESS: Window::SetFullscreen, live;
//   - the window's size in WINDOWED: SDL_SetWindowSize once SDL has given the window back from
//     fullscreen or maximised (SDL ignores a resize before that), looked at again every 100 ms until
//     the window has the size. The size is physical pixels; SDL's window coordinates are those divided
//     by the window's pixel density (Wayland and macOS scaling). The window is then centred on the
//     display's usable area (X11; a Wayland client cannot place its window);
//   - the sizes offered are the 16:9 steps that fit the display's usable area with the window's frame,
//     as on Windows (SDL's own answers; the SDK presets when SDL gives none), and BORDERLESS shows the
//     display's own size;
//   - there is no FULLSCREEN: SDL switches a display's mode through XRandR on X11 only (Wayland scales
//     instead) and the mode would have to be put back after a crash;
//   - under gamescope (the Steam Deck's Game Mode, or run by hand) the compositor sets the window's
//     size and puts back any other: WindowSizeFixed() is true, the RESOLUTION row is grey and shows
//     the window's size, and nothing is asked of SDL. A compositor that is not recognised but keeps a
//     window at another size than asked is told in the log once and the window is left where it is.
//
// Every change runs on the UI thread, queued from the setting's change callback (which runs with the
// settings registry locked and must not touch the window).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui {
class Window;
}

namespace mcla::display {

struct Size {
  uint32_t width = 0;
  uint32_t height = 0;
  bool operator==(const Size&) const = default;
  bool empty() const { return width == 0 || height == 0; }
};

enum class Mode : uint8_t { kWindowed, kBorderless, kExclusive };

// UI thread, once the window is open (ReXApp::SetupPresentation): takes over the `fullscreen`
// setting's change callback from the SDK's, follows `fullscreen_exclusive` and `resolution` too, and
// applies a FULLSCREEN choice that is already saved (the SDK opened the window borderless).
void Initialize(rex::ui::Window* window);

// UI thread: the focus of the window changed. FULLSCREEN gives the display its own mode back while
// another window has the focus, and takes its mode again when this one has it back.
void OnFocusChanged(bool focused);

// UI thread: the display's own mode is put back (the game is closing). Safe to call when nothing
// was changed.
void Shutdown();

// What the settings ask for now.
Mode WantedMode();

// Whether FULLSCREEN can be offered here (Windows: a display mode can be changed), and whether the
// window's size can change while the game runs (Windows; elsewhere once Initialize has found the
// SDK's SDL window, and the compositor does not set the size: WindowSizeFixed). When it cannot,
// `resolution` is read at the next start. Any thread.
bool ExclusiveSupported();
bool LiveResize();
// The compositor decides the size of the game's window (gamescope): the RESOLUTION row is grey and
// shows the size the window has. Always false on Windows. Any thread.
bool WindowSizeFixed();

// The size a `resolution` text stands for. False for empty text ("default") and for text that is no
// size.
bool ParseSize(std::string_view text, Size* out);
// "1920x1080": what is written into `resolution`; "1920X1080": what the menu shows.
std::string SizeText(Size size);
std::string SizeLabel(Size size);

// Any thread. Zero while the window is not there (or on a platform with no way to ask).
// The window's own size (the output the picture is made at), and the display's: the desktop mode
// the player set in Windows, which a FULLSCREEN mode this game switched to does not change. Off
// Windows the display's size and the usable area are SDL's, read on the UI thread when the window
// opened and again at most once a second while a menu asks (so up to a second old).
Size OutputSize();
Size DesktopSize();

// The sizes the display lists (ascending, one per size whatever its refresh rates, from 1280x720 up:
// the game's own screen is 1280x720), and the sizes a window can have here: 16:9 steps from 1280x720
// up whose window (frame included) fits the display's work area. Both empty where they cannot be
// asked; the menu then offers its own steps. Both are the display's OWN. Windows lists by the mode the
// display is in (asked while a 3840x2160 display is in 1280x720, the list loses the sizes above it, the
// display's own among them), so the display's list is a book (display_sizes.h) that every look at the
// driver is added to and none replaces, the driver is not asked while this game has the display
// switched, and the display's own mode (the registry settings), the mode it is in and the monitor's own
// size (the display configuration API) are in it whether the driver listed them or not. The window's
// steps count the work area the display has in its own mode. Windows only: no FULLSCREEN elsewhere
// (display_mode.cpp; the Linux side sizes the window through SDL and has no list of modes).
std::vector<Size> ExclusiveSizes();
std::vector<Size> WindowedSizes();

// What the window is asked to be, in physical pixels: in a window, `resolution`; with none, the size the
// window has now (what the row shows). In FULLSCREEN, the listed mode nearest to `fullscreen_resolution`,
// or the display's own for empty text (never the smallest mode: empty when the display's own mode is not
// known either, which switches nothing).
Size WantedWindowedSize();
Size WantedExclusiveSize();

}  // namespace mcla::display
