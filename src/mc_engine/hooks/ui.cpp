// UI and HUD: intro skip, the movie lighting null check, ReXGlue settings from
// Game Options, speedometer units, Xbox / PlayStation button glyphs, the UI
// text language, and the two places the UI counts frames the 360 drew at 30 a
// second: the Flash movies' step and the menu cursor's auto-repeat.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hooks.h"
#include "hooks_internal.h"
#include "../graphics_button.h"
#include "../logging.h"
#include "../online/online_common.h"  // IsGuestPtr

REXCVAR_DEFINE_BOOL(skip_intro, false, "MCLA/Patches", "Skip the intro videos to prevent graphical issues.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(rexglue_settings_in_gameoptions, false, "MCLA/Settings",
    "Replace Game Options with ReXGlue Settings overlay (F4)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(speed_units, "kmh", "MCLA/HUD",
    "Speedometer / distance units. Drives the game's own metric formatter "
    "(sub_8238DDF0): game = console profile default (mph on NTSC), kmh = metric "
    "(km/h, km, m), mph = imperial (mph, miles, ft). Converts both the number and "
    "the unit label; the analog dial tick art stays as authored.\n"
    "kmh also converts the live HUD speedometer, which does NOT go through that "
    "formatter: the HUD movie multiplies m/s by 2.237 in its own ActionScript, so "
    "hud_units.cpp rewrites that constant (and the per-glyph unit label) in guest "
    "memory. The radar detector's speed-limit sign stays in mph -- its number comes "
    "from the guest and the game's metric branch for it is dead code.")
    .allowed({"game", "kmh", "mph"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(button_prompts, "xbox", "MCLA/UI",
    "On-screen button glyphs. xbox = A/B/X/Y + LB/RB/LT/RT, playstation = "
    "cross/circle/square/triangle + L1/R1/L2/R2. Both sets already ship inside "
    "the game's own UI movies and texture dictionary, so this only flips the "
    "'platform' flag their ActionScript reads - no asset is swapped or reloaded. "
    "Menus already on screen keep their old glyphs until reopened, and the boot "
    "legal screen switches to its PS3 text page.\n"
    "keyboard = key caps. The movies have no third glyph strip, so this stays on "
    "platform 0 and keyboard_native.cpp paints a key cap over the pixels of the "
    "Xbox A/B/X/Y/LB/RB/LT/RT bitmaps (the key bound to that menu button, kb_ui_*).")
    .allowed({"xbox", "playstation", "keyboard"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(language, "auto", "MCLA/Localization",
    "UI text language. MCLA shipped five full translations inside its own "
    "ui/mc4stringtable*.strtbl files (English, Spanish, French, German, "
    "Italian) but the retail NTSC/U build can only reach three of them: the "
    "language mapper (sub_82387928) sends German and Italian back to English "
    "whenever XGetGameRegion() reports the Americas. auto keeps that original "
    "behaviour; any other value overrides the index the string table loads.\n"
    "Japanese is not offered. Its strings, glyph pages and (given the retail "
    "jp_fonts folder) metrics all exist, but not one of the fourteen jp_*.xsf "
    "movies shipped on this disc -- only their .sharetex. sub_821FCB90 builds "
    "the jp_ name, the loader faults instead of returning 0, and the fallback "
    "to the latin movie right below it never runs. Measured: hard crash.\n"
    "pt is the empty eleventh slot every shipped .strtbl carries but never "
    "fills. It stays blank -- and therefore English -- unless a mod archive "
    "supplies files that fill it; tools/strtbl.py writes them.\n"
    "Switching in-game reloads the table live. Screens already drawn keep "
    "their old text until they are reopened -- the pause menu itself included, "
    "since the game resolves a row's label once when the row is built.")
    .allowed({"auto", "en", "es", "fr", "de", "it", "pt"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(ui_movie_pacing, "PHOTOALBUMMOVIE", "MCLA/UI",
    "Flash movies advanced at the console's 30 frames a second instead of one frame "
    "per rendered frame. The game steps every UI movie exactly one frame per update, "
    "which on the 360 was 30 times a second; above 30 FPS those movies play faster "
    "-- the photo album twice as fast at 60. Comma-separated movie names "
    "(PHOTOALBUMMOVIE, POPUPMOVIE, PAUSEMOVIE, GARAGEMOVIE, NAVSYSMOVIE, ...), '*' "
    "for every movie, empty for none. The HUD is two movies, HUDMAPMOVIE and "
    "HUDGAUGE (needles, song ticker, race countdown); it is left out by default: "
    "stepping 30 times a second it would visibly lag a 60 FPS world. Only movies "
    "the game steps one frame per update are paced.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(ui_countdown_fix, true, "MCLA/UI",
    "Keep the race countdown (3, 2, 1, GO) in time when the HUD's gauge movie steps "
    "less often than the game updates (HUDGAUGE or '*' in ui_movie_pacing). The "
    "movie's script adds the last update's time to the "
    "countdown's clock on every step, so with 30 steps a second at 120 FPS the "
    "countdown ran at a quarter of its speed and a race began on the 2 or the 3. "
    "On, the clock is given the time of all the updates since the movie's last step.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(ui_input_repeat_30hz, true, "MCLA/UI",
    "Menu cursor auto-repeat at the console's pace. The game repeats a held "
    "direction or button by counting frames -- the first repeat after 7, then one "
    "every 6 -- which on the 360 meant 233 ms and then 200 ms. Above 30 FPS it "
    "repeats faster with every frame: at 144 FPS a quick tap moves a list or the "
    "photo album's slot cursor two places, and holding it scrolls ~24 items a "
    "second. On, the count advances once every 1/30 s.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

bool SkipIntro() {
    return REXCVAR_GET(skip_intro);
}

// 0x821315E4 in sub_82131508, the branch taken when dword_82830B14 (the legals
// screen object) is null:
//
//     if (dword_82830B14) { ...ordered teardown... }
//     else                  sub_822E5B00(dword_8287E064, -1);
//
// sub_822E5B00 ORs its argument into *(mgr + 424), which sub_822E5B60 folds into
// *(mgr + 364) on the next frame. That word is the render pass bitmask that
// sub_822E6408 walks bit by bit (v12 = 1; ...; v12 *= 2; while v12 != 0x40000000),
// dispatching the 13 renderers of every set bit.
//
// Every normal caller ORs 0xFEFFFFFF instead: sub_821F9918 (0x821F99BC) and
// sub_821FCED8 (0x821FCF10). Bit 24 is deliberately excluded and the game never
// sets it anywhere else. This branch's -1 does set it, so skipping the intro
// enables a pass whose stage (mgr[24]) was never prepared, and it draws with
// uninitialised state - corrupt geometry and shading across the frame.
//
// Rewrite the argument to the same mask the rest of the engine uses. Applies to
// any way of reaching a null legals object, so it is independent of which
// SkipIntro hook is active.
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {
    if (REXCVAR_GET(skip_intro)) {
        r4.u32 = 0xFEFFFFFFu;
    }
}

// Intro/legals pacing is not patched here: see HoldUntimedSwap in
// frame_timing.cpp.
//
// The previous Hook_IntroHalfRate skipped every other SWF advance, which is
// only correct at exactly 60 FPS. With a configurable fps_limit (30 / 60 / 120 /
// 144 / uncapped) it is wrong at every other setting: at 120 it still runs 2x
// fast, at 30 it runs at half speed. It was removed from the midnightclub fork
// for the same reason and is not being reintroduced.
//
// Hard caps at 30, 45 and 60 gave the same intro speed in midnightclub because
// the legals/logo loop never runs the engine timer (sub_821BDA90), where the
// frame limiter lives: the cap never reached those frames at all. They are
// paced at the swap instead.

// 0x82725100, in sub_827250A8's per-movie lighting pass. The game asks the movie
// for its "lights" node and then uses the answer without checking it:
//
//   827250FC  bl sub_825ED480      r3 = the member, 0 when the movie has none
//   82725100  bl sub_825EF9F0      `return a1[2] == 5 ? *a1 : 0`  -- reads a1[2]
//   82725104  mr r26, r3
//   82725108  lwz r10, 0(r26)      -- and reads r26[0]
//
// Either read faults on a null. sub_825ED480 returns 0 while the movie's member
// table at +128 is still null, which is the state a movie is in before it has
// finished being built; on the 360 the lighting pass never runs that early, but
// under the recomp's thread timing it sometimes does. That is the intermittent
// "read of guest 0x00000008 in sub_825EF9F0" crash, and it is a null check the
// game simply does not have.
//
// So answer the question sub_825EF9F0 would have answered, and when the answer
// is null skip the block it feeds -- the hook jumps to 0x82725144, the `li r3, 1`
// that closes the scope. A movie with no lights node has nothing to light.
bool MCLA_UI_SkipMissingLights(PPCRegister& r3) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const uint32_t node = static_cast<uint32_t>(r3.u32);
    if (base && node) {
        const uint8_t* p = base + node + 8;
        const uint32_t kind =
            (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        if (kind == 5) return false;  // a real lights node: let the game run
    }

    static uint32_t skipped = 0;
    if (++skipped <= 4) {
        MC_WARN("[ui] movie has no lights node yet (r3={:#010x}), skipping the "
                "lighting pass this frame; the game would have dereferenced it",
                node);
    }
    return true;
}

// Speedometer / distance units. sub_8238DDF0 is the game's unit formatter; at
// 0x8238DE54 it loads the metric flag dword_8288E5A0 into r11, then r11==0
// selects imperial ("%.1fmph"/miles/ft), r11!=0 selects metric ("%.1fk/h" with
// value*1.609344 / km / m). We replace that load with our forced value so the
// game's own metric path renders km/h — number and label both. "game" leaves
// the console profile default in place (original lwz runs).
bool Patch_SpeedUnits(PPCRegister& r11) {
    std::string units = REXCVAR_GET(speed_units);
    if (units == "kmh") { r11.u64 = 1; return true; }
    if (units == "mph") { r11.u64 = 0; return true; }
    return false;  // "game": keep the profile/region default
}

// ── Button prompt glyphs (Xbox <-> PlayStation) ──────────────────────────
//
// Every MCLA UI movie ships BOTH glyph sets, and so does the texture dictionary
// the 360 build actually loads (resources/ui/shared_latin.xtd): shared_13 is the
// PS3 circle, shared_15 the square, shared_24 the cross and shared_36 the
// triangle, sitting right next to the 360 A/B/X/Y and LB/RB/LT/RT art. The
// ActionScript picks a strip with `icon.gotoAndStop(iconID + platform * 49)`
// (pause, popup, prompt, garage, navsys, raceeditor; legals uses platform * 39
// for its text pages). Native code only ever hands the movie an abstract iconID
// (sub_82637E68: 1 = accept, 2 = cancel), so nothing on the C++ side is bound to
// the 360 art — the whole switch is one flag.
//
// (The stray resources/ui/0x989F63FD.xtd is "shared.xtd", the PS3 build's own
// sprite pack. It is dead weight: the XEX only ever loads shared_latin/shared_jp
// (sub_821FE648), and its shared_N indices do not line up with the ones the .xsf
// files import by name, so swapping it in would scramble the UI. We don't.)
//
// `platform` is an mcRegistry int the movies read at frame 1 through the
// FSCommand getvar handler (sub_82721728 -> mcVariant::asInt sub_822031A8, which
// for type 3 returns *(entry + 88)). mcUIManager's ctor builds it with a
// hard-coded 0 (sub_821FDED8, 0x821FE204..0x821FE228) and sub_821F8038 re-pushes
// the same 0 into every movie as it is created. We own both: capture the entry
// as it is built and keep +88/+92 in sync with the cvar, and feed the push.
// 0 = Xbox 360, 1 = PS3.

// mcRegistry "platform" entry, captured by Hook_PlatformVarInit.
static std::atomic<uint32_t> g_platform_var_ea{0};
// Last value written, so the tick only touches guest memory on a real change.
static std::atomic<int> g_platform_applied{-1};

static int WantedPlatformValue() {
    std::string mode = REXCVAR_GET(button_prompts);
    return mode == "playstation" ? 1 : 0;
}

// +88 (0x58) is the int mcVariant::asInt returns for type 3, +92 (0x5C) the
// plain "%d" char buffer sub_823DC018 formatted into for the string readers.
static void WritePlatformVar(uint32_t entry, int value) {
    auto* rt = rex::Runtime::instance();
    uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base || !entry) return;

    base[entry + 88] = 0;
    base[entry + 89] = 0;
    base[entry + 90] = 0;
    base[entry + 91] = static_cast<uint8_t>(value & 0xFF);

    base[entry + 92] = static_cast<uint8_t>('0' + (value & 1));
    base[entry + 93] = 0;
}

// Called once per frame from Patch_DeltaTimePre so the cvar can be flipped live
// from the F4 menu. Movies already on screen keep the glyphs they resolved at
// their own frame 1; anything opened after this picks up the new set.
void TickButtonPrompts() {
    const uint32_t entry = g_platform_var_ea.load(std::memory_order_relaxed);
    if (!entry) return;

    const int want = WantedPlatformValue();
    if (want == g_platform_applied.load(std::memory_order_relaxed)) return;

    WritePlatformVar(entry, want);
    g_platform_applied.store(want, std::memory_order_relaxed);
    MC_INFO("[buttons] prompt glyphs -> {} (platform={})",
            want ? "PlayStation" : "Xbox 360", want);
}

// mcUIManager ctor, right after the "platform" mcVariant has been created and
// its value/text written with 0 — r27 is the entry, still live for the game's
// own `stw r23, 0x9C(r27)` (type = 3) on the next instruction. Void hook: the
// original `lis r6` still runs. Applying here rather than waiting for the tick
// matters, because the same ctor goes on to load raceeditor/garage/policecam/
// credits a few instructions later.
void Hook_PlatformVarInit(PPCRegister& r27) {
    const uint32_t entry = static_cast<uint32_t>(r27.u64);
    if (!IsGuestPtr(entry)) return;

    const int want = WantedPlatformValue();
    g_platform_var_ea.store(entry, std::memory_order_relaxed);
    WritePlatformVar(entry, want);
    g_platform_applied.store(want, std::memory_order_relaxed);
    MC_INFO("[buttons] mcRegistry 'platform' @0x{:08X}, glyphs = {}", entry,
            want ? "PlayStation" : "Xbox 360");
}

// sub_821F8038 pushes aspect/lang/zone/platform into a movie as it is created.
// Replaces the `li r5, 0` at 0x821F8160 that feeds the platform push; returning
// true skips it and resumes at 0x821F8164. Measured: sub_822C2EA8 calls this
// once at boot for the intro movie only, so it is not the path the menus use —
// see Hook_SwfContextEnter below for those.
bool Patch_PlatformPush(PPCRegister& r5) {
    r5.u64 = static_cast<uint64_t>(WantedPlatformValue());
    return true;
}

// Live switching.
//
// Every UI movie reads `platform` exactly once, on its own frame 1:
//
//   _global.d_platform = 0
//   FSCommand:getvar "platform"        -> sub_82721728 writes _global.d_platform
//   _global.platform   = _global.d_platform
//
// and the movies are loaded once and kept, so the registry value alone only
// takes effect on a fresh boot. What the frame code actually evaluates on every
// render, though, is `_global.platform` itself (`icon.gotoAndStop(iconID +
// platform * 49)` reads the member each time), so writing that variable in a
// live movie switches the glyphs immediately.
//
// sub_825EE970 is the AVM's "run this action buffer" entry: it parks its
// argument in dword_828FF970 for the duration, which makes r3 a context that is
// live by definition. Pushing from there is what keeps this free of dangling
// pointers — we never store a context to write to later, we only write to the
// one being handed to us, and only when its last known value differs.
constexpr uint32_t kSwfSetVarIntFn = 0x825EE0E0;  // (ctx, name, int)
constexpr uint32_t kStrPlatform    = 0x8201ABDC;  // the XEX's own "platform"

struct SwfPlatformCtx {
    uint32_t ctx = 0;
    int value = -1;
};
constexpr int kMaxSwfCtx = 16;
static SwfPlatformCtx g_swf_ctx[kMaxSwfCtx];
static int g_swf_ctx_count = 0;

static uint32_t CallGuestFn3(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1, a2);
}

void Hook_SwfContextEnter(PPCRegister& r3) {
    const uint32_t ctx = static_cast<uint32_t>(r3.u64);
    if (!ctx || !g_platform_var_ea.load(std::memory_order_relaxed)) return;

    const int want = WantedPlatformValue();

    int slot = -1;
    for (int i = 0; i < g_swf_ctx_count; ++i) {
        if (g_swf_ctx[i].ctx != ctx) continue;
        if (g_swf_ctx[i].value == want) return;  // already current
        slot = i;
        break;
    }
    if (slot < 0) {
        // Full is not expected (a handful of movies exist); recycling slot 0
        // just costs one redundant push if it ever happens.
        slot = g_swf_ctx_count < kMaxSwfCtx ? g_swf_ctx_count++ : 0;
        g_swf_ctx[slot].ctx = ctx;
    }

    CallGuestFn3(kSwfSetVarIntFn, ctx, kStrPlatform, uint32_t(want));
    g_swf_ctx[slot].value = want;
}

bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) {
    if (!REXCVAR_GET(rexglue_settings_in_gameoptions)) {
        return false;
    }
    mc::ui::RequestOpenRexGraphicsMenu();
    r3.u64 = 1;
    return true;
}

// ── UI movie pacing ──────────────────────────────────────────────────────
//
// *(0x8286D804) is mcUIManager; +60 is its movie map (atMap name -> movie**:
// buckets* @+60, u16 count @+64; node +0 char* key, +4 u16 key length,
// +8 movie**, +12 next). sub_821F9FB8 is the lookup by name. A movie has its
// swf context at +56, the enabled flag at +68 and the commit flag at +71.
//
// sub_821FC588(mgr, dt), mcUIManager::Update, runs with the clock's unscaled
// delta (0x827D7558) and steps every enabled movie with sub_82720898(movie,
// ..., byte_827DC1FC, 1): ctx->vtbl[1] (sub_827221D8 / sub_827238B0), both
// landing in sub_825EDC08(ctx, dt, ?, one_frame, ?), the swf advance.
// one_frame != 0 advances EXACTLY one frame and ignores dt; 0 accumulates dt
// and advances a frame per 256/rate seconds (rate = u16 8.8 fps at
// movieDef+48). byte_827DC1FC, what the UI manager passes as one_frame, is 1 in
// the image and nothing writes it: every movie advances one frame per update.
// The 360 updated 30 times a second, so that is what the movies were made for;
// at 60 FPS every Flash screen runs twice as fast, uncapped faster still. The
// photo album's ActionScript animates per frame -- the preview panel eases
// toward its target x, the overwrite highlight pulses with sin(f / 10) -- so
// the gallery is where it shows.
//
// A paced movie steps once per 1/30 s of real time: the per-update step of a
// movie that is not due holds back its advance, and the rest of that update
// (3D targets, commit) still runs. Which movies: the ones named in
// ui_movie_pacing, or all of them with '*'.
//
// Two callers step movies, both with the one-frame flag in r5 (byte_827DC1FC):
//   sub_821FC588, mcUIManager::Update, every movie of its map (r6 = 1);
//   sub_8223D648, the HUD manager's update, the HUD gauge movie (r6 = 0), which
//   the HUD code creates itself, so the UI manager's map does not name it.
// A movie the map does not name is therefore the gauge movie, called HUDGAUGE
// here (the song ticker under the speedometer, the needles, the race countdown's
// ring). HUDMAPMOVIE, the other half of the HUD, is in the map.
// Only movies stepped one frame per update are paced; a movie the game steps by
// elapsed time keeps its own pace and would lose the time of a held update.
// Nothing is paced before the game loop has run (g_frame_heartbeat): the
// start-up logos are drawn by a loop of their own and are held to 30 a second
// at the swap (HoldUntimedSwap in frame_timing.cpp).
REX_EXTERN(__imp__rex_sub_82720898);  // movie per-update step
REX_EXTERN(__imp__rex_sub_825EDC08);  // swf advance

namespace {
constexpr uint32_t kUiManagerPtr = 0x8286D804;
constexpr uint32_t kMgrMovieMap  = 60;
constexpr uint32_t kMovieContext = 56;
constexpr uint32_t kMovieEnabled = 68;
constexpr int64_t kStepPeriodNs  = 1'000'000'000 / 30;

int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t R32(const uint8_t* base, uint32_t ea) { return ea ? ReadGuestU32(base, ea) : 0; }
uint16_t R16(const uint8_t* base, uint32_t ea) {
    return ea ? uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]) : 0;
}

// A guest C string, or "" when the pointer is null or the bytes are not text.
std::string GuestString(const uint8_t* base, uint32_t ea, size_t max = 48) {
    if (ea < 0x10000u) return {};
    const char* s = reinterpret_cast<const char*>(base + ea);
    size_t n = 0;
    while (n < max && s[n]) {
        const unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x20 || c > 0x7E) return {};
        ++n;
    }
    return std::string(s, n);
}

// A movie of the UI manager's map: its entry (what sub_82720898 gets) and the
// name it is filed under (sub_821F9FB8 looks one up by it).
struct Screen {
    uint32_t entry;
    std::string name;
};

void ReadScreens(const uint8_t* base, std::vector<Screen>& out) {
    out.clear();
    const uint32_t mgr = R32(base, kUiManagerPtr);
    if (!mgr) return;
    const uint32_t buckets = R32(base, mgr + kMgrMovieMap);
    const uint16_t count = R16(base, mgr + kMgrMovieMap + 4);
    for (uint32_t i = 0; buckets && i < count; ++i) {
        for (uint32_t node = R32(base, buckets + 4 * i), guard = 0; node && guard < 256;
             node = R32(base, node + 12), ++guard) {
            if (!R16(base, node + 4)) continue;
            const uint32_t holder = R32(base, node + 8);
            const uint32_t entry = holder ? R32(base, holder) : 0;
            if (entry) out.push_back(Screen{entry, GuestString(base, R32(base, node), 64)});
        }
    }
}

// A movie that is paced.
struct Movie {
    uint32_t entry = 0;
    uint32_t context = 0;  // its swf context (what the advance gets), +56
    std::string name;
    bool hud = false;
    int64_t next_due_ns = 0;
    uint32_t advanced = 0;
    uint32_t held = 0;
};

std::mutex g_pacing_mutex;
std::vector<Movie> g_movies;
std::vector<Screen> g_screens;  // the manager's map, read once per game frame
std::vector<std::string> g_paced_names;
bool g_paced_all = false;
std::string g_paced_list;
bool g_paced_list_read = false;
uint64_t g_resolved_frame = ~0ull;
int64_t g_pacing_report_ns = 0;

// The context whose advance the movie update on this thread is holding back.
thread_local uint32_t t_held_context = 0;

// Re-reads the cvar and the movie map once per game frame: movies load and unload
// with the screens that own them.
void ResolveFrame(const uint8_t* base) {
    const uint64_t frame = g_frame_heartbeat.load(std::memory_order_relaxed);
    if (frame == g_resolved_frame) return;
    g_resolved_frame = frame;

    const std::string list = REXCVAR_GET(ui_movie_pacing);
    if (!g_paced_list_read || list != g_paced_list) {
        g_paced_list_read = true;
        g_paced_list = list;
        g_paced_names.clear();
        g_paced_all = false;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t end = list.find(',', pos);
            if (end == std::string::npos) end = list.size();
            std::string name = list.substr(pos, end - pos);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name == "*") g_paced_all = true;
            else if (!name.empty()) g_paced_names.push_back(name);
            pos = end + 1;
        }
    }
    ReadScreens(base, g_screens);
}

