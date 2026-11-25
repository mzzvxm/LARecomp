#ifndef LARECOMP_INTERIOR_CAMERA_RULES_HLSLI
#define LARECOMP_INTERIOR_CAMERA_RULES_HLSLI

// Shared with the host and the standalone unit test.
#ifdef __cplusplus
#include <cstdint>
namespace mcla::gpu::interior_camera {
using uint = uint32_t;
#define INTERIOR_RULE constexpr
#else
#define INTERIOR_RULE static
#endif

INTERIOR_RULE const uint kSsgiMatchColour = 1u << 25;
INTERIOR_RULE const uint kRtAoMatchGuide = 1u << 22;

#ifdef __cplusplus
constexpr bool IsCockpit(int rendered_rig) { return rendered_rig == 1; }

constexpr bool ApplyGiShield(bool cockpit_only, int rendered_rig) {
  return !cockpit_only || IsCockpit(rendered_rig);
}
#endif

INTERIOR_RULE uint SsgiSampleLevel(bool player_receiver, uint level) {
  return player_receiver ? 0u : level;
}

INTERIOR_RULE uint RtAoSampleOf(uint sample_index, uint guide_index, uint sample_id, uint guide_id,
                               float depth_delta, float depth_tolerance) {
  return sample_id == guide_id && depth_delta <= depth_tolerance ? sample_index : guide_index;
}

#ifdef __cplusplus
}  // namespace mcla::gpu::interior_camera
#endif
#undef INTERIOR_RULE
#endif
