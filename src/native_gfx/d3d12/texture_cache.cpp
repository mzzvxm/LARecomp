#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture cache.
// See texture_cache.h for the design and the render-target bridge rationale.

#include "texture_cache.h"

#include <algorithm>
#include <set>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "../guest/guest_resources.h"
#include "context.h"

REXCVAR_DECLARE(uint32_t, mcla_native_gfx_texcache_mb);
REXCVAR_DECLARE(bool, mcla_native_gfx_gen_mips);
REXCVAR_DECLARE(bool, mcla_native_gfx_verify_textures);

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

uint64_t TextureCache::HashGuestSampled(const uint8_t* p, uint64_t size) {
  // Eight 64-byte slices spread evenly over the surface. Hashing every byte was
  // measured on the buffer cache at 25 MB a frame and doubled the frame time;
  // sampling caught the same staleness for a fortieth of the cost. Mixes eight
  // bytes at a time, and folds the size in so a resize alone is a mismatch.
  constexpr uint64_t kSlice = 64;
  constexpr uint32_t kSlices = 8;
  uint64_t h = size * 1099511628211ull;
  for (uint32_t i = 0; i < kSlices; ++i) {
    const uint64_t off = size > kSlice ? (size - kSlice) * i / (kSlices - 1) : 0;
    const uint64_t len = size > kSlice ? kSlice : size;
    for (uint64_t b = 0; b + 8 <= len; b += 8) {
      uint64_t v;
      std::memcpy(&v, p + off + b, 8);
      h = (h ^ v) * 1099511628211ull;
      h ^= h >> 29;
    }
  }
  return h;
}

