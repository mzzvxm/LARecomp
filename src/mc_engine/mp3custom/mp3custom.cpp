#ifndef REXGLUE_HAS_XEO3_TARGET
#include "mp3custom.h"
#include "mp3_player.h"

#include "../logging.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(custom_music, true, "MCLA/Audio",
    "Expose <exe>/music/*.mp3 and the User Music folder as radio songs. The "
    "tracks are decoded on the host; the guest radio only carries the row.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(custom_music_volume, 0.5, "MCLA/Audio",
    "Trim applied on top of the in-game music slider for custom MP3 tracks. "
    "The shipped radio goes through the RAGE mixer and its ducking curves, "
    "which the host player does not, so 1.0 comes out noticeably louder.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// ── Guest addresses (see docs at the bottom of this file) ─────────────

// mcMusicManager, ctor sub_821EFB98.
constexpr uint32_t kMusicManagerGlobal = 0x8286D048;
constexpr uint32_t kStringTableGlobal  = 0x8286D7FC;

// The audio settings block, sub_82387B90() -> &unk_8288E920. mcAudioOptionsForm
// (sub_8264DBC8) copies five floats from +1240 into its rows, in the order the
// rows are built: effects, music, voice, player vehicle, cutscene. So the music
// slider lives at +1244.
//
// mgr+32 is *not* usable for this: the ctor parks 1.0 there and only the audio
// options form ever writes the real value, so a custom track started before the
// player has visited that screen would come in at full volume.
constexpr uint32_t kSettingsMusicVolume = 0x8288E920 + 1244;

constexpr uint32_t kGuestMallocFn      = 0x82130528;  // sysMemAllocator::Allocate
constexpr uint32_t kArrayAppendFn      = 0x8262E420;  // atArray::Append(this, grow)
constexpr uint32_t kGotoSongFn         = 0x821F0600;  // mcMusicManager::GotoSong(idx)
constexpr uint32_t kPlayFn             = 0x821F0108;  // mcMusicManager::Play(flags)

// mcMusicManager layout, all confirmed against sub_821F0600 / sub_821EF950 /
// sub_821EED18.
//   +32          float, music volume slider (written by mcAudioOptionsForm)
//   +48          current song index within the current genre
//   +52          current genre
//   +68          current song entry
//   +88 + 8*g    atArray<songEntry*> for genre g: ptr, u16 count, u16 capacity
constexpr uint32_t kMgrVolume      = 32;
constexpr uint32_t kMgrSongIndex   = 48;
constexpr uint32_t kMgrGenre       = 52;
constexpr uint32_t kMgrCurrentSong = 68;
constexpr uint32_t kMgrGenreArrays = 88;

// +24 is the "the RAGE music path is not the one playing" flag. The per-frame
// update callback sub_821F0838 bails out early on it:
//
//     v5 = *(a1 + 384);                  // the live sound object
//     if ( !v5 ) {
//         if ( *(a1 + 24) == 1 ) return 1;
//         ...
//         sub_821EF950(a1, 0);           // otherwise: try to start it again
//
// A custom track never produces a sound object, so without this flag the
// manager re-enters StartCurrentSong every single frame. Retail leaves it at 0
// (the ctor only sets it when audMusicXenon reports XMP unavailable), so
// toggling it per song is safe.
constexpr uint32_t kMgrNoCue = 24;

// MUSIC_GENRE_* order comes from the name table at off_827DA540.
constexpr uint32_t kGenreAll       = 7;  // MUSIC_GENRE_ALL
constexpr uint32_t kGenreOnTheFly  = 8;  // MUSIC_GENRE_ON_THE_FLY -> "Custom Playlists"

// Song entry, built by mcMusicManager::AddSong (sub_821EF310):
//   [0] music game object   [1] hash code   [2] char* display name
constexpr uint32_t kEntrySize = 12;

