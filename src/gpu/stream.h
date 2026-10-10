#pragma once
// ===========================================================================
// MCLA GPU renderer -- the command stream
// ===========================================================================
// A byte ring of variable-size records (records.h) between the guest threads
// that capture D3D calls and the render thread that executes them. Replaces
// M1/M2's fixed-slot ring, which cannot carry thousands of draws a frame of
// variable size.
//
// Producer: any guest thread, serialized by the producer mutex for the whole
// write (Writer holds it from Begin to Commit), so a record is written in place
// in the ring with no copy. A record that would cross the end of the buffer is
// preceded by a kPad record and starts again at offset 0. The producer blocks
// only when the ring is full.
//
// Publication is batched (reeot render_thread.cpp publishes every 8 commands
// or at once while its consumer sleeps): the tail the consumer sees moves every
// kPublishRecords records or kPublishBytes bytes, at once when the consumer is
// asleep, and always on a sync record (the present). Waking the consumer is a
// syscall, publishing is one atomic store.
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
#include <cstdint>
#include <memory>
#include <mutex>

#include "records.h"

namespace mcla::gpu {

class CommandStream {
 public:
  static constexpr uint32_t kDefaultCapacity = 64u << 20;
  static constexpr uint32_t kPublishRecords = 32;
  static constexpr uint32_t kPublishBytes = 256u << 10;

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

  // One record being written. Holds the producer mutex until Commit or
  // destruction (destruction without Commit abandons the space: nothing is
  // published, the next record reuses it).
  class Writer {
   public:
    explicit Writer(CommandStream& stream) : stream_(stream), lock_(stream.producer_mutex_) {}
    ~Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Reserves room for `payload_bytes` and returns where to write them, or
    // null when the stream is stopped or the record is too large (logged).
    uint8_t* Begin(RecordType type, uint32_t payload_bytes);
    // Publishes per the batching rules (always when `sync`). Returns the
    // record's sequence, 0 if nothing was begun.
    uint64_t Commit(bool sync);

   private:
    CommandStream& stream_;
    std::unique_lock<std::mutex> lock_;
    uint64_t record_at_ = 0;  // ring position of the header
    uint32_t record_size_ = 0;
    RecordType type_ = RecordType::kPad;
    bool begun_ = false;
  };

  // Blocks until the record `sequence` was executed, or the stream stopped.
  void WaitExecuted(uint64_t sequence);

  // --- render thread ---
  // The next published record (never kPad), or null when there is none.
  const RecordHeader* Next();
  // Gives the space of the record returned by Next back to the producers.
  void Release(const RecordHeader* record);
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
  uint8_t* At(uint64_t position) const { return buffer_.get() + (position % capacity_); }
  bool WaitForSpace(std::unique_lock<std::mutex>& producer_lock, uint64_t end);
  void Publish(uint64_t tail);

  std::unique_ptr<uint8_t[]> buffer_;
  uint32_t capacity_ = 0;

  // Producer state, guarded by producer_mutex_.
  std::mutex producer_mutex_;
  uint64_t tail_ = 0;  // next free byte
  uint64_t next_sequence_ = 1;
  uint32_t unpublished_records_ = 0;
  uint64_t unpublished_bytes_ = 0;

  std::atomic<uint64_t> published_tail_{0};
  std::atomic<uint64_t> head_{0};  // consumer: first byte not yet released
  std::atomic<bool> stop_{false};

  // Sleeping consumer / waiting producer (Dekker with the positions).
  std::mutex wake_mutex_;
  std::condition_variable work_cv_;
  std::condition_variable space_cv_;
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<bool> producer_waiting_{false};

  // Presents waiting for execution.
  std::mutex done_mutex_;
  std::condition_variable done_cv_;
  std::atomic<uint64_t> executed_sequence_{0};

  // Consumer-side cursor (render thread only).
  uint64_t read_ = 0;

  std::atomic<uint64_t> consumer_wakes_{0};
  std::atomic<uint64_t> producer_waits_{0};
  std::atomic<uint64_t> bytes_written_{0};
};

}  // namespace mcla::gpu
