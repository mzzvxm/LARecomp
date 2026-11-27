#include "export.h"
#include "sha256.h"

#include "../../documentation/contract/cpp/mini_json.h"
#include "../../documentation/contract/cpp/ui_expr_ref.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <stdexcept>

using namespace larecomp::settings_ui;
using namespace mcla_menu;
using toml::table;

namespace {

using Contract = larecomp::minijson::Json;
namespace expr = larecomp::uiexpr;

struct Checks {
    int count = 0;
    std::vector<std::string> failures;

    void operator()(bool pass, std::string_view description) {
        ++count;
        if (!pass) failures.emplace_back(description);
    }
};

Contract ReadContract(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    const std::string bytes(std::istreambuf_iterator<char>(in), {});
    Contract document;
    std::string error;
    if (!larecomp::minijson::Parse(bytes, &document, &error) || !document.IsObject()) {
        throw std::runtime_error(path.string() + ": expected a JSON object " + error);
    }
    return document;
}

const Contract& Field(const Contract& node, std::string_view key) {
    static const Contract missing;
    const auto* field = node.Find(key);
    return field ? *field : missing;
}

using Choice = std::pair<std::string_view, std::string_view>;
constexpr Choice quality[] = {{"off", "OFF"}, {"high", "HIGH"}, {"ultra", "ULTRA"}};
constexpr Choice strength[] = {{"light", "1"}, {"default", "2"}, {"strong", "3"}, {"stronger", "4"}, {"strongest", "5"}};
constexpr Choice on_off[] = {{"off", "OFF"}, {"on", "ON"}};
constexpr std::string_view named_presets[] = {"vanilla", "enhanced", "enhanced_rt"};
struct BounceRow {
    const char* id;
    const char* setting;
    const char* label;
    std::span<const Choice> choices;
    std::array<std::string_view, 3> preset_cells;
    bool legacy_auto;
};
constexpr BounceRow bounce_rows[] = {
    {"PM_RxGpuSsgi", "mcla_gpu_ssgi", "BOUNCE LIGHT", quality, {"off", "off", "high"}, true},
    {"PM_RxGpuRtGi", "mcla_gpu_rt_gi", "BOUNCE LIGHT RT", quality, {"off", "off", "high"}, true},
    {"PM_RxGpuSsgiLevel", "mcla_gpu_ssgi_intensity", "BOUNCE STRENGTH", strength, {"default", "default", "default"}, false},
    {"PM_RxGpuLightBounce", "mcla_gpu_light_bounce", "LIGHT BOUNCE", on_off, {"off", "on", "on"}, false},
};

bool IsBounceSetting(std::string_view setting) {
    return setting.starts_with("mcla_gpu_ssgi") || setting.starts_with("mcla_gpu_rt_gi") ||
           setting.starts_with("mcla_gpu_light_bounce");
}

struct BounceEnv final : expr::Env {
    const Contract& machine;
    std::map<std::string, expr::Def, std::less<>> definitions;
    std::string ao = "off", screen = "off", rt = "off";
    bool no_update = false, replaced = false, pressure = false;

