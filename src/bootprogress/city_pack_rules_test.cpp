// A check that needs no game: the city pack's start-up rules (bootprogress/city_pack_rules.h): which state the renderer is told, when the game builds the pack in the background and
// how hard, the words of the log and the command line's @citypack lines; and the hit data's (CITY-HIT-ACTION): which command line actions run in which order (PlanActions), the states and
// the words of its build and check. Build: cmake --build <build dir> --target larecomp_city_pack_rules_test, then run it (exit 0 = passed);
// or standalone: clang++ -std=c++23 -I src src/bootprogress/city_pack_rules_test.cpp

#include <cstdio>
#include <string>

#include "bootprogress/city_pack_rules.h"

using namespace larecomp::citypackbuild;
using mcla::gpu::city_rt::PackState;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

void TestState() {
  // every combination of the five facts: the order of StateFor's answers
  for (int bits = 0; bits < 32; ++bits) {
    Facts f;
    f.source_exists = (bits & 1) != 0;
    f.pack_exists = (bits & 2) != 0;
    f.current = (bits & 4) != 0;
    f.building = (bits & 8) != 0;
    f.failed = (bits & 16) != 0;
    const PackState s = StateFor(f);
    if (f.current) CHECK(s == PackState::kCurrent);                                  // current wins over everything (a build of this session that ran before did its job)
    else if (f.building) CHECK(s == PackState::kBuilding);                           // a build that runs is waited for
    else if (f.failed) CHECK(s == PackState::kFailed);                               // a failure is final for the session
    else if (!f.source_exists) CHECK(s == PackState::kNoSource);                     // nothing to build from
    else CHECK(s == (f.pack_exists ? PackState::kStale : PackState::kMissing));      // a first start, or built from other files
  }
  // the first start of a player the launcher did not set up: no pack, the game archive there -> missing -> the renderer's request starts a build
  Facts first;
  first.source_exists = true;
  CHECK(StateFor(first) == PackState::kMissing);
  CHECK(ShouldStartBackgroundBuild(StateFor(first), false));
  // asked again while it runs (the renderer asks only once, but the answer is safe): no second build
  CHECK(!ShouldStartBackgroundBuild(StateFor(first), true));
  first.building = true;
  CHECK(StateFor(first) == PackState::kBuilding && !ShouldStartBackgroundBuild(StateFor(first), false));
  // a game update changed the archive: stale -> built again
  Facts update;
  update.source_exists = update.pack_exists = true;
  CHECK(StateFor(update) == PackState::kStale && ShouldStartBackgroundBuild(StateFor(update), false));
  // a failed build is not started again in the session, and a missing archive never starts one
  Facts failed = update;
  failed.failed = true;
  CHECK(!ShouldStartBackgroundBuild(StateFor(failed), false));
  Facts none;
  CHECK(StateFor(none) == PackState::kNoSource && !ShouldStartBackgroundBuild(StateFor(none), false));
}

void TestWorkers() {
  CHECK(BackgroundWorkers(0) == 1 && BackgroundWorkers(1) == 1 && BackgroundWorkers(2) == 1 && BackgroundWorkers(4) == 1 && BackgroundWorkers(7) == 1);
  CHECK(BackgroundWorkers(8) == 2 && BackgroundWorkers(12) == 3 && BackgroundWorkers(16) == 4);
  CHECK(BackgroundWorkers(32) == 4 && BackgroundWorkers(128) == 4);  // never more than four
  for (uint32_t t = 0; t < 200; ++t) CHECK(BackgroundWorkers(t) >= 1 && BackgroundWorkers(t) <= 4 && BackgroundWorkers(t) * 4 <= (t < 4 ? 4u : t));  // a quarter at most, one at least
}

void TestWords() {
  CHECK(GroupThousands(0) == "0" && GroupThousands(7) == "7" && GroupThousands(999) == "999" && GroupThousands(1000) == "1,000");
  CHECK(GroupThousands(22236322) == "22,236,322" && GroupThousands(382485) == "382,485" && GroupThousands(1234567890123ull) == "1,234,567,890,123");
  CHECK(MebiBytes(248ull * 1024 * 1024) == "248 MiB" && MebiBytes(1024 * 512) == "0.5 MiB" && MebiBytes(9ull * 1024 * 1024) == "9.0 MiB");
  CHECK(BuiltText(439, 22236322, 382485, 248ull * 1024 * 1024, 3.44) == "built 439 sectors (22,236,322 detail and 382,485 far triangles), 248 MiB, in 3.4 s");
}

