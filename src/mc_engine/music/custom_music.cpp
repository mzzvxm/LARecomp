#include "mc_engine/music/custom_music.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "../../larecomp_log.h"
#include "mc_engine/boot_progress.h"
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
constexpr const char* kWaveSlots = "audio/x360/config/waveslots.xml";
constexpr const char* kTemplate = "ROCK_CSS_RATISDEAD";
constexpr const char* kManager = "MUSIC_0_MANAGER";

const char* const kGenres[] = {"ECLECTIC", "ELECTRONIC", "HARDROCK", "HIPHOP",
                               "ROCK",     "TECHNO",     "WEST_RAP"};
constexpr int kGenreCount = 7;

// MUSIC_0_MANAGER lists its songs in seven groups; each group holds exactly one
// genre, in this order (measured, and every shipped song agrees).
constexpr int kGroupOfGenre[kGenreCount] = {6, 2, 0, 4, 1, 3, 5};

// A group's song count is ONE BYTE in the manager record (sub_821EF450 reads it
// with lbz and counts with a byte). Write a 256th song into a group and the
// count wraps to zero: the loader then reads the next group's count out of the
// middle of a song hash, walks off the end of the record and dies before the
// first frame. That was the ~250-track ceiling -- 12 shipped ECLECTIC songs
// plus 243 custom ones, all landing in the same group because ECLECTIC is the
// default genre.
//
// Which group a song is listed in does NOT decide which genre it plays under.
// sub_821EF310 takes the hash from the group body, resolves the game object,
// and files the song by the object's own genre byte (+10) into a per-genre
// atArray whose count is a u16. So a full group can spill into any other one:
// the track still shows up under its own genre, and the ceiling becomes
// 7 x 255 = 1785 songs instead of 255.
constexpr int kGroupCapacity = 255;

// How far the slot header budget is allowed to be pushed. Not a taste call: a
// bank goes into the archive as one plain file, and the modloader refuses an
// entry over 0x3FFFFFFF bytes. Both waves carry the whole bitstream, so a bank
// stays under a gigabyte while the MP3 is under ~512 MB -- about three and a
// half hours at 320 kbps -- and the header for that is a shade under 2 MB.
//
// Reaching it would grow the seven STREAM slots by about 2 MB each, on a slot
// pool that ships at 20 MB -- and only a folder that really holds a track that
// long ever pays it.
constexpr uint32_t kHeaderBudgetCeiling = 0x200000;

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

// One plain file out of the archive, inflated if it is stored deflated -- which
// is what bit30 of the entry flag says.
bool ReadArchiveFile(const modloader::Rpf3Reader& archive, const std::string& path,
                     std::vector<uint8_t>& out) {
    modloader::Rpf3Entry entry;
    if (!archive.Find(path, entry)) {
        LARECOMP_APP_ERROR("[music] {} has no {}", archive.path().string(), path);
        return false;
    }
    if (!archive.ReadFile(entry, out)) {
        LARECOMP_APP_ERROR("[music] cannot read {}", path);
        return false;
    }
    if (entry.flag & 0x40000000u) {
        int out_length = 0;
        char* inflated = stbi_zlib_decode_noheader_malloc(
            reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), &out_length);
        if (!inflated || out_length <= 0) {
            LARECOMP_APP_ERROR("[music] cannot inflate {}", path);
            return false;
        }
        out.assign(inflated, inflated + out_length);
        std::free(inflated);
    }
    return true;
}

