#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced bounce light's SETTING and its pause row (CITY-RT M3, package G4 of perf/CITY-RT-M3.md section 9)
// ===========================================================================
// One row on GRAPHICS > LIGHTING AND SHADOWS, after BOUNCE LIGHT: RT BOUNCE LIGHT, AUTO / OFF / HIGH / ULTRA (mcla_gpu_rt_gi, live, saved). This header is every decision about it that
// needs no device, so that gi_setting_test.cpp (and the pause menu, which includes it) can check them: what a setting's word comes to on a card, the gate that greys the row and what it says,
// AUTO's label, when BOUNCE LIGHT says "(REPLACED)", and the debug cvar that sets the probes' own feedback. gi_setting.cpp holds the two flags the render thread sets for the menu's thread
// and the setting's LiveText.
//
// WHAT THE WORDS COME TO (Decide). `auto` is rt_tier.h's table cell of mcla_gpu_rt_gi at the card's RUNG (the ray tracing tier gated by the class: Low is none, Mid is at most weak): OFF without
// ray tracing and on a weak card, HIGH on a strong one (the owner's decision of 2026-11-21: split the High class by ray tracing strength; AUTO ON where the card traces quickly, the screen-space
// bounce light everywhere else). off / high / ultra are the player's own and are never overridden, except where the gate is shut (below): the layout a level runs on a rung is gi_frame.h LayoutFor
// (a weak card by hand gets the lean field at HIGH, the standard one at ULTRA; none never runs a field). A word that is none of the four (an older larecomp.toml) is `auto`.
//
// THE GATE (Gate): the row is grey, with its reason after the label, where the field cannot run: " (NO RT GPU)" where the card does not trace (tier none: the API cannot, or it has no ray tracing
// units), " (TOO SLOW)" where it does but the class is Low (shared memory: nothing heavy runs there), and " (NOT READY)" while the probe update (package G2) is not built into this renderer: with
// no update the field would hold nothing, and the screen-space bounce light must stay the source (gi_apply.h IDENTITY: the field fades in only when it stands, so a missing update would only waste
// 75 MiB; the flag keeps even that from happening). THE WIDTH: a pause page may be 41 characters of its widest label (suffix included) plus its widest value (pause_menu.cpp "Page width"); the page's widest
// value is 14 ("AUTO (DEFAULT)") and its widest label 27 (FOLIAGE SHADOWS (NO SHADOW)), so "RT BOUNCE LIGHT" (15) takes a reason of 12 characters at most: rt_tier.h's " (GPU TOO SLOW)" (15) would make
// 30 + 14 = 44, which is why the too slow reason is " (TOO SLOW)" here (RT SHADOWS, a shorter label, keeps the long one).
//
// THE "(REPLACED)" HINT. The BOUNCE LIGHT row (mcla_gpu_ssgi) reads " (REPLACED)" while the probe field stands and the screen-space bounce light is not built (the field's fade is 1:
// gi_apply_rules.h SsgiShareFor is 0): the row still sets what runs where the field does not (a cut, the first moments, the places it does not cover), and BOUNCE LIGHT STRENGTH stays on while either
// bounce light runs, since both are as strong as it says.
//
// Header only, includes nothing of Plume or ReXGlue. THREADING: pure functions; the flags are in gi_setting.cpp.
//
// CHECK: nothing here was run in the game. The wording " (NOT READY)" is for the time between this package and G2 and goes with the flag when G2 lands.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "gi_frame.h"
#include "rt_tier.h"

