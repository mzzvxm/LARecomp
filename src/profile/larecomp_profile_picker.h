// larecomp - Save picker
//
// Which save to play, asked before the runtime exists. The SDK fork files the
// saves under <user data>/<user_name>/..., and its own first-launch prompt is a
// single text box: with several saves on disk, typing a name that happens to
// match an old one silently plays that one, and typing a new one renames the
// last-used save to it. The picker lists the saves that are there, can start a
// new one without touching the others, and can remember the choice.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace larecomp {

// Decides whether the player has to choose, from what is on disk and in
// larecomp.toml. Call it from SetupEnvironment(), right after the base has read
// the config and before the base's own name prompt can look at user_name: when
// a choice is due and user_name is empty, it is seeded with the newest save so
// that prompt never opens.
void PrepareProfileChoice(const std::filesystem::path& user_data_root);

// Shows the picker and returns at once. True means it is up and `on_finished`
// will run (on the UI thread, from the dialog's own draw) once the player has
// chosen; false means there was nothing to ask and `on_finished` is never
// called. Call it from OnFinalizePaths() before anything that reads the profile
// name (the save check, the content manager).
//
// Asynchronous like the other first-launch dialogs: OnFinalizePaths returns
// std::nullopt while it is up and resumes from `on_finished`.
bool ShowProfilePicker(rex::ui::ImGuiDrawer* drawer, const rex::PathConfig& paths,
                       std::function<void()> on_finished);

}  // namespace larecomp
