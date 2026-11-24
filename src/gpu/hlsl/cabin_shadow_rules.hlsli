#ifndef LARECOMP_CABIN_SHADOW_RULES_HLSLI
#define LARECOMP_CABIN_SHADOW_RULES_HLSLI

// Shared by the shaders, the host and the CPU test.
#ifdef __cplusplus
#include <cstdint>
namespace mcla::gpu::cabin_shadow {
using uint = uint32_t;
#define CABIN_RULE constexpr
#else
#define CABIN_RULE static
#endif

CABIN_RULE const uint kCabinMarch = 1u;
CABIN_RULE const uint kCabinMap = 2u;
CABIN_RULE const uint kCabinRt = 3u;
CABIN_RULE const uint kCabinFill = 4u;
CABIN_RULE const uint kCabinRtFlag = 1u << 2;
CABIN_RULE const uint kCabinApplyIdShift = 9u;
// Bits 0–2 of the depth already carry the vehicle flag and the normal's source sample.
CABIN_RULE const uint kCabinDepthBit = 1u << 3;

#ifdef __cplusplus
constexpr uint CabinMode(int mode) {
  return mode >= 0 && mode <= int(kCabinFill) ? uint(mode) : kCabinMap;
}

constexpr uint CabinPlayerId(int camera_rig, bool paused, bool car_valid, uint player_id) {
  return camera_rig == 1 && !paused && car_valid && player_id >= 1u && player_id <= 23u ? player_id : 0u;
}

constexpr uint CabinSourceMode(uint mode) {
  return mode <= kCabinRt ? mode : 0u;
}
#endif

CABIN_RULE uint CabinMarkDepth(uint depth, uint player_id, uint stencil_id) {
  if (player_id == 0u || stencil_id == 0u) {
    return depth;
  }
  return (depth & ~kCabinDepthBit) | (stencil_id == player_id ? kCabinDepthBit : 0u);
}

CABIN_RULE bool CabinMarkedDepth(uint depth) {
  const uint mask = kCabinDepthBit | 1u;
  return (depth & mask) == mask;
}

CABIN_RULE bool CabinCheckMarch(uint mode, uint depth) {
  return mode == kCabinMarch && CabinMarkedDepth(depth);
}

CABIN_RULE bool CabinSkipMap(uint mode, uint depth, bool map_debug_view) {
  return mode == kCabinMap && CabinMarkedDepth(depth) && !map_debug_view;
}

CABIN_RULE uint CabinSunMask(uint flags, uint depth, uint rays, uint player_parts) {
  return (flags & kCabinRtFlag) != 0u && CabinMarkedDepth(depth) ? rays & ~player_parts : rays;
}

CABIN_RULE uint CabinApplyId(uint sample_and_lookup) {
  return (sample_and_lookup >> kCabinApplyIdShift) & 31u;
}

CABIN_RULE float CabinFillVisibility(float visibility, uint player_id, uint stencil_id) {
  // Restore 75% of the removed sun. A fully lit pixel keeps the same visibility.
  return player_id != 0u && stencil_id == player_id ? visibility + 0.75f * (1.0f - visibility) : visibility;
}

#ifdef __cplusplus
}  // namespace mcla::gpu::cabin_shadow
#endif
#undef CABIN_RULE
#endif
