#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — memory census. See memory_census.h.

#include "memory_census.h"

#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/d3d12/d3d12_api.h>

#include <psapi.h>

#include "context.h"
#include "pipeline_cache.h"
#include "render_target_pool.h"
#include "resource_cache.h"
#include "texture_cache.h"

REXCVAR_DECLARE(uint32_t, mcla_native_gfx_census);

namespace mcla::native_gfx {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

// Walks the process address space and buckets every COMMITTED region. This is
// what separates "a cache is leaking" from "something reserved gigabytes at
// boot": the native caches only accounted for 258 MiB of a 3.3 GiB commit, and
// the first report already showed 2.7 GiB at frame 16, so the bulk is neither a
// leak nor ours. Private commit is heap and guest memory; image and mapped are
// the recompiled binary and file mappings. The largest private regions are
// printed with their base address so they can be attributed -- a region at the
// guest membase is the guest arena, anything else is host heap.
struct AddressSpaceCensus {
  uint64_t private_bytes = 0;
  uint64_t image_bytes = 0;
  uint64_t mapped_bytes = 0;
  uint64_t reserved_bytes = 0;  // reserved but not committed: costs no memory
  uint64_t guest_arena_bytes = 0;
  // Split by type: whether the arena lands in the process commit charge
  // (private) or in the pagefile-backed section accounting (mapped) decides
  // whether it is part of the number Task Manager's commit column shows.
  uint64_t guest_arena_private = 0;
  uint64_t guest_arena_mapped = 0;
  size_t private_regions = 0;
  // The three biggest private committed regions, for attribution.
  uint64_t top_base[3] = {};
  uint64_t top_size[3] = {};
  // Private commit bucketed by region size. A heap that grew organically has a
  // smooth spread; a block allocator shows up as one bucket holding almost
  // everything in regions of identical size, which names the allocator without
  // needing to hook it. Buckets: <1, 1-4, 4-16, 16-64, >=64 MiB.
  static constexpr size_t kBuckets = 5;
  size_t bucket_count[kBuckets] = {};
  uint64_t bucket_bytes[kBuckets] = {};
};

size_t SizeBucket(uint64_t size) {
  if (size < 1 * kMiB) return 0;
  if (size < 4 * kMiB) return 1;
  if (size < 16 * kMiB) return 2;
  if (size < 64 * kMiB) return 3;
  return 4;
}

AddressSpaceCensus WalkAddressSpace(const uint8_t* guest_membase) {
  AddressSpaceCensus c;
  // 8 GiB, not 4: the physical membase is the virtual one + 4 GiB (xmemory.cpp,
  // `physical_membase_ = mapping_base_ + 0x100000000ull`), so the arena spans
  // both aliases. It is sparse address space; only the committed pages inside it
  // cost anything, so it is measured rather than assumed.
  const uintptr_t guest_lo = reinterpret_cast<uintptr_t>(guest_membase);
  const uintptr_t guest_hi = guest_lo ? guest_lo + (8ull << 30) : 0;

  MEMORY_BASIC_INFORMATION mbi = {};
  uintptr_t address = 0;
  while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uint64_t size = uint64_t(mbi.RegionSize);
    if (mbi.State == MEM_COMMIT) {
      // Checked for EVERY type, not just private: guest memory is a pagefile-
      // backed section (the virtual/physical aliasing needs one), so it comes
      // back as MEM_MAPPED. Testing the range only inside the private branch
      // reported guest_arena=0 while 4.4 GiB sat in `mapped`.
      if (guest_lo && base >= guest_lo && base < guest_hi) {
        c.guest_arena_bytes += size;
        if (mbi.Type == MEM_MAPPED) {
          c.guest_arena_mapped += size;
        } else if (mbi.Type != MEM_IMAGE) {
          c.guest_arena_private += size;
        }
      }
      switch (mbi.Type) {
        case MEM_IMAGE:
          c.image_bytes += size;
          break;
        case MEM_MAPPED:
          c.mapped_bytes += size;
          break;
        default:  // MEM_PRIVATE
          c.private_bytes += size;
          ++c.private_regions;
          {
            const size_t b = SizeBucket(size);
            ++c.bucket_count[b];
            c.bucket_bytes[b] += size;
          }
          for (int i = 0; i < 3; ++i) {
            if (size > c.top_size[i]) {
              for (int j = 2; j > i; --j) {
                c.top_size[j] = c.top_size[j - 1];
                c.top_base[j] = c.top_base[j - 1];
              }
              c.top_size[i] = size;
              c.top_base[i] = base;
              break;
            }
          }
          break;
      }
    } else if (mbi.State == MEM_RESERVE) {
      c.reserved_bytes += size;
    }
    const uintptr_t next = base + uintptr_t(size);
    if (next <= address) {
      break;  // no forward progress: stop rather than spin
    }
    address = next;
  }
  return c;
}

// The adapter the shared device runs on, resolved once from its LUID. Held as a
// raw pointer with one reference for the process lifetime: the census outlives
// every frame and re-querying the factory per report is pure overhead.
IDXGIAdapter3* AdapterFor(ID3D12Device* device) {
  static IDXGIAdapter3* cached = nullptr;
  static bool tried = false;
  if (tried) {
    return cached;
  }
  tried = true;
  if (!device) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (SUCCEEDED(adapter.As(&adapter3))) {
    cached = adapter3.Detach();  // one reference, held for the process lifetime
  }
  return cached;
}

}  // namespace

