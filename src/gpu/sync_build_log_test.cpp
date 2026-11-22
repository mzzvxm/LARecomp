// Check of the render thread's sync build lines (sync_build_log.h): the rate limit, the counts and the
// wording that perf/scripts/stall_check.py reads. A program of its own: it needs no renderer, no ReXGlue
// and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_sync_build_log_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_sync_build_log_test

#include <cstdint>
#include <cstdio>
#include <string>

#include "sync_build_log.h"

using namespace mcla::gpu::sync_build_log;

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

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void TestRateLimit() {
  Check(!LogThis(0), "event 0 does not exist");
  bool first_all = true;
  for (uint64_t n = 1; n <= kLoggedFirst; ++n) first_all = first_all && LogThis(n);
  Check(first_all, "the first kLoggedFirst events each get a line");
  // After the first ones only every kLoggedEvery-th: the numbers in between are silent.
  uint64_t logged = 0;
  uint64_t last = 0;
  bool every = true;
  for (uint64_t n = kLoggedFirst + 1; n <= 1000; ++n) {
    if (LogThis(n)) {
      ++logged;
      every = every && n % kLoggedEvery == 0;
      last = n;
    }
  }
  Check(every, "after the first ones only multiples of kLoggedEvery");
  Check(logged > 0 && last > 900, "the tail of a long run still gets lines");
  // 276 builds at the first night frames (NIGHT-STALL): a few dozen lines, not 276.
  uint64_t night = 0;
  for (uint64_t n = 1; n <= 276; ++n) night += LogThis(n) ? 1 : 0;
  Check(night >= kLoggedFirst && night < 60, "276 builds write fewer than 60 lines");
  // The count of a run is the largest #N the lines name: the last logged number is within kLoggedEvery of the total.
  Check(1000 - last < kLoggedEvery, "the largest #N is within kLoggedEvery of the real count");
}

void TestCounters() {
  Counters c;
  Check(c.NoteBuild(40'000'000) == 1, "the first build is #1");
  Check(c.NoteBuild(10'000'000) == 2, "the second build is #2");
  Check(c.NoteWait(7'000'000) == 1, "waits are counted on their own: #1");
  Check(c.builds == 2 && c.waits == 1, "two builds, one wait");
  Check(c.build_ns == 50'000'000 && c.wait_ns == 7'000'000, "the times add up");
  Check(c.longest_ns == 40'000'000, "the longest of both kinds");
}

KeyWords NightKey() {
  KeyWords k;
  k.vs = 0x0123456789ABCDEFull;
  k.ps = 0xFEDCBA9876543210ull;
  k.vs_variant = 0;
  k.ps_variant = 0x2A;
  k.color_count = 4;
  k.sample_count = 2;
  k.stencil = true;
  return k;
}

void TestBuildLine() {
  const std::string line = FormatBuild(12, 3456, 41'200'000, Why::kNew, NightKey(), false);
  Check(line == "[pipeline] sync build #12: guest frame 3456, 41.2 ms, new, vs 0123456789ABCDEF/0 ps "
                "FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil, background build refused",
        "the build line has the documented shape: " + line);
  KeyWords plain = NightKey();
  plain.stencil = false;
  const std::string allowed = FormatBuild(1, 1, 0, Why::kClaimed, plain, true);
  Check(Has(allowed, "0.0 ms, claimed,") && Has(allowed, "no stencil") && Has(allowed, "background build allowed"),
        "claimed, no stencil, allowed: " + allowed);
  Check(Has(FormatBuild(2, 1, 1'000'000, Why::kAgain, plain, true), "1.0 ms, again,"), "again");
  Check(WhyName(Why::kNew) == std::string("new") && WhyName(Why::kClaimed) == std::string("claimed") &&
            WhyName(Why::kAgain) == std::string("again"),
        "the three reasons keep their words (stall_check.py reads them)");
}

void TestWaitLine() {
  const std::string line = FormatWait(3, 3460, 8'700'000, "list", NightKey());
  Check(line == "[pipeline] sync wait #3: guest frame 3460, 8.7 ms, list build, vs 0123456789ABCDEF/0 ps "
                "FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil",
        "the wait line has the documented shape: " + line);
}

void TestSessionLine() {
  Counters c;
  c.NoteBuild(30'000'000);
  c.NoteBuild(12'500'000);
  c.NoteWait(5'000'000);
  const std::string line = FormatSession(c);
  Check(line == "[pipeline] sync builds this session: 2 build(s) 42.5 ms, 1 wait(s) 5.0 ms, longest 30.0 ms",
        "the session line: " + line);
  Check(Has(FormatSession(Counters{}), "0 build(s) 0.0 ms, 0 wait(s) 0.0 ms"), "an empty session still reads");
}

// The words the health checks read as an error must not be in any of the lines: a sync build is information.
void TestNoErrorWords() {
  const std::string all = FormatBuild(1, 1, 1, Why::kNew, NightKey(), true) + FormatBuild(1, 1, 1, Why::kClaimed, NightKey(), false) +
                          FormatBuild(1, 1, 1, Why::kAgain, NightKey(), false) + FormatWait(1, 1, 1, "list", NightKey()) +
                          FormatWait(1, 1, 1, "request", NightKey()) + FormatSession(Counters{});
  for (const char* word : {"fail", "error", "crash", "stalled", "validation", "Watchdog"}) {
    Check(!Has(all, word), std::string("no \"") + word + "\" in any line");
  }
}

}  // namespace

int main() {
  TestRateLimit();
  TestCounters();
  TestBuildLine();
  TestWaitLine();
  TestSessionLine();
  TestNoErrorWords();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  std::printf(g_failed == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
  return g_failed == 0 ? 0 : 1;
}
