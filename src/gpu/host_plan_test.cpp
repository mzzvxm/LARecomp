// Check of the rule that says which host passes a start makes pipelines for (host_plan.h): the class
// against the settings' texts, the plan, the words of the log line and the occlusion's pipeline groups.
// A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_host_plan_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_host_plan_test
//
// The expected values are written out here again on purpose, as gpu_class_test.cpp does: the table of
// GFX-UPGRADES-DESIGN.md section 9 and the owners' old ReadMode functions are the other side of the check.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "host_plan.h"

using namespace mcla::gpu::host_plan;
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

const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
const char* const kClassNames[] = {"low", "mid", "high"};

// What `auto` stands for, written out again (the design's table, section 9): off/low/high, in class order.
struct AutoRow {
  Pass pass;
  const char* cells[3];
};
const AutoRow kAuto[] = {
    {Pass::kContact, {"off", "low", "high"}},
    {Pass::kShafts, {"off", "low", "high"}},
    // off on every class until its cost is measured (owner, 2026-11-13; gpu_class.h ab21a334)
    {Pass::kBounce, {"off", "off", "off"}},
};

Level LevelOf(const std::string& name) {
  Level level = Level::kOff;
  ParseLevel(name, &level);
  return level;
}

void CheckOcclusion() {
  // No `auto`, no class: the same on every class; a word that is not off, low or high is off.
  const struct {
    const char* text;
    Level want;
    bool unknown;
  } rows[] = {
      {"off", Level::kOff, false},   {"low", Level::kLow, false},   {"high", Level::kHigh, false},
      {"High", Level::kHigh, false}, {"LOW", Level::kLow, false},   {"auto", Level::kOff, true},
      {"", Level::kOff, true},       {"ultra", Level::kOff, true},  {"hgih", Level::kOff, true},
      {"high ", Level::kOff, true},
  };
  for (int k = 0; k < 3; ++k) {
    for (const auto& row : rows) {
      const Choice c = Resolve(Pass::kOcclusion, kClasses[k], true, row.text);
      const std::string what = std::string("occlusion '") + row.text + "' on " + kClassNames[k];
      Check(c.level == row.want, what + ": level");
      Check(c.unknown == row.unknown, what + ": unknown");
      Check(!c.automatic, what + ": never automatic");
    }
  }
}

void CheckAutoPasses() {
  const char* const kOwn[] = {"off", "low", "high", "Off", "LOW", "High"};
  for (const AutoRow& row : kAuto) {
    const std::string name(PassName(row.pass));
    for (int k = 0; k < 3; ++k) {
      const std::string cell = name + " on " + kClassNames[k];
      const Choice a = Resolve(row.pass, kClasses[k], true, "auto");
      Check(a.level == LevelOf(row.cells[k]), cell + ": auto is " + row.cells[k]);
      Check(a.automatic && !a.unknown, cell + ": auto is automatic and known");
      const Choice big = Resolve(row.pass, kClasses[k], true, "AUTO");
      Check(big.level == a.level && big.automatic, cell + ": AUTO is auto");
      for (const char* own : kOwn) {
        const Choice c = Resolve(row.pass, kClasses[k], true, own);
        Check(c.level == LevelOf(Lower(own)) && !c.automatic && !c.unknown,
              cell + ": '" + own + "' is the player's own and the class is not looked at");
      }
      // The gpu_class table is the same one.
      Check(std::string(mcla::gpu::gpu_class::Cell(EffectOf(row.pass), kClasses[k])) == row.cells[k],
            cell + ": the table gpu_class.h holds is the one this test expects");
    }
  }
  // A word that is not a value: off for the contact shadows and the bounce light, `auto` for the shafts.
  for (int k = 0; k < 3; ++k) {
    for (const char* junk : {"", "ultra", "hgih", "on", "game", "auto "}) {
      const std::string where = std::string("'") + junk + "' on " + kClassNames[k];
      const Choice contact = Resolve(Pass::kContact, kClasses[k], true, junk);
      Check(contact.level == Level::kOff && contact.unknown && !contact.automatic,
            "contact shadows " + where + ": off, unknown");
      const Choice bounce = Resolve(Pass::kBounce, kClasses[k], true, junk);
      Check(bounce.level == Level::kOff && bounce.unknown && !bounce.automatic,
            "bounce light " + where + ": off, unknown");
      const Choice shafts = Resolve(Pass::kShafts, kClasses[k], true, junk);
      Check(shafts.level == LevelOf(kAuto[1].cells[k]) && shafts.unknown && shafts.automatic,
            "sun shafts " + where + ": taken as auto (" + kAuto[1].cells[k] + ")");
    }
  }
  // The bounce light's `auto` waits for the class: off while it is not known, whatever class is guessed.
  for (int k = 0; k < 3; ++k) {
    const Choice c = Resolve(Pass::kBounce, kClasses[k], false, "auto");
    Check(c.level == Level::kOff && c.automatic, std::string("bounce light auto, class not known, guessed ") +
                                                     kClassNames[k] + ": off");
    const Choice own = Resolve(Pass::kBounce, kClasses[k], false, "high");
    Check(own.level == Level::kHigh, "bounce light high, class not known: the player's own is kept");
    // the other two do not look at it
    Check(Resolve(Pass::kShafts, kClasses[k], false, "auto").level == LevelOf(kAuto[1].cells[k]),
          "sun shafts auto does not wait for the class");
    Check(Resolve(Pass::kContact, kClasses[k], false, "auto").level == LevelOf(kAuto[0].cells[k]),
          "contact shadows auto does not wait for the class");
  }
}

