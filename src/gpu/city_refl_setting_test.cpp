// Check of the city reflections' setting and its pause row (city_refl_setting.h, pause_selfrefl_row.h RayTracedRuns): CITY-RT M2 package R8. A program of its own: it needs no renderer, no game files and no
// GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_city_refl_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/city_refl_setting_test.cpp src/gpu/city_refl_setting.cpp
//   run: larecomp_city_refl_setting_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check)
//
// It checks, against a model written out here (not computed from the code under test): what each word of mcla_gpu_rt_reflections comes to on every graphics class and ray tracing tier (the owner's rule: AUTO is
// HIGH where the card traces quickly, OFF on every other; a hand-set level runs wherever the card can trace and is never overridden except by the gate), the gate's order and what each state says and allows, that
// every state fits the REFLECTIONS page (41 characters of label and value), how the state of the city's files becomes a word on the row, that the row follows the ray traced CAR SELF-REFLECTIONS row for every
// pair of its settings, AUTO's label and the launcher's line; and then the TEXT of the wiring: the row's place and constructor in pause_menu.cpp, the setting in the three settings files (live, not debug-only),
// the scenes' publishing of their probes, the host's order (the wants go out before the pass waits for the data), and the CMake lines.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "city_refl_setting.h"
#include "rt_tier.h"

namespace crs = mcla::gpu::city_refl_setting;
namespace cr = mcla::gpu::city_refl;
namespace cm = mcla::gpu::city_rt;
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
constexpr crs::Data kData[] = {crs::Data::kUnknown, crs::Data::kReady, crs::Data::kBuilding, crs::Data::kUnavailable};
const char* const kWords[] = {"auto", "off", "high", "ultra", "", "low", "medium", "AUTO", "bogus"};

std::string Name(gc::GpuClass c, gc::RtTier t, const std::string& word) {
  return std::string("class ") + std::string(gc::ClassName(c)) + ", tier " + std::string(gc::RtTierName(t)) + ", word '" + word + "'";
}

// ---------------------------------------------------------------------------
// the model: the owner's rule, written out
// ---------------------------------------------------------------------------

// The rung: no ray tracing for the Low class or a card that does not trace; a Mid class is at most weak; High is the card's own tier.
enum class Rung { kNone, kWeak, kStrong };
Rung RungOf(gc::GpuClass, gc::RtTier t) {
  if (t == gc::RtTier::kNone) return Rung::kNone;
  if (t == gc::RtTier::kWeak) return Rung::kWeak;
  return Rung::kStrong;
}

// What a word comes to: off is off, high and ultra are the player's own wherever the card can trace at all, anything else is `auto`, which is HIGH on a strong card and OFF on every other.
cm::Level ExpectedLevel(Rung rung, gc::RtTier tier, const std::string& word) {
  if (tier == gc::RtTier::kNone) return cm::Level::kOff;
  if (word == "off") return cm::Level::kOff;
  if (word == "high") return cm::Level::kHigh;
  if (word == "ultra") return cm::Level::kUltra;
  return cm::Level::kOff;
}

crs::Gate ExpectedGate(gc::GpuClass, gc::RtTier t, bool car_rt_on, bool wanted, crs::Data data) {
  if (t == gc::RtTier::kNone) return crs::Gate::kNoRt;
  if (!car_rt_on) return crs::Gate::kNeedsCarRt;
  if (wanted && data == crs::Data::kUnavailable) return crs::Gate::kNotReady;
  if (wanted && data == crs::Data::kBuilding) return crs::Gate::kBuilding;
  return crs::Gate::kRuns;
}

