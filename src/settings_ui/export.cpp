#include "export.h"

#include "os/file_replace.h"
#include "gpu/gpu_class.h"
#include "gpu/rt_tier.h"
// CHECK: the contract parser's arity map has an exit-time destructor. GX calls it only at startup.
#include "../../documentation/contract/cpp/ui_expr_ref.h"
#include "sha256.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <locale>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>

namespace larecomp::settings_ui {
namespace {

using mcla_menu::ItemDef;
using mcla_menu::ItemKind;
using Table = toml::table;
using Array = toml::array;

std::string_view Text(const toml::node_view<const toml::node>& node) {
    return node.value_or(std::string_view{});
}

std::string_view Label(std::string_view prefix) {
    while (!prefix.empty() && (prefix.back() == ':' || prefix.back() == ' ')) {
        prefix.remove_suffix(1);
    }
    return prefix;
}

std::string Number(double number) {
    if (number == 0) return "0";
    char buffer[64];
    auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), number, std::chars_format::fixed);
    if (error != std::errc{}) throw std::runtime_error("cannot format a menu value");
    return {buffer, end};
}

// Menu suffixes describe when the player sees a change, which can differ from the setting's mode.
std::string_view Apply(std::string_view life, std::string_view suffix) {
    if (suffix == " (NEXT START)") return "next_start";
    if (suffix == " (NEXT HOST)") return "next_host";
    if (suffix == " (RESTART)") return "restart";
    if (life == "restart") return "restart";
    if (life == "startup") return "next_start";
    if (life == "until_launch") return "on_reload";
    return "live";
}

constexpr bool HasDynamics(const mcla_menu::RowDynamics* d) {
    return d && (d->value_label || d->str_label || d->enabled || d->suffix || d->label ||
                 d->value_count || d->value_index || d->set_value || d->cvar);
}

void RendererMask(Table& table, uint8_t mask) {
    if (mask == mcla_menu::kRendNative) table.insert("renderers", Array{"native"});
    if (mask == mcla_menu::kRendXenia) table.insert("renderers", Array{"xenia"});
}

void AddAuto(Table& settings) {
    namespace gc = mcla::gpu::gpu_class;
    for (const auto& effect : gc::kEffects) {
        if (auto* setting = settings[effect.cvar].as_table()) {
            setting->insert_or_assign("auto", Table{
                {"by", "class"}, {"cells", Table{{"low", effect.low}, {"mid", effect.mid}, {"high", effect.high}}}});
        }
    }
    for (const auto& handheld : gc::kHandheldCells) {
        if (auto* descriptor = settings[gc::CvarName(handheld.effect)]["auto"].as_table()) {
            descriptor->insert("handheld", Table{{"low", handheld.value}});
        }
    }
    for (const auto& scale : gc::kScales) {
        if (auto* setting = settings[scale.cvar].as_table()) {
            setting->insert_or_assign("auto", Table{
                {"by", "class"}, {"auto_value", scale.zero_is_auto ? 0 : -1},
                {"cells", Table{{"low", scale.low}, {"mid", scale.mid}, {"high", scale.high}}}});
        }
    }
    // The RT table supersedes the legacy class cell for RT SHADOWS.
    for (const auto& effect : gc::kRtEffects) {
        if (auto* setting = settings[effect.cvar].as_table()) {
            // CHECK: v1 validates resolved cells as writable words. Internal results such as
            // "both" for RT AO and "on" for car RT need a validator change before export.
            if (effect.cvar == "mcla_gpu_ao_rt" || effect.cvar == "mcla_gpu_car_selfrefl_rt") continue;
            setting->insert_or_assign("auto", Table{
                {"by", "rt_rung"}, {"cells", Table{{"none", effect.none}, {"weak", effect.weak}, {"strong", effect.strong}}}});
        }
    }
}

void Overlay(Table& target, const Table& source, std::string_view excluded = {}) {
    for (const auto& [key, value] : source) {
        if (key != excluded) target.insert_or_assign(key, value);
    }
}

void OverlayRow(Table& target, const Table& source) {
    if (source.contains("values") || source.contains("values_ref") || source.contains("values_from")) {
        target.erase("values");
        target.erase("values_ref");
        target.erase("values_from");
        target.erase("fallback_values");
    }
    if (source.contains("reads") && !source.contains("setting")) target.erase("setting");
    Overlay(target, source, "value");
}