void TestLines() {
  // every status has its own word, and the exit code follows what a launcher needs: 0 when the pack is in place or not wanted
  CHECK(StatusName(Status::kOk) == "ok" && StatusName(Status::kUpToDate) == "up to date" && StatusName(Status::kNotNeeded) == "not needed");
  CHECK(StatusName(Status::kCancelled) == "cancelled" && StatusName(Status::kFailed) == "failed" && StatusName(Status::kNoArchive) == "no archive");
  CHECK(ExitCodeFor(Status::kOk) == 0 && ExitCodeFor(Status::kUpToDate) == 0 && ExitCodeFor(Status::kNotNeeded) == 0);
  CHECK(ExitCodeFor(Status::kCancelled) == 1 && ExitCodeFor(Status::kFailed) == 1 && ExitCodeFor(Status::kNoArchive) == 1);
  // the hit data's two more answers (CITY-HIT-ACTION): the hit data goes over a city pack that is not there; the stat check found it missing or stale. Both are exit 1.
  CHECK(StatusName(Status::kNoPack) == "no pack" && StatusName(Status::kNotCurrent) == "not current");
  CHECK(ExitCodeFor(Status::kNoPack) == 1 && ExitCodeFor(Status::kNotCurrent) == 1);
  CHECK(LineResult(Status::kNoPack, "x") == "@citypack|result|no pack|x" && LineResult(Status::kNotCurrent, "why") == "@citypack|result|not current|why");
  // every status has a distinct word (a launcher switches on it), and only the three good ones exit 0
  {
    int good = 0;
    std::string words;
    for (const Status status : {Status::kOk, Status::kUpToDate, Status::kNotNeeded, Status::kCancelled, Status::kFailed, Status::kNoArchive, Status::kNoPack, Status::kNotCurrent}) {
      good += ExitCodeFor(status) == 0 ? 1 : 0;
      const std::string word = "|" + std::string(StatusName(status)) + "|";
      CHECK(words.find(word) == std::string::npos);
      words += word;
    }
    CHECK(good == 3);
  }

  CHECK(LineFolder("C:\\Games\\MCLA\\cache\\shaders\\city") == "@citypack|folder|C:\\Games\\MCLA\\cache\\shaders\\city");
  CHECK(LinePhase(439, "city sectors") == "@citypack|phase|439|city sectors" && LinePhase(0, "x") == "@citypack|phase|0|x");
  CHECK(LineStep(40, 439, "hw_ss_07") == "@citypack|step|40|439|hw_ss_07");
  CHECK(LineLog(0, "a") == "@citypack|log|info|a" && LineLog(1, "a") == "@citypack|log|warn|a" && LineLog(2, "a") == "@citypack|log|error|a");
  CHECK(LineResult(Status::kOk, "built") == "@citypack|result|ok|built" && LineResult(Status::kUpToDate, "x") == "@citypack|result|up to date|x");
  // free text cannot break the line or the field grammar: no newline, no tab, no separator; long text is cut
  const std::string wild = LineLog(2, "a|b\nc\td\r");
  CHECK(wild == "@citypack|log|error|a b c d " && wild.find('\n') == std::string::npos);
  CHECK(Field(std::string(2000, 'x')).size() == 600 && Field(std::string(2000, 'x')).substr(597) == "...");
  CHECK(LineFolder(std::string(3000, 'p')).size() == std::string("@citypack|folder|").size() + 1000);
  // a launcher splits on the first fields only: the result's free text keeps everything after the second separator in ONE field
  const std::string line = LineResult(Status::kFailed, "no | pipe here");
  size_t bars = 0;
  for (const char c : line) bars += c == '|';
  CHECK(bars == 3);
}

