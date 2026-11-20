// Check of the headlight shadow maps' rules (local_shadow_rules.h): the setting's words, sizes and view counts, the atlas packing, the lights'
// grouping and the view that covers a group, the matrices (a known point lands in the right atlas texel, in both handednesses and with the
// NDC signs flipped; the replay's c8-c11' puts a model point where the light's view sees its world point), the camera found from a draw's
// own constants, the casters (opaque, alpha tested, skinned, vehicle, impostor, billboard) over a list of vertex shaders as the pack names
// them, the lamps' own car, the budget, the face culling and the pipeline key. A program of its own: it needs no renderer, no ReXGlue and no
// game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_local_shadow_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_local_shadow_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

#include "headlight_types.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"

namespace ls = mcla::gpu::local_shadow;
namespace gc = mcla::gpu::gpu_class;
namespace tp = mcla::gpu::temporal;

// The stub of the rig's one function (src/gpu/headlight_types.h, the rig's in headlight_rig.cpp): the test links no renderer, and the renderer's
// half of the feature is the only caller.
namespace mcla::gpu {
const HeadlightSet& CurrentHeadlights() {
  static const HeadlightSet empty{};
  return empty;
}
}  // namespace mcla::gpu

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void CheckNear(double a, double b, double tol, const std::string& what) {
  ++g_checks;
  if (!Near(a, b, tol)) {
    ++g_failed;
    std::printf("FAIL: %s: %.9g vs %.9g (tol %.3g)\n", what.c_str(), a, b, tol);
  }
}

// ---------------------------------------------------------------------------
// the published structs
// ---------------------------------------------------------------------------

void CheckTypes() {
  using larecomp::gpu::LocalShadowFrame;
  using larecomp::gpu::LocalShadowView;
  Check(sizeof(LocalShadowView) == 16 * 4 + 4 * 4 + 4 + 4 + 4 + 4, "LocalShadowView is 96 bytes: matrix, rect, bias, texel, light index, valid");
  LocalShadowFrame frame;
  Check(frame.views.size() == 4 && frame.count == 0, "LocalShadowFrame holds four views and starts empty");
  larecomp::gpu::HeadlightSet set;
  Check(set.lights.size() == 4 && set.count == 0, "HeadlightSet holds four lights and starts empty");
  Check(larecomp::gpu::CurrentHeadlights().count == 0, "the stub rig has no lights");
}

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

void CheckSetting() {
  const gc::GpuClass classes[] = {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh};
  const ls::Level want[] = {ls::Level::kOff, ls::Level::kHigh, ls::Level::kUltra};
  for (int k = 0; k < 3; ++k) {
    const ls::Choice c = ls::Resolve(classes[k], "auto");
    const std::string at = std::string("auto on ") + std::string(gc::ClassName(classes[k]));
    Check(c.level == want[k] && c.automatic && !c.unknown, at + ": off / high / ultra by class");
    Check(ls::Resolve(classes[k], "AUTO").level == want[k], at + ": the word is case blind");
    Check(ls::Resolve(classes[k], "off").level == ls::Level::kOff && !ls::Resolve(classes[k], "off").automatic, at + ": off is kept");
    Check(ls::Resolve(classes[k], "high").level == ls::Level::kHigh, at + ": high is kept");
    Check(ls::Resolve(classes[k], "ultra").level == ls::Level::kUltra, at + ": ultra is kept");
    const ls::Choice odd = ls::Resolve(classes[k], "low");
    Check(odd.level == want[k] && odd.automatic && odd.unknown, at + ": the old word low is taken as auto");
  }
  Check(ls::TileSize(ls::Level::kOff) == 0 && ls::TileSize(ls::Level::kHigh) == 512 && ls::TileSize(ls::Level::kUltra) == 1024,
        "the tile is 512 on high and 1024 on ultra");
  Check(ls::MaxViews(ls::Level::kOff, 0) == 0 && ls::MaxViews(ls::Level::kOff, 3) == 0, "off draws no view, whatever the override");
  Check(ls::MaxViews(ls::Level::kHigh, 0) == 2 && ls::MaxViews(ls::Level::kUltra, 0) == 3, "high shadows two cars (the player's and the nearest other car's), ultra three");
  Check(ls::MaxViews(ls::Level::kHigh, 3) == 3 && ls::MaxViews(ls::Level::kUltra, 9) == 4, "the debug override takes 1 to 4 views");
  const ls::Choice mid = ls::Resolve(gc::GpuClass::kMid, "auto");
  const std::string text = ls::Describe(mid, gc::GpuClass::kMid);
  Check(text.find("high") != std::string::npos && text.find("512x512") != std::string::npos && text.find("auto on the mid class") != std::string::npos,
        "Describe says the level, the size and why: " + text);
  Check(ls::Describe(ls::Resolve(gc::GpuClass::kLow, "auto"), gc::GpuClass::kLow).find("512") == std::string::npos, "an off level has no size in its line");
  // No level is called low.
  for (const char* c : {"off", "high", "ultra"}) {
    Check(std::string(ls::LevelName(ls::Resolve(gc::GpuClass::kHigh, c).level)).find("low") == std::string::npos, "no level is called low");
  }
}

// ---------------------------------------------------------------------------
// the atlas
// ---------------------------------------------------------------------------

void CheckAtlas() {
  const ls::AtlasLayout one = ls::MakeAtlas(512, 1);
  Check(one.width() == 512 && one.height() == 512 && one.cols == 1 && one.rows == 1, "one view: a single tile");
  const ls::AtlasLayout two = ls::MakeAtlas(1024, 2);
  Check(two.width() == 2048 && two.height() == 1024, "two views: two tiles side by side");
  const ls::AtlasLayout four = ls::MakeAtlas(512, 4);
  Check(four.width() == 1024 && four.height() == 1024, "four 512 views: a 1024x1024 atlas");
  Check(ls::MakeAtlas(512, 9).views == 4, "never more than four views");
  Check(!ls::MakeAtlas(0, 2).valid() && !ls::MakeAtlas(512, 0).valid(), "no tile or no view: no atlas");
  for (uint32_t views = 1; views <= 4; ++views) {
    const ls::AtlasLayout atlas = ls::MakeAtlas(512, views);
    std::unordered_set<uint64_t> seen;
    for (uint32_t k = 0; k < views; ++k) {
      const ls::TileRect r = ls::TileAt(atlas, k);
      Check(r.size == 512 && r.x + r.size <= atlas.width() && r.y + r.size <= atlas.height(), "tile " + std::to_string(k) + " is inside the atlas");
      Check(seen.insert((uint64_t(r.x) << 32) | r.y).second, "tiles do not share a corner");
      float uv[4];
      ls::UvRect(atlas, k, uv);
      CheckNear(uv[0] * atlas.width(), r.x, 1e-3, "uv x of tile " + std::to_string(k));
      CheckNear(uv[1] * atlas.height(), r.y, 1e-3, "uv y of tile " + std::to_string(k));
      CheckNear(uv[2] * atlas.width(), r.size, 1e-3, "uv width of tile " + std::to_string(k));
      CheckNear(uv[3] * atlas.height(), r.size, 1e-3, "uv height of tile " + std::to_string(k));
    }
    Check(ls::TileAt(atlas, views).size == 0, "a tile past the last view is empty");
  }
  // The four tiles of a 2x2 atlas: left to right, top to bottom.
  const ls::AtlasLayout a = ls::MakeAtlas(512, 4);
  Check(ls::TileAt(a, 0).x == 0 && ls::TileAt(a, 0).y == 0 && ls::TileAt(a, 1).x == 512 && ls::TileAt(a, 1).y == 0 && ls::TileAt(a, 2).x == 0 &&
            ls::TileAt(a, 2).y == 512 && ls::TileAt(a, 3).x == 512 && ls::TileAt(a, 3).y == 512,
        "tiles go left to right, top to bottom");
}

// ---------------------------------------------------------------------------
// lights, groups, views
// ---------------------------------------------------------------------------

ls::Light Lamp(double x, double y, double z, double dx, double dy, double dz, uint32_t car, bool player, uint32_t index, double cos_outer = 0.8,
               double range = 40.0) {
  const float pos[3] = {float(x), float(y), float(z)};
  const float dir[3] = {float(dx), float(dy), float(dz)};
  ls::Light l;
  const bool ok = ls::MakeLight(pos, dir, float(cos_outer), float(range), car, player, index, &l);
  Check(ok, "a good light is accepted");
  return l;
}

void CheckLights() {
  ls::Light l;
  const float pos[3] = {1, 2, 3};
  const float dir[3] = {0, 0, 2};
  Check(ls::MakeLight(pos, dir, 0.8f, 30.0f, 7, true, 2, &l), "a light is made");
  CheckNear(l.dir[2], 1.0, 1e-12, "the direction is normalised");
  Check(l.index == 2 && l.car_key == 7 && l.is_player, "its place, car and player flag are kept");
  const float zero[3] = {0, 0, 0};
  Check(!ls::MakeLight(pos, zero, 0.8f, 30.0f, 7, true, 0, &l), "a light with no direction is refused");
  const float nan_pos[3] = {NAN, 0, 0};
  Check(!ls::MakeLight(nan_pos, dir, 0.8f, 30.0f, 7, true, 0, &l), "a light with a position that is not a number is refused");
  Check(!ls::MakeLight(pos, dir, 1.0f, 30.0f, 7, true, 0, &l) && !ls::MakeLight(pos, dir, -1.0f, 30.0f, 7, true, 0, &l), "a cone of 0 or 180 degrees is refused");
  Check(ls::MakeLight(pos, dir, 0.8f, 0.0f, 7, true, 0, &l) && l.range == ls::kDefaultRange, "no range: the default");
  Check(ls::MakeLight(pos, dir, 0.8f, 5000.0f, 7, true, 0, &l) && l.range == ls::kMaxRange, "a range is clamped");
  Check(ls::MakeLight(pos, dir, 0.8f, 1.0f, 7, true, 0, &l) && l.range == ls::kMinRange, "a short range is raised to the least");
}

void CheckGroups() {
  // The player's two lamps (car 1), an opponent's pair (car 2, 30 m ahead) and a traffic car's single lamp (car 3, 12 m off).
  ls::Light lights[4] = {
      Lamp(-0.7, 0.7, 0, 0, 0, 1, 2, false, 0), Lamp(-0.7, 0.7, 30, 0, 0, -1, 2, false, 1),  // placeholders, replaced below
      Lamp(0, 0, 0, 0, 0, 1, 3, false, 2), Lamp(0, 0, 0, 0, 0, 1, 3, false, 3)};
  lights[0] = Lamp(-0.7, 0.7, 0, 0, 0, 1, 1, true, 0);
  lights[1] = Lamp(0.7, 0.7, 0, 0, 0, 1, 1, true, 1);
  lights[2] = Lamp(10, 0.7, 30, 0, 0, -1, 2, false, 2);
  lights[3] = Lamp(12, 0.7, 12, 1, 0, 0, 3, false, 3);
  ls::Group groups[4];
  uint32_t n = ls::GroupLights(lights, 4, 4, groups);
  Check(n == 3, "three cars, three groups");
  Check(groups[0].is_player && groups[0].count == 2 && groups[0].members[0] == 0 && groups[0].members[1] == 1, "the player's two lamps share the first view");
  Check(groups[1].car_key == 3 && groups[2].car_key == 2, "the others follow nearest first (12 m before 30 m)");
  CheckNear(groups[0].eye[0], 0.0, 1e-9, "the eye is the mean of the lamps (x)");
  CheckNear(groups[0].eye[1], 0.7, 1e-6, "the eye is the mean of the lamps (y)");
  Check(ls::GroupLights(lights, 4, 1, groups) == 1 && groups[0].is_player, "one view: the player's");
  Check(ls::GroupLights(lights, 4, 2, groups) == 2 && groups[1].car_key == 3, "two views: the player's and the nearest other car's");
  Check(ls::GroupLights(lights, 4, 0, groups) == 0, "no view: no group");
  // No light of the player's: the first group is the reference and keeps its place.
  ls::Light others[2] = {Lamp(50, 0, 0, 0, 0, 1, 5, false, 0), Lamp(0, 0, 0, 0, 0, 1, 6, false, 1)};
  Check(ls::GroupLights(others, 2, 2, groups) == 2 && groups[0].car_key == 5 && groups[1].car_key == 6, "with no player's lamp the first light's car is the reference");
  // Four lamps of one car are one group.
  ls::Light same[4] = {Lamp(0, 0, 0, 0, 0, 1, 9, true, 0), Lamp(1, 0, 0, 0, 0, 1, 9, true, 1), Lamp(2, 0, 0, 0, 0, 1, 9, true, 2), Lamp(3, 0, 0, 0, 0, 1, 9, true, 3)};
  Check(ls::GroupLights(same, 4, 4, groups) == 1 && groups[0].count == 4, "four lamps of one car are one view");
}

// A point is inside the view's frustum (D3D clip space).
bool InsideClip(const tp::Mat4& clip, const double p[3]) {
  double o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = p[0] * clip.m[0][j] + p[1] * clip.m[1][j] + p[2] * clip.m[2][j] + clip.m[3][j];
  }
  return o[3] > 0 && std::fabs(o[0]) <= o[3] + 1e-9 && std::fabs(o[1]) <= o[3] + 1e-9 && o[2] >= -1e-9 && o[2] <= o[3] + 1e-9;
}

