#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcla::gpu::title_capture {

inline std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == text.npos) return {};
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

inline std::optional<uint32_t> Number(std::string_view text) {
  if (text.empty()) return std::nullopt;
  uint32_t value = 0;
  const auto end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return value;
}

struct Selector {
  std::string text;
  std::optional<uint32_t> tile;
  bool seen = false;

  bool Matches(uint32_t source_tile, std::string_view label, std::string_view side_view) const {
    return tile ? *tile == source_tile : text == label || text == side_view;
  }
};

struct Plan {
  std::vector<Selector> targets;
  std::vector<uint32_t> seconds;
};

inline std::optional<Plan> Parse(std::string_view targets, std::string_view seconds) {
  Plan plan;
  for (;;) {
    const auto comma = targets.find(',');
    const auto text = Trim(targets.substr(0, comma));
    if (text.empty()) return std::nullopt;
    if (text != "scene") {
      Selector selector{std::string(text), std::nullopt};
      if (text.starts_with("rt:")) {
        selector.tile = Number(text.substr(3));
        if (!selector.tile) return std::nullopt;
      }
      plan.targets.push_back(std::move(selector));
    }
    if (comma == targets.npos) break;
    targets.remove_prefix(comma + 1);
  }
  for (;;) {
    const auto comma = seconds.find(',');
    const auto time = Number(Trim(seconds.substr(0, comma)));
    if (!time) return std::nullopt;
    plan.seconds.push_back(*time);
    if (comma == seconds.npos) break;
    seconds.remove_prefix(comma + 1);
  }
  std::sort(plan.seconds.begin(), plan.seconds.end());
  plan.seconds.erase(std::unique(plan.seconds.begin(), plan.seconds.end()), plan.seconds.end());
  return plan;
}

using Clock = std::chrono::steady_clock;

inline bool Due(uint32_t seconds, Clock::time_point title, Clock::time_point now) {
  return title != Clock::time_point{} && now - title >= std::chrono::seconds(seconds);
}

}  // namespace mcla::gpu::title_capture
