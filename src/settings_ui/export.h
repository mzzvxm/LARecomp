#pragma once

#include "menu_catalog.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

namespace larecomp::settings_ui {

struct Inputs {
    std::string_view game_version;
    std::string_view commit;
    std::string_view exe_id;
    std::string_view text;
    std::string_view launcher_rows;
    std::string_view rules;
    std::string_view phase_a;
    std::string_view min_launcher;  // launcher-recipe.json's min_launcher; empty = none (the field is left out)
};

toml::table BuildDocument(toml::table settings, std::span<const Menu> menus, const Inputs& inputs);
std::string Json(const toml::table& table);
std::string SettingsHash(const toml::table& settings);
void WriteDocument(const std::filesystem::path& path, std::string_view json);

}  // namespace larecomp::settings_ui