// ── The music game object ─────────────────────────────────────────────
//
// A real one is a record inside the audio metadata blob: sub_82144A20 hashes a
// name and binary-searches a sorted {offset, nameHash} table (sub_8214D6B8),
// returning blobBase + offset. AddSong reads exactly four fields out of it:
//
//   +10  u8    genre index (MUSIC_GENRE_*)
//   +11  u32   hash code -- UNALIGNED, and this is the song's identity
//   +19  u8    name length
//   +20  char  name, not NUL-terminated
//
// Sharing one borrowed object across every custom track was the original
// shortcut, and it is why the game could not tell custom tracks apart from each
// other or from the shipped song the object belonged to: the identity of "what
// is playing" is read off the OBJECT, not off the entry --
//
//   sub_821EEF08   *(*(mgr+68) + 11)     what is playing
//   sub_821EF1F8   *(*(mgr+68) + 11)     which cue to stop
//   sub_821F0700   entry[0] == resolved  find a song by name
//
// Those four (with AddSong) are the whole set of readers in the music path;
// nothing derives an index from the pointer and nothing checks that it lies
// inside the blob, so a plain guest allocation works. The playlist does not
// care either way -- sub_821EF5E0 stores ENTRY pointers at mgr+152, and the
// shuffle table at mgr+352 stores plain indices.
//
// The id we invent is never a real cue. That is strictly safer than the donor
// was: the paths that would start or stop a cue with it now ask for something
// that does not exist instead of asking for the donor song's cue.
constexpr uint32_t kObjSize    = 64;  // generous; the real record is larger
constexpr uint32_t kObjGenre   = 10;  // u8
constexpr uint32_t kObjHash    = 11;  // u32, unaligned
constexpr uint32_t kObjNameLen = 19;  // u8
constexpr uint32_t kObjName    = 20;  // char[]
constexpr uint32_t kObjNameMax = kObjSize - kObjName - 1;

// ── Guest memory helpers ──────────────────────────────────────────────

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

bool IsGuestPtr(uint32_t ea) { return ea >= 0x82000000u && ea < 0xC0000000u; }

uint32_t ReadGuestBE32(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return 0;
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t ReadGuestBE16(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return 0;
    const uint8_t* p = base + ea;
    return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1]));
}

void WriteGuestBE32(uint32_t ea, uint32_t v) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return;
    uint8_t* p = base + ea;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

void WriteGuestU8(uint32_t ea, uint8_t v) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return;
    base[ea] = v;
}

float ReadGuestBEFloat(uint32_t ea) {
    uint32_t bits = ReadGuestBE32(ea);
    float    out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

uint32_t CallGuest1(uint32_t fn_addr, uint32_t a0) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0);
}

uint32_t CallGuest2(uint32_t fn_addr, uint32_t a0, uint32_t a1) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1);
}

uint32_t AllocGuestString(const std::string& text) {
    uint8_t* base = GetMembase();
    if (!base) return 0;
    const uint32_t len = static_cast<uint32_t>(text.size()) + 1;
    uint32_t buf = CallGuest1(kGuestMallocFn, len);
    if (buf) std::memcpy(base + buf, text.c_str(), len);
    return buf;
}

// atStringHash, sub_821C9790: Jenkins one-at-a-time over the lower-cased
// string, with '\' folded to '/'. The final mixing constants are the usual
// h += h<<3 / h ^= h>>11 / h += h<<15 written as multiplies.
uint32_t MCLAHashString(const char* str) {
    uint32_t h = 0;
    for (const char* p = str; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c == '\\') c = '/';
        // sub_821C9790 keeps the character in a signed char and lets it
        // sign-extend into the accumulator, so anything >= 0x80 subtracts.
        h = (h + static_cast<uint32_t>(static_cast<int32_t>(c))) * 1025u;
        h ^= h >> 6;
    }
    h *= 9u;
    h ^= h >> 11;
    h *= 32769u;
    return h;
}

// Reimplements the tail of sub_82218268's map: bucket array at +0, u16 bucket
// count at +4, nodes are {hash, value, next}.
bool InsertHashMapEntry(uint32_t hashmap_ea, uint32_t hash, uint32_t value) {
    uint32_t buckets     = ReadGuestBE32(hashmap_ea);
    uint16_t num_buckets = ReadGuestBE16(hashmap_ea + 4);
    if (!num_buckets || !buckets) return false;

    uint32_t node = CallGuest1(kGuestMallocFn, kEntrySize);
    if (!node) return false;

    const uint32_t bucket = hash % num_buckets;
    WriteGuestBE32(node + 0, hash);
    WriteGuestBE32(node + 4, value);
    WriteGuestBE32(node + 8, ReadGuestBE32(buckets + bucket * 4));
    WriteGuestBE32(buckets + bucket * 4, node);
    return true;
}

// ── Track list ────────────────────────────────────────────────────────

struct Track {
    std::wstring path;
    std::string  title;         // filename stem, shown in the radio list
    uint32_t     entry_ea = 0;  // guest song entry once installed
    uint32_t     title_ea = 0;  // guest copy of the title (song entry [2])
    uint32_t     object_ea = 0; // synthetic music game object (song entry [0])
};

std::vector<Track> g_tracks;
bool               g_installed = false;
bool               g_strings_registered = false;
int                g_current   = -1;  // index into g_tracks, -1 = not ours

