// Folder lookups of the modloader that ignore the case of a folder's name.
//
// A mod is made on Windows, where "Mods", "mods" and "MODS" are one folder, and is then copied to the PC it is played on. On Linux (the Steam Deck,
// SteamOS, a desktop) the file system tells the spellings apart, so a mod made with <mod>/Rims/ or a game folder with a "Mods" folder was not found:
// the wheels, the raw game files, the vehicles, the texture replacements and the music of such a mod were left out with no message at all. The
// names the modloader looks for are lower case; these helpers find the folder that is spelled any other way. Standard library only, so that
// mod_paths_test.cpp builds on its own.
//
// What they do: the exact spelling wins (the cheap case, and the only one that happens on Windows or macOS); else the entries of the folder are
// compared with the wanted name ignoring ASCII case, and where two entries differ only in case (possible on Linux only) the first in name order
// is taken, so the answer does not change from one run to the next. Nothing is created, renamed or written. A name nothing matches comes back as
// the exact path the caller asked for, which does not exist: the callers test the result with is_directory exactly as they did before.
#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mc::modloader {

// ASCII case folding only: the names the modloader looks for are ASCII, and a locale-dependent fold would make two PCs disagree.
inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

// Which of `names` (the entries of one folder) is `want`, or -1. The exact spelling first; else the first match ignoring case in the order the
// names sort (byte order), whatever order the file system listed them in.
inline int PickNoCase(const std::vector<std::string>& names, std::string_view want) {
    int found = -1;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == want) return int(i);
        if (!EqualsNoCase(names[i], want)) continue;
        if (found < 0 || names[i] < names[size_t(found)]) found = int(i);
    }
    return found;
}

// dir / `name` as the folder spells it. `want_directory`: only a directory counts as a match (a FILE called "Rims" is not the rims folder).
inline std::filesystem::path FindChildNoCase(const std::filesystem::path& dir, std::string_view name,
                                             bool want_directory) {
    const std::filesystem::path exact = dir / std::filesystem::path(std::string(name));
    std::error_code ec;
    if (want_directory ? std::filesystem::is_directory(exact, ec) : std::filesystem::exists(exact, ec)) return exact;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code kind_ec;
        if (want_directory && !entry.is_directory(kind_ec)) continue;
        names.push_back(entry.path().filename().string());
    }
    const int pick = PickNoCase(names, name);
    return pick < 0 ? exact : dir / std::filesystem::path(names[size_t(pick)]);
}

// The folder called `name` (ignoring case) inside `dir`.
inline std::filesystem::path FindDirNoCase(const std::filesystem::path& dir, std::string_view name) {
    return FindChildNoCase(dir, name, /*want_directory=*/true);
}

}  // namespace mc::modloader
