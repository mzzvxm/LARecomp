#include "mc_engine/music/custom_music.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "../../larecomp_log.h"
#include "mc_engine/modloader/rpf3.h"
#include "mc_engine/music/audio_dat.h"
#include "mc_engine/music/wave_bank.h"

// stb_image carries a raw-deflate decoder and its implementation is already
// compiled into modloader/texture.cpp; the archive stores plain files deflated.
#include "mc_engine/modloader/stb_image.h"

namespace fs = std::filesystem;

namespace mc::music {
namespace {

constexpr const char* kCacheFolder = ".custommusic";
constexpr const char* kSourceArchive = "xarchive_cache.rpf";
constexpr const char* kTemplate = "ROCK_CSS_RATISDEAD";
constexpr const char* kManager = "MUSIC_0_MANAGER";

const char* const kGenres[] = {"ECLECTIC", "ELECTRONIC", "HARDROCK", "HIPHOP",
                               "ROCK",     "TECHNO",     "WEST_RAP"};
constexpr int kGenreCount = 7;

// MUSIC_0_MANAGER lists its songs in seven groups; each group holds exactly one
// genre, in this order (measured, and every shipped song agrees).
constexpr int kGroupOfGenre[kGenreCount] = {6, 2, 0, 4, 1, 3, 5};

std::vector<std::pair<std::string, std::string>> g_titles;
std::vector<std::string> g_claimed;

// ------------------------------------------------------------------- helpers

std::string Upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(std::string_view text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return std::string(text.substr(a, b - a));
}

// Anything a person types -> something RAGE can carry as a name.
std::string Ident(std::string_view text) {
    std::string out;
    bool pending = false;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            if (pending && !out.empty()) out.push_back('_');
            pending = false;
            out.push_back(static_cast<char>(std::toupper(c)));
        } else {
            pending = true;
        }
    }
    return out.empty() ? "TRACK" : out;
}

std::string Camel(std::string_view text) {
    std::string out;
    bool head = true;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(head ? std::toupper(c) : c));
            head = false;
        } else {
            head = true;
        }
    }
    return out;
}

// `Music_Rock_MyBand_MySong` -> `MyBand - MySong`, the shipped convention.
std::string DefaultDisplay(const std::string& title) {
    std::vector<std::string> parts;
    std::stringstream stream(title);
    std::string part;
    while (std::getline(stream, part, '_')) parts.push_back(part);
    if (!parts.empty() && parts.front() == "Music") parts.erase(parts.begin());
    if (parts.size() > 1) parts.erase(parts.begin());  // drop the genre
    if (parts.empty()) return title;
    std::string out = parts.front();
    for (size_t i = 1; i < parts.size(); ++i) out += " - " + parts[i];
    return out;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return output.good();
}

// --------------------------------------------------------------- the shipped pair

