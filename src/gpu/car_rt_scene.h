#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car as ray tracing acceleration structures
// ===========================================================================
// For ray traced car-only effects (first: the car's reflections of itself, car_rt_refl). One bottom-level structure per mesh
// of the player's car, built ONCE from guest memory when the mesh is first seen (car_frame.h MeshReader) and kept while its
// key lives; every frame one top-level structure with one instance per draw of the car, placed by the draw's matrix. The
// instances are in the frame's VIEW space (the AO's: x right, y up, z forward = the linear depth t; ao_common.hlsli
// AoViewPos), so a pass at the history trigger traces rays in the same space as the depth image and the history image.
//
// D3D12 with DXR tier 1.1 (inline ray queries, Shader Model 6.5), and Vulkan with VK_KHR_ray_query (Plume's feat/ray-query: the structures
// are VkAccelerationStructureKHR, the shaders SPIR-V 1.5 from the same HLSL); keep the decision in ONE function, CarRtSupported.
//
// car_rt_scene.cpp implements CarRtScene, hlsl/car_rt_common.hlsli its shader side and src/gpu/CMakeLists.txt the function
// larecomp_gpu_host_shader_rt; the ray traced self-reflection (car_rt_refl.h) uses View(), the descriptor set and the .hlsli.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
//
// HOW IT RUNS
//   - A consumer calls Request() in every frame it wants the structures (CarFrame collects the car's draws from the frame after a
//     request on, and Request() asks it to). A request counts for the frame and the next. After 600 frames without one everything is
//     given back (the next request builds the meshes again).
//   - The mesh builds: CarRtScene is a car_frame::Listener. For a body draw whose mesh_key it has not seen, the listener (inside the
//     draw's call, maybe inside a render pass) only READS the triangles from guest memory into CPU vectors (smooth normals made
//     where the declaration had none). At the history trigger (OnResolve: the first copy of the main depth into 078B0000, outside
//     any render pass) the staged meshes are uploaded into large shared buffers (positions float3, indices uint32, normals float3;
//     ranges, so a mesh given back leaves a hole a later one fills), their bottom-level structures are built (PREFER_FAST_TRACE) into
//     shared structure pages, and the structures of this frame's draws go into the top level. A frame builds at most
//     kBuildTrianglesPerFrame triangles; the draws whose mesh waits are left out of the top level until the next frame.
//   - The memory: meshes (positions, normals, indices and structure) stay under 64 MiB; one not named by any draw for 300 frames goes
//     back, and over the cap the least recently seen ones go first (car_rt_scene_rules.h). A range given back is reused only after
//     the GPU is done with the frame that gave it back (FrameRing::RetireAction). The log says the totals the first time and
//     whenever they change by a quarter. A car is about 91,000 triangles and 77,000 vertices: about 1 MiB of positions, 1 MiB of
//     normals, 1.1 MiB of indices and 5 to 8 MiB of structures.
//   - The top level: one instance a body draw with a built structure, the transform the draw's model-to-world matrix through the frame's
//     camera (VIEW space, float 3x4), the mask by the draw's kind (hlsl/car_rt_common.hlsli CAR_RT_MASK_*), InstanceID and
//     InstanceIndex the row of the table (binding 1). The driver (kSkinned draws) is not in the first version. The instance buffer,
//     the table, the scratch and the structure of the top level are per frame slot (the GPU is done with a slot when the FrameRing
//     comes back to it), so nothing here waits for the GPU.
//   - View() is valid only when all of that was recorded on the list the caller's pass runs on, with the barrier that lets a compute
//     pass read the structure. Consumers read it after OnResolve (the Scene calls the passes in that order).
//   - Nothing of the structures is kept in the bindless heap (it does not fit: an acceleration structure descriptor differs from a
//     texture's): the set below is the car RT set, bound by each consumer at a set index of its own pipeline layout (SetDesc()).
//
// THE CHECK (mcla_gpu_car_rt_check, debug): a compute pass after the occlusion's passes traces one primary ray from the view-space origin
// through every vehicle pixel of the occlusion's linear depth image and compares the committed hit's distance with the pixel's (hlsl/
// car_rt_check_cs.hlsl); it counts coverage and error and logs them every 120 frames ("[car rt] check ..."); mcla_gpu_car_rt_view 1/2
// shows the error / the normals over the scene. This is what proves the camera space, the matrices and the meshes on the first run.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

