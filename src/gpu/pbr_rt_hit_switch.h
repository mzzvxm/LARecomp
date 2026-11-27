#pragma once

#include "pbr_screen_switch.h"

namespace mcla::gpu::pbr_rt {

// The car and world hit frame writers copy the setting into kFramePbr.
inline bool HitShadingEnabled() {
  return pbr_screen::Enabled();
}

}  // namespace mcla::gpu::pbr_rt