bool LoadShippedDat(const fs::path& archive_path, AudioDat& game, AudioDat& sounds) {
    modloader::Rpf3Reader archive;
    if (!archive.Open(archive_path)) {
        LARECOMP_APP_ERROR("[music] cannot open {}", archive_path.string());
        return false;
    }

    AudioDat* targets[2] = {&game, &sounds};
    const char* names[2] = {"game.dat", "sounds.dat"};
    for (int i = 0; i < 2; ++i) {
        modloader::Rpf3Entry entry;
        const std::string path = std::string("audio/x360/config/") + names[i];
        if (!archive.Find(path, entry)) {
            LARECOMP_APP_ERROR("[music] {} has no {}", archive_path.string(), path);
            return false;
        }
        std::vector<uint8_t> raw;
        if (!archive.ReadFile(entry, raw)) {
            LARECOMP_APP_ERROR("[music] cannot read {}", path);
            return false;
        }
        // A plain file's flag is bit30 (compressed) plus its uncompressed size.
        if (entry.flag & 0x40000000u) {
            int out_length = 0;
            char* inflated = stbi_zlib_decode_noheader_malloc(
                reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()),
                &out_length);
            if (!inflated || out_length <= 0) {
                LARECOMP_APP_ERROR("[music] cannot inflate {}", path);
                return false;
            }
            raw.assign(inflated, inflated + out_length);
            std::free(inflated);
        }
        std::string error;
        if (!targets[i]->Parse(raw, error)) {
            LARECOMP_APP_ERROR("[music] {}: {}", path, error);
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ the records

// A game.dat type 0x23 record. +1 is filled in by AudioDat::AddObject.
std::vector<uint8_t> MusicObject(int genre, uint32_t cue_hash, const std::string& title) {
    std::vector<uint8_t> rec(20 + title.size(), 0);
    rec[0] = 0x23;
    rec[10] = static_cast<uint8_t>(genre);
    for (int i = 0; i < 4; ++i) rec[11 + i] = static_cast<uint8_t>(cue_hash >> (24 - 8 * i));
    const uint32_t constant = 0xE38FCF16u;
    for (int i = 0; i < 4; ++i) rec[15 + i] = static_cast<uint8_t>(constant >> (24 - 8 * i));
    rec[19] = static_cast<uint8_t>(title.size());
    std::memcpy(rec.data() + 20, title.data(), title.size());
    return rec;
}

// Appends a song to the manager's group for `genre`.
std::vector<uint8_t> ManagerWith(const std::vector<uint8_t>& record, int genre,
                                 uint32_t song_hash) {
    const int group = kGroupOfGenre[genre];
    std::vector<uint8_t> out(record.begin(), record.begin() + 42);
    size_t p = 42;
    for (int g = 0; p < record.size(); ++g) {
        const uint8_t n = record[p];
        const size_t body = 4ull * n;
        if (p + 1 + body > record.size()) break;
        out.push_back(static_cast<uint8_t>(g == group ? n + 1 : n));
        out.insert(out.end(), record.begin() + p + 1, record.begin() + p + 1 + body);
        if (g == group) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(song_hash >> (24 - 8 * i)));
        }
        p += 1 + body;
    }
    return out;
}

void StoreBE32At(std::vector<uint8_t>& rec, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) rec[at + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}

struct Song {
    fs::path audio;
    std::string name;     // radio entry, e.g. ROCK_SOMEBAND_ASONG
    std::string title;    // string-table key, e.g. Music_Rock_SomeBand_ASong
    std::string bank;     // someband_asong
    std::string display;  // what the radio shows
    int genre = 0;
};

// The two wave leaves, the cue that pairs them, the radio entry, and the
// manager that lists it. Records are cloned from a shipped song so every field
// nobody has decoded yet keeps its shipped value.
bool AddSong(AudioDat& game, AudioDat& sounds, const Song& song) {
    const std::string template_bank = std::string(kTemplate).substr(std::strlen("ROCK_"));
    const std::string template_cue = std::string("SND_") + kTemplate;
    const std::string template_leaf[2] = {
        "MUSIC_" + template_bank + "_" + template_bank + "_LEFT",
        "MUSIC_" + template_bank + "_" + template_bank + "_RIGHT",
    };

    const std::string bank_upper = Upper(song.bank);
    const std::string leaf[2] = {
        "MUSIC_" + bank_upper + "_" + bank_upper + "_LEFT",
        "MUSIC_" + bank_upper + "_" + bank_upper + "_RIGHT",
    };
    const char* const sides[2] = {"left", "right"};

    for (int i = 0; i < 2; ++i) {
        std::vector<uint8_t> rec = sounds.Record(template_leaf[i]);
        if (rec.size() < 8) return false;
        StoreBE32At(rec, rec.size() - 8, modloader::RageHash("music/" + song.bank));
        StoreBE32At(rec, rec.size() - 4,
                    modloader::RageHash(song.bank + "_" + sides[i]));
        const uint32_t off = sounds.AddObject(leaf[i], rec);
        sounds.AddFixupB(off + static_cast<uint32_t>(rec.size()) - 8);
    }

    std::vector<uint8_t> cue = sounds.Record(template_cue);
    if (cue.size() < 16) return false;
    StoreBE32At(cue, cue.size() - 16, modloader::RageHash(leaf[0]));
    StoreBE32At(cue, cue.size() - 8, modloader::RageHash(leaf[1]));
    const std::string cue_name = "SND_" + song.name;
    const uint32_t cue_off = sounds.AddObject(cue_name, cue);
    sounds.AddFixupA(cue_off + static_cast<uint32_t>(cue.size()) - 16);
    sounds.AddFixupA(cue_off + static_cast<uint32_t>(cue.size()) - 8);

    // The bank path is resolved through the string table, not by hash alone.
    sounds.AddString("MUSIC\\" + bank_upper);

    game.AddObject(song.name,
                   MusicObject(song.genre, modloader::RageHash(cue_name), song.title));
    game.ReplaceObject(kManager, ManagerWith(game.Record(kManager), song.genre,
                                             modloader::RageHash(song.name)));
    return true;
}

