// MCLA GPU renderer -- the city as ray tracing acceleration structures. See city_rt_scene.h.

#include "city_rt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "car_rt_instances_rules.h"
#include "car_rt_scene_rules.h"
#include "citypack/pack_format.h"
#include "city_hit_scene.h"
#include "city_refl_setting.h"
#include "device.h"
#include "pass_timing.h"
#include "rt_tier.h"
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
  city_refl_setting::NoteBase(city_refl_setting::Data::kUnknown);
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
  if (pack_probe_frame_ != 0 && frame_ < pack_probe_frame_ + probe_wait_) {
    return false;
  }
  pack_probe_frame_ = frame_ == 0 ? 1 : frame_;
  if (probe_) {
    // The app's verdict first (stat only): a pack that is missing or built from other files than the player's now is never opened; the game's shadows stand until the app has
    // built the right one (in the background: a minute at most on a slow PC), which the probe then reports as current.
    std::string why;
    const city_rt::PackState state = probe_(&why);
    probe_wait_ = city_rt::ProbeWaitFrames(state, build_requested_);
    // The pause menu's RT CITY REFLECTIONS row says how the pack stands (city_refl_setting.h): a missing or stale pack is "being built" where the app can build it (asked just below).
    city_refl_setting::NoteBase(city_refl_setting::DataOf(state, bool(request_build_)));
    if (!city_rt::CanOpenPack(state)) {
      if (city_rt::WantsBuild(state) && !build_requested_ && request_build_) {
        build_requested_ = true;
        probe_wait_ = city_rt::ProbeWaitFrames(state, true);
        request_build_();
      }
      if (logged_pack_state_ != int(state)) {
        logged_pack_state_ = int(state);
        REXLOG_INFO("{} the city pack is {}{}: ray traced city shadows are off until it is current (the game's shadows stand){}; looked for {}", kPrefix, city_rt::PackStateName(state),
                    why.empty() ? std::string() : " (" + why + ")",
                    city_rt::WantsBuild(state) ? "; the game builds it in the background (launcher setup runs larecomp.exe --build_city_pack, which has it ready at the first start)"
                                               : state == city_rt::PackState::kBuilding ? "; it is being built in the background, checked again in 2 s" : "",
                    pack_file_.string());
      }
      return false;
    }
    logged_pack_state_ = int(state);
  } else {
    probe_wait_ = kReprobeFrames;
  }
  std::string error;
  if (!pack_.Open(pack_file_, &error)) {
    city_refl_setting::NoteBase(city_refl_setting::Data::kUnavailable);
    if (!pack_missing_logged_) {
      pack_missing_logged_ = true;
      if (probe_) {
        REXLOG_WARN("{} the city pack is current by its stamp but cannot be read ({}): ray traced city shadows stay off; delete {} and its stamp to have the game build it again", kPrefix, error,
                    pack_file_.string());
      } else {
        REXLOG_INFO("{} the city pack is not there or cannot be read ({}): ray traced city shadows are off until it is built (the launcher's setup builds it from the game files; "
                    "tools/citypack_tool build does it by hand); looked for {}",
                    kPrefix, error, pack_file_.string());
      }
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
    s.hi_pool = s.has_hi() ? city_hit::EstimateBlockBytes(r.hi_vertices, s.hi_triangles) : 0;  // what the sector's block will take of the hit data's pool (the plan counts it when the pool is there)
    s.lod_pool = s.has_lod() ? city_hit::EstimateBlockBytes(r.lod_vertices, s.lod_triangles) : 0;
    hi_triangles += s.hi_triangles;
    lod_triangles += s.lod_triangles;
  }
  counts_.sectors = count;
  REXLOG_INFO("{} the city pack is open: {} sectors, {} hi detail triangles and {} lod triangles, {:.0f} MiB on disk, builder version {}, classes left out: mask {:#x}", kPrefix, count,
              hi_triangles, lod_triangles, double(pack_.header().file_size) / (1024.0 * 1024.0), pack_.header().builder_version, pack_.header().drop_mask);
  city_refl_setting::NoteBase(city_refl_setting::Data::kReady);
  if (hit_) hit_->SetBase(&pack_);  // the hit file is opened beside the pack, when the hit data is wanted
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
    const citypack::MeshKind kind = request.detail == city_rt::Detail::kHi ? citypack::MeshKind::kHi : citypack::MeshKind::kLod;
    result.ok = pack_.ReadMesh(request.sector, kind, &result.mesh, &result.error);
    if (result.ok && result.mesh.empty()) {
      result.ok = false;
      result.error = "an empty mesh";
    }
    if (result.ok && request.with_hit && hit_ != nullptr) {
      // The hit blob of the same mesh. A blob that is missing or does not fit the mesh leaves the block with geometry only: the mesh itself is good.
      result.has_hit = hit_->ReadBlob(request.sector, kind, &result.hit, &result.hit_error);
      if (result.has_hit && (result.hit.triangle_count != result.mesh.triangle_count() || result.hit.opaque_triangles != result.mesh.opaque_triangles)) {
        result.has_hit = false;
        result.hit_error = "the hit blob's triangles are not the mesh's";
      }
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
    if (city_rt::ReadBeforePool(hit_ != nullptr && hit_->ready(), r.request.with_hit)) {
      GiveBackForReread(r.request.sector, r.request.detail);  // read before the pool existed: not built as geometry only, read again with the blob
      continue;
    }
    if (std::any_of(staged_.begin(), staged_.end(), [&](const Staged& s) { return s.sector == r.request.sector && s.detail == r.request.detail; })) {
      // A second read of a mesh that waits for its build: the states were reset while the first was in the worker's queue (a live HIGH <-> ULTRA switch makes the hit data's pool again and gives every
      // structure back) and the plan asked again. Built twice, the second structure would replace the first in the sector's slot at once, while the list being recorded builds it.
      continue;
    }
    Staged staged;
    staged.sector = r.request.sector;
    staged.detail = r.request.detail;
    staged.mesh = std::move(r.mesh);
    staged.hit_wanted = r.request.with_hit;
    staged.has_hit = r.has_hit;
    if (r.has_hit) {
      staged.hit = std::move(r.hit);
    } else if (r.request.with_hit && !r.hit_error.empty() && !hit_bad_data_logged_) {
      hit_bad_data_logged_ = true;
      REXLOG_WARN("{} sector {} ({}) {}: no usable hit data ({}): its block holds geometry only (logged once)", kPrefix, r.request.sector, pack_.name(r.request.sector),
                  r.request.detail == city_rt::Detail::kHi ? "hi detail" : "lod", r.hit_error);
    }
    staged.bytes = StagedBytesOf(staged);
    staged_bytes_ += staged.bytes;
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
  class_ = gpu_class::CurrentRtClass();  // the budgets follow the ray tracing tier (rt_tier.h: a weak card of the High class is Mid, none is Low), which the benchmark can move after Initialize
  budget_ = city_rt::WithRadius(city_rt::BudgetFor(class_, level_), radius_override);
  if (hit_ != nullptr) {
    budget_ = hit_->Adjust(budget_);  // the pool's cap and staging, when the pool is there
    // Structures built before the pool was made, or under another pool, have no blocks: given back, they are built again with hit data (CityHitScene::epoch). A pool that was given
    // back leaves the structures as they are: their blocks are gone, the shadows do not need them, and a pool made later is a new epoch.
    if (hit_->epoch() != hit_epoch_) {
      hit_epoch_ = hit_->epoch();
      if (hit_->ready() && !built_.empty()) {
        ReleaseAll("the hit data's pool was made");
      }
    }
  }
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
  if (hit_ != nullptr) {
    hit_->Evict(sector, detail == city_rt::Detail::kLod);  // the row is cleared now; the range comes back when the GPU is done with the frames in flight
  }
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
  (detail == city_rt::Detail::kHi ? st.hi_pool_held : st.lod_pool_held) = 0;
}

void CityRtScene::GiveBackForReread(uint32_t sector, city_rt::Detail detail) {
  city_rt::SectorState& st = states_[sector];
  (detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kNone;
  (detail == city_rt::Detail::kHi ? st.hi_bytes : st.lod_bytes) = 0;
  (detail == city_rt::Detail::kHi ? st.hi_pool_held : st.lod_pool_held) = 0;
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

uint64_t CityRtScene::StagedBytesOf(const Staged& s) const {
  if (s.hit_wanted) {
    return citypack::PlanPoolBlock(s.mesh.vertex_count(), s.mesh.triangle_count(), s.has_hit ? uint32_t(s.hit.runs.size()) : 0u, s.has_hit, s.has_hit && s.hit.has_corners()).total_bytes;
  }
  return PositionBytes(s.mesh) + IndexBytes(s.mesh);
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
        staged_bytes_ -= std::min(staged_bytes_, s.bytes);
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
        // With the pool made, the read brings the hit blob too and the sector's block is counted at its estimate until the real one is placed.
        const bool with_hit = hit_ != nullptr && hit_->ready();
        (r.detail == city_rt::Detail::kHi ? st.hi_pool_held : st.lod_pool_held) = with_hit ? (r.detail == city_rt::Detail::kHi ? sectors_[r.sector].hi_pool : sectors_[r.sector].lod_pool) : 0;
        requests_.push_back({r.sector, r.detail, with_hit});
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
  // A mesh read before the hit data's pool existed (no blob with it) is not built as geometry only now that the pool is there: it goes back to be read again with its blob (PollLoads does the same for
  // a read that arrives after the pool was made).
  if (hit_ != nullptr && hit_->ready()) {
    for (const Staged& s : staged_) {
      if (city_rt::ReadBeforePool(true, s.hit_wanted)) {
        GiveBackForReread(s.sector, s.detail);
        staged_bytes_ -= std::min(staged_bytes_, s.bytes);
      }
    }
    std::erase_if(staged_, [](const Staged& s) { return city_rt::ReadBeforePool(true, s.hit_wanted); });
    if (staged_.empty()) {
      return false;
    }
  }
  // The frame's schedule: nearest first within the class's triangles and staged bytes.
  std::vector<city_rt::BuildItem> items;
  items.reserve(staged_.size());
  for (Staged& s : staged_) {
    const citypack::SectorRecord& rec = pack_.sector(s.sector);
    s.distance = city_rt::DistanceToBox(camera_, rec.aabb_min, rec.aabb_max);
    items.push_back({s.mesh.triangle_count(), s.bytes, s.distance});
  }
  const std::vector<uint8_t> take = city_rt::PlanBuilds(items, budget_.build_triangles, budget_.upload_bytes);

  struct Job {
    size_t staged_index;
    std::unique_ptr<plume::RenderBuffer> input;  // the mesh's own input buffer (no hit data); a pooled job builds over its block in the pool instead
    uint64_t index_offset = 0;
    plume::RenderBottomLevelASBuildInfo info;
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::unique_ptr<plume::RenderAccelerationStructure> structure;
    uint64_t size = 0;
    bool pooled = false;
    CityHitScene::Reservation block;
  };
  std::vector<Job> jobs;
  uint64_t scratch_needed = 0;
  bool pool_copies = false;  // blocks were copied into the pool in this list (it is left readable below, whatever else happens)
  // A mesh that cannot be built now is tried again after `retry` frames.
  const auto give_up = [&](Staged& s, uint64_t retry) {
    city_rt::SectorState& st = states_[s.sector];
    (s.detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kFailed;
    (s.detail == city_rt::Detail::kHi ? st.hi_retry_frame : st.lod_retry_frame) = frame_ + retry;
    (s.detail == city_rt::Detail::kHi ? st.hi_pool_held : st.lod_pool_held) = 0;
    ++counts_.failures;
  };
  for (size_t i = 0; i < staged_.size(); ++i) {
    if (!take[i]) {
      continue;
    }
    Staged& s = staged_[i];
    const citypack::MeshData& m = s.mesh;
    Job job;
    job.staged_index = i;
    plume::RenderBuffer* geometry_buffer = nullptr;
    uint64_t vertex_at = 0;
    // WITH HIT DATA the block in the pool is the upload and the build's input (city_hit_scene.h): positions and indices are written once, where the hit shading reads them.
    if (hit_ != nullptr && hit_->ready() && s.hit_wanted) {
      job.block = hit_->Reserve(s.sector, s.detail == city_rt::Detail::kLod, m, s.has_hit ? &s.hit : nullptr, slot_index);
      if (job.block.ok) {
        hit_->RecordCopy(list, &job.block);
        pool_copies = true;
        job.pooled = true;
        geometry_buffer = hit_->pool_buffer();
        vertex_at = job.block.base + job.block.plan.position_offset;
        job.index_offset = job.block.base + job.block.plan.index_offset;
      } else if (job.block.why == CityHitScene::Reservation::Why::kNoStaging) {
        break;  // no staging room this frame: the rest next frame
      } else if (job.block.why == CityHitScene::Reservation::Why::kNoRoom) {
        // The plan counts the pool's bytes, a range that long may still be missing (a hole too small, or the ranges eviction gave back are free a frame later): soon again.
        give_up(s, city_hit::kPoolRetryFrames);
        job.staged_index = size_t(-1);
        jobs.push_back(std::move(job));
        continue;
      } else if (job.block.why == CityHitScene::Reservation::Why::kBadData && !hit_bad_data_logged_) {
        hit_bad_data_logged_ = true;
        REXLOG_WARN("{} sector {} {}: the hit data does not fit its mesh ({}): built without it (logged once)", kPrefix, s.sector, s.detail == city_rt::Detail::kHi ? "hi detail" : "lod", job.block.detail);
      }
      // kBadData and kNotReady fall through: the mesh is built as in M1, its row stays invalid
    }
    if (!job.pooled) {
      const uint64_t position_bytes = PositionBytes(m);
      const uint64_t index_bytes = IndexBytes(m);
      const UploadSpan span = upload_->Allocate(slot_index, position_bytes + index_bytes, 16);
      if (!span) {
        break;  // no staging room this frame: the rest next frame
      }
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
      geometry_buffer = job.input.get();
      vertex_at = 0;
    }

    // The geometries: the opaque triangles (one geometry flagged opaque), then the alpha tested ones (not opaque: a query meets them as candidates).
    plume::RenderBottomLevelASMesh geometry[2];
    uint32_t geometries = 0;
    const uint32_t opaque = m.opaque_triangles;
    const uint32_t alpha = m.alpha_triangles();
    if (opaque != 0) {
      geometry[geometries++] = plume::RenderBottomLevelASMesh(plume::RenderBufferReference(geometry_buffer, job.index_offset), plume::RenderBufferReference(geometry_buffer, vertex_at),
                                                              plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32B32_FLOAT, opaque * 3, m.vertex_count(), 12, /*isOpaque=*/true);
    }
    if (alpha != 0) {
      geometry[geometries++] = plume::RenderBottomLevelASMesh(plume::RenderBufferReference(geometry_buffer, job.index_offset + uint64_t(opaque) * 12),
                                                              plume::RenderBufferReference(geometry_buffer, vertex_at), plume::RenderFormat::R32_UINT,
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
      // No room (or no object): the mesh is tried again after a while. Its block was copied: the range comes back when the GPU is done.
      if (job.pooled) {
        hit_->Discard(job.block);
      }
      give_up(s, kRetryFrames);
      frames_->Retire(std::move(job.input));
      job.staged_index = size_t(-1);
      jobs.push_back(std::move(job));
      continue;
    }
    scratch_needed = std::max(scratch_needed, job.info.scratchSize);
    jobs.push_back(std::move(job));
  }
  // The blocks were copied into the pool: from here it is read (by the builds below, which take their vertices and indices from it, and by the shaders). Always, so that the pool is never
  // left in the copy state for the next frame's shaders.
  if (hit_ != nullptr && pool_copies) {
    hit_->EndUpload(list);
  }
  if (jobs.empty()) {
    return pool_copies;
  }
  SlotResources& res = slots_[slot_index];
  if (!EnsureScratch(res, slot_index, std::max<uint64_t>(scratch_needed, 256))) {
    REXLOG_ERROR("{} the builds' scratch ({:.1f} MiB) could not be made: the meshes of this frame wait", kPrefix, double(scratch_needed) / (1024.0 * 1024.0));
    for (Job& j : jobs) {
      if (j.pooled && j.structure) {
        hit_->Discard(j.block);  // copied, never built: the mesh is read into a new block when it is tried again
      }
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
    target.bounds_known = car_rt_instances::BoundsOf(s.mesh.positions, target.lo.data(), target.hi.data());
    structure_bytes_ += j.size;
    counts_.triangles += target.triangles;
    ++counts_.builds;
    {
      // the probe update's log (DrainBuilt): the sector's box in the world
      const citypack::SectorRecord& built_rec = pack_.sector(s.sector);
      if (built_log_.size() < kBuiltLogMax) {
        BuiltBox box;
        for (int a = 0; a < 3; ++a) {
          box.lo[a] = double(built_rec.aabb_min[a]);
          box.hi[a] = double(built_rec.aabb_max[a]);
        }
        box.hi_detail = s.detail == city_rt::Detail::kHi;
        built_log_.push_back(box);
      } else {
        ++built_dropped_;
      }
    }
    counts_.build_triangles += target.triangles;
    city_rt::SectorState& st = states_[s.sector];
    (s.detail == city_rt::Detail::kHi ? st.hi : st.lod) = city_rt::Residency::kResident;
    (s.detail == city_rt::Detail::kHi ? st.hi_bytes : st.lod_bytes) = j.size;
    // The hit data's row becomes valid (the sector table the GPU reads is copied at the descriptor writes that follow this call).
    const bool committed = j.pooled && hit_->Commit(j.block, s.mesh.triangle_count(), s.mesh.opaque_triangles, 0, s.distance);  // the distance is the sector's textures' priority
    (s.detail == city_rt::Detail::kHi ? st.hi_pool_held : st.lod_pool_held) = committed ? city_hit::PoolAllocator::Round(j.block.plan.total_bytes) : 0;
    done_staged.push_back(uint32_t(j.staged_index));
    frames_->Retire(std::move(j.input));  // the structure needs none of it: the GPU is done with it when this frame is (a pooled job has none: the block stays)
  }
  // The structures are read by the top level's build that follows in this list.
  if (!built.empty()) {
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), built.data(), uint32_t(built.size()), nullptr, 0);
    if (!first_build_logged_) {
      first_build_logged_ = true;
      REXLOG_INFO("{} the first city structures are built (frame {}): {} of them, {} triangles{}", kPrefix, frame_, built.size(), counts_.build_triangles,
                  hit_ != nullptr && hit_->ready() ? "; with hit data in the pool" : "");
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
      staged_bytes_ -= std::min(staged_bytes_, staged_[i].bytes);
    }
  }
  staged_ = std::move(remaining);
  return true;
}

uint32_t CityRtScene::Prepare(const temporal::Mat4& view, bool flip_z, plume::RenderCommandList* list, uint32_t slot, std::vector<plume::RenderTopLevelASInstance>* instances, rt_scene::Bounds* bounds) {
  recorded_ = false;
  instances_this_frame_ = 0;
  if (!device_ || level_ == city_rt::Level::kOff || !budget_.on() || !list || !instances || slot >= kFramesInFlight) {
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
  if (hit_ != nullptr) {
    hit_->Record(list, slot);  // the material table, once
  }
  PollLoads();
  Plan(camera_);
  recorded_ = BuildStaged(list, slot);
  RefreshTexturePriorities();

  // The instances: one transform, the cars' own (a mesh in world space, identity model matrix).
  float transform[3][4];
  if (!rr::PackInstanceTransform(temporal::Identity(), view, flip_z, transform)) {
    return 0;
  }
  uint32_t added = 0;
  rt_scene::Bounds world_bounds;
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
    world_bounds.Include(s.lo.data(), s.hi.data(), s.bounds_known);
    ++added;
  }
  if (bounds && added != 0) {
    bounds->IncludeTransformed(world_bounds.lo.data(), world_bounds.hi.data(), transform, world_bounds.Usable());
  }
  instances_this_frame_ = added;
  LogState("");
  return added;
}

// The city textures of a sector are read nearest sector first (city_texture_set.h): the built sectors' distances from the camera are handed to the hit data every few frames.
void CityRtScene::RefreshTexturePriorities() {
  if (hit_ == nullptr || !camera_known_ || frame_ < next_priority_frame_) {
    return;
  }
  next_priority_frame_ = frame_ + city_texture::kPriorityRefreshFrames;
  for (uint32_t i = 0; i < built_.size(); ++i) {
    const bool hi = built_[i].hi.structure != nullptr;
    const bool lod = built_[i].lod.structure != nullptr;
    if (!hi && !lod) {
      continue;
    }
    const citypack::SectorRecord& rec = pack_.sector(i);
    const float distance = city_rt::DistanceToBox(camera_, rec.aabb_min, rec.aabb_max);
    if (hi) hit_->SetSectorDistance(i, false, distance);
    if (lod) hit_->SetSectorDistance(i, true, distance);
  }
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

void CityRtScene::DrainBuilt(std::vector<BuiltBox>* out, uint32_t* dropped) {
  if (out != nullptr) {
    out->insert(out->end(), built_log_.begin(), built_log_.end());
  }
  if (dropped != nullptr) {
    *dropped = built_dropped_;
  }
  built_log_.clear();
  built_dropped_ = 0;
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
  return std::format("; city ray tracing: {} hi and {} lod sectors, {:.1f} MiB, {} builds, {} evictions, {} failures{}", s.hi_resident, s.lod_resident,
                     double(s.structure_bytes) / (1024.0 * 1024.0), s.builds, s.evictions, s.failures, hit_ != nullptr ? hit_->CensusText() : std::string());
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
