#ifndef REXGLUE_HAS_XEO3_TARGET
//
// What the Director's PANEL needs of the game beyond the commands every client has (DIRECTOR-DESIGN.md section 3; src/mc_engine/ui/director_panel.cpp):
//
//   path.set           writes a whole route file again: `routes/<name>.route` gets the points the panel edited (a waypoint moved, deleted, a speed changed, the route
//                      smoothed). path.waypoint only appends and car.path.record.stop writes a recording once, so a route that is edited needs this. The same rules as the
//                      other route writers: a relative name with no blanks, under the scene's routes folder, written atomically.
//   the camera's path  a snapshot contributor: the scene's camera track evaluated over the scene's length, one sample every 0.25 s or so, for the panel to draw in the world
//                      (Snapshot::cam_path). The cars a rig follows are taken where they are NOW (a chase camera over a car that drives is drawn behind the car's place at
//                      this moment: exact for a parked car, a tripod, a `pos` key and an orbit round a parked car; CHECK: a path of a moving car's rig is only its shape
//                      at the car's present place).
//
// Both are inert unless the Director is on (the commands need it; the contributor runs from its snapshot).
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "../camera.h"
#include "settings/immortal.h"
#include "../cars.h"
#include "../scene_runtime.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"
#include "../path_follow.h"

namespace mc::cinematic::director {

namespace {

namespace cam = mc::cinematic::camera;
using internal::Arg;
using internal::MakeSchema;

// A route name's word: no blanks, quotes or comment marks (the other route writers' rule).
bool GoodWord(const std::string& w) {
    return !w.empty() && w.find_first_of(" \t\"#") == std::string::npos;
}

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

constexpr size_t kMinRoutePoints = 2;
constexpr size_t kMaxRoutePoints = 5000;

void RegisterPanelCommands() {
    RegisterCommand(
        "path.set",
        [](const Request& req, const Responder& r) {
            const Json& pts = req.args.Get("points");
            if (!pts.is_array() || pts.size() < kMinRoutePoints || pts.size() > kMaxRoutePoints) {
                return r.Fail(ErrorCode::kBadArgs, "path.set: 'points' is a list of 2 to 5000 points [x, y, z, speed]");
            }
            std::vector<WayPoint> wp;
            for (size_t i = 0; i < pts.size(); ++i) {
                const Json& p = pts.at(i);
                if (!p.is_array() || p.size() < 3 || !p.at(0).is_number() || !p.at(1).is_number() || !p.at(2).is_number() || (p.size() > 3 && !p.at(3).is_number())) {
                    return r.Fail(ErrorCode::kBadArgs, "path.set: point " + std::to_string(i + 1) + " is not [x, y, z] or [x, y, z, speed]");
                }
                WayPoint w;
                w.x = p.at(0).AsNumber(), w.y = p.at(1).AsNumber(), w.z = p.at(2).AsNumber();
                w.speed = p.size() > 3 ? p.at(3).AsNumber() : 14.0;
                if (!std::isfinite(w.x + w.y + w.z + w.speed)) return r.Fail(ErrorCode::kBadArgs, "path.set: point " + std::to_string(i + 1) + " is not a number");
                if (!(w.speed >= 0.5 && w.speed <= 120.0)) return r.Fail(ErrorCode::kBadArgs, "path.set: point " + std::to_string(i + 1) + ": 'speed' must be 0.5 to 120 metres a second");
                wp.push_back(w);
            }
            std::filesystem::path file;
            std::string stored, err;
            if (!RouteWritePath(req.args.StringOr("route", ""), &file, &stored, &err)) return r.Fail(ErrorCode::kRefused, err);
            if (!internal::WriteTextFileAtomic(file, FormatRouteFile(wp, "written by the Director panel\nx,z,speed,y (metres, metres a second)"), &err)) return r.Fail(ErrorCode::kFailed, err);
            Json res = Json::Object();
            res.Set("route", stored);
            res.Set("points", static_cast<int64_t>(wp.size()));
            res.Set("length_m", RouteLength(wp));
            r.Done(std::move(res));
        },
        MakeSchema("cars", "write a whole route file again: routes/<route>.route gets exactly these points (the panel's edits of a route)",
                   {Arg("route", ArgType::kString, true, "the route's name"), Arg("points", ArgType::kArray, true, "[[x, y, z, speed], ...], 2 to 5000 points, speed 0.5 to 120 m/s")},
                   "{route, points, length_m}"));
}

const bool kRegistered = (RegisterPanelCommands(), true);

// ---------------------------------------------------------------------------------------------------------------- the camera's path

class PathCars final : public cam::CarSource {
  public:
    bool Get(const std::string& id, cam::CarPose* out) override {
        CarPose p;
        if (!GetCarPose(id, &p)) return false;
        out->position = {p.position[0], p.position[1], p.position[2]};
        out->frame.right = {p.right[0], p.right[1], p.right[2]};
        out->frame.up = {p.up[0], p.up[1], p.up[2]};
        out->frame.forward = {p.forward[0], p.forward[1], p.forward[2]};
        return true;
    }
};

struct PathCache {
    const Scene* scene = nullptr;
    int64_t rev = -1;
    std::chrono::steady_clock::time_point at{};
    std::vector<SnapCamSample> samples;
};

constexpr double kRefreshSeconds = 1.0;  // the cars move: the path of a rig over them is drawn again this often
constexpr size_t kMaxSamples = 600;

void ContributeCameraPath(Snapshot& snap) {
    static PathCache& cache = larecomp::Leak<PathCache>();
    const Scene* scene = LoadedScene();
    if (!scene) {
        cache.scene = nullptr;
        cache.samples.clear();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = cache.scene == scene && cache.rev == snap.rev && std::chrono::duration<double>(now - cache.at).count() < kRefreshSeconds;
    if (!fresh) {
        cache.scene = scene;
        cache.rev = snap.rev;
        cache.at = now;
        cache.samples.clear();
        cam::BuildResult built = cam::BuildTrack(*scene);
        if (built.Ok() && !built.track.Empty()) {
            double end = scene->Duration() > 0.0 ? scene->Duration() : scene->LastKeyEnd();
            end = std::clamp(end, 1.0, 900.0);
            const double step = std::max(0.25, end / static_cast<double>(kMaxSamples));
            cam::Rig rig;
            PathCars cars;
            for (double t = 0.0; t <= end + 1e-9 && cache.samples.size() < kMaxSamples; t += step) {
                const cam::View v = rig.Evaluate(built.track, t, cars);
                if (!v.active) continue;  // the game's own camera: no path there
                SnapCamSample s;
                s.t = t;
                s.pos[0] = v.position.x, s.pos[1] = v.position.y, s.pos[2] = v.position.z;
                s.cut = v.cut;
                cache.samples.push_back(s);
            }
        }
    }
    snap.cam_path = cache.samples;
}

const bool kContributor = (AddSnapshotContributor(&ContributeCameraPath), true);

}  // namespace

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
