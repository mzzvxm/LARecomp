#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's car commands (DIRECTOR-PROTOCOL.md "cars"): the list and the picker's vehicle names, a transient placement, the scene statements that spawn,
// delete and route a car (inserted as lines; the soft reload makes them real without a restart), and the routes: recording a drive, dropping waypoints at the
// camera, listing and reading the route files.
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "../cars.h"
#include "../freecam.h"
#include "../freecam_runtime.h"
#include "../path_follow.h"
#include "../scene_runtime.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"

namespace mc::cinematic::director {

namespace {

using internal::Arg;
using internal::EditOutcome;
using internal::MakeSchema;
namespace fc = mc::cinematic::freecam;

std::string Num(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return (s.empty() || s == "-0") ? "0" : s;
}

bool GoodId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

// A word of a scene statement: no blanks, quotes or comment marks.
bool GoodWord(const std::string& w) {
    return !w.empty() && w.find_first_of(" \t\"#") == std::string::npos;
}

// What a `at` or `heading` argument writes: a number or a vec3 as numbers, a string as it is (a spot word).
bool PointWords(const Json& v, std::string* out) {
    double p[3];
    if (v.ToVec3(p)) {
        *out = Num(p[0]) + "," + Num(p[1]) + "," + Num(p[2]);
        return true;
    }
    if (v.is_string() && GoodWord(v.AsString())) {
        *out = v.AsString();
        return true;
    }
    return false;
}

const char* By(const Request& req) {
    return req.source == Source::kPanel || req.source == Source::kConsole ? "panel" : "agent";
}

// The reply of a command that inserted a line: where it went and what it says.
Json InsertedJson(const EditOutcome& o, const std::string& text) {
    Json res = o.result;
    Json lines = Json::Array();
    for (int line : o.lines) lines.Push(line);
    res.Set("lines", std::move(lines));
    res.Set("line", o.lines.empty() ? 0 : o.lines[0]);
    res.Set("text", text);
    return res;
}

// ---- the route files

// A route the Director is making (waypoints dropped one at a time) has ONE point after the first drop; a route a car drives needs two (ParseRoute's default).
constexpr size_t kMakingRoutePoints = 1;

// Looks for a route file the way the cars package does: beside the scene, or in a `routes` folder beside it.
bool FindRoute(const std::string& name, std::filesystem::path* out, std::string* error) {
    const PathCheck c = CheckRelativePath(name);
    if (!c.ok) {
        *error = "route '" + name + "': " + c.error;
        return false;
    }
    const std::filesystem::path dir = internal::SceneFolder();
    if (dir.empty()) {
        *error = "no scene is loaded: routes are looked up beside it";
        return false;
    }
    std::vector<std::string> names{c.normalised};
    if (c.normalised.size() < 6 || c.normalised.substr(c.normalised.size() - 6) != ".route") names.push_back(c.normalised + ".route");
    std::error_code ec;
    for (const std::string& n : names) {
        for (const char* prefix : {"routes/", ""}) {
            const std::filesystem::path p = dir / (std::string(prefix) + n);
            if (std::filesystem::is_regular_file(p, ec)) {
                *out = p;
                return true;
            }
        }
    }
    *error = "no route file '" + name + "' beside the scene or in its routes folder";
    return false;
}

std::vector<WayPoint> ToWayPoints(const Route& r) {
    std::vector<WayPoint> out;
    for (const RoutePoint& p : r.points) out.push_back({p.x, p.has_y ? p.y : 0.0, p.z, p.speed < 0.0 ? kDefaultRouteSpeed : p.speed});
    return out;
}

Json PointsJson(const std::vector<WayPoint>& pts) {
    Json a = Json::Array();
    for (const WayPoint& p : pts) {
        Json q = Json::Array();
        q.Push(p.x);
        q.Push(p.y);
        q.Push(p.z);
        q.Push(p.speed);
        a.Push(std::move(q));
    }
    return a;
}

// Where a new route is written: routes/<name>.route under the scene's folder.
bool RouteWritePath(const std::string& name, std::filesystem::path* out, std::string* stored_name, std::string* error) {
    const PathCheck c = CheckRelativePath(name);
    if (!c.ok || !GoodWord(c.normalised)) {
        *error = "route '" + name + "': " + (c.ok ? "a route name has no blanks or quotes" : c.error);
        return false;
    }
    std::string n = c.normalised;
    if (n.size() < 6 || n.substr(n.size() - 6) != ".route") n += ".route";
    *stored_name = n;
    return internal::ResolveWritePath("routes/" + n, true, out, error);
}

// ---- recording a drive

struct Recording {
    bool on = false;
    std::string id, name;
    double every = 2.0;
    std::vector<WayPoint> points;
    double t_last = 0.0;
    double last[3] = {};
    bool have_last = false;
    uint64_t samples = 0;
};
Recording g_rec;

double WallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RegisterCars() {
    RegisterCommand(
        "cars.list",
        [](const Request&, const Responder& r) {
            const auto snap = GetSnapshot();
            Json cars = Json::Array();
            for (const SceneCarInfo& c : ListSceneCars()) {
                Json j = Json::Object();
                j.Set("id", c.id);
                j.Set("model", c.model);
                j.Set("pos", Json::Vec3(c.position[0], c.position[1], c.position[2]));
                j.Set("heading", c.heading);
                double speed = 0.0;
                if (snap) {
                    for (const SnapCar& s : snap->cars) {
                        if (s.id == c.id) speed = s.speed;
                    }
                }
                j.Set("speed", speed);
                j.Set("ready", c.ready);
                j.Set("on_path", c.on_path);
                cars.Push(std::move(j));
            }
            Json res = Json::Object();
            res.Set("cars", std::move(cars));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "the scene's cars and the player's", {}, "{cars:[{id, model, pos, heading, speed, ready, on_path}]}"));

    RegisterCommand(
        "cars.models",
        [](const Request& req, const Responder& r) {
            std::string search = req.args.StringOr("search", "");
            std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            Json models = Json::Array();
            for (const std::string& name : ListCarModels()) {
                std::string low = name;
                std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!search.empty() && low.find(search) == std::string::npos) continue;
                Json m = Json::Object();
                m.Set("name", name);
                m.Set("class", "");  // CHECK: the game's car table has the class in a field the toolkit has not read yet
                models.Push(std::move(m));
            }
            Json res = Json::Object();
            res.Set("models", std::move(models));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "the game's vehicle names (for the picker)", {Arg("search", ArgType::kString, false, "keep names holding this text")}, "{models:[{name, class}]}"));

