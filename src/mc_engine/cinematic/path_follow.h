#pragma once
//
// Routes and how a car moves along one (trailer toolkit, work package M4c): the pure half. A route is a polyline on the ground, one point
// a line in the files the test harness already uses (`x,z,speed`, metres and metres a second; the files of ports\scripts\tours\routes),
// with an optional fourth column, the height. This file reads one, works out where a car is on it at a given time (position, heading,
// speed), and cuts it into the few destinations the game's own AI takes. It knows nothing of the game, the SDK or the log, so
// path_follow_test.cpp builds it on its own.
//
// FILE. One point a line: `x z`, `x,z`, `x,z,speed` or `x,z,speed,y` (commas or blanks; x east, z south, y up; speed in m/s). A line
// that starts with `#` and an empty line are skipped. A point with no speed has the harness's default (12 m/s). The height is all or
// nothing: a file in which some points have one and others do not is refused. At most 20000 points.
//
// SPEED. The car goes along the polyline at the speed of the route's points, linear in between (so the speed is continuous: no jump at
// a point), or at a constant speed of its own, or at the route's speeds times a factor. A speed under 0.5 m/s is held at 0.5 so that a
// path always ends. The time to cover a segment whose speed goes from v0 to v1 over L metres is exact (L ln(v1/v0) / (v1 - v0)), and
// the position at a time is its inverse, so the car's place is a closed-form function of the time since it started: a scrub, a restart
// and a take at another frame rate all give the same place at the same time.
//
// HEADING. The way the car's nose points is the direction of the chord of the route from 2 m behind the car to 2 m ahead of it (clamped at
// the ends): a corner is taken over 4 m instead of snapping, which is what a car's wheelbase does. In degrees, the scene's convention
// (car_verbs.h): 0 along +Z, 90 along +X.
//

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace mc::cinematic {

constexpr size_t kMaxRoutePoints = 20000;
constexpr double kDefaultRouteSpeed = 12.0;  // m/s, a point with no speed (what the harness's route reader uses)
constexpr double kMinPathSpeed = 0.5;        // m/s: slower is held at this
constexpr double kMaxPathSpeed = 120.0;
constexpr double kMinSpeedScale = 0.05;
constexpr double kMaxSpeedScale = 10.0;
constexpr double kHeadingWindow = 4.0;  // metres: the chord the heading is taken over

struct RoutePoint {
    double x = 0.0;
    double z = 0.0;
    double speed = -1.0;  // m/s; negative = the file gave none
    double y = 0.0;
    bool has_y = false;
};

struct Route {
    std::vector<RoutePoint> points;
    bool has_y = false;  // every point has a height
};

// Reads a route file's text. False with `error` ("line 7: ..."). A route has to have 2 points; length is checked by PathSampler::Build.
bool ParseRoute(std::string_view text, Route* out, std::string* error);

// How fast the car goes: the route's own speeds, a speed of its own, or the route's speeds times `value`.
enum class SpeedKind : uint8_t { kFile, kMetersPerSecond, kScale };
struct SpeedSpec {
    SpeedKind kind = SpeedKind::kFile;
    double value = 1.0;  // m/s (kMetersPerSecond) or the factor (kScale); unused for kFile
};
// "12" (m/s), "x1.5" or "1.5x" (a factor) into a SpeedSpec. False with `error` ("a speed from 0.5 to 120 metres a second ..."). Inline, so that the
// car statements' parser (car_verbs.h, read by scene.cpp) needs no other source.
// ...and DescribeSpeed: "12 m/s", "x1.5 of the route's speeds", "the route's own speeds".
namespace path_detail {

inline bool ParseNumber(const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double x = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(x)) return false;
    *v = x;
    return true;
}

inline std::string Num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

}  // namespace path_detail

