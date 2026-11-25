#pragma once
// ===========================================================================
// MCLA GPU renderer -- the replay of the guest's frames
// ===========================================================================
// Executes the scene records of
// each guest frame -- draws, clears, resolves, tiling -- into the frame's
// command list, which the Renderer opens at the frame's first scene record
// and submits with the present (frame.h).
//
// A draw goes through Prepare, which decides everything about it that does
// not need its buffers or its targets:
//   decoded Xenos state (draw_state.h) -> constant banks rebuilt and checked
//   (constants.h) -> shaders from the pack -> host primitive (topology.h) ->
//   target formats (format.h) -> input layout (input_layout.h) -> pipeline
//   key, depth bias and pipeline (pipelines.h) -> viewport and NDC transform
//   (viewport.h) -> samplers of every fetch constant the shaders read
//   (samplers.h) and the texture tables -> the three constant buffers
//   uploaded: VS bank, PS bank (each only when it changed since the last
//   upload of the frame) and SharedConstants (only when it differs from the
//   previous draw's).
// Record then binds the draw's EDRAM targets (edram.h) and records it; state
// already bound in the list is not bound again. Clears are recorded from
// their records. The texture tables name the bindless slots the
// TextureCache gives for each fetch constant the shaders read (textures.h,
//): a resolve product, or a texture decoded from guest memory,
// whose uploads are recorded into the frame's list right there.
//
// A draw that cannot be drawn correctly is skipped, never drawn with a
// guess: the reason is counted, its first occurrences logged with the draw's
// identity. Draws with no effect (no target written, both faces culled) are
// counted apart -- they are not failures.
//
// One skip is not a failure: a draw whose pipeline a builder thread is still
// making (pipelines.h) is left out for now -- "pipeline pending", counted
// apart, not logged, not remembered by the pipeline memo -- when every target
// it renders into was drawn into in each of the last kPipelineSkipFrames guest
// frames (MaySkipForPipeline): the frame's draws are made again every frame, so
// the next frame's draw, with the pipeline by then, puts back what the skipped
// one left out. A draw into any other target waits for its pipeline instead.
// What the rule cannot know: whether the picture of a skipped draw lasts
// beyond its frame -- a resolve the game keeps for later or reads on the CPU
// (the lock of a resolve destination comes after the draws; nothing at draw
// time says one is coming) -- and whether the game draws into a target in
// every frame for the same purpose. It sees only that the target was drawn
// into in each of the last frames; a target made for one picture (car paint,
// vinyl) is not.
// While a capture runs (sync_builds.h) the rule only counts: no draw is left out for a pending
// pipeline, every one waits for it.
//
// Statistics: a line every kReportInterval guest frames and at shutdown.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "draw_census.h"
#include "draw_target_memo.h"
#include "edram.h"
#include "frame_layers.h"
#include "frame_order.h"
#include "gbuffer_material.h"
#include "gbuffer_materials.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "geometry.h"
#include "hdr_tonemap.h"
#include "input_layout.h"
#include "menu_world.h"
#include "mirror_draws.h"
#include "msaa_level.h"
#include "pipelines.h"
#include "thread_load.h"
#include "records.h"
#include "render_cpu.h"
#include "shaders.h"
#include "shadow_filter.h"
#include "stencil_ref.h"
#include "topology.h"
#include "ui_sharp.h"
#include "upload.h"
#include "upscaler_masks.h"
#include "viewport.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class GuestLayout;
class GuestOcclusion;
class PassTiming;
class SunShafts;
class GiApply;
class GiIntegration;
class RtWorldFrame;
class LocalLights;
class ScreenReflections;
class ScreenShared;
class Volumetrics;
class CarSelfRefl;
class CarDump;
class CarFrame;
class HeadlightRig;
class CarShadowMap;
class MirrorView;
class MirrorOverlay;
class LocalShadow;
class LightManager;
class CarRtScene;
class CarMirrorRt;
class MirrorGlass;
class GBuffer;
class PostAa;
class ResolveCache;
class Samplers;
class TemporalSystem;
class TextureCache;

class Scene {
 public:
  static constexpr uint64_t kReportInterval = 600;

  explicit Scene(ShaderStore& shaders) : shaders_(shaders), input_layouts_(shaders) {}
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  void Initialize(UploadRing& upload, Pipelines& pipelines, Samplers& samplers,
                  GeometryCache& geometry, EdramTargets& edram, TextureCache& textures,
                  ResolveCache& resolve, const GuestLayout& layout, const BindlessHeap& heap,
                  PassTiming& pass_timing, PostAa& post);
  // The UI layer split (frame_layers.h); not owned. Null or inactive: no draw is touched.
  void SetFrameLayers(FrameLayers* layers) { layers_ = layers; }
  // HDR stage 3 (hdr_tonemap.h): the tone map and what the game draws after it are drawn again into
  // a float image; not owned. Null or inactive: no draw is touched.
  void SetHdrToneMap(HdrToneMap* tone_map) { hdr_tone_ = tone_map; }

