#pragma once
// ===========================================================================
// MCLA GPU renderer -- conversions between today's light shapes and the one Light (perf/LIGHT-UNIFY.md 7.1 step U1)
// ===========================================================================
// FROM the producers that exist today INTO a Light:
//   LightFromHeadlight   the headlight rig's `Headlight` (headlight_types.h; a template, so this header needs no rig include: any type with those members converts,
//                        and `lamp_offset` where the type has it, as on feat/hl-per-lamp)
//   LightFromGameLamp    a record of the game's per-frame lamp list (vol_lamp_list.h GameLamp), in the game's units, as the volume's MakeVolLamp reads it
//   LightFromModLight    a record of a mod's lights.bin (mod_lights.cpp), the same lamp under the mod's own id
//   MakeOmniLight        a police point, a neon light: an omni light of a plateau and a range
//   LightFromVolLamp     the volume's own VolLamp (the reverse of ToVolLamp, for self-checks)
//   ShadowFrameFromLocal the shadow replay's LocalShadowFrame (local_shadow_types.h), its views grouped by light as a light's entries have to be
// FROM a Light INTO a consumer's shape (the projections of LU-4: they must equal what the consumer makes today):
//   ToVolLamp            the volume's lamp: THE SAME four rows (VolLamp is a 64 byte twin of GpuLight's data), its colour in the volume's units and the weight folded in
//   ToVolShadowView      the volume's VolShadowView of a LocalShadowView (the same 96 bytes, field for field)
// and the CarLights of a vehicle's light points (NoteCarPoint).
//
// Templates over "any type with those members" wherever a producer's struct is not in a pure header (the convention of light_set.h and volumetrics_types.h), so this file needs
// nothing but light_frame.h, light_id.h and light_laws.h; the real structs of main are in light_frame_test.cpp's round trips. No behaviour: nothing in the game calls any of it yet.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"

namespace larecomp::gpu {

// ---------------------------------------------------------------------------
// into a Light
// ---------------------------------------------------------------------------

// The headlight rig's `Headlight` (headlight_types.h: pos, dir, cos_outer, cos_inner, range, color, car_key, is_player; and lamp_offset on feat/hl-per-lamp) as a Light: a spot of
// law kHeadlight with the rig's D, source kHeadlight, the car's pair id, every role, no shadow. `color` is already times the strength, the fade and the night factor.
template <class HeadlightT>
inline Light LightFromHeadlight(const HeadlightT& h) {
  Light l;
  for (int k = 0; k < 3; ++k) {
    l.pos[k] = h.pos[k];
    l.dir[k] = h.dir[k];
    l.color[k] = h.color[k];
  }
  l.range = h.range;
  l.cos_outer = h.cos_outer;
  l.cos_inner = h.cos_inner;
  l.law_a = light_laws::kHeadlightFalloffDistance;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  l.law = LightLaw::kHeadlight;
  l.roles = kRoleMaskAll;
  l.car_key = h.car_key;
  l.flags = h.is_player ? kLightFlagPlayer : 0u;
  l.id = light_id::CarLampId(h.car_key, light_id::CarLamp::kPair);
  l.shadow.reach = h.range;
  if constexpr (requires { h.lamp_offset[0]; }) {
    for (int k = 0; k < 3; ++k) {
      l.offset[k] = h.lamp_offset[k];
    }
  }
  return l;
}

// A lamp in the game's units (the game's list, a mod's lights.bin): colour (0..1) x intensity x the prop pass's fade is the Light's colour, the law is the game's with I as its
// plateau, the range is the reach sqrt(100 I) kept between 8 and 120 m, the cone is the game's (ConeCosines) or none for an omni light (type 0, an angle of 360 or no axis).
// The arithmetic is volumetrics_lamps.h MakeVolLamp's, so ToVolLamp of this is that lamp bit for bit. `on` is not read (the volume's picker never did either). The game's own
// shaders light with it (kLightFlagGameLit, kRoleMaskGameLit).
inline Light MakeGameLampLight(const float pos[3], const float colour[3], const float dir[3], float intensity_in, float cone_deg, float spread_deg, float scale, uint8_t type,
                               LightSource source, uint64_t id) {
  Light l;
  const float intensity = std::max(intensity_in, 0.0f);
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = pos[i];
    l.color[i] = std::max(colour[i], 0.0f) * intensity * std::clamp(scale, 0.0f, 1.0f);
  }
  l.range = light_laws::LampRange(intensity);
  const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (len > 1e-3f) {
    for (int i = 0; i < 3; ++i) {
      l.dir[i] = dir[i] / len;
    }
  } else {
    l.dir[0] = 0.0f;
    l.dir[1] = -1.0f;
    l.dir[2] = 0.0f;
  }
  if (type == 0 || cone_deg >= 359.0f || len <= 1e-3f) {
    l.cos_outer = -1.0f;  // a point light: no cone
    l.cos_inner = -1.0f;
    l.kind = LightKind::kPoint;
    l.law = LightLaw::kOmni;
  } else {
    light_laws::ConeCosines(cone_deg, spread_deg, &l.cos_outer, &l.cos_inner);
    l.kind = LightKind::kSpot;
    l.law = LightLaw::kGameLamp;
  }
  l.law_a = intensity;
  l.source = source;
  l.roles = kRoleMaskGameLit;
  l.flags = kLightFlagGameLit;
  l.id = id;
  l.shadow.reach = light_laws::LampShadowReach(intensity);
  return l;
}