// The pacing state of a movie, or nullptr when the game steps it as it always did.
Movie* MovieFor(const uint8_t* base, uint32_t entry) {
    ResolveFrame(base);
    const Screen* screen = nullptr;
    for (const Screen& s : g_screens) {
        if (s.entry == entry) {
            screen = &s;
            break;
        }
    }
    const std::string_view name = screen ? std::string_view(screen->name) : std::string_view("HUDGAUGE");
    bool paced = g_paced_all;
    for (size_t i = 0; !paced && i < g_paced_names.size(); ++i) paced = g_paced_names[i] == name;
    if (!paced) return nullptr;

    Movie* m = nullptr;
    for (Movie& c : g_movies) {
        if (c.entry == entry) {
            m = &c;
            break;
        }
    }
    if (!m) {
        if (g_movies.size() >= 64) g_movies.clear();
        g_movies.emplace_back();
        m = &g_movies.back();
        m->entry = entry;
    }
    if (m->name != name) m->name.assign(name);
    m->hud = !screen || name == "HUDMAPMOVIE";
    const uint32_t context = R32(base, entry + kMovieContext);
    if (context != m->context) {
        m->context = context;
        m->next_due_ns = 0;
    }
    return m;
}

// One movie frame per 1/30 s of real time. A movie due within a quarter of a period is
// stepped early, so at 30 FPS every update still steps once and a frame that lands a hair
// early at 60 FPS is not pushed a whole one late. A movie that fell more than a period
// behind (disabled, a stall, below 30 FPS) restarts its schedule instead of catching up --
// the console never stepped a movie twice in one update.
bool StepDue(Movie& m, int64_t now) {
    if (m.next_due_ns && now + kStepPeriodNs / 4 < m.next_due_ns) return false;
    const bool behind = !m.next_due_ns || now > m.next_due_ns + kStepPeriodNs;
    m.next_due_ns = behind ? now + kStepPeriodNs : m.next_due_ns + kStepPeriodNs;
    return true;
}

