// Check of the Director panel's pure parts: the timeline (director_timeline.h), the world gizmos (director_gizmos.h) and the panel's logic (director_panel_core.h). A program of its
// own: it needs no game, no ReXGlue and no ImGui, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_director_panel_test      (CMakeLists.txt)
//   <build dir>/larecomp_director_panel_test
//
// What it proves, beyond each function's own cases: every command the panel builds is applied to a real scene text by the REAL SceneEdit (the ops of `scene.edit`) and the new text
// is what the user meant (a dragged key moved only its time word, a duplicate kept the original, an orbit's sweep wrote `az a..b over 8`, a car's drop rewrote only its `at`);
// the keys of a real parsed scene become the lanes and markers; the projection of the gizmos agrees with the Director's own camera maths and with the present's frame
// (frame_fit.h) in a 16:9, an ultrawide and a tall window.
//
// The compile by hand (the msvc-target clang of labuild; ws2_32 on Windows) is the target's own source list in CMakeLists.txt.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "director_gizmos.h"
#include "director_panel_core.h"
#include "director_text.h"
#include "director_timeline.h"
#include "mc_engine/cinematic/director/director_math.h"
#include "mc_engine/cinematic/director/scene_edit.h"
#include "mc_engine/cinematic/scene.h"
#include "mc_engine/display/frame_fit.h"

using namespace mcla::ui::director;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}
void CheckNum(double got, double want, const std::string& what, double tol = 1e-6) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}
void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s:\n  got  [%s]\n  want [%s]\n", what.c_str(), got.c_str(), want.c_str());
    }
}

// ---------------------------------------------------------------------------------------------------------------- a real scene

const char* kScene =
    "scene demo fps 60 step 1/60\n"                                                 // 1
    "world hour 16.5 hold weather nice traffic off peds off\n"                      // 2
    "at 0 hud off\n"                                                                // 3
    "at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62   # the hero\n"  // 4
    "at 6 car p1 path run.route speed 12\n"                                         // 5
    "cam 0 pos 800 20 -790 yaw 60 pitch -5 fov 50 ease inout\n"                    // 6
    "cam 4 pos 810 12 -800 look 819.6 17.6 -808.4 fov 35\n"                         // 7
    "cam 6 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50   # follow\n"         // 8
    "cam 9 rig orbit car:p1 r 8 el 10 az 20..200 over 6\n"                          // 9
    "cut 9\n"                                                                       // 10
    "at 6 capture fps 60 name demo sink png\n"                                      // 11
    "at 12 capture off\n"                                                           // 12
    "end 14.5\n";                                                                   // 13

struct World {
    std::string text = kScene;
    mc::cinematic::ParseResult parsed;
    std::vector<dir::SnapEvent> events;
    int64_t rev = 7;
    SceneText scene_text;
    World() {
        parsed = mc::cinematic::ParseScene(text);
        events = dir::TimelineEvents(parsed.scene, text);
        scene_text.Set(rev, text);
    }
    EditContext Ctx() const { return EditContext{rev, &scene_text}; }
};

// The Cmd as the Director would take it: the `scene.edit` ops applied to the text by the real SceneEdit.
dir::EditResult ApplyEdit(const World& w, const Cmd& c) {
    std::vector<dir::EditOp> ops;
    std::string error;
    dir::EditResult bad;
    if (c.cmd != "scene.edit") {
        bad.ok = false;
        bad.message = "not a scene.edit: " + c.cmd;
        return bad;
    }
    for (const dir::Json& j : c.args.Get("ops").items()) {
        dir::EditOp op;
        if (!dir::EditOpFromJson(j, &op, &error)) {
            bad.ok = false;
            bad.message = error;
            return bad;
        }
        ops.push_back(std::move(op));
    }
    return dir::SceneEdit::Apply(w.text, ops, static_cast<int64_t>(c.args.NumberOr("base_rev", -1)));
}

std::string LineOf(const std::string& text, int n) {
    return SplitLines(text).at(static_cast<size_t>(n) - 1);
}

int MarkerLine(const Layout& l, int line) {
    return FindMarkerByLine(l, line);
}

// ---------------------------------------------------------------------------------------------------------------- gizmos

dir::SnapCamera MakeCamera(double x, double y, double z, double yaw, double pitch, double fov, double aspect) {
    dir::SnapCamera c;
    c.valid = true;
    c.mode = "freecam";
    c.pose.pos[0] = x, c.pose.pos[1] = y, c.pose.pos[2] = z;
    c.pose.yaw = yaw, c.pose.pitch = pitch, c.pose.fov = fov;
    c.aspect = aspect;
    return c;
}

void TestPicture() {
    const RectF a = PictureRect(1920, 1080, 16.0 / 9.0);
    CheckNum(a.x, 0, "a 16:9 window is the picture: x");
    CheckNum(a.w, 1920, "a 16:9 window is the picture: w");
    CheckNum(a.h, 1080, "a 16:9 window is the picture: h");
    // an ultrawide picture in a 16:9 window: bars above and below
    const RectF u = PictureRect(1920, 1080, 64.0 / 27.0);
    CheckNum(u.w, 1920, "a 21:9 picture fills the window's width");
    CheckNum(u.h, 1920.0 / (64.0 / 27.0), "... and is shorter");
    CheckNum(u.y, (1080 - u.h) / 2, "... centred");
    // a 4:3 picture in a 16:9 window: bars at the sides
    const RectF f = PictureRect(1920, 1080, 4.0 / 3.0);
    CheckNum(f.h, 1080, "a 4:3 picture fills the window's height");
    CheckNum(f.w, 1440, "... and is narrower");
    CheckNum(f.x, 240, "... centred");
    Check(PictureRect(0, 1080, 1.7).Empty(), "no window, no picture");
    // the same rectangle as the present's, to the pixel (frame_fit.h FrameOf rounds to whole pixels)
    for (const double aspect : {16.0 / 9.0, 64.0 / 27.0, 4.0 / 3.0, 16.0 / 10.0, 32.0 / 9.0, 9.0 / 16.0}) {
        for (const auto& wh : {std::pair<uint32_t, uint32_t>{1920, 1080}, {2560, 1080}, {1280, 720}, {1080, 1920}, {3440, 1440}}) {
            const mc::frame::Rect r = mc::frame::FrameOf(wh.first, wh.second, aspect);
            const RectF p = PictureRect(static_cast<float>(wh.first), static_cast<float>(wh.second), aspect);
            Check(std::fabs(p.x - r.x) <= 1.0 && std::fabs(p.y - r.y) <= 1.0 && std::fabs(p.w - r.w) <= 1.0 && std::fabs(p.h - r.h) <= 1.0,
                  "the picture rectangle is the present's frame within a pixel: " + std::to_string(wh.first) + "x" + std::to_string(wh.second) + " at " + std::to_string(aspect));
        }
    }
    CheckNum(EffectiveFov(50.0, 16.0 / 9.0), 50.0, "a 16:9 picture keeps the fov");
    CheckNum(EffectiveFov(50.0, 64.0 / 27.0), 50.0, "a wide picture keeps the vertical fov (Hor+)");
    Check(EffectiveFov(50.0, 4.0 / 3.0) > 50.0 + 5.0, "a tall picture widens the vertical fov (Vert+)");
    CheckNum(EffectiveFov(50.0, 4.0 / 3.0), mc::frame::VertPlusFov(50.0, 4.0 / 3.0), "... as the renderer does");
}

