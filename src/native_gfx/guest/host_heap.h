#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest-visible allocator for host objects
// ===========================================================================
// When the native runtime starts creating D3D resources on the game's behalf,
// the object it hands back has to live at an address the guest can hold in a
// pointer and dereference: the engine stores it, passes it to other D3D calls,
// and reads its header fields directly (D3DResource::Common, ReferenceCount).
//
// So the allocation has to come out of GUEST memory, not the host heap. This
// reserves one block in the guest VIRTUAL window (v40000000) and runs a single
// o1heap over it.
//
// Virtual, not physical, and that distinction is the whole design. The
// physical windows vA0000000 / vC0000000 / vE0000000 are three views of ONE
// 512 MiB parent heap, and that parent is the console's RAM -- which the game
// needs all of. An earlier version took 32 MiB of vA0000000 and the title died
// while still loading, on the game's own allocation failing. Nothing here
// needs to be physical: only the pixels a texture describes are addressed
// physically, through the fetch constant, while the header itself is only ever
// dereferenced by guest code. Below 0xE0000000 a guest VA translates
// identically for the runtime and for recompiled code, which is all that is
// required.
//
// o1heap rather than the runtime's own BaseHeap::Alloc because these are many
// small structs with a create/destroy churn: BaseHeap does not coalesce on
// free and fragments into "failed to find contiguous range". o1heap does.
// ===========================================================================

#include <cstddef>
#include <cstdint>

namespace mcla::native_gfx {

class HostHeap {
 public:
  static HostHeap& Get();

  // Reserves the guest block and initializes the heap. Call once during
  // native runtime bring-up, before the first allocation. Idempotent.
  // Returns false (and logs) on failure; the caller must treat that as fatal
  // for the resource-owning path.
  bool Init();

  bool ready() const;

  // 16-byte-aligned host pointer into the guest block, or nullptr when
  // exhausted (logged once). `alignment` must be <= 16.
  void* Alloc(std::size_t size, std::size_t alignment);
  void Free(void* host_ptr);

  // Same allocation, addressed as a guest VA. For call sites that hand the
  // address straight to the game rather than keeping a host pointer.
  // Returns 0 on failure.
  uint32_t AllocGuest(std::size_t size, std::size_t alignment);
  void FreeGuest(uint32_t guest_va);

  struct Snapshot {
    std::size_t capacity = 0;
    std::size_t allocated = 0;
    std::size_t peak_allocated = 0;
    uint64_t oom_count = 0;
    uint64_t live_count = 0;
    bool ready = false;
  };
  Snapshot GetSnapshot();
};

}  // namespace mcla::native_gfx
