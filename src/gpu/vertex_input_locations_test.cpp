#include "vertex_input_locations.h"

#include <array>
#include <cstdio>

namespace {

struct Element {
  const char* semanticName;
  uint32_t semanticIndex;
  uint32_t location;
  uint32_t slot;
  uint32_t offset;
  uint32_t format;
};

}  // namespace

int main() {
  using namespace mcla::gpu::vertex_input_locations;

  // The v8 translator's locations, with the attributes in a different order.
  std::array elements{
      Element{"POSITION", 0, 17, 0, 0, 6}, Element{"BLENDWEIGHT", 0, 1, 0, 12, 2},
      Element{"BLENDINDICES", 0, 2, 0, 24, 2}, Element{"NORMAL", 0, 3, 0, 40, 3},
      Element{"COLOR", 1, 4, 1, 4, 4}, Element{"TEXCOORD", 0, 5, 0, 56, 5},
      Element{"COLOR", 0, 6, 1, 0, 4}, Element{"POSITION", 1, 7, 2, 0, 6},
      Element{"TANGENT", 0, 8, 2, 12, 3}, Element{"BINORMAL", 0, 9, 2, 24, 3},
      Element{"TEXCOORD", 1, 10, 3, 0, 5}, Element{"TEXCOORD", 2, 11, 3, 8, 5},
      Element{"TEXCOORD", 3, 12, 3, 16, 5}, Element{"TEXCOORD", 4, 13, 3, 24, 5},
      Element{"TEXCOORD", 5, 14, 3, 32, 5}, Element{"TEXCOORD", 6, 15, 3, 40, 5},
      Element{"TEXCOORD", 7, 16, 3, 48, 5}, Element{"TEXCOORD", 8, 19, 3, 56, 5},
      Element{nullptr, 0, 20, 3, 0, 1},
  };
  const auto original = elements;
  const std::array<uint32_t, elements.size()> expected{
      0, 10, 9, 1, 11, 4, 8, 16, 2, 3, 5, 6, 7, 12, 13, 14, 15, 19, 20,
  };
  if (!RestoreFixedLocations(elements) || RestoreFixedLocations(elements)) {
    std::fprintf(stderr, "Expected one migration and then a stable layout\n");
    return 1;
  }
  for (size_t i = 0; i < elements.size(); ++i) {
    const auto& got = elements[i];
    const auto& before = original[i];
    if (got.location != expected[i] || got.semanticName != before.semanticName ||
        got.semanticIndex != before.semanticIndex || got.slot != before.slot ||
        got.offset != before.offset || got.format != before.format) {
      std::fprintf(stderr, "Wrong attribute at index %zu\n", i);
      return 1;
    }
  }
  std::array<Element, 0> empty{};
  if (RestoreFixedLocations(empty) || FixedLocation("PSIZE", 0) != kUnmapped) {
    std::fprintf(stderr, "Unexpected location for an empty layout or unmapped semantic\n");
    return 1;
  }
  std::printf("vertex input locations PASS (%zu attributes)\n", elements.size());
}
