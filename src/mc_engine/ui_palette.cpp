#include "ui_palette.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "hooks/hooks_internal.h"
#include "logging.h"
#include "pause_menu.h"
#include "swf_dom.h"

namespace larecomp {
namespace {

constexpr uint32_t kGameObjects = 0x82874374;  // dword_82874374: the game-object array
constexpr uint32_t kTableOffset = 1568;        // swatch 0, from table (sub_826A3780)
constexpr uint32_t kSwatchStride = 48;

// Used when the game's paint table cannot be read; roughly the garage's range.
constexpr uint8_t kBuiltin[kUiPaletteColors][3] = {
    {0xE0, 0x20, 0x20}, {0xC0, 0x10, 0x30}, {0xFF, 0x60, 0x20}, {0xFF, 0xA0, 0x00},
    {0xFF, 0xD8, 0x00}, {0xC8, 0xE0, 0x20}, {0x60, 0xD0, 0x30}, {0x10, 0xA0, 0x40},
    {0x00, 0xB8, 0xA0}, {0x00, 0xD8, 0xF0}, {0x40, 0xA0, 0xFF}, {0x20, 0x60, 0xFF},
    {0x20, 0x30, 0xC0}, {0x60, 0x40, 0xE0}, {0xA0, 0x30, 0xE0}, {0xE0, 0x40, 0xC0},
    {0xFF, 0x60, 0xA0}, {0x80, 0x40, 0x20}, {0xFF, 0xFF, 0xFF}, {0xD0, 0xD0, 0xD0},
    {0x90, 0x90, 0x90}, {0x60, 0x60, 0x60}, {0x30, 0x30, 0x30}, {0x10, 0x10, 0x10},
};

uint8_t g_rgb[kUiPaletteColors][3];
char g_hex[kUiPaletteColors + 1][8] = {};
bool g_ready = false;

bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xC0000000u && (p & 3u) == 0; }

// The garage's swatches, read the way the garage reads them. False unless every
// one of the 24 looks like a color.
bool ReadGameTable(const uint8_t* base, uint8_t out[kUiPaletteColors][3]) {
    const uint32_t arr = ReadGuestU32(base, kGameObjects);
    if (!PlausiblePtr(arr)) return false;
    // sub_822A3998 picks slot 2 or 34 by a byte of the thread's context; whichever
    // leads to a readable table is the one.
    for (const uint32_t slot : {2u, 34u}) {
        const uint32_t obj = ReadGuestU32(base, arr + 4 * slot);
        if (!PlausiblePtr(obj)) continue;
        const uint32_t a = ReadGuestU32(base, obj + 48);
        if (!PlausiblePtr(a)) continue;
        const uint32_t b = ReadGuestU32(base, a + 132);
        if (!PlausiblePtr(b)) continue;
        const uint32_t table = b + 64;
        bool ok = true, varied = false;
        uint8_t tmp[kUiPaletteColors][3];
        for (int i = 0; i < kUiPaletteColors && ok; ++i) {
            const uint32_t at = table + kTableOffset + kSwatchStride * i;
            for (int c = 0; c < 3; ++c) {
                const float f = ReadGuestF32(base, at + 4 * c);
                if (!(f >= 0.0f && f <= 1.0f)) {
                    ok = false;
                    break;
                }
                tmp[i][c] = uint8_t(f * 255.0f + 0.5f);
                if (tmp[i][c] != tmp[0][c]) varied = true;
            }
        }
        if (ok && varied) {
            std::copy(&tmp[0][0], &tmp[0][0] + sizeof(tmp), &out[0][0]);
            MC_INFO("[ui-palette] game table: slot {} obj=0x{:08X} table=0x{:08X}", slot, obj, table);
            return true;
        }
    }
    return false;
}

void FillHex() {
    g_hex[0][0] = 0;
    for (int i = 0; i < kUiPaletteColors; ++i)
        std::snprintf(g_hex[i + 1], sizeof(g_hex[i + 1]), "%02X%02X%02X", g_rgb[i][0], g_rgb[i][1],
                      g_rgb[i][2]);
}

}  // namespace

