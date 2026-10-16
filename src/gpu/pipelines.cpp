// MCLA GPU renderer -- graphics pipelines of the guest draws. See pipelines.h.
//
// Canonicalization follows reblue's pipeline cache (BSD-3-Clause) in spirit;
// the polygon offset conversion is ported from Xenia's draw_util
// GetD3D10IntegerPolygonOffset (SDK rex/graphics/util/draw.h; Copyright Ben
// Vanik, the Xenia authors, BSD-3-Clause).

#include "pipelines.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "backend.h"
#include "constants.h"
#include "device.h"
#include "layout.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_pipeline_cache, true, "MCLA/GPU",
    "Keep the pipelines the game asked for across sessions ("
    "24.35, M5.3): their keys go to cache/mcla_gpu_pipelines_<backend>.bin beside the "
    "executable on exit, and are read back at start. Off: nothing is read, recorded or written.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

namespace {

// The pipeline cache file: a header, the layouts, the records.
// Host byte order; any mismatch discards the whole file.
constexpr char kCacheMagic[8] = {'M', 'C', 'L', 'A', 'P', 'S', 'O', '\0'};
constexpr uint32_t kCacheVersion = 1;
constexpr uint32_t kMaxStoredElements = 64;
constexpr uint32_t kMaxSemanticName = 64;

struct CacheHeader {
  char magic[8] = {};
  uint32_t version = 0;
  uint32_t key_size = 0;
  uint32_t backend = 0;
  uint32_t layout_count = 0;
  uint32_t record_count = 0;
  uint32_t reserved = 0;
};
static_assert(sizeof(CacheHeader) == 32);

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

// Reads what the file holds, never past its end.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool Get(void* out, size_t n) {
    if (n > size_ - at_) {
      return false;
    }
    std::memcpy(out, data_ + at_, n);
    at_ += n;
    return true;
  }
  bool Get32(uint32_t* v) { return Get(v, 4); }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
};

using plume::RenderBlend;
using plume::RenderBlendOperation;
using plume::RenderComparisonFunction;
using plume::RenderStencilOp;

enum ReportKind : uint32_t {
  kReportConstantBlend = 1u << 0,
  kReportStencilBackFace = 1u << 1,
  kReportPointFill = 1u << 2,
  kReportEdramMode = 1u << 3,
  kReportColorGap = 1u << 4,
  kReportAlphaTestFunction = 1u << 6,
  kReportAlphaTestNoVariant = 1u << 7,
};

// The key packs these enums in 4-bit fields.
static_assert(uint32_t(RenderBlend::INV_BLEND_FACTOR) < 16, "blend factors used fit 4 bits");
static_assert(uint32_t(RenderBlendOperation::MAX) < 16);
static_assert(uint32_t(RenderComparisonFunction::ALWAYS) < 16);
static_assert(uint32_t(RenderStencilOp::DECREMENT_AND_WRAP) < 16);
static_assert(uint32_t(plume::RenderCullMode::BACK) < 16);
static_assert(uint32_t(plume::RenderFillMode::WIREFRAME) < 8);

RenderBlend Factor(xe::BlendFactor f, bool alpha, bool* constant) {
  switch (f) {
    case xe::BlendFactor::kZero: return RenderBlend::ZERO;
    case xe::BlendFactor::kOne: return RenderBlend::ONE;
    case xe::BlendFactor::kSrcColor: return alpha ? RenderBlend::SRC_ALPHA : RenderBlend::SRC_COLOR;
    case xe::BlendFactor::kOneMinusSrcColor:
      return alpha ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
    case xe::BlendFactor::kSrcAlpha: return RenderBlend::SRC_ALPHA;
    case xe::BlendFactor::kOneMinusSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
    case xe::BlendFactor::kDstColor: return alpha ? RenderBlend::DEST_ALPHA : RenderBlend::DEST_COLOR;
    case xe::BlendFactor::kOneMinusDstColor:
      return alpha ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
    case xe::BlendFactor::kDstAlpha: return RenderBlend::DEST_ALPHA;
    case xe::BlendFactor::kOneMinusDstAlpha: return RenderBlend::INV_DEST_ALPHA;
    case xe::BlendFactor::kConstantColor:
    case xe::BlendFactor::kConstantAlpha:
      *constant = true;
      return RenderBlend::BLEND_FACTOR;
    case xe::BlendFactor::kOneMinusConstantColor:
    case xe::BlendFactor::kOneMinusConstantAlpha:
      *constant = true;
      return RenderBlend::INV_BLEND_FACTOR;
    case xe::BlendFactor::kSrcAlphaSaturate: return RenderBlend::SRC_ALPHA_SAT;
  }
  return RenderBlend::ONE;
}

RenderBlendOperation Op(xe::BlendOp op) {
  switch (op) {
    case xe::BlendOp::kAdd: return RenderBlendOperation::ADD;
    case xe::BlendOp::kSubtract: return RenderBlendOperation::SUBTRACT;
    case xe::BlendOp::kMin: return RenderBlendOperation::MIN;
    case xe::BlendOp::kMax: return RenderBlendOperation::MAX;
    case xe::BlendOp::kRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
  }
  return RenderBlendOperation::ADD;
}

