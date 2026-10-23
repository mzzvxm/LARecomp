#pragma once
//
// The online identity window (ImGui, F8 = bind_identity): who the player is
// online (gamertag, XUID, key), whether the server admits that nick, the
// recovery code to write down, a transfer to cancel, a nick to recover, and
// the password-protected backup. It opens by itself when one of those needs
// the player. Input works like the server browser's: the game gets none
// while it is open.
//

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace larecomp {

// UI thread, once, after the drawers exist (FinishSetupAfterBootBuild). Also
// starts the identity worker.
void StartIdentity(rex::ui::ImGuiDrawer* imgui_drawer);

// True while the window is open.
bool IdentityCapturesInput();

}  // namespace larecomp
