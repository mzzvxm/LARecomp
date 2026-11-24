#include "export.h"
#include "sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace larecomp::settings_ui;
using namespace mcla_menu;
using toml::table;

int main(int argc, char** argv) {
    int checks = 0;
    int failed = 0;
    const auto check = [&](bool pass, std::string_view description) {
        ++checks;
        if (!pass) {
            ++failed;
            std::cerr << "FAIL " << description << '\n';
        }
    };
    const auto metadata = [](const char* type, const char* value, const char* life = "live") {
        return table{{"type", type}, {"default", value}, {"life", life}};
    };
    const table catalog{
        {"enabled", metadata("bool", "true")},
        {"mcla_gpu_shadow_filter", metadata("string", "auto")},
        {"mcla_gpu_rt_shadows", metadata("string", "auto")},
        {"traffic_unspawn_dist", metadata("number", "0")},
        {"step", metadata("number", "0", "restart")},
    };
    constexpr double steps[] = {0, 0.5, 1};
    constexpr double distance[] = {0, 250.5};
    constexpr const char* modes[] = {"auto", "game", "soft", "pcss"};
    constexpr const char* labels[] = {"AUTO", "GAME", "SOFT", "PCSS"};
    constexpr RowDynamics dynamic = {.enabled = +[] { throw std::logic_error("live callback called"); return true; }};
    const Menu menus[] = {
        {"tab", "Hub", "GRAPHICS", "Picture settings", false,
            {Submenu("PM_Open", "DETAIL", "Detail")}},
        {"detail", "Detail", "DETAIL", nullptr, false,
            {Bool("PM_Enabled", "enabled", "ENABLED: ", true),
             NativeOnly(WithDynamics(Str("PM_Filter", "mcla_gpu_shadow_filter", "FILTER: ", modes, labels, 4), &dynamic)),
             Dbl("PM_Range", "traffic_unspawn_dist", "RANGE: ", distance, 2, "%gM"),
             Dbl("PM_Step", "step", "STEP: ", steps, 3, "%.1fX", " (NEXT START)", "STOCK")}},
    };
    const Inputs input{
        "test", "0000000000000000000000000000000000000001", "test:id",
        R"(
[rows.PM_Open]
help = "Open the detail menu."
[rows.PM_Enabled]
help = "Choose whether to enable the effect."
[rows.PM_Filter]
help = "Choose a shadow filter."
native = "kFilter"
[rows.PM_Filter.value.game]
title = "Game's Own"
[rows.PM_Range]
help = "Sets traffic range."
[rows.PM_Step]
help = "Choose the step."
)",
        R"(
[[tabs]]
id = "launcher"
label = "EXTRAS"
[[tabs.sections]]
id = "other"
label = "OTHER"
[[tabs.sections.rows]]
id = "L_step"
label = "STEP"
kind = "number"
surfaces = ["launcher"]
setting = "step"
help = "Choose the step."
)",
        R"(
[defs]
available = "api.car_rt ?? false"
[value_sets.filters]
values = [{id = "auto", label = "AUTO"}, {id = "game", label = "GAME"}, {id = "soft", label = "SOFT"}, {id = "pcss", label = "PCSS"}]
[rows.PM_Filter]
hidden_unless = "gpu.class != 'low'"
values_ref = "filters"
[rows.PM_Filter.value.pcss]
when = "features.pcss"
[rows.PM_Range]
states = [{when = "live.hdr_active ?? false", enabled = false, reason = "HDR controls this setting."}]
)",
        "",
    };
    const auto doc = BuildDocument(catalog, menus, input);
    const auto json = Json(doc);
    const auto again = Json(BuildDocument(catalog, menus, input));
    check(json == again, "successive builds produce identical bytes");
    const auto sections = doc["tabs"][0]["sections"];
    const auto rows = sections[1]["rows"];
    check(sections[0]["rows"][0]["target"].value_or("") == std::string_view("detail"), "hub link points at child");
    check(rows[0]["default"].value_or("") == std::string_view("false"), "invert bool default");
    check(rows[0]["label"].value_or("") == std::string_view("ENABLED"), "remove menu punctuation");
    check(rows[1]["native"].value_or("") == std::string_view("kFilter"), "retain native marker");
    check(rows[1]["renderers"][0].value_or("") == std::string_view("native"), "carry renderer mask");
    check(rows[1]["values"][0]["auto_label"].value_or(false), "class auto flag");
    check(rows[1]["values"][1]["title"].value_or("") == std::string_view("Game's Own"), "apply text to selected value");
    check(rows[1]["values"][3]["when"].value_or("") == std::string_view("features.pcss"), "apply value rule");
    check(rows[2]["values"][0]["auto_label"].value_or(false), "numeric auto flag");
    check(rows[2]["values"][1]["id"].value_or("") == std::string_view("250.5"), "canonical numeric id");
    check(rows[3]["values"][0]["label"].value_or("") == std::string_view("STOCK"), "pre-render special zero label");
    check(rows[3]["values"][1]["label"].value_or("") == std::string_view("0.5X"), "pre-render numeric label");
    check(rows[3]["apply"].value_or("") == std::string_view("next_start"), "apply static suffix");
    check(rows[3]["states"][0]["suffix"].value_or("") == std::string_view(" (NEXT START)"), "retain the game's suffix");
    check(doc["tabs"][1]["sections"][0]["rows"][0]["apply"].value_or("") == std::string_view("restart"),
          "launcher-only row uses setting lifecycle");
    check(doc["settings"]["mcla_gpu_shadow_filter"]["auto"]["handheld"]["low"].value_or("") ==
          std::string_view("soft"), "handheld override");
    check(doc["settings"]["mcla_gpu_rt_shadows"]["auto"]["by"].value_or("") ==
          std::string_view("rt_rung"), "RT overrides class descriptor");
    check(doc["settings"]["traffic_unspawn_dist"]["auto"]["cells"]["low"].value_or(0.0) == 250.0,
          "numeric auto cells");
    constexpr std::string_view paths[]{"api.car_rt", "features.pcss", "gpu.class", "live.hdr_active"};
    const auto* facts = doc["facts_used"].as_array();
    check(facts->size() == std::size(paths), "unique fact count");
    for (size_t i = 0; i < std::size(paths); ++i) {
        check(doc["facts_used"][i].value_or("") == paths[i], "sorted parsed fact");
    }
    auto changed = catalog;
    changed["step"].as_table()->insert_or_assign("default", "1");
    check(SettingsHash(catalog) != SettingsHash(changed), "default contributes to hash");
    changed = catalog;
    changed["step"].as_table()->insert("allowed", toml::array{"0", "1"});
    check(SettingsHash(catalog) != SettingsHash(changed), "constraints contribute to hash");
    check(Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256");
    check(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "short SHA-256");
    check(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 padding across blocks");

    if (argc > 1) {
        const auto path = std::filesystem::path(argv[1]);
        WriteDocument(path, json);
        WriteDocument(path, again);
        std::ifstream in(path, std::ios::binary);
        check(std::string(std::istreambuf_iterator<char>(in), {}) == json, "atomic replacement preserves bytes");
    }
    std::cout << checks << " checks, " << failed << " failed\n";
    return failed != 0;
}
