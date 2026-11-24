// The REFERENCE evaluator of the settings-UI condition language (LAUNCHER-FOLLOWS-GAME section 1.7, expr_version 1).
//
// What this file is: the one place the language's meaning is written as code, small and pure (no game headers, no JSON, no I/O, C++20, header
// only). The pause menu's evaluator (agent RM: src/settings/ui_expr.{h,cpp}) and the launcher's (Core\Contract\UiExpr.cs) must give the same answer as
// this one for every case of documentation/contract/condition-tests.json; the three suites read that one file. RM may copy this header into
// src/settings/ and extend it (more speed, the game's own value type), but the semantics below do not change without a new expr_version and new vectors.
//
// ---- Grammar (the doc's, with the lexical rules it left open) ----------------------------------------------------------------------------------
//   expr     := or
//   or       := and ( '||' and )*
//   and      := not ( '&&' not )*
//   not      := '!' not | cmp
//   cmp      := coalesce ( ( '==' | '!=' | '<' | '<=' | '>' | '>=' ) coalesce | 'in' '[' literal ( ',' literal )* ']' )?     (at most ONE comparison)
//   coalesce := primary ( '??' primary )*                                                                                       (left associative)
//   primary  := literal | path | call | '(' expr ')'
//   literal  := 'text' | "text" | number | true | false | null          text: either quote, \\ \' \" are the only escapes; number: -?digits[.digits][e[+-]digits]
//   path     := name ( '.' name )*          name := [A-Za-z_][A-Za-z0-9_]*      call := name '(' [ expr ( ',' expr )* ] ')'
//   White space (space, tab, CR, LF) separates tokens and is otherwise ignored. true, false, null and in are reserved words as the whole of a primary.
//   A syntax error makes the whole expression null (Program::ok == false, Program::error says where); it is also a lint error in the exporter.
//
// ---- Values ----------------------------------------------------------------------------------------------------------------------------------
//   null, bool, number (a double), string, list (a list of values, from a JSON array in machine.json). A JSON object is not a value (a path that ends on one is null).
//
// ---- Paths (the first word decides) ----------------------------------------------------------------------------------------------------------
//   s.<setting>      Env::Setting(name): the setting's TYPED value (the file's line, else the catalog default); null when the catalog does not know it.
//   d.<name>         a def: an expression, or a cases list (first case whose `when` is true, or has none; its value is a literal or {expr}); null when no case matches.
//                    Defs may read defs; a loop or nesting deeper than 32 is null.
//   renderer         "native" or "xenia" (Env::Renderer), null when the host does not say.
//   gpu.* api.* features.* os.* display.* cpu.* build.* install.* live.*     Env::Fact(whole dotted path); missing facts are null. live.* is null in the launcher.
//   anything else    null (an unknown namespace is not an error at run time; the exporter's lint rejects it).
//
// ---- Operators (three-valued: true, false, null) ---------------------------------------------------------------------------------------------
//   An operand of ! && || that is not a bool counts as null. Both operands are always evaluated (no short cut: there are no side effects).
//   !null = null.   false && x = false, x && false = false; true && true = true; every other && is null.   true || x = true, x || true = true; false || false = false; every other || is null.
//   == and !=:  null on either side, or two different kinds, or a list: null.  Same kind scalars: number == number is exact (IEEE), string == string is exact, case sensitive.
//   < <= > >=:  both numbers: the answer; anything else: null.
//   x in [a, b]: the same as (x == a) || (x == b) || ... under the || above (x null: null; a literal of another kind is a null term).
//   a ?? b:     b when a is null, else a. (A false is not null.)
//
// ---- Functions (closed set; a call to any other name, or with the wrong number of arguments, makes the WHOLE expression null: Program::unsupported_call) --
//   resolved(s.x)    the setting's value with `auto` replaced through its auto descriptor on this PC: argument must be exactly a path s.<name> (else null).
//                    value null -> null.  The value is `auto` when it is the string "auto" (or, when the descriptor has auto_value, the number equal to it).
//                    Not auto -> the value, unchanged. Auto -> the descriptor's cell (below); no descriptor, or a cell that is unknown -> null.
//   auto_of('x')     the same cell whatever the file holds (the argument is an expression that must be a string, else null).
//   known(v)         true when v is not null, else false. Never null.
//   contains(l, x)   l must be a list (null or not a list: null); x null: null; true when an element has x's kind and equals it, else false.
//   min(a, b) max(a, b)   two numbers, else null.
//
//   The cell of a descriptor { by, cells, handheld?, auto_value? }:
//     by "class":   gpu.class null -> null.  When gpu.handheld is true and `handheld` has an entry for the class, that entry; else cells[class]; absent -> null.
//     by "rt_rung": gpu.rt_rung null -> null. cells[rung]; absent -> null.
//     by "preset":  Env::Presets(): the first class_to_preset entry whose `when` is true gives a preset name; that preset's values[setting]; any step missing -> null.
//     any other `by`: null.
//
// ---- Where a result is used (the doc's table; helpers below) ---------------------------------------------------------------------------------
//   hidden_unless / values[].when: ListedUnlessFalse (null -> listed).   states / variants / label_rules / cases when: Matches (null -> no match).   state enabled: EnabledOr (null -> enabled).
//   A result that is not a bool where a bool is needed is null (and a lint error in the exporter).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::uiexpr {