RenderComparisonFunction Compare(xe::CompareFunction f) {
  switch (f) {
    case xe::CompareFunction::kNever: return RenderComparisonFunction::NEVER;
    case xe::CompareFunction::kLess: return RenderComparisonFunction::LESS;
    case xe::CompareFunction::kEqual: return RenderComparisonFunction::EQUAL;
    case xe::CompareFunction::kLessEqual: return RenderComparisonFunction::LESS_EQUAL;
    case xe::CompareFunction::kGreater: return RenderComparisonFunction::GREATER;
    case xe::CompareFunction::kNotEqual: return RenderComparisonFunction::NOT_EQUAL;
    case xe::CompareFunction::kGreaterEqual: return RenderComparisonFunction::GREATER_EQUAL;
    case xe::CompareFunction::kAlways: return RenderComparisonFunction::ALWAYS;
  }
  return RenderComparisonFunction::ALWAYS;
}

RenderStencilOp StencilOp(xe::StencilOp op) {
  switch (op) {
    case xe::StencilOp::kKeep: return RenderStencilOp::KEEP;
    case xe::StencilOp::kZero: return RenderStencilOp::ZERO;
    case xe::StencilOp::kReplace: return RenderStencilOp::REPLACE;
    case xe::StencilOp::kIncrementClamp: return RenderStencilOp::INCREMENT_AND_CLAMP;
    case xe::StencilOp::kDecrementClamp: return RenderStencilOp::DECREMENT_AND_CLAMP;
    case xe::StencilOp::kInvert: return RenderStencilOp::INVERT;
    case xe::StencilOp::kIncrementWrap: return RenderStencilOp::INCREMENT_AND_WRAP;
    case xe::StencilOp::kDecrementWrap: return RenderStencilOp::DECREMENT_AND_WRAP;
  }
  return RenderStencilOp::KEEP;
}

// Xenia's GetD3D10IntegerPolygonOffset: the guest offset in depth-buffer units
// to the host's integer bias -- unorm24 x (2^24 - 1); float24 x 2^24 in steps of
// 8 (the float32 ULPs of one float24 ULP), with ceil so a used offset never
// rounds to none.
int32_t IntegerPolygonOffset(xe::DepthRenderTargetFormat format, float offset) {
  const bool float24 = format == xe::DepthRenderTargetFormat::kD24FS8;
  int32_t value = int32_t(std::ceil(std::abs(offset) * (float24 ? float(1u << 24) * (1.0f / 8.0f)
                                                                : float((1u << 24) - 1))));
  if (float24) {
    value <<= 3;
  }
  return offset < 0.0f ? -value : value;
}

// Blend field packing (4 bits each): enable, src, dst, op, srcA, dstA, opA, mask.
uint32_t PackBlend(const plume::RenderBlendDesc& b) {
  return uint32_t(b.blendEnabled) | (uint32_t(b.srcBlend) << 4) | (uint32_t(b.dstBlend) << 8) |
         (uint32_t(b.blendOp) << 12) | (uint32_t(b.srcBlendAlpha) << 16) |
         (uint32_t(b.dstBlendAlpha) << 20) | (uint32_t(b.blendOpAlpha) << 24) |
         (uint32_t(b.renderTargetWriteMask) << 28);
}

plume::RenderBlendDesc UnpackBlend(uint32_t v) {
  plume::RenderBlendDesc b;
  b.blendEnabled = (v & 0xF) != 0;
  b.srcBlend = RenderBlend((v >> 4) & 0xF);
  b.dstBlend = RenderBlend((v >> 8) & 0xF);
  b.blendOp = RenderBlendOperation((v >> 12) & 0xF);
  b.srcBlendAlpha = RenderBlend((v >> 16) & 0xF);
  b.dstBlendAlpha = RenderBlend((v >> 20) & 0xF);
  b.blendOpAlpha = RenderBlendOperation((v >> 24) & 0xF);
  b.renderTargetWriteMask = uint8_t((v >> 28) & 0xF);
  return b;
}

}  // namespace

size_t PipelineKeyHash::operator()(const PipelineKey& k) const {
  return size_t(XXH3_64bits(&k, sizeof(k)));
}

Pipelines::~Pipelines() {
  Shutdown();
}

