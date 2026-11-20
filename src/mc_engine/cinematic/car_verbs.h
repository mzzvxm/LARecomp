#pragma once
//
// The car statements of a scene (trailer toolkit, work package M4b): the words after `car`, read into a command. Pure: it knows
// nothing of the game, so cars_test.cpp builds it on its own, and scene.cpp uses it to report a bad car line with its line number
// when the file is parsed. Header only (inline), so that every program that links scene.cpp can do so without another source.
//
//   car <id> spawn <model> at <x,y,z> [heading <deg>] [paint <n>]      a car of that model (the game's vehicle name, vp_lam_murcielago_06)
//   car <id> spawn profile <N> at <x,y,z> [heading <deg>]  a car from garage slot N of the player's save, dressed as the owner left it
//   car <id> ambient <model> at <x,y,z> [heading <deg>] [paint <n>]    a REAL ambient (traffic-class) car, placed through AmbientMgr_AddAmbientToLayout (ambient_car.h);
//                                                          it stands still where it is put, with no route, and goes when the scene ends
//   car <id> spawn|ambient ... near <car> <right>,<up>,<forward> [heading <deg>]   the same, at a place in another car's frame (below)
//   car <id> delete                                        the car goes (every car a scene spawned or placed goes when it ends)
//   car player profile <N>                                 the player's own car becomes garage slot N (the old one comes back at the end)
//   car player at <x,y,z> [heading <deg>]                  the player's own car is put at that place (below); he is put back at the scene's end when that is safe
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
// `paint <n>` (also `colour <n>` and `color <n>`; n a whole number from 0 to 9999) is the car's PAINT. It is not a colour name: it is a
// number that always gives the same look to the same model, so two runs of a scene (or two builds) show the same car. Without it a scene
// car takes the paint its place in the scene file gives it (the first ambient car 0, the next 1, and so on: scene_seed.h, DefaultPaint),
// never a random one. What it does in the game: for an ambient car it replaces the random seed the game gives the car's own generator
// when it makes the car (scene_seed.h has the reading of the game's code and the CHECK); for a racer-class car (`spawn <model>`) the
// game's generators are held while the car is loaded. A garage car (`spawn profile N`) is dressed as the owner left it: `paint` there
// is an error.
//
// `near <car> <right>,<up>,<forward>` (instead of `at`, for `spawn` and `ambient`) puts the car at a place in ANOTHER car's frame: so many metres to its right, up and
// ahead of it (the frame is the car's own: right and forward lie along its matrix rows, so a car on a slope has them tilted with it). <car> is `player` or the id of a car the
// scene made earlier (a spawned car must be ready, an ambient car placed, before a car can be near it). The place is worked out when the line runs, from the pose that car has
// THEN; until it has a pose (a car spawned a moment ago, the player's car still being put by `car player at`) the car waits, and the log says what for. `heading` after `near`
// is relative: degrees from that car's heading (0, the default, is the same way as it; 180 faces it; 90 is a quarter turn to its LEFT: a scene heading grows counter-clockwise seen from
// above, 0 along +Z, 90 along +X). `at` and `near` do not go together.
// A car's width is not known here: two cars that are to stand 1.5 m apart side by side are (their half widths + 1.5) apart: 3.35 for two 1.85 m cars.
//
// `car player at <x,y,z> [heading <deg>]` is the one statement that moves the PLAYER's car. It is put there with Racer_Teleport on his racer when the line runs, with no
// speed (the same native a spawned car is put with; it sets the matrix, no ground snap, so ask for a y a little above the road and the car settles on it). Without `heading` he
// keeps the way he faces. When the scene ends he is put back where he stood before the first `car player at` (Racer_Teleport again) only if that is safe: his car is the same one,
// he is still within 5 m of where the scene left him (somebody who drove off is not fetched back), the game is in free roam, and the garage dressing is done.
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

enum class CarVerb : uint8_t { kSpawn, kDelete, kProfile, kPath, kStop, kAmbient, kPlace };
enum class PathMode : uint8_t { kKinematic, kBrain };
enum class CarSource : uint8_t { kModel, kGarage };  // spawn: a vehicle name, or a garage slot

