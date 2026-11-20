#pragma once
// ===========================================================================
// MCLA GPU renderer -- the headlight shadow maps: the rules that need no renderer
// ===========================================================================
// The renderer's half is local_shadow.h (perf/LOCAL-SHADOWS-SURVEY.md, option A). This header holds what needs no Plume, no ReXGlue and
// no game, as pure functions on plain numbers, so that local_shadow_rules_test.cpp builds on its own:
//
//   - the setting: mcla_gpu_headlight_shadows' words, the size of a view per level and how many views a level draws (Resolve, MaxViews);
//   - the lights: which of the rig's lights share a view (GroupLights: one view covers both lamps of a car), the view that covers a group
//     (FitView: eye, direction, field of view, near and far planes) and its matrices (LightClip, WorldToShadow);
//   - the atlas: where view k sits in it, in texels and in uv (MakeAtlas, TileRect, UvRect);
//   - the replay's matrix: a main-view draw's gWorldViewProj (VS c8-c11) turned into model -> light clip (ReplaceWvp), and the main camera's
//     view-projection found from the draws' own constants (DeriveCamera);
//   - which draws are casters (ClassifyDraw: opaque, alpha tested; skinned, impostor, billboard, a vehicle's own parts...) and which of them
//     a view keeps (KeepForView, SelectForView);
//   - the pipelines' key (PipeKey) and the face culling a caster is drawn with (ResolveCull).
//
// THE REPLAY'S ONE TRICK (survey 4.2 point 1). Every vertex shader of the pack that places a vertex multiplies it by gWorldViewProj, VS
// c8-c11. In a main-view draw that is gWorld x VP_main (VP_main = the camera's view-projection, the game's own clip space), so
//   c8-c11' = c8-c11 x VP_main^-1 x VP_light = gWorld x VP_light
// puts the same vertex, through the same vertex shader (position decompression, skinning, wind, instancing: all the game's), where the light's
// view sees it. VP_main^-1 x VP_light is one 4x4 for the whole frame (C below); a draw costs one 4x4 product. A draw whose c8-c11 times
// VP_main^-1 is not a model-to-world matrix (its last column is not 0 0 0 1: the vertex shader is not of this kind) is left out
// (Skip::kNotWvp), which is also what guards VP_main against a draw that is not the main view's.
//
// THE LIGHT'S CLIP SPACE is Direct3D's (x and y in [-w, w], z in [0, w], z = 0 at the near plane), in the same handedness as the game's own
// camera (IsRightHanded: from the view inverse the draws carry in VS c12-c15), so that the winding of a triangle seen from the light is
// the winding the game's state (front face, cull) expects. The shared constants of the replay carry the NDC transform of the pass (the
// signs of the main view's pos_scale x and y, scale 1 and offset 0 otherwise: the main view's viewport maps are not the atlas tile's).
//
// DEPTH. The map stores z/w of that projection: 0 at the near plane (kNearMeters in front of the lamps), 1 at the far plane (the light's
// range), 1 also where nothing was drawn (the clear value). z/w = A (1 - n / d) for a point d metres along the view axis (A = f / (f - n)):
// the reader compares  receiver_depth - depth_bias  against the stored value (stored < that = shadowed). Treat a stored 1 as "nothing
// here" before comparing, and a receiver depth below 0 or above 1 as lit (outside the map's planes).
//
// Matrices are row vectors (v' = v M) as in temporal_math.h, except WorldToShadow, which is the column-vector, row-major form the
// published LocalShadowView carries (local_shadow_types.h). This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "temporal_math.h"

