// larecomp - Save importer / porter
//
// Imports an existing Midnight Club: LA save from another emulator into the
// larecomp content store, converting the RAGE container build tag as needed:
//   - Xbox 360 (Xenia)  : mc4.sav,  build tag 14 -> copied as-is
//   - PlayStation 3 (RPCS3): RAGE.SAV, build tag 15 -> patched to 14
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <filesystem>

#include <rex/rex_app.h>

namespace rex::ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace rex::ui

namespace larecomp {

// True if a usable mc4.sav already exists in the larecomp content store.
bool SaveAlreadyPresent(const std::filesystem::path& user_data_root);

// Shows the pre-runtime save-import wizard and pumps the UI until the user
// imports a save or skips. Skipping is not an error: the game then offers to
// create a new save on its own. Safe to call only when SaveAlreadyPresent()
// is false.
void RunSaveImportWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                 rex::ui::Window* window,
                                 rex::ui::ImGuiDrawer* drawer,
                                 const rex::PathConfig& paths);

}  // namespace larecomp
