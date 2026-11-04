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

// Merges into the file what the player changed while the game ran (cvars whose
// last writer was SetFlagByName), plus `held`. Everything else in the file stays
// as it is: other lines, comments, keys of cvars this build does not have. Values
// from the command line or the environment are not written. A changed value that
// is back at its default only rewrites a line the file already has. The file is
// replaced in one step (see profiles::EditConfig). False, with the file as it
// was, when it cannot be written.
bool SaveCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held = {});

// Only `held`, none of the live cvars.
bool SaveHeldCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held);

}  // namespace larecomp