namespace mcla::gpu::local_shadow {

namespace tp = ::mcla::gpu::temporal;

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

// mcla_gpu_headlight_shadows: auto, off, high or ultra. No level is called low (owner, 2026-11-14): two sizes are high and ultra.
enum class Level : uint8_t { kOff, kHigh, kUltra };

inline constexpr uint32_t kTileHigh = 512;    // texels a view
inline constexpr uint32_t kTileUltra = 1024;
inline constexpr uint32_t kMaxViews = 4;      // the published frame holds four views (local_shadow_types.h)

constexpr uint32_t TileSize(Level level) { return level == Level::kUltra ? kTileUltra : level == Level::kHigh ? kTileHigh : 0u; }

constexpr std::string_view LevelName(Level level) { return level == Level::kUltra ? "ultra" : level == Level::kHigh ? "high" : "off"; }

// How many cars' lamps get a view of their own: high shadows the player's lamps (one view for both), ultra the nearest other car's too.
// `override_views` (mcla_gpu_headlight_shadow_views, debug) 1 to 4 replaces it; 0 takes the level's.
constexpr uint32_t MaxViews(Level level, int override_views) {
  if (level == Level::kOff) {
    return 0;
  }
  if (override_views >= 1) {
    return uint32_t(override_views > int(kMaxViews) ? int(kMaxViews) : override_views);
  }
  return level == Level::kUltra ? 2u : 1u;
}

struct Choice {
  Level level = Level::kOff;
  bool automatic = false;  // `auto`: the class decided (also a text the setting does not know)
  bool unknown = false;    // the text is none of the setting's words (it then runs as `auto`)
  uint32_t tile() const { return TileSize(level); }
};

inline std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

inline Level ParseLevel(std::string_view text, bool* known) {
  *known = true;
  if (text == "off") return Level::kOff;
  if (text == "high") return Level::kHigh;
  if (text == "ultra") return Level::kUltra;
  *known = false;
  return Level::kOff;
}

// `auto` is the class table's cell (gpu_class.h Effect::kHeadlightShadows: off on shared memory, high on Mid, ultra on High); a word that is
// no value of the setting (the low of an older larecomp.toml) counts as `auto`.
inline Choice Resolve(gpu_class::GpuClass cls, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  bool known = false;
  const Level own = ParseLevel(text, &known);
  if (known) {
    choice.level = own;
    return choice;
  }
  choice.automatic = true;
  choice.unknown = text != "auto";
  bool cell_known = false;
  choice.level = ParseLevel(gpu_class::Cell(gpu_class::Effect::kHeadlightShadows, cls), &cell_known);
  return choice;
}

// "headlight shadows high, 512x512 a view (auto on the mid class)".
inline std::string Describe(const Choice& choice, gpu_class::GpuClass cls) {
  std::string text = "headlight shadows ";
  text += LevelName(choice.level);
  if (choice.level != Level::kOff) {
    text += ", " + std::to_string(choice.tile()) + "x" + std::to_string(choice.tile()) + " a view";
  }
  text += " (";
  if (choice.unknown) {
    text += "not a value of the setting: taken as auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else if (choice.automatic) {
    text += "auto on the ";
    text += gpu_class::ClassName(cls);
    text += " class";
  } else {
    text += "set";
  }
  text += ')';
  return text;
}

// ---------------------------------------------------------------------------
// the atlas
// ---------------------------------------------------------------------------

// `views` square tiles of `tile` texels, two to a row: 1 view 1x1 tiles, 2 views 2x1, 3 or 4 views 2x2.
struct AtlasLayout {
  uint32_t tile = 0;
  uint32_t cols = 0;
  uint32_t rows = 0;
  uint32_t views = 0;
  uint32_t width() const { return tile * cols; }
  uint32_t height() const { return tile * rows; }
  bool valid() const { return tile != 0 && views != 0; }
};

inline AtlasLayout MakeAtlas(uint32_t tile, uint32_t views) {
  AtlasLayout atlas;
  views = std::min(views, kMaxViews);
  if (tile == 0 || views == 0) {
    return atlas;
  }
  atlas.tile = tile;
  atlas.views = views;
  atlas.cols = views >= 2 ? 2u : 1u;
  atlas.rows = (views + atlas.cols - 1) / atlas.cols;
  return atlas;
}

struct TileRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t size = 0;
};

inline TileRect TileAt(const AtlasLayout& atlas, uint32_t index) {
  TileRect r;
  if (!atlas.valid() || index >= atlas.views) {
    return r;
  }
  r.x = (index % atlas.cols) * atlas.tile;
  r.y = (index / atlas.cols) * atlas.tile;
  r.size = atlas.tile;
  return r;
}

// The tile as atlas uv: {u offset, v offset, u size, v size}.
inline void UvRect(const AtlasLayout& atlas, uint32_t index, float out[4]) {
  const TileRect r = TileAt(atlas, index);
  const double w = double(atlas.width());
  const double h = double(atlas.height());
  out[0] = w > 0 ? float(double(r.x) / w) : 0.0f;
  out[1] = h > 0 ? float(double(r.y) / h) : 0.0f;
  out[2] = w > 0 ? float(double(r.size) / w) : 0.0f;
  out[3] = h > 0 ? float(double(r.size) / h) : 0.0f;
}

// ---------------------------------------------------------------------------
// the lights, the groups and the view that covers a group
// ---------------------------------------------------------------------------

inline constexpr double kPi = 3.14159265358979323846;
inline double Rad(double degrees) { return degrees * kPi / 180.0; }

// What the rig (headlight_rig.h) says of one light, in double.
struct Light {
  double pos[3] = {};
  double dir[3] = {0.0, 0.0, 1.0};  // unit, the way the light shines
  double cos_outer = 0.8;
  double range = 40.0;
  uint32_t car_key = 0;
  bool is_player = false;
  uint32_t index = 0;               // the light's place in the rig's set (LocalShadowView::light_index)
};

