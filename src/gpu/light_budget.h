#pragma once
// ===========================================================================
// MCLA GPU renderer -- the one budget table of the lights: what each level of the lighting settings lets the pickers choose
// (perf/LIGHT-UNIFY.md section 3.4 and step U3, package LU-3)
// ===========================================================================
// Three settings say how many lights there are to pick: HEADLIGHTS (off | on), LIGHT SHADOWS (off | medium | high | ultra: how many cars have shadow views and for how many of those every
// headlamp has a view of its own) and VOLUMETRIC LIGHT (off | medium | high | ultra: how many street lamps and headlights the froxel grid reads). Until now each consumer held its own
// numbers: the rig's `kMaxLights` (4), local_shadow_rules.h's level table (the three level shape), volumetrics_types.h's GridFor. This header is where they are written once. The
// consumers read them from here: local_shadow_rules.h's `kThreeLevels`, `kTwoLevels`, `MaxCars`, `LampCars` and `MaxViews` are this table, and the volume's grid keeps its own sizes (its
// lamp and headlight counts are checked against the row below by light_pick_test.cpp).
//
// Pure: plain numbers, no renderer, no guest. light_pick_test.cpp checks it.
//
//   Level            off | medium | high | ultra: the words of the two settings that have levels (the same four in both).
//   ShadowRow        one row of the shadow level table: the texels of a view's side, the cars it shadows (the player's first, then the nearest others) and, of those, how many (in the same
//                    order) have a view for each lamp; the other cars have one view for the car.
//   BudgetFor        everything a picker needs for a frame, from the three settings.
//
// The street lamps' shadows (LAMP-SHADOWS, feat/lamp-shadows, not landed) are RESERVED: `lamp_views` and `lamp_views_spare` are 0 here and the rows are where their numbers go when the
// lamp picker moves into the manager (LU-5): 2 (+1) at medium, 4 (+1) at high, 8 (+2) at ultra, 512 texel tiles, as lamp_shadow_rules.h of that branch has them.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

namespace mcla::gpu::light_budget {

enum class Level : uint8_t { kOff, kMedium, kHigh, kUltra };

// The rig lights four cars at the most (headlight_rig_rules.h kMaxLights is this number: a test holds them equal): the lit shaders' block has four lights.
inline constexpr uint32_t kMaxHeadlights = 4;
// The shadow replay holds this many views in a frame (local_shadow_types.h kMaxShadowViews: a test holds them equal).
inline constexpr uint32_t kMaxShadowViews = 8;
// The cars that can have views: the rig lights four.
inline constexpr uint32_t kMaxShadowCars = 4;

// One row of the shadow level table.
struct ShadowRow {
  Level level = Level::kOff;
  std::string_view word;
  uint32_t tile = 0;       // texels of a view's side
  uint32_t cars = 0;       // cars that have views (the player's first, then the nearest others)
  uint32_t lamp_cars = 0;  // of those, how many (in the same order) have a view for each headlamp
};

// THE TWO LEVEL SHAPE (the first plan of 2026-11-20, kept for the tests and as a way back): both levels have a view for every lamp of every car they shadow. High: the player's car and the
// nearest other car, 4 views of 512 (8 MiB); ultra: the player's car and the two nearest, 6 views of 1024 (48 MiB).
inline constexpr ShadowRow kTwoLevels[] = {
    {Level::kHigh, "high", 512, 2, 2},
    {Level::kUltra, "ultra", 1024, 3, 3},
};
// THE THREE LEVEL SHAPE (IN USE since 2026-11-20: the cost runs of perf/out/HL-PER-LAMP.txt showed a view for each headlamp adds +0.05 to +0.15 ms of graphics card time and +0.04 to
// +0.16 ms of render thread time on an RTX 5080, and a weaker PC pays several times that, so the lamps' views are for the higher levels only): medium is the first version's high (one
// view for each of two cars, 512, 2 views, 4 MiB), high gives the player's car a view for each lamp and the nearest other car one view (3 views of 512, 6 MiB), ultra every shadowed car a
// view for each lamp (6 views of 1024, 48 MiB). Auto: Mid medium, High ultra, Low off (gpu_class.h).
inline constexpr ShadowRow kThreeLevels[] = {
    {Level::kMedium, "medium", 512, 2, 0},
    {Level::kHigh, "high", 512, 2, 1},
    {Level::kUltra, "ultra", 1024, 3, 3},
};
// THE ONE DATA CHANGE between the shapes (local_shadow_rules.h's kThreeLevelShape picks the table in use).
using Table = std::span<const ShadowRow>;

// The row a level has in a table: its own, else (a level the shape does not have: `medium` in the two level shape) the next one up, else the greatest below. Null for off.
constexpr const ShadowRow* ShadowRowOf(Level level, Table table) {
  if (level == Level::kOff || table.empty()) {
    return nullptr;
  }
  const ShadowRow* best = nullptr;
  for (const ShadowRow& row : table) {
    if (row.level == level) {
      return &row;
    }
    if (row.level > level && (!best || row.level < best->level)) {
      best = &row;
    }
  }
  return best ? best : &table[table.size() - 1];
}

// The volume's grid: the lamps and the headlights it reads at a level (volumetrics_types.h GridFor: a test holds these equal).
struct VolumeRow {
  Level level = Level::kOff;
  uint32_t lamps = 0;
  uint32_t headlights = 0;
};
inline constexpr VolumeRow kVolume[] = {
    {Level::kMedium, 12, 2},
    {Level::kHigh, 32, 4},
    {Level::kUltra, 48, 4},
};
constexpr const VolumeRow* VolumeRowOf(Level level) {
  for (const VolumeRow& row : kVolume) {
    if (row.level == level) {
      return &row;
    }
  }
  return nullptr;
}

// What the pickers choose from, for one frame.
struct LightBudget {
  // the headlight block of the lit shaders (the forward headlight stage; the deferred pass later): the lights it holds, the player's first. 0 with HEADLIGHTS off.
  uint32_t headlights = 0;
  // the shadow replay: the cars that have views, of those the ones with a view for each lamp, the most views and the tile's side.
  uint32_t shadow_cars = 0;
  uint32_t shadow_lamp_cars = 0;
  uint32_t shadow_views = 0;
  uint32_t shadow_tile = 0;
  // the volume: the street lamps and headlights its grid reads (the volume still picks its own lamps: they move here with LU-5).
  uint32_t volume_lamps = 0;
  uint32_t volume_headlights = 0;
  // RESERVED for the street lamps' shadows (see the top): views wanted, the spare slots for a lamp whose shadow fades out, the tile's side. All 0 until the lamp picker is here.
  uint32_t lamp_views = 0;
  uint32_t lamp_views_spare = 0;
  uint32_t lamp_tile = 0;

