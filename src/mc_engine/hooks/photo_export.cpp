// EXPORT ALBUM: the photo album's "PUBLISH ALBUM" button writes the album's photos to a
// folder on the PC instead of uploading them to the Social Club, which is gone.
//
// What the game does (default.xex, 16/11/2026):
//
//   tune/ui/photoalbum.sc.xml, LocalMode and FullScreenPicture:
//     uinput.x_pressed  -> net.AuthenticateGamer('Upload')
//     net.authenticated -> photoAlbum.UploadPhoto()
//     photoAlbum.uploadAlbumStart -> UI.ActivateAndFocus(PA_UploadPhotoStart_FullScreen)
//   tune/ui/photo/dialog.sc.xml:
//     PA_UploadPhotoStart_FullScreen  "Online_Upload_Photo" (please wait), no prompts;
//       photoAlbum.uploadAlbumSuccess -> PA_UploadPhotoSuccess_FullScreen
//       photoAlbum.uploadPhotoFail    -> PA_UploadPhotoFailFullScreen
//                                        ("Online_Upload_Photo_Error_Retry", YES runs
//                                        photoAlbum.UploadPhoto() again)
//   The button's label is the string-table key PA_publish, which the prompt builder
//   sub_8220F040 puts on three of the album's prompts. Those four keys are used by the
//   photo album's dialogs and nothing else.
//
//   sub_82289AB0(netui, kind)  net.AuthenticateGamer (mcNetUI command 26). Fails with
//                              failNotOnline / failNotSignedIn / failPrivileges unless
//                              the profile is signed in to LIVE; on success it posts
//                              net.authenticated (event 8) and returns 1. 'Upload' is
//                              only ever asked for by the photo album.
//   sub_8263FA10, command 14   photoAlbum.UploadPhoto: needs a LIVE profile, a photo in
//                              the album and local mode (album+1225 == 0); posts
//                              uploadAlbumStart (event 16) and queues every occupied
//                              slot for rage::rlUpload.
//
//   The album (mcUIPhotoAlbum, from the "PhotoAlbumScene" state):
//     album+428   the PHOTO_ALBUM save buffer, laid out as the file on disk:
//                 +0 photo count, +4 occupied mask, +12 sixteen {slot, JPEG size, id},
//                 +204 + 204800 * n slot n's JPEG (baseline JFIF, 1280x720, the bytes
//                 the game decodes for the album -- written out as they are).
//     album+856   photo count;  album+860 occupied-slot mask (bit n = slot n).
//     album+1225  server mode (the online album; unused since the service closed).
//
//   The string table (*0x8286D7FC):
//     mgr+4    txtStringTable: map {u32 buckets, buckets*, count}, node {hash, entry,
//              next}; entry+8 is the text, UTF-8 (sub_825F2200 narrows the .strtbl's
//              UTF-16 with sub_821E4958). Rebuilt by every language switch
//              (sub_82218A38) and when downloaded content brings a table of its own.
//     mgr+16   the display cache, 132-byte buffers. sub_82218268 refreshes a cached
//              buffer from mgr+4 on every lookup of a key the loaded table has
//              (sub_822180B0), so a key that exists is changed at mgr+4, not here --
//              and the refresh copies with no bound: the text must fit 127 bytes.
//     mgr+0x1C the language index (en es fr de it pt = 0..5).
//
// So: the four keys' entries point at text of ours (a fresh guest allocation each time,
// never written again: the game may free an entry's text when a downloaded table
// replaces it); AuthenticateGamer('Upload') answers authenticated straight away;
// UploadPhoto writes the occupied slots' JPEGs and posts uploadAlbumStart, then the
// result a little later through the navigator's delayed events, so the game's own
// dialogs say "exporting", then where the photos went or that it failed.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include "hooks.h"
#include "hooks_internal.h"
#include "mc_engine/core/logging.h"

