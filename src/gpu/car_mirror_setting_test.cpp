// Check of the ray traced side mirrors' setting and its pause row (car_mirror_setting.h, car_mirror_rt_rules.h Wanted): MIRROR-ROW. A program of its own: it needs no renderer, no game files and no GPU,
// and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_car_mirror_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/car_mirror_setting_test.cpp
//   run: larecomp_car_mirror_setting_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check)
//
// It checks, against a model written out here (not computed from the code under test): what each word of mcla_gpu_car_mirror_rt comes to on every graphics class and ray tracing tier (the owner's rule:
// AUTO is ON where the card traces quickly, OFF on every other; a hand-set ON runs on any card that traces and is not of the Low class; OFF is off), that the row's answer is the renderer's own (the pass's
// Wanted with the tier's class, for every pair), the gate's order and what each state says and allows, that every state fits the REFLECTIONS page (41 characters of label and value), AUTO's label and the
// launcher's line; and then the TEXT of the wiring: the row's place and constructor in pause_menu.cpp, the setting in the settings files (live, not debug-only, no longer on the exposure list), the host's
// reading and the CMake lines.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "car_mirror_rt_rules.h"
#include "car_mirror_setting.h"
#include "rt_tier.h"

namespace cs = mcla::gpu::car_mirror_setting;
namespace cm = mcla::gpu::car_mirror;
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
const char* const kWords[] = {"auto", "off", "on", "", "high", "ultra", "low", "AUTO", "On", "bogus", "auto "};

std::string Name(gc::GpuClass c, gc::RtTier t, const std::string& word) {
  return std::string("class ") + std::string(gc::ClassName(c)) + ", tier " + std::string(gc::RtTierName(t)) + ", word '" + word + "'";
}

// ---------------------------------------------------------------------------
// the model: the owner's rule, written out
// ---------------------------------------------------------------------------

// The rung: the tier, no more than weak on a Mid class card, none on the Low class.
gc::RtTier ModelRung(gc::GpuClass c, gc::RtTier t) {
  if (c == gc::GpuClass::kLow) return gc::RtTier::kNone;
  if (c == gc::GpuClass::kMid && t == gc::RtTier::kStrong) return gc::RtTier::kWeak;
  return t;
}

// What a word comes to: off is off; on runs wherever a card traces and is not of the Low class; auto is on at the strong rung only; a word that is none of the three is auto.
bool ModelWanted(gc::GpuClass c, gc::RtTier t, const std::string& word) {
  const gc::RtTier rung = ModelRung(c, t);
  if (rung == gc::RtTier::kNone) return false;  // no ray tracing, or the Low class (the rung of it is none)
  if (word == "off") return false;
  if (word == "on") return true;
  return rung == gc::RtTier::kStrong;
}

void CellChecks() {
  Check(gc::RtCvarName(gc::RtEffect::kCarMirrorRt) == "mcla_gpu_car_mirror_rt", "the table's cvar is mcla_gpu_car_mirror_rt");
  Check(gc::RtCell(gc::RtEffect::kCarMirrorRt, gc::RtTier::kNone) == "off" && gc::RtCell(gc::RtEffect::kCarMirrorRt, gc::RtTier::kWeak) == "off" &&
            gc::RtCell(gc::RtEffect::kCarMirrorRt, gc::RtTier::kStrong) == "on",
        "AUTO is off without ray tracing, off on a weak card and on a strong one");
  int pairs = 0;
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      Check(gc::RtRung(c, t) == ModelRung(c, t), std::string("the rung of ") + Name(c, t, "") + " is the model's");
      for (const char* word : kWords) {
        ++pairs;
        const cs::Decision d = cs::Decide(c, t, word);
        Check(d.wanted == ModelWanted(c, t, word), Name(c, t, word) + ": the word comes to " + (ModelWanted(c, t, word) ? "on" : "off"));
        // the renderer's own answer: the pass's Wanted, with the tier's class (CurrentRtClass), for a device that can trace
        const bool host = cm::Wanted(cm::SettingOf(word), true, gc::RtClass(c, t));
        Check(host == d.wanted, Name(c, t, word) + ": the row's answer is the renderer's (car_mirror_rt_rules.h Wanted with the tier's class)");
        Check(!cm::Wanted(cm::SettingOf(word), false, gc::RtClass(c, t)), Name(c, t, word) + ": a device that cannot trace the car wants nothing");
        // a grey row's setting does nothing
        Check(cs::RowEnabled(d.gate) || !d.wanted, Name(c, t, word) + ": a grey row never runs the pass");
      }
    }
  }
  Check(pairs == 3 * 3 * int(sizeof(kWords) / sizeof(kWords[0])), "every class, tier and word was looked at");
  // the table's `auto` is the same as the model's for every pair, through ResolveRtFor
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      const std::string cell = gc::ResolveRtFor(c, t, gc::RtEffect::kCarMirrorRt, "auto");
      Check((cell == "on") == ModelWanted(c, t, "auto"), Name(c, t, "auto") + ": ResolveRtFor says " + cell);
      Check(gc::ResolveRtFor(c, t, gc::RtEffect::kCarMirrorRt, "on") == "on" && gc::ResolveRtFor(c, t, gc::RtEffect::kCarMirrorRt, "off") == "off", Name(c, t, "") + ": the player's own words are never overridden by the table");
    }
  }
  // the words
  Check(cs::ChoiceOf("auto") == cs::Choice::kAuto && cs::ChoiceOf("off") == cs::Choice::kOff && cs::ChoiceOf("on") == cs::Choice::kOn && cs::ChoiceOf("") == cs::Choice::kAuto &&
            cs::ChoiceOf("high") == cs::Choice::kAuto && cs::ChoiceOf("low") == cs::Choice::kAuto && cs::ChoiceOf("ON") == cs::Choice::kAuto,
        "the three words; anything else (an older larecomp.toml) is auto");
  for (const char* word : kWords) {
    Check(int(cs::ChoiceOf(word)) == int(cm::SettingOf(word)), std::string("the row's reading of '") + word + "' is the pass's");
  }
}