void CheckFit() {
  // The player's lamps 1.4 m apart, shining 20 degrees off the car's axis outwards? No: straight ahead (+z), a cone of 30 degrees.
  ls::Light lights[2] = {Lamp(-0.7, 0.7, 0, 0, 0, 1, 1, true, 0, std::cos(ls::Rad(30)), 40.0), Lamp(0.7, 0.7, 0, 0, 0, 1, 1, true, 1, std::cos(ls::Rad(30)), 40.0)};
  ls::Group groups[4];
  ls::GroupLights(lights, 2, 1, groups);
  ls::ViewSpec spec;
  Check(ls::FitView(lights, groups[0], &spec), "a view is fitted");
  CheckNear(spec.dir[2], 1.0, 1e-9, "the view looks the way the lamps do");
  Check(spec.eye[2] < 0.0, "the eye sits behind the lamps (they are inside the frustum)");
  CheckNear(spec.eye[0], 0.0, 1e-9, "centred between the lamps");
  Check(spec.near_m > ls::kNearMeters && Near(spec.near_m - spec.lamps_m, ls::kNearMeters, 1e-9), "the near plane is kNearMeters in front of the lamps");
  Check(Near(spec.far_m - spec.lamps_m, 40.0, 1e-9), "the far plane is the lamps' range in front of them");
  Check(spec.fov > 2.0 * std::acos(std::cos(ls::Rad(30))) && spec.fov < ls::Rad(150), "the field of view holds the cone and a margin");
  for (bool rh : {true, false}) {
    tp::Mat4 clip;
    Check(ls::LightClip(spec, rh, &clip), "the clip matrix is made");
    // Each lamp's own cone reaches the far plane inside the frustum: its axis points and the points on its cone's rim at 10 m and 30 m.
    for (const ls::Light& lamp : lights) {
      for (double dist : {3.0, 10.0, 30.0}) {
        const double theta = std::acos(lamp.cos_outer) * 0.98;
        for (double around : {0.0, 1.5708, 3.1416, 4.7124}) {
          // The rim direction: the axis turned by theta toward `around` about +z.
          const double dir[3] = {std::sin(theta) * std::cos(around), std::sin(theta) * std::sin(around), std::cos(theta)};
          const double p[3] = {lamp.pos[0] + dir[0] * dist, lamp.pos[1] + dir[1] * dist, lamp.pos[2] + dir[2] * dist};
          Check(InsideClip(clip, p), std::string("the lamp's cone at ") + std::to_string(dist) + " m is inside the view (" + (rh ? "right" : "left") + " handed)");
        }
      }
    }
  }
  // One lamp straight down (a street lamp's shape, a view that looks along -y): the basis must not collapse.
  ls::Light down = Lamp(5, 8, 5, 0, -1, 0, 4, false, 0, std::cos(ls::Rad(40)), 20.0);
  ls::Group dg;
  ls::GroupLights(&down, 1, 1, &dg);
  ls::ViewSpec dspec;
  tp::Mat4 dclip;
  Check(ls::FitView(&down, dg, &dspec) && ls::LightClip(dspec, true, &dclip), "a lamp that looks straight down gets a view");
  const double under[3] = {5, 1, 5};
  Check(InsideClip(dclip, under), "a point under the lamp is inside its view");
  // A degenerate view is refused.
  ls::ViewSpec bad = spec;
  bad.far_m = bad.near_m;
  tp::Mat4 unused;
  Check(!ls::LightClip(bad, true, &unused), "far not beyond near is refused");
  bad = spec;
  bad.fov = 0;
  Check(!ls::LightClip(bad, true, &unused), "no field of view is refused");
}

// ---------------------------------------------------------------------------
// the published matrix: a known point lands in the right atlas texel
// ---------------------------------------------------------------------------

// The texel (x, y in atlas pixels) a point lands in, by the long way: clip -> NDC -> viewport of the tile, Direct3D conventions (y up in NDC,
// the first texel row on top), with the pass's NDC signs.
bool Reference(const tp::Mat4& clip, const ls::AtlasLayout& atlas, uint32_t view, const double p[3], double sx, double sy, double* px, double* py, double* depth) {
  double o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = p[0] * clip.m[0][j] + p[1] * clip.m[1][j] + p[2] * clip.m[2][j] + clip.m[3][j];
  }
  if (!(o[3] > 0)) {
    return false;
  }
  const double nx = sx * o[0] / o[3];
  const double ny = sy * o[1] / o[3];
  const ls::TileRect t = ls::TileAt(atlas, view);
  *px = t.x + (nx * 0.5 + 0.5) * t.size;
  *py = t.y + (0.5 - ny * 0.5) * t.size;
  *depth = o[2] / o[3];
  return true;
}

void CheckWorldToShadow() {
  const ls::AtlasLayout atlas = ls::MakeAtlas(512, 4);
  ls::Light lamp = Lamp(100.0, 1.0, -200.0, 0.6, 0.0, 0.8, 1, true, 0, std::cos(ls::Rad(30)), 50.0);
  ls::Group g;
  ls::GroupLights(&lamp, 1, 1, &g);
  ls::ViewSpec spec;
  Check(ls::FitView(&lamp, g, &spec), "the view of the point test");
  for (bool rh : {true, false}) {
    for (double sx : {1.0, -1.0}) {
      for (double sy : {1.0, -1.0}) {
        for (uint32_t view = 0; view < 4; ++view) {
          tp::Mat4 clip;
          Check(ls::LightClip(spec, rh, &clip), "clip");
          float rect[4];
          ls::UvRect(atlas, view, rect);
          float m[16];
          ls::WorldToShadow(clip, rect, sx, sy, m);
          // Points on the axis, to the side, above, and one behind the light.
          const double axis[3] = {lamp.pos[0] + lamp.dir[0] * 10.0, lamp.pos[1], lamp.pos[2] + lamp.dir[2] * 10.0};
          double right[3] = {lamp.dir[2], 0.0, -lamp.dir[0]};  // some horizontal perpendicular
          const double pts[4][3] = {{axis[0], axis[1], axis[2]},
                                    {axis[0] + right[0] * 2.0, axis[1], axis[2] + right[2] * 2.0},
                                    {axis[0], axis[1] + 1.5, axis[2]},
                                    {lamp.pos[0] - lamp.dir[0] * 5.0, lamp.pos[1], lamp.pos[2] - lamp.dir[2] * 5.0}};
          for (int i = 0; i < 4; ++i) {
            double u, v, d, px, py, rd;
            const bool a = ls::MapPoint(m, pts[i], &u, &v, &d);
            const bool b = Reference(clip, atlas, view, pts[i], sx, sy, &px, &py, &rd);
            const std::string at = std::string(rh ? "RH " : "LH ") + "sx" + std::to_string(int(sx)) + " sy" + std::to_string(int(sy)) + " view " + std::to_string(view) +
                                   " point " + std::to_string(i);
            Check(a == b, at + ": behind the light is behind in both");
            if (a && b) {
              CheckNear(u * atlas.width(), px, 2e-2, at + ": atlas x texel");
              CheckNear(v * atlas.height(), py, 2e-2, at + ": atlas y texel");
              CheckNear(d, rd, 1e-6, at + ": depth");
            }
          }
          // The axis lands on the middle of the tile (the view looks along it) for the lamp's own axis through the eye.
          double u, v, d;
          const double on_axis[3] = {spec.eye[0] + spec.dir[0] * 12.0, spec.eye[1] + spec.dir[1] * 12.0, spec.eye[2] + spec.dir[2] * 12.0};
          Check(ls::MapPoint(m, on_axis, &u, &v, &d), "a point on the view's axis");
          const ls::TileRect t = ls::TileAt(atlas, view);
          CheckNear(u * atlas.width(), t.x + t.size * 0.5, 0.05, "the axis is the middle of the tile (x)");
          CheckNear(v * atlas.height(), t.y + t.size * 0.5, 0.05, "the axis is the middle of the tile (y)");
          // The depth is 0 at the near plane and 1 at the far plane.
          const double at_near[3] = {spec.eye[0] + spec.dir[0] * spec.near_m, spec.eye[1] + spec.dir[1] * spec.near_m, spec.eye[2] + spec.dir[2] * spec.near_m};
          const double at_far[3] = {spec.eye[0] + spec.dir[0] * spec.far_m, spec.eye[1] + spec.dir[1] * spec.far_m, spec.eye[2] + spec.dir[2] * spec.far_m};
          Check(ls::MapPoint(m, at_near, &u, &v, &d), "the near plane");
          CheckNear(d, 0.0, 1e-5, "depth is 0 at the near plane");
          Check(ls::MapPoint(m, at_far, &u, &v, &d), "the far plane");
          CheckNear(d, 1.0, 1e-5, "depth is 1 at the far plane");
        }
      }
    }
  }
  // The depth follows A (1 - n / d): a point 10 m along the axis from a lamp whose plane is the near plane's neighbour.
  {
    tp::Mat4 clip;
    ls::LightClip(spec, true, &clip);
    float rect[4];
    ls::UvRect(atlas, 0, rect);
    float m[16];
    ls::WorldToShadow(clip, rect, 1.0, 1.0, m);
    const double dist = 10.0 + spec.lamps_m;  // along the view axis from the eye
    const double p[3] = {spec.eye[0] + spec.dir[0] * dist, spec.eye[1] + spec.dir[1] * dist, spec.eye[2] + spec.dir[2] * dist};
    double u, v, d;
    ls::MapPoint(m, p, &u, &v, &d);
    const double a = spec.far_m / (spec.far_m - spec.near_m);
    CheckNear(d, a * (1.0 - spec.near_m / dist), 1e-6, "depth is f/(f-n) (1 - n/d)");
  }
  // A texel from the numbers a person can follow: a lamp at the origin looking +z, 60 degrees (a 512 tile, one view), a left handed view with
  // near 1 and far 11 (kept from the fit by building the spec by hand): the point (d tan 30, 0, d) is the right edge's middle texel.
  {
    ls::ViewSpec hand;
    hand.eye[0] = hand.eye[1] = hand.eye[2] = 0.0;
    hand.dir[0] = 0.0;
    hand.dir[1] = 0.0;
    hand.dir[2] = 1.0;
    hand.fov = ls::Rad(60.0);
    hand.near_m = 1.0;
    hand.far_m = 11.0;
    tp::Mat4 clip;
    Check(ls::LightClip(hand, false, &clip), "the hand made view");
    const ls::AtlasLayout one = ls::MakeAtlas(512, 1);
    float rect[4];
    ls::UvRect(one, 0, rect);
    float m[16];
    ls::WorldToShadow(clip, rect, 1.0, 1.0, m);
    const double d = 5.0;
    const double edge[3] = {d * std::tan(ls::Rad(30.0)), 0.0, d};
    double u, v, dep;
    Check(ls::MapPoint(m, edge, &u, &v, &dep), "the edge point");
    CheckNear(u * 512.0, 512.0, 1e-3, "the edge of the field of view is the right edge of the tile");
    CheckNear(v * 512.0, 256.0, 1e-3, "... on the middle row");
    const double up[3] = {0.0, d * std::tan(ls::Rad(30.0)) * 0.5, d};
    ls::MapPoint(m, up, &u, &v, &dep);
    CheckNear(u * 512.0, 256.0, 1e-3, "a point above the axis keeps the middle column");
    CheckNear(v * 512.0, 128.0, 1e-3, "... and lands on the upper half of the tile (the first row is the top)");
    CheckNear(dep, (11.0 / 10.0) * (1.0 - 1.0 / 5.0), 1e-6, "its depth, by hand");
  }
}

// ---------------------------------------------------------------------------
// the replay: the camera out of a draw's bank, and c8-c11'
// ---------------------------------------------------------------------------

struct Mat {
  tp::Mat4 m = tp::Identity();
};

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 t = tp::Identity();
  t.m[3][0] = x;
  t.m[3][1] = y;
  t.m[3][2] = z;
  return t;
}

tp::Mat4 RotationY(double degrees) {
  const double c = std::cos(ls::Rad(degrees)), s = std::sin(ls::Rad(degrees));
  tp::Mat4 r = tp::Identity();
  r.m[0][0] = c;
  r.m[0][2] = -s;
  r.m[2][0] = s;
  r.m[2][2] = c;
  return r;
}

// The game's camera: a right handed view (the camera looks along -z), the view inverse with the right, up and backward axes and the position,
// the projection with near 5 and far 10000 (P22 = -f/(f-n), P23 = -1, P32 = -n f/(f-n)).
struct GameCamera {
  tp::Mat4 view_inverse;
  tp::Mat4 view;
  tp::Mat4 projection;
  tp::Mat4 vp;
};

GameCamera MakeCamera(double yaw_degrees, double x, double y, double z, bool right_handed = true) {
  GameCamera c;
  // The camera's frame: the rotation about y, then the position.
  tp::Mat4 rot = RotationY(yaw_degrees);
  c.view_inverse = tp::Identity();
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      c.view_inverse.m[r][k] = rot.m[r][k];
    }
  }
  if (!right_handed) {
    // A left handed camera: the backward axis (z) is mirrored, so that right x up = -backward.
    for (int k = 0; k < 3; ++k) {
      c.view_inverse.m[2][k] = -c.view_inverse.m[2][k];
    }
  }
  c.view_inverse.m[3][0] = x;
  c.view_inverse.m[3][1] = y;
  c.view_inverse.m[3][2] = z;
  tp::Inverse(c.view_inverse, &c.view);
  const double n = 5.0, f = 10000.0, sy = 1.0 / std::tan(ls::Rad(40.0)), sx = sy / (16.0 / 9.0);
  c.projection = tp::Mat4{};
  c.projection.m[0][0] = sx;
  c.projection.m[1][1] = sy;
  c.projection.m[2][2] = -f / (f - n);
  c.projection.m[2][3] = -1.0;
  c.projection.m[3][2] = -n * f / (f - n);
  c.vp = tp::Mul(c.view, c.projection);
  return c;
}

