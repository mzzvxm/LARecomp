#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced side mirrors: the SETTING and its pause row (MIRROR-ROW)
// ===========================================================================
// One row on GRAPHICS > REFLECTIONS, right after RT CITY REFLECTIONS: RT SIDE MIRRORS, AUTO / OFF / ON (mcla_gpu_car_mirror_rt, live, saved). It is one level, so the words are the player's two
// (OFF, ON) and AUTO: nothing is called low and there is no second level to name. This header is every decision about the row that needs no device, so that car_mirror_setting_test.cpp (and the
// pause menu, which includes it) can check them: what a word comes to on a card, the gate that greys the row and what it says, and AUTO's label. car_mirror_rt_rules.h has the pass's own rules
// (Wanted, the cell's pixel cap); the two agree for every class and tier (the test holds them to each other).
//
// WHAT THE WORDS COME TO (Decide). `auto` is rt_tier.h's table cell of mcla_gpu_car_mirror_rt at the card's RUNG (the ray tracing tier gated by the class: Low is none, Mid is at most weak): OFF
// without ray tracing and on a weak card, ON on a strong one. The pass costs little (0.03 to 0.1 ms, estimated) but it asks for the car's ray tracing structures in every frame the setting
// wants it, which a weak card keeps for the car's own effects only: the owner's rule is that a weaker card gets it off by itself. `on` is the player's own: any card that can trace and is not
// of the Low class (a Mid class card runs it with the smaller pixel cap, car_mirror_rt_rules.h LimitsFor). `off` keeps the game's mirrors. A word that is none of the three (an older
// larecomp.toml) is `auto`.
//
// THE GATE (GateFor): the first that holds is the row's state:
//   kNoRt     the card does not trace rays (tier none): grey, " (NO RT GPU)" (Direct3D 12 with DXR 1.1 and shader model 6.5, or Vulkan with ray queries)
//   kTooSlow  it does but the class is Low (shared memory): grey, " (TOO SLOW)" (rt_tier.h's " (GPU TOO SLOW)" is too wide for this page)
//   kRuns     the row is on
// The row does NOT depend on CAR SELF-REFLECTIONS: the mirror asks for the car's structures itself (car_mirror_rt.cpp), so it works with the car's own ray traced reflection off.
// The pass shows only in the first person cameras (cockpit, bumper), where the glass is big enough to read, and does nothing in a frame without a mirror pair, in the pause and the garage
// views: the row is on whatever the camera is.
//
// THE WORDS (RowLabel, RowSuffix) and THE PAGE WIDTH. The pause movie sizes a page to its widest label (suffix included) plus its widest value over every row and every state (pause_menu.cpp
// "Page width"): at most 41 characters. REFLECTIONS' widest value is "AUTO (RAY TRACED)" (17, CAR SELF-REFLECTIONS) and its widest label "REFLECTIONS ON SURFACES" (23): 40. "RT SIDE MIRRORS" is 15,
// so a reason after it would make the page wider (15 + 12 + 17 = 44): while the row is not on, its label is the short "MIRRORS" and the reason follows ("MIRRORS (NO RT GPU)" is 19, the
// longest). Every state is held to the page's budget by a static_assert below.
//
// Header only; includes nothing of Plume or ReXGlue.
//
// CHECK: nothing here was run in the game. The wording of the reasons is the width's (the owner may prefer another within the budget).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "rt_tier.h"