    RegisterCommand(
        "car.place",
        [](const Request& req, const Responder& r) {
            const std::string id = req.args.StringOr("id", "");
            double pos[3];
            if (!req.args.Get("pos").ToVec3(pos)) return r.Fail(ErrorCode::kBadArgs, "car.place: 'pos' must be [x, y, z]");
            const bool has_heading = req.args.Get("heading").is_number();
            const int code = PlaceCar(id, pos, req.args.NumberOr("heading", 0.0), has_heading);
            if (code == 1) return r.Fail(ErrorCode::kBadArgs, "car.place: no car '" + id + "' in the scene");
            if (code == 2) return r.Fail(ErrorCode::kNotReady, "car " + id + " is not ready (still being made)");
            Json res = Json::Object();
            res.Set("id", id);
            res.Set("placed", true);
            for (const SceneCarInfo& c : ListSceneCars()) {
                if (c.id == id && c.on_path && !c.brain) res.Set("on_path", true);  // the route puts it back at the next update
            }
            r.Done(std::move(res));
        },
        MakeSchema("cars", "put a car there now (live, transient: nothing is written; a car on a kinematic path goes back to its route)",
                   {Arg("id", ArgType::kString, true), Arg("pos", ArgType::kVec3, true), Arg("heading", ArgType::kNumber, false, "degrees, 0 along +Z")}, "{id, placed}"));

