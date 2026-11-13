#pragma once
// ===========================================================================
// MCLA GPU renderer -- the video memory budget of a GPU that shares memory with the CPU
// ===========================================================================
// What the caches trim against (vram_budget.h) is Device::QueryVideoMemory: the process's use and
// the OS's budget for "the adapter's local video memory". On a card with its own memory that is one
// heap and the number is right. On an integrated GPU (the Steam Deck's APU, a desktop CPU's iGPU, a
// laptop's) there is no such heap: the "video memory" is system memory, carved up by the driver, and
// which part of it the API calls local differs by API and driver. The question is whether the number
// the caches get is the memory the GPU can really use, or only the slice the API labels "local"
// (the Deck's BIOS carve-out is 1 GiB by default, up to 4 GiB, while the GPU can use far more).
//
// What each API says (perf\UMA-BUDGET.md has the sources, the measurements and the confidence of each):
//   D3D12/DXGI   a UMA adapter has ONE pool (Microsoft's residency page), and QueryVideoMemoryInfo's LOCAL
//                segment group is that pool: measured on the 2-CU iGPU, "dedicated VRAM 485 MiB, budget 15648
//                MiB" (DXGI_ADAPTER_DESC's DedicatedVideoMemory is the BIOS carve-out; the budget is the
//                shared memory, about half of the 31 GiB of RAM). So the LOCAL budget is already right.
//   Vulkan/RADV  on an APU the driver exposes two fake heaps, 2/3 of (GTT + carve-out) as a DEVICE_LOCAL
//                heap and 1/3 as a host-only heap (Mesa 21.1, May 2021, commit 08d162f0b57), or ONE
//                DEVICE_LOCAL heap with all of it when the drirc option radv_enable_unified_heap_on_apu is
//                on. The old rule (the biggest DEVICE_LOCAL heap) therefore reads two thirds of what the GPU
//                can use on a Deck, never the 1 GiB carve-out, and counts none of the host-only third.
//                The Windows AMD driver does the same split (measured: 10.54 GiB device-local + 5.27 GiB
//                host heap, 10251 MiB budget for the first).
// So the Vulkan number is low by the host-only third and, on a driver that exposed only the carve-out as
// device-local, could be a thousand MiB. The rule below counts what the GPU can reach.
//
// The rule (shared-memory GPUs only; a discrete card keeps its biggest-device-local-heap reading
// exactly as before):
//   Vulkan   the sum of the budgets (and of the usages) of every heap the GPU can allocate from: the
//            DEVICE_LOCAL ones and the host-only ones that have a HOST_VISIBLE memory type. A heap that
//            reports no budget is left out.
//   D3D12    the LOCAL segment group (the one pool).
//   both     then held to the CAP: at most kRamSharePercent of the machine's RAM (the OS and the CPU
//            side of the game need the rest, and a driver that lists the same memory in two heaps cannot
//            double the budget), and to the FLOOR: at least kFloorBytes (a shared-memory GPU is never
//            "out" of a 1 GiB carve-out: amdgpu moves buffers between the carve-out and GTT) but never
//            more than the cap. The use is NOT floored or capped: it is what the OS says.
// A creation that fails still triggers the response on any adapter (vram_budget.h), so the floor cannot
// hide a real shortage.
//
// This header includes nothing of Plume, Vulkan or DXGI: uma_budget_test.cpp builds on its own and
// checks the rule on Deck-like, iGPU-like and discrete inputs. The backends (backend_vulkan.cpp,
// backend_d3d12.cpp) fill the Heap list from their API and call Compute / ComputeSegment;
// Device::Open logs Describe's text once.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::gpu::uma_budget {

inline constexpr uint64_t kMiB = 1ull << 20;
// The most of the machine's RAM a shared-memory GPU's budget is read as.
inline constexpr uint32_t kRamSharePercent = 75;
// The least a shared-memory GPU's budget is read as (the LARecomp's use at 1x is 0.6-0.8 GiB, 1.5 GiB
// with the pause menu's 4x MSAA targets).
inline constexpr uint64_t kFloorBytes = 2048 * kMiB;
// VK_MAX_MEMORY_HEAPS.
inline constexpr uint32_t kMaxHeaps = 16;

