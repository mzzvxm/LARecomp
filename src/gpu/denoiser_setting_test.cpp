// Check of the DENOISER setting and its pause row (denoiser_setting.h, dlss_rr_rules.h, upscaling_menu.cpp, temporal.cpp, temporal_dlss.cpp): package RR-5 of perf/DLSS-RR.md. A program of its own: it needs no renderer, no game
// files and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_denoiser_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/denoiser_setting_test.cpp
//   run: larecomp_denoiser_setting_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check)
//
// It checks: the row's words (stored, menu, launcher) against NVIDIA's guidelines (the feature's name, the tooltip word for word, OFF and AUTO, nothing called low); the gate that greys the row, over the WHOLE table of
// what the menu knows (against an oracle written out here, for the shipped consumer table, where no effect has a raw mode yet, and for a made-up one with cells flipped); what the row says in each state and that it fits the
// page (41 characters of the widest label plus the widest value); AUTO's label; then the TEXT of the wiring: the row in upscaling_menu.cpp, the plan and the snapshot in temporal.cpp, the plugin request in
// temporal_dlss.cpp, the three settings files, the exposure check's lists, the CMake lines.

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

namespace dn = mcla::gpu::denoiser;
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
std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return text;
}

constexpr dn::Gate kGates[] = {dn::Gate::kRuns,      dn::Gate::kStarting,   dn::Gate::kApplying, dn::Gate::kRestart,   dn::Gate::kNoRtx,
                               dn::Gate::kNotAvailable, dn::Gate::kNeedsDlss, dn::Gate::kSetup,    dn::Gate::kFailed, dn::Gate::kNotNeeded};
static_assert(sizeof(kGates) / sizeof(kGates[0]) == size_t(dn::kGateCount), "every gate is listed");

// ---------------------------------------------------------------------------
// the words and NVIDIA's rules
// ---------------------------------------------------------------------------

