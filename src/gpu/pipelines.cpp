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
#include <set>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "backend.h"
#include "constants.h"
#include "device.h"
#include "format.h"
#include "layout.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(
    mcla_gpu_skip_shaders, "", "MCLA/GPU",
    "Workaround for a driver that dies compiling one particular shader combination: a "
    "comma-separated list of shader identities (the 16 hex digits of the pack's keys). A "
    "guest pipeline that uses any of them is not built, and its draws are treated like those "
    "of a pipeline that failed. Measured on Mesa's dzn (Vulkan over D3D12 in WSL2), which "
    "overflows its stack in vkCreateGraphicsPipelines for the pair b000e9d48f010bd8 + "
    "68387d4a0b431769 (each alone is fine).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
// True when the pipeline uses a shader named by mcla_gpu_skip_shaders.
bool SkipShaders(uint64_t vs, uint64_t ps) {
  static const std::set<uint64_t> keys = [] {
    std::set<uint64_t> out;
    std::stringstream list(REXCVAR_GET(mcla_gpu_skip_shaders));
    std::string item;
    while (std::getline(list, item, ',')) {
      if (!item.empty()) {
        out.insert(std::strtoull(item.c_str(), nullptr, 16));
      }
    }
    return out;
  }();
  return !keys.empty() && (keys.count(vs) != 0 || (ps != 0 && keys.count(ps) != 0));
}
}  // namespace

REXCVAR_DEFINE_BOOL(
    mcla_gpu_pipeline_cache, true, "MCLA/GPU",
    "Keep the pipelines the game asked for across sessions ("
    "24.35, M5.3): their keys go to cache/mcla_gpu_pipelines_<backend>.bin beside the "
    "executable on exit, and are read back at start -- the other backend's file too, so what one "
    "API saw is ready in the other (24.40 V2). Off: nothing is read, recorded or written.")
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

// PackBlend and UnpackBlend (the blend words of a key) are in pipelines.h: the UI layer split reads
// and changes them (frame_layers.h).

// Whether the draw that missed may be left out for now (Pipelines::Get).
bool MaySkip(const Deferral* deferral) {
  return deferral && deferral->may_skip && deferral->may_skip(deferral->context);
}

// Boot mode (pipelines.h): the processor count below which it runs, the longest it lasts, and
// how far the cursor must have to go for a claim to move it (a shorter jump gains nothing).
constexpr uint32_t kBootBoostBelow = 8;
constexpr std::chrono::seconds kBootSeconds{40};
constexpr size_t kFollowMinGap = 8;

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
  // Where kD24S8 is kept as D32 float: the guest's constant depth offset counts 2^-24 steps,
  // and a float depth buffer counts steps of 2^(e-23) for depth near 2^e. The shadow atlas
  // holds depth of about 1e-4 to 4e-4 (e is -14 to -12), which asks for 2^11 to 2^13.
  constexpr uint32_t kDepth24BiasShift = 12;
  depth24_bias_shift_ = format::Depth24Unsupported() ? kDepth24BiasShift : 0u;
  REXLOG_INFO("[gpu] pipelines: depth bias {}", dynamic_depth_bias_
                                                    ? "dynamic (set per draw on the command list)"
                                                    : "static (part of each pipeline)");
}

