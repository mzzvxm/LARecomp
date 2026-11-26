#pragma once

#include <string_view>

#include "gpu_class.h"

namespace mcla::gpu::upscale_defaults {

// FSR 3.1 Quality uses two thirds of the output per axis. Use a moderate RCAS
// step to recover detail at this ratio.
inline constexpr std::string_view kFsr31 = "fsr31";
inline constexpr std::string_view kQuality = "quality";
inline constexpr std::string_view kFsr31Sharpness = "0.4";
inline constexpr std::string_view kFsr1Sharpness = "0.8";  // reduction in stops, menu strength 0.6

enum class Path { kUnchanged, kFsr31, kFsr1 };

struct DisplayChoices {
  bool upscaler = false;
  bool render_scale = false;
  bool fsr1 = false;
  bool fsr1_preset = false;
};

constexpr Path Select(gpu_class::GpuClass cls, const DisplayChoices& chosen, bool fsr31_can_start) {
  // A saved display path includes explicit Off and scale 0 (follow the window).
  if (cls != gpu_class::GpuClass::kLow || chosen.upscaler || chosen.render_scale ||
      chosen.fsr1 || chosen.fsr1_preset) {
    return Path::kUnchanged;
  }
  return fsr31_can_start ? Path::kFsr31 : Path::kFsr1;
}

}  // namespace mcla::gpu::upscale_defaults
