// MCLA GPU renderer -- the command stream. See stream.h.
//
// Design followed: reeot gpu/render_thread.cpp (BSD-3-Clause) -- batched
// publication of the tail, a consumer that is woken only when it sleeps.
// Reimplemented over variable-size records.

#include "stream.h"

#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include <rex/logging.h>

namespace mcla::gpu {

namespace {

// How long the consumer polls an empty stream before it sleeps. During a frame
// the guest publishes a batch every few tens of microseconds; sleeping between
// two batches would make every batch a syscall on the guest thread.
constexpr std::chrono::microseconds kSpinBeforeSleep{200};

inline void CpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

}  // namespace

const char* RecordTypeName(RecordType type) {
  switch (type) {
    case RecordType::kPad: return "pad";
    case RecordType::kPresent: return "present";
    case RecordType::kDraw: return "draw";
    case RecordType::kClear: return "clear";
    case RecordType::kResolve: return "resolve";
    case RecordType::kTilingBegin: return "tiling begin";
    case RecordType::kTilingEnd: return "tiling end";
    case RecordType::kFence: return "fence";
    case RecordType::kLock: return "lock";
    case RecordType::kCount: break;
  }
  return "unknown";
}

bool CommandStream::Initialize(uint32_t capacity) {
  if (capacity < 4 * kRecordAlignment || capacity % kRecordAlignment != 0) {
    REXLOG_ERROR("[gpu] command stream: capacity {} is not a usable multiple of {}", capacity,
                 kRecordAlignment);
    return false;
  }
  // Not value-initialized: 64 MiB of zeros nobody reads.
  buffer_.reset(new uint8_t[capacity]);
  if (reinterpret_cast<uintptr_t>(buffer_.get()) % kRecordAlignment != 0) {
    REXLOG_ERROR("[gpu] command stream: buffer is not {}-byte aligned", kRecordAlignment);
    buffer_.reset();
    return false;
  }
  capacity_ = capacity;
  tail_ = 0;
  next_sequence_ = 1;
  unpublished_records_ = 0;
  unpublished_bytes_ = 0;
  published_tail_.store(0, std::memory_order_relaxed);
  head_.store(0, std::memory_order_relaxed);
  executed_sequence_.store(0, std::memory_order_relaxed);
  read_ = 0;
  released_ = 0;
  stop_.store(false, std::memory_order_release);
  return true;
}

void CommandStream::Stop() {
  stop_.store(true, std::memory_order_seq_cst);
  {
    std::lock_guard lock(wake_mutex_);
  }
  work_cv_.notify_all();
  space_cv_.notify_all();
  {
    std::lock_guard lock(done_mutex_);
  }
  done_cv_.notify_all();
}

// --- producer --------------------------------------------------------------

uint8_t* CommandStream::Writer::Begin(RecordType type, uint32_t payload_bytes) {
  CommandStream& s = stream_;
  if (begun_ || type == RecordType::kPad || !s.buffer_ || s.stopped()) {
    return nullptr;
  }
  if (payload_bytes > s.max_payload()) {
    REXLOG_ERROR("[gpu] command stream: a {} record of {} bytes exceeds the {}-byte limit; "
                 "the event is not captured",
                 RecordTypeName(type), payload_bytes, s.max_payload());
    return nullptr;
  }
  const uint32_t size = AlignRecord(uint32_t(sizeof(RecordHeader)) + payload_bytes);
  uint64_t at = s.tail_;
  const uint64_t offset = at % s.capacity_;
  const uint64_t pad = offset + size > s.capacity_ ? s.capacity_ - offset : 0;
  if (!s.WaitForSpace(lock_, at + pad + size)) {
    return nullptr;
  }
  if (pad != 0) {
    auto* filler = reinterpret_cast<RecordHeader*>(s.At(at));
    *filler = RecordHeader{};
    filler->size = uint32_t(pad);
    filler->type = RecordType::kPad;
    at += pad;
    s.tail_ = at;
    s.unpublished_bytes_ += pad;
  }
  auto* header = reinterpret_cast<RecordHeader*>(s.At(at));
  *header = RecordHeader{};
  header->size = size;
  header->type = type;
  record_at_ = at;
  record_size_ = size;
  type_ = type;
  begun_ = true;
  return reinterpret_cast<uint8_t*>(header + 1);
}

uint64_t CommandStream::Writer::Commit(Publication publication) {
  if (!begun_) {
    return 0;
  }
  begun_ = false;
  CommandStream& s = stream_;
  auto* header = reinterpret_cast<RecordHeader*>(s.At(record_at_));
  const uint64_t sequence = s.next_sequence_++;
  header->sequence = sequence;
  header->flags = publication == Publication::kWaited ? kRecordSync : 0;
  s.tail_ = record_at_ + record_size_;
  ++s.unpublished_records_;
  s.unpublished_bytes_ += record_size_;
  s.bytes_written_.fetch_add(record_size_, std::memory_order_relaxed);
  if (publication != Publication::kBatched || s.unpublished_records_ >= kPublishRecords ||
      s.unpublished_bytes_ >= kPublishBytes ||
      s.consumer_sleeping_.load(std::memory_order_seq_cst)) {
    s.Publish(s.tail_);
  }
  return sequence;
}

bool CommandStream::WaitForSpace(std::unique_lock<std::mutex>& producer_lock, uint64_t end) {
  (void)producer_lock;  // held by the caller for the whole wait: producers are serialized
  if (end - head_.load(std::memory_order_acquire) <= capacity_) {
    return !stopped();
  }
  // Everything written so far must be visible, or the consumer could never
  // free the space this producer waits for.
  Publish(tail_);
  producer_waits_.fetch_add(1, std::memory_order_relaxed);
  producer_waiting_.store(true, std::memory_order_seq_cst);
  {
    std::unique_lock lock(wake_mutex_);
    space_cv_.wait(lock, [&] {
      return stop_.load(std::memory_order_seq_cst) ||
             end - head_.load(std::memory_order_seq_cst) <= capacity_;
    });
  }
  producer_waiting_.store(false, std::memory_order_seq_cst);
  return !stopped();
}

void CommandStream::Publish(uint64_t tail) {
  unpublished_records_ = 0;
  unpublished_bytes_ = 0;
  published_tail_.store(tail, std::memory_order_seq_cst);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
    {
      std::lock_guard lock(wake_mutex_);
    }
    work_cv_.notify_one();
  }
}

