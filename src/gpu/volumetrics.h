#pragma once
// ===========================================================================
// MCLA GPU renderer -- volumetric light: beams under the lamps and the headlights, a haze light shows in, the sun through the cascades
// ===========================================================================
// perf/VOLUMETRICS-SURVEY.md is the design. The game has a flat distance fog (one full-screen quad), and in rain a cone mesh under each lamp
// (xrage_spotlightfog); nothing in it is lit. This adds a froxel volume (volumetrics_types.h: a camera aligned grid of voxels, x and y over the
// picture, z in log spaced slices, stored as a 2D slice atlas so that no 3D descriptor is needed on D3D12 or Vulkan) that is lit by compute
// passes and composited into the scene colour once. WHAT lights it: the sun through the game's own cascade shadow atlas (lamp posts and trees
// throw shafts), the street lamps of the game's per frame list, the cars' headlights (and their shadow maps), and the game's own fog colour.
//
// WHERE (the frame, as the sun shafts: shafts.h, AO-DESIGN.md 2.1): two points of the main view.
//   1. the mid-scene depth copy (phase 0x02000000 into 078B0000: shafts.h kTriggerDestination): the lamp cull and fused INJECT/INTEGRATE
//      (hlsl/volumetrics_inject_cs.hlsl, volumetrics_cull_cs.hlsl) run here, after the car shadow map, the headlights' shadow replay and the
//      ambient occlusion have recorded, so the beams see THIS frame's shadow atlas. The inputs of the frame are gathered by then: the sun's
//      constants from the main view's draws, the cascades from the shadow collector's draws (phase 0x1000), the camera from the vertex
//      constants once they are trusted else the previous frame's, the fog quad's colour of the last frame.
//   2. the first copy of the main colour after the first point (the end-of-scene copy; shafts_anchor.h's policy): the COMPOSITE
//      (hlsl/volumetrics_apply_ps.hlsl), BEFORE the copy is made, in the same blend as the shafts when active, so that the temporal upscaler, the bloom
//      and the tone map all see the beams. One full-screen pass, blend (ONE, SRC_ALPHA) on every sample of the multisampled target, depth from
//      the main depth plane. The policy keeps it alive if a build gives no such copy (then it goes in at the first point, before the fog).
// The game's lamp cones (xrage_spotlightfog, 4 pixel shader identities: upscaler_masks.cpp kLightVolume) are not drawn in a frame the volume
// recorded (the same lamps are in the volume, with depth and a medium); mcla_gpu_vol_keep_cones draws them too (the A/B in rain).
//
// SETTING: mcla_gpu_volumetrics = auto | off | medium | high | ultra (volumetrics_level.h; gpu_class.h kEffects: Low off, Mid medium, High high),
// live: the grid is made again at the next frame, the history dropped. Off costs a flag read. The debug cvars mcla_gpu_vol_* tune the look.
//
// GATING: needs a volume light source, a valid camera and a main depth plane.
// Pause, photo and garage composite at their tiled depth copy with a steady lens.
// The full map is outside the volume views.
//
// RENDER SCALE, MSAA, UPSCALERS: the grid does not depend on the render size; the composite runs at the render size and is the only per pixel
// cost; it is before the anchor, so DLSS / FSR see the beams as colour they accumulate.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are made on the host builder thread
// (host_precache.h) when the setting at the start uses them; a setting switched on later makes them in the background (HoldUntilBuilt).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "atmosphere.h"
#include "constants.h"
#include "edram.h"
#include "frame.h"
#include "host_passes.h"
#include "menu_world.h"
#include "records.h"
#include "resolve.h"
#include "shafts_anchor.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "volumetrics_level.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;

class Volumetrics {
 public:
  using Level = volumetrics::Level;

  // The destination of the game's mid-scene depth copy (the occlusion's and the sun shafts' trigger too).
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  Volumetrics() = default;
  ~Volumetrics();
  Volumetrics(const Volumetrics&) = delete;
  Volumetrics& operator=(const Volumetrics&) = delete;

