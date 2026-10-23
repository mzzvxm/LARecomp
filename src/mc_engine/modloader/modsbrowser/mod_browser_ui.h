#pragma once
//
// The mod browser window (ImGui, F5 = bind_mod_browser): the mods in this
// game's models/ (switch one off or on, clear the old copies the server
// browser kept, update one a server has newer) and every mod the known
// servers offer (install one). Each shows its mod.toml: picture, title,
// version, author, description. Changes
// take a restart, which the window does for you. Input works like the server
// browser's: the game gets none while it is open.
//

struct ImGuiIO;

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace larecomp {

// UI thread, once (StartServerBrowser calls it).
void StartModBrowser(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::ImmediateDrawer* immediate_drawer);

// Opens the window on its next frame. Any thread.
void ModBrowserOpen();

// True while the window is open.
bool ModBrowserCapturesInput();

// The pad, read through XInput and handed to ImGui's navigation; *b_pressed
// is true on the frame B goes down. Shared with the server browser.
bool FeedBrowserGamepad(ImGuiIO& io, bool* b_pressed);

}  // namespace larecomp