REXCVAR_DEFINE_BOOL(photo_album_export, true, "MCLA/PhotoMode",
    "The photo album's PUBLISH ALBUM button becomes EXPORT ALBUM: it saves every photo "
    "in the album as a JPEG in photo_album_export_dir instead of uploading the album to "
    "the Social Club (closed). Photos already in the folder are not written twice.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(photo_album_export_dir, "", "MCLA/PhotoMode",
    "Folder EXPORT ALBUM writes the photos to. Empty = Pictures\\LARecomp.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_82289AB0);  // net.AuthenticateGamer(netui, kind)
REX_EXTERN(__imp__rex_sub_821FA230);  // mcUIManager::FindElement(name)
REX_EXTERN(__imp__rex_sub_823DEDD8);  // RTTI cast (__RTDynamicCast)
REX_EXTERN(__imp__rex_sub_821E6800);  // play a UI sound by name
REX_EXTERN(__imp__rex_sub_82130528);  // guest operator new
REX_EXTERN(__imp__rex_sub_8268EE10);  // post an event (obj, idx, 3, -1)
REX_EXTERN(__imp__rex_sub_8268D6F8);  // the machine's index of an event object
REX_EXTERN(__imp__rex_sub_8268ECE0);  // throwDelayedEvent(machine, packed, delay)

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kStringTableMgr = 0x8286D7FC;
constexpr uint32_t kMgrTable       = 4;
constexpr uint32_t kMgrLanguage    = 0x1C;
constexpr uint32_t kEntryText      = 8;
constexpr size_t   kMaxTextBytes   = 127;  // the display cache's 132-byte buffers
constexpr uint32_t kTextAlloc      = 132;

constexpr uint32_t kUiManagerPtr   = 0x8286D804;
constexpr uint32_t kMgrNavigator   = 52;

constexpr uint32_t kPhotoAlbumObj   = 0x82826134;  // the "photoAlbum" event object
constexpr uint32_t kCmdUploadPhoto  = 14;
constexpr uint32_t kEvUploadAlbumStart   = 16;
constexpr uint32_t kEvUploadAlbumSuccess = 17;
constexpr uint32_t kEvUploadPhotoFail    = 15;
constexpr uint32_t kNetEvAuthenticated   = 8;
constexpr double   kResultDelaySeconds   = 0.6;

constexpr uint32_t kStrPhotoAlbumScene = 0x8201CDB4;  // "PhotoAlbumScene"
constexpr uint32_t kRttiVhsmState      = 0x827DA9A4;
constexpr uint32_t kRttiUiPhotoAlbum   = 0x827DA9EC;
constexpr uint32_t kStrUploadSound     = 0x82090B44;  // "UI_PHOTO_UPLOAD_MASTER"

constexpr uint32_t kAlbumSaveBuffer = 428;
constexpr uint32_t kAlbumCount      = 856;
constexpr uint32_t kAlbumMask       = 860;
constexpr uint32_t kAlbumServerMode = 1225;
constexpr uint32_t kSaveSlotTable   = 12;   // sixteen {slot, size, id}
constexpr uint32_t kSaveFirstJpeg   = 204;
constexpr uint32_t kSaveSlotBytes   = 204800;
constexpr int      kSlots           = 16;

uint32_t R32(const uint8_t* base, uint32_t ea) { return ea ? ReadGuestU32(base, ea) : 0; }

// rage::atStringHash over the lower-cased key -- the hash the .strtbl stores.
uint32_t KeyHash(std::string_view s) {
    uint32_t h = 0;
    for (char ch : s) {
        uint8_t c = static_cast<uint8_t>(ch);
        if (c >= 'A' && c <= 'Z') c = uint8_t(c + 32);
        h += c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

enum Key { kKeyPublish, kKeyWait, kKeyDone, kKeyFail, kKeyCount };
constexpr const char* kKeyNames[kKeyCount] = {
    "PA_publish", "Online_Upload_Photo", "Online_Upload_Photo_Success",
    "Online_Upload_Photo_Error_Retry"};

// UTF-8, written with escapes so the source charset does not matter. {n} is the photo
// count, {dir} the folder.
struct LangText {
    const char* publish;
    const char* wait;
    const char* done_one;
    const char* done_many;
    const char* fail;
};
constexpr LangText kText[] = {
    // en
    {"EXPORT ALBUM", "Exporting your photo album...", "1 photo exported to {dir}",
     "{n} photos exported to {dir}",
     "The photo album could not be exported to {dir}. Would you like to retry?"},
    // es
    {"EXPORTAR \xC3\x81" "LBUM", "Exportando tu \xC3\xA1" "lbum de fotos...",
     "1 foto exportada a {dir}", "{n} fotos exportadas a {dir}",
     "No se pudo exportar el \xC3\xA1" "lbum de fotos a {dir}. \xC2\xBF" "Quieres volver a intentarlo?"},
    // fr
    {"EXPORTER L'ALBUM", "Exportation de l'album photo...",
     "1 photo export\xC3\xA9" "e vers {dir}", "{n} photos export\xC3\xA9" "es vers {dir}",
     "Impossible d'exporter l'album photo vers {dir}. Voulez-vous r\xC3\xA9" "essayer ?"},
    // de
    {"ALBUM EXPORTIEREN", "Fotoalbum wird exportiert...", "1 Foto exportiert nach {dir}",
     "{n} Fotos exportiert nach {dir}",
     "Das Fotoalbum konnte nicht nach {dir} exportiert werden. Erneut versuchen?"},
    // it
    {"ESPORTA ALBUM", "Esportazione dell'album fotografico...", "1 foto esportata in {dir}",
     "{n} foto esportate in {dir}",
     "Impossibile esportare l'album fotografico in {dir}. Riprovare?"},
    // pt
    {"EXPORTAR \xC3\x81" "LBUM", "Exportando seu \xC3\xA1" "lbum de fotos...",
     "1 foto exportada para {dir}", "{n} fotos exportadas para {dir}",
     "N\xC3\xA3" "o foi poss\xC3\xAD" "vel exportar o \xC3\xA1" "lbum de fotos para {dir}. "
     "Deseja tentar de novo?"},
};

const LangText& TextFor(const uint8_t* base) {
    const uint32_t mgr = R32(base, kStringTableMgr);
    const uint32_t lang = mgr ? R32(base, mgr + kMgrLanguage) : 0;
    return kText[lang < std::size(kText) ? lang : 0];
}

std::string Replace(std::string s, std::string_view what, std::string_view with) {
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
    return s;
}

// Cuts at a character boundary.
std::string ClampUtf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

// A message with {dir}, shortened from the left of the folder ("...\LARecomp") when the
// whole thing would not fit the display buffer.
std::string WithDir(const std::string& pattern, const std::string& dir) {
    std::string out = Replace(pattern, "{dir}", dir);
    if (out.size() <= kMaxTextBytes) return out;
    const size_t fixed = out.size() - dir.size();
    const size_t room = kMaxTextBytes > fixed ? kMaxTextBytes - fixed : 0;
    std::string tail;
    if (room > 4) {
        tail = dir.substr(dir.size() - (room - 3));
        const size_t sep = tail.find_first_of("\\/");
        if (sep != std::string::npos && sep + 1 < tail.size()) tail = tail.substr(sep);
        while (!tail.empty() && (static_cast<uint8_t>(tail[0]) & 0xC0) == 0x80) tail.erase(0, 1);
        tail = "..." + tail;
    }
    return ClampUtf8(Replace(pattern, "{dir}", tail), kMaxTextBytes);
}

// The string table's entry for a key, 0 when the loaded table has none.
uint32_t TableEntry(const uint8_t* base, uint32_t hash, uint32_t* map_out) {
    const uint32_t mgr = R32(base, kStringTableMgr);
    const uint32_t map = mgr ? R32(base, mgr + kMgrTable) : 0;
    if (map_out) *map_out = map;
    if (!map) return 0;
    const uint32_t buckets = R32(base, map + 0);
    const uint32_t array = R32(base, map + 4);
    if (!buckets || !array) return 0;
    uint32_t node = R32(base, array + 4 * (hash % buckets));
    for (int guard = 0; node && guard < 4096; ++guard, node = R32(base, node + 8)) {
        if (R32(base, node) == hash) return R32(base, node + 4);
    }
    return 0;
}

// The table's text is UTF-8 as far as the loader reads (sub_821E4958); a table whose
// accented text is not valid UTF-8 is taken as Latin-1 and gets ours in Latin-1 too.
enum class Charset { kUnknown, kUtf8, kLatin1 };
Charset g_charset = Charset::kUnknown;

Charset CharsetOf(const uint8_t* s, size_t max) {
    bool high = false;
    for (size_t i = 0; i < max && s[i]; ++i) {
        const uint8_t c = s[i];
        if (c < 0x80) continue;
        high = true;
        size_t extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (!extra) return Charset::kLatin1;
        for (size_t k = 1; k <= extra; ++k)
            if (i + k >= max || (s[i + k] & 0xC0) != 0x80) return Charset::kLatin1;
        i += extra;
    }
    return high ? Charset::kUtf8 : Charset::kUnknown;
}

std::string Utf8ToLatin1(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) out.push_back(char(c));
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            out.push_back(char(((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F)));
            ++i;
        } else {
            out.push_back('?');
            while (i + 1 < s.size() && (static_cast<uint8_t>(s[i + 1]) & 0xC0) == 0x80) ++i;
        }
    }
    return out;
}

struct Override {
    uint32_t map = 0;
    uint32_t entry = 0;
    uint32_t original = 0;  // the game's text pointer
    uint32_t mine = 0;      // ours, while the entry still points at it
    std::string text;
};
std::array<Override, kKeyCount> g_override;

// Points the key's entry at `text`, unless it already shows exactly that.
void SetKeyText(PPCContext& ctx, uint8_t* base, Key key, const std::string& text_in) {
    Override& o = g_override[key];
    uint32_t map = 0;
    const uint32_t entry = TableEntry(base, KeyHash(kKeyNames[key]), &map);
    if (!entry) return;
    const uint32_t current = R32(base, entry + kEntryText);
    const bool ours = o.mine && o.map == map && o.entry == entry && current == o.mine;
    if (!ours) {
        // A new table (language switch, downloaded content) or a first visit: what the
        // entry points at now is the game's text.
        o.map = map;
        o.entry = entry;
        o.original = current;
        if (current) {
            const Charset cs = CharsetOf(base + current, 512);
            if (cs != Charset::kUnknown && cs != g_charset) {
                g_charset = cs;
                MC_INFO("[photo-export] string table text is {} (from {})",
                        cs == Charset::kUtf8 ? "UTF-8" : "Latin-1", kKeyNames[key]);
            }
        }
    }
    std::string text = ClampUtf8(text_in, kMaxTextBytes);
    if (g_charset == Charset::kLatin1) text = Utf8ToLatin1(text);
    if (ours && o.text == text) return;

    ctx.r3.u64 = kTextAlloc;
    __imp__rex_sub_82130528(ctx, base);
    const uint32_t buf = ctx.r3.u32;
    if (!buf) return;
    std::memcpy(base + buf, text.data(), text.size());
    base[buf + text.size()] = 0;
    WriteGuestU32(base, entry + kEntryText, buf);
    o.mine = buf;
    o.text = text;
}

// Option turned off: the entries we changed get the game's text back.
void RestoreKeyTexts(uint8_t* base) {
    for (int k = 0; k < kKeyCount; ++k) {
        Override& o = g_override[k];
        if (!o.mine) continue;
        uint32_t map = 0;
        const uint32_t entry = TableEntry(base, KeyHash(kKeyNames[k]), &map);
        if (entry && entry == o.entry && map == o.map &&
            R32(base, entry + kEntryText) == o.mine && o.original)
            WriteGuestU32(base, entry + kEntryText, o.original);
        o = Override{};
    }
}

void ApplyButtonTexts(PPCContext& ctx, uint8_t* base) {
    const LangText& t = TextFor(base);
    SetKeyText(ctx, base, kKeyPublish, t.publish);
    SetKeyText(ctx, base, kKeyWait, t.wait);
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

std::string PathUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

fs::path ExportDir() {
    const std::string configured = REXCVAR_GET(photo_album_export_dir);
    if (!configured.empty()) return PathFromUtf8(configured);
#ifdef _WIN32
    // FOLDERID_Pictures, spelled out so the GUID needs no import library.
    static const GUID kFolderPictures = {
        0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};
    PWSTR pictures = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(kFolderPictures, 0, nullptr, &pictures)) && pictures) {
        fs::path p = fs::path(pictures) / "LARecomp";
        CoTaskMemFree(pictures);
        return p;
    }
    if (pictures) CoTaskMemFree(pictures);
#endif
    auto* rt = rex::Runtime::instance();
    fs::path root = rt ? rt->user_data_root() : fs::path{};
    return (root.empty() ? fs::path(".") : root) / "photos";
}

