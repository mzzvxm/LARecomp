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
//   mcla_gpu_light_shadows       auto | off | high | ultra (the pause menu's LIGHT SHADOWS: the headlights now, the street lamps later; it was
//                                mcla_gpu_headlight_shadows, which stays as an alias). high: a view of 512 x 512 for the player's lamps (one view
//                                covers both), ultra: 1024 x 1024 and the nearest other car's lamps too. `auto` is high on Mid, ultra on High, off on Low
//                                (gpu_class.h Effect::kLightShadows). Live: a change applies at the next frame.
//   COLLECT (Scene::OnDraw, every prepared draw of the main view's colour pass before the anchor): the draw's shaders, input layout, host
//   geometry (valid until the frame's lists are submitted), its vertex constants (a host copy of the bank, 4 KiB, once per bank version),
//   its shared constants (once per distinct block) and its pixel constants span. A draw is a caster when it is opaque (blend off, depth
//   test and write on, a pixel shader), draws triangles, is no impostor (the pack's computed fetch index flag) or listed billboard, and its
//   pixel shader writes no depth; the alpha tested ones (the pixel shader's spec variant 1) draw with their own pixel shader, which with no
//   colour target the driver cuts down to the alpha test. Collecting costs a few hundred bytes a draw, and only runs while the setting is on
//   and a headlight is lit (or was a moment ago).
//   REPLAY (the history trigger, the game's mid-scene depth copy into 078B0000 after every opaque draw of the main view, Scene::OnResolve
//   calls us before the ambient occlusion's passes): the rig's lights are grouped by car (a car's two lamps share a view), each group gets a
//   spot view that covers its lamps' cones (local_shadow_rules.h FitView, LightClip), and every caster is drawn into each view with the game's
//   own vertex shader, the draw's own vertex and index views, a copy of its vertex constants whose c8-c11 (gWorldViewProj) is replaced by
//   gWorldViewProj x VP_main^-1 x VP_light (local_shadow_rules.h: that is gWorld x VP_light), a copy of its shared constants whose NDC transform
//   is the pass's, and no pixel shader (or its own, for the alpha tested). The main camera VP_main is found in the same frame's stored banks
//   (DeriveCamera). Faces: the ones turned from the light are kept (mcla_gpu_headlight_shadow_cull). The lamps' own car (a vehicle's part whose
//   root is within 3.5 m of the view's eye) and everything nearer than 1.5 m to the lamps (the near plane) are left out.
//   The map is of THIS frame's draws but the lit draws that read it came BEFORE the trigger, so they read the previous frame's map: one frame of
//   latency (survey finding 4). Frame() and AtlasInfo() are what the lit draws of the next frame read (and the translucent draws after the
//   trigger of this one).
//
// WHAT IT COSTS ON THE RENDER THREAD, and what keeps it down. Every draw of a map costs its commands and the copy of its vertex constants (the game's
// vertex shaders read one 4 KiB bank of 256 registers through one address: only c8-c11 differ from the main view's draw, but a bank cannot be
// patched in place, the main view's own copy is still to be read by the draws before the trigger, and a vertex shader that took the matrix from
// somewhere else is a change of the shader pack), so the cost goes with the number of draws and banks, and these cut them:
//   - BY DAY nothing is collected or replayed (no headlight is lit); the draws are only looked at one frame in eight, to list the pipelines.
//   - THE CONE CULL (mcla_gpu_headlight_shadow_cone): a draw whose place (its world matrix's translation, from c8-c11) is outside every view's cone
//     and reach, plus a margin for how far its geometry reaches from the place (7 m for a car, 40 m for any other draw: the guest's vertex buffers are
//     not read for bounds), is dropped at collection, before its bank is copied. Each view then keeps only its own (local_shadow_rules.h SphereInView).
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
// pass and of the collecting on the render thread (CPU).
//
// THE GLUE. After the maps of a frame are recorded (and whenever they are given up) HandOver() tells headlight_rig::SetLocalShadows the views and the
// atlas's bindless slot with the fixed point/clamp sampler (kSamplerPointClamp): the lit shaders of the headlight variant compare four taps themselves.
// The rig copies the views and moves them to the lights of the set that reads them by the car (headlight_rig_rules.h RemapShadowViews).
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread. The pipelines are made on the host builder thread.
// ===========================================================================

#include <cstdint>
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
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "pipelines.h"
#include "records.h"
#include "resolve.h"
#include "shaders.h"
#include "temporal_identities.h"
#include "upload.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class GuestLayout;
class PassTiming;

// What the lit draws read: the atlas, as a bindless texture (a depth plane, one float) and its size.
struct LocalShadowAtlas {
  bool valid = false;        // a map of the last trigger's frame is there and holds at least one view
  uint32_t slot = 0xFFFFFFFFu;  // bindless slot of the atlas
  uint32_t width = 0;        // texels
  uint32_t height = 0;
  uint32_t tile = 0;         // texels a view
};

class LocalShadow {
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
  ~LocalShadow();
  LocalShadow(const LocalShadow&) = delete;
  LocalShadow& operator=(const LocalShadow&) = delete;

  // Makes nothing but the debug view's shader; the pipelines are made when a draw of their shape is first seen. Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing,
                  UploadRing& upload, ShaderStore& shaders, GuestLayout& layout);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the settings, forgets the last frame's draws.
  void BeginFrame();
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

  // The frame's views as the lights alone say them (BeginFrame): what Collect culls against once the camera is known.
  void PlanEarlyViews();
  // The place of a draw (the translation of its world matrix) from its c8-c11, with the frame's camera.
  void EvaluatePlace(const ConstantBanks::Banks& banks);
  // Tells the headlight rig the maps (or that there are none): its lit shaders read them. After every change of published_ and atlas_info_.
  void HandOver();
  bool EnsureAtlas(const local_shadow::AtlasLayout& layout);
  void ReleaseImages();
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
  uint32_t idle_frames_ = 0;
  uint64_t frame_ = 0;
  uint32_t lit_frames_left_ = 0;  // frames a map is still collected for after the last lit headlight

  // The frame.
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  bool trigger_last_frame_ = false;
  std::vector<Caster> casters_;
  std::vector<float> banks_;            // host order, 1024 floats (a register bank) per distinct bank version
  uint64_t last_bank_version_ = 0;
  uint32_t last_bank_index_ = 0;
  bool have_bank_ = false;
  // The cone cull's frame: the views the lights alone make (the set does not change until the next present), the camera found from the first
  // banks (the same one the replay takes), and the place of the last bank version looked at.
  local_shadow::ViewSpec early_specs_[4];
  uint32_t early_views_ = 0;
  local_shadow::Camera early_camera_;
  uint32_t early_tries_ = 0;
  bool eval_have_ = false;
  uint64_t eval_version_ = 0;
  bool eval_ok_ = false;
  double eval_origin_[3] = {};
  std::vector<SharedConstants> shared_;  // one per distinct block
  uint32_t counts_[size_t(local_shadow::Skip::kCount)] = {};  // this frame's collection verdicts
  uint32_t collected_ = 0;

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
  uint64_t draws_total_ = 0;
  uint64_t skipped_total_[size_t(local_shadow::Skip::kCount)] = {};
  uint64_t no_map_[6] = {};
  uint32_t last_views_ = 0;
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
