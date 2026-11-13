// The car statements of a scene: see car_verbs.h.

#include "car_verbs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc::cinematic {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxCoordinate = 100000.0;
constexpr int kMaxSlot = 999;
constexpr size_t kMaxModelChars = 47;  // the game keeps a vehicle name in a 64-byte buffer

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ParseNumber(const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(x)) return false;
    *v = x;
    return true;
}

bool ParseSlot(const std::string& s, int* v) {
    if (s.empty() || s.size() > 4) return false;
    int x = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        x = x * 10 + (c - '0');
    }
    if (x > kMaxSlot) return false;
    *v = x;
    return true;
}

bool IsModelName(const std::string& s) {
    if (s.empty() || s.size() > kMaxModelChars) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// "a,b,c" into three numbers.
bool ParseTriple(const std::string& s, double out[3]) {
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t comma = s.find(',', start);
        if (i < 2 && comma == std::string::npos) return false;
        if (i == 2 && comma != std::string::npos) return false;
        const std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!ParseNumber(part, &out[i])) return false;
        start = comma + 1;
    }
    return true;
}

std::string Num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

constexpr const char* kSpawnUsage = "car <id> spawn <model|profile N> at <x,y,z> [heading <deg>]";

CarParse Fail(std::string* error, const std::string& message) {
    *error = message;
    return CarParse::kError;
}

CarParse ParseSpawn(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
    out->verb = CarVerb::kSpawn;
    if (w.size() < 3) return Fail(error, std::string("spawn what? ") + kSpawnUsage);
    size_t i = 3;
    const std::string what = Lower(w[2]);
    if (what == "profile") {
        if (w.size() < 4 || !ParseSlot(w[3], &out->slot)) {
            return Fail(error, "spawn profile needs a garage slot, a whole number from 0 (car <id> spawn profile 2 at ...)");
        }
        out->source = CarSource::kGarage;
        i = 4;
    } else if ((what.rfind("garage:", 0) == 0 && ParseSlot(what.substr(7), &out->slot)) ||
               (what.rfind("slot:", 0) == 0 && ParseSlot(what.substr(5), &out->slot))) {
        out->source = CarSource::kGarage;
    } else if (what.find(':') != std::string::npos) {
        return Fail(error, "'" + w[2] + "' is not a car: a vehicle name, profile N, garage:N or slot:N");
    } else {
        if (!IsModelName(what)) {
            return Fail(error, "'" + w[2] + "' is not a vehicle name (lower case letters, digits and _ - ., up to 47)");
        }
        out->source = CarSource::kModel;
        out->model = what;
    }
    bool have_at = false;
    while (i < w.size()) {
        const std::string key = Lower(w[i]);
        if (key == "at") {
            if (have_at) return Fail(error, "spawn: a second 'at'");
            if (i + 1 >= w.size()) return Fail(error, "at needs x,y,z");
            if (w[i + 1].find(',') != std::string::npos) {
                if (!ParseTriple(w[i + 1], out->position)) return Fail(error, "bad position '" + w[i + 1] + "' (x,y,z: three numbers)");
                i += 2;
            } else {
                if (i + 3 >= w.size()) return Fail(error, "at needs x,y,z (as x,y,z or as three numbers)");
                for (int k = 0; k < 3; ++k) {
                    if (!ParseNumber(w[i + 1 + static_cast<size_t>(k)], &out->position[k])) {
                        return Fail(error, "bad position '" + w[i + 1 + static_cast<size_t>(k)] + "' (x,y,z: three numbers)");
                    }
                }
                i += 4;
            }
            for (double c : out->position) {
                if (std::fabs(c) > kMaxCoordinate) return Fail(error, "the position is outside the world (more than 100000 from the origin)");
            }
            have_at = true;
        } else if (key == "heading") {
            if (out->has_heading) return Fail(error, "spawn: a second 'heading'");
            double h = 0.0;
            if (i + 1 >= w.size() || !ParseNumber(w[i + 1], &h)) return Fail(error, "heading needs degrees (heading 270)");
            out->heading = NormalizeDegrees(h);
            out->has_heading = true;
            i += 2;
        } else {
            return Fail(error, "spawn: unknown option '" + w[i] + "' (at, heading)");
        }
    }
    if (!have_at) return Fail(error, std::string("spawn needs a place: ") + kSpawnUsage);
    return CarParse::kOk;
}

}  // namespace

double NormalizeDegrees(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    if (d >= 360.0) d = 0.0;
    return d;
}

double NativeHeadingDeg(double scene_heading) {
    return NormalizeDegrees(scene_heading + 180.0);
}

double SceneHeadingDeg(double forward_x, double forward_z) {
    return NormalizeDegrees(std::atan2(forward_x, forward_z) * 180.0 / kPi);
}

CarParse ParseCarCommand(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
    *out = CarCommand{};
    error->clear();
    if (w.size() < 2) return Fail(error, "car <id> <what the car does>");
    out->id = w[0];
    const std::string verb = Lower(w[1]);
    const bool player = Lower(w[0]) == "player";
    if (verb == "spawn") {
        if (player) return Fail(error, "'player' is the player's own car: pick another id (only `car player profile N` applies to it)");
        return ParseSpawn(w, out, error);
    }
    if (verb == "delete" || verb == "despawn") {
        if (player) return Fail(error, "'player' is the player's own car: it cannot be deleted");
        if (w.size() != 2) return Fail(error, "car <id> delete takes nothing after it");
        out->verb = CarVerb::kDelete;
        return CarParse::kOk;
    }
    if (verb == "profile") {
        if (!player) {
            return Fail(error, "only the player's car is dressed with `profile` (car player profile N); a spawned car takes its slot in `spawn profile N`");
        }
        if (w.size() != 3 || !ParseSlot(w[2], &out->slot)) {
            return Fail(error, "car player profile <N>: N is a garage slot, a whole number from 0");
        }
        out->verb = CarVerb::kProfile;
        return CarParse::kOk;
    }
    return CarParse::kOtherVerb;
}

std::string DescribeCarCommand(const CarCommand& c) {
    switch (c.verb) {
        case CarVerb::kDelete:
            return c.id + " delete";
        case CarVerb::kProfile:
            return c.id + " profile " + std::to_string(c.slot);
        case CarVerb::kSpawn:
            break;
    }
    std::string out = c.id + " spawn ";
    out += c.source == CarSource::kGarage ? "profile " + std::to_string(c.slot) : c.model;
    out += " at (" + Num(c.position[0]) + ", " + Num(c.position[1]) + ", " + Num(c.position[2]) + ") heading " + Num(c.heading);
    return out;
}

}  // namespace mc::cinematic
