#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's hit data in the GPU: the pool, the sector table, the eviction and the memory it holds (CITY-RT M2 package H3)
// ===========================================================================
// src/citypack/city_hit_format.h froze WHAT the GPU holds for a ray that hits the city: one pool buffer (gCityPool, t6) with a block for every sector detail's per-triangle data
// (indices, positions, run ids, the run table, the corners), the sector table (gCitySectors, t7: two rows a slot, slot = InstanceID & 0x7FFFFF), the material table (t8) and the
// texture table (t9). This header is HOW it is kept, as pure functions and classes on plain numbers, so that city_hit_pool_rules_test.cpp builds on its own:
//
//   PoolBudget / PoolBudgetFor / WithPool   the pool's size and the staging a frame by graphics class and level (64 / 128 / 256 MiB: city_hit_format.h HitBudget::pool_bytes);
//                                           the pool's cap in the streaming plan (city_rt_rules.h Budget::pool_cap), so the structures' memory and the pool are planned together
//   EstimateBlockBytes                      a sector detail's block from the base pack's record (the plan wants a number before the hit blob is read)
//   FitCapacity                             the pool the video memory leaves room for: the class's size, else half of it, down to 32 MiB, else none (the level steps down)
//   PoolAllocator                           first fit with coalescing, 16 byte aligned (city_hit_format.h kPoolAlignment), 32 bit offsets (kMaxPoolBytes)
//   SectorTable                             the 2 x sector_count rows the GPU reads, with the block each valid row names and a check that no two overlap
//   PoolBook                                allocator + table + the blocks together: Place, Commit, Evict (the row is cleared at once; the range is given back by FreeRange once the
//                                           GPU is done with the frames that could read it), the ledger of what is held
//   TextureTable                            the CPU side of t9 (H2 writes a row when a texture is uploaded); a version number says when the GPU copy is stale
//   ProbeChecksum / the probe's layout      the pool's check: a sampled checksum of each block, made on the CPU when the block is filled and on the GPU by a compute pass that reads
//                                           the pool through t6 and t7 (hlsl/city_hit_probe_cs.hlsl); the two must agree
//
// THE LIFE OF A BLOCK (city_rt_scene.cpp BuildStaged, the device side of this): the worker reads the sector detail's base mesh and its hit blob; at the history trigger PlanPoolBlock
// sizes the block, Place gives it a range, FillPoolBlock writes the bytes into staging, a copy takes them to the pool, the structure is BUILT OVER the block's own positions and
// indices (the pool is the build's input: nothing is uploaded twice and nothing is given back after the build, as M1 did), Commit makes the sector table's row valid. Eviction: Evict
// clears the row (the next frame's table copy says "no hit data here"), the structure goes to the FrameRing's retirement, and the range comes back through FrameRing::RetireAction so
// that no frame in flight can read bytes a new block is being copied over.
//
// This header includes citypack/city_hit_format.h (the block plan and the row), city_rt_rules.h, car_rt_scene_rules.h (the range allocator), gpu_class.h and vram_room.h; nothing of
// the renderer. The test links src/citypack/city_hit_format.cpp and pack_format.cpp.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "car_rt_scene_rules.h"
#include "citypack/city_hit_format.h"
#include "city_rt_rules.h"
#include "gpu_class.h"
#include "vram_room.h"