// The race countdown (3, 2, 1, GO) is timed by the gauge movie's own script. The HUD
// update (sub_82212298) sets the movie's ActionScript variable `countdownUpdate` to
// the update's elapsed time on every update, and the script adds it to the countdown's
// clock on every STEP. On the console both ran 30 times a second, so a step added 1/30 s.
// A movie that steps less often than the game updates sees only the last update's time and
// the countdown runs slow (a race starts on the "2" at 120 FPS). Hook_UiCountdownDt gives
// the variable the time since the movie's last step instead.
struct HudClock {
    uint32_t context = 0;  // the gauge movie's swf context, from the hook
    double total = 0.0;     // what the game wrote, in seconds, since the movie appeared
    double consumed = 0.0;  // the part of it a step of the movie has taken
    uint32_t since_step = 0;  // writes since a step of this movie was last seen
};
std::mutex g_hud_mutex;
HudClock g_hud;
std::atomic<uint32_t> g_hud_context{0};  // g_hud.context, readable without the lock
// With no step seen for this many writes (a HUD that is hidden) the game's value is left
// alone rather than feeding a clock nobody reads.
constexpr uint32_t kHudLiveWrites = 100;
// What one feed may hold at most: an engine frame's worth (the 125 ms clamp of
// MCLAFrameDelta), so a movie that was not stepped for a while does not get minutes at once.
constexpr double kHudMaxFeedSeconds = 0.125;

