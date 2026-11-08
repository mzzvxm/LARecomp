#pragma once
// ===========================================================================
// MCLA GPU renderer -- the command stream
// ===========================================================================
// A byte ring of variable-size records (records.h) between the guest threads
// that capture D3D calls and the render thread that executes them. Replaces
// M1/M2's fixed-slot ring, which cannot carry thousands of draws a frame of
// variable size.
//
// Producer: any guest thread, serialized by the producer lock for the whole
// write (Writer holds it from its construction to its destruction), so a record
// is written in place in the ring with no copy. A record that would cross the
// end of the buffer is preceded by a kPad record and starts again at offset 0.
// The producer blocks only when the ring is full. GuestCapture builds its
// Writer first in every entry point and relies on the lock for its own state
// too, so a draw takes one lock, not two.
//
// Publication is batched (reeot render_thread.cpp publishes every 8 commands
// or at once while its consumer sleeps): the tail the consumer sees moves every
// kPublishRecords records or kPublishBytes bytes, at once when the consumer is
// asleep, and always on a record committed with Publication::kNow or kWaited
// (a fence, the present). Waking the consumer is a syscall, publishing is one
// atomic store.
//
// Consumer: the render thread. It reads up to the published tail, spins
// briefly when it is empty, then sleeps on a condition with the idle period as
// timeout. Space is returned record by record.
//
// Ordering: record bytes are written before the release store of the
// published tail and read after its acquire load. The sleeping/waiting flags
// and the positions they guard are sequentially consistent on both sides
// (Dekker), so neither side can miss the other's wake-up.
//
// Threading: producer side from any thread; consumer side (Next, Release,
// WaitForWork, MarkExecuted) from the render thread only.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "records.h"

namespace mcla::gpu {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the cache-line alignment below, on purpose
#endif

class CommandStream {
 public:
  static constexpr uint32_t kDefaultCapacity = 64u << 20;
  static constexpr uint32_t kPublishRecords = 32;
  static constexpr uint32_t kPublishBytes = 256u << 10;
  // While a sleeping consumer has been woken and has not run yet, the records that follow are
  // published this many at a time (it reads whatever is published when it runs).
  static constexpr uint32_t kWakingPublishRecords = 8;
  // The consumer gives space back once this much went by (Release).
  static constexpr uint32_t kReleaseBytes = 64u << 10;

  CommandStream() = default;
  CommandStream(const CommandStream&) = delete;
  CommandStream& operator=(const CommandStream&) = delete;

  // `capacity` is a multiple of kRecordAlignment.
  bool Initialize(uint32_t capacity = kDefaultCapacity);
  // Releases every producer and waiter; later writes fail.
  void Stop();
  bool stopped() const { return stop_.load(std::memory_order_acquire); }

  // Largest payload one record can carry (a quarter of the ring: a record that
  // big would starve every other record anyway).
  uint32_t max_payload() const { return capacity_ / 4 - sizeof(RecordHeader); }

  // The producers' lock. One thread (the guest's render thread) takes it for
  // about a microsecond at every draw and the others rarely, so it is a test and
  // set lock: taking it is one locked exchange and releasing it a plain store
  // (a std::mutex release is a second locked instruction, which also waits for
  // the stores of the record just written to reach the cache). A thread that
  // finds it taken spins on plain loads, then gives its core away, then sleeps
  // a millisecond at a time: the holder can be blocked in WaitForSpace for as
  // long as the render thread is behind.
  class ProducerLock {
   public:
    void lock() {
      if (!held_.exchange(true, std::memory_order_acquire)) {
        return;
      }
      LockSlow();
    }
    bool try_lock() {
      return !held_.load(std::memory_order_relaxed) &&
             !held_.exchange(true, std::memory_order_acquire);
    }
    void unlock() { held_.store(false, std::memory_order_release); }

   private:
    void LockSlow();
    std::atomic<bool> held_{false};
  };

  // One record being written. Holds the producer lock from its construction to
  // its destruction, with or without a record begun, and can write several
  // records one after the other (Begin again after Commit). Destruction without
  // Commit abandons the space: nothing is published, the next record reuses it.
  class Writer {
   public:
    explicit Writer(CommandStream& stream) : stream_(stream), lock_(stream.producer_lock_) {}
    ~Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Reserves room for `payload_bytes` and returns where to write them, or
    // null when the stream is stopped or the record is too large (logged).
    uint8_t* Begin(RecordType type, uint32_t payload_bytes);
    // Makes the record visible per `publication` (records.h). Returns the
    // record's sequence, 0 if nothing was begun.
    uint64_t Commit(Publication publication);

