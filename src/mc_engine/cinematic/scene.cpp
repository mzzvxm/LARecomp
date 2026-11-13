//
// The scene file's parser, timeline and clock: see scene.h for the format and for what this file is not (no game, no log).
//

#include "scene.h"

#include "car_verbs.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <utility>

namespace mc::cinematic {

namespace {

constexpr const char* kWeatherNames[] = {"nice", "cloudy", "stormy", "foggy"};
constexpr double kMaxSeconds = 86400.0;
constexpr double kMaxHour = 48.0;
constexpr double kMaxTimeScale = 10.0;
constexpr double kEpsilon = 1e-9;

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

bool ParseNumber(std::string_view s, double* out) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);  // from_chars does not take a plus sign
    if (s.empty() || s.front() == '+') return false;
    double v = 0.0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// Seconds ("3", "3.5") or minutes:seconds ("1:30.5"); not negative.
bool ParseTime(std::string_view s, double* out) {
    const size_t colon = s.find(':');
    double v = 0.0;
    if (colon == std::string_view::npos) {
        if (!ParseNumber(s, &v)) return false;
    } else {
        double minutes = 0.0, seconds = 0.0;
        if (!ParseNumber(s.substr(0, colon), &minutes) || !ParseNumber(s.substr(colon + 1), &seconds)) return false;
        if (minutes < 0.0 || minutes != std::floor(minutes) || seconds < 0.0 || seconds >= 60.0) return false;
        v = minutes * 60.0 + seconds;
    }
    if (v < 0.0 || v > kMaxSeconds) return false;
    *out = v;
    return true;
}

// A length: more than zero.
bool ParseDuration(std::string_view s, double* out) {
    double v = 0.0;
    if (!ParseNumber(s, &v) || v <= 0.0 || v > kMaxSeconds) return false;
    *out = v;
    return true;
}

// "1/60" or a number of seconds.
bool ParseStep(std::string_view s, double* out) {
    const size_t slash = s.find('/');
    double v = 0.0;
    if (slash == std::string_view::npos) {
        if (!ParseNumber(s, &v)) return false;
    } else {
        double num = 0.0, den = 0.0;
        if (!ParseNumber(s.substr(0, slash), &num) || !ParseNumber(s.substr(slash + 1), &den) || den <= 0.0) return false;
        v = num / den;
    }
    if (v <= 0.0 || v > 10.0) return false;
    *out = v;
    return true;
}

// "a..b" (a range) or a single number. Negative numbers are the caller's to refuse.
bool ParseRangeOrNumber(std::string_view s, double* a, double* b, bool* is_range) {
    const size_t dots = s.find("..");
    if (dots == std::string_view::npos) {
        *is_range = false;
        return ParseNumber(s, a);
    }
    *is_range = true;
    return ParseNumber(s.substr(0, dots), a) && ParseNumber(s.substr(dots + 2), b);
}

bool ParseEase(std::string_view s, Ease* out) {
    const std::string l = Lower(s);
    if (l == "linear") *out = Ease::kLinear;
    else if (l == "in") *out = Ease::kIn;
    else if (l == "out") *out = Ease::kOut;
    else if (l == "inout") *out = Ease::kInOut;
    else return false;
    return true;
}

bool IsIdentifier(std::string_view s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(letter || (i > 0 && digit))) return false;
    }
    return true;
}

bool IsMarkName(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

// A take's name, as the folder will be called (take_folder.h's SanitizeTakeName keeps such a name as it is): letters, digits, _ - . , at most 64,
// starting with a letter, a digit or _.
bool IsTakeName(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!(alnum || (i > 0 && (c == '-' || c == '.')))) return false;
    }
    return true;
}

bool ParseOnOff(std::string_view s, int* out) {
    const std::string l = Lower(s);
    if (l == "on" || l == "1" || l == "true") *out = 1;
    else if (l == "off" || l == "0" || l == "false") *out = 0;
    else return false;
    return true;
}

// Words of one line: split on blanks, "double quotes" keep a word together (and may be empty), '#' at the start of a word
// (outside quotes) starts a comment. *unterminated is set when a quote is not closed.
std::vector<std::string> SplitWords(std::string_view line, bool* unterminated) {
    std::vector<std::string> words;
    std::string cur;
    bool quoted = false, have = false;
    *unterminated = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!quoted && c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) break;
        if (c == '"') {
            quoted = !quoted;
            have = true;
        } else if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
            if (have) words.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (quoted) *unterminated = true;
    if (have) words.push_back(cur);
    return words;
}

bool IsWorldTrack(Track t) {
    switch (t) {
        case Track::kHour:
        case Track::kWeather:
        case Track::kTraffic:
        case Track::kPeds:
        case Track::kParked:
        case Track::kPolice:
        case Track::kTimeScale:
            return true;
        default:
            return false;
    }
}

constexpr const char* kVerbList =
    "hour, weather, traffic, peds, parked, police, timescale, set, label, mark, split, capture, cam, cut, hud, car, start, world, end, statelog";

// The slowest and fastest step a capture can take: the engine timer clamps the scaled step to 0.0001 .. 0.1 s.
constexpr double kMinCaptureFps = 10.0;
constexpr double kMaxCaptureFps = 1000.0;

using Args = std::vector<std::string>;