// Length of the JPEG at p: the album's own size field when it frames a whole JPEG,
// otherwise up to the EOI found by walking the segments. 0 when it is not a JPEG.
size_t JpegLength(const uint8_t* p, size_t max, uint32_t stated) {
    if (max < 4 || p[0] != 0xFF || p[1] != 0xD8) return 0;
    if (stated >= 4 && stated <= max && p[stated - 2] == 0xFF && p[stated - 1] == 0xD9)
        return stated;
    size_t i = 2;
    while (i + 4 <= max) {
        if (p[i] != 0xFF) return 0;
        const uint8_t m = p[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0xD9) return i + 2;
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        const size_t len = (size_t(p[i + 2]) << 8) | p[i + 3];
        if (len < 2) return 0;
        i += 2 + len;
        if (m != 0xDA) continue;
        // Entropy-coded data: runs to the next marker that is not a stuffed 0xFF00 or
        // a restart marker.
        while (i + 1 < max) {
            if (p[i] == 0xFF) {
                const uint8_t n = p[i + 1];
                if (n == 0x00 || (n >= 0xD0 && n <= 0xD7)) { i += 2; continue; }
                if (n == 0xFF) { ++i; continue; }
                break;
            }
            ++i;
        }
    }
    return 0;
}

uint32_t Fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

