// MCLA GPU renderer -- the player's car as ray tracing acceleration structures. See car_rt_scene.h.

#include "car_rt_scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ao.h"
#include "descriptors.h"
#include "device.h"
#include "edram.h"
#include "host_shaders.h"
#include "menu_world.h"
#include "pass_timing.h"
#include "temporal_identities.h"
#include "upload.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_car_rt_check, false, "MCLA/GPU",
    "Debug: proves the player's car as ray tracing structures (car_rt_scene.h) on a graphics card that has them (Direct3D 12 with "
    "DirectX Raytracing 1.1, Vulkan with ray queries). After the ambient occlusion's passes one ray is traced from the camera through every vehicle pixel of its "
    "linear depth image and the hit's distance is compared with the pixel's; the log says every 120 frames, under [car rt], how many "
    "vehicle pixels the structures cover and how far the distances are apart. Only the pixels the scene is meant to hold are traced: "
    "the player's car and the other cars the frame kept (mcla_gpu_car_rt_cars); the pixels of other cars are counted apart as not in "
    "the scene by design (the driver is in the player's count and shows as a miss). It "
    "needs the ambient occlusion on, and it asks for the car's meshes to be built. Costs a ray a vehicle pixel; never saved. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_rt_view, 0, "MCLA/GPU",
    "Debug: shows the car ray tracing check's picture over the scene (only while mcla_gpu_car_rt_check is on): 1 the error of each "
    "vehicle pixel's hit (green below 2%, yellow at 2%, red from 10%, magenta: nothing hit, grey-blue: a car outside the scene by design), "
    "2 the hit's normal as colour (x right, y up, z forward, times 0.5 plus 0.5). 0: off. Live.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_rt_masks, 0, "MCLA/GPU",
    "Debug: how the car's parts are told apart in the top level: 0 the tyres by their shader and glass by being drawn after the world's "
    "first draw (only in a frame where some of the car's draws are and some are not: where all are, nothing is glass), 1 every part "
    "that is no tyre is a body. Live.")
    .debug_only()
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace rr = car_rt_rules;

// The check's scope reads as many of the other cars' boxes as the selection keeps.
static_assert(rr::kScopeMaxOtherBoxes >= car_rt_instances::kMaxCars, "the check's scope reads every kept car's box");

namespace {

constexpr const char* kPrefix = "[car rt]";

// D3D_SHADER_MODEL_6_5 as RenderDeviceCapabilities::shaderModel gives it.
constexpr uint32_t kShaderModel65 = 0x65;
// The frames without a request after which everything is given back.
constexpr uint32_t kIdleReleaseFrames = 600;
// Frames between two check readbacks.
constexpr uint64_t kReadbackEveryFrames = 120;
// Bytes of the structure pages (a car's structures are 5 to 8 MiB); a larger structure gets a page of its own.
constexpr uint64_t kPageBytes = 8ull << 20;
// The growth steps of the shared buffers (vertices, indices).
constexpr uint64_t kVertexStep = 16384;
constexpr uint64_t kIndexStep = 65536;
// What a mesh's structure is guessed to take before it is built, per triangle (for the cap's room only; the build's own size replaces it).
// CHECK: the first run's "[car rt] ... structures N MiB" line for the real number (about 40 to 60 bytes a triangle is expected).
constexpr uint64_t kStructureBytesPerTriangle = 96;
// Frames a staged mesh may find no room before it is given up.
constexpr uint32_t kMaxAttempts = 30;
// The check's counters.
constexpr uint64_t kCounterBytes = rr::kStatsWords * 4;
// The check's rays start here and stop here (view-space distance, the game's world units).
constexpr float kCheckTMin = 0.01f;
constexpr float kCheckTMax = 1000.0f;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

std::string EdgeText(double edge) {
  return edge > 1.0 ? std::string("over 25%") : std::format("under {:.2g}%", edge * 100.0);
}

double Mib(uint64_t bytes) {
  return double(bytes) / double(1ull << 20);
}

}  // namespace

// ---------------------------------------------------------------------------
// support
// ---------------------------------------------------------------------------

std::string CarRtUnsupportedReason(const plume::RenderDeviceCapabilities& caps, bool vulkan) {
  if (vulkan) {
    // Plume's rayQuery is VK_KHR_ray_query with VK_KHR_acceleration_structure, buffer device addresses and Vulkan 1.2 together (plume_vulkan.cpp),
    // the whole of what the structures' builds and the SPIR-V 1.5 shaders (hlsl/car_rt_*.hlsl) need. The ray tracing pipelines are not needed.
    if (!caps.rayQuery) {
      return "the Vulkan driver does not offer ray queries (VK_KHR_ray_query with VK_KHR_acceleration_structure, buffer device addresses and Vulkan 1.2)";
    }
    if (!caps.bufferDeviceAddress || !caps.descriptorIndexing) {
      return "the Vulkan driver lacks buffer device addresses or descriptor indexing, which the car's ray tracing structures need";
    }
    return std::string();
  }
  if (!caps.raytracing || !caps.raytracingStateUpdate) {
    return "the graphics card does not offer DirectX Raytracing tier 1.1 (inline ray queries)";
  }
  if (caps.shaderModel < kShaderModel65) {
    return std::format("the Direct3D 12 runtime offers shader model {}.{} and the ray queries need 6.5", caps.shaderModel >> 4,
                       caps.shaderModel & 0xF);
  }
  return std::string();
}

bool CarRtSupported(const plume::RenderDeviceCapabilities& caps, bool vulkan) {
  return CarRtUnsupportedReason(caps, vulkan).empty();
}

// A mesh with its structure being placed this frame (the bottom level's build waits for the arenas to be final).
struct CarRtScene::Placed {
  uint64_t key = 0;
  Mesh mesh;
  plume::RenderBottomLevelASBuildInfo info;
  Mesh* node = nullptr;  // in meshes_ once the structure exists
  bool ok = false;
};

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

CarRtScene::~CarRtScene() {
  Shutdown();
}

bool CarRtScene::Initialize(Device& device, FrameRing& frames, UploadRing& upload, BindlessHeap& heap, HostPasses& passes,
                            EdramTargets& edram, PassTiming& pass_timing, CarFrame& car_frame) {
  device_ = &device;
  frames_ = &frames;
  upload_ = &upload;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  car_frame_ = &car_frame;

  // The car RT set, as car_rt_scene.h says: t0 the top level, t1 the per-instance rows, t2 the indices, t3 the normals.
  set_builder_.begin();
  tlas_index_ = set_builder_.addAccelerationStructure(0);
  table_index_ = set_builder_.addStructuredBuffer(1);
  indices_index_ = set_builder_.addByteAddressBuffer(2);
  normals_index_ = set_builder_.addByteAddressBuffer(3);
  set_builder_.end();
  for (SlotResources& slot : slots_) {
    slot.set = device.CreateDescriptorSet(set_builder_, 4, 0, "car rt set");
    if (!slot.set) {
      REXLOG_ERROR("{} the descriptor set could not be made: no car ray tracing this session", kPrefix);
      Shutdown();
      return false;
    }
  }
  // The check pass is optional: without it the structures are still built.
  if (!MakeCheck(passes)) {
    check_unavailable_logged_ = true;
  }
  if (!listener_added_) {
    car_frame.AddListener(this);
    listener_added_ = true;
  }
  ready_ = true;
  return true;
}

