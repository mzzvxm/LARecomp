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
    uint64_t merges = 0;         // regions coalesced by an overlapping request
    uint64_t upload_failures = 0;  // real errors
    uint64_t unreadable = 0;       // guest range not committed
    // Why the most recent Resolve failed. Without this a failure is just
    // "could not be resolved", which names five different causes.
    const char* last_failure = nullptr;
    uint32_t last_failure_addr = 0;
    uint32_t last_failure_size = 0;
  };

  void Shutdown(D3D12Context& context);

  // Resolves [guest_address, guest_address + size) to a native resource plus
  // the offset of that range inside it. Records any upload on the current
  // frame's command list. Returns false on failure (diagnosed via stats).
  bool Resolve(D3D12Context& context, ID3D12GraphicsCommandList* cl, uint32_t guest_address,
               uint32_t size, BufferSwap swap, BufferBinding& out);

  // Marks every region intersecting the range dirty; they re-upload on the
  // next Resolve.
  void InvalidateRange(uint32_t guest_address, uint32_t size);

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
  };

  using RegionMap = std::map<uint32_t, Region>;

  Region* FindContaining(RegionMap& map, uint32_t address, uint32_t size);
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
  // Shared all-zero vertex stream. A default-heap buffer is zero-initialised by
  // D3D12 on creation, so it needs no upload.
  Microsoft::WRL::ComPtr<ID3D12Resource> zero_stream_;
  Stats stats_;
};

}  // namespace mcla::native_gfx
