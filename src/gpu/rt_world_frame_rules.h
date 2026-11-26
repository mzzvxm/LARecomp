#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing WORLD FRAME: the rules (CITY-RT package FRAME; the public header is rt_world_frame.h, the shader side hlsl/rt_world_frame.hlsli)
// ===========================================================================
// WHAT THIS IS. The numbers every ray traced consumer of the WORLD needs in a frame, whether or not the player's car is in it: the lighting of the world as the camera's draws saw it, the
// camera, the sun, the exposure, the time of day, the weather, the hit shading's calibration, and the lamps near the camera with a grid over them. rt_hit_frame.h already defines the first
// half of it (32 rows: the sun, the ambient, the environment map, the game's sun cascades) but only the car scene filled them, from the car's own draws; a top level with no car had one empty
// row (OnResolveCityOnly), so the city hit library (hlsl/city_hit_shade.hlsli) answered "no data" there. This file is the PURE half of what fills them every frame: the layout of the two
// buffers, their packing, the lamp schedule (the on hours and the night curve), the merge of the static lamp table with the game's live list, the budget, the 64 m grid and the lookup the
// shader does, as a C++ twin. Nothing here includes the renderer (rt_world_frame_test.cpp builds alone); hlsl/rt_world_frame.hlsli holds the same numbers and is compiled as C++ by the test
// and compared with this file.
//
// TWO BUFFERS, both StructuredBuffer<uint4>, one copy a frame slot, written by the host before the frame's passes (host visible, no copy pass):
//
//   gRtWorld  (t10)   kTotalRows rows (768 bytes)
//       rows 0..31   the HIT FRAME: exactly rt_hit_frame.h's kFrameRows, so `RT_HIT_FRAME_ROW(i)` = `gRtWorld[i]` makes hlsl/rt_hit_frame.hlsli (RtHitLoadFrame) and every city hit
//                    library function read the world's light unchanged. The sun, the ambient and the cascades are the WORLD'S (the commonest light of the main view's draws: the sun's colour
//                    and the ambient as the buildings and the road were lit; a car's own draws carry a light of their own), not the car's.
//       rows 32..47  the WORLD ROWS (below)
//   gRtLamps  (t11)   the lamps of this frame in VIEW space, sorted by cell, with the cells' ranges: row 0 a header, then the cell table (two cells a row), then three rows a lamp
//
// THE WORLD ROWS (32 + n)
//   32  x frame serial (low 32 bits of the scene's frame)       y flags (kWorld*)       z hour (float, 0..24; -1: unknown)      w lamp schedule (float 0..1, see LAMPS)
//   33  x units (float: 2^exposure bias: the paint's units to the picture's)   y wetness (float 0 dry .. 1 rain)   z weather index (uint 0 nice, 1 cloudy, 2 stormy, 3 foggy; ~0: unknown)
//       w scene clock in seconds (float)
//   34  xyz the camera's place in the game's world (float)      w the world sun's night factor (rt_hit::NightFactor of the world's sun colour: informational, NOT the lamps')
//   35  xyz the direction to the sun in WORLD space (unit; 0 when kWorldSunWorld is clear)      w 0
//   36  x grid origin x (float, 0)   y origin z (0)   z the cell's side in metres (64)   w the lookup radius a consumer takes by default (metres, 64)
//   37  x the window's first cell x (float, an integer)   y its first cell z   z cells across (uint)   w cells deep (uint)
//   38  x lamps uploaded (uint)   y lamps in the window before the budget cut (uint)   z cells (nx x nz)   w lamps the game's live list gave this frame
//   39  x 1 in a calibration frame (the shader sets CITY_HIT_P_CALIBRATE)   y calibration windows closed so far   z reserved   w the world kind (0 none, 1 play, 2 pause, 3 garage, 4 map)
//   40..43  the class gains, 16 floats, class c in row 40 + c / 4 component c % 4 (rt_hit_calibration.h ClassGains::Table; 1 until a window has moved one; the glowing class' range is wider)
//   44  x the EMITTERS uploaded (uint)   y the row of gRtLamps the first one is at (uint)   z how many of them are static (the first z: the probe field reads only these)   w the groups' mask (rt_emitters_rules.h)
//       [GI-BOUNCE-RT]: the cars' lamps and the other lights of the light pass's classes, as GpuLight rows (four a light, WORLD space) after the lamps' rows of gRtLamps; 0 emitters: no row is read
//   45..46  reserved (zero)
//   47  x the street lamps' schedule (float 0..1: what a lamp lens glows by, kGlowLens)   y the fill lights' night factor (float 0..1: what neon, signs and lit windows glow by, kGlowNight)
//       z the game's live lamp list's lamps that shine now (uint)   w flags (kEvidence*: the live list was read as evidence of the lamps being on or off)    [CITY-RT R7]
// The hit frame's flags (row 0 w: kFrameShade ...) say what of the light is valid; kWorldValid says the rows were filled this frame at all. A consumer reads kWorldValid first: clear, and
// every other number is stale or zero (the slot's buffer is cleared when it is bound and not filled that frame).
//
// THE LAMPS (CITY-RT-M3 G2b's table, the game's laws; src/citypack/lamp_table.h says which list holds what). The frame's lamp set is the union of
//   * the STATIC table's lights within the budget's radius of the camera that shine at this hour: the pack's lights.xlm fill lights (15,174) and, when the owner of the table merged a live
//     read into it (lamp_merge.h MergeLive), the street lamps (6,298) too;
//   * the game's LIVE per-frame list (vol_lamp_list.h: the street lamps within 300 m of the eye, with the prop pass's own fade): used as it is, and, while it is fresh, it REPLACES the table's
//     street lamps in its reach (SelectLamps' rule: the game's list says which of them are on and how bright; the table says nothing else about them).
// Every light the frame keeps is a record the library reads (city_hit_shade_rules.h MakeGpuLamp: view space, the game's units), with the colour ALREADY times the lamp's activity over the
// frame's schedule, so that the library's `colour x p.lampFactor` with p.lampFactor = the schedule (row 32 w) is colour x activity: a street lamp is 0 or 1 by the prop pass's hours (the
// live list: its scale), a fill light follows the night curve; a lamp with no activity is not uploaded at all.
//   THE SCHEDULE = max(the fraction of the street lamps that are on, the fill lights' night factor), 0 by day, 1 at night, smooth over dusk and dawn: the mean of the prop pass's per lamp rule
//   `hour > on - c - j or hour < off + c + j` over its 16 jitter steps j (0 to 0.25 h), and lamp_table.h EstimateNightFactor (a 0.5 h ramp) for the fill lights; the lamp hours are the table's
//   (18 and 6, the TODSunNice values: the live floats are the truth, CHECK). Without an hour (the clock is not up yet) it is the world sun's night factor. The schedule is ALSO what the emissive
//   parts (lamp lenses, neon, lit windows) glow by (CityHitParams::lampFactor), so the whole city comes on together.
//   THE LIVE EVIDENCE (R7; "the schedule by hour and weather"). The hour is the prop pass's rule with the lamp hours of the table (18 and 6, the TODSunNice values); the game loads the lamp
//   hours from the active time-of-day set, so a stormy or foggy sky can have other ones, and the weather's own effect on the street lamps is in the game's per frame list: the list is made by the prop
//   pass with the live hours and the lamps' fade. So a fresh list is the cross-check: with lamps that shine in it the street lamps ARE on (ApplyLiveEvidence: the share of the table's street lamps in
//   the list's reach that shine, times their mean fade; by day, in a storm, that alone), with no lamp in it while the static table has street lamps within the list's reach (a merged table:
//   lamp_merge.h) they are OFF (the schedule's street part is 0), and with neither (no fresh list, or no street lamp near enough to say) the hour decides. The fill lights keep the night curve of
//   the hour (the game's ShaderGlobals.w: CHECK against a stormy day). The street part and the fill part are separate numbers in row 47: a lamp lens glows by the street lamps' schedule, neon,
//   signs and lit windows by the fill lights' (a traffic signal always glows). CHECK: at dusk a lens glows at the FRACTION of lamps that are on, where in the game each lamp is on or off.
//   THE GRID: 64 m cells over x and z (absolute: cell = floor(x / 64)), a WINDOW of at most 16 x 16 cells around the camera that holds the budget's radius; a lamp outside it is dropped. The
//   shader finds the lamps near a point by the cells that cover [p - r, p + r]: at most 5 cells an axis, a cell's lamps contiguous. `r` defaults to 64 m: a lamp of 20 (a fill light) reaches 44.7 m
//   where its term is 1 per cent, a street lamp of 70 reaches 84 m, of 100 reaches 100 m: a radius of 100 takes every lamp that matters at the cost of 16 cells instead of 9.
//   THE BUDGET (BudgetFor): the nearest `max_lamps` to the camera are kept when more shine (High 512 lamps in 320 m, Ultra 1024 in 448 m, the smallest 256 in 192 m).
// gRtLamps layout (rows): 0 {lamps uploaded, cells, the row the lamps start at, serial}; 1.. the cell table, two cells a row: row 1 + c / 2 holds (first, count) of cell 2 (c / 2) in x, y
// and of cell 2 (c / 2) + 1 in z, w, where `first` is a LAMP index and cells run row by row (c = cz * nx + cx, window relative); then lamp i at rows base + 3 i .. base + 3 i + 2; then the EMITTERS
// (row 44 of gRtWorld says where): emitter i at rows e + 4 i .. e + 4 i + 3, a GpuLight each (light_frame.h; the static ones first).
//
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "citypack/lamp_merge.h"
#include "citypack/lamp_table.h"
#include "city_hit_shade_rules.h"
#include "rt_hit_calibration.h"
#include "rt_hit_frame.h"
#include "vol_lamp_list.h"