// The movie with this context is about to step: it takes the time it was given.
void HudClockStep(uint32_t context) {
    if (context != g_hud_context.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_hud_mutex);
    if (context != g_hud.context) return;
    g_hud.since_step = 0;
    g_hud.consumed = g_hud.total;
}

// Every 10 s, into photo_ui_trace's log while the album scene is up.
void ReportPacing(const uint8_t* base, int64_t now) {
    constexpr int64_t kReportNs = 10'000'000'000;
    if (now - g_pacing_report_ns < kReportNs) return;
    g_pacing_report_ns = now;
    const bool trace = PhotoUiTracing(base);
    for (Movie& m : g_movies) {
        if (trace && (m.advanced || m.held))
            MC_INFO("[photo-ui] pacing {}: {} frame(s) advanced, {} update(s) held in the last 10 s",
                    m.name, m.advanced, m.held);
        m.advanced = 0;
        m.held = 0;
    }
}
}  // namespace

// Mid-asm hook at 0x822124DC in sub_82212298 (the HUD's update), right after
// `fmr f1,f31` and before `bl sub_825EE188` sets the ActionScript variable
// `countdownUpdate` from f1. r3 is the gauge movie's swf context and f1 this update's
// elapsed time. Gives the variable the time since the movie's last step: at least this
// update's, at most one engine frame's worth. A movie stepped on every update is given
// what it was given before, its own update's time.
void Hook_UiCountdownDt(PPCRegister& r3, PPCRegister& f1) {
    const double dt = f1.f64;
    if (!(dt >= 0.0 && dt < 1.0)) return;  // not a time: leave it to the game
    std::lock_guard<std::mutex> lock(g_hud_mutex);
    if (g_hud.context != r3.u32) {
        g_hud.context = r3.u32;
        g_hud_context.store(r3.u32, std::memory_order_relaxed);
        g_hud.total = g_hud.consumed = 0.0;
        g_hud.since_step = 0;
    }
    g_hud.total += dt;
    ++g_hud.since_step;
    if (!REXCVAR_GET(ui_countdown_fix) || g_hud.since_step > kHudLiveWrites) return;
    const double fed =
        std::max(dt, std::min(g_hud.total - g_hud.consumed, std::max(dt, kHudMaxFeedSeconds)));
    f1.f64 = static_cast<double>(static_cast<float>(fed));
}

