#pragma once
// ===========================================================================
// MCLA GPU renderer -- level seeds: the pipelines of an MSAA level, made ahead from the saved lists (pure rules)
// ===========================================================================
// Pipelines::PrebuildForLevel makes ahead the pipelines the session has DRAWN, at the sample count of the
// level the player is changing to. What the session has not drawn yet (the pause view or the map when
// the player changed the setting in free roam, a race, a shop) has no copy at the new level: it is
// made when its first draw asks, on the render thread or in the background with the draw left out for
// a few frames (STARTER-ROUND2: 361 to 613 new keys in a switch run, nearly all level copies of listed
// records, 10 to 50 ms each cold). The seeds close that gap: the records of the saved lists (the cache
// file and the starter list) that have more than one sample are converted to the new level's sample
// count and handed to the builder threads behind everything else, never waited for.
//
// This file holds the rules that need no renderer, no device and no GPU, so that level_seeds_test.cpp
// checks them alone:
//   AtLevel, PlanSeeds   which records become seeds, in which order, how many (the key conversion and
//                        the skips: a pipeline that exists, is queued or recorded at that level is not
//                        made again; the same pipeline reached from two records is made once)
//   AllowedCount,
//   ShortOfRoom          the memory bound: seeds only while the video memory in use stays under
//                        kStartPercent of the OS budget, at most kMaxSeeds at a time, and the unused
//                        ones are let go when the use passes kDropPercent (the caches' own response
//                        starts at 90, vram_budget.h: the seeds are a luxury and go first)
//   Decide               what a level change does to the seeds already there (a new level drops the
//                        untaken ones of the old one; the same level keeps them)
//   Slots, MayStart      the queue rule: seeds come after the urgent work, the copies a switch waits for
//                        and the precache list; never in boot mode; never on every worker (one stays
//                        free for urgent work, as long as there are two)
//   SeedBook             the run's counts (queued, ready, taken ...) for the log line, with a
//                        generation so that a build of an earlier run finishing late counts for nothing
//
// Threading: PlanSeeds, Decide and the constexpr rules are pure; SeedBook has its own mutex (the builder
// threads report into it, the render thread reads it).
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>
#include <unordered_set>
#include <vector>

