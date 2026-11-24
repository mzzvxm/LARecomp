// Cross-check vectors for the launcher (LAUNCHER-FOLLOWS-GAME section 6, agent XT).
//
//   clang++ -std=c++23 -Isrc src/settings/ui_xcheck_vectors_test.cpp -o ui_xcheck_vectors
//   ui_xcheck_vectors <settings-ui.json> <fleet folder> <vectors.json>
//
// The question the launcher's renderer lives on: for the same machine facts and the same settings, does the launcher's evaluator say what the game's says? This program asks the
// GAME's evaluator (src/settings/ui_expr.h, the one the pause menu runs). It reads a real dump of the game's settings UI (settings-ui.json, from tools\check_settings_dump.py),
// collects every expression the dump holds (every `when`, `hidden_unless`, `decode`, `needs` and `expr`, every def, the auto cells of every setting that has them), and evaluates
// each one over a grid: every machine of the fleet (documentation/contract/examples/fleet/*.machine.json) x both renderers x a set of settings assignments (the defaults, every
// setting a rule reads at its first and at its last value, and seeded random picks). The launcher's test (tests\LARecomp.Launcher.Core.Tests\ContractCrossCheckTests.cs) reads
// the vectors file and evaluates the same grid with its own evaluator; one differing result is one failing test with the expression, the machine and the settings named.
//
// The rules the game's evaluator and the launcher's share: live.* is null for both (the launcher never has the running game), a setting the file does not set reads the catalog's
// default, text from the file is typed by the catalog's type. Nothing here is the game's evaluator under another name: this file only builds the host (Env) from the JSON.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../../documentation/contract/cpp/mini_json.h"
#include "ui_expr.h"

using larecomp::minijson::Json;
using namespace larecomp::uiexpr;

namespace {

namespace fs = std::filesystem;

Value ToValue(const Json* j) {
  if (!j) return Value::Null();
  switch (j->type) {
    case Json::Type::kBool: return Value::Bool(j->b);
    case Json::Type::kNumber: return Value::Number(j->n);
    case Json::Type::kString: return Value::String(j->s);
    case Json::Type::kArray: {
      std::vector<Value> items;
      for (const Json& e : j->a) items.push_back(ToValue(&e));
      return Value::List(std::move(items));
    }
    default: return Value::Null();
  }
}

std::map<std::string, Value> Cells(const Json* j) {
  std::map<std::string, Value> r;
  if (j && j->IsObject()) {
    for (const auto& kv : j->o) r[kv.first] = ToValue(&kv.second);
  }
  return r;
}

std::string Escape(const std::string& s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          r += buf;
        } else {
          r += char(c);
        }
    }
  }
  return r + "\"";
}

// A value as JSON text: also the key the distinct-values table is built on.
std::string JsonOf(const Value& v) {
  switch (v.kind) {
    case Value::Kind::kNull: return "null";
    case Value::Kind::kBool: return v.b ? "true" : "false";
    case Value::Kind::kNumber: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", v.n);
      return buf;
    }
    case Value::Kind::kString: return Escape(v.s);
    case Value::Kind::kList: {
      std::string r = "[";
      for (size_t i = 0; i < v.list.size(); ++i) r += (i ? "," : "") + JsonOf(v.list[i]);
      return r + "]";
    }
  }
  return "null";
}

// The host the pause menu has, built from a dump and one machine file: the same inputs the launcher gives its evaluator.
class DumpEnv final : public Env {
 public:
  struct Entry {
    std::string type;
    Value default_value;
    bool has_auto = false;
    AutoDescriptor auto_descriptor;
  };

