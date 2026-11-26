// Check of the denoiser setting and its pause row (denoiser_setting.h, ray_regen_rules.h, dlss_rr_rules.h, upscaling_menu.cpp, frame_generation_menu.cpp, pause_menu.cpp, temporal.cpp, temporal_dlss.cpp): package RR-5 of perf/DLSS-RR.md,
// and the one on / off row of the backlog item DENOISER-TOGGLE (DLSS RAY RECONSTRUCTION on an RTX card, AMD FSR RAY REGENERATION on a Radeon RX 9000, folded into DISPLAY). A program of its own: it needs no renderer, no game
// files and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_denoiser_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/denoiser_setting_test.cpp
//   run: larecomp_denoiser_setting_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check)
//
// It checks: the words (the effective setting's, the debug chooser's, the row's labels, the launcher's) against NVIDIA's guidelines (the feature's name, the tooltip word for word, nothing called low); which setting the plan
// reads (the row ON / OFF, or the chooser when it says auto, rr or ours); which PC has which row; the gate that greys the row, over the WHOLE table of what the menu knows (against an oracle written out here, for the shipped
// consumer table and for a made-up one with cells flipped); what the row says in each state; the page: DISPLAY counts its widest label plus its widest value over every row in every state and it stays within 41 characters (the
// whole names with their reasons would not); then the TEXT of the wiring: the row in upscaling_menu.cpp, the page and the hub in pause_menu.cpp, the plan and the snapshot in temporal.cpp, the plugin request in
// temporal_dlss.cpp, the settings files, the exposure check's lists, the CMake lines.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "denoiser_setting.h"
#include "ray_regen_rules.h"

namespace dn = mcla::gpu::denoiser;
namespace rg = mcla::gpu::rayregen;
namespace rr = mcla::gpu::rr;

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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t x = At(text, a);
  const size_t y = At(text, b);
  return x != std::string::npos && y != std::string::npos && x < y;
}
bool HasWord(const std::string& text, const std::string& word) {
  const std::string l = [&] { std::string t = text; for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c))); return t; }();
  for (size_t at = l.find(word); at != std::string::npos; at = l.find(word, at + 1)) {
    const bool left = at == 0 || !std::isalpha(static_cast<unsigned char>(l[at - 1]));
    const bool right = at + word.size() >= l.size() || !std::isalpha(static_cast<unsigned char>(l[at + word.size()]));
    if (left && right) return true;
  }
  return false;
}
std::string Upper(std::string text) {
  for (char& c : text) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  return text;
}
std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return text;
}

constexpr dn::Gate kGates[] = {dn::Gate::kRuns,      dn::Gate::kStarting,   dn::Gate::kApplying, dn::Gate::kRestart,   dn::Gate::kHidden,
                               dn::Gate::kNotAvailable, dn::Gate::kNeedsDlss, dn::Gate::kSetup,    dn::Gate::kFailed, dn::Gate::kNotNeeded};
static_assert(sizeof(kGates) / sizeof(kGates[0]) == size_t(dn::kGateCount), "every gate is listed");

// ---------------------------------------------------------------------------
// the words and NVIDIA's rules
// ---------------------------------------------------------------------------

void WordChecks() {
  // the effective setting's three words, in the rules' order: auto, rr, ours (the row ON is rr, OFF is ours; auto is the debug chooser's)
  Check(std::string(dn::kWords[0]) == "auto" && std::string(dn::kWords[1]) == "rr" && std::string(dn::kWords[2]) == "ours", "the effective setting's words are auto, rr, ours");
  Check(std::string(dn::kChooserWords[0]) == "row" && std::string(dn::kChooserWords[1]) == "auto" && std::string(dn::kChooserWords[2]) == "rr" && std::string(dn::kChooserWords[3]) == "ours" &&
            std::string(dn::kChooserRow) == "row",
        "the debug chooser's words are row (the default: the row decides), auto, rr, ours");
  for (int i = 0; i < 3; ++i) {
    rr::Denoiser d = rr::Denoiser::kOurs;
    Check(rr::ParseDenoiser(dn::kWords[i], &d) && dn::IndexOf(d) == size_t(i) && std::string(dn::WordOf(d)) == dn::kWords[i], std::string("word ") + dn::kWords[i] + " round trips through the rules' parser");
    // the chooser's auto, rr and ours override the row, whichever way the row is
    Check(dn::IsOverride(dn::kWords[i]) && dn::Effective(dn::kWords[i], false) == d && dn::Effective(dn::kWords[i], true) == d, std::string("the chooser's ") + dn::kWords[i] + " overrides the row");
  }
  // the row decides when the chooser says row, or anything that is none of the three (an older larecomp.toml, a typo)
  for (const char* word : {"row", "", "ROW", "AUTO", "dlss", "nrd", "low", "on", "true"}) {
    Check(!dn::IsOverride(word), std::string("'") + word + "' is no override: the row decides");
    Check(dn::Effective(word, true) == rr::Denoiser::kRr && dn::Effective(word, false) == rr::Denoiser::kOurs, std::string("the row ON is rr and OFF is ours when the chooser says '") + word + "'");
  }
  Check(std::string(dn::kCvarName) == "mcla_gpu_denoiser" && std::string(dn::kVendorCvarName) == "mcla_gpu_vendor_denoiser", "the debug chooser's cvar keeps its name (the A/B chains set it) and the row has its own");
  Check(dn::LegacyVendorPreference("rr") == true, "saved DLSS RR or FSR RAY REGEN migrates to ON");
  Check(!dn::LegacyVendorPreference("auto"), "saved AUTO follows the hardware default");
  Check(dn::LegacyVendorPreference("ours") == false, "saved OURS migrates to OFF");
  Check(!dn::LegacyVendorPreference("row") && !dn::LegacyVendorPreference(""), "the default chooser and absent values do not migrate");
  Check(rr::NameLooksLikeRtx("NVIDIA GeForce RTX 5080") && rr::NameLooksLikeRtx("Quadro rTx 8000") &&
        !rr::NameLooksLikeRtx("NVIDIA GeForce GTX 1080"), "only RTX adapter names get the NVIDIA toggle");
  // which PC has which row: an RTX card the DLSS one, a Radeon RX 9000 on Windows 11 the AMD one, every other card none
  Check(dn::RowFor(false, false) == dn::VendorRow::kNone && dn::RowFor(false, true) == dn::VendorRow::kDlssRr && dn::RowFor(true, false) == dn::VendorRow::kRayRegen && dn::RowFor(true, true) == dn::VendorRow::kRayRegen,
        "an RTX card has DLSS RAY RECONSTRUCTION, a Radeon RX 9000 AMD FSR RAY REGENERATION, any other card no row");
  Check(std::string_view(dn::VendorName(dn::VendorRow::kNone)) == "none" &&
        std::string_view(dn::VendorName(dn::VendorRow::kDlssRr)) == "nvidia" &&
        std::string_view(dn::VendorName(dn::VendorRow::kRayRegen)) == "amd",
        "the menu and machine export use the same names for each vendor row");
  // the row's labels: the feature's name in capitals while the row says nothing, NVIDIA's abbreviation before a reason
  Check(std::string(dn::kMenuLabel) == "DLSS RAY RECONSTRUCTION" && std::string(dn::kMenuShortLabel) == "DLSS RR", "the row says DLSS RAY RECONSTRUCTION, DLSS RR before a reason");
  Check(Upper(dn::kLauncherLabel) == dn::kMenuLabel, "the menu's label is the launcher's name in capitals");
  Check(std::string(dn::kLauncherLabel) == "DLSS Ray Reconstruction", "the launcher says NVIDIA's name in full");
  // NVIDIA: "DLSS" is always attached to the feature's name, in every place that names it
  Check(Has(dn::kMenuLabel, "DLSS") && Has(dn::kMenuShortLabel, "DLSS") && Has(dn::kLauncherLabel, "DLSS Ray Reconstruction"), "the feature's name carries DLSS (menu and launcher)");
  // the menu abbreviates for the page's width; the launcher, the wiki text and the log write the name in full
  const std::string launcher_texts = std::string(dn::kLauncherDescription) + dn::kCreditLine + dn::kLauncherLabel;
  Check(!Has(launcher_texts, "DLSS RR"), "the launcher's texts write DLSS Ray Reconstruction in full, never the menu's abbreviation");
  Check(Has(dn::kLauncherDescription, "DLSS Ray Reconstruction") && Has(dn::kCreditLine, "DLSS Ray Reconstruction"), "each text names the feature");
  // NVIDIA's tooltip, word for word (RTX UI Developer Guidelines, "UI Tooltip or Setting Description")
  Check(std::string(dn::kNvidiaTooltip) ==
            "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card.",
        "NVIDIA's tooltip, word for word");
  Check(Has(dn::kLauncherDescription, "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content") &&
            Has(dn::kLauncherDescription, "GeForce RTX graphics card") && Has(dn::kLauncherDescription, "needs DLSS as the upscaler"),
        "the launcher's description uses NVIDIA's words and names the two requirements (a GeForce RTX card, DLSS Super Resolution as the upscaler)");
  Check(Has(dn::kCreditLine, "NVIDIA") && Has(dn::kCreditLine, "trademarks"), "the credit line carries NVIDIA's trademark notice");
  // the owner's rule: no graphics setting has a level called low
  for (const char* text : {dn::kMenuLabel, dn::kMenuShortLabel, dn::kLauncherLabel, dn::kLauncherDescription}) {
    Check(!HasWord(text, "low"), std::string("nothing is called low: ") + text);
  }
  // no sentence that the cleanup is "better" than the game's: the launcher says what it is, the wiki's text has the measured numbers (none yet)
  Check(!Has(Lower(dn::kLauncherDescription), "better than"), "the description claims no measured advantage");
  Check(Has(dn::kLauncherDescription, "Off uses the game's own cleanup."), "the description says what OFF is");
}

