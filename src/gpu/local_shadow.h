#pragma once
// ===========================================================================
// MCLA GPU renderer -- shadow maps for the cars' headlights (perf/LOCAL-SHADOWS-SURVEY.md, option A)
// ===========================================================================
// The game has no shadow from a light that is not the sun. This draws the main view's opaque draws again, depth only, from each lit car's
// headlights into a small atlas of spot views, and publishes the atlas and the views' matrices (local_shadow_types.h) for the pass that
// lights the world with the headlights (headlight_rig.h) to read. The rules (setting, atlas, matrices, classification) are in
// local_shadow_rules.h and tested without a renderer; this file is the renderer's half, beside car_shadow_map.h, whose replay of the
// player's car from the sun is its model.
//
// WHAT IT DOES, and when.
//   mcla_gpu_light_shadows       auto | off | medium | high | ultra (the pause menu's LIGHT SHADOWS: the headlights now, the street lamps later; it was
//                                mcla_gpu_headlight_shadows, which stays as an alias; local_shadow_rules.h kLevelTable has the rows and is the one place to change:
//                                the three level shape, owner 2026-11-20, after the cost runs of a view for each headlamp). The levels differ by how many cars are
//                                shadowed, by how many of them have a view for EACH HEADLAMP, and by how sharp. medium: the player's car and the nearest other car,
//                                one view for each car, 2 views of 768 x 768 (9 MiB of video memory); high: the same two cars, the player's car with a view for
//                                each lamp and the other car with one, 3 views of 1024 x 1024 (24 MiB); ultra: the player's car and the two nearest other cars, every
//                                car with a view for each lamp, 6 views of 1280 x 1280 (75 MiB). The sizes are chosen so that a texel is 1.3 to 1.9 cm in the world at
//                                10 m from the lamps (the first version's 512 gave 3.5 cm and lost every shadow thinner than 3 texels: HL-SHADOW-QUALITY, 2026-11-21). `auto` is medium on Mid, ultra on High, off on Low (gpu_class.h
//                                Effect::kLightShadows). Live: a change applies at the next frame.
//                                A CAR'S LIGHT IS ONE TERM (the middle of its two lamps: headlight_types.h) AND ITS SHADOW IS THE MEAN OF ITS LAMPS' VIEWS: a
//                                pole in front of the car that both lamps light casts two shadows, each half as dark where only one lamp is blocked, and the
//                                shadow of a car in front has the half-lit band of one lamp. The lamps' places come from the rig (Headlight::lamp_offset: the
//                                game's two Headlight points, else the front lenses, else 0.55 m either side of the light).
//                                WHICH CARS: the player's car always, the others by importance (nearer to the camera and on the screen first), with a
//                                hysteresis so that two cars of the same importance do not trade views back and forth (local_shadow_rules.h ViewPicker); a car's
//                                views come and go together. A car the rig lights but that has no view is lit without a shadow. mcla_gpu_headlight_shadow_views
//                                (debug) is the views of a car: 1 one for the car (the first version's), 2 one for each lamp, 0 the level's.
//                                A view costs about 0.02 ms of the graphics card and 0.05 to 0.1 ms of the render thread (the draws are replayed once more for
//                                each view); the size barely costs. perf/HL-PER-LAMP-RUN.md has the cost plan of the lamps' views.
//   COLLECT (Scene::OnDraw, every prepared draw of the main view's colour pass before the anchor): the draw's shaders, input layout, host
//   geometry (valid until the frame's lists are submitted), its vertex constants (a host copy of the bank, 4 KiB, once per bank version),
//   its shared constants (once per distinct block) and its pixel constants span. A draw is a caster when it is opaque (blend off, depth
//   test and write on, a pixel shader), draws triangles, is no impostor (the pack's computed fetch index flag) or listed billboard, and its
//   pixel shader writes no depth; the alpha tested ones (the pixel shader's spec variant 1) draw with their own pixel shader, which with no
//   colour target the driver cuts down to the alpha test. Collecting costs a few hundred bytes a draw, and only runs while the setting is on
//   and a headlight is lit (or was a moment ago).
//   REPLAY (the history trigger, the game's mid-scene depth copy into 078B0000 after every opaque draw of the main view, Scene::OnResolve
//   calls us before the ambient occlusion's passes): the rig's lights are grouped by car and the level's cars are picked, each car gets a view for each
//   headlamp or one for the car (local_shadow_rules.h PlanViews), each view a spot frustum that covers the light's cone from where the lamp is (FitView, LightClip), and
//   every caster is drawn into each view with the game's
//   own vertex shader, the draw's own vertex and index views, a copy of its vertex constants whose c8-c11 (gWorldViewProj) is replaced by
//   gWorldViewProj x VP_main^-1 x VP_light (local_shadow_rules.h: that is gWorld x VP_light), a copy of its shared constants whose NDC transform
//   is the pass's, and no pixel shader (or its own, for the alpha tested). The main camera VP_main is found in the same frame's stored banks
//   (DeriveCamera). Faces: the world's draws keep the ones turned from the light, a vehicle's parts are drawn on both (local_shadow_rules.h
//   ResolveCasterCull; mcla_gpu_headlight_shadow_cull is the debug policy). The lamps' own car (a vehicle's part whose
//   root is within 3.5 m of the view's eye) and everything nearer than 1.5 m to the lamps (the near plane) are left out.
//   THE GROUND SKIRT: each vehicle part is drawn a second time into the same view, 0.22 m lower and 0.6 m farther along the light (local_shadow_rules.h "the
//   ground skirt"; mcla_gpu_headlight_shadow_skirt turns it off). A car floats 0.1 to 0.2 m over the road and a lamp as low as a car's own shines through that
//   gap onto the road behind it, so the road behind a car was lit except for four stripes where the tyres are (the owner's picture of 2026-11-20); the copy puts
//   the underbody on the road. It sits behind the car's own surfaces for the lamp, so it shadows none of them.
//   The map is of THIS frame's draws but the lit draws that read it came BEFORE the trigger, so they read the previous frame's map: one frame of
//   latency (survey finding 4). Frame() and AtlasInfo() are what the lit draws of the next frame read (and the translucent draws after the
//   trigger of this one).
//
// WHAT IT COSTS ON THE RENDER THREAD, and what keeps it down. Every draw of a map costs its commands and the copy of its vertex constants (the game's
// vertex shaders read one 4 KiB bank of 256 registers through one address: only c8-c11 differ from the main view's draw, but a bank cannot be
// patched in place, the main view's own copy is still to be read by the draws before the trigger, and a vertex shader that took the matrix from
// somewhere else is a change of the shader pack), so the cost goes with the number of draws and banks, and these cut them:
//   - BY DAY nothing is collected or replayed (no headlight is lit); the draws are only looked at one frame in eight, to list the pipelines.
//   - THE CONE CULL (mcla_gpu_headlight_shadow_cone): a draw whose place (its world matrix's translation, from c8-c11) is outside every CAR's view and reach, plus a margin
//     for how far its geometry reaches from the place (7 m for a car, 40 m for any other draw: the guest's vertex buffers are not read for bounds), is dropped at
//     collection, before its bank is copied. The car's view there is ONE for both lamps (local_shadow_rules.h FitCar: a cone from behind both lamps that holds both of
//     theirs), so a draw is tested once for a car, and collected once for both lamp views; each view then keeps only its own (SphereInView), and what is the same for
//     every view (the place is a world matrix, the pixel constants, a made pipeline) is settled once.
//   - NOT ONE DRAW INTO BOTH LAMP TILES (considered 2026-11-20, HL-PER-LAMP). A caster could be drawn once with two instances and a per-instance matrix, each to its own tile
//     (SV_ViewportArrayIndex), which would halve the draws and the constant banks. It needs three things this tree does not have and a blind change should not add: (1) a
//     vertex stage in the variant pack that rewrites c8-c11 for the game's ~700 vertex shaders (the pack's vertex stages name at most four shaders, variant_pack.h
//     kVariantVertexShaders; skinned, instanced and wind vertex shaders each read the matrix their own way), (2) the viewport index written from a vertex shader on both
//     APIs: Plume enables neither `multiViewport` + `shaderOutputViewportIndex` on Vulkan nor the D3D12 option for it (a Plume PR), (3) a second matrix beside the 4 KiB
//     bank (the root descriptor reads exactly 256 registers). The gain is also small: a view costs 0.05 to 0.1 ms of the render thread, the saving at most half of the second
//     lamp's. And the cheaper sharing, drawing each caster into both tiles with one set of binds, loses to the viewport and scissor that change at every draw: for 300
//     draws the model of local_shadow_rules_test.cpp CheckSharedWork counts 2974 commands for two passes and 3477 (17 per cent more) for the interleaved order, so a car's two
//     views are drawn one after the other. What the lamps share is the collection and the planning (below), a few per cent of a view's cost.
//   - A bank is written in three pieces around c8-c11, never written twice.
// THE PLAYER'S OWN CAR. The survey (4.2 point 8) took the player's car from CarFrame so as not to draw it twice. Here it comes from the same list as
// every other draw (the main view draws it at full detail, with the game's own vertex shader), and the car that owns a view's lamps is not drawn
// into that view at all (a vehicle part whose root is within 3.5 m of the view's eye, plus the near plane): it casts nothing into its own
// headlights but still casts into another car's. CarFrame is not needed, and at night nothing asks for it (the contact shadows are gated off).
//
// THE TEST LIGHT. mcla_gpu_headlight_shadow_test 1 makes a map from a light of its own at the camera whenever no car's headlight is lit (by day
// too), published to nobody: with mcla_gpu_headlight_shadow_view 1 the map from the camera's own point of view must look like the main view's depth
// (survey CHECK C10: c8-c11 as the position matrix in every draw), which tells the replay's matrices before the rig's lamps exist.
//
// THE PIPELINES. One per (vertex shader, vertex declaration layout, topology, faces, pixel shader for the alpha tested), the game's own shaders
// and layout (the guest pipeline layout), made on the host builder (HostPrecache::AddLive) the first time any main-view draw of that shape is
// seen while the setting is on, which is every frame of the game view, so by night the daytime's have been made. A draw whose pipeline is not
// made yet is left out of the map for now (counted, never waited for).
//
// THE LOG. "[local-shadow] ..." lines: one when the choice changes, one at the first map and then every 1800 frames while the feature runs:
// views, draws replayed (of the collected), skipped by kind (the cone cull's too), the vertex constant banks written, the atlas, the time of the
// pass and of the collecting on the render thread (CPU). And, from the first frame that has a car in it and then every 1800 frames, a line that counts the
// vehicle draws of that frame by vertex shader identity (collected, or why not: local_shadow_rules.h VehicleCensus; perf/scripts/vs_identities.py names the
// identities): what a car is made of in the map.
//
// THE GLUE. After the maps of a frame are recorded (and whenever they are given up) HandOver() tells headlight_rig::SetLocalShadows the views and the
// atlas's bindless slot with the fixed point/clamp sampler (kSamplerPointClamp): the lit shaders of the headlight variant compare the texels themselves, as the
// share of a kernel of 1 to 3 texels that is lit, read by four gathers of the 4 x 4 footprint and compared at the receiver's own plane, with a penumbra that is sharp at a
// contact and widens with the distance from the blocker (light_shadow_filter.h ViewLit is the CPU twin; headlight_rig_rules.h SoftShadow is the first version's 3 x 3 box).
// The rig copies the views and moves them to the lights of the set that reads them by the car (headlight_rig_rules.h RemapShadowViews).
//
// THE SHADOW ATLAS (LIGHT-UNIFY U5, shadow_atlas.h). LocalShadow is the FIRST PROVIDER of the one shadow atlas (ShadowProvider: it asks for its pool of tiles, draws into them and hands over its
// views with the id of the light each is of). With mcla_gpu_light_frame bit 8 (light_project.h kFlipAtlas) it records into the atlas object (the texture, the layout of the tiles, the clear,
// THE ONE barrier for the pixel and the compute readers, the memory fitted to the video memory budget) instead of its own atlas code below, which stays for the A/B (one release after the
// default flips). The collection, the replay and the pipelines do not move: the lamp shadows' branch (parked) edits them heavily, and the provider interface is what a second provider needs.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread. The pipelines are made on the host builder thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "geometry.h"
#include "host_passes.h"
#include "input_layout.h"
#include "light_pick_record.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "pause_world_fx.h"
#include "pipelines.h"
#include "records.h"
#include "resolve.h"
#include "shaders.h"
#include "shadow_atlas_rules.h"
#include "shadow_provider.h"
#include "temporal_identities.h"
#include "upload.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class GuestLayout;
class LampShadows;   // lamp_shadows.h
class LightManager;  // light_manager.h
class PassTiming;
class ShadowAtlas;   // shadow_atlas.h

