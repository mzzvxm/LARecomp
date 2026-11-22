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
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "native_gfx/guest/physical_alias.h"
#include "render_cpu.h"

using namespace mcla::gpu::render_cpu;
using mcla::native_gfx::kPhysicalAliases;
using mcla::native_gfx::PhysicalRangeReadable;

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

// ---------------------------------------------------------------------------
// physical_alias.h: the order of the alias checks
// ---------------------------------------------------------------------------

// The check as IsPhysicalRangeReadable wrote it before the hint (guest_resources.cpp): the 0xE0 alias (one page off, skipped below its
// page), then 0xC0 and 0xA0, the first yes wins.
template <class Check>
bool Reference(uint32_t physical_address, uint64_t size, Check&& check) {
  const uint32_t phys = physical_address & 0x1FFFFFFFu;
  if (size == 0 || uint64_t(phys) + size > 0x20000000ull) {
    return false;
  }
  if (phys >= 0x1000u && check(0xE0000000u + (phys - 0x1000u), size)) {
    return true;
  }
  static constexpr uint32_t kOffsetFreeAliases[] = {0xC0000000u, 0xA0000000u};
  for (uint32_t aliasBase : kOffsetFreeAliases) {
    if (check(aliasBase | phys, size)) {
      return true;
    }
  }
  return false;
}

// A mutex that copies as a new one (the heaps below are copied to run the reference on the same state).
struct CopyableMutex {
  std::mutex m;
  CopyableMutex() = default;
  CopyableMutex(const CopyableMutex&) {}
  CopyableMutex& operator=(const CopyableMutex&) { return *this; }
};

// Three page tables, one per alias, as sets of readable 4 KiB page runs in alias-local pages (a range is readable in an alias when
// every page of it is inside one run: the heap's QueryRangeAccess walks the pages of the range).
struct FakeHeaps {
  std::vector<std::pair<uint32_t, uint32_t>> runs[3];  // [first page, last page], by table index (0 = 0xE0, 1 = 0xC0, 2 = 0xA0)
  uint64_t calls = 0;
  uint64_t calls_by_alias[3] = {0, 0, 0};
  CopyableMutex lock_holder;

  static int IndexOf(uint32_t virtual_address) {
    switch (virtual_address >> 29) {  // 0xE0000000, 0xC0000000, 0xA0000000 and the 512 MiB behind each
      case 7: return 0;
      case 6: return 1;
      case 5: return 2;
      default: return -1;
    }
  }
  bool operator()(uint32_t virtual_address, uint64_t size) {
    ++calls;
    const int alias = IndexOf(virtual_address);
    if (alias < 0 || size == 0 || uint64_t(virtual_address) + size > 0x100000000ull) {
      return false;
    }
    ++calls_by_alias[alias];
    const uint32_t local = virtual_address & 0x1FFFFFFFu;
    const uint32_t first = local >> 12;
    const uint32_t last = uint32_t((uint64_t(local) + size - 1) >> 12);
    // The heap's lock and a lookup of the run holding the first page (the runs are sorted by their first page: a binary search, so a no
    // costs what a yes does, as a lock and a page walk do).
    std::lock_guard<std::mutex> lock(lock_holder.m);
    const auto& r = runs[alias];
    auto it = std::upper_bound(r.begin(), r.end(), std::make_pair(first, UINT32_MAX));
    if (it == r.begin()) {
      return false;
    }
    --it;
    return it->first <= first && last <= it->second;
  }
};

FakeHeaps MakeHeaps(std::mt19937_64& rng, double c0_share) {
  FakeHeaps h;
  // The physical space in 1 MiB cells (256 pages): each cell is committed through one alias, or two, or none; vertex data (cells in
  // 0xC0) dominates, as in the game.
  for (uint32_t cell = 0; cell < 512; ++cell) {
    const double roll = double(rng() % 1000) / 1000.0;
    const uint32_t first = cell * 256;
    const uint32_t last = first + 255;
    if (roll < c0_share) {
      h.runs[1].push_back({first, last});
    } else if (roll < c0_share + 0.15) {
      h.runs[0].push_back({first ? first - 1 : 0, last - 1});  // the 0xE0 alias is one page off: physical page v is its page v - 1
    } else if (roll < c0_share + 0.2) {
      h.runs[2].push_back({first, last});
    } else if (roll < c0_share + 0.25) {
      h.runs[0].push_back({first ? first - 1 : 0, last - 1});
      h.runs[1].push_back({first, last});  // committed in two aliases
    }
  }
  return h;
}

