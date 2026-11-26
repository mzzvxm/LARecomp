#pragma once

#include <optional>
#include <string_view>

namespace mcla::gpu::mirror_setting {

// The old raster row used named levels. Preserve Off and fold every old
// enabled level into the single fixed-quality option.
constexpr std::string_view CanonicalWord(std::string_view word) {
  if (word == "auto" || word == "high" || word == "ultra") return "on";
  return word;
}

constexpr std::optional<std::string_view> LegacyRtWord(std::string_view word) {
  if (word == "off") return "off";
  if (word == "on" || word == "auto") return "on";
  return std::nullopt;
}

struct Result {
  int mode = 0;  // 1 is the fixed production size; 2 to 6 are debug sizes.
  bool finder = false;
  bool overlay = false;
  bool operator==(const Result&) const = default;
};

constexpr Result Resolve(std::string_view word, int debug_view, bool debug_glass, bool debug_overlay) {
  const bool on = CanonicalWord(word) == "on";
  const int mode = debug_view >= 1 && debug_view <= 6 ? debug_view : int(on);
  return {mode, on || debug_glass, on || debug_overlay};
}

Result Current();
void LogIfChanged(const Result& now);

}  // namespace mcla::gpu::mirror_setting