void TestProjector() {
    // a camera at the origin looking along +Z (yaw 0), 16:9, 1920x1080
    const dir::SnapCamera cam = MakeCamera(0, 0, 0, 0, 0, 50, 16.0 / 9.0);
    const Projector p = Projector::Make(cam, 1920, 1080);
    Check(p.valid, "a camera makes a projector");
    Vec2f px;
    double depth = 0;
    const double ahead[3] = {0, 0, 10};
    Check(p.Project(ahead, &px, &depth), "a point ahead is in front");
    CheckNum(px.x, 960, "straight ahead is the middle of the picture: x", 1e-3);
    CheckNum(px.y, 540, "... y", 1e-3);
    CheckNum(depth, 10, "the depth is the distance along the view");
    // up: the point at the edge of the vertical fov is the top of the picture
    const double top[3] = {0, 10 * std::tan(25.0 * 3.14159265358979 / 180.0), 10};
    Check(p.Project(top, &px), "a point at the top edge");
    CheckNum(px.y, 0, "the vertical fov's edge is the picture's top", 1e-2);
    // the camera's right at yaw 0 is -X (the world is right-handed): a point at -X is to the right
    const double right_pt[3] = {-1, 0, 10};
    Check(p.Project(right_pt, &px), "a point to the right");
    Check(px.x > 960, "yaw 0 looks along +Z, so -X is to the right");
    const double behind[3] = {0, 0, -5};
    Check(!p.Project(behind, &px), "a point behind the camera is not drawn");
    // the Director's own maths agree (view_proj of director_math.h)
    for (const double aspect : {16.0 / 9.0, 64.0 / 27.0}) {
        dir::SnapCamera c = MakeCamera(819.6, 19.0, -800.0, 80.0, -4.0, 40.0, aspect);
        dir::ViewProjection(c.pose, aspect, 0.1, 10000.0, c.view_proj);
        const Projector q = Projector::Make(c, 1920, 1080);
        const double pts[4][3] = {{830, 17, -790}, {825, 22, -795}, {840, 15, -810}, {822, 19.5, -797}};
        for (const auto& pt : pts) {
            double nx, ny, d, w;
            const bool ok = dir::ProjectPoint(c.view_proj, pt, &nx, &ny, &d, &w);
            Vec2f got;
            const bool ok2 = q.Project(pt, &got);
            Check(ok == ok2, "in front agrees with ProjectPoint");
            if (ok && ok2) {
                const float ex = static_cast<float>(q.picture.x + (nx * 0.5 + 0.5) * q.picture.w);
                const float ey = static_cast<float>(q.picture.y + (0.5 - ny * 0.5) * q.picture.h);
                CheckNum(got.x, ex, "x agrees with the Director's view-projection (aspect " + std::to_string(aspect) + ")", 0.05);
                CheckNum(got.y, ey, "y agrees", 0.05);
            }
        }
    }
    // the ray through a pixel comes back to that pixel
    dir::SnapCamera c = MakeCamera(10, 5, 20, 33, -12, 47, 16.0 / 9.0);
    c.pose.roll = 7;
    const Projector q = Projector::Make(c, 2560, 1440);
    for (const Vec2f pix : {Vec2f{100, 200}, Vec2f{1280, 720}, Vec2f{2400, 1300}, Vec2f{640, 1000}}) {
        const Ray r = q.PixelRay(pix);
        const double world[3] = {r.origin[0] + r.dir[0] * 25, r.origin[1] + r.dir[1] * 25, r.origin[2] + r.dir[2] * 25};
        Vec2f back;
        Check(q.Project(world, &back), "a point on the ray is in front");
        CheckNum(back.x, pix.x, "ray -> pixel: x", 0.01);
        CheckNum(back.y, pix.y, "ray -> pixel: y", 0.01);
    }
    // the ground under a pixel
    const dir::SnapCamera low = MakeCamera(0, 10, 0, 0, -45, 50, 16.0 / 9.0);
    const Projector lp = Projector::Make(low, 1920, 1080);
    double hit[3];
    Check(RayPlaneY(lp.PixelRay({960, 540}), 0.0, hit), "the middle pixel of a camera looking down at 45 degrees meets the ground");
    CheckNum(hit[1], 0.0, "on the plane");
    CheckNum(hit[2], 10.0, "10 m ahead (45 degrees from 10 m up)", 1e-6);
    Check(!RayPlaneY(lp.PixelRay({960, 5}), 0.0, hit) || hit[2] > 10.0, "a pixel above the horizon does not meet the ground ahead");
    double u, v;
    Check(lp.ToUV({960, 540}, &u, &v) && std::fabs(u - 0.5) < 1e-6 && std::fabs(v - 0.5) < 1e-6, "the middle is u 0.5, v 0.5");
    // an ultrawide picture in a 16:9 window: the picture is a band, UV is the picture's
    const Projector uw = Projector::Make(MakeCamera(0, 0, 0, 0, 0, 50, 64.0 / 27.0), 1920, 1080);
    Check(!uw.ToUV({960, 10}, &u, &v), "a pixel in the bar above the picture is outside it");
    Check(uw.ToUV({960, 540}, &u, &v) && std::fabs(v - 0.5) < 1e-6, "the middle is still the middle");
    // an unknown camera
    dir::SnapCamera none;
    Check(!Projector::Make(none, 1920, 1080).valid, "no camera, no projector");
}

void TestSegments() {
    const Projector p = Projector::Make(MakeCamera(0, 0, 0, 0, 0, 50, 16.0 / 9.0), 1920, 1080);
    Vec2f a, b;
    const double a1[3] = {0, 0, 5}, b1[3] = {0, 0, 20};
    Check(p.ProjectSegment(a1, b1, &a, &b), "a segment ahead");
    CheckNum(a.x, 960, "projects on the middle", 1e-3);
    // one end behind the camera: cut at the near plane, still drawn
    const double a2[3] = {-1, 0, -10}, b2[3] = {-1, 0, 10};
    Check(p.ProjectSegment(a2, b2, &a, &b), "a segment crossing the camera plane is cut, not dropped");
    Check(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y), "the cut ends are finite");
    const double a3[3] = {0, 0, -3}, b3[3] = {0, 0, -9};
    Check(!p.ProjectSegment(a3, b3, &a, &b), "a segment wholly behind is not drawn");
    // the cut end lies on the line (the same x for a vertical line in the camera's frame)
    const double a4[3] = {-2, 0, -4}, b4[3] = {-2, 0, 30};
    Check(p.ProjectSegment(a4, b4, &a, &b), "cut");
    CheckNum(b.x, p.picture.x + (-(-2.0) / (30.0 * p.tan_half_fov * p.aspect) * 0.5 + 0.5) * p.picture.w, "the far end is where the point projects", 1e-2);
}

void TestGizmoPrimitives() {
    CheckEq(std::to_string(SpeedColor(0) & 0xFF) + "," + std::to_string((SpeedColor(0) >> 8) & 0xFF), "58,140", "a standing car is blue");
    Check((SpeedColor(100) & 0xFF) > 200 && ((SpeedColor(100) >> 8) & 0xFF) < 120, "a very fast one is red");
    Check(SpeedColor(12) != SpeedColor(24), "the colour follows the speed");
    CheckNum(static_cast<double>(SpeedColor(30) >> 24), 255, "opaque");
    CheckNum(static_cast<double>(WithAlpha(SpeedColor(30), 40) >> 24), 40, "WithAlpha");

    const Projector p = Projector::Make(MakeCamera(0, 20, -30, 0, -25, 60, 16.0 / 9.0), 1920, 1080);
    std::vector<std::vector<double>> pts = {{0, 0, 0, 5}, {0, 0, 40, 15}, {10, 0, 80, 30}};
    Gizmos g;
    AddRoute(p, pts, 100, "run", true, &g, 60.0f);
    Check(g.lines.size() >= 2, "a route is a polyline");
    Check(g.dots.size() == 3 && g.dots[0].id == 100 && g.dots[2].id == 102, "every point is a handle with an id");
    Check(!g.labels.empty() && g.labels.back().text == "run", "the name is a label at the first point");
    {
        std::vector<uint32_t> colors;
        for (const Line2& ln : g.lines) {
            if (std::find(colors.begin(), colors.end(), ln.color) == colors.end()) colors.push_back(ln.color);
        }
        Check(colors.size() >= 2, "segments are coloured by their speed");
    }
    Gizmos plain;
    AddRoute(p, pts, -1, "", false, &plain);
    Check(plain.dots.empty() && plain.labels.empty(), "no handles, no label when not asked");
    // nearest handle
    Check(NearestDot(g.dots, g.dots[1].p, 6.0f) == 1, "the handle under the pointer");
    Check(NearestDot(g.dots, {g.dots[1].p.x + 400, g.dots[1].p.y + 300}, 6.0f) == -1, "none far away");
    std::vector<Dot2> mixed = {{{10, 10}, 8, 0, true, -1}, {{12, 10}, 8, 0, true, 5}};
    Check(NearestDot(mixed, {11, 10}, 6.0f) == 1, "a dot with no id is not a handle");

    // a car
    dir::SnapCar car;
    car.id = "p1";
    car.model = "vp_lam_murcielago_06";
    car.pos[0] = 0, car.pos[1] = 0, car.pos[2] = 10;
    car.ready = true;
    Gizmos cg;
    AddCar(p, car, true, 3, &cg);
    Check(cg.dots.size() == 1 && !cg.dots[0].filled, "a car is a ring");
    Check(cg.dots[0].id == 3 && NearestDot(cg.dots, cg.dots[0].p, 2.0f) == 0, "the ring is a handle that names the car");
    Check(cg.labels.size() == 1 && cg.labels[0].text.find("p1") == 0, "... with its id");
    car.ready = false;
    Gizmos loading;
    AddCar(p, car, false, -1, &loading);
    Check(loading.labels[0].text.find("loading") != std::string::npos, "a car that is not made says so");
    car.pos[2] = -500;
    Gizmos behind;
    AddCar(p, car, false, -1, &behind);
    Check(behind.dots.empty(), "a car behind the camera has no mark");

    // a frustum is 8 lines (4 edges, 4 rim sides) in front of the camera
    dir::SnapPose key;
    key.pos[2] = 10;
    key.yaw = 180;  // looking back at the camera along -Z
    key.fov = 40;
    Gizmos fr;
    AddFrustum(p, key, 16.0 / 9.0, 4.0, Rgba(255, 255, 255), true, &fr);
    Check(fr.lines.size() == 8, "a frustum has eight lines");
    Check(fr.lines[0].width > 2.0f, "a selected one is bold");

    // the camera path: a cut breaks the line
    std::vector<dir::SnapCamSample> samples(3);
    samples[0].pos[2] = 10;
    samples[1].pos[2] = 20;
    samples[2].pos[2] = 30;
    Gizmos path;
    AddCameraPath(p, samples, Rgba(1, 2, 3), &path);
    Check(path.lines.size() == 2, "a path of three samples is two lines");
    samples[2].cut = true;
    Gizmos cut;
    AddCameraPath(p, samples, Rgba(1, 2, 3), &cut);
    Check(cut.lines.size() == 1, "no line across a cut");

    // the spline through keys passes through them
    std::vector<dir::SnapCamSample> keys(3);
    keys[0].t = 0;
    keys[1].t = 4;
    keys[2].t = 8;
    keys[1].pos[0] = 10;
    keys[2].pos[0] = 0;
    keys[2].pos[2] = 20;
    const std::vector<dir::SnapCamSample> sp = SplineThrough(keys, 8);
    Check(sp.size() == 2 * 8 + 1, "per_segment samples a segment and the last key");
    CheckNum(sp[8].pos[0], 10, "the spline passes through the middle key", 1e-9);
    CheckNum(sp.back().pos[2], 20, "... and ends on the last", 1e-9);
    CheckNum(sp[4].t, 2.0, "times are interpolated");
}

// ---------------------------------------------------------------------------------------------------------------- timeline