// The .dat pair, plus the wave slot table the header budget lives in. A missing
// waveslots.xml is not fatal: it only means tracks stay inside the stock budget.
bool LoadShippedDat(const fs::path& archive_path, AudioDat& game, AudioDat& sounds,
                    std::string& wave_slots) {
    modloader::Rpf3Reader archive;
    if (!archive.Open(archive_path)) {
        LARECOMP_APP_ERROR("[music] cannot open {}", archive_path.string());
        return false;
    }

    AudioDat* targets[2] = {&game, &sounds};
    const char* names[2] = {"game.dat", "sounds.dat"};
    for (int i = 0; i < 2; ++i) {
        const std::string path = std::string("audio/x360/config/") + names[i];
        std::vector<uint8_t> raw;
        if (!ReadArchiveFile(archive, path, raw)) return false;
        std::string error;
        if (!targets[i]->Parse(raw, error)) {
            LARECOMP_APP_ERROR("[music] {}: {}", path, error);
            return false;
        }
    }

    std::vector<uint8_t> xml;
    if (ReadArchiveFile(archive, kWaveSlots, xml)) {
        wave_slots.assign(xml.begin(), xml.end());
    }
    return true;
}

// ---------------------------------------------------------------- the slot budget

// How much header a bank already built is asking for: `blockSize` at +0x2C is
// the offset of chunk 0, so it is exactly what the slot has to hold. Read off
// the cached file so a reused bank still counts towards the budget.
uint32_t BankHeaderSize(const fs::path& bank) {
    std::ifstream input(bank, std::ios::binary);
    if (!input) return 0;
    uint8_t head[0x30] = {};
    input.read(reinterpret_cast<char*>(head), sizeof(head));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(head))) return 0;
    return (static_cast<uint32_t>(head[0x2C]) << 24) | (static_cast<uint32_t>(head[0x2D]) << 16) |
           (static_cast<uint32_t>(head[0x2E]) << 8) | head[0x2F];
}

// Where a `<Key value="N" />` sits inside one slot, and what N is.
bool FindSlotValue(const std::string& slot, const char* key, size_t& first, size_t& last,
                   uint32_t& value) {
    const size_t at = slot.find(key);
    if (at == std::string::npos) return false;
    first = at + std::strlen(key);
    last = slot.find('"', first);
    if (last == std::string::npos) return false;
    value = static_cast<uint32_t>(
        std::strtoul(slot.substr(first, last - first).c_str(), nullptr, 10));
    return true;
}

