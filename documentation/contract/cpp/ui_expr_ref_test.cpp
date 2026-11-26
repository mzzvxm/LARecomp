// A standalone check of the reference evaluator (ui_expr_ref.h) against the shared test vectors (documentation/contract/condition-tests.json), which the
// launcher's C# evaluator and the game's own evaluator must pass too, and against the golden example files for the row rules (states, hidden_unless,
// value `when`, label_rules).
//
//   clang++ -std=c++20 -O1 -I documentation/contract/cpp documentation/contract/cpp/ui_expr_ref_test.cpp -o ui_expr_ref_test.exe
//   ui_expr_ref_test.exe [<documentation/contract folder>]      (default: the folder two levels above this file; exit 0 = passed)
//
// No game, no SDK, no window. Run documentation/contract/cpp/mutation_check.py to see the vectors catch a broken evaluator.

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mini_json.h"
#include "ui_expr_ref.h"

using larecomp::minijson::Json;
using namespace larecomp::uiexpr;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
      ++g_failed;                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

void Fail(const std::string& id, const std::string& what) {
  ++g_checks;
  ++g_failed;
  std::printf("FAIL %s: %s\n", id.c_str(), what.c_str());
}
void Pass() { ++g_checks; }

// ---- JSON -> Value ---------------------------------------------------------------------------------------------------------------------

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
    default: return Value::Null();   // null, and an object is not a value
  }
}

std::string Show(const Value& v) {
  switch (v.kind) {
    case Value::Kind::kNull: return "null";
    case Value::Kind::kBool: return v.b ? "true" : "false";
    case Value::Kind::kNumber: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", v.n);
      return buf;
    }
    case Value::Kind::kString: return "'" + v.s + "'";
    case Value::Kind::kList: {
      std::string r = "[";
      for (size_t i = 0; i < v.list.size(); ++i) r += (i ? "," : "") + Show(v.list[i]);
      return r + "]";
    }
  }
  return "?";
}

bool SameValue(const Value& a, const Value& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == Value::Kind::kNull) return true;
  if (a.kind == Value::Kind::kList) {
    if (a.list.size() != b.list.size()) return false;
    for (size_t i = 0; i < a.list.size(); ++i) {
      if (!SameValue(a.list[i], b.list[i])) return false;
    }
    return true;
  }
  return SameScalar(a, b);
}

std::vector<std::string> Strings(const Json* j) {
  std::vector<std::string> r;
  if (j && j->IsArray()) {
    for (const Json& e : j->a) r.push_back(e.s);
  }
  return r;
}

// ---- The environment a test context describes ------------------------------------------------------------------------------------------

std::map<std::string, Value> Cells(const Json* j) {
  std::map<std::string, Value> r;
  if (j && j->IsObject()) {
    for (const auto& kv : j->o) r[kv.first] = ToValue(&kv.second);
  }
  return r;
}