  // Makes the layout and the shaders and lists the pipelines with the host precache (before the Renderer starts it). Never fails the renderer:
  // without them there is no volume (logged).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The producers behind the frozen interface (volumetrics_types.h VolInputSource): called once a frame at the first point. Null: the core's own
  // defaults only (the sun, the cascades, a haze by the wetness and the fog's colour; no lamp, no headlight).
  void SetInputSource(volumetrics::VolInputSource* source) { source_ = source; }

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame(gbuffer::PauseFxView menu_view, bool capture);
  const HostImage* CaptureLight() const { return captured_ ? &capture_ : nullptr; }
  bool CaptureEmpty() const;
  // True while the draw hooks have anything to do.
  bool wants_draws() const { return level_ != Level::kOff; }
  // The game's sun cascades of this frame as the shadow collector's draws told them (the data FillCascades gives the volume: the light matrices, the boxes, the atlas), for another pass that
  // wants the sun's shadow of the world: the car ray tracing's shaded hits (car_rt_hit_rules.h). False where the frame has none (no collector draw yet, a stale one, or night and rain,
  // when the game draws no cascades). `fade` is the collector's own: the visibility is lerp(lit, 1, fade). Render thread; valid from the frame's first collector draw.
  bool CurrentSun(volumetrics::VolSun* sun, float* fade) const;