void WordChecks() {
  // the three words, in the row's order: AUTO, DLSS RR, OURS (the brief's order; NVIDIA's list is OFF then AUTO: our OURS is the OFF, our DLSS RR the explicit ON)
  Check(std::string(dn::kWords[0]) == "auto" && std::string(dn::kWords[1]) == "rr" && std::string(dn::kWords[2]) == "ours", "the stored words are auto, rr, ours");
  Check(std::string(dn::kMenuNames[0]) == "AUTO" && std::string(dn::kMenuNames[1]) == "DLSS RR" && std::string(dn::kMenuNames[2]) == "OURS", "the menu says AUTO, DLSS RR, OURS");
  Check(std::string(dn::kLauncherNames[0]) == "Auto" && std::string(dn::kLauncherNames[1]) == "DLSS Ray Reconstruction" && std::string(dn::kLauncherNames[2]) == "Ours",
        "the launcher says Auto, DLSS Ray Reconstruction (NVIDIA's name in full), Ours");
  for (int i = 0; i < 3; ++i) {
    rr::Denoiser d = rr::Denoiser::kOurs;
    Check(rr::ParseDenoiser(dn::kWords[i], &d) && dn::IndexOf(d) == size_t(i) && std::string(dn::WordOf(d)) == dn::kWords[i], std::string("word ") + dn::kWords[i] + " round trips through the rules' parser");
    Check(dn::Parse(dn::kWords[i]) == d, std::string("Parse(") + dn::kWords[i] + ")");
  }
  Check(dn::Parse("") == rr::Denoiser::kAuto && dn::Parse("AUTO") == rr::Denoiser::kAuto && dn::Parse("dlss") == rr::Denoiser::kAuto && dn::Parse("nrd") == rr::Denoiser::kAuto && dn::Parse("low") == rr::Denoiser::kAuto,
        "a word that is none of the three (an older larecomp.toml, a typo) is AUTO");
  Check(std::string(dn::kCvarName) == "mcla_gpu_denoiser" && std::string(dn::kMenuLabel) == "DENOISER", "the cvar and the row's label");
  // NVIDIA: "DLSS" is always attached to the feature's name, in every place that names it
  Check(Has(dn::kMenuNames[1], "DLSS") && Has(dn::kLauncherNames[1], "DLSS Ray Reconstruction"), "the feature's name carries DLSS (menu and launcher)");
  // the menu abbreviates for the page's width; the launcher, the wiki text and the log write the name in full
  const std::string launcher_texts = std::string(dn::kLauncherDescription) + dn::kLauncherAutoDescription + dn::kCreditLine + dn::kLauncherLabel;
  Check(!Has(launcher_texts, "DLSS RR"), "the launcher's texts write DLSS Ray Reconstruction in full, never the menu's abbreviation");
  Check(Has(dn::kLauncherDescription, "DLSS Ray Reconstruction") && Has(dn::kLauncherAutoDescription, "DLSS Ray Reconstruction") && Has(dn::kCreditLine, "DLSS Ray Reconstruction"), "each text names the feature");
  // NVIDIA's tooltip, word for word (RTX UI Developer Guidelines, "UI Tooltip or Setting Description")
  Check(std::string(dn::kNvidiaTooltip) ==
            "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card.",
        "NVIDIA's tooltip, word for word");
  Check(Has(dn::kLauncherDescription, "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content") &&
            Has(dn::kLauncherDescription, "GeForce RTX graphics card") && Has(dn::kLauncherDescription, "needs DLSS as the upscaler"),
        "the launcher's description uses NVIDIA's words and names the two requirements (a GeForce RTX card, DLSS Super Resolution as the upscaler)");
  Check(Has(dn::kCreditLine, "NVIDIA") && Has(dn::kCreditLine, "trademarks"), "the credit line carries NVIDIA's trademark notice");
  // the owner's rule: no graphics setting has a level called low
  for (const char* text : {dn::kMenuNames[0], dn::kMenuNames[1], dn::kMenuNames[2], dn::kLauncherNames[0], dn::kLauncherNames[1], dn::kLauncherNames[2], dn::kLauncherDescription, dn::kLauncherAutoDescription}) {
    Check(!HasWord(text, "low"), std::string("nothing is called low: ") + text);
  }
  // no sentence that the cleanup is "better" than the game's: the launcher says what it is, the wiki's text has the measured numbers (none yet)
  Check(!Has(Lower(dn::kLauncherDescription), "better than"), "the description claims no measured advantage");
}

// ---------------------------------------------------------------------------
// the gate: the whole table against an oracle
// ---------------------------------------------------------------------------