void OverlayValues(Table& row, const Table& overlay, const Table& sets, std::string_view id) {
    auto* values = row["values"].as_array();
    if (!values) values = row["fallback_values"].as_array();
    // A value-specific overlay on a reference is local to the row.
    if (!values && row.contains("values_ref")) {
        if (const auto* set = sets[Text(std::as_const(row)["values_ref"])].as_array()) {
            row.insert("values", *set);
            row.erase("values_ref");
            values = row["values"].as_array();
        }
    }
    std::set<std::string_view> applied;
    if (values) {
        for (auto& value : *values) {
            auto& entry = *value.as_table();
            const auto key = Text(std::as_const(entry)["id"]);
            if (const auto* fields = overlay[key].as_table()) {
                Overlay(entry, *fields, "id");
                applied.insert(key);
            }
        }
    }
    if (applied.size() != overlay.size()) {
        throw std::runtime_error(std::string(id) + " has an overlay for a value it does not list");
    }
}

Table Row(const ItemDef& item, const Table& settings, const Table& text, const Table& rules,
          const Table& phase_a, const Table& sets,
          const std::map<std::string_view, std::string_view>& section_ids, bool debug) {
    std::string_view kind;
    switch (item.kind) {
    case ItemKind::kBool: kind = "bool"; break;
    case ItemKind::kStrCycle: kind = "choice"; break;
    case ItemKind::kDblCycle: kind = "number"; break;
    case ItemKind::kCarbonBit: kind = "carbon"; break;
    case ItemKind::kAction: kind = "action"; break;
    case ItemKind::kColor: kind = "color"; break;
    case ItemKind::kSubmenu: kind = "link"; break;
    }

    Table row{{"id", item.key}, {"label", Label(item.prefix)}, {"kind", kind}};
    RendererMask(row, item.renderers);
    if (item.kind == ItemKind::kSubmenu) {
        row.insert("target", section_ids.at(item.cvar));
    } else if (item.cvar) {
        row.insert("setting", item.cvar);
    }
    if (item.kind == ItemKind::kAction || item.kind == ItemKind::kCarbonBit || item.kind == ItemKind::kSubmenu) {
        row.insert("surfaces", Array{"game"});
    }
    if (item.inverted) row.insert("inverted", true);
    if (item.dfmt) row.insert("format", item.dfmt);

    Array values;
    if (item.kind == ItemKind::kStrCycle && item.svals) {
        for (int index = 0; index < item.nvals; ++index) {
            values.push_back(Table{{"id", item.svals[index]}, {"label", item.slabels[index]}});
        }
    } else if (item.kind == ItemKind::kDblCycle) {
        for (int index = 0; index < item.nvals; ++index) {
            double number = item.dvals[index];
            char formatted[64];
            std::snprintf(formatted, sizeof(formatted), item.dfmt, number);
            values.push_back(Table{{"id", Number(number)},
                                   {"label", number == 0 && item.zero_label ? item.zero_label : formatted}});
        }
    }
    if (!values.empty()) row.insert("values", std::move(values));
    if (item.suffix && *item.suffix) {
        Table state{{"when", "true"}, {"suffix", item.suffix}};
        if (std::string_view(item.suffix).find("RESTART") != std::string_view::npos) {
            state.insert("needs_restart", true);
        }
        row.insert("states", Array{std::move(state)});
    }

    const auto* text_fields = text[item.key].as_table();
    const auto* rule_fields = rules[item.key].as_table();
    if (const auto* fields = phase_a[item.key].as_table()) OverlayRow(row, *fields);
    if (text_fields) OverlayRow(row, *text_fields);

    // Use the static projection during phase A. A migrated row is identified by its actual callbacks.
    if (HasDynamics(item.dynamics)) {
        if (!row.contains("native")) throw std::runtime_error(std::string(item.key) + " is missing its native dynamics name");
    } else {
        row.erase("native");
    }
    if (rule_fields) {
        OverlayRow(row, *rule_fields);
        if (const auto* fields = (*rule_fields)["value"].as_table()) OverlayValues(row, *fields, sets, item.key);
    }
    if (text_fields) {
        if (const auto* fields = (*text_fields)["value"].as_table()) OverlayValues(row, *fields, sets, item.key);
    }

    const auto setting_name = Text(std::as_const(row)["setting"]);
    const auto setting = settings[setting_name];
    if (!setting_name.empty() && !setting.is_table()) {
        throw std::runtime_error(std::string(item.key) + " names an unregistered setting: " + std::string(setting_name));
    }
    if (setting.is_table()) {
        const auto fallback = Text(setting["default"]);
        if (!row.contains("default") && !row.contains("values_from")) {
            row.insert("default", item.inverted ? (fallback == "true" ? "false" : "true") : fallback);
        }
        const auto life = Text(setting["life"]);
        if (!row.contains("apply")) {
            const auto apply = Apply(life, item.suffix);
            row.insert("apply", apply);
            if (life == "live" && apply != "live" && !row.contains("apply_note")) {
                throw std::runtime_error(std::string(item.key) + " needs apply_note for its menu suffix");
            }
        }
        if (setting["debug"].value_or(false)) row.insert_or_assign("debug", true);
        const auto automatic = setting["auto"];
        if (automatic) {
            const auto auto_value = automatic["auto_value"].value<double>();
            const auto auto_id = auto_value ? Number(*auto_value) : "auto";
            if (auto* listed = row["values"].as_array()) {
                for (auto& value : *listed) {
                    auto& entry = *value.as_table();
                    if (Text(std::as_const(entry)["id"]) == auto_id) {
                        entry.insert("auto_label", true);
                        if (auto_value && Text(std::as_const(entry)["label"]) != "AUTO") {
                            entry.insert_or_assign("label", "AUTO");
                        }
                    }
                }
            }
        }
    }
    if (!debug && !row["debug"].value_or(false) && !row.contains("help")) {
        throw std::runtime_error(std::string(item.key) + " needs help in ui_text.toml");
    }
    return row;
}

