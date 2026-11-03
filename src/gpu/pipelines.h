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
// Pipelines are kept for the life of the renderer. One nobody made ahead is
// made on the render thread when the draw that needs it may not be left out,
// and by a builder thread when it may (background builds, below).
//
// Persistence (M5.3; mcla_gpu_pipeline_cache): every pipeline
// created is recorded -- its key, with the input layout by content (the
// session's InputLayout::id means nothing to the next one) -- and the records
// go to cache/mcla_gpu_pipelines_<backend>.bin beside the executable, the
// earlier ones first, read back at the next Initialize. They are written on
// Shutdown and during play (Autosave), so a crash or a killed process keeps
// what the session had seen by then. A starter list ships beside the pack
// (assets/mcla_pipelines.bin, built with tools/pipelines/pipeline_list.py) and
// is read after them, so a first start precaches what play usually needs.
// Precache (M5.3b): workers make the records' pipelines ahead of use, in the
// file's order; Get takes one that is ready, waits for one being made, and
// makes one no worker has taken yet itself (the workers then skip it). A
// pipeline is taken only for a layout equal to its record's, byte for byte.
// Session layouts with other ids and the same content share a persistent key,
// and so the pipeline: the later keys alias the first one's.
//
// Background builds: a pipeline nobody precached cost 20 to 150 ms on the
// render thread when a draw first needed it (measured, RTX 5080, cold driver
// cache), and about 50 such misses were left in a cold 3 minute drive after
// the starter list. When Get misses it asks the Scene (Deferral) whether the
// draw may be left out for now: only a draw into targets the game draws into
// in every frame, so that the next frame's draw puts back what the skipped one
// left out (scene.h, MaySkipForPipeline). Then the key goes to the builder
// threads -- the precache workers, who take these before the rest of their
// list -- and Get returns null with *pending set. The Scene skips the draw,
// counts it apart from a failure and does not memoize the null. The next Get
// for the key finds the build still going (pending again) or finished (taken:
// the render thread moves the pipeline into pipelines_, as it does for a
// precached one). A key that has a precache record is moved to the front of
// the workers' queue instead of being made here. A draw that may not be
// skipped keeps the old way: a build under way is waited for, never made
// twice; one no worker has begun is made here, and the worker drops it. A
// build that fails in the background is reported once and stays null, like one
// that failed here. A finished build nobody asks for again is taken at the end
// of the frame (EndFrame), so it is recorded and persists like any other.
//
// Threading: the render thread; the builder threads (the precache workers)
// touch only the entries and requests they were handed, the ShaderStore
// (GetShader locks) and the device; the autosave thread only the bytes it was
// given and the file. The render thread owns requests_ and the `asked` fields
// of the entries; a request goes to the workers through urgent_ (work_mutex_)
// and comes back through its state and pipeline under precache_mutex_. A hit
// in pipelines_ takes no lock.
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

// What Get asks when a pipeline is not there yet: may the draw be left out
// until a builder thread has made it? A plain function and a context, so that
// a lookup that hits has nothing to prepare for it.
struct Deferral {
  bool (*may_skip)(const void* context) = nullptr;
  const void* context = nullptr;
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

  // The pipeline for `key`: made on first use, or taken from a thread that made
  // it. Null when creation failed (logged once per key) or, with *pending set,
  // when a builder thread is making it and `deferral` said the draw may be left
  // out: the caller skips the draw, does not remember the null, and asks again
  // for the next draw with the key. Without `deferral` and `pending` the call
  // never returns pending.
  plume::RenderPipeline* Get(const PipelineKey& key, const PipelineInputs& inputs,
                             const Deferral* deferral = nullptr, bool* pending = nullptr);

  // Once a frame (a guest present), on the render thread: counts the frame,
  // takes the background builds that finished, and Autosave.
  void EndFrame();

  // Once a frame, on the render thread: writes the records to the file when
  // there are new ones and the last write was a while ago. The file is written
  // on another thread.
  void Autosave();

