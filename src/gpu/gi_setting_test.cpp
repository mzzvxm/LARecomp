// Check of the ray traced bounce light's setting and its integration (gi_setting.h, gi_integration.h / .cpp): CITY-RT M3 package G4. A program of its own: it needs no renderer, no game files and no GPU, and
// exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_setting_test.cpp
//   run: larecomp_gi_setting_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check with its numbers)
//
// It checks: what each word of mcla_gpu_rt_gi comes to on every graphics class and ray tracing tier (the owner's rule: AUTO is ON where the card traces quickly and the screen-space bounce light is the source
// everywhere else; a hand-set level runs the layout of gi_frame.h LayoutFor and is never overridden except by the gate), the gate that greys the row and the words it says (and that they fit the page), AUTO's
// label, the launcher's line, the two enums' glue, the "(REPLACED)" hint's rule, the strength row's rule, the debug feedback; and then the TEXT of the wiring: the frame's order in scene.cpp (the field's frame
// starts before the apply's, its trigger work comes before the occlusion's), the integration's order inside the trigger, the renderer's start and end, the pause row, the three settings files and the CMake lines.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "gi_frame.h"
#include "gi_setting.h"
#include "rt_tier.h"

namespace gi = mcla::gpu::gi;
namespace gs = mcla::gpu::gi_setting;
namespace gc = mcla::gpu::gpu_class;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  } else if (g_verbose) {
    std::printf("ok: %s\n", what.c_str());
  }
}

constexpr gc::GpuClass kClasses[] = {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh};
constexpr gc::RtTier kTiers[] = {gc::RtTier::kNone, gc::RtTier::kWeak, gc::RtTier::kStrong};
const char* const kWords[] = {"auto", "off", "high", "ultra", "", "low", "medium", "AUTO", "bogus"};

std::string Name(gc::GpuClass c, gc::RtTier t, const std::string& word, bool built) {
  return std::string("class ") + std::string(gc::ClassName(c)) + ", tier " + std::string(gc::RtTierName(t)) + ", word '" + word + "', update " + (built ? "built" : "not built");
}

