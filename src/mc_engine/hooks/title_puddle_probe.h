#pragma once

#include <cstdint>
#include <string_view>

#include "os/environment.h"

namespace mc::title_puddles {

enum class Arm { Off, Baseline, DryResponse, NoPlanar, NoProjected, NoGlows, FixedExposure, NoBloom };

constexpr Arm Parse(std::string_view value) {
  if (value == "baseline") return Arm::Baseline;
  if (value == "dry-response") return Arm::DryResponse;
  if (value == "no-planar") return Arm::NoPlanar;
  if (value == "no-projected") return Arm::NoProjected;
  if (value == "no-glows") return Arm::NoGlows;
  if (value == "fixed-exposure") return Arm::FixedExposure;
  if (value == "no-bloom") return Arm::NoBloom;
  return Arm::Off;
}

inline Arm ConfiguredArm() {
  static const Arm arm = Parse(larecomp::os::ReadEnvironment("TRP2_ARM").value_or(""));
  return arm;
}

// The normal dry value is documented in ssr.cpp. Leave the independent
// reflection-strength component, mirror plane and w alone in this arm.
constexpr void RoadParams(Arm arm, double& wetness) {
  if (arm == Arm::DryResponse) wetness = 0.095f;
}

// sub_8237AC90 takes the glow branch when r14 is zero and the projected
// branch otherwise. Jump to the same point increment as the game's rejects.
constexpr bool SkipVehiclePoint(Arm arm, uint32_t route) {
  return route == 0 ? arm == Arm::NoGlows : arm == Arm::NoProjected;
}

enum class Input { Other, Exposure, Bloom };

// Classify inputs for the tone-map A/B arms using the guest texture sizes.
constexpr Input ClassifyToneInput(bool vertex, bool pixel, uint32_t width, uint32_t height) {
  if (vertex && width == 1 && height == 1) return Input::Exposure;
  if (pixel && width == 320 && height == 180) return Input::Bloom;
  return Input::Other;
}

}  // namespace mc::title_puddles
