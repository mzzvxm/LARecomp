#pragma once
//
// The gate of the hidden DEBUG menu.
//
// The debug menu is for testers and for bug reports. A player never sees it unless it was
// unlocked on purpose, in one of three ways:
//   --debug_menu                 on the command line,
//   REX_DEBUG_MENU=1             in the environment (the SDK maps REX_<NAME> to a cvar),
//   debug_menu = true            in larecomp.toml (written by hand: the game never writes it).
// The command line wins over the environment and the environment over the file, so
// --debug_menu=false locks it again for one run.
//
// Everything that is hidden asks DebugMenuUnlocked() and nothing else; nothing else knows how
// the unlock happened. The answer is fixed at start-up (InitDebugMenu), so a running game cannot
// unlock itself, whoever writes the cvar later.
//
// Settings tagged .debug_only() are never written to larecomp.toml by a change made while the
// game runs (config_save.cpp), and the ones that differ from their default are listed in the
// log at every start, with where each value came from.
//

#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>

namespace larecomp {

// True when the debug menu is unlocked. Cheap and thread-safe. Before InitDebugMenu has run it
// reads the cvar as it is then; afterwards it is the answer fixed at start-up.
bool DebugMenuUnlocked();

// Where the unlock came from, as the log prints it: "command line", "environment" or
// "larecomp.toml". Empty while the menu is locked.
std::string_view DebugMenuUnlockSource();

// Sets is_debug_only in the registry for the SDK cvars that stand behind DEBUG rows (the SDK's
// presenter effect and the graphics API). The SDK itself is not changed; a tag the SDK does not
// know about is skipped. Call it once the SDK cvars are registered; InitDebugMenu does.
void MarkSdkDebugCvars();

// Once at start-up, after the command line, the environment and larecomp.toml have been read:
// marks the SDK debug cvars, fixes the answer of DebugMenuUnlocked(), and writes the start-up
// log lines: whether the menu is unlocked and by what, then the debug-only cvars that are not at
// their default with where each value came from, so a bug report shows what was changed.
// A second call does nothing.
void InitDebugMenu();

// One debug-only cvar that is not at its default.
struct DebugCvarChange {
  std::string name;
  std::string value;                // as the cvar's own getter prints it
  rex::cvar::Source source;         // who wrote it last
};

// Every debug-only cvar whose value differs from its default, by name. Left out: debug_menu itself,
// and a value LARecomp's own start-up code gave (NoteStartupCvar) that nobody has changed since.
std::vector<DebugCvarChange> NonDefaultDebugCvars();

// "command line", "environment", "larecomp.toml", "this session" (SetFlagByName: a menu, the
// settings panel, the console) or "default".
const char* CvarSourceName(rex::cvar::Source source);

namespace testing {
// For the unit test only: forgets the fixed answer so a test can start again.
void ResetDebugMenuForTesting();
}  // namespace testing

}  // namespace larecomp
