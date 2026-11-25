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
  using mcla::gpu::render_wait::ComposedPresentCeiling;
  using mcla::gpu::render_wait::ComposedPresentDue;
  using mcla::gpu::render_wait::LimitRenderRateForPresents;
  using mcla::gpu::render_wait::MayUseComposedCopy;
  using mcla::gpu::render_wait::ShouldSpin;
  Check(ShouldSpin("spin"), "explicit spin restores the legacy wait");
  Check(!ShouldSpin("wait"), "explicit wait blocks");
  Check(!ShouldSpin("auto"), "auto blocks on every CPU");
  Check(!ShouldSpin(""), "empty mode blocks");
  Check(!ShouldSpin("SPIN"), "unexpected case blocks");
  Check(!ShouldSpin("spinwait"), "a prefix match is insufficient");

  Check(MayUseComposedCopy(true, true, 0x10DE), "Windows NVIDIA Vulkan may use the DWM copy path");
  Check(!MayUseComposedCopy(false, true, 0x10DE), "Linux NVIDIA Vulkan is unaffected");
  Check(!MayUseComposedCopy(true, false, 0x10DE), "Windows NVIDIA D3D12 is unaffected");
  Check(!MayUseComposedCopy(true, true, 0x1002), "Windows AMD Vulkan is unaffected");
  Check(!MayUseComposedCopy(true, true, 0x8086), "Windows Intel Vulkan is unaffected");
  Check(!MayUseComposedCopy(true, true, 0), "an unknown adapter is unaffected");
  Check(ComposedPresentCeiling(true, true, 240) == 0, "exclusive fullscreen is exempt");
  Check(ComposedPresentCeiling(false, false, 240) == 0, "the debug switch disables the ceiling");
  Check(ComposedPresentCeiling(false, true, 0) == 60, "missing refresh uses a 60 FPS fallback");
  Check(ComposedPresentCeiling(false, true, 1) == 60, "default refresh sentinel uses the fallback");
  Check(ComposedPresentCeiling(false, true, 60) == 60, "a 60 Hz display uses 60 FPS");
  Check(ComposedPresentCeiling(false, true, 75) == 75, "a 75 Hz display uses 75 FPS");
  Check(ComposedPresentCeiling(false, true, 120) == 120, "a 120 Hz display uses 120 FPS");
  Check(ComposedPresentCeiling(false, true, 240) == 120, "a 240 Hz display uses the safety ceiling");
  Check(LimitRenderRateForPresents(0, 0, 1) == 0, "uncapped stays uncapped outside the policy");
  Check(LimitRenderRateForPresents(144, 0, 1) == 144, "player cap is unchanged outside the policy");
  Check(LimitRenderRateForPresents(0, 120, 1) == 120, "uncapped uses the composed ceiling");
  Check(LimitRenderRateForPresents(60, 120, 1) == 60, "lower player cap wins");
  Check(LimitRenderRateForPresents(144, 120, 1) == 120, "composed ceiling wins over a higher cap");
  Check(LimitRenderRateForPresents(120, 120, 1) == 120, "a matching player cap stays the same");
  Check(LimitRenderRateForPresents(0, 75, 2) == 37, "two presents per frame round the render cap down");
  Check(LimitRenderRateForPresents(0, 120, 4) == 30, "four presents per frame use one quarter of the ceiling");
  Check(LimitRenderRateForPresents(20, 120, 4) == 20, "a lower frame generation cap still wins");
  Check(LimitRenderRateForPresents(0, 120, 0) == 120, "a missing multiplier counts one present");
  Check(LimitRenderRateForPresents(0, 60, 61) == 1, "rounding cannot make the limit uncapped");
  Check(ComposedPresentDue(0, 10000000, 120) == 0, "first present has no hold");
  Check(ComposedPresentDue(10000000, 10000000, 0) == 0, "disabled policy has no hold");
  Check(ComposedPresentDue(10000000, 10000000, 120) == 10083334,
        "the next present is spaced from the actual call, rounded up to a whole tick");

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
