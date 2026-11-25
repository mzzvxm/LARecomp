#pragma once
// ===========================================================================
// The AppImage variables the launcher leaves in the game's environment (Linux)
// ===========================================================================
// The launcher is shipped as an AppImage. The AppImage runtime sets four variables for the program inside it: APPIMAGE (the file the
// player started), APPDIR (the folder the image is mounted at), ARGV0 and OWD (the folder it was started from). Everything the launcher
// starts inherits them, the game included, and three things trust APPIMAGE:
//   * the SDK's IsPackagedApp() (rex/platform/file.h) says "a packaged app" when APPIMAGE is set, so GetAppRoot() is
//     ~/.config/larecomp instead of the game's own folder: larecomp.toml and logs/ would not be where the launcher, the README and
//     RUNNING.md say they are;
//   * the SDK's Relaunch() (a title that restarts itself) starts $APPIMAGE, which is the launcher, not the game;
//   * RESTART GAME (mc_engine/core/restart.cpp) did the same.
// The game is not inside the launcher's AppImage. It only is when the game itself is an AppImage, and then its own file lies inside its
// own $APPDIR. So the variables count only when the program's own file is inside $APPDIR; otherwise they are the launcher's, and the
// game removes them from its own process at the very start (DropInheritedAppImageVariables), before the SDK reads any of them.
// Windows and the other systems: nothing sets these variables there, and every function does nothing or says "no".
// ===========================================================================

#include <string>
#include <string_view>

namespace larecomp::os {

// The rule on its own, with both inputs given so that a check can try every case: whether `program` (this process's own file, as
// /proc/self/exe resolves it) is a file inside the mount folder `mount` (the value of $APPDIR). A mount that is empty, relative or the
// root folder never counts: everything would be "inside" it.
bool ProgramIsInsideMount(std::string_view mount, std::string_view program);

// True when this program itself runs from an AppImage: $APPIMAGE and $APPDIR are set and its own file is inside $APPDIR.
bool RunsFromAppImage();

// The AppImage file to start this program again from, or "" when RunsFromAppImage() is false. A mounted AppImage's own path
// (/proc/self/exe) is gone once the program has closed; the AppImage file is not.
std::string OwnAppImageFile();

// Removes APPIMAGE, APPDIR, ARGV0 and OWD from this process's environment unless RunsFromAppImage(). Call it once, first thing in the
// program, before any other thread exists (setenv/unsetenv are not safe beside a getenv on another thread). Returns the names it
// removed, comma separated, "" when there was nothing to remove; the log is not open yet at that point, so the caller says it later.
std::string DropInheritedAppImageVariables();

}  // namespace larecomp::os
