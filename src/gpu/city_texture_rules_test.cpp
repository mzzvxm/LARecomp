// Check of the city texture set's rules (city_texture_rules.h, citypack/texture_residency.h as the loader drives them): CITY-RT M2 package H2 finished for M1c. A program of its own: no
// renderer, no game files, no GPU; exit 0 = passed.
//
//   build: cmake --build <dir> --target larecomp_city_texture_rules_test, then run src/gpu/larecomp_city_texture_rules_test
//   or by hand: clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/city_texture_rules_test.cpp src/citypack/city_hit_format.cpp src/citypack/pack_format.cpp src/citypack/texture_residency.cpp
//
// It checks: the bindless slot maths (the room beyond the reserve, the most the set may hold with what the game takes, the limits by scope), the footprints of every level of a spread of textures
// (the arguments of the copy against Plume's D3D12 backend's own arithmetic: the width and height rounded to the block, the pitch from the row width, the 256 and 512 byte rules; the levels
// smaller than a block; the staging and the file's bytes agree), the video memory a texture takes (4 KiB for a small base level, 64 KiB for the rest), the budgets by class, level and scope
// (the order between them, what is off), the pressure rule, the sector's two texture lists, and the loader's whole conversation played with a fake device: a retail-sized table (8,342 pictures
// in the sizes the census found), the residency's plans driven the way CityTextureSet::Pump drives them (reads finish after a few frames, uploads take slots, evictions give them back), with
// the invariants held at every frame (the resident bytes under the budget, the slots under the limit, the uploads under the frame's bytes and count).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "city_texture_rules.h"

using namespace mcla::gpu;
using namespace mcla::gpu::city_texture;
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

cf::HitTexture Make(uint32_t w, uint32_t h, uint32_t levels, cf::HitTextureFormat format = cf::HitTextureFormat::kBc1) {
  cf::HitTexture t;
  t.width = uint16_t(w);
  t.height = uint16_t(h);
  t.mip_count = uint8_t(levels);
  t.format = format;
  t.data_size = uint32_t(t.ChainBytes(0));
  return t;
}

// ---------------------------------------------------------------------------
// the slots
// ---------------------------------------------------------------------------

void TestSlots() {
  CHECK(kSlotCapacity == 32765);
  CHECK(SlotRoom(0) == kSlotCapacity - kSlotReserve);
  CHECK(SlotRoom(3000) == kSlotCapacity - 3000 - kSlotReserve);
  CHECK(SlotRoom(kSlotCapacity - kSlotReserve) == 0 && SlotRoom(kSlotCapacity - kSlotReserve - 1) == 1 && SlotRoom(kSlotCapacity) == 0 && SlotRoom(kSlotCapacity + 100) == 0 && SlotRoom(0xFFFFFFFFu) == 0);
  CHECK(SlotRoom(10, 0, 100) == 90 && SlotRoom(10, 100, 100) == 0 && SlotRoom(10, 95, 100) == 0 && SlotRoom(10, 80, 100) == 10);
  // the retail set (8,342 pictures) fits at full scope with the game's own 3,000 slots, and a sector's few cut-outs fit the alpha scope whatever else is going on
  CHECK(kMaxSlotsAll >= 8342 && kMaxSlotsAll + kSlotReserve <= kSlotCapacity);
  CHECK(SlotLimit(TextureScope::kAll) == kMaxSlotsAll && SlotLimit(TextureScope::kAlpha) == kMaxSlotsAlpha && SlotLimit(TextureScope::kNone) == 0);
  CHECK(MaxTextures(TextureScope::kAll, 3000, 0) == kMaxSlotsAll);
  CHECK(MaxTextures(TextureScope::kAlpha, 3000, 0) == kMaxSlotsAlpha);
  CHECK(MaxTextures(TextureScope::kNone, 3000, 0) == 0);
  // the total counts the city's own: with 4,000 of 7,000 taken by the city and 3,000 by the others, the room is what the others leave
  CHECK(MaxTextures(TextureScope::kAll, 7000, 4000) == kMaxSlotsAll);
  // the others fill the heap: the set may hold nothing more than the scope's limit allows and the room gives
  CHECK(MaxTextures(TextureScope::kAll, kSlotCapacity - kSlotReserve - 500 + 100, 100) == 500);
  CHECK(MaxTextures(TextureScope::kAll, kSlotCapacity, 0) == 0 && MaxTextures(TextureScope::kAll, kSlotCapacity - kSlotReserve, 0) == 0);  // 0 is "none": the residency's no-limit is 0xFFFFFFFF
  CHECK(MaxTextures(TextureScope::kAll, 100, 5000) == kMaxSlotsAll);  // an in_use smaller than the city's own (a stale read): the others are 0
  // the city set never leaves fewer than the reserve free, whatever the others do
  for (uint32_t others : {0u, 1000u, 3000u, 12000u, 20000u, 20477u, 20478u, 25000u, 32765u}) {
    const uint32_t take = MaxTextures(TextureScope::kAll, others + 0, 0);
    CHECK(others + take + kSlotReserve <= std::max(kSlotCapacity, others + kSlotReserve) || take == 0);
    CHECK(others >= kSlotCapacity - kSlotReserve ? take == 0 : others + take <= kSlotCapacity - kSlotReserve);
  }
}

