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
// earlier ones first, read back at the next StartPersistence (when the shader pack is
// attached: the records are checked against it). They are written on
// Shutdown and during play (Autosave), so a crash or a killed process keeps
// what the session had seen by then. A starter list ships in <executable>/assets
// (mcla_pipelines.bin, built with tools/pipelines/pipeline_list.py; the pack's own
// folder does not matter) and is read after them, so a first start precaches what
// play usually needs.
// Precache (M5.3b): workers make the records' pipelines ahead of use, in the
// file's order; Get takes one that is ready, waits for one being made, and
// makes one no worker has taken yet itself (the workers then skip it). A
// pipeline is taken only for a layout equal to its record's, byte for byte.
// Session layouts with other ids and the same content share a persistent key,
// and so the pipeline: the later keys alias the first one's.
// With fewer than 8 usable processors the workers start in boot mode (boot_mode_):
// more of them, at normal priority, following the render thread through the list.
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
// Live MSAA level (PrebuildForLevel): a pipeline's sample count is its draw's targets' host
// samples, which the MSAA level sets for an image of the screen the guest multisamples
// (EdramTargets::HostSamplesAt). A draw whose targets take the level says so (PipelineInputs::
// msaa_guest_samples), and the first time its pipeline is had it is remembered apart from its
// sample count. When the level is about to change, one copy per remembered pipeline at the new
// level's sample count is handed to the builder threads at a lower priority than anything a draw
// is waiting for (the queue after urgent_, before the precache list); the caller waits for them
// (PrebuildWaiting) and only then changes the targets, so no frame draws with a pipeline that is
// not there. A copy a draw asks for before it is ready is moved up to urgent_ like any other
// pending build. A copy is taken (and recorded for the next start) by the first draw that asks for
// it, not at the end of the frame like other background builds: copies for a level the player went
// back from leave nothing in the cache file.
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

// The blend words of a key (PipelineKey::blend), 4 bits per field: enable, src, dst, op, srcA, dstA,
// opA, write mask. The UI layer split (frame_layers.h) reads them to tell a draw it can separate and
// changes the alpha part.
inline uint32_t PackBlend(const plume::RenderBlendDesc& b) {
  return uint32_t(b.blendEnabled) | (uint32_t(b.srcBlend) << 4) | (uint32_t(b.dstBlend) << 8) |
         (uint32_t(b.blendOp) << 12) | (uint32_t(b.srcBlendAlpha) << 16) |
         (uint32_t(b.dstBlendAlpha) << 20) | (uint32_t(b.blendOpAlpha) << 24) |
         (uint32_t(b.renderTargetWriteMask) << 28);
}

inline plume::RenderBlendDesc UnpackBlend(uint32_t v) {
  plume::RenderBlendDesc b;
  b.blendEnabled = (v & 0xF) != 0;
  b.srcBlend = plume::RenderBlend((v >> 4) & 0xF);
  b.dstBlend = plume::RenderBlend((v >> 8) & 0xF);
  b.blendOp = plume::RenderBlendOperation((v >> 12) & 0xF);
  b.srcBlendAlpha = plume::RenderBlend((v >> 16) & 0xF);
  b.dstBlendAlpha = plume::RenderBlend((v >> 20) & 0xF);
  b.blendOpAlpha = plume::RenderBlendOperation((v >> 24) & 0xF);
  b.renderTargetWriteMask = uint8_t((v >> 28) & 0xF);
  return b;
}

// What a draw needs besides the key to create its pipeline.
struct PipelineInputs {
  const ShaderStore::Entry* vs = nullptr;
  const ShaderStore::Entry* ps = nullptr;
  const InputLayout* input_layout = nullptr;
  // The draw's targets take the host's MSAA level (EdramTargets::HostSamplesFollowLevel): their
  // guest sample count, 2 or 4; 0 for any other draw. Only for Pipelines::Get, which remembers the
  // pipeline of such a draw for PrebuildForLevel.
  uint32_t msaa_guest_samples = 0;
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

  // Takes the device, the layout and the shader store (which may still be pending: its Find
  // then returns null and no draw reaches the pipelines). Starts nothing: no cache is read and
  // no thread runs until StartPersistence.
  void Initialize(Device& device, const GuestLayout& layout, ShaderStore& shaders);
  // The second half of the start, once the shader pack is attached (the records are checked
  // against it, and the precache workers call GetShader): reads the cache file and the starter
  // list, then starts the precache. Called right after Initialize when a pack was in place at
  // start, and from the main thread after the pack was built and attached otherwise; with the
  // guest not drawing yet. A second call does nothing.
  void StartPersistence(ShaderStore& shaders);
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

