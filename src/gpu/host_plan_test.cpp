// Check of the rule that says which host passes a start makes pipelines for (host_plan.h): the class
// against the settings' texts, the plan, the words of the log line and the occlusion's pipeline groups
// (with the contact shadows' and the bounce light's).
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

#include "ao_size.h"
#include "host_plan.h"

using namespace mcla::gpu::host_plan;
using mcla::gpu::gpu_class::GpuClass;
namespace sl = mcla::gpu::shafts_level;

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

bool Near(double a, double b, double tolerance) { return (a > b ? a - b : b - a) <= tolerance; }

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
const char* const kClassNames[] = {"low", "mid", "high"};

// What `auto` stands for, written out again (the design's table, section 9): off/low/high, in class order. The
// sun shafts are not here: their setting is a strength, not a level (CheckShafts below). The bounce light's is
// low on High only (owner, 2026-11-14: about 0.75 ms at 4K on an RTX 5080), off on Mid and Low.
struct AutoRow {
  Pass pass;
  const char* cells[3];
};
const AutoRow kAuto[] = {
    {Pass::kContact, {"off", "low", "high"}},
    {Pass::kBounce, {"off", "off", "low"}},
    // (the car self-reflections are not here: their one level is `on`, which the plan reads as its high, and the old low and high are
    // read as on, so they have a check of their own, CheckCarRefl)
    // the ambient occlusion: off on Low, low on Mid, high on High (gpu_class.h: AO-DESIGN.md section 5)
    {Pass::kOcclusion, {"off", "low", "high"}},
};

Level LevelOf(const std::string& name) {
  Level level = Level::kOff;
  ParseLevel(name, &level);
  return level;
}