// ---- Value ---------------------------------------------------------------------------------------------------------------------------------

struct Value {
  enum class Kind { kNull, kBool, kNumber, kString, kList };
  Kind kind = Kind::kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> list;

  static Value Null() { return Value{}; }
  static Value Bool(bool v) {
    Value r;
    r.kind = Kind::kBool;
    r.b = v;
    return r;
  }
  static Value Number(double v) {
    Value r;
    r.kind = Kind::kNumber;
    r.n = v;
    return r;
  }
  static Value String(std::string v) {
    Value r;
    r.kind = Kind::kString;
    r.s = std::move(v);
    return r;
  }
  static Value List(std::vector<Value> v) {
    Value r;
    r.kind = Kind::kList;
    r.list = std::move(v);
    return r;
  }

  bool IsNull() const { return kind == Kind::kNull; }
  bool IsTrue() const { return kind == Kind::kBool && b; }
  bool IsFalse() const { return kind == Kind::kBool && !b; }
};

// Same kind and equal; lists are never equal (== on a list is null, see Compare).
inline bool SameScalar(const Value& a, const Value& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case Value::Kind::kBool: return a.b == b.b;
    case Value::Kind::kNumber: return a.n == b.n;
    case Value::Kind::kString: return a.s == b.s;
    default: return false;
  }
}

// A value for a bool operand: a bool stays, everything else is null.
inline Value Tri(const Value& v) { return v.kind == Value::Kind::kBool ? v : Value::Null(); }

inline Value Not(const Value& a) {
  Value t = Tri(a);
  return t.IsNull() ? t : Value::Bool(!t.b);
}
inline Value And(const Value& a, const Value& b) {
  Value x = Tri(a), y = Tri(b);
  if (x.IsFalse() || y.IsFalse()) return Value::Bool(false);
  if (x.IsTrue() && y.IsTrue()) return Value::Bool(true);
  return Value::Null();
}
inline Value Or(const Value& a, const Value& b) {
  Value x = Tri(a), y = Tri(b);
  if (x.IsTrue() || y.IsTrue()) return Value::Bool(true);
  if (x.IsFalse() && y.IsFalse()) return Value::Bool(false);
  return Value::Null();
}

// op: "==" "!=" "<" "<=" ">" ">=".
inline Value Compare(std::string_view op, const Value& a, const Value& b) {
  if (a.IsNull() || b.IsNull() || a.kind != b.kind) return Value::Null();
  if (op == "==" || op == "!=") {
    if (a.kind == Value::Kind::kList) return Value::Null();
    bool eq = SameScalar(a, b);
    return Value::Bool(op == "==" ? eq : !eq);
  }
  if (a.kind != Value::Kind::kNumber) return Value::Null();
  if (op == "<") return Value::Bool(a.n < b.n);
  if (op == "<=") return Value::Bool(a.n <= b.n);
  if (op == ">") return Value::Bool(a.n > b.n);
  if (op == ">=") return Value::Bool(a.n >= b.n);
  return Value::Null();
}

