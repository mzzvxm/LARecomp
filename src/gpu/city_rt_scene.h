#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city as ray tracing acceleration structures (CITY-RT M1)
// ===========================================================================
// The city pack (src/citypack, built from the player's own game files: 439 streamed sectors and the far level of detail, every position in world space) is kept
// round the camera as bottom level structures, one for a sector's hi detail and one for its lod, and the top level the car scene builds every frame (car_rt_scene.h)
// holds the nearest of each sector: the structures of the city and the cars are ONE top level in the frame's view space, so one ray query sees both. The pass that uses
// them is city_rt_shadow.h (the sun's shadows); reflections and global illumination come after it (CITY-RT M2, M3).
//
// WHAT IT DOES, in the order of a frame:
//   BeginFrame(level)      the setting's level, the class's budget (city_rt_rules.h); asks nothing of the device
//   Prepare(...)           called by CarRtScene::OnResolve at the history trigger, before the car scene's top level is built:
//                            - the camera's place in the world (the inverse of the frame's view matrix)
//                            - reads that finished are taken in (a worker thread reads and decodes the pack's blobs: PackReader::ReadMesh)
//                            - the streaming plan (city_rt_rules.h PlanStreaming): reads to ask for, structures to give back
//                            - the frame's builds (PlanBuilds: nearest first within the class's triangles and bytes): each mesh is uploaded (positions float3, indices
//                              uint32) into a buffer of its own, built into a bottom level structure (PREFER_FAST_TRACE; the opaque triangles are one geometry flagged
//                              opaque, the alpha tested ones a second, not opaque) over the slot's scratch, and its input buffer is given back (a shadow query needs the
//                              structure alone: the vertex and index data are not kept, alpha geometry's texture is not in them)
//                            - the instances: for every sector the hi structure once it is built, else the lod, never both; all share the world-to-view transform of the
//                              cars' instances (car_rt_scene_rules.h PackInstanceTransform with an identity model), mask kMaskCity (0x80), InstanceID
//                              city_rt::InstanceId(sector, detail)
//   the pass               traces against the top level; SafeRange() says how far from the camera its answer holds (the structures are the game's own hi detail there)
//
// WITH HIT DATA (CITY-RT M2 H3, city_hit_scene.h): when the scene service says some consumer reads the city's hit data and the pool is made, a mesh is uploaded ONCE, as a block of the pool
// (indices, positions, run ids, the run table, the corners), the structure is built over those very bytes and the block stays; its sector table row becomes valid when the build is
// recorded and is cleared when the structure is given back (the range later, when the GPU is done). The worker reads the hit blob beside the base mesh. Structures built before the
// pool was made (or under another pool) have no blocks: they are given back and built again, once (CityHitScene::epoch). Without hit data nothing below changes.
//
// MEMORY. A structure is 57 to 96 bytes a triangle (no compaction: Plume has no post-build compaction query; a Plume change that adds one shrinks this by a third or
// more, and Budget's cap is in structure bytes, so it needs no change here). The class budgets are city_rt_rules.h BudgetFor: Mid 96 MiB (hi detail within 150 m),
// High 256 MiB (300 m), ultra 512 MiB (600 m). Everything is given back after kIdleReleaseFrames frames without a request.
//
// D3D12 with DXR 1.1 (inline ray queries) and, where Plume's ray query work is in the pin, Vulkan: the same decision as the car scene (CarRtSupported).
// Threading: the render thread, and one worker thread that only reads the pack.
// ===========================================================================

#include <array>
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
#include "citypack/pack_reader.h"
#include "city_rt_rules.h"
#include "frame.h"
#include "temporal_math.h"

namespace mcla::gpu {

namespace citypack = larecomp::citypack;

class CityHitScene;
class Device;
class GpuTiming;
class PassTiming;
class UploadRing;

class CityRtScene {
 public:
  // Frames without a request after which every structure is given back (10 s at 60).
  static constexpr uint32_t kIdleReleaseFrames = 600;
  // A read or a build that failed is tried again after this many frames.
  static constexpr uint32_t kRetryFrames = 600;
  // Ask the pack for its presence again after this many frames when it was not there at the first request (and the app has no say: no probe set). With the app's probe the wait is
  // city_pack_state.h ProbeWaitFrames: 2 s while a build runs, a minute otherwise.
  static constexpr uint32_t kReprobeFrames = 3600;

  CityRtScene() = default;
  ~CityRtScene();
  CityRtScene(const CityRtScene&) = delete;
  CityRtScene& operator=(const CityRtScene&) = delete;