std::string WideToUtf8(const std::wstring& w) {
#if defined(_WIN32)
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), len, nullptr, nullptr);
    return out;
#else
    return std::string(w.begin(), w.end());
#endif
}

void ScanFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;

    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;

        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](wchar_t c) { return wchar_t(::towlower(c)); });
        if (ext != L".mp3") continue;

        Track t;
        t.path  = e.path().wstring();
        t.title = WideToUtf8(e.path().stem().wstring());
        if (t.title.empty()) continue;
        // The row text is copied into a 132-byte guest buffer by the string
        // table, and mcMusicManager's own names never run long either.
        if (t.title.size() > 100) t.title.resize(100);
        g_tracks.push_back(std::move(t));
    }
}

void CollectTracks() {
    std::error_code ec;

#if defined(_WIN32)
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        ScanFolder(std::filesystem::path(exe).parent_path() / L"music");
    }
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) {
        ScanFolder(std::filesystem::path(home) / L"Documents" / L"Rockstar Games" / L"LARecomp" /
                   L"User Music");
    }
#else
    ScanFolder(std::filesystem::current_path(ec) / "music");
#endif

    std::sort(g_tracks.begin(), g_tracks.end(),
              [](const Track& a, const Track& b) { return a.title < b.title; });
}

// ── Guest installation ────────────────────────────────────────────────

uint32_t GenreArray(uint32_t mgr, uint32_t genre) {
    return mgr + kMgrGenreArrays + 8 * genre;
}

// Appends one entry pointer to the genre's atArray through the game's own
// grow-by-16 Append, so the capacity bookkeeping stays the game's business.
bool AppendToGenre(uint32_t mgr, uint32_t genre, uint32_t entry_ea) {
    uint32_t slot = CallGuest2(kArrayAppendFn, GenreArray(mgr, genre), 16);
    if (!IsGuestPtr(slot)) return false;
    WriteGuestBE32(slot, entry_ea);
    return true;
}

// Installs every scanned track into `mgr`. Called from the ctor, just before
// mcMusicManager::RebuildShuffle (sub_821EED18) runs, so the shuffle tables are
// built with the custom rows already counted.
void InstallTracks(uint32_t mgr) {
    if (g_installed || g_tracks.empty() || !IsGuestPtr(mgr)) return;

    // Sanity only: the shipped rows have to be in by now, because the shuffle
    // tables are rebuilt right after this and they count what is in the arrays.
    uint32_t all_ptr   = ReadGuestBE32(GenreArray(mgr, kGenreAll));
    uint16_t all_count = ReadGuestBE16(GenreArray(mgr, kGenreAll) + 4);
    if (!IsGuestPtr(all_ptr) || all_count == 0) {
        MC_WARN("[mp3custom] genre ALL is empty at install time, skipping");
        return;
    }

    uint32_t added = 0;

    for (size_t i = 0; i < g_tracks.size(); ++i) {
        Track& t = g_tracks[i];

        uint32_t title_ea = AllocGuestString(t.title);
        uint32_t entry_ea = CallGuest1(kGuestMallocFn, kEntrySize);
        uint32_t obj_ea   = CallGuest1(kGuestMallocFn, kObjSize);
        if (!IsGuestPtr(title_ea) || !IsGuestPtr(entry_ea) || !IsGuestPtr(obj_ea)) {
            MC_WARN("[mp3custom] guest allocation failed for '{}'", t.title);
            break;
        }

        // An id the shipped songs cannot collide with. It is the song's identity
        // everywhere the game asks "what is playing" (sub_821EEF08), so it has to
        // be unique per track -- one shared object was the old bug.
        const uint32_t hash_code = 0xC0DE0000u + static_cast<uint32_t>(i);

        // Its own music game object, laid out the way AddSong reads one.
        uint8_t* base = GetMembase();
        if (base) std::memset(base + obj_ea, 0, kObjSize);
        WriteGuestU8(obj_ea + kObjGenre, uint8_t(kGenreOnTheFly));
        WriteGuestBE32(obj_ea + kObjHash, hash_code);
        std::string obj_name = t.title;
        if (obj_name.size() > kObjNameMax) obj_name.resize(kObjNameMax);
        WriteGuestU8(obj_ea + kObjNameLen, uint8_t(obj_name.size()));
        if (base) std::memcpy(base + obj_ea + kObjName, obj_name.data(), obj_name.size());

        WriteGuestBE32(entry_ea + 0, obj_ea);
        WriteGuestBE32(entry_ea + 4, hash_code);
        WriteGuestBE32(entry_ea + 8, title_ea);
        t.entry_ea = entry_ea;
        t.title_ea = title_ea;
        t.object_ea = obj_ea;

        if (!AppendToGenre(mgr, kGenreAll, entry_ea) ||
            !AppendToGenre(mgr, kGenreOnTheFly, entry_ea)) {
            MC_WARN("[mp3custom] append failed for '{}'", t.title);
            break;
        }

        ++added;
        MC_INFO("[mp3custom] installed '{}' entry=0x{:08X} obj=0x{:08X} id=0x{:08X}",
                t.title, entry_ea, obj_ea, hash_code);
    }

    g_installed = true;
    MC_INFO("[mp3custom] {} custom track(s) in MUSIC_GENRE_ALL and MUSIC_GENRE_ON_THE_FLY", added);
}

