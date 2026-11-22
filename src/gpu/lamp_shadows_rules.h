#pragma once
// ===========================================================================
// MCLA GPU renderer -- the street lamps' shadows, second try: the rules of the PROVIDER that needs no renderer (perf/LAMP-SHADOWS-2.md, package LAMP-SHADOWS-4)
// ===========================================================================
// WHY AGAIN. The first try (feat/lamp-shadows, parked 2026-11-21) worked (the lit shaders' lamp stage multiplied each grid lamp by a visibility) and was nearly invisible: the game's own lamps
// light a lot weakly next to the ambient, and a shadow can only darken what the lamp adds (under 1 per cent of the picture, +0.2 to +0.6 ms). Since then the deferred light pass exists
// (local_lights.h: GB-H1..H3), the shadow atlas is one object with providers (shadow_atlas.h: LU-5) and the lamp table is exact (CITY-RT G2b). So the lamps' shadows are re-made as a PROVIDER
// of the atlas read by the light pass, with the lamp's light under the pass's control (its strength, its law, its edge) instead of inside the game's shaders:
//
//   WHICH LAMPS   the lamps of the game's per-frame list (vol_lamp_list.h: the hook, mean 64 a night frame, none by day; positions as the game's unpacked light has them: the exact ones) or,
//                 with no list (the hook is not in the build, a toolkit scene), the static lamp table's selection (CITY-RT G2b: citypack/lamp_merge.h SelectLamps; FromTableLamp adapts it).
//                 The nearest N by what they add to the view (Picker: light x the share of the lamp's SHADOW sphere in the camera's frustum / (d^2 + I)), N = the level's row.
//   WHICH VIEW    ONE downward spot frustum a lamp (a street lamp points down: cone 125 degrees), never a cube: FitLampView = local_shadow_rules.h FitView on a group of the one lamp.
//   LAYERS        STATIC (the world that does not move: drawn once, added to as the main view shows more, kept) and LIVE (cars and people: every frame). PlanLampPass decides what each pass draws.
//   ATLAS         two pools (live, static) of sticky tiles: a lamp keeps its tile while it is wanted (shadow_atlas_rules.h StickySlots-like: this picker's tracks). Budgets: shadow_atlas_rules.h
//                 kLampRows (2 +1 / 4 +1 / 8 +2 lamps of 512 texels at medium / high / ultra).
//   READER        the light pass (hlsl/local_light_ps.hlsl) through the ONE lookup over a table of its own (lamp_shadow_frame.h, hlsl/lamp_shadow_lookup.hlsli). Two modes (LampMode):
//                   kSubtract  the game's grid still lights the lamp; the pass computes the same term for the lamp the grid has (its x and z through the game's half float: GridPosition) and
//                              REMOVES the shadowed share, term x (1 - visibility). Works for every lamp the game lights, cars and people included (the forward stage never reached them).
//                   kOwn       the lamp is not in the grid (the lamp A/B's mod lamps today: mod_lights.cpp builds the grid with them off): the pass ADDS term x visibility.
//
// THE EXACT POSITION. The first try matched the shaders' four grid lamps by x and z and had to reproduce the grid texture's HALF FLOAT truncation (up to 2 m at the city's edges) in the block.
// The pass needs no match: it takes the lamp's own position for the shadow (the view is made from it), and for the SUBTRACTED term the grid's own x and z so that exactly what the grid added
// is taken away. GameHalfBits / GamePosition are that conversion (guest sub_8213E850).
//
// Pure: plain numbers, no renderer, no guest, no SDK. lamp_shadows_test.cpp checks it. Namespace mcla::gpu::lamp_shadows.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lamp_shadow_frame.h"
#include "light_budget.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "light_pick.h"
#include "local_shadow_rules.h"
#include "shadow_atlas_rules.h"
#include "temporal_math.h"
#include "vol_lamp_list.h"