// What the lit draws read: the atlas, as a bindless texture (a depth plane, one float) and its size.
struct LocalShadowAtlas {
  bool valid = false;        // a map of the last trigger's frame is there and holds at least one view
  uint32_t slot = 0xFFFFFFFFu;  // bindless slot of the atlas
  uint32_t width = 0;        // texels
  uint32_t height = 0;
  uint32_t tile = 0;         // texels a view
};

class LocalShadow final : public ShadowProvider {
 public:
  // The destination of the game's mid-scene depth copy in the retail build (car_shadow_map.h, ao.h): another build gives another address and
  // then there is no map, never a wrong one.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // One prepared draw of the main view, as Scene::NoteLocalShadow hands it over (valid for the call only; the geometry's buffers and the
  // pixel constants' span are valid until the frame's lists are submitted).
  struct MainDraw {
    temporal::MainMatch match;
    const PipelineKey* key = nullptr;
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;
    const InputLayout* input_layout = nullptr;
    const DrawGeometry* geometry = nullptr;
    const ConstantBanks::Banks* banks = nullptr;
    const SharedConstants* shared = nullptr;
    UploadSpan ps_constants;
    uint32_t stencil_ids = 0;
  };

  LocalShadow();
  ~LocalShadow() override;
  LocalShadow(const LocalShadow&) = delete;
  LocalShadow& operator=(const LocalShadow&) = delete;

