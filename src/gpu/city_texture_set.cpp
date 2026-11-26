// MCLA GPU renderer -- the city's texture set on the GPU. See city_texture_set.h.

#include "city_texture_set.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "upload.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[city tex]";
namespace cp = larecomp::citypack;
namespace ct = city_texture;

// The rules header holds the numbers of the heap and the upload ring as constants of its own (it includes nothing of the renderer): they must be the renderer's.
static_assert(ct::kSlotCapacity == kBindlessTextureCount - kFirstAllocatableTextureSlot, "the slot maths is the bindless heap's");
static_assert(ct::kPitchAlignment == kTextureCopyPitchAlignment && ct::kOffsetAlignment == kTextureCopyOffsetAlignment, "the footprints follow the upload ring's placement rules");

plume::RenderFormat HostFormat(cp::HitTextureFormat format) {
  switch (format) {
    case cp::HitTextureFormat::kBc1: return plume::RenderFormat::BC1_UNORM;
    case cp::HitTextureFormat::kBc2: return plume::RenderFormat::BC2_UNORM;
    case cp::HitTextureFormat::kBc3: return plume::RenderFormat::BC3_UNORM;
    default: return plume::RenderFormat::R8G8B8A8_UNORM;
  }
}

constexpr plume::RenderBarrierStages kReadStages = plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE);

uint64_t NowNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

}  // namespace

CityTextureSet::~CityTextureSet() { StopWorker(); }

bool CityTextureSet::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload, const std::filesystem::path& texture_file, std::vector<cp::HitTexture> table,
                                uint64_t hit_table_hash, RowSink sink, std::string* error) {
  Shutdown();
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    REXLOG_WARN("{} {}", kPrefix, why);
    return false;
  };
  if (table.empty()) return fail("the hit file has no textures");
  {
    cp::TextureFileReader check;
    std::string why;
    if (!check.Open(texture_file, table, hit_table_hash, &why)) return fail(why);
  }
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  sink_ = std::move(sink);
  file_ = texture_file;
  hit_table_hash_ = hit_table_hash;
  table_ = std::move(table);
  residency_ = std::make_unique<cp::TextureResidency>(table_);
  gpu_.clear();
  gpu_.resize(table_.size());
  sectors_.clear();
  priorities_.clear();
  ready_.clear();
  budget_ = ct::TextureBudget{};
  scope_ = TextureScope::kNone;
  applied_scope_ = TextureScope::kNone;
  next_pressure_check_ = 0;
  slots_held_ = 0;
  logged_pressure_ = false;
  stats_ = Stats{};
  read_ns_.store(0);
  stop_.store(false);
  worker_ = std::thread([this] { WorkerMain(); });
  REXLOG_INFO("{} the texture file is open: {} textures, {:.1f} MiB, hit table hash {:016x}", kPrefix, table_.size(), double(std::filesystem::file_size(texture_file)) / (1024.0 * 1024.0),
              hit_table_hash);
  return true;
}

void CityTextureSet::Shutdown() {
  StopWorker();
  if (device_ && frames_ && heap_) {
    uint32_t released = 0;
    for (uint32_t i = 0; i < gpu_.size(); ++i) released += Release(i) ? 1u : 0u;
    if (released != 0) REXLOG_INFO("{} the texture set is given back: {} textures, {}", kPrefix, released, CensusText());
  }
  ready_.clear();
  sectors_.clear();
  priorities_.clear();
  gpu_.clear();
  residency_.reset();
  table_.clear();
  sink_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
  slots_held_ = 0;
}

void CityTextureSet::StopWorker() {
  if (!worker_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    stop_.store(true);
  }
  wake_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  requests_.clear();
  results_.clear();
}

