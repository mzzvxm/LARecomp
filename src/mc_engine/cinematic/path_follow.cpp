// Routes and how a car moves along one: see path_follow.h.

#include "path_follow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mc::cinematic {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMergeDistance = 0.01;  // metres: points closer than this are one
constexpr double kMinRouteLength = 1.0;  // metres

using path_detail::Num;
using path_detail::ParseNumber;

double NormalizeDeg(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    if (d >= 360.0) d = 0.0;
    return d;
}

}  // namespace

bool ParseRoute(std::string_view text, Route* out, std::string* error, size_t min_points) {
    *out = Route{};
    error->clear();
    size_t pos = 0;
    int line_no = 0;
    size_t with_y = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string line(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++line_no;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        for (char& c : line) {
            if (c == ',' || c == '\t' || c == '\r') c = ' ';
        }
        std::vector<std::string> words;
        std::string cur;
        for (char c : line) {
            if (c == ' ') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) words.push_back(cur);
        if (words.empty()) continue;

        const std::string where = "line " + std::to_string(line_no) + ": ";
        if (words.size() < 2 || words.size() > 4) {
            *error = where + "a route point is x,z[,speed[,y]] (" + std::to_string(words.size()) + " numbers here)";
            return false;
        }
        double v[4] = {0, 0, -1.0, 0};
        for (size_t i = 0; i < words.size(); ++i) {
            if (!ParseNumber(words[i], &v[i])) {
                *error = where + "'" + words[i] + "' is not a number";
                return false;
            }
        }
        if (std::fabs(v[0]) > 100000.0 || std::fabs(v[1]) > 100000.0 || (words.size() == 4 && std::fabs(v[3]) > 100000.0)) {
            *error = where + "a coordinate outside the world (more than 100000 from the origin)";
            return false;
        }
        if (words.size() >= 3 && (v[2] < 0.0 || v[2] > 1000.0)) {
            *error = where + "a speed from 0 to 1000 metres a second, not " + Num(v[2]);
            return false;
        }
        RoutePoint p;
        p.x = v[0];
        p.z = v[1];
        p.speed = words.size() >= 3 ? v[2] : -1.0;
        if (words.size() == 4) {
            p.y = v[3];
            p.has_y = true;
            ++with_y;
        }
        if (out->points.size() >= kMaxRoutePoints) {
            *error = where + "more than " + std::to_string(kMaxRoutePoints) + " points";
            return false;
        }
        out->points.push_back(p);
    }
    if (out->points.size() < min_points) {
        *error = "a route needs at least " + std::to_string(min_points) + (min_points == 1 ? " point" : " points") + " (x,z[,speed[,y]] a line), this one has " + std::to_string(out->points.size());
        return false;
    }
    if (with_y != 0 && with_y != out->points.size()) {
        *error = "the height (the fourth number) is on " + std::to_string(with_y) + " of " + std::to_string(out->points.size()) +
                 " points: give it on all of them or on none";
        return false;
    }
    out->has_y = with_y != 0;
    return true;
}

// ================================================================================================================ PathSampler

bool PathSampler::Build(const Route& route, const SpeedSpec& speed, std::string* error) {
    points_.clear();
    speed_.clear();
    cum_s_.clear();
    cum_t_.clear();
    has_y_ = false;
    error->clear();
    for (const RoutePoint& p : route.points) {
        if (!points_.empty() && std::hypot(p.x - points_.back().x, p.z - points_.back().z) < kMergeDistance) continue;
        points_.push_back(p);
    }
    if (points_.size() < 2) {
        *error = "the route has no two points more than 1 cm apart";
        points_.clear();
        return false;
    }
    cum_s_.assign(points_.size(), 0.0);
    for (size_t i = 1; i < points_.size(); ++i) {
        cum_s_[i] = cum_s_[i - 1] + std::hypot(points_[i].x - points_[i - 1].x, points_[i].z - points_[i - 1].z);
    }
    if (cum_s_.back() < kMinRouteLength) {
        *error = "the route is " + Num(cum_s_.back()) + " m long: a path needs at least 1 m";
        points_.clear();
        cum_s_.clear();
        return false;
    }
    has_y_ = route.has_y;
    speed_.resize(points_.size());
    for (size_t i = 0; i < points_.size(); ++i) {
        const double file = points_[i].speed >= 0.0 ? points_[i].speed : kDefaultRouteSpeed;
        double v = file;
        if (speed.kind == SpeedKind::kMetersPerSecond) v = speed.value;
        else if (speed.kind == SpeedKind::kScale) v = file * speed.value;
        speed_[i] = std::clamp(v, kMinPathSpeed, kMaxPathSpeed);
    }
    cum_t_.assign(points_.size(), 0.0);
    for (size_t i = 0; i + 1 < points_.size(); ++i) {
        const double len = cum_s_[i + 1] - cum_s_[i];
        const double v0 = speed_[i];
        const double v1 = speed_[i + 1];
        const double dt = std::fabs(v1 - v0) < 1e-9 * std::max(v0, v1) ? len / v0 : len * std::log(v1 / v0) / (v1 - v0);
        cum_t_[i + 1] = cum_t_[i] + dt;
    }
    return true;
}

double PathSampler::MeanSpeed() const {
    return Valid() && Duration() > 0.0 ? Length() / Duration() : 0.0;
}

