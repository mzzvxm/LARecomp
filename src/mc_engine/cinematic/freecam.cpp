//
// The free camera's flight and the raw track file: see freecam.h for the controls and the file (no game, no log in here).
//

#include "freecam.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace mc::cinematic::freecam {

namespace {

using cam::kDeg;
using cam::Vec3;

double Clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

int16_t Be16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>((p[0] << 8) | p[1]));
}

double Axis(const uint8_t* p) {
    return std::clamp(static_cast<double>(Be16(p)) / 32767.0, -1.0, 1.0);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- the pad

Pad PadFromGuest(const uint8_t* s) {
    Pad pad;
    pad.buttons = static_cast<uint16_t>((s[4] << 8) | s[5]);
    pad.lt = s[6] / 255.0;
    pad.rt = s[7] / 255.0;
    pad.lx = Axis(s + 8);
    pad.ly = Axis(s + 10);
    pad.rx = Axis(s + 12);
    pad.ry = Axis(s + 14);
    return pad;
}

void ClearGuestPad(uint8_t* s) {
    const uint16_t held = static_cast<uint16_t>((s[4] << 8) | s[5]);
    const uint16_t buttons = (held & kBtnBack) ? uint16_t(0) : static_cast<uint16_t>(held & kBtnStart);
    s[4] = static_cast<uint8_t>(buttons >> 8);
    s[5] = static_cast<uint8_t>(buttons);
    s[6] = 0;
    s[7] = 0;
    for (int i = 8; i < 16; ++i) s[i] = 0;
}

void StickResponse(double x, double y, double zone, double* out_x, double* out_y) {
    zone = std::clamp(zone, 0.0, 0.9);
    const double len = std::hypot(x, y);
    if (len <= zone || len < 1e-12) {
        *out_x = 0.0;
        *out_y = 0.0;
        return;
    }
    const double s = std::pow(std::min(1.0, (len - zone) / (1.0 - zone)), kStickExponent);
    *out_x = x / len * s;
    *out_y = y / len * s;
}

double TriggerResponse(double v, double zone) {
    zone = std::clamp(zone, 0.0, 0.9);
    if (v <= zone) return 0.0;
    return Clamp01((v - zone) / (1.0 - zone));
}

// ---------------------------------------------------------------------------------------------------------------- angles

cam::Basis BasisOf(const Pose& pose) {
    return cam::WithRoll(cam::BasisFromYawPitch(pose.yaw, pose.pitch), pose.roll);
}

void AnglesFromBasis(const cam::Basis& basis, double keep_yaw, double* yaw, double* pitch, double* roll) {
    const Vec3 f = cam::Normalize(basis.forward, {0.0, 0.0, 1.0});
    *pitch = std::asin(std::clamp(f.y, -1.0, 1.0)) / kDeg;
    *yaw = std::hypot(f.x, f.z) < 1e-6 ? keep_yaw : std::atan2(f.x, f.z) / kDeg;
    // WithRoll: right = level right * cos(roll) + level up * sin(roll)
    const cam::Basis level = cam::BasisFromYawPitch(*yaw, *pitch);
    const Vec3 r = cam::Normalize(basis.right, level.right);
    *roll = std::atan2(cam::Dot(r, level.up), cam::Dot(r, level.right)) / kDeg;
}

Pose PoseFromBasis(Vec3 position, const cam::Basis& basis, double fov, double keep_yaw) {
    Pose p;
    p.position = position;
    p.fov = fov;
    AnglesFromBasis(basis, keep_yaw, &p.yaw, &p.pitch, &p.roll);
    return p;
}

cam::SlotRows SlotRowsOf(const Pose& pose) {
    return cam::ToSlotRows(BasisOf(pose));
}

Pose PoseFromSlotRows(Vec3 position, const cam::SlotRows& rows, double fov, double keep_yaw) {
    return PoseFromBasis(position, cam::FromSlotRows(rows), fov, keep_yaw);
}

double AngleBetween(Vec3 a, Vec3 b) {
    return std::acos(std::clamp(cam::Dot(cam::Normalize(a, {0.0, 0.0, 1.0}), cam::Normalize(b, {0.0, 0.0, 1.0})), -1.0, 1.0)) / kDeg;
}

double WrapDegrees(double d) {
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

// ---------------------------------------------------------------------------------------------------------------- the flight

void Flyer::Seed(const Pose& pose) {
    pose_ = pose;
    pose_.pitch = std::clamp(pose_.pitch, -kMaxPitch, kMaxPitch);
    home_fov_ = pose.fov;
    strafe_ = forward_ = rise_ = 0.0;
    yaw_rate_ = pitch_rate_ = roll_rate_ = zoom_ = 0.0;
}

Events Flyer::Step(const Pad& pad, double dt, const Params& p) {
    Events ev;
    dt = std::clamp(dt, 0.0, 0.25);

    // buttons that act once per press; with Back held the face buttons are the recorder's chords instead
    const uint16_t pressed = static_cast<uint16_t>(pad.buttons & ~prev_buttons_);
    prev_buttons_ = pad.buttons;
    const bool back = (pad.buttons & kBtnBack) != 0;
    if (back) {
        ev.toggle_record = (pressed & kBtnA) != 0;
        ev.home = (pressed & kBtnX) != 0;
        ev.mark_cut = (pressed & kBtnY) != 0;
        ev.add_key = (pressed & kBtnB) != 0;
        ev.next_key = (pressed & kBtnRb) != 0;
        ev.prev_key = (pressed & kBtnLb) != 0;
        ev.play_pause = (pressed & kBtnStart) != 0;
        ev.step_frame = (pressed & kBtnDpadRight) != 0;
        ev.render_take = (pressed & kBtnDpadUp) != 0;
    } else {
        if (pressed & kBtnY) {
            ev.reset_view = true;
            pose_.roll = 0.0;
            pose_.fov = home_fov_;
            roll_rate_ = 0.0;
            zoom_ = 0.0;
        }
        if (pressed & kBtnDpadLeft) speed_mult_ = std::max(kMinSpeedMult, speed_mult_ / kSpeedStep);
        if (pressed & kBtnDpadRight) speed_mult_ = std::min(kMaxSpeedMult, speed_mult_ * kSpeedStep);
    }

    // the wheel of the panel: steps of the speed, as the D-pad's (one at a time at most: a step is x1.25)
    for (int n = std::clamp(pad.speed_steps, -40, 40); n != 0; n += (n > 0 ? -1 : 1)) {
        speed_mult_ = n > 0 ? std::min(kMaxSpeedMult, speed_mult_ * kSpeedStep) : std::max(kMinSpeedMult, speed_mult_ / kSpeedStep);
    }

    // what the controls ask for now
    double strafe = 0.0, forward = 0.0, yaw_in = 0.0, pitch_in = 0.0;
    StickResponse(pad.lx, pad.ly, p.deadzone, &strafe, &forward);
    StickResponse(pad.rx, pad.ry, p.deadzone, &yaw_in, &pitch_in);
    const double rise = TriggerResponse(pad.rt) - TriggerResponse(pad.lt);
    // LB tilts left: + lifts the right side (not while Back is held: then they are the chords that go to the previous / next key)
    const double roll_in = back ? 0.0 : ((pad.buttons & kBtnLb) ? 1.0 : 0.0) - ((pad.buttons & kBtnRb) ? 1.0 : 0.0);
    const double zoom_in = ((pad.buttons & kBtnDpadUp) ? 1.0 : 0.0) - ((pad.buttons & kBtnDpadDown) ? 1.0 : 0.0);

    // The smoothed inputs and their mean over this update. A one-pole smoothing has the exact mean s = target + (before - target) * (tau / dt)
    // * (1 - e^(-dt / tau)): integrating that, a flight covers the same distance at 30, 60 and 240 updates a second.
    const auto smooth = [dt](double* state, double target, double tau) {
        const double before = *state;
        if (tau <= 1e-6) {
            *state = target;
            return target;
        }
        if (dt <= 0.0) return before;
        const double decay = std::exp(-dt / tau);
        *state = target + (before - target) * decay;
        return target + (before - target) * (tau / dt) * (1.0 - decay);
    };
    const double strafe_mean = smooth(&strafe_, strafe, p.inertia);
    const double forward_mean = smooth(&forward_, forward, p.inertia);
    const double rise_mean = smooth(&rise_, rise, p.inertia);
    const double yaw_mean = smooth(&yaw_rate_, yaw_in * p.look, p.inertia * 0.5);
    const double pitch_mean = smooth(&pitch_rate_, pitch_in * p.look, p.inertia * 0.5);
    const double roll_mean = smooth(&roll_rate_, roll_in * p.roll_rate, p.inertia * 0.5);
    const double zoom_mean = smooth(&zoom_, zoom_in, p.inertia * 0.5);

    // look, roll, zoom. The game's world is right-handed (x east, z south) and a growing yaw goes from +Z to +X, which is a turn to the LEFT: the stick to the right
    // (yaw_in > 0) lowers the yaw.
    pose_.yaw -= yaw_mean * dt + pad.look_right;  // the mouse's look (the panel): degrees, applied as they are
    pose_.pitch = std::clamp(pose_.pitch + pitch_mean * dt + pad.look_up, -kMaxPitch, kMaxPitch);
    pose_.roll += roll_mean * dt;
    if (zoom_mean != 0.0) {
        const double focal = cam::FocalFromFov(pose_.fov) * std::exp(p.zoom_rate * zoom_mean * dt);
        pose_.fov = std::clamp(cam::FovFromFocal(focal), p.min_fov, p.max_fov);
    }

    // move: forward along the view (pitch included), sideways level (the camera's own right, forward x up: -X at yaw 0, +Z at yaw 90), up along Y
    const double modifier = ((pad.buttons & kBtnL3) ? p.fast : 1.0) * ((pad.buttons & kBtnR3) ? p.slow : 1.0);
    const double step = p.speed * speed_mult_ * modifier * dt;
    const double yaw = pose_.yaw * kDeg, pitch = pose_.pitch * kDeg;
    const Vec3 fwd{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const Vec3 right = cam::HeadingBasis(yaw).right;
    pose_.position = pose_.position + fwd * (forward_mean * step) + right * (strafe_mean * step) + Vec3{0.0, rise_mean * step, 0.0};
    return ev;
}

// ---------------------------------------------------------------------------------------------------------------- the raw track

std::string RawHeader(const std::vector<std::string>& notes) {
    std::string out = kRawMagic;
    out += '\n';
    for (const std::string& n : notes) {
        out += "# ";
        out += n;
        out += '\n';
    }
    out += kRawColumns;
    out += '\n';
    return out;
}

std::string RawRow(const Sample& s) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf), "%.6f,%.6f,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u\n", s.t, s.scene_t,
                                static_cast<unsigned>(s.frame), s.pose.position.x, s.pose.position.y, s.pose.position.z, s.pose.yaw,
                                s.pose.pitch, s.pose.roll, s.pose.fov, static_cast<unsigned>(s.flags));
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

namespace {

bool ParseDouble(std::string_view s, double* out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto r = std::from_chars(first, last, *out);
    return r.ec == std::errc() && r.ptr == last && std::isfinite(*out);
}

}  // namespace

bool ParseRawTrack(std::string_view text, RawTrack* out, std::string* error) {
    out->notes.clear();
    out->samples.clear();
    int line_no = 0;
    bool magic = false, header = false;
    size_t pos = 0;
    const auto fail = [&](const std::string& message) {
        if (error) *error = "line " + std::to_string(line_no) + ": " + message;
        return false;
    };
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) {
            if (eol >= text.size()) break;
            continue;
        }
        if (line_no == 1) {
            if (line != kRawMagic) return fail(std::string("not a flight file (the first line is '") + std::string(line) + "', not '" + kRawMagic + "')");
            magic = true;
            continue;
        }
        if (line.front() == '#') {
            if (!header) {
                std::string_view note = line.substr(1);
                if (!note.empty() && note.front() == ' ') note.remove_prefix(1);
                out->notes.emplace_back(note);
            }
            continue;
        }
        if (!header) {
            if (line != kRawColumns) return fail(std::string("the header row is '") + std::string(line) + "', not '" + kRawColumns + "'");
            header = true;
            continue;
        }
        double v[11] = {};
        size_t at = 0;
        for (int i = 0; i < 11; ++i) {
            const size_t comma = line.find(',', at);
            const bool last = i == 10;
            if ((comma == std::string_view::npos) != last) return fail("a sample has " + std::string(last ? "more" : "fewer") + " than 11 columns");
            const std::string_view cell = line.substr(at, last ? std::string_view::npos : comma - at);
            if (!ParseDouble(cell, &v[i])) return fail("column " + std::to_string(i + 1) + " is not a number ('" + std::string(cell) + "')");
            at = comma + 1;
        }
        Sample s;
        s.t = v[0];
        s.scene_t = v[1];
        s.frame = static_cast<uint32_t>(v[2]);
        s.pose.position = {v[3], v[4], v[5]};
        s.pose.yaw = v[6];
        s.pose.pitch = v[7];
        s.pose.roll = v[8];
        s.pose.fov = v[9];
        s.flags = static_cast<uint32_t>(v[10]);
        if (!out->samples.empty() && s.t < out->samples.back().t) return fail("time goes backwards (" + std::to_string(s.t) + " after " + std::to_string(out->samples.back().t) + ")");
        out->samples.push_back(s);
    }
    if (!magic) {
        line_no = 0;
        return fail("empty file");
    }
    if (!header) return fail("no header row");
    return true;
}

Stats Summarize(const RawTrack& track) {
    Stats st;
    st.samples = track.samples.size();
    if (track.samples.empty()) return st;
    st.duration = track.samples.back().t - track.samples.front().t;
    for (size_t i = 0; i < track.samples.size(); ++i) {
        const Sample& s = track.samples[i];
        if (s.flags & kFlagCut) ++st.cuts;
        if (i == 0) continue;
        const Sample& a = track.samples[i - 1];
        if (s.flags & kFlagCut) continue;  // the jump of a cut is not flown
        const double d = cam::Length(s.pose.position - a.pose.position);
        st.distance += d;
        const double dt = s.t - a.t;
        if (dt > 1e-9) st.max_speed = std::max(st.max_speed, d / dt);
    }
    return st;
}

}  // namespace mc::cinematic::freecam
