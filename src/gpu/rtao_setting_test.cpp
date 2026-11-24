#include "rtao_setting.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>

namespace {

namespace fs = std::filesystem;
namespace ao = mcla::gpu::rtao_setting;
using ao::Choice;
using ao::Gate;
using ao::gc::GpuClass;
using ao::gc::RtTier;
using mcla::gpu::host_plan::Level;

int checks = 0;
int failures = 0;

void Check(bool ok, std::string_view message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

constexpr GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
constexpr RtTier kTiers[] = {RtTier::kNone, RtTier::kWeak, RtTier::kStrong};
constexpr Choice kChoices[] = {Choice::kAuto, Choice::kOff, Choice::kHigh, Choice::kUltra, Choice::kRt};

void CheckDecisions() {
  constexpr Choice auto_choices[3][3] = {
      {Choice::kOff, Choice::kOff, Choice::kOff},
      {Choice::kHigh, Choice::kHigh, Choice::kHigh},
      {Choice::kUltra, Choice::kUltra, Choice::kRt},
  };
  constexpr RtTier rungs[3][3] = {
      {RtTier::kNone, RtTier::kNone, RtTier::kNone},
      {RtTier::kNone, RtTier::kWeak, RtTier::kWeak},
      {RtTier::kNone, RtTier::kWeak, RtTier::kStrong},
  };
  constexpr Level screen[] = {Level::kOff, Level::kOff, Level::kHigh, Level::kUltra, Level::kUltra};
  for (int c = 0; c < 3; ++c) {
    for (int t = 0; t < 3; ++t) {
      const auto cls = kClasses[c];
      const auto tier = kTiers[t];
      const auto cell = ao::gc::RtCell(ao::gc::RtEffect::kRtAo, rungs[c][t]);
      Check(cell == (auto_choices[c][t] == Choice::kRt ? "rt" : ""), "RT Auto cell");
      for (const bool built : {false, true}) {
        const Gate gate = t == 0 ? Gate::kNoRt : c == 0 ? Gate::kTooSlow
                                                       : built ? Gate::kRuns : Gate::kNotBuilt;
        Check(ao::GateFor(cls, tier, built) == gate, "capability gate and precedence");
        for (const Choice saved : kChoices) {
          const Choice chosen = saved == Choice::kAuto ? auto_choices[c][t] : saved;
          const auto d = ao::Decide(cls, true, tier, saved, built);
          Check(d.chosen == chosen, "resolved AO choice");
          Check(d.screen == screen[static_cast<int>(chosen)], "screen-space quality");
          Check(d.rung == rungs[c][t], "effective RT rung");
          Check(d.gate == gate, "decision carries capability gate");
          Check(d.rt == (chosen == Choice::kRt && gate == Gate::kRuns), "only available RT runs");
        }
      }
    }
  }
  for (const auto cls : kClasses) {
    for (const auto tier : kTiers) {
      const auto unknown = ao::Decide(cls, false, tier, Choice::kAuto, true);
      Check(unknown.screen == Level::kOff && !unknown.rt && unknown.rung == RtTier::kNone,
            "unknown GPU class uses integrated Auto");
    }
  }
}

void CheckLabels() {
  constexpr std::string_view words[] = {"auto", "off", "high", "ultra", "rt"};
  constexpr std::string_view names[] = {"AUTO", "OFF", "HIGH", "ULTRA", "RAY TRACED"};
  constexpr Gate gates[] = {Gate::kRuns, Gate::kNoRt, Gate::kTooSlow, Gate::kNotBuilt};
  constexpr std::string_view traced[] = {"RAY TRACED", "RT (NO RT GPU)", "RT (TOO SLOW)", "RT (NOT READY)"};
  constexpr std::size_t page_label_width = 27;
  for (int i = 0; i < 5; ++i) {
    Check(ao::kValues[i] == words[i] && ao::kNames[i] == names[i], "menu and launcher value and name order");
    Check(ao::ParseChoice(words[i]) == kChoices[i], "saved words");
    Check(ao::ParseChoice(ao::gc::Upper(words[i])) == kChoices[i], "case-insensitive saved words");
    for (int g = 0; g < 4; ++g) {
      const auto label = ao::ValueLabel(kChoices[i], gates[g]);
      Check(label == (i == 4 ? traced[g] : names[i]), "visible label and gate reason");
      Check(label.size() <= 14 && label.size() + page_label_width <= 41, "label fits graphics page");
    }
  }
  for (const std::string_view unknown : {"low", "LOW", "", "rt ", "typo", "on"}) {
    Check(ao::ParseChoice(unknown) == Choice::kAuto, "unknown and legacy choices are Auto");
  }
  constexpr std::string_view auto_labels[] = {"AUTO (OFF)", "AUTO (HIGH)", "AUTO (ULTRA)"};
  for (int c = 0; c < 3; ++c) {
    for (const auto tier : kTiers) {
      for (const bool built : {false, true}) {
        const auto label = ao::AutoLabel(ao::Decide(kClasses[c], true, tier, Choice::kAuto, built));
        const auto expected = c == 2 && tier == RtTier::kStrong && built ? "AUTO (RT)" : auto_labels[c];
        Check(label == expected && label.size() <= 14, "Auto labels show the effective level within width");
      }
    }
  }
  Check(ao::AutoDescription() ==
            "Auto is Off on integrated graphics, High on a mid-range card, Ultra on a high-end card "
            "and Ray traced on a high-end card that traces rays quickly.",
        "launcher Auto sentence follows the class and RT cells");
}

std::string Read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void CheckReaders(const fs::path& src) {
  // Literal uses outside the setting and metadata must be one of these entry points.
  std::map<std::string, int> uses = {
      {"gpu/ao.cpp", 1}, {"gpu/effect_compare.h", 1}, {"gpu/rt_tier.h", 1}, {"gpu/gpu_class.h", 1},
      {"mc_engine/display/effect_compare.cpp", 1}, {"mc_engine/menus/pause_menu.cpp", 3},
  };
  for (const auto& entry : fs::recursive_directory_iterator(src)) {
    const auto& file = entry.path();
    if (!entry.is_regular_file() || (file.extension() != ".cpp" && file.extension() != ".h") ||
        file.stem().string().ends_with("_test")) {
      continue;
    }
    const std::string text = Read(file);
    if (text.find("mcla_gpu_ao.Get(") != std::string::npos ||
        text.find("mcla_gpu_ao.GetText(") != std::string::npos) {
      Check(false, "AO setting must be read through LiveText in " + file.generic_string());
    }
    constexpr std::string_view cvar = "\"mcla_gpu_ao\"";
    for (std::size_t pos = text.find(cvar); pos != std::string::npos; pos = text.find(cvar, pos + cvar.size())) {
      const auto path = file.lexically_relative(src).generic_string();
      Check(--uses[path] >= 0, "new AO reader or metadata in " + path + " needs review");
    }
  }
  const auto contains = [&](std::string_view file, std::string_view code) {
    return Read(src / file).find(code) != std::string::npos;
  };
  Check(contains("gpu/ao.cpp", "rtao_setting::DecideFor(text)") &&
            contains("gpu/ao.cpp", "rtao_setting::DecideFor(ao_text)"),
        "startup precache and frame mode use the live resolver");
  Check(contains("gpu/host_plan.h", "rtao_setting::Decide(cls, class_known, tier,"),
        "host plan delegates to the AO resolver");
  Check(contains("mc_engine/menus/pause_menu.cpp", "rtao_setting::DecideFor(kNativeAoVals[i])") &&
            contains("mc_engine/menus/pause_menu.cpp", "rtao_setting::DecideFor(CvarGet(\"mcla_gpu_ao\"))"),
        "menu labels and AO intensity dependency use the resolver");
  Check(contains("gpu/effect_compare_rules.h", "rtao_setting::ParseChoice(saved.occlusion)") &&
            contains("gpu/effect_compare_rules.h", "rtao_setting::Decide(saved.cls,"),
        "comparison resolves the saved setting");
  Check(contains("settings/generated/settings_gpu.h", "rex::Allowed(\"auto\", \"off\", \"high\", \"ultra\", \"rt\")") &&
            contains("settings/generated/manifest.json", "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"high\\\", \\\"ultra\\\", \\\"rt\\\")"),
        "settings declaration and manifest accept the fifth value");
}

}  // namespace

int main(int argc, char** argv) {
  CheckDecisions();
  CheckLabels();
  const fs::path src = argc > 1 ? fs::path(argv[1]) : fs::path(__FILE__).parent_path().parent_path();
  CheckReaders(src);
  std::cout << "rtao_setting_test: " << checks << " checks, " << failures << " failures\n";
  return failures != 0;
}
