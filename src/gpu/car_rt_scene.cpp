// MCLA GPU renderer -- the player's car as ray tracing acceleration structures. See car_rt_scene.h.

#include "car_rt_scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/immortal.h"
#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "ao.h"
#include "car_mirror_rt_rules.h"
#include "car_rt_hit_rules.h"
#include "car_rt_refl_rules.h"
#include "city_rt_scene.h"
#include "rt_scene_service.h"
#include "descriptors.h"
#include "device.h"
#include "edram.h"
#include "host_shaders.h"
#include "menu_world.h"
#include "pass_timing.h"
#include "temporal_identities.h"
#include "textures.h"
#include "upload.h"

namespace mcla::gpu {

namespace rr = car_rt_rules;

// The check's scope reads as many of the other cars' boxes as the selection keeps.
static_assert(rr::kScopeMaxOtherBoxes >= car_rt_instances::kMaxCars, "the check's scope reads every kept car's box");

namespace {

constexpr const char* kPrefix = "[car rt]";

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

std::string CarRtUnsupportedReason(const plume::RenderDeviceCapabilities& caps, bool vulkan, const backend::WaveOps& waves) {
  // Plume's rayQuery is VK_KHR_ray_query with VK_KHR_acceleration_structure, buffer device addresses and Vulkan 1.2 together (plume_vulkan.cpp),
  // the whole of what the structures' builds and the SPIR-V 1.5 shaders (hlsl/car_rt_*.hlsl) need. The ray tracing pipelines are not needed.
  // The wave operations are not in Plume's capabilities: backend::QueryWaveOps asks the device (hlsl/car_rt_check_cs.hlsl and rt_bench_cs.hlsl use them).
  // The rules themselves (car_rt_rules::UnsupportedReason) need no device and have a test of their own.
  return rr::UnsupportedReason(caps, vulkan, waves);
}

bool CarRtSupported(const plume::RenderDeviceCapabilities& caps, bool vulkan, const backend::WaveOps& waves) {
  return CarRtUnsupportedReason(caps, vulkan, waves).empty();
}

// A mesh with its structure being placed this frame (the bottom level's build waits for the arenas to be final).
struct CarRtScene::Placed {
  uint64_t key = 0;
  Mesh mesh;
  plume::RenderBottomLevelASBuildInfo info;
  Mesh* node = nullptr;  // in meshes_ once the structure exists; valid until PlaceWalkedCars (it can give the mesh back): later, look the mesh up by `key`
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

