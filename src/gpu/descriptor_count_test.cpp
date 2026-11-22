// Static audit of every Plume descriptor set the renderer makes (tools/check_descriptor_counts.py is its Python twin: the two print the same lines).
//
// Why: Device::CreateDescriptorSet(builder, views, samplers, name) takes the counts the caller says its builder describes and compares them with what the
// backend allocated. D3D12 allocates one view for every descriptor of every range (a sampler range: samplers) and throws a set away whose count differs;
// Vulkan does not count. A builder with two ranges made with a count of 1 therefore failed on D3D12 every frame while Vulkan stayed healthy (the city
// shadow pass, 2026-11-21: the pass never ran on D3D12; the probe field's two sets were the same mistake). The binding code has the same trap the other
// way round: a descriptor index past the set's ranges writes into another set's slots of the shared D3D12 heap, a buffer written into a texture range
// asserts (or is garbage in a release build), a structured buffer without its structure view reads a null pointer.
//
// What it reads (every .cpp / .h under the folder it is given, never a *_test.cpp; give it the repo's src folder or src/gpu):
//   * every builder: `X.begin(); X.addTexture(..); for (..) X.addReadWriteTexture(i); Helper(X); X.end(boundless, size);`
//   * every `->CreateDescriptorSet(builder, views, samplers, name)` call: the counts are evaluated (literals, constexpr names, enumerators, casts, sums,
//     ternaries, `rules::kName` through namespace aliases) and compared with the builder's ranges as D3D12 counts them. A builder that is a parameter
//     (or a member set from one) is followed to the callers.
//   * every setTexture / setBuffer / setSampler / setAccelerationStructure call on a created set: the descriptor index is inside the set, the range there
//     has the type the call fills, a structured buffer gets its structure view; a range of the set that nothing in the file writes is a WARN.
// Anything it cannot evaluate is an UNJUDGED line naming the file and line (never silently passed).
//
// A program of its own (standard library only): it needs no renderer, no ReXGlue and no GPU. It first runs its own cases on made-up sources (the city
// shadow's mistake as it was, one mutation of each rule), then reads the folder. Exit 0 when nothing is an ERROR.
//
//   cmake --build <build dir> --target larecomp_descriptor_count_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_descriptor_count_test <src dir> [--print] [--strict]
//   --print also lists the sets that agree; --strict makes an UNJUDGED line a failure too.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Int = int64_t;

// ------------------------------------------------------------------------------------------------------------------------------------------ lexer

enum class K { Id, Num, Str, P };

struct Tok {
  K kind;
  std::string text;
  int line;
};
using Toks = std::vector<Tok>;

bool IsAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool StartsWith(const std::string& s, const std::string& prefix) { return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0; }
bool EndsWith(const std::string& s, const std::string& suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }

template <typename T>
bool Contains(const std::vector<T>& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool In(const std::string& s, std::initializer_list<const char*> options) {
  for (const char* o : options) {
    if (s == o) return true;
  }
  return false;
}

bool IsMulti3(const std::string& s) { return s == "..." || s == "<<=" || s == ">>="; }
bool IsMulti2(const std::string& s) {
  return In(s, {"::", "->", "<<", ">>", "&&", "||", "==", "!=", "<=", ">=", "++", "--", "+=", "-=", "*=", "/=", "|=", "&=", "^=", "%="});
}

int CountNewlines(const std::string& text, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from; i < to && i < text.size(); ++i) n += text[i] == '\n';
  return n;
}

void Lex(const std::string& text, Toks* toks, std::vector<std::string>* includes) {
  size_t i = 0;
  const size_t n = text.size();
  int line = 1;
  bool line_start = true;
  while (i < n) {
    const char c = text[i];
    if (c == '\n') {
      ++line;
      ++i;
      line_start = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      size_t j = text.find("*/", i + 2);
      if (j == std::string::npos) j = n >= 2 ? n - 2 : 0;
      line += CountNewlines(text, i, j + 2);
      i = j + 2;
      continue;
    }
    if (c == '#' && line_start) {
      size_t j = i;
      size_t k;
      while (true) {
        k = text.find('\n', j);
        if (k == std::string::npos) {
          k = n;
          break;
        }
        if ((k > 0 && text[k - 1] == '\\') || (k > 1 && text[k - 1] == '\r' && text[k - 2] == '\\')) {
          j = k + 1;
          ++line;
          continue;
        }
        break;
      }
      std::string d = text.substr(i + 1, k - (i + 1));
      d.erase(0, d.find_first_not_of(" \t\r\f\v"));
      if (StartsWith(d, "include")) {
        std::string rest = d.substr(7);
        rest.erase(0, rest.find_first_not_of(" \t\r\f\v"));
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
        if (!rest.empty() && (rest[0] == '"' || rest[0] == '<')) {
          const char close = rest[0] == '"' ? '"' : '>';
          const size_t e = rest.find(close, 1);
          if (e != std::string::npos && e > 0) includes->push_back(rest.substr(1, e - 1));
        }
      }
      i = k;
      continue;
    }
    line_start = false;
    if (IsAlpha(c)) {
      size_t j = i + 1;
      while (j < n && IsAlnum(text[j])) ++j;
      const std::string word = text.substr(i, j - i);
      if (j < n && text[j] == '"' && In(word, {"R", "u8R", "uR", "UR", "LR"})) {
        const size_t d = text.find('(', j);
        const std::string delim = text.substr(j + 1, d - (j + 1));
        size_t e = text.find(")" + delim + "\"", d);
        if (e == std::string::npos) e = n - 1;
        const size_t end = e + delim.size() + 2;
        toks->push_back({K::Str, "", line});
        line += CountNewlines(text, i, end);
        i = end;
        continue;
      }
      toks->push_back({K::Id, word, line});
      i = j;
      continue;
    }
    if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(text[i + 1]))) {
      size_t j = i + 1;
      while (j < n && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '.' || text[j] == '_' || text[j] == '\'')) {
        if (std::strchr("eEpP", text[j]) && j + 1 < n && (text[j + 1] == '+' || text[j + 1] == '-')) {
          std::string head = text.substr(i, j - i);
          for (char& h : head) h = static_cast<char>(std::tolower(static_cast<unsigned char>(h)));
          if (!StartsWith(head, "0x")) ++j;
        }
        ++j;
      }
      toks->push_back({K::Num, text.substr(i, j - i), line});
      i = j;
      continue;
    }
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < n && text[j] != c) {
        if (text[j] == '\\') ++j;
        if (j < n && text[j] == '\n') ++line;
        ++j;
      }
      toks->push_back({K::Str, "", line});
      i = j + 1;
      continue;
    }
    const std::string three = text.substr(i, 3);
    const std::string two = text.substr(i, 2);
    if (IsMulti3(three)) {
      toks->push_back({K::P, three, line});
      i += 3;
    } else if (IsMulti2(two)) {
      toks->push_back({K::P, two, line});
      i += 2;
    } else {
      toks->push_back({K::P, std::string(1, c), line});
      ++i;
    }
  }
}