// A record of the game's lamp list (vol_lamp_list.h GameLamp: pos, colour, dir, intensity, cone_deg, spread_deg, scale, type), id = the lamp's place (light_id.h LampId).
template <class GameLampT>
inline Light LightFromGameLamp(const GameLampT& g) {
  return MakeGameLampLight(g.pos, g.colour, g.dir, g.intensity, g.cone_deg, g.spread_deg, g.scale, uint8_t(g.type), LightSource::kLamp, light_id::LampId(g.pos));
}

// A record of a mod's lights.bin (mod_lights.cpp: position, colour, direction, intensity, cone angle, cone spread, u32 id, type): the same lamp, source kMod, id from the mod's key
// and the record's own id. The record has no fade (scale 1).
template <class ModLightT>
inline Light LightFromModLight(const ModLightT& m, uint32_t mod_key) {
  return MakeGameLampLight(m.pos, m.colour, m.dir, m.intensity, m.cone_deg, m.spread_deg, 1.0f, uint8_t(m.type), LightSource::kMod, light_id::ModLampId(mod_key, m.id));
}

// An omni light with no game record (a police point, a neon light): `colour` x `level` (0..1, what the point's intensity says), the game's distance law with `plateau_sq` as the
// plateau's radius squared, out to `range`. Every role: the game does not light with it.
inline Light MakeOmniLight(LightSource source, const float pos[3], const float colour[3], float level, float plateau_sq, float range, uint64_t id) {
  Light l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = pos[i];
    l.color[i] = std::max(colour[i], 0.0f) * std::max(level, 0.0f);
  }
  l.range = range;
  l.cos_outer = -1.0f;
  l.cos_inner = -1.0f;
  l.kind = LightKind::kPoint;
  l.law = LightLaw::kOmni;
  l.law_a = plateau_sq;
  l.source = source;
  l.roles = kRoleMaskAll;
  l.id = id;
  l.shadow.reach = range;
  return l;
}

// ---------------------------------------------------------------------------
// the volume's lamp
// ---------------------------------------------------------------------------

// A Light as the volume's lamp (volumetrics_types.h VolLamp: pos, range, dir, cos_outer, colour, cos_inner, shadow_view, pad[0] = the near radius squared and, on
// feat/hl-per-lamp, shadow_view_b). The colour is the Light's in the volume's units (light_laws.h VolumeUnits: a headlight's kPeak x D^2) times `units` (a multiplier: 1 is the
// calibrated one) and times the volume's own weight (`vol_weight`, applied last as the lamp picker does). The shadow views are the entries of the ShadowFrame the Light names:
// the first, and the second when it has two (a car's two headlamps, or a lamp's static and live layers). A VolLampT that is not a VolLamp but has those members works too.
template <class VolLampT>
inline VolLampT ToVolLamp(const Light& l, float units = 1.0f) {
  VolLampT v{};
  const float mul = light_laws::VolumeUnits(l) * units;
  for (int i = 0; i < 3; ++i) {
    v.pos[i] = l.pos[i];
    v.dir[i] = l.dir[i];
    v.colour[i] = l.color[i] * mul * l.vol_weight;
  }
  v.range = l.range;
  v.cos_outer = l.cos_outer;
  v.cos_inner = l.cos_inner;
  const bool shadowed = l.shadow.kind == ShadowKind::kAtlas && l.shadow.view_count >= 1;
  v.shadow_view = shadowed ? int32_t(l.shadow.first_view) : -1;
  if constexpr (requires { v.shadow_view_b; }) {
    v.shadow_view_b = shadowed && l.shadow.view_count >= 2 ? int32_t(l.shadow.first_view) + 1 : -1;
  }
  v.pad[0] = light_laws::VolumeNearSq(l);
  return v;
}

