// A check of the atlas's JSON answers (atlas_answers.h) that needs no game:
//   cmake --build <build dir> --target larecomp_atlas_answers_test, then run <build dir>/larecomp_atlas_answers_test (exit 0 = passed).
// Builds only with the command layer's json.cpp in the tree (package A); the target is added by CMake when that file exists.

#include "atlas_answers.h"

#ifndef LARECOMP_ATLAS_HAS_DIRECTOR
#include <cstdio>
int main() {
    std::puts("director.h is not in this tree: nothing to test");
    return 0;
}
#else

#include <cmath>
#include <cstdio>
#include <string>

using namespace mc::cinematic::atlas;
using mc::cinematic::director::Json;
using mc::cinematic::director::Snapshot;

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

Json Parse(const char* text) {
    Json j;
    std::string err;
    if (!Json::Parse(text, &j, &err)) std::printf("bad test json: %s\n", err.c_str());
    return j;
}

World MakeWorld() {
    World w;
    Node road;
    road.id = 0;
    road.kind = NodeKind::kRoad;
    road.name = "SYN_Test_St.";
    road.speed = 20.0f;
    road.rails = {0, 1};
    w.nodes.push_back(road);
    for (int r = 0; r < 2; ++r) {
        Rail rail;
        rail.id = r;
        rail.node = 0;
        rail.len = 30.0f;
        rail.cv = {static_cast<uint16_t>(3 * r), static_cast<uint16_t>(3 * r + 3)};
        for (int i = 0; i <= 6; ++i) {
            rail.pts.push_back(30.0f * r + 5.0f * i);
            rail.pts.push_back(0.0f);
            rail.pts.push_back(0.0f);
        }
        rail.dir = 1;
        if (r == 0) rail.next = {1};
        if (r == 1) rail.prev = {0};
        w.rails.push_back(rail);
    }
    for (int i = 0; i < 2; ++i) {
        Light l;
        l.i = i;
        l.pos[0] = 1.0f + 3 * i;
        l.pos[1] = 2.0f + 3 * i;
        l.pos[2] = 3.0f + 3 * i;
        l.intensity = 20.0f;
        l.spot = i == 1;
        l.cone = i == 0 ? 360.0f : 60.0f;
        w.lights.push_back(l);
    }
    w.garages = StaticGarages();
    return w;
}

}  // namespace

