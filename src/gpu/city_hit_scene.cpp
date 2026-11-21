// MCLA GPU renderer -- the city's hit data in the GPU. See city_hit_scene.h.

#include "city_hit_scene.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "pass_timing.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[city hit]";
namespace cf = larecomp::citypack;

uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

}  // namespace

CityHitScene::~CityHitScene() = default;

CityHitScene::Bindings CityHitScene::AddBindings(plume::RenderDescriptorSetBuilder& builder) {
  Bindings out;
  out.pool = builder.addByteAddressBuffer(kPoolRegister);
  out.sectors = builder.addStructuredBuffer(kSectorsRegister);
  out.materials = builder.addStructuredBuffer(kMaterialsRegister);
  out.textures = builder.addStructuredBuffer(kTexturesRegister);
  return out;
}

bool CityHitScene::Initialize(Device& device, FrameRing& frames, UploadRing& upload, PassTiming& pass_timing, const std::filesystem::path& hit_file, gpu_class::GpuClass cls) {
  device_ = &device;
  frames_ = &frames;
  upload_ = &upload;
  pass_timing_ = &pass_timing;
  hit_file_ = hit_file;
  class_ = cls;
  book_ = std::make_shared<city_hit::PoolBook>();
  textures_.Reset(1);  // one row, not uploaded: what a shader that reads the table before H2's loader has run finds
  return true;
}

void CityHitScene::Shutdown() {
  DropPool("shutdown");
  probe_.Shutdown();
  for (SlotTables& slot : slots_) {
    if (slot.sectors && slot.sectors_cpu) slot.sectors->unmap(0, nullptr);
    if (slot.textures && slot.textures_cpu) slot.textures->unmap(0, nullptr);
    slot = SlotTables{};
  }
  for (Standin* s : {&standin_pool_, &standin_sectors_, &standin_materials_, &standin_textures_}) {
    if (s->buffer && s->cpu) s->buffer->unmap(0, nullptr);
    *s = Standin{};
  }
  standin_bytes_ = 0;
  reader_ = cf::HitReader{};
  reader_open_ = false;
  base_ = nullptr;
  sector_count_ = 0;
  book_.reset();
  device_ = nullptr;
  frames_ = nullptr;
  upload_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the file and the pool
// ---------------------------------------------------------------------------

void CityHitScene::SetBase(const cf::PackReader* base) {
  if (base_ == base) return;
  base_ = base;
  sector_count_ = base ? base->sector_count() : 0;
  reader_open_ = false;
  reader_ = cf::HitReader{};
  retry_frame_ = 0;
  if (!base) DropPool("the base pack was given up");
}

bool CityHitScene::OpenFile() {
  if (reader_open_) return true;
  if (!base_ || !base_->is_open()) return false;
  std::string error;
  if (!reader_.Open(hit_file_, cf::IdentityOf(*base_), &error)) {
    retry_frame_ = frame_ + city_hit::kFileRetryFrames;
    if (logged_state_ != 1 || file_problem_ != error) {
      logged_state_ = 1;
      file_problem_ = error;
      REXLOG_INFO("{} the city hit data is not used: {} (looked for {}); the structures and the shadows go on without it; the file is built from the game files (citypack_tool, and the launcher "
                  "once reflections ask for it), looked for again in a minute",
                  kPrefix, error, hit_file_.string());
    }
    return false;
  }
  reader_open_ = true;
  logged_state_ = 2;
  textures_.Reset(std::max<uint32_t>(1, uint32_t(reader_.textures().size())));
  REXLOG_INFO("{} the city hit file is open: {} sectors, {} materials, {} textures, {:.0f} MiB on disk, builder version {}", kPrefix, reader_.sector_count(), reader_.material_count(),
              reader_.textures().size(), double(reader_.header().file_size) / (1024.0 * 1024.0), reader_.header().builder_version);
  return true;
}

bool CityHitScene::ReadBlob(uint32_t sector, cf::MeshKind kind, cf::HitBlob* out, std::string* error) const {
  if (!reader_open_) {
    if (error) error->clear();
    return false;
  }
  return reader_.ReadBlob(sector, kind, out, error);
}

std::unique_ptr<plume::RenderBuffer> CityHitScene::MakeUpload(uint64_t bytes, uint8_t** cpu, const char* name) {
  std::unique_ptr<plume::RenderBuffer> buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::STORAGE), name);
  if (!buffer) return nullptr;
  const plume::RenderRange no_read(0, 0);
  uint8_t* mapped = static_cast<uint8_t*>(buffer->map(0, &no_read));
  if (!mapped) return nullptr;
  *cpu = mapped;
  return buffer;
}