void CommandStream::WaitExecuted(uint64_t sequence) {
  if (sequence == 0 || executed_sequence_.load(std::memory_order_acquire) >= sequence) {
    return;
  }
  std::unique_lock lock(done_mutex_);
  done_cv_.wait(lock, [&] {
    return stop_.load(std::memory_order_acquire) ||
           executed_sequence_.load(std::memory_order_acquire) >= sequence;
  });
}

// --- consumer --------------------------------------------------------------

const RecordHeader* CommandStream::Next() {
  for (;;) {
    const uint64_t published = published_tail_.load(std::memory_order_acquire);
    if (read_ == published) {
      return nullptr;
    }
    const auto* header = reinterpret_cast<const RecordHeader*>(At(read_));
    const uint64_t offset = read_ % capacity_;
    if (header->size < sizeof(RecordHeader) || header->size % kRecordAlignment != 0 ||
        offset + header->size > capacity_ || read_ + header->size > published) {
      // Only a bug in this file can get here. Stopping is the only safe answer:
      // every later record would be read from the wrong place.
      REXLOG_ERROR("[gpu] command stream: corrupt record at {} (size {}, type {}); stopping",
                   read_, header->size, static_cast<uint32_t>(header->type));
      Stop();
      return nullptr;
    }
    if (header->type == RecordType::kPad) {
      Release(header);
      continue;
    }
    return header;
  }
}

void CommandStream::Release(const RecordHeader* record) {
  read_ += record->size;
  // head_ bounds the producers' free space and they read it at every record:
  // stored at every record, its cache line moved between the two cores twice
  // a record. It is stored once kReleaseBytes went by,
  // at once when a producer waits for space (it then waits on the consumer,
  // which is behind and still has records to run), and before the consumer
  // waits for work (WaitForWork). Space runs out only with the consumer far
  // behind, never within kReleaseBytes of it.
  if (read_ - released_ >= kReleaseBytes || producer_waiting_.load(std::memory_order_seq_cst)) {
    ReleaseNow();
  }
}

void CommandStream::ReleaseNow() {
  released_ = read_;
  head_.store(read_, std::memory_order_seq_cst);
  if (producer_waiting_.load(std::memory_order_seq_cst)) {
    {
      std::lock_guard lock(wake_mutex_);
    }
    space_cv_.notify_all();
  }
}

bool CommandStream::WaitForWork(std::chrono::milliseconds timeout) {
  if (released_ != read_) {
    ReleaseNow();
  }
  const auto spin_until = std::chrono::steady_clock::now() + kSpinBeforeSleep;
  for (;;) {
    for (int i = 0; i < 64; ++i) {
      if (stopped() || published_tail_.load(std::memory_order_acquire) != read_) {
        return true;
      }
      CpuRelax();
    }
    if (std::chrono::steady_clock::now() >= spin_until) {
      break;
    }
  }
  consumer_sleeping_.store(true, std::memory_order_seq_cst);
  bool work;
  {
    std::unique_lock lock(wake_mutex_);
    work = work_cv_.wait_for(lock, timeout, [&] {
      return stop_.load(std::memory_order_seq_cst) ||
             published_tail_.load(std::memory_order_seq_cst) != read_;
    });
  }
  consumer_sleeping_.store(false, std::memory_order_seq_cst);
  if (work) {
    consumer_wakes_.fetch_add(1, std::memory_order_relaxed);
  }
  return work;
}

void CommandStream::MarkExecuted(uint64_t sequence) {
  executed_sequence_.store(sequence, std::memory_order_release);
  {
    std::lock_guard lock(done_mutex_);
  }
  done_cv_.notify_all();
}

}  // namespace mcla::gpu