void CellChecks() {
  int combos = 0;
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      for (const char* w : kWords) {
        const std::string word = w;
        const Rung rung = RungOf(c, t);
        const cm::Level expect_wanted = ExpectedLevel(rung, t, word);
        for (const bool car : {false, true}) {
          for (const crs::Data data : kData) {
            ++combos;
            const crs::Decision d = crs::Decide(c, t, word, car, data);
            const crs::Gate gate = ExpectedGate(c, t, car, expect_wanted != cm::Level::kOff, data);
            const std::string name = Name(c, t, word) + (car ? ", car RT on" : ", car RT off") + ", data " + std::string(crs::DataName(data));
            Check(d.wanted == expect_wanted, name + ": the word comes to " + std::string(cm::LevelName(expect_wanted)));
            Check(d.gate == gate, name + ": the gate is as the order says");
            // what runs: the word, only while the gate is open and the data stands
            const cm::Level expect_runs = (gate == crs::Gate::kRuns && data == crs::Data::kReady) ? expect_wanted : cm::Level::kOff;
            Check(d.level == expect_runs, name + ": what runs is " + std::string(cm::LevelName(expect_runs)));
            Check(d.data == data, name + ": the data's state is kept");
          }
        }
      }
    }
  }
  Check(combos == 3 * 3 * 9 * 2 * 4, "every class, tier, word, car state and data state was looked at");

  // the owner's rule in plain sentences
  Check(crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", true, crs::Data::kReady).level == cm::Level::kOff, "legacy Auto leaves city reflections off on a strong card");
  Check(crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kWeak, "auto", true, crs::Data::kReady).level == cm::Level::kOff, "AUTO on a weak card runs nothing");
  Check(crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kNone, "auto", true, crs::Data::kReady).level == cm::Level::kOff, "AUTO without ray tracing runs nothing");
  Check(crs::Decide(gc::GpuClass::kMid, gc::RtTier::kStrong, "auto", true, crs::Data::kReady).level == cm::Level::kOff, "AUTO on a Mid class runs nothing (its rung is at most weak)");
  Check(crs::Decide(gc::GpuClass::kMid, gc::RtTier::kWeak, "high", true, crs::Data::kReady).level == cm::Level::kHigh, "a weak card by hand runs HIGH (the Mid class's cell)");
  Check(crs::Decide(gc::GpuClass::kMid, gc::RtTier::kWeak, "ultra", true, crs::Data::kReady).level == cm::Level::kUltra, "a weak card by hand runs ULTRA");
  for (std::string_view word : {"on", "auto", "high", "ultra"}) {
    Check(crs::DecidePaint(gc::GpuClass::kMid, gc::RtTier::kWeak, word, true, crs::Data::kReady).level == cm::Level::kHigh,
          "an enabled paint setting has the same single level on a weak ray-query device");
  }
  Check(crs::DecidePaint(gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", true, crs::Data::kReady).level == cm::Level::kOff,
        "the paint city setting keeps explicit Off");
  Check(crs::DecidePaint(gc::GpuClass::kMid, gc::RtTier::kNone, "on", true, crs::Data::kReady).level == cm::Level::kOff,
        "city paint needs ray queries");
  Check(crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", true, crs::Data::kReady).level == cm::Level::kOff, "OFF is off on a strong card");
  Check(crs::Decide(gc::GpuClass::kLow, gc::RtTier::kStrong, "ultra", true, crs::Data::kReady).level == cm::Level::kUltra, "the Low class can run the selected RT level");
  // the row's AUTO is the table's cell; the table is rt_tier.h's
  Check(std::string(gc::RtCell(gc::RtEffect::kRtReflections, gc::RtTier::kNone)) == "off" && std::string(gc::RtCell(gc::RtEffect::kRtReflections, gc::RtTier::kWeak)) == "off" &&
            std::string(gc::RtCell(gc::RtEffect::kRtReflections, gc::RtTier::kStrong)) == "off",
        "rt_tier.h's cells of mcla_gpu_rt_reflections: off on all rungs");
  // the words are the player's own: a word is never changed into another by the gate (only the level that runs is off)
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      const crs::Decision d = crs::Decide(c, t, "ultra", false, crs::Data::kReady);
      Check(d.wanted == (t == gc::RtTier::kNone ? cm::Level::kOff : cm::Level::kUltra), Name(c, t, "ultra") + ": the player's word is still what it comes to while the car's ray tracing is off");
    }
  }
}

// ---------------------------------------------------------------------------
// the gate's words, the page's width
// ---------------------------------------------------------------------------