namespace mcla::gpu::lamp_shadows {

namespace lg = ::larecomp::gpu;
namespace lb = ::mcla::gpu::light_budget;
namespace ls = ::mcla::gpu::local_shadow;
namespace sa = ::mcla::gpu::shadow_atlas;
namespace tp = ::mcla::gpu::temporal;

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

inline constexpr uint32_t kTile = 512;                       // texels a lamp's tile (a 114 degree view over 512 texels is about 5.4 cm a texel straight below a 9 m lamp; 8 cm with the whole cone)
// A lamp's view holds its cone up to this half angle only (HL-SHADOW-QUALITY's rule for the headlights' beams, local_shadow_rules.h kShadowConeCapDegrees): a street lamp's term at 55 degrees from
// its axis is under 5 per cent of what it is straight below (the cone's smoothstep is 0.25 there, cos^3 of the angle 0.19: ground that far out is 13 m from a 9 m lamp), so the last 7.5 degrees
// of the cone would cost every texel a quarter of its size for a share nobody sees. A receiver outside the view's rectangle is lit and the lookup fades the shadow out over the last 5 per cent
// of the tile (kHlBorderFade), so the view's edge is no line in the picture.
inline constexpr double kConeCapDegrees = 55.0;
inline constexpr uint32_t kMaxSlots = lg::kMaxLampSlots;     // wanted + spare at the largest level
inline constexpr double kMaxPickMetres = 90.0;               // a lamp farther than this from the eye is no candidate (its pool is out of any view)
inline constexpr double kKeepBonus = light_pick::kLampKeepBonus;                            // a lamp that holds a tile scores this much more than a rival that does not
inline constexpr float kFadeSeconds = float(light_pick::kLampFadeSeconds);                       // a lamp's shadow strength goes 0 to 1 (or back) in this long
inline constexpr double kReachShare = 8.0;                   // the shadow reaches where the lamp's term is an eighth: sqrt(8 I), kept between kMinReach and kMaxReach
inline constexpr double kMinReach = 14.0;
inline constexpr double kMaxReach = 30.0;
inline constexpr uint32_t kMaxDrawsPerLampView = 768;        // the most draws a lamp's view takes in a frame (the farthest are left out)
inline constexpr uint32_t kStaticAddsPerFrame = 384;         // the most static draws a view adds in one frame (a lamp that has just taken its tile fills in over a few frames)
inline constexpr uint64_t kRefreshFrames = 7200;             // a lamp's static layer is built again this often (two minutes at 60 FPS), staggered by tile: the safety net, not the rule
inline constexpr uint64_t kRefreshStagger = 211;
inline constexpr uint32_t kDrawnSlots = 4096;                // the static draws a layer remembers
// How far a mover's origin may lie outside a lamp's frustum and still cast into it: a car's own 7 m (a person is a metre across); the world's origins keep their 40 m margin.
inline constexpr double kDynamicReach = ls::kVehicleReach;
inline constexpr float kPassRangeMargin = 2.0f;              // metres added to the shadow reach for the SUBTRACT mode's light range (the edge fade ends at the reach itself)

static_assert(kMaxSlots == 10 && lg::kMaxLampEntries == 20, "ultra: 8 wanted + 2 spare lamps, a static and a live entry each");

// ---------------------------------------------------------------------------
// the lamp, and where it comes from
// ---------------------------------------------------------------------------

enum class Origin : uint8_t { kLiveList = 0, kTable = 1 };
// How the light pass uses the lamp's shadow (mcla_gpu_lamp_shadows_mode): kSubtract takes the shadowed share away from what the game's grid lit, kOwn adds the lamp's light (the grid has it off).
enum class LampMode : uint8_t { kSubtract = 0, kOwn = 1 };
inline LampMode ModeOf(int setting) { return setting == 1 ? LampMode::kOwn : LampMode::kSubtract; }

struct Lamp {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float colour[3] = {1.0f, 1.0f, 1.0f};   // rgb 0..1 (the intensity is separate)
  float dir[3] = {0.0f, -1.0f, 0.0f};
  float intensity = 0.0f;                 // the game's units: 20 a fill light, 55 to 100 a street lamp
  float cone_deg = 360.0f;
  float spread_deg = 0.0f;
  float scale = 1.0f;                     // the game's own fade (the prop pass's, 0..1); the table's lamps are 1 (the hours decide)
  uint8_t type = 1;                       // 0 omni, 1 spot
  uint8_t on = 1;
  Origin origin = Origin::kLiveList;
  uint64_t id = 0;                        // light_id.h LampId(pos)
};

// A record of the game's per-frame list (vol_lamp_list.h).
inline Lamp FromGameLamp(const vol_lamps::GameLamp& g) {
  Lamp l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = g.pos[i];
    l.colour[i] = g.colour[i];
    l.dir[i] = g.dir[i];
  }
  l.intensity = g.intensity;
  l.cone_deg = g.cone_deg;
  l.spread_deg = g.spread_deg;
  l.scale = g.scale;
  l.type = g.type;
  l.on = g.on;
  l.origin = Origin::kLiveList;
  l.id = lg::light_id::LampId(g.pos);
  return l;
}

// A lamp of the static table (citypack/lamp_table.h Lamp: pos, colour, dir, intensity, cone_deg, spread_deg, spot, on), made `scale` strong (the table has no fade: the caller's hours /
// night factor). The adapter is a template so that this header needs no citypack: any record with those members works (lamp_shadows_test.cpp holds a mock; the real one is checked by
// perf's syntax check against city-rt-g2b).
template <class TableLamp>
inline Lamp FromTableLamp(const TableLamp& t, float scale = 1.0f) {
  Lamp l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = t.pos[i];
    l.colour[i] = t.colour[i];
    l.dir[i] = t.dir[i];
  }
  l.intensity = t.intensity;
  l.cone_deg = t.cone_deg;
  l.spread_deg = t.spread_deg;
  l.scale = scale;
  l.type = uint8_t(t.spot != 0 ? 1 : 0);
  l.on = uint8_t(t.on != 0 ? 1 : 0);
  l.origin = Origin::kTable;
  l.id = lg::light_id::LampId(t.pos);
  return l;
}

// A lamp a shadow view can be made for: a spot (type 1) that is on, has a cone with an axis, an intensity and a fade, with numbers a light can have. An omni light has no axis (one
// frustum cannot hold it) and gets none.
inline bool Usable(const Lamp& l) {
  const auto ok = [](float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; };
  for (int i = 0; i < 3; ++i) {
    if (!ok(l.pos[i]) || !ok(l.colour[i]) || !ok(l.dir[i])) {
      return false;
    }
  }
  if (!ok(l.intensity) || !ok(l.cone_deg) || !ok(l.spread_deg) || !ok(l.scale) || l.type != 1 || l.on == 0 || !(l.intensity > 0.0f) || !(l.scale > 0.0f) || !(l.cone_deg > 1.0f) ||
      !(l.cone_deg < 359.0f)) {
    return false;
  }
  return std::sqrt(double(l.dir[0]) * l.dir[0] + double(l.dir[1]) * l.dir[1] + double(l.dir[2]) * l.dir[2]) > 1e-3;
}

// Where the lamp's SHADOW reaches (not its light): where its term is an eighth, sqrt(8 I): 21 m for a lamp of 55, 28 m for 100, kept between 14 and 30 m (light_laws.h LampShadowReach, the
// first try's measured choice: a twentieth put the whole street into four overlapping views and cost three to five times the estimate).
inline double ShadowReach(float intensity) { return double(lg::light_laws::LampShadowReach(intensity)); }

// ---------------------------------------------------------------------------
// the game's own half float: what the light grid's position texture holds of a lamp (guest sub_8213E850, read in the recompiled code)
// ---------------------------------------------------------------------------