// ---------------------------------------------------------------------------
// the cells: the owner's rule, written out (not computed from the code under test)
// ---------------------------------------------------------------------------
// Expected level and layout for a word on a (class, tier) with the update built. `auto` is high only where the rung is strong (High class, strong tier); a hand-set word is the player's own where the field may run at
// all (a rung above none).
void CellChecks() {
  struct Case {
    gc::GpuClass cls;
    gc::RtTier tier;
    const char* word;
    gi::Level level;
    gi::Layout layout;
  };
  const Case cases[] = {
      // High class, strong: AUTO is HIGH (the standard field); ULTRA the extended one with the per-pixel pass
      {gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", gi::Level::kOff, gi::Layout::kNone},
      {gc::GpuClass::kHigh, gc::RtTier::kStrong, "high", gi::Level::kHigh, gi::Layout::kStandard},
      {gc::GpuClass::kHigh, gc::RtTier::kStrong, "ultra", gi::Level::kUltra, gi::Layout::kExtended},
      {gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", gi::Level::kOff, gi::Layout::kNone},
      // High class, weak (an RTX 3060): AUTO is OFF (the screen-space bounce light stays); by hand the lean field at HIGH, the standard one at ULTRA (probes only)
      {gc::GpuClass::kHigh, gc::RtTier::kWeak, "auto", gi::Level::kOff, gi::Layout::kNone},
      {gc::GpuClass::kHigh, gc::RtTier::kWeak, "high", gi::Level::kHigh, gi::Layout::kLean},
      {gc::GpuClass::kHigh, gc::RtTier::kWeak, "ultra", gi::Level::kUltra, gi::Layout::kStandard},
      {gc::GpuClass::kHigh, gc::RtTier::kWeak, "off", gi::Level::kOff, gi::Layout::kNone},
      // Mid class: the rung is at most weak, even on a strong tier (a card under 6 GB or a slower family)
      {gc::GpuClass::kMid, gc::RtTier::kStrong, "auto", gi::Level::kOff, gi::Layout::kNone},
      {gc::GpuClass::kMid, gc::RtTier::kStrong, "high", gi::Level::kHigh, gi::Layout::kLean},
      {gc::GpuClass::kMid, gc::RtTier::kStrong, "ultra", gi::Level::kUltra, gi::Layout::kStandard},
      {gc::GpuClass::kMid, gc::RtTier::kWeak, "auto", gi::Level::kOff, gi::Layout::kNone},
      {gc::GpuClass::kMid, gc::RtTier::kWeak, "high", gi::Level::kHigh, gi::Layout::kLean},
  };
  for (const Case& c : cases) {
    const gs::Decision d = gs::Decide(c.cls, c.tier, c.word, true);
    Check(d.gate == gs::Gate::kRuns, "the gate is open: " + Name(c.cls, c.tier, c.word, true));
    Check(d.level == c.level, "the level is " + std::string(gi::LevelName(c.level)) + ": " + Name(c.cls, c.tier, c.word, true));
    Check(d.layout == c.layout, "the layout is " + std::string(gi::LayoutName(c.layout)) + ": " + Name(c.cls, c.tier, c.word, true));
    Check(d.layout == gi::LayoutFor(d.rung, d.level), "the layout is gi_frame.h LayoutFor of the rung and the level: " + Name(c.cls, c.tier, c.word, true));
    Check(gi::FieldOn(d.layout) == (d.level != gi::Level::kOff), "a level that is on runs a field: " + Name(c.cls, c.tier, c.word, true));
  }
  // AUTO on the strong tier is exactly where the per-pixel pass is not (HIGH): the per-pixel pass is the ULTRA luxury of a strong card
  const gs::Decision strong_ultra = gs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "ultra", true);
  Check(gi::PixelPassAllowed(strong_ultra.rung, strong_ultra.layout), "a strong card at ULTRA may have the per-pixel pass");
  const gs::Decision weak_ultra = gs::Decide(gc::GpuClass::kHigh, gc::RtTier::kWeak, "ultra", true);
  Check(!gi::PixelPassAllowed(weak_ultra.rung, weak_ultra.layout), "a weak card never has the per-pixel pass");

  // The words that are none of the four are `auto` (an older larecomp.toml, a command line: the setting never fails)
  for (const char* word : {"", "low", "medium", "AUTO", "bogus", "Off"}) {
    for (gc::GpuClass cls : kClasses) {
      for (gc::RtTier tier : kTiers) {
        const gs::Decision a = gs::Decide(cls, tier, word, true);
        const gs::Decision b = gs::Decide(cls, tier, "auto", true);
        Check(a.level == b.level && a.layout == b.layout && a.gate == b.gate, std::string("a word that is no value runs as auto: ") + Name(cls, tier, word, true));
      }
    }
  }

  // THE GATE, over every class, tier, word and state of the update
  for (gc::GpuClass cls : kClasses) {
    for (gc::RtTier tier : kTiers) {
      for (const char* word : kWords) {
        for (bool built : {true, false}) {
          const gs::Decision d = gs::Decide(cls, tier, word, built);
          const std::string name = Name(cls, tier, word, built);
          gs::Gate want = gs::Gate::kRuns;
          if (tier == gc::RtTier::kNone) {
            want = gs::Gate::kNoRt;
          } else if (cls == gc::GpuClass::kLow) {
            want = gs::Gate::kTooSlow;
          } else if (!built) {
            want = gs::Gate::kNotBuilt;
          }
          Check(d.gate == want, "the gate: " + name);
          if (want != gs::Gate::kRuns) {
            Check(d.level == gi::Level::kOff && d.layout == gi::Layout::kNone && !gs::RowEnabled(d.gate), "a shut gate runs nothing and greys the row, whatever the word says: " + name);
          } else {
            Check(gs::RowEnabled(d.gate), "an open gate leaves the row on: " + name);
          }
          // the rung is the class's gating of the tier (rt_tier.h), as gi_frame.h's Rung
          Check(uint8_t(d.rung) == uint8_t(gc::RtRung(cls, tier)), "the rung is RtRung: " + name);
        }
      }
    }
  }

  // The off word is off everywhere and a hand-set word is never overridden where the gate is open
  for (gc::GpuClass cls : kClasses) {
    for (gc::RtTier tier : kTiers) {
      Check(gs::Decide(cls, tier, "off", true).level == gi::Level::kOff, std::string("off is off: ") + Name(cls, tier, "off", true));
      const gs::Decision hi = gs::Decide(cls, tier, "high", true);
      const bool open = hi.gate == gs::Gate::kRuns;
      Check((hi.level == gi::Level::kHigh) == open, std::string("high is high wherever the gate is open: ") + Name(cls, tier, "high", true));
      const gs::Decision ul = gs::Decide(cls, tier, "ultra", true);
      Check((ul.level == gi::Level::kUltra) == open, std::string("ultra is ultra wherever the gate is open: ") + Name(cls, tier, "ultra", true));
    }
  }
}

// ---------------------------------------------------------------------------
// the labels, the reasons, the page
// ---------------------------------------------------------------------------
void LabelChecks() {
  // AUTO (HIGH) only on the High class with a strong tier and the update built; AUTO (OFF) in every other cell
  for (gc::GpuClass cls : kClasses) {
    for (gc::RtTier tier : kTiers) {
      for (bool built : {true, false}) {
        Check(gs::AutoLabel(cls, tier, built) == "AUTO (OFF)", "AUTO's label: " + Name(cls, tier, "auto", built));
      }
    }
  }
  // rt_tier.h's cell and the label agree where the gate is open (the same table)
  for (gc::GpuClass cls : kClasses) {
    for (gc::RtTier tier : kTiers) {
      if (gs::GateFor(cls, tier, true) == gs::Gate::kRuns) {
        Check(gs::AutoLabel(cls, tier, true) == gc::AutoLabelRtFor(cls, tier, gc::RtEffect::kRtGi), "AUTO's label is rt_tier.h's where the row is on: " + Name(cls, tier, "auto", true));
      }
    }
  }
  // the reasons, and the page: "RT BOUNCE LIGHT" with the longest reason, with the page's widest value, is at most the 41 a page can take (pause_menu.cpp "Page width")
  const char* const label = "RT BOUNCE LIGHT";
  size_t widest = 0;
  for (gs::Gate gate : {gs::Gate::kRuns, gs::Gate::kNoRt, gs::Gate::kTooSlow, gs::Gate::kNotBuilt}) {
    const size_t n = std::strlen(label) + std::strlen(gs::RowSuffix(gate));
    widest = std::max(widest, n);
    Check(std::strlen(gs::RowSuffix(gate)) <= 12, std::string("the reason fits a page: '") + gs::RowSuffix(gate) + "'");
  }
  const size_t widest_value = std::strlen("AUTO (DEFAULT)");  // SUN SHAFTS' on the same page
  Check(widest <= 27 && widest + widest_value <= 41, "RT BOUNCE LIGHT with its longest reason fits LIGHTING AND SHADOWS: " + std::to_string(widest) + " + " + std::to_string(widest_value));
  Check(std::strlen("RT BOUNCE LIGHT (GPU TOO SLOW)") + widest_value > 41, "rt_tier.h's ' (GPU TOO SLOW)' would not fit this row's page: that is why the reason here is shorter");
  Check(std::strlen("BOUNCE LIGHT") + std::strlen(gs::kReplacedSuffix) <= 27, "BOUNCE LIGHT (REPLACED) fits the page");
  Check(std::string(gs::kReplacedSuffix) == " (REPLACED)", "the hint's words");
  Check(std::string(gs::RowSuffix(gs::Gate::kRuns)).empty(), "an open gate says nothing");
  Check(std::string(gs::RowSuffix(gs::Gate::kNoRt)) == " (NO RT GPU)" && std::string(gs::RowSuffix(gs::Gate::kNoRt)) == std::string(gc::RtRowSuffix(gc::GpuClass::kHigh, gc::RtTier::kNone)),
        "no ray tracing says (NO RT GPU), rt_tier.h's own words");
  Check(std::string(gc::RtRowSuffix(gc::GpuClass::kLow, gc::RtTier::kWeak)) == " (GPU TOO SLOW)" && std::string(gs::RowSuffix(gs::Gate::kTooSlow)) == " (TOO SLOW)",
        "the shared class says (TOO SLOW) here, where rt_tier.h's longer words do not fit");
  Check(std::string(gs::RowSuffix(gs::Gate::kNotBuilt)) == " (NOT READY)", "the update that is not built says (NOT READY)");
  // the launcher's line under its row
  Check(gs::LauncherAutoLine() == "Auto is Off without ray tracing, Off on a weak ray tracing card and Off on a strong one.", "the launcher's line says what Auto is at each tier: " + gs::LauncherAutoLine());
  Check(gs::LauncherAutoLine() == gc::RtAutoDescription(gc::RtEffect::kRtGi), "... and it is rt_tier.h's RtAutoDescription");
  // the cell and the cvar
  Check(gc::RtCvarName(gc::RtEffect::kRtGi) == "mcla_gpu_rt_gi", "the table's cvar is mcla_gpu_rt_gi");
  Check(gc::RtCell(gc::RtEffect::kRtGi, gc::RtTier::kNone) == "off" && gc::RtCell(gc::RtEffect::kRtGi, gc::RtTier::kWeak) == "off" && gc::RtCell(gc::RtEffect::kRtGi, gc::RtTier::kStrong) == "off",
        "legacy Auto is off on every rung");
  Check(gs::CellsAgree(), "gi_frame.h's AutoLevel is the table's cell");
}

// ---------------------------------------------------------------------------
// the "(REPLACED)" rule, the strength row, the feedback
// ---------------------------------------------------------------------------
gi::GiFrame StandingFrame() {
  gi::GiFrame f;
  f.level = gi::Level::kHigh;
  f.layout = gi::Layout::kStandard;
  f.cascade_count = 3;
  f.valid = gi::kFieldProducts;
  f.slot[gi::kIrradiance] = 10;
  f.slot[gi::kMoments] = 11;
  f.slot[gi::kProbeWeight] = 12;
  f.fade = 1.0f;
  f.safe_range_m = 250.0f;
  return f;
}

void ReplacedChecks() {
  gi::GiFrame f = StandingFrame();
  Check(gs::ReplacedBy(f), "a field that stands with fade 1 replaces the screen-space bounce light");
  f.fade = 0.5f;
  Check(!gs::ReplacedBy(f), "a field that is still fading in does not (both run: a crossfade)");
  f = StandingFrame();
  f.fade = 0.0f;
  Check(!gs::ReplacedBy(f), "a field at fade 0 does not");
  f = StandingFrame();
  f.level = gi::Level::kOff;
  Check(!gs::ReplacedBy(f), "a field whose level is off does not");
  f = StandingFrame();
  f.safe_range_m = 0.0f;
  Check(!gs::ReplacedBy(f), "no structures standing (safe range 0) does not");
  f = StandingFrame();
  f.valid = 0;
  Check(!gs::ReplacedBy(f), "a frame with no field in it (an empty GiFrame) does not");
  f = StandingFrame();
  f.slot[gi::kMoments] = gi::kNoSlot;
  Check(!gs::ReplacedBy(f), "a frame with a product missing does not");
  Check(!gs::ReplacedBy(gi::GiFrame{}), "the frame the field starts each frame with does not");

  // the strength row: on while either bounce light runs
  const gs::Decision on = gs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "high", true);
  const gs::Decision off = gs::Decide(gc::GpuClass::kHigh, gc::RtTier::kWeak, "auto", true);
  Check(gs::AnyBounceLight(false, on), "BOUNCE LIGHT STRENGTH is on with only the probe field running");
  Check(gs::AnyBounceLight(true, off), "... with only the screen-space bounce light running");
  Check(gs::AnyBounceLight(true, on), "... with both");
  Check(!gs::AnyBounceLight(false, off), "... and off with neither");
}