namespace mcla::gpu::city_hit {

namespace cf = larecomp::citypack;
using Level = city_rt::Level;

inline constexpr uint64_t kMiB = 1ull << 20;

// ---------------------------------------------------------------------------
// the budget
// ---------------------------------------------------------------------------

struct PoolBudget {
  uint64_t capacity = 0;      // the pool buffer's size, bytes (0: no hit data)
  uint32_t staged_bytes = 0;  // bytes staged in one frame for builds WITH hit data (the pool block is the whole upload: indices, positions and attributes), city_rt_rules.h Budget::upload_bytes
  bool on() const { return capacity != 0; }
};

// M2 section 8: hit pool 17-40 / 33-61 (+17 for the far lod) / 88-133 (+17) MB of data at the three levels, staged 9 / 17 / 20 MiB a frame; the buffers are the sizes the format header
// names, "64 to 256 MiB": the data at p90 is under half of each, the rest is room for the ring's movement and for fragmentation.
constexpr PoolBudget PoolBudgetFor(gpu_class::GpuClass, Level level) {
  if (level == Level::kOff) return {};
  return level == Level::kUltra ? PoolBudget{256 * kMiB, uint32_t(20 * kMiB)} : PoolBudget{128 * kMiB, uint32_t(17 * kMiB)};
}

// The share of the pool the streaming plan may fill: a first fit allocator over blocks of 0.3 to 12 MiB keeps its holes small, and the tenth that is left is what a hole the next block
// does not fit costs. The plan counts bytes; an allocation that still fails is retried (kPoolRetryFrames).
inline constexpr uint64_t kPlanSharePercent = 90;
constexpr uint64_t PlanCap(uint64_t capacity) { return capacity * kPlanSharePercent / 100 / cf::kPoolAlignment * cf::kPoolAlignment; }

// The city's budget with hit data: the pool's cap in the plan, and staging big enough for the blocks.
constexpr city_rt::Budget WithPool(city_rt::Budget budget, const PoolBudget& pool) {
  if (!budget.on() || !pool.on()) return budget;
  budget.pool_cap = PlanCap(pool.capacity);
  budget.upload_bytes = std::max<uint64_t>(budget.upload_bytes, pool.staged_bytes);
  return budget;
}

// H0's HitBudget fields this package owns.
constexpr cf::HitBudget HitBudgetFor(gpu_class::GpuClass cls, Level level) {
  cf::HitBudget h;
  const PoolBudget pool = PoolBudgetFor(cls, level);
  h.pool_bytes = pool.capacity;
  h.staged_bytes = pool.staged_bytes;
  return h;
}

// A pool block of a sector detail before its hit blob is read: the plan's number. The run table is the part that is not known (the blob's runs); 1024 runs is 32 KiB, which is
// small beside the 12 bytes of a triangle's indices and the 24 of its corners, and the real block is exact (PlanPoolBlock on the blob).
inline constexpr uint32_t kEstimatedRunsPerBlock = 1024;
inline uint64_t EstimateBlockBytes(uint32_t vertices, uint32_t triangles) { return cf::PlanPoolBlock(vertices, triangles, kEstimatedRunsPerBlock, true, true).total_bytes; }

// A block that could not be placed (no free range that long) is tried again after this many frames: the ranges eviction gave back become free one frame later (FrameRing), and the
// plan has already counted them as free.
inline constexpr uint64_t kPoolRetryFrames = 8;
// A hit file that is missing, stale or does not belong to the pack is looked for again after this many frames (a minute): the hit data is built by hand until the launcher does it.
inline constexpr uint64_t kFileRetryFrames = 3600;

// ---------------------------------------------------------------------------
// the memory the pool may take
// ---------------------------------------------------------------------------

inline constexpr uint64_t kMinPoolBytes = 32 * kMiB;

// The pool size the video memory has room for, from the class's `wanted`: the same size when `used + wanted` stays under vram_room::kHighPercent of the budget, else half, down to
// kMinPoolBytes, else 0 (no hit data this time: the level steps down to none, the structures and shadows go on). A budget of 0 (an adapter that reports none) fits anything: the
// creation itself is the judge then.
inline uint64_t FitCapacity(uint64_t wanted, uint64_t vram_used, uint64_t vram_budget) {
  for (uint64_t capacity = wanted; capacity >= kMinPoolBytes; capacity /= 2) {
    if (vram_room::HasRoom(vram_used, vram_budget, capacity)) return capacity;
  }
  return 0;
}

// What the hit data holds in video memory (the renderer's census line and VideoMemoryBudget's holder).
struct Held {
  uint64_t pool = 0;       // the pool buffer
  uint64_t materials = 0;  // the material table
  uint64_t tables = 0;     // the sector and texture tables, every frame slot's copy, and the stand-in buffers
  uint64_t textures = 0;   // the resident city textures (city_texture_set.h), by the allocator's rule
  uint64_t Total() const { return pool + materials + tables + textures; }
};

// ---------------------------------------------------------------------------
// the allocator
// ---------------------------------------------------------------------------

// First fit with coalescing over the pool's bytes, every range 16 aligned and a multiple of 16 long (a sector's arrays all start on 16, city_hit_format.h PlanPoolBlock).
class PoolAllocator {
 public:
  struct Stats {
    uint64_t capacity = 0;
    uint64_t used = 0;
    uint64_t peak = 0;
    uint64_t largest_free = 0;
    uint64_t allocations = 0;
    uint64_t frees = 0;
    uint64_t failures = 0;
    size_t free_ranges = 0;
  };

