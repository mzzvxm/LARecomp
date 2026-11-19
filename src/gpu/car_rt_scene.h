#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car (and the cars beside it) as ray tracing acceleration structures
// ===========================================================================
// For ray traced car effects (first: the car's reflections of itself and of the cars driving alongside, car_rt_refl). One bottom-level structure per
// mesh of the player's car and of the other cars the selection keeps (car_rt_instances.h), built ONCE from guest memory when the mesh is first seen
// (car_frame.h MeshReader) and kept while its key lives; every instance of a model names the same structure. Every frame one top-level structure
// with one instance per draw of the cars, placed by the draw's matrix. The
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
//   - OTHER CARS (car_rt_instances.h, perf/RT-CARS-DESIGN.md): the cars driving alongside (opponents, traffic, parked cars) are in the same top level.
//     CarFrame recognises their draws (the same recipe of the vertex shader's microcode as the player's, told apart by their stencil IDs) and keeps
//     the nearest few within a radius of the player's vehicle, by graphics class (Mid 2 cars within 10 m, High 4 within 14 m); OnOtherCarDraw reads
//     the triangles of a mesh the scene has not seen within a budget of its own, and the instance list adds a draw of such a car as an instance of
//     the SAME structure its model's other cars use (the key is the model's triangles in model space) with the mask CAR_RT_MASK_OTHERS: the
//     reflection's rays hit it, the primary ray that finds the player's car never does. The other cars' share of the memory stops short of the
//     cap by a reserve for the player's own car. mcla_gpu_car_rt_cars (-1 class, 0 none, N) and mcla_gpu_car_rt_cars_radius are the debug switches.
//     Each kept car's BOX in view space (its instances' transformed mesh boxes) is kept too: CarRtView::boxes, and in the table after the cars' rows,
//     for the ray traced occlusion's region (car_rt_ao.h), which traces round the player's car and each of them, not round one box that holds them all.
//   - CARS THE CAMERA DOES NOT SEE (car_rt_memory_rules.h): the cars above come from the frame's DRAWS, and the game culls a car that is
//     out of the picture (the one beside the player, level with him) so it has none. CarFrame reads the game's own lists of vehicles each frame (the opponent
//     manager's racers, the ambient manager's live cars) and CarMemory keeps, for each car that WAS drawn, its draws relative to its vehicle's matrix (a kit);
//     a car in the list that is not drawn this frame, within the radius, is placed again from its kit at its vehicle's matrix of this frame (the meshes stay
//     resident while it is placed: Build's find marks them seen) and goes to the instance list as another car, ranked by the distance to the player with
//     the drawn ones within the class's count. A car never drawn has no kit and gets nothing. mcla_gpu_car_rt_cars_memory (debug) is the A/B switch.
//   - CARS FROM THEIR OWN MODEL DATA (car_model_rt_rules.h, car_model_walk.h, perf/RT-MODEL-SURVEY.md; mcla_gpu_car_rt_cars_model): a car the camera does not draw this
//     frame is built from the car's own model data in guest memory, not from draws it once made. CarFrame's walker walks every racer near the player in the first
//     vehicle draw's call of the frame (the guest is held back): per part the geometries of the lod the reflection wants, their vertex and index buffers, and the
//     part matrices of the frame. The scene (OnWalkedCars, inside that call) reads the triangles of the cars the main view does not show into CPU vectors like a
//     draw's (one mesh per geometry, keyed by the buffers' addresses, so cars of one model share them), within the class's budget for a new car (triangles and
//     KiB read, the same numbers as another car's draws) and its own share of the memory; at the trigger the meshes are built into bottom level structures within
//     a budget of walked triangles a frame, and the walked cars the frame does NOT draw (the draw path keeps those) and the kits do not place (mode) are placed
//     by the walk's part matrices through the SAME world-to-view conversion as a draw's instance (PackInstanceTransform), ranked with the other cars by the
//     distance to the player within the class's count, with the car's paint colour in its record. The walker checks its placements against the draws of the cars the
//     camera does draw and says the error in metres (the "PLACEMENT PROOF" line). A car that has not been placed for a while gives its meshes back.
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
// THE CHECK (mcla_gpu_car_rt_check, debug; recorded by OnAfterOcclusion): a compute pass after the occlusion's passes traces one primary ray from the view-space origin
// through every vehicle pixel of the occlusion's linear depth image that the scene is meant to hold and compares the committed hit's distance
// with the pixel's (hlsl/car_rt_check_cs.hlsl); it counts coverage and error and logs them every 120 frames ("[car rt] check ..."); mcla_gpu_car_rt_view 1/2/3
// shows the error / the normals / another car's paint row over the scene. This is what proves the camera space, the matrices and the meshes on the first run.
// THE PAINT PROBE (RT-MODEL-CARS-4): the same pass's thread (0, 0) copies the first four cars' records (the two rows each, CarRow and CarPaintRow) out of the table with the shader's own
// loads, and the host compares them, bit for bit, with the rows it wrote that frame (car_rt_instances_rules.h CarRecordLog): the "[car rt] paint probe of frame N" line says IDENTICAL
// for each car or DIFFERENT. That is the proof that the paint colour the reflection pass uses is the one the host sent (car_rt_scene_rules.h DecodeProbe).
// The pixels the scene is meant to hold are the player's car's and those of the other cars the frame's selection kept (the class's radius and count,
// the cars 0 switch); the driver is left out of the scene (its pixels stay in the player's count: the depth image has no mask for it). A vehicle
// pixel of any other car is counted apart as "not in the scene by design" (car_rt_scene_rules.h THE SCOPE: placed in the kept cars' boxes).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
#include "car_model_rt_rules.h"
#include "car_rt_instances.h"
#include "car_rt_memory_rules.h"
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
//                                       w = the draw's place in car_frame::Frame::draws (an instance of another car: the row of its car's
//                                       record, which follows the instances' rows in this table: xyz the car's body centre in view space,
//                                       w its radius, as float bits); after the cars' records come two rows for the box of each other car
//                                       that has an instance (CarRtView::box_row): its minimum and its maximum in view space, as float bits
//   2  ByteAddressBuffer                every mesh's triangle-list indices (uint32, relative to the mesh's first vertex)
//   3  ByteAddressBuffer                every mesh's vertex normals (float3, 12 bytes a vertex, model space, unit length)
struct CarRtView {
  bool valid = false;                         // this frame's top level was built and recorded on the list before the caller's pass
  plume::RenderDescriptorSet* set = nullptr;  // bound by the caller at its own set index
  uint32_t instance_count = 0;
  // The OTHER cars' boxes in the frame's view space (the union of each kept car's instances' transformed mesh boxes), one an other car with any
  // instance in this top level, nearest first. `boxes` points at the scene's own list (valid until the next OnResolve or the present). The same
  // boxes are in binding 1's table for a shader: two rows a box (minimum, maximum as float bits) from row `box_row`. Where the player's own box
  // is (CarFrame's world box through the camera) is up to the caller: this is what the scene knows of the cars beside it.
  const car_rt_instances::ViewBox* boxes = nullptr;
  uint32_t box_count = 0;
  uint32_t box_row = 0;
  // The other cars' records in the same table (two rows a car, CarRow then CarPaintRow, from row `car_row`) and what the host wrote in each (the check's probe compares them).
  uint32_t car_row = 0;
  uint32_t car_count = 0;
  std::vector<car_rt_instances::CarRecordLog> records;
  // The player's own car's box in the same space, from its listed instances' transformed mesh boxes (valid = false: none had a box). Used by the check.
  car_rt_instances::ViewBox player_box;
  // The WORLD's up (+y) in the frame's view space, a unit vector: the sky's direction for a ray that wants the sky (the chrome's sky probe in
  // car_rt_refl_cs.hlsl; the camera pitches, so the view's own y is not it). (0, 1, 0) where the frame's camera gave none.
  float up[3] = {0.0f, 1.0f, 0.0f};
  // The opponents' reflection pass (car_rt_opp_refl.h): the instances of other cars' draws of the paint's shader in this top level (car_rt_scene_rules.h kMaskOtherPaint), and the row of
  // the player's car's record in binding 1's table (0: none; the record is added when there are such instances).
  uint32_t other_paint_instances = 0;
  uint32_t player_row = 0;
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
  // A resolve, after ResolveCache::Execute and BEFORE the ambient occlusion's passes (they trace rays in the top level): at the frame's trigger
  // it uploads and builds what is new and builds the top level. `list` is the frame's open list outside any render pass, `slot` its frame
  // slot. True when anything was recorded (the caller's list state is gone). The check pass is not recorded here: it reads the occlusion's
  // linear depth, which the occlusion's passes make after this call (OnAfterOcclusion).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot);
  // The same resolve again, after the ambient occlusion's passes: with the check on (mcla_gpu_car_rt_check) and this frame's top level built,
  // records the check pass over the occlusion's linear depth image (`ao`). True when anything was recorded.
  bool OnAfterOcclusion(const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot, const AmbientOcclusion* ao);
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
  // The same for a draw of another car the selection kept, within the budgets of the other cars (car_rt_instances_rules.h Limits, OthersMayAdd).
  void OnOtherCarDraw(const car_frame::OtherDraw& draw, const car_frame::MeshReader& reader) override;
  // The cars of the model walk (car_model_walk.h): reads the triangles of the cars it is to stage, inside the first vehicle draw's call of the frame.
  void OnWalkedCars(const std::vector<car_model_walk::WalkedCar>& cars, const car_model_walk::MeshSource& source) override;

