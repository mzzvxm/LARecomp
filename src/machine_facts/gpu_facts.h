#pragma once

#include "document.h"

namespace larecomp::machine {

GpuFacts ReadGpuFacts(bool probe, const std::filesystem::path& folder);
void RefreshGpuFacts(GpuFacts& gpu);

}  // namespace larecomp::machine