void Pipelines::StartPersistence(ShaderStore& shaders) {
  if (!device_ || persistence_started_) {
    return;
  }
  persistence_started_ = true;
  shaders_ = &shaders;
  // persist_ is set last: the render thread's Autosave (EndFrame) reads it first, and then finds
  // the records below in place.
  const bool persist = REXCVAR_GET(mcla_gpu_pipeline_cache);
  if (persist) {
    // The cache is <executable>/cache/mcla_gpu_pipelines_<api>.bin and the starter list
    // <executable>/assets/mcla_pipelines.bin, whichever folder the shader pack came from.
    const std::filesystem::path exe_folder = rex::filesystem::GetExecutableFolder();
    std::string backend = BackendName();
    std::transform(backend.begin(), backend.end(), backend.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    cache_path_ = exe_folder / "cache" / std::format("mcla_gpu_pipelines_{}.bin", backend);
    std::string other = BackendName(ActiveBackend() == Backend::kD3D12 ? Backend::kVulkan
                                                                       : Backend::kD3D12);
    std::transform(other.begin(), other.end(), other.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    other_cache_path_ = cache_path_.parent_path() / std::format("mcla_gpu_pipelines_{}.bin", other);
    starter_path_ = exe_folder / "assets" / "mcla_pipelines.bin";
    LoadRecords();
  }
  // The workers run without a cache file too: they make what the render
  // thread hands them (background builds).
  StartPrecache();
  persist_ = persist;
}

void Pipelines::Shutdown() {
  StopPrecache();
  JoinAutosave();
  if (persist_ && shaders_) {
    SaveRecords();
  }
  pipelines_.clear();
  level_dependent_.clear();
  level_dependent_index_.clear();
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  records_.clear();
  recorded_.clear();
  stored_layouts_.clear();
  stored_layout_index_.clear();
  session_layouts_.clear();
  names_.clear();
  loaded_records_ = 0;
  own_file_records_ = 0;
  frame_ = 0;
  persist_ = false;
  persistence_started_ = false;
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
  RecordStored(key, StoreLayout(layout));
}

void Pipelines::RecordStored(const PipelineKey& key, uint32_t index) {
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

// The logical processors this process may run on: its affinity mask where
// there is one (a game limited to some cores has only those), else the system's.
uint32_t Pipelines::UsableProcessors() {
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
    return uint32_t(__builtin_popcountll(process));
  }
#endif
  return std::max(1u, std::thread::hardware_concurrency());
}

void Pipelines::StartPrecache() {
  precached_.reserve(records_.size());
  for (const Record& r : records_) {
    auto p = std::make_unique<Precached>();
    p->key = r.key;
    p->layout = stored_layouts_[r.layout].get();  // owned by a unique_ptr: stable
    p->index = uint32_t(precached_.size());
    precached_by_key_.emplace(r.key, p.get());
    precached_.push_back(std::move(p));
  }
  // Half the processors, below normal priority: the guest boots meanwhile, and
  // a pipeline made here is one the render thread does not stop for later.
  // The workers run with an empty list too, and stay when it is done: they
  // make the pipelines the render thread hands them (background builds).
  // With fewer than kBootBoostBelow processors the first seconds are different
  // (boot mode, pipelines.h): up to all the processors but one, at normal priority.
  const uint32_t usable = UsableProcessors();
  steady_workers_ = std::clamp(usable / 2, 1u, 6u);
  const bool boot = !precached_.empty() && usable < kBootBoostBelow;
  const uint32_t count =
      boot ? std::clamp(usable > 1 ? usable - 1 : 1u, steady_workers_, 6u) : steady_workers_;
  precache_start_ = std::chrono::steady_clock::now();
  stop_precache_ = false;
  list_cursor_ = 0;
  list_end_ = precached_.size();
  list_skipped_.clear();
  precache_ready_ = 0;
  precache_failed_ = 0;
  precache_finished_ = 0;
  boot_claims_ = 0;
  boot_waits_ = 0;
  boot_deadline_ = precache_start_ + kBootSeconds;
  boot_mode_ = boot;
  for (uint32_t i = 0; i < count; ++i) {
    workers_.emplace_back([this, i] { PrecacheWorker(i); });
  }
  if (precached_.empty()) {
    REXLOG_INFO("[gpu] pipelines: nothing to precache; {} threads for background builds", count);
  } else if (boot) {
    REXLOG_INFO("[gpu] pipelines: precaching {} pipelines on {} threads; boot mode on {} usable "
                "processors: normal priority until the list is done or {} s have passed, then {} "
                "threads at below normal",
                precached_.size(), count, usable, kBootSeconds.count(), steady_workers_);
  } else {
    REXLOG_INFO("[gpu] pipelines: precaching {} pipelines on {} threads",
                precached_.size(), count);
  }
}

void Pipelines::StopPrecache() {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    stop_precache_ = true;
  }
  work_cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  boot_mode_ = false;
  // The workers are gone: a request that finished is taken (and recorded, so
  // the next session has it); one still queued is dropped.
  TakeFinishedRequests();
  requests_.clear();
  urgent_.clear();
  low_.clear();
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  list_skipped_.clear();
  precached_by_key_.clear();
  precached_.clear();  // the GPU is idle: unclaimed pipelines die here
  aliases_.clear();
}

void Pipelines::PrecacheWorker(uint32_t index) {
#if defined(_WIN32)
  bool priority_set = false;
  bool priority_boosted = false;
#endif
  for (;;) {
    // What the render thread handed over comes first, then the next entry of
    // the list; with neither the worker sleeps. The workers above
    // steady_workers_ are the boot mode's: they leave when it is over.
    Urgent urgent;
    size_t entry = 0;
    bool from_list = false;
    {
      std::unique_lock<std::mutex> lock(work_mutex_);
      const auto leaves = [&] { return index >= steady_workers_ && !boot_mode_.load(); };
      work_cv_.wait(lock, [&] {
        return stop_precache_.load() || leaves() || !urgent_.empty() || !low_.empty() ||
               HasListWork();
      });
      if (stop_precache_.load()) {
        return;
      }
      if (leaves()) {
        if (!urgent_.empty() || !low_.empty() || HasListWork()) {
          work_cv_.notify_one();  // what woke this one goes to a worker that stays
        }
        return;
      }
      if (!urgent_.empty()) {
        urgent = std::move(urgent_.front());
        urgent_.pop_front();
      } else if (!low_.empty()) {
        // The copies for a coming MSAA level: nothing a draw waits for, and not the boot list.
        urgent = std::move(low_.front());
        low_.pop_front();
      } else {
        from_list = NextListEntry(entry);
      }
    }
#if defined(_WIN32)
    // Normal priority in boot mode, below normal after: the guest's threads come first once
    // the render thread no longer waits for the list.
    const bool boosted = boot_mode_.load();
    if (!priority_set || boosted != priority_boosted) {
      SetThreadPriority(GetCurrentThread(),
                        boosted ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
      priority_set = true;
      priority_boosted = boosted;
    }
#endif
    if (urgent.request) {
      BuildRequest(*urgent.request);
    } else if (urgent.precached) {
      BuildPrecached(*urgent.precached);
    } else if (from_list) {
      BuildPrecached(*precached_[entry]);
    }
  }
}

bool Pipelines::HasListWork() const {
  return list_cursor_ < list_end_ || !list_skipped_.empty();
}

bool Pipelines::NextListEntry(size_t& index) {
  for (;;) {
    while (list_cursor_ >= list_end_) {
      if (list_skipped_.empty()) {
        return false;
      }
      list_cursor_ = list_skipped_.front().begin;
      list_end_ = list_skipped_.front().end;
      list_skipped_.pop_front();
    }
    const size_t i = list_cursor_++;
    // One the render thread made or a worker has is left alone.
    if (precached_[i]->state.load() == kQueued) {
      index = i;
      return true;
    }
  }
}

void Pipelines::NoteClaim(size_t index) {
  if (!boot_mode_.load()) {
    return;
  }
  ++boot_claims_;
  FollowRenderThread(index);
}

void Pipelines::FollowRenderThread(size_t index) {
  // The render thread had to make entry `index` because no worker had reached it. It asks in
  // about the order the list was recorded in, so the next entries are the ones it will want:
  // the workers go on after it, and what lies between them and it (not asked for yet) waits
  // until the run is over. A claim behind the cursor or beyond the run changes nothing.
  std::lock_guard<std::mutex> lock(work_mutex_);
  const size_t next = index + 1;
  if (next <= list_cursor_ || next > list_end_ || next - list_cursor_ < kFollowMinGap) {
    return;
  }
  list_skipped_.push_back(ListRange{list_cursor_, index});
  list_cursor_ = next;
}

void Pipelines::EndBootMode(const char* why) {
  if (!boot_mode_.load()) {
    return;
  }
  {
    // Under the workers' mutex, or one about to wait would miss the wake-up.
    std::lock_guard<std::mutex> lock(work_mutex_);
    if (!boot_mode_.exchange(false)) {
      return;
    }
  }
  work_cv_.notify_all();
  REXLOG_INFO("[gpu] pipelines: boot mode over after {:.1f} s ({}): {} of {} entries left the "
              "queue, {} made by the render thread, {} builds it waited for",
              double(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - precache_start_)
                         .count()) /
                  1000.0,
              why, precache_finished_.load(), precached_.size(), boot_claims_.load(),
              boot_waits_.load());
}

void Pipelines::BuildPrecached(Precached& p) {
  uint8_t expected = kQueued;
  if (!p.state.compare_exchange_strong(expected, kBuilding)) {
    return;  // the render thread claimed it, or another worker has it
  }
  plume::RenderGraphicsPipelineDesc desc;
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(p.key, shaders_->Find(p.key.vs), p.key.ps ? shaders_->Find(p.key.ps) : nullptr,
                *p.layout, desc, &vs_result, &ps_result)) {
    if (!SkipShaders(p.key.vs, p.key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "precached guest pipeline");
    }
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
  NotePrecacheEntryDone();
}

void Pipelines::BuildRequest(Request& r) {
  uint8_t expected = kQueued;
  if (!r.state.compare_exchange_strong(expected, kBuilding)) {
    return;  // the render thread made it itself
  }
  plume::RenderGraphicsPipelineDesc desc;
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(r.key, r.vs, r.ps, *r.layout, desc, &vs_result, &ps_result)) {
    if (!SkipShaders(r.key.vs, r.key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "background guest pipeline");
    }
  }
  const bool made = pipeline != nullptr;
  {
    std::lock_guard<std::mutex> lock(precache_mutex_);
    r.pipeline = std::move(pipeline);
    r.vs_result = vs_result;
    r.ps_result = ps_result;
    r.state = made ? kReady : kFailed;
  }
  precache_done_.notify_all();
}