  // False (nothing changes) for a capacity of 0, beyond kMaxPoolBytes, or that is not a whole number of 16 byte units. Everything given out so far is forgotten.
  bool Reset(uint64_t capacity) {
    if (capacity == 0 || capacity > cf::kMaxPoolBytes || capacity % cf::kPoolAlignment != 0) return false;
    ranges_.Reset(capacity);
    stats_ = Stats{};
    stats_.capacity = capacity;
    return true;
  }
  // `bytes` rounded up to 16. False: no free range that long (`largest_free` says how far off).
  bool Alloc(uint64_t bytes, uint64_t* base) {
    const uint64_t size = Round(bytes);
    uint64_t at = 0;
    if (size == 0 || !ranges_.Alloc(size, cf::kPoolAlignment, &at)) {
      ++stats_.failures;
      return false;
    }
    *base = at;
    stats_.used += size;
    stats_.peak = std::max(stats_.peak, stats_.used);
    ++stats_.allocations;
    return true;
  }
  void Free(uint64_t base, uint64_t bytes) {
    const uint64_t size = Round(bytes);
    if (size == 0 || base + size > stats_.capacity) return;
    ranges_.Free(base, size);
    stats_.used -= std::min(stats_.used, size);
    ++stats_.frees;
  }
  uint64_t capacity() const { return stats_.capacity; }
  uint64_t used() const { return stats_.used; }
  uint64_t free_bytes() const { return stats_.capacity - std::min(stats_.capacity, stats_.used); }
  Stats stats() const {
    Stats s = stats_;
    s.largest_free = ranges_.largest_free();
    s.free_ranges = ranges_.free_ranges();
    return s;
  }
  // How broken up the free space is: 0 when it is one range, toward 100 as the largest free range is a smaller part of the free bytes.
  uint32_t FragmentationPercent() const {
    const uint64_t free_total = free_bytes();
    if (free_total == 0) return 0;
    return uint32_t(100 - ranges_.largest_free() * 100 / free_total);
  }
  static uint64_t Round(uint64_t bytes) { return (bytes + cf::kPoolAlignment - 1) / cf::kPoolAlignment * cf::kPoolAlignment; }