// The list row is not raw text: sub_82631FD0 resolves it through the string
// table, and a miss lands in sub_82217CA0, which renders "!!<key>!!" and caches
// that under the key's hash. So each title needs an entry of its own, keyed on
// the title itself so the fallback text and the resolved text agree.
//
// This cannot run from the manager ctor -- the string table global is still
// null that early -- so it is retried from the tick until it takes. Nodes are
// pushed at the head of the bucket, which also means a "!!...!!" node the UI
// already cached gets shadowed rather than fought over.
void RegisterStrings() {
    if (g_strings_registered || g_tracks.empty()) return;

    uint32_t table = ReadGuestBE32(kStringTableGlobal);
    if (!IsGuestPtr(table)) return;

    uint32_t done = 0;
    for (const Track& t : g_tracks) {
        if (!t.title_ea) continue;
        if (InsertHashMapEntry(table + 16, MCLAHashString(t.title.c_str()), t.title_ea)) ++done;
    }
    if (!done) return;

    g_strings_registered = true;
    MC_INFO("[mp3custom] {} title(s) registered in the string table", done);
}

// Same 0..1 value the engine gets through sub_821E6980 -> sub_82547CB0. Falls
// back to mgr+32 if the settings block has not been filled in yet.
//
// That value is squared on the way out. XAudio2::SetVolume is linear amplitude
// while the RAGE mixer treats the slider as a taper, so feeding it straight
// through makes the middle of the slider far louder than the shipped radio at
// the same setting. The cvar on top is the remaining fixed offset.
float CurrentMusicVolume(uint32_t mgr) {
    float v = ReadGuestBEFloat(kSettingsMusicVolume);
    if (!(v >= 0.0f && v <= 1.0f)) v = ReadGuestBEFloat(mgr + kMgrVolume);
    if (!(v >= 0.0f && v <= 1.0f)) v = 1.0f;

    double trim = REXCVAR_GET(custom_music_volume);
    if (trim < 0.0) trim = 0.0;
    if (trim > 1.0) trim = 1.0;

    return static_cast<float>(double(v) * double(v) * trim);
}

