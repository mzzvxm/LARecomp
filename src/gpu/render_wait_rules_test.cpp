#include "render_wait_rules.h"

#include <cstdint>
#include <cstdio>
#include <limits>

#include "os/cpu_affinity_rules.h"

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

void CheckMask(unsigned requested, uint64_t allowed, uint64_t expected, unsigned expected_count) {
  uint64_t selected = 0;
  const unsigned count = larecomp::os::SelectFirstLogicalProcessors(
      requested, std::numeric_limits<uint64_t>::digits,
      [&](unsigned cpu) { return (allowed & (uint64_t{1} << cpu)) != 0; },
      [&](unsigned cpu) { selected |= uint64_t{1} << cpu; });
  Check(selected == expected, "selected the first eligible processors");
  Check(count == expected_count, "reported the effective processor count");
}

}  // namespace

int main() {
  using mcla::gpu::render_wait::ShouldSpin;
  Check(ShouldSpin("spin"), "explicit spin restores the legacy wait");
  Check(!ShouldSpin("wait"), "explicit wait blocks");
  Check(!ShouldSpin("auto"), "auto blocks on every CPU");
  Check(!ShouldSpin(""), "empty mode blocks");
  Check(!ShouldSpin("SPIN"), "unexpected case blocks");
  Check(!ShouldSpin("spinwait"), "a prefix match is insufficient");

  CheckMask(0, 0xffff, 0, 0);
  CheckMask(6, 0xffff, 0x003f, 6);
  CheckMask(8, 0xffff, 0x00ff, 8);
  CheckMask(8, 0x5034, 0x5034, 5);
  CheckMask(3, 0x5034, 0x0034, 3);
  CheckMask(8, 0, 0, 0);
  CheckMask(2, (uint64_t{1} << 63) | 4, (uint64_t{1} << 63) | 4, 2);

  // Linux CPU IDs need not fit in a 64-bit Windows affinity mask.
  unsigned selected[2]{};
  unsigned next = 0;
  const unsigned count = larecomp::os::SelectFirstLogicalProcessors(
      2, 1024, [](unsigned cpu) { return cpu == 2 || cpu == 500 || cpu == 1023; },
      [&](unsigned cpu) {
        if (next < 2) selected[next] = cpu;
        ++next;
      });
  Check(count == 2 && next == 2, "limited a sparse Linux mask to two processors");
  Check(selected[0] == 2 && selected[1] == 500, "selected high-numbered Linux CPUs in order");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