// car_frame.h names these without including them.
#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "input_layout.h"
#include "shaders.h"
#include "topology.h"
#include "car_frame.h"
#include "car_rt_scene_rules.h"
#include "frame.h"
#include "host_passes.h"
#include "records.h"
#include "resolve.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class Device;
class EdramTargets;
class PassTiming;
class UploadRing;

// The descriptor set every ray traced car pass binds (hlsl/car_rt_common.hlsli declares it; the bindings, in this order):
//   0  RaytracingAccelerationStructure  the frame's top level
//   1  StructuredBuffer<uint4>          per instance (InstanceIndex()): x = its mesh's first index in buffer 2 (uint32 units),
//                                       y = its mesh's first vertex in buffer 3, z = flags (car_rt_common.hlsli CAR_RT_*),
//                                       w = the draw's place in car_frame::Frame::draws
//   2  ByteAddressBuffer                every mesh's triangle-list indices (uint32, relative to the mesh's first vertex)
//   3  ByteAddressBuffer                every mesh's vertex normals (float3, 12 bytes a vertex, model space, unit length)
struct CarRtView {
  bool valid = false;                         // this frame's top level was built and recorded on the list before the caller's pass
  plume::RenderDescriptorSet* set = nullptr;  // bound by the caller at its own set index
  uint32_t instance_count = 0;
};

// The decision whether this device can run the ray traced car effects (D3D12: DXR 1.1 and Shader Model 6.5; Vulkan: Plume's rayQuery, that is
// VK_KHR_ray_query with VK_KHR_acceleration_structure, buffer device addresses and Vulkan 1.2). The ONE place for the decision.
bool CarRtSupported(const plume::RenderDeviceCapabilities& caps, bool vulkan);
// Why not, in plain words for the log; empty when it is supported.
std::string CarRtUnsupportedReason(const plume::RenderDeviceCapabilities& caps, bool vulkan);

class CarRtScene final : public car_frame::Listener {
 public:
  // The destination of the game's mid-scene depth copy in the retail build (ao.h, car_selfrefl.h): another build gives another
  // address and then there is no top level, never a wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  CarRtScene() = default;
  ~CarRtScene() override;
  CarRtScene(const CarRtScene&) = delete;
  CarRtScene& operator=(const CarRtScene&) = delete;

  // Only where CarRtSupported said yes. Makes the descriptor sets and, where the shader exists, the check pass's layout and pipeline,
  // and registers with `car_frame` (which must be initialized). False: nothing is made (logged) and the object does nothing.
  bool Initialize(Device& device, FrameRing& frames, UploadRing& upload, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram,
                  PassTiming& pass_timing, CarFrame& car_frame);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();
  bool ready() const { return ready_; }

  // A user wants the structures: call it in every frame it needs them (any time in the frame). A request counts for the frame and the
  // next, and asks CarFrame to collect the car's draws.
  void Request();