// NaN and a magnitude below the smallest normal half (2^-14) give 0, a larger one is clamped to +-65504, and the rest takes the sign, the exponent (the float's biased exponent + 16, the low
// 6 bits) and the top 10 bits of the mantissa: TRUNCATION toward zero, not rounding. The grid builder (sub_823033F0) writes each lamp's x and z through it into an RGBA16F texel.
inline uint16_t GameHalfBits(float value) {
  if (!(value == value) || std::fabs(value) < 6.103515625e-5f) {
    return 0;
  }
  value = std::clamp(value, -65504.0f, 65504.0f);
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = bits >> 31;
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  const uint32_t mantissa = (bits >> 13) & 0x3FFu;
  return uint16_t((sign << 15) | (((exponent + 16u) & 0x3Fu) << 10) | mantissa);
}
// A half's value (what the GPU's float fetch of the texel returns: exact).
inline float HalfBitsToFloat(uint16_t half) {
  const uint32_t exponent = (half >> 10) & 0x1Fu;
  const uint32_t mantissa = half & 0x3FFu;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa != 0 ? std::nanf("") : HUGE_VALF;
  } else {
    value = std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15);
  }
  return (half & 0x8000u) != 0 ? -value : value;
}
// A coordinate as the game's position texture holds it.
inline float GamePosition(float value) { return HalfBitsToFloat(GameHalfBits(value)); }
// The lamp's place as the GRID has it (x and z through the half float; the height is not in that texture's x and z: CHECK where the shader takes it from): the place the subtracted term is made at.
inline void GridPosition(const float pos[3], float out[3]) {
  out[0] = GamePosition(pos[0]);
  out[1] = pos[1];
  out[2] = GamePosition(pos[2]);
}

// ---------------------------------------------------------------------------
// the level: how many lamps, how big a tile
// ---------------------------------------------------------------------------

// A level's room for lamps: `wanted` shadowed at once, `spare` more tiles that lamps leaving the pick fade out in, `tile` texels square, and whether each tile has a static twin.
struct Budget {
  uint32_t wanted = 0;
  uint32_t spare = 0;
  uint32_t tile = 0;
  bool cache = true;
  constexpr uint32_t tiles() const { return wanted + spare; }
  constexpr bool on() const { return wanted != 0 && tile != 0; }
  friend constexpr bool operator==(const Budget&, const Budget&) = default;
};

// The budget of a LIGHT SHADOWS level (shadow_atlas_rules.h kLampRows). `override_lamps` (mcla_gpu_lamp_shadows_lamps, debug): -1 no lamp views, 0 the level's, n the lamps (at most
// kMaxSlots - spare). The level `off` has none.
constexpr Budget BudgetFor(lb::Level level, int override_lamps, bool cache) {
  if (level == lb::Level::kOff || override_lamps < 0) {
    return Budget{};
  }
  const sa::LampRow* row = sa::LampRowOf(level);
  if (row == nullptr) {
    return Budget{};
  }
  Budget b{row->views, row->spare, row->tile, cache};
  if (override_lamps > 0) {
    b.wanted = std::min<uint32_t>(uint32_t(override_lamps), kMaxSlots - 1);
    b.spare = b.wanted >= 2 ? std::max(1u, b.wanted / 4u) : 0u;
    b.wanted = std::min(b.wanted, kMaxSlots - b.spare);
  }
  return b;
}

// The pools the lamps ask of the atlas: the LIVE tiles (cleared and drawn every record) and, with the cache, the STATIC twins (kept); both sticky (a tile stays with a lamp).
inline void RequestPools(const Budget& b, sa::AtlasSpec* spec) {
  if (!b.on()) {
    return;
  }
  spec->Add({sa::ProviderId::kLamps, sa::PoolRole::kLive, b.tile, b.tiles(), true});
  if (b.cache) {
    spec->Add({sa::ProviderId::kLamps, sa::PoolRole::kStatic, b.tile, b.tiles(), true});
  }
}

// The video memory a budget adds to the atlas (live + static pools, 8 bytes a texel): High 4 + 1 tiles = 10 MiB (20 with the cache), Ultra 10 tiles = 20 (40 with the cache).
inline uint64_t PoolBytes(const Budget& b) {
  return b.on() ? uint64_t(b.tiles()) * b.tile * b.tile * sa::kTexelBytes * (b.cache ? 2u : 1u) : 0ull;
}

// ---------------------------------------------------------------------------
// the view of a lamp
// ---------------------------------------------------------------------------

// The lamp as the replay's light (local_shadow_rules.h Light): its place, its axis, the outer half angle of its cone and the shadow reach. `index` is its place in the frame's table. False
// for a lamp MakeLight refuses.
inline bool MakeReplayLight(const Lamp& l, uint32_t index, ls::Light* out) {
  float cos_outer = 0.0f, cos_inner = 0.0f;
  lg::light_laws::ConeCosines(l.cone_deg, l.spread_deg, &cos_outer, &cos_inner);
  float dir[3] = {l.dir[0], l.dir[1], l.dir[2]};
  const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (!(len > 1e-3f)) {
    dir[0] = 0.0f;
    dir[1] = -1.0f;
    dir[2] = 0.0f;
  }
  if (!ls::MakeLight(l.pos, dir, cos_outer, float(ShadowReach(l.intensity)), uint32_t(l.id & 0xFFFFFFFFu), false, index, out)) {
    return false;
  }
  // MakeLight caps the shadow view at the headlight beam's 34 degrees; a street lamp's cone is wider: its own cap (kConeCapDegrees) held to its cone.
  out->shadow_half = std::min(std::acos(std::clamp(out->cos_outer, -1.0, 1.0)), ls::Rad(kConeCapDegrees));
  return true;
}

// The lamp's view: one spot frustum that holds its cone (local_shadow_rules.h FitView on a group of the one lamp), the eye at the lamp, the near plane kNearMeters (1.5 m) below it so that
// the lamp's own housing and arm cast nothing.
inline bool FitLampView(const ls::Light& light, ls::ViewSpec* out) {
  ls::Group group;
  group.members[0] = 0;
  group.count = 1;
  group.car_key = light.car_key;
  for (int k = 0; k < 3; ++k) {
    group.eye[k] = light.pos[k];
  }
  return ls::FitView(&light, group, out);
}

