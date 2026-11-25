// Check of the host MSAA level's rules (msaa_plan.h): what mcla_gpu_msaa says, and when the renderer makes a change of it while
// the game runs (MsaaLevel::BeginFrame asks the Planner once a frame at the boundary and does the work it decides). A program
// of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds. The time is the test's: a made-up
// clock that is moved by hand, so the settle, the pipelines' wait and the retries are checked to the millisecond.
//
//   cmake --build <build dir> --target larecomp_msaa_plan_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_msaa_plan_test
//
// The expected values are written out here again on purpose: a change of a time or of a step in the header alone fails here.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "msaa_plan.h"

using namespace mcla::gpu::msaa_plan;
using std::chrono::milliseconds;
using std::chrono::seconds;

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

void CheckN(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got, (unsigned long long)want);
  }
}

const Clock::time_point kT0 = Clock::time_point{} + seconds(1000);

// What one Plan call did, from the outside: the calls it made and what it said.
struct Run {
  Decision d;
  int staged = 0;   // calls of `stage`
  int waited = 0;   // calls of `waiting`
};

// The frame at `t` (milliseconds after kT0): wanted level, the level in force and the flags.
Frame At(int64_t t_ms, uint32_t wanted, uint32_t level, bool upscaling = false, bool settled = true, bool moved = false) {
  Frame f;
  f.wanted = wanted;
  f.level = level;
  f.upscaling = upscaling;
  f.settled = settled;
  f.moved = moved;
  f.now = kT0 + milliseconds(t_ms);
  return f;
}

Run Step(Planner& p, const Frame& f, uint32_t waiting_pipelines = 0) {
  Run r;
  r.d = p.Plan(f, [&] { ++r.staged; }, [&] { ++r.waited; return waiting_pipelines; });
  return r;
}

void TheText() {
  struct Case {
    const char* text;
    uint32_t level;
    bool off;
    bool invalid;
  };
  const Case cases[] = {
      {"off", 1, true, false},  {"OFF", 1, true, false}, {" Off ", 1, true, false}, {"1", 1, true, false},
      {"2", 2, false, false},   {"2x", 2, false, false}, {"4", 4, false, false},    {"4x", 4, false, false},
      {"4X", 4, false, false},  {" 4 x ", 4, false, false}, {"8", 8, false, false}, {"8x", 8, false, false},
      {"", 2, false, true},     {"x", 2, false, true},   {"3", 2, false, true},     {"16", 2, false, true},
      {"on", 2, false, true},   {"0", 2, false, true},   {"xx", 2, false, true},
  };
  for (const Case& c : cases) {
    const Parsed p = ParseText(c.text);
    const std::string at = std::string("'") + c.text + "'";
    CheckN(p.level, c.level, "the level of " + at);
    Check(p.off == c.off, "off of " + at);
    Check(p.invalid == c.invalid, "invalid of " + at + ": the console's 2 is used and the log says so");
  }
}

void TheDevicesLimit() {
  const uint32_t all = 2 | 4 | 8;
  CheckN(LevelWithin(all, 8), 8, "8x on a device that does 2, 4 and 8");
  CheckN(LevelWithin(all, 4), 4, "4x");
  CheckN(LevelWithin(2 | 4, 8), 4, "8x on a device with no 8: 4x");
  CheckN(LevelWithin(2, 8), 2, "8x on a device with 2 only: 2x");
  CheckN(LevelWithin(8, 4), 2, "4x with no 4 (8 alone does not help): the next lower, 2x");
  CheckN(LevelWithin(0, 8), 2, "nothing reported: the game's own 2x, never 0");
  CheckN(LevelWithin(0, 2), 2, "2x is kept whatever the mask says");
  CheckN(LevelWithin(0, 1), 1, "1x always works");
  CheckN(LevelWithin(all, 1), 1, "1x stays 1x on any device");
}

void TheUpscalerTakesTheLevel() {
  static_assert(UpscalerTakesLevel(true, false), "an upscaler that runs takes the level");
  static_assert(!UpscalerTakesLevel(true, true), "unless the debug switch keeps the player's level under it");
  static_assert(!UpscalerTakesLevel(false, false), "no upscaler: the setting's level");
  static_assert(!UpscalerTakesLevel(false, true), "no upscaler, the switch on: the setting's level");
  Check(true, "the upscaler rule is checked at compile time");
}

