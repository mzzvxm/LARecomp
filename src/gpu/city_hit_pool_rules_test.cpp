// Check of the city hit data's pool (city_hit_pool_rules.h): CITY-RT M2 package H3. A program of its own: no renderer, no game files, no GPU; exit 0 = passed.
//
//   build: cmake --build <dir> --target larecomp_city_hit_pool_rules_test, then run src/gpu/larecomp_city_hit_pool_rules_test [<src/gpu dir>]
//   or by hand: clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/city_hit_pool_rules_test.cpp src/citypack/city_hit_format.cpp src/citypack/pack_format.cpp
//   (with the src/gpu folder as the argument it also checks that hlsl/city_hit_probe_cs.hlsl holds this header's numbers)
//
// It checks: the budgets by class and level, the pool's cap in the streaming plan (a plan with no pool is the plan it was; a pool that is the short one limits the loads, trades the
// farthest block for a nearer one, and says so), the block estimate against the plan the real block gets, the memory the card leaves room for, the allocator (first fit, coalescing,
// 16 byte units, the 32 bit offsets, fragmentation), the sector table (rows, versions, the check that finds an overlap or a stray offset), the book (place, commit, evict with the
// range freed later, a generation that drops a stale free, abandon, a stress of 30,000 random operations with the invariants checked), the probe (the checksum against a plain
// implementation, the verdicts), and the whole chain on synthetic sectors: blocks filled by FillPoolBlock, placed by the book, and decoded through the sector table by DecodeHit.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "city_hit_pool_rules.h"

using namespace mcla::gpu;
using namespace mcla::gpu::city_hit;
using mcla::gpu::gpu_class::GpuClass;
namespace cf = larecomp::citypack;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

struct Lcg {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t Next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(s >> 33);
  }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : Next() % n; }
};

// ---------------------------------------------------------------------------
// the budget
// ---------------------------------------------------------------------------