// ---- Result helpers: what null means where a result is used --------------------------------------------------------------------------------

inline bool Matches(const Value& v) { return v.IsTrue(); }                  // states, variants, label_rules, cases: only a true matches
inline bool ListedUnlessFalse(const Value& v) { return !v.IsFalse(); }      // hidden_unless, values[].when: only a false hides (a non-bool is null: listed)
inline bool EnabledOr(const Value& enabled_field_or_null) {                  // a state's `enabled` (null/absent: enabled)
  return !enabled_field_or_null.IsFalse();
}

// ---- Typed setting values (the one place a text becomes a typed value) -------------------------------------------------------------------

// `type` is the catalog's: bool | int | number | string | path. `raw` is what the file line / catalog default holds: already typed (a JSON bool / number /
// string) or the text of a TOML value. A raw that does not fit the type is null (never guessed).
inline bool ParseNumberText(std::string_view t, double* out) {
  if (t.empty()) return false;
  size_t i = 0;
  if (t[i] == '-') ++i;
  size_t d = i;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
  if (i == d) return false;
  if (i < t.size() && t[i] == '.') {
    ++i;
    size_t f = i;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
    if (i == f) return false;
  }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    ++i;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
    size_t e = i;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
    if (i == e) return false;
  }
  if (i != t.size()) return false;
  *out = std::strtod(std::string(t).c_str(), nullptr);
  return true;
}

inline Value TypedSetting(std::string_view type, const Value& raw) {
  if (raw.IsNull()) return Value::Null();
  if (type == "bool") {
    if (raw.kind == Value::Kind::kBool) return raw;
    if (raw.kind == Value::Kind::kString && raw.s == "true") return Value::Bool(true);
    if (raw.kind == Value::Kind::kString && raw.s == "false") return Value::Bool(false);
    return Value::Null();
  }
  if (type == "int" || type == "number") {
    double n = 0;
    if (raw.kind == Value::Kind::kNumber) n = raw.n;
    else if (raw.kind == Value::Kind::kString && ParseNumberText(raw.s, &n)) {}
    else return Value::Null();
    if (type == "int" && n != std::floor(n)) return Value::Null();
    return Value::Number(n);
  }
  if (type == "string" || type == "path") {
    return raw.kind == Value::Kind::kString ? raw : Value::Null();
  }
  return Value::Null();
}

// ---- Parsed programs -----------------------------------------------------------------------------------------------------------------------

struct Node {
  enum class Kind { kLiteral, kPath, kCall, kNot, kAnd, kOr, kCmp, kIn, kCoalesce };
  Kind kind = Kind::kLiteral;
  Value literal;                           // kLiteral
  std::vector<std::string> segs;           // kPath: the dotted words
  std::string name;                        // kCall: the function; kCmp: the operator
  std::vector<std::shared_ptr<Node>> kids; // operands / arguments; kIn: kids[0] is the tested expression
  std::vector<Value> in_list;              // kIn: the literals
};

struct Program {
  std::shared_ptr<Node> root;
  bool ok = false;                         // parsed
  std::string error;                       // when !ok
  bool unsupported_call = false;           // a call to a name outside the v1 set, or with the wrong number of arguments: the whole expression is null
  std::vector<std::string> paths;          // every path the text names, dotted, sorted, no duplicates (s.x, d.x, gpu.class, renderer, ...)
  std::vector<std::string> unguarded_live; // live.* paths not inside the left operand of a `??` (the exporter's lint 3)
};