bool CityHitScene::MakePool() {
  // CHECK 3: the class's pool sizes (64 / 128 / 256 MiB) against the real working set: the census line says the blocks' MiB and the fragmentation; the p90 estimate is 29 / 50 / 120 MiB.
  const city_hit::PoolBudget want = city_hit::PoolBudgetFor(class_, level_);
  if (!want.on() || sector_count_ == 0 || !OpenFile()) return false;
  const backend::VideoMemory memory = device_->QueryVideoMemory();
  const uint64_t capacity = city_hit::FitCapacity(want.capacity, memory.used, memory.budget);
  if (capacity == 0) {
    retry_frame_ = frame_ + city_hit::kFileRetryFrames;
    if (logged_state_ != 3) {
      logged_state_ = 3;
      REXLOG_WARN("{} no room in the video memory for the city hit pool ({} MiB of {} MiB used, the class's pool {} MiB, the smallest {} MiB): the hit data stays off, the shadows go on", kPrefix,
                  memory.used >> 20, memory.budget >> 20, want.capacity >> 20, city_hit::kMinPoolBytes >> 20);
    }
    return false;
  }
  std::unique_ptr<plume::RenderBuffer> pool =
      device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(capacity, plume::RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT | plume::RenderBufferFlag::STORAGE), "city hit pool");
  const uint64_t material_bytes = uint64_t(reader_.material_words().size()) * 4;
  std::unique_ptr<plume::RenderBuffer> materials =
      material_bytes != 0 ? device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(AlignUp(material_bytes, 256), plume::RenderBufferFlag::STORAGE), "city hit materials") : nullptr;
  if (!pool || !materials || !book_->Reset(capacity, sector_count_)) {
    retry_frame_ = frame_ + city_hit::kFileRetryFrames;
    REXLOG_ERROR("{} the city hit pool ({} MiB) or the material table could not be made: the hit data stays off", kPrefix, capacity >> 20);
    return false;
  }
  pool_ = std::move(pool);
  materials_ = std::move(materials);
  materials_bytes_ = AlignUp(material_bytes, 256);
  materials_pending_ = true;
  pool_budget_ = city_hit::PoolBudget{capacity, want.staged_bytes};
  requested_capacity_ = want.capacity;
  access_ = Access::kNone;
  ++epoch_;
  logged_state_ = 4;
  REXLOG_INFO("{} the city hit pool is made: {} MiB (the class's {} MiB), {} sector slots, {} materials ({:.1f} KiB), staging {} MiB a frame, the plan fills {} MiB of it", kPrefix, capacity >> 20,
              want.capacity >> 20, sector_count_ * 2, reader_.material_count(), double(material_bytes) / 1024.0, uint64_t(want.staged_bytes) >> 20, city_hit::PlanCap(capacity) >> 20);
  return true;
}

void CityHitScene::DropPool(const char* why) {
  const bool had = pool_ != nullptr;
  if (pool_) frames_->Retire(std::move(pool_));
  if (materials_) frames_->Retire(std::move(materials_));
  if (book_) book_->Forget();  // the deferred frees of this pool are dropped with its generation
  materials_pending_ = false;
  materials_bytes_ = 0;
  pool_budget_ = city_hit::PoolBudget{};
  requested_capacity_ = 0;
  access_ = Access::kNone;
  copied_this_frame_ = false;
  if (had) {
    ++epoch_;
    REXLOG_INFO("{} the city hit pool is given back ({}); {} blocks were committed, {} evicted", kPrefix, why, counts_.committed, counts_.evicted);
  }
}

void CityHitScene::BeginFrame(city_rt::Level level) {
  ++frame_;
  if (!device_) return;
  level_ = level;
  copied_this_frame_ = false;
  if (level_ != city_rt::Level::kOff) {
    last_wanted_frame_ = frame_;
    const uint64_t wanted = city_hit::PoolBudgetFor(class_, level_).capacity;
    if (pool_ && wanted != requested_capacity_) DropPool("the level changed");  // another size: the structures built over the old blocks are built again (epoch)
    if (!pool_ && base_ && frame_ >= retry_frame_) MakePool();
    return;
  }
  if (pool_ && frame_ > last_wanted_frame_ + kIdleReleaseFrames) DropPool("no request for a while");
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

// CHECK 1: the pool's barriers on the D3D12 debug layer and Vulkan sync validation: COPY write then GRAPHICS|COMPUTE read, once a frame, the same pair the car scene's shared buffers use (SetAccess);
// the pool is both the structures' build input (ACCELERATION_STRUCTURE_INPUT) and a shader resource (STORAGE), the flags the car scene's index buffer has.
void CityHitScene::SetPoolAccess(plume::RenderCommandList* list, Access access) {
  if (!pool_ || access_ == access) return;
  if (access == Access::kCopyWrite) {
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::WRITE));
  } else if (access == Access::kRead) {
    list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::READ));
  }
  access_ = access;
}