void CheckOcclusion() {
  // The player's own values: the same on every class; a word that is not auto, off, low or high is off (`auto` follows
  // the class: CheckAutoPasses).
  const struct {
    const char* text;
    Level want;
    bool unknown;
  } rows[] = {
      {"off", Level::kOff, false},   {"low", Level::kLow, false},   {"high", Level::kHigh, false},
      {"High", Level::kHigh, false}, {"LOW", Level::kLow, false},
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
  // A word that is not a value: off for the contact shadows and the bounce light (the shafts: CheckShafts).
  for (int k = 0; k < 3; ++k) {
    for (const char* junk : {"", "ultra", "hgih", "on", "game", "auto "}) {
      const std::string where = std::string("'") + junk + "' on " + kClassNames[k];
      const Choice contact = Resolve(Pass::kContact, kClasses[k], true, junk);
      Check(contact.level == Level::kOff && contact.unknown && !contact.automatic,
            "contact shadows " + where + ": off, unknown");
      const Choice bounce = Resolve(Pass::kBounce, kClasses[k], true, junk);
      Check(bounce.level == Level::kOff && bounce.unknown && !bounce.automatic,
            "bounce light " + where + ": off, unknown");
      const Choice car = Resolve(Pass::kCarRefl, kClasses[k], true, junk);
      const bool car_value = std::string(junk) == "on";  // `on` is the car self-reflections' own level
      Check(car_value ? (car.level == Level::kHigh && !car.unknown && !car.automatic)
                      : (car.level == Level::kOff && car.unknown && !car.automatic),
            "car self-reflections " + where + (car_value ? ": on, a value" : ": off, unknown"));
      const Choice ao = Resolve(Pass::kOcclusion, kClasses[k], true, junk);
      Check(ao.level == Level::kOff && ao.unknown && !ao.automatic,
            "ambient occlusion " + where + ": off, unknown");
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
    Check(Resolve(Pass::kShafts, kClasses[k], false, "auto").strength ==
              Resolve(Pass::kShafts, kClasses[k], true, "auto").strength,
          "sun shafts auto does not wait for the class");
    Check(Resolve(Pass::kContact, kClasses[k], false, "auto").level == LevelOf(kAuto[0].cells[k]),
          "contact shadows auto does not wait for the class");
    Check(Resolve(Pass::kCarRefl, kClasses[k], false, "auto").level == (k == 0 ? Level::kOff : Level::kHigh),
          "car self-reflections auto does not wait for the class");
  }
}

// The sun shafts' setting is a strength (shafts_level.h): off, light, default, strong, stronger, strongest, `auto` (DEFAULT on Mid
// and High, OFF on Low), and the old low and high (read as default). The plan's level is off for no shafts, else the ray
// quality the class gives: low on Low and Mid (32 steps), high on High (64).
void CheckShafts() {
  const Level quality[] = {Level::kLow, Level::kLow, Level::kHigh};
  const sl::Level auto_strength[] = {sl::Level::kOff, sl::Level::kDefault, sl::Level::kDefault};
  struct Own {
    const char* text;
    sl::Level strength;
  };
  const Own own[] = {{"off", sl::Level::kOff},         {"light", sl::Level::kLight},   {"default", sl::Level::kDefault},
                     {"strong", sl::Level::kStrong},   {"stronger", sl::Level::kStronger}, {"strongest", sl::Level::kStrongest},
                     {"Strong", sl::Level::kStrong},
                     {"OFF", sl::Level::kOff}};
  for (int k = 0; k < 3; ++k) {
    const std::string at = std::string("sun shafts on ") + kClassNames[k];
    const bool auto_on = auto_strength[k] != sl::Level::kOff;
    for (const char* text : {"auto", "AUTO"}) {
      const Choice a = Resolve(Pass::kShafts, kClasses[k], true, text);
      Check(a.strength == auto_strength[k] && a.automatic && !a.unknown && !a.legacy, at + ": '" + text + "' is automatic: " + std::string(sl::Name(auto_strength[k])));
      Check(a.level == (auto_on ? quality[k] : Level::kOff), at + ": '" + text + "': the level is the ray quality, or off");
    }
    for (const Own& o : own) {
      const Choice c = Resolve(Pass::kShafts, kClasses[k], true, o.text);
      const bool on = o.strength != sl::Level::kOff;
      Check(c.strength == o.strength && !c.automatic && !c.unknown && !c.legacy, at + ": '" + o.text + "' is the player's own");
      Check(c.level == (on ? quality[k] : Level::kOff), at + ": '" + o.text + "': the level is the ray quality, or off");
    }
    // The old values: default on every class (also on Low, where `auto` is off), marked.
    for (const char* old : {"low", "high", "Low", "HIGH"}) {
      const Choice c = Resolve(Pass::kShafts, kClasses[k], true, old);
      Check(c.strength == sl::Level::kDefault && c.legacy && !c.automatic && !c.unknown && c.level == quality[k],
            at + ": the old '" + old + "' is read as default");
    }
    // A word that is not a value counts as `auto`.
    for (const char* junk : {"", "ultra", "hgih", "on", "game", "auto "}) {
      const Choice c = Resolve(Pass::kShafts, kClasses[k], true, junk);
      Check(c.strength == auto_strength[k] && c.unknown && c.automatic && !c.legacy && c.level == (auto_on ? quality[k] : Level::kOff),
            at + ": '" + junk + "' is taken as auto (" + std::string(sl::Name(auto_strength[k])) + ")");
    }
    // The other passes carry no strength.
    Check(Resolve(Pass::kContact, kClasses[k], true, "high").strength == sl::Level::kOff, at + ": contact shadows leave the strength alone");
  }
}

struct PlanRow {
  const char* ao;
  const char* contact;
  const char* shafts;
  const char* bounce;
  GpuClass cls;
  // the passes that are on, as five letters in the order occlusion, contact, shafts, bounce, car self-reflections: '-' off
  const char* on;
  const char* what;
};

void CheckPlans() {
  const PlanRow rows[] = {
      // The defaults of a fresh install (every setting auto: the occlusion follows the class too).
      {"auto", "auto", "auto", "auto", GpuClass::kLow, "-----", "fresh install, Low: nothing is on"},
      {"auto", "auto", "auto", "auto", GpuClass::kMid, "OCS--", "fresh install, Mid: occlusion, contact shadows, shafts"},
      {"auto", "auto", "auto", "auto", GpuClass::kHigh, "OCSB-", "fresh install, High: all four (the bounce light's auto is low on High)"},
      // An older larecomp.toml holds high for the occlusion: kept as it is on every class.
      {"high", "auto", "auto", "auto", GpuClass::kLow, "O----", "defaults, Low: only the occlusion"},
      {"high", "auto", "auto", "auto", GpuClass::kMid, "OCS--", "defaults, Mid: occlusion, contact shadows, shafts"},
      {"high", "auto", "auto", "auto", GpuClass::kHigh, "OCSB-", "defaults, High: all four (the bounce light's auto is low on High)"},
      // The occlusion off: nothing of it, and no bounce light (it needs the occlusion's images).
      {"off", "auto", "auto", "auto", GpuClass::kHigh, "-CS--", "occlusion off, High: contact and shafts stay, bounce light cannot"},
      {"off", "auto", "auto", "auto", GpuClass::kLow, "-----", "occlusion off, Low: nothing"},
      {"off", "off", "off", "off", GpuClass::kHigh, "-----", "everything off, High"},
      {"low", "off", "off", "off", GpuClass::kHigh, "O----", "occlusion low only"},
      // A player's own choices override the class both ways.
      {"high", "high", "strong", "high", GpuClass::kLow, "OCSB-", "all on by hand on Low: the player's own"},
      {"high", "off", "off", "off", GpuClass::kHigh, "O----", "all off on High: the player's own"},
      {"high", "low", "auto", "auto", GpuClass::kLow, "OC---", "contact low on Low"},
      {"high", "auto", "strong", "auto", GpuClass::kMid, "OCS--", "shafts strong on Mid"},
      {"high", "auto", "high", "auto", GpuClass::kLow, "O-S--", "the old shafts value high on Low: read as default, on"},
      {"high", "auto", "light", "auto", GpuClass::kLow, "O-S--", "shafts light on Low: the player's own"},
      {"high", "auto", "off", "off", GpuClass::kHigh, "OC---", "shafts off on High: the player's own"},
      {"high", "auto", "auto", "low", GpuClass::kMid, "OCSB-", "bounce light low on Mid"},
      {"high", "auto", "auto", "high", GpuClass::kHigh, "OCSB-", "bounce light high on High"},
      {"low", "off", "off", "low", GpuClass::kLow, "O--B-", "bounce light low with the occlusion low on Low"},
      {"off", "auto", "auto", "high", GpuClass::kHigh, "-CS--", "bounce light high with the occlusion off: held off"},
      {"high", "auto", "auto", "low", GpuClass::kHigh, "OCSB-", "bounce light low on High: the player's own, as auto"},
      {"high", "auto", "auto", "off", GpuClass::kHigh, "OCS--", "bounce light off on High: the player's own"},
      // Words that are not values.
      {"auto", "junk", "junk", "junk", GpuClass::kHigh, "O-S--", "junk: occlusion auto (high on High), contact off, shafts as auto (High), bounce off"},
      {"junk", "auto", "auto", "auto", GpuClass::kHigh, "-CS--", "junk for the occlusion: off, the rest as auto"},
  };
  for (const PlanRow& row : rows) {
    const Plan plan = Decide(row.cls, true, row.ao, row.contact, row.shafts, row.bounce);
    std::string got;
    for (int i = 0; i < kPassCount; ++i) {
      got += plan.pass[i].level != Level::kOff ? "OCSBR"[i] : '-';
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
            high.of(Pass::kBounce).level == Level::kLow,
        "plan: High's levels are high, high, low");
  const Plan high_low = Decide(GpuClass::kHigh, true, "high", "auto", "auto", "low");
  Check(high_low.of(Pass::kBounce).level == Level::kLow && !high_low.of(Pass::kBounce).automatic &&
            !high_low.of(Pass::kBounce).held,
        "plan: the bounce light set to low on High is low, set by the player");
  const Plan high_gi = Decide(GpuClass::kHigh, true, "high", "auto", "auto", "high");
  Check(high_gi.of(Pass::kBounce).level == Level::kHigh, "plan: High with the bounce light set to high: high");
  // The class unknown: the bounce light waits, the rest follow the class given.
  const Plan unknown = Decide(GpuClass::kHigh, false, "high", "auto", "auto", "auto");
  Check(!unknown.on(Pass::kBounce) && unknown.on(Pass::kContact) && unknown.on(Pass::kShafts),
        "plan: with the class not known the bounce light is off");
}

// The car self-reflections in the plan: they stand alone (no other pass is needed for them, and they need none of the
// occlusion's images), so the plan holds them on wherever the setting says, with the other four as they were.
void CheckCarRefl() {
  // One level, `on` (the plan's high): low and high cost the same (perf/out/FXCOST-B6.txt), so the setting dropped low; an old
  // low or high is read as on and flagged legacy. Level::kLow is never what the setting comes to.
  struct Row {
    const char* car;
    GpuClass cls;
    Level want;
    bool automatic;
    bool legacy;
    const char* what;
  };
  const Row rows[] = {
      {"auto", GpuClass::kLow, Level::kOff, true, false, "auto on Low: off (an integrated GPU does without)"},
      {"auto", GpuClass::kMid, Level::kHigh, true, false, "auto on Mid: on (it was low; the two cost the same)"},
      {"auto", GpuClass::kHigh, Level::kHigh, true, false, "auto on High: on"},
      {"AUTO", GpuClass::kMid, Level::kHigh, true, false, "AUTO (capitals) on Mid: on"},
      {"off", GpuClass::kHigh, Level::kOff, false, false, "off on High"},
      {"on", GpuClass::kLow, Level::kHigh, false, false, "on on Low: the player's own"},
      {"ON", GpuClass::kMid, Level::kHigh, false, false, "ON (capitals) on Mid"},
      {"low", GpuClass::kLow, Level::kHigh, false, true, "low on Low: an old value, read as on"},
      {"low", GpuClass::kHigh, Level::kHigh, false, true, "low on High: an old value, read as on"},
      {"LOW", GpuClass::kMid, Level::kHigh, false, true, "LOW (capitals) on Mid: read as on"},
      {"high", GpuClass::kLow, Level::kHigh, false, true, "high on Low: an old value, read as on"},
      {"High", GpuClass::kMid, Level::kHigh, false, true, "High (capital) on Mid: read as on"},
      {"hgih", GpuClass::kHigh, Level::kOff, false, false, "a word that is not a value: off"},
  };
  for (const Row& row : rows) {
    const Plan plan = Decide(row.cls, true, "high", "auto", "auto", "auto", row.car);
    Check(plan.of(Pass::kCarRefl).level == row.want, std::string("car self-reflections ") + row.what + ": level");
    Check(plan.of(Pass::kCarRefl).level != Level::kLow, std::string("car self-reflections ") + row.what + ": never the low level");
    Check(plan.of(Pass::kCarRefl).automatic == row.automatic, std::string("car self-reflections ") + row.what + ": automatic");
    Check(plan.of(Pass::kCarRefl).legacy == row.legacy, std::string("car self-reflections ") + row.what + ": legacy");
    // The other four do not change with it.
    const Plan without = Decide(row.cls, true, "high", "auto", "auto", "auto");
    for (const Pass p : {Pass::kOcclusion, Pass::kContact, Pass::kShafts, Pass::kBounce}) {
      Check(plan.of(p).level == without.of(p).level, std::string("car self-reflections ") + row.what + ": the other passes are as they were");
    }
  }
  // Not asked for: off (the other callers' call, the compare key's old rows).
  Check(!Decide(GpuClass::kHigh, true, "high", "auto", "auto", "auto").on(Pass::kCarRefl),
        "car self-reflections: a plan made without the setting has them off");
  // Not held by the occlusion: with the occlusion off they stay as they are.
  Check(Decide(GpuClass::kHigh, true, "off", "auto", "auto", "auto", "auto").on(Pass::kCarRefl),
        "car self-reflections do not need the ambient occlusion");
  CheckEq(std::string(PassName(Pass::kCarRefl)), "car self-reflections", "PassName(car self-reflections)");
  CheckEq(Describe(Pass::kCarRefl, Resolve(Pass::kCarRefl, GpuClass::kMid, true, "auto"), GpuClass::kMid),
          "car self-reflections on (auto on the mid class)", "Describe: car self-reflections, auto on Mid");
  CheckEq(Describe(Pass::kCarRefl, Resolve(Pass::kCarRefl, GpuClass::kHigh, true, "off"), GpuClass::kHigh),
          "car self-reflections off (set)", "Describe: car self-reflections set to off");
  CheckEq(Describe(Pass::kCarRefl, Resolve(Pass::kCarRefl, GpuClass::kHigh, true, "on"), GpuClass::kHigh),
          "car self-reflections on (set)", "Describe: car self-reflections set to on");
  CheckEq(Describe(Pass::kCarRefl, Resolve(Pass::kCarRefl, GpuClass::kMid, true, "low"), GpuClass::kMid),
          "car self-reflections on (an old value of the setting (low or high): read as on)",
          "Describe: car self-reflections, an old low");
  CheckEq(std::string(mcla::gpu::gpu_class::CvarName(EffectOf(Pass::kCarRefl))), "mcla_gpu_car_selfrefl",
          "the car self-reflections' pass reads the cvar's own cell of the table");
}

void CheckDescribe() {
  CheckEq(Describe(Pass::kContact, Resolve(Pass::kContact, GpuClass::kMid, true, "auto"), GpuClass::kMid),
          "contact shadows low (auto on the mid class)", "Describe: auto on Mid");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kLow, true, "auto"), GpuClass::kLow),
          "sun shafts off (auto on the low class)", "Describe: auto on Low");
  CheckEq(Describe(Pass::kOcclusion, Resolve(Pass::kOcclusion, GpuClass::kHigh, true, "high"), GpuClass::kHigh),
          "ambient occlusion high (set)", "Describe: the occlusion's own value");
  CheckEq(Describe(Pass::kOcclusion, Resolve(Pass::kOcclusion, GpuClass::kMid, true, "auto"), GpuClass::kMid),
          "ambient occlusion low (auto on the mid class)", "Describe: the occlusion's auto on Mid");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kMid, true, "auto"), GpuClass::kMid),
          "sun shafts default (auto on the mid class)", "Describe: shafts auto on Mid");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kHigh, true, "strong"), GpuClass::kHigh),
          "sun shafts strong (set)", "Describe: a strength the player set");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kHigh, true, "high"), GpuClass::kHigh),
          "sun shafts default (an old value of the setting (low or high): read as default)",
          "Describe: an old value");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kHigh, true, "hgih"), GpuClass::kHigh),
          "sun shafts default (not a value of the setting: taken as auto on the high class)",
          "Describe: a word the shafts take as auto");
  CheckEq(Describe(Pass::kShafts, Resolve(Pass::kShafts, GpuClass::kLow, true, "hgih"), GpuClass::kLow),
          "sun shafts off (not a value of the setting: taken as auto on the low class)",
          "Describe: a word the shafts take as auto, on Low");
  CheckEq(Describe(Pass::kContact, Resolve(Pass::kContact, GpuClass::kHigh, true, "hgih"), GpuClass::kHigh),
          "contact shadows off (not a value of the setting: off)", "Describe: a word that is off");
  CheckEq(std::string(PassName(Pass::kBounce)), "bounce light", "PassName(bounce)");
  CheckEq(Describe(Pass::kBounce, Resolve(Pass::kBounce, GpuClass::kHigh, true, "auto"), GpuClass::kHigh),
          "bounce light low (auto on the high class)", "Describe: the bounce light's auto is low on High");
  CheckEq(Describe(Pass::kBounce, Resolve(Pass::kBounce, GpuClass::kHigh, true, "low"), GpuClass::kHigh),
          "bounce light low (set)", "Describe: the bounce light set to low");
  CheckEq(Describe(Pass::kBounce,
                   HoldBounce(Resolve(Pass::kBounce, GpuClass::kHigh, true, "high"), Level::kOff),
                   GpuClass::kHigh),
          "bounce light off (needs the ambient occlusion, which is off)",
          "Describe: the bounce light held off by the occlusion");
}

