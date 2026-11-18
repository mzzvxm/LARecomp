#pragma once

#include <filesystem>
#include <functional>

#include "app_paths.h"

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

bool IsGameInstalled(const std::filesystem::path& game_root);
std::filesystem::path RememberedGameInstall(const std::filesystem::path& user_data_root);

// Shows the ISO install wizard and returns at once; `complete` runs on the UI
// thread, from the dialog's draw, after a successful install, with the folder
// the player installed to in game_root: `complete` makes it the title's game
// root (title.game_root, kUntilLaunch: writable until LaunchTitle). Nothing pumps the UI here: the app's setup loop (LarecompApp::RunSetupFrame)
// keeps it drawn until `complete` has run.
void ShowRexglueIsoInstallWizard(rex::ui::IUISystem& ui, const Paths& paths,
                                 std::function<void(Paths)> complete);

// Unattended install from the LARECOMP_INSTALL_ISO environment variable, for
// scripted setups. `attempted` says whether the variable was set at all; the
// return value whether the install then succeeded.
bool TryAutomatedIsoInstall(const Paths& paths, bool& attempted);

}  // namespace larecomp