void TestLayout() {
    const World w;
    Check(w.parsed.Ok(), "the sample scene parses (" + std::to_string(w.parsed.ErrorCount()) + " errors)");
    const Layout l = BuildLayout(w.events, w.parsed.scene.Duration());
    // lanes
    Check(l.lanes.size() >= 4, "camera, the car, world and capture lanes");
    CheckEq(l.lanes[0].title, "CAMERA", "the camera lane comes first");
    CheckEq(l.lanes[1].title, "CAR p1", "then a lane per car");
    bool world = false, capture = false, hud = false;
    for (const Lane& ln : l.lanes) {
        world = world || ln.kind == LaneKind::kWorld;
        capture = capture || ln.kind == LaneKind::kCapture;
        hud = hud || ln.kind == LaneKind::kHud;
    }
    Check(world && capture && hud, "world, hud and capture lanes");
    CheckNum(l.end, 14.5, "the scene's end");
    // markers
    const int m6 = MarkerLine(l, 6), m7 = MarkerLine(l, 7), m8 = MarkerLine(l, 8), m9 = MarkerLine(l, 9), m10 = MarkerLine(l, 10);
    Check(m6 >= 0 && m7 >= 0 && m8 >= 0 && m9 >= 0 && m10 >= 0, "every camera key and the cut have a marker");
    CheckEq(l.markers[static_cast<size_t>(m6)].label, "pos fov 50", "a pos key's label");
    CheckEq(l.markers[static_cast<size_t>(m6)].ease, "inout", "its ease");
    CheckEq(l.markers[static_cast<size_t>(m7)].label, "pos look fov 35", "a look key's label");
    CheckEq(l.markers[static_cast<size_t>(m8)].label, "rig chase p1", "a rig key's label");
    CheckEq(l.markers[static_cast<size_t>(m9)].label, "rig orbit p1", "an orbit rig's label");
    Check(l.markers[static_cast<size_t>(m10)].is_cut, "the cut is a cut");
    Check(l.markers[static_cast<size_t>(m6)].lane == 0 && l.markers[static_cast<size_t>(m10)].lane == 0, "keys and cuts share the camera lane");
    // spans: a rig runs to the next camera key
    Check(l.markers[static_cast<size_t>(m8)].has_span, "a rig has a bar");
    CheckNum(l.markers[static_cast<size_t>(m8)].span_end, 9.0, "the chase runs to the orbit", 1e-9);
    CheckNum(l.markers[static_cast<size_t>(m9)].span_end, 14.5, "the last rig runs to the end");
    Check(!l.markers[static_cast<size_t>(m6)].has_span, "a pos key has no bar");
    // the car's lane
    const int spawn = MarkerLine(l, 4), path = MarkerLine(l, 5);
    CheckEq(l.markers[static_cast<size_t>(spawn)].label, "spawn vp_lam_murcielago_06", "the spawn's label");
    CheckEq(l.markers[static_cast<size_t>(path)].label, "path run.route", "the path's label");
    Check(l.markers[static_cast<size_t>(path)].has_span && l.markers[static_cast<size_t>(path)].span_end == 14.5, "a path runs to the end when nothing stops it");
    Check(l.markers[static_cast<size_t>(spawn)].lane == 1 && l.markers[static_cast<size_t>(path)].lane == 1, "on the car's lane");
    // capture labels
    const int c11 = MarkerLine(l, 11), c12 = MarkerLine(l, 12);
    CheckEq(l.markers[static_cast<size_t>(c11)].label, "take 60 fps", "the take's in");
    CheckEq(l.markers[static_cast<size_t>(c12)].label, "take off", "the take's out");
    // order inside a lane
    for (size_t i = 1; i < l.markers.size(); ++i) {
        if (l.markers[i].lane == l.markers[i - 1].lane) Check(l.markers[i].t >= l.markers[i - 1].t, "markers are in time order inside a lane");
    }
    // no events: the camera lane is still there
    const Layout empty = BuildLayout({}, -1.0);
    Check(empty.lanes.size() == 1 && empty.lanes[0].kind == LaneKind::kCamera, "an empty scene has the camera lane");
    CheckNum(empty.end, 5.0, "and a 5 s view");
    // a path that is stopped
    std::vector<dir::SnapEvent> ev = w.events;
    dir::SnapEvent stop;
    stop.line = 20;
    stop.t = 8;
    stop.track = "car";
    stop.kind = dir::EventKind::kCar;
    stop.car = "p1";
    stop.text = "at 8 car p1 stop";
    ev.push_back(stop);
    const Layout l2 = BuildLayout(ev, 14.5);
    CheckNum(l2.markers[static_cast<size_t>(MarkerLine(l2, 5))].span_end, 8.0, "a path runs to its stop");
    CheckEq(DescribeStatement(stop), "stop", "a stop's label");
    dir::SnapEvent off;
    off.kind = dir::EventKind::kCamera;
    off.text = "cam 12 off";
    CheckEq(DescribeStatement(off), "off", "cam off");
    dir::SnapEvent hour;
    hour.kind = dir::EventKind::kWorld;
    hour.text = "at 3 hour 18 over 4";
    CheckEq(DescribeStatement(hour), "hour 18", "an hour key");
}

void TestAxis() {
    Axis a;
    a.x0 = 100;
    a.x1 = 1100;
    a.t0 = 0;
    a.t1 = 10;
    CheckNum(a.PixelsPerSecond(), 100, "100 px a second");
    CheckNum(a.TimeToX(5), 600, "5 s is at 600");
    CheckNum(a.XToTime(600), 5, "and back");
    // zoom keeps the time under the pointer where it was
    a.ZoomAt(600, 2.0);
    CheckNum(a.XToTime(600), 5.0, "the time under the pointer stays", 1e-9);
    CheckNum(a.t1 - a.t0, 5.0, "twice the zoom is half the span", 1e-9);
    a.ZoomAt(100, 2.0);
    CheckNum(a.t0, 2.5, "zoom at the left edge keeps the left edge", 1e-9);
    a.ZoomAt(100, 1e9);
    CheckNum(a.t1 - a.t0, kMinViewSpan, "the closest zoom", 1e-9);
    a.ZoomAt(100, 1e-9);
    CheckNum(a.t1 - a.t0, kMaxViewSpan, "the widest zoom", 1e-6);
    // pan: the content follows the pointer
    Axis b;
    b.x0 = 0;
    b.x1 = 1000;
    b.t0 = 0;
    b.t1 = 10;
    b.Pan(100);
    CheckNum(b.t0, -1.0, "dragging right shows earlier times");
    CheckNum(b.t1, 9.0, "...");
    // keep inside
    b.KeepInside(0, 10);
    Check(b.t0 >= -1.0 - 1e-9 && b.t1 <= 11.0 + 1e-9, "the view stays near the scene");
    Axis c;
    c.x0 = 0;
    c.x1 = 1000;
    c.t0 = 50;
    c.t1 = 60;
    c.KeepInside(0, 10);
    Check(c.t1 <= 11.0 + 1e-9, "a view far away is brought back");
    Axis f;
    f.x0 = 0;
    f.x1 = 800;
    f.Fit(20);
    Check(f.t0 < 0.0 && f.t1 > 20.0, "Fit shows the whole scene with a margin");
}

void TestRuler() {
    Axis a;
    a.x0 = 0;
    a.x1 = 1000;
    a.t0 = 0;
    a.t1 = 10;
    const std::vector<Tick> t = RulerTicks(a);
    Check(!t.empty(), "ticks");
    int majors = 0;
    for (const Tick& k : t) majors += k.major ? 1 : 0;
    Check(majors >= 5 && majors <= 12, "about one label every 80 px: " + std::to_string(majors));
    for (const Tick& k : t) {
        if (k.major) Check(!k.label.empty(), "a major tick is labelled");
        else Check(k.label.empty(), "a minor tick is not");
    }
    // a long view: minutes
    a.t1 = 600;
    const std::vector<Tick> m = RulerTicks(a);
    Check(!m.empty() && m.size() < 200, "ten minutes in 1000 px is a short list of ticks");
    bool minute = false;
    for (const Tick& k : m) minute = minute || k.label.find(':') != std::string::npos;
    Check(minute, "labels past a minute read m:ss");
    // a deep view: fractions
    a.t0 = 1.0;
    a.t1 = 1.5;
    const std::vector<Tick> d = RulerTicks(a);
    bool frac = false;
    for (const Tick& k : d) frac = frac || (k.major && k.label.find('.') != std::string::npos);
    Check(frac, "a deep zoom labels fractions of a second");
    CheckEq(FormatClock(7.25), "7.25", "clock under a minute");
    CheckEq(FormatClock(67.3, 1), "1:07.3", "clock over a minute");
    CheckEq(FormatClock(0.0, 0), "0", "clock zero");
}

