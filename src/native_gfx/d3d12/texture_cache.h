#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — texture cache
// ===========================================================================
// Resolves a Xenos texture fetch constant to an ID3D12Resource + SRV.
//
//   texture fetch
//        |
//        +-- is this address a live render target / resolve output?
//        |        yes -> return the native RT resource (no copy, no
//        |               duplicate of the same image in two places)
//        |
//        +-- no  -> guest memory -> untile (if tiled) -> upload -> SRV
//
// The render-target bridge is an interface here and an implementation in the
// render-target system, which lands next. It exists now because the
// telemetry showed the case is real and common: depth (24_8, 24_8_FLOAT)
// and single-channel float (32_FLOAT) fetches at screen resolutions
// (1280x720, 640x360) are shadow maps and post-process buffers being read
// back, not textures living in guest memory.
//
// Supported formats are exactly the ten the game was observed to bind (see
// guest/texture_format.h). An unsupported format is reported and the draw
// is failed rather than rendered with wrong data.
// ===========================================================================

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

#include "../guest/texture_format.h"

namespace mcla::native_gfx {

class D3D12Context;

// Implemented by the render-target system. Lets the texture cache hand back
// a resource the GPU already owns instead of re-reading guest memory.
class RenderTargetLookup {
 public:
  virtual ~RenderTargetLookup() = default;

  // Returns the native resource backing `guest_address` if that address is
  // the destination of a resolve (or a live render target), else nullptr.
  // `out_state` receives the resource state the caller must transition from.
  // `want_depth` is what the FETCH expects: a depth-sourced format wants the
  // depth resolve, a colour format the colour resolve. The guest reuses one
  // address for both (a depth resolve and a colour resolve of a different
  // size), so matching on address alone handed a colour fetch the depth
  // resource — the ShadowCollector (k_8_8_8_8) was reading depth bits, which
  // is why shading was flat.
  virtual ID3D12Resource* FindResolvedTarget(uint32_t guest_address, uint32_t width,
                                             uint32_t height, bool want_depth,
                                             D3D12_RESOURCE_STATES* out_state) = 0;

  // True when the guest resolved something to this address, i.e. the data is
  // produced by the GPU and does not exist in guest memory. Classifying by
  // FORMAT cannot see this: a shadow-collector buffer is a plain k_8_8_8_8
  // colour target, indistinguishable from an ordinary texture by its format,
  // and decoding it from memory yields whatever was there before — counted as
  // a successful texture while feeding garbage into the lighting term.
  virtual bool IsGpuProduced(uint32_t guest_address, uint32_t width,
                             uint32_t height) const = 0;
  virtual bool IsStaleGpuAddress(uint32_t guest_address, uint32_t width,
                                 uint32_t height) const = 0;
};

// Where the pixels a slot ends up sampling actually came from. `resolved`
// alone cannot answer the question that matters when an image looks wrong:
// a fetch that decodes stale guest memory reports success exactly like one
// served by the render-target bridge, and the shader has no way to tell a
// real buffer from a zero-filled one. The composite's exposure input is the
// case in point -- it decoded guest memory holding 0.0 and blacked out the
// whole pass while logging as a successful bind.
enum class TextureSource : uint32_t {
  kUnresolved = 0,        // nothing returned; the caller binds its fallback
  kRenderTargetBridge,    // the resource the GPU already owns (trustworthy)
  kGuestDecode,           // decoded from guest memory (only as good as memory)
  kFallback,              // the neutral 1x1 white substitute
};

// One-character tag for the diagnostics, so a 9-slot dump stays on one line.
inline char TextureSourceTag(TextureSource s) {
  switch (s) {
    case TextureSource::kRenderTargetBridge: return 'R';
    case TextureSource::kGuestDecode:        return 'G';
    case TextureSource::kFallback:           return 'F';
    default:                                 return 'U';
  }
}

class TextureCache {
 public:
  struct Stats {
    // Texturas de nivel unico que ganharam uma cadeia gerada no host, para a
    // anisotropia ter nivel que escolher. Ver mcla_native_gfx_gen_mips.
    uint64_t generated_chains = 0;
    uint64_t hits = 0;
    uint64_t uploads = 0;             // first-use decode+upload (expected)
    uint64_t render_target_hits = 0;
    // GPU-produced address the bridge had no entry for. Counted separately
    // because it USED to be folded into render_target_hits, which reported a
    // miss as a hit and hid exactly the failure being hunted.
    uint64_t bridge_refusals = 0;
    uint64_t stale_gpu_addresses = 0;  // TEMP INSTRUMENTATION  // served by the RT bridge
    uint64_t unsupported_format = 0;  // real errors
    uint64_t decode_failures = 0;
    uint64_t evictions = 0;      // entries dropped to stay inside the budget
    uint64_t evicted_bytes = 0;
    // Entries dropped because the guest rewrote the memory they decoded. A
    // texture streamed in over several frames shows up here; a zero here with
    // half-decoded textures on screen means the watch never armed.
    uint64_t invalidated = 0;
    uint64_t live_bytes = 0;     // sum of the resident entries' resource sizes
  };

