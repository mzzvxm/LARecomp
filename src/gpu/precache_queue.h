#pragma once
// ===========================================================================
// MCLA GPU renderer -- the precache queue: its order, and where the workers go next (pure rules)
// ===========================================================================
// Pipelines::StartPrecache turns the starter list (records_ and the level copies, setup_copies_) into the
// queue the precache workers make from, and the render thread's claims move the workers within it. This file
// holds those rules without a renderer, a device or a GPU, so that precache_queue_test.cpp checks them alone:
//
//   DeferTarget, Order   the queue's order: the list's own order with each level copy right after the record it
//                        was made from; a start at another sample count than the default has the entries at
//                        the other multisampled counts (which its draws never ask for first) after all the others
//   Cursor               where the workers are: the next entry of the list, and the windows the render thread's
//                        claims put ahead of it (boot mode)
//
// The claims. A draw that may not be skipped (a target not drawn every frame yet, a stencil write, one sample)
// that finds its entry not begun makes the pipeline itself: a claim. In boot mode (a weak CPU: Pipelines::
// kBootBoostBelow) a claim says where the render thread is in the list, and the workers go there next. The
// render thread does not ask in the order of the list: the title's first frames asked for an entry of the
// one-sample block at 1789 (a UI pass) first and then went back to the entries of the world at 1347 to 1390,
// which the cursor of the three workers was about to reach (STARTER-ROUND3, sr3v-boot-4, 4 logical processors).
// The cursor used to JUMP to the entry after the claim and park what it passed over until the end of the list:
// the render thread then made the 44 entries of the parked run itself (644 ms of its first burst; round 2 of the
// same tour had 17: how many it makes depends on where the cursor is at the first stray claim). A claim now puts a WINDOW (the
// next kFollowWindow entries after it) in front of the cursor and leaves the cursor where it is: a claim ahead
// of the render thread's real place costs the workers one window of work, not the entries the cursor passed.
// Nothing is parked, so every entry of the list is still made once, in the end, by the cursor or a window.
//
// Threading: none here. The caller holds Pipelines::work_mutex_ for every call (the workers call Next, the
// render thread calls Follow).
// ===========================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace mcla::gpu::precache_queue {

// The entries after a claim that the workers make first: what 3 workers make in 0.4 s (9 ms an entry; 6 workers
// 0.2 s), more than twice the 48 entries a first draw burst asked for in one block (sr3v-vk-high-menus: 46 claims
// in the entries 1789 to 1836).
inline constexpr size_t kFollowWindow = 128;
// The windows kept at once: the newest claim's first. A claim in a different place than the last ones (the
// title's one-sample block, then the world) keeps the earlier windows for later; more than this many drop the
// oldest (the cursor still makes their entries in time).
inline constexpr size_t kMaxFollowWindows = 8;

// The sample count a start's queue is ordered by, or 0 for the list's own order: the count the start draws
// the images of the screen at (Pipelines::start_samples_, the last of them: the upscaler's 1 after the MSAA
// level), when it is not the default one. The command line prebuild (`prebuild`) makes the whole list and
// has no draws to order by.
inline uint32_t DeferTarget(bool prebuild, const std::vector<uint32_t>& start_samples, uint32_t default_samples) {
  const uint32_t target = start_samples.empty() ? 0u : start_samples.back();
  return (!prebuild && target != 0 && target != default_samples) ? target : 0u;
}

// The queue: every record in the list's order, each level copy right after the record it was made from
// (`copy_source[i]` is the index in `records` of `copies[i]`'s record, ascending), except that with a
// `defer_target` (DeferTarget) the entries with more than one sample whose count is not that target go after
// all the others in their old order. Both lists are returned as pointers into the arguments; each entry once.
// `Record` has `key.sample_count`.
template <class Record>
std::vector<const Record*> Order(const std::vector<Record>& records, const std::vector<Record>& copies,
                                 const std::vector<uint32_t>& copy_source, uint32_t defer_target) {
  const auto later = [&](const Record& r) {
    return defer_target != 0 && r.key.sample_count > 1 && r.key.sample_count != defer_target;
  };
  std::vector<const Record*> queue;
  std::vector<const Record*> after;
  queue.reserve(records.size() + copies.size());
  size_t next_copy = 0;
  for (size_t i = 0; i < records.size(); ++i) {
    (later(records[i]) ? after : queue).push_back(&records[i]);
    for (; next_copy < copies.size() && copy_source[next_copy] == i; ++next_copy) {
      (later(copies[next_copy]) ? after : queue).push_back(&copies[next_copy]);
    }
  }
  queue.insert(queue.end(), after.begin(), after.end());
  return queue;
}

// Where the workers go next in a queue of `end` entries: the windows first (the newest claim's first), then
// the cursor, which has not been moved by any claim. Next returns the entries `queued` accepts (the ones
// nobody has begun) and drops the rest, so an entry two places hold (a window and the cursor's run) is made
// once and a worker that finds it begun goes on.
class Cursor {
 public:
  struct Window {
    size_t begin = 0;
    size_t end = 0;  // exclusive
  };

  void Reset(size_t end) {
    next_ = 0;
    end_ = end;
    windows_.clear();
  }

  // Work left to find: false only when Next has nothing more to hand out.
  bool HasWork() const { return next_ < end_ || !windows_.empty(); }

  // The next entry a worker makes, or false when there is none (the queue is done).
  template <class Queued>
  bool Next(size_t& index, Queued&& queued) {
    for (;;) {
      size_t i;
      if (!windows_.empty()) {
        Window& w = windows_.front();
        i = w.begin++;
        if (w.begin >= w.end) {
          windows_.pop_front();
        }
      } else if (next_ < end_) {
        i = next_++;
      } else {
        return false;
      }
      if (queued(i)) {
        index = i;
        return true;
      }
    }
  }

  // The render thread made entry `claimed` itself: the entries after it are the next ones it will ask
  // for, the workers make them first. A claim at the end of the queue has none after it.
  void Follow(size_t claimed) {
    const size_t begin = claimed + 1;
    if (begin >= end_) {
      return;
    }
    windows_.push_front(Window{begin, std::min(end_, begin + kFollowWindow)});
    if (windows_.size() > kMaxFollowWindows) {
      windows_.pop_back();
    }
  }

  // For the checks and the log: the cursor's place, the windows waiting, the entries in them.
  size_t next() const { return next_; }
  size_t end() const { return end_; }
  const std::deque<Window>& windows() const { return windows_; }
  size_t window_entries() const {
    size_t n = 0;
    for (const Window& w : windows_) {
      n += w.end - w.begin;
    }
    return n;
  }

 private:
  size_t next_ = 0;
  size_t end_ = 0;
  std::deque<Window> windows_;
};

}  // namespace mcla::gpu::precache_queue
