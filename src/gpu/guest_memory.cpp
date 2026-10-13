// MCLA GPU renderer -- guest memory as the render thread sees it. See
// guest_memory.h.

#include "guest_memory.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "native_gfx/guest/guest_resources.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

GuestMemory::~GuestMemory() {
  Shutdown();
}

bool GuestMemory::Attach() {
  if (memory_) {
    return true;
  }
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    if (!reported_no_memory_) {
      reported_no_memory_ = true;
      REXLOG_ERROR("[gpu] guest memory: no memory system to watch with; nothing is armed");
    }
    return false;
  }
  callback_ = memory->RegisterPhysicalMemoryInvalidationCallback(&OnInvalidate, this);
  memory_ = memory;
  return true;
}

void GuestMemory::Shutdown() {
  if (memory_ && callback_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(callback_);
  }
  callback_ = nullptr;
  memory_ = nullptr;
  std::lock_guard lock(mutex_);
  pending_.clear();
  pending_flag_.store(false, std::memory_order_release);
}

const uint8_t* GuestMemory::Read(uint32_t physical, uint32_t length) const {
  if (length == 0 || !nfx::IsPhysicalRangeReadable(physical, length)) {
    return nullptr;
  }
  return nfx::TranslatePhysicalGuest(physical);
}

bool GuestMemory::Arm(uint32_t physical, uint32_t length) {
  if (length == 0 || !Attach()) {
    return false;
  }
  memory_->EnablePhysicalMemoryAccessCallbacks(physical, length,
                                               /*enable_invalidation_notifications=*/true,
                                               /*enable_data_providers=*/false);
  ++arms_;
  armed_bytes_ += length;
  return true;
}

bool GuestMemory::IsWatchable(uint32_t physical, uint32_t length) {
  if (length == 0 || !Attach()) {
    return false;
  }
  const uint64_t end = uint64_t(physical) + length;
  if (end > 0x20000000ull) {
    return false;
  }
  // The three physical aliases each keep their own page table (the same walk
  // as nfx::IsPhysicalRangeReadable): 0xE0000000 is one page off (virtual
  // 0xE0000000 + v is physical v + 0x1000), 0xC0000000 and 0xA0000000 are not.
  struct Alias {
    uint32_t base;
    uint32_t page_offset;
  };
  static constexpr Alias kAliases[] = {{0xE0000000u, 0x1000u}, {0xC0000000u, 0}, {0xA0000000u, 0}};
  const uint32_t last = uint32_t(end - 1);
  for (const Alias& alias : kAliases) {
    if (physical < alias.page_offset) {
      continue;
    }
    const uint32_t low = alias.base + (physical - alias.page_offset);
    const uint32_t high = alias.base + (last - alias.page_offset);
    rex::memory::BaseHeap* heap = memory_->LookupHeap(low);
    if (!heap || memory_->LookupHeap(high) != heap) {
      continue;
    }
    switch (heap->QueryRangeAccess(low, high)) {
      case rex::memory::PageAccess::kReadWrite:
        return true;
      case rex::memory::PageAccess::kReadOnly:
        return false;  // the SDK will not watch these pages
      default:
        break;  // not committed in this alias
    }
  }
  return false;
}

void GuestMemory::Drain(std::vector<Range>& out) {
  if (!pending_flag_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lock(mutex_);
  out.insert(out.end(), pending_.begin(), pending_.end());
  pending_.clear();
  pending_flag_.store(false, std::memory_order_release);
}

GuestMemory::Stats GuestMemory::TakeStats() {
  Stats stats;
  stats.arms = arms_;
  stats.armed_bytes = armed_bytes_;
  stats.notifications = notifications_.exchange(0, std::memory_order_relaxed);
  stats.notified_bytes = notified_bytes_.exchange(0, std::memory_order_relaxed);
  arms_ = 0;
  armed_bytes_ = 0;
  return stats;
}

// On the writing thread, inside the memory system's global lock: queue and go.
std::pair<uint32_t, uint32_t> GuestMemory::OnInvalidate(void* context, uint32_t start,
                                                        uint32_t length, bool /*exact_range*/) {
  auto* self = static_cast<GuestMemory*>(context);
  {
    std::lock_guard lock(self->mutex_);
    self->pending_.push_back(Range{start, length});
    self->pending_flag_.store(true, std::memory_order_release);
  }
  self->notifications_.fetch_add(1, std::memory_order_relaxed);
  self->notified_bytes_.fetch_add(length, std::memory_order_relaxed);
  // Exactly the notified range: nothing beyond the written pages is unwatched.
  return {start, length};
}

}  // namespace mcla::gpu