void Pipelines::NotePrecacheEntryDone() {
  // Each entry leaves the queue once: a worker builds it, or the render thread
  // claims it. The last one to leave says so.
  if (precache_finished_.fetch_add(1) + 1 == precached_.size() && !stop_precache_.load()) {
    REXLOG_INFO("[gpu] pipelines: precache done in {:.1f} ms: {} ready, {} failed",
                double(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - precache_start_)
                           .count()) /
                    1000.0,
                precache_ready_.load(), precache_failed_.load());
    EndBootMode("the list is done");
  }
}

void Pipelines::PushUrgent(Urgent urgent) {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    urgent_.push_back(std::move(urgent));
  }
  work_cv_.notify_one();
}

plume::RenderPipeline* Pipelines::TakePrecached(const PipelineKey& key, Miss& miss,
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
      miss.take = Take::kFailed;
      return nullptr;
    }
    aliases_.emplace(key, first->second.get());
    ++stats_.shared;
    return first->second.get();
  }
  uint8_t state = p.state.load();
  if ((state == kQueued || state == kBuilding) && MaySkip(miss.deferral)) {
    // The draw may wait for it: a worker makes it, and this one before the
    // rest of the list.
    if (!p.asked) {
      p.asked = true;
      p.asked_frame = frame_;
      ++stats_.pending_keys;
      if (state == kQueued) {
        Urgent urgent;
        urgent.precached = &p;
        PushUrgent(std::move(urgent));
      }
    }
    ++stats_.pending_draws;
    miss.take = Take::kPending;
    return nullptr;
  }
  state = kQueued;
  if (p.state.compare_exchange_strong(state, kClaimed)) {
    miss.take = Take::kMakeHere;
    NoteClaim(p.index);
    NotePrecacheEntryDone();
    ++stats_.precache_claimed;  // no worker at it yet: the caller makes it now
    p.taken = true;
    p.taken_as = key;
    return nullptr;
  }
  if (state == kBuilding) {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(precache_mutex_);
    precache_done_.wait(lock, [&] { return p.state.load() != kBuilding; });
    if (boot_mode_.load()) {
      ++boot_waits_;
    }
    ++stats_.precache_waits;
    stats_.precache_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count());
  }
  std::lock_guard<std::mutex> lock(precache_mutex_);
  if (p.state.load() != kReady) {
    miss.take = Take::kFailed;
    return nullptr;  // failed: made (and reported) on the caller's path
  }
  p.state = kClaimed;
  p.taken = true;
  p.taken_as = key;
  if (p.asked) {
    NotePendingTaken(p.asked_frame);
  }
  plume::RenderPipeline* raw = p.pipeline.get();
  pipelines_.emplace(key, std::move(p.pipeline));
  ++stats_.precache_taken;
  return raw;
}

