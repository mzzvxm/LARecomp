#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's texture set on the GPU (CITY-RT M2 package H2, finished for the cut-out shadows of M1c)
// ===========================================================================
// The textures a ray's hit on the city shows (src/citypack/texture_set.h: every picture of the city's dictionaries, 138 MB on disk as zstd frames and 232 MB of blocks at full detail, built from
// the player's own game files) made resident as the streaming ring asks, and published to the hit shading as rows of the texture table:
//
//   a GpuTextureRow (16 bytes, city_hit_format.h) a texture: the bindless slot of the texture's view (bit 31 the gamma bit), its full size, the number of resident levels and the first one. A
//   texture that is not resident has slot kNoSlot (0xFFFFFFFF): the hit takes the material's average colour, the cut-out shadows fall back to the stochastic rule: nothing ever waits for a load
//   and nothing is wrong while it loads. The rows go to the sink Initialize was given (CityHitScene::SetTextureRow, which owns t9 and its copy for every frame slot).
//
// What decides what is resident is citypack/texture_residency.h (the cap of the graphics class, the budget, the sector priority, the uploads of a frame) and city_texture_rules.h (the slots,
// the footprints, the memory a texture takes, the budgets by class, level and scope); this class does what they say, once a frame on the render thread (Pump), in this order:
//   1. reads that finished are taken in (a worker thread reads each texture from the file: its chain unpacked from the zstd frame, from the first level the cap keeps),
//   2. the residency's configuration for the frame: the class's cap and bytes, the bindless slots that are free, the video memory's pressure (a look every 30 frames: when the adapter is
//      nearly full the set stops growing),
//   3. the residency plan,
//   4. evictions: the view and the texture are retired (the FrameRing frees them once the GPU is done with the frame that last used them), the bindless slot is released (BindlessHeap: rewritten
//      to the null texture at once, returned to the free stack only when the frame is proven complete), the row reads kNoSlot,
//   5. reads handed to the worker,
//   6. uploads: the texture is created (BC1 / BC2 / BC3 / RGBA8, the levels from the first resident one), its levels are copied through the UploadRing (the footprints of city_texture_rules.h,
//      the arguments the game's own texture cache gives Plume), it goes to SHADER_READ, a view takes a bindless slot, the row is published.
// The row of a texture is written in the same list as the copies of its levels, so a shader of this frame that finds a slot finds a texture that has its data.
//
// WHAT IS WANTED. The scope (city_texture_scope.h) says whose textures: the cut-outs' of every resident sector (the shadows), or every texture of them (the reflections). The sectors are the
// streaming ring's (CityHitScene tells AddSector when a block is committed and RemoveSector when it is evicted, with the sector's distance as its priority, refreshed every 30 frames).
//
// MEMORY. The residency's budget counts each texture as the allocator will (city_texture_rules.h AllocationBytes: 4 KiB units for a small base level, 64 KiB for the rest). HeldBytes() is what is
// resident: the renderer's video memory budget lists it (VideoMemoryBudget::AddHolder: the response to a shortage cannot release it by itself, the class's budget and the pressure rule lower
// what is asked instead). A creation the device refuses gives the texture back and tries again later (ResidencyConfig::retry_frames).
//
// CHECK (nothing here was built or run; each is a line of the first run):
//   1. (closed in part) the PlacedFootprint arguments for BC levels smaller than one block: they are the ones the game's own cache gives (textures.cpp Upload), which Plume's D3D12 backend rounds
//      to the block and its Vulkan backend takes as the buffer row length; city_texture_rules_test.cpp checks them against that arithmetic for every level of a spread of sizes. Still a line
//      of the first run on each backend: no validation message from the copies of a 2 x 2 and 1 x 1 level;
//   2. the real video memory of the set against HeldBytes(): the census line says the accounted MiB, the memory line the adapter's use; D3D12MA's small alignment rule is "conservative" (4 KiB
//      for a base level of 64 KiB or less), the guess here;
//   3. the cost of the views in the bindless heap (kBindlessTextureCount 32768, the game's own textures take about 1,000 to 3,000): the census says the slots in use against the heap;
//   4. a hitch from the creation of a few dozen textures in one frame at the first ring: the uploads are limited by bytes and by count (city_texture_rules.h BudgetFor, 8 or 16 a frame);
//   5. the cost of the zstd unpack on the worker (the set measured 0.1 s for the whole load at CITY-HIT-SIZE; read "[city tex]" read_ms in the census).
//
// Threading: everything but the worker's file reads is the render thread's.
// ===========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "citypack/city_hit_format.h"
#include "citypack/texture_residency.h"
#include "citypack/texture_set.h"
#include "city_texture_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class UploadRing;

class CityTextureSet {
 public:
  // A row of the texture table changed (a texture was uploaded: its slot; given back: kNoSlot).
  using RowSink = std::function<void(uint32_t index, const larecomp::citypack::GpuTextureRow& row)>;

