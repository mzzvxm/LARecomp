#pragma once

#include <filesystem>
#include <functional>

#include <rex/app/title_app.h>
#include "app_paths.h"

namespace larecomp {

bool IsGameInstalled(const std::filesystem::path& game_root);
std::filesystem::path RememberedGameInstall(const std::filesystem::path& user_data_root);

// Shows the ISO install wizard and returns at once; `complete` runs on the UI
// thread, from the dialog's draw, after a successful install. Nothing pumps the
// UI here: OnFinalizePaths returns std::nullopt while the wizard is up and the
// app's own loop keeps it drawn (rex::ReXApp's asynchronous path hook).
void ShowRexglueIsoInstallWizard(rex::ui::ImGuiDrawer* drawer, larecomp::Paths runtime_paths,
                                 std::function<void(larecomp::Paths)> complete);

// Unattended install from the LARECOMP_INSTALL_ISO environment variable, for
// scripted setups. `attempted` says whether the variable was set at all; the
// return value whether the install then succeeded.
bool TryAutomatedIsoInstall(const larecomp::Paths& runtime_paths, bool& attempted);

}  // namespace larecomp
