#pragma once
//
// The online status pop-up: top-right corner, ImGui, in the spirit of the
// achievement and controller toasts. Shows which server the game is on, its
// name and icon (when the relay has them), how many games are on it, and the
// ping. It comes up for a few seconds whenever the connection changes, and
// stays while the SYSTEM LINK menu is on screen.
//

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

// Creates the pop-up (the UI system owns it). UI thread, once, after the window is open.
void StartOnlineToast(rex::ui::IUISystem& ui);

// Keeps the pop-up up for a moment longer. Any thread.
void OnlineToastPoke();

// Linux (online_platform.h): shows the pop-up for a few seconds with its one sentence, "Online play is not available on Linux
// yet". Where online exists this is the same as OnlineToastPoke. Any thread.
void OnlineToastNotice();

}  // namespace larecomp