// ---------------------------------------------------------------------------
// the gate: the whole table against an oracle
// ---------------------------------------------------------------------------

// Written out from the rules of the header's comment, not computed from the code under test. The PC's own reasons come in the order the plan's rules ask them (build, files, started, plugin asked, supported,
// not failed), which the oracle spells.
dn::Gate Oracle(const dn::MenuInputs& m, const rr::ConsumerTable& table) {
  using dn::Gate;
  if (!m.dlss_listed) return Gate::kHidden;
  if (m.dlss_unavailable || m.debug == rr::DebugSwitch::kOff) return Gate::kNotAvailable;
  if (m.dlss_chosen && m.dlss_waits_for_restart) return m.setting == rr::Denoiser::kOurs ? Gate::kRuns : Gate::kRestart;
  if (!m.dlss_chosen) return Gate::kNeedsDlss;
  if (m.dlss_needs_setup) return Gate::kSetup;
  const bool row = m.debug == rr::DebugSwitch::kRow;
  bool built = false;
  bool any_on = false;
  for (size_t i = 0; i < rr::kConsumerCount; ++i) {
    built = built || table[i].raw_mode_built;
    any_on = any_on || (((m.consumers_on >> i) & 1u) != 0 && table[i].raw_mode_built);
  }
  if (row && !built) return Gate::kNotNeeded;
  if (!m.facts.build_has_rr) return Gate::kNotAvailable;
  if (!m.facts.files_ok) return Gate::kSetup;
  if (!m.facts.streamline_running) return Gate::kStarting;
  if (!m.facts.asked) {
    if (!m.facts.loadable) return Gate::kNotAvailable;
    return m.setting == rr::Denoiser::kOurs ? Gate::kRuns : Gate::kRestart;
  }
  if (!m.facts.supported) return Gate::kNotAvailable;
  if (m.facts.failed) return Gate::kFailed;
  if (row && !any_on) return Gate::kNotNeeded;
  return m.context_making ? Gate::kApplying : Gate::kRuns;
}

void GateTable(const rr::ConsumerTable& table, const char* which, long long* cases, long long counts[dn::kGateCount]) {
  for (int bits = 0; bits < (1 << 5); ++bits) {
    for (int debug = 0; debug < 4; ++debug) {
      for (int setting = 0; setting < 3; ++setting) {
        for (int facts = 0; facts < (1 << 7); ++facts) {
          for (int on = 0; on < 2; ++on) {
            for (int making = 0; making < 2; ++making) {
              dn::MenuInputs m;
              m.dlss_listed = bits & 1;
              m.dlss_chosen = (bits >> 1) & 1;
              m.dlss_waits_for_restart = (bits >> 2) & 1;
              m.dlss_needs_setup = (bits >> 3) & 1;
              m.dlss_unavailable = (bits >> 4) & 1;
              m.debug = rr::DebugSwitch(debug);
              m.setting = rr::Denoiser(setting);
              m.facts.build_has_rr = facts & 1;
              m.facts.files_ok = (facts >> 1) & 1;
              m.facts.streamline_running = (facts >> 2) & 1;
              m.facts.asked = (facts >> 3) & 1;
              m.facts.loadable = (facts >> 4) & 1;
              m.facts.supported = (facts >> 5) & 1;
              m.facts.failed = (facts >> 6) & 1;
              m.consumers_on = on ? rr::kAllConsumers : 0u;
              m.context_making = making;
              const dn::Gate g = dn::GateOf(m, table);
              const dn::Gate want = Oracle(m, table);
              ++*cases;
              ++counts[int(g)];
              if (g != want) {
                Check(false, std::string(which) + ": gate " + std::to_string(int(g)) + " but the oracle says " + std::to_string(int(want)) + " (listed " + std::to_string(m.dlss_listed) + " chosen " +
                                 std::to_string(m.dlss_chosen) + " waits " + std::to_string(m.dlss_waits_for_restart) + " setup " + std::to_string(m.dlss_needs_setup) + " debug " + std::to_string(debug) +
                                 " setting " + std::to_string(setting) + " facts " + std::to_string(facts) + " on " + std::to_string(on) + " making " + std::to_string(making) + ")");
                return;
              }
            }
          }
        }
      }
    }
  }
}

void GateChecks() {
  const rr::ConsumerTable made = {{{"a", true, true}, {"b", true, false}, {"c", false, true}, {"d", false, false}}};
  for (int which = 0; which < 2; ++which) {
    const rr::ConsumerTable& table = which == 0 ? rr::kConsumers : made;
    const char* name = which == 0 ? "the shipped table (SSR and the cars' reflections built, none trusted to AUTO)" : "a made-up table (two effects built)";
    long long cases = 0;
    long long counts[dn::kGateCount] = {};
    GateTable(table, name, &cases, counts);
    Check(cases == 32 * 4 * 3 * 128 * 2 * 2, std::string(name) + ": the table size");
    // Every gate is reached by some input, in the shipped table too (the debug switch's states, the PC's own limits and NOT NEEDED).
    int reached = 0;
    for (int g = 0; g < dn::kGateCount; ++g) reached += counts[g] != 0;
    Check(reached == dn::kGateCount, std::string(name) + ": every gate is reached by some input");
  }
  // The headline states, by hand. A made-up PC: an RTX card, DLSS chosen and running, everything loaded and supported, an effect that has a raw mode is on.
  dn::MenuInputs m;
  m.setting = rr::Denoiser::kRr;  // the row ON (the default of a MenuInputs is OFF: ours)
  m.dlss_listed = m.dlss_chosen = true;
  m.facts = {true, true, true, true, false, true, false};  // build, files, started, supported, failed (false), asked, loadable (false)
  m.consumers_on = 1u;
  Check(dn::GateOf(m, made) == dn::Gate::kRuns, "everything there and an effect on: the row is on, nothing said");
  m.context_making = true;
  Check(dn::GateOf(m, made) == dn::Gate::kApplying, "the context is being made: (APPLYING), still on");
  m.context_making = false;
  m.consumers_on = 4u;  // the one that is not built
  Check(dn::GateOf(m, made) == dn::Gate::kNotNeeded, "only an effect with no raw mode is on: (NOT NEEDED)");
  m.consumers_on = 0;
  Check(dn::GateOf(m, made) == dn::Gate::kNotNeeded, "no effect on: (NOT NEEDED), NVIDIA's \"disable when ray tracing is not enabled\"");
  m.debug = rr::DebugSwitch::kOn;
  Check(dn::GateOf(m, made) == dn::Gate::kRuns, "the debug switch's on lifts the rule");
  m.debug = rr::DebugSwitch::kRow;
  m.consumers_on = 1u;
  m.dlss_listed = false;
  Check(dn::GateOf(m, made) == dn::Gate::kHidden, "not an NVIDIA RTX card: no row at all (the menu never lists it: RowFor), whatever else");
  m.dlss_listed = true;
  m.dlss_chosen = false;
  Check(dn::GateOf(m, made) == dn::Gate::kNeedsDlss, "FSR or no upscaler: (NEEDS DLSS) (NVIDIA: requires Super Resolution on)");
  m.dlss_chosen = true;
  m.facts.supported = false;
  Check(dn::GateOf(m, made) == dn::Gate::kNotAvailable, "Streamline says the card or driver cannot: (N/A)");
  m.facts.supported = true;
  m.facts.failed = true;
  Check(dn::GateOf(m, made) == dn::Gate::kFailed, "dropped in this session: (FAILED)");
  m.facts.failed = false;
  m.facts.files_ok = false;
  Check(dn::GateOf(m, made) == dn::Gate::kSetup, "files missing: (SETUP)");
  m.facts.files_ok = true;
  m.facts.asked = false;
  m.facts.loadable = true;
  Check(dn::GateOf(m, made) == dn::Gate::kRestart, "the plugin was left out of the start (the row said OFF) and the row is ON now: (RESTART), on");
  m.setting = rr::Denoiser::kOurs;
  Check(dn::GateOf(m, made) == dn::Gate::kRuns, "... and while the row IS OFF nothing is said (no restart is pending)");
  m.setting = rr::Denoiser::kAuto;  // the debug chooser's auto leaves OFF too
  m.facts.streamline_running = false;
  Check(dn::GateOf(m, made) == dn::Gate::kStarting, "Streamline still starting: on, nothing said yet");
  // the shipped table (RR-INT, CITY-RT R6, CITY-RT G6): all four effects have a raw mode. The row is grey (NOT NEEDED) while none is on, and on for a PC that could use it while one is.
  dn::MenuInputs s;
  s.dlss_listed = s.dlss_chosen = true;
  s.facts = {true, true, true, true, false, true, false};
  const uint32_t ssr = rr::ConsumerBit(rr::Consumer::kSsr);
  const uint32_t cars = rr::ConsumerBit(rr::Consumer::kCarRtReflections);
  const uint32_t city = rr::ConsumerBit(rr::Consumer::kCityRtReflections);
  const uint32_t gi = rr::ConsumerBit(rr::Consumer::kRtGi);
  s.consumers_on = 0;
  Check(dn::GateOf(s) == dn::Gate::kNotNeeded, "shipped table, no effect on: (NOT NEEDED), on a PC that could run it");
  for (const uint32_t on : {ssr, cars, city, gi, ssr | cars, ssr | city, city | gi, rr::kAllConsumers}) {
    s.consumers_on = on;
    Check(dn::GateOf(s) == dn::Gate::kRuns, "shipped table, SSR, the cars' reflections, the city's or the bounce light's pass on (set " + std::to_string(on) + "): the row is on, nothing said");
    Check(!rr::AnyAutoConsumerOn(on), "... and the debug chooser's auto would still be ours (set " + std::to_string(on) + "): no effect is trusted to AUTO yet");
  }
  s.consumers_on = ssr;
  s.setting = rr::Denoiser::kRr;
  Check(dn::GateOf(s) == dn::Gate::kRuns, "the row's RR value with SSR on: on");
  s.consumers_on = 0;
  Check(dn::GateOf(s) == dn::Gate::kNotNeeded, "... and with nothing on: (NOT NEEDED)");
  s.debug = rr::DebugSwitch::kOn;
  Check(dn::GateOf(s) == dn::Gate::kRuns, "... the debug switch's on lifts that");
  s.debug = rr::DebugSwitch::kOff;
  Check(dn::GateOf(s) == dn::Gate::kNotAvailable, "... and its off says (N/A)");
  s.debug = rr::DebugSwitch::kProbe;
  Check(dn::GateOf(s) == dn::Gate::kRuns, "probe leaves the row on");
  // Vulkan: DLSS chosen but run from the next start
  dn::MenuInputs v;
  v.setting = rr::Denoiser::kRr;
  v.dlss_listed = v.dlss_chosen = v.dlss_waits_for_restart = true;
  v.consumers_on = 1u;
  Check(dn::GateOf(v, made) == dn::Gate::kRestart, "DLSS waits for its restart (Vulkan) and the row is ON: (RESTART), which the one prompt covers");
  v.setting = rr::Denoiser::kOurs;
  Check(dn::GateOf(v, made) == dn::Gate::kRuns, "... nothing on OURS");
  dn::MenuInputs setup;
  setup.dlss_listed = setup.dlss_chosen = setup.dlss_needs_setup = true;
  Check(dn::GateOf(setup, made) == dn::Gate::kSetup, "DLSS's own files missing: (SETUP)");
}