  DumpEnv(const Json& doc, const Json* machine, std::string renderer, const std::map<std::string, std::string>* config)
      : machine_(machine), renderer_(std::move(renderer)), config_(config) {
    if (const Json* settings = doc.Find("settings")) {
      for (const auto& kv : settings->o) {
        Entry s;
        s.type = kv.second.StringOr("type", "string");
        s.default_value = ToValue(kv.second.Find("default"));
        if (const Json* a = kv.second.Find("auto")) {
          AutoDescriptor d;
          d.by = a->StringOr("by", "");
          d.cells = Cells(a->Find("cells"));
          d.handheld = Cells(a->Find("handheld"));
          if (const Json* av = a->Find("auto_value")) {
            d.has_auto_value = true;
            d.auto_value = av->IsNumber() ? av->n : 0;
          }
          s.auto_descriptor = d;
          s.has_auto = true;
        }
        settings_[kv.first] = std::move(s);
      }
    }
    if (const Json* defs = doc.Find("defs")) {
      for (const auto& kv : defs->o) {
        Def d;
        if (kv.second.IsString()) {
          d.expr = std::make_shared<Program>(Parse(kv.second.s));
        } else if (const Json* cases = kv.second.Find("cases")) {
          for (const Json& c : cases->a) {
            DefCase dc;
            if (const Json* w = c.Find("when")) dc.when = std::make_shared<Program>(Parse(w->s));
            const Json* v = c.Find("value");
            if (v && v->IsObject() && v->Find("expr")) dc.expr = std::make_shared<Program>(Parse(v->Find("expr")->s));
            else dc.literal = ToValue(v);
            d.cases.push_back(std::move(dc));
          }
        }
        defs_[kv.first] = std::move(d);
      }
    }
    if (const Json* presets = doc.Find("presets"); presets && presets->IsObject()) {
      if (const Json* c2p = presets->Find("class_to_preset")) {
        for (const Json& e : c2p->a) {
          preset_table_.class_to_preset.emplace_back(std::make_shared<Program>(Parse(e.StringOr("when", "null"))), e.StringOr("preset", ""));
        }
      }
      if (const Json* items = presets->Find("items")) {
        for (const auto& kv : items->o) {
          if (const Json* vals = kv.second.Find("values")) {
            std::map<std::string, Value> typed;
            for (const auto& pv : vals->o) {
              auto s = settings_.find(pv.first);
              typed[pv.first] = s == settings_.end() ? Value::Null() : TypedSetting(s->second.type, ToValue(&pv.second));
            }
            preset_table_.values[kv.first] = std::move(typed);
          }
        }
      }
      has_presets_ = true;
    }
  }

  Value Setting(std::string_view name) const override {
    auto it = settings_.find(std::string(name));
    if (it == settings_.end()) return Value::Null();
    auto line = config_ ? config_->find(std::string(name)) : std::map<std::string, std::string>::const_iterator{};
    const bool has_line = config_ && line != config_->end();
    Value raw = has_line ? Value::String(line->second) : it->second.default_value;
    return TypedSetting(it->second.type, raw);
  }
  Value Fact(std::string_view path) const override {
    if (path.substr(0, 5) == "live.") return Value::Null();   // the launcher never has the running game
    return machine_ ? ToValue(machine_->Path(path)) : Value::Null();
  }
  Value Renderer() const override { return Value::String(renderer_); }
  const AutoDescriptor* Auto(std::string_view setting) const override {
    auto it = settings_.find(std::string(setting));
    return it != settings_.end() && it->second.has_auto ? &it->second.auto_descriptor : nullptr;
  }
  const Def* DefOf(std::string_view name) const override {
    auto it = defs_.find(std::string(name));
    return it == defs_.end() ? nullptr : &it->second;
  }
  const PresetTable* Presets() const override { return has_presets_ ? &preset_table_ : nullptr; }

  const std::map<std::string, Entry>& settings() const { return settings_; }

 private:
  const Json* machine_;
  std::string renderer_;
  const std::map<std::string, std::string>* config_;
  std::map<std::string, Entry> settings_;
  std::map<std::string, Def> defs_;
  PresetTable preset_table_;
  bool has_presets_ = false;
};

// ---- Every expression of the dump -------------------------------------------------------------------------------------------------------------

void Collect(const Json& node, const std::string& key, std::vector<std::string>* out) {
  if (node.IsString()) {
    if (key == "when" || key == "hidden_unless" || key == "needs" || key == "expr" || key == "decode") out->push_back(node.s);
    return;
  }
  if (node.IsArray()) {
    for (const Json& e : node.a) Collect(e, key, out);
  } else if (node.IsObject()) {
    for (const auto& kv : node.o) Collect(kv.second, kv.first, out);
  }
}

// ---- A small seeded generator (the standard library's distributions differ between implementations) -------------------------------------------