bool Pipelines::StartRequest(const PipelineKey& key, const PipelineInputs& inputs) {
  if (workers_.empty() || stop_precache_.load() || !inputs.vs || !inputs.input_layout ||
      (key.ps != 0 && !inputs.ps)) {
    return false;
  }
  // The worker reads the layout and the shaders, so it gets the stored copy
  // (the session's layout may change under it) and the pack's entries.
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return false;
  }
  auto request = std::make_shared<Request>();
  request->key = key;
  request->vs = inputs.vs;
  request->ps = key.ps != 0 ? inputs.ps : nullptr;
  request->layout = stored_layouts_[stored].get();
  request->stored = stored;
  request->asked_frame = frame_;
  requests_.emplace(key, request);
  Urgent urgent;
  urgent.request = std::move(request);
  PushUrgent(std::move(urgent));
  ++stats_.pending_keys;
  ++stats_.pending_draws;
  return true;
}

plume::RenderPipeline* Pipelines::TakeRequested(const PipelineKey& key, Miss& miss) {
  const auto found = requests_.find(key);
  if (found == requests_.end()) {
    return nullptr;
  }
  Request& r = *found->second;
  uint8_t state = r.state.load();
  if (state == kQueued || state == kBuilding) {
    if (MaySkip(miss.deferral)) {
      if (r.low && !r.promoted) {
        // A copy for the coming MSAA level that a draw already needs: a thread takes it next.
        r.promoted = true;
        ++stats_.pending_keys;
        if (state == kQueued) {
          Urgent urgent;
          urgent.request = found->second;
          PushUrgent(std::move(urgent));
        }
      }
      ++stats_.pending_draws;
      miss.take = Take::kPending;
      return nullptr;
    }
    state = kQueued;
    if (r.state.compare_exchange_strong(state, kClaimed)) {
      // No worker at it yet: made here now, and the worker that finds it in
      // the queue finds it claimed.
      ++stats_.made_here_new;
      requests_.erase(found);
      miss.take = Take::kMakeHere;
      return nullptr;
    }
    if (state == kBuilding) {
      const auto t0 = std::chrono::steady_clock::now();
      std::unique_lock<std::mutex> lock(precache_mutex_);
      precache_done_.wait(lock, [&] { return r.state.load() != kBuilding; });
      ++stats_.request_waits;
      stats_.request_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count());
    }
  }
  plume::RenderPipeline* raw = AdoptRequest(key, r);
  requests_.erase(found);
  if (!raw) {
    miss.take = Take::kReported;
  }
  return raw;
}