// The integer a literal spells, or nothing (a float, junk).
std::optional<Int> NumberValue(const std::string& text) {
  std::string t;
  for (char c : text) {
    if (c != '\'') t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  while (!t.empty() && (t.back() == 'u' || t.back() == 'l')) t.pop_back();
  if (t.empty()) return std::nullopt;
  int base = 10;
  size_t start = 0;
  if (StartsWith(t, "0x")) {
    base = 16;
    start = 2;
  } else if (StartsWith(t, "0b")) {
    base = 2;
    start = 2;
  } else if (t.find('.') != std::string::npos || t.find('e') != std::string::npos || t.find('f') != std::string::npos) {
    return std::nullopt;
  } else if (t.size() > 1 && t[0] == '0') {
    base = 8;
    start = 1;
  }
  if (start >= t.size()) return std::nullopt;
  Int v = 0;
  for (size_t i = start; i < t.size(); ++i) {
    const char c = t[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else return std::nullopt;
    if (d >= base) return std::nullopt;
    v = v * base + d;
  }
  return v;
}

// Tokens [lo, hi) joined into one string ('a.b->c' keeps its shape).
std::string Jt(const Toks& toks, size_t lo, size_t hi) {
  std::string out;
  for (size_t i = lo; i < hi && i < toks.size(); ++i) out += toks[i].kind == K::Str ? "\"\"" : toks[i].text;
  return out;
}
std::string Jt(const Toks& toks) { return Jt(toks, 0, toks.size()); }

Toks Slice(const Toks& toks, size_t lo, size_t hi) {
  if (hi > toks.size()) hi = toks.size();
  if (lo >= hi) return {};
  return Toks(toks.begin() + static_cast<std::ptrdiff_t>(lo), toks.begin() + static_cast<std::ptrdiff_t>(hi));
}

bool IsP(const Tok& t, const char* s) { return t.kind == K::P && t.text == s; }

// toks split at the top level separator token text (parentheses, brackets, braces nest).
std::vector<Toks> SplitTop(const Toks& toks, const std::string& sep) {
  std::vector<Toks> out;
  Toks cur;
  int d = 0;
  for (const Tok& t : toks) {
    if (t.kind == K::P && (t.text == "(" || t.text == "[" || t.text == "{")) ++d;
    else if (t.kind == K::P && (t.text == ")" || t.text == "]" || t.text == "}")) --d;
    if (t.kind == K::P && t.text == sep && d == 0) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(t);
    }
  }
  out.push_back(cur);
  return out;
}

// ------------------------------------------------------------------------------------------------------------------------------------------ one file

struct SrcFile;

struct Const {
  std::string name;
  Toks expr;
  std::vector<std::string> scopes, types;
  SrcFile* file = nullptr;
  int idx = 0;
  bool has_local = false;
  int local_lo = 0, local_hi = 0;
  bool is_auto = false;  // an enumerator without a value: the one before plus one
  Const* prev = nullptr;
};

struct Param {
  std::string name;
  bool is_builder;
  bool has_default;
};

struct Def {  // a function definition with a descriptor set builder parameter
  std::string name;
  std::vector<std::string> quals;
  std::vector<Param> params;
  int body_lo, body_hi;
  SrcFile* file;
};

struct Scope {
  std::string kind, name;
  int open, close;
};

struct SrcFile {
  std::string path;
  Toks toks;
  std::vector<std::string> includes;
  std::vector<int> pair;
  std::vector<Scope> scopes;
  std::deque<Const> consts;
  std::map<std::string, std::vector<std::string>> aliases;
  std::deque<Def> defs;

  SrcFile(std::string p, const std::string& text) : path(std::move(p)) {
    Lex(text, &toks, &includes);
    pair.assign(toks.size(), -1);
    std::vector<int> stack;
    for (int i = 0; i < static_cast<int>(toks.size()); ++i) {
      const Tok& t = toks[i];
      if (t.kind != K::P) continue;
      const std::string& s = t.text;
      if (s == "(" || s == "[" || s == "{") {
        stack.push_back(i);
      } else if (s == ")" || s == "]" || s == "}") {
        const std::string want = s == ")" ? "(" : (s == "]" ? "[" : "{");
        while (!stack.empty() && toks[stack.back()].text != want) stack.pop_back();
        if (!stack.empty()) {
          const int o = stack.back();
          stack.pop_back();
          pair[o] = i;
          pair[i] = o;
        }
      }
    }
    ScanScopes();
    ScanConsts();
    ScanDefs();
  }

  // Index of the first token of the statement that the token before i belongs to (after the previous ; { or } at the same nesting).
  int StmtStart(int i) const {
    int j = i - 1;
    while (j >= 0) {
      const Tok& t = toks[j];
      if (t.kind == K::P) {
        if ((t.text == ")" || t.text == "]") && pair[j] >= 0) {
          j = pair[j] - 1;
          continue;
        }
        if (t.text == ";" || t.text == "{" || t.text == "}") return j + 1;
      }
      --j;
    }
    return 0;
  }

  void ScanScopes() {
    for (int i = 0; i < static_cast<int>(toks.size()); ++i) {
      if (!IsP(toks[i], "{") || pair[i] < 0) continue;
      const Toks head = Slice(toks, StmtStart(i), i);
      std::vector<std::string> texts, words;
      for (const Tok& h : head) {
        texts.push_back(h.text);
        if (h.kind == K::Id) words.push_back(h.text);
      }
      const bool has_paren = Contains(texts, std::string("("));
      std::string kind = "block", name;
      if (Contains(texts, std::string("="))) {
        kind = "block";
      } else if (Contains(words, std::string("namespace")) && !has_paren) {
        kind = "ns";
        int k = 0;
        for (int j = 0; j < static_cast<int>(head.size()); ++j) {
          if (head[j].text == "namespace") k = j;
        }
        for (size_t j = k + 1; j < head.size(); ++j) {
          if (head[j].kind == K::Id) name = head[j].text;
        }
      } else if (Contains(words, std::string("extern")) && !has_paren) {
        kind = "ns";
      } else if (Contains(words, std::string("enum"))) {
        kind = "enum";
        int k = 0;
        for (int j = 0; j < static_cast<int>(head.size()); ++j) {
          if (head[j].text == "enum") {
            k = j;
            break;
          }
        }
        for (size_t j = k + 1; j < head.size(); ++j) {
          if (head[j].kind == K::Id && head[j].text != "class" && head[j].text != "struct") {
            name = head[j].text;
            break;
          }
        }
      } else {
        for (size_t j = 0; j < head.size(); ++j) {
          bool paren_before = false;
          for (size_t q = 0; q < j; ++q) paren_before = paren_before || texts[q] == "(";
          if (head[j].kind == K::Id && In(head[j].text, {"struct", "class", "union"}) && !paren_before) {
            for (size_t x = j + 1; x < head.size(); ++x) {
              if (head[x].kind == K::Id) {
                if (head[x].text != "final") {
                  kind = "type";
                  name = head[x].text;
                }
                break;
              }
            }
            break;
          }
        }
      }
      scopes.push_back({kind, name, i, pair[i]});
    }
  }

  // The scopes around token p, outermost first.
  std::vector<Scope> ChainAt(int p) const {
    std::vector<Scope> out;
    for (const Scope& sc : scopes) {
      if (sc.open < p && p < sc.close) out.push_back(sc);
    }
    std::stable_sort(out.begin(), out.end(), [](const Scope& a, const Scope& b) { return a.open < b.open; });
    return out;
  }

  bool LocalBlock(int p, int* lo, int* hi) const {
    for (const Scope& sc : ChainAt(p)) {
      if (sc.kind == "block") {
        *lo = sc.open;
        *hi = sc.close;
        return true;
      }
    }
    return false;
  }

  // Names of the classes a use at p can see: the lexical ones and the qualifiers of the function definition that holds p.
  std::vector<std::string> EnclosingClasses(int p) const {
    std::vector<std::string> names;
    const std::vector<Scope> chain = ChainAt(p);
    for (const Scope& sc : chain) {
      if (sc.kind == "type") names.push_back(sc.name);
    }
    for (const Scope& sc : chain) {
      if (sc.kind == "block") {
        const Toks head = Slice(toks, StmtStart(sc.open), sc.open);
        for (size_t j = 0; j < head.size(); ++j) {
          if (head[j].text == "(") {
            int k = static_cast<int>(j) - 1;
            while (k >= 2 && head[k].kind == K::Id && head[k - 1].text == "::" && head[k - 2].kind == K::Id) {
              names.push_back(head[k - 2].text);
              k -= 2;
            }
            break;
          }
        }
        break;
      }
    }
    return names;
  }

  void ScanConsts() {
    const int n = static_cast<int>(toks.size());
    int i = 0;
    while (i < n) {
      const int s = i;
      int j = i;
      while (j < n) {
        const Tok& t = toks[j];
        if (t.kind == K::P) {
          if ((t.text == "(" || t.text == "[") && pair[j] >= 0) {
            j = pair[j] + 1;
            continue;
          }
          if (t.text == ";" || t.text == "{" || t.text == "}") break;
        }
        ++j;
      }
      if (j < n && toks[j].text == ";" && j > s) Stmt(s, Slice(toks, s, j));
      i = j + 1;
    }
    for (const Scope& sc : scopes) {
      if (sc.kind == "enum") Enum(sc);
    }
  }

  void NamesOf(const std::vector<Scope>& chain, std::vector<std::string>* scope_names, std::vector<std::string>* types) const {
    for (const Scope& sc : chain) {
      if ((sc.kind == "ns" || sc.kind == "type" || sc.kind == "enum") && !sc.name.empty()) scope_names->push_back(sc.name);
      if (sc.kind == "type" && !sc.name.empty()) types->push_back(sc.name);
    }
  }

  void Stmt(int s, const Toks& stmt) {
    std::vector<std::string> texts;
    for (const Tok& t : stmt) texts.push_back(t.text);
    if (texts[0] == "namespace" && Contains(texts, std::string("="))) {
      const size_t k = std::find(texts.begin(), texts.end(), "=") - texts.begin();
      std::string last;
      for (size_t x = k + 1; x < stmt.size(); ++x) {
        if (stmt[x].kind == K::Id) last = stmt[x].text;
      }
      if (!last.empty() && k >= 2) aliases[texts[1]].push_back(last);
      return;
    }
    if (!Contains(texts, std::string("constexpr")) && !Contains(texts, std::string("const"))) return;
    if (!Contains(texts, std::string("="))) return;
    const size_t eq = std::find(texts.begin(), texts.end(), "=") - texts.begin();
    bool paren = false;
    for (size_t x = 0; x < eq; ++x) paren = paren || texts[x] == "(";
    if (paren || eq == 0 || stmt[eq - 1].kind != K::Id) return;
    Const c;
    c.name = stmt[eq - 1].text;
    c.expr = SplitTop(Slice(stmt, eq + 1, stmt.size()), ",")[0];
    NamesOf(ChainAt(s), &c.scopes, &c.types);
    c.file = this;
    c.idx = s;
    c.has_local = LocalBlock(s, &c.local_lo, &c.local_hi);
    consts.push_back(std::move(c));
  }

  void Enum(const Scope& sc) {
    std::vector<std::string> scope_names, types;
    NamesOf(ChainAt(sc.open), &scope_names, &types);
    int lo = 0, hi = 0;
    const bool local = LocalBlock(sc.open, &lo, &hi);
    Const* prev = nullptr;
    for (const Toks& item : SplitTop(Slice(toks, sc.open + 1, sc.close), ",")) {
      if (item.empty() || item[0].kind != K::Id) continue;
      Const c;
      c.name = item[0].text;
      const bool has_value = item.size() > 2 && item[1].text == "=";
      if (has_value) c.expr = Slice(item, 2, item.size());
      c.scopes = scope_names;
      if (!sc.name.empty()) c.scopes.push_back(sc.name);
      c.types = types;
      c.file = this;
      c.idx = sc.open;
      c.has_local = local;
      c.local_lo = lo;
      c.local_hi = hi;
      if (!has_value) {
        c.is_auto = true;
        c.prev = prev;
      }
      consts.push_back(std::move(c));
      prev = &consts.back();
    }
  }

  void ScanDefs() {
    const int n = static_cast<int>(toks.size());
    for (int i = 1; i < n; ++i) {
      if (!IsP(toks[i], "(") || pair[i] < 0 || toks[i - 1].kind != K::Id) continue;
      const int c = pair[i];
      int j = c + 1;
      while (j < n && toks[j].kind == K::Id && In(toks[j].text, {"const", "noexcept", "override", "final"})) ++j;
      if (j >= n || toks[j].text != "{" || pair[j] < 0) continue;
      std::vector<Param> plist;
      bool any_builder = false;
      for (const Toks& p : SplitTop(Slice(toks, i + 1, c), ",")) {
        std::vector<std::string> texts;
        for (const Tok& t : p) texts.push_back(t.text);
        size_t eq = texts.size();
        for (size_t x = 0; x < texts.size(); ++x) {
          if (texts[x] == "=") {
            eq = x;
            break;
          }
        }
        std::string last_id;
        bool is_b = false;
        for (size_t x = 0; x < eq; ++x) {
          if (p[x].kind == K::Id) last_id = p[x].text;
          if (texts[x] == "RenderDescriptorSetBuilder") is_b = true;
        }
        any_builder = any_builder || is_b;
        plist.push_back({last_id, is_b, eq < texts.size()});
      }
      if (!any_builder) continue;
      Def d;
      int k = i - 1;
      while (k >= 2 && toks[k - 1].text == "::" && toks[k - 2].kind == K::Id) {
        d.quals.push_back(toks[k - 2].text);
        k -= 2;
      }
      d.name = toks[i - 1].text;
      d.params = std::move(plist);
      d.body_lo = j;
      d.body_hi = pair[j];
      d.file = this;
      defs.push_back(std::move(d));
    }
  }
};

// ------------------------------------------------------------------------------------------------------------------------------------------ the tree

// The sources: a folder on disk or made-up files; read is called lazily.
struct Tree {
  std::vector<std::string> paths;  // sorted, relative, '/' separators
  std::function<std::string(const std::string&)> read;
  std::map<std::string, std::unique_ptr<SrcFile>> files;
  std::map<std::string, std::vector<std::string>> by_base;
  std::map<std::string, std::vector<std::string>> closures;
  std::vector<std::string> interesting;

  void Init(std::vector<std::string> names) {
    for (const std::string& p : names) {
      if ((EndsWith(p, ".cpp") || EndsWith(p, ".h") || EndsWith(p, ".hpp") || EndsWith(p, ".cc")) && !EndsWith(p, "_test.cpp") && !StartsWith(p, "thirdparty/")) paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& p : paths) by_base[Basename(p)].push_back(p);
    for (const std::string& p : paths) {
      const std::string raw = read(p);
      if (raw.find("RenderDescriptorSetBuilder") != std::string::npos || raw.find("CreateDescriptorSet") != std::string::npos || raw.find("createDescriptorSet") != std::string::npos)
        interesting.push_back(p);
    }
  }

  static std::string Basename(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
  }
  static std::string Dirname(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
  }

  SrcFile& Get(const std::string& p) {
    auto it = files.find(p);
    if (it == files.end()) it = files.emplace(p, std::make_unique<SrcFile>(p, read(p))).first;
    return *it->second;
  }

  std::optional<std::string> ResolveInclude(const std::string& inc, const std::string& from) {
    auto it = by_base.find(Basename(inc));
    if (it == by_base.end() || it->second.empty()) return std::nullopt;
    std::string inc_n = inc;
    std::replace(inc_n.begin(), inc_n.end(), '\\', '/');
    for (const std::string& c : it->second) {
      if (c == inc_n || EndsWith(c, "/" + inc_n)) return c;
    }
    for (const std::string& c : it->second) {
      if (Dirname(c) == Dirname(from)) return c;
    }
    return it->second[0];
  }

  // The file and every header it includes, transitively (in include order).
  const std::vector<std::string>& Closure(const std::string& p) {
    auto got = closures.find(p);
    if (got != closures.end()) return got->second;
    std::vector<std::string> seen;
    std::vector<std::string> todo{p};
    while (!todo.empty()) {
      const std::string q = todo.front();
      todo.erase(todo.begin());
      if (Contains(seen, q)) continue;
      seen.push_back(q);
      for (const std::string& inc : Get(q).includes) {
        if (auto r = ResolveInclude(inc, q)) todo.push_back(*r);
      }
    }
    return closures.emplace(p, std::move(seen)).first->second;
  }
};

// ------------------------------------------------------------------------------------------------------------------------------------------ expressions

constexpr size_t kCap = 64;

// A set of possible values, or unknown (ok == false).
struct Vals {
  bool ok = false;
  std::set<Int> v;
  static Vals None() { return {}; }
  static Vals Of(Int x) { return {true, {x}}; }
  bool operator==(const Vals& o) const { return ok == o.ok && v == o.v; }
};

using Env = std::map<std::string, std::set<Int>>;
using BinFn = std::function<std::optional<Int>(Int, Int)>;

Vals BinOp(const Vals& a, const Vals& b, const BinFn& fn) {
  if (!a.ok || !b.ok) return Vals::None();
  Vals out;
  out.ok = true;
  for (Int x : a.v) {
    for (Int y : b.v) {
      const std::optional<Int> r = fn(x, y);
      if (!r) return Vals::None();
      out.v.insert(*r);
      if (out.v.size() > kCap) return Vals::None();
    }
  }
  return out;
}

Int FloorDiv(Int x, Int y) {
  Int q = x / y;
  if ((x % y != 0) && ((x < 0) != (y < 0))) --q;
  return q;
}
Int FloorMod(Int x, Int y) {
  Int m = x % y;
  if (m != 0 && ((m < 0) != (y < 0))) m += y;
  return m;
}

struct Evaluator;

struct Parser {
  Evaluator& ev;
  const Toks& toks;
  SrcFile& f;
  int p;
  const Env& env;
  size_t i = 0;
  bool bad = false;

  Parser(Evaluator& e, const Toks& t, SrcFile& file, int pos, const Env& en) : ev(e), toks(t), f(file), p(pos), env(en) {}

  std::string Peek(size_t k = 0) const { return i + k < toks.size() ? toks[i + k].text : std::string("\x01"); }
  bool PeekIs(size_t k, const char* s) const { return i + k < toks.size() && toks[i + k].text == s; }

  Vals Expr();
  Vals Binary(Vals (Parser::*sub)(), const std::vector<std::pair<const char*, BinFn>>& ops);
  Vals Lor() { return Binary(&Parser::Land, {{"||", [](Int x, Int y) -> std::optional<Int> { return Int((x != 0) || (y != 0)); }}}); }
  Vals Land() { return Binary(&Parser::Bor, {{"&&", [](Int x, Int y) -> std::optional<Int> { return Int((x != 0) && (y != 0)); }}}); }
  Vals Bor() { return Binary(&Parser::Bxor, {{"|", [](Int x, Int y) -> std::optional<Int> { return x | y; }}}); }
  Vals Bxor() { return Binary(&Parser::Band, {{"^", [](Int x, Int y) -> std::optional<Int> { return x ^ y; }}}); }
  Vals Band() { return Binary(&Parser::Eq, {{"&", [](Int x, Int y) -> std::optional<Int> { return x & y; }}}); }
  Vals Eq() {
    return Binary(&Parser::Rel, {{"==", [](Int x, Int y) -> std::optional<Int> { return Int(x == y); }}, {"!=", [](Int x, Int y) -> std::optional<Int> { return Int(x != y); }}});
  }
  Vals Rel() {
    return Binary(&Parser::Shift, {{"<", [](Int x, Int y) -> std::optional<Int> { return Int(x < y); }},
                                   {">", [](Int x, Int y) -> std::optional<Int> { return Int(x > y); }},
                                   {"<=", [](Int x, Int y) -> std::optional<Int> { return Int(x <= y); }},
                                   {">=", [](Int x, Int y) -> std::optional<Int> { return Int(x >= y); }}});
  }
  Vals Shift() {
    return Binary(&Parser::Add, {{"<<", [](Int x, Int y) -> std::optional<Int> { return (y >= 0 && y < 64) ? std::optional<Int>(Int(uint64_t(x) << y)) : std::nullopt; }},
                                 {">>", [](Int x, Int y) -> std::optional<Int> { return (y >= 0 && y < 64) ? std::optional<Int>(x >> y) : std::nullopt; }}});
  }
  Vals Add() {
    return Binary(&Parser::Mul, {{"+", [](Int x, Int y) -> std::optional<Int> { return x + y; }}, {"-", [](Int x, Int y) -> std::optional<Int> { return x - y; }}});
  }
  Vals Mul() {
    return Binary(&Parser::Unary, {{"*", [](Int x, Int y) -> std::optional<Int> { return x * y; }},
                                   {"/", [](Int x, Int y) -> std::optional<Int> { return y == 0 ? std::nullopt : std::optional<Int>(FloorDiv(x, y)); }},
                                   {"%", [](Int x, Int y) -> std::optional<Int> { return y == 0 ? std::nullopt : std::optional<Int>(FloorMod(x, y)); }}});
  }
  Vals Unary();
  Vals Primary();
  bool CloseParen() {
    if (!PeekIs(0, ")")) {
      bad = true;
      return false;
    }
    ++i;
    return true;
  }
  void Swallow();
  static Vals Cast(const Vals& a, const std::string& ty) {
    if (!a.ok) return Vals::None();
    if (ty == "uint32_t" || ty == "unsigned") {
      Vals out;
      out.ok = true;
      for (Int x : a.v) out.v.insert(x & 0xFFFFFFFF);
      return out;
    }
    return a;
  }
};

struct Evaluator {
  Tree& tree;
  std::map<const Const*, Vals> memo;
  std::set<const Const*> busy;

  explicit Evaluator(Tree& t) : tree(t) {}

  std::vector<std::string> AliasTargets(const std::vector<std::string>& files, const std::string& q) {
    std::vector<std::string> out{q};
    for (const std::string& path : files) {
      const auto& al = tree.Get(path).aliases;
      auto it = al.find(q);
      if (it == al.end()) continue;
      for (const std::string& t : it->second) {
        if (!Contains(out, t)) out.push_back(t);
      }
    }
    return out;
  }

  // The values of the constant named by `names` (A, B, kName) used in file f at token p.
  Vals Lookup(SrcFile& f, int p, const std::vector<std::string>& names) {
    const std::string& name = names.back();
    const std::vector<std::string> quals(names.begin(), names.end() - 1);
    const std::vector<std::string> files = tree.Closure(f.path);
    std::vector<std::string> enclosing;
    if (quals.empty()) enclosing = f.EnclosingClasses(p);
    struct Cand {
      bool same_file, local;
      Const* c;
    };
    std::vector<Cand> cands;
    for (const std::string& path : files) {
      SrcFile& sf = tree.Get(path);
      for (Const& c : sf.consts) {
        if (c.name != name) continue;
        if (c.has_local && !(&sf == &f && c.local_lo < p && p < c.local_hi)) continue;
        if (!quals.empty()) {
          const std::string inner = c.scopes.empty() ? std::string() : c.scopes.back();
          if (!Contains(AliasTargets(files, quals.back()), inner)) continue;
        } else if (!c.types.empty()) {
          bool any = false;
          for (const std::string& t : c.types) any = any || Contains(enclosing, t);
          if (!any) continue;
        }
        cands.push_back({&sf == &f, c.has_local, &c});
      }
    }
    if (cands.empty()) return Vals::None();
    for (int pick : {1, 0}) {
      std::vector<Cand> sub;
      for (const Cand& x : cands) {
        if (pick == 1 ? x.local : x.same_file) sub.push_back(x);
      }
      if (!sub.empty()) {
        cands = sub;
        break;
      }
    }
    Vals out;
    out.ok = true;
    for (const Cand& c : cands) {
      const Vals v = ConstValue(*c.c);
      if (!v.ok) return Vals::None();
      out.v.insert(v.v.begin(), v.v.end());
    }
    return out.v.size() <= kCap ? out : Vals::None();
  }

  Vals ConstValue(const Const& c) {
    auto m = memo.find(&c);
    if (m != memo.end()) return m->second;
    if (busy.count(&c)) return Vals::None();
    busy.insert(&c);
    Vals v;
    if (c.is_auto) {
      if (c.prev == nullptr) {
        v = Vals::Of(0);
      } else {
        const Vals pv = ConstValue(*c.prev);
        if (pv.ok) {
          v.ok = true;
          for (Int x : pv.v) v.v.insert(x + 1);
        }
      }
    } else {
      v = Eval(c.expr, *c.file, c.idx, {});
    }
    busy.erase(&c);
    memo[&c] = v;
    return v;
  }

  // The set of values of the expression, or unknown when it is not something this check can evaluate.
  Vals Eval(const Toks& toks, SrcFile& f, int p, const Env& env) {
    Parser parser(*this, toks, f, p, env);
    const Vals v = parser.Expr();
    if (parser.bad || parser.i != toks.size()) return Vals::None();
    return v;
  }

  std::optional<Int> One(const Toks& toks, SrcFile& f, int p, const Env& env = {}) {
    const Vals v = Eval(toks, f, p, env);
    if (v.ok && v.v.size() == 1) return *v.v.begin();
    return std::nullopt;
  }
};

Vals Parser::Binary(Vals (Parser::*sub)(), const std::vector<std::pair<const char*, BinFn>>& ops) {
  Vals a = (this->*sub)();
  while (true) {
    const BinFn* fn = nullptr;
    for (const auto& o : ops) {
      if (i < toks.size() && toks[i].text == o.first) fn = &o.second;
    }
    if (fn == nullptr) break;
    ++i;
    const Vals b = (this->*sub)();
    a = BinOp(a, b, *fn);
  }
  return a;
}

Vals Parser::Expr() {
  const Vals c = Lor();
  if (PeekIs(0, "?")) {
    ++i;
    const Vals a = Expr();
    if (!PeekIs(0, ":")) {
      bad = true;
      return Vals::None();
    }
    ++i;
    const Vals b = Expr();
    if (c.ok && c.v == std::set<Int>{0}) return b;
    if (c.ok && !c.v.count(0)) return a;
    if (!a.ok || !b.ok) return Vals::None();
    Vals u = a;
    u.v.insert(b.v.begin(), b.v.end());
    return u.v.size() <= kCap ? u : Vals::None();
  }
  return c;
}

Vals Parser::Unary() {
  const std::string t = Peek();
  if (t == "-") {
    ++i;
    const Vals a = Unary();
    if (!a.ok) return Vals::None();
    Vals out;
    out.ok = true;
    for (Int x : a.v) out.v.insert(-x);
    return out;
  }
  if (t == "+") {
    ++i;
    return Unary();
  }
  if (t == "!") {
    ++i;
    const Vals a = Unary();
    if (!a.ok) return Vals::None();
    Vals out;
    out.ok = true;
    for (Int x : a.v) out.v.insert(Int(x == 0));
    return out;
  }
  if (t == "~") {
    ++i;
    const Vals a = Unary();
    if (!a.ok) return Vals::None();
    Vals out;
    out.ok = true;
    for (Int x : a.v) out.v.insert((~x) & 0xFFFFFFFF);
    return out;
  }
  return Primary();
}

bool IsCastType(const std::string& s) { return In(s, {"uint32_t", "uint64_t", "int", "unsigned", "size_t", "int32_t", "int64_t", "uint16_t", "uint8_t", "long", "short"}); }

Vals Parser::Primary() {
  if (i >= toks.size()) {
    bad = true;
    return Vals::None();
  }
  const Tok tk = toks[i];
  const std::string& t = tk.text;
  if (tk.kind == K::Num) {
    ++i;
    const std::optional<Int> v = NumberValue(t);
    return v ? Vals::Of(*v) : Vals::None();
  }
  if (t == "(" && tk.kind == K::P) {
    if (i + 2 < toks.size() && IsCastType(toks[i + 1].text) && toks[i + 2].text == ")") {
      const std::string ty = toks[i + 1].text;
      i += 3;
      return Cast(Unary(), ty);
    }
    ++i;
    const Vals a = Expr();
    return CloseParen() ? a : Vals::None();
  }
  if (tk.kind == K::Id) {
    if (t == "true" || t == "false") {
      ++i;
      return Vals::Of(t == "true" ? 1 : 0);
    }
    if (t == "static_cast" && PeekIs(1, "<")) {
      size_t j = i + 2;
      std::string ty;
      while (j < toks.size() && toks[j].text != ">") {
        ty = toks[j].text;
        ++j;
      }
      i = j + 1;
      if (!PeekIs(0, "(")) {
        bad = true;
        return Vals::None();
      }
      ++i;
      const Vals a = Expr();
      return CloseParen() ? Cast(a, ty) : Vals::None();
    }
    if (IsCastType(t) && PeekIs(1, "(")) {
      i += 2;
      const Vals a = Expr();
      return CloseParen() ? Cast(a, t) : Vals::None();
    }
    std::vector<std::string> names{t};
    ++i;
    while (PeekIs(0, "::") && i + 1 < toks.size() && toks[i + 1].kind == K::Id) {
      names.push_back(toks[i + 1].text);
      i += 2;
    }
    if (i < toks.size() && In(toks[i].text, {"(", ".", "->", "[", "<"}) && toks[i].kind == K::P) {
      Swallow();  // a call, a member, a template: not a constant here
      return Vals::None();
    }
    if (names.size() == 1) {
      auto e = env.find(names[0]);
      if (e != env.end()) return {true, e->second};
    }
    return ev.Lookup(f, p, names);
  }
  bad = true;
  return Vals::None();
}

void Parser::Swallow() {
  while (i < toks.size()) {
    const std::string& t = toks[i].text;
    if (toks[i].kind == K::P && (t == "(" || t == "[")) {
      int d = 1;
      ++i;
      while (i < toks.size() && d) {
        const std::string& u = toks[i].text;
        if (toks[i].kind == K::P && (u == "(" || u == "[")) ++d;
        else if (toks[i].kind == K::P && (u == ")" || u == "]")) --d;
        ++i;
      }
      continue;
    }
    if (toks[i].kind == K::P && (t == "." || t == "->" || t == "::")) {
      ++i;
      continue;
    }
    if (toks[i].kind == K::Id && i > 0 && toks[i - 1].kind == K::P && In(toks[i - 1].text, {".", "->", "::"})) {
      ++i;
      continue;
    }
    break;
  }
}

// ------------------------------------------------------------------------------------------------------------------------------------------ builders

std::string KindType(const std::string& kind) {
  static const std::map<std::string, std::string> m = {
      {"ConstantBuffer", "cbv"},        {"FormattedBuffer", "srv-formatted"},       {"ReadWriteFormattedBuffer", "uav-formatted"},
      {"Texture", "texture"},           {"ReadWriteTexture", "rw-texture"},         {"Sampler", "sampler"},
      {"ImmutableSampler", "sampler-immutable"}, {"StructuredBuffer", "structured"}, {"ReadWriteStructuredBuffer", "rw-structured"},
      {"ByteAddressBuffer", "byte"},    {"ReadWriteByteAddressBuffer", "rw-byte"},  {"AccelerationStructure", "accel"}};
  auto it = m.find(kind);
  return it == m.end() ? "?" : it->second;
}

bool CallAccepts(const std::string& api, const std::string& ty) {
  if (api == "setTexture") return ty == "texture" || ty == "rw-texture";
  if (api == "setBuffer") return In(ty, {"cbv", "srv-formatted", "uav-formatted", "structured", "rw-structured", "byte", "rw-byte"});
  if (api == "setSampler") return ty == "sampler" || ty == "sampler-immutable";
  if (api == "setAccelerationStructure") return ty == "accel";
  return false;
}

bool IsCallApi(const std::string& s) { return In(s, {"setTexture", "setBuffer", "setSampler", "setAccelerationStructure"}); }

struct Item {  // one range of a builder, as the fill code adds it
  std::string kind;
  std::optional<Int> count;
  Int reps = 1;
  std::optional<Int> binding;
  SrcFile* file;
  int line;
  std::string assigned;  // '' when the add's result is not stored
  std::string via;       // the helper that added it
  Int start = 0, length = 0;
  bool placed = false;
  std::string Ty() const { return KindType(kind); }
};

struct Region {
  SrcFile* file;
  std::string recv;
  int line;
  std::vector<Item> items;
  std::vector<std::string> reasons;  // why the totals are not exact
  std::optional<Int> boundless;
  bool is_boundless = false;
  std::map<std::string, Int> assigned;
  Int total_views = 0, total_samplers = 0, total = 0;

  Region(SrcFile* f, std::string r, int l) : file(f), recv(std::move(r)), line(l) {}
  bool Exact() const { return reasons.empty(); }

  // Positions, totals (views, samplers) as D3D12 counts them, and the index variables.
  void Finish() {
    Int pos = 0, views = 0, samplers = 0;
    const size_t n = items.size();
    for (size_t k = 0; k < n; ++k) {
      Item& it = items[k];
      if (!it.count) {
        reasons.push_back("the range at line " + std::to_string(it.line) + " has a count that is not a number");
        continue;
      }
      Int c = *it.count * it.reps;
      if (k == n - 1 && is_boundless) {
        if (!boundless) {
          reasons.push_back("the boundless size is not a number");
          continue;
        }
        c = *boundless;
      }
      it.start = pos;
      it.length = c;
      it.placed = true;
      pos += c;
      if (it.Ty() == "sampler") samplers += c;
      else views += c;
      if (!it.assigned.empty()) assigned[it.assigned] = it.start;
    }
    total_views = views;
    total_samplers = samplers;
    total = pos;
  }

  const Item* TypeAt(Int i) const {
    for (const Item& it : items) {
      if (it.placed && it.start <= i && i < it.start + it.length) return &it;
    }
    return nullptr;
  }
};

struct Site {  // a builder that can arrive at a CreateDescriptorSet call
  std::string who;
  std::vector<const Region*> regions;
  Env env;
};

struct Line {
  int order;
  std::string level, path;
  int line;
  std::string kind, text;
  bool operator<(const Line& o) const { return std::tie(order, path, line, kind, text, level) < std::tie(o.order, o.path, o.line, o.kind, o.text, o.level); }
};

std::string Str(Int v) { return std::to_string(v); }

struct Audit {
  Tree& tree;
  Evaluator ev;
  std::set<Line> lines;
  int calls_n = 0, judged_n = 0, unjudged_n = 0, errors_n = 0, bindings_n = 0, bindings_judged_n = 0;
  std::map<std::pair<std::string, std::string>, std::vector<Region>> regions;
  std::map<std::string, std::vector<Def*>> defs;
  std::set<std::string> agree;
  std::map<std::string, std::string> members;  // a helper's returned struct member -> its range type ('' when two disagree)

  explicit Audit(Tree& t) : tree(t), ev(t) {}

  void Add(const std::string& level, const std::string& path, int line, const std::string& kind, const std::string& text) {
    const int order = level == "ERROR" ? 0 : (level == "WARN" ? 1 : (level == "UNJUDGED" ? 2 : 3));
    lines.insert({order, level, path, line, kind, text});
  }
  void Error(const std::string& path, int line, const std::string& kind, const std::string& text) {
    ++errors_n;
    Add("ERROR", path, line, kind, text);
  }

  // ---- helpers over tokens
  std::vector<Toks> SplitArgs(SrcFile& f, int open_idx) {
    std::vector<Toks> args = SplitTop(Slice(f.toks, open_idx + 1, f.pair[open_idx]), ",");
    if (args.size() == 1 && args[0].empty()) return {};
    return args;
  }

  bool MatchRecv(SrcFile& f, int i, const Toks& rt) {
    const Toks& toks = f.toks;
    if (i > 0 && toks[i - 1].kind == K::P && In(toks[i - 1].text, {".", "->", "::"})) return false;
    for (size_t k = 0; k < rt.size(); ++k) {
      if (i + static_cast<int>(k) >= static_cast<int>(toks.size()) || toks[i + k].text != rt[k].text) return false;
    }
    return true;
  }

  // The statement (a block or a single statement) that starts at token b: (first token, end, the token after it).
  void BodyOf(SrcFile& f, int b, int hi, int* lo, int* end, int* next) {
    const Toks& toks = f.toks;
    if (b < hi && toks[b].text == "{" && f.pair[b] >= 0) {
      *lo = b + 1;
      *end = f.pair[b];
      *next = f.pair[b] + 1;
      return;
    }
    int e = b;
    while (e < hi && toks[e].text != ";") {
      if (toks[e].kind == K::P && In(toks[e].text, {"(", "[", "{"}) && f.pair[e] >= 0) e = f.pair[e];
      ++e;
    }
    *lo = b;
    *end = e;
    *next = e + 1;
  }

  // Iterations of `for (init; cond; step)` in the usual form `for (T i = A; i < B; ++i)`.
  std::optional<Int> Trip(SrcFile& f, int o, int c) {
    const std::vector<Toks> parts = SplitTop(Slice(f.toks, o + 1, c), ";");
    if (parts.size() != 3) return std::nullopt;
    const Toks& init = parts[0];
    const Toks& cond = parts[1];
    const Toks& step = parts[2];
    size_t eq = init.size();
    for (size_t x = 0; x < init.size(); ++x) {
      if (init[x].text == "=") {
        eq = x;
        break;
      }
    }
    if (eq == init.size() || eq == 0) return std::nullopt;
    const std::string var = init[eq - 1].text;
    const std::optional<Int> a = ev.One(Slice(init, eq + 1, init.size()), f, o);
    if (cond.size() < 3 || cond[0].text != var || (cond[1].text != "<" && cond[1].text != "<=")) return std::nullopt;
    const std::optional<Int> b = ev.One(Slice(cond, 2, cond.size()), f, o);
    const std::string sj = Jt(step);
    if (!a || !b || !(sj == "++" + var || sj == var + "++" || sj == var + "+=1")) return std::nullopt;
    return std::max<Int>(0, *b - *a + (cond[1].text == "<=" ? 1 : 0));
  }

  // ---- fill code: the ranges between begin() and end()
  void Fill(SrcFile& f, int lo, int hi, const Toks& rt, Region& region, const std::string& via, int depth) {
    const Toks& toks = f.toks;
    int i = lo;
    while (i < hi) {
      const Tok& t = toks[i];
      const bool nxt_is_paren = i + 1 < hi && toks[i + 1].text == "(" && f.pair[i + 1] >= 0;
      if (t.kind == K::Id && t.text == "for" && nxt_is_paren) {
        const int c = f.pair[i + 1];
        int b_lo, b_hi, nxt;
        BodyOf(f, c + 1, hi, &b_lo, &b_hi, &nxt);
        const std::optional<Int> trip = Trip(f, i + 1, c);
        Region sub(&f, region.recv, region.line);
        Fill(f, b_lo, b_hi, rt, sub, via, depth);
        if (!trip && !sub.items.empty()) region.reasons.push_back("line " + std::to_string(t.line) + ": a loop whose count is not a number holds ranges");
        for (Item& it : sub.items) it.reps *= trip ? *trip : 1;
        for (Item& it : sub.items) region.items.push_back(it);
        for (const std::string& r : sub.reasons) region.reasons.push_back(r);
        i = nxt;
        continue;
      }
      if (t.kind == K::Id && In(t.text, {"if", "switch", "while"}) && nxt_is_paren) {
        int b_lo, b_hi, nxt;
        BodyOf(f, f.pair[i + 1] + 1, hi, &b_lo, &b_hi, &nxt);
        Region sub(&f, region.recv, region.line);
        Fill(f, b_lo, b_hi, rt, sub, via, depth);
        if (!sub.items.empty()) region.reasons.push_back("line " + std::to_string(t.line) + ": ranges are added under a condition");
        for (Item& it : sub.items) region.items.push_back(it);
        for (const std::string& r : sub.reasons) region.reasons.push_back(r);
        i = nxt;
        continue;
      }
      if (t.kind == K::Id && MatchRecv(f, i, rt)) {
        const int k = i + static_cast<int>(rt.size());
        if (k + 2 < static_cast<int>(toks.size()) && toks[k].text == "." && toks[k + 1].kind == K::Id && StartsWith(toks[k + 1].text, "add") && toks[k + 2].text == "(" && f.pair[k + 2] >= 0) {
          AddRange(f, i, k, region, via);
          i = f.pair[k + 2] + 1;
          continue;
        }
      }
      if (t.kind == K::Id && nxt_is_paren && !In(t.text, {"sizeof", "return", "static_cast"})) {
        const std::vector<Toks> args = SplitArgs(f, i + 1);
        int hit = -1;
        for (size_t ai = 0; ai < args.size(); ++ai) {
          std::string a = Jt(args[ai]);
          a.erase(0, a.find_first_not_of("&*"));
          if (a == Jt(rt)) hit = static_cast<int>(ai);
        }
        if (hit >= 0) {
          HelperCall(f, i, hit, region, depth);
          i = f.pair[i + 1] + 1;
          continue;
        }
      }
      ++i;
    }
  }

  void AddRange(SrcFile& f, int i, int k, Region& region, const std::string& via) {
    const Toks& toks = f.toks;
    const std::string kind = toks[k + 1].text.substr(3);
    const int line = toks[k + 1].line;
    const std::vector<Toks> args = SplitArgs(f, k + 2);
    std::string assigned;
    if (i >= 2 && toks[i - 1].text == "=") {
      int j = i - 2;
      while (j >= 0 && (toks[j].kind == K::Id || (toks[j].kind == K::P && In(toks[j].text, {".", "->", "::"})))) --j;
      assigned = Jt(toks, j + 1, i - 1);
    }
    if (KindType(kind) == "?") region.reasons.push_back("line " + std::to_string(line) + ": add" + kind + " is not a range this check knows");
    std::optional<Int> count = 1;
    if (kind == "ImmutableSampler") {
      if (args.size() >= 3) count = ev.One(args[2], f, i);
    } else if (args.size() >= 2) {
      count = ev.One(args[1], f, i);
    }
    Item it;
    it.kind = kind;
    it.count = count;
    it.binding = args.empty() ? std::nullopt : ev.One(args[0], f, i);
    it.file = &f;
    it.line = line;
    it.assigned = assigned;
    it.via = via;
    region.items.push_back(std::move(it));
  }

  void HelperCall(SrcFile& f, int i, int hit, Region& region, int depth) {
    const Toks& toks = f.toks;
    const std::string name = toks[i].text;
    std::string qual;
    bool has_qual = false;
    if (i >= 2 && toks[i - 1].text == "::" && toks[i - 2].kind == K::Id) {
      qual = toks[i - 2].text;
      has_qual = true;
    }
    std::vector<Def*> cands;
    auto dit = defs.find(name);
    if (dit != defs.end()) cands = dit->second;
    if (has_qual) {
      std::vector<Def*> narrowed;
      for (Def* d : cands) {
        if (!d->quals.empty() && d->quals[0] == qual) narrowed.push_back(d);
      }
      if (!narrowed.empty()) cands = narrowed;
    }
    for (Def* d : cands) {
      if (hit < static_cast<int>(d->params.size()) && d->params[hit].is_builder && depth < 4) {
        Region sub(d->file, region.recv, region.line);
        const Toks rt{{K::Id, d->params[hit].name, 0}};
        Fill(*d->file, d->body_lo + 1, d->body_hi, rt, sub, name, depth + 1);
        for (Item& it : sub.items) {
          it.via = name;
          const size_t dot = it.assigned.rfind('.');
          if (!it.assigned.empty() && it.assigned.find('.') != std::string::npos) {
            const std::string member = it.assigned.substr(dot + 1);
            auto old = members.find(member);
            if (old == members.end() || old->second == it.Ty()) members[member] = it.Ty();
            else members[member] = "";
          }
        }
        for (Item& it : sub.items) region.items.push_back(it);
        for (const std::string& r : sub.reasons) region.reasons.push_back(r);
        return;
      }
    }
    if (name != "addDescriptorSet" && name != "CreateDescriptorSet") {
      region.reasons.push_back("line " + std::to_string(toks[i].line) + ": the builder is passed to " + name + ", which this check could not find");
    }
  }

  // ---- finding things
  void CollectDefs() {
    for (const std::string& p : tree.interesting) {
      for (Def& d : tree.Get(p).defs) defs[d.name].push_back(&d);
    }
  }

  void CollectRegions(SrcFile& f) {
    const Toks& toks = f.toks;
    const int n = static_cast<int>(toks.size());
    for (int i = 2; i < n - 2; ++i) {
      if (!(toks[i].text == "begin" && toks[i].kind == K::Id && toks[i + 1].text == "(" && toks[i + 2].text == ")" && toks[i - 1].kind == K::P && (toks[i - 1].text == "." || toks[i - 1].text == "->"))) continue;
      int j = i - 2;
      while (j >= 0 && (toks[j].kind == K::Id || (toks[j].kind == K::P && In(toks[j].text, {".", "->", "::"})))) --j;
      const Toks rt = Slice(toks, j + 1, i - 1);
      if (rt.empty() || (j >= 0 && toks[j].kind == K::P && (toks[j].text == ")" || toks[j].text == "]"))) continue;  // a layout builder's begin(a, b) has arguments
      int end = -1;
      for (int k = i + 3; k < n - 1; ++k) {
        if (toks[k].text == "end" && toks[k].kind == K::Id && toks[k - 1].kind == K::P && (toks[k - 1].text == "." || toks[k - 1].text == "->") && toks[k + 1].text == "(" &&
            MatchRecv(f, k - 1 - static_cast<int>(rt.size()), rt)) {
          end = k;
          break;
        }
      }
      if (end < 0) continue;
      Region region(&f, Jt(rt), toks[i].line);
      Fill(f, i + 3, end - 1 - static_cast<int>(rt.size()), rt, region, "", 0);
      const std::vector<Toks> args = SplitArgs(f, end + 1);
      if (!args.empty() && Jt(args[0]) == "true") {
        region.is_boundless = true;
        region.boundless = args.size() > 1 ? ev.One(args[1], f, end) : std::nullopt;
      }
      region.Finish();
      regions[{f.path, region.recv}].push_back(std::move(region));
    }
  }

  // ---- the calls
  std::string LhsOf(SrcFile& f, int i) {
    const Toks& toks = f.toks;
    int j = i - 1;
    while (j >= 0 && (toks[j].kind == K::Id || (toks[j].kind == K::P && In(toks[j].text, {".", "->", "::"})))) --j;
    if (j >= 1 && toks[j].text == "=") {
      int k = j - 1;
      std::vector<std::string> parts;
      while (k >= 0) {
        const Tok& t = toks[k];
        if (t.text == "]" && f.pair[k] >= 0) {
          k = f.pair[k] - 1;
          continue;
        }
        if (t.kind == K::Id || (t.kind == K::P && (t.text == "." || t.text == "->"))) {
          parts.push_back(t.text);
          --k;
          continue;
        }
        break;
      }
      std::string out;
      for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += *it;
      return out;
    }
    return "";
  }

  static std::string LastName(const std::string& s) {
    size_t cut = 0;
    const size_t dot = s.rfind('.');
    const size_t arrow = s.rfind("->");
    if (dot != std::string::npos) cut = dot + 1;
    if (arrow != std::string::npos && arrow + 2 > cut) cut = arrow + 2;
    return s.substr(cut);
  }

  // How many CreateDescriptorSet calls in the file assign to each last name.
  std::map<std::string, int> SetNames(SrcFile& f) {
    std::map<std::string, int> out;
    const Toks& toks = f.toks;
    for (int i = 1; i < static_cast<int>(toks.size()) - 1; ++i) {
      if (toks[i].text == "CreateDescriptorSet" && toks[i + 1].text == "(" && toks[i - 1].kind == K::P && (toks[i - 1].text == "." || toks[i - 1].text == "->")) {
        const std::string lhs = LhsOf(f, i);
        if (!lhs.empty()) ++out[LastName(lhs)];
      }
    }
    return out;
  }

  Def* EnclosingDef(SrcFile& f, int i) {
    Def* best = nullptr;
    for (Def& d : f.defs) {
      if (d.body_lo < i && i < d.body_hi && (best == nullptr || d.body_lo > best->body_lo)) best = &d;
    }
    return best;
  }

  // What a caller says of the other parameters: nullptr / 0 / false are 0, an address is nonzero; anything else is unknown.
  Env ParamEnv(const Def& d, int pi, const std::vector<Toks>& args) {
    Env env;
    for (size_t j = 0; j < d.params.size(); ++j) {
      if (static_cast<int>(j) == pi || j >= args.size() || d.params[j].name.empty()) continue;
      const std::string t = Jt(args[j]);
      if (t == "nullptr" || t == "0" || t == "false" || t == "NULL") env[d.params[j].name] = {0};
      else if (!t.empty() && t[0] == '&') env[d.params[j].name] = {1};
    }
    return env;
  }

  static std::string StripPtr(std::string s) {
    s.erase(0, s.find_first_not_of("*&"));
    return s;
  }

  // The builders that can arrive at the call i; empty when the builder cannot be found.
  std::vector<Site> Resolve(SrcFile& f, int i, const Toks& arg) {
    const std::string text = StripPtr(Jt(arg));
    auto direct = regions.find({f.path, text});
    if (direct != regions.end() && !direct->second.empty()) {
      Site s;
      s.who = text;
      for (const Region& r : direct->second) s.regions.push_back(&r);
      return {s};
    }
    Def* d = EnclosingDef(f, i);
    Def* param_def = nullptr;
    int param_idx = -1;
    bool via_member = true;
    if (d != nullptr) {
      for (size_t pi = 0; pi < d->params.size(); ++pi) {
        if (d->params[pi].is_builder && d->params[pi].name == text) {
          param_def = d;
          param_idx = static_cast<int>(pi);
          via_member = false;
        }
      }
    }
    if (param_def == nullptr) {
      // `member = &param;` or `member = param;` inside a definition of this file that has the builder parameter
      for (Def& dd : f.defs) {
        for (int k = dd.body_lo; k < dd.body_hi; ++k) {
          if (f.toks[k].text == text && k + 1 < static_cast<int>(f.toks.size()) && f.toks[k + 1].text == "=") {
            const Toks rhs = Slice(f.toks, k + 2, k + 4);
            std::string nm;
            if (rhs.size() > 1 && rhs[0].text == "&") nm = rhs[1].text;
            else if (!rhs.empty()) nm = rhs[0].text;
            for (size_t pi = 0; pi < dd.params.size(); ++pi) {
              if (dd.params[pi].is_builder && dd.params[pi].name == nm) {
                param_def = &dd;
                param_idx = static_cast<int>(pi);
              }
            }
          }
        }
      }
    }
    if (param_def == nullptr) return {};
    std::vector<Site> sites;
    for (const std::string& p2 : tree.interesting) {
      SrcFile& g = tree.Get(p2);
      const Toks& toks = g.toks;
      for (int k = 0; k + 1 < static_cast<int>(toks.size()); ++k) {
        if (toks[k].kind != K::Id || toks[k].text != param_def->name || toks[k + 1].text != "(" || g.pair[k + 1] < 0) continue;
        const std::vector<Toks> args = SplitArgs(g, k + 1);
        // the default arguments live in the header: any count from the builder's position up to all of them is a call
        if (param_idx >= static_cast<int>(args.size()) || args.size() > param_def->params.size()) continue;
        bool declaration = false;
        for (const Tok& x : args[param_idx]) declaration = declaration || x.text == "RenderDescriptorSetBuilder";
        if (declaration) continue;  // the definition or a declaration is not a call
        const std::string cand = StripPtr(Jt(args[param_idx]));
        auto regs = regions.find({g.path, cand});
        if (regs != regions.end() && !regs->second.empty()) {
          Site s;
          s.who = cand + " (passed at " + g.path + ":" + std::to_string(toks[k].line) + ")";
          for (const Region& r : regs->second) s.regions.push_back(&r);
          if (!via_member) s.env = ParamEnv(*param_def, param_idx, args);
          sites.push_back(std::move(s));
        }
      }
    }
    return sites;
  }

  static std::string FmtSet(const Vals& s) {
    std::string out;
    for (Int x : s.v) out += (out.empty() ? "" : "/") + Str(x);
    return out;
  }

  void Unjudged(SrcFile& f, int line, const std::string& label, const std::string& why) {
    ++unjudged_n;
    Add("UNJUDGED", f.path, line, "unjudged", label + ": " + why);
  }

  void Calls(SrcFile& f) {
    const Toks& toks = f.toks;
    const std::map<std::string, int> creators = SetNames(f);
    for (int i = 1; i < static_cast<int>(toks.size()) - 1; ++i) {
      if (toks[i].kind != K::Id || toks[i].text != "CreateDescriptorSet" || toks[i + 1].text != "(" || toks[i - 1].kind != K::P || !(toks[i - 1].text == "." || toks[i - 1].text == "->") ||
          f.pair[i + 1] < 0)
        continue;
      ++calls_n;
      const std::vector<Toks> args = SplitArgs(f, i + 1);
      const int line = toks[i].line;
      if (args.size() != 4) {
        Error(f.path, line, "args", "CreateDescriptorSet takes (builder, views, samplers, name); this call has " + std::to_string(args.size()) + " arguments");
        continue;
      }
      const std::string label = StripPtr(Jt(args[0]));
      const std::vector<Site> sites = Resolve(f, i, args[0]);
      if (sites.empty()) {
        Unjudged(f, line, label, "no builder with this name is filled (begin() .. end()) in this file, and it is not a parameter whose callers are found");
        continue;
      }
      bool judged = false, whole = true;
      std::vector<const Region*> exact_regions;
      for (const Site& site : sites) {
        const Vals views = ev.Eval(args[1], f, i, site.env);
        const Vals samplers = ev.Eval(args[2], f, i, site.env);
        if (!views.ok || !samplers.ok) {
          Unjudged(f, line, label, "the counts '" + Jt(args[1]) + "', '" + Jt(args[2]) + "' are not numbers this check can evaluate");
          whole = false;
          continue;
        }
        for (const Region* r : site.regions) {
          if (!r->Exact()) {
            std::string why;
            for (const std::string& x : r->reasons) why += (why.empty() ? "" : "; ") + x;
            Unjudged(f, line, label, "the ranges of " + site.who + " are not exact: " + why);
            whole = false;
            continue;
          }
          judged = true;
          exact_regions.push_back(r);
          if (views.v.count(r->total_views) && samplers.v.count(r->total_samplers)) {
            agree.insert(f.path + ":" + std::to_string(line) + " " + label + " " + Str(r->total_views) + " views + " + Str(r->total_samplers) + " samplers");
          } else {
            Error(f.path, line, "count",
                  label + ": CreateDescriptorSet says " + FmtSet(views) + " views + " + FmtSet(samplers) + " samplers, the ranges of " + site.who + " (" + r->file->path + ":" +
                      std::to_string(r->line) + ") add up to " + Str(r->total_views) + " + " + Str(r->total_samplers) + ": D3D12 refuses the set (Vulkan does not count)");
          }
        }
      }
      if (judged) ++judged_n;
      if (whole && exact_regions.size() == 1) Bindings(f, i, LhsOf(f, i), *exact_regions[0], label, creators);
    }
  }

  // ---- the binding code
  // The receiver chain of the call whose name is token k (after '.' or '->'), without [..] and .get(), as text.
  std::string ReceiverOf(SrcFile& f, int k) {
    const Toks& toks = f.toks;
    int j = k - 2;
    std::vector<std::string> chain;
    while (j >= 0) {
      const Tok& u = toks[j];
      if (u.text == "]" && f.pair[j] >= 0) {
        j = f.pair[j] - 1;
        continue;
      }
      if (u.text == ")" && f.pair[j] >= 2 && toks[f.pair[j] - 1].text == "get" && (toks[f.pair[j] - 2].text == "." || toks[f.pair[j] - 2].text == "->")) {
        j = f.pair[j] - 3;
        continue;
      }
      if (u.kind == K::Id || (u.kind == K::P && (u.text == "." || u.text == "->"))) {
        chain.push_back(u.text);
        --j;
        continue;
      }
      break;
    }
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) out += *it;
    return out;
  }

  // Every set* call in the file: (token index of the call name, receiver text).
  std::vector<std::pair<int, std::string>> BindingCalls(SrcFile& f) {
    const Toks& toks = f.toks;
    std::vector<std::pair<int, std::string>> out;
    for (int k = 2; k < static_cast<int>(toks.size()) - 1; ++k) {
      const Tok& t = toks[k];
      if (t.kind == K::Id && IsCallApi(t.text) && toks[k + 1].text == "(" && toks[k - 1].kind == K::P && (toks[k - 1].text == "." || toks[k - 1].text == "->") && f.pair[k + 1] >= 0)
        out.emplace_back(k, ReceiverOf(f, k));
    }
    return out;
  }

  // Loop variables (`for (T i = A; i < B; ++i)`) around token k as value sets.
  Env LoopEnv(SrcFile& f, int k) {
    Env env;
    const Toks& toks = f.toks;
    for (int i = 0; i < k; ++i) {
      const Tok& t = toks[i];
      if (t.kind == K::Id && t.text == "for" && i + 1 < static_cast<int>(toks.size()) && toks[i + 1].text == "(" && f.pair[i + 1] >= 0) {
        int lo, hi, nxt;
        BodyOf(f, f.pair[i + 1] + 1, static_cast<int>(toks.size()), &lo, &hi, &nxt);
        if (lo - 1 <= k && k <= hi) {
          const int c = f.pair[i + 1];
          const Toks head = Slice(toks, i + 2, c);
          int eq = -1, semi = -1;
          for (size_t x = 0; x < head.size(); ++x) {
            if (head[x].text == "=" && eq < 0) eq = static_cast<int>(x);
            if (head[x].text == ";" && semi < 0) semi = static_cast<int>(x);
          }
          if (eq >= 1 && semi >= 0) {
            const std::string var = head[eq - 1].text;
            const std::optional<Int> n = Trip(f, i + 1, c);
            const std::optional<Int> a = ev.One(Slice(head, eq + 1, semi), f, i);
            if (n && a) {
              std::set<Int> s;
              for (Int x = *a; x < *a + *n; ++x) s.insert(x);
              env[var] = s;
            }
          }
        }
      }
    }
    return env;
  }

  void CheckCall(SrcFile& f, int line, const std::string& label, const std::string& api, const std::vector<Toks>& args, const Item& it, Int iv, const Region& r) {
    if (!CallAccepts(api, it.Ty())) {
      Error(f.path, line, "type", label + ": " + api + " fills descriptor " + Str(iv) + ", which the builder (" + r.file->path + ":" + std::to_string(it.line) + ") declares as a " + it.kind + " range");
      return;
    }
    if (api == "setBuffer" && (it.Ty() == "structured" || it.Ty() == "rw-structured")) {
      const bool missing = args.size() < 4 || In(Jt(args[3]), {"nullptr", "0", "NULL"});
      if (missing)
        Error(f.path, line, "view",
              label + ": setBuffer fills descriptor " + Str(iv) + ", a structured buffer range (" + r.file->path + ":" + std::to_string(it.line) +
                  "), without its structure view (D3D12 reads stride and element count from it)");
    }
  }

  void Bindings(SrcFile& f, int call_idx, const std::string& key, const Region& r, const std::string& label, const std::map<std::string, int>& creators) {
    if (key.empty()) return;
    const Toks& toks = f.toks;
    const std::string last = LastName(key);
    std::vector<std::pair<int, std::string>> calls;
    for (const auto& kc : BindingCalls(f)) {
      auto c = creators.find(last);
      const int count = c == creators.end() ? 0 : c->second;
      if (kc.second == key || (LastName(kc.second) == last && count <= 1)) calls.push_back(kc);
    }
    if (calls.empty()) {
      Add("NOTE", f.path, toks[call_idx].line, "nobinding",
          label + ": no setTexture/setBuffer call on '" + key + "' in this file (the set is filled elsewhere): the descriptors are not checked");
      return;
    }
    std::set<Int> written;
    bool all_known = true;
    for (const auto& kc : calls) {
      const int k = kc.first;
      ++bindings_n;
      const std::string api = toks[k].text;
      const std::vector<Toks> args = SplitArgs(f, k + 1);
      const int line = toks[k].line;
      Vals idx;
      if (!args.empty()) {
        const std::string a0 = Jt(args[0]);
        auto as = r.assigned.find(a0);
        if (as != r.assigned.end()) idx = Vals::Of(as->second);
        else idx = ev.Eval(args[0], f, k, LoopEnv(f, k));
      }
      if (!idx.ok) {
        all_known = false;
        continue;
      }
      ++bindings_judged_n;
      for (Int iv : idx.v) {
        if (iv >= r.total) {
          Error(f.path, line, "index",
                label + ": " + api + "(" + Str(iv) + ", ..) is past the set: the builder (" + r.file->path + ":" + std::to_string(r.line) + ") has " + Str(r.total) +
                    " descriptors (D3D12 writes into the next set's slots of the shared heap)");
          continue;
        }
        const Item* it = r.TypeAt(iv);
        if (it == nullptr) continue;
        written.insert(iv);
        CheckCall(f, line, label, api, args, *it, iv, r);
      }
    }
    if (all_known) {
      for (const Item& it : r.items) {
        if (!it.placed || !it.via.empty()) continue;  // a helper's ranges are written where the helper's struct is used
        for (Int d = it.start; d < std::min(it.start + it.length, it.start + 64); ++d) {
          if (!written.count(d)) {
            Add("WARN", f.path, toks[call_idx].line, "unwritten", label + ": descriptor " + Str(d) + " (" + it.kind + ", line " + std::to_string(it.line) + " of the builder) is never written in this file");
            break;
          }
        }
      }
    }
  }

  // set* calls on a set that is a parameter, whose index is a member of a helper's returned struct (b.pool): the member's range type is checked.
  void MemberCalls(SrcFile& f) {
    std::set<std::string> known;
    const Toks& toks = f.toks;
    for (int i = 1; i < static_cast<int>(toks.size()) - 1; ++i) {
      if (toks[i].text == "CreateDescriptorSet" && toks[i + 1].text == "(" && toks[i - 1].kind == K::P && (toks[i - 1].text == "." || toks[i - 1].text == "->")) known.insert(LhsOf(f, i));
    }
    for (const auto& kc : BindingCalls(f)) {
      if (known.count(kc.second)) continue;
      const int k = kc.first;
      const std::vector<Toks> args = SplitArgs(f, k + 1);
      if (args.empty()) continue;
      const Toks& a0 = args[0];
      if (a0.empty() || a0.back().kind != K::Id || !members.count(a0.back().text) || !(a0.size() == 1 || a0.size() == 3)) continue;
      const std::string ty = members[a0.back().text];
      if (ty.empty()) continue;
      const std::string api = toks[k].text;
      const int line = toks[k].line;
      const std::string label = Jt(a0);
      if (!CallAccepts(api, ty)) {
        Error(f.path, line, "type", label + ": " + api + " fills a " + ty + " range of the helper's builder");
      } else if (api == "setBuffer" && (ty == "structured" || ty == "rw-structured") && (args.size() < 4 || In(Jt(args[3]), {"nullptr", "0", "NULL"}))) {
        Error(f.path, line, "view", label + ": setBuffer fills a structured buffer range of the helper's builder without its structure view");
      }
    }
  }

  void Run() {
    CollectDefs();
    for (const std::string& p : tree.interesting) CollectRegions(tree.Get(p));
    for (const std::string& p : tree.interesting) Calls(tree.Get(p));
    for (const std::string& p : tree.interesting) MemberCalls(tree.Get(p));
    for (const std::string& p : tree.interesting) {
      SrcFile& f = tree.Get(p);
      if (Tree::Basename(p) == "device.cpp") continue;
      const Toks& toks = f.toks;
      for (int i = 2; i < static_cast<int>(toks.size()) - 2; ++i) {
        if (toks[i].text == "createDescriptorSet" && toks[i + 1].text == "(")
          Add("WARN", p, toks[i].line, "bypass", "createDescriptorSet is called on the Plume device directly: the counts are not checked (use Device::CreateDescriptorSet)");
        if (toks[i].text == "create" && toks[i + 1].text == "(" && toks[i - 1].text == "." && toks[i - 2].kind == K::Id && regions.count({p, toks[i - 2].text}))
          Add("WARN", p, toks[i].line, "bypass", toks[i - 2].text + ".create(..) makes a set from a descriptor builder outside Device::CreateDescriptorSet");
      }
    }
  }

  std::vector<std::string> Report(bool show_agree) const {
    std::vector<std::string> out;
    for (const Line& l : lines) out.push_back(l.level + " " + l.path + ":" + std::to_string(l.line) + " [" + l.kind + "] " + l.text);
    if (show_agree) {
      for (const std::string& a : agree) out.push_back("OK " + a);
    }
    out.push_back("descriptor sets: " + std::to_string(calls_n) + " CreateDescriptorSet calls, " + std::to_string(judged_n) + " judged against their builders, " + std::to_string(unjudged_n) +
                  " unjudged, " + std::to_string(bindings_n) + " binding calls (" + std::to_string(bindings_judged_n) + " with a known index), " + std::to_string(errors_n) + " errors");
    return out;
  }

  int CountLevel(const std::string& level) const {
    int n = 0;
    for (const Line& l : lines) n += l.level == level;
    return n;
  }
  std::vector<std::string> KindsOf(const std::string& level) const {
    std::vector<std::string> k;
    for (const Line& l : lines) {
      if (l.level == level) k.push_back(l.kind);
    }
    std::sort(k.begin(), k.end());
    return k;
  }
};