int main() {
    const World w = MakeWorld();
    const Index index(w);
    auto snap = std::make_shared<Snapshot>();
    snap->spots.push_back({"gate", {10.0, 0.0, 0.0}, 90.0, "the gate"});
    mc::cinematic::director::SnapCar car;
    car.id = "p1";
    car.pos[0] = 20.0;
    car.pos[1] = 0.5;
    car.pos[2] = 1.0;
    snap->cars.push_back(car);
    snap->camera.valid = true;
    snap->camera.pose.pos[1] = 2.0;
    snap->camera.pose.yaw = 90.0;
    snap->camera.pose.fov = 60.0;
    snap->camera.aspect = 2.0;

    Vec3 p;
    bool has_y = false;
    std::string err;
    Check(ResolvePosition(Parse("[1,2,3]"), snap.get(), &p, &has_y, &err) && has_y && Near(p.z, 3.0), "position: [x, y, z]");
    Check(ResolvePosition(Parse("[1,3]"), snap.get(), &p, &has_y, &err) && !has_y && Near(p.z, 3.0), "position: [x, z]");
    Check(ResolvePosition(Parse("{\"x\":4,\"z\":5}"), snap.get(), &p, &has_y, &err) && !has_y && Near(p.x, 4.0), "position: {x, z}");
    Check(ResolvePosition(Parse("\"car:p1\""), snap.get(), &p, &has_y, &err) && Near(p.x, 20.0) && has_y, "position: a scene car");
    Check(ResolvePosition(Parse("\"spot:gate\""), snap.get(), &p, &has_y, &err) && Near(p.x, 10.0), "position: a spot");
    // heading 90: forward = +X, right = (-cos 90, 0, sin 90) = +Z
    Check(ResolvePosition(Parse("\"spot:gate@2,1,5\""), snap.get(), &p, &has_y, &err) && Near(p.x, 15.0) && Near(p.y, 1.0) && Near(p.z, 2.0),
          "position: a spot with an offset in its heading frame (right = +Z at heading 90)");
    Check(!ResolvePosition(Parse("\"spot:nope\""), snap.get(), &p, &has_y, &err) && err.find("nope") != std::string::npos, "position: an unknown spot is named");
    Check(!ResolvePosition(Parse("\"car:zz\""), snap.get(), &p, &has_y, &err), "position: an unknown car");
    Check(!ResolvePosition(Parse("[1]"), nullptr, &p, &has_y, &err), "position: a bad array");

    Json res;
    Check(AnswerLights(index, Parse("{\"near\":[2,2,3],\"r\":20}"), snap.get(), &res, &err) && res.Get("in_radius").AsInt() == 2 && res.Get("lights").size() == 2,
          "lights: two in 20 m");
    Check(Near(res.Get("lights").at(0).Get("dist").AsNumber(), 1.0) && Near(res.Get("lights").at(0).Get("reach_m").AsNumber(), 44.7, 0.05) &&
              res.Get("lights").at(1).Get("type").AsString() == "spot",
          "lights: closest first, reach, type");
    Check(!AnswerLights(index, Parse("{\"r\":20}"), snap.get(), &res, &err), "lights: near is needed");
    Check(!AnswerLights(index, Parse("{\"near\":[0,0,0],\"kind\":\"x\"}"), snap.get(), &res, &err), "lights: a bad kind");

    Check(AnswerRoad(index, Parse("{\"pos\":[15,0,3]}"), snap.get(), &res, &err) && res.Get("nearest").size() >= 1, "road: answers");
    const Json& r0 = res.Get("nearest").at(0);
    Check(r0.Get("rail").AsInt() == 0 && Near(r0.Get("dist_m").AsNumber(), 3.0) && r0.Get("name").AsString() == "SYN_Test_St." && Near(r0.Get("heading_deg").AsNumber(), 90.0, 0.5),
          "road: nearest rail, name, heading");
    Check(r0.Get("next").size() == 1 && r0.Get("next").at(0).AsInt() == 1 && res.Get("on_road").AsBool(true) == true, "road: the next rails and on_road");
    Check(AnswerRoad(index, Parse("{\"pos\":\"car:p1\"}"), snap.get(), &res, &err), "road: a car as the position");

    Check(AnswerRoute(index, Parse("{\"from\":[2,0,1],\"to\":[58,0,-1]}"), snap.get(), &res, &err) && res.Get("rails").size() == 2 && Near(res.Get("length_m").AsNumber(), 56.0, 1.0),
          "route: across the join");
    Check(res.Get("points").at(0).size() == 4 && res.Get("names").at(0).AsString() == "SYN_Test_St.", "route: points are x, z, speed, y");
    Check(!AnswerRoute(index, Parse("{\"from\":[58,0,0],\"to\":[2,0,0]}"), snap.get(), &res, &err) && !err.empty(), "route: no way backwards is an error");
    Check(AnswerRoute(index, Parse("{\"from\":{\"rail\":0,\"t\":0.5},\"to\":{\"rail\":1,\"t\":0.5},\"speed\":10}"), snap.get(), &res, &err), "route: ends as rail fractions");
    Check(!AnswerRoute(index, Parse("{\"from\":[0,0,0]}"), snap.get(), &res, &err), "route: to is needed");

    AnswerGarages(w, &res);
    Check(res.Get("garages").size() == 3 && res.Get("garages").at(0).Get("name").AsString() == "hollywood", "garages");

    CameraPose pose;
    Check(ResolveCamera(nullptr, snap.get(), &pose, &err) && Near(pose.yaw, 90.0) && Near(pose.aspect, 2.0), "camera: the snapshot's");
    Json over = Parse("{\"pos\":[0,2,0],\"yaw\":0,\"fov\":40}");
    Check(ResolveCamera(&over, snap.get(), &pose, &err) && Near(pose.yaw, 0.0) && Near(pose.fov, 40.0), "camera: an override");
    Snapshot empty;
    Check(!ResolveCamera(nullptr, &empty, &pose, &err), "camera: none yet");
    CameraPose looks;
    looks.pos = {0, 2, 0};
    looks.yaw = 90;
    looks.fov = 60;
    looks.aspect = 2.0;
    AnswerProjection(looks, {10, 2, 0}, 1.0, &res);
    Check(res.Get("in_frame").AsBool() && Near(res.Get("screen").at(0).AsNumber(), 0.5) && Near(res.Get("dist_m").AsNumber(), 10.0), "visible: a point ahead is the picture's centre");
    AnswerProjection(looks, {-10, 2, 0}, 1.0, &res);
    Check(!res.Get("in_front").AsBool(true) && res.Get("screen").is_null(), "visible: behind the camera");

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}

#endif