    BounceEnv(const Contract& machine, const Contract& defs) : machine(machine) {
        for (const auto* id : {"ao_on", "rt_gi_open", "bounce_on"}) {
            definitions[id].expr = std::make_shared<expr::Program>(expr::Parse(Field(defs, id).s));
        }
    }
    expr::Value Setting(std::string_view key) const override {
        if (key == "mcla_gpu_ao") return expr::Value::String(ao);
        if (key == "mcla_gpu_ssgi") return expr::Value::String(screen);
        if (key == "mcla_gpu_rt_gi") return expr::Value::String(rt);
        return {};
    }
    expr::Value Fact(std::string_view key) const override {
        if (no_update && key == "build.rt_gi_update_built") return expr::Value::Bool(false);
        if (replaced && key == "live.rt_gi_replaces_bounce") return expr::Value::Bool(true);
        if (pressure && key == "live.rt_gi_resource_pressure") return expr::Value::Bool(true);
        const auto* fact = machine.Path(key);
        if (fact && fact->IsString()) return expr::Value::String(fact->s);
        if (fact && fact->IsBool()) return expr::Value::Bool(fact->b);
        return {};
    }
    expr::Value Renderer() const override { return expr::Value::String("native"); }
    const expr::AutoDescriptor* Auto(std::string_view) const override { return nullptr; }
    const expr::Def* DefOf(std::string_view key) const override {
        const auto it = definitions.find(key);
        return it == definitions.end() ? nullptr : &it->second;
    }
};

void CheckBounceState(Checks& check, const Contract& row, const BounceEnv& env, bool enabled,
                      std::string_view suffix, std::string_view scenario) {
    const auto description = row.StringOr("id", "missing row") + " " + std::string(scenario);
    const auto* hidden = row.Find("hidden_unless");
    check(!hidden || expr::ListedUnlessFalse(expr::Evaluate(expr::Parse(hidden->s), env)), description + " listed");
    for (const auto& state : Field(row, "states").a) {
        const auto when = expr::Parse(Field(state, "when").s);
        check(when.ok && !when.unsupported_call, description + " state expression");
        if (!expr::Matches(expr::Evaluate(when, env))) continue;
        check(state.BoolOr("enabled", true) == enabled && Field(state, "suffix").s == suffix, description + " state");
        return;
    }
    check(enabled && suffix.empty(), description + " state");
}

struct BounceMachine {
    const char* name;
    Contract facts;
    bool rt;
};

Checks CheckBounceExport(const Contract& doc, std::span<const BounceMachine> machines) {
    Checks check;
    std::array<const Contract*, std::size(bounce_rows)> rows{};
    std::array<int, std::size(bounce_rows)> occurrences{};
    const auto& settings = Field(doc, "settings");
    const auto& presets = Field(doc, "presets");
    const auto& items = Field(presets, "items");
    bool found_quarter_size = false;
    for (const auto& tab : Field(doc, "tabs").a) {
        for (const auto& section : Field(tab, "sections").a) {
            for (const auto& row : Field(section, "rows").a) {
                const auto& id = Field(row, "id").s;
                const auto& key = Field(row, "setting").s;
                const auto* expected = std::ranges::find(bounce_rows, id, &BounceRow::id);
                if (expected != std::end(bounce_rows)) {
                    const auto index = expected - bounce_rows;
                    rows[index] = &row;
                    ++occurrences[index];
                    check(Field(tab, "id").s == "gfx" && Field(tab, "label").s == "GRAPHICS" &&
                          Field(section, "id").s == "lighting" && Field(section, "source_menu").s == "RxLightingMenu" &&
                          Field(section, "label").s == "LIGHTING AND SHADOWS", id + " page");
                    check(!tab.BoolOr("debug", false) && !section.BoolOr("debug", false) &&
                          !row.BoolOr("debug", false), id + " player visibility");
                } else if (IsBounceSetting(key)) {
                    found_quarter_size |= id == "PM_RxDbgSsgiQuarter" && key == "mcla_gpu_ssgi_quarter";
                    check(Field(settings, key).BoolOr("debug", false) && tab.BoolOr("debug", false) &&
                          section.BoolOr("debug", false) && row.BoolOr("debug", false), id + " debug visibility");
                }
            }
        }
    }
    check(found_quarter_size, "quarter-size bounce has a diagnostic row");
    check(Field(settings, "mcla_gpu_ssgi_strength").BoolOr("debug", false), "raw SSGI strength override is diagnostic");

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& expected = bounce_rows[i];
        const std::string id(expected.id);
        check(occurrences[i] == 1, id + " occurs once");
        if (!rows[i]) continue;
        const auto& row = *rows[i];
        const auto& setting = Field(settings, expected.setting);
        check(Field(row, "setting").s == expected.setting && Field(row, "label").s == expected.label &&
              Field(row, "kind").s == "choice", id + " binding and label");
        check(Field(row, "apply").s == "live" && Field(setting, "life").s == "live" &&
              Field(setting, "type").s == "string" && !setting.BoolOr("debug", true), id + " live player setting");
        const auto& renderers = Field(row, "renderers").a;
        check(renderers.size() == 1 && renderers[0].s == "native", id + " native renderer");
        if (const auto* surfaces = row.Find("surfaces")) {
            check(std::ranges::count(surfaces->a, "launcher", &Contract::s) == 1 &&
                  std::ranges::count(surfaces->a, "game", &Contract::s) == 1, id + " game and launcher surfaces");
        }
        const auto& values = Field(row, "values").a;
        check(std::ranges::equal(values, expected.choices, [](const Contract& value, const Choice& choice) {
            return Field(value, "id").s == choice.first && Field(value, "label").s == choice.second && !value.Find("when");
        }), id + " choice IDs and labels");
        if (expected.legacy_auto) {
            check(Field(row, "decode").s == std::string("resolved(s.") + expected.setting + ")", id + " legacy choice decoding");
        }
        const auto& allowed = Field(setting, "allowed").a;
        check(allowed.size() == expected.choices.size() + expected.legacy_auto &&
              (!expected.legacy_auto || allowed.front().s == "auto") &&
              std::ranges::equal(std::span(allowed).subspan(expected.legacy_auto), expected.choices,
                                 [](const Contract& value, const Choice& choice) { return value.s == choice.first; }),
              id + " allowed setting values");
        check(std::ranges::count(Field(presets, "covers").a, expected.setting, &Contract::s) == 1, id + " covered by presets");
        for (size_t p = 0; p < std::size(named_presets); ++p) {
            const auto& cells = Field(Field(items, named_presets[p]), "values");
            check(Field(cells, expected.setting).s == expected.preset_cells[p], id + " " + std::string(named_presets[p]) + " cell");
        }
    }
    if (std::ranges::find(rows, nullptr) != rows.end()) return check;
    const auto& [screen, rt, strength_row, moving] = rows;
    const auto& enhanced_rt = Field(items, "enhanced_rt");
    const auto needs = expr::Parse(Field(enhanced_rt, "needs").s);
    check(needs.ok && !Field(enhanced_rt, "needs_reason").s.empty(), "ENHANCED RT explains its capability requirement");