bool CarRtScene::MakeCheck(HostPasses& passes) {
  const HostShaderBlob blob = CarRtCheckShader();
  if (!blob.data) {
    REXLOG_INFO("{} the check pass has no shader in this build: mcla_gpu_car_rt_check does nothing", kPrefix);
    return false;
  }
  check_set_builder_.begin();
  check_view_index_ = check_set_builder_.addReadWriteTexture(0);              // u0
  check_stats_index_ = check_set_builder_.addReadWriteByteAddressBuffer(1);  // u1
  check_set_builder_.end();
  for (SlotResources& slot : slots_) {
    slot.check_set = device_->CreateDescriptorSet(check_set_builder_, 2, 0, "car rt check set");
    if (!slot.check_set) {
      return false;
    }
  }
  // The bindless textures (set 0, space 0), the pass's own (set 1, space 1), the car RT set (set 2, space 2), the push range (b0, space 3):
  // the layout the ray traced reflection pass has too.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap_->texture_set_builder());
  builder.addDescriptorSet(check_set_builder_);
  builder.addDescriptorSet(set_builder_);
  builder.addPushConstant(0, 3, sizeof(CheckPush), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  check_layout_ = device_->CreatePipelineLayout(builder, "car rt check layout");
  if (!check_layout_) {
    return false;
  }
  check_shader_ = device_->CreateShader(blob.data, blob.size, "main", blob.name);
  if (!check_shader_) {
    return false;
  }
  check_pipeline_ = device_->CreateComputePipeline(plume::RenderComputePipelineDesc(check_layout_.get(), check_shader_.get(), 8, 8, 1),
                                                   blob.name);
  if (!check_pipeline_) {
    return false;
  }
  const HostShaderBlob view = CarRtViewPixelShader();
  view_shader_ = passes.CreateShader(view.data, view.size, view.name);  // its pipelines are made when a view is first asked for
  check_counters_ = device_->CreateBuffer(
      plume::RenderBufferDesc::DefaultBuffer(kCounterBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS),
      "car rt check counters");
  readback_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kCounterBytes), "car rt check readback");
  return check_counters_ && readback_;
}

void CarRtScene::Shutdown() {
  // The GPU is idle and the FrameRing drained (the deferred frees ran): everything goes at once.
  ++generation_;
  view_ = CarRtView{};
  meshes_.clear();
  staged_.clear();
  staged_order_.clear();
  failed_.clear();
  positions_ = Shared{};
  normals_ = Shared{};
  indices_ = Shared{};
  vertex_ranges_.Reset(0);
  index_ranges_.Reset(0);
  pages_.clear();
  for (SlotResources& slot : slots_) {
    if (slot.instances && slot.instances_cpu) {
      slot.instances->unmap(0, nullptr);
    }
    if (slot.table && slot.table_cpu) {
      slot.table->unmap(0, nullptr);
    }
    slot = SlotResources{};
  }
  for (auto& pipeline : view_pipelines_) {
    pipeline.reset();
  }
  view_shader_.reset();
  check_pipeline_.reset();
  check_shader_.reset();
  check_layout_.reset();
  if (passes_ && view_image_.texture) {
    passes_->ReleaseImage(view_image_);
  }
  view_image_ = HostImage{};
  check_counters_.reset();
  readback_.reset();
  resident_bytes_ = 0;
  resident_triangles_ = 0;
  ready_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  upload_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  car_frame_ = nullptr;
}

void CarRtScene::Request() {
  requested_ = true;
  if (car_frame_) {
    // The draws of the player's car and, within the class's limits, of the cars beside it (car_rt_instances_rules.h); the debug cvars are read here.
    car_frame_->RequestOtherCars(instances_.CurrentLimits());
  }
}

