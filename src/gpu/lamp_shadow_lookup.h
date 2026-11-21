#pragma once
// ===========================================================================
// MCLA GPU renderer -- a street lamp's shadow lookup: the ONE lookup (light_shadow_lookup.h) over the lamps' table, plus the edge fade (perf/LAMP-SHADOWS-2.md)
// ===========================================================================
// The reference the HLSL twin (hlsl/lamp_shadow_lookup.hlsli) is held to. A lamp's light names its entries of the lamps' table (LampShadowFrame) in the pass's combined entry space
// (ShadowRef::first_view = kLampEntryBase + the row); LampLit takes that number, finds the rows, and applies THE ONE COMBINE RULE (static x live layers, views averaged, the share blended
// by the light's weight) through shadow_lookup::LightLit, with one thing a car's view has no need of: the shadow ENDS at the map's far plane (the lamp's reach, 14 to 30 m), so a shadow's
// edge there would show as a line across the road. The visibility is therefore faded to 1 over the last fifth of the reach, by the distance from the lamp:
//
//     lit = Blend( lerp(1, mean over the lamp's layers, edge), weight )            edge = 1 - smoothstep(kLampFadeFrom x reach, reach, |p - lamp|)
//
// Pure: no renderer, no guest. Namespace larecomp::gpu::shadow_lookup.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "lamp_shadow_frame.h"
#include "light_shadow_lookup.h"

namespace larecomp::gpu::shadow_lookup {

// 1 inside kLampFadeFrom x reach of the lamp, 0 at the reach and beyond, a smoothstep between. A reach of 0 (not known) fades nothing.
inline float LampEdgeFade(float distance, float reach) {
  if (!(reach > 0.0f)) {
    return 1.0f;
  }
  const float a = kLampFadeFrom * reach;
  const float t = std::clamp((distance - a) / (reach - a), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The visibility of one lamp at the world point `p`: `ref.first_view` is the combined entry number (kLampEntryBase + row), `lamp` the lamp's place. 1 = lit. A light that is not an
// atlas light, has no views, or names an entry below the lamps' table is lit.
template <class Depth>
inline float LampLit(const LampShadowFrame& frame, const ShadowRef& ref, const float lamp[3], const float p[3], Mode mode, Depth&& depth_at) {
  if (ref.kind != ShadowKind::kAtlas || ref.view_count == 0 || uint32_t(ref.first_view) < kLampEntryBase) {
    return 1.0f;
  }
  ShadowRef local = ref;
  local.first_view = uint8_t(uint32_t(ref.first_view) - kLampEntryBase);
  local.weight = 1.0f;  // exact mean; the weight is blended last
  if (uint32_t(local.first_view) >= frame.count) {
    return 1.0f;
  }
  float mean = LightLit(frame, local, p, mode, depth_at);
  const float reach = LampReach(frame.views[local.first_view]);
  const float d[3] = {p[0] - lamp[0], p[1] - lamp[1], p[2] - lamp[2]};
  const float edge = LampEdgeFade(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), reach);
  if (edge < 1.0f) {
    mean = 1.0f + (mean - 1.0f) * edge;
  }
  return Blend(mean, ref.weight);
}

}  // namespace larecomp::gpu::shadow_lookup
