// MCLA GPU renderer -- an A/B inside one process. See ab.h.

#include "ab.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(
    mcla_gpu_ab_interleave, false, "MCLA/GPU",
    "Diagnostic (M6a): alternate the render thread's code under test between its old "
    "(A) and candidate (B) path every statistics window, and log each window's prepare time "
    "under its variant. Off: the candidate path always runs.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu::ab {

namespace detail {
bool g_b = true;
}

namespace {
bool g_enabled = false;
}

void Initialize() {
  g_enabled = REXCVAR_GET(mcla_gpu_ab_interleave);
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