namespace detail {

struct Tok {
  enum class T { kEnd, kIdent, kString, kNumber, kPunct };
  T t = T::kEnd;
  std::string text;                        // kPunct: the operator; kString: the text; kIdent: the first word
  std::vector<std::string> segs;           // kIdent: all dotted words
  double num = 0;
  size_t pos = 0;
};

inline bool IsIdentStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
inline bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

inline bool Tokenize(std::string_view s, std::vector<Tok>* out, std::string* err) {
  size_t i = 0;
  auto fail = [&](const std::string& what, size_t at) {
    *err = what + " at column " + std::to_string(at + 1);
    return false;
  };
  while (true) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    Tok tok;
    tok.pos = i;
    if (i >= s.size()) {
      tok.t = Tok::T::kEnd;
      out->push_back(tok);
      return true;
    }
    char c = s[i];
    if (c == '\'' || c == '"') {
      char q = c;
      ++i;
      std::string text;
      bool closed = false;
      while (i < s.size()) {
        char d = s[i++];
        if (d == q) {
          closed = true;
          break;
        }
        if (d == '\\') {
          if (i >= s.size()) return fail("unterminated text", tok.pos);
          char e = s[i++];
          if (e == '\\' || e == '\'' || e == '"') text.push_back(e);
          else return fail("unknown escape", i - 2);
        } else {
          text.push_back(d);
        }
      }
      if (!closed) return fail("unterminated text", tok.pos);
      tok.t = Tok::T::kString;
      tok.text = std::move(text);
      out->push_back(tok);
      continue;
    }
    if (IsDigit(c) || (c == '-' && i + 1 < s.size() && IsDigit(s[i + 1]))) {
      size_t start = i;
      if (c == '-') ++i;
      while (i < s.size() && IsDigit(s[i])) ++i;
      if (i + 1 < s.size() && s[i] == '.' && IsDigit(s[i + 1])) {
        ++i;
        while (i < s.size() && IsDigit(s[i])) ++i;
      }
      if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t save = i;
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i < s.size() && IsDigit(s[i])) {
          while (i < s.size() && IsDigit(s[i])) ++i;
        } else {
          i = save;
        }
      }
      if (i < s.size() && IsIdentChar(s[i])) return fail("a number runs into a word", start);
      tok.t = Tok::T::kNumber;
      tok.num = std::strtod(std::string(s.substr(start, i - start)).c_str(), nullptr);
      out->push_back(tok);
      continue;
    }
    if (IsIdentStart(c)) {
      std::vector<std::string> segs;
      while (true) {
        size_t start = i;
        while (i < s.size() && IsIdentChar(s[i])) ++i;
        segs.emplace_back(s.substr(start, i - start));
        if (i + 1 < s.size() && s[i] == '.' && IsIdentStart(s[i + 1])) {
          ++i;
          continue;
        }
        break;
      }
      tok.t = Tok::T::kIdent;
      tok.text = segs[0];
      tok.segs = std::move(segs);
      out->push_back(tok);
      continue;
    }
    static const char* const kTwo[] = {"||", "&&", "==", "!=", "<=", ">=", "??"};
    bool two = false;
    if (i + 1 < s.size()) {
      for (const char* op : kTwo) {
        if (s[i] == op[0] && s[i + 1] == op[1]) {
          tok.t = Tok::T::kPunct;
          tok.text = op;
          i += 2;
          two = true;
          break;
        }
      }
    }
    if (two) {
      out->push_back(tok);
      continue;
    }
    if (c == '!' || c == '<' || c == '>' || c == '(' || c == ')' || c == '[' || c == ']' || c == ',') {
      tok.t = Tok::T::kPunct;
      tok.text = std::string(1, c);
      ++i;
      out->push_back(tok);
      continue;
    }
    return fail(std::string("unexpected character '") + c + "'", i);
  }
}

class Parser {
 public:
  Parser(std::vector<Tok> toks, std::string* err) : toks_(std::move(toks)), err_(err) {}

  std::shared_ptr<Node> Run() {
    auto n = Or();
    if (!n) return nullptr;
    if (Peek().t != Tok::T::kEnd) {
      Fail("unexpected '" + Describe(Peek()) + "'");
      return nullptr;
    }
    return n;
  }

