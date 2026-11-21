#pragma once
// ===========================================================================
// MCLA GPU renderer -- the consumers' shapes as PROJECTIONS of the LightFrame (perf/LIGHT-UNIFY.md 6.1 and 7.1 step U4, package LU-4)
// ===========================================================================
// The three consumers that read lights today each read their own producer: the lit shaders' headlight block reads the headlight rig (HeadlightRig::Block()), the volumetric light reads
// the rig's set, the replay's frame and its own lamp picker (VolumetricsInputs), and the shadow replay reads the rig's set (LocalShadow::PickViews). LU-4 moves each to the one frame
// the LightManager makes (light_frame.h), one consumer at a time, with NO change to what the consumer receives: its constants, inputs and lights are BYTE FOR BYTE what they were.
// That is what this file is: each function takes a LightFrame and returns exactly the bytes the consumer's own producer made from the same lights, by running the consumer's own
// builder over the frame's data (headlight_rules::MakeConstants, the conversions of light_convert.h, local_shadow::MakeLight) instead of a second copy of its arithmetic. What can go
// wrong is only the DATA (a light that is not the rig's, a view in another order), and that is what the checks below look for and light_project_test.cpp holds to recorded frames.
//
//   4a  ProjectHeadlightBlock   the headlight variant's constant block (headlight_rig_rules.h HeadlightConstants, 1072 bytes), the camera as the caller gives it
//   4b  ProjectVolumeLamps,     the volume's street lamps, headlights, shadow views and atlas (VolInputs: volumetrics_types.h), as VolumetricsInputs::FillLamps and FillHeadlightInputs
//       ProjectVolumeHeadlights  make them
//   4c  ProjectShadowLights     the lights the shadow replay's pick reads (local_shadow::Light: the rig's lights in double, each with its place in the rig), as LocalShadow::PickViews makes them
// and, for each, the check the manager runs while the census is on: the same data made by the legacy producer and by the projection, compared as bytes (CheckBlockBytes, ...).
//
// THE RIG'S LIGHTS OF A FRAME. The frame holds the rig's lights in slots of their own (a light keeps its slot while it lives), and the consumers want them in the RIG'S ORDER (the
// player's first, then by rank: the block's light i, the volume's headlight i and the replay's light index i are the same light). The frame says that order in its surface ranking
// (LightFrame::order[kSurface], which the Collector fills with the rig's lights in the rig's order, light_manager_rules.h RebuildOrder): RigLightsOf() reads the headlights (source
// kHeadlight) of it, at most four.
// THE SHADOW VIEWS. A light's views are consecutive entries of the frame's table (ShadowFrame, grouped by light as ShadowFrameFromLocal makes them); the replay publishes a
// light's views one after the other (local_shadow.cpp: per planned view, per light it covers), so the table's order is the replay's and the block's `shadow[]` and the volume's
// `shadow_views[]` come out in the order they had. Views no rig light owns are not projected (the block's remap drops them too).
//
// Pure: no renderer, no guest. Namespace mcla::gpu::light_project.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#include "headlight_rig_rules.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_manager_rules.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "volumetrics_headlights.h"
#include "volumetrics_types.h"