class Parser {
  public:
    ParseResult Run(std::string_view text) {
        if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // a byte order mark
        int line_no = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string_view::npos) nl = text.size();
            ++line_no;
            Line(line_no, text.substr(pos, nl - pos));
            if (nl >= text.size()) break;
            pos = nl + 1;
        }
        Finish();
        std::stable_sort(result_.diagnostics.begin(), result_.diagnostics.end(),
                         [](const Diagnostic& x, const Diagnostic& y) { return x.line < y.line; });  // the whole-file ones (line 0) first
        result_.scene.events = std::move(events_);
        return std::move(result_);
    }

  private:
    void Error(int line, std::string message) {
        result_.diagnostics.push_back({line, Severity::kError, std::move(message)});
    }
    void Warn(int line, std::string message) {
        result_.diagnostics.push_back({line, Severity::kWarning, std::move(message)});
    }

    void Add(Event e) {
        if (e.t + kEpsilon < max_t_) {
            Warn(e.line, "t=" + Num(e.t) + " comes after line " + std::to_string(max_line_) + " (t=" + Num(max_t_) +
                             "): keys run in time order whatever the file's, but keep it in order");
        }
        if (e.t > max_t_) {
            max_t_ = e.t;
            max_line_ = e.line;
        }
        events_.push_back(std::move(e));
    }

    void Line(int line, std::string_view text) {
        bool unterminated = false;
        const Args w = SplitWords(text, &unterminated);
        if (unterminated) {
            Error(line, "a double quote is not closed");
            return;
        }
        if (w.empty()) return;
        ++statements_;
        const std::string head = Lower(w[0]);
        if (head == "scene") {
            Header(line, w);
            return;
        }
        double t = 0.0;
        size_t first = 1;
        std::string verb = head;
        if (head == "at") {
            if (w.size() < 3) {
                Error(line, "at <time> <statement>: the statement is missing");
                return;
            }
            if (!ParseTime(w[1], &t)) {
                Error(line, "bad time '" + w[1] + "' (seconds, or minutes:seconds)");
                return;
            }
            verb = Lower(w[2]);
            first = 3;
        } else if (head == "cam" || head == "cut" || head == "end") {
            if (w.size() < 2) {
                Error(line, head + " <time>" + (head == "cam" ? " <...>" : "") + ": the time is missing");
                return;
            }
            if (!ParseTime(w[1], &t)) {
                Error(line, "bad time '" + w[1] + "' (seconds, or minutes:seconds)");
                return;
            }
            first = 2;
        }
        Verb(line, t, verb, Args(w.begin() + static_cast<std::ptrdiff_t>(first), w.end()));
    }

    void Header(int line, const Args& w) {
        if (have_header_) {
            Error(line, "a second 'scene' line (the first is on line " + std::to_string(header_line_) + ")");
            return;
        }
        have_header_ = true;
        header_line_ = line;
        if (w.size() < 2) {
            Error(line, "scene <name> [fps <n>] [step <seconds or 1/n>]: the name is missing");
            return;
        }
        result_.scene.name = w[1];
        ForPairs(line, "scene", w, 2, [&](const std::string& key, const std::string& value) {
            double v = 0.0;
            if (key == "fps") {
                if (!ParseNumber(value, &v) || v <= 0.0 || v > 1000.0) {
                    Error(line, "bad fps '" + value + "' (more than 0, at most 1000)");
                    return false;
                }
                result_.scene.fps = v;
                return true;
            }
            if (key == "step") {
                if (!ParseStep(value, &v)) {
                    Error(line, "bad step '" + value + "' (seconds, or 1/n)");
                    return false;
                }
                result_.scene.step = v;
                return true;
            }
            Error(line, "scene: unknown option '" + key + "' (fps, step)");
            return false;
        });
    }

    // Calls on(key, value) for each `key value` pair of a[from..]; stops at the first false (on() has reported it).
    template <typename F>
    bool ForPairs(int line, const std::string& verb, const Args& a, size_t from, F on) {
        for (size_t i = from; i < a.size(); i += 2) {
            if (i + 1 >= a.size()) {
                Error(line, verb + ": '" + a[i] + "' needs a value");
                return false;
            }
            if (!on(Lower(a[i]), a[i + 1])) return false;
        }
        return true;
    }

    void Verb(int line, double t, const std::string& verb, const Args& a) {
        if (verb == "world") return World(line, t, a);
        if (verb == "hour") return Hour(line, t, a);
        if (verb == "weather") return Weather(line, t, a);
        if (verb == "traffic") return Toggle(line, t, a, Track::kTraffic, verb);
        if (verb == "peds") return Toggle(line, t, a, Track::kPeds, verb);
        if (verb == "parked") return Toggle(line, t, a, Track::kParked, verb);
        if (verb == "police") return Toggle(line, t, a, Track::kPolice, verb);
        if (verb == "timescale") return TimeScale(line, t, a);
        if (verb == "set") return SetCvar(line, t, a);
        if (verb == "label") return Label(line, t, a);
        if (verb == "mark") return Mark(line, t, a);
        if (verb == "capture") return Capture(line, t, a);
        if (verb == "split") return Split(line, t, a);
        if (verb == "end") return End(line, t, a);
        if (verb == "cam" || verb == "cut" || verb == "hud" || verb == "car" || verb == "start") return Opaque(line, t, verb, a);
        if (verb == "statelog") return StateLog(line, t, a);
        Error(line, "unknown statement '" + verb + "' (" + kVerbList + ")");
    }

    void World(int line, double t, const Args& a) {
        if (a.empty()) {
            Error(line, "world needs keys: hour, weather, traffic, peds, parked, police, timescale");
            return;
        }
        size_t i = 0;
        while (i < a.size()) {
            const std::string key = Lower(a[i]);
            if (key == "hold") {
                ++i;
                continue;
            }
            const bool known = key == "hour" || key == "weather" || key == "traffic" || key == "peds" || key == "parked" ||
                               key == "police" || key == "timescale";
            if (!known) {
                Error(line, "world: unknown key '" + a[i] + "' (hour, weather, traffic, peds, parked, police, timescale)");
                return;
            }
            if (i + 1 >= a.size()) {
                Error(line, "world: '" + a[i] + "' needs a value");
                return;
            }
            Verb(line, t, key, Args{a[i + 1]});
            i += 2;
        }
    }

    void Hour(int line, double t, const Args& args) {
        Args a;
        for (const std::string& w : args) {
            if (Lower(w) != "hold") a.push_back(w);  // `hour 17.4 hold`: pinning is what a single hour does anyway
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kHour;
        if (a.empty()) {
            Error(line, "hour needs a value: hour <h>, hour <a>..<b> over <seconds>, or hour free");
            return;
        }
        if (Lower(a[0]) == "free") {
            if (a.size() > 1) Error(line, "hour free takes nothing after it");
            e.release = true;
            Add(std::move(e));
            return;
        }
        double lo = 0.0, hi = 0.0;
        bool range = false;
        if (!ParseRangeOrNumber(a[0], &lo, &hi, &range) || lo < 0.0 || lo > kMaxHour || (range && (hi < 0.0 || hi > kMaxHour))) {
            Error(line, "bad hour '" + a[0] + "' (a number from 0 to 24, or a range a..b; up to 48 to run past midnight)");
            return;
        }
        if (range) {
            e.has_from = true;
            e.from = lo;
            e.to = hi;
        } else {
            e.to = lo;
        }
        bool have_over = false;
        const bool ok = ForPairs(line, "hour", a, 1, [&](const std::string& key, const std::string& value) {
            if (key == "over") {
                if (!ParseDuration(value, &e.over)) {
                    Error(line, "bad length '" + value + "' after over (seconds, more than 0)");
                    return false;
                }
                have_over = true;
                return true;
            }
            if (key == "ease") {
                if (!ParseEase(value, &e.ease)) {
                    Error(line, "unknown ease '" + value + "' (linear, in, out, inout)");
                    return false;
                }
                return true;
            }
            Error(line, "hour: unknown option '" + key + "' (over, ease)");
            return false;
        });
        if (!ok) return;
        if (range && !have_over) {
            Error(line, "an hour ramp needs a length: hour " + a[0] + " over <seconds>");
            return;
        }
        if (!range && have_over) {
            Error(line, "a single hour does not move: write hour <a>..<b> over <seconds> for a ramp");
            return;
        }
        Add(std::move(e));
    }

    void Weather(int line, double t, const Args& a) {
        if (a.size() != 1) {
            Error(line, "weather <nice|cloudy|stormy|foggy|game>");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kWeather;
        if (Lower(a[0]) == "game") {
            e.release = true;
        } else {
            e.index = WeatherIndexFromName(a[0]);
            if (e.index < 0) {
                Error(line, "unknown weather '" + a[0] + "' (nice, cloudy, stormy, foggy or game)");
                return;
            }
        }
        Add(std::move(e));
    }

    void Toggle(int line, double t, const Args& a, Track track, const std::string& verb) {
        int state = 0;
        if (a.size() != 1 || !ParseOnOff(a[0], &state)) {
            Error(line, verb + " on|off");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = track;
        e.state = state;
        Add(std::move(e));
    }

    void TimeScale(int line, double t, const Args& a) {
        if (a.empty()) {
            Error(line, "timescale <x> [over <seconds>] [ease <e>], or timescale <a>..<b> over <seconds>");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kTimeScale;
        double lo = 0.0, hi = 0.0;
        bool range = false;
        if (!ParseRangeOrNumber(a[0], &lo, &hi, &range) || lo < 0.0 || lo > kMaxTimeScale || (range && (hi < 0.0 || hi > kMaxTimeScale))) {
            Error(line, "bad time scale '" + a[0] + "' (0 to 10; 1 is the game's own speed)");
            return;
        }
        if (range) {
            e.has_from = true;
            e.from = lo;
            e.to = hi;
        } else {
            e.to = lo;
        }
        bool have_over = false;
        const bool ok = ForPairs(line, "timescale", a, 1, [&](const std::string& key, const std::string& value) {
            if (key == "over") {
                if (!ParseDuration(value, &e.over)) {
                    Error(line, "bad length '" + value + "' after over (seconds, more than 0)");
                    return false;
                }
                have_over = true;
                return true;
            }
            if (key == "ease") {
                if (!ParseEase(value, &e.ease)) {
                    Error(line, "unknown ease '" + value + "' (linear, in, out, inout)");
                    return false;
                }
                return true;
            }
            Error(line, "timescale: unknown option '" + key + "' (over, ease)");
            return false;
        });
        if (!ok) return;
        if (range && !have_over) {
            Error(line, "a time scale ramp needs a length: timescale " + a[0] + " over <seconds>");
            return;
        }
        const double smallest = range ? std::min(lo, hi) : lo;
        if (smallest < 0.01) {
            Warn(line, "a time scale under 0.01 is held at the game's own lowest step (about 0.006 at 60 FPS)");
        }
        Add(std::move(e));
    }

    void SetCvar(int line, double t, const Args& a) {
        if (a.size() < 2) {
            Error(line, "set <cvar> <value...>");
            return;
        }
        if (!IsIdentifier(a[0])) {
            Error(line, "'" + a[0] + "' is not a setting name (letters, digits and _)");
            return;
        }
        if (const std::string why = CvarRefusal(a[0]); !why.empty()) {
            Error(line, why);
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kCvar;
        e.name = a[0];
        for (size_t i = 1; i < a.size(); ++i) e.text += (i > 1 ? " " : "") + a[i];
        Add(std::move(e));
    }

    void Label(int line, double t, const Args& a) {
        if (a.empty() || a[0].empty()) {
            Error(line, "label \"<text>\" [side <s>] [fade <seconds>] [for <seconds>]: the text is missing");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kLabel;
        e.text = a[0];
        const bool ok = ForPairs(line, "label", a, 1, [&](const std::string& key, const std::string& value) {
            if (key == "side") {
                const std::string s = Lower(value);
                if (s != "left" && s != "right" && s != "center" && s != "top" && s != "bottom") {
                    Error(line, "unknown label side '" + value + "' (left, right, center, top, bottom)");
                    return false;
                }
                e.side = s;
                return true;
            }
            if (key == "fade") {
                double v = 0.0;
                if (!ParseNumber(value, &v) || v < 0.0 || v > 60.0) {
                    Error(line, "bad fade '" + value + "' (seconds, 0 to 60)");
                    return false;
                }
                e.fade = v;
                return true;
            }
            if (key == "for") {
                if (!ParseDuration(value, &e.over)) {
                    Error(line, "bad length '" + value + "' after for (seconds, more than 0)");
                    return false;
                }
                return true;
            }
            Error(line, "label: unknown option '" + key + "' (side, fade, for)");
            return false;
        });
        if (ok) Add(std::move(e));
    }

    void Mark(int line, double t, const Args& a) {
        if (a.size() != 1 || !IsMarkName(a[0])) {
            Error(line, "mark <name> (letters, digits and _ - . :)");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kMark;
        e.name = a[0];
        Add(std::move(e));
    }

    // `statelog on [dir <folder>]` / `statelog off`: the per-update state log (state_log.h) is an option of the whole scene, so it takes no time.
    void StateLog(int line, double t, const Args& a) {
        constexpr const char* kShape = "statelog on [dir <folder>], or statelog off";
        if (t > kEpsilon) {
            Error(line, "statelog is an option of the whole scene: write it without a time (" + std::string(kShape) + ")");
            return;
        }
        if (statelog_line_ != 0) {
            Error(line, "a second 'statelog' (the first is on line " + std::to_string(statelog_line_) + ")");
            return;
        }
        if (a.empty()) {
            Error(line, std::string(kShape) + ": nothing after 'statelog'");
            return;
        }
        const std::string state = Lower(a[0]);
        if (state != "on" && state != "off") {
            Error(line, std::string(kShape) + ": '" + a[0] + "' is neither");
            return;
        }
        std::string dir;
        if (state == "off") {
            if (a.size() != 1) {
                Error(line, "statelog off takes nothing after it");
                return;
            }
        } else {
            const bool ok = ForPairs(line, "statelog", a, 1, [&](const std::string& key, const std::string& value) {
                if (key == "dir") {
                    if (value.empty()) {
                        Error(line, "statelog dir needs a folder");
                        return false;
                    }
                    dir = value;
                    return true;
                }
                Error(line, "statelog: unknown option '" + key + "' (dir)");
                return false;
            });
            if (!ok) return;
        }
        statelog_line_ = line;
        result_.scene.statelog = state == "on";
        result_.scene.statelog_dir = dir;
    }

    // split <x> | split <a>..<b> over <s> [ease <e>] | split off: the renderer's before/after split (gpu/split.h), x in 0..1.
    void Split(int line, double t, const Args& a) {
        if (a.empty()) {
            Error(line, "split needs a value: split <x> (0 to 1 of the picture's width), split <a>..<b> over <seconds>, or split off");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kSplit;
        if (Lower(a[0]) == "off") {
            if (a.size() > 1) Error(line, "split off takes nothing after it");
            e.release = true;
            Add(std::move(e));
            return;
        }
        double lo = 0.0, hi = 0.0;
        bool range = false;
        if (!ParseRangeOrNumber(a[0], &lo, &hi, &range) || lo < 0.0 || lo > 1.0 || (range && (hi < 0.0 || hi > 1.0))) {
            Error(line, "bad split position '" + a[0] + "' (a number from 0 to 1 of the picture's width, or a range a..b; split off for none)");
            return;
        }
        if (range) {
            e.has_from = true;
            e.from = lo;
            e.to = hi;
        } else {
            e.to = lo;
        }
        bool have_over = false;
        const bool ok = ForPairs(line, "split", a, 1, [&](const std::string& key, const std::string& value) {
            if (key == "over") {
                if (!ParseDuration(value, &e.over)) {
                    Error(line, "bad length '" + value + "' after over (seconds, more than 0)");
                    return false;
                }
                have_over = true;
                return true;
            }
            if (key == "ease") {
                if (!ParseEase(value, &e.ease)) {
                    Error(line, "unknown ease '" + value + "' (linear, in, out, inout)");
                    return false;
                }
                return true;
            }
            Error(line, "split: unknown option '" + key + "' (over, ease)");
            return false;
        });
        if (!ok) return;
        if (range && !have_over) {
            Error(line, "a split ramp needs a length: split " + a[0] + " over <seconds>");
            return;
        }
        if (!range && have_over) {
            Error(line, "a single split position does not move: write split <a>..<b> over <seconds> for a ramp");
            return;
        }
        Add(std::move(e));
    }

    void End(int line, double t, const Args& a) {
        if (!a.empty()) {
            Error(line, "end <time> takes nothing after the time");
            return;
        }
        if (end_line_ != 0) {
            Error(line, "a second 'end' (the first is on line " + std::to_string(end_line_) + ")");
            return;
        }
        end_line_ = line;
        result_.scene.end = t;
    }

    // `capture fps <n> [step 1/<n>] [name <take>] [sink none|ffmpeg|png|raw] [statelog on|off]` starts a take, `capture off` ends it. The step of
    // a take is 1/fps (the capture rate and the simulation step are one; slow motion is a timescale track and a higher fps), so a step that says
    // otherwise is an error. `name` is the take's folder (a name that is taken gets -2, -3 ...), `sink` what the frames go to, `statelog on` asks
    // for the state log (state.csv) of the take (capture_mode.cpp, take_folder.h).
    void Capture(int line, double t, const Args& a) {
        if (a.empty()) {
            Error(line, "capture fps <n> [step 1/<n>] [name <take>] [sink <sink>] [statelog on|off], or capture off: nothing after 'capture'");
            return;
        }
        Event e;
        e.t = t;
        e.line = line;
        e.track = Track::kCapture;
        if (Lower(a[0]) == "off") {
            if (a.size() != 1) {
                Error(line, "capture off takes nothing after it");
                return;
            }
            e.state = 0;
            Add(std::move(e));
            return;
        }
        if (Lower(a[0]) != "fps") {
            Error(line, "capture fps <n> [step 1/<n>] [name <take>] [sink <sink>] [statelog on|off], or capture off: '" + a[0] + "' is neither");
            return;
        }
        double fps = 0.0;
        double step = 0.0;
        bool have_step = false;
        const bool ok = ForPairs(line, "capture", a, 0, [&](const std::string& key, const std::string& value) {
            if (key == "fps") {
                if (!ParseNumber(value, &fps) || fps < kMinCaptureFps || fps > kMaxCaptureFps) {
                    Error(line, "bad fps '" + value + "' (" + Num(kMinCaptureFps) + " to " + Num(kMaxCaptureFps) +
                                    ": the engine timer cannot step slower than 10 a second)");
                    return false;
                }
                return true;
            }
            if (key == "step") {
                if (!ParseStep(value, &step)) {
                    Error(line, "bad step '" + value + "' (seconds, or 1/n)");
                    return false;
                }
                have_step = true;
                return true;
            }
            if (key == "name") {
                if (!IsTakeName(value)) {
                    Error(line, "bad take name '" + value + "' (letters, digits and _ - . , at most 64, not starting with . or -)");
                    return false;
                }
                e.name = value;
                return true;
            }
            if (key == "sink") {
                const std::string sink = Lower(value);
                if (sink != "none" && sink != "ffmpeg" && sink != "png" && sink != "raw") {  // capture_core.h's SinkKind
                    Error(line, "bad sink '" + value + "' (none, ffmpeg, png or raw)");
                    return false;
                }
                e.text = sink;
                return true;
            }
            if (key == "statelog") {
                const std::string v = Lower(value);
                if (v != "on" && v != "off") {
                    Error(line, "statelog on or statelog off, not '" + value + "'");
                    return false;
                }
                e.index = v == "on" ? 1 : 0;
                return true;
            }
            Error(line, "capture: unknown option '" + key + "' (fps, step, name, sink, statelog)");
            return false;
        });
        if (!ok) return;
        if (have_step && std::fabs(step * fps - 1.0) > 1e-4) {
            Error(line, "the step of a capture is 1/fps (here 1/" + Num(fps) + "): one step a scene, and slow motion is a timescale key "
                        "with a higher capture fps");
            return;
        }
        e.state = 1;
        e.to = fps;
        Add(std::move(e));
    }

    // camera, cut, hud, car and start: only their shape is checked here; the packages that apply them read the words.
    void Opaque(int line, double t, const std::string& verb, const Args& a) {
        Event e;
        e.t = t;
        e.line = line;
        e.words = a;
        if (verb == "cam") {
            e.track = Track::kCamera;
            if (a.empty()) {
                Error(line, "cam <time> <what the camera does>: nothing after the time");
                return;
            }
        } else if (verb == "cut") {
            e.track = Track::kCut;
            if (!a.empty()) {
                Error(line, "cut <time> takes nothing after the time");
                return;
            }
        } else if (verb == "hud") {
            e.track = Track::kHud;
            if (a.empty()) {
                Error(line, "hud on, hud off or hud fade <a>..<b> to <x>");
                return;
            }
        } else if (verb == "car") {
            e.track = Track::kCar;
            if (a.size() < 2 || !IsIdentifier(a[0])) {
                Error(line, "car <id> <what the car does> (the id is letters, digits and _)");
                return;
            }
            // The car verbs of the cars package (car_verbs.h) are read here, so a bad line is an error with its number; a verb that
            // is not one of theirs (a path: M4c) passes.
            CarCommand car;
            std::string why;
            if (ParseCarCommand(a, &car, &why) == CarParse::kError) {
                Error(line, why);
                return;
            }
        } else {
            e.track = Track::kStart;
            if (a.empty()) {
                Error(line, "start <what the take starts from>: nothing after it");
                return;
            }
        }
        Add(std::move(e));
    }

    // After the last line: the order, the end and the length checks that need the whole file.
    void Finish() {
        Scene& scene = result_.scene;
        if (statements_ == 0) {
            Error(0, "the file has no statements");
            return;
        }
        std::stable_sort(events_.begin(), events_.end(), [](const Event& x, const Event& y) { return x.t < y.t; });

        std::array<const Event*, 16> last{};
        for (const Event& e : events_) {
            if (!IsWorldTrack(e.track) && e.track != Track::kSplit) continue;
            const size_t k = static_cast<size_t>(e.track);
            if (last[k] && std::fabs(last[k]->t - e.t) < kEpsilon) {
                Warn(e.line, std::string("two ") + TrackName(e.track) + " keys at t=" + Num(e.t) + " (line " +
                                 std::to_string(last[k]->line) + " and this one): the later wins");
            }
            last[k] = &e;
        }

        // The take: one `capture fps`, an optional `capture off` after it.
        const Event* cap_start = nullptr;
        const Event* cap_stop = nullptr;
        for (const Event& e : events_) {
            if (e.track != Track::kCapture) continue;
            if (e.state == 1) {
                if (cap_start) {
                    Error(e.line, "a second 'capture fps' (the first is on line " + std::to_string(cap_start->line) +
                                      "): a scene has one capture rate, and slow motion is a timescale key with a higher fps");
                } else {
                    cap_start = &e;
                }
            } else if (!cap_start) {
                Error(e.line, "'capture off' with no 'capture fps' before it");
            } else if (cap_stop) {
                Warn(e.line, "a second 'capture off' (the first is on line " + std::to_string(cap_stop->line) + "): the first ends the take");
            } else if (e.t <= cap_start->t + kEpsilon) {
                Error(e.line, "'capture off' at t=" + Num(e.t) + " ends a take that starts at t=" + Num(cap_start->t) + " (line " +
                                  std::to_string(cap_start->line) + "): nothing would be captured");
            } else {
                cap_stop = &e;
            }
        }
        if (cap_start) {
            scene.capture_fps = cap_start->to;
            scene.capture_name = cap_start->name;
            scene.capture_sink = cap_start->text;
            scene.capture_state_log = cap_start->index == 1;
            scene.capture_from = cap_start->t;
            scene.capture_to = cap_stop ? cap_stop->t : scene.end;
            if (scene.fps > 0.0 && std::fabs(scene.fps - scene.capture_fps) > 1e-6) {
                Warn(cap_start->line, "the header says fps " + Num(scene.fps) + " and the capture says " + Num(scene.capture_fps) + ": the capture's is used");
            }
            if (!cap_stop && scene.end < 0.0) {
                Warn(cap_start->line, "no 'end' and no 'capture off': the take runs until the scene is turned off");
            }
        }

        if (scene.end < 0.0) {
            Warn(0, "no 'end': the scene never finishes, and its last values hold until it is turned off");
        } else {
            for (const Event& e : events_) {
                if (e.t > scene.end + kEpsilon) {
                    Error(e.line, std::string("the ") + TrackName(e.track) + " key at t=" + Num(e.t) +
                                      " is after the scene's end (t=" + Num(scene.end) + ", line " +
                                      std::to_string(end_line_) + ")");
                } else if (e.over > 0.0 && e.t + e.over > scene.end + kEpsilon) {
                    Warn(e.line, std::string("the ") + TrackName(e.track) + " key runs to t=" + Num(e.t + e.over) +
                                     ", past the scene's end (t=" + Num(scene.end) + "): it is cut off there");
                }
            }
        }
    }

    ParseResult result_;
    std::vector<Event> events_;
    int statements_ = 0;
    bool have_header_ = false;
    int header_line_ = 0;
    int end_line_ = 0;
    int statelog_line_ = 0;
    double max_t_ = 0.0;
    int max_line_ = 0;
};

double RampValue(double a, double b, double t0, double over, Ease ease, double t) {
    if (over <= 0.0 || t >= t0 + over) return b;
    if (t <= t0) return a;
    return a + (b - a) * ApplyEase(ease, (t - t0) / over);
}

double WrapHour(double h) {
    h = std::fmod(h, 24.0);
    if (h < 0.0) h += 24.0;
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- public

std::string FormatDiagnostic(const Diagnostic& d, std::string_view file_name) {
    std::string out;
    if (!file_name.empty()) out += std::string(file_name);
    if (d.line > 0) {
        out += out.empty() ? "line " : ":";
        out += std::to_string(d.line);
    }
    if (!out.empty()) out += ": ";
    out += d.severity == Severity::kError ? "error: " : "warning: ";
    out += d.message;
    return out;
}

double ApplyEase(Ease ease, double u) {
    u = std::clamp(u, 0.0, 1.0);
    switch (ease) {
        case Ease::kLinear:
            return u;
        case Ease::kIn:
            return u * u;
        case Ease::kOut:
            return 1.0 - (1.0 - u) * (1.0 - u);
        case Ease::kInOut:
            return u * u * (3.0 - 2.0 * u);
    }
    return u;
}

const char* TrackName(Track track) {
    switch (track) {
        case Track::kHour: return "hour";
        case Track::kWeather: return "weather";
        case Track::kTraffic: return "traffic";
        case Track::kPeds: return "peds";
        case Track::kParked: return "parked";
        case Track::kPolice: return "police";
        case Track::kTimeScale: return "timescale";
        case Track::kCvar: return "set";
        case Track::kLabel: return "label";
        case Track::kMark: return "mark";
        case Track::kSplit: return "split";
        case Track::kCamera: return "cam";
        case Track::kCut: return "cut";
        case Track::kHud: return "hud";
        case Track::kCar: return "car";
        case Track::kStart: return "start";
        case Track::kCapture: return "capture";
    }
    return "?";
}

int WeatherIndexFromName(std::string_view name) {
    const std::string l = Lower(name);
    for (int i = 0; i < 4; ++i) {
        if (l == kWeatherNames[i]) return i;
    }
    return -1;
}

namespace {

// Settings a scene may not set, by exact name (lower case), besides the patterns in CvarRefusal. Each one names a program, a path, a
// server or arguments, or makes the game write a file of its own (the file is in the game's folder, not the take's). Found by going through
// every REXCVAR_DEFINE_* of the game and the SDK on 2026-11-13; add a setting here when it does not end in or hold one of the patterns.
constexpr std::string_view kRefusedCvars[] = {
    "cl",                                // extra command line for the guest
    "user_name",                         // names the save's folder
    "import_vinyl",                      // a .vgp file name, read from the game's folder
    "export_vinyl",                      // writes a .vgp file
    "capture_vinyl_shapes",              // writes the vinyl shape map
    "allow_game_relative_writes",        // lets the game write below game://
    "lzx_stats",                         // appends to lzx_stats.txt
    "menu_cam_record",                   // writes menu_cameras.txt
    "wheel_learn",                       // opens a console and writes wheel_profiles.ini
    "mcla_gpu_trace_frame",              // writes mcla_gpu_trace_<n>.txt
    "mcla_gpu_trace_count",
    "mcla_native_gfx_capture",           // writes mcla_native_gfx_frame.txt and .tga
    "mcla_native_gfx_firstdraw",         // writes mcla_native_gfx_firstdraw.txt and .tga
    "mcla_native_gfx_ab_cvar",           // flips another setting by name, which would go round this list
    "rmr_server",                        // a server the game talks to
    "online_master_server",
    "server_browser_servers",
    "server_browser_pending",
    "mcla_state_log",                    // the state log's file or folder (M3a)
    "mcla_freecam_record",               // writes a flight file (M1d)
    "mcla_freecam_out",                  // names it
};

constexpr std::string_view kRefusedSuffixes[] = {
    "_path", "_paths", "_dir", "_dirs", "_folder", "_file", "_files", "_root", "_exe", "_args", "_command", "_cmd", "_csv",
    "_probe", "_diag", "_trace", "_census", "_telemetry",
};

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

std::string CvarRefusal(std::string_view name) {
    const std::string n = Lower(name);
    const std::string given(name);
    if (StartsWith(n, "mcla_scene")) return "a scene does not set " + given;
    bool refused = StartsWith(n, "mcla_capture_") || n.find("dump") != std::string::npos;
    for (const std::string_view suffix : kRefusedSuffixes) refused = refused || EndsWith(n, suffix);
    for (const std::string_view exact : kRefusedCvars) refused = refused || n == exact;
    if (!refused) return std::string();
    return "a scene does not set " + given +
           ": it names a program, a path, a server or arguments, or makes the game write a file of its own (put it on the command line)";
}

double TimeScaleForEngine(double scene_value) {
    if (!std::isfinite(scene_value)) return -1.0;
    return std::clamp(scene_value, 0.0, kMaxTimeScale);
}

double EngineStepRatio(double scaled, double unscaled) {
    if (!std::isfinite(scaled) || !std::isfinite(unscaled) || unscaled <= 0.0 || scaled < 0.0) return 0.0;
    return scaled / unscaled;
}

bool TimeScaleFollowed(double want, double ratio) {
    if (!std::isfinite(want) || !std::isfinite(ratio)) return false;
    return std::fabs(ratio - want) <= 0.05 + 0.1 * want;
}

double Scene::LastKeyEnd() const {
    double last = 0.0;
    for (const Event& e : events) last = std::max(last, e.t + (e.over > 0.0 ? e.over : 0.0));
    return last;
}

int ParseResult::ErrorCount() const {
    int n = 0;
    for (const Diagnostic& d : diagnostics) n += d.severity == Severity::kError ? 1 : 0;
    return n;
}

int ParseResult::WarningCount() const {
    int n = 0;
    for (const Diagnostic& d : diagnostics) n += d.severity == Severity::kWarning ? 1 : 0;
    return n;
}

ParseResult ParseScene(std::string_view text) {
    return Parser().Run(text);
}

std::string Summarize(const Scene& scene) {
    constexpr Track kOrder[] = {Track::kHour,   Track::kWeather, Track::kTraffic, Track::kPeds,   Track::kParked,
                                Track::kPolice, Track::kTimeScale, Track::kCvar,  Track::kLabel,  Track::kMark,
                                Track::kSplit,  Track::kCapture, Track::kCamera,  Track::kCut,     Track::kHud,     Track::kCar,
                                Track::kStart};
    std::string out = std::to_string(scene.events.size()) + (scene.events.size() == 1 ? " event" : " events");
    std::string parts;
    for (const Track track : kOrder) {
        size_t n = 0;
        for (const Event& e : scene.events) n += e.track == track ? 1 : 0;
        if (n == 0) continue;
        if (!parts.empty()) parts += ", ";
        parts += std::string(TrackName(track)) + " " + std::to_string(n);
    }
    if (!parts.empty()) out += " (" + parts + ")";
    out += scene.end >= 0.0 ? "; ends at " + Num(scene.end) + " s" : "; no end";
    if (scene.statelog) out += "; state log on" + (scene.statelog_dir.empty() ? std::string() : " (dir " + scene.statelog_dir + ")");
    return out;
}

WorldState EvaluateWorld(const Scene& scene, double t) {
    WorldState w;
    const Event* hour = nullptr;
    bool ts_have = false;
    double ts_t0 = 0.0, ts_over = 0.0, ts_a = 1.0, ts_b = 1.0;
    Ease ts_ease = Ease::kLinear;
    for (const Event& e : scene.events) {
        if (e.t > t) break;  // sorted by time
        switch (e.track) {
            case Track::kHour:
                hour = e.release ? nullptr : &e;
                break;
            case Track::kWeather:
                w.weather = e.release ? -1 : e.index;
                break;
            case Track::kTraffic:
                w.traffic = e.state;
                break;
            case Track::kPeds:
                w.peds = e.state;
                break;
            case Track::kParked:
                w.parked = e.state;
                break;
            case Track::kPolice:
                w.police = e.state;
                break;
            case Track::kTimeScale: {
                const double start =
                    e.has_from ? e.from : (ts_have ? RampValue(ts_a, ts_b, ts_t0, ts_over, ts_ease, e.t) : 1.0);
                ts_have = true;
                ts_t0 = e.t;
                ts_over = e.over;
                ts_a = start;
                ts_b = e.to;
                ts_ease = e.ease;
                break;
            }
            default:
                break;
        }
    }
    if (hour) {
        w.hour_set = true;
        w.hour = WrapHour(hour->has_from ? RampValue(hour->from, hour->to, hour->t, hour->over, hour->ease, t) : hour->to);
    }
    if (ts_have) {
        w.timescale_set = true;
        w.timescale = RampValue(ts_a, ts_b, ts_t0, ts_over, ts_ease, t);
    }
    return w;
}

CaptureWindow EvaluateCapture(const Scene& scene, double t) {
    CaptureWindow w;
    if (scene.capture_fps <= 0.0) return w;
    if (t + kEpsilon < scene.capture_from) return w;
    if (scene.capture_to >= 0.0 && t + kEpsilon >= scene.capture_to) return w;
    w.on = true;
    w.fps = scene.capture_fps;
    return w;
}

SplitState EvaluateSplit(const Scene& scene, double t) {
    SplitState s;
    const Event* key = nullptr;
    for (const Event& e : scene.events) {
        if (e.t > t) break;  // sorted by time
        if (e.track != Track::kSplit) continue;
        key = e.release ? nullptr : &e;
    }
    if (key) {
        s.set = true;
        s.position = std::clamp(key->has_from ? RampValue(key->from, key->to, key->t, key->over, key->ease, t) : key->to, 0.0, 1.0);
    }
    return s;
}

void EventCursor::Take(const Scene& scene, double t, std::vector<const Event*>* out) {
    while (next_ < scene.events.size() && scene.events[next_].t <= t + kEpsilon) {
        out->push_back(&scene.events[next_]);
        ++next_;
    }
}

SceneClock::Step SceneClock::Advance(bool world_ready, bool paused, double dt, double duration) {
    Step s;
    switch (state_) {
        case State::kIdle:
        case State::kFinished:
            break;
        case State::kWaiting:
            if (world_ready && !paused) {
                if (++ready_ticks_ >= config_.settle_ticks) {
                    state_ = State::kRunning;
                    t_ = 0.0;
                    s.started = true;
                }
            } else {
                ready_ticks_ = 0;
            }
            break;
        case State::kRunning:
            if (world_ready && !paused && dt > 0.0) {
                t_ += std::min(dt, config_.max_dt);
                s.advanced = true;
                if (duration >= 0.0 && t_ >= duration) {
                    t_ = duration;
                    state_ = State::kFinished;
                    s.finished = true;
                }
            }
            break;
    }
    s.state = state_;
    s.t = t_;
    return s;
}

void SceneClock::Arm() {
    state_ = State::kWaiting;
    t_ = 0.0;
    ready_ticks_ = 0;
}

void SceneClock::Disarm() {
    state_ = State::kIdle;
    t_ = 0.0;
    ready_ticks_ = 0;
}

void SceneClock::Restart() {
    state_ = State::kRunning;
    t_ = 0.0;
    ready_ticks_ = 0;
}

void SceneClock::SetTime(double t, double duration) {
    if (state_ != State::kRunning && state_ != State::kFinished) return;
    t_ = std::max(0.0, t);
    if (duration >= 0.0) t_ = std::min(t_, duration);
    state_ = State::kRunning;
}

void ReloadWatcher::Reset() {
    have_loaded_ = false;
    loaded_ = {};
    have_pending_ = false;
    pending_ = {};
    pending_since_ = 0;
    missing_reported_ = false;
}

ReloadWatcher::Action ReloadWatcher::Poll(const FileStamp& now, int64_t now_ms) {
    if (!now.exists) {
        have_pending_ = false;
        if (!missing_reported_) {
            missing_reported_ = true;
            return Action::kMissing;
        }
        return Action::kNone;
    }
    missing_reported_ = false;
    if (have_loaded_ && now == loaded_) {
        have_pending_ = false;
        return Action::kNone;
    }
    if (!have_pending_ || !(now == pending_)) {
        pending_ = now;
        have_pending_ = true;
        pending_since_ = now_ms;
    }
    // The first read of a file with something in it is at once; every later one waits until the file stopped changing.
    if (!have_loaded_ && now.size > 0) return Action::kLoad;
    return now_ms - pending_since_ >= settle_ms_ ? Action::kLoad : Action::kNone;
}

void ReloadWatcher::Loaded(const FileStamp& stamp) {
    have_loaded_ = true;
    loaded_ = stamp;
    have_pending_ = false;
}

}  // namespace mc::cinematic
