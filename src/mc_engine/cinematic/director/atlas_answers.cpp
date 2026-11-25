#include "atlas_answers.h"

#ifdef LARECOMP_ATLAS_HAS_DIRECTOR

#include <cmath>
#include <cstdlib>

namespace mc::cinematic::atlas {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

double Round(double v, int decimals) {
    const double s = std::pow(10.0, decimals);
    return std::round(v * s) / s;
}

Json Vec3J(const double v[3], int decimals) { return Json::Vec3(Round(v[0], decimals), Round(v[1], decimals), Round(v[2], decimals)); }

Json FloatVec(const float v[3], int decimals) {
    const double d[3] = {v[0], v[1], v[2]};
    return Vec3J(d, decimals);
}

bool ParseOffset(std::string_view text, double* r, double* u, double* f) {
    const std::string s(text);
    char* end = nullptr;
    const char* p = s.c_str();
    double v[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        if (i < 2) {
            if (*p != ',') return false;
            ++p;
        }
    }
    if (*p != '\0') return false;
    *r = v[0];
    *u = v[1];
    *f = v[2];
    return true;
}

}  // namespace

Json VecJson(const Vec3& v) {
    const double d[3] = {v.x, v.y, v.z};
    return Vec3J(d, 2);
}

Json LightJson(const Light& l, double dist, double reach) {
    Json o = Json::Object();
    o.Set("i", l.i);
    o.Set("pos", FloatVec(l.pos, 3));
    o.Set("color", FloatVec(l.color, 4));
    o.Set("dir", FloatVec(l.dir, 4));
    o.Set("intensity", Round(l.intensity, 3));
    o.Set("cone", Round(l.cone, 2));
    o.Set("spread", Round(l.spread, 2));
    o.Set("type", l.spot ? "spot" : "omni");
    o.Set("on", l.on);
    o.Set("src", l.lamp ? "lamp" : "file");
    o.Set("dist", Round(dist, 2));
    o.Set("reach_m", Round(reach, 1));
    return o;
}

bool ResolvePosition(const Json& value, const director::Snapshot* snap, Vec3* pos, bool* has_y, std::string* error) {
    *has_y = false;
    if (value.is_array()) {
        double v[3];
        if (value.size() == 3 && value.ToVec3(v)) {
            *pos = {v[0], v[1], v[2]};
            *has_y = true;
            return true;
        }
        if (value.size() == 2 && value.at(0).is_number() && value.at(1).is_number()) {
            *pos = {value.at(0).AsNumber(), 0.0, value.at(1).AsNumber()};
            return true;
        }
        *error = "a position is [x, y, z] or [x, z]";
        return false;
    }
    if (value.is_object()) {
        if (!value.Get("x").is_number() || !value.Get("z").is_number()) {
            *error = "a position object needs x and z";
            return false;
        }
        *pos = {value.Get("x").AsNumber(), value.Get("y").AsNumber(), value.Get("z").AsNumber()};
        *has_y = value.Get("y").is_number();
        return true;
    }
    if (!value.is_string()) {
        *error = "a position is [x, y, z], [x, z], {x, z}, \"player\", \"car:<id>\", \"camera\" or \"spot:<name>\"";
        return false;
    }
    std::string text = value.AsString();
    if (snap && text == "camera") {
        *pos = {snap->camera.pose.pos[0], snap->camera.pose.pos[1], snap->camera.pose.pos[2]};
        *has_y = true;
        return true;
    }
    if (text == "player" || text.rfind("car:", 0) == 0) {
        const std::string id = text == "player" ? text : text.substr(4);
        if (snap) {
            for (const director::SnapCar& c : snap->cars) {
                if (c.id == id) {
                    *pos = {c.pos[0], c.pos[1], c.pos[2]};
                    *has_y = true;
                    return true;
                }
            }
        }
        *error = "no car '" + id + "' in the scene";
        return false;
    }
    if (text.rfind("spot:", 0) == 0) text = text.substr(5);
    std::string name = text;
    double r = 0.0, u = 0.0, f = 0.0;
    bool offset = false;
    const size_t at = text.find('@');
    if (at != std::string::npos) {
        name = text.substr(0, at);
        if (!ParseOffset(std::string_view(text).substr(at + 1), &r, &u, &f)) {
            *error = "the offset of '" + text + "' is @right,up,forward";
            return false;
        }
        offset = true;
    }
    if (snap) {
        for (const director::SnapSpot& s : snap->spots) {
            if (s.name != name) continue;
            const double h = s.heading * kDeg;
            const double fx = std::sin(h), fz = std::cos(h);
            *pos = {s.pos[0], s.pos[1], s.pos[2]};
            if (offset) *pos = {s.pos[0] + f * fx - r * fz, s.pos[1] + u, s.pos[2] + f * fz + r * fx};
            *has_y = true;
            return true;
        }
    }
    *error = "unknown spot '" + name + "'";
    return false;
}