bool FactPath(std::string_view path) {
    constexpr std::string_view roots[] = {"gpu", "api", "features", "os", "display", "cpu", "build", "install", "live"};
    return std::ranges::find(roots, path.substr(0, path.find('.'))) != std::end(roots);
}

void ExpressionFacts(std::string_view expression, std::set<std::string>& facts) {
    const auto program = uiexpr::Parse(expression);
    if (!program.ok) throw std::runtime_error("invalid settings UI expression: " + std::string(expression));
    for (const std::string& path : program.paths) {
        if (FactPath(path)) facts.insert(path);
    }
}

void CollectFacts(const toml::node& node, std::set<std::string>& facts) {
    if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table) {
            const auto name = key.str();
            if (value.is_string() && (name == "when" || name == "hidden_unless" || name == "decode" ||
                                      name == "needs" || name == "expr")) {
                ExpressionFacts(value.value_or(std::string_view{}), facts);
            } else if (name != "writes" && name != "args") {
                CollectFacts(value, facts);
            }
        }
    } else if (const auto* array = node.as_array()) {
        for (const auto& child : *array) CollectFacts(child, facts);
    }
}

}  // namespace

std::string Json(const Table& table) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << toml::json_formatter{table} << '\n';
    return out.str();
}

std::string SettingsHash(const Table& settings) {
    std::string lines;
    for (const auto& [name, metadata] : settings) {
        const auto& fields = *metadata.as_table();
        Table signature;
        for (const auto field : {"allowed", "range", "no_save", "debug"}) {
            if (const auto* value = fields.get(field)) signature.insert(field, *value);
        }
        // Escape delimiters in default text so there is one canonical record per setting.
        lines.append(name.str()).append("|").append(Text(fields["type"])).append("|");
        for (const char c : Text(fields["default"])) {
            switch (c) {
            case '\r': lines.append("\\r"); break;
            case '\n': lines.append("\\n"); break;
            case '|': lines.append("\\|"); break;
            case '\\': lines.append("\\\\"); break;
            default: lines.push_back(c); break;
            }
        }
        auto constraints = Json(signature);
        std::erase(constraints, '\n');
        lines.append("|").append(Text(fields["life"])).append("|").append(constraints).append("\n");
    }
    return Sha256(lines);
}