namespace mcla::gpu::level_seeds {

// The most seeds one level holds (made, queued or being made, not yet drawn with). The measured tours
// asked for 450 to 700 level copies of the list's records (STARTER-ROUND2); the list has about 1300
// multisampled pipelines, so the cap leaves out the tail of a list rather than the first seeds.
inline constexpr uint32_t kMaxSeeds = 1024;
// What the memory rules count for one pipeline: about 85 KB measured on D3D12 (the comment at
// Pipelines::StartPersistence), Vulkan keeps more; 128 KiB covers both.
inline constexpr uint64_t kBytesEach = 128ull << 10;
// Seeds are made only while the use of video memory (the OS's figure, Device::QueryVideoMemory) plus
// what they take stays under this share of its budget, and let go above the second share.
inline constexpr uint32_t kStartPercent = 80;
inline constexpr uint32_t kDropPercent = 85;
// Frames between two looks at the budget while seeds are held (VideoMemoryBudget::kCheckFrames).
inline constexpr uint64_t kCheckFrames = 30;
// Made seeds that are let go are destroyed this many a frame (the driver frees each on the render thread:
// VideoMemoryBudget::kMaxItems), so that letting 1,000 go is not a hitch.
inline constexpr uint32_t kReleasePerFrame = 64;

// How many more seeds the budget allows, `held` being the seeds the level already holds (queued, being
// made or made and not drawn with). An adapter that reports no budget (0) has nothing to compare with:
// the cap alone then.
constexpr uint32_t AllowedCount(uint64_t used, uint64_t budget, uint32_t held) {
  if (held >= kMaxSeeds) {
    return 0;
  }
  const uint32_t cap = kMaxSeeds - held;
  if (budget == 0) {
    return cap;
  }
  const uint64_t line = budget * kStartPercent / 100;
  if (used >= line) {
    return 0;
  }
  return uint32_t(std::min<uint64_t>(cap, (line - used) / kBytesEach));
}

// The use is past the line where the unused seeds are let go. False when the adapter reports no budget.
constexpr bool ShortOfRoom(uint64_t used, uint64_t budget) {
  return budget != 0 && used * 100 >= budget * kDropPercent;
}

// What a call for `level` does to the seeds of `seeded_level` (0: none yet).
enum class Retarget : uint8_t {
  kStart,    // no seeds: make the set
  kKeep,     // the same level again: the seeds stay, only the records that are new are added
  kRestart,  // another level: the untaken seeds of the old one are dropped, then the set is made
};

constexpr Retarget Decide(uint32_t seeded_level, uint32_t level) {
  if (seeded_level == 0) {
    return Retarget::kStart;
  }
  return seeded_level == level ? Retarget::kKeep : Retarget::kRestart;
}

// Workers that may build seeds at once: all but one so that urgent work always finds a free worker; a
// single worker builds them too (an urgent request then waits for the build under way: 10 to 50 ms).
constexpr uint32_t Slots(uint32_t steady_workers) {
  return steady_workers > 1 ? steady_workers - 1 : 1;
}

// Whether a worker that found nothing else to do takes a seed: one is queued, boot mode is over (the
// boot list has every processor), and fewer than `slots` seeds are being built. The caller has already
// found no urgent entry, no copy for a switch and no entry of the precache list.
constexpr bool MayStart(bool queued, bool boot_mode, uint32_t active, uint32_t slots) {
  return queued && !boot_mode && active < slots;
}

// The key of a record at `level`: the same pipeline but for its sample count.
template <class Key>
Key AtLevel(Key key, uint32_t level) {
  key.sample_count = level;
  return key;
}

struct PlanCounts {
  uint32_t sources = 0;     // records with more than one sample (the images of the screen are multisampled)
  uint32_t single = 0;      // records with one sample or none: no source, not counted as skipped
  uint32_t same_level = 0;  // sources that already are at the level
  uint32_t duplicate = 0;   // sources whose copy at the level another source gave already
  uint32_t present = 0;     // copies a pipeline, a queue entry or a record has already
  uint32_t limited = 0;     // copies left out by `limit`
  uint32_t picked = 0;      // seeds
};

// The records (in the order given) to make seeds of for `level`: each record with more than one sample
// whose key at `level` is not `present` and not picked already, at most `limit`; those recorded at
// `from_level` (the level in force: the draws the game makes now) first, then the others, each group in
// the list's order (the order the pipelines were first needed in). Returns indices into `records`.
// A `Record` has a `key` with `sample_count`; `Hash` hashes the key; `present(key at level)` says whether
// nothing needs making for it.
template <class Record, class Hash, class Present>
std::vector<uint32_t> PlanSeeds(const std::vector<Record>& records, uint32_t level,
                                uint32_t from_level, uint32_t limit, Present&& present,
                                PlanCounts& counts) {
  using Key = std::decay_t<decltype(std::declval<Record>().key)>;
  counts = PlanCounts{};
  std::vector<uint32_t> picks;
  std::unordered_set<Key, Hash> seen;
  const auto pass = [&](bool first_group) {
    for (uint32_t i = 0; i < records.size(); ++i) {
      const uint32_t samples = records[i].key.sample_count;
      if (samples <= 1) {
        if (first_group) {
          ++counts.single;  // counted once, in the first pass
        }
        continue;
      }
      if ((samples == from_level) != first_group) {
        continue;
      }
      ++counts.sources;
      if (samples == level) {
        ++counts.same_level;
        continue;
      }
      const Key copy = AtLevel(records[i].key, level);
      if (!seen.insert(copy).second) {
        ++counts.duplicate;
        continue;
      }
      if (present(copy)) {
        ++counts.present;
        continue;
      }
      if (picks.size() >= limit) {
        ++counts.limited;
        continue;
      }
      picks.push_back(i);
    }
  };
  pass(true);
  pass(false);
  counts.picked = uint32_t(picks.size());
  return picks;
}

// One run of seeds, from the call that queues them to the line that says how it went. Each seed carries
// the generation Begin returned: an event of an earlier run (a build that finishes after the level
// changed again) is ignored. All members lock; any thread.
class SeedBook {
 public:
  using Clock = std::chrono::steady_clock;