void NothingToDo() {
  Planner p;
  const Run r = Step(p, At(0, 2, 2));
  Check(r.d.verdict == Verdict::kRest, "the level in force is the one wanted: rest");
  CheckN(r.staged + r.waited, 0, "nothing is asked of the pipelines");
  Check(!p.switching(), "not switching");
  // Many frames at rest stay at rest, with no state growing.
  for (int i = 1; i < 100; ++i) {
    Check(Step(p, At(i * 16, 4, 4)).d.verdict == Verdict::kRest, "rest at frame " + std::to_string(i));
  }
}

void ARowStep() {
  // The row goes from 2X to 4X at t = 0 on a settled path: nothing for kSettle, then the pipelines are asked for once, then the
  // level is changed when they are ready, at one boundary, and the planner rests.
  Planner p;
  Run r = Step(p, At(0, 4, 2));
  Check(r.d.verdict == Verdict::kWaiting, "the setting just changed: wait for it to stand still");
  CheckN(r.staged, 0, "... nothing asked of the pipelines yet");
  Check(!p.switching(), "... and the row does not say (APPLYING) yet");
  r = Step(p, At(399, 4, 2));
  Check(r.d.verdict == Verdict::kWaiting && r.staged == 0, "399 ms: still waiting");
  r = Step(p, At(400, 4, 2), /*waiting*/ 7);
  CheckN(r.staged, 1, "400 ms: the pipelines are asked for");
  Check(r.d.staged_now, "... and the decision says so");
  Check(r.d.verdict == Verdict::kWaitingPipelines && r.d.waiting == 7, "seven are not made yet: the old level goes on");
  Check(p.switching(), "the row says (APPLYING) from here");
  r = Step(p, At(416, 4, 2), 3);
  CheckN(r.staged, 0, "the next frame does not ask again");
  Check(r.d.verdict == Verdict::kWaitingPipelines && r.d.waiting == 3, "three left");
  r = Step(p, At(432, 4, 2), 0);
  Check(r.d.verdict == Verdict::kApply && !r.d.timed_out && r.d.waiting == 0, "all made: change the level now");
  CheckN(r.staged, 0, "... without asking the pipelines again");
  Check(!p.Applied(kT0 + milliseconds(432), true), "the change was made: no failure");
  Check(!p.switching(), "the row stops saying (APPLYING)");
  // The next frame sees the new level in force.
  r = Step(p, At(448, 4, 4));
  Check(r.d.verdict == Verdict::kRest, "rest at the new level");
  // And a later change starts over from nothing.
  r = Step(p, At(10000, 8, 4));
  Check(r.d.verdict == Verdict::kWaiting, "a later change waits to stand still again");
}

void MakeBeforeBreak() {
  // The order is fixed: the pipelines are asked for (once) before anyone looks how many are left, and nobody looks while a
  // path change or a remake of the targets holds the change.
  Planner p;
  std::vector<char> calls;
  auto plan = [&](const Frame& f, uint32_t left) {
    return p.Plan(f, [&] { calls.push_back('S'); }, [&] { calls.push_back('W'); return left; });
  };
  plan(At(0, 4, 2), 0);
  Decision d = plan(At(400, 4, 2, false, /*settled*/ false), 0);
  Check(d.verdict == Verdict::kHolding && d.staged_now, "a path change is waiting: the pipelines are asked for, the level is held");
  d = plan(At(416, 4, 2, false, true, /*moved*/ true), 0);
  Check(d.verdict == Verdict::kHolding && !d.staged_now, "the targets were remade this frame: held again, nothing asked twice");
  d = plan(At(432, 4, 2), 2);
  Check(d.verdict == Verdict::kWaitingPipelines, "free to change, two pipelines left");
  d = plan(At(448, 4, 2), 0);
  Check(d.verdict == Verdict::kApply, "ready");
  CheckN(calls.size(), 3, "three calls in all: one stage, two looks at the count");
  Check(calls == std::vector<char>({'S', 'W', 'W'}), "stage first, the count only once the change is not held");
}

void AStepThroughValues() {
  // The row is stepped 2X -> 4X -> 8X by a player holding the button: nothing is made for the 4X in between.
  Planner p;
  int staged = 0;
  auto plan = [&](const Frame& f) { return p.Plan(f, [&] { ++staged; }, [&] { return 0u; }); };
  Check(plan(At(0, 4, 2)).verdict == Verdict::kWaiting, "4X asked");
  Check(plan(At(200, 8, 2)).verdict == Verdict::kWaiting, "8X asked 200 ms later: the settle starts over");
  Check(plan(At(599, 8, 2)).verdict == Verdict::kWaiting, "599 ms: 399 ms after the last step, still waiting");
  CheckN(staged, 0, "nothing was asked for 4X, nor for 8X yet");
  const Decision d = plan(At(600, 8, 2));
  Check(d.verdict == Verdict::kApply && d.staged_now, "600 ms: 8X stood still for 400 ms, asked and made");
  CheckN(staged, 1, "one set of pipelines asked for, the 8X's");
}