plume::RenderPipeline* Pipelines::AdoptRequest(const PipelineKey& key, Request& r) {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  ShaderStore::ShaderResult vs_result;
  ShaderStore::ShaderResult ps_result;
  {
    std::lock_guard<std::mutex> lock(precache_mutex_);
    pipeline = std::move(r.pipeline);
    vs_result = r.vs_result;
    ps_result = r.ps_result;
  }
  if (pipeline) {
    ++stats_.background_taken;
    if (!r.low || r.promoted) {
      NotePendingTaken(r.asked_frame);  // a draw was waiting for it
    }
    if (persist_) {
      RecordStored(key, r.stored);
    }
  } else {
    LogFailure(key, vs_result, ps_result);
  }
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

void Pipelines::TakeFinishedRequests() {
  for (auto it = requests_.begin(); it != requests_.end();) {
    const uint8_t state = it->second->state.load();
    if (it->second->low && !it->second->promoted) {
      // A copy for a coming MSAA level that no draw has asked for: it is taken (and recorded for
      // the next start) by the first draw that does, so a level the player went back from leaves
      // nothing in the cache file.
      ++it;
      continue;
    }
    if (state == kReady || state == kFailed) {
      AdoptRequest(it->first, *it->second);
      it = requests_.erase(it);
    } else {
      ++it;
    }
  }
}

void Pipelines::NotePendingTaken(uint64_t asked_frame) {
  const uint64_t frames = frame_ - asked_frame;
  ++stats_.pending_taken;
  stats_.pending_frames_sum += frames;
  stats_.pending_frames_max = std::max(stats_.pending_frames_max, frames);
}

void Pipelines::EndFrame() {
  ++frame_;
  if (boot_mode_.load(std::memory_order_relaxed) &&
      std::chrono::steady_clock::now() >= boot_deadline_) {
    EndBootMode("time limit");
  }
  if (!requests_.empty()) {
    TakeFinishedRequests();
  }
  Autosave();
}

void Pipelines::LoadRecords() {
  // This API's file, then the other API's: a key names guest
  // state, shaders of the pack, Plume formats and a layout by its content's
  // hash -- nothing of the API -- so what one API saw precaches the other.
  // The one exception is a static depth bias (D3D12 without dynamic bias),
  // part of the key: a device with a dynamic bias zeroes it in every file,
  // its own too -- a D3D12 file from a build with the static bias holds keys
  // no draw asks for once the bias is dynamic.
  const Backend other = ActiveBackend() == Backend::kD3D12 ? Backend::kVulkan : Backend::kD3D12;
  own_file_records_ = LoadFile(cache_path_, ActiveBackend(), dynamic_depth_bias_);
  saved_records_ = own_file_records_;
  LoadFile(other_cache_path_, other, dynamic_depth_bias_);
  // Then the starter list that ships beside the pack (tools/pipelines): what
  // play usually needs, so a first start makes it here rather than at draw time.
  // It is a D3D12 list whose keys keep their static depth bias.
  LoadFile(starter_path_, Backend::kD3D12, dynamic_depth_bias_);
  loaded_records_ = records_.size();
  last_save_ = std::chrono::steady_clock::now();
}

size_t Pipelines::LoadFile(const std::filesystem::path& path, Backend backend, bool zero_bias) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_INFO("[gpu] pipelines: no cache at {} yet", path.string());
    return 0;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  Reader in(data.data(), data.size());
  CacheHeader header;
  const auto discard = [&](const char* why) -> size_t {
    REXLOG_ERROR("[gpu] pipelines: the cache {} is discarded: {}", path.string(),
                 why);
    return 0;
  };
  if (!in.Get(&header, sizeof(header)) ||
      std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0) {
    return discard("not a pipeline cache");
  }
  if (header.version != kCacheVersion || header.key_size != sizeof(PipelineKey) ||
      header.backend != uint32_t(backend)) {
    return discard("written by another version or backend");
  }
  // Everything is read before anything is kept: a fault anywhere adds nothing.
  // (Semantic names go to names_ as they are read; a discarded file leaves a
  // few unused strings there.)
  std::vector<std::unique_ptr<StoredLayout>> layouts;
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
    layouts.push_back(std::move(s));
  }
  std::vector<Record> records(header.record_count);
  for (Record& r : records) {
    if (!in.Get(&r.key, sizeof(r.key)) || !in.Get32(&r.layout) || r.layout >= layouts.size()) {
      return discard("a record is cut short or names no layout");
    }
  }
  // Kept: the layouts not known yet, then the records not known yet.
  std::vector<uint32_t> layout_of_file;  // file layout index -> stored_layouts_
  for (std::unique_ptr<StoredLayout>& s : layouts) {
    const auto known = stored_layout_index_.find(s->hash);
    if (known != stored_layout_index_.end()) {
      layout_of_file.push_back(known->second);
      continue;
    }
    layout_of_file.push_back(uint32_t(stored_layouts_.size()));
    stored_layout_index_.emplace(s->hash, uint32_t(stored_layouts_.size()));
    stored_layouts_.push_back(std::move(s));
  }
  size_t added = 0;
  uint32_t dropped = 0;
  uint32_t zeroed = 0;
  for (Record r : records) {
    r.layout = layout_of_file[r.layout];
    // A shader the pack no longer has: the pipeline cannot be made. Nor can one with an HDR scene
    // variant (stage 3 of the HDR output) while the HDR pack is not loaded or lacks it.
    if (!shaders_->Find(r.key.vs) || (r.key.ps != 0 && !shaders_->Find(r.key.ps)) ||
        (r.key.ps_variant == ShaderStore::kHdrVariant &&
         (r.key.ps == 0 || !shaders_->Find(r.key.ps)->hdr_variant))) {
      ++dropped;
      continue;
    }
    if (zero_bias) {
      if (r.key.depth_bias != 0 || r.key.slope_scaled_depth_bias != 0.0f) {
        ++zeroed;
      }
      r.key.depth_bias = 0;
      r.key.slope_scaled_depth_bias = 0.0f;
    }
    // A depth format this device has no target for (the other API's file holds D24).
    r.key.depth_format =
        uint32_t(format::HostDepthFormat(plume::RenderFormat(r.key.depth_format)));
    if (recorded_.insert(r.key).second) {
      records_.push_back(r);
      ++added;
    }
  }
  REXLOG_INFO("[gpu] pipelines: {} records and {} layouts in {}: {} new here ({} dropped: a "
              "shader the pack does not have{})",
              header.record_count, header.layout_count, path.string(), added, dropped,
              zeroed ? std::format("; {} static depth biases zeroed", zeroed) : std::string());
  return added;
}