  struct Report {
    uint32_t level = 0;
    uint32_t queued = 0;     // handed to the builders
    uint32_t ready = 0;      // made by a worker
    uint32_t failed = 0;     // a worker could not make them (a draw that asks makes it, and says why)
    uint32_t taken = 0;      // a draw took a ready one (or waited for one being made and took it)
    uint32_t claimed = 0;    // a draw that may not be left out made it on the render thread
    uint32_t cancelled = 0;  // dropped before a worker began it
    uint32_t freed = 0;      // made, never asked for, let go
    double build_ms = 0.0;   // from the call to the last build that finished
  };

  // A new run: returns its generation (never 0). Anything of an earlier run is forgotten.
  uint32_t Begin(uint32_t level, uint32_t queued, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    if (generation_ == 0) {
      generation_ = 1;
    }
    report_ = Report{};
    report_.level = level;
    report_.queued = queued;
    began_ = now;
    last_finish_ = now;
    reported_ = false;
    active_ = true;
    return generation_;
  }
  // More seeds for the run in progress (the same level again).
  void Add(uint32_t generation, uint32_t queued) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Current(generation)) {
      return;
    }
    report_.queued += queued;
    reported_ = false;
  }
  // A worker finished a build.
  void Finished(uint32_t generation, bool made, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Current(generation)) {
      return;
    }
    ++(made ? report_.ready : report_.failed);
    last_finish_ = std::max(last_finish_, now);
  }
  void Taken(uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Current(generation)) {
      ++report_.taken;
    }
  }
  void Claimed(uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Current(generation)) {
      ++report_.claimed;
    }
  }
  void Cancelled(uint32_t generation, uint32_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Current(generation)) {
      report_.cancelled += n;
    }
  }
  void Freed(uint32_t generation, uint32_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Current(generation)) {
      report_.freed += n;
    }
  }

  // Queued or being made.
  uint32_t Outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return OutstandingLocked();
  }
  // Seeds that hold (or will hold) memory: queued, being made, or made and not drawn with.
  uint32_t Live() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? LiveLocked() : 0;
  }
  bool Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
  }
  Report Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Report r = report_;
    r.build_ms = BuildMs();
    return r;
  }
  // True once per run (or per Add): nothing is queued or being made any more. Fills `out`.
  bool TakeDone(Report& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || reported_ || report_.queued == 0 || OutstandingLocked() != 0) {
      return false;
    }
    reported_ = true;
    out = report_;
    out.build_ms = BuildMs();
    return true;
  }
  // The run is over (its seeds were let go): events of its generation count for nothing from now on.
  void End() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    ++generation_;
    if (generation_ == 0) {
      generation_ = 1;
    }
  }

 private:
  bool Current(uint32_t generation) const { return active_ && generation == generation_; }
  uint32_t OutstandingLocked() const {
    const uint32_t done = report_.ready + report_.failed + report_.claimed + report_.cancelled;
    return report_.queued > done ? report_.queued - done : 0;
  }
  uint32_t LiveLocked() const {
    // made and not yet taken or freed, plus what is still to be made; a failed or claimed seed
    // holds nothing.
    const uint32_t gone = report_.failed + report_.claimed + report_.cancelled + report_.taken + report_.freed;
    return report_.queued > gone ? report_.queued - gone : 0;
  }
  double BuildMs() const {
    return std::chrono::duration<double, std::milli>(last_finish_ - began_).count();
  }

  mutable std::mutex mutex_;
  uint32_t generation_ = 0;
  bool active_ = false;
  bool reported_ = false;
  Report report_;
  Clock::time_point began_{};
  Clock::time_point last_finish_{};
};

}  // namespace mcla::gpu::level_seeds
