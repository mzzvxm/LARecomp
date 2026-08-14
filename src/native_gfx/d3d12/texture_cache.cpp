#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture cache.
// See texture_cache.h for the design and the render-target bridge rationale.

#include "texture_cache.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "../guest/guest_resources.h"
#include "context.h"

REXCVAR_DECLARE(uint32_t, mcla_native_gfx_texcache_mb);

namespace mcla::native_gfx {

namespace {

// D3D12 requires each texture upload row to be 256-byte aligned.
constexpr uint32_t kUploadRowAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

inline uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint64_t Hash64(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

}  // namespace

uint64_t TextureCache::MakeKey(const TextureFetch& f) {
  struct {
    uint32_t addr, width, height, format, pitch, endian, swizzle, tiled;
  } k = {f.base_address, f.width,    f.height,  f.format,
         f.pitch,        f.endianness, f.swizzle, f.tiled ? 1u : 0u};
  return Hash64(&k, sizeof(k));
}

void TextureCache::Shutdown(D3D12Context& context) {
  StopWatchingGuestWrites();
  for (auto& [key, e] : entries_) {
    if (e.resource) {
      context.DeferRelease(e.resource.Detach());
    }
  }
  entries_.clear();
  stats_.live_bytes = 0;
}

std::pair<uint32_t, uint32_t> TextureCache::InvalidationThunk(void* context_ptr,
                                                              uint32_t physical_address_start,
                                                              uint32_t length, bool exact_range) {
  (void)exact_range;
  auto* self = static_cast<TextureCache*>(context_ptr);
  if (self && length) {
    std::lock_guard<std::mutex> lock(self->invalidation_mutex_);
    self->pending_invalidations_.emplace_back(physical_address_start, length);
  }
  return std::make_pair(physical_address_start, length);
}

bool TextureCache::StartWatchingGuestWrites() {
  if (invalidation_handle_) {
    return true;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("[native_gfx] texture write watch: no memory system");
    return false;
  }
  invalidation_handle_ = memory->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  if (!invalidation_handle_) {
    REXLOG_ERROR("[native_gfx] texture write watch registration failed");
    return false;
  }
  REXLOG_INFO("[native_gfx] texture write watch registered");
  return true;
}

void TextureCache::StopWatchingGuestWrites() {
  if (!invalidation_handle_) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  if (auto* memory = runtime ? runtime->memory() : nullptr) {
    memory->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
  }
  invalidation_handle_ = nullptr;
}

void TextureCache::WatchEntry(const Entry& entry) {
  if (!invalidation_handle_ || !entry.guest_size) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  if (auto* memory = runtime ? runtime->memory() : nullptr) {
    memory->EnablePhysicalMemoryAccessCallbacks(entry.guest_base & 0x1FFFFFFFu,
                                                uint32_t(entry.guest_size),
                                                /*enable_invalidation_notifications=*/true,
                                                /*enable_data_providers=*/false);
  }
}

void TextureCache::ApplyPendingInvalidations(D3D12Context& context) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    if (pending_invalidations_.empty()) {
      return;
    }
    ranges.swap(pending_invalidations_);
  }
  // Dropping the entry rather than flagging it is what re-decodes the texture:
  // the next Resolve misses and rebuilds it from whatever the guest has now.
  // The release is fence-gated, so a command list still referencing the old
  // resource stays valid, and TextureBinder rebuilds its SRVs every frame
  // (the descriptor heap halves flip), so no stale pointer-keyed view survives.
  for (const auto& [start, length] : ranges) {
    const uint64_t lo = start & 0x1FFFFFFFu;
    const uint64_t hi = lo + length;
    for (auto it = entries_.begin(); it != entries_.end();) {
      Entry& e = it->second;
      const uint64_t e_lo = e.guest_base & 0x1FFFFFFFu;
      const uint64_t e_hi = e_lo + e.guest_size;
      if (e.guest_size && e_lo < hi && lo < e_hi) {
        if (e.resource) {
          context.DeferRelease(e.resource.Detach());
        }
        stats_.live_bytes -= (e.bytes <= stats_.live_bytes) ? e.bytes : stats_.live_bytes;
        ++stats_.invalidated;
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void TextureCache::EvictToBudget(D3D12Context& context, uint64_t current_frame) {
  const uint64_t budget_mb = REXCVAR_GET(mcla_native_gfx_texcache_mb);
  if (budget_mb == 0) {
    return;  // explicitly unbounded
  }
  const uint64_t budget = budget_mb * 1024ull * 1024ull;
  if (stats_.live_bytes <= budget) {
    return;
  }
  // Oldest first, skipping anything this frame has already bound.
  std::vector<std::pair<uint64_t, uint64_t>> victims;  // (last_use_frame, key)
  victims.reserve(entries_.size());
  for (const auto& [key, e] : entries_) {
    if (e.last_use_frame != current_frame) {
      victims.emplace_back(e.last_use_frame, key);
    }
  }
  std::sort(victims.begin(), victims.end());
  for (const auto& victim : victims) {
    if (stats_.live_bytes <= budget) {
      break;
    }
    auto it = entries_.find(victim.second);
    if (it == entries_.end()) {
      continue;
    }
    if (it->second.resource) {
      context.DeferRelease(it->second.resource.Detach());
    }
    stats_.live_bytes -= std::min(stats_.live_bytes, it->second.bytes);
    stats_.evicted_bytes += it->second.bytes;
    ++stats_.evictions;
    entries_.erase(it);
  }
}

ID3D12Resource* TextureCache::Resolve(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                                      const uint8_t* base, const TextureFetch& fetch,
                                      TextureSource* out_source) {
  // Default to "nothing usable came back". Only the two paths that actually
  // return a resource overwrite this, so every failure return stays honest
  // without having to be touched individually.
  if (out_source) {
    *out_source = TextureSource::kUnresolved;
  }
  // TEMP DIAG (remove after): trace the exposure fetch at 0x02D6C000.
  if (fetch.base_address == 0x02D6C000u) {
    static unsigned n = 0;
    ++n;
    // First few AND a periodic sample. Capping at six showed only frame 1,
    // where the fetch necessarily precedes the frame's first resolve -- the
    // steady state, which is what decides whether the bridge works, was never
    // in the log.
    if (n <= 6 || (n % 600) == 0) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        // Read the guest DWORD at the exposure address both raw and byteswapped,
        // interpreted as float. If Xenia's exempt exposure pass wrote it, one of
        // these is a plausible exposure scalar (not 0, not NaN).
        uint32_t raw_le = 0;
        // Texture fetch addresses are physical, while `base` is the virtual
        // guest membase. Do not probe them directly: an uncommitted physical
        // page is a host access violation, and this diagnostic runs before the
        // normal range check below. Use the same checked physical translation
        // as the real upload path instead.
        const uint8_t* physical = nullptr;
        if (IsPhysicalRangeReadable(fetch.base_address, sizeof(raw_le))) {
          physical = TranslatePhysicalGuest(fetch.base_address);
        }
        if (physical) {
          std::memcpy(&raw_le, physical, sizeof(raw_le));
        }
        const uint32_t raw_be = __builtin_bswap32(raw_le);
        float f_le, f_be;
        std::memcpy(&f_le, &raw_le, 4);
        std::memcpy(&f_be, &raw_be, 4);
        std::fprintf(f,
                     "TEXRESOLVE 0x02D6C000 type_valid=%d %ux%u fmt=%u rtlookup=%d | "
                     "guest readable=%d raw=0x%08X f_le=%g f_be=%g\n",
                     fetch.type_valid ? 1 : 0, fetch.width, fetch.height, fetch.format,
                     rt_lookup_ ? 1 : 0, physical ? 1 : 0, raw_le, f_le, f_be);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  if (!fetch.type_valid || fetch.width == 0 || fetch.height == 0) {
    ++stats_.decode_failures;
    return nullptr;
  }

  // Render-target bridge first: never duplicate an image the GPU already
  // owns. Depth and float targets read back as textures land here.
  if (rt_lookup_) {
    D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_COMMON;
    // A depth-sourced fetch format wants the depth resolve; anything else wants
    // the colour resolve. Without this the ShadowCollector (colour) was served
    // the depth resolve that shares its address.
    const bool want_depth = IsRenderTargetSourcedFormat(fetch.format);
    if (ID3D12Resource* rt =
            rt_lookup_->FindResolvedTarget(fetch.base_address, fetch.width, fetch.height,
                                           want_depth, &rt_state)) {
      ++stats_.render_target_hits;
      if (out_source) {
        *out_source = TextureSource::kRenderTargetBridge;
      }
      return rt;
    }
  }
  if (rt_lookup_ &&
      rt_lookup_->IsStaleGpuAddress(fetch.base_address, fetch.width,
                                    fetch.height)) {
    ++stats_.stale_gpu_addresses;
  }
  if ((rt_lookup_ &&
       rt_lookup_->IsGpuProduced(fetch.base_address, fetch.width, fetch.height)) ||
      IsRenderTargetSourcedFormat(fetch.format)) {
    // Reached only when the bridge has no entry for this address. Falling
    // through to the normal path would decode guest memory the GPU never
    // wrote and count it as a success; failing here keeps the accounting
    // honest and lets the caller bind its neutral fallback instead.
    //
    // This guard must NOT be an `else` of the lookup above: once a lookup was
    // installed, an `else` stops running entirely and every missed depth
    // fetch silently starts decoding stale memory.
    //
    // There used to be an exemption here for a TINY float scalar (<=16x16
    // k_32_FLOAT), i.e. the HDR exposure / luminance-adaptation buffer, on the
    // grounds that in mixed mode the emulated (Xenia) path produced it and
    // wrote the value into guest memory. Both halves of that were measured
    // false: the native runtime DOES resolve the exposure address (the log
    // shows NOTE_RESOLVE dest=0x02D6C000 1x1 from_depth=0 src_fmt=41), and
    // guest memory there reads 0x00000000. So the exemption decoded a zero,
    // reported it as a successful bind, and multiplied the whole composite to
    // black -- the exact failure its own comment warned about. Letting the
    // exposure refuse here binds the neutral WHITE fallback (1.0) instead,
    // which is pass-through for a multiplicative input and the correct
    // degradation on the frames before the first resolve registers.
    ++stats_.bridge_refusals;
    return nullptr;
  }

  // TEMP DIAG (remove after): content probe for the composite's screen-sized
  // k_8_8_8_8 input. Its SOURCE is known (guest decode); what is unknown is
  // whether anything ever WROTE it. The producing pass has pitch 1280, which
  // native_render_suppress_mode=2 suppresses on the emulated side, and no native
  // resolve registers this address at 1280x720 -- so the expectation is bytes
  // nobody produced. Sampling four spread-out dwords distinguishes "uniform
  // never-written fill" from "real image".
  if (fetch.base_address == 0x06ACD000u && fetch.width >= 1024) {
    static unsigned n = 0;
    ++n;
    if (n <= 4 || (n % 600) == 0) {
      const uint32_t probe_size = fetch.width * fetch.height * 4u;
      uint32_t px[4] = {};
      bool readable = IsPhysicalRangeReadable(fetch.base_address, probe_size);
      if (readable) {
        if (const uint8_t* p = TranslatePhysicalGuest(fetch.base_address)) {
          const uint32_t offs[4] = {0u, probe_size / 3u, probe_size / 2u, probe_size - 4u};
          for (int i = 0; i < 4; ++i) {
            std::memcpy(&px[i], p + (offs[i] & ~3u), 4);
          }
        } else {
          readable = false;
        }
      }
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "S13PROBE 0x06ACD000 %ux%u f%u readable=%d px=%08X %08X %08X %08X\n",
                     fetch.width, fetch.height, fetch.format, readable ? 1 : 0, px[0], px[1],
                     px[2], px[3]);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }

  const FormatInfo fi = GetFormatInfo(fetch.format);
  const uint32_t dxgi = TextureFormatToDxgi(fetch.format);
  if (fi.bytes_per_block == 0 || dxgi == 0) {
    if (++stats_.unsupported_format <= 16) {
      REXLOG_ERROR("[native_gfx] unsupported texture format {} ({}x{}) at {:#010x}", fetch.format,
                   fetch.width, fetch.height, fetch.base_address);
    }
    return nullptr;
  }

  // Ranges the guest rewrote since the last draw. Applying them before the
  // lookup is what lets a texture that streamed in after its first use be
  // decoded again instead of being served half-finished forever.
  ApplyPendingInvalidations(context);

  const uint64_t key = MakeKey(fetch);
  auto it = entries_.find(key);
  if (it != entries_.end() && it->second.resource) {
    it->second.last_use_frame = context.frame_index();
    ++stats_.hits;
    if (out_source) {
      *out_source = TextureSource::kGuestDecode;
    }
    return it->second.resource.Get();
  }

  const uint32_t width_blocks = (fetch.width + fi.block_width - 1) / fi.block_width;
  const uint32_t height_blocks = (fetch.height + fi.block_height - 1) / fi.block_height;
  const uint32_t src_pitch = width_blocks * fi.bytes_per_block;

  // The whole source extent must be committed guest memory. Being inside the
  // 4 GiB reservation is not enough — the reservation is sparse, and a stale
  // fetch constant pointing at an uncommitted page faults the process.
  const uint64_t src_size =
      fetch.tiled
          ? TiledSurfaceSizeBytes(width_blocks, height_blocks, fi.bytes_per_block)
          : uint64_t(height_blocks) * (fetch.pitch
                                           ? (fetch.pitch / fi.block_width) * fi.bytes_per_block
                                           : src_pitch);
  if (!IsPhysicalRangeReadable(fetch.base_address, src_size)) {
    ++stats_.decode_failures;
    return nullptr;
  }
  const uint32_t upload_pitch = Align(src_pitch, kUploadRowAlignment);
  const uint64_t upload_size = uint64_t(upload_pitch) * height_blocks;

  Entry& e = entries_[key];
  e.fetch = fetch;
  e.last_use_frame = context.frame_index();
  if (!e.resource) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = fetch.width;
    desc.Height = fetch.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT(dxgi);
    desc.SampleDesc.Count = 1;
    const HRESULT hr = context.device()->CreateCommittedResource(
        &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&e.resource));
    if (FAILED(hr)) {
      context.ReportCreateFailure("texture", hr);
      REXLOG_ERROR("[native_gfx] texture creation failed ({}x{} fmt {})", fetch.width,
                   fetch.height, fetch.format);
      entries_.erase(key);
      ++stats_.decode_failures;
      return nullptr;
    }
    e.state = D3D12_RESOURCE_STATE_COPY_DEST;
    // Charge the entry against the budget with what the resource actually
    // occupies (mip/alignment padding included), not with the guest extent.
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        context.device()->GetResourceAllocationInfo(0, 1, &desc);
    e.bytes = info.SizeInBytes;
    stats_.live_bytes += e.bytes;
  }

  D3D12Context::UploadAlloc staging;
  if (!context.AllocateUpload(upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, staging)) {
    ++stats_.decode_failures;
    return nullptr;
  }

  // Texture fetch constants carry PHYSICAL addresses (the Xenos reads memory
  // directly); they must go through the physical membase.
  (void)base;
  const uint8_t* src = TranslatePhysicalGuest(fetch.base_address);
  if (!src) {
    ++stats_.decode_failures;
    return nullptr;
  }
  auto* dst = static_cast<uint8_t*>(staging.cpu);
  if (fetch.tiled) {
    // Tiled surfaces are padded to 32x32-block macro tiles; pass the real
    // readable extent so a malformed fetch constant cannot read past it.
    if (!UntileSurface2D(dst, upload_pitch, src,
                         TiledSurfaceSizeBytes(width_blocks, height_blocks, fi.bytes_per_block),
                         width_blocks, height_blocks, fi.bytes_per_block)) {
      ++stats_.decode_failures;
    }
  } else {
    // Linear: the guest pitch is in texels and may exceed the used width.
    const uint32_t guest_pitch =
        fetch.pitch ? (fetch.pitch / fi.block_width) * fi.bytes_per_block : src_pitch;
    for (uint32_t y = 0; y < height_blocks; ++y) {
      std::memcpy(dst + size_t(y) * upload_pitch, src + size_t(y) * guest_pitch, src_pitch);
    }
  }

  // Undo the guest's big-endian storage. Applied after untiling because
  // untiling moves whole blocks, so every block boundary is still aligned to
  // the swap width. Skipping this leaves DXT colour endpoints byte-reversed,
  // which decodes as coloured speckle rather than as a missing texture.
  SwapTextureData(fetch.endianness, dst, upload_size);

  if (e.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Transition.pResource = e.resource.Get();
    barrier.Transition.StateBefore = e.state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);
  }

  D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
  dst_loc.pResource = e.resource.Get();
  dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_loc.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src_loc = {};
  src_loc.pResource = staging.buffer;
  src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src_loc.PlacedFootprint.Offset = staging.offset;
  src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT(dxgi);
  src_loc.PlacedFootprint.Footprint.Width = fetch.width;
  src_loc.PlacedFootprint.Footprint.Height = fetch.height;
  src_loc.PlacedFootprint.Footprint.Depth = 1;
  src_loc.PlacedFootprint.Footprint.RowPitch = upload_pitch;
  cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Transition.pResource = e.resource.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &barrier);
  e.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

  ++stats_.uploads;
  // Remember what was decoded and arm the write watch over it, so the guest
  // finishing a streamed texture drops this entry instead of leaving the
  // half-decoded upload resident for the rest of the session.
  e.guest_base = fetch.base_address;
  e.guest_size = src_size;
  WatchEntry(e);
  // Take the pointer before evicting: EvictToBudget erases entries, and while it
  // never touches one used this frame (this one), the reference `e` must not
  // outlive the call.
  ID3D12Resource* uploaded = e.resource.Get();
  if (out_source) {
    *out_source = TextureSource::kGuestDecode;
  }
  EvictToBudget(context, context.frame_index());
  return uploaded;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
