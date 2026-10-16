#pragma once
// ===========================================================================
// MCLA GPU renderer -- the resolve memory: what resolves write, as guest bytes
// ===========================================================================
// (M5.2b). On the console a resolve
// writes guest memory, and any later fetch of those bytes -- with the
// destination's own shape or another one -- reads what the resolve left. The
// resolve bridge (resolve.h) keeps each destination as a host texture keyed by
// the destination's fetch, which serves the fetches with that key; this keeps
// the bytes themselves, on the GPU, for every other reader: a fetch with
// another format, size, tiling or base over memory resolves wrote
// (reinterpretation), decoded from here in its own layout.
//
// Storage. Guest physical memory in 64 KiB chunks; a chunk gets a place in a
// pool buffer the first time a resolve or a read needs it, through a table of
// kChunkCount entries the shaders read (hlsl/resolve_memory.hlsli). The pool
// starts at kInitialPoolChunks and doubles when full (copied on the GPU, in
// list order; the old buffer retired by the FrameRing). Chunks are never given
// back: the RAGE render-target pools resolve into fixed ranges (425 chunks,
// 26.6 MiB, in the measured frames).
//
// Pages (4 KiB, the CPU's bookkeeping):
//  - valid: the pool holds the console's bytes for the whole page. Set when the
//    page is uploaded from guest memory or a resolve covers all of it; cleared
//    when the CPU writes it (the write watch, armed on valid pages).
//  - newer: a resolve wrote bytes there that guest memory does not hold. A
//    fetch over such a page (and no product of its image) reads from here.
//  - a sequence number, bumped by every write into the page, so a decoded
//    texture knows when it is stale.
// A resolve that covers a page only in part needs the rest of it: the page is
// uploaded from guest memory first unless it is already valid.
//
// Encode (hlsl/resolve_memory_cs.hlsl): a compute dispatch writes a resolve's
// rectangle as bytes in the destination's layout and endian -- colour from
// the product the conversion pass wrote, depth from the EDRAM target (stencil
// included). Depth is encoded right after its resolve: the EDRAM target
// changes with the next draw. Colour waits in a batch (each dispatch after its
// conversion pass was a GPU bubble: 1.1 ms/frame for 56 resolves):
// the product keeps its texels until a resolve writes it again, so the batch
// is recorded at the end of the frame, or earlier when something must see the
// bytes or the product changes -- before a decode, a lock, a depth encode, or
// a resolve into the same level that overlaps a waiting rectangle without
// covering it (one that covers it replaces it). Dispatches whose pages overlap
// keep their order behind a barrier; the rest run back to back.
//
// Decode (hlsl/resolve_memory_ps.hlsl): a full-screen pass per level into a
// host texture with the fetch's key, cached until a page of its range changes.
// Formats: the uncompressed ones of format.h's table; block-compressed reads
// are reported (kUnsupported) and left to the guest-memory path.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "guest_memory.h"
#include "region_index.h"
#include "textures.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class HostPasses;
class UploadRing;

class ResolveMemory {
 public:
  static constexpr uint32_t kChunkShift = 16;  // 64 KiB
  static constexpr uint32_t kChunkBytes = 1u << kChunkShift;
  static constexpr uint32_t kChunkCount = 8192;  // 512 MiB of physical memory
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageBytes = 1u << kPageShift;
  static constexpr uint32_t kPageCount = kChunkCount << (kChunkShift - kPageShift);
  static constexpr uint32_t kPagesPerChunk = 1u << (kChunkShift - kPageShift);
  static constexpr uint32_t kNoChunk = 0xFFFFFFFFu;
  static constexpr uint32_t kInitialPoolChunks = 512;  // 32 MiB
  static constexpr uint64_t kEvictFrames = 600;

  ResolveMemory() = default;
  ~ResolveMemory();
  ResolveMemory(const ResolveMemory&) = delete;
  ResolveMemory& operator=(const ResolveMemory&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  GuestMemory& memory, HostPasses& passes, TextureCache& textures);
  // GPU idle, FrameRing drained, before the BindlessHeap shuts down.
  void Shutdown();

  void BeginFrame(uint32_t slot);
  // Takes the write notifications (TextureCache::Sync calls it once per draw).
  void Sync();

