#include "profile_ref.h"

#include <cstring>
#include <filesystem>

#include "../identity/identity_core.h"
#include "../protocol/identity_protocol.h"

namespace larecomp::online_tool {

namespace fs = std::filesystem;
namespace core = identity::core;

namespace {

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

bool PlainGamertag(const std::string& s) {
    if (s.empty() || s.size() > identity::kNickMax) return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || std::strchr("<>:\"/\\|?*", c) != nullptr) return false;
    }
    return s != "." && s != "..";
}

bool ResolveProfileFiles(const ProfileRef& ref, ProfileFiles& p, ProfileProblem& problem) {
    p = ProfileFiles{};
    p.dir = ref.identity_dir.empty() ? core::DefaultIdentityDir() : PathFromUtf8(ref.identity_dir);
    p.gamertag = ref.gamertag;
    if (!p.gamertag.empty() && !PlainGamertag(p.gamertag)) {
        problem = {"bad_gamertag", "the gamertag is empty, longer than 15 bytes or holds characters a folder name cannot"};
        return false;
    }
    if (!ref.profile.empty()) {
        if (!core::ValidProfileId(ref.profile)) {
            problem = {"bad_profile", "a profile id is 16 hex digits, or name_ and the gamertag"};
            return false;
        }
        p.id = ref.profile;
        if (p.id.size() == 16)
            for (char& c : p.id) c = (c >= 'a' && c <= 'f') ? char(c - 'a' + 'A') : c;
        if (p.gamertag.empty() && p.id.rfind("name_", 0) == 0) p.gamertag = p.id.substr(5);
    } else {
        uint64_t xuid = 0;
        if (!PlainGamertag(p.gamertag)) {
            problem = {"bad_gamertag", "name the profile by a gamertag that is a plain folder name"};
            return false;
        }
        if (!core::ReadProfileXuid(PathFromUtf8(ref.saves), p.gamertag, xuid)) {
            problem = {"profile_not_found",
                       "no profile.xuid in the saves folder of that gamertag: the game has not made this profile yet"};
            return false;
        }
        p.id = core::ProfileId(xuid, p.gamertag);
    }
    p.file_id = p.id;
    // An SDK without per-profile ids filed the key by name; the game moves it at its next start.
    std::error_code ec;
    if (p.id.rfind("name_", 0) != 0 && !p.gamertag.empty() && !fs::exists(core::KeyPath(p.dir, p.id), ec) &&
        fs::exists(core::KeyPath(p.dir, "name_" + p.gamertag), ec))
        p.file_id = "name_" + p.gamertag;
    return true;
}

bool LoadRmrToken(const ProfileFiles& files, std::array<uint8_t, 16>& token, ProfileProblem& problem) {
    core::KeyRecord rec;
    if (core::LoadKey(files.dir, files.file_id, rec) != core::Load::kLoaded) {
        std::error_code ec;
        if (fs::exists(core::KeyPath(files.dir, files.file_id), ec))
            problem = {"key_unreadable",
                       "the key file cannot be read by this user on this PC, or is damaged: restore a backup of it"};
        else
            problem = {"no_key", "this profile has no key yet: start the game once with it"};
        return false;
    }
    if (!rec.has_rmr) {
        problem = {"no_token",
                   "this profile has no Rate My Ride token yet: the game makes it the first time it uses Rate My Ride or "
                   "the leaderboards"};
        return false;
    }
    token = rec.rmr;
    return true;
}

}  // namespace larecomp::online_tool