  // The guest's occlusion queries: while one is open, the draws are counted
  // (occlusion.h). Null: none are.
  void SetOcclusion(GuestOcclusion* occlusion) { occlusion_ = occlusion; }
  // The temporal groundwork (temporal.h): called at the frame's start, for every prepared draw,
  // at the composite marker and at the present. Null: none.
  void SetTemporal(TemporalSystem* temporal) { temporal_ = temporal; }
  // The screen-space ambient occlusion (ao.h): told of the main view's draws, the camera, the
  // depth clears and the resolves, and run at the mid-scene depth resolve. Null: none.
  void SetAmbientOcclusion(AmbientOcclusion* ao) { ao_ = ao; }
  // The sun shafts (shafts.h): told of the main view's draws, the camera and the resolves, and run at
  // the mid-scene depth resolve (the sky) and before the end-of-scene copy of the colour (the light).
  // Null: none.
  void SetSunShafts(SunShafts* shafts) { shafts_ = shafts; }
  // The volumetric light (volumetrics.h): told of the shadow collector's and the main view's draws, the camera and the resolves, run at the game's
  // mid-scene depth copy (the volume) and at its end-of-scene copy (the composite), and it has the game's lamp cones left out of a frame it made.
  void SetVolumetrics(Volumetrics* volumetrics) { vol_ = volumetrics; }
  // The car self-reflection prototype (car_selfrefl.h): told of the main view's draws, the camera and the mid-scene depth
  // copy, and asked for the constants of the car paint variant's draws. Null: none (the variant then draws as the game's).
  void SetCarSelfRefl(CarSelfRefl* car_refl) { car_refl_ = car_refl; }
  // The car dump (car_dump.h, debug-only): told of the main view's draws that write a vehicle ID in the frame it dumps, of the camera and
  // of the present. Null: none.
  void SetCarDump(CarDump* car_dump) { car_dump_ = car_dump; }
  // The player car's draws (car_frame.h): told of the main view's draws that write a vehicle ID in the frames a car effect asks for them,
  // and of the frame's start and its present. Null: none.
  void SetCarFrame(CarFrame* car_frame) { car_frame_ = car_frame; }
  // The side mirrors' glass finder (mirror_glass.h, MIRROR-VIEW MV3): told of the frame's start (it asks CarFrame for the draws) and the present (it evaluates before the frame is cleared). Null: none.
  void SetMirrorGlass(MirrorGlass* mirror_glass) { mirror_glass_ = mirror_glass; }
  // The cars' headlights (headlight_rig.h): told of the main view's lens draws, of the frame's start and its present; the draws of the lit shaders' headlight
  // variant (ShaderStore::kHeadlightVariant) take its block of lights as constants (Scene::FillVariantBlock). Null: none (the variant then is never chosen).
  void SetHeadlightRig(HeadlightRig* headlights) { headlights_ = headlights; }
  // The player car's sun shadow map (car_shadow_map.h): told of the frame boundary, the anchor, the composite marker and the mid-scene
  // depth copy, where it records its map BEFORE the ambient occlusion's passes. Null: none.
  void SetCarShadowMap(CarShadowMap* car_shadow) { car_shadow_ = car_shadow; }
  // The shared rear view (mirror_view.h): told of the frame boundary and of the guest's two markers around the mirror's phase (OnMirrorMarker); while a phase redirects, the draws and the clear of the
  // phase go into its image instead of the main target (RecordMirrorDraw, OnClear), and its picture is stamped into the HUD's rectangle at the first copy of the main colour after the phase (OnResolve).
  // Null: none.
  void SetMirrorView(MirrorView* mirror_view) { mirror_view_ = mirror_view; }
  // The side mirrors' glass overlay (mirror_overlay.h, MIRROR-VIEW MV4): told of the frame's start and of the end marker of a phase that drew a fresh picture (the camera the picture was drawn from), and run at the
  // first copy of the main colour after the phase's end, right after the HUD stamp: the car's mirror glasses drawn again with a lookup into the view's picture. Null: none.
  void SetMirrorOverlay(MirrorOverlay* mirror_overlay) { mirror_overlay_ = mirror_overlay; }
  // The headlight shadow maps (local_shadow.h): offered the main view's colour pass draws while they are on, told of the frame boundary, the
  // anchor and the composite marker, and run at the mid-scene depth copy (before the ambient occlusion's passes, as the car shadow map is). Null: none.
  void SetLocalShadow(LocalShadow* local_shadow) { local_shadow_ = local_shadow; }
  // The passive light manager (light_manager.h): told of the frame start, of every resolve after the headlight shadow maps' and of the present (before the rig makes the next
  // frame's set). It only reads; nothing reads it. Null: none.
  void SetLightManager(LightManager* light_manager) { light_manager_ = light_manager; }
  // The player's car as ray tracing structures (car_rt_scene.h): told of the anchor, the composite and the present, and run at the mid-scene
  // depth copy (the top level is built there, before the car self-reflection's own pass). Null: none.
  void SetCarRtScene(CarRtScene* car_rt) { car_rt_ = car_rt; }
  // The player's side mirrors, ray traced (car_mirror_rt.h): told of the frame's start and the present, and run at the same mid-scene depth copy, after the top level is built and the car
  // self-reflection's own pass has recorded. Null: none.
  void SetCarMirrorRt(CarMirrorRt* car_mirror) { car_mirror_ = car_mirror; }
  // The G-buffer's sidecar (gbuffer.h): the main view's lit draws carry a second colour target and, for the shaders that have the variant, write the
  // normal into it; told of the frame's start, the guest's clears of the main colour and the mid-scene depth copy. Null: none (no draw is touched).
  void SetGBuffer(GBuffer* gbuffer) { gbuffer_ = gbuffer; }
  // The shared screen products (screen_shared.h): told of the frame's start (before the ambient occlusion's, which declares its wants after it) and asked to draw their debug view
  // at the mid-scene depth copy, after the other effects. Their frame is published by the ambient occlusion's depth chain. Null: none.
  void SetScreenShared(ScreenShared* screen) { screen_ = screen; }
  // The deferred local light pass (local_lights.h): told of the frame's start (before the G-buffer's and the shared products', which read the wants it makes), fed the main view's colour
  // draws' constants (the units' vote) while it is on, and run at the mid-scene depth copy right after the ambient occlusion's apply. Null: none.
  void SetLocalLights(LocalLights* local_lights) { local_lights_ = local_lights; }
  // The screen-space reflections (ssr.h; null: none). Not owned.
  void SetScreenReflections(ScreenReflections* ssr) { ssr_ = ssr; }
  // The probe field's apply (gi_apply.h; CITY-RT M3 G3): told of the frame's start (before the G-buffer's and the shared products', which read the wants it makes) and run at the mid-scene depth copy
  // right after the ambient occlusion's apply and before the deferred lights. Null: none. Not owned.
  void SetGiApply(GiApply* gi_apply) { gi_apply_ = gi_apply; }
  // The probe field's integration (gi_integration.h; CITY-RT M3 G4): told of the frame's start BEFORE the apply (the field's frame starts there) and run at the mid-scene depth copy BEFORE the occlusion's
  // OnResolve (the field's maintenance, the probe update and the weights: the occlusion asks whether the screen-space bounce light may be skipped while it plans). Null: none. Not owned.
  void SetGiIntegration(GiIntegration* gi_integration) { gi_integration_ = gi_integration; }
  // The ray tracing world frame (rt_world_frame.h; CITY-RT FRAME): told of the frame's start, given the volumetric light's sun cascades and, at the mid-scene depth copy right after the car scene's
  // OnResolve (which builds the top level and tells it the view), made: the world's rows and the lamps of the frame, car or no car. Null: none. Not owned.
  void SetRtWorldFrame(RtWorldFrame* rt_world) { rt_world_ = rt_world; }

