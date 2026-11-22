#pragma once
// ===========================================================================
// MCLA GPU renderer -- the headlight shadow maps: the rules that need no renderer
// ===========================================================================
// The renderer's half is local_shadow.h (perf/LOCAL-SHADOWS-SURVEY.md, option A). This header holds what needs no Plume, no ReXGlue and
// no game, as pure functions on plain numbers, so that local_shadow_rules_test.cpp builds on its own:
//
//   - the setting: mcla_gpu_light_shadows' words and THE LEVEL TABLE (kLevelTable: the size of a view, how many cars a level shadows and for how many of them each
//     headlamp has a view of its own; the three level shape is the one in use since 2026-11-20, kThreeLevelShape picks the table: one constant), Resolve, MaxViews;
//   - the lights: which of the rig's lights share a view (BuildGroups: one view covers all the lamps of a car), which cars get views (ViewPicker: the player's car,
//     then the others by their importance, with a hysteresis so that two cars of the same importance do not trade a view back and forth), the views of a car (PlanViews:
//     one for each headlamp, or one for the car), the view that covers a group (FitView: eye, direction, field of view, near and far planes) and its matrices (LightClip,
//     WorldToShadow);
//   - the atlas: where view k sits in it, in texels and in uv, and what it holds in video memory (MakeAtlas, TileRect, UvRect, AtlasBytes);
//   - the replay's matrix: a main-view draw's gWorldViewProj (VS c8-c11) turned into model -> light clip (ReplaceWvp), and the main camera's
//     view-projection found from the draws' own constants (DeriveCamera);
//   - which draws are casters (ClassifyDraw: opaque, alpha tested; skinned, impostor, billboard, a vehicle's own parts...) and which of them
//     a view keeps (the view's cone and reach: SphereInView; the lamps' own car and the budget: SelectForView);
//   - the pipelines' key (PipeKey) and the face culling a caster is drawn with (ResolveCull; ResolveCasterCull for a vehicle's part);
//   - the ground skirt: the second copy of a vehicle part that closes the gap under a car (SkirtShift, SkirtReplayMatrix);
//   - the census of the vehicles' draws by shader (VehicleCensus).
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
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu_class.h"
#include "light_budget.h"
#include "light_influence.h"
#include "light_pick.h"
#include "local_shadow_types.h"
#include "temporal_math.h"

