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
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    Slot& s = slots_[i];
    // Each list owns its allocator/pool, so a slot's begin() resets only its
    // own memory, and only after Advance() waited for the slot's fence.
    s.list = queue->createCommandList();
    s.fence = dev->createCommandFence();
    s.acquire = dev->createCommandSemaphore();
    if (!s.list || !s.fence || !s.acquire) {
      REXLOG_ERROR("[gpu] frame ring: could not create the objects of slot {}", i);
      for (Slot& t : slots_) {
        t = Slot{};
      }
      return false;
    }
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
  device_ = nullptr;
}

plume::RenderCommandList* FrameRing::Begin() {
  if (!device_ || device_->IsLost()) {
    return nullptr;
  }
  Slot& s = slots_[slot_];
  if (!open_) {
    s.list->begin();
    open_ = true;
  }
  return s.list.get();
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
  s.list->end();
  open_ = false;
  const plume::RenderCommandList* lists[] = {s.list.get()};
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
  slots_[slot_].list->end();
  open_ = false;
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
  if (!open_ && !device_->IsLost()) {
    // A slot fence covers its command list, not what the queue runs after it:
    // the swap chain's present (and on D3D12 the wait on the render semaphore
    // before it) is queued after the frame's fence. Releasing back buffers on
    // the strength of the frame fences alone is what the D3D12 debug layer
    // caught as CORRUPTION ("referenced by GPU operations in-flight", 06/10,
    // swap chain resize). One empty submission, fenced, after everything
    // queued so far closes that gap; waiting for it below proves the queue
    // idle.
    SubmitQueueFence();
  }
  // Wait in submission order so the completed serial only ever moves forward
  // to a value whose predecessors are all done.
  uint32_t order[kFramesInFlight];
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    order[i] = i;
  }
  std::sort(order, order + kFramesInFlight,
            [&](uint32_t a, uint32_t b) { return slots_[a].serial < slots_[b].serial; });
  for (uint32_t i : order) {
    WaitSlot(slots_[i]);
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
  completed_serial_ = next_serial_ - 1;
  ReleaseCompleted();
}

void FrameRing::SubmitQueueFence() {
  // The current slot's list is reused: its own fence is waited first, so the
  // reset inside begin() cannot touch commands the GPU still runs.
  Slot& s = slots_[slot_];
  WaitSlot(s);
  if (device_->IsLost()) {
    return;
  }
  s.list->begin();
  s.list->end();
  const plume::RenderCommandList* lists[] = {s.list.get()};
  device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fence.get());
  s.submitted = true;
  s.serial = next_serial_++;
}

void FrameRing::ReleaseCompleted() {
  // Retirements are appended with a non-decreasing serial, so the completed
  // ones are a prefix.
  while (!retired_.empty() && retired_.front().serial <= completed_serial_) {
    retired_.pop_front();
  }
}

}  // namespace mcla::gpu
