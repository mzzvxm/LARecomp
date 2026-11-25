// The Director panel's timeline, the pure half: see director_timeline.h.

#include "director_timeline.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "director_text.h"

namespace mcla::ui::director {

const char* LaneKindName(LaneKind kind) {
    switch (kind) {
        case LaneKind::kCamera: return "CAMERA";
        case LaneKind::kCar: return "CAR";
        case LaneKind::kWorld: return "WORLD";
        case LaneKind::kHud: return "HUD";
        case LaneKind::kCapture: return "CAPTURE";
        case LaneKind::kMarks: return "MARKS";
        case LaneKind::kOther: return "OTHER";
    }
    return "?";
}

namespace {

LaneKind LaneOf(dir::EventKind kind) {
    switch (kind) {
        case dir::EventKind::kCamera:
        case dir::EventKind::kCut: return LaneKind::kCamera;
        case dir::EventKind::kCar: return LaneKind::kCar;
        case dir::EventKind::kWorld:
        case dir::EventKind::kSetting: return LaneKind::kWorld;
        case dir::EventKind::kHud: return LaneKind::kHud;
        case dir::EventKind::kCapture: return LaneKind::kCapture;
        case dir::EventKind::kSplit:
        case dir::EventKind::kLabel:
        case dir::EventKind::kMark: return LaneKind::kMarks;
        case dir::EventKind::kOther: return LaneKind::kOther;
    }
    return LaneKind::kOther;
}

// "car:p1" -> "p1"; anything else as it is.
std::string CarOf(const std::string& word) {
    return word.rfind("car:", 0) == 0 ? word.substr(4) : word;
}

std::string FirstCarWord(const std::vector<std::string>& w) {
    for (const std::string& s : w) {
        if (s.rfind("car:", 0) == 0 && s.size() > 4) return s.substr(4);
    }
    return {};
}

}  // namespace

std::string DescribeStatement(const dir::SnapEvent& e) {
    const std::vector<std::string> w = Words(e.text);
    if (w.empty()) return {};
    switch (e.kind) {
        case dir::EventKind::kCut: return "cut";
        case dir::EventKind::kCamera: {
            if (FindWord(w, "off") >= 0) return "off";
            const int rig = FindWord(w, "rig");
            if (rig >= 0 && static_cast<size_t>(rig) + 1 < w.size()) {
                std::string s = "rig " + w[static_cast<size_t>(rig) + 1];
                const std::string car = FirstCarWord(w);
                if (!car.empty()) s += " " + car;
                return s;
            }
            std::string s;
            const auto add = [&](const std::string& part) {
                if (!s.empty()) s += " ";
                s += part;
            };
            if (FindWord(w, "attach") >= 0) add("attach " + CarOf(ValueAfter(w, "attach")));
            else if (FindWord(w, "orbit") >= 0) add("orbit " + CarOf(ValueAfter(w, "orbit")));
            else if (FindWord(w, "pos") >= 0) add("pos");
            if (FindWord(w, "look") >= 0) add("look");
            const std::string fov = ValueAfter(w, "fov");
            if (!fov.empty()) add("fov " + fov);
            return s.empty() ? "cam" : s;
        }
        case dir::EventKind::kCar: {
            const int c = FindWord(w, "car", 0);
            if (c < 0 || static_cast<size_t>(c) + 2 >= w.size()) return "car";
            const std::string verb = w[static_cast<size_t>(c) + 2];
            if (verb == "spawn" || verb == "ambient") {
                std::string model = static_cast<size_t>(c) + 3 < w.size() ? w[static_cast<size_t>(c) + 3] : "";
                if (model == "profile" && static_cast<size_t>(c) + 4 < w.size()) model += " " + w[static_cast<size_t>(c) + 4];
                return verb + " " + model;
            }
            if (verb == "path") return "path " + (static_cast<size_t>(c) + 3 < w.size() ? w[static_cast<size_t>(c) + 3] : std::string());
            return verb;
        }
        case dir::EventKind::kWorld: {
            for (const char* key : {"hour", "weather", "traffic", "peds", "parked", "police", "timescale"}) {
                const int i = FindWord(w, key, 0);
                if (i >= 0) {
                    std::string s = key;
                    if (static_cast<size_t>(i) + 1 < w.size()) s += " " + w[static_cast<size_t>(i) + 1];
                    return s;
                }
            }
            return e.track;
        }
        case dir::EventKind::kSetting: {
            const int i = FindWord(w, "set", 0);
            if (i >= 0 && static_cast<size_t>(i) + 2 < w.size()) return w[static_cast<size_t>(i) + 1] + " " + w[static_cast<size_t>(i) + 2];
            return "set";
        }
        case dir::EventKind::kHud: {
            const int i = FindWord(w, "hud", 0);
            std::string s = "hud";
            for (size_t k = static_cast<size_t>(std::max(i, 0)) + 1; k < w.size() && k < static_cast<size_t>(std::max(i, 0)) + 3; ++k) s += " " + w[k];
            return s;
        }
        case dir::EventKind::kCapture: {
            const int i = FindWord(w, "capture", 0);
            if (i >= 0 && static_cast<size_t>(i) + 1 < w.size() && w[static_cast<size_t>(i) + 1] == "off") return "take off";
            const std::string fps = ValueAfter(w, "fps");
            return fps.empty() ? "take" : "take " + fps + " fps";
        }
        case dir::EventKind::kLabel:
        case dir::EventKind::kMark: {
            const int i = FindWord(w, e.kind == dir::EventKind::kLabel ? "label" : "mark", 0);
            std::string s = e.kind == dir::EventKind::kLabel ? "label" : "mark";
            if (i >= 0 && static_cast<size_t>(i) + 1 < w.size()) s += " " + w[static_cast<size_t>(i) + 1];
            return s;
        }
        case dir::EventKind::kSplit: return "split";
        case dir::EventKind::kOther: break;
    }
    return e.track;
}

Layout BuildLayout(const std::vector<dir::SnapEvent>& events, double duration) {
    Layout layout;
    // the lanes: CAMERA always, then a lane per car in the order the cars first appear, then the rest that have keys
    layout.lanes.push_back({LaneKind::kCamera, "CAMERA", ""});
    std::map<std::string, int> car_lane;
    bool have[8] = {};
    for (const dir::SnapEvent& e : events) {
        const LaneKind lk = LaneOf(e.kind);
        if (lk == LaneKind::kCar) {
            if (!e.car.empty() && !car_lane.count(e.car)) {
                car_lane[e.car] = 0;
                layout.lanes.push_back({LaneKind::kCar, "CAR " + e.car, e.car});
                car_lane[e.car] = static_cast<int>(layout.lanes.size()) - 1;
            }
        } else if (lk != LaneKind::kCamera) {
            have[static_cast<int>(lk)] = true;
        }
    }
    int fixed_lane[8];
    for (int& l : fixed_lane) l = -1;
    fixed_lane[static_cast<int>(LaneKind::kCamera)] = 0;
    for (LaneKind lk : {LaneKind::kWorld, LaneKind::kHud, LaneKind::kCapture, LaneKind::kMarks, LaneKind::kOther}) {
        if (!have[static_cast<int>(lk)]) continue;
        layout.lanes.push_back({lk, LaneKindName(lk), ""});
        fixed_lane[static_cast<int>(lk)] = static_cast<int>(layout.lanes.size()) - 1;
    }

    double last = 0.0;
    for (const dir::SnapEvent& e : events) {
        Marker m;
        m.line = e.line;
        m.t = e.t;
        m.kind = e.kind;
        m.text = e.text;
        const std::vector<std::string> w = Words(e.text);
        m.verb = w.empty() ? std::string() : w[0];
        m.label = DescribeStatement(e);
        m.is_cut = e.kind == dir::EventKind::kCut;
        m.movable = e.kind != dir::EventKind::kOther || m.verb != "start";
        if (e.kind == dir::EventKind::kCamera) m.ease = ValueAfter(w, "ease");
        const LaneKind lk = LaneOf(e.kind);
        if (lk == LaneKind::kCar) {
            const auto it = car_lane.find(e.car);
            if (it == car_lane.end()) continue;  // a car statement with no id: nothing to put it on
            m.lane = it->second;
        } else {
            m.lane = fixed_lane[static_cast<int>(lk)];
        }
        last = std::max(last, e.t);
        layout.markers.push_back(std::move(m));
    }
    std::stable_sort(layout.markers.begin(), layout.markers.end(), [](const Marker& a, const Marker& b) { return a.lane != b.lane ? a.lane < b.lane : a.t < b.t; });
    layout.end = duration > 0.0 ? duration : std::max(5.0, last + std::max(2.0, last * 0.1));

    // spans
    for (size_t i = 0; i < layout.markers.size(); ++i) {
        Marker& m = layout.markers[i];
        const bool rig = m.kind == dir::EventKind::kCamera && m.label.rfind("rig ", 0) == 0;
        const bool path = m.kind == dir::EventKind::kCar && m.label.rfind("path", 0) == 0;
        const bool spawn = m.kind == dir::EventKind::kCar && (m.label.rfind("spawn", 0) == 0 || m.label.rfind("ambient", 0) == 0);
        if (!rig && !path && !spawn) continue;
        double stop = layout.end;
        for (size_t k = i + 1; k < layout.markers.size() && layout.markers[k].lane == m.lane; ++k) {
            const Marker& n = layout.markers[k];
            if (n.t <= m.t) continue;
            bool ends = false;
            if (rig) ends = n.kind == dir::EventKind::kCamera;
            else if (path) ends = n.label.rfind("path", 0) == 0 || n.label == "stop" || n.label == "delete";
            else ends = n.label == "delete";
            if (ends) {
                stop = n.t;
                break;
            }
        }
        m.has_span = stop > m.t;
        m.span_end = stop;
    }
    return layout;
}

int FindMarkerByLine(const Layout& layout, int line) {
    for (size_t i = 0; i < layout.markers.size(); ++i) {
        if (layout.markers[i].line == line) return static_cast<int>(i);
    }
    return -1;
}

std::vector<double> MarkerTimes(const Layout& layout, int except_index) {
    std::vector<double> t;
    for (size_t i = 0; i < layout.markers.size(); ++i) {
        if (static_cast<int>(i) == except_index) continue;
        t.push_back(layout.markers[i].t);
    }
    return t;
}

// ---------------------------------------------------------------------------------------------------------------- the axis

double Axis::PixelsPerSecond() const {
    const double span = t1 - t0;
    return span > 1e-9 ? static_cast<double>(x1 - x0) / span : 1.0;
}

float Axis::TimeToX(double t) const {
    return static_cast<float>(x0 + (t - t0) * PixelsPerSecond());
}

double Axis::XToTime(float x) const {
    return t0 + (static_cast<double>(x) - x0) / PixelsPerSecond();
}

void Axis::Fit(double end) {
    end = std::max(end, 1.0);
    const double margin = end * 0.03;
    t0 = -margin;
    t1 = end + margin;
}

void Axis::ZoomAt(float x, double factor) {
    if (!(factor > 0.0) || !std::isfinite(factor)) return;
    const double span = t1 - t0;
    const double new_span = std::clamp(span / factor, kMinViewSpan, kMaxViewSpan);
    const double width = static_cast<double>(x1 - x0);
    const double frac = width > 0.0 ? (static_cast<double>(x) - x0) / width : 0.5;
    const double at = XToTime(x);
    t0 = at - frac * new_span;
    t1 = t0 + new_span;
}

void Axis::Pan(float dx_pixels) {
    const double dt = -static_cast<double>(dx_pixels) / PixelsPerSecond();
    t0 += dt;
    t1 += dt;
}

void Axis::KeepInside(double lo, double hi) {
    const double span = t1 - t0;
    const double margin = span * 0.1;
    if (span >= (hi - lo) + 2.0 * margin) {  // the whole scene fits: centre it
        const double mid = (lo + hi) * 0.5;
        t0 = mid - span * 0.5;
        t1 = t0 + span;
        return;
    }
    if (t0 < lo - margin) {
        t0 = lo - margin;
        t1 = t0 + span;
    }
    if (t1 > hi + margin) {
        t1 = hi + margin;
        t0 = t1 - span;
    }
}

std::vector<Tick> RulerTicks(const Axis& axis, float min_label_px) {
    static const struct {
        double step;
        int minor_div;
    } kSteps[] = {{0.01, 5}, {0.02, 4}, {0.05, 5}, {0.1, 5}, {0.2, 4}, {0.5, 5}, {1, 5}, {2, 4}, {5, 5}, {10, 5}, {15, 3}, {30, 6}, {60, 6}, {120, 4}, {300, 5}, {600, 5}, {1800, 6}, {3600, 6}};
    std::vector<Tick> out;
    const double pps = axis.PixelsPerSecond();
    if (!(pps > 0.0) || axis.t1 <= axis.t0) return out;
    size_t pick = sizeof(kSteps) / sizeof(kSteps[0]) - 1;
    for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i) {
        if (kSteps[i].step * pps >= min_label_px) {
            pick = i;
            break;
        }
    }
    const double step = kSteps[pick].step;
    const int div = kSteps[pick].minor_div;
    const double minor = step / div;
    const int decimals = step < 0.1 ? 2 : (step < 1.0 ? 1 : 0);
    const long long k0 = static_cast<long long>(std::floor(axis.t0 / minor));
    const long long k1 = static_cast<long long>(std::ceil(axis.t1 / minor));
    if (k1 - k0 > 4000) return out;  // a view this deep is not asked for; never build a list that long
    for (long long k = k0; k <= k1; ++k) {
        Tick t;
        t.t = static_cast<double>(k) * minor;
        t.major = (((k % div) + div) % div) == 0;
        if (t.major) t.label = FormatClock(t.t, decimals);
        out.push_back(std::move(t));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- snapping

double GridStep(const Axis& axis, double frame_step) {
    const double pps = std::max(axis.PixelsPerSecond(), 1e-9);
    if (frame_step > 1e-6 && frame_step * pps >= static_cast<double>(kFrameGridPx)) return frame_step;
    static const double kSteps[] = {0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0};
    for (double step : kSteps) {
        if (step * pps >= static_cast<double>(kGridMinPx)) return step;
    }
    return 600.0;
}

Snapped SnapTime(double t, const Axis& axis, const std::vector<double>& marks, double playhead, double end, double frame_step, float snap_px) {
    Snapped best;
    best.t = std::max(0.0, t);
    const double reach = static_cast<double>(snap_px) / std::max(axis.PixelsPerSecond(), 1e-9);  // seconds
    double best_d = reach;
    const auto consider = [&](double candidate, SnapKind kind) {
        const double d = std::fabs(candidate - t);
        if (d < best_d) {
            best_d = d;
            best.t = std::max(0.0, candidate);
            best.kind = kind;
        }
    };
    for (double m : marks) consider(m, SnapKind::kMark);
    consider(playhead, SnapKind::kPlayhead);
    consider(0.0, SnapKind::kStart);
    if (end > 0.0) consider(end, SnapKind::kEnd);
    if (best.kind != SnapKind::kNone) return best;
    const double step = GridStep(axis, frame_step);
    const bool frames = frame_step > 1e-6 && step == frame_step;
    best.t = std::max(0.0, std::round(t / step) * step);
    if (!frames) best.t = std::round(best.t * 1000.0) / 1000.0;  // 0.1 * 55 is 5.500000000000001: the file gets 5.5
    best.kind = frames ? SnapKind::kFrame : SnapKind::kGrid;
    return best;
}

// ---------------------------------------------------------------------------------------------------------------- hit test

int LaneAt(const Metrics& m, float y, int lane_count) {
    if (y < m.ruler_h) return -1;
    float top = m.ruler_h;
    for (int lane = 0; lane < lane_count; ++lane) {
        const float h = m.LaneHeight(lane);
        if (y < top) return -1;  // the gap above this lane
        if (y <= top + h) return lane;
        top += h + m.lane_gap;
    }
    return -1;
}

std::vector<bool> CollapsedLanes(const Metrics& m, const std::vector<int>& pref, float avail_h, float min_lane_h, int keep_open) {
    const int n = static_cast<int>(pref.size());
    std::vector<bool> out(pref.size(), false);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = pref[static_cast<size_t>(i)] < 0;
    // the room the open lanes need at the smallest height: the ruler, every lane's gap, the strips, then min_lane_h for each open one
    const auto need = [&]() {
        float h = m.ruler_h;
        for (int i = 0; i < n; ++i) h += m.lane_gap + (out[static_cast<size_t>(i)] ? m.collapsed_h : min_lane_h);
        return h;
    };
    for (int i = n - 1; i >= 0 && need() > avail_h; --i) {
        if (out[static_cast<size_t>(i)] || pref[static_cast<size_t>(i)] != 0 || i == keep_open) continue;
        out[static_cast<size_t>(i)] = true;
    }
    return out;
}

float FitLaneHeight(const Metrics& m, int lane_count, float avail_h, float min_h, float max_h) {
    int open = 0;
    float strips = 0.0f;
    for (int i = 0; i < lane_count; ++i) {
        if (m.Collapsed(i)) strips += m.collapsed_h + m.lane_gap;
        else ++open;
    }
    if (open == 0) return std::clamp(max_h, min_h, max_h);
    const float h = (avail_h - m.ruler_h - strips) / static_cast<float>(open) - m.lane_gap;
    return std::clamp(h, min_h, max_h);
}

int HitMarker(const Layout& layout, const Axis& axis, const Metrics& metrics, Vec2f p, float slop) {
    int best = -1;
    float best_d = 0.0f;
    for (size_t i = 0; i < layout.markers.size(); ++i) {
        const Marker& m = layout.markers[i];
        const float cx = axis.TimeToX(m.t);
        const float top = metrics.LaneTop(m.lane), bottom = top + metrics.LaneHeight(m.lane);
        float d;
        if (m.is_cut) {
            if (p.y < top || p.y > bottom) continue;
            d = std::fabs(p.x - cx);
            if (d > slop) continue;
        } else {
            const float cy = metrics.LaneCenter(m.lane);
            const float dx = p.x - cx, dy = p.y - cy;
            if (std::fabs(dx) > slop || std::fabs(dy) > metrics.MarkerRadius(m.lane) + slop * 0.5f) continue;
            d = std::sqrt(dx * dx + dy * dy);
        }
        if (best < 0 || d <= best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

int HitSpan(const Layout& layout, const Axis& axis, const Metrics& metrics, Vec2f p) {
    int best = -1;
    float best_w = 0.0f;
    for (size_t i = 0; i < layout.markers.size(); ++i) {
        const Marker& m = layout.markers[i];
        if (!m.has_span) continue;
        const float xa = axis.TimeToX(m.t), xb = axis.TimeToX(m.span_end);
        const float cy = metrics.LaneCenter(m.lane);
        if (p.x < xa || p.x > xb || std::fabs(p.y - cy) > 5.0f) continue;
        if (best < 0 || xb - xa < best_w) {
            best = static_cast<int>(i);
            best_w = xb - xa;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------------------- dragging

Drag BeginDrag(const Layout& layout, int marker, const Axis& axis, float x, bool duplicate) {
    Drag d;
    if (marker < 0 || static_cast<size_t>(marker) >= layout.markers.size()) return d;
    const Marker& m = layout.markers[static_cast<size_t>(marker)];
    if (!m.movable) return d;
    d.active = true;
    d.marker = marker;
    d.line = m.line;
    d.t_start = m.t;
    d.t_now = m.t;
    d.grab_dt = axis.XToTime(x) - m.t;
    d.duplicate = duplicate;
    d.start_x = x;
    return d;
}

void UpdateDrag(Drag* drag, const Axis& axis, float x, const std::vector<double>& marks, double playhead, double end, double frame_step, bool snap) {
    if (!drag || !drag->active) return;
    if (std::fabs(x - drag->start_x) > kDragSlopPx) drag->moved = true;
    if (!drag->moved) {
        drag->t_now = drag->t_start;
        drag->snap = SnapKind::kNone;
        return;
    }
    const double want = axis.XToTime(x) - drag->grab_dt;
    if (snap) {
        const Snapped s = SnapTime(want, axis, marks, playhead, end, frame_step);
        drag->t_now = s.t;
        drag->snap = s.kind;
    } else {
        drag->t_now = std::max(0.0, std::round(want * 1000.0) / 1000.0);
        drag->snap = SnapKind::kNone;
    }
}

DragOutcome FinishDrag(const Drag& drag) {
    if (!drag.active || !drag.moved) return DragOutcome::kNothing;
    if (std::fabs(drag.t_now - drag.t_start) < 0.0005) return DragOutcome::kNothing;
    return drag.duplicate ? DragOutcome::kDuplicate : DragOutcome::kMove;
}

double ScrubTime(const Axis& axis, float x, double end, double frame_step) {
    double t = std::clamp(axis.XToTime(x), 0.0, std::max(end, 0.0));
    if (frame_step > 1e-6) t = std::min(std::max(end, 0.0), std::round(t / frame_step) * frame_step);
    return t;
}

}  // namespace mcla::ui::director