// ---------------------------------------------------------------------------
// the pick
// ---------------------------------------------------------------------------

// What the pick measures the lamps against: the camera's eye and its world -> clip (the previous frame's: the pick is made when the frame opens).
struct View {
  bool have_eye = false;
  double eye[3] = {};
  bool have_vp = false;
  tp::Mat4 view_projection;  // world -> the game's clip (Camera::view_projection)
};

// The distance of a point in front of (positive) or behind (negative) a plane of the frustum, in metres: the plane is column `a` plus `sign` times column `b` of the world -> clip matrix (b < 0:
// column a alone), whose x, y, z are the plane's normal.
inline double PlaneDistance(const tp::Mat4& m, const double p[3], int a, int b, double sign) {
  double n[3];
  for (int i = 0; i < 3; ++i) {
    n[i] = m.m[i][a] + (b >= 0 ? sign * m.m[i][b] : 0.0);
  }
  const double d0 = m.m[3][a] + (b >= 0 ? sign * m.m[3][b] : 0.0);
  const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (!(length > 1e-12)) {
    return 1.0e9;  // no plane
  }
  return (n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + d0) / length;
}

// How much of a lamp's shadow sphere is in the camera's frustum (its sides and its near plane): 1 when it touches it, falling to 0 a whole radius outside it. A lamp behind the camera still
// lights the road in front of it, as far as `radius` reaches.
inline double FrustumShare(const tp::Mat4& vp, const double p[3], double radius) {
  const double s = std::min({PlaneDistance(vp, p, 0, 3, 1.0), PlaneDistance(vp, p, 3, 0, -1.0), PlaneDistance(vp, p, 1, 3, 1.0), PlaneDistance(vp, p, 3, 1, -1.0),
                             PlaneDistance(vp, p, 2, -1, 0.0)});
  if (s >= 0.0) {
    return 1.0;
  }
  return std::clamp(1.0 + s / std::max(radius, 1e-3), 0.0, 1.0);
}

// What a lamp adds to the view: its light over its distance squared plus its plateau, times how much of its SHADOW sphere is in the frustum (0: nothing; off, too far). The shadow's reach, not the
// volume's: a lamp whose map cannot reach the picture (a pool behind a building, a lamp 40 m past the frame) gets no tile.
inline double Score(const Lamp& l, const View& view) {
  const double peak = std::max({l.colour[0], l.colour[1], l.colour[2]});
  const double light = peak * std::max(l.intensity, 0.0f) * std::clamp(l.scale, 0.0f, 1.0f);
  if (!(light > 0.0)) {
    return 0.0;
  }
  const double p[3] = {l.pos[0], l.pos[1], l.pos[2]};
  double d2 = 0.0;
  if (view.have_eye) {
    for (int i = 0; i < 3; ++i) {
      d2 += (p[i] - view.eye[i]) * (p[i] - view.eye[i]);
    }
    if (d2 > kMaxPickMetres * kMaxPickMetres) {
      return 0.0;
    }
  }
  const double radius = ShadowReach(l.intensity);
  const double factor = view.have_vp ? FrustumShare(view.view_projection, p, radius) : 1.0;
  return light * factor / (d2 + double(l.intensity));
}

struct PickParams {
  Budget budget;
  float dt = 0.0f;     // seconds (the scene clock) since the last pick; 0 on the first
  bool reset = false;  // a cut (the camera jumped, a long gap): every picked lamp starts at full strength
};

// One lamp that holds a tile.
struct Entry {
  uint64_t id = 0;
  Lamp lamp;
  uint32_t tile = 0;      // its tile (0 .. budget.tiles() - 1), the same while it is tracked
  float weight = 0.0f;    // 0..1, the shadow's strength
  bool wanted = false;    // in the budget's lamps (else it is fading out)
  bool fresh = false;     // took its tile in this pick (its static layer is empty)
};

struct PickResult {
  std::array<Entry, kMaxSlots> entries{};
  uint32_t count = 0;
  // for the census
  uint32_t listed = 0;    // the lamps given
  uint32_t usable = 0;    // spots that light something in reach of the view
  uint32_t wanted = 0;
  uint32_t fading = 0;
  uint32_t dropped = 0;   // wanted but no tile was free (a lamp fading out holds it until it is gone)
  double best_score = 0.0;
};

// Picks the lamps that get a tile, frame after frame. One of these lives as long as the feature does; Reset() when the lights go (a day, a menu).
class Picker {
 public:
  void Reset() {
    tracks_.clear();
    started_ = false;
  }
  size_t tracked() const { return tracks_.size(); }