void FeedbackChecks() {
  Check(gs::BounceWeight(0.5) == gi::kBounceWeightDefault && gi::kBounceWeightDefault == 0.5f, "the feedback's default is 0.5 (about two bounces)");
  Check(gs::BounceWeight(0.0) == 0.0f, "0 is strictly one bounce");
  Check(gs::BounceWeight(-3.0) == 0.0f, "below 0 is 0");
  Check(gs::BounceWeight(5.0) == float(gs::kBounceWeightMax) && gs::kBounceWeightMax < 1.0, "the loop cannot grow: 0.95 at most");
  Check(gs::BounceWeight(std::numeric_limits<double>::quiet_NaN()) == gi::kBounceWeightDefault, "not a number is the default");
  Check(gs::BounceWeight(0.95) == float(0.95), "0.95 is allowed");
}

// ---------------------------------------------------------------------------
// the text of the wiring
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}
// the first position of `needle` at or after `from`, npos when none
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t pa = At(text, a);
  const size_t pb = At(text, b);
  return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

void SceneChecks(const std::string& dir) {
  const std::string scene = ReadFile(dir + "/scene.cpp");
  // THE FRAME'S START: the field's frame first (GiField::BeginFrame inside the integration), then the apply (which reads GiField::running() to make its wants), then the G-buffer and the products
  Check(Before(scene, "gi_integration_->BeginFrame(frame_serial_);", "gi_apply_->BeginFrame();"), "ORDER: the field's frame starts before the apply's (scene.cpp BeginFrame)");
  Check(Before(scene, "gi_apply_->BeginFrame();", "gbuffer_->BeginFrame(frame_serial_, pause_fx_candidate);"), "ORDER: ... and the apply's before the G-buffer reads the wants it makes");
  // THE TRIGGER: the field's maintenance, update and weights before the occlusion plans (it asks for the screen-space bounce light's share), the apply after the occlusion's apply
  const std::string ao = "ao_->OnResolve(record, summary, list, UpscalerRunning());";
  Check(Before(scene, "gi_integration_->OnResolve(summary, list, slot_);", ao), "ORDER: the field is published before the occlusion plans its frame (scene.cpp OnResolve)");
  Check(Before(scene, ao, "gi_apply_->OnTrigger(summary, list, gbuffer_published, slot_, raw_gi,"), "ORDER: ... and the apply still follows the occlusion's apply");
  // after the structures' build (the car scene's top level holds the city), which the probe update traces against
  Check(Before(scene, "car_rt_->OnResolve(record, summary, list, slot_);", "gi_integration_->OnResolve(summary, list, slot_);"), "ORDER: the field's trigger work comes after the ray tracing structures' (the top level the update traces)");
  Check(Count(scene, "gi_integration_->") == 3 && Has(scene, "void SetGiIntegration") == false, "scene.cpp calls the integration at the frame's start, at the trigger and (the game's camera cut) at the anchor, once each");
  // THE ANCHOR: a camera cut the game asked for (Scene::NoteAnchorCut sets anchor_cut_ first) goes to the field as a teleport at the next frame's start (CITY-RT-REVIEW M4)
  Check(Before(scene, "NoteAnchorCut(banks);", "gi_integration_->NoteRequestedCut();") && Has(scene, "if (gi_integration_ && anchor_cut_) {"),
        "ORDER: the anchor's requested cut is read (NoteAnchorCut) before the integration is told, and only when there is one");
  const std::string header = ReadFile(dir + "/scene.h");
  Check(Has(header, "void SetGiIntegration(GiIntegration* gi_integration)") && Has(header, "GiIntegration* gi_integration_ = nullptr;") && Has(header, "class GiIntegration;"), "scene.h has the setter, the member and the forward declaration");
}

