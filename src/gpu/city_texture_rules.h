#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city texture set on the GPU: the rules and the maths that need no renderer (CITY-RT M2 package H2, finished for M1c)
// ===========================================================================
// city_texture_set.h makes the textures of the city's hit data resident (the texture file the hit file's builder writes, citypack/texture_set.h), reading them on a worker thread, creating
// them as BC1 / BC2 / BC3 (RGBA8 for the few raw ones) textures, copying their levels through the upload ring and giving each a bindless slot that the texture table (t9) names. What
// needs no GPU to decide is here, so that city_texture_rules_test.cpp builds on its own and checks it:
//
//   SLOTS       the bindless heap has 32,765 slots to hand out (descriptors.h kBindlessTextureCount less the three nulls); the game's own textures, the edram targets, the host passes and the
//               overlay take 1,000 to 3,000 (CHECK 3 of city_texture_set.h: read it from the log), and the city set can ask for 8,342 pictures. The set never takes the last kSlotReserve
//               slots: MaxTextures() is the most it may hold now, and the residency (citypack/texture_residency.h ResidencyConfig::max_textures) evicts or waits instead of asking for more.
//   FOOTPRINTS  the arguments of the copy of one level: the same ones the game's own texture cache gives Plume (textures.cpp Upload: the level's size in texels, not rounded to a block, and the row
//               width in texels of the staging row), which Plume's D3D12 backend rounds up to the block itself and its Vulkan backend takes as bufferRowLength. A level smaller than a block (the 2 x 2
//               and 1 x 1 ends of a chain) is one block row of one block. The staging rows are on 256 bytes and each level's staging on 512 (the D3D12 placement rules, on both backends).
//   MEMORY      the video memory a texture takes. D3D12MA (Plume's allocator) places a texture on 4 KiB when it qualifies for the small alignment (the conservative rule: a base level of at most
//               64 KiB) and on 64 KiB when it does not, so a texture takes its chain rounded to 4 KiB or to 64 KiB (AllocationBytes). The budget counts that. The real figure is CHECK 2: the memory
//               line next to the "[city tex]" census says both.
//   BUDGETS     the cap on a texture's size, the bytes and the uploads a frame by graphics class, level and scope (city_texture_scope.h): what the cut-out shadows need is a fraction of what the
//               reflections will.
//   PRESSURE    when the adapter's video memory is nearly full the set stops growing (vram_room.h): what is resident stays, nothing new is read.
//
// This header includes citypack/city_hit_format.h and citypack/texture_residency.h (declarations), city_rt_rules.h, gpu_class.h, vram_room.h and nothing of the renderer: the test links
// src/citypack/city_hit_format.cpp, pack_format.cpp and texture_residency.cpp.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <vector>

#include "citypack/city_hit_format.h"
#include "citypack/texture_residency.h"
#include "city_rt_rules.h"
#include "city_texture_scope.h"
#include "gpu_class.h"
#include "vram_room.h"