// ---------------------------------------------------------------------------
// the footprints
// ---------------------------------------------------------------------------

void TestFootprints() {
  CHECK(BlockWidth(cf::HitTextureFormat::kBc1) == 4 && BlockWidth(cf::HitTextureFormat::kBc2) == 4 && BlockWidth(cf::HitTextureFormat::kBc3) == 4 && BlockWidth(cf::HitTextureFormat::kRgba8) == 1);
  CHECK(BlockBytes(cf::HitTextureFormat::kBc1) == 8 && BlockBytes(cf::HitTextureFormat::kBc2) == 16 && BlockBytes(cf::HitTextureFormat::kBc3) == 16 && BlockBytes(cf::HitTextureFormat::kRgba8) == 4);
  CHECK(kPitchAlignment == 256 && kOffsetAlignment == 512);

  // an 8 x 8 BC3 chain: the 2 x 2 and 1 x 1 levels are one block
  {
    const cf::HitTexture t = Make(8, 8, 4, cf::HitTextureFormat::kBc3);
    const std::vector<Footprint> f = FootprintsOf(t, 0);
    CHECK(f.size() == 4);
    CHECK(f[0].width == 8 && f[0].height == 8 && f[0].rows == 2 && f[0].pitch == 256 && f[0].row_width == 64 && f[0].staging_bytes == 512);
    CHECK(f[1].width == 4 && f[1].height == 4 && f[1].rows == 1 && f[1].pitch == 256 && f[1].row_width == 64);
    CHECK(f[2].width == 2 && f[2].height == 2 && f[2].rows == 1 && f[2].pitch == 256 && f[2].row_width == 64 && f[2].staging_bytes == 256);
    CHECK(f[3].width == 1 && f[3].height == 1 && f[3].rows == 1 && f[3].pitch == 256);
    // what Plume's D3D12 backend asks the driver for: the level rounded to the block, the pitch of the row's blocks
    const D3d12Footprint d2 = PlumeD3d12(f[2], t.format);
    CHECK(d2.width == 4 && d2.height == 4 && d2.row_pitch == 256);
    for (const Footprint& fp : f) CHECK(FootprintAccepted(fp, t.format, 0) && FootprintAccepted(fp, t.format, 512) && !FootprintAccepted(fp, t.format, 256));
  }
  // a 4 x 4 BC1 base from a 512 x 512 chain at the cap: the first resident level at 128 is level 2
  {
    const cf::HitTexture t = Make(512, 512, 9);
    const std::vector<Footprint> f = FootprintsOf(t, 2);
    CHECK(f.size() == 7 && f[0].width == 128 && f[0].height == 128 && f[0].rows == 32 && f[0].pitch == 256 && f[0].row_width == 128 && f[6].width == 2 && f[6].height == 2);
  }
  // the raw format: one texel blocks, rows on 256
  {
    const cf::HitTexture t = Make(30, 10, 2, cf::HitTextureFormat::kRgba8);
    const std::vector<Footprint> f = FootprintsOf(t, 0);
    CHECK(f.size() == 2 && f[0].width == 30 && f[0].height == 10 && f[0].rows == 10 && f[0].pitch == 256 && f[0].row_width == 64 && f[1].width == 15 && f[1].rows == 5);
    for (const Footprint& fp : f) CHECK(FootprintAccepted(fp, t.format, 1024));
  }

  // a spread of sizes, formats and first levels: every footprint is one Plume accepts, the levels add up to the chain, the staging covers the rows
  Lcg rng;
  uint64_t levels_seen = 0, sub_block = 0;
  const cf::HitTextureFormat formats[] = {cf::HitTextureFormat::kBc1, cf::HitTextureFormat::kBc2, cf::HitTextureFormat::kBc3, cf::HitTextureFormat::kRgba8};
  const uint32_t sides[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};
  for (cf::HitTextureFormat format : formats) {
    for (uint32_t w : sides) {
      for (uint32_t h : sides) {
        if (format != cf::HitTextureFormat::kRgba8 && (w < 4 || h < 4)) continue;  // a block compressed picture of the game is whole blocks at its base
        uint32_t full = 1;
        while ((std::max(w, h) >> (full - 1)) > 1) ++full;
        for (uint32_t levels : {1u, std::min(full, 3u), full}) {
          const cf::HitTexture t = Make(w, h, levels, format);
          for (uint32_t first = 0; first < levels; ++first) {
            const std::vector<Footprint> f = FootprintsOf(t, first);
            CHECK(f.size() == levels - first);
            uint64_t tight = 0, staged = 0;
            for (size_t i = 0; i < f.size(); ++i) {
              const uint32_t lw = std::max(w >> (first + i), 1u), lh = std::max(h >> (first + i), 1u);
              CHECK(f[i].width == lw && f[i].height == lh);
              CHECK(FootprintAccepted(f[i], format, 512 * i));
              const uint32_t bw = BlockWidth(format);
              CHECK(f[i].rows == (lh + bw - 1) / bw);
              tight += uint64_t(((lw + bw - 1) / bw) * BlockBytes(format)) * f[i].rows;
              staged += f[i].staging_bytes;
              ++levels_seen;
              sub_block += (lw < 4 || lh < 4) && format != cf::HitTextureFormat::kRgba8 ? 1 : 0;
            }
            CHECK(tight == t.ChainBytes(first));
            CHECK(staged >= tight && StagingBytes(t, first) >= staged && StagingBytes(t, first) <= staged + 511 * f.size());
          }
        }
      }
    }
  }
  CHECK(levels_seen > 1000 && sub_block > 100);  // the spread did reach the levels smaller than a block
  (void)rng;

  // a footprint that Plume would refuse
  {
    Footprint bad;
    bad.width = 8;
    bad.height = 8;
    bad.pitch = 100;  // not on 256
    bad.rows = 2;
    bad.row_width = 8;
    bad.staging_bytes = 200;
    CHECK(!FootprintAccepted(bad, cf::HitTextureFormat::kBc1, 0));
    Footprint narrow;
    narrow.width = 16;
    narrow.height = 4;
    narrow.pitch = 256;
    narrow.rows = 1;
    narrow.row_width = 8;  // a row narrower than the level
    narrow.staging_bytes = 256;
    CHECK(!FootprintAccepted(narrow, cf::HitTextureFormat::kBc1, 0));
    CHECK(!FootprintAccepted(Footprint{}, cf::HitTextureFormat::kBc1, 0));
  }
}