 private:
  car_rt_rules::RangeAllocator ranges_;
  Stats stats_;
};

// ---------------------------------------------------------------------------
// the sector table
// ---------------------------------------------------------------------------

// Two rows a slot, 2 x sector_count slots, slot = (sector << 1) | detail (city_hit_format.h SectorRowOf). A slot whose flags are 0 is a structure that is not built or has no hit data:
// the shader's `valid` test. The table keeps beside each valid row the extent of the block it names, for Verify.
class SectorTable {
 public:
  void Reset(uint32_t sector_count) {
    rows_.assign(size_t(sector_count) * 2, cf::GpuSectorRow{});
    extents_.assign(rows_.size(), Extent{});
    valid_ = 0;
    ++version_;
  }
  uint32_t slots() const { return uint32_t(rows_.size()); }
  uint32_t sector_count() const { return uint32_t(rows_.size() / 2); }
  // False: the slot is outside the table, the row is not valid, or the block is not 16 aligned.
  bool Set(uint32_t sector, bool lod, const cf::GpuSectorRow& row, uint64_t base, uint64_t bytes) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    if (slot >= rows_.size() || (row.flags & cf::kSectorRowValid) == 0 || base % cf::kPoolAlignment != 0 || bytes == 0) return false;
    if (rows_[slot].flags & cf::kSectorRowValid) --valid_;
    rows_[slot] = row;
    extents_[slot] = Extent{base, bytes};
    ++valid_;
    ++version_;
    return true;
  }
  // False: nothing was there.
  bool Clear(uint32_t sector, bool lod) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    if (slot >= rows_.size() || (rows_[slot].flags & cf::kSectorRowValid) == 0) return false;
    rows_[slot] = cf::GpuSectorRow{};
    extents_[slot] = Extent{};
    --valid_;
    ++version_;
    return true;
  }
  void ClearAll() {
    for (uint32_t i = 0; i < rows_.size(); ++i) {
      rows_[i] = cf::GpuSectorRow{};
      extents_[i] = Extent{};
    }
    valid_ = 0;
    ++version_;
  }
  const cf::GpuSectorRow& Row(uint32_t sector, bool lod) const { return rows_[cf::SectorRowOf(sector, lod)]; }
  const cf::GpuSectorRow& RowAtSlot(uint32_t slot) const { return rows_[slot]; }
  bool valid(uint32_t sector, bool lod) const { return (rows_[cf::SectorRowOf(sector, lod)].flags & cf::kSectorRowValid) != 0; }
  uint64_t block_base(uint32_t slot) const { return extents_[slot].base; }
  uint64_t block_bytes(uint32_t slot) const { return extents_[slot].bytes; }
  uint32_t valid_count() const { return valid_; }
  // Changes with every Set and Clear: a frame slot's GPU copy whose version is older is stale.
  uint64_t version() const { return version_; }
  const void* data() const { return rows_.data(); }
  size_t bytes() const { return rows_.size() * sizeof(cf::GpuSectorRow); }

  // "" when every valid row is sound: its block inside the pool, 16 aligned, its five offsets inside the block and 16 aligned (the ones its flags say are there), no two blocks
  // overlapping. Else the first problem, in words.
  std::string Verify(uint64_t pool_capacity) const {
    struct Span {
      uint64_t base, end;
      uint32_t slot;
    };
    std::vector<Span> spans;
    for (uint32_t slot = 0; slot < rows_.size(); ++slot) {
      const cf::GpuSectorRow& r = rows_[slot];
      const Extent& e = extents_[slot];
      if ((r.flags & cf::kSectorRowValid) == 0) {
        if (e.bytes != 0) return "slot " + std::to_string(slot) + ": a block is named by a row that is not valid";
        continue;
      }
      if (e.base % cf::kPoolAlignment != 0 || e.bytes == 0 || e.base + e.bytes > pool_capacity) return "slot " + std::to_string(slot) + ": the block is outside the pool or not aligned";
      const auto inside = [&](uint32_t offset) { return offset >= e.base && uint64_t(offset) <= e.base + e.bytes && offset % cf::kPoolAlignment == 0; };
      if (!inside(r.index_offset) || !inside(r.position_offset)) return "slot " + std::to_string(slot) + ": an index or position offset is outside its block";
      if ((r.flags & cf::kSectorRowRuns) && (!inside(r.run_id_offset) || !inside(r.run_table_offset))) return "slot " + std::to_string(slot) + ": a run offset is outside its block";
      if ((r.flags & cf::kSectorRowCorners) && !inside(r.attribute_offset)) return "slot " + std::to_string(slot) + ": the corner offset is outside its block";
      if ((r.flags & cf::kSectorRowCorners) && (r.flags & cf::kSectorRowRuns) == 0) return "slot " + std::to_string(slot) + ": corners without runs";
      if (r.opaque_triangles > r.triangle_count || r.triangle_count == 0) return "slot " + std::to_string(slot) + ": the triangle counts are wrong";
      spans.push_back({e.base, e.base + e.bytes, slot});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.base < b.base; });
    for (size_t i = 1; i < spans.size(); ++i) {
      if (spans[i].base < spans[i - 1].end) return "slots " + std::to_string(spans[i - 1].slot) + " and " + std::to_string(spans[i].slot) + ": their blocks overlap";
    }
    return {};
  }

 private:
  struct Extent {
    uint64_t base = 0;
    uint64_t bytes = 0;
  };
  std::vector<cf::GpuSectorRow> rows_;
  std::vector<Extent> extents_;
  uint32_t valid_ = 0;
  uint64_t version_ = 1;
};

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

// A sampled checksum of a REGION: every kProbeStrideWords-th 32 bit word (little endian, from the region's start) and the last word, acc = acc * 31 + word + index in the region.
// hlsl/city_hit_probe_cs.hlsl does the same loop over the pool through t6; the host makes this one over the staging bytes it copies. kProbeStrideWords is prime so that no array's stride
// hides in it.
inline constexpr uint32_t kProbeStrideWords = 61;
inline uint64_t ProbeSamples(uint64_t size) {
  const uint64_t words = size / 4;
  return words == 0 ? 0 : (words - 1) / kProbeStrideWords + 1 + ((words - 1) % kProbeStrideWords != 0 ? 1 : 0);
}
inline uint32_t ProbeRegion(uint32_t acc, const uint8_t* bytes, uint64_t size) {
  const uint64_t words = size / 4;
  const auto word = [&](uint64_t i) {
    uint32_t w;
    std::memcpy(&w, bytes + i * 4, 4);
    return w;
  };
  for (uint64_t i = 0; i < words; i += kProbeStrideWords) acc = acc * 31u + word(i) + uint32_t(i);
  if (words != 0 && (words - 1) % kProbeStrideWords != 0) acc = acc * 31u + word(words - 1) + uint32_t(words - 1);
  return acc;
}
inline uint32_t ProbeChecksum(const uint8_t* bytes, uint64_t size) { return ProbeRegion(0, bytes, size); }

