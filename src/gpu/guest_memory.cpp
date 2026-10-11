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