inline constexpr double kNearMeters = 1.5;     // casters nearer to the lamps than this are left out (the near plane: the car's own nose)
inline constexpr double kMinRange = 8.0;
inline constexpr double kMaxRange = 80.0;
inline constexpr double kDefaultRange = 40.0;
inline constexpr double kMinFovDegrees = 20.0;
inline constexpr double kMaxFovDegrees = 150.0;
inline constexpr double kFovMarginDegrees = 4.0;
inline constexpr double kOwnCarRadius = 3.5;   // a vehicle's draw whose root is nearer to the view's eye than this is the lamps' own car
inline constexpr uint32_t kMaxDrawsPerView = 1024;

namespace detail {

inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross(const double a[3], const double b[3], double out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool Normalize(double a[3]) {
  const double length = std::sqrt(Dot(a, a));
  if (!(length > 1e-9) || !std::isfinite(length)) {
    return false;
  }
  a[0] /= length;
  a[1] /= length;
  a[2] /= length;
  return true;
}
inline double Distance(const double a[3], const double b[3]) {
  const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(Dot(d, d));
}

}  // namespace detail

// A light the rig gave: finite, a direction that can be normalised, a cone with an angle. A bad light is left out of every view.
inline bool MakeLight(const float pos[3], const float dir[3], float cos_outer, float range, uint32_t car_key, bool is_player, uint32_t index,
                      Light* out) {
  Light l;
  for (int k = 0; k < 3; ++k) {
    l.pos[k] = double(pos[k]);
    l.dir[k] = double(dir[k]);
    if (!std::isfinite(l.pos[k]) || !std::isfinite(l.dir[k])) {
      return false;
    }
  }
  if (!detail::Normalize(l.dir) || !std::isfinite(cos_outer) || cos_outer <= -1.0f || cos_outer >= 1.0f) {
    return false;
  }
  l.cos_outer = double(cos_outer);
  l.range = (std::isfinite(range) && range > 0.0f) ? std::clamp(double(range), kMinRange, kMaxRange) : kDefaultRange;
  l.car_key = car_key;
  l.is_player = is_player;
  l.index = index;
  *out = l;
  return true;
}

// The lights that share one view: the lamps of one car (one car_key).
struct Group {
  std::array<uint32_t, 4> members{};  // indices into the light array handed to GroupLights
  uint32_t count = 0;
  uint32_t car_key = 0;
  bool is_player = false;
  double eye[3] = {};                 // the mean of the lamps' positions
};

inline uint32_t GroupLights(const Light* lights, uint32_t count, uint32_t max_groups, Group* out) {
  std::array<Group, 4> all{};
  uint32_t groups = 0;
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    uint32_t g = 0;
    while (g < groups && all[g].car_key != lights[i].car_key) {
      ++g;
    }
    if (g == groups) {
      all[g] = Group{};
      all[g].car_key = lights[i].car_key;
      ++groups;
    }
    Group& group = all[g];
    group.members[group.count++] = i;
    group.is_player = group.is_player || lights[i].is_player;
  }
  for (uint32_t g = 0; g < groups; ++g) {
    for (int k = 0; k < 3; ++k) {
      double sum = 0.0;
      for (uint32_t m = 0; m < all[g].count; ++m) {
        sum += lights[all[g].members[m]].pos[k];
      }
      all[g].eye[k] = sum / double(all[g].count);
    }
  }
  // The player's car first, then the others nearest first (to the player's lamps, or to the first group when no lamp is the player's).
  const double* reference = all[0].eye;
  for (uint32_t g = 0; g < groups; ++g) {
    if (all[g].is_player) {
      reference = all[g].eye;
      break;
    }
  }
  std::array<double, 4> distance{};
  for (uint32_t g = 0; g < groups; ++g) {
    distance[g] = all[g].is_player ? -1.0 : detail::Distance(all[g].eye, reference);
  }
  std::array<uint32_t, 4> order{0, 1, 2, 3};
  std::stable_sort(order.begin(), order.begin() + groups, [&](uint32_t a, uint32_t b) { return distance[a] < distance[b]; });
  const uint32_t take = std::min(groups, max_groups);
  for (uint32_t k = 0; k < take; ++k) {
    out[k] = all[order[k]];
  }
  return take;
}

// The view that covers a group's lamps.
struct ViewSpec {
  double eye[3] = {};
  double dir[3] = {0.0, 0.0, 1.0};
  double fov = 1.0;     // the full field of view, radians (square)
  double near_m = kNearMeters;
  double far_m = kDefaultRange;
  double lamps_m = 0.0; // how far behind the lamps the eye sits (the lamps are inside the frustum)
};

