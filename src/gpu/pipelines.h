#pragma once
// ===========================================================================
// MCLA GPU renderer -- graphics pipelines of the guest draws
// ===========================================================================
// A PipelineKey is the content of a
// pipeline -- shader identities and spec variants, input layout, topology
// class, target formats and sample count, blend per target, depth/stencil,
// raster state -- with no pointer in it, canonicalized so equal states give
// equal keys (the lessons reblue's pipeline cache encodes):
//   - a target the pixel shader does not write, or whose write mask is 0,
//     gets no blend and mask 0 (the hardware leaves it untouched; D3D12 would
//     write undefined values for a missing SV_Target);
//   - *_COLOR factors in the alpha equation become *_ALPHA;
//   - MIN/MAX are declared with ONE/ONE: the host ignores the factors there,
//     and the source factor the Xenos applies before MIN/MAX goes to the
//     shader (g_BlendPremult*, constants.h);
//   - a copy blend (ONE, ZERO, ADD on both equations) is "blend off";
//   - depth and stencil state are zeroed when depth/stencil are off;
//   - the stencil reference is static (Plume has no dynamic stencil
//     reference; three references per state were measured, 24.3);
//   - the depth bias is dynamic where the device has it (Vulkan always,
//     D3D12 with options16 and Plume's Agility SDK build), as reblue does, and
//     part of the key otherwise;
//   - the pixel shader's ALPHA_TEST variant (contract v6: the one whose
//     coverage depends on alpha) only for an alpha test the shader can run as
//     "alpha >= threshold" (constants.h AlphaTest), alpha to coverage, or
//     both; every other draw keeps variant 0, which has no discard or
//     SV_Coverage of its own and so keeps the host's early depth/stencil test.
// Disabled blending is declared as the copy blend (ONE, ZERO, ADD): Plume
// translates every field whether blending is on or not, and UNKNOWN is not a
// valid D3D12 or Vulkan value.
// What the host cannot express is reported once per kind: a constant blend
// factor (Plume has no setBlendFactor), separate back-face stencil masks or
// reference (one set of masks in a D3D12 pipeline), point fill, an alpha
// test function that is not a lower bound, alpha test or alpha to coverage on
// a pixel shader compiled without the variant. Both faces culled is not a
// failure: the draw has no effect (the caller skips it).
//
// Pipelines are created synchronously on the render thread on first use and
// kept for the life of the renderer.
//
// Persistence (M5.3; mcla_gpu_pipeline_cache): every pipeline
// created is recorded -- its key, with the input layout by content (the
// session's InputLayout::id means nothing to the next one) -- and on Shutdown
// the records go to cache/mcla_gpu_pipelines_<backend>.bin beside the
// executable, the earlier ones first, read back at the next Initialize.
// Precache (M5.3b): workers make the records' pipelines ahead of use, in the
// file's order; Get takes one that is ready, waits for one being made, and
// makes one no worker has taken yet itself (the workers then skip it). A
// pipeline is taken only for a layout equal to its record's, byte for byte.
// Session layouts with other ids and the same content share a persistent key,
// and so the pipeline: the later keys alias the first one's.
//
// Threading: the render thread; the precache workers touch only their own
// records, the ShaderStore (GetShader locks) and the device.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "backend.h"
#include "draw_state.h"
#include "input_layout.h"
#include "shaders.h"

namespace mcla::gpu {

class Device;
class GuestLayout;

struct PipelineKey {
  uint64_t vs = 0;
  uint64_t ps = 0;  // 0 = no pixel shader (depth only)
  uint32_t vs_variant = 0;
  uint32_t ps_variant = 0;
  uint32_t input_layout = 0;
  uint32_t topology = 0;      // plume::RenderPrimitiveTopology
  uint32_t sample_count = 1;
  uint32_t color_count = 0;
  uint32_t color_format[kColorTargetCount] = {};  // plume::RenderFormat
  uint32_t depth_format = 0;
  // Per target: enable | src | dst | op | srcA | dstA | opA | mask, 4 bits each.
  uint32_t blend[kColorTargetCount] = {};
  uint32_t depth = 0;    // enable | write | func
  uint32_t stencil = 0;  // enable | func, fail, zpass, zfail front; func, fail, zpass back (4 bits)
  uint32_t stencil_masks = 0;  // read | write | ref, 8 bits each
  uint32_t raster = 0;   // cull | front ccw | fill | depth clip
  int32_t depth_bias = 0;  // static depth bias only (0 with the dynamic one)
  float slope_scaled_depth_bias = 0.0f;
  uint32_t reserved = 0;  // keeps the struct free of padding: it is hashed and compared as bytes

  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(PipelineKey) == 104, "PipelineKey must have no padding");

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const;
};

