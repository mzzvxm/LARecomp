// UI theme: recolors the Flash menus.
//
// Every colored thing the game's Flash player (rage::swf*) draws -- vector
// fills, line strokes, static text runs, dynamic text -- ends in one of five
// instructions, `stw r11, dword_828396F4@l(rX)`, that publish the final ARGB
// (fill style color x the instance color transform, already clamped) as the
// immediate-mode vertex color the draw that follows consumes:
//
//   0x82603E18  swfTEXT::Draw (sub_82603BB8)   run color before the first record
//   0x82604020  swfTEXT::Draw                  per-record color (case 2)
//   0x8260474C  swfEDITTEXT draw (sub_82604548) dynamic text, inst +68..70
//   0x82607C88  sub_82607AF8                   shape fill vertex color
//   0x82607E28  sub_82607CA0                   shape line color
//
// The hooks sit BEFORE the store (the default for a midasm_hook), so editing r11
// edits what the draw sees. dword_828396F4 itself is the global current-vertex
// color of ALL immediate-mode drawing (146 xrefs), which is why nothing hooks the
// global or its consumers -- only these swf store sites.
//
// A movie is told apart by wrapping sub_82720908, the 2D draw phase of the per-
// phase dispatcher sub_82721258 (r3 = the mcUIManager movie entry, whose name is
// the key of the manager's movie map, "HUDMOVIE" etc.); it ends in the swf
// context's vtable[2] draw. sub_827250A8 / sub_82724CF0 are the 3D phases
// ("dyn3ds"/"scenegraph" nodes, pass name "world") -- NOT the swf draw, and the
// likeliest home of the menu's glass frame.
//
// ui_color_log writes the colors it sees so the families below can be measured
// instead of guessed. The theme cvars are empty (off) by default.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hooks.h"
#include "hooks_internal.h"
#include "../logging.h"
#include "../swf_dom.h"

