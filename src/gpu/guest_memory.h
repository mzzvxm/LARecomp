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
// one notification per arm. OnInvalidate only queues the range -- in the queue
// of every subscriber, so each cache that keeps copies (geometry, textures,
// resolve products, the census) sees every write whoever armed the page; the
// render thread takes its queue with Drain() before it resolves the resources
// of a record. The callback hands back exactly the range it was told about, so
// the SDK unprotects nothing beyond the written pages (anything more would stop
// notifying for a neighbouring watched range).
//
// A page the guest keeps read-only or inaccessible is not watched by the SDK
// (EnableAccessCallbacks neither protects nor flags it, and a later Protect
// that makes it writable only notifies flagged pages): IsWatchable tells a
// cache whether it may rely on the watch for a range.
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

  // A cache's own queue of written ranges.
  using Subscriber = uint32_t;
  static constexpr uint32_t kMaxSubscribers = 4;

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
  // The same pointer, for writing (the write-back of a resolve
  // product). It is the physical view, which the watch does not protect (the
  // SDK protects the guest's virtual views): a write through it notifies no
  // cache, so the caller only writes what the caches that hold the range
  // already have.
  uint8_t* WritablePhysical(uint32_t physical, uint32_t length) const;

  // Watches the pages of [physical, physical + length) until the next write.
  // False (logged once) when there is no memory system to watch with.
  bool Arm(uint32_t physical, uint32_t length);

  // True when every page of the range is committed read-write in the
  // physical heap alias that holds it, i.e. Arm really watches all of it.
  bool IsWatchable(uint32_t physical, uint32_t length);

  // Render thread: a queue that receives every range notified from now on --
  // not the ones before, so a cache subscribes before it arms anything (the
  // census subscribes on its first use). `name` is for the stats.
  // Every subscriber must drain regularly. kMaxSubscribers is a hard limit
  // (logged, and the caller gets no queue: it must not rely on the watch).
  static constexpr Subscriber kNoSubscriber = UINT32_MAX;
  Subscriber Subscribe(const char* name);

  // True when ranges were queued for `s` since its last Drain (one atomic load).
  bool HasInvalidations(Subscriber s) const {
    return s < kMaxSubscribers && queues_[s].flag.load(std::memory_order_acquire);
  }
  // Moves the ranges queued for `s` into `out` (appended).
  void Drain(Subscriber s, std::vector<Range>& out);

  // Since the previous call.
  Stats TakeStats();

 private:
  bool Attach();
  static std::pair<uint32_t, uint32_t> OnInvalidate(void* context, uint32_t start,
                                                    uint32_t length, bool exact_range);

  rex::memory::Memory* memory_ = nullptr;
  void* callback_ = nullptr;
  bool reported_no_memory_ = false;

  struct Queue {
    std::vector<Range> pending;  // guarded by mutex_
    std::atomic<bool> flag{false};
    const char* name = nullptr;
  };
  std::mutex mutex_;  // guards the queues' pending vectors and subscriber_count_
  Queue queues_[kMaxSubscribers];
  uint32_t subscriber_count_ = 0;
  std::atomic<uint64_t> notifications_{0};
  std::atomic<uint64_t> notified_bytes_{0};

  // Render thread.
  uint64_t arms_ = 0;
  uint64_t armed_bytes_ = 0;
};

}  // namespace mcla::gpu