// One memory heap as the API lists it, with what the OS says about this process's share of it.
struct Heap {
  uint64_t size = 0;
  uint64_t budget = 0;  // 0: not reported
  uint64_t usage = 0;
  bool device_local = false;
  bool host_visible = false;  // some memory type of the heap can be mapped: the GPU uses it too
};

enum class Rule : uint8_t {
  kNone,         // nothing reported
  kBiggestHeap,  // a card with its own memory: the biggest device-local heap (the rule before this file)
  kSharedHeaps,  // a shared-memory GPU on Vulkan: every heap the GPU can reach, summed
  kSegment,      // a shared-memory GPU on D3D12: the LOCAL segment group
};

struct Result {
  uint64_t used = 0;
  uint64_t budget = 0;      // what the caches compare with; 0: nothing reported
  uint64_t raw_budget = 0;  // before the cap and the floor
  uint32_t heap_mask = 0;   // bit i: heap i was counted
  uint32_t heaps = 0;       // how many
  Rule rule = Rule::kNone;
  bool capped = false;
  bool floored = false;
  bool reported() const { return budget != 0; }
};

// Whether the GPU shares memory with the CPU. Plume's UMA flag (D3D12: DXGI's UMA; Vulkan: an
// integrated device with a host-visible device-local type), its integrated type (Vulkan only: D3D12
// never fills the type), or a CPU rasteriser.
inline bool IsSharedMemory(bool uma, bool integrated, bool cpu_rasteriser) {
  return uma || integrated || cpu_rasteriser;
}

// The most a shared-memory GPU's budget is read as for this much RAM; UINT64_MAX when the RAM is
// unknown (0).
inline uint64_t RamCap(uint64_t system_bytes) {
  return system_bytes == 0 ? UINT64_MAX : system_bytes * kRamSharePercent / 100;
}

// The least it is read as: the floor, or the cap when that is lower (a machine with 2 GiB of RAM).
inline uint64_t Floor(uint64_t system_bytes) {
  const uint64_t cap = RamCap(system_bytes);
  return kFloorBytes < cap ? kFloorBytes : cap;
}

// The cap and the floor applied to a shared-memory result that reports a budget.
inline void Clamp(Result& r, uint64_t system_bytes) {
  r.raw_budget = r.budget;
  if (r.budget == 0) {
    return;
  }
  const uint64_t cap = RamCap(system_bytes);
  const uint64_t floor = Floor(system_bytes);
  if (r.budget > cap) {
    r.budget = cap;
    r.capped = true;
  }
  if (r.budget < floor) {
    r.budget = floor;
    r.floored = true;
  }
}

// The reading of a card with its own memory, as Device::QueryVideoMemory has always made it: the
// biggest DEVICE_LOCAL heap (the others are the small host-visible window of a discrete card), its
// budget and its usage; nothing when there is none or it reports no budget.
inline Result BiggestHeap(const Heap* heaps, uint32_t count) {
  Result r;
  uint32_t best = UINT32_MAX;
  uint64_t biggest = 0;
  for (uint32_t i = 0; i < count && i < kMaxHeaps; ++i) {
    if (heaps[i].device_local && heaps[i].size > biggest) {
      best = i;
      biggest = heaps[i].size;
    }
  }
  if (best == UINT32_MAX || heaps[best].budget == 0) {
    return r;
  }
  r.used = heaps[best].usage;
  r.budget = heaps[best].budget;
  r.raw_budget = r.budget;
  r.heap_mask = 1u << best;
  r.heaps = 1;
  r.rule = Rule::kBiggestHeap;
  return r;
}

