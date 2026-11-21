// MCLA GPU renderer -- the city as ray tracing acceleration structures. See city_rt_scene.h.

#include "city_rt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "car_rt_scene_rules.h"
#include "citypack/pack_format.h"
#include "device.h"
#include "pass_timing.h"
#include "upload.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[city rt]";
namespace rr = car_rt_rules;

uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// A mesh's bytes in its input buffer: the positions (float3) and the indices (uint32), each at 16.
uint64_t PositionBytes(const citypack::MeshData& m) { return AlignUp(uint64_t(m.vertex_count()) * 12, 16); }
uint64_t IndexBytes(const citypack::MeshData& m) { return AlignUp(uint64_t(m.indices.size()) * 4, 16); }

}  // namespace

CityRtScene::~CityRtScene() { StopWorker(); }

bool CityRtScene::Initialize(Device& device, FrameRing& frames, UploadRing& upload, PassTiming& pass_timing, const std::filesystem::path& pack_file,
                             gpu_class::GpuClass cls) {
  device_ = &device;
  frames_ = &frames;
  upload_ = &upload;
  pass_timing_ = &pass_timing;
  pack_file_ = pack_file;
  class_ = cls;
  return true;
}

void CityRtScene::Shutdown() {
  StopWorker();
  ReleaseAll("shutdown");
  for (SlotResources& slot : slots_) {
    slot.scratch.reset();
    slot.scratch_bytes = 0;
  }
  built_.clear();
  states_.clear();
  sectors_.clear();
  staged_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  upload_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pack and the worker
// ---------------------------------------------------------------------------

bool CityRtScene::OpenPack() {
  if (pack_.is_open()) {
    return true;
  }
  if (pack_probe_frame_ != 0 && frame_ < pack_probe_frame_ + kReprobeFrames) {
    return false;
  }
  pack_probe_frame_ = frame_ == 0 ? 1 : frame_;
  std::string error;
  if (!pack_.Open(pack_file_, &error)) {
    if (!pack_missing_logged_) {
      pack_missing_logged_ = true;
      REXLOG_INFO("{} the city pack is not there or cannot be read ({}): ray traced city shadows are off until it is built (the launcher's setup builds it from the game files; "
                  "tools/citypack_tool build does it by hand); looked for {}",
                  kPrefix, error, pack_file_.string());
    }
    return false;
  }
  const uint32_t count = pack_.sector_count();
  sectors_.assign(count, city_rt::SectorInfo{});
  states_.assign(count, city_rt::SectorState{});
  built_.clear();
  built_.resize(count);
  uint64_t hi_triangles = 0, lod_triangles = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const citypack::SectorRecord& r = pack_.sector(i);
    city_rt::SectorInfo& s = sectors_[i];
    std::memcpy(s.box_min, r.aabb_min, sizeof(s.box_min));
    std::memcpy(s.box_max, r.aabb_max, sizeof(s.box_max));
    s.hi_triangles = r.has_hi() ? r.hi_triangles : 0;
    s.lod_triangles = r.has_lod() ? r.lod_triangles : 0;
    hi_triangles += s.hi_triangles;
    lod_triangles += s.lod_triangles;
  }
  counts_.sectors = count;
  REXLOG_INFO("{} the city pack is open: {} sectors, {} hi detail triangles and {} lod triangles, {:.0f} MiB on disk, builder version {}, classes left out: mask {:#x}", kPrefix, count,
              hi_triangles, lod_triangles, double(pack_.header().file_size) / (1024.0 * 1024.0), pack_.header().builder_version, pack_.header().drop_mask);
  StartWorker();
  return true;
}

void CityRtScene::StartWorker() {
  if (worker_.joinable()) {
    return;
  }
  stop_.store(false);
  worker_ = std::thread([this] { WorkerMain(); });
}

void CityRtScene::StopWorker() {
  if (!worker_.joinable()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    stop_.store(true);
  }
  wake_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  requests_.clear();
  results_.clear();
  reads_in_flight_.store(0);
}