// --------------------------------------------------------------- the manifest

struct Override {
    std::string genre, name, title, bank, display;
};

// key = value, with a [file name] section per file, the same shape as
// music_titles.txt -- larecomp has no JSON parser and this is read at boot.
// Dropped into a music folder that has no manifest yet, so the format explains
// itself where people will actually look for it. Every line is a comment, which
// means the file changes nothing until someone edits it.
void WriteManifestTemplate(const fs::path& folder) {
    std::error_code ec;
    if (fs::exists(folder / "manifest.txt", ec) || fs::exists(folder / "music.txt", ec)) return;

    std::ofstream out(folder / "manifest.txt");
    if (!out) return;
    out <<
        R"(# Custom radio music for Midnight Club: Los Angeles.
#
# Drop .mp3 files next to this file and they are on the radio the next time the
# game starts. Nothing else to do -- each one becomes a real game object, a real
# cue and a real streaming wave bank, the same way the shipped songs are built.
#
#
# NAMING
#
# The file name is read as "Artist - Title.mp3", which is the convention the
# shipped songs follow:
#
#     Some Band - A Song.mp3   ->  radio shows "Some Band - A Song"
#     A Song.mp3               ->  radio shows "A Song"
#
# Everything below is optional. It only exists to override what the file name
# already says.
#
#
# DEFAULTS
#
# Written before the first [section], these apply to every file in this folder.
# Without one, tracks land in ECLECTIC.
#
#     genre = ROCK
#
# The genres are the game's own seven:
#
#     ECLECTIC   ELECTRONIC   HARDROCK   HIPHOP   ROCK   TECHNO   WEST_RAP
#
#
# PER FILE
#
# A section named exactly like the file overrides the defaults for that one
# track:
#
#     [Some Band - A Song.mp3]
#     genre   = HIPHOP
#     display = Some Band - A Song
#     name    = HIPHOP_SOMEBAND_ASONG
#     bank    = someband_asong
#     title   = Music_HipHop_SomeBand_ASong
#
# What each one is:
#
#     genre     which of the seven lists the track shows up in
#     display   what the radio shows -- the only one most people want
#     name      the radio entry's internal name, rarely worth setting
#     bank      the audio file name inside the archive
#     title     the string-table key behind `display`
#
# Use `display` when the file name is ugly but you do not want to rename the
# file. Everything except `display` is a single word: anything after the first
# space is ignored, so a stray explanation left on the line does no harm.
#
#
# WHAT IS SUPPORTED
#
# * MPEG-1 Layer III (plain .mp3) only. The frames are copied into the bank
#   untouched and decoded at runtime, so there is no re-encode and no quality
#   loss -- but a format the runtime cannot decode cannot be carried this way.
#   Anything else in this folder is ignored here and picked up by the old host
#   player instead, which plays it over the game rather than through the radio.
#
# * Track length is capped by the game's wave slot, not by us: roughly 5 minutes
#   at 128 kbps, or 2.5 minutes at 320 kbps. A track that does not fit is
#   skipped with a line in debug_la.txt saying how much of it would have fit.
#
# * Two tracks cannot share a bank name. If two files reduce to the same one,
#   the second is skipped -- give it its own `bank` above.
#
#
# HOUSEKEEPING
#
# Built banks are cached in models/.custommusic and only rebuilt when a source
# file changes, so a normal start costs nothing. Delete that folder to force a
# full rebuild. Nothing here ever modifies the game's own files.
)";
}

std::map<std::string, Override> ReadManifest(const fs::path& folder, Override& defaults) {
    std::map<std::string, Override> per_file;
    std::ifstream input;
    for (const char* name : {"manifest.txt", "music.txt"}) {
        input.open(folder / name);
        if (input) break;
        input.clear();
    }
    if (!input) return per_file;

    Override* current = &defaults;
    std::string line;
    while (std::getline(input, line)) {
        const std::string text = Trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        if (text.front() == '[' && text.back() == ']') {
            current = &per_file[Lower(Trim(text.substr(1, text.size() - 2)))];
            continue;
        }
        const size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Lower(Trim(text.substr(0, equals)));

        // Trailing prose has to be tolerated: people copy an example line out of
        // the comments above and keep the explanation that follows it. Every
        // field except `display` is a single token, so cutting at the first
        // space recovers the value instead of storing the sentence.
        std::string value = Trim(text.substr(equals + 1));
        const size_t inline_comment = value.find_first_of("#;");
        if (inline_comment != std::string::npos) value = Trim(value.substr(0, inline_comment));
        if (key != "display") {
            const size_t space = value.find_first_of(" \t");
            if (space != std::string::npos) value = value.substr(0, space);
        }

        if (key == "genre") current->genre = value;
        else if (key == "name") current->name = value;
        else if (key == "title") current->title = value;
        else if (key == "bank") current->bank = value;
        else if (key == "display") current->display = value;
    }
    return per_file;
}

