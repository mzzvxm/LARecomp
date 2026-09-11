#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — buffer cache.
// See resource_cache.h for the region/binding split rationale.

#include "resource_cache.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "../guest/guest_resources.h"
#include "context.h"

REXCVAR_DECLARE(uint32_t, mcla_native_gfx_region_kb);
REXCVAR_DECLARE(bool, mcla_native_gfx_verify_regions);

namespace mcla::native_gfx {

namespace {
// Regions are rounded out to this granularity so that the many small
// suballocated fetches inside one pool coalesce into a single resource
// instead of producing thousands of tiny ones.
constexpr uint32_t kRegionGranularity = 4096;

// Ceiling on a merged region.
//
// The merge takes the union of the request and every region it overlaps, and
// a union only grows: one request touching two regions bridges them plus
// everything between, and the result is more likely to overlap the next
// request, so it merges again. Measured runaway: the average geometry upload
// grew from 170 KB to 650 KB purely by giving the ring more room, and
// re-uploads went from 38 to 245 per frame -- a region only has to be dirtied
// anywhere to be re-sent whole.
//
// Past this size the merge is declined and the request gets a region of its
// own. Duplicating bytes across two regions is harmless here: the data is
// read-only to the GPU, and invalidation is by address range, so both copies
// are dirtied by the same guest write.
constexpr uint32_t kMaxRegionBytesDefault = 128u << 10;

inline uint32_t AlignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }
inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Endian-swapping copy. Region bases are 4096-aligned (or dword-aligned on
// the fallback path), so the swap lanes line up with the guest's own dword
// lanes and a straight loop is enough. A trailing partial unit is copied
// through unchanged: it cannot be part of a complete element.
void SwapCopy(uint8_t* dst, const uint8_t* src, uint32_t size, BufferSwap swap) {
  switch (swap) {
    case BufferSwap::k8in32: {
      const uint32_t whole = size & ~3u;
      for (uint32_t i = 0; i < whole; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      if (whole != size) {
        std::memcpy(dst + whole, src + whole, size - whole);
      }
      break;
    }
    case BufferSwap::k8in16: {
      const uint32_t whole = size & ~1u;
      for (uint32_t i = 0; i < whole; i += 2) {
        uint16_t v;
        std::memcpy(&v, src + i, 2);
        v = uint16_t((v >> 8) | (v << 8));
        std::memcpy(dst + i, &v, 2);
      }
      if (whole != size) {
        dst[whole] = src[whole];
      }
      break;
    }
    default:
      std::memcpy(dst, src, size);
      break;
  }
}
}  // namespace

void SwapCopyBytes(uint8_t* dst, const uint8_t* src, uint32_t size, BufferSwap swap) {
  SwapCopy(dst, src, size, swap);
}


void BufferCache::Shutdown(D3D12Context& context) {
  StopWatchingGuestWrites();
  for (RegionMap& map : regions_) {
    for (auto& [base, r] : map) {
      if (r.resource) {
        context.DeferRelease(r.resource.Detach());
      }
    }
    map.clear();
  }
  if (zero_stream_) {
    context.DeferRelease(zero_stream_.Detach());
  }
}

namespace {
// TEMP DIAG (MESHSTALE): FNV-1a over guest bytes. Cheap enough to run on every
// upload and on a handful of draws a frame; the point is only "same or not".
uint64_t HashGuestBytes(const uint8_t* p, uint32_t size) {
  // Eight bytes at a time. Byte-at-a-time FNV was a third of the cost of the
  // whole-region verification it was written for; this is change detection, not
  // cryptography, so mixing whole words is enough and is several times faster.
  uint64_t h = 1469598103934665603ull;
  const uint32_t whole = size & ~7u;
  for (uint32_t i = 0; i < whole; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 1099511628211ull;
    h ^= h >> 29;
  }
  for (uint32_t i = whole; i < size; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}
}  // namespace

namespace {
// Three 64-byte slices of a block -- head, middle, tail -- instead of all 4096.
//
// This is change detection for a RECYCLED buffer: when the streaming system
// drops a new mesh at an address, essentially every byte differs, so sampling
// catches it. Hashing every byte of every bound block was exact and cost
// ~12 MB a frame (wall ~55 ms -> ~110 ms), which trades one bug for another.
// Both sides -- the upload and the check -- must sample identically.
uint64_t HashBlockSampled(const uint8_t* p, uint32_t len) {
  // Eight 64-byte slices spread evenly, i.e. one in every 512 bytes of the
  // block. Three (head/middle/tail) was cheaper and let a real case through --
  // MESHSTALE still reported one region whose changed bytes missed all three.
  constexpr uint32_t kSlice = 64;
  constexpr uint32_t kSlices = 8;
  uint64_t h = uint64_t(len) * 1099511628211ull;
  for (uint32_t i = 0; i < kSlices; ++i) {
    const uint32_t off = len > kSlice ? uint32_t(uint64_t(len - kSlice) * i / (kSlices - 1)) : 0u;
    h ^= HashGuestBytes(p + off, std::min(kSlice, len - off));
    h *= 1099511628211ull;
    h ^= h >> 31;
  }
  return h;
}
}  // namespace

bool BufferCache::VerifyRegion(uint32_t guest_address, uint32_t size, BufferSwap swap,
                               uint32_t* out_region_base, uint32_t* out_region_size,
                               uint64_t* out_uploaded, uint64_t* out_live) {
  Region* r = FindContaining(regions_[uint32_t(swap)], guest_address, size);
  if (!r || r->content_hash == 0 || r->dirty) {
    // A dirty region is one we already know needs re-uploading; comparing it
    // would report a mismatch that the next Resolve is about to fix, which is
    // the opposite of the signal wanted here.
    return false;
  }
  const uint8_t* p = TranslatePhysicalGuest(r->base);
  if (!p || !IsPhysicalRangeReadable(r->base, r->size)) {
    return false;
  }
  if (out_region_base) *out_region_base = r->base;
  if (out_region_size) *out_region_size = r->size;
  if (out_uploaded) *out_uploaded = r->content_hash;
  if (out_live) *out_live = HashGuestBytes(p, r->size);
  return true;
}

BufferCache::Region* BufferCache::FindContaining(RegionMap& map, uint32_t address,
                                                 uint32_t size) {
  if (map.empty()) {
    return nullptr;
  }
  auto it = map.upper_bound(address);
  if (it == map.begin()) {
    return nullptr;
  }
  --it;  // greatest base <= address
  Region& r = it->second;
  if (uint64_t(address) + size <= uint64_t(r.base) + r.size) {
    return &r;
  }
  return nullptr;
}

bool BufferCache::UploadRegion(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                               Region& region, BufferSwap swap) {
  D3D12Context::UploadAlloc staging;
  if (!context.AllocateUpload(region.size, 4, staging, D3D12Context::UploadTag::kGeometry)) {
    stats_.last_failure = "upload ring allocation failed";
    ++stats_.upload_failures;
    return false;
  }
  const uint8_t* src = TranslatePhysicalGuest(region.base);
  if (!src) {
    stats_.last_failure = "physical translation returned null";
    ++stats_.upload_failures;
    return false;
  }
  SwapCopy(static_cast<uint8_t*>(staging.cpu), src, region.size, swap);
  // TEMP DIAG (MESHSTALE): hash of the GUEST bytes this upload carried, so a
  // later draw can ask whether the GPU's copy still matches guest memory. Reads
  // `src` -- ordinary cached memory -- exactly as the note below prescribes.
  region.content_hash = HashGuestBytes(src, region.size);
  {
    const uint32_t blocks = (region.size + kVerifyBlock - 1u) / kVerifyBlock;
    region.block_hash.assign(blocks, 0);
    region.block_frame.assign(blocks, 0);
    for (uint32_t b = 0; b < blocks; ++b) {
      const uint32_t off = b * kVerifyBlock;
      region.block_hash[b] = HashBlockSampled(src + off, std::min(kVerifyBlock, region.size - off));
    }
  }
  // The content hash lived here and has been removed. It answered its question
  // -- 27% of invalidation-driven re-uploads carried byte-identical data -- but
  // it read back from `staging.cpu`, which is an upload heap: write-combined,
  // uncached, and murderously slow to READ. It took geom from 25ms to 114ms a
  // frame, so the timing measured alongside it was meaningless. If this is ever
  // wanted again, hash `src` (ordinary cached guest memory) and store the swap
  // mode next to the hash, because the same bytes under a different swap are
  // not the same upload. Never read back from staging.

  if (region.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Transition.pResource = region.resource.Get();
    barrier.Transition.StateBefore = region.state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);
  }
  cl->CopyBufferRegion(region.resource.Get(), 0, staging.buffer, staging.offset, region.size);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Transition.pResource = region.resource.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter =
      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &barrier);
  region.state = barrier.Transition.StateAfter;
  region.dirty = false;
  // The watch is consumed when it fires, so it has to be re-armed after every
  // upload or the region would only ever be invalidated once.
  WatchRegion(region);
  return true;
}

bool BufferCache::Resolve(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                          uint32_t guest_address, uint32_t size, BufferSwap swap,
                          BufferBinding& out) {
  out = BufferBinding{};
  // Guest buffer addresses arrive spelled two different ways: a vertex
  // stream's comes from the fetch constant and is already physical
  // (0x10F13E40), while an index buffer's comes from the D3D object and still
  // carries its physical-heap window (0xB0710270). The write watch reports
  // physical addresses, so a region keyed by the windowed spelling never
  // matched an invalidation: its contents were uploaded once and then frozen
  // for the rest of the run. Normalising here is also what keeps the same
  // memory from living in two host regions under two keys.
  guest_address &= 0x1FFFFFFFu;
  stats_.last_failure = nullptr;
  stats_.last_failure_addr = guest_address;
  stats_.last_failure_size = size;
  if (!cl || size == 0) {
    stats_.last_failure = cl ? "size == 0" : "no command list";
    ++stats_.upload_failures;
    return false;
  }
  // Ranges the guest overwrote since the last draw, queued by the write watch
  // from other threads. Applying them here, before the lookup, is what keeps a
  // recycled vertex buffer from being drawn with its previous contents.
  ApplyPendingInvalidations();

  RegionMap& map = regions_[uint32_t(swap)];
  Region* region = FindContaining(map, guest_address, size);
  // The readability check exists to keep UploadRegion from reading unmapped
  // guest pages, so it belongs on the paths that actually read them. Running it
  // first cost the whole frame: IsPhysicalRangeReadable walks the page table of
  // the entire range, under the heap's recursive mutex, once per physical alias
  // (0xE0 reports false for the 64 KiB allocations most vertex data lives in, so
  // the common case pays for two full walks) -- and it ran twice per draw, for
  // the vertex stream and the index buffer, on a path where nothing is read at
  // all. Nothing marks a region dirty today (BufferCache::InvalidateRange has no
  // caller), so after warmup every Resolve is a hit and every one of those walks
  // was pure overhead. Measured before this: 250 ms of frame time with
  // gpu_wait = 0.
  const auto readable = [&]() {
    if (IsPhysicalRangeReadable(guest_address, size)) {
      return true;
    }
    stats_.last_failure = "guest range not readable (physical)";
    ++stats_.unreadable;
    return false;
  };
  // A clean region is only clean because the write watch says so, and measured
  // on MCLA it lies: five xPed vertex regions held bytes that differed from
  // guest memory for 200 consecutive observations each, none of them marked
  // dirty -- the watch never fired for those writes. The game recycles vertex
  // buffer memory constantly while streaming, so a lost notification means a
  // new mesh renders with the previous mesh's bytes (the exploded pedestrians).
  //
  // This re-checks the region's own hash against guest memory and marks it
  // dirty when they differ, which turns a lost notification into a re-upload
  // instead of a corrupt draw. Once per region per frame: a region bound by
  // twenty draws is hashed once, and only regions something actually binds are
  // touched at all.
  if (region && !region->dirty && REXCVAR_GET(mcla_native_gfx_verify_regions) &&
      !region->block_hash.empty()) {
    // Only the blocks this request actually reads, and each at most once a
    // frame. Verifying the whole region was correct and cost 25 MB of hashing
    // per frame (wall ~55 ms -> ~150 ms); a draw cannot be corrupted by bytes
    // it does not read.
    if (const uint8_t* p = TranslatePhysicalGuest(region->base)) {
      const uint32_t rel = guest_address - region->base;
      const uint32_t first = rel / kVerifyBlock;
      const uint32_t last =
          std::min<uint32_t>((rel + size - 1u) / kVerifyBlock,
                             uint32_t(region->block_hash.size()) - 1u);
      const uint32_t frame32 = uint32_t(frame_) | 1u;  // 0 means "never checked"
      for (uint32_t b = first; b <= last; ++b) {
        if (region->block_frame[b] == frame32) {
          continue;
        }
        region->block_frame[b] = frame32;
        const uint32_t off = b * kVerifyBlock;
        const uint32_t len = std::min(kVerifyBlock, region->size - off);
        stats_.verify_bytes += 512u;  // eight 64-byte slices, not the block
        ++stats_.verify_regions;
        if (HashBlockSampled(p + off, len) != region->block_hash[b]) {
          region->dirty = true;
          ++stats_.verify_catches;
          break;
        }
      }
    }
  }
  if (region && !region->dirty) {
    ++stats_.hits;
  } else if (region) {
    if (!readable()) {
      return false;
    }
    if (!UploadRegion(context, cl, *region, swap)) {
      return false;
    }
    ++stats_.reuploads;
  } else {
    if (!readable()) {
      return false;
    }
    // Build a region covering the request plus anything it overlaps, so the
    // same bytes never live in two resources.
    uint32_t lo = AlignDown(guest_address, kRegionGranularity);
    uint32_t hi = AlignUp(uint32_t(uint64_t(guest_address) + size), kRegionGranularity);
    const uint32_t max_region = REXCVAR_GET(mcla_native_gfx_region_kb)
                                    ? uint32_t(REXCVAR_GET(mcla_native_gfx_region_kb)) << 10
                                    : kMaxRegionBytesDefault;
    std::vector<uint32_t> merged;
    for (auto it = map.begin(); it != map.end();) {
      const uint32_t r_lo = it->second.base;
      const uint32_t r_hi = it->second.base + it->second.size;
      if (r_lo < hi && lo < r_hi) {
        const uint32_t new_lo = r_lo < lo ? r_lo : lo;
        const uint32_t new_hi = r_hi > hi ? r_hi : hi;
        if (new_hi - new_lo > max_region) {
          // Declining keeps this request's own region small. The overlapping
          // region stays as it is and keeps serving whoever it already covers.
          ++stats_.merge_declined;
          ++it;
          continue;
        }
        lo = new_lo;
        hi = new_hi;
        merged.push_back(r_lo);
        ++it;
      } else if (r_lo >= hi) {
        break;
      } else {
        ++it;
      }
    }
    for (uint32_t base : merged) {
      auto it = map.find(base);
      if (it != map.end()) {
        if (it->second.resource) {
          context.DeferRelease(it->second.resource.Detach());
        }
        map.erase(it);
        region_index_stale_ = true;
        ++stats_.merges;
      }
    }
    // The merged extent must still be fully readable before it is copied.
    const uint32_t region_size = hi - lo;
    if (!IsPhysicalRangeReadable(lo, region_size)) {
      // Fall back to the exact requested range, which was already validated.
      // Keep the base dword-aligned so the swap lanes stay in step with the
      // guest's; view_offset absorbs the difference.
      lo = AlignDown(guest_address, 4);
      hi = AlignUp(uint32_t(uint64_t(guest_address) + size), 4);
    }

    // A declined merge leaves overlapping regions in the map, and one of them
    // can share this exact base -- map::emplace would then keep the OLD,
    // smaller region and hand it back as if it were the new one, so the view
    // built for the request runs past its end ("buffer view out of range").
    // Anything the new extent fully covers is redundant, so drop it first;
    // regions that stick out beyond the extent are left alone, which is the
    // whole point of declining.
    for (auto it = map.lower_bound(lo); it != map.end() && it->second.base < hi;) {
      const uint32_t r_lo = it->second.base;
      const uint32_t r_hi = r_lo + it->second.size;
      if (r_lo >= lo && r_hi <= hi) {
        if (it->second.resource) {
          context.DeferRelease(it->second.resource.Detach());
        }
        it = map.erase(it);
        region_index_stale_ = true;
        ++stats_.merges;
      } else {
        ++it;
      }
    }

    Region fresh;
    fresh.base = lo;
    fresh.size = hi - lo;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = fresh.size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(context.device()->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&fresh.resource)))) {
      REXLOG_ERROR("[native_gfx] buffer region creation failed ({:#010x}+{})", fresh.base,
                   fresh.size);
      stats_.last_failure = "CreateCommittedResource failed";
      ++stats_.upload_failures;
      return false;
    }
    fresh.state = D3D12_RESOURCE_STATE_COPY_DEST;
    auto [it, inserted] = map.emplace(fresh.base, std::move(fresh));
    if (!inserted) {
      // Should be unreachable after the sweep above, but silently reusing a
      // region that does not cover the request is exactly the failure this
      // path just fixed, so refuse instead of rendering from the wrong bytes.
      REXLOG_ERROR("[native_gfx] region {:#010x}+{} already present, cannot cover {:#010x}+{}",
                   it->second.base, it->second.size, guest_address, size);
      stats_.last_failure = "region base collision";
      ++stats_.upload_failures;
      return false;
    }
    region_index_stale_ = true;
    region = &it->second;
    if (!UploadRegion(context, cl, *region, swap)) {
      return false;
    }
    ++stats_.uploads;
  }

  out.resource = region->resource.Get();
  out.region_base = region->base;
  out.region_size = region->size;
  out.view_offset = guest_address - region->base;
  out.gpu_address = region->resource->GetGPUVirtualAddress() + out.view_offset;
  // Bounds: the whole requested range must sit inside the region.
  if (uint64_t(out.view_offset) + size > region->size) {
    REXLOG_ERROR("[native_gfx] buffer view out of range: {:#010x}+{} in region {:#010x}+{}",
                 guest_address, size, region->base, region->size);
    out = BufferBinding{};
    stats_.last_failure = "view out of region bounds";
    ++stats_.upload_failures;
    return false;
  }
  return true;
}

