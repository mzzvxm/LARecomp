#pragma once

#include "side_views.h"

namespace mcla::gpu::foliage_impostor_view {

// Xenos k_8_8_8_8 and kD24FS8, checked against the SDK in the rule's .cpp.
inline constexpr uint8_t kColorFormat = 0;
inline constexpr uint8_t kDepthFormat = 1;

// The pitch-160 foliage bake binds colour, a second colour for night normals,
// and shared depth. The separately used pitch-280 surface has other consumers.
template <typename Key>
bool Matches(const Key& key, uint32_t size_bits) {
  if (key.reserved != 0 || key.pitch != 160 || key.samples != 4) {
    return false;
  }
  if (size_bits != 0 && !side_views::DeclaredSize(size_bits, 128, 128)) {
    return false;
  }
  if (key.depth) {
    return key.base_tiles == 0 && key.format == kDepthFormat;
  }
  return (key.base_tiles == 224 || key.base_tiles == 288) && key.format == kColorFormat;
}

inline constexpr bool FollowsOutput(bool native_size, bool sharp_minimap) {
  // The debug off arm restores the old scaling rule, including its minimap setting.
  return !native_size && sharp_minimap;
}

}  // namespace mcla::gpu::foliage_impostor_view