// What the probe reads of a block: the regions whose size the SECTOR ROW tells the shader (the row has the triangle count but no vertex or run count): the indices (12 bytes a triangle),
// the first kProbePositionBytes of the positions (three vertices: a block has at least that), the run ids ((triangles + 1) / 2 words) and the first run record, and the corners (24 bytes a
// triangle), in that order, the checksum running on through them. `block` is the block's first byte (the plan's offsets are from it).
inline constexpr uint32_t kProbePositionBytes = 36;
inline uint32_t ProbeOfBlock(const uint8_t* block, const cf::PoolBlockPlan& plan, uint32_t triangles, uint64_t* samples) {
  uint32_t acc = 0;
  uint64_t n = 0;
  const auto region = [&](uint64_t offset, uint64_t size) {
    acc = ProbeRegion(acc, block + offset, size);
    n += ProbeSamples(size);
  };
  region(plan.index_offset, uint64_t(triangles) * 12);
  region(plan.position_offset, kProbePositionBytes);
  if (plan.has_runs) {
    region(plan.run_id_offset, (uint64_t(triangles) + 1) / 2 * 4);
    region(plan.run_table_offset, cf::kHitRunBytes);
  }
  if (plan.has_corners) region(plan.attribute_offset, uint64_t(triangles) * 24);
  if (samples) *samples = n;
  return acc;
}

// The probe's result buffer (u0 of its pass, uint32 words): a header, then a record for every sector table slot.
inline constexpr uint32_t kProbeMagic = 0x42525043;  // "CPRB"
inline constexpr uint32_t kProbeHeaderWords = 4;     // magic, gCityMaterials[0].x, gCityTextures[0].x, the slots the pass looked at
inline constexpr uint32_t kProbeSlotWords = 4;       // checksum, samples, the row's flags, the row's triangle count
inline constexpr uint64_t ProbeBufferBytes(uint32_t slots) { return (uint64_t(kProbeHeaderWords) + uint64_t(slots) * kProbeSlotWords) * 4; }

struct ProbeBlock {  // what the host expects of a valid slot
  uint32_t slot = 0;
  uint32_t checksum = 0;
  uint32_t samples = 0;
  uint32_t flags = 0;
  uint32_t triangles = 0;
};
struct ProbeVerdict {
  uint32_t checked = 0;
  uint32_t good = 0;
  uint32_t bad_checksum = 0;
  uint32_t bad_row = 0;        // flags or triangle count differ
  uint32_t stale_skipped = 0;  // a slot that was evicted or changed after the pass was recorded: not judged
  bool header_ok = false;
  std::string first_problem;
};
// `words` is the read back buffer (ProbeBufferBytes(slots) / 4 words); `expected` the host's blocks at the time the pass was RECORDED; `still_current(slot, checksum)` says whether the slot
// still holds that block now (a block evicted or replaced meanwhile is not judged).
template <class Current>
ProbeVerdict JudgeProbe(const uint32_t* words, size_t word_count, uint32_t slots, uint32_t material0_word, uint32_t texture0_slot, const std::vector<ProbeBlock>& expected, Current still_current) {
  ProbeVerdict v;
  if (word_count < kProbeHeaderWords || words[0] != kProbeMagic) {
    v.first_problem = "the probe's buffer has no magic: the pass did not run";
    return v;
  }
  v.header_ok = words[1] == material0_word && words[2] == texture0_slot && words[3] == slots;
  if (!v.header_ok) v.first_problem = "the material table's first word, the texture table's first slot or the slot count read through the GPU is not the host's";
  for (const ProbeBlock& e : expected) {
    const size_t at = size_t(kProbeHeaderWords) + size_t(e.slot) * kProbeSlotWords;
    if (e.slot >= slots || at + kProbeSlotWords > word_count) continue;
    if (!still_current(e.slot, e.checksum)) {
      ++v.stale_skipped;
      continue;
    }
    ++v.checked;
    const uint32_t* r = words + at;
    if (r[0] != e.checksum || r[1] != e.samples) {
      ++v.bad_checksum;
      if (v.first_problem.empty()) v.first_problem = "slot " + std::to_string(e.slot) + ": the pool's bytes read through the GPU are not the bytes that were copied";
    } else if (r[2] != e.flags || r[3] != e.triangles) {
      ++v.bad_row;
      if (v.first_problem.empty()) v.first_problem = "slot " + std::to_string(e.slot) + ": the sector table's row read through the GPU is not the host's";
    } else {
      ++v.good;
    }
  }
  return v;
}