namespace mcla::gpu::local_shadow {

namespace tp = ::mcla::gpu::temporal;

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

// mcla_gpu_light_shadows (was mcla_gpu_headlight_shadows): auto, off, medium, high or ultra (the three level shape below; the two level shape has no medium). No level is
// called low (owner, 2026-11-14): the levels are the table's.
//
// THE LEVELS (owner, 2026-11-20): a level differs by how many cars' lights are shadowed, by how many of those have a view for EACH HEADLAMP (a pole that both lamps light
// casts two shadows, and a car's shadow has the half-lit band of one lamp) and by how sharp the views are. A car's light is one term (the middle of its two lamps,
// headlight_types.h) whose shadow is the mean of its lamps' views.
// THE LEVEL TABLE LIVES IN light_budget.h since LU-3 (the one budget table of the lights): the level words, the rows, the two shapes, and the cars and views a level gives. The names here
// are the same ones, so nothing that reads them changes.
using Level = light_budget::Level;

// One row of the level table: the setting's word, the texels of a view's side, the cars it shadows (the player's first, then the nearest others) and, of those, how many
// (in the same order) have a view for each lamp: the other cars have one view for the car.
using LevelRow = light_budget::ShadowRow;

// THE TWO LEVEL SHAPE (the first plan of 2026-11-20, kept for the tests and as a way back) and THE THREE LEVEL SHAPE (IN USE since 2026-11-20): light_budget.h holds both tables and the
// reasons for the three level shape (medium 2 views, high 3 views, ultra 6 views; auto: Mid medium, High ultra, Low off: gpu_class.h).
// THE SIZE OF A VIEW (HL-SHADOW-QUALITY, 2026-11-21: the owner saw the poles', signs' and kerbs' shadows "extremely dither-y and nowhere near as sharp as they should be" on 4K):
// the tile is chosen so that one texel stays small in the WORLD where the shadows are looked at, whatever the level: TexelMeters below says how many centimetres a texel is at 10 m
// from the lamps (the view's field is about 72 degrees for a car's view and 80 for a lamp's: kShadowConeCapDegrees). The first version's 512 gave 3.5 cm at 10 m, 7 cm at 20 m
// and a bollard (10 cm) or a sign post 1.4 to 3 texels wide: thinner than the old lookup's 3 texel box, so its shadow came and went as the car moved. Now medium 768 (1.9 cm at
// 10 m, 9 MiB), high 1024 (1.4 to 1.6 cm, 24 MiB), ultra 1280 (1.3 cm for a lamp's view, 75 MiB): the numbers are in light_budget.h kThreeLevels. The graphics card time of a view
// hardly depends on its size (the replay is vertex bound: perf/out/HL-PER-LAMP.txt), what grows is the video memory and the clear of the atlas.
inline constexpr auto& kTwoLevels = light_budget::kTwoLevels;
inline constexpr auto& kThreeLevels = light_budget::kThreeLevels;
// THE ONE DATA CHANGE between the shapes. The rest follows: the pause menu's words (MenuWords), the class table's cells (gpu_class.h kEffects: Mid's `auto` is `medium` in the
// three level shape and was `high` in the two level one; a cell the table does not have takes the next level up, Row below), the setting's allowed words and help text
// (settings_gpu.h, manifest.json).
inline constexpr bool kThreeLevelShape = true;
inline constexpr std::span<const LevelRow> kLevelTable = kThreeLevelShape ? std::span<const LevelRow>(light_budget::kThreeLevels) : std::span<const LevelRow>(light_budget::kTwoLevels);

using LevelTable = std::span<const LevelRow>;

// The published frame holds this many views (local_shadow_types.h); no level makes more than the table's greatest (6).
inline constexpr uint32_t kMaxViews = larecomp::gpu::kMaxShadowViews;
// The cars that can have views: the rig lights four.
inline constexpr uint32_t kMaxCars = light_budget::kMaxShadowCars;

// Every function below takes the table too (default: the shape in use), so that the tests read both shapes without building twice.

// The row a level has in the table: its own, else (a level the shape does not have: `medium` in the two level shape, only the tests' table now) the next one up, else the greatest
// below. Null for off.
constexpr const LevelRow* Row(Level level, LevelTable table = kLevelTable) { return light_budget::ShadowRowOf(level, table); }

// The level a saved level comes to in this shape (off stays off).
constexpr Level Effective(Level level, LevelTable table = kLevelTable) {
  const LevelRow* row = Row(level, table);
  return row ? row->level : Level::kOff;
}

constexpr uint32_t TileSize(Level level, LevelTable table = kLevelTable) { return light_budget::ShadowTile(level, table); }

constexpr std::string_view LevelName(Level level) {
  return level == Level::kUltra ? "ultra" : level == Level::kHigh ? "high" : level == Level::kMedium ? "medium" : "off";
}

// The cars a level shadows (the player's car first). The debug setting does not change it: it is the views of a car (below).
constexpr uint32_t MaxCars(Level level, LevelTable table = kLevelTable) { return light_budget::ShadowCars(level, table); }

// How many cars (the first ones, the player's first) have a view for each lamp. `views_per_car` is the debug setting mcla_gpu_headlight_shadow_views: 1 gives every car one
// view (the first version's), 2 gives every car a view for each lamp, 0 takes the level's own.
constexpr uint32_t LampCars(Level level, int views_per_car, LevelTable table = kLevelTable) { return light_budget::ShadowLampCars(level, views_per_car, table); }

// How many views a level draws at the most: a view for each shadowed car and a second for each car that has its lamps apart.
constexpr uint32_t MaxViews(Level level, int views_per_car, LevelTable table = kLevelTable) { return light_budget::ShadowViews(level, views_per_car, table); }

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

// The words of the setting, the table's or not (a word the shape does not have, `medium` in the two level one, is a value that comes to the next level up: Effective). The three
// level shape has all of them.
inline Level ParseLevel(std::string_view text, bool* known) {
  *known = true;
  if (text == "off") return Level::kOff;
  if (text == "medium") return Level::kMedium;
  if (text == "high") return Level::kHigh;
  if (text == "ultra") return Level::kUltra;
  *known = false;
  return Level::kOff;
}

// `auto` is the class table's cell (gpu_class.h Effect::kLightShadows: off on shared memory, medium on Mid, ultra on High); a word that is
// no value of the setting (the low of an older larecomp.toml) counts as `auto`. The level a Choice holds is always one the table has.
inline Choice Resolve(gpu_class::GpuClass cls, std::string_view saved) {
  Choice choice;
  const std::string text = Lower(saved);
  bool known = false;
  const Level own = ParseLevel(text, &known);
  if (known) {
    choice.level = Effective(own);
    return choice;
  }
  choice.automatic = true;
  choice.unknown = text != "auto";
  bool cell_known = false;
  choice.level = Effective(ParseLevel(gpu_class::Cell(gpu_class::Effect::kLightShadows, cls), &cell_known));
  return choice;
}

// The pause menu's row: the stored words (and the shown ones, upper case) in the order the row lists them: auto, off, then the table's levels. The row's own code builds its
// arrays from this, so that a change of shape is one change (kThreeLevelShape).
inline constexpr uint32_t kMenuWordMax = 5;  // auto, off and up to three levels
struct MenuWords {
  std::array<std::string_view, kMenuWordMax> stored{};
  std::array<std::string_view, kMenuWordMax> shown{};
  uint32_t count = 0;
};
constexpr MenuWords MakeMenuWords(LevelTable table = kLevelTable) {
  MenuWords w;
  w.stored[w.count] = "auto";
  w.shown[w.count++] = "AUTO";
  w.stored[w.count] = "off";
  w.shown[w.count++] = "OFF";
  for (const LevelRow& row : table) {
    if (w.count < kMenuWordMax) {
      w.stored[w.count] = row.word;
      w.shown[w.count++] = row.level == Level::kUltra ? "ULTRA" : row.level == Level::kHigh ? "HIGH" : "MEDIUM";
    }
  }
  return w;
}
inline constexpr MenuWords kMenuWords = MakeMenuWords();

// "light shadows high, up to 3 views of 512x512: 2 car(s), a view for each lamp of the first 1 (set)".
inline std::string Describe(const Choice& choice, gpu_class::GpuClass cls, int views_per_car = 0) {
  std::string text = "light shadows ";
  text += LevelName(choice.level);
  if (choice.level != Level::kOff) {
    const uint32_t cars = MaxCars(choice.level);
    const uint32_t lamps = LampCars(choice.level, views_per_car);
    text += ", up to " + std::to_string(MaxViews(choice.level, views_per_car)) + " views of " + std::to_string(choice.tile()) + "x" + std::to_string(choice.tile()) + ": " +
            std::to_string(cars) + " car(s), ";
    text += lamps == 0 ? "one view for each" : lamps >= cars ? "a view for each lamp" : "a view for each lamp of the first " + std::to_string(lamps);
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

// `views` square tiles of `tile` texels: one row for up to three views (1 view 1x1 tiles, 2 views 2x1, 3 views 3x1), 2 x 2 for four, 3 x 2 for five and six (ultra: 3072 x
// 2048), 4 x 2 for seven and eight. The size follows the level, never the number of cars lit at the moment, so the atlas is not made again when a car comes into view.
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
  atlas.cols = views <= 3 ? views : views == 4 ? 2u : views <= 6 ? 3u : 4u;
  atlas.rows = (views + atlas.cols - 1) / atlas.cols;
  return atlas;
}

// The atlas a level makes (`views_per_car` as MaxViews): medium 2 tiles of 512 (1024 x 512), high 3 of 512 (1536 x 512), ultra 6 of 1024 (3072 x 2048); in the two level shape
// high is 4 tiles of 512 (1024 x 1024).
inline AtlasLayout AtlasFor(Level level, int views_per_car, LevelTable table = kLevelTable) {
  return MakeAtlas(TileSize(level, table), MaxViews(level, views_per_car, table));
}

// What the atlas holds in video memory: D32 float + S8 is 8 bytes a texel (plume's RenderFormatSize: 24 bits unused), a lower bound (the driver pads). In the three level shape
// medium is 4 MiB, high 6 MiB and ultra 48 MiB (the two level shape's high is 8 MiB; the first version's ultra was 24 with one view for each car). CHECK: D32 float alone (4 bytes a texel: half of this) needs the depth plane read as it is on both backends.
inline constexpr uint64_t kAtlasTexelBytes = 8;
inline uint64_t AtlasBytes(const AtlasLayout& atlas) { return uint64_t(atlas.width()) * atlas.height() * kAtlasTexelBytes; }

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
  double shadow_half = 0.0;         // the half angle the shadow view must hold (radians): the cone's, held to kShadowConeCapDegrees by MakeLight; 0: the cone's own (a Light made by hand)
  double range = 40.0;
  uint32_t car_key = 0;
  bool is_player = false;
  uint32_t index = 0;               // the light's place in the rig's set (LocalShadowView::light_index)
  double lamp_offset[3] = {};       // from the light to the car's right headlamp (the left one is the opposite): headlight_types.h; zero: the lamps are not known
};

inline constexpr double kNearMeters = 1.5;     // casters nearer to the lamps than this are left out (the near plane: the car's own nose)
inline constexpr double kMinRange = 8.0;
inline constexpr double kMaxRange = 80.0;
inline constexpr double kDefaultRange = 40.0;
inline constexpr double kMinFovDegrees = 20.0;
inline constexpr double kMaxFovDegrees = 150.0;
inline constexpr double kFovMarginDegrees = 2.0;
// A view holds the beam's cone up to this half angle only (the beam's own is 38 degrees, the weight of the light there is 12 percent at 34 and falling: light_influence / the rig's
// smoothstep from the outer to the inner cosine): the field is about 72 degrees instead of 84, so every texel is a fifth smaller in the world for the same tile. A receiver outside
// the view's rectangle is lit, and the lookup fades the shadow out over the last kHlBorderFade of the tile so that the edge of the view is no line in the picture.
inline constexpr double kShadowConeCapDegrees = 34.0;
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
                      Light* out, const float* lamp_offset = nullptr) {
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
  l.shadow_half = std::min(std::acos(l.cos_outer), Rad(kShadowConeCapDegrees));
  l.range = (std::isfinite(range) && range > 0.0f) ? std::clamp(double(range), kMinRange, kMaxRange) : kDefaultRange;
  l.car_key = car_key;
  l.is_player = is_player;
  l.index = index;
  if (lamp_offset) {
    // A lamp offset that is not a number or longer than a car is wide is not believed: the light then has one view.
    double length2 = 0.0;
    bool finite = true;
    for (int k = 0; k < 3; ++k) {
      finite = finite && std::isfinite(lamp_offset[k]);
      length2 += double(lamp_offset[k]) * double(lamp_offset[k]);
    }
    if (finite && length2 <= 2.0 * 2.0) {
      for (int k = 0; k < 3; ++k) {
        l.lamp_offset[k] = double(lamp_offset[k]);
      }
    }
  }
  *out = l;
  return true;
}

