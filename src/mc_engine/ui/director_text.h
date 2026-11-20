#pragma once
//
// Small text helpers of the Director panel's pure parts: a scene statement as words, the numbers the scene file writes, the clock of the ruler. Header only, standard
// library only (director_panel_test.cpp builds it with the rest).
//

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::ui::director {

// The words of a scene statement: split at blanks, a "double quoted" part is one word (the quotes are kept), a '#' that starts a word or follows a blank ends the statement.
inline std::vector<std::string> Words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, have = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!quoted && c == '#' && !have) break;
        if (c == '"') {
            quoted = !quoted;
            cur += c;
            have = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
            if (have) out.push_back(std::move(cur));
            cur.clear();
            have = false;
            continue;
        }
        cur += c;
        have = true;
    }
    if (have) out.push_back(std::move(cur));
    return out;
}

// The index of `key` among the words (after the first), or -1.
inline int FindWord(const std::vector<std::string>& words, std::string_view key, size_t from = 1) {
    for (size_t i = from; i < words.size(); ++i) {
        if (words[i] == key) return static_cast<int>(i);
    }
    return -1;
}

// The word after `key`, or "".
inline std::string ValueAfter(const std::vector<std::string>& words, std::string_view key) {
    const int i = FindWord(words, key);
    if (i < 0 || static_cast<size_t>(i) + 1 >= words.size()) return {};
    return words[static_cast<size_t>(i) + 1];
}

// A number as the scene file writes it: three decimals at most, trailing zeros dropped ("3", "3.25", "-0.5").
inline std::string Num(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return (s.empty() || s == "-0") ? "0" : s;
}

// A scene time on the ruler and in the transport bar: "7.25" under a minute, "1:07.3" above it.
inline std::string FormatClock(double t, int decimals = 2) {
    if (!(t > -1e9) || !(t < 1e9)) return "--";
    const bool neg = t < 0.0;
    if (neg) t = -t;
    char buf[48];
    if (t < 60.0) {
        std::snprintf(buf, sizeof(buf), "%s%.*f", neg ? "-" : "", decimals, t);
    } else {
        const int m = static_cast<int>(t / 60.0);
        const double s = t - m * 60.0;
        std::snprintf(buf, sizeof(buf), "%s%d:%0*.*f", neg ? "-" : "", m, decimals ? decimals + 3 : 2, decimals, s);
    }
    return buf;
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string h = Lower(std::string(haystack)), n = Lower(std::string(needle));
    return h.find(n) != std::string::npos;
}

}  // namespace mcla::ui::director
