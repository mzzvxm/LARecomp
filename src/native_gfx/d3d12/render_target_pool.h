#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — render targets and the resolve bridge
// ===========================================================================
// The Xenos renders into EDRAM and only makes an image samplable by RESOLVING
// it into main memory; the address a texture fetch points at is therefore the
// resolve DESTINATION, never the EDRAM base. Reproducing that is what lets a
// pass feed a later one.
//
// The guest side is D3DDevice_Resolve (sub_82420BA8), decompiled:
//
//   sub_82420BA8(dev, flags, pSourceRect, pDestTexture, pDestPoint,
//                DestLevel, DestSliceOrFace, pClearColor, ClearZ, ...)
//
//   flags & 7 == 4   -> the source is the DEPTH buffer (dev+12456);
//                       0..3 select colour render targets
//   pDestTexture     -> a D3D texture object whose dwords at +28/+32/+36 are
//                       the fetch constant a later draw will use, so the
//                       destination address is decoded with exactly the same
//                       code path as any other texture
//   the function writes (*(pDestTexture+32) & 0xFFFFF000) & 0x1FFFFFFF plus a
//   tile offset into dev+10780, which is RB_COPY_DEST_BASE (0x2319)
//
// So: a resolve is "copy the target I have been rendering into, to this guest
// address". This pool keeps both halves — the live targets, keyed by their
// configuration, and the resolved copies, keyed by guest address — and answers
// texture fetches against the latter.
//
// Observed in MCLA: every material draw samples one k_24_8 (D24S8) texture at
// 0x06E65000, 1280x1280 — the cascaded shadow atlas, four 640x640 tiles. It is
// the only unresolved fetch in the whole scene pass, and without it the shadow
// term in every shader reads whatever texture happens to sit at descriptor 0.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

#include "texture_cache.h"

namespace mcla::native_gfx {

class D3D12Context;

// Where a resolve reads from and where it writes to. The shadow map resolves
// four 640x640 cascades into one 1280x1280 atlas, so a resolve is a sub-rect
// copy, not a whole-surface one.
struct ResolveRegion {
  int32_t src_x = 0, src_y = 0;
  int32_t dst_x = 0, dst_y = 0;
  uint32_t width = 0, height = 0;
};

// Everything that has to match for two draws to share a render target.
struct RenderTargetKey {
  uint32_t rt_format = 0;  // DXGI
  uint32_t ds_format = 0;  // DXGI
  uint32_t sample_count = 1;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator<(const RenderTargetKey& o) const;
  bool operator==(const RenderTargetKey& o) const;
};

// One colour + depth pair plus its view heaps.
struct RenderTarget {
  Microsoft::WRL::ComPtr<ID3D12Resource> color;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap;
  RenderTargetKey key;
  // Readable view of the typeless depth resource; the DSV format cannot be
  // used for an SRV, so this is what a shader must bind it with.
  uint32_t depth_shader_format = 0;
  // The value the depth target is cleared to, decided by the pass's own
  // viewport rather than assumed. Hardcoding 0.0 (right for the reverse-Z main
  // pass) left every normal-Z pass rejecting all its geometry, so the shadow
  // map resolved to an all-zero atlas.
  float clear_depth = 0.0f;
  // Tracked because the depth resource alternates between being written as a
  // DSV and read as an SRV. Sampling it while it is still in DEPTH_WRITE is
  // invalid and removes the device.
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  // Tracked for the same reason as the depth: a colour resolve has to take
  // the target out of RENDER_TARGET to copy from it, and put it back.
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  bool cleared = false;  // cleared once, then accumulated into
};

class RenderTargetPool : public RenderTargetLookup {
 public:
  struct Stats {
    uint64_t targets_created = 0;
    uint64_t resolves = 0;
    uint64_t resolves_depth = 0;
    uint64_t resolve_copies_created = 0;
    uint64_t lookup_hits = 0;
    uint64_t lookup_misses = 0;
    // TEMP INSTRUMENTATION: why a lookup missed.
    uint64_t miss_no_entry = 0;
    uint64_t miss_no_resource = 0;
    uint64_t miss_too_small = 0;
    uint64_t miss_kind_mismatch = 0;  // depth resolve fetched as colour or vice versa
    uint64_t resolves_without_target = 0;  // resolve of a pass we never rendered
    uint64_t targets_refused = 0;          // over the budget below
    uint64_t bytes_allocated = 0;
  };