void TestSnap() {
    Axis a;
    a.x0 = 0;
    a.x1 = 1000;
    a.t0 = 0;
    a.t1 = 10;  // 100 px a second: 7 px = 0.07 s
    const std::vector<double> marks = {2.0, 4.5};
    Snapped s = SnapTime(2.05, a, marks, 6.0, 10.0, 1.0 / 60.0);
    CheckNum(s.t, 2.0, "within 7 px of a key: snaps to it");
    Check(s.kind == SnapKind::kMark, "(a mark)");
    s = SnapTime(6.04, a, marks, 6.0, 10.0, 1.0 / 60.0);
    Check(s.kind == SnapKind::kPlayhead && s.t == 6.0, "to the playhead");
    s = SnapTime(0.03, a, marks, 6.0, 10.0, 1.0 / 60.0);
    Check(s.kind == SnapKind::kStart && s.t == 0.0, "to zero");
    s = SnapTime(9.96, a, marks, 6.0, 10.0, 1.0 / 60.0);
    Check(s.kind == SnapKind::kEnd && s.t == 10.0, "to the end");
    s = SnapTime(3.0, a, marks, 6.0, 10.0, 1.0 / 60.0);
    Check(s.kind == SnapKind::kFrame, "to the frame grid when nothing is near");
    CheckNum(std::fmod(s.t * 60.0, 1.0) < 1e-6 || std::fmod(s.t * 60.0, 1.0) > 1 - 1e-6 ? 0.0 : 1.0, 0.0, "a whole number of frames");
    s = SnapTime(3.0123, a, marks, 6.0, 10.0, 0.0);
    CheckNum(s.t, 3.012, "no grid: to the millisecond");
    s = SnapTime(-1.0, a, marks, 6.0, 10.0, 0.0);
    CheckNum(s.t, 0.0, "never negative");
    // the pixel reach follows the zoom: zoomed far in, 0.05 s is many pixels and does not snap
    a.t0 = 2.0;
    a.t1 = 2.5;  // 2000 px a second
    s = SnapTime(2.1, a, marks, 6.0, 10.0, 0.0);
    CheckNum(s.t, 2.1, "zoomed in, a key 0.1 s away is far");
}

void TestHitAndDrag() {
    const World w;
    const Layout l = BuildLayout(w.events, w.parsed.scene.Duration());
    Axis a;
    a.x0 = 0;
    a.x1 = 1450;
    a.t0 = 0;
    a.t1 = 14.5;  // 100 px a second
    const Metrics m;
    const int m7 = MarkerLine(l, 7);
    const Marker& mk = l.markers[static_cast<size_t>(m7)];
    Vec2f at{a.TimeToX(mk.t), m.LaneCenter(mk.lane)};
    Check(HitMarker(l, a, m, at, 8.0f) == m7, "the pointer on a key hits it");
    Check(HitMarker(l, a, m, {at.x + 5, at.y + 2}, 8.0f) == m7, "within the slop");
    Check(HitMarker(l, a, m, {at.x + 40, at.y}, 8.0f) != m7, "not 40 px away");
    // a cut is hit anywhere in its lane
    const int cut = MarkerLine(l, 10);
    const float cx = a.TimeToX(l.markers[static_cast<size_t>(cut)].t);
    Check(HitMarker(l, a, m, {cx + 1, m.LaneTop(0) + 3}, 6.0f) == cut, "a cut is a line across its lane");
    // two keys at the same time: the one drawn on top (the later one) wins a tie
    Check(LaneAt(m, 5, 4) == -1, "the ruler is no lane");
    Check(LaneAt(m, m.LaneCenter(2), 4) == 2, "the lane under y");
    Check(LaneAt(m, m.LaneTop(1) + m.lane_h + 1, 4) == -1, "the gap between lanes");
    Check(LaneAt(m, 9999, 4) == -1, "below the lanes");
    // a bar
    const int rig = MarkerLine(l, 8);
    const Marker& rm = l.markers[static_cast<size_t>(rig)];
    const Vec2f bar{(a.TimeToX(rm.t) + a.TimeToX(rm.span_end)) * 0.5f, m.LaneCenter(rm.lane)};
    Check(HitSpan(l, a, m, bar) == rig, "a point on a rig's bar hits the bar");
    Check(HitSpan(l, a, m, {bar.x, bar.y + 20}) == -1, "not off it");

    // drag: 4 px slop, snapping, the grab offset
    Drag d = BeginDrag(l, m7, a, at.x + 3.0f, false);
    Check(d.active && d.marker == m7 && d.line == 7, "a drag begins on a key");
    CheckNum(d.t_start, mk.t, "from the key's time");
    UpdateDrag(&d, a, at.x + 5.0f, MarkerTimes(l, m7), 6.0, 14.5, 1.0 / 60.0, true);
    Check(!d.moved && d.t_now == mk.t, "inside the slop nothing moves");
    Check(FinishDrag(d) == DragOutcome::kNothing, "and a drop there does nothing");
    UpdateDrag(&d, a, a.TimeToX(5.5f) + 3.0f, MarkerTimes(l, m7), 6.0, 14.5, 1.0 / 60.0, true);
    Check(d.moved, "past the slop it moves");
    // the key does not jump to the pointer: the grab was 3 px (0.03 s) right of the key
    CheckNum(d.t_now, 5.5, "the key lands where the pointer minus the grab says (snapped to the frame grid)", 1.0 / 120.0 + 1e-9);
    Check(FinishDrag(d) == DragOutcome::kMove, "a drop is a move");
    // alt: a copy
    Drag dup = BeginDrag(l, m7, a, at.x, true);
    UpdateDrag(&dup, a, at.x + 200.0f, MarkerTimes(l, m7), 0.0, 14.5, 1.0 / 60.0, true);
    Check(FinishDrag(dup) == DragOutcome::kDuplicate, "an Alt drop is a copy");
    // snapping to another key
    Drag s = BeginDrag(l, m7, a, at.x, false);
    UpdateDrag(&s, a, a.TimeToX(6.0f) + 4.0f, MarkerTimes(l, m7), 0.5, 14.5, 1.0 / 60.0, true);
    CheckNum(s.t_now, 6.0, "snaps to the key at 6 s");
    Check(s.snap == SnapKind::kMark, "(a mark)");
    UpdateDrag(&s, a, a.TimeToX(6.0f) + 4.0f, MarkerTimes(l, m7), 0.5, 14.5, 1.0 / 60.0, false);
    Check(s.t_now > 6.0 && s.snap == SnapKind::kNone, "Shift: free, to the millisecond");
    // back to the start: not a move
    UpdateDrag(&s, a, at.x + 40.0f, MarkerTimes(l, m7), 0.5, 14.5, 1.0 / 60.0, false);
    UpdateDrag(&s, a, at.x, MarkerTimes(l, m7), 0.5, 14.5, 1.0 / 60.0, true);
    Check(FinishDrag(s) == DragOutcome::kNothing, "dropped where it started, it is no edit");
    // a key that cannot be moved
    std::vector<dir::SnapEvent> ev;
    dir::SnapEvent st;
    st.line = 2;
    st.t = 0;
    st.track = "start";
    st.kind = dir::EventKind::kOther;
    st.text = "start save x";
    ev.push_back(st);
    const Layout sl = BuildLayout(ev, 5);
    Check(!BeginDrag(sl, 0, a, 0.0f, false).active, "the preamble is not draggable");
    // the playhead
    CheckNum(ScrubTime(a, a.TimeToX(3.3f), 14.5, 0.0), 3.3, "a click on the ruler is a time", 1e-3);
    CheckNum(ScrubTime(a, -50.0f, 14.5, 0.0), 0.0, "clamped at 0");
    CheckNum(ScrubTime(a, 9000.0f, 14.5, 0.0), 14.5, "clamped at the end");
    const double fs = ScrubTime(a, a.TimeToX(3.3f), 14.5, 1.0 / 30.0);
    CheckNum(std::round(fs * 30.0), fs * 30.0, "snapped to the frame grid", 1e-9);
}

// ---------------------------------------------------------------------------------------------------------------- the dispatcher

void TestDispatcher() {
    std::vector<dir::Posted> posted;
    bool accept = true;
    Dispatcher d([&](dir::Posted&& p) {
        if (!accept) return false;
        posted.push_back(std::move(p));
        return true;
    });
    int answered = 0;
    std::string seen;
    Cmd c;
    c.cmd = "transport.seek";
    c.args.Set("t", 3.5);
    Check(d.Send(c, [&](const dir::Reply& r) {
        ++answered;
        seen = r.result.StringOr("note", "");
    }), "a command is posted");
    Check(posted.size() == 1 && posted[0].cmd == "transport.seek" && posted[0].source == dir::Source::kPanel, "as the panel's");
    CheckNum(posted[0].args.NumberOr("t", 0), 3.5, "with its arguments");
    d.Drain(1.0);
    Check(answered == 0, "nothing is answered before the reply");
    Check(d.pending() == 1, "one is waiting");
    // the reply comes from the game thread
    dir::Reply r;
    r.ok = true;
    r.result.Set("note", "ok");
    posted[0].on_reply(r);
    Check(answered == 0, "the continuation does not run on the game thread");
    d.Drain(1.1);
    Check(answered == 1 && seen == "ok", "it runs from Drain");
    Check(d.pending() == 0, "and the command is done");
    d.Drain(1.2);
    Check(answered == 1, "once");
    // a failed reply is a notice
    Cmd f;
    f.cmd = "scene.edit";
    d.Send(f);
    dir::Reply bad;
    bad.ok = false;
    bad.code = dir::ErrorCode::kStale;
    bad.message = "line 6 is not what you read";
    posted.back().on_reply(bad);
    d.Drain(2.0);
    Check(!d.notices().empty() && d.notices().back().error && d.notices().back().text.find("stale") != std::string::npos, "an error reply is a notice");
    // a refusal
    accept = false;
    Check(!d.Send(f), "a Director that is off refuses");
    Check(d.pending() == 0, "nothing waits for it");
    Check(d.notices().back().text.find("did not take it") != std::string::npos, "and says so");
    // the callback outlives the dispatcher
    accept = true;
    posted.clear();
    std::function<void(const dir::Reply&)> late;
    {
        Dispatcher gone([&](dir::Posted&& p) {
            late = p.on_reply;
            return true;
        });
        gone.Send(f);
    }
    late(r);  // must not crash
    Check(true, "a late reply after the panel is gone is harmless");
    // replies of two commands, the second answered first
    Dispatcher e([&](dir::Posted&& p) {
        posted.push_back(std::move(p));
        return true;
    });
    posted.clear();
    std::string order;
    e.Send(f, [&](const dir::Reply&) { order += "A"; });
    e.Send(f, [&](const dir::Reply&) { order += "B"; });
    posted[1].on_reply(r);
    e.Drain(0);
    posted[0].on_reply(r);
    e.Drain(0);
    CheckEq(order, "BA", "a slow command does not hold back a fast one");
    // many notices keep the last eight
    for (int i = 0; i < 20; ++i) e.Note(i, true, "n" + std::to_string(i));
    Check(e.notices().size() == 8 && e.notices().back().text == "n19", "the last eight notices");
}