  // Every prepared draw of the frame, before it is recorded: a draw of the sun shadow collector (its pixel shader has the shadow filter feature in the
  // variant pack: `ps_features` is ShaderStore::Entry::features) tells the cascades (PS c63 / c64-c87) and the atlas (its fetch constant 15's bindless
  // slot, from `shared_constants`, the draw's SharedConstants). It only reads what the pack lists: the volumetric light adds no shader variant.
  void OnPreparedDraw(const ConstantBanks::Banks& banks, uint32_t ps_features, const uint8_t* shared_constants);
  // A prepared draw of the main view (play or pause/map, temporal_identities.h MatchMainView): the light constants of its banks, the vertex
  // constants' view inverse, the fog quad's colour (by the pixel shader's identity).
  void OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks, uint64_t vs_identity, uint64_t ps_identity);
  // The game's motion blur draw: this frame's camera, which the next frame's first point uses (the lens, and the pose until the vertex
  // constants are trusted).
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A resolve, BEFORE ResolveCache::Execute makes its copy, with `list` the frame's open list outside any render pass: the first copy of the main
  // colour after the first point is the second point. True when the pending volume should composite at this copy.
  bool BeforeResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  bool AtMidScene() const { return pending_.valid && anchor_ == shafts_anchor::Anchor::kMidScene; }
  bool Composite(plume::RenderCommandList* list, const AtmosphereShafts* shafts, bool& merged);
  // A resolve, after Execute: the mid-scene depth copy is the first point. `frame_slot`: the FrameRing's slot being recorded (the constants'
  // buffer). `upscaler_running`: a temporal upscaler works on this frame.
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t frame_slot,
                 bool upscaler_running);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary.
  void OnPresent();
  void ResetPresetHistory() { history_ = {}; }

  // The game's lamp cones are not drawn while this is true (the volume of this frame holds the lamps).
  bool SkipsLightCones() const { return recorded_this_frame_ && !keep_cones_; }
  // The world of the frame that is ending (menu_world.h), for the scene's line per change of world, and the level in force.
  menu_world::World FrameWorld() const;
  std::string LevelName() const;
  // The video memory the images hold now.
  uint64_t HeldBytes() const;

 private:
  // The push block of the compute passes (hlsl/volumetrics_common.hlsli VolPush) and of the composite (volumetrics_apply_ps.hlsl VolApplyPush).
  struct ComputePush {
    uint32_t slots[4];
    float data[4];
  };
  static_assert(sizeof(ComputePush) == 32);
  struct ApplyPush {
    uint32_t slots[4];
    float sizes[4];
    float depth[4];
    float lin[4];
  };
  static_assert(sizeof(ApplyPush) == HostPasses::kPushConstantBytes);
  struct MergedPush {
    ApplyPush volume;
    float shafts_tint[3];
    uint32_t shafts_slot;
  };
  static_assert(sizeof(MergedPush) == 80);

  // The compute paths and the composite per colour target's sample count.
  enum Variant : uint32_t {
    kVInject = 0,
    kVIntegrate = 1,
    kVCull = 2,
    kVFused = 3,
    kVApplyBase = 4,  // + log2(samples)
    kVMergedBase = kVApplyBase + 4,
    kVariantCount = kVMergedBase + 4,
  };
  static uint32_t ApplyVariant(uint32_t host_samples, bool merged = false);
  static std::string VariantName(uint32_t variant);

  // The light constants of a main view draw, as raw dwords: VS c16.xyz (the direction the sunlight travels), PS c39.xyz (the sun colour) and VS c43.x
  // (the wetness). The commonest over the draws before the first point is the frame's (shafts.h Seen).
  struct Seen {
    uint32_t dwords[7] = {};
    uint32_t draws = 0;
  };
  static constexpr size_t kSeenSlots = 8;

  // What the first point worked out for the second.
  struct Pending {
    bool valid = false;
    EdramKey colour;   // the main colour's identity of the frame
    EdramKey depth;    // and the depth's
    uint32_t host_samples = 0;
    uint32_t depth_sample = 0;  // the host sample that is the guest's sample 0
    uint32_t render[2] = {};    // the picture in host pixels
    volumetrics::GridDesc grid;
    float far_m = 0.0f;
    float extinction_scale = 0.25f;
    float cap = 0.0f;  // the composite's soft limit on the light a pixel gets (VolWorld::mix[1]; 0: none)
    float lin[4] = {};
  };

  // Why a frame had no volume (the census says it).
  enum class Outcome : uint8_t {
    kNoTrigger,    // no resolve of the frame was the first point
    kWorld,        // the world has none: the pause menu's, the map, the garage
    kNoCamera,     // no previous frame's camera
    kNoLight,      // no usable light constants
    kGated,        // nothing to show: a medium too thin, no sun, no lamp, no headlight
    kTooSmall,     // the picture is smaller than the passes are worth
    kNotRecorded,  // a pipeline, an image or a buffer could not be made
    kSkyOnly,      // the volume was made, the second point never came (nothing was composited)
    kRecorded,     // the volume made and composited
  };
  static const char* OutcomeName(Outcome outcome);

  struct UavSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    const plume::RenderTexture* texture = nullptr;
  };

  struct GridResources {
    volumetrics::GridDesc grid;
    std::array<HostImage, 2> scatter;
    HostImage accum;
    std::array<UavSet, 3> uav;
    std::array<std::unique_ptr<plume::RenderDescriptorSet>, 2> fused;
    std::unique_ptr<plume::RenderBuffer> lamp_tiles;
    uint64_t lamp_bytes = 0;
  };

  void SwapGrid(GridResources& spare);
  void ReleaseSpareGrid(GridResources& spare);

  // The vertex constants' view inverse is the frame's own camera once it matched the anchor's (shafts.cpp, the same numbers).
  bool PoseFromVertexConstants(volumetrics::VolCamera* out) const;
  bool IsFirstPoint(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  static void PictureSize(const ResolveCache::Summary& summary, uint32_t* width, uint32_t* height);
  static bool ColourSourceKey(const ResolveRecord& record, EdramKey* key);
  EdramTargets::Target* TargetOf(const EdramKey& key, plume::RenderCommandList* list);

  // The frame's inputs: the sun, the cascades, a haze by the wetness and the fog quad's colour, then the producers' (SetInputSource).
  struct Light {
    bool valid = false;
    float to_sun[3] = {};
    float colour[3] = {};
    float wetness = 0.0f;
    double elevation = 0.0;  // degrees
  };
  bool ComputeLight(Light* out) const;
  void FillDefaults(const Light& light, const volumetrics::VolFrameContext& frame, volumetrics::VolInputs* in);
  void FillCascades(volumetrics::VolInputs* in) const;
  // The sun of FillCascades alone: false (and an empty `sun`) where the collector has nothing usable.
  bool FillSun(volumetrics::VolSun* sun) const;

  // The passes (volumetrics.cpp).
  bool RecordVolume(plume::RenderCommandList* list, uint32_t frame_slot, const Pending& plan, const volumetrics::VolGpuConstants& constants,
                    bool use_history, bool use_shadows);
  bool RecordComposite(plume::RenderCommandList* list, const Pending& plan, const AtmosphereShafts* shafts);
  bool EnsureResources(const volumetrics::GridDesc& grid);
  void ReleaseResources();
  bool EnsureConstantSet(uint32_t slot);
  bool EnsureUavSet(uint32_t index, const HostImage& image);
  bool EnsureLampTiles(const volumetrics::GridDesc& grid);
  void RetireUav(UavSet& uav);
  plume::RenderPipeline* Pipeline(uint32_t variant);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  bool MakeLayoutAndShaders();
  std::vector<uint32_t> Variants() const;
  void ListPrecache(const volumetrics::Choice& choice);
  void HoldUntilBuilt(Level* level);
  void EndFrame();
  void Census();

  // --- settings, read at the frame's start ---
  Level level_ = Level::kOff;
  uint32_t census_every_ = 0;
  uint32_t off_frames_ = 0;
  bool keep_cones_ = false;
  float test_flags_ = 0.0f;
  bool stable_ = true;      // mcla_gpu_vol_stable: the VOL-STABLE rules (volumetrics_stable.h); false is the first version, for the A/B
  bool amortize_ = true;
  uint32_t view_ = 0;       // mcla_gpu_vol_view: 0 the picture, 1 the volume's light alone, 2 the same x8, 3 x32

  // --- the frame ---
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool play_view_ = false;
  bool pause_view_ = false;
  bool pause_lens_steady_ = false;
  gbuffer::PauseFxView last_menu_run_ = gbuffer::PauseFxView::kNone;
  bool first_point_seen_ = false;
  uint32_t main_draws_ = 0;
  std::array<Seen, kSeenSlots> seen_{};
  uint32_t seen_count_ = 0;
  Seen last_seen_;
  uint32_t vs_view_inverse_[16] = {};
  uint32_t vs_view_inverse_draws_ = 0;
  Pending pending_;
  Outcome outcome_ = Outcome::kNoTrigger;
  bool recorded_this_frame_ = false;   // the volume was made this frame
  bool composited_ = false;
  bool copy_seen_ = false;
  shafts_anchor::Anchor anchor_ = shafts_anchor::Anchor::kEndOfScene;
  uint32_t frame_slot_ = 0;

  // The camera: the previous frame's, and whether the vertex constants have been shown to hold a frame's own view.
  temporal::Camera previous_;
  gbuffer::PauseFxView menu_view_ = gbuffer::PauseFxView::kNone;
  bool have_previous_ = false;
  bool previous_pause_ = false;
  uint64_t previous_frame_ = 0;
  bool vs_pose_trusted_ = false;
  // The anchor camera of the frame before previous_ (when that was the frame before it), so that a frame whose vertex pose is not trusted can move the previous camera on.
  volumetrics::VolCamera cam_older_;
  bool have_older_ = false;

  // The collector's constants (the last frame that had any) and the atlas slot.
  struct Collector {
    bool valid = false;
    uint64_t frame = 0;
    float regs[24][4] = {};        // c64-c87
    float shader_globals_z = 0.0f; // PS c63.z
    uint32_t atlas_slot = volumetrics::kNoSlot;
    uint32_t draws = 0;
  };
  Collector collector_;
  // The fog quad's constants of the last frame it was drawn in: SuperFogMaxColor (PS c64: rgb, opacity in w).
  float fog_colour_[4] = {0.05f, 0.06f, 0.08f, 0.0f};
  bool fog_seen_ = false;
  uint64_t fog_frame_ = 0;

  // The history: the previous volume's camera and grid, the frames between, the smoothed ground.
  volumetrics::HistoryUse history_;
  std::array<volumetrics::VolLamp, volumetrics::kMaxLamps> history_lamps_{};
  uint32_t history_lamp_count_ = 0;
  uint64_t history_frame_ = 0;
  Level history_level_ = Level::kOff;
  float ground_y_ = 0.0f;
  bool ground_valid_ = false;
  float wind_[2] = {};
  uint64_t last_fill_frame_ = 0;
  double last_fill_ms_ = 0.0;      // the scene clock at the last Fill (stable: the producers' dt is real time, not frames over 60)
  double history_clock_ms_ = 0.0;  // the scene clock when the history's volume was made (the history's weight follows the time between)
  float motion_ = 0.0f;            // the camera's move, smoothed over about a tenth of a second (volumetrics_stable.h HistoryMotion)
  float last_weight_ = 0.0f;       // the history's weight of the last volume (the census)
  uint32_t extrapolated_camera_ = 0;  // the volumes whose camera was the previous one moved on (VOL-STABLE-2; the census)

  // --- the GPU side ---
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  volumetrics::VolInputSource* source_ = nullptr;
  bool ready_ = false;  // the layout and the shaders exist
  bool shaders_failed_ = false;
  plume::RenderDescriptorSetBuilder cb_builder_;
  plume::RenderDescriptorSetBuilder uav_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> inject_shader_;
  std::unique_ptr<plume::RenderShader> integrate_shader_;
  plume::RenderDescriptorSetBuilder fused_uav_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> fused_layout_;
  std::unique_ptr<plume::RenderShader> cull_shader_;
  std::unique_ptr<plume::RenderShader> fused_shader_;
  std::array<std::unique_ptr<plume::RenderShader>, 4> apply_shaders_;  // plain/MS depth, standalone/merged
  std::unique_ptr<plume::RenderPipelineLayout> merged_layout_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> pipeline_failed_{};
  bool listed_ = false;
  bool built_ = false;
  // The images: the scatter volume twice (this frame's and last frame's), the accumulated one; all RGBA16F, the 2D atlas.
  std::array<HostImage, 2> scatter_;
  HostImage accum_;
  HostImage capture_;
  std::array<std::unique_ptr<plume::RenderPipeline>, 2> capture_pipelines_;
  bool capture_wanted_ = false;
  bool capture_off_ = false;
  bool captured_ = false;
  uint32_t parity_ = 0;  // the scatter written this frame (the other is the history)
  volumetrics::GridDesc made_for_;
  std::array<GridResources, 2> spare_grids_;
  // One constants buffer and set per frame slot, one UAV set per image.
  std::array<std::unique_ptr<plume::RenderBuffer>, kFramesInFlight> cb_;
  std::array<std::unique_ptr<plume::RenderDescriptorSet>, kFramesInFlight> cb_set_;
  std::array<UavSet, 3> uav_sets_;  // scatter 0, scatter 1, accum
  std::array<std::unique_ptr<plume::RenderDescriptorSet>, 2> fused_sets_;
  std::unique_ptr<plume::RenderBuffer> lamp_tiles_;
  uint64_t lamp_tile_bytes_ = 0;
  uint32_t unused_frames_ = 0;
  bool resources_failed_logged_ = false;
  bool first_logged_ = false;

  shafts_anchor::Policy policy_;
  uint32_t last_level_logged_ = ~0u;
  uint32_t census_frames_logged_ = 0;
  uint64_t runs_ = 0;
  uint64_t gated_ = 0;
  uint64_t lost_ = 0;
  uint64_t cones_skipped_ = 0;
  uint64_t triggers_ = 0;
  uint64_t skipped_no_camera_ = 0;
  uint64_t volumes_ = 0;         // the frames that made the passes (the census: how many of them had the frame's own camera)
  uint64_t current_camera_ = 0;  // ... of which the vertex constants gave the frame's own camera, not the previous frame's
  float census_sun_[4] = {};     // the last usable light constants' direction TO the sun (x, y, z, in the game's world: heading = atan2(x, z)) and its elevation in degrees: the census says it,
                                 // so that a scene's camera can be aimed at the sun (chain-volumetrics.ps1's dusk views)
  bool census_sun_valid_ = false;
  float census_sun_colour_[3] = {};  // the sun's colour of the frame (PS c39) as the first point read it, and the collector's fade (ShaderGlobals.z; sun_dir.w): the census says
  float census_sun_fade_ = 0.0f;     // them, to calibrate the sun's gain against a dusk picture (VOL-LOOK-TUNE CHECK)
  // The look of the last frame (volumetrics_rules.h LookFor): the weights of the weathers and hours and the cell they came to, for the census.
  volumetrics::LookWeights look_weights_;
  volumetrics::VolLook look_;
};

}  // namespace mcla::gpu