  void Shutdown(D3D12Context& context);

  // Returns the target for this configuration, creating it on first use.
  // Never null unless creation fails.
  RenderTarget* Acquire(D3D12Context& context, const RenderTargetKey& key,
                        float clear_depth);

  // Looks up an EXISTING target without creating one. A resolve must never
  // create: the render state at resolve time is not a request to allocate, and
  // trusting it as one allocates a target per bogus viewport until the GPU
  // runs out of memory (observed: every later resource creation failing).
  RenderTarget* Find(const RenderTargetKey& key);

  // Marks every pooled target as un-cleared so the next frame re-clears it on
  // its first draw. Continuous mode re-renders the whole scene each frame; the
  // clear was a one-shot latch (`cleared` set once, then accumulated), which is
  // right for a single bounded capture but leaves moving objects trailing in
  // continuous mode -- the previous frame's geometry is never erased. Called at
  // each continuous frame boundary.
  void MarkAllUncleared();

  // Puts a target's depth back into DEPTH_WRITE before it is bound as a DSV
  // again, undoing the transition RecordResolve made to let it be sampled.
  void PrepareForRendering(ID3D12GraphicsCommandList* cl, RenderTarget& target);

  // Registers a DEPTH resolve without touching a command list. A resolve must
  // not submit work: the guest performs hundreds per frame, and opening and
  // submitting a frame for each one (with its fence wait, on the queue shared
  // with the Xenia command processor) hangs the GPU — observed as
  // DXGI_ERROR_DEVICE_HUNG 0x887A0001. The state transition the depth
  // resource needs is queued instead and issued by FlushPendingTransitions on
  // the next draw's command list.
  void NoteResolve(RenderTarget& source, bool from_depth, uint32_t dest_address,
                   uint32_t dest_width, uint32_t dest_height, const ResolveRegion& region);

  // Issues the barriers queued by NoteDepthResolve.
  void FlushPendingTransitions(ID3D12GraphicsCommandList* cl);

  // Issues the depth copies queued by NoteDepthResolve.
  void FlushPendingCopies(D3D12Context& context, ID3D12GraphicsCommandList* cl);

  // Records a resolve: copies `source` (colour or depth) into a shader
  // readable texture registered at `dest_address`. Single-sampled targets are
  // copied, multisampled ones are resolved.
  void RecordResolve(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                     RenderTarget& source, bool from_depth, uint32_t dest_address,
                     uint32_t width, uint32_t height);

  // RenderTargetLookup.
  ID3D12Resource* FindResolvedTarget(uint32_t guest_address, uint32_t width, uint32_t height,
                                     bool want_depth,
                                     D3D12_RESOURCE_STATES* out_state) override;

  // Registers a resolve destination directly against an existing resource,
  // used for depth: a typeless depth target is already the image a fetch
  // wants, so copying it into a colour texture would only lose precision (and
  // D3D12 has no depth resolve or depth-to-colour copy anyway).
  bool IsGpuProduced(uint32_t guest_address, uint32_t width,
                     uint32_t height) const override;
  // TEMP INSTRUMENTATION: address is a known resolve destination but the
  // extent differs, i.e. the memory was reused by an ordinary texture.
  bool IsStaleGpuAddress(uint32_t guest_address, uint32_t width,
                         uint32_t height) const override;

  // Records that the guest resolved to this address, whether or not we can
  // produce the contents yet.
  void NoteDestination(uint32_t dest_address, uint32_t width, uint32_t height);

