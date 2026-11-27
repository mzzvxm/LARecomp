#pragma once

#include "settings/larecomp_settings.h"

namespace mcla::gpu::pbr_screen {

// Read the live Graphics setting for each pass that packs PBR state.
inline bool Enabled() {
  return larecomp::settings::mcla_gpu_pbr.Get();
}

}  // namespace mcla::gpu::pbr_screen