void Pipelines::Initialize(Device& device, const GuestLayout& layout, ShaderStore& shaders) {
  device_ = &device;
  layout_ = &layout;
  shaders_ = &shaders;
  dynamic_depth_bias_ = device.capabilities().dynamicDepthBias;
  REXLOG_INFO("[gpu] pipelines: depth bias {}", dynamic_depth_bias_
                                                    ? "dynamic (set per draw on the command list)"
                                                    : "static (part of each pipeline)");
  persist_ = REXCVAR_GET(mcla_gpu_pipeline_cache);
  if (persist_) {
    // The pack is <executable>/assets/mcla_shaders.*.pack.
    std::string backend = BackendName();
    std::transform(backend.begin(), backend.end(), backend.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    cache_path_ = shaders.path().parent_path().parent_path() / "cache" /
                  std::format("mcla_gpu_pipelines_{}.bin", backend);
    LoadRecords();
    StartPrecache();
  }
}

void Pipelines::Shutdown() {
  StopPrecache();
  if (persist_ && shaders_) {
    SaveRecords();
  }
  pipelines_.clear();
  records_.clear();
  recorded_.clear();
  stored_layouts_.clear();
  stored_layout_index_.clear();
  session_layouts_.clear();
  names_.clear();
  loaded_records_ = 0;
  persist_ = false;
  device_ = nullptr;
  layout_ = nullptr;
  shaders_ = nullptr;
}

// ---------------------------------------------------------------------------
// persistence (M5.3a)
// ---------------------------------------------------------------------------

void Pipelines::SerializeLayout(const plume::RenderInputElement* elements, size_t element_count,
                                const plume::RenderInputSlot* slots, size_t slot_count,
                                std::vector<uint8_t>& out) {
  Put32(out, uint32_t(element_count));
  Put32(out, uint32_t(slot_count));
  for (size_t i = 0; i < element_count; ++i) {
    const plume::RenderInputElement& e = elements[i];
    const std::string_view name = e.semanticName ? e.semanticName : "";
    Put32(out, uint32_t(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    Put32(out, e.semanticIndex);
    Put32(out, e.location);
    Put32(out, uint32_t(e.format));
    Put32(out, e.slotIndex);
    Put32(out, e.alignedByteOffset);
  }
  for (size_t i = 0; i < slot_count; ++i) {
    Put32(out, slots[i].index);
    Put32(out, slots[i].stride);
    Put32(out, uint32_t(slots[i].classification));
  }
}

uint32_t Pipelines::StoreLayout(const InputLayout& layout) {
  const auto known = session_layouts_.find(layout.id);
  if (known != session_layouts_.end()) {
    return known->second;
  }
  std::vector<uint8_t> bytes;
  SerializeLayout(layout.elements.data(), layout.elements.size(), layout.slots.data(),
                  layout.slots.size(), bytes);
  const uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
  uint32_t index;
  const auto stored = stored_layout_index_.find(hash);
  if (stored != stored_layout_index_.end()) {
    index = stored->second;
    std::vector<uint8_t> other;
    const StoredLayout& s = *stored_layouts_[index];
    SerializeLayout(s.elements.data(), s.elements.size(), s.slots.data(), s.slots.size(), other);
    if (other != bytes) {
      // Two layouts with one hash: neither is persisted nor precached for this one.
      REXLOG_ERROR("[gpu] pipelines: two input layouts share the hash {:016X}; pipelines of the "
                   "second are neither recorded nor taken from the precache",
                   hash);
      session_layouts_.emplace(layout.id, kNoLayout);
      return kNoLayout;
    }
  } else {
    auto s = std::make_unique<StoredLayout>();
    s->hash = hash;
    s->slots = layout.slots;
    s->elements = layout.elements;
    for (plume::RenderInputElement& e : s->elements) {
      names_.emplace_back(e.semanticName ? e.semanticName : "");
      e.semanticName = names_.back().c_str();
    }
    index = uint32_t(stored_layouts_.size());
    stored_layouts_.push_back(std::move(s));
    stored_layout_index_.emplace(hash, index);
  }
  session_layouts_.emplace(layout.id, index);
  return index;
}

PipelineKey Pipelines::PersistentKey(const PipelineKey& key, uint32_t stored) const {
  const uint64_t hash = stored_layouts_[stored]->hash;
  PipelineKey persistent = key;
  persistent.input_layout = uint32_t(hash);
  persistent.reserved = uint32_t(hash >> 32);
  return persistent;
}

void Pipelines::RecordPipeline(const PipelineKey& key, const InputLayout& layout) {
  const uint32_t index = StoreLayout(layout);
  if (index == kNoLayout) {
    return;
  }
  const PipelineKey persistent = PersistentKey(key, index);
  if (recorded_.insert(persistent).second) {
    records_.push_back(Record{persistent, index});
  }
}

// ---------------------------------------------------------------------------
// precache (M5.3b)
// ---------------------------------------------------------------------------

void Pipelines::StartPrecache() {
  if (records_.empty()) {
    return;
  }
  precached_.reserve(records_.size());
  for (const Record& r : records_) {
    auto p = std::make_unique<Precached>();
    p->key = r.key;
    p->layout = stored_layouts_[r.layout].get();  // owned by a unique_ptr: stable
    precached_by_key_.emplace(r.key, p.get());
    precached_.push_back(std::move(p));
  }
  // A few threads beside the game's own: the guest boots meanwhile.
  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t count = std::clamp(hardware / 4, 1u, 3u);
  precache_start_ = std::chrono::steady_clock::now();
  stop_precache_ = false;
  next_precache_ = 0;
  workers_running_ = count;
  for (uint32_t i = 0; i < count; ++i) {
    workers_.emplace_back([this] { PrecacheWorker(); });
  }
  REXLOG_INFO("[gpu] pipelines: precaching {} pipelines on {} threads",
              precached_.size(), count);
}

void Pipelines::StopPrecache() {
  stop_precache_ = true;
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  precached_by_key_.clear();
  precached_.clear();  // the GPU is idle: unclaimed pipelines die here
  aliases_.clear();
}

void Pipelines::PrecacheWorker() {
  while (!stop_precache_.load(std::memory_order_relaxed)) {
    const size_t i = next_precache_.fetch_add(1);
    if (i >= precached_.size()) {
      break;
    }
    Precached& p = *precached_[i];
    uint8_t expected = kQueued;
    if (!p.state.compare_exchange_strong(expected, kBuilding)) {
      continue;  // the render thread claimed it
    }
    plume::RenderGraphicsPipelineDesc desc;
    ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
    ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    if (BuildDesc(p.key, shaders_->Find(p.key.vs), p.key.ps ? shaders_->Find(p.key.ps) : nullptr,
                  *p.layout, desc, &vs_result, &ps_result)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "precached guest pipeline");
    }
    const bool made = pipeline != nullptr;
    {
      // The render thread may take it the moment the state says so.
      std::lock_guard<std::mutex> lock(precache_mutex_);
      p.pipeline = std::move(pipeline);
      p.state = made ? kReady : kFailed;
    }
    ++(made ? precache_ready_ : precache_failed_);
    precache_done_.notify_all();
  }
  if (--workers_running_ == 0 && !stop_precache_) {
    REXLOG_INFO("[gpu] pipelines: precache done in {:.1f} ms: {} ready, {} failed",
                double(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - precache_start_)
                           .count()) /
                    1000.0,
                precache_ready_.load(), precache_failed_.load());
  }
}

plume::RenderPipeline* Pipelines::TakePrecached(const PipelineKey& key,
                                                const PipelineInputs& inputs) {
  if (precached_by_key_.empty() || !inputs.input_layout) {
    return nullptr;
  }
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return nullptr;
  }
  const auto found = precached_by_key_.find(PersistentKey(key, stored));
  if (found == precached_by_key_.end()) {
    return nullptr;
  }
  Precached& p = *found->second;
  if (p.taken) {
    // Another session key with the same persistent key went first: its
    // pipeline is this one's too.
    const auto first = pipelines_.find(p.taken_as);
    if (first == pipelines_.end() || !first->second) {
      return nullptr;
    }
    aliases_.emplace(key, first->second.get());
    ++stats_.shared;
    return first->second.get();
  }
  uint8_t state = kQueued;
  if (p.state.compare_exchange_strong(state, kClaimed)) {
    ++stats_.precache_claimed;  // no worker at it yet: the caller makes it now
    p.taken = true;
    p.taken_as = key;
    return nullptr;
  }
  if (state == kBuilding) {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(precache_mutex_);
    precache_done_.wait(lock, [&] { return p.state.load() != kBuilding; });
    ++stats_.precache_waits;
    stats_.precache_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count());
  }
  std::lock_guard<std::mutex> lock(precache_mutex_);
  if (p.state.load() != kReady) {
    return nullptr;  // failed: made (and reported) on the caller's path
  }
  p.state = kClaimed;
  p.taken = true;
  p.taken_as = key;
  plume::RenderPipeline* raw = p.pipeline.get();
  pipelines_.emplace(key, std::move(p.pipeline));
  ++stats_.precache_taken;
  return raw;
}