    RegisterCommand(
        "car.spawn",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            const std::string id = req.args.StringOr("id", "");
            if (!GoodId(id)) return r.Fail(ErrorCode::kBadArgs, "car.spawn: 'id' is letters, digits, _ and - (at most 32)");
            const bool has_model = req.args.Get("model").is_string(), has_profile = req.args.Get("profile").is_number();
            if (has_model == has_profile) return r.Fail(ErrorCode::kBadArgs, "car.spawn: give exactly one of 'model' and 'profile'");
            const double t = req.args.NumberOr("t", SceneTime());
            if (!(t >= 0.0)) return r.Fail(ErrorCode::kBadArgs, "car.spawn: 't' must be 0 or more");
            std::string text = "at " + FormatSceneTime(t) + " car " + id;
            if (id == "player") {
                if (!has_profile) return r.Fail(ErrorCode::kBadArgs, "car.spawn: the player's car takes only 'profile'");
                text += " profile " + std::to_string(req.args.IntOr("profile", 0));
            } else {
                std::string at;
                if (!PointWords(req.args.Get("at"), &at)) return r.Fail(ErrorCode::kBadArgs, "car.spawn: 'at' must be [x, y, z] or a spot word");
                if (has_model) {
                    const std::string model = req.args.StringOr("model", "");
                    if (!GoodWord(model)) return r.Fail(ErrorCode::kBadArgs, "car.spawn: 'model' is a vehicle name (no blanks)");
                    text += " spawn " + model;
                } else {
                    text += " spawn profile " + std::to_string(req.args.IntOr("profile", 0));
                }
                text += " at " + at;
                if (const Json* h = req.args.Find("heading"); h && !h->is_null()) {
                    std::string hw;
                    if (h->is_number()) {
                        hw = Num(h->AsNumber());
                    } else if (h->is_string() && GoodWord(h->AsString())) {
                        hw = h->AsString();
                    } else {
                        return r.Fail(ErrorCode::kBadArgs, "car.spawn: 'heading' is a number of degrees or a spot word");
                    }
                    text += " heading " + hw;
                }
                if (const Json* p = req.args.Find("paint"); p && p->is_number()) text += " paint " + std::to_string(p->AsInt());
            }
            const EditOutcome o = internal::InsertSceneLine(text, By(req));
            if (!o.ok) return r.Fail(o.code, o.message, o.details);
            Json res = InsertedJson(o, text);
            if (!req.wait_settled || id == "player") return r.Done(std::move(res));
            auto result = std::make_shared<Json>(std::move(res));
            const uint64_t since = internal::TickCount();
            r.Defer([id, result, since](const Responder& d) {
                if (internal::TickCount() < since + 3) return;
                for (const SceneCarInfo& c : ListSceneCars()) {
                    if (c.id == id && c.ready) {
                        result->Set("ready", true);
                        return d.Done(*result);
                    }
                }
            });
        },
        MakeSchema("cars", "insert `at <t> car <id> spawn ...` (the soft reload makes it real, no restart); with wait:settled, answers when the car is ready",
                   {Arg("id", ArgType::kString, true, "the scene's name for the car; \"player\" with profile"), Arg("model", ArgType::kString, false, "a vehicle name"),
                    Arg("profile", ArgType::kInt, false, "a garage slot, from 0"), Arg("at", ArgType::kAny, false, "[x, y, z] or a spot word"),
                    Arg("heading", ArgType::kAny, false, "degrees, or a spot word"), Arg("paint", ArgType::kInt, false, "0 to 9999: the same look every run"),
                    Arg("t", ArgType::kNumber, false, "default the scene's time now")},
                   "{line, lines, text, rev, hash}", true));