// One cone from the mean of the lamps, pushed back along the axis until it holds every lamp's own cone: a lamp offset a from the axis
// needs an apex a / tan(half angle) behind it (the cone from there has radius a at the lamp's plane, and from then on contains the lamp's).
inline bool FitView(const Light* lights, const Group& group, ViewSpec* out) {
  if (group.count == 0) {
    return false;
  }
  ViewSpec v;
  double dir[3] = {};
  double range = 0.0;
  for (uint32_t m = 0; m < group.count; ++m) {
    const Light& l = lights[group.members[m]];
    for (int k = 0; k < 3; ++k) {
      dir[k] += l.dir[k];
    }
    range = std::max(range, l.range);
  }
  if (!detail::Normalize(dir)) {
    for (int k = 0; k < 3; ++k) {
      dir[k] = lights[group.members[0]].dir[k];
    }
  }
  double half = 0.0;
  double spread = 0.0;
  for (uint32_t m = 0; m < group.count; ++m) {
    const Light& l = lights[group.members[m]];
    const double tilt = std::acos(std::clamp(detail::Dot(l.dir, dir), -1.0, 1.0));
    half = std::max(half, tilt + std::acos(std::clamp(l.cos_outer, -1.0, 1.0)));
    const double offset[3] = {l.pos[0] - group.eye[0], l.pos[1] - group.eye[1], l.pos[2] - group.eye[2]};
    const double along = detail::Dot(offset, dir);
    const double lateral[3] = {offset[0] - along * dir[0], offset[1] - along * dir[1], offset[2] - along * dir[2]};
    spread = std::max(spread, std::sqrt(detail::Dot(lateral, lateral)));
  }
  half += Rad(kFovMarginDegrees);
  half = std::clamp(half, Rad(kMinFovDegrees) * 0.5, Rad(kMaxFovDegrees) * 0.5);
  v.fov = 2.0 * half;
  const double back = spread / std::tan(half);
  for (int k = 0; k < 3; ++k) {
    v.dir[k] = dir[k];
    v.eye[k] = group.eye[k] - back * dir[k];
  }
  v.lamps_m = back;
  v.near_m = back + kNearMeters;
  v.far_m = std::max(back + std::clamp(range, kMinRange, kMaxRange), v.near_m + 1.0);
  *out = v;
  return true;
}

// The handedness of the game's camera, from the rows of its view inverse (VS c12 right, c13 up, c14 backward): true when right x up = backward.
inline bool IsRightHanded(const tp::Mat4& view_inverse) {
  const double r[3] = {view_inverse.m[0][0], view_inverse.m[0][1], view_inverse.m[0][2]};
  const double u[3] = {view_inverse.m[1][0], view_inverse.m[1][1], view_inverse.m[1][2]};
  const double b[3] = {view_inverse.m[2][0], view_inverse.m[2][1], view_inverse.m[2][2]};
  double c[3];
  detail::Cross(r, u, c);
  return detail::Dot(c, b) > 0.0;
}

// The light's world -> clip, Direct3D clip space (z in [0, w]), looking along spec.dir with the handedness of the game's camera.
inline bool LightClip(const ViewSpec& spec, bool right_handed, tp::Mat4* out) {
  double dir[3] = {spec.dir[0], spec.dir[1], spec.dir[2]};
  if (!detail::Normalize(dir) || !(spec.far_m > spec.near_m) || !(spec.near_m > 0.0) || !(spec.fov > 0.0) || !(spec.fov < kPi)) {
    return false;
  }
  double up[3] = {0.0, 1.0, 0.0};  // world y is up
  if (std::fabs(dir[1]) > 0.98) {
    up[1] = 0.0;
    up[2] = 1.0;
  }
  double z[3];
  for (int k = 0; k < 3; ++k) {
    z[k] = right_handed ? -dir[k] : dir[k];  // the camera's z: backward in a right handed view, forward in a left handed one
  }
  double x[3], y[3];
  detail::Cross(up, z, x);
  if (!detail::Normalize(x)) {
    return false;
  }
  detail::Cross(z, x, y);
  tp::Mat4 v = tp::Identity();
  for (int r = 0; r < 3; ++r) {
    v.m[r][0] = x[r];
    v.m[r][1] = y[r];
    v.m[r][2] = z[r];
  }
  v.m[3][0] = -detail::Dot(x, spec.eye);
  v.m[3][1] = -detail::Dot(y, spec.eye);
  v.m[3][2] = -detail::Dot(z, spec.eye);
  const double s = 1.0 / std::tan(spec.fov * 0.5);
  const double n = spec.near_m;
  const double f = spec.far_m;
  tp::Mat4 p;
  p.m[0][0] = s;
  p.m[1][1] = s;
  if (right_handed) {
    p.m[2][2] = -f / (f - n);
    p.m[2][3] = -1.0;
    p.m[3][2] = -n * f / (f - n);
  } else {
    p.m[2][2] = f / (f - n);
    p.m[2][3] = 1.0;
    p.m[3][2] = -n * f / (f - n);
  }
  *out = tp::Mul(v, p);
  return tp::Finite(*out);
}