void RowChecks() {
  // what each gate says after the label, whether it is on, and the launcher's line
  for (const dn::Gate g : kGates) {
    const bool on = dn::RowEnabled(g);
    const std::string suffix = dn::RowSuffix(g);
    const std::string reason = dn::LauncherReason(g);
    const bool says_nothing = g == dn::Gate::kRuns || g == dn::Gate::kStarting || g == dn::Gate::kHidden;
    Check(suffix.empty() == says_nothing, "gate " + std::to_string(int(g)) + ": the suffix is empty exactly where the row has nothing to say (a hidden row is not drawn)");
    Check(reason.empty() == says_nothing, "gate " + std::to_string(int(g)) + ": the launcher's reason likewise");
    Check(on == (g == dn::Gate::kRuns || g == dn::Gate::kStarting || g == dn::Gate::kApplying || g == dn::Gate::kRestart), "gate " + std::to_string(int(g)) + ": on exactly for runs, starting, applying and restart");
    Check(suffix.empty() || (suffix[0] == ' ' && suffix[1] == '(' && suffix.back() == ')'), "gate " + std::to_string(int(g)) + ": the suffix is ' (WORDS)'");
    Check(suffix == Upper(suffix), "gate " + std::to_string(int(g)) + ": capitals, as the menu's font");
    // the label: the whole name while the row says nothing, the short one before a reason
    Check(std::string(dn::RowLabel(g)) == (suffix.empty() ? dn::kMenuLabel : dn::kMenuShortLabel), "gate " + std::to_string(int(g)) + ": the whole name while nothing is said, DLSS RR before a reason");
    // the words a player and the restart prompt rely on
    if (g == dn::Gate::kRestart) Check(suffix.find("RESTART") != std::string::npos, "the restart prompt finds the word RESTART in the suffix");
    if (g != dn::Gate::kRestart) Check(suffix.find("RESTART") == std::string::npos, "no other state offers a restart");
  }
  Check(std::string(dn::RowSuffix(dn::Gate::kHidden)).empty() && std::string(dn::RowSuffix(dn::Gate::kNotAvailable)) == " (N/A)" && std::string(dn::RowSuffix(dn::Gate::kNeedsDlss)) == " (NEEDS DLSS)" &&
            std::string(dn::RowSuffix(dn::Gate::kSetup)) == " (SETUP)" && std::string(dn::RowSuffix(dn::Gate::kFailed)) == " (FAILED)" && std::string(dn::RowSuffix(dn::Gate::kNotNeeded)) == " (NOT NEEDED)" &&
            std::string(dn::RowSuffix(dn::Gate::kApplying)) == " (APPLYING)" && std::string(dn::RowSuffix(dn::Gate::kRestart)) == " (RESTART)",
        "the words of the suffixes: the owner's list, no 'NO RTX GPU' (such a PC has no row)");
  // a label in front of the longest reason: 13 characters (" (NEEDS DLSS)", " (NOT NEEDED)")
  size_t longest_reason = 0;
  for (const dn::Gate g : kGates) longest_reason = std::max(longest_reason, std::string(dn::RowSuffix(g)).size());
  Check(longest_reason == 13, "the longest reason is 13 characters");
  Check(std::string(dn::kMenuLabel).size() == 23 && std::string(dn::kMenuShortLabel).size() == 7, "the whole name is 23 characters, the short one 7");
}