  // The Renderer opened the frame's first list (frame slot `slot`).
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);
  // The frame's last list is about to be submitted, outside any render pass
  // (the geometry's promotions are recorded into it); null: the frame is
  // abandoned.
  void EndFrame(plume::RenderCommandList* list);
  // Before a batch flush ends `list`: closes the render pass.
  void BeforeFlush(plume::RenderCommandList* list);
  // A batch flush replaced the open list: everything is bound again.
  void OnListChanged(plume::RenderCommandList* list);

  // `list`: the frame's open list, null when the device is lost (the banks are
  // still applied: the deltas are incremental).
  void OnDraw(const uint8_t* payload, plume::RenderCommandList* list);
  // A draw record queued behind the one being executed (stream sequence `sequence`): the textures
  // its shaders read, as its fetch constants name them, are offered to TextureCache::Prefetch
  // (decode ahead on the pool). Changes nothing the draw itself will do.
  void PrefetchTextures(uint64_t sequence, const uint8_t* payload);
  void OnClear(const ClearRecord& record, plume::RenderCommandList* list);
  void OnResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  void OnTilingBegin(const TilingBeginRecord& record, plume::RenderCommandList* list);
  void OnTilingEnd(const TilingEndRecord& record, plume::RenderCommandList* list);
  // The scene's image is composed: what the frame draws from here
  // on is screen-space. M6c.0 notes where (the census); `list` is the frame's
  // open list, for the passes that run at this point.
  void OnComposite(const CompositeRecord& record, plume::RenderCommandList* list);

  // Ultrawide: one of the messages the guest sends through a composite record's bucket
  // (mc_engine/display/ultrawide.h, IsUiMessage), in order with the draws around it. Either the bracket of
  // the UI pass's world icons, 3D movies drawn with the (widened) camera's own projection
  // (NoteWorldIcons): their draws are not squeezed. Or the bracket of one piece of the HUD
  // (NoteUiPiece): its draws are moved toward the left or the right screen edge (the top or the bottom one
  // in a tall picture), all by the same amount, by the setting hud_width (UiShiftOf). Render thread.
  void OnUiMessage(uint32_t bucket);
  // The guest's begin or end marker of the rear view mirror's phase (mirror_view_link.h kMarkerBegin / kMarkerEnd), in order with the draws around it. Unlike the UI messages it needs the open list: the
  // begin moves the frame to the view's pass, the end closes it and resolves the picture. Render thread. `tag`: the begin's second word, the field the phase's camera was built with (mirror_view_link.h
  // PhaseField, MV-J2); the end's is 0.
  void OnMirrorMarker(uint32_t bucket, uint32_t tag, plume::RenderCommandList* list);

  // A guest present: the frame boundary of the statistics.
  void OnPresent();
  // The last guest frame that presented had a 3D scene in it: the end of the post chain (the
  // composite marker) came in it. False for a movie, a loading screen or a menu on a flat background,
  // which draw no scene. The present shows such a frame as it is (mc_engine/display/ultrawide.h).
  bool last_frame_had_scene() const { return last_frame_had_scene_; }
  // Final report (at renderer shutdown).
  void Finish();

  uint64_t draws_since_flush() const { return draws_since_flush_; }

 private:
  enum class Skip : uint8_t {
    kDeviceLost,
    kVsUnreadable,
    kPsUnreadable,
    kDeclUnreadable,
    kIndexUnreadable,
    kInlineUnreadable,
    kInlineUnterminated,
    kPrimitive,
    kVsNotInPack,
    kPsNotInPack,
    kStageMismatch,
    kTargetFormat,
    kInputLayout,
    kEdramMode,
    kColorGap,
    kPipeline,
    kPipelinePending,  // a builder thread is making it (pipelines.h): the draw waits
    kUpload,
    kGeometry,     // GeometryCache::Result, counted per result
    kTarget,       // an EDRAM target the host could not make
    kFramebuffer,  // the framebuffer of its targets could not be made
    kCount,
  };
  enum class NoEffect : uint8_t { kNoTarget, kCulled, kNoPrimitive, kEmptyScissor, kCount };

  // Everything a draw needs once its buffers and targets exist.
  struct PreparedDraw {
    XenosState state;
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;
    const InputLayout* input_layout = nullptr;
    HostPrimitive primitive;
    DrawGeometry geometry;
    plume::RenderPipeline* pipeline = nullptr;
    PipelineKey key;
    DepthBias bias;
    ViewportInfo viewport;
    UploadSpan vs_constants;
    UploadSpan ps_constants;
    UploadSpan shared_constants;
    // The UI sharpening: the fetch slots of the draw's pixel shader the variant sharpens (ui_sharp.h UiSharpConstants::slots), for the draws that
    // took it.
    uint32_t ui_sharp_slots = 0;
    Scale scale;  // the resolution scale its targets are (or are made) at
    // The declared size of the surface its targets are bound through (DrawRecord::target_size_bits, as
    // SizeBits; 0 when not known): what tells a class's use of an identity from another pass's
    // (EdramTargets::DepthLayer), so every place that makes the draw's depth key needs it.
    uint32_t target_size_bits = 0;
    EdramKey memo_depth_key;
    uint64_t memo_depth_serial = 0;
    // The UI layer split: what OnDraw does before recording the draw (the key's alpha blend was
    // already changed in Prepare when the draw joins the layer).
    FrameLayers::DrawAction ui_layer = FrameLayers::DrawAction::kNone;
    // HDR stage 3: the draw is drawn again into the float scene image once it is recorded
    // (RecordHdrDuplicate), with this pipeline; the tone map's second draw also has its own shared
    // constants (the 608 bytes and the 16 of the HDR variant).
    HdrToneMap::Action hdr_action = HdrToneMap::Action::kNone;
    plume::RenderPipeline* hdr_pipeline = nullptr;
    UploadSpan hdr_shared;
    // A draw the upscalers' masks take (a car's shadow decal, a particle, a light volume, glass), while an
    // upscaler runs: the draw is drawn again into its layer's coverage image once it is recorded
    // (RecordUpscalerMask, upscaler_masks.h), with this pipeline.
    plume::RenderPipeline* mask_pipeline = nullptr;
    UpscalerMasks::Kind mask_kind = UpscalerMasks::Kind::kNone;
    // Set by Record, for the second draw.
    plume::RenderRect scissor;
    // Ultrawide: the draw is UI and is drawn squeezed sideways around the centre of the screen by
    // this factor (UiSqueezeOf); 1: as the game has it. In a TALL picture (ui_vertical) the squeeze, the
    // scissor's and the shift below are along y, about the middle of the screen's height.
    float ui_squeeze = 1.0f;
    bool ui_vertical = false;
    // The screen's height in guest pixels (the squeeze's extent when it is vertical).
    uint32_t ui_screen_height = 0;
    // Ultrawide: the factor the draw's scissor is squeezed by: ui_squeeze for the flat UI, and for a 3D
    // object of the UI (drawn whole, ui_squeeze 1) the UI's own, so that it is clipped to the middle 16:9
    // as the rest of the UI is (UiSqueezeOf).
    float ui_clip = 1.0f;
    // The draw puts nothing on the screen (its scissor is empty): the phone parked behind the pause menu
    // (UiSqueezeOf).
    bool ui_hidden = false;
    // Ultrawide: and then moved sideways by this share of the screen's width, to the right when
    // positive (UiShiftOf): the draw is one of a HUD piece that hud_width moves toward a screen edge. Vertical:
    // by this share of the screen's height, downwards when positive.
    float ui_shift = 0.0f;
    bool scene = false;  // it draws into the scene's identity, which is made at a scale of its own
    // The G-buffer (gbuffer.h): the draw carries the sidecar's planes as colour targets after the guest's (the pipeline's key has them; `key` stays the guest's) and, when
    // `gbuffer_variant` is not 0, its pixel shader is that variant of the pack (ShaderStore::kGBufferVariant or its alpha test twin, or a word composed with another
    // feature) and writes them: `gbuffer_mode` says what the draw does to them (gbuffer_rules.h PlaneMode: write, clear what it covers, or leave them). Where the backend has
    // no blend state per colour target (gbuffer_rules.h kD3d12PerTargetBlend) only the writers are attached.
    bool gbuffer_attached = false;
    uint32_t gbuffer_variant = 0;
    uint32_t gbuffer_planes = 0;  // the planes the key carries (PlaneCount: the colour slots after the guest's)
    gbuffer::PlaneMode gbuffer_mode = gbuffer::PlaneMode::kNone;
    // The per-texture material row of the texture the draw reads in its pixel shader's diffuse slot (gbuffer_materials.h): found in Prepare for a draw that writes the planes, applied to the
    // draw's material word in FillVariantBlock. Empty when there is no table, no census, or no such texture.
    gbuffer::TextureMaterials::Hit gbuffer_texture;
  };

  // kCount = prepared; otherwise why not. `no_effect` is set instead for a
  // draw that changes nothing.
  Skip Prepare(const DrawView& view, const ConstantBanks::Banks& banks, PreparedDraw& d,
               bool* no_effect, NoEffect* no_effect_reason, std::string* detail);
  EdramKey DepthKey(const PreparedDraw& d) const;
  // The frames in a row a target must have been drawn into for a draw into it
  // to wait for a pipeline (~0.5 s at 60 FPS: past a loading screen, a menu or
  // a one-shot render to texture).
  static constexpr uint32_t kPipelineSkipFrames = 30;
  // What Pipelines::Get asks on a miss (Deferral): may the draw be left out?
  // Only when every target the draw renders into was drawn into in each of the
  // last kPipelineSkipFrames frames. (A draw whose key carries the headlight's
  // word or the G-buffer's planes is not asked this: its fallback, the game's
  // own pipeline, draws instead, so it waits whatever this says: variant_wait.h.)
  struct SkipAsk {
    const Scene* scene;
    const PreparedDraw* draw;
  };
  static bool MaySkipForPipeline(const void* context);
  // A draw left out for its pipeline still counts as a draw into its targets,
  // or a frame that skipped all of a target's draws would end its run.
  void NoteTargetsDrawn(const PreparedDraw& d);
  // `scale`: the draw's targets' resolution scale; a resolve product made at a
  // larger one is read through its reduced copy (TextureCache::Bind2D). `bias`: the extra mip
  // bias of the draw's samplers, in 1/32 levels (SceneBias for a draw into the scene's identity
  // while it is drawn smaller than the output, else 0).
  void FillTextureTables(const DrawView& view, uint32_t fetch_mask, Scale scale,
                         SharedConstants& sc, int32_t bias);
  // The mip bias of the scene's draws while the scene is drawn at a scale below the images of the
  // screen's, in 1/32 levels: log2(render scale / output scale), the levels the output size would
  // have picked, plus one more (FSR's guide). Once a frame.
  int32_t SceneBias();
  // Records a prepared draw. kCount when recorded; kTarget/kFramebuffer when
  // not; *no_effect for an empty scissor.
  Skip Record(PreparedDraw& d, const DrawRecord& r, plume::RenderCommandList* list,
              bool* no_effect, std::string* detail);
  // The same for a draw of the rear view mirror's phase while it redirects (MirrorView): into the view's framebuffer with the viewport and the scissor mapped into it, no EDRAM bookkeeping, no
  // coverage check. kTarget when the draw cannot go into the view (it is left out, never drawn into the main target).
  Skip RecordMirrorDraw(PreparedDraw& d, const DrawRecord& r, plume::RenderCommandList* list, bool* no_effect, std::string* detail);
  // The guest's clear of the phase's rectangle while the phase redirects: into the view (the main target is not cleared, whatever happens here). `flags`, `color`, `z` and `stencil` as OnClear read them, the
  // rectangle in guest pixels of the main target, the keys of the two surfaces the clear named (their EDRAM targets say the formats and the host samples), RT0's exponent bias.
  void ClearMirrorView(plume::RenderCommandList* list, uint32_t flags, const float color[4], float z, uint32_t stencil, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const EdramKey& color_key,
                       int32_t color_exp_bias, const EdramKey& depth_key);
  // The first copy of the main colour after the end marker: the stamp of the view's picture into the HUD's rectangle, before the copy reads the colour. True when it recorded.
  bool RecordMirrorStamp(const ResolveRecord& record, plume::RenderCommandList* list);
  // The main colour target a copy of the game view's colour reads (the stamp's and the glass overlay's target): null with *other_copy true when the copy is not the game view's (the reader waits for the one that
  // is), null with it false when the target is not there; `key` is the colour's identity (the depth's samples). Creates nothing but a grown copy of the target (EdramTargets::Get).
  EdramTargets::Target* MirrorCopyColour(const ResolveRecord& record, plume::RenderCommandList* list, bool* other_copy, EdramKey* key);
  // The same copy, after the stamp: the side mirrors' glass drawn again into the main colour with the main depth bound (mirror_overlay.h). True when it recorded.
  bool RecordMirrorGlass(const ResolveRecord& record, plume::RenderCommandList* list);
  // EDRAM ownership (M5.2d): how the draw treats what its
  // targets' tiles held, noted in EdramTargets' tile map, and reported when
  // it depends on another identity's bytes.
  // Returns the draw's coverage check (EdramTargets::kNoCheck when it has
  // none), for Record to wrap the draw with.
  uint32_t NoteEdramAccess(const PreparedDraw& d, const DrawRecord& r,
                           EdramTargets::Target* const* colors, EdramTargets::Target* depth,
                           const plume::RenderRect& scissor);
  uint32_t NoteEdramTargets(const PreparedDraw& d, const DrawRecord& r,
                            EdramTargets::Target* const* colors, EdramTargets::Target* depth,
                            const plume::RenderRect& scissor);
  // The last draw NoteEdramAccess noted: a draw equal to it in everything the
  // notes depend on, with the map's serial unchanged, notes nothing new.
  struct EdramNoteKey {
    const void* targets[5] = {};
    int32_t scissor[4] = {};
    const void* pipeline = nullptr;
    uint32_t primitive = 0;
    uint32_t count = 0;
    uint32_t kills = 0;
    uint32_t aa_mask = 0;
    uint32_t color_control = 0;
    uint64_t serial = 0;
    bool operator==(const EdramNoteKey& o) const;
  };
  EdramNoteKey last_edram_note_;
  // Binds `framebuffer` (closing the open render pass first) unless bound.
  void BindFramebuffer(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer);
  // The UI layer split starts or ends with a draw (FrameLayers::Start/End): copies, clears and
  // draws into the UI target, outside a render pass; the draw binds everything again after.
  void UiLayersBeforeDraw(FrameLayers::DrawAction action, plume::RenderCommandList* list);
  // HDR stage 3: asks the HdrToneMap about the draw (the tone map, or a draw into its target before
  // the marker) and, for one it wants drawn again, looks the second pipeline up (a build still
  // under way leaves the second draw out: the frame has no float scene).
  void PrepareHdrDraw(PreparedDraw& d, const PipelineInputs& inputs,
                      const ConstantBanks::Banks& banks, uint32_t host_samples);
  // The draw `d` was just recorded: records it again into the float scene image (its own
  // framebuffer, pipeline, and shared constants for the tone map; everything else the draw's).
  void RecordHdrDuplicate(const PreparedDraw& d, plume::RenderCommandList* list);
  // A draw the upscalers' masks take (upscaler_masks.h) in the main view, in a frame an upscaler works on
  // and before its anchor: looks up the pipeline that draws it again into its layer's coverage image (a
  // build still under way leaves the second draw out: the frame has less of a mask).
  void PrepareUpscalerMask(PreparedDraw& d, const PipelineInputs& inputs, UpscalerMasks::Kind kind);
  // The draw `d` was just recorded: records it again into the coverage image (its own framebuffer with
  // the game's depth beside it, and pipeline; everything else the draw's), the way RecordHdrDuplicate
  // does.
  void RecordUpscalerMask(const PreparedDraw& d, plume::RenderCommandList* list);
  static bool AlwaysMaySkip(const void*) { return true; }
  void ResetListState();
  bool UploadConstants(uint32_t device, const ConstantBanks::Banks& banks,
                       const SharedConstants& shared, PreparedDraw& d);
  // A draw whose key has variant pack features that read constants after the shared block (shaderpack/variant_chain.h kFeatures:
  // the shadow filter 16 bytes (shadow_filter.h), the car paint and chrome 112 (car_selfrefl.h), the UI sharpening 16 (ui_sharp.h) ...)
  // uploads its own copy of the shared constants and the blocks of its features, in one span that replaces `shared_constants`. Each block
  // is at the place the chain's layout gives it for the draw's feature set (chain::LayoutConstants: dense in feature order, a feature alone
  // at byte 608), the same function the pack builder used for the text, so the blocks of a composed shader never overlap. The last
  // few uploads are kept for the next draws when nothing changed.
  // `ps_word` is the variant word of the pipeline the draw is recorded with: d.key.ps_variant, or the G-buffer's composed one (d.key stays the guest's).
  bool UploadVariantConstants(const ConstantBanks::Banks& banks, const SharedConstants& shared, PreparedDraw& d, uint32_t ps_word);
  // The `size` bytes of the block of `feature` for the draw `d` and its constant banks (the table's constants_bytes). A feature that is added
  // to the chain with a block adds its case here (scene.cpp), next to its table row.
  void FillVariantBlock(uint32_t feature, const ConstantBanks::Banks& banks, const PreparedDraw& d, uint8_t* out, uint32_t size);
  // The G-buffer (gbuffer.h): asks the rules (gbuffer_rules.h Decide) whether the draw carries the sidecar and what it does to it, sets d.gbuffer_attached, d.gbuffer_mode
  // and d.gbuffer_variant (the variant word of the pipeline: the G-buffer's alone, or composed with the features the draw has, the alpha test twin for an alpha test
  // draw), and returns the key the pipeline is made with: the guest's key widened by the planes' colour targets (each with the blend state of the draw's mode) and
  // with that word. The block of constants the variant reads is Scene::FillVariantBlock's, with the other features' (UploadVariantConstants).
  PipelineKey GBufferKey(const XenosState& state, const DrawRecord& r, PreparedDraw& d);
  // The camera rows of the G-buffer's constants (view_ray and camera_to_world: for the derivative normal), from the previous frame's camera; false when there is none.
  bool FillGBufferCamera(gbuffer::GBufferConstants* block, const PreparedDraw& d) const;
  // The UI sharpening (ui_sharp.h): the variant word the draw in the UI phase takes (kUiSharpVariant, or kUiSharpAlphaVariant for a draw whose key
  // is the alpha test spec variant; 0: none, the key stays the game's), with the fetch slots it sharpens left in d.ui_sharp_slots, and the census of
  // what the UI phase reads and why each draw did or did not take it when mcla_ui_sharp_census is on.
  uint32_t UiSharpDraw(const DrawView& view, const XenosState& state, PreparedDraw& d, Scale scale);
  // Where the draw stands as far as the UI sharpening goes (ui_sharp.h Context): after the marker, into one of the menus' render-to-texture views
  // (the UI map views side view), an inline draw of a frame with no scene, or not a UI draw at all.
  ui_sharp::Context UiSharpContext(const DrawView& view, const XenosState& state) const;
  void CountSkip(Skip skip, const DrawRecord& r, const std::string& detail);
  // The ambient occlusion and the sun shafts: a prepared draw, before it is recorded (only main view
  // draws are told), with the shaders' identities (the sky's draws tell the shafts where the sun is).
  void NoteAmbientOcclusion(const ConstantBanks::Banks& banks, const PreparedDraw& d,
                            uint64_t vs_identity, uint64_t ps_identity, const DrawView& view);
  // The headlight shadow maps' collector: a prepared draw, before it is recorded (only draws of the main view's colour pass are kept). `shared` is
  // the draw's shared constants as Prepare made them (the span in `d` is write-combined memory and is not read).
  void NoteLocalShadow(const ConstantBanks::Banks& banks, const SharedConstants& shared, const PreparedDraw& d);
  // A temporal upscaler works on this frame (it jitters it and will replace its colour): the
  // occlusion's noise then rotates from frame to frame, which the upscaler averages out.
  bool UpscalerRunning() const;
  // The frame's 3D world (menu_world.h) as the effects that watch the main view saw it, and one log line when
  // it changed: the world, how it is drawn (the host's samples and scale, the upscaler, HDR) and the effects in
  // it. At the end of the frame, before the effects forget it.
  void NoteMenuWorld();
  // Ultrawide (mc_engine/display/ultrawide.h): the UI the game draws after the scene is composed is made for
  // a 16:9 screen, and the present stretches the whole image over a wider window. The factor (below 1)
  // a draw's screen-space x is squeezed by around the screen's centre so that the stretch brings it
  // back to 16:9 in the middle of the window; 1 for a draw that keeps its place: before the marker,
  // not into the screen's image, a copy that blends nothing (the scene copied into the pause view's
  // target), a blended rectangle over the whole screen (a fade: a draw of inline vertices that cover
  // it), the world icons (OnUiMessage), or an aspect ratio of 16:9.
  // `clip` is set to the factor the draw's scissor is squeezed by: the same as the result, except for a
  // 3D object of the UI (a draw from a vertex buffer), which keeps its shape (result 1) but is clipped
  // to the middle 16:9 with the rest of the UI.
  float UiSqueezeOf(const DrawView& view, const XenosState& state, const PipelineKey& key,
                    const InputLayout* layout, PrimitiveConversion conversion, float* clip);
  // The share of the screen's width a squeezed UI draw is moved sideways by afterwards (to the right
  // when positive): 0 unless the draw belongs to a piece of the HUD the guest has bracketed
  // (OnUiMessage) and hud_width is above 0. A left piece moves by -t * (1 - k) / 2, a right piece by
  // t * (1 - k) / 2, for the squeeze k and t = hud_width / 100: the shift that puts the piece's edge
  // as far from the screen's edge as on a 16:9 screen when t is 1 (the squeeze took 1 - k of the
  // screen's width, half of it on each side).
  // In a TALL picture the squeeze and the shift are along the screen's height: a top piece moves by
  // -t * (1 - k) / 2 of it, a bottom piece by t * (1 - k) / 2 (positive is downwards).
  // The Sidekick phone (UiSqueezeOf marks its draws) moves as a left (bottom) piece of the HUD does.
  float UiShiftOf(float squeeze);
  // The temporal system: a prepared draw, before its constants are uploaded; may move it.
  void NoteTemporal(const DrawView& view, const ConstantBanks::Banks& banks,
                    const PreparedDraw& d, SharedConstants& shared);
  // The scene camera's cuts (cut_request.h): the anchor draw's camera is looked up among the cuts the game asked for. A hit tells the
  // temporal system and the occlusion (their histories start again) and marks the anchor's own draw for NeutralizeAnchorBlur.
  void NoteAnchorCut(const ConstantBanks::Banks& banks);
  // The anchor draw of a requested cut: its motion blur's previous-frame matrices (c69 to c164) become this frame's own, in a copy of
  // the pixel constants that only this draw reads.
  void NeutralizeAnchorBlur(const ConstantBanks::Banks& banks, PreparedDraw& d);
  void Report(const char* why);

  ShaderStore& shaders_;
  InputLayouts input_layouts_;
  ConstantBanks banks_;
  UploadRing* upload_ = nullptr;
  Pipelines* pipelines_ = nullptr;
  Samplers* samplers_ = nullptr;
  GeometryCache* geometry_ = nullptr;
  EdramTargets* edram_ = nullptr;
  TextureCache* textures_ = nullptr;
  ResolveCache* resolve_ = nullptr;
  const GuestLayout* layout_ = nullptr;
  const BindlessHeap* heap_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GuestOcclusion* occlusion_ = nullptr;
  TemporalSystem* temporal_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  SunShafts* shafts_ = nullptr;
  Volumetrics* vol_ = nullptr;
  menu_world::Announcer menu_world_;  // the one line per change of world
  CarSelfRefl* car_refl_ = nullptr;
  CarDump* car_dump_ = nullptr;
  CarFrame* car_frame_ = nullptr;
  MirrorGlass* mirror_glass_ = nullptr;
  HeadlightRig* headlights_ = nullptr;
  CarShadowMap* car_shadow_ = nullptr;
  MirrorView* mirror_view_ = nullptr;
  MirrorOverlay* mirror_overlay_ = nullptr;
  LocalShadow* local_shadow_ = nullptr;
  LightManager* light_manager_ = nullptr;
  GBuffer* gbuffer_ = nullptr;
  ScreenShared* screen_ = nullptr;
  LocalLights* local_lights_ = nullptr;
  ScreenReflections* ssr_ = nullptr;
  // The order the frame's passes ran in at the trigger and the end-of-scene copy, held to frame_order.h's table at each guest present (LIGHT-UNIFY U9).
  frame_order::Recorder order_;
  GiApply* gi_apply_ = nullptr;
  GiIntegration* gi_integration_ = nullptr;
  RtWorldFrame* rt_world_ = nullptr;
  // The draws' material words (gbuffer_material.h): the table the shader pack build wrote beside the variant pack, read once, on the first G-buffer draw after the pack is attached.
  gbuffer::MaterialTable gbuffer_materials_;
  // The per-texture rows over those words (gbuffer_materials.h): the table files, hot reloaded, and the census of what the G-buffer's draws read (MCLA_GBUFFER_CENSUS=1).
  gbuffer::TextureMaterials gbuffer_texmat_;
  CarRtScene* car_rt_ = nullptr;
  CarMirrorRt* car_mirror_ = nullptr;
  // The host MSAA level while the game runs (msaa_level.h): checked at every frame boundary.
  MsaaLevel msaa_;
  // The target the last pass mark named (PassTiming): a clear or a resolve in
  // between leaves the same framebuffer bound, and the draws after it are a new
  // stretch of that pass.
  plume::RenderFramebuffer* marked_framebuffer_ = nullptr;
  // Cleared surfaces whose header was made at another pitch/MSAA than the
  // device's RB_SURFACE_INFO, logged once each (the first 16).
  std::unordered_set<uint64_t> clear_other_pitch_logged_;
  SharedConstants base_shared_{};  // the texture tables on the null textures

  // The frame being recorded.
  plume::RenderCommandList* list_ = nullptr;
  uint32_t slot_ = 0;
  struct DeviceUpload {
    uint64_t vs_version = 0;  // 0 = nothing uploaded in this frame
    uint64_t ps_version = 0;
    UploadSpan vs;
    UploadSpan ps;
  };
  std::unordered_map<uint32_t, DeviceUpload> device_uploads_;
  // render_cpu_mask bit 4: the last device's entry of device_uploads_ (UploadConstants). Every clear of the map goes through
  // ForgetDeviceUploads, which also drops this.
  render_cpu::LastEntry<std::unordered_map<uint32_t, DeviceUpload>> last_device_upload_;
  bool device_cache_ = false;
  void ForgetDeviceUploads() {
    device_uploads_.clear();
    last_device_upload_.Forget();
  }
  SharedConstants last_shared_{};
  UploadSpan last_shared_span_;
  // The last uploads of UploadVariantConstants, one per feature set (round robin over four): the shadow collector's draws and the UI's
  // repeat their constants for hundreds of draws.
  struct VariantUpload {
    uint32_t features = 0;
    uint32_t block_bytes = 0;
    SharedConstants shared{};
    std::array<uint8_t, larecomp::shaderpack::chain::MaxConstantsBytes() - sizeof(SharedConstants)> block{};
    UploadSpan span;
  };
  std::array<VariantUpload, 4> variant_uploads_{};
  uint32_t variant_upload_next_ = 0;
  ui_sharp::Census ui_sharp_census_;
  uint64_t draws_since_flush_ = 0;
  // What the open list has bound (ResetListState forgets it).
  struct ListState {
    bool layout_bound = false;
    plume::RenderFramebuffer* framebuffer = nullptr;
    const plume::RenderPipeline* pipeline = nullptr;
    plume::RenderBufferReference constants[3];
    bool constants_bound = false;
    bool depth_bias_bound = false;
    DepthBias depth_bias;
    // The stencil reference the list holds (every guest pipeline takes it from the list: stencil_ref.h).
    stencil_ref::Held stencil_reference;
    // Rasterizer and input state as last set (reblue's
    // dirtyStates): a draw sets only what differs.
    bool viewport_bound = false;
    plume::RenderViewport viewport;
    bool scissor_bound = false;
    plume::RenderRect scissor;
    uint32_t vertex_slot_count = 0;  // 0: no vertex buffers set on the list
    std::array<plume::RenderVertexBufferView, DrawGeometry::kMaxSlots> vertex_views{};
    std::array<plume::RenderInputSlot, DrawGeometry::kMaxSlots> vertex_slots{};
    bool index_bound = false;
    plume::RenderIndexBufferView index_view;
  };
  ListState bound_;

  // What FillTextureTables resolved for a fetch constant, by its raw dwords:
  // reused while the TextureCache's epoch is unchanged (textures.h) and, for a
  // binding good for one frame only, within that frame.
  struct FetchMemo {
    uint32_t dwords[6] = {};
    uint64_t epoch = 0;     // 0 = empty
    uint64_t frame = 0;     // the frame a per-frame binding was made in, 0 = any
    uint32_t texture = 0;   // table value (bindless slot | gamma bit)
    uint32_t sampler = 0;
    uint8_t dimension = 0;  // xenos::DataDimension
    bool is_texture = false;
    uint32_t page = 0;        // physical page whose product generation it saw
    uint32_t generation = 0;
    Scale scale = Scale::None();  // the reading draw's targets' scale (a product may bind reduced)
    int32_t bias = 0;       // the draw's extra mip bias (SceneBias), in 1/32 levels
    uint64_t used = 0;      // replacement: the least recently used way goes
  };
  // 4-way set associative: ~2000 distinct fetch constants a frame thrash a
  // direct-mapped table (measured: 748 resolutions/frame with 4096 slots).
  static constexpr size_t kFetchMemoWays = 4;
  static constexpr size_t kFetchMemoSets = 2048;
  std::array<FetchMemo, kFetchMemoWays * kFetchMemoSets> fetch_memo_{};
  uint64_t fetch_memo_clock_ = 0;
  // The entry each fetch slot used last (reblue's
  // SamplerSlotCache): a slot whose fetch constant repeats skips the hash and
  // the set search; the entry is checked as the search checks a way.
  std::array<FetchMemo*, guest::kFetchGroupCount> last_fetch_{};
  uint64_t frame_serial_ = 0;
  // The scene camera's cut (NoteAnchorCut): this frame's anchor is the frame of a cut the game asked for / that anchor draw has not
  // had its blur constants made neutral yet.
  bool anchor_cut_ = false;
  bool cut_blur_pending_ = false;
  uint32_t cut_frames_ = 0;
  uint32_t cut_blur_refused_ = 0;
  // SceneBias's value for the frame it was worked out in.
  uint64_t scene_bias_serial_ = 0;
  int32_t scene_bias_ = 0;
  int32_t scene_bias_logged_ = INT32_MIN;  // the value the log last named
  // Writes one fetch constant's table entries from `memo`.
  static void ApplyFetch(const FetchMemo& memo, uint32_t slot, SharedConstants& sc);

  // Statistics: the current window and the whole run.
  struct Stats {
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t prepared = 0;
    std::array<uint64_t, size_t(Skip::kCount)> skips{};
    std::array<uint64_t, size_t(NoEffect::kCount)> no_effect{};
    std::array<uint64_t, kPrimitiveConversionCount> conversions{};
    std::array<uint64_t, size_t(GeometryCache::Result::kCount)> geometry_results{};
    uint64_t bank_checks = 0;
    uint64_t bank_mismatches = 0;
    // Draws with the pixel shader's ALPHA_TEST variant (coverage depends on
    // alpha), and of them those whose alpha test discards.
    uint64_t alpha_coverage_draws = 0;
    uint64_t alpha_test_draws = 0;
    uint64_t clip_disabled_draws = 0;
    // Alpha to mask (RB_COLORCONTROL) on a draw that writes colour: in all, on
    // single-sampled targets (a dithered per-pixel threshold), with RT0's
    // exponent bias (the shader tests the alpha before it, as the hardware),
    // and given other offsets than the game's (mcla_gpu_a2c_dither_all).
    uint64_t alpha_to_mask_draws = 0;
    uint64_t alpha_to_mask_1x = 0;
    uint64_t alpha_to_mask_biased = 0;
    uint64_t alpha_to_mask_replaced = 0;
    uint64_t depth_bias_draws = 0;
    uint64_t constant_uploads = 0;  // VS/PS banks
    uint64_t shared_uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t fetches = 0;  // fetch constants resolved to samplers
    uint64_t fetch_not_texture = 0;
    uint64_t fetch_memo_hits = 0;
    uint64_t census_scale_reads = 0;  // mcla_gpu_census_scale: products read across scales
    uint64_t recorded = 0;           // draws recorded into the list
    uint64_t clears = 0;
    uint64_t clears_color = 0;       // colour targets cleared
    uint64_t clears_depth = 0;       // depth/stencil targets cleared
    uint64_t clears_empty = 0;       // rectangle empty after the viewport/scissor
    uint64_t clears_failed = 0;      // unreadable record or target not made
    uint64_t clears_other_pitch = 0; // targets whose surface header was made at another
                                     // pitch/MSAA than the device's RB_SURFACE_INFO
    uint64_t resolves = 0;
    uint64_t tiling_begins = 0;
    uint64_t tiling_ends = 0;
    uint64_t prepare_cycles = 0;  // TSC cycles (cycles.h)
    // The rear view mirror's draws (mirror_draws.h): prepared draws whose viewport is the mirror's rectangle, the ones of them recorded, the frames that had any,
    // the TSC cycles they took to prepare and record, and the ones kept from the main view's per-draw consumers (mcla_gpu_mirror_class).
    uint64_t mirror_prepared = 0;
    uint64_t mirror_recorded = 0;
    uint64_t mirror_frames = 0;
    uint64_t mirror_cycles = 0;
    uint64_t mirror_class_skips = 0;
    uint64_t pipelines_created = 0;
    uint64_t pipelines_failed = 0;
    uint64_t pipeline_create_ns = 0;
  };
  void Add(Stats& into, const Stats& from) const;
  Stats window_;
  Stats total_;
  DrawCensus draw_census_;      // diagnostic, mcla_gpu_census_ps
  TargetCensus target_census_;  // diagnostic, mcla_gpu_census_targets
  CompositeCensus composite_census_;  // the scene's composition, always on
  // The colour target 0 of the frame's last recorded draw (null: none yet) and
  // the guest rectangle from the origin its draws covered since it became the
  // last: the target and region the scene is composed into when the composite
  // record comes.
  EdramTargets::Target* last_color0_ = nullptr;
  uint32_t last_color0_width_ = 0;
  uint32_t last_color0_height_ = 0;
  // The composite marker came in the frame being recorded; OnPresent keeps it for the present.
  bool scene_marker_seen_ = false;
  bool last_frame_had_scene_ = false;
  // From the composite marker to the guest's present: what the frame draws is screen-space, the HUD
  // and the menus (UiSqueezeOf).
  bool ui_phase_ = false;
  // Inside the guest's draw of the world icons (OnUiMessage): drawn as the game has them.
  bool ui_world_ = false;
  // Inside the guest's draw of one piece of the HUD (OnUiMessage): -1 anchored to the left edge, 1 to the
  // right edge, 0 not inside one. The same for a tall picture's top (-1) and bottom (1) edge.
  int ui_piece_side_ = 0;
  int ui_piece_vert_ = 0;
  // The piece is one that always sits at its edge (kUiEdgeLeft / kUiEdgeRight / kUiEdgeBottom): moved
  // fully, whatever hud_width is.
  bool ui_piece_at_edge_ = false;
  // The picture is tall: the draw UiSqueezeOf has just looked at is squeezed along y (the screen's height,
  // in guest pixels, is ui_screen_height_).
  bool ui_vertical_ = false;
  uint32_t ui_screen_height_ = 0;
  // The draw UiSqueezeOf has just looked at is one of the Sidekick phone's (UiShiftOf moves it with
  // hud_width), and the last UI 3D draw was one of the phone's meshes (its screen quad follows them).
  bool ui_phone_draw_ = false;
  bool ui_after_phone_mesh_ = false;
  // The pause menu's card has been drawn in this frame's UI, and the draw UiSqueezeOf has just looked at is
  // the phone parked behind it: not drawn (an empty scissor).
  bool ui_card_this_frame_ = false;
  bool ui_hide_draw_ = false;
  // How many full-screen UI draws kept the whole screen have been logged (UiSqueezeOf).
  uint32_t ultrawide_fills_logged_ = 0;
  // The vertex shaders of the UI draws from a vertex buffer (3D objects) that kept their shape, logged
  // once each (UiSqueezeOf).
  std::array<uint64_t, 12> ultrawide_3d_vs_{};
  uint32_t ultrawide_3d_logged_ = 0;
  PostAa* post_ = nullptr;  // the scene's anti-aliasing at the composite record
  FrameLayers* layers_ = nullptr;  // the UI layer split (frame_layers.h), from the composite record
  HdrToneMap* hdr_tone_ = nullptr;  // the float scene of HDR output (hdr_tonemap.h)

  // The last draw's lookups: consecutive draws mostly name
  // the same targets, shaders and pipeline. Each memo holds only while what it
  // answered from is unchanged.
  //  - targets and their framebuffer: the same EDRAM keys, a scissor that fits
  //    the targets' heights (no growth), and EdramTargets::change_serial;
  //  - pipeline: the same PipelineKey (it names the shaders and the layout);
  //  - shaders: the same identity (the pack's entries live for the session).
  struct TargetsMemo {
    uint64_t serial = 0;  // EdramTargets::change_serial when made; 0 = empty
    uint32_t color_count = 0;
    EdramKey color_keys[kColorTargetCount];
    bool has_depth = false;
    EdramKey depth_key;
    uint32_t height = 0;  // the smallest of the targets' heights
    EdramTargets::Target* colors[kColorTargetCount] = {};
    EdramTargets::Target* depth = nullptr;
    plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t gbuffer_generation = 0;  // the sidecar the framebuffer has (GBuffer::generation), 0: none
  };
  TargetsMemo targets_memo_;
  DrawTargetMemo draw_target_memo_;
  bool target_sig_ = false;
  bool layout_two_way_ = false;
  // The last draw's Xenos state and what Prepare made from it (
  // M6a.7). Three draws in four repeat the previous draw's window R and
  // surfaces byte for byte -- the bytes stand in for the dirty states reblue
  // keeps at its Set* hooks -- and then the decode, the target formats, the
  // pipeline key (for the same shaders, layout and topology), the viewport
  // and the state part of the shared constants are the previous draw's.
  struct StateMemo {
    bool valid = false;
    uint32_t device = 0;
    uint8_t registers[guest::kRegisterWindowBytes] = {};
    uint32_t surfaces[guest::kSurfaceCount] = {};
    XenosState state;
    bool formats_valid = false;
    plume::RenderFormat color_formats[kColorTargetCount] = {};
    plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN;
    bool key_valid = false;
    const ShaderStore::Entry* key_vs = nullptr;
    const ShaderStore::Entry* key_ps = nullptr;
    const InputLayout* key_layout = nullptr;
    plume::RenderPrimitiveTopology key_topology = {};
    bool key_polygonal = false;
    PipelineKey key;
    DepthBias bias;
    // The draw's targets' scale (DrawScale), good while the targets are those
    // EdramTargets had at change serial `draw_scale_serial` and the key names
    // the same first target.
    bool draw_scale_valid = false;
    uint64_t draw_scale_serial = 0;
    uint32_t draw_scale_colors = 0;
    uint32_t draw_scale_depth_format = 0;
    DrawTargetScale draw_scale;
    bool constants_valid = false;
    bool ps_writes_depth = false;
    Scale scale;  // the targets' resolution scale the viewport was made for (24.45)
    uint32_t samples = 1;  // and their host samples, for g_AlphaToMask
    ViewportInfo viewport;
    SharedConstants shared;  // before the texture tables
  };
  StateMemo state_memo_;
  PipelineKey pipeline_memo_key_{};
  plume::RenderPipeline* pipeline_memo_ = nullptr;
  uint64_t vs_memo_identity_ = 0;
  const ShaderStore::Entry* vs_memo_ = nullptr;
  uint64_t ps_memo_identity_ = 0;
  const ShaderStore::Entry* ps_memo_ = nullptr;
  // PrefetchTextures' own last shaders (the draws behind the executing one are not the ones the
  // memos above follow).
  uint64_t prefetch_vs_identity_ = 0;
  const ShaderStore::Entry* prefetch_vs_ = nullptr;
  uint64_t prefetch_ps_identity_ = 0;
  const ShaderStore::Entry* prefetch_ps_ = nullptr;

  // mcla_gpu_scene_profile (M6a): the TSC cycles each stage of a
  // draw took, summed over the window (cycles.h turns them into time). Off
  // (read at start): each Lap is a branch.
  enum class Stage : uint8_t {
    kBanks,      // the constant banks (ConstantBanks::Apply)
    kDecode,     // the Xenos state, primitive and target formats
    kShaders,    // the pack's shaders and the input layout
    kGeometry,   // GeometryCache::Prepare
    kPipeline,   // the pipeline key and its lookup
    kConstants,  // viewport and SharedConstants
    kTextures,   // the texture and sampler tables
    kUpload,     // the constants into the upload ring
    kTargets,    // EDRAM targets, their notes, barriers, framebuffer
    kBindPipeline,   // pipeline layout and pipeline
    kBindConstants,  // root descriptors (D3D12) or push constants (Vulkan)
    kBindState,      // viewport, scissor, depth bias
    kBindStreams,    // vertex and index buffers
    kDraw,           // the draw call (and a coverage check around it)
    kCount,
  };
  void LapStart();
  void Lap(Stage stage);
  // render_cpu_mask (read at start) and ab::B(): which of the render thread's small CPU savings run in this statistics window
  // (render_cpu.h). Called at the start and at each window's end, where ab::NextWindow() may have changed the variant.
  void ApplyRenderCpu();
  int32_t render_cpu_mask_ = 0;
  render_cpu::DrawSampler draw_sampler_;  // the timers of OnDraw's prepare statistic (window_.prepare_cycles)
  void ReportProfile(const char* why, double frames, const ThreadLoad::Sample& load);
  bool profile_ = false;
  bool scale_depth_bias_ = true;  // mcla_gpu_scale_depth_bias, read each frame
  // The rear view mirror's rectangle in guest pixels (mirror_draws.h, published by the guest every frame the mirror is up) and mcla_gpu_mirror_class, read each frame;
  // mirror_frame_seen_: the frame in progress has had a mirror draw.
  mirror_draws::Rect mirror_rect_;
  bool mirror_class_ = false;
  bool mirror_frame_seen_ = false;
  bool census_scale_ = false;     // mcla_gpu_census_scale, read at start
  std::unordered_set<uint64_t> census_scale_logged_;  // NoteScaleRead's cases (the first 64)
  uint64_t lap_ = 0;
  std::array<uint64_t, size_t(Stage::kCount)> stage_cycles_{};
  // How often each memo of the last draw answered (M6a.3), for the profile.
  enum class Memo : uint8_t { kTargets, kPipeline, kVertexShader, kPixelShader, kState, kTargetSig, kCount };
  std::array<uint64_t, size_t(Memo::kCount)> memo_hits_{};
  std::array<uint64_t, size_t(Memo::kCount)> memo_tries_{};
  // mcla_gpu_ab_interleave (ab.h): each variant's prepare ns/draw summed
  // over its interval windows, the first window (warm-up) left out.
  // The intervals between the guest's presents as the render thread runs them
  // (TSC cycles), per window: the frame pacing a hitch shows in (
  // V3), whoever caused it -- the guest, a pipeline made on the spot, a wait.
  std::vector<uint64_t> present_intervals_;
  uint64_t last_present_ = 0;
  double ab_sum_[2] = {};
  double ab_cpu_sum_[2] = {};  // the render thread's CPU ms/frame
  uint32_t ab_count_[2] = {};
  uint32_t ab_windows_ = 0;
  std::array<uint32_t, size_t(Skip::kCount)> skip_logged_{};
  uint32_t constants_issues_reported_ = 0;
  uint32_t mismatches_logged_ = 0;
  // The game's alpha-to-mask offsets already reported as replaced, one bit
  // per value.
  std::array<uint64_t, 8> alpha_to_mask_replaced_logged_{};  // [solid taken: +4]
};

}  // namespace mcla::gpu