uint64_t CarRtScene::HeldBytes() const {
  uint64_t bytes = positions_.bytes + normals_.bytes + indices_.bytes;
  for (const Page& page : pages_) {
    bytes += page.bytes;
  }
  for (const SlotResources& slot : slots_) {
    bytes += slot.tlas_bytes + slot.scratch_bytes + slot.instances_bytes + slot.table_bytes;
  }
  if (view_image_.texture) {
    bytes += uint64_t(view_image_.width) * view_image_.height * 4;
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the meshes: read in the draw's call
// ---------------------------------------------------------------------------

void CarRtScene::OnPlayerDraw(const car_frame::Draw& draw, const car_frame::MeshReader& reader) {
  if (!ready_ || !Wants()) {
    return;  // another effect asked CarFrame for the draws: nothing to read for this one
  }
  if (draw.part != car_frame::Part::kBody) {
    ++stats_counts_.skinned_left_out;  // the driver: not in the first version
    return;
  }
  if (draw.mesh_key == 0) {
    ++stats_counts_.no_mesh_key;
    return;
  }
  const uint64_t key = draw.mesh_key;
  if (const auto it = meshes_.find(key); it != meshes_.end()) {
    it->second.last_seen = frame_;
    return;
  }
  if (staged_.contains(key)) {
    return;
  }
  if (const auto failed = failed_.find(key); failed != failed_.end()) {
    if (frame_ < failed->second + rr::kRetryFailedFrames) {
      return;
    }
    failed_.erase(failed);
  }
  if (staged_triangles_ >= rr::kStageTrianglesPerFrame) {
    return;  // the rest next frame: a draw's call is no place for a long read
  }
  ReadAndStage(key, reader);
}

// A draw of another car the selection kept (car_rt_instances_rules.h): the same read, within what the other cars may spend in a frame (the triangles
// read, the share of the video memory the player's own car does not need) and never at the player's expense: the frame's total budget counts them too.
void CarRtScene::OnOtherCarDraw(const car_frame::OtherDraw& draw, const car_frame::MeshReader& reader) {
  if (!ready_ || !Wants() || draw.mesh_key == 0) {
    return;
  }
  const uint64_t key = draw.mesh_key;
  if (const auto it = meshes_.find(key); it != meshes_.end()) {
    it->second.last_seen = frame_;
    return;
  }
  if (staged_.contains(key)) {
    return;
  }
  if (const auto failed = failed_.find(key); failed != failed_.end()) {
    if (frame_ < failed->second + rr::kRetryFailedFrames) {
      return;
    }
    failed_.erase(failed);
  }
  if (staged_triangles_ >= rr::kStageTrianglesPerFrame) {
    return;
  }
  const car_rt_instances::Limits& limits = car_frame_->other_limits();
  // Before reading: the draw's own count says about how many triangles the mesh has (a list's third of the indices).
  const uint64_t hint = draw.indices / 3;
  if (!car_rt_instances::MayStage(staged_triangles_other_, hint, limits.stage_triangles)) {
    ++instances_.work().over_stage_budget;  // a later frame: the draw's car appears in parts
    return;
  }
  uint64_t staged_bytes = 0;
  for (const auto& [staged_key, staged] : staged_) {
    staged_bytes += uint64_t(staged.positions.size()) / 3 * 24 + uint64_t(staged.indices.size()) * 4 + uint64_t(staged.triangles) * kStructureBytesPerTriangle;
  }
  if (!car_rt_instances::OthersMayAdd(resident_bytes_, staged_bytes, car_rt_instances::EstimateMeshBytes(draw.indices, kStructureBytesPerTriangle))) {
    ++instances_.work().over_memory;  // the other cars' share is used: the least recently seen meshes go first when the cap asks (Evictions)
    return;
  }
  const uint32_t triangles = ReadAndStage(key, reader);
  if (triangles != 0) {
    staged_triangles_other_ += triangles;
    ++instances_.work().meshes_read;
    instances_.work().triangles_read += triangles;
  }
}

// The tail of both listeners: the triangles of an unseen mesh into CPU vectors, cleaned, staged for the trigger.
uint32_t CarRtScene::ReadAndStage(uint64_t key, const car_frame::MeshReader& reader) {
  Staged staged;
  if (!reader.ReadTriangles(&staged.positions, &staged.indices, &staged.normals)) {
    failed_[key] = frame_;
    ++stats_counts_.read_failed;
    return 0;
  }
  const rr::MeshClean clean = rr::CleanMesh(&staged.positions, &staged.indices, &staged.normals);
  const uint64_t vertices = staged.positions.size() / 3;
  staged.triangles = uint32_t(staged.indices.size() / 3);
  if (staged.triangles == 0 || vertices == 0 || vertices > rr::kMaxMeshVertices || staged.triangles > rr::kMaxMeshTriangles) {
    failed_[key] = frame_;
    ++stats_counts_.read_failed;
    if (stats_counts_.read_failed <= 8) {
      REXLOG_WARN("{} a mesh was left out: {} vertices, {} triangles ({} of {} triangles dropped as unusable)", kPrefix, vertices,
                  staged.triangles, clean.triangles_dropped, clean.triangles_in);
    }
    return 0;
  }
  staged.has_bounds = car_rt_instances::BoundsOf(staged.positions, staged.bounds_min, staged.bounds_max);
  const uint32_t triangles = staged.triangles;
  staged_triangles_ += triangles;
  staged_order_.push_back(key);
  staged_.emplace(key, std::move(staged));
  return triangles;
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

bool CarRtScene::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const {
  // The ambient occlusion's trigger (ao.cpp Judge, car_selfrefl.cpp IsTrigger): the first copy of the main depth that is no end-of-scene
  // copy, into 078B0000, before the anchor. The play view's rectangle starts at the origin and stays inside the picture; the pause
  // view's three tiles come in an order nobody has looked at, so any rectangle inside the picture will do and the destination must be
  // the whole picture.
  if (!summary.depth_copy || !summary.source || trigger_seen_) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  const bool pause = samples == temporal::kPauseSamples;
  if (!temporal::IsPlaySamples(samples) && !pause) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth || summary.dest_address != kTriggerDestination || anchor_seen_ ||
      composite_seen_) {
    return false;
  }
  const uint32_t width = std::min(summary.dest_width, summary.source->width);
  const uint32_t height = std::min(summary.dest_height, summary.source->height);
  if (pause) {
    return menu_world::PauseFirstPointGeometry(summary.x0, summary.y0, summary.x1, summary.y1, width, height, summary.source->width,
                                               summary.source->height, EdramTargets::kHeightAlignment);
  }
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 && uint32_t(summary.x1) <= width &&
         uint32_t(summary.y1) <= height;
}

bool CarRtScene::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                           uint32_t slot) {
  if (!ready_ || !list || slot >= kFramesInFlight || !IsTrigger(record, summary)) {
    return false;
  }
  trigger_seen_ = true;
  ++stats_counts_.triggers;
  view_ = CarRtView{};
  check_due_ = false;
  if (!Wants()) {
    return false;
  }
  const car_frame::Frame& frame = car_frame_->Current();
  if (!frame.valid || frame.draws.empty()) {
    ++stats_counts_.frames_no_car;
    return false;
  }

  // The camera's z. The frame says its camera is the AO's view space (z forward); the game's own view matrix looks down -z. The
  // player's car is in front of the camera, so which side of the camera's plane it is on says which one this is.
  // CHECK: Frame::view's x (right) and y (up) are taken as the AO's; the first check run says (a mirrored x or a wrong y leaves the
  // coverage near 0 and the view 1 all magenta), and the log line below says which z it found.
  {
    const double position[3] = {frame.player_world.m[3][0], frame.player_world.m[3][1], frame.player_world.m[3][2]};
    const int sign = rr::ViewZSign(frame.view, position);
    if (sign != 0) {
      const int state = sign < 0 ? 1 : -1;
      if (state != flip_state_) {
        flip_state_ = state;
        REXLOG_INFO("{} the frame's camera matrix has z {}: {}", kPrefix, state == 1 ? "backward, as the game's own view matrix has it" : "forward, as the occlusion's view space has it",
                    state == 1 ? "the instances' z is flipped into the occlusion's view space" : "used as it is");
      }
    }
  }
  const bool flip_z = flip_state_ == 1;

  // The meshes this frame's draws name are seen now (they are not given back for the cap's sake).
  for (const car_frame::Draw& draw : frame.draws) {
    if (draw.part == car_frame::Part::kBody && draw.mesh_key != 0) {
      if (const auto it = meshes_.find(draw.mesh_key); it != meshes_.end()) {
        it->second.last_seen = frame_;
      }
    }
  }

  // What this frame adds, and what the cap leaves room for.
  std::vector<uint32_t> staged_triangles;
  for (const uint64_t key : staged_order_) {
    staged_triangles.push_back(staged_.at(key).triangles);
  }
  const size_t take = rr::TakeWithinBudget(staged_triangles, rr::kBuildTrianglesPerFrame);
  uint64_t need_bytes = 0;
  for (size_t i = 0; i < take; ++i) {
    const Staged& s = staged_.at(staged_order_[i]);
    need_bytes += uint64_t(s.positions.size()) / 3 * 24 + uint64_t(s.indices.size()) * 4 + uint64_t(s.triangles) * kStructureBytesPerTriangle;
  }
  Evictions(need_bytes);

  pass_timing_->Mark(list, "car rt build");
  std::vector<Placed> placed;
  uint64_t scratch_needed = 0;
  PlaceStaged(list, slot, &placed);

  // Their bottom-level structures: the shared buffers are final now, so the build's inputs can name them.
  for (Placed& p : placed) {
    Mesh& m = p.mesh;
    const plume::RenderBottomLevelASMesh geometry(
        plume::RenderBufferReference(indices_.buffer.get(), uint64_t(m.first_index) * 4),
        plume::RenderBufferReference(positions_.buffer.get(), uint64_t(m.first_vertex) * 12), plume::RenderFormat::R32_UINT,
        plume::RenderFormat::R32G32B32_FLOAT, m.index_count, m.vertex_count, 12, /*isOpaque=*/true);
    device_->device()->setBottomLevelASBuildInfo(p.info, &geometry, 1, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
    const uint64_t size = AlignUp(p.info.accelerationStructureSize, rr::kStructureAlignment);
    uint32_t page = 0;
    uint64_t offset = 0;
    std::unique_ptr<plume::RenderAccelerationStructure> structure;
    if (size != 0 && AllocStructure(size, &page, &offset)) {
      structure = device_->device()->createAccelerationStructure(plume::RenderAccelerationStructureDesc(
          plume::RenderAccelerationStructureType::BOTTOM_LEVEL, plume::RenderBufferReference(pages_[page].buffer.get(), offset), size));
      if (!structure) {
        pages_[page].ranges.Free(offset, size);
      }
    }
    if (!structure) {
      // No room (or no object): the ranges go back once the copies recorded into them are done, the mesh is tried again later.
      FreeRanges(m.first_vertex, m.vertex_count, m.first_index, m.index_count, -1, 0, 0);
      failed_[p.key] = frame_;
      ++stats_counts_.no_memory;
      continue;
    }
    m.page = page;
    m.structure_offset = offset;
    m.structure_size = size;
    m.bytes = uint64_t(m.vertex_count) * 24 + uint64_t(m.index_count) * 4 + size;
    m.structure = std::move(structure);
    scratch_needed = std::max(scratch_needed, p.info.scratchSize);
    resident_bytes_ += m.bytes;
    resident_triangles_ += m.index_count / 3;
    p.node = &meshes_.emplace(p.key, std::move(m)).first->second;
    p.ok = true;
    ++stats_counts_.meshes_built;
  }
  if (!placed.empty() && !first_build_logged_) {
    first_build_logged_ = true;
    REXLOG_INFO("{} the first meshes of the player's car are built into ray tracing structures", kPrefix);
  }

  // The frame's instances: the draws whose mesh has a structure.
  size_t body_draws = 0, after_world = 0;
  for (const car_frame::Draw& draw : frame.draws) {
    if (draw.part == car_frame::Part::kBody && draw.mesh_key != 0) {
      ++body_draws;
      after_world += draw.after_world ? 1 : 0;
    }
  }
  // CHECK: "glass = drawn after the frame's first world draw" against the census (the spike's section 2 says 18 translucent draws came
  // after the world in one view and the whole car did in another): the check's log counts the hits by class, mcla_gpu_car_rt_masks 1
  // turns the rule off.
  const bool glass_rule = REXCVAR_GET(mcla_gpu_car_rt_masks) == 0 && rr::GlassRuleInformative(after_world, body_draws);
  // The cars the camera does not see (car_rt_memory_rules.h): the kits of the cars drawn this frame are made or refreshed, and each kit whose car is in the
  // game's list, near the player, and not drawn is placed at its vehicle's matrix. Only meshes with a structure count (the scene gave back the others).
  remembered_.Clear();
  if (instances_.RememberedOn()) {
    car_rt_memory::CarMemory::Input memory_in;
    memory_in.guest_frame = frame.guest_frame;
    memory_in.limits = &car_frame_->other_limits();
    memory_in.player_known = frame.player_vehicle_known;
    for (int c = 0; c < 3; ++c) {
      memory_in.player[c] = frame.player_vehicle[c];
    }
    memory_in.vehicles_valid = frame.vehicles_valid;
    memory_in.vehicles = &frame.vehicles;
    memory_in.seen = &frame.seen_cars;
    memory_in.cars = &frame.other_cars;
    memory_in.draws = &frame.other_draws;
    memory_.Update(
        memory_in,
        [&](uint64_t key) {
          const auto it = meshes_.find(key);
          return it != meshes_.end() && it->second.structure != nullptr;
        },
        &remembered_);
  } else {
    memory_.Clear();
  }
  // === RT-CARS BLOCK (reconcile with feat/rt-vulkan): the instance list (car_rt_instances.h) -> the top level's instances. The player's draws and
  // the other cars' draws whose mesh has a structure come out of `instances_` as items (mesh key, transform in view space, mask) and as the table's
  // rows (one per item, then one per other car); this block only turns an item into a RenderTopLevelASInstance on the mesh's structure.
  const car_rt_instances::List& cars = instances_.Build(
      frame, car_frame_->other_limits(), flip_z, glass_rule, [&](uint64_t key, car_rt_instances::MeshRef* ref) {
        const auto it = meshes_.find(key);
        if (it == meshes_.end() || !it->second.structure) {
          return false;
        }
        it->second.last_seen = frame_;  // a mesh of a car in this frame's list is seen (the cap does not take it back)
        ref->first_index = it->second.first_index;
        ref->first_vertex = it->second.first_vertex;
        std::memcpy(ref->bounds_min, it->second.bounds_min, sizeof(ref->bounds_min));
        std::memcpy(ref->bounds_max, it->second.bounds_max, sizeof(ref->bounds_max));
        ref->has_bounds = it->second.has_bounds;
        return true;
      },
      &remembered_.cars, &remembered_.draws);
  stats_counts_.missing_mesh += cars.stats.missing_mesh;
  stats_counts_.bad_matrix += cars.stats.bad_matrix;
  std::vector<plume::RenderTopLevelASInstance> instances;
  instances.reserve(cars.items.size());
  for (uint32_t index = 0; index < cars.items.size(); ++index) {
    const car_rt_instances::Item& item = cars.items[index];
    const Mesh& m = meshes_.at(item.mesh_key);
    plume::RenderTopLevelASInstance instance(plume::RenderBufferReference(pages_[m.page].buffer.get(), m.structure_offset), index, item.mask, 0,
                                             /*cullDisable=*/true, plume::RenderAffineTransform());
    std::memcpy(instance.transform.m, item.transform, sizeof(item.transform));
    instances.push_back(instance);
  }
  const std::vector<rr::InstanceRow>& rows = cars.rows;  // the instances' rows, then the other cars' records: one table
  // === END OF THE RT-CARS BLOCK ===

  // The top level's own description and the slot's buffers for it (the slot's GPU work is done: the FrameRing waited for it).
  SlotResources& res = slots_[slot];
  const uint32_t count = uint32_t(instances.size());
  plume::RenderTopLevelASBuildInfo tlas_info;
  if (count != 0) {
    device_->device()->setTopLevelASBuildInfo(tlas_info, instances.data(), count, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
  }
  const bool slot_ok = count != 0 ? EnsureSlot(res, slot, tlas_info.accelerationStructureSize, std::max(scratch_needed, tlas_info.scratchSize),
                                                tlas_info.instancesBufferData.size(), uint64_t(rows.size()) * sizeof(rr::InstanceRow))
                                  : (scratch_needed == 0 || EnsureSlot(res, slot, 0, scratch_needed, 0, 0));
  bool recorded = !placed.empty();
  if (!slot_ok) {
    // The builds cannot be recorded (no scratch): a mesh whose structure was never built must not reach a top level, so the meshes
    // placed in this frame go back (their copies are recorded already; the ranges are reused after the GPU is done with them).
    REXLOG_ERROR("{} the frame slot's buffers could not be made: no top level this frame, {} new meshes given up", kPrefix, placed.size());
    for (Placed& p : placed) {
      if (p.ok) {
        Evict(p.key);
        failed_[p.key] = frame_;
      }
    }
    LogTotals("buffers failed");
    return recorded;
  }

  // The buffers the builds read: copied into above, read by the builds and by the shaders from here.
  if (positions_.buffer) {
    SetAccess(list, positions_, Access::kRead);
    SetAccess(list, normals_, Access::kRead);
    SetAccess(list, indices_, Access::kRead);
  }
  // The bottom levels, one after the other over the same scratch (a barrier between).
  for (Placed& p : placed) {
    if (!p.ok) {
      continue;
    }
    list->barriers(plume::RenderBarrierStage::COMPUTE,
                   plume::RenderBufferBarrier(res.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
    list->buildBottomLevelAS(p.node->structure.get(), plume::RenderBufferReference(res.scratch.get(), 0), p.info);
    recorded = true;
  }

  if (count != 0) {
    // CHECK: the BLAS and TLAS builds on the D3D12 debug layer: the inputs are moved from COPY_DEST to ALL_SHADER_RESOURCE by SetAccess
    // (a buffer in COMMON would rely on implicit promotion), the scratch is in UNORDERED_ACCESS by its barrier, the structures stay in
    // RAYTRACING_ACCELERATION_STRUCTURE, whose barrier is a UAV barrier.
    std::memcpy(res.instances_cpu, tlas_info.instancesBufferData.data(), tlas_info.instancesBufferData.size());
    std::memcpy(res.table_cpu, rows.data(), rows.size() * sizeof(rr::InstanceRow));
    std::vector<plume::RenderBufferBarrier> before_top;
    before_top.reserve(pages_.size() + 1);
    for (const Page& page : pages_) {
      before_top.emplace_back(page.buffer.get(), plume::RenderBufferAccess::READ);
    }
    before_top.emplace_back(res.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE);
    list->barriers(plume::RenderBarrierStage::COMPUTE, before_top.data(), uint32_t(before_top.size()), nullptr, 0);
    list->buildTopLevelAS(res.tlas.get(), plume::RenderBufferReference(res.scratch.get(), 0), plume::RenderBufferReference(res.instances.get(), 0),
                          tlas_info);
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE),
                   plume::RenderBufferBarrier(res.tlas_buffer.get(), plume::RenderBufferAccess::READ));
    // The set: written for this slot now (its last use is finished), so a consumer binds it as it is.
    const plume::RenderBufferStructuredView rows_view(sizeof(rr::InstanceRow), 0);
    res.set->setAccelerationStructure(tlas_index_, res.tlas.get());
    res.set->setBuffer(table_index_, res.table.get(), uint64_t(rows.size()) * sizeof(rr::InstanceRow), &rows_view);
    res.set->setBuffer(indices_index_, indices_.buffer.get(), 0);
    res.set->setBuffer(normals_index_, normals_.buffer.get(), 0);
    view_.valid = true;
    view_.set = res.set.get();
    view_.instance_count = count;
    view_.boxes = cars.boxes.empty() ? nullptr : cars.boxes.data();
    view_.box_count = uint32_t(cars.boxes.size());
    view_.box_row = cars.box_row;
    view_.player_box = cars.player_box;
    {
      const double world_up[3] = {0.0, 1.0, 0.0};
      car_rt_instances::ViewVector(frame.view, flip_z, world_up, view_.up);  // false: the default (0, 1, 0) stays
    }
    stats_counts_.frames_with_tlas += 1;
    stats_counts_.instances += count;
    recorded = true;
  }
  instances_.NoteFrame(frame_, stats_counts_.meshes_built, car_frame_->other_limits());
  LogMemory(frame);
  LogTotals("");
  // The check reads the occlusion's linear depth, which its passes make after this call: it is recorded by OnAfterOcclusion.
  check_due_ = REXCVAR_GET(mcla_gpu_car_rt_check) && view_.valid;
  return recorded;
}

// The cars-from-memory lines. First: the first kit made (a car that was drawn near the player and is matched to the game's list). A warning, once: the lists
// cannot be read while the other cars are on. Then a line every kMemoryLogFrames triggers while anything is going on: the last frame's numbers and the totals.
void CarRtScene::LogMemory(const car_frame::Frame& frame) {
  if (!car_frame_->other_limits().On() || !instances_.RememberedOn()) {
    return;
  }
  constexpr uint64_t kMemoryLogFrames = 300;
  const car_rt_memory::Stats& s = remembered_.stats;
  const car_rt_memory::Totals& t = memory_.totals();
  if (frame.player_vehicle_known && !frame.vehicles_valid && !memory_lists_warned_) {
    memory_lists_warned_ = true;
    REXLOG_WARN("{} frame {}: the game's lists of vehicles (the opponent manager's racers, the ambient manager's live cars) cannot be read: cars the camera does not see "
                "are not placed (CHECK the guest addresses in car_mesh_read.h; the car frame log's 'the game's vehicles' text says which list)",
                kPrefix, frame_);
  }
  if (t.recorded != 0 && !memory_kit_logged_) {
    memory_kit_logged_ = true;
    REXLOG_INFO("{} frame {}: the first car is remembered (matched to the game's own list of vehicles, its draws kept relative to its matrix): {}", kPrefix, frame_,
                car_rt_memory::Describe(s));
  }
  if (memory_log_frame_ == 0) {
    memory_log_frame_ = frame_;
  }
  if (frame_ < memory_log_frame_ + kMemoryLogFrames) {
    return;
  }
  if (t.recorded != 0 || s.vehicles != 0 || t.no_player != 0) {
    REXLOG_INFO("{} memory, frames {}..{}: {} triggers; cars placed from memory in {} of them ({} in all, {} draws, {} left out for want of a structure), most at once {}; {} kits made, {} "
                "refused, {} dropped, most held {}; most vehicles near the player {}, worst match offset {:.2f} m, {} triggers without a readable list; last frame: {}",
                kPrefix, memory_log_frame_, frame_, t.frames, t.frames_placed, t.cars_placed, t.draws_placed, t.not_resident, t.most_placed, t.recorded, t.record_refused,
                t.dropped, t.most_kits, t.vehicles_max, t.worst_offset, t.no_player, car_rt_memory::Describe(s));
  }
  memory_.ResetTotals();
  memory_log_frame_ = frame_;
}

bool CarRtScene::OnAfterOcclusion(const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot, const AmbientOcclusion* ao) {
  if (!check_due_ || !ready_ || !list || slot >= kFramesInFlight || !view_.valid) {
    return false;
  }
  check_due_ = false;
  RecordCheck(list, slot, summary, ao);
  return true;
}

// ---------------------------------------------------------------------------
// eviction
// ---------------------------------------------------------------------------

bool CarRtScene::Evictions(uint64_t need_bytes) {
  std::vector<rr::MeshUse> uses;
  uses.reserve(meshes_.size());
  for (const auto& [key, mesh] : meshes_) {
    uses.push_back(rr::MeshUse{key, mesh.bytes, mesh.last_seen});
  }
  rr::EvictionRequest request;
  request.frame = frame_;
  request.need_bytes = need_bytes;
  const rr::EvictionPlan plan = rr::PlanEviction(uses, request);
  for (const uint64_t key : plan.keys) {
    Evict(key);
  }
  if (!plan.keys.empty()) {
    stats_counts_.meshes_evicted += plan.keys.size();
    LogTotals("meshes given back");
  }
  return plan.fits;
}

void CarRtScene::Evict(uint64_t key) {
  const auto it = meshes_.find(key);
  if (it == meshes_.end()) {
    return;
  }
  Mesh& m = it->second;
  FreeRanges(m.first_vertex, m.vertex_count, m.first_index, m.index_count, m.structure ? int64_t(m.page) : -1, m.structure_offset,
             m.structure_size);
  if (m.structure) {
    frames_->Retire(std::move(m.structure));
  }
  resident_bytes_ -= std::min(resident_bytes_, m.bytes);
  resident_triangles_ -= std::min<uint64_t>(resident_triangles_, m.index_count / 3);
  meshes_.erase(it);
}

void CarRtScene::FreeRanges(uint32_t first_vertex, uint32_t vertex_count, uint32_t first_index, uint32_t index_count, int64_t page,
                            uint64_t structure_offset, uint64_t structure_size) {
  // Reused only after the GPU is done with the frame being recorded: its lists name these ranges (the copies into them, the builds, the
  // shaders' reads).
  frames_->RetireAction([this, generation = generation_, first_vertex, vertex_count, first_index, index_count, page, structure_offset,
                         structure_size] {
    if (generation != generation_) {
      return;  // everything was given back since
    }
    vertex_ranges_.Free(first_vertex, vertex_count);
    index_ranges_.Free(first_index, index_count);
    if (page >= 0 && size_t(page) < pages_.size()) {
      pages_[size_t(page)].ranges.Free(structure_offset, structure_size);
    }
  });
}

void CarRtScene::ReleaseAll(const char* why) {
  view_ = CarRtView{};
  memory_.Clear();
  remembered_.Clear();
  const bool held = !meshes_.empty() || positions_.buffer || !pages_.empty() || slots_[0].tlas_buffer || slots_[1].tlas_buffer;
  if (!held) {
    return;
  }
  ++generation_;
  for (auto& [key, mesh] : meshes_) {
    if (mesh.structure) {
      frames_->Retire(std::move(mesh.structure));
    }
  }
  meshes_.clear();
  staged_.clear();
  staged_order_.clear();
  failed_.clear();
  resident_bytes_ = 0;
  resident_triangles_ = 0;
  for (Shared* shared : {&positions_, &normals_, &indices_}) {
    if (shared->buffer) {
      frames_->Retire(std::move(shared->buffer));
    }
    *shared = Shared{};
  }
  vertex_ranges_.Reset(0);
  index_ranges_.Reset(0);
  for (Page& page : pages_) {
    frames_->Retire(std::move(page.buffer));
  }
  pages_.clear();
  for (SlotResources& slot : slots_) {
    ReleaseSlot(slot);
  }
  logged_resident_ = ~uint64_t(0);
  REXLOG_INFO("{} the meshes and structures are given back ({})", kPrefix, why);
}

void CarRtScene::ReleaseSlot(SlotResources& slot) {
  if (slot.instances && slot.instances_cpu) {
    slot.instances->unmap(0, nullptr);
  }
  if (slot.table && slot.table_cpu) {
    slot.table->unmap(0, nullptr);
  }
  frames_->Retire(std::move(slot.tlas));
  frames_->Retire(std::move(slot.tlas_buffer));
  frames_->Retire(std::move(slot.scratch));
  frames_->Retire(std::move(slot.instances));
  frames_->Retire(std::move(slot.table));
  slot.tlas_bytes = slot.scratch_bytes = slot.instances_bytes = slot.table_bytes = 0;
  slot.instances_cpu = nullptr;
  slot.table_cpu = nullptr;
}

// ---------------------------------------------------------------------------
// the shared buffers
// ---------------------------------------------------------------------------

bool CarRtScene::MakeShared(Shared& shared, uint64_t bytes, plume::RenderBufferFlags flags, const char* name) {
  shared.buffer = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(bytes, flags), name);
  shared.bytes = shared.buffer ? bytes : 0;
  shared.access = Access::kNone;
  return shared.buffer != nullptr;
}

void CarRtScene::SetAccess(plume::RenderCommandList* list, Shared& shared, Access access) {
  if (!shared.buffer || shared.access == access) {
    return;
  }
  switch (access) {
    case Access::kCopyWrite:
      list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(shared.buffer.get(), plume::RenderBufferAccess::WRITE));
      break;
    case Access::kCopyRead:
      list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(shared.buffer.get(), plume::RenderBufferAccess::READ));
      break;
    case Access::kRead:
      list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE),
                     plume::RenderBufferBarrier(shared.buffer.get(), plume::RenderBufferAccess::READ));
      break;
    case Access::kNone:
      break;
  }
  shared.access = access;
}