// Movie per-update step. A paced movie that is not due holds back its advance this
// update; the rest of the update (3D targets, commit) still runs.
//
// Except while a photo is being taken (photo_album.cpp): the game hides its whole UI
// when the countdown ends and grabs the front buffer more than three updates later,
// counting on the movie having stepped -- and so hidden itself -- in every one of
// them. Held to 30 steps a second the movie could still be on screen when the grab
// comes at a few hundred FPS, so for that tenth of a second it steps every update as
// it always did. The same for a tenth of a second after a discarded photo brings the
// city back (PhotoCityReturning): the step that hides the full-screen photo must not
// wait for the next 30 Hz period while the city overwrites the photo's memory.
extern "C" REX_FUNC(rex_sub_82720898) {
    const uint32_t entry = ctx.r3.u32;
    const bool one_frame = (ctx.r5.u32 & 0xFF) != 0;
    uint32_t held = 0;
    if (entry && one_frame && base[entry + kMovieEnabled] &&
        g_frame_heartbeat.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_pacing_mutex);
        if (Movie* m = MovieFor(base, entry)) {
            const int64_t now = SteadyNowNs();
            if (PhotoCaptureInProgress(base) || PhotoCityReturning()) {
                m->next_due_ns = 0;  // back on the 30 Hz schedule from the next update
                ++m->advanced;
            } else if (StepDue(*m, now)) {
                ++m->advanced;
            } else {
                ++m->held;
                held = m->context;
            }
            ReportPacing(base, now);
        }
    }
    const uint32_t outer = t_held_context;
    t_held_context = held;
    __imp__rex_sub_82720898(ctx, base);
    t_held_context = outer;
}

