// UI and HUD: intro skip, the movie lighting null check, ReXGlue settings from
// Game Options, speedometer units, Xbox / PlayStation button glyphs and the UI
// text language.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <atomic>
#include <cstdint>
#include <string>

#include "hooks.h"
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
    "legal screen switches to its PS3 text page.")
    .allowed({"xbox", "playstation"})
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

// Intro/legals pacing is not patched here; see PaceUntimedSwap in
// frame_timing.cpp.
//
// The previous Hook_IntroHalfRate skipped every other SWF advance, which is
// only correct at exactly 60 FPS. With a configurable fps_limit (30 / 60 / 120 /
// 144 / uncapped) it is wrong at every other setting: at 120 it still runs 2x
// fast, at 30 it runs at half speed. It was removed from the midnightclub fork
// for the same reason and is not being reintroduced.
//
// Hard caps at 30, 45 and 60 gave identical intro speed in midnightclub because
// the legals/logo loop never calls the engine timer (sub_821BDA90), which is
// where the frame limiter lives: the cap was never applied to those frames at
// all. They are paced at the swap instead.

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
