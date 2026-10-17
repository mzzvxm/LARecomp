// larecomp - Save profiles on disk
//
// The pre-runtime view of the saves under the user data root: which gamertags
// have a save, the marker the SDK's content manager keeps, and the two lines of
// larecomp.toml the save picker is allowed to touch. Standard library only, so
// it can be exercised without the SDK.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::profiles {

namespace fs = std::filesystem;

// One save the player can pick: a directory under the user data root named after
// the gamertag, holding <title id>/00000001/<package>/<file>.
struct Profile {
  std::string name;                  // directory name == gamertag, UTF-8
  fs::file_time_type last_played{};  // newest file under the saved-game packages
  bool has_save = false;             // false: named by user_name, nothing written yet
};

// The SDK fork's UserProfile::SanitizeName: what a typed name becomes before it
// names a directory. A copy for builds whose SDK does not have it.
std::string SanitizeName(std::string_view raw);

// Directory names of the user data root that are not saves: the shared cache,
// achievements and Rate My Ride folders, the hex XUID / title id directories,
// anything hidden. A gamertag with one of these names would write into them.
bool IsReservedName(std::string_view name);

// Windows folders ignore case; ASCII is enough for the names that can collide.
bool SameName(std::string_view a, std::string_view b);

using Sanitizer = std::string (*)(std::string_view);

// Every save under `root`, newest first. A directory counts when it holds a
// saved-game package and its name survives `sanitize` unchanged: user_name goes
// through the sanitizer before it names the directory, so any other name could
// never be reached by choosing it.
std::vector<Profile> Enumerate(const fs::path& root, Sanitizer sanitize);

// <root>/.profile: the directory name the SDK's content manager last filed the
// profile under. When user_name differs from it and the directory it names
// exists, the content manager RENAMES that directory to the new name.
std::string ReadMarker(const fs::path& root);
bool WriteMarker(const fs::path& root, std::string_view name);

// A TOML basic string, escaped the way the SDK's own serializer does.
std::string QuoteTomlString(std::string_view value);

// One edit of the flat `key = value` larecomp.toml. `value` is the raw TOML text
// (already quoted for strings); nullopt removes the key.
struct ConfigEdit {
  std::string key;
  std::optional<std::string> value;
};

// Rewrites just these keys in place and leaves every other byte of the file
// alone, creating the file when it is missing. The SDK's SaveConfig is no
// substitute: it serializes every cvar that differs from its default, whatever
// set it, and drops the lines of cvars this process never registered.
bool EditConfig(const fs::path& config, const std::vector<ConfigEdit>& edits);

// UTF-8 <-> path, independent of the ANSI code page.
fs::path PathFromUtf8(std::string_view utf8);
std::string Utf8FromPath(const fs::path& path);

}  // namespace larecomp::profiles