bool CarRtScene::GrowVertices(plume::RenderCommandList* list, uint64_t needed_vertices) {
  const uint64_t limit = rr::kMemoryCapBytes / 24;
  if (needed_vertices > limit) {
    return false;
  }
  const uint64_t capacity = vertex_ranges_.capacity();
  if (needed_vertices <= capacity && positions_.buffer) {
    return true;
  }
  const uint64_t grown = rr::GrownCapacity(capacity, needed_vertices, kVertexStep, limit);
  Shared positions, normals;
  if (!MakeShared(positions, grown * 12, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT, "car rt positions") ||
      !MakeShared(normals, grown * 12, plume::RenderBufferFlag::STORAGE, "car rt normals")) {
    return false;
  }
  // What the ranges hold moves with the buffers (the structures need none of it again, the shaders read the normals).
  const uint64_t used_bytes = vertex_ranges_.high_water() * 12;
  if (positions_.buffer && used_bytes != 0) {
    SetAccess(list, positions_, Access::kCopyRead);
    SetAccess(list, positions, Access::kCopyWrite);
    list->copyBufferRegion(plume::RenderBufferReference(positions.buffer.get(), 0), plume::RenderBufferReference(positions_.buffer.get(), 0),
                           used_bytes);
    SetAccess(list, normals_, Access::kCopyRead);
    SetAccess(list, normals, Access::kCopyWrite);
    list->copyBufferRegion(plume::RenderBufferReference(normals.buffer.get(), 0), plume::RenderBufferReference(normals_.buffer.get(), 0),
                           used_bytes);
  }
  if (positions_.buffer) {
    frames_->Retire(std::move(positions_.buffer));
  }
  if (normals_.buffer) {
    frames_->Retire(std::move(normals_.buffer));
  }
  positions_ = std::move(positions);
  normals_ = std::move(normals);
  vertex_ranges_.Grow(grown);
  return true;
}