// ------------------------------------------------------------------------------------------------------------------------------------------ the sources

std::string ReadFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A folder on disk.
bool AuditDirectory(const std::string& dir, bool show, bool strict, int* errors, int* unjudged) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  std::vector<std::string> names;
  const fs::path root(dir);
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const fs::path& p = it->path();
    if (it->is_directory(ec)) {
      const std::string n = p.filename().string();
      if (n == ".git" || n == "thirdparty" || n == "build" || n == "out") it.disable_recursion_pending();
      continue;
    }
    std::string rel = fs::relative(p, root, ec).generic_string();
    names.push_back(rel);
  }
  Tree tree;
  tree.read = [&](const std::string& rel) { return ReadFile(root / rel); };
  tree.Init(names);
  Audit audit(tree);
  audit.Run();
  for (const std::string& line : audit.Report(show)) std::printf("%s\n", line.c_str());
  *errors = audit.errors_n;
  *unjudged = audit.unjudged_n;
  (void)strict;
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------------------ selftest

const std::string kTwo = "void F() {\n b_.begin();\n b_.addReadWriteTexture(0);\n b_.addReadWriteByteAddressBuffer(1);\n b_.end();\n";
const std::string kBase =
    "void F() {\n b_.begin();\n a_ = b_.addReadWriteTexture(0);\n c_ = b_.addReadWriteByteAddressBuffer(1);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 2, 0, \"x\");\n";
