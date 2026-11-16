// The HUD track of a scene: the keys and their evaluation. See hud_track.h. Pure code, no game.

#include "hud_track.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>

namespace mc::cinematic {

namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool Fail(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

// A whole word that is a finite decimal number (no trailing characters, no hex, no inf or nan).
bool ParseNumber(const std::string& s, double* out) {
    if (s.empty() || s.find_first_not_of("0123456789+-.eE") != std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

// off, on or a number 0..1.
bool ParseAlpha(const std::string& word, float* out, std::string* error) {
    const std::string w = Lower(word);
    if (w == "off") {
        *out = 0.0f;
        return true;
    }
    if (w == "on") {
        *out = 1.0f;
        return true;
    }
    double value = 0.0;
    if (!ParseNumber(w, &value)) {
        return Fail(error, "hud: '" + word + "' is not off, on or an alpha from 0 to 1");
    }
    if (value < 0.0 || value > 1.0) {
        return Fail(error, "hud: alpha " + word + " is outside 0 to 1");
    }
    *out = float(value);
    return true;
}

const char* const kExpected =
    "hud: expected off, on, an alpha from 0 to 1, 'fade <a>..<b> to <x>' or 'fade over <s> to <x>'";

// One stretch of the alpha: from t0 to t1 in a straight line from v0 to v1, then v1.
struct Segment {
    double t0 = -std::numeric_limits<double>::infinity();
    double t1 = -std::numeric_limits<double>::infinity();
    float v0 = 1.0f;
    float v1 = 1.0f;
    float At(double t) const {
        if (t >= t1) {
            return v1;
        }
        if (t <= t0) {
            return v0;
        }
        return v0 + (v1 - v0) * float((t - t0) / (t1 - t0));
    }
};

}  // namespace

bool ParseHudKey(const std::vector<std::string>& words, double at, HudKey* key, std::string* error) {
    if (words.empty()) {
        return Fail(error, kExpected);
    }
    HudKey out;
    out.t = at;
    out.end = at;
    if (Lower(words[0]) != "fade") {
        if (words.size() != 1) {
            return Fail(error, kExpected);
        }
        if (!ParseAlpha(words[0], &out.alpha, error)) {
            return false;
        }
        *key = out;
        return true;
    }
    // fade <a>..<b> to <x>   (4 words)      fade over <s> to <x>   (5 words)
    const bool over = words.size() == 5 && Lower(words[1]) == "over";
    if (!over && words.size() != 4) {
        return Fail(error, kExpected);
    }
    const std::size_t to_at = over ? 3 : 2;
    if (Lower(words[to_at]) != "to") {
        return Fail(error, kExpected);
    }
    if (!ParseAlpha(words[to_at + 1], &out.alpha, error)) {
        return false;
    }
    if (over) {
        double seconds = 0.0;
        if (!ParseNumber(words[2], &seconds) || seconds < 0.0) {
            return Fail(error, "hud fade: '" + words[2] + "' is not a number of seconds (0 or more)");
        }
        out.end = at + seconds;
    } else {
        const std::string& range = words[1];
        const std::size_t dots = range.find("..");
        double a = 0.0;
        double b = 0.0;
        if (dots == std::string::npos || !ParseNumber(range.substr(0, dots), &a) ||
            !ParseNumber(range.substr(dots + 2), &b)) {
            return Fail(error, "hud fade: '" + range + "' is not <a>..<b>, two scene times in seconds");
        }
        if (a < 0.0) {
            return Fail(error, "hud fade: the start time " + range.substr(0, dots) + " is before the scene starts");
        }
        if (b < a) {
            return Fail(error, "hud fade: " + range + " ends before it starts");
        }
        out.t = a;
        out.end = b;
    }
    *key = out;
    return true;
}

void HudTrack::Add(const HudKey& key) {
    const auto after = std::upper_bound(keys_.begin(), keys_.end(), key.t,
                                        [](double t, const HudKey& k) { return t < k.t; });
    keys_.insert(after, key);
}

float HudTrack::AlphaAt(double t) const {
    Segment now;
    for (const HudKey& k : keys_) {
        if (t < k.t) {
            break;
        }
        Segment next;
        next.t0 = k.t;
        next.t1 = std::max(k.end, k.t);
        next.v0 = now.At(k.t);
        next.v1 = k.alpha;
        now = next;
    }
    return now.At(t);
}

double HudTrack::End() const {
    double end = 0.0;
    for (const HudKey& k : keys_) {
        end = std::max(end, std::max(k.t, k.end));
    }
    return end;
}

}  // namespace mc::cinematic