void TestAliasEqualsReference() {
  std::mt19937_64 rng(77);
  for (const double share : {0.0, 0.5, 0.9}) {
    FakeHeaps heaps = MakeHeaps(rng, share);
    uint32_t hint = 0;
    uint64_t different = 0;
    uint64_t yes = 0;
    uint64_t asked = 0;
    for (int i = 0; i < 150000; ++i) {
      uint32_t phys = uint32_t(rng());
      uint64_t size;
      switch (rng() % 8) {
        case 0: size = 0; break;
        case 1: size = 1; break;
        case 2: size = uint64_t(rng() % 70000) + 1; break;
        case 3: size = uint64_t(rng() % 5000000) + 1; break;  // runs over cells
        case 4: size = 0x20000000ull - (phys & 0x1FFFFFFFu) + (rng() % 3) - 1; break;  // at the edge of the space
        case 5: size = 0xFFFFFFFFFFull; break;
        default: size = uint64_t(rng() % 20000) + 1;
      }
      if (rng() % 11 == 0) phys &= 0x1FFFu;     // near the bottom: the 0xE0 alias's one page
      if (rng() % 7 == 0) phys |= 0xE0000000u;  // the high bits are ignored
      FakeHeaps ref_heaps = heaps;
      const bool want = Reference(phys, size, ref_heaps);
      const uint64_t calls_before = heaps.calls;
      const bool got = PhysicalRangeReadable(phys, size, &hint, heaps);
      asked += heaps.calls - calls_before;
      yes += got;
      different += got != want;
    }
    Check(different == 0, "the answer is the reference's on 150000 random ranges (heaps with " + std::to_string(int(share * 100)) + "% in 0xC0), " +
                              std::to_string(yes) + " yes");
    Check(yes > 1000, "the sample has enough ranges that are readable (" + std::to_string(yes) + ")");
    Check(asked > 0, "checks were asked");
  }
}

void TestAliasCallCounts() {
  // One alias commits everything (0xC0): after the first yes the hint names it, and a range costs one check.
  FakeHeaps heaps;
  heaps.runs[1].push_back({0, 0x1FFFF});
  uint32_t hint = 0;
  Check(PhysicalRangeReadable(0x00400000u, 4096, &hint, heaps) && hint == 1, "a yes in 0xC0 sets the hint to 1");
  Check(heaps.calls == 2, "the first query asked 0xE0 (no) then 0xC0 (yes): 2 checks");
  heaps.calls = 0;
  for (int i = 0; i < 100; ++i) {
    Check(PhysicalRangeReadable(0x00400000u + uint32_t(i) * 4096u, 4096, &hint, heaps), "every later range is readable");
  }
  Check(heaps.calls == 100, "with the hint on 0xC0 each range costs 1 check");
  // A table-order walk (no hint) asks 0xE0 first every time.
  FakeHeaps plain;
  plain.runs[1].push_back({0, 0x1FFFF});
  for (int i = 0; i < 100; ++i) {
    Check(PhysicalRangeReadable(0x00400000u + uint32_t(i) * 4096u, 4096, nullptr, plain), "no hint: readable");
  }
  Check(plain.calls == 200, "no hint: 2 checks a range, as before");
  // A range nobody commits asks every alias, whatever the hint says (and leaves the hint alone).
  FakeHeaps none;
  hint = 2;
  Check(!PhysicalRangeReadable(0x01000000u, 4096, &hint, none), "a range in no alias is not readable");
  Check(none.calls == 3 && hint == 2, "all three aliases were asked, the hint stayed");
  // The hint moves with the answer.
  FakeHeaps mixed;
  mixed.runs[0].push_back({0x1FFF, 0x2FFE});  // physical pages 0x2000 and up through 0xE0 (its page v - 1)
  mixed.runs[2].push_back({0x100, 0x1FF});    // physical pages 0x100.. through 0xA0
  hint = 0;
  Check(PhysicalRangeReadable(0x00100000u, 100, &hint, mixed) && hint == 2, "a yes only in 0xA0 sets the hint to 2");
  mixed.calls = 0;
  Check(PhysicalRangeReadable(0x02000000u, 100, &hint, mixed) && hint == 0, "then a yes only in 0xE0 sets it back to 0");
  Check(mixed.calls == 2, "the hint (2) was wrong: asked 0xA0 (no), then the table order from 0xE0 (yes): 2 checks");
  // A hint out of range is ignored (table order).
  hint = 9;
  mixed.calls = 0;
  Check(PhysicalRangeReadable(0x02000000u, 100, &hint, mixed), "a hint out of range still gives the answer");
  Check(mixed.calls == 1, "and asks in table order (0xE0 first, a yes)");
}

