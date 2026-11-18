#pragma once
//
// The mod browser window (ImGui, F5 = ui.bind.mod_browser): the mods in this
// game's models/ (switch one off or on, clear the old copies the server
// browser kept, update one a server has newer) and every mod the known
// servers offer (install one). Each shows its mod.toml: picture, title,
// version, author, description. Changes
// take a restart, which the window does for you. Input works like the server
// browser's: the game gets none while it is open.
//

struct ImGuiIO;

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

// UI thread, once (StartServerBrowser calls it): adds the window to the UI system, which owns it.
void StartModBrowser(rex::ui::IUISystem& ui);

// The key (ui.bind.mod_browser, F5): LarecompApp::StartOverlays binds it to this through a hotkey overlay.
void ModBrowserKeyPressed();

// Opens the window on its next frame. Any thread.
void ModBrowserOpen();

// True while the window is open.
bool ModBrowserCapturesInput();

// The pads (ui/gamepad.h: XInput for Xbox pads, SDL for the others), handed to
// ImGui's navigation; *b_pressed is true on the frame B goes down. Shared with
// the server browser.
bool FeedBrowserGamepad(ImGuiIO& io, bool* b_pressed);

// The window that fed the pads closed: let them go (a window that is still
// open reads them again on its next frame).
void ReleaseBrowserGamepad();

}  // namespace larecomp