bool CarRtScene::GrowIndices(plume::RenderCommandList* list, uint64_t needed_indices) {
  const uint64_t limit = rr::kMemoryCapBytes / 4;
  if (needed_indices > limit) {
    return false;
  }
  const uint64_t capacity = index_ranges_.capacity();
  if (needed_indices <= capacity && indices_.buffer) {
    return true;
  }
  const uint64_t grown = rr::GrownCapacity(capacity, needed_indices, kIndexStep, limit);
  Shared indices;
  if (!MakeShared(indices, grown * 4, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT | plume::RenderBufferFlag::STORAGE,
                  "car rt indices")) {
    return false;
  }
  const uint64_t used_bytes = index_ranges_.high_water() * 4;
  if (indices_.buffer && used_bytes != 0) {
    SetAccess(list, indices_, Access::kCopyRead);
    SetAccess(list, indices, Access::kCopyWrite);
    list->copyBufferRegion(plume::RenderBufferReference(indices.buffer.get(), 0), plume::RenderBufferReference(indices_.buffer.get(), 0),
                           used_bytes);
  }
  if (indices_.buffer) {
    frames_->Retire(std::move(indices_.buffer));
  }
  indices_ = std::move(indices);
  index_ranges_.Grow(grown);
  return true;
}

bool CarRtScene::AllocVertices(plume::RenderCommandList* list, uint32_t count, uint32_t* first) {
  uint64_t offset = 0;
  if (positions_.buffer && vertex_ranges_.Alloc(count, 1, &offset)) {
    *first = uint32_t(offset);
    return true;
  }
  // No range that long: the tail grows by at least the request.
  if (!GrowVertices(list, vertex_ranges_.capacity() + count) || !vertex_ranges_.Alloc(count, 1, &offset)) {
    return false;
  }
  *first = uint32_t(offset);
  return true;
}

