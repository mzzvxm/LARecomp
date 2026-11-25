#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::hitch {

class Cadence {
 public:
  double median_ms() const { return median_ms_; }

  bool Observe(double ms) {
    if (ms <= 0.0) return false;
    const bool hitch = ms > 50.0 || (count_ >= 8 && ms > 2.0 * median_ms_);
    samples_[next_] = ms;
    next_ = (next_ + 1) % samples_.size();
    count_ = std::min(count_ + 1, samples_.size());
    if (count_ >= 8) {
      auto sorted = samples_;
      auto middle = sorted.begin() + count_ / 2;
      std::nth_element(sorted.begin(), middle, sorted.begin() + count_);
      median_ms_ = *middle;
      if (count_ % 2 == 0) {
        median_ms_ = (median_ms_ + *std::max_element(sorted.begin(), middle)) * 0.5;
      }
    }
    return hitch;
  }

 private:
  std::array<double, 63> samples_{};
  size_t next_ = 0;
  size_t count_ = 0;
  double median_ms_ = 0.0;
};

class RateLimit {
 public:
  bool Allow(uint64_t now_ms) {
    if (now_ms - window_ms_ >= 1000) {
      window_ms_ = now_ms;
      lines_ = 0;
    }
    if (lines_ == 8) {
      ++suppressed_;
      return false;
    }
    ++lines_;
    return true;
  }

  uint64_t TakeSuppressed() {
    const auto count = suppressed_;
    suppressed_ = 0;
    return count;
  }

 private:
  uint64_t window_ms_ = 0;
  uint64_t suppressed_ = 0;
  uint32_t lines_ = 0;
};

}  // namespace mcla::gpu::hitch
