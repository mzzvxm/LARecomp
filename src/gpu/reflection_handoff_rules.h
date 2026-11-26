#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

namespace mcla::gpu::reflection_handoff {

struct Weights {
  float screen, reuse, rt, planar, env;
};

// Each confidence is conditional on the preceding sources leaving weight.
constexpr Weights Resolve(float screen, float reuse, float rt, float planar) {
  screen = std::clamp(screen, 0.0f, 1.0f);
  const float after_screen = 1.0f - screen;
  reuse = after_screen * std::clamp(reuse, 0.0f, 1.0f);
  const float after_reuse = after_screen - reuse;
  rt = after_reuse * std::clamp(rt, 0.0f, 1.0f);
  const float after_rt = after_reuse - rt;
  planar = after_rt * std::clamp(planar, 0.0f, 1.0f);
  return {screen, reuse, rt, planar, after_rt - planar};
}

inline constexpr uint32_t kHistoryLevels = 6;

// ByteAddressBuffer layout shared by the graphics trace and the RT trace.
struct Frame {
  uint32_t enabled = 0;
  uint32_t env_word = 0;
  float exposure = 0.0f;
  uint32_t previous_levels = 0;
  uint32_t previous_colour = 0;
  uint32_t previous_size = 0;
  std::array<uint32_t, kHistoryLevels> previous_hiz{};
  std::array<float, 2> previous_tan{};
  float previous_near = 0.0f;
  float previous_exposure_scale = 1.0f;
  std::array<float, 4> previous_projection{};
  std::array<float, 12> motion{};
  uint32_t classify_flags = 0;
  uint32_t classify_history = 0;
  uint32_t classify_inputs = 0;
  uint32_t classify_group_mask = 0;
  uint32_t classify_stride = 0;
  uint32_t classify_plan = 0;
  std::array<float, 3> env_camera_delta{  std::array<float, 3> rt_bounds_min{};
  uint32_t rt_bounds_valid = 0;
  std::array<float, 3> rt_bounds_max{};
};
static_assert(sizeof(Frame) == 200 && offsetof(Frame, specular_ao) == 168);
static_assert(offsetof(Frame, rt_bounds_min) == 172 && offsetof(Frame, rt_bounds_valid) == 184 && offsetof(Frame, rt_bounds_max) == 188);
static_assert(offsetof(Frame, classify_flags) == 128 && offsetof(Frame, classify_stride) == 144);
static_assert(offsetof(Frame, env_camera_delta) == 152 && offsetof(Frame, env_has_origin) == 164);
inline constexpr uint32_t kClassifyEnabled = 1u;
inline constexpr uint32_t kClassifyHistory = 2u;
inline constexpr uint32_t kClassifySparse = 4u;
static_assert(offsetof(Frame, previous_hiz) == 24 && offsetof(Frame, previous_tan) == 48 &&
              offsetof(Frame, previous_projection) == 64 && offsetof(Frame, motion) == 80);

constexpr bool ReuseHistory(bool enabled, bool adjacent, bool motion, bool size, bool lens, bool products) {
  return enabled && adjacent && motion && size && lens && products;
}

inline constexpr float kReuseMaximum = 0.55f;
inline constexpr float kReuseFacingCutoff = 0.15f;

constexpr float ReuseTrust(float edge, float facing, float depth_error) {
  return kReuseMaximum * std::clamp(edge, 0.0f, 1.0f) *
         std::clamp((facing - kReuseFacingCutoff) / 0.35f, 0.0f, 1.0f) *
         (1.0f - std::clamp(depth_error, 0.0f, 1.0f));
}

constexpr float ReuseAgreement(float normal_dot, float depth_error) {
  const float error = depth_error < 0.0f ? -depth_error : depth_error;
  return std::clamp((normal_dot - 0.80f) / 0.20f, 0.0f, 1.0f) * (1.0f - std::clamp(error, 0.0f, 1.0f));
}

// The thinner the support around a hit, the less tolerated depth penetration.
constexpr float Thickness(float depth, float relative, float support) {
  const float thick = std::max(0.3f, relative * depth);
  return thick * (0.25f + 0.75f * std::clamp(support, 0.0f, 1.0f));
}

}  // namespace mcla::gpu::reflection_handoff