namespace mcla::gpu::rt_world {

namespace ch = larecomp::citypack;

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHitRows = rt_hit::kFrameRows;  // 32: rows 0..31 are rt_hit_frame.h's
inline constexpr uint32_t kRowStatus = 32;
inline constexpr uint32_t kRowLight = 33;
inline constexpr uint32_t kRowCamera = 34;
inline constexpr uint32_t kRowSunWorld = 35;
inline constexpr uint32_t kRowGrid = 36;
inline constexpr uint32_t kRowWindow = 37;
inline constexpr uint32_t kRowCounts = 38;
inline constexpr uint32_t kRowCalibration = 39;
inline constexpr uint32_t kRowGains = 40;  // 40..43
inline constexpr uint32_t kRowEmitters = 44;  // GI-BOUNCE-RT: the emitters' count, row, static count, groups
inline constexpr uint32_t kRowSchedule = 47;  // R7: the street lamps' and the fill lights' schedules, the live evidence
inline constexpr uint32_t kTotalRows = 48;
inline constexpr uint32_t kRowBytes = 16;
inline constexpr uint32_t kWorldBytes = kTotalRows * kRowBytes;
inline constexpr uint32_t kClasses = rt_hit_cal::kClasses;  // 16: the G-buffer's classes

// the registers in the ray tracing descriptor set (hlsl/rt_world_frame.hlsli), after the car scene's t0..t5 and the hit data's t6..t9, and the views they add to the set
inline constexpr uint32_t kWorldRegister = 10;
inline constexpr uint32_t kLampsRegister = 11;
inline constexpr uint32_t kBindingCount = 2;

// flags (row 32 y)
inline constexpr uint32_t kWorldValid = 1;        // the rows were filled this frame
inline constexpr uint32_t kWorldHour = 2;         // the hour is known (the clock is up)
inline constexpr uint32_t kWorldCameraKnown = 4;  // the camera's place in the game's world is valid (a view was given)
inline constexpr uint32_t kWorldSunWorld = 8;     // the direction to the sun in the world's space is valid
inline constexpr uint32_t kWorldLiveFresh = 16;   // the game's live lamp list was fresh this frame
inline constexpr uint32_t kWorldLamps = 32;       // gRtLamps holds this frame's window (it may hold no lamp: day)
inline constexpr uint32_t kWorldStatsFrame = 64;  // a calibration frame: the shader adds its samples to the counters
inline constexpr uint32_t kWorldCar = 128;        // the player's car is in this frame's top level (informational)
inline constexpr uint32_t kWorldLight = 256;      // the world's light was read from the main view's draws (else the car's or none: rt_hit_frame.h's flags say what shades)

inline constexpr uint32_t kUnknownWeather = 0xFFFFFFFFu;

// the grid
inline constexpr float kCellMetres = 64.0f;
inline constexpr uint32_t kMaxWindowAxis = 16;                      // cells across and deep at most
inline constexpr uint32_t kMaxWindowCells = kMaxWindowAxis * kMaxWindowAxis;
inline constexpr uint32_t kMaxLookupAxis = 5;                       // a lookup reads at most this many cells an axis
inline constexpr float kDefaultLookupMetres = 64.0f;
inline constexpr float kMaxLookupCells = 2.0f;                      // the radius is clamped to this many cell sides
inline constexpr uint32_t kCellRowsMax = kMaxWindowCells / 2;       // two cells a row
inline constexpr uint32_t kLampRowsEach = 3;
inline constexpr uint32_t kMaxLampsHard = 2048;                     // the buffer's room (the budgets stay below)
inline constexpr uint32_t kEmitterRows = 4;                         // GI-BOUNCE-RT: a GpuLight (rt_emitters_rules.h kRowsEach)
inline constexpr uint32_t kMaxEmitters = 32;                        // ... and the most of them (rt_emitters_rules.h kMaxEmitters; a test holds them equal)

// the calibration's cadence: one frame in this many is a stats frame (rt_hit_calibration.h: the counters are atomics)
inline constexpr uint32_t kStatsPeriod = 20;
inline constexpr uint32_t kWindowFrames = 60;                       // the host closes a calibration window this often (frames)
constexpr bool StatsFrameDue(uint64_t frame) { return frame % kStatsPeriod == 0; }

// the numbers of the exposure and the wetness
inline constexpr int32_t kMainTargetExpBias = 4;                    // the game's main colour target's exponent bias
inline constexpr float kDefaultUnits = 16.0f;                       // 2^4
inline constexpr float kWetDry = 0.095f;                            // the road pixel shader's wetness constant, dry ...
inline constexpr float kWetRain = 1.17f;                            // ... and in rain (VS c43.x, ao.h)
inline float UnitsOf(float exposure_factor) { return std::isfinite(exposure_factor) && exposure_factor > 1.0001f ? exposure_factor : kDefaultUnits; }
inline float WetOf(float raw) { return std::isfinite(raw) ? std::clamp((raw - kWetDry) / (kWetRain - kWetDry), 0.0f, 1.0f) : 0.0f; }

// ---------------------------------------------------------------------------
// the lamp schedule: the on hours and the night curve
// ---------------------------------------------------------------------------

inline constexpr uint32_t kJitterSteps = 16;  // lamp_table.h LampJitterHours: (z & 0xF) x 16 steps of kLampJitterHoursPerStep

// The fraction of the street lamps that are on at `hour`: the mean of the prop pass's rule over its sixteen jitter values (every one is as likely: the jitter is a hash).
inline float StreetFraction(const ch::LampHours& hours, float hour) {
  uint32_t on = 0;
  for (uint32_t s = 0; s < kJitterSteps; ++s) {
    const float jitter = float(s * 16u) * ch::kLampJitterHoursPerStep;
    on += ch::StreetLampOn(hours, hour, jitter) ? 1u : 0u;
  }
  return float(on) / float(kJitterSteps);
}

struct Schedule {
  bool from_hour = false;  // the hour was known; else both numbers are the night hint
  float street = 0.0f;     // the fraction of street lamps on
  float fill = 0.0f;       // the fill lights' night factor
  float factor = 0.0f;     // max of the two: what the library's lampFactor takes (the lamps' own colours already hold their activity over it)
  uint32_t evidence = 0;   // kEvidence*: what the game's live list said about the street lamps (R7)
  float hour_street = 0.0f;  // the street part as the hour (or the hint) gave it, before the evidence
};

// What the game's live lamp list says about the street lamps (CITY-RT R7). The list is made by the prop pass, with the live on hours and the lamps' fade, so it knows the weather's effect on them.
inline constexpr uint32_t kEvidenceOn = 1;   // a fresh list had lamps that shine: the street lamps are on
inline constexpr uint32_t kEvidenceOff = 2;  // a fresh list had none while the table has street lamps in its reach: they are off
inline constexpr uint32_t kEvidenceMinTableStreets = 3;  // street lamps the table must have within the list's reach before an empty list says "off" (a list is empty where no lamp is near)
struct LiveEvidence {
  bool fresh = false;          // the list was copied recently
  uint32_t given = 0;          // its entries
  uint32_t shining = 0;        // the entries that are on with a fade over 0 and an intensity
  float mean_scale = 0.0f;     // the mean fade (0..1) of those
  uint32_t table_streets = 0;  // the static table's street lamps within the list's reach (300 m) of the eye; 0 for a table without street lamps (they come with a merge)
};

// `hour` outside 0..24 or not a number: unknown. `night_hint` (the world sun's night factor, < 0: none) stands in for both then.
inline Schedule ScheduleAt(const ch::LampHours& hours, float hour, float night_hint) {
  Schedule s;
  if (std::isfinite(hour) && hour >= 0.0f && hour < 24.0f) {
    s.from_hour = true;
    s.street = StreetFraction(hours, hour);
    s.fill = ch::EstimateNightFactor(hours, hour);
  } else if (std::isfinite(night_hint) && night_hint >= 0.0f) {
    s.street = s.fill = std::clamp(night_hint, 0.0f, 1.0f);
  }
  s.hour_street = s.street;
  s.factor = std::max(s.street, s.fill);
  return s;
}

// The schedule corrected by the game's list (from the hour's own street part, so that applying it twice is the same as once). Lamps that shine in a fresh list are ON: where the table says how many
// street lamps there are in the list's reach the street part is the share of them that shine times their mean fade (the dusk's staircase as the game has it), at least the hour's; without
// that count it is the hour's where the hour says some are on, else the mean fade (all of them, by day, in a storm). An empty fresh list next to a table that has street lamps in the reach says they
// are OFF (the street part is 0). Anything else leaves the hour's. The fill lights are the hour's either way. `factor` follows.
inline Schedule ApplyLiveEvidence(Schedule s, const LiveEvidence& e) {
  s.evidence = 0;
  s.street = s.hour_street;
  const bool table_known = e.table_streets >= kEvidenceMinTableStreets;
  if (e.fresh && e.shining > 0) {
    s.evidence = kEvidenceOn;
    const float mean = std::clamp(e.mean_scale, 0.0f, 1.0f);
    const float from_list = table_known ? std::min(1.0f, float(e.shining) / float(e.table_streets)) * mean : mean;
    s.street = s.hour_street > 0.0f ? std::max(s.hour_street, table_known ? from_list : 0.0f) : from_list;
  } else if (e.fresh && e.shining == 0 && table_known && s.hour_street > 0.0f) {
    s.evidence = kEvidenceOff;
    s.street = 0.0f;
  }
  s.factor = std::max(s.street, s.fill);
  return s;
}
// The evidence of a list of the game's lamps (the entries as LiveLampOf made them).
template <class Lamps>
inline LiveEvidence EvidenceOf(bool fresh, const Lamps& live, uint32_t table_streets) {
  LiveEvidence e;
  e.fresh = fresh;
  e.table_streets = table_streets;
  double scale_sum = 0.0;
  for (const auto& g : live) {
    ++e.given;
    if (g.lamp.on && g.scale > 0.0f && g.lamp.intensity > 0.0f) {
      ++e.shining;
      scale_sum += std::min(g.scale, 1.0f);
    }
  }
  e.mean_scale = e.shining != 0 ? float(scale_sum / double(e.shining)) : 0.0f;
  return e;
}

// How much one lamp of the static table shines in this schedule: a street lamp by the prop pass's own rule (0 or 1) unless the game's live list said otherwise (evidence: off is 0; on, where the
// hour says all are off, is the street part), every other light by the night factor; the light's own on byte first.
inline float ActivityOf(const ch::Lamp& lamp, const Schedule& s, const ch::LampHours& hours, float hour) {
  if (!lamp.on) {
    return 0.0f;
  }
  const bool street = lamp.source == ch::LampSource::kStreetLamp;
  if (street && (s.evidence & kEvidenceOff) != 0) {
    return 0.0f;
  }
  float activity = 0.0f;
  if (!s.from_hour) {
    activity = street ? s.street : s.fill;
  } else {
    activity = ch::LampActivity(lamp, hours, hour, s.fill);
  }
  if (street && (s.evidence & kEvidenceOn) != 0 && s.hour_street <= 0.0f) {
    activity = s.street;  // the hour says every street lamp is off and the game's list says they shine: no per lamp rule to follow, the street part for all of them
  }
  return activity;
}

// ---------------------------------------------------------------------------
// the budget
// ---------------------------------------------------------------------------

enum class Tier : uint8_t { kMid = 0, kHigh = 1, kUltra = 2 };
struct LampBudget {
  uint32_t max_lamps = 512;
  float radius_m = 320.0f;  // lamps beyond this (3-D) from the camera are not taken
};
constexpr LampBudget BudgetFor(Tier tier) {
  switch (tier) {
    case Tier::kMid: return {256, 192.0f};
    case Tier::kHigh: return {512, 320.0f};
    case Tier::kUltra: return {1024, 448.0f};
  }
  return {512, 320.0f};
}

// ---------------------------------------------------------------------------
// the game's live list as lamps
// ---------------------------------------------------------------------------

// A street lamp of the game's per-frame list: the lamp (a street lamp, in the game's units) and the prop pass's fade (0..1: by distance and by the lamps' hours).
struct LiveLamp {
  ch::Lamp lamp;
  float scale = 1.0f;
};
inline LiveLamp LiveLampOf(const vol_lamps::GameLamp& g) {
  LiveLamp out;
  ch::Lamp& l = out.lamp;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = g.pos[i];
    l.colour[i] = g.colour[i];
    l.dir[i] = g.dir[i];
  }
  l.intensity = g.intensity;
  l.cone_deg = g.cone_deg;
  l.spread_deg = g.spread_deg;
  l.spot = g.type != 0 ? 1 : 0;
  l.on = g.on != 0 ? 1 : 0;
  l.source = ch::LampSource::kStreetLamp;
  out.scale = g.scale;
  return out;
}