// ---------------------------------------------------------------------------
// the memory
// ---------------------------------------------------------------------------

void TestMemory() {
  const cf::HitTexture wall = Make(512, 512, 4);   // 131072 + 32768 + 8192 + 2048 = 174080: the base is over 64 KiB
  const cf::HitTexture fence = Make(256, 256, 5);  // the base is 32 KiB: the small unit
  const cf::HitTexture tiny = Make(32, 32, 6);
  CHECK(AllocationBytes(wall, 0) == 196608 && AllocationBytes(wall, 1) == 45056 && AllocationBytes(wall, 2) == 12288);
  CHECK(AllocationBytes(fence, 0) % 4096 == 0 && AllocationBytes(fence, 0) >= fence.ChainBytes(0) && AllocationBytes(fence, 0) < fence.ChainBytes(0) + 4096);
  CHECK(AllocationBytes(tiny, 0) == 4096);
  for (const cf::HitTexture* t : {&wall, &fence, &tiny}) {
    uint64_t previous = ~0ull;
    for (uint32_t first = 0; first < t->mip_count; ++first) {
      const uint64_t bytes = AllocationBytes(*t, first);
      CHECK(bytes >= t->ChainBytes(first) && bytes <= previous);  // a higher first level never takes more
      previous = bytes;
    }
  }
  // the unit is exactly the base level's rule
  CHECK(kSmallAlignment == 4096 && kLargeAlignment == 65536 && kSmallBaseLimit == 65536);
  const cf::HitTexture edge = Make(512, 256, 3);  // base 512 x 256 BC1 = 65536: on the limit, small
  CHECK(cf::HitMipBytes(cf::HitTextureFormat::kBc1, 512, 256) == 65536 && AllocationBytes(edge, 0) % 4096 == 0 && AllocationBytes(edge, 0) <= 65536 + 16384 + 4096);
  const cf::HitTexture over = Make(512, 512, 1, cf::HitTextureFormat::kBc1);  // 131072: 2 x 64 KiB
  CHECK(AllocationBytes(over, 0) == 131072);
  const cf::HitTexture over3 = Make(512, 512, 1, cf::HitTextureFormat::kBc3);  // 262144
  CHECK(AllocationBytes(over3, 0) == 262144);
}

