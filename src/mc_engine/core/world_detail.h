#pragma once
// The WORLD DETAIL numbers as the game's hooks apply them and the pause rows show them: the traffic LOD scale, the traffic range and
// the pedestrian and parked car densities, whose `auto` (the default) is a number chosen by the GPU class (gpu/gpu_class.h kScales; the
// city LOD scale is the fifth and has its own functions in hooks/graphics.cpp). One place, so that what a row says `auto` comes to is
// what the hooks apply.
//
// With the native renderer the class is the one Plume found (gpu_class::ScaleInForce); in a build without it, and until the class
// is known (always with the Xenia renderer), `auto` is the Low cell, which is what these settings were before the class chose them.

#include <string>

#include "gpu/gpu_class.h"

namespace mcla::world_detail {

using mcla::gpu::gpu_class::Scale;

// The value to apply for the setting's own value: the value itself, or what `auto` comes to. Any thread, cheap.
inline double InForce(Scale scale, double user_value) {
#if defined(LARECOMP_HAS_GPU)
  return mcla::gpu::gpu_class::ScaleInForce(scale, user_value);
#else
  return mcla::gpu::gpu_class::ScaleFor(scale, false, mcla::gpu::gpu_class::GpuClass::kLow, user_value);
#endif
}

// The pause row's label for the setting's value: "AUTO (3X)", "AUTO (400M)" or the value ("2X", "250M").
inline std::string Label(Scale scale, double user_value) {
#if defined(LARECOMP_HAS_GPU)
  return mcla::gpu::gpu_class::ScaleLabel(scale, user_value);
#else
  return mcla::gpu::gpu_class::ScaleLabelFor(scale, false, mcla::gpu::gpu_class::GpuClass::kLow, user_value);
#endif
}

}  // namespace mcla::world_detail
