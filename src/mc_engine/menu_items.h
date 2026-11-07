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
#include <string>

namespace mcla_menu {

enum class ItemKind { kBool, kStrCycle, kDblCycle, kCarbonBit, kAction, kColor, kSubmenu };

// Action ids for ItemKind::kAction, carried in ItemDef::nvals.
enum ActionId {
    kActionPlayCutscene = 1, kActionQuitYes, kActionQuitNo, kActionDumpTextures,
    kActionReloadTextures, kActionRestart,
    // The "restart now?" prompt shown when a menu is left with a setting that needs one.
    kActionRestartLater, kActionRestartNow
};

// Which renderer a row belongs to. The renderer is a restart-only choice, so
// the pause menu only lists the rows of the one this launch is running.
enum RendererMask : uint8_t { kRendXenia = 1, kRendNative = 2, kRendAny = 3 };

// How a row writes a setting: the pause menu's own (it remembers what the setting was for the
// "restart now?" prompt, holds the restart-only ones back and logs the change). RowDynamics::set_value
// is given it, so a row that sets several cvars goes through the same path as any other row.
using CvarWriter = void (*)(const char* cvar, const char* value);

// What a row shows and allows when that depends on other settings. Computed
// each time the row is drawn; every member is optional.
struct RowDynamics {
    // kDblCycle: the label of a value. Two values with the same label are the
    // same entry, and the cvar's own value gets an entry of its own, in order,
    // when it labels as none of `dvals` (a value set in larecomp.toml): the row
    // never shows a value it is not on, and stepping off that entry drops it.
    std::string (*value_label)(double value) = nullptr;
    // kStrCycle: the label of value i, instead of `slabels`.
    std::string (*str_label)(int i) = nullptr;
    // False greys the row out: the game's own disabled row -- the label drawn
    // grey and no value (the pause movie draws none for it), and left/right
    // and A do nothing. Null: always enabled.
    bool (*enabled)() = nullptr;
    // The label's suffix, instead of `suffix`; it can say why the row is grey.
    const char* (*suffix)() = nullptr;
    // The label's text, instead of `prefix` (its trailing ": " is dropped as usual): a row that is named
    // for what it sets now ("DLSS MODE", "RENDER SCALE"). A fixed set of texts, no allocation.
    const char* (*label)() = nullptr;
    // kStrCycle: a list of the row's own, instead of `svals`/`nvals` and the cvar. All three are set
    // together, with `str_label` for the text of each value. The game's value row shows three values
    // at a time (a window the pause menu moves along its list, pause_menu.cpp "kRowWindow"), so the
    // list may be longer, shorter or another list from one draw to the next.
    //   value_count  how many values the list has now (at least 1; the row is listed with its count at
    //                the time the page is built, which must be above 0)
    //   value_index  which one the row is on now
    //   set_value    the row was stepped to value i: write whatever that means, through `write`
    //                (any number of cvars). `cvar` still names the setting the row is about, which the
    //                "restart now?" prompt looks up (it may be null for a row that has none).
    int (*value_count)() = nullptr;
    int (*value_index)() = nullptr;
    void (*set_value)(int i, CvarWriter write) = nullptr;
    // kBool, kStrCycle, kDblCycle: the setting the row is about, instead of `cvar`: a row that is one
    // setting on one renderer and another on the other (VSYNC, MSAA, RENDER SCALE: mcla_gpu_vsync or
    // vsync, ...), one row with one name in one place. A checkbox reads and writes it; the "restart now?"
    // prompt looks it up (a row whose suffix says RESTART). May return null: a row that writes several
    // settings through `set_value` has none to give.
    const char* (*cvar)() = nullptr;
};

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
    // Labels, suffix and enabled state that follow other settings. Null: the
    // row is what the fields above say.
    const RowDynamics* dynamics = nullptr;
};

// True when this launch runs the NATIVE (Plume) renderer, false for the emulated one (XENIA). The renderer
// is a restart-only choice, so the answer never changes while the game runs.
bool NativeRendererRuns();

// A row of one renderer only, e.g. XeniaOnly(Dbl(...)).
constexpr ItemDef XeniaOnly(ItemDef it)  { it.renderers = kRendXenia;  return it; }
constexpr ItemDef NativeOnly(ItemDef it) { it.renderers = kRendNative; return it; }
// A row whose labels or state follow other settings, e.g.
// WithDynamics(Dbl(...), &kSomeDynamics).
constexpr ItemDef WithDynamics(ItemDef it, const RowDynamics* d) { it.dynamics = d; return it; }

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