  struct Stats {
    uint64_t created = 0;      // on the render thread, at draw time
    uint64_t failed = 0;
    uint64_t create_ns = 0;
    uint64_t precache_taken = 0;    // made by a worker, ready when asked for
    uint64_t precache_waits = 0;    // ... being made when asked for: waited
    uint64_t precache_wait_ns = 0;
    uint64_t precache_claimed = 0;  // in the file, no worker at it yet: made here
    uint64_t shared = 0;            // keys whose persistent key another key's pipeline serves
    // Background builds. A key is pending from the first draw skipped for it
    // (a key of the precache list that no worker had finished counts too) to the
    // take; the wait is counted in frames (EndFrame).
    uint64_t pending_keys = 0;        // handed to the builders, or moved up their queue
    uint64_t pending_draws = 0;       // lookups answered "pending": the draw was skipped
    uint64_t pending_taken = 0;       // pending keys taken ready
    uint64_t pending_frames_sum = 0;  // frames from the first skip to the take, summed
    uint64_t pending_frames_max = 0;  // ... and the longest
    uint64_t background_taken = 0;    // of the taken, made for a key no record named
    // What stayed on the render thread, and why.
    uint64_t request_waits = 0;       // waited for a background build of a key no record named
    uint64_t request_wait_ns = 0;
    uint64_t made_here_new = 0;       // made here: nobody had begun it, no waiting
    uint64_t made_here_retry = 0;     // made here again after a background build failed
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
  // The same for a layout already stored (RecordPipeline's second half).
  void RecordStored(const PipelineKey& key, uint32_t stored);
  void LoadRecords();
  // Reads one cache file and adds the records it holds that are not known
  // yet; a file with any fault adds nothing. `zero_bias` zeroes the static
  // depth bias of its keys (another API's file read by one with a dynamic
  // bias). Returns the records added.
  size_t LoadFile(const std::filesystem::path& path, Backend backend, bool zero_bias);
  // The file's bytes for the records as they are now. Empty: nothing to write.
  std::vector<uint8_t> SerializeRecords() const;
  void WriteCacheFile(const std::vector<uint8_t>& bytes, size_t records, size_t layouts) const;
  void SaveRecords();
  void JoinAutosave();

  // The description of `key`'s pipeline with the given shaders and layout;
  // false (the results say why) when a shader cannot be had.
  bool BuildDesc(const PipelineKey& key, const ShaderStore::Entry* vs,
                 const ShaderStore::Entry* ps, const StoredLayout& layout,
                 plume::RenderGraphicsPipelineDesc& desc, ShaderStore::ShaderResult* vs_result,
                 ShaderStore::ShaderResult* ps_result);
  // The persistent key of a session key with the layout `stored`.
  PipelineKey PersistentKey(const PipelineKey& key, uint32_t stored) const;
  // Counts a pipeline that could not be made and reports it (the first few).
  void LogFailure(const PipelineKey& key, ShaderStore::ShaderResult vs_result,
                  ShaderStore::ShaderResult ps_result);
  void StartPrecache();
  void StopPrecache();
  void PrecacheWorker();

  // How a lookup that gave no pipeline ended: nothing is known of the key
  // (kNone); a build is under way and the draw may wait (kPending); the caller
  // makes it now, already counted (kMakeHere); a precached entry failed in
  // the background (kFailed: made again, and reported, on the caller's path); a
  // request failed in the background and was reported (kReported).
  enum class Take : uint8_t { kNone, kPending, kMakeHere, kFailed, kReported };
  struct Miss {
    const Deferral* deferral = nullptr;  // null: the draw may not be skipped
    Take take = Take::kNone;
  };
  // A ready precached pipeline for `key` (moved into pipelines_), or null: none,
  // being made while the draw may wait, being claimed now for the render thread
  // to make, or failed (`miss.take` says which).
  plume::RenderPipeline* TakePrecached(const PipelineKey& key, Miss& miss,
                                       const PipelineInputs& inputs);