  void RegisterDirect(uint32_t dest_address, ID3D12Resource* resource, uint32_t width,
                      uint32_t height, uint32_t shader_format, D3D12_RESOURCE_STATES state);

  // Writes every registered resolve destination to a .tga next to the report.
  // Counting a fetch as "resolved" only proves a resource was handed back; it
  // says nothing about the contents. The shadow atlas is the case that matters:
  // if it comes out empty or uniform, the bridge is connected but the pass that
  // fills it is not.
  void DumpResolved(D3D12Context& context, const std::filesystem::path& dir, FILE* log);

  const Stats& stats() const { return stats_; }

  // Memory census. `targets` is the pool proper (budgeted); `resolved` is the
  // per-destination copies, which are NOT budgeted and grow with the number of
  // distinct resolve destinations the guest uses.
  struct Census {
    size_t targets = 0;
    uint64_t target_bytes = 0;  // the pool's own running total
    size_t resolved = 0;
    uint64_t resolved_bytes = 0;
    size_t orphaned = 0;
    size_t pending_copies = 0;
    size_t gpu_produced = 0;
  };
  Census TakeCensus(ID3D12Device* device) const;

 private:
  struct ResolvedCopy {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgi_format = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    // Colour copies are allocated here; depth entries borrow a pooled target.
    bool owned = false;
    // Which kind of surface the resource was CREATED for. The guest reuses a
    // destination address for a colour resolve and a depth resolve of the same
    // size, and matching on the extent alone handed the depth resolve the
    // colour resource: CopyTextureRegion then failed with "source format is
    // R32_TYPELESS and the destination format is R16G16B16A16_TYPELESS",
    // which killed the whole command list.
    bool from_depth = false;
  };

  // A pass renders into one target, but the guest cycles through many
  // transient configurations; pooling every one of them exhausted GPU memory
  // and every later resource creation then failed (texture, PSO, buffer).
  // Budget both the count and the footprint, and refuse past it rather than
  // letting an allocation failure surface somewhere unrelated.
  //
  // The COUNT was the binding limit at 10, and it is the wrong thing to bind
  // on: it charged a 4x4 target the same as a 1280x720 one. MCLA's exposure
  // and bloom chain is a run of tiny targets (128x128, 64x64, 16x16, 4x4,
  // 1x1), so the count ran out while only 29 MiB of the byte budget was in
  // use — every one of those passes was refused, none of them resolved, and
  // the composite that samples the chain came out black. The footprint is the
  // constraint that actually protects against exhaustion, so the count is now
  // only a guard against unbounded growth.
  static constexpr uint32_t kMaxTargets = 64;
  static constexpr uint64_t kMaxTargetBytes = 512ull * 1024 * 1024;

  std::map<RenderTargetKey, RenderTarget> targets_;
  // Address -> the extent that was resolved there. The address alone is not
  // enough: the guest reuses memory, so an ordinary texture allocated where a
  // resolve destination used to live would be taken for GPU-produced data and
  // handed the neutral fallback -- an object that simply loses its texture.
  struct ProducedExtent {
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::map<uint32_t, ProducedExtent> gpu_produced_;
  std::map<uint32_t, ResolvedCopy> resolved_;  // keyed by guest destination address
  std::vector<RenderTarget*> pending_to_shader_;
  // Depth copies still to be issued; a resolve must not submit work.
  struct PendingCopy {
    RenderTarget* source = nullptr;
    uint32_t dest_address = 0;
    ResolveRegion region;
    bool from_depth = false;
  };
  std::vector<PendingCopy> pending_copies_;
  // Resolve-destination resources retired when the guest reuses a destination
  // address with a new format/size. NoteResolve has no D3D12Context so it cannot
  // defer-release directly; it parks the old resource here and FlushPendingCopies
  // (which has the context) drains it through the fence-gated DeferRelease. This
  // replaces the previous Detach()-and-leak, which grew VRAM unbounded.
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> orphaned_resources_;
  Stats stats_;
};

}  // namespace mcla::native_gfx
