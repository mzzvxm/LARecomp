// MCLA GPU renderer -- the renderer's own pipelines, made ahead of first use.
// See host_precache.h.

#include "host_precache.h"

#include <format>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

void HostPrecache::Add(uint64_t key, std::string name, Make make) {
  if (builder_.joinable()) {
    REXLOG_WARN("[gpu] host pipelines: '{}' listed after the builder started; its owner makes it",
                name);
    return;
  }
  if (by_key_.contains(key)) {
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->key = key;
  entry->name = std::move(name);
  entry->make = std::move(make);
  by_key_.emplace(key, entry.get());
  entries_.push_back(std::move(entry));
}

void HostPrecache::Start() {
  if (builder_.joinable() || entries_.empty()) {
    return;
  }
  stop_.store(false, std::memory_order_release);
  start_ = Clock::now();
  builder_ = std::thread([this] { Run(); });
  REXLOG_INFO("[gpu] host pipelines: {} listed, made ahead on one builder thread",
              entries_.size());
}

void HostPrecache::Stop() {
  if (builder_.joinable()) {
    stop_.store(true, std::memory_order_release);
    builder_.join();
  }
  if (!entries_.empty() || stats_.unlisted != 0) {
    Summarize();
  }
  by_key_.clear();
  entries_.clear();  // the pipelines nobody took die here: the GPU is idle
  unlisted_names_.clear();
  stats_ = Stats{};
  built_ = 0;
  failed_ = 0;
  skipped_ = 0;
  raised_ = false;
  stop_.store(false, std::memory_order_release);
}

void HostPrecache::Run() {
#if defined(_WIN32)
  // The guest boots meanwhile, and so do the guest pipelines' workers.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (const auto& entry : entries_) {
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    uint8_t expected = kQueued;
    if (!entry->state.compare_exchange_strong(expected, kBuilding)) {
      ++skipped_;  // the render thread claimed it
      continue;
    }
    const Clock::time_point began = Clock::now();
    std::unique_ptr<plume::RenderPipeline> pipeline = entry->make();
    const bool made = pipeline != nullptr;
    {
      // The render thread may take it the moment the state says so.
      std::lock_guard<std::mutex> lock(mutex_);
      entry->build_ns = NsSince(began);
      entry->pipeline = std::move(pipeline);
      entry->state.store(made ? kReady : kFailed);
    }
    ++(made ? built_ : failed_);
    done_.notify_all();
  }
  REXLOG_INFO("[gpu] host pipelines: {} made ahead in {:.1f} ms ({} failed, {} left to the render "
              "thread, which began them first)",
              built_.load(), Ms(NsSince(start_)), failed_.load(), skipped_.load());
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
      return pipeline;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (entry->state.load() == kBuilding) {
#if defined(_WIN32)
        if (!raised_ && builder_.joinable()) {
          raised_ = true;
          SetThreadPriority(builder_.native_handle(), THREAD_PRIORITY_NORMAL);
        }
#endif
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
  for (const auto& entry : entries_) {
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
  const uint32_t built = built_.load();
  const uint32_t here = stats_.claimed + stats_.unlisted;
  REXLOG_INFO("[gpu] host pipelines: {} listed; {} made ahead by the builder thread, {} of them "
              "asked for ({:.1f} ms of compiles kept off the render thread, {} waited for {:.1f} "
              "ms), {} never; {} made on the render thread at first use ({} listed but not begun "
              "by the builder, {:.1f} ms; {} not listed, {:.1f} ms); {} failed{}",
              entries_.size(), built, stats_.taken, Ms(off_ns), stats_.waited, Ms(stats_.wait_ns),
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