REXCVAR_DEFINE_STRING(ui_theme_accent, "", "MCLA/UI",
    "Replaces the blue/teal that every menu uses (selection bar, highlights, "
    "arrows) with this color, as RRGGBB hex. Empty keeps the game's own. "
    "Shades of the original keep their brightness relative to it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(ui_theme_title, "", "MCLA/UI",
    "Replaces the yellow of the menu titles and the spinning globe, as RRGGBB "
    "hex. Empty keeps the game's own.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(ui_theme_frame, "", "MCLA/UI",
    "Replaces the gray of the menu frame, as RRGGBB hex. Empty keeps the "
    "game's own.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(ui_theme_skip_movies, "HUDMOVIE", "MCLA/UI",
    "Comma-separated Flash movie names the theme leaves alone. The HUD movie "
    "shares the menus' teal and yellow in its speedometer; empty themes every "
    "movie.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(ui_color_log, false, "MCLA/Diagnostics",
    "Log every distinct Flash UI color (movie, draw site, ARGB, bound texture) "
    "with a running count, to measure the families ui_theme_* recolors.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_82720908);  // 2D draw phase of one movie

namespace {

enum Site : uint8_t { kTextInit, kTextRun, kEdit, kShape, kLine, kSiteCount };
constexpr const char* kSiteName[kSiteCount] = {"text0", "text", "edit", "fill", "line"};

constexpr uint32_t kCurTexture = 0x82839710;  // set by sub_8217D9A0
constexpr uint32_t kDefTexture = 0x82839CF0;  // what 0 means there

struct Family {
    bool on = false;
    float r = 0, g = 0, b = 0;  // target, 0..1
};

struct Config {
    uint64_t frame = ~0ull;
    uint32_t epoch = 0;
    Family accent, title, frame_;
    std::string skip;
    bool log = false;
};

thread_local Config t_cfg;
thread_local char t_movie[48] = "?";
thread_local bool t_skip = false;
// One-entry memo: consecutive vertices of a run carry the same color.
thread_local uint32_t t_memo_in = 0, t_memo_out = 0, t_memo_epoch = ~0u;

bool ParseHex(const std::string& s, Family& f) {
    f = Family{};
    size_t i = 0;
    if (i < s.size() && s[i] == '#') ++i;
    if (s.size() - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    if (s.size() - i != 6) return false;
    uint32_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    f.on = true;
    f.r = ((v >> 16) & 0xFF) / 255.0f;
    f.g = ((v >> 8) & 0xFF) / 255.0f;
    f.b = (v & 0xFF) / 255.0f;
    return true;
}

void RefreshConfig() {
    const uint64_t frame = g_frame_heartbeat.load(std::memory_order_relaxed);
    if (frame == t_cfg.frame) return;
    t_cfg.frame = frame;
    Config n;
    n.frame = frame;
    ParseHex(REXCVAR_GET(ui_theme_accent), n.accent);
    ParseHex(REXCVAR_GET(ui_theme_title), n.title);
    ParseHex(REXCVAR_GET(ui_theme_frame), n.frame_);
    n.skip = REXCVAR_GET(ui_theme_skip_movies);
    n.log = REXCVAR_GET(ui_color_log);
    const bool same = n.accent.on == t_cfg.accent.on && n.accent.r == t_cfg.accent.r &&
                      n.accent.g == t_cfg.accent.g && n.accent.b == t_cfg.accent.b &&
                      n.title.on == t_cfg.title.on && n.title.r == t_cfg.title.r &&
                      n.title.g == t_cfg.title.g && n.title.b == t_cfg.title.b &&
                      n.frame_.on == t_cfg.frame_.on && n.frame_.r == t_cfg.frame_.r &&
                      n.frame_.g == t_cfg.frame_.g && n.frame_.b == t_cfg.frame_.b;
    n.epoch = t_cfg.epoch + (same ? 0 : 1);
    t_cfg = std::move(n);
}

bool InList(const std::string& list, const char* name) {
    size_t pos = 0;
    const size_t len = std::strlen(name);
    while (pos <= list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        size_t a = pos, b = end;
        while (a < b && list[a] == ' ') ++a;
        while (b > a && list[b - 1] == ' ') --b;
        if (b - a == len && std::memcmp(list.data() + a, name, len) == 0) return true;
        pos = end + 1;
    }
    return false;
}

// ── color families ────────────────────────────────────────────────────────
// Bands are in HSV (hue in degrees) and are the first guess from the screenshot
// colors; ui_color_log exists to replace them with measured ones. `ref_v` is the
// brightness (V) of the family's own nominal color: a shade at ref_v maps to the
// target exactly, a darker one to a proportionally darker target.
struct Band {
    float h0, h1;     // hue window, degrees
    float s0, s1;     // saturation window
    float v0, v1;     // value window
    float ref_v;
};
// Measured 20/10/2026 (ui_color_log, pause + Music Playlists):
//   accent: selected text 03F9FA, bar 02B7B7, dark bar 142828, blue edit 0099FF,
//           1BEBF7 / 2BE7FF fills                       -> hue 180..205, V .16..1
//   title:  title text F4CC04, globe/glow fills F3CB03 + FCCC30 (textured, white
//           x vertex color), FFCB00                      -> hue 48
//   frame:  CCCCCC solid fill (the game's `colorWhite`); the texts DDDDDD / AAAAAA /
//           888888 sit right beside it in V, so the window is narrow. Unconfirmed
//           that CCCCCC is the visible rim.
constexpr Band kAccentBand = {150.f, 225.f, 0.30f, 1.00f, 0.10f, 1.00f, 0.98f};
constexpr Band kTitleBand  = { 38.f,  70.f, 0.45f, 1.00f, 0.10f, 1.00f, 0.96f};
constexpr Band kFrameBand  = {  0.f, 360.f, 0.00f, 0.02f, 0.78f, 0.83f, 0.80f};

struct Hsv { float h, s, v; };

Hsv ToHsv(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float d = mx - mn;
    float h = 0.f;
    if (d > 1e-6f) {
        if (mx == r) h = 60.f * std::fmod((g - b) / d, 6.f);
        else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
        else h = 60.f * ((r - g) / d + 4.f);
        if (h < 0.f) h += 360.f;
    }
    return {h, mx > 1e-6f ? d / mx : 0.f, mx};
}

bool InBand(const Band& bd, const Hsv& c) {
    return c.h >= bd.h0 && c.h <= bd.h1 && c.s >= bd.s0 && c.s <= bd.s1 && c.v >= bd.v0 &&
           c.v <= bd.v1;
}

uint32_t Tint(uint32_t argb, const Family& f, const Band& bd, const Hsv& c) {
    const float k = c.v / bd.ref_v;
    float r = f.r * k, g = f.g * k, b = f.b * k;
    const float mx = std::max({r, g, b});
    if (mx > 1.f) { r /= mx; g /= mx; b /= mx; }
    auto q = [](float x) { return uint32_t(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
    return (argb & 0xFF000000u) | (q(r) << 16) | (q(g) << 8) | q(b);
}

uint32_t Remap(uint32_t argb) {
    if (t_skip) return argb;
    const Config& c = t_cfg;
    if (!c.accent.on && !c.title.on && !c.frame_.on) return argb;
    if (t_memo_epoch == c.epoch && t_memo_in == argb) return t_memo_out;
    const Hsv h = ToHsv(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f,
                        (argb & 0xFF) / 255.f);
    uint32_t out = argb;
    if (c.title.on && InBand(kTitleBand, h)) out = Tint(argb, c.title, kTitleBand, h);
    else if (c.accent.on && InBand(kAccentBand, h)) out = Tint(argb, c.accent, kAccentBand, h);
    else if (c.frame_.on && InBand(kFrameBand, h)) out = Tint(argb, c.frame_, kFrameBand, h);
    t_memo_epoch = c.epoch;
    t_memo_in = argb;
    t_memo_out = out;
    return out;
}

// ── movie names ───────────────────────────────────────────────────────────
// mcUIManager (*0x8286D804) +60 is the movie map: buckets* @+60, u16 count @+64;
// node +0 char* key, +4 u16 key length, +8 movie**, +12 next (see ui.cpp).
constexpr uint32_t kUiManagerPtr = 0x8286D804;
constexpr uint32_t kMgrMovieMap = 60;

uint16_t ReadGuestU16(const uint8_t* base, uint32_t ea) {
    return ea ? uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]) : 0;
}

thread_local std::unordered_map<uint32_t, std::string> t_names;
thread_local uint64_t t_names_frame = ~0ull;

std::string GuestText(const uint8_t* base, uint32_t ea, size_t max = 40);

const std::string* MovieName(const uint8_t* base, uint32_t entry) {
    const uint64_t frame = g_frame_heartbeat.load(std::memory_order_relaxed);
    auto find = [&]() -> const std::string* {
        auto it = t_names.find(entry);
        return it == t_names.end() ? nullptr : &it->second;
    };
    if (frame == t_names_frame) {
        if (const std::string* n = find()) return n;
        // A movie that appeared mid-frame: fall through to a rescan.
    }
    t_names_frame = frame;
    t_names.clear();
    const uint32_t mgr = ReadGuestU32(base, kUiManagerPtr);
    if (!mgr) return nullptr;
    const uint32_t buckets = ReadGuestU32(base, mgr + kMgrMovieMap);
    const uint16_t count = ReadGuestU16(base, mgr + kMgrMovieMap + 4);
    for (uint32_t i = 0; buckets && i < count; ++i) {
        for (uint32_t node = ReadGuestU32(base, buckets + 4 * i), guard = 0; node && guard < 256;
             node = ReadGuestU32(base, node + 12), ++guard) {
            if (!ReadGuestU16(base, node + 4)) continue;
            const uint32_t holder = ReadGuestU32(base, node + 8);
            const uint32_t e = holder ? ReadGuestU32(base, holder) : 0;
            std::string name = GuestText(base, ReadGuestU32(base, node), 47);
            if (e && !name.empty()) t_names[e] = std::move(name);
        }
    }
    return find();
}

// ── measurement ───────────────────────────────────────────────────────────
struct Seen {
    uint64_t n = 0;
    uint8_t amin = 255, amax = 0;
    std::string note;
};
constexpr size_t kMaxSeen = 2000;
uint32_t g_note_calls = 0;
std::mutex g_log_mutex;
std::unordered_map<std::string, Seen> g_seen;
int64_t g_next_dump_ns = 0;

std::string GuestText(const uint8_t* base, uint32_t ea, size_t max) {
    if (ea < 0x10000u || ea >= 0xC0000000u) return {};
    const char* s = reinterpret_cast<const char*>(base + ea);
    size_t n = 0;
    while (n < max && s[n]) {
        const unsigned char ch = static_cast<unsigned char>(s[n]);
        if (ch < 0x20 || ch > 0x7E) return {};
        ++n;
    }
    return n < max ? std::string(s, n) : std::string{};
}

const uint8_t* GuestBase() {
    static const uint8_t* base = nullptr;
    if (!base) base = rex::Runtime::instance()->virtual_membase();
    return base;
}

void DumpSeen(int64_t now_ns);

void NoteColor(Site site, uint32_t argb) {
    const uint8_t* base = GuestBase();
    const uint32_t tex = ReadGuestU32(base, kCurTexture);
    const uint32_t def = ReadGuestU32(base, kDefTexture);
    const uint8_t alpha = uint8_t(argb >> 24);
    char key[96];
    std::snprintf(key, sizeof(key), "%s|%s|%06X|%08X", t_movie, kSiteName[site], argb & 0xFFFFFFu,
                  tex == def ? 0u : tex);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    // Fill fires per vertex: look at the clock only now and then.
    if ((++g_note_calls & 0x3FFF) == 0)
        DumpSeen(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count());
    auto it = g_seen.find(key);
    const bool fresh = it == g_seen.end();
    if (fresh) {
        if (g_seen.size() >= kMaxSeen) return;
        it = g_seen.try_emplace(key).first;
    }
    Seen& e = it->second;
    ++e.n;
    e.amin = std::min(e.amin, alpha);
    e.amax = std::max(e.amax, alpha);
    if (!fresh) return;
    std::string note = tex == def ? "untextured" : "tex";
    if (tex != def) {
        // grcTexture keeps its name at +0x18, as a pointer.
        const std::string name = GuestText(base, ReadGuestU32(base, tex + 0x18));
        char buf[24];
        std::snprintf(buf, sizeof(buf), " %08X", tex);
        note += buf;
        if (!name.empty()) note += " '" + name + "'";
    }
    e.note = note;
    MC_INFO("[uicolor] new movie={} site={} argb={:08X} {}", t_movie, kSiteName[site], argb, note);
}

// Caller holds g_log_mutex.
void DumpSeen(int64_t now_ns) {
    if (now_ns < g_next_dump_ns) return;
    g_next_dump_ns = now_ns + 8'000'000'000ll;
    std::vector<std::pair<const std::string*, const Seen*>> rows;
    rows.reserve(g_seen.size());
    for (const auto& kv : g_seen) rows.emplace_back(&kv.first, &kv.second);
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second->n > b.second->n; });
    MC_INFO("[uicolor] ---- {} distinct (movie|site|RGB|tex) ----", rows.size());
    for (size_t i = 0; i < rows.size() && i < 80; ++i)
        MC_INFO("[uicolor] {:>9} a={:02X}-{:02X} {} {}", rows[i].second->n, rows[i].second->amin,
                rows[i].second->amax, *rows[i].first, rows[i].second->note);
}

void Apply(Site site, PPCRegister& r11) {
    // The UI OPTIONS swatches (ui_palette.cpp) show the colors the theme is
    // picked from; remapping them would show the theme's guess instead.
    if (larecomp::swf::DrawingRaw()) return;
    RefreshConfig();
    const uint32_t in = r11.u32;
    if (t_cfg.log) NoteColor(site, in);
    const uint32_t out = Remap(in);
    if (out != in) r11.u64 = out;
}

}  // namespace