// A bank (256 registers x 4 floats) of a draw: gWorld c0-c3, gWorldViewProj c8-c11 = gWorld x VP, the view inverse c12-c15.
std::vector<float> MakeBank(const GameCamera& cam, const tp::Mat4& world) {
  std::vector<float> bank(256 * 4, 0.0f);
  const tp::Mat4 wvp = tp::Mul(world, cam.vp);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      bank[(0 + r) * 4 + c] = float(world.m[r][c]);
      bank[(8 + r) * 4 + c] = float(wvp.m[r][c]);
      bank[(12 + r) * 4 + c] = float(cam.view_inverse.m[r][c]);
    }
  }
  return bank;
}

void CheckCamera() {
  const GameCamera cam = MakeCamera(35.0, 2903.0, 12.0, -810.0);
  const tp::Mat4 world = tp::Mul(RotationY(70.0), Translation(2910.0, 1.0, -800.0));
  std::vector<float> bank = MakeBank(cam, world);
  const ls::Camera found = ls::DeriveCamera(bank.data());
  Check(found.valid, "the camera is found from a draw's own constants");
  Check(found.right_handed, "a right handed camera is told");
  CheckNear(found.eye[0], 2903.0, 1e-3, "the eye's x");
  // The camera looks along the negative of c14 (the view inverse's backward axis).
  CheckNear(found.forward[0] * found.forward[0] + found.forward[1] * found.forward[1] + found.forward[2] * found.forward[2], 1.0, 1e-9, "the forward axis is a unit vector");
  CheckNear(found.forward[0], -cam.view_inverse.m[2][0], 1e-6, "forward is minus c14 (x)");
  CheckNear(found.forward[2], -cam.view_inverse.m[2][2], 1e-6, "forward is minus c14 (z)");
  if (found.valid) {
    // The view-projection is the game's: a world point goes to the same clip.
    const double p[4] = {2915.0, 3.0, -790.0, 1.0};
    double a[4], b[4];
    for (int j = 0; j < 4; ++j) {
      a[j] = p[0] * found.view_projection.m[0][j] + p[1] * found.view_projection.m[1][j] + p[2] * found.view_projection.m[2][j] + found.view_projection.m[3][j];
      b[j] = p[0] * cam.vp.m[0][j] + p[1] * cam.vp.m[1][j] + p[2] * cam.vp.m[2][j] + cam.vp.m[3][j];
    }
    for (int j = 0; j < 4; ++j) {
      CheckNear(a[j] / std::max(1.0, std::fabs(b[3])), b[j] / std::max(1.0, std::fabs(b[3])), 2e-3, "the derived view-projection is the game's, component " + std::to_string(j));
    }
  }
  // A left handed camera is told too.
  const GameCamera lh = MakeCamera(10.0, 100.0, 5.0, 100.0, false);
  const ls::Camera lh_found = ls::DeriveCamera(MakeBank(lh, Translation(110, 0, 90)).data());
  Check(!lh_found.valid || !lh_found.right_handed, "a mirrored camera is not told right handed");
  // A draw whose c0-c3 is not its world matrix (here a skewed garbage matrix) gives no camera.
  std::vector<float> bad = bank;
  for (int c = 0; c < 4; ++c) {
    bad[0 * 4 + c] = float(c == 0 ? 3.0 : 0.0);
    bad[1 * 4 + c] = float(c == 1 ? 0.0 : 0.0);  // a singular matrix
    bad[2 * 4 + c] = 0.0f;
    bad[3 * 4 + c] = 0.0f;
  }
  Check(!ls::DeriveCamera(bad.data()).valid, "a draw with no world matrix gives no camera");
  // The view inverse of a draw that never set c12-c15 (zeros) gives none.
  std::vector<float> zero = bank;
  for (int r = 12; r < 16; ++r) {
    for (int c = 0; c < 4; ++c) {
      zero[r * 4 + c] = 0.0f;
    }
  }
  Check(!ls::DeriveCamera(zero.data()).valid, "no view inverse: no camera");
  // c8-c11 that is not world x VP (a different projection) fails the projection's zeros.
  std::vector<float> junk = bank;
  for (int c = 0; c < 4; ++c) {
    junk[8 * 4 + c] = float(1.0 + c);
    junk[9 * 4 + c] = float(2.0 - c);
    junk[10 * 4 + c] = float(c * c);
    junk[11 * 4 + c] = float(3 + c);
  }
  Check(!ls::DeriveCamera(junk.data()).valid, "c8-c11 that is not world x view-projection gives no camera");
}

void CheckReplay() {
  const GameCamera cam = MakeCamera(-20.0, 2903.0, 14.0, -810.0);
  // The camera from one draw, then other draws through it.
  const tp::Mat4 world0 = tp::Mul(RotationY(33.0), Translation(2890.0, 0.5, -820.0));
  const ls::Camera camera = ls::DeriveCamera(MakeBank(cam, world0).data());
  Check(camera.valid, "the camera of the replay");
  ls::Light lamps[2] = {Lamp(2900.0, 1.0, -815.0, 0.3, -0.05, 0.95, 1, true, 0, std::cos(ls::Rad(30)), 45.0), Lamp(2901.4, 1.0, -815.0, 0.3, -0.05, 0.95, 1, true, 1, std::cos(ls::Rad(30)), 45.0)};
  ls::Group g;
  ls::GroupLights(lamps, 2, 1, &g);
  ls::ViewSpec spec;
  ls::FitView(lamps, g, &spec);
  tp::Mat4 light_clip, replay;
  Check(ls::LightClip(spec, camera.right_handed, &light_clip), "the light clip");
  Check(ls::ReplayMatrix(camera, light_clip, &replay), "the replay matrix");
  // A draw: gWorld of a prop 8 m ahead of the lamps, and some model points. c8-c11' x model point = light clip of the world point.
  const tp::Mat4 prop = tp::Mul(RotationY(120.0), Translation(2902.0, 0.0, -808.0));
  const std::vector<float> bank = MakeBank(cam, prop);
  const ls::WvpFacts facts = ls::CheckWvp(bank.data() + 8 * 4, camera);
  Check(facts.ok, "a draw of the main view passes the world check");
  CheckNear(facts.origin[0], 2902.0, 1e-2, "its origin's x is the world matrix's translation");
  CheckNear(facts.origin[2], -808.0, 1e-2, "its origin's z is the world matrix's translation");
  float rows[16];
  ls::ReplaceWvp(bank.data() + 8 * 4, replay, rows);
  const double model[5][3] = {{0, 0, 0}, {0.5, 1.2, -0.3}, {-1.0, 2.0, 0.7}, {0.0, 3.0, 1.0}, {2.0, 0.0, -2.0}};
  for (const auto& mp : model) {
    // world point
    double w[3];
    for (int k = 0; k < 3; ++k) {
      w[k] = mp[0] * prop.m[0][k] + mp[1] * prop.m[1][k] + mp[2] * prop.m[2][k] + prop.m[3][k];
    }
    double via_rows[4], direct[4];
    for (int j = 0; j < 4; ++j) {
      via_rows[j] = mp[0] * rows[0 * 4 + j] + mp[1] * rows[1 * 4 + j] + mp[2] * rows[2 * 4 + j] + rows[3 * 4 + j];
      direct[j] = w[0] * light_clip.m[0][j] + w[1] * light_clip.m[1][j] + w[2] * light_clip.m[2][j] + light_clip.m[3][j];
    }
    const double scale = std::max(1.0, std::fabs(direct[3]));
    for (int j = 0; j < 4; ++j) {
      CheckNear(via_rows[j] / scale, direct[j] / scale, 5e-3, "c8-c11' takes a model point to the light's clip, component " + std::to_string(j));
    }
  }
  // The replaced rows keep float precision at the map's own scale: a point 20 m from the lamp is inside a tile's texel to a fraction of one.
  {
    const double mp[3] = {0.0, 1.0, 0.0};
    double w[3];
    for (int k = 0; k < 3; ++k) {
      w[k] = mp[0] * prop.m[0][k] + mp[1] * prop.m[1][k] + mp[2] * prop.m[2][k] + prop.m[3][k];
    }
    double o_rows[4], o_direct[4];
    for (int j = 0; j < 4; ++j) {
      o_rows[j] = mp[0] * rows[0 * 4 + j] + mp[1] * rows[1 * 4 + j] + mp[2] * rows[2 * 4 + j] + rows[3 * 4 + j];
      o_direct[j] = w[0] * light_clip.m[0][j] + w[1] * light_clip.m[1][j] + w[2] * light_clip.m[2][j] + light_clip.m[3][j];
    }
    const double ndc_rows = o_rows[0] / o_rows[3], ndc_direct = o_direct[0] / o_direct[3];
    // 1 texel of a 512 tile is 2/512 of NDC.
    Check(std::fabs(ndc_rows - ndc_direct) < 2.0 / 512.0 * 0.5, "the replaced matrix lands within half a texel at 3000 m from the origin");
  }
  // The writer and the reader agree: the texel the replayed vertex lands in (c8-c11' -> NDC -> the tile's viewport) is the texel the published
  // matrix gives its world point, in a tile of a 4 view atlas, with the NDC signs the main view had.
  {
    const ls::AtlasLayout atlas = ls::MakeAtlas(512, 4);
    for (uint32_t view = 0; view < 4; ++view) {
      float rect[4];
      ls::UvRect(atlas, view, rect);
      float published[16];
      ls::WorldToShadow(light_clip, rect, 1.0, 1.0, published);
      const ls::TileRect tile = ls::TileAt(atlas, view);
      for (const auto& mp : model) {
        double w[3];
        for (int k = 0; k < 3; ++k) {
          w[k] = mp[0] * prop.m[0][k] + mp[1] * prop.m[1][k] + mp[2] * prop.m[2][k] + prop.m[3][k];
        }
        double c[4];
        for (int j = 0; j < 4; ++j) {
          c[j] = mp[0] * rows[0 * 4 + j] + mp[1] * rows[1 * 4 + j] + mp[2] * rows[2 * 4 + j] + rows[3 * 4 + j];
        }
        if (!(c[3] > 0.5)) {
          continue;  // behind the light: not rasterized
        }
        const double px = tile.x + (c[0] / c[3] * 0.5 + 0.5) * tile.size;
        const double py = tile.y + (0.5 - c[1] / c[3] * 0.5) * tile.size;
        double u, v, d;
        if (ls::MapPoint(published, w, &u, &v, &d)) {
          CheckNear(u * atlas.width(), px, 0.5, "the reader's texel x is the writer's, tile " + std::to_string(view));
          CheckNear(v * atlas.height(), py, 0.5, "the reader's texel y is the writer's, tile " + std::to_string(view));
          CheckNear(d, c[2] / c[3], 1e-3, "the reader's depth is the writer's, tile " + std::to_string(view));
        }
      }
    }
  }
  // A draw whose vertex shader is not of the kind: c8-c11 is a screen space matrix.
  std::vector<float> ui = bank;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      ui[(8 + r) * 4 + c] = float(r == c ? 1.0 : 0.0);
    }
  }
  Check(!ls::CheckWvp(ui.data() + 8 * 4, camera).ok, "c8-c11 = identity (a screen space draw) is not a world matrix times the camera");
  // The check cannot tell a camera of another frame with the same lens (a rigid motion of the camera keeps the last column): VP_main must come
  // from the frame it is used in (local_shadow.cpp derives it from the frame's own stored banks at the history trigger). A draw of a camera with
  // ANOTHER lens does fail it.
  GameCamera wide = MakeCamera(-20.0, 2903.0, 14.0, -810.0);
  const double n2 = 0.5, f2 = 300.0;  // another near and far (a sky dome's or a first person pass's own projection)
  wide.projection.m[2][2] = -f2 / (f2 - n2);
  wide.projection.m[3][2] = -n2 * f2 / (f2 - n2);
  wide.vp = tp::Mul(wide.view, wide.projection);
  Check(!ls::CheckWvp(MakeBank(wide, prop).data() + 8 * 4, camera).ok, "a draw of a camera with another near and far fails the check");
}

// ---------------------------------------------------------------------------
// the casters
// ---------------------------------------------------------------------------

// Representative vertex shaders as the pack names them (group__entry), with what the pack says of them at run time.
struct Shader {
  const char* name;
  bool computed_fetch;
  bool skinned;
  ls::Skip skip;      // for an opaque draw of this shader
  ls::Kind kind;      // when it is drawn
};

