#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — buffer cache.
// See resource_cache.h for the region/binding split rationale.

#include "resource_cache.h"

#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "../guest/guest_resources.h"
#include "context.h"

namespace mcla::native_gfx {

namespace {
// Regions are rounded out to this granularity so that the many small
// suballocated fetches inside one pool coalesce into a single resource
// instead of producing thousands of tiny ones.
constexpr uint32_t kRegionGranularity = 4096;

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
  if (!context.AllocateUpload(region.size, 4, staging)) {
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
    std::vector<uint32_t> merged;
    for (auto it = map.begin(); it != map.end();) {
      const uint32_t r_lo = it->second.base;
      const uint32_t r_hi = it->second.base + it->second.size;
      if (r_lo < hi && lo < r_hi) {
        lo = r_lo < lo ? r_lo : lo;
        hi = r_hi > hi ? r_hi : hi;
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
  for (const auto& [start, length] : ranges) {
    InvalidateRange(start, length);
  }
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