struct CarCommand {
    std::string id;
    CarVerb verb = CarVerb::kSpawn;
    CarSource source = CarSource::kModel;
    std::string model;  // kModel: the vehicle name, lower case
    int slot = -1;      // kGarage, and the profile verb: the garage slot, from 0
    double position[3] = {0.0, 0.0, 0.0};
    double heading = 0.0;  // degrees in [0, 360): the camera's yaw convention (`relative`: the offset from the other car's heading)
    bool has_heading = false;
    // spawn, ambient: `near <car> <right>,<up>,<forward>`: the place is in the frame of the car `anchor` (an id, or "player"), worked out when the line runs
    // (ResolveRelative in car_spawn.h); `heading` is then the offset from that car's heading. `position` is not used until the Director has resolved it.
    // (Named `relative`, not `near`: windows.h defines `near` as an empty macro.)
    bool relative = false;
    std::string anchor;
    double offset[3] = {0.0, 0.0, 0.0};  // metres to the anchor's right, up and ahead
    // path:
    std::string route;                  // the route file as written
    SpeedSpec speed;                    // how fast
    PathMode mode = PathMode::kKinematic;
    // spawn, ambient: the paint (see the header), from `paint <n>`; without it the Director gives the car the next default one (paint_default).
    bool has_paint = false;
    int paint = 0;
    bool paint_default = false;  // set by the Director: the paint was not written in the scene
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
constexpr int kMaxPaint = 9999;
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

inline bool ParsePaint(const std::string& s, int* v) {
    if (s.empty() || s.size() > 4) return false;
    int x = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        x = x * 10 + (c - '0');
    }
    if (x > kMaxPaint) return false;
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

constexpr const char* kSpawnUsage = "car <id> spawn <model|profile N> (at <x,y,z> | near <car> <right,up,forward>) [heading <deg>] [paint <n>]";
constexpr const char* kAmbientUsage = "car <id> ambient <model> (at <x,y,z> | near <car> <right,up,forward>) [heading <deg>] [paint <n>]";
constexpr const char* kPlaceUsage = "car player at <x,y,z> [heading <deg>]";
constexpr double kMaxNearOffset = 1000.0;  // metres: a car "near" another is within a kilometre (an ambient car must be within one of the player)

inline CarParse Fail(std::string* error, const std::string& message) {
    *error = message;
    return CarParse::kError;
}

// The three numbers after a keyword: as one word "a,b,c" or as three words. `first` is the index of the first number (the word after `at`, or after the car's id for `near`),
// `key` the keyword for the messages, `shape` what the three are ("x,y,z") and `what` what they make ("position"). Moves *i past them. The messages are the `at` ones of old.
inline bool ReadTriple(const std::vector<std::string>& w, size_t* i, size_t first, const char* key, const char* shape, const char* what, double out[3], std::string* error) {
    if (first >= w.size()) {
        *error = std::string(key) + " needs " + shape;
        return false;
    }
    if (w[first].find(',') != std::string::npos) {
        if (!ParseTriple(w[first], out)) {
            *error = std::string("bad ") + what + " '" + w[first] + "' (" + shape + ": three numbers)";
            return false;
        }
        *i = first + 1;
        return true;
    }
    if (first + 2 >= w.size()) {
        *error = std::string(key) + " needs " + shape + " (as " + shape + " or as three numbers)";
        return false;
    }
    for (size_t k = 0; k < 3; ++k) {
        if (!ParseNumber(w[first + k], &out[k])) {
            *error = std::string("bad ") + what + " '" + w[first + k] + "' (" + shape + ": three numbers)";
            return false;
        }
    }
    *i = first + 3;
    return true;
}

// A car's id as the scene's parser takes one: a letter or _, then letters, digits and _.
inline bool IsCarId(const std::string& s) {
    if (s.empty()) return false;
    for (size_t k = 0; k < s.size(); ++k) {
        const char c = s[k];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && k > 0))) return false;
    }
    return true;
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
            if (!ReadTriple(w, &i, i + 1, "at", "x,y,z", "position", out->position, error)) return CarParse::kError;
            for (double c : out->position) {
                if (std::fabs(c) > kMaxCoordinate) return Fail(error, "the position is outside the world (more than 100000 from the origin)");
            }
            have_at = true;
        } else if (key == "near") {
            if (out->relative) return Fail(error, std::string(verb) + ": a second 'near'");
            if (i + 1 >= w.size()) return Fail(error, "near needs a car and a place in its frame (near player 3.35,0,0: right, up, forward in metres)");
            std::string anchor = w[i + 1];
            if (!IsCarId(anchor)) {
                return Fail(error, "near needs the id of a car, or player ('" + anchor + "' is not one): near player 3.35,0,0 (right, up, forward in metres)");
            }
            if (Lower(anchor) == "player") anchor = "player";
            if (anchor == out->id) return Fail(error, "car " + out->id + " cannot be near itself");
            if (!ReadTriple(w, &i, i + 2, "near", "right,up,forward", "offset", out->offset, error)) return CarParse::kError;
            for (double c : out->offset) {
                if (std::fabs(c) > kMaxNearOffset) return Fail(error, "the offset is more than 1000 m from the car it is near: a car that far is placed with `at`");
            }
            out->relative = true;
            out->anchor = anchor;
        } else if (key == "heading") {
            if (out->has_heading) return Fail(error, std::string(verb) + ": a second 'heading'");
            double h = 0.0;
            if (i + 1 >= w.size() || !ParseNumber(w[i + 1], &h)) return Fail(error, "heading needs degrees (heading 270)");
            out->heading = NormalizeDegrees(h);
            out->has_heading = true;
            i += 2;
        } else if (key == "paint" || key == "colour" || key == "color") {
            if (out->has_paint) return Fail(error, std::string(verb) + ": a second 'paint'");
            if (out->source == CarSource::kGarage) {
                return Fail(error, "a garage car (spawn profile N) keeps the paint the owner gave it: paint is for a model name or an ambient car");
            }
            int p = 0;
            if (i + 1 >= w.size() || !ParsePaint(w[i + 1], &p)) return Fail(error, "paint needs a whole number from 0 to 9999 (paint 3)");
            out->paint = p;
            out->has_paint = true;
            i += 2;
        } else {
            return Fail(error, std::string(verb) + ": unknown option '" + w[i] + "' (at, near, heading, paint)");
        }
    }
    if (have_at && out->relative) {
        return Fail(error, std::string(verb) + ": `at` (a place in the world) and `near` (a place in a car's frame) do not go together: give one of them");
    }
    if (!have_at && !out->relative) return Fail(error, std::string(verb) + " needs a place: " + usage);
    return CarParse::kOk;
}