// What a draw needs besides the key to create its pipeline.
struct PipelineInputs {
  const ShaderStore::Entry* vs = nullptr;
  const ShaderStore::Entry* ps = nullptr;
  const InputLayout* input_layout = nullptr;
};

// The polygon offset of a draw in host units.
struct DepthBias {
  int32_t constant = 0;
  float slope = 0.0f;
  bool operator==(const DepthBias& o) const { return constant == o.constant && slope == o.slope; }
};

// Why BuildKey refused a draw.
enum class KeyResult : uint8_t {
  kOk,
  kNoEffect,       // both faces culled: nothing of the draw survives
  kEdramMode,      // an EDRAM mode other than colour+depth or depth only (reported)
  kColorGap,       // colour targets with a gap below a bound one (reported)
};

class Pipelines {
 public:
  Pipelines() = default;
  ~Pipelines();
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;

  void Initialize(Device& device, const GuestLayout& layout, ShaderStore& shaders);
  // The GPU must be idle.
  void Shutdown();

  // Fills `key` (and the draw's depth bias, which the caller sets on the list
  // when it is dynamic) from the draw. `topology` is the host's (after any
  // conversion); `polygonal` the guest primitive's (topology.h IsPolygonal),
  // which decides culling, fill mode and the face whose polygon offset applies.
  // Anything but kOk: the draw is skipped.
  KeyResult BuildKey(const XenosState& state, const PipelineInputs& inputs,
                     plume::RenderPrimitiveTopology topology, bool polygonal,
                     const plume::RenderFormat color_formats[kColorTargetCount],
                     plume::RenderFormat depth_format, PipelineKey& key, DepthBias& bias);

  // True when every pipeline takes its depth bias from the command list.
  bool dynamic_depth_bias() const { return dynamic_depth_bias_; }

  // The pipeline for `key`, created on first use. Null when creation failed
  // (logged once per key).
  plume::RenderPipeline* Get(const PipelineKey& key, const PipelineInputs& inputs);

  struct Stats {
    uint64_t created = 0;      // on the render thread, at draw time
    uint64_t failed = 0;
    uint64_t create_ns = 0;
    uint64_t precache_taken = 0;    // made by a worker, ready when asked for
    uint64_t precache_waits = 0;    // ... being made when asked for: waited
    uint64_t precache_wait_ns = 0;
    uint64_t precache_claimed = 0;  // in the file, no worker at it yet: made here
    uint64_t shared = 0;            // keys whose persistent key another key's pipeline serves
  };
  Stats TakeStats();
  size_t size() const { return pipelines_.size(); }

 private:
  // An input layout by content, the persistent stand-in of InputLayout::id:
  // the elements (their semantic names owned by names_) and slots, and the
  // hash of their serialized form.
  struct StoredLayout {
    uint64_t hash = 0;
    std::vector<plume::RenderInputElement> elements;
    std::vector<plume::RenderInputSlot> slots;
  };
  // A pipeline the game asked for, as the file keeps it: the key with
  // input_layout | reserved << 32 = its layout's hash.
  struct Record {
    PipelineKey key;
    uint32_t layout = 0;  // index in stored_layouts_
  };