// The published matrix (local_shadow_types.h): column-vector, row-major, world p -> (u w, v w, depth w, w) of the ATLAS, so that
// (M * float4(p, 1)).xy / w is the atlas uv and .z / w the depth. `uv_rect` is {u offset, v offset, u size, v size} of the tile; `sx` and `sy`
// are the signs of the pass's NDC x and y (the main view's pos_scale signs: +1 and +1 in a normal view).
inline void WorldToShadow(const tp::Mat4& light_clip, const float uv_rect[4], double sx, double sy, float out[16]) {
  tp::Mat4 t = tp::Identity();
  t.m[0][0] = 0.5 * double(uv_rect[2]) * sx;
  t.m[3][0] = double(uv_rect[0]) + 0.5 * double(uv_rect[2]);
  t.m[1][1] = -0.5 * double(uv_rect[3]) * sy;
  t.m[3][1] = double(uv_rect[1]) + 0.5 * double(uv_rect[3]);
  const tp::Mat4 m = tp::Mul(light_clip, t);
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      out[j * 4 + i] = float(m.m[i][j]);
    }
  }
}

// A world point through a published matrix: the atlas uv and the depth (false behind the light: w <= 0).
inline bool MapPoint(const float world_to_shadow[16], const double p[3], double* u, double* v, double* depth) {
  double o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = double(world_to_shadow[j * 4 + 0]) * p[0] + double(world_to_shadow[j * 4 + 1]) * p[1] + double(world_to_shadow[j * 4 + 2]) * p[2] +
           double(world_to_shadow[j * 4 + 3]);
  }
  if (!(o[3] > 0.0)) {
    return false;
  }
  *u = o[0] / o[3];
  *v = o[1] / o[3];
  *depth = o[2] / o[3];
  return true;
}

// The constant bias the reader subtracts from a receiver's depth (z/w units). The slope scaled part is the pipeline's. For the nearest
// receivers (3 m) it is a fraction of a millimetre, 20 m out about 5 cm and 40 m out 20 cm: shadows far from the car are soft and dim.
inline constexpr float kDepthBias = 2e-4f;
inline constexpr float kSlopeScaledDepthBias = 2.0f;

// ---------------------------------------------------------------------------
// the replay's matrices
// ---------------------------------------------------------------------------

// A bank register's four floats, host order: the bank is 256 registers of four floats.
inline tp::Mat4 BankMatrix(const float* bank, uint32_t first_register) {
  tp::Mat4 m;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      m.m[r][c] = double(bank[(first_register + r) * 4 + c]);
    }
  }
  return m;
}

inline constexpr uint32_t kWorldRegister = 0;      // gWorld c0-c3
inline constexpr uint32_t kWvpRegister = 8;        // gWorldViewProj c8-c11
inline constexpr uint32_t kViewInverseRegister = 12;  // c12-c15

// The view inverse of the game: rows 0-2 the axes with w 0, row 3 the position with w 1.
inline bool LooksLikeViewInverse(const tp::Mat4& m) {
  if (!tp::Finite(m)) {
    return false;
  }
  return std::fabs(m.m[0][3]) < 1e-3 && std::fabs(m.m[1][3]) < 1e-3 && std::fabs(m.m[2][3]) < 1e-3 && std::fabs(m.m[3][3] - 1.0) < 1e-3;
}

// A perspective projection of the game's kind (view space -> clip): the zeros of a symmetric or shifted frustum, w = -z or z.
inline bool LooksLikeProjection(const tp::Mat4& p) {
  if (!tp::Finite(p)) {
    return false;
  }
  const double scale = std::max(std::fabs(p.m[0][0]), std::fabs(p.m[1][1]));
  if (!(scale > 1e-6)) {
    return false;
  }
  const double tol = 1e-3 * scale + 1e-4;
  const double zeros[] = {p.m[0][1], p.m[0][2], p.m[0][3], p.m[1][0], p.m[1][2], p.m[1][3], p.m[3][0], p.m[3][1], p.m[3][3]};
  for (double z : zeros) {
    if (std::fabs(z) > tol) {
      return false;
    }
  }
  return std::fabs(std::fabs(p.m[2][3]) - 1.0) < 1e-2;
}