namespace mcla::gpu::gi_setting {

namespace gc = gpu_class;

// ---------------------------------------------------------------------------
// the glue between gi_frame.h and rt_tier.h (the two branches meet here: gi_frame.h says "the glue is a cast")
// ---------------------------------------------------------------------------

static_assert(uint8_t(gi::Rung::kNone) == uint8_t(gc::RtTier::kNone) && uint8_t(gi::Rung::kWeak) == uint8_t(gc::RtTier::kWeak) && uint8_t(gi::Rung::kStrong) == uint8_t(gc::RtTier::kStrong),
              "gi::Rung and gpu_class::RtTier have the same values: the glue is a cast");

constexpr gi::Rung RungOf(gc::RtTier tier) { return static_cast<gi::Rung>(static_cast<uint8_t>(tier)); }
constexpr gc::RtTier TierOf(gi::Rung rung) { return static_cast<gc::RtTier>(static_cast<uint8_t>(rung)); }

// The table's cell of mcla_gpu_rt_gi at each rung and gi_frame.h's AutoLevel are the same words (the second is "a copy of the cell, so that the two can be compared when the branches meet").
constexpr bool CellsAgree() {
  for (uint8_t r = 0; r < 3; ++r) {
    const gc::RtTier tier = static_cast<gc::RtTier>(r);
    if (gi::LevelName(gi::AutoLevel(RungOf(tier))) != gc::RtCell(gc::RtEffect::kRtGi, tier)) return false;
  }
  return true;
}
static_assert(CellsAgree(), "rt_tier.h's mcla_gpu_rt_gi cells and gi_frame.h's AutoLevel are the same table");

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

enum class Gate : uint8_t {
  kRuns,      // the field may run: the row is on
  kNoRt,      // the card does not trace rays (tier none)
  kTooSlow,   // it does, but the class is Low (shared memory)
  kNotBuilt,  // the probe update (G2) is not in this renderer
};

constexpr Gate GateFor(gc::GpuClass cls, gc::RtTier tier, bool update_built) {
  if (tier == gc::RtTier::kNone) return Gate::kNoRt;
  if (cls == gc::GpuClass::kLow) return Gate::kTooSlow;
  if (!update_built) return Gate::kNotBuilt;
  return Gate::kRuns;
}

// What the grey row says after its label (at most 12 characters: see THE WIDTH; the first is rt_tier.h RtRowSuffix's).
constexpr const char* RowSuffix(Gate gate) {
  switch (gate) {
    case Gate::kNoRt: return " (NO RT GPU)";
    case Gate::kTooSlow: return " (TOO SLOW)";
    case Gate::kNotBuilt: return " (NOT READY)";
    default: return "";
  }
}
constexpr bool RowEnabled(Gate gate) { return gate == Gate::kRuns; }

// ---------------------------------------------------------------------------
// what a setting's word comes to
// ---------------------------------------------------------------------------

struct Decision {
  gi::Level level = gi::Level::kOff;    // what runs: the word resolved, off where the gate is shut
  gi::Rung rung = gi::Rung::kNone;      // rt_tier.h RtRung of the class and the tier, as gi_frame.h's Rung
  gi::Layout layout = gi::Layout::kNone;  // which budget the field runs (gi_frame.h LayoutFor)
  Gate gate = Gate::kNoRt;
};

// The word of the setting as a level, `auto` through the table's cell for the rung; a word that is none of off / high / ultra / auto is `auto`.
inline gi::Level LevelOfWord(gc::GpuClass cls, gc::RtTier tier, std::string_view word) {
  gi::Level level = gi::Level::kOff;
  if (gi::ParseLevel(word, &level)) return level;
  return gi::ParseLevel(gc::RtCell(gc::RtEffect::kRtGi, gc::RtRung(cls, tier)), &level) ? level : gi::Level::kOff;
}

inline Decision Decide(gc::GpuClass cls, gc::RtTier tier, std::string_view word, bool update_built) {
  Decision d;
  d.gate = GateFor(cls, tier, update_built);
  d.rung = RungOf(gc::RtRung(cls, tier));
  d.level = d.gate == Gate::kRuns ? LevelOfWord(cls, tier, word) : gi::Level::kOff;
  d.layout = gi::LayoutFor(d.rung, d.level);
  return d;
}

// "AUTO (HIGH)" / "AUTO (OFF)": the pause row's label for `auto`, what it comes to now (OFF where the gate is shut).
inline std::string AutoLabel(gc::GpuClass cls, gc::RtTier tier, bool update_built) {
  return gc::AutoLabelOf(gi::LevelName(Decide(cls, tier, "auto", update_built).level));
}

// What the launcher says under its row (rt_tier.h RtAutoDescription): "Auto is Off without ray tracing, Off on a weak ray tracing card and High on a strong one."
inline std::string LauncherAutoLine() { return gc::RtAutoDescription(gc::RtEffect::kRtGi); }

// ---------------------------------------------------------------------------
// the BOUNCE LIGHT rows
// ---------------------------------------------------------------------------

inline constexpr const char kReplacedSuffix[] = " (REPLACED)";

// The BOUNCE LIGHT row says (REPLACED): the field stands in the published frame and has taken the whole of the screen-space bounce light's share (fade 1: SsgiShareFor 0).
inline bool ReplacedBy(const gi::GiFrame& frame) { return frame.ReplacesSsgi() && frame.fade >= 0.999f; }

// BOUNCE LIGHT STRENGTH is on while either bounce light runs.
constexpr bool AnyBounceLight(bool screen_space_on, const Decision& rt) { return screen_space_on || rt.level != gi::Level::kOff; }

// ---------------------------------------------------------------------------
// the debug feedback
// ---------------------------------------------------------------------------

// mcla_gpu_rt_gi_bounces: the probes' own feedback (the weight of the field's own light in a hit's shading), 0 = strictly one bounce, 0.95 at most so that the loop cannot grow (M3 Q3).
// Not a number (NaN) is the default.
inline constexpr double kBounceWeightMax = 0.95;
inline float BounceWeight(double value) {
  if (!(value == value)) return gi::kBounceWeightDefault;
  return float(std::clamp(value, 0.0, kBounceWeightMax));
}

// --- the process's state (gi_setting.cpp) ---------------------------------------------------------------------------------------------------

// The setting as it stands now: Decide of this process's class, ray tracing tier, the setting's word and whether the update is built. Any thread, cheap.
Decision Current();
// The probe update is built in (gi_integration.h SetUpdate sets it) / the field stands and replaces the screen-space bounce light (the integration sets it once a frame).
void NoteUpdateBuilt(bool built);
void NoteReplaced(bool replaced);
// BounceWeight of mcla_gpu_rt_gi_bounces. Any thread.
float BounceWeightNow();

}  // namespace mcla::gpu::gi_setting