  // One resolve's copy (resolve.cpp Execute), after its conversion pass.
  struct Copy {
    const TextureKey* key = nullptr;  // the destination
    uint32_t level = 0;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // texels of the level, x1/y1 exclusive
    uint32_t mode = 0;  // the conversion and its flags (resolve.h ResolveCache::Mode)
    // Colour: the product's view of every level, unswizzled.
    uint32_t product_slot = 0;
    // Depth: the EDRAM target's planes and how the copy reads them.
    bool depth = false;
    bool msaa = false;
    uint32_t source_slot = 0;
    uint32_t stencil_slot = 0;
    int32_t offset_x = 0;  // EDRAM pixel = destination texel + offset
    int32_t offset_y = 0;
    uint32_t samples = 0;  // as resolve_ps.hlsl takes them
  };
  // Records the copy's bytes into the resolve memory, outside any render pass,
  // leaving none open; binds its own compute state. False when it could not
  // (logged, counted): the pages it would have written stay as they were.
  bool Encode(plume::RenderCommandList* list, const Copy& copy);
  // Before a resolve writes `level` of the product of `key` over [x0, x1) x
  // [y0, y1): waiting encodes of that level that the rectangle covers are
  // dropped (the new one writes their bytes again); one it overlaps otherwise
  // sends the whole batch first.
  void BeforeResolve(plume::RenderCommandList* list, const TextureKey& key, uint32_t level,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1);
  // Records every waiting encode (end of frame, and before anything reads the
  // pool). Leaves no render pass open.
  void Flush(plume::RenderCommandList* list);

  // Whether a fetch of `key` (its base and mip ranges) reads bytes a resolve
  // wrote that guest memory does not hold.
  bool Holds(const TextureKey& key);

  enum class BindResult : uint8_t { kOk, kUnsupported, kFailed };
  // `key` decoded from the resolve memory, its views by `host_swizzle`:
  // *slot = the bindless slot. Records the decode into `list` when the texture
  // is new or a page of its range changed since; that leaves the list with no
  // framebuffer and the decode pass's own state (*state_changed = true).
  BindResult Bind(const TextureKey& key, uint16_t host_swizzle, plume::RenderCommandList* list,
                  uint32_t* slot, bool* state_changed, std::string* why);

  // A CPU lock of [address, address + size): the
  // pages of the range with bytes guest memory lacks are copied from the pool
  // into guest memory, whole, before the CPU reads them -- the resolves' bytes
  // in their order, whatever product made them. `submit_recorded` sends what
  // the frame recorded so far to the queue; the caller records the encode
  // batch first (Flush). Waits for the GPU. The pages written, or -1 when the
  // copy could not be made (logged).
  int64_t WriteBack(uint32_t address, uint32_t size,
                    const std::function<void()>& submit_recorded);

  std::string TakeSummary(uint64_t frames);

 private:
  enum PageFlag : uint8_t {
    kValid = 1u << 0,
    kNewer = 1u << 1,
    kArmed = 1u << 2,
  };
  enum class PoolAccess : uint8_t { kNone, kCopyWrite, kCopyRead, kComputeWrite, kGraphicsRead };
  // A texture decoded from the resolve memory.
  struct Decoded {
    TextureKey key;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t kind = 0;  // resolve_memory_ps.hlsl kDecode*
    std::unique_ptr<plume::RenderTexture> texture;
    TextureViews views;
    std::vector<std::unique_ptr<plume::RenderTextureView>> level_views;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> level_framebuffers;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    uint32_t ranges[2][2] = {};
    RegionIndex::Id region_ids[2] = {RegionIndex::kNone, RegionIndex::kNone};
    uint64_t decoded_sequence = 0;  // 0 = never decoded
    uint64_t decoded_frame = 0;     // the frame of its last decode
    uint64_t last_frame = 0;
  };

  // Places `chunk` in the pool (growing it when full). False when the pool
  // could not grow (logged).
  bool EnsureChunk(plume::RenderCommandList* list, uint32_t chunk);
  bool GrowPool(plume::RenderCommandList* list);
  bool MakeSets();
  void UploadTable(plume::RenderCommandList* list);
  // Uploads the given pages (not valid) from guest memory; each one uploaded
  // becomes valid. The pages must have their chunks.
  void UploadPages(plume::RenderCommandList* list, const std::vector<uint32_t>& pages);
  void PoolBarrier(plume::RenderCommandList* list, PoolAccess access);
  void ArmPages(const std::vector<uint32_t>& pages);
  // Every decoded texture over [start, start + length): its memoized bindings
  // are dropped (TextureCache::BumpProductGeneration of its pages). `cpu`: the
  // CPU wrote the range (Sync), reported when it lands on a texture decoded in
  // the last frames.
  void TouchDecoded(uint32_t start, uint32_t length, bool cpu);
  plume::RenderPipeline* DecodePipeline(plume::RenderFormat format);
  void DestroyDecoded(Decoded& decoded, bool for_shutdown);
  void EvictSome();
  void Fail(const char* what, const std::string& detail);
  // A colour encode waiting in the batch.
  struct Pending {
    TextureKey key;
    uint32_t level = 0;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t first_page = 0;  // the pages it writes, as a range
    uint32_t last_page = 0;
    uint32_t groups_x = 0;
    uint32_t groups_y = 0;
    std::array<uint32_t, 16> push{};
  };
  std::vector<Pending> pending_;
  // The lock write-back's own list, fence and readback buffer: the copy runs
  // after the work it reads, and the CPU waits for it.
  bool EnsureReadback(uint64_t bytes);
  std::unique_ptr<plume::RenderCommandList> readback_list_;
  std::unique_ptr<plume::RenderCommandFence> readback_fence_;
  std::unique_ptr<plume::RenderBuffer> readback_buffer_;
  uint64_t readback_bytes_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> wave_;  // page ranges of the current wave

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  GuestMemory* memory_ = nullptr;
  GuestMemory::Subscriber subscriber_ = GuestMemory::kNoSubscriber;
  HostPasses* passes_ = nullptr;
  TextureCache* textures_ = nullptr;

