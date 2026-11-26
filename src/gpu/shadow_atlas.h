#pragma once
// ===========================================================================
// MCLA GPU renderer -- THE shadow atlas: one depth texture for every provider of shadow views (perf/LIGHT-UNIFY.md section 4, package LU-5 = step U5)
// ===========================================================================
// Until now the headlight replay (local_shadow.h) made, held, cleared, transitioned and published its own atlas, and the volume's inject and the lit draws each had to be told the state it
// was left in. This is that part, extracted and shared: the texture (D32 float + S8, the depth plane viewed as one float on both backends), its layout (shadow_atlas_rules.h: pools of tiles
// by provider, laid out by the level and never by the lights in view), the memory it holds (reported as a holder to the video memory budget, and fitted to it before it is made), the
// clear, THE ONE barrier after a record (shader read for the pixel AND the compute stage: the f427298a rule lives in UseTracker, used only here), and the published form (the frame's
// ShadowFrame: each view with the id of its light and its layer; the ray traced masks' channels).
//
// PROVIDERS (shadow_provider.h) ask for pools, draw into their tiles and hand over their views: the headlight replay is the first (LocalShadow implements ShadowProvider; its collection, its
// replay and its pipelines stay where they are: the lamp shadows' branch, parked, edits that file heavily and a move would only make conflicts); the street lamps' shadows and the ray traced
// mask plug into the same interface later.
//
// THE A/B. mcla_gpu_light_frame bit 8 (light_project.h kFlipAtlas): the replay records into THIS atlas (its own legacy atlas code stays in local_shadow.cpp for the A/B and goes one release
// after the default flips) and the light manager takes its shadow views from Published() by light id instead of the key remap of the rig's carry; the manager's self-check compares the two
// frames every frame while the census is on. Off (the default): nothing here runs, nothing is made.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "light_budget.h"
#include "light_frame.h"
#include "shadow_atlas_rules.h"
#include "shadow_provider.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;

class ShadowAtlas {
 public:
  ShadowAtlas();
  ~ShadowAtlas();
  ShadowAtlas(const ShadowAtlas&) = delete;
  ShadowAtlas& operator=(const ShadowAtlas&) = delete;

  // Makes nothing: the texture is made by the first Ensure after a plan that asks for tiles. Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap);
  // GPU idle and the FrameRing drained, before the heap shuts down (the slot is not handed back: the heap goes after).
  void Shutdown();

  // The providers (one for each shadow_atlas::ProviderId; the pointer must outlive the atlas or be removed). Render thread.
  void AddProvider(ShadowProvider* provider);
  void RemoveProvider(ShadowProvider* provider);
  bool HasProvider(shadow_atlas::ProviderId id) const { return providers_[uint32_t(id)] != nullptr; }

  // THE FRAME'S PLAN. Asks every provider for its pools (ProviderRequest), lays them out, tells each provider (OnLayout); returns the layout the texture is to have. Cheap: no GPU call.
  const shadow_atlas::Layout& Plan();
  // The texture for the planned layout: kept when its size is already that, else made (the old one goes after the lists that name it). False when it could not be made.
  bool Ensure();

  // THE RECORD PASS (a provider's trigger): BeginRecord transitions to depth write, sets the atlas as the target and clears what the clear plan says (the whole texture when every pool is
  // drawn every record); SetTile sets the viewport and scissor of a tile; EndRecord leaves the target and transitions to shader read for the pixel and the compute stages. A provider that
  // records calls Begin once, SetTile for each tile it draws, End once, even when it stops half way. Several providers' passes in one frame are Begin + End each (the clear plan
  // spares a static pool that is not invalid).
  bool BeginRecord(plume::RenderCommandList* list);
  void SetTile(plume::RenderCommandList* list, const shadow_atlas::Rect& tile);
  void EndRecord(plume::RenderCommandList* list);
  // Clears one tile to "nothing drawn" inside a record pass (after BeginRecord, before the tile's draws): a street lamp's static layer built again (lamp_shadows_rules.h DecideStatic).
  void ClearTile(plume::RenderCommandList* list, const shadow_atlas::Rect& tile);
  // Changes whenever the texture is made, made again or given back, or the pools moved inside it: what a static layer cached is not in it any more (a provider with a static pool compares).
  uint32_t generation() const { return generation_; }
  // A static pool whose cached maps are no good (a new layout, a slot given to another light): cleared at the next record. Bit = the pool's index in the layout.
  void InvalidateStatic(uint32_t pool_bits) { static_invalid_ |= pool_bits; }

  // THE PUBLISHED FORM: asks every provider for its views (CollectViews) and keeps the frame they make (views grouped by light, ids, layers, the masks) with the atlas as it is now
  // (`valid` false, or no views: the empty frame). `sampler` = the point sampler's slot the lit shaders compare with. Call whenever a provider's views change (the replay's HandOver).
  void Collect(uint32_t sampler);
  // Gives the published views up (a menu, a frame with no trigger, a release): the next Published() is empty.
  void ClearPublished();
  const shadow_atlas::Published& published() const { return published_; }

  // The texture's bindless slot and size, the layout it has, whether there is one.
  bool has_texture() const { return texture_ != nullptr; }
  uint32_t slot() const { return slot_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const shadow_atlas::Layout& layout() const { return layout_; }
  const shadow_atlas::Layout& planned() const { return planned_; }
  // The video memory it holds now (the budget's holder: renderer.cpp).
  uint64_t HeldBytes() const;
  // Gives the texture back after the lists that name it (a level change, an idle atlas, a step down). The published views go with it.
  void Release();
  // A line for the log / the census: the size, the pools and what is held.
  std::string Describe() const;

 private:
  static constexpr uint64_t kFitRetainFrames = 60;
  static constexpr uint64_t kFitRefusedRetainFrames = 300;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  std::array<ShadowProvider*, shadow_atlas::kProviderCount> providers_{};

  shadow_atlas::AtlasSpec spec_;
  shadow_atlas::Layout planned_;  // what Plan made this frame
  shadow_atlas::Layout layout_;   // what the texture has
  shadow_atlas::UseTracker use_;
  uint32_t static_invalid_ = 0;
  uint32_t generation_ = 1;

  std::unique_ptr<plume::RenderTexture> texture_;
  std::unique_ptr<plume::RenderTextureView> view_;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer_;
  uint32_t slot_ = 0xFFFFFFFFu;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  shadow_atlas::Published published_;

  bool memory_warned_ = false;
  bool create_failed_logged_ = false;
};

}  // namespace mcla::gpu
