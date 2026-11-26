#pragma once

#include <cstdint>

namespace mcla::gpu::rt_denoise::v2 {

inline constexpr uint32_t kFeatures = 30;
inline constexpr uint32_t kControl = 31;
inline constexpr uint32_t kIterationShift = 16;
inline constexpr uint32_t kHasHistory = 1u << 19;
inline constexpr uint32_t kFinal = 1u << 20;
inline constexpr uint32_t kMaxIterations = 5;
inline constexpr uint32_t kFeatureBytes = 16;

// The two widest stages are useful on diffuse bounce and rough reflections.
// Visibility has smaller penumbrae and RGB packs three independent lights.
inline constexpr uint32_t kDiffuseIterations = 5;
inline constexpr uint32_t kAmbientIterations = 4;
inline constexpr uint32_t VisibilityIterations(uint32_t traced_step) { return traced_step == 1 ? 4 : 3; }
inline constexpr uint32_t ReflectionIterations(bool ultra, int override_iterations) {
  return override_iterations > 0 ? uint32_t(override_iterations) : (ultra ? 5 : 4);
}

}  // namespace mcla::gpu::rt_denoise::v2
