// The Director panel's world gizmos, the pure half: see director_gizmos.h.

#include "director_gizmos.h"

#include <algorithm>
#include <cmath>

#include "mc_engine/cinematic/camera.h"
#include "mc_engine/cinematic/director/director_math.h"
#include "mc_engine/display/frame_fit.h"

namespace mcla::ui::director {

namespace cam = mc::cinematic::camera;

namespace {

double Dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

uint32_t Channel(uint32_t color, int shift) {
    return (color >> shift) & 0xFFu;
}

uint32_t Lerp(uint32_t a, uint32_t b, double u) {
    const auto mix = [&](int shift) { return static_cast<uint32_t>(std::lround(Channel(a, shift) + (static_cast<double>(Channel(b, shift)) - Channel(a, shift)) * u)); };
    return (mix(24) << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

constexpr double kNearClip = 0.05;  // metres: a segment is cut here (the camera's own near plane is usually 0.1 and may be larger)

}  // namespace

uint32_t WithAlpha(uint32_t color, uint32_t alpha) {
    return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
}

RectF PictureRect(float window_w, float window_h, double aspect) {
    RectF r;
    if (!(window_w > 0.0f) || !(window_h > 0.0f) || !(aspect > 0.0)) return r;
    const double shown = static_cast<double>(window_h) * aspect;
    const double scale = std::min(static_cast<double>(window_w) / shown, 1.0);
    r.w = static_cast<float>(shown * scale);
    r.h = static_cast<float>(static_cast<double>(window_h) * scale);
    r.x = (window_w - r.w) * 0.5f;
    r.y = (window_h - r.h) * 0.5f;
    return r;
}

double EffectiveFov(double slot_fov_degrees, double aspect) {
    return mc::frame::VertPlusFov(slot_fov_degrees, aspect);
}

// ---------------------------------------------------------------------------------------------------------------- the projector

Projector Projector::Make(const dir::SnapCamera& camera, float window_w, float window_h) {
    Projector p;
    if (!camera.valid) return p;
    p.aspect = camera.aspect > 0.05 ? camera.aspect : 16.0 / 9.0;
    p.picture = PictureRect(window_w, window_h, p.aspect);
    if (p.picture.Empty()) return p;
    dir::PoseAxes(camera.pose, p.right, p.up, p.fwd);
    for (int i = 0; i < 3; ++i) p.pos[i] = camera.pose.pos[i];
    const double fov = std::clamp(EffectiveFov(std::clamp(camera.pose.fov, 1.0, 170.0), p.aspect), 1.0, 175.0);
    p.tan_half_fov = std::tan(fov * cam::kDeg * 0.5);
    p.near_plane = camera.near_plane > 0.0 ? camera.near_plane : 0.1;
    p.valid = true;
    return p;
}

bool Projector::ProjectCameraSpace(double x, double y, double z, Vec2f* out) const {
    if (!valid || !(z > 1e-6)) return false;
    const double ndc_x = x / (z * tan_half_fov * aspect);
    const double ndc_y = y / (z * tan_half_fov);
    out->x = static_cast<float>(picture.x + (ndc_x * 0.5 + 0.5) * picture.w);
    out->y = static_cast<float>(picture.y + (0.5 - ndc_y * 0.5) * picture.h);
    return true;
}

bool Projector::Project(const double p[3], Vec2f* out, double* depth) const {
    if (!valid) return false;
    const double rel[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    const double z = Dot3(rel, fwd);
    if (!ProjectCameraSpace(Dot3(rel, right), Dot3(rel, up), z, out)) return false;
    if (depth) *depth = z;
    return true;
}

bool Projector::ProjectSegment(const double a[3], const double b[3], Vec2f* pa, Vec2f* pb) const {
    if (!valid) return false;
    const double ra[3] = {a[0] - pos[0], a[1] - pos[1], a[2] - pos[2]};
    const double rb[3] = {b[0] - pos[0], b[1] - pos[1], b[2] - pos[2]};
    double ca[3] = {Dot3(ra, right), Dot3(ra, up), Dot3(ra, fwd)};
    double cb[3] = {Dot3(rb, right), Dot3(rb, up), Dot3(rb, fwd)};
    const double clip = std::max(kNearClip, 1e-3);
    if (ca[2] < clip && cb[2] < clip) return false;
    // cut the end that is behind the plane z = clip
    if (ca[2] < clip || cb[2] < clip) {
        double* behind = ca[2] < clip ? ca : cb;
        const double* front = ca[2] < clip ? cb : ca;
        const double u = (clip - behind[2]) / (front[2] - behind[2]);
        for (int i = 0; i < 3; ++i) behind[i] += (front[i] - behind[i]) * u;
        behind[2] = clip;
    }
    return ProjectCameraSpace(ca[0], ca[1], ca[2], pa) && ProjectCameraSpace(cb[0], cb[1], cb[2], pb);
}

Ray Projector::PixelRay(Vec2f px) const {
    Ray ray;
    for (int i = 0; i < 3; ++i) ray.origin[i] = pos[i];
    if (!valid) {
        ray.dir[0] = ray.dir[1] = 0.0;
        ray.dir[2] = 1.0;
        return ray;
    }
    const double ndc_x = (static_cast<double>(px.x) - picture.x) / picture.w * 2.0 - 1.0;
    const double ndc_y = 1.0 - (static_cast<double>(px.y) - picture.y) / picture.h * 2.0;
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = fwd[i] + right[i] * ndc_x * tan_half_fov * aspect + up[i] * ndc_y * tan_half_fov;
    const double len = std::sqrt(Dot3(d, d));
    for (int i = 0; i < 3; ++i) ray.dir[i] = len > 0.0 ? d[i] / len : fwd[i];
    return ray;
}

bool Projector::ToUV(Vec2f px, double* u, double* v) const {
    if (!valid) return false;
    *u = (static_cast<double>(px.x) - picture.x) / picture.w;
    *v = (static_cast<double>(px.y) - picture.y) / picture.h;
    return *u >= 0.0 && *u <= 1.0 && *v >= 0.0 && *v <= 1.0;
}

bool RayPlaneY(const Ray& ray, double plane_y, double out[3]) {
    if (std::fabs(ray.dir[1]) < 1e-9) return false;
    const double t = (plane_y - ray.origin[1]) / ray.dir[1];
    if (!(t > 0.0)) return false;
    for (int i = 0; i < 3; ++i) out[i] = ray.origin[i] + ray.dir[i] * t;
    out[1] = plane_y;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- colours and primitives

uint32_t SpeedColor(double speed_mps) {
    static const struct {
        double at;
        uint32_t color;
    } kStops[] = {{0.0, Rgba(58, 140, 255)}, {12.0, Rgba(51, 214, 214)}, {24.0, Rgba(242, 227, 5)}, {40.0, Rgba(238, 85, 68)}};
    if (!(speed_mps > 0.0)) return kStops[0].color;
    for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
        if (speed_mps <= kStops[i].at) return Lerp(kStops[i - 1].color, kStops[i].color, (speed_mps - kStops[i - 1].at) / (kStops[i].at - kStops[i - 1].at));
    }
    return kStops[3].color;
}

namespace {

void AddArrow(Vec2f a, Vec2f b, uint32_t color, float width, Gizmos* out) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float ux = dx / len, uy = dy / len;
    const float size = 9.0f;
    const Vec2f tip = b;
    const Vec2f l{tip.x - ux * size - uy * size * 0.5f, tip.y - uy * size + ux * size * 0.5f};
    const Vec2f r{tip.x - ux * size + uy * size * 0.5f, tip.y - uy * size - ux * size * 0.5f};
    out->lines.push_back({tip, l, color, width});
    out->lines.push_back({tip, r, color, width});
}

bool PointOf(const std::vector<double>& p, double out[3]) {
    if (p.size() < 3) return false;
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
    return true;
}

}  // namespace

void AddRoute(const Projector& view, const std::vector<std::vector<double>>& points, int id_base, const std::string& label, bool highlighted, Gizmos* out, float arrow_px) {
    if (!view.valid || points.empty()) return;
    const float width = highlighted ? 3.0f : 2.0f;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        double a[3], b[3];
        if (!PointOf(points[i], a) || !PointOf(points[i + 1], b)) continue;
        Vec2f pa, pb;
        if (!view.ProjectSegment(a, b, &pa, &pb)) continue;
        const double speed = points[i].size() > 3 ? points[i][3] : 14.0;
        const uint32_t color = WithAlpha(SpeedColor(speed), highlighted ? 255 : 200);
        out->lines.push_back({pa, pb, color, width});
        const float dx = pb.x - pa.x, dy = pb.y - pa.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (arrow_px > 0.0f && len >= arrow_px * 0.5f) {
            const Vec2f mid{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
            const Vec2f tail{mid.x - dx / len * 6.0f, mid.y - dy / len * 6.0f};
            AddArrow(tail, mid, color, width, out);
        }
    }
    if (id_base >= 0) {
        for (size_t i = 0; i < points.size(); ++i) {
            double p[3];
            Vec2f px;
            if (!PointOf(points[i], p) || !view.Project(p, &px)) continue;
            const double speed = points[i].size() > 3 ? points[i][3] : 14.0;
            out->dots.push_back({px, i == 0 ? 7.0f : 5.5f, WithAlpha(SpeedColor(speed), 255), true, id_base + static_cast<int>(i)});
            out->labels.push_back({px, std::to_string(i + 1), Rgba(255, 255, 255)});
        }
    }
    if (!label.empty()) {
        double p[3];
        Vec2f px;
        if (PointOf(points.front(), p) && view.Project(p, &px)) out->labels.push_back({px, label, highlighted ? Rgba(255, 255, 255) : Rgba(210, 210, 200)});
    }
}

void AddCameraPath(const Projector& view, const std::vector<dir::SnapCamSample>& samples, uint32_t color, Gizmos* out) {
    if (!view.valid || samples.size() < 2) return;
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        if (samples[i + 1].cut) continue;  // a cut is a jump: no line across it
        Vec2f pa, pb;
        if (view.ProjectSegment(samples[i].pos, samples[i + 1].pos, &pa, &pb)) out->lines.push_back({pa, pb, color, 2.0f});
    }
}

void AddFrustum(const Projector& view, const dir::SnapPose& key, double aspect, double length, uint32_t color, bool selected, Gizmos* out) {
    if (!view.valid) return;
    double right[3], up[3], fwd[3];
    dir::PoseAxes(key, right, up, fwd);
    const double th = std::tan(std::clamp(EffectiveFov(key.fov, aspect), 1.0, 175.0) * cam::kDeg * 0.5);
    const double hh = th * length, hw = hh * (aspect > 0.05 ? aspect : 16.0 / 9.0);
    const auto corner = [&](double sx, double sy, double c[3]) {
        for (int i = 0; i < 3; ++i) c[i] = key.pos[i] + fwd[i] * length + right[i] * hw * sx + up[i] * hh * sy;
    };
    double c[4][3];
    corner(-1, 1, c[0]);
    corner(1, 1, c[1]);
    corner(1, -1, c[2]);
    corner(-1, -1, c[3]);
    const float width = selected ? 2.8f : 1.5f;
    for (int i = 0; i < 4; ++i) {
        Vec2f pa, pb;
        if (view.ProjectSegment(key.pos, c[i], &pa, &pb)) out->lines.push_back({pa, pb, color, width});
        if (view.ProjectSegment(c[i], c[(i + 1) % 4], &pa, &pb)) out->lines.push_back({pa, pb, color, width});
    }
}

void AddCar(const Projector& view, const dir::SnapCar& car, bool selected, int id, Gizmos* out) {
    if (!view.valid) return;
    double p[3] = {car.pos[0], car.pos[1] + 1.0, car.pos[2]};
    Vec2f px;
    double depth = 0.0;
    if (!view.Project(p, &px, &depth)) return;
    const uint32_t color = !car.ready ? Rgba(150, 150, 150) : selected ? Rgba(242, 227, 5) : Rgba(51, 214, 214);
    // the ring's size follows the car's size in the picture (about 4 m long), at least 8 pixels
    const float radius = std::clamp(static_cast<float>(2.0 / (depth * view.tan_half_fov) * view.picture.h * 0.5), 8.0f, 90.0f);
    out->dots.push_back({px, radius, color, false, id});
    std::string text = car.id;
    if (!car.model.empty()) {
        std::string m = car.model;
        if (m.size() > 22) m = m.substr(0, 22);
        text += "  " + m;
    }
    if (!car.ready) text += "  (loading)";
    out->labels.push_back({{px.x, px.y - radius - 4.0f}, text, color});
}

int NearestDot(const std::vector<Dot2>& dots, Vec2f px, float radius) {
    int best = -1;
    float best_d = 0.0f;
    for (size_t i = 0; i < dots.size(); ++i) {
        if (dots[i].id < 0) continue;
        const float dx = dots[i].p.x - px.x, dy = dots[i].p.y - px.y;
        const float d = dx * dx + dy * dy;
        const float reach = std::max(radius, dots[i].radius);
        if (d > reach * reach) continue;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

std::vector<dir::SnapCamSample> SplineThrough(const std::vector<dir::SnapCamSample>& keys, int per_segment) {
    std::vector<dir::SnapCamSample> out;
    if (keys.size() < 2 || per_segment < 1) return keys;
    const auto vec = [](const dir::SnapCamSample& s) { return cam::Vec3{s.pos[0], s.pos[1], s.pos[2]}; };
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const cam::Vec3 p0 = vec(keys[i == 0 ? 0 : i - 1]), p1 = vec(keys[i]), p2 = vec(keys[i + 1]), p3 = vec(keys[std::min(i + 2, keys.size() - 1)]);
        for (int k = 0; k < per_segment; ++k) {
            const double u = static_cast<double>(k) / per_segment;
            const cam::Vec3 q = k == 0 ? p1 : cam::CatmullRom(p0, p1, p2, p3, u);
            dir::SnapCamSample s;
            s.t = keys[i].t + (keys[i + 1].t - keys[i].t) * u;
            s.pos[0] = q.x, s.pos[1] = q.y, s.pos[2] = q.z;
            s.cut = k == 0 && keys[i].cut;
            out.push_back(s);
        }
    }
    out.push_back(keys.back());
    return out;
}

}  // namespace mcla::ui::director
