#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's hit data in the GPU: the pool, the sector table and the tables the hit shading reads (CITY-RT M2 package H3)
// ===========================================================================
// What a ray that hits the city needs beyond the triangle (texture coordinates, vertex colour, the material, the texture) lives in the file mcla_city_hit.v1.pack
// (src/citypack/city_hit_format.h), built from the player's game files. This class keeps it in the GPU, next to the structures (city_rt_scene.h), as four buffers the ray tracing
// descriptor set carries after the car scene's own (city_hit_common.hlsli):
//
//   t6  gCityPool       ONE ByteAddressBuffer of 64 to 256 MiB (city_hit_pool_rules.h PoolBudgetFor): a block for every sector detail's indices, positions, run ids, run table and corners
//   t7  gCitySectors    the sector table, two uint4 a slot (slot = InstanceID & 0x7FFFFF): where each block is; a slot of flags 0 has no hit data. A copy for every frame slot, written by the CPU
//   t8  gCityMaterials  the global material table, from the file, uploaded once
//   t9  gCityTextures   the texture table, one uint4 a texture: H2's loader writes a row (SetTextureRow) when a texture is uploaded or given back
//
// THE POOL IS THE BUILD'S INPUT. A sector detail with hit data is uploaded ONCE: the block holds the positions and the indices, and the bottom level structure is built over those very
// bytes (the geometry names the pool at the block's offsets); nothing is given back after the build, as M1 did without hit data. The block is also what the hit shading reads.
//
// WHAT CityRtScene DOES WITH IT (city_rt_scene.cpp): the worker thread reads the sector's hit blob beside the base mesh (ReadBlob); BuildStaged calls Reserve (plan the block, place it, write
// it into staging), RecordCopy (staging to the pool), builds the structure over the block, Commit (the row is valid); Release calls Evict (the row is cleared now, the range comes back when
// the GPU is done with the frames in flight). Record (once a frame, before the builds) uploads the material table when it is new and keeps the pool's barriers.
//
// WHO WANTS IT. BeginFrame(level) every frame, from the scene service (rt_scene_service.h): the level of the hit data the consumers asked for. Off for 600 frames and the pool and the
// tables go back. A level change that changes the pool's size, or a pool that is made or dropped, changes epoch(): structures built under another epoch have no blocks (or blocks that
// are gone), so CityRtScene gives them back and builds them again.
//
// MEMORY. The pool's size is the class's, or less where the video memory has no room (city_hit_pool_rules.h FitCapacity: the level steps down, then to none, and the shadows go on).
// HeldBytes() is what the renderer's census and VideoMemoryBudget's holder list say. The unit tests of the rules are city_hit_pool_rules_test.cpp.
//
// Threading: the render thread, except ReadBlob (the worker). Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "citypack/hit_reader.h"
#include "citypack/pack_reader.h"
#include "city_hit_pool_rules.h"
#include "city_hit_probe.h"
#include "frame.h"
#include "upload.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;

class CityHitScene {
 public:
  // The registers in the ray tracing descriptor set (city_hit_common.hlsli).
  static constexpr uint32_t kPoolRegister = 6;
  static constexpr uint32_t kSectorsRegister = 7;
  static constexpr uint32_t kMaterialsRegister = 8;
  static constexpr uint32_t kTexturesRegister = 9;
  static constexpr uint32_t kBindingCount = 4;
  // Frames without a want after which the pool and the tables go back (10 s at 60).
  static constexpr uint32_t kIdleReleaseFrames = 600;

  // The descriptor ranges' indices in the set builder the bindings were added to (what Bind writes through).
  struct Bindings {
    uint32_t pool = 0;
    uint32_t sectors = 0;
    uint32_t materials = 0;
    uint32_t textures = 0;
  };
  // Adds t6..t9 to the ray tracing set's builder (CarRtScene::Initialize, before the builder's end()); the set is then created with kBindingCount more descriptors.
  static Bindings AddBindings(plume::RenderDescriptorSetBuilder& builder);

  CityHitScene() = default;
  ~CityHitScene();
  CityHitScene(const CityHitScene&) = delete;
  CityHitScene& operator=(const CityHitScene&) = delete;

  // `hit_file` is where the hit file should be (the shader pack folder's `city` folder, citypack::kHitFileName); it is opened when first wanted and the base pack is open.
  bool Initialize(Device& device, FrameRing& frames, UploadRing& upload, PassTiming& pass_timing, const std::filesystem::path& hit_file, gpu_class::GpuClass cls);
  // GPU idle and the FrameRing drained.
  void Shutdown();
  bool initialized() const { return device_ != nullptr; }