/// The lights that share one view: the lamps of one car (one car_key).
struct Group {
  std::array<uint32_t, 4> members{};  // indices into the light array handed to BuildGroups
  uint32_t count = 0;
  uint32_t car_key = 0;
  bool is_player = false;
  double eye[3] = {};                 // the mean of the lamps' positions
};

// The lamps of `count` lights (at most four, the rig's set) grouped by car, in the order the cars first appear. Returns how many groups (cars) there are.
inline uint32_t BuildGroups(const Light* lights, uint32_t count, Group out[4]) {
  uint32_t groups = 0;
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    uint32_t g = 0;
    while (g < groups && out[g].car_key != lights[i].car_key) {
      ++g;
    }
    if (g == groups) {
      out[g] = Group{};
      out[g].car_key = lights[i].car_key;
      ++groups;
    }
    Group& group = out[g];
    group.members[group.count++] = i;
    group.is_player = group.is_player || lights[i].is_player;
  }
  for (uint32_t g = 0; g < groups; ++g) {
    for (int k = 0; k < 3; ++k) {
      double sum = 0.0;
      for (uint32_t m = 0; m < out[g].count; ++m) {
        sum += lights[out[g].members[m]].pos[k];
      }
      out[g].eye[k] = sum / double(out[g].count);
    }
  }
  return groups;
}

// ---------------------------------------------------------------------------
// which cars get a view
// ---------------------------------------------------------------------------

// A level shadows a few cars (MaxCars) and the rig lights up to four: the player's car always has its views (when its lamps are lit), the others compete for the rest
// by their importance, the same one the rig ranks its own lights by (headlight_rig_rules.h Tracker: 1 / (1 + (d / 25)^2) from the camera, 0.35 times that for a car
// whose beam lights nothing in the picture, since a shadow nobody can see is wasted, 1 for one whose beam is in the view: light_influence.h BeamReach; the first version
// asked whether the car's LAMP was on the screen, and so ranked a car at the edge of the frame, whose beam crosses the picture, as if it lit nothing): the nearer a car and the
// more of its beam is in the picture, the more of the picture it lights. A car
// that holds a view counts kViewKeepBonus times more than it is worth, so a challenger must beat it by that much: two cars of about the same importance, which
// swap places with every metre of distance, would otherwise trade the view back and forth (a shadow that pops in and out of the other car's headlights every few
// frames). The rig's own picking has the same hysteresis (keep_bonus 1.5) for the four lights it lights.
inline constexpr double kViewHalfDistance = 25.0;   // m: the distance at which a car's importance is a half
inline constexpr double kViewOffScreen = 0.35;      // the importance of a car whose lamps are off the screen, as a share
inline constexpr double kViewKeepBonus = 1.3;       // a car that holds a view is worth this much more than it is
inline constexpr double kViewScreenMargin = 1.3;    // how far outside the screen (a share of the clip box) a lamp may be and still count as on it (the rig's number)

// What the picking knows of the frame. The camera is the LAST frame's (the picking is made when the frame opens, the camera is found from the frame's draws): a
// car's distance and place on the screen change a little in one frame. `have_reference` false: the distance is from the player's lamps (or the first car's when no
// lamp is the player's).
struct PickContext {
  bool have_reference = false;
  double reference[3] = {};              // the camera's eye
  bool have_screen = false;
  tp::Mat4 view_projection;              // world -> the game's clip (row vectors: DeriveCamera's)
  // HL-EDGE-FADE (owner 2026-11-20: a traffic car's headlights fade near the frame's edge): the camera's frustum. When it is valid a car's importance is how much of its BEAM
  // is in the view (light_influence.h BeamReach of the car's lamps: the strongest), not where its lamp is: a car at the edge of the frame whose beam crosses the picture is
  // as important as one in the middle of it. Without it (not valid) the first version's rule applies: have_screen and view_projection say whether the lamps are on the screen.
  light_influence::ViewFrustum frustum;
};