// ---------------------------------------------------------------------------
// the budgets
// ---------------------------------------------------------------------------

void TestBudgets() {
  using city_rt::Level;
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const Level levels[] = {Level::kOff, Level::kHigh, Level::kUltra};
  const TextureScope scopes[] = {TextureScope::kNone, TextureScope::kAlpha, TextureScope::kAll};
  for (GpuClass cls : classes) {
    for (Level level : levels) {
      for (TextureScope scope : scopes) {
        const TextureBudget b = BudgetFor(cls, level, scope);
        const bool off = cls == GpuClass::kLow || level == Level::kOff || scope == TextureScope::kNone;
        CHECK(b.on() == !off);
        if (off) {
          CHECK(b.bytes == 0 && b.cap_px == 0 && b.upload_bytes == 0 && b.uploads == 0);
        } else {
          CHECK(b.bytes >= 8 * kMiB && b.upload_bytes >= kMiB && b.uploads >= 1 && b.uploads <= 64);
        }
      }
    }
  }
  // M2 section 8's caps for the reflections' scope: 128 on a Mid card, 256 at High, the full size at ultra
  CHECK(BudgetFor(GpuClass::kMid, Level::kHigh, TextureScope::kAll).cap_px == 128 && BudgetFor(GpuClass::kHigh, Level::kHigh, TextureScope::kAll).cap_px == 256 &&
        BudgetFor(GpuClass::kHigh, Level::kUltra, TextureScope::kAll).cap_px == 0);
  // the cut-outs keep the shape, not the detail
  CHECK(BudgetFor(GpuClass::kMid, Level::kHigh, TextureScope::kAlpha).cap_px == 128 && BudgetFor(GpuClass::kHigh, Level::kHigh, TextureScope::kAlpha).cap_px == 256 &&
        BudgetFor(GpuClass::kHigh, Level::kUltra, TextureScope::kAlpha).cap_px == 512);
  // a Mid card asked for ultra by hand gets the High class's high where the pool does (a Mid card's "ultra" is its own high)
  CHECK(BudgetFor(GpuClass::kMid, Level::kUltra, TextureScope::kAll).bytes == BudgetFor(GpuClass::kMid, Level::kHigh, TextureScope::kAll).bytes);
  // more class, more bytes; the cut-outs take less than everything
  for (TextureScope scope : {TextureScope::kAlpha, TextureScope::kAll}) {
    CHECK(BudgetFor(GpuClass::kMid, Level::kHigh, scope).bytes < BudgetFor(GpuClass::kHigh, Level::kHigh, scope).bytes);
    CHECK(BudgetFor(GpuClass::kHigh, Level::kHigh, scope).bytes < BudgetFor(GpuClass::kHigh, Level::kUltra, scope).bytes);
  }
  for (GpuClass cls : {GpuClass::kMid, GpuClass::kHigh}) {
    for (Level level : {Level::kHigh, Level::kUltra}) {
      CHECK(BudgetFor(cls, level, TextureScope::kAlpha).bytes < BudgetFor(cls, level, TextureScope::kAll).bytes);
      CHECK(BudgetFor(cls, level, TextureScope::kAlpha).upload_bytes <= BudgetFor(cls, level, TextureScope::kAll).upload_bytes);
    }
  }
  // the residency's configuration
  const TextureBudget b = BudgetFor(GpuClass::kHigh, Level::kHigh, TextureScope::kAll);
  const cf::ResidencyConfig c = ConfigFor(b, 5000, b.bytes);
  CHECK(c.cap_px == 256 && c.budget_bytes == 128 * kMiB && c.upload_bytes_per_frame == 6 * kMiB && c.max_uploads_per_frame == 16 && c.max_textures == 5000);
  CHECK(c.allocation_alignment == 4096 && c.large_alignment == 65536 && c.small_base_limit == 65536);
  CHECK(c.max_reads_in_flight == cf::ResidencyConfig{}.max_reads_in_flight && c.idle_frames == cf::ResidencyConfig{}.idle_frames);
}