// ---------------------------------------------------------------------------------------------------------------- edits against the real SceneEdit

void TestSceneTextAndExpect() {
    const World w;
    CheckEq(w.scene_text.Line(4), "at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62   # the hero", "the text keeps the comment");
    CheckEq(w.scene_text.Line(99), "", "no such line");
    CheckNum(w.scene_text.LineCount(), 13, "the lines");
    const EditContext ctx = w.Ctx();
    CheckEq(ctx.ExpectFor(4, "at 0 car p1 spawn ..."), w.scene_text.Line(4), "expect is the full line, comment and all");
    EditContext old = ctx;
    old.rev = 8;  // the snapshot moved on: the cached text is not trusted
    CheckEq(old.ExpectFor(4, "statement"), "statement", "a text of another revision is not trusted");
    EditContext none{7, nullptr};
    CheckEq(none.ExpectFor(4, "statement"), "statement", "no text: the statement");
    Check(SplitLines("a\r\nb\n").size() == 2, "CRLF and a final newline");
}

void TestKeyEdits() {
    World w;
    const Layout l = BuildLayout(w.events, w.parsed.scene.Duration());
    const EditContext ctx = w.Ctx();
    const Marker& k7 = l.markers[static_cast<size_t>(MarkerLine(l, 7))];

    // move: only the time word of the line changes
    Cmd mv = MoveKey(ctx, k7, 4.5);
    CheckEq(mv.cmd, "scene.edit", "a drag is a scene.edit");
    CheckNum(mv.args.NumberOr("base_rev", 0), 7, "against the revision read");
    dir::EditResult r = ApplyEdit(w, mv);
    Check(r.ok, "the move applies: " + r.message);
    CheckEq(LineOf(r.text, 7), "cam 4.5 pos 810 12 -800 look 819.6 17.6 -808.4 fov 35", "only the time word changed");
    CheckEq(LineOf(r.text, 6), "cam 0 pos 800 20 -790 yaw 60 pitch -5 fov 50 ease inout", "the other keys are untouched");
    Check(r.text.size() == w.text.size() + 2, "the file grew by the two characters of '.5'");
    // a key with a comment: the move keeps it
    const Marker& k8 = l.markers[static_cast<size_t>(MarkerLine(l, 8))];
    r = ApplyEdit(w, MoveKey(ctx, k8, 7.25));
    Check(r.ok, "a key with a comment moves");
    CheckEq(LineOf(r.text, 8), "cam 7.25 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50   # follow", "the comment and the spacing stay");
    // a stale line: the statement is not the line (the comment), so without the text the edit is refused
    const EditContext no_text{7, nullptr};
    r = ApplyEdit(w, MoveKey(no_text, k8, 7.25));
    Check(!r.ok && r.code == dir::ErrorCode::kStale, "without the line's full text a commented key is refused as stale, and the panel re-reads");
    // duplicate: the original stays, the copy is a statement at the new time
    r = ApplyEdit(w, DuplicateKey(ctx, k8, 7.5));
    Check(r.ok, "a duplicate applies: " + r.message);
    Check(r.text.find("cam 6 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50   # follow") != std::string::npos, "the original is still there");
    Check(r.text.find("cam 7.5 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50\n") != std::string::npos, "and a copy at 7.5 (the comment is not copied)");
    // delete
    r = ApplyEdit(w, DeleteKey(ctx, k7));
    Check(r.ok && SplitLines(r.text).size() == 12, "a delete takes the line");
    Check(r.text.find("look 819.6") == std::string::npos || r.text.find("cam 4 pos 810") == std::string::npos, "the key is gone");
    // ease
    r = ApplyEdit(w, SetEase(ctx, k7, "out"));
    Check(r.ok, "an ease applies");
    CheckEq(LineOf(r.text, 7), "cam 4 pos 810 12 -800 look 819.6 17.6 -808.4 fov 35 ease out", "the word is added to a key that had none");
    const Marker& k6 = l.markers[static_cast<size_t>(MarkerLine(l, 6))];
    r = ApplyEdit(w, SetEase(ctx, k6, "linear"));
    CheckEq(LineOf(r.text, 6), "cam 0 pos 800 20 -790 yaw 60 pitch -5 fov 50 ease linear", "and changed on one that had it");
    // a key's fov
    r = ApplyEdit(w, SetKeyWord(ctx, k7, "fov", "28"));
    CheckEq(LineOf(r.text, 7), "cam 4 pos 810 12 -800 look 819.6 17.6 -808.4 fov 28", "a field of view");
    // update from a pose: the key takes the free camera's place, keeps its time and its ease, drops a look
    dir::SnapPose pose;
    pose.pos[0] = 801.5, pose.pos[1] = 21, pose.pos[2] = -791.25;
    pose.yaw = 62.5, pose.pitch = -3, pose.roll = 1.5, pose.fov = 44;
    r = ApplyEdit(w, UpdateKeyFromPose(ctx, k6, pose));
    Check(r.ok, "an update applies: " + r.message);
    CheckEq(LineOf(r.text, 6), "cam 0 pos 801.5 21 -791.25 yaw 62.5 pitch -3 roll 1.5 fov 44 ease inout", "the key is the pose, with its ease");
    r = ApplyEdit(w, UpdateKeyFromPose(ctx, k8, pose));
    CheckEq(LineOf(r.text, 8), "cam 6 pos 801.5 21 -791.25 yaw 62.5 pitch -3 roll 1.5 fov 44   # follow", "a rig key becomes a pose key and keeps its comment");
    // a cut
    r = ApplyEdit(w, CutAt(7, 3.25));
    Check(r.ok && r.text.find("cut 3.25") != std::string::npos, "a cut is inserted");
    // add a key: camera.key.add
    Cmd add = AddKeyHere(0, false, "inout", true);
    CheckEq(add.cmd, "camera.key.add", "a key at the playhead is camera.key.add");
    Check(!add.args.Has("t") && add.args.StringOr("ease", "") == "inout" && add.args.BoolOr("cut", false), "its arguments");
    Cmd add_at = AddKeyHere(3.5, true, "", false);
    CheckNum(add_at.args.NumberOr("t", -1), 3.5, "at a time");
    Check(!add_at.args.Has("ease") && !add_at.args.Has("cut"), "and nothing else");
    // the strip prints the wire's own line
    CheckEq(FormatCmd(add_at), "camera.key.add t=3.5", "the 'what I did' line is the console grammar");
    // every command parses back through the console grammar (the same commands, no JSON)
    std::string cmd, err;
    dir::Json args;
    Check(dir::ParseConsoleLine(FormatCmd(mv), &cmd, &args, &err), "the move's line parses back: " + err);
    CheckEq(cmd, "scene.edit", "...");
    // a statement at a new time keeps the layout
    CheckEq(StatementAtTime("cam 6 rig chase car:p1   # c", 2.0), "cam 2 rig chase car:p1", "StatementAtTime");
    CheckEq(StatementAtTime("at 3 hour 12", 4.5), "at 4.5 hour 12", "an `at` statement");
}

void TestKeyPoseAndNavigation() {
    dir::SnapPose p;
    Check(ParseKeyPose("cam 0 pos 800 20 -790 yaw 60 pitch -5 fov 50 ease inout", &p), "a pos key has a pose of its own");
    CheckNum(p.pos[0], 800, "x");
    CheckNum(p.yaw, 60, "yaw");
    CheckNum(p.pitch, -5, "pitch");
    CheckNum(p.fov, 50, "fov");
    Check(ParseKeyPose("cam 4 pos 810 12 -800 look 819.6 17.6 -808.4 fov 35", &p), "a pos + look key");
    CheckNum(p.fov, 35, "fov");
    double yaw = 0, pitch = 0;
    const double from[3] = {810, 12, -800}, to[3] = {819.6, 17.6, -808.4};
    dir::LookAtAngles(from, to, &yaw, &pitch);
    CheckNum(p.yaw, yaw, "the aim of a look key", 1e-9);
    CheckNum(p.pitch, pitch, "...", 1e-9);
    Check(!ParseKeyPose("cam 6 rig chase car:p1 dist 7", &p), "a rig depends on a car");
    Check(!ParseKeyPose("cam 6 attach car:p1 0 2 -6", &p), "so does an attach");
    Check(!ParseKeyPose("cam 6 pos 1 2 3 look car:p1", &p), "and a look at a car");
    Check(!ParseKeyPose("cam 6 pos 1 2 3", &p), "a position alone has no aim");
    Check(!ParseKeyPose("cut 6", &p), "a cut is no key");

    World w;
    dir::Snapshot snap;
    snap.events = w.events;
    snap.rev = 7;
    snap.transport.t = 5.0;
    std::vector<Cmd> next = GoToKeyCmds(snap, +1);
    Check(next.size() == 3, "next key: select, seek, and the camera");
    CheckEq(next[0].cmd, "ui.select", "it selects the key");
    CheckNum(next[0].args.NumberOr("line", 0), 8, "(line 8: the chase at 6 s)");
    CheckEq(next[1].cmd, "transport.seek", "goes to its time");
    CheckNum(next[1].args.NumberOr("t", 0), 6, "(6 s)");
    CheckEq(next[2].cmd, "camera.mode", "a rig key shows through the scene's camera");
    CheckEq(next[2].args.StringOr("mode", ""), "scene", "(scene)");
    std::vector<Cmd> prev = GoToKeyCmds(snap, -1);
    Check(prev.size() == 3 && prev[0].args.NumberOr("line", 0) == 7, "previous key: line 7 at 4 s");
    CheckEq(prev[2].cmd, "camera.set", "a pos key puts the free camera on its pose");
    Check(prev[2].args.Get("pos").size() == 3, "with its position");
    snap.transport.t = 9.0;
    Check(GoToKeyCmds(snap, +1).empty(), "no key after the last");
    snap.transport.t = 0.0;
    Check(GoToKeyCmds(snap, -1).empty(), "no key before the first");
    snap.transport.t = 6.0;
    Check(GoToKeyCmds(snap, +1)[1].args.NumberOr("t", 0) == 9.0, "from a key's own time the next one is the following key");
    Check(CameraKeys(snap.events).size() == 4, "four camera keys (the cut is not one)");
}