void ABackAndForthBeforeItIsMade() {
  // 2X -> 4X and back to 2X within the settle: nothing was made, nothing is left over.
  Planner p;
  int staged = 0;
  auto plan = [&](const Frame& f) { return p.Plan(f, [&] { ++staged; }, [&] { return 0u; }); };
  Check(plan(At(0, 4, 2)).verdict == Verdict::kWaiting, "4X asked");
  Check(plan(At(100, 2, 2)).verdict == Verdict::kRest, "back to 2X: rest");
  Check(plan(At(450, 2, 2)).verdict == Verdict::kRest, "and nothing happens later");
  CheckN(staged, 0, "no pipelines were asked for");
  // A later 4X has its own full settle.
  Check(plan(At(500, 4, 2)).verdict == Verdict::kWaiting, "4X again");
  Check(plan(At(899, 4, 2)).verdict == Verdict::kWaiting, "399 ms after the new ask");
  Check(plan(At(900, 4, 2)).verdict == Verdict::kApply, "400 ms");
}

void TheUpscalersPathNeedsNoSettle() {
  // An upscaler that comes on (the level goes to 1): the path change settled already, so the level follows at once.
  {
    Planner p;
    int staged = 0;
    // The frame before: no upscaler, 4X in force and wanted.
    p.Plan(At(0, 4, 4), [&] { ++staged; }, [&] { return 0u; });
    const Decision d = p.Plan(At(16, 1, 4, /*upscaling*/ true), [&] { ++staged; }, [&] { return 0u; });
    Check(d.verdict == Verdict::kApply && d.staged_now, "the upscaler runs: level 1 with no wait");
    CheckN(staged, 1, "its pipelines asked for in the same frame");
    Check(!p.Applied(kT0 + milliseconds(16), true), "made");
    // The upscaler goes off: the player's level returns at once too.
    const Decision back = p.Plan(At(5000, 4, 1, /*upscaling*/ false), [&] { ++staged; }, [&] { return 0u; });
    Check(back.verdict == Verdict::kApply, "the upscaler stops: the player's level returns with no wait");
  }
  // The same wanted change with the path unchanged waits: a row stepped while an upscaler is on (greyed in the menu) or off.
  {
    Planner p;
    p.Plan(At(0, 2, 2, true), [] {}, [] { return 0u; });  // the first call sees `upscaling` change from the planner's start
    const Decision d = p.Plan(At(1000, 4, 2, true), [] {}, [] { return 0u; });
    Check(d.verdict == Verdict::kWaiting, "no change of path: the settle applies");
  }
  // While a path change or the render scale is being made the level follows it, whichever way it was asked.
  {
    Planner p;
    p.Plan(At(0, 4, 4), [] {}, [] { return 0u; });
    const Decision d = p.Plan(At(16, 1, 4, true, /*settled*/ false), [] {}, [] { return 0u; });
    Check(d.verdict == Verdict::kHolding && d.staged_now, "the path is still being made: the pipelines now, the level after it");
  }
}

void TheWaitIsBounded() {
  // Pipelines that never finish: the level changes after kMaxWait anyway, the rest are made as draws ask for them.
  Planner p;
  Run r = Step(p, At(0, 4, 2));
  r = Step(p, At(400, 4, 2), 5);
  Check(r.d.verdict == Verdict::kWaitingPipelines, "waiting for the pipelines");
  r = Step(p, At(400 + 19999, 4, 2), 5);
  Check(r.d.verdict == Verdict::kWaitingPipelines && !r.d.timed_out, "19.999 s after they were asked for: still waiting");
  r = Step(p, At(400 + 20000, 4, 2), 5);
  Check(r.d.verdict == Verdict::kApply && r.d.timed_out && r.d.waiting == 5, "20 s: the change is made, five still left");
  CheckN(uint64_t(kMaxWait.count()), 20, "the bound is twenty seconds");
}