    for (const auto& machine : machines) {
        const std::string name(machine.name);
        BounceEnv env(machine.facts, Field(doc, "defs"));
        check(expr::Matches(expr::Evaluate(needs, env)) == machine.rt, name + " ENHANCED RT availability");
        const auto state = [&](const Contract& row, bool enabled, std::string_view suffix, std::string_view scenario) {
            CheckBounceState(check, row, env, enabled, suffix, name + " " + std::string(scenario));
        };
        state(*screen, false, "", "AO off");
        state(*rt, machine.rt, machine.rt ? "" : " (NO RT GPU)", "RT capability");
        state(*strength_row, false, "", "both GI modes off");
        state(*moving, true, "", "AO and GI off");

        env.ao = "on";
        env.screen = "high";
        state(*screen, true, "", "screen bounce on");
        state(*strength_row, true, "", "screen bounce alone");

        env.screen = "off";
        env.rt = "high";
        env.replaced = env.pressure = machine.rt;
        state(*screen, true, machine.rt ? " (REPLACED)" : "", "RT replacement");
        state(*rt, machine.rt, machine.rt ? " (VRAM)" : " (NO RT GPU)", "RT warning");
        env.ao = "off";
        state(*screen, false, machine.rt ? " (REPLACED)" : "", "RT replacement with AO off");
        state(*strength_row, machine.rt, "", "RT bounce alone");

        env.no_update = true;
        state(*rt, false, machine.rt ? " (NOT READY)" : " (NO RT GPU)", "RT update missing");
        state(*strength_row, false, "", "RT bounce unavailable");
    }
    return check;
}

Contract& MutableField(Contract& object, std::string_view key) {
    for (auto& [name, value] : std::views::reverse(object.o)) {
        if (name == key) return value;
    }
    throw std::runtime_error("mutation needs field " + std::string(key));
}

Contract& NamedEntry(Contract& array, std::string_view id) {
    for (auto& entry : array.a) {
        if (Field(entry, "id").s == id) return entry;
    }
    throw std::runtime_error("mutation needs entry " + std::string(id));
}

