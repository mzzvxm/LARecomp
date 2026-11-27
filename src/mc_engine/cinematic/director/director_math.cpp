// The Director's pure helpers: see director_math.h.

#include "director_math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "../camera.h"
#include "../freecam.h"
#include "scene_edit.h"

namespace mc::cinematic::director {

namespace cam = mc::cinematic::camera;
namespace fc = mc::cinematic::freecam;

// ---------------------------------------------------------------------------------------------------------------- the camera

namespace {

double Dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

fc::Pose ToFlyPose(const SnapPose& p) {
    fc::Pose f;
    f.position = {p.pos[0], p.pos[1], p.pos[2]};
    f.yaw = p.yaw;
    f.pitch = p.pitch;
    f.roll = p.roll;
    f.fov = p.fov;
    return f;
}

}  // namespace

void PoseAxes(const SnapPose& pose, double right[3], double up[3], double forward[3]) {
    const cam::Basis b = fc::BasisOf(ToFlyPose(pose));
    right[0] = b.right.x, right[1] = b.right.y, right[2] = b.right.z;
    up[0] = b.up.x, up[1] = b.up.y, up[2] = b.up.z;
    forward[0] = b.forward.x, forward[1] = b.forward.y, forward[2] = b.forward.z;
}

void PoseFromAxes(const double pos[3], const double right[3], const double up[3], const double forward[3], double fov, double keep_yaw, SnapPose* out) {
    cam::Basis b;
    b.right = {right[0], right[1], right[2]};
    b.up = {up[0], up[1], up[2]};
    b.forward = {forward[0], forward[1], forward[2]};
    double yaw = 0, pitch = 0, roll = 0;
    fc::AnglesFromBasis(b, keep_yaw, &yaw, &pitch, &roll);
    out->pos[0] = pos[0], out->pos[1] = pos[1], out->pos[2] = pos[2];
    out->yaw = yaw;
    out->pitch = pitch;
    out->roll = roll;
    out->fov = fov;
}

void ViewProjection(const SnapPose& pose, double aspect, double near_plane, double far_plane, double out[16]) {
    double r[3], u[3], f[3];
    PoseAxes(pose, r, u, f);
    const double fov = std::clamp(pose.fov, 1.0, 170.0);
    const double th = std::tan(fov * cam::kDeg * 0.5);
    const double asp = aspect > 0.01 ? aspect : 16.0 / 9.0;
    const double n = std::max(near_plane, 1e-4);
    const double fa = std::max(far_plane, n + 1e-3);
    const double a = fa / (fa - n);
    const double b = -fa * n / (fa - n);
    const double sx = 1.0 / (th * asp);
    const double sy = 1.0 / th;
    const double rp = Dot3(r, pose.pos), up_ = Dot3(u, pose.pos), fp = Dot3(f, pose.pos);
    for (int i = 0; i < 3; ++i) {
        out[0 + i] = r[i] * sx;
        out[4 + i] = u[i] * sy;
        out[8 + i] = f[i] * a;
        out[12 + i] = f[i];
    }
    out[3] = -rp * sx;
    out[7] = -up_ * sy;
    out[11] = -fp * a + b;
    out[15] = -fp;
}

bool ProjectPoint(const double m[16], const double p[3], double* ndc_x, double* ndc_y, double* depth, double* w) {
    const auto row = [&](int r) { return m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]; };
    const double cx = row(0), cy = row(1), cz = row(2), cw = row(3);
    if (w) *w = cw;
    if (!(cw > 1e-9)) return false;
    if (ndc_x) *ndc_x = cx / cw;
    if (ndc_y) *ndc_y = cy / cw;
    if (depth) *depth = cz / cw;
    return true;
}

void LookAtAngles(const double from[3], const double to[3], double* yaw, double* pitch) {
    const double dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const double flat = std::hypot(dx, dz);
    if (flat < 1e-9 && std::fabs(dy) < 1e-9) {
        *yaw = 0.0;
        *pitch = 0.0;
        return;
    }
    *yaw = flat < 1e-9 ? 0.0 : std::atan2(dx, dz) / cam::kDeg;
    *pitch = std::atan2(dy, flat) / cam::kDeg;
}

// ---------------------------------------------------------------------------------------------------------------- time