// The reverse, for the manager's self-check and the tests: the Light a VolLamp stands for under `law` (a headlight's D comes from the near radius, its colour is divided by
// kPeak x D^2; a lamp's I is the near radius squared over the share). Lossy as a projection is: no id, no car, no weight (1), no velocity.
template <class VolLampT>
inline Light LightFromVolLamp(const VolLampT& v, LightLaw law, LightSource source) {
  Light l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = v.pos[i];
    l.dir[i] = v.dir[i];
    l.color[i] = v.colour[i];
  }
  l.range = v.range;
  l.cos_outer = v.cos_outer;
  l.cos_inner = v.cos_inner;
  l.kind = v.cos_outer <= -0.999f ? LightKind::kPoint : LightKind::kSpot;
  l.law = law;
  l.source = source;
  const float near_sq = v.pad[0];
  if (law == LightLaw::kHeadlight) {
    const float d_fall = near_sq > 0.0f ? std::sqrt(near_sq) : light_laws::kHeadlightFalloffDistance;
    l.law_a = d_fall;
    const float units = light_laws::kHeadlightPeak * d_fall * d_fall;
    for (int i = 0; i < 3; ++i) {
      l.color[i] = v.colour[i] / units;
    }
  } else {
    l.law_a = near_sq / light_laws::kLampNearShare;
    l.roles = kRoleMaskGameLit;
    l.flags = kLightFlagGameLit;
  }
  if (v.shadow_view >= 0) {
    l.shadow.kind = ShadowKind::kAtlas;
    l.shadow.first_view = uint8_t(v.shadow_view);
    l.shadow.view_count = 1;
    if constexpr (requires { v.shadow_view_b; }) {
      if (v.shadow_view_b >= 0) {
        l.shadow.view_count = 2;
      }
    }
  }
  return l;
}

// ---------------------------------------------------------------------------
// the shadow views
// ---------------------------------------------------------------------------

// The volume's VolShadowView of a LocalShadowView: the same 96 bytes, every field copied (volumetrics_types.h FillShadowViews does exactly this).
static_assert(sizeof(LocalShadowView) == 96, "a LocalShadowView is 96 bytes: the volume's VolShadowView and the lit shaders' block read it field for field");
template <class VolShadowViewT>
inline VolShadowViewT ToVolShadowView(const LocalShadowView& v) {
  VolShadowViewT d{};
  for (int k = 0; k < 16; ++k) {
    d.world_to_shadow[k] = v.world_to_shadow[k];
  }
  for (int k = 0; k < 4; ++k) {
    d.atlas_rect[k] = v.atlas_rect[k];
  }
  d.depth_bias = v.depth_bias;
  d.texel_size = v.texel_size;
  d.light_index = v.light_index;
  d.valid = v.valid;
  return d;
}

// The shadow replay's frame (local_shadow_types.h LocalShadowFrame: `views`, `count`; each view names the rig's light by `light_index`) as a ShadowFrame: `light_ids[i]` is the
// id of the light the rig calls i (`light_count` of them), every view is of that light, kSingle. The views are GROUPED BY LIGHT, as a light's entries must be consecutive: the
// valid views of the first light come first (at most two of them), then the next light's, and so on, then the views that count for nobody (invalid, or a third for a light), in
// their order; LocalShadowView::light_index is untouched (the legacy projections keep the rig's index). `atlas` is the atlas's info as the replay published it.
template <class LocalShadowFrameT>
inline ShadowFrame ShadowFrameFromLocal(const LocalShadowFrameT& local, const ShadowAtlasInfo& atlas, const uint64_t* light_ids, uint32_t light_count) {
  ShadowFrame out;
  out.atlas = atlas;
  const uint32_t in_count = std::min<uint32_t>(uint32_t(local.count), kMaxLightShadowViews);
  std::array<bool, kMaxLightShadowViews> taken{};
  const auto id_of = [&](uint32_t light_index) { return light_index < light_count && light_ids != nullptr ? light_ids[light_index] : kNoLightId; };
  std::array<uint32_t, kMaxLightShadowViews> grouped{};  // the rig indices of the lights that have their group already
  uint32_t grouped_count = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < in_count; ++i) {
    if (taken[i] || local.views[i].valid == 0) {
      continue;
    }
    const uint32_t light = local.views[i].light_index;
    if (std::find(grouped.begin(), grouped.begin() + grouped_count, light) != grouped.begin() + grouped_count) {
      continue;  // a third view of a light that has two: it counts for nobody
    }
    grouped[grouped_count++] = light;
    uint32_t in_group = 0;
    for (uint32_t j = i; j < in_count && in_group < 2; ++j) {
      if (!taken[j] && local.views[j].valid != 0 && local.views[j].light_index == light) {
        taken[j] = true;
        out.views[n] = local.views[j];
        out.view_light[n] = id_of(light);
        out.view_layer[n] = ShadowLayer::kSingle;
        ++n;
        ++in_group;
      }
    }
  }
  for (uint32_t i = 0; i < in_count; ++i) {
    if (!taken[i]) {
      out.views[n] = local.views[i];
      out.view_light[n] = kNoLightId;
      out.view_layer[n] = ShadowLayer::kSingle;
      ++n;
    }
  }
  out.count = n;
  return out;
}