void CheckClassification() {
  // The world's own draws write stencil ID 0 (bit 0 of StencilIdsWritten); only IDs 1 to 23 are vehicles (the headlights run of 2026-11-20 counted every world draw as a
  // vehicle: opaque 0 in 14000 maps).
  Check(!ls::WritesVehicleId(0u), "a draw with no stencil ID is no vehicle's");
  Check(!ls::WritesVehicleId(1u), "a draw that writes ID 0 (the world's) is no vehicle's");
  Check(ls::WritesVehicleId(1u << 1), "a draw that writes ID 1 (the player's car) is a vehicle's");
  Check(ls::WritesVehicleId((1u << 0) | (1u << 5)), "ID 0 and ID 5 together: a vehicle's");
  Check(ls::WritesVehicleId(1u << 23), "ID 23 is a vehicle's");
  {
    ls::DrawFacts world;
    world.color_pass = true;
    world.color_count = 1;
    world.depth_test = world.depth_write = world.has_ps = world.triangles = true;
    world.vehicle = ls::WritesVehicleId(1u);  // an opaque city draw: ID 0 only
    ls::Kind kind;
    Check(ls::ClassifyDraw(world, 0, &kind) == ls::Skip::kNone && kind == ls::Kind::kOpaque, "an opaque city draw is an opaque caster, not a vehicle's part");
  }
  const Shader shaders[] = {
      {"xCityDefault__VSCityDefault", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xCityBumpSpec__VSCityBumpSpec", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xCityRoad__VSCityRoad", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xPropDefault__VSPropDefault", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xPropMetal__VSPropMetal", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xCityFoliage__VSCityFoliage", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xPropFoliage__VSPropFoliage", false, false, ls::Skip::kNone, ls::Kind::kOpaque},
      {"xPropFoliage__VSPropFoliageSkinned", false, true, ls::Skip::kNone, ls::Kind::kSkinned},
      {"xPropFoliageTrunk__VSPropFoliageTrunkSkinned", false, true, ls::Skip::kNone, ls::Kind::kSkinned},
      {"xCharacter_normalmap__VSSkinned_Wind", false, true, ls::Skip::kNone, ls::Kind::kSkinned},
      // The foliage impostors fold four corners onto one source vertex (the pack's computed fetch index flag): left out.
      {"xPropFoliageImpostor__VSPropInstanceFoliage", true, false, ls::Skip::kImpostor, ls::Kind::kOpaque},
      {"xPropFoliageImpostor__VSPropInstanceFoliageNight", true, false, ls::Skip::kImpostor, ls::Kind::kOpaque},
      {"xrain_system__ParticleRenderVS", true, false, ls::Skip::kImpostor, ls::Kind::kOpaque},
  };
  for (const Shader& s : shaders) {
    ls::DrawFacts f;
    f.color_pass = true;
    f.color_count = 1;
    f.depth_test = f.depth_write = f.has_ps = f.triangles = true;
    f.vs_computed_fetch = s.computed_fetch;
    f.vs_skinned = s.skinned;
    f.vs_key = 0x1000 + uint64_t(&s - shaders);
    ls::Kind kind;
    const ls::Skip got = ls::ClassifyDraw(f, 0, &kind);
    Check(got == s.skip, std::string(s.name) + ": " + ls::SkipName(got) + " expected " + ls::SkipName(s.skip));
    if (got == ls::Skip::kNone) {
      Check(kind == s.kind, std::string(s.name) + ": kind " + ls::KindName(kind));
    }
    // The same shader with an alpha test: alpha tested unless it is skinned or a vehicle's (those keep their kind).
    f.alpha_tested = true;
    const ls::Skip alpha = ls::ClassifyDraw(f, 0, &kind);
    Check(alpha == s.skip, std::string(s.name) + ": the alpha test changes no verdict");
    if (alpha == ls::Skip::kNone) {
      Check(kind == (s.skinned ? ls::Kind::kSkinned : ls::Kind::kAlphaTested), std::string(s.name) + ": alpha tested kind");
    }
  }
  // Not opaque draws.
  ls::DrawFacts base;
  base.color_pass = true;
  base.color_count = 1;
  base.depth_test = base.depth_write = base.has_ps = base.triangles = true;
  ls::Kind kind;
  Check(ls::ClassifyDraw(base, 0, &kind) == ls::Skip::kNone && kind == ls::Kind::kOpaque, "the plain opaque draw");
  ls::DrawFacts f = base;
  f.blend0 = true;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNotOpaque, "a blended draw is not a caster");
  f = base;
  f.depth_write = false;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNotOpaque, "a draw that writes no depth is not a caster");
  f = base;
  f.color_pass = false;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNotOpaque, "a draw outside the main view's colour pass is not a caster");
  f = base;
  f.has_ps = false;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNotOpaque, "a depth only draw (the pre-pass) is not collected twice");
  f = base;
  f.triangles = false;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNotTriangles, "lines and points draw nothing");
  f = base;
  f.ps_writes_depth = true;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kPsDepth, "a pixel shader that writes depth is left out");
  f = base;
  f.vehicle = true;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNone && kind == ls::Kind::kVehicle, "a vehicle's part is a caster of its own kind");
  f.vs_skinned = true;
  Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kNone && kind == ls::Kind::kVehicle, "a skinned vehicle part (the driver) stays a vehicle's");
  // The debug mask.
  f = base;
  f.vs_skinned = true;
  Check(ls::ClassifyDraw(f, ls::KindBit(ls::Kind::kSkinned), &kind) == ls::Skip::kKindMasked, "the skinned bit masks skinned parts");
  Check(ls::ClassifyDraw(base, ls::KindBit(ls::Kind::kSkinned), &kind) == ls::Skip::kNone, "... and nothing else");
  f = base;
  f.alpha_tested = true;
  Check(ls::ClassifyDraw(f, ls::KindBit(ls::Kind::kAlphaTested), &kind) == ls::Skip::kKindMasked, "the alpha bit masks alpha tested draws");
  f.vs_skinned = true;
  Check(ls::ClassifyDraw(f, ls::KindBit(ls::Kind::kAlphaTested), &kind) == ls::Skip::kKindMasked, "... skinned ones too");
  f = base;
  f.vehicle = true;
  Check(ls::ClassifyDraw(f, ls::KindBit(ls::Kind::kVehicle), &kind) == ls::Skip::kKindMasked, "the vehicle bit masks vehicles");
  // The billboard table: sorted identities of the game's two billboard shaders and the foliage impostors in the main view; each is left out by
  // identity even when the pack does not flag its fetch (the billboards), and a different identity is not touched.
  uint64_t last = 0;
  for (const ls::NamedVs& v : ls::kBillboardVertexShaders) {
    Check(v.key > last, std::string(v.name) + ": the billboard identities are sorted and distinct");
    last = v.key;
    Check(ls::IsBillboardVs(v.key), std::string(v.name) + " is a billboard by identity");
    ls::DrawFacts f = base;
    f.vs_key = v.key;
    ls::Kind kind;
    const ls::Skip got = ls::ClassifyDraw(f, 0, &kind);
    Check(got == ls::Skip::kBillboard, std::string(v.name) + ": left out as a billboard (" + ls::SkipName(got) + ")");
    f.vs_computed_fetch = true;
    Check(ls::ClassifyDraw(f, 0, &kind) == ls::Skip::kImpostor, std::string(v.name) + ": with the pack's flag, an impostor");
    Check(std::string(v.name).find("Billboard") != std::string::npos || std::string(v.name).find("Impostor") != std::string::npos, "the list holds billboards and impostors only");
  }
  Check(!ls::IsBillboardVs(0) && !ls::IsBillboardVs(0x1234), "identity 0 and a stranger are never billboards");
}

void CheckSelection() {
  const double eye[3] = {0, 0, 0};
  const double near_car[3] = {1.0, 0.0, 2.0};
  const double far_car[3] = {0.0, 0.0, 12.0};
  Check(ls::IsOwnCar(near_car, eye) && !ls::IsOwnCar(far_car, eye), "a vehicle part within 3.5 m of the eye is the lamps' own car");
  // Selection: the own car's parts go, the rest stay in draw order; over the budget the farthest go.
  std::vector<ls::Candidate> cands;
  for (uint32_t i = 0; i < 10; ++i) {
    ls::Candidate c;
    c.index = i;
    c.distance = double(i) * 5.0;
    c.vehicle = (i == 0 || i == 1);  // two parts of the car at the lamps (0 m and 5 m: only the first is inside the radius)
    cands.push_back(c);
  }
  uint32_t skipped[size_t(ls::Skip::kCount)] = {};
  std::vector<uint32_t> keep(cands.size());
  uint32_t n = ls::SelectForView(cands.data(), uint32_t(cands.size()), 100, keep.data(), skipped);
  Check(n == 9 && skipped[size_t(ls::Skip::kOwnCar)] == 1, "one own part is dropped, nine are kept");
  Check(keep[0] == 1 && std::is_sorted(keep.begin(), keep.begin() + n), "the rest are kept in draw order");
  uint32_t skipped2[size_t(ls::Skip::kCount)] = {};
  n = ls::SelectForView(cands.data(), uint32_t(cands.size()), 4, keep.data(), skipped2);
  Check(n == 4 && skipped2[size_t(ls::Skip::kBudget)] == 5 && skipped2[size_t(ls::Skip::kOwnCar)] == 1, "over the budget the farthest five go");
  Check(std::is_sorted(keep.begin(), keep.begin() + n), "the budgeted ones stay in draw order");
  Check(keep[0] == 1 && keep[1] == 2 && keep[2] == 3 && keep[3] == 4, "the nearest four of the rest are kept");
  // A non-vehicle at the lamps is not dropped: only a vehicle's own parts are (a pole beside the lamp casts, the near plane takes the rest).
  ls::Candidate pole;
  pole.distance = 0.5;
  uint32_t s3[size_t(ls::Skip::kCount)] = {};
  uint32_t k3[1];
  Check(ls::SelectForView(&pole, 1, 8, k3, s3) == 1, "a pole at the lamps is kept");
  Check(ls::kMaxDrawsPerView >= 512, "the budget holds more than a street's casters");
}

void CheckViewCull() {
  // A view from the origin looking along +z with a 76 degree field, from 1.5 m to 70 m (the usual headlight, no offset behind the lamps).
  ls::ViewSpec spec;
  spec.eye[0] = spec.eye[1] = spec.eye[2] = 0.0;
  spec.dir[0] = 0.0;
  spec.dir[1] = 0.0;
  spec.dir[2] = 1.0;
  spec.fov = ls::Rad(76.0);
  spec.near_m = 1.5;
  spec.far_m = 70.0;
  const double ahead[3] = {0.0, 0.0, 30.0};
  const double behind[3] = {0.0, 0.0, -30.0};
  const double too_far[3] = {0.0, 0.0, 140.0};
  const double beyond[3] = {0.0, 0.0, 85.0};
  const double side[3] = {60.0, 0.0, 20.0};
  const double edge[3] = {20.0, 0.0, 30.0};      // tan 33.7 degrees: just inside the square's side at 30 m (the half width there is 23.4 m)
  const double corner[3] = {30.0, 10.0, 30.0};   // outside the square at 30 m (half width 23.4 m), inside the cone that holds it (33.2 m from the axis)
  Check(ls::SphereInView(spec, ahead, 0.0), "a point ahead is in the view");
  Check(!ls::SphereInView(spec, behind, 0.0), "a point behind the eye is out");
  Check(ls::SphereInView(spec, behind, ls::kWorldReach), "... but not with a margin that reaches the plane of the eye");
  Check(!ls::SphereInView(spec, behind, ls::kVehicleReach), "... and a car's margin does not reach from 30 m behind");
  Check(!ls::SphereInView(spec, too_far, ls::kWorldReach), "a thing 140 m away is out even with the world's margin");
  Check(ls::SphereInView(spec, beyond, ls::kWorldReach) && !ls::SphereInView(spec, beyond, ls::kVehicleReach),
        "85 m out: a building's margin reaches the far plane, a car's does not");
  Check(!ls::SphereInView(spec, side, ls::kVehicleReach), "60 m to the side at 20 m ahead is out for a car");
  Check(ls::SphereInView(spec, side, ls::kWorldReach), "... and in for a thing that reaches 40 m");
  Check(ls::SphereInView(spec, edge, 0.0), "a point at the edge of the field is in");
  Check(ls::SphereInView(spec, corner, 0.0), "the cone holds what the square's corners reach (conservative)");
  const double clear_miss[3] = {40.0, 40.0, 30.0};
  Check(!ls::SphereInView(spec, clear_miss, 0.0), "... and nothing farther out");
  // A cone is a test of angles: along a sweep of the point's side offset at 30 m the answer flips once, between the corner's reach and a clear miss.
  int flips = 0;
  bool last = true;
  for (int i = 0; i <= 100; ++i) {
    const double p[3] = {double(i), 0.0, 30.0};
    const bool in = ls::SphereInView(spec, p, 1.0);
    flips += in != last ? 1 : 0;
    last = in;
  }
  Check(flips == 1 && !last, "the answer flips once going out sideways");
  // The lamps sit inside the frustum, an offset behind them: the near plane is measured from the eye.
  ls::ViewSpec offset = spec;
  offset.near_m = 3.0;
  offset.far_m = 75.0;
  const double just_in_front[3] = {0.0, 0.0, 2.0};
  Check(!ls::SphereInView(offset, just_in_front, 0.0), "a point nearer than the (offset) near plane is out");
  Check(ls::SphereInView(offset, just_in_front, 2.0), "... with a margin it can reach past it");
  // The same test through FitView: the cull follows the view the rules make, for a car's two lamps, and nothing in front of the car within its
  // reach is lost.
  ls::Light left, right;
  const float lp[3] = {-0.6f, 0.7f, 2.0f}, rp[3] = {0.6f, 0.7f, 2.0f}, dir[3] = {0.0f, 0.0f, 1.0f};
  Check(ls::MakeLight(lp, dir, 0.788f, 70.0f, 5, true, 0, &left) && ls::MakeLight(rp, dir, 0.788f, 70.0f, 5, true, 1, &right), "two lamps make lights");
  ls::Light lights[2] = {left, right};
  ls::Group groups[4];
  const uint32_t made = ls::GroupLights(lights, 2, 2, groups);
  ls::ViewSpec fitted;
  Check(made == 1 && ls::FitView(lights, groups[0], &fitted), "one view for the car's lamps");
  bool every_lit_point_is_kept = true;
  for (double z = 4.0; z <= 69.0; z += 3.0) {  // from 2 m ahead of the lamps: the near plane takes what is nearer
    for (double x = -30.0; x <= 30.0; x += 3.0) {
      // A point the light's cone reaches (cos of the angle off the axis above the outer cosine, measured from the left lamp).
      const double v[3] = {x - lp[0], 0.0 - lp[1] + 0.5, z - lp[2]};
      const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (len > 1.0 && len < 70.0 && v[2] / len > 0.788) {
        const double world[3] = {x, 0.5, z};
        every_lit_point_is_kept = every_lit_point_is_kept && ls::SphereInView(fitted, world, 0.0);
      }
    }
  }
  Check(every_lit_point_is_kept, "every point the lamp's cone reaches is inside the fitted view's cull");
  // The published rectangle is u0 v0 u1 v1 (the shader compares the point with it), not the offset and size UvRect gives.
  ls::AtlasLayout atlas = ls::MakeAtlas(512, 2);
  float uv[4], rect[4];
  ls::UvRect(atlas, 1, uv);
  ls::PublishedRect(uv, rect);
  CheckNear(rect[0], 0.5, 1e-6, "the second tile starts at u 0.5");
  CheckNear(rect[1], 0.0, 1e-6, "... at v 0");
  CheckNear(rect[2], 1.0, 1e-6, "... and ends at u 1");
  CheckNear(rect[3], 1.0, 1e-6, "... and v 1");
  Check(uv[2] == 0.5f, "(the uv rectangle itself is an offset and a size)");
  Check(ls::SkipName(ls::Skip::kOutOfView)[0] != '?', "the cull has a name in the log");
}

