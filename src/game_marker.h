#pragma once
//
// The "a game is running" marker: one named object that exists exactly as long as a LARecomp
// process of this program file runs, so that the launcher (and anything else that wants to know)
// can ask "is the game running?" with one call that costs about two microseconds, instead of
// listing every process on the PC every second or two.
//
//   Windows  a named mutex, Local\LARecomp.Game.<key>, created (not owned) at start and never
//            closed. It is gone when the last process holding it ends, however it ends. Asking:
//            OpenMutexW(SYNCHRONIZE, ...); found, or "access denied" (a game that runs elevated
//            has an object a normal process may not open) = running.
//   Linux    a lock file, $XDG_RUNTIME_DIR/larecomp/game-<key>.lock (else /tmp/larecomp-<uid>/),
//            with a SHARED flock held by the game for as long as it runs (the descriptor is close-on-
//            exec, so nothing the game starts keeps the lock alive). Asking: open the file and try an
//            exclusive non-blocking flock; it fails exactly while a game holds the shared one. Shared
//            on purpose: RESTART GAME starts the new process while the old one is still there, and
//            both hold it, so there is no moment in between when the launcher sees "not running".
//            (An abstract socket was the other candidate: a second bind of the same name fails, so a
//            restarted game could not mark itself while the old one was alive.)
//
// <key> is the program file's identity (game_marker_core.h: FNV-1a of its path), so that two
// installs, or a game built in a build folder next to the installed one, do not mark each other.
//
// Created first thing in LarecompApp's constructor (before the restart wait and before any logging),
// by every process of larecomp.exe including the ones the launcher starts to make shaders: the same
// processes the launcher's old process list counted. Nothing polls this; the launcher checks it
// when its window is activated and when the game it started ends.
//

#include <string>

namespace larecomp::marker {

// Creates the marker for this process. Idempotent, any thread, never throws, returns whether the
// marker is held. A failure is not fatal: the launcher then falls back to the game process it
// started itself, or to a process list when it has to be sure.
bool Create();

// One line for the log, after logging is up: the name or path, or why there is none.
std::string Describe();

}  // namespace larecomp::marker
