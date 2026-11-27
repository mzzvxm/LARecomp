// MCLA GPU renderer -- the renderer's own pipelines, made ahead of first use.
// See host_precache.h.

#include "host_precache.h"

#include <algorithm>

#include <format>

#include <rex/logging/log_category.h>

#include "os/thread_priority.h"

namespace mcla::gpu {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NsSince(Clock::time_point from) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - from).count());
}

double Ms(uint64_t ns) {
  return double(ns) / 1.0e6;
}

// "a; b; c".
std::string Join(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) {
    if (!text.empty()) {
      text += "; ";
    }
    text += name;
  }
  return text;
}

}  // namespace

HostPrecache::~HostPrecache() {
  Stop();
}

bool HostPrecache::Add(uint64_t key, std::string name, Make make) {
  if (builder_.joinable()) {
    REXLOG_WARN("[gpu] host pipelines: '{}' listed after the builder started; its owner makes it",
                name);
    return false;
  }
  if (by_key_.contains(key)) {
    return false;
  }
  auto entry = std::make_unique<Entry>();
  entry->key = key;
  entry->name = std::move(name);
  entry->make = std::move(make);
  by_key_.emplace(key, entry.get());
  entries_.push_back(std::move(entry));
  return true;
}

void HostPrecache::Note(std::string text, uint32_t listed) {
  notes_.push_back(Noted{std::move(text), listed});
}

void HostPrecache::Start() {
  if (builder_.joinable()) {
    return;
  }
  // One line per start: what was listed, and for the passes the settings switch, why.
  std::string by_setting;
  uint32_t noted = 0;
  for (const Noted& note : notes_) {
    if (!by_setting.empty()) {
      by_setting += "; ";
    }
    by_setting += note.listed != 0
                      ? std::format("{}: {}", note.text, note.listed)
                      : std::format("{}: none, made in the background if it is switched on", note.text);
    noted += note.listed;
  }
  const size_t listed = entries_.size();
  if (entries_.empty()) {
    if (!notes_.empty()) {
      REXLOG_INFO("[gpu] host pipelines at start: nothing listed ({})", by_setting);
    }
    return;
  }
  stop_.store(false, std::memory_order_release);
  start_ = Clock::now();
  builder_ = std::thread([this] { Run(); });
  if (notes_.empty()) {
    REXLOG_INFO("[gpu] host pipelines: {} listed, made ahead on one builder thread", listed);
  } else {
    REXLOG_INFO("[gpu] host pipelines at start: {} listed, made ahead on one builder thread; by the "
                "settings: {}; the other {} are the resolve conversions' and the decode's, which "
                "every start uses",
                listed, by_setting, listed > noted ? listed - noted : 0u);
  }
}