void CheckCullAndKeys() {
  using ls::Cull;
  Check(ls::ResolveCull(0, Cull::kBack) == Cull::kFront, "front faces are culled where the game culls back faces");
  Check(ls::ResolveCull(0, Cull::kFront) == Cull::kBack, "... and the other way round for an inside out mesh");
  Check(ls::ResolveCull(0, Cull::kNone) == Cull::kNone, "a two sided mesh stays two sided");
  Check(ls::ResolveCull(1, Cull::kBack) == Cull::kNone && ls::ResolveCull(1, Cull::kFront) == Cull::kNone, "policy 1 culls nothing");
  Check(ls::ResolveCull(2, Cull::kBack) == Cull::kBack && ls::ResolveCull(2, Cull::kFront) == Cull::kFront, "policy 2 is the game's own");
  ls::PipeKey a, b;
  a.vs = 5;
  a.layout = 3;
  b = a;
  Check(a == b && ls::PipeKeyHash()(a) == ls::PipeKeyHash()(b), "equal keys hash alike");
  b.cull = 2;
  Check(!(a == b) && ls::PipeKeyHash()(a) != ls::PipeKeyHash()(b), "keys that differ in the faces differ");
  b = a;
  b.ps = 9;
  b.ps_variant = 1;
  Check(!(a == b), "an alpha tested pipeline is another pipeline");
  std::unordered_set<ls::PipeKey, ls::PipeKeyHash> set;
  for (uint32_t i = 0; i < 200; ++i) {
    ls::PipeKey k;
    k.vs = i * 7919ull;
    k.layout = i % 13;
    k.topology = i % 4;
    set.insert(k);
  }
  Check(set.size() == 200, "200 distinct keys stay distinct in a set");
}

// A key built the way the renderer builds it (default made, the fields set one by one) in storage that holds other bytes: its tail padding is whatever
// the stack had there. The HEADLIGHTS runs of 2026-11-20 made 98 759 to 543 441 "different" pipelines of ~250 shapes because the key was compared as bytes.
ls::PipeKey* MakeKeyOver(void* storage, unsigned char fill, uint64_t vs, uint32_t layout, uint32_t topology, uint8_t cull, uint64_t ps) {
  std::memset(storage, fill, sizeof(ls::PipeKey));
  ls::PipeKey* k = new (storage) ls::PipeKey;
  k->vs = vs;
  k->layout = layout;
  k->topology = topology;
  k->front_ccw = uint8_t(layout & 1u);
  k->cull = cull;
  if (ps != 0) {
    k->ps = ps;
    k->ps_variant = 1;
  }
  return k;
}

void CheckKeyPadding() {
  alignas(ls::PipeKey) unsigned char storage[sizeof(ls::PipeKey)];
  const unsigned char fills[] = {0x00, 0xFF, 0xAA, 0x55, 0x12, 0xC3, 0x7E};
  // One shape over every fill: equal, one hash, one entry in a set.
  const ls::PipeKey first = *MakeKeyOver(storage, 0x00, 0x1234567890ABCDEFull, 7, 4, 1, 0);
  std::unordered_set<ls::PipeKey, ls::PipeKeyHash> one;
  bool all_equal = true;
  bool all_hash = true;
  for (unsigned char fill : fills) {
    const ls::PipeKey* k = MakeKeyOver(storage, fill, 0x1234567890ABCDEFull, 7, 4, 1, 0);
    all_equal = all_equal && *k == first;
    all_hash = all_hash && ls::PipeKeyHash()(*k) == ls::PipeKeyHash()(first);
    one.insert(*k);
  }
  Check(all_equal, "the same shape of draw is the same key whatever bytes the padding holds");
  Check(all_hash, "... and has the same hash");
  Check(one.size() == 1, "... so a set of them holds one entry");
  // 40 shapes made 500 times each over changing garbage: exactly 40 keys (the replay's pipeline table never grows on a repeat).
  std::unordered_set<ls::PipeKey, ls::PipeKeyHash> shapes;
  uint32_t lcg = 12345u;
  for (uint32_t round = 0; round < 500; ++round) {
    for (uint32_t shape = 0; shape < 40; ++shape) {
      lcg = lcg * 1664525u + 1013904223u;
      shapes.insert(*MakeKeyOver(storage, static_cast<unsigned char>(lcg >> 24), 0xABCD0000ull + shape * 977ull, shape % 9, shape % 3, uint8_t(shape % 3), (shape % 5 == 0) ? 0xFEED0000ull + shape : 0));
    }
  }
  Check(shapes.size() == 40, "40 shapes seen 500 times over garbage stay 40 keys (" + std::to_string(shapes.size()) + ")");
  Check(ls::kMaxPipelines >= 4 * 500 && ls::kMaxPipelines <= 20000 && ls::kNoPipeline == 0xFFFFFFFFu, "the pipeline table's safety cap sits above any real street and far below a runaway");
  // every field still counts
  ls::PipeKey a = first;
  ls::PipeKey b = first;
  b.vs_variant = 1;
  Check(!(a == b) && ls::PipeKeyHash()(a) != ls::PipeKeyHash()(b), "the vertex shader's variant counts");
  b = first;
  b.topology = 5;
  Check(!(a == b), "the topology counts");
  b = first;
  b.front_ccw = uint8_t(!first.front_ccw);
  Check(!(a == b), "the winding counts");
  b = first;
  b.layout = 8;
  Check(!(a == b), "the layout counts");
  b = first;
  b.ps_variant = 1;
  Check(!(a == b), "the pixel shader's variant counts");
}

// ---------------------------------------------------------------------------
// a car between a lamp and the road (the owner's picture of 2026-11-20: four thin stripes behind a car that should shadow its whole width)
// ---------------------------------------------------------------------------

struct P2 {
  double x, y;
};
struct Edge2 {
  P2 a, b;
  P2 n;  // outward normal (unit)
};

// Whether the open segment p -> q crosses the segment a -> b (not at the ends of p -> q).
bool Crosses(P2 p, P2 q, P2 a, P2 b) {
  const double rx = q.x - p.x, ry = q.y - p.y, sx = b.x - a.x, sy = b.y - a.y;
  const double den = rx * sy - ry * sx;
  if (std::fabs(den) < 1e-12) {
    return false;
  }
  const double t = ((a.x - p.x) * sy - (a.y - p.y) * sx) / den;
  const double u = ((a.x - p.x) * ry - (a.y - p.y) * rx) / den;
  return t > 1e-6 && t < 1.0 - 1e-6 && u >= 0.0 && u <= 1.0;
}

// The profile of a car seen from the side, x toward the lamp, y up, the road at y = 0: the underbody 0.16 m over the road (ground clearance), a bumper, a
// hood, a windscreen, a roof, a rear window and a trunk, 4.6 m long and 1.2 m high.
std::vector<Edge2> CarProfile() {
  const P2 pts[] = {{2.30, 0.16}, {2.34, 0.50}, {1.90, 0.70}, {0.90, 0.80}, {0.20, 1.20}, {-1.20, 1.20}, {-1.90, 0.95}, {-2.30, 0.92}, {-2.30, 0.16}};
  const size_t n = sizeof(pts) / sizeof(pts[0]);
  P2 centre{0.0, 0.0};
  for (const P2& p : pts) {
    centre.x += p.x / double(n);
    centre.y += p.y / double(n);
  }
  std::vector<Edge2> edges;
  for (size_t i = 0; i < n; ++i) {
    Edge2 e;
    e.a = pts[i];
    e.b = pts[(i + 1) % n];
    const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, len = std::sqrt(dx * dx + dy * dy);
    e.n = {dy / len, -dx / len};
    const double out = e.n.x * ((e.a.x + e.b.x) / 2 - centre.x) + e.n.y * ((e.a.y + e.b.y) / 2 - centre.y);
    if (out < 0.0) {
      e.n = {-e.n.x, -e.n.y};
    }
    edges.push_back(e);
  }
  return edges;
}

// The edges a pass over `cull` draws for a lamp at `lamp`, moved by `shift`: kFront (front faces culled) keeps the faces turned from the lamp, kBack the faces it
// sees, kNone both.
std::vector<Edge2> DrawnEdges(const std::vector<Edge2>& edges, P2 lamp, ls::Cull cull, P2 shift = {0.0, 0.0}) {
  std::vector<Edge2> out;
  for (Edge2 e : edges) {
    const P2 mid{(e.a.x + e.b.x) / 2 + shift.x, (e.a.y + e.b.y) / 2 + shift.y};
    const bool sees = e.n.x * (lamp.x - mid.x) + e.n.y * (lamp.y - mid.y) > 0.0;
    if ((cull == ls::Cull::kFront && sees) || (cull == ls::Cull::kBack && !sees)) {
      continue;
    }
    e.a = {e.a.x + shift.x, e.a.y + shift.y};
    e.b = {e.b.x + shift.x, e.b.y + shift.y};
    out.push_back(e);
  }
  return out;
}

bool Blocked(P2 lamp, P2 receiver, const std::vector<Edge2>& edges) {
  for (const Edge2& e : edges) {
    if (Crosses(lamp, receiver, e.a, e.b)) {
      return true;
    }
  }
  return false;
}

// The unit way the light travels, from a lamp `distance` m from the car and `height` m high toward the middle of the car (0.9 m high), in the plane of the profile.
void LightDirection(double distance, double height, double dir[3]) {
  dir[0] = -distance;
  dir[1] = 0.9 - height;
  dir[2] = 0.0;
  const double length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
  dir[0] /= length;
  dir[1] /= length;
}

