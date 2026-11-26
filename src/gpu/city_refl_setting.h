#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city in the car paint's reflections: the SETTING and its pause row (CITY-RT M2, package R8 of perf/CITY-RT-M2.md section 9)
// ===========================================================================
// One row on GRAPHICS > REFLECTIONS, right after CAR SELF-REFLECTIONS whose ray traced pass it rides on: RT CITY REFLECTIONS, AUTO / OFF / HIGH / ULTRA (mcla_gpu_rt_reflections, live, saved). This header is
// every decision about it that needs no device, so that city_refl_setting_test.cpp (and the pause menu, which includes it) can check them: what a setting's word comes to on a card, the gate that
// greys the row and what it says, how the state of the city's files becomes a word on the row, and AUTO's label. city_refl_setting.cpp holds the two flags the render thread sets for the menu's thread.
// city_refl_rules.h has the levels and the cells (LevelFor, CellFor); this is the part the menu and the host share.
//
// WHAT THE WORDS COME TO (Decide). `auto` is rt_tier.h's table cell of mcla_gpu_rt_reflections at the card's RUNG (the ray tracing tier gated by the class: Low is none, Mid is at most weak): OFF without
// ray tracing and on a weak card, HIGH on a strong one (the owner's goal is a strong card's look: the buildings, lamp posts and signs of the street clearly in the paint). off / high / ultra are the
// player's own and never overridden, except where the gate is shut (below); a weak card set to HIGH or ULTRA by hand runs the Mid class's cell (city_refl_rules.h CellFor: 60 m, two rays). A word that
// is none of the four (an older larecomp.toml) is `auto`. `wanted` is what the word comes to on the card; `level` is what runs now: `wanted` once the gate is open and the city's data stands, else off.
//
// THE GATE (GateFor), in this order; the first that holds is the row's state:
//   kNoRt        the card does not trace rays (tier none): grey, " (NO RT GPU)"
//   kTooSlow     it does but the class is Low (shared memory): grey, " (TOO SLOW)" (rt_tier.h's " (GPU TOO SLOW)" is too wide for this page)
//   kNeedsCarRt  the ray traced CAR SELF-REFLECTIONS is not running (that row is OFF, ON or AUTO without ray tracing: pause_selfrefl_row.h RayTracedRuns): the city's reflection is one more thing the
//                car's own ray traced pass casts, so it has nothing to ride on; grey, " (NEEDS CAR RT)". The setting is kept and comes back when that row is on RAY TRACED again.
//   kNotReady    the city's data cannot be had this session (the game's archive is not there, or a build of this session failed: the log says why): grey, " (NOT READY)"
//   kBuilding    the pack and the hit data are missing or out of date and the game builds them in the background (the launcher's setup has them ready at the first start): " (BUILDING DATA)", and the
//                row is NOT grey: the setting works, it is saved, and the city comes into the paint by itself when the data is done (the effect says what it waits for instead of locking the row for as
//                long as the build lasts). Only while the word comes to something above off: a row that says OFF (or AUTO that comes to OFF) has no business with the data.
//   kRuns        none of these: the row is on
// The "wants" of the effect (the structures, the hit data and the textures: scene service) go out whenever the word comes to something above off and the ray traced car pass runs, the data's state
// included: it is the want that makes the app build the data. Only the pass itself waits for the data.
//
// THE WORDS (RowLabel, RowSuffix) and THE PAGE WIDTH. The pause movie sizes a page to its widest label (suffix included) plus its widest value over every row and every state (pause_menu.cpp "Page
// width"): at most 41 characters. This page's widest value is "AUTO (RAY TRACED)" (17, CAR SELF-REFLECTIONS) and its widest label "REFLECTIONS ON SURFACES" (23): 40. "RT CITY REFLECTIONS" is 19, so a reason after
// it would make the page wider (19 + 12 + 17 = 48): while the row is not on, its label is the short "RT CITY" and the reason follows ("RT CITY (BUILDING DATA)" is 23, the longest). Every state is held
// to the page's budget by a static_assert below.
//
// THE DATA'S STATE (Data, DataOf, Combine). Two probes of the app (gpu.h CityPackHooks) tell the scenes how the files stand: the city pack (city_rt_scene.cpp OpenPack) and the hit data and its textures
// beside it (city_hit_scene.cpp OpenFile); each scene publishes the answer here (NoteBase, NoteHit) and the menu reads the two as one (DataNow). A pack that is missing or stale counts as BEING BUILT
// where the app can build it (the scene asks for the build at that very probe), as not available where it cannot.
//
// Header only apart from the two flags (city_refl_setting.cpp); includes nothing of Plume or ReXGlue.
//
// CHECK: nothing here was run in the game. The wording of the reasons is the width's (the owner may prefer another within the budget); the state is read by the menu thread from two atomics the render
// thread writes at a probe (every two seconds while a build runs).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "city_pack_state.h"
#include "city_refl_rules.h"
#include "gpu_class.h"
#include "rt_tier.h"