// The reading of a shared-memory GPU on Vulkan: every heap the GPU can allocate from, summed, then
// capped and floored.
inline Result SharedHeaps(const Heap* heaps, uint32_t count, uint64_t system_bytes) {
  Result r;
  for (uint32_t i = 0; i < count && i < kMaxHeaps; ++i) {
    const Heap& h = heaps[i];
    if (!(h.device_local || h.host_visible) || h.budget == 0) {
      continue;
    }
    r.used += h.usage;
    r.budget += h.budget;
    r.heap_mask |= 1u << i;
    ++r.heaps;
  }
  if (r.heaps == 0) {
    return Result{};
  }
  r.rule = Rule::kSharedHeaps;
  Clamp(r, system_bytes);
  return r;
}

// The reading of a shared-memory GPU on D3D12: the LOCAL segment group, capped and floored.
inline Result Segment(uint64_t used, uint64_t budget, uint64_t system_bytes) {
  Result r;
  if (budget == 0) {
    return r;
  }
  r.used = used;
  r.budget = budget;
  r.heaps = 1;
  r.heap_mask = 1;
  r.rule = Rule::kSegment;
  Clamp(r, system_bytes);
  return r;
}

// Vulkan: the reading for this adapter.
inline Result Compute(const Heap* heaps, uint32_t count, bool shared, uint64_t system_bytes) {
  return shared ? SharedHeaps(heaps, count, system_bytes) : BiggestHeap(heaps, count);
}

// The plain-text account of a reading, for the one log line: what kind of adapter, which heaps were
// counted and how they add up, and whether the cap or the floor changed the sum. `heaps` may be null
// (D3D12: no list); `segment_text` then names the segments.
inline std::string Describe(const Result& r, const Heap* heaps, uint32_t count,
                            uint64_t system_bytes, const std::string& segment_text = {}) {
  const auto mib = [](uint64_t bytes) { return std::to_string(bytes / kMiB); };
  if (r.rule == Rule::kNone) {
    return "video memory budget: the adapter reports none (a creation that fails is the only sign "
           "of a shortage)";
  }
  std::string text = "video memory budget: ";
  if (r.rule == Rule::kBiggestHeap) {
    uint32_t index = 0;
    while (index < kMaxHeaps && !(r.heap_mask & (1u << index))) {
      ++index;
    }
    text += "the adapter has its own memory: the biggest device-local heap, heap " +
            std::to_string(index) + ", " + mib(r.budget) + " MiB";
    return text;
  }
  if (r.rule == Rule::kSegment) {
    text += "shared-memory adapter: the LOCAL segment, " + mib(r.raw_budget) + " MiB" +
            (segment_text.empty() ? std::string() : " (" + segment_text + ")");
  } else {
    text += "shared-memory adapter: " + std::to_string(r.heaps) + " heaps counted (";
    bool first = true;
    for (uint32_t i = 0; i < count && i < kMaxHeaps; ++i) {
      if (!(r.heap_mask & (1u << i))) {
        continue;
      }
      text += std::string(first ? "" : ", ") + "heap " + std::to_string(i) +
              (heaps[i].device_local ? " device-local " : " host ") + mib(heaps[i].budget) + " MiB";
      first = false;
    }
    text += ") = " + mib(r.raw_budget) + " MiB";
    for (uint32_t i = 0; i < count && i < kMaxHeaps; ++i) {
      if (!(r.heap_mask & (1u << i)) &&
          (heaps[i].device_local || heaps[i].host_visible)) {
        text += "; heap " + std::to_string(i) + " reports no budget, left out";
      }
    }
  }
  if (r.capped) {
    text += "; held to " + std::to_string(kRamSharePercent) + "% of the " +
            mib(system_bytes) + " MiB of RAM = " + mib(r.budget) + " MiB";
  } else if (r.floored) {
    text += "; raised to the floor, " + mib(r.budget) + " MiB";
  } else if (system_bytes != 0) {
    text += "; within " + std::to_string(kRamSharePercent) + "% of the " + mib(system_bytes) +
            " MiB of RAM";
  }
  if (system_bytes == 0) {
    text += "; the RAM is unknown, no cap";
  }
  return text;
}

// The machine's physical RAM in bytes, 0 when it cannot be read. (uma_budget.cpp.)
uint64_t SystemMemoryBytes();

}  // namespace mcla::gpu::uma_budget