inline bool ParseSpeedWord(std::string_view word, SpeedSpec* out, std::string* error) {
    std::string w(word);
    for (char& c : w) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    SpeedSpec spec;
    double v = 0.0;
    if (!w.empty() && w.front() == 'x') {
        spec.kind = SpeedKind::kScale;
        if (!path_detail::ParseNumber(w.substr(1), &v)) {
            *error = "speed '" + std::string(word) + "': a factor of the route's own speeds is x1.5 (or 1.5x)";
            return false;
        }
    } else if (w.size() > 1 && w.back() == 'x') {
        spec.kind = SpeedKind::kScale;
        if (!path_detail::ParseNumber(w.substr(0, w.size() - 1), &v)) {
            *error = "speed '" + std::string(word) + "': a factor of the route's own speeds is x1.5 (or 1.5x)";
            return false;
        }
    } else {
        spec.kind = SpeedKind::kMetersPerSecond;
        if (!path_detail::ParseNumber(w, &v)) {
            *error = "speed needs metres a second (speed 12) or a factor of the route's own speeds (speed x1.5), not '" + std::string(word) + "'";
            return false;
        }
    }
    spec.value = v;
    if (spec.kind == SpeedKind::kScale) {
        if (v < kMinSpeedScale || v > kMaxSpeedScale) {
            *error = "a speed factor from " + path_detail::Num(kMinSpeedScale) + " to " + path_detail::Num(kMaxSpeedScale) + " (speed x1.5), not " + path_detail::Num(v);
            return false;
        }
    } else if (v < kMinPathSpeed || v > kMaxPathSpeed) {
        *error = "a speed from " + path_detail::Num(kMinPathSpeed) + " to " + path_detail::Num(kMaxPathSpeed) + " metres a second (speed 12), not " + path_detail::Num(v);
        return false;
    }
    *out = spec;
    return true;
}

inline std::string DescribeSpeed(const SpeedSpec& speed) {
    switch (speed.kind) {
        case SpeedKind::kFile: return "the route's own speeds";
        case SpeedKind::kMetersPerSecond: return path_detail::Num(speed.value) + " m/s";
        case SpeedKind::kScale: return "x" + path_detail::Num(speed.value) + " of the route's speeds";
    }
    return "?";
}


struct PathSample {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool has_y = false;     // y is the route's height; false: the route has none and y is 0
    double heading = 0.0;   // degrees in [0, 360)
    double speed = 0.0;     // m/s; 0 once the route is over
    double s = 0.0;         // metres from the first point
    bool finished = false;  // the car is at the last point and the time is up
};

class PathSampler {
  public:
    // Builds the tables: false with `error` when the route is shorter than 1 m (points less than 1 cm apart are one point).
    bool Build(const Route& route, const SpeedSpec& speed, std::string* error);

    bool Valid() const { return points_.size() >= 2; }
    size_t PointCount() const { return points_.size(); }
    bool has_y() const { return has_y_; }
    double Length() const { return Valid() ? cum_s_.back() : 0.0; }
    // Seconds from the first point to the last at the chosen speeds.
    double Duration() const { return Valid() ? cum_t_.back() : 0.0; }
    // Length / Duration: the one speed that takes as long.
    double MeanSpeed() const;

    // Where the car is `tau` seconds after it started (before 0: the first point; after Duration(): the last, finished, speed 0).
    PathSample AtTime(double tau) const;
    // Where the route is `s` metres from its start (clamped to 0..Length()); speed is the speed of the profile there.
    PathSample AtDistance(double s) const;
    // Seconds from the start to the point `s` metres along.
    double TimeAtDistance(double s) const;

    // The nearest place on the route to (x, z): its distance from the start and how far off the route (0 on it).
    struct Nearest {
        double s = 0.0;
        double distance = 0.0;
    };
    Nearest NearestTo(double x, double z) const;

    // The route cut into at most `max_points` places, at least `min_spacing` metres apart, evenly spread along it, the last place always
    // the end; the start itself is left out (the car is there). For the game's AI, which takes up to 64 destinations.
    std::vector<PathSample> Resample(size_t max_points, double min_spacing) const;

  private:
    size_t SegmentAtDistance(double s) const;
    double DistanceAtTime(double tau, size_t* segment) const;
    PathSample PointAt(double s) const;

    std::vector<RoutePoint> points_;  // duplicates removed
    std::vector<double> speed_;       // m/s at each point, clamped
    std::vector<double> cum_s_;       // metres from the first point
    std::vector<double> cum_t_;       // seconds from the first point
    bool has_y_ = false;
};

}  // namespace mc::cinematic