// `car player at <x,y,z> [heading <deg>]` (w = the words after `car`: the id, `at`, ...): the player's own car is put at that place.
inline CarParse ParsePlace(const std::vector<std::string>& w, CarCommand* out, std::string* error) {
    out->verb = CarVerb::kPlace;
    bool have_at = false;
    size_t i = 1;
    while (i < w.size()) {
        const std::string key = Lower(w[i]);
        if (key == "at") {
            if (have_at) return Fail(error, "car player at: a second 'at'");
            if (!ReadTriple(w, &i, i + 1, "at", "x,y,z", "position", out->position, error)) return CarParse::kError;
            for (double c : out->position) {
                if (std::fabs(c) > kMaxCoordinate) return Fail(error, "the position is outside the world (more than 100000 from the origin)");
            }
            have_at = true;
        } else if (key == "heading") {
            if (out->has_heading) return Fail(error, "car player at: a second 'heading'");
            double h = 0.0;
            if (i + 1 >= w.size() || !ParseNumber(w[i + 1], &h)) return Fail(error, "heading needs degrees (heading 270)");
            out->heading = NormalizeDegrees(h);
            out->has_heading = true;
            i += 2;
        } else {
            return Fail(error, "car player at: unknown option '" + w[i] + "' (heading; a car near another is `spawn ... near`)");
        }
    }
    if (!have_at) return Fail(error, std::string("car player at needs a place: ") + kPlaceUsage);
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
        if (player) return Fail(error, "'player' is the player's own car: pick another id (only `car player profile N` and `car player at ...` apply to it)");
        return ParseSpawn(w, out, error);
    }
    if (verb == "ambient") {
        if (player) return Fail(error, "'player' is the player's own car: pick another id (only `car player profile N` and `car player at ...` apply to it)");
        return ParseSpawn(w, out, error, true);
    }
    if (verb == "delete" || verb == "despawn") {
        if (player) return Fail(error, "'player' is the player's own car: it cannot be deleted");
        if (w.size() != 2) return Fail(error, "car <id> delete takes nothing after it");
        out->verb = CarVerb::kDelete;
        return CarParse::kOk;
    }
    if (verb == "at") {
        if (!player) {
            return Fail(error, "only the player's car is put with `at` (car player at <x,y,z> [heading <deg>]); a spawned car is placed by its own spawn or ambient line (`at`, or `near` another car)");
        }
        return ParsePlace(w, out, error);
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
        case CarVerb::kPlace:
            return c.id + " at (" + Num(c.position[0]) + ", " + Num(c.position[1]) + ", " + Num(c.position[2]) + ")" +
                   (c.has_heading ? " heading " + Num(c.heading) : std::string(" (keeps the way it faces)"));
        case CarVerb::kSpawn:
        case CarVerb::kAmbient:
            break;
    }
    std::string out = c.id + (c.verb == CarVerb::kAmbient ? " ambient " : " spawn ");
    out += c.source == CarSource::kGarage ? "profile " + std::to_string(c.slot) : c.model;
    if (c.relative) {
        out += " near " + c.anchor + " (" + Num(c.offset[0]) + " right, " + Num(c.offset[1]) + " up, " + Num(c.offset[2]) + " ahead) heading " + (c.has_heading ? "+" + Num(c.heading) : std::string("+0.00")) +
               " from its own";
    } else {
        out += " at (" + Num(c.position[0]) + ", " + Num(c.position[1]) + ", " + Num(c.position[2]) + ") heading " + Num(c.heading);
    }
    if (c.has_paint) out += " paint " + std::to_string(c.paint) + (c.paint_default ? " (the default)" : "");
    return out;
}

}  // namespace mc::cinematic