bool ResolveRouteEnd(const Json& value, const director::Snapshot* snap, RouteEnd* end, std::string* error) {
    if (value.is_object() && value.Has("rail")) {
        end->by_rail = true;
        end->rail = static_cast<int>(value.IntOr("rail", -1));
        end->t = value.NumberOr("t", 0.0);
        return true;
    }
    Vec3 p;
    bool has_y = false;
    if (!ResolvePosition(value, snap, &p, &has_y, error)) return false;
    end->x = p.x;
    end->y = p.y;
    end->z = p.z;
    end->has_y = has_y;
    return true;
}

bool ResolveCamera(const Json* override_pose, const director::Snapshot* snap, CameraPose* pose, std::string* error) {
    if (override_pose && override_pose->is_object()) {
        double p[3];
        if (!override_pose->Get("pos").ToVec3(p)) {
            *error = "camera.pos must be [x, y, z]";
            return false;
        }
        pose->pos = {p[0], p[1], p[2]};
        pose->yaw = override_pose->NumberOr("yaw", 0.0);
        pose->pitch = override_pose->NumberOr("pitch", 0.0);
        pose->roll = override_pose->NumberOr("roll", 0.0);
        pose->fov = override_pose->NumberOr("fov", snap ? snap->camera.pose.fov : 50.0);
        pose->aspect = snap ? snap->camera.aspect : 16.0 / 9.0;
        pose->near_plane = snap ? snap->camera.near_plane : 0.1;
        return true;
    }
    if (!snap || !snap->camera.valid) {
        *error = "no camera yet";
        return false;
    }
    const director::SnapPose& s = snap->camera.pose;
    pose->pos = {s.pos[0], s.pos[1], s.pos[2]};
    pose->yaw = s.yaw;
    pose->pitch = s.pitch;
    pose->roll = s.roll;
    pose->fov = s.fov;
    pose->aspect = snap->camera.aspect;
    pose->near_plane = snap->camera.near_plane;
    return true;
}

bool AnswerLights(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error) {
    Vec3 p;
    bool has_y = false;
    const Json& where = args.Has("near") ? args.Get("near") : args.Get("pos");
    if (where.is_null()) {
        *error = "near is needed: a position";
        return false;
    }
    if (!ResolvePosition(where, snap, &p, &has_y, error)) return false;
    const double r = args.NumberOr("r", 60.0);
    const int n = static_cast<int>(args.IntOr("n", 20));
    if (!(r > 0.0 && r <= 5000.0) || n < 0) {
        *error = "r must be 0..5000 metres and n at least 0";
        return false;
    }
    const std::string kind = args.StringOr("kind", "");
    if (!kind.empty() && kind != "spot" && kind != "omni") {
        *error = "kind is \"spot\" or \"omni\"";
        return false;
    }
    const std::string src = args.StringOr("src", "");
    if (!src.empty() && src != "lamp" && src != "file") {
        *error = "src is \"lamp\" (the street lamps) or \"file\" (the file's own lights)";
        return false;
    }
    const LightsAnswer a = NearLights(index, p.x, p.z, has_y, p.y, r, n, args.NumberOr("min_intensity", 0.0), kind, src);
    Json list = Json::Array();
    for (const LightHit& h : a.hits) list.Push(LightJson(index.world().lights[h.light], h.dist, h.reach));
    *result = Json::Object();
    result->Set("count", static_cast<int>(a.hits.size()));
    result->Set("in_radius", a.in_radius);
    result->Set("lights", std::move(list));
    return true;
}

