#pragma once

// RESTART GAME (the pause menu's QUIT form): start larecomp.exe again, then close this one.
namespace mcla {

// Starts larecomp.exe again with this process's command line, plus an inheritable handle to this
// process for the new one to wait on (WaitForRestartedGame). Only that handle is inherited. The caller
// saves the settings and waits for saves first, and closes the game after. False if it could not
// start the new one (logged); then the caller should keep playing. On Linux the new game gets this
// process's id on its command line instead of a handle, and an AppImage is started from its own
// file (its mount goes away when this game closes).
bool RelaunchGame();

// Called first thing at start: when this process was started by RelaunchGame, waits for the old one
// to be gone (up to 30 s) before the settings, the saves or the window are touched; if it is still
// there after that, says so and quits rather than run beside it (two games would both write the
// career save). Does nothing on a normal start.
void WaitForRestartedGame();

}  // namespace mcla