void Pipelines::LoadRecords() {
  std::ifstream file(cache_path_, std::ios::binary);
  if (!file) {
    REXLOG_INFO("[gpu] pipelines: no cache at {} yet", cache_path_.string());
    return;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  Reader in(data.data(), data.size());
  CacheHeader header;
  const auto discard = [&](const char* why) {
    REXLOG_ERROR("[gpu] pipelines: the cache {} is discarded: {}",
                 cache_path_.string(), why);
    records_.clear();
    recorded_.clear();
    stored_layouts_.clear();
    stored_layout_index_.clear();
    names_.clear();
  };
  if (!in.Get(&header, sizeof(header)) ||
      std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0) {
    return discard("not a pipeline cache");
  }
  if (header.version != kCacheVersion || header.key_size != sizeof(PipelineKey) ||
      header.backend != uint32_t(ActiveBackend())) {
    return discard("written by another version or backend");
  }
  std::vector<uint32_t> layout_of_file;  // file layout index -> stored_layouts_
  for (uint32_t i = 0; i < header.layout_count; ++i) {
    uint64_t hash = 0;
    uint32_t element_count = 0;
    uint32_t slot_count = 0;
    if (!in.Get(&hash, 8) || !in.Get32(&element_count) || !in.Get32(&slot_count) ||
        element_count > kMaxStoredElements || slot_count > kMaxStoredElements) {
      return discard("a layout is cut short or too large");
    }
    auto s = std::make_unique<StoredLayout>();
    s->hash = hash;
    s->elements.resize(element_count);
    s->slots.resize(slot_count);
    for (plume::RenderInputElement& e : s->elements) {
      uint32_t name_size = 0;
      char name[kMaxSemanticName] = {};
      uint32_t format = 0;
      if (!in.Get32(&name_size) || name_size >= kMaxSemanticName || !in.Get(name, name_size) ||
          !in.Get32(&e.semanticIndex) || !in.Get32(&e.location) || !in.Get32(&format) ||
          !in.Get32(&e.slotIndex) || !in.Get32(&e.alignedByteOffset)) {
        return discard("an input element is cut short");
      }
      names_.emplace_back(name, name_size);
      e.semanticName = names_.back().c_str();
      e.format = plume::RenderFormat(format);
    }
    for (plume::RenderInputSlot& slot : s->slots) {
      uint32_t classification = 0;
      if (!in.Get32(&slot.index) || !in.Get32(&slot.stride) || !in.Get32(&classification)) {
        return discard("an input slot is cut short");
      }
      slot.classification = plume::RenderInputSlotClassification(classification);
    }
    // The hash is the content's: a layout whose bytes do not give it is not
    // the one the records were made with.
    std::vector<uint8_t> bytes;
    SerializeLayout(s->elements.data(), s->elements.size(), s->slots.data(), s->slots.size(),
                    bytes);
    if (XXH3_64bits(bytes.data(), bytes.size()) != hash) {
      return discard("a layout does not match its hash");
    }
    const auto known = stored_layout_index_.find(hash);
    if (known != stored_layout_index_.end()) {
      layout_of_file.push_back(known->second);
      continue;
    }
    layout_of_file.push_back(uint32_t(stored_layouts_.size()));
    stored_layout_index_.emplace(hash, uint32_t(stored_layouts_.size()));
    stored_layouts_.push_back(std::move(s));
  }
  uint32_t dropped = 0;
  for (uint32_t i = 0; i < header.record_count; ++i) {
    Record r;
    uint32_t layout = 0;
    if (!in.Get(&r.key, sizeof(r.key)) || !in.Get32(&layout) || layout >= layout_of_file.size()) {
      return discard("a record is cut short or names no layout");
    }
    r.layout = layout_of_file[layout];
    // A shader the pack no longer has: the pipeline cannot be made.
    if (!shaders_->Find(r.key.vs) || (r.key.ps != 0 && !shaders_->Find(r.key.ps))) {
      ++dropped;
      continue;
    }
    if (recorded_.insert(r.key).second) {
      records_.push_back(r);
    }
  }
  loaded_records_ = records_.size();
  REXLOG_INFO("[gpu] pipelines: {} records and {} layouts read from {} ({} dropped: a shader "
              "the pack does not have)",
              records_.size(), stored_layouts_.size(), cache_path_.string(), dropped);
}

void Pipelines::SaveRecords() {
  if (records_.size() == loaded_records_) {
    return;  // nothing new: the file already holds them
  }
  CacheHeader header;
  std::memcpy(header.magic, kCacheMagic, sizeof(kCacheMagic));
  header.version = kCacheVersion;
  header.key_size = sizeof(PipelineKey);
  header.backend = uint32_t(ActiveBackend());
  header.layout_count = uint32_t(stored_layouts_.size());
  header.record_count = uint32_t(records_.size());
  std::vector<uint8_t> out(sizeof(header));
  std::memcpy(out.data(), &header, sizeof(header));
  for (const auto& s : stored_layouts_) {
    const uint8_t* hash = reinterpret_cast<const uint8_t*>(&s->hash);
    out.insert(out.end(), hash, hash + 8);
    SerializeLayout(s->elements.data(), s->elements.size(), s->slots.data(), s->slots.size(),
                    out);
  }
  for (const Record& r : records_) {
    const uint8_t* key = reinterpret_cast<const uint8_t*>(&r.key);
    out.insert(out.end(), key, key + sizeof(r.key));
    Put32(out, r.layout);
  }
  std::error_code ec;
  std::filesystem::create_directories(cache_path_.parent_path(), ec);
  std::filesystem::path temp = cache_path_;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!file) {
      REXLOG_ERROR("[gpu] pipelines: the cache {} could not be written",
                   temp.string());
      return;
    }
  }
  std::filesystem::rename(temp, cache_path_, ec);
  if (ec) {
    REXLOG_ERROR("[gpu] pipelines: the cache {} could not replace the old one: {}",
                 cache_path_.string(), ec.message());
    return;
  }
  REXLOG_INFO("[gpu] pipelines: {} records ({} new this session) and {} layouts written to {}",
              records_.size(), records_.size() - loaded_records_, stored_layouts_.size(),
              cache_path_.string());
}

