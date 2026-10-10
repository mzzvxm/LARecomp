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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

#include "texture_cache.h"

namespace mcla::native_gfx {

class D3D12Context;

// TEMP DIAG (RECLEAR): which path armed needs_depth_reclear, and on what shape.
//
// Two places arm it and they disagree about intent: NoteResolve INFERS a pass
// end from the shape of a full-source depth resolve, while NotifyResolve reads
// the clear-depth request the guest actually sent. They are indistinguishable
// once the flag is set, so attributing a wrong reclear to one of them needs
// this. Writes distinct site/shape combinations to native_gfx_diag.txt behind
// mcla_native_gfx_reclear_probe.
void NoteReclearArmed(const char* site, uint32_t width, uint32_t height);

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
  // The surface pitch and the sample count the GUEST asked for. Shape alone is
  // not an identity: measured on the pause menu, two different 1280x720/rt=28
  // surfaces shared one pooled target, so whatever the second drew landed in
  // the first's texture and never reached the screen. What separates them is
  // what the guest requested -- one pass asks for 1 sample, the other for 4 --
  // which `sample_count` cannot carry, because that field holds what the pool
  // ALLOCATES and both of those collapse to one.
  uint32_t guest_msaa = 0;
  uint32_t surface_pitch = 0;
  uint32_t sample_count = 1;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator<(const RenderTargetKey& o) const;
  bool operator==(const RenderTargetKey& o) const;
};

