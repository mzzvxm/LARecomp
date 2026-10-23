#pragma once
//
// A server's mods (browser_protocol.h, ModManifest): which ones this game is
// missing or has in another version, downloading them from the relay's HTTP
// service into models/, and restarting the game so the mod loader picks them
// up. Everything here blocks; call it from worker threads.
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace larecomp::browser {

enum class ModState { kInstalled, kMissing, kOutdated };

struct ModFile {
    std::string path;  // relative, '/'-separated
    uint64_t bytes = 0;
    uint64_t hash = 0;  // FNV-1a 64
    bool have = false;  // the local copy matches
};

// What a mod says about itself: mod.toml at the root of its folder (the mod
// loader only reads meshes there, so it never sees it):
//   title = "Nissan Silvia S15"
//   description = "..."
//   author = "..."
//   version = "1.2.0"
//   image = "preview.png"   (PNG or JPG in the same folder, 1 MB at most)
struct ModInfo {
    std::string title;
    std::string description;
    std::string author;
    std::string version;
    std::string image;  // file name inside the mod
};

ModInfo ParseModInfo(const std::string& text);
// models/<folder>/mod.toml (and its image), empty when absent.
ModInfo ReadLocalModInfo(const std::string& folder);
std::vector<uint8_t> ReadLocalModImage(const std::string& folder, const ModInfo& info);
// Dotted numbers compared piece by piece ("1.10" > "1.9"); <0 when a is older.
// Anything that is not a number compares as text.
int CompareVersions(const std::string& a, const std::string& b);

struct ServerMod {
    std::string name;
    bool required = false;
    uint64_t bytes = 0;       // all its files
    uint64_t need_bytes = 0;  // what is not here yet
    ModState state = ModState::kMissing;
    std::vector<ModFile> files;
    ModInfo info;                // the server's mod.toml, when asked for
    std::vector<uint8_t> image;  // its image, when asked for
};

struct ModCheck {
    bool ok = false;     // the manifest was read
    std::string error;   // why not
    std::vector<ServerMod> mods;
    bool RequiredReady() const;  // every required mod is installed as the server has it
    bool AllReady() const;
};

// Reads the server's manifest and compares it with models/ here. Hashes only
// the local files whose size already matches.
// with_info also reads each mod's mod.toml; with_images also its image.
ModCheck CheckServerMods(const std::string& host, uint16_t http_port, bool with_info = false,
                         bool with_images = false);

// A download of some of a check's mods. Each mod goes to models/.download_<mod>
// first; only when all its files arrived and matched their hash does the old
// folder move aside (models/.old_<mod>_<time>, which the mod loader skips) and
// the new one take its name.
class ModDownload {
public:
    struct Progress {
        uint64_t done = 0;
        uint64_t total = 0;
        std::string current;  // "mod/path"
        bool finished = false;
        bool failed = false;
        std::string error;
    };

    ModDownload(std::string host, uint16_t http_port, std::vector<ServerMod> mods);
    ~ModDownload();
    void Cancel();
    Progress Get() const;

    struct State;  // the download thread's, defined in mod_download.cpp

private:
    std::shared_ptr<State> state_;
};

// Starts the game again with the same command line once this process has
// exited, then asks this one to close. False if the relaunch could not be set up.
bool RestartGame();

std::string FormatBytes(uint64_t bytes);

}  // namespace larecomp::browser