struct PlanRow {
  const char* ao;
  const char* contact;
  const char* shafts;
  const char* bounce;
  GpuClass cls;
  // the passes that are on, as four letters in the order occlusion, contact, shafts, bounce: '-' off
  const char* on;
  const char* what;
};

void CheckPlans() {
  const PlanRow rows[] = {
      // The defaults of a fresh install (mcla_gpu_ao high, the others auto).
      {"high", "auto", "auto", "auto", GpuClass::kLow, "O---", "defaults, Low: only the occlusion"},
      {"high", "auto", "auto", "auto", GpuClass::kMid, "OCS-", "defaults, Mid: occlusion, contact shadows, shafts"},
      {"high", "auto", "auto", "auto", GpuClass::kHigh, "OCS-", "defaults, High: occlusion, contact shadows, shafts (bounce light's auto is off on every class)"},
      // The occlusion off: nothing of it, and no bounce light (it needs the occlusion's images).
      {"off", "auto", "auto", "auto", GpuClass::kHigh, "-CS-", "occlusion off, High: contact and shafts stay, bounce light cannot"},
      {"off", "auto", "auto", "auto", GpuClass::kLow, "----", "occlusion off, Low: nothing"},
      {"off", "off", "off", "off", GpuClass::kHigh, "----", "everything off, High"},
      {"low", "off", "off", "off", GpuClass::kHigh, "O---", "occlusion low only"},
      // A player's own choices override the class both ways.
      {"high", "high", "high", "high", GpuClass::kLow, "OCSB", "all high on Low: the player's own"},
      {"high", "off", "off", "off", GpuClass::kHigh, "O---", "all off on High: the player's own"},
      {"high", "low", "auto", "auto", GpuClass::kLow, "OC--", "contact low on Low"},
      {"high", "auto", "high", "auto", GpuClass::kMid, "OCS-", "shafts high on Mid"},
      {"high", "auto", "auto", "low", GpuClass::kMid, "OCSB", "bounce light low on Mid"},
      {"high", "auto", "auto", "low", GpuClass::kHigh, "OCSB", "bounce light low on High: the player's own (auto is off)"},
      // Words that are not values.
      {"auto", "junk", "junk", "junk", GpuClass::kHigh, "--S-", "junk: occlusion off, contact off, shafts as auto (High), bounce off"},
  };
  for (const PlanRow& row : rows) {
    const Plan plan = Decide(row.cls, true, row.ao, row.contact, row.shafts, row.bounce);
    std::string got;
    for (int i = 0; i < kPassCount; ++i) {
      got += plan.pass[i].level != Level::kOff ? "OCSB"[i] : '-';
    }
    CheckEq(got, row.on, std::string("plan: ") + row.what);
  }
  // The levels, not only on or off.
  const Plan mid = Decide(GpuClass::kMid, true, "low", "auto", "auto", "auto");
  Check(mid.of(Pass::kOcclusion).level == Level::kLow && mid.of(Pass::kContact).level == Level::kLow &&
            mid.of(Pass::kShafts).level == Level::kLow && mid.of(Pass::kBounce).level == Level::kOff,
        "plan: Mid's levels are low, low, low, off");
  const Plan high = Decide(GpuClass::kHigh, true, "high", "auto", "auto", "auto");
  Check(high.of(Pass::kContact).level == Level::kHigh && high.of(Pass::kShafts).level == Level::kHigh &&
            high.of(Pass::kBounce).level == Level::kOff,
        "plan: High's levels are high, high, off");
  const Plan high_gi = Decide(GpuClass::kHigh, true, "high", "auto", "auto", "high");
  Check(high_gi.of(Pass::kBounce).level == Level::kHigh, "plan: High with the bounce light set to high: high");
  // The class unknown: the bounce light waits, the rest follow the class given.
  const Plan unknown = Decide(GpuClass::kHigh, false, "high", "auto", "auto", "auto");
  Check(!unknown.on(Pass::kBounce) && unknown.on(Pass::kContact) && unknown.on(Pass::kShafts),
        "plan: with the class not known the bounce light is off");
}