// DISPLAY counts ONE widest label (suffix included) and ONE widest value over every row of the page and every value a row can step to (pause_menu.cpp "Page width"), at most 41 characters together. The table is the page's
// rows on the NATIVE renderer, each in every state its words can be in. The labels and reasons of the denoiser rows come from the headers themselves (this is where a longer one would show), the other rows' words are
// literals checked against their sources, so a row that grows is found here too.
void PageWidthChecks(const std::string& dir) {
  struct W {
    std::string text;
    const char* where;  // the source file (relative to src/mc_engine/menus) that holds the words, "" for a header's own
    std::string needle; // text that must be in that source
  };
  const std::string pause = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/upscaling_menu.cpp");
  const std::string fg = ReadFile(dir + "/../mc_engine/menus/frame_generation_menu.cpp");
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const std::string display = ReadFile(dir + "/../mc_engine/menus/display_menu.h");
  const auto source_of = [&](const char* where) -> const std::string& {
    const std::string w = where;
    if (w == "ui_rules.toml") return rules;
    return w == "pause_menu.cpp" ? pause : (w == "upscaling_menu.cpp" ? menu : (w == "frame_generation_menu.cpp" ? fg : display));
  };
  // every label the page can show (NATIVE renderer)
  std::vector<W> labels = {
      {"DISPLAY MODE", "pause_menu.cpp", "\"DISPLAY MODE: \""},
      {"RESOLUTION (7680X4320)", "display_menu.h", "RESOLUTION 10, 22 with its"},
      {"RESOLUTION (10240X4320)", "display_menu.h", "a display of 10240 pixels across: 23"},
      {"ASPECT RATIO", "pause_menu.cpp", "\"ASPECT RATIO: \""},
      {"HDR", "pause_menu.cpp", "\"HDR\", \"RxGpuHdrMenu\""},
      {"DISPLAY GAMMA", "pause_menu.cpp", "\"DISPLAY GAMMA: \""},
      {"VSYNC", "pause_menu.cpp", "\"VSYNC: \""},
      {"FPS LIMIT (FRAME GEN)", "ui_rules.toml", " (FRAME GEN)"},
      {"NVIDIA REFLEX", "pause_menu.cpp", "NVIDIA REFLEX: "},
      {"REFLEX (NOT WORKING)", "ui_rules.toml", " (NOT WORKING)"},
      {"REFLEX (FRAME GEN)", "ui_rules.toml", " (FRAME GEN)"},
      {"REFLEX (PAUSED)", "ui_rules.toml", " (PAUSED)"},
      {"AMD ANTI-LAG 2", "pause_menu.cpp", "AMD ANTI-LAG 2: "},
      {"ANTI-LAG 2 (NOT WORKING)", "ui_rules.toml", "ANTI-LAG 2"},
      {"ANTI-LAG 2 (FRAME GEN)", "ui_rules.toml", " (FRAME GEN)"},
      {"ANTI-LAG 2 (PAUSED)", "ui_rules.toml", " (PAUSED)"},
      {"UPSCALING (APPLYING)", "ui_rules.toml", " (APPLYING)"},
      {"UPSCALING (RESTART)", "ui_rules.toml", " (RESTART)"},
      {"UPSCALING (NO ROOM)", "ui_rules.toml", " (NO ROOM)"},
      {"RENDER SCALE", "ui_rules.toml", "RENDER SCALE"},
      {"FSR 1 MODE", "ui_rules.toml", "FSR 1 MODE"},
      {"FSR 3.1 MODE", "ui_rules.toml", "FSR 3.1 MODE"},
      {"FSR 4 MODE", "ui_rules.toml", "FSR 4 MODE"},
      {"DLSS MODE (SETUP)", "ui_rules.toml", " (SETUP)"},
      {"DLSS MODE (RESTART)", "ui_rules.toml", "DLSS MODE"},
      {"SHARPNESS (FSR ONLY)", "ui_rules.toml", " (FSR ONLY)"},
      {"FRAME GENERATION", "frame_generation_menu.cpp", "FRAME GENERATION: "},
      {"FRAME GEN (APPLYING)", "ui_rules.toml", "live.frame_generation_applying"},
      {"FRAME GEN (RESTART)", "ui_rules.toml", "features.frame_generation.dlss_after_restart"},
      {"FRAME GEN MULTIPLIER", "frame_generation_menu.cpp", "\"FRAME GEN MULTIPLIER: \""},
      {"FRAME GEN FPS LIMIT", "frame_generation_menu.cpp", "\"FRAME GEN FPS LIMIT: \""},
  };
  // the denoiser rows, from the headers: every state of the RTX row and of the Radeon row
  for (const dn::Gate g : kGates) labels.push_back({std::string(dn::RowLabel(g)) + dn::RowSuffix(g), "", ""});
  for (int g = 0; g < rg::kGateCount; ++g) labels.push_back({std::string(rg::RowLabel(rg::Gate(g))) + rg::RowSuffix(rg::Gate(g)), "", ""});
  std::vector<W> values = {
      {"BORDERLESS", "ui_rules.toml", "BORDERLESS"},
      {"FULLSCREEN", "ui_rules.toml", "FULLSCREEN"},
      {"10240X4320", "display_menu.h", "10 at five digits across"},
      {"CONSOLE", "pause_menu.cpp", "kDisplayGammaVals"},
      {"UNCAPPED", "pause_menu.cpp", "\"UNCAPPED\""},
      {"ON + BOOST", "pause_menu.cpp", "\"ON + BOOST\""},
      {"DLSS (SETUP)", "ui_rules.toml", "DLSS (SETUP)"},
      {"NATIVE AA (100%)", "ui_rules.toml", "NATIVE AA (100%)"},
      {"PERFORMANCE (50%)", "ui_rules.toml", "PERFORMANCE (50%)"},
      {"PERFORMANCE (N/A)", "ui_rules.toml", "PERFORMANCE (N/A)"},
      {"ULTRA PERF. (33%)", "ui_rules.toml", "ULTRA PERF. (33%)"},
      {"ULTRA QUAL. (77%)", "ui_rules.toml", "ULTRA QUAL. (77%)"},
      {"LIMITED (300%)", "upscaling_menu.cpp", "\"LIMITED (\""},
      {"CUSTOM (300%)", "upscaling_menu.cpp", "\"CUSTOM (\""},
      {"FSR 3.1 (NO ROOM)", "ui_rules.toml", "FSR 3.1 (NO ROOM)"},
      {"FSR 3.1 (FAILED)", "ui_rules.toml", "FSR 3.1 (FAILED)"},
      {"DLSS (FAILED)", "ui_rules.toml", "DLSS (FAILED)"},
      {"DLSS (N/A)", "ui_rules.toml", "DLSS (N/A)"},
      {"HALF DISPLAY 116", "ui_rules.toml", "HALF DISPLAY {live.frame_generation_half_display}"},
      {"1/4 DISPLAY 130", "ui_rules.toml", "1/{d.shown_per_rendered} DISPLAY"},
      {"120 (480 SHOWN)", "ui_rules.toml", "{value*d.shown_per_rendered} SHOWN"},
      {"USE FPS LIMIT", "ui_rules.toml", "USE FPS LIMIT"},
      {"ON", "", ""},
      {"OFF", "", ""},
  };
  size_t widest_label = 0, widest_value = 0;
  std::string widest_label_text, widest_value_text;
  for (const W& l : labels) {
    if (l.text.size() > widest_label) { widest_label = l.text.size(); widest_label_text = l.text; }
    if (*l.where) Check(Has(source_of(l.where), l.needle), "the label '" + l.text + "' is still in " + l.where);
  }
  for (const W& v : values) {
    if (v.text.size() > widest_value) { widest_value = v.text.size(); widest_value_text = v.text; }
    if (*v.where) Check(Has(source_of(v.where), v.needle), "the value '" + v.text + "' is still in " + v.where);
  }
  Check(widest_label + widest_value <= 41, "DISPLAY stays within 41 characters over every row in every state: " + std::to_string(widest_label) + " ('" + widest_label_text + "') + " + std::to_string(widest_value) + " ('" + widest_value_text + "')");
  Check(widest_label == 24 && std::string(rg::kMenuLabel).size() == 24 && widest_value == 17, "the widest label is 24 (AMD FSR RAY REGENERATION is one of them) and the widest value 17: 41 of 41, nothing to spare");
  // what would not fit: the whole names with their reasons, and the two old words that made the page 46 and 42
  Check(std::string(dn::kMenuLabel).size() + std::string(dn::RowSuffix(dn::Gate::kNeedsDlss)).size() + widest_value > 41, "'DLSS RAY RECONSTRUCTION (NEEDS DLSS)' (36) with the page's widest value would not fit");
  Check(std::string(rg::kMenuLabel).size() + std::string(rg::RowSuffix(rg::Gate::kNeedsD3D12)).size() + widest_value > 41, "'AMD FSR RAY REGENERATION (D3D12 ONLY)' (37) with the page's widest value would not fit");
  Check(std::string("AMD ANTI-LAG 2 (NOT WORKING)").size() + widest_value > 41, "the latency row's old 'AMD ANTI-LAG 2 (NOT WORKING)' (28) would not fit the page");
  Check(std::string(rg::kMenuLabel).size() + std::string("FSR 3.1 (APPLYING)").size() > 41, "frame generation's old 'FSR 3.1 (APPLYING)' value (18) would not fit beside the AMD row's name");
  // the denoiser rows' own words
  Check(std::string(rg::kMenuLabel) == "AMD FSR RAY REGENERATION" && std::string(rg::kMenuShortLabel) == "RAY REGEN" && Upper(rg::kLauncherName) == rg::kMenuLabel, "the Radeon row says AMD's name in capitals, RAY REGEN before a reason");
  // The pieces the page is made of (the words that are not in the headers) are in the sources: the rows' own tables
  Check(Has(rules, "label = \"REFLEX\"") && Has(rules, "label = \"ANTI-LAG 2\""),
        "the latency row says its short name before a reason");
  Check(Has(rules, "live.frame_generation_applying ?? false") && Has(rules, "label = \"FRAME GEN\"") &&
            !Has(rules, "FSR 3.1 (APPLYING)") && !Has(rules, "DLSS (APPLYING)"),
        "frame generation says APPLYING in its row label");
}

