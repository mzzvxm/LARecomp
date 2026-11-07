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
//                         FULLSCREEN (exclusive: the display is switched to the size of `resolution`)
//   resolution            the SDK's text ("720p", "1080p", "1440p", "4k", "1920x1080" or empty): the
//                         window's size in WINDOWED, the display's mode in FULLSCREEN, not used in
//                         BORDERLESS (the display's own size). Empty is the app's default: 1280x720
//                         in a window, the display's size in FULLSCREEN. The SDK also reads it when
//                         it starts, for the first window and as the game's video mode, which is why
//                         it stays the one setting; window_width and window_height, when set, win
//                         over it for the window as they do there.
//
// What the SDK's window can and cannot do (LAR-RXSDK src/ui/window_sdl.cpp): it is an SDL window
// whose only live change is SDL_SetWindowFullscreen(on/off) (borderless: no display mode is ever
// given to SDL), it has no call to resize, and its SDL is not the one this executable carries (two
// copies of SDL run: the window's is inside rexruntime.dll), so nothing here can call SDL on it.
// Windows is therefore driven through the window's HWND (Window::GetNativeWindowHandle):
//   - WINDOWED <-> BORDERLESS: Window::SetFullscreen, live, as the old FULLSCREEN row did;
//   - the window's size in WINDOWED: SetWindowPos on the HWND, live (SDL follows the size from the
//     window messages and the renderers rebuild their swap chain as for any resize);
//   - FULLSCREEN: SetFullscreen(true) for the frameless window, plus ChangeDisplaySettingsEx with the
//     mode (CDS_FULLSCREEN: reverts by itself when the process ends), put back when the mode is left,
//     when the window loses focus, and when the game closes.
// Other platforms (Linux): WINDOWED and BORDERLESS only, live; the window's size is read at the next
// start (a restart), and there is no FULLSCREEN.
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
// window's size can change while the game runs (Windows). Elsewhere `resolution` is read at the
// next start.
bool ExclusiveSupported();
bool LiveResize();

// The size a `resolution` text stands for. False for empty text ("default") and for text that is no
// size.
bool ParseSize(std::string_view text, Size* out);
// "1920x1080": what is written into `resolution`; "1920X1080": what the menu shows.
std::string SizeText(Size size);
std::string SizeLabel(Size size);

// Any thread. Zero while the window is not there (or on a platform with no way to ask).
// The window's own size (the output the picture is made at), and the display's: the desktop mode
// the player set in Windows, which a FULLSCREEN mode this game switched to does not change.
Size OutputSize();
Size DesktopSize();

// The sizes the display lists (ascending, one per size whatever its refresh rates, from 1280x720 up:
// the game's own screen is 1280x720), and the sizes a window can have here: 16:9 steps from 1280x720
// up whose window (frame included) fits the display's work area. Both empty where they cannot be
// asked; the menu then offers its own steps.
std::vector<Size> ExclusiveSizes();
std::vector<Size> WindowedSizes();

// What the window is asked to be: in a window, `window_width`/`window_height` where set, else
// `resolution`, else 1280x720. In FULLSCREEN, the listed mode nearest to `resolution`, or the
// display's own for empty text.
Size WantedWindowedSize();
Size WantedExclusiveSize();

}  // namespace mcla::display
