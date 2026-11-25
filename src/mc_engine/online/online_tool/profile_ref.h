#pragma once
//
// larecomp_online_tool: which saved profile a command works on, and where its key file is. Shared by the identity
// commands (identity_commands.cpp) and by the rmr, lb and tournament commands (online_tool_remote.cpp, `--profile`:
// the player's Rate My Ride token comes from the same key file the game and the identity commands use, so the
// launcher never has to hold the token or pass it on stdin).
//
// Naming a profile (README-identity.md, "Naming the profile"):
//   --profile <id>                    16 hex digits, or name_<gamertag> for a profile that still has the legacy XUID
//   --saves <folder> --gamertag <n>   the saves folder and the gamertag; the id is read from <saves>/<n>/profile.xuid
//   --identity-dir <folder>           the folder of the key files (default: the game's, core::DefaultIdentityDir())
//

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace larecomp::online_tool {

struct ProfileRef {
    std::string profile;       // --profile
    std::string saves;         // --saves
    std::string gamertag;      // --gamertag
    std::string identity_dir;  // --identity-dir

    // Names a profile by itself: an id, or a saves folder with a gamertag.
    bool Names() const { return !profile.empty() || (!saves.empty() && !gamertag.empty()); }
    // Any of the four was given (which is a command line mistake unless Names()).
    bool Any() const { return !profile.empty() || !saves.empty() || !gamertag.empty() || !identity_dir.empty(); }
};

struct ProfileFiles {
    std::string id;       // the profile's id as the game names it
    std::string file_id;  // the id its key file is named by: differs only for a key still filed by name
    std::string gamertag;
    std::filesystem::path dir;  // the identity folder
};

struct ProfileProblem {
    // "bad_profile" | "bad_gamertag" (the command line named something that cannot be a profile), "profile_not_found",
    // "no_key", "key_unreadable", "no_token" (the profile's files say otherwise).
    std::string code;
    std::string message;  // one plain sentence
    bool CommandLine() const { return code == "bad_profile" || code == "bad_gamertag"; }
};

// A gamertag the tool will use as a folder name: 1 to 15 bytes, no control characters, none of < > : " / \ | ? *.
bool PlainGamertag(const std::string& s);

// Works out the profile's id and where its key file is. False (and `problem`) when the command line names something that
// cannot be a profile or the profile.xuid it points at is not there. Touches no key.
bool ResolveProfileFiles(const ProfileRef& ref, ProfileFiles& out, ProfileProblem& problem);

// The profile's Rate My Ride token (read-only: it works while the game runs). False (and `problem`) when the profile has no
// key file, the key cannot be read by this user, or it holds no token yet (the game makes it the first time it uses Rate
// My Ride or the leaderboards). The token is a secret: the caller never prints it.
bool LoadRmrToken(const ProfileFiles& files, std::array<uint8_t, 16>& token, ProfileProblem& problem);

}  // namespace larecomp::online_tool