  struct Stats {
    uint64_t reads = 0;            // reads handed to the worker
    uint64_t read_failures = 0;
    uint64_t uploads = 0;          // textures made and filled
    uint64_t upload_bytes = 0;     // block data copied
    uint64_t evictions = 0;
    uint64_t creation_failures = 0;
    uint64_t upload_deferred = 0;  // an upload that found no upload memory this frame
    uint64_t growth_blocked_checks = 0;  // looks at the video memory that said "no growth"
    uint64_t read_ns = 0;          // the worker's time in reads and unpacking
    uint32_t resident = 0;
    uint32_t slots_held = 0;
    uint64_t held_bytes = 0;       // accounted bytes of what is resident (video memory)
    uint64_t committed_bytes = 0;  // ... plus what is being read or waits for its upload
  };

  CityTextureSet() = default;
  ~CityTextureSet();
  CityTextureSet(const CityTextureSet&) = delete;
  CityTextureSet& operator=(const CityTextureSet&) = delete;

  // Opens the texture file against the hit file's texture table and table hash (a file of another build is refused) and starts the reader. Every row is "not resident" until the first Pump
  // publishes one. False (with `error`) when the file does not match.
  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload, const std::filesystem::path& texture_file, std::vector<larecomp::citypack::HitTexture> table,
                  uint64_t hit_table_hash, RowSink sink, std::string* error);
  // Gives every texture back (retired through the FrameRing, so the GPU need not be idle) and stops the reader. Every row is written "not resident" through the sink first.
  void Shutdown();
  bool is_open() const { return device_ != nullptr; }

  // The budget and the scope (city_texture_rules.h BudgetFor), applied by the next Pump. A changed cap gives back what is held at another first level. An off budget (class, level or scope)
  // keeps the set open and empty: nothing is read.
  void Configure(const city_texture::TextureBudget& budget, TextureScope scope);
  TextureScope scope() const { return scope_; }

  // The streaming ring's sectors (the sector row of city_hit_format.h: (sector << 1) | detail): the textures a sector's hits can show (city_texture::CollectSector) for both scopes, and its
  // priority (the ring's distance, smaller = sooner). A sector added again replaces its lists.
  void AddSector(uint32_t row, city_texture::SectorTextures textures, float priority);
  void SetSectorPriority(uint32_t row, float priority);
  void RemoveSector(uint32_t row);
  // Every sector at once (the pool's blocks are gone).
  void ClearSectors();

  // Once a frame, before the passes that read the texture table, outside a render pass. `frame_slot` is the FrameRing slot being recorded.
  void Pump(plume::RenderCommandList* list, uint32_t frame_slot, uint64_t frame);

  // Accounted bytes of what is resident: the video memory it takes by the allocator's rule (the holder of the renderer's budget).
  uint64_t HeldBytes() const { return residency_ ? residency_->resident_bytes() : 0; }
  Stats stats() const;
  std::string CensusText() const;
  const std::vector<larecomp::citypack::HitTexture>& table() const { return table_; }

 private:
  struct Gpu {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = 0xFFFFFFFFu;
  };
  struct Request {
    uint32_t index = 0;
    uint32_t first_mip = 0;
  };
  struct Result {
    uint32_t index = 0;
    bool ok = false;
    uint64_t ns = 0;
    std::vector<uint8_t> bytes;
    std::string error;
  };

  void WorkerMain();
  void StopWorker();
  void TakeResults();
  // Releases the texture's GPU objects and writes its row as not resident. Returns false when there was nothing to release.
  bool Release(uint32_t index);
  // Creates, fills and publishes texture `index` from the bytes the worker read. False: the texture was not made (the residency was told).
  bool Upload(uint32_t index, plume::RenderCommandList* list, uint32_t frame_slot);
  void Publish(uint32_t index, const larecomp::citypack::GpuTextureRow& row);
  larecomp::citypack::GpuTextureRow AbsentRow(uint32_t index) const;
  void Apply(uint64_t frame);
  void RefreshSectors();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  RowSink sink_;
  std::filesystem::path file_;
  uint64_t hit_table_hash_ = 0;
  std::vector<larecomp::citypack::HitTexture> table_;
  std::unique_ptr<larecomp::citypack::TextureResidency> residency_;
  std::vector<Gpu> gpu_;
  std::unordered_map<uint32_t, std::vector<uint8_t>> ready_;  // read, waiting for the frame's upload budget (render thread only)
  std::unordered_map<uint32_t, city_texture::SectorTextures> sectors_;  // the sectors the residency was told about, with both lists (the scope can change)
  std::unordered_map<uint32_t, float> priorities_;
  city_texture::TextureBudget budget_;
  TextureScope scope_ = TextureScope::kNone;
  TextureScope applied_scope_ = TextureScope::kNone;
  uint64_t next_pressure_check_ = 0;
  uint32_t slots_held_ = 0;
  Stats stats_;
  uint64_t frame_ = 0;
  bool logged_pressure_ = false;

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Request> requests_;
  std::deque<Result> results_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> read_ns_{0};
};

}  // namespace mcla::gpu