bool CarRtScene::AllocIndices(plume::RenderCommandList* list, uint32_t count, uint32_t* first) {
  uint64_t offset = 0;
  if (indices_.buffer && index_ranges_.Alloc(count, 1, &offset)) {
    *first = uint32_t(offset);
    return true;
  }
  if (!GrowIndices(list, index_ranges_.capacity() + count) || !index_ranges_.Alloc(count, 1, &offset)) {
    return false;
  }
  *first = uint32_t(offset);
  return true;
}

bool CarRtScene::AllocStructure(uint64_t size, uint32_t* page, uint64_t* offset) {
  for (uint32_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i].ranges.Alloc(size, rr::kStructureAlignment, offset)) {
      *page = i;
      return true;
    }
  }
  uint64_t held = 0;
  for (const Page& p : pages_) {
    held += p.bytes;
  }
  const uint64_t bytes = std::max<uint64_t>(kPageBytes, AlignUp(size, 1ull << 20));
  if (held + bytes > rr::kMemoryCapBytes) {
    return false;
  }
  Page fresh;
  fresh.buffer = device_->CreateBuffer(plume::RenderBufferDesc::AccelerationStructureBuffer(bytes), "car rt structures");
  if (!fresh.buffer) {
    return false;
  }
  fresh.bytes = bytes;
  fresh.ranges.Reset(bytes);
  pages_.push_back(std::move(fresh));
  if (!pages_.back().ranges.Alloc(size, rr::kStructureAlignment, offset)) {
    return false;
  }
  *page = uint32_t(pages_.size() - 1);
  return true;
}

void CarRtScene::PlaceStaged(plume::RenderCommandList* list, uint32_t slot, std::vector<Placed>* placed) {
  if (staged_order_.empty()) {
    return;
  }
  std::vector<uint32_t> triangles;
  triangles.reserve(staged_order_.size());
  for (const uint64_t key : staged_order_) {
    triangles.push_back(staged_.at(key).triangles);
  }
  const size_t take = rr::TakeWithinBudget(triangles, rr::kBuildTrianglesPerFrame);
  // One growth for the whole batch, not one for each mesh (the first frame holds the car's 195 draws).
  uint64_t total_vertices = 0, total_indices = 0;
  for (size_t i = 0; i < take; ++i) {
    const Staged& s = staged_.at(staged_order_[i]);
    total_vertices += s.positions.size() / 3;
    total_indices += s.indices.size();
  }
  if (vertex_ranges_.used() + total_vertices > vertex_ranges_.capacity() || !positions_.buffer) {
    GrowVertices(list, vertex_ranges_.used() + total_vertices);  // a refusal shows at the allocations below
  }
  if (index_ranges_.used() + total_indices > index_ranges_.capacity() || !indices_.buffer) {
    GrowIndices(list, index_ranges_.used() + total_indices);
  }

  std::vector<uint64_t> done;
  bool upload_failed = false;
  for (size_t i = 0; i < take && !upload_failed; ++i) {
    const uint64_t key = staged_order_[i];
    Staged& staged = staged_.at(key);
    const uint32_t vertex_count = uint32_t(staged.positions.size() / 3);
    const uint32_t index_count = uint32_t(staged.indices.size());
    uint32_t first_vertex = 0, first_index = 0;
    if (!AllocVertices(list, vertex_count, &first_vertex)) {
      ++stats_counts_.no_memory;
      if (++staged.attempts > kMaxAttempts) {
        failed_[key] = frame_;
        done.push_back(key);
        REXLOG_WARN("{} a mesh of {} triangles found no room in {} frames: left out", kPrefix, staged.triangles, kMaxAttempts);
      }
      continue;
    }
    if (!AllocIndices(list, index_count, &first_index)) {
      vertex_ranges_.Free(first_vertex, vertex_count);  // nothing was recorded into it
      ++stats_counts_.no_memory;
      if (++staged.attempts > kMaxAttempts) {
        failed_[key] = frame_;
        done.push_back(key);
      }
      continue;
    }
    const uint64_t position_bytes = uint64_t(vertex_count) * 12;
    const uint64_t index_bytes = uint64_t(index_count) * 4;
    const UploadSpan span = upload_->Allocate(slot, position_bytes * 2 + index_bytes, 16);
    if (!span) {
      vertex_ranges_.Free(first_vertex, vertex_count);
      index_ranges_.Free(first_index, index_count);
      upload_failed = true;  // the rest next frame
      continue;
    }
    std::memcpy(span.cpu, staged.positions.data(), position_bytes);
    std::memcpy(span.cpu + position_bytes, staged.normals.data(), position_bytes);
    std::memcpy(span.cpu + position_bytes * 2, staged.indices.data(), index_bytes);
    SetAccess(list, positions_, Access::kCopyWrite);
    SetAccess(list, normals_, Access::kCopyWrite);
    SetAccess(list, indices_, Access::kCopyWrite);
    list->copyBufferRegion(plume::RenderBufferReference(positions_.buffer.get(), uint64_t(first_vertex) * 12),
                           plume::RenderBufferReference(span.buffer, span.offset), position_bytes);
    list->copyBufferRegion(plume::RenderBufferReference(normals_.buffer.get(), uint64_t(first_vertex) * 12),
                           plume::RenderBufferReference(span.buffer, span.offset + position_bytes), position_bytes);
    list->copyBufferRegion(plume::RenderBufferReference(indices_.buffer.get(), uint64_t(first_index) * 4),
                           plume::RenderBufferReference(span.buffer, span.offset + position_bytes * 2), index_bytes);
    Placed p;
    p.key = key;
    p.mesh.first_vertex = first_vertex;
    p.mesh.vertex_count = vertex_count;
    p.mesh.first_index = first_index;
    p.mesh.index_count = index_count;
    p.mesh.last_seen = frame_;
    std::memcpy(p.mesh.bounds_min, staged.bounds_min, sizeof(p.mesh.bounds_min));
    std::memcpy(p.mesh.bounds_max, staged.bounds_max, sizeof(p.mesh.bounds_max));
    p.mesh.has_bounds = staged.has_bounds;
    placed->push_back(std::move(p));
    done.push_back(key);
  }
  for (const uint64_t key : done) {
    staged_.erase(key);
  }
  std::erase_if(staged_order_, [&](uint64_t key) { return !staged_.contains(key); });
}

