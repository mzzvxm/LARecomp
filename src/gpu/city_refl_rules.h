#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city in the car paint's reflections: the rules (CITY-RT M2 packages R1 and R2)
// ===========================================================================
// THE GOAL (the owner's, 2026-11-20): in the paint's reflection you can CLEARLY IDENTIFY what you see: the building across the street, the lamp posts and the lamp heads, a sign, the car
// beside you; the way Gran Turismo 7 does on a strong ray tracing card. Until now the player's paint reflected the game's 512x256 environment map (every street lamp a blob: TRUNK-LIGHTS)
// and the cars' parts (car_rt_refl.h). A reflection ray that finds none of the cars' parts now goes on into the CITY (the structures of city_rt_scene.h, in the same top level: one
// TLAS, the scene service's) and its hit is made a colour by the city hit library (hlsl/city_hit_shade.hlsli, CITY-RT M2 H4): "the picture's own colour where the hit is on the screen, shaded
// from its material where it is not" (ResolveHit, CITY-RT-M2.md section 4). The sky stays the game's environment map: a ray that leaves the structures is a miss and the paint keeps it.
//
// WHAT REFLECTS WHAT (CITY-RT-M2.md sections 2 and 6):
//   the player's paint        R1  the cone of 4 rays and the clear-coat mirror ray (car_rt_refl_cs.hlsl) take the city bit (kPartCity) in their mask: a ray's closest hit is a car part or a city
//                             triangle (CityHitTrace: the cut-outs' exact alpha in the candidate loop). The image's codes are the ones the paint's variant already reads (alpha 1 + w, rgb the colour),
//                             so no variant changes: where the cone finds the city the environment's weight goes down by the paint's fresnel share, over the sky it stays.
//   other cars' paint         R2  the opponents' variant (car_rt_refl_opp.hlsli) is the same shader file: the same trace, its own budget (OppTexels, OppRays: HIGH 250 k texels at 2 rays, ULTRA 500 k at
//                             4: the pass's texel plan already spends 4, 2 or 1 ray a texel by the area and takes the smaller). Traffic and parked cars (RT-TRAFFIC-REFL, feat/rt-next) take
//                             the same cell where that lands.
//   chrome, glass             R3 (CITY-RT M2 R3a, R3b; the player's own): the chrome variant's city stage reads the paint's image for the colour of the city and the cars beside (feature 13,
//                             shaderpack/variants/car_chrome_city.h, wanted by pipelines.cpp while the level is not off and mcla_gpu_city_refl_chrome is on), and an additive pass over the pixels of the
//                             car's windows (car_rt_glass_refl.h, mcla_gpu_city_refl_glass) puts the mirror reflection of the city in the glass by its Fresnel. Both are served at every level.
//
// THE LEVELS (the cvar mcla_gpu_rt_reflections: auto / off / high / ultra; live; the pause menu's row comes with R8): `auto` is the rt_tier.h cell of kRtReflections (off without ray tracing and on
// a WEAK card, high on a strong one: the owner's goal is a strong card's look; a weak card may still be set to high or ultra by hand and runs the Mid class's cell). The cell holds the reach
// (HIGH 150 m, ULTRA 1,000 m with the weight full to 300 m: the structures' far level of detail; a Mid class by hand 60 m, 150 m at ultra), the cone's rays, the opponents' budget and the
// texture scope (all: a reflection shades a hit from its diffuse). Reach, rays and texture cap are CITY-RT-M2.md section 8; the opponents' texel budgets are this package's (CHECK: cost).
//
// WHAT THE PASS ASKS FOR: the scene service (rt_scene_service.h) gets Want(kCityReflections, {cars, city level, hit level, all textures}); ScreenShared gets Want(kCarRtRefl, colour_post | lin_depth)
// (the screen shortcut of ResolveHit, with the mip chain of colour_post). A frame whose products are not there (the want lags one frame: the first frame after a switch, a pause view) resolves
// by the model alone. The shader's constants are a small buffer of rows (GpuRows, bound as set 3), written by the host every frame.
//
// THE NUMBERS THE PASS DOES NOT MAKE UP: the sun (frame rows), the lamp schedule (the sun's elevation, the headlight rig's night curve: LampSchedule; the lamp list and the game's own
// lamp rule are R7), the wetness (0: the rain's wetness is R4's), the haze (none: CITY_HIT_HAZE is not wired). Each is a CHECK in city_refl.h.
//
// This header includes no renderer: city_refl_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "city_rt_rules.h"
#include "city_texture_scope.h"
#include "gpu_class.h"
#include "rt_hit_calibration.h"
#include "rt_tier.h"