  // Makes nothing but the debug view's shader; the pipelines are made when a draw of their shape is first seen. Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing,
                  UploadRing& upload, ShaderStore& shaders, GuestLayout& layout);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  // The LightManager's picker (light_pick.h LightPicker): the cars that get views are picked with its `shadow_cars` and the level is noted into its budget each frame. Null: the replay's own. Render thread.
  void SetLightPicker(light_pick::LightPicker* picker) {
    light_picker_ = picker;
    picker_.SetPicker(picker ? &picker->shadow_cars : nullptr);
  }

  // The light manager (light_manager.h): with mcla_gpu_light_frame bit 4 the replay picks its cars and makes its views from the manager's frame of lights instead of the rig's set (LIGHT-UNIFY
  // U4c), the same lights bit for bit. Null (the default): the rig's set. Render thread.
  void SetLightManager(LightManager* light_manager) { light_manager_ = light_manager; }

  // LAMP-SHADOWS-4 (lamp_shadows.h, perf/LAMP-SHADOWS-2.md): the street lamps' shadows are a second provider of the shared atlas, drawn by THIS replay: the same collected main-view draws, replayed from
  // each picked lamp's frustum into the lamp's tiles at the same trigger, in the same record pass. While mcla_gpu_lamp_shadows is on the replay records into the shared atlas (the lamps' pools are
  // there) and, by night, collects the draws for the lamps as well as for the cars (the cone cull keeps the draws a lamp's or a car's view can see). Null (the default): nothing of it runs.
  void SetLampShadows(LampShadows* lamps) { lamps_ = lamps; }

  // The one shadow atlas (shadow_atlas.h): this replay is its first provider. With mcla_gpu_light_frame bit 8 (LIGHT-UNIFY U5, light_project.h kFlipAtlas) it records into the atlas object and
  // publishes its views through it; without, its own atlas code runs as before. Null: its own. Render thread, before the first frame.
  void SetShadowAtlas(ShadowAtlas* atlas);
  // Whether this frame records into the shadow atlas (the flip, decided at BeginFrame).
  bool using_atlas() const { return use_atlas_; }

  // ShadowProvider (shadow_provider.h): what the replay asks of the atlas, what it is told of the layout, and the views it publishes.
  shadow_atlas::ProviderId provider_id() const override { return shadow_atlas::ProviderId::kReplay; }
  void RequestPools(shadow_atlas::AtlasSpec* spec) const override;
  void OnLayout(const shadow_atlas::Layout& layout) override;
  void CollectViews(shadow_atlas::ShadowFrameBuilder* builder) const override;

  // The Scene opened a frame's first list: reads the settings, forgets the last frame's draws.
  void BeginFrame(gbuffer::PauseFxView pause_fx, bool pause_ready);
  // True while Scene::OnDraw should offer the main view's draws: every frame while a headlight is lit, one frame in a few otherwise (the pipelines are
  // listed from them).
  bool wants_draws() const { return offering_; }
  // A prepared draw of the main view's colour pass (the Scene checks nothing but wants_draws()).
  void OnMainDraw(const MainDraw& draw);
  // The game's motion blur draw (the anchor) and the composite marker: a depth copy after either is not the trigger.
  void OnAnchor() { anchor_seen_ = true; }
  void OnComposite() { composite_seen_ = true; }
  // A resolve, after ResolveCache::Execute and BEFORE the ambient occlusion's OnResolve: the history trigger is where the maps are recorded.
  // `slot` is the frame's upload slot. True when passes were recorded (the caller's list state is gone).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot);

  // What the pass that lights the world with the headlights reads: the views of the last trigger's frame, and the atlas. Empty (count 0, not
  // valid) when no map was recorded. Render thread.
  const larecomp::gpu::LocalShadowFrame& Frame() const { return published_; }
  const LocalShadowAtlas& AtlasInfo() const { return atlas_info_; }

  // The video memory the atlas holds now.
  uint64_t HeldBytes() const;

 private:
  // The depth atlas: a D32 float + S8 target (the format the renderer's own depth targets use, whose depth plane Plume views as one float on
  // both backends), a view in a bindless slot, a framebuffer of the depth alone.
  struct Atlas {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t width = 0;
    uint32_t height = 0;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  };
  // One pipeline of the replay.
  struct PipeEntry {
    local_shadow::PipeKey key;
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;
    const InputLayout* layout = nullptr;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    bool queued = false;   // in to_list_ or already handed to the precache
    bool flushed = false;  // handed to the precache: Ready() and Acquire() are meaningful
    bool failed = false;
  };
  // One collected caster.
  struct Caster {
    uint32_t pipe = 0;          // index into pipes_
    uint32_t bank = 0;          // index into the bank arena
    uint32_t shared = 0;        // index into shared_
    uint8_t kind = 0;           // local_shadow::Kind
    bool vehicle = false;
    DrawGeometry geometry;
    const InputLayout* layout = nullptr;
    UploadSpan ps_constants;    // the draw's own (alpha tested draws read it)
    double origin[3] = {};      // its world matrix's translation (filled at the trigger)
    bool wvp_ok = false;
  };
  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // OnResolve's work (OnResolve tells the headlight rig of the result).
  bool RecordMaps(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t slot);
  // OnMainDraw's work (OnMainDraw times it).
  void Collect(const MainDraw& draw);
  bool MakeDebugShader();
  // The shader/vertex facts of one vertex shader (cached by identity).
  bool VsSkinned(const ShaderStore::Entry& vs);
  // The pipeline index for the key of `draw` (made and listed on first sight).
  uint32_t PipelineFor(const MainDraw& draw, const local_shadow::PipeKey& key);
  // The pipeline of entry `i` if it is made; null with *pending set when it is listed or being made, null when it failed.
  plume::RenderPipeline* Pipeline(uint32_t i, bool* pending);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(const local_shadow::PipeKey& key, const ShaderStore::Entry* vs, const ShaderStore::Entry* ps,
                                                      const InputLayout* layout);
  // Hands the pipelines first seen since the last call to the precache's background builder (at most every few frames).
  void FlushListing();
  static uint64_t PrecacheKey(uint32_t id);

  // The frame's lights, the cars that get views and the views of each car (BeginFrame, once: the cone cull and the replay work from the same cars and views).
  void PickViews();
  // The frame's views as the lights alone say them: what Collect culls against once the camera is known.
  void PlanEarlyViews();
  // The place of a draw (the translation of its world matrix) from its c8-c11, with the frame's camera.
  void EvaluatePlace(const ConstantBanks::Banks& banks);
  // Tells the headlight rig the maps (or that there are none): its lit shaders read them. After every change of published_ and atlas_info_.
  void HandOver();
  bool EnsureAtlas(const local_shadow::AtlasLayout& layout);
  void ReleaseImages();
  // The atlas the frame records into, whichever code makes it (the shadow atlas object with the flip, else the replay's own).
  uint32_t AtlasSlot() const;
  uint32_t AtlasWidth() const;
  uint32_t AtlasHeight() const;
  void Census();
  struct Plan;  // defined in the .cpp: the frame's views and what the pass needs
  bool RecordPass(Plan& plan, plume::RenderCommandList* list, uint32_t slot);
  bool RecordDebugView(const ResolveCache::Summary& summary, plume::RenderCommandList* list, const Plan& plan);
  plume::RenderPipeline* DebugPipeline(uint32_t host_samples);

  // The frame's decisions (BeginFrame).
  local_shadow::Choice choice_;
  bool wanted_ = false;      // the setting is on and the feature is ready: the pipelines are listed
  bool offering_ = false;    // the main view's draws are offered in this frame (all of them while collecting, one frame in kListEveryFrames otherwise)
  bool collecting_ = false;  // ... and a headlight is lit (or was): the draws are stored for the replay
  int view_mode_ = 0;
  int skip_mask_ = 0;
  int cull_policy_ = 0;
  int views_override_ = 0;
  int test_mode_ = 0;
  float bias_scale_ = 1.0f;
  int cone_cull_ = 1;
  int skirt_ = 1;           // mcla_gpu_headlight_shadow_skirt
  uint32_t idle_frames_ = 0;
  uint64_t frame_ = 0;
  uint32_t lit_frames_left_ = 0;  // frames a map is still collected for after the last lit headlight

  // The frame.
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  gbuffer::PauseFxView pause_fx_ = gbuffer::PauseFxView::kNone;
  bool pause_ready_ = false;
  bool play_casters_ = false;
  bool pause_casters_ = false;
  bool trigger_last_frame_ = false;
  std::vector<Caster> casters_;
  std::vector<float> banks_;            // host order, 1024 floats (a register bank) per distinct bank version
  uint64_t last_bank_version_ = 0;
  uint32_t last_bank_index_ = 0;
  bool have_bank_ = false;
  // The frame's lights (the rig's set, as the rules read it), the cars that get a view (the player's first), and what the picking remembers across frames: who
  // holds a view (the hysteresis), the last frame's main camera (the importance's distance and screen) and which cars had the views at the last change.
  local_shadow::Light frame_lights_[4];
  uint32_t frame_light_count_ = 0;
  local_shadow::Group frame_groups_[4];     // the picked cars, the player's first
  uint32_t frame_cars_ = 0;
  local_shadow::ViewPlan frame_plan_;       // the views of those cars: a view for each headlamp, or one for the car (local_shadow_rules.h PlanViews)
  uint32_t frame_views_ = 0;
  local_shadow::ViewPicker picker_;
  light_pick::LightPicker* light_picker_ = nullptr;  // the manager's (SetLightPicker)
  ShadowAtlas* shadow_atlas_ = nullptr;              // the one shadow atlas (SetShadowAtlas, LU-5)
  bool use_atlas_ = false;                           // this frame records into it (mcla_gpu_light_frame bit 8, decided at BeginFrame)
  shadow_atlas::Layout atlas_layout_;                // the layout the atlas planned for this frame (OnLayout) and the replay's pool in it
  int replay_pool_ = -1;
  std::array<uint64_t, 4> published_ids_{};          // the id of the rig's light i, for the lights the last map was made for (CollectViews)
  LightManager* light_manager_ = nullptr;            // the manager's frame of lights (SetLightManager, LU-4c)
  LampShadows* lamps_ = nullptr;                     // the street lamps' provider (SetLampShadows, LAMP-SHADOWS-4)
  bool pass_partial_ = false;                        // the last record pass ran out of upload room (the lamps' static layers are then built again)
  std::string record_path_;  // mcla_gpu_light_pick_record: the pick's inputs of every frame, for light_pick_test (light_pick_record.h)
  std::ofstream record_file_;
  local_shadow::Camera last_camera_;
  uint32_t last_view_count_ = 0;            // cars that held views at the last change
  uint32_t last_view_keys_[local_shadow::kMaxCars] = {};
  uint32_t view_changes_ = 0;
  // The cone cull's frame: one view for each car that holds all of the car's views (the lights alone make it: the set does not change until the next present), the camera found
  // from the first banks (the same one the replay takes), and the place of the last bank version looked at.
  local_shadow::ViewSpec early_specs_[local_shadow::kMaxCars];
  uint32_t early_views_ = 0;                // cars planned
  local_shadow::Camera early_camera_;
  uint32_t early_tries_ = 0;
  bool eval_have_ = false;
  uint64_t eval_version_ = 0;
  bool eval_ok_ = false;
  double eval_origin_[3] = {};
  std::vector<SharedConstants> shared_;  // one per distinct block
  uint32_t counts_[size_t(local_shadow::Skip::kCount)] = {};  // this frame's collection verdicts
  uint32_t collected_ = 0;
  local_shadow::VehicleCensus vehicle_census_;  // this frame's vehicle draws by vertex shader (the log's "vehicle draws" line)
  uint64_t next_vehicle_log_ = 0;               // the frame from which the next such line may be written
  uint32_t vehicle_logs_ = 0;

  // What the lit draws read.
  larecomp::gpu::LocalShadowFrame published_;
  LocalShadowAtlas atlas_info_;

  // The GPU side.
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  UploadRing* upload_ = nullptr;
  ShaderStore* shaders_ = nullptr;
  GuestLayout* guest_layout_ = nullptr;
  bool ready_ = false;
  Atlas atlas_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> view_pipelines_;
  std::unordered_set<uint32_t> view_failed_;
  std::vector<PipeEntry> pipes_;
  std::unordered_map<local_shadow::PipeKey, uint32_t, local_shadow::PipeKeyHash> pipe_ids_;
  std::vector<uint32_t> to_list_;
  uint64_t next_flush_ = 0;
  std::unordered_map<uint64_t, bool> vs_skinned_;
  std::vector<ShaderStore::VertexElement> elements_;  // scratch for VsSkinned

  // What the log has said once, and the totals.
  uint32_t last_choice_logged_ = ~0u;
  uint32_t atlas_logged_ = 0;
  bool pending_logged_ = false;
  bool pipe_cap_logged_ = false;  // the pipeline table reached rules::kMaxPipelines (logged once)
  bool camera_logged_ = false;
  uint64_t triggers_ = 0;
  uint64_t maps_ = 0;
  uint64_t views_total_ = 0;
  uint64_t lamp_views_total_ = 0;  // views made from a headlamp
  uint64_t draws_total_ = 0;
  uint64_t skipped_total_[size_t(local_shadow::Skip::kCount)] = {};
  uint64_t no_map_[6] = {};
  uint32_t last_views_ = 0;
  uint32_t last_lamp_views_ = 0;
  uint32_t last_draws_ = 0;
  uint32_t last_collected_ = 0;
  uint32_t last_banks_ = 0;       // banks written into the maps in the last map (all views)
  uint64_t banks_total_ = 0;
  uint32_t last_kinds_[size_t(local_shadow::Kind::kCount)] = {};
  uint64_t last_cpu_ns_ = 0;
  uint64_t cpu_ns_total_ = 0;
  uint64_t collect_cycles_ = 0;  // this frame's collecting, TSC cycles
  uint64_t last_collect_ns_ = 0;
  uint64_t collect_ns_total_ = 0;
  uint64_t pending_draws_ = 0;
};

}  // namespace mcla::gpu