constexpr crs::Gate kGates[] = {crs::Gate::kRuns, crs::Gate::kNoRt, crs::Gate::kTooSlow, crs::Gate::kNeedsCarRt, crs::Gate::kNotReady, crs::Gate::kBuilding};

std::string LabelOf(crs::Gate g) { return std::string(crs::RowLabel(g)) + crs::RowSuffix(g); }

void RowChecks() {
  Check(LabelOf(crs::Gate::kRuns) == "RT CITY REFLECTIONS", "the row is RT CITY REFLECTIONS while it is on");
  Check(LabelOf(crs::Gate::kNoRt) == "RT CITY (NO RT GPU)", "no ray tracing card: RT CITY (NO RT GPU)");
  Check(LabelOf(crs::Gate::kTooSlow) == "RT CITY (TOO SLOW)", "the Low class: RT CITY (TOO SLOW)");
  Check(LabelOf(crs::Gate::kNeedsCarRt) == "RT CITY (NEEDS CAR RT)", "the car's ray traced reflection is off: RT CITY (NEEDS CAR RT)");
  Check(LabelOf(crs::Gate::kNotReady) == "RT CITY (NOT READY)", "the data cannot be had: RT CITY (NOT READY)");
  Check(LabelOf(crs::Gate::kBuilding) == "RT CITY (BUILDING DATA)", "the data is being built: RT CITY (BUILDING DATA)");
  // grey: the four that cannot run; building is not grey (the setting works and waits)
  Check(crs::RowEnabled(crs::Gate::kRuns) && crs::RowEnabled(crs::Gate::kBuilding), "the row is on while it runs and while it waits for the data it builds");
  Check(!crs::RowEnabled(crs::Gate::kNoRt) && !crs::RowEnabled(crs::Gate::kTooSlow) && !crs::RowEnabled(crs::Gate::kNeedsCarRt) && !crs::RowEnabled(crs::Gate::kNotReady), "the four states that cannot run are grey");
  for (const crs::Gate g : kGates) {
    const std::string suffix = crs::RowSuffix(g);
    const std::string name = std::string("state ") + std::string(crs::GateName(g));
    Check(suffix.empty() == (g == crs::Gate::kRuns), name + ": only the open row has no reason");
    Check(suffix.empty() || (suffix.size() > 3 && suffix.substr(0, 2) == " (" && suffix.back() == ')'), name + ": the reason is ' (...)' like every other suffix");
    Check(suffix.find("LOW") == std::string::npos || g == crs::Gate::kTooSlow, name + ": nothing is called low");
    Check(!std::string(crs::GateName(g)).empty(), name + ": it has a name for the log");
  }

  for (const auto gate : kGates) {
    Check(LabelOf(gate).size() + std::string_view("ULTRA").size() <= 41,
          "the city paint RT row fits alongside the concrete quality levels");
  }

  // what a state is for the same (class, tier): the row greys exactly where the gate says
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      for (const bool car : {false, true}) {
        const crs::Decision d = crs::Decide(c, t, "auto", car, crs::Data::kReady);
        const bool can_run = t != gc::RtTier::kNone && car;
        Check(crs::RowEnabled(d.gate) == can_run, Name(c, t, "auto") + (car ? ", car RT on" : ", car RT off") + ": the row is on exactly where the card can trace and the car's pass runs");
      }
    }
  }

  // AUTO's label says what it comes to on the card; the launcher's line
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      const std::string expect = ExpectedLevel(RungOf(c, t), t, "auto") == cm::Level::kHigh ? "AUTO (HIGH)" : "AUTO (OFF)";
      Check(crs::AutoLabel(c, t) == expect, Name(c, t, "auto") + ": AUTO's label is " + expect);
    }
  }
  Check(crs::LauncherAutoLine() == "Auto is Off without ray tracing, Off on a weak ray tracing card and Off on a strong one.", "the launcher's line under the row: " + crs::LauncherAutoLine());
}

// ---------------------------------------------------------------------------
// the data's state
// ---------------------------------------------------------------------------