// Written out from the rules of the header's comment, not computed from the code under test. The PC's own reasons come in the order the plan's rules ask them (build, files, started, plugin asked, supported,
// not failed), which the oracle spells.
dn::Gate Oracle(const dn::MenuInputs& m, const rr::ConsumerTable& table) {
  using dn::Gate;
  if (!m.dlss_listed) return Gate::kNoRtx;
  if (m.debug == rr::DebugSwitch::kOff) return Gate::kNotAvailable;
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
  for (int bits = 0; bits < (1 << 4); ++bits) {
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
    const char* name = which == 0 ? "the shipped table (no raw mode)" : "a made-up table (two effects built)";
    long long cases = 0;
    long long counts[dn::kGateCount] = {};
    GateTable(table, name, &cases, counts);
    Check(cases == 16 * 4 * 3 * 128 * 2 * 2, std::string(name) + ": the table size");
    // Every gate is reached by some input, in the shipped table too (the debug switch's states, the PC's own limits and NOT NEEDED).
    int reached = 0;
    for (int g = 0; g < dn::kGateCount; ++g) reached += counts[g] != 0;
    Check(reached == dn::kGateCount, std::string(name) + ": every gate is reached by some input");
  }
  // The headline states, by hand. A made-up PC: an RTX card, DLSS chosen and running, everything loaded and supported, an effect that has a raw mode is on.
  dn::MenuInputs m;
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
  Check(dn::GateOf(m, made) == dn::Gate::kNoRtx, "not an NVIDIA RTX card: (NO RTX GPU), whatever else");
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
  Check(dn::GateOf(m, made) == dn::Gate::kRestart, "the plugin was left out of the start (the row said OURS) and the row has left OURS: (RESTART), on");
  m.setting = rr::Denoiser::kOurs;
  Check(dn::GateOf(m, made) == dn::Gate::kRuns, "... and while the row IS on OURS nothing is said (no restart is pending)");
  m.setting = rr::Denoiser::kAuto;
  m.facts.streamline_running = false;
  Check(dn::GateOf(m, made) == dn::Gate::kStarting, "Streamline still starting: on, nothing said yet");
  // the shipped table: no raw mode, so on auto the row is grey for every PC that could use it, and only the PC's own limits come before that
  dn::MenuInputs s;
  s.dlss_listed = s.dlss_chosen = true;
  s.facts = {true, true, true, true, false, true, false};
  s.consumers_on = rr::kAllConsumers;
  Check(dn::GateOf(s) == dn::Gate::kNotNeeded, "today (no effect has a raw mode): (NOT NEEDED) even with every effect on, on a PC that could run it");
  s.setting = rr::Denoiser::kRr;
  Check(dn::GateOf(s) == dn::Gate::kNotNeeded, "... and the row's RR value does not change that");
  s.debug = rr::DebugSwitch::kOn;
  Check(dn::GateOf(s) == dn::Gate::kRuns, "... the debug switch's on does");
  s.debug = rr::DebugSwitch::kOff;
  Check(dn::GateOf(s) == dn::Gate::kNotAvailable, "... and its off says (N/A)");
  s.debug = rr::DebugSwitch::kProbe;
  Check(dn::GateOf(s) == dn::Gate::kRuns, "probe leaves the row on");
  // Vulkan: DLSS chosen but run from the next start
  dn::MenuInputs v;
  v.dlss_listed = v.dlss_chosen = v.dlss_waits_for_restart = true;
  v.consumers_on = 1u;
  Check(dn::GateOf(v, made) == dn::Gate::kRestart, "DLSS waits for its restart (Vulkan) and the row is on AUTO: (RESTART), which the one prompt covers");
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
    const bool says_nothing = g == dn::Gate::kRuns || g == dn::Gate::kStarting;
    Check(suffix.empty() == says_nothing, "gate " + std::to_string(int(g)) + ": the suffix is empty exactly where the row has nothing to say");
    Check(reason.empty() == says_nothing, "gate " + std::to_string(int(g)) + ": the launcher's reason likewise");
    Check(on == (g == dn::Gate::kRuns || g == dn::Gate::kStarting || g == dn::Gate::kApplying || g == dn::Gate::kRestart), "gate " + std::to_string(int(g)) + ": on exactly for runs, starting, applying and restart");
    Check(suffix.empty() || (suffix[0] == ' ' && suffix[1] == '(' && suffix.back() == ')'), "gate " + std::to_string(int(g)) + ": the suffix is ' (WORDS)'");
    Check(suffix == [&] { std::string u = suffix; for (char& c : u) c = char(std::toupper(static_cast<unsigned char>(c))); return u; }(), "gate " + std::to_string(int(g)) + ": capitals, as the menu's font");
    // the words a player and the restart prompt rely on
    if (g == dn::Gate::kRestart) Check(suffix.find("RESTART") != std::string::npos, "the restart prompt finds the word RESTART in the suffix");
    if (g != dn::Gate::kRestart) Check(suffix.find("RESTART") == std::string::npos, "no other state offers a restart");
  }
  Check(std::string(dn::RowSuffix(dn::Gate::kNoRtx)) == " (NO RTX GPU)" && std::string(dn::RowSuffix(dn::Gate::kNotAvailable)) == " (N/A)" && std::string(dn::RowSuffix(dn::Gate::kNeedsDlss)) == " (NEEDS DLSS)" &&
            std::string(dn::RowSuffix(dn::Gate::kSetup)) == " (SETUP)" && std::string(dn::RowSuffix(dn::Gate::kFailed)) == " (FAILED)" && std::string(dn::RowSuffix(dn::Gate::kNotNeeded)) == " (NOT NEEDED)" &&
            std::string(dn::RowSuffix(dn::Gate::kApplying)) == " (APPLYING)" && std::string(dn::RowSuffix(dn::Gate::kRestart)) == " (RESTART)",
        "the words of the suffixes");
  // the page's width (pause_menu.cpp "Page width"): the widest label plus the widest value, at most 41. UPSCALING on NATIVE: its own widest label is 20 ("UPSCALING (APPLYING)", "SHARPNESS (FSR ONLY)", "FRAME
  // GEN MULTIPLIER"), its widest value 18 ("FSR 3.1 (APPLYING)": frame generation's row; the mode row's longest is 17).
  size_t widest_label = 20;
  for (const dn::Gate g : kGates) widest_label = std::max(widest_label, std::string(dn::kMenuLabel).size() + std::string(dn::RowSuffix(g)).size());
  size_t widest_value = 18;
  for (const dn::Gate g : kGates) {
    for (const uint32_t on : {0u, rr::kAllConsumers}) widest_value = std::max(widest_value, dn::AutoLabel(g, on).size());
  }
  for (const char* name : dn::kMenuNames) widest_value = std::max(widest_value, std::string(name).size());
  Check(widest_label + widest_value <= 41, "the UPSCALING page stays within 41 characters: " + std::to_string(widest_label) + " + " + std::to_string(widest_value));
  Check(std::string(dn::kMenuLabel).size() + std::string(dn::RowSuffix(dn::Gate::kNoRtx)).size() == 21, "the row's longest label is 21 (DENOISER (NO RTX GPU))");
  Check(dn::AutoLabel(dn::Gate::kRuns, 0).size() <= 14 && std::string("AUTO (DLSS RR)").size() == 14, "AUTO's longest label is 14");
}