  void Report(uint32_t kind, const char* what);
  // The layout's content as written to the file (and hashed).
  static void SerializeLayout(const plume::RenderInputElement* elements, size_t element_count,
                              const plume::RenderInputSlot* slots, size_t slot_count,
                              std::vector<uint8_t>& out);
  // The stored layout of a session layout (made on first use).
  uint32_t StoreLayout(const InputLayout& layout);
  void RecordPipeline(const PipelineKey& key, const InputLayout& layout);
  void LoadRecords();
  // Reads one cache file and adds the records it holds that are not known
  // yet; a file with any fault adds nothing. `zero_bias` zeroes the static
  // depth bias of its keys (another API's file read by one with a dynamic
  // bias). Returns the records added.
  size_t LoadFile(const std::filesystem::path& path, Backend backend, bool zero_bias);
  void SaveRecords();

  // The description of `key`'s pipeline with the given shaders and layout;
  // false (the results say why) when a shader cannot be had.
  bool BuildDesc(const PipelineKey& key, const ShaderStore::Entry* vs,
                 const ShaderStore::Entry* ps, const StoredLayout& layout,
                 plume::RenderGraphicsPipelineDesc& desc, ShaderStore::ShaderResult* vs_result,
                 ShaderStore::ShaderResult* ps_result);
  // The persistent key of a session key with the layout `stored`.
  PipelineKey PersistentKey(const PipelineKey& key, uint32_t stored) const;
  void StartPrecache();
  void StopPrecache();
  void PrecacheWorker();
  // A ready precached pipeline for `key` (moved into pipelines_), or null:
  // none, being claimed now for the render thread to make, or failed.
  plume::RenderPipeline* TakePrecached(const PipelineKey& key, const PipelineInputs& inputs);

  Device* device_ = nullptr;
  const GuestLayout* layout_ = nullptr;
  ShaderStore* shaders_ = nullptr;
  std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash>
      pipelines_;
  Stats stats_;
  uint32_t reported_kinds_ = 0;
  bool dynamic_depth_bias_ = false;

  // Persistence: this API's file and the other API's (read too,
  //), the layouts and records they hold plus the session's new
  // ones, the persistent keys recorded, how many records came from the files
  // and how many from this API's own.
  bool persist_ = false;
  std::filesystem::path cache_path_;
  std::filesystem::path other_cache_path_;
  std::deque<std::string> names_;
  std::vector<std::unique_ptr<StoredLayout>> stored_layouts_;
  std::unordered_map<uint64_t, uint32_t> stored_layout_index_;  // hash -> stored_layouts_
  std::unordered_map<uint32_t, uint32_t> session_layouts_;      // InputLayout::id -> stored
  std::vector<Record> records_;
  std::unordered_set<PipelineKey, PipelineKeyHash> recorded_;
  size_t loaded_records_ = 0;
  size_t own_file_records_ = 0;
  static constexpr uint32_t kNoLayout = 0xFFFFFFFFu;  // a layout that cannot be persisted

  // Precache: one entry per loaded record, its state moved by
  // compare-exchange (queued -> building or claimed; building -> ready or
  // failed, under precache_mutex_ for the waiter).
  enum PrecacheState : uint8_t { kQueued, kBuilding, kReady, kFailed, kClaimed };
  struct Precached {
    PipelineKey key;                  // persistent
    const StoredLayout* layout = nullptr;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    std::atomic<uint8_t> state{kQueued};
    PipelineKey taken_as;             // the session key it went to (claimed or taken)
    bool taken = false;
  };
  std::vector<std::unique_ptr<Precached>> precached_;
  // Session keys served by another key's pipeline (same persistent key).
  std::unordered_map<PipelineKey, plume::RenderPipeline*, PipelineKeyHash> aliases_;
  std::unordered_map<PipelineKey, Precached*, PipelineKeyHash> precached_by_key_;
  std::vector<std::thread> workers_;
  std::atomic<size_t> next_precache_{0};
  std::atomic<bool> stop_precache_{false};
  std::atomic<uint32_t> precache_ready_{0};
  std::atomic<uint32_t> precache_failed_{0};
  std::atomic<uint32_t> workers_running_{0};
  std::chrono::steady_clock::time_point precache_start_;
  std::mutex precache_mutex_;
  std::condition_variable precache_done_;
};

}  // namespace mcla::gpu