// One colour + depth pair plus its view heaps.
struct RenderTarget {
  Microsoft::WRL::ComPtr<ID3D12Resource> color;
  // Second colour target, attached on demand by EnsureSecondTarget when a draw
  // writes oC1. Its RTV is slot 1 of rtv_heap, so the two are contiguous and
  // OMSetRenderTargets takes them as one range. NOT part of the pool key: on
  // Xenos both targets of a pass come out of the same EDRAM allocation, so a
  // pass that mixes MRT and single-target draws -- an impostor tile whose trunk
  // and leaves use different shaders -- has to land on ONE surface set, or the
  // resolve takes whichever half the last draw happened to use.
  Microsoft::WRL::ComPtr<ID3D12Resource> color1;
  uint32_t rt1_format = 0;  // DXGI format of color1, 0 while unattached
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap;
  // Slot 0 of dsv_heap writes depth; slot 1 is the read-only view.
  uint32_t dsv_descriptor_size = 0;
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
  D3D12_RESOURCE_STATES color1_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  uint32_t rtv_descriptor_size = 0;
  // A pass the guest only CLEARED and then resolved, with no draws. Nothing
  // calls PrepareForRendering for such a pass, so the colour surface would be
  // copied out undefined. NoteResolve hands it to the copy it queues, and
  // FlushPendingCopies performs this clear itself right before that copy. See
  // RequestClearOnlyFill.
  bool pending_guest_clear = false;
  float pending_clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool cleared = false;  // cleared once, then accumulated into
  // The colour surface has been cleared at least once, so it holds something
  // defined -- a fresh resource holds whatever the driver left. Only then may a
  // frame keep its colour (mcla_native_gfx_keep_idle_display).
  bool color_defined = false;
  // A depth resolve that covers the WHOLE source ends that surface's pass: on
  // Xenos the depth lives in EDRAM, which the next pass starts from scratch.
  // Without this the shadow map's 640x640 surface, reused by four consecutive
  // cascades and resolved into four atlas quadrants, kept cascade 0's depth
  // for all four -- cascade 2's 191 draws were rejected outright and its
  // quadrant came out BIT-IDENTICAL to cascade 1's, which is what left the sun
  // valid only inside a straight-edged box around the player.
  //
  // Scoped to depth and to a full-source resolve deliberately. Measured over
  // four frames: the only full-source depth resolves in the game are the four
  // shadow cascades; the main scene's depth resolves are banded tiles
  // (1280x512 + 1280x208) and must keep accumulating. Colour is untouched --
  // 144 of its resolves are full-source and re-clearing those would be a
  // different change with a different blast radius.
  bool needs_depth_reclear = false;
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
    // A resolve that found its shape among the retired variants of its address
    // (resolved_spares_) instead of allocating, and variants dropped for room.
    uint64_t resolve_variant_reuses = 0;
    uint64_t resolve_variants_evicted = 0;
    // Resolve destinations the CPU wrote after their resolve, and the lookups
    // answered from guest memory because of it instead of the GPU copy.
    uint64_t cpu_overwrites = 0;
    uint64_t cpu_overwrite_skips = 0;
  };

  void Shutdown(D3D12Context& context);

  // A resolve destination the guest CPU writes afterwards holds the CPU's data
  // from then on -- on the console memory is shared, so the next fetch reads
  // what the CPU left there. MCLA does this on purpose: the photo preview
  // texture lives at 0x06ACD000, the memory the motion blur's depth resolve
  // uses every frame, and the bridge kept handing the UI that depth copy.
  // These register the SDK's physical write watch for the destinations, so
  // such a write turns the GPU copy off until the next resolve there.
  bool StartWatchingCpuWrites();
  void StopWatchingCpuWrites();

  // Returns the target for this configuration, creating it on first use.
  // Never null unless creation fails.
  RenderTarget* Acquire(D3D12Context& context, const RenderTargetKey& key,
                        float clear_depth);

  // Looks up an EXISTING target without creating one. A resolve must never
  // create: the render state at resolve time is not a request to allocate, and
  // trusting it as one allocates a target per bogus viewport until the GPU
  // runs out of memory (observed: every later resource creation failing).
  RenderTarget* Find(const RenderTargetKey& key);

  // Same shape, ANY colour format. A resolve builds its key from the viewport
  // registers plus rs.color_format, and MCLA has two 320x180 post-process passes
  // -- one HDR (rt_format 10), one LDR (28) -- so the LDR key misses a pool that
  // only ever rendered the HDR one, every frame. Measured: `RESOLVE_MISS
  // dest=0x02DE6000 320x180 rt_fmt=28 count=600` while `RESOLVE_DEST
  // dest=0x02D6E000 320x180 src_fmt=10` succeeds, and the tonemap then samples a
  // BLACK 320x180 (mean 0.0004) where the emulated path has real data.
  //
  // Deliberately NOT part of Find(): it is a fallback for a resolve that already
  // missed, never a lookup a draw can take.
  RenderTarget* FindByShape(uint32_t width, uint32_t height, uint32_t ds_format,
                            uint32_t sample_count);

  // TEMP DIAG: every pooled target of one colour format, written to the diag
  // file. A resolve that misses says nothing on its own -- the question is
  // whether the image it wanted is sitting in the pool under another shape.
  void LogTargetsForFormat(uint32_t rt_format, const char* why);
  // TEMP DIAG (mcla_native_gfx_vinyl_diag): the same list, as a string for the
  // main log -- LogTargetsForFormat stops after 40 lines per process.
  std::string DescribeTargetsForFormat(uint32_t rt_format) const;

  // Marks every pooled target as un-cleared so the next frame re-clears it on
  // its first draw. Continuous mode re-renders the whole scene each frame; the
  // clear was a one-shot latch (`cleared` set once, then accumulated), which is
  // right for a single bounded capture but leaves moving objects trailing in
  // continuous mode -- the previous frame's geometry is never erased. Called at
  // each continuous frame boundary.
  void MarkAllUncleared();

  // Puts a target's depth back into DEPTH_WRITE before it is bound as a DSV
  // again, undoing the transition RecordResolve made to let it be sampled.
  void PrepareForRendering(ID3D12GraphicsCommandList* cl, RenderTarget& target,
                           bool depth_read_only = false);

  // Attaches (or re-creates) the pass's second colour target. Cheap and
  // idempotent once the format matches; returns false only if creation failed,
  // in which case the caller must bind one target rather than two.
  bool EnsureSecondTarget(D3D12Context& context, RenderTarget& target, uint32_t dxgi_format);

  // Registers a DEPTH resolve without touching a command list. A resolve must
  // not submit work: the guest performs hundreds per frame, and opening and
  // submitting a frame for each one (with its fence wait, on the queue shared
  // with the Xenia command processor) hangs the GPU — observed as
  // DXGI_ERROR_DEVICE_HUNG 0x887A0001. The state transition the depth
  // resource needs is queued instead and issued by FlushPendingTransitions on
  // the next draw's command list.
  // `color_index` is RB_COPY_CONTROL.copy_src_select: which of the guest's
  // colour targets this resolve reads. Ignoring it is what handed target 0's
  // pixels to target 1's destination address.
  void NoteResolve(RenderTarget& source, bool from_depth, uint32_t dest_address,
                   uint32_t dest_width, uint32_t dest_height, const ResolveRegion& region,
                   uint32_t color_index = 0);

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

  // A colour resolve brought back to the CPU, rows tightly packed (width * 4
  // bytes each), in the channel order of `dxgi_format`.
  struct CpuReadback {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgi_format = 0;
    // The copy was a retired variant rather than the newest resolve at the
    // address: the guest resolved something of another shape there since.
    bool from_variant = false;
  };

  // Reads the 8:8:8:8 colour resolve registered at `dest_address` with exactly
  // this extent back to the CPU. For a guest that locks a resolve destination
  // and reads it with the CPU, which on the console sees the pixels the resolve
  // wrote to main memory; here they only ever exist in the copy. Queued resolve
  // copies are flushed first, since the lock can come before the next draw that
  // would issue them. Opens its own command list, so the caller must have
  // closed any open batch, and waits for the GPU: for rare reads only. Returns
  // false when there is no usable copy.
  bool ReadbackResolvedColor(D3D12Context& context, uint32_t dest_address, uint32_t width,
                             uint32_t height, CpuReadback* out);

  // True when an 8:8:8:8 colour copy of exactly this extent exists at the
  // address and guest memory does not hold it yet -- i.e. a CPU lock there
  // needs ReadbackResolvedColor. False when the CPU wrote the memory after the
  // resolve: then guest memory is the newer image. No GPU work.
  bool NeedsGuestWriteBack(uint32_t dest_address, uint32_t width, uint32_t height);

  // RenderTargetLookup.
  ID3D12Resource* FindResolvedTarget(uint32_t guest_address, uint32_t width, uint32_t height,
                                     bool want_depth,
                                     D3D12_RESOURCE_STATES* out_state) override;

  // The depth resolve at `guest_address` seen as an ordinary 8888 texture, or
  // null when there is no such entry. Separate from FindResolvedTarget because
  // the two differ in what the CALLER asked for, not in what is stored: a
  // k_8_8_8_8 fetch over a depth resolve is a deliberate guest idiom and has
  // to be answered with the repacked copy, while FindResolvedTarget's
  // `want_depth` is about depth-formatted fetches.
  ID3D12Resource* FindResolvedDepthAs8888(uint32_t guest_address, uint32_t width,
                                          uint32_t height,
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
  // produce the contents yet. `bytes` is the guest memory the destination
  // covers (tile-padded); it is what the CPU-write watch protects. A resolve
  // ends any CPU overwrite: the GPU copy is the newer image again.
  void NoteDestination(uint32_t dest_address, uint32_t width, uint32_t height,
                       uint32_t bytes);

  // Arms the clear a CLEAR-ONLY pass needs: the guest cleared this surface and
  // resolved it without issuing a single draw, so the pool's usual policy clear
  // (which runs on a pass's first draw) never happens. FlushPendingCopies does
  // it just before reading the surface.
  void RequestClearOnlyFill(RenderTarget& target, const float rgba[4]);

  void RegisterDirect(uint32_t dest_address, ID3D12Resource* resource, uint32_t width,
                      uint32_t height, uint32_t shader_format, D3D12_RESOURCE_STATES state);

  // EXPERIMENT: a colour resolve the pool has no source for is currently
  // dropped. The emulated path cannot drop one -- it resolves out of EDRAM by
  // address, so it always produces something. This aliases a missed colour
  // resolve onto the most recent successful colour copy, purely to find out
  // whether anything actually SAMPLES that destination. Returns true if an
  // alias was registered.
  bool AliasMissedColourResolve(uint32_t dest_address, uint32_t width, uint32_t height);

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
    uint64_t target_bytes = 0;  // actual colour/depth/MRT/MSAA scratch allocations
    size_t resolved = 0;
    uint64_t resolved_bytes = 0;
    size_t orphaned = 0;
    uint64_t orphaned_bytes = 0;
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
    // A depth-stencil entry also carries a PACKED copy: one R32G32_FLOAT
    // texture with depth in R and stencil/256 in G. A guest k_24_8 fetch is
    // handed this instead of the raw two-plane surface, since one D3D12 view
    // cannot expose both planes. See shaders/depth_stencil_pack_cs.hlsl.
    Microsoft::WRL::ComPtr<ID3D12Resource> packed;
    D3D12_RESOURCE_STATES packed_state = D3D12_RESOURCE_STATE_COMMON;
    // And a SECOND packed copy, R8G8B8A8_UNORM, for the other guest format the
    // same depth resolve is read through. MCLA resolves the depth buffer to a
    // separate address and samples THAT as a plain k_8_8_8_8 colour texture to
    // recover the stencil byte -- measured in blur.rdc as addr 0x06ACD000,
    // 1280x720, format 6, swizzle 0x60A, endian 2, produced by a resolve whose
    // source index is 4 (depth). Its components reproduce the post-endian
    // fetch: (stencil, D[7:0], D[15:8], D[23:16]).
    Microsoft::WRL::ComPtr<ID3D12Resource> packed8888;
    D3D12_RESOURCE_STATES packed8888_state = D3D12_RESOURCE_STATE_COMMON;
    // Guest memory already holds this copy's pixels: ReadbackResolvedColor
    // wrote it back and no resolve has landed here since. Lets a second lock
    // skip the GPU wait, and keeps a stale copy from overwriting what the CPU
    // wrote after it took the first one. Every NoteResolve clears it.
    bool guest_current = false;
  };

  // Builds (or refreshes) the packed depth+stencil copy of `dst`. No-op when
  // the entry is not a two-plane depth-stencil.
  void PackResolvedDepthStencil(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                                ResolvedCopy& dst);

  // The 8:8:8:8 colour copy of exactly this extent at the address: the newest
  // resolve there, else its retired variant (the address is aliased). Null if
  // neither fits.
  ResolvedCopy* FindCpuReadableCopy(uint32_t dest_address, uint32_t width, uint32_t height,
                                    bool* from_variant);


  // A pass renders into one target, but the guest cycles through many
  // transient configurations; pooling every one of them exhausted GPU memory
  // and every later resource creation then failed (texture, PSO, buffer).
  // Preserve the count and estimated-footprint admission guards below. The
  // estimate is not physical VRAM: stats_ and TakeCensus account for MSAA and
  // allocation alignment separately. Enforcing that larger physical footprint
  // here would drop previously accepted passes without a safe target eviction
  // policy (resolves and pending copies retain pointers into targets_).
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
  static constexpr uint64_t kMaxEstimatedTargetBytes = 512ull * 1024 * 1024;
  uint64_t estimated_target_bytes_ = 0;

  std::map<RenderTargetKey, RenderTarget> targets_;
  // Address -> the extent that was resolved there. The address alone is not
  // enough: the guest reuses memory, so an ordinary texture allocated where a
  // resolve destination used to live would be taken for GPU-produced data and
  // handed the neutral fallback -- an object that simply loses its texture.
  struct ProducedExtent {
    uint32_t width = 0;
    uint32_t height = 0;
    // Guest bytes the resolve covers (tile-padded), the range the CPU-write
    // watch protects. 0 until a resolve has said how big it is.
    uint32_t bytes = 0;
    // The write watch is armed over `bytes` and no write has hit it since.
    bool watched = false;
    // The CPU wrote here after the last resolve, so guest memory -- not the
    // GPU copy -- holds the current image until the guest resolves here again.
    bool cpu_dirty = false;
  };
  std::map<uint32_t, ProducedExtent> gpu_produced_;

  // CPU writes to resolve destinations, reported by the SDK's physical write
  // watch on whatever guest thread wrote, and applied on the render thread.
  static std::pair<uint32_t, uint32_t> CpuWriteThunk(void* context_ptr,
                                                     uint32_t physical_address_start,
                                                     uint32_t length, bool exact_range);
  // Marks every destination a queued write overlaps. Render thread only.
  void ApplyCpuWrites();
  bool CpuOverwrote(uint32_t guest_address) const;
  void* cpu_watch_handle_ = nullptr;
  std::mutex cpu_write_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> cpu_writes_;
  bool cpu_write_overflow_ = false;  // under cpu_write_mutex_
  std::atomic<bool> cpu_write_pending_{false};
  std::map<uint32_t, ResolvedCopy> resolved_;  // keyed by guest destination address
  std::vector<RenderTarget*> pending_to_shader_;
  // Depth copies still to be issued; a resolve must not submit work.
  struct PendingCopy {
    RenderTarget* source = nullptr;
    uint32_t dest_address = 0;
    ResolveRegion region;
    bool from_depth = false;
    uint32_t color_index = 0;
    // The clear-only fill, carried by the copy it belongs to rather than by the
    // target: two clear-only passes of one shape share one pooled target, and
    // with the colour kept on the target the second request overwrote the
    // first before the flush, so both copies took the second colour.
    bool fill = false;
    float fill_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  };
  std::vector<PendingCopy> pending_copies_;
  // Resolve-destination resources retired when the guest reuses a destination
  // address with a new format/size. NoteResolve has no D3D12Context so it cannot
  // defer-release directly; it parks the old resource here and FlushPendingCopies
  // (which has the context) drains it through the fence-gated DeferRelease. This
  // replaces the previous Detach()-and-leak, which grew VRAM unbounded.
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> orphaned_resources_;

  // Owned resolve copies retired by a resolve of a different shape at the same
  // address, kept for reuse. The guest alternates shapes at one destination,
  // and destroying the loser every time recreated a 1280x720 copy and its
  // packed depth+stencil twin (7 MiB each) about 25 times a second -- ~8% of the
  // render thread inside the driver's allocate/free calls. Lookups still see
  // only resolved_, the most recent resolve at each address, exactly as before;
  // this only keeps the resources instead of freeing and recreating them.
  // mcla_native_gfx_resolve_variants turns it off.
  struct ResolvedSpare {
    uint32_t address = 0;
    ResolvedCopy copy;
  };
  static constexpr size_t kMaxResolvedSpares = 16;
  std::vector<ResolvedSpare> resolved_spares_;
  // Retires the entry leaving resolved_: kept when owned and the reuse is on,
  // otherwise parked for the fence-gated release as before.
  void StashResolvedVariant(uint32_t address, ResolvedCopy&& copy);
  // Moves a retired variant of exactly this shape back out; false if none.
  bool TakeResolvedVariant(uint32_t address, uint32_t width, uint32_t height,
                           uint32_t dxgi_format, bool from_depth, ResolvedCopy* out);

  // Single-sampled stand-ins for a multisampled target, so a sub-rect resolve
  // still has something CopyTextureRegion can read: D3D12 refuses a copy whose
  // source is multisampled. Created on demand, one per (target, kind), and
  // kept for the life of the pool -- the scene target is the only surface that
  // is ever multisampled (mcla_native_gfx_msaa) and it is long-lived.
  struct MsaaScratch {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgi_format = 0;
  };
  std::map<std::pair<const RenderTarget*, bool>, MsaaScratch> msaa_scratch_;

  // Resolves the whole multisampled surface into its scratch and returns it,
  // or nullptr when that cannot be done. Colour goes through
  // ResolveSubresource; depth needs ResolveSubresourceRegion, which lives on
  // ID3D12GraphicsCommandList1.
  ID3D12Resource* ResolveMsaaToScratch(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                                       RenderTarget& source, bool from_depth,
                                       D3D12_RESOURCE_STATES final_state);

  Stats stats_;
};

}  // namespace mcla::native_gfx