  // The host MSAA level is about to become `level` (2, 4 or 8): hands the builder threads one copy,
  // at the sample count `level` gives it, of every pipeline Get was asked for with
  // PipelineInputs::msaa_guest_samples set (the sample count is `level`, also where the guest's
  // is higher). A pipeline that is already there, queued or being made for another reason is not
  // made again. Render thread. Calling it again for the same or another level starts a new count.
  struct LevelPrebuild {
    uint32_t known = 0;    // pipelines remembered (draws into targets that take the level)
    uint32_t present = 0;  // ... whose copy at `level` was there already or needs nothing made
    uint32_t queued = 0;   // copies handed to the builders by this call
    uint32_t waiting = 0;  // copies, this call's or earlier ones, not finished yet
  };
  LevelPrebuild PrebuildForLevel(uint32_t level);
  // Copies of the last PrebuildForLevel not finished yet (being made or waiting for a thread).
  // 0: the targets may change. A copy that failed counts as finished: its draws are made (and the
  // failure reported) the usual way.
  uint32_t PrebuildWaiting() const;

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
  static uint32_t UsableProcessors();
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
  void PrecacheWorker(uint32_t index);

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
  // Get's slow path: the key is in neither map.
  plume::RenderPipeline* Resolve(const PipelineKey& key, const PipelineInputs& inputs,
                                 const Deferral* deferral, bool* pending);
  // Remembers a pipeline just had for a draw whose targets take the MSAA level (PrebuildForLevel).
  void NoteLevelDependent(const PipelineKey& key, const PipelineInputs& inputs);

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
  // persist_ is atomic because StartPersistence sets it (last) on the thread that attaches the
  // pack while the render thread may already be running idle ticks.
  std::atomic<bool> persist_{false};
  bool persistence_started_ = false;  // StartPersistence ran (main thread)
  std::filesystem::path cache_path_;  // <exe>/cache/mcla_gpu_pipelines_<api>.bin
  std::filesystem::path other_cache_path_;
  std::filesystem::path starter_path_;  // <exe>/assets/mcla_pipelines.bin
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
    uint32_t index = 0;               // its place in precached_ (and in records_)
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
  // Where the workers are in the list (work_mutex_): the next entry and the end of the run they
  // are in. The entries a jump of the cursor passed over (FollowRenderThread) wait in
  // list_skipped_ and are made after the run.
  struct ListRange {
    size_t begin = 0;
    size_t end = 0;
  };
  size_t list_cursor_ = 0;
  size_t list_end_ = 0;
  std::deque<ListRange> list_skipped_;
  std::atomic<bool> stop_precache_{false};
  std::atomic<uint32_t> precache_ready_{0};
  std::atomic<uint32_t> precache_failed_{0};
  std::atomic<uint32_t> precache_finished_{0};  // entries built by a worker or claimed here
  std::chrono::steady_clock::time_point precache_start_;
  std::mutex precache_mutex_;
  std::condition_variable precache_done_;

  // Boot mode (fewer than kBootBoostBelow usable processors, a list to make): on a first start
  // the title's first frame asks for hundreds of the list's pipelines at once, and two workers
  // at below normal priority, sharing four processors with the guest's own threads, made them
  // slower than the guest asked. Until the list is done or kBootSeconds have passed, up to all
  // the processors but one make pipelines at normal priority, and a claim by the render thread
  // moves the workers on to the entries after it (FollowRenderThread). Then the extra workers
  // leave and the others drop to below normal, as with more processors, which never have it.
  std::atomic<bool> boot_mode_{false};
  uint32_t steady_workers_ = 0;           // the workers that stay: indices below it
  std::chrono::steady_clock::time_point boot_deadline_;
  std::atomic<uint32_t> boot_claims_{0};  // entries the render thread made itself in boot mode
  std::atomic<uint32_t> boot_waits_{0};   // ... and builds under way it waited for
  void EndBootMode(const char* why);
  // The render thread made entry `index` itself in boot mode: the workers go on after it, and the
  // entries between their place and it are made once the run is over.
  void FollowRenderThread(size_t index);
  void NoteClaim(size_t index);
  bool HasListWork() const;                // work_mutex_ held
  bool NextListEntry(size_t& index);       // work_mutex_ held

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
    // A copy for a coming MSAA level (PrebuildForLevel), in low_ until a draw asks for it (then
    // `promoted`: it was moved up to urgent_). Render thread only.
    bool low = false;
    bool promoted = false;
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
  // After urgent_, before the precache list: the copies for a coming MSAA level (work_mutex_).
  std::deque<Urgent> low_;
  // The pipelines drawn into targets that take the MSAA level, apart from their sample count: the
  // session key with sample_count = the draw's guest samples (2 or 4), and the stored layout.
  struct LevelDependent {
    PipelineKey key;
    uint32_t stored = 0;
  };
  std::vector<LevelDependent> level_dependent_;
  std::unordered_set<PipelineKey, PipelineKeyHash> level_dependent_index_;
  // The copies PrebuildForLevel waits for.
  std::vector<std::shared_ptr<Request>> prebuild_requests_;
  std::vector<Precached*> prebuild_precached_;
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
