// Check of the small render-thread CPU savings (render_cpu.h; physical_alias.h). A program of its own: it needs no renderer, no ReXGlue
// and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_render_cpu_test          (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_render_cpu_test [--bench]
//
// --bench times the old and the new path of each saving on this PC (cache-hot, one core), the numbers perf\RENDER-CPU-HOT.md quotes.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "render_cpu.h"

using namespace mcla::gpu::render_cpu;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_bench = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// ---------------------------------------------------------------------------
// the mask
// ---------------------------------------------------------------------------

void TestBits() {
  Check(kAll == (kSampledDrawTimers | kAliasHint | kDeviceCache), "kAll is every bit");
  Check((kSampledDrawTimers & kAliasHint) == 0 && (kSampledDrawTimers & kDeviceCache) == 0 && (kAliasHint & kDeviceCache) == 0,
        "the bits are distinct");
  Check(Has(kAll, kSampledDrawTimers) && Has(kAll, kAliasHint) && Has(kAll, kDeviceCache), "the default mask has every saving");
  Check(!Has(0, kSampledDrawTimers) && !Has(0, kAliasHint) && !Has(0, kDeviceCache), "mask 0 has none");
  Check(Has(kAliasHint, kAliasHint) && !Has(kAliasHint, kDeviceCache), "a single bit");
  // The setting's range is 0..7: every bit pattern in it is a mask of the three.
  Check(kAll == 7, "the setting's range {0, 7} is the whole mask");
}

// ---------------------------------------------------------------------------
// DrawSampler
// ---------------------------------------------------------------------------

void TestSamplerOff() {
  DrawSampler s;  // default: not sampled
  Check(!s.sampled(), "a new sampler is not sampling");
  bool all = true;
  for (int i = 0; i < 1000; ++i) {
    all &= s.Take();
  }
  Check(all, "not sampled: every draw is timed");
  Check(s.Scale(12345) == 12345, "not sampled: the scale is the identity");
  s.Configure(true);
  s.Configure(false);
  all = true;
  for (int i = 0; i < 100; ++i) {
    all &= s.Take();
  }
  Check(all && s.Scale(77) == 77, "Configure(false) goes back to timing every draw");
}

void TestSamplerOn() {
  DrawSampler s;
  s.Configure(true);
  Check(s.sampled(), "Configure(true) samples");
  Check(DrawSampler::kPeriod % 2 == 1 && DrawSampler::kPeriod > 1, "the period is odd (no aliasing with a pattern of 2, 4, 8)");
  // The first draw is timed, then one in kPeriod.
  Check(s.Take(), "the first draw after Configure is timed");
  uint32_t gap = 0;
  uint32_t timed = 1;
  bool even = true;
  for (uint32_t i = 1; i < 7000; ++i) {
    ++gap;
    if (s.Take()) {
      even &= gap == DrawSampler::kPeriod;
      gap = 0;
      ++timed;
    }
  }
  Check(even, "timed draws are exactly kPeriod apart");
  Check(timed == 1000, "7000 draws: 1000 timed");
  Check(s.Scale(100) == 100u * DrawSampler::kPeriod, "a timed draw counts kPeriod times");
}

// A statistic summed per draw, exact against sampled: the sampled sum is an estimate of the exact one. The draws' costs are a mix like
// the race's (cheap ones, some dear, a periodic pattern of 2 and 8 on top, an occasional heavy one); the sampler's counter runs on across
// the frames, as the scene's does.
void TestSamplerEstimate() {
  std::mt19937_64 rng(20261122);
  std::vector<uint64_t> cost;
  for (int frame = 0; frame < 600; ++frame) {
    const int draws = 3500 + int(rng() % 600);
    for (int i = 0; i < draws; ++i) {
      uint64_t c = 800 + (rng() % 1500);
      if (i % 8 == 0) c += 4000;
      if (i % 2 == 1) c += 300;
      if (rng() % 97 == 0) c += 20000;
      cost.push_back(c);
    }
  }
  uint64_t exact = 0;
  for (const uint64_t c : cost) exact += c;
  DrawSampler s;
  s.Configure(true);
  uint64_t estimate = 0;
  uint32_t timed = 0;
  for (const uint64_t c : cost) {
    if (s.Take()) {
      estimate += s.Scale(c);
      ++timed;
    }
  }
  const double error = std::fabs(double(estimate) - double(exact)) / double(exact);
  Check(error < 0.01, "the sampled sum is within 1% of the exact one over 600 frames (error " + std::to_string(error * 100.0) + "%)");
  Check(timed >= cost.size() / DrawSampler::kPeriod && timed <= cost.size() / DrawSampler::kPeriod + 1,
        "one draw in kPeriod was timed: " + std::to_string(timed) + " of " + std::to_string(cost.size()));
  DrawSampler t;
  uint64_t exact_all = 0;
  for (const uint64_t c : cost) {
    if (t.Take()) exact_all += t.Scale(c);
  }
  Check(exact_all == exact, "not sampled: the sum is the exact one");
  // Window by window (600 frames in a run are one window; here 10 windows of 60 frames): no window is far off.
  DrawSampler w;
  w.Configure(true);
  size_t at = 0;
  double worst = 0.0;
  for (int window = 0; window < 10; ++window) {
    uint64_t window_exact = 0;
    uint64_t window_estimate = 0;
    const size_t end = std::min(cost.size(), at + cost.size() / 10);
    for (; at < end; ++at) {
      window_exact += cost[at];
      if (w.Take()) window_estimate += w.Scale(cost[at]);
    }
    worst = std::max(worst, std::fabs(double(window_estimate) - double(window_exact)) / double(window_exact));
  }
  Check(worst < 0.03, "every window of 60 frames is within 3% (worst " + std::to_string(worst * 100.0) + "%)");
}

// The timers' cost is the saving: a clock read per timed draw. A bench of Take on its own (it must be far under a clock read).
void BenchSampler() {
  using Clock = std::chrono::steady_clock;
  DrawSampler s;
  s.Configure(true);
  constexpr int kN = 100'000'000;
  uint64_t taken = 0;
  const auto t0 = Clock::now();
  for (int i = 0; i < kN; ++i) {
    taken += s.Take();
  }
  const auto t1 = Clock::now();
  std::printf("  DrawSampler::Take: %.2f ns a call (%llu taken)\n", std::chrono::duration<double, std::nano>(t1 - t0).count() / kN,
              (unsigned long long)taken);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) g_bench = true;
  }
  TestBits();
  TestSamplerOff();
  TestSamplerOn();
  TestSamplerEstimate();
  if (g_bench) {
    BenchSampler();
  }
  std::printf("render_cpu_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
