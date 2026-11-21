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
//   4b  ProjectVolume*          the volume's lamps, headlights, shadow views and atlas (VolInputs), a later step of this package
//   4c  ProjectShadowLights     the lights the shadow replay's pick reads (local_shadow::Light), a later step of this package
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
#include "local_shadow_types.h"

namespace mcla::gpu::light_project {

namespace lg = ::larecomp::gpu;
namespace lm = ::larecomp::gpu::light_manager;
namespace hr = ::mcla::gpu::headlight_rules;

// The A/B of the flips (mcla_gpu_light_frame, a bit mask): which consumers read the frame instead of their own producer. 0 (the default until the runs prove the bytes equal): none.
inline constexpr uint32_t kFlipHeadlightBlock = 1u << 0;  // 4a: the lit shaders' headlight block
inline constexpr uint32_t kFlipVolume = 1u << 1;          // 4b: the volumetric light's lamps, headlights and shadow views
inline constexpr uint32_t kFlipShadowLights = 1u << 2;    // 4c: the lights the headlight shadow replay picks cars and makes views from
inline constexpr uint32_t kFlipAll = kFlipHeadlightBlock | kFlipVolume | kFlipShadowLights;

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

}  // namespace mcla::gpu::light_project
