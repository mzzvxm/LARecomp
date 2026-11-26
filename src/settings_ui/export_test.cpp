#include "export.h"
#include "sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace larecomp::settings_ui;
using namespace mcla_menu;
using toml::table;

// CHECK: compare the installed executable's dump with the source harness dump.
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
    const Menu menus[] = {
        {"tab", "Hub", "GRAPHICS", "Picture settings", false,
            {Submenu("PM_Open", "DETAIL", "Detail")}},
        {"detail", "Detail", "DETAIL", nullptr, false,
            {Bool("PM_Enabled", "enabled", "ENABLED: ", true),
             NativeOnly(Str("PM_Filter", "mcla_gpu_shadow_filter", "FILTER: ", modes, labels, 4)),
             Dbl("PM_Range", "traffic_unspawn_dist", "RANGE: ", distance, 2, "%gM"),
             Dbl("PM_Step", "step", "STEP: ", steps, 3, "%.1fX", " (NEXT START)", "STOCK"),
             Str("PM_Mode", "step", "MODE: ", nullptr, nullptr, 0)}},
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
[rows.PM_Mode]
help = "Choose the mode."
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
values_from = {generator = "local.range", pause_only = true}
fallback_values = [{id = "0", label = "AUTO"}, {id = "250.5", label = "250.5M"}]
[[rows.PM_Mode.variants]]
when = "renderer == 'native'"
setting = "mcla_gpu_shadow_filter"
values_from = {generator = "local.filter", pause_only = true}
fallback_values = [{id = "game", label = "GAME", when = "features.pcss"}]
[[rows.PM_Mode.variants]]
when = "true"
values_from = {generator = "render_scale.shares", args = {renderer = "xenia"}}
fallback_values = [{id = "1", label = "100%"}]
)",
        "",
        "1.336.600",
    };
    const auto doc = BuildDocument(catalog, menus, input);
    const auto json = Json(doc);
    const auto again = Json(BuildDocument(catalog, menus, input));
    check(json == again, "successive builds produce identical bytes");
    // min_launcher is launcher-recipe.json's value, written at the top level next to min_reader; left out when the build has none.
    check(doc["min_launcher"].value_or("") == std::string_view("1.336.600") && doc["min_reader"].value_or(0) == 1,
          "min_launcher carries the recipe's value beside min_reader");
    auto ungated = input;
    ungated.min_launcher = {};
    check(!BuildDocument(catalog, menus, ungated).contains("min_launcher"), "a build without min_launcher leaves the field out");
    const auto sections = doc["tabs"][0]["sections"];
    const auto rows = sections[1]["rows"];
    check(sections[0]["rows"][0]["target"].value_or("") == std::string_view("detail"), "hub link points at child");
    check(rows[0]["default"].value_or("") == std::string_view("false"), "invert bool default");
    check(rows[0]["label"].value_or("") == std::string_view("ENABLED"), "remove menu punctuation");
    check(!rows[1].as_table()->contains("native"), "no native marker survives: the rules replaced the callbacks");
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
    check(!rows[2]["values_from"] && !rows[2]["fallback_values"], "omit a pause-only generator and its fallback fields");
    check(!rows[4]["default"], "a variant-only list uses the selected setting's default");
    const auto variants = rows[4]["variants"];
    check(!variants[0]["values_from"] && !variants[0]["fallback_values"], "omit pause-only generator fields from variants");
    check(variants[0]["values"][0]["id"].value_or("") == std::string_view("game") &&
          variants[0]["values"][0]["when"].value_or("") == std::string_view("features.pcss"), "export conditional fallback values as static steps");
    check(variants[1]["values_from"]["generator"].value_or("") == std::string_view("render_scale.shares") &&
          variants[1]["fallback_values"][0]["id"].value_or("") == std::string_view("1"), "preserve a v1 generator and its fallback");
    for (const char* fallback : {"", "fallback_values = []"}) {
        const auto rules = std::string("[rows.PM_Range]\nvalues_from = {generator = \"local.range\", pause_only = true}\n") + fallback;
        auto invalid = input;
        invalid.rules = rules;
        bool rejected = false;
        try {
            BuildDocument(catalog, menus, invalid);
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "a pause-only generator needs fallback values";
        }
        check(rejected, "reject a pause-only generator with no static steps");
    }
    check(doc["tabs"][1]["sections"][0]["rows"][0]["apply"].value_or("") == std::string_view("restart"),
          "launcher-only row uses setting lifecycle");
    const auto shadow_auto = doc["settings"]["mcla_gpu_shadow_filter"]["auto"];
    check(!shadow_auto["handheld"], "there is no handheld override");
    for (const auto cls : {"low", "mid", "high"}) {
        check(shadow_auto["cells"][cls].value_or("") == std::string_view("pcss"), "legacy Auto is the same across classes");
    }
    check(doc["settings"]["mcla_gpu_rt_shadows"]["auto"]["by"].value_or("") ==
          std::string_view("rt_rung"), "RT overrides class descriptor");
    check(doc["settings"]["traffic_unspawn_dist"]["auto"]["cells"]["low"].value_or(0.0) == 300.0,
          "numeric auto cells");
    check(!doc["settings"]["mcla_gpu_vendor_denoiser"], "a catalog without the toggle stays unchanged");
    auto denoiser_catalog = catalog;
    denoiser_catalog.insert("mcla_gpu_vendor_denoiser", metadata("bool", "true"));
    const auto denoiser_doc = BuildDocument(denoiser_catalog, menus, input);
    const auto vendor = denoiser_doc["settings"]["mcla_gpu_vendor_denoiser"];
    const auto legacy = vendor["legacy_from"];
    check(vendor["default"].value_or("") == std::string_view("true"), "legacy lookup keeps the catalog default");
    check(legacy["setting"].value_or("") == std::string_view("mcla_gpu_denoiser"), "identify the saved legacy key");
    const auto* map = legacy["map"].as_table();
    check(map && map->size() == 2, "export only explicit legacy choices");
    check(legacy["map"]["rr"].value<bool>() == true, "the old NVIDIA and AMD choices both use rr and map to true");
    check(!legacy["map"]["auto"], "legacy auto leaves the default in effect");
    check(legacy["map"]["ours"].value<bool>() == false, "ours maps to false");
    constexpr std::string_view paths[]{"api.car_rt", "features.pcss", "gpu.class", "live.hdr_active"};
    const auto* facts = doc["facts_used"].as_array();
    check(facts->size() == std::size(paths), "unique fact count");
    for (size_t i = 0; i < std::size(paths); ++i) {
        check(doc["facts_used"][i].value_or("") == paths[i], "sorted parsed fact");
    }
    // The graphics presets are the game's own table (gpu/gfx_preset.h), exported where the catalog has the setting that holds the choice.
    check(!doc.contains("presets"), "no presets block where the catalog has no preset setting");
    auto with_presets = catalog;
    with_presets.insert("mcla_gpu_preset", metadata("string", "vanilla"));
    const auto preset_doc = BuildDocument(with_presets, menus, input);
    const auto presets = preset_doc["presets"];
    check(presets["setting"].value_or("") == std::string_view("mcla_gpu_preset"), "the presets name their setting");
    check(presets["order"].as_array() && presets["order"].as_array()->size() == 4 && presets["order"][0].value_or("") == std::string_view("vanilla") &&
          presets["order"][3].value_or("") == std::string_view("custom"), "four presets, VANILLA first, CUSTOM last");
    check(presets["first_start"].value_or("") == std::string_view("enhanced"), "ENHANCED is the first-start fallback");
    check(presets["first_start_by"].value_or("") == std::string_view("class_to_preset"), "first start uses the recommendation rules");
    check(presets["legacy_file_preset"].value_or("") == std::string_view("custom"),
          "a file with a covered setting and no preset starts as CUSTOM");
    const auto legacy_keys = presets["legacy_file_keys"];
    check(legacy_keys.as_array() && legacy_keys.as_array()->size() == 3 &&
          legacy_keys[0].value_or("") == std::string_view("mcla_gpu_msaa") &&
          legacy_keys[1].value_or("") == std::string_view("mcla_gpu_aa") &&
          legacy_keys[2].value_or("") == std::string_view("mcla_gpu_car_mirror_rt"),
          "old AA and RT mirror keys also trigger legacy CUSTOM");
    check(presets["items"]["vanilla"]["values"]["mcla_gpu_ao"].value_or("") == std::string_view("off") &&
          presets["items"]["enhanced"]["values"]["mcla_gpu_ao"].value_or("") == std::string_view("on") &&
          presets["items"]["enhanced_rt"]["values"]["mcla_gpu_ao"].value_or("") == std::string_view("rt"), "the values are the table's");
    check(presets["items"]["enhanced_rt"]["needs"].value_or("") == std::string_view("gpu.rt_tier != 'none'") && presets["items"]["enhanced_rt"]["needs_reason"] &&
          !presets["items"]["enhanced"]["needs"], "only the RT presets need a ray tracing tier, and say why");
    check(!presets["items"]["custom"]["values"], "CUSTOM has no values of its own");
    const auto* covers = presets["covers"].as_array();
    check(covers && covers->size() == 38, "38 covered settings");
    const auto covered = [covers](std::string_view key) {
        return covers && std::ranges::any_of(*covers, [key](const toml::node& node) {
            return node.value_or(std::string_view{}) == key;
        });
    };
    check(covered("mcla_gpu_ao"), "a saved AO setting triggers the exported legacy preset");
    check(covered("mcla_gpu_ao_rt") && presets["items"]["vanilla"]["values"]["mcla_gpu_ao_rt"].value_or("") == std::string_view("off") &&
          presets["items"]["enhanced_rt"]["values"]["mcla_gpu_ao_rt"].value_or("") == std::string_view("both"),
          "car occlusion RT has its own concrete preset cells");
    check(!covered("mcla_gpu_msaa") && !covered("mcla_gpu_aa"), "both Display AA settings are outside presets");
    check(covered("mcla_gpu_ssr_rt") && presets["items"]["enhanced"]["values"]["mcla_gpu_ssr_rt"].value_or("") == std::string_view("off") &&
          presets["items"]["enhanced_rt"]["values"]["mcla_gpu_ssr_rt"].value_or("") == std::string_view("high"),
          "the surface-reflection fallback is explicit and independent");
    check(!presets["items"]["vanilla"]["values"]["fps_limit"] && !presets["items"]["vanilla"]["values"]["mcla_gpu_upscaler"], "a preset names no DISPLAY or UPSCALING setting");
    const auto rules = presets["class_to_preset"];
    check(rules.as_array() && rules.as_array()->size() == 3 &&
          rules[0]["when"].value_or("") == std::string_view("gpu.class == 'high' && gpu.rt_tier == 'strong'") &&
          rules[0]["preset"].value_or("") == std::string_view("enhanced_rt") &&
          rules[1]["when"].value_or("") == std::string_view("gpu.class == 'low'") &&
          rules[1]["preset"].value_or("") == std::string_view("vanilla") &&
          rules[2]["when"].value_or("") == std::string_view("true") &&
          rules[2]["preset"].value_or(std::string_view{}) == presets["first_start"].value_or(std::string_view{}),
          "the exported rules give the first-start preset");
    check(presets["flip"]["to"].value_or("") == std::string_view("custom") && presets["flip"]["writes"][0]["setting"].value_or("") == std::string_view("mcla_gpu_preset_from"),
          "a change of a covered row flips to CUSTOM and remembers where it started");
    bool has_tier = false;
    for (const auto& fact : *preset_doc["facts_used"].as_array()) has_tier = has_tier || fact.value_or("") == std::string_view("gpu.rt_tier");
    check(has_tier, "the recommendation's facts are in facts_used");
    check(Json(preset_doc) == Json(BuildDocument(with_presets, menus, input)), "the presets are deterministic");
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
