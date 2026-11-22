// MCLA GPU renderer -- the ray traced bounce light's setting: the flags the render thread sets for the pause menu's thread, and the setting's word as the render thread reads it. See gi_setting.h.

#include "gi_setting.h"

#include <atomic>

#include "settings/larecomp_settings.h"

#include "gpu.h"
#include "live_text.h"

namespace mcla::gpu {

namespace {

// Set by the integration when the probe update is built in (gi_integration.h SetUpdate); false until then.
std::atomic<bool> g_update_built{false};
// Set once a frame by the integration: the field stands and has taken the whole of the screen-space bounce light's share.
std::atomic<bool> g_replaced{false};

LiveText& Setting() {
  static LiveText text("mcla_gpu_rt_gi");
  return text;
}

}  // namespace

bool RtGiUpdateBuilt() { return g_update_built.load(std::memory_order_relaxed); }

bool RtGiReplacesBounceLight() { return g_replaced.load(std::memory_order_relaxed); }

namespace gi_setting {

void NoteUpdateBuilt(bool built) { g_update_built.store(built, std::memory_order_relaxed); }

void NoteReplaced(bool replaced) { g_replaced.store(replaced, std::memory_order_relaxed); }

Decision Current() {
  // Until the class is found (the Xenia renderer never finds it) a setting whose `auto` differs by class acts as on the weakest (gpu_class.h Known): the Low class and the tier none gate the field shut.
  const gc::GpuClass cls = gc::Known() ? gc::Current() : gc::GpuClass::kLow;
  return Decide(cls, gc::CurrentRtTier(), Setting().Get(), RtGiUpdateBuilt());
}

float BounceWeightNow() { return BounceWeight(larecomp::settings::mcla_gpu_rt_gi_bounces.Get()); }

}  // namespace gi_setting

}  // namespace mcla::gpu
