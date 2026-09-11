#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest-visible allocator for host objects.
// See host_heap.h for why the allocation has to come out of guest memory.

#include "host_heap.h"

#include <mutex>

#include <o1heap.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

// Defined in guest/texture_ownership.cpp, at global scope like every cvar here.
REXCVAR_DECLARE(uint32_t, mcla_native_gfx_own_heap_kb);

namespace mcla::native_gfx {

namespace {

// Default block size. A texture header is 52 bytes, which o1heap rounds up to
// a 128-byte fragment, so 4 MiB is around 32k live objects -- the registry
// peaks near 4600. GetSnapshot().peak_allocated says when it needs raising.
//
// Kept modest on purpose: see the heap choice in Init(). Must stay a power of
// two, which is what o1heap can use in full.
constexpr uint32_t kHeapSizeDefault = 4u * 1024u * 1024u;

uint32_t HeapSizeFromCvar() {
  const uint64_t kb = REXCVAR_GET(mcla_native_gfx_own_heap_kb);
  if (!kb) {
    return kHeapSizeDefault;
  }
  uint64_t bytes = kb << 10;
  // Round DOWN to a power of two: o1heap only uses the largest power-of-two
  // span of its arena, so anything above one is reserved and never handed out.
  uint32_t pot = 64u * 1024u;
  while (uint64_t(pot) * 2 <= bytes && pot < (1u << 30)) {
    pot *= 2;
  }
  return pot;
}

struct HeapState {
  std::mutex mutex;  // o1heap has no internal synchronization.
  O1HeapInstance* handle = nullptr;
  uint32_t guest_va = 0;
  uint32_t size = 0;
  void* host_base = nullptr;
  uint64_t live_count = 0;
  bool ready = false;
  bool oom_logged = false;
};

HeapState& state() {
  static HeapState s;
  return s;
}

}  // namespace

HostHeap& HostHeap::Get() {
  static HostHeap instance;
  return instance;
}

bool HostHeap::Init() {
  HeapState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.ready) {
    return true;
  }

  auto* kernel = rex::system::KernelState::shared();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("[native_gfx] HostHeap: no Memory instance");
    return false;
  }

  // The VIRTUAL 64 KB-page heap (v40000000, ~1008 MiB), NOT a physical window.
  //
  // The physical windows vA0000000 / vC0000000 / vE0000000 are all views of one
  // 512 MiB parent (Memory::Initialize) -- that parent IS the console's RAM, and
  // the game needs essentially all of it. Reserving 32 MiB of vA0000000 here
  // took it straight out of the game's budget: measured, the reservation
  // succeeded ("HostHeap ready: 32 MiB at guest 0xa54a0000") and five seconds
  // later the title died on "BaseHeap::Alloc failed to find contiguous range /
  // PhysicalHeap::Alloc unable to alloc physical memory in parent heap" while
  // still loading.
  //
  // Nothing about these objects needs physical memory. Only the PIXELS a
  // texture describes are addressed physically, through the fetch constant; the
  // header itself is only ever dereferenced by guest code. What it does need is
  // that a guest VA translates the same way for the runtime and for recompiled
  // code, and that holds for any address below 0xE0000000 -- the only range
  // PhysicalHostOffset shifts (rex/system/xmemory.h). AdoptInitTexture asserts
  // it per allocation anyway.
  rex::memory::BaseHeap* heap =
      memory->LookupHeapByType(/*physical=*/false, /*page_size=*/64u * 1024u);
  if (!heap) {
    REXLOG_ERROR("[native_gfx] HostHeap: LookupHeapByType(virtual, 64K) returned null");
    return false;
  }

  const uint32_t heap_size = HeapSizeFromCvar();
  uint32_t guest_va = 0;
  const bool ok = heap->Alloc(heap_size, 64u * 1024u,
                              rex::memory::kMemoryAllocationReserve |
                                  rex::memory::kMemoryAllocationCommit,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              /*top_down=*/true, &guest_va);
  if (!ok || !guest_va) {
    REXLOG_ERROR("[native_gfx] HostHeap: failed to reserve {} bytes in the virtual window",
                 heap_size);
    return false;
  }

  void* host_base = memory->TranslateVirtual<void*>(guest_va);
  O1HeapInstance* handle = o1heapInit(host_base, heap_size);
  if (!handle) {
    REXLOG_ERROR("[native_gfx] HostHeap: o1heapInit failed (size {})", heap_size);
    return false;
  }

  s.handle = handle;
  s.guest_va = guest_va;
  s.host_base = host_base;
  s.size = heap_size;
  s.ready = true;
  REXLOG_INFO("[native_gfx] HostHeap ready: {} KiB of GUEST VIRTUAL memory at {:#010x} "
              "(no console RAM taken)",
              heap_size >> 10, guest_va);
  return true;
}

bool HostHeap::ready() const {
  HeapState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.ready;
}

void* HostHeap::Alloc(std::size_t size, std::size_t alignment) {
  HeapState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready || size == 0) {
    return nullptr;
  }
  // o1heap always returns O1HEAP_ALIGNMENT (16) aligned blocks; anything
  // stricter would need a different allocator, so refuse rather than hand back
  // a misaligned pointer.
  if (alignment > O1HEAP_ALIGNMENT) {
    REXLOG_ERROR("[native_gfx] HostHeap: alignment {} exceeds o1heap's {}", alignment,
                 uint32_t(O1HEAP_ALIGNMENT));
    return nullptr;
  }
  void* p = o1heapAllocate(s.handle, size);
  if (!p) {
    if (!s.oom_logged) {
      s.oom_logged = true;
      const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
      REXLOG_ERROR(
          "[native_gfx] HostHeap exhausted: {} bytes requested, {} of {} allocated (peak {}). "
          "Raise mcla_native_gfx_own_heap_kb.",
          size, d.allocated, uint64_t(s.size), d.peak_allocated);
    }
    return nullptr;
  }
  ++s.live_count;
  return p;
}

void HostHeap::Free(void* host_ptr) {
  if (!host_ptr) {
    return;
  }
  HeapState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready) {
    return;
  }
  o1heapFree(s.handle, host_ptr);
  if (s.live_count) {
    --s.live_count;
  }
}

uint32_t HostHeap::AllocGuest(std::size_t size, std::size_t alignment) {
  void* p = Alloc(size, alignment);
  if (!p) {
    return 0;
  }
  auto* kernel = rex::system::KernelState::shared();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    Free(p);
    return 0;
  }
  return memory->HostToGuestVirtual(p);
}

void HostHeap::FreeGuest(uint32_t guest_va) {
  if (!guest_va) {
    return;
  }
  auto* kernel = rex::system::KernelState::shared();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    return;
  }
  Free(memory->TranslateVirtual<void*>(guest_va));
}

HostHeap::Snapshot HostHeap::GetSnapshot() {
  Snapshot out;
  HeapState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  out.ready = s.ready;
  if (!s.ready) {
    return out;
  }
  const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
  out.capacity = d.capacity;
  out.allocated = d.allocated;
  out.peak_allocated = d.peak_allocated;
  out.oom_count = d.oom_count;
  out.live_count = s.live_count;
  return out;
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