  // `pack_file` is where the pack should be (the shader pack folder's `city` folder, citypack::kPackFileName); it is opened at the first request, so a start without the
  // pack costs nothing. `cls` is the device's class (gpu_class.h): its budgets. False: nothing can be traced (the caller has checked CarRtSupported).
  bool Initialize(Device& device, FrameRing& frames, UploadRing& upload, PassTiming& pass_timing, GpuTiming& timing, const std::filesystem::path& pack_file, gpu_class::GpuClass cls);
  // GPU idle and the FrameRing drained.
  void Shutdown();
  bool initialized() const { return device_ != nullptr; }

  // The app's say on the pack (city_pack_state.h, gpu.h SetCityPackHooks), before the first frame: `probe` says which state the pack is in for the player's game files (stat only) and the pack is
  // opened only when it is current; `request_build` is called once when it is missing or stale (the app builds it in the background and the probe answers "being built" from then on). Both
  // empty: the pack is opened when it is there, as a pack built by hand with tools/citypack_tool is.
  void SetPackProbe(std::function<city_rt::PackState(std::string* why)> probe, std::function<void()> request_build) {
    probe_ = std::move(probe);
    request_build_ = std::move(request_build);
  }
  // CITY-RT M2 H3: the hit data's pool (city_hit_scene.h), before the first frame. Null: no hit data, the structures are built as in M1.
  void SetHitScene(CityHitScene* hit) { hit_ = hit; }

  // Every frame, from the effect that wants the city's structures (the shadow pass's BeginFrame): `level` is the setting's, `radius_override` the debug radius (0 = the class's).
  void BeginFrame(city_rt::Level level, float radius_override);
  bool wanted() const { return level_ != city_rt::Level::kOff; }
  // The pack is open and holds sectors (false before the first request, and when the pack is not there).
  bool pack_ready() const { return pack_.is_open(); }
  // The open pack (its lamps section is what the ray tracing world frame's lamp table reads, rt_world_frame.h), null while none is open. Render thread; the pointer stays valid until the pack is given up.
  const citypack::PackReader* pack_if_open() const { return pack_.is_open() ? &pack_ : nullptr; }

  // Called by CarRtScene::OnResolve in the frame's trigger, after the cars' instances are made and before the top level is described: reads, plan, builds (recorded on
  // `list`, outside any render pass; `slot` is the frame's slot), and the city's instances appended to `instances` (in the frame's view space). `view` is the frame's
  // camera (world to view, row vectors), `flip_z` the car scene's decision about its z. Returns the instances appended. Nothing when not wanted or the pack is not there.
  uint32_t Prepare(const temporal::Mat4& view, bool flip_z, plume::RenderCommandList* list, uint32_t slot, std::vector<plume::RenderTopLevelASInstance>* instances);
  // True when Prepare recorded GPU work in this frame's list (the caller's list state is gone then, as the car scene's builds).
  bool recorded() const { return recorded_; }

  // How far from the camera a shadow from this frame's structures may be applied (city_rt_rules.h SafeReceiverRange): the metres, in this frame. `game_hi_radius` is the game's own
  // hi detail radius (300 m times lod_city_scale).
  float SafeRange(float game_hi_radius) const;
  // The far reach the structures have this frame, metres (rays longer than this meet nothing: the lod ends there).
  float FarRadius() const { return budget_.far_radius; }
  // The camera's world position of the frame Prepare last saw.
  const double* camera() const { return camera_; }

  // CITY-RT M3 G2: the structures built since the last call, as boxes in the world (the sector's box from the pack, metres), for the probe update: the probes near a built sector have data that is stale
  // (their rays can meet geometry that was not there). At most kBuiltLogMax boxes are kept between two calls; `*dropped` (optional) is how many more were built.
  static constexpr uint32_t kBuiltLogMax = 256;
  struct BuiltBox {
    double lo[3] = {0, 0, 0};
    double hi[3] = {0, 0, 0};
    bool hi_detail = false;
  };
  void DrainBuilt(std::vector<BuiltBox>* out, uint32_t* dropped);

  struct Stats {
    uint32_t sectors = 0;        // in the pack
    uint32_t hi_resident = 0;
    uint32_t lod_resident = 0;
    uint32_t loading = 0;        // reads in flight and meshes waiting to be built
    uint32_t instances = 0;      // in this frame's top level
    uint64_t triangles = 0;      // in the structures
    uint64_t structure_bytes = 0;
    uint64_t builds = 0;         // since the start
    uint64_t build_triangles = 0;
    uint64_t evictions = 0;
    uint64_t failures = 0;
  };
  Stats stats() const;
  // The video memory the structures, their scratch and the meshes waiting hold now.
  uint64_t HeldBytes() const;
  // The text of the renderer's city census line (Renderer::ReportCityCensus: the structures, then the hit data's pool); empty where there is nothing.
  std::string CensusText() const;
  void ObserveBuildGpu(uint64_t ns, uint64_t triangles) { build_cost_.Observe(ns, triangles); }

