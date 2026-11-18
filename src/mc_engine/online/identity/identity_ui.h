#pragma once
//
// The online identity window (ImGui, F8 = ui.bind.identity): who the player is
// online (gamertag, XUID, key), whether the server admits that nick, the
// recovery code to write down, a transfer to cancel, a nick to recover, and
// the password-protected backup. It opens by itself when one of those needs
// the player. Input works like the server browser's: the game gets none
// while it is open.
//

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

// UI thread, once, after the window is open (LarecompApp::RunBootBuild). Adds the window to the UI system, which
// owns it, and starts the identity worker.
void StartIdentity(rex::ui::IUISystem& ui);

// The key (ui.bind.identity, F8): LarecompApp::StartOverlays binds it to this through a hotkey overlay.
void IdentityKeyPressed();

// True while the window is open.
bool IdentityCapturesInput();

}  // namespace larecomp
