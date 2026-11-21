// A check that needs no game: the city pack's start-up rules (bootprogress/city_pack_rules.h): which state the renderer is told, when the game builds the pack in the background and
// how hard, the words of the log and the command line's @citypack lines. Build: cmake --build <build dir> --target larecomp_city_pack_rules_test, then run it (exit 0 = passed);
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

}  // namespace

int main() {
  TestState();
  TestWorkers();
  TestWords();
  TestLines();
  if (g_failed != 0) {
    std::printf("city_pack_rules_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("city_pack_rules_test: %d checks passed\n", g_checks);
  return 0;
}