// The swf advance. Only the per-update step above ever holds one back; every other
// path (sub_82720288's double step on a movie's first frames, ...) runs untouched.
extern "C" REX_FUNC(rex_sub_825EDC08) {
    if (t_held_context && ctx.r3.u32 == t_held_context) return;
    HudClockStep(ctx.r3.u32);  // whichever path made the step
    __imp__rex_sub_825EDC08(ctx, base);
}

// ── Menu cursor auto-repeat ──────────────────────────────────────────────
//
// sub_82223CD8 is FlashNavigator::fnInputHandler's update (the handler sits at
// navigator + 0x403D0; mcUIManager::Update sub_821FC588 calls it once per UI
// update through the sub_82224920 thunk). It turns the 24 mapped UI buttons
// into the uinput events every menu listens to, and a held button repeats by
// counting updates:
//
//     pressed   target[i] = 7, count[i] = 0, post X_pressed and X
//     released  target[i] = 7, count[i] = 0, post X_released
//     held      if (++count[i] == target[i]) {
//                   count[i] = 0, target[i] = handler[0x323] (6), post X }
//
// count = byte_8286E49C[24], target = byte_8286E480[24]. Button i's state is
// the 12 bytes at unk_8286E4B8 + 12 * i: down = bit 7 of state[2] ^ state[0],
// state[3] the same for the previous update. Nothing but this function and the
// reset sub_8221D6A8 touches count or target.
//
// At the 360's 30 updates a second that is a first repeat 233 ms after the
// press and then one every 200 ms. Uncapped it follows the frame rate: at
// ~144 FPS `uinput.right` came 47-67 ms after the press and then every
// 39-42 ms, so a normal tap moved the photo album's slot cursor two slots and
// holding it ran ~24 a second. Every list and grid in the game shares this.
//
// So a held button's count advances once per 1/30 s: on an update where its
// tick is not due, one is taken off before the game adds one. Presses,
// releases and the events themselves still run every update.
REX_EXTERN(__imp__rex_sub_82223CD8);  // FlashNavigator::fnInputHandler update

