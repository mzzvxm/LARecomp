#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's texture set on the GPU (CITY-RT M2 package H2)
// ===========================================================================
// The textures a ray's hit on the city shows (src/citypack/texture_set.h: every picture of the city's dictionaries, 232 MB at full detail in the texture file, built from the player's own
// game files) made resident as the streaming ring asks, and published to the hit shading as ONE table:
//
//   rows_buffer()   StructuredBuffer<uint4> gCityTextures (t9 of the RT descriptor set, city_hit_format.h THE GPU LAYOUT), one GpuTextureRow (16 bytes) a texture: the bindless slot of the
//                   texture's view (bit 31 the gamma bit), its full size, the number of resident levels and the first one. A texture that is not resident has slot kNoSlot (0xFFFFFFFF): the
//                   hit takes the material's average colour (a "fallback colour"): nothing ever waits for a load and nothing is wrong while it loads.
//
// What decides what is resident is citypack/texture_residency.h (the cap of the graphics class, the budget, the sector priority, the 8 MB a frame of uploads); this class does what that says,
// in this order, once a frame, on the render thread (Pump):
//   1. reads that finished are taken in (a worker thread reads each texture's levels from the file: the full chain, or from the first level the cap keeps),
//   2. the residency plan for the frame,
//   3. evictions: the view and the texture are retired (the FrameRing frees them once the GPU is done with the frame that last used them), the bindless slot is released (BindlessHeap:
//      rewritten to the null texture at once, returned to the free stack only when the frame is proven complete), the row reads kNoSlot,
//   4. reads handed to the worker,
//   5. uploads: the texture is created (BC1 / BC2 / BC3 / RGBA8, the levels from the first resident one), its levels are copied through the UploadRing (rows on 256 bytes, levels on 512:
//      the D3D12 placement rules on both backends), it goes to SHADER_READ, a view takes a bindless slot, the row is published,
//   5. the changed rows are copied into rows_buffer() (one copy for the changed range) and the buffer is put back to read.
// The row of a texture is written in the same list as the copies of its levels, so a shader of this frame that finds a slot finds a texture that has its data.
//
// MEMORY. The residency budget counts each texture's levels in whole 4 KiB (AccountedBytes); HeldBytes() is that sum for what is resident, read, or waiting: the renderer adds it as a holder of
// vram_budget.h (VideoMemoryBudget::AddHolder("city textures", ...)): the response to a shortage cannot release it by itself (the class's budget and the governor lower texture_bytes instead).
// A creation the device refuses (a full video memory) gives the texture back and tries again later (ResidencyConfig::retry_frames).
//
// CHECK (nothing here was built or run; each is a line of the first run):
//   1. the PlacedFootprint arguments for BC levels smaller than one block (the 2 x 2 and 1 x 1 levels) on both backends;
//   2. the real video memory of 8,300 small textures on D3D12 (64 KiB placement of committed resources?) against HeldBytes(): read it from the memory line;
//   3. the cost of 8,300 views in the bindless heap (kBindlessTextureCount 32768, the game's own textures take about 1,000 to 3,000);
//   4. a hitch from the creation of a few hundred textures in one frame at the first ring (the uploads are budgeted by bytes, not by count: a count limit may be needed).
//
// Threading: everything but the worker's file reads is the render thread's.
// ===========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
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

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class UploadRing;

class CityTextureSet {
 public:
  struct Stats {
    uint64_t reads = 0;            // reads handed to the worker
    uint64_t read_failures = 0;
    uint64_t uploads = 0;          // textures made and filled
    uint64_t upload_bytes = 0;     // block data copied
    uint64_t evictions = 0;
    uint64_t creation_failures = 0;
    uint64_t upload_deferred = 0;  // an upload that found no upload memory this frame
    uint32_t resident = 0;
    uint64_t held_bytes = 0;
  };

  CityTextureSet() = default;
  ~CityTextureSet();
  CityTextureSet(const CityTextureSet&) = delete;
  CityTextureSet& operator=(const CityTextureSet&) = delete;

  // Opens the texture file against the hit file's texture table and table hash (a file of another build is refused), makes the rows buffer (every row "not resident" until the first Pump
  // writes it) and starts the reader. False (with `error`) when the file does not match or the buffer cannot be made.
  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload, const std::filesystem::path& texture_file,
                  std::vector<larecomp::citypack::HitTexture> table, uint64_t hit_table_hash, std::string* error);
  // The GPU must be idle and the FrameRing drained (as CityRtScene::Shutdown).
  void Shutdown();
  bool is_open() const { return device_ != nullptr; }

  // The level's numbers (HitBudget::texture_cap_px, texture_bytes, texture_upload_bytes), applied by the next Pump. A changed cap gives back what is held at another first level.
  void Configure(const larecomp::citypack::HitBudget& budget);

  // The streaming ring's sectors (the sector row of city_hit_format.h: (sector << 1) | detail): the textures a sector's hits can show (citypack::CollectTextures over its runs) and its
  // priority (the ring's distance, smaller = sooner).
  void AddSector(uint32_t row, const uint16_t* textures, size_t count, float priority);
  void SetSectorPriority(uint32_t row, float priority);
  void RemoveSector(uint32_t row);

  // Once a frame, before the passes that read rows_buffer(), outside a render pass. `frame_slot` is the FrameRing slot being recorded.
  void Pump(plume::RenderCommandList* list, uint32_t frame_slot, uint64_t frame);

  // t9. Null until Initialize succeeded.
  plume::RenderBuffer* rows_buffer() const { return rows_buffer_.get(); }
  uint64_t rows_buffer_bytes() const { return rows_cpu_.size() * sizeof(larecomp::citypack::GpuTextureRow); }
  // The residency's accounted bytes of what is resident, being read or waiting to be uploaded.
  uint64_t HeldBytes() const { return residency_ ? residency_->committed_bytes() : 0; }
  Stats stats() const;
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
  void SetRow(uint32_t index, const larecomp::citypack::GpuTextureRow& row);
  larecomp::citypack::GpuTextureRow AbsentRow(uint32_t index) const;
  void FlushRows(plume::RenderCommandList* list, uint32_t frame_slot);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  std::filesystem::path file_;
  uint64_t hit_table_hash_ = 0;
  std::vector<larecomp::citypack::HitTexture> table_;
  std::unique_ptr<larecomp::citypack::TextureResidency> residency_;
  std::vector<Gpu> gpu_;
  std::unordered_map<uint32_t, std::vector<uint8_t>> ready_;  // read, waiting for the frame's upload budget (render thread only)
  std::vector<larecomp::citypack::GpuTextureRow> rows_cpu_;
  std::unique_ptr<plume::RenderBuffer> rows_buffer_;
  uint32_t dirty_first_ = 0xFFFFFFFFu;
  uint32_t dirty_last_ = 0;
  Stats stats_;
  uint64_t frame_ = 0;

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Request> requests_;
  std::deque<Result> results_;
  std::atomic<bool> stop_{false};
};

}  // namespace mcla::gpu
