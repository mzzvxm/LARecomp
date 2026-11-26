// Check of the sun shafts' strength levels (shafts_level.h): the setting's words and their multipliers, what `auto`
// comes to on each GPU class, the old values low and high, a text that is no value, the ray steps and the words
// of the log. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shafts_level_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shafts_level_test
//
// The expected values are written out here again on purpose, as gpu_class_test.cpp does: the owner's mapping of
// 2026-11-13 (LIGHT 1, DEFAULT 2, STRONG 3, STRONGER 4, STRONGEST 5 times the built-in strength, OFF 0; DEFAULT
// the usual value; OFF on integrated graphics by `auto`) is the other side of the check.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "shafts_level.h"

using namespace mcla::gpu::shafts_level;
using mcla::gpu::gpu_class::GpuClass;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

void CheckNum(double got, double want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g\n", what.c_str(), got, want);
  }
}

const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
const char* const kClassNames[] = {"low", "mid", "high"};

struct LevelRow {
  Level level;
  const char* word;
  double multiplier;
  const char* described;
};
// The five levels of the setting, in the order of the pause row (after AUTO).
const LevelRow kLevels[] = {
    {Level::kOff, "off", 0.0, "off"},
    {Level::kLight, "light", 1.0, "light x1"},
    {Level::kDefault, "default", 2.0, "default x2"},
    {Level::kStrong, "strong", 3.0, "strong x3"},
    {Level::kStronger, "stronger", 4.0, "stronger x4"},
    {Level::kStrongest, "strongest", 5.0, "strongest x5"},
};

void CheckLevels() {
  Check(sizeof(kLevels) / sizeof(kLevels[0]) == static_cast<std::size_t>(kLevelCount), "the test lists every level");
  for (int i = 0; i < kLevelCount; ++i) {
    const LevelRow& row = kLevels[i];
    const std::string what = std::string("level ") + row.word;
    Check(static_cast<int>(row.level) == i, what + ": in the order of the enum");
    CheckEq(std::string(Name(row.level)), row.word, what + ": name");
    CheckNum(Multiplier(row.level), row.multiplier, what + ": multiplier");
    CheckEq(Describe(row.level), row.described, what + ": words of the log");
    Check(On(row.level) == (i != 0), what + ": on");
    Level parsed = Level::kOff;
    Check(ParseLevel(row.word, &parsed) && parsed == row.level, what + ": ParseLevel");
    // The player's own choice is the same on every class: the class is never looked at.
    for (int k = 0; k < 3; ++k) {
      const Choice c = Resolve(kClasses[k], row.word);
      const std::string at = what + " on " + kClassNames[k];
      Check(c.level == row.level && !c.automatic && !c.unknown && !c.legacy, at + ": the player's own, as it is");
      const Choice up = Resolve(kClasses[k], mcla::gpu::gpu_class::Upper(row.word));
      Check(up.level == row.level, at + ": case does not matter");
    }
  }
  // The mapping the owner decided: the old intensity multipliers.
  CheckNum(Multiplier(Level::kLight), 1.0, "LIGHT is the old intensity 1");
  CheckNum(Multiplier(Level::kDefault), 2.0, "DEFAULT is the old intensity 2");
  CheckNum(Multiplier(Level::kStrong), 3.0, "STRONG is the old intensity 3");
  CheckNum(Multiplier(Level::kStronger), 4.0, "STRONGER is the old intensity 4");
  CheckNum(Multiplier(Level::kStrongest), 5.0, "STRONGEST is intensity 5");
  CheckNum(Multiplier(Level::kOff), 0.0, "OFF is 0: the game's own picture");
  Level level = Level::kStrong;
  Check(!ParseLevel("auto", &level) && level == Level::kStrong, "ParseLevel(auto) is no level and leaves the value");
  Check(!ParseLevel("low", &level) && !ParseLevel("high", &level), "ParseLevel(low, high): the old values are not levels");
  Check(!ParseLevel("Default", &level) && !ParseLevel("", &level) && !ParseLevel("default ", &level), "ParseLevel is exact");
  CheckEq(Lower("AuTo"), "auto", "Lower");
}

void CheckAuto() {
  // Legacy Auto uses the ENHANCED preset value on every GPU.
  const Level want[] = {Level::kDefault, Level::kDefault, Level::kDefault};
  const char* const label[] = {"AUTO (DEFAULT)", "AUTO (DEFAULT)", "AUTO (DEFAULT)"};
  for (int k = 0; k < 3; ++k) {
    const std::string at = std::string("auto on ") + kClassNames[k];
    for (const char* text : {"auto", "AUTO", "Auto"}) {
      const Choice c = Resolve(kClasses[k], text);
      Check(c.level == want[k] && c.automatic && !c.unknown && !c.legacy, at + ": '" + text + "' comes to " + std::string(Name(want[k])));
    }
    CheckEq(mcla::gpu::gpu_class::AutoLabelFor(kClasses[k], mcla::gpu::gpu_class::Effect::kSunShafts), label[k],
            at + ": the pause row's label");
    // gpu_class.h's table is the same one.
    CheckEq(std::string(mcla::gpu::gpu_class::Cell(mcla::gpu::gpu_class::Effect::kSunShafts, kClasses[k])),
            std::string(Name(want[k])), at + ": the table gpu_class.h holds");
  }
}

void CheckOldValues() {
  // low and high of the older setting (32 or 64 steps, both the same strength): read as DEFAULT on every class,
  // also on Low where `auto` is off (the player chose them by hand).
  for (int k = 0; k < 3; ++k) {
    for (const char* old : {"low", "high", "Low", "HIGH"}) {
      const Choice c = Resolve(kClasses[k], old);
      const std::string at = std::string("old value '") + old + "' on " + kClassNames[k];
      Check(c.level == Level::kDefault, at + ": default");
      Check(c.legacy && !c.automatic && !c.unknown, at + ": marked as an old value, not auto, not unknown");
    }
  }
}

void CheckUnknown() {
  // A hand-edited text that is no value counts as `auto`, as it always did.
  const Level want[] = {Level::kDefault, Level::kDefault, Level::kDefault};
  for (int k = 0; k < 3; ++k) {
    for (const char* junk : {"", "ultra", "hgih", "on", "game", "auto ", " off", "medium", "stong", "1"}) {
      const Choice c = Resolve(kClasses[k], junk);
      const std::string at = std::string("'") + junk + "' on " + kClassNames[k];
      Check(c.level == want[k] && c.automatic && c.unknown && !c.legacy, at + ": taken as auto (" + std::string(Name(want[k])) + ")");
    }
  }
}

void CheckSteps() {
  CheckNum(LegacySteps(0), 0, "zero uses the iterated blur");
  CheckNum(LegacySteps(-5), 0, "negative uses the iterated blur");
  CheckNum(LegacySteps(32), 32, "old 32-tap arm");
  CheckNum(LegacySteps(64), 64, "old 64-tap arm");
  CheckNum(LegacySteps(1), kMinSteps, "clamp to smallest legacy budget");
  CheckNum(LegacySteps(4000), kMaxSteps, "clamp to largest legacy budget");
}

}  // namespace

int main() {
  CheckLevels();
  CheckAuto();
  CheckOldValues();
  CheckUnknown();
  CheckSteps();
  if (g_failed != 0) {
    std::printf("shafts_level_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("shafts_level_test: %d checks passed\n", g_checks);
  return 0;
}