namespace mcla::gpu::city_refl {

using Level = city_rt::Level;
using Choice = city_rt::Choice;

// ---------------------------------------------------------------------------
// the mask
// ---------------------------------------------------------------------------

// The city's instance bit (city_rt_rules.h kMaskCity; hlsl CAR_RT_MASK_CITY). A reflection ray takes it in its parts; the primary ray never does (it finds the car's own panel).
inline constexpr uint32_t kPartCity = 0x80;
static_assert(kPartCity == city_rt::kMaskCity, "the city's mask bit is the structures' own");

// Which surfaces reflect the city. The paint of the player's car (R1) and of other cars (R2), and the player's chrome and glass (R3) are served.
inline constexpr uint32_t kClassPaintPlayer = 1;
inline constexpr uint32_t kClassPaintOthers = 2;
inline constexpr uint32_t kClassChrome = 4;
inline constexpr uint32_t kClassGlass = 8;
inline constexpr uint32_t kServedClasses = kClassPaintPlayer | kClassPaintOthers | kClassChrome | kClassGlass;

constexpr uint32_t ClassesFor(Level level) { return level == Level::kOff ? 0u : kServedClasses; }
// Whether a class reflects the city at a level (the chrome's stage and the glass pass ask this, each with its debug switch).
constexpr bool Serves(Level level, uint32_t cls) { return (ClassesFor(level) & cls) == cls; }

// The reflection ray's parts with the city's bit set or taken away by the level.
constexpr uint32_t ReflectionParts(uint32_t parts, Level level) { return level == Level::kOff ? (parts & ~kPartCity) : (parts | kPartCity); }

// How far a reflection ray is cast: the longest of the reaches of what it may hit (the player's own parts', another car's and, with the city's bit, the city's).
constexpr float CastLength(uint32_t parts, float cars_reach, float city_reach) { return (parts & kPartCity) != 0 ? std::max(cars_reach, city_reach) : cars_reach; }

// ---------------------------------------------------------------------------
// the levels
// ---------------------------------------------------------------------------

struct Cell {
  Level level = Level::kOff;
  float reach = 0.0f;          // m: a city hit weighs 1 up to fade_from of it and nothing at it; the ray is cast this far (the structures end first where they are built less far)
  float fade_from = 0.6f;
  uint32_t cone_rays = 4;      // the cone's rays the level allows at most (4 whole; 2 the outer pair)
  uint64_t opp_texels = 0;     // the opponents' pass's area at the full cone, in image texels
  uint32_t opp_rays = 4;       // the most rays a texel of another car's paint casts into the city
  TextureScope textures = TextureScope::kNone;
  bool coat_layer = false;     // a city hit also takes the paint's clear-coat layer (the image's sign: car_rt_hit_rules.h CoatLayerAmount)
  bool operator==(const Cell&) const = default;
};

// The cell of a level on a ray tracing budget class (rt_tier.h RtClass: a weak card is at most Mid). Low is none.
constexpr Cell CellFor(Level level, gpu_class::GpuClass cls) {
  Cell c;
  if (level == Level::kOff || cls == gpu_class::GpuClass::kLow) {
    return c;
  }
  c.level = level;
  c.textures = TextureScope::kAll;
  c.coat_layer = true;
  if (cls == gpu_class::GpuClass::kHigh) {
    if (level == Level::kHigh) {
      c.reach = 150.0f;
      c.fade_from = 0.6f;
      c.cone_rays = 4;
      c.opp_texels = 250000;
      c.opp_rays = 2;
    } else {
      c.reach = 1000.0f;
      c.fade_from = 0.3f;
      c.cone_rays = 4;
      c.opp_texels = 500000;
      c.opp_rays = 4;
    }
  } else if (level == Level::kHigh) {
    c.reach = 60.0f;  // a Mid class by hand (CITY-RT-M2.md section 8: "60 m, hi ring 150 m")
    c.fade_from = 0.6f;
    c.cone_rays = 2;
    c.opp_texels = 100000;
    c.opp_rays = 1;
  } else {
    c.reach = 150.0f;  // ultra asked of a Mid class: the High cell's reach with the Mid class's rays
    c.fade_from = 0.6f;
    c.cone_rays = 2;
    c.opp_texels = 150000;
    c.opp_rays = 2;
  }
  return c;
}

// The level a setting text comes to on this class and tier: `auto` is rt_tier.h's cell of kRtReflections (the rung gates it), a hand set high or ultra runs wherever the card can trace
// (never on the rung none: no ray tracing units, or the Low class). Debug-only values the list does not know are `auto`.
inline Level LevelFor(gpu_class::GpuClass cls, gpu_class::RtTier tier, std::string_view text) {
  if (gpu_class::RtRung(cls, tier) == gpu_class::RtTier::kNone) {
    return Level::kOff;
  }
  const Choice choice = city_rt::ParseChoice(text);
  switch (choice) {
    case Choice::kOff: return Level::kOff;
    case Choice::kHigh: return Level::kHigh;
    case Choice::kUltra: return Level::kUltra;
    default: break;
  }
  return city_rt::LevelOfCell(gpu_class::RtCell(gpu_class::RtEffect::kRtReflections, gpu_class::RtRung(cls, tier)));
}

// The cell with the debug reach override (0: the level's).
constexpr Cell WithReach(Cell c, float reach) {
  if (reach > 0.0f && c.level != Level::kOff) {
    c.reach = std::clamp(reach, 5.0f, 3000.0f);
  }
  return c;
}

// What the pass asks the scene service for (rt_scene_rules.h Needs, minus the type: the host builds it).
struct Wants {
  bool cars = false;
  Level city = Level::kOff;
  Level hit = Level::kOff;
  TextureScope textures = TextureScope::kNone;
};
constexpr Wants WantsFor(const Cell& c) {
  Wants w;
  if (c.level != Level::kOff) {
    w.cars = true;
    w.city = c.level;
    w.hit = c.level;
    w.textures = c.textures;
  }
  return w;
}

// The opponents' pass's texel budget with the city on: the class's own (car_rt_opp_refl_rules.h TexelBudget) or the cell's, the smaller; the rays a texel casts at most.
constexpr uint64_t OppBudget(uint64_t class_budget, const Cell& c) { return c.level == Level::kOff ? class_budget : std::min<uint64_t>(class_budget, c.opp_texels); }
constexpr uint32_t OppRays(uint32_t planned, const Cell& c) { return c.level == Level::kOff ? planned : std::min<uint32_t>(planned, c.opp_rays); }

// The weight of a city hit by its distance: 1 up to fade_from of the reach, a smoothstep to 0 at the reach (the car hits' HitFade).
inline float CityFade(float distance, float reach, float fade_from) {
  if (!(reach > 0.0f)) {
    return 0.0f;
  }
  const float lo = fade_from * reach;
  const float t = std::clamp((distance - lo) / std::max(reach - lo, 1e-6f), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// How wide a reflected ray is where it hits, in metres: the cone's width (2 t tan) and the pixel's spread along the path (the history texel's angular size times the path length): what the hit's
// texture level and the picture's mip are read at. A mirror (tan 0) is as sharp as one history texel is wide at that distance.
constexpr float HitFootprint(float distance, float cone_tan, float texel_tan) { return 2.0f * distance * cone_tan + distance * texel_tan; }
// The angular size of a history texel: 2 tan(half the vertical field of view) over the image's height.
constexpr float TexelTan(float tan_y, float image_height) { return 2.0f * tan_y / std::max(image_height, 1.0f); }

// ---------------------------------------------------------------------------
// the lamp schedule
// ---------------------------------------------------------------------------

// Lamp lenses, neon signs and lit windows glow by the lamp schedule: 1 at night, 0 by day. The world's lamp rule (the game's own hours, the lamp list's count: R7) is not wired here; this takes the
// sun's elevation through the headlight rig's night curve (headlight_rig_rules.h NightFactor: 1 at and below 3 degrees, 0 from 12, smooth between; "19 h is about 2.5 degrees with a grey sun"). It
// needs the frame's sun direction (RT_HIT_FRAME_SUN); without it the lamps do not glow.
inline constexpr float kNightFullDegrees = 3.0f;
inline constexpr float kNightNoneDegrees = 12.0f;
inline float LampSchedule(float sun_sine, bool sun_known) {
  if (!sun_known) {
    return 0.0f;
  }
  const float degrees = std::asin(std::clamp(sun_sine, -1.0f, 1.0f)) * 180.0f / 3.14159265358979f;
  const float t = std::clamp((degrees - kNightFullDegrees) / (kNightNoneDegrees - kNightFullDegrees), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// the shader's numbers (the sun's disc, the picture's trust)
// ---------------------------------------------------------------------------

// The sun disc the hit's sun ray is spread over: tan of 3 times the real sun's angular radius (0.265 degrees), as the city's shadow pass (city_rt_shadow_rules.h kDefaultSunScale 3).
inline constexpr float kSunTan = 0.01388f;
inline constexpr float kSunReach = 0.0f;       // 0: the library's (2,000 m)
inline constexpr float kAoMix = 0.5f;          // the share of the baked vertex occlusion in a hit's albedo (CityHitParams::aoMix)
inline constexpr float kLodBias = 0.0f;
inline constexpr float kScreenToleranceM = 0.05f;    // the picture's depth tolerance at a hit: this plus kScreenToleranceRel of the view depth plus the slope
inline constexpr float kScreenToleranceRel = 0.01f;
inline constexpr float kScreenEdgeBand = 0.04f;      // ndc: a hit this near the picture's edge fades to the model
inline constexpr float kScreenMinFacing = 0.05f;
inline constexpr float kMinGain = 0.5f;
inline constexpr float kMaxGain = 3.0f;
inline constexpr uint32_t kMaxColourLevels = 8;

// The library's flags (hlsl CITY_HIT_P_*) a REFLECT pass sets: the sun's ray (the city has real shadows: the game's cascades are not read), the glow of what glows, the picture's colour.
inline constexpr uint32_t kPSunRay = 1;
inline constexpr uint32_t kPSunCascade = 2;
inline constexpr uint32_t kPLamps = 4;
inline constexpr uint32_t kPLampShadow = 8;
inline constexpr uint32_t kPEmissive = 16;
inline constexpr uint32_t kPCalibrate = 32;
inline constexpr uint32_t kPScreen = 64;
constexpr uint32_t ParamFlags(bool screen, bool stats) { return kPSunRay | kPEmissive | (screen ? kPScreen : 0u) | (stats ? kPCalibrate : 0u); }

// The size of colour_post's mip level `level` for a level 0 of `size`: halved with the odd edge rounded up (screen_frame.h HizLevelSize; hlsl CityReflLevelSize).
constexpr uint32_t LevelSize(uint32_t size, uint32_t level) { return std::max<uint32_t>(1, (size + (1u << level) - 1u) >> level); }
// The mip the picture's colour is read at for a hit's footprint (city_hit_shade_rules.h ScreenMip) clamped to the chain.
inline float ClampMip(float mip, uint32_t levels) { return std::clamp(mip, 0.0f, float(std::max<uint32_t>(levels, 1) - 1)); }

// ---------------------------------------------------------------------------
// the constants rows (set 3, t0: a ByteAddressBuffer the host writes every frame; hlsl/city_refl.hlsli reads the same rows)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRowWords = 4;
inline constexpr uint32_t kRows = 13;
inline constexpr uint32_t kRowBytes = kRowWords * 4;
inline constexpr uint32_t kBufferBytes = 256;  // the upload buffer's size (kRows rows and room)
static_assert(kRows * kRowBytes <= kBufferBytes);

// the rows, by index
inline constexpr uint32_t kRowControl = 0;   // x flags, y level (1 high, 2 ultra), z the frame's serial (the noise), w the library's flags (CITY_HIT_P_*)
inline constexpr uint32_t kRowDepth = 1;     // x lin_depth level 0's bindless slot, y, z its width and height, w colour_post's levels
inline constexpr uint32_t kRowColour = 2;    // x, y colour_post level 0's width and height, z, w reserved
inline constexpr uint32_t kRowSlotsA = 3;    // colour_post levels 0 to 3: bindless slots
inline constexpr uint32_t kRowSlotsB = 4;    // levels 4 to 7
inline constexpr uint32_t kRowReach = 5;     // floats: x the city reach, y where its fade starts, z the sun disc's tangent, w the sun ray's reach
inline constexpr uint32_t kRowScreen = 6;    // floats: the picture's tolerance in metres, its share of the depth, the edge band, the least facing
inline constexpr uint32_t kRowAlbedo = 7;    // floats: x the vertex occlusion's share, y the texture level's bias, z the wetness, w the debug view (as float bits of an int)
inline constexpr uint32_t kRowGains = 8;     // four rows of floats: the 16 classes' gains (rt_hit_calibration.h ClassGains::Table)
inline constexpr uint32_t kRowGainsLast = 11;
inline constexpr uint32_t kRowMore = 12;     // x the cone's tangent scale (1), y, z, w reserved

// the control flags
inline constexpr uint32_t kFlagOn = 1;        // the pass traces the city this frame
inline constexpr uint32_t kFlagScreen = 2;    // lin_depth and colour_post are in the constants
inline constexpr uint32_t kFlagStats = 4;     // a stats frame: the counters count
inline constexpr uint32_t kFlagCoat = 8;      // a city hit takes the paint's clear-coat layer (the image's sign)

struct RowsIn {
  Level level = Level::kOff;
  bool stats = false;
  uint32_t serial = 0;
  bool screen = false;
  uint32_t depth_slot = 0xFFFFFFFFu;
  uint32_t depth_width = 0, depth_height = 0;
  uint32_t colour_levels = 0;
  uint32_t colour_width = 0, colour_height = 0;
  uint32_t colour_slots[kMaxColourLevels] = {};
  Cell cell;
  float sun_tan = kSunTan;
  float wet = 0.0f;
  int32_t view = 0;
  float gains[rt_hit_cal::kClasses] = {};
};

inline uint32_t Bits(float f) {
  uint32_t u;
  static_assert(sizeof(u) == sizeof(f));
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float FloatOf(uint32_t u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

// The rows of a frame. An empty RowsIn (level off) is all zero: the shader sees flags 0.
inline void MakeRows(const RowsIn& in, uint32_t rows[kRows][kRowWords]) {
  std::memset(rows, 0, sizeof(uint32_t) * kRows * kRowWords);
  if (in.level == Level::kOff) {
    return;
  }
  const bool screen = in.screen && in.depth_slot != 0xFFFFFFFFu && in.depth_width != 0 && in.depth_height != 0 && in.colour_levels != 0 && in.colour_width != 0 && in.colour_height != 0;
  uint32_t flags = kFlagOn;
  flags |= screen ? kFlagScreen : 0u;
  flags |= in.stats ? kFlagStats : 0u;
  flags |= in.cell.coat_layer ? kFlagCoat : 0u;
  rows[kRowControl][0] = flags;
  rows[kRowControl][1] = in.level == Level::kUltra ? 2u : 1u;
  rows[kRowControl][2] = in.serial;
  rows[kRowControl][3] = ParamFlags(screen, in.stats);
  if (screen) {
    rows[kRowDepth][0] = in.depth_slot;
    rows[kRowDepth][1] = in.depth_width;
    rows[kRowDepth][2] = in.depth_height;
    rows[kRowDepth][3] = std::min(in.colour_levels, kMaxColourLevels);
    rows[kRowColour][0] = in.colour_width;
    rows[kRowColour][1] = in.colour_height;
    for (uint32_t i = 0; i < kMaxColourLevels; ++i) {
      rows[kRowSlotsA + i / 4][i % 4] = i < in.colour_levels ? in.colour_slots[i] : 0xFFFFFFFFu;
    }
  }
  rows[kRowReach][0] = Bits(in.cell.reach);
  rows[kRowReach][1] = Bits(in.cell.fade_from);
  rows[kRowReach][2] = Bits(in.sun_tan);
  rows[kRowReach][3] = Bits(kSunReach);
  rows[kRowScreen][0] = Bits(kScreenToleranceM);
  rows[kRowScreen][1] = Bits(kScreenToleranceRel);
  rows[kRowScreen][2] = Bits(kScreenEdgeBand);
  rows[kRowScreen][3] = Bits(kScreenMinFacing);
  rows[kRowAlbedo][0] = Bits(kAoMix);
  rows[kRowAlbedo][1] = Bits(kLodBias);
  rows[kRowAlbedo][2] = Bits(in.wet);
  rows[kRowAlbedo][3] = uint32_t(in.view);
  for (uint32_t c = 0; c < rt_hit_cal::kClasses; ++c) {
    rows[kRowGains + c / 4][c % 4] = Bits(std::clamp(in.gains[c] > 0.0f ? in.gains[c] : 1.0f, kMinGain, kMaxGain));
  }
  rows[kRowMore][0] = Bits(1.0f);
}

// ---------------------------------------------------------------------------
// the counters (the city's own buffer, set 3 u1: the calibration's three words a class, then the census)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kCalWords = rt_hit_cal::kStatsWords;  // 48: count, the picture's summed log, the model's, for each of the 16 classes
inline constexpr uint32_t kStatTexels = kCalWords + 0;          // paint texels that cast city rays
inline constexpr uint32_t kStatRays = kCalWords + 1;            // all rays cast for them (cone and mirror)
inline constexpr uint32_t kStatCity = kCalWords + 2;            // rays whose closest hit is a city triangle
inline constexpr uint32_t kStatCar = kCalWords + 3;             // ... a car part
inline constexpr uint32_t kStatMiss = kCalWords + 4;            // ... nothing (the environment map takes them)
inline constexpr uint32_t kStatScreen = kCalWords + 5;          // city hits the picture speaks for (weight at least a half)
inline constexpr uint32_t kStatShaded = kCalWords + 6;          // ... the model does
inline constexpr uint32_t kStatLamps = kCalWords + 7;           // city hits on lamp lenses, neon signs, lit windows (class emissive)
inline constexpr uint32_t kStatNoData = kCalWords + 8;          // city hits the library could not resolve (no data in the pool): counted as misses too
inline constexpr uint32_t kStatsWords = kCalWords + 12;
inline constexpr uint32_t kStatsBytes = kStatsWords * 4;
inline constexpr uint32_t kClassEmissive = 13;                  // gbuffer_types.h SurfaceClass

// A stats frame's counters, summed over the window of a census line.
struct Totals {
  uint64_t frames = 0;
  uint64_t texels = 0, rays = 0, city = 0, car = 0, miss = 0, screen = 0, shaded = 0, lamps = 0, no_data = 0;
  uint64_t samples = 0;  // the calibration's samples (every class)
  void Add(const uint32_t words[kStatsWords]) {
    ++frames;
    texels += words[kStatTexels];
    rays += words[kStatRays];
    city += words[kStatCity];
    car += words[kStatCar];
    miss += words[kStatMiss];
    screen += words[kStatScreen];
    shaded += words[kStatShaded];
    lamps += words[kStatLamps];
    no_data += words[kStatNoData];
    for (uint32_t c = 0; c < rt_hit_cal::kClasses; ++c) {
      samples += words[rt_hit_cal::StatsWord(c, rt_hit_cal::kCountWord)];
    }
  }
};

inline constexpr uint64_t kLogEveryFrames = 120;   // the census line: every this many frames while the pass runs, and at its first stats frame

// The census lines the report reads (perf/city-rt-m2/README.md: `[city refl] frame N: level high, reach 150 m, texels T, rays R, city C, car K, miss M, screen S, shaded H, lamps L` and
// `[city refl] gain frame N: solid 1.20, ground 0.95, glass 1.10, emissive 1.40, samples 5120`). The numbers are the mean of a stats frame of the window.
inline std::string CensusText(uint64_t frame, Level level, float reach, const Totals& t) {
  const double n = double(std::max<uint64_t>(t.frames, 1));
  char buf[320];
  std::snprintf(buf, sizeof(buf), "[city refl] frame %llu: level %s, reach %.0f m, texels %.0f, rays %.0f, city %.0f, car %.0f, miss %.0f, screen %.0f, shaded %.0f, lamps %.0f",
                static_cast<unsigned long long>(frame), std::string(city_rt::LevelName(level)).c_str(), double(reach), double(t.texels) / n, double(t.rays) / n, double(t.city) / n,
                double(t.car) / n, double(t.miss) / n, double(t.screen) / n, double(t.shaded) / n, double(t.lamps) / n);
  return buf;
}
// The classes of the gain line: G-buffer classes 1 (world) for the solid, 2 (road) for the ground, 11 (glass), 13 (emissive).
inline constexpr uint32_t kGainClassSolid = 1, kGainClassGround = 2, kGainClassGlass = 11, kGainClassEmissive = 13;
inline std::string GainText(uint64_t frame, const rt_hit_cal::ClassGains& g, uint64_t samples) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "[city refl] gain frame %llu: solid %.2f, ground %.2f, glass %.2f, emissive %.2f, samples %llu", static_cast<unsigned long long>(frame), double(g.Gain(kGainClassSolid)),
                double(g.Gain(kGainClassGround)), double(g.Gain(kGainClassGlass)), double(g.Gain(kGainClassEmissive)), static_cast<unsigned long long>(samples));
  return buf;
}

}  // namespace mcla::gpu::city_refl