struct ExportResult {
    bool ok = false;
    int photos = 0;    // in the folder now, written or already there
    int written = 0;
    std::string dir;
};

ExportResult WriteAlbum(const uint8_t* base, uint32_t album) {
    ExportResult r;
    const fs::path dir = ExportDir();
    r.dir = PathUtf8(dir.lexically_normal().make_preferred());
    const uint32_t save = R32(base, album + kAlbumSaveBuffer);
    const uint32_t mask = R32(base, album + kAlbumMask);
    if (!save) {
        MC_WARN("[photo-export] the album has no save buffer");
        return r;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        MC_WARN("[photo-export] cannot create {}: {}", r.dir, ec.message());
        return r;
    }

    bool failed = false;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (!(mask & (1u << slot))) continue;
        const uint8_t* jpeg = base + save + kSaveFirstJpeg + kSaveSlotBytes * uint32_t(slot);
        const uint32_t stated = R32(base, save + kSaveSlotTable + 12 * uint32_t(slot) + 4);
        const size_t len = JpegLength(jpeg, kSaveSlotBytes, stated);
        if (!len) {
            MC_WARN("[photo-export] slot {} holds no JPEG, skipped", slot);
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "MCLA_%08X.jpg", Fnv1a(jpeg, len));
        const fs::path file = dir / name;
        if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) == len) {
            ++r.photos;
            continue;
        }
        const fs::path tmp = dir / (std::string(name) + ".part");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(jpeg), std::streamsize(len));
            out.close();
            if (!out) {
                MC_WARN("[photo-export] writing {} failed", PathUtf8(tmp));
                fs::remove(tmp, ec);
                failed = true;
                continue;
            }
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            MC_WARN("[photo-export] {} -> {}: {}", PathUtf8(tmp), name, ec.message());
            fs::remove(tmp, ec);
            failed = true;
            continue;
        }
        ++r.photos;
        ++r.written;
    }
    r.ok = !failed && r.photos > 0;
    MC_INFO("[photo-export] {} photo(s) in {} ({} new){}", r.photos, r.dir, r.written,
            failed ? ", some could not be written" : "");
    return r;
}