// ---------------------------------------------------------------------------
// the pool, the table and the blocks together
// ---------------------------------------------------------------------------

// One block of the pool and what is known about it.
struct Block {
  bool present = false;
  uint64_t base = 0;
  uint64_t bytes = 0;
  uint32_t checksum = 0;
  uint32_t samples = 0;
  uint32_t triangles = 0;
  uint32_t flags = 0;
};

class PoolBook {
 public:
  // A new pool of `capacity` bytes for a pack of `sector_count` sectors: everything is forgotten (the GPU side retires the old buffer). Bumps the generation: a free that an older
  // generation queued is dropped (FreeRange).
  bool Reset(uint64_t capacity, uint32_t sector_count) {
    ++generation_;
    blocks_.assign(size_t(sector_count) * 2, Block{});
    table_.Reset(sector_count);
    pending_free_bytes_ = 0;
    return allocator_.Reset(capacity);
  }
  void Forget() {
    ++generation_;
    blocks_.clear();
    table_.Reset(0);
    pending_free_bytes_ = 0;
    allocator_ = PoolAllocator{};
  }
  uint64_t generation() const { return generation_; }
  bool ready() const { return allocator_.capacity() != 0; }

  // Gives the sector detail a range of `bytes`. False: the slot is not in the table, already holds a block (evict it first), or no range is that long (retry after kPoolRetryFrames).
  bool Place(uint32_t sector, bool lod, uint64_t bytes, uint64_t* base) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    if (slot >= blocks_.size() || blocks_[slot].present) return false;
    uint64_t at = 0;
    if (!allocator_.Alloc(bytes, &at)) return false;
    Block& b = blocks_[slot];
    b = Block{};
    b.present = true;
    b.base = at;
    b.bytes = PoolAllocator::Round(bytes);
    *base = at;
    return true;
  }
  // The block was filled and its structure built: the row becomes valid. False (and the block is given back at once: it was never visible) when the plan does not fit the range or
  // the row is refused.
  bool Commit(uint32_t sector, bool lod, const cf::PoolBlockPlan& plan, uint32_t triangle_count, uint32_t opaque_triangles, uint32_t extra_flags, uint32_t checksum, uint32_t samples) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    if (slot >= blocks_.size() || !blocks_[slot].present) return false;
    Block& b = blocks_[slot];
    cf::GpuSectorRow row;
    if (plan.total_bytes > b.bytes || !cf::MakeSectorRow(plan, b.base, triangle_count, opaque_triangles, extra_flags, &row) || !table_.Set(sector, lod, row, b.base, b.bytes)) {
      allocator_.Free(b.base, b.bytes);
      b = Block{};
      return false;
    }
    b.checksum = checksum;
    b.samples = samples;
    b.triangles = triangle_count;
    b.flags = row.flags;
    return true;
  }
  // Gives back a block that was placed and never committed (the build failed): free at once, no frame could have read it. False: nothing was placed.
  bool Abandon(uint32_t sector, bool lod) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    if (slot >= blocks_.size() || !blocks_[slot].present) return false;
    Block& b = blocks_[slot];
    if (table_.valid(sector, lod)) return false;  // committed: Evict it
    allocator_.Free(b.base, b.bytes);
    b = Block{};
    return true;
  }
  // The sector detail's structure was given back: the row is cleared NOW (the table the next frame copies says there is no hit data), and the block comes back for the caller to free
  // once the GPU is done with the frames that could read it (FreeRange). `present` false: there was none.
  Block Evict(uint32_t sector, bool lod) {
    const uint32_t slot = cf::SectorRowOf(sector, lod);
    Block out;
    if (slot >= blocks_.size() || !blocks_[slot].present) return out;
    out = blocks_[slot];
    blocks_[slot] = Block{};
    table_.Clear(sector, lod);
    pending_free_bytes_ += out.bytes;
    return out;
  }
  // The deferred half of Evict. `generation` is the one the block was evicted in: after a Reset or Forget it is gone with the pool.
  void FreeRange(uint64_t generation, uint64_t base, uint64_t bytes) {
    if (generation != generation_) return;
    allocator_.Free(base, bytes);
    pending_free_bytes_ -= std::min(pending_free_bytes_, bytes);
  }
  // Clears every row and gives every block back at once (a Reset's cheaper cousin: the pool and the allocator stay; the caller has retired the GPU's use).
  void EvictAll(std::vector<Block>* out) {
    for (uint32_t slot = 0; slot < blocks_.size(); ++slot) {
      if (!blocks_[slot].present) continue;
      if (out) out->push_back(blocks_[slot]);
      pending_free_bytes_ += blocks_[slot].bytes;
      blocks_[slot] = Block{};
    }
    table_.ClearAll();
  }

  const SectorTable& table() const { return table_; }
  const PoolAllocator& allocator() const { return allocator_; }
  const Block& block(uint32_t sector, bool lod) const { return blocks_[cf::SectorRowOf(sector, lod)]; }
  const Block& block_at_slot(uint32_t slot) const { return blocks_[slot]; }
  // Bytes evicted and not free yet (the frames in flight may read them).
  uint64_t pending_free_bytes() const { return pending_free_bytes_; }
  // Bytes the committed blocks hold (what the streaming plan counts as the pool's use).
  uint64_t committed_bytes() const {
    uint64_t sum = 0;
    for (const Block& b : blocks_) {
      if (b.present && b.flags != 0) sum += b.bytes;
    }
    return sum;
  }
  // The blocks the probe is to check: every committed one.
  std::vector<ProbeBlock> ProbeList() const {
    std::vector<ProbeBlock> list;
    for (uint32_t slot = 0; slot < blocks_.size(); ++slot) {
      const Block& b = blocks_[slot];
      if (b.present && b.flags != 0) list.push_back({slot, b.checksum, b.samples, b.flags, b.triangles});
    }
    return list;
  }
  // "" when the book is sound: the table's own check, and the allocator's use is the blocks' bytes plus what waits to be freed.
  std::string Verify() const {
    std::string why = table_.Verify(allocator_.capacity());
    if (!why.empty()) return why;
    uint64_t held = 0;
    for (const Block& b : blocks_) held += b.present ? b.bytes : 0;
    if (held + pending_free_bytes_ != allocator_.used()) return "the allocator's use (" + std::to_string(allocator_.used()) + ") is not the blocks' bytes (" + std::to_string(held) + ") plus the ranges waiting to be freed (" + std::to_string(pending_free_bytes_) + ")";
    return {};
  }

 private:
  PoolAllocator allocator_;
  SectorTable table_;
  std::vector<Block> blocks_;
  uint64_t pending_free_bytes_ = 0;
  uint64_t generation_ = 1;
};