bool AnswerRoad(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error) {
    Vec3 p;
    bool has_y = false;
    const Json& pos = args.Has("pos") ? args.Get("pos") : args.Get("near");
    if (pos.is_null()) {
        *error = "pos is needed: a position";
        return false;
    }
    if (!ResolvePosition(pos, snap, &p, &has_y, error)) return false;
    const double r = args.NumberOr("r", 40.0);
    const int n = static_cast<int>(args.IntOr("n", 6));
    if (!(r > 0.0 && r <= 2000.0) || n < 1) {
        *error = "r must be 0..2000 metres and n at least 1";
        return false;
    }
    const World& w = index.world();
    if (w.rails.empty()) {
        *error = "the world has no rail network";
        return false;
    }
    const bool has_heading = args.Get("heading").is_number();
    auto hits = ProjectOnRails(index, p.x, p.z, has_y, p.y, r, has_heading, args.NumberOr("heading", 0.0));
    if (hits.size() > static_cast<size_t>(n)) hits.resize(n);
    Json list = Json::Array();
    for (const Projection& h : hits) {
        const Rail& rail = w.rails[h.rail];
        Json o = Json::Object();
        o.Set("rail", rail.id);
        o.Set("node", rail.node);
        o.Set("kind", rail.node >= 0 ? NodeKindName(w.nodes[rail.node].kind) : "?");
        o.Set("name", std::string(NameOf(w, rail)));
        o.Set("speed", Round(SpeedOf(w, rail), 2));
        o.Set("dir", rail.dir != 0 ? Json(rail.dir) : Json());
        o.Set("dist_m", Round(h.dist, 2));
        o.Set("t", Round(h.t, 4));
        o.Set("pos", VecJson(h.point));
        o.Set("heading_deg", Round(h.heading, 1));
        o.Set("len_m", Round(rail.len, 2));
        Json next = Json::Array();
        for (int nx : rail.next) next.Push(nx);
        o.Set("next", std::move(next));
        list.Push(std::move(o));
    }
    *result = Json::Object();
    result->Set("on_road", !hits.empty() && hits[0].dist <= 6.0);
    result->Set("nearest", std::move(list));
    return true;
}

bool AnswerRoute(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error) {
    if (!args.Has("from") || !args.Has("to")) {
        *error = "from and to are needed";
        return false;
    }
    RouteEnd from, to;
    if (!ResolveRouteEnd(args.Get("from"), snap, &from, error) || !ResolveRouteEnd(args.Get("to"), snap, &to, error)) return false;
    if (args.Get("heading").is_number()) {
        from.has_heading = true;
        from.heading = args.NumberOr("heading", 0.0);
    }
    std::vector<RouteEnd> via;
    for (const Json& v : args.Get("via").items()) {
        RouteEnd e;
        if (!ResolveRouteEnd(v, snap, &e, error)) return false;
        via.push_back(e);
        if (via.size() > 16) {
            *error = "at most 16 via points";
            return false;
        }
    }
    const double cap = args.NumberOr("speed", 0.0);
    if (cap < 0.0 || cap > 120.0) {
        *error = "speed is a cap in m/s, 0..120";
        return false;
    }
    const RouteAnswer a = PlanRoute(index, from, via, to, cap);
    if (!a.ok) {
        *error = a.error;
        return false;
    }
    Json pts = Json::Array();
    for (const RoutePoint& p : a.points) {
        Json q = Json::Array();
        q.Push(Round(p.x, 2));
        q.Push(Round(p.z, 2));
        q.Push(Round(p.speed, 2));
        q.Push(Round(p.y, 2));
        pts.Push(std::move(q));
    }
    Json rails = Json::Array();
    for (int r : a.rails) rails.Push(r);
    Json names = Json::Array();
    for (const std::string& s : a.names) names.Push(s);
    Json start = Json::Object(), end = Json::Object();
    start.Set("rail", a.start_rail);
    start.Set("dist_m", Round(a.start_dist, 2));
    end.Set("rail", a.end_rail);
    end.Set("dist_m", Round(a.end_dist, 2));
    *result = Json::Object();
    result->Set("points", std::move(pts));
    result->Set("rails", std::move(rails));
    result->Set("length_m", Round(a.length_m, 1));
    result->Set("duration_s", Round(a.duration_s, 1));
    result->Set("start", std::move(start));
    result->Set("end", std::move(end));
    result->Set("names", std::move(names));
    return true;
}

void AnswerGarages(const World& world, Json* result) {
    Json list = Json::Array();
    for (const Garage& g : world.garages) {
        Json o = Json::Object();
        o.Set("name", g.name);
        o.Set("pos", Json::Vec3(Round(g.pos.x, 3), Round(g.pos.y, 3), Round(g.pos.z, 3)));
        list.Push(std::move(o));
    }
    *result = Json::Object();
    result->Set("garages", std::move(list));
}

void AnswerProjection(const CameraPose& pose, Vec3 subject, double radius, Json* result) {
    const Projected p = ProjectSphere(pose, subject, radius);
    *result = Json::Object();
    result->Set("in_front", p.in_front);
    result->Set("in_frame", p.in_frame);
    if (p.in_front) {
        Json s = Json::Array();
        s.Push(Round(p.u, 4));
        s.Push(Round(p.v, 4));
        result->Set("screen", std::move(s));
        result->Set("size_frac", Round(p.size_frac, 4));
    } else {
        result->Set("screen", Json());
        result->Set("size_frac", Json());
    }
    result->Set("dist_m", Round(p.dist, 2));
    result->Set("depth_m", Round(p.depth, 2));
}

}  // namespace mc::cinematic::atlas

#endif  // LARECOMP_ATLAS_HAS_DIRECTOR