// The album behind PhotoAlbumScene, the way sub_8263FA10 finds it.
uint32_t FindAlbum(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = kStrPhotoAlbumScene;
    __imp__rex_sub_821FA230(ctx, base);
    const uint32_t element = ctx.r3.u32;
    if (!element) return 0;
    ctx.r3.u64 = element;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = kRttiVhsmState;
    ctx.r6.u64 = kRttiUiPhotoAlbum;
    ctx.r7.u64 = 0;
    __imp__rex_sub_823DEDD8(ctx, base);
    return ctx.r3.u32;
}

void PostAlbumEvent(PPCContext& ctx, uint8_t* base, uint32_t event) {
    ctx.r3.u64 = kPhotoAlbumObj;
    ctx.r4.u64 = event;
    ctx.r5.u64 = 3;
    ctx.r6.s64 = -1;
    __imp__rex_sub_8268EE10(ctx, base);
}

// The event goes on the navigator's delayed list, as sub_82216168 puts one there --
// except for its last argument: sub_82690C18 holds an entry with that byte set while
// the game is paused (dword_828CCEA0 +4), and the album pauses the game. With 0 the
// delay runs on the UI's clock.
bool PostAlbumEventLater(PPCContext& ctx, uint8_t* base, uint32_t event, double seconds) {
    const uint32_t mgr = R32(base, kUiManagerPtr);
    const uint32_t nav = mgr ? R32(base, mgr + kMgrNavigator) : 0;
    if (!nav) return false;
    const uint32_t machine = nav + 4;
    ctx.r3.u64 = machine;
    ctx.r4.u64 = kPhotoAlbumObj;
    __imp__rex_sub_8268D6F8(ctx, base);
    const uint32_t object_index = ctx.r3.u32 & 0xFFFFu;
    ctx.r3.u64 = machine;
    ctx.r4.u64 = (object_index << 16) | (event & 0xFFFFu);
    ctx.f1.f64 = seconds;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0;
    __imp__rex_sub_8268ECE0(ctx, base);
    return true;
}

