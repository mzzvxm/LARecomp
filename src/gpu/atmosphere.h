#pragma once

#include <cstdint>

#include "edram.h"

namespace mcla::gpu {

// The resolved shaft texture and the target it is meant to light.
struct AtmosphereShafts {
  EdramKey colour;
  uint32_t width;
  uint32_t height;
  uint32_t samples;
  uint32_t slot;
  float tint[3];
};

}  // namespace mcla::gpu
