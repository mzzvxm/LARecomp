// MCLA GPU renderer -- the city's texture set on the GPU. See city_texture_set.h.

#include "city_texture_set.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "upload.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[city tex]";
namespace cp = larecomp::citypack;

plume::RenderFormat HostFormat(cp::HitTextureFormat format) {
  switch (format) {
    case cp::HitTextureFormat::kBc1: return plume::RenderFormat::BC1_UNORM;
    case cp::HitTextureFormat::kBc2: return plume::RenderFormat::BC2_UNORM;
    case cp::HitTextureFormat::kBc3: return plume::RenderFormat::BC3_UNORM;
    default: return plume::RenderFormat::R8G8B8A8_UNORM;
  }
}

// What a block of the format is: texels on a side (1 for the raw format) and bytes.
uint32_t BlockSide(cp::HitTextureFormat format) { return format == cp::HitTextureFormat::kRgba8 ? 1u : 4u; }
uint32_t BlockBytes(cp::HitTextureFormat format) { return format == cp::HitTextureFormat::kBc1 ? 8u : (format == cp::HitTextureFormat::kRgba8 ? 4u : 16u); }

constexpr plume::RenderBarrierStages kReadStages = plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE);

}  // namespace

CityTextureSet::~CityTextureSet() { StopWorker(); }

bool CityTextureSet::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload, const std::filesystem::path& texture_file,
                                std::vector<cp::HitTexture> table, uint64_t hit_table_hash, std::string* error) {
  Shutdown();
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    REXLOG_WARN("{} {}", kPrefix, why);
    return false;
  };
  {
    cp::TextureFileReader check;
    std::string why;
    if (!check.Open(texture_file, table, hit_table_hash, &why)) return fail(why);
  }
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  file_ = texture_file;
  hit_table_hash_ = hit_table_hash;
  table_ = std::move(table);
  residency_ = std::make_unique<cp::TextureResidency>(table_);
  gpu_.clear();
  gpu_.resize(table_.size());
  rows_cpu_.assign(std::max<size_t>(table_.size(), 1), cp::GpuTextureRow{});
  for (uint32_t i = 0; i < table_.size(); ++i) rows_cpu_[i] = AbsentRow(i);
  rows_buffer_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(rows_buffer_bytes(), plume::RenderBufferFlag::STORAGE), "city texture rows");
  if (!rows_buffer_) {
    residency_.reset();
    rows_cpu_.clear();
    gpu_.clear();
    table_.clear();
    device_ = nullptr;
    frames_ = nullptr;
    heap_ = nullptr;
    upload_ = nullptr;
    return fail("the city texture rows buffer could not be made");
  }
  dirty_first_ = 0;
  dirty_last_ = uint32_t(rows_cpu_.size() - 1);
  stats_ = Stats{};
  stop_.store(false);
  worker_ = std::thread([this] { WorkerMain(); });
  REXLOG_INFO("{} the texture file is open: {} textures, {:.1f} MiB, hit table hash {:016x}", kPrefix, table_.size(), double(std::filesystem::file_size(texture_file)) / (1024.0 * 1024.0),
              hit_table_hash);
  return true;
}

void CityTextureSet::Shutdown() {
  StopWorker();
  if (device_ && frames_ && heap_) {
    for (uint32_t i = 0; i < gpu_.size(); ++i) Release(i);
  }
  ready_.clear();
  rows_buffer_.reset();
  rows_cpu_.clear();
  gpu_.clear();
  residency_.reset();
  table_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
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
    if (opened) {
      result.ok = reader.ReadMips(request.index, request.first_mip, &result.bytes, &result.error);
    } else {
      result.error = open_error;
    }
    {
      std::lock_guard lock(mutex_);
      results_.push_back(std::move(result));
    }
  }
}

void CityTextureSet::Configure(const larecomp::citypack::HitBudget& budget) {
  if (!residency_) return;
  cp::ResidencyConfig config = residency_->config();
  config.cap_px = budget.texture_cap_px;
  config.budget_bytes = budget.texture_bytes;
  config.upload_bytes_per_frame = budget.texture_upload_bytes;
  residency_->Configure(config);
}

