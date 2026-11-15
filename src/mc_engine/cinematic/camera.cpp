//
// The camera rig's maths, track parser and evaluator: see camera.h for the keys and for what this file is not (no game, no log).
//

#include "camera.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>

namespace mc::cinematic::camera {

// ---------------------------------------------------------------------------------------------------------------- vectors

double Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double Length(Vec3 a) {
    return std::sqrt(Dot(a, a));
}

Vec3 Normalize(Vec3 a, Vec3 fallback) {
    const double l = Length(a);
    return l < 1e-9 ? fallback : a * (1.0 / l);
}

Vec3 Lerp(Vec3 a, Vec3 b, double u) {
    return a + (b - a) * u;
}

double Lerp(double a, double b, double u) {
    return a + (b - a) * u;
}

// ---------------------------------------------------------------------------------------------------------------- rotations

Quat Normalize(Quat q) {
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-12) return Quat{};
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

Quat Slerp(Quat a, Quat b, double u) {
    a = Normalize(a);
    b = Normalize(b);
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0.0) {  // q and -q are the same rotation: take the short way
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    if (d > 0.9995) {
        return Normalize({a.w + (b.w - a.w) * u, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u});
    }
    const double theta0 = std::acos(std::min(d, 1.0));
    const double theta = theta0 * u;
    const double sin0 = std::sin(theta0);
    const double s0 = std::cos(theta) - d * std::sin(theta) / sin0;
    const double s1 = std::sin(theta) / sin0;
    return Normalize({a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1});
}

Vec3 Rotate(Quat q, Vec3 v) {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = Cross(qv, v) * 2.0;
    return v + t * q.w + Cross(qv, t);
}

Quat ToQuat(const Basis& b) {
    // The matrix with right, up, back (minus forward) as its columns: a proper rotation (right x up = back).
    const double m00 = b.right.x, m01 = b.up.x, m02 = -b.forward.x;
    const double m10 = b.right.y, m11 = b.up.y, m12 = -b.forward.y;
    const double m20 = b.right.z, m21 = b.up.z, m22 = -b.forward.z;
    const double trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = {s / 4.0, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = {(m21 - m12) / s, s / 4.0, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = {(m02 - m20) / s, (m01 + m10) / s, s / 4.0, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, s / 4.0};
    }
    return Normalize(q);
}

Basis ToBasis(Quat q) {
    q = Normalize(q);
    Basis b;
    b.right = Rotate(q, {1.0, 0.0, 0.0});
    b.up = Rotate(q, {0.0, 1.0, 0.0});
    b.forward = Rotate(q, {0.0, 0.0, -1.0});
    return b;
}

SlotRows ToSlotRows(const Basis& b) {
    SlotRows r;
    r.right = b.right;
    r.up = b.up;
    r.back = b.forward * -1.0;
    return r;
}

Basis FromSlotRows(const SlotRows& rows) {
    Basis b;
    b.right = rows.right;
    b.up = rows.up;
    b.forward = rows.back * -1.0;
    return b;
}

Basis BasisFromYawPitch(double yaw_deg, double pitch_deg) {
    const double p = std::clamp(pitch_deg, -89.0, 89.0) * kDeg;
    const double y = yaw_deg * kDeg;
    const Vec3 f{std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
    Basis b;
    b.forward = f;
    b.right = Normalize(Cross(f, {0.0, 1.0, 0.0}), {-1.0, 0.0, 0.0});
    b.up = Cross(b.right, f);
    return b;
}

Basis BasisLookAt(Vec3 from, Vec3 to, const Basis& fallback) {
    const Vec3 d = to - from;
    if (Length(d) < 1e-6) return fallback;
    const Vec3 f = Normalize(d, fallback.forward);
    Vec3 r = Cross(f, {0.0, 1.0, 0.0});
    if (Length(r) < 1e-6) {
        // straight up or down: the roll of the fallback decides
        r = fallback.right - f * Dot(fallback.right, f);
        if (Length(r) < 1e-6) r = {-1.0, 0.0, 0.0};
    }
    Basis b;
    b.forward = f;
    b.right = Normalize(r, {-1.0, 0.0, 0.0});
    b.up = Cross(b.right, f);
    return b;
}

Basis WithRoll(const Basis& b, double roll_deg) {
    if (roll_deg == 0.0) return b;
    const double c = std::cos(roll_deg * kDeg);
    const double s = std::sin(roll_deg * kDeg);
    Basis out;
    out.forward = b.forward;
    out.right = b.right * c + b.up * s;
    out.up = b.up * c - b.right * s;
    return out;
}

double HeadingOf(Vec3 forward, double keep) {
    if (std::hypot(forward.x, forward.z) < 1e-4) return keep;
    return std::atan2(forward.x, forward.z);
}

Basis HeadingBasis(double heading_rad) {
    Basis b;
    b.forward = {std::sin(heading_rad), 0.0, std::cos(heading_rad)};
    b.right = {-std::cos(heading_rad), 0.0, std::sin(heading_rad)};  // forward x up
    b.up = {0.0, 1.0, 0.0};
    return b;
}

// ---------------------------------------------------------------------------------------------------------------- interpolation

double EaseCurve(Ease ease, double u) {
    return ApplyEase(ease, u);
}

double FocalFromFov(double fov_deg) {
    return 1.0 / std::tan(std::clamp(fov_deg, 1.0, 170.0) * kDeg * 0.5);
}

double FovFromFocal(double focal) {
    return 2.0 * std::atan(1.0 / focal) / kDeg;
}

double LerpFov(double a_deg, double b_deg, double u) {
    return FovFromFocal(Lerp(FocalFromFov(a_deg), FocalFromFov(b_deg), u));
}

Vec3 CatmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double u) {
    u = std::clamp(u, 0.0, 1.0);
    const double d12 = Length(p2 - p1);
    if (d12 < 1e-9) return Lerp(p1, p2, u);
    // A neighbour on top of its key gives no direction: reflect the other side.
    if (Length(p1 - p0) < 1e-9) p0 = p1 - (p2 - p1);
    if (Length(p3 - p2) < 1e-9) p3 = p2 + (p2 - p1);
    const double t0 = 0.0;
    const double t1 = t0 + std::max(std::sqrt(Length(p1 - p0)), 1e-6);
    const double t2 = t1 + std::max(std::sqrt(d12), 1e-6);
    const double t3 = t2 + std::max(std::sqrt(Length(p3 - p2)), 1e-6);
    const double t = t1 + u * (t2 - t1);
    const Vec3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const Vec3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const Vec3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const Vec3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const Vec3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

Vec3 OrbitPoint(Vec3 target, const Basis& hf, double radius, double az_deg, double el_deg) {
    const double az = az_deg * kDeg;
    const double el = el_deg * kDeg;
    const Vec3 flat = hf.forward * (-std::cos(az)) + hf.right * std::sin(az);
    return target + (flat * std::cos(el) + Vec3{0.0, 1.0, 0.0} * std::sin(el)) * radius;
}

// ---------------------------------------------------------------------------------------------------------------- the follow lag

double Damped::Update(double target, double dt, double smooth) {
    if (smooth <= 1e-6) {
        Snap(target);
        return x;
    }
    if (dt <= 0.0) return x;
    const double omega = 2.0 / smooth;
    const double c1 = x - target;
    const double c2 = v + omega * c1;
    const double e = std::exp(-omega * dt);
    x = target + (c1 + c2 * dt) * e;
    v = (c2 - omega * (c1 + c2 * dt)) * e;
    return x;
}

void Damped3::Snap(Vec3 at) {
    x.Snap(at.x);
    y.Snap(at.y);
    z.Snap(at.z);
}

Vec3 Damped3::Update(Vec3 target, double dt, double smooth) {
    return {x.Update(target.x, dt, smooth), y.Update(target.y, dt, smooth), z.Update(target.z, dt, smooth)};
}

double DampedAngle::Update(double target, double dt, double smooth) {
    const double d = std::remainder(target - a.x, 2.0 * kPi);  // -pi..pi: the short way round
    return a.Update(a.x + d, dt, smooth);
}

// ---------------------------------------------------------------------------------------------------------------- the track: parser

namespace {

constexpr double kEps = 1e-9;
constexpr const char* kCamWords = "pos, attach, orbit, yaw, pitch, look, roll, fov, near, far, follow, aimfollow, heading, ease or off";

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

bool IsIdentifier(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// "car:p1" -> "p1" ("car:Player" -> "player"); false when it is not a car reference.
bool ParseCarRef(std::string_view s, std::string* id) {
    if (s.size() < 5 || Lower(s.substr(0, 4)) != "car:") return false;
    const std::string_view name = s.substr(4);
    if (!IsIdentifier(name)) return false;
    *id = Lower(name) == "player" ? std::string("player") : std::string(name);
    return true;
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

class Builder {
  public:
    BuildResult Run(const Scene& scene) {
        for (const Event& e : scene.events) {
            if (e.track == Track::kCamera) Camera(e);
            else if (e.track == Track::kCut) Cut(e);
        }
        Finish();
        return std::move(r_);
    }

  private:
    void Error(int line, std::string message) { r_.diagnostics.push_back({line, Severity::kError, std::move(message)}); }
    void Warn(int line, std::string message) { r_.diagnostics.push_back({line, Severity::kWarning, std::move(message)}); }

    void NoteCar(const std::string& id) {
        auto& cars = r_.track.cars;
        if (std::find(cars.begin(), cars.end(), id) == cars.end()) cars.push_back(id);
    }

    // Reads the words of one `cam` statement.
    void Camera(const Event& e) {
        const std::vector<std::string>& w = e.words;
        const int line = e.line;
        if (w.size() == 1 && Lower(w[0]) == "off") {
            if (on_) {
                r_.track.gates.push_back({e.t, false});
                on_ = false;
            } else {
                Warn(line, "cam " + Num(e.t) + " off: the camera is not the scene's at that time (nothing to give back)");
            }
            return;
        }

        PosKey pk;
        AimKey ak;
        bool has_pos = false, has_yaw = false, has_pitch = false, has_look = false;
        bool has_roll = false, has_fov = false, has_near = false, has_far = false;
        bool has_follow = false, has_aimfollow = false, heading = false;
        bool has_r = false, has_az = false, has_el = false, has_at = false;
        double roll = 0.0, fov = 0.0, near_v = 0.0, far_v = 0.0, follow = 0.0, aimfollow = 0.0;
        Ease ease = Ease::kLinear;
        std::string pos_word;
        bool bad = false;

        const auto number = [&](size_t at, const char* word, double* v) {
            if (!w.empty() && at < w.size() && ParseNumber(w[at], v)) return true;
            Error(line, std::string("cam: '") + word + "' needs a number" + (at < w.size() ? " (got '" + w[at] + "')" : std::string()));
            bad = true;
            return false;
        };
        const auto numbers3 = [&](size_t at, const char* word, Vec3* v) {
            double a[3] = {};
            for (int k = 0; k < 3; ++k) {
                if (at + k >= w.size() || !ParseNumber(w[at + k], &a[k])) {
                    Error(line, std::string("cam: '") + word + "' needs three numbers (x y z)" +
                                    (at + k < w.size() ? " (got '" + w[at + k] + "')" : std::string()));
                    bad = true;
                    return false;
                }
            }
            *v = {a[0], a[1], a[2]};
            return true;
        };
        // An optional offset after a car reference: all three numbers, or none.
        const auto optional3 = [&](size_t at, const char* word, Vec3* v) {
            double probe = 0.0;
            if (at >= w.size() || !ParseNumber(w[at], &probe)) return size_t(0);
            return numbers3(at, word, v) ? size_t(3) : size_t(0);
        };

        size_t i = 0;
        while (i < w.size() && !bad) {
            const std::string key = Lower(w[i]);
            ++i;
            if (key == "pos") {
                if (has_pos) { Error(line, "cam: two positions in one key (" + pos_word + " and pos)"); bad = true; break; }
                has_pos = true;
                pos_word = "pos";
                pk.kind = PosKind::kWorld;
                if (!numbers3(i, "pos", &pk.point)) break;
                i += 3;
            } else if (key == "attach" || key == "orbit") {
                if (has_pos) { Error(line, "cam: two positions in one key (" + pos_word + " and " + key + ")"); bad = true; break; }
                has_pos = true;
                pos_word = key;
                pk.kind = key == "attach" ? PosKind::kAttach : PosKind::kOrbit;
                if (i >= w.size() || !ParseCarRef(w[i], &pk.car)) {
                    Error(line, "cam: '" + key + "' needs a car: " + key + " car:<id> (car:player is the player's)");
                    bad = true;
                    break;
                }
                ++i;
                NoteCar(pk.car);
                if (key == "attach") i += optional3(i, "attach", &pk.point);
            } else if (key == "r") {
                if (!number(i, "r", &pk.radius)) break;
                has_r = true;
                ++i;
            } else if (key == "az") {
                if (!number(i, "az", &pk.azimuth)) break;
                has_az = true;
                ++i;
            } else if (key == "el") {
                if (!number(i, "el", &pk.elevation)) break;
                has_el = true;
                ++i;
            } else if (key == "at") {
                Vec3 v;
                if (!numbers3(i, "at", &v)) break;
                has_at = true;
                at_ = v;
                i += 3;
            } else if (key == "heading") {
                heading = true;
            } else if (key == "follow") {
                if (!number(i, "follow", &follow)) break;
                has_follow = true;
                ++i;
            } else if (key == "aimfollow") {
                if (!number(i, "aimfollow", &aimfollow)) break;
                has_aimfollow = true;
                ++i;
            } else if (key == "yaw") {
                if (!number(i, "yaw", &ak.yaw)) break;
                has_yaw = true;
                ++i;
            } else if (key == "pitch") {
                if (!number(i, "pitch", &ak.pitch)) break;
                has_pitch = true;
                ++i;
            } else if (key == "look") {
                if (has_look) { Error(line, "cam: two 'look' in one key"); bad = true; break; }
                has_look = true;
                if (i < w.size() && ParseCarRef(w[i], &ak.car)) {
                    ak.kind = AimKind::kLookCar;
                    ++i;
                    NoteCar(ak.car);
                    i += optional3(i, "look", &ak.point);
                } else {
                    ak.kind = AimKind::kLookPoint;
                    if (!numbers3(i, "look", &ak.point)) break;
                    i += 3;
                }
            } else if (key == "roll") {
                if (!number(i, "roll", &roll)) break;
                has_roll = true;
                ++i;
            } else if (key == "fov") {
                if (!number(i, "fov", &fov)) break;
                has_fov = true;
                ++i;
            } else if (key == "near") {
                if (!number(i, "near", &near_v)) break;
                has_near = true;
                ++i;
            } else if (key == "far") {
                if (!number(i, "far", &far_v)) break;
                has_far = true;
                ++i;
            } else if (key == "ease") {
                if (i >= w.size() || !ParseEase(w[i], &ease)) {
                    Error(line, "cam: bad ease" + (i < w.size() ? " '" + w[i] + "'" : std::string()) + " (linear, in, out or inout)");
                    bad = true;
                    break;
                }
                ++i;
            } else if (key == "off") {
                Error(line, "cam <time> off takes nothing else");
                bad = true;
            } else {
                Error(line, "cam: unknown word '" + w[i - 1] + "' (" + kCamWords + ")");
                bad = true;
            }
        }
        if (bad) return;

        // ---- what the words mean together
        const bool orbit = has_pos && pk.kind == PosKind::kOrbit;
        const bool attach = has_pos && pk.kind == PosKind::kAttach;
        if ((has_r || has_az || has_el || has_at) && !orbit) {
            Error(line, "cam: r, az, el and at belong to 'orbit car:<id>'");
            return;
        }
        if (has_follow && !(attach || orbit)) {
            Error(line, "cam: 'follow' lags an attach or orbit key (aimfollow lags a look at a car)");
            return;
        }
        if (heading && !(attach || orbit)) {
            Error(line, "cam: 'heading' belongs to 'attach car:<id>'");
            return;
        }
        if ((has_yaw || has_pitch) && has_look) {
            Error(line, "cam: yaw/pitch and look are two ways to aim: use one");
            return;
        }
        if (has_aimfollow && !(has_look && ak.kind == AimKind::kLookCar) && !orbit) {
            Error(line, "cam: 'aimfollow' lags a 'look car:<id>' aim");
            return;
        }
        if (has_follow && (follow < 0.0 || follow > 10.0)) {
            Error(line, "cam: follow " + Num(follow) + " (0 to 10 seconds)");
            return;
        }
        if (has_aimfollow && (aimfollow < 0.0 || aimfollow > 10.0)) {
            Error(line, "cam: aimfollow " + Num(aimfollow) + " (0 to 10 seconds)");
            return;
        }
        if (orbit) {
            if (pk.radius <= 0.0 || pk.radius > 5000.0) {
                Error(line, "cam: orbit r " + Num(pk.radius) + " (more than 0 and at most 5000 metres)");
                return;
            }
            if (pk.elevation < -89.0 || pk.elevation > 89.0) {
                Error(line, "cam: orbit el " + Num(pk.elevation) + " (-89 to 89 degrees)");
                return;
            }
        }
        if (has_fov && !(fov >= 1.0 && fov <= 170.0)) {
            Error(line, "cam: fov " + Num(fov) + " (1 to 170 degrees, vertical)");
            return;
        }
        if (has_near && !(near_v > 0.0 && near_v <= 1000.0)) {
            Error(line, "cam: near " + Num(near_v) + " (more than 0, at most 1000 metres)");
            return;
        }
        if (has_far && !(far_v > 0.0 && far_v <= 100000.0)) {
            Error(line, "cam: far " + Num(far_v) + " (more than 0, at most 100000 metres)");
            return;
        }
        if (has_near && has_far && far_v <= near_v) {
            Error(line, "cam: far " + Num(far_v) + " is not beyond near " + Num(near_v));
            return;
        }
        if ((has_yaw || has_pitch) && (std::fabs(ak.pitch) > 89.0)) {
            Warn(line, "cam: pitch " + Num(ak.pitch) + " is clamped to 89 degrees (looking straight up or down has no roll)");
        }

        const bool has_aim = has_yaw || has_pitch || has_look;
        const bool names_something = has_pos || has_aim || has_roll || has_fov || has_near || has_far || orbit;
        if (!names_something) {
            Error(line, "cam <time> <what the camera does>: nothing it can do in '" + (w.empty() ? std::string() : w[0]) + "' (" + kCamWords + ")");
            return;
        }

        // ---- the keys
        if (!on_) {
            r_.track.gates.push_back({e.t, true});
            on_ = true;
            if (first_cam_line_ == 0) first_cam_line_ = line;
        }
        if (has_pos) {
            pk.t = e.t;
            pk.line = line;
            pk.ease = ease;
            pk.follow = has_follow ? follow : 0.0;
            pk.heading_only = heading;
            if (orbit) pk.point = has_at ? at_ : Vec3{};
            if (has_pos && pk.kind == PosKind::kWorld) pk.car.clear();
            r_.track.pos.push_back(pk);
        }
        AimKey aim_key = ak;
        bool add_aim = false;
        if (has_yaw || has_pitch) {
            aim_key.kind = AimKind::kAngles;
            add_aim = true;
        } else if (has_look) {
            aim_key.follow = has_aimfollow ? aimfollow : 0.0;
            add_aim = true;
        } else if (orbit) {
            // an orbit looks at what it circles unless the key says otherwise
            aim_key = AimKey{};
            aim_key.kind = AimKind::kLookCar;
            aim_key.car = pk.car;
            aim_key.point = pk.point;
            aim_key.follow = has_aimfollow ? aimfollow : (has_follow ? follow : 0.0);
            add_aim = true;
        }
        if (add_aim) {
            aim_key.t = e.t;
            aim_key.line = line;
            aim_key.ease = ease;
            r_.track.aim.push_back(aim_key);
        }
        const auto scalar = [&](std::vector<ScalarKey>& v, double value) {
            ScalarKey k;
            k.t = e.t;
            k.line = line;
            k.ease = ease;
            k.value = value;
            v.push_back(k);
        };
        if (has_roll) scalar(r_.track.roll, roll);
        if (has_fov) scalar(r_.track.fov, fov);
        if (has_near) scalar(r_.track.near_plane, near_v);
        if (has_far) scalar(r_.track.far_plane, far_v);
        if (e.t < last_cam_t_ - kEps) {
            Warn(line, "the camera key at t=" + Num(e.t) + " comes after one at t=" + Num(last_cam_t_) + ": keys run in time order");
        }
        last_cam_t_ = std::max(last_cam_t_, e.t);
    }

    void Cut(const Event& e) {
        r_.track.cuts.push_back(e.t);
    }

    void Finish() {
        CameraTrack& tr = r_.track;
        if (tr.gates.empty()) {
            if (!tr.cuts.empty()) {
                Warn(0, "'cut' without any camera key: it only starts the renderer's histories again (nothing in the scene moves the camera)");
            }
            return;
        }
        std::vector<double> shots = tr.cuts;
        for (const Gate& g : tr.gates) {
            if (g.on) shots.push_back(g.t);
        }
        std::sort(shots.begin(), shots.end());
        shots.erase(std::unique(shots.begin(), shots.end(), [](double a, double b) { return std::fabs(a - b) < kEps; }), shots.end());
        tr.shots = std::move(shots);
        std::sort(tr.cuts.begin(), tr.cuts.end());

        const auto assign = [&](auto& keys) {
            for (auto& k : keys) k.shot = tr.ShotAt(k.t);
        };
        assign(tr.pos);
        assign(tr.aim);
        assign(tr.roll);
        assign(tr.fov);
        assign(tr.near_plane);
        assign(tr.far_plane);

        // The first take-over has to give the camera a place and an aim.
        const double t0 = tr.gates.front().t;
        const bool pos0 = std::any_of(tr.pos.begin(), tr.pos.end(), [&](const PosKey& k) { return k.t <= t0 + kEps; });
        const bool aim0 = std::any_of(tr.aim.begin(), tr.aim.end(), [&](const AimKey& k) { return k.t <= t0 + kEps; });
        if (!pos0 || !aim0) {
            Error(first_cam_line_, std::string("the camera takes over at t=") + Num(t0) + " with no " +
                                       (!pos0 && !aim0 ? "position and no aim" : (!pos0 ? "position" : "aim")) +
                                       ": the first key needs pos (or attach, or orbit) and yaw/pitch (or look)");
        }
        // A cut nothing follows.
        for (const double c : tr.cuts) {
            const bool key_at_or_after = std::any_of(tr.pos.begin(), tr.pos.end(), [&](const PosKey& k) { return k.t >= c - kEps; }) ||
                                         std::any_of(tr.aim.begin(), tr.aim.end(), [&](const AimKey& k) { return k.t >= c - kEps; });
            if (!key_at_or_after) {
                Warn(0, "'cut " + Num(c) + "' has no camera key at or after it: the camera carries on from where it was");
            }
        }
    }

    BuildResult r_;
    bool on_ = false;
    int first_cam_line_ = 0;
    double last_cam_t_ = -1e18;
    Vec3 at_;
};

}  // namespace

int BuildResult::ErrorCount() const {
    int n = 0;
    for (const Diagnostic& d : diagnostics) n += d.severity == Severity::kError;
    return n;
}

BuildResult BuildTrack(const Scene& scene) {
    Builder b;
    return b.Run(scene);
}

std::string Summarize(const CameraTrack& track) {
    if (track.Empty()) return "camera: no keys";
    size_t attach = 0, orbit = 0;
    for (const PosKey& k : track.pos) {
        attach += k.kind == PosKind::kAttach;
        orbit += k.kind == PosKind::kOrbit;
    }
    std::string s = "camera: " + std::to_string(track.pos.size()) + " position keys";
    if (attach || orbit) s += " (" + std::to_string(attach) + " attach, " + std::to_string(orbit) + " orbit)";
    s += ", " + std::to_string(track.aim.size()) + " aim keys, fov " + std::to_string(track.fov.size()) + ", roll " +
         std::to_string(track.roll.size()) + ", near/far " + std::to_string(track.near_plane.size() + track.far_plane.size());
    s += "; " + std::to_string(track.cuts.size()) + " cut" + (track.cuts.size() == 1 ? "" : "s");
    s += "; takes over at " + Num(track.gates.front().t) + " s";
    if (track.gates.size() > 1) s += " (" + std::to_string(track.gates.size()) + " take-overs and hand-backs)";
    if (!track.cars.empty()) {
        s += "; cars:";
        for (const std::string& c : track.cars) s += " " + c;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- the track: queries

int CameraTrack::ShotAt(double t) const {
    int n = 0;
    for (const double s : shots) {
        if (s <= t + kEps) ++n;
        else break;
    }
    return n;
}

double CameraTrack::ShotStart(int shot) const {
    if (shot <= 0 || static_cast<size_t>(shot) > shots.size()) return -std::numeric_limits<double>::infinity();
    return shots[static_cast<size_t>(shot) - 1];
}

bool CameraTrack::OnAt(double t) const {
    bool on = false;
    for (const Gate& g : gates) {
        if (g.t <= t + kEps) on = g.on;
        else break;
    }
    return on;
}

// ---------------------------------------------------------------------------------------------------------------- the rig

namespace {

// The keys of one channel that a shot uses, in time order. A channel whose first key in the shot comes after the shot's start (or
// has none in it) carries on from the last key before: a copy of it at the start, with a linear segment.
template <class K>
struct ShotKeys {
    K synthetic{};
    std::vector<const K*> keys;
};

template <class K>
void Gather(const std::vector<K>& all, int shot, double shot_start, ShotKeys<K>* out) {
    out->keys.clear();
    size_t lo = 0;
    while (lo < all.size() && all[lo].shot < shot) ++lo;
    size_t hi = lo;
    while (hi < all.size() && all[hi].shot == shot) ++hi;
    if (lo > 0 && (lo == hi || all[lo].t > shot_start + kEps)) {
        out->synthetic = all[lo - 1];
        out->synthetic.t = shot_start;
        out->synthetic.ease = Ease::kLinear;
        out->synthetic.shot = shot;
        out->keys.push_back(&out->synthetic);
    }
    for (size_t i = lo; i < hi; ++i) out->keys.push_back(&all[i]);
}

// The segment of `keys` that holds t: index of its first key, and the eased position in it; false when t is before the first
// key or at or after the last (then the first or last holds).
template <class P>
bool Segment(const std::vector<const P*>& keys, double t, size_t* i, double* u) {
    const size_t n = keys.size();
    if (n < 2 || t <= keys.front()->t || t >= keys.back()->t) return false;
    size_t k = 0;
    while (k + 2 < n && keys[k + 1]->t <= t) ++k;
    const double span = keys[k + 1]->t - keys[k]->t;
    if (!(span > 0.0)) return false;
    *i = k;
    *u = EaseCurve(keys[k]->ease, (t - keys[k]->t) / span);
    return true;
}

double ScalarAt(const std::vector<const ScalarKey*>& keys, double t, bool focal, double unset) {
    if (keys.empty()) return unset;
    size_t i = 0;
    double u = 0.0;
    if (!Segment(keys, t, &i, &u)) return t <= keys.front()->t ? keys.front()->value : keys.back()->value;
    const double a = keys[i]->value;
    const double b = keys[i + 1]->value;
    return focal ? LerpFov(a, b, u) : Lerp(a, b, u);
}

Basis SaneFrame(const Basis& f) {
    if (Length(f.forward) < 0.5 || Length(f.right) < 0.5 || Length(f.up) < 0.5) return Basis{};
    return f;
}

}  // namespace

void Rig::Reset() {
    have_last_ = false;
    last_t_ = 0.0;
    was_active_ = false;
    frame_ = 0;
    dt_ = 0.0;
    springs_.clear();
    slots_.clear();
    missing_.clear();
}

Rig::CarSlot& Rig::Slot(const std::string& car, CarSource& cars) {
    auto it = std::find_if(slots_.begin(), slots_.end(), [&](const CarSlot& s) { return s.car == car; });
    if (it == slots_.end()) {
        slots_.push_back({});
        slots_.back().car = car;
        it = slots_.end() - 1;
    }
    if (!it->fresh) {
        it->fresh = true;
        CarPose p;
        if (cars.Get(car, &p)) {
            p.frame = SaneFrame(p.frame);
            it->pose = p;
            it->have = true;
        } else {
            ++missing_this_frame_;
            if (std::find(missing_.begin(), missing_.end(), car) == missing_.end()) missing_.push_back(car);
        }
    }
    return *it;
}

Rig::Followed Rig::Follow(const std::string& car, double smooth, CarSource& cars) {
    const CarSlot& slot = Slot(car, cars);
    Followed f;
    f.valid = slot.have;
    const CarPose p = slot.pose;  // default: the origin, level
    f.frame = p.frame;
    if (smooth <= 1e-6) {
        f.position = p.position;
        f.heading = HeadingOf(p.frame.forward, 0.0);
        return f;
    }
    auto it = std::find_if(springs_.begin(), springs_.end(), [&](const Spring& s) { return s.car == car && s.smooth == smooth; });
    if (it == springs_.end()) {
        springs_.push_back({});
        springs_.back().car = car;
        springs_.back().smooth = smooth;
        it = springs_.end() - 1;
    }
    Spring& s = *it;
    if (s.frame != frame_) {
        const double raw = HeadingOf(p.frame.forward, s.heading.Value());
        if (!s.init || s.frame + 1 != frame_) {  // new, restarted, or unused for a while: start on the car
            s.position.Snap(p.position);
            s.heading.Snap(raw);
            s.init = true;
        } else {
            s.position.Update(p.position, dt_, smooth);
            s.heading.Update(raw, dt_, smooth);
        }
        s.frame = frame_;
    }
    f.position = s.position.Value();
    f.heading = s.heading.Value();
    return f;
}

Vec3 Rig::KeyPosition(const PosKey& k, CarSource& cars) {
    switch (k.kind) {
        case PosKind::kWorld:
            return k.point;
        case PosKind::kAttach: {
            const Followed f = Follow(k.car, k.follow, cars);
            const Basis frame = k.heading_only ? HeadingBasis(f.heading) : f.frame;
            return f.position + frame.right * k.point.x + frame.up * k.point.y + frame.forward * k.point.z;
        }
        case PosKind::kOrbit: {
            const Followed f = Follow(k.car, k.follow, cars);
            const Basis hf = HeadingBasis(f.heading);
            const Vec3 target = f.position + hf.right * k.point.x + hf.up * k.point.y + hf.forward * k.point.z;
            return OrbitPoint(target, hf, k.radius, k.azimuth, k.elevation);
        }
    }
    return k.point;
}

Vec3 Rig::EvalPosition(const std::vector<const PosKey*>& k, double t, CarSource& cars) {
    if (k.empty()) return {};
    size_t i = 0;
    double u = 0.0;
    if (!Segment(k, t, &i, &u)) return KeyPosition(t <= k.front()->t ? *k.front() : *k.back(), cars);
    const PosKey& a = *k[i];
    const PosKey& b = *k[i + 1];
    if (a.kind == PosKind::kOrbit && b.kind == PosKind::kOrbit && a.car == b.car) {
        // an arc around the car, not a chord through it
        const Followed f = Follow(a.car, a.follow, cars);
        const Basis hf = HeadingBasis(f.heading);
        const Vec3 at = Lerp(a.point, b.point, u);
        const Vec3 target = f.position + hf.right * at.x + hf.up * at.y + hf.forward * at.z;
        return OrbitPoint(target, hf, Lerp(a.radius, b.radius, u), Lerp(a.azimuth, b.azimuth, u), Lerp(a.elevation, b.elevation, u));
    }
    const Vec3 p1 = KeyPosition(a, cars);
    const Vec3 p2 = KeyPosition(b, cars);
    const Vec3 p0 = i > 0 ? KeyPosition(*k[i - 1], cars) : p1 - (p2 - p1);
    const Vec3 p3 = i + 2 < k.size() ? KeyPosition(*k[i + 2], cars) : p2 + (p2 - p1);
    return CatmullRom(p0, p1, p2, p3, u);
}

Basis Rig::KeyAim(const AimKey& k, Vec3 camera, CarSource& cars) {
    switch (k.kind) {
        case AimKind::kAngles:
            return BasisFromYawPitch(k.yaw, k.pitch);
        case AimKind::kLookPoint:
            return BasisLookAt(camera, k.point, Basis{});
        case AimKind::kLookCar: {
            const Followed f = Follow(k.car, k.follow, cars);
            const Basis hf = HeadingBasis(f.heading);
            const Vec3 target = f.position + hf.right * k.point.x + hf.up * k.point.y + hf.forward * k.point.z;
            return BasisLookAt(camera, target, hf);
        }
    }
    return Basis{};
}

Basis Rig::EvalAim(const std::vector<const AimKey*>& k, double t, Vec3 camera, CarSource& cars) {
    if (k.empty()) return Basis{};
    size_t i = 0;
    double u = 0.0;
    if (!Segment(k, t, &i, &u)) return KeyAim(t <= k.front()->t ? *k.front() : *k.back(), camera, cars);
    const Quat qa = ToQuat(KeyAim(*k[i], camera, cars));
    const Quat qb = ToQuat(KeyAim(*k[i + 1], camera, cars));
    return ToBasis(Slerp(qa, qb, u));
}

View Rig::Evaluate(const CameraTrack& track, double t, CarSource& cars) {
    View v;
    ++frame_;
    missing_this_frame_ = 0;
    for (CarSlot& s : slots_) s.fresh = false;

    // A step of the clock, or a jump (a restart, a scrub): the springs start again on the jump.
    const bool jumped = have_last_ && (t < last_t_ - kEps || t - last_t_ > 0.5);
    dt_ = (have_last_ && !jumped) ? t - last_t_ : 0.0;
    if (!have_last_ || jumped) {
        for (Spring& s : springs_) s.init = false;
    }
    bool crossed = false;
    if (have_last_ && !jumped) {
        for (const double s : track.shots) {
            if (s > last_t_ + kEps && s <= t + kEps) crossed = true;
        }
    }
    have_last_ = true;
    last_t_ = t;

    const int shot = track.ShotAt(t);
    v.shot = shot;
    if (!track.OnAt(t)) {
        v.handed_back = was_active_;
        v.cut = was_active_;
        was_active_ = false;
        return v;
    }
    v.active = true;
    v.took_over = !was_active_;
    v.cut = v.took_over || crossed || jumped;
    was_active_ = true;

    const double shot_start = track.ShotStart(shot);
    ShotKeys<PosKey> pk;
    Gather(track.pos, shot, shot_start, &pk);
    ShotKeys<AimKey> ak;
    Gather(track.aim, shot, shot_start, &ak);
    ShotKeys<ScalarKey> rk, fk, nk, xk;
    Gather(track.roll, shot, shot_start, &rk);
    Gather(track.fov, shot, shot_start, &fk);
    Gather(track.near_plane, shot, shot_start, &nk);
    Gather(track.far_plane, shot, shot_start, &xk);

    v.position = EvalPosition(pk.keys, t, cars);
    const Basis aim = EvalAim(ak.keys, t, v.position, cars);
    v.basis = WithRoll(aim, ScalarAt(rk.keys, t, false, 0.0));
    v.fov = ScalarAt(fk.keys, t, true, 0.0);
    v.near_plane = ScalarAt(nk.keys, t, false, 0.0);
    v.far_plane = ScalarAt(xk.keys, t, false, 0.0);
    v.missing_cars = missing_this_frame_;
    return v;
}

}  // namespace mc::cinematic::camera