// A world point inside the screen with `margin` (a share of the clip box: 1.3 is a third beyond each edge); a point behind the camera is not.
inline bool OnScreen(const tp::Mat4& view_projection, const double p[3], double margin) {
  double o[4];
  for (int j = 0; j < 4; ++j) {
    o[j] = p[0] * view_projection.m[0][j] + p[1] * view_projection.m[1][j] + p[2] * view_projection.m[2][j] + view_projection.m[3][j];
  }
  return o[3] > 0.0 && std::fabs(o[0]) <= margin * o[3] && std::fabs(o[1]) <= margin * o[3];
}

// The importance of a car for a view from how much of its beam is in the view (0 .. 1: light_influence.h BeamReach): kViewOffScreen (0.35) for a car whose beam lights nothing
// in the picture, 1 from a reach of light_influence::kReachFull, smooth between (the first version's `on_screen` was a step: 1 or 0.35 by the lamp's place).
inline double ViewImportanceReach(double distance, double reach, bool holds_view) {
  const double r = distance / kViewHalfDistance;
  double score = 1.0 / (1.0 + r * r);
  score *= light_influence::ReachWeight(reach);
  if (holds_view) {
    score *= kViewKeepBonus;
  }
  return score;
}

// How much of a group's beams is in the view: the strongest reach of its lamps (the lights of one car share a view).
inline double GroupReach(const Light* lights, const Group& group, const light_influence::ViewFrustum& frustum) {
  double reach = 0.0;
  for (uint32_t m = 0; m < group.count; ++m) {
    const Light& l = lights[group.members[m]];
    reach = std::max(reach, light_influence::BeamReach(frustum, l.pos, l.dir, l.range, std::acos(std::clamp(l.cos_outer, -1.0, 1.0))));
  }
  return reach;
}

// The importance of a car for a view (the player's is above every number this makes).
inline double ViewImportance(double distance, bool on_screen, bool holds_view) {
  const double r = distance / kViewHalfDistance;
  double score = 1.0 / (1.0 + r * r);
  if (!on_screen) {
    score *= kViewOffScreen;
  }
  if (holds_view) {
    score *= kViewKeepBonus;
  }
  return score;
}

// Picks the cars that get a view, frame after frame: it remembers which cars held one in the last pick (the hysteresis). One of these lives as long as the
// feature does; Forget() when the lights have been off for a while (a new night is a new start).
// LU-3: the pick itself is light_pick.h's (the held pick of its shadow_cars picker: the importance, the keep bonus of 1.3, the rank); this class finds the groups, the reference and each car's
// reach and hands them over (the unified mode, the default), or runs the code that was here (mcla_gpu_light_pick = legacy, the A/B). `held_` is the last pick either way.
class ViewPicker {
 public:
  ViewPicker() : mode_(light_pick::DefaultMode()) {}

  void SetMode(light_pick::PickMode mode) {
    if (mode != mode_) {
      mode_ = mode;
      seed_pending_ = mode == light_pick::PickMode::kUnified;
    }
  }
  light_pick::PickMode mode() const { return mode_; }
  // The picker the unified mode uses: the LightManager's (light_pick.h LightPicker::shadow_cars). Null (the default): this picker's own.
  void SetPicker(light_pick::Picker* picker) { external_ = picker; }

  // The groups of `lights`, the player's car first and then by importance (ties: the lower car key), at most `max_cars` of them, into `out` (room for four).
  // Returns how many. Updates the cars that hold views. (A car that holds views has all of them: its lamps' views come and go together.)
  uint32_t Pick(const Light* lights, uint32_t count, uint32_t max_cars, const PickContext& context, Group* out) {
    Group all[4];
    const uint32_t groups = BuildGroups(lights, count, all);
    if (max_cars == 0) {
      held_count_ = 0;
      picker().Forget();
      return 0;
    }
    if (groups == 0) {
      return 0;  // no lamp lit at the moment: who holds a view is kept (a night's gap of a few frames), Forget() ends it
    }
    // The distances are from the camera, or from the player's lamps (the first car's when no lamp is the player's).
    const double* reference = all[0].eye;
    for (uint32_t g = 0; g < groups; ++g) {
      if (all[g].is_player) {
        reference = all[g].eye;
        break;
      }
    }
    if (context.have_reference) {
      reference = context.reference;
    }
    if (mode_ == light_pick::PickMode::kUnified) {
      return PickUnified(lights, all, groups, reference, max_cars, context, out);
    }
    std::array<double, 4> score{};
    for (uint32_t g = 0; g < groups; ++g) {
      if (all[g].is_player) {
        score[g] = kPlayerScore - detail::Distance(all[g].eye, reference) * 1e-6;  // a player's lamps in two groups (not made by the rig): the nearer first
        continue;
      }
      if (context.frustum.valid) {
        score[g] = ViewImportanceReach(detail::Distance(all[g].eye, reference), GroupReach(lights, all[g], context.frustum), Holds(all[g].car_key));
        continue;
      }
      const bool on_screen = !context.have_screen || OnScreen(context.view_projection, all[g].eye, kViewScreenMargin);
      score[g] = ViewImportance(detail::Distance(all[g].eye, reference), on_screen, Holds(all[g].car_key));
    }
    std::array<uint32_t, 4> order{0, 1, 2, 3};
    std::stable_sort(order.begin(), order.begin() + groups, [&](uint32_t a, uint32_t b) {
      if (score[a] != score[b]) {
        return score[a] > score[b];
      }
      return all[a].car_key < all[b].car_key;
    });
    const uint32_t take = std::min({groups, max_cars, kMaxCars});
    held_count_ = take;
    for (uint32_t k = 0; k < take; ++k) {
      out[k] = all[order[k]];
      held_[k] = out[k].car_key;
    }
    return take;
  }

