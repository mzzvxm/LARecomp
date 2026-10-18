#pragma once
// ===========================================================================
// MCLA GPU renderer -- frames in flight and resource retirement
// ===========================================================================
// kFramesInFlight slots, each with its command lists, fence and swap-chain
// acquire semaphore. While the GPU executes slot N the render thread records
// slot N+1; Advance() moves to the next slot and waits for the fence of the
// submission that last used it before its lists are reset. Two slots is what
// reblue and reeot both run (gpu/device.h kNumFrames, gpu/device.h kNumFrames):
// one more lets the CPU run further ahead without hiding anything new.
//
// A guest frame is recorded from its first scene record to its present
//, possibly in several batches: a
// batch flush ends the list being recorded, executes it WITHOUT a fence and
// opens the slot's next list (created on first need). Only the frame's last
// list signals the slot's fence; on one queue it completes after every batch
// before it, so waiting for it proves the whole slot idle.
//
// Plume's RenderCommandFence is binary (plume_d3d12.cpp: an auto-reset event
// armed by executeCommandLists; plume_vulkan.cpp: vkWaitForFences then
// vkResetFences). Waiting on a fence that was never submitted blocks forever,
// so each slot records whether it has a submission outstanding, and a slot is
// submitted at most once between two waits.
//
// Retirement. A GPU object that is no longer wanted may still be referenced by
// a command list the GPU has not finished. Retire() takes it over and tags it
// with the serial of the frame submission that will close the lists being
// recorded; it is destroyed once a fence wait proves that serial complete. On
// one queue submissions complete in order, so "serial S done" covers every
// earlier use, batches included (a batch executes before its frame's fenced
// list). This holds for any slot count and for a frame that is abandoned
// instead of submitted (its serial passes to the next submission, which
// completes later than any use recorded before it). reblue and reeot instead
// park objects in per-slot graveyards keyed by the recording slot, which is
// correct for two slots and needs reblue's RetireSlot guard because several
// guest threads retire concurrently; here only the render thread records or
// retires.
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
#include <vector>

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

  // Opens the current slot's first list if no list is open. Null when the
  // device is lost or the ring is not initialized.
  plume::RenderCommandList* Begin();
  bool is_open() const { return open_; }
  plume::RenderCommandList* list() const {
    return open_ ? slots_[slot_].lists[slots_[slot_].next_list - 1].get() : nullptr;
  }
  uint32_t slot() const { return slot_; }

  // The semaphore a swap-chain acquire for this slot signals, and its submission
  // waits on. One per slot: a binary semaphore cannot be re-signalled while a
  // wait on it is still pending.
  plume::RenderCommandSemaphore* acquire_semaphore() const {
    return slots_[slot_].acquire.get();
  }

  // Ends the open list, executes it without a fence and opens the slot's next
  // list, which it returns. Null when no list is open or the device is lost
  // (the open list is then abandoned).
  plume::RenderCommandList* FlushBatch();

  // Ends the open list and executes it, signalling the slot's fence. False when
  // nothing was executed (no open list, or the device is lost and the list was
  // abandoned): work recorded in it, timestamps included, never runs.
  bool Submit(plume::RenderCommandSemaphore** waits, uint32_t wait_count,
              plume::RenderCommandSemaphore** signals, uint32_t signal_count);
  // Ends the open list without executing it (nothing it recorded reaches the
  // GPU; batches already flushed did).
  void Abandon();

  // Moves to the next slot, waits for its outstanding submission, and destroys
  // whatever retired object that wait proved idle.
  void Advance();

  // Waits until the queue is idle -- every submission, and the queue work that
  // follows the last one (the swap chain's present), which no frame fence
  // covers -- then waits each slot's outstanding fence (consuming its signal,
  // see above), and destroys every retired object no open list can name. Used
  // around swap-chain rebuilds and at shutdown; valid with a list open (the
  // guest frame being recorded). With the device lost the waits are skipped
  // (a removed device never signals a fence).
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
  // Batches flushed since the last call (the stats line).
  uint64_t TakeBatchFlushes() {
    const uint64_t n = batch_flushes_;
    batch_flushes_ = 0;
    return n;
  }

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
    // The frame's batches in recording order; lists[next_list - 1] is the open
    // one while a frame is recorded. Each list owns its allocator/pool, reset
    // by its begin() only after the slot's fence was waited.
    std::vector<std::unique_ptr<plume::RenderCommandList>> lists;
    uint32_t next_list = 0;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderCommandSemaphore> acquire;
    bool submitted = false;  // a fence signal is outstanding
    uint64_t serial = 0;     // serial of that submission
  };

  // Opens the current slot's next list (creating it if needed).
  plume::RenderCommandList* OpenNextList();
  void WaitSlot(Slot& slot);
  void ReleaseCompleted();

  Device* device_ = nullptr;
  Slot slots_[kFramesInFlight];
  // An empty list and a fence of their own: WaitIdle executes them after
  // everything queued so far and waits, which works whether or not a slot's
  // list is open.
  std::unique_ptr<plume::RenderCommandList> idle_list_;
  std::unique_ptr<plume::RenderCommandFence> idle_fence_;
  uint32_t slot_ = 0;
  bool open_ = false;
  // Serial the next Submit() will carry; objects retired now are freed after it.
  uint64_t next_serial_ = 1;
  uint64_t completed_serial_ = 0;
  uint64_t batch_flushes_ = 0;
  std::deque<Retirement> retired_;
};

}  // namespace mcla::gpu