void CityRtScene::WorkerMain() {
  for (;;) {
    LoadRequest request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_.load() || !requests_.empty(); });
      if (stop_.load()) {
        return;
      }
      request = requests_.front();
      requests_.pop_front();
    }
    LoadResult result;
    result.request = request;
    result.ok = pack_.ReadMesh(request.sector, request.detail == city_rt::Detail::kHi ? citypack::MeshKind::kHi : citypack::MeshKind::kLod, &result.mesh, &result.error);
    if (result.ok && result.mesh.empty()) {
      result.ok = false;
      result.error = "an empty mesh";
    }
    {
      std::lock_guard lock(mutex_);
      results_.push_back(std::move(result));
    }
  }
}

void CityRtScene::PollLoads() {
  std::deque<LoadResult> done;
  {
    std::lock_guard lock(mutex_);
    done.swap(results_);
  }
  for (LoadResult& r : done) {
    reads_in_flight_.fetch_sub(1);
    city_rt::SectorState& st = states_[r.request.sector];
    city_rt::Residency& residency = r.request.detail == city_rt::Detail::kHi ? st.hi : st.lod;
    uint64_t& retry = r.request.detail == city_rt::Detail::kHi ? st.hi_retry_frame : st.lod_retry_frame;
    if (residency != city_rt::Residency::kLoading) {
      continue;  // given back while the read was on its way (the setting went off, the camera moved on): the mesh is not wanted
    }
    if (!r.ok) {
      residency = city_rt::Residency::kFailed;
      retry = frame_ + kRetryFrames;
      ++counts_.failures;
      REXLOG_WARN("{} sector {} ({}) {}: {}", kPrefix, r.request.sector, pack_.name(r.request.sector), r.request.detail == city_rt::Detail::kHi ? "hi detail" : "lod", r.error);
      continue;
    }
    Staged staged;
    staged.sector = r.request.sector;
    staged.detail = r.request.detail;
    staged.mesh = std::move(r.mesh);
    staged_bytes_ += PositionBytes(staged.mesh) + IndexBytes(staged.mesh);
    staged_.push_back(std::move(staged));
  }
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CityRtScene::BeginFrame(city_rt::Level level, float radius_override) {
  ++frame_;
  recorded_ = false;
  instances_this_frame_ = 0;
  level_ = device_ ? level : city_rt::Level::kOff;
  budget_ = city_rt::WithRadius(city_rt::BudgetFor(class_, level_), radius_override);
  if (level_ != city_rt::Level::kOff) {
    last_wanted_frame_ = frame_;
    return;
  }
  // The structures go back after a while without a request (the setting was switched off, or the pass is gated).
  if (!built_.empty() && frame_ > last_wanted_frame_ + kIdleReleaseFrames) {
    ReleaseAll("no request for a while");
  }
}

void CityRtScene::Release(uint32_t sector, city_rt::Detail detail) {
  SectorSlot& slot = built_[sector];
  Structure& s = detail == city_rt::Detail::kHi ? slot.hi : slot.lod;
  city_rt::SectorState& st = states_[sector];
  if (s.structure || s.buffer) {
    structure_bytes_ -= std::min(structure_bytes_, s.bytes);
    counts_.triangles -= std::min<uint64_t>(counts_.triangles, s.triangles);
    // an instance of the frame being recorded may name it: it goes back after the GPU is done with the frames in flight
    if (s.structure) frames_->Retire(std::move(s.structure));
    if (s.buffer) frames_->Retire(std::move(s.buffer));
    ++counts_.evictions;
  }
  s = Structure{};
  (detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kNone;
  (detail == city_rt::Detail::kHi ? st.hi_bytes : st.lod_bytes) = 0;
}

void CityRtScene::ReleaseAll(const char* why) {
  if (built_.empty()) {
    return;
  }
  uint32_t released = 0;
  if (device_) {
    for (uint32_t i = 0; i < built_.size(); ++i) {
      for (const city_rt::Detail d : {city_rt::Detail::kHi, city_rt::Detail::kLod}) {
        const Structure& s = d == city_rt::Detail::kHi ? built_[i].hi : built_[i].lod;
        if (s.structure || s.buffer) {
          Release(i, d);
          ++released;
        }
      }
    }
  }
  // meshes read and waiting, and the states of reads in flight, go with them (a read still in the worker's queue is dropped when it arrives: its state is kNone)
  staged_.clear();
  staged_bytes_ = 0;
  for (city_rt::SectorState& st : states_) {
    st = city_rt::SectorState{};
  }
  if (released != 0) {
    REXLOG_INFO("{} {} structures given back ({})", kPrefix, released, why);
  }
}

void CityRtScene::Plan(const double camera[3]) {
  city_rt::PlanInput in;
  std::memcpy(in.camera, camera, sizeof(in.camera));
  in.frame = frame_;
  in.budget = budget_;
  in.loading = reads_in_flight_.load();
  const city_rt::Plan plan = city_rt::PlanStreaming(in, sectors_, states_);
  for (const city_rt::Request& e : plan.evictions) {
    Release(e.sector, e.detail);
    // a mesh of that sector waiting for its build is no longer wanted either
    std::erase_if(staged_, [&](const Staged& s) {
      if (s.sector == e.sector && s.detail == e.detail) {
        staged_bytes_ -= std::min(staged_bytes_, PositionBytes(s.mesh) + IndexBytes(s.mesh));
        return true;
      }
      return false;
    });
  }
  if (!plan.loads.empty()) {
    {
      std::lock_guard lock(mutex_);
      for (const city_rt::Request& r : plan.loads) {
        city_rt::SectorState& st = states_[r.sector];
        (r.detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kLoading;
        (r.detail == city_rt::Detail::kHi ? st.hi_bytes : st.lod_bytes) =
            city_rt::EstimateStructureBytes(r.detail == city_rt::Detail::kHi ? sectors_[r.sector].hi_triangles : sectors_[r.sector].lod_triangles);
        requests_.push_back({r.sector, r.detail});
      }
    }
    reads_in_flight_.fetch_add(uint32_t(plan.loads.size()));
    wake_.notify_all();
  }
}

bool CityRtScene::EnsureScratch(SlotResources& slot, uint32_t index, uint64_t bytes) {
  if (bytes <= slot.scratch_bytes) {
    return true;
  }
  const uint64_t room = AlignUp(bytes + bytes / 4, 1 << 16);
  const std::string name = std::format("city rt scratch {}", index);
  std::unique_ptr<plume::RenderBuffer> buffer = device_->CreateBuffer(
      plume::RenderBufferDesc::DefaultBuffer(room, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH | plume::RenderBufferFlag::UNORDERED_ACCESS), name.c_str());
  if (!buffer) {
    return false;
  }
  frames_->Retire(std::move(slot.scratch));
  slot.scratch = std::move(buffer);
  slot.scratch_bytes = room;
  return true;
}

bool CityRtScene::BuildStaged(plume::RenderCommandList* list, uint32_t slot_index) {
  if (staged_.empty()) {
    return false;
  }
  // The frame's schedule: nearest first within the class's triangles and staged bytes.
  std::vector<city_rt::BuildItem> items;
  items.reserve(staged_.size());
  for (Staged& s : staged_) {
    const citypack::SectorRecord& rec = pack_.sector(s.sector);
    s.distance = city_rt::DistanceToBox(camera_, rec.aabb_min, rec.aabb_max);
    items.push_back({s.mesh.triangle_count(), PositionBytes(s.mesh) + IndexBytes(s.mesh), s.distance});
  }
  const std::vector<uint8_t> take = city_rt::PlanBuilds(items, budget_.build_triangles, budget_.upload_bytes);

  struct Job {
    size_t staged_index;
    std::unique_ptr<plume::RenderBuffer> input;
    uint64_t index_offset = 0;
    plume::RenderBottomLevelASBuildInfo info;
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::unique_ptr<plume::RenderAccelerationStructure> structure;
    uint64_t size = 0;
  };
  std::vector<Job> jobs;
  uint64_t scratch_needed = 0;
  for (size_t i = 0; i < staged_.size(); ++i) {
    if (!take[i]) {
      continue;
    }
    Staged& s = staged_[i];
    const citypack::MeshData& m = s.mesh;
    const uint64_t position_bytes = PositionBytes(m);
    const uint64_t index_bytes = IndexBytes(m);
    const UploadSpan span = upload_->Allocate(slot_index, position_bytes + index_bytes, 16);
    if (!span) {
      break;  // no staging room this frame: the rest next frame
    }
    Job job;
    job.staged_index = i;
    job.input = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(position_bytes + index_bytes, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT),
                                      std::format("city rt mesh {} {}", s.sector, s.detail == city_rt::Detail::kHi ? "hi" : "lod").c_str());
    if (!job.input) {
      break;
    }
    std::memcpy(span.cpu, m.positions.data(), size_t(m.vertex_count()) * 12);
    std::memcpy(span.cpu + position_bytes, m.indices.data(), m.indices.size() * 4);
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(job.input.get(), plume::RenderBufferAccess::WRITE));
    list->copyBufferRegion(plume::RenderBufferReference(job.input.get(), 0), span.reference(), position_bytes + index_bytes);
    job.index_offset = position_bytes;

    // The geometries: the opaque triangles (one geometry flagged opaque), then the alpha tested ones (not opaque: a query meets them as candidates).
    plume::RenderBottomLevelASMesh geometry[2];
    uint32_t geometries = 0;
    const uint32_t opaque = m.opaque_triangles;
    const uint32_t alpha = m.alpha_triangles();
    if (opaque != 0) {
      geometry[geometries++] = plume::RenderBottomLevelASMesh(plume::RenderBufferReference(job.input.get(), job.index_offset), plume::RenderBufferReference(job.input.get(), 0),
                                                              plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32B32_FLOAT, opaque * 3, m.vertex_count(), 12, /*isOpaque=*/true);
    }
    if (alpha != 0) {
      geometry[geometries++] = plume::RenderBottomLevelASMesh(plume::RenderBufferReference(job.input.get(), job.index_offset + uint64_t(opaque) * 12),
                                                              plume::RenderBufferReference(job.input.get(), 0), plume::RenderFormat::R32_UINT,
                                                              plume::RenderFormat::R32G32B32_FLOAT, alpha * 3, m.vertex_count(), 12, /*isOpaque=*/false);
    }
    device_->device()->setBottomLevelASBuildInfo(job.info, geometry, geometries, /*preferFastBuild=*/false, /*preferFastTrace=*/true);
    job.size = AlignUp(job.info.accelerationStructureSize, rr::kStructureAlignment);
    if (job.size != 0) {
      job.buffer = device_->CreateBuffer(plume::RenderBufferDesc::AccelerationStructureBuffer(job.size), std::format("city rt structure {}", s.sector).c_str());
      if (job.buffer) {
        job.structure = device_->device()->createAccelerationStructure(
            plume::RenderAccelerationStructureDesc(plume::RenderAccelerationStructureType::BOTTOM_LEVEL, plume::RenderBufferReference(job.buffer.get(), 0), job.size));
      }
    }
    if (!job.structure) {
      // No room (or no object): the mesh is tried again after a while.
      city_rt::SectorState& st = states_[s.sector];
      (s.detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kFailed;
      (s.detail == city_rt::Detail::kHi ? st.hi_retry_frame : st.lod_retry_frame) = frame_ + kRetryFrames;
      ++counts_.failures;
      frames_->Retire(std::move(job.input));
      job.staged_index = size_t(-1);
      jobs.push_back(std::move(job));
      continue;
    }
    scratch_needed = std::max(scratch_needed, job.info.scratchSize);
    jobs.push_back(std::move(job));
  }
  if (jobs.empty()) {
    return false;
  }
  SlotResources& res = slots_[slot_index];
  if (!EnsureScratch(res, slot_index, std::max<uint64_t>(scratch_needed, 256))) {
    REXLOG_ERROR("{} the builds' scratch ({:.1f} MiB) could not be made: the meshes of this frame wait", kPrefix, double(scratch_needed) / (1024.0 * 1024.0));
    for (Job& j : jobs) {
      if (j.input) frames_->Retire(std::move(j.input));
    }
    return true;  // the copies were recorded; nothing was built
  }

  // The inputs were copied into: the builds read them.
  std::vector<plume::RenderBufferBarrier> inputs;
  for (const Job& j : jobs) {
    if (j.input && j.structure) {
      inputs.emplace_back(j.input.get(), plume::RenderBufferAccess::READ);
    }
  }
  if (!inputs.empty()) {
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), inputs.data(), uint32_t(inputs.size()), nullptr, 0);
  }
  std::vector<plume::RenderBufferBarrier> built;
  std::vector<uint32_t> done_staged;
  for (Job& j : jobs) {
    if (!j.structure || j.staged_index == size_t(-1)) {
      continue;
    }
    Staged& s = staged_[j.staged_index];
    list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(res.scratch.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
    list->buildBottomLevelAS(j.structure.get(), plume::RenderBufferReference(res.scratch.get(), 0), j.info);
    built.emplace_back(j.buffer.get(), plume::RenderBufferAccess::READ);
    Structure& target = s.detail == city_rt::Detail::kHi ? built_[s.sector].hi : built_[s.sector].lod;
    target.buffer = std::move(j.buffer);
    target.structure = std::move(j.structure);
    target.bytes = j.size;
    target.triangles = s.mesh.triangle_count();
    structure_bytes_ += j.size;
    counts_.triangles += target.triangles;
    ++counts_.builds;
    counts_.build_triangles += target.triangles;
    city_rt::SectorState& st = states_[s.sector];
    (s.detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kResident;
    (s.detail == city_rt::Detail::kHi ? st.hi_bytes : st.lod_bytes) = j.size;
    done_staged.push_back(uint32_t(j.staged_index));
    frames_->Retire(std::move(j.input));  // the structure needs none of it: the GPU is done with it when this frame is
  }
  // The structures are read by the top level's build that follows in this list.
  if (!built.empty()) {
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), built.data(), uint32_t(built.size()), nullptr, 0);
    if (!first_build_logged_) {
      first_build_logged_ = true;
      REXLOG_INFO("{} the first city structures are built (frame {}): {} of them, {} triangles", kPrefix, frame_, built.size(), counts_.build_triangles);
    }
  }
  // The staged meshes that were built are gone; those that failed or were given back go too; the rest (still loading: not scheduled this frame) wait.
  std::sort(done_staged.begin(), done_staged.end());
  std::vector<Staged> remaining;
  remaining.reserve(staged_.size());
  for (size_t i = 0; i < staged_.size(); ++i) {
    const city_rt::SectorState& st = states_[staged_[i].sector];
    const city_rt::Residency state = staged_[i].detail == city_rt::Detail::kHi ? st.hi : st.lod;
    const bool taken = std::binary_search(done_staged.begin(), done_staged.end(), uint32_t(i));
    if (!taken && state == city_rt::Residency::kLoading) {
      remaining.push_back(std::move(staged_[i]));
    } else {
      staged_bytes_ -= std::min(staged_bytes_, PositionBytes(staged_[i].mesh) + IndexBytes(staged_[i].mesh));
    }
  }
  staged_ = std::move(remaining);
  return true;
}

