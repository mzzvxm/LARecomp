#pragma once
//
// The pages of the hidden DEBUG menu (SETTINGS-REVIEW.md 5.2 and 5.3).
//
// pause_menu.cpp lists them in kMenus as MenuDef::debug_only menus: a DEBUG hub on the SETTINGS tab
// (a button only while DebugMenuUnlocked(), debug_menu.h) whose rows open seven pages. Their rows are
// built here, next to the actions they run, and handed over through MenuDef::extra, the way
// CameraLookMenuItems hands over the free-look rows.
//
// Every row names a cvar tagged .debug_only() and live (not restart-only): tools/check_settings_exposure.py
// reads this file as the debug tables and fails on a row that breaks either rule. A change made here goes
// through the pause menu's own CvarSet, so it applies at once; it is never written to larecomp.toml
// (the save rule in config_save.cpp), so what a tester changes lasts until the game closes.
//
// A feature that wants a debug row of its own adds it to one of the tables in debug_menu_pages.cpp (or a
// page's own row function), with a key that starts with PM_RxDbg and a cvar tagged .debug_only().
//

#include <string>

#include "menu_items.h"

namespace mcla_menu {

// The rows of the DEBUG hub (one Submenu row per page) and of each page, for MenuDef::extra. The rows
// that only one renderer has (the Plume renderer's views and logs) are tagged NativeOnly.
const ItemDef* DebugHubRows(int& count);
const ItemDef* DebugFixesRows(int& count);
const ItemDef* DebugPerformanceRows(int& count);
const ItemDef* DebugRenderingRows(int& count);
const ItemDef* DebugCameraRows(int& count);
const ItemDef* DebugToolsRows(int& count);
const ItemDef* DebugLoggingRows(int& count);
const ItemDef* DebugInfoRows(int& count);

// The text of an action row of the debug pages (kActionDebugShowChanged .. kActionDebugTelemetry): its
// own words, with the count of changed settings on the one that lists them and a short answer for a
// moment after a click. Guest thread, like the menu; cheap enough to call on every draw.
std::string DebugActionLabel(ActionId id);

// What a click on such a row does. Guest thread.
void DebugActionRun(ActionId id);

}  // namespace mcla_menu