  bool unsupported = false;

 private:
  const Tok& Peek() const { return toks_[pos_]; }
  const Tok& Take() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
  bool IsPunct(const char* p) const { return Peek().t == Tok::T::kPunct && Peek().text == p; }
  bool IsWord(const char* w) const { return Peek().t == Tok::T::kIdent && Peek().segs.size() == 1 && Peek().text == w; }

  static std::string Describe(const Tok& t) {
    switch (t.t) {
      case Tok::T::kEnd: return "end of text";
      case Tok::T::kString: return "text";
      case Tok::T::kNumber: return "number";
      default: return t.text;
    }
  }

  void Fail(const std::string& what) {
    if (err_->empty()) *err_ = what + " at column " + std::to_string(Peek().pos + 1);
  }

  static std::shared_ptr<Node> Make(Node::Kind k) {
    auto n = std::make_shared<Node>();
    n->kind = k;
    return n;
  }

  std::shared_ptr<Node> Or() {
    auto left = And();
    while (left && IsPunct("||")) {
      Take();
      auto right = And();
      if (!right) return nullptr;
      auto n = Make(Node::Kind::kOr);
      n->kids = {left, right};
      left = n;
    }
    return left;
  }

  std::shared_ptr<Node> And() {
    auto left = NotExpr();
    while (left && IsPunct("&&")) {
      Take();
      auto right = NotExpr();
      if (!right) return nullptr;
      auto n = Make(Node::Kind::kAnd);
      n->kids = {left, right};
      left = n;
    }
    return left;
  }

  std::shared_ptr<Node> NotExpr() {
    if (IsPunct("!")) {
      Take();
      auto inner = NotExpr();
      if (!inner) return nullptr;
      auto n = Make(Node::Kind::kNot);
      n->kids = {inner};
      return n;
    }
    return Cmp();
  }

  bool ParseLiteral(Value* out) {
    const Tok& t = Peek();
    if (t.t == Tok::T::kString) {
      *out = Value::String(t.text);
    } else if (t.t == Tok::T::kNumber) {
      *out = Value::Number(t.num);
    } else if (IsWord("true")) {
      *out = Value::Bool(true);
    } else if (IsWord("false")) {
      *out = Value::Bool(false);
    } else if (IsWord("null")) {
      *out = Value::Null();
    } else {
      Fail("expected a text, number, true, false or null");
      return false;
    }
    Take();
    return true;
  }

  std::shared_ptr<Node> Cmp() {
    auto left = Coalesce();
    if (!left) return nullptr;
    if (Peek().t == Tok::T::kPunct &&
        (Peek().text == "==" || Peek().text == "!=" || Peek().text == "<" || Peek().text == "<=" || Peek().text == ">" || Peek().text == ">=")) {
      std::string op = Take().text;
      auto right = Coalesce();
      if (!right) return nullptr;
      auto n = Make(Node::Kind::kCmp);
      n->name = op;
      n->kids = {left, right};
      return n;
    }
    if (IsWord("in")) {
      Take();
      if (!IsPunct("[")) {
        Fail("expected '[' after in");
        return nullptr;
      }
      Take();
      auto n = Make(Node::Kind::kIn);
      n->kids = {left};
      while (true) {
        Value v;
        if (!ParseLiteral(&v)) return nullptr;
        n->in_list.push_back(std::move(v));
        if (IsPunct(",")) {
          Take();
          continue;
        }
        break;
      }
      if (!IsPunct("]")) {
        Fail("expected ',' or ']'");
        return nullptr;
      }
      Take();
      return n;
    }
    return left;
  }

  std::shared_ptr<Node> Coalesce() {
    auto left = Primary();
    while (left && IsPunct("??")) {
      Take();
      auto right = Primary();
      if (!right) return nullptr;
      auto n = Make(Node::Kind::kCoalesce);
      n->kids = {left, right};
      left = n;
    }
    return left;
  }