void ExportAlbum(PPCContext& ctx, uint8_t* base) {
    const uint32_t album = FindAlbum(ctx, base);
    if (!album) return;
    // The game's own conditions, minus the LIVE profile.
    if (base[album + kAlbumServerMode] ||
        static_cast<int32_t>(R32(base, album + kAlbumCount)) <= 0)
        return;

    ctx.r3.u64 = kStrUploadSound;
    __imp__rex_sub_821E6800(ctx, base);

    const ExportResult r = WriteAlbum(base, album);
    const LangText& t = TextFor(base);
    if (r.ok) {
        const std::string pattern =
            Replace(r.photos == 1 ? t.done_one : t.done_many, "{n}", std::to_string(r.photos));
        SetKeyText(ctx, base, kKeyDone, WithDir(pattern, r.dir));
    } else {
        SetKeyText(ctx, base, kKeyFail, WithDir(t.fail, r.dir));
    }

    PostAlbumEvent(ctx, base, kEvUploadAlbumStart);
    const uint32_t result = r.ok ? kEvUploadAlbumSuccess : kEvUploadPhotoFail;
    if (!PostAlbumEventLater(ctx, base, result, kResultDelaySeconds))
        PostAlbumEvent(ctx, base, result);
}

bool GuestStrIEquals(const uint8_t* base, uint32_t ea, std::string_view want) {
    if (ea < 0x10000u) return false;
    const char* s = reinterpret_cast<const char*>(base + ea);
    for (size_t i = 0; i < want.size(); ++i) {
        char a = s[i], b = want[i];
        if (!a) return false;
        if (a >= 'A' && a <= 'Z') a = char(a + 32);
        if (b >= 'A' && b <= 'Z') b = char(b + 32);
        if (a != b) return false;
    }
    return s[want.size()] == 0;
}

}  // namespace

bool PhotoAlbumExportCommand(PPCContext& ctx, uint8_t* base, uint32_t cmd) {
    if (!REXCVAR_GET(photo_album_export)) {
        RestoreKeyTexts(base);
        return false;
    }
    ApplyButtonTexts(ctx, base);
    if (cmd != kCmdUploadPhoto) return false;
    ExportAlbum(ctx, base);
    ctx.r3.u64 = 0;
    return true;
}

// net.AuthenticateGamer: the album asks for 'Upload' before UploadPhoto. An export needs
// no LIVE profile, so that one is answered at once, the way the game answers success.
extern "C" REX_FUNC(rex_sub_82289AB0) {
    if (REXCVAR_GET(photo_album_export) && GuestStrIEquals(base, ctx.r4.u32, "Upload")) {
        ctx.r4.u64 = kNetEvAuthenticated;
        ctx.r5.u64 = 3;
        ctx.r6.s64 = -1;
        __imp__rex_sub_8268EE10(ctx, base);  // r3 is still the mcNetUI object
        ctx.r3.u64 = 1;
        return;
    }
    __imp__rex_sub_82289AB0(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