void CityTextureSet::AddSector(uint32_t row, const uint16_t* textures, size_t count, float priority) {
  if (residency_) residency_->AddSector(row, textures, count, priority);
}
void CityTextureSet::SetSectorPriority(uint32_t row, float priority) {
  if (residency_) residency_->SetSectorPriority(row, priority);
}
void CityTextureSet::RemoveSector(uint32_t row) {
  if (residency_) residency_->RemoveSector(row);
}

CityTextureSet::Stats CityTextureSet::stats() const {
  Stats s = stats_;
  if (residency_) {
    s.resident = residency_->resident_count();
    s.held_bytes = residency_->committed_bytes();
  }
  return s;
}

cp::GpuTextureRow CityTextureSet::AbsentRow(uint32_t index) const {
  const cp::HitTexture& t = table_[index];
  cp::GpuTextureRow row;
  row.slot = cp::kNoSlot;
  row.size = uint32_t(t.width) | (uint32_t(t.height) << 16);
  row.mips = uint32_t(t.flags) << 16;  // no resident level
  return row;
}

void CityTextureSet::SetRow(uint32_t index, const cp::GpuTextureRow& row) {
  rows_cpu_[index] = row;
  dirty_first_ = std::min(dirty_first_, index);
  dirty_last_ = std::max(dirty_last_, index);
}

bool CityTextureSet::Release(uint32_t index) {
  Gpu& g = gpu_[index];
  const bool had = g.slot != kInvalidBindlessSlot;
  if (had) heap_->ReleaseTexture(g.slot, TextureDimension::k2D);  // the slot reads the null texture from now on; the view and the texture go after it
  frames_->Retire(std::move(g.view));
  frames_->Retire(std::move(g.texture));
  g = Gpu{};
  if (had || rows_cpu_[index].slot != cp::kNoSlot) SetRow(index, AbsentRow(index));
  return had;
}

void CityTextureSet::TakeResults() {
  std::deque<Result> done;
  {
    std::lock_guard lock(mutex_);
    done.swap(results_);
  }
  for (Result& r : done) {
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
  const uint32_t side = BlockSide(t.format);
  const uint32_t block_bytes = BlockBytes(t.format);

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
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get(), k, 0),
                            plume::RenderTextureCopyLocation::PlacedFootprint(span.buffer, format, c.width, c.height, 1, (c.pitch / block_bytes) * side, span.offset));
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
  SetRow(index, cp::MakeTextureRow(t, slot, first));
  ++stats_.uploads;
  stats_.upload_bytes += bytes.size();
  ready_.erase(bytes_it);
  return true;
}

void CityTextureSet::FlushRows(plume::RenderCommandList* list, uint32_t frame_slot) {
  if (!rows_buffer_ || dirty_first_ > dirty_last_) return;
  const uint64_t count = uint64_t(dirty_last_) - dirty_first_ + 1;
  const uint64_t bytes = count * sizeof(cp::GpuTextureRow);
  const UploadSpan span = upload_->Allocate(frame_slot, bytes, 16);
  if (!span) return;  // no staging room: the rows go next frame, a texture whose row says "not resident" is just drawn from its average
  std::memcpy(span.cpu, rows_cpu_.data() + dirty_first_, size_t(bytes));
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(rows_buffer_.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(rows_buffer_.get(), uint64_t(dirty_first_) * sizeof(cp::GpuTextureRow)), span.reference(), bytes);
  list->barriers(kReadStages, plume::RenderBufferBarrier(rows_buffer_.get(), plume::RenderBufferAccess::READ));
  dirty_first_ = 0xFFFFFFFFu;
  dirty_last_ = 0;
}

void CityTextureSet::Pump(plume::RenderCommandList* list, uint32_t frame_slot, uint64_t frame) {
  if (!residency_ || !list) return;
  frame_ = frame;
  TakeResults();
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
  FlushRows(list, frame_slot);
}

}  // namespace mcla::gpu
