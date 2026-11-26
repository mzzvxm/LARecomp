#pragma once

#include "side_views.h"

namespace mcla::gpu::ui_map_views {

// Xenos render target formats, checked against the SDK in side_view_ui_map.cpp.
inline constexpr uint8_t kColorFormat = 0;  // k_8_8_8_8
inline constexpr uint8_t kDepthFormat = 1;  // kD24FS8

struct Identity {
  bool depth;
  uint16_t base_tiles;
  uint16_t pitch;
  uint8_t samples;
  uint32_t width;
  uint32_t height;
};
inline constexpr Identity kIdentities[] = {
    {false, 224, 280, 4, 256, 256},
    {true, 0, 280, 4, 256, 256},
    {false, 168, 240, 4, 220, 220},  // minimap: colour only
    {false, 0, 760, 4, 749, 430},    // D-pad quick map: colour only
    {false, 960, 960, 2, 960, 640},  // menu page, with the depth below
    {true, 0, 960, 2, 960, 640},
};

// EdramKey in the renderer, or the corresponding fields in the standalone test.
template <typename Key>
bool Matches(const Key& key, uint32_t size_bits) {
  if (key.reserved != 0) {
    return false;
  }
  for (const Identity& identity : kIdentities) {
    if (key.samples != identity.samples || key.pitch != identity.pitch || key.base_tiles != identity.base_tiles ||
        (key.depth != 0) != identity.depth) {
      continue;
    }
    // A different declared size at the same key belongs to another pass.
    if (size_bits != 0 && !side_views::DeclaredSize(size_bits, identity.width, identity.height)) {
      return false;
    }
    return key.format == (identity.depth ? kDepthFormat : kColorFormat);
  }
  return false;
}

}  // namespace mcla::gpu::ui_map_views
