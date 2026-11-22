#pragma once
// ===========================================================================
// Asking the player for a file without GTK, and the typed path it falls back to (Linux)
// ===========================================================================
// The ISO installer and the save importer ask the player for a file. On Windows that is the system's own dialog. On Linux it used to be a
// GTK 3 file chooser, and the game linked GTK 3 for it: the game did not start where libgtk-3 is missing (a minimal distribution, a container,
// an immutable system that does not ship it) and, as built, the two dialogs used GTK without ever calling gtk_init. It is now the SDK's file dialog
// (rex::ui::IUISystem::ShowFileDialog, which is SDL3's: it asks the xdg desktop portal over D-Bus and falls back to zenity), and the game links
// no GTK at all. Where neither the portal nor zenity answers (no portal in the session, a stripped container) the SDK's dialog returns an error,
// and the two wizards say so in one log line and show a box to type the file's path in, so a player is never left without a way to go on.
//
// This file is the part of that which needs no SDK, no window and no GTK, so that file_pick_test.cpp checks it alone:
//   * FromDialogAnswer   what the SDK's dialog gave (did it open, the paths, the error) as one of: a file, a cancel, or "no chooser";
//   * UnavailableText    the sentence a wizard shows over the path box;
//   * ParseTypedPath     what a typed or pasted path means: the whitespace and one pair of quotes around it, a file:// address (a drag from a file
//                        manager), a leading ~, the backslashes of a shell's escapes; then whether it names a file.
// The wizards (isoinstaller/install_wizard_dialog.cpp, saveporter/larecomp_save_porter.cpp) draw the box and call these.
// ===========================================================================

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::os {

// What asking for a file gave: a path, a cancel, or no chooser at all.
struct FilePick {
  std::filesystem::path path;  // the chosen file; empty when there is none
  bool cancelled = false;      // a chooser opened and the player closed it without choosing
  std::string unavailable;     // not empty: no chooser could be opened; the reason as the system gave it. The caller shows the path box.
};

// The SDK's file dialog answered: `opened` is whether it opened (false: it returned an error, `error` is its message), `chosen` what the player
// picked (empty: a cancel). More than one path (a dialog asked for one file gives one) takes the first.
FilePick FromDialogAnswer(bool opened, const std::vector<std::filesystem::path>& chosen, std::string_view error);

// The line over the path box: "No file chooser could be opened here (<reason>). Type the path of <what> below instead." `what` is plain words ("the
// Xbox 360 ISO"); a reason that is empty is left out.
std::string UnavailableText(std::string_view reason, std::string_view what);

// What a typed or pasted path comes to.
struct TypedPath {
  std::filesystem::path path;  // the file, when error is empty
  std::string error;           // not empty: what is wrong with the text, for the player ("There is no file at ...")
};

// The text of a path box as a path to a file that exists. Steps, in this order: the whitespace at both ends goes; one pair of matching quotes
// around the path goes; a file:// address (and file://localhost/) is the path in it with its %XX escapes decoded; on every system but Windows
// (where the backslash is the separator) a backslash makes the next character plain, as a shell's does for a space; `~` and `~/` stand for `home`
// (when it is not empty). The result must be a regular file: a folder, a missing file and an empty text each give an error that says which.
TypedPath ParseTypedPath(std::string_view typed, std::string_view home);

}  // namespace larecomp::os