void DataChecks() {
  using S = cm::PackState;
  const struct {
    S state;
    bool can_build;
    crs::Data expect;
  } table[] = {
      {S::kCurrent, true, crs::Data::kReady},       {S::kCurrent, false, crs::Data::kReady},
      {S::kBuilding, true, crs::Data::kBuilding},   {S::kBuilding, false, crs::Data::kBuilding},
      {S::kMissing, true, crs::Data::kBuilding},    {S::kMissing, false, crs::Data::kUnavailable},
      {S::kStale, true, crs::Data::kBuilding},      {S::kStale, false, crs::Data::kUnavailable},
      {S::kNoSource, true, crs::Data::kUnavailable}, {S::kNoSource, false, crs::Data::kUnavailable},
      {S::kFailed, true, crs::Data::kUnavailable},  {S::kFailed, false, crs::Data::kUnavailable},
  };
  for (const auto& e : table) {
    Check(crs::DataOf(e.state, e.can_build) == e.expect,
          std::string("the pack is ") + std::string(cm::PackStateName(e.state)) + (e.can_build ? ", the app can build" : ", no build to start") + ": " + std::string(crs::DataName(e.expect)));
  }
  // the two stages as one: the pack decides first, the hit data after
  for (const crs::Data base : kData) {
    for (const crs::Data hit : kData) {
      crs::Data expect = hit;
      if (base == crs::Data::kUnavailable) expect = crs::Data::kUnavailable;
      else if (base == crs::Data::kBuilding) expect = crs::Data::kBuilding;
      else if (base == crs::Data::kUnknown) expect = crs::Data::kUnknown;
      Check(crs::Combine(base, hit) == expect, std::string("pack ") + std::string(crs::DataName(base)) + ", hit data " + std::string(crs::DataName(hit)) + ": " + std::string(crs::DataName(expect)));
    }
  }
  // the process's flags (city_refl_setting.cpp): unknown at the start, then the two stages as the scenes publish them, read as one
  Check(crs::DataNow() == crs::Data::kUnknown, "the flags start unknown");
  for (const crs::Data base : kData) {
    for (const crs::Data hit : kData) {
      crs::NoteBase(base);
      crs::NoteHit(hit);
      Check(crs::DataNow() == crs::Combine(base, hit), std::string("published: pack ") + std::string(crs::DataName(base)) + ", hit data " + std::string(crs::DataName(hit)) + " read as one");
    }
  }
  crs::NoteBase(crs::Data::kUnknown);
  crs::NoteHit(crs::Data::kUnknown);
  Check(crs::DataNow() == crs::Data::kUnknown, "both unknown again after shutdown");

  // The first run, step by step, on a strong card with the car's ray traced reflection on and the word at its default: what the row says and whether the pass runs.
  struct Step {
    crs::Data base, hit;
    crs::Gate gate;
    bool runs;
  };
  const Step run[] = {
      {crs::Data::kUnknown, crs::Data::kUnknown, crs::Gate::kRuns, false},          // before the first probe: the row is on, nothing runs yet
      {crs::Data::kBuilding, crs::Data::kUnknown, crs::Gate::kBuilding, false},     // the pack is being built
      {crs::Data::kReady, crs::Data::kUnknown, crs::Gate::kRuns, false},            // the pack is open, the hit data not asked about yet
      {crs::Data::kReady, crs::Data::kBuilding, crs::Gate::kBuilding, false},       // the hit data is being built
      {crs::Data::kReady, crs::Data::kReady, crs::Gate::kRuns, true},               // both stand: the city is in the paint
      {crs::Data::kReady, crs::Data::kUnavailable, crs::Gate::kNotReady, false},    // the hit data cannot be had (a failed build)
      {crs::Data::kUnavailable, crs::Data::kUnknown, crs::Gate::kNotReady, false},  // no game archive: not this session
  };
  for (const Step& s : run) {
    const crs::Data data = crs::Combine(s.base, s.hit);
    const crs::Decision d = crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "high", true, data);
    Check(d.gate == s.gate && (d.level != cm::Level::kOff) == s.runs,
          std::string("first run: pack ") + std::string(crs::DataName(s.base)) + ", hit data " + std::string(crs::DataName(s.hit)) + " -> " + std::string(crs::GateName(d.gate)) + (s.runs ? ", the pass runs" : ", the pass waits"));
  }
  // a word that comes to off has no business with the data: the row stays on and the wants stay off
  for (const crs::Data data : kData) {
    const crs::Decision d = crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", true, data);
    Check(d.gate == crs::Gate::kRuns && d.level == cm::Level::kOff && d.wanted == cm::Level::kOff, std::string("OFF with the data ") + std::string(crs::DataName(data)) + ": the row is on, nothing is wanted");
    const crs::Decision w = crs::Decide(gc::GpuClass::kMid, gc::RtTier::kWeak, "auto", true, data);
    Check(w.gate == crs::Gate::kRuns && w.wanted == cm::Level::kOff, std::string("AUTO on a weak card with the data ") + std::string(crs::DataName(data)) + ": the row is on, nothing is wanted");
  }
}

