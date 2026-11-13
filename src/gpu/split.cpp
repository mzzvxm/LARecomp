// MCLA GPU renderer -- the before/after split's cvars and the one function the renderer calls (split.h)

#include "split.h"

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_split, -1.0, "MCLA/GPU",
    "TRAILER TOOLKIT: a before/after split in one frame. A position from 0 to 1 across the picture's width: the effects "
    "named by mcla_gpu_split_effects (ambient occlusion, the present's dither) are applied only left of it, and right of "
    "it the picture is drawn without them. -1 is off (the default; nothing changes and nothing costs), 1 is the effect "
    "everywhere. A scene's `split <a>..<b> over <s>` keys write it; a compositor draws the divider line. Live.")
    .debug_only()
    .range(-1.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(
    mcla_gpu_split_effects, "all", "MCLA/GPU",
    "TRAILER TOOLKIT: which effects mcla_gpu_split cuts: ao (ambient occlusion only), dither (the present's dither only) "
    "or all. An effect that is off in its own setting stays off. Used only while mcla_gpu_split is on. Live.")
    .allowed({"all", "ao", "dither"})
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::split {

namespace {

// Whether the last call saw the split on, for the log lines (the renderer asks at least once a frame, on one thread).
std::atomic<bool> g_was_active{false};

}  // namespace

std::optional<double> PositionFor(Effect effect) {
  const double position = REXCVAR_GET(mcla_gpu_split);
  const bool active = Active(position);
  if (active != g_was_active.load(std::memory_order_relaxed)) {
    g_was_active.store(active, std::memory_order_relaxed);
    if (active) {
      REXLOG_INFO("[gpu] split: on at {:.3f} of the picture's width, effects: {} (the effect left of it)", position,
                  REXCVAR_GET(mcla_gpu_split_effects));
    } else {
      REXLOG_INFO("[gpu] split: off (the effects are applied everywhere)");
    }
  }
  if (!active) {
    return std::nullopt;
  }
  if ((EffectsOf(REXCVAR_GET(mcla_gpu_split_effects)) & uint32_t(effect)) == 0) {
    return std::nullopt;
  }
  return position;
}

}  // namespace mcla::gpu::split