const std::string kEns =
    "bool Ens(S& out, plume::RenderDescriptorSetBuilder& builder, int second, const char* name) {\n out.set = d->CreateDescriptorSet(builder, second ? 2u : 1u, 0, name);\n return true;\n}\n";
const std::string kHitH =
    "struct Hit { static constexpr uint32_t kBindingCount = 4; };\nstruct World { static constexpr uint32_t kBindingCount = rt::kBindingCount; };\nnamespace rt { inline constexpr uint32_t kBindingCount = 2; }\n";
const std::string kHitCpp =
    "Hit::Bindings Hit::AddB(plume::RenderDescriptorSetBuilder& builder) {\n  Bindings o;\n  o.a = builder.addByteAddressBuffer(6);\n  o.b = builder.addStructuredBuffer(7);\n"
    "  o.c = builder.addStructuredBuffer(8);\n  o.d = builder.addStructuredBuffer(9);\n  return o;\n}\n"
    "World::Bindings World::AddB(plume::RenderDescriptorSetBuilder& builder) {\n  Bindings o;\n  o.w = builder.addStructuredBuffer(10);\n  o.l = builder.addStructuredBuffer(11);\n  return o;\n}\n";
const std::string kHitUse =
    "#include \"h.h\"\nvoid G() {\n b_.begin();\n b_.addStructuredBuffer(0);\n Hit::AddB(b_);\n World::AddB(b_);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1 + Hit::kBindingCount + World::kBindingCount, 0, \"x\");\n}\n";
