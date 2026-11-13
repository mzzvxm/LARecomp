// MCLA GPU renderer -- what the process holds, by kind. See process_memory.h.

#include "process_memory.h"

#include <cstddef>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

REXCVAR_DEFINE_UINT32(
    mcla_gpu_process_memory_log, 0, "MCLA/GPU",
    "Diagnostic: every this many frames, log what the process's memory is made of (the commit "
    "charge and working set, the committed address space by kind, the private regions by size, "
    "the process heaps, the biggest regions). It walks the whole address space, which takes "
    "some milliseconds on the render thread each time. 0: off.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

#if defined(_WIN32)

namespace {

constexpr uint64_t kMiB = 1ull << 20;
// The guest arena: the 4 GiB virtual view and the 512 MiB physical one after
// it, one pagefile-backed section (xmemory.cpp: mapping_size 0x120000000).
constexpr uint64_t kGuestArenaBytes = 0x120000000ull + 0x10000ull;

constexpr size_t kBuckets = 5;
constexpr const char* kBucketNames[kBuckets] = {"<1 MiB", "1-4 MiB", "4-16 MiB", "16-64 MiB",
                                                "64+ MiB"};
constexpr size_t kTop = 6;

size_t BucketOf(uint64_t size) {
  if (size < 1 * kMiB) return 0;
  if (size < 4 * kMiB) return 1;
  if (size < 16 * kMiB) return 2;
  if (size < 64 * kMiB) return 3;
  return 4;
}

struct Region {
  uintptr_t base = 0;
  uint64_t size = 0;
  const char* kind = "";
  bool guest = false;
};

struct Census {
  uint64_t private_bytes = 0;
  uint64_t private_regions = 0;
  uint64_t mapped_bytes = 0;
  uint64_t mapped_regions = 0;
  uint64_t guest_bytes = 0;  // the part of the mapped bytes inside the guest arena
  uint64_t image_bytes = 0;
  uint64_t reserved_bytes = 0;  // reserved, not committed: costs no memory
  uint64_t bucket_bytes[kBuckets] = {};
  uint64_t bucket_regions[kBuckets] = {};
  Region top[kTop];
};

void AddTop(Census& c, const Region& r) {
  for (size_t i = 0; i < kTop; ++i) {
    if (r.size > c.top[i].size) {
      for (size_t j = kTop - 1; j > i; --j) {
        c.top[j] = c.top[j - 1];
      }
      c.top[i] = r;
      return;
    }
  }
}

Census Walk(uintptr_t guest_low) {
  Census c;
  const uintptr_t guest_high = guest_low ? guest_low + uintptr_t(kGuestArenaBytes) : 0;
  MEMORY_BASIC_INFORMATION mbi = {};
  uintptr_t address = 0;
  while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uint64_t size = uint64_t(mbi.RegionSize);
    if (mbi.State == MEM_COMMIT) {
      Region r;
      r.base = base;
      r.size = size;
      r.guest = guest_low != 0 && base >= guest_low && base < guest_high;
      if (mbi.Type == MEM_IMAGE) {
        r.kind = "image";
        c.image_bytes += size;
      } else if (mbi.Type == MEM_MAPPED) {
        r.kind = "mapped";
        c.mapped_bytes += size;
        ++c.mapped_regions;
        if (r.guest) {
          c.guest_bytes += size;
        }
      } else {
        r.kind = "private";
        c.private_bytes += size;
        ++c.private_regions;
        const size_t bucket = BucketOf(size);
        c.bucket_bytes[bucket] += size;
        ++c.bucket_regions[bucket];
      }
      AddTop(c, r);
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

void Report(uint64_t frame) {
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                       sizeof(pmc));

  const rex::Runtime* runtime = rex::Runtime::instance();
  const uint8_t* membase = runtime ? runtime->virtual_membase() : nullptr;
  const Census c = Walk(reinterpret_cast<uintptr_t>(membase));

  // The CRT's malloc and every other heap of the process: what was allocated
  // against what the heap holds committed (the difference is fragmentation and
  // free blocks a heap has not given back).
  // CHECK: not compiled; HeapSummary and HEAP_SUMMARY come with windows.h (heapapi.h, winnt.h)
  // on the Windows SDKs from 7 on, which the build uses.
  constexpr DWORD kMaxHeaps = 64;
  HANDLE heaps[kMaxHeaps];
  const DWORD heap_count = GetProcessHeaps(kMaxHeaps, heaps);
  uint64_t heap_allocated = 0;
  uint64_t heap_committed = 0;
  DWORD heaps_summed = 0;
  for (DWORD i = 0; i < heap_count && i < kMaxHeaps; ++i) {
    HEAP_SUMMARY summary = {};
    summary.cb = sizeof(summary);
    if (HeapSummary(heaps[i], 0, &summary)) {
      heap_allocated += uint64_t(summary.cbAllocated);
      heap_committed += uint64_t(summary.cbCommitted);
      ++heaps_summed;
    }
  }

  REXLOG_INFO(
      "[gpu] process memory (frame {}): commit {} MiB, working set {} MiB (peak {} MiB) | "
      "committed address space: private {} MiB in {} regions, mapped {} MiB in {} regions (guest "
      "arena {} MiB), image {} MiB | reserved only {} MiB | {} heaps: {} MiB allocated, {} MiB "
      "committed",
      frame, uint64_t(pmc.PrivateUsage) / kMiB, uint64_t(pmc.WorkingSetSize) / kMiB,
      uint64_t(pmc.PeakWorkingSetSize) / kMiB, c.private_bytes / kMiB, c.private_regions,
      c.mapped_bytes / kMiB, c.mapped_regions, c.guest_bytes / kMiB, c.image_bytes / kMiB,
      c.reserved_bytes / kMiB, heaps_summed, heap_allocated / kMiB, heap_committed / kMiB);

  std::string sizes;
  for (size_t i = 0; i < kBuckets; ++i) {
    sizes += std::format("{}{} {} MiB in {}", i ? ", " : "", kBucketNames[i],
                         c.bucket_bytes[i] / kMiB, c.bucket_regions[i]);
  }
  std::string biggest;
  for (const Region& r : c.top) {
    if (r.size == 0) {
      break;
    }
    biggest += std::format("{}{:#x} {} MiB {}{}", biggest.empty() ? "" : ", ", uint64_t(r.base),
                           r.size / kMiB, r.kind, r.guest ? " (guest)" : "");
  }
  REXLOG_INFO("[gpu] process memory (frame {}): private regions by size: {} | biggest committed "
              "regions: {}",
              frame, sizes, biggest);
}

}  // namespace

void ProcessMemoryTick(uint64_t frame) {
  const uint32_t every = REXCVAR_GET(mcla_gpu_process_memory_log);
  if (every == 0) {
    return;
  }
  // Render thread only: no lock.
  static uint64_t next = 0;
  if (frame < next) {
    return;
  }
  next = frame + every;
  Report(frame);
}

#else

void ProcessMemoryTick(uint64_t) {}

#endif

}  // namespace mcla::gpu