// Every light of the frame names its views: for each light with an id, the valid entries of `shadow` whose `view_light` is that id (consecutive, as ShadowFrameFromLocal and the
// atlas manager make them): kind kAtlas, first_view, view_count (a kStatic + kLive pair counts as two entries, a light has at most two), the weight 1 (a fade is the manager's).
// A light the shadow frame says nothing of keeps kNone.
inline void BindShadowRefs(LightFrame* frame) {
  for (uint32_t i = 0; i < frame->count && i < kMaxLights; ++i) {
    Light& l = frame->lights[i];
    if (l.id == kNoLightId) {
      continue;
    }
    uint32_t first = kMaxLightShadowViews;
    uint32_t count = 0;
    for (uint32_t e = 0; e < frame->shadow.count && e < kMaxLightShadowViews; ++e) {
      if (frame->shadow.view_light[e] == l.id && frame->shadow.views[e].valid != 0 && count < 2) {
        first = std::min(first, e);
        ++count;
      }
    }
    if (count == 0) {
      l.shadow.kind = ShadowKind::kNone;
      l.shadow.view_count = 0;
      l.shadow.first_view = 0;
      continue;
    }
    l.shadow.kind = ShadowKind::kAtlas;
    l.shadow.first_view = uint8_t(first);
    l.shadow.view_count = uint8_t(count);
  }
}

// ---------------------------------------------------------------------------
// the cars' light state
// ---------------------------------------------------------------------------

// The kinds of a vehicle's light point the night probe has read (headlight_points.h: 0 Brakelight, 1 Taillight, 3 Headlight, 4 HiBeam; 2 and 5 are not named; 6 is a turn
// signal, whose side the probe did not read). // CHECK: the turn signals' sides and the police kinds (survey C4) before LU-7 sets kCarLightTurn* and kCarLightPolice*.
inline constexpr uint32_t kPointKindBrake = 0, kPointKindTail = 1, kPointKindHeadlight = 3, kPointKindHiBeam = 4;
inline constexpr float kPointOffIntensity = 0.05f;  // at and below it a point is off (headlight_points.h kOffIntensity)
inline constexpr float kPointFullIntensity = 2.0f;  // what a point reads when it is on (headlight_points.h kFullIntensity)

// Notes one of a vehicle's light points in its CarLights: the bit of its kind when the point is on, and the level of its family as the largest of its points' (intensity over the
// full one, 0..1). False for a kind it does not know.
inline bool NoteCarPoint(CarLights* car, uint32_t kind, float intensity) {
  uint32_t bit = 0;
  uint32_t level = 0;
  switch (kind) {
    case kPointKindBrake: bit = kCarLightBrake; level = kCarLevelBrake; break;
    case kPointKindTail: bit = kCarLightTail; level = kCarLevelTailTurn; break;
    case kPointKindHeadlight: bit = kCarLightHead; level = kCarLevelHead; break;
    case kPointKindHiBeam: bit = kCarLightHiBeam; level = kCarLevelHead; break;
    default: return false;
  }
  if (intensity > kPointOffIntensity) {
    car->bits |= bit;
    car->level[level] = std::max(car->level[level], light_laws::Saturate(intensity / kPointFullIntensity));
  }
  return true;
}

}  // namespace larecomp::gpu