uint64_t TextureCache::MakeKey(const TextureFetch& f) {
  // The mip fields are part of the identity: the same base address can be
  // fetched with and without a chain, and serving the chainless entry for the
  // mipped fetch would silently drop the levels again.
  struct {
    uint32_t addr, width, height, format, pitch, endian, swizzle, tiled;
    uint32_t mip_addr, mip_max, packed;
  } k = {f.base_address, f.width,      f.height,  f.format,
         f.pitch,        f.endianness, f.swizzle, f.tiled ? 1u : 0u,
         f.mip_address,  f.mip_max_level, f.packed_mips ? 1u : 0u};
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

namespace {
// TEMP DIAG (remove after): a fetch that cannot be resolved falls back to the
// neutral 1x1 white, and the counter only says "decode_failures" -- which names
// six different sites. This says WHICH one, once per (address, reason), and it
// exists because the square minimap traced to exactly this: the circular mask
// in xAlphaModulate__PS_Textured samples fetch slot 0 and computes 1 - mask, so
// a mask that falls back to white subtracts nothing and the corners survive.
void NoteResolveFailure(const TextureFetch& fetch, const char* why) {
  static std::set<uint64_t> seen;
  const uint64_t sig = (uint64_t(fetch.base_address) << 8) ^ uint64_t(why[0]) ^
                       (uint64_t(fetch.format) << 1);
  if (!seen.insert(sig).second) {
    return;
  }
  if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
    std::fprintf(f, "TEXFAIL 0x%08X %ux%u fmt=%u tiled=%d pitch=%u -> %s\n", fetch.base_address,
                 fetch.width, fetch.height, fetch.format, fetch.tiled ? 1 : 0, fetch.pitch, why);
    std::fflush(f);
    std::fclose(f);
  }
}
}  // namespace

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
    // Backstop for a lost invalidation: re-check the guest bytes this entry was
    // decoded from, once per entry per frame, and drop the entry when they have
    // changed. Without it the minimap's circular mask stayed a stale, unrelated
    // texture for the whole session -- the punch sampled it, produced a
    // near-constant `1 - mask`, and the map kept its square corners.
    const bool verify = REXCVAR_GET(mcla_native_gfx_verify_textures) &&
                        it->second.content_hash != 0 &&
                        it->second.verified_frame != context.frame_index();
    if (verify) {
      it->second.verified_frame = context.frame_index();
      const uint8_t* g = TranslatePhysicalGuest(it->second.guest_base);
      if (g && IsPhysicalRangeReadable(it->second.guest_base, it->second.guest_size)) {
        ++stats_.verify_checks;
        if (HashGuestSampled(g, it->second.guest_size) != it->second.content_hash) {
          ++stats_.verify_catches;
          if (it->second.resource) {
            context.DeferRelease(it->second.resource.Detach());
          }
          stats_.live_bytes -= std::min(stats_.live_bytes, it->second.bytes);
          entries_.erase(it);
          it = entries_.end();
        }
      }
    }
    if (it != entries_.end()) {
      it->second.last_use_frame = context.frame_index();
      ++stats_.hits;
      if (out_source) {
        *out_source = TextureSource::kGuestDecode;
      }
      return it->second.resource.Get();
    }
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

  // ---- mip chain -------------------------------------------------------
  //
  // The Xenos keeps level 0 under base_address and levels 1..n under a SECOND
  // pointer, mip_address, with the distance to each level given by the guest
  // layout rules (power-of-two row pitches, 32-block tiles, 4 KB level
  // alignment). This cache used to create every texture with MipLevels = 1 and
  // upload only level 0, so a distant high-frequency surface -- a tower of
  // windows -- had nothing to filter down to and aliased into moire. Measured
  // on the attract skyline: 707 of 900 fetches carry a chain.
  //
  // The per-level byte offsets come from the SDK's own layout maths
  // (texture_util::GetGuestTextureLayout, pure arithmetic over the dimensions:
  // no registers, no packets, nothing from the command processor, and it is
  // already inside rexruntime, which this target already links).
  //
  // The packed mip tail is NOT unpacked here. Once min(width, height) falls to
  // 32 blocks the guest stops giving each level its own storage and crams the
  // rest into one shared block, addressed through GetPackedMipOffset. Rather
  // than declare levels this code cannot fill -- which would sample garbage --
  // the chain simply stops at the last unpacked level. For a 512x512 that is
  // 512 down to 32, which is where essentially all of the aliasing lives.
  const uint32_t guest_max_level =
      (fetch.mip_address != 0 && fetch.mip_max_level != 0) ? fetch.mip_max_level : 0u;
  const rex::graphics::texture_util::TextureGuestLayout guest_layout =
      rex::graphics::texture_util::GetGuestTextureLayout(
          rex::graphics::xenos::DataDimension::k2DOrStacked, fetch.pitch / 32u, fetch.width, fetch.height,
          /*depth_or_array_size=*/1u, fetch.tiled, rex::graphics::xenos::TextureFormat(fetch.format),
          fetch.packed_mips, /*has_base=*/true, guest_max_level);

  // Per-level source description, built once so the resource can be created
  // with exactly the number of levels that turn out to be readable. A level
  // whose guest memory is not committed ends the chain rather than failing the
  // texture: a shorter chain still filters, a missing subresource does not.
  struct MipPlan {
    uint32_t guest_address;
    uint32_t guest_pitch;   // bytes between block rows in guest memory
    uint64_t guest_size;    // bytes that must be readable
    uint32_t width;         // texels
    uint32_t height;
    uint32_t width_blocks;
    uint32_t height_blocks;
    uint32_t upload_pitch;
    uint64_t upload_size;
    // Packed mip tail: once min(width, height) reaches 16 texels the guest stops
    // giving each level its own storage and crams the rest into one shared tile.
    // These levels read from the packed level's surface at a block offset.
    bool packed = false;
    uint32_t src_extent_x_blocks = 0;
    uint32_t src_extent_y_blocks = 0;
    uint32_t off_x_blocks = 0;
    uint32_t off_y_blocks = 0;
  };
  MipPlan plans[16];
  uint32_t level_count = 0;
  {
    uint32_t last_level = 0;
    if (guest_max_level != 0) {
      // The packed tail used to be dropped here, which capped a 256x256 chain at
      // level 3 of 6. Anisotropic filtering then had nothing coarse to pick from
      // and measured as a no-op: 16x moved a road's mean gradient by 0.2%
      // (7.601 vs 7.586) where the emulated path sat at 8.387.
      last_level = std::min(guest_max_level, guest_layout.max_level);
    }
    last_level = std::min(last_level, uint32_t(std::size(plans)) - 1u);
    for (uint32_t level = 0; level <= last_level; ++level) {
      MipPlan p = {};
      p.width = std::max(1u, fetch.width >> level);
      p.height = std::max(1u, fetch.height >> level);
      p.width_blocks = (p.width + fi.block_width - 1) / fi.block_width;
      p.height_blocks = (p.height + fi.block_height - 1) / fi.block_height;
      const uint32_t packed_level = guest_layout.packed_level;
      p.packed = packed_level != UINT32_MAX && level >= packed_level;
      const uint32_t src_level = p.packed ? packed_level : level;
      const auto& lv = src_level == 0 ? guest_layout.base : guest_layout.mips[src_level];
      p.guest_address = src_level == 0
                            ? fetch.base_address
                            : fetch.mip_address + guest_layout.mip_offsets_bytes[src_level];
      p.src_extent_x_blocks = lv.x_extent_blocks ? lv.x_extent_blocks : p.width_blocks;
      p.src_extent_y_blocks = lv.y_extent_blocks ? lv.y_extent_blocks : p.height_blocks;
      if (p.packed) {
        // Where this level sits inside the shared tile. Same arithmetic the SDK
        // uses for its own packed copies (D3D12TextureCache, GetPackedMipOffset).
        uint32_t off_z = 0;
        rex::graphics::texture_util::GetPackedMipOffset(
            fetch.width, fetch.height, 1u, rex::graphics::xenos::TextureFormat(fetch.format), level,
            p.off_x_blocks, p.off_y_blocks, off_z);
      }
      p.guest_pitch = lv.row_pitch_bytes ? lv.row_pitch_bytes
                                         : p.src_extent_x_blocks * fi.bytes_per_block;
      p.guest_size = fetch.tiled ? TiledSurfaceSizeBytes(p.src_extent_x_blocks,
                                                         p.src_extent_y_blocks, fi.bytes_per_block)
                                 : uint64_t(p.src_extent_y_blocks) * p.guest_pitch;
      p.upload_pitch = Align(p.width_blocks * fi.bytes_per_block, kUploadRowAlignment);
      p.upload_size = uint64_t(p.upload_pitch) * p.height_blocks;
      if (p.guest_address == 0 || !IsPhysicalRangeReadable(p.guest_address, p.guest_size)) {
        break;
      }
      plans[level] = p;
      level_count = level + 1;
    }
  }
  if (level_count == 0) {
    ++stats_.decode_failures;
    NoteResolveFailure(fetch, "no usable mip level");
    return nullptr;
  }

  // ---- cadeia gerada no host --------------------------------------------
  //
  // Anisotropia so existe para escolher um nivel mais fino ao longo do eixo
  // maior; uma textura de nivel unico nao tem o que escolher, e o sampler sai
  // ANISOTROPIC 16x sem mudar pixel nenhum. Medido no passe de cena: de 16.1M
  // de binds >= 512x512, 12.7M sao kBaseMap e ZERO deles carrega cadeia --
  // o jogo marca base-map justamente porque a textura tem um nivel so.
  //
  // Desses, 70% (9.0M) sao formato 6 = k_8_8_8_8, sem compressao, entao gerar a
  // cadeia e um box filter de 2x2 na CPU: nada de decodificar e recomprimir BC.
  // Fica restrito a 32 bits por texel em bloco 1x1 exatamente por isso.
  //
  // E melhoria, nao paridade: o 360 tambem aliasava nessas superficies. Por isso
  // vive atras de cvar.
  uint32_t generated_levels = 0;
  const bool can_generate = REXCVAR_GET(mcla_native_gfx_gen_mips) && guest_max_level == 0 &&
                            level_count == 1 && fi.block_width == 1 && fi.block_height == 1 &&
                            fi.bytes_per_block == 4 && fetch.width >= 8 && fetch.height >= 8;
  if (can_generate) {
    uint32_t w = fetch.width, h = fetch.height;
    while (w > 1 || h > 1) {
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
      ++generated_levels;
    }
  }
  const uint32_t total_levels = level_count + generated_levels;

  // Yardstick for the chain above: how many fetches carry mip data at all, and
  // how many levels we ended up able to supply.
  {
    static uint32_t with_mips = 0;
    static uint32_t total = 0;
    static uint32_t levels_sum = 0;
    ++total;
    levels_sum += level_count;
    if (guest_max_level != 0) {
      ++with_mips;
    }
    if (total == 1000) {
      REXLOG_INFO("[native_gfx] texmip: {} of {} fetches carry a chain, {} levels uploaded",
                  with_mips, total, levels_sum);
    }
  }

  Entry& e = entries_[key];
  e.fetch = fetch;
  e.host_generated_mips = generated_levels != 0;
  e.last_use_frame = context.frame_index();
  if (!e.resource) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = fetch.width;
    desc.Height = fetch.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = UINT16(total_levels);
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

  // Texture fetch constants carry PHYSICAL addresses (the Xenos reads memory
  // directly); they must go through the physical membase.
  (void)base;

  if (e.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_RESOURCE_BARRIER to_copy = {};
    to_copy.Transition.pResource = e.resource.Get();
    to_copy.Transition.StateBefore = e.state;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &to_copy);
    e.state = D3D12_RESOURCE_STATE_COPY_DEST;
  }

  // Copia do nivel 0 ja decodificado (destilado, endianness resolvida), de onde
  // a cadeia gerada sai por reducao sucessiva. So e preenchida quando ha o que
  // gerar, para nao custar uma copia em toda textura.
  std::vector<uint8_t> base_level;
  uint32_t base_pitch = 0;

  for (uint32_t level = 0; level < level_count; ++level) {
    const MipPlan& p = plans[level];
    const uint8_t* src = TranslatePhysicalGuest(p.guest_address);
    if (!src) {
      ++stats_.decode_failures;
      NoteResolveFailure(fetch, "physical translation null");
      return nullptr;
    }
    D3D12Context::UploadAlloc staging;
    if (!context.AllocateUpload(p.upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, staging,
                                D3D12Context::UploadTag::kTexture)) {
      ++stats_.decode_failures;
      NoteResolveFailure(fetch, "upload ring allocation failed");
      return nullptr;
    }
    auto* dst = static_cast<uint8_t*>(staging.cpu);
    if (p.packed) {
      // The whole shared tile is decoded once, then this level's rectangle is
      // lifted out of it. Decoding straight into the staging buffer is not an
      // option: the levels are laid out side by side inside the tile, so the
      // source rows are wider than the destination.
      const uint32_t src_pitch = p.src_extent_x_blocks * fi.bytes_per_block;
      std::vector<uint8_t> tile(size_t(src_pitch) * p.src_extent_y_blocks);
      if (fetch.tiled) {
        if (!UntileSurface2D(tile.data(), src_pitch, src, p.guest_size, p.src_extent_x_blocks,
                             p.src_extent_y_blocks, fi.bytes_per_block)) {
          ++stats_.decode_failures;
        }
      } else {
        const uint32_t row_bytes = p.src_extent_x_blocks * fi.bytes_per_block;
        for (uint32_t y = 0; y < p.src_extent_y_blocks; ++y) {
          std::memcpy(tile.data() + size_t(y) * src_pitch, src + size_t(y) * p.guest_pitch,
                      row_bytes);
        }
      }
      const uint32_t row_bytes = p.width_blocks * fi.bytes_per_block;
      for (uint32_t y = 0; y < p.height_blocks; ++y) {
        const size_t sy = size_t(p.off_y_blocks) + y;
        if (sy >= p.src_extent_y_blocks) break;
        const size_t sx = size_t(p.off_x_blocks) * fi.bytes_per_block;
        if (sx + row_bytes > src_pitch) break;
        std::memcpy(dst + size_t(y) * p.upload_pitch, tile.data() + sy * src_pitch + sx, row_bytes);
      }
    } else if (fetch.tiled) {
      // Tiled surfaces are padded to 32x32-block macro tiles; pass the real
      // readable extent so a malformed fetch constant cannot read past it.
      if (!UntileSurface2D(dst, p.upload_pitch, src, p.guest_size, p.width_blocks,
                           p.height_blocks, fi.bytes_per_block)) {
        ++stats_.decode_failures;
      }
    } else {
      // Linear: the guest row pitch comes from the layout (256-aligned for the
      // base, power-of-two aligned for the mips) and may exceed the used width.
      const uint32_t row_bytes = p.width_blocks * fi.bytes_per_block;
      for (uint32_t y = 0; y < p.height_blocks; ++y) {
        std::memcpy(dst + size_t(y) * p.upload_pitch, src + size_t(y) * p.guest_pitch, row_bytes);
      }
    }

    // Undo the guest's big-endian storage. Applied after untiling because
    // untiling moves whole blocks, so every block boundary is still aligned to
    // the swap width. Skipping this leaves DXT colour endpoints byte-reversed,
    // which decodes as coloured speckle rather than as a missing texture.
    SwapTextureData(fetch.endianness, dst, p.upload_size);

    // Guarda o nivel 0 ja decodificado para a cadeia gerada abaixo. Tem de ser
    // aqui, depois do untile e do swap: e exatamente o que a GPU amostra.
    if (generated_levels && level == 0) {
      base_pitch = p.upload_pitch;
      base_level.assign(dst, dst + size_t(p.upload_pitch) * p.height);
    }


    // TEMP DIAG (remove after): the decoded pixels of the minimap circular
    // mask, the last link of that investigation never inspected. Three failure
    // modes all look identical from outside and are told apart here:
    //   all 0xFF        -> mask is solid white, so `1 - mask` is 0 and the
    //                      One/One/ReversedSubtract punch erases nothing;
    //   scrambled       -> a 32x32 surface is exactly one Xenos macro tile, so
    //                      untiling it as linear shuffles the circle;
    //   channel swapped -> big-endian storage decoded into the wrong byte.
    // The shader reads .x (RED) of this texture, so RED is what is printed.
    // TEMP DIAG (remove after): the REAL minimap mask is the 256x256 DXT1 at
    // 0x02D64000, not the 32x32 white placeholder that misled the earlier
    // rounds. Print color0 of every other 4x4 block as a 32x32 grid: the
    // staging buffer here is already untiled and endian-swapped, so this is
    // exactly what the GPU will sample. A circle means the decode is right and
    // the fault is elsewhere; noise means the untile or the endian swap is
    // wrong for DXT1.
    if (level == 0 && p.width == 256 && p.height == 256 && fi.bytes_per_block == 8) {
      // Keyed by ADDRESS, not a single latch: the first 256x256 DXT1 the game
      // decodes is not the mask (it caught 0x0A5C0000, a detail texture with no
      // circle in it). Dump the first few distinct ones so the mask can be
      // picked out by eye.
      // Only the mask. Dumping "the first 256x256 DXT1" caught 0x0A5C0000 and
      // five more world textures that load first; the mask is 0x02D64000, the
      // address the PUNCHTEX diag reports for the REV_SUBTRACT draw. Stable
      // across every run measured so far.
      static bool dumped_dxt = false;
      if (!dumped_dxt && fetch.base_address == 0x02D64000u) {
        dumped_dxt = true;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "MASK256 0x%08X fmt=%u tiled=%d endian=%u upload_pitch=%u\n",
                       fetch.base_address, fetch.format, fetch.tiled ? 1 : 0, fetch.endianness,
                       p.upload_pitch);
          for (uint32_t by = 0; by < 64; by += 2) {
            char row[40];
            for (uint32_t bx = 0; bx < 64; bx += 2) {
              uint16_t c0 = 0;
              std::memcpy(&c0, dst + size_t(by) * p.upload_pitch + size_t(bx) * 8, 2);
              const uint32_t lum = ((c0 >> 11) & 31u) * 8u;
              row[bx / 2] = lum < 32 ? '.' : (lum < 96 ? ':' : (lum < 160 ? '+' :
                                        (lum < 224 ? '*' : '#')));
            }
            row[32] = 0;
            std::fprintf(f, "MASK256R %s\n", row);
          }
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    // TEMP DIAG (GLOWTEX): a textura de glow que o passe SeedRTNoZ amostra para
    // semear o alvo de reflexo da agua. Pular esse passe derruba o reflexo de
    // p50 0.8562 para 0.0064, entao ele e quem enche o buffer de claro, e a
    // saida dele e glow * StreakParams.y * cor. Este e o glow. Staging ja esta
    // untiled e com endian trocado, entao e o que a GPU vai amostrar.
    if (level == 0 && p.width == 128 && p.height == 128 && fi.bytes_per_block == 8 &&
        fetch.base_address == 0x05EE4000u) {
      static bool glow_dumped = false;
      if (!glow_dumped) {
        glow_dumped = true;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "GLOWTEX 0x%08X fmt=%u tiled=%d endian=%u upload_pitch=%u\n",
                       fetch.base_address, fetch.format, fetch.tiled ? 1 : 0, fetch.endianness,
                       p.upload_pitch);
          double sum = 0.0;
          uint32_t n = 0, bright = 0;
          for (uint32_t by = 0; by < 32; ++by) {
            char row[40];
            for (uint32_t bx = 0; bx < 32; ++bx) {
              uint16_t c0 = 0;
              std::memcpy(&c0, dst + size_t(by) * p.upload_pitch + size_t(bx) * 8, 2);
              const uint32_t lum = ((c0 >> 11) & 31u) * 8u;
              sum += lum; ++n; if (lum > 200) ++bright;
              row[bx] = lum < 32 ? '.' : (lum < 96 ? ':' : (lum < 160 ? '+' :
                                    (lum < 224 ? '*' : '#')));
            }
            row[32] = 0;
            std::fprintf(f, "GLOWTEXR %s\n", row);
          }
          std::fprintf(f, "GLOWTEX media=%.1f brilhantes=%.1f%%\n", sum / (n ? n : 1),
                       100.0 * bright / (n ? n : 1));
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    if (level == 0 && p.width == 32 && p.height == 32 && fi.bytes_per_block == 4) {
      static bool dumped_mask = false;
      if (!dumped_mask) {
        dumped_mask = true;
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "MASK32 0x%08X fmt=%u tiled=%d endian=%u pitch=%u upload_pitch=%u\n",
                       fetch.base_address, fetch.format, fetch.tiled ? 1 : 0, fetch.endianness,
                       fetch.pitch, p.upload_pitch);
          for (uint32_t y = 0; y < 32; ++y) {
            char row[40];
            for (uint32_t x = 0; x < 32; ++x) {
              const uint8_t r = dst[size_t(y) * p.upload_pitch + size_t(x) * 4];
              row[x] = r < 32 ? '.' : (r < 96 ? ':' : (r < 160 ? '+' : (r < 224 ? '*' : '#')));
            }
            row[32] = 0;
            std::fprintf(f, "MASK32R %s\n", row);
          }
          std::fflush(f);
          std::fclose(f);
        }
      }
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = e.resource.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = level;
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = staging.buffer;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = staging.offset;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT(dxgi);
    src_loc.PlacedFootprint.Footprint.Width = p.width;
    src_loc.PlacedFootprint.Footprint.Height = p.height;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = p.upload_pitch;
    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
  }

  // Cadeia gerada: box filter 2x2 por nivel, em RGBA8. Cada nivel sai do
  // ANTERIOR (nao do nivel 0) para o filtro acumular como uma piramide de
  // verdade em vez de reamostrar o topo cada vez.
  if (generated_levels && !base_level.empty()) {
    std::vector<uint8_t> prev = std::move(base_level);
    uint32_t prev_w = fetch.width, prev_h = fetch.height, prev_pitch = base_pitch;
    for (uint32_t i = 0; i < generated_levels; ++i) {
      const uint32_t w = prev_w > 1 ? prev_w / 2 : 1;
      const uint32_t h = prev_h > 1 ? prev_h / 2 : 1;
      const uint32_t pitch =
          (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
      D3D12Context::UploadAlloc staging;
      if (!context.AllocateUpload(size_t(pitch) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                  staging, D3D12Context::UploadTag::kTexture)) {
        break;  // sem espaco no ring: a cadeia para aqui, os niveis ja subidos valem
      }
      auto* out = static_cast<uint8_t*>(staging.cpu);
      for (uint32_t y = 0; y < h; ++y) {
        const uint32_t y0 = y * 2u;
        const uint32_t y1 = (y0 + 1u < prev_h) ? y0 + 1u : y0;
        const uint8_t* r0 = prev.data() + size_t(y0) * prev_pitch;
        const uint8_t* r1 = prev.data() + size_t(y1) * prev_pitch;
        uint8_t* o = out + size_t(y) * pitch;
        for (uint32_t x = 0; x < w; ++x) {
          const uint32_t x0 = x * 2u;
          const uint32_t x1 = (x0 + 1u < prev_w) ? x0 + 1u : x0;
          for (uint32_t c = 0; c < 4u; ++c) {
            const uint32_t sum = uint32_t(r0[x0 * 4u + c]) + uint32_t(r0[x1 * 4u + c]) +
                                 uint32_t(r1[x0 * 4u + c]) + uint32_t(r1[x1 * 4u + c]);
            o[x * 4u + c] = uint8_t((sum + 2u) / 4u);
          }
        }
      }
      D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
      dst_loc.pResource = e.resource.Get();
      dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst_loc.SubresourceIndex = level_count + i;
      D3D12_TEXTURE_COPY_LOCATION src_loc = {};
      src_loc.pResource = staging.buffer;
      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src_loc.PlacedFootprint.Offset = staging.offset;
      src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT(dxgi);
      src_loc.PlacedFootprint.Footprint.Width = w;
      src_loc.PlacedFootprint.Footprint.Height = h;
      src_loc.PlacedFootprint.Footprint.Depth = 1;
      src_loc.PlacedFootprint.Footprint.RowPitch = pitch;
      cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
      prev.assign(out, out + size_t(pitch) * h);
      prev_w = w; prev_h = h; prev_pitch = pitch;
    }
    ++stats_.generated_chains;
  }

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
  // Sampled hash of the bytes this decode read, so a later Resolve can tell
  // whether guest memory has moved on. The watch alone is not enough: measured,
  // the minimap mask's entry held a different texture for the whole session
  // while the guest had the circle sitting at that very address.
  e.content_hash = 0;
  e.verified_frame = context.frame_index();
  if (const uint8_t* g = TranslatePhysicalGuest(fetch.base_address)) {
    if (IsPhysicalRangeReadable(fetch.base_address, src_size)) {
      e.content_hash = HashGuestSampled(g, src_size);
    }
  }
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