// ---------------------------------------------------------------------------
// the frame's lamps
// ---------------------------------------------------------------------------

// What the game's per frame list covers around the eye (the prop pass's AddLight): the table's street lamps within it are the ones the list speaks for.
inline constexpr float kLiveListReach = 300.0f;

struct LampInput {
  const ch::LampTable* table = nullptr;           // the static table (null: only the live list)
  const std::vector<LiveLamp>* live = nullptr;     // the game's list of this frame (null: none)
  bool live_fresh = false;                         // the list was copied recently: it replaces the table's street lamps in its reach
  float live_radius_m = kLiveListReach;            // what the game's list covers (the prop pass's AddLight)
  double camera[3] = {0, 0, 0};                    // the camera's place in the game's world
  float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // the world's axes in view space (rt_hit::FrameInput::world_axes_view)
  float hour = -1.0f;
  Schedule schedule;
  LampBudget budget;
  bool track_probe_lamps = false;
};

struct LampStats {
  uint32_t from_table = 0;      // lamps the table gave within the radius that shine
  uint32_t from_live = 0;       // lamps of the live list that shine
  uint32_t inactive = 0;        // lights within the radius with no activity (off at this hour, or the game's fade 0)
  uint32_t over_budget = 0;     // dropped for the budget (the farthest)
  uint32_t outside_window = 0;  // dropped for the window's size
  uint32_t kept = 0;
  uint32_t live_given = 0;      // the live list's size
};