void AutoChecks() {
  const rr::ConsumerTable made = {{{"a", true, true}, {"b", true, false}, {"c", false, true}, {"d", false, false}}};
  for (const dn::Gate g : kGates) {
    for (uint32_t on = 0; on <= rr::kAllConsumers; ++on) {
      const bool open = g == dn::Gate::kRuns || g == dn::Gate::kStarting || g == dn::Gate::kApplying;
      const bool trusted = (on & 1u) != 0;  // only effect a is built AND trusted
      Check(dn::AutoIsRr(g, on, made) == (open && trusted), "AUTO comes to RR exactly where the row is open and an effect RR is known to clean better is on (gate " + std::to_string(int(g)) + ", on " + std::to_string(on) + ")");
      Check(dn::AutoLabel(g, on, made) == (open && trusted ? "AUTO (DLSS RR)" : "AUTO (OURS)"), "AUTO's label (gate " + std::to_string(int(g)) + ", on " + std::to_string(on) + ")");
    }
  }
  Check(dn::AutoLabel(dn::Gate::kRuns, rr::kAllConsumers) == "AUTO (OURS)", "today (the shipped table): AUTO is OURS wherever, whatever is on");
  Check(dn::AutoLabel(dn::Gate::kRestart, 1u, made) == "AUTO (OURS)", "(RESTART): the plugin is not loaded, so AUTO is OURS until the restart");
}