// What the plan reads. The row ON is rr, OFF is ours, and the debug chooser overrides it; the plan and the plugin request read the same word (denoiser_setting.cpp Setting).
void EffectiveChecks() {
  const rr::ConsumerTable made = {{{"a", true, true}, {"b", true, false}, {"c", false, true}, {"d", false, false}}};
  // the row OFF: nothing of RR is chosen and the plugin is not asked for; the row ON: RR wherever an effect with a raw mode is on
  for (const rr::DebugSwitch debug : {rr::DebugSwitch::kRow}) {
    rr::PluginInputs p;
    p.debug = debug;
    p.raw_mode_built = true;
    p.build_has_rr = true;
    p.files_ok = true;
    p.looks_rtx = true;
    p.d3d12 = true;
    p.setting = dn::Effective("row", false);
    Check(!rr::WantRrPlugin(p), "the row OFF (the default): the plugin is not asked for at the start");
    p.setting = dn::Effective("row", true);
    Check(rr::WantRrPlugin(p), "the row ON: the plugin is asked for at the start, on a PC that can");
    p.setting = dn::Effective("auto", false);
    Check(rr::WantRrPlugin(p), "the debug chooser's auto loads it too, whatever the row says");
    p.setting = dn::Effective("ours", true);
    Check(!rr::WantRrPlugin(p), "the debug chooser's ours leaves it out, whatever the row says");
  }
  rr::PlanInputs in;
  in.facts = {true, true, true, true, false, true, false};
  in.upscaler_is_dlss = true;
  in.guides_ready = true;
  in.room_ok = true;
  in.consumers_active = rr::AnyAutoConsumerOn(1u, made);
  in.consumers_any = rr::AnyConsumerOn(1u, made);
  in.setting = dn::Effective("row", false);
  Check(!rr::SelectDenoiser(in).use_rr, "the row OFF: our own denoisers, with an effect on");
  in.setting = dn::Effective("row", true);
  Check(rr::SelectDenoiser(in).use_rr, "the row ON: Ray Reconstruction takes the frame while an effect with a raw mode is on");
  in.consumers_any = in.consumers_active = false;
  Check(!rr::SelectDenoiser(in).use_rr, "... and not while none is");
  // nothing is trusted to the chooser's auto yet: the shipped table
  Check(!rr::AnyAutoConsumerOn(rr::kAllConsumers), "the debug chooser's auto is ours today: no effect is trusted to it");
}

// ---------------------------------------------------------------------------
// the text of the wiring
// ---------------------------------------------------------------------------

void DefaultChecks() {
  namespace rg = mcla::gpu::rayregen;
  const auto legacy_auto = dn::LegacyVendorPreference("auto");
  const auto automatic = dn::ResolvePreference("row", legacy_auto);
  Check(automatic.uses_default && automatic.setting == rr::Denoiser::kRr, "legacy AUTO requests the vendor default");
  Check(dn::ToggleOn(legacy_auto, true) && !dn::ToggleOn(legacy_auto, false), "legacy AUTO follows eligibility in both directions");
  Check(dn::ToggleOn(true, false) && !dn::ToggleOn(false, true), "explicit values win over either hardware default");
  const auto on = dn::ResolvePreference("row", true);
  const auto off = dn::ResolvePreference("row", false);
  Check(!on.uses_default && on.setting == rr::Denoiser::kRr, "saved ON is an explicit choice");
  Check(!off.uses_default && off.setting == rr::Denoiser::kOurs, "saved OFF is an explicit choice");
  const auto debug_auto = dn::ResolvePreference("auto", std::nullopt);
  Check(!debug_auto.uses_default && debug_auto.setting == rr::Denoiser::kAuto, "the live debug auto keeps its A/B policy");
  Check(dn::ResolvePreference("rr", false).setting == rr::Denoiser::kRr, "the debug chooser still overrides saved OFF");

  const uint32_t ssr = rr::ConsumerBit(rr::Consumer::kSsr);
  const uint32_t rt_ao = rr::ConsumerBit(rr::Consumer::kRtAo);
  Check(!rr::AnyRtConsumerOn(0) && !rr::AnyRtConsumerOn(ssr), "Vanilla or SSR alone does not request the automatic RR path");
  Check(rr::AnyRtConsumerOn(rt_ao) && rr::AnyRtConsumerOn(ssr | rt_ao), "an active RT effect qualifies with or without SSR");
  auto table = rr::kConsumers;
  table[size_t(rr::Consumer::kRtAo)].raw_mode_built = false;
  Check(!rr::AnyRtConsumerOn(rt_ao, table), "the RT effect must be able to feed RR");

  rr::PlanInputs rr_in;
  rr_in.setting = automatic.setting;
  rr_in.uses_default = automatic.uses_default;
  rr_in.rtx_adapter = true;
  rr_in.rt_consumer_on = true;
  rr_in.consumers_any = true;
  rr_in.upscaler_is_dlss = true;
  rr_in.guides_ready = true;
  rr_in.facts.build_has_rr = rr_in.facts.files_ok = rr_in.facts.streamline_running = true;
  rr_in.facts.asked = rr_in.facts.supported = true;
  Check(rr::SelectDenoiser(rr_in).use_rr, "RTX with DLSS, the files and a compatible RT effect defaults to RR");
  const auto refuses_rr = [&](bool rr::PlanInputs::*field, rr::Why why, const char* message) {
    auto in = rr_in;
    in.*field = false;
    const auto plan = rr::SelectDenoiser(in);
    Check(!plan.use_rr && plan.why == why, message);
  };
  refuses_rr(&rr::PlanInputs::rtx_adapter, rr::Why::kNeedsRtx, "non-RTX defaults off");
  refuses_rr(&rr::PlanInputs::upscaler_is_dlss, rr::Why::kUpscalerNotDlss, "non-DLSS upscaling defaults off");
  refuses_rr(&rr::PlanInputs::rt_consumer_on, rr::Why::kNoRtConsumer, "SSR alone defaults off");
  rr_in.facts.streamline_running = false;
  refuses_rr(&rr::PlanInputs::upscaler_is_dlss, rr::Why::kUpscalerNotDlss, "report that DLSS is off even before Streamline starts");
  rr_in.facts.streamline_running = true;
  rr_in.facts.files_ok = false;
  Check(rr::SelectDenoiser(rr_in).why == rr::Why::kFilesMissing, "missing RR files leave the default off");
  rr_in.facts.files_ok = true;
  rr_in.facts.supported = false;
  Check(rr::SelectDenoiser(rr_in).why == rr::Why::kNotSupported, "a rejected NVIDIA driver leaves the default off");
  rr_in.facts.supported = true;
  rr_in.rt_consumer_on = false;
  rr_in.setting = on.setting;
  rr_in.uses_default = on.uses_default;
  Check(rr::SelectDenoiser(rr_in).use_rr, "saved ON can still use RR with SSR alone");
  rr_in.rt_consumer_on = true;
  rr_in.setting = off.setting;
  Check(rr::SelectDenoiser(rr_in).why == rr::Why::kSettingOurs, "saved OFF keeps ours with every prerequisite met");

  rg::Facts amd;
  amd.build_has = amd.files_ok = amd.probed = amd.supported = true;
  amd.device = rg::DeviceVerdict::kOk;
  Check(rg::DefaultOn(amd, rr::DebugSwitch::kRow), "an eligible RX 9000 defaults to Ray Regeneration without an upscaler or signal prerequisite for the toggle");
  const auto refuses_amd = [&](bool rg::Facts::*field, const char* message) {
    auto facts = amd;
    facts.*field = false;
    Check(!rg::DefaultOn(facts, rr::DebugSwitch::kRow), message);
  };
  refuses_amd(&rg::Facts::build_has, "AMD default is off without the runtime build");
  refuses_amd(&rg::Facts::files_ok, "AMD default is off without its files");
  refuses_amd(&rg::Facts::probed, "AMD default is off while support is unknown");
  refuses_amd(&rg::Facts::supported, "AMD default is off when the provider refuses the card");
  amd.device = rg::DeviceVerdict::kNotD3D12;
  Check(!rg::DefaultOn(amd, rr::DebugSwitch::kRow), "AMD default is off on Vulkan");
}

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/upscaling_menu.cpp");
  const std::string pause = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const std::string fg = ReadFile(dir + "/../mc_engine/menus/frame_generation_menu.cpp");
  const std::string fg_h = ReadFile(dir + "/../mc_engine/menus/frame_generation_menu.h");
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const size_t sharp = At(menu, "Str(\"PM_RxGpuUpscSharp\",");
  const size_t den = At(menu, "NativeOnly(Bool(\"PM_RxGpuDenoiser\", \"mcla_gpu_vendor_denoiser\", \"DLSS RAY RECONSTRUCTION: \"))");
  const size_t end = At(menu, "};", den == std::string::npos ? 0 : den);
  Check(sharp != std::string::npos && den != std::string::npos && sharp < den && den < end,
        "the native-only denoiser row is the last upscaling row, after SHARPNESS");
  Check(Has(menu, "constexpr int kUpscalerRowCount = 3;") && Has(menu, "PauseRowListed(kRows[kUpscalerRowCount])"),
        "the rule controls whether the fourth row is listed");
  Check(Has(rules, "[rows.PM_RxGpuDenoiser]") && Has(rules, "d.denoiser_state") &&
        Has(rules, "live.denoiser_gate ?? d.denoiser_fallback"),
        "the row evaluates the vendor gate in the declarative states");
  Check(Has(pause, "return mcla_menu::VendorDenoiserOn() ? \"true\" : \"false\";") &&
            Has(menu, "dn::ToggleOn(saved, dn::Current().default_on)") &&
            Has(menu, "dn::ToggleOn(saved, rg::DefaultOn(rg::MenuFacts(), rg::ReadDebugSwitch()))"),
        "row display and clicks use the same vendor default and preserve explicit settings");
  Check(!Has(menu, "kDenoiserVals") && !Has(menu, "kDenoiserNames") && !Has(menu, "\"DENOISER: \""),
        "the three-way AUTO / DLSS RR / OURS row is gone");
  Check(Has(menu, "mcla::gpu::denoiser::GateOf(DenoiserInputs())") &&
        Has(menu, "rg::GateOf(rg::MenuFacts(), rg::ReadDebugSwitch(), mcla::gpu::denoiser::ConsumersOn(), rg::ContextMaking())"),
        "each vendor uses its own backend gate");
  Check(Has(menu, "gpu::denoiser::RowFor(gpu::rayregen::RowListed(), gpu::denoiser::RtxAdapter())") &&
            Has(menu, "bool AmdRow() { return DenoiserRowKind() == mcla::gpu::denoiser::VendorRow::kRayRegen; }"),
        "which row a PC has: the AMD one by the card's name and Windows, the DLSS one on RTX");
  Check(Has(menu, "#include \"gpu/denoiser_setting.h\"") && Has(menu, "#include \"gpu/ray_regen_plan.h\""), "the menu includes the rules");
  Check(Has(menu, "m.dlss_chosen = Chosen() == Upscaler::kDlss;") && Has(menu, "m.dlss_waits_for_restart = DlssWaitsForRestart();") && Has(menu, "m.dlss_listed = DenoiserRowKind()") &&
            Has(menu, "m.setting = preference.uses_default && !snapshot.default_on") && !Has(menu, "\"mcla_gpu_denoiser\"") && Has(menu, "m.facts = snapshot.facts;"),
        "the menu's inputs: DLSS as listed, chosen and waiting for a restart, the effective setting (the row and the debug chooser), the renderer's facts");
  // the page: UPSCALING is not a page any more, DISPLAY has its rows
  Check(!Has(pause, "RxUpscalingMenu") && !Has(pause, "PM_RxSubUpscaling") && !Has(pause, "PM_RxTabUpscaling"), "UPSCALING is not a page or a hub row any more");
  Check(Has(pause, "     DisplayPageRows, false, UpscalingMenuRelayout},"), "DISPLAY takes its rows from DisplayPageRows and its relayout hook is the upscaling rows'");
  Check(Before(pause, "Str(\"PM_RxDisplayMode\"", "Bool(\"PM_RxVsync\"") &&
        Before(pause, "Bool(\"PM_RxVsync\"", "Str(\"PM_RxResolution\""), "DISPLAY places VSYNC between mode and resolution");
  const size_t latency_at = At(pause, "const ItemDef* latency = LowLatencyMenuRows(latency_count);");
  const size_t upscaling_at = At(pause, "const ItemDef* upscaling = UpscalingPageRows(upscaling_count);");
  const size_t push_latency = At(pause, "for (int i = 0; i < latency_count; ++i) made->rows.push_back(latency[i]);");
  const size_t push_upscaling = At(pause, "for (int i = 0; i < upscaling_count; ++i) made->rows.push_back(upscaling[i]);");
  Check(latency_at != std::string::npos && upscaling_at != std::string::npos && latency_at < upscaling_at && push_latency != std::string::npos && push_upscaling != std::string::npos && push_latency < push_upscaling,
        "DISPLAY's extra rows are the latency row, then the upscaling block");
  Check(Has(pause, "Display has up to 17 rows.") && Has(pause, "at or under 41 characters"),
        "the page's width comment counts the page");
  Check(Has(fg_h, "const mcla_menu::ItemDef* UpscalingPageRows(int& count);") && Has(fg_h, "bool UpscalingMenuRelayout(bool apply);") && Has(fg, "bool UpscalingMenuRelayout(bool apply) {") &&
            Has(fg, "return g_built != nullptr && (live != g_built || count != g_built_count);"),
        "the page keeps the rows it was built with until it is built again (the relayout hook is real: a changed row set rebuilds the page)");
  // the exposure check finds the row's cvar in a normal file
  const std::string toml = ReadFile(dir + "/../../tools/settings_exposure.toml");
  Check(Has(toml, "normal_files = [") && Has(toml.substr(At(toml, "normal_files = ["), 260), "\"menus/upscaling_menu.cpp\""), "tools/settings_exposure.toml lists upscaling_menu.cpp among the files whose rows are for every player (so the row's cvar needs no entry)");
}

