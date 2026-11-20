// The Director's JSON value: see json.h.

#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mc::cinematic::director {

namespace {

const Json kNull;
const std::string kEmptyString;

// ---------------------------------------------------------------------------------------------------------------- writing

void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The length of the valid UTF-8 sequence at s[i], or 0 when there is none (overlong forms, surrogates and code points past U+10FFFF are not valid).
size_t Utf8Length(std::string_view s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    size_t n = 0;
    uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return n;
}

}  // namespace

void AppendJsonString(std::string_view text, std::string* out) {
    out->push_back('"');
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': out->append("\\\""); ++i; continue;
            case '\\': out->append("\\\\"); ++i; continue;
            case '\b': out->append("\\b"); ++i; continue;
            case '\f': out->append("\\f"); ++i; continue;
            case '\n': out->append("\\n"); ++i; continue;
            case '\r': out->append("\\r"); ++i; continue;
            case '\t': out->append("\\t"); ++i; continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out->append(buf);
            ++i;
            continue;
        }
        if (c < 0x80) {
            out->push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        const size_t n = Utf8Length(text, i);
        if (n == 0) {
            out->append("\xEF\xBF\xBD");  // U+FFFD
            ++i;
        } else {
            out->append(text.substr(i, n));
            i += n;
        }
    }
    out->push_back('"');
}

std::string FormatNumber(double v) {
    if (!std::isfinite(v)) return "null";
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(v));
        std::string s(buf, r.ptr);
        if (s == "-0") return "0";
        return s;
    }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc()) return "null";
    return std::string(buf, r.ptr);
}

// ---------------------------------------------------------------------------------------------------------------- the value

bool Json::is_integer() const {
    return type_ == Type::kNumber && std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) <= 9007199254740992.0;
}

int64_t Json::AsInt(int64_t fallback) const {
    if (type_ != Type::kNumber || !std::isfinite(num_)) return fallback;
    if (num_ >= 9.2e18) return std::numeric_limits<int64_t>::max();
    if (num_ <= -9.2e18) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(num_);
}

const std::string& Json::AsString() const {
    return type_ == Type::kString ? str_ : kEmptyString;
}

const Json& Json::at(size_t i) const {
    if (type_ != Type::kArray || i >= items_.size()) return kNull;
    return items_[i];
}

Json& Json::Push(Json value) {
    if (type_ == Type::kNull) type_ = Type::kArray;
    if (type_ != Type::kArray) return *this;  // not an array: a programming error, ignored
    items_.push_back(std::move(value));
    return items_.back();
}

const Json* Json::Find(std::string_view key) const {
    if (type_ != Type::kObject) return nullptr;
    for (const Member& m : members_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

const Json& Json::Get(std::string_view key) const {
    const Json* j = Find(key);
    return j ? *j : kNull;
}

Json& Json::Set(std::string_view key, Json value) {
    if (type_ == Type::kNull) type_ = Type::kObject;
    static Json sink;
    if (type_ != Type::kObject) return sink;
    for (Member& m : members_) {
        if (m.first == key) {
            m.second = std::move(value);
            return m.second;
        }
    }
    members_.emplace_back(std::string(key), std::move(value));
    return members_.back().second;
}

bool Json::Erase(std::string_view key) {
    if (type_ != Type::kObject) return false;
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->first == key) {
            members_.erase(it);
            return true;
        }
    }
    return false;
}

double Json::NumberOr(std::string_view key, double fallback) const {
    const Json* j = Find(key);
    return j && j->is_number() ? j->num_ : fallback;
}

int64_t Json::IntOr(std::string_view key, int64_t fallback) const {
    const Json* j = Find(key);
    return j && j->is_number() ? j->AsInt(fallback) : fallback;
}

bool Json::BoolOr(std::string_view key, bool fallback) const {
    const Json* j = Find(key);
    return j && j->is_bool() ? j->bool_ : fallback;
}

std::string Json::StringOr(std::string_view key, std::string_view fallback) const {
    const Json* j = Find(key);
    return j && j->is_string() ? j->str_ : std::string(fallback);
}

bool Json::ToVec3(double out[3]) const {
    if (type_ != Type::kArray || items_.size() != 3) return false;
    for (const Json& j : items_) {
        if (!j.is_number() || !std::isfinite(j.num_)) return false;
    }
    for (int i = 0; i < 3; ++i) out[i] = items_[static_cast<size_t>(i)].num_;
    return true;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::kNull: return true;
        case Type::kBool: return bool_ == o.bool_;
        case Type::kNumber: return num_ == o.num_;
        case Type::kString: return str_ == o.str_;
        case Type::kArray: return items_ == o.items_;
        case Type::kObject: return members_ == o.members_;
    }
    return false;
}

void Json::DumpTo(std::string* out) const {
    switch (type_) {
        case Type::kNull: out->append("null"); return;
        case Type::kBool: out->append(bool_ ? "true" : "false"); return;
        case Type::kNumber: out->append(FormatNumber(num_)); return;
        case Type::kString: AppendJsonString(str_, out); return;
        case Type::kArray: {
            out->push_back('[');
            bool first = true;
            for (const Json& j : items_) {
                if (!first) out->push_back(',');
                first = false;
                j.DumpTo(out);
            }
            out->push_back(']');
            return;
        }
        case Type::kObject: {
            out->push_back('{');
            bool first = true;
            for (const Member& m : members_) {
                if (!first) out->push_back(',');
                first = false;
                AppendJsonString(m.first, out);
                out->push_back(':');
                m.second.DumpTo(out);
            }
            out->push_back('}');
            return;
        }
    }
}