  // The render-target bridge is optional during bring-up: without it, RT-
  // sourced fetches are counted and skipped rather than mis-decoded.
  void SetRenderTargetLookup(RenderTargetLookup* lookup) { rt_lookup_ = lookup; }

  void Shutdown(D3D12Context& context);

  // Resolves a fetch to a shader-visible resource. Records any upload on the
  // current frame's command list. Returns nullptr on failure.
  // `out_source` reports which path produced the resource, for diagnosis.
  ID3D12Resource* Resolve(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                          const uint8_t* base, const TextureFetch& fetch,
                          TextureSource* out_source = nullptr);

  const Stats& stats() const { return stats_; }

  // Resident entries, for the memory census.
  size_t entry_count() const { return entries_.size(); }

  // Starts watching guest writes, so a texture whose memory the game rewrites
  // is decoded again instead of being served from the cache forever.
  //
  // This is the same hole BufferCache had: an entry, once uploaded, was
  // returned for the rest of the session. Textures stream in, so a tile
  // sampled while it was still arriving froze half-decoded -- measured on the
  // map screen ("maps-bugs.rdc"), where tile 11256 is a 512x512 BC1 whose left
  // half is still the untouched upload buffer, and the tiles that were sampled
  // early stay black in a regular grid across the map.
  //
  // A failure only means textures keep the old never-invalidate behaviour.
  bool StartWatchingGuestWrites();
  void StopWatchingGuestWrites();

 private:
  struct Entry {
    // A cadeia deste recurso nao veio do guest: foi gerada por box filter no
    // host. O sampler precisa saber para poder soltar o MaxLOD de um base-map.
    bool host_generated_mips = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    TextureFetch fetch;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    // For the budget: what this entry costs and when it was last needed.
    uint64_t bytes = 0;
    uint64_t last_use_frame = 0;
    // Guest extent this entry decoded, so an invalidation can tell whether a
    // write landed inside it.
    uint32_t guest_base = 0;
    uint64_t guest_size = 0;
  };

  // Re-arms the write watch over an entry's guest range. Consumed when it
  // fires, so it is set again after every upload.
  void WatchEntry(const Entry& entry);
  // Drops entries the guest overwrote. Called from Resolve, which holds the
  // context needed for the fence-gated release.
  void ApplyPendingInvalidations(D3D12Context& context);

  // Runs on the guest thread that performed the write, inside the memory
  // system's global critical region: it may only record the range.
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context_ptr,
                                                         uint32_t physical_address_start,
                                                         uint32_t length, bool exact_range);

  // Drops least-recently-used entries until the cache fits the budget. Entries
  // touched during `current_frame` are never evicted: their SRVs are already in
  // this frame's descriptor heap and the draws referencing them are still being
  // recorded. Everything else is safe because the release is fence-gated
  // (DeferRelease), so the resource outlives any command list still using it.
  void EvictToBudget(D3D12Context& context, uint64_t current_frame);

  // Key: the fetch fields that change interpretation of the same memory —
  // address, size, format, tiling, pitch, endianness and swizzle. Anything
  // that only affects sampling (filters, LOD bias, clamp) belongs to the
  // sampler, not the resource, and is deliberately excluded.
  static uint64_t MakeKey(const TextureFetch& f);

  std::unordered_map<uint64_t, Entry> entries_;
  RenderTargetLookup* rt_lookup_ = nullptr;
  std::mutex invalidation_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> pending_invalidations_;
  void* invalidation_handle_ = nullptr;
  Stats stats_;
};

}  // namespace mcla::native_gfx