 private:
  // A mesh read from guest memory, waiting for the trigger.
  struct Staged {
    uint32_t attempts = 0;  // frames it found no room
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    uint32_t triangles = 0;
    float bounds_min[3] = {};  // the mesh's box in its model space (car_rt_instances_rules.h BoundsOf)
    float bounds_max[3] = {};
    bool has_bounds = false;
    bool walked = false;  // read from a car's model data (the walk), not from a draw: it has a build budget of its own
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
    float bounds_min[3] = {};  // the mesh's box in its model space, for the cars' boxes (car_rt_instances_rules.h GrowBox)
    float bounds_max[3] = {};
    bool has_bounds = false;
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

  // The check pass's push block (hlsl/car_rt_check_cs.hlsl CheckPush), 80 bytes.
  struct CheckPush {
    uint32_t slots[4];  // the linear depth image's slot, its width and height, the flags (bit 0 write the picture, bits 8-15 the view mode, bits 16-23 the cars' records the table holds)
    float lens[4];      // tan of the half field of view (x, y), the ray's start and end
    uint32_t scope[4];  // the first row of the other kept cars' boxes in the car RT table, how many, the flags (car_rt_scene_rules.h kScopeFlag*), the first row of the cars' records
    float box_min[4];   // the player's car's box in view space (xyz), grown by the host
    float box_max[4];
  };
  static_assert(sizeof(CheckPush) == 80);
  // What the check's scope held in the frame it was recorded for, for the log line (ProcessReadback).
  struct CheckScope {
    uint32_t kept_cars = 0;     // the other cars with a box in the top level (the frame's selection, those with a built mesh)
    bool player_box = false;    // the player's box was known (else only the other cars' boxes counted)
    uint32_t car_count = 0;     // the cars' records in the table that frame (the probe read the first car_rt_rules::kProbeCars)
    std::vector<car_rt_instances::CarRecordLog> records;  // what the host wrote for each (the probe's words are compared with it)
  };
  // The display pass's push block (hlsl/car_rt_view_ps.hlsl CarRtViewPush), 32 bytes.
  struct ViewPush {
    uint32_t slots[4];
    float sizes[4];
  };
  static_assert(sizeof(ViewPush) == 32);

  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  bool Wants() const { return requested_ || requested_prev_; }
  // Reads an unseen mesh from the draw's guest memory and stages it for the trigger (the tail OnPlayerDraw and OnOtherCarDraw share). The triangles
  // it staged: 0 when it left the mesh out.
  uint32_t ReadAndStage(uint64_t key, const car_frame::MeshReader& reader);
  // The rest of it for a mesh that is read already (a walked car's geometry): clean, box, stage. 0: left out.
  uint32_t FinishStaged(uint64_t key, Staged&& staged);
  // The bytes the staged meshes will take (positions, indices, structures), for the other cars' share of the memory.
  uint64_t StagedBytes() const;
  // The walked cars of the frame: the walker's Resolve, the choice of the cars to place, their instances (appended to the remembered cars' lists), the paint.
  void PlaceWalkedCars(const car_frame::Frame& frame, bool flip_z);
  // Gives a walked mesh back (resident or staged).
  void GiveBack(uint64_t key);

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
  // The cars-from-memory lines of the log: the first kit, a vehicle list that cannot be read, and a line every few hundred frames.
  void LogMemory(const car_frame::Frame& frame);
  void ReleaseAll(const char* why);
  void ReleaseSlot(SlotResources& slot);