void HostPrecache::Stop() {
  if (builder_.joinable() || !live_builders_.empty()) {
    stop_.store(true, std::memory_order_release);
  }
  if (builder_.joinable()) {
    builder_.join();
  }
  for (std::thread& worker : live_builders_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  live_builders_.clear();
  if (!entries_.empty() || !live_entries_.empty() || stats_.unlisted != 0) {
    Summarize();
  }
  by_key_.clear();
  entries_.clear();  // the pipelines nobody took die here: the GPU is idle
  live_entries_.clear();
  notes_.clear();
  render_logged_ = 0;
  unlisted_names_.clear();
  stats_ = Stats{};
  built_ = 0;
  failed_ = 0;
  skipped_ = 0;
  raised_ = false;
  stop_.store(false, std::memory_order_release);
}

void HostPrecache::Run() {
  // The guest boots meanwhile, and so do the guest pipelines' workers. Raisable: Acquire brings
  // this thread back to normal when a draw waits for the pipeline it is making, which on Linux
  // needs a lowering that can be undone without privileges (os/thread_priority.h).
  larecomp::os::SetCurrentThreadIntent(larecomp::os::ThreadIntent::kBackground,
                                       larecomp::os::Lowering::kRaisable);
  for (const auto& entry : entries_) {
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    if (BuildEntry(*entry) == Built::kClaimed) {
      ++skipped_;  // the render thread claimed it
    }
  }
  REXLOG_INFO("[gpu] host pipelines: {} made ahead in {:.1f} ms ({} failed, {} left to the render "
              "thread, which began them first)",
              built_.load(), Ms(NsSince(start_)), failed_.load(), skipped_.load());
}

HostPrecache::Built HostPrecache::BuildEntry(Entry& entry) {
  uint8_t expected = kQueued;
  if (!entry.state.compare_exchange_strong(expected, kBuilding)) {
    return Built::kClaimed;
  }
  const Clock::time_point began = Clock::now();
  std::unique_ptr<plume::RenderPipeline> pipeline = entry.make();
  const bool made = pipeline != nullptr;
  {
    // The render thread may take it the moment the state says so.
    std::lock_guard<std::mutex> lock(mutex_);
    entry.build_ns = NsSince(began);
    entry.pipeline = std::move(pipeline);
    entry.state.store(made ? kReady : kFailed);
  }
  ++(made ? built_ : failed_);
  done_.notify_all();
  return made ? Built::kMade : Built::kFailed;
}

uint32_t HostPrecache::AddLive(std::string pass, std::vector<Item> items) {
  std::vector<Entry*> fresh;
  for (Item& item : items) {
    if (by_key_.contains(item.key)) {
      continue;
    }
    auto entry = std::make_unique<Entry>();
    entry->key = item.key;
    entry->name = std::move(item.name);
    entry->make = std::move(item.make);
    by_key_.emplace(entry->key, entry.get());
    fresh.push_back(entry.get());
    live_entries_.push_back(std::move(entry));
  }
  if (fresh.empty()) {
    return 0;
  }
  const uint32_t count = uint32_t(fresh.size());
  live_builders_.emplace_back(
      [this, pass = std::move(pass), list = std::move(fresh), began = Clock::now()] {
        RunLive(pass, list, began);
      });
  return count;
}

void HostPrecache::RunLive(const std::string& pass, const std::vector<Entry*>& list,
                           Clock::time_point began) {
  // The game runs meanwhile; nothing waits for this worker (the owner draws without the pass until
  // Ready), so there is no raising: a plain, one-way lowering (Windows: below normal, as always;
  // Linux: SCHED_BATCH and a higher nice value, os/thread_priority.h).
  larecomp::os::SetCurrentThreadIntent(larecomp::os::ThreadIntent::kBackground);
  uint32_t made = 0, failed = 0, left = 0;
  for (Entry* entry : list) {
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    switch (BuildEntry(*entry)) {
      case Built::kMade: ++made; break;
      case Built::kFailed: ++failed; break;
      case Built::kClaimed: ++left; break;
    }
  }
  REXLOG_INFO("[gpu] host pipelines: {}: {} made in the background in {:.1f} ms ({} failed, {} left to "
              "the render thread, which began them first)",
              pass, made, Ms(NsSince(began)), failed, left);
}

bool HostPrecache::Pending() const {
  const auto pending = [](const auto& entry) {
    if (!entry) return false;
    const uint8_t state = entry->state.load(std::memory_order_acquire);
    return state == kQueued || state == kBuilding;
  };
  return std::any_of(entries_.begin(), entries_.end(), pending) ||
         std::any_of(live_entries_.begin(), live_entries_.end(), pending);
}

bool HostPrecache::Ready(uint64_t key) const {
  const auto found = by_key_.find(key);
  if (found == by_key_.end()) {
    return true;  // nobody listed it: nothing is being made for it
  }
  const uint8_t state = found->second->state.load(std::memory_order_acquire);
  return state != kQueued && state != kBuilding;
}

void HostPrecache::NoteRenderThread(const std::string& name, const char* why, uint64_t ns) {
  if (render_logged_ >= kRenderLogMax) {
    return;
  }
  ++render_logged_;
  REXLOG_INFO("[gpu] host pipelines: '{}' made on the render thread ({}, {:.1f} ms)", name, why,
              Ms(ns));
}

std::unique_ptr<plume::RenderPipeline> HostPrecache::Acquire(uint64_t key,
                                                             const std::string& name,
                                                             const Make& make_unlisted) {
  const auto found = by_key_.find(key);
  Entry* entry = found != by_key_.end() ? found->second : nullptr;
  if (entry) {
    uint8_t expected = kQueued;
    if (entry->state.compare_exchange_strong(expected, kClaimed)) {
      // The builder has not begun it: made here, and the builder skips it.
      entry->used = true;
      const Clock::time_point began = Clock::now();
      std::unique_ptr<plume::RenderPipeline> pipeline = entry->make();
      entry->build_ns = NsSince(began);
      ++stats_.claimed;
      stats_.claimed_ns += entry->build_ns;
      if (!pipeline) {
        ++stats_.failed;
      }
      NoteRenderThread(entry->name, "listed, not begun by the builder yet", entry->build_ns);
      return pipeline;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (entry->state.load() == kBuilding) {
        if (!raised_ && builder_.joinable()) {
          raised_ = true;
          larecomp::os::SetThreadIntent(builder_, larecomp::os::ThreadIntent::kNormal);
        }
        const Clock::time_point began = Clock::now();
        done_.wait(lock, [&] { return entry->state.load() != kBuilding; });
        ++stats_.waited;
        stats_.wait_ns += NsSince(began);
      }
      const uint8_t state = entry->state.load();
      if (state == kReady) {
        entry->state.store(kTaken);
        entry->used = true;
        ++stats_.taken;
        return std::move(entry->pipeline);
      }
      if (state == kFailed) {
        entry->used = true;
        return nullptr;  // Device logged it; not tried again
      }
    }
    // Claimed or taken before: the owner asks again for what it gave up.
  }
  const Clock::time_point began = Clock::now();
  std::unique_ptr<plume::RenderPipeline> pipeline = make_unlisted();
  ++stats_.unlisted;
  stats_.unlisted_ns += NsSince(began);
  NoteRenderThread(name, "not listed", NsSince(began));
  if (!pipeline) {
    ++stats_.failed;
  }
  if (unlisted_names_.size() < 16) {
    unlisted_names_.push_back(name);
  }
  return pipeline;
}

void HostPrecache::Summarize() {
  std::vector<std::string> asked, never;
  uint32_t not_begun = 0;
  uint64_t off_ns = 0;  // compile time of the pipelines taken that the render thread did not spend
  const auto scan = [&](const std::vector<std::unique_ptr<Entry>>& list) {
    for (const auto& entry : list) {
      const uint8_t state = entry->state.load();
      if (entry->used) {
        const char* where = state == kClaimed ? ", on the render thread" : "";
        asked.push_back(entry->build_ns
                            ? std::format("{} ({:.1f} ms{})", entry->name, Ms(entry->build_ns), where)
                            : entry->name);
      } else {
        never.push_back(entry->name);
      }
      if (state == kTaken) {
        off_ns += entry->build_ns;
      }
      if (state == kQueued) {
        ++not_begun;
      }
    }
  };
  scan(entries_);
  scan(live_entries_);
  const uint32_t built = built_.load();
  const uint32_t here = stats_.claimed + stats_.unlisted;
  REXLOG_INFO("[gpu] host pipelines: {} listed{}; {} made ahead by the builder threads, {} of them "
              "asked for ({:.1f} ms of compiles kept off the render thread, {} waited for {:.1f} "
              "ms), {} never; {} made on the render thread at first use ({} listed but not begun "
              "by the builder, {:.1f} ms; {} not listed, {:.1f} ms); {} failed{}",
              entries_.size() + live_entries_.size(),
              live_entries_.empty()
                  ? std::string()
                  : std::format(" ({} of them after the start, for a setting switched on)",
                                live_entries_.size()),
              built, stats_.taken, Ms(off_ns), stats_.waited, Ms(stats_.wait_ns),
              built > stats_.taken ? built - stats_.taken : 0u, here, stats_.claimed,
              Ms(stats_.claimed_ns), stats_.unlisted, Ms(stats_.unlisted_ns),
              failed_.load() + stats_.failed,
              not_begun ? std::format(", {} not begun when stopped", not_begun) : std::string());
  if (!asked.empty()) {
    REXLOG_INFO("[gpu] host pipelines asked for: {}", Join(asked));
  }
  if (!never.empty()) {
    REXLOG_INFO("[gpu] host pipelines never asked for: {}", Join(never));
  }
  if (!unlisted_names_.empty()) {
    REXLOG_INFO("[gpu] host pipelines not listed, made at first use: {}", Join(unlisted_names_));
  }
}

}  // namespace mcla::gpu