void IntegrationChecks(const std::string& dir) {
  const std::string cpp = ReadFile(dir + "/gi_integration.cpp");
  // inside the trigger: maintenance, the update, the weights, in the field's own order (gi_field.h THE FRAME)
  const size_t maint = At(cpp, "field_->RecordMaintenance(list, frame_slot)");
  const size_t update = At(cpp, "update_(list, frame_slot, field_->update_frame());");
  const size_t finish = At(cpp, "field_->RecordFinish(list, frame_slot)");
  Check(maint != std::string::npos && update != std::string::npos && finish != std::string::npos && maint < update && update < finish, "ORDER: RecordMaintenance, the probe update, RecordFinish");
  // the world: the first copy of the main view's depth, the 4 sample identity is not the play view
  Check(Has(cpp, "temporal::MainDepthSamples(summary.source_key)") && Has(cpp, "trigger_frame_ == frame_") && Has(cpp, "samples != temporal::kPauseSamples") && Has(cpp, "summary.depth_copy"),
        "the trigger is the first main view depth copy of a frame, and the 4 sample identity (pause, garage, map) is not the play view");
  // BeginFrame: the published frame is read for the hint BEFORE the field's own BeginFrame empties it; the watchdog; the Inputs
  Check(Before(cpp, "gi_setting::NoteReplaced(gi_setting::ReplacedBy(field_->frame()));", "field_->BeginFrame(in);"), "ORDER: the hint is read from the last frame's published field before the field starts a new frame");
  Check(Has(cpp, "apply_->Starved()") && Has(cpp, "apply_->ClearStarved()") && Has(cpp, "frame >= idle_until_") && Has(cpp, "kStarvedHoldFrames"), "the watchdog idles the field and clears the count");
  for (const char* field : {"in.rung = decision.rung;", "in.level = decision.level;", "in.world_ok =", "in.camera[i]", "in.sun_rgb[i]", "in.ambient_up_rgb[i]", "in.ambient_down_rgb[i]", "in.sun_sin_elevation", "in.time_s =",
                            "in.safe_range_m =", "in.strength = GiApply::LevelStrength();", "in.bounce_weight = gi_setting::BounceWeightNow();", "in.frame = frame;"}) {
    Check(Has(cpp, field), std::string("the field's Inputs are filled: ") + field);
  }
  Check(Has(cpp, "in.cut = gi_field::CutFor(cut_pending_, relight_pending_);") && Has(cpp, "cut_pending_ = false;") && Before(cpp, "in.cut = gi_field::CutFor(cut_pending_, relight_pending_);", "field_->BeginFrame(in);") &&
            !Has(cpp, "CHECK G4f"),
        "the field's Inputs carry the game's camera cut once (CutFor) and the request is spent");
  Check(Has(ReadFile(dir + "/gi_integration.h"), "void NoteRequestedCut() { cut_pending_ = true; }") &&
            Has(ReadFile(dir + "/gi_field_rules.h"), "constexpr CutKind CutFor(bool game_cut, bool hour_jump = false) { return game_cut ? CutKind::kTeleport : hour_jump ? CutKind::kRelight : CutKind::kNone; }"),
        "the integration takes the request and the rule maps it to a teleport (a game's cut wins over an hour jump's relight)");
  // THE HOUR JUMP (CITY-RT-REVIEW M4, G4g): the clock's count and the sun's step are tested at the play view's trigger, the relight is handed over with the next frame's Inputs and kept until a frame with a world
  Check(Has(cpp, "if (world_play_ && relight_.Observe(frame_, mcla::gpu::world_clock::HourJumps(), light_ok, to_sun)) {") && Has(cpp, "relight_pending_ = true;") && Has(cpp, "#include \"world_clock.h\"") &&
            Has(cpp, "if (in.world_ok) {\n    relight_pending_ = false;") && Has(cpp, "relight_.Reset();") && Has(cpp, "light_ok = true;"),
        "the integration pairs the clock's hour jumps with the sun's step at the play view's trigger, hands the relight to the next frame's Inputs and spends it where the field has a world");
  Check(Before(cpp, "LatchPoseAndLight();", "return field_->RecordFinish(list, frame_slot);") && Has(ReadFile(dir + "/gi_integration.h"), "gi_field::RelightDetector relight_;") && Has(ReadFile(dir + "/gi_integration.h"), "bool relight_pending_ = false;"),
        "the detector and the pending flag are the integration's");
  {
    const std::string field = ReadFile(dir + "/gi_field.cpp");
    Check(Has(field, "if (move_.fresh || move_.cut || move_.relight) {\n        fade_.Reset();") && Has(field, "Log(move_.cut ? \"cut\" : move_.relight ? \"relight\" : move_.fresh ? \"fresh\" : \"reduced\");"),
          "a relight restarts the fade like a cut (the field shows nothing until it is traced again) and is logged as one");
    const std::string tod = ReadFile(dir + "/../mc_engine/gameplay/time_of_day.cpp");
    Check(Has(tod, "mcla::gpu::world_clock::Publish(ReadF32(base, mgr + kTodCurrent), static_cast<int32_t>(ReadU32(base, kWeatherIndex)));"),
          "the clock's hour jumps are counted in the one call the lighting hook already makes (world_clock.h Publish): the Director's hour, the settings', a mission start's and a skip all pass it");
  }
  Check(Has(cpp, "SceneClockNowMs(frame)") && Has(cpp, "city_->SafeRange(GameHiRadius())") && Has(cpp, "300.0 * gpu_class::CityLodScale"), "the clock is the scene clock's and the safe range is M1's, asked with the game's hi radius");
  Check(Has(cpp, "update_ = nullptr;") && Has(cpp, "gi_setting::NoteUpdateBuilt(static_cast<bool>(update_))"), "the update's seam says the update exists (the setting's gate) and Shutdown takes it away");
  Check(Before(cpp, "gi_setting::NoteUpdateBuilt(static_cast<bool>(update_) || larecomp::settings::mcla_gpu_rt_gi_dry_run.Get());", "const gi_setting::Decision decision = gi_setting::Current();"),
        "ORDER: the dry run opens the gate before the setting is read each frame");
  // the apply: the level's strength and the watchdog
  const std::string apply = ReadFile(dir + "/gi_apply.cpp");
  const std::string apply_h = ReadFile(dir + "/gi_apply.h");
  Check(Has(apply, "float GiApply::LevelStrength() { return LevelNumbers().strength; }") && Has(apply_h, "static float LevelStrength();") && Has(apply_h, "kStarvedFrames = 8") && Has(apply_h, "bool Starved() const"),
        "the apply publishes the level's strength and the watchdog");
  Check(Has(apply, "expect_add_ = !nothing_to_add;") && Has(apply, "starved_frames_ = recorded ? 0u : starved_frames_ + 1u;") && Has(apply, "bool GiApply::OnTriggerImpl("),
        "the watchdog counts only the frames the apply was expected to add its light and drew nothing");
}

