#pragma once
//
// Saving the settings the player changed into larecomp.toml.
//
// The SDK's rex::cvar::SaveConfig is not used for this: it writes a string as
// "C:\Users\..", where \U starts an escape and the whole file fails to parse on
// the next launch; it truncates the file in place, so a crash mid-write loses it;
// it drops the lines of cvars this process never registered; and it saves every
// value that differs from its default, including the ones given on the command
// line for this run only.
//

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp {

// A value a menu keeps for the next launch instead of applying it now: the cvar
// name and its value text. Written whatever the cvar's live value is.
using HeldCvars = std::vector<std::pair<std::string, std::string>>;

// The TOML text that reads back as `value` for the cvar `name`: a quoted,
// escaped string for a string cvar, the bare number or bool for the others.
// A cvar this build does not register (an SDK without it) is told by its value.
// Nullopt when no valid TOML can hold it (a string that is not UTF-8, a number
// that is not one): saving it would make the whole file unreadable.
std::optional<std::string> CvarTomlValue(std::string_view name, std::string_view value);

// LARecomp's own startup code sets cvars through SetFlagByName too (the texture
// cache sizes and the other GPU defaults it works out for this PC), so a save
// would take them for the player's choices and freeze them in the file: a later
// version with better defaults, or another graphics card, would no longer change
// them. Call this right after such a set. The cvar's value at that moment is
// remembered as its startup value, and a save does not write the cvar while it
// still has that value. Not for what the player picked (the save picker's
// user_name): that stays a change.
void NoteStartupCvar(std::string_view name);

// True when `name` was given a value with NoteStartupCvar and has it still: LARecomp's own default for
// this PC, not something anybody chose.
bool IsAtStartupValue(std::string_view name);

// A cvar that belongs to this run only, such as a preview a menu row switches on while the row is
// highlighted: a save never writes its live value, whoever set it. A line the file already has for it
// is put back to the cvar's default (so an old save of the cvar is cleaned, not kept), and no line is
// ever added. Call it once at startup.
void NoteSessionOnlyCvar(std::string_view name);

// Merges into the file what the player changed while the game ran (cvars whose
// last writer was SetFlagByName), plus `held`. Everything else in the file stays
// as it is: other lines, comments, keys of cvars this build does not have. Values
// from the command line or the environment are not written. A changed value that
// is back at its default, or at its startup value (NoteStartupCvar), only rewrites
// a line the file already has. A cvar tagged .debug_only() is never written for a
// change made while the game runs (the DEBUG menu, the settings panel, the console),
// and a line the file already has for it is left exactly as it is, so a tester's
// hand-written value stays. `held` is the exception: a value a menu holds for the
// next launch is written whatever the cvar is. The file is replaced in one step (see
// profiles::EditConfig). False, with the file as it was, when it cannot be written.
bool SaveCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held = {});

// Only `held`, none of the live cvars.
bool SaveHeldCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held);

}  // namespace larecomp