  std::shared_ptr<Node> Primary() {
    const Tok& t = Peek();
    if (t.t == Tok::T::kString || t.t == Tok::T::kNumber || IsWord("true") || IsWord("false") || IsWord("null")) {
      Value v;
      if (!ParseLiteral(&v)) return nullptr;
      auto n = Make(Node::Kind::kLiteral);
      n->literal = std::move(v);
      return n;
    }
    if (IsPunct("(")) {
      Take();
      auto inner = Or();
      if (!inner) return nullptr;
      if (!IsPunct(")")) {
        Fail("expected ')'");
        return nullptr;
      }
      Take();
      return inner;
    }
    if (t.t == Tok::T::kIdent) {
      if (t.segs.size() == 1 && toks_[pos_ + 1].t == Tok::T::kPunct && toks_[pos_ + 1].text == "(") {
        auto n = Make(Node::Kind::kCall);
        n->name = t.text;
        Take();
        Take();
        if (!IsPunct(")")) {
          while (true) {
            auto arg = Or();
            if (!arg) return nullptr;
            n->kids.push_back(arg);
            if (IsPunct(",")) {
              Take();
              continue;
            }
            break;
          }
        }
        if (!IsPunct(")")) {
          Fail("expected ',' or ')'");
          return nullptr;
        }
        Take();
        static const std::map<std::string, size_t> kArity = {{"resolved", 1}, {"auto_of", 1}, {"known", 1}, {"contains", 2}, {"min", 2}, {"max", 2}};
        auto it = kArity.find(n->name);
        if (it == kArity.end() || it->second != n->kids.size()) unsupported = true;
        return n;
      }
      if (t.segs.size() > 1 && toks_[pos_ + 1].t == Tok::T::kPunct && toks_[pos_ + 1].text == "(") {
        Take();
        Fail("a function name has no dots");
        return nullptr;
      }
      if (t.segs.size() == 1 && (t.text == "in")) {
        Fail("unexpected in");
        return nullptr;
      }
      auto n = Make(Node::Kind::kPath);
      n->segs = t.segs;
      Take();
      return n;
    }
    Fail("expected a value, found '" + Describe(t) + "'");
    return nullptr;
  }

  std::vector<Tok> toks_;
  size_t pos_ = 0;
  std::string* err_;
};

inline std::string Joined(const std::vector<std::string>& segs) {
  std::string r;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i) r.push_back('.');
    r += segs[i];
  }
  return r;
}

inline void Collect(const Node& n, bool guarded, std::set<std::string>* paths, std::set<std::string>* unguarded_live) {
  if (n.kind == Node::Kind::kPath) {
    std::string p = Joined(n.segs);
    paths->insert(p);
    if (n.segs[0] == "live" && !guarded) unguarded_live->insert(p);
    return;
  }
  for (size_t i = 0; i < n.kids.size(); ++i) {
    bool g = guarded || (n.kind == Node::Kind::kCoalesce && i == 0);
    Collect(*n.kids[i], g, paths, unguarded_live);
  }
}

}  // namespace detail

inline Program Parse(std::string_view text) {
  Program p;
  std::vector<detail::Tok> toks;
  if (!detail::Tokenize(text, &toks, &p.error)) return p;
  detail::Parser parser(std::move(toks), &p.error);
  p.root = parser.Run();
  if (!p.root) {
    if (p.error.empty()) p.error = "syntax error";
    return p;
  }
  p.ok = true;
  p.unsupported_call = parser.unsupported;
  std::set<std::string> paths, live;
  detail::Collect(*p.root, false, &paths, &live);
  p.paths.assign(paths.begin(), paths.end());
  p.unguarded_live.assign(live.begin(), live.end());
  return p;
}

// ---- Environment ---------------------------------------------------------------------------------------------------------------------------

// What `by` says about an `auto` (settings-ui.json settings.<name>.auto).
struct AutoDescriptor {
  std::string by;                          // "class" | "rt_rung" | "preset"
  std::map<std::string, Value> cells;      // low/mid/high or none/weak/strong -> the value auto stands for
  std::map<std::string, Value> handheld;   // by "class": cells that replace `cells` on a handheld
  bool has_auto_value = false;             // numeric rows: the number that stands for auto
  double auto_value = 0;
};

