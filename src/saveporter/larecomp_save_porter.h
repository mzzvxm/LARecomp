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
#include <functional>

#include <rex/app/title_app.h>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace larecomp {

// True if a usable mc4.sav already exists in the larecomp content store.
bool SaveAlreadyPresent(const std::filesystem::path& user_data_root);

// Shows the pre-runtime save-import wizard and returns at once. True means the
// wizard is up and `on_finished` will run (on the UI thread, from the dialog's
// own draw) when the player imports a save or skips; false means there was
// nothing to offer -- skip_save_import, no emulator save found, no UI -- and
// `on_finished` is never called. Skipping is not an error: the game then
// creates a new save on its own. Safe to call only when SaveAlreadyPresent()
// is false.
//
// Asynchronous on purpose. The SDK's window backend is SDL, and a nested
// message loop under it never drains SDL's event queue; the stock SDK does not
// even offer a way to pump it. OnFinalizePaths returns std::nullopt while the
// wizard is up and resumes from `on_finished`.
bool ShowSaveImportWizard(rex::ui::ImGuiDrawer* drawer, const rex::PathConfig& paths,
                          std::function<void()> on_finished);

}  // namespace larecomp