std::vector<uint8_t> Pipelines::SerializeRecords() const {
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
  return out;
}

void Pipelines::WriteCacheFile(const std::vector<uint8_t>& bytes, size_t records,
                               size_t layouts) const {
  std::error_code ec;
  std::filesystem::create_directories(cache_path_.parent_path(), ec);
  std::filesystem::path temp = cache_path_;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
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
  REXLOG_INFO("[gpu] pipelines: {} records ({} new this session, {} from the other API's file) "
              "and {} layouts written to {}",
              records, records - loaded_records_, loaded_records_ - own_file_records_, layouts,
              cache_path_.string());
}

void Pipelines::SaveRecords() {
  // Written even when an autosave holds everything already: one that failed
  // would otherwise lose the session's records.
  if (records_.size() == own_file_records_) {
    return;  // nothing beyond what this API's file already holds
  }
  WriteCacheFile(SerializeRecords(), records_.size(), stored_layouts_.size());
}

void Pipelines::JoinAutosave() {
  if (autosave_thread_.joinable()) {
    autosave_thread_.join();
  }
}

void Pipelines::Autosave() {
  if (!persist_ || records_.size() == saved_records_ ||
      autosave_running_.load(std::memory_order_acquire)) {
    return;
  }
  constexpr auto kInterval = std::chrono::seconds(30);
  const auto now = std::chrono::steady_clock::now();
  if (now - last_save_ < kInterval) {
    return;
  }
  last_save_ = now;
  saved_records_ = records_.size();
  JoinAutosave();  // finished: only the handle is left
  autosave_running_.store(true, std::memory_order_release);
  autosave_thread_ = std::thread(
      [this, bytes = SerializeRecords(), records = records_.size(),
       layouts = stored_layouts_.size()] {
        WriteCacheFile(bytes, records, layouts);
        autosave_running_.store(false, std::memory_order_release);
      });
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
    if (depth24_bias_shift_ != 0 && state.depth_format == xe::DepthRenderTargetFormat::kD24S8) {
      bias.constant = int32_t(std::clamp<int64_t>(
          int64_t(bias.constant) * (int64_t(1) << depth24_bias_shift_), INT32_MIN, INT32_MAX));
    }
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

void Pipelines::LogFailure(const PipelineKey& key, ShaderStore::ShaderResult vs_result,
                           ShaderStore::ShaderResult ps_result) {
  ++stats_.failed;
  if (stats_.failed <= 16) {
    REXLOG_ERROR("[gpu] pipeline for vs {:016X}/{} ps {:016X}/{} not created (vs shader {}, ps "
                 "shader {}, {} colour targets, depth format {})",
                 key.vs, key.vs_variant, key.ps, key.ps_variant,
                 ShaderStore::ResultName(vs_result), ShaderStore::ResultName(ps_result),
                 key.color_count, key.depth_format);
  }
}

plume::RenderPipeline* Pipelines::Get(const PipelineKey& key, const PipelineInputs& inputs,
                                      const Deferral* deferral, bool* pending) {
  if (pending) {
    *pending = false;
  } else {
    deferral = nullptr;  // nowhere to say "pending": the draw may not wait
  }
  const auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.get();
  }
  if (const auto alias = aliases_.find(key); alias != aliases_.end()) {
    return alias->second;
  }
  plume::RenderPipeline* got = Resolve(key, inputs, deferral, pending);
  if (got && inputs.msaa_guest_samples != 0) {
    NoteLevelDependent(key, inputs);  // once: the next lookups hit the maps above
  }
  return got;
}