    RegisterCommand(
        "car.delete",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            const std::string id = req.args.StringOr("id", "");
            if (!GoodId(id) || id == "player") return r.Fail(ErrorCode::kBadArgs, "car.delete: 'id' is a scene car's name");
            const double t = req.args.NumberOr("t", SceneTime());
            if (!(t >= 0.0)) return r.Fail(ErrorCode::kBadArgs, "car.delete: 't' must be 0 or more");
            const std::string text = "at " + FormatSceneTime(t) + " car " + id + " delete";
            const EditOutcome o = internal::InsertSceneLine(text, By(req));
            if (!o.ok) return r.Fail(o.code, o.message, o.details);
            r.Done(InsertedJson(o, text));
        },
        MakeSchema("cars", "insert `at <t> car <id> delete`", {Arg("id", ArgType::kString, true), Arg("t", ArgType::kNumber, false, "default now")}, "{line, lines, text, rev, hash}", true));

    RegisterCommand(
        "car.path",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            const std::string id = req.args.StringOr("id", "");
            if (!GoodId(id) || id == "player") return r.Fail(ErrorCode::kBadArgs, "car.path: 'id' is a scene car's name");
            const std::string route = req.args.StringOr("route", "");
            const PathCheck rc = CheckRelativePath(route);
            if (!rc.ok || !GoodWord(route)) return r.Fail(ErrorCode::kBadArgs, "car.path: 'route' is a relative route file name (no blanks, no '..')");
            const double t = req.args.NumberOr("t", SceneTime());
            if (!(t >= 0.0)) return r.Fail(ErrorCode::kBadArgs, "car.path: 't' must be 0 or more");
            std::string text = "at " + FormatSceneTime(t) + " car " + id + " path " + rc.normalised;
            if (const Json* sp = req.args.Find("speed"); sp && !sp->is_null()) {
                std::string sw;
                if (sp->is_number()) sw = Num(sp->AsNumber());
                else if (sp->is_string() && GoodWord(sp->AsString())) sw = sp->AsString();
                else return r.Fail(ErrorCode::kBadArgs, "car.path: 'speed' is metres a second (12) or a factor (\"x1.5\")");
                text += " speed " + sw;
            }
            const std::string mode = req.args.StringOr("mode", "");
            if (!mode.empty()) {
                if (mode != "kinematic" && mode != "brain") return r.Fail(ErrorCode::kBadArgs, "car.path: 'mode' must be kinematic or brain");
                text += " mode " + mode;
            }
            const EditOutcome o = internal::InsertSceneLine(text, By(req));
            if (!o.ok) return r.Fail(o.code, o.message, o.details);
            r.Done(InsertedJson(o, text));
        },
        MakeSchema("cars", "insert `at <t> car <id> path <route> [speed ..] [mode ..]`: the car drives a route file (beside the scene or in its routes folder)",
                   {Arg("id", ArgType::kString, true), Arg("route", ArgType::kString, true), Arg("speed", ArgType::kAny, false, "m/s, or \"x1.5\" of the route's speeds"),
                    Arg("mode", ArgType::kString, false, "kinematic (default, exact) or brain (the game's driver; cannot be scrubbed)"), Arg("t", ArgType::kNumber, false)},
                   "{line, lines, text, rev, hash}", true));

    RegisterCommand(
        "car.path.record",
        [](const Request& req, const Responder& r) {
            if (g_rec.on) return r.Fail(ErrorCode::kNotReady, "a recording of car " + g_rec.id + " is running: car.path.record.stop first");
            const std::string id = req.args.StringOr("id", "");
            bool known = false;
            for (const SceneCarInfo& c : ListSceneCars()) known = known || (c.id == id && c.ready);
            if (!known) return r.Fail(ErrorCode::kNotReady, "car.path.record: car '" + id + "' is not in the scene or not ready");
            std::filesystem::path unused;
            std::string stored, err;
            if (!RouteWritePath(req.args.StringOr("name", ""), &unused, &stored, &err)) return r.Fail(ErrorCode::kRefused, err);
            const double every = req.args.NumberOr("every_m", 2.0);
            if (!(every >= 0.1 && every <= 100.0)) return r.Fail(ErrorCode::kBadArgs, "car.path.record: 'every_m' must be 0.1 to 100");
            g_rec = Recording{};
            g_rec.on = true;
            g_rec.id = id;
            g_rec.name = stored;
            g_rec.every = every;
            Json res = Json::Object();
            res.Set("recording", true);
            res.Set("id", id);
            res.Set("route", stored);
            r.Done(std::move(res));
        },
        MakeSchema("cars", "record a car's drive (the player's, or a scene car's): drive it, then car.path.record.stop", {Arg("id", ArgType::kString, true), Arg("name", ArgType::kString, true, "the route's name"),
                                                                                                                         Arg("every_m", ArgType::kNumber, false, "a sample every this many metres, default 2")},
                   "{recording, id, route}"));

    RegisterCommand(
        "car.path.record.stop",
        [](const Request&, const Responder& r) {
            if (!g_rec.on) return r.Fail(ErrorCode::kNotReady, "no recording is running");
            Recording rec = std::move(g_rec);
            g_rec = Recording{};
            if (rec.points.size() < 2) return r.Fail(ErrorCode::kFailed, "the recording has " + std::to_string(rec.points.size()) + " point(s): drive some metres first");
            const std::vector<WayPoint> reduced = ReduceRoute(rec.points, 0.25, 2.0);
            std::filesystem::path file;
            std::string stored, err;
            if (!RouteWritePath(rec.name, &file, &stored, &err)) return r.Fail(ErrorCode::kRefused, err);
            const std::string header = "recorded by the Director from car " + rec.id + ": " + std::to_string(rec.samples) + " samples, " + std::to_string(reduced.size()) +
                                       " points kept\nx,z,speed,y (metres, metres a second)";
            if (!internal::WriteTextFileAtomic(file, FormatRouteFile(reduced, header), &err)) return r.Fail(ErrorCode::kFailed, err);
            LARECOMP_APP_INFO("[dir] route {} written: {} points, {:.1f} m", internal::Utf8(file), reduced.size(), RouteLength(reduced));
            Json res = Json::Object();
            res.Set("route", stored);
            res.Set("points", static_cast<int64_t>(reduced.size()));
            res.Set("length_m", RouteLength(reduced));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "end the recording and write routes/<name>.route (x,z,speed,y, reduced to what the shape needs)", {}, "{route, points, length_m}"));

    RegisterCommand(
        "path.waypoint",
        [](const Request& req, const Responder& r) {
            double at[3];
            const Json& a = req.args.Get("at");
            if (a.is_string() && a.AsString() == "camera") {
                fc::Pose f;
                if (FreecamGetPose(&f)) at[0] = f.position.x, at[1] = f.position.y, at[2] = f.position.z;
                else {
                    const SnapPose p = internal::CurrentCamera().pose;
                    at[0] = p.pos[0], at[1] = p.pos[1], at[2] = p.pos[2];
                }
            } else if (!a.ToVec3(at)) {
                return r.Fail(ErrorCode::kBadArgs, "path.waypoint: 'at' must be \"camera\" or [x, y, z]");
            }
            const double speed = req.args.NumberOr("speed", 14.0);
            if (!(speed >= 0.5 && speed <= 120.0)) return r.Fail(ErrorCode::kBadArgs, "path.waypoint: 'speed' must be 0.5 to 120 metres a second");
            std::filesystem::path file;
            std::string stored, err;
            if (!RouteWritePath(req.args.StringOr("route", ""), &file, &stored, &err)) return r.Fail(ErrorCode::kRefused, err);
            std::vector<WayPoint> pts;
            std::string text;
            if (internal::ReadTextFile(file, &text)) {
                // (a route being made has one point after the first drop: it must read, or the second drop fails)
                Route route;
                std::string perr;
                if (!ParseRoute(text, &route, &perr, kMakingRoutePoints)) return r.Fail(ErrorCode::kFailed, "the route file does not read: " + perr);
                pts = ToWayPoints(route);
            }
            pts.push_back({at[0], at[1], at[2], speed});
            if (!internal::WriteTextFileAtomic(file, FormatRouteFile(pts, "waypoints dropped by the Director\nx,z,speed,y"), &err)) return r.Fail(ErrorCode::kFailed, err);
            Json res = Json::Object();
            res.Set("route", stored);
            res.Set("points", static_cast<int64_t>(pts.size()));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "append a waypoint to routes/<route>.route (made when it is not there): fly the camera, drop points", {Arg("route", ArgType::kString, true),
                                                                                                                                   Arg("at", ArgType::kTarget, true, "\"camera\" or [x, y, z]"),
                                                                                                                                   Arg("speed", ArgType::kNumber, false, "metres a second, default 14")},
                   "{route, points}"));

    RegisterCommand(
        "path.list",
        [](const Request&, const Responder& r) {
            const std::filesystem::path dir = internal::SceneFolder();
            Json routes = Json::Array();
            if (!dir.empty()) {
                std::error_code ec;
                int n = 0;
                for (const char* sub : {"routes", ""}) {
                    const std::filesystem::path d = sub[0] ? dir / sub : dir;
                    for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                        if (!it->is_regular_file(ec) || it->path().extension() != ".route" || ++n > 200) continue;
                        std::string text;
                        Route route;
                        std::string perr;
                        Json j = Json::Object();
                        j.Set("name", internal::Utf8(it->path().stem()));
                        if (internal::ReadTextFile(it->path(), &text) && ParseRoute(text, &route, &perr, kMakingRoutePoints)) {
                            const std::vector<WayPoint> pts = ToWayPoints(route);
                            j.Set("points", static_cast<int64_t>(pts.size()));
                            j.Set("length_m", RouteLength(pts));
                        } else {
                            j.Set("error", perr);
                        }
                        routes.Push(std::move(j));
                    }
                }
            }
            Json res = Json::Object();
            res.Set("routes", std::move(routes));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "the route files beside the scene and in its routes folder", {}, "{routes:[{name, points, length_m}]}"));

    RegisterCommand(
        "path.get",
        [](const Request& req, const Responder& r) {
            std::filesystem::path file;
            std::string err;
            if (!FindRoute(req.args.StringOr("route", ""), &file, &err)) return r.Fail(ErrorCode::kFailed, err);
            std::string text;
            Route route;
            if (!internal::ReadTextFile(file, &text) || !ParseRoute(text, &route, &err, kMakingRoutePoints)) return r.Fail(ErrorCode::kFailed, "the route file does not read: " + err);
            const std::vector<WayPoint> pts = ToWayPoints(route);
            Json res = Json::Object();
            res.Set("route", req.args.StringOr("route", ""));
            res.Set("points", PointsJson(pts));
            res.Set("length_m", RouteLength(pts));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "a route's points", {Arg("route", ArgType::kString, true)}, "{route, points:[[x, y, z, speed]], length_m}"));
}

