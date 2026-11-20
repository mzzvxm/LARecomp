// Edits of the scene file as line-addressed text patches: see scene_edit.h.

#include "scene_edit.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace mc::cinematic::director {

// ---------------------------------------------------------------------------------------------------------------- words and statements

namespace {

struct Word {
    size_t begin = 0;
    size_t end = 0;
    std::string_view text;
};

// The words of a statement line: blanks separate them, "double quotes" keep one word, a '#' at the start or after a blank (outside quotes) starts a comment.
// `comment` is where that comment starts (npos: none).
std::vector<Word> SplitWords(std::string_view line, size_t* comment = nullptr) {
    std::vector<Word> words;
    if (comment) *comment = std::string_view::npos;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') {
            if (comment) *comment = i;
            break;
        }
        size_t j = i;
        if (c == '"') {
            j = i + 1;
            while (j < n && line[j] != '"') ++j;
            if (j < n) ++j;
        } else {
            while (j < n && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
        }
        words.push_back({i, j, line.substr(i, j - i)});
        i = j;
    }
    return words;
}

std::string_view TrimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool IsBlank(std::string_view s) {
    return TrimRight(s).empty() || s.find_first_not_of(" \t\r") == std::string_view::npos;
}

}  // namespace

bool ParseSceneTime(std::string_view word, double* seconds) {
    if (word.empty()) return false;
    const auto number = [](std::string_view s, double* v, bool allow_fraction) {
        if (s.empty()) return false;
        size_t i = 0;
        size_t digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i, ++digits;
        if (i < s.size() && s[i] == '.') {
            if (!allow_fraction) return false;
            ++i;
            size_t frac = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i, ++frac;
            if (frac == 0 && digits == 0) return false;
            digits += frac;
        }
        if (i != s.size() || digits == 0) return false;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
        return r.ec == std::errc() && r.ptr == s.data() + s.size();
    };
    const size_t colon = word.find(':');
    if (colon == std::string_view::npos) {
        double v = 0.0;
        if (!number(word, &v, true)) return false;
        *seconds = v;
        return true;
    }
    double m = 0.0, s = 0.0;
    if (!number(word.substr(0, colon), &m, false) || !number(word.substr(colon + 1), &s, true)) return false;
    *seconds = m * 60.0 + s;
    return true;
}

