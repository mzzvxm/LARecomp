#pragma once
//
// A small, strict JSON reader for larecomp_online_tool: it reads files other programs wrote (the game's
// online_tables.json, see ../online_tables_core.h) and the check reads the tool's own documents with it. Not a general
// library: numbers are kept as a double and, when the text had no fraction or exponent and fits, as an exact integer;
// the depth and the size are capped, so a hostile or damaged file is an error and never a crash.
//

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace larecomp::online_tool::json {

struct Value {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Type type = Type::kNull;
    bool boolean = false;
    double number = 0;
    int64_t integer = 0;
    bool is_integer = false;  // the number was written as a plain integer that fits an int64
    std::string text;
    std::vector<Value> items;         // an array's elements, or an object's values (keys[i] names items[i])
    std::vector<std::string> keys;    // an object's member names, in file order

    bool IsNull() const { return type == Type::kNull; }
    bool IsObject() const { return type == Type::kObject; }
    bool IsArray() const { return type == Type::kArray; }
    bool IsString() const { return type == Type::kString; }
    bool IsNumber() const { return type == Type::kNumber; }
    bool IsBool() const { return type == Type::kBool; }

    // The member `key` of an object (the first one of that name), or null.
    const Value* Get(const std::string& key) const {
        if (type != Type::kObject) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    // "a.b.0.c": members by name and array elements by index; null when any step is missing.
    const Value* Path(const std::string& path) const {
        const Value* v = this;
        size_t pos = 0;
        while (v && pos <= path.size()) {
            size_t dot = path.find('.', pos);
            if (dot == std::string::npos) dot = path.size();
            const std::string step = path.substr(pos, dot - pos);
            if (v->type == Type::kArray) {
                size_t idx = 0;
                if (step.empty() || step.size() > 9) return nullptr;
                for (char c : step) {
                    if (c < '0' || c > '9') return nullptr;
                    idx = idx * 10 + size_t(c - '0');
                }
                v = idx < v->items.size() ? &v->items[idx] : nullptr;
            } else {
                v = v->Get(step);
            }
            pos = dot + 1;
        }
        return v;
    }
    int64_t Int(int64_t fallback = 0) const { return type == Type::kNumber && is_integer ? integer : fallback; }
    const std::string& Text() const { return text; }
};

class Reader {
public:
    // Parses `in` (a whole document). False with `error` set on anything else than one valid JSON value.
    static bool Parse(const std::string& in, Value& out, std::string& error) {
        Reader r(in);
        out = Value{};
        r.SkipSpace();
        if (!r.ParseValue(out, 0)) {
            error = r.error_.empty() ? "invalid JSON" : r.error_;
            return false;
        }
        r.SkipSpace();
        if (r.pos_ != in.size()) {
            error = "text after the JSON value";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    explicit Reader(const std::string& s) : in_(s) {}

    bool Fail(const char* why) {
        if (error_.empty()) error_ = std::string(why) + " at byte " + std::to_string(pos_);
        return false;
    }
    void SkipSpace() {
        while (pos_ < in_.size() && (in_[pos_] == ' ' || in_[pos_] == '\t' || in_[pos_] == '\n' || in_[pos_] == '\r')) ++pos_;
    }
    bool Literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (in_.compare(pos_, n, word) != 0) return Fail("bad literal");
        pos_ += n;
        return true;
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > kMaxDepth) return Fail("nested too deep");
        if (pos_ >= in_.size()) return Fail("unexpected end");
        const char c = in_[pos_];
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') {
            v.type = Value::Type::kString;
            return ParseString(v.text);
        }
        if (c == 't') {
            v.type = Value::Type::kBool;
            v.boolean = true;
            return Literal("true");
        }
        if (c == 'f') {
            v.type = Value::Type::kBool;
            v.boolean = false;
            return Literal("false");
        }
        if (c == 'n') {
            v.type = Value::Type::kNull;
            return Literal("null");
        }
        return ParseNumber(v);
    }

    bool ParseObject(Value& v, int depth) {
        v.type = Value::Type::kObject;
        ++pos_;  // {
        SkipSpace();
        if (pos_ < in_.size() && in_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (pos_ >= in_.size() || in_[pos_] != '"') return Fail("expected a member name");
            std::string key;
            if (!ParseString(key)) return false;
            SkipSpace();
            if (pos_ >= in_.size() || in_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            SkipSpace();
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            v.keys.push_back(std::move(key));
            v.items.push_back(std::move(member));
            SkipSpace();
            if (pos_ < in_.size() && in_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < in_.size() && in_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& v, int depth) {
        v.type = Value::Type::kArray;
        ++pos_;  // [
        SkipSpace();
        if (pos_ < in_.size() && in_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            v.items.push_back(std::move(item));
            SkipSpace();
            if (pos_ < in_.size() && in_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < in_.size() && in_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t& out) {
        if (pos_ + 4 > in_.size()) return Fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = in_[pos_ + size_t(i)];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        pos_ += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // "
        out.clear();
        while (pos_ < in_.size()) {
            const unsigned char c = static_cast<unsigned char>(in_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return Fail("control character in a string");
            if (c != '\\') {
                out += char(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= in_.size()) return Fail("unfinished escape");
            const char e = in_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {  // a surrogate pair
                        if (in_.compare(pos_, 2, "\\u") != 0) return Fail("lone surrogate");
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (!Hex4(lo)) return false;
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
        return Fail("unterminated string");
    }

    bool ParseNumber(Value& v) {
        const size_t start = pos_;
        bool plain = true;
        if (pos_ < in_.size() && in_[pos_] == '-') ++pos_;
        const size_t digits = pos_;
        while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') ++pos_;
        if (pos_ == digits) return Fail("expected a value");
        if (in_[digits] == '0' && pos_ - digits > 1) return Fail("leading zero");
        if (pos_ < in_.size() && in_[pos_] == '.') {
            plain = false;
            ++pos_;
            const size_t frac = pos_;
            while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') ++pos_;
            if (pos_ == frac) return Fail("bad fraction");
        }
        if (pos_ < in_.size() && (in_[pos_] == 'e' || in_[pos_] == 'E')) {
            plain = false;
            ++pos_;
            if (pos_ < in_.size() && (in_[pos_] == '+' || in_[pos_] == '-')) ++pos_;
            const size_t exp = pos_;
            while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') ++pos_;
            if (pos_ == exp) return Fail("bad exponent");
        }
        const std::string text = in_.substr(start, pos_ - start);
        v.type = Value::Type::kNumber;
        v.number = std::strtod(text.c_str(), nullptr);
        v.is_integer = false;
        if (plain && text.size() <= 18) {  // fits an int64 for sure
            v.integer = std::strtoll(text.c_str(), nullptr, 10);
            v.is_integer = true;
        } else if (plain) {
            // 19 digits or more: exact only when it round-trips
            errno = 0;
            const long long n = std::strtoll(text.c_str(), nullptr, 10);
            if (errno == 0) {
                v.integer = n;
                v.is_integer = true;
            }
        }
        return true;
    }

    const std::string& in_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace larecomp::online_tool::json