void CityHitScene::Record(plume::RenderCommandList* list, uint32_t slot) {
  if (!pool_ || !materials_ || !materials_pending_ || !list || slot >= kFramesInFlight) return;
  // The material table, once: staged, copied, readable. The pool's blocks are copied after this in the same list.
  const uint64_t bytes = materials_bytes_;
  const UploadSpan span = upload_->Allocate(slot, bytes, 16);
  if (!span) return;  // no staging this frame: the next one
  std::memset(span.cpu, 0, size_t(bytes));
  std::memcpy(span.cpu, reader_.material_words().data(), reader_.material_words().size() * 4);
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(materials_.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(materials_.get(), 0), span.reference(), bytes);
  list->barriers(plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE), plume::RenderBufferBarrier(materials_.get(), plume::RenderBufferAccess::READ));
  materials_pending_ = false;
}

CityHitScene::Reservation CityHitScene::Reserve(uint32_t sector, bool lod, const cf::MeshData& mesh, const cf::HitBlob* blob, uint32_t frame_slot) {
  Reservation r;
  r.sector = sector;
  r.lod = lod;
  if (!ready() || frame_slot >= kFramesInFlight) {
    r.why = Reservation::Why::kNotReady;
    return r;
  }
  const bool runs = blob != nullptr;
  const bool corners = blob != nullptr && blob->has_corners();
  r.plan = cf::PlanPoolBlock(mesh.vertex_count(), mesh.triangle_count(), blob ? uint32_t(blob->runs.size()) : 0u, runs, corners);
  if (!book_->Place(sector, lod, r.plan.total_bytes, &r.base)) {
    r.why = Reservation::Why::kNoRoom;
    ++counts_.failures_no_room;
    return r;
  }
  r.span = upload_->Allocate(frame_slot, r.plan.total_bytes, 16);
  if (!r.span) {
    book_->Abandon(sector, lod);
    r.why = Reservation::Why::kNoStaging;
    ++counts_.failures_staging;
    return r;
  }
  std::string error;
  if (!cf::FillPoolBlock(r.plan, mesh, blob, r.span.cpu, size_t(r.span.size), &error)) {
    book_->Abandon(sector, lod);
    r.why = Reservation::Why::kBadData;
    r.detail = error;
    ++counts_.failures_data;
    return r;
  }
  uint64_t samples = 0;
  r.checksum = city_hit::ProbeOfBlock(r.span.cpu, r.plan, mesh.triangle_count(), &samples);
  r.samples = uint32_t(samples);
  r.ok = true;
  ++counts_.reserved;
  return r;
}

void CityHitScene::RecordCopy(plume::RenderCommandList* list, Reservation* r) {
  if (!r || !r->ok || !pool_) return;
  if (!copied_this_frame_) {
    copied_this_frame_ = true;
    pass_timing_->Mark(list, "city hit upload");
  }
  SetPoolAccess(list, Access::kCopyWrite);
  list->copyBufferRegion(plume::RenderBufferReference(pool_.get(), r->base), r->span.reference(), r->plan.total_bytes);
  r->copied = true;
  counts_.staged_bytes += r->plan.total_bytes;
}

void CityHitScene::EndUpload(plume::RenderCommandList* list) {
  if (access_ == Access::kCopyWrite) SetPoolAccess(list, Access::kRead);
}

bool CityHitScene::Commit(const Reservation& r, uint32_t triangle_count, uint32_t opaque_triangles, uint32_t extra_flags) {
  if (!r.ok || !book_) return false;
  if (!book_->Commit(r.sector, r.lod, r.plan, triangle_count, opaque_triangles, extra_flags, r.checksum, r.samples)) return false;
  ++counts_.committed;
  if (!first_block_logged_) {
    first_block_logged_ = true;
    REXLOG_INFO("{} the first hit data block is in the pool: sector {} {}, {} triangles, {} runs{}, {:.1f} KiB at offset {}", kPrefix, r.sector, r.lod ? "lod" : "hi", triangle_count,
                r.plan.has_runs ? 1 : 0, r.plan.has_corners ? " and corners" : "", double(r.plan.total_bytes) / 1024.0, r.base);
  }
  return true;
}