std::string FormatSceneTime(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

StatementInfo ReadStatement(std::string_view line) {
    StatementInfo info;
    const std::vector<Word> w = SplitWords(line);
    if (w.empty()) return info;
    info.is_statement = true;
    info.verb = std::string(w[0].text);
    info.track = info.verb;
    double t = 0.0;
    if ((info.verb == "cam" || info.verb == "cut" || info.verb == "end") && w.size() > 1 && ParseSceneTime(w[1].text, &t)) {
        info.has_time = true;
        info.t = t;
        info.time_begin = w[1].begin;
        info.time_end = w[1].end;
        info.track = info.verb == "end" ? "end" : "camera";
        return info;
    }
    if (info.verb == "at" && w.size() > 2 && ParseSceneTime(w[1].text, &t)) {
        info.has_time = true;
        info.t = t;
        info.time_begin = w[1].begin;
        info.time_end = w[1].end;
        info.verb = std::string(w[2].text);
        info.track = info.verb;
        if ((info.verb == "car" || info.verb == "set") && w.size() > 3) info.track = info.verb + ":" + std::string(w[3].text);
    }
    return info;
}

std::string StripComment(std::string_view line) {
    size_t comment = std::string_view::npos;
    SplitWords(line, &comment);
    std::string_view s = comment == std::string_view::npos ? line : line.substr(0, comment);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    return std::string(TrimRight(s));
}

const char* EditKindName(EditKind kind) {
    switch (kind) {
        case EditKind::kInsert: return "insert";
        case EditKind::kReplace: return "replace";
        case EditKind::kDelete: return "delete";
        case EditKind::kMove: return "move";
        case EditKind::kParam: return "param";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------------------- the ops as JSON

bool EditOpFromJson(const Json& json, EditOp* out, std::string* error) {
    if (!json.is_object()) {
        *error = "an op is an object: {\"op\":\"insert\",\"text\":\"...\"}";
        return false;
    }
    const Json& kind = json.Get("op");
    if (!kind.is_string()) {
        *error = "an op needs 'op' (insert, replace, delete, move or param)";
        return false;
    }
    EditOp op;
    const std::string& k = kind.AsString();
    if (k == "insert") op.kind = EditKind::kInsert;
    else if (k == "replace") op.kind = EditKind::kReplace;
    else if (k == "delete") op.kind = EditKind::kDelete;
    else if (k == "move") op.kind = EditKind::kMove;
    else if (k == "param") op.kind = EditKind::kParam;
    else {
        *error = "unknown op '" + k + "' (insert, replace, delete, move or param)";
        return false;
    }
    const auto allowed = [&](std::initializer_list<const char*> names) {
        for (const Json::Member& m : json.members()) {
            if (m.first == "op") continue;
            bool ok = false;
            for (const char* n : names) ok = ok || m.first == n;
            if (!ok) {
                *error = k + " has no field '" + m.first + "'";
                return false;
            }
        }
        return true;
    };
    const auto need_string = [&](const char* name, std::string* dst) {
        const Json& v = json.Get(name);
        if (!v.is_string()) {
            *error = k + " needs '" + name + "' (a string)";
            return false;
        }
        *dst = v.AsString();
        return true;
    };
    const auto need_line = [&]() {
        const Json& v = json.Get("line");
        if (!v.is_integer() || v.AsInt() < 1 || v.AsInt() > 10000000) {
            *error = k + " needs 'line' (a line number, from 1)";
            return false;
        }
        op.line = static_cast<int>(v.AsInt());
        return need_string("expect", &op.expect);
    };
    switch (op.kind) {
        case EditKind::kInsert:
            if (!allowed({"text"}) || !need_string("text", &op.text)) return false;
            break;
        case EditKind::kReplace:
            if (!allowed({"line", "expect", "text"}) || !need_line() || !need_string("text", &op.text)) return false;
            break;
        case EditKind::kDelete:
            if (!allowed({"line", "expect"}) || !need_line()) return false;
            break;
        case EditKind::kMove: {
            if (!allowed({"line", "expect", "t"}) || !need_line()) return false;
            const Json& t = json.Get("t");
            if (!t.is_number() || !std::isfinite(t.AsNumber()) || t.AsNumber() < 0.0) {
                *error = "move needs 't' (a time in seconds, 0 or more)";
                return false;
            }
            op.has_t = true;
            op.t = t.AsNumber();
            break;
        }
        case EditKind::kParam: {
            if (!allowed({"line", "expect", "key", "value"}) || !need_line() || !need_string("key", &op.key)) return false;
            const Json& v = json.Get("value");
            if (v.is_number()) {
                op.value = FormatNumber(v.AsNumber());
            } else if (v.is_string()) {
                op.value = v.AsString();
            } else {
                *error = "param needs 'value' (a string or a number)";
                return false;
            }
            break;
        }
    }
    *out = std::move(op);
    return true;
}

Json EditOpToJson(const EditOp& op) {
    Json j = Json::Object();
    j.Set("op", EditKindName(op.kind));
    switch (op.kind) {
        case EditKind::kInsert:
            j.Set("text", op.text);
            break;
        case EditKind::kReplace:
            j.Set("line", op.line);
            j.Set("expect", op.expect);
            j.Set("text", op.text);
            break;
        case EditKind::kDelete:
            j.Set("line", op.line);
            j.Set("expect", op.expect);
            break;
        case EditKind::kMove:
            j.Set("line", op.line);
            j.Set("expect", op.expect);
            j.Set("t", op.t);
            break;
        case EditKind::kParam:
            j.Set("line", op.line);
            j.Set("expect", op.expect);
            j.Set("key", op.key);
            j.Set("value", op.value);
            break;
    }
    return j;
}

// ---------------------------------------------------------------------------------------------------------------- lines of a text

namespace {

struct Lines {
    std::vector<std::string> lines;
    std::string eol = "\n";
    bool final_newline = false;
};

Lines SplitText(std::string_view text) {
    Lines out;
    const size_t first_nl = text.find('\n');
    if (first_nl != std::string_view::npos && first_nl > 0 && text[first_nl - 1] == '\r') out.eol = "\r\n";
    out.final_newline = !text.empty() && text.back() == '\n';
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        const size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.lines.emplace_back(line);
        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    return out;
}

std::string JoinText(const std::vector<std::string>& lines, const std::string& eol, bool final_newline) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || final_newline) out += eol;
    }
    return out;
}

struct Entry {
    int orig = 0;  // the line number in the text the caller read; 0 for a line an op made
    std::string text;
    bool written = false;
};

EditResult Bad(ErrorCode code, int op, std::string message, Json details = Json()) {
    EditResult r;
    r.ok = false;
    r.code = code;
    r.failed_op = op;
    r.message = "op " + std::to_string(op) + ": " + std::move(message);
    r.details = std::move(details);
    return r;
}

// The text of a line for a `stale` answer and for comparing: without trailing blanks and CR.
std::string_view ForCompare(std::string_view s) {
    return TrimRight(s);
}

}  // namespace