// Which of the command line's actions run, and in which order (PlanActions: main.cpp reads the five flags from the arguments, the app runs the plan).
void TestActionPlan() {
  // nothing asked: no batch mode, the game starts
  CHECK(!PlanActions(ActionFlags{}).any());
  // each alone is its own mode and nothing follows
  for (int which = 0; which < 5; ++which) {
    ActionFlags f;
    f.shader_packs = which == 0;
    f.precache = which == 1;
    f.city_pack = which == 2;
    f.city_hit = which == 3;
    f.check_city_hit = which == 4;
    const ActionPlan plan = PlanActions(f);
    const ActionKind want[] = {ActionKind::kBuildShaderPacks, ActionKind::kPrecachePipelines, ActionKind::kBuildCityPack, ActionKind::kBuildCityHit, ActionKind::kCheckCityHit};
    CHECK(plan.first == want[which] && plan.any() && !plan.then_city_pack && !plan.then_city_hit && !plan.then_check_city_hit);
  }
  // the launcher's setup: the shader packs, then the pack, the hit data and its check on one command line: the first is the packs, the rest follow in the order the files depend on each other
  {
    ActionFlags f;
    f.shader_packs = f.city_pack = f.city_hit = f.check_city_hit = true;
    const ActionPlan plan = PlanActions(f);
    CHECK(plan.first == ActionKind::kBuildShaderPacks && plan.then_city_pack && plan.then_city_hit && plan.then_check_city_hit);
  }
  // the pack and the hit data together: the pack first, the hit data after it; the hit data with its check: the build first, then the check
  {
    ActionFlags f;
    f.city_pack = f.city_hit = true;
    const ActionPlan plan = PlanActions(f);
    CHECK(plan.first == ActionKind::kBuildCityPack && !plan.then_city_pack && plan.then_city_hit && !plan.then_check_city_hit);
    f.city_pack = false;
    f.check_city_hit = true;
    const ActionPlan build_check = PlanActions(f);
    CHECK(build_check.first == ActionKind::kBuildCityHit && !build_check.then_city_hit && build_check.then_check_city_hit && !build_check.then_city_pack);
  }
  // precache with the city actions: the pipelines first (as the pack always followed them)
  {
    ActionFlags f;
    f.precache = f.city_hit = true;
    const ActionPlan plan = PlanActions(f);
    CHECK(plan.first == ActionKind::kPrecachePipelines && !plan.then_city_pack && plan.then_city_hit);
  }
  // both of the first two: the first only, as before (the second is not run, nor is it a "then")
  {
    ActionFlags f;
    f.shader_packs = f.precache = true;
    const ActionPlan plan = PlanActions(f);
    CHECK(plan.first == ActionKind::kBuildShaderPacks && !plan.then_city_pack && !plan.then_city_hit && !plan.then_check_city_hit);
  }
  // every combination of the five: a city action is the first or a follower, never both, never lost; the first is the earliest asked
  for (int bits = 1; bits < 32; ++bits) {
    ActionFlags f;
    f.shader_packs = (bits & 1) != 0;
    f.precache = (bits & 2) != 0;
    f.city_pack = (bits & 4) != 0;
    f.city_hit = (bits & 8) != 0;
    f.check_city_hit = (bits & 16) != 0;
    const ActionPlan plan = PlanActions(f);
    CHECK(plan.any());
    CHECK(f.city_pack == (plan.first == ActionKind::kBuildCityPack || plan.then_city_pack) && !(plan.first == ActionKind::kBuildCityPack && plan.then_city_pack));
    CHECK(f.city_hit == (plan.first == ActionKind::kBuildCityHit || plan.then_city_hit) && !(plan.first == ActionKind::kBuildCityHit && plan.then_city_hit));
    CHECK(f.check_city_hit == (plan.first == ActionKind::kCheckCityHit || plan.then_check_city_hit) && !(plan.first == ActionKind::kCheckCityHit && plan.then_check_city_hit));
    const ActionKind earliest = f.shader_packs     ? ActionKind::kBuildShaderPacks
                                : f.precache       ? ActionKind::kPrecachePipelines
                                : f.city_pack      ? ActionKind::kBuildCityPack
                                : f.city_hit       ? ActionKind::kBuildCityHit
                                                   : ActionKind::kCheckCityHit;
    CHECK(plan.first == earliest);
  }
}