int TrackForEntry(uint32_t entry_ea) {
    if (!entry_ea) return -1;
    for (size_t i = 0; i < g_tracks.size(); ++i) {
        if (g_tracks[i].entry_ea == entry_ea) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

// ── Public entry points ───────────────────────────────────────────────

void InitCustomMusic() {
    if (!REXCVAR_GET(custom_music)) return;

    CollectTracks();
    if (g_tracks.empty()) {
        MC_INFO("[mp3custom] no .mp3 found in <exe>/music or the User Music folder");
        return;
    }

    if (!mp3custom::PlayerInit()) {
        MC_WARN("[mp3custom] host player unavailable, custom tracks disabled");
        g_tracks.clear();
        return;
    }

    MC_INFO("[mp3custom] {} track(s) queued for install", g_tracks.size());
}

void TickCustomMusic() {
    if (g_tracks.empty()) return;

    uint32_t mgr = ReadGuestBE32(kMusicManagerGlobal);
    if (!IsGuestPtr(mgr)) return;

    RegisterStrings();  // no-op once the string table has taken the titles

    // Follow the in-game music slider so the custom station is not louder than
    // the shipped one.
    mp3custom::PlayerSetVolume(CurrentMusicVolume(mgr));

    if (g_current < 0) return;

    // The player stops on its own at the end of the file; move the station on
    // exactly the way the UI would, which re-enters sub_821EF950 and therefore
    // our start hook.
    if (mp3custom::PlayerTrackEnded()) {
        uint32_t genre = ReadGuestBE32(mgr + kMgrGenre);
        uint16_t count = ReadGuestBE16(GenreArray(mgr, genre) + 4);
        if (count > 0) {
            uint32_t next = (ReadGuestBE32(mgr + kMgrSongIndex) + 1) % count;
            g_current = -1;
            CallGuest2(kGotoSongFn, mgr, next);
            CallGuest2(kPlayFn, mgr, 0);
        } else {
            g_current = -1;
            mp3custom::PlayerStop();
        }
    }
}

// ── Midasm hooks ──────────────────────────────────────────────────────

// 0x821EFF4C, `bl sub_821EED18` at the tail of the mcMusicManager ctor. r31 is
// the manager; both AddSong passes have already run, RebuildShuffle has not.
void MCLA_CustomMusic_Install(PPCRegister& r31) {
    InstallTracks(static_cast<uint32_t>(r31.u32));
}

// 0x821EFB5C, the `bl sub_821F4E78` inside mcMusicManager::StartCurrentSong
// (sub_821EF950) that hands the current song's cue to the audio engine. r31 is
// the manager. Returning true jumps to loc_821EFB78, which is the same tail the
// "music volume is off" path takes: the callback pair at +376/+380 is still
// written, but no guest cue is started.
bool MCLA_CustomMusic_StartSong(PPCRegister& r31) {
    uint32_t mgr = static_cast<uint32_t>(r31.u32);
    int      idx = TrackForEntry(ReadGuestBE32(mgr + kMgrCurrentSong));

    if (idx < 0) {
        if (g_current >= 0) {
            g_current = -1;
            mp3custom::PlayerStop();
        }
        WriteGuestBE32(mgr + kMgrNoCue, 0);  // shipped song: normal RAGE path
        return false;
    }

    // Stops sub_821F0838 from calling StartCurrentSong again every frame. The
    // `*(a1 + 24) != 1` test at the top of sub_821EF950 was already taken for
    // this call, so writing it here only affects the frames that follow.
    WriteGuestBE32(mgr + kMgrNoCue, 1);

    // The manager re-enters this path on a retry or a repeated select; without
    // this the decode thread would be torn down and restarted before it ever
    // finished, which is silence plus a thread churn the frame rate feels.
    // It is also the resume side of MCLA_CustomMusic_Pause.
    if (g_current == idx && mp3custom::PlayerIsPlaying()) {
        mp3custom::PlayerPause(false);
        return true;
    }

    g_current = idx;
    // Before the first buffer, not on the next tick: the voice would otherwise
    // open at whatever the master voice defaults to.
    mp3custom::PlayerSetVolume(CurrentMusicVolume(mgr));
    mp3custom::PlayerPlayFile(g_tracks[size_t(idx)].path);
    MC_INFO("[mp3custom] playing '{}'", g_tracks[size_t(idx)].title);
    return true;
}

// 0x821EF1F8, mcMusicManager::Stop(this, fade). Every teardown of the radio
// funnels through here -- the garage entry reaches it via sub_821F0C10, and it
// is also where the manager would hand a stop to audMusicXenon when mgr+24 is
// set. Without this the host track keeps playing over the garage music.
void MCLA_CustomMusic_Stop(PPCRegister& r3) {
    if (g_current < 0) return;
    g_current = -1;
    mp3custom::PlayerStop();
    uint32_t mgr = static_cast<uint32_t>(r3.u32);
    if (IsGuestPtr(mgr)) WriteGuestBE32(mgr + kMgrNoCue, 0);
}

// 0x821EF158, mcMusicManager::Pause(this, ...). Resumed from the early-out in
// MCLA_CustomMusic_StartSong, which is what the manager calls back into.
void MCLA_CustomMusic_Pause(PPCRegister& r3) {
    (void)r3;
    if (g_current < 0) return;
    mp3custom::PlayerPause(true);
}

// 0x821F0600, mcMusicManager::GotoSong(this, index): the one place that moves
// mgr+68 to a new song, from the UI, the shuffle and our own end-of-track
// advance alike. Clearing the flag here means a shipped song picked right after
// a custom one is not stuck on the "no cue" branch of sub_821EF950.
void MCLA_CustomMusic_SelectSong(PPCRegister& r3) {
    uint32_t mgr = static_cast<uint32_t>(r3.u32);
    if (IsGuestPtr(mgr)) WriteGuestBE32(mgr + kMgrNoCue, 0);
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>

void InitCustomMusic() {}
void TickCustomMusic() {}
void MCLA_CustomMusic_Install(PPCRegister&) {}
bool MCLA_CustomMusic_StartSong(PPCRegister&) { return false; }
void MCLA_CustomMusic_SelectSong(PPCRegister&) {}
void MCLA_CustomMusic_Stop(PPCRegister&) {}
void MCLA_CustomMusic_Pause(PPCRegister&) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
