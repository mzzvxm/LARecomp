// Unit test of the fence poll's yield rule (hooks/fence_spin.h).
//   clang++ -std=c++20 -I src/mc_engine/hooks src/mc_engine/hooks/fence_spin_test.cpp -o fence_spin_test && ./fence_spin_test
// Exit code 0 = passed.
#include "fence_spin.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

}  // namespace

int main() {
  using mcla::hooks::FencePollYieldDue;
  using mcla::hooks::kFencePollsPerYield;

  Check(kFencePollsPerYield == 16, "the Windows hook yielded on every 16th poll, and so does Linux");

  // The first 15 polls only pause; the 16th yields and the count starts again.
  uint32_t count = 0;
  bool early_yield = false;
  for (uint32_t i = 1; i < kFencePollsPerYield; ++i) early_yield = early_yield || FencePollYieldDue(count);
  Check(!early_yield, "polls 1 to 15 do not yield");
  Check(count == kFencePollsPerYield - 1, "15 polls leave the count at 15");
  Check(FencePollYieldDue(count), "poll 16 yields");
  Check(count == 0, "the count starts again after a yield");

  // Over many polls the yields are exactly every 16th: 1600 polls, 100 yields, at polls 16, 32, ...
  count = 0;
  uint32_t yields = 0;
  bool every_sixteenth = true;
  for (uint32_t poll = 1; poll <= 1600; ++poll) {
    const bool due = FencePollYieldDue(count);
    if (due) ++yields;
    if (due != (poll % 16 == 0)) every_sixteenth = false;
  }
  Check(yields == 100, "1600 polls yield 100 times");
  Check(every_sixteenth, "the yields fall on polls 16, 32, 48, ...");

  // Each thread has its own count: two counts do not disturb each other.
  uint32_t a = 0;
  uint32_t b = 0;
  for (int i = 0; i < 10; ++i) FencePollYieldDue(a);
  Check(!FencePollYieldDue(b) && b == 1, "another thread's count is its own");
  Check(a == 10, "and the first one is untouched");

  // A count that starts above the limit (never happens: a thread_local starts at 0) still yields once and restarts.
  uint32_t odd = 100;
  Check(FencePollYieldDue(odd) && odd == 0, "a count past the limit yields and restarts");

  if (g_failures == 0) std::printf("fence_spin_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