// What the frame's lamp buffer holds, on the host as the words gRtLamps has, and the window the rows 36 to 38 describe.
struct LampFrame {
  struct ProbeLamp {
    uint64_t key;
    float position[3];
    float reach;
  };
  float cell_m = kCellMetres;
  int32_t cx0 = 0;
  int32_t cz0 = 0;
  uint32_t nx = 0;
  uint32_t nz = 0;
  uint32_t lamp_count = 0;
  uint32_t lamp_row_base = 1;
  LampStats stats;
  std::vector<uint32_t> words;  // 4 per row
  std::vector<ProbeLamp> probe_lamps;  // selected lamps in world space for GI invalidation
  // GI-BOUNCE-RT: the emitters appended after the lamps' rows (AppendEmitters): how many, the row the first is at, how many of them are static
  uint32_t emitter_count = 0;
  uint32_t emitter_row_base = 0;
  uint32_t emitter_static = 0;
  uint32_t emitter_groups = 0;

  uint32_t cells() const { return nx * nz; }
  uint32_t rows() const { return uint32_t(words.size() / 4); }
};

// Appends the emitters (`count` GpuLights as kEmitterRows x 4 words each, the first `static_count` of them static) after the rows the lamps end at. More than kMaxEmitters are cut. The lamps' rows are not touched
// (a lamp's row is found by the cell table, so the rows after the last lamp are free): the emitters' row is where the words ended.
inline void AppendEmitters(LampFrame* f, const uint32_t* words, uint32_t count, uint32_t static_count, uint32_t groups) {
  count = std::min(count, kMaxEmitters);
  f->emitter_count = count;
  f->emitter_static = std::min(static_count, count);
  f->emitter_groups = groups;
  f->emitter_row_base = 0;
  if (count == 0) {
    return;
  }
  f->emitter_row_base = f->rows();
  f->words.insert(f->words.end(), words, words + size_t(count) * kEmitterRows * 4u);
}

inline constexpr uint32_t CellRowsFor(uint32_t cells) { return (cells + 1) / 2; }
inline constexpr uint32_t MaxLampRows(uint32_t max_lamps) { return 1 + kCellRowsMax + kLampRowsEach * max_lamps; }
inline constexpr uint32_t kLampBufferBytes = (MaxLampRows(kMaxLampsHard) + kEmitterRows * kMaxEmitters) * kRowBytes;

inline int32_t CellIndexOf(float v) { return int32_t(std::floor(double(v) / double(kCellMetres))); }

// The record of a light of the table (or of the game's list) in view space with its colour times `weight`, the library's GPU record (city_hit_shade_rules.h MakeGpuLamp).
inline void MakeLampRecord(const ch::Lamp& lamp, float weight, const double camera[3], const float axes[3][3], uint32_t rows[3][4]) {
  city_hit_shade::WorldLamp w;
  for (int i = 0; i < 3; ++i) {
    w.position[i] = lamp.pos[i];
    w.colour[i] = lamp.colour[i] * weight;
    w.axis[i] = lamp.dir[i];
  }
  w.intensity = lamp.intensity;
  const float axis_length = std::sqrt(lamp.dir[0] * lamp.dir[0] + lamp.dir[1] * lamp.dir[1] + lamp.dir[2] * lamp.dir[2]);
  if (!lamp.is_spot() || lamp.cone_deg >= 359.0f || axis_length <= 1e-3f) {
    w.cone_cos_outer = w.cone_cos_inner = -2.0f;  // no cone (the library's rule is "outer below -1.5"; LampConeCosines' -1 and -1 would divide by zero)
  } else {
    ch::LampConeCosines(lamp.cone_deg, lamp.spread_deg, &w.cone_cos_outer, &w.cone_cos_inner);
  }
  const float cam[3] = {float(camera[0]), float(camera[1]), float(camera[2])};
  city_hit_shade::MakeGpuLamp(w, cam, axes, rows);
}

namespace detail {
struct Candidate {
  const ch::Lamp* lamp = nullptr;
  float activity = 0.0f;
  float distance2 = 0.0f;
  uint64_t order = 0;  // the tie break: the table's index; a live lamp's place key (lamp_table.h LampKeyOf) above every table index, so that the order of the game's list does not matter
  int32_t cx = 0, cz = 0;
};
inline float Distance2(const ch::Lamp& l, const double camera[3]) {
  const double dx = double(l.pos[0]) - camera[0], dy = double(l.pos[1]) - camera[1], dz = double(l.pos[2]) - camera[2];
  return float(dx * dx + dy * dy + dz * dz);
}
}  // namespace detail