// One def: an expression, or a cases list.
struct DefCase {
  std::shared_ptr<Program> when;           // null: no `when` (always matches)
  Value literal;                           // the case's value when `expr` is null
  std::shared_ptr<Program> expr;           // {"expr": "..."}: computed
};
struct Def {
  std::shared_ptr<Program> expr;           // a plain expression def
  std::vector<DefCase> cases;              // else a cases def
};

struct PresetTable {                       // settings-ui.json presets (section 1.9), as far as the evaluator reads it
  std::vector<std::pair<std::shared_ptr<Program>, std::string>> class_to_preset;
  std::map<std::string, std::map<std::string, Value>> values;   // preset name -> setting -> value
};

class Env {
 public:
  virtual ~Env() = default;
  virtual Value Setting(std::string_view name) const = 0;            // typed (see TypedSetting); null when the catalog has no such setting
  virtual Value Fact(std::string_view dotted_path) const = 0;        // "gpu.class", "live.hdr_active": null when missing
  virtual Value Renderer() const = 0;                                // "native" | "xenia" | null
  virtual const AutoDescriptor* Auto(std::string_view setting) const = 0;
  virtual const Def* DefOf(std::string_view name) const = 0;
  virtual const PresetTable* Presets() const { return nullptr; }
};

// ---- Evaluation ----------------------------------------------------------------------------------------------------------------------------

inline Value Evaluate(const Program& p, const Env& env, int depth = 0);