  // Descriptor sets and layouts: the encode's (pool read-write, table) and
  // the decode's (pool and table read-only).
  plume::RenderDescriptorSetBuilder encode_set_builder_;
  plume::RenderDescriptorSetBuilder decode_set_builder_;
  uint32_t encode_pool_index_ = 0;
  uint32_t encode_table_index_ = 0;
  uint32_t decode_pool_index_ = 0;
  uint32_t decode_table_index_ = 0;
  std::unique_ptr<plume::RenderDescriptorSet> encode_set_;
  std::unique_ptr<plume::RenderDescriptorSet> decode_set_;
  std::unique_ptr<plume::RenderPipelineLayout> encode_layout_;
  std::unique_ptr<plume::RenderPipelineLayout> decode_layout_;
  // Encode pipelines: [0] colour, [1] depth, [2] multisampled depth.
  std::unique_ptr<plume::RenderShader> encode_shaders_[3];
  std::unique_ptr<plume::RenderPipeline> encode_pipelines_[3];
  std::unique_ptr<plume::RenderShader> decode_shader_;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> decode_pipelines_;

  std::unique_ptr<plume::RenderBuffer> pool_;
  uint32_t pool_chunks_ = 0;  // capacity
  uint32_t used_chunks_ = 0;
  PoolAccess pool_access_ = PoolAccess::kNone;
  std::unique_ptr<plume::RenderBuffer> table_;
  bool table_dirty_ = true;
  bool table_read_barrier_ = false;  // the table was written since its last read barrier

  std::vector<uint32_t> chunk_slot_;   // by chunk: its place in the pool, or kNoChunk
  std::vector<uint8_t> page_flags_;    // by page: PageFlag
  std::vector<uint64_t> page_sequence_;  // by page: the sequence of its last write
  // By page: the xenos TextureFormat of the resolve that wrote it last (0xFF:
  // none since the CPU's), for the report of a texture read in another format.
  std::vector<uint8_t> page_writer_;
  std::vector<TextureKey> decode_logged_;  // keys whose first decode was reported (32 at most)
  std::vector<TextureKey> late_logged_;    // keys reported written by the CPU after a decode
  uint64_t sequence_ = 0;
  uint32_t newer_low_page_ = UINT32_MAX;  // bounds of the pages ever marked newer
  uint32_t newer_high_page_ = 0;

  std::unordered_map<TextureKey, std::unique_ptr<Decoded>, TextureKeyHash> decoded_;
  RegionIndex decoded_regions_;
  std::vector<std::vector<Decoded*>> decoded_owners_;  // by region id

  uint32_t slot_ = 0;
  uint64_t frame_ = 1;
  std::vector<GuestMemory::Range> drained_;
  // Encode scratch: the pages a copy touches and the bytes of each it covers.
  std::vector<std::pair<uint32_t, uint32_t>> touched_;
  std::vector<uint32_t> pages_;
  std::unordered_map<std::string, uint32_t> failures_logged_;  // by kind, the first 4 each

  struct Stats {
    uint64_t encodes = 0;
    uint64_t encodes_dropped = 0;  // replaced by a later resolve of the same rectangle
    uint64_t flushes = 0;          // batches recorded
    uint64_t waves = 0;            // barriers inside batches (overlapping pages)
    uint64_t encoded_bytes = 0;
    uint64_t encode_failures = 0;
    uint64_t pages_uploaded = 0;
    uint64_t chunks_placed = 0;
    uint64_t pool_growths = 0;
    uint64_t holds_checks = 0;
    uint64_t binds = 0;
    uint64_t decodes = 0;
    uint64_t decoded_levels = 0;
    uint64_t bind_unsupported = 0;
    uint64_t bind_failures = 0;
    uint64_t cpu_writes = 0;        // pages the CPU wrote that the pool held
    uint64_t cpu_writes_newer = 0;  // ... of which with bytes guest memory did not hold
    uint64_t evicted = 0;
    uint64_t lock_write_backs = 0;     // locks that copied pages back
    uint64_t lock_pages = 0;           // pages copied back
    uint64_t lock_pages_unknown = 0;   // newer pages the pool does not hold whole (logged)
  };
  Stats stats_;
};

}  // namespace mcla::gpu