Checks CheckBounceMutations(const Contract& doc, std::span<const BounceMachine> machines) {
    Checks check;
    const auto lighting = [](Contract& copy, std::string_view page) -> Contract& {
        auto& gfx = NamedEntry(MutableField(copy, "tabs"), "gfx");
        return NamedEntry(MutableField(gfx, "sections"), page);
    };
    const auto rejects = [&](std::string_view mutation, std::string_view expected, auto change) {
        auto copy = doc;
        change(copy);
        const auto result = CheckBounceExport(copy, machines);
        check(std::ranges::find(result.failures, expected) != result.failures.end(), mutation);
    };
    rejects("drop screen bounce row", "PM_RxGpuSsgi occurs once", [&](Contract& copy) {
        auto& rows = MutableField(lighting(copy, "lighting"), "rows");
        std::erase_if(rows.a, [](const auto& row) { return Field(row, "id").s == "PM_RxGpuSsgi"; });
    });
    rejects("move light bounce to Display", "PM_RxGpuLightBounce page", [&](Contract& copy) {
        auto& rows = MutableField(lighting(copy, "lighting"), "rows");
        auto& display = MutableField(lighting(copy, "display"), "rows");
        display.a.push_back(NamedEntry(rows, "PM_RxGpuLightBounce"));
        std::erase_if(rows.a, [](const auto& row) { return Field(row, "id").s == "PM_RxGpuLightBounce"; });
    });
    rejects("use RT rung for the capability guard", "PM_RxGpuRtGi Steam Deck Vulkan RT capability state", [&](Contract& copy) {
        auto& rows = MutableField(lighting(copy, "lighting"), "rows");
        auto& states = MutableField(NamedEntry(rows, "PM_RxGpuRtGi"), "states");
        const auto no_rt = std::ranges::find_if(states.a, [](const auto& state) {
            return Field(state, "suffix").s == " (NO RT GPU)";
        });
        if (no_rt == states.a.end()) throw std::runtime_error("mutation needs NO RT GPU state");
        MutableField(*no_rt, "when").s = "gpu.rt_rung == 'none'";
    });
    rejects("change the ENHANCED RT screen bounce cell", "PM_RxGpuSsgi enhanced_rt cell", [](Contract& copy) {
        auto& items = MutableField(MutableField(copy, "presets"), "items");
        auto& values = MutableField(MutableField(items, "enhanced_rt"), "values");
        MutableField(values, "mcla_gpu_ssgi").s = "off";
    });
    rejects("leak quarter-size bounce into Lighting", "PM_RxDbgSsgiQuarter debug visibility", [&](Contract& copy) {
        auto& debug = NamedEntry(MutableField(copy, "tabs"), "debug");
        for (auto& section : MutableField(debug, "sections").a) {
            auto& rows = MutableField(section, "rows").a;
            const auto entry = std::ranges::find_if(rows, [](const auto& row) {
                return Field(row, "id").s == "PM_RxDbgSsgiQuarter";
            });
            if (entry == rows.end()) continue;
            auto leaked = std::move(*entry);
            rows.erase(entry);
            MutableField(leaked, "debug").b = false;
            MutableField(lighting(copy, "lighting"), "rows").a.push_back(std::move(leaked));
            return;
        }
        throw std::runtime_error("mutation needs quarter-size bounce row");
    });
    return check;
}

int CheckBounceDump(const std::filesystem::path& dump, const std::filesystem::path& examples) {
    // CHECK: run this against the staged executable's dump and compare the launcher's exact-pin UI readback.
    const auto doc = ReadContract(dump);
    const BounceMachine machines[] = {
        {"GTX 1080 probe", ReadContract(examples / "machine.probe-no-rt.example.json"), false},
        {"RTX 5080 D3D12", ReadContract(examples / "fleet/rtx5080-d3d12.machine.json"), true},
        {"RTX 5080 Vulkan", ReadContract(examples / "fleet/rtx5080-vulkan.machine.json"), true},
        {"Steam Deck Vulkan", ReadContract(examples / "fleet/steam-deck.machine.json"), true},
    };
    const auto check = CheckBounceExport(doc, machines);
    for (const auto& failure : check.failures) std::cerr << "FAIL " << failure << '\n';
    std::cout << check.count << " bounce checks, " << check.failures.size() << " failed\n";
    if (!check.failures.empty()) return 1;
    const auto mutations = CheckBounceMutations(doc, machines);
    for (const auto& escaped : mutations.failures) std::cerr << "FAIL mutation escaped: " << escaped << '\n';
    std::cout << mutations.count - mutations.failures.size() << '/' << mutations.count << " bounce mutations caught\n";
    return !mutations.failures.empty();
}

}  // namespace

// CHECK: compare the installed executable's dump with the source harness dump.
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bounce-export") {
        if (argc < 3 || argc > 4) {
            std::cerr << "usage: larecomp_settings_ui_test --bounce-export <settings-ui.json> [contract examples dir]\n";
            return 2;
        }
        try {
            return CheckBounceDump(argv[2], argc == 4 ? argv[3] : "documentation/contract/examples");
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 2;
        }
    }
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
    for (const auto& row : bounce_rows) {
        check(covered(row.setting), std::string(row.setting) + " is preset-covered");
        for (size_t p = 0; p < std::size(named_presets); ++p) {
            const auto cells = presets["items"][named_presets[p]]["values"];
            check(cells[row.setting].value_or(std::string_view{}) == row.preset_cells[p],
                  std::string(row.setting) + " " + std::string(named_presets[p]) + " preset cell");
        }
    }
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
