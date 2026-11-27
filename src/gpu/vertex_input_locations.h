#pragma once

#include <cstdint>
#include <string_view>

namespace mcla::gpu::vertex_input_locations {

constexpr uint32_t kUnmapped = UINT32_MAX;

// LAR-XenosRecomp's fixed USAGE_LOCATIONS for the v8 shader pack. Used for
// both new guest layouts and layouts loaded from the shared pipeline lists.
constexpr uint32_t FixedLocation(std::string_view semantic, uint32_t index) {
  struct Input {
    std::string_view semantic;
    uint32_t index;
  };
  constexpr Input inputs[] = {
      {"POSITION", 0}, {"NORMAL", 0}, {"TANGENT", 0}, {"BINORMAL", 0},
      {"TEXCOORD", 0}, {"TEXCOORD", 1}, {"TEXCOORD", 2}, {"TEXCOORD", 3},
      {"COLOR", 0}, {"BLENDINDICES", 0}, {"BLENDWEIGHT", 0}, {"COLOR", 1},
      {"TEXCOORD", 4}, {"TEXCOORD", 5}, {"TEXCOORD", 6}, {"TEXCOORD", 7}, {"POSITION", 1},
  };
  for (uint32_t location = 0; location < sizeof(inputs) / sizeof(inputs[0]); ++location) {
    if (inputs[location].index == index && inputs[location].semantic == semantic) {
      return location;
    }
  }
  return kUnmapped;
}

template <typename Elements>
bool RestoreFixedLocations(Elements& elements) {
  bool changed = false;
  for (auto& element : elements) {
    if (element.semanticName) {
      const uint32_t location = FixedLocation(element.semanticName, element.semanticIndex);
      if (location != kUnmapped && element.location != location) {
        element.location = location;
        changed = true;
      }
    }
  }
  return changed;
}

}  // namespace mcla::gpu::vertex_input_locations
