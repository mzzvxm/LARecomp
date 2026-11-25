#pragma once
//
// "Is the game running?", for the commands of larecomp_online_tool that change identity files. The game reads its key
// file once and rewrites the whole file on every change it makes (a recovery code, the Rate My Ride token), so a second
// writer while it runs can lose a change. The tool therefore refuses to write unless it knows the game is closed.
//
// It asks the way the launcher does (game_marker.h): the marker a running game holds (a named mutex on Windows, a
// lock file on Linux), and, for a game old enough to have no marker, a look at the process list for a process of the
// same program file. Both look at ONE install's program file (--game-exe), named by its path.
//

#include <string>

namespace larecomp::online_tool {

struct GameLook {
    bool running = false;  // a game of this program file runs
    bool unsure = false;   // something may run that could not be told apart (a process of that name that could not be
                           // opened): treated as running by the tool
    std::string how;       // "marker", "process" or "" (what showed it, for the documents)
};

// `exe` is the program file's path as the launcher starts it (UTF-8). It need not exist (a game that was moved or
// deleted while it runs is still found by its marker).
GameLook LookForGame(const std::string& exe);

}  // namespace larecomp::online_tool