// ---------------------------------------------------------------------------------------------------------------- rigs

void TestRigs() {
    World w;
    const Layout l = BuildLayout(w.events, w.parsed.scene.Duration());
    const EditContext ctx = w.Ctx();
    RigInfo chase;
    Check(ParseRig(w.scene_text.Line(8), &chase), "a rig key parses");
    CheckEq(chase.kind, "chase", "its kind");
    CheckEq(chase.subject, "p1", "its car");
    const std::vector<RigKnob>& knobs = KnobsFor("chase");
    Check(knobs.size() == 6, "a chase has six sliders");
    CheckNum(KnobValue(chase, knobs[0]), 7.0, "dist as written");
    CheckNum(KnobValue(chase, knobs[2]), 0.0, "side: the default when the key has none");
    CheckNum(KnobValue(chase, knobs[3]), 0.35, "lag");
    Check(!ParseRig("cam 0 pos 1 2 3", &chase), "a pos key is not a rig");
    CheckNum(KeyKnobValue("cam 6 rig chase car:p1 fov 42", KeyKnobs()[0]), 42, "the key's fov");
    CheckNum(KeyKnobValue("cam 6 rig chase car:p1", KeyKnobs()[0]), 50, "the default fov");

    // a slider: one word pair, clamped to its range
    const Marker& m8 = l.markers[static_cast<size_t>(MarkerLine(l, 8))];
    dir::EditResult r = ApplyEdit(w, SetKnob(ctx, m8, knobs[0], 9.5));
    Check(r.ok, "a slider applies: " + r.message);
    CheckEq(LineOf(r.text, 8), "cam 6 rig chase car:p1 dist 9.5 height 1.9 lag 0.35 fov 50   # follow", "only the distance changed");
    r = ApplyEdit(w, SetKnob(ctx, m8, knobs[0], 500.0));
    CheckEq(LineOf(r.text, 8), "cam 6 rig chase car:p1 dist 30 height 1.9 lag 0.35 fov 50   # follow", "clamped to the slider's range");
    r = ApplyEdit(w, SetKnob(ctx, m8, knobs[2], 2.5));
    CheckEq(LineOf(r.text, 8), "cam 6 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50 side 2.5   # follow", "a word the key lacks is added");
    r = ApplyEdit(w, SetKnob(ctx, m8, KeyKnobs()[0], 35));
    CheckEq(LineOf(r.text, 8), "cam 6 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 35   # follow", "the key's own fov");

    // the orbit's sweep
    const Marker& m9 = l.markers[static_cast<size_t>(MarkerLine(l, 9))];
    RigInfo orbit;
    Check(ParseRig(w.scene_text.Line(9), &orbit), "an orbit parses");
    Check(orbit.az_range && orbit.az_from == 20 && orbit.az_to == 200 && orbit.has_over, "its sweep, 20 to 200 over 6");
    r = ApplyEdit(w, SetOrbitAz(ctx, m9, orbit, 0, 90));
    Check(r.ok, "the sweep applies: " + r.message);
    CheckEq(LineOf(r.text, 9), "cam 9 rig orbit car:p1 r 8 el 10 az 0..90 over 6", "az a..b, over kept");
    r = ApplyEdit(w, SetOrbitAz(ctx, m9, orbit, 45, 45));
    CheckEq(LineOf(r.text, 9), "cam 9 rig orbit car:p1 r 8 el 10 az 45 over 6", "no sweep: one angle");
    RigInfo no_over = orbit;
    no_over.has_over = false;
    r = ApplyEdit(w, SetOrbitAz(ctx, m9, no_over, 10, 100));
    Check(LineOf(r.text, 9).find("over 8") != std::string::npos, "a sweep with no seconds gets 8");

    // a new rig key
    NewRig nr;
    nr.kind = "chase";
    nr.car = "p1";
    CheckEq(NewRigWords(nr), "chase car:p1 dist 6 height 1.8 lag 0.3", "a chase");
    nr.kind = "orbit";
    CheckEq(NewRigWords(nr), "orbit car:p1 r 8 el 10 az 20..200 over 8", "an orbit sweeps");
    nr.kind = "tripod";
    nr.has_pos = true;
    nr.pos[0] = 10, nr.pos[1] = 2, nr.pos[2] = -3.5;
    CheckEq(NewRigWords(nr), "tripod at 10 2 -3.5 track car:p1", "a tripod stands at the camera");
    nr.kind = "flyby";
    nr.route = "run.route";
    CheckEq(NewRigWords(nr), "flyby path run.route track car:p1", "a fly-by");
    nr.kind = "hood";
    CheckEq(NewRigWords(nr), "hood car:p1", "a mount");
    nr.car = "";
    CheckEq(NewRigWords(nr), "hood car:player", "no car: the player's");
    Cmd add = AddRigKey(2.0, true, nr);
    CheckEq(add.cmd, "camera.key.add", "through camera.key.add");
    CheckEq(add.args.StringOr("rig", ""), "hood car:player", "with its words");
    // every kind has sliders in range
    for (const std::string& kind : RigKinds()) {
        for (const RigKnob& k : KnobsFor(kind)) Check(k.lo < k.hi && k.def >= k.lo && k.def <= k.hi, "the slider " + kind + "/" + k.key + " has a sane range");
    }
    for (const RigKnob& k : KeyKnobs()) Check(k.lo < k.hi && k.def >= k.lo && k.def <= k.hi, std::string("the key slider ") + k.key);
    // the rig's words are accepted by the real parser: a scene with every kind
    std::string scene = "scene t\nat 0 car p1 spawn vp_lam_murcielago_06 at 1,2,3\n";
    double t = 1;
    for (const std::string& kind : RigKinds()) {
        NewRig n;
        n.kind = kind;
        n.car = "p1";
        n.has_pos = true;
        n.route = "run.route";
        scene += "cam " + std::to_string(t++) + " rig " + NewRigWords(n) + "\n";
    }
    const mc::cinematic::ParseResult pr = mc::cinematic::ParseScene(scene);
    bool no_rig_errors = true;
    for (const auto& d : pr.diagnostics) {
        if (d.severity == mc::cinematic::Severity::kError) no_rig_errors = false;
    }
    Check(no_rig_errors, "the scene parser accepts every kind of new rig key");
}

// ---------------------------------------------------------------------------------------------------------------- cars