void Pipelines::NoteLevelDependent(const PipelineKey& key, const PipelineInputs& inputs) {
  PipelineKey canonical = key;
  canonical.sample_count = inputs.msaa_guest_samples;
  if (!level_dependent_index_.insert(canonical).second || !inputs.input_layout) {
    return;
  }
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return;  // neither recorded nor built in the background: made at the draw when needed
  }
  level_dependent_.push_back(LevelDependent{canonical, stored});
}

Pipelines::LevelPrebuild Pipelines::PrebuildForLevel(uint32_t level) {
  // CHECK: that the copies are the pipelines the first draws after the switch ask for (the same key
  // but the sample count: the log's pipeline statistics for the minute after a switch should show
  // few or no pending draws and nothing made on the render thread; a missing one shows as a draw
  // left out for a few frames), that the build of 4 and 8 sample pipelines works on both backends
  // (an 8 sample pipeline against an 8 sample framebuffer under the D3D12 debug layer and Vulkan
  // validation), and how long the set takes at the first switch of a session (the 20 s limit).
  // Also that the pause menu's and the map's world draws, whose targets now have the level's samples
  // (or 1 without MSAA), mostly find the game view's pipelines (the first pause open compiles few or
  // none on the render thread), and that a level change leaves them a copy too.
  LevelPrebuild result;
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  result.known = uint32_t(level_dependent_.size());
  if (level_dependent_.empty() || workers_.empty() || stop_precache_.load() || !shaders_) {
    return result;
  }
  std::vector<Urgent> items;
  for (const LevelDependent& d : level_dependent_) {
    PipelineKey key = d.key;
    // EdramTargets::HostSamplesAt: the level, which may be fewer than the guest's samples (the
    // pause menu's and the map's world view, which the game draws with 4).
    key.sample_count = level;
    if (pipelines_.count(key) != 0 || aliases_.count(key) != 0) {
      ++result.present;
      continue;
    }
    if (const auto asked = requests_.find(key); asked != requests_.end()) {
      const uint8_t state = asked->second->state.load();
      if (state == kQueued || state == kBuilding) {
        prebuild_requests_.push_back(asked->second);
      } else {
        ++result.present;  // finished: the end of the frame, or the first draw, takes it
      }
      continue;
    }
    if (const auto listed = precached_by_key_.find(PersistentKey(key, d.stored));
        listed != precached_by_key_.end()) {
      // A record of the cache file or the starter list has it: that entry is made, the first draw
      // takes it (TakePrecached).
      Precached& p = *listed->second;
      const uint8_t state = p.state.load();
      if (p.taken || state == kReady || state == kFailed || state == kClaimed) {
        ++result.present;
        continue;
      }
      prebuild_precached_.push_back(&p);
      if (state == kQueued) {
        Urgent item;
        item.precached = &p;
        items.push_back(std::move(item));
        ++result.queued;
      }
      continue;
    }
    const ShaderStore::Entry* vs = shaders_->Find(key.vs);
    const ShaderStore::Entry* ps = key.ps != 0 ? shaders_->Find(key.ps) : nullptr;
    if (!vs || (key.ps != 0 && !ps)) {
      continue;  // not in the pack (a pack that changed): its draws are skipped as they were
    }
    auto request = std::make_shared<Request>();
    request->key = key;
    request->vs = vs;
    request->ps = ps;
    request->layout = stored_layouts_[d.stored].get();
    request->stored = d.stored;
    request->asked_frame = frame_;
    request->low = true;
    requests_.emplace(key, request);
    prebuild_requests_.push_back(request);
    Urgent item;
    item.request = std::move(request);
    items.push_back(std::move(item));
    ++result.queued;
  }
  if (!items.empty()) {
    {
      std::lock_guard<std::mutex> lock(work_mutex_);
      for (Urgent& item : items) {
        low_.push_back(std::move(item));
      }
    }
    work_cv_.notify_all();
  }
  result.waiting = PrebuildWaiting();
  return result;
}