size_t PathSampler::SegmentAtDistance(double s) const {
    const auto it = std::upper_bound(cum_s_.begin(), cum_s_.end(), s);
    size_t i = it == cum_s_.begin() ? 0 : static_cast<size_t>(it - cum_s_.begin()) - 1;
    if (i + 2 > points_.size()) i = points_.size() - 2;
    return i;
}

PathSample PathSampler::PointAt(double s) const {
    PathSample out;
    if (!Valid()) return out;
    const double length = Length();
    s = std::clamp(s, 0.0, length);
    auto place = [&](double d, double* x, double* z, double* y, double* v) {
        const size_t i = SegmentAtDistance(d);
        const double seg = cum_s_[i + 1] - cum_s_[i];
        const double f = seg > 0.0 ? std::clamp((d - cum_s_[i]) / seg, 0.0, 1.0) : 0.0;
        *x = points_[i].x + (points_[i + 1].x - points_[i].x) * f;
        *z = points_[i].z + (points_[i + 1].z - points_[i].z) * f;
        *y = has_y_ ? points_[i].y + (points_[i + 1].y - points_[i].y) * f : 0.0;
        *v = speed_[i] + (speed_[i + 1] - speed_[i]) * f;
    };
    double x = 0, z = 0, y = 0, v = 0;
    place(s, &x, &z, &y, &v);
    out.x = x;
    out.z = z;
    out.y = y;
    out.has_y = has_y_;
    out.speed = v;
    out.s = s;

    // the heading: the chord from 2 m behind to 2 m ahead
    const double a = std::max(0.0, s - kHeadingWindow * 0.5);
    const double b = std::min(length, s + kHeadingWindow * 0.5);
    double ax = 0, az = 0, ay = 0, av = 0, bx = 0, bz = 0, by = 0, bv = 0;
    place(a, &ax, &az, &ay, &av);
    place(b, &bx, &bz, &by, &bv);
    double dx = bx - ax;
    double dz = bz - az;
    if (std::hypot(dx, dz) < 1e-6) {
        const size_t i = SegmentAtDistance(s);
        dx = points_[i + 1].x - points_[i].x;
        dz = points_[i + 1].z - points_[i].z;
    }
    out.heading = NormalizeDeg(std::atan2(dx, dz) * 180.0 / kPi);
    return out;
}

PathSample PathSampler::AtDistance(double s) const {
    return PointAt(s);
}

double PathSampler::DistanceAtTime(double tau, size_t* segment) const {
    const auto it = std::upper_bound(cum_t_.begin(), cum_t_.end(), tau);
    size_t i = it == cum_t_.begin() ? 0 : static_cast<size_t>(it - cum_t_.begin()) - 1;
    if (i + 2 > points_.size()) i = points_.size() - 2;
    if (segment) *segment = i;
    const double len = cum_s_[i + 1] - cum_s_[i];
    const double local = tau - cum_t_[i];
    const double v0 = speed_[i];
    const double a = (speed_[i + 1] - v0) / len;  // dv/ds: v grows as exp(a t)
    double d;
    if (std::fabs(a * local) < 1e-9) d = v0 * local;
    else d = v0 * std::expm1(a * local) / a;
    return cum_s_[i] + std::clamp(d, 0.0, len);
}

PathSample PathSampler::AtTime(double tau) const {
    if (!Valid()) return {};
    if (!(tau > 0.0)) return PointAt(0.0);
    if (tau >= Duration()) {
        PathSample end = PointAt(Length());
        end.finished = true;
        end.speed = 0.0;
        return end;
    }
    return PointAt(DistanceAtTime(tau, nullptr));
}

double PathSampler::TimeAtDistance(double s) const {
    if (!Valid()) return 0.0;
    s = std::clamp(s, 0.0, Length());
    const size_t i = SegmentAtDistance(s);
    const double len = cum_s_[i + 1] - cum_s_[i];
    const double d = s - cum_s_[i];
    const double v0 = speed_[i];
    const double v1 = speed_[i + 1];
    if (std::fabs(v1 - v0) < 1e-9 * std::max(v0, v1)) return cum_t_[i] + d / v0;
    const double a = (v1 - v0) / len;
    return cum_t_[i] + std::log((v0 + a * d) / v0) / a;
}

PathSampler::Nearest PathSampler::NearestTo(double x, double z) const {
    Nearest best;
    if (!Valid()) return best;
    best.distance = 1e300;
    for (size_t i = 0; i + 1 < points_.size(); ++i) {
        const double ax = points_[i].x, az = points_[i].z;
        const double bx = points_[i + 1].x, bz = points_[i + 1].z;
        const double dx = bx - ax, dz = bz - az;
        const double len2 = dx * dx + dz * dz;
        const double t = len2 > 0.0 ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(x - (ax + dx * t), z - (az + dz * t));
        if (d < best.distance) {
            best.distance = d;
            best.s = cum_s_[i] + t * (cum_s_[i + 1] - cum_s_[i]);
        }
    }
    return best;
}

std::vector<PathSample> PathSampler::Resample(size_t max_points, double min_spacing) const {
    std::vector<PathSample> out;
    if (!Valid() || max_points == 0) return out;
    const double length = Length();
    const double spacing = std::max(min_spacing, 0.01);
    size_t n = static_cast<size_t>(std::floor(length / spacing));
    n = std::clamp<size_t>(n, 1, max_points);
    for (size_t k = 1; k <= n; ++k) out.push_back(AtDistance(length * static_cast<double>(k) / static_cast<double>(n)));
    return out;
}

}  // namespace mc::cinematic