bool CarRtScene::EnsureSlot(SlotResources& s, uint32_t index, uint64_t tlas_bytes, uint64_t scratch_bytes, uint64_t instance_bytes,
                            uint64_t table_bytes) {
  const auto room = [](uint64_t bytes) { return AlignUp(bytes + bytes / 4, 4096); };
  if (tlas_bytes != 0 && s.tlas_bytes < tlas_bytes) {
    // The top level sits at offset 0 of a buffer of its own: Plume's D3D12 descriptor for an acceleration structure used the buffer's base
    // address and ignored the structure's offset until its feat/ray-query 3c81454 (pinned with the Vulkan work). Kept at offset 0: on Vulkan
    // the descriptor names the structure itself, so the offset never mattered there.

    const uint64_t bytes = room(tlas_bytes);
    const std::string name = std::format("car rt top level {}", index);
    std::unique_ptr<plume::RenderBuffer> buffer =
        device_->CreateBuffer(plume::RenderBufferDesc::AccelerationStructureBuffer(bytes), name.c_str());
    if (!buffer) {
      return false;
    }
    std::unique_ptr<plume::RenderAccelerationStructure> structure = device_->device()->createAccelerationStructure(
        plume::RenderAccelerationStructureDesc(plume::RenderAccelerationStructureType::TOP_LEVEL, plume::RenderBufferReference(buffer.get(), 0),
                                               bytes));
    if (!structure) {
      return false;
    }
    frames_->Retire(std::move(s.tlas));
    frames_->Retire(std::move(s.tlas_buffer));
    s.tlas_buffer = std::move(buffer);
    s.tlas = std::move(structure);
    s.tlas_bytes = bytes;
  }
  if (scratch_bytes != 0 && s.scratch_bytes < scratch_bytes) {
    const uint64_t bytes = room(scratch_bytes);
    const std::string name = std::format("car rt scratch {}", index);
    std::unique_ptr<plume::RenderBuffer> buffer = device_->CreateBuffer(
        plume::RenderBufferDesc::DefaultBuffer(bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH |
                                                          plume::RenderBufferFlag::UNORDERED_ACCESS),
        name.c_str());
    if (!buffer) {
      return false;
    }
    frames_->Retire(std::move(s.scratch));
    s.scratch = std::move(buffer);
    s.scratch_bytes = bytes;
  }
  const auto upload_buffer = [&](std::unique_ptr<plume::RenderBuffer>& target, uint8_t*& cpu, uint64_t& have, uint64_t want,
                                 plume::RenderBufferFlags flags, const char* what) {
    if (want == 0 || have >= want) {
      return true;
    }
    const uint64_t bytes = room(want);
    const std::string name = std::format("car rt {} {}", what, index);
    std::unique_ptr<plume::RenderBuffer> buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(bytes, flags), name.c_str());
    if (!buffer) {
      return false;
    }
    const plume::RenderRange no_read(0, 0);
    uint8_t* mapped = static_cast<uint8_t*>(buffer->map(0, &no_read));
    if (!mapped) {
      return false;
    }
    if (target && cpu) {
      target->unmap(0, nullptr);
    }
    frames_->Retire(std::move(target));
    target = std::move(buffer);
    cpu = mapped;
    have = bytes;
    return true;
  };
  return upload_buffer(s.instances, s.instances_cpu, s.instances_bytes, instance_bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT,
                       "instances") &&
         upload_buffer(s.table, s.table_cpu, s.table_bytes, table_bytes, plume::RenderBufferFlag::STORAGE, "table");
}

void CarRtScene::LogTotals(const char* why) {
  const uint64_t total = resident_bytes_;
  if (!rr::TotalsChangedMuch(logged_resident_, total)) {
    return;
  }
  logged_resident_ = total;
  uint64_t held = 0;
  for (const SlotResources& slot : slots_) {
    held += slot.tlas_bytes + slot.scratch_bytes + slot.instances_bytes + slot.table_bytes;
  }
  uint64_t structures = 0;
  for (const auto& [key, mesh] : meshes_) {
    structures += mesh.structure_size;
  }
  uint64_t vertices = 0;
  for (const auto& [key, mesh] : meshes_) {
    vertices += mesh.vertex_count;
  }
  REXLOG_INFO("{} {} meshes resident{}{}: {} triangles, {} vertices, {:.1f} MiB (structures {:.1f}, positions and normals {:.1f}, indices {:.1f}); the "
              "buffers hold {:.1f} MiB of the cap's {:.0f}, the frame slots {:.1f} MiB; built {}, given back {}, left out {} (no room {})",
              kPrefix, meshes_.size(), why[0] ? " (" : "", why[0] ? std::string(why) + ")" : std::string(), resident_triangles_, vertices,
              Mib(total), Mib(structures), Mib(vertices * 24), Mib(resident_triangles_ * 12), Mib(HeldBytes() - held),
              Mib(rr::kMemoryCapBytes), Mib(held), stats_counts_.meshes_built, stats_counts_.meshes_evicted,
              stats_counts_.read_failed + stats_counts_.no_memory, stats_counts_.no_memory);
}

// ---------------------------------------------------------------------------
// the frame boundary
// ---------------------------------------------------------------------------

void CarRtScene::OnPresent() {
  if (ready_) {
    if (REXCVAR_GET(mcla_gpu_car_rt_check)) {
      Request();  // the check asks for the structures, so for the car's draws too
    }
    if (requested_ || requested_prev_) {
      idle_frames_ = 0;
    } else if (idle_frames_ < kIdleReleaseFrames && ++idle_frames_ == kIdleReleaseFrames) {
      ReleaseAll("no effect has asked for them for a while");
    }
  }
  requested_prev_ = requested_;
  requested_ = false;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
  check_due_ = false;
  view_ = CarRtView{};
  staged_triangles_ = 0;
  staged_triangles_other_ = 0;
  ++frame_;
}

// ---------------------------------------------------------------------------
// the check
// ---------------------------------------------------------------------------