const bool kRegistered = (RegisterCars(), true);

}  // namespace

namespace internal {

// Once an update: a recording takes a sample every `every_m` metres of the car's drive.
void TickCarHandlers() {
    if (!g_rec.on) return;
    for (const SceneCarInfo& c : ListSceneCars()) {
        if (c.id != g_rec.id || !c.ready) continue;
        const double now = WallSeconds();
        if (!g_rec.have_last) {
            g_rec.points.push_back({c.position[0], c.position[1], c.position[2], 14.0});
            std::copy(c.position, c.position + 3, g_rec.last);
            g_rec.t_last = now;
            g_rec.have_last = true;
            ++g_rec.samples;
            return;
        }
        const double d = std::hypot(c.position[0] - g_rec.last[0], c.position[2] - g_rec.last[2]);
        if (d < g_rec.every) return;
        const double dt = std::max(now - g_rec.t_last, 1e-3);
        const double speed = std::clamp(d / dt, 0.5, 120.0);  // CHECK: wall-clock speed; at a fixed step with a slow render it reads low
        if (g_rec.points.size() == 1) g_rec.points[0].speed = speed;
        g_rec.points.push_back({c.position[0], c.position[1], c.position[2], speed});
        std::copy(c.position, c.position + 3, g_rec.last);
        g_rec.t_last = now;
        ++g_rec.samples;
        return;
    }
}

}  // namespace internal

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
