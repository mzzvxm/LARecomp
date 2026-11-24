#pragma once

#include "mc_engine/menus/menu_items.h"

#include <vector>

namespace larecomp::settings_ui {

struct Menu {
    const char* button;
    const char* key;
    const char* label;
    const char* blurb;
    bool debug;
    std::vector<mcla_menu::ItemDef> rows;
};

// Walks the menu tables without guest state, renderer discovery, or dynamic callbacks.
std::vector<Menu> AllSettingsMenus();

}  // namespace larecomp::settings_ui

void AppendAllUpscalingPageRows(std::vector<mcla_menu::ItemDef>& rows);