// ---------------------------------------------------------------------------
// pressure
// ---------------------------------------------------------------------------

void TestPressure() {
  CHECK(GrowthAllowed(1000, 0, 100) && GrowthAllowed(0, 0, 1ull << 40));  // an adapter with no budget to compare with: the creation is the judge
  CHECK(GrowthAllowed(8000, 12000, 1000) && !GrowthAllowed(10000, 12000, 1000) && !GrowthAllowed(12000, 12000, 0));
  CHECK(GrowthAllowed(10799, 12000, 0) && !GrowthAllowed(10800, 12000, 0));  // 90 percent of the budget
  CHECK(EffectiveBytes(100, 40, true) == 100 && EffectiveBytes(100, 40, false) == 40 && EffectiveBytes(100, 400, false) == 100 && EffectiveBytes(100, 0, false) == 1);
  CHECK(kPressureCheckFrames >= 1 && kPriorityRefreshFrames >= 1);
  CHECK(SectorPriority(-3.0f) == 0.0f && SectorPriority(12.5f) == 12.5f);
}

// ---------------------------------------------------------------------------
// a sector's textures
// ---------------------------------------------------------------------------

void TestSectorLists() {
  std::vector<uint32_t> words(8 * 5, 0);
  const auto set = [&](uint32_t m, uint16_t diffuse, uint16_t layer) { words[8 * m + 1] = uint32_t(diffuse) | (uint32_t(layer) << 16); };
  set(0, cf::kNoTexture, cf::kNoTexture);
  set(1, 7, cf::kNoTexture);
  set(2, 9, cf::kNoTexture);
  set(3, 7, 11);
  set(4, 3, cf::kNoTexture);
  std::vector<cf::HitRun> runs(5);
  for (uint32_t i = 0; i < 5; ++i) runs[i].material = i;
  runs[2].flags = cf::kRunAlphaTested;
  runs[4].flags = cf::kRunAlphaTested | cf::kRunHasUv;
  const SectorTextures s = CollectSector(runs, words.data(), 5);
  CHECK(s.alpha == (std::vector<uint16_t>{3, 9}) && s.all == (std::vector<uint16_t>{3, 7, 9, 11}));
  CHECK(&s.For(TextureScope::kAlpha) == &s.alpha && &s.For(TextureScope::kAll) == &s.all && &s.For(TextureScope::kNone) == &s.alpha);
  // the alpha list is always inside the whole list
  for (uint16_t a : s.alpha) CHECK(std::binary_search(s.all.begin(), s.all.end(), a));
  const SectorTextures none = CollectSector({}, words.data(), 5);
  CHECK(none.alpha.empty() && none.all.empty());
}

// ---------------------------------------------------------------------------
// the loader's conversation, with a fake device
// ---------------------------------------------------------------------------