  friend constexpr bool operator==(const LightBudget&, const LightBudget&) = default;
};

// The cars a level shadows (the player's car first). The debug setting does not change it: it is the views of a car (below).
constexpr uint32_t ShadowCars(Level level, Table table) {
  const ShadowRow* row = ShadowRowOf(level, table);
  return row ? std::min(row->cars, kMaxShadowCars) : 0u;
}
constexpr uint32_t ShadowTile(Level level, Table table) {
  const ShadowRow* row = ShadowRowOf(level, table);
  return row ? row->tile : 0u;
}
// How many cars (the first ones, the player's first) have a view for each lamp. `views_per_car` is the debug setting mcla_gpu_headlight_shadow_views: 1 gives every car one view (the first
// version's), 2 gives every car a view for each lamp, 0 takes the level's own.
constexpr uint32_t ShadowLampCars(Level level, int views_per_car, Table table) {
  const ShadowRow* row = ShadowRowOf(level, table);
  if (!row) {
    return 0;
  }
  const uint32_t cars = ShadowCars(level, table);
  if (views_per_car >= 2) {
    return cars;
  }
  if (views_per_car == 1) {
    return 0;
  }
  return std::min(row->lamp_cars, cars);
}
// How many views a level draws at the most: a view for each shadowed car and a second for each car that has its lamps apart.
constexpr uint32_t ShadowViews(Level level, int views_per_car, Table table) {
  return std::min(ShadowCars(level, table) + ShadowLampCars(level, views_per_car, table), kMaxShadowViews);
}

// The budget of a frame from the three settings: HEADLIGHTS (on or off), LIGHT SHADOWS (`shadows`, the level it resolved to; `views_per_car` the debug setting) and VOLUMETRIC LIGHT
// (`volume`, the level it resolved to), with the table of the shape in use.
constexpr LightBudget BudgetFor(bool headlights_on, Level shadows, int views_per_car, Level volume, Table table) {
  LightBudget b;
  b.headlights = headlights_on ? kMaxHeadlights : 0u;
  b.shadow_cars = ShadowCars(shadows, table);
  b.shadow_lamp_cars = ShadowLampCars(shadows, views_per_car, table);
  b.shadow_views = ShadowViews(shadows, views_per_car, table);
  b.shadow_tile = ShadowTile(shadows, table);
  if (const VolumeRow* row = VolumeRowOf(volume)) {
    b.volume_lamps = row->lamps;
    b.volume_headlights = row->headlights;
  }
  return b;
}

}  // namespace mcla::gpu::light_budget
