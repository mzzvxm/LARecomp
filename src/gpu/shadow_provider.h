#pragma once
// ===========================================================================
// MCLA GPU renderer -- the interface of a provider of shadow views for the one atlas (perf/LIGHT-UNIFY.md 4.2, package LU-5)
// ===========================================================================
// WHO PLUGS IN. The headlight draw replay (local_shadow.h LocalShadow: the FIRST provider), the street lamps' shadows (feat/lamp-shadows, PARKED by the owner: when it lands it becomes a
// second provider asking for a live pool and a static pool, and keeps its picker and its draw cap; nothing of it is here) and the ray traced mask (CITY-RT CP-D: a provider that asks for NO
// pool and publishes channels). The atlas (shadow_atlas.h) owns the texture, the layout, the barriers and the memory; a provider owns what it draws and the views it makes.
//
// WHAT A PROVIDER DOES, per frame (the Scene's calls into the atlas and the providers are in shadow_atlas.h):
//   1 RequestPools   at the frame's start (after the level is resolved and the cars or lamps are picked): the pools it wants (shadow_atlas_rules.h PoolRequest: a tile, a count, a role, sticky or
//                    not). A provider with nothing to draw asks for none.
//   2 OnLayout       the atlas has laid the frame's pools out (and made the texture, or refused: `layout.valid()` false): where the provider's tiles are (Layout::FindPool, SlotRect).
//   3 the record     the provider's own business, at its own trigger (the replay: Scene::OnResolve at the mid-scene depth copy): atlas.BeginRecord, atlas.SetTile for each tile it draws,
//                    atlas.EndRecord (the ONE barrier: pixel and compute readers). Several providers share one record pass when they trigger at the same point (a later step; today only the
//                    replay records).
//   4 CollectViews   the views it made, each with the id of the light it is of and its layer, into a ShadowFrameBuilder; masks too. The atlas publishes them once, as one ShadowFrame.
//
// Pure of plume: the command list is only named (a forward declaration). Threading: the render thread.
// ===========================================================================

#include <cstdint>

#include "shadow_atlas_rules.h"

namespace plume {
struct RenderCommandList;
}

namespace mcla::gpu {

class ShadowProvider {
 public:
  virtual ~ShadowProvider() = default;

  virtual shadow_atlas::ProviderId provider_id() const = 0;

  // 1. Adds the pools this provider wants of the atlas this frame to `spec` (none: nothing added).
  virtual void RequestPools(shadow_atlas::AtlasSpec* spec) const = 0;

  // 2. The frame's layout (it can be invalid: the atlas could not be made; the provider then makes nothing this frame).
  virtual void OnLayout(const shadow_atlas::Layout& layout) { (void)layout; }

  // Whether its pools have a STATIC twin (the lamps' static layers): the atlas fits the level's memory with them (shadow_atlas_rules.h FitLevel).
  virtual bool wants_static_pool() const { return false; }

  // 4. The views (and masks) it made at its last record, for the atlas to publish: AddView(view, light id, layer), AddMask(light id, channel, weight). Nothing when it has none.
  virtual void CollectViews(shadow_atlas::ShadowFrameBuilder* builder) const = 0;
};

}  // namespace mcla::gpu