void Pipelines::Report(uint32_t kind, const char* what) {
  if (reported_kinds_ & kind) {
    return;
  }
  reported_kinds_ |= kind;
  REXLOG_ERROR("[gpu] pipelines: {} (reported once)", what);
}

KeyResult Pipelines::BuildKey(const XenosState& state, const PipelineInputs& inputs,
                              plume::RenderPrimitiveTopology topology, bool polygonal,
                              const plume::RenderFormat color_formats[kColorTargetCount],
                              plume::RenderFormat depth_format, PipelineKey& key,
                              DepthBias& bias) {
  key = PipelineKey{};
  bias = DepthBias{};
  const ShaderStore::Entry* vs = inputs.vs;
  const ShaderStore::Entry* ps = inputs.ps;
  const xe::EdramMode mode = state.mode.edram_mode;
  if (mode != xe::EdramMode::kColorDepth && mode != xe::EdramMode::kDepthOnly) {
    Report(kReportEdramMode, "a draw with an EDRAM mode other than colour+depth or depth only");
    return KeyResult::kEdramMode;
  }
  const bool depth_only = mode == xe::EdramMode::kDepthOnly;
  // Xenia's reading of kDepthOnly: the pixel shader is not run. In MCLA every
  // such draw has no pixel shader bound anyway (frame census 10/10).
  if (depth_only) {
    ps = nullptr;
  }

  key.vs = vs->key;
  key.ps = ps ? ps->key : 0;
  key.vs_variant = (vs->spec_mask != 0 && inputs.input_layout->packed_normal) ? 1 : 0;
  if (ps && (ps->ps_outputs & ShaderStore::kOutputColor0) != 0) {
    const AlphaTest alpha_test = ComputeAlphaTest(state);
    if (alpha_test.unsupported) {
      Report(kReportAlphaTestFunction,
             "an alpha test function that is not a lower bound (LESS, EQUAL, LESS_EQUAL, or "
             "NOT_EQUAL with a reference other than 0); nothing is discarded");
    }
    // Variant 1 (ALPHA_TEST) is the one whose coverage depends on alpha: the
    // alpha test, alpha to coverage, or both (Xenia's DoesCoverageDependOnAlpha;
    // contract v6). Variant 0 has no discard and no SV_Coverage of its own, so
    // the host keeps its early depth/stencil test.
    if (alpha_test.discard || state.color_control.alpha_to_mask_enable) {
      if (ps->spec_mask != 0) {
        key.ps_variant = 1;
      } else {
        Report(kReportAlphaTestNoVariant,
               "alpha test or alpha to coverage on a pixel shader compiled without the ALPHA_TEST "
               "variant; nothing is discarded and every sample is covered");
      }
    }
  }
  key.input_layout = inputs.input_layout->id;
  key.topology = uint32_t(topology);
  key.sample_count = MsaaSampleCount(state.msaa);

  // Colour targets: contiguous from 0, the framebuffer binds them in order.
  const uint32_t ps_outputs = ps ? ps->ps_outputs : 0;
  uint32_t color_count = 0;
  if (!depth_only) {
    for (uint32_t i = 0; i < kColorTargetCount; ++i) {
      if (state.color[i].bound) {
        color_count = i + 1;
      }
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      if (!state.color[i].bound) {
        Report(kReportColorGap, "colour targets with a gap below a bound one");
        return KeyResult::kColorGap;
      }
    }
  }
  key.color_count = color_count;
  bool constant_blend = false;
  for (uint32_t i = 0; i < color_count; ++i) {
    const ColorTargetState& t = state.color[i];
    key.color_format[i] = uint32_t(color_formats[i]);
    plume::RenderBlendDesc b = plume::RenderBlendDesc::Copy();
    const bool written = (ps_outputs & (1u << i)) != 0;
    b.renderTargetWriteMask = written ? uint8_t(t.write_mask) : 0;
    if (b.renderTargetWriteMask != 0) {
      const xreg::RB_BLENDCONTROL& bc = t.blend;
      b.blendOp = Op(bc.color_comb_fcn);
      b.blendOpAlpha = Op(bc.alpha_comb_fcn);
      const bool minmax_color =
          b.blendOp == RenderBlendOperation::MIN || b.blendOp == RenderBlendOperation::MAX;
      const bool minmax_alpha =
          b.blendOpAlpha == RenderBlendOperation::MIN || b.blendOpAlpha == RenderBlendOperation::MAX;
      b.srcBlend = minmax_color ? RenderBlend::ONE : Factor(bc.color_srcblend, false, &constant_blend);
      b.dstBlend = minmax_color ? RenderBlend::ONE : Factor(bc.color_destblend, false, &constant_blend);
      b.srcBlendAlpha =
          minmax_alpha ? RenderBlend::ONE : Factor(bc.alpha_srcblend, true, &constant_blend);
      b.dstBlendAlpha =
          minmax_alpha ? RenderBlend::ONE : Factor(bc.alpha_destblend, true, &constant_blend);
      const bool copy = b.srcBlend == RenderBlend::ONE && b.dstBlend == RenderBlend::ZERO &&
                        b.blendOp == RenderBlendOperation::ADD &&
                        b.srcBlendAlpha == RenderBlend::ONE &&
                        b.dstBlendAlpha == RenderBlend::ZERO &&
                        b.blendOpAlpha == RenderBlendOperation::ADD;
      b.blendEnabled = !copy;
    }
    key.blend[i] = PackBlend(b);
  }
  if (constant_blend) {
    Report(kReportConstantBlend,
           "a blend with the constant colour/alpha factor (Plume has no dynamic blend factor; the "
           "host default factor is used)");
  }

  if (state.depth_bound) {
    key.depth_format = uint32_t(depth_format);
    const xreg::RB_DEPTHCONTROL& dc = state.depth_control;
    // Z_ENABLE off turns off the write too (D3D9 semantics, as Xenia reads it).
    if (dc.z_enable) {
      key.depth = 1u | (uint32_t(dc.z_write_enable) << 1) | (uint32_t(Compare(dc.zfunc)) << 2);
    }
    if (dc.stencil_enable) {
      const xe::CompareFunction func_bf = dc.backface_enable ? dc.stencilfunc_bf : dc.stencilfunc;
      const xe::StencilOp fail_bf = dc.backface_enable ? dc.stencilfail_bf : dc.stencilfail;
      const xe::StencilOp zpass_bf = dc.backface_enable ? dc.stencilzpass_bf : dc.stencilzpass;
      const xe::StencilOp zfail_bf = dc.backface_enable ? dc.stencilzfail_bf : dc.stencilzfail;
      key.stencil = 1u | (uint32_t(Compare(dc.stencilfunc)) << 1) |
                    (uint32_t(StencilOp(dc.stencilfail)) << 5) |
                    (uint32_t(StencilOp(dc.stencilzpass)) << 9) |
                    (uint32_t(StencilOp(dc.stencilzfail)) << 13) |
                    (uint32_t(Compare(func_bf)) << 17) | (uint32_t(StencilOp(fail_bf)) << 21) |
                    (uint32_t(StencilOp(zpass_bf)) << 25);
      key.stencil_masks = uint32_t(state.stencil.stencilmask) |
                          (uint32_t(state.stencil.stencilwritemask) << 8) |
                          (uint32_t(state.stencil.stencilref) << 16) |
                          (uint32_t(StencilOp(zfail_bf)) << 24);
      if (dc.backface_enable && (state.stencil_bf.stencilmask != state.stencil.stencilmask ||
                                 state.stencil_bf.stencilwritemask !=
                                     state.stencil.stencilwritemask ||
                                 state.stencil_bf.stencilref != state.stencil.stencilref)) {
        Report(kReportStencilBackFace,
               "back-face stencil masks or reference different from the front (one set in a "
               "pipeline; the front ones are used)");
      }
    }
  }

  // Raster state. Only polygonal primitives are culled, filled by mode and
  // offset by face (Xenia): a rectangle list is drawn as triangles on the host
  // but is none of these.
  const xreg::PA_SU_SC_MODE_CNTL& sc = state.sc_mode;
  plume::RenderCullMode cull = plume::RenderCullMode::NONE;
  if (polygonal) {
    if (sc.cull_front && sc.cull_back) {
      return KeyResult::kNoEffect;
    }
    cull = sc.cull_front ? plume::RenderCullMode::FRONT
                         : sc.cull_back ? plume::RenderCullMode::BACK : plume::RenderCullMode::NONE;
  }
  // `face`: 0 = front is counter-clockwise, 1 = clockwise.
  const bool front_ccw = sc.face == 0;
  plume::RenderFillMode fill = plume::RenderFillMode::SOLID;
  if (polygonal && sc.poly_mode == xe::PolygonModeEnable::kDualMode) {
    const xe::PolygonType type =
        sc.cull_front ? sc.polymode_back_ptype : sc.polymode_front_ptype;
    if (type == xe::PolygonType::kLines) {
      fill = plume::RenderFillMode::WIREFRAME;
    } else if (type == xe::PolygonType::kPoints) {
      Report(kReportPointFill, "point fill mode (the host has solid and wireframe only)");
    }
  }
  // Polygon offset for the face that is drawn (Xenia GetPreferredFacePolygonOffset).
  float poly_scale = 0.0f;
  float poly_offset = 0.0f;
  if (state.depth_bound) {
    if (polygonal) {
      if (!sc.cull_front && sc.poly_offset_front_enable) {
        poly_scale = state.poly_offset[0];
        poly_offset = state.poly_offset[1];
      } else if (!sc.cull_back && sc.poly_offset_back_enable) {
        poly_scale = state.poly_offset[2];
        poly_offset = state.poly_offset[3];
      }
    } else if (sc.poly_offset_para_enable) {
      poly_scale = state.poly_offset[0];
      poly_offset = state.poly_offset[1];
    }
  }
  if (poly_scale != 0.0f || poly_offset != 0.0f) {
    bias.constant = IntegerPolygonOffset(state.depth_format, poly_offset);
    bias.slope = poly_scale * xe::kPolygonOffsetScaleSubpixelUnit;
    if (!dynamic_depth_bias_) {
      key.depth_bias = bias.constant;
      key.slope_scaled_depth_bias = bias.slope;
    }
  }
  const bool depth_clip = !state.clip.clip_disable;
  // Alpha to coverage is the pixel shader's (contract v6: its ALPHA_TEST
  // variant, constants.h AlphaToMaskParameters): the host's would test the
  // alpha after the exponent bias, never the one the hardware tests.
  key.raster = uint32_t(cull) | (uint32_t(front_ccw) << 4) | (uint32_t(fill) << 5) |
               (uint32_t(depth_clip) << 8);
  return KeyResult::kOk;
}