  void Update(const std::vector<Lamp>& list, const View& view, const PickParams& params, PickResult* out) {
    *out = PickResult{};
    out->listed = uint32_t(list.size());
    const uint32_t tiles = std::min(params.budget.tiles(), kMaxSlots);
    if (!params.budget.on() || tiles == 0) {
      tracks_.clear();
      return;
    }
    const uint32_t wanted_cap = std::min(params.budget.wanted, tiles);
    const bool instant = params.reset || !started_ || !(params.dt > 0.0f);
    const float step = instant ? 1.0f : std::min(params.dt, 0.25f) / kFadeSeconds;

    // The candidates: every usable lamp that lights something in view, once per place (the best score of its records).
    struct Candidate {
      uint64_t id;
      Lamp lamp;
      double score;
      double ranked;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(list.size());
    std::unordered_map<uint64_t, size_t> by_id;
    for (const Lamp& l : list) {
      if (!Usable(l)) {
        continue;
      }
      const double score = Score(l, view);
      if (!(score > 0.0)) {
        continue;
      }
      const auto found = by_id.find(l.id);
      if (found != by_id.end()) {
        Candidate& c = candidates[found->second];
        if (score > c.score) {
          c.lamp = l;
          c.score = score;
        }
        continue;
      }
      by_id.emplace(l.id, candidates.size());
      candidates.push_back({l.id, l, score, 0.0});
    }
    out->usable = uint32_t(candidates.size());
    std::unordered_map<uint64_t, size_t> track_of;
    for (size_t i = 0; i < tracks_.size(); ++i) {
      track_of.emplace(tracks_[i].id, i);
    }
    for (Candidate& c : candidates) {
      const auto t = track_of.find(c.id);
      c.ranked = c.score * ((t != track_of.end() && tracks_[t->second].wanted) ? kKeepBonus : 1.0);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.ranked != b.ranked ? a.ranked > b.ranked : a.id < b.id; });
    out->best_score = candidates.empty() ? 0.0 : candidates.front().score;
    const size_t wanted = std::min<size_t>(candidates.size(), wanted_cap);

    std::unordered_map<uint64_t, const Candidate*> now;  // the list's lamps by place, for the tracks that are leaving
    for (const Candidate& c : candidates) {
      now.emplace(c.id, &c);
    }
    std::unordered_map<uint64_t, bool> is_wanted;
    for (size_t i = 0; i < wanted; ++i) {
      is_wanted.emplace(candidates[i].id, true);
    }

    // The tracks that stay: the old ones, wanted (they grow) or not (they fade and go at weight 0). A tile is free when no track that stays holds it.
    std::vector<Track> next;
    next.reserve(tiles);
    std::array<bool, kMaxSlots> used{};
    for (const Track& old : tracks_) {
      Track t = old;
      const auto still = now.find(old.id);
      if (still != now.end()) {
        t.lamp = still->second->lamp;  // the list's data if it is still there, else the last it had
      }
      t.fresh = false;
      if (is_wanted.count(old.id) != 0) {
        t.wanted = true;
        t.weight = std::min(1.0f, old.weight + step);
      } else {
        t.wanted = false;
        t.weight = old.weight - step;
        if (!(t.weight > 0.0f)) {
          continue;  // gone: its tile is free
        }
      }
      if (t.tile < kMaxSlots) {
        used[t.tile] = true;
      }
      next.push_back(t);
    }
    // The new lamps (wanted, no track): the nearest free tile in the budget, else they wait (a lamp fading out holds its tile until it is gone).
    for (size_t i = 0; i < wanted; ++i) {
      const Candidate& c = candidates[i];
      if (track_of.count(c.id) != 0) {
        continue;
      }
      uint32_t tile = tiles;
      for (uint32_t k = 0; k < tiles; ++k) {
        if (!used[k]) {
          tile = k;
          break;
        }
      }
      if (tile == tiles) {
        ++out->dropped;
        continue;
      }
      used[tile] = true;
      Track t;
      t.id = c.id;
      t.lamp = c.lamp;
      t.tile = tile;
      t.wanted = true;
      t.fresh = true;
      t.weight = instant ? 1.0f : std::min(1.0f, step);
      next.push_back(t);
    }
    std::sort(next.begin(), next.end(), [](const Track& a, const Track& b) { return a.tile < b.tile; });
    tracks_ = std::move(next);
    started_ = true;
    for (const Track& t : tracks_) {
      if (out->count >= kMaxSlots) {
        break;
      }
      Entry& e = out->entries[out->count++];
      e.id = t.id;
      e.lamp = t.lamp;
      e.tile = t.tile;
      e.weight = t.weight;
      e.wanted = t.wanted;
      e.fresh = t.fresh;
      (t.wanted ? out->wanted : out->fading) += 1;
    }
  }

 private:
  struct Track {
    uint64_t id = 0;
    Lamp lamp;
    uint32_t tile = 0;
    float weight = 0.0f;
    bool wanted = false;
    bool fresh = false;
  };
  std::vector<Track> tracks_;
  bool started_ = false;
};

// ---------------------------------------------------------------------------
// the casters: which move, and what identifies a static one
// ---------------------------------------------------------------------------

// A draw is a MOVER (drawn again every frame, into the live tile) when it is a vehicle's part (the draws that write a vehicle's stencil ID: cars and the people in them) or a person's (a skinned
// character); every other caster is the static world's. The city's props have skinned twins (xProp*Skinned: palms and their trunks, traffic signals, neon, glass, metal props, the lamps' own
// glass: a skeleton that sways them, never a place that changes): taking EVERY skinned vertex shader for a mover (the first run) put a hundred and more world draws a lamp view into the live pass
// and the cache saved nothing. The identities (runtime FNV-1a 64 of the vfetch-normalised microcode; perf/scripts/vs_identities.py) of the prop families' skinned main-view shaders: sorted.
inline constexpr ls::NamedVs kStaticSkinnedVertexShaders[] = {
    {0x049FE324815E9A08ull, "xPropFoliage__VSPropFoliageSkinnedNight"},
    {0x093190AECF16C878ull, "xPropDefault__VSPropDefaultNightSkinned"},
    {0x093619ACFDBBB9FEull, "xPropMetal__VSPropMetalSkinned"},
    {0x16E47C6D6609A470ull, "xPropFoliageTrunk__VSPropFoliageTrunkSkinned"},
    {0x317E32BEE4AD9982ull, "xPropGlass__VSPropGlassSkinned"},
    {0x324F9A3B7517628Full, "xPropTrafficSignal__VSPropTrafficSignalSkinned"},
    {0x3BB11B547A11CCDEull, "xPropNeonTint__VSPropNeonTintSkinned"},
    {0x4665150C64D67F84ull, "xPropBumpSpecTint__VSPropBumpSpecTintSkinned"},
    {0x4914B75B2E19731Bull, "xPropBumpSpec__VSPropBumpSpecNightSkinned"},
    {0x7F423D6F1660EB38ull, "xPropBumpSpec__VSPropBumpSpecSkinned"},
    {0x8991379E466FAD61ull, "xTestFoliage__VSSkinned"},
    {0x959FE934CFB1D124ull, "xPropFoliage__VSPropFoliageSkinned"},
    {0x9C74069D078963D7ull, "xPropBumpSpecTint__VSPropBumpSpecTintNightSkinned"},
    {0xAD179853802A3431ull, "xPropLampGlassTint__VSPropLampGlassTintSkinned"},
    {0xF34521D8C03369EBull, "xPropDefault__VSPropDefaultSkinned"},
};