void RendererChecks(const std::string& dir) {
  const std::string r = ReadFile(dir + "/renderer.cpp");
  const std::string h = ReadFile(dir + "/renderer.h");
  Check(Has(h, "GiField gi_field_;") && Has(h, "GiIntegration gi_integration_;") && Has(h, "#include \"gi_field.h\"") && Has(h, "#include \"gi_integration.h\""), "renderer.h owns the field and the integration");
  Check(Before(r, "gi_field_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_setting::Current().level != gi::Level::kOff);", "passes_.precache().Start();"),
        "the field's pipelines are listed before the precache starts, when the setting is not off at the start");
  Check(Before(r, "gi_apply_.Initialize(", "gi_apply_.SetGiField(&gi_field_);") && Has(r, "gi_integration_.Initialize(&gi_field_, &gi_apply_, &ao_, &city_rt_);") && Has(r, "scene_.SetGiIntegration(&gi_integration_);"),
        "the apply is given the field, the integration its owners, the scene the integration");
  Check(Has(r, "vram_.AddHolder(\"probe field\", [this] { return gi_field_.HeldBytes(); }, VramSetting::kRtBounce);") && Has(r, "\", probe field \" + mib(gi_field_.HeldBytes())"), "the memory budget and the census line count the field");
  // the end, in both places (a failed start and the normal one): the integration, then the apply that reads the field, then the field
  Check(Count(r, "gi_integration_.Shutdown();") == 2 && Count(r, "gi_field_.Shutdown();") == 2, "both shutdown paths stop the integration and the field");
  size_t from = 0;
  bool ordered = true;
  for (int i = 0; i < 2; ++i) {
    const size_t a = At(r, "gi_integration_.Shutdown();", from);
    const size_t b = At(r, "gi_apply_.Shutdown();", a == std::string::npos ? 0 : a);
    const size_t c = At(r, "gi_field_.Shutdown();", b == std::string::npos ? 0 : b);
    ordered = ordered && a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c;
    from = c == std::string::npos ? r.size() : c + 1;
  }
  Check(ordered, "ORDER: the integration stops first, then the apply that reads the field, then the field (before HostPasses)");
}

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const size_t ssgi = At(menu, "Str(\"PM_RxGpuSsgi\",");
  const size_t rt = At(menu, "Str(\"PM_RxGpuRtGi\", \"mcla_gpu_rt_gi\", \"BOUNCE LIGHT RT: \",");
  const size_t level = At(menu, "Str(\"PM_RxGpuSsgiLevel\",");
  Check(ssgi != std::string::npos && rt != std::string::npos && level != std::string::npos && ssgi < rt && rt < level, "the RT BOUNCE LIGHT row is after BOUNCE LIGHT and before its strength row, on LIGHTING AND SHADOWS");
  Check(Has(menu, "kQualityVals[] = {\"off\", \"high\", \"ultra\"}"), "RT bounce light uses concrete quality levels");
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const size_t begin = At(rules, "[rows.PM_RxGpuRtGi]");
  const size_t next = At(rules, "[rows.PM_RxGpuSsgiLevel]", begin);
  const std::string row = begin == std::string::npos ? "" : rules.substr(begin, next - begin);
  Check(Has(row, "gpu.rt_tier") && Has(row, "d.rt_field_class") && Has(row, "build.rt_gi_update_built") &&
            Has(row, "decode = \"resolved(s.mcla_gpu_rt_gi)\""),
        "the RT BOUNCE LIGHT row uses the GPU, build and Auto rules");
  Check(Has(rules, "live.rt_gi_replaces_bounce") && Has(rules, " (REPLACED)") &&
            Has(menu, "gpu::RtGiReplacesBounceLight()"),
        "the BOUNCE LIGHT row reads (REPLACED) while the field stands");
  Check(Has(rules, "bounce_on =") && Has(rules, "resolved(s.mcla_gpu_ssgi)") && Has(rules, "resolved(s.mcla_gpu_rt_gi)"),
        "BOUNCE LIGHT STRENGTH is on while either bounce light runs");
  Check(!row.empty() && !Has(row, "d.ao_on"), "RT BOUNCE LIGHT is independent of the ambient occlusion");

}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  // (Written in pieces: tools/check_settings_exposure.py takes a literal definition macro anywhere in src for a setting definition.)
  const std::string define = std::string("REX_SET") + "TING(";
  Check(Has(header, define + "std::string, mcla_gpu_rt_gi, \"off\", \"") && Has(header, "rex::Allowed(\"auto\", \"off\", \"high\", \"ultra\"));") && Has(header, "inline auto& mcla_gpu_rt_gi = g_gpu.mcla_gpu_rt_gi;"),
        "mcla_gpu_rt_gi is a string setting, off by default, with the four words, live (no restart trait), and has its alias");
  const size_t at = At(header, define + "std::string, mcla_gpu_rt_gi, \"off\"");
  const size_t end = at == std::string::npos ? 0 : header.find(");", at);
  const std::string def = at == std::string::npos ? "" : header.substr(at, end - at);
  Check(!def.empty() && !Has(def, "kRestart") && !Has(def, "kDebugOnly") && Has(def, "Live."), "the setting is live, not debug-only, and its help names the row");
  Check(Has(header, define + "bool, mcla_gpu_rt_gi_dry_run, false, \"") && Has(header, "inline auto& mcla_gpu_rt_gi_dry_run = g_gpu.mcla_gpu_rt_gi_dry_run;") && Has(manifest, "\"mcla_gpu_rt_gi_dry_run\": {") &&
            Has(meta, "{\"mcla_gpu_rt_gi_dry_run\", \"MCLA/GPU\"},"),
        "mcla_gpu_rt_gi_dry_run is a debug-only switch in the three settings files");
  Check(Has(header, define + "f64, mcla_gpu_rt_gi_bounces, 0.5, \"") && Has(header, "rex::kDebugOnly, rex::Range{0.0, 0.95});") && Has(header, "inline auto& mcla_gpu_rt_gi_bounces = g_gpu.mcla_gpu_rt_gi_bounces;"),
        "mcla_gpu_rt_gi_bounces is a debug-only number, 0.5 by default, 0 to 0.95, with its alias");
  Check(Has(manifest, "\"mcla_gpu_rt_gi\": {") && Has(manifest, "\"mcla_gpu_rt_gi_bounces\": {") && Has(meta, "{\"mcla_gpu_rt_gi\", \"MCLA/GPU\"},") && Has(meta, "{\"mcla_gpu_rt_gi_bounces\", \"MCLA/GPU\"},"), "both are in the manifest and the meta list");
  const size_t mat = At(manifest, "\"mcla_gpu_rt_gi\": {");
  const size_t mend = mat == std::string::npos ? 0 : manifest.find("\"comments\"", mat);
  const std::string entry = mat == std::string::npos ? "" : manifest.substr(mat, mend - mat);
  Check(Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"off\\\"\"") && Has(entry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"high\\\", \\\"ultra\\\")") && !Has(entry, "kDebugOnly"),
        "the manifest entry is the same setting");
  // the help in the manifest is the header's text
  Check(!def.empty() && Has(manifest, "Traces bounced light from city geometry.") && Has(def, "Traces bounced light from city geometry."),
        "the help reads the same in the manifest and the header");
  // the meta order follows the header's (view, then the feedback)
  Check(Before(meta, "{\"mcla_gpu_rt_gi\", \"MCLA/GPU\"},", "{\"mcla_gpu_rt_gi_view\", \"MCLA/GPU\"},") && Before(meta, "{\"mcla_gpu_rt_gi_view\", \"MCLA/GPU\"},", "{\"mcla_gpu_rt_gi_bounces\", \"MCLA/GPU\"},"), "the meta list's order follows the header's");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "    gi_integration.cpp\n") && Has(cmake, "    gi_setting.cpp\n"), "the library builds gi_integration.cpp and gi_setting.cpp");
  Check(Has(cmake, "add_executable(larecomp_gi_setting_test EXCLUDE_FROM_ALL gi_setting_test.cpp)"), "the test has its CMake target");
  const std::string gpu_h = ReadFile(dir + "/gpu.h");
  Check(Has(gpu_h, "bool RtGiUpdateBuilt();") && Has(gpu_h, "bool RtGiReplacesBounceLight();"), "gpu.h declares what the pause menu asks");
  const std::string cpp = ReadFile(dir + "/gi_setting.cpp");
  Check(Has(cpp, "LiveText text(\"mcla_gpu_rt_gi\");") && Has(cpp, "gc::Known() ? gc::Current() : gc::GpuClass::kLow") && Has(cpp, "bool RtGiReplacesBounceLight()") && Has(cpp, "bool RtGiUpdateBuilt()"),
        "the setting is read as a LiveText, the class as the weakest until it is known, and the two flags are answered");
}

std::string DirOfThisFile() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = DirOfThisFile();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v") {
      g_verbose = true;
    } else {
      dir = a;
    }
  }
  CellChecks();
  LabelChecks();
  ReplacedChecks();
  FeedbackChecks();
  SceneChecks(dir);
  IntegrationChecks(dir);
  RendererChecks(dir);
  MenuChecks(dir);
  SettingsChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