  // The car RT set, as car_rt_scene.h says: t0 the top level, t1 the per-instance rows, t2 the indices, t3 the normals, t4 the texture coordinates, t5 the materials.
  set_builder_.begin();
  tlas_index_ = set_builder_.addAccelerationStructure(0);
  table_index_ = set_builder_.addStructuredBuffer(1);
  indices_index_ = set_builder_.addByteAddressBuffer(2);
  normals_index_ = set_builder_.addByteAddressBuffer(3);
  uvs_index_ = set_builder_.addByteAddressBuffer(4);              // RT-HIT-SHADING: the texture coordinates ...
  materials_index_ = set_builder_.addStructuredBuffer(5);         // ... and the frame's lighting and the instances' materials
  hit_bindings_ = CityHitScene::AddBindings(set_builder_);  // CITY-RT M2 H3: t6 the pool, t7 the sector table, t8 the materials, t9 the textures (after rt-hit-shading's t4 and t5)
  world_bindings_ = RtWorldFrame::AddBindings(set_builder_);  // CITY-RT FRAME: t10 the world's rows, t11 the lamps (rt_world_frame.h)
  set_builder_.end();
  for (SlotResources& slot : slots_) {
    // The views the ranges add up to: t0 the top level and t1..t5 (6), the hit data's four, the world frame's two. D3D12 sizes a set by this count, not by the ranges.
    slot.set = device.CreateDescriptorSet(set_builder_, 6 + CityHitScene::kBindingCount + RtWorldFrame::kBindingCount, 0, "car rt set");
    if (!slot.set) {
      REXLOG_ERROR("{} the descriptor set could not be made: no car ray tracing this session", kPrefix);
      Shutdown();
      return false;
    }
  }
  // A zeroed buffer for the bindings the car meshes fill, in a top level that has no car (OnResolveCityOnly): a set that names nothing is not valid on every backend.
  standin_ = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(256, plume::RenderBufferFlag::STORAGE), "car rt stand-in");
  if (standin_) {
    const plume::RenderRange no_read(0, 0);
    if (uint8_t* mapped = static_cast<uint8_t*>(standin_->map(0, &no_read))) {
      std::memset(mapped, 0, 256);
    } else {
      standin_.reset();
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
  lights_source_.SetEnabled(false);  // the hook notes no kind for this consumer any more (LU-8)
  lamp_set_.Clear();
  instances_.SetLamps(nullptr);
  view_ = CarRtView{};
  meshes_.clear();
  staged_.clear();
  staged_order_.clear();
  failed_.clear();
  walked_keys_.clear();
  walked_set_.Clear();
  walked_present_.clear();
  parked_seen_.clear();
  positions_ = Shared{};
  normals_ = Shared{};
  uvs_ = Shared{};
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
    if (slot.materials && slot.materials_cpu) {
      slot.materials->unmap(0, nullptr);
    }
    slot = SlotResources{};
  }
  diffuse_slots_.clear();
  textures_ = nullptr;
  for (auto& pipeline : view_pipelines_) {
    pipeline.reset();
  }
  view_shader_.reset();
  check_pipeline_.reset();
  check_shader_.reset();
  check_layout_.reset();
  standin_.reset();
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
  if (service_) {
    service_->NoteCarWant();  // the board knows the car effects too (rt_scene_service.h)
  }
  RequestCollect();
}

void CarRtScene::RequestCollect() {
  requested_ = true;
  if (car_frame_) {
    // The draws of the player's car and, within the class's limits, of the cars beside it (car_rt_instances_rules.h); the debug cvars are read here.
    car_frame_->RequestOtherCars(instances_.CurrentLimits(), instances_.ModelMode(), instances_.TrafficCars(), instances_.WheelsOn());
  }
}

uint64_t CarRtScene::HeldBytes() const {
  uint64_t bytes = positions_.bytes + normals_.bytes + uvs_.bytes + indices_.bytes;
  for (const Page& page : pages_) {
    bytes += page.bytes;
  }
  for (const SlotResources& slot : slots_) {
    bytes += slot.tlas_bytes + slot.scratch_bytes + slot.instances_bytes + slot.table_bytes + slot.materials_bytes;
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

// The cars of the model walk (car_model_walk.h), inside the first vehicle draw's call of the frame: the triangles of the geometries of the cars it is to stage (not
// shown by the main view, near the player, within the class's count and a spare), largest geometry first, read from the car's own buffers like a draw's mesh:
// within the class's budget for a car that is new (the triangles and the KiB of guest data a frame: car_model_rt::MayStage, the first read of a frame always taken),
// the other cars' share of the video memory and the frame's total. A mesh already built, staged or failed is skipped (one mesh per buffer address: the cars of one
// model share them).
void CarRtScene::OnWalkedCars(const std::vector<car_model_walk::WalkedCar>& cars, const car_model_walk::MeshSource& source) {
  walked_spent_ = car_model_rt::StageBudget{};
  if (!ready_ || !Wants() || !car_frame_->others_on()) {
    return;
  }
  const car_rt_instances::Limits& limits = car_frame_->other_limits();
  ++walked_stats_.frames;
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  for (const car_model_walk::WalkedCar& car : cars) {
    if (!car.staging || !car.walk) {
      continue;
    }
    for (const car_model_rt::Part& part : car.parts) {
      const uint64_t key = part.key;
      if (const auto it = meshes_.find(key); it != meshes_.end()) {
        it->second.last_seen = frame_;
        continue;
      }
      if (staged_.contains(key)) {
        continue;
      }
      if (const auto failed = failed_.find(key); failed != failed_.end()) {
        if (frame_ < failed->second + rr::kRetryFailedFrames) {
          continue;
        }
        failed_.erase(failed);
      }
      if (part.geometry >= car.walk->geometries.size()) {
        continue;
      }
      const car_model_walk::Geometry& geometry = car.walk->geometries[part.geometry];
      uint32_t triangles = 0;
      uint64_t bytes = 0;
      car_model_rt::CostOf(geometry, &triangles, &bytes);
      if (staged_triangles_ >= rr::kStageTrianglesPerFrame || !car_model_rt::MayStage(walked_spent_, triangles, bytes, limits)) {
        ++walked_stats_.held_budget;  // a later frame: the car appears in parts, the largest first
        ++instances_.work().over_stage_budget;
        continue;
      }
      if (!car_rt_instances::OthersMayAdd(resident_bytes_, StagedBytes(), car_rt_instances::EstimateMeshBytes(geometry.index_count, kStructureBytesPerTriangle))) {
        ++walked_stats_.held_memory;
        ++instances_.work().over_memory;
        continue;
      }
      car_model_rt::MeshRead info;
      const car_model_rt::MeshProblem problem = source.Read(*car.walk, part.geometry, &positions, &indices, &info);
      if (problem != car_model_rt::MeshProblem::kNone) {
        failed_[key] = frame_;
        ++walked_stats_.read_failed;
        if (walked_stats_.read_failed <= 8) {
          REXLOG_WARN("{} a walked car's geometry could not be read ({}): part {} of vehicle {:#010x}, {} vertices, {} indices, stride {}", kPrefix, car_model_rt::MeshProblemName(problem),
                      part.part, car.vehicle, geometry.vertex_count, geometry.index_count, geometry.vb_stride);
        }
        continue;
      }
      Staged staged;
      staged.positions = std::move(positions);
      staged.indices = std::move(indices);
      staged.walked = true;
      positions = std::vector<float>();
      indices = std::vector<uint32_t>();
      const uint32_t made = FinishStaged(key, std::move(staged));
      if (made != 0) {
        walked_keys_.insert(key);
        walked_stats_.most_read_triangles = std::max<uint64_t>(walked_stats_.most_read_triangles, walked_spent_.triangles + triangles);
        walked_stats_.most_read_kib = std::max<uint64_t>(walked_stats_.most_read_kib, (walked_spent_.bytes + info.bytes) / 1024u);
        walked_spent_.triangles += triangles;
        walked_spent_.bytes += info.bytes;
        staged_triangles_other_ += made;
        ++walked_stats_.meshes_read;
        walked_stats_.triangles_read += made;
        walked_stats_.kib_read += info.bytes / 1024u;
        ++instances_.work().meshes_read;
        instances_.work().triangles_read += made;
      }
    }
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
  // RT-HIT-SHADING: the draw's texture coordinates, once a mesh (a draw without them, or in a format not read, shades with its class colour).
  if (reader.ReadTexcoords(&staged.uv_floats)) {
    ++material_stats_.uv_meshes_read;
  } else {
    ++material_stats_.no_uv_meshes_read;
  }
  return FinishStaged(key, std::move(staged));
}

uint64_t CarRtScene::StagedBytes() const {
  uint64_t bytes = 0;
  for (const auto& [staged_key, staged] : staged_) {
    bytes += uint64_t(staged.positions.size()) / 3 * 24 + uint64_t(staged.indices.size()) * 4 + uint64_t(staged.triangles) * kStructureBytesPerTriangle;
  }
  return bytes;
}

uint32_t CarRtScene::FinishStaged(uint64_t key, Staged&& staged_in) {
  Staged staged = std::move(staged_in);
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
  staged.patch = car_mirror::MeasurePatch(staged.positions, staged.indices, staged.normals);  // a small flat mesh may be a mirror's glass (a big one is left at once)
  // The texture coordinates: one pair a vertex (the clean-up keeps every vertex), as half2 words for the shaders; the mesh's texel density for the mip level.
  if (staged.uv_floats.size() == vertices * 2) {
    staged.uvs.resize(size_t(vertices));
    for (size_t v = 0; v < size_t(vertices); ++v) {
      staged.uvs[v] = car_rt_material::PackUv(staged.uv_floats[v * 2], staged.uv_floats[v * 2 + 1]);
    }
    staged.uv_density = car_rt_material::MeshUvDensity(staged.positions, staged.uv_floats, staged.indices);
  }
  std::vector<float>().swap(staged.uv_floats);
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
  trigger_this_call_ = false;
  if (!ready_ || !list || slot >= kFramesInFlight || !IsTrigger(record, summary)) {
    return false;
  }
  trigger_seen_ = true;
  trigger_this_call_ = true;
  ++stats_counts_.triggers;
  view_ = CarRtView{};
  check_due_ = false;
  if (!Wants()) {
    return OnResolveCityOnly(list, slot);  // nobody asked for the cars: the city may still be wanted (rt_scene_service.h)
  }
  const car_frame::Frame& frame = car_frame_->Current();
  if (!frame.valid || frame.draws.empty()) {
    ++stats_counts_.frames_no_car;
    return OnResolveCityOnly(list, slot);  // the car is not in the frame: the city alone, in the predicted camera
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

  // What this frame adds, and what the cap leaves room for. The meshes read from a car's model data have a build budget of their own inside the frame's: a new car
  // of the walk never starves the player's car (car_model_rt::PlanBuilds).
  std::vector<car_model_rt::BuildItem> build_items;
  for (const uint64_t key : staged_order_) {
    const Staged& s = staged_.at(key);
    build_items.push_back(car_model_rt::BuildItem{s.triangles, s.walked});
  }
  build_plan_ = car_model_rt::PlanBuilds(build_items, rr::kBuildTrianglesPerFrame, car_frame_->other_limits().stage_triangles);
  uint64_t need_bytes = 0;
  for (size_t i = 0; i < build_plan_.size(); ++i) {
    if (!build_plan_[i]) {
      continue;
    }
    const Staged& s = staged_.at(staged_order_[i]);
    need_bytes += uint64_t(s.positions.size()) / 3 * 24 + uint64_t(s.indices.size()) * 4 + uint64_t(s.triangles) * kStructureBytesPerTriangle;
  }
  walked_build_triangles_frame_ = 0;
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
    m.bytes = uint64_t(m.vertex_count) * 28 + uint64_t(m.index_count) * 4 + size;  // positions, normals and texture coordinates, indices, structure
    m.structure = std::move(structure);
    scratch_needed = std::max(scratch_needed, p.info.scratchSize);
    resident_bytes_ += m.bytes;
    resident_triangles_ += m.index_count / 3;
    p.node = &meshes_.emplace(p.key, std::move(m)).first->second;
    p.ok = true;
    ++stats_counts_.meshes_built;
    if (walked_keys_.contains(p.key)) {
      ++walked_stats_.meshes_built;
      walked_stats_.triangles_built += p.node->index_count / 3;
      walked_build_triangles_frame_ += p.node->index_count / 3;
    }
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
  const bool glass_rule = larecomp::settings::mcla_gpu_car_rt_masks.Get() == 0 && rr::GlassRuleInformative(after_world, body_draws);
  // The cars the camera does not see (car_rt_memory_rules.h): the kits of the cars drawn this frame are made or refreshed, and each kit whose car is in the
  // game's list, near the player, and not drawn is placed at its vehicle's matrix. Only meshes with a structure count (the scene gave back the others).
  remembered_.Clear();
  if (instances_.RememberedOn()) {
    car_rt_memory::CarMemory::Input memory_in;
    memory_in.guest_frame = frame.guest_frame;
    memory_in.limits = &car_frame_->other_limits();
    memory_in.match_metres = instances_.MatchMetres();
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
  // The cars from the cars' own model data (car_model_rt_rules.h): the walker's Resolve (which of them the frame draws, the calibration and the proof of the
  // placements), the cars to place, their instances beside the remembered cars', the paint.
  extra_cars_.assign(remembered_.cars.begin(), remembered_.cars.end());
  extra_draws_.assign(remembered_.draws.begin(), remembered_.draws.end());
  PlaceWalkedCars(frame, flip_z);
  // === RT-CARS BLOCK (reconcile with feat/rt-vulkan): the instance list (car_rt_instances.h) -> the top level's instances. The player's draws and
  // the other cars' draws whose mesh has a structure come out of `instances_` as items (mesh key, transform in view space, mask) and as the table's
  // rows (one per item, then one per other car); this block only turns an item into a RenderTopLevelASInstance on the mesh's structure.
  // The traffic cars the walk placed have a count of their own (car_model_parts_rt_rules.h TrafficCarsFor): the list keeps the class's count of cars and these besides.
  car_rt_instances::Limits build_limits = car_frame_->other_limits();
  build_limits.cars = std::min<uint32_t>(build_limits.cars + walked_traffic_placed_, car_rt_instances::kMaxCars);
  UpdateLamps(frame, build_limits.On());  // LU-8: the game's lamps of this frame, for the other cars' lenses
  const car_rt_instances::List& cars = instances_.Build(
      frame, build_limits, flip_z, glass_rule, [&](uint64_t key, car_rt_instances::MeshRef* ref) {
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
        ref->has_uv = it->second.has_uv;
        ref->uv_density = it->second.uv_density;
        ref->patch = it->second.patch;
        return true;
      },
      &extra_cars_, &extra_draws_, instances_.ModelMode() != car_model_rt::Mode::kOff ? &paint_infos_ : nullptr);
  walked_stats_.painted_drawn += instances_.drawn_painted();
  stats_counts_.missing_mesh += cars.stats.missing_mesh;
  stats_counts_.bad_matrix += cars.stats.bad_matrix;
  std::vector<plume::RenderTopLevelASInstance> instances;
  instances.reserve(cars.items.size());
  rt_scene::Bounds scene_bounds;
  for (uint32_t index = 0; index < cars.items.size(); ++index) {
    const car_rt_instances::Item& item = cars.items[index];
    const Mesh& m = meshes_.at(item.mesh_key);
    plume::RenderTopLevelASInstance instance(plume::RenderBufferReference(pages_[m.page].buffer.get(), m.structure_offset), index, item.mask, 0,
                                             /*cullDisable=*/true, plume::RenderAffineTransform());
    std::memcpy(instance.transform.m, item.transform, sizeof(item.transform));
    instances.push_back(instance);
    scene_bounds.IncludeTransformed(m.bounds_min, m.bounds_max, item.transform, m.has_bounds);
  }
  // The instances' rows, then the other cars' records: one table. A top level that holds only the city (no car has a built mesh yet) still has a table of one row (the buffer is made
  // for the set; no instance names it).
  static const std::vector<rr::InstanceRow>& kOneRow = larecomp::Leak<const std::vector<rr::InstanceRow>>(1);   // never destroyed (EXIT-HARD)
  const std::vector<rr::InstanceRow>& rows = cars.rows.empty() ? kOneRow : cars.rows;
  // === END OF THE RT-CARS BLOCK ===

  // === CITY-RT BLOCK (city_rt_scene.h, CITY-RT M1): the city's structures join the top level. They are read, planned and built here (recorded on this list), and their instances are
  // described in the frame's view space with the cars' own transform and the mask kMaskCity, which none of the cars' passes traces and the city's shadow pass does. Nothing when the
  // city scene is off or has no pack.
  bool city_recorded = false;
  if (city_ != nullptr && city_->wanted()) {
    city_->Prepare(frame.view, flip_z, list, slot, &instances, &scene_bounds);
    city_recorded = city_->recorded();
  }
  // === END OF THE CITY-RT BLOCK ===

  // The top level's own description and the slot's buffers for it (the slot's GPU work is done: the FrameRing waited for it).
  SlotResources& res = slots_[slot];
  const uint32_t count = uint32_t(instances.size());
  plume::RenderTopLevelASBuildInfo tlas_info;
  std::vector<uint32_t> material_words;  // RT-HIT-SHADING: the frame's lighting rows and the instances' materials (car_rt_material_rules.h)
  if (count != 0) {
    device_->device()->setTopLevelASBuildInfo(tlas_info, instances.data(), count, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
    BuildMaterialRows(frame, flip_z, cars, &material_words);
  }
  const bool slot_ok = count != 0 ? EnsureSlot(res, slot, tlas_info.accelerationStructureSize, std::max(scratch_needed, tlas_info.scratchSize),
                                                tlas_info.instancesBufferData.size(), uint64_t(rows.size()) * sizeof(rr::InstanceRow),
                                                uint64_t(material_words.size()) * sizeof(uint32_t))
                                  : (scratch_needed == 0 || EnsureSlot(res, slot, 0, scratch_needed, 0, 0, 0));
  bool recorded = !placed.empty() || city_recorded;
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
    SetAccess(list, uvs_, Access::kRead);
    SetAccess(list, indices_, Access::kRead);
  }
  // The bottom levels, one after the other over the same scratch (a barrier between).
  for (Placed& p : placed) {
    if (!p.ok) {
      continue;
    }
    // Found again by its key: PlaceWalkedCars (above) gives back the meshes of the cars that left the walk's set, and one of them can be a mesh placed in this very frame:
    // its entry is erased and its structure retired, so p.node would point into an erased node and the build would get a null structure (ppr1, 2026-11-21).
    Mesh* const mesh = rr::BuildableMesh(meshes_, p.key);
    if (mesh == nullptr) {
      continue;
    }
    list->barriers(plume::RenderBarrierStage::COMPUTE,
                   plume::RenderBufferBarrier(res.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
    list->buildBottomLevelAS(mesh->structure.get(), plume::RenderBufferReference(res.scratch.get(), 0), p.info);
    recorded = true;
  }

  if (count != 0) {
    // CHECK: the BLAS and TLAS builds on the D3D12 debug layer: the inputs are moved from COPY_DEST to ALL_SHADER_RESOURCE by SetAccess
    // (a buffer in COMMON would rely on implicit promotion), the scratch is in UNORDERED_ACCESS by its barrier, the structures stay in
    // RAYTRACING_ACCELERATION_STRUCTURE, whose barrier is a UAV barrier.
    std::memcpy(res.instances_cpu, tlas_info.instancesBufferData.data(), tlas_info.instancesBufferData.size());
    std::memcpy(res.table_cpu, rows.data(), rows.size() * sizeof(rr::InstanceRow));
    std::memcpy(res.materials_cpu, material_words.data(), material_words.size() * sizeof(uint32_t));
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
    // The shared mesh buffers exist only once a car mesh was placed; a top level that holds the city and no placed car mesh (the car's draws are in the frame, their meshes were not built or were
    // given up) names the stand-in instead, as OnResolveCityOnly does: Plume's Vulkan setBuffer ignores a null buffer, which would leave this slot's binding as it was (unwritten, or a buffer
    // retired since), and a set that names nothing is not valid on every backend.
    res.set->setBuffer(indices_index_, indices_.buffer ? indices_.buffer.get() : standin_.get(), 0);
    res.set->setBuffer(normals_index_, normals_.buffer ? normals_.buffer.get() : standin_.get(), 0);
    const plume::RenderBufferStructuredView materials_view(sizeof(uint32_t) * 4, 0);
    res.set->setBuffer(uvs_index_, uvs_.buffer ? uvs_.buffer.get() : standin_.get(), 0);
    res.set->setBuffer(materials_index_, res.materials.get(), uint64_t(material_words.size()) * sizeof(uint32_t), &materials_view);
    NoteMaterials(cars, material_words);
    if (hit_) {
      // The set has rt-hit-shading's t4 and t5 and the hit data's t6..t9 after them (6 + CityHitScene::kBindingCount views, Initialize).
      hit_->Bind(res.set.get(), hit_bindings_, slot);  // t6..t9: the hit data's pool and tables (city_hit_scene.h)
      hit_->RecordProbe(list, res.set.get(), set_builder_.descriptorSetDesc, *passes_, *heap_);  // debug: reads them back through the set
    }
    if (world_) {
      world_->NoteView(frame.view, flip_z, /*car=*/true);  // CITY-RT FRAME: the top level's view space, and t10, t11 (the scene's trigger block fills them next: rt_world_frame.h)
      world_->Bind(res.set.get(), world_bindings_, slot);
    }
    view_.valid = true;
    view_.scene_valid = true;
    if (service_) {
      service_->NoteCarTop();
    }
    view_.set = res.set.get();
    view_.instance_count = count;
    view_.bounds = scene_bounds;
    view_.boxes = cars.boxes.empty() ? nullptr : cars.boxes.data();
    view_.box_count = uint32_t(cars.boxes.size());
    view_.box_row = cars.box_row;
    view_.car_row = cars.car_row;
    view_.car_count = uint32_t(cars.records.size());
    view_.records = cars.records;
    view_.player_box = cars.player_box;
    if (service_ && service_->Wants(rt_scene::Consumer::kRtAo)) {
      for (uint32_t i = 0; i < cars.stats.player_instances; ++i) {
        const auto& item = cars.items[i];
        const uint64_t part = item.mesh_key ^ (uint64_t(item.mask) << 56);
        view_.player_mesh_key += (part ^ (part >> 29)) * 0x9E3779B185EBCA87ull;
      }
    }
    view_.other_paint_instances = cars.other_paint_instances;
    view_.traffic_paint_instances = cars.traffic_paint_instances;
    view_.player_row = cars.player_row;
    view_.mirror = cars.mirror;
    view_.mirror_box[0] = cars.mirror_box[0];
    view_.mirror_box[1] = cars.mirror_box[1];
    if (!car_mirror::LensOf(frame.view, frame.view_projection, &view_.tan_x, &view_.tan_y)) {
      view_.tan_x = 0.0;
      view_.tan_y = 0.0;
    }
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
  check_due_ = larecomp::settings::mcla_gpu_car_rt_check.Get() && view_.valid;
  return recorded;
}

// ---------------------------------------------------------------------------
// the top level without the car (CITY-RT M2 H3, rt_scene_service.h)
// ---------------------------------------------------------------------------
// The player's car is what finds the frame's camera (CarFrame reads it out of the car's draws) and what the top level was built for. A consumer of the WORLD (the city's sun shadows where the
// car is out of the picture, M2's reflections of the street, M3's probes) needs the city in a top level in every frame, car or no car. So when OnResolve finds no car in the frame, or
// nobody asked for the cars, the scene service decides (PlanTlas): with the city wanted and a camera known, the city's instances alone are placed by the PREDICTED camera (the last frames'
// anchor poses carried one step on, rt_scene_rules.h CameraPredictor; a frame that is a cut holds the last pose), built into this slot's top level and bound in this slot's set exactly as
// the car's is. `view_.valid` stays false (it is the CAR's top level: no car effect runs on it), `view_.scene_valid` is true.
// This stays apart from OnResolve's own tail on purpose (the car path and its descriptor writes are rt-hit-shading's too): merge the two once both have landed.
// CHECK: the predicted camera against the depth image: mcla_gpu_city_rt_check (the primary rays against the depth, within 1 and 5 percent) in a frame with no car is the proof; its numbers
// are in perf/CITY-RT-H3-RUNPLAN.md.
bool CarRtScene::OnResolveCityOnly(plume::RenderCommandList* list, uint32_t slot) {
  if (service_ == nullptr || city_ == nullptr || hit_ == nullptr || !standin_ || !city_->wanted()) {
    return false;
  }
  const rt_scene::TlasPlan plan = service_->PlanTlas(/*car_frame_valid=*/false);
  if (!plan.build || plan.view != rt_scene::ViewSource::kCamera) {
    return false;
  }
  temporal::Mat4 view;
  bool flip_z = true;
  // CHECK: flip_z comes from rt_scene_rules.h CameraPredictor::LooksDownMinusZ (the anchor's view looks down -z: true); a mirrored or inside-out city in the check's picture says it is wrong.
  if (!service_->PredictedView(&view, &flip_z)) {
    return false;
  }
  std::vector<plume::RenderTopLevelASInstance> instances;
  rt_scene::Bounds scene_bounds;
  city_->Prepare(view, flip_z, list, slot, &instances, &scene_bounds);
  bool recorded = city_->recorded();
  const uint32_t count = uint32_t(instances.size());
  if (count == 0) {
    return recorded;
  }
  SlotResources& res = slots_[slot];
  plume::RenderTopLevelASBuildInfo tlas_info;
  device_->device()->setTopLevelASBuildInfo(tlas_info, instances.data(), count, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
  static const rr::InstanceRow kOneRow{};  // the table the set names: one row, no instance of this top level looks at it
  if (!EnsureSlot(res, slot, tlas_info.accelerationStructureSize, tlas_info.scratchSize, tlas_info.instancesBufferData.size(), sizeof(rr::InstanceRow), sizeof(uint32_t) * 4)) {
    REXLOG_ERROR("{} the frame slot's buffers could not be made: no top level of the city this frame", kPrefix);
    return recorded;
  }
  std::memcpy(res.instances_cpu, tlas_info.instancesBufferData.data(), tlas_info.instancesBufferData.size());
  std::memcpy(res.table_cpu, &kOneRow, sizeof(rr::InstanceRow));
  std::memset(res.materials_cpu, 0, sizeof(uint32_t) * 4);  // RT-HIT-SHADING's materials binding (t5): one empty row, no instance of this top level is a car
  list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(res.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  list->buildTopLevelAS(res.tlas.get(), plume::RenderBufferReference(res.scratch.get(), 0), plume::RenderBufferReference(res.instances.get(), 0), tlas_info);
  list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE),
                 plume::RenderBufferBarrier(res.tlas_buffer.get(), plume::RenderBufferAccess::READ));
  // The set, as the car path writes it, with the stand-in where a car mesh buffer does not exist yet.
  const plume::RenderBufferStructuredView rows_view(sizeof(rr::InstanceRow), 0);
  res.set->setAccelerationStructure(tlas_index_, res.tlas.get());
  res.set->setBuffer(table_index_, res.table.get(), sizeof(rr::InstanceRow), &rows_view);
  res.set->setBuffer(indices_index_, indices_.buffer ? indices_.buffer.get() : standin_.get(), 0);
  res.set->setBuffer(normals_index_, normals_.buffer ? normals_.buffer.get() : standin_.get(), 0);
  const plume::RenderBufferStructuredView materials_view(sizeof(uint32_t) * 4, 0);
  res.set->setBuffer(uvs_index_, uvs_.buffer ? uvs_.buffer.get() : standin_.get(), 0);
  res.set->setBuffer(materials_index_, res.materials.get(), sizeof(uint32_t) * 4, &materials_view);
  hit_->Bind(res.set.get(), hit_bindings_, slot);
  hit_->RecordProbe(list, res.set.get(), set_builder_.descriptorSetDesc, *passes_, *heap_);
  if (world_) {
    world_->NoteView(view, flip_z, /*car=*/false);  // CITY-RT FRAME: the predicted camera's view space, and t10, t11
    world_->Bind(res.set.get(), world_bindings_, slot);
  }
  view_.scene_valid = true;
  view_.set = res.set.get();
  view_.instance_count = count;
  view_.bounds = scene_bounds;
  {
    const double world_up[3] = {0.0, 1.0, 0.0};
    car_rt_instances::ViewVector(view, flip_z, world_up, view_.up);  // false: the default (0, 1, 0) stays
  }
  return true;
}

// ---------------------------------------------------------------------------
// LU-8: the game's lamps for the other cars' lenses
// ---------------------------------------------------------------------------

// The light points' source is on while the setting is and other cars are in the scene (the hook then notes every kind of every car: light_points.h Want, the ray traced scene's own
// consumer), polled once a frame before the instance list is built (the points of the last 100 ms become cars, car_lights.h Build) and flattened into the lamp set the list reads
// (car_rt_lights_rules.h MakeLampSet). Cars are ranked by the distance to the player's vehicle, the player's own first (the lamps of cars the scene does not hold are of no use here, and
// the table holds at most light_frame.h kMaxCars). With the setting off, or no other cars, the set is empty and every lens keeps the night factor's guess.
void CarRtScene::UpdateLamps(const car_frame::Frame& frame, bool others_on) {
  namespace cl = larecomp::gpu::car_lights;
  const bool want = others_on && larecomp::settings::mcla_gpu_car_rt_lamps.Get();
  if (want != lights_source_.enabled()) {
    lights_source_.SetEnabled(want);
    if (want && !lamps_logged_) {
      lamps_logged_ = true;
      REXLOG_INFO("{} the other cars' lenses follow the game's own lamps (the light points of every car, 19 kinds, polled once a frame: LIGHT-UNIFY LU-8); a lens no lamp stands in keeps the night "
                  "factor's guess; mcla_gpu_car_rt_lamps (debug) is the switch", kPrefix);
    }
  }
  if (!want) {
    lamp_set_.Clear();
    instances_.SetLamps(nullptr);
    return;
  }
  static const std::vector<cl::FrameIn> kNoFrames;
  const cl::CarLightsFrame& lights = lights_source_.Poll(kNoFrames, frame.player_vehicle_known ? frame.player_vehicle : nullptr);
  car_rt_lights::MakeLampSet(lights, &lamp_set_);
  lens_stats_.NoteFrame(lamp_set_);
  instances_.SetLamps(lamp_set_.Valid() ? &lamp_set_ : nullptr);
}

// ---------------------------------------------------------------------------
// RT-HIT-SHADING: the frame's material rows
// ---------------------------------------------------------------------------

// The diffuse texture of a material as a bindless slot THIS frame, asked of the texture cache by the fetch constant the draw had (the slot a draw bound may be gone by now: a car
// the camera does not draw holds nothing alive). A texture the cache does not hold shades with the class colour. The answers of a frame are kept by the fetch (a frame's
// instances share a few dozen textures).
uint32_t CarRtScene::DiffuseSlot(const car_rt_material::FetchInfo& fetch) {
  if (!textures_ || !fetch.texture) {
    return car_rt_material::kNoSlot;
  }
  uint64_t key = 0xCBF29CE484222325ull;
  for (const uint32_t dword : fetch.dwords) {
    key = (key ^ dword) * 0x100000001B3ull;
    key ^= key >> 29;
  }
  if (const auto found = diffuse_slots_.find(key); found != diffuse_slots_.end()) {
    return found->second;
  }
  const TextureFetch texture = ReadTextureFetch(fetch.dwords);
  const uint32_t slot = textures_->Peek2D(texture);
  const uint32_t answer = (slot & ~kTextureGammaBit) == kNullTexture2DSlot ? car_rt_material::kNoSlot : slot;
  diffuse_slots_.emplace(key, answer);
  return answer;
}

// The rows the shaders read at binding 5: the frame's light (the car's own draws' constants, the sun and the world's axes in this frame's view space) and a material an instance.
void CarRtScene::BuildMaterialRows(const car_frame::Frame& frame, bool flip_z, const car_rt_instances::List& list, std::vector<uint32_t>* words) {
  namespace crm = car_rt_material;
  diffuse_slots_.clear();
  const size_t items = list.items.size();
  words->assign((size_t(crm::kFrameRows) + items * crm::kMaterialRows) * 4u, 0u);
  crm::FrameInput in;
  in.light = frame.light;
  in.tuning.enabled = larecomp::settings::mcla_gpu_car_rt_hit_shading.Get();
  in.tuning.shadow = larecomp::settings::mcla_gpu_car_rt_hit_shadow.Get();
  // the gain is the setting's times the exposure the reflection pass measured against the history's own colours (car_rt_hit_rules.h census::; 1 until it has, and with the calibration off)
  in.tuning.gain = float(larecomp::settings::mcla_gpu_car_rt_hit_gain.Get()) * car_rt_hit::census::GainOf(hit_stops_);
  in.tuning.ambient = float(larecomp::settings::mcla_gpu_car_rt_hit_ambient.Get());
  in.tuning.spec = float(larecomp::settings::mcla_gpu_car_rt_hit_spec.Get());
  in.tuning.cascade = larecomp::settings::mcla_gpu_car_rt_hit_cascade.Get();
  in.tuning.detail = float(larecomp::settings::mcla_gpu_car_rt_hit_detail.Get());  // RT-HIT-SHADING-5: the hit's own detail (the sky's openness, the contact, the coat's glints)
  in.cascades = sun_cascades_;
  {
    double camera[3];
    if (car_rt_instances::CameraWorld(frame.view, camera)) {
      in.camera_known = true;
      for (int c = 0; c < 3; ++c) {
        in.camera_world[c] = float(camera[c]);
      }
    }
  }
  // The sun is the WORLD's (SetWorldSun), not the cars' own draws' light direction (Frame::to_sun): that one read -5 degrees at noon, so the hour from the sun's height said night by day (RT-NEXT-DAY,
  // car_rt_material_rules.h PickFrameSun). No world sun this frame: no sun, the shading by daylight.
  if (world_sun_known_) {
    in.sun_known = car_rt_instances::ViewVector(frame.view, flip_z, world_sun_, in.to_sun_view);
  }
  for (int axis = 0; axis < 3; ++axis) {
    const double unit[3] = {axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0};
    car_rt_instances::ViewVector(frame.view, flip_z, unit, in.world_axes_view[axis]);  // false: the identity stays
  }
  uint32_t(*rows)[4] = reinterpret_cast<uint32_t(*)[4]>(words->data());
  crm::MakeFrameRows(in, rows);
  const crm::InstanceMaterial none;
  for (size_t i = 0; i < items; ++i) {
    const crm::InstanceMaterial& im = i < list.materials.size() ? list.materials[i] : none;
    const uint32_t slot = im.material.diffuse.texture ? DiffuseSlot(im.material.diffuse) : crm::kNoSlot;
    crm::MakeRows(im, slot, reinterpret_cast<uint32_t(*)[4]>(words->data() + (size_t(crm::kFrameRows) + i * crm::kMaterialRows) * 4u));
  }
}

// What the materials were: the window's totals (the class of every instance, the textures asked for and found, the lighting read), the first of each class and of the
// frame's light in the log. A line every kMaterialLogFrames triggers while the shading is on.
void CarRtScene::NoteMaterials(const car_rt_instances::List& list, const std::vector<uint32_t>& words) {
  namespace crm = car_rt_material;
  constexpr uint64_t kMaterialLogFrames = 300;
  MaterialStats& m = material_stats_;
  if (m.first_frame == 0) {
    m.first_frame = frame_;
  }
  const car_frame::Frame& frame = car_frame_->Current();
  ++m.frames;
  m.light_frames += frame.light.valid ? 1u : 0u;
  m.env_frames += frame.light.env_sharp != 0 ? 1u : 0u;
  m.sun_frames += world_sun_known_ ? 1u : 0u;
  m.cascade_frames += (words.size() > 3 && (words[3] & crm::kFrameCascade) != 0u) ? 1u : 0u;
  lens_stats_.glowing += list.stats.lens_glowing;
  lens_stats_.dark += list.stats.lens_dark;
  lens_stats_.undriven += list.stats.lens_undriven;
  lens_stats_.lenses += list.stats.lens_glowing + list.stats.lens_dark + list.stats.lens_undriven;
  lens_stats_.level_max = std::max(lens_stats_.level_max, list.stats.lens_level_max);
  for (size_t i = 0; i < list.items.size(); ++i) {
    const uint32_t* row = words.data() + (size_t(crm::kFrameRows) + i * crm::kMaterialRows) * 4u;
    const crm::RowsDecoded decoded = crm::DecodeRows(reinterpret_cast<const uint32_t(*)[4]>(row));
    ++m.instances;
    ++m.by_class[std::min<uint32_t>(uint32_t(decoded.cls), crm::kClassCount - 1)];
    const bool textured = i < list.materials.size() && list.materials[i].material.diffuse.texture;
    if (textured) {
      ++m.textured;
      ++(decoded.slot != crm::kNoSlot ? m.resolved : m.unresolved);
    }
    const uint32_t cls = uint32_t(decoded.cls);
    if (cls != 0 && cls < crm::kClassCount && !material_class_logged_[cls] && i < list.materials.size()) {
      material_class_logged_[cls] = true;
      const bool player = i < list.stats.player_instances;
      std::string record;
      if (decoded.cls == crm::Class::kPaint) {
        for (const car_model_rt::CarInfo& info : paint_infos_) {
          if (info.is_player == player && info.paint.known) {
            record = std::format("; the car's customisation record: A ({:.3f} {:.3f} {:.3f}) B ({:.3f} {:.3f} {:.3f}), A raised to the gamma ({:.3f} {:.3f} {:.3f})", info.paint.a[0],
                                 info.paint.a[1], info.paint.a[2], info.paint.b[0], info.paint.b[1], info.paint.b[2], std::pow(std::max(info.paint.a[0], 0.0f), crm::kWalkedPaintGamma),
                                 std::pow(std::max(info.paint.a[1], 0.0f), crm::kWalkedPaintGamma), std::pow(std::max(info.paint.a[2], 0.0f), crm::kWalkedPaintGamma));
            break;
          }
        }
      }
      REXLOG_INFO("{} frame {}: the first instance of class {} ({}'s): {}; the row's slot {}, mip bias {:.2f}{}", kPrefix, frame_, crm::ClassName(decoded.cls), player ? "the player's car" : "another car",
                  crm::Describe(list.materials[i].material, 0), decoded.slot == crm::kNoSlot ? std::string("none") : std::format("{:#x}", decoded.slot), decoded.lod_base, record);
    }
  }
  if (!material_frame_logged_ && frame.light.valid) {
    material_frame_logged_ = true;
    crm::PickedSun world_sun;
    world_sun.known = world_sun_known_;
    for (int c = 0; c < 3; ++c) {
      world_sun.to_sun[c] = world_sun_[c];
    }
    REXLOG_INFO("{} frame {}: the cars' light as their draws saw it: sun ({:.3f} {:.3f} {:.3f}), ambient ({:.3f} {:.3f} {:.3f}), sky ({:.3f} {:.3f} {:.3f}), night factor {:.2f} (by the sun's height; by the colour {:.2f}), "
                "the environment map's slots {:#x} and {:#x}, the sun's direction in the view space ({:.2f} {:.2f} {:.2f}), the sun: {} (the cars' own draws' light direction read {:.1f} degrees: not the world's), shading {}",
                kPrefix, frame_, frame.light.sun[0], frame.light.sun[1], frame.light.sun[2], frame.light.ambient[0], frame.light.ambient[1], frame.light.ambient[2], frame.light.sky[0], frame.light.sky[1],
                frame.light.sky[2], crm::FromBits(words[3 * 4 + 2]), crm::NightFactor(frame.light.sun), frame.light.env_sharp, frame.light.env_blur, crm::FromBits(words[0]), crm::FromBits(words[1]),
                crm::FromBits(words[2]),
                world_sun.known ? std::format("the world's, {:.1f} degrees over the horizon", crm::SunElevationDegrees(world_sun)) : std::string("none this frame (no world light: the shading is by daylight)"),
                std::asin(std::clamp(frame.to_sun[1], -1.0, 1.0)) * (180.0 / 3.14159265358979323846),
                larecomp::settings::mcla_gpu_car_rt_hit_shading.Get() ? "on" : "off (today's stand-in)");
  }
  if (frame_ >= m.first_frame + kMaterialLogFrames && m.frames != 0) {
    std::string classes;
    for (uint32_t c = 0; c < crm::kClassCount; ++c) {
      if (m.by_class[c] != 0) {
        classes += std::format("{}{} {:.1f}", classes.empty() ? "" : ", ", crm::ClassName(crm::Class(c)), double(m.by_class[c]) / double(m.frames));
      }
    }
    REXLOG_INFO("{} materials, frames {}..{}: {} with a top level; instances a frame by class: {}; textured {:.1f} a frame, {:.1f}% found in the cache, {} not found; the lighting read from the "
                "cars' draws in {} frames, the environment map's slot in {}, the sun's direction in {}, the game's sun cascades (the sun's shadow at a shaded hit) in {}; meshes read with texture coordinates {}, without {}",
                kPrefix, m.first_frame, frame_, m.frames, classes.empty() ? std::string("none") : classes, double(m.textured) / double(m.frames),
                m.textured != 0 ? 100.0 * double(m.resolved) / double(m.textured) : 100.0, m.unresolved, m.light_frames, m.env_frames, m.sun_frames, m.cascade_frames, m.uv_meshes_read, m.no_uv_meshes_read);
    if (lens_stats_.frames != 0) {
      REXLOG_INFO("{} lamps, frames {}..{}: {}", kPrefix, m.first_frame, frame_, car_rt_lights::Text(lens_stats_));
      lens_stats_ = car_rt_lights::LensStats{};
    }
    const uint64_t first = frame_;
    material_stats_ = MaterialStats{};
    material_stats_.first_frame = first;
  }
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
                "refused, {} dropped, most held {}; most vehicles near the player {}, worst match offset {:.2f} m, {} triggers without a readable list; the kits' prediction of "
                "the game's own draws of the same cars (placed at the vehicle's matrix of that frame): {} draws, mean error {:.3f} m, worst {:.3f} m; last frame: {}",
                kPrefix, memory_log_frame_, frame_, t.frames, t.frames_placed, t.cars_placed, t.draws_placed, t.not_resident, t.most_placed, t.recorded, t.record_refused,
                t.dropped, t.most_kits, t.vehicles_max, t.worst_offset, t.no_player, t.predicted_draws,
                t.predicted_draws != 0 ? t.prediction_sum / double(t.predicted_draws) : 0.0, t.prediction_worst, car_rt_memory::Describe(s));
  }
  memory_.ResetTotals();
  memory_log_frame_ = frame_;
}

// Gives a walked mesh back: staged (not built yet) or resident (its ranges and structure go back once the GPU is done with the frame).
void CarRtScene::GiveBack(uint64_t key) {
  if (staged_.erase(key) != 0) {
    std::erase(staged_order_, key);
  }
  if (meshes_.contains(key)) {
    Evict(key);
  }
  walked_keys_.erase(key);
  ++walked_stats_.given_back;
}

// THE MODEL WALK'S CARS IN THE TOP LEVEL. The walker (car_model_walk.h) walked the cars in the frame's first vehicle draw; here, at the history trigger, its Resolve says
// which of them the frame drew (the draw path keeps those) and checks its placements against the draws of the cars the camera sees; then the cars to place (the class's
// count by the distance to the player, not drawn, and by the mode not where a remembered car stands) get an OtherCar and one OtherDraw per part whose mesh has a
// structure, the draw's model-to-world being the part's placement in WORLD space: the instance list puts it through the frame's view like every draw's
// (rr::PackInstanceTransform with the scene's own flip), which is the conversion the first memory check lacked. The remembered cars the walk can give a paint colour
// to (a walked car at the same place) get it. extra_cars_ / extra_draws_ come in with the remembered cars' and leave with the walked ones added.
void CarRtScene::PlaceWalkedCars(const car_frame::Frame& frame, bool flip_z) {
  namespace crt = car_model_rt;
  car_model_walk::Walker& walker = car_frame_->walker();
  if (!walker.producing()) {
    walked_present_.clear();
    paint_infos_.clear();
    walked_composition_.clear();
    walked_traffic_placed_ = 0;
    return;
  }
  const car_model_walk::ProofReport proof = walker.Resolve(frame.view, flip_z, &frame.other_cars);
  const std::vector<car_model_walk::WalkedCar>& walked = walker.walked();
  const car_rt_instances::Limits& limits = car_frame_->other_limits();
  ++walked_stats_.walk_frames;
  walked_stats_.proof_pairs += proof.pairs;
  walked_stats_.proof_worst = std::max(walked_stats_.proof_worst, proof.worst);

  std::vector<crt::CarInfo> infos;
  infos.reserve(walked.size());
  for (const car_model_walk::WalkedCar& car : walked) {
    crt::CarInfo info;
    info.vehicle = car.vehicle;
    info.is_player = car.is_player;
    info.drawn = car.drawn;
    info.visible = car.visible;
    info.unclaimed = car.unclaimed;
    info.traffic = car.traffic;
    info.distance = car.distance;
    info.paint = car.paint;
    info.parts = uint32_t(car.parts.size());
    for (int c = 0; c < 3; ++c) {
      info.position[c] = car.position[c];
    }
    infos.push_back(info);
  }
  // the cars the camera draws take the paint of the walked car at their place (CarRtInstances::Build): the game draws a neighbour beside the door while the camera, in the gap
  // beside it, sees none of it, so the history has no colour of it (RT-MODEL-CARS-3)
  paint_infos_ = infos;
  const crt::Mode mode = instances_.ModelMode();
  std::vector<uint32_t> placed = car_model_parts::ChoosePlacedAll(infos, limits, mode, remembered_.cars, walker.production().traffic_cars);
  // No car is placed by part matrices that the draws of the cars the camera sees have not confirmed (the calibration decided, and some hypothesis fits them): a
  // wrong placement would put a car in the wrong place, which is worse than none.
  // (The traffic cars' parts exist only once the traffic calibration fits the draws: they are not held here; the racers' are.)
  const bool pose_ok = walker.calibration().fits();
  if (!pose_ok) {
    const size_t racers = size_t(std::count_if(placed.begin(), placed.end(), [&](uint32_t i) { return !walked[i].traffic; }));
    if (racers != 0) {
      if (frame_ >= walked_pose_warn_frame_ + 600) {
        walked_pose_warn_frame_ = frame_;
        REXLOG_WARN("{} frame {}: {} walked cars are not placed: {} ({} pairs of a walked geometry and its draw compared so far: {})", kPrefix, frame_, racers,
                    walker.calibration().decided() ? "no hypothesis for the part matrices fits the draws of the cars the camera sees" : "the placement is not checked against the draws yet",
                    walker.calibration().samples(), walker.calibration().Describe());
      }
      ++walked_stats_.held_pose;
      std::erase_if(placed, [&](uint32_t i) { return !walked[i].traffic; });
    }
  }

  // the model data first: a remembered car at a walked car's place is the same car, and goes
  if (mode == crt::Mode::kModelFirst && !placed.empty()) {
    std::vector<uint32_t> drop = crt::RememberedCovered(extra_cars_, infos, placed);
    if (!drop.empty()) {
      std::unordered_set<uint32_t> ids;
      for (const uint32_t k : drop) {
        ids.insert(extra_cars_[k].stencil_id);
      }
      std::erase_if(extra_draws_, [&](const car_rt_instances::OtherDraw& d) { return ids.contains(d.stencil_id); });
      std::erase_if(extra_cars_, [&](const car_rt_instances::OtherCar& c) { return ids.contains(c.stencil_id); });
    }
  }
  // the paint of the remembered cars that stay (the kit knows their shapes, not their colour: the history holds nothing of a car the camera does not draw)
  for (car_rt_instances::OtherCar& car : extra_cars_) {
    float rgb[3];
    if (car.remembered && !car.walked && mode != crt::Mode::kOff && crt::PaintForRemembered(car, infos, rgb)) {
      car.has_paint = true;
      for (int c = 0; c < 3; ++c) {
        car.paint[c] = rgb[c];
      }
      ++walked_stats_.painted;
    }
  }

  // the walked cars
  std::vector<crt::WalkedSet::Placed> keeping;
  std::unordered_set<uint32_t> present;
  uint32_t slot = 0, instances = 0, left_out = 0;
  std::string entered;
  std::unordered_map<uint32_t, car_model_parts::Composition> resident_by_car;
  uint32_t traffic_placed = 0, racers_placed = 0, parts_body = 0, parts_tyres = 0, parts_rims = 0, parts_model = 0, parts_wheels_held = 0;
  uint32_t parked_placed = 0, parked_instances = 0;  // RT-PARKED-CARS: placed cars the main view shows (no draw is theirs)
  const bool parked_paint_on = larecomp::settings::mcla_gpu_car_rt_parked_paint.Get();  // RT-PARKED-PAINT: live, read once a frame
  uint32_t parked_paint_cars = 0;
  for (const uint32_t index : placed) {
    const car_model_walk::WalkedCar& car = walked[index];
    car_rt_instances::OtherCar other;
    other.stencil_id = crt::kWalkedIdBase + slot;
    other.remembered = true;  // the camera does not draw it: the history holds nothing of it (a negative radius in its record)
    other.walked = true;
    // RT-PARKED-PAINT: a traffic car the main view shows is a parked one (the placed cars are never drawn this frame): the camera sees it, the history holds its marked texels, its parts carry the
    // traffic paint's bit and its record's radius is positive (car_rt_instances_rules.h ParkedPaintPart, RecordRadius)
    const bool parked_paint = parked_paint_on && car.traffic && car.visible;
    other.parked = parked_paint;
    other.distance = car.distance;
    const crt::PaintColour paint = infos[index].paint;
    if (paint.known) {
      other.has_paint = true;
      other.has_paint_ab = true;  // the record's colours A and B: the walked parts of paint class take them (car_rt_material_rules.h WalkedMaterial)
      for (int c = 0; c < 3; ++c) {
        other.paint[c] = paint.rgb[c];
        other.paint_a[c] = paint.a[c];
        other.paint_b[c] = paint.b[c];
      }
      ++walked_stats_.painted;
    }
    crt::WalkedSet::Placed keep;
    keep.vehicle = car.vehicle;
    uint32_t order = 0, resident = 0;
    car_model_parts::Composition comp;
    comp.skinned_left_out = car.composition.skinned_left_out;
    for (const crt::Part& part : car.parts) {
      keep.keys.push_back(part.key);
      const auto mesh = meshes_.find(part.key);
      if (mesh == meshes_.end() || !mesh->second.structure) {
        ++left_out;
        continue;
      }
      car_model_parts::Count(&comp, part.part);
      car_rt_instances::OtherDraw draw;
      draw.stencil_id = other.stencil_id;
      draw.order = order++;
      draw.part = part.part;  // RT-HIT-SHADING: the part's usual material (the model data has no shader to ask)
      draw.parked_paint = parked_paint;
      draw.mesh_key = part.key;
      draw.indices = part.indices;
      draw.model_to_world = part.world;
      other.Add(part.world, car.distance);
      extra_draws_.push_back(draw);
      ++resident;
    }
    keeping.push_back(std::move(keep));
    if (resident == 0) {
      continue;  // nothing of it is built yet (the first frames of a car that is new)
    }
    present.insert(car.vehicle);
    instances += resident;
    if (car.traffic && car.visible) {
      ++parked_placed;
      parked_instances += resident;
      parked_paint_cars += parked_paint ? 1u : 0u;
      if (parked_logged_ < 8 && parked_seen_.insert(car.vehicle).second) {
        ++parked_logged_;
        REXLOG_INFO("{} frame {}: parked car {:#010x} ({:.1f} m from the player) is shown by the main view and no draw is its: placed in the top level from its model data, {} of {} parts (RT-PARKED-CARS)", kPrefix,
                    frame_, car.vehicle, car.distance, resident, car.parts.size());
      }
    }
    extra_cars_.push_back(other);
    entered += std::format("{}{:#010x} at {:.1f} m: {} of {} parts, paint {}", entered.empty() ? "" : "; ", car.vehicle, car.distance, resident, car.parts.size(), crt::Describe(paint));
    ++slot;
    // RT-MODEL-PARTS: what it has in the top level by kind
    resident_by_car[car.vehicle] = comp;
    traffic_placed += car.traffic ? 1u : 0u;
    racers_placed += car.traffic ? 0u : 1u;
    parts_body += comp.body;
    parts_tyres += comp.tyres;
    parts_rims += comp.rims;
    parts_model += comp.model;
    parts_wheels_held += car.wheels_held;
  }
  // the meshes of the cars that left
  for (const uint64_t key : walked_set_.Update(frame_, keeping)) {
    GiveBack(key);
  }
  if (!walked.empty()) {
    ++walked_stats_.frames_placed;
  }
  walked_stats_.cars_placed += present.size();
  walked_stats_.instances += instances;
  walked_stats_.missing_parts += left_out;
  walked_stats_.most_cars = std::max<uint32_t>(walked_stats_.most_cars, uint32_t(present.size()));
  walked_stats_.walk_us_sum += walker.last_walk_microseconds();
  walked_stats_.most_built_triangles = std::max(walked_stats_.most_built_triangles, walked_build_triangles_frame_);
  walked_traffic_placed_ = traffic_placed;
  NotePartsKinds(walker, walked, resident_by_car, present, traffic_placed, racers_placed, parts_body, parts_tyres, parts_rims, parts_model, parts_wheels_held, proof);
  NoteParkedCars(proof, parked_placed, parked_instances, parked_paint_cars);

  // the log: a car comes or goes, and the totals every few hundred frames
  if (present != walked_present_) {
    std::string left;
    for (const uint32_t v : walked_present_) {
      if (!present.contains(v)) {
        left += std::format("{}{:#010x}", left.empty() ? "" : ", ", v);
      }
    }
    if (!present.empty() || !left.empty()) {
      REXLOG_INFO("{} frame {}: walked cars in the top level (built from the cars' own model data, placed by '{}', {} view space z): {} [{}]{}; {} other cars with {} instances in all, "
                  "{} of them placed from memory or from model data",
                  kPrefix, frame_, crt::PoseName(walker.pose()), flip_z ? "flipped to the AO's" : "the AO's", present.size(), entered,
                  left.empty() ? std::string() : std::format("; left: {}", left), extra_cars_.size(), extra_draws_.size(), extra_cars_.size());
    }
    walked_present_ = present;
    walked_first_logged_ = walked_first_logged_ || !present.empty();
  }
  if (walked_log_frame_ == 0) {
    walked_log_frame_ = frame_;
  }
  if (frame_ >= walked_log_frame_ + 300 && walked_stats_.walk_frames != 0) {
    const WalkedStats& w = walked_stats_;
    REXLOG_INFO("{} walked cars (car_model_rt_rules.h), frames {}..{}: walk in {} frames ({:.0f} us a frame on the render thread, {} frames with no car placed for want of a checked pose), cars placed in {} of them ({} in all, at most {} at once, {} instances, {} parts left out for want of a "
                "structure); read from the cars' model data: {} meshes ({} triangles, {} KiB; at most {} triangles and {} KiB in a frame; {} could not be read, {} held back by the class's budget of {} triangles and {} KiB a frame, {} by the "
                "memory share); built: {} structures ({} triangles, at most {} in a frame, the walked budget {} a frame); given back {}; cars with a paint colour {} (and {} car-frames in which a car the game draws took the paint of the walked car at its place); pose '{}' ({}), the "
                "placement proof of this window: {} pairs, worst {:.4f} m; last frame: {} walked cars, {} drawn by the frame (the draw path keeps those), the proof {}",
                kPrefix, walked_log_frame_, frame_, w.walk_frames, w.walk_frames != 0 ? w.walk_us_sum / double(w.walk_frames) : 0.0, w.held_pose, w.frames_placed, w.cars_placed, w.most_cars, w.instances, w.missing_parts, w.meshes_read, w.triangles_read, w.kib_read,
                w.most_read_triangles, w.most_read_kib, w.read_failed, w.held_budget, limits.stage_triangles, limits.content_bytes / 1024u, w.held_memory, w.meshes_built, w.triangles_built,
                w.most_built_triangles, limits.stage_triangles, w.given_back, w.painted, w.painted_drawn, crt::PoseName(walker.pose()),
                walker.calibration().decided() ? (walker.calibration().fits() ? "decided against the draws: it fits" : "decided against the draws: NO hypothesis fits") : "not decided yet",
                w.proof_pairs, w.proof_worst, proof.walked_cars, proof.drawn_cars,
                proof.ran ? std::format("{} geometries of {} cars, mean {:.4f} m, worst {:.4f} m", proof.pairs, proof.cars, proof.mean, proof.worst) : std::string("did not run"));
    // THE PAINT ROWS (RT-MODEL-CARS-4): for every car near the player, the game's customisation record (colours A and B, paint type, gradient mode) and the row made of it (the
    // colour raised to the gamma and times the light, in the paint's units); the reflection pass multiplies the row by 2^exp_bias and its shade, so a hidden hit on the car reaches the
    // paint as row x shade. The paint probe (the check's line) says the shader reads these rows as written.
    {
      const crt::PaintTuning tuning = car_model_walk::CurrentPaintTuning();
      std::string rows;
      for (const crt::CarInfo& info : infos) {
        if (info.distance > 25.0 && !info.is_player) {
          continue;
        }
        rows += std::format("{}{:#010x} ({}, {}, at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m): paint {}", rows.empty() ? "" : "; ", info.vehicle,
                            info.is_player ? "the player's" : info.traffic ? "a traffic car (no paint record: the reflection shows the fixed grey)" : "another car",
                            info.drawn ? "drawn" : "not drawn", info.position[0], info.position[1], info.position[2], info.distance, crt::Describe(info.paint));
      }
      REXLOG_INFO("{} paint rows, frame {} (row = record colour ^ {:.2f} x {:.2f}, in the paint's units; the pass multiplies it by 2^exp_bias and by its shade {}): {}", kPrefix, frame_, tuning.gamma,
                  tuning.lit, car_rt_refl::kShade, rows.empty() ? std::string("no car") : rows);
    }
    walked_stats_ = WalkedStats{};
    walked_log_frame_ = frame_;
  }
}

// RT-MODEL-PARTS: the kinds of what the walked cars have in the top level. Per frame the instances by kind are summed (body = the body shell and the model set's parts, wheels = tyres
// and rims, model = a traffic car's one drawable; the driver is never there: it is skinned and the walk and the draw path leave it out); a car's composition is logged once it has
// been the same for kCompositionStableFrames (a car appears in parts over a few frames, so the first lines of a new car would be partial), and a window line says the totals.
void CarRtScene::NotePartsKinds(car_model_walk::Walker& walker, const std::vector<car_model_walk::WalkedCar>& walked,
                                const std::unordered_map<uint32_t, car_model_parts::Composition>& resident_by_car, const std::unordered_set<uint32_t>& present, uint32_t traffic_placed,
                                uint32_t racers_placed, uint32_t body, uint32_t tyres, uint32_t rims, uint32_t model, uint32_t wheels_held, const car_model_walk::ProofReport& proof) {
  constexpr uint64_t kCompositionStableFrames = 20;
  PartsStats& s = parts_stats_;
  if (!present.empty()) {
    ++s.frames;
    s.body_sum += body;
    s.tyres_sum += tyres;
    s.rims_sum += rims;
    s.wheels_sum += uint64_t(tyres) + rims;
    s.model_sum += model;
    s.body_max = std::max(s.body_max, body);
    s.wheels_max = std::max(s.wheels_max, tyres + rims);
    s.model_max = std::max(s.model_max, model);
    s.racer_frames += racers_placed != 0 ? 1u : 0u;
    s.traffic_frames += traffic_placed != 0 ? 1u : 0u;
    s.traffic_most = std::max(s.traffic_most, traffic_placed);
    s.traffic_car_frames += traffic_placed;
  }
  s.wheels_held += wheels_held;

  // a car's composition, logged when it has settled
  for (const uint32_t vehicle : present) {
    const auto resident = resident_by_car.find(vehicle);
    if (resident == resident_by_car.end()) {
      continue;
    }
    CompositionState& state = walked_composition_[vehicle];
    if (!(resident->second == state.now) || state.since == 0) {
      state.now = resident->second;
      state.since = frame_;
    }
    if ((!state.logged_any || !(state.now == state.logged)) && frame_ >= state.since + kCompositionStableFrames) {
      for (const car_model_walk::WalkedCar& car : walked) {
        if (car.vehicle != vehicle) {
          continue;
        }
        const car_model_walk::CarWalk* walk = car.walk.get();
        std::string extra;
        if (!car.traffic && walk) {
          const car_model_walk::WheelWalk& w = walk->wheels;
          extra = std::format("; wheel resources tyre id {} ({}), rim id {} ({}), the draw's flags tyre {} rim {} hidden {}, wheels drawn {}{}{}{}; wheel pose tyres '{}' ({}), rims '{}' ({})", w.tyre_id,
                              w.tyre_ok ? "ok" : "NOT handed out", w.rim_id, w.rim_ok ? "ok" : "NOT handed out", w.tyre_on ? "on" : "off", w.rim_on ? "on" : "off", w.hidden ? "YES" : "no",
                              w.present[0] ? 1 : 0, w.present[1] ? 1 : 0, w.present[2] ? 1 : 0, w.present[3] ? 1 : 0, car_model_walk::WheelPoseName(walker.wheel_calibration().chosen(false)),
                              walker.wheel_calibration().fits(false) ? "fits the draws" : "NOT confirmed by the draws", car_model_walk::WheelPoseName(walker.wheel_calibration().chosen(true)),
                              walker.wheel_calibration().fits(true) ? "fits the draws" : "NOT confirmed by the draws");
        } else if (car.traffic) {
          extra = std::format("; item lod {}, car type {:#010x}, {} bones in the pose block, pose '{}' ({}){}", car.item_lod, car.car_type, car.traffic_pose.bones.size(),
                              car_model_walk::TrafficPoseName(walker.traffic_calibration().chosen()), walker.traffic_calibration().fits() ? "fits the draws" : "NOT confirmed by the draws",
                              car.colour.read ? std::format(", raw palette colour ({:.2f}, {:.2f}, {:.2f}) x {:.2f} (not used)", car.colour.rgba[0], car.colour.rgba[1], car.colour.rgba[2], car.colour.scale)
                                              : std::string());
        }
        REXLOG_INFO("{} frame {}: walked car {:#010x} ({}, {:.1f} m from the player) has in the top level: {}; {} parts walked ({} geometries in use){}", kPrefix, frame_, vehicle,
                    car.traffic ? "traffic" : "racer", car.distance, car_model_parts::Describe(state.now, car.traffic), car.parts.size(), walk ? walk->in_use.geometries : 0u, extra);
        break;
      }
      state.logged = state.now;
      state.logged_any = true;
    }
  }
  for (auto it = walked_composition_.begin(); it != walked_composition_.end();) {
    it = present.contains(it->first) ? std::next(it) : walked_composition_.erase(it);
  }

  // the window line
  if (parts_log_frame_ == 0) {
    parts_log_frame_ = frame_;
  }
  if (frame_ >= parts_log_frame_ + 300 && (s.frames != 0 || walker.traffic_scan().items != 0)) {
    const double n = double(std::max<uint64_t>(s.frames, 1));
    const car_model_walk::Walker::TrafficScan& scan = walker.traffic_scan();
    REXLOG_INFO("{} walked parts by kind, frames {}..{}: a walked car placed in {} frames; instances a frame (mean, most): body {:.1f} ({}), wheels {:.1f} ({}; {:.1f} tyre + {:.1f} rim), a traffic "
                "car's model {:.1f} ({}), driver 0 (skinned: left out as the draw path leaves it); racers placed in {} frames, traffic cars in {} frames (at most {} at once, {:.2f} a frame); "
                "racer car-frames whose wheels waited for a wheel pose that fits the draws {}; traffic scan (last frame): client {}, {} model nodes, {} render items ({} unreadable), "
                "{} with the car within the radius, {} of those rejected by the draw's own gates, {} shown by the main view, {} not shown by it with no usable bone block, {} placed by the "
                "block's check against the car's own matrix (no draw confirmed the pose); wheel pose: tyres '{}' ({}), rims '{}' ({}); traffic pose '{}' ({}); "
                "proofs of the last calibrating frame: wheels tyres {} pairs worst {:.4f} m, rims {} pairs worst {:.4f} m; traffic {} pairs worst {:.4f} m ({} traffic cars walked, {} drawn by the frame)",
                kPrefix, parts_log_frame_, frame_, s.frames, double(s.body_sum) / n, s.body_max, double(s.wheels_sum) / n, s.wheels_max, double(s.tyres_sum) / n, double(s.rims_sum) / n,
                double(s.model_sum) / n, s.model_max, s.racer_frames, s.traffic_frames, s.traffic_most, double(s.traffic_car_frames) / n, s.wheels_held, scan.client ? "found" : "NOT FOUND",
                scan.models, scan.items, scan.unreadable, scan.in_range, scan.rejected, scan.visible, scan.block_missing, scan.self_checked, car_model_walk::WheelPoseName(walker.wheel_calibration().chosen(false)),
                walker.wheel_calibration().fits(false) ? "fits" : walker.wheel_calibration().decided(false) ? "NO FIT" : "not decided", car_model_walk::WheelPoseName(walker.wheel_calibration().chosen(true)),
                walker.wheel_calibration().fits(true) ? "fits" : walker.wheel_calibration().decided(true) ? "NO FIT" : "not decided", car_model_walk::TrafficPoseName(walker.traffic_calibration().chosen()),
                walker.traffic_calibration().fits() ? "fits" : walker.traffic_calibration().decided() ? "NO FIT" : "not decided", proof.wheel_pairs[0], proof.wheel_worst[0], proof.wheel_pairs[1],
                proof.wheel_worst[1], proof.traffic_pairs, proof.traffic_worst, proof.traffic_walked, proof.traffic_drawn);
    parts_stats_ = PartsStats{};
    parts_log_frame_ = frame_;
  }
}

// RT-PARKED-CARS: the window line. A traffic car the main view shows is the draw path's when a draw of the frame is its or the draw path keeps a car at its place; the rest are UNCLAIMED (a parked
// car writes no vehicle stencil ID) and the walk places them like hidden cars, within the traffic count and radius. The line says how many there were and how many entered the top level.
void CarRtScene::NoteParkedCars(const car_model_walk::ProofReport& proof, uint32_t placed, uint32_t instances, uint32_t paint_cars) {
  ParkedStats& s = parked_stats_;
  ++s.frames;
  s.shown_sum += proof.traffic_shown;
  s.unclaimed_sum += proof.traffic_unclaimed;
  s.by_place_sum += proof.traffic_by_place;
  s.shown_max = std::max(s.shown_max, proof.traffic_shown);
  s.unclaimed_max = std::max(s.unclaimed_max, proof.traffic_unclaimed);
  if (placed != 0) {
    ++s.placed_frames;
    s.placed_sum += placed;
    s.instances_sum += instances;
    s.placed_max = std::max(s.placed_max, placed);
  }
  s.paint_cars_sum += paint_cars;  // RT-PARKED-PAINT: the placed cars that carry the traffic paint's bit
  s.paint_cars_max = std::max(s.paint_cars_max, paint_cars);
  if (parked_log_frame_ == 0) {
    parked_log_frame_ = frame_;
  }
  if (frame_ >= parked_log_frame_ + 300) {
    const double n = double(std::max<uint64_t>(s.frames, 1));
    const double placed_n = double(std::max<uint64_t>(s.placed_frames, 1));
    REXLOG_INFO("{} parked cars (RT-PARKED-CARS), frames {}..{}: traffic cars the main view shows {:.2f} a frame (at most {}), of them claimed by a draw {:.2f} (of those {:.2f} by the draw path's car at their place) and UNCLAIMED "
                "{:.2f} (at most {}); unclaimed cars placed in the top level in {} frames ({:.2f} a frame, at most {}, {:.1f} instances a frame); mcla_gpu_car_rt_cars_parked {}",
                kPrefix, parked_log_frame_, frame_, double(s.shown_sum) / n, s.shown_max, double(s.shown_sum - s.unclaimed_sum) / n, double(s.by_place_sum) / n, double(s.unclaimed_sum) / n, s.unclaimed_max,
                s.placed_frames, double(s.placed_sum) / placed_n, s.placed_max, double(s.instances_sum) / placed_n, larecomp::settings::mcla_gpu_car_rt_cars_parked.Get() ? "on" : "off");
    REXLOG_INFO("{} parked paint (RT-PARKED-PAINT), frames {}..{}: placed parked cars with the traffic paint's bit {:.2f} a frame (at most {}), of {:.2f} placed; their pixels are marked in the history and traced by the opponents' pass "
                "within the traffic's share; mcla_gpu_car_rt_parked_paint {}",
                kPrefix, parked_log_frame_, frame_, double(s.paint_cars_sum) / n, s.paint_cars_max, double(s.placed_sum) / n, larecomp::settings::mcla_gpu_car_rt_parked_paint.Get() ? "on" : "off");
    parked_stats_ = ParkedStats{};
    parked_log_frame_ = frame_;
  }
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
  walked_keys_.erase(key);
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
  walked_keys_.clear();
  walked_set_.Clear();
  walked_present_.clear();
  parked_seen_.clear();
  resident_bytes_ = 0;
  resident_triangles_ = 0;
  for (Shared* shared : {&positions_, &normals_, &uvs_, &indices_}) {
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
  if (slot.materials && slot.materials_cpu) {
    slot.materials->unmap(0, nullptr);
  }
  frames_->Retire(std::move(slot.tlas));
  frames_->Retire(std::move(slot.tlas_buffer));
  frames_->Retire(std::move(slot.scratch));
  frames_->Retire(std::move(slot.instances));
  frames_->Retire(std::move(slot.table));
  frames_->Retire(std::move(slot.materials));
  slot.tlas_bytes = slot.scratch_bytes = slot.instances_bytes = slot.table_bytes = slot.materials_bytes = 0;
  slot.instances_cpu = nullptr;
  slot.table_cpu = nullptr;
  slot.materials_cpu = nullptr;
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
  shared.copied_bytes = 0;  // a barrier was recorded: it ordered the growth's copy too
}

void CarRtScene::OrderCopyInto(plume::RenderCommandList* list, Shared& shared, uint64_t offset, uint64_t bytes) {
  if (!shared.buffer || shared.access != Access::kCopyWrite || !rr::CopyNeedsBarrier(shared.copied_bytes, offset, bytes)) {
    return;
  }
  // The buffer is in the copy-write state already (SetAccess records nothing then): this barrier orders the new mesh's copy after the growth's.
  // Plume's Vulkan barrier is a transfer-to-transfer memory dependency whatever the state before; its D3D12 one records nothing for a buffer
  // that stays in COPY_DEST.
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(shared.buffer.get(), plume::RenderBufferAccess::WRITE));
  shared.copied_bytes = 0;
}

bool CarRtScene::GrowVertices(plume::RenderCommandList* list, uint64_t needed_vertices) {
  const uint64_t limit = rr::kMemoryCapBytes / 28;  // positions, normals and texture coordinates
  if (needed_vertices > limit) {
    return false;
  }
  const uint64_t capacity = vertex_ranges_.capacity();
  if (needed_vertices <= capacity && positions_.buffer) {
    return true;
  }
  const uint64_t grown = rr::GrownCapacity(capacity, needed_vertices, kVertexStep, limit);
  Shared positions, normals, uvs;
  if (!MakeShared(positions, grown * 12, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT, "car rt positions") ||
      !MakeShared(normals, grown * 12, plume::RenderBufferFlag::STORAGE, "car rt normals") ||
      !MakeShared(uvs, grown * 4, plume::RenderBufferFlag::STORAGE, "car rt texture coordinates")) {
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
    if (uvs_.buffer) {
      SetAccess(list, uvs_, Access::kCopyRead);
      SetAccess(list, uvs, Access::kCopyWrite);
      list->copyBufferRegion(plume::RenderBufferReference(uvs.buffer.get(), 0), plume::RenderBufferReference(uvs_.buffer.get(), 0),
                             vertex_ranges_.high_water() * 4);
      uvs.copied_bytes = vertex_ranges_.high_water() * 4;
    }
    // The copies wrote everything below the high water, the free holes too: a mesh placed into one later in this list is ordered after them
    // (OrderCopyInto).
    positions.copied_bytes = used_bytes;
    normals.copied_bytes = used_bytes;
  }
  if (positions_.buffer) {
    frames_->Retire(std::move(positions_.buffer));
  }
  if (normals_.buffer) {
    frames_->Retire(std::move(normals_.buffer));
  }
  if (uvs_.buffer) {
    frames_->Retire(std::move(uvs_.buffer));
  }
  positions_ = std::move(positions);
  normals_ = std::move(normals);
  uvs_ = std::move(uvs);
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
    indices.copied_bytes = used_bytes;  // as for the vertices
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
  if (build_plan_.size() != staged_order_.size()) {
    // (OnResolve made the plan: this is for a caller that did not)
    std::vector<car_model_rt::BuildItem> items;
    for (const uint64_t key : staged_order_) {
      items.push_back(car_model_rt::BuildItem{staged_.at(key).triangles, staged_.at(key).walked});
    }
    build_plan_ = car_model_rt::PlanBuilds(items, rr::kBuildTrianglesPerFrame, car_frame_->other_limits().stage_triangles);
  }
  const size_t take = staged_order_.size();
  // One growth for the whole batch, not one for each mesh (the first frame holds the car's 195 draws).
  uint64_t total_vertices = 0, total_indices = 0;
  for (size_t i = 0; i < take; ++i) {
    if (!build_plan_[i]) {
      continue;
    }
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
    if (!build_plan_[i]) {
      continue;  // over the frame's (or the walked meshes') build budget: the next frame
    }
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
    const bool has_uv = staged.uvs.size() == vertex_count && vertex_count != 0 && uvs_.buffer;
    const uint64_t uv_bytes = has_uv ? uint64_t(vertex_count) * 4 : 0;
    const UploadSpan span = upload_->Allocate(slot, position_bytes * 2 + index_bytes + uv_bytes, 16);
    if (!span) {
      vertex_ranges_.Free(first_vertex, vertex_count);
      index_ranges_.Free(first_index, index_count);
      upload_failed = true;  // the rest next frame
      continue;
    }
    std::memcpy(span.cpu, staged.positions.data(), position_bytes);
    std::memcpy(span.cpu + position_bytes, staged.normals.data(), position_bytes);
    std::memcpy(span.cpu + position_bytes * 2, staged.indices.data(), index_bytes);
    if (has_uv) {
      std::memcpy(span.cpu + position_bytes * 2 + index_bytes, staged.uvs.data(), uv_bytes);
    }
    SetAccess(list, positions_, Access::kCopyWrite);
    SetAccess(list, normals_, Access::kCopyWrite);
    SetAccess(list, indices_, Access::kCopyWrite);
    if (has_uv) {
      SetAccess(list, uvs_, Access::kCopyWrite);
    }
    // First fit hands a new mesh the holes below the high water that a growth earlier in this list has copied the old bytes into: the three
    // copies below come after a barrier then (the validation layer's WRITE_AFTER_WRITE on the 0.11 line's model walk, which gives meshes back
    // and places hundreds in a frame).
    OrderCopyInto(list, positions_, uint64_t(first_vertex) * 12, position_bytes);
    OrderCopyInto(list, normals_, uint64_t(first_vertex) * 12, position_bytes);
    OrderCopyInto(list, indices_, uint64_t(first_index) * 4, index_bytes);
    if (has_uv) {
      OrderCopyInto(list, uvs_, uint64_t(first_vertex) * 4, uv_bytes);
    }
    list->copyBufferRegion(plume::RenderBufferReference(positions_.buffer.get(), uint64_t(first_vertex) * 12),
                           plume::RenderBufferReference(span.buffer, span.offset), position_bytes);
    list->copyBufferRegion(plume::RenderBufferReference(normals_.buffer.get(), uint64_t(first_vertex) * 12),
                           plume::RenderBufferReference(span.buffer, span.offset + position_bytes), position_bytes);
    list->copyBufferRegion(plume::RenderBufferReference(indices_.buffer.get(), uint64_t(first_index) * 4),
                           plume::RenderBufferReference(span.buffer, span.offset + position_bytes * 2), index_bytes);
    if (has_uv) {
      list->copyBufferRegion(plume::RenderBufferReference(uvs_.buffer.get(), uint64_t(first_vertex) * 4),
                             plume::RenderBufferReference(span.buffer, span.offset + position_bytes * 2 + index_bytes), uv_bytes);
    }
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
    p.mesh.has_uv = has_uv;
    p.mesh.uv_density = has_uv ? staged.uv_density : 0.0f;
    p.mesh.patch = staged.patch;
    placed->push_back(std::move(p));
    done.push_back(key);
  }
  for (const uint64_t key : done) {
    staged_.erase(key);
  }
  std::erase_if(staged_order_, [&](uint64_t key) { return !staged_.contains(key); });
}

bool CarRtScene::EnsureSlot(SlotResources& s, uint32_t index, uint64_t tlas_bytes, uint64_t scratch_bytes, uint64_t instance_bytes,
                            uint64_t table_bytes, uint64_t material_bytes) {
  const auto room = [](uint64_t bytes) { return AlignUp(bytes + bytes / 4, 4096); };
  if (tlas_bytes != 0 && s.tlas_bytes < tlas_bytes) {
    // The top level sits at offset 0 of a buffer of its own: Plume's D3D12 descriptor for an acceleration structure used the buffer's base
    // address and ignored the structure's offset until mzzvxm/plume#13 (3c81454, in the pin since 502978a9). Kept at offset 0 all the same, so
    // nothing here depends on it: on Vulkan the descriptor names the structure itself, so the offset never mattered there.

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
         upload_buffer(s.table, s.table_cpu, s.table_bytes, table_bytes, plume::RenderBufferFlag::STORAGE, "table") &&
         upload_buffer(s.materials, s.materials_cpu, s.materials_bytes, material_bytes, plume::RenderBufferFlag::STORAGE, "materials");
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

void CarRtScene::OnAnchor(const ConstantBanks::Banks& banks, bool cut) {
  anchor_seen_ = true;
  if (service_) {
    service_->NoteAnchor(banks, cut);
  }
}

void CarRtScene::OnPresent() {
  if (ready_) {
    if (larecomp::settings::mcla_gpu_car_rt_check.Get()) {
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
  if (service_) {
    service_->EndFrame();  // the wants of the frame become the last frame's; the city and the pool are told the level for the next one
  }
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
  const int view_mode = larecomp::settings::mcla_gpu_car_rt_view.Get();
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
  push.slots[3] = (want_view ? 1u : 0u) | (uint32_t(view_mode) << 8) | (std::min(view_.car_count, 0xFFu) << 16);
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
  push.scope[3] = view_.car_row;
  scope.car_count = view_.car_count;
  scope.records = view_.records;  // what the host wrote for each car this frame: the probe's words are compared with it when the readback comes
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
  // THE PAINT PROBE (RT-MODEL-CARS-4): the first cars' records as the shader's own loads read them from the table, against what the host wrote there that frame. This is the proof
  // that the paint the reflection pass hands the paint reaches the shader unchanged (the units and the mapping are the pass's and the rules'; the log's paint rows say what they are).
  if (scope.car_count != 0) {
    std::string cars_text;
    uint32_t checked = 0, same = 0;
    for (uint32_t k = 0; k < rr::kProbeCars && k < scope.car_count && k < scope.records.size(); ++k) {
      const rr::ProbeCar p = rr::DecodeProbe(words, k, scope.car_count);
      const car_rt_instances::CarRecordLog& w = scope.records[k];
      const bool match = rr::ProbeMatches(p, w.centre, w.radius, w.has_paint, w.paint);
      ++checked;
      same += match ? 1u : 0u;
      cars_text += std::format("{}slot {} (car {}{}{}, {:.1f} m): the shader read paint {} ({:.4f}, {:.4f}, {:.4f}), radius {:.2f}, centre ({:.2f}, {:.2f}, {:.2f}); the host wrote paint {} ({:.4f}, {:.4f}, {:.4f}), radius {:.2f} -> {}",
                               cars_text.empty() ? "" : "; ", w.slot, w.stencil_id, w.remembered ? ", not drawn" : "", w.walked ? ", built from model data" : "", w.distance,
                               (p.flags & 1u) != 0 ? "known" : "none", p.paint[0], p.paint[1], p.paint[2], p.radius, p.centre[0], p.centre[1], p.centre[2],
                               w.has_paint ? "known" : "none", w.paint[0], w.paint[1], w.paint[2], w.radius, match ? "IDENTICAL" : "DIFFERENT");
    }
    REXLOG_INFO("{} paint probe of frame {}: {} of the first {} cars' records (of {}) read by the shader are the bits the host wrote [{}]", kPrefix, frame, same, checked, scope.car_count,
                cars_text);
  }
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