D3D12_GPU_VIRTUAL_ADDRESS BufferCache::ZeroStreamAddress(D3D12Context& context) {
  if (zero_stream_) {
    return zero_stream_->GetGPUVirtualAddress();
  }
  // 256 bytes rather than the 16 a float4 needs: a vertex buffer view must not
  // run past the resource, and binding it with stride 0 makes every vertex read
  // offset 0 regardless, so the extra room costs nothing and keeps the view
  // valid if a caller ever uses a non-zero stride.
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 256;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // A committed default-heap resource is zero-initialised by D3D12, so this
  // needs no upload and no command list.
  if (FAILED(context.device()->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr,
          IID_PPV_ARGS(&zero_stream_)))) {
    REXLOG_ERROR("[native_gfx] zero vertex stream creation failed");
    return 0;
  }
  return zero_stream_->GetGPUVirtualAddress();
}

void BufferCache::Census(size_t& count, uint64_t& bytes) const {
  count = 0;
  bytes = 0;
  for (const RegionMap& map : regions_) {
    count += map.size();
    for (const auto& [base, r] : map) {
      bytes += r.size;
    }
  }
}

void BufferCache::InvalidateRange(uint32_t guest_address, uint32_t size) {
  // Both sides compared in physical space: the watch reports physical
  // addresses, and Resolve keys every region the same way.
  const uint64_t lo = guest_address & 0x1FFFFFFFu;
  const uint64_t hi = lo + size;
  for (RegionMap& map : regions_) {
    for (auto& [base, r] : map) {
      if (uint64_t(r.base & 0x1FFFFFFFu) < hi &&
          lo < uint64_t(r.base & 0x1FFFFFFFu) + r.size) {
        r.dirty = true;
      }
    }
  }
}

