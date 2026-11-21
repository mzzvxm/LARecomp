#pragma once
// ===========================================================================
// MCLA GPU renderer -- the volumetric light's headlights: the rig's set and the shadow atlas, in the volume's units (perf/VOLUMETRICS-SURVEY.md V9)
// ===========================================================================
// Pure (no guest, no renderer, no GPU): volumetrics_inputs_test.cpp checks it with the real headers of the headlight rig and the shadow replay
// (src/gpu/headlight_types.h, local_shadow_types.h, headlight_rig_rules.h) and, on a tree that lacks them, with their field-for-field mirrors.
//
// THE HEADLIGHTS (headlight_types.h): HeadlightSet = up to four Headlight {pos, dir, cos_outer, cos_inner, range, color (already times the
// strength, the fade and the night factor), car_key, is_player}, the player's first. volumetrics_types.h FillHeadlights makes them VolLamps (a
// light that fades is a dimmer one: the colour goes to zero). What it leaves is the UNITS: the lit shaders' headlight term is
//   kPeak x cone x 1 / (1 + (d / D)^2) x window x ...     (headlight_rig_rules.h: kPeak 2.0, kFalloffDistance D = 26 m since the rig's calibration of 2026-11-20, HEADLIGHT-FIX-2;
//                                                         the first look was 2.4 and 9 m: volumetrics_inputs_test.cpp holds the two constants against the rig's)
// which is colour x kPeak x D^2 / (d^2 + D^2): the volume's own law with the near radius squared D^2 and a colour of kPeak x D^2 times the set's. ApplyUnits does
// exactly that, so that the beam in the air and the pool on the road are the same light (volumetrics_lamps.h says the same for the street lamps).
//
// THE SHADOW ATLAS (local_shadow.h, local_shadow_types.h). The replay draws the main view's opaque draws again from each lit car's lamps into
// a depth atlas (D32 float + S8, a bindless slot whose view is the depth plane as one float), at the first point of the frame, BEFORE the volume's passes
// (Scene::OnResolve: car shadow, headlight replay, occlusion, sun shafts, volumetric light), so the volume reads THIS frame's maps. What it hands over:
//   LocalShadow::AtlasInfo()  {valid, slot, width, height, tile}: the atlas as a bindless texture and its size in texels
//   LocalShadow::Frame()      LocalShadowFrame {views[8], count}: one LocalShadowView per LAMP (a car has two, one for each headlamp, each with its own tile; the
//                             light's share is the mean of its two: VolLamp::shadow_view and shadow_view_b; a car with one view has one): world_to_shadow
//                             (column vectors, row-major: o = M (p, 1); the atlas uv is o.xy / o.w and the depth o.z / o.w), atlas_rect = u0 v0 u1 v1 (the tile's
//                             far corner, NOT its size: the comment of volumetrics_types.h VolShadowView says "u offset, v offset, u size, v size", which is the replay
//                             rules' internal form; the published one is the corner and the shaders of both consumers read it so), depth_bias, texel_size (a share
//                             of the tile), light_index (the headlight of the set), valid.
// ATLAS STATE, which the compute pass depends on (the V9 answer):
//   * the replay used to leave the atlas in SHADER_READ with a barrier of stage GRAPHICS only (local_shadow.cpp, after the pass): on D3D12 that is PIXEL_SHADER_RESOURCE,
//     which a compute shader may not read (NON_PIXEL_SHADER_RESOURCE is the compute state; the debug layer says so); on Vulkan the barrier's destination stage was
//     the fragment shader only. The volume's inject is a compute pass, so the replay's last barrier now names both stages (GRAPHICS | COMPUTE: D3D12
//     ALL_SHADER_RESOURCE, Vulkan fragment and compute): `list->barriers(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE, read)`.
//     The volume cannot do it itself: LocalShadow hands out the slot and not the texture, and Plume's D3D12 barrier needs the texture.
//   * the descriptor: the bindless heap's texture set (the same Texture2D<float4>[] the inject reads the history through); the depth plane reads as `.x`.
//   * a point sampler's compare is the reader's (the inject uses Load and weights four compares by the texel fraction): no sampler slot is used.
// The set and the maps are consistent within the frame (the set does not change until the next present, the maps were made for it); one frame of latency
// against the car's position is the replay's (survey finding 4), and a moving beam is moved ahead by the rig's velocity.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "volumetrics_types.h"

