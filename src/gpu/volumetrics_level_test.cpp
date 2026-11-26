// Check of the volumetric light's setting (volumetrics_level.h, gpu_class.h kEffects): the words, what `auto` comes to on each GPU class, a text that is no
// value, a text in capitals, and the words of the log. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_volumetrics_level_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_volumetrics_level_test
//
// The expected values are written out here again on purpose (VOLUMETRICS-SURVEY.md section 4.1): Low off, Mid medium, High high, ultra a deliberate pick,
// nothing called low.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "volumetrics_level.h"

using namespace mcla::gpu::volumetrics;
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

void CheckWords() {
  CheckEq(std::string(Name(Level::kOff)), "off", "name off");
  CheckEq(std::string(Name(Level::kMedium)), "medium", "name medium");
  CheckEq(std::string(Name(Level::kHigh)), "high", "name high");
  CheckEq(std::string(Name(Level::kUltra)), "ultra", "name ultra");
  Check(!On(Level::kOff) && On(Level::kMedium) && On(Level::kHigh) && On(Level::kUltra), "off is the only level that is off");
  for (const char* word : {"off", "medium", "high", "ultra"}) {
    Level level = Level::kOff;
    Check(ParseLevel(word, &level) && std::string(Name(level)) == word, std::string("ParseLevel round trip: ") + word);
  }
  Level level = Level::kHigh;
  for (const char* word : {"auto", "low", "", "Medium", "mid", "on", "high "}) {
    Check(!ParseLevel(word, &level), std::string("ParseLevel is exact: '") + word + "' is no level");
  }
}

void CheckResolve() {
  struct Row {
    GpuClass cls;
    const char* text;
    Level level;
    bool automatic;
    bool unknown;
  };
  const Row rows[] = {
      {GpuClass::kLow, "auto", Level::kMedium, true, false},
      {GpuClass::kMid, "auto", Level::kMedium, true, false},
      {GpuClass::kHigh, "auto", Level::kMedium, true, false},
      {GpuClass::kLow, "AUTO", Level::kMedium, true, false},
      {GpuClass::kMid, "Auto", Level::kMedium, true, false},
      // a hand set value is kept on every class, ultra on the Low class too (a deliberate pick)
      {GpuClass::kLow, "ultra", Level::kUltra, false, false},
      {GpuClass::kLow, "medium", Level::kMedium, false, false},
      {GpuClass::kMid, "off", Level::kOff, false, false},
      {GpuClass::kMid, "ultra", Level::kUltra, false, false},
      {GpuClass::kHigh, "off", Level::kOff, false, false},
      {GpuClass::kHigh, "Ultra", Level::kUltra, false, false},
      {GpuClass::kHigh, "medium", Level::kMedium, false, false},
      // a text that is no value is `auto`
      {GpuClass::kMid, "low", Level::kMedium, true, true},
      {GpuClass::kHigh, "hgih", Level::kMedium, true, true},
      {GpuClass::kLow, "", Level::kMedium, true, true},
  };
  for (const Row& row : rows) {
    const Choice choice = Resolve(row.cls, row.text);
    const std::string at = std::string("Resolve(") + std::string(mcla::gpu::gpu_class::ClassName(row.cls)) + ", '" + row.text + "')";
    Check(choice.level == row.level, at + ": level " + std::string(Name(choice.level)) + ", want " + std::string(Name(row.level)));
    Check(choice.automatic == row.automatic, at + ": automatic");
    Check(choice.unknown == row.unknown, at + ": unknown");
  }
  // Auto never picks ultra, and the classes only go up.
  int last = -1;
  for (const GpuClass cls : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    const Level level = Resolve(cls, "auto").level;
    Check(level != Level::kUltra, "auto never picks ultra");
    Check(int(level) >= last, "the classes only go up");
    last = int(level);
  }
}

void CheckDescribe() {
  CheckEq(Describe(GpuClass::kHigh, Resolve(GpuClass::kHigh, "auto")), "medium (auto on the high class)", "describe auto on High");
  CheckEq(Describe(GpuClass::kLow, Resolve(GpuClass::kLow, "auto")), "medium (auto on the low class)", "describe auto on Low");
  CheckEq(Describe(GpuClass::kMid, Resolve(GpuClass::kMid, "ultra")), "ultra (set)", "describe a set value");
  CheckEq(Describe(GpuClass::kMid, Resolve(GpuClass::kMid, "bogus")), "medium (not a value of the setting: taken as auto on the mid class)",
          "describe an unknown value");
}

void CheckGrids() {
  // The survey's grids (section 3.4) and the lamp / headlight budgets of the cost table (section 4).
  struct Row {
    Level level;
    uint32_t nx, ny, nz, tx, ty, aw, ah, lamps, heads;
    bool temporal;
  };
  const Row rows[] = {
      {Level::kMedium, 96, 54, 32, 8, 4, 768, 216, 12, 2, false},
      {Level::kHigh, 128, 72, 48, 8, 6, 1024, 432, 32, 4, true},
      {Level::kUltra, 160, 90, 64, 8, 8, 1280, 720, 48, 4, true},
  };
  for (const Row& row : rows) {
    const GridDesc g = GridFor(row.level);
    const std::string at = std::string("grid of ") + std::string(Name(row.level));
    Check(g.valid(), at + " is valid");
    Check(g.nx == row.nx && g.ny == row.ny && g.nz == row.nz, at + ": froxels");
    Check(g.tiles_x == row.tx && g.tiles_y == row.ty && g.tiles_x * g.tiles_y == g.nz, at + ": tiles");
    Check(g.atlas_width() == row.aw && g.atlas_height() == row.ah, at + ": atlas");
    Check(g.lamps == row.lamps && g.headlights == row.heads, at + ": lights read");
    Check(g.lamps <= kMaxLamps && g.headlights <= kMaxHeadlights, at + ": within the arrays");
    Check(g.temporal == row.temporal, at + ": history");
    // The composite packs nx, ny, nz and tiles_x in 8 bits each.
    Check(g.nx < 256 && g.ny < 256 && g.nz < 256 && g.tiles_x < 256, at + ": fits the push packing");
  }
  Check(!GridFor(Level::kOff).valid(), "off has no grid");
  Check(GridFor(Level::kMedium).far_m == 120.0f && GridFor(Level::kHigh).far_m == 160.0f, "far planes");
  Check(GridFor(Level::kMedium).voxels() == 165888 && GridFor(Level::kHigh).voxels() == 442368 && GridFor(Level::kUltra).voxels() == 921600,
        "voxel counts of the survey's cost table");
}

}  // namespace

int main() {
  CheckWords();
  CheckResolve();
  CheckDescribe();
  CheckGrids();
  if (g_failed != 0) {
    std::printf("volumetrics_level_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("volumetrics_level_test: %d checks passed\n", g_checks);
  return 0;
}