std::string Json::Dump() const {
    std::string s;
    DumpTo(&s);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- parsing

namespace {

class Parser {
  public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Run(Json* out, std::string* error) {
        SkipBlanks();
        if (!Value(out, 0)) {
            if (error) *error = Where() + error_;
            return false;
        }
        SkipBlanks();
        if (i_ != s_.size()) {
            error_ = "unexpected text after the value";
            if (error) *error = Where() + error_;
            return false;
        }
        return true;
    }

  private:
    std::string Where() const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return "line " + std::to_string(line) + ", column " + std::to_string(col) + ": ";
    }

    bool Fail(const char* why) {
        error_ = why;
        return false;
    }

    void SkipBlanks() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool Literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    bool Value(Json* out, int depth) {
        if (depth > kMaxJsonDepth) return Fail("nested too deep");
        if (i_ >= s_.size()) return Fail("the text ends where a value is expected");
        const char c = s_[i_];
        switch (c) {
            case '{': return Object(out, depth);
            case '[': return Array(out, depth);
            case '"': {
                std::string str;
                if (!String(&str)) return false;
                *out = Json(std::move(str));
                return true;
            }
            case 't':
                if (!Literal("true")) return Fail("expected a value");
                *out = Json(true);
                return true;
            case 'f':
                if (!Literal("false")) return Fail("expected a value");
                *out = Json(false);
                return true;
            case 'n':
                if (!Literal("null")) return Fail("expected a value");
                *out = Json();
                return true;
            default: break;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(out);
        return Fail("expected a value");
    }

    bool Number(Json* out) {
        const size_t begin = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return Fail("a number ends after the minus sign");
        if (s_[i_] == '0') {
            ++i_;
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            return Fail("a number has no digits");
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return Fail("a number has no digits after the point");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return Fail("a number has no digits in its exponent");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        double v = 0.0;
        const auto r = std::from_chars(s_.data() + begin, s_.data() + i_, v);
        if (r.ec == std::errc::result_out_of_range) return Fail("a number is out of range");
        if (r.ec != std::errc() || r.ptr != s_.data() + i_) return Fail("a number is malformed");
        *out = Json(v);
        return true;
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool Hex4(uint32_t* v) {
        if (i_ + 4 > s_.size()) return false;
        uint32_t x = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = Hex(s_[i_ + static_cast<size_t>(k)]);
            if (h < 0) return false;
            x = (x << 4) | static_cast<uint32_t>(h);
        }
        i_ += 4;
        *v = x;
        return true;
    }

    bool String(std::string* out) {
        ++i_;  // the opening quote
        while (true) {
            if (i_ >= s_.size()) return Fail("a string is not closed");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c < 0x20) return Fail("a control character in a string (write it as \\n, \\t or \\uXXXX)");
            if (c != '\\') {
                if (c < 0x80) {
                    out->push_back(static_cast<char>(c));
                    ++i_;
                } else {
                    const size_t n = Utf8Length(s_, i_);
                    if (n == 0) return Fail("a string is not valid UTF-8");
                    out->append(s_.substr(i_, n));
                    i_ += n;
                }
                continue;
            }
            ++i_;
            if (i_ >= s_.size()) return Fail("a string ends inside an escape");
            const char e = s_[i_++];
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
                    uint32_t cp = 0;
                    if (!Hex4(&cp)) return Fail("a \\u escape needs four hex digits");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            i_ += 2;
                            if (!Hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return Fail("a high surrogate is not followed by a low one");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            return Fail("a high surrogate is not followed by a low one");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Fail("a low surrogate with no high one");
                    }
                    AppendUtf8(cp, out);
                    break;
                }
                default: return Fail("an unknown escape in a string");
            }
        }
    }

    bool Array(Json* out, int depth) {
        ++i_;
        *out = Json::Array();
        SkipBlanks();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            Json v;
            SkipBlanks();
            if (!Value(&v, depth + 1)) return false;
            out->Push(std::move(v));
            SkipBlanks();
            if (i_ >= s_.size()) return Fail("an array is not closed");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or ']' in an array");
        }
    }

    bool Object(Json* out, int depth) {
        ++i_;
        *out = Json::Object();
        SkipBlanks();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            SkipBlanks();
            if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a member name (a string) in an object");
            std::string key;
            if (!String(&key)) return false;
            SkipBlanks();
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':' after a member name");
            ++i_;
            SkipBlanks();
            Json v;
            if (!Value(&v, depth + 1)) return false;
            out->Set(key, std::move(v));
            SkipBlanks();
            if (i_ >= s_.size()) return Fail("an object is not closed");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or '}' in an object");
        }
    }

    std::string_view s_;
    size_t i_ = 0;
    std::string error_;
};

}  // namespace

bool Json::Parse(std::string_view text, Json* out, std::string* error) {
    Json value;
    Parser parser(text);
    if (!parser.Run(&value, error)) return false;
    *out = std::move(value);
    return true;
}

}  // namespace mc::cinematic::director