void CityTextureSet::WorkerMain() {
  cp::TextureFileReader reader;
  std::string open_error;
  const bool opened = reader.Open(file_, table_, hit_table_hash_, &open_error);
  for (;;) {
    Request request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_.load() || !requests_.empty(); });
      if (stop_.load()) return;
      request = requests_.front();
      requests_.pop_front();
    }
    Result result;
    result.index = request.index;
    const uint64_t t0 = NowNs();
    if (opened) {
      result.ok = reader.ReadMips(request.index, request.first_mip, &result.bytes, &result.error);
    } else {
      result.error = open_error;
    }
    result.ns = NowNs() - t0;
    {
      std::lock_guard lock(mutex_);
      results_.push_back(std::move(result));
    }
  }
}

// ---------------------------------------------------------------------------
// what is wanted
// ---------------------------------------------------------------------------

void CityTextureSet::Configure(const ct::TextureBudget& budget, TextureScope scope) {
  if (!residency_) return;
  budget_ = budget;
  scope_ = budget.on() ? scope : TextureScope::kNone;
}

void CityTextureSet::RefreshSectors() {
  // The lists of the scope in force, for every sector the set knows. Nothing is wanted while the budget is off.
  for (const auto& [row, textures] : sectors_) {
    const std::vector<uint16_t>& list = scope_ == TextureScope::kNone ? textures.alpha : textures.For(scope_);
    const size_t count = scope_ == TextureScope::kNone ? 0 : list.size();
    const auto p = priorities_.find(row);
    residency_->AddSector(row, list.data(), count, p != priorities_.end() ? p->second : 0.0f);
  }
  applied_scope_ = scope_;
}

void CityTextureSet::AddSector(uint32_t row, ct::SectorTextures textures, float priority) {
  if (!residency_) return;
  const std::vector<uint16_t>& list = scope_ == TextureScope::kNone ? textures.alpha : textures.For(scope_);
  const size_t count = scope_ == TextureScope::kNone ? 0 : list.size();
  residency_->AddSector(row, list.data(), count, priority);
  priorities_[row] = priority;
  sectors_[row] = std::move(textures);
}

void CityTextureSet::SetSectorPriority(uint32_t row, float priority) {
  if (!residency_) return;
  residency_->SetSectorPriority(row, priority);
  const auto it = priorities_.find(row);
  if (it != priorities_.end()) it->second = priority;
}

void CityTextureSet::RemoveSector(uint32_t row) {
  if (!residency_) return;
  residency_->RemoveSector(row);
  sectors_.erase(row);
  priorities_.erase(row);
}

void CityTextureSet::ClearSectors() {
  if (!residency_) return;
  for (const auto& entry : sectors_) residency_->RemoveSector(entry.first);
  sectors_.clear();
  priorities_.clear();
}

CityTextureSet::Stats CityTextureSet::stats() const {
  Stats s = stats_;
  s.read_ns = read_ns_.load();
  s.slots_held = slots_held_;
  if (residency_) {
    s.resident = residency_->resident_count();
    s.held_bytes = residency_->resident_bytes();
    s.committed_bytes = residency_->committed_bytes();
  }
  return s;
}

std::string CityTextureSet::CensusText() const {
  if (!residency_) return {};
  const Stats s = stats();
  const uint32_t in_use = heap_ ? heap_->textures_in_use() : 0;
  return std::format("city textures ({}, cap {}): {} resident of {}, {:.1f} MiB accounted ({:.1f} MiB of the budget), {} slots held of {} in use, {} reads ({:.0f} ms), {} uploads, {} evictions, {} failed reads, "
                     "{} refused creations, {} deferred uploads, {} looks at a full video memory",
                     ScopeName(scope_), budget_.cap_px == 0 ? std::string("full") : std::format("{} px", budget_.cap_px), s.resident, table_.size(), double(s.held_bytes) / (1024.0 * 1024.0),
                     double(budget_.bytes) / (1024.0 * 1024.0), s.slots_held, in_use, s.reads, double(s.read_ns) / 1e6, s.uploads, s.evictions, s.read_failures, s.creation_failures,
                     s.upload_deferred, s.growth_blocked_checks);
}

// ---------------------------------------------------------------------------
// the rows and the objects
// ---------------------------------------------------------------------------