// Makes the frame's lamp buffer: the static table's lamps and the live list's merged by the rule in the comment at the top, the activity folded into the colour, the nearest `max_lamps`, a
// window of cells around the camera, the lamps sorted into it. Deterministic: the same input gives the same words. With a schedule of 0 (day) the buffer holds the window and no lamp.
inline void BuildLampFrame(const LampInput& in, LampFrame* out) {
  *out = LampFrame{};
  LampFrame& f = *out;
  const LampBudget budget = {std::min(in.budget.max_lamps, kMaxLampsHard), std::max(in.budget.radius_m, 1.0f)};
  const ch::LampHours default_hours;
  const ch::LampHours& hours = in.table != nullptr ? in.table->hours() : default_hours;

  // the window: the cells that cover the budget's radius around the camera, at most kMaxWindowAxis an axis (centred on the camera's cell where it is more)
  const float radius = budget.radius_m;
  const auto axis_window = [&](double centre, int32_t* first, uint32_t* count) {
    int32_t a = CellIndexOf(float(centre - double(radius)));
    int32_t b = CellIndexOf(float(centre + double(radius)));
    if (b - a + 1 > int32_t(kMaxWindowAxis)) {
      const int32_t mid = CellIndexOf(float(centre));
      a = mid - int32_t(kMaxWindowAxis / 2) + 1;
      b = a + int32_t(kMaxWindowAxis) - 1;
    }
    *first = a;
    *count = uint32_t(b - a + 1);
  };
  axis_window(in.camera[0], &f.cx0, &f.nx);
  axis_window(in.camera[2], &f.cz0, &f.nz);
  f.cell_m = kCellMetres;

  std::vector<detail::Candidate> candidates;
  std::vector<ch::Lamp> live_lamps;  // the game's lamps with the activity folded in: kept alive until the words are made
  const float factor = in.schedule.factor;
  const bool shining = factor > 1e-3f;

  if (in.table != nullptr && !in.table->empty() && shining) {
    ch::LampSelection selection;
    for (int i = 0; i < 3; ++i) selection.eye[i] = float(in.camera[i]);
    selection.radius_m = radius;
    selection.live_list_fresh = in.live_fresh;
    selection.live_radius_m = in.live_radius_m;
    selection.hour = -1.0f;  // every light: the activity is judged here (it can come from a hint with no hour)
    std::vector<uint32_t> picked;
    ch::SelectLamps(*in.table, selection, &picked);
    for (const uint32_t index : picked) {
      const ch::Lamp& lamp = (*in.table)[index];
      const float activity = ActivityOf(lamp, in.schedule, hours, in.hour);
      if (!(activity > 0.0f)) {
        ++f.stats.inactive;
        continue;
      }
      detail::Candidate c;
      c.lamp = &lamp;
      c.activity = activity;
      c.distance2 = detail::Distance2(lamp, in.camera);
      c.order = index;
      candidates.push_back(c);
      ++f.stats.from_table;
    }
  }
  if (in.live != nullptr) {
    f.stats.live_given = uint32_t(in.live->size());
    if (shining) {
      live_lamps.reserve(in.live->size());
      for (size_t i = 0; i < in.live->size(); ++i) {
        const LiveLamp& g = (*in.live)[i];
        if (!g.lamp.on || !(g.scale > 0.0f) || !(g.lamp.intensity > 0.0f)) {
          ++f.stats.inactive;
          continue;
        }
        if (detail::Distance2(g.lamp, in.camera) > radius * radius) {
          continue;
        }
        live_lamps.push_back(g.lamp);
        detail::Candidate c;
        c.activity = std::min(g.scale, 1.0f);
        c.distance2 = detail::Distance2(g.lamp, in.camera);
        c.order = (uint64_t(1) << 63) | (ch::LampKeyOf(g.lamp) >> 1);
        candidates.push_back(c);
        ++f.stats.from_live;
      }
      // the live lamps' pointers now that the vector will not move
      size_t at = 0;
      for (detail::Candidate& c : candidates) {
        if (c.lamp == nullptr) c.lamp = &live_lamps[at++];
      }
    }
  }

  // the cells, the window's rule, the budget's cut
  for (detail::Candidate& c : candidates) {
    c.cx = CellIndexOf(c.lamp->pos[0]);
    c.cz = CellIndexOf(c.lamp->pos[2]);
  }
  {
    size_t kept = 0;
    for (const detail::Candidate& c : candidates) {
      const int32_t x = c.cx - f.cx0, z = c.cz - f.cz0;
      if (x < 0 || z < 0 || x >= int32_t(f.nx) || z >= int32_t(f.nz)) {
        ++f.stats.outside_window;
        continue;
      }
      candidates[kept++] = c;
    }
    candidates.resize(kept);
  }
  if (candidates.size() > budget.max_lamps) {
    std::sort(candidates.begin(), candidates.end(), [](const detail::Candidate& a, const detail::Candidate& b) {
      return a.distance2 != b.distance2 ? a.distance2 < b.distance2 : a.order < b.order;
    });
    f.stats.over_budget = uint32_t(candidates.size()) - budget.max_lamps;
    candidates.resize(budget.max_lamps);
  }
  std::sort(candidates.begin(), candidates.end(), [&](const detail::Candidate& a, const detail::Candidate& b) {
    const int32_t ca = (a.cz - f.cz0) * int32_t(f.nx) + (a.cx - f.cx0), cb = (b.cz - f.cz0) * int32_t(f.nx) + (b.cx - f.cx0);
    return ca != cb ? ca < cb : a.order < b.order;
  });
  f.stats.kept = uint32_t(candidates.size());
  f.lamp_count = f.stats.kept;
  if (in.track_probe_lamps) f.probe_lamps.reserve(f.lamp_count);

  // the words
  const uint32_t cells = f.nx * f.nz;
  f.lamp_row_base = 1 + CellRowsFor(cells);
  f.words.assign(size_t(f.lamp_row_base + kLampRowsEach * f.lamp_count) * 4u, 0u);
  std::vector<uint32_t> first(cells, 0), count(cells, 0);
  for (uint32_t i = 0; i < f.lamp_count; ++i) {
    const detail::Candidate& c = candidates[i];
    const uint32_t cell = uint32_t((c.cz - f.cz0) * int32_t(f.nx) + (c.cx - f.cx0));
    if (count[cell] == 0) first[cell] = i;
    ++count[cell];
    uint32_t rows[3][4];
    MakeLampRecord(*c.lamp, c.activity / factor, in.camera, in.axes, rows);
    std::memcpy(&f.words[size_t(f.lamp_row_base + kLampRowsEach * i) * 4u], rows, sizeof(rows));
    if (in.track_probe_lamps) {
      const ch::Lamp& lamp = *c.lamp;
      // Distance fade and camera transforms change as the view moves. Keep the
      // lamp's identity and intrinsic light properties for local invalidation.
      uint64_t key = ch::LampKeyOf(lamp);
      const auto add = [&](float value) {
        key = (key ^ rt_hit::Bits(value)) * 1099511628211ull;
      };
      for (float value : lamp.colour) add(value);
      for (float value : lamp.dir) add(value);
      add(lamp.intensity);
      add(lamp.cone_deg);
      add(lamp.spread_deg);
      add(float(lamp.spot));
      f.probe_lamps.push_back({key, {lamp.pos[0], lamp.pos[1], lamp.pos[2]}, ch::LampReach(lamp.intensity)});
    }
  }
  f.words[0] = f.lamp_count;
  f.words[1] = cells;
  f.words[2] = f.lamp_row_base;
  for (uint32_t c = 0; c < cells; ++c) {
    const size_t at = size_t(1 + c / 2) * 4u + size_t(c % 2) * 2u;
    f.words[at] = first[c];
    f.words[at + 1] = count[c];
  }
}