void TestCars() {
    World w;
    const EditContext ctx = w.Ctx();
    dir::Snapshot snap;
    snap.events = w.events;
    dir::SnapCar c1;
    c1.id = "p1";
    dir::SnapCar c2;
    c2.id = "p2";
    snap.cars = {c1, c2};
    CheckEq(NextCarId(snap.cars, snap.events), "p3", "the first free id");
    snap.cars = {};
    CheckEq(NextCarId(snap.cars, snap.events), "p2", "ids in the scene's text count too");
    const double at[3] = {819.6, 17.6, -808.4};
    Cmd sp = SpawnCar("p2", "vp_lam_murcielago_06", at, 62.04, 0.0, -1);
    CheckEq(sp.cmd, "car.spawn", "spawn here is car.spawn");
    CheckEq(sp.args.StringOr("id", ""), "p2", "id");
    CheckEq(sp.args.StringOr("model", ""), "vp_lam_murcielago_06", "model");
    Check(sp.args.Get("at").size() == 3, "at");
    CheckNum(sp.args.NumberOr("heading", 0), 62.0, "heading rounded to a tenth");
    CheckNum(sp.args.NumberOr("t", -1), 0.0, "at 0: the ready gate holds the clock for it");
    Check(!sp.args.Has("paint"), "no paint unless asked");
    Check(!SpawnCar("p3", "m", at, 0, -1.0, 12).args.Has("t") && SpawnCar("p3", "m", at, 0, -1.0, 12).args.IntOr("paint", -1) == 12, "t < 0 leaves the time to the scene's clock; paint");
    // the pick
    CheckEq(FormatCmd(PickAt(0.5, 0.5)), "query.pick u=0.5 v=0.5", "the crosshair");
    dir::Reply hit;
    hit.ok = true;
    hit.result.Set("hit", true);
    hit.result.Set("pos", dir::Json::Vec3(1, 2, 3));
    double pos[3];
    std::string why;
    Check(ParsePick(hit, pos, &why) && pos[2] == 3, "a hit has a position");
    dir::Reply miss;
    miss.ok = true;
    miss.result.Set("hit", false);
    Check(!ParsePick(miss, pos, &why) && why.find("ground") != std::string::npos, "a miss says what to do");
    dir::Reply err;
    err.ok = false;
    err.message = "the physics level is not up";
    Check(!ParsePick(err, pos, &why) && why == err.message, "an error says why");
    // finding a car's lines
    Check(SpawnLineOf(w.events, "p1") == 4, "the spawn line of p1");
    Check(SpawnLineOf(w.events, "p9") == -1, "none for a car the scene does not have");
    const std::vector<int> lines = LinesOfCar(w.events, "p1");
    Check(lines.size() == 2 && lines[0] == 4 && lines[1] == 5, "all of p1's lines");
    // drag a car to a new place: one write on release, the spawn line's `at`
    const double drop[3] = {820.5, 17.6, -801.0};
    dir::EditResult r = ApplyEdit(w, MoveCarSpawn(ctx, w.events, "p1", drop, 0, false));
    Check(r.ok, "the drop applies: " + r.message);
    CheckEq(LineOf(r.text, 4), "at 0 car p1 spawn vp_lam_murcielago_06 at 820.5,17.6,-801 heading 62   # the hero", "only the place changed, the leading `at 0` is not the place");
    r = ApplyEdit(w, MoveCarSpawn(ctx, w.events, "p1", drop, 91.25, true));
    CheckEq(LineOf(r.text, 4), "at 0 car p1 spawn vp_lam_murcielago_06 at 820.5,17.6,-801 heading 91.25   # the hero", "place and heading together");
    Check(MoveCarSpawn(ctx, w.events, "p9", drop, 0, false).cmd.empty(), "a car the scene does not spawn is not moved by an edit");
    // heading and paint
    r = ApplyEdit(w, SetCarHeading(ctx, w.events, "p1", -90));
    CheckEq(LineOf(r.text, 4), "at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 270   # the hero", "a heading wraps into 0..360");
    r = ApplyEdit(w, SetCarPaint(ctx, w.events, "p1", 42));
    Check(LineOf(r.text, 4).find("paint 42") != std::string::npos, "a paint");
    // delete the car: every line of it
    r = ApplyEdit(w, DeleteCar(ctx, w.events, "p1"));
    Check(r.ok && SplitLines(r.text).size() == 11, "both lines of the car go");
    Check(r.text.find("car p1") == std::string::npos, "nothing of it is left");
    Check(DeleteCar(ctx, w.events, "p9").cmd.empty(), "no lines, no command");
    // the live move while dragging
    Cmd place = PlaceCar("p1", drop, 33.33, true);
    CheckEq(place.cmd, "car.place", "dragging is car.place");
    CheckNum(place.args.NumberOr("heading", 0), 33.3, "heading");
    Check(!PlaceCar("p1", drop, 0, false).args.Has("heading"), "no heading unless set");
    // the picker
    const std::vector<std::string> all = {"vp_lam_murcielago_06", "vp_lam_gallardo_05", "vp_bmw_m3_06", "vp_ford_mustang_gt_05", "vp_nis_350z_05"};
    Check(FilterModels(all, "", {}, 100).size() == 5, "no search: all");
    Check(FilterModels(all, "LAM", {}, 100).size() == 2, "a search is case blind");
    Check(FilterModels(all, "lam 06", {}, 100).size() == 1, "every word must be in the name");
    Check(FilterModels(all, "zzz", {}, 100).empty(), "nothing");
    const std::vector<std::string> rec = FilterModels(all, "", {"vp_nis_350z_05", "gone"}, 100);
    CheckEq(rec[0], "vp_nis_350z_05", "recent ones first");
    Check(rec.size() == 5, "no duplicates, a recent that is gone is dropped");
    Check(FilterModels(all, "", {}, 2).size() == 2, "a cap");
    std::vector<std::string> recents;
    for (int i = 0; i < 12; ++i) NoteRecent(&recents, "m" + std::to_string(i));
    Check(recents.size() == 8 && recents[0] == "m11", "the eight latest, newest first");
    NoteRecent(&recents, "m5");
    CheckEq(recents[0], "m5", "a model used again moves to the front");
    Check(std::count(recents.begin(), recents.end(), std::string("m5")) == 1, "once");
    CheckEq(ModelTitle("vp_lam_murcielago_06"), "LAM MURCIELAGO 06", "a readable title");
    CheckEq(CarIds(snap)[0], "player", "the player is first in the pickers");
}

// ---------------------------------------------------------------------------------------------------------------- paths

void TestPaths() {
    dir::Json reply = dir::Json::Object();
    dir::Json pts = dir::Json::Array();
    const double data[4][4] = {{0, 0, 0, 10}, {0, 0, 20, 14}, {10, 0, 40, 20}, {10, 1, 60, 8}};
    for (const auto& p : data) {
        dir::Json q = dir::Json::Array();
        for (double v : p) q.Push(v);
        pts.Push(std::move(q));
    }
    reply.Set("points", std::move(pts));
    RouteDoc r = RouteFromReply("run.route", reply);
    Check(r.loaded && r.pts.size() == 4 && r.pts[2].x == 10 && r.pts[3].speed == 8, "path.get's reply is the route");
    CheckNum(RouteLengthM(r), 20 + std::hypot(10, 20) + 20, "length along x and z", 1e-9);
    Check(RouteSeconds(r) > 3.0 && RouteSeconds(r) < 8.0, "seconds at its own speeds");
    CheckNum(RouteStartHeading(r), 0.0, "the first segment runs along +Z: heading 0", 1e-9);
    RouteDoc east;
    east.pts = {{0, 0, 0, 10}, {10, 0, 0, 10}};
    CheckNum(RouteStartHeading(east), 90.0, "along +X: heading 90", 1e-9);

    // path.set carries every point
    Cmd set = SetRoute(r);
    CheckEq(set.cmd, "path.set", "writing a route is path.set");
    CheckEq(set.args.StringOr("route", ""), "run.route", "the route");
    Check(set.args.Get("points").size() == 4 && set.args.Get("points").at(2).size() == 4, "every point has x, y, z, speed");
    CheckEq(FormatCmd(DropWaypointAtCamera("run", 14)), "path.waypoint route=run at=camera speed=14", "a waypoint at the camera");
    const double at[3] = {1, 2, 3};
    Check(DropWaypoint("run", at, 9).args.Get("at").size() == 3, "a waypoint at a place");
    CheckEq(GetRoute("run.route").cmd, "path.get", "reading");
    Cmd rec = StartRecording("player", "drive1", 2.0);
    CheckEq(rec.cmd, "car.path.record", "recording");
    CheckEq(rec.args.StringOr("name", ""), "drive1", "its name");
    CheckEq(StopRecording().cmd, "car.path.record.stop", "stopping");
    Cmd assign = AssignRoute("p1", "drive1.route", 0.0, 6.0, true);
    CheckEq(assign.cmd, "car.path", "a car drives a route");
    Check(!assign.args.Has("speed") && assign.args.NumberOr("t", -1) == 6.0, "the route's own speeds when no speed is given");
    Check(AssignRoute("p1", "r", 12.0, 0, false).args.NumberOr("speed", 0) == 12.0 && !AssignRoute("p1", "r", 12.0, 0, false).args.Has("t"), "a speed; no time: now");

    // editing
    RouteDoc e = r;
    RouteInsertAfter(&e, 1);
    Check(e.pts.size() == 5 && e.pts[2].x == 5 && e.pts[2].z == 30 && e.pts[2].speed == 17, "a point inserted in the middle of two");
    RouteInsertAfter(&e, 4);
    Check(e.pts.size() == 6 && e.pts[5].z > e.pts[4].z, "one after the last continues along the last segment");
    Check(RouteDelete(&e, 2) && e.pts.size() == 5, "delete");
    RouteDoc two;
    two.pts = {{0, 0, 0, 5}, {0, 0, 10, 5}};
    Check(!RouteDelete(&two, 0), "a route keeps two points");
    RouteMove(&e, 0, 3.0, 4.0);
    Check(e.pts[0].x == 3.0 && e.pts[0].z == 4.0 && e.pts[0].y == 0.0, "a move changes x and z, not the height");
    RouteSetSpeed(&e, 1, 500.0);
    CheckNum(e.pts[1].speed, 120.0, "a speed is held to 0.5..120");
    RouteSetSpeed(&e, 1, 0.0);
    CheckNum(e.pts[1].speed, 0.5, "...");
    RouteSetAllSpeeds(&e, 22);
    Check(std::all_of(e.pts.begin(), e.pts.end(), [](const WayPt& p) { return p.speed == 22; }), "every speed");
    // smoothing
    RouteDoc zig;
    zig.pts = {{0, 0, 0, 10}, {10, 0, 0, 10}, {10, 0, 10, 20}, {20, 0, 10, 20}};
    const RouteDoc sm = SmoothRoute(zig, 1);
    Check(sm.pts.size() == 6, "a pass of corner cutting on 4 points gives 6: " + std::to_string(sm.pts.size()));
    Check(sm.pts.front().x == 0 && sm.pts.back().x == 20, "the ends stay");
    Check(std::none_of(sm.pts.begin(), sm.pts.end(), [](const WayPt& p) { return p.x == 10 && p.z == 0; }), "the corner is cut");
    Check(RouteLengthM(sm) < RouteLengthM(zig), "and the way is shorter");
    CheckNum(SmoothRoute(zig, 0).pts.size(), 4, "no pass, no change");
    Check(SmoothRoute(two, 3).pts.size() == 2, "two points are a straight line already");
    // replay with a car: the spawn at the first point heading along the route, then the path
    r.name = "drive1.route";
    const std::vector<Cmd> replay = ReplayCmds("p4", "vp_bmw_m3_06", r, 2.0, 0.0);
    Check(replay.size() == 2 && replay[0].cmd == "car.spawn" && replay[1].cmd == "car.path", "a replay is a spawn and a path");
    CheckNum(replay[0].args.NumberOr("t", -1), 0.0, "spawned at 0");
    CheckNum(replay[1].args.NumberOr("t", -1), 2.0, "the path at the playhead");
    CheckEq(replay[1].args.StringOr("route", ""), "drive1.route", "the recorded route");
    Check(ReplayCmds("p4", "m", RouteDoc{}, 0, 0).empty(), "an empty route is not replayed");
    CheckEq(SafeRouteName("my run 1!"), "my-run-1", "a route name keeps letters, digits, - and _");
    CheckEq(SafeRouteName(""), "", "empty stays empty");
    // the gizmo points
    Check(GizmoPoints(r).size() == 4 && GizmoPoints(r)[1][3] == 14, "points for the drawer");
}