  void Forget() {
    held_count_ = 0;
    picker().Forget();
  }
  uint32_t held_count() const { return held_count_; }
  uint32_t held_key(uint32_t i) const { return i < held_count_ ? held_[i] : 0u; }
  bool Holds(uint32_t car_key) const {
    for (uint32_t i = 0; i < held_count_; ++i) {
      if (held_[i] == car_key) {
        return true;
      }
    }
    return false;
  }

 private:
  static constexpr double kPlayerScore = light_pick::kPlayerScore;

  light_pick::Picker& picker() { return external_ ? *external_ : own_picker_; }

  // The unified pick: one entry for each group (a car), the held pick of light_pick.h, the groups it chose in its order.
  uint32_t PickUnified(const Light* lights, const Group* all, uint32_t groups, const double* reference, uint32_t max_cars, const PickContext& context, Group* out) {
    light_pick::Picker& pick = picker();
    pick.SetTuning(light_pick::ShadowCarTuning());
    if (seed_pending_) {
      // A switch from the legacy mode in a run: the picker takes who holds views.
      seed_pending_ = false;
      pick.Forget();
      for (uint32_t i = 0; i < held_count_; ++i) {
        pick.Seed(held_[i], true, 1.0f, false);
      }
    }
    entries_.clear();
    for (uint32_t g = 0; g < groups; ++g) {
      light_pick::Entry e;
      e.id = all[g].car_key;
      e.player = all[g].is_player;
      e.distance = detail::Distance(all[g].eye, reference);
      if (e.player) {
        e.player_penalty = e.distance * 1e-6;  // a player's lamps in two groups (not made by the rig): the nearer first
      } else if (context.frustum.valid) {
        e.view_known = true;
        e.reach = GroupReach(lights, all[g], context.frustum);
      } else {
        e.on_screen = !context.have_screen || OnScreen(context.view_projection, all[g].eye, kViewScreenMargin);
      }
      entries_.push_back(e);
    }
    const uint32_t take = std::min({groups, max_cars, kMaxCars});
    pick.PickHeld(entries_, take, &result_);
    held_count_ = take;
    for (uint32_t k = 0; k < take; ++k) {
      out[k] = all[result_.shown[k]];
      held_[k] = out[k].car_key;
    }
    return take;
  }

  light_pick::PickMode mode_ = light_pick::PickMode::kUnified;
  light_pick::Picker own_picker_{light_pick::ShadowCarTuning()};
  light_pick::Picker* external_ = nullptr;
  bool seed_pending_ = false;
  std::vector<light_pick::Entry> entries_;
  light_pick::Result result_;
  std::array<uint32_t, kMaxCars> held_{};
  uint32_t held_count_ = 0;
};