uint32_t CityRtScene::Prepare(const temporal::Mat4& view, bool flip_z, plume::RenderCommandList* list, uint32_t slot, std::vector<plume::RenderTopLevelASInstance>* instances) {
  recorded_ = false;
  instances_this_frame_ = 0;
  if (!device_ || level_ == city_rt::Level::kOff || !budget_.on() || !list || slot >= kFramesInFlight) {
    return 0;
  }
  if (!OpenPack()) {
    return 0;
  }
  // The camera's place in the world: the view matrix is world to view (row vectors), so the origin of the view space is row 3 of its inverse.
  temporal::Mat4 inverse;
  if (!temporal::Inverse(view, &inverse)) {
    return 0;
  }
  camera_[0] = inverse.m[3][0];
  camera_[1] = inverse.m[3][1];
  camera_[2] = inverse.m[3][2];
  if (!std::isfinite(camera_[0]) || !std::isfinite(camera_[1]) || !std::isfinite(camera_[2])) {
    return 0;
  }
  camera_known_ = true;

  pass_timing_->Mark(list, "city rt build");
  PollLoads();
  Plan(camera_);
  recorded_ = BuildStaged(list, slot);

  // The instances: one transform, the cars' own (a mesh in world space, identity model matrix).
  float transform[3][4];
  if (!rr::PackInstanceTransform(temporal::Identity(), view, flip_z, transform)) {
    return 0;
  }
  uint32_t added = 0;
  for (uint32_t i = 0; i < built_.size(); ++i) {
    city_rt::Detail detail = city_rt::Detail::kHi;
    if (!city_rt::PickInstance(states_[i], &detail)) {
      continue;
    }
    const Structure& s = detail == city_rt::Detail::kHi ? built_[i].hi : built_[i].lod;
    if (!s.structure || !s.buffer) {
      continue;
    }
    plume::RenderTopLevelASInstance instance(plume::RenderBufferReference(s.buffer.get(), 0), city_rt::InstanceId(i, detail), city_rt::kMaskCity, 0, /*cullDisable=*/true,
                                             plume::RenderAffineTransform());
    std::memcpy(instance.transform.m, transform, sizeof(transform));
    instances->push_back(instance);
    ++added;
  }
  instances_this_frame_ = added;
  LogState("");
  return added;
}