void UiPaletteInit() {
    if (g_ready) return;
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (base && ReadGameTable(base, g_rgb)) {
        MC_INFO("[ui-palette] using the garage's paint table (first swatch {:02X}{:02X}{:02X})",
                g_rgb[0][0], g_rgb[0][1], g_rgb[0][2]);
    } else {
        std::copy(&kBuiltin[0][0], &kBuiltin[0][0] + sizeof(kBuiltin), &g_rgb[0][0]);
        MC_WARN("[ui-palette] the game's paint table was not readable, using the built-in swatches");
    }
    FillHex();
    g_ready = true;
}

const char* UiPaletteHex(int k) {
    if (!g_ready) UiPaletteInit();
    return g_hex[std::clamp(k, 0, kUiPaletteColors)];
}

}  // namespace larecomp

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc.h>
#include <rex/ppc/function.h>

#include "swf_import.h"

REX_EXTERN(__imp__rex_sub_825EE1C8);  // swfCONTEXT: a number from _global

REXCVAR_DEFINE_BOOL(ui_color_buttons, true, "MCLA/UI",
    "Show the pause menu's color buttons (UI OPTIONS) as the garage's own "
    "PickColor: garage.xsf's color button, placed in the pause movie and "
    "driven the way the garage's Fillmenu drives it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp {
namespace {

// ── The garage's PickColor in the pause movie ───────────────────────────────
//
// garage.xsf's color button is sprite #1070. The garage menu (#1127) places it
// ten times, swatches01..swatches10, at depths 3170.. and at the base (13.15,
// 22.2): 6.7 left of and 7.2 below that menu's slotsA (19.85, 15.0). Its
// frame 0 is
//
//   depth  1  swatch_cursor   #659   the yellow frame behind the chosen cell
//   depth  5  all_swatches    #1068  swatche01..05 (#652, 23.1 apart) and the
//                                    animated arrow_left / arrow_rignt (#171)
//   depth 20                  #1069  the closed button's box
//   depth 21  simple_swatche  #652   the closed button's color
//   DoAction  stop()
//
// and its frame 1 removes those four depths again. The garage's Fillmenu
// (garage2d only, so the pause movie's copy of it never gets there) drives one
// button per color row -- type 30 closed, type 31 open:
//
//   closed  swatch_cursor._alpha = 0, all_swatches._alpha = 0
//   both    _alpha = 100, _x = list_intercol1 + list_intercol + 15,
//           _y = row * list_interline + list_margin,
//           assignColor(simple_swatche, color)
//   open    all_swatches._alpha = 100; elements[i].offset follows select
//           (numswatches = 5, four rules, in Apply); swatche01..05 get the
//           colors offset .. offset + 4; swatch_cursor._x = 23.1 * (select -
//           offset), swatch_cursor._alpha = 100
//
// The sprite is imported (swf_import.h) and placed in the pause movie's
// 'menu', at the garage's depths and at the garage's base relative to the
// pause menu's own slotsA, and what Fillmenu does is done here with the same
// properties and numbers -- the layout values come from the pause movie's
// _global, which its own Fillmenu / MeasureCols set for the list on screen.
//
// The pause movie's pool has about a hundred free instances with the menu up,
// and a whole button holds 26. A closed one shows only its box and
// simple_swatche (4 instances with the root). So a button is left stopped on
// frame 0 -- where its own stop() leaves it -- without running that frame:
// the box comes from the frame's anonymous PlaceObject2, which a stopped
// sprite's timeline places on every update (sub_8260A7D0, command vtable
// +12), and simple_swatche, swatch_cursor and all_swatches from the frame's
// named PlaceObject2 commands, run in the movie's update right before the
// button updates -- the last two only while it is open. The button's frame-1
// RemoveObject2 commands take those two away again when it closes.
constexpr char kMovieName[] = "PAUSEMOVIE";
constexpr char kGarageMovie[] = "resources/ui/garage/garage.xsf";
constexpr int kButtonChar = 1070;
constexpr uint16_t kDepthCursor = 1;
constexpr uint16_t kDepthStrip = 5;
constexpr uint16_t kDepthSimple = 21;
constexpr int kButtons = PauseMenuColorRows::kMax;
// #1127's depths of swatches01..swatches10 (names and depths are not in step).
constexpr uint16_t kGarageDepth[10] = {3170, 3214, 3192, 3258, 3236,
                                      3302, 3280, 3346, 3324, 3368};
static_assert(kButtons <= 10, "the garage menu has ten buttons");
constexpr float kBaseFromSlotsX = 13.15f - 19.85f;
constexpr float kBaseFromSlotsY = 22.2f - 15.0f;
// The pause menu's (#453) slotsA, for when the live one cannot be read.
constexpr float kPauseSlotsX = 23.1f;
constexpr float kPauseSlotsY = 13.65f;
constexpr int kNumSwatches = 5;       // garage _global.numswatches
constexpr float kCursorStep = 23.1f;  // swatch_cursor._x = 23.1 * (select - offset)
constexpr float kColumnGap = 15.0f;   // _x = list_intercol1 + list_intercol + 15
constexpr const char* kCellNames[kNumSwatches] = {"swatche01", "swatche02", "swatche03",
                                                  "swatche04", "swatche05"};
// Instances of a closed button (root, box, simple_swatche and its shape), and
// what opening one adds (swatch_cursor 5, all_swatches 17).
constexpr int kClosedInstances = 4;
constexpr int kOpenInstances = 22;
// Left in the movie's pool for the timelines after ours. An empty pool is fatal.
constexpr int kPoolMargin = 32;

// swfINSTANCE
constexpr uint32_t kInstAlpha = 79;
constexpr uint32_t kInstBaseX = 92;  // the base translation its PlaceObject2 gave it
constexpr uint32_t kInstBaseY = 96;
constexpr uint32_t kInstPropY = 120;    // _y
constexpr uint32_t kInstPlaying = 172;  // the ctor sets it, stop() clears it
constexpr uint32_t kInstName = 196;

// _global numbers the layout is made of.
enum { kIntercol1, kIntercol, kInterline, kMargin, kNumGlobals };
constexpr const char* kGlobalNames[kNumGlobals] = {"list_intercol1", "list_intercol",
                                                   "list_interline", "list_margin"};

struct Button {
    uint32_t root = 0;
    int offset = 0;  // Fillmenu's elements[tempID].offset
};

struct Layout {
    bool ok = false;
    float x = 0.0f;  // _x of every button
    float interline = 0.0f, margin = 0.0f;
    float base_x = 0.0f, base_y = 0.0f;
};

struct Buttons {
    uint32_t ctx = 0;   // swfCONTEXT they were placed in
    uint32_t menu = 0;  // the 'menu' instance they are children of
    int count = 0;
    Button btn[kButtons];
    Layout layout;
};

// Guest thread only (the UI tick and the movie update both run on it).
Buttons g_btn;
int g_open = -1;  // the button whose strip BeforeButtonUpdate keeps, -1 = none

struct Commands {
    uint32_t simple = 0, cursor = 0, strip = 0;    // frame 0, named PlaceObject2
    uint32_t remove_cursor = 0, remove_strip = 0;  // frame 1, RemoveObject2
};
const swf::ImportedSprite* g_button = nullptr;
Commands g_cmd;
uint32_t g_names[kButtons] = {};          // "swatches01".. in guest memory
uint32_t g_global_keys[kNumGlobals] = {};  // the _global names, in guest memory
uint32_t g_global_out = 0;
bool g_import_tried = false;
// Per opening of the screen: one build attempt, each "why not" logged once.
bool g_build_failed = false;
bool g_why_logged = false;
bool g_layout_logged = false;
bool g_globals_logged = false;
bool g_pool_logged = false;

// Inside PAUSEMOVIE's update, right before a button updates (swf_import.h):
// what of frame 0 the button shows is placed here, where its own timeline
// would place it.
void BeforeButtonUpdate(PPCContext& ctx, uint8_t* base, uint32_t root) {
    int k = 0;
    while (k < g_btn.count && g_btn.btn[k].root != root) ++k;
    if (k == g_btn.count) return;  // not one of this screen's
    if (!swf::ChildAtDepth(base, root, kDepthSimple))
        swf::RunCommand(ctx, base, g_cmd.simple, root);
    const bool has_cursor = swf::ChildAtDepth(base, root, kDepthCursor) != 0;
    const bool has_strip = swf::ChildAtDepth(base, root, kDepthStrip) != 0;
    if (g_open == k) {
        if (has_cursor && has_strip) return;
        const int free = swf::PoolFree(base, g_btn.ctx);
        if (free < kOpenInstances + kPoolMargin) {
            if (!g_pool_logged)
                MC_WARN("[ui-palette] not opening button {}: {} has {} free instances, the "
                        "strip needs {} and {} stay free", k, kMovieName, free, kOpenInstances,
                        kPoolMargin);
            g_pool_logged = true;
            return;
        }
        if (!has_cursor) swf::RunCommand(ctx, base, g_cmd.cursor, root);
        if (!has_strip) swf::RunCommand(ctx, base, g_cmd.strip, root);
        MC_INFO("[ui-palette] button {} open: swatch_cursor 0x{:08X}, all_swatches 0x{:08X}, "
                "pool {} -> {} free", k, swf::ChildAtDepth(base, root, kDepthCursor),
                swf::ChildAtDepth(base, root, kDepthStrip), free,
                swf::PoolFree(base, g_btn.ctx));
    } else if (has_cursor || has_strip) {
        if (has_cursor) swf::RunCommand(ctx, base, g_cmd.remove_cursor, root);
        if (has_strip) swf::RunCommand(ctx, base, g_cmd.remove_strip, root);
        MC_INFO("[ui-palette] button {} closed, pool {} free", k, swf::PoolFree(base, g_btn.ctx));
    }
}

// The garage's button, imported once -- the first time a color row is on
// screen, so a boot never depends on it -- with the commands of it that are
// run here. Null when it cannot be had; the log says why.
const swf::ImportedSprite* GarageButton() {
    if (g_import_tried) return g_button;
    g_import_tried = true;
    uint8_t* base = rex::Runtime::instance()->virtual_membase();
    const swf::ImportedSprite* imp = swf::ImportSprite(kGarageMovie, kButtonChar);
    if (!imp) {
        MC_ERROR("[ui-palette] the garage's color button could not be imported; the color rows "
                 "of UI OPTIONS show no button");
        return nullptr;
    }
    Commands c;
    c.simple = swf::FrameCommand(base, imp->character, 0, 1, kDepthSimple);
    c.cursor = swf::FrameCommand(base, imp->character, 0, 1, kDepthCursor);
    c.strip = swf::FrameCommand(base, imp->character, 0, 1, kDepthStrip);
    c.remove_cursor = swf::FrameCommand(base, imp->character, 1, 2, kDepthCursor);
    c.remove_strip = swf::FrameCommand(base, imp->character, 1, 2, kDepthStrip);
    if (!c.simple || !c.cursor || !c.strip || !c.remove_cursor || !c.remove_strip) {
        MC_ERROR("[ui-palette] #{} is not the button it was: simple 0x{:08X} cursor 0x{:08X} "
                 "strip 0x{:08X} remove 0x{:08X} / 0x{:08X}", kButtonChar, c.simple, c.cursor,
                 c.strip, c.remove_cursor, c.remove_strip);
        return nullptr;
    }
    for (int k = 0; k < kButtons; ++k) {
        char name[16];
        std::snprintf(name, sizeof(name), "swatches%02d", k + 1);
        g_names[k] = swf::GuestString(name);
    }
    for (int i = 0; i < kNumGlobals; ++i) g_global_keys[i] = swf::GuestString(kGlobalNames[i]);
    g_global_out = swf::GuestScratch(4);
    g_cmd = c;
    swf::SetBeforeUpdate(imp->character, BeforeButtonUpdate);
    g_button = imp;
    MC_INFO("[ui-palette] garage color button #{} ready: sprite 0x{:08X}, commands simple "
            "0x{:08X} cursor 0x{:08X} strip 0x{:08X}, removes 0x{:08X} 0x{:08X}", kButtonChar,
            imp->character, c.simple, c.cursor, c.strip, c.remove_cursor, c.remove_strip);
    return imp;
}

uint32_t PauseMenuClip(const uint8_t* base, swf::Movie& mv) {
    if (!swf::FindMovie(base, kMovieName, mv)) return 0;
    const uint32_t screen = swf::FindChild(base, mv.root, "screen");
    return screen ? swf::FindChild(base, screen, "menu") : 0;
}

bool StillThere(const uint8_t* base) {
    swf::Movie mv;
    if (PauseMenuClip(base, mv) != g_btn.menu || mv.ctx != g_btn.ctx) return false;
    for (int k = 0; k < g_btn.count; ++k)
        if (g_btn.btn[k].root &&
            swf::ChildAtDepth(base, g_btn.menu, kGarageDepth[k]) != g_btn.btn[k].root)
            return false;
    return true;
}

void Forget() {
    g_btn = Buttons{};
    g_open = -1;
    swf::SetRaw(nullptr, 0);
}

void Destroy(uint8_t* base) {
    swf::Bind bind(base, g_btn.ctx);
    if (!bind.ok()) return;  // mid-update; next tick
    for (int k = 0; k < g_btn.count; ++k)
        if (g_btn.btn[k].root) swf::Remove(base, g_btn.menu, kGarageDepth[k]);
    MC_INFO("[ui-palette] color buttons removed from {} (pool {} free)", kMovieName,
            swf::PoolFree(base, g_btn.ctx));
    Forget();
}

bool Build(uint8_t* base, const swf::Movie& mv, uint32_t menu, int count) {
    const swf::ImportedSprite* button = GarageButton();
    if (!button) {
        g_build_failed = true;
        return false;
    }
    UiPaletteInit();
    count = std::min(count, kButtons);
    const int free_before = swf::PoolFree(base, mv.ctx);
    const int need = count * kClosedInstances + kOpenInstances + kPoolMargin;
    if (count <= 0 || free_before < need) {
        MC_WARN("[ui-palette] not placing {} color buttons: {} has {} free instances (needs {})",
                count, kMovieName, free_before, need);
        g_build_failed = true;
        return false;
    }

    swf::Bind bind(base, mv.ctx);
    if (!bind.ok()) {
        MC_WARN("[ui-palette] not placing the color buttons: {} is mid-update", kMovieName);
        return false;
    }

    Buttons b;
    b.ctx = mv.ctx;
    b.menu = menu;
    b.count = count;
    // The garage's base for its buttons, kept relative to the menu's slotsA.
    float slots_x = kPauseSlotsX, slots_y = kPauseSlotsY;
    if (const uint32_t slots_a = swf::FindChild(base, menu, "slotsA")) {
        slots_x = ReadGuestF32(base, slots_a + kInstBaseX);
        slots_y = ReadGuestF32(base, slots_a + kInstBaseY);
    }
    b.layout.base_x = slots_x + kBaseFromSlotsX;
    b.layout.base_y = slots_y + kBaseFromSlotsY;
    for (int k = 0; k < count; ++k) {
        const uint32_t root = swf::Place(base, menu, kGarageDepth[k], button->character);
        if (!root) {
            for (int i = 0; i < k; ++i) swf::Remove(base, menu, kGarageDepth[i]);
            MC_WARN("[ui-palette] PlaceObject of the garage button failed at depth {}",
                    kGarageDepth[k]);
            g_build_failed = true;
            return false;
        }
        // Stopped on frame 0, where its own stop() leaves it, without running
        // the frame: BeforeButtonUpdate runs the parts of it that are shown.
        base[root + kInstPlaying] = 0;
        if (g_names[k]) WriteGuestU32(base, root + kInstName, g_names[k]);
        swf::SetBaseTranslation(base, root, b.layout.base_x, b.layout.base_y);
        swf::SetAlphaPercent(base, root, 0);  // Apply shows it on its row
        b.btn[k].root = root;
    }
    g_btn = b;
    g_open = -1;
    g_layout_logged = false;
    MC_INFO("[ui-palette] {} garage color buttons placed in {} 'menu' 0x{:08X}: depths {}.., "
            "base ({:.2f}, {:.2f}) from slotsA ({:.2f}, {:.2f}), pool {} -> {} free", count,
            kMovieName, menu, kGarageDepth[0], b.layout.base_x, b.layout.base_y, slots_x, slots_y,
            free_before, swf::PoolFree(base, mv.ctx));
    return true;
}

uint32_t PaletteRgb(int i) {
    return (uint32_t(g_rgb[i][0]) << 16) | (uint32_t(g_rgb[i][1]) << 8) | g_rgb[i][2];
}

// Value 0 is the game's own color for the row's family -- what the menu shows
// while that theme cvar is empty.
uint32_t ValueRgb(const char* cvar, int value) {
    if (value >= 1 && value <= kUiPaletteColors) return PaletteRgb(value - 1);
    if (cvar && std::strcmp(cvar, "ui_theme_title") == 0) return 0xF4CC04;
    if (cvar && std::strcmp(cvar, "ui_theme_frame") == 0) return 0xCCCCCC;
    return 0x03F9FA;
}

// The four layout numbers as the pause movie's _global holds them now. One it
// does not have reads as 0 -- what the garage's script gets for it, since this
// VM's ToNumber makes undefined 0 (sub_825EF8C8). Bit i of the result is set
// for each name that was missing; -1 when nothing could be read at all.
int ReadGlobals(uint8_t* base, uint32_t ctx, float out[kNumGlobals]) {
    if (!g_global_out) return -1;
    int missing = 0;
    for (int i = 0; i < kNumGlobals; ++i) {
        out[i] = 0.0f;
        if (!g_global_keys[i]) return -1;
        WriteGuestF32(base, g_global_out, 0.0f);
        if (rex::ppc::GuestToHostFunction<uint32_t>(__imp__rex_sub_825EE1C8, ctx,
                                                    g_global_keys[i], g_global_out))
            out[i] = ReadGuestF32(base, g_global_out);
        else
            missing |= 1 << i;
    }
    return missing;
}

uint32_t RowSlot(const uint8_t* base, uint32_t slots_a, int slot) {
    if (!slots_a || slot < 0 || slot > 23) return 0;
    char name[8];
    std::snprintf(name, sizeof(name), "slot%d", slot);
    return swf::FindChild(base, slots_a, name);
}

// Fillmenu's type 30 / 31 for every button (the top of this section).
void Apply(uint8_t* base, const PauseMenuColorRows& rows) {
    Layout& lay = g_btn.layout;
    float g[kNumGlobals];
    const int missing = ReadGlobals(base, g_btn.ctx, g);
    if (missing >= 0) {
        lay.x = g[kIntercol1] + g[kIntercol] + kColumnGap;
        lay.interline = g[kInterline];
        lay.margin = g[kMargin];
        if (!g_layout_logged)
            MC_INFO("[ui-palette] layout from {} _global: list_intercol1 {:.1f} list_intercol "
                    "{:.1f} list_interline {:.1f} list_margin {:.1f} -> _x {:.1f}", kMovieName,
                    g[kIntercol1], g[kIntercol], g[kInterline], g[kMargin], lay.x);
        if (missing && !g_globals_logged) {
            std::string names;
            for (int i = 0; i < kNumGlobals; ++i)
                if (missing & (1 << i)) names += std::string(names.empty() ? "" : ", ") + kGlobalNames[i];
            MC_WARN("[ui-palette] {} _global has no {}; read as 0, as the garage's script would",
                    kMovieName, names);
            g_globals_logged = true;
        }
        lay.ok = true;
        g_layout_logged = true;
    } else if (!g_globals_logged) {
        MC_WARN("[ui-palette] the layout numbers could not be read from {} _global (no guest "
                "scratch); the buttons stay hidden", kMovieName);
        g_globals_logged = true;
    }
    const uint32_t slots_a = swf::FindChild(base, g_btn.menu, "slotsA");

    uint32_t raw[swf::kMaxRaw];
    int nraw = 0;
    int open = -1;
    for (int k = 0; k < g_btn.count; ++k) {
        Button& b = g_btn.btn[k];
        if (!b.root) continue;
        swf::KeepDrawn(base, b.root);
        const uint32_t cursor = swf::FindChild(base, b.root, "swatch_cursor");
        const uint32_t strip = swf::FindChild(base, b.root, "all_swatches");
        const int slot = k < rows.count ? rows.row[k] - rows.scroll : -1;
        const uint32_t row = RowSlot(base, slots_a, slot);
        if (!lay.ok || !row || base[row + kInstAlpha] == 0) {
            swf::SetAlphaPercent(base, b.root, 0);  // its row is not on screen
            continue;
        }
        const bool is_open = k == rows.open;
        if (!is_open) {
            if (cursor) swf::SetAlphaPercent(base, cursor, 0);
            if (strip) swf::SetAlphaPercent(base, strip, 0);
        } else if (strip) {
            swf::SetAlphaPercent(base, strip, 100);
        }
        swf::SetAlphaPercent(base, b.root, 100);
        swf::SetX(base, b.root, lay.x);
        // Fillmenu gives the button the _y it gives the row's slotsA.slotN
        // (row * list_interline + list_margin) in the same pass, so the row's
        // own _y is that number as the rows on screen were laid out with it.
        // _global.list_margin itself can read 0 by then (InitData resets it),
        // which put the buttons ~2.7 rows too high.
        swf::SetY(base, b.root, ReadGuestF32(base, row + kInstPropY));
        if (const uint32_t simple = swf::FindChild(base, b.root, "simple_swatche")) {
            swf::SetTint(base, simple, ValueRgb(rows.cvar[k], rows.value[k]));
            if (nraw < swf::kMaxRaw) raw[nraw++] = simple;
        }
        if (!is_open) continue;

        open = k;
        // elements[tempID].offset, moved the way Fillmenu moves it.
        const int select = rows.value[k];
        int& off = b.offset;
        if (select == 0) off = 0;
        if (select > kNumSwatches - off) off = select - kNumSwatches;
        if (!(select < off + kNumSwatches)) ++off;
        if (select < off && off > 0) --off;
        for (int c = 0; strip && c < kNumSwatches; ++c) {
            const uint32_t cell = swf::FindChild(base, strip, kCellNames[c]);
            if (!cell) continue;
            // elements[v] past the end is undefined, and setRGB(undefined) is black.
            const int v = off + c;
            swf::SetTint(base, cell, v < rows.values ? ValueRgb(rows.cvar[k], v) : 0x000000);
            if (nraw < swf::kMaxRaw) raw[nraw++] = cell;
        }
        if (cursor) {
            swf::SetX(base, cursor, kCursorStep * float(select - off));
            swf::SetAlphaPercent(base, cursor, 100);
        }
    }
    g_open = open;
    swf::SetRaw(raw, nraw);
}

}  // namespace

void UiPaletteTick(uint8_t* base) {
    if (!base) return;
    if (g_btn.menu && !StillThere(base)) {
        MC_INFO("[ui-palette] {} was rebuilt; the old color buttons went with it", kMovieName);
        Forget();
    }

    PauseMenuColorRows rows;
    const bool open = REXCVAR_GET(ui_color_buttons) && PauseMenuColorButtons(&rows);
    if (!open || (g_btn.menu && rows.count != g_btn.count)) {
        if (g_btn.menu) Destroy(base);
        if (!open) {
            // One more try, and fresh log lines, the next time the screen opens.
            g_build_failed = false;
            g_why_logged = false;
            g_globals_logged = false;
            g_pool_logged = false;
            return;
        }
    }

    swf::Movie mv;
    const uint32_t menu = PauseMenuClip(base, mv);
    if (!menu || !mv.enabled) {
        if (!g_why_logged)
            MC_WARN("[ui-palette] color buttons on screen but no clip to put them in: {} "
                    "ctx=0x{:08X} root=0x{:08X} menu=0x{:08X} enabled={}", kMovieName, mv.ctx,
                    mv.root, menu, mv.enabled);
        g_why_logged = true;
        if (g_btn.menu) Destroy(base);
        return;
    }
    if (!g_btn.menu && (g_build_failed || !Build(base, mv, menu, rows.count))) return;
    Apply(base, rows);
}

}  // namespace larecomp

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp {
void UiPaletteTick(uint8_t*) {}
}  // namespace larecomp

#endif  // REXGLUE_HAS_XEO3_TARGET