inline bool IsStaticSkinnedVs(uint64_t vs_key) {
  for (const ls::NamedVs& v : kStaticSkinnedVertexShaders) {
    if (v.key == vs_key) {
      return true;
    }
  }
  return false;
}

inline bool IsDynamic(ls::Kind kind, uint64_t vs_key) {
  if (kind == ls::Kind::kVehicle) {
    return true;
  }
  return kind == ls::Kind::kSkinned && !IsStaticSkinnedVs(vs_key);
}

// What identifies a draw from frame to frame: the mesh (its buffers and range, the shader and the layout) and where it stands (to a sixteenth of a metre). A mesh that is drawn again at another
// place is another draw (a prop that was moved).
struct DrawKey {
  uint64_t vs = 0;
  uint32_t layout = 0;
  uint32_t topology = 0;
  uint32_t first = 0;
  uint32_t count = 0;
  int32_t base_vertex = 0;
  uint64_t index_buffer = 0;    // the host buffer's identity (a pointer is enough: it is only compared)
  uint64_t index_offset = 0;
  uint64_t vertex_buffer = 0;
  uint64_t vertex_offset = 0;
  double origin[3] = {};
};

inline uint64_t StaticDrawId(const DrawKey& k) {
  uint64_t words[12];
  words[0] = k.vs;
  words[1] = (uint64_t(k.layout) << 32) | k.topology;
  words[2] = (uint64_t(k.first) << 32) | k.count;
  words[3] = uint64_t(uint32_t(k.base_vertex));
  words[4] = k.index_buffer;
  words[5] = k.index_offset;
  words[6] = k.vertex_buffer;
  words[7] = k.vertex_offset;
  for (int i = 0; i < 3; ++i) {
    words[8 + i] = uint64_t(int64_t(std::llround(k.origin[i] * 16.0)));
  }
  words[11] = 0x9E3779B97F4A7C15ull;
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint64_t w : words) {
    h ^= w;
    h *= 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h != 0 ? h : 1ull;  // 0 is the set's empty slot
}

// ---------------------------------------------------------------------------
// the static layer
// ---------------------------------------------------------------------------

// The set of the static draws a layer holds: open addressing, never removes (the layer is cleared as a whole).
class DrawnSet {
 public:
  void Clear() {
    if (!slots_.empty()) {
      std::fill(slots_.begin(), slots_.end(), 0ull);
    }
    used_ = 0;
  }
  bool Contains(uint64_t id) const {
    if (slots_.empty()) {
      return false;
    }
    const size_t mask = slots_.size() - 1;
    for (size_t i = size_t(id) & mask;; i = (i + 1) & mask) {
      if (slots_[i] == id) {
        return true;
      }
      if (slots_[i] == 0) {
        return false;
      }
    }
  }
  // Adds an id; true when it was new.
  bool Insert(uint64_t id) {
    if (slots_.empty()) {
      slots_.assign(kDrawnSlots, 0ull);
    }
    const size_t mask = slots_.size() - 1;
    for (size_t i = size_t(id) & mask;; i = (i + 1) & mask) {
      if (slots_[i] == id) {
        return false;
      }
      if (slots_[i] == 0) {
        slots_[i] = id;
        ++used_;
        return true;
      }
    }
  }
  // Three quarters full: the layer is built again rather than let a lookup run long.
  bool Full() const { return !slots_.empty() && used_ * 4 >= slots_.size() * 3; }
  size_t size() const { return used_; }

 private:
  std::vector<uint64_t> slots_;
  size_t used_ = 0;
};

// One lamp tile's static layer.
struct StaticLayer {
  bool valid = false;   // the layer holds a map made for `id`
  uint64_t id = 0;      // the lamp it was made for
  uint64_t built = 0;   // the frame it was (re)built in
  uint64_t epoch = 0;   // the world epoch it was made in
  DrawnSet drawn;
  void Invalidate() {
    valid = false;
    drawn.Clear();
  }
};

enum class Build : uint8_t { kAdd, kRebuild };

// WHEN A LAMP'S STATIC LAYER IS BUILT AGAIN ("rebuilt only when the sector changes"). A street lamp does not move and neither does the world it casts from, so a layer made for the lamp stays
// good until the WORLD around it changes. kRebuild (the layer is cleared and every static draw of the view is drawn into it) when: the lamp just took the tile (a new id) or the layer was never
// built; the atlas was made again or its pools moved (`atlas_fresh`: the texture holds nothing); the world epoch changed (`world_epoch`: the caller bumps it when a city sector streams in or
// out; 0 forever when nothing says so); the set of remembered draws is full; or, as the safety net where no epoch is given, every kRefreshFrames (staggered by tile so that the lamps do not all
// rebuild in one frame). Otherwise kAdd: the static draws the layer does not hold yet are added (the main view shows more of the lamp's cone as the camera moves: a pole that was in the frame a
// second ago still casts).
inline Build DecideStatic(const StaticLayer& layer, uint64_t id, uint64_t frame, uint32_t tile, bool atlas_fresh, uint64_t world_epoch = 0) {
  if (!layer.valid || layer.id != id || atlas_fresh || layer.epoch != world_epoch || layer.drawn.Full()) {
    return Build::kRebuild;
  }
  if (frame >= layer.built + kRefreshFrames + uint64_t(tile) * kRefreshStagger) {
    return Build::kRebuild;
  }
  return Build::kAdd;
}

// ---------------------------------------------------------------------------
// what one lamp's passes draw
// ---------------------------------------------------------------------------

// One collected caster as the planner needs it (the renderer fills these: local_shadow.cpp).
struct CasterFact {
  uint32_t index = 0;        // the replay's caster index (what the passes draw)
  double origin[3] = {};     // its world matrix's translation
  bool vehicle = false;      // a vehicle's part (reach: kVehicleReach; else the world's)
  bool mover = false;        // IsDynamic
};