void BufferCache::NoteGuestWrite(uint32_t guest_address, uint32_t size) {
  if (!size) {
    return;
  }
  std::lock_guard<std::mutex> lock(invalidation_mutex_);
  pending_invalidations_.emplace_back(guest_address, size);
  ++stats_.unlock_invalidations;
  stats_.unlock_bytes += size;
}

std::pair<uint32_t, uint32_t> BufferCache::InvalidationThunk(void* context_ptr,
                                                             uint32_t physical_address_start,
                                                             uint32_t length, bool exact_range) {
  (void)exact_range;
  auto* self = static_cast<BufferCache*>(context_ptr);
  if (self && length) {
    // Runs on whichever guest thread performed the write, inside the memory
    // system's global critical region. Touching regions_ here would race the
    // draw thread and invite a lock-order inversion against the heap mutex, so
    // this only records the range; Resolve applies it.
    std::lock_guard<std::mutex> lock(self->invalidation_mutex_);
    self->pending_invalidations_.emplace_back(physical_address_start, length);
  }
  // Nothing to keep watched beyond the range that fired.
  return std::make_pair(physical_address_start, length);
}

bool BufferCache::StartWatchingGuestWrites() {
  if (invalidation_handle_) {
    return true;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("[native_gfx] buffer write watch: no memory system");
    return false;
  }
  invalidation_handle_ = memory->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  if (!invalidation_handle_) {
    REXLOG_ERROR("[native_gfx] buffer write watch registration failed");
    return false;
  }
  REXLOG_INFO("[native_gfx] buffer write watch registered");
  return true;
}