double WorldTimeAt(const Scene& scene, double t, double step) {
    if (!(t > 0.0)) return 0.0;
    const bool any = std::any_of(scene.events.begin(), scene.events.end(), [](const Event& e) { return e.track == Track::kTimeScale; });
    if (!any) return t;
    step = std::max(step, 1e-4);
    double world = 0.0;
    double at = 0.0;
    while (at < t - 1e-12) {
        const double dt = std::min(step, t - at);
        const WorldState w = EvaluateWorld(scene, at + dt * 0.5);
        const double scale = w.timescale_set ? std::clamp(w.timescale, 0.0, 10.0) : 1.0;
        world += scale * dt;
        at += dt;
    }
    return world;
}

ScrubInfo ScrubbableAt(const Scene& scene, double t) {
    ScrubInfo info;
    std::vector<std::string> why;
    for (const Event& e : scene.events) {
        if (e.track != Track::kCar) continue;
        for (size_t i = 0; i + 1 < e.words.size(); ++i) {
            if (e.words[i] == "mode" && e.words[i + 1] == "brain") {
                why.push_back("car " + (e.words.empty() ? std::string("?") : e.words[0]) + " drives in mode brain (the game's own driver cannot be rewound)");
                break;
            }
        }
    }
    const WorldState w = EvaluateWorld(scene, t);
    if (w.traffic != 0) why.push_back("traffic is running (add `at 0 traffic off`)");
    if (w.peds != 0 && !(w.peds_scale == 0.0)) why.push_back("peds are running (add `at 0 peds off`)");
    if (!why.empty()) {
        info.scrubbable = false;
        for (size_t i = 0; i < why.size(); ++i) info.hint += (i ? "; " : "") + why[i];
        info.hint += ": a seek cannot rewind them, play from the start with transport.restart";
    }
    return info;
}

// ---------------------------------------------------------------------------------------------------------------- routes

namespace {

void Rdp(const std::vector<WayPoint>& p, size_t a, size_t b, double tol, std::vector<bool>* keep) {
    if (b <= a + 1) return;
    double worst = -1.0;
    size_t at = a;
    const WayPoint &pa = p[a], &pb = p[b];
    const double dx = pb.x - pa.x, dy = pb.y - pa.y, dz = pb.z - pa.z;
    const double len2 = dx * dx + dy * dy + dz * dz;
    for (size_t i = a + 1; i < b; ++i) {
        const double ex = p[i].x - pa.x, ey = p[i].y - pa.y, ez = p[i].z - pa.z;
        double d;
        if (len2 < 1e-12) {
            d = std::sqrt(ex * ex + ey * ey + ez * ez);
        } else {
            const double u = std::clamp((ex * dx + ey * dy + ez * dz) / len2, 0.0, 1.0);
            const double qx = ex - u * dx, qy = ey - u * dy, qz = ez - u * dz;
            d = std::sqrt(qx * qx + qy * qy + qz * qz);
        }
        if (d > worst) {
            worst = d;
            at = i;
        }
    }
    if (worst > tol) {
        (*keep)[at] = true;
        Rdp(p, a, at, tol, keep);
        Rdp(p, at, b, tol, keep);
    }
}

}  // namespace

std::vector<WayPoint> ReduceRoute(const std::vector<WayPoint>& points, double tolerance_m, double speed_step) {
    if (points.size() <= 2) return points;
    std::vector<bool> keep(points.size(), false);
    keep.front() = keep.back() = true;
    // a point where the speed has changed enough stays: the car's pace is part of the shot
    double last_speed = points.front().speed;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        if (std::fabs(points[i].speed - last_speed) > speed_step) {
            keep[i] = true;
            last_speed = points[i].speed;
        }
    }
    size_t a = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        if (!keep[i]) continue;
        Rdp(points, a, i, tolerance_m, &keep);
        a = i;
    }
    std::vector<WayPoint> out;
    for (size_t i = 0; i < points.size(); ++i) {
        if (keep[i]) out.push_back(points[i]);
    }
    return out;
}

double RouteLength(const std::vector<WayPoint>& points) {
    double len = 0.0;
    for (size_t i = 1; i < points.size(); ++i) len += std::hypot(points[i].x - points[i - 1].x, points[i].z - points[i - 1].z);
    return len;
}