// Raises the header budget of every STREAM slot in waveslots.xml.
//
// A slot's `Size` is NOT its header budget: it is the header plus the streaming
// window that follows it -- 2 x 0x20000 for the music slots, 0x10000 for the
// others -- and all 41 slots share one 20 MB pool. So the window is preserved
// and only the header part grows, which is what the extra heap actually buys.
//
// Only STREAM slots are touched: those are the ones a music bank loads into,
// and one of them also stages the header read in audWaveSlot::RequestLoad. WAVE
// and BANK slots, and any slot already roomier than the budget, are left alone.
std::string PatchWaveSlots(const std::string& xml, uint32_t budget, int& patched,
                           uint32_t& added_bytes) {
    patched = 0;
    added_bytes = 0;

    std::string out;
    out.reserve(xml.size() + 256);

    size_t at = 0;
    while (true) {
        const size_t open = xml.find("<Slot>", at);
        if (open == std::string::npos) break;
        const size_t close = xml.find("</Slot>", open);
        if (close == std::string::npos) break;

        const size_t end = close + 7;
        std::string slot = xml.substr(open, end - open);
        out.append(xml, at, open - at);
        at = end;

        size_t header_first = 0, header_last = 0, size_first = 0, size_last = 0;
        uint32_t header = 0, size = 0;
        if (slot.find(">STREAM<") == std::string::npos ||
            !FindSlotValue(slot, "<MaxHeaderSize value=\"", header_first, header_last, header) ||
            !FindSlotValue(slot, "<Size value=\"", size_first, size_last, size) ||
            header >= budget || size < header) {
            out += slot;
            continue;
        }

        const uint32_t grow = budget - header;
        // The Size field sits after MaxHeaderSize in every shipped slot, so it
        // is rewritten first and the earlier offsets stay valid.
        slot = slot.substr(0, size_first) + std::to_string(size + grow) + slot.substr(size_last);
        slot = slot.substr(0, header_first) + std::to_string(budget) + slot.substr(header_last);

        added_bytes += grow;
        ++patched;
        out += slot;
    }
    out.append(xml, at, std::string::npos);
    return out;
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

// How many songs each group already lists. False when the record does not walk
// cleanly, which means the shape assumed here is not the shape on disk.
bool ReadGroupCounts(const std::vector<uint8_t>& record, int (&out)[kGenreCount]) {
    if (record.size() < 43) return false;
    size_t p = 42;
    for (int g = 0; g < kGenreCount; ++g) {
        if (p >= record.size()) return false;
        const size_t n = record[p];
        if (p + 1 + 4 * n > record.size()) return false;
        out[g] = static_cast<int>(n);
        p += 1 + 4 * n;
    }
    return p == record.size();
}

// Appends whole lists of songs to the manager's groups in one pass. Done once
// at the end rather than per song: ReplaceObject leaves the old body behind as
// dead space, so rewriting the record N times costs O(N^2) bytes in game.dat.
std::vector<uint8_t> ManagerWith(const std::vector<uint8_t>& record,
                                 const std::vector<uint32_t> (&added)[kGenreCount]) {
    std::vector<uint8_t> out(record.begin(), record.begin() + 42);
    size_t p = 42;
    for (int g = 0; g < kGenreCount && p < record.size(); ++g) {
        const uint8_t n = record[p];
        const size_t body = 4ull * n;
        if (p + 1 + body > record.size()) break;
        out.push_back(static_cast<uint8_t>(n + added[g].size()));
        out.insert(out.end(), record.begin() + p + 1, record.begin() + p + 1 + body);
        for (uint32_t song_hash : added[g]) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>(song_hash >> (24 - 8 * i)));
            }
        }
        p += 1 + body;
    }
    return out;
}