// ---------------------------------------------------------------------------
// the text of the wiring
// ---------------------------------------------------------------------------

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/upscaling_menu.cpp");
  const std::string pause = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const size_t sharp = At(menu, "Str(\"PM_RxGpuUpscSharp\",");
  const size_t den = At(menu, "Str(\"PM_RxGpuDenoiser\", \"mcla_gpu_denoiser\", \"DENOISER: \", kDenoiserVals, kDenoiserNames, 3),");
  const size_t end = At(menu, "};", den == std::string::npos ? 0 : den);
  Check(sharp != std::string::npos && den != std::string::npos && sharp < den && den < end, "the DENOISER row is after SHARPNESS, in the upscaler's own rows (UPSCALING page, before frame generation's)");
  Check(den != std::string::npos && Has(menu.substr(den > 40 ? den - 40 : 0, 40), "NativeOnly(WithDynamics("), "the row is native only (the Xenia renderer has no upscalers)");
  Check(Has(menu, "const char* const kDenoiserVals[] = {\"auto\", \"rr\", \"ours\"};") && Has(menu, "const char* const kDenoiserNames[] = {\"AUTO\", \"DLSS RR\", \"OURS\"};"),
        "the row's words are AUTO / DLSS RR / OURS, stored auto / rr / ours");
  Check(Has(menu, "constexpr RowDynamics kDenoiserDynamics = {.str_label = DenoiserValueLabel,") && Has(menu, ".enabled = DenoiserEnabled,") && Has(menu, ".suffix = DenoiserSuffix};"),
        "the row's label, grey state and reason come from the gate");
  Check(Has(menu, "mcla::gpu::denoiser::GateOf(DenoiserInputs())") && Has(menu, "mcla::gpu::denoiser::RowEnabled(DenoiserGate())") && Has(menu, "mcla::gpu::denoiser::RowSuffix(DenoiserGate())") &&
            Has(menu, "dn::AutoLabel(DenoiserGate(), dn::ConsumersOn())"),
        "the menu asks denoiser_setting.h for the gate, the suffix and AUTO's label");
  Check(Has(menu, "#include \"gpu/denoiser_setting.h\""), "the menu includes the rules");
  Check(Has(menu, "m.dlss_chosen = Chosen() == Upscaler::kDlss;") && Has(menu, "m.dlss_waits_for_restart = DlssWaitsForRestart();") && Has(menu, "m.dlss_listed = DlssListed(a);") &&
            Has(menu, "m.setting = dn::Parse(Cvar(\"mcla_gpu_denoiser\"));") && Has(menu, "m.facts = snapshot.facts;"),
        "the menu's inputs: DLSS as listed, chosen and waiting for a restart, the saved word, the renderer's facts");
  Check(Has(pause, "UpscalingPageRows: the upscaler's rows, DENOISER and frame generation's, 7 at most") && Has(pause, "DENOISER (NATIVE only"), "the page's width comment counts the row");
  // the exposure check finds the row's cvar in a normal file
  const std::string toml = ReadFile(dir + "/../../tools/settings_exposure.toml");
  Check(Has(toml, "normal_files = [") && Has(toml.substr(At(toml, "normal_files = ["), 260), "\"menus/upscaling_menu.cpp\""), "tools/settings_exposure.toml lists upscaling_menu.cpp among the files whose rows are for every player (so the row's cvar needs no entry)");
}