void CheckHold() {
  // The bounce light is held off exactly while the occlusion is off and the bounce light would be on.
  for (const char* text : {"auto", "off", "low", "high", "junk"}) {
    for (int k = 0; k < 3; ++k) {
      const std::string where = std::string("'") + text + "' on " + kClassNames[k];
      const Choice own = Resolve(Pass::kBounce, kClasses[k], true, text);
      const Choice with_ao = HoldBounce(own, Level::kLow);
      Check(with_ao.level == own.level && !with_ao.held, "hold: " + where + " with the occlusion on is as it was");
      const Choice no_ao = HoldBounce(own, Level::kOff);
      Check(no_ao.level == Level::kOff, "hold: " + where + " with the occlusion off is off");
      Check(no_ao.held == (own.level != Level::kOff),
            "hold: " + where + " is marked held only when it would have been on");
      Check(no_ao.automatic == own.automatic && no_ao.unknown == own.unknown,
            "hold: " + where + ": what the setting said is kept");
    }
  }
}

void CheckAmbient() {
  // Every triple of levels, with and without the census.
  const Level levels[] = {Level::kOff, Level::kLow, Level::kHigh};
  for (const Level occlusion : levels) {
    for (const Level contact : levels) {
      for (const Level bounce : levels) {
        for (const bool census : {false, true}) {
          const AmbientNeeds n = Ambient(occlusion, contact, bounce, census);
          const bool ao = occlusion != Level::kOff;
          const bool cs = contact != Level::kOff;
          const bool gi = ao && bounce != Level::kOff;
          const std::string what = std::string("occlusion ") + std::string(LevelName(occlusion)) +
                                   ", contact " + std::string(LevelName(contact)) + ", bounce " +
                                   std::string(LevelName(bounce)) + (census ? ", census" : "");
          Check(n.depth == (ao || cs || census), what + ": depth");
          Check(n.mip == (ao || census), what + ": mip");
          Check(n.occlusion == ao, what + ": occlusion's visibility and history");
          Check(n.denoise == (ao || cs), what + ": denoise");
          Check(n.apply == (ao || cs), what + ": apply");
          Check(n.contact == cs, what + ": contact march");
          Check(n.bounce == gi, what + ": bounce light's passes (only with the occlusion)");
          Check(n.any() == (ao || cs || census), what + ": any");
        }
      }
    }
  }
  // The ones that matter at start: all off lists nothing; the contact alone has no visibility chain; the
  // bounce light alone (its setting on, the occlusion off) lists nothing; all on lists every group.
  Check(!Ambient(Level::kOff, Level::kOff, Level::kOff, false).any(),
        "nothing is needed with the occlusion, the contact shadows and the bounce light off");
  const AmbientNeeds contact_only = Ambient(Level::kOff, Level::kLow, Level::kOff, false);
  Check(contact_only.depth && contact_only.denoise && contact_only.apply && contact_only.contact &&
            !contact_only.occlusion && !contact_only.mip && !contact_only.bounce,
        "contact alone: depth, denoise, apply and the march; no visibility chain, no coarser levels");
  Check(!Ambient(Level::kOff, Level::kOff, Level::kHigh, false).any(),
        "the bounce light without the occlusion needs nothing");
  const AmbientNeeds all = Ambient(Level::kHigh, Level::kHigh, Level::kHigh, false);
  Check(all.depth && all.mip && all.occlusion && all.denoise && all.apply && all.contact && all.bounce,
        "all three on: every group, the occlusion's plain ones too (the pause menu's world runs them)");
}