// ---------------------------------------------------------------------------
// the gate, the words and the page width
// ---------------------------------------------------------------------------

void RowChecks() {
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      const cs::Gate g = cs::GateFor(c, t);
      const cs::Gate want = t == gc::RtTier::kNone ? cs::Gate::kNoRt : (c == gc::GpuClass::kLow ? cs::Gate::kTooSlow : cs::Gate::kRuns);
      Check(g == want, Name(c, t, "") + ": the gate is no ray tracing first, then the Low class");
      Check(cs::Decide(c, t, "on").gate == g, Name(c, t, "") + ": Decide carries the gate");
    }
  }
  Check(cs::RowEnabled(cs::Gate::kRuns) && !cs::RowEnabled(cs::Gate::kNoRt) && !cs::RowEnabled(cs::Gate::kTooSlow), "only the open gate is not grey");
  Check(std::string(cs::RowLabel(cs::Gate::kRuns)) == "RT SIDE MIRRORS" && std::string(cs::RowSuffix(cs::Gate::kRuns)).empty(), "on: the whole name, no suffix");
  Check(std::string(cs::RowLabel(cs::Gate::kNoRt)) == "RT MIRRORS" && std::string(cs::RowSuffix(cs::Gate::kNoRt)) == " (NO RT GPU)", "no ray tracing: the short name and ' (NO RT GPU)'");
  Check(std::string(cs::RowLabel(cs::Gate::kTooSlow)) == "RT MIRRORS" && std::string(cs::RowSuffix(cs::Gate::kTooSlow)) == " (TOO SLOW)", "the Low class: the short name and ' (TOO SLOW)'");
  Check(std::string(cs::kFullLabel) + ": " == "RT SIDE MIRRORS: ", "the row's prefix is the header's whole label");
  // the page: the longest label of any state with the widest value; the REFLECTIONS page's own widest label is 23
  size_t widest = 0;
  for (const cs::Gate g : {cs::Gate::kRuns, cs::Gate::kNoRt, cs::Gate::kTooSlow}) {
    const std::string label = std::string(cs::RowLabel(g)) + cs::RowSuffix(g);
    widest = widest > label.size() ? widest : label.size();
    Check(label.size() + cs::kWidestValue <= cs::kPageChars, "the state '" + label + "' fits the page");
    Check(label.size() <= cs::kWidestOtherLabel, "the state '" + label + "' is no wider than the page's own widest label");
  }
  Check(widest == 22, "the widest label of the row is 'RT MIRRORS (NO RT GPU)', 22");
  Check(std::string("RT SIDE MIRRORS (NO RT GPU)").size() + cs::kWidestValue > cs::kPageChars, "the whole name with a reason would not fit (why the label is short while grey)");
  Check(cs::kWidestValue == std::string("AUTO (RAY TRACED)").size() && cs::kWidestOtherLabel == std::string("REFLECTIONS ON SURFACES").size() && cs::kPageChars == 41, "the page's numbers are CAR SELF-REFLECTIONS', REFLECTIONS ON SURFACES' and the movie's");
  // AUTO's label and the launcher's line
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      Check(cs::AutoLabel(c, t) == (ModelWanted(c, t, "auto") ? "AUTO (ON)" : "AUTO (OFF)"), Name(c, t, "auto") + ": AUTO's label says what it comes to");
      Check(cs::AutoLabel(c, t).size() <= cs::kWidestValue, Name(c, t, "auto") + ": AUTO's label fits the page's value width");
    }
  }
  Check(cs::LauncherAutoLine() == "Auto is Off without ray tracing, Off on a weak ray tracing card and On on a strong one.", "the launcher's line: " + cs::LauncherAutoLine());
  Check(cs::LauncherAutoLine() == gc::RtAutoDescription(gc::RtEffect::kCarMirrorRt), "... and it is rt_tier.h's RtAutoDescription");
  Check(cs::GateName(cs::Gate::kRuns) == "on" && cs::GateName(cs::Gate::kNoRt) == "no ray tracing graphics card" && cs::GateName(cs::Gate::kTooSlow) == "graphics card too slow", "the gate's names (for the log)");
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
size_t At(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t x = At(text, a), y = At(text, b);
  return x != std::string::npos && y != std::string::npos && x < y;
}

