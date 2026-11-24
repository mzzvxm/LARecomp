// A small JSON reader (parse only, no writer) for the contract's standalone tests and for tools that read the contract files with no library:
// strict RFC 8259 (no comments, no trailing commas), UTF-8, objects keep the file's order and duplicate keys keep the LAST value.
// Header only, no dependency. The game's own code is free to use another reader; this one exists so a test builds with `clang++ -std=c++20` alone.
//
//   larecomp::minijson::Json doc;  std::string err;
//   if (!larecomp::minijson::Parse(text, &doc, &err)) ...      // err: "line 3: expected ':'"
//   const Json* rows = doc.Find("tabs");  doc.Path("a.b.c")

#pragma once

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::minijson {

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  bool IsNull() const { return type == Type::kNull; }
  bool IsBool() const { return type == Type::kBool; }
  bool IsNumber() const { return type == Type::kNumber; }
  bool IsString() const { return type == Type::kString; }
  bool IsArray() const { return type == Type::kArray; }
  bool IsObject() const { return type == Type::kObject; }

  // The member of an object (the last one when a key repeats), or nullptr (also when this is not an object).
  const Json* Find(std::string_view key) const {
    if (type != Type::kObject) return nullptr;
    for (size_t i = o.size(); i-- > 0;) {
      if (o[i].first == key) return &o[i].second;
    }
    return nullptr;
  }

  // A dotted path of object members ("features.upscalers.dlss"); nullptr when any step is missing or not an object.
  const Json* Path(std::string_view dotted) const {
    const Json* cur = this;
    size_t pos = 0;
    while (cur && pos <= dotted.size()) {
      size_t dot = dotted.find('.', pos);
      std::string_view part = dotted.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
      cur = cur->Find(part);
      if (dot == std::string_view::npos) break;
      pos = dot + 1;
    }
    return cur;
  }

  std::string StringOr(std::string_view key, std::string fallback) const {
    const Json* v = Find(key);
    return v && v->IsString() ? v->s : fallback;
  }
  bool BoolOr(std::string_view key, bool fallback) const {
    const Json* v = Find(key);
    return v && v->IsBool() ? v->b : fallback;
  }
  double NumberOr(std::string_view key, double fallback) const {
    const Json* v = Find(key);
    return v && v->IsNumber() ? v->n : fallback;
  }
};

namespace detail {

class Reader {
 public:
  explicit Reader(std::string_view t) : t_(t) {}

  bool Run(Json* out, std::string* err) {
    SkipWs();
    if (!Value(out, 0)) {
      if (err) *err = error_;
      return false;
    }
    SkipWs();
    if (pos_ != t_.size()) {
      Fail("unexpected text after the document");
      if (err) *err = error_;
      return false;
    }
    return true;
  }

 private:
  bool Fail(const char* what) {
    if (error_.empty()) {
      int line = 1;
      for (size_t i = 0; i < pos_ && i < t_.size(); ++i) {
        if (t_[i] == '\n') ++line;
      }
      error_ = "line " + std::to_string(line) + ": " + what;
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\r' || t_[pos_] == '\n')) ++pos_;
  }

  bool Literal(const char* word) {
    size_t n = std::string_view(word).size();
    if (t_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }

  static void AppendUtf8(std::string* out, unsigned cp) {
    if (cp < 0x80) {
      out->push_back(char(cp));
    } else if (cp < 0x800) {
      out->push_back(char(0xC0 | (cp >> 6)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(char(0xE0 | (cp >> 12)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(char(0xF0 | (cp >> 18)));
      out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(unsigned* out) {
    if (pos_ + 4 > t_.size()) return Fail("short \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = t_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  bool String(std::string* out) {
    if (pos_ >= t_.size() || t_[pos_] != '"') return Fail("expected a string");
    ++pos_;
    out->clear();
    while (true) {
      if (pos_ >= t_.size()) return Fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(t_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20) return Fail("control character in a string");
      if (c != '\\') {
        out->push_back(char(c));
        continue;
      }
      if (pos_ >= t_.size()) return Fail("unterminated escape");
      char e = t_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (!(pos_ + 1 < t_.size() && t_[pos_] == '\\' && t_[pos_ + 1] == 'u')) return Fail("lone surrogate");
            pos_ += 2;
            unsigned lo = 0;
            if (!Hex4(&lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return Fail("bad surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("lone surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return Fail("bad escape");
      }
    }
  }

  bool Number(double* out) {
    size_t start = pos_;
    if (pos_ < t_.size() && t_[pos_] == '-') ++pos_;
    if (pos_ >= t_.size()) return Fail("bad number");
    if (t_[pos_] == '0') {
      ++pos_;
    } else if (t_[pos_] >= '1' && t_[pos_] <= '9') {
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
    } else {
      return Fail("bad number");
    }
    if (pos_ < t_.size() && t_[pos_] == '.') {
      ++pos_;
      size_t d = pos_;
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
      if (pos_ == d) return Fail("bad number");
    }
    if (pos_ < t_.size() && (t_[pos_] == 'e' || t_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < t_.size() && (t_[pos_] == '+' || t_[pos_] == '-')) ++pos_;
      size_t d = pos_;
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
      if (pos_ == d) return Fail("bad number");
    }
    std::string text(t_.substr(start, pos_ - start));
    *out = std::strtod(text.c_str(), nullptr);
    return true;
  }

  bool Value(Json* out, int depth) {
    if (depth > 200) return Fail("nested too deep");
    if (pos_ >= t_.size()) return Fail("unexpected end");
    char c = t_[pos_];
    *out = Json{};
    if (c == '{') {
      ++pos_;
      out->type = Json::Type::kObject;
      SkipWs();
      if (pos_ < t_.size() && t_[pos_] == '}') {
        ++pos_;
        return true;
      }
      while (true) {
        SkipWs();
        std::string key;
        if (!String(&key)) return false;
        SkipWs();
        if (pos_ >= t_.size() || t_[pos_] != ':') return Fail("expected ':'");
        ++pos_;
        SkipWs();
        Json v;
        if (!Value(&v, depth + 1)) return false;
        out->o.emplace_back(std::move(key), std::move(v));
        SkipWs();
        if (pos_ < t_.size() && t_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < t_.size() && t_[pos_] == '}') {
          ++pos_;
          return true;
        }
        return Fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++pos_;
      out->type = Json::Type::kArray;
      SkipWs();
      if (pos_ < t_.size() && t_[pos_] == ']') {
        ++pos_;
        return true;
      }
      while (true) {
        SkipWs();
        Json v;
        if (!Value(&v, depth + 1)) return false;
        out->a.push_back(std::move(v));
        SkipWs();
        if (pos_ < t_.size() && t_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < t_.size() && t_[pos_] == ']') {
          ++pos_;
          return true;
        }
        return Fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      out->type = Json::Type::kString;
      return String(&out->s);
    }
    if (c == 't') {
      if (!Literal("true")) return Fail("bad literal");
      out->type = Json::Type::kBool;
      out->b = true;
      return true;
    }
    if (c == 'f') {
      if (!Literal("false")) return Fail("bad literal");
      out->type = Json::Type::kBool;
      return true;
    }
    if (c == 'n') {
      if (!Literal("null")) return Fail("bad literal");
      return true;
    }
    out->type = Json::Type::kNumber;
    return Number(&out->n);
  }

  std::string_view t_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace detail

inline bool Parse(std::string_view text, Json* out, std::string* err = nullptr) {
  // A UTF-8 byte order mark is tolerated (Windows editors write one); the contract files have none.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  return detail::Reader(text).Run(out, err);
}

inline bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace larecomp::minijson