  // The Scene's hooks, as car_selfrefl.h's: the game's motion blur draw and the composite come after the trigger, the present ends the frame.
  void OnAnchor() { anchor_seen_ = true; }
  void OnComposite() { composite_seen_ = true; }
  // A resolve, after ResolveCache::Execute and after the ambient occlusion's passes (the check reads their linear depth image): at the
  // frame's trigger it uploads and builds what is new, builds the top level and, with the check on, records the check. `list` is the
  // frame's open list outside any render pass, `slot` its frame slot. True when anything was recorded (the caller's list state is gone).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot,
                 const AmbientOcclusion* ao);
  // A guest present: the frame boundary.
  void OnPresent();

  // This frame's top level and its set (valid after OnResolve, until the present).
  const CarRtView& View() const { return view_; }
  // The set's layout, for the caller's pipeline layout.
  const plume::RenderDescriptorSetDesc& SetDesc() const { return set_builder_.descriptorSetDesc; }

  // The video memory the structures, the shared buffers and the per-slot buffers hold now.
  uint64_t HeldBytes() const;

  // car_frame::Listener: reads the triangles of an unseen mesh into CPU vectors (no GPU work here).
  void OnPlayerDraw(const car_frame::Draw& draw, const car_frame::MeshReader& reader) override;

 private:
  // A mesh read from guest memory, waiting for the trigger.
  struct Staged {
    uint32_t attempts = 0;  // frames it found no room
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    uint32_t triangles = 0;
  };
  // A mesh in the shared buffers, with its bottom-level structure once built.
  struct Mesh {
    uint32_t first_vertex = 0;  // in the vertex ranges (positions and normals share them)
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t page = 0;  // the structure's page, offset and size
    uint64_t structure_offset = 0;
    uint64_t structure_size = 0;
    uint64_t bytes = 0;  // positions, normals, indices and structure
    uint64_t last_seen = 0;
    std::unique_ptr<plume::RenderAccelerationStructure> structure;
  };
  // A shared buffer and what its last barrier left it for (Plume tracks the D3D12 state on the buffer itself; this says whether the next
  // use needs a barrier).
  enum class Access : uint8_t { kNone, kCopyWrite, kCopyRead, kRead };
  struct Shared {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint64_t bytes = 0;
    Access access = Access::kNone;
  };
  // A page of structures: one buffer, with the ranges its structures hold.
  struct Page {
    std::unique_ptr<plume::RenderBuffer> buffer;
    car_rt_rules::RangeAllocator ranges;
    uint64_t bytes = 0;
  };
  // What a frame slot owns (the GPU is done with a slot when the FrameRing comes back to it).
  struct SlotResources {
    std::unique_ptr<plume::RenderBuffer> tlas_buffer;  // the top level's structure, at offset 0 of its own buffer
    std::unique_ptr<plume::RenderAccelerationStructure> tlas;
    uint64_t tlas_bytes = 0;
    std::unique_ptr<plume::RenderBuffer> scratch;  // the builds' scratch (the bottom levels one after the other, then the top level)
    uint64_t scratch_bytes = 0;
    std::unique_ptr<plume::RenderBuffer> instances;  // the top level's instance descriptions (an upload buffer, mapped)
    uint8_t* instances_cpu = nullptr;
    uint64_t instances_bytes = 0;
    std::unique_ptr<plume::RenderBuffer> table;  // the per-instance rows (an upload buffer, mapped)
    uint8_t* table_cpu = nullptr;
    uint64_t table_bytes = 0;
    std::unique_ptr<plume::RenderDescriptorSet> set;        // the car RT set
    std::unique_ptr<plume::RenderDescriptorSet> check_set;  // the check pass's own (u0 the picture, u1 the counters)
  };
  struct Placed;

  // The check pass's push block (hlsl/car_rt_check_cs.hlsl CheckPush), 32 bytes.
  struct CheckPush {
    uint32_t slots[4];  // the linear depth image's slot, its width and height, the flags (bit 0 write the picture, bits 8-15 the view mode)
    float lens[4];      // tan of the half field of view (x, y), the ray's start and end
  };
  static_assert(sizeof(CheckPush) == 32);
  // The display pass's push block (hlsl/car_rt_view_ps.hlsl CarRtViewPush), 32 bytes.
  struct ViewPush {
    uint32_t slots[4];
    float sizes[4];
  };
  static_assert(sizeof(ViewPush) == 32);

  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  bool Wants() const { return requested_ || requested_prev_; }

  // --- the shared buffers ---
  bool MakeShared(Shared& shared, uint64_t bytes, plume::RenderBufferFlags flags, const char* name);
  void SetAccess(plume::RenderCommandList* list, Shared& shared, Access access);
  // Grows a shared buffer pair to hold `needed_units` (units of `unit_bytes`), copying what the ranges hold.
  bool GrowVertices(plume::RenderCommandList* list, uint64_t needed_vertices);
  bool GrowIndices(plume::RenderCommandList* list, uint64_t needed_indices);
  bool AllocVertices(plume::RenderCommandList* list, uint32_t count, uint32_t* first);
  bool AllocIndices(plume::RenderCommandList* list, uint32_t count, uint32_t* first);
  bool AllocStructure(uint64_t size, uint32_t* page, uint64_t* offset);
  // Gives a mesh's ranges back once the GPU is done with the frame being recorded, and its structure with them.
  void FreeRanges(uint32_t first_vertex, uint32_t vertex_count, uint32_t first_index, uint32_t index_count, int64_t page,
                  uint64_t structure_offset, uint64_t structure_size);
  void Evict(uint64_t key);

  // --- the frame ---
  // Gives back what the policy says (car_rt_scene_rules.h PlanEviction) with `need_bytes` about to be added; false: they do not fit the cap.
  bool Evictions(uint64_t need_bytes);
  // Uploads the staged meshes within the frame's budget into the shared buffers, places their structures and records their builds.
  void PlaceStaged(plume::RenderCommandList* list, uint32_t slot, std::vector<Placed>* placed);
  bool EnsureSlot(SlotResources& slot, uint32_t index, uint64_t tlas_bytes, uint64_t scratch_bytes, uint64_t instance_bytes,
                  uint64_t table_bytes);
  void LogTotals(const char* why);
  void ReleaseAll(const char* why);
  void ReleaseSlot(SlotResources& slot);

  // --- the check ---
  bool MakeCheck(HostPasses& passes);
  void RecordCheck(plume::RenderCommandList* list, uint32_t slot, const ResolveCache::Summary& summary, const AmbientOcclusion* ao);
  bool RecordView(plume::RenderCommandList* list, const ResolveCache::Summary& summary, uint32_t image_slot, uint32_t image_width,
                  uint32_t image_height);
  plume::RenderPipeline* ViewPipeline(uint32_t samples);
  void ProcessReadback(uint64_t frame, uint64_t generation, uint32_t instances, uint32_t width, uint32_t height);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  UploadRing* upload_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarFrame* car_frame_ = nullptr;
  bool ready_ = false;
  bool listener_added_ = false;

  // The frame.
  uint64_t frame_ = 0;
  uint64_t generation_ = 0;  // bumped when everything is given back: a deferred free of an older generation is dropped
  bool requested_ = false;
  bool requested_prev_ = false;
  uint32_t idle_frames_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  CarRtView view_;
  int flip_state_ = 0;  // 1: the frame's camera has z backward (the game's own) and is flipped; -1: z forward already; 0 not decided yet
  uint32_t staged_triangles_ = 0;  // read this frame

  // The meshes.
  std::unordered_map<uint64_t, Mesh> meshes_;
  std::unordered_map<uint64_t, Staged> staged_;
  std::vector<uint64_t> staged_order_;
  std::unordered_map<uint64_t, uint64_t> failed_;  // key -> the frame it failed in
  uint64_t resident_bytes_ = 0;
  uint64_t resident_triangles_ = 0;

  // The shared buffers: positions and normals (ranges of vertices, parallel), indices (ranges of uint32), and the structure pages.
  Shared positions_;
  Shared normals_;
  Shared indices_;
  car_rt_rules::RangeAllocator vertex_ranges_;
  car_rt_rules::RangeAllocator index_ranges_;
  std::vector<Page> pages_;

  // The descriptor set of the car RT (bindings 0-3) and what each slot owns.
  plume::RenderDescriptorSetBuilder set_builder_;
  uint32_t tlas_index_ = 0;
  uint32_t table_index_ = 0;
  uint32_t indices_index_ = 0;
  uint32_t normals_index_ = 0;
  std::array<SlotResources, kFramesInFlight> slots_;

  // The check.
  plume::RenderDescriptorSetBuilder check_set_builder_;
  uint32_t check_view_index_ = 0;
  uint32_t check_stats_index_ = 0;
  std::unique_ptr<plume::RenderPipelineLayout> check_layout_;
  std::unique_ptr<plume::RenderShader> check_shader_;
  std::unique_ptr<plume::RenderPipeline> check_pipeline_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  std::array<std::unique_ptr<plume::RenderPipeline>, 4> view_pipelines_;
  std::array<bool, 4> view_pipeline_failed_{};
  HostImage view_image_;  // R8G8B8A8_UNORM, a compute pass writes it: the AO's linear depth image's size, or 1x1 with no view
  std::unique_ptr<plume::RenderBuffer> check_counters_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  bool readback_busy_ = false;
  uint64_t next_readback_ = 0;
  bool check_unavailable_logged_ = false;

  // What the log has said, and the totals.
  uint64_t logged_resident_ = ~uint64_t(0);
  bool first_build_logged_ = false;
  struct Stats {
    uint64_t frames_with_tlas = 0;
    uint64_t instances = 0;            // summed over those frames
    uint64_t meshes_built = 0;
    uint64_t meshes_evicted = 0;
    uint64_t read_failed = 0;
    uint64_t no_mesh_key = 0;
    uint64_t skinned_left_out = 0;
    uint64_t no_memory = 0;
    uint64_t missing_mesh = 0;         // draws left out of a top level: their mesh waits
    uint64_t bad_matrix = 0;
    uint64_t frames_no_car = 0;
    uint64_t checks = 0;
    uint64_t check_no_depth = 0;
    uint64_t triggers = 0;
  } stats_counts_;
};

}  // namespace mcla::gpu
