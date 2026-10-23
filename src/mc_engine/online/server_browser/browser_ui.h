#pragma once
//
// The server browser window (ImGui): every relay the master lists plus the
// player's own, with icon, name, description, players and ping; each relay's
// rooms with what they are playing; join one. F6 (bind_server_browser) or the
// SERVER BROWSER row in SYSTEM LINK / XBOX LIVE opens it. While it is open the
// game gets no input; keyboard, mouse and a pad (XInput) drive the window.
//

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui
namespace rex::system {
class IInputSystem;
}

namespace larecomp {

// UI thread, once, after the drawers exist (FinishSetupAfterBootBuild).
void StartServerBrowser(rex::ui::ImGuiDrawer* imgui_drawer,
                        rex::ui::ImmediateDrawer* immediate_drawer,
                        rex::system::IInputSystem* input_system);

// Opens the window on its next frame. Any thread.
void ServerBrowserOpen();

// True while the window is open: the game must not read the keyboard or pad.
bool ServerBrowserCapturesInput();

}  // namespace larecomp