// ---------------------------------------------------------------------------
// the row follows the CAR SELF-REFLECTIONS row
// ---------------------------------------------------------------------------

void InterplayChecks() {
  int pairs = 0;
  for (const char* selfrefl : {"auto", "off", "on", "low", ""}) {
    for (const char* rt : {"auto", "off", "on", "maybe", ""}) {
      for (const bool auto_on : {false, true}) {
        for (const bool available : {false, true}) {
          ++pairs;
          const std::string_view base(selfrefl);
          const bool car = available && std::string_view(rt) == "on" && (base == "on" || (base == "auto" && auto_on));
          const crs::Decision d = crs::Decide(gc::GpuClass::kHigh, gc::RtTier::kStrong, "high", car, crs::Data::kReady);
          const std::string name = std::string("CAR SELF-REFLECTIONS ") + selfrefl + "/" + rt + (auto_on ? ", auto on" : ", auto off") + (available ? ", can run" : ", cannot run");
          Check((d.gate == crs::Gate::kNeedsCarRt) == !car, name + ": the city row is grey with NEEDS CAR RT exactly while the car's ray traced reflection is not running");
          Check((d.level == cm::Level::kHigh) == car, name + ": the city runs exactly while the car's ray traced reflection does");
        }
      }
    }
  }
  Check(pairs == 5 * 5 * 2 * 2, "every pair of the car row's two settings was looked at");
  // the value the car row writes for RAY TRACED turns the city row on again (no other value of it does, except AUTO where AUTO comes to ray traced)

}

// ---------------------------------------------------------------------------
// the text of the wiring
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("cannot open %s\n", path.c_str());
    return "";
  }
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
int Count_(const std::string& text, const std::string& what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}
size_t At(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t x = At(text, a), y = At(text, b);
  return x != std::string::npos && y != std::string::npos && x < y;
}

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const size_t items = At(menu, "const ItemDef kReflectionItems[] = {");
  const size_t items_end = items == std::string::npos ? std::string::npos : At(menu, "\n};", items);
  const std::string page = items == std::string::npos || items_end == std::string::npos ? "" : menu.substr(items, items_end - items);
  Check(Before(page, "\"PM_RxGpuCarSelfReflRt\"", "\"PM_RxGpuRtCityRefl\"") &&
            Before(page, "\"PM_RxGpuRtCityRefl\"", "\"PM_RxGpuSideMirrors\""),
        "the city paint row is grouped with the traced car paint");
  Check(Has(page, "NativeOnly(Str(\"PM_RxGpuRtCityRefl\", \"mcla_gpu_rt_reflections\", \"CITY IN CAR PAINT RT: \""),
        "city paint is a native-only RT row");
  Check(Has(page, "kOnOffVals, kOnOffNames, 2"), "the row offers Off and On");
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const size_t begin = At(rules, "[rows.PM_RxGpuRtCityRefl]");
  const size_t next = At(rules, "[rows.PM_RxGpuSideMirrors]", begin);
  const std::string row = begin == std::string::npos ? "" : rules.substr(begin, next - begin);
  Check(Has(row, "d.car_rt_runs") && Has(row, "live.city_data") && Has(row, "decode = \"d.rt_city_choice\"") &&
            Has(menu, "city_refl_setting::DataNow()"),
        "the row evaluates the car reflection and the live city-data facts");
  Check(Has(rules, "car_rt_runs =") && Has(rules, "s.mcla_gpu_car_selfrefl_rt"),
        "the city gate uses the separate traced car self-reflections setting");
  Check(!row.empty() && !Has(row, "shadows") && !Has(row, "ao_on"),
        "city paint reflections are independent of shadows and occlusion");

}