int GenreFromName(const std::string& text, int fallback) {
    const std::string want = Upper(Trim(text));
    if (want.empty()) return fallback;
    for (int i = 0; i < kGenreCount; ++i) {
        if (want == kGenres[i]) return i;
    }
    return fallback;
}

// Fills in everything a song needs from its file name, read as
// "Artist - Title" -- the convention the shipped names follow.
Song SongFromFile(const fs::path& path, const Override& defaults, const Override& own) {
    const std::string stem = path.stem().string();
    std::string artist, title = stem;
    const size_t dash = stem.find(" - ");
    if (dash != std::string::npos) {
        artist = Trim(stem.substr(0, dash));
        title = Trim(stem.substr(dash + 3));
    }

    Song song;
    song.audio = path;
    song.genre = GenreFromName(own.genre.empty() ? defaults.genre : own.genre, 0);

    const std::string genre_name = kGenres[song.genre];
    const std::string tail =
        artist.empty() ? Ident(stem) : Ident(artist) + "_" + Ident(title);
    song.name = genre_name + "_" + tail;
    song.title = "Music_" + Camel(genre_name) +
                 (artist.empty() ? "_" + Camel(stem)
                                 : "_" + Camel(artist) + "_" + Camel(title));
    song.bank = Lower(tail);
    song.display = stem;

    if (!own.name.empty()) song.name = own.name;
    if (!own.title.empty()) song.title = own.title;
    if (!own.bank.empty()) song.bank = Lower(own.bank);
    if (!own.display.empty()) song.display = own.display;
    return song;
}

std::vector<fs::path> MusicFolders(const fs::path& exe_dir) {
    std::vector<fs::path> folders{exe_dir / "music"};
#if defined(_WIN32)
    if (const char* home = std::getenv("USERPROFILE")) {
        folders.push_back(fs::path(home) / "Documents" / "Rockstar Games" / "LARecomp" /
                          "User Music");
    }
#else
    if (const char* home = std::getenv("HOME")) {
        folders.push_back(fs::path(home) / "Documents" / "Rockstar Games" / "LARecomp" /
                          "User Music");
    }
#endif
    return folders;
}

std::vector<Song> ScanSongs(const fs::path& exe_dir) {
    std::vector<Song> songs;
    std::vector<std::string> taken;
    std::error_code ec;

    for (const fs::path& folder : MusicFolders(exe_dir)) {
        if (!fs::is_directory(folder, ec)) continue;

        WriteManifestTemplate(folder);
        Override defaults;
        const std::map<std::string, Override> per_file = ReadManifest(folder, defaults);

        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (Lower(entry.path().extension().string()) == ".mp3") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());

        for (const fs::path& file : files) {
            const auto found = per_file.find(Lower(file.filename().string()));
            const Override own = found == per_file.end() ? Override{} : found->second;
            Song song = SongFromFile(file, defaults, own);
            if (std::find(taken.begin(), taken.end(), song.bank) != taken.end()) {
                LARECOMP_APP_ERROR("[music] {}: another track already claims the bank name {}",
                                   file.filename().string(), song.bank);
                continue;
            }
            taken.push_back(song.bank);
            songs.push_back(std::move(song));
        }
    }
    return songs;
}

// ------------------------------------------------------------------- the cache

// One line per bank: <bank> <size> <write time>. A boot that changes nothing
// rebuilds nothing, which matters because a bank is several megabytes.
std::map<std::string, std::string> ReadCacheIndex(const fs::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        out[line.substr(0, space)] = Trim(line.substr(space + 1));
    }
    return out;
}