namespace mcla::gpu::city_texture {

namespace cf = larecomp::citypack;
using Level = city_rt::Level;

inline constexpr uint64_t kMiB = 1ull << 20;

// ---------------------------------------------------------------------------
// the bindless slots
// ---------------------------------------------------------------------------

// descriptors.h: kBindlessTextureCount (32,768) less kFirstAllocatableTextureSlot (3). city_texture_set.cpp static_asserts the two are the same number.
inline constexpr uint32_t kSlotCapacity = 32768 - 3;
// The slots the city set leaves to everything else: the game's textures (1,000 to 3,000 measured), the targets, the passes and the overlay, and the room a world entry's burst needs.
inline constexpr uint32_t kSlotReserve = 12288;
// The most slots the set may hold whatever is free: every picture of the retail set at full scope (8,342, with room for the builds that come), a few hundred cut-outs at the alpha scope.
inline constexpr uint32_t kMaxSlotsAll = 10240;
inline constexpr uint32_t kMaxSlotsAlpha = 2048;

constexpr uint32_t SlotLimit(TextureScope scope) { return scope == TextureScope::kAll ? kMaxSlotsAll : scope == TextureScope::kAlpha ? kMaxSlotsAlpha : 0u; }

// Slots beyond the reserve that are still free, `in_use` being everything taken (BindlessHeap::textures_in_use(), the city set's included).
constexpr uint32_t SlotRoom(uint32_t in_use, uint32_t reserve = kSlotReserve, uint32_t capacity = kSlotCapacity) {
  const uint32_t free_slots = capacity > in_use ? capacity - in_use : 0;
  return free_slots > reserve ? free_slots - reserve : 0;
}

// ResidencyConfig::max_textures: what the set holds now (`city_held`, of the `in_use` total) and the room there is, at most the scope's limit. 0 when the scope asks for nothing.
constexpr uint32_t MaxTextures(TextureScope scope, uint32_t in_use, uint32_t city_held, uint32_t reserve = kSlotReserve, uint32_t capacity = kSlotCapacity) {
  const uint32_t others = in_use > city_held ? in_use - city_held : 0;
  return std::min(SlotLimit(scope), SlotRoom(others, reserve, capacity));
}

// ---------------------------------------------------------------------------
// the footprints of a texture's levels
// ---------------------------------------------------------------------------

constexpr uint32_t BlockWidth(cf::HitTextureFormat format) { return format == cf::HitTextureFormat::kRgba8 ? 1u : 4u; }
constexpr uint32_t BlockBytes(cf::HitTextureFormat format) { return format == cf::HitTextureFormat::kRgba8 ? 4u : format == cf::HitTextureFormat::kBc1 ? 8u : 16u; }

// The D3D12 placement rules, as upload.h's kTextureCopyPitchAlignment and kTextureCopyOffsetAlignment (city_texture_set.cpp static_asserts them).
inline constexpr uint32_t kPitchAlignment = 256;
inline constexpr uint64_t kOffsetAlignment = 512;

// plume::RenderTextureCopyLocation::PlacedFootprint(buffer, format, width, height, 1, row_width, offset), the arguments of one level.
struct Footprint {
  uint32_t width = 0;      // texels, the level's own (not rounded to a block)
  uint32_t height = 0;
  uint32_t row_width = 0;  // texels of a staging row: the pitch in blocks times the block's width
  uint32_t pitch = 0;      // bytes of a staging row
  uint32_t rows = 0;       // block rows
  uint64_t staging_bytes = 0;
};

inline Footprint FootprintOf(const cf::UploadLevel& level, cf::HitTextureFormat format) {
  Footprint f;
  f.width = level.width;
  f.height = level.height;
  f.pitch = level.pitch;
  f.rows = level.rows;
  f.row_width = (level.pitch / BlockBytes(format)) * BlockWidth(format);
  f.staging_bytes = level.staging_bytes();
  return f;
}

// What Plume's D3D12 backend makes of a footprint (plume_d3d12.cpp toD3D12 PLACED_FOOTPRINT): the width and height rounded up to the block, the row pitch from the row width's blocks. The test
// compares the level's own numbers with it.
struct D3d12Footprint {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
};
constexpr D3d12Footprint PlumeD3d12(const Footprint& f, cf::HitTextureFormat format) {
  const uint32_t bw = BlockWidth(format);
  D3d12Footprint out;
  out.width = ((f.width + bw - 1) / bw) * bw;
  out.height = ((f.height + bw - 1) / bw) * bw;
  out.row_pitch = ((f.row_width + bw - 1) / bw) * BlockBytes(format);
  return out;
}

// The levels of a texture from `first_mip`, each as its footprint (citypack::PlanUploadLevels at the D3D12 pitch).
inline std::vector<Footprint> FootprintsOf(const cf::HitTexture& texture, uint32_t first_mip) {
  std::vector<Footprint> out;
  for (const cf::UploadLevel& level : cf::PlanUploadLevels(texture, first_mip, kPitchAlignment)) out.push_back(FootprintOf(level, texture.format));
  return out;
}

// The staging memory a texture's upload takes from the upload ring: every level's rows, each level starting on kOffsetAlignment (the worst case: the ring pads the offset up to it).
inline uint64_t StagingBytes(const cf::HitTexture& texture, uint32_t first_mip) {
  uint64_t total = 0;
  for (const Footprint& f : FootprintsOf(texture, first_mip)) total += (f.staging_bytes + kOffsetAlignment - 1) / kOffsetAlignment * kOffsetAlignment;
  return total;
}

// A footprint Plume's D3D12 backend accepts for a copy at `offset` of staging memory (the asserts in toD3D12), and that holds the whole level: rows of whole blocks, a pitch of 256 bytes, an
// offset of 512, a row wide enough for the level.
constexpr bool FootprintAccepted(const Footprint& f, cf::HitTextureFormat format, uint64_t offset) {
  const uint32_t bw = BlockWidth(format);
  const D3d12Footprint d = PlumeD3d12(f, format);
  return f.width != 0 && f.height != 0 && offset % kOffsetAlignment == 0 && d.row_pitch % kPitchAlignment == 0 && f.pitch == d.row_pitch &&
         d.row_pitch >= ((f.width + bw - 1) / bw) * BlockBytes(format) && f.rows == (f.height + bw - 1) / bw && f.row_width % bw == 0 && f.row_width >= f.width &&
         f.staging_bytes == uint64_t(f.pitch) * f.rows;
}

// ---------------------------------------------------------------------------
// the video memory a texture takes
// ---------------------------------------------------------------------------

inline constexpr uint64_t kSmallAlignment = 4096;
inline constexpr uint64_t kLargeAlignment = 65536;
inline constexpr uint64_t kSmallBaseLimit = 65536;  // a first resident level of this many bytes or less qualifies for the small alignment

// What the budget counts for texture `index` resident from `first_mip`: the chain's bytes rounded up to the allocator's unit for it.
inline uint64_t AllocationBytes(const cf::HitTexture& texture, uint32_t first_mip) { return cf::AccountedBytes(texture, first_mip, kSmallAlignment, kLargeAlignment, kSmallBaseLimit); }

// ---------------------------------------------------------------------------
// the budgets
// ---------------------------------------------------------------------------

struct TextureBudget {
  uint32_t cap_px = 0;           // the longest side a resident texture keeps (0: the file's full size)
  uint64_t bytes = 0;            // the resident set's cap, in AllocationBytes
  uint32_t upload_bytes = 0;     // texture bytes made resident in one frame
  uint32_t uploads = 0;          // textures made in one frame
  bool on() const { return bytes != 0; }
};

// M2 section 8's texture caps (128 / 256 / full) and the bytes that follow from counting a texture in 4 and 64 KiB units. The cut-out scope (the sun shadows' alpha) needs the shape of a
// cut-out, not its detail: 128 / 256 / 512 px, a few dozen textures a sector at most. CHECK: the byte numbers are estimates until the first memory line (the retail set at full detail is 233 MB
// of data in 8,342 pictures; its allocation under the rule above is read from the census).
constexpr TextureBudget BudgetFor(gpu_class::GpuClass, Level level, TextureScope scope) {
  if (level == Level::kOff || scope == TextureScope::kNone) return TextureBudget{};
  // a Mid card asked for ultra by hand gets the High class's high, as the pool does (city_hit_pool_rules.h PoolBudgetFor)
  const bool ultra = level == Level::kUltra;
  TextureBudget b;
  if (scope == TextureScope::kAlpha) {
    b.cap_px = ultra ? 512u : 256u;
    b.bytes = (ultra ? 32 : 24) * kMiB;
    b.upload_bytes = uint32_t(2 * kMiB);
    b.uploads = 8;
    return b;
  }
  b.cap_px = ultra ? 0u : 256u;
  b.bytes = (ultra ? 256 : 128) * kMiB;
  b.upload_bytes = uint32_t(6 * kMiB);
  b.uploads = 16;
  return b;
}

// The residency's configuration for a budget and the slots free now: the other fields keep ResidencyConfig's defaults (reads in flight, idle time, retry time).
inline cf::ResidencyConfig ConfigFor(const TextureBudget& budget, uint32_t max_textures, uint64_t effective_bytes) {
  cf::ResidencyConfig c;
  c.cap_px = budget.cap_px;
  c.budget_bytes = effective_bytes;
  c.upload_bytes_per_frame = budget.upload_bytes;
  c.max_uploads_per_frame = budget.uploads;
  c.max_textures = max_textures;
  c.allocation_alignment = kSmallAlignment;
  c.large_alignment = kLargeAlignment;
  c.small_base_limit = kSmallBaseLimit;
  return c;
}

// ---------------------------------------------------------------------------
// pressure
// ---------------------------------------------------------------------------

// Frames between two looks at the adapter's video memory (a query a few microseconds long, but there is no use in making it every frame).
inline constexpr uint32_t kPressureCheckFrames = 30;
// The set may grow while its next `margin` bytes keep the use under vram_room's line; the margin is a few frames of reads, uploads and the game's own growth.
inline constexpr uint64_t kGrowthMargin = 64 * kMiB;
constexpr bool GrowthAllowed(uint64_t used, uint64_t budget, uint64_t margin) { return vram_room::HasRoom(used, budget, margin); }
// The byte cap the residency gets: the class's, or what is held now when the video memory is nearly full (what is resident stays, nothing new is read), never 0 (0 is "no limit").
constexpr uint64_t EffectiveBytes(uint64_t budget_bytes, uint64_t held_bytes, bool growth_allowed) {
  if (growth_allowed) return budget_bytes;
  return std::max<uint64_t>(std::min(budget_bytes, held_bytes), 1);
}

// ---------------------------------------------------------------------------
// a sector's textures
// ---------------------------------------------------------------------------

// The textures a sector detail's runs can show, kept for both scopes (the scope can change while the sector stays: the reflections turn on).
struct SectorTextures {
  std::vector<uint16_t> alpha;  // the cut-out runs' (the shadows)
  std::vector<uint16_t> all;    // every run's (the reflections, the global illumination)
  const std::vector<uint16_t>& For(TextureScope scope) const { return scope == TextureScope::kAll ? all : alpha; }
};

inline SectorTextures CollectSector(const std::vector<cf::HitRun>& runs, const uint32_t* material_words, size_t material_count) {
  SectorTextures s;
  s.alpha = cf::CollectTexturesOfWords(runs, material_words, material_count, true);
  s.all = cf::CollectTexturesOfWords(runs, material_words, material_count, false);
  return s;
}

// How soon a sector's textures are wanted: its distance from the camera in metres (smaller = sooner), the number the residency sorts by.
inline float SectorPriority(float distance_m) { return std::max(distance_m, 0.0f); }
// A sector's priority is refreshed every this many frames (the camera moves a few metres in that time; the sort changes by the sector order, not by the metre).
inline constexpr uint32_t kPriorityRefreshFrames = 30;

}  // namespace mcla::gpu::city_texture
