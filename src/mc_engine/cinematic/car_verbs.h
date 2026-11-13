#pragma once
//
// The car statements of a scene (trailer toolkit, work package M4b): the words after `car`, read into a command. Pure: it knows
// nothing of the game, so cars_test.cpp builds it on its own, and scene.cpp uses it to report a bad car line with its line number
// when the file is parsed. Header only (inline), so that every program that links scene.cpp can do so without another source.
//
//   car <id> spawn <model> at <x,y,z> [heading <deg>]      a car of that model (the game's vehicle name, vp_lam_murcielago_06)
//   car <id> spawn profile <N> at <x,y,z> [heading <deg>]  a car from garage slot N of the player's save, dressed as the owner left it
//   car <id> delete                                        the car goes (every car a scene spawned goes when it ends)
//   car player profile <N>                                 the player's own car becomes garage slot N (the old one comes back at the end)
//
// `at` takes x,y,z as one word or as three; the position is a world position (x east, z south, y up). `heading` is where the
// nose points, in degrees, as the camera's yaw: 0 along +Z, 90 along +X (the game's own Racer_Teleport angle is this plus 180:
// NativeHeadingDeg). Garage slots count from 0. `garage:N` and `slot:N` mean `profile N`; `despawn` means `delete`. The id is
// what the camera's `car:<id>` names; `player` is the player's own car and takes only `profile`.
//
// Other verbs of the car track (a path, a speed: package M4c) are not this file's: ParseCarCommand says kOtherVerb and the
// scene parser lets them through.
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mc::cinematic {

enum class CarVerb : uint8_t { kSpawn, kDelete, kProfile };
enum class CarSource : uint8_t { kModel, kGarage };  // spawn: a vehicle name, or a garage slot

struct CarCommand {
    std::string id;
    CarVerb verb = CarVerb::kSpawn;
    CarSource source = CarSource::kModel;
    std::string model;  // kModel: the vehicle name, lower case
    int slot = -1;      // kGarage, and the profile verb: the garage slot, from 0
    double position[3] = {0.0, 0.0, 0.0};
    double heading = 0.0;  // degrees in [0, 360): the camera's yaw convention
    bool has_heading = false;
};

enum class CarParse : uint8_t { kOk, kOtherVerb, kError };

// Degrees into [0, 360).
inline double NormalizeDegrees(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    if (d >= 360.0) d = 0.0;
    return d;
}

// The angle Racer_Teleport and the opponent loader take for a scene heading: the car's back row is (sin a, 0, cos a), so a nose
// along (sin h, cos h) needs a = h + 180.
inline double NativeHeadingDeg(double scene_heading) {
    return NormalizeDegrees(scene_heading + 180.0);
}

// The scene heading of a car whose nose points along (forward_x, forward_z): atan2(x, z), in [0, 360).
inline double SceneHeadingDeg(double forward_x, double forward_z) {
    return NormalizeDegrees(std::atan2(forward_x, forward_z) * 180.0 / 3.14159265358979323846);
}

namespace car_verbs_detail {

constexpr double kMaxCoordinate = 100000.0;
constexpr int kMaxSlot = 999;
constexpr size_t kMaxModelChars = 47;  // the game keeps a vehicle name in a 64-byte buffer

inline std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline bool ParseNumber(const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(x)) return false;
    *v = x;
    return true;
}

inline bool ParseSlot(const std::string& s, int* v) {
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

inline bool IsModelName(const std::string& s) {
    if (s.empty() || s.size() > kMaxModelChars) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// "a,b,c" into three numbers.
inline bool ParseTriple(const std::string& s, double out[3]) {
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

inline std::string Num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

constexpr const char* kSpawnUsage = "car <id> spawn <model|profile N> at <x,y,z> [heading <deg>]";

inline CarParse Fail(std::string* error, const std::string& message) {
    *error = message;
    return CarParse::kError;
}

inline CarParse ParseSpawn(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
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

}  // namespace car_verbs_detail

// `words` are the words after the `car` statement (the id first). On kError `error` says what is wrong, in the scene parser's
// style; on kOtherVerb nothing is wrong, another package reads the line.
inline CarParse ParseCarCommand(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
    using namespace car_verbs_detail;
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

// "p1 spawn vp_lam_murcielago_06 at (900.00, 17.50, -850.00) heading 270.00" for the log.
inline std::string DescribeCarCommand(const CarCommand& c) {
    using namespace car_verbs_detail;
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