void CityHitScene::Abandon(const Reservation& r) {
  if (r.ok && book_) book_->Abandon(r.sector, r.lod);
}

void CityHitScene::DeferFree(const city_hit::Block& block) {
  if (!block.present || !book_) return;
  frames_->RetireAction([book = book_, generation = book_->generation(), base = block.base, bytes = block.bytes] { book->FreeRange(generation, base, bytes); });
}

void CityHitScene::Discard(const Reservation& r) {
  if (!r.ok || !book_) return;
  DeferFree(book_->Evict(r.sector, r.lod));
}

void CityHitScene::Evict(uint32_t sector, bool lod) {
  if (!book_ || !book_->ready()) return;
  const city_hit::Block gone = book_->Evict(sector, lod);
  if (gone.present) {
    ++counts_.evicted;
    DeferFree(gone);
  }
}

// ---------------------------------------------------------------------------
// the tables
// ---------------------------------------------------------------------------

bool CityHitScene::MakeStandin(Standin& s, uint64_t bytes, const void* fill, uint64_t fill_bytes, const char* name) {
  uint8_t* cpu = nullptr;
  std::unique_ptr<plume::RenderBuffer> buffer = MakeUpload(bytes, &cpu, name);
  if (!buffer) return false;
  std::memset(cpu, 0, size_t(bytes));
  if (fill && fill_bytes != 0) std::memcpy(cpu, fill, size_t(std::min(fill_bytes, bytes)));
  s.buffer = std::move(buffer);
  s.cpu = cpu;
  standin_bytes_ += bytes;
  return true;
}

uint64_t CityHitScene::SectorTableBytes() const {
  return book_ && book_->ready() ? book_->table().bytes() : uint64_t(sector_count_) * 2 * sizeof(cf::GpuSectorRow);
}

bool CityHitScene::EnsureTables(SlotTables& slot, uint32_t index) {
  const auto room = [](uint64_t bytes) { return AlignUp(std::max<uint64_t>(bytes, 256), 4096); };
  // The sector table's size is the pack's even while there is no pool (all zero then: flags 0, "no hit data"), so that a read of any slot is inside the buffer.
  const uint64_t sector_bytes = SectorTableBytes();
  if (sector_bytes != 0 && slot.sectors_bytes < sector_bytes) {
    uint8_t* cpu = nullptr;
    std::unique_ptr<plume::RenderBuffer> buffer = MakeUpload(room(sector_bytes), &cpu, std::format("city hit sectors {}", index).c_str());
    if (!buffer) return false;
    if (slot.sectors && slot.sectors_cpu) slot.sectors->unmap(0, nullptr);
    frames_->Retire(std::move(slot.sectors));
    slot.sectors = std::move(buffer);
    slot.sectors_cpu = cpu;
    slot.sectors_bytes = room(sector_bytes);
    slot.sectors_version = 0;  // the new buffer holds nothing yet
    std::memset(cpu, 0, size_t(slot.sectors_bytes));
  }
  const uint64_t texture_bytes = textures_.bytes();
  if (texture_bytes != 0 && slot.textures_bytes < texture_bytes) {
    uint8_t* cpu = nullptr;
    std::unique_ptr<plume::RenderBuffer> buffer = MakeUpload(room(texture_bytes), &cpu, std::format("city hit textures {}", index).c_str());
    if (!buffer) return false;
    if (slot.textures && slot.textures_cpu) slot.textures->unmap(0, nullptr);
    frames_->Retire(std::move(slot.textures));
    slot.textures = std::move(buffer);
    slot.textures_cpu = cpu;
    slot.textures_bytes = room(texture_bytes);
    slot.textures_version = 0;
    std::memset(cpu, 0, size_t(slot.textures_bytes));
  }
  return true;
}

