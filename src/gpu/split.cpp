// MCLA GPU renderer -- the before/after split's cvars and the one function the renderer calls (split.h)

#include "split.h"

#include <atomic>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging.h>

namespace mcla::gpu::split {

namespace {

// Whether the last call saw the split on, for the log lines (the renderer asks at least once a frame, on one thread).
std::atomic<bool> g_was_active{false};

}  // namespace

std::optional<double> PositionFor(Effect effect) {
  const double position = larecomp::settings::mcla_gpu_split.Get();
  const bool active = Active(position);
  if (active != g_was_active.load(std::memory_order_relaxed)) {
    g_was_active.store(active, std::memory_order_relaxed);
    if (active) {
      REXLOG_INFO("[gpu] split: on at {:.3f} of the picture's width, effects: {} (the effect left of it)", position,
                  larecomp::settings::mcla_gpu_split_effects.Get());
    } else {
      REXLOG_INFO("[gpu] split: off (the effects are applied everywhere)");
    }
  }
  if (!active) {
    return std::nullopt;
  }
  if ((EffectsOf(larecomp::settings::mcla_gpu_split_effects.Get()) & uint32_t(effect)) == 0) {
    return std::nullopt;
  }
  return position;
}

}  // namespace mcla::gpu::split