struct PassInput {
  ls::ViewSpec spec;         // the lamp's frustum
  bool cone_cull = true;
  bool cache = true;
  uint32_t tile = 0;
  uint64_t id = 0;           // the lamp
  uint64_t frame = 0;
  uint64_t world_epoch = 0;
  bool atlas_fresh = false;
  uint32_t max_draws = kMaxDrawsPerLampView;
};

struct PassPlan {
  Build build = Build::kAdd;
  bool cache = false;
  std::vector<uint32_t> static_draws;  // replay caster indices drawn into the STATIC tile: all of the view's world draws on a rebuild, else the ones the layer does not hold yet
  std::vector<uint64_t> new_ids;       // what the static layer is to remember after the pass (never before: a layer is not claimed to hold a draw that was not recorded)
  std::vector<uint32_t> live_draws;    // drawn into the LIVE tile (cache off: everything, into the one tile)
  uint32_t world_in_view = 0;          // world draws in the cone
  uint32_t movers_in_view = 0;
  uint32_t known = 0;                  // world draws the layer already held (not drawn again)
  uint32_t over_budget = 0;            // left out for the draw cap (the farthest)
  bool rebuild() const { return build == Build::kRebuild; }
};

// The planner: from the caster facts of the frame and a lamp's frustum, what the lamp's static pass and live pass draw. `id_of(fact index)` gives a world draw's identity (StaticDrawId; made
// lazily by the caller, only for the draws a static layer is asked about). The nearest `max_draws` of each list are kept, in draw order (the farthest are left out: counted).
template <class IdOf>
inline void PlanLampPass(const StaticLayer& layer, const PassInput& in, const CasterFact* casters, uint32_t count, IdOf&& id_of, PassPlan* out) {
  *out = PassPlan{};
  out->cache = in.cache;
  std::vector<uint32_t> world, movers;
  world.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const CasterFact& c = casters[i];
    const double reach = c.mover ? kDynamicReach : ls::CasterReach(c.vehicle);
    if (in.cone_cull && !ls::SphereInView(in.spec, c.origin, reach)) {
      continue;
    }
    ((in.cache && c.mover) ? movers : world).push_back(i);
  }
  out->world_in_view = uint32_t(world.size());
  out->movers_in_view = uint32_t(movers.size());
  // The nearest `max_draws` of a list in draw order (no lamp has an own car: nothing is left out for that).
  std::vector<ls::Candidate> candidates;
  std::vector<uint32_t> keep;
  const auto choose = [&](const std::vector<uint32_t>& list, std::vector<uint32_t>* chosen) {
    candidates.clear();
    candidates.reserve(list.size());
    for (const uint32_t i : list) {
      ls::Candidate cand;
      cand.index = i;
      cand.vehicle = false;
      cand.distance = ls::detail::Distance(casters[i].origin, in.spec.eye);
      candidates.push_back(cand);
    }
    keep.assign(candidates.size(), 0);
    uint32_t verdicts[size_t(ls::Skip::kCount)] = {};
    const uint32_t kept = ls::SelectForView(candidates.data(), uint32_t(candidates.size()), in.max_draws, keep.data(), verdicts);
    out->over_budget += verdicts[size_t(ls::Skip::kBudget)];
    chosen->assign(kept, 0);
    for (uint32_t k = 0; k < kept; ++k) {
      (*chosen)[k] = candidates[keep[k]].index;
    }
  };
  std::vector<uint32_t> world_chosen, movers_chosen;
  choose(world, &world_chosen);
  if (!in.cache) {
    out->live_draws.clear();
    for (const uint32_t i : world_chosen) {
      out->live_draws.push_back(casters[i].index);
    }
    return;
  }
  choose(movers, &movers_chosen);
  for (const uint32_t i : movers_chosen) {
    out->live_draws.push_back(casters[i].index);
  }
  out->build = DecideStatic(layer, in.id, in.frame, in.tile, in.atlas_fresh, in.world_epoch);
  for (const uint32_t i : world_chosen) {
    const uint64_t id = id_of(i);
    if (out->build == Build::kAdd && layer.drawn.Contains(id)) {
      ++out->known;
      continue;
    }
    if (out->build == Build::kAdd && out->static_draws.size() >= kStaticAddsPerFrame) {
      continue;  // the rest next frame
    }
    out->static_draws.push_back(casters[i].index);
    out->new_ids.push_back(id);
  }
}

// What the layer holds after the pass was recorded: `partial` (the upload ring ran out: the pass is not the map the plan said) forgets it, a rebuild starts the layer over and a kAdd adds.
inline void CommitStatic(StaticLayer* layer, const PassPlan& plan, uint64_t id, uint64_t frame, uint64_t world_epoch, bool partial) {
  if (partial) {
    layer->Invalidate();
    return;
  }
  if (plan.rebuild()) {
    layer->Invalidate();
    layer->valid = true;
    layer->id = id;
    layer->built = frame;
    layer->epoch = world_epoch;
  }
  for (const uint64_t d : plan.new_ids) {
    layer->drawn.Insert(d);
  }
}

// ---------------------------------------------------------------------------
// the published frame and the lamps' lights
// ---------------------------------------------------------------------------

// One tracked lamp as the publisher needs it.
struct Published {
  uint64_t id = 0;
  uint32_t tile = 0;
  float weight = 0.0f;
  bool valid = false;          // a view was made (the planner and the matrices worked)
  bool has_live = false;       // the live tile holds casters this frame
  float reach = 0.0f;          // metres
  tp::Mat4 clip;               // the lamp's LightClip
};

// Where a lamp's entries are in the frame (the combined first entry and how many), per tracked lamp in order.
struct LampRef {
  uint64_t id = 0;
  uint32_t first_view = 0;     // kLampEntryBase + the row
  uint32_t view_count = 0;     // 0: not published
  float weight = 0.0f;
  float reach = 0.0f;
};