// CHECK 2: a ByteAddressBuffer of 64 to 256 MiB as one descriptor (setBuffer with size 0) on both backends, and the two StructuredBuffer<uint4> views (stride 16) over upload buffers, as the car
// scene's own table; the probe (mcla_gpu_city_hit_probe) is what proves it on each backend.
void CityHitScene::Bind(plume::RenderDescriptorSet* set, const Bindings& b, uint32_t slot_index) {
  if (!set || !device_ || slot_index >= kFramesInFlight) return;
  // The stand-ins: small, zeroed (sector rows with flags 0, a default material, a texture row that is not uploaded), made once.
  if (!standin_pool_.buffer) {
    uint32_t material[8];
    cf::HitMaterial{}.Pack(material);
    const cf::GpuTextureRow no_texture;
    MakeStandin(standin_pool_, 256, nullptr, 0, "city hit pool stand-in");
    MakeStandin(standin_sectors_, 256, nullptr, 0, "city hit sectors stand-in");
    MakeStandin(standin_materials_, 256, material, sizeof(material), "city hit materials stand-in");
    MakeStandin(standin_textures_, 256, &no_texture, sizeof(no_texture), "city hit textures stand-in");
  }
  SlotTables& st = slots_[slot_index];
  const bool tables = EnsureTables(st, slot_index);
  const plume::RenderBufferStructuredView rows(16, 0);

  set->setBuffer(b.pool, pool_ ? pool_.get() : standin_pool_.buffer.get(), 0);

  if (tables && st.sectors) {
    if (book_ && book_->ready()) {
      const city_hit::SectorTable& table = book_->table();
      if (st.sectors_version != table.version()) {
        std::memcpy(st.sectors_cpu, table.data(), table.bytes());
        st.sectors_version = table.version();
      }
    } else if (st.sectors_version != 0) {
      // The pool was given back: this slot's copy still names blocks that are gone. All zero again: flags 0, "no hit data".
      std::memset(st.sectors_cpu, 0, size_t(st.sectors_bytes));
      st.sectors_version = 0;
    }
    set->setBuffer(b.sectors, st.sectors.get(), SectorTableBytes(), &rows);
  } else if (standin_sectors_.buffer) {
    set->setBuffer(b.sectors, standin_sectors_.buffer.get(), 256, &rows);
  }

  if (pool_ && materials_ && !materials_pending_) {
    set->setBuffer(b.materials, materials_.get(), materials_bytes_, &rows);
  } else if (standin_materials_.buffer) {
    set->setBuffer(b.materials, standin_materials_.buffer.get(), 256, &rows);
  }

  if (tables && st.textures && textures_.count() != 0) {
    if (st.textures_version != textures_.version()) {
      std::memcpy(st.textures_cpu, textures_.data(), textures_.bytes());
      st.textures_version = textures_.version();
    }
    set->setBuffer(b.textures, st.textures.get(), textures_.bytes(), &rows);
  } else if (standin_textures_.buffer) {
    set->setBuffer(b.textures, standin_textures_.buffer.get(), 256, &rows);
  }
}

void CityHitScene::RecordProbe(plume::RenderCommandList* list, plume::RenderDescriptorSet* scene_set, const plume::RenderDescriptorSetDesc& scene_desc, HostPasses& passes, BindlessHeap& heap) {
  if (!device_ || !ready() || !larecomp::settings::mcla_gpu_city_hit_probe.Get()) return;
  probe_.Record(*device_, *frames_, passes, heap, scene_desc, list, scene_set, book_, material_words().empty() ? 0u : material_words()[0], textures_.first_slot(), frame_);
}

// ---------------------------------------------------------------------------
// what it holds
// ---------------------------------------------------------------------------

CityHitScene::Stats CityHitScene::stats() const {
  Stats s = counts_;
  if (book_) {
    const city_hit::PoolAllocator::Stats a = book_->allocator().stats();
    s.pool_bytes = pool_ ? a.capacity : 0;
    s.used_bytes = a.used;
    s.committed_bytes = book_->committed_bytes();
    s.pending_free = book_->pending_free_bytes();
    s.fragmentation = book_->allocator().FragmentationPercent();
    s.blocks = book_->table().valid_count();
  }
  return s;
}

city_hit::Held CityHitScene::Held() const {
  city_hit::Held h;
  h.pool = pool_ && book_ ? book_->allocator().capacity() : 0;
  h.materials = materials_bytes_;
  for (const SlotTables& slot : slots_) h.tables += slot.sectors_bytes + slot.textures_bytes;
  h.tables += standin_bytes_ + probe_.HeldBytes();
  return h;
}

std::string CityHitScene::CensusText() const {
  if (!pool_) return {};
  const Stats s = stats();
  return std::format("; city hit data: pool {:.0f} MiB, {:.1f} MiB in {} blocks, {} reserved, {} evicted, {} failed (no room {}, bad data {}, staging {}), fragmentation {}%",
                     double(s.pool_bytes) / (1024.0 * 1024.0), double(s.committed_bytes) / (1024.0 * 1024.0), s.blocks, s.reserved, s.evicted,
                     s.failures_no_room + s.failures_data + s.failures_staging, s.failures_no_room, s.failures_data, s.failures_staging, s.fragmentation);
}

}  // namespace mcla::gpu
