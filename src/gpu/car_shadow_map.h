#pragma once
// ===========================================================================
// MCLA GPU renderer -- a sun shadow map of the player's car alone
// ===========================================================================
// The game's sun shadow map holds the player's car at a forced low level of detail in cascades whose texels are 1 to 25 cm:
// the wing, the mirrors, the sills and the wheels cast no shadow of their own shape on the car or on the road near it, and
// the screen-space contact shadows (ao_contact_ps.hlsl) cannot see a part's thickness. This map draws the player's car again,
// depth only, from the sun, at full detail, with the geometry its own main-view draws used (car_frame.h), into a small square
// map fitted to the car. The contact shadows pass reads it.
//
// CONTRACT (2026-11-18): feat/car-shadow-map implements CarShadowMap (car_shadow_map.cpp: the replay, the fit, the debug
// view, the setting); feat/car-shadow-apply reads View() in the AO's contact pass. The view struct below is fixed; the class
// may grow.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu {

struct CarShadowMapView {
  bool valid = false;           // a map of THIS frame's player car was recorded on the list before the AO's passes
  uint32_t slot = 0xFFFFFFFFu;  // bindless texture slot of the map: one float channel, depth from the sun (0 nearest the sun,
                                // 1 farthest), 1 where no part of the car is
  uint32_t size = 0;            // texels per side (square)
  // VIEW space (the AO's: x right, y up, z forward = the linear depth t; ao_common.hlsli AoViewPos) -> the map:
  //   u = dot(float4(p, 1), to_map[0]), v = dot(float4(p, 1), to_map[1]), depth = dot(float4(p, 1), to_map[2]),
  // u and v in [0, 1] across the map (v down), depth in [0, 1] as stored. A point is in the car's shadow when the stored depth
  // at (u, v) is smaller than its own depth by more than a bias.
  float to_map[3][4] = {};
  // The same three rows in a bindless 4x1 R32G32B32A32_FLOAT texture (texel k = to_map[k], texel 3 = (texel_m, depth_m, size,
  // 1)), for a pass whose push block has no room for a matrix. 0xFFFFFFFF: none.
  uint32_t matrix_slot = 0xFFFFFFFFu;
  float texel_m = 0.0f;  // a texel's side in metres
  float depth_m = 0.0f;  // the map's depth range in metres (a stored depth difference times this = metres)
};

// The implementer adds the class here: Initialize/Shutdown, a per-frame entry the Scene calls at the history trigger BEFORE the
// ambient occlusion's passes (it records the map on that list), the setting, and:
//   const CarShadowMapView& View() const;   // the map of this frame; valid == false when none was drawn
class CarShadowMap;

}  // namespace mcla::gpu
