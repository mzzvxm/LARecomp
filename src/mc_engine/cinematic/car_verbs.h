#pragma once
//
// The car statements of a scene (trailer toolkit, work package M4b): the words after `car`, read into a command. Pure: it knows
// nothing of the game, so cars_test.cpp builds it on its own, and scene.cpp uses it to report a bad car line with its line number
// when the file is parsed. Header only (inline), so that every program that links scene.cpp can do so without another source.
//
//   car <id> spawn <model> at <x,y,z> [heading <deg>]      a car of that model (the game's vehicle name, vp_lam_murcielago_06)
//   car <id> spawn profile <N> at <x,y,z> [heading <deg>]  a car from garage slot N of the player's save, dressed as the owner left it
//   car <id> ambient <model> at <x,y,z> [heading <deg>]    a REAL ambient (traffic-class) car, placed through AmbientMgr_AddAmbientToLayout (ambient_car.h);
//                                                          it stands still where it is put, with no route, and goes when the scene ends
//   car <id> delete                                        the car goes (every car a scene spawned or placed goes when it ends)
//   car player profile <N>                                 the player's own car becomes garage slot N (the old one comes back at the end)
//   car <id> path <route file> [speed <x>] [mode brain|kinematic]   the car drives a route (work package M4c; path_follow.h has the file)
//   car <id> stop                                          the car stops where it is (a path ends; the car stays)
//
// `at` takes x,y,z as one word or as three; the position is a world position (x east, z south, y up). `heading` is where the
// nose points, in degrees, as the camera's yaw: 0 along +Z, 90 along +X (the game's own Racer_Teleport angle is this plus 180:
// NativeHeadingDeg). Garage slots count from 0. `garage:N` and `slot:N` mean `profile N`; `despawn` means `delete`. The id is
// what the camera's `car:<id>` names; `player` is the player's own car and takes only `profile`.
//
// `path` names a route file by a relative path (beside the scene file, or in a `routes` folder beside it; no `..`, no drive, no leading
// slash). `speed` is metres a second (`speed 12`) or a factor of the route's own speeds (`speed x1.5`); without it the route's own
// speeds are used. `mode kinematic` (the default) puts the car on the route at every update, exactly; `mode brain` hands the route to the
// game's own AI driver as destinations and leaves the driving to it.
//
// `ambient` takes a model name only (no garage slot): the name is one of the game's ambient types, up to 31 characters (the layout item's
// name buffer), and the game says whether it knows it when the car is placed. Its `heading` is the same scene heading as a spawned car's;
// the layout's own angle is 180 degrees from it (LayoutHeadingDeg).
//
// Any other verb is not this file's: ParseCarCommand says kOtherVerb and the scene parser lets it through (the runner warns).
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "path_follow.h"

namespace mc::cinematic {

enum class CarVerb : uint8_t { kSpawn, kDelete, kProfile, kPath, kStop, kAmbient };
enum class PathMode : uint8_t { kKinematic, kBrain };
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
    // path:
    std::string route;                  // the route file as written
    SpeedSpec speed;                    // how fast
    PathMode mode = PathMode::kKinematic;
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

// The angle AmbientMgr_AddAmbientToLayout takes (the layout item stores it as radians in +132) for a scene heading: the same angle as Racer_Teleport's,
// that is 180 degrees from the scene's. The traffic probe's run (TRAFFICPROBE.txt): a car placed with the angle 272 stood at the scene heading 92, the
// heading the player's own car had when the angle was taken from it (back row (sin a, 0, cos a), nose at a - 180).
inline double LayoutHeadingDeg(double scene_heading) {
    return NativeHeadingDeg(scene_heading);
}

// The scene heading of a car the layout placed with the angle `layout_heading`.
inline double SceneHeadingFromLayoutDeg(double layout_heading) {
    return NormalizeDegrees(layout_heading - 180.0);
}

// The scene heading of a car whose nose points along (forward_x, forward_z): atan2(x, z), in [0, 360).
inline double SceneHeadingDeg(double forward_x, double forward_z) {
    return NormalizeDegrees(std::atan2(forward_x, forward_z) * 180.0 / 3.14159265358979323846);
}

namespace car_verbs_detail {

constexpr double kMaxCoordinate = 100000.0;
constexpr int kMaxSlot = 999;
constexpr size_t kMaxModelChars = 47;  // the game keeps a vehicle name in a 64-byte buffer
constexpr size_t kMaxAmbientModelChars = 31;  // the layout item's name buffer is 32 bytes (sub_826E1418: snprintf(item + 40, 32, "%s", name))

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
constexpr const char* kAmbientUsage = "car <id> ambient <model> at <x,y,z> [heading <deg>]";

inline CarParse Fail(std::string* error, const std::string& message) {
    *error = message;
    return CarParse::kError;
}

// `ambient` true: the words of `car <id> ambient ...`: a model only (an ambient type of the game), the same `at` and `heading`.
inline CarParse ParseSpawn(const std::vector<std::string>& w, CarCommand* out, std::string* error, bool ambient = false) {
    const char* verb = ambient ? "ambient" : "spawn";
    const char* usage = ambient ? kAmbientUsage : kSpawnUsage;
    out->verb = ambient ? CarVerb::kAmbient : CarVerb::kSpawn;
    if (w.size() < 3) return Fail(error, std::string(ambient ? "ambient what? " : "spawn what? ") + usage);
    size_t i = 3;
    const std::string what = Lower(w[2]);
    if (ambient) {
        if (what == "profile" || what.find(':') != std::string::npos) {
            return Fail(error, "'" + w[2] + "' is not an ambient type: an ambient car is made from a vehicle name of the game's traffic list (va_gmc_yukon_05), not a garage car");
        }
        if (!IsModelName(what) || what.size() > kMaxAmbientModelChars) {
            return Fail(error, "'" + w[2] + "' is not an ambient type name (lower case letters, digits and _ - ., up to 31)");
        }
        out->source = CarSource::kModel;
        out->model = what;
    } else if (what == "profile") {
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
            if (have_at) return Fail(error, std::string(verb) + ": a second 'at'");
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
            if (out->has_heading) return Fail(error, std::string(verb) + ": a second 'heading'");
            double h = 0.0;
            if (i + 1 >= w.size() || !ParseNumber(w[i + 1], &h)) return Fail(error, "heading needs degrees (heading 270)");
            out->heading = NormalizeDegrees(h);
            out->has_heading = true;
            i += 2;
        } else {
            return Fail(error, std::string(verb) + ": unknown option '" + w[i] + "' (at, heading)");
        }
    }
    if (!have_at) return Fail(error, std::string(verb) + " needs a place: " + usage);
    return CarParse::kOk;
}

constexpr size_t kMaxRoutePathChars = 120;
constexpr const char* kPathUsage = "car <id> path <route file> [speed <x>] [mode brain|kinematic]";

// A route is named by a relative path under the scene's folder: letters, digits and _ - . / \ and blanks, no `..` part, no drive, no leading slash.
inline bool IsRoutePath(const std::string& s) {
    if (s.empty() || s.size() > kMaxRoutePathChars) return false;
    if (s.front() == '/' || s.front() == '\\') return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' ||
                        c == '\\' || c == ' ';
        if (!ok) return false;
    }
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find_first_of("/\\", start);
        if (end == std::string::npos) end = s.size();
        const std::string part = s.substr(start, end - start);
        if (part == ".." || part == ".") return false;
        start = end + 1;
    }
    return true;
}