// The hit data's states for the renderer's probe and the background build: the same facts and answers as the pack's (Facts, StateFor), with "exists" meaning the hit file or its stamp is there and
// "current" the stamp check (CheckHitData: the stamp, the sizes, the city pack beside it). The cases a first start, an update, a deleted texture file and a failed build come to.
void TestHitStates() {
  // a first start with no launcher setup: the archive is there, nothing else -> missing -> the probe's request starts the build, once
  Facts first;
  first.source_exists = true;
  CHECK(StateFor(first) == PackState::kMissing && ShouldStartBackgroundBuild(StateFor(first), false) && !ShouldStartBackgroundBuild(StateFor(first), true));
  // the launcher's setup built it: current, whatever else is true, nothing to build
  Facts built = first;
  built.pack_exists = built.current = true;
  CHECK(StateFor(built) == PackState::kCurrent && !ShouldStartBackgroundBuild(StateFor(built), false));
  // a game update, a changed mod, another builder version, or a deleted texture file (the stamp holds the texture file's size): the files are there and the stamp says no -> stale -> built again
  Facts stale = first;
  stale.pack_exists = true;
  CHECK(StateFor(stale) == PackState::kStale && ShouldStartBackgroundBuild(StateFor(stale), false));
  // while it builds the renderer is told "being built" (it looks again in 2 s); a failure is final for the session; no archive, nothing to build from
  Facts running = stale;
  running.building = true;
  CHECK(StateFor(running) == PackState::kBuilding && !ShouldStartBackgroundBuild(StateFor(running), true));
  Facts failed = stale;
  failed.failed = true;
  CHECK(StateFor(failed) == PackState::kFailed && !ShouldStartBackgroundBuild(StateFor(failed), true));
  Facts none;
  CHECK(StateFor(none) == PackState::kNoSource && !ShouldStartBackgroundBuild(StateFor(none), false));
}

void TestHitWords() {
  CHECK(HitBuiltText(439, 85422, 6805, 8342, 110ull * 1024 * 1024, 138ull * 1024 * 1024, 12.34) ==
        "built the hit data: 439 sectors, 85,422 runs, 6,805 materials, 8,342 pictures; hit file 110 MiB, texture file 138 MiB, in 12.3 s");
  // a hit file built without textures names no texture file
  CHECK(HitBuiltText(3, 12, 8, 0, 4096, 0, 0.05) == "built the hit data: 3 sectors, 12 runs, 8 materials, 0 pictures; hit file 0.0 MiB, in 0.1 s");
  CHECK(HitCurrentText("C:/cache/city").rfind("The city hit data in C:/cache/city is up to date", 0) == 0);
  // the deep check: whole, whole without textures, and the counts of the problems
  CHECK(DeepCheckText(0, 0, 0, 8342, false, 7.04) == "The city data is whole: the city pack, the hit file and 8,342 pictures of the texture file read back with no problems, in 7.0 s.");
  CHECK(DeepCheckText(0, 0, 0, 0, true, 0.2).find("(it has no textures)") != std::string::npos);
  const std::string bad = DeepCheckText(0, 2, 1, 8342, false, 3.0);
  CHECK(bad.find("0 in the city pack, 2 in the hit file, 1 in the texture file") != std::string::npos && bad.find("build_city_hit_force=true") != std::string::npos);
  // the lines a launcher reads, for the hit data's results (the same grammar as the pack's: three separators, the free text last)
  for (const Status status : {Status::kOk, Status::kUpToDate, Status::kNoPack, Status::kNotCurrent, Status::kFailed}) {
    const std::string line = LineResult(status, HitCurrentText("D:/x | y"));
    size_t bars = 0;
    for (const char c : line) bars += c == '|';
    CHECK(bars == 3 && line.rfind("@citypack|result|", 0) == 0 && line.find('\n') == std::string::npos);
  }
}

}  // namespace

int main() {
  TestState();
  TestWorkers();
  TestWords();
  TestLines();
  TestActionPlan();
  TestHitStates();
  TestHitWords();
  if (g_failed != 0) {
    std::printf("city_pack_rules_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("city_pack_rules_test: %d checks passed\n", g_checks);
  return 0;
}
