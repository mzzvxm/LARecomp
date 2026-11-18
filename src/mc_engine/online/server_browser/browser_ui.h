#pragma once
//
// The server browser window (ImGui): every relay the master lists plus the
// player's own, with icon, name, description, players and ping; each relay's
// rooms with what they are playing; join one. F6 (ui.bind.server_browser) or the
// SERVER BROWSER row in SYSTEM LINK / XBOX LIVE opens it. While it is open the
// game gets no input; keyboard, mouse and a pad (XInput) drive the window.
//

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

// UI thread, once, after the window is open (LarecompApp::RunBootBuild): adds the window (and the mod browser) to
// the UI system, which owns them.
void StartServerBrowser(rex::ui::IUISystem& ui);

// The key (ui.bind.server_browser, F6): LarecompApp::StartOverlays binds it to this through a hotkey overlay.
void ServerBrowserKeyPressed();

// Opens the window on its next frame. Any thread.
void ServerBrowserOpen();

// True while the window is open: the game must not read the keyboard or pad.
bool ServerBrowserCapturesInput();

}  // namespace larecomp