cp::GpuTextureRow CityTextureSet::AbsentRow(uint32_t index) const {
  const cp::HitTexture& t = table_[index];
  cp::GpuTextureRow row;
  row.slot = cp::kNoSlot;
  row.size = uint32_t(t.width) | (uint32_t(t.height) << 16);
  row.mips = uint32_t(t.flags) << 16;  // no resident level
  return row;
}

void CityTextureSet::Publish(uint32_t index, const cp::GpuTextureRow& row) {
  if (sink_) sink_(index, row);
}

bool CityTextureSet::Release(uint32_t index) {
  Gpu& g = gpu_[index];
  const bool had = g.slot != kInvalidBindlessSlot;
  if (had) {
    heap_->ReleaseTexture(g.slot, TextureDimension::k2D);  // the slot reads the null texture from now on; the view and the texture go after it
    --slots_held_;
  }
  frames_->Retire(std::move(g.view));
  frames_->Retire(std::move(g.texture));
  g = Gpu{};
  if (had) Publish(index, AbsentRow(index));
  return had;
}

void CityTextureSet::TakeResults() {
  std::deque<Result> done;
  {
    std::lock_guard lock(mutex_);
    done.swap(results_);
  }
  for (Result& r : done) {
    read_ns_.fetch_add(r.ns);
    const bool keep = residency_->ReadFinished(r.index, r.ok);
    if (!r.ok) {
      if (++stats_.read_failures <= 5) REXLOG_WARN("{} texture {} could not be read: {}", kPrefix, r.index, r.error);
      continue;
    }
    if (keep) ready_[r.index] = std::move(r.bytes);
  }
}