float CityRtScene::SafeRange(float game_hi_radius) const {
  if (!camera_known_ || !budget_.on() || states_.empty()) {
    return 0.0f;
  }
  city_rt::PlanInput in;
  std::memcpy(in.camera, camera_, sizeof(in.camera));
  in.budget = budget_;
  return city_rt::SafeReceiverRange(in, sectors_, states_, game_hi_radius);
}

CityRtScene::Stats CityRtScene::stats() const {
  Stats s = counts_;
  s.instances = instances_this_frame_;
  s.structure_bytes = structure_bytes_;
  s.loading = reads_in_flight_.load() + uint32_t(staged_.size());
  s.hi_resident = 0;
  s.lod_resident = 0;
  for (const city_rt::SectorState& st : states_) {
    s.hi_resident += st.hi == city_rt::Residency::kResident ? 1u : 0u;
    s.lod_resident += st.lod == city_rt::Residency::kResident ? 1u : 0u;
  }
  return s;
}

uint64_t CityRtScene::HeldBytes() const {
  uint64_t scratch = 0;
  for (const SlotResources& slot : slots_) {
    scratch += slot.scratch_bytes;
  }
  return structure_bytes_ + scratch + staged_bytes_;
}

std::string CityRtScene::CensusText() const {
  if (!pack_.is_open()) {
    return {};
  }
  const Stats s = stats();
  return std::format("; city ray tracing: {} hi and {} lod sectors, {:.1f} MiB, {} builds, {} evictions, {} failures", s.hi_resident, s.lod_resident,
                     double(s.structure_bytes) / (1024.0 * 1024.0), s.builds, s.evictions, s.failures);
}