uint32_t Pipelines::PrebuildWaiting() const {
  uint32_t waiting = 0;
  for (const std::shared_ptr<Request>& r : prebuild_requests_) {
    const uint8_t state = r->state.load();
    waiting += state == kQueued || state == kBuilding;
  }
  for (const Precached* p : prebuild_precached_) {
    const uint8_t state = p->state.load();
    waiting += state == kQueued || state == kBuilding;
  }
  return waiting;
}

plume::RenderPipeline* Pipelines::Resolve(const PipelineKey& key, const PipelineInputs& inputs,
                                          const Deferral* deferral, bool* pending) {
  Miss miss;
  miss.deferral = deferral;
  // A background build asked for earlier, then the precache list's entry.
  if (!requests_.empty()) {
    if (plume::RenderPipeline* requested = TakeRequested(key, miss)) {
      return requested;
    }
  }
  if (miss.take == Take::kNone) {
    if (plume::RenderPipeline* precached = TakePrecached(key, miss, inputs)) {
      return precached;
    }
  }
  switch (miss.take) {
    case Take::kPending:
      *pending = true;
      return nullptr;
    case Take::kReported:
      return nullptr;
    case Take::kNone:
      // Nobody has it: a worker makes it when the draw may wait for it.
      if (MaySkip(deferral) && StartRequest(key, inputs)) {
        *pending = true;
        return nullptr;
      }
      ++stats_.made_here_new;
      break;
    case Take::kFailed:
      ++stats_.made_here_retry;
      break;
    case Take::kMakeHere:
      break;
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
    if (!SkipShaders(key.vs, key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "guest pipeline");
    }
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
    LogFailure(key, vs_result, ps_result);
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