const std::string kLoop3 = "void F() {\n b_.begin();\n for (uint32_t i = 0; i < 3; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 3, 0, \"x\");\n";
const std::string kInitUse = "void R() {\n q_.begin();\n q_.addReadWriteTexture(0);\n q_.addReadWriteTexture(1);\n q_.end();\n g_.I(q_);\n}\n";

std::string InitWith(int count) {
  return "void I(plume::RenderDescriptorSetBuilder& output_builder) {\n o_ = &output_builder;\n}\nbool S() {\n set_ = d->CreateDescriptorSet(*o_, " + std::to_string(count) +
         ", 0, \"x\");\n return true;\n}\n";
}

struct Case {
  std::string name;
  std::map<std::string, std::string> files;
  std::vector<std::string> errors;
  int unjudged;
  std::vector<std::string> warns;
};

std::vector<Case> Cases() {
  const std::string c1 = " s_ = d->CreateDescriptorSet(b_, ";
  auto make = [&](const std::string& counts) { return kTwo + c1 + counts + ", \"x\");\n}\n"; };
  auto set_sources = [](const std::string& text) { return std::map<std::string, std::string>{{"a.cpp", text}}; };
  std::vector<Case> c;
  c.push_back({"the city shadow mistake: two ranges, a count of 1", set_sources(make("1, 0")), {"count"}, 0, {}});
  c.push_back({"two ranges, two views", set_sources(make("2, 0")), {}, 0, {}});
  c.push_back({"a u suffix", set_sources(make("2u, 0u")), {}, 0, {}});
  c.push_back({"one view too many", set_sources(make("3, 0")), {"count"}, 0, {}});
  c.push_back({"a sampler count on a view set", set_sources(make("2, 1")), {"count"}, 0, {}});
  c.push_back({"a comment", set_sources(kTwo + " // s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"), {}, 0, {}});
  c.push_back({"a constant count", set_sources("constexpr uint32_t kViews = 2;\n" + make("kViews, 0")), {}, 0, {}});
  c.push_back({"a wrong constant count", set_sources("constexpr uint32_t kViews = 5;\n" + make("kViews, 0")), {"count"}, 0, {}});
  c.push_back({"an unknown count", set_sources(make("n, 0")), {}, 1, {}});
  c.push_back({"a namespace alias constant",
               {{"r.h", "namespace gi_update { inline constexpr uint32_t kSetViews = 2; }\n"},
                {"a.cpp", "#include \"r.h\"\nnamespace rules = gi_update;\nconstexpr uint32_t kSetViews = rules::kSetViews;\n" + make("kSetViews, 0")}},
               {}, 0, {}});
  c.push_back({"an enumerator", set_sources("enum class Bind : uint32_t { kA, kB, kC };\n" + make("uint32_t(Bind::kC), 0")), {}, 0, {}});
  c.push_back({"class constants and helpers that add ranges", {{"h.h", kHitH}, {"a.cpp", kHitCpp + kHitUse}}, {}, 0, {}});
  c.push_back({"a helper that adds one more than its constant says",
               {{"h.h", "struct Hit { static constexpr uint32_t kBindingCount = 3; };\n"},
                {"a.cpp", "#include \"h.h\"\nvoid G() {\n b_.begin();\n Hit::AddB(b_);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, Hit::kBindingCount, 0, \"x\");\n}\n" + kHitCpp}},
               {"count"}, 0, {}});
  c.push_back({"a loop of ranges",
               set_sources("constexpr uint32_t kN = 5;\nvoid F() {\n b_.begin();\n for (uint32_t i = 0; i < kN; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, uint32_t(kN), 0, \"x\");\n}\n"),
               {}, 0, {}});
  c.push_back({"a loop of ranges, one short",
               set_sources("constexpr uint32_t kN = 5;\nvoid F() {\n b_.begin();\n for (uint32_t i = 0; i < kN; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 4, 0, \"x\");\n}\n"),
               {"count"}, 0, {}});
  c.push_back({"samplers", set_sources("void F() {\n b.begin();\n b.addSampler(0, 8);\n b.addTexture(1);\n b.end();\n x = d.CreateDescriptorSet(b, 1, 8, \"s\");\n}\n"), {}, 0, {}});
  c.push_back({"samplers, views swapped", set_sources("void F() {\n b.begin();\n b.addSampler(0, 8);\n b.addTexture(1);\n b.end();\n x = d.CreateDescriptorSet(b, 9, 0, \"s\");\n}\n"), {"count"}, 0, {}});
  c.push_back({"a boundless range", set_sources("constexpr uint32_t kT = 32768;\nvoid F() {\n b.begin();\n b.addTexture(0, kT);\n b.end(true, kT);\n x = d.CreateDescriptorSet(b, kT, 0, \"s\");\n}\n"), {}, 0, {}});
  c.push_back({"a boundless range, wrong size",
               set_sources("constexpr uint32_t kT = 32768;\nvoid F() {\n b.begin();\n b.addTexture(0, kT);\n b.end(true, 100);\n x = d.CreateDescriptorSet(b, kT, 0, \"s\");\n}\n"), {"count"}, 0, {}});
  c.push_back({"a struct member builder",
               set_sources("void F(B& b) {\n b.tlas_builder.begin();\n b.tlas_index = b.tlas_builder.addAccelerationStructure(0);\n b.tlas_builder.end();\n b.tlas_set = d.CreateDescriptorSet(b.tlas_builder, 1, 0, \"x\");\n b.tlas_set->setAccelerationStructure(b.tlas_index, t);\n}\n"),
               {}, 0, {}});
  c.push_back({"check_set_builder_ is not set_builder_",
               set_sources("void F() {\n check_set_builder_.begin();\n check_set_builder_.addReadWriteTexture(0);\n check_set_builder_.addReadWriteByteAddressBuffer(1);\n check_set_builder_.end();\n"
                           " set_builder_.begin();\n set_builder_.addAccelerationStructure(0);\n set_builder_.addStructuredBuffer(1);\n set_builder_.addByteAddressBuffer(2);\n set_builder_.addByteAddressBuffer(3);\n set_builder_.end();\n"
                           " a = d.CreateDescriptorSet(set_builder_, 4, 0, \"a\");\n b = d.CreateDescriptorSet(check_set_builder_, 2, 0, \"b\");\n}\n"),
               {}, 0, {}});
  c.push_back({"a ternary count over a parameter builder",
               set_sources("void F(B& o) {\n o_.begin();\n o_.addReadWriteTexture(0);\n o_.addReadWriteTexture(1);\n o_.end();\n n_.begin();\n n_.addReadWriteTexture(0);\n n_.end();\n"
                           " Ens(a, o_, &aux, \"x\");\n Ens(b, n_, nullptr, \"y\");\n}\n" + kEns),
               {}, 0, {}});
  c.push_back({"a ternary count that misses a builder",
               set_sources("void F(B& o) {\n o_.begin();\n o_.addReadWriteTexture(0);\n o_.addReadWriteTexture(1);\n o_.addReadWriteTexture(2);\n o_.end();\n Ens(a, o_, &aux, \"x\");\n}\n" + kEns),
               {"count"}, 0, {}});
  c.push_back({"a member builder set from a parameter", set_sources(InitWith(2) + kInitUse), {}, 0, {}});
  c.push_back({"a member builder set from a parameter, wrong count", set_sources(InitWith(3) + kInitUse), {"count"}, 0, {}});
  // the binding code
  c.push_back({"binding: right", set_sources(kBase + " s_->setTexture(0, t, l, v);\n s_->setBuffer(1, buf, 4);\n}\n"), {}, 0, {}});
  c.push_back({"binding: through the index variables", set_sources(kBase + " s_->setTexture(a_, t, l, v);\n s_->setBuffer(c_, buf, 4);\n}\n"), {}, 0, {}});
  c.push_back({"binding: an index past the set", set_sources(kBase + " s_->setTexture(0, t, l, v);\n s_->setBuffer(2, buf, 4);\n}\n"), {"index"}, 0, {"unwritten"}});
  c.push_back({"binding: a buffer into a texture range", set_sources(kBase + " s_->setBuffer(0, t, 4);\n s_->setBuffer(1, buf, 4);\n}\n"), {"type"}, 0, {}});
  c.push_back({"binding: a texture into a buffer range", set_sources(kBase + " s_->setTexture(0, t, l, v);\n s_->setTexture(1, t, l, v);\n}\n"), {"type"}, 0, {}});
  c.push_back({"binding: a descriptor never written", set_sources(kBase + " s_->setTexture(0, t, l, v);\n}\n"), {}, 0, {"unwritten"}});
  c.push_back({"binding: a structured buffer without its view",
               set_sources("void F() {\n b_.begin();\n b_.addStructuredBuffer(0);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(0, buf, 16);\n}\n"), {"view"}, 0, {}});
  c.push_back({"binding: a structured buffer with its view",
               set_sources("void F() {\n b_.begin();\n b_.addStructuredBuffer(0);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(0, buf, 16, &rows);\n}\n"), {}, 0, {}});
  c.push_back({"binding: a loop over the index", set_sources(kLoop3 + " for (size_t i = 0; i < 3; ++i) {\n  s_->setTexture(uint32_t(i), t[i], l, v);\n }\n}\n"), {}, 0, {}});
  c.push_back({"binding: a loop one past", set_sources(kLoop3 + " for (size_t i = 0; i < 4; ++i) {\n  s_->setTexture(uint32_t(i), t[i], l, v);\n }\n}\n"), {"index"}, 0, {}});
  c.push_back({"binding: a register that is not the position",
               set_sources("void F() {\n b_.begin();\n b_.addByteAddressBuffer(3);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(3, buf, 4);\n}\n"), {"index"}, 0, {"unwritten"}});
  c.push_back({"binding: a helper's struct member in the wrong kind of call",
               {{"h.h", kHitH},
                {"a.cpp", kHitCpp + kHitUse + "void W(plume::RenderDescriptorSet* set, const Hit::Bindings& b) {\n set->setTexture(b.a, t, l, v);\n set->setBuffer(b.b, buf, 8);\n set->setBuffer(b.c, buf, 8, &rows);\n}\n"}},
               {"type", "view"}, 0, {}});
  c.push_back({"a conditional range",
               set_sources("void F(bool c) {\n b_.begin();\n b_.addReadWriteTexture(0);\n if (c) {\n  b_.addReadWriteTexture(1);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"), {}, 1, {}});
  c.push_back({"an unresolved builder", set_sources("void F() {\n s_ = d->CreateDescriptorSet(other_, 1, 0, \"x\");\n}\n"), {}, 1, {}});
  c.push_back({"the wrong number of arguments", set_sources("void F() {\n s_ = d->CreateDescriptorSet(other_, 1, 0);\n}\n"), {"args"}, 0, {}});
  c.push_back({"a bypass", set_sources("void F() {\n b_.begin();\n b_.addTexture(0);\n b_.end();\n x = dev->createDescriptorSet(desc);\n}\n"), {}, 0, {"bypass"}});
  c.push_back({"a set made in a test file is not read", {{"a_test.cpp", kTwo + " s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"}}, {}, 0, {}});
  return c;
}