namespace detail {

constexpr int kMaxDepth = 32;

inline bool IsFactNamespace(const std::string& w) {
  return w == "gpu" || w == "api" || w == "features" || w == "os" || w == "display" || w == "cpu" || w == "build" || w == "install" || w == "live";
}

inline Value EvalDef(const std::string& name, const Env& env, int depth) {
  if (depth >= kMaxDepth) return Value::Null();
  const Def* d = env.DefOf(name);
  if (!d) return Value::Null();
  if (d->expr) return Evaluate(*d->expr, env, depth + 1);
  for (const DefCase& c : d->cases) {
    if (c.when && !Matches(Evaluate(*c.when, env, depth + 1))) continue;
    if (c.expr) return Evaluate(*c.expr, env, depth + 1);
    return c.literal;
  }
  return Value::Null();
}

inline Value CellOf(const AutoDescriptor& d, const Env& env, std::string_view setting, int depth) {
  auto pick = [](const std::map<std::string, Value>& cells, const std::string& key) -> Value {
    auto it = cells.find(key);
    return it == cells.end() ? Value::Null() : it->second;
  };
  if (d.by == "class") {
    Value cls = env.Fact("gpu.class");
    if (cls.kind != Value::Kind::kString) return Value::Null();
    if (env.Fact("gpu.handheld").IsTrue()) {
      Value h = pick(d.handheld, cls.s);
      if (!h.IsNull()) return h;
    }
    return pick(d.cells, cls.s);
  }
  if (d.by == "rt_rung") {
    Value rung = env.Fact("gpu.rt_rung");
    if (rung.kind != Value::Kind::kString) return Value::Null();
    return pick(d.cells, rung.s);
  }
  if (d.by == "preset") {
    const PresetTable* t = env.Presets();
    if (!t) return Value::Null();
    for (const auto& entry : t->class_to_preset) {
      if (!entry.first || !Matches(Evaluate(*entry.first, env, depth + 1))) continue;
      auto it = t->values.find(entry.second);
      if (it == t->values.end()) return Value::Null();
      auto cell = it->second.find(std::string(setting));
      return cell == it->second.end() ? Value::Null() : cell->second;
    }
    return Value::Null();
  }
  return Value::Null();
}

inline Value EvalNode(const Node& n, const Env& env, int depth) {
  switch (n.kind) {
    case Node::Kind::kLiteral: return n.literal;
    case Node::Kind::kPath: {
      const auto& s = n.segs;
      if (s[0] == "s") return s.size() == 2 ? env.Setting(s[1]) : Value::Null();
      if (s[0] == "d") return s.size() == 2 ? EvalDef(s[1], env, depth) : Value::Null();
      if (s[0] == "renderer") return s.size() == 1 ? env.Renderer() : Value::Null();
      if (IsFactNamespace(s[0]) && s.size() >= 2) return env.Fact(Joined(s));
      return Value::Null();
    }
    case Node::Kind::kNot: return Not(EvalNode(*n.kids[0], env, depth));
    case Node::Kind::kAnd: {
      Value a = EvalNode(*n.kids[0], env, depth), b = EvalNode(*n.kids[1], env, depth);
      return And(a, b);
    }
    case Node::Kind::kOr: {
      Value a = EvalNode(*n.kids[0], env, depth), b = EvalNode(*n.kids[1], env, depth);
      return Or(a, b);
    }
    case Node::Kind::kCmp: {
      Value a = EvalNode(*n.kids[0], env, depth), b = EvalNode(*n.kids[1], env, depth);
      return Compare(n.name, a, b);
    }
    case Node::Kind::kIn: {
      Value x = EvalNode(*n.kids[0], env, depth);
      Value acc = Value::Bool(false);
      for (const Value& lit : n.in_list) acc = Or(acc, Compare("==", x, lit));
      return acc;
    }
    case Node::Kind::kCoalesce: {
      Value a = EvalNode(*n.kids[0], env, depth);
      Value b = EvalNode(*n.kids[1], env, depth);
      return a.IsNull() ? b : a;
    }
    case Node::Kind::kCall: {
      const std::string& f = n.name;
      if (f == "known") return Value::Bool(!EvalNode(*n.kids[0], env, depth).IsNull());
      if (f == "contains") {
        Value l = EvalNode(*n.kids[0], env, depth), x = EvalNode(*n.kids[1], env, depth);
        if (l.kind != Value::Kind::kList || x.IsNull()) return Value::Null();
        for (const Value& e : l.list) {
          if (SameScalar(e, x)) return Value::Bool(true);
        }
        return Value::Bool(false);
      }
      if (f == "min" || f == "max") {
        Value a = EvalNode(*n.kids[0], env, depth), b = EvalNode(*n.kids[1], env, depth);
        if (a.kind != Value::Kind::kNumber || b.kind != Value::Kind::kNumber) return Value::Null();
        return Value::Number(f == "min" ? std::min(a.n, b.n) : std::max(a.n, b.n));
      }
      if (f == "auto_of") {
        Value name = EvalNode(*n.kids[0], env, depth);
        if (name.kind != Value::Kind::kString) return Value::Null();
        const AutoDescriptor* d = env.Auto(name.s);
        return d ? CellOf(*d, env, name.s, depth) : Value::Null();
      }
      if (f == "resolved") {
        const Node& arg = *n.kids[0];
        if (arg.kind != Node::Kind::kPath || arg.segs.size() != 2 || arg.segs[0] != "s") return Value::Null();
        const std::string& setting = arg.segs[1];
        Value v = env.Setting(setting);
        if (v.IsNull()) return v;
        const AutoDescriptor* d = env.Auto(setting);
        bool is_auto = (d && d->has_auto_value) ? (v.kind == Value::Kind::kNumber && v.n == d->auto_value)
                                                : (v.kind == Value::Kind::kString && v.s == "auto");
        if (!is_auto) return v;
        return d ? CellOf(*d, env, setting, depth) : Value::Null();
      }
      return Value::Null();
    }
  }
  return Value::Null();
}

}  // namespace detail

// The value of a program: null for a program that did not parse or that calls a function outside the v1 set.
inline Value Evaluate(const Program& p, const Env& env, int depth) {
  if (!p.ok || p.unsupported_call || !p.root) return Value::Null();
  if (depth >= detail::kMaxDepth) return Value::Null();
  return detail::EvalNode(*p.root, env, depth);
}

}  // namespace larecomp::uiexpr