// The group this song can actually be listed in: its own genre's while that one
// has room, otherwise the emptiest group there is. Spilling is free -- the song
// still plays under its own genre, see kGroupCapacity -- so the only thing lost
// is tidiness in a file nobody reads. -1 when every group is full.
int PlaceInGroup(int genre, const int (&counts)[kGenreCount]) {
    const int own = kGroupOfGenre[genre];
    if (counts[own] < kGroupCapacity) return own;

    int best = -1;
    for (int g = 0; g < kGenreCount; ++g) {
        if (counts[g] >= kGroupCapacity) continue;
        if (best < 0 || counts[g] < counts[best]) best = g;
    }
    return best;
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

// The two wave leaves, the cue that pairs them and the radio entry. Records are
// cloned from a shipped song so every field nobody has decoded yet keeps its
// shipped value. Listing the entry in the manager is the caller's job, done for
// every song at once at the end.
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

    boot::BeginPhase("Music", static_cast<int>(songs.size()));

    AudioDat game, sounds;
    std::string wave_slots;
    if (!LoadShippedDat(game_root / kSourceArchive, game, sounds, wave_slots)) {
        boot::EndPhase();
        return {};
    }
    if (game.IndexOf(kManager) < 0) {
        LARECOMP_APP_ERROR("[music] the shipped game.dat has no {}", kManager);
        boot::EndPhase();
        return {};
    }

    // What the shipped record already lists, so the byte each group's count
    // lives in never wraps.
    const std::vector<uint8_t> manager = game.Record(kManager);
    int group_count[kGenreCount] = {};
    if (!ReadGroupCounts(manager, group_count)) {
        LARECOMP_APP_ERROR("[music] {} does not walk as seven groups, radio left alone", kManager);
        boot::EndPhase();
        return {};
    }
    std::vector<uint32_t> added[kGenreCount];
    int spilled = 0, refused = 0, failed = 0;

    // Tracks are built against a budget nobody is going to hit, and the slot
    // table is then written to fit the tallest header that actually turned up.
    // Guest heap is only spent on what the folder really needs, and a track is
    // no longer refused for being long -- see kStockHeaderSize.
    //
    // That only holds while the table can actually be rewritten, so it is
    // checked here, before a single track goes in. A bank whose header is past
    // what the slot reads plays a truncated chunk table: the stream runs into
    // bytes that are not packets, the XMA context tries them as XMA ("There are
    // no bits to copy!"), the track repeats itself and the audio thread can
    // hang. Without a table to patch, long tracks are refused like they always
    // were.
    uint32_t budget_ceiling = kStockHeaderSize;
    if (wave_slots.empty()) {
        LARECOMP_APP_ERROR("[music] {} could not be read, so tracks stay inside the stock {} byte "
                           "header budget",
                           kWaveSlots, kStockHeaderSize);
    } else {
        int probe_patched = 0;
        uint32_t probe_bytes = 0;
        PatchWaveSlots(wave_slots, kHeaderBudgetCeiling, probe_patched, probe_bytes);
        if (probe_patched > 0) {
            budget_ceiling = kHeaderBudgetCeiling;
        } else {
            LARECOMP_APP_ERROR("[music] {} has no STREAM slot to raise, so tracks stay inside the "
                               "stock {} byte header budget",
                               kWaveSlots, kStockHeaderSize);
        }
    }
    uint32_t needed_header = kStockHeaderSize;

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
        // Stepped here rather than at the end, so a track that is skipped still
        // moves the bar and still names itself in the popup.
        boot::Step(song.display.empty() ? song.bank : song.display);

        // Room in the record first: a bank built for a song the manager cannot
        // list is several megabytes written for nothing.
        const int group = PlaceInGroup(song.genre, group_count);
        if (group < 0) {
            ++refused;
            continue;
        }
        if (group != kGroupOfGenre[song.genre]) ++spilled;

        const fs::path bank_path = banks_dir / song.bank;
        const std::string stamp = SourceStamp(song.audio);
        const auto previous = cached.find(song.bank);
        bool reusable = !stamp.empty() && previous != cached.end() &&
                        previous->second == stamp && fs::exists(bank_path, ec);
        // A cached bank was built against whatever budget that boot had. If it
        // asks for more than this boot can give, it goes through the builder
        // again, which refuses it with the reason instead of shipping it.
        uint32_t cached_header = 0;
        if (reusable) {
            cached_header = BankHeaderSize(bank_path);
            if (cached_header == 0 || cached_header > budget_ceiling) reusable = false;
        }

        if (!reusable) {
            std::vector<uint8_t> mp3;
            if (!ReadFile(song.audio, mp3)) {
                LARECOMP_APP_ERROR("[music] cannot read {}", song.audio.string());
                ++failed;
                continue;
            }
            std::vector<uint8_t> bank;
            BankInfo info;
            std::string error;
            if (!BuildMp3Bank(mp3, modloader::RageHash(song.bank + "_left"),
                              modloader::RageHash(song.bank + "_right"), budget_ceiling, bank,
                              info, error)) {
                LARECOMP_APP_ERROR("[music] {}: {}", song.audio.filename().string(), error);
                ++failed;
                continue;
            }
            if (!WriteFile(bank_path, bank)) {
                LARECOMP_APP_ERROR("[music] cannot write {}", bank_path.string());
                ++failed;
                continue;
            }
            LARECOMP_APP_INFO(
                "[music] {} -> {} ({:.1f} s, {} Hz, {} ch, {} packets, {:.1f} MB, header {:#x})",
                song.audio.filename().string(), song.bank,
                static_cast<double>(info.samples) / info.sample_rate, info.sample_rate,
                info.channels, info.packets, static_cast<double>(bank.size()) / (1024.0 * 1024.0),
                info.header_end);
            needed_header = std::max(needed_header, info.block_size);
        } else {
            needed_header = std::max(needed_header, cached_header);
        }

        if (!AddSong(game, sounds, song)) {
            LARECOMP_APP_ERROR("[music] {}: the template records are missing from sounds.dat",
                               song.name);
            ++failed;
            continue;
        }

        added[group].push_back(modloader::RageHash(song.name));
        ++group_count[group];

        fresh[song.bank] = stamp;
        files.push_back(GeneratedFile{"audio/x360/sfx/music/" + song.bank, bank_path});
        g_titles.emplace_back(song.title,
                              song.display.empty() ? DefaultDisplay(song.title) : song.display);
        g_claimed.push_back(Lower(song.audio.string()));
    }

    if (files.empty()) {
        boot::EndPhase();
        return {};
    }

    game.ReplaceObject(kManager, ManagerWith(manager, added));

    if (spilled > 0) {
        LARECOMP_APP_INFO("[music] {} track(s) listed under another genre's group because theirs "
                          "was full at {} -- they still play under their own genre",
                          spilled, kGroupCapacity);
    }
    if (refused > 0) {
        LARECOMP_APP_ERROR("[music] the radio is full at {} songs -- {} track(s) skipped",
                           kGenreCount * kGroupCapacity, refused);
    }
    if (failed > 0) {
        // Nothing catches these any more: the old host player is off by default,
        // so a track that does not build simply is not on the radio. Say so
        // once, plainly, instead of leaving the person to count the rows.
        LARECOMP_APP_ERROR(
            "[music] {} track(s) could not be built and are NOT on the radio -- see the lines "
            "above for which and why",
            failed);
    }

    // A budget past the shipped one only works if the slot table says so, and
    // the table has to be shipped whenever the tracks need it -- including on a
    // boot where every bank came from the cache and nothing was built.
    // needed_header can only pass the stock budget when budget_ceiling did, so
    // the table is known to be patchable by now.
    if (needed_header > kStockHeaderSize) {
        {
            int patched = 0;
            uint32_t added_bytes = 0;
            const std::string xml =
                PatchWaveSlots(wave_slots, needed_header, patched, added_bytes);
            const fs::path slots_out = config_dir / "waveslots.xml";
            const std::vector<uint8_t> bytes(xml.begin(), xml.end());
            if (patched > 0 && WriteFile(slots_out, bytes)) {
                files.push_back(GeneratedFile{kWaveSlots, slots_out});
                LARECOMP_APP_INFO(
                    "[music] slot header budget raised to {} bytes on {} streaming slot(s) "
                    "(stock is {}), {:.2f} MB more guest heap",
                    needed_header, patched, kStockHeaderSize,
                    static_cast<double>(added_bytes) / (1024.0 * 1024.0));
            } else {
                LARECOMP_APP_ERROR("[music] cannot write the patched {} -- tracks with a header "
                                   "past {} bytes will not stream correctly this boot",
                                   kWaveSlots, kStockHeaderSize);
            }
        }
    }

    const fs::path game_out = config_dir / "game.dat";
    const fs::path sounds_out = config_dir / "sounds.dat";
    if (!WriteFile(game_out, game.Serialize()) || !WriteFile(sounds_out, sounds.Serialize())) {
        LARECOMP_APP_ERROR("[music] cannot write the rebuilt .dat pair");
        boot::EndPhase();
        return {};
    }
    files.push_back(GeneratedFile{"audio/x360/config/game.dat", game_out});
    files.push_back(GeneratedFile{"audio/x360/config/sounds.dat", sounds_out});

    std::ofstream index(index_path, std::ios::trunc);
    for (const auto& [bank, stamp] : fresh) index << bank << ' ' << stamp << '\n';

    LARECOMP_APP_INFO("[music] {} track(s) on the radio, {} object(s) in game.dat",
                      g_titles.size(), game.object_count());
    boot::EndPhase();
    return files;
}

}  // namespace mc::music