void CheckCarCastsItsSilhouette() {
  using ls::Cull;
  // The faces: the world keeps the policy's, a vehicle's part is drawn on both faces under the default one, the debug policies mean what they said.
  Check(ls::ResolveCasterCull(0, Cull::kBack, false) == Cull::kFront, "a world draw keeps the faces turned from the lamp");
  Check(ls::ResolveCasterCull(0, Cull::kBack, true) == Cull::kNone, "a vehicle's part is drawn on both faces");
  Check(ls::ResolveCasterCull(0, Cull::kFront, true) == Cull::kNone, "... whichever the game culled");
  Check(ls::ResolveCasterCull(1, Cull::kBack, true) == Cull::kNone, "policy 1 is both faces for a vehicle too");
  Check(ls::ResolveCasterCull(2, Cull::kBack, true) == Cull::kBack && ls::ResolveCasterCull(2, Cull::kBack, false) == Cull::kBack,
        "policy 2 is the game's own for a vehicle as for the world");

  const std::vector<Edge2> car = CarProfile();
  // The car as one solid down to the road: the reference of what "casts its whole silhouette" means (the gap under the floor filled in).
  std::vector<Edge2> solid = car;
  solid.push_back({{2.30, 0.0}, {2.30, 0.16}, {1.0, 0.0}});
  solid.push_back({{-2.30, 0.0}, {-2.30, 0.16}, {-1.0, 0.0}});
  for (const double lamp_height : {0.45, 0.65}) {
    const double distance = 20.0;
    const P2 lamp{distance, lamp_height};
    double dir[3], shift3[3];
    LightDirection(distance, lamp_height, dir);
    ls::SkirtShift(dir, shift3);
    const P2 shift{shift3[0], shift3[1]};
    CheckNear(shift3[2], 0.0, 1e-12, "the skirt does not move sideways for a light along x");
    Check(shift.x < -0.5 && shift.y < -ls::kSkirtDrop * 0.9, "the skirt's copy is lower and farther along the light");
    const std::vector<Edge2> old_way = DrawnEdges(car, lamp, Cull::kFront);  // the faces turned from the lamp
    const std::vector<Edge2> both = DrawnEdges(car, lamp, Cull::kNone);
    const std::vector<Edge2> copy = DrawnEdges(car, lamp, Cull::kNone, shift);
    std::vector<Edge2> with_skirt = both;
    with_skirt.insert(with_skirt.end(), copy.begin(), copy.end());
    int in_umbra = 0, leaks_old = 0, leaks_both = 0, leaks_skirt = 0, dark_beyond = 0, outside = 0;
    for (double x = -2.45; x > -12.0; x -= 0.05) {
      const P2 receiver{x, 0.0};
      if (Blocked(lamp, receiver, solid)) {
        ++in_umbra;
        leaks_old += !Blocked(lamp, receiver, old_way);
        leaks_both += !Blocked(lamp, receiver, both);
        leaks_skirt += !Blocked(lamp, receiver, with_skirt);
      } else {
        ++outside;
        dark_beyond += Blocked(lamp, receiver, with_skirt);
      }
    }
    const std::string at = " (lamp " + std::to_string(lamp_height) + " m high, " + std::to_string(int(distance)) + " m away)";
    Check(in_umbra > 20, "the road behind the car has an umbra to check" + at);
    Check(leaks_old > 0 && leaks_both > 0,
          "without the skirt the light gets through under the car: " + std::to_string(leaks_old) + " and " + std::to_string(leaks_both) + " of " + std::to_string(in_umbra) +
              " road samples lit inside the car's silhouette" + at);
    Check(leaks_skirt == 0, "with the skirt the whole silhouette is shadow: " + std::to_string(leaks_skirt) + " of " + std::to_string(in_umbra) + " lit" + at);
    Check(outside == 0 && dark_beyond == 0, "(a lamp this low shadows the whole road behind the car)" + at);
  }
  // A lamp above the roof (1.6 m, 6 m away): the shadow ends where the rays clear the roof, and the skirt moves that end by nothing.
  {
    const double lamp_height = 1.6, distance = 6.0;
    const P2 lamp{distance, lamp_height};
    double dir[3], shift3[3];
    LightDirection(distance, lamp_height, dir);
    ls::SkirtShift(dir, shift3);
    const P2 shift{shift3[0], shift3[1]};
    std::vector<Edge2> with_skirt = DrawnEdges(car, lamp, Cull::kNone);
    const std::vector<Edge2> copy = DrawnEdges(car, lamp, Cull::kNone, shift);
    with_skirt.insert(with_skirt.end(), copy.begin(), copy.end());
    int in_umbra = 0, outside = 0, leaks = 0, dark_beyond = 0;
    for (double x = -2.45; x > -40.0; x -= 0.05) {
      const P2 receiver{x, 0.0};
      const bool skirt_blocked = Blocked(lamp, receiver, with_skirt);
      if (Blocked(lamp, receiver, solid)) {
        ++in_umbra;
        leaks += !skirt_blocked;
      } else {
        ++outside;
        dark_beyond += skirt_blocked;
      }
    }
    Check(in_umbra > 100 && outside > 100, "a lamp above the roof has both a shadow and a lit road behind it: " + std::to_string(in_umbra) + " and " + std::to_string(outside));
    Check(leaks == 0 && dark_beyond == 0, "the skirt changes neither end of that shadow: " + std::to_string(leaks) + " lit inside, " + std::to_string(dark_beyond) + " dark outside");
  }

  // The skirt never shadows the surfaces of its own car that the lamp lights: a point just off a face the lamp sees has no skirt edge between it and the lamp.
  for (const double lamp_height : {0.45, 0.65, 0.9}) {
    for (const double distance : {6.0, 20.0, 40.0}) {
      const P2 lamp{distance, lamp_height};
      double dir[3], shift3[3];
      LightDirection(distance, lamp_height, dir);
      ls::SkirtShift(dir, shift3);
      const std::vector<Edge2> copy = DrawnEdges(car, lamp, Cull::kNone, {shift3[0], shift3[1]});
      int lit_points = 0, shadowed = 0;
      for (const Edge2& e : car) {
        const P2 mid{(e.a.x + e.b.x) / 2, (e.a.y + e.b.y) / 2};
        if (e.n.x * (lamp.x - mid.x) + e.n.y * (lamp.y - mid.y) <= 0.0) {
          continue;  // a face the lamp does not see is not lit by it
        }
        for (int k = 1; k < 50; ++k) {
          const double t = double(k) / 50.0;
          const P2 p{e.a.x + (e.b.x - e.a.x) * t + e.n.x * 1e-3, e.a.y + (e.b.y - e.a.y) * t + e.n.y * 1e-3};
          ++lit_points;
          shadowed += Blocked(lamp, p, copy);
        }
      }
      Check(lit_points > 100 && shadowed == 0,
            "the skirt's copy casts no shadow on its own car: " + std::to_string(shadowed) + " of " + std::to_string(lit_points) + " lit points (lamp " +
                std::to_string(lamp_height) + " m high, " + std::to_string(int(distance)) + " m away)");
    }
  }
}

void CheckSkirtMatrix() {
  const GameCamera cam = MakeCamera(-20.0, 2903.0, 14.0, -810.0);
  const tp::Mat4 world0 = tp::Mul(RotationY(33.0), Translation(2890.0, 0.5, -820.0));
  const ls::Camera camera = ls::DeriveCamera(MakeBank(cam, world0).data());
  Check(camera.valid, "the camera of the skirt check");
  ls::Light lamps[2] = {Lamp(2900.0, 1.0, -815.0, 0.3, -0.05, 0.95, 1, true, 0, std::cos(ls::Rad(30)), 45.0),
                        Lamp(2901.4, 1.0, -815.0, 0.3, -0.05, 0.95, 1, true, 1, std::cos(ls::Rad(30)), 45.0)};
  ls::Group g;
  ls::GroupLights(lamps, 2, 1, &g);
  ls::ViewSpec spec;
  ls::FitView(lamps, g, &spec);
  tp::Mat4 light_clip, replay, skirt;
  Check(ls::LightClip(spec, camera.right_handed, &light_clip) && ls::ReplayMatrix(camera, light_clip, &replay), "the view's matrices");
  double shift[3];
  ls::SkirtShift(spec.dir, shift);
  Check(ls::SkirtReplayMatrix(camera, light_clip, shift, &skirt), "the skirt's replay matrix");
  CheckNear(shift[0] * spec.dir[0] + shift[1] * spec.dir[1] + shift[2] * spec.dir[2] + ls::kSkirtDrop * spec.dir[1], ls::kSkirtPush, 1e-9,
            "the shift is the push along the light and the drop");
  // A car's draw: c8-c11 x the skirt matrix takes a model point to the light's clip of the world point moved by the shift; the car's own matrix to the point itself.
  const tp::Mat4 car = tp::Mul(RotationY(120.0), Translation(2902.0, 0.0, -808.0));
  const std::vector<float> bank = MakeBank(cam, car);
  float own[16], moved[16];
  ls::ReplaceWvp(bank.data() + 8 * 4, replay, own);
  ls::ReplaceWvp(bank.data() + 8 * 4, skirt, moved);
  const double model[4][3] = {{0, 0, 0}, {0.5, 1.2, -0.3}, {-1.0, 0.2, 0.7}, {2.0, 0.0, -2.0}};
  for (const auto& mp : model) {
    double w[3];
    for (int k = 0; k < 3; ++k) {
      w[k] = mp[0] * car.m[0][k] + mp[1] * car.m[1][k] + mp[2] * car.m[2][k] + car.m[3][k];
    }
    for (int pass = 0; pass < 2; ++pass) {
      const double p[3] = {w[0] + (pass ? shift[0] : 0.0), w[1] + (pass ? shift[1] : 0.0), w[2] + (pass ? shift[2] : 0.0)};
      const float* rows = pass ? moved : own;
      double via[4], direct[4];
      for (int j = 0; j < 4; ++j) {
        via[j] = mp[0] * rows[0 * 4 + j] + mp[1] * rows[1 * 4 + j] + mp[2] * rows[2 * 4 + j] + rows[3 * 4 + j];
        direct[j] = p[0] * light_clip.m[0][j] + p[1] * light_clip.m[1][j] + p[2] * light_clip.m[2][j] + light_clip.m[3][j];
      }
      const double scale = std::max(1.0, std::fabs(direct[3]));
      for (int j = 0; j < 4; ++j) {
        CheckNear(via[j] / scale, direct[j] / scale, 5e-3,
                  std::string(pass ? "the skirt's copy" : "the car") + " lands where the light's clip puts the world point, component " + std::to_string(j));
      }
    }
  }
  // Without a camera there is no matrix.
  Check(!ls::SkirtReplayMatrix(ls::Camera{}, light_clip, shift, &skirt), "no camera, no skirt matrix");
  // The part's own ray: from the eye to its place, unit; the view's axis when it is at the eye.
  {
    const double eye[3] = {10.0, 2.0, -5.0}, axis[3] = {0.0, 0.0, 1.0}, place[3] = {10.0 + 30.0, 2.0, -5.0 + 40.0};
    double dir[3];
    ls::SkirtDirection(eye, place, axis, dir);
    CheckNear(dir[0], 0.6, 1e-12, "the part's ray: x");
    CheckNear(dir[1], 0.0, 1e-12, "the part's ray: y");
    CheckNear(dir[2], 0.8, 1e-12, "the part's ray: z");
    ls::SkirtDirection(eye, eye, axis, dir);
    CheckNear(dir[2], 1.0, 1e-12, "a part at the eye takes the view's axis");
    // The shift along a ray 40 degrees off the axis still lands the copy behind the part's own surface: its component along the ray is the push.
    double shift2[3];
    const double level[3] = {0.0, 0.0, 1.0};
    ls::SkirtShift(level, shift2);
    CheckNear(shift2[1], -ls::kSkirtDrop, 1e-12, "a level ray moves the copy straight down by the drop");
    const double off[3] = {std::sin(ls::Rad(40.0)), 0.0, std::cos(ls::Rad(40.0))};
    ls::SkirtShift(off, shift2);
    CheckNear(shift2[0] * off[0] + shift2[1] * off[1] + shift2[2] * off[2], ls::kSkirtPush, 1e-12, "along a ray 40 degrees off the axis the push is the full push");
  }
}

void CheckVehicleCensus() {
  ls::VehicleCensus census;
  census.Note(0x23FB35FC659D4A67ull, ls::Skip::kNone);
  census.Note(0x23FB35FC659D4A67ull, ls::Skip::kNone);
  census.Note(0x3B4E4762B73D5239ull, ls::Skip::kNotOpaque);
  census.Note(0x23FB35FC659D4A67ull, ls::Skip::kOutOfView);
  Check(census.draws == 4 && census.used == 2 && census.overflow == 0, "the census counts draws and shaders");
  const std::string text = ls::DescribeVehicleCensus(census);
  Check(text.find("23FB35FC659D4A67: 2 drawn, 1 outside the lamps' view;") != std::string::npos, "the log names a shader's verdicts: " + text);
  Check(text.find("3B4E4762B73D5239: 1 not opaque;") != std::string::npos, "... each shader on its own: " + text);
  for (uint64_t i = 0; i < 40; ++i) {
    census.Note(0x1000 + i, ls::Skip::kNone);
  }
  Check(census.used == ls::VehicleCensus::kRows && census.overflow == 10 && census.draws == 44, "the 33rd shader and beyond are counted, not named");
  Check(ls::DescribeVehicleCensus(census).find("(and 10 draws of other shaders)") != std::string::npos, "... and the log says so");
  census.Clear();
  Check(census.draws == 0 && census.used == 0 && ls::DescribeVehicleCensus(census).empty(), "a cleared census is empty");
}

// ---------------------------------------------------------------------------
// the levels: views, sizes, memory (owner, 2026-11-20: the levels differ by how many cars are shadowed, not only by how sharp)
// ---------------------------------------------------------------------------