// The main camera from one draw's bank: its view inverse (c12-c15), its gWorld (c0-c3) and gWorldViewProj (c8-c11). VP = gWorld^-1 x
// gWorldViewProj, and the projection VP leaves once the view is taken off (view inverse x VP) must look like one: a draw whose c0-c3 is not
// its world matrix, or whose c8-c11 is not world x view-projection, does not pass.
struct Camera {
  bool valid = false;
  bool right_handed = true;
  tp::Mat4 view_projection;          // world -> the game's clip
  tp::Mat4 view_projection_inverse;
  double eye[3] = {};
  double forward[3] = {0.0, 0.0, 1.0};  // the way it looks: the view inverse's c14 is BACKWARD, forward is its negative
};

inline Camera DeriveCamera(const float* bank) {
  Camera cam;
  const tp::Mat4 view_inverse = BankMatrix(bank, kViewInverseRegister);
  const tp::Mat4 world = BankMatrix(bank, kWorldRegister);
  const tp::Mat4 wvp = BankMatrix(bank, kWvpRegister);
  tp::Mat4 world_inverse, vp, vp_inverse;
  if (!LooksLikeViewInverse(view_inverse) || !tp::Finite(world) || !tp::Finite(wvp) || !tp::Inverse(world, &world_inverse)) {
    return cam;
  }
  vp = tp::Mul(world_inverse, wvp);
  if (!tp::Finite(vp) || !LooksLikeProjection(tp::Mul(view_inverse, vp)) || !tp::Inverse(vp, &vp_inverse)) {
    return cam;
  }
  if (tp::MaxAbsDiff(tp::Mul(vp, vp_inverse), tp::Identity()) > 1e-3) {
    return cam;
  }
  cam.valid = true;
  cam.right_handed = IsRightHanded(view_inverse);
  cam.view_projection = vp;
  cam.view_projection_inverse = vp_inverse;
  for (int k = 0; k < 3; ++k) {
    cam.eye[k] = view_inverse.m[3][k];
    cam.forward[k] = -view_inverse.m[2][k];
  }
  detail::Normalize(cam.forward);
  return cam;
}

// VP_main^-1 x VP_light: the one matrix a frame's draws are taken through (c8-c11' = c8-c11 x this).
inline bool ReplayMatrix(const Camera& camera, const tp::Mat4& light_clip, tp::Mat4* out) {
  if (!camera.valid) {
    return false;
  }
  *out = tp::Mul(camera.view_projection_inverse, light_clip);
  return tp::Finite(*out);
}

// What a draw's gWorldViewProj (16 floats, the bank's c8-c11) says of it: its model-to-world (c8-c11 x VP_main^-1), whose last column must be
// 0 0 0 1, and its place in the world (the matrix's row 3). `ok` false: the vertex shader is not one of the kind this replay handles.
struct WvpFacts {
  bool ok = false;
  double origin[3] = {};
};

inline WvpFacts CheckWvp(const float* wvp16, const Camera& camera) {
  WvpFacts facts;
  tp::Mat4 wvp;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      wvp.m[r][c] = double(wvp16[r * 4 + c]);
    }
  }
  const tp::Mat4 world = tp::Mul(wvp, camera.view_projection_inverse);
  if (!tp::Finite(world)) {
    return facts;
  }
  const double err = std::max({std::fabs(world.m[0][3]), std::fabs(world.m[1][3]), std::fabs(world.m[2][3]), std::fabs(world.m[3][3] - 1.0)});
  if (err > 2e-3 + 2e-6 * tp::MaxAbs(world)) {
    return facts;
  }
  facts.ok = true;
  for (int k = 0; k < 3; ++k) {
    facts.origin[k] = world.m[3][k];
  }
  return facts;
}

// The replaced registers: out = wvp x replay, 16 floats, row-major, rows = registers (host order, as the bank holds them).
inline void ReplaceWvp(const float* wvp16, const tp::Mat4& replay, float out[16]) {
  tp::Mat4 wvp;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      wvp.m[r][c] = double(wvp16[r * 4 + c]);
    }
  }
  const tp::Mat4 m = tp::Mul(wvp, replay);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(m.m[r][c]);
    }
  }
}

// ---------------------------------------------------------------------------
// which draws are casters
// ---------------------------------------------------------------------------

