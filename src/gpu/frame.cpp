// MCLA GPU renderer -- frames in flight and retirement. See frame.h.

#include "frame.h"

#include <algorithm>

#include <rex/logging.h>

#include "device.h"

namespace mcla::gpu {

FrameRing::~FrameRing() {
  Shutdown();
}

bool FrameRing::Initialize(Device& device) {
  if (device_) {
    return true;
  }
  plume::RenderDevice* dev = device.device();
  plume::RenderCommandQueue* queue = device.queue();
  if (!dev || !queue) {
    REXLOG_ERROR("[gpu] frame ring: no device");
    return false;
  }
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("[gpu] frame ring: could not create {}", what);
    for (Slot& t : slots_) {
      t = Slot{};
    }
    idle_list_.reset();
    idle_fence_.reset();
    return false;
  };
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    Slot& s = slots_[i];
    // Each list owns its allocator/pool, so a slot's begin() resets only its
    // own memory, and only after Advance() waited for the slot's fence. More
    // lists are created when a frame flushes batches (OpenNextList).
    s.lists.push_back(queue->createCommandList());
    s.fence = dev->createCommandFence();
    s.acquire = dev->createCommandSemaphore();
    if (!s.lists.back() || !s.fence || !s.acquire) {
      return fail("the objects of a frame slot");
    }
  }
  idle_list_ = queue->createCommandList();
  idle_fence_ = dev->createCommandFence();
  if (!idle_list_ || !idle_fence_) {
    return fail("the idle list and fence");
  }
  device_ = &device;
  slot_ = 0;
  open_ = false;
  return true;
}

void FrameRing::Shutdown() {
  if (!device_) {
    return;
  }
  if (open_) {
    Abandon();
  }
  WaitIdle();
  retired_.clear();
  for (Slot& s : slots_) {
    s = Slot{};
  }
  idle_list_.reset();
  idle_fence_.reset();
  device_ = nullptr;
}

plume::RenderCommandList* FrameRing::OpenNextList() {
  Slot& s = slots_[slot_];
  if (s.next_list == s.lists.size()) {
    std::unique_ptr<plume::RenderCommandList> list = device_->queue()->createCommandList();
    if (!list) {
      REXLOG_ERROR("[gpu] frame ring: could not create batch list {} of slot {}", s.next_list,
                   slot_);
      return nullptr;
    }
    s.lists.push_back(std::move(list));
  }
  plume::RenderCommandList* list = s.lists[s.next_list].get();
  list->begin();
  ++s.next_list;
  open_ = true;
  return list;
}

plume::RenderCommandList* FrameRing::Begin() {
  if (!device_ || device_->IsLost()) {
    return nullptr;
  }
  if (open_) {
    return list();
  }
  slots_[slot_].next_list = 0;
  return OpenNextList();
}

plume::RenderCommandList* FrameRing::FlushBatch() {
  if (!open_) {
    return nullptr;
  }
  if (device_->CheckLost("batch flush")) {
    Abandon();
    return nullptr;
  }
  Slot& s = slots_[slot_];
  plume::RenderCommandList* current = s.lists[s.next_list - 1].get();
  current->end();
  open_ = false;
  const plume::RenderCommandList* lists[] = {current};
  device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, nullptr);
  ++batch_flushes_;
  return OpenNextList();
}

bool FrameRing::Submit(plume::RenderCommandSemaphore** waits, uint32_t wait_count,
                       plume::RenderCommandSemaphore** signals, uint32_t signal_count) {
  if (!open_) {
    return false;
  }
  Slot& s = slots_[slot_];
  if (device_->CheckLost("frame submit")) {
    Abandon();
    return false;
  }
  plume::RenderCommandList* current = s.lists[s.next_list - 1].get();
  current->end();
  open_ = false;
  const plume::RenderCommandList* lists[] = {current};
  device_->queue()->executeCommandLists(lists, 1, waits, wait_count, signals, signal_count,
                                        s.fence.get());
  s.submitted = true;
  s.serial = next_serial_++;
  return true;
}

void FrameRing::Abandon() {
  if (!open_) {
    return;
  }
  Slot& s = slots_[slot_];
  s.lists[s.next_list - 1]->end();
  open_ = false;
  if (s.next_list > 1 && !device_->IsLost()) {
    // Batches of this frame already run without a fence: nothing may reset
    // their lists before they are done. Rare (a frame cut short), so the
    // simplest proof is enough: wait for the queue to drain.
    WaitIdle();
  }
}

void FrameRing::WaitSlot(Slot& slot) {
  if (!slot.submitted) {
    return;
  }
  // A removed device never signals: waiting would hang the render thread
  // forever (reblue and reeot skip the drain for the same reason).
  if (!device_->CheckLost("fence wait")) {
    device_->queue()->waitForCommandFence(slot.fence.get());
    completed_serial_ = std::max(completed_serial_, slot.serial);
  }
  slot.submitted = false;
}

void FrameRing::Advance() {
  if (!device_) {
    return;
  }
  if (open_) {
    // A list left open across a frame boundary would be reset under its own
    // commands by the slot's next Begin(). Nothing reaches here legitimately.
    REXLOG_ERROR("[gpu] frame ring: advancing past an open command list; abandoning it");
    Abandon();
  }
  slot_ = (slot_ + 1) % kFramesInFlight;
  WaitSlot(slots_[slot_]);
  ReleaseCompleted();
}

void FrameRing::WaitIdle() {
  if (!device_) {
    return;
  }
  if (!device_->CheckLost("idle wait")) {
    // A slot fence covers its command lists, not what the queue runs after
    // them: the swap chain's present (and on D3D12 the wait on the render
    // semaphore before it) is queued after the frame's fence. Releasing back
    // buffers on the strength of the frame fences alone is what the D3D12
    // debug layer caught as CORRUPTION ("referenced by GPU operations
    // in-flight", 06/10, swap chain resize). An empty submission with a fence
    // of its own, after everything queued so far, closes that gap whether or
    // not a frame's list is open; waiting for it proves the queue idle.
    idle_list_->begin();
    idle_list_->end();
    const plume::RenderCommandList* lists[] = {idle_list_.get()};
    device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, idle_fence_.get());
    device_->queue()->waitForCommandFence(idle_fence_.get());
    // Every serial handed out so far belongs to a submission before the idle
    // one, so all of them are complete.
    completed_serial_ = next_serial_ - 1;
  }
  for (Slot& s : slots_) {
    s.submitted = false;
  }
  if (device_->IsLost() || !open_) {
    // Every submission is complete (or, with the device lost, will never run)
    // and no recorded-but-unsubmitted commands exist, so nothing retired can be
    // referenced -- including objects tagged next_serial_, which no submission
    // carries yet.
    retired_.clear();
    return;
  }
  // A list is open: what it already recorded may name objects retired after
  // the recording started. Those carry next_serial_ and wait for its submission.
  ReleaseCompleted();
}

void FrameRing::ReleaseCompleted() {
  // Retirements are appended with a non-decreasing serial, so the completed
  // ones are a prefix.
  while (!retired_.empty() && retired_.front().serial <= completed_serial_) {
    retired_.pop_front();
  }
}

}  // namespace mcla::gpu
