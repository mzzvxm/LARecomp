// Check of the render thread's wait for a guest pipeline a worker is making (pipeline_wait.h): when it hands
// the worker its priority, when it makes the pipeline itself, and that it never does either too early. A
// program of its own: it needs no renderer, no ReXGlue, no driver and no threads, and exits 0 when every
// check holds.
//
//   cmake --build <build dir> --target larecomp_pipeline_wait_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_pipeline_wait_test

#include <cstdint>
#include <cstdio>
#include <string>

#include "pipeline_wait.h"

using namespace mcla::gpu::pipeline_wait;

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

Facts At(uint32_t ms, bool raised = false, bool known = true, bool may = true) {
  Facts f;
  f.waited_ms = ms;
  f.raised = raised;
  f.builder_known = known;
  f.may_make_here = may;
  return f;
}

void TestOrdinaryCompileIsLeftAlone() {
  // A guest pipeline costs 20 to 150 ms cold: nothing is raised or doubled for a wait that short.
  for (uint32_t ms = 0; ms < kRaiseAfterMs; ms += kSliceMs) {
    Check(Next(At(ms)) == Step::kKeepWaiting, "a wait of " + std::to_string(ms) + " ms just waits");
  }
  Check(kRaiseAfterMs > 20, "the raise is later than the shortest ordinary compile");
}

void TestRaiseThenMakeHere() {
  Check(Next(At(kRaiseAfterMs)) == Step::kRaise, "at kRaiseAfterMs the worker is raised");
  Check(Next(At(kRaiseAfterMs, true)) == Step::kKeepWaiting, "raised once: it waits on, no second raise");
  Check(Next(At(kMakeHereAfterMs - 1, true)) == Step::kKeepWaiting, "just before kMakeHereAfterMs it still waits");
  Check(Next(At(kMakeHereAfterMs, true)) == Step::kMakeHere, "at kMakeHereAfterMs the render thread makes it too");
  Check(kRaiseAfterMs < kMakeHereAfterMs, "the raise comes before the second compile");
  Check(kMakeHereAfterMs > 157, "a slow ordinary build (157 ms on the render thread in the 11-22 runs) is not doubled");
}

void TestTheFiveSecondWait() {
  // vkrt1-race-vulkan: one wait of 5363.8 ms. Walk it in slices as WaitForBuild does, with a worker that
  // stays starved until it is raised and then needs 100 ms: the wait ends at once after the raise, far under
  // the old 5.4 s, whichever of the two steps does it.
  uint32_t waited = 0;
  bool raised = false;
  bool made_here = false;
  uint32_t raised_at = 0;
  while (waited < 5364 && !made_here) {
    const Step step = Next(At(waited, raised));
    if (step == Step::kRaise) {
      raised = true;
      raised_at = waited;
    } else if (step == Step::kMakeHere) {
      made_here = true;
      break;
    }
    if (raised && waited >= raised_at + 100) {
      break;  // the raised worker finished its compile
    }
    waited += kSliceMs;
  }
  Check(waited < 200, "a starved worker's pipeline is in hand within 200 ms (waited " + std::to_string(waited) + ")");
  // And a worker that cannot be raised (Linux, one-way lowering) or a driver that is slow: the render thread
  // makes it itself after kMakeHereAfterMs, never after seconds.
  uint32_t t = 0;
  while (Next(At(t, true)) != Step::kMakeHere && t < 10000) {
    t += kSliceMs;
  }
  Check(t >= kMakeHereAfterMs && t < kMakeHereAfterMs + kSliceMs, "a wait that cannot be raised ends at kMakeHereAfterMs");
}

void TestUnknownBuilder() {
  // The worker says which one it is a moment after the state reads "building": no raise before then, but the
  // second compile does not depend on it.
  Check(Next(At(kRaiseAfterMs + 100, false, false)) == Step::kKeepWaiting, "no raise while the worker is unknown");
  Check(Next(At(kRaiseAfterMs + 100, false, true)) == Step::kRaise, "the raise comes once the worker is known");
  Check(Next(At(kMakeHereAfterMs, false, false)) == Step::kMakeHere, "the render thread makes it too whether or not the worker is known");
}

void TestCannotMakeHere() {
  Check(Next(At(kMakeHereAfterMs + 1000, true, true, false)) == Step::kKeepWaiting,
        "a caller that cannot make the pipeline keeps waiting (the raise still happens)");
  Check(Next(At(kRaiseAfterMs, false, true, false)) == Step::kRaise, "and raises the worker");
}

void TestOrderOfSteps() {
  // Over a whole wait the steps come in order and each at most once.
  uint32_t raises = 0;
  uint32_t makes = 0;
  bool raised = false;
  bool made_before_raise = false;
  for (uint32_t ms = 0; ms <= 2000; ms += kSliceMs) {
    const Step step = Next(At(ms, raised));
    if (step == Step::kRaise) {
      ++raises;
      raised = true;
    }
    if (step == Step::kMakeHere) {
      ++makes;
      made_before_raise = made_before_raise || !raised;
      break;
    }
  }
  Check(raises == 1, "one raise per wait");
  Check(makes == 1, "one make-here per wait");
  Check(!made_before_raise, "the raise comes before the make-here");
}

}  // namespace

int main() {
  TestOrdinaryCompileIsLeftAlone();
  TestRaiseThenMakeHere();
  TestTheFiveSecondWait();
  TestUnknownBuilder();
  TestCannotMakeHere();
  TestOrderOfSteps();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  std::printf(g_failed == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
  return g_failed == 0 ? 0 : 1;
}