void PlanChecks(const std::string& dir) {
  const std::string t = ReadFile(dir + "/temporal.cpp");
  const size_t at = At(t, "void TemporalSystem::UpdateDenoiserPlan() {");
  const size_t end = At(t, "// Ray Reconstruction's second context, the room for it and the dispatch (RR-3)", at == std::string::npos ? 0 : at);
  const std::string fn = (at == std::string::npos || end == std::string::npos) ? std::string() : t.substr(at, end - at);
  Check(!fn.empty(), "UpdateDenoiserPlan is where it was");
  Check(Has(fn, "in.setting = denoiser::Setting();"), "the plan reads the row's word (a LiveText: the menu's thread writes the cvar)");
  Check(Has(fn, "const uint32_t consumers_on = denoiser::ConsumersOn();") && Has(fn, "in.consumers_active = rr::AnyAutoConsumerOn(consumers_on);") && Has(fn, "in.consumers_any = rr::AnyConsumerOn(consumers_on);"),
        "the effects that are on are read from the board, AUTO's list and the row's");
  Check(Has(fn, "in.debug = rr::DebugSwitch::kRow;"), "an unreadable debug switch leaves the row to decide");
  Check(Has(fn, "rr_facts_.asked = facts.asked;") && Has(fn, "rr_facts_.loadable = facts.loadable;"), "the facts carry whether the plugin was asked for and whether a restart would load it");
  // the room is asked only where everything else says RR
  const size_t pre = At(fn, "in.room_ok = true;\n  in.guides_ready = true;\n  if (rr::SelectDenoiser(in).use_rr) {\n    in.room_ok = RrRoomOk();");
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
  Check(Has(fn, "in.setting = denoiser::Setting();") && Has(fn, "in.raw_mode_built = rr::AnyRawModeBuilt();"), "the plugin request reads the row and whether any effect has a raw mode");
  const size_t files = At(fn, "in.files_ok = RrFilesOk(ExeDirectory(), &why);");
  const size_t wanted = At(fn, "if (rr::PluginWantedByChoice(in)) {");
  Check(files != std::string::npos && wanted != std::string::npos && wanted < files, "the files are looked at only where the choice wants the plugin (a row on OURS costs nothing)");
  Check(Has(fn, "what_if.setting = rr::Denoiser::kAuto;") && Has(fn, "g_rr_loadable.store(rr::WantRrPlugin(what_if)"), "loadable is what a start with the row on AUTO would do");
  Check(Has(fn, "return rr::WantRrPlugin(in);"), "the rules decide");
  Check(Has(d, "std::atomic<bool> g_rr_loadable{false};") && Has(d, "std::atomic<bool> g_rr_files_bad{false};"), "the two flags are atomics set before slInit");
  Check(Has(d, "facts.files_ok = !g_rr_files_bad.load(std::memory_order_relaxed);") && Has(d, "facts.loadable = !rt.rr_asked && g_rr_loadable.load(std::memory_order_relaxed);"), "the facts carry them");
  Check(Has(d, "rr::DebugSwitch value = rr::DebugSwitch::kRow;"), "an unreadable debug switch leaves the row to decide");
  Check(!Has(d, "in.debug != rr::DebugSwitch::kOff") || !Has(fn, "in.debug != rr::DebugSwitch::kOff"), "the plugin request no longer treats the debug switch as the master");
  const std::string h = ReadFile(dir + "/temporal_dlss.h");
  Check(Has(h, "bool loadable = false;") && Has(h, "bool files_ok = true;"), "temporal_dlss.h's facts have loadable, and files_ok means no known problem");
}

