#pragma once
//
// The row description the pause menu's submenus are built from.
//
// This lives in a header of its own so a feature can ship its own rows next to
// its own code instead of having them spelled out in pause_menu.cpp. The menu
// still owns everything about *rendering* a row -- this is only the data.
//
// Each submenu is a table of ItemDef; the Flash list's selected index maps
// straight into the table, so item order in the table is item order on screen.
// A click cycles the row's cvar to its next value, and labels are rebuilt from
// the live cvar on every render, so a change made from the console shows up
// here too.

#include <cstdint>

namespace mcla_menu {

enum class ItemKind { kBool, kStrCycle, kDblCycle, kCarbonBit, kAction, kColor, kSubmenu };

// Action ids for ItemKind::kAction, carried in ItemDef::nvals.
enum ActionId {
    kActionPlayCutscene = 1, kActionQuitYes, kActionQuitNo, kActionDumpTextures,
    kActionReloadTextures
};

// Which renderer a row belongs to. The renderer is a restart-only choice, so
// the pause menu only lists the rows of the one this launch is running.
enum RendererMask : uint8_t { kRendXenia = 1, kRendNative = 2, kRendAny = 3 };

struct ItemDef {
    const char* key;      // guest state name + string-table key (unique!)
    ItemKind kind;
    const char* cvar;     // ignored for kCarbonBit
    const char* prefix;   // label prefix, e.g. "FULLSCREEN: "
    const char* suffix;   // e.g. " (RESTART)"
    bool inverted;        // kBool: cvar true renders as OFF (disable_* cvars)
    const char* const* svals;   // kStrCycle: cvar values
    const char* const* slabels; // kStrCycle: display labels (parallel)
    int nvals;                  // kStrCycle / kDblCycle count
    const double* dvals;        // kDblCycle values
    const char* dfmt;           // kDblCycle value format, e.g. "%.1fX"
    // kDblCycle: label to show instead of the formatted number when the value
    // is 0. Lets a numeric row carry an "off / leave it alone" entry without
    // rendering it as a bare "0". Null = format 0 like any other value.
    const char* zero_label;
    // Renderers this row is shown for. Anything not tagged is shown for all.
    uint8_t renderers = kRendAny;
};

// A row of one renderer only, e.g. XeniaOnly(Dbl(...)).
constexpr ItemDef XeniaOnly(ItemDef it)  { it.renderers = kRendXenia;  return it; }
constexpr ItemDef NativeOnly(ItemDef it) { it.renderers = kRendNative; return it; }

constexpr ItemDef Bool(const char* key, const char* cvar, const char* prefix,
                       bool inverted = false, const char* suffix = "") {
    return {key, ItemKind::kBool, cvar, prefix, suffix, inverted,
            nullptr, nullptr, 0, nullptr, nullptr};
}

constexpr ItemDef Str(const char* key, const char* cvar, const char* prefix,
                      const char* const* vals, const char* const* labels, int n,
                      const char* suffix = "") {
    return {key, ItemKind::kStrCycle, cvar, prefix, suffix, false,
            vals, labels, n, nullptr, nullptr};
}

constexpr ItemDef Dbl(const char* key, const char* cvar, const char* prefix,
                      const double* vals, int n, const char* fmt,
                      const char* suffix = "", const char* zero_label = nullptr) {
    return {key, ItemKind::kDblCycle, cvar, prefix, suffix, false,
            nullptr, nullptr, n, vals, fmt, zero_label};
}

// A row that does something when it is clicked instead of holding a value. It
// has no value list, so it renders as a plain label. `nvals` carries the
// ActionId.
constexpr ItemDef Action(const char* key, ActionId id) {
    return {key, ItemKind::kAction, nullptr, "", "", false,
            nullptr, nullptr, int(id), nullptr, nullptr};
}

// A color button, the garage's PickColor on a pause row: closed it shows the
// cvar's color beside the label; A opens a strip of swatches, left/right walk
// it with the color applied live, A keeps it and B puts the old one back.
// `vals` are the cvar values (RRGGBB, "" = the game's own color) in swatch
// order; the swatches themselves are drawn by ui_palette.cpp.
constexpr ItemDef Color(const char* key, const char* cvar, const char* prefix,
                        const char* const* vals, int n) {
    return {key, ItemKind::kColor, cvar, prefix, "", false,
            vals, nullptr, n, nullptr, nullptr};
}

// A row that opens another menu on top of this one, the way OPTIONS opens
// CONTROLLER: B on that menu comes back here. `menu_key` is the opened menu's
// MenuDef::menu_key (pause_menu.cpp); a menu some row opens gets no button of
// its own on the pause SETTINGS tab. Renders as a plain label, like the rows of
// OPTIONS do. `menu_key` rides in `cvar`, which a submenu row has no use for.
constexpr ItemDef Submenu(const char* key, const char* label, const char* menu_key) {
    return {key, ItemKind::kSubmenu, menu_key, label, "", false,
            nullptr, nullptr, 0, nullptr, nullptr};
}

// Carbon fiber part group. Unlike every other item here this one is NOT
// backed by a cvar: it flips a bit in the customization data of the car the
// player is driving, so each car carries its own selection and it persists
// in the save on its own. `nvals` carries the group bit.
constexpr ItemDef Carbon(const char* key, uint8_t group, const char* prefix,
                         const char* suffix = "") {
    return {key, ItemKind::kCarbonBit, nullptr, prefix, suffix, false,
            nullptr, nullptr, group, nullptr, nullptr};
}

}  // namespace mcla_menu