void CityRtScene::LogState(const char* why) {
  const Stats s = stats();
  // A line when the resident sets change by a few sectors, and one every ten seconds while anything moves.
  const uint64_t signature = (uint64_t(s.hi_resident) << 40) | (uint64_t(s.lod_resident) << 20) | uint64_t(s.loading);
  if (signature == logged_signature_ && frame_ < log_frame_ + 3600) {
    return;
  }
  if (log_frame_ != 0 && frame_ < log_frame_ + 300 && signature != logged_signature_) {
    return;  // not more often than every five seconds
  }
  logged_signature_ = signature;
  log_frame_ = frame_;
  REXLOG_INFO("{} frame {}: {} hi detail and {} lod sectors built ({:.1f} MiB of structures, {} triangles; the class's cap {:.0f} MiB, rings {:.0f} m hi and {:.0f} m lod), {} reading or "
              "waiting, {} instances in the top level, safe range {:.0f} m; {} builds ({} triangles), {} given back, {} failures so far {}",
              kPrefix, frame_, s.hi_resident, s.lod_resident, double(s.structure_bytes) / (1024.0 * 1024.0), s.triangles, double(budget_.memory_cap) / (1024.0 * 1024.0),
              budget_.hi_radius, budget_.far_radius, s.loading, s.instances, SafeRange(1.0e9f), s.builds, s.build_triangles, s.evictions, s.failures, why);
}

}  // namespace mcla::gpu