namespace mcla::gpu::light_project {

namespace lg = ::larecomp::gpu;
namespace lm = ::larecomp::gpu::light_manager;
namespace hr = ::mcla::gpu::headlight_rules;
namespace vol = ::mcla::gpu::volumetrics;
namespace ls = ::mcla::gpu::local_shadow;

// The A/B of the flips (mcla_gpu_light_frame, a bit mask): which consumers read the frame instead of their own producer. 0 (the default until the runs prove the bytes equal): none.
inline constexpr uint32_t kFlipHeadlightBlock = 1u << 0;  // 4a: the lit shaders' headlight block
inline constexpr uint32_t kFlipVolume = 1u << 1;          // 4b: the volumetric light's lamps, headlights and shadow views
inline constexpr uint32_t kFlipShadowLights = 1u << 2;    // 4c: the lights the headlight shadow replay picks cars and makes views from
inline constexpr uint32_t kFlipAll = kFlipHeadlightBlock | kFlipVolume | kFlipShadowLights;  // the three consumers of LU-4 (the default of the setting when the runs have shown their bytes equal)
// LU-5: the shadow views themselves. The headlight replay records into the one shadow atlas (shadow_atlas.h: its texture, layout, clear, THE ONE barrier for the pixel and compute readers, the
// memory fitted to the budget) and the manager takes the frame's shadow views from the atlas's published form BY LIGHT ID (shadow_atlas_rules.h RebindByIds) instead of the key remap of the
// rig's carry; with the census on, the two frames are compared field by field every frame (Check::kShadowFrame). Not part of kFlipAll: it has its own proof run (RUNPLAN-lu-5.txt).
inline constexpr uint32_t kFlipAtlas = 1u << 3;
inline constexpr uint32_t kFlipMask = kFlipAll | kFlipAtlas;  // every bit the setting knows

// ---------------------------------------------------------------------------
// the rig's lights of a frame
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRigLights = hr::kMaxLights;  // 4

struct RigLights {
  uint32_t count = 0;
  std::array<uint32_t, kRigLights> slot{};  // the frame's slot of rig light i
};

// The headlights of the frame's surface ranking, in its order, at most four: the rig's set as the Collector made it.
inline RigLights RigLightsOf(const lg::LightFrame& frame) {
  RigLights rig;
  const uint32_t n = std::min<uint32_t>(frame.role_count[uint32_t(lg::LightRole::kSurface)], lg::kMaxLights);
  for (uint32_t r = 0; r < n && rig.count < kRigLights; ++r) {
    const uint32_t slot = frame.order[uint32_t(lg::LightRole::kSurface)][r];
    if (slot >= lg::kMaxLights) {
      continue;
    }
    const lg::Light& l = frame.lights[slot];
    if (l.id != lg::kNoLightId && l.source == lg::LightSource::kHeadlight) {
      rig.slot[rig.count++] = slot;
    }
  }
  return rig;
}

// The views the rig's lights own, in the frame's order, as the block's remap makes them: valid ones only, each named for the rig index of its light (LocalShadowView::light_index),
// at most two a light (a third counts for nobody) and eight in all.
inline lg::LocalShadowFrame ShadowViewsOfRig(const lg::LightFrame& frame, const RigLights& rig) {
  lg::LocalShadowFrame out;
  std::array<uint32_t, kRigLights> taken{};
  const uint32_t views = std::min<uint32_t>(frame.shadow.count, lg::kMaxLightShadowViews);
  for (uint32_t e = 0; e < views && out.count < out.views.size(); ++e) {
    const lg::LocalShadowView& v = frame.shadow.views[e];
    const uint64_t owner = frame.shadow.view_light[e];
    if (v.valid == 0 || owner == lg::kNoLightId) {
      continue;
    }
    for (uint32_t i = 0; i < rig.count; ++i) {
      if (frame.lights[rig.slot[i]].id != owner) {
        continue;
      }
      if (taken[i] < 2) {
        ++taken[i];
        lg::LocalShadowView moved = v;
        moved.light_index = i;
        out.views[out.count++] = moved;
      }
      break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// 4a: the headlight variant's constant block
// ---------------------------------------------------------------------------

// The block the lit shaders read after the shared constants (headlight_rig_rules.h): the rig's lights of the frame as the shader's rows, the box they reach, the frame's shadow views
// and the atlas they are in. `camera` is the world position the block carries as a fallback (every draw whose pixel constants hold the camera overwrites it: Scene::FillVariantBlock).
// The frame's atlas slot and sampler are the block's; with no view a light reads, the block has none (every light lit, no atlas read), as MakeConstants does it.
inline hr::HeadlightConstants ProjectHeadlightBlock(const lg::LightFrame& frame, const float camera[3]) {
  const RigLights rig = RigLightsOf(frame);
  std::array<hr::Lamp, kRigLights> lamps{};
  for (uint32_t i = 0; i < rig.count; ++i) {
    const lg::Light& l = frame.lights[rig.slot[i]];
    for (int c = 0; c < 3; ++c) {
      lamps[i].position[c] = l.pos[c];
      lamps[i].direction[c] = l.dir[c];
      lamps[i].color[c] = l.color[c];
    }
    lamps[i].range = l.range;
    lamps[i].cos_outer = l.cos_outer;
    lamps[i].cos_inner = l.cos_inner;
  }
  const lg::LocalShadowFrame views = ShadowViewsOfRig(frame, rig);
  return hr::MakeConstants(lamps.data(), rig.count, camera, views.count != 0 ? &views : nullptr, frame.shadow.atlas.slot, frame.shadow.atlas.sampler);
}

// ---------------------------------------------------------------------------
// 4b: the volume's light inputs
// ---------------------------------------------------------------------------

// The street lamps of the frame's volume ranking (in the order the pick gave them: the fades' spare slots last) appended to the volume's inputs after the lamps that are in already (the
// core's test lamp), each as the volume's lamp (light_convert.h ToVolLamp: the Light's colour x the weight the picker folded in). Returns how many were appended. Only the game's lamps
// (source kLamp): the volume's own other lights are not here yet.
inline uint32_t ProjectVolumeLamps(const lg::LightFrame& frame, vol::VolInputs* out) {
  uint32_t added = 0;
  const uint32_t n = std::min<uint32_t>(frame.role_count[uint32_t(lg::LightRole::kVolume)], lg::kMaxLights);
  for (uint32_t r = 0; r < n && out->lamp_count < vol::kMaxLamps; ++r) {
    const lg::Light& l = frame.Ranked(lg::LightRole::kVolume, r);
    if (l.id == lg::kNoLightId || l.source != lg::LightSource::kLamp) {
      continue;
    }
    out->lamps[out->lamp_count++] = lg::ToVolLamp<vol::VolLamp>(l);
    ++added;
  }
  return added;
}

// The volume's headlights and the shadow views they read, as FillHeadlightInputs makes them from the rig's set and the replay's frame: each rig light as the volume's lamp in the volume's
// units (ToVolLamp), the frame's view table as the volume's (VolShadowView: the same 96 bytes), each light's one or two views by its ShadowRef, the views the volume cannot read dropped
// (the same DropUnusableViews as FillShadows), the atlas's slot and size. An empty rig leaves `out` as it is (the legacy producer does). No atlas, or no view: the volume has none.
inline void ProjectVolumeHeadlights(const lg::LightFrame& frame, vol::VolInputs* out) {
  const RigLights rig = RigLightsOf(frame);
  if (rig.count == 0) {
    return;
  }
  uint32_t n = 0;
  for (uint32_t i = 0; i < rig.count && n < vol::kMaxHeadlights; ++i) {
    out->headlights[n++] = lg::ToVolLamp<vol::VolLamp>(frame.lights[rig.slot[i]]);
  }
  out->headlight_count = n;
  const lg::ShadowAtlasInfo& atlas = frame.shadow.atlas;
  const bool usable = atlas.valid && atlas.slot != vol::kNoSlot && atlas.width != 0 && atlas.height != 0 && frame.shadow.count != 0;
  out->shadow_view_count = 0;
  out->shadow_atlas_slot = vol::kNoSlot;
  if (!usable) {
    for (uint32_t i = 0; i < out->headlight_count; ++i) {
      out->headlights[i].shadow_view = -1;
      out->headlights[i].shadow_view_b = -1;
    }
    return;
  }
  const uint32_t views = std::min<uint32_t>(frame.shadow.count, vol::kMaxShadowViews);
  for (uint32_t e = 0; e < views; ++e) {
    out->shadow_views[e] = lg::ToVolShadowView<vol::VolShadowView>(frame.shadow.views[e]);
  }
  out->shadow_view_count = views;
  out->shadow_atlas_slot = atlas.slot;
  out->shadow_atlas_size[0] = float(atlas.width);
  out->shadow_atlas_size[1] = float(atlas.height);
  vol::DropUnusableViews(out);
}

// ---------------------------------------------------------------------------
// 4c: the lights the shadow replay picks its cars and makes its views from
// ---------------------------------------------------------------------------

// The rig's lights of the frame as the replay's rules read them (LocalShadow::PickViews): each through local_shadow::MakeLight (the rules' own validation and widening to double), named
// for its place in the rig (LocalShadowView::light_index), with the lamp offset the light has. A light the rules refuse is left out and the next one still carries its own place, as in
// PickViews. At most four (the groups take four). Returns how many are in `out`.
inline uint32_t ProjectShadowLights(const lg::LightFrame& frame, ls::Light out[4]) {
  const RigLights rig = RigLightsOf(frame);
  uint32_t count = 0;
  for (uint32_t i = 0; i < rig.count && count < 4; ++i) {
    const lg::Light& l = frame.lights[rig.slot[i]];
    if (ls::MakeLight(l.pos, l.dir, l.cos_outer, l.range, l.car_key, (l.flags & lg::kLightFlagPlayer) != 0, i, &out[count], l.offset)) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// the checks: the legacy producer's bytes against the projection's
// ---------------------------------------------------------------------------

namespace detail {

// Whether two blocks of bytes are the same, and where they first differ (the byte offset), for the census line.
inline bool SameBytes(const void* a, const void* b, size_t bytes, size_t* first_difference) {
  if (std::memcmp(a, b, bytes) == 0) {
    return true;
  }
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  size_t i = 0;
  while (i < bytes && x[i] == y[i]) {
    ++i;
  }
  *first_difference = i;
  return false;
}

// Which part of the 1072 byte block a byte is in.
inline const char* BlockPart(size_t offset) {
  if (offset < offsetof(hr::HeadlightConstants, camera)) return "the light count";
  if (offset < offsetof(hr::HeadlightConstants, bounds_lo)) return "the camera";
  if (offset < offsetof(hr::HeadlightConstants, lights)) return "the reach box";
  if (offset < offsetof(hr::HeadlightConstants, shadow_of)) return "a light's rows";
  if (offset < offsetof(hr::HeadlightConstants, shadow_of_b)) return "which view a light reads";
  if (offset < offsetof(hr::HeadlightConstants, atlas)) return "which second view a light reads";
  if (offset < offsetof(hr::HeadlightConstants, shadow)) return "the atlas";
  return "a shadow view";
}

}  // namespace detail

// 4a: the block the rig makes (`legacy`, HeadlightRig::Block()) against the projection of the frame with the legacy block's own camera, byte for byte. One check a call.
inline void CheckBlockBytes(const lg::LightFrame& frame, const hr::HeadlightConstants& legacy, lm::Check kind, lm::SelfCheck* sc) {
  static_assert(sizeof(hr::HeadlightConstants) == 1072, "the block is compared as bytes: no member may be added without this check knowing");
  const hr::HeadlightConstants projected = ProjectHeadlightBlock(frame, legacy.camera);
  size_t at = 0;
  if (detail::SameBytes(&projected, &legacy, sizeof(legacy), &at)) {
    sc->Note(kind, std::string());
    return;
  }
  sc->Note(kind, std::format("the projected headlight block differs from the rig's at byte {} ({}); {} lights in the frame, {} in the rig's block", at, detail::BlockPart(at),
                             RigLightsOf(frame).count, legacy.header[0]));
}

// 4b: the volume's light inputs made from the frame (`projected`) against what the legacy producer made from the same entry (`legacy`): the lamps (all of the array, so a lamp past the
// count is a difference too), the headlights, the shadow views, the counts, the atlas's slot and size, byte for byte. Names the first lamp or headlight that differs and the part of it.
inline const char* VolLampPart(size_t offset) {
  if (offset < 12) return "its position";
  if (offset < 16) return "its range";
  if (offset < 28) return "its axis";
  if (offset < 32) return "its outer cone";
  if (offset < 44) return "its colour";
  if (offset < 48) return "its inner cone";
  if (offset < 52) return "which view it reads";
  if (offset < 56) return "its near radius";
  if (offset < 60) return "which second view it reads";
  return "its last word";
}

inline void CheckVolumeBytes(const vol::VolInputs& projected, const vol::VolInputs& legacy, lm::SelfCheck* sc) {
  static_assert(sizeof(vol::VolLamp) == 64 && sizeof(vol::VolShadowView) == 96, "the volume's lamps and views are compared as bytes: no padding, no member added unseen");
  std::string fail;
  const auto add = [&](const std::string& what) {
    if (fail.size() < 360) {
      fail += fail.empty() ? what : "; " + what;
    }
  };
  size_t at = 0;
  if (projected.lamp_count != legacy.lamp_count) {
    add(std::format("the frame gives {} lamps, the legacy producer {}", projected.lamp_count, legacy.lamp_count));
  }
  if (projected.headlight_count != legacy.headlight_count) {
    add(std::format("the frame gives {} headlights, the legacy producer {}", projected.headlight_count, legacy.headlight_count));
  }
  if (projected.shadow_view_count != legacy.shadow_view_count) {
    add(std::format("the frame gives {} shadow views, the legacy producer {}", projected.shadow_view_count, legacy.shadow_view_count));
  }
  if (projected.shadow_atlas_slot != legacy.shadow_atlas_slot) {
    add(std::format("the atlas slot is {} against {}", projected.shadow_atlas_slot, legacy.shadow_atlas_slot));
  }
  if (!detail::SameBytes(projected.shadow_atlas_size, legacy.shadow_atlas_size, sizeof(legacy.shadow_atlas_size), &at)) {
    add(std::format("the atlas size is {}x{} against {}x{}", projected.shadow_atlas_size[0], projected.shadow_atlas_size[1], legacy.shadow_atlas_size[0], legacy.shadow_atlas_size[1]));
  }
  for (uint32_t i = 0; i < vol::kMaxLamps; ++i) {
    if (!detail::SameBytes(&projected.lamps[i], &legacy.lamps[i], sizeof(vol::VolLamp), &at)) {
      add(std::format("lamp {} of {} differs at byte {} ({})", i, legacy.lamp_count, at, VolLampPart(at)));
      break;
    }
  }
  for (uint32_t i = 0; i < vol::kMaxHeadlights; ++i) {
    if (!detail::SameBytes(&projected.headlights[i], &legacy.headlights[i], sizeof(vol::VolLamp), &at)) {
      add(std::format("headlight {} of {} differs at byte {} ({})", i, legacy.headlight_count, at, VolLampPart(at)));
      break;
    }
  }
  for (uint32_t i = 0; i < vol::kMaxShadowViews; ++i) {
    if (!detail::SameBytes(&projected.shadow_views[i], &legacy.shadow_views[i], sizeof(vol::VolShadowView), &at)) {
      add(std::format("shadow view {} of {} differs at byte {}", i, legacy.shadow_view_count, at));
      break;
    }
  }
  sc->Note(lm::Check::kVolumeBytes, fail);
}

// 4c: the lights made from the frame against the ones PickViews made from the rig's set (`legacy`), field by field and bit for bit (doubles: the same arithmetic over the same floats). Names
// the first light and field that differs.
inline void CheckShadowLights(const ls::Light* legacy, uint32_t legacy_count, const ls::Light* projected, uint32_t projected_count, lm::SelfCheck* sc) {
  std::string fail;
  if (legacy_count != projected_count) {
    fail = std::format("the frame gives {} lights to the replay, the rig's set {}", projected_count, legacy_count);
  }
  const auto same3 = [](const double* a, const double* b) {
    return lm::detail::SameBits(a, b, 3 * sizeof(double));
  };
  for (uint32_t i = 0; i < std::min(legacy_count, projected_count) && fail.empty(); ++i) {
    const ls::Light& a = legacy[i];
    const ls::Light& b = projected[i];
    const char* part = nullptr;
    if (!same3(a.pos, b.pos)) part = "its position";
    else if (!same3(a.dir, b.dir)) part = "its axis";
    else if (!lm::detail::SameBits(&a.cos_outer, &b.cos_outer, sizeof(double))) part = "its cone";
    else if (!lm::detail::SameBits(&a.range, &b.range, sizeof(double))) part = "its range";
    else if (a.car_key != b.car_key) part = "its car";
    else if (a.is_player != b.is_player) part = "whether it is the player's";
    else if (a.index != b.index) part = "its place in the rig";
    else if (!same3(a.lamp_offset, b.lamp_offset)) part = "its lamp offset";
    if (part != nullptr) {
      fail = std::format("light {} of {} differs from the rig's in {}", i, legacy_count, part);
    }
  }
  sc->Note(lm::Check::kShadowLights, fail);
}

}  // namespace mcla::gpu::light_project