toml::table BuildDocument(Table settings, std::span<const Menu> menus, const Inputs& inputs) {
    const auto text = toml::parse(inputs.text, std::string_view("ui_text.toml"));
    const auto launcher = toml::parse(inputs.launcher_rows, std::string_view("launcher_rows.toml"));
    const auto rules = toml::parse(inputs.rules, std::string_view("ui_rules.toml"));
    const auto phase_a = toml::parse(inputs.phase_a, std::string_view("phase_a.toml"));
    const Table empty;
    const auto table_or_empty = [&empty](const toml::node_view<const toml::node>& node) -> const Table& {
        if (const auto* table = node.as_table()) return *table;
        return empty;
    };
    const auto& text_rows = table_or_empty(text["rows"]);
    const auto& rule_rows = table_or_empty(rules["rows"]);
    const auto& phase_a_rows = table_or_empty(phase_a["rows"]);
    Table sets;
    for (const auto& [name, definition] : table_or_empty(rules["value_sets"])) {
        if (const auto* table = definition.as_table()) {
            if (const auto* values = (*table)["values"].as_array()) {
                sets.insert(name, *values);
                continue;
            }
        }
        if (!definition.is_array()) throw std::runtime_error("invalid value set: " + std::string(name.str()));
        sets.insert(name, definition);
    }
    Table defs = table_or_empty(phase_a["defs"]);
    Overlay(defs, table_or_empty(rules["defs"]));

    AddAuto(settings);
    Table doc{
        {"schema", "larecomp.settings-ui"}, {"schema_version", 1}, {"min_reader", 1}, {"expr_version", 1},
        {"generated", Table{{"game_version", inputs.game_version}, {"tool", "larecomp.exe --dump_settings_ui"},
                            {"exe_id", inputs.exe_id}, {"settings_hash", SettingsHash(settings)}}},
        {"config", Table{{"file", "larecomp.toml"}, {"style", "flat"}}},
        {"value_sets", sets}, {"defs", defs}
    };
    if (!inputs.commit.empty()) doc["generated"].as_table()->insert("commit", inputs.commit);
    if (const auto* presets = rules.get("presets")) doc.insert("presets", *presets);

    std::map<std::string_view, std::string_view> ids;
    std::map<std::string_view, const Menu*> by_key;
    std::set<std::string_view> linked;
    for (const auto& menu : menus) {
        const auto id = Text(text["sections"][menu.key]["id"]);
        ids.emplace(menu.key, id.empty() ? menu.button : id);
        by_key.emplace(menu.key, &menu);
        for (const auto& row : menu.rows) {
            if (row.kind == ItemKind::kSubmenu) linked.insert(row.cvar);
        }
    }

    std::set<std::string_view> included;
    const auto append_section = [&](auto&& self, Array& sections, const Menu& menu, uint8_t renderers) -> void {
        if (!included.insert(menu.key).second) throw std::runtime_error(std::string("menu included twice: ") + menu.key);
        Table section{{"id", ids.at(menu.key)}, {"source_menu", menu.key}, {"label", menu.label}};
        RendererMask(section, renderers);
        if (menu.debug) section.insert("debug", true);
        if (menu.blurb) section.insert("blurb", menu.blurb);
        if (const auto* fields = text["sections"][menu.key].as_table()) Overlay(section, *fields, "rows");

        Array rows;
        for (const auto& item : menu.rows) {
            rows.push_back(Row(item, settings, text_rows, rule_rows, phase_a_rows, sets, ids, menu.debug));
        }
        section.insert("rows", std::move(rows));
        sections.push_back(std::move(section));
        for (const auto& row : menu.rows) {
            if (row.kind == ItemKind::kSubmenu) {
                self(self, sections, *by_key.at(row.cvar), renderers & row.renderers);
            }
        }
    };

    Array tabs;
    for (const auto& menu : menus) {
        if (linked.contains(menu.key)) continue;
        Table tab{{"id", menu.button}, {"label", menu.label}};
        if (menu.blurb) tab.insert("blurb", menu.blurb);
        if (menu.debug) tab.insert("debug", true);
        if (const auto* fields = text["tabs"][menu.key].as_table()) Overlay(tab, *fields);
        Array sections;
        append_section(append_section, sections, menu, mcla_menu::kRendAny);
        tab.insert("sections", std::move(sections));
        tabs.push_back(std::move(tab));
    }
    if (included.size() != menus.size()) throw std::runtime_error("settings menu links contain a cycle");
    if (const auto* extra = launcher["tabs"].as_array()) {
        for (const auto& tab : *extra) {
            Table added = *tab.as_table();
            for (auto& section : *added["sections"].as_array()) {
                for (auto& row : *(*section.as_table())["rows"].as_array()) {
                    auto& fields = *row.as_table();
                    const auto name = Text(std::as_const(fields)["setting"]);
                    const auto metadata = std::as_const(settings)[name];
                    if (!metadata.is_table()) {
                        throw std::runtime_error("launcher row names an unregistered setting: " + std::string(name));
                    }
                    fields.insert("apply", Apply(Text(metadata["life"]), {}));
                }
            }
            tabs.push_back(std::move(added));
        }
    }

    std::set<std::string> facts;
    CollectFacts(tabs, facts);
    CollectFacts(sets, facts);
    CollectFacts(defs, facts);
    for (const auto& [key, def] : defs) {
        if (def.is_string()) ExpressionFacts(def.value_or(std::string_view{}), facts);
    }
    if (const auto* presets = doc.get("presets")) CollectFacts(*presets, facts);
    Array fact_list;
    for (const auto& fact : facts) fact_list.push_back(fact);

    doc.insert("facts_used", std::move(fact_list));
    doc.insert("tabs", std::move(tabs));
    doc.insert("settings", std::move(settings));
    return doc;
}

void WriteDocument(const std::filesystem::path& path, std::string_view json) {
    const auto temp = os::TempPathFor(path);
    std::ofstream file(temp, std::ios::binary);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    file.close();
    if (!file) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw std::runtime_error("cannot write the temporary settings UI file");
    }
    std::string reason;
    if (os::MoveIntoPlace(temp, path, &reason) == os::MoveResult::kFailed) {
        throw std::runtime_error("cannot replace the settings UI file: " + reason);
    }
}

}  // namespace larecomp::settings_ui