  // --- the check ---
  bool MakeCheck(HostPasses& passes);
  void RecordCheck(plume::RenderCommandList* list, uint32_t slot, const ResolveCache::Summary& summary, const AmbientOcclusion* ao);
  bool RecordView(plume::RenderCommandList* list, const ResolveCache::Summary& summary, uint32_t image_slot, uint32_t image_width,
                  uint32_t image_height);
  plume::RenderPipeline* ViewPipeline(uint32_t samples);
  void ProcessReadback(uint64_t frame, uint64_t generation, uint32_t instances, const CheckScope& scope, uint32_t width, uint32_t height);

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
  bool check_due_ = false;  // this frame's top level was built and the check is on: OnAfterOcclusion records it
  CarRtView view_;
  int flip_state_ = 0;  // 1: the frame's camera has z backward (the game's own) and is flipped; -1: z forward already; 0 not decided yet
  uint32_t staged_triangles_ = 0;        // read this frame
  uint32_t staged_triangles_other_ = 0;  // ... of them for other cars (their own budget inside it)
  CarRtInstances instances_;  // the frame's instance list: the player's draws and the other cars' (car_rt_instances.h)
  // The cars the camera does not see (car_rt_memory_rules.h): the kits of the cars that were drawn, and this frame's placements.
  car_rt_memory::CarMemory memory_;
  car_rt_memory::CarMemory::Output remembered_;
  // The cars from model data: the lists the instance list takes beside the remembered cars' (remembered_ + the walked cars), what the frame's build plan is, the
  // cleanup set, the budget spent in the frame's walk, and what the log says.
  std::vector<car_rt_instances::OtherCar> extra_cars_;
  std::vector<car_rt_instances::OtherDraw> extra_draws_;
  std::vector<car_model_rt::CarInfo> paint_infos_;  // the walked cars of this frame (their paint): the cars the camera draws take it (CarRtInstances::Build, PaintForDrawn)
  std::vector<uint8_t> build_plan_;                // by staged_order_: built this frame (car_model_rt::PlanBuilds)
  car_model_rt::WalkedSet walked_set_;
  car_model_rt::StageBudget walked_spent_;
  std::unordered_set<uint64_t> walked_keys_;       // the meshes read from model data (resident or staged)
  struct WalkedStats {
    uint64_t frames = 0, frames_placed = 0;        // frames the walk produced / placed at least one car
    uint64_t cars_placed = 0, instances = 0, missing_parts = 0;
    uint32_t most_cars = 0;
    uint64_t meshes_read = 0, triangles_read = 0, kib_read = 0, read_failed = 0, held_budget = 0, held_memory = 0;
    uint64_t meshes_built = 0, triangles_built = 0;  // walked meshes that got a structure
    uint64_t given_back = 0;
    uint64_t painted = 0;                           // placed cars (and remembered ones) with a paint colour
    uint64_t painted_drawn = 0;                     // cars-frames: cars the camera draws that were given the paint of the walked car at their place (CarRtInstances::Build)
    uint64_t proof_pairs = 0;
    uint64_t held_pose = 0;                         // frames whose walked cars were held back: no hypothesis of the part matrices was confirmed by the draws
    uint64_t most_read_triangles = 0, most_read_kib = 0;  // the most read from guest memory in one frame (the class's budget, plus the first mesh of a frame)
    uint64_t most_built_triangles = 0;              // the most walked triangles built into structures in one frame
    double proof_worst = 0.0;
    double walk_us_sum = 0.0;
    uint32_t walk_frames = 0;
  } walked_stats_;
  bool walked_first_logged_ = false;
  uint64_t walked_pose_warn_frame_ = 0;
  std::unordered_set<uint32_t> walked_present_;   // the vehicles placed in the last frame (the log says when one comes and goes)
  uint64_t walked_log_frame_ = 0;
  uint64_t walked_build_triangles_frame_ = 0;       // built in the frame being recorded
  uint64_t memory_log_frame_ = 0;
  bool memory_kit_logged_ = false;
  bool memory_lists_warned_ = false;

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
