#pragma once
// GI-BOUNCE-JOIN (gi_bounce_carrier.h, perf/GI-BOUNCE.md section JOIN): the four group switches as the group mask, read where they are used (live), for the screen-space half (ao.cpp PlanGi, scene.cpp's light pass).
// The ray traced half reads the same four settings in rt_world_frame.cpp (EmitterGroupsNow); gi_bounce_carrier_test.cpp holds that the two read the same ones.

#include "gi_bounce_carrier.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu::gi_bounce {

inline uint32_t GroupsNow() {
  namespace st = ::larecomp::settings;
  re::Switches s;
  s.headlights = st::mcla_gpu_gi_bounce_headlights.Get();
  s.neon = st::mcla_gpu_gi_bounce_neon.Get();
  s.rear = st::mcla_gpu_gi_bounce_rear.Get();
  s.other = st::mcla_gpu_gi_bounce_other.Get();
  return re::MaskOf(s);
}

}  // namespace mcla::gpu::gi_bounce