// The picking with no history and no camera: the player's car first, then the others nearest first to the player's lamps, at most `max_groups` (the first
// version's rule, kept for the checks of the groups and the views).
inline uint32_t GroupLights(const Light* lights, uint32_t count, uint32_t max_groups, Group* out) {
  ViewPicker picker;
  return picker.Pick(lights, count, max_groups, PickContext{}, out);
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

// The half angle (radians) of the cone a light's shadow view must hold: shadow_half when MakeLight made it, else the cone's own.
inline double ShadowHalf(const Light& l) { return l.shadow_half > 0.0 ? l.shadow_half : std::acos(std::clamp(l.cos_outer, -1.0, 1.0)); }

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
    half = std::max(half, tilt + ShadowHalf(l));
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

// ---------------------------------------------------------------------------
// the views of a car: one for each headlamp, or one for the car
// ---------------------------------------------------------------------------

// The rig's light is a CAR's (the middle of its two headlamps, one term); what a level shadows is the lamps: a car that has its lamps apart (the light's lamp_offset)
// and is among the first LampCars of a level gets a view from each headlamp, so that a pole in front of it casts two shadows (the shader's share of the light is the mean
// of the two) and a car's shadow has the half-lit band of one lamp. The other cars, and a light whose lamps are not known, have one view from the light.
enum class LampSide : uint8_t { kWhole, kA, kB };  // from the car's light, from the lamp at light - offset, from the lamp at light + offset

inline constexpr double kMinLampOffset = 0.15;   // m: a light whose lamps are nearer together than this has one view
// A lamp is off the light's axis by the offset, so a point at the edge of the light's cone, near the car, is outside the cone of the lamp's own view: the lamp's view is
// widened to hold the light's whole cone from this distance on. (A point nearer than that, at the very edge of the cone, where the light is faint, is outside one of the
// two views and counts as lit by it: the mean then takes half of the other's shadow.)
inline constexpr double kLampCoverMeters = 6.0;

// The outer half angle (radians) a lamp's view needs, `offset` metres off the light's axis, to hold the light's cone (`outer_half`) from kLampCoverMeters on.
inline double LampOuterHalfAngle(double outer_half, double offset) {
  return std::atan2(kLampCoverMeters * std::sin(outer_half) + std::fabs(offset), kLampCoverMeters * std::cos(outer_half));
}

inline double LampOffsetLength(const Light& light) {
  return std::sqrt(light.lamp_offset[0] * light.lamp_offset[0] + light.lamp_offset[1] * light.lamp_offset[1] + light.lamp_offset[2] * light.lamp_offset[2]);
}

// One view to make: the lights it covers (FitView's input) and whose view it is.
struct ViewSlot {
  std::array<Light, 4> lights{};  // a lamp's view: one light at the lamp, its cone widened; a car's view: the car's lights as the rig made them
  Group group;                    // its members index `lights`
  uint32_t car = 0;               // the car's place among the picked cars
  LampSide side = LampSide::kWhole;
};

struct ViewPlan {
  std::array<ViewSlot, kMaxViews> views{};
  uint32_t count = 0;
  uint32_t cars = 0;                            // cars that have views
  std::array<uint32_t, kMaxCars> first_view{};  // each car's first view and how many it has: the views of a car are next to each other and come and go together
  std::array<uint32_t, kMaxCars> view_count{};
};

// The views for the picked cars (`cars`, from ViewPicker::Pick: the player's first), at most `max_views` (MaxViews): a car that does not fit has none, and a lamp car that
// would take the last place alone gets one view for the car instead (the two views of a car are never split). `lamp_cars` is LampCars(level, ...).
inline void PlanViews(const Light* lights, const Group* cars, uint32_t car_count, uint32_t lamp_cars, uint32_t max_views, ViewPlan* out) {
  *out = ViewPlan{};
  max_views = std::min(max_views, kMaxViews);
  for (uint32_t c = 0; c < car_count && c < kMaxCars; ++c) {
    const Group& car = cars[c];
    if (car.count == 0 || out->count >= max_views) {
      break;
    }
    const bool lamps = c < lamp_cars && car.count == 1 && LampOffsetLength(lights[car.members[0]]) >= kMinLampOffset && out->count + 2 <= max_views;
    out->first_view[c] = out->count;
    if (lamps) {
      const Light& light = lights[car.members[0]];
      const double offset = LampOffsetLength(light);
      const double widened = LampOuterHalfAngle(ShadowHalf(light), offset);
      for (uint32_t k = 0; k < 2; ++k) {
        ViewSlot& v = out->views[out->count++];
        v.car = c;
        v.side = k == 0 ? LampSide::kA : LampSide::kB;
        Light lamp = light;
        const double sign = k == 0 ? -1.0 : 1.0;
        for (int m = 0; m < 3; ++m) {
          lamp.pos[m] = light.pos[m] + sign * light.lamp_offset[m];
          lamp.lamp_offset[m] = 0.0;
        }
        lamp.cos_outer = std::cos(widened);
        lamp.shadow_half = widened;
        v.lights[0] = lamp;
        v.group = Group{};
        v.group.members[0] = 0;
        v.group.count = 1;
        v.group.car_key = car.car_key;
        v.group.is_player = car.is_player;
        for (int m = 0; m < 3; ++m) {
          v.group.eye[m] = lamp.pos[m];
        }
      }
    } else {
      ViewSlot& v = out->views[out->count++];
      v.car = c;
      v.side = LampSide::kWhole;
      v.group = car;
      v.group.count = std::min<uint32_t>(car.count, 4);
      for (uint32_t m = 0; m < v.group.count; ++m) {
        v.lights[m] = lights[car.members[m]];
        v.group.members[m] = m;
      }
    }
    out->view_count[c] = out->count - out->first_view[c];
    out->cars = c + 1;
  }
}

// The one view that holds all the views of a car: the frame's cone cull tests a draw against it once for a car, not once for each lamp (a car's two views share the
// casters the collection keeps, and the draws they keep for themselves are told apart at the replay, against each view's own cone). False when the car has no view.
inline bool FitCar(const ViewPlan& plan, uint32_t car, ViewSpec* out) {
  if (car >= plan.cars || plan.view_count[car] == 0) {
    return false;
  }
  std::array<Light, 4> lights{};
  Group group;
  group.car_key = plan.views[plan.first_view[car]].group.car_key;
  group.is_player = plan.views[plan.first_view[car]].group.is_player;
  uint32_t n = 0;
  for (uint32_t k = 0; k < plan.view_count[car]; ++k) {
    const ViewSlot& v = plan.views[plan.first_view[car] + k];
    for (uint32_t m = 0; m < v.group.count && n < 4; ++m) {
      lights[n] = v.lights[v.group.members[m]];
      group.members[n] = n;
      ++n;
    }
  }
  group.count = n;
  for (int k = 0; k < 3; ++k) {
    double sum = 0.0;
    for (uint32_t m = 0; m < n; ++m) {
      sum += lights[m].pos[k];
    }
    group.eye[k] = n ? sum / double(n) : 0.0;
  }
  return FitView(lights.data(), group, out);
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

// How far a draw's geometry may reach from its place (the origin of its world matrix), for the cone test below. A draw of a vehicle is a car's part: its
// root is the car's, the longest (a bus, a truck with a trailer) reaches about this far. Any other draw has no bounds here (the guest's vertex
// buffers are not read), so it gets a margin that holds a building or a stretch of road: a thing larger than that and mostly outside the view is
// left out wrongly (the debug setting mcla_gpu_headlight_shadow_cone 0 turns the test off to look for such a shadow).
inline constexpr double kVehicleReach = 7.0;
inline constexpr double kWorldReach = 40.0;
constexpr double CasterReach(bool vehicle) { return vehicle ? kVehicleReach : kWorldReach; }

// Whether a sphere (a draw's place and its reach) can touch the view's frustum: nearer than the near plane or farther than the far one, or outside
// the cone that holds the square frustum (half the diagonal's angle), is out. Conservative: a sphere that reaches the plane of the eye, where the cone
// is no test, stays. The planes are measured along the axis from the eye (spec.near_m already holds the lamps' offset behind the lamps).
inline bool SphereInView(const ViewSpec& spec, const double centre[3], double reach) {
  const double offset[3] = {centre[0] - spec.eye[0], centre[1] - spec.eye[1], centre[2] - spec.eye[2]};
  const double along = detail::Dot(offset, spec.dir);
  if (along + reach < spec.near_m || along - reach > spec.far_m) {
    return false;
  }
  if (along <= reach) {
    return true;
  }
  const double tan_diagonal = std::tan(spec.fov * 0.5) * 1.4142135623730951;
  const double cos_diagonal = 1.0 / std::sqrt(1.0 + tan_diagonal * tan_diagonal);
  const double sideways = std::sqrt(std::max(0.0, detail::Dot(offset, offset) - along * along));
  return (sideways - along * tan_diagonal) * cos_diagonal <= reach;
}

// The cone cull at collection tests a draw against the car's one union view (FitCar), whose apex is behind the lamps (lamps_m): a sphere that reaches the plane of a lamp is
// "in" for that lamp's own view by its early-out, and for the union only when it reaches `lamps_m` farther. The reach is raised by that much so that a draw either lamp's view
// keeps is never dropped by the union (the test is conservative: it may keep a draw no view needs, never drop one a view does).
inline bool SphereInCarView(const ViewSpec& car_spec, const double centre[3], double reach) { return SphereInView(car_spec, centre, reach + car_spec.lamps_m); }

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

// The tile as the published view carries it (local_shadow_types.h atlas_rect): u0, v0, u1, v1, from UvRect's offset and size.
inline void PublishedRect(const float uv_rect[4], float out[4]) {
  out[0] = uv_rect[0];
  out[1] = uv_rect[1];
  out[2] = uv_rect[0] + uv_rect[2];
  out[3] = uv_rect[1] + uv_rect[3];
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

// The constant bias the reader subtracts from a receiver's depth (z/w units). It only has to cover the float error of the two depths that are compared (the replay's vertices
// and the lit pixel's own: about 1e-5 at world coordinates near 1000, both transformed by a matrix that carries the translation), because the lookup compares at the RECEIVER'S
// PLANE (light_shadow_filter.h: the depth the receiver's own surface has at each texel it reads, so a road seen at 4 degrees needs no bias for its own slope). For the nearest
// receivers (3 m) it is a fraction of a millimetre, 20 m out about 4 cm and 40 m out 15 cm.
inline constexpr float kDepthBias = 2e-4f;
// The map's own slope scaled bias (the pipeline's: SlopeScaledDepthBias times the largest depth change over one pixel of the tile, in z/w). The first versions made it 2 because
// the lookup read a 3 x 3 texel box at one depth, so a surface at a grazing angle needed a margin for two texels of its own slope (a road at 4 degrees from a lamp 0.7 m up: about
// 1 m of depth at 10 m); that margin pushed every grazing blocker (a kerb's top, a sill, a car's roof) away from the lamps and left a bollard's or a pole's thin shadow with nothing
// to start from. The plane compare replaces it: 1 keeps a small margin for the surfaces that are not planes (a car's curved panels) and for the rasteriser's own rounding.
inline constexpr float kSlopeScaledDepthBias = 1.0f;

// ---------------------------------------------------------------------------
// how big a texel is in the world
// ---------------------------------------------------------------------------

// The size, in metres, of one texel of a view's tile at `distance` metres along its axis (the view is square): 2 d tan(fov / 2) / tile. The levels' tile sizes are chosen by it.
inline double TexelMeters(const ViewSpec& spec, uint32_t tile, double distance) {
  return tile == 0 ? 0.0 : 2.0 * distance * std::tan(spec.fov * 0.5) / double(tile);
}

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
  kOutOfView,    // its place is outside the view's cone and reach (SphereInView with the draw's margin): nothing of it can be in the map
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
    case Skip::kOutOfView: return "outside the lamps' view";
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
  bool vehicle = false;           // the draw writes a vehicle's stencil ID (WritesVehicleId)
  uint64_t vs_key = 0;
};

// Whether a draw writes a vehicle's stencil ID: the IDs 1 to 23 (vehicle_motion.h StencilIdsWritten, bit k = ID k). Bit 0 is ID 0, which the WORLD's own draws write
// (every opaque draw of the city: ground, buildings, props): a test of `!= 0` made every world draw a "vehicle" (the headlights run of 2026-11-20: 14000 maps, opaque 0,
// alpha tested 0, vehicle 420, the atlas flat dark) with a car's 7 m reach for the cone cull and the own-car skip near the lamps.
constexpr bool WritesVehicleId(uint32_t stencil_ids) { return (stencil_ids & ~1u) != 0; }

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

// The faces a CASTER is drawn with. The world keeps the policy's (faces turned from the light: a ground, a wall or a facade that faces the lamps casts no
// shadow on itself); a VEHICLE's part is drawn on both faces under the default policy. A car's body is an open shell of panels that face outwards, and from a
// lamp as low as a car's own the shadow on the road behind it is made by the panels the lamp SEES (the bumper, the sills, the front of the underbody), which a
// "faces turned from the light" pass throws away (the owner's picture of 2026-11-20: dark stripes where the tyres are and the road between them lit, behind a car
// that should shadow its whole width). Both faces keep the nearest, which is the face the lamp sees.
inline Cull ResolveCasterCull(int policy, Cull game, bool vehicle) {
  if (vehicle && policy == 0) {
    return Cull::kNone;
  }
  return ResolveCull(policy, game);
}

// ---------------------------------------------------------------------------
// alpha to mask in the atlas
// ---------------------------------------------------------------------------

// The game's alpha tested colour draws (foliage, fences, the LOD cross-fades of the city's props: alpha written by the shader, not 1) end with the Xbox 360's alpha to mask: the
// alpha is turned into a coverage by a 2 x 2 ordered dither on the PIXEL's parity (shared constants word 540, shader_recompiler.cpp computeAlphaToMask: offsets 3, 1, 0, 2 of the
// game's colour passes with MSAA, one of four thresholds 0.625 / 0.875 / 1.0 / 0.75 at 2 samples). In the main view that pattern is one pixel of a 720p picture resolved by the MSAA;
// replayed into the atlas it is one TEXEL: a prop that is fading by distance, and any cut-out with a soft edge, casts a shadow with one texel in two missing, a stipple the lookup
// cannot tell from a real dither (HL-SHADOW-QUALITY, 2026-11-21: the "dither-y" shadows of the world's objects). The atlas has ONE sample, so the replay hands those draws the solid
// offsets (2, 2, 2, 2: coverage when alpha >= 0.5, a plain cut-out that the lookup filters like any edge): the same pixel shader, only the constant differs.
inline constexpr uint32_t kAlphaToMaskEnable = 0x100u;                                 // bit 8 of the word: alpha to mask is on for the draw
inline constexpr uint32_t kAtlasAlphaToMask = 0xAAu | kAlphaToMaskEnable;               // offsets 2 2 2 2, samples log2 0 (bits 9 and up)
constexpr uint32_t AtlasAlphaToMask(uint32_t game_word) { return (game_word & kAlphaToMaskEnable) != 0 ? kAtlasAlphaToMask : 0u; }

// ---------------------------------------------------------------------------
// the ground skirt
// ---------------------------------------------------------------------------

// A car has ground clearance: its underbody floats 0.1 to 0.2 m over the road, and the road behind it is lit through that gap by a lamp that is low enough (a
// ray that enters the gap under the bumper and leaves under the rear lip). Real light does that, and in the picture it reads as a few thin stripes where the
// tyres and the exhaust block it, not as the shadow of a car. So each vehicle part is drawn a second time into the same view, `kSkirtDrop` lower, which puts its
// underbody on the road and closes the gap, and `kSkirtPush` farther along the light so that the copy sits BEHIND the car's own surfaces for the lamp (a surface
// that faces the lamp is nearer than its copy, so the car is never shadowed by its own skirt: the copy of a sloped hood or windscreen, lowered, would come out in
// front of it otherwise). The copy goes below the road by the difference between its drop and the car's clearance: a receiver is on the road, so nothing under it
// is ever between a lamp and a receiver.
inline constexpr double kSkirtDrop = 0.22;  // metres
inline constexpr double kSkirtPush = 0.60;  // metres along the way the light travels

// The way the light travels from a view's eye to a vehicle part's place (unit): the skirt moves along the part's own ray, not along the view's axis, so that a car at
// the edge of a wide view, which the lamp lights from an angle, gets its copy behind its own surfaces too. `axis` stands in when the part is at the eye.
inline void SkirtDirection(const double eye[3], const double origin[3], const double axis[3], double out[3]) {
  double d[3] = {origin[0] - eye[0], origin[1] - eye[1], origin[2] - eye[2]};
  if (!detail::Normalize(d)) {
    d[0] = axis[0];
    d[1] = axis[1];
    d[2] = axis[2];
  }
  out[0] = d[0];
  out[1] = d[1];
  out[2] = d[2];
}

// The world-space shift of the skirt's copy for light that travels along `dir` (unit).
inline void SkirtShift(const double dir[3], double out[3]) {
  out[0] = dir[0] * kSkirtPush;
  out[1] = dir[1] * kSkirtPush - kSkirtDrop;
  out[2] = dir[2] * kSkirtPush;
}

// ReplayMatrix of the copy: VP_main^-1 x translate(shift) x VP_light, so that c8-c11' = gWorld x translate(shift) x VP_light.
inline bool SkirtReplayMatrix(const Camera& camera, const tp::Mat4& light_clip, const double shift[3], tp::Mat4* out) {
  if (!camera.valid) {
    return false;
  }
  tp::Mat4 move = tp::Identity();
  for (int k = 0; k < 3; ++k) {
    move.m[3][k] = shift[k];
  }
  *out = tp::Mul(tp::Mul(camera.view_projection_inverse, move), light_clip);
  return tp::Finite(*out);
}

// A pipeline of the replay: the game's vertex shader (and its own pixel shader, for the alpha tested draws, which the driver cuts down to the
// alpha test: there is no colour target), the layout of the draw's vertex declaration, the topology, the faces drawn.
//
// THE KEY IS COMPARED AND HASHED FIELD BY FIELD, never as bytes. The struct has 4 bytes of tail padding (36 bytes of fields in 40; the two uint64
// make it 8-aligned) that a key built on the stack never writes: a byte compare saw the stack's old contents, so the same shape of draw was
// a new key now and then, and the replay listed 98 000 to 543 000 "different" pipelines for the background builder in a minute of a night
// (HEADLIGHTS runs of 2026-11-20: hs-ultra 98 759, mid hs-high 472 713, against 210 to 300 on the runs that happened to have zeros there).
struct PipeKey {
  uint64_t vs = 0;
  uint64_t ps = 0;  // 0: no pixel shader
  uint32_t vs_variant = 0;
  uint32_t ps_variant = 0;
  uint32_t layout = 0;
  uint32_t topology = 0;
  uint8_t cull = 0;
  uint8_t front_ccw = 0;
  bool operator==(const PipeKey& o) const {
    return vs == o.vs && ps == o.ps && vs_variant == o.vs_variant && ps_variant == o.ps_variant && layout == o.layout && topology == o.topology &&
           cull == o.cull && front_ccw == o.front_ccw;
  }
};

// A safety net, not a budget: a night street has 150 to 500 shapes of draw, a whole drive a few thousand. Past this the replay stops listing new ones (a draw of a
// shape never seen is left out of the maps, counted as "pipeline pending or failed", and the log says so once), so that a key that stops repeating can never
// feed the background builder tens of thousands of pipelines again.
inline constexpr uint32_t kMaxPipelines = 8192;
inline constexpr uint32_t kNoPipeline = 0xFFFFFFFFu;

struct PipeKeyHash {
  size_t operator()(const PipeKey& k) const {
    // The fields only (no padding byte is read): FNV-1a over 64-bit words, each field widened.
    const uint64_t words[5] = {k.vs, k.ps, (uint64_t(k.vs_variant) << 32) | k.ps_variant, (uint64_t(k.layout) << 32) | k.topology,
                               (uint64_t(k.cull) << 8) | k.front_ccw};
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t w : words) {
      h ^= w;
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

// ---------------------------------------------------------------------------
// the vehicles' census
// ---------------------------------------------------------------------------

// What became of the vehicle draws of a frame, by vertex shader identity: a car is some 200 draws of some 20 shaders (perf/scripts/vs_identities.py names the
// identities), and the logs of 2026-11-20 only said "vehicle 314" for two cars, which could not tell a car whose body is in the map from a car that is only
// tyres. `Note` is called once for each vehicle draw with the verdict it got at collection (kNone: collected).
struct VehicleCensus {
  static constexpr uint32_t kRows = 32;
  struct Row {
    uint64_t vs = 0;
    uint32_t counts[size_t(Skip::kCount)] = {};
  };
  Row rows[kRows];
  uint32_t used = 0;
  uint32_t overflow = 0;  // draws of a shader after the 32nd
  uint32_t draws = 0;
  void Clear() { *this = VehicleCensus{}; }
  void Note(uint64_t vs, Skip verdict) {
    ++draws;
    for (uint32_t i = 0; i < used; ++i) {
      if (rows[i].vs == vs) {
        ++rows[i].counts[size_t(verdict)];
        return;
      }
    }
    if (used == kRows) {
      ++overflow;
      return;
    }
    rows[used].vs = vs;
    ++rows[used].counts[size_t(verdict)];
    ++used;
  }
};

// " 23FB35FC659D4A67: 14 drawn; 3B4E4762B73D5239: 6 not opaque; ..." (the log's text; "drawn" is the collected ones, the views then drop the lamps' own car).
inline std::string DescribeVehicleCensus(const VehicleCensus& census) {
  std::string text;
  for (uint32_t i = 0; i < census.used; ++i) {
    char id[24];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(census.rows[i].vs));
    text += " ";
    text += id;
    text += ":";
    bool first = true;
    for (size_t k = 0; k < size_t(Skip::kCount); ++k) {
      if (census.rows[i].counts[k] != 0) {
        text += first ? " " : ", ";
        text += std::to_string(census.rows[i].counts[k]) + " " + SkipName(Skip(k));
        first = false;
      }
    }
    text += ";";
  }
  if (census.overflow != 0) {
    text += " (and " + std::to_string(census.overflow) + " draws of other shaders)";
  }
  return text;
}

}  // namespace mcla::gpu::local_shadow
