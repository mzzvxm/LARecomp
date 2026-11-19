// Check of setting_source.h, which the source checks of ao_halo_test, ao_paint_test and ao_size_test use to read a setting's definition in
// either form: the REXCVAR_DEFINE_<TYPE> call of rexglue 0.10 and the REX_SETTING row of 0.11. A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_setting_source_test      (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_setting_source_test
//
// The texts are shortened copies of what main's ao.cpp and src/settings/generated/settings_gpu.h hold, keeping what a reader has to get past:
// a help text with commas, brackets, a semicolon and an apostrophe, a comment between two definitions, a name that starts another's.

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "setting_source.h"

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

namespace ss = setting_source;

// The samples write the two macros as @DEF_ and @ROW(: tools/check_settings_exposure.py reads every source for definitions and would take the
// samples for settings. Expand puts the names in.
std::string Expand(std::string text) {
  const std::pair<const char*, std::string> names[] = {{"@DEF_", std::string("REXCVAR_") + "DEFINE_"}, {"@ROW(", std::string("REX_") + "SETTING("}};
  for (const auto& [token, name] : names) {
    for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + name.size())) {
      text.replace(at, std::strlen(token), name);
    }
  }
  return text;
}

const std::string kOldCpp = Expand(R"CPP(
// @DEF_* lines are in this file; this comment names the family, not a call.
@DEF_DOUBLE(
    mcla_gpu_ao_car_reach_extra, 0.9, "MCLA/GPU",
    "Not the one asked for: starts with the same words.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
@DEF_DOUBLE(
    mcla_gpu_ao_car_reach, 0.4, "MCLA/GPU",
    "Diagnostic: how far what a car blocks reaches (0.4 of the default 3 m is 1.2 m: the dark line, not a halo); the car's own "
    "body is darkened with the whole radius, as before. 1 (or 0): a wall. Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
@DEF_BOOL(
    mcla_gpu_ssgi_half, true, "MCLA/GPU",
    "Diagnostic: half size above 1080 lines. Live; the first frame after the change makes the images again.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
@DEF_DOUBLE(
    mcla_gpu_ao_paint, 0.3125, "MCLA/GPU",
    "Rounded to sixteenths, 1/16 to 1. Live.")
    .debug_only()
    .range(0.0625, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
)CPP");

const std::string kRows = Expand(R"ROWS(
  // One alpha-to-mask pattern for every draw (a comment with a ) and a @ROW( in it).
  @ROW(f64, mcla_gpu_ao_car_reach_extra, 0.9, "Not the one asked for.", rex::kDebugOnly);

  @ROW(f64, mcla_gpu_ao_car_reach, 0.4, "Diagnostic: how far what a car blocks reaches (0.4 of the default 3 m is 1.2 m: the dark line, not a halo); "
    "the car's own body is darkened with the whole radius, as before. 1 (or 0): a wall. Live.", rex::kDebugOnly, rex::Range{0.0, 1.0});

  @ROW(bool, mcla_gpu_ssgi_half, true, "Half size above 1080 lines. Live; needs the images again.", rex::kRestart, rex::kDebugOnly);  // a comment, with a ")"

  @ROW(f64, mcla_gpu_ao_paint, 0.3125, "Rounded to sixteenths, 1/16 to 1. Live.", rex::kDebugOnly, rex::Range{0.0625, 1.0});

  @ROW(std::string, mcla_gpu_mode, "auto", "One of auto, off.", rex::Allowed("auto", "off"), rex::kStartupOnly);

  @ROW(bool, mcla_gpu_plain, true, "No traits at all (live, a player's setting).");
)ROWS");

void CheckRows() {
  const ss::Info reach = ss::FromRow(kRows, "mcla_gpu_ao_car_reach");
  Check(reach.found && reach.default_text == "0.4", "a row: the default is read (and not the default of a name that starts the same)");
  Check(reach.debug_only && reach.live && reach.has_range && reach.range_lo == 0.0 && reach.range_hi == 1.0, "a row: debug-only, live, range 0 to 1");
  Check(ss::DefaultIs(reach, 0.4f) && !ss::DefaultIs(reach, 0.5), "a row: the default compares as a number (to a float constant too)");

  const ss::Info half = ss::FromRow(kRows, "mcla_gpu_ssgi_half");
  Check(half.found && ss::DefaultIsTrue(half) && half.debug_only && !half.live && !half.has_range, "a row: kRestart is not live, no range");

  const ss::Info paint = ss::FromRow(kRows, "mcla_gpu_ao_paint");
  Check(paint.found && paint.live && paint.has_range && paint.range_lo == 0.0625 && paint.range_hi == 1.0, "a row: the range 1/16 to 1");

  const ss::Info mode = ss::FromRow(kRows, "mcla_gpu_mode");
  Check(mode.found && mode.default_text == "\"auto\"" && !mode.live && !mode.debug_only && !mode.has_range, "a row: a string setting with an Allowed list and kStartupOnly");

  const ss::Info plain = ss::FromRow(kRows, "mcla_gpu_plain");
  Check(plain.found && plain.live && !plain.debug_only, "a row with no traits is live and not debug-only");

  Check(!ss::FromRow(kRows, "mcla_gpu_ao").found && !ss::FromRow(kRows, "mcla_gpu_missing").found, "a row: a name that is only the start of another is not found");
  Check(!ss::FromRow(kOldCpp, "mcla_gpu_ao_car_reach").found, "a row: the old form is not read as a row");
}

void CheckDefines() {
  const ss::Info reach = ss::FromDefine(kOldCpp, "mcla_gpu_ao_car_reach");
  Check(reach.found && reach.default_text == "0.4", "a define: the default is read (and not the default of a name that starts the same)");
  Check(reach.debug_only && reach.live && reach.has_range && reach.range_lo == 0.0 && reach.range_hi == 1.0, "a define: debug-only, live, range 0 to 1");

  const ss::Info half = ss::FromDefine(kOldCpp, "mcla_gpu_ssgi_half");
  Check(half.found && ss::DefaultIsTrue(half) && !half.debug_only && !half.live && !half.has_range, "a define: kRequiresRestart is not live, no .debug_only()");

  const ss::Info paint = ss::FromDefine(kOldCpp, "mcla_gpu_ao_paint");
  Check(paint.found && paint.debug_only && paint.live && paint.range_lo == 0.0625 && paint.range_hi == 1.0, "a define: the range 1/16 to 1 after a help text with semicolons");

  Check(!ss::FromDefine(kOldCpp, "mcla_gpu_ao").found && !ss::FromDefine(kOldCpp, "mcla_gpu_missing").found, "a define: a name that is only the start of another is not found");
  Check(!ss::FromDefine(kRows, "mcla_gpu_ao_car_reach").found, "a define: the new form is not read as a define");
}

void CheckFind() {
  // No src/settings/generated folder beside this path: Find falls back to the define in the .cpp text.
  const ss::Info old = ss::Find("no-such-folder/gpu", kOldCpp, "mcla_gpu_ao_paint");
  Check(old.found && old.live && old.debug_only, "Find: with no generated folder it reads the define");
  Check(!ss::Find("no-such-folder/gpu", "", "mcla_gpu_ao_paint").found, "Find: nothing anywhere is not found");
  Check(!ss::DefaultIs(ss::Info{}, 0.0) && !ss::DefaultIsTrue(ss::Info{}), "an unfound setting has no default");
}

}  // namespace

int main() {
  CheckRows();
  CheckDefines();
  CheckFind();
  std::printf("setting_source_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