// The text of one setting's definition in settings_gpu.h: from the text that finds it to the next definition (a help text can hold ");").
std::string DefinitionOf(const std::string& header, const std::string& start) {
  const size_t at = At(header, start);
  if (at == std::string::npos) return "";
  const size_t next = header.find("\n  REX_SET" "TING(", at);
  return header.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  // (Written in pieces: tools/check_settings_exposure.py takes a literal definition macro anywhere in src for a setting definition.)
  const std::string define = std::string("REX_SET") + "TING(";
  const std::string def = DefinitionOf(header, define + "std::string, mcla_gpu_rt_reflections, \"off\"");
  Check(!def.empty() && Has(def, "rex::Allowed(\"auto\", \"off\", \"on\", \"high\", \"ultra\")"), "the city row accepts On and reads legacy saves");
  Check(!def.empty() && !Has(def, "kDebugOnly") && !Has(def, "kRestart") && Has(def, "Live."), "it is not debug-only any more (it has its row), and it is live (no restart trait)");
  Check(Has(header, "inline auto& mcla_gpu_rt_reflections = g_gpu.mcla_gpu_rt_reflections;") && Has(meta, "{\"mcla_gpu_rt_reflections\", \"MCLA/GPU\"},"), "the alias and the meta list have it");
  const size_t mat = At(manifest, "\"mcla_gpu_rt_reflections\": {");
  const size_t mend = mat == std::string::npos ? 0 : manifest.find("\"comments\"", mat);
  const std::string entry = mat == std::string::npos ? "" : manifest.substr(mat, mend - mat);
  Check(!entry.empty() && Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"off\\\"\"") && Has(entry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"on\\\", \\\"high\\\", \\\"ultra\\\")") && !Has(entry, "kDebugOnly") &&
            Has(entry, "City in Car Paint RT needs"),
        "the manifest entry is the same setting: live, not debug-only, with the row named in its help");
  // the debug settings around it stay debug-only
  for (const char* name : {"mcla_gpu_city_refl_screen", "mcla_gpu_city_refl_coat", "mcla_gpu_city_refl_reach", "mcla_gpu_city_refl_view"}) {
    const std::string d = DefinitionOf(header, std::string(", ") + name + ", ");
    Check(!d.empty() && Has(d, "kDebugOnly"), std::string(name) + " stays debug-only");
  }
}

void SceneChecks(const std::string& dir) {
  const std::string pack = ReadFile(dir + "/city_rt_scene.cpp");
  const std::string hit = ReadFile(dir + "/city_hit_scene.cpp");
  Check(Has(pack, "#include \"city_refl_setting.h\"") && Has(pack, "city_refl_setting::NoteBase(city_refl_setting::DataOf(state, bool(request_build_)));"), "city_rt_scene.cpp publishes the pack's probe answer, with whether the app can build");
  Check(Before(pack, "city_refl_setting::NoteBase(city_refl_setting::DataOf(state, bool(request_build_)));", "if (!city_rt::CanOpenPack(state)) {"), "...at the probe, before it decides what to do with it");
  Check(Has(pack, "city_refl_setting::NoteBase(city_refl_setting::Data::kUnavailable);") && Has(pack, "city_refl_setting::NoteBase(city_refl_setting::Data::kReady);") && Has(pack, "city_refl_setting::NoteBase(city_refl_setting::Data::kUnknown);"),
        "the pack is unavailable when it cannot be read, ready when it is open, unknown again at shutdown");
  Check(Before(pack, "city_refl_setting::NoteBase(city_refl_setting::Data::kReady);", "hit_->SetBase(&pack_);"), "the pack is ready before the hit scene is given it (SetBase resets the hit data's state)");
  Check(Has(hit, "#include \"city_refl_setting.h\"") && Has(hit, "city_refl_setting::NoteHit(city_refl_setting::DataOf(state, bool(request_build_)));"), "city_hit_scene.cpp publishes the hit data's probe answer, with whether the app can build");
  Check(Before(hit, "city_refl_setting::NoteHit(city_refl_setting::DataOf(state, bool(request_build_)));", "if (!step.open) {"), "...at the probe, before the build is asked for");
  Check(Has(hit, "city_refl_setting::NoteHit(city_refl_setting::Data::kUnavailable);") && Has(hit, "city_refl_setting::NoteHit(city_refl_setting::Data::kReady);") && Count_(hit, "city_refl_setting::NoteHit(city_refl_setting::Data::kUnknown);") == 2,
        "the hit data is unavailable when it cannot be read, ready when it is open, unknown again at shutdown and when the base pack changes");
}

