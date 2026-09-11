#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest buffer -> D3D12 resource cache
// ===========================================================================
// Vertex and index buffers only. Textures live in texture_cache.*.
//
// The cache separates the PHYSICAL RESOURCE from the PER-DRAW BINDING:
//
//   guest address range  ->  one ID3D12Resource covering a REGION
//   vertex fetch         ->  view offset/stride/size inside that region
//
// That split is required by the observed data: 16k unique fetch ranges per
// session with ~9k of them intersecting and ~6.5k fully contained in another
// (the game suballocates geometry out of pools). Creating one resource per
// fetch range would duplicate the same bytes many times over; instead
// overlapping requests are merged into a single region and each draw binds a
// subrange of it.
//
// Addresses in fetch constants are PHYSICAL (the Xenos reads memory
// directly) — see guest/guest_resources.h. They are translated through the
// physical membase, never the virtual one.
//
// Invalidation: dynamic buffers go through grcVertexBufferD3D::Lock/Unlock;
// an Unlock hook calls InvalidateRange so intersecting regions re-upload.
// Static geometry ships pre-baked in RPF resources and uploads once.
// ===========================================================================

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

// Guest memory is big-endian and the Xenos applies the swap during the fetch,
// so the bytes must be swapped before D3D12's little-endian input assembler
// reads them. The width comes from the fetch constant, NOT from the vfetch
// instruction — VertexFetchInstruction has no endian field at all, and the
// D3D9 vfetch patcher (sub_82423A38) only ever writes format, the two format
// flags, the fetch constant index, stride and offset.
//
// Observed over a full session: all 1984 vertex buffers use k8in32 and all
// 1946 index buffers use k8in16.
//
// A whole-dword swap is correct for every element format in use because
// DXGI's little-endian field extraction from a dword matches the Xenos'
// bit-field extraction from the already-swapped dword. For k_16_16_FLOAT the
// Xenos takes x from bits [15:0], which is exactly the pair of bytes DXGI
// reads first.
enum class BufferSwap : uint32_t {
  kNone = 0,
  k8in16 = 1,  // 16-bit indices
  k8in32 = 2,  // vertex data
  kCount = 3,
};

// Endian-swapping copy, exported for callers that have to build a vertex
// buffer themselves instead of resolving one. The swap has to match what the
// cache would have applied, or the same bytes reach the GPU two different ways
// in the same draw.
void SwapCopyBytes(uint8_t* dst, const uint8_t* src, uint32_t size, BufferSwap swap);

// What a draw needs to build a vertex/index buffer view.
struct BufferBinding {
  ID3D12Resource* resource = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;  // of the requested range
  uint32_t region_base = 0;                   // guest base of the region
  uint32_t region_size = 0;
  uint32_t view_offset = 0;  // requested address - region_base
  bool valid() const { return resource != nullptr; }
};

class BufferCache {
 public:
  struct Stats {
    uint64_t hits = 0;
    uint64_t uploads = 0;        // first-use uploads (expected, not errors)
    uint64_t reuploads = 0;      // invalidation-driven
    // How many of those the guest told us about itself, against the page
    // write watch. The unlock range is exact and costs no fault, so the more
    // of the total it accounts for, the less the watch is earning.
    uint64_t unlock_invalidations = 0;
    uint64_t merges = 0;         // regions coalesced by an overlapping request
    uint64_t merge_declined = 0; // merge refused: union would exceed the cap
    uint64_t upload_failures = 0;  // real errors
    // ---- TEMP INSTRUMENTATION: where the geom milliseconds go ----------------
    // Baseline says geom = 25ms/frame with 437 re-uploads and only 2 exact
    // unlock notifications, so the page write watch is driving almost all of
    // them. These answer the three questions that decide whether that is
    // removable, before anything is changed:
    //   1. is the time really the uploads, or the lookups?      -> upload_us
    //   2. which source dirties the regions?    -> thunk_ranges / unlock ranges
    //   3. do the re-uploaded BYTES actually differ, or is the
    //      region being re-sent unchanged?               -> reuploads_identical
    double upload_us = 0;             // wall time inside UploadRegion
    uint64_t upload_bytes = 0;        // bytes copied by first-use uploads
    uint64_t reupload_bytes = 0;      // bytes copied by invalidation re-uploads
    uint64_t reuploads_identical = 0; // re-upload whose content hash was unchanged
    uint64_t thunk_ranges = 0;        // invalidation ranges from the page watch
    uint64_t thunk_bytes = 0;
    uint64_t unlock_bytes = 0;        // bytes covered by exact unlock ranges
    uint64_t regions_dirtied = 0;     // regions actually flipped to dirty
    // InvalidateRange walks EVERY region for EVERY pending range. If the
    // region count is in the thousands and the watch fires hundreds of times a
    // frame, this is a quadratic scan sitting in the middle of the draw path,
    // and it would look exactly like "geom is slow" without naming itself.
    uint64_t inval_scan_steps = 0;
    uint64_t region_count = 0;  // live regions, both swaps
    uint64_t unreadable = 0;       // guest range not committed
    // Re-verification backstop: regions hashed, bytes hashed, and the ones that
    // came back different from what was uploaded -- i.e. invalidations the
    // write watch lost. A non-zero `verify_catches` is the bug reproducing.
    uint64_t verify_regions = 0;
    uint64_t verify_bytes = 0;
    uint64_t verify_catches = 0;
    // Why the most recent Resolve failed. Without this a failure is just
    // "could not be resolved", which names five different causes.
    const char* last_failure = nullptr;
    uint32_t last_failure_addr = 0;
    uint32_t last_failure_size = 0;
  };

