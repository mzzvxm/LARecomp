#pragma once
//
// A small JSON writer for larecomp_online_tool: one document per call on stdout, read by the launcher. Strings are
// written as strictly valid UTF-8 (a byte that is not part of a valid sequence becomes U+FFFD, control characters
// become \u00XX), so a hostile server's text can never make the document unreadable; numbers are plain integers,
// except a rating average or a float column of a leaderboard row (Double: a fixed number of significant digits, never
// in the PC's own number format, never NaN or infinity).
//

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "../server_browser/browser_query.h"

namespace larecomp::online_tool {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    JsonWriter& BeginObject() { return Open('{'); }
    JsonWriter& EndObject() { return Close('}'); }
    JsonWriter& BeginArray() { return Open('['); }
    JsonWriter& EndArray() { return Close(']'); }

    // The next value's name; only inside an object.
    JsonWriter& Key(std::string_view name) {
        Separate();
        AppendString(name);
        out_ += pretty_ ? ": " : ":";
        after_key_ = true;
        return *this;
    }

    JsonWriter& String(std::string_view s) {
        Value();
        AppendString(s);
        return *this;
    }
    JsonWriter& Int(int64_t v) {
        Value();
        out_ += std::to_string(v);
        return *this;
    }
    JsonWriter& Uint(uint64_t v) {
        Value();
        out_ += std::to_string(v);
        return *this;
    }
    // A finite number with `digits` significant digits (1..17), written with '.' whatever the PC's language is;
    // NaN and infinity (which JSON cannot hold) become null.
    JsonWriter& Double(double v, int digits = 9) {
        Value();
        if (!(v - v == 0.0)) {  // NaN or infinity
            out_ += "null";
            return *this;
        }
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.*g", digits < 1 ? 1 : (digits > 17 ? 17 : digits), v);
        for (char* p = buf; *p; ++p)
            if (*p == ',') *p = '.';  // a PC that formats numbers with a decimal comma
        out_ += buf;
        return *this;
    }
    JsonWriter& Bool(bool v) {
        Value();
        out_ += v ? "true" : "false";
        return *this;
    }
    JsonWriter& Null() {
        Value();
        out_ += "null";
        return *this;
    }
    // 8 lowercase hex digits as a string: a CRC or a content hash (JSON numbers are doubles to many readers).
    JsonWriter& Hex32(uint32_t v) {
        static const char kDigits[] = "0123456789abcdef";
        char buf[9];
        for (int i = 7; i >= 0; --i, v >>= 4) buf[i] = kDigits[v & 15];
        buf[8] = 0;
        return String(buf);
    }

    // key: value shorthands.
    JsonWriter& Field(std::string_view k, std::string_view v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, const char* v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, const std::string& v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, bool v) { return Key(k).Bool(v); }
    JsonWriter& Field(std::string_view k, int v) { return Key(k).Int(v); }
    JsonWriter& Field(std::string_view k, int64_t v) { return Key(k).Int(v); }
    JsonWriter& Field(std::string_view k, uint32_t v) { return Key(k).Uint(v); }
    JsonWriter& Field(std::string_view k, uint64_t v) { return Key(k).Uint(v); }

    const std::string& str() const { return out_; }

private:
    struct Level {
        bool first = true;
    };

    // Before anything that starts a new element: the comma and the line break.
    void Separate() {
        if (after_key_) return;
        if (!stack_.empty()) {
            if (!stack_.back().first) out_ += ',';
            stack_.back().first = false;
            Newline();
        }
    }
    void Value() {
        Separate();
        after_key_ = false;
    }
    void Newline() {
        if (!pretty_) return;
        out_ += '\n';
        out_.append(stack_.size() * 2, ' ');
    }
    JsonWriter& Open(char c) {
        Value();
        out_ += c;
        stack_.push_back({});
        return *this;
    }
    JsonWriter& Close(char c) {
        const bool empty = stack_.empty() || stack_.back().first;
        if (!stack_.empty()) stack_.pop_back();
        if (!empty) Newline();
        out_ += c;
        return *this;
    }

    void AppendString(std::string_view s) {
        static const char kDigits[] = "0123456789abcdef";
        out_ += '"';
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = p[i];
            if (c < 0x80) {
                switch (c) {
                    case '"': out_ += "\\\""; break;
                    case '\\': out_ += "\\\\"; break;
                    case '\n': out_ += "\\n"; break;
                    case '\r': out_ += "\\r"; break;
                    case '\t': out_ += "\\t"; break;
                    default:
                        if (c < 0x20 || c == 0x7F) {
                            out_ += "\\u00";
                            out_ += kDigits[c >> 4];
                            out_ += kDigits[c & 15];
                        } else {
                            out_ += char(c);
                        }
                }
                ++i;
                continue;
            }
            const size_t n = browser::Utf8SequenceLength(p + i, s.size() - i);
            if (n == 0) {
                out_ += "\xEF\xBF\xBD";  // U+FFFD
                ++i;
                continue;
            }
            out_.append(s.data() + i, n);
            i += n;
        }
        out_ += '"';
    }

    bool pretty_ = false;
    bool after_key_ = false;
    std::string out_;
    std::vector<Level> stack_;
};

}  // namespace larecomp::online_tool