// ---------------------------------------------------------------------------
// the world rows
// ---------------------------------------------------------------------------

struct WorldInput {
  uint64_t frame = 0;
  rt_hit::FrameInput hit;                     // the world's light, the sun, the cascades, the axes: rows 0..31
  float hour = -1.0f;
  uint32_t weather = kUnknownWeather;
  float units = kDefaultUnits;
  float wet = 0.0f;                           // 0..1
  float time_s = 0.0f;
  bool camera_known = false;
  float camera[3] = {0, 0, 0};
  bool sun_world_known = false;
  float to_sun_world[3] = {0, 1, 0};
  bool world_light = false;                   // `hit.light` is the world's (kWorldLight)
  bool car = false;
  bool live_fresh = false;
  bool stats_frame = false;
  uint32_t calibration_windows = 0;
  uint32_t world_kind = 0;
  Schedule schedule;
  uint32_t live_shining = 0;                  // the game's live list's lamps that shine (LiveEvidence::shining)
  const LampFrame* lamps = nullptr;           // null: no lamp window (kWorldLamps clear)
  // GI-BOUNCE-RT: the emitters (row 44); independent of the lamp window (a car's headlights shine by day, in a frame with no lamp)
  uint32_t emitter_count = 0, emitter_row_base = 0, emitter_static = 0, emitter_groups = 0;
  float gains[kClasses] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
};

inline void MakeWorldRows(const WorldInput& in, uint32_t rows[kTotalRows][4]) {
  std::memset(rows, 0, size_t(kTotalRows) * kRowBytes);
  uint32_t hit_rows[rt_hit::kFrameRows][4];
  rt_hit::MakeFrameRows(in.hit, hit_rows);
  std::memcpy(rows, hit_rows, sizeof(hit_rows));

  const bool hour_known = in.schedule.from_hour;
  uint32_t flags = kWorldValid;
  flags |= hour_known ? kWorldHour : 0u;
  flags |= in.camera_known ? kWorldCameraKnown : 0u;
  flags |= in.sun_world_known ? kWorldSunWorld : 0u;
  flags |= in.live_fresh ? kWorldLiveFresh : 0u;
  flags |= in.lamps != nullptr ? kWorldLamps : 0u;
  flags |= in.stats_frame ? kWorldStatsFrame : 0u;
  flags |= in.car ? kWorldCar : 0u;
  flags |= in.world_light ? kWorldLight : 0u;
  rows[kRowStatus][0] = uint32_t(in.frame & 0xFFFFFFFFu);
  rows[kRowStatus][1] = flags;
  rows[kRowStatus][2] = rt_hit::Bits(hour_known ? in.hour : -1.0f);
  rows[kRowStatus][3] = rt_hit::Bits(in.schedule.factor);

  rows[kRowLight][0] = rt_hit::Bits(in.units);
  rows[kRowLight][1] = rt_hit::Bits(std::clamp(in.wet, 0.0f, 1.0f));
  rows[kRowLight][2] = in.weather;
  rows[kRowLight][3] = rt_hit::Bits(in.time_s);

  for (int c = 0; c < 3; ++c) {
    rows[kRowCamera][c] = rt_hit::Bits(in.camera_known ? in.camera[c] : 0.0f);
    rows[kRowSunWorld][c] = rt_hit::Bits(in.sun_world_known ? in.to_sun_world[c] : 0.0f);
  }
  rows[kRowCamera][3] = rt_hit::Bits(rt_hit::NightFactor(in.hit.light.sun));

  rows[kRowGrid][0] = rt_hit::Bits(0.0f);
  rows[kRowGrid][1] = rt_hit::Bits(0.0f);
  rows[kRowGrid][2] = rt_hit::Bits(kCellMetres);
  rows[kRowGrid][3] = rt_hit::Bits(kDefaultLookupMetres);
  if (in.lamps != nullptr) {
    const LampFrame& l = *in.lamps;
    rows[kRowWindow][0] = rt_hit::Bits(float(l.cx0));
    rows[kRowWindow][1] = rt_hit::Bits(float(l.cz0));
    rows[kRowWindow][2] = l.nx;
    rows[kRowWindow][3] = l.nz;
    rows[kRowCounts][0] = l.lamp_count;
    rows[kRowCounts][1] = l.stats.kept + l.stats.over_budget;
    rows[kRowCounts][2] = l.cells();
    rows[kRowCounts][3] = l.stats.live_given;
  }
  rows[kRowCalibration][0] = in.stats_frame ? 1u : 0u;
  rows[kRowCalibration][1] = in.calibration_windows;
  rows[kRowCalibration][3] = in.world_kind;
  for (uint32_t c = 0; c < kClasses; ++c) {
    rows[kRowGains + c / 4][c % 4] = rt_hit::Bits(in.gains[c]);
  }
  if (in.emitter_count != 0) {
    rows[kRowEmitters][0] = in.emitter_count;
    rows[kRowEmitters][1] = in.emitter_row_base;
    rows[kRowEmitters][2] = std::min(in.emitter_static, in.emitter_count);
    rows[kRowEmitters][3] = in.emitter_groups;
  }
  rows[kRowSchedule][0] = rt_hit::Bits(std::clamp(in.schedule.street, 0.0f, 1.0f));
  rows[kRowSchedule][1] = rt_hit::Bits(std::clamp(in.schedule.fill, 0.0f, 1.0f));
  rows[kRowSchedule][2] = in.live_shining;
  rows[kRowSchedule][3] = in.schedule.evidence;
}