// A table that looks like the retail set (8,342 pictures; the sizes of the census, in the mix of its size classes) built from a seed.
std::vector<cf::HitTexture> RetailLikeTable(uint32_t count, Lcg& rng) {
  std::vector<cf::HitTexture> table;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t pick = rng.Below(1000);
    uint32_t w, h;
    if (pick < 350) { w = 64; h = 64; }
    else if (pick < 800) { w = 128; h = 128; }
    else if (pick < 950) { w = 256; h = 256; }
    else if (pick < 980) { w = 512; h = 256; }
    else if (pick < 995) { w = 512; h = 512; }
    else if (pick < 997) { w = 1024; h = 512; }
    else if (pick < 998) { w = 8; h = 512; }  // the elongated ones
    else { w = 1024; h = 1024; }
    const cf::HitTextureFormat format = rng.Below(10) < 7 ? cf::HitTextureFormat::kBc1 : cf::HitTextureFormat::kBc3;
    uint32_t levels = 1;
    while ((std::max(w, h) >> (levels - 1)) > 4 && levels < 11) ++levels;
    levels = std::max(1u, levels - rng.Below(3));  // the game stores some chains short
    table.push_back(Make(w, h, levels, format));
  }
  return table;
}

struct FakeHeap {
  uint32_t others = 2500;      // the game's own slots
  uint32_t city = 0;
  uint32_t peak_city = 0;
  uint32_t in_use() const { return others + city; }
};

struct Conversation {
  uint64_t frames = 0;
  uint64_t reads = 0, uploads = 0, evictions = 0;
  uint32_t max_uploads_in_a_frame = 0;
  uint64_t max_upload_bytes_in_a_frame = 0;
  uint64_t max_resident_bytes = 0;
  uint32_t max_resident = 0;
  uint32_t max_slots_over_limit = 0;
  bool bytes_over_budget = false;
  bool slot_floor_broken = false;
  bool footprint_refused = false;
  uint64_t staging_max_frame = 0;
};

// The loop of CityTextureSet::Pump without the GPU: reads finish `read_delay` frames after they are handed out, an upload takes a slot and gives the accounted bytes, an eviction gives both back,
// and the sector set moves: `rings` is a list of sector texture lists, a window of them is added and removed as the "camera" moves.
Conversation Play(const std::vector<cf::HitTexture>& table, const TextureBudget& budget, TextureScope scope, uint32_t read_delay, const std::vector<std::vector<uint16_t>>& sectors, uint32_t window,
                  uint32_t frames_per_step, uint32_t steps, FakeHeap* heap) {
  cf::TextureResidency residency(table);
  Conversation out;
  struct Pending { uint32_t index; uint64_t ready_at; };
  std::vector<Pending> reading;
  std::set<uint32_t> resident;
  std::vector<uint64_t> resident_bytes(table.size(), 0);
  uint64_t held = 0;
  uint64_t frame = 0;
  uint32_t first_sector = 0;
  for (uint32_t i = 0; i < window && i < sectors.size(); ++i) residency.AddSector(i, sectors[i].data(), sectors[i].size(), float(i));
  const uint64_t limit_bytes = budget.bytes;
  for (uint32_t step = 0; step < steps; ++step) {
    for (uint32_t f = 0; f < frames_per_step; ++f) {
      ++frame;
      // Pump: results in, config from the slots free now, the plan, evictions, reads, uploads
      for (size_t i = 0; i < reading.size();) {
        if (reading[i].ready_at <= frame) {
          residency.ReadFinished(reading[i].index, true);
          reading.erase(reading.begin() + long(i));
        } else {
          ++i;
        }
      }
      const uint32_t max_textures = MaxTextures(scope, heap->in_use(), heap->city);
      residency.Configure(ConfigFor(budget, max_textures, budget.bytes));
      const cf::TextureResidency::Plan plan = residency.Step(frame);
      for (uint32_t index : plan.evictions) {
        if (resident.erase(index) != 0) {
          --heap->city;
          held -= resident_bytes[index];
          resident_bytes[index] = 0;
          ++out.evictions;
        }
      }
      for (const auto& read : plan.reads) {
        reading.push_back({read.index, frame + read_delay});
        ++out.reads;
      }
      uint64_t upload_bytes = 0, staging = 0;
      for (uint32_t index : plan.uploads) {
        const uint32_t first = residency.first_mip(index);
        for (const Footprint& fp : FootprintsOf(table[index], first)) out.footprint_refused |= !FootprintAccepted(fp, table[index].format, 0);
        staging += StagingBytes(table[index], first);
        upload_bytes += table[index].ChainBytes(first);
        resident.insert(index);
        resident_bytes[index] = AllocationBytes(table[index], first);
        held += resident_bytes[index];
        ++heap->city;
        ++out.uploads;
      }
      out.max_uploads_in_a_frame = std::max<uint32_t>(out.max_uploads_in_a_frame, uint32_t(plan.uploads.size()));
      out.max_upload_bytes_in_a_frame = std::max(out.max_upload_bytes_in_a_frame, upload_bytes);
      out.staging_max_frame = std::max(out.staging_max_frame, staging);
      out.max_resident_bytes = std::max(out.max_resident_bytes, held);
      out.max_resident = std::max<uint32_t>(out.max_resident, uint32_t(resident.size()));
      heap->peak_city = std::max(heap->peak_city, heap->city);
      if (held > limit_bytes) out.bytes_over_budget = true;
      if (heap->in_use() + kSlotReserve > kSlotCapacity && heap->city > MaxTextures(scope, heap->in_use(), heap->city) && heap->others + kSlotReserve <= kSlotCapacity) out.slot_floor_broken = true;
      if (heap->city > SlotLimit(scope)) ++out.max_slots_over_limit;
      ++out.frames;
    }
    // the camera moves one sector on
    if (first_sector + window < sectors.size()) {
      residency.RemoveSector(first_sector);
      residency.AddSector(first_sector + window, sectors[first_sector + window].data(), sectors[first_sector + window].size(), float(window));
      for (uint32_t i = first_sector + 1; i < first_sector + window; ++i) residency.SetSectorPriority(i, float(i - first_sector - 1));
      ++first_sector;
    }
  }
  return out;
}