// Why a draw is not drawn into a view (or at all). Order = the log's counters.
enum class Skip : uint8_t {
  kNone,
  kNotOpaque,    // blended, no depth write, no colour or no pixel shader: not an opaque draw of the colour pass
  kNotTriangles, // lines, points, nothing to draw
  kImpostor,     // a camera-facing card (a vertex shader that folds four corners onto one vertex, or a billboard group's)
  kBillboard,
  kPsDepth,      // the pixel shader writes depth: not a plain caster
  kNotWvp,       // c8-c11 is not gWorld x VP_main (the vertex shader is not of the replay's kind)
  kOwnCar,       // a vehicle's part nearer to the view's eye than kOwnCarRadius: the lamps' own car
  kKindMasked,   // mcla_gpu_headlight_shadow_skip names its kind (debug)
  kPipeline,     // its pipeline is still being made, or failed
  kUnusable,     // no usable geometry
  kBudget,       // over kMaxDrawsPerView: the farthest are left out
  kCount,
};

constexpr const char* SkipName(Skip s) {
  switch (s) {
    case Skip::kNone: return "drawn";
    case Skip::kNotOpaque: return "not opaque";
    case Skip::kNotTriangles: return "not triangles";
    case Skip::kImpostor: return "impostor";
    case Skip::kBillboard: return "billboard";
    case Skip::kPsDepth: return "pixel shader writes depth";
    case Skip::kNotWvp: return "c8-c11 is not world x view-projection";
    case Skip::kOwnCar: return "the lamps' own car";
    case Skip::kKindMasked: return "kind masked";
    case Skip::kPipeline: return "pipeline pending or failed";
    case Skip::kUnusable: return "unusable geometry";
    case Skip::kBudget: return "over the budget";
    case Skip::kCount: break;
  }
  return "?";
}

// The kinds of a caster that count apart (mcla_gpu_headlight_shadow_skip is a mask of these, debug only).
enum class Kind : uint8_t { kOpaque, kAlphaTested, kSkinned, kVehicle, kCount };
inline constexpr uint32_t KindBit(Kind k) { return 1u << uint32_t(k); }
constexpr const char* KindName(Kind k) {
  switch (k) {
    case Kind::kOpaque: return "opaque";
    case Kind::kAlphaTested: return "alpha tested";
    case Kind::kSkinned: return "skinned";
    case Kind::kVehicle: return "vehicle";
    case Kind::kCount: break;
  }
  return "?";
}

// The vertex shaders (runtime identity, shaders.h: FNV-1a 64 of the vfetch-normalised microcode) of the camera-facing cards that a replay from
// another point of view would turn toward the MAIN camera: the city's billboards (xCityBillboard VSCityBillboard and VSCityBillboardEnv, the
// distant buildings' cards) and the foliage impostors in the main view (xPropFoliageImpostor VSPropInstanceFoliage and its night twin). The
// impostors also fold four corners onto one source vertex, which the pack flags (ShaderStore::kComputedFetchIndex, also the rain's particle
// vertex shader), so they are left out by that flag as well; the billboards only by this list. Sorted by identity.
// The identities were made from the game's containers by perf/scripts/vs_identities.py (the runtime's normalisation ported to Python and checked on
// the anchor's and the tone map's vertex shaders, which it finds among the 1280 of the game): `python -I perf/scripts/vs_identities.py
// "Billboard|Impostor"`. A different game build gives other identities and then nothing here matches: a billboard is drawn like any card.
struct NamedVs {
  uint64_t key;
  const char* name;
};
inline constexpr NamedVs kBillboardVertexShaders[] = {
    {0x28160E3F809C1C64ull, "xPropFoliageImpostor__VSPropInstanceFoliage"},
    {0x4FE2A70BEFE5A3CFull, "xCityBillboard__VSCityBillboard"},
    {0xB11D7CEC72A529CFull, "xPropFoliageImpostor__VSPropInstanceFoliageNight"},
    {0xF7D58087866B8AB1ull, "xCityBillboard__VSCityBillboardEnv"},
};

inline bool IsBillboardVs(uint64_t key) {
  for (const NamedVs& v : kBillboardVertexShaders) {
    if (v.key == key) {
      return true;
    }
  }
  return false;
}

// What the collector knows of one prepared draw.
struct DrawFacts {
  bool color_pass = false;        // colour 0 and depth of the main view's play view
  uint32_t color_count = 0;
  bool depth_test = false;
  bool depth_write = false;
  bool blend0 = false;            // blending on colour target 0
  bool has_ps = false;
  bool ps_writes_depth = false;
  bool triangles = false;
  bool alpha_tested = false;      // the pixel shader's alpha test variant (spec variant 1)
  bool vs_computed_fetch = false; // the vertex shader folds corners (impostors)
  bool vs_skinned = false;        // BLENDINDICES in the vertex shader's inputs
  bool vehicle = false;           // the draw writes a vehicle's stencil ID
  uint64_t vs_key = 0;
};

