#pragma once
//
// The online status pop-up: top-right corner, ImGui, in the spirit of the
// achievement and controller toasts. Shows which server the game is on, its
// name and icon (when the relay has them), how many games are on it, and the
// ping. It comes up for a few seconds whenever the connection changes, and
// stays while the SYSTEM LINK menu is on screen.
//

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace larecomp {

// Creates the pop-up. UI thread, once, after the drawers exist.
void StartOnlineToast(rex::ui::ImGuiDrawer* imgui_drawer,
                      rex::ui::ImmediateDrawer* immediate_drawer);

// Keeps the pop-up up for a moment longer. Any thread.
void OnlineToastPoke();

}  // namespace larecomp