void CheckDescribe() {
  CheckEq(Describe(Pass::kContact, Resolve(Pass::kContact, GpuClass::kMid, true, "auto"), GpuClass::kMid),
          "contact shadows low (auto on the mid class)", "Describe: auto on Mid");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kLow, true, "auto"), GpuClass::kLow),
          "sun shafts off (auto on the low class)", "Describe: auto on Low");
  CheckEq(Describe(Pass::kOcclusion, Resolve(Pass::kOcclusion, GpuClass::kHigh, true, "high"), GpuClass::kHigh),
          "ambient occlusion high (set)", "Describe: the occlusion's own value");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kHigh, true, "high"), GpuClass::kHigh),
          "sun shafts high (set)", "Describe: a value the player set");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kHigh, true, "hgih"), GpuClass::kHigh),
          "sun shafts high (not a value of the setting: taken as auto on the high class)",
          "Describe: a word the shafts take as auto");
  CheckEq(Describe(Pass::kContact, Resolve(Pass::kContact, GpuClass::kHigh, true, "hgih"), GpuClass::kHigh),
          "contact shadows off (not a value of the setting: off)", "Describe: a word that is off");
  CheckEq(std::string(PassName(Pass::kBounce)), "bounce light", "PassName(bounce)");
}

void CheckAmbient() {
  // Every pair of levels, with and without the census.
  const Level levels[] = {Level::kOff, Level::kLow, Level::kHigh};
  for (const Level occlusion : levels) {
    for (const Level contact : levels) {
      for (const bool census : {false, true}) {
        const AmbientNeeds n = Ambient(occlusion, contact, census);
        const bool ao = occlusion != Level::kOff;
        const bool cs = contact != Level::kOff;
        const std::string what = std::string("occlusion ") + std::string(LevelName(occlusion)) + ", contact " +
                                 std::string(LevelName(contact)) + (census ? ", census" : "");
        Check(n.depth == (ao || cs || census), what + ": depth");
        Check(n.mip == (ao || census), what + ": mip");
        Check(n.occlusion == ao, what + ": occlusion's visibility and history");
        Check(n.denoise == (ao || cs), what + ": denoise");
        Check(n.apply == (ao || cs), what + ": apply");
        Check(n.contact == cs, what + ": contact march");
        Check(n.any() == (ao || cs || census), what + ": any");
      }
    }
  }
  // The two that matter at start: both off lists nothing; the contact alone has no visibility chain.
  Check(!Ambient(Level::kOff, Level::kOff, false).any(), "nothing is needed with the occlusion and the contact off");
  const AmbientNeeds contact_only = Ambient(Level::kOff, Level::kLow, false);
  Check(contact_only.depth && contact_only.denoise && contact_only.apply && contact_only.contact &&
            !contact_only.occlusion && !contact_only.mip,
        "contact alone: depth, denoise, apply and the march; no visibility chain, no coarser levels");
}

}  // namespace

int main() {
  CheckOcclusion();
  CheckAutoPasses();
  CheckPlans();
  CheckDescribe();
  CheckAmbient();
  if (g_failed != 0) {
    std::printf("host_plan_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("host_plan_test: %d checks passed\n", g_checks);
  return 0;
}