// What a shader reads of the world rows (the twin of RtWorldLoad).
struct WorldDecoded {
  uint32_t serial = 0, flags = 0;
  float hour = -1.0f, lamp_factor = 0.0f;
  float units = 1.0f, wet = 0.0f;
  uint32_t weather = kUnknownWeather;
  float time_s = 0.0f;
  float camera[3] = {};
  float sun_night = 0.0f;
  float to_sun_world[3] = {};
  float cell_m = kCellMetres, lookup_m = kDefaultLookupMetres, origin_x = 0.0f, origin_z = 0.0f;
  int32_t cx0 = 0, cz0 = 0;
  uint32_t nx = 0, nz = 0;
  uint32_t lamp_count = 0, lamps_in_window = 0, cells = 0, live_count = 0;
  uint32_t stats_frame = 0, windows = 0, world_kind = 0;
  float gains[kClasses] = {};
  float street = 0.0f, fill = 0.0f;           // row 47: the street lamps' and the fill lights' schedules
  uint32_t live_shining = 0, evidence = 0;
  uint32_t emitter_count = 0, emitter_row_base = 0, emitter_static = 0, emitter_groups = 0;   // row 44
  bool Valid() const { return (flags & kWorldValid) != 0; }
  // What a glowing material of this kind glows by (the twin of hlsl RtWorldGlowFactor): a lamp lens by the street lamps' schedule, a traffic signal always, everything else (neon, signs, lit windows,
  // interiors) by the fill lights'.
  float GlowFactor(uint32_t kind) const { return kind == city_hit_shade::kGlowAlways ? 1.0f : (kind == city_hit_shade::kGlowLens ? street : fill); }
};
inline WorldDecoded DecodeWorldRows(const uint32_t rows[kTotalRows][4]) {
  WorldDecoded d;
  d.serial = rows[kRowStatus][0];
  d.flags = rows[kRowStatus][1];
  d.hour = rt_hit::FromBits(rows[kRowStatus][2]);
  d.lamp_factor = rt_hit::FromBits(rows[kRowStatus][3]);
  d.units = rt_hit::FromBits(rows[kRowLight][0]);
  d.wet = rt_hit::FromBits(rows[kRowLight][1]);
  d.weather = rows[kRowLight][2];
  d.time_s = rt_hit::FromBits(rows[kRowLight][3]);
  for (int c = 0; c < 3; ++c) {
    d.camera[c] = rt_hit::FromBits(rows[kRowCamera][c]);
    d.to_sun_world[c] = rt_hit::FromBits(rows[kRowSunWorld][c]);
  }
  d.sun_night = rt_hit::FromBits(rows[kRowCamera][3]);
  d.origin_x = rt_hit::FromBits(rows[kRowGrid][0]);
  d.origin_z = rt_hit::FromBits(rows[kRowGrid][1]);
  d.cell_m = rt_hit::FromBits(rows[kRowGrid][2]);
  d.lookup_m = rt_hit::FromBits(rows[kRowGrid][3]);
  d.cx0 = int32_t(rt_hit::FromBits(rows[kRowWindow][0]));
  d.cz0 = int32_t(rt_hit::FromBits(rows[kRowWindow][1]));
  d.nx = rows[kRowWindow][2];
  d.nz = rows[kRowWindow][3];
  d.lamp_count = rows[kRowCounts][0];
  d.lamps_in_window = rows[kRowCounts][1];
  d.cells = rows[kRowCounts][2];
  d.live_count = rows[kRowCounts][3];
  d.stats_frame = rows[kRowCalibration][0];
  d.windows = rows[kRowCalibration][1];
  d.world_kind = rows[kRowCalibration][3];
  for (uint32_t c = 0; c < kClasses; ++c) {
    d.gains[c] = rt_hit::FromBits(rows[kRowGains + c / 4][c % 4]);
  }
  d.street = rt_hit::FromBits(rows[kRowSchedule][0]);
  d.fill = rt_hit::FromBits(rows[kRowSchedule][1]);
  d.live_shining = rows[kRowSchedule][2];
  d.evidence = rows[kRowSchedule][3];
  d.emitter_count = rows[kRowEmitters][0];
  d.emitter_row_base = rows[kRowEmitters][1];
  d.emitter_static = rows[kRowEmitters][2];
  d.emitter_groups = rows[kRowEmitters][3];
  return d;
}

// ---------------------------------------------------------------------------
// the lookup: the lamps near a point (the twin of RtLampBegin / RtLampStep in hlsl/rt_world_frame.hlsli, in the same order)
// ---------------------------------------------------------------------------

// A VIEW space point in the game's world: the camera plus the point along the world's axes (rt_hit::WorldPoint, the axes are rows 5, 6 and 7 of the hit frame).
inline void ViewToWorld(const WorldDecoded& w, const float axes[3][3], const float view[3], float out[3]) {
  for (int k = 0; k < 3; ++k) {
    out[k] = w.camera[k] + (view[0] * axes[k][0] + view[1] * axes[k][1] + view[2] * axes[k][2]);  // as the shader's dot: the same rounding
  }
}

struct LampCursor {
  int32_t cx = 0, cz = 0;      // the cell now (window relative)
  int32_t x0 = 0, x1 = -1, z1 = -1;
  uint32_t index = 0, end = 0;  // the lamp now, and where this cell's lamps end
  bool done = true;
};

// The cell table's (first, count) of window cell (cx, cz).
inline void CellRange(const uint32_t* lamp_words, uint32_t nx, int32_t cx, int32_t cz, uint32_t* first, uint32_t* count) {
  const uint32_t c = uint32_t(cz) * nx + uint32_t(cx);
  const uint32_t* row = lamp_words + size_t(1 + c / 2) * 4u + size_t(c % 2) * 2u;
  *first = row[0];
  *count = row[1];
}

// Skips forward to a cell that has lamps, from the cell the cursor is on (its lamps not yet taken).
inline void SettleCursor(const WorldDecoded& w, const uint32_t* lamp_words, LampCursor* c) {
  for (uint32_t guard = 0; guard < kMaxLookupAxis * kMaxLookupAxis + 1; ++guard) {
    uint32_t first = 0, count = 0;
    CellRange(lamp_words, w.nx, c->cx, c->cz, &first, &count);
    if (count != 0) {
      c->index = first;
      c->end = first + count;
      return;
    }
    if (++c->cx > c->x1) {
      c->cx = c->x0;
      if (++c->cz > c->z1) {
        c->done = true;
        return;
      }
    }
  }
  c->done = true;
}

// `view` is the point in view space; `radius` in metres (clamped to kMaxLookupCells cell sides); the cursor stands on the first lamp (done: none).
inline LampCursor LampBegin(const WorldDecoded& w, const uint32_t* lamp_words, const float axes[3][3], const float view[3], float radius) {
  LampCursor c;
  if ((w.flags & kWorldLamps) == 0 || w.nx == 0 || w.nz == 0 || w.lamp_count == 0 || !(w.cell_m > 0.0f)) {
    return c;
  }
  float p[3];
  ViewToWorld(w, axes, view, p);
  const float r = std::clamp(radius, 0.0f, kMaxLookupCells * w.cell_m);
  int32_t x0 = int32_t(std::floor((p[0] - r - w.origin_x) / w.cell_m)) - w.cx0;
  int32_t x1 = int32_t(std::floor((p[0] + r - w.origin_x) / w.cell_m)) - w.cx0;
  int32_t z0 = int32_t(std::floor((p[2] - r - w.origin_z) / w.cell_m)) - w.cz0;
  int32_t z1 = int32_t(std::floor((p[2] + r - w.origin_z) / w.cell_m)) - w.cz0;
  x0 = std::max(x0, 0);
  z0 = std::max(z0, 0);
  x1 = std::min(x1, int32_t(w.nx) - 1);
  z1 = std::min(z1, int32_t(w.nz) - 1);
  x1 = std::min(x1, x0 + int32_t(kMaxLookupAxis) - 1);
  z1 = std::min(z1, z0 + int32_t(kMaxLookupAxis) - 1);
  if (x0 > x1 || z0 > z1) {
    return c;
  }
  c.cx = x0;
  c.cz = z0;
  c.x0 = x0;
  c.x1 = x1;
  c.z1 = z1;
  c.done = false;
  SettleCursor(w, lamp_words, &c);
  return c;
}

// The cursor on the next lamp (done at the end).
inline LampCursor LampStep(const WorldDecoded& w, const uint32_t* lamp_words, LampCursor c) {
  if (c.done) {
    return c;
  }
  if (++c.index < c.end) {
    return c;
  }
  if (++c.cx > c.x1) {
    c.cx = c.x0;
    if (++c.cz > c.z1) {
      c.done = true;
      return c;
    }
  }
  SettleCursor(w, lamp_words, &c);
  return c;
}

// The lamps a lookup finds, in iteration order.
inline void LampsNear(const WorldDecoded& w, const uint32_t* lamp_words, const float axes[3][3], const float view[3], float radius, std::vector<uint32_t>* out) {
  out->clear();
  for (LampCursor c = LampBegin(w, lamp_words, axes, view, radius); !c.done; c = LampStep(w, lamp_words, c)) {
    out->push_back(c.index);
  }
}