void TestAliasEdges() {
  FakeHeaps heaps;
  heaps.runs[0].push_back({0, 0x1FFFF});
  heaps.runs[1].push_back({0, 0x1FFFF});
  heaps.runs[2].push_back({0, 0x1FFFF});
  uint32_t hint = 1;
  Check(!PhysicalRangeReadable(0x1000, 0, &hint, heaps) && heaps.calls == 0, "size 0: not readable, nothing asked");
  Check(!PhysicalRangeReadable(0x1FFFFFFFu, 2, &hint, heaps) && heaps.calls == 0, "past the end of the 512 MiB space: not readable, nothing asked");
  Check(PhysicalRangeReadable(0x1FFFFFFFu, 1, &hint, heaps), "the last byte of the space is readable");
  // Below the 0xE0 alias's first page that alias is skipped (not asked), the others answer.
  FakeHeaps low;
  low.runs[1].push_back({0, 0xFF});
  hint = 0;
  Check(PhysicalRangeReadable(0x0800, 16, &hint, low) && hint == 1, "below 0x1000 the 0xC0 alias answers");
  Check(low.calls == 1 && low.calls_by_alias[0] == 0, "the 0xE0 alias was not asked below its first page");
  // High bits of the address are dropped (an alias address passed by mistake is the same physical address).
  low.calls = 0;
  Check(PhysicalRangeReadable(0xC0000800u, 16, &hint, low), "the high bits are masked off, as before");
  Check(kPhysicalAliases[0].base == 0xE0000000u && kPhysicalAliases[0].page_offset == 0x1000u && kPhysicalAliases[1].base == 0xC0000000u &&
            kPhysicalAliases[1].page_offset == 0 && kPhysicalAliases[2].base == 0xA0000000u && kPhysicalAliases[2].page_offset == 0,
        "the table is the three aliases in the order they were asked");
}

// What the saving is worth: the same queries through a check that is a scan of the heap's runs (the real one takes a lock and walks a page table).
void BenchAlias() {
  using Clock = std::chrono::steady_clock;
  std::mt19937_64 rng(5);
  FakeHeaps heaps = MakeHeaps(rng, 0.9);
  std::vector<uint32_t> queries;
  for (int i = 0; i < 200000; ++i) {
    const uint32_t cell = uint32_t(rng() % 500);
    queries.push_back(cell * 0x100000u + uint32_t(rng() % 0x80000u));
  }
  for (const bool hinted : {false, true}) {
    FakeHeaps h = heaps;
    uint32_t hint = 0;
    uint64_t yes = 0;
    const auto t0 = Clock::now();
    for (int round = 0; round < 20; ++round) {
      for (const uint32_t q : queries) {
        yes += PhysicalRangeReadable(q, 4096, hinted ? &hint : nullptr, h);
      }
    }
    const auto t1 = Clock::now();
    const double n = double(queries.size()) * 20.0;
    std::printf("  alias walk %s: %.2f checks a query, %.1f ns a query (%llu yes)\n", hinted ? "with the hint" : "table order  ", double(h.calls) / n,
                std::chrono::duration<double, std::nano>(t1 - t0).count() / n, (unsigned long long)yes);
  }
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
  TestAliasEqualsReference();
  TestAliasCallCounts();
  TestAliasEdges();
  if (g_bench) {
    BenchSampler();
    BenchAlias();
  }
  std::printf("render_cpu_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