   private:
    CommandStream& stream_;
    std::unique_lock<ProducerLock> lock_;
    uint64_t record_at_ = 0;  // ring position of the header
    uint64_t record_offset_ = 0;  // the same position within the buffer (record_at_ % capacity)
    uint32_t record_size_ = 0;
    RecordType type_ = RecordType::kPad;
    bool begun_ = false;
  };

  // Blocks until the record `sequence` was executed, or the stream stopped. Polls for about 100 us
  // before it sleeps, except with 5 or fewer logical processors (stream.cpp). The present is the
  // record it waits for, once a frame, and its release point is a few tens of microseconds away.
  void WaitExecuted(uint64_t sequence);

  // --- render thread ---
  // The next published record (never kPad), or null when there is none.
  const RecordHeader* Next();
  // Gives the space of the record returned by Next back to the producers.
  void Release(const RecordHeader* record);
  // Looking ahead without consuming (the renderer decodes the textures of the draws queued behind
  // the one it executes): the ring position of the record Next last returned (until Release moves
  // it on); and the published record at `*position`, which then moves past it. Null at the end of
  // the published records. Pads are skipped. A record Peek returned stays readable until the
  // consumer releases it, which only the render thread does, so the pointer is good for the rest
  // of the record being executed. Render thread only.
  uint64_t consumer_position() const { return read_; }
  const RecordHeader* Peek(uint64_t* position) const;
  // Spins briefly, then sleeps until a record is published, the stream stops,
  // or `timeout` passes. True when there is work (or stop), false on timeout.
  bool WaitForWork(std::chrono::milliseconds timeout);
  // Wakes WaitExecuted callers of every sequence up to `sequence`.
  void MarkExecuted(uint64_t sequence);

  // Counters (render thread reads them for its stats; approximate across threads).
  uint64_t consumer_wakes() const { return consumer_wakes_.load(std::memory_order_relaxed); }
  uint64_t producer_waits() const { return producer_waits_.load(std::memory_order_relaxed); }
  uint64_t bytes_written() const { return bytes_written_.load(std::memory_order_relaxed); }

 private:
  bool WaitForSpace(std::unique_lock<ProducerLock>& producer_lock, uint64_t end);
  void Publish(uint64_t tail);
  // Stores head_ = read_ and wakes a producer waiting for space.
  void ReleaseNow();

  // The members are grouped by who writes them, each group on its own cache
  // line. The producer writes its cursors at every record while the consumer
  // polls published_tail_ in its idle spin and the producer reads head_ at
  // every record; with all of them on one or two lines each record moved the
  // line between the two cores.
  static constexpr size_t kCacheLine = 64;

  // Read only after Initialize.
  alignas(kCacheLine) std::unique_ptr<uint8_t[]> buffer_;
  uint32_t capacity_ = 0;

  // Producer state, guarded by producer_lock_.
  alignas(kCacheLine) ProducerLock producer_lock_;
  uint64_t tail_ = 0;  // next free byte
  // tail_ % capacity_, kept beside it: the producer needs the offset at every record, and the
  // remainder of a 64-bit division by a value only known at run time was three divisions a draw.
  uint64_t tail_offset_ = 0;
  uint64_t next_sequence_ = 1;
  uint32_t unpublished_records_ = 0;
  uint64_t unpublished_bytes_ = 0;
  // A Publish found the consumer asleep and woke it, and no later look found it awake. While set,
  // the next records do not wake it again (Commit).
  bool consumer_woken_ = false;
  std::atomic<uint64_t> bytes_written_{0};  // stored under producer_lock_ only
  std::atomic<uint64_t> producer_waits_{0};

  // Written by the producer once per batch (or rarely), polled by the consumer.
  alignas(kCacheLine) std::atomic<uint64_t> published_tail_{0};
  std::atomic<bool> stop_{false};
  // Sleeping consumer / waiting producer (Dekker with the positions).
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<bool> producer_waiting_{false};

  // Written by the consumer every kReleaseBytes, read by the producer.
  alignas(kCacheLine) std::atomic<uint64_t> head_{0};  // consumer: first byte not yet released

  // Consumer-side cursor (render thread only), and where head_ was last
  // stored (Release).
  alignas(kCacheLine) uint64_t read_ = 0;
  uint64_t read_offset_ = 0;  // read_ % capacity_, kept beside it (Next and Release)
  uint64_t released_ = 0;
  std::atomic<uint64_t> consumer_wakes_{0};
  // Presents waiting for execution: stored by the consumer, read by the waiter.
  std::atomic<uint64_t> executed_sequence_{0};

  // Waiting and waking (cold).
  alignas(kCacheLine) std::mutex wake_mutex_;
  std::condition_variable work_cv_;
  std::condition_variable space_cv_;
  std::mutex done_mutex_;
  std::condition_variable done_cv_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace mcla::gpu
