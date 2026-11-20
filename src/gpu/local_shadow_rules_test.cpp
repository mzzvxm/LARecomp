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
  Check(ls::MaxViews(ls::Level::kHigh, 0) == 1 && ls::MaxViews(ls::Level::kUltra, 0) == 2, "high shadows the player's lamps, ultra one car more");
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

}  // namespace

int main() {
  CheckTypes();
  CheckSetting();
  CheckAtlas();
  CheckLights();
  CheckGroups();
  CheckFit();
  CheckWorldToShadow();
  CheckCamera();
  CheckReplay();
  CheckClassification();
  CheckSelection();
  CheckViewCull();
  CheckCullAndKeys();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
