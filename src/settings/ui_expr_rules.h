#pragma once

#include <toml++/toml.hpp>

#include "ui_expr.h"

namespace larecomp::uiexpr {

inline std::shared_ptr<Program> ReadCondition(const toml::table& table, std::string_view field) {
    if (auto text = table[field].value<std::string>())
        return std::make_shared<Program>(Parse(*text));
    return {};
}

inline Value ReadValue(const toml::node* node) {
    if (!node) return {};
    if (const auto* v = node->as_boolean()) return Value::Bool(v->get());
    if (const auto* v = node->as_integer()) return Value::Number(double(v->get()));
    if (const auto* v = node->as_floating_point()) return Value::Number(v->get());
    if (const auto* v = node->as_string()) return Value::String(v->get());
    return {};
}

inline Def ReadDefinition(const toml::node& node) {
    Def result;
    if (const auto* text = node.as_string()) {
        result.expr = std::make_shared<Program>(Parse(text->get()));
    } else if (const auto* table = node.as_table()) {
        if (const auto* cases = (*table)["cases"].as_array()) {
            for (const auto& entry : *cases) {
                if (const auto* item = entry.as_table()) {
                    DefCase rule;
                    rule.when = ReadCondition(*item, "when");
                    const auto value = (*item)["value"];
                    if (const auto* expression = value.as_table())
                        rule.expr = ReadCondition(*expression, "expr");
                    else
                        rule.literal = ReadValue(value.node());
                    result.cases.push_back(std::move(rule));
                }
            }
        }
    }
    return result;
}

struct ConditionalTable {
    std::shared_ptr<Program> when;
    const toml::table* data;
};

inline std::vector<ConditionalTable> ReadTables(const toml::array* array) {
    std::vector<ConditionalTable> result;
    if (array) {
        for (const auto& entry : *array) {
            if (const auto* table = entry.as_table())
                result.push_back({ReadCondition(*table, "when"), table});
        }
    }
    return result;
}

inline const toml::table* MatchedTable(const std::vector<ConditionalTable>& rules, const Env& env) {
    const auto* match = FirstMatch(rules, env, [](const auto& rule) { return rule.when.get(); });
    return match ? match->data : nullptr;
}

struct RuleFields {
    explicit RuleFields(const toml::table& table)
        : data(&table), hidden(ReadCondition(table, "hidden_unless")),
          states(ReadTables(table["states"].as_array())), values(ReadTables(table["values"].as_array())),
          fallback(ReadTables(table["fallback_values"].as_array())) {
        if (const auto* value = table.get("decode")) decode = ReadDefinition(*value);
        if (const auto* overlays = table["value"].as_table()) {
            for (const auto& [id, overlay] : *overlays)
                if (const auto* fields = overlay.as_table()) {
                    labels.emplace(id.str(), ReadTables((*fields)["label_rules"].as_array()));
                    value_whens.emplace(id.str(), ReadCondition(*fields, "when"));
                }
        }
        for (const auto& value : values) {
            const auto id = (*value.data)["id"].value_or(std::string_view{});
            if (!labels.contains(id))
                labels.emplace(id, ReadTables((*value.data)["label_rules"].as_array()));
        }
    }

    const toml::table* data;
    std::shared_ptr<const Program> hidden;
    std::vector<ConditionalTable> states;
    std::vector<ConditionalTable> values;
    std::vector<ConditionalTable> fallback;
    std::map<std::string, std::vector<ConditionalTable>, std::less<>> labels;
    std::map<std::string, std::shared_ptr<const Program>, std::less<>> value_whens;
    Def decode;
};

struct RowRules {
    explicit RowRules(const toml::table& table) : base(table) {
        for (const auto& variant : ReadTables(table["variants"].as_array()))
            variants.emplace_back(variant.when, RuleFields(*variant.data));
    }
    const RuleFields* Variant(const Env& env) const {
        const auto* match = FirstMatch(variants, env, [](const auto& entry) { return entry.first.get(); });
        return match ? &match->second : nullptr;
    }
    RuleFields base;
    std::vector<std::pair<std::shared_ptr<const Program>, RuleFields>> variants;
};

// The source document owns the tables referenced by the compiled rules.
class RuleBook {
 public:
    explicit RuleBook(std::string_view source) : source_(toml::parse(source)) {
        if (const auto* defs = source_["defs"].as_table())
            for (const auto& [id, rule] : *defs) defs_.emplace(id.str(), ReadDefinition(rule));
        if (const auto* rows = source_["rows"].as_table())
            for (const auto& [id, rule] : *rows)
                if (const auto* table = rule.as_table()) rows_.try_emplace(std::string(id.str()), *table);
        if (const auto* sets = source_["value_sets"].as_table())
            for (const auto& [name, set] : *sets)
                if (const auto* table = set.as_table()) sets_.try_emplace(std::string(name.str()), *table);
    }
    RuleBook(const RuleBook&) = delete;
    RuleBook& operator=(const RuleBook&) = delete;