void MemoryCensusTick(D3D12Context& context, const BufferCache& buffers,
                      const TextureCache& textures, const PipelineCache& pipelines,
                      const RenderTargetPool& render_targets) {
  const uint32_t interval = REXCVAR_GET(mcla_native_gfx_census);
  if (interval == 0) {
    return;
  }
  static uint64_t tick = 0;
  if ((tick++ % interval) != 0) {
    return;
  }

  // Process side. PrivateUsage is the commit charge (what the process asked the
  // OS for); WorkingSetSize is what is resident. A resource leak paged out of
  // VRAM inflates both.
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(),
                       reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

  // Adapter side. Local = VRAM, non-local = system memory the GPU addresses,
  // which is where a default heap goes once VRAM is full.
  DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonlocal = {};
  if (IDXGIAdapter3* adapter = AdapterFor(context.device())) {
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
  }

  size_t buffer_count = 0;
  uint64_t buffer_bytes = 0;
  buffers.Census(buffer_count, buffer_bytes);
  const RenderTargetPool::Census rt = render_targets.TakeCensus(context.device());
  const TextureCache::Stats& tex = textures.stats();

  // Growth between reports is what identifies the culprit: every pool prints its
  // absolute size, and the one whose delta tracks the process delta is the leak.
  static uint64_t prev_private = 0;
  const uint64_t private_mib = uint64_t(pmc.PrivateUsage) / kMiB;
  const int64_t delta = int64_t(private_mib) - int64_t(prev_private);
  prev_private = private_mib;

  FILE* f = std::fopen("native_gfx_mem.txt", "ab");
  if (!f) {
    return;
  }
  std::fprintf(f,
               "frame=%llu commit=%llu MiB (%+lld) ws=%llu MiB | vram_local=%llu/%llu MiB "
               "nonlocal=%llu MiB | tex=%zu/%llu MiB evict=%llu/%llu MiB | buf=%zu/%llu MiB | "
               "pso=%zu | rt=%zu/%llu MiB resolved=%zu/%llu MiB orphan=%zu pendcopy=%zu "
               "produced=%zu | pending_release=%zu\n",
               static_cast<unsigned long long>(context.frame_index()),
               static_cast<unsigned long long>(private_mib), static_cast<long long>(delta),
               static_cast<unsigned long long>(uint64_t(pmc.WorkingSetSize) / kMiB),
               static_cast<unsigned long long>(local.CurrentUsage / kMiB),
               static_cast<unsigned long long>(local.Budget / kMiB),
               static_cast<unsigned long long>(nonlocal.CurrentUsage / kMiB),
               textures.entry_count(), static_cast<unsigned long long>(tex.live_bytes / kMiB),
               static_cast<unsigned long long>(tex.evictions),
               static_cast<unsigned long long>(tex.evicted_bytes / kMiB), buffer_count,
               static_cast<unsigned long long>(buffer_bytes / kMiB), pipelines.pipeline_count(),
               rt.targets, static_cast<unsigned long long>(rt.target_bytes / kMiB), rt.resolved,
               static_cast<unsigned long long>(rt.resolved_bytes / kMiB), rt.orphaned,
               rt.pending_copies, rt.gpu_produced, context.pending_release_count());

  // Address-space breakdown, on its own line: it answers a different question
  // from the cache sizes above (WHAT holds the commit, not WHICH cache grew).
  const uint8_t* membase = rex::Runtime::instance() ? rex::Runtime::instance()->virtual_membase()
                                                    : nullptr;
  const AddressSpaceCensus as = WalkAddressSpace(membase);
  std::fprintf(f,
               "  vm: private=%llu MiB (%zu regions, guest_arena=%llu MiB = %llu priv + %llu "
               "mapped) image=%llu MiB "
               "mapped=%llu MiB reserved=%llu MiB | top: %p=%llu MiB %p=%llu MiB %p=%llu MiB\n",
               static_cast<unsigned long long>(as.private_bytes / kMiB), as.private_regions,
               static_cast<unsigned long long>(as.guest_arena_bytes / kMiB),
               static_cast<unsigned long long>(as.guest_arena_private / kMiB),
               static_cast<unsigned long long>(as.guest_arena_mapped / kMiB),
               static_cast<unsigned long long>(as.image_bytes / kMiB),
               static_cast<unsigned long long>(as.mapped_bytes / kMiB),
               static_cast<unsigned long long>(as.reserved_bytes / kMiB),
               reinterpret_cast<void*>(as.top_base[0]),
               static_cast<unsigned long long>(as.top_size[0] / kMiB),
               reinterpret_cast<void*>(as.top_base[1]),
               static_cast<unsigned long long>(as.top_size[1] / kMiB),
               reinterpret_cast<void*>(as.top_base[2]),
               static_cast<unsigned long long>(as.top_size[2] / kMiB));
  std::fprintf(f,
               "  private by region size: <1MiB=%zu/%llu MiB 1-4=%zu/%llu MiB 4-16=%zu/%llu MiB "
               "16-64=%zu/%llu MiB >=64=%zu/%llu MiB\n",
               as.bucket_count[0], static_cast<unsigned long long>(as.bucket_bytes[0] / kMiB),
               as.bucket_count[1], static_cast<unsigned long long>(as.bucket_bytes[1] / kMiB),
               as.bucket_count[2], static_cast<unsigned long long>(as.bucket_bytes[2] / kMiB),
               as.bucket_count[3], static_cast<unsigned long long>(as.bucket_bytes[3] / kMiB),
               as.bucket_count[4], static_cast<unsigned long long>(as.bucket_bytes[4] / kMiB));
  std::fflush(f);
  std::fclose(f);
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