// RR-INT: the effects are consumers (the table's cells and the calls of the effects' BeginFrame, as text).
void ConsumerWiringChecks(const std::string& dir) {
  const std::string rules = ReadFile(dir + "/dlss_rr_rules.h");
  Check(Has(rules, "{\"screen-space reflections\", true, false},") && Has(rules, "{\"ray traced car reflections\", true, false},"), "dlss_rr_rules.h: SSR and the cars' reflections have a raw mode and are not trusted to AUTO yet");
  Check(Has(rules, "{\"ray traced city reflections\", true, false},") && Has(rules, "{\"ray traced bounce light\", true, false},"),
        "dlss_rr_rules.h: the city's reflections (the fallback, CITY-RT R6) and the bounce light's per-pixel pass (CITY-RT G6) have a raw mode and are not trusted to AUTO");
  const std::string ssr = ReadFile(dir + "/ssr.cpp");
  const size_t sb = At(ssr, "void ScreenReflections::BeginFrame(gbuffer::PauseFxView pause_fx) {");
  const size_t se = At(ssr, "\n}\n", sb == std::string::npos ? 0 : sb);
  const std::string sfn = (sb == std::string::npos || se == std::string::npos) ? std::string() : ssr.substr(sb, se - sb);
  Check(!sfn.empty() && Has(sfn, "denoiser::NoteConsumer(rr::Consumer::kSsr, level_ != ssr::Level::kOff);"), "ScreenReflections::BeginFrame tells the board whether SSR is on");
  Check(Before(sfn, "level_ = level;", "denoiser::NoteConsumer(rr::Consumer::kSsr"), "... after the level is settled (a setting that cannot be made reads as off)");
  Check(Has(sfn, "denoiser::NoteConsumer(rr::Consumer::kCityRtReflections, rt_.wanted());") && Before(sfn, "rt_.BeginFrame(garage ? ssr::Level::kOff : level_);", "denoiser::NoteConsumer(rr::Consumer::kCityRtReflections"),
        "ScreenReflections::BeginFrame tells the board whether the ray traced fallback is wanted (CITY-RT R6), after the fallback has decided its level");
  Check(Has(ssr, "#include \"denoiser_setting.h\""), "ssr.cpp includes the board");
  const std::string car = ReadFile(dir + "/car_selfrefl.cpp");
  const size_t cb = At(car, "void CarSelfRefl::BeginFrame(gbuffer::PauseFxView pause_fx) {");
  const size_t ce = At(car, "\n}\n", cb == std::string::npos ? 0 : cb);
  const std::string cfn = (cb == std::string::npos || ce == std::string::npos) ? std::string() : car.substr(cb, ce - cb);
  Check(!cfn.empty() && Has(cfn, "denoiser::NoteConsumer(rr::Consumer::kCarRtReflections, rt_ && rt_->active());"), "CarSelfRefl::BeginFrame tells the board whether the ray traced reflections run");
  Check(Before(cfn, "rt_->BeginFrame(mode != Mode::kOff, garage_fx_);", "denoiser::NoteConsumer(rr::Consumer::kCarRtReflections"), "... after the ray traced pass has decided whether it is wanted");
  Check(Has(car, "#include \"denoiser_setting.h\""), "car_selfrefl.cpp includes the board");
  // CITY-RT G6: the bounce light's per-pixel pass says it is wanted from the probe field's apply (gi_apply.cpp) at the frame's start, before the temporal system's boundary (frame_order.h: kBeginGiApply is before
  // kBeginTemporal, so the plan does not lag); nobody else claims to be it
  const std::string gi_apply = ReadFile(dir + "/gi_apply.cpp");
  const size_t gb = At(gi_apply, "void GiApply::BeginFrame() {");
  const size_t ge = At(gi_apply, "\n}\n", gb == std::string::npos ? 0 : gb);
  const std::string gfn = (gb == std::string::npos || ge == std::string::npos) ? std::string() : gi_apply.substr(gb, ge - gb);
  Check(!gfn.empty() && Has(gfn, "denoiser::NoteConsumer(rr::Consumer::kRtGi, pixel_wanted_);") &&
            Has(gfn, "pixel_wanted_ = enabled_ && pixel_ != nullptr && pixel_->available() && gi_pixel::ConsumerWanted(field_->fit().wanted);") &&
            Before(gfn, "pixel_wanted_ = enabled_", "denoiser::NoteConsumer(rr::Consumer::kRtGi") && Before(gfn, "enabled_ = field_ != nullptr && field_->running();", "pixel_wanted_ = enabled_"),
        "GiApply::BeginFrame tells the board whether the bounce light's per-pixel pass is wanted (the field runs, the frame's layout has the pass, the pass is made), after the field's running state is known, every frame");
  Check(Has(gi_apply, "#include \"denoiser_setting.h\""), "gi_apply.cpp includes the board");
  const std::string scene = ReadFile(dir + "/scene.cpp");
  Check(!Has(scene, "NoteConsumer(") && !Has(ssr, "Consumer::kRtGi") && !Has(car, "Consumer::kRtGi") && !Has(car, "Consumer::kCityRtReflections") && !Has(gi_apply, "Consumer::kSsr") && !Has(gi_apply, "Consumer::kCarRtReflections") &&
            !Has(gi_apply, "Consumer::kCityRtReflections") && !Has(ReadFile(dir + "/gi_pixel.cpp"), "NoteConsumer("),
        "no one else calls the board (the pass's host does not either: its apply does)");
  const std::string temporal = ReadFile(dir + "/temporal.cpp");
  Check(!Has(temporal, "in.consumers_active = false;") && !Has(temporal, "TODO(RR-5") && !Has(ssr, "TODO(RR-5") && !Has(car, "TODO(RR-5") && !Has(rules, "TODO(RR-5"), "the merge's hooks are done: no placeholder is left");
}