void BufferCache::StopWatchingGuestWrites() {
  if (!invalidation_handle_) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  if (auto* memory = runtime ? runtime->memory() : nullptr) {
    memory->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
  }
  invalidation_handle_ = nullptr;
}

void BufferCache::WatchRegion(const Region& region) {
  if (!invalidation_handle_ || !region.size) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  if (auto* memory = runtime ? runtime->memory() : nullptr) {
    // Invalidation notifications only; this cache has no data provider to
    // service a read fault with.
    memory->EnablePhysicalMemoryAccessCallbacks(region.base & 0x1FFFFFFFu, region.size,
                                                /*enable_invalidation_notifications=*/true,
                                                /*enable_data_providers=*/false);
  }
}

void BufferCache::ApplyPendingInvalidations() {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    if (pending_invalidations_.empty()) {
      return;
    }
    ranges.swap(pending_invalidations_);
  }

  // This used to be `for (range) InvalidateRange(range)`, and InvalidateRange
  // walks EVERY region. Measured: 1141 live regions and ~1300 pending ranges a
  // frame = 1.49 MILLION red-black-tree node visits per frame, about 22ms --
  // which is essentially all of the 25.5ms that BufferCache::Resolve cost, and
  // it was hiding inside a counter that only said "geom is slow". The uploads
  // it was blamed on are 1.8ms.
  //
  // Coalescing first turns it into ONE pass: sort the ranges, merge the ones
  // that touch, then visit each region once and binary-search it against the
  // merged list. Deliberately makes no assumption about how region keys order
  // against physical addresses -- the map is keyed by guest base while the
  // comparison is physical, and guest->physical is not a plain mask, so using
  // the map's own ordering to narrow the walk would be wrong.
  std::vector<std::pair<uint64_t, uint64_t>> merged;
  merged.reserve(ranges.size());
  for (const auto& [start, length] : ranges) {
    const uint64_t lo = start & 0x1FFFFFFFu;
    merged.emplace_back(lo, lo + length);
  }
  std::sort(merged.begin(), merged.end());
  size_t w = 0;
  for (size_t i = 0; i < merged.size(); ++i) {
    if (w != 0 && merged[i].first <= merged[w - 1].second) {
      if (merged[i].second > merged[w - 1].second) {
        merged[w - 1].second = merged[i].second;
      }
    } else {
      merged[w++] = merged[i];
    }
  }
  merged.resize(w);

  if (region_index_stale_) {
    RebuildRegionIndex();
  }
  // Walk the RANGES and binary-search the regions, not the other way round.
  // Regions are disjoint and sorted by physical start, so for each range the
  // candidates are a contiguous run: the first entry that can reach into it,
  // then forward while the next one still starts before the range ends.
  for (const auto& [lo, hi] : merged) {
    auto it = std::lower_bound(region_index_.begin(), region_index_.end(), lo,
                               [](const RegionIndexEntry& e, uint64_t v) {
                                 return e.physical_hi <= v;
                               });
    for (; it != region_index_.end() && it->physical_lo < hi; ++it) {
      ++stats_.inval_scan_steps;
      if (!it->region->dirty) {
        ++stats_.regions_dirtied;
      }
      it->region->dirty = true;
    }
  }
}

void BufferCache::RebuildRegionIndex() {
  region_index_.clear();
  for (RegionMap& map : regions_) {
    for (auto& [base, r] : map) {
      const uint64_t lo = r.base & 0x1FFFFFFFu;
      region_index_.push_back({lo, lo + r.size, &r});
    }
  }
  std::sort(region_index_.begin(), region_index_.end(),
            [](const RegionIndexEntry& a, const RegionIndexEntry& b) {
              return a.physical_lo < b.physical_lo;
            });
  region_index_stale_ = false;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