  void Shutdown(D3D12Context& context);

  // Logs the hits/uploads/reuploads split every 600 calls. Call once per frame.
  void ReportPeriodic();

  // Resolves [guest_address, guest_address + size) to a native resource plus
  // the offset of that range inside it. Records any upload on the current
  // frame's command list. Returns false on failure (diagnosed via stats).
  bool Resolve(D3D12Context& context, ID3D12GraphicsCommandList* cl, uint32_t guest_address,
               uint32_t size, BufferSwap swap, BufferBinding& out);

  // Marks every region intersecting the range dirty; they re-upload on the
  // next Resolve.
  void InvalidateRange(uint32_t guest_address, uint32_t size);

  // The guest's OWN dirty range, from D3DResource_Unlock. Queued rather than
  // applied: InvalidateRange walks regions_, which the render thread mutates,
  // and this runs on whichever guest thread did the unlock. The queue is
  // drained at the top of Resolve, exactly where the write watch's is.
  //
  // This is the range the resource has been accumulating in BaseFlush (+0x14),
  // packed 16.16 in 128-byte units -- exact bytes, no page-protection fault,
  // and it arrives when the guest itself considers the data final. Measured on
  // MCLA: vertex buffers are locked ~3600 times per 30 s (resource type 1)
  // while textures are locked only at load, so this is where that signal is
  // worth anything.
  void NoteGuestWrite(uint32_t guest_address, uint32_t size);

  // Starts watching guest writes so InvalidateRange fires by itself. Without
  // this nothing ever marked a region dirty, so a guest range uploaded once was
  // never uploaded again -- and MCLA recycles vertex buffer memory constantly
  // while streaming, so a later mesh placed at the same address rendered with
  // the PREVIOUS mesh's bytes. Measured in "native bugs.rdc": draw 22001's
  // fetch constant says 65912 bytes at 28 bytes per vertex (2354 vertices,
  // exact), while the bytes at that address have a 40-byte period, and the
  // stale vertices collapse into the black stretched polygons that take over
  // the screen near the ground.
  //
  // Safe to call more than once; a failure only means invalidation stays
  // manual, so it degrades to the old behaviour rather than breaking.
  bool StartWatchingGuestWrites();
  void StopWatchingGuestWrites();

  // Drains the invalidations the watch callback queued from other threads.
  // Called at the top of Resolve, where the regions are already being touched.
  void ApplyPendingInvalidations();

  const Stats& stats() const { return stats_; }

  // Live regions and the bytes they occupy, for the memory census.
  void Census(size_t& count, uint64_t& bytes) const;

  // TEMP DIAG (MESHSTALE): does the region backing this range still match the
  // guest memory it was uploaded from?
  //
  // The stale-region bug this cache exists to prevent is invisible until it
  // reaches the screen, and by then the frame is gone. This compares the hash
  // taken of the GUEST bytes at upload time against a fresh hash of the same
  // bytes now, so a missed invalidation is caught the moment it happens rather
  // than when it happens to deform something the eye catches.
  //
  // Hashes guest memory on both sides, never the upload heap -- reading back
  // from write-combined staging took geom from 25ms to 114ms a frame the last
  // time it was tried.
  //
  // Returns false when there is no region for the range (nothing to say).
  bool VerifyRegion(uint32_t guest_address, uint32_t size, BufferSwap swap,
                    uint32_t* out_region_base, uint32_t* out_region_size, uint64_t* out_uploaded,
                    uint64_t* out_live);