void TestConversation() {
  Lcg rng;
  const uint32_t count = 8342;
  const std::vector<cf::HitTexture> table = RetailLikeTable(count, rng);
  uint64_t total_data = 0;
  for (const cf::HitTexture& t : table) total_data += t.data_size;
  CHECK(total_data > 100 * kMiB && total_data < 600 * kMiB);

  // sectors: 439 of them, each naming a few hundred pictures out of the set (some shared: the common ones), the cut-outs a few dozen
  const uint32_t sector_count = 120;
  std::vector<std::vector<uint16_t>> all_lists, alpha_lists;
  for (uint32_t s = 0; s < sector_count; ++s) {
    std::set<uint16_t> a, c;
    for (uint32_t i = 0; i < 200; ++i) a.insert(uint16_t(rng.Below(count)));
    for (uint32_t i = 0; i < 40; ++i) c.insert(uint16_t(rng.Below(500)));  // the common ones, shared
    a.insert(c.begin(), c.end());
    all_lists.emplace_back(a.begin(), a.end());
    std::set<uint16_t> cut;
    for (uint32_t i = 0; i < 12; ++i) cut.insert(uint16_t(rng.Below(300)));
    alpha_lists.emplace_back(cut.begin(), cut.end());
  }

  for (GpuClass cls : {GpuClass::kMid, GpuClass::kHigh}) {
    for (city_rt::Level level : {city_rt::Level::kHigh, city_rt::Level::kUltra}) {
      for (TextureScope scope : {TextureScope::kAlpha, TextureScope::kAll}) {
        const TextureBudget budget = BudgetFor(cls, level, scope);
        FakeHeap heap;
        const Conversation c = Play(table, budget, scope, 3, scope == TextureScope::kAll ? all_lists : alpha_lists, 12, 20, 60, &heap);
        CHECK(c.frames == 1200);
        CHECK(c.uploads > 0 && c.reads >= c.uploads);                          // the set filled
        CHECK(!c.bytes_over_budget && c.max_resident_bytes <= budget.bytes);   // the accounted bytes never pass the class's budget
        CHECK(c.max_uploads_in_a_frame <= budget.uploads);                     // the uploads of a frame under the count
        // the bytes of a frame: the budget's, and at most one texture more (a texture is never held back for good: the first of the frame always goes)
        uint64_t biggest = 0;
        for (const cf::HitTexture& t : table) biggest = std::max<uint64_t>(biggest, t.data_size);
        CHECK(c.max_upload_bytes_in_a_frame <= budget.upload_bytes + biggest);
        CHECK(c.max_slots_over_limit == 0 && heap.peak_city <= SlotLimit(scope));
        CHECK(!c.slot_floor_broken && !c.footprint_refused);
        CHECK(heap.city == c.uploads - c.evictions);                           // every slot taken was given back or is held
        CHECK(c.staging_max_frame <= (budget.upload_bytes + biggest) * 2 + 64 * 1024 * 16);  // the staging a frame asks of the upload ring stays within a chunk or two
      }
    }
  }

  // the game takes the slots: the city set shrinks to what is left beyond the reserve, never beyond
  {
    const TextureBudget budget = BudgetFor(GpuClass::kHigh, city_rt::Level::kUltra, TextureScope::kAll);
    FakeHeap heap;
    heap.others = kSlotCapacity - kSlotReserve - 400;  // room for 400
    const Conversation c = Play(table, budget, TextureScope::kAll, 2, all_lists, 12, 20, 40, &heap);
    CHECK(heap.peak_city <= 400 && c.uploads > 0);
    CHECK(heap.in_use() + kSlotReserve <= kSlotCapacity);
    FakeHeap full;
    full.others = kSlotCapacity - kSlotReserve;  // none to spare
    const Conversation none = Play(table, budget, TextureScope::kAll, 2, all_lists, 12, 20, 20, &full);
    CHECK(none.uploads == 0 && none.reads == 0 && full.city == 0);
  }
  // the memory is nearly full: no growth (EffectiveBytes holds the set at what it has): a set that held 50 MiB reads nothing more
  {
    const TextureBudget budget = BudgetFor(GpuClass::kHigh, city_rt::Level::kHigh, TextureScope::kAll);
    cf::TextureResidency residency(table);
    residency.Configure(ConfigFor(budget, 10000, budget.bytes));
    const uint16_t some[] = {0, 1, 2, 3, 4, 5};
    residency.AddSector(1, some, 6, 1.0f);
    const cf::TextureResidency::Plan first = residency.Step(1);
    CHECK(first.reads.size() == 6);  // max_reads_in_flight
    for (const auto& r : first.reads) residency.ReadFinished(r.index, true);
    const cf::TextureResidency::Plan second = residency.Step(2);
    CHECK(second.uploads.size() == 6 && second.reads.empty());
    const uint64_t held = residency.committed_bytes();
    const uint16_t later[] = {6, 7, 8};
    residency.AddSector(2, later, 3, 2.0f);  // a worse priority than what is held
    residency.Configure(ConfigFor(budget, 10000, EffectiveBytes(budget.bytes, held, false)));
    const cf::TextureResidency::Plan more = residency.Step(3);
    CHECK(more.reads.empty() && more.evictions.empty() && residency.committed_bytes() == held);  // nothing new, nothing lost
    residency.Configure(ConfigFor(budget, 10000, EffectiveBytes(budget.bytes, held, true)));
    CHECK(residency.Step(4).reads.size() == 3);  // the pressure passed: the three are read
  }
  // the scope changes while a sector stays: the reflections turn on, the alpha list is replaced by the whole one
  {
    const TextureBudget alpha = BudgetFor(GpuClass::kHigh, city_rt::Level::kHigh, TextureScope::kAlpha);
    const TextureBudget all = BudgetFor(GpuClass::kHigh, city_rt::Level::kHigh, TextureScope::kAll);
    cf::TextureResidency residency(table);
    residency.Configure(ConfigFor(alpha, 10000, alpha.bytes));
    SectorTextures lists;
    lists.alpha = {10, 11};
    lists.all = {10, 11, 12, 13, 14};
    residency.AddSector(5, lists.For(TextureScope::kAlpha).data(), lists.For(TextureScope::kAlpha).size(), 1.0f);
    CHECK(residency.Step(1).reads.size() == 2 && residency.referenced_by(12) == 0);
    residency.Configure(ConfigFor(all, 10000, all.bytes));
    residency.AddSector(5, lists.For(TextureScope::kAll).data(), lists.For(TextureScope::kAll).size(), 1.0f);
    const cf::TextureResidency::Plan plan = residency.Step(2);
    CHECK(residency.referenced_by(12) == 1 && residency.referenced_by(10) == 1);
    CHECK(plan.reads.size() <= 6);
  }
}

}  // namespace

int main() {
  TestSlots();
  TestFootprints();
  TestMemory();
  TestBudgets();
  TestPressure();
  TestSectorLists();
  TestConversation();
  std::printf("city texture rules test: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