inline CarParse ParsePath(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
    out->verb = CarVerb::kPath;
    if (w.size() < 3) return Fail(error, std::string("path needs a route file: ") + kPathUsage);
    if (!IsRoutePath(w[2])) {
        return Fail(error, "'" + w[2] +
                               "' is not a route file name (a name or a relative path next to the scene file or in its routes folder: letters, digits and "
                               "_ - . / and blanks; no .. and no drive or leading slash)");
    }
    out->route = w[2];
    bool have_speed = false;
    bool have_mode = false;
    size_t i = 3;
    while (i < w.size()) {
        const std::string key = Lower(w[i]);
        if (key == "speed") {
            if (have_speed) return Fail(error, "path: a second 'speed'");
            if (i + 1 >= w.size()) return Fail(error, "speed needs metres a second (speed 12) or a factor of the route's own speeds (speed x1.5)");
            if (!ParseSpeedWord(w[i + 1], &out->speed, error)) return CarParse::kError;
            have_speed = true;
            i += 2;
        } else if (key == "mode") {
            if (have_mode) return Fail(error, "path: a second 'mode'");
            if (i + 1 >= w.size()) return Fail(error, "mode needs brain or kinematic");
            const std::string m = Lower(w[i + 1]);
            if (m == "brain") out->mode = PathMode::kBrain;
            else if (m == "kinematic") out->mode = PathMode::kKinematic;
            else return Fail(error, "mode '" + w[i + 1] + "': brain (the game's own AI drives) or kinematic (the car is put on the route at every update)");
            have_mode = true;
            i += 2;
        } else {
            return Fail(error, "path: unknown option '" + w[i] + "' (speed, mode)");
        }
    }
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
    if (verb == "ambient") {
        if (player) return Fail(error, "'player' is the player's own car: pick another id (only `car player profile N` applies to it)");
        return ParseSpawn(w, out, error, true);
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
    if (verb == "path") {
        if (player) return Fail(error, "'player' is the player's own car: a path is for the scene's cars (the test harness's `follow` drives the player)");
        return ParsePath(w, out, error);
    }
    if (verb == "stop") {
        if (player) return Fail(error, "'player' is the player's own car: it cannot be stopped by a scene");
        if (w.size() != 2) return Fail(error, "car <id> stop takes nothing after it");
        out->verb = CarVerb::kStop;
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
        case CarVerb::kStop:
            return c.id + " stop";
        case CarVerb::kPath:
            return c.id + " path " + c.route + " at " + DescribeSpeed(c.speed) + " (" + (c.mode == PathMode::kBrain ? "brain" : "kinematic") + ")";
        case CarVerb::kSpawn:
        case CarVerb::kAmbient:
            break;
    }
    std::string out = c.id + (c.verb == CarVerb::kAmbient ? " ambient " : " spawn ");
    out += c.source == CarSource::kGarage ? "profile " + std::to_string(c.slot) : c.model;
    out += " at (" + Num(c.position[0]) + ", " + Num(c.position[1]) + ", " + Num(c.position[2]) + ") heading " + Num(c.heading);
    return out;
}

}  // namespace mc::cinematic