// The sizes of the images those levels make (ao_size.h): above 1080 lines of render height the occlusion's and the bounce light's images
// are half size on every class, 1080 and below are as they were, and the memory is never more than before. The same rule as ao.cpp's.
void CheckImageSizes() {
  namespace as = mcla::gpu::ao_size;
  const double mib = 1024.0 * 1024.0;
  const struct {
    uint32_t w, h;
  } pictures[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
  for (const GpuClass cls : kClasses) {
    for (const char* bounce : {"off", "low", "high"}) {
      const Plan plan = Decide(cls, true, "high", "auto", "off", bounce);
      const Level ao = plan.of(Pass::kOcclusion).level;
      const Level cs = plan.of(Pass::kContact).level;
      const Level gi = plan.of(Pass::kBounce).level;
      for (const auto& p : pictures) {
        const std::string what = std::string(kClassNames[static_cast<int>(cls)]) + " class, bounce " + bounce + ", " +
                                 std::to_string(p.w) + "x" + std::to_string(p.h);
        as::Inputs in;
        in.render_height = p.h;
        in.occlusion = ao != Level::kOff;
        in.occlusion_high = ao == Level::kHigh;
        in.contact = cs != Level::kOff;
        in.contact_high = cs == Level::kHigh;
        in.bounce = in.occlusion && gi != Level::kOff;
        in.bounce_high = in.bounce && gi == Level::kHigh;
        const as::Choice c = as::Decide(in);
        as::Frame f;
        f.render[0] = p.w;
        f.render[1] = p.h;
        f.choice = c;
        f.occlusion = in.occlusion;
        f.contact = in.contact;
        f.bounce = in.bounce;
        if (p.h <= 1080) {
          Check(!c.by_height && !c.contact_full, what + ": 1080 lines and below the height rule does nothing");
        } else if (in.occlusion) {
          Check(c.low && c.by_height, what + ": above 1080 lines the occlusion (and the bounce light) is traced at half size");
          Check(c.contact_full == in.contact, what + ": the contact shadows keep their size exactly when they are on");
        }
        Check(as::Bytes(f).Total() <= as::BytesBefore(f).Total(), what + ": no more video memory than before");
        if (in.occlusion && !in.bounce && p.h > 1080) {
          Check(as::Bytes(f).Total() < as::BytesBefore(f).Total(), what + ": less video memory than before");
        }
        // The numbers of the audit: the occlusion on high alone holds 131.5 MiB at 4K and 58.4 at 1440p, the High class (contact high) as well.
        if (in.occlusion && !in.bounce && p.h == 2160) {
          Check(Near(as::BytesBefore(f).Total() / mib, 131.5, 0.1), what + ": before 131.5 MiB");
          const double want = in.contact ? 80.3 : 32.9;
          Check(Near(as::Bytes(f).Total() / mib, want, 0.1), what + ": now " + std::to_string(want) + " MiB");
        }
        if (in.occlusion && !in.bounce && p.h == 1440) {
          const double want = in.contact ? 35.7 : 14.6;
          Check(Near(as::BytesBefore(f).Total() / mib, 58.4, 0.1) && Near(as::Bytes(f).Total() / mib, want, 0.1),
                what + ": 1440p 58.4 MiB before, " + std::to_string(want) + " now");
        }
      }
    }
  }
}

}  // namespace

int main() {
  CheckImageSizes();
  CheckOcclusion();
  CheckAutoPasses();
  CheckShafts();
  CheckPlans();
  CheckCarRefl();
  CheckDescribe();
  CheckHold();
  CheckAmbient();
  if (g_failed != 0) {
    std::printf("host_plan_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("host_plan_test: %d checks passed\n", g_checks);
  return 0;
}