void PlanChecks(const std::string& dir) {
  const std::string t = ReadFile(dir + "/temporal.cpp");
  const size_t at = At(t, "void TemporalSystem::UpdateDenoiserPlan() {");
  const size_t end = At(t, "// Ray Reconstruction's second context, the room for it and the dispatch (RR-3)", at == std::string::npos ? 0 : at);
  const std::string fn = (at == std::string::npos || end == std::string::npos) ? std::string() : t.substr(at, end - at);
  Check(!fn.empty(), "UpdateDenoiserPlan is where it was");
  Check(Has(fn, "const auto preference = denoiser::ReadPreference();") && Has(fn, "in.uses_default = preference.uses_default;") &&
            Has(fn, "in.rt_consumer_on = rr::AnyRtConsumerOn(consumers_on);"), "the plan applies the RT consumer requirement only to the default");
  Check(Has(fn, "const uint32_t consumers_on = denoiser::ConsumersOn();") && Has(fn, "in.consumers_active = rr::AnyAutoConsumerOn(consumers_on);") && Has(fn, "in.consumers_any = rr::AnyConsumerOn(consumers_on);"),
        "the effects that are on are read from the board, AUTO's list and the row's");
  Check(Has(fn, "in.debug = rr::DebugSwitch::kRow;"), "an unreadable debug switch leaves the row to decide");
  Check(Has(fn, "rr_facts_.asked = facts.asked;") && Has(fn, "rr_facts_.loadable = facts.loadable;"), "the facts carry whether the plugin was asked for and whether a restart would load it");
  // the room is asked only where everything else says RR
  const size_t pre = At(fn, "const bool candidate = rr::SelectDenoiser(in).use_rr;");
  Check(Has(fn, "if (candidate) {\n    in.room_ok = RrRoomOk();") && Has(fn, "snapshot.default_on = default_on;"),
        "the menu gets the support verdict before the per-frame guide and memory checks");
  const size_t real = At(fn, "in.guides_ready = rr_guides_ready_;");
  const size_t plan = At(fn, "rr_plan_ = rr::SelectDenoiser(in);");
  Check(pre != std::string::npos && real != std::string::npos && plan != std::string::npos && pre < real && real < plan, "the room is asked only where the rules, with the room and the guides taken as met, say RR; then the real guides; then the plan");
  const size_t publish = At(fn, "denoiser::Publish(snapshot);");
  Check(publish != std::string::npos && plan != std::string::npos && plan < publish && Has(fn, "snapshot.debug = in.debug;") && Has(fn, "snapshot.facts = rr_facts_;") && Has(fn, "snapshot.plan = rr_plan_;"),
        "the plan is made, then the snapshot the menu reads is published");
  Check(Has(fn, "rr::SelectDenoiser(in).use_rr") && !Has(fn, "debug == rr::DebugSwitch::kOn && in.facts.supported"), "the old debug-only room condition is gone");
  Check(Has(t, "#include \"denoiser_setting.h\""), "temporal.cpp includes the setting");
  Check(Has(fn, "(frame_number_ & 15u) == 0 || rr::ContextWanted(rr_plan_.why)"), "the facts are read every 16 frames, and every frame while RR is wanted");
  // the census line is printed where RR is in play, not for every player
  Check(Has(t, "rr_debug_ != rr::DebugSwitch::kRow || rr::ContextWanted(rr_plan_last_.why) || rr_plan_last_.use_rr || rr_context_asked_ || w.rr_frames != 0"), "the census line says where RR is in play");
  const std::string h = ReadFile(dir + "/temporal.h");
  Check(Has(h, "rr::Facts rr_facts_;") && Has(h, "bool rr_facts_valid_ = false;") && Has(h, "rr::DebugSwitch rr_debug_ = rr::DebugSwitch::kRow;"), "temporal.h has the cached facts");
}

void PluginChecks(const std::string& dir) {
  const std::string d = ReadFile(dir + "/temporal_dlss.cpp");
  const size_t at = At(d, "bool WantRayReconstructionPlugin(Backend backend, bool looks_rtx, bool dlss_saved_at_start) {");
  const size_t end = At(d, "std::string ResultText(sl::Result result) {", at == std::string::npos ? 0 : at);
  const std::string fn = (at == std::string::npos || end == std::string::npos) ? std::string() : d.substr(at, end - at);
  Check(!fn.empty(), "WantRayReconstructionPlugin is where it was");
  Check(Has(fn, "in.setting = denoiser::Setting();") && Has(fn, "in.raw_mode_built = rr::AnyRawModeBuilt();"), "the plugin request reads the effective setting and whether any effect has a raw mode");
  const size_t files = At(fn, "in.files_ok = RrFilesOk(ExeDirectory(), &why);");
  const size_t wanted = At(fn, "if (loadable && rr::PluginWantedByChoice(in)) {");
  Check(files != std::string::npos && wanted != std::string::npos && wanted < files, "the files are looked at only where the choice wants the plugin (a row on OURS costs nothing)");
  Check(Has(fn, "what_if.setting = rr::Denoiser::kRr;") && Has(fn, "const bool loadable = rr::WantRrPlugin(what_if);") &&
        Has(fn, "g_rr_loadable.store(loadable"), "loadable is what a start with the row ON would do");
  Check(Has(fn, "return rr::WantRrPlugin(in);"), "the rules decide");
  Check(Has(d, "std::atomic<bool> g_rr_loadable{false};") && Has(d, "std::atomic<bool> g_rr_files_bad{false};"), "the two flags are atomics set before slInit");
  Check(Has(d, "facts.files_ok = !g_rr_files_bad.load(std::memory_order_relaxed);") && Has(d, "facts.loadable = !rt.rr_asked && g_rr_loadable.load(std::memory_order_relaxed);"), "the facts carry them");
  Check(Has(d, "rr::DebugSwitch value = rr::DebugSwitch::kRow;"), "an unreadable debug switch leaves the row to decide");
  Check(!Has(d, "in.debug != rr::DebugSwitch::kOff") || !Has(fn, "in.debug != rr::DebugSwitch::kOff"), "the plugin request no longer treats the debug switch as the master");
  const std::string h = ReadFile(dir + "/temporal_dlss.h");
  Check(Has(h, "bool loadable = false;") && Has(h, "bool files_ok = true;"), "temporal_dlss.h's facts have loadable, and files_ok means no known problem");
}