namespace mcla::gpu::car_mirror_setting {

namespace gc = gpu_class;

// ---------------------------------------------------------------------------
// the words
// ---------------------------------------------------------------------------

enum class Choice : uint8_t { kAuto, kOff, kOn };

// The stored word as a choice; a word that is none of the three is `auto` (car_mirror_rt_rules.h SettingOf reads it the same way).
constexpr Choice ChoiceOf(std::string_view word) {
  if (word == "off") return Choice::kOff;
  if (word == "on") return Choice::kOn;
  return Choice::kAuto;
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

enum class Gate : uint8_t {
  kRuns,     // the row is on
  kNoRt,     // the card does not trace rays (tier none)
  kTooSlow,  // it does, but the class is Low (shared memory)
};

constexpr Gate GateFor(gc::GpuClass cls, gc::RtTier tier) {
  if (tier == gc::RtTier::kNone) return Gate::kNoRt;
  if (cls == gc::GpuClass::kLow) return Gate::kTooSlow;
  return Gate::kRuns;
}

// The row's label in each state: the whole name while it is on, a short one with the reason after it otherwise (see THE PAGE WIDTH).
inline constexpr const char kFullLabel[] = "RT SIDE MIRRORS";
inline constexpr const char kShortLabel[] = "MIRRORS";
constexpr const char* RowLabel(Gate gate) { return gate == Gate::kRuns ? kFullLabel : kShortLabel; }
constexpr const char* RowSuffix(Gate gate) {
  switch (gate) {
    case Gate::kNoRt: return " (NO RT GPU)";
    case Gate::kTooSlow: return " (TOO SLOW)";
    default: return "";
  }
}
// Grey: the row's value is not shown and a step does nothing.
constexpr bool RowEnabled(Gate gate) { return gate == Gate::kRuns; }

constexpr std::string_view GateName(Gate gate) {
  switch (gate) {
    case Gate::kRuns: return "on";
    case Gate::kNoRt: return "no ray tracing graphics card";
    default: return "graphics card too slow";
  }
}

// The page's budget (pause_menu.cpp "Page width"): 41 characters of the widest label and the widest value. REFLECTIONS' widest value is CAR SELF-REFLECTIONS' "AUTO (RAY TRACED)".
inline constexpr size_t kPageChars = 41;
inline constexpr size_t kWidestValue = 17;
inline constexpr size_t kWidestOtherLabel = 23;  // "REFLECTIONS ON SURFACES"
constexpr size_t LabelChars(Gate gate) { return std::string_view(RowLabel(gate)).size() + std::string_view(RowSuffix(gate)).size(); }
constexpr bool EveryStateFits() {
  for (uint8_t g = 0; g <= uint8_t(Gate::kTooSlow); ++g) {
    if (LabelChars(Gate(g)) + kWidestValue > kPageChars) return false;
  }
  return true;
}
static_assert(EveryStateFits(), "every state of the RT SIDE MIRRORS row fits the REFLECTIONS page (41 characters of label and value)");
static_assert(LabelChars(Gate::kRuns) <= kWidestOtherLabel && LabelChars(Gate::kNoRt) <= kWidestOtherLabel && LabelChars(Gate::kTooSlow) <= kWidestOtherLabel,
              "the row's reasons are no wider than the page's own widest label, so the page is as wide as it was");

// ---------------------------------------------------------------------------
// what a setting's word comes to
// ---------------------------------------------------------------------------

struct Decision {
  bool wanted = false;  // the pass is wanted on this card (the device's own answer, that the car's scene is ready, comes on top of it: car_mirror_rt_rules.h Wanted's `supported`)
  gc::RtTier rung = gc::RtTier::kNone;
  Gate gate = Gate::kNoRt;
};

// What a word comes to on a card of this class and tier: the same answer as car_mirror_rt_rules.h Wanted(SettingOf(word), true, RtClass(cls, tier)), written from the table's cell.
inline Decision Decide(gc::GpuClass cls, gc::RtTier tier, std::string_view word) {
  Decision d;
  d.rung = gc::RtRung(cls, tier);
  d.gate = GateFor(cls, tier);
  if (d.gate != Gate::kRuns) {
    return d;  // no ray tracing, or the Low class: nothing runs, whatever the word says
  }
  switch (ChoiceOf(word)) {
    case Choice::kOff: d.wanted = false; break;
    case Choice::kOn: d.wanted = true; break;
    default: d.wanted = gc::RtCell(gc::RtEffect::kCarMirrorRt, d.rung) == "on"; break;
  }
  return d;
}

// "AUTO (ON)" / "AUTO (OFF)": the pause row's label for `auto`, what it comes to on this card (a grey row shows no value).
inline std::string AutoLabel(gc::GpuClass cls, gc::RtTier tier) { return gc::AutoLabelRtFor(cls, tier, gc::RtEffect::kCarMirrorRt); }

// What the launcher says under its row (rt_tier.h RtAutoDescription): "Auto is Off without ray tracing, Off on a weak ray tracing card and On on a strong one."
inline std::string LauncherAutoLine() { return gc::RtAutoDescription(gc::RtEffect::kCarMirrorRt); }

}  // namespace mcla::gpu::car_mirror_setting