  Device* device_ = nullptr;
  const GuestLayout* layout_ = nullptr;
  ShaderStore* shaders_ = nullptr;
  std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash>
      pipelines_;
  // Where kD24S8 is kept as D32 float (format::Depth24Unsupported): the shift that turns
  // the guest's constant offset in 2^-24 steps into the float buffer's steps; 0 otherwise.
  uint32_t depth24_bias_shift_ = 0;
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
  std::filesystem::path starter_path_;  // assets/mcla_pipelines.bin
  std::deque<std::string> names_;
  std::vector<std::unique_ptr<StoredLayout>> stored_layouts_;
  std::unordered_map<uint64_t, uint32_t> stored_layout_index_;  // hash -> stored_layouts_
  std::unordered_map<uint32_t, uint32_t> session_layouts_;      // InputLayout::id -> stored
  std::vector<Record> records_;
  std::unordered_set<PipelineKey, PipelineKeyHash> recorded_;
  size_t loaded_records_ = 0;
  size_t own_file_records_ = 0;
  size_t saved_records_ = 0;  // what the file holds, or what the running autosave will leave in it
  std::chrono::steady_clock::time_point last_save_;
  std::thread autosave_thread_;
  std::atomic<bool> autosave_running_{false};
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
    // Render thread only: a draw that may be skipped asked for it, in this frame
    // (EndFrame's count); the worker is told to make it first.
    bool asked = false;
    uint64_t asked_frame = 0;
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
  std::atomic<uint32_t> precache_finished_{0};  // entries built by a worker or claimed here
  std::chrono::steady_clock::time_point precache_start_;
  std::mutex precache_mutex_;
  std::condition_variable precache_done_;

  // Background builds for keys no record names. A request holds what the
  // worker reads -- the shaders (the pack's, which live as long as the store),
  // and the layout through stored_layouts_ -- and its state moves like an
  // entry's (queued -> building -> ready or failed, under precache_mutex_;
  // claimed: the render thread made it). The map and the requests' lifetime are
  // the render thread's; a request still queued when taken is kept alive by the
  // queue until a worker finds it claimed.
  struct Request {
    PipelineKey key;                        // the session key
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;  // null: depth only
    const StoredLayout* layout = nullptr;    // owned by stored_layouts_
    uint32_t stored = 0;                     // its index there
    uint64_t asked_frame = 0;
    std::atomic<uint8_t> state{kQueued};
    std::unique_ptr<plume::RenderPipeline> pipeline;
    ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
    ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  };
  // Work a worker takes before the rest of the precache list: a request, or
  // an entry of the list a draw is waiting for.
  struct Urgent {
    Precached* precached = nullptr;
    std::shared_ptr<Request> request;
  };
  std::unordered_map<PipelineKey, std::shared_ptr<Request>, PipelineKeyHash> requests_;
  std::deque<Urgent> urgent_;  // work_mutex_
  std::mutex work_mutex_;
  std::condition_variable work_cv_;
  uint64_t frame_ = 0;  // EndFrame's count, for the waits

  void PushUrgent(Urgent urgent);
  // Hands `key` to the builders. False when it cannot be built in the
  // background (no shaders, a layout that cannot be stored, no workers).
  bool StartRequest(const PipelineKey& key, const PipelineInputs& inputs);
  // The background build of `key`, if one was asked for: taken when ready,
  // waited for when the draw may not be skipped (made here when no worker had
  // begun it), pending when it may.
  plume::RenderPipeline* TakeRequested(const PipelineKey& key, Miss& miss);
  // A finished request into pipelines_ (null for a failure, reported) and the
  // records.
  plume::RenderPipeline* AdoptRequest(const PipelineKey& key, Request& request);
  void TakeFinishedRequests();
  void NotePendingTaken(uint64_t asked_frame);
  void NotePrecacheEntryDone();
  void BuildPrecached(Precached& entry);
  void BuildRequest(Request& request);
};

}  // namespace mcla::gpu