bool Pipelines::BuildDesc(const PipelineKey& key, const ShaderStore::Entry* vs,
                          const ShaderStore::Entry* ps, const StoredLayout& layout,
                          plume::RenderGraphicsPipelineDesc& desc,
                          ShaderStore::ShaderResult* vs_result,
                          ShaderStore::ShaderResult* ps_result) {
  desc.pipelineLayout = layout_->layout();
  desc.vertexShader = vs ? shaders_->GetShader(*vs, key.vs_variant, vs_result) : nullptr;
  if (key.ps != 0) {
    desc.pixelShader = ps ? shaders_->GetShader(*ps, key.ps_variant, ps_result) : nullptr;
  }
  // No specialization constants: each variant is its own blob on both backends
  // (shaders.h, contract v6).
  if (!desc.vertexShader || (key.ps != 0 && !desc.pixelShader)) {
    return false;
  }
  desc.inputSlots = layout.slots.data();
  desc.inputSlotsCount = uint32_t(layout.slots.size());
  desc.inputElements = layout.elements.data();
  desc.inputElementsCount = uint32_t(layout.elements.size());
  desc.primitiveTopology = plume::RenderPrimitiveTopology(key.topology);
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(key.sample_count));
  desc.renderTargetCount = key.color_count;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    desc.renderTargetFormat[i] = plume::RenderFormat(key.color_format[i]);
    desc.renderTargetBlend[i] = UnpackBlend(key.blend[i]);
  }
  desc.depthTargetFormat = plume::RenderFormat(key.depth_format);
  desc.depthEnabled = (key.depth & 1u) != 0;
  desc.depthWriteEnabled = (key.depth & 2u) != 0;
  desc.depthFunction =
      desc.depthEnabled ? RenderComparisonFunction((key.depth >> 2) & 0xF)
                        : RenderComparisonFunction::ALWAYS;
  desc.stencilEnabled = (key.stencil & 1u) != 0;
  if (desc.stencilEnabled) {
    desc.stencilFrontFace.compareFunction = RenderComparisonFunction((key.stencil >> 1) & 0xF);
    desc.stencilFrontFace.failOp = RenderStencilOp((key.stencil >> 5) & 0xF);
    desc.stencilFrontFace.passOp = RenderStencilOp((key.stencil >> 9) & 0xF);
    desc.stencilFrontFace.depthFailOp = RenderStencilOp((key.stencil >> 13) & 0xF);
    desc.stencilBackFace.compareFunction = RenderComparisonFunction((key.stencil >> 17) & 0xF);
    desc.stencilBackFace.failOp = RenderStencilOp((key.stencil >> 21) & 0xF);
    desc.stencilBackFace.passOp = RenderStencilOp((key.stencil >> 25) & 0xF);
    desc.stencilBackFace.depthFailOp = RenderStencilOp((key.stencil_masks >> 24) & 0xF);
    desc.stencilReadMask = key.stencil_masks & 0xFFu;
    desc.stencilWriteMask = (key.stencil_masks >> 8) & 0xFFu;
    desc.stencilReference = (key.stencil_masks >> 16) & 0xFFu;
  }
  desc.cullMode = plume::RenderCullMode(key.raster & 0xF);
  desc.frontFace = ((key.raster >> 4) & 1u) ? plume::RenderFrontFace::COUNTER_CLOCKWISE
                                            : plume::RenderFrontFace::CLOCKWISE;
  desc.fillMode = plume::RenderFillMode((key.raster >> 5) & 0x7);
  desc.depthClipEnabled = ((key.raster >> 8) & 1u) != 0;
  desc.alphaToCoverageEnabled = false;  // the shader's (contract v6)
  desc.dynamicDepthBiasEnabled = dynamic_depth_bias_;
  desc.depthBias = key.depth_bias;
  desc.slopeScaledDepthBias = key.slope_scaled_depth_bias;
  return true;
}