class JsonEnv final : public Env {
 public:
  // `ctx`: { renderer, facts, config, settings, defs, presets }; `doc`: a settings-ui.json whose settings/defs/presets fill what ctx lacks.
  JsonEnv(const Json& ctx, const Json* doc) : ctx_(ctx) {
    const Json* settings = ctx.Find("settings") ? ctx.Find("settings") : (doc ? doc->Find("settings") : nullptr);
    const Json* defs = ctx.Find("defs") ? ctx.Find("defs") : (doc ? doc->Find("defs") : nullptr);
    const Json* presets = ctx.Find("presets") ? ctx.Find("presets") : (doc ? doc->Find("presets") : nullptr);
    if (settings && settings->IsObject()) {
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
    if (defs && defs->IsObject()) {
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
    if (presets && presets->IsObject()) {
      if (const Json* c2p = presets->Find("class_to_preset")) {
        for (const Json& e : c2p->a) preset_table_.class_to_preset.emplace_back(std::make_shared<Program>(Parse(e.StringOr("when", "null"))), e.StringOr("preset", ""));
      }
      if (const Json* items = presets->Find("items")) {
        for (const auto& kv : items->o) {
          if (const Json* vals = kv.second.Find("values")) {
            std::map<std::string, Value> typed;   // a preset's value is the file's text: typed by its setting, like Setting()
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
    const Json* config = ctx_.Find("config");
    const Json* line = config ? config->Find(name) : nullptr;
    Value raw = line ? ToValue(line) : it->second.default_value;
    return TypedSetting(it->second.type, raw);
  }
  Value Fact(std::string_view path) const override {
    const Json* facts = ctx_.Find("facts");
    if (!facts) return Value::Null();
    return ToValue(facts->Path(path));
  }
  Value Renderer() const override { return ToValue(ctx_.Find("renderer")); }
  const AutoDescriptor* Auto(std::string_view setting) const override {
    auto it = settings_.find(std::string(setting));
    return it != settings_.end() && it->second.has_auto ? &it->second.auto_descriptor : nullptr;
  }
  const Def* DefOf(std::string_view name) const override {
    auto it = defs_.find(std::string(name));
    return it == defs_.end() ? nullptr : &it->second;
  }
  const PresetTable* Presets() const override { return has_presets_ ? &preset_table_ : nullptr; }

 private:
  struct Entry {
    std::string type;
    Value default_value;
    bool has_auto = false;
    AutoDescriptor auto_descriptor;
  };
  const Json& ctx_;
  std::map<std::string, Entry> settings_;
  std::map<std::string, Def> defs_;
  PresetTable preset_table_;
  bool has_presets_ = false;
};

// ---- The vectors ---------------------------------------------------------------------------------------------------------------------

std::string g_dir;

bool Load(const std::string& rel, Json* out) {
  std::string text, err;
  if (!larecomp::minijson::ReadFile(g_dir + "/" + rel, &text)) {
    Fail(rel, "cannot read the file");
    return false;
  }
  if (!larecomp::minijson::Parse(text, out, &err)) {
    Fail(rel, "not JSON: " + err);
    return false;
  }
  return true;
}

// A context is { base?, renderer?, facts?, config?, settings?, defs?, presets? }. `base` names an entry of `contexts` (which may have a base itself);
// the rest is merged over it like a JSON merge patch (RFC 7396): objects merge key by key, anything else replaces, and a null deletes the key.
void MergeOver(Json* target, const Json& patch) {
  if (!patch.IsObject()) {
    *target = patch;
    return;
  }
  if (!target->IsObject()) {
    *target = Json{};
    target->type = Json::Type::kObject;
  }
  for (const auto& kv : patch.o) {
    if (kv.first == "base") continue;
    size_t at = target->o.size();
    for (size_t i = 0; i < target->o.size(); ++i) {
      if (target->o[i].first == kv.first) at = i;
    }
    if (kv.second.IsNull()) {
      if (at < target->o.size()) target->o.erase(target->o.begin() + long(at));
    } else if (at < target->o.size()) {
      MergeOver(&target->o[at].second, kv.second);
    } else {
      Json fresh;
      MergeOver(&fresh, kv.second);
      target->o.emplace_back(kv.first, std::move(fresh));
    }
  }
}

Json ResolveContext(const Json& tests, const Json& ctx, int depth) {
  Json result;
  result.type = Json::Type::kObject;
  if (depth > 8) return result;
  if (ctx.IsString()) {
    const Json* named = tests.Path("contexts." + ctx.s);
    return named ? ResolveContext(tests, *named, depth + 1) : result;
  }
  if (const Json* base = ctx.Find("base")) result = ResolveContext(tests, *base, depth + 1);
  MergeOver(&result, ctx);
  return result;
}

std::deque<Json> g_contexts;   // resolved contexts, kept alive for the run (a deque does not move its elements)

const Json* ContextOf(const Json& tests, const Json& c) {
  const Json* ctx = c.Find("ctx");
  Json empty;
  empty.type = Json::Type::kString;
  empty.s = "empty";
  g_contexts.push_back(ResolveContext(tests, ctx ? *ctx : empty, 0));
  return &g_contexts.back();
}

void RunExpressionCases(const Json& tests) {
  const Json* cases = tests.Find("cases");
  CHECK(cases && cases->IsArray() && cases->a.size() >= 100);
  if (!cases) return;
  for (const Json& c : cases->a) {
    std::string id = c.StringOr("id", "?");
    const Json* ctx = ContextOf(tests, c);
    if (!ctx) {
      Fail(id, "unknown context");
      continue;
    }
    JsonEnv env(*ctx, tests.Find("shared"));
    Program p = Parse(c.StringOr("expr", ""));
    bool want_syntax_error = c.BoolOr("syntax_error", false);
    if (p.ok == want_syntax_error) {
      Fail(id, std::string("parse ") + (p.ok ? "worked" : "failed: " + p.error) + ", expected the opposite");
      continue;
    }
    if (c.BoolOr("unsupported_call", false) != p.unsupported_call) {
      Fail(id, "unsupported_call differs");
      continue;
    }
    if (const Json* want = c.Find("paths")) {
      if (Strings(want) != p.paths) {
        Fail(id, "paths differ");
        continue;
      }
    }
    if (const Json* want = c.Find("unguarded_live")) {
      if (Strings(want) != p.unguarded_live) {
        Fail(id, "unguarded_live differs");
        continue;
      }
    }
    if (c.Find("expect") || want_syntax_error || p.unsupported_call) {
      Value got = Evaluate(p, env);
      Value want = ToValue(c.Find("expect"));
      if (!SameValue(got, want)) {
        Fail(id, "expr '" + c.StringOr("expr", "") + "' gave " + Show(got) + ", expected " + Show(want));
        continue;
      }
      if (const Json* cond = c.Find("cond")) {
        // The place a result is used: cond is the answer of Matches / ListedUnlessFalse for true/false/null, as {"matches": b, "listed": b}.
        if (Matches(got) != cond->BoolOr("matches", Matches(got)) || ListedUnlessFalse(got) != cond->BoolOr("listed", ListedUnlessFalse(got))) {
          Fail(id, "cond differs");
          continue;
        }
      }
    }
    Pass();
  }
}

// ---- Row rules over the golden example files --------------------------------------------------------------------------------------

const Json* FindRow(const Json& doc, const std::string& id) {
  const Json* tabs = doc.Find("tabs");
  if (!tabs) return nullptr;
  for (const Json& tab : tabs->a) {
    const Json* sections = tab.Find("sections");
    if (!sections) continue;
    for (const Json& sec : sections->a) {
      const Json* rows = sec.Find("rows");
      if (!rows) continue;
      for (const Json& row : rows->a) {
        if (row.StringOr("id", "") == id) return &row;
      }
    }
  }
  return nullptr;
}

const Json* FindValue(const Json* values, const std::string& id) {
  if (!values || !values->IsArray()) return nullptr;
  for (const Json& v : values->a) {
    if (v.StringOr("id", "") == id) return &v;
  }
  return nullptr;
}

std::map<std::string, std::unique_ptr<Json>> g_docs;

const Json* Doc(const std::string& rel) {
  auto it = g_docs.find(rel);
  if (it != g_docs.end()) return it->second.get();
  auto doc = std::make_unique<Json>();
  if (!Load(rel, doc.get())) return nullptr;
  return (g_docs[rel] = std::move(doc)).get();
}

Value EvalText(const std::string& text, const Env& env) { return Evaluate(Parse(text), env); }

void RunRowCases(const Json& tests) {
  if (const Json* cases = tests.Find("state_cases")) {
    CHECK(cases->a.size() >= 10);
    for (const Json& c : cases->a) {
      std::string id = c.StringOr("id", "?");
      const Json* doc = Doc(c.StringOr("file", ""));
      const Json* ctx = ContextOf(tests, c);
      const Json* row = doc ? FindRow(*doc, c.StringOr("row", "")) : nullptr;
      if (!doc || !ctx || !row) {
        Fail(id, "file, context or row not found");
        continue;
      }
      JsonEnv env(*ctx, doc);
      const Json* states = row->Find(c.StringOr("list", "states"));
      int matched = -1;
      const Json* state = nullptr;
      if (states) {
        for (size_t i = 0; i < states->a.size(); ++i) {
          const Json* when = states->a[i].Find("when");
          if (when && Matches(EvalText(when->s, env))) {
            matched = int(i);
            state = &states->a[i];
            break;
          }
        }
      }
      const Json* expect = c.Find("expect");
      bool ok = true;
      const Json* want_state = expect->Find("state");
      if (!want_state || (want_state->IsNull() ? matched != -1 : matched != int(want_state->n))) ok = false;
      bool enabled = state ? EnabledOr(ToValue(state->Find("enabled"))) : true;
      std::string label = state && state->Find("label") ? state->Find("label")->s : row->StringOr("label", "");
      std::string suffix = state ? state->StringOr("suffix", "") : "";
      if (const Json* e = expect->Find("enabled")) ok = ok && e->b == enabled;
      if (const Json* e = expect->Find("label")) ok = ok && e->s == label;
      if (const Json* e = expect->Find("suffix")) ok = ok && e->s == suffix;
      if (const Json* e = expect->Find("tag")) ok = ok && state && state->StringOr("tag", "") == e->s;
      if (const Json* e = expect->Find("needs_restart")) ok = ok && state && state->BoolOr("needs_restart", false) == e->b;
      if (ok) Pass();
      else Fail(id, "matched state " + std::to_string(matched) + ", enabled " + (enabled ? "yes" : "no") + ", label '" + label + "', suffix '" + suffix + "'");
    }
  }
  if (const Json* cases = tests.Find("decode_cases")) {
    CHECK(cases->a.size() >= 8);
    for (const Json& c : cases->a) {
      std::string id = c.StringOr("id", "?");
      const Json* doc = Doc(c.StringOr("file", ""));
      const Json* ctx = ContextOf(tests, c);
      const Json* row = doc ? FindRow(*doc, c.StringOr("row", "")) : nullptr;
      const Json* decode = row ? row->Find("decode") : nullptr;
      if (!doc || !ctx || !decode) {
        Fail(id, "file, context, row or decode not found");
        continue;
      }
      JsonEnv env(*ctx, doc);
      Value got;
      if (decode->IsString()) {
        got = EvalText(decode->s, env);                     // "decode": "d.some_def"
      } else if (const Json* list = decode->Find("cases")) {
        for (const Json& k : list->a) {                     // first case whose when is true (a case with no when always matches)
          const Json* when = k.Find("when");
          if (!when || Matches(EvalText(when->s, env))) {
            const Json* value = k.Find("value");
            // a literal id, or {"expr": "..."}: the value is computed (as a def case's value is), e.g. the setting's own text or the effective number of a legacy 0
            got = value && value->IsObject() && value->Find("expr") ? EvalText(value->Find("expr")->s, env) : ToValue(value);
            break;
          }
        }
      }
      if (got.kind == Value::Kind::kString && got.s == c.StringOr("expect_value", "")) Pass();
      else Fail(id, "decoded " + Show(got));
    }
  }
  if (const Json* cases = tests.Find("listing_cases")) {
    CHECK(cases->a.size() >= 8);
    for (const Json& c : cases->a) {
      std::string id = c.StringOr("id", "?");
      const Json* doc = Doc(c.StringOr("file", ""));
      const Json* ctx = ContextOf(tests, c);
      if (!doc || !ctx) {
        Fail(id, "file or context not found");
        continue;
      }
      const Json* holder = nullptr;   // the row, or the value set, that holds the value
      if (c.Find("row")) holder = FindRow(*doc, c.StringOr("row", ""));
      const Json* when_owner = holder;
      if (c.Find("value")) {
        const Json* values = nullptr;
        if (c.Find("value_set")) values = doc->Path("value_sets." + c.StringOr("value_set", ""));
        else if (holder) values = holder->Find("values");
        when_owner = FindValue(values, c.StringOr("value", ""));
      }
      if (!when_owner) {
        Fail(id, "row or value not found");
        continue;
      }
      JsonEnv env(*ctx, doc);
      const Json* when = when_owner->Find(c.Find("value") ? "when" : "hidden_unless");
      bool listed = when ? ListedUnlessFalse(EvalText(when->s, env)) : true;
      if (listed == c.BoolOr("expect_listed", !listed)) Pass();
      else Fail(id, std::string("listed ") + (listed ? "yes" : "no"));
    }
  }
  if (const Json* cases = tests.Find("label_cases")) {
    CHECK(cases->a.size() >= 5);
    for (const Json& c : cases->a) {
      std::string id = c.StringOr("id", "?");
      const Json* doc = Doc(c.StringOr("file", ""));
      const Json* ctx = ContextOf(tests, c);
      if (!doc || !ctx) {
        Fail(id, "file or context not found");
        continue;
      }
      const Json* values = nullptr;
      if (c.Find("value_set")) values = doc->Path("value_sets." + c.StringOr("value_set", ""));
      else if (const Json* row = FindRow(*doc, c.StringOr("row", ""))) values = row->Find("values");
      const Json* value = FindValue(values, c.StringOr("value", ""));
      if (!value) {
        Fail(id, "value not found");
        continue;
      }
      JsonEnv env(*ctx, doc);
      std::string label = value->StringOr("label", "");
      if (const Json* rules = value->Find("label_rules")) {
        for (const Json& rule : rules->a) {
          if (Matches(EvalText(rule.StringOr("when", "null"), env))) {
            label = rule.StringOr("label", label);
            break;
          }
        }
      }
      if (label == c.StringOr("expect_label", "\x01")) Pass();
      else Fail(id, "label '" + label + "'");
    }
  }
}

// ---- A few direct checks of what the vectors cannot say (the API itself) -----------------------------------------------------------------

void TestApi() {
  Program p = Parse("gpu.class == 'high' && (live.x ?? s.y)");
  CHECK(p.ok && !p.unsupported_call);
  CHECK((p.paths == std::vector<std::string>{"gpu.class", "live.x", "s.y"}));
  CHECK(p.unguarded_live.empty());
  CHECK(Parse("live.x == 1 ?? true").unguarded_live.size() == 1);   // ?? binds tighter than ==: the live path is not inside a `??` operand
  CHECK(!Parse("a ==").ok && !Parse("").ok && !Parse("(1").ok && !Parse("1 2").ok && !Parse("'x").ok);
  CHECK(TypedSetting("bool", Value::String("true")).IsTrue());
  CHECK(TypedSetting("bool", Value::String("yes")).IsNull());
  CHECK(TypedSetting("int", Value::Number(1.5)).IsNull());
  CHECK(TypedSetting("int", Value::String("60")).n == 60);
  CHECK(TypedSetting("number", Value::String("1.5")).n == 1.5);
  CHECK(TypedSetting("string", Value::Number(1)).IsNull());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    g_dir = argv[1];
  } else {
    g_dir = __FILE__;
    for (int i = 0; i < 2; ++i) {   // .../contract/cpp/ui_expr_ref_test.cpp -> .../contract
      size_t cut = g_dir.find_last_of("/\\");
      g_dir = cut == std::string::npos ? "." : g_dir.substr(0, cut);
    }
  }
  Json tests;
  if (Load("condition-tests.json", &tests)) {
    CHECK(tests.StringOr("schema", "") == "larecomp.condition-tests");
    CHECK(tests.NumberOr("expr_version", 0) == 1);
    RunExpressionCases(tests);
    RunRowCases(tests);
  }
  TestApi();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