// The three rows of lamp `i` of the buffer.
inline void LampRows(const uint32_t* lamp_words, uint32_t i, uint32_t rows[3][4]) {
  const uint32_t base = lamp_words[2];
  std::memcpy(rows, lamp_words + size_t(base + kLampRowsEach * i) * 4u, sizeof(uint32_t) * 12);
}

// The lamp light at a hit through the world frame's grid (CITY-RT R7; the twin of hlsl/city_hit_shade.hlsli CityHitLamps with CITY_HIT_LAMP_GRID): the lamps of the lookup around the VIEW space point
// `position` (the world rows' lookup radius, kLampLookup where they give none) in the cursor's order as the library's lamps, the first kLampMax of them that reach the point examined (the walk passes at most
// kLampWalkMax lamps), their light summed and capped in luminance;
// with `shadow` one lamp is chosen by `noise` and `blocked` is its shadow ray (city_hit_shade::LampPickCapped).
inline city_hit_shade::LampLight LampLightAt(const WorldDecoded& w, const uint32_t* lamp_words, const float axes[3][3], float lamp_factor, city_hit_shade::V3 position, city_hit_shade::V3 normal,
                                             bool shadow, float noise, const std::function<bool(city_hit_shade::V3, city_hit_shade::V3, float)>& blocked) {
  const float view[3] = {position.x, position.y, position.z};
  const float radius = w.lookup_m > 0.0f ? w.lookup_m : city_hit_shade::kLampLookup;
  std::vector<uint32_t> index;
  LampsNear(w, lamp_words, axes, view, radius, &index);
  std::vector<city_hit_shade::Lamp> lamps;
  lamps.reserve(index.size());
  for (const uint32_t i : index) {
    uint32_t rows[3][4];
    LampRows(lamp_words, i, rows);
    lamps.push_back(city_hit_shade::UnpackGpuLamp(rows));
  }
  const uint32_t count = uint32_t(lamps.size());
  return shadow ? city_hit_shade::LampPickCapped(lamp_factor, lamps.data(), count, position, normal, noise, position.z, blocked)
                : city_hit_shade::LampsCapped(lamp_factor, lamps.data(), count, position, normal);
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

// The lamp term's and the glow's counters (hlsl CITY_HIT_CENSUS_*: eight words) of the stats frames, summed; a pass hands a stats frame's words to Add once its readback is back.
struct LampCensus {
  uint64_t words[city_hit_shade::kCensusWords] = {};
  uint32_t frames = 0;  // stats frames added
  void Add(const uint32_t w[city_hit_shade::kCensusWords]) {
    for (uint32_t i = 0; i < city_hit_shade::kCensusWords; ++i) words[i] += w[i];
    ++frames;
  }
};
inline std::string LampCensusText(const LampCensus& c) {
  if (c.frames == 0) {
    return {};
  }
  using namespace city_hit_shade;
  const uint64_t lookups = c.words[kCensusLookups];
  const double n = lookups != 0 ? double(lookups) : 1.0;
  char buffer[480];
  std::snprintf(buffer, sizeof(buffer),
                "; lamp term at hits: %llu lookups in %u stats frames, %.1f lamps examined a hit (%.1f lit), the cap of %u lamps hit in %.2f%% and the sum's cap of %.1f in %.2f%%; glowing hits: night curve %llu, "
                "street lamp lenses %llu, always %llu",
                (unsigned long long)lookups, c.frames, double(c.words[kCensusExamined]) / n, double(c.words[kCensusLit]) / n, kLampMax, 100.0 * double(c.words[kCensusCapCount]) / n, double(kLampSumCap),
                100.0 * double(c.words[kCensusCapSum]) / n, (unsigned long long)c.words[kCensusGlowNight], (unsigned long long)c.words[kCensusGlowNight + 1],
                (unsigned long long)c.words[kCensusGlowNight + 2]);
  return buffer;
}

struct Census {
  uint64_t frames = 0;           // frames the rows were filled
  uint64_t frames_no_view = 0;   // frames a fill was asked for and no top level's view was given
  uint64_t frames_no_light = 0;  // ... with no world light (the AO had no draws)
  uint64_t frames_light_car = 0; // ... where the light was the car's
  uint64_t frames_hour = 0;      // ... with the hour known
  uint64_t frames_live = 0;      // ... with a fresh live list
  uint64_t frames_night = 0;     // ... with a schedule over 0
  uint64_t frames_cascade = 0;   // ... with the game's sun cascades
  uint64_t frames_env = 0;       // ... with the environment map's slot
  uint64_t frames_evidence_on = 0;   // ... where the game's live lamp list said the street lamps are on (R7)
  uint64_t frames_evidence_off = 0;  // ... and where it said they are off while the hour said on
  uint64_t lamps_sum = 0;        // lamps uploaded, summed (the mean is a frame's)
  uint32_t lamps_max = 0;
  uint64_t table_sum = 0, live_sum = 0, over_budget_sum = 0, outside_sum = 0, inactive_sum = 0;
  uint64_t bytes_sum = 0;
  uint32_t windows_closed = 0;
  uint32_t classes_moved = 0;
};

// One line's worth, "; " first (renderer.cpp's census line), empty when nothing was filled.
inline std::string CensusText(const Census& c, float hour, float schedule, uint32_t lamps_now, bool table_loaded) {
  if (c.frames == 0 && c.frames_no_view == 0) {
    return {};
  }
  const double n = c.frames != 0 ? double(c.frames) : 1.0;
  char buffer[900];
  std::snprintf(buffer, sizeof(buffer),
                "; ray tracing world frame: %llu frames filled (%llu without a view); the world's light in %.0f%% (the car's in %.0f%%), the hour in %.0f%% (now %.2f), the sun's cascades in %.0f%%, "
                "the environment map in %.0f%%; lamps a frame %.1f mean, %u most, %u now (the table %.1f, the live list %.1f, off %.1f, over the budget %.1f, outside the window %.1f), the schedule %.2f, "
                "fresh live list in %.0f%%, the static table %s; %.1f KiB written a frame; calibration windows %u (%u class gains moved); the game's lamp list said the street lamps are on in %.1f%% of the frames "
                "and off in %.1f%%",
                (unsigned long long)c.frames, (unsigned long long)c.frames_no_view, 100.0 * double(c.frames - c.frames_no_light) / n, 100.0 * double(c.frames_light_car) / n,
                100.0 * double(c.frames_hour) / n, double(hour), 100.0 * double(c.frames_cascade) / n, 100.0 * double(c.frames_env) / n, double(c.lamps_sum) / n, c.lamps_max, lamps_now,
                double(c.table_sum) / n, double(c.live_sum) / n, double(c.inactive_sum) / n, double(c.over_budget_sum) / n, double(c.outside_sum) / n, double(schedule),
                100.0 * double(c.frames_live) / n, table_loaded ? "loaded" : "not loaded", double(c.bytes_sum) / n / 1024.0, c.windows_closed, c.classes_moved, 100.0 * double(c.frames_evidence_on) / n,
                100.0 * double(c.frames_evidence_off) / n);
  return buffer;
}

}  // namespace mcla::gpu::rt_world
