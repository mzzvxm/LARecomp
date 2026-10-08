#pragma once
// ===========================================================================
// MCLA GPU renderer -- frames in flight and resource retirement
// ===========================================================================
// kFramesInFlight slots, each with its own command list, fence and swap-chain
// acquire semaphore. While the GPU executes slot N the render thread records
// slot N+1; Advance() moves to the next slot and waits for the fence of the
// submission that last used it before its list is reset. Two slots is what
// reblue and reeot both run (gpu/device.h kNumFrames, gpu/device.h kNumFrames):
// one more lets the CPU run further ahead without hiding anything new.
//
// Plume's RenderCommandFence is binary (plume_d3d12.cpp: an auto-reset event
// armed by executeCommandLists; plume_vulkan.cpp: vkWaitForFences then
// vkResetFences). Waiting on a fence that was never submitted blocks forever,
// so each slot records whether it has a submission outstanding, and a slot is
// submitted at most once between two waits.
//
// Retirement. A GPU object that is no longer wanted may still be referenced by
// a command list the GPU has not finished. Retire() takes it over and tags it
// with the serial of the submission that will close the list being recorded;
// it is destroyed once a fence wait proves that serial complete. On one queue
// submissions complete in order, so "serial S done" covers every earlier use.
// This holds for any slot count and for a list that is abandoned instead of
// submitted (its serial passes to the next submission, which completes later
// than any use recorded before it). reblue and reeot instead park objects in
// per-slot graveyards keyed by the recording slot, which is correct for two
// slots and needs reblue's RetireSlot guard because several guest threads
// retire concurrently; here only the render thread records or retires.
//
// RetireAction defers work rather than an object -- rewriting a bindless slot
// to its null descriptor and freeing it (descriptors.h) -- under the same rule.
// Retirements, objects and actions alike, are released in the order they were
// retired. Whoever retires an action must outlive the ring's pending work: the
// renderer drains the ring (WaitIdle) before it shuts any component down.
//
// Threading: render thread only, after Initialize. Not internally locked.
// ===========================================================================

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;

inline constexpr uint32_t kFramesInFlight = 2;

class FrameRing {
 public:
  FrameRing() = default;
  ~FrameRing();
  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;

  bool Initialize(Device& device);
  // Waits for every outstanding submission (unless the device is lost), then
  // destroys every retired object, the lists, fences and semaphores.
  void Shutdown();

  // Opens the current slot's command list if it is not open yet. Null when the
  // device is lost or the ring is not initialized.
  plume::RenderCommandList* Begin();
  bool is_open() const { return open_; }
  plume::RenderCommandList* list() const { return open_ ? slots_[slot_].list.get() : nullptr; }
  uint32_t slot() const { return slot_; }

  // The semaphore a swap-chain acquire for this slot signals, and its submission
  // waits on. One per slot: a binary semaphore cannot be re-signalled while a
  // wait on it is still pending.
  plume::RenderCommandSemaphore* acquire_semaphore() const {
    return slots_[slot_].acquire.get();
  }

  // Ends the open list and executes it, signalling the slot's fence. False when
  // nothing was executed (no open list, or the device is lost and the list was
  // abandoned): work recorded in it, timestamps included, never runs.
  bool Submit(plume::RenderCommandSemaphore** waits, uint32_t wait_count,
              plume::RenderCommandSemaphore** signals, uint32_t signal_count);
  // Ends the open list without executing it (nothing it recorded reaches the GPU).
  void Abandon();

  // Moves to the next slot, waits for its outstanding submission, and destroys
  // whatever retired object that wait proved idle.
  void Advance();

  // Waits until the queue is idle -- every submission, and the queue work that
  // follows the last one (the swap chain's present), which no frame fence
  // covers -- and destroys every retired object. Used around swap-chain
  // rebuilds and at shutdown. With the device lost the waits are skipped (a
  // removed device never signals a fence).
  void WaitIdle();

  template <typename T>
  void Retire(std::unique_ptr<T> object) {
    if (object) {
      retired_.push_back({next_serial_, std::make_unique<Holder<T>>(std::move(object))});
    }
  }

  // Runs `action` once every use recorded so far has completed on the GPU.
  void RetireAction(std::function<void()> action) {
    if (action) {
      retired_.push_back({next_serial_, std::make_unique<Action>(std::move(action))});
    }
  }

  size_t retired_count() const { return retired_.size(); }

 private:
  struct RetiredObject {
    virtual ~RetiredObject() = default;
  };
  template <typename T>
  struct Holder final : RetiredObject {
    explicit Holder(std::unique_ptr<T> o) : object(std::move(o)) {}
    std::unique_ptr<T> object;
  };
  struct Action final : RetiredObject {
    explicit Action(std::function<void()> f) : run(std::move(f)) {}
    ~Action() override { run(); }
    std::function<void()> run;
  };
  struct Retirement {
    uint64_t serial;
    std::unique_ptr<RetiredObject> object;
  };
  struct Slot {
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderCommandSemaphore> acquire;
    bool submitted = false;  // a fence signal is outstanding
    uint64_t serial = 0;     // serial of that submission
  };

  void WaitSlot(Slot& slot);
  // Executes an empty list from the current slot, signalling its fence after
  // everything queued so far. Only with no list open.
  void SubmitQueueFence();
  void ReleaseCompleted();

  Device* device_ = nullptr;
  Slot slots_[kFramesInFlight];
  uint32_t slot_ = 0;
  bool open_ = false;
  // Serial the next Submit() will carry; objects retired now are freed after it.
  uint64_t next_serial_ = 1;
  uint64_t completed_serial_ = 0;
  std::deque<Retirement> retired_;
};

}  // namespace mcla::gpu