std::string FormatRouteFile(const std::vector<WayPoint>& points, const std::string& header) {
    std::string out;
    size_t i = 0;
    while (i < header.size()) {
        const size_t nl = header.find('\n', i);
        out += "# " + header.substr(i, nl == std::string::npos ? std::string::npos : nl - i) + "\n";
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    for (const WayPoint& p : points) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f,%.2f\n", p.x, p.z, p.speed, p.y);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- paths

PathCheck CheckRelativePath(std::string_view path) {
    PathCheck r;
    if (path.empty()) {
        r.error = "the path is empty";
        return r;
    }
    if (path.size() > 200) {
        r.error = "the path is longer than 200 characters";
        return r;
    }
    if (path.find('\0') != std::string_view::npos) {
        r.error = "the path holds a NUL";
        return r;
    }
    if (path.front() == '/' || path.front() == '\\') {
        r.error = "the path must be relative (no leading separator)";
        return r;
    }
    std::vector<std::string> parts;
    std::string cur;
    const auto flush = [&]() {
        if (!cur.empty()) parts.push_back(cur);
        cur.clear();
    };
    for (const char c : path) {
        if (c == '/' || c == '\\') flush();
        else cur.push_back(c);
    }
    flush();
    if (parts.empty()) {
        r.error = "the path has no name in it";
        return r;
    }
    static const char* kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::string joined;
    for (const std::string& p : parts) {
        if (p == "." || p == "..") {
            r.error = "the path must not hold '.' or '..'";
            return r;
        }
        if (p.find_first_of(":<>\"|?*") != std::string::npos || std::any_of(p.begin(), p.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
            r.error = "the path must not hold a drive, a colon or any of < > \" | ? *";
            return r;
        }
        if (p.back() == '.' || p.back() == ' ') {
            r.error = "a name must not end with a dot or a blank";
            return r;
        }
        std::string stem = p.substr(0, p.find('.'));
        for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* res : kReserved) {
            if (stem == res) {
                r.error = "'" + p + "' is a reserved device name";
                return r;
            }
        }
        if (!joined.empty()) joined += "/";
        joined += p;
    }
    r.ok = true;
    r.normalised = joined;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- the scene as rows

EventKind KindOfTrack(Track track) {
    switch (track) {
        case Track::kCamera: return EventKind::kCamera;
        case Track::kCut: return EventKind::kCut;
        case Track::kCar: return EventKind::kCar;
        case Track::kHour:
        case Track::kWeather:
        case Track::kTraffic:
        case Track::kPeds:
        case Track::kParked:
        case Track::kPolice:
        case Track::kTimeScale:
        case Track::kPreset:
        case Track::kPresetLabel: return EventKind::kWorld;
        case Track::kCvar: return EventKind::kSetting;
        case Track::kLabel: return EventKind::kLabel;
        case Track::kMark: return EventKind::kMark;
        case Track::kHud: return EventKind::kHud;
        case Track::kSplit: return EventKind::kSplit;
        case Track::kCapture: return EventKind::kCapture;
        case Track::kStart: return EventKind::kOther;
    }
    return EventKind::kOther;
}

std::string StatementText(std::string_view text, int line) {
    if (line < 1) return {};
    size_t i = 0;
    for (int n = 1; n < line; ++n) {
        const size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) return {};
        i = nl + 1;
    }
    if (i > text.size()) return {};
    const size_t nl = text.find('\n', i);
    return StripComment(text.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i));
}

std::vector<SnapEvent> TimelineEvents(const Scene& scene, std::string_view file_text) {
    std::vector<SnapEvent> out;
    out.reserve(scene.events.size());
    for (const Event& e : scene.events) {
        SnapEvent s;
        s.line = e.line;
        s.t = e.t;
        s.track = TrackName(e.track);
        s.text = StatementText(file_text, e.line);
        s.kind = KindOfTrack(e.track);
        if (e.track == Track::kCar && !e.words.empty()) s.car = e.words[0];
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> SceneCarIds(const Scene& scene) {
    std::vector<std::string> ids;
    for (const Event& e : scene.events) {
        if (e.track != Track::kCar || e.words.empty()) continue;
        if (std::find(ids.begin(), ids.end(), e.words[0]) == ids.end()) ids.push_back(e.words[0]);
    }
    return ids;
}

}  // namespace mc::cinematic::director