bool CityTextureSet::Upload(uint32_t index, plume::RenderCommandList* list, uint32_t frame_slot) {
  const cp::HitTexture& t = table_[index];
  const auto bytes_it = ready_.find(index);
  if (bytes_it == ready_.end()) {
    residency_->CreationFailed(index);  // no bytes: it was never read, or the loader lost them
    return false;
  }
  const std::vector<uint8_t>& bytes = bytes_it->second;
  const uint32_t first = residency_->first_mip(index);
  const uint32_t levels = t.mip_count - first;
  const plume::RenderFormat format = HostFormat(t.format);

  // The staging of every level first: if the upload memory is short nothing has been recorded and the bytes wait for the next frame.
  const std::vector<cp::UploadLevel> plan = cp::PlanUploadLevels(t, first, uint32_t(kTextureCopyPitchAlignment));
  std::vector<UploadSpan> spans(levels);
  uint64_t source = 0;
  for (uint32_t k = 0; k < levels; ++k) {
    source += uint64_t(plan[k].row_bytes) * plan[k].rows;
    spans[k] = upload_->Allocate(frame_slot, plan[k].staging_bytes(), kTextureCopyOffsetAlignment);
    if (!spans[k]) {
      ++stats_.upload_deferred;
      residency_->UploadFailed(index);
      return false;
    }
  }
  if (source != bytes.size()) {
    REXLOG_ERROR("{} texture {}: the file gave {} bytes, the levels from {} need {}", kPrefix, index, bytes.size(), first, source);
    ready_.erase(bytes_it);
    residency_->CreationFailed(index);
    return false;
  }

  const std::string name = device_->object_names() ? std::format("city texture {}", index) : std::string();
  const char* debug_name = name.empty() ? nullptr : name.c_str();
  std::unique_ptr<plume::RenderTexture> texture =
      device_->CreateTexture(plume::RenderTextureDesc::Texture2D(std::max(uint32_t(t.width) >> first, 1u), std::max(uint32_t(t.height) >> first, 1u), levels, format), debug_name);
  if (!texture) {
    if (++stats_.creation_failures <= 3) REXLOG_WARN("{} texture {} ({}x{}, {} levels from {}) could not be created: it is tried again later", kPrefix, index, t.width, t.height, levels, first);
    ready_.erase(bytes_it);
    residency_->CreationFailed(index);
    return false;
  }
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t k = 0; k < levels; ++k) {
    const cp::UploadLevel& c = plan[k];
    const UploadSpan& span = spans[k];
    for (uint32_t r = 0; r < c.rows; ++r) std::memcpy(span.cpu + uint64_t(r) * c.pitch, bytes.data() + c.source_offset + uint64_t(r) * c.row_bytes, c.row_bytes);
    // the arguments the game's own texture cache gives (textures.cpp Upload): the level's size in texels, the staging row's width in texels (city_texture_rules.h FootprintOf)
    const ct::Footprint fp = ct::FootprintOf(c, t.format);
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get(), k, 0),
                            plume::RenderTextureCopyLocation::PlacedFootprint(span.buffer, format, fp.width, fp.height, 1, fp.row_width, span.offset));
  }
  list->barriers(kReadStages, plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));

  plume::RenderTextureViewDesc view_desc = plume::RenderTextureViewDesc::Texture2D(format);
  view_desc.mipLevels = levels;
  std::unique_ptr<plume::RenderTextureView> view = device_->CreateTextureView(*texture, view_desc, debug_name);
  uint32_t slot = kInvalidBindlessSlot;
  if (view) slot = heap_->AllocateTexture(*texture, *view);
  if (slot == kInvalidBindlessSlot) {
    // The copies are in the list: the texture goes through the ring's retirement, not straight out.
    frames_->Retire(std::move(view));
    frames_->Retire(std::move(texture));
    if (++stats_.creation_failures <= 3) REXLOG_WARN("{} texture {}: no view or no bindless slot ({} in use): it is tried again later", kPrefix, index, heap_->textures_in_use());
    ready_.erase(bytes_it);
    residency_->CreationFailed(index);
    return false;
  }
  Gpu& g = gpu_[index];
  g.texture = std::move(texture);
  g.view = std::move(view);
  g.slot = slot;
  ++slots_held_;
  Publish(index, cp::MakeTextureRow(t, slot, first));  // the gamma bit is the texture's flag (bit 31 of the row's slot)
  if (stats_.uploads == 0) {
    REXLOG_INFO("{} the first texture is resident (frame {}): {} ({}x{}, {} levels from {}, bindless slot {})", kPrefix, frame_, index, t.width, t.height, levels, first, slot);
  }
  ++stats_.uploads;
  stats_.upload_bytes += bytes.size();
  ready_.erase(bytes_it);
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void CityTextureSet::Apply(uint64_t frame) {
  if (device_ && frame >= next_pressure_check_) {
    next_pressure_check_ = frame + ct::kPressureCheckFrames;
    const auto memory = device_->QueryVideoMemory();
    const bool pressure = !ct::GrowthAllowed(memory.used, memory.budget, ct::kGrowthMargin);
    if (pressure && !logged_pressure_) {
      REXLOG_WARN("{} video memory is nearly full ({} of {} MiB in use); city textures retain the selected detail",
                  kPrefix, memory.used >> 20, memory.budget >> 20);
    }
    logged_pressure_ = pressure;
  }
  if (scope_ != applied_scope_) RefreshSectors();
  const uint32_t max_textures = ct::MaxTextures(scope_, heap_->textures_in_use(), slots_held_);
  const uint64_t bytes = budget_.on() ? budget_.bytes : 1;
  residency_->Configure(ct::ConfigFor(budget_, max_textures, bytes));
}

void CityTextureSet::Pump(plume::RenderCommandList* list, uint32_t frame_slot, uint64_t frame) {
  if (!residency_ || !list) return;
  frame_ = frame;
  TakeResults();
  Apply(frame);
  const cp::TextureResidency::Plan plan = residency_->Step(frame);
  for (const uint32_t index : plan.evictions) {
    Release(index);
    ready_.erase(index);  // bytes of a texture that was Ready, given back before its upload
    ++stats_.evictions;
  }
  if (!plan.reads.empty()) {
    {
      std::lock_guard lock(mutex_);
      for (const cp::TextureResidency::Read& r : plan.reads) requests_.push_back({r.index, r.first_mip});
    }
    stats_.reads += plan.reads.size();
    wake_.notify_one();
  }
  for (const uint32_t index : plan.uploads) Upload(index, list, frame_slot);
}

}  // namespace mcla::gpu