// ---------------------------------------------------------------------------
// the texture table (t9)
// ---------------------------------------------------------------------------

// The CPU side of gCityTextures: one GpuTextureRow a texture, written by H2's loader when a texture is uploaded (slot) or given back (kNoSlot). `version` says when a frame slot's GPU copy
// is stale.
class TextureTable {
 public:
  void Reset(uint32_t texture_count) {
    rows_.assign(texture_count, cf::GpuTextureRow{});  // every row kNoSlot: not uploaded, a hit takes the material's average colour
    ++version_;
  }
  uint32_t count() const { return uint32_t(rows_.size()); }
  bool Set(uint32_t index, const cf::GpuTextureRow& row) {
    if (index >= rows_.size()) return false;
    rows_[index] = row;
    ++version_;
    return true;
  }
  const cf::GpuTextureRow& Row(uint32_t index) const { return rows_[index]; }
  uint64_t version() const { return version_; }
  const void* data() const { return rows_.data(); }
  size_t bytes() const { return rows_.size() * sizeof(cf::GpuTextureRow); }
  uint32_t first_slot() const { return rows_.empty() ? cf::kNoSlot : rows_[0].slot; }

 private:
  std::vector<cf::GpuTextureRow> rows_;
  uint64_t version_ = 1;
};

}  // namespace mcla::gpu::city_hit