namespace mcla::gpu::city_refl_setting {

namespace gc = gpu_class;
namespace cr = city_refl;
using Level = cr::Level;

// ---------------------------------------------------------------------------
// the state of the city's data
// ---------------------------------------------------------------------------

enum class Data : uint8_t {
  kUnknown,      // not asked yet (nothing wants the data, or the pack is not open: no hit probe yet)
  kReady,        // the files are current and open
  kBuilding,     // missing or out of date and the game builds them (a background thread)
  kUnavailable,  // cannot be had this session: no game archive, a failed build, a file that cannot be read
};

// One probe's answer as a Data. `can_build`: the app has a build to start (the scene asks for it at the same probe, once).
constexpr Data DataOf(city_rt::PackState state, bool can_build) {
  switch (state) {
    case city_rt::PackState::kCurrent:
      return Data::kReady;
    case city_rt::PackState::kBuilding:
      return Data::kBuilding;
    case city_rt::PackState::kMissing:
    case city_rt::PackState::kStale:
      return can_build ? Data::kBuilding : Data::kUnavailable;
    default:  // kNoSource, kFailed
      return Data::kUnavailable;
  }
}

// The two stages as one: the hit data is read beside the pack, so what the pack is decides first.
constexpr Data Combine(Data base, Data hit) {
  switch (base) {
    case Data::kUnavailable:
      return Data::kUnavailable;
    case Data::kBuilding:
      return Data::kBuilding;
    case Data::kUnknown:
      return Data::kUnknown;
    default:
      return hit;
  }
}

constexpr std::string_view DataName(Data data) {
  switch (data) {
    case Data::kUnknown:
      return "not asked yet";
    case Data::kReady:
      return "ready";
    case Data::kBuilding:
      return "being built";
    default:
      return "not available";
  }
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

enum class Gate : uint8_t {
  kRuns,         // the row is on
  kNoRt,         // the card does not trace rays (tier none)
  kTooSlow,      // it does, but the class is Low (shared memory)
  kNeedsCarRt,   // the ray traced CAR SELF-REFLECTIONS is not running
  kNotReady,     // the city's data cannot be had this session
  kBuilding,     // the game builds the city's data (the row stays on)
};

// `wanted`: the word comes to something above off (the data's state only matters then).
constexpr Gate GateFor(gc::GpuClass, gc::RtTier tier, bool car_rt_on, bool wanted, Data data) {
  if (tier == gc::RtTier::kNone) return Gate::kNoRt;
  if (!car_rt_on) return Gate::kNeedsCarRt;
  if (wanted && data == Data::kUnavailable) return Gate::kNotReady;
  if (wanted && data == Data::kBuilding) return Gate::kBuilding;
  return Gate::kRuns;
}

// The row's label in each state: the whole name while it is on, a short one with the reason after it otherwise (see THE PAGE WIDTH).
inline constexpr const char kFullLabel[] = "RT CITY REFLECTIONS";
inline constexpr const char kShortLabel[] = "RT CITY";
constexpr const char* RowLabel(Gate gate) { return gate == Gate::kRuns ? kFullLabel : kShortLabel; }
constexpr const char* RowSuffix(Gate gate) {
  switch (gate) {
    case Gate::kNoRt: return " (NO RT GPU)";
    case Gate::kTooSlow: return " (TOO SLOW)";
    case Gate::kNeedsCarRt: return " (NEEDS CAR RT)";
    case Gate::kNotReady: return " (NOT READY)";
    case Gate::kBuilding: return " (BUILDING DATA)";
    default: return "";
  }
}
// Grey: the row's value is not shown and a step does nothing. The BUILDING state is not grey (see THE GATE).
constexpr bool RowEnabled(Gate gate) { return gate == Gate::kRuns || gate == Gate::kBuilding; }

constexpr std::string_view GateName(Gate gate) {
  switch (gate) {
    case Gate::kRuns: return "on";
    case Gate::kNoRt: return "no ray tracing graphics card";
    case Gate::kTooSlow: return "graphics card too slow";
    case Gate::kNeedsCarRt: return "ray traced car self-reflections are not on";
    case Gate::kNotReady: return "the city data is not available";
    default: return "waiting for the city data";
  }
}

// The page's budget (pause_menu.cpp "Page width"): 41 characters of the widest label and the widest value. REFLECTIONS' widest value is CAR SELF-REFLECTIONS' "AUTO (RAY TRACED)".
inline constexpr size_t kPageChars = 41;
inline constexpr size_t kWidestValue = 17;
inline constexpr size_t kWidestOtherLabel = 23;  // "REFLECTIONS ON SURFACES"
constexpr size_t LabelChars(Gate gate) { return std::string_view(RowLabel(gate)).size() + std::string_view(RowSuffix(gate)).size(); }
constexpr bool EveryStateFits() {
  for (uint8_t g = 0; g <= uint8_t(Gate::kBuilding); ++g) {
    if (LabelChars(Gate(g)) + kWidestValue > kPageChars) return false;
  }
  return true;
}
static_assert(EveryStateFits(), "every state of the RT CITY REFLECTIONS row fits the REFLECTIONS page (41 characters of label and value)");
static_assert(LabelChars(Gate::kBuilding) <= kWidestOtherLabel && LabelChars(Gate::kNeedsCarRt) <= kWidestOtherLabel,
              "the row's reasons are no wider than the page's own widest label, so the page is as wide as it was");

// ---------------------------------------------------------------------------
// what a setting's word comes to
// ---------------------------------------------------------------------------

struct Decision {
  Level wanted = Level::kOff;      // what the word comes to on this card (off where it cannot trace); the effect's wants are made from it
  Level level = Level::kOff;       // what runs now: `wanted` where the gate is open and the data stands, else off
  gc::RtTier rung = gc::RtTier::kNone;
  Gate gate = Gate::kNoRt;
  Data data = Data::kUnknown;
};

// `car_rt_on`: the ray traced car self-reflection runs (the renderer: the pass is active; the menu: pause_selfrefl_row.h RayTracedRuns).
inline Decision Decide(gc::GpuClass cls, gc::RtTier tier, std::string_view word, bool car_rt_on, Data data) {
  Decision d;
  d.rung = gc::RtRung(cls, tier);
  d.wanted = cr::LevelFor(cls, tier, word);
  d.data = data;
  d.gate = GateFor(cls, tier, car_rt_on, d.wanted != Level::kOff, data);
  d.level = (d.gate == Gate::kRuns && data == Data::kReady) ? d.wanted : Level::kOff;
  return d;
}

// "AUTO (HIGH)" / "AUTO (OFF)": the pause row's label for `auto`, what it comes to on this card (the gate and the data do not change it: a grey row shows no value, and while the data is built
// the row says what the word will do).
inline std::string AutoLabel(gc::GpuClass cls, gc::RtTier tier) { return gc::AutoLabelRtFor(cls, tier, gc::RtEffect::kRtReflections); }

// What the launcher says under its row (rt_tier.h RtAutoDescription): "Auto is Off without ray tracing, Off on a weak ray tracing card and High on a strong one."
inline std::string LauncherAutoLine() { return gc::RtAutoDescription(gc::RtEffect::kRtReflections); }

// --- the process's state (city_refl_setting.cpp) -----------------------------------------------------------------------------------------------

// The scenes publish their probes' answers (any thread; the render thread does): the city pack's (CityRtScene) and the hit data's (CityHitScene). Unknown again when the scene gives the file up.
void NoteBase(Data data);
void NoteHit(Data data);
// The two as one (Combine). Any thread, cheap.
Data DataNow();

}  // namespace mcla::gpu::city_refl_setting