 private:
  // One structure.
  struct Structure {
    std::unique_ptr<plume::RenderBuffer> buffer;  // the structure's own buffer (AccelerationStructureBuffer), the instance names it at offset 0
    std::unique_ptr<plume::RenderAccelerationStructure> structure;
    uint64_t bytes = 0;
    uint32_t triangles = 0;
  };
  struct SectorSlot {
    Structure hi, lod;
  };
  // A mesh the worker read, waiting for the trigger's build.
  struct Staged {
    uint32_t sector = 0;
    city_rt::Detail detail = city_rt::Detail::kHi;
    citypack::MeshData mesh;
    float distance = 0.0f;
    bool hit_wanted = false;   // the read was asked to bring the hit blob (the pool was ready at the plan)
    bool has_hit = false;      // ... and the file had one
    citypack::HitBlob hit;
    uint64_t bytes = 0;        // what the mesh will take of the frame's staging (the pool block with hit data, positions and indices without)
  };
  struct LoadRequest {
    uint32_t sector = 0;
    city_rt::Detail detail = city_rt::Detail::kHi;
    bool with_hit = false;
  };
  struct LoadResult {
    LoadRequest request;
    bool ok = false;
    std::string error;
    citypack::MeshData mesh;
    bool has_hit = false;
    citypack::HitBlob hit;
    std::string hit_error;
  };
  struct SlotResources {
    std::unique_ptr<plume::RenderBuffer> scratch;  // the builds' scratch (one after the other, a barrier between)
    uint64_t scratch_bytes = 0;
  };

  bool OpenPack();
  void StartWorker();
  void StopWorker();
  void WorkerMain();
  void ReleaseAll(const char* why);
  void Release(uint32_t sector, city_rt::Detail detail);
  // A mesh read without the hit blob before the hit data's pool existed (city_rt::ReadBeforePool): the sector detail goes back to kNone and the plan asks again, with the blob.
  void GiveBackForReread(uint32_t sector, city_rt::Detail detail);
  void PollLoads();
  void Plan(const double camera[3]);
  // What a staged mesh takes of the staging: the pool block's bytes when the hit data rides with it, else positions and indices.
  uint64_t StagedBytesOf(const Staged& staged) const;
  bool EnsureScratch(SlotResources& slot, uint32_t index, uint64_t bytes);
  // Uploads and builds the staged meshes the frame's schedule takes. True when anything was recorded.
  bool BuildStaged(plume::RenderCommandList* list, uint32_t slot);
  void RefreshTexturePriorities();
  void LogState(const char* why);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  UploadRing* upload_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GpuTiming* timing_ = nullptr;
  CityHitScene* hit_ = nullptr;  // the hit data's pool (SetHitScene), not owned
  uint64_t hit_epoch_ = 0;       // the pool epoch the built structures were made under
  bool hit_bad_data_logged_ = false;
  gpu_class::GpuClass class_ = gpu_class::GpuClass::kLow;
  std::filesystem::path pack_file_;

  // The setting and the frame.
  city_rt::Level level_ = city_rt::Level::kOff;
  city_rt::Budget budget_;
  city_rt::BuildCost build_cost_;
  uint64_t frame_ = 0;
  uint64_t last_wanted_frame_ = 0;
  uint64_t pack_probe_frame_ = 0;
  bool pack_missing_logged_ = false;
  std::function<city_rt::PackState(std::string* why)> probe_;
  std::function<void()> request_build_;
  uint32_t probe_wait_ = kReprobeFrames;
  bool build_requested_ = false;
  int logged_pack_state_ = -1;  // the last state the log said (one line a change)
  bool recorded_ = false;
  double camera_[3] = {0, 0, 0};
  bool camera_known_ = false;
  std::vector<BuiltBox> built_log_;  // see DrainBuilt
  uint32_t built_dropped_ = 0;
  uint64_t next_priority_frame_ = 0;  // the sectors' distances go to the hit data's texture set again at this frame (CITY-RT M1c)

  // The pack and what is built of it.
  citypack::PackReader pack_;
  std::vector<city_rt::SectorInfo> sectors_;
  std::vector<city_rt::SectorState> states_;
  std::vector<SectorSlot> built_;
  std::vector<Staged> staged_;
  uint64_t structure_bytes_ = 0;
  uint64_t staged_bytes_ = 0;
  std::array<SlotResources, kFramesInFlight> slots_;

  // The worker that reads the pack.
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<LoadRequest> requests_;
  std::deque<LoadResult> results_;
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> reads_in_flight_{0};  // requested and not yet taken in by PollLoads

  // The totals and the log.
  Stats counts_;
  uint32_t instances_this_frame_ = 0;
  uint64_t logged_signature_ = ~uint64_t(0);
  uint64_t log_frame_ = 0;
  bool first_build_logged_ = false;
};

}  // namespace mcla::gpu
