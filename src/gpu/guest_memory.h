#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest memory as the render thread sees it
// ===========================================================================
// The render thread reads what a record names -- vertex and index buffers,
// textures -- straight from guest memory, after the guest has moved on
//. Two things make that safe:
// the fences (24.20) keep the guest from reusing memory a pending record
// names, and the SDK's write watch tells the caches when memory they hold a
// copy of was written, so a copy is reused only while it is still exact.
//
// The watch: Arm() asks the SDK to write-protect the pages of a range. The
// next guest write to any of them -- or a file read into them, or the range
// being freed or made writable -- calls OnInvalidate on the writing thread,
// inside the memory system's global lock, and unprotects the touched pages:
// one notification per arm. OnInvalidate only queues the range; the render
// thread takes the queue with Drain() before it resolves the resources of a
// record. The callback hands back exactly the range it was told about, so the
// SDK unprotects nothing beyond the written pages (anything more would stop
// notifying for a neighbouring watched range).
//
// A page the guest keeps read-only or inaccessible is not watched by the SDK
// (EnableAccessCallbacks skips it); a cache must not rely on the watch for
// such a range (pending for M4.4).
//
// The renderer starts before the guest, when the runtime has no memory system
// yet, so the callback is registered the first time a range is armed.
//
// Threading: Arm, Read, Drain and the stats on the render thread; Shutdown on
// the lifecycle thread once the render thread stopped; the callback on any
// guest thread.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace mcla::gpu {

class GuestMemory {
 public:
  struct Range {
    uint32_t start = 0;  // physical
    uint32_t length = 0;
  };

  struct Stats {
    uint64_t arms = 0;
    uint64_t armed_bytes = 0;
    uint64_t notifications = 0;
    uint64_t notified_bytes = 0;
  };

  GuestMemory() = default;
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  // Unregisters the invalidation callback, if it was registered. Nothing may be
  // armed or drained afterwards.
  void Shutdown();

  // The guest's own conversion of a windowed address to a physical one (the draw builder sub_8241D620): a no-op on an address that is
  // already physical, plus one page for the 0xE0000000 window.
  static uint32_t ToPhysical(uint32_t address) {
    return (address & 0x1FFFFFFFu) + (((address >> 20) + 0x200u) & 0x1000u);
  }

  // Host pointer to `length` bytes at physical `address`, or null when any of
  // the range is not committed guest memory.
  const uint8_t* Read(uint32_t physical, uint32_t length) const;

  // Watches the pages of [physical, physical + length) until the next write.
  // False (logged once) when there is no memory system to watch with.
  bool Arm(uint32_t physical, uint32_t length);

  // True when ranges were queued since the last Drain (one atomic load).
  bool HasInvalidations() const { return pending_flag_.load(std::memory_order_acquire); }
  // Moves the queued ranges into `out` (appended).
  void Drain(std::vector<Range>& out);

  // Since the previous call.
  Stats TakeStats();

 private:
  bool Attach();
  static std::pair<uint32_t, uint32_t> OnInvalidate(void* context, uint32_t start,
                                                    uint32_t length, bool exact_range);

  rex::memory::Memory* memory_ = nullptr;
  void* callback_ = nullptr;
  bool reported_no_memory_ = false;

  std::mutex mutex_;  // guards pending_
  std::vector<Range> pending_;
  std::atomic<bool> pending_flag_{false};
  std::atomic<uint64_t> notifications_{0};
  std::atomic<uint64_t> notified_bytes_{0};

  // Render thread.
  uint64_t arms_ = 0;
  uint64_t armed_bytes_ = 0;
};

}  // namespace mcla::gpu