inline Skip ClassifyDraw(const DrawFacts& f, uint32_t kind_mask, Kind* kind) {
  *kind = Kind::kOpaque;
  if (!f.color_pass || f.color_count == 0 || !f.depth_test || !f.depth_write || f.blend0 || !f.has_ps) {
    return Skip::kNotOpaque;
  }
  if (!f.triangles) {
    return Skip::kNotTriangles;
  }
  if (f.vs_computed_fetch) {
    return Skip::kImpostor;
  }
  if (IsBillboardVs(f.vs_key)) {
    return Skip::kBillboard;
  }
  if (f.ps_writes_depth) {
    return Skip::kPsDepth;
  }
  *kind = f.vehicle ? Kind::kVehicle : f.vs_skinned ? Kind::kSkinned : f.alpha_tested ? Kind::kAlphaTested : Kind::kOpaque;
  // The alpha test is a mask of its own: a skinned or vehicle part that is alpha tested is masked by either bit.
  if ((kind_mask & KindBit(*kind)) != 0 || (f.alpha_tested && (kind_mask & KindBit(Kind::kAlphaTested)) != 0)) {
    return Skip::kKindMasked;
  }
  return Skip::kNone;
}

// A vehicle's part whose root is within kOwnCarRadius of the view's eye belongs to the car the lamps are on: it casts nothing into its own lamps.
inline bool IsOwnCar(const double origin[3], const double eye[3]) { return detail::Distance(origin, eye) < kOwnCarRadius; }

// ---------------------------------------------------------------------------
// the face culling and the pipelines
// ---------------------------------------------------------------------------

enum class Cull : uint8_t { kNone, kFront, kBack };

// mcla_gpu_headlight_shadow_cull (debug): 0 the opposite faces to the game's (the faces turned from the light: front face culling for the
// usual back face culled mesh), 1 none, 2 the game's own. A two sided mesh (the game culls nothing) stays two sided: a fence or a leaf casts
// whichever way it faces.
inline Cull ResolveCull(int policy, Cull game) {
  if (game == Cull::kNone || policy == 1) {
    return Cull::kNone;
  }
  if (policy == 2) {
    return game;
  }
  return game == Cull::kBack ? Cull::kFront : Cull::kBack;
}

// A pipeline of the replay: the game's vertex shader (and its own pixel shader, for the alpha tested draws, which the driver cuts down to the
// alpha test: there is no colour target), the layout of the draw's vertex declaration, the topology, the faces drawn.
struct PipeKey {
  uint64_t vs = 0;
  uint64_t ps = 0;  // 0: no pixel shader
  uint32_t vs_variant = 0;
  uint32_t ps_variant = 0;
  uint32_t layout = 0;
  uint32_t topology = 0;
  uint8_t cull = 0;
  uint8_t front_ccw = 0;
  uint8_t pad[2] = {};
  bool operator==(const PipeKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(PipeKey) == 40, "PipeKey has no padding: it is compared as bytes");

struct PipeKeyHash {
  size_t operator()(const PipeKey& k) const {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(&k);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < sizeof(PipeKey) / 8; ++i) {
      h ^= w[i];
      h *= 0x100000001B3ull;
      h ^= h >> 29;
    }
    return size_t(h);
  }
};

// ---------------------------------------------------------------------------
// the casters a view keeps
// ---------------------------------------------------------------------------

// One caster as the selection sees it.
struct Candidate {
  uint32_t index = 0;
  double distance = 0.0;  // from the view's eye to the draw's origin
  bool vehicle = false;
};

// Which of the candidates a view draws: the lamps' own car's parts go (kOwnCar, counted), and when more than `budget` remain the farthest are
// left out (kBudget). `keep` receives the indices in draw order. Returns the counts through `skipped` (indexed by Skip).
inline uint32_t SelectForView(const Candidate* candidates, uint32_t count, uint32_t budget, uint32_t* keep,
                              uint32_t skipped[uint32_t(Skip::kCount)]) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (candidates[i].vehicle && candidates[i].distance < kOwnCarRadius) {
      ++skipped[uint32_t(Skip::kOwnCar)];
      continue;
    }
    keep[n++] = i;
  }
  if (n > budget) {
    // The nearest `budget`, kept in draw order.
    std::nth_element(keep, keep + budget, keep + n, [&](uint32_t a, uint32_t b) { return candidates[a].distance < candidates[b].distance; });
    skipped[uint32_t(Skip::kBudget)] += n - budget;
    n = budget;
    std::sort(keep, keep + n);
  }
  return n;
}

inline double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

}  // namespace mcla::gpu::local_shadow