std::string DefinitionOf(const std::string& header, const std::string& name, const std::string& type) {
  const std::string define = std::string("REX_SET") + "TING(";
  const size_t at = At(header, define + type + ", " + name + ", ");
  if (at == std::string::npos) return std::string();
  const size_t end = header.find(");\n", at);  // a statement ends with the bracket, a semicolon and a new line; the help text may hold ");" inside
  return end == std::string::npos ? std::string() : header.substr(at, end - at + 1);
}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  // The bool requests vendor support by default; the vendor rules resolve the row and the plan.
  const std::string row = DefinitionOf(header, "mcla_gpu_vendor_denoiser", "bool");
  Check(!row.empty() && Has(row, "mcla_gpu_vendor_denoiser, true, \""), "mcla_gpu_vendor_denoiser requests the vendor denoiser by default");
  Check(!row.empty() && !Has(row, "kDebugOnly") && !Has(row, "kRequiresRestart") && Has(row, "Live.") && Has(row, "DLSS RAY RECONSTRUCTION") && Has(row, "AMD FSR RAY REGENERATION"),
        "it is a player setting (not debug-only), live (no restart trait), and its help names both rows");
  Check(Has(header, "inline auto& mcla_gpu_vendor_denoiser = g_gpu.mcla_gpu_vendor_denoiser;") && Has(meta, "{\"mcla_gpu_vendor_denoiser\", \"MCLA/GPU\"},") && Has(manifest, "\"mcla_gpu_vendor_denoiser\": {"),
        "it has its alias, its meta line and its manifest entry");
  const size_t rm0 = At(manifest, "\"mcla_gpu_vendor_denoiser\": {");
  const std::string rentry0 = rm0 == std::string::npos ? std::string() : manifest.substr(rm0, At(manifest, "\"comments\"", rm0) - rm0);
  Check(Has(rentry0, "\"type\": \"bool\"") && Has(rentry0, "\"default\": \"true\"") && !Has(rentry0, "kDebugOnly") && !Has(rentry0, "kRequiresRestart"), "the manifest entry is the same setting");
  // the old chooser: still there under its name for the A/B chains, debug-only, with the row's word first
  const std::string den = DefinitionOf(header, "mcla_gpu_denoiser", "std::string");
  Check(!den.empty() && Has(den, "mcla_gpu_denoiser, \"row\", \"") && Has(den, "rex::kDebugOnly, rex::Allowed(\"row\", \"auto\", \"rr\", \"ours\")"), "mcla_gpu_denoiser is the debug chooser: a string setting, row by default, with row, auto, rr, ours");
  Check(!den.empty() && !Has(den, "kRequiresRestart") && Has(den, "Live;") && Has(den, "mcla_gpu_vendor_denoiser") && Has(den, "DLSS RAY RECONSTRUCTION"), "it is live (no restart trait) and its help names the row it overrides");
  Check(Has(header, "inline auto& mcla_gpu_denoiser = g_gpu.mcla_gpu_denoiser;") && Has(meta, "{\"mcla_gpu_denoiser\", \"MCLA/GPU\"},") && Has(manifest, "\"mcla_gpu_denoiser\": {"), "it has its alias, its meta line and its manifest entry");
  const size_t m = At(manifest, "\"mcla_gpu_denoiser\": {");
  const std::string entry = m == std::string::npos ? std::string() : manifest.substr(m, At(manifest, "\"comments\"", m) - m);
  Check(Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"row\\\"\"") && Has(entry, "rex::Allowed(\\\"row\\\", \\\"auto\\\", \\\"rr\\\", \\\"ours\\\")") && Has(entry, "rex::kDebugOnly") && !Has(entry, "kRequiresRestart"),
        "the manifest entry is the same setting");
  Check(Has(manifest, "Debug (A/B): overrides the pause menu's DLSS RAY RECONSTRUCTION / AMD FSR RAY REGENERATION row") && Has(den, "Debug (A/B): overrides the pause menu's DLSS RAY RECONSTRUCTION / AMD FSR RAY REGENERATION row"),
        "the help reads the same in the manifest and the header");
  // the debug switch: auto by default, four words, still debug-only and restart-only
  const std::string rrdef = DefinitionOf(header, "mcla_gpu_dlss_rr", "std::string");
  Check(!rrdef.empty() && Has(rrdef, "mcla_gpu_dlss_rr, \"auto\", \"") && Has(rrdef, "rex::kDebugOnly, rex::kRequiresRestart, rex::Allowed(\"auto\", \"off\", \"probe\", \"on\")"), "mcla_gpu_dlss_rr is auto by default with four words, still debug-only and restart-only");
  const size_t rm = At(manifest, "\"mcla_gpu_dlss_rr\": {");
  const std::string rentry = rm == std::string::npos ? std::string() : manifest.substr(rm, At(manifest, "\"comments\"", rm) - rm);
  Check(Has(rentry, "\"default\": \"\\\"auto\\\"\"") && Has(rentry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"probe\\\", \\\"on\\\")") && Has(rentry, "rex::kDebugOnly") && Has(rentry, "rex::kRequiresRestart"), "the manifest entry is the same");
  Check(Before(header, std::string("REX_SET") + "TING(bool, mcla_gpu_vendor_denoiser, ", std::string("REX_SET") + "TING(std::string, mcla_gpu_denoiser, ") &&
            Before(header, std::string("REX_SET") + "TING(std::string, mcla_gpu_denoiser, ", std::string("REX_SET") + "TING(std::string, mcla_gpu_dlss_rr, "),
        "the row's setting is defined before the chooser, which is before the debug switch that overrides both");
  // the help of the switch agrees with the rules' words
  Check(Has(rrdef, "overrides the DLSS RAY RECONSTRUCTION row (mcla_gpu_vendor_denoiser, and the debug chooser mcla_gpu_denoiser)") && Has(rrdef, "auto (the default): the row decides") && Has(rrdef, "off: nothing of it exists whatever the row says") &&
            !Has(rrdef, "DENOISER row"),
        "the debug switch's help says it overrides the row and the chooser, and names no row that is gone");
  const std::string regen = DefinitionOf(header, "mcla_gpu_ray_regen", "std::string") + DefinitionOf(header, "mcla_gpu_ray_regen_ssr", "i32");
  Check(!regen.empty() && Has(regen, "the AMD FSR RAY REGENERATION row") && !Has(regen, "DENOISER row"), "the Ray Regeneration settings' help names the row by its name");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "    denoiser_setting.cpp\n"), "the library builds denoiser_setting.cpp");
  Check(Has(cmake, "add_executable(larecomp_denoiser_setting_test EXCLUDE_FROM_ALL denoiser_setting_test.cpp)"), "the test has its CMake target");
  const std::string cpp = ReadFile(dir + "/denoiser_setting.cpp");
  Check(Has(cpp, "LiveText text(\"mcla_gpu_denoiser\");") &&
            Has(cpp, "ls::mcla_gpu_denoiser.GetLayer() >= rex::SettingLayer::kEnvironment") &&
            Has(cpp, "ResolvePreference(debug_override ? Chooser().Get() : kChooserRow, PlayerPreference())") &&
            Has(cpp, "toggle.GetLayer() == rex::SettingLayer::kDefault") && Has(cpp, "g_consumers.fetch_or(bit") &&
            Has(cpp, "g_consumers.fetch_and(~bit"),
        "the effective setting is the chooser (a LiveText) and the row's bool, and the board is atomic");
  const std::string config = ReadFile(dir + "/../settings/larecomp_settings.cpp");
  Check(Has(config, "dn::LegacyVendorPreference(text)") &&
        Has(config, "put_title(mcla_gpu_vendor_denoiser.GetName(), *legacy ? \"true\" : \"false\")"),
        "the file's legacy value seeds the toggle below the file layer so an explicit new value wins");
  const std::string save = ReadFile(dir + "/../mc_engine/core/config_save.cpp");
  Check(Has(save, "const bool unchanged = name != \"mcla_gpu_vendor_denoiser\" &&"),
        "changing the toggle to OFF saves an explicit value even if the new key is absent");
  const std::string facts = ReadFile(dir + "/../machine_facts/gpu_facts.cpp");
  Check(Has(facts, "rayregen::RowListed(") &&
        Has(facts, "out.denoiser = denoiser::VendorName(denoiser::RowFor(regen, rtx))"),
        "the machine export uses row eligibility instead of the adapter vendor alone");
  const std::string h = ReadFile(dir + "/denoiser_setting.h");
  Check(Has(h, "void NoteConsumer(rr::Consumer consumer, bool on);") && Has(h, "void Publish(const Snapshot& snapshot);") && Has(h, "uint32_t ConsumersOn();") && Has(h, "rr::Denoiser Setting();"), "the header declares what the effects and the renderer call");
  Check(!Has(h, "#include <windows.h>") && !Has(h, "plume") && !Has(h, "sl.h"), "the header is pure (no Plume, no Streamline, no Windows)");
  // the device the AMD row is listed by: the name and the Windows build are noted at the start
  const std::string plan = ReadFile(dir + "/ray_regen_plan.cpp");
  const std::string plan_h = ReadFile(dir + "/ray_regen_plan.h");
  const std::string temporal = ReadFile(dir + "/temporal.cpp");
  Check(Has(plan_h, "void NoteDevice(uint32_t vendor_id, bool d3d12, std::string_view adapter_name, uint32_t windows_build);") && Has(plan_h, "bool RowListed();") && Has(plan, "g_row_listed.store(mcla::gpu::rayregen::RowListed(list), std::memory_order_relaxed);") &&
            Has(plan, "list.vendor_id = vendor_id;") && Has(plan, "list.adapter_name = adapter_name;") && Has(plan, "list.windows_build = windows_build;") &&
            Has(plan, "bool RowListed() { return g_row_listed.load(std::memory_order_relaxed); }"),
        "the planner notes the card's vendor, name and Windows build once and says whether the pause menu lists the AMD row");
  Check(Has(temporal, "rayregen::NoteDevice(uint32_t(device.description().vendor), ActiveBackend() == Backend::kD3D12, device.description().name, WindowsBuildNumber());") && Has(ReadFile(dir + "/device.h"), "uint32_t WindowsBuildNumber();") &&
            Has(ReadFile(dir + "/device.cpp"), "uint32_t WindowsBuildNumber() { return ReadWindowsBuild().build; }"),
        "the start hands the planner the adapter's name and the Windows build");
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
  WordChecks();
  GateChecks();
  RowChecks();
  PageWidthChecks(dir);
  EffectiveChecks();
  DefaultChecks();
  MenuChecks(dir);
  ConsumerWiringChecks(dir);
  PlanChecks(dir);
  PluginChecks(dir);
  SettingsChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