  // Every frame: the level of hit data the consumers asked for (the scene service's merge). Makes and gives back the pool.
  void BeginFrame(city_rt::Level level);
  city_rt::Level level() const { return level_; }
  // The base pack is open (CityRtScene::OpenPack): the sector table's size and the check the hit file is matched to. Null again when the pack is given up.
  void SetBase(const larecomp::citypack::PackReader* base);

  // The pool exists, the table is sized, the file is open: blocks may be reserved this frame.
  bool ready() const { return level_ != city_rt::Level::kOff && pool_ != nullptr && book_ && book_->ready(); }
  // Changes whenever structures built before no longer match the hit data (the pool was made, dropped or resized).
  uint64_t epoch() const { return epoch_; }
  // The city budget with the pool's cap and staging (city_hit_pool_rules.h WithPool), when ready; else as given.
  city_rt::Budget Adjust(const city_rt::Budget& budget) const { return ready() ? city_hit::WithPool(budget, pool_budget_) : budget; }

  // --- the worker thread ---
  // The decoded hit blob of a sector detail (the file is opened by each call). False with an empty error: the file has no blob for that mesh (the block then holds geometry only).
  bool ReadBlob(uint32_t sector, larecomp::citypack::MeshKind kind, larecomp::citypack::HitBlob* out, std::string* error) const;
  bool file_open() const { return reader_open_; }

  // --- the render thread: the frame ---
  // Before the builds, outside any render pass: uploads the material table when it is new.
  void Record(plume::RenderCommandList* list, uint32_t slot);

  // A block for a sector detail, planned, placed and written into staging.
  struct Reservation {
    enum class Why : uint8_t { kNone, kNotReady, kNoRoom, kNoStaging, kBadData };
    bool ok = false;
    Why why = Why::kNone;
    std::string detail;  // kBadData: what FillPoolBlock said
    uint64_t base = 0;
    larecomp::citypack::PoolBlockPlan plan;
    UploadSpan span;
    uint32_t checksum = 0;
    uint32_t samples = 0;
    uint32_t sector = 0;
    bool lod = false;
    bool copied = false;
  };
  // `blob` null: the block holds the indices and positions only (no runs, no corners). The upload staging is the frame slot's.
  Reservation Reserve(uint32_t sector, bool lod, const larecomp::citypack::MeshData& mesh, const larecomp::citypack::HitBlob* blob, uint32_t frame_slot);
  // The block's bytes from staging into the pool (the pool is moved to the copy state once for the frame's copies; EndUpload ends it).
  void RecordCopy(plume::RenderCommandList* list, Reservation* reservation);
  // After the frame's copies and before the structures are built over the blocks: the pool is readable (build input, shader resource).
  void EndUpload(plume::RenderCommandList* list);
  // The pool's buffer and the block's offsets for a structure's build geometry.
  plume::RenderBuffer* pool_buffer() const { return pool_.get(); }
  // The structure over the block was built: the row becomes valid. False: refused (the block is given back at once).
  bool Commit(const Reservation& reservation, uint32_t triangle_count, uint32_t opaque_triangles, uint32_t extra_flags);
  // A reservation that was never copied (the build was given up before): the range is free at once.
  void Abandon(const Reservation& reservation);
  // A block that was copied but whose structure was not built: the row stays invalid and the range comes back when the GPU is done.
  void Discard(const Reservation& reservation);
  // The sector detail's structure was given back: the row is cleared now, the range later.
  void Evict(uint32_t sector, bool lod);

  // --- the tables ---
  // H2: the texture table (t9): one row a texture. Reset sizes it; Set writes a row (a texture uploaded: its slot; given back: kNoSlot).
  void ResetTextures(uint32_t count) { textures_.Reset(count); }
  void SetTextureRow(uint32_t index, const larecomp::citypack::GpuTextureRow& row) { textures_.Set(index, row); }
  uint32_t texture_count() const { return textures_.count(); }
  // Writes t6..t9 of a frame slot's descriptor set (after the top level's own writes). The tables are copied into the slot's buffers when they changed. What is not there is bound to a
  // small zeroed stand-in, so that a shader that reads before the data exists finds "no hit data".
  void Bind(plume::RenderDescriptorSet* set, const Bindings& bindings, uint32_t slot);