namespace {
constexpr uint32_t kUiButtons        = 24;
constexpr uint32_t kUiRepeatCount    = 0x8286E49C;  // u8[24]
constexpr uint32_t kUiButtonState    = 0x8286E4B8;  // 12 bytes per button
constexpr int64_t kUiRepeatPeriodNs  = 1'000'000'000 / 30;
// Takes the tick that lands a hair early at 60 FPS instead of a whole frame late.
constexpr int64_t kUiRepeatSlackNs   = 2'000'000;
}  // namespace

extern "C" REX_FUNC(rex_sub_82223CD8) {
    if (!REXCVAR_GET(ui_input_repeat_30hz)) {
        __imp__rex_sub_82223CD8(ctx, base);
        return;
    }

    // Only the UI update runs this, one thread.
    static int64_t next_tick[kUiButtons] = {};
    const int64_t now = SteadyNowNs();
    uint8_t* const count = base + kUiRepeatCount;
    uint8_t before[kUiButtons] = {};
    uint32_t held_back = 0;

    for (uint32_t i = 0; i < kUiButtons; ++i) {
        const uint8_t* state = base + kUiButtonState + 12 * i;
        const bool down = ((state[2] ^ state[0]) & 0x80) != 0;
        const bool was_down = ((state[3] ^ state[0]) & 0x80) != 0;
        if (!down) continue;
        if (!was_down) {
            next_tick[i] = now + kUiRepeatPeriodNs;  // the press: first tick 1/30 s away
            continue;
        }
        if (now + kUiRepeatSlackNs >= next_tick[i]) {
            // Due: the game's own ++count stands. A button that fell more than two
            // periods behind restarts its schedule instead of catching up.
            next_tick[i] = now - next_tick[i] > 2 * kUiRepeatPeriodNs
                               ? now + kUiRepeatPeriodNs
                               : next_tick[i] + kUiRepeatPeriodNs;
            continue;
        }
        before[i] = count[i];
        count[i] = static_cast<uint8_t>(before[i] - 1);
        held_back |= 1u << i;
    }

    __imp__rex_sub_82223CD8(ctx, base);

    // Some updates skip the whole button loop (the jumps to 0x82224470 at
    // 0x82223FE8 / 0x82224000): put back what the game did not add.
    for (uint32_t i = 0; held_back && i < kUiButtons; ++i) {
        if ((held_back >> i & 1) && count[i] == static_cast<uint8_t>(before[i] - 1))
            count[i] = before[i];
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool SkipIntro() { return false; }
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {}
bool MCLA_UI_SkipMissingLights(PPCRegister& r3) { return false; }
bool Patch_SpeedUnits(PPCRegister& r11) { return false; }
void TickButtonPrompts() {}
void Hook_PlatformVarInit(PPCRegister& r27) {}
bool Patch_PlatformPush(PPCRegister& r5) { return false; }
void Hook_SwfContextEnter(PPCRegister& r3) {}
bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