namespace mcla::gpu::volumetrics {

// The rig's look (headlight_rig_rules.h kPeak and kFalloffDistance): the volume's headlight term has to be the same light.
inline constexpr float kHeadlightPeak = 2.0f;
inline constexpr float kHeadlightFalloffDistance = 26.0f;

// A VolLamp made from a Headlight (FillHeadlights) in the volume's units: colour x kPeak x D^2 and the near radius squared D^2 (volumetrics_lamps.h
// LampNearSq: pad[0]), so that its term is the lit shaders' kPeak / (1 + (d / D)^2) for any distance.
inline void ApplyHeadlightUnits(VolLamp* l, float units = 1.0f) {
  constexpr float d2 = kHeadlightFalloffDistance * kHeadlightFalloffDistance;
  for (int i = 0; i < 3; ++i) {
    l->colour[i] *= kHeadlightPeak * d2 * units;
  }
  l->pad[0] = d2;
}

// The atlas as LocalShadow::AtlasInfo() gives it.
struct ShadowAtlasState {
  bool valid = false;
  uint32_t slot = kNoSlot;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Whether a published view can be read as it is: finite numbers, a tile inside the atlas (u0 < u1, v0 < v1, all in 0..1), a bias and a texel size that
// make sense.
inline bool UsableView(const VolShadowView& v) {
  if (v.valid == 0) {
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(v.world_to_shadow[i])) {
      return false;
    }
  }
  const float* r = v.atlas_rect;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(r[i]) || r[i] < 0.0f || r[i] > 1.0f) {
      return false;
    }
  }
  return r[0] < r[2] && r[1] < r[3] && std::isfinite(v.depth_bias) && v.depth_bias >= 0.0f && v.depth_bias < 0.5f && std::isfinite(v.texel_size);
}

// The frame's LocalShadowFrame and the atlas into `out`: FillShadowViews (volumetrics_types.h) first, then the views that cannot be read are dropped and
// the lights that named them are lit (a light with no usable view takes no lookup). The headlights must be in `out` already. `frame` null, an atlas that
// is not valid, or no slot: no shadows at all.
template <class LocalShadowFrameT>
void FillShadows(const LocalShadowFrameT* frame, const ShadowAtlasState& atlas, VolInputs* out) {
  const bool usable = frame != nullptr && atlas.valid && atlas.slot != kNoSlot && atlas.width != 0 && atlas.height != 0 && frame->count != 0;
  FillShadowViews(usable ? frame : static_cast<const LocalShadowFrameT*>(nullptr), usable ? atlas.slot : kNoSlot, float(atlas.width), float(atlas.height), out);
  if (!usable) {
    return;
  }
  for (uint32_t i = 0; i < out->shadow_view_count; ++i) {
    if (UsableView(out->shadow_views[i])) {
      continue;
    }
    out->shadow_views[i].valid = 0;
    for (uint32_t j = 0; j < out->headlight_count; ++j) {
      if (out->headlights[j].shadow_view == int32_t(i)) {
        out->headlights[j].shadow_view = -1;
      }
      if (out->headlights[j].shadow_view_b == int32_t(i)) {
        out->headlights[j].shadow_view_b = -1;
      }
    }
  }
  bool any = false;
  for (uint32_t j = 0; j < out->headlight_count; ++j) {
    VolLamp& light = out->headlights[j];
    if (light.shadow_view < 0 && light.shadow_view_b >= 0) {
      // Only the car's second lamp has a view that can be read: it is the light's one.
      light.shadow_view = light.shadow_view_b;
      light.shadow_view_b = -1;
    }
    any = any || light.shadow_view >= 0;
  }
  if (!any) {
    // Not one light reads a view: nothing to bind (the shader's flag stays off and the atlas is not touched).
    out->shadow_view_count = 0;
    out->shadow_atlas_slot = kNoSlot;
  }
}

}  // namespace mcla::gpu::volumetrics