void TestBudgets() {
  using city_rt::Level;
  CHECK(PoolBudgetFor(GpuClass::kLow, Level::kHigh).on() && !PoolBudgetFor(GpuClass::kHigh, Level::kOff).on() && PoolBudgetFor(GpuClass::kLow, Level::kUltra).on());
  CHECK(PoolBudgetFor(GpuClass::kMid, Level::kHigh).capacity == 128 * kMiB && PoolBudgetFor(GpuClass::kMid, Level::kHigh).staged_bytes == 17 * kMiB);
  CHECK(PoolBudgetFor(GpuClass::kHigh, Level::kHigh).capacity == 128 * kMiB && PoolBudgetFor(GpuClass::kHigh, Level::kHigh).staged_bytes == 17 * kMiB);
  CHECK(PoolBudgetFor(GpuClass::kHigh, Level::kUltra).capacity == 256 * kMiB && PoolBudgetFor(GpuClass::kHigh, Level::kUltra).staged_bytes == 20 * kMiB);
  CHECK(PoolBudgetFor(GpuClass::kMid, Level::kUltra).capacity == 256 * kMiB);
  // the class budgets are the format header's "64 to 256 MiB", whole 16 byte units, under the 32 bit offsets
  for (const GpuClass cls : {GpuClass::kMid, GpuClass::kHigh}) {
    for (const Level level : {Level::kHigh, Level::kUltra}) {
      const PoolBudget p = PoolBudgetFor(cls, level);
      CHECK(p.capacity >= 64 * kMiB && p.capacity <= 256 * kMiB && p.capacity % cf::kPoolAlignment == 0 && p.capacity < cf::kMaxPoolBytes);
    }
  }
  // the plan's cap is nine tenths, in whole 16 byte units
  for (const uint64_t capacity : {64 * kMiB, 128 * kMiB, 256 * kMiB}) {
    CHECK(PlanCap(capacity) % 16 == 0 && PlanCap(capacity) <= capacity / 10 * 9 + 16 && PlanCap(capacity) + capacity / 10 >= capacity - 16);
  }
  // WithPool: the cap and the staging, nothing when either is off
  const city_rt::Budget high = city_rt::BudgetFor(GpuClass::kHigh, Level::kHigh);
  const city_rt::Budget with = WithPool(high, PoolBudgetFor(GpuClass::kHigh, Level::kHigh));
  CHECK(with.pool_cap == PlanCap(128 * kMiB) && with.upload_bytes == 17 * kMiB && with.memory_cap == high.memory_cap && with.hi_radius == high.hi_radius);
  CHECK(WithPool(high, PoolBudget{}).pool_cap == 0 && WithPool(city_rt::Budget{}, PoolBudgetFor(GpuClass::kHigh, Level::kHigh)).pool_cap == 0);
  CHECK(high.pool_cap == 0 && city_rt::BudgetFor(GpuClass::kMid, Level::kHigh).pool_cap == 0);  // M1's budgets carry no pool
  // a budget whose staging is already above the pool's keeps it
  city_rt::Budget big = high;
  big.upload_bytes = 64 * kMiB;
  CHECK(WithPool(big, PoolBudgetFor(GpuClass::kHigh, Level::kHigh)).upload_bytes == 64 * kMiB);
  // H0's HitBudget fields
  const cf::HitBudget h = HitBudgetFor(GpuClass::kHigh, Level::kUltra);
  CHECK(h.pool_bytes == 256 * kMiB && h.staged_bytes == 20 * kMiB && h.on() && HitBudgetFor(GpuClass::kLow, Level::kUltra).on());
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

void TestFit() {
  // plenty of room: the class's size
  CHECK(FitCapacity(256 * kMiB, 1000 * kMiB, 16000 * kMiB) == 256 * kMiB);
  // an adapter that reports no budget fits anything (the creation is the judge)
  CHECK(FitCapacity(256 * kMiB, 123456789, 0) == 256 * kMiB);
  // the line is kHighPercent of the budget: 90% of 4000 MiB is 3600
  CHECK(FitCapacity(256 * kMiB, 3300 * kMiB, 4000 * kMiB) == 256 * kMiB);   // 3556 < 3600
  CHECK(FitCapacity(256 * kMiB, 3400 * kMiB, 4000 * kMiB) == 128 * kMiB);   // 3656 over, 3528 under
  CHECK(FitCapacity(256 * kMiB, 3500 * kMiB, 4000 * kMiB) == 64 * kMiB);    // 3564 under
  CHECK(FitCapacity(256 * kMiB, 3550 * kMiB, 4000 * kMiB) == 32 * kMiB);    // 3582 under
  CHECK(FitCapacity(256 * kMiB, 3580 * kMiB, 4000 * kMiB) == 0);            // nothing fits: no hit data this time
  CHECK(FitCapacity(64 * kMiB, 3560 * kMiB, 4000 * kMiB) == 32 * kMiB);
  // never more than asked, never under the floor
  CHECK(FitCapacity(16 * kMiB, 0, 4000 * kMiB) == 0);
  for (uint64_t used = 0; used < 4000 * kMiB; used += 37 * kMiB) {
    const uint64_t got = FitCapacity(256 * kMiB, used, 4000 * kMiB);
    CHECK(got == 0 || (got >= kMinPoolBytes && got <= 256 * kMiB));
  }
}

// ---------------------------------------------------------------------------
// the allocator
// ---------------------------------------------------------------------------

void TestAllocator() {
  PoolAllocator pool;
  CHECK(!pool.Reset(0) && !pool.Reset(100) && !pool.Reset(cf::kMaxPoolBytes + 16));  // not a whole number of 16 byte units, or beyond the 32 bit offsets
  CHECK(pool.Reset(1024) && pool.capacity() == 1024 && pool.used() == 0);
  uint64_t a = 0, b = 0, c = 0, d = 0;
  CHECK(pool.Alloc(100, &a) && a == 0);   // rounded to 112
  CHECK(pool.Alloc(16, &b) && b == 112);
  CHECK(pool.Alloc(400, &c) && c == 128 && c % 16 == 0);
  CHECK(pool.used() == 112 + 16 + 400);
  CHECK(!pool.Alloc(0, &d));
  CHECK(!pool.Alloc(1024, &d));            // too long
  CHECK(pool.stats().failures == 2);
  // a hole in the middle: reused by what fits, not by what does not
  pool.Free(b, 16);
  CHECK(pool.used() == 512 && pool.stats().free_ranges == 2);
  CHECK(pool.Alloc(32, &d) && d == 528);   // the 16 byte hole is too small for 32: first fit goes on to the tail
  CHECK(pool.Alloc(16, &d) && d == 112);   // ... and the next 16 byte block takes it
  // coalescing: free everything, one range of the whole pool again
  pool.Free(a, 100);
  pool.Free(112, 16);
  pool.Free(c, 400);
  pool.Free(528, 32);
  CHECK(pool.used() == 0 && pool.stats().free_ranges == 1 && pool.stats().largest_free == 1024 && pool.FragmentationPercent() == 0);
  CHECK(pool.stats().peak >= 560);
  // fragmentation: alternate blocks free, the free bytes are many small ranges
  PoolAllocator f;
  CHECK(f.Reset(16 * 100));
  std::vector<uint64_t> bases(100);
  for (uint64_t i = 0; i < 100; ++i) CHECK(f.Alloc(16, &bases[i]));
  for (uint64_t i = 0; i < 100; i += 2) f.Free(bases[i], 16);
  CHECK(f.free_bytes() == 50 * 16 && f.stats().largest_free == 16 && f.FragmentationPercent() >= 97);
  uint64_t big = 0;
  CHECK(!f.Alloc(32, &big));               // 800 bytes free in all, none 32 long: the failure the retry rule is for
  // the allocator never hands out the same byte twice (a random walk)
  PoolAllocator r;
  CHECK(r.Reset(64 * kMiB));
  Lcg rng;
  struct Live { uint64_t base, bytes; };
  std::vector<Live> live;
  for (int step = 0; step < 6000; ++step) {
    if (live.empty() || rng.Below(100) < 55) {
      const uint64_t bytes = 16 + rng.Below(600000);
      uint64_t at = 0;
      if (r.Alloc(bytes, &at)) live.push_back({at, PoolAllocator::Round(bytes)});
    } else {
      const size_t i = rng.Below(uint32_t(live.size()));
      r.Free(live[i].base, live[i].bytes);
      live.erase(live.begin() + long(i));
    }
  }
  std::sort(live.begin(), live.end(), [](const Live& x, const Live& y) { return x.base < y.base; });
  bool disjoint = true;
  uint64_t sum = 0;
  for (size_t i = 0; i < live.size(); ++i) {
    sum += live[i].bytes;
    if (i + 1 < live.size() && live[i].base + live[i].bytes > live[i + 1].base) disjoint = false;
    if (live[i].base % 16 != 0 || live[i].base + live[i].bytes > r.capacity()) disjoint = false;
  }
  CHECK(disjoint && sum == r.used());
  // a free that names a range outside the pool is ignored
  const uint64_t used_before = r.used();
  r.Free(r.capacity(), 64);
  CHECK(r.used() == used_before);
}

// ---------------------------------------------------------------------------
// blocks: a synthetic sector detail of `triangles` triangles
// ---------------------------------------------------------------------------

struct Synth {
  cf::MeshData mesh;
  cf::HitBlob blob;
  cf::PoolBlockPlan plan;
  std::vector<uint8_t> bytes;
};

Synth MakeSynth(Lcg& rng, uint32_t triangles, uint32_t opaque, bool runs, bool corners) {
  Synth s;
  const uint32_t vertices = std::max(3u, triangles * 3 / 5);
  s.mesh.positions.resize(size_t(vertices) * 3);
  for (float& p : s.mesh.positions) p = float(int32_t(rng.Below(4000)) - 2000) * 0.5f;
  s.mesh.indices.resize(size_t(triangles) * 3);
  for (uint32_t& i : s.mesh.indices) i = rng.Below(vertices);
  s.mesh.opaque_triangles = opaque;
  if (runs) {
    s.blob.triangle_count = triangles;
    s.blob.opaque_triangles = opaque;
    s.blob.flags = corners ? cf::kHitBlobHasCorners : 0;
    // two runs when there are both kinds of triangles, else one; a run never crosses the opaque / alpha tested border
    const auto add = [&](uint32_t first, uint32_t count, bool alpha) {
      if (count == 0) return;
      cf::HitRun r;
      r.first_triangle = first;
      r.triangle_count = count;
      r.material = rng.Below(5);
      r.flags = (alpha ? cf::kRunAlphaTested : 0u) | cf::kRunHasUv | cf::kRunHasColour;
      s.blob.runs.push_back(r);
    };
    add(0, opaque, false);
    add(opaque, triangles - opaque, true);
    if (corners) {
      s.blob.corners.resize(size_t(triangles) * 6);
      for (uint32_t& w : s.blob.corners) w = rng.Next();
    }
  }
  s.plan = cf::PlanPoolBlock(vertices, triangles, runs ? uint32_t(s.blob.runs.size()) : 0, runs, corners);
  s.bytes.assign(size_t(s.plan.total_bytes), 0);
  std::string error;
  const bool ok = cf::FillPoolBlock(s.plan, s.mesh, runs ? &s.blob : nullptr, s.bytes.data(), s.bytes.size(), &error);
  if (!ok) std::fprintf(stderr, "synthetic block: %s\n", error.c_str());
  CHECK(ok);
  return s;
}

void TestEstimate() {
  // The estimate is the plan of a block with the estimated number of runs; the real block differs by its runs only
  const uint32_t triangles = 50000, vertices = 30000;
  const uint64_t estimate = EstimateBlockBytes(vertices, triangles);
  CHECK(estimate == cf::PlanPoolBlock(vertices, triangles, kEstimatedRunsPerBlock, true, true).total_bytes);
  for (const uint32_t runs : {1u, 40u, 605u, 1024u}) {
    const uint64_t real = cf::PlanPoolBlock(vertices, triangles, runs, true, true).total_bytes;
    CHECK(real <= estimate);
    CHECK(estimate - real <= uint64_t(kEstimatedRunsPerBlock) * cf::kHitRunBytes + 64);
  }
  // M2 section 3.3: about 45 bytes a triangle (12 index, 7 position, 2 run id, 24 corners) plus the run table
  const double per_triangle = double(estimate) / triangles;
  CHECK(per_triangle > 44.0 && per_triangle < 50.0);
  // a larger sector costs more, monotonically
  CHECK(EstimateBlockBytes(vertices, triangles) < EstimateBlockBytes(vertices, triangles * 2) && EstimateBlockBytes(vertices, triangles) < EstimateBlockBytes(vertices * 2, triangles));
}

// ---------------------------------------------------------------------------
// the sector table
// ---------------------------------------------------------------------------

cf::GpuSectorRow RowFor(const cf::PoolBlockPlan& plan, uint64_t base, uint32_t triangles, uint32_t opaque) {
  cf::GpuSectorRow row;
  CHECK(cf::MakeSectorRow(plan, base, triangles, opaque, 0, &row));
  return row;
}

void TestSectorTable() {
  SectorTable t;
  t.Reset(439);
  CHECK(t.slots() == 878 && t.sector_count() == 439 && t.valid_count() == 0 && t.bytes() == 878 * 32);  // "878 for the retail game, 28 KiB"
  for (uint32_t slot = 0; slot < t.slots(); slot += 97) CHECK(t.RowAtSlot(slot).flags == 0);
  const cf::PoolBlockPlan plan = cf::PlanPoolBlock(1000, 1500, 3, true, true);
  const uint64_t v0 = t.version();
  CHECK(t.Set(5, false, RowFor(plan, 4096, 1500, 1200), 4096, plan.total_bytes));
  CHECK(t.version() > v0 && t.valid_count() == 1 && t.valid(5, false) && !t.valid(5, true) && t.Row(5, false).triangle_count == 1500);
  CHECK(t.block_base(cf::SectorRowOf(5, false)) == 4096 && t.block_bytes(cf::SectorRowOf(5, false)) == plan.total_bytes);
  // a lod and a hi of one sector are different slots, as the instance id says
  const cf::PoolBlockPlan lod = cf::PlanPoolBlock(30, 40, 1, true, true);
  CHECK(t.Set(5, true, RowFor(lod, 1 << 20, 40, 40), 1 << 20, lod.total_bytes) && t.valid_count() == 2 && t.Row(5, true).triangle_count == 40 && t.Row(5, false).triangle_count == 1500);
  // replacing a row does not count twice
  CHECK(t.Set(5, false, RowFor(plan, 8192, 1500, 1200), 8192, plan.total_bytes) && t.valid_count() == 2 && t.Row(5, false).index_offset == 8192);
  // refused: outside the table, a row that is not valid, a block that is not aligned, an empty block
  CHECK(!t.Set(439, false, RowFor(plan, 0, 1500, 1200), 0, 16) && !t.Set(5, false, cf::GpuSectorRow{}, 4096, 16) && !t.Set(6, false, RowFor(plan, 4096, 1500, 1200), 4097, 16) &&
        !t.Set(6, false, RowFor(plan, 4096, 1500, 1200), 4096, 0));
  CHECK(t.valid_count() == 2);
  // clear
  const uint64_t v1 = t.version();
  CHECK(t.Clear(5, false) && !t.Clear(5, false) && t.valid_count() == 1 && t.version() > v1 && t.Row(5, false).flags == 0 && t.block_bytes(cf::SectorRowOf(5, false)) == 0);
  CHECK(!t.Clear(9999, false));
  CHECK(t.Verify(64 * kMiB).empty());

  // the check finds what is wrong
  SectorTable bad;
  bad.Reset(10);
  const cf::PoolBlockPlan p = cf::PlanPoolBlock(100, 150, 2, true, true);
  CHECK(bad.Set(1, false, RowFor(p, 1024, 150, 150), 1024, p.total_bytes) && bad.Set(2, false, RowFor(p, 1024 + p.total_bytes + 16, 150, 150), 1024 + p.total_bytes + 16, p.total_bytes));
  CHECK(bad.Verify(64 * kMiB).empty());
  CHECK(!bad.Verify(2048).empty());  // a pool that is too small for the second block
  CHECK(bad.Set(3, false, RowFor(p, 1024 + p.total_bytes / 2, 150, 150), 1024 + p.total_bytes / 2, p.total_bytes) && bad.Verify(64 * kMiB).find("overlap") != std::string::npos);
  SectorTable stray;
  stray.Reset(4);
  cf::GpuSectorRow row = RowFor(p, 1024, 150, 150);
  row.run_table_offset = uint32_t(1024 + p.total_bytes + 4096);  // beyond its block
  CHECK(stray.Set(0, false, row, 1024, p.total_bytes) && !stray.Verify(64 * kMiB).empty());
  row = RowFor(p, 1024, 150, 150);
  row.flags &= ~cf::kSectorRowRuns;  // corners without runs
  CHECK(stray.Set(0, false, row, 1024, p.total_bytes) && stray.Verify(64 * kMiB).find("corners without runs") != std::string::npos);
  row = RowFor(p, 1024, 150, 150);
  row.opaque_triangles = 151;
  CHECK(stray.Set(0, false, row, 1024, p.total_bytes) && !stray.Verify(64 * kMiB).empty());
  stray.ClearAll();
  CHECK(stray.valid_count() == 0 && stray.Verify(64 * kMiB).empty());
}

// ---------------------------------------------------------------------------
// the book
// ---------------------------------------------------------------------------

void TestBook() {
  PoolBook book;
  CHECK(!book.ready());
  CHECK(book.Reset(8 * kMiB, 20) && book.ready() && book.Verify().empty());
  const uint64_t gen = book.generation();
  Lcg rng;
  const Synth s = MakeSynth(rng, 3000, 2500, true, true);
  uint64_t probe_samples = 0;
  const uint32_t checksum = ProbeOfBlock(s.bytes.data(), s.plan, 3000, &probe_samples);
  uint64_t base = 0;
  CHECK(book.Place(3, false, s.plan.total_bytes, &base) && base == 0);
  CHECK(!book.Place(3, false, s.plan.total_bytes, &base));  // a slot holds one block: evict first
  CHECK(!book.table().valid(3, false) && book.committed_bytes() == 0);  // placed, not visible yet
  CHECK(book.Commit(3, false, s.plan, 3000, 2500, 0, checksum, uint32_t(probe_samples)));
  CHECK(book.table().valid(3, false) && book.table().Row(3, false).index_offset == base && book.committed_bytes() == PoolAllocator::Round(s.plan.total_bytes));
  CHECK(book.Verify().empty());
  const std::vector<ProbeBlock> list = book.ProbeList();
  CHECK(list.size() == 1 && list[0].slot == cf::SectorRowOf(3, false) && list[0].checksum == checksum && list[0].triangles == 3000);

  // a second block goes after the first
  uint64_t base2 = 0;
  CHECK(book.Place(3, true, 4096, &base2) && base2 == PoolAllocator::Round(s.plan.total_bytes));
  // abandoned (the build failed): free at once, never visible
  CHECK(book.Abandon(3, true) && !book.Abandon(3, true) && book.Verify().empty());
  CHECK(!book.Abandon(3, false));  // a committed block is evicted, not abandoned

  // eviction: the row is cleared NOW, the range is free LATER
  const uint64_t version_before = book.table().version();
  const Block gone = book.Evict(3, false);
  CHECK(gone.present && gone.base == 0 && gone.bytes == PoolAllocator::Round(s.plan.total_bytes));
  CHECK(!book.table().valid(3, false) && book.table().version() > version_before && book.committed_bytes() == 0);
  CHECK(book.pending_free_bytes() == gone.bytes && book.allocator().used() == gone.bytes);  // the GPU may still read it
  CHECK(book.Verify().empty());
  uint64_t other = 0;
  CHECK(book.Place(4, false, 64, &other) && other == gone.bytes);  // the evicted range is NOT handed out yet
  CHECK(book.Abandon(4, false));
  book.FreeRange(gen, gone.base, gone.bytes);
  CHECK(book.pending_free_bytes() == 0 && book.allocator().used() == 0 && book.Verify().empty());
  CHECK(book.Place(4, false, 64, &other) && other == 0);  // now it is
  CHECK(!book.Evict(7, false).present);                    // nothing there

  // a generation that a reset has passed drops the free it queued: the new pool's bytes are not freed by the old pool's frees
  const Block again = [&] {
    uint64_t at = 0;
    CHECK(book.Place(6, false, 4096, &at));
    return book.Evict(6, false);
  }();
  CHECK(again.present && book.pending_free_bytes() == again.bytes);
  CHECK(book.Reset(8 * kMiB, 20) && book.generation() > gen);
  uint64_t fresh = 0;
  CHECK(book.Place(6, false, 4096, &fresh) && fresh == 0);
  book.FreeRange(gen, again.base, again.bytes);  // the old pool's deferred free arrives: ignored
  CHECK(book.allocator().used() == 4096 && book.block(6, false).present);

  // a plan that does not fit its range is refused and the range goes back at once
  PoolBook small;
  CHECK(small.Reset(1 * kMiB, 4));
  uint64_t at = 0;
  CHECK(small.Place(0, false, 256, &at));
  CHECK(!small.Commit(0, false, s.plan, 3000, 2500, 0, 0, 0));  // the block is 256 bytes, the plan far more
  CHECK(!small.block(0, false).present && small.allocator().used() == 0 && small.Verify().empty());
  // a slot that does not exist
  CHECK(!small.Place(4, false, 256, &at) && !small.Commit(9, false, s.plan, 1, 1, 0, 0, 0));
  // EvictAll
  CHECK(small.Place(0, false, s.plan.total_bytes, &at) && small.Commit(0, false, s.plan, 3000, 2500, 0, 0, 0));
  std::vector<Block> all;
  small.EvictAll(&all);
  CHECK(all.size() == 1 && small.table().valid_count() == 0 && small.pending_free_bytes() == all[0].bytes && small.Verify().empty());
  small.Forget();
  CHECK(!small.ready() && !small.Place(0, false, 16, &at));
}

// The book against 30,000 random operations: the invariants hold throughout and nothing leaks.
void TestBookStress() {
  PoolBook book;
  const uint32_t sectors = 60;
  CHECK(book.Reset(48 * kMiB, sectors));
  Lcg rng;
  struct Pending { uint64_t generation, base, bytes; int frames; };
  std::vector<Pending> pending;
  std::vector<Synth> pool_of_synth;
  for (int i = 0; i < 6; ++i) pool_of_synth.push_back(MakeSynth(rng, 400 + 1500u * uint32_t(i), (400 + 1500u * uint32_t(i)) / 2, true, i % 3 != 0));
  std::set<uint32_t> committed;
  uint32_t placement_failures = 0, evictions = 0, commits = 0;
  for (int step = 0; step < 30000; ++step) {
    const uint32_t sector = rng.Below(sectors);
    const bool lod = rng.Below(4) == 0;
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    const uint32_t roll = rng.Below(100);
    if (roll < 45) {
      if (book.block(sector, lod).present) continue;
      const Synth& s = pool_of_synth[rng.Below(6)];
      uint64_t base = 0;
      if (!book.Place(sector, lod, s.plan.total_bytes, &base)) {
        ++placement_failures;
        continue;
      }
      if (rng.Below(10) == 0) {
        CHECK(book.Abandon(sector, lod));
      } else {
        CHECK(book.Commit(sector, lod, s.plan, s.mesh.triangle_count(), s.mesh.opaque_triangles, 0, 7u * slot, 3));
        committed.insert(slot);
        ++commits;
      }
    } else if (roll < 90) {
      const Block b = book.Evict(sector, lod);
      if (b.present) {
        pending.push_back({book.generation(), b.base, b.bytes, 2});  // given back two frames later
        committed.erase(slot);
        ++evictions;
      }
    } else {
      for (Pending& p : pending) --p.frames;  // a frame passes
      for (auto it = pending.begin(); it != pending.end();) {
        if (it->frames <= 0) {
          book.FreeRange(it->generation, it->base, it->bytes);
          it = pending.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (step % 50 == 0) {
      const std::string why = book.Verify();
      if (!why.empty()) std::fprintf(stderr, "step %d: %s\n", step, why.c_str());
      CHECK(why.empty());
      CHECK(book.table().valid_count() == committed.size());
    }
  }
  CHECK(book.Verify().empty() && commits > 2000 && evictions > 2000);
  // everything given back: the pool is one range again
  std::vector<Block> rest;
  book.EvictAll(&rest);
  for (const Block& b : rest) book.FreeRange(book.generation(), b.base, b.bytes);
  for (const Pending& p : pending) book.FreeRange(p.generation, p.base, p.bytes);
  CHECK(book.allocator().used() == 0 && book.allocator().stats().free_ranges == 1 && book.pending_free_bytes() == 0 && book.Verify().empty());
  std::printf("  stress: %u commits, %u evictions, %u placement failures, peak %.1f of %.0f MiB\n", commits, evictions, placement_failures, double(book.allocator().stats().peak) / kMiB,
              double(book.allocator().capacity()) / kMiB);
}

// ---------------------------------------------------------------------------
// the streaming plan with a pool
// ---------------------------------------------------------------------------

std::vector<city_rt::SectorInfo> Grid(int n, float size, uint32_t hi, uint32_t lod, bool pooled) {
  std::vector<city_rt::SectorInfo> sectors;
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      city_rt::SectorInfo s;
      s.box_min[0] = (float(x) - float(n) / 2) * size;
      s.box_max[0] = s.box_min[0] + size;
      s.box_min[2] = (float(z) - float(n) / 2) * size;
      s.box_max[2] = s.box_min[2] + size;
      s.box_min[1] = -10;
      s.box_max[1] = 60;
      s.hi_triangles = hi;
      s.lod_triangles = lod;
      if (pooled) {
        s.hi_pool = EstimateBlockBytes(hi * 3 / 5, hi);
        s.lod_pool = EstimateBlockBytes(lod * 3 / 5, lod);
      }
      sectors.push_back(s);
    }
  }
  return sectors;
}

// A stand-in for the scene's streaming with the pool: the plan's loads arrive after `latency` plans, are built (a block placed in the book, committed with its estimate's size) and the
// plan's evictions give back the structure at once and the pool's range after `free_delay` plans.
struct Sim {
  std::vector<city_rt::SectorInfo> sectors;
  std::vector<city_rt::SectorState> states;
  city_rt::Budget budget;
  PoolBook book;
  int latency = 1, free_delay = 2;
  uint32_t loading = 0;
  struct Load { city_rt::Request r; int left; };
  std::vector<Load> loads;
  struct Free { uint64_t generation, base, bytes; int left; };
  std::vector<Free> frees;
  uint64_t frame = 0;
  uint32_t placement_failures = 0, over_pool_plans = 0, loads_total = 0, evictions_total = 0;
  uint64_t max_committed = 0, max_state_pool = 0;
  std::vector<cf::PoolBlockPlan> plans;  // by slot

  Sim(std::vector<city_rt::SectorInfo> s, city_rt::Budget b, uint64_t capacity) : sectors(std::move(s)), states(sectors.size()), budget(b) {
    book.Reset(capacity, uint32_t(sectors.size()));
  }
  uint64_t PoolOf(const city_rt::SectorInfo& s, city_rt::Detail d) const { return d == city_rt::Detail::kHi ? s.hi_pool : s.lod_pool; }

  city_rt::Plan Step(double x, double z) {
    ++frame;
    for (auto it = loads.begin(); it != loads.end();) {
      if (--it->left > 0) {
        ++it;
        continue;
      }
      const city_rt::Request r = it->r;
      it = loads.erase(it);
      --loading;
      city_rt::SectorState& st = states[r.sector];
      const bool hi = r.detail == city_rt::Detail::kHi;
      if ((hi ? st.hi : st.lod) != city_rt::Residency::kLoading) continue;
      const city_rt::SectorInfo& si = sectors[r.sector];
      const uint32_t triangles = hi ? si.hi_triangles : si.lod_triangles;
      const cf::PoolBlockPlan plan = cf::PlanPoolBlock(triangles * 3 / 5, triangles, 600, true, true);
      uint64_t base = 0;
      if (budget.pool_cap != 0 && !book.Place(r.sector, !hi, plan.total_bytes, &base)) {
        ++placement_failures;
        (hi ? st.hi : st.lod) = city_rt::Residency::kFailed;
        (hi ? st.hi_retry_frame : st.lod_retry_frame) = frame + kPoolRetryFrames;
        (hi ? st.hi_pool_held : st.lod_pool_held) = 0;
        continue;
      }
      if (budget.pool_cap != 0) {
        const bool ok = book.Commit(r.sector, !hi, plan, triangles, triangles / 2, 0, 0, 0);
        CHECK(ok);
        (hi ? st.hi_pool_held : st.lod_pool_held) = PoolAllocator::Round(plan.total_bytes);
      }
      (hi ? st.hi : st.lod) = city_rt::Residency::kResident;
      (hi ? st.hi_bytes : st.lod_bytes) = city_rt::EstimateStructureBytes(triangles);
    }
    for (auto it = frees.begin(); it != frees.end();) {
      if (--it->left <= 0) {
        book.FreeRange(it->generation, it->base, it->bytes);
        it = frees.erase(it);
      } else {
        ++it;
      }
    }
    city_rt::PlanInput in;
    in.camera[0] = x;
    in.camera[2] = z;
    in.frame = frame;
    in.budget = budget;
    in.loading = loading;
    const city_rt::Plan plan = city_rt::PlanStreaming(in, sectors, states);
    for (const city_rt::Request& e : plan.evictions) {
      city_rt::SectorState& st = states[e.sector];
      const bool hi = e.detail == city_rt::Detail::kHi;
      (hi ? st.hi : st.lod) = city_rt::Residency::kNone;
      (hi ? st.hi_bytes : st.lod_bytes) = 0;
      (hi ? st.hi_pool_held : st.lod_pool_held) = 0;
      const Block b = book.Evict(e.sector, !hi);
      if (b.present) frees.push_back({book.generation(), b.base, b.bytes, free_delay});
      ++evictions_total;
    }
    for (const city_rt::Request& r : plan.loads) {
      city_rt::SectorState& st = states[r.sector];
      const bool hi = r.detail == city_rt::Detail::kHi;
      (hi ? st.hi : st.lod) = city_rt::Residency::kLoading;
      (hi ? st.hi_bytes : st.lod_bytes) = city_rt::EstimateStructureBytes(hi ? sectors[r.sector].hi_triangles : sectors[r.sector].lod_triangles);
      (hi ? st.hi_pool_held : st.lod_pool_held) = budget.pool_cap != 0 ? PoolOf(sectors[r.sector], r.detail) : 0;
      loads.push_back({r, latency});
      ++loading;
      ++loads_total;
    }
    over_pool_plans += plan.over_pool_cap ? 1 : 0;
    max_committed = std::max(max_committed, book.committed_bytes());
    uint64_t held = 0;
    for (const city_rt::SectorState& st : states) held += st.hi_pool_held + st.lod_pool_held;
    max_state_pool = std::max(max_state_pool, held);
    return plan;
  }
};

void TestPlanWithPool() {
  using city_rt::Level;
  // 1. No pool cap: the plan is the plan it was, whatever the sectors' pool estimates say
  for (const int n : {8, 12}) {
    const city_rt::Budget b = city_rt::BudgetFor(GpuClass::kHigh, Level::kHigh);
    Sim a(Grid(n, 60.0f, 20000, 800, false), b, 0), c(Grid(n, 60.0f, 20000, 800, true), b, 0);
    bool same = true;
    for (int step = 0; step < 80; ++step) {
      const double x = -200 + step * 5.0, z = 10;
      const city_rt::Plan pa = a.Step(x, z), pc = c.Step(x, z);
      if (pa.loads.size() != pc.loads.size() || pa.evictions.size() != pc.evictions.size() || pa.resident_bytes != pc.resident_bytes || pa.over_cap != pc.over_cap || pc.resident_pool_bytes != 0 ||
          pc.over_pool_cap) {
        same = false;
      }
      for (size_t i = 0; i < pa.loads.size() && i < pc.loads.size(); ++i) same = same && pa.loads[i] == pc.loads[i];
    }
    CHECK(same);
  }

  // 2. A pool that is the short one: structures have room for everything, the pool does not
  city_rt::Budget roomy = city_rt::BudgetFor(GpuClass::kHigh, Level::kHigh);
  roomy.memory_cap = 64ull << 30;                                   // the structures' memory is not the limit
  const uint64_t sector_pool = EstimateBlockBytes(20000 * 3 / 5, 20000);
  roomy.pool_cap = sector_pool * 5;                                 // room for five hi sectors (and a few lods)
  Sim sim(Grid(10, 60.0f, 20000, 800, true), roomy, sector_pool * 6);
  uint64_t over = 0;
  for (int step = 0; step < 60; ++step) {
    const city_rt::Plan plan = sim.Step(0.0, 0.0);
    over += plan.over_pool_cap ? 1 : 0;
    // the pool is never planned past its cap
    CHECK(sim.max_state_pool <= roomy.pool_cap);
  }
  CHECK(over > 0);                            // the plan said the pool was in the way
  CHECK(sim.max_committed <= roomy.pool_cap);
  CHECK(sim.placement_failures == 0);         // and every block that was planned found its range (a tenth of the pool is the headroom)
  CHECK(sim.book.Verify().empty());

  // 3. The farthest block is traded for a nearer one: drive toward a far corner with a pool for few sectors; the nearest sectors win the room
  city_rt::Budget tight = roomy;
  tight.pool_cap = sector_pool * 3 + 144 * EstimateBlockBytes(800 * 3 / 5, 800);  // every lod (the safety net, never traded) and three hi sectors
  Sim drive(Grid(12, 60.0f, 20000, 800, true), tight, tight.pool_cap + sector_pool);
  for (int step = 0; step < 200; ++step) {
    drive.Step(-300.0 + step * 3.0, 0.0);
    CHECK(drive.max_state_pool <= tight.pool_cap);
  }
  std::printf("  drive: %u loads, %u evictions, placement failures %u, over-pool plans %u\n", drive.loads_total, drive.evictions_total, drive.placement_failures, drive.over_pool_plans);
  CHECK(drive.evictions_total > 5 && drive.loads_total > 8);
  CHECK(drive.book.Verify().empty());
  // at the end the camera is at x = 297: the sectors held are the ones nearest it
  double worst_held = 0.0;
  for (uint32_t i = 0; i < drive.sectors.size(); ++i) {
    if (drive.states[i].hi == city_rt::Residency::kResident) {
      worst_held = std::max<double>(worst_held, city_rt::DistanceToBox(std::array<double, 3>{297.0, 0.0, 0.0}.data(), drive.sectors[i].box_min, drive.sectors[i].box_max));
    }
  }
  CHECK(worst_held < 150.0);

  // 4. Count the held pool blocks independently of how the resident BLASes are charged.
  city_rt::PlanInput in;
  in.camera[0] = 0.0;
  in.camera[2] = 0.0;
  in.budget = roomy;
  const std::vector<city_rt::SectorInfo> sectors = Grid(4, 60.0f, 20000, 800, true);
  std::vector<city_rt::SectorState> states(sectors.size());
  for (city_rt::SectorState& st : states) {
    st.hi = city_rt::Residency::kResident;
    st.hi_bytes = 1000;
    st.hi_pool_held = 4096;
  }
  const city_rt::Plan fixed = city_rt::PlanStreaming(in, sectors, states);
  CHECK(fixed.resident_pool_bytes == 4096u * 16u && fixed.resident_bytes == 16u * 20000u * 96u);
  in.fixed_selection = false;
  const city_rt::Plan legacy = city_rt::PlanStreaming(in, sectors, states);
  CHECK(legacy.resident_pool_bytes == 4096u * 16u && legacy.resident_bytes == 16000u);
  in.budget.pool_cap = 0;  // without the cap the pool is not counted at all
  CHECK(city_rt::PlanStreaming(in, sectors, states).resident_pool_bytes == 0);
}

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

// The checksum, written the plain way.
uint32_t PlainChecksum(const std::vector<uint8_t>& bytes) {
  const size_t words = bytes.size() / 4;
  std::vector<uint32_t> w(words);
  std::memcpy(w.data(), bytes.data(), words * 4);
  uint32_t acc = 0;
  std::vector<size_t> at;
  for (size_t i = 0; i < words; i += 61) at.push_back(i);
  if (words != 0 && at.back() != words - 1) at.push_back(words - 1);
  for (const size_t i : at) acc = acc * 31u + w[i] + uint32_t(i);
  return acc;
}

void TestProbe() {
  Lcg rng;
  for (const size_t size : {size_t(0), size_t(4), size_t(16), size_t(244), size_t(248), size_t(1000), size_t(65536), size_t(1 << 20) + 16}) {
    std::vector<uint8_t> bytes(size);
    for (uint8_t& b : bytes) b = uint8_t(rng.Next());
    CHECK(ProbeChecksum(bytes.data(), bytes.size()) == PlainChecksum(bytes));
    const size_t words = size / 4;
    size_t samples = 0;
    for (size_t i = 0; i < words; i += 61) ++samples;
    if (words != 0 && (words - 1) % 61 != 0) ++samples;
    CHECK(ProbeSamples(size) == samples);
  }
  // one changed sampled word changes it; a word that is never sampled does not (the probe is a sample, the unit test of the copy is the byte compare in the run)
  std::vector<uint8_t> bytes(4096);
  for (uint8_t& b : bytes) b = uint8_t(rng.Next());
  const uint32_t base = ProbeChecksum(bytes.data(), bytes.size());
  std::vector<uint8_t> changed = bytes;
  changed[61 * 4] ^= 0x10;
  CHECK(ProbeChecksum(changed.data(), changed.size()) != base);
  changed = bytes;
  changed[bytes.size() - 1] ^= 0x01;  // the last word is always sampled
  CHECK(ProbeChecksum(changed.data(), changed.size()) != base);
  changed = bytes;
  changed[4] ^= 0x01;  // word 1
  CHECK(ProbeChecksum(changed.data(), changed.size()) == base);
  // the position matters: two words swapped
  changed = bytes;
  std::swap_ranges(changed.begin(), changed.begin() + 4, changed.begin() + 61 * 4);
  CHECK(ProbeChecksum(changed.data(), changed.size()) != base || std::equal(changed.begin(), changed.end(), bytes.begin()));

  // the verdicts
  const uint32_t slots = 6;
  std::vector<uint32_t> words(kProbeHeaderWords + slots * kProbeSlotWords, 0);
  words[0] = kProbeMagic;
  words[1] = 0xABCD;
  words[2] = 0xFFFFFFFFu;
  words[3] = slots;
  std::vector<ProbeBlock> expected = {{1, 111, 10, 7, 40}, {2, 222, 20, 3, 50}, {4, 444, 30, 7, 60}, {5, 555, 40, 7, 70}};
  const auto put = [&](uint32_t slot, uint32_t checksum, uint32_t samples, uint32_t flags, uint32_t triangles) {
    uint32_t* r = &words[kProbeHeaderWords + slot * kProbeSlotWords];
    r[0] = checksum;
    r[1] = samples;
    r[2] = flags;
    r[3] = triangles;
  };
  put(1, 111, 10, 7, 40);
  put(2, 222, 20, 3, 50);
  put(4, 999, 30, 7, 60);  // the bytes differ
  put(5, 555, 40, 5, 70);  // the row differs
  const auto current = [](uint32_t slot, uint32_t) { return slot != 2; };  // slot 2 was evicted meanwhile
  ProbeVerdict v = JudgeProbe(words.data(), words.size(), slots, 0xABCD, 0xFFFFFFFFu, expected, current);
  CHECK(v.header_ok && v.checked == 3 && v.good == 1 && v.bad_checksum == 1 && v.bad_row == 1 && v.stale_skipped == 1);
  CHECK(v.first_problem.find("slot 4") != std::string::npos);
  v = JudgeProbe(words.data(), words.size(), slots, 0x1111, 0xFFFFFFFFu, expected, current);
  CHECK(!v.header_ok && !v.first_problem.empty());
  words[0] = 0;
  v = JudgeProbe(words.data(), words.size(), slots, 0xABCD, 0xFFFFFFFFu, expected, current);
  CHECK(v.checked == 0 && !v.header_ok && v.first_problem.find("did not run") != std::string::npos);
  CHECK(ProbeBufferBytes(878) == (4 + 878 * 4) * 4);

  // A block's probe: the regions the sector row sizes, in order, the checksum running on; a byte outside them is not seen, a byte inside is
  for (const bool runs : {false, true}) {
    for (const bool corners : {false, true}) {
      if (corners && !runs) continue;
      Synth s = MakeSynth(rng, 700, 300, runs, corners);
      uint64_t samples = 0;
      const uint32_t sum = ProbeOfBlock(s.bytes.data(), s.plan, 700, &samples);
      // the same by hand
      uint32_t acc = ProbeRegion(0, s.bytes.data() + s.plan.index_offset, 700 * 12);
      uint64_t n = ProbeSamples(700 * 12);
      acc = ProbeRegion(acc, s.bytes.data() + s.plan.position_offset, kProbePositionBytes);
      n += ProbeSamples(kProbePositionBytes);
      if (runs) {
        acc = ProbeRegion(acc, s.bytes.data() + s.plan.run_id_offset, 350 * 4);
        acc = ProbeRegion(acc, s.bytes.data() + s.plan.run_table_offset, 32);
        n += ProbeSamples(350 * 4) + ProbeSamples(32);
      }
      if (corners) {
        acc = ProbeRegion(acc, s.bytes.data() + s.plan.attribute_offset, 700 * 24);
        n += ProbeSamples(700 * 24);
      }
      CHECK(sum == acc && samples == n);
      // a change in the first index is seen; a change in the 4th vertex (beyond the 36 bytes) is not: the build's own check (the primary rays against the depth) covers the rest
      Synth t = s;
      t.bytes[s.plan.index_offset] ^= 0x01;
      CHECK(ProbeOfBlock(t.bytes.data(), t.plan, 700, nullptr) != sum);
      t = s;
      t.bytes[s.plan.position_offset + 40] ^= 0x01;
      CHECK(ProbeOfBlock(t.bytes.data(), t.plan, 700, nullptr) == sum);
      if (corners) {
        t = s;
        t.bytes[s.plan.attribute_offset + 700 * 24 - 1] ^= 0x01;  // the last corner byte: the last word of the region is always sampled
        CHECK(ProbeOfBlock(t.bytes.data(), t.plan, 700, nullptr) != sum);
      }
    }
  }
}

// The shader's own numbers are this header's (given the src/gpu folder).
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void TestShaderNumbers(const std::string& gpu_dir) {
  const std::string text = ReadFile(gpu_dir + "/hlsl/city_hit_probe_cs.hlsl");
  if (text.empty()) {
    std::printf("  (hlsl/city_hit_probe_cs.hlsl not found under %s: the shader's numbers were not checked)\n", gpu_dir.c_str());
    return;
  }
  const auto has = [&](const std::string& s) { return text.find(s) != std::string::npos; };
  CHECK(has("#define PROBE_STRIDE_WORDS " + std::to_string(kProbeStrideWords) + "u"));
  CHECK(has("#define PROBE_MAGIC 0x" + [] {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", kProbeMagic);
    return std::string(buf);
  }() + "u"));
  CHECK(has("#define PROBE_HEADER_WORDS " + std::to_string(kProbeHeaderWords) + "u"));
  CHECK(has("#define PROBE_SLOT_WORDS " + std::to_string(kProbeSlotWords) + "u"));
  CHECK(has("acc * 31u"));
  CHECK(has("gCityPool.Load("));
  CHECK(has("#define CITY_HIT_SPACE 2") && has("#include \"city_hit_common.hlsli\"") && has("#define PROBE_POSITION_BYTES " + std::to_string(kProbePositionBytes) + "u"));  // t6..t9 come from the shared decode header
  CHECK(has("gCityMaterials[0].x") && has("gCityTextures[0].x") && has("CityHitLoadSector(slot)"));
}

// ---------------------------------------------------------------------------
// the whole chain: blocks -> the book -> the pool bytes -> the sector table -> the decode
// ---------------------------------------------------------------------------

void TestChain() {
  Lcg rng;
  PoolBook book;
  const uint32_t sector_count = 8;
  CHECK(book.Reset(4 * kMiB, sector_count));
  std::vector<uint8_t> pool(size_t(4 * kMiB), 0xA5);
  struct Placed { Synth s; uint32_t sector; bool lod; uint64_t base; };
  std::vector<Placed> placed;
  for (uint32_t sector = 0; sector < sector_count; ++sector) {
    for (const bool lod : {false, true}) {
      if (sector == 5 && !lod) continue;  // never built
      const uint32_t triangles = 60 + rng.Below(900);
      const bool runs = !(sector == 2 && !lod);                    // geometry only
      const bool corners = runs && !(sector == 1 && !lod);         // runs without corners
      Placed p{MakeSynth(rng, triangles, (sector + (lod ? 1u : 0u)) % 3 == 0 ? triangles : triangles / 2, runs, corners), sector, lod, 0};
      CHECK(book.Place(sector, lod, p.s.plan.total_bytes, &p.base));
      std::memcpy(pool.data() + p.base, p.s.bytes.data(), p.s.bytes.size());
      uint64_t samples = 0;
      const uint32_t checksum = ProbeOfBlock(p.s.bytes.data(), p.s.plan, triangles, &samples);
      CHECK(book.Commit(sector, lod, p.s.plan, triangles, p.s.mesh.opaque_triangles, 0, checksum, uint32_t(samples)));
      placed.push_back(std::move(p));
    }
  }
  CHECK(book.Verify().empty() && book.table().valid_count() == placed.size());

  // the decode, through the table the GPU would read (two rows a slot = the book's rows as they are)
  uint32_t material_words[8] = {};
  cf::HitView view;
  view.pool = pool.data();
  view.pool_size = pool.size();
  view.sectors = static_cast<const uint32_t*>(book.table().data());
  view.sector_slots = book.table().slots();
  view.materials = material_words;
  view.material_count = 1;
  uint32_t decoded = 0, matched = 0;
  for (const Placed& p : placed) {
    const uint32_t instance = city_rt::InstanceId(p.sector, p.lod ? city_rt::Detail::kLod : city_rt::Detail::kHi);
    for (uint32_t t = 0; t < p.s.mesh.triangle_count(); t += 1 + rng.Below(40)) {
      const bool alpha_geometry = t >= p.s.mesh.opaque_triangles && p.s.mesh.opaque_triangles != 0;
      const uint32_t geometry = alpha_geometry ? 1 : 0;
      const uint32_t primitive = alpha_geometry ? t - p.s.mesh.opaque_triangles : t;
      const cf::DecodedHit h = cf::DecodeHit(view, instance, geometry, primitive, 0.25f, 0.5f);
      ++decoded;
      if (!h.valid || h.triangle != t) continue;
      bool same = true;
      for (int k = 0; k < 3; ++k) {
        const uint32_t vi = p.s.mesh.indices[size_t(t) * 3 + size_t(k)];
        for (int c = 0; c < 3; ++c) same = same && h.p[k][c] == p.s.mesh.positions[size_t(vi) * 3 + size_t(c)];
      }
      matched += same ? 1 : 0;
    }
  }
  CHECK(decoded > 100 && matched == decoded);  // every triangle that was asked for came back with the positions that went in

  // a slot that is not built decodes as nothing, and so does one that was evicted
  const uint32_t unbuilt = city_rt::InstanceId(5, city_rt::Detail::kHi);
  CHECK(!cf::DecodeHit(view, unbuilt, 0, 0, 0.3f, 0.3f).valid);
  const Placed& victim = placed.front();
  const Block gone = book.Evict(victim.sector, victim.lod);
  CHECK(gone.present);
  CHECK(!cf::DecodeHit(view, city_rt::InstanceId(victim.sector, victim.lod ? city_rt::Detail::kLod : city_rt::Detail::kHi), 0, 0, 0.3f, 0.3f).valid);
  // the others are untouched by it
  const Placed& keep = placed.back();
  CHECK(cf::DecodeHit(view, city_rt::InstanceId(keep.sector, keep.lod ? city_rt::Detail::kLod : city_rt::Detail::kHi), 0, 0, 0.3f, 0.3f).valid);

  // the probe's expectations are the book's, and the checksum the host made is the checksum of the bytes in the pool
  for (const ProbeBlock& e : book.ProbeList()) {
    const Block& b = book.block_at_slot(e.slot);
    // the checksum was made over the block's plan bytes (total_bytes), the block's range is rounded to 16: the same bytes (total_bytes is a multiple of 16)
    // the checksum the host made over the staging bytes is the one over the bytes in the pool, region by region (what the shader reads: the sector row has no vertex or run counts)
    const Placed* owner = nullptr;
    for (const Placed& p : placed) owner = cf::SectorRowOf(p.sector, p.lod) == e.slot ? &p : owner;
    CHECK(owner != nullptr);
    uint64_t samples = 0;
    CHECK(owner != nullptr && e.checksum == ProbeOfBlock(pool.data() + b.base, owner->s.plan, owner->s.mesh.triangle_count(), &samples) && e.samples == samples);
  }
}

}  // namespace

int main(int argc, char** argv) {
  TestBudgets();
  TestFit();
  TestAllocator();
  TestEstimate();
  TestSectorTable();
  TestBook();
  TestBookStress();
  TestPlanWithPool();
  TestProbe();
  TestChain();
  std::string gpu_dir;
  if (argc > 1) gpu_dir = argv[1];
  if (!gpu_dir.empty()) TestShaderNumbers(gpu_dir);
  if (g_failures != 0) {
    std::fprintf(stderr, "city_hit_pool_rules_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("city_hit_pool_rules_test: %d checks passed\n", g_checks);
  return 0;
}