std::string DefinitionOf(const std::string& header, const std::string& name) {
  const std::string define = std::string("REX_SET") + "TING(";
  const size_t at = At(header, define + "std::string, " + name + ", ");
  if (at == std::string::npos) return std::string();
  const size_t end = header.find(");\n", at);  // a statement ends with the bracket, a semicolon and a new line; the help text may hold ");" inside
  return end == std::string::npos ? std::string() : header.substr(at, end - at + 1);
}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  const std::string den = DefinitionOf(header, "mcla_gpu_denoiser");
  Check(!den.empty() && Has(den, "mcla_gpu_denoiser, \"auto\", \"") && Has(den, "rex::Allowed(\"auto\", \"rr\", \"ours\")"), "mcla_gpu_denoiser is a string setting, auto by default, with the three words");
  Check(!den.empty() && !Has(den, "kDebugOnly") && !Has(den, "kRestart") && Has(den, "Live;") && Has(den, "DENOISER row") && Has(den, "DLSS Ray Reconstruction"), "it is a player setting (not debug-only), live (no restart trait), and its help names the row and NVIDIA's feature");
  Check(Has(header, "inline auto& mcla_gpu_denoiser = g_gpu.mcla_gpu_denoiser;") && Has(meta, "{\"mcla_gpu_denoiser\", \"MCLA/GPU\"},") && Has(manifest, "\"mcla_gpu_denoiser\": {"), "it has its alias, its meta line and its manifest entry");
  const size_t m = At(manifest, "\"mcla_gpu_denoiser\": {");
  const std::string entry = m == std::string::npos ? std::string() : manifest.substr(m, At(manifest, "\"comments\"", m) - m);
  Check(Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"auto\\\"\"") && Has(entry, "rex::Allowed(\\\"auto\\\", \\\"rr\\\", \\\"ours\\\")") && !Has(entry, "kDebugOnly") && !Has(entry, "kRestart"), "the manifest entry is the same setting");
  Check(Has(manifest, "DENOISER: who cleans the grain out of the noisy effects") && Has(den, "DENOISER: who cleans the grain out of the noisy effects"), "the help reads the same in the manifest and the header");
  // the debug switch: auto by default, four words, still debug-only and restart-only
  const std::string rrdef = DefinitionOf(header, "mcla_gpu_dlss_rr");
  Check(!rrdef.empty() && Has(rrdef, "mcla_gpu_dlss_rr, \"auto\", \"") && Has(rrdef, "rex::kDebugOnly, rex::kRestart, rex::Allowed(\"auto\", \"off\", \"probe\", \"on\")"), "mcla_gpu_dlss_rr is auto by default with four words, still debug-only and restart-only");
  const size_t rm = At(manifest, "\"mcla_gpu_dlss_rr\": {");
  const std::string rentry = rm == std::string::npos ? std::string() : manifest.substr(rm, At(manifest, "\"comments\"", rm) - rm);
  Check(Has(rentry, "\"default\": \"\\\"auto\\\"\"") && Has(rentry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"probe\\\", \\\"on\\\")") && Has(rentry, "rex::kDebugOnly") && Has(rentry, "rex::kRestart"), "the manifest entry is the same");
  Check(Before(header, std::string("REX_SET") + "TING(std::string, mcla_gpu_denoiser, ", std::string("REX_SET") + "TING(std::string, mcla_gpu_dlss_rr, "), "the row's setting is defined before the debug switch that overrides it");
  // the help of the two agree with the rules' words
  Check(Has(rrdef, "overrides the DENOISER row") && Has(rrdef, "auto (the default): the row decides") && Has(rrdef, "off: nothing of it exists whatever the row says"), "the debug switch's help says it overrides the row");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "    denoiser_setting.cpp\n"), "the library builds denoiser_setting.cpp");
  Check(Has(cmake, "add_executable(larecomp_denoiser_setting_test EXCLUDE_FROM_ALL denoiser_setting_test.cpp)"), "the test has its CMake target");
  const std::string cpp = ReadFile(dir + "/denoiser_setting.cpp");
  Check(Has(cpp, "LiveText text(\"mcla_gpu_denoiser\");") && Has(cpp, "rr::Denoiser Setting() { return Parse(Text().Get()); }") && Has(cpp, "g_consumers.fetch_or(bit") && Has(cpp, "g_consumers.fetch_and(~bit"),
        "the setting is read as a LiveText and the board is atomic");
  const std::string h = ReadFile(dir + "/denoiser_setting.h");
  Check(Has(h, "void NoteConsumer(rr::Consumer consumer, bool on);") && Has(h, "void Publish(const Snapshot& snapshot);") && Has(h, "uint32_t ConsumersOn();"), "the header declares what the effects and the renderer call");
  Check(!Has(h, "#include <windows.h>") && !Has(h, "plume") && !Has(h, "sl.h"), "the header is pure (no Plume, no Streamline, no Windows)");
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
  AutoChecks();
  MenuChecks(dir);
  PlanChecks(dir);
  PluginChecks(dir);
  SettingsChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
