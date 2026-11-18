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

#include "app_paths.h"

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace larecomp {

// Decides whether the player has to choose, from what is on disk and in
// larecomp.toml. Call it from LarecompApp::OnInit, after the SDK has applied
// larecomp.toml, the environment and the command line, and before LaunchTitle
// (the SDK's own name prompt, which saves larecomp.toml in its own scoped
// format, would open there whenever kernel.user_name is empty): when a choice is
// due and user_name is empty, it is seeded with the newest save so that prompt
// never opens, and a first run with no save and no name is asked by the picker.
void PrepareProfileChoice(const std::filesystem::path& user_data_root);

// Shows the picker and returns at once. True means it is up and `on_finished`
// will run (on the UI thread, from the dialog's own draw) once the player has
// chosen; false means there was nothing to ask and `on_finished` is never
// called. Call it before anything that reads the profile name (the save check,
// the content manager).
//
// Asynchronous like the other first-launch dialogs: the app's setup loop
// (LarecompApp::RunSetupFrame) pumps the UI system and goes on from `on_finished`.
// `paths.config_file` is where the two lines of the choice are written (flat
// keys: user_name and profile_auto_select).
bool ShowProfilePicker(rex::ui::IUISystem& ui, const Paths& paths,
                       std::function<void()> on_finished);

}  // namespace larecomp