struct Rng {
  uint64_t x;
  uint64_t Next() {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    return x >> 33;
  }
  size_t Pick(size_t n) { return n ? size_t(Next() % n) : 0; }
};

std::string NumberText(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// The texts a setting may hold in larecomp.toml, for the grid: its allowed words, or the neighbours of a number.
std::vector<std::string> Candidates(const Json& setting) {
  std::vector<std::string> r;
  const std::string type = setting.StringOr("type", "string");
  const std::string def = setting.StringOr("default", "");
  if (type == "bool") return {"true", "false"};
  if (const Json* allowed = setting.Find("allowed"); allowed && allowed->IsArray() && !allowed->a.empty()) {
    for (const Json& e : allowed->a) r.push_back(e.IsString() ? e.s : (e.IsNumber() ? NumberText(e.n) : JsonOf(ToValue(&e))));
    return r;
  }
  if (type == "int" || type == "number") {
    r.push_back(def);
    if (const Json* range = setting.Find("range"); range && range->IsArray() && range->a.size() == 2) {
      r.push_back(NumberText(range->a[0].n));
      r.push_back(NumberText(range->a[1].n));
      r.push_back(NumberText((range->a[0].n + range->a[1].n) / 2));
    } else {
      r.insert(r.end(), {"0", "1", "60", "144"});
    }
    if (const Json* a = setting.Find("auto")) {
      if (const Json* av = a->Find("auto_value"); av && av->IsNumber()) r.push_back(NumberText(av->n));
    }
    return r;
  }
  r.push_back(def);
  if (setting.Find("auto")) r.push_back("auto");
  return r;
}

bool ReadJson(const fs::path& path, Json* out) {
  std::string text, err;
  if (!larecomp::minijson::ReadFile(path.string(), &text) || !larecomp::minijson::Parse(text, out, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", path.string().c_str(), err.c_str());
    return false;
  }
  return true;
}

std::string Code(size_t index) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  return {alphabet[(index >> 6) & 63], alphabet[index & 63]};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: ui_xcheck_vectors <settings-ui.json> <fleet folder> <vectors.json>\n");
    return 2;
  }
  Json doc;
  if (!ReadJson(argv[1], &doc)) return 1;
  std::vector<std::pair<std::string, Json>> machines;
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(argv[2])) {
    if (entry.path().filename().string().ends_with(".machine.json")) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& f : files) {
    Json m;
    if (!ReadJson(f, &m)) return 1;
    std::string id = f.filename().string();
    id.resize(id.size() - std::string(".machine.json").size());
    machines.emplace_back(id, std::move(m));
  }
  if (machines.empty()) {
    std::fprintf(stderr, "no *.machine.json in %s\n", argv[2]);
    return 1;
  }

  // Expressions: every one the dump holds, then the defs by name, then the auto cell of every setting that has one.
  std::vector<std::string> raw;
  Collect(doc, "", &raw);
  std::set<std::string> unique(raw.begin(), raw.end());
  if (const Json* defs = doc.Find("defs")) {
    for (const auto& kv : defs->o) {
      unique.insert("d." + kv.first);
      if (kv.second.IsString()) unique.insert(kv.second.s);   // a def that is one expression: its own text reads settings too
    }
  }
  if (const Json* settings = doc.Find("settings")) {
    for (const auto& kv : settings->o) {
      if (kv.second.Find("auto")) {
        unique.insert("resolved(s." + kv.first + ")");
        unique.insert("auto_of('" + kv.first + "')");
      }
    }
  }
  // A decode written as a text is an expression too (rows with several settings); a cases object has its own `when`s, collected above.
  std::vector<std::string> expressions(unique.begin(), unique.end());
  std::vector<Program> programs;
  size_t unparsed = 0;
  std::set<std::string> read_settings;
  for (const std::string& text : expressions) {
    programs.push_back(Parse(text));
    if (!programs.back().ok) ++unparsed;
    for (const std::string& path : programs.back().paths) {
      if (path.rfind("s.", 0) == 0) read_settings.insert(path.substr(2));
    }
  }
  for (const std::string& text : expressions) {
    // resolved(s.x) / auto_of('x') read the catalog's descriptor of x: its value matters too.
    if (text.rfind("resolved(s.", 0) == 0) read_settings.insert(text.substr(11, text.size() - 12));
  }

  const Json* catalog = doc.Find("settings");
  struct Variable { std::string name; std::vector<std::string> values; };
  std::vector<Variable> variables;
  for (const std::string& name : read_settings) {
    const Json* s = catalog ? catalog->Find(name) : nullptr;
    if (!s) continue;
    auto values = Candidates(*s);
    std::vector<std::string> distinct;
    for (const std::string& v : values) {
      if (std::find(distinct.begin(), distinct.end(), v) == distinct.end()) distinct.push_back(v);
    }
    variables.push_back({name, distinct});
  }

  constexpr int kConfigs = 10;
  std::vector<std::string> value_table;                // distinct result values as JSON text
  std::map<std::string, size_t> value_index;
  auto Index = [&](const std::string& json) {
    auto it = value_index.find(json);
    if (it != value_index.end()) return it->second;
    value_index[json] = value_table.size();
    value_table.push_back(json);
    return value_table.size() - 1;
  };

  std::string out;
  out += "{\n  \"schema\": \"larecomp.xcheck-vectors\",\n  \"version\": 1,\n";
  const Json* generated = doc.Find("generated");
  out += "  \"dump\": { \"settings_hash\": " + Escape(generated ? generated->StringOr("settings_hash", "") : "") +
         ", \"game_version\": " + Escape(generated ? generated->StringOr("game_version", "") : "") + " },\n";
  out += "  \"expressions\": [\n";
  for (size_t i = 0; i < expressions.size(); ++i) out += "    " + Escape(expressions[i]) + (i + 1 < expressions.size() ? ",\n" : "\n");
  out += "  ],\n  \"contexts\": [\n";

  size_t contexts = 0, evaluations = 0;
  std::string body;
  for (const auto& [machine_id, machine] : machines) {
    for (const std::string renderer : {"native", "xenia"}) {
      for (int c = 0; c < kConfigs; ++c) {
        std::map<std::string, std::string> config;
        Rng rng{0x9E3779B97F4A7C15ull ^ (uint64_t(c) * 1000003ull) ^ std::hash<std::string>{}(machine_id + renderer)};
        for (const Variable& v : variables) {
          if (c == 0) break;                                              // the defaults: an empty larecomp.toml
          if (c == 1) config[v.name] = v.values.front();
          else if (c == 2) config[v.name] = v.values.back();
          else config[v.name] = v.values[rng.Pick(v.values.size())];
        }
        DumpEnv env(doc, &machine, renderer, &config);
        std::string codes;
        for (const Program& p : programs) {
          codes += Code(Index(JsonOf(Evaluate(p, env))));
          ++evaluations;
        }
        body += std::string(contexts ? ",\n" : "") + "    { \"machine\": " + Escape(machine_id) + ", \"renderer\": " + Escape(renderer) + ", \"config\": {";
        bool first = true;
        for (const auto& [name, text] : config) {
          body += std::string(first ? " " : ", ") + Escape(name) + ": " + Escape(text);
          first = false;
        }
        body += std::string(first ? "" : " ") + "}, \"results\": " + Escape(codes) + " }";
        ++contexts;
      }
    }
  }
  out += body + "\n  ],\n  \"values\": [\n";
  for (size_t i = 0; i < value_table.size(); ++i) out += "    " + value_table[i] + (i + 1 < value_table.size() ? ",\n" : "\n");
  out += "  ]\n}\n";

  if (value_table.size() > 4096) {
    std::fprintf(stderr, "too many distinct values (%zu)\n", value_table.size());
    return 1;
  }
  fs::create_directories(fs::path(argv[3]).parent_path());
  std::ofstream file(argv[3], std::ios::binary | std::ios::trunc);
  file << out;
  file.close();
  std::printf("%zu expressions (%zu do not parse: unknown calls and the like, they evaluate to null), %zu settings varied, %zu machines x 2 renderers x %d settings = %zu contexts, %zu evaluations, %zu distinct results\n",
              expressions.size(), unparsed, variables.size(), machines.size(), kConfigs, contexts, evaluations, value_table.size());
  return file ? 0 : 1;
}