// 2D draw phase of one movie: tags the swf draws inside it with the movie's name.
extern "C" REX_FUNC(rex_sub_82720908) {
    RefreshConfig();
    char prev[sizeof(t_movie)];
    std::memcpy(prev, t_movie, sizeof(prev));
    const bool prev_skip = t_skip;

    const std::string* name = MovieName(base, ctx.r3.u32);
    if (name) std::snprintf(t_movie, sizeof(t_movie), "%s", name->c_str());
    else std::snprintf(t_movie, sizeof(t_movie), "?%08X", ctx.r3.u32);
    t_skip = !t_cfg.skip.empty() && InList(t_cfg.skip, t_movie);
    t_memo_epoch = ~0u;

    __imp__rex_sub_82720908(ctx, base);

    std::memcpy(t_movie, prev, sizeof(prev));
    t_skip = prev_skip;
    t_memo_epoch = ~0u;
}

void Hook_UiColor_TextInit(PPCRegister& r11) { Apply(kTextInit, r11); }
void Hook_UiColor_TextRun(PPCRegister& r11) { Apply(kTextRun, r11); }
void Hook_UiColor_Edit(PPCRegister& r11) { Apply(kEdit, r11); }
void Hook_UiColor_Fill(PPCRegister& r11) { Apply(kShape, r11); }
void Hook_UiColor_Line(PPCRegister& r11) { Apply(kLine, r11); }

#endif  // REXGLUE_HAS_XEO3_TARGET