void CheckLevels() {
  Check(ls::kViewsHigh == 2 && ls::kViewsUltra == 3, "high shadows two cars (the player's and the nearest other car's), ultra three");
  Check(ls::MaxViews(ls::Level::kHigh, 0) == 2 && ls::MaxViews(ls::Level::kUltra, 0) == 3 && ls::MaxViews(ls::Level::kOff, 0) == 0, "the levels' view counts");
  Check(ls::MaxViews(ls::Level::kHigh, 1) == 1 && ls::MaxViews(ls::Level::kHigh, 4) == 4 && ls::MaxViews(ls::Level::kUltra, 7) == 4 &&
            ls::MaxViews(ls::Level::kUltra, -3) == 3,
        "the debug override takes 1 to 4 views, a negative one is the level's");
  // Every level draws views the published frame and the headlight variant's block can hold (a view for each lit lamp, a lamp for each car: four).
  larecomp::gpu::LocalShadowFrame frame;
  Check(ls::kMaxViews == frame.views.size() && ls::kViewsUltra <= ls::kMaxViews && ls::kViewsHigh < ls::kViewsUltra, "the views a level draws fit the published frame");

  // The atlas of every level and every number of views, at both tile sizes: where each tile is, inside the atlas, none on another, and the memory.
  struct Case {
    uint32_t tile, views, width, height;
  };
  const Case cases[] = {{512, 1, 512, 512},   {512, 2, 1024, 512},   {512, 3, 1536, 512},   {512, 4, 1024, 1024},
                        {1024, 1, 1024, 1024}, {1024, 2, 2048, 1024}, {1024, 3, 3072, 1024}, {1024, 4, 2048, 2048}};
  for (const Case& c : cases) {
    const std::string at = std::to_string(c.views) + " view(s) of " + std::to_string(c.tile);
    const ls::AtlasLayout atlas = ls::MakeAtlas(c.tile, c.views);
    Check(atlas.valid() && atlas.views == c.views && atlas.width() == c.width && atlas.height() == c.height, at + ": the atlas is " + std::to_string(c.width) + "x" + std::to_string(c.height));
    std::unordered_set<uint64_t> seen;
    for (uint32_t k = 0; k < c.views; ++k) {
      const ls::TileRect r = ls::TileAt(atlas, k);
      Check(r.size == c.tile && r.x % c.tile == 0 && r.y % c.tile == 0 && r.x + r.size <= atlas.width() && r.y + r.size <= atlas.height(), at + ": tile " + std::to_string(k) + " is inside the atlas, on the tile grid");
      Check(seen.insert((uint64_t(r.x) << 32) | r.y).second, at + ": tile " + std::to_string(k) + " is a tile of its own");
      // The published rectangle (what the shader compares a point with) is the tile, in uv, and no other tile's.
      float uv[4], rect[4];
      ls::UvRect(atlas, k, uv);
      ls::PublishedRect(uv, rect);
      CheckNear(rect[0] * atlas.width(), r.x, 1e-2, at + ": rect u0 of tile " + std::to_string(k));
      CheckNear(rect[1] * atlas.height(), r.y, 1e-2, at + ": rect v0 of tile " + std::to_string(k));
      CheckNear(rect[2] * atlas.width(), r.x + r.size, 1e-2, at + ": rect u1 of tile " + std::to_string(k));
      CheckNear(rect[3] * atlas.height(), r.y + r.size, 1e-2, at + ": rect v1 of tile " + std::to_string(k));
    }
    // Unused texels: none for up to three views in a row; a 2x2 atlas of four is full too.
    Check(uint64_t(atlas.width()) * atlas.height() == uint64_t(c.tile) * c.tile * c.views, at + ": no tile is left over");
    Check(ls::AtlasBytes(atlas) == uint64_t(c.width) * c.height * 8, at + ": 8 bytes a texel (D32 float + S8)");
  }
  // Memory: high 4 MiB (it was 2), ultra 24 MiB (it was 16): the figures in the setting's help.
  const ls::AtlasLayout high = ls::AtlasFor(ls::Level::kHigh, 0);
  const ls::AtlasLayout ultra = ls::AtlasFor(ls::Level::kUltra, 0);
  Check(high.width() == 1024 && high.height() == 512 && high.cols == 2 && high.rows == 1, "high: 1024 x 512, two tiles of 512 side by side");
  Check(ultra.width() == 3072 && ultra.height() == 1024 && ultra.cols == 3 && ultra.rows == 1, "ultra: 3072 x 1024, three tiles of 1024 in a row");
  CheckNear(ls::Mib(ls::AtlasBytes(high)), 4.0, 1e-9, "high holds 4 MiB");
  CheckNear(ls::Mib(ls::AtlasBytes(ultra)), 24.0, 1e-9, "ultra holds 24 MiB (a 2 x 2 layout of three would hold 32)");
  Check(ls::AtlasBytes(ls::AtlasFor(ls::Level::kOff, 0)) == 0 && !ls::AtlasFor(ls::Level::kOff, 0).valid(), "off holds nothing");
  Check(ls::AtlasFor(ls::Level::kUltra, 4).width() == 2048 && ls::AtlasFor(ls::Level::kUltra, 4).height() == 2048, "the debug override of four views is a 2 x 2 atlas");
  Check(ls::AtlasFor(ls::Level::kHigh, 0).width() <= 3072 && ls::AtlasFor(ls::Level::kUltra, 0).width() <= 4096, "a texture a Vulkan device must support (4096)");
  // The size follows the level, not the number of cars lit: the atlas is the same for one car or three.
  Check(ls::AtlasFor(ls::Level::kUltra, 0).views == 3 && ls::AtlasFor(ls::Level::kHigh, 0).views == 2, "the atlas has a tile for every view of its level");
  // What the log says.
  const std::string text = ls::Describe(ls::Resolve(gc::GpuClass::kHigh, "auto"), gc::GpuClass::kHigh);
  Check(text.find("ultra") != std::string::npos && text.find("up to 3 views of 1024x1024") != std::string::npos, "Describe says three views of 1024: " + text);
  const std::string mid = ls::Describe(ls::Resolve(gc::GpuClass::kMid, "auto"), gc::GpuClass::kMid);
  Check(mid.find("up to 2 views of 512x512") != std::string::npos, "Describe says two views of 512: " + mid);
}

// A point through a published view of a 3-in-a-row atlas lands in its own tile's rectangle and in no other's: what the lit shaders' rectangle test does.
void CheckThreeViewsLand() {
  for (uint32_t tile : {512u, 1024u}) {
    const ls::AtlasLayout atlas = ls::MakeAtlas(tile, 3);
    ls::Light lamps[3] = {Lamp(0.0, 0.7, 0.0, 0.0, 0.0, 1.0, 1, true, 0), Lamp(30.0, 0.7, 40.0, 0.0, 0.0, -1.0, 2, false, 1), Lamp(-20.0, 0.7, 25.0, 1.0, 0.0, 0.0, 3, false, 2)};
    for (uint32_t k = 0; k < 3; ++k) {
      ls::Group g;
      ls::GroupLights(&lamps[k], 1, 1, &g);
      ls::ViewSpec spec;
      tp::Mat4 clip;
      Check(ls::FitView(&lamps[k], g, &spec) && ls::LightClip(spec, true, &clip), "a view for the lamp");
      float uv[4], rect[4], m[16];
      ls::UvRect(atlas, k, uv);
      ls::PublishedRect(uv, rect);
      ls::WorldToShadow(clip, uv, 1.0, 1.0, m);
      for (double dist : {8.0, 20.0, 35.0}) {
        const double p[3] = {spec.eye[0] + spec.dir[0] * (spec.lamps_m + dist), spec.eye[1] + spec.dir[1] * (spec.lamps_m + dist),
                             spec.eye[2] + spec.dir[2] * (spec.lamps_m + dist)};
        double u, v, d;
        const std::string at = "tile " + std::to_string(tile) + " view " + std::to_string(k) + " at " + std::to_string(int(dist)) + " m";
        Check(ls::MapPoint(m, p, &u, &v, &d), at + ": in front of the view");
        for (uint32_t other = 0; other < 3; ++other) {
          float ouv[4], orect[4];
          ls::UvRect(atlas, other, ouv);
          ls::PublishedRect(ouv, orect);
          const bool inside = u >= orect[0] && u <= orect[2] && v >= orect[1] && v <= orect[3];
          Check(inside == (other == k), at + ": the axis point is in tile " + std::to_string(other) + "'s rectangle only when it is its own");
        }
        CheckNear(u * atlas.width(), ls::TileAt(atlas, k).x + tile * 0.5, 0.1, at + ": the axis is the middle of the tile (x)");
        CheckNear(v * atlas.height(), tile * 0.5, 0.1, at + ": the axis is the middle of the tile (y)");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// which cars get a view
// ---------------------------------------------------------------------------

// A car with one lamp at (x, 0.7, z).
ls::Light CarLamp(uint32_t key, double x, double z, bool player = false, uint32_t index = 0) { return Lamp(x, 0.7, z, 0.0, 0.0, 1.0, key, player, index); }

// Picks with a fresh set of lights each time (the index is the light's place in the set): the car keys in the picked order.
std::vector<uint32_t> Keys(const ls::Group* groups, uint32_t n) {
  std::vector<uint32_t> keys;
  for (uint32_t i = 0; i < n; ++i) {
    keys.push_back(groups[i].car_key);
  }
  return keys;
}

bool Same(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) { return a == std::vector<uint32_t>(b); }

void CheckPicking() {
  const ls::PickContext none;
  // The player's car first and always; the others nearest first. Four lit cars: the player's, and others at 30, 12 and 60 m from its lamps.
  ls::Light cars[4] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 30, false, 1), CarLamp(3, 12, 0, false, 2), CarLamp(4, 0, -60, false, 3)};
  ls::Group g[4];
  {
    ls::ViewPicker p;
    uint32_t n = p.Pick(cars, 4, ls::MaxViews(ls::Level::kHigh, 0), none, g);
    Check(n == 2 && Same(Keys(g, n), {1, 3}), "high: the player's car and the nearest other car (12 m)");
    ls::ViewPicker q;
    n = q.Pick(cars, 4, ls::MaxViews(ls::Level::kUltra, 0), none, g);
    Check(n == 3 && Same(Keys(g, n), {1, 3, 2}), "ultra: the player's car and the two nearest other cars (12 m, 30 m); the 60 m car has none");
    ls::ViewPicker r;
    n = r.Pick(cars, 4, 4, none, g);
    Check(n == 4 && Same(Keys(g, n), {1, 3, 2, 4}), "four views: every car, nearest first");
    ls::ViewPicker s;
    n = s.Pick(cars, 4, 1, none, g);
    Check(n == 1 && g[0].is_player, "one view: the player's");
    Check(s.Pick(cars, 4, 0, none, g) == 0 && s.held_count() == 0, "no view: none, and nobody holds one");
    Check(s.Pick(cars, 0, 3, none, g) == 0, "no light: no view");
  }
  // The player's car is first even when it is the farthest from everything (the player listed last in the set, with a car that is near the camera).
  {
    ls::Light order[3] = {CarLamp(7, 0, 5, false, 0), CarLamp(8, 0, 9, false, 1), CarLamp(1, 0, 200, true, 2)};
    ls::PickContext camera;
    camera.have_reference = true;  // the camera is next to the other cars
    camera.reference[2] = 6.0;
    ls::ViewPicker p;
    const uint32_t n = p.Pick(order, 3, 2, camera, g);
    Check(n == 2 && g[0].car_key == 1 && g[0].is_player && g[1].car_key == 7, "the player's car is first whatever its place, then the nearest to the camera");
  }
  // No lamp of the player's: the best other cars, from the first light's place when there is no camera.
  {
    ls::Light others[3] = {CarLamp(5, 50, 0, false, 0), CarLamp(6, 0, 0, false, 1), CarLamp(9, 20, 0, false, 2)};
    ls::ViewPicker p;
    const uint32_t n = p.Pick(others, 3, 3, none, g);
    Check(n == 3 && Same(Keys(g, n), {5, 9, 6}), "no player's lamp: the first light's car is the reference (itself, then 30 m, then 50 m)");
  }
  // The camera, not the player, is the reference when it is known: a car beside the camera but far from the player's car wins the second view.
  {
    ls::Light mix[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 20, 0, false, 1), CarLamp(3, 0, -40, false, 2)};
    ls::PickContext camera;
    camera.have_reference = true;
    camera.reference[2] = -35.0;  // 5 m from car 3, 35 m from car 2
    ls::ViewPicker p;
    const uint32_t n = p.Pick(mix, 3, 2, camera, g);
    Check(n == 2 && g[1].car_key == 3, "the importance is measured from the camera when it is known");
  }
  // The screen: a near car whose lamps are off the screen is worth 0.35 times as much as it is. A camera at the origin looking along +z, 90 degrees.
  {
    ls::ViewSpec view;
    view.eye[0] = view.eye[1] = view.eye[2] = 0.0;
    view.dir[0] = 0.0;
    view.dir[1] = 0.0;
    view.dir[2] = 1.0;
    view.fov = ls::Rad(90.0);
    view.near_m = 0.5;
    view.far_m = 500.0;
    tp::Mat4 vp;
    Check(ls::LightClip(view, false, &vp), "a camera for the screen test");
    const double on[3] = {0.0, 0.0, 10.0}, edge_in[3] = {12.0, 0.0, 10.0}, edge_out[3] = {14.0, 0.0, 10.0}, behind[3] = {0.0, 0.0, -5.0}, above[3] = {0.0, 14.0, 10.0};
    Check(ls::OnScreen(vp, on, 1.3) && ls::OnScreen(vp, edge_in, 1.3) && !ls::OnScreen(vp, edge_out, 1.3) && !ls::OnScreen(vp, behind, 1.3) && !ls::OnScreen(vp, above, 1.3),
          "a point is on the screen up to a third beyond the edge, never behind the camera");
    Check(!ls::OnScreen(vp, edge_in, 1.0) && ls::OnScreen(vp, on, 1.0), "the margin is a share of the clip box");
    ls::Light two[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, -15, false, 1), CarLamp(3, 0, 40, false, 2)};  // car 2 is behind the camera, car 3 ahead
    ls::PickContext screen;
    screen.have_reference = true;
    screen.have_screen = true;
    screen.view_projection = vp;
    ls::ViewPicker p;
    uint32_t n = p.Pick(two, 3, 2, screen, g);
    Check(n == 2 && g[1].car_key == 3, "a car 15 m behind the camera loses the view to one 40 m ahead of it (0.35 x 0.74 = 0.26 against 0.28)");
    ls::ViewPicker q;
    n = q.Pick(two, 3, 2, none, g);  // no screen known: distance alone, car 2 first
    Check(n == 2 && g[1].car_key == 2, "with no screen known the distance alone ranks them");
  }
  // The importance's numbers: a half at 25 m, 0.35 off the screen, 1.3 for the holder.
  CheckNear(ls::ViewImportance(0.0, true, false), 1.0, 1e-12, "importance 1 at the camera");
  CheckNear(ls::ViewImportance(25.0, true, false), 0.5, 1e-12, "... a half at 25 m");
  CheckNear(ls::ViewImportance(25.0, false, false), 0.5 * 0.35, 1e-12, "... 0.35 of that off the screen");
  CheckNear(ls::ViewImportance(25.0, true, true), 0.5 * 1.3, 1e-12, "... 1.3 times it for the car that holds a view");
  Check(ls::ViewImportance(10.0, true, false) > ls::ViewImportance(20.0, true, false) && ls::ViewImportance(10.0, false, false) < ls::ViewImportance(10.0, true, false),
        "nearer and on the screen is more important");
}