// ---------------------------------------------------------------------------------------------------------------- take, transport, flying

void TestTakeAndTransport() {
    TakeForm f;
    Cmd t = TakeRenderCmd(f);
    CheckEq(t.cmd, "take.render", "the take is take.render");
    CheckNum(t.args.NumberOr("fps", 0), 60, "fps");
    CheckEq(t.args.StringOr("sink", ""), "png", "sink");
    Check(!t.args.Has("name") && !t.args.Has("from") && !t.args.Has("to"), "the whole scene, a name from the scene");
    f.name = "My shot 2";
    f.from = 2.0;
    f.to = 9.5;
    f.sink = 1;
    f.fps = 30;
    t = TakeRenderCmd(f);
    CheckEq(t.args.StringOr("name", ""), "My-shot-2", "a name made safe");
    CheckNum(t.args.NumberOr("from", -1), 2.0, "from");
    CheckNum(t.args.NumberOr("to", -1), 9.5, "to");
    CheckEq(t.args.StringOr("sink", ""), "ffmpeg", "the sink");
    f.sink = 99;
    CheckEq(TakeRenderCmd(f).args.StringOr("sink", ""), "raw", "a sink index is clamped");

    dir::SnapTransport tr;
    tr.paused = false;
    CheckEq(TogglePlayCmd(tr).cmd, "transport.pause", "Space pauses what plays");
    tr.paused = true;
    CheckEq(TogglePlayCmd(tr).cmd, "transport.play", "and plays what is held");
    CheckNum(SeekCmd(-3).args.NumberOr("t", 9), 0, "a seek is never before 0");
    CheckNum(StepCmd(0).args.NumberOr("frames", 0), 1, "a step is at least a frame");
    Check(FixedModeCmd(true).args.BoolOr("fixed", false), "fixed mode");
    CheckEq(CameraModeCmd("freecam").args.StringOr("mode", ""), "freecam", "camera mode");
    dir::SnapPose p;
    p.pos[0] = 1;
    p.yaw = 12.345;
    CheckNum(CameraSetCmd(p).args.NumberOr("yaw", 0), 12.35, "camera.set rounds the angles");
    CheckEq(LookAtCmd("car:p1").args.StringOr("target", ""), "car:p1", "look at");
    CheckNum(RestartCmd(0).args.size(), 0, "a restart from the start has no time");
    CheckNum(RestartCmd(4).args.NumberOr("t", 0), 4, "or from a time");
    tr = dir::SnapTransport{};
    tr.step = 1.0 / 30;
    CheckNum(FrameStep(tr), 1.0 / 30, "the fixed step is a frame");
    tr.step = 0;
    tr.fps = 24;
    CheckNum(FrameStep(tr), 1.0 / 24, "the header's fps next");
    tr.fps = 0;
    CheckNum(FrameStep(tr), 1.0 / 60, "else 1/60");
    tr.state = "running";
    CheckEq(TransportStateText(tr), "RUNNING", "state text");
    tr.paused = true;
    tr.frozen = true;
    Check(TransportStateText(tr).find("FROZEN") != std::string::npos, "a paused scene with the world held");
    tr.holding_for_ready = true;
    Check(TransportStateText(tr).find("CARS") != std::string::npos, "waiting for the cars");
    CheckEq(CameraModeText("freecam"), "FREE CAMERA", "mode text");
    CheckEq(CameraModeText("scene"), "SCENE CAMERA", "...");
    CheckEq(CameraModeText("anything"), "GAME CAMERA", "...");
}

void TestFlying() {
    FlyKeys k;
    FlyTuning tune;
    fc::Pad pad = MakeFlyPad(k, tune, 50);
    Check(pad.buttons == 0 && pad.lx == 0 && pad.ly == 0 && pad.speed_steps == 0 && pad.look_right == 0, "no keys, a pad at rest");
    Check(!AnyFlyInput(k), "no input");
    k.forward = true;
    k.right = true;
    k.up = true;
    k.fast = true;
    pad = MakeFlyPad(k, tune, 50);
    Check(pad.ly == 1.0 && pad.lx == 1.0 && pad.rt == 1.0 && pad.lt == 0.0, "W and D and E: forward, right, up");
    Check((pad.buttons & fc::kBtnL3) != 0 && (pad.buttons & fc::kBtnR3) == 0, "Shift is fast");
    k = FlyKeys{};
    k.back = true;
    k.left = true;
    k.down = true;
    k.slow = true;
    k.roll_left = true;
    k.level = true;
    k.zoom_in = true;
    pad = MakeFlyPad(k, tune, 50);
    Check(pad.ly == -1.0 && pad.lx == -1.0 && pad.lt == 1.0, "S and A and Q");
    Check((pad.buttons & fc::kBtnR3) && (pad.buttons & fc::kBtnLb) && (pad.buttons & fc::kBtnY) && (pad.buttons & fc::kBtnDpadUp), "Ctrl slow, Z tilt, R level, + zoom");
    // the mouse: only while the right button is held
    k = FlyKeys{};
    k.mouse_dx = 10;
    k.mouse_dy = -5;
    pad = MakeFlyPad(k, tune, 50);
    Check(pad.look_right == 0 && pad.look_up == 0, "the mouse does not look unless the button is held");
    k.look = true;
    pad = MakeFlyPad(k, tune, 50);
    CheckNum(pad.look_right, 1.2, "10 px to the right is 1.2 degrees to the right", 1e-9);
    CheckNum(pad.look_up, 0.6, "5 px up is 0.6 degrees up", 1e-9);
    pad = MakeFlyPad(k, tune, 25);
    CheckNum(pad.look_right, 0.6, "zoomed in to half the fov the same travel turns half as far", 1e-9);
    k.mouse_dy = 10;
    Check(MakeFlyPad(k, tune, 50).look_up < 0, "mouse down looks down");
    Check(AnyFlyInput(k), "a look is input");
    k = FlyKeys{};
    k.wheel = 3;
    pad = MakeFlyPad(k, tune, 50);
    CheckNum(pad.speed_steps, 3, "wheel notches are speed steps");
    k.wheel = 99;
    CheckNum(MakeFlyPad(k, tune, 50).speed_steps, 10, "at most ten at once");
    // the pad flies a real flight: W for a second moves along the view at the flight's speed
    fc::Flyer flyer;
    fc::Pose start;
    flyer.Seed(start);
    fc::Params params;
    params.inertia = 0.0;
    FlyKeys w;
    w.forward = true;
    for (int i = 0; i < 60; ++i) flyer.Step(MakeFlyPad(w, tune, 50), 1.0 / 60, params);
    CheckNum(flyer.pose().position.z, params.speed, "a second of W is `speed` metres along +Z (yaw 0)", 1e-6);
    FlyKeys look;
    look.look = true;
    look.mouse_dx = 100;
    flyer.Step(MakeFlyPad(look, tune, 50), 1.0 / 60, params);
    CheckNum(flyer.pose().yaw, -12.0, "100 px right turned the camera 12 degrees right", 1e-9);

    // the pad's chords as commands
    World world;
    dir::Snapshot snap;
    snap.events = world.events;
    snap.transport.t = 5.0;
    snap.transport.paused = true;
    fc::Events ev;
    ev.add_key = true;
    ev.play_pause = true;
    ev.step_frame = true;
    TakeForm take;
    std::vector<Cmd> cmds = CommandsForFlyEvents(ev, &snap, take);
    Check(cmds.size() == 3, "add key, play and one frame");
    CheckEq(cmds[0].cmd, "camera.key.add", "Back + B");
    CheckEq(cmds[1].cmd, "transport.play", "Back + Start plays a held scene");
    CheckEq(cmds[2].cmd, "transport.step", "Back + D-pad right");
    snap.transport.paused = false;
    ev = fc::Events{};
    ev.play_pause = true;
    CheckEq(CommandsForFlyEvents(ev, &snap, take)[0].cmd, "transport.pause", "and pauses a running one");
    ev = fc::Events{};
    ev.next_key = true;
    Check(CommandsForFlyEvents(ev, &snap, take).size() == 3 && CommandsForFlyEvents(ev, &snap, take)[0].cmd == "ui.select", "Back + RB: the next key");
    ev = fc::Events{};
    ev.render_take = true;
    CheckEq(CommandsForFlyEvents(ev, &snap, take)[0].cmd, "take.render", "Back + D-pad up renders the take");
    ev = fc::Events{};
    ev.next_key = true;
    Check(CommandsForFlyEvents(ev, nullptr, take).empty(), "with no snapshot there is no key to go to");
    Check(CommandsForFlyEvents(fc::Events{}, &snap, take).empty(), "no event, no command");
}

}  // namespace

int main() {
    TestPicture();
    TestProjector();
    TestSegments();
    TestGizmoPrimitives();
    TestLayout();
    TestAxis();
    TestRuler();
    TestSnap();
    TestHitAndDrag();
    TestDispatcher();
    TestSceneTextAndExpect();
    TestKeyEdits();
    TestKeyPoseAndNavigation();
    TestRigs();
    TestCars();
    TestPaths();
    TestTakeAndTransport();
    TestFlying();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