  // GPU address of a small, permanently zero buffer, created on first use.
  // Bound with stride 0 for an attribute the vertex declaration does not
  // supply: the Xenos leaves that vfetch unpatched and the shader reads zeros,
  // so a zero stream reproduces the hardware exactly. Returns 0 on failure.
  D3D12_GPU_VIRTUAL_ADDRESS ZeroStreamAddress(D3D12Context& context);

 private:
  struct Region {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t base = 0;
    uint32_t size = 0;
    bool dirty = true;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    // Hash of the guest bytes as they were last uploaded. Only read by the
    // instrumentation: it answers whether an invalidation-driven re-upload is
    // actually carrying new data, which is the difference between "the guest
    // rewrites this every frame" and "the watch fired on a page that did not
    // change" -- opposite conclusions, and nothing in the counters told them
    // apart.
    uint64_t content_hash = 0;
    // Per-4 KiB hashes of the guest bytes this region was last uploaded from,
    // plus the frame each block was last re-checked.
    //
    // Whole-region verification was correct and unaffordable: 25 MB hashed per
    // frame took wall from ~55 ms to ~150 ms. A draw only reads the sub-range
    // it binds, so only the blocks that sub-range covers need checking, and a
    // block bound by twenty draws is checked once a frame.
    std::vector<uint64_t> block_hash;
    std::vector<uint32_t> block_frame;
  };
  static constexpr uint32_t kVerifyBlock = 4096;

  using RegionMap = std::map<uint32_t, Region>;

  Region* FindContaining(RegionMap& map, uint32_t address, uint32_t size);

  // Regions sorted by PHYSICAL start, which is the space invalidation works in.
  // Without it, marking one written range dirty walked every region: measured
  // at 1141 live regions and ~991 non-empty drains a frame, that was 1.49
  // MILLION tree-node visits per frame (~22ms), and it accounted for nearly all
  // of BufferCache::Resolve's 25.5ms. The uploads it was blamed for are 1.8ms.
  //
  // Rebuilt lazily, and that is cheap because regions are almost static: first
  // -use uploads measure 0.5 per frame. Region pointers come from a std::map,
  // whose nodes are stable, so an entry stays valid until its region is erased
  // -- every insert and erase sets the stale flag.
  struct RegionIndexEntry {
    uint64_t physical_lo = 0;
    uint64_t physical_hi = 0;
    Region* region = nullptr;
  };
  std::vector<RegionIndexEntry> region_index_;
  bool region_index_stale_ = true;
  void RebuildRegionIndex();
  bool UploadRegion(D3D12Context& context, ID3D12GraphicsCommandList* cl, Region& region,
                    BufferSwap swap);
  // Re-arms the write watch over a region. The watch is consumed when it fires,
  // so it has to be set again after every upload.
  void WatchRegion(const Region& region);

  // The guest thread that writes the memory runs this, so it may not touch
  // regions_ -- it only appends, and Resolve drains.
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context_ptr,
                                                         uint32_t physical_address_start,
                                                         uint32_t length, bool exact_range);

  // One map per swap width. The swapped bytes are baked into the resource, so
  // the same guest range fetched with two different widths cannot share one:
  // keeping the maps separate makes that impossible by construction instead of
  // relying on every caller to pass a consistent width.
  RegionMap regions_[uint32_t(BufferSwap::kCount)];  // keyed by guest base, non-overlapping
  // Ranges the guest wrote since the last drain. Guarded because the callback
  // runs on whichever thread performed the write.
  std::mutex invalidation_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> pending_invalidations_;
  void* invalidation_handle_ = nullptr;
  // Bumped by ReportPeriodic, which the frame boundary already calls once a
  // frame; only used to throttle the re-verification to once per region.
  uint64_t frame_ = 0;
  // Shared all-zero vertex stream. A default-heap buffer is zero-initialised by
  // D3D12 on creation, so it needs no upload.
  Microsoft::WRL::ComPtr<ID3D12Resource> zero_stream_;
  Stats stats_;
};

}  // namespace mcla::native_gfx
