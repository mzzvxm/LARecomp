// MCLA GPU renderer -- an A/B inside one process. See ab.h.

#include "ab.h"

#include "settings/larecomp_settings.h"

namespace mcla::gpu::ab {

namespace detail {
bool g_b = true;
}

namespace {
bool g_enabled = false;
}

void Initialize() {
  g_enabled = larecomp::settings::mcla_gpu_ab_interleave.Get();
  detail::g_b = true;
}

bool Enabled() {
  return g_enabled;
}

void NextWindow() {
  if (g_enabled) {
    detail::g_b = !detail::g_b;
  }
}

}  // namespace mcla::gpu::ab