    const RowRules* Row(std::string_view id) const {
        const auto it = rows_.find(id);
        return it == rows_.end() ? nullptr : &it->second;
    }
    const Def* Definition(std::string_view id) const {
        const auto it = defs_.find(id);
        return it == defs_.end() ? nullptr : &it->second;
    }
    const RuleFields* ValueSet(std::string_view id) const {
        const auto it = sets_.find(id);
        return it == sets_.end() ? nullptr : &it->second;
    }

 private:
    const toml::table source_;
    std::map<std::string, Def, std::less<>> defs_;
    std::map<std::string, RowRules, std::less<>> rows_;
    std::map<std::string, RuleFields, std::less<>> sets_;
};

// A view over one row's base and selected variant. Fields omitted by the
// variant keep their base value, including an explicitly empty state list.
class RuleView {
 public:
    RuleView(const RowRules& row, const Env& env, const RuleBook* book = nullptr)
        : base_(row.base), variant_(row.Variant(env)), env_(env),
          values_ref_(book ? book->ValueSet(Field("values_ref").value_or(std::string_view{})) : nullptr) {}

    const RuleFields& Owner(std::string_view field) const {
        return variant_ && variant_->data->contains(field) ? *variant_ : base_;
    }
    toml::node_view<const toml::node> Field(std::string_view field) const {
        return (*Owner(field).data)[field];
    }
    const toml::table* State() const { return MatchedTable(Owner("states").states, env_); }
    const toml::table* SelectedVariant() const { return variant_ ? variant_->data : nullptr; }
    bool IsListed() const {
        const auto& condition = Owner("hidden_unless").hidden;
        return !condition || ListedUnlessFalse(Evaluate(*condition, env_));
    }
    std::vector<const toml::table*> Choices() const {
        std::vector<const toml::table*> result;
        const auto& values = values_ref_ ? values_ref_->values : Field("values_from") ?
                             Owner("fallback_values").fallback : Owner("values").values;
        for (const auto& value : values) {
            const auto id = (*value.data)["id"].value_or(std::string_view{});
            const Program* when = value.when.get();
            for (const auto* owner : {variant_, &base_}) {
                if (!owner) continue;
                const auto override = owner->value_whens.find(id);
                if (override != owner->value_whens.end() && override->second) {
                    when = override->second.get();
                    break;
                }
            }
            if (!when || ListedUnlessFalse(Evaluate(*when, env_)))
                result.push_back(value.data);
        }
        return result;
    }
    const toml::table* Overlay(std::string_view id) const {
        if (variant_)
            if (const auto* item = (*variant_->data)["value"][id].as_table()) return item;
        return (*base_.data)["value"][id].as_table();
    }
    std::optional<std::string> Label(std::string_view id, const toml::table* entry = nullptr,
                                      std::string_view fallback = {}, std::string_view setting = {},
                                      std::optional<double> number = {}, const char* format = "%g") const {
        const auto* overlay = Overlay(id);
        const auto text = [&](const toml::table* table, std::string_view field) {
            return table ? (*table)[field].value<std::string_view>() : std::nullopt;
        };
        for (const RuleFields* owner : {variant_, &base_, values_ref_}) {
            if (!owner) continue;
            const auto it = owner->labels.find(id);
            if (it == owner->labels.end()) continue;
            if (auto label = text(MatchedTable(it->second, env_), "label"))
                if (auto expanded = ExpandTemplate(*label, env_, number, id)) return expanded;
            break;
        }
        const auto auto_label = [&](const toml::table* table) {
            return table && (*table)["auto_label"].value_or(false);
        };
        if ((auto_label(overlay) || auto_label(entry)) && !setting.empty()) {
            const Value value = Evaluate(Parse("auto_of('" + std::string(setting) + "')"), env_);
            if (!value.IsNull()) {
                if (value.kind == Value::Kind::kNumber)
                    return ExpandTemplate("AUTO ({resolved:format})", env_, number, id, value, format);
                std::string label = ValueText(value);
                for (char& ch : label) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                return "AUTO (" + label + ")";
            }
        }
        if (auto label = text(overlay, "label")) return ExpandTemplate(*label, env_, number, id);
        if (auto label = text(entry, "label")) return ExpandTemplate(*label, env_, number, id);
        if (!fallback.empty()) return std::string(fallback);
        return {};
    }

 private:
    const RuleFields& base_;
    const RuleFields* variant_;
    const Env& env_;
    const RuleFields* values_ref_;
};

inline Value Decode(const Def& decode, const Env& env) {
    if (decode.expr) return Evaluate(*decode.expr, env);
    if (const auto* rule = FirstMatch(decode.cases, env, [](const auto& entry) { return entry.when.get(); }))
        return rule->expr ? Evaluate(*rule->expr, env) : rule->literal;
    return {};
}

}  // namespace larecomp::uiexpr