plume::RenderPipeline* Pipelines::Get(const PipelineKey& key, const PipelineInputs& inputs) {
  const auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.get();
  }
  if (const auto alias = aliases_.find(key); alias != aliases_.end()) {
    return alias->second;
  }
  if (plume::RenderPipeline* precached = TakePrecached(key, inputs)) {
    return precached;
  }
  const auto t0 = std::chrono::steady_clock::now();
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  StoredLayout layout;  // the session layout's arrays, copied
  layout.elements = inputs.input_layout->elements;
  layout.slots = inputs.input_layout->slots;
  plume::RenderGraphicsPipelineDesc desc;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(key, inputs.vs, key.ps != 0 ? inputs.ps : nullptr, layout, desc, &vs_result,
                &ps_result)) {
    pipeline = device_->CreateGraphicsPipeline(desc, "guest pipeline");
  }
  stats_.create_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
  if (pipeline) {
    ++stats_.created;
    if (persist_) {
      RecordPipeline(key, *inputs.input_layout);
    }
  } else {
    ++stats_.failed;
    if (stats_.failed <= 16) {
      REXLOG_ERROR("[gpu] pipeline for vs {:016X}/{} ps {:016X}/{} not created (vs shader {}, ps "
                   "shader {}, {} colour targets, depth format {})",
                   key.vs, key.vs_variant, key.ps, key.ps_variant,
                   ShaderStore::ResultName(vs_result), ShaderStore::ResultName(ps_result),
                   key.color_count, key.depth_format);
    }
  }
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

Pipelines::Stats Pipelines::TakeStats() {
  Stats s = stats_;
  stats_ = Stats{};
  return s;
}

}  // namespace mcla::gpu