// The text of one definition in a settings header: from the name's opening up to the end of its statement.
std::string DefinitionOf(const std::string& header, const std::string& open) {
  const size_t at = At(header, open);
  if (at == std::string::npos) return "";
  const size_t end = header.find("));", at);
  return end == std::string::npos ? "" : header.substr(at, end + 3 - at);
}

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const size_t items = At(menu, "const ItemDef kReflectionItems[] = {");
  const size_t items_end = items == std::string::npos ? std::string::npos : At(menu, "\n};", items);
  const std::string page = items == std::string::npos || items_end == std::string::npos ? "" : menu.substr(items, items_end - items);
  Check(Has(page, "NativeOnly(Str(\"PM_RxGpuRtMirror\", \"mcla_gpu_car_mirror_rt\",") && Has(page, "\"RT SIDE MIRRORS: \", kNativeMirrorRtVals, kNativeMirrorRtNames, 3)),"),
        "the row is a native-only cycle of mcla_gpu_car_mirror_rt with three values, on the REFLECTIONS page");
  Check(Before(page, "\"PM_RxGpuRtCityRefl\"", "\"PM_RxGpuRtMirror\"") && Before(page, "\"PM_RxGpuRtMirror\"", "\"PM_RxGpuSsr\""), "the row is after RT CITY REFLECTIONS and before REFLECTIONS ON SURFACES");
  Check(Has(menu, "kNativeMirrorRtVals[]  = {\"auto\", \"off\", \"on\"};") && Has(menu, "kNativeMirrorRtNames[] = {\"AUTO\", \"OFF\", \"ON\"};"), "the row's words are AUTO / OFF / ON (one level: On; nothing is called low)");
  // The row's greying, short label and reasons are data now (src/settings/ui_rules.toml, the shared rules; ui_parity_test.cpp holds them against car_mirror_setting.h for every class and tier).
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const size_t begin = At(rules, "[rows.PM_RxGpuRtMirror]");
  const size_t next = At(rules, "[rows.PM_RxGpuCarSelfRefl]", begin);
  const std::string row = begin == std::string::npos ? "" : rules.substr(begin, next - begin);
  Check(Has(row, "gpu.rt_tier == 'none'") && Has(row, "\" (NO RT GPU)\"") && Has(row, "d.rt_field_class == 'low'") && Has(row, "\" (TOO SLOW)\"") && Has(row, "label = \"RT MIRRORS\"") &&
            Has(row, "auto_label = true") && Has(row, "\"AUTO (OFF)\""),
        "the row's grey states, reasons, short label and AUTO label are the gate's (car_mirror_setting.h GateFor: the ray tracing tier, then the Low class)");
  Check(std::string(cs::kFullLabel) + ": " == "RT SIDE MIRRORS: " && Has(menu, "\"RT SIDE MIRRORS: \""), "the row's prefix is the header's whole label");
  // the mirror asks for its structures itself: the row has nothing to do with the car row, the shadows or the occlusion
  Check(!row.empty() && !Has(row, "car_rt_runs") && !Has(row, "shadows") && !Has(row, "ao_on"), "RT SIDE MIRRORS has nothing to do with the car row, the shadows or the occlusion");
  Check(Has(menu, "\"Road, car, city, mirrors and surfaces.\"") && std::strlen("Road, car, city, mirrors and surfaces.") <= 40, "the tab's line names the mirrors (at most 40 characters, the longest of the other tabs' lines)");
  Check(Has(menu, "RT SIDE MIRRORS, REFLECTIONS ON SURFACES (kReflectionItems)") && Has(menu, "REFLECTIONS (kReflectionItems, 7 rows)"), "the page's comments name the row (the seventh is SIDE MIRRORS, before this one)");
}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  // (Written in pieces: tools/check_settings_exposure.py takes a literal definition macro anywhere in src for a setting definition.)
  const std::string define = std::string("REX_SET") + "TING(";
  const std::string def = DefinitionOf(header, define + "std::string, mcla_gpu_car_mirror_rt, \"auto\"");
  Check(!def.empty() && Has(def, "rex::Allowed(\"auto\", \"off\", \"on\")"), "mcla_gpu_car_mirror_rt is a string setting, auto by default, with the three words");
  Check(!def.empty() && !Has(def, "kDebugOnly") && !Has(def, "kRestart") && !Has(def, "kRequiresRestart") && Has(def, "Live."), "it is not debug-only (it has its row) and it is live (no restart trait)");
  Check(!def.empty() && Has(def, "RT SIDE MIRRORS") && Has(def, "Vulkan with ray queries"), "its help names the row and both graphics APIs");
  Check(Has(header, "inline auto& mcla_gpu_car_mirror_rt = g_gpu.mcla_gpu_car_mirror_rt;") && Has(meta, "{\"mcla_gpu_car_mirror_rt\", \"MCLA/GPU\"},"), "the alias and the meta list have it");
  const size_t mat = At(manifest, "\"mcla_gpu_car_mirror_rt\": {");
  const size_t mend = mat == std::string::npos ? 0 : manifest.find("\"comments\"", mat);
  const std::string entry = mat == std::string::npos ? "" : manifest.substr(mat, mend - mat);
  Check(!entry.empty() && Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"auto\\\"\"") && Has(entry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"on\\\")") && !Has(entry, "kDebugOnly") &&
            Has(entry, "RT SIDE MIRRORS") && Has(entry, "Vulkan with ray queries"),
        "the manifest entry is the same setting: live, not debug-only, with the row named in its help");
  // its two tuning settings stay debug-only (no player row)
  for (const char* name : {"mcla_gpu_car_mirror_rt_pixels", "mcla_gpu_car_mirror_rt_strength"}) {
    const std::string d = DefinitionOf(header, std::string(", ") + name + ", ");
    Check(!d.empty() && Has(d, "kDebugOnly"), std::string(name) + " stays debug-only");
  }
  // the exposure list no longer holds it: with a row it is classified by the row
  const std::string toml = ReadFile(dir + "/../../tools/settings_exposure.toml");
  Check(!toml.empty() && !Has(toml, "mcla_gpu_car_mirror_rt\"]") && !Has(toml, "ray traced side mirrors (feat/mirror-rt"), "tools/settings_exposure.toml has no exception for it any more (the row classifies it)");
}

void HostChecks(const std::string& dir) {
  const std::string cpp = ReadFile(dir + "/car_mirror_rt.cpp");
  Check(Has(cpp, "cm::Wanted(setting, true, gpu_class::CurrentRtClass())") && Has(cpp, "cm::LimitsFor(gpu_class::CurrentRtClass(), int("), "the host reads the setting through the pass's rule, with the ray tracing tier's class");
  const std::string tier = ReadFile(dir + "/rt_tier.h");
  Check(Has(tier, "kCarMirrorRt,") && Has(tier, "inline constexpr int kRtEffectCount = 6;") && Has(tier, "{\"mcla_gpu_car_mirror_rt\", \"off\", \"off\", \"on\"},"), "rt_tier.h has the effect, with its cells");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "add_executable(larecomp_car_mirror_setting_test EXCLUDE_FROM_ALL car_mirror_setting_test.cpp)") && Has(cmake, "target_include_directories(larecomp_car_mirror_setting_test PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/..\")"),
        "the test has its CMake target");
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
  MenuChecks(dir);
  SettingsChecks(dir);
  HostChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