std::string TextHash(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::vector<int> ChangedLines(std::string_view before, std::string_view after) {
    const Lines a = SplitText(before);
    const Lines b = SplitText(after);
    const size_t n = a.lines.size();
    const size_t m = b.lines.size();
    size_t pre = 0;
    while (pre < n && pre < m && a.lines[pre] == b.lines[pre]) ++pre;
    size_t suf = 0;
    while (suf < n - pre && suf < m - pre && a.lines[n - 1 - suf] == b.lines[m - 1 - suf]) ++suf;
    const size_t an = n - pre - suf;
    const size_t bn = m - pre - suf;
    std::vector<int> changed;
    if (an == 0) {
        for (size_t j = 0; j < bn; ++j) changed.push_back(static_cast<int>(pre + j + 1));
        return changed;
    }
    if (bn == 0) return changed;  // only deletions: no line of `after` is new
    constexpr size_t kCap = 4u * 1000u * 1000u;
    if (an * bn > kCap) {  // too much to compare properly: by position
        for (size_t j = 0; j < bn; ++j) {
            if (j >= an || a.lines[pre + j] != b.lines[pre + j]) changed.push_back(static_cast<int>(pre + j + 1));
        }
        return changed;
    }
    // Longest common subsequence of the middle part: dp[i][j] = the LCS of a[i..], b[j..].
    const size_t w = bn + 1;
    std::vector<uint16_t> dp((an + 1) * w, 0);
    for (size_t i = an; i-- > 0;) {
        for (size_t j = bn; j-- > 0;) {
            if (a.lines[pre + i] == b.lines[pre + j]) dp[i * w + j] = static_cast<uint16_t>(std::min<size_t>(dp[(i + 1) * w + j + 1] + 1, 65535));
            else dp[i * w + j] = std::max(dp[(i + 1) * w + j], dp[i * w + j + 1]);
        }
    }
    std::vector<bool> kept(bn, false);
    size_t i = 0, j = 0;
    while (i < an && j < bn) {
        if (a.lines[pre + i] == b.lines[pre + j]) {
            kept[j] = true;
            ++i;
            ++j;
        } else if (dp[(i + 1) * w + j] >= dp[i * w + j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
    for (size_t k = 0; k < bn; ++k) {
        if (!kept[k]) changed.push_back(static_cast<int>(pre + k + 1));
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------- Apply

EditResult SceneEdit::Apply(std::string_view text, const std::vector<EditOp>& ops, int64_t base_rev) {
    if (ops.empty()) return Bad(ErrorCode::kBadArgs, 0, "there are no ops");
    Lines file = SplitText(text);
    std::vector<Entry> entries;
    entries.reserve(file.lines.size() + ops.size());
    for (size_t i = 0; i < file.lines.size(); ++i) entries.push_back({static_cast<int>(i + 1), file.lines[i], false});
    const int original_lines = static_cast<int>(file.lines.size());
    std::vector<bool> deleted(file.lines.size() + 1, false);

    const auto find = [&](int op_index, int line, const std::string& expect, Entry** out, EditResult* fail) {
        if (line < 1 || line > original_lines) {
            *fail = Bad(ErrorCode::kBadArgs, op_index, "there is no line " + std::to_string(line) + " (the file has " + std::to_string(original_lines) + ")");
            return false;
        }
        if (deleted[static_cast<size_t>(line)]) {
            *fail = Bad(ErrorCode::kBadArgs, op_index, "line " + std::to_string(line) + " was deleted by an earlier op");
            return false;
        }
        for (Entry& e : entries) {
            if (e.orig != line) continue;
            if (ForCompare(e.text) != ForCompare(expect)) {
                Json d = Json::Object();
                d.Set("line", line);
                d.Set("actual", e.text);
                *fail = Bad(ErrorCode::kStale, op_index, "line " + std::to_string(line) + " is not what you read: it says '" + e.text + "'", std::move(d));
                return false;
            }
            *out = &e;
            return true;
        }
        *fail = Bad(ErrorCode::kBadArgs, op_index, "there is no line " + std::to_string(line));
        return false;
    };

    for (size_t oi = 0; oi < ops.size(); ++oi) {
        const EditOp& op = ops[oi];
        const int op_index = static_cast<int>(oi);
        EditResult fail;
        switch (op.kind) {
            case EditKind::kInsert: {
                std::string body = op.text;
                // one line per line of the text; the file's own ending is put back at the end
                std::vector<std::string> new_lines;
                {
                    size_t i = 0;
                    while (i <= body.size()) {
                        size_t nl = body.find('\n', i);
                        std::string line = body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        new_lines.push_back(std::move(line));
                        if (nl == std::string::npos) break;
                        i = nl + 1;
                    }
                    while (!new_lines.empty() && IsBlank(new_lines.back())) new_lines.pop_back();
                }
                if (new_lines.empty() || IsBlank(new_lines.front())) return Bad(ErrorCode::kBadArgs, op_index, "insert needs some text");
                // the line that decides the place: the first statement of the text
                StatementInfo key;
                for (const std::string& l : new_lines) {
                    key = ReadStatement(l);
                    if (key.is_statement) break;
                }
                size_t pos = entries.size();
                bool placed = false;
                if (key.has_time) {
                    for (size_t k = 0; k < entries.size(); ++k) {
                        const StatementInfo st = ReadStatement(entries[k].text);
                        if (st.has_time && st.track == key.track && st.t <= key.t + 1e-9) {
                            pos = k + 1;
                            placed = true;
                        }
                    }
                }
                if (!placed) {
                    // after the header: the last `scene`, `world`, `statelog` or `start` line; else at the first statement; else at the end
                    for (size_t k = 0; k < entries.size(); ++k) {
                        const StatementInfo st = ReadStatement(entries[k].text);
                        if (st.is_statement && !st.has_time && (st.verb == "scene" || st.verb == "world" || st.verb == "statelog" || st.verb == "start")) {
                            pos = k + 1;
                            placed = true;
                        }
                    }
                    if (!placed) {
                        for (size_t k = 0; k < entries.size(); ++k) {
                            if (ReadStatement(entries[k].text).is_statement) {
                                pos = k;
                                placed = true;
                                break;
                            }
                        }
                    }
                }
                std::vector<Entry> add;
                for (std::string& l : new_lines) add.push_back({0, std::move(l), true});
                entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(pos), std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));
                break;
            }
            case EditKind::kReplace: {
                Entry* e = nullptr;
                if (!find(op_index, op.line, op.expect, &e, &fail)) return fail;
                std::vector<std::string> parts;
                {
                    size_t i = 0;
                    const std::string& body = op.text;
                    while (i <= body.size()) {
                        size_t nl = body.find('\n', i);
                        std::string line = body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        parts.push_back(std::move(line));
                        if (nl == std::string::npos) break;
                        i = nl + 1;
                    }
                }
                const size_t idx = static_cast<size_t>(e - entries.data());
                e->text = parts[0];
                e->written = true;
                if (parts.size() > 1) {
                    std::vector<Entry> add;
                    for (size_t k = 1; k < parts.size(); ++k) add.push_back({0, parts[k], true});
                    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(idx + 1), add.begin(), add.end());
                }
                break;
            }
            case EditKind::kDelete: {
                Entry* e = nullptr;
                if (!find(op_index, op.line, op.expect, &e, &fail)) return fail;
                deleted[static_cast<size_t>(op.line)] = true;
                entries.erase(entries.begin() + (e - entries.data()));
                break;
            }
            case EditKind::kMove: {
                Entry* e = nullptr;
                if (!find(op_index, op.line, op.expect, &e, &fail)) return fail;
                const StatementInfo st = ReadStatement(e->text);
                if (!st.has_time) return Bad(ErrorCode::kBadArgs, op_index, "line " + std::to_string(op.line) + " has no time word to move");
                if (!op.has_t || !(op.t >= 0.0) || !std::isfinite(op.t)) return Bad(ErrorCode::kBadArgs, op_index, "move needs a time of 0 or more");
                e->text = e->text.substr(0, st.time_begin) + FormatSceneTime(op.t) + e->text.substr(st.time_end);
                e->written = true;
                break;
            }
            case EditKind::kParam: {
                Entry* e = nullptr;
                if (!find(op_index, op.line, op.expect, &e, &fail)) return fail;
                if (op.key.empty() || op.key.find_first_of(" \t\"#") != std::string::npos) return Bad(ErrorCode::kBadArgs, op_index, "param needs a key that is one word");
                if (op.value.empty()) return Bad(ErrorCode::kBadArgs, op_index, "param needs a value");
                size_t comment = std::string_view::npos;
                const std::vector<Word> words = SplitWords(e->text, &comment);
                if (words.empty()) return Bad(ErrorCode::kBadArgs, op_index, "line " + std::to_string(op.line) + " is not a statement");
                const size_t value_words = SplitWords(op.value).size() == 0 ? 1 : SplitWords(op.value).size();
                size_t idx = words.size();
                for (size_t k = 1; k < words.size(); ++k) {
                    if (words[k].text == op.key) {
                        idx = k;
                        break;
                    }
                }
                if (idx == words.size()) {
                    const size_t at = words.back().end;
                    e->text = e->text.substr(0, at) + " " + op.key + " " + op.value + e->text.substr(at);
                } else if (idx + 1 >= words.size()) {
                    const size_t at = words[idx].end;
                    e->text = e->text.substr(0, at) + " " + op.value + e->text.substr(at);
                } else {
                    const size_t last = std::min(idx + value_words, words.size() - 1);
                    e->text = e->text.substr(0, words[idx + 1].begin) + op.value + e->text.substr(words[last].end);
                }
                e->written = true;
                break;
            }
        }
    }

    EditResult r;
    r.ok = true;
    r.rev = base_rev + 1;
    std::vector<std::string> out_lines;
    out_lines.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        out_lines.push_back(entries[i].text);
        if (entries[i].written) r.lines.push_back(static_cast<int>(i + 1));
    }
    const bool final_newline = file.lines.empty() ? true : file.final_newline;
    r.text = JoinText(out_lines, file.eol, final_newline);
    r.changed = ChangedLines(text, r.text);
    return r;
}

}  // namespace mc::cinematic::director