// The views of cars at the edge of the frame (HL-EDGE-FADE, owner 2026-11-20: a traffic car's headlights fade near the frame's edge). A car is ranked for a view by how much of
// its BEAM is in the picture (light_influence.h), not by whether its LAMP is on the screen: the first version ranked a car off the screen at 0.35 and so gave the view to a car
// whose beam points out of the picture rather than to one at the edge whose beam crosses it.
void CheckPickingReach() {
  namespace li = mcla::gpu::light_influence;
  // The importance's numbers: the first version's 0.35 for a beam that lights nothing, 1 from a reach of 0.45, smooth between; a holder keeps its 1.3.
  CheckNear(ls::ViewImportanceReach(25.0, 0.0, false), 0.5 * 0.35, 1e-12, "a beam that lights nothing in the picture: 0.35 of the car's importance (a half at 25 m)");
  CheckNear(ls::ViewImportanceReach(25.0, 0.45, false), 0.5, 1e-12, "a reach of 0.45: in full");
  CheckNear(ls::ViewImportanceReach(25.0, 1.0, false), 0.5, 1e-12, "a reach of 1: in full");
  CheckNear(ls::ViewImportanceReach(25.0, 1.0, true), 0.5 * 1.3, 1e-12, "the holder of a view keeps its 1.3");
  {
    bool monotone = true;
    double previous = 0.0;
    for (double r = 0.0; r <= 1.0; r += 0.01) {
      const double v = ls::ViewImportanceReach(30.0, r, false);
      monotone &= v >= previous - 1e-12;
      previous = v;
    }
    Check(monotone, "the importance never falls as the beam's reach grows");
  }

  // A camera at the origin looking along +z, 90 degrees (the frustum's half width is the depth).
  ls::ViewSpec view;
  view.eye[0] = view.eye[1] = view.eye[2] = 0.0;
  view.dir[0] = 0.0;
  view.dir[1] = 0.0;
  view.dir[2] = 1.0;
  view.fov = ls::Rad(90.0);
  view.near_m = 0.5;
  view.far_m = 500.0;
  tp::Mat4 vp;
  Check(ls::LightClip(view, false, &vp), "a camera for the reach test");
  ls::PickContext ctx;
  ctx.have_reference = true;
  ctx.have_screen = true;
  ctx.view_projection = vp;
  Check(li::MakeViewFrustum(vp.m, view.eye, &ctx.frustum) && ctx.frustum.valid, "the camera's frustum is made from the same matrix");
  ls::PickContext legacy = ctx;
  legacy.frustum = li::ViewFrustum{};  // the first version's: the lamp's place on the screen

  // GroupReach: the strongest of a car's lamps. A car with two lamps, one in the view and one far outside pointing out of it: the car's reach is the first one's.
  {
    ls::Light two[2] = {Lamp(0.0, 0.7, 20.0, 0.0, 0.0, 1.0, 5, false, 0), Lamp(-60.0, 0.7, 20.0, -1.0, 0.0, 0.0, 5, false, 1)};
    ls::Group g[4];
    ls::ViewPicker p;
    p.Pick(two, 2, 2, legacy, g);
    const double both = ls::GroupReach(two, g[0], ctx.frustum);
    ls::Light only_out[1] = {Lamp(-60.0, 0.7, 20.0, -1.0, 0.0, 0.0, 5, false, 0)};
    ls::Group h[4];
    ls::ViewPicker q;
    q.Pick(only_out, 1, 2, legacy, h);
    const double out_alone = ls::GroupReach(only_out, h[0], ctx.frustum);
    Check(both > 0.9 && out_alone < 0.01, "a car's reach is its best lamp's: " + std::to_string(both) + " with a lamp in the view, " + std::to_string(out_alone) + " with the other alone");
  }

  // Two cars for the one other view that high has: E off the RIGHT edge (5 m out at 20 m) with its beam across the picture, F off the LEFT edge with its beam pointing out of it.
  // Both lamps are off the screen (beyond the margin of 1.3 too): the first version ranks them alike and the lower key, F, wins; the view gives E the view.
  {
    ls::Light cars[3] = {Lamp(0.0, 0.7, -3.0, 0.0, 0.0, 1.0, 1, true, 0),     // the player's car behind the camera
                         Lamp(-40.0, 0.7, 30.0, -1.0, 0.0, 0.0, 2, false, 1),  // F: the lower key, off the left edge, pointing out of the picture
                         Lamp(40.0, 0.7, 30.0, -1.0, 0.0, 0.0, 3, false, 2)};  // E: off the right edge, pointing across the picture
    ls::Group g[4];
    ls::ViewPicker first;
    uint32_t n = first.Pick(cars, 3, 2, legacy, g);
    Check(n == 2 && g[1].car_key == 2, "the first version: two cars off the screen tie and the lower key (the car whose beam points out of the picture) has the view");
    ls::ViewPicker second;
    n = second.Pick(cars, 3, 2, ctx, g);
    Check(n == 2 && g[1].car_key == 3, "with the view: the car at the edge whose beam crosses the picture has it");
    // Hysteresis holds as before with the view: the holder keeps its view against a challenger that is somewhat better.
    ls::Light now[3] = {cars[0], Lamp(-40.0, 0.7, 30.0, -1.0, 0.0, 0.0, 2, false, 1), Lamp(36.0, 0.7, 30.0, -1.0, 0.0, 0.0, 3, false, 2)};
    ls::ViewPicker third;
    third.Pick(now, 3, 2, ctx, g);
    Check(g[1].car_key == 3, "(the car at the edge holds the view)");
    // F now turns to point along the road into the picture: its reach rises, but E holds the view with its 1.3 against a challenger that is not clearly better.
    ls::Light turned[3] = {cars[0], Lamp(-30.0, 0.7, 30.0, 0.0, 0.0, 1.0, 2, false, 1), Lamp(36.0, 0.7, 30.0, -1.0, 0.0, 0.0, 3, false, 2)};
    const uint32_t m = third.Pick(turned, 3, 2, ctx, g);
    Check(m == 2 && g[1].car_key == 3, "the holder keeps the view against a challenger whose beam now crosses the picture as well (1.3)");
  }

  // The car at the edge, seen as the first version saw it: a lamp 10 m beyond the left edge at 30 m depth, beam along the road: lit by the view (its beam is in the picture) and
  // ranked like a car in the middle at the same distance, where the first version gave it 0.35.
  {
    ls::Light edge[2] = {Lamp(0.0, 0.7, -3.0, 0.0, 0.0, 1.0, 1, true, 0), Lamp(-40.0, 0.7, 30.0, 0.0, 0.0, 1.0, 9, false, 1)};
    ls::Group g[4];
    ls::ViewPicker p;
    p.Pick(edge, 2, 2, ctx, g);
    const double reach = ls::GroupReach(edge, g[1], ctx.frustum);
    const double distance = std::sqrt(40.0 * 40.0 + 33.0 * 33.0);
    const double importance = ls::ViewImportanceReach(distance, reach, false);
    const double middle = ls::ViewImportance(distance, true, false);
    Check(reach > 0.45 && std::fabs(importance - middle) < 1e-9, "a car at the edge whose beam goes into the picture is as important as one in the middle at its distance (reach " + std::to_string(reach) + ")");
  }

  // No frustum: nothing is different (the first version's rule is read).
  {
    ls::Light cars[3] = {Lamp(0.0, 0.7, -3.0, 0.0, 0.0, 1.0, 1, true, 0), Lamp(0.0, 0.7, -15.0, 0.0, 0.0, 1.0, 2, false, 1), Lamp(0.0, 0.7, 40.0, 0.0, 0.0, 1.0, 3, false, 2)};
    ls::Group g[4];
    ls::ViewPicker p;
    const uint32_t n = p.Pick(cars, 3, 2, legacy, g);
    Check(n == 2, "the first version's pick still works with no frustum");
  }
}

void CheckPickingHysteresis() {
  const ls::PickContext none;
  ls::Group g[4];
  // Two other cars at about the same distance from the player's car that trade places with every frame (a pass, two cars side by side): with only ONE view for
  // them (high with the player's car and one other) the picking without a memory flips every other frame, the picker keeps the holder.
  int flips_plain = 0, flips_sticky = 0;
  uint32_t last_plain = 0, last_sticky = 0;
  ls::ViewPicker sticky;
  for (int frame = 0; frame < 200; ++frame) {
    const double wobble = (frame % 2 == 0) ? 0.5 : -0.5;                  // metres: the cars' distances cross every frame
    ls::Light cars[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 20 + wobble, false, 1), CarLamp(3, 0, 20 - wobble, false, 2)};
    ls::Group plain[4];
    const uint32_t a = ls::GroupLights(cars, 3, 2, plain);
    const uint32_t b = sticky.Pick(cars, 3, 2, none, g);
    Check(a == 2 && b == 2, "two views");
    if (frame > 0) {
      flips_plain += plain[1].car_key != last_plain ? 1 : 0;
      flips_sticky += g[1].car_key != last_sticky ? 1 : 0;
    }
    last_plain = plain[1].car_key;
    last_sticky = g[1].car_key;
  }
  Check(flips_plain >= 190, "without a memory the view goes back and forth with every wobble (" + std::to_string(flips_plain) + " swaps in 200 frames)");
  Check(flips_sticky == 0, "with the holder kept it never moves (" + std::to_string(flips_sticky) + " swaps)");
  // A challenger takes the view when it is clearly more important: the holder 25 m away (0.5), the challenger nearer. 1.3 x 0.5 = 0.65 = 1 / (1 + (d/25)^2) at d = 18.3 m.
  {
    ls::ViewPicker p;
    ls::Light start[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 25, false, 1), CarLamp(3, 0, 40, false, 2)};
    Check(p.Pick(start, 3, 2, none, g) == 2 && g[1].car_key == 2 && p.Holds(2) && p.Holds(1) && !p.Holds(3), "the nearer car gets the view first");
    ls::Light close_a[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 25, false, 1), CarLamp(3, 0, 20, false, 2)};  // the challenger at 20 m: 0.61 < 0.65
    Check(p.Pick(close_a, 3, 2, none, g) == 2 && g[1].car_key == 2, "a challenger at 20 m does not take a holder's view at 25 m");
    ls::Light close_b[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 25, false, 1), CarLamp(3, 0, 15, false, 2)};  // 15 m: 0.735 > 0.65
    Check(p.Pick(close_b, 3, 2, none, g) == 2 && g[1].car_key == 3, "a challenger at 15 m takes it");
    Check(p.Holds(3) && !p.Holds(2), "... and now it holds it: the old holder must beat it by the same margin");
    Check(p.Pick(close_a, 3, 2, none, g) == 2 && g[1].car_key == 3, "so the 20 m car (0.61) against the new holder at 15 m keeps it");
  }
  // A holder that leaves the set loses the view; one that comes back later (after Forget, or after another holder) starts with no bonus.
  {
    ls::ViewPicker p;
    ls::Light a[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 30, false, 1), CarLamp(3, 0, 32, false, 2)};
    p.Pick(a, 3, 2, none, g);
    Check(g[1].car_key == 2, "the nearer of two gets the view");
    ls::Light b[2] = {CarLamp(1, 0, 0, true, 0), CarLamp(3, 0, 32, false, 1)};
    Check(p.Pick(b, 2, 2, none, g) == 2 && g[1].car_key == 3 && !p.Holds(2), "the holder left: the other car takes the view");
    p.Pick(a, 3, 2, none, g);
    Check(g[1].car_key == 3, "car 2 is back and 2 m nearer: the new holder keeps it (1.3 x)");
    p.Forget();
    Check(p.held_count() == 0 && !p.Holds(3), "Forget ends every hold");
    p.Pick(a, 3, 2, none, g);
    Check(g[1].car_key == 2, "after Forget the nearer car wins again");
    // A gap of no lights keeps the holders (a night's frame or two without a set); a view count of 0 ends them.
    p.Pick(nullptr, 0, 2, none, g);
    Check(p.Holds(2), "no light for a frame: who holds a view is kept");
    ls::Light c[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 34, false, 1), CarLamp(3, 0, 32, false, 2)};
    p.Pick(c, 3, 2, none, g);
    Check(g[1].car_key == 2, "... so the holder is still ahead of a challenger 2 m nearer");
    p.Pick(c, 3, 0, none, g);
    Check(p.held_count() == 0, "a level with no views ends them");
  }
  // The holders of a level with more views: three views, three cars hold them; a fourth that comes near enough takes the farthest holder's, never the player's.
  {
    ls::ViewPicker p;
    ls::Light cars[4] = {CarLamp(1, 0, 0, true, 0), CarLamp(2, 0, 20, false, 1), CarLamp(3, 0, 30, false, 2), CarLamp(4, 0, 90, false, 3)};
    Check(p.Pick(cars, 4, 3, none, g) == 3 && Same(Keys(g, 3), {1, 2, 3}), "ultra: the player's, 20 m and 30 m");
    cars[3] = CarLamp(4, 0, 10, false, 3);
    Check(p.Pick(cars, 4, 3, none, g) == 3 && Same(Keys(g, 3), {1, 4, 2}) && !p.Holds(3), "a car at 10 m takes the farthest holder's view (the car at 30 m), never the player's");
    cars[3] = CarLamp(4, 0, 25, false, 3);
    Check(p.Pick(cars, 4, 3, none, g) == 3 && Same(Keys(g, 3), {1, 2, 4}) && p.Holds(4) && !p.Holds(3),
          "(the car that took the view, now at 25 m, keeps it against the car at 30 m: 0.65 against 0.41; the car at 20 m is ahead of it)");
  }
  // The view order does not matter to the maps but is stable: the same lights and holders always give the same order (ties by car key).
  {
    ls::Light twins[3] = {CarLamp(1, 0, 0, true, 0), CarLamp(9, 10, 0, false, 1), CarLamp(5, -10, 0, false, 2)};
    ls::ViewPicker p, q;
    p.Pick(twins, 3, 3, none, g);
    const std::vector<uint32_t> first = Keys(g, 3);
    q.Pick(twins, 3, 3, none, g);
    Check(first == Keys(g, 3) && Same(first, {1, 5, 9}), "two cars at the same distance: the lower car key first");
  }
}

}  // namespace

int main() {
  CheckTypes();
  CheckSetting();
  CheckAtlas();
  CheckLevels();
  CheckThreeViewsLand();
  CheckLights();
  CheckGroups();
  CheckPicking();
  CheckPickingHysteresis();
  CheckPickingReach();
  CheckFit();
  CheckWorldToShadow();
  CheckCamera();
  CheckReplay();
  CheckClassification();
  CheckSelection();
  CheckViewCull();
  CheckCullAndKeys();
  CheckKeyPadding();
  CheckCarCastsItsSilhouette();
  CheckSkirtMatrix();
  CheckVehicleCensus();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