std::string SourceStamp(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto written = fs::last_write_time(path, ec);
    if (ec) return {};
    return std::to_string(size) + ":" +
           std::to_string(written.time_since_epoch().count());
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& Titles() { return g_titles; }

bool Claimed(const fs::path& audio) {
    const std::string key = Lower(audio.string());
    return std::find(g_claimed.begin(), g_claimed.end(), key) != g_claimed.end();
}

std::vector<GeneratedFile> Build(const fs::path& exe_dir, const fs::path& game_root) {
    g_titles.clear();
    g_claimed.clear();

    const std::vector<Song> songs = ScanSongs(exe_dir);
    if (songs.empty()) return {};

    AudioDat game, sounds;
    if (!LoadShippedDat(game_root / kSourceArchive, game, sounds)) return {};
    if (game.IndexOf(kManager) < 0) {
        LARECOMP_APP_ERROR("[music] the shipped game.dat has no {}", kManager);
        return {};
    }

    const fs::path cache_dir = exe_dir / "models" / kCacheFolder;
    const fs::path banks_dir = cache_dir / "files" / "audio" / "x360" / "sfx" / "music";
    const fs::path config_dir = cache_dir / "files" / "audio" / "x360" / "config";
    std::error_code ec;
    fs::create_directories(banks_dir, ec);
    fs::create_directories(config_dir, ec);

    const fs::path index_path = cache_dir / "cache.txt";
    const std::map<std::string, std::string> cached = ReadCacheIndex(index_path);
    std::map<std::string, std::string> fresh;

    std::vector<GeneratedFile> files;
    for (const Song& song : songs) {
        const fs::path bank_path = banks_dir / song.bank;
        const std::string stamp = SourceStamp(song.audio);
        const auto previous = cached.find(song.bank);
        const bool reusable = !stamp.empty() && previous != cached.end() &&
                              previous->second == stamp && fs::exists(bank_path, ec);

        if (!reusable) {
            std::vector<uint8_t> mp3;
            if (!ReadFile(song.audio, mp3)) {
                LARECOMP_APP_ERROR("[music] cannot read {}", song.audio.string());
                continue;
            }
            std::vector<uint8_t> bank;
            BankInfo info;
            std::string error;
            if (!BuildMp3Bank(mp3, modloader::RageHash(song.bank + "_left"),
                              modloader::RageHash(song.bank + "_right"), bank, info, error)) {
                LARECOMP_APP_ERROR("[music] {}: {}", song.audio.filename().string(), error);
                continue;
            }
            if (!WriteFile(bank_path, bank)) {
                LARECOMP_APP_ERROR("[music] cannot write {}", bank_path.string());
                continue;
            }
            LARECOMP_APP_INFO(
                "[music] {} -> {} ({:.1f} s, {} Hz, {} ch, {} packets, {:.1f} MB, header {:#x})",
                song.audio.filename().string(), song.bank,
                static_cast<double>(info.samples) / info.sample_rate, info.sample_rate,
                info.channels, info.packets, static_cast<double>(bank.size()) / (1024.0 * 1024.0),
                info.header_end);
        }

        if (!AddSong(game, sounds, song)) {
            LARECOMP_APP_ERROR("[music] {}: the template records are missing from sounds.dat",
                               song.name);
            continue;
        }

        fresh[song.bank] = stamp;
        files.push_back(GeneratedFile{"audio/x360/sfx/music/" + song.bank, bank_path});
        g_titles.emplace_back(song.title,
                              song.display.empty() ? DefaultDisplay(song.title) : song.display);
        g_claimed.push_back(Lower(song.audio.string()));
    }

    if (files.empty()) return {};

    const fs::path game_out = config_dir / "game.dat";
    const fs::path sounds_out = config_dir / "sounds.dat";
    if (!WriteFile(game_out, game.Serialize()) || !WriteFile(sounds_out, sounds.Serialize())) {
        LARECOMP_APP_ERROR("[music] cannot write the rebuilt .dat pair");
        return {};
    }
    files.push_back(GeneratedFile{"audio/x360/config/game.dat", game_out});
    files.push_back(GeneratedFile{"audio/x360/config/sounds.dat", sounds_out});

    std::ofstream index(index_path, std::ios::trunc);
    for (const auto& [bank, stamp] : fresh) index << bank << ' ' << stamp << '\n';

    LARECOMP_APP_INFO("[music] {} track(s) on the radio, {} object(s) in game.dat",
                      g_titles.size(), game.object_count());
    return files;
}

}  // namespace mc::music