struct PublishInput {
  const sa::Layout* layout = nullptr;
  int live_pool = -1;          // the layout's pools (Layout::FindPool): -1 not there
  int static_pool = -1;
  bool cache = true;
  double sx = 1.0, sy = 1.0;   // the pass's NDC signs (WorldToShadow)
  float depth_bias = ls::kDepthBias;
  uint32_t atlas_slot = lg::kNoLightSlot;
  uint32_t sampler = lg::kNoLightSlot;
  bool atlas_valid = false;
};

// The lamps' entries: for each published lamp the static entry (matrix to its static tile), then the live twin when the live tile holds casters, or one single entry when the cache is off (the
// live pool's tile holds everything). Entries are consecutive per lamp, in the order given; a lamp that does not fit (20 entries) is left out. `refs` gets one row per input lamp.
inline void BuildFrame(const std::vector<Published>& lamps, const PublishInput& in, lg::LampShadowFrame* out, std::vector<LampRef>* refs) {
  *out = lg::LampShadowFrame{};
  refs->assign(lamps.size(), LampRef{});
  if (in.layout == nullptr || !in.layout->valid()) {
    return;
  }
  out->atlas = sa::InfoOf(*in.layout, in.atlas_slot, in.sampler, in.atlas_valid);
  const auto add = [&](const Published& p, const sa::Rect& rect, lg::ShadowLayer layer) {
    if (out->count >= lg::kMaxLampEntries || !rect.valid()) {
      return false;
    }
    float uv[4];
    sa::UvOf(*in.layout, rect, uv);
    lg::LocalShadowView& v = out->views[out->count];
    ls::WorldToShadow(p.clip, uv, in.sx, in.sy, v.world_to_shadow);
    ls::PublishedRect(uv, v.atlas_rect);
    v.depth_bias = in.depth_bias;
    v.texel_size = rect.size != 0 ? 1.0f / float(rect.size) : 0.0f;
    lg::SetLampReach(&v, p.reach);
    v.valid = 1;
    out->view_light[out->count] = p.id;
    out->view_layer[out->count] = layer;
    ++out->count;
    return true;
  };
  for (size_t k = 0; k < lamps.size(); ++k) {
    const Published& p = lamps[k];
    if (!p.valid || p.id == lg::kNoLightId || in.live_pool < 0) {
      continue;
    }
    const sa::Rect live = in.layout->SlotRect(uint32_t(in.live_pool), p.tile);
    const sa::Rect stat = (in.cache && in.static_pool >= 0) ? in.layout->SlotRect(uint32_t(in.static_pool), p.tile) : sa::Rect{};
    const uint32_t first = out->count;
    if (in.cache && stat.valid()) {
      if (out->count + (p.has_live ? 2u : 1u) > lg::kMaxLampEntries) {
        continue;
      }
      add(p, stat, lg::ShadowLayer::kStatic);
      if (p.has_live) {
        add(p, live, lg::ShadowLayer::kLive);
      }
    } else {
      add(p, live, lg::ShadowLayer::kSingle);
    }
    if (out->count > first) {
      refs->at(k) = LampRef{p.id, lg::kLampEntryBase + first, out->count - first, p.weight, p.reach};
    }
  }
}

// The light the pass draws for a tracked lamp. kSubtract: the lamp as the GRID has it (x and z through the game's half float), the range the shadow's reach (+ a margin: beyond it the shadow
// is faded out and there is nothing to take away), the game's flag set (the pass subtracts for a lamp the game lights). kOwn: the lamp as the game's list has it, its own range, the flag
// cleared (the pass adds it: the grid has it off). Its ShadowRef names the lamp's entries and the weight; no entries: no atlas ref (the pass leaves it out in subtract mode).
inline lg::Light MakePassLight(const Lamp& lamp, const LampRef& ref, LampMode mode) {
  lg::Light l;
  float pos[3];
  if (mode == LampMode::kSubtract) {
    GridPosition(lamp.pos, pos);
  } else {
    for (int i = 0; i < 3; ++i) {
      pos[i] = lamp.pos[i];
    }
  }
  l = lg::MakeGameLampLight(pos, lamp.colour, lamp.dir, lamp.intensity, lamp.cone_deg, lamp.spread_deg, lamp.scale, lamp.type, lg::LightSource::kLamp, lamp.id);
  const float reach = float(ShadowReach(lamp.intensity));
  if (mode == LampMode::kSubtract) {
    l.range = std::min(l.range, reach + kPassRangeMargin);
    l.roles = lg::kRoleMaskGameLit;
  } else {
    l.flags &= ~lg::kLightFlagGameLit;  // the pass adds this lamp: it is not in the grid (local_lights_rules.h AsPassLight)
    l.roles = uint8_t(lg::kRoleMaskGameLit | lg::kRoleMaskSurface);
  }
  if (ref.view_count != 0) {
    l.shadow.kind = lg::ShadowKind::kAtlas;
    l.shadow.first_view = uint8_t(ref.first_view);
    l.shadow.view_count = uint8_t(ref.view_count);
    l.shadow.weight = ref.weight;
    l.shadow.reach = ref.reach > 0.0f ? ref.reach : reach;
  }
  return l;
}

// ---------------------------------------------------------------------------
// the log
// ---------------------------------------------------------------------------

// "high: 4 lamps (+1 spare) of 512x512, static layers on: 5 + 5 tiles, 20.0 MiB".
inline std::string Describe(const Budget& b) {
  if (!b.on()) {
    return "no lamp views";
  }
  std::string text = std::to_string(b.wanted) + " lamps (+" + std::to_string(b.spare) + " spare) of " + std::to_string(b.tile) + "x" + std::to_string(b.tile);
  text += b.cache ? ", static layers on: " + std::to_string(b.tiles()) + " + " + std::to_string(b.tiles()) + " tiles" : ", one layer: " + std::to_string(b.tiles()) + " tiles";
  char mib[32];
  std::snprintf(mib, sizeof(mib), ", %.1f MiB", sa::Mib(PoolBytes(b)));
  return text + mib;
}

}  // namespace mcla::gpu::lamp_shadows