std::string Join(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i];
  return out + "]";
}

int RunSelfTest() {
  int fails = 0;
  const std::vector<Case> cases = Cases();
  for (const Case& c : cases) {
    Tree tree;
    tree.read = [&](const std::string& p) { return c.files.at(p); };
    std::vector<std::string> names;
    for (const auto& kv : c.files) names.push_back(kv.first);
    tree.Init(names);
    Audit audit(tree);
    audit.Run();
    std::vector<std::string> want_e = c.errors, want_w = c.warns;
    std::sort(want_e.begin(), want_e.end());
    std::sort(want_w.begin(), want_w.end());
    if (audit.KindsOf("ERROR") != want_e || audit.CountLevel("UNJUDGED") != c.unjudged || audit.KindsOf("WARN") != want_w) {
      ++fails;
      std::printf("SELFTEST FAIL %s: errors %s (want %s), unjudged %d (want %d), warns %s (want %s)\n", c.name.c_str(), Join(audit.KindsOf("ERROR")).c_str(), Join(want_e).c_str(),
                  audit.CountLevel("UNJUDGED"), c.unjudged, Join(audit.KindsOf("WARN")).c_str(), Join(want_w).c_str());
      for (const std::string& line : audit.Report(false)) std::printf("    %s\n", line.c_str());
    }
  }
  std::printf("selftest: %s (%d cases)\n", fails == 0 ? "PASS" : (std::to_string(fails) + " FAILED").c_str(), static_cast<int>(cases.size()));
  return fails;
}

}  // namespace

int main(int argc, char** argv) {
  bool show = false, strict = false, selftest_only = false;
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) show = true;
    else if (std::strcmp(argv[i], "--strict") == 0) strict = true;
    else if (std::strcmp(argv[i], "--selftest-only") == 0) selftest_only = true;
    else dir = argv[i];
  }
  const int fails = RunSelfTest();
  if (fails != 0) return 1;
  if (selftest_only || dir.empty()) {
    if (dir.empty()) std::printf("(no source folder given: only the made-up sources were checked)\n");
    return 0;
  }
  int errors = 0, unjudged = 0;
  if (!AuditDirectory(dir, show, strict, &errors, &unjudged)) {
    std::printf("descriptor_count_test: %s is not a folder\n", dir.c_str());
    return 2;
  }
  if (errors != 0) {
    std::printf("descriptor_count_test: %d descriptor set mismatches\n", errors);
    return 1;
  }
  if (strict && unjudged != 0) {
    std::printf("descriptor_count_test: %d descriptor sets could not be judged (--strict)\n", unjudged);
    return 1;
  }
  std::printf("descriptor_count_test: every descriptor set agrees with its builder\n");
  return 0;
}