void CarRtScene::RecordCheck(plume::RenderCommandList* list, uint32_t slot, const ResolveCache::Summary& summary,
                             const AmbientOcclusion* ao) {
  if (!check_pipeline_ || !check_counters_ || !readback_) {
    if (!check_unavailable_logged_) {
      check_unavailable_logged_ = true;
      REXLOG_WARN("{} mcla_gpu_car_rt_check is on but the check pass could not be made", kPrefix);
    }
    return;
  }
  AmbientOcclusion::LinearDepthView depth;
  if (!ao || !ao->FrameLinearDepth(&depth) || !depth.image || !depth.image->texture) {
    ++stats_counts_.check_no_depth;
    if (stats_counts_.check_no_depth == 1) {
      REXLOG_WARN("{} the check needs the ambient occlusion's linear depth image and this frame has none (the occlusion and the contact "
                  "shadows are off, or their passes did not run): this frame has no check, and a frame that has the image is checked again; the "
                  "frames without one so far are counted at the end of every check line (this warning is said once)", kPrefix);
    }
    return;
  }
  const int view_mode = REXCVAR_GET(mcla_gpu_car_rt_view);
  const bool due = frame_ >= next_readback_ && !readback_busy_;
  if (!due && view_mode == 0) {
    return;
  }
  const uint32_t width = depth.image->width;
  const uint32_t height = depth.image->height;
  const bool want_view = view_mode != 0;
  if (!passes_->EnsureImage(view_image_, want_view ? width : 1, want_view ? height : 1, plume::RenderFormat::R8G8B8A8_UNORM, "car rt check view",
                            plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS)) {
    return;
  }
  const UploadSpan zero = upload_->Allocate(slot, kCounterBytes, 16);
  if (!zero) {
    return;
  }
  // CHECK: the barrier on the occlusion's linear depth image below (the AO left it for pixel shaders; a compute read needs the
  // non-pixel bit too) and the typed UAV store into an R8G8B8A8_UNORM image, on the D3D12 debug layer.

  std::memset(zero.cpu, 0, size_t(kCounterBytes));

  pass_timing_->Mark(list, "car rt check");
  SlotResources& res = slots_[slot];
  // The counters start at zero.
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(check_counters_.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(check_counters_.get(), 0), zero.reference(), kCounterBytes);
  // The depth image the occlusion's last pass left for pixel shaders is read by a compute shader now (both stages), the picture is written.
  list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE),
                 plume::RenderTextureBarrier(depth.image->texture.get(), plume::RenderTextureLayout::SHADER_READ));
  if (view_image_.layout != plume::RenderTextureLayout::GENERAL) {
    list->barriers(plume::RenderBarrierStage::COMPUTE,
                   plume::RenderTextureBarrier(view_image_.texture.get(), plume::RenderTextureLayout::GENERAL));
    view_image_.layout = plume::RenderTextureLayout::GENERAL;
  }
  list->barriers(plume::RenderBarrierStage::COMPUTE,
                 plume::RenderBufferBarrier(check_counters_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  res.check_set->setTexture(check_view_index_, view_image_.texture.get(), plume::RenderTextureLayout::GENERAL, view_image_.view.get());
  res.check_set->setBuffer(check_stats_index_, check_counters_.get(), kCounterBytes);

  CheckPush push{};
  push.slots[0] = depth.image->slot;
  push.slots[1] = width;
  push.slots[2] = height;
  push.slots[3] = (want_view ? 1u : 0u) | (uint32_t(view_mode) << 8);
  push.lens[0] = depth.tan_half_fov[0];
  push.lens[1] = depth.tan_half_fov[1];
  push.lens[2] = kCheckTMin;
  push.lens[3] = kCheckTMax;
  // The scope: the pixels the scene is meant to hold are those in the player's car's box (its listed instances' transformed mesh boxes, grown by the
  // margin) or in the box of one of the other cars the top level holds (the table's rows from box_row, two a box, grown by the margin in the shader).
  CheckScope scope;
  scope.player_box = view_.player_box.valid;
  if (scope.player_box) {
    for (int c = 0; c < 3; ++c) {
      push.box_min[c] = view_.player_box.min[c] - rr::kScopeBoxMargin;
      push.box_max[c] = view_.player_box.max[c] + rr::kScopeBoxMargin;
    }
  }
  scope.kept_cars = view_.boxes ? std::min(view_.box_count, rr::kScopeMaxOtherBoxes) : 0u;
  push.scope[0] = view_.box_row;
  push.scope[1] = scope.kept_cars;
  push.scope[2] = scope.player_box ? rr::kScopeFlagPlayerBox : 0u;
  list->setComputePipelineLayout(check_layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), 0);
  list->setComputeDescriptorSet(res.check_set.get(), 1);
  list->setComputeDescriptorSet(res.set.get(), 2);
  list->setPipeline(check_pipeline_.get());
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  ++stats_counts_.checks;

  if (due) {
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(check_counters_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(check_counters_.get(), 0),
                           kCounterBytes);
    readback_busy_ = true;
    next_readback_ = frame_ + kReadbackEveryFrames;
    frames_->RetireAction([this, frame = frame_, generation = generation_, instances = view_.instance_count, scope, width, height] {
      ProcessReadback(frame, generation, instances, scope, width, height);
    });
  }
  if (want_view) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(view_image_.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    view_image_.layout = plume::RenderTextureLayout::SHADER_READ;
    RecordView(list, summary, view_image_.slot, width, height);
  }
}

void CarRtScene::ProcessReadback(uint64_t frame, uint64_t generation, uint32_t instances, const CheckScope& scope, uint32_t width, uint32_t height) {
  readback_busy_ = false;
  if (generation != generation_ || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} check of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  uint32_t words[rr::kStatsWords];
  std::memcpy(words, mapped, sizeof(words));
  readback_->unmap();
  const rr::CheckSummary s = rr::DecodeCheckStats(words);
  // The line the chain reads (perf/scripts/chain-rtint.ps1): "check of frame N: P vehicle pixels ... X% hit" and "on the other cars' parts" as before, and
  // since the scope "O vehicle pixels not in the scene by design" and "K other kept cars". P counts only the pixels the scene is meant to hold (the player's
  // car and the other cars the frame kept); O is the vehicle pixels of the cars outside the selection, counted apart.
  const std::string no_player_box = scope.player_box ? std::string() : std::string("; the player's car's box was not known (none of its meshes had a box): "
                                                                                     "every pixel in no other car's box is counted");
  if (s.vehicle == 0) {
    if (s.outside == 0) {
      REXLOG_INFO("{} check of frame {}: no vehicle pixel in the {}x{} depth image (no car on the screen), the top level held {} instances; check frames "
                  "without a depth image so far: {}", kPrefix, frame, width, height, instances, stats_counts_.check_no_depth);
    } else {
      REXLOG_INFO("{} check of frame {}: no vehicle pixel in the {}x{} depth image that the scene is meant to hold ({} vehicle pixels not in the scene by design: "
                  "cars outside the selection; {} other kept cars), the top level held {} instances{}; check frames without a depth image so far: {}",
                  kPrefix, frame, width, height, s.outside, scope.kept_cars, instances, no_player_box, stats_counts_.check_no_depth);
    }
    return;
  }
  REXLOG_INFO("{} check of frame {}: {} vehicle pixels in the {}x{} depth image that the scene is meant to hold ({} of them in the box of {} other kept cars, "
              "the rest in the player's car's), {:.1f}% hit a part of a car in the scene ({} missed: the driver, parts not built yet); {} vehicle pixels "
              "not in the scene by design (cars outside the selection: beyond the class's radius or count, or the cars 0 switch); the hit's distance is "
              "off the image's by {:.2f}% on average, {} at the median and {} at the 95th percentile, {:.1f}% are within 1%; nearer than the image by over "
              "2%: {:.1f}%, farther: {:.1f}%; hits by class: {} body, {} glass, {} tyres, {} on the other cars' parts; the top level held {} instances{}; "
              "check frames without a depth image so far: {}",
              kPrefix, frame, s.vehicle, width, height, s.others, scope.kept_cars, s.coverage * 100.0, s.missed, s.outside, s.mean_error * 100.0,
              EdgeText(s.p50), EdgeText(s.p95), s.within_1pct * 100.0, s.nearer * 100.0, s.farther * 100.0, s.body, s.glass, s.tyres, s.other,
              instances, no_player_box, stats_counts_.check_no_depth);
}

plume::RenderPipeline* CarRtScene::ViewPipeline(uint32_t samples) {
  if (samples == 0 || (samples & (samples - 1)) != 0 || samples > 8 || !view_shader_) {
    return nullptr;
  }
  const uint32_t index = uint32_t(std::countr_zero(samples));
  if (!view_pipelines_[index] && !view_pipeline_failed_[index]) {
    // A debug view: made when first asked for, on the render thread (a hitch once).
    plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
    blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
    blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                  uint8_t(plume::RenderColorWriteEnable::BLUE);
    view_pipelines_[index] = passes_->CreatePipeline(passes_->layout(), *view_shader_, plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples,
                                                     "car rt view");
    view_pipeline_failed_[index] = !view_pipelines_[index];
  }
  return view_pipelines_[index].get();
}

bool CarRtScene::RecordView(plume::RenderCommandList* list, const ResolveCache::Summary& summary, uint32_t image_slot, uint32_t image_width,
                            uint32_t image_height) {
  if (!summary.source || image_slot == kInvalidBindlessSlot) {
    return false;
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != summary.source->host_samples || found->scale != summary.source->scale) {
    return false;
  }
  plume::RenderPipeline* pipeline = ViewPipeline(found->host_samples);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  const uint32_t picture_width = std::min(summary.dest_width, summary.source->width);
  const uint32_t picture_height = std::min(summary.dest_height, summary.source->height);
  const uint32_t render_width = summary.source->scale.Edge(picture_width);
  const uint32_t render_height = summary.source->scale.Edge(picture_height);
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(render_width), float(render_height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(render_width), int32_t(render_height));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  ViewPush push{};
  push.slots[0] = image_slot;
  push.sizes[0] = float(image_width);
  push.sizes[1] = float(image_height);
  push.sizes[2] = float(render_width);
  push.sizes[3] = float(render_height);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

}  // namespace mcla::gpu