  // mcla_gpu_city_hit_probe (debug): records the probe (city_hit_probe.h) in a frame whose top level and descriptors t6..t9 are written. `scene_set` is the frame slot's ray tracing set, `scene_desc`
  // its description. Nothing when the setting is off or the pool is not ready.
  void RecordProbe(plume::RenderCommandList* list, plume::RenderDescriptorSet* scene_set, const plume::RenderDescriptorSetDesc& scene_desc, HostPasses& passes, BindlessHeap& heap);

  // --- what the pool holds ---
  struct Stats {
    uint64_t pool_bytes = 0;       // the buffer's size (0: none)
    uint64_t used_bytes = 0;       // the allocator's use: blocks + waiting to be freed
    uint64_t committed_bytes = 0;  // blocks that are visible
    uint32_t blocks = 0;
    uint64_t pending_free = 0;
    uint32_t fragmentation = 0;    // percent
    uint64_t reserved = 0, committed = 0, evicted = 0, failures_no_room = 0, failures_data = 0, failures_staging = 0;
    uint64_t staged_bytes = 0;     // copied to the pool since the start
  };
  Stats stats() const;
  city_hit::Held Held() const;
  uint64_t HeldBytes() const { return Held().Total(); }
  std::string CensusText() const;
  // The book's own check ("" when sound), for the log and the run's gates.
  std::string Verify() const { return book_ ? book_->Verify() : std::string(); }
  const city_hit::PoolBook* book() const { return book_.get(); }
  const city_hit::TextureTable& textures() const { return textures_; }
  const std::vector<uint32_t>& material_words() const { return reader_.material_words(); }

 private:
  enum class Access : uint8_t { kNone, kCopyWrite, kRead };
  struct SlotTables {
    std::unique_ptr<plume::RenderBuffer> sectors;
    uint8_t* sectors_cpu = nullptr;
    uint64_t sectors_bytes = 0;
    uint64_t sectors_version = 0;
    std::unique_ptr<plume::RenderBuffer> textures;
    uint8_t* textures_cpu = nullptr;
    uint64_t textures_bytes = 0;
    uint64_t textures_version = 0;
  };
  struct Standin {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t* cpu = nullptr;
  };

  bool OpenFile();
  bool MakePool();
  void DeferFree(const city_hit::Block& block);
  void DropPool(const char* why);
  bool EnsureTables(SlotTables& slot, uint32_t index);
  uint64_t SectorTableBytes() const;
  bool MakeStandin(Standin& s, uint64_t bytes, const void* fill, uint64_t fill_bytes, const char* name);
  std::unique_ptr<plume::RenderBuffer> MakeUpload(uint64_t bytes, uint8_t** cpu, const char* name);
  void SetPoolAccess(plume::RenderCommandList* list, Access access);
  void LogState(const char* why);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  UploadRing* upload_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  gpu_class::GpuClass class_ = gpu_class::GpuClass::kLow;
  std::filesystem::path hit_file_;

  // The setting and the frame.
  city_rt::Level level_ = city_rt::Level::kOff;
  uint64_t frame_ = 0;
  uint64_t last_wanted_frame_ = 0;
  uint64_t epoch_ = 1;
  uint64_t retry_frame_ = 0;       // the file or the pool is tried again at this frame
  uint64_t requested_capacity_ = 0;  // the class's size for the level the pool was made for (before the video memory's say)
  int logged_state_ = -1;

  // The base pack and the file.
  const larecomp::citypack::PackReader* base_ = nullptr;
  larecomp::citypack::HitReader reader_;
  bool reader_open_ = false;
  std::string file_problem_;

  // The pool.
  std::unique_ptr<plume::RenderBuffer> pool_;
  city_hit::PoolBudget pool_budget_;
  std::shared_ptr<city_hit::PoolBook> book_;  // shared: the deferred frees (FrameRing::RetireAction) hold it, so they are safe whenever the ring runs them
  Access access_ = Access::kNone;
  bool copied_this_frame_ = false;
  std::unique_ptr<plume::RenderBuffer> materials_;
  bool materials_pending_ = false;
  uint64_t materials_bytes_ = 0;
  city_hit::TextureTable textures_;
  std::array<SlotTables, kFramesInFlight> slots_;
  Standin standin_pool_, standin_sectors_, standin_materials_, standin_textures_;
  uint64_t standin_bytes_ = 0;
  uint32_t sector_count_ = 0;

  CityHitProbe probe_;
  mutable Stats counts_;
  bool first_block_logged_ = false;
};

}  // namespace mcla::gpu