void ARetryAfterAFailure() {
  Planner p;
  Step(p, At(0, 8, 2));
  Run r = Step(p, At(400, 8, 2));
  Check(r.d.verdict == Verdict::kApply, "ready at 400 ms");
  const Clock::time_point began = kT0 + milliseconds(400);
  Check(p.Applied(began, false), "the first failure is reported once");
  Check(!p.switching(), "a failure ends (APPLYING): the old level goes on");
  CheckN(uint64_t(p.retry_delay().count()), 3, "asked again after 3 s");
  r = Step(p, At(400 + 2999, 8, 2));
  Check(r.d.verdict == Verdict::kWaiting && r.staged == 0, "2.999 s after the failure: not yet");
  r = Step(p, At(400 + 3000, 8, 2));
  Check(r.d.verdict == Verdict::kApply, "3 s: the change is tried again");
  CheckN(r.staged, 0, "... without asking the pipelines again (they were asked for once)");
  Check(p.switching(), "(APPLYING) again while it is tried");
  // Each failure doubles the wait, up to 30 s, and says nothing more.
  int64_t t = 400 + 3000;
  const int64_t want_seconds[] = {6, 12, 24, 30, 30};
  for (int64_t secs : want_seconds) {
    Check(!p.Applied(kT0 + milliseconds(t), false), "a later failure is not reported again");
    CheckN(uint64_t(p.retry_delay().count()), uint64_t(secs), "the next wait is " + std::to_string(secs) + " s");
    r = Step(p, At(t + secs * 1000 - 1, 8, 2));
    Check(r.d.verdict == Verdict::kWaiting, "waiting out the " + std::to_string(secs) + " s");
    t += secs * 1000;
    r = Step(p, At(t, 8, 2));
    Check(r.d.verdict == Verdict::kApply, "tried again after " + std::to_string(secs) + " s");
  }
  // At last it fits (video memory came free): the plan rests and the next change starts from scratch.
  Check(!p.Applied(kT0 + milliseconds(t), true), "made at last");
  Check(Step(p, At(t + 16, 8, 8)).d.verdict == Verdict::kRest, "rest at 8X");
  r = Step(p, At(t + 10000, 4, 8));
  Check(r.d.verdict == Verdict::kWaiting, "a new change waits to stand still");
  r = Step(p, At(t + 10400, 4, 8));
  Check(r.d.verdict == Verdict::kApply && r.staged == 1, "and asks its pipelines once");
  Check(p.Applied(kT0 + milliseconds(t + 10400), false), "its first failure is reported: the earlier failures were forgotten");
  CheckN(uint64_t(p.retry_delay().count()), 3, "and starts again at 3 s");
}

void AnotherLevelDuringARetry() {
  // 8X could not be made; the player picks 4X: that is asked at once (after its own settle), not after the 8X's retry delay.
  Planner p;
  Step(p, At(0, 8, 2));
  Step(p, At(400, 8, 2));
  p.Applied(kT0 + milliseconds(400), false);  // retry due at 3.4 s
  Run r = Step(p, At(1000, 4, 2));
  Check(r.d.verdict == Verdict::kWaiting && r.staged == 0, "4X asked: its settle starts");
  r = Step(p, At(1400, 4, 2));
  Check(r.d.verdict == Verdict::kApply && r.staged == 1, "400 ms later: 4X is staged and tried, well before the 8X's retry at 3.4 s");
  // The player goes back to the level in force instead: everything is forgotten, no retry is left over.
  Planner q;
  Step(q, At(0, 8, 2));
  Step(q, At(400, 8, 2));
  q.Applied(kT0 + milliseconds(400), false);
  Check(Step(q, At(1000, 2, 2)).d.verdict == Verdict::kRest, "back to the level in force: rest");
  Check(q.retry_after() == Clock::time_point{}, "no retry is left over");
  Step(q, At(5000, 8, 2));
  r = Step(q, At(5400, 8, 2));
  Check(r.d.verdict == Verdict::kApply && r.staged == 1, "a later 8X is a new change with its own settle and its own pipelines");
}

void TheTimesAreTheOnesTheHeaderDocuments() {
  CheckN(uint64_t(kSettle.count()), 400, "settle 400 ms");
  CheckN(uint64_t(kRetry.count()), 3, "retry 3 s");
  CheckN(uint64_t(kRetryMax.count()), 30, "retry cap 30 s");
}

}  // namespace

int main() {
  TheText();
  TheDevicesLimit();
  TheUpscalerTakesTheLevel();
  NothingToDo();
  ARowStep();
  MakeBeforeBreak();
  AStepThroughValues();
  ABackAndForthBeforeItIsMade();
  TheUpscalersPathNeedsNoSettle();
  TheWaitIsBounded();
  ARetryAfterAFailure();
  AnotherLevelDuringARetry();
  TheTimesAreTheOnesTheHeaderDocuments();
  if (g_failed != 0) {
    std::printf("msaa_plan_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("msaa_plan_test: %d checks passed\n", g_checks);
  return 0;
}