void HostChecks(const std::string& dir) {
  const std::string host = ReadFile(dir + "/city_refl.cpp");
  const std::string host_h = ReadFile(dir + "/city_refl.h");
  Check(Has(host, "const auto decide = one_ray ? crs::DecidePaint : crs::Decide;") && Has(host, "LiveText text(\"mcla_gpu_rt_reflections\")"),
        "the host decides with the same function the menu uses: the setting read live, the pass running (the car's ray traced reflection on), the data's state");
  // the wants go out before the pass waits for the data (they make the app build it), the cell only once the data stands
  const size_t want = At(host, "service_->Want(rt_scene::Consumer::kCityReflections, needs);");
  const size_t wait = At(host, "if (decision.level == cr::Level::kOff) {\n    return;");
  const size_t cell = At(host, "  cell_ = cell;\n  cell_.coat_layer");
  const size_t active = At(host, "pass_active_ = true;");
  Check(want != std::string::npos && wait != std::string::npos && cell != std::string::npos && active != std::string::npos && want < wait && wait < cell && cell < active,
        "ORDER: the wants go out, then the pass waits for the data, then the cell and the pass are set");
  Check(Has(host, "if (decision.wanted == cr::Level::kOff) {\n    return;\n  }") && Has(host, "const cr::Wants want = cr::WantsFor(cell);"), "a word that comes to off wants nothing; the wants are the cell's");
  Check(Has(host, "void CityRefl::NoteGate(const city_refl_setting::Decision& decision)") && Has(host, "city_refl_setting::GateName(decision.gate)") && Has(host_h, "void NoteGate(const city_refl_setting::Decision& decision);") && Has(host_h, "int last_gate_state_ = -1;"),
        "the log says one line when the state changes");
  Check(Has(host_h, "#include \"city_refl_setting.h\"") && !Has(host_h, "debug-only until R8"), "city_refl.h includes the setting and no longer says the setting is debug-only");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "    city_refl.cpp\n    city_refl_setting.cpp\n"), "the library builds city_refl_setting.cpp");
  Check(Has(cmake, "add_executable(larecomp_city_refl_setting_test EXCLUDE_FROM_ALL city_refl_setting_test.cpp city_refl_setting.cpp)"), "the test has its CMake target, with the two flags' source");
  const std::string cpp = ReadFile(dir + "/city_refl_setting.cpp");
  Check(Has(cpp, "void NoteBase(Data data)") && Has(cpp, "void NoteHit(Data data)") && Has(cpp, "Data DataNow() { return Combine(Load(g_base), Load(g_hit)); }") && Has(cpp, "std::atomic<uint8_t> g_base{static_cast<uint8_t>(Data::kUnknown)};"),
        "the two flags are atomics, unknown at the start, and DataNow is the two as one");
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
  RowChecks();
  DataChecks();
  InterplayChecks();
  MenuChecks(dir);
  SettingsChecks(dir);
  SceneChecks(dir);
  HostChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
