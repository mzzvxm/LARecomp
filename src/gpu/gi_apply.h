#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's apply (CITY-RT M3, package G3)
// ===========================================================================
// perf/CITY-RT-M3.md section 4 is the design, gi_frame.h (G0) and gi_field.h (G1) the field it reads, gi_apply_rules.h THE RULES (and the twin of the shader's maths: gi_apply_test.cpp checks the
// two), hlsl/gi_apply_ps.hlsl the passes. This file is the Plume side, modelled on the deferred local light pass (local_lights.h) and the screen-space reflections (ssr.h):
//
//   WHEN      at the game's mid-scene depth copy (the trigger), AFTER the ambient occlusion's apply and BEFORE the deferred lights (scene.cpp OnResolve, step 3a: the occlusion has multiplied the
//             pixel's colour, this adds the bounce light and multiplies it by the occlusion's visibility itself; then the lamps' direct light, which neither of them multiplies).
//   WHAT      one full screen additive draw into the main colour (and, above 1080 lines of render height, a lookup draw at the occlusion's traced size before it, into an RGBA16F image): for every pixel
//             the G-buffer has a normal and an albedo for, albedo x the field's net irradiance, with the bounce light's cap, ceiling and vehicle rules (gi_apply_rules.h has the formula and the reasons).
//   NEEDS     the field's published frame (GiField::frame(), gpu_frame()), the G-buffer's RT1 (world normal) and RT2 (albedo, lit weight) (GBuffer::Want kRtBounce), the shared depth chain's level 0
//             and camera (ScreenShared::Want kRtBounce: the occlusion's depth chain runs for it when the occlusion is off), the occlusion's visibility image (AmbientOcclusion::FrameOcclusionNow,
//             optional: without it the bounce is not multiplied) and its light constants and pose.
//   SSGI      TakeSsgiShare (below) is what the occlusion asks when it plans its bounce light (ao.cpp PlanGi): 1 with no field in the frame, (1 - fade) while the field fades in, 0 once it stands (no
//             colour chain, no ring: the plain occlusion's visibility runs). The share the occlusion RAN with is latched (NoteSsgiRan) and the field's weight is min(fade, 1 - that share): the two can
//             never add up to more than one bounce, and a frame the occlusion planned before the field was published gets no field bounce.
//
// THE PER-PIXEL PASS (G5, gi_pixel.h) is recorded by this class right before the lookup, in the same list, while the frame's layout has one (ULTRA on a strong card): the lookup then mixes the pass's denoised
// term into the field's irradiance where the pass traced the pixel and its history has grown (gi_pixel_rules.h Weight). Its two marks are 'rt gi pixel' and 'rt gi denoise'. At every other layout the pass is
// never asked and this class does what it did before it existed.
// G6 (CITY-RT, perf/DLSS-RR.md): the pass is a CONSUMER of the DENOISER row. BeginFrame tells the board whether the pass is wanted (denoiser::NoteConsumer(rr::Consumer::kRtGi, ...): the frame's layout has it and
// it is made); in a frame DLSS Ray Reconstruction takes (OnTrigger's `raw_requested`, from TemporalSystem::RawModes().RtGi()) the pass skips its filter, the lookup mixes the 1 spp signal at the weight 1, and
// RawHitImage() gives the guide pass the ray's hit distance. With the plan not RR nothing of this exists.
//
// IDENTITY. Nothing is recorded, bound or drawn, and nothing of the occlusion changes, while any of these holds: no GiField is set (G4 sets it), the field is not running this frame, its frame is not
// published (no field in it), the field's share is 0 (fade 0, or the occlusion's screen-space bounce light ran at its full strength), the strength is 0, or the frame lacks the G-buffer's planes, the
// camera or the light. A frame with the field on adds only where the field has something (an empty field, a field of zeros and a field that is the open ground add exactly nothing: gi_apply_test.cpp).
//
// THE ORDER A CALLER KEEPS (G4 wires it): GiField::BeginFrame BEFORE this class's BeginFrame (the wants follow GiField::running()), and GiField::RecordFinish BEFORE ao.OnResolve in the trigger (the
// occlusion asks TakeSsgiShare while it plans; a frame whose field is not published yet leaves the screen-space bounce light whole: the safe direction).
//
// BINDING: its own pipeline layout (the heap's texture set at spaces 0 and 1 (plain and multisampled), the samplers at space 2, a buffer set at space 3 with the field's 20 rows and the apply's 9
// (gi_apply_rules.h MakeBuffer, 464 bytes of a 512 byte upload buffer a frame in flight) and, after them, the player's cabin's 2 (gi_cabin_rules.h: 32 bytes, the buffer's last), the push block (b0, space4),
// 32 bytes). The pipelines (the lookup RGBA16F with no blend; the add, ONE / ONE,
// RGB only, at the main colour's format and the host's sample count; the debug view, SRC_ALPHA / INV_SRC_ALPHA) are made on first use, one compile each, the first frame the effect runs.
//
// THE CABIN (INTERIOR-CAM-FIX, gi_cabin_rules.h): the probes trace the city only, so the player's roof does not exist for them; on the cabin's own surfaces (the driver, the dash, the wheel, the seats)
// the field's share is multiplied by a share while the camera is inside a roofed car, with the glass's share left. mcla_gpu_gi_cabin (debug only, live) is the A/B; without the car's box or the
// material plane the rule is off for the frame and the apply is what it was.
//
// SETTINGS: mcla_gpu_rt_gi_view (debug only, live; gi_apply_rules.h lists the views). The effect itself is driven by the field's level (G4's mcla_gpu_rt_gi) through the frame the field publishes. The
// strength and the cap are the BOUNCE LIGHT STRENGTH level's (ssgi_level.h, read here exactly as the occlusion reads them, debug overrides included): the field's published `strength` is not used, so the
// owner's tuning is the same on a ray tracing card and a card without, whatever the integration fills in GiField::Inputs.
//
// Logs (info, "[gi apply]"): the first frame the pass runs (size, traced size, upsample, samples, the field's layout, strength, cap), every 600 frames the frames run and why frames did not.
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread (GPU idle).
//
// CHECK (nothing here could be run: no build, no game; each is a line of the first runs):
//   G3a  the energy against the screen-space bounce light at the same strength (C8): the canyon scene, BOUNCE LIGHT STRENGTH default, HIGH field against SSGI: the shaded wall's mean luma and the
//        bounce's colour; the dead band (kDeadBand 2%) and the ceiling's room (the pixel's own colour is estimated as albedo x (ambient + sun x N.L): lamps and the sun's shadow are not in it).
//   G3b  the pixels where the field's confidence is under 1 get less bounce, not SSGI's (the colour chain is a frame-level decision): inside walls, under the ground, past the safe range, the first
//        0.4 s after a cut. If it shows, the colour chain can keep running for those frames (a share above 0 until the field's coverage is known).
//   G3c  RT2 is the albedo before the game multiplies the vertex colour's alpha (the baked occlusion) into it (the light pass's LL2): the bounce is brighter in creases than the game's own light; the
//        occlusion's term multiplies it, as intended.
//   G3d  pixels with no RT1 or no RT2 (the families the G-buffer does not cover yet, roads and terrain: RT2 = 0) take no bounce: the same coverage as the light pass; D3D12's per target blend fix
//        (Plume #15, LL4) before judging on D3D12.
//   G3e  MSAA 2x / 4x and the upscaler's jitter: the planes and the depth are read at the guest's sample 0 (the lookup at the traced size reads the depth plane at (2x, 2y)); a one pixel fringe at silhouettes.
//   G3f  the layout (the buffer at set 3, the push block at space 4, four sets) and the upload buffer as a raw SRV on both back ends; the main depth in the shader-read layout after the occlusion's
//        apply; the lookup image's first use (cleared once: D3D12 debug layer id 1422); the debug layer and Vulkan validation clean.
//   G3g  the occlusion's visibility image is read in the shader-read layout after the occlusion's apply (it is a sampled texture there); the paint power and the fade's end are the occlusion's own.
//   G3h  the light constants the occlusion read (PS c26, c39) are in the units of the field's irradiance (the car hit's frame rows: c39 with its exposure): GiField::Inputs' sun and ambient and these
//        must be the same registers (C9); the sun direction is the world one (AmbientOcclusion::SunDirection).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "edram.h"
#include "frame.h"
#include "gi_apply_rules.h"
#include "host_passes.h"
#include "resolve.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class CarFrame;
class Device;
class FrameRing;
class GBuffer;
class GiField;
class GiPixel;
class PassTiming;
class ScreenShared;

class GiApply {
 public:
  GiApply() = default;
  ~GiApply();
  GiApply(const GiApply&) = delete;
  GiApply& operator=(const GiApply&) = delete;

  // Makes nothing but remembers the owners; the layout, shaders and pipelines are made when the effect first runs. Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The owners of what it reads (not owned; null: that part is missing and the pass does not run). The field is G4's to give (the integration owns the GiField).
  void SetGBuffer(GBuffer* gbuffer) { gbuffer_ = gbuffer; }
  void SetScreenShared(ScreenShared* screen) { screen_ = screen; }
  void SetAmbientOcclusion(AmbientOcclusion* ao) { ao_ = ao; }
  void SetGiField(const GiField* field) { field_ = field; }
  // G5's per-pixel detail pass (gi_pixel.h): run by this class before its lookup, at ULTRA on a strong card only; null (or not available): never. Not owned.
  void SetGiPixel(GiPixel* pixel) { pixel_ = pixel; }
  // The player car's draws and box of the frame (car_frame.h): the player's CABIN term needs the box (gi_cabin_rules.h, INTERIOR-CAM-FIX). BeginFrame asks for the frames while the field runs and the
  // setting (mcla_gpu_gi_cabin) is on; null: no cabin term. Not owned.
  void SetCarFrame(CarFrame* car_frame) { car_frame_ = car_frame; }

  // The Scene opened a frame's first list, BEFORE the G-buffer's and the shared products' own BeginFrame (they read the wants it makes) and after GiField::BeginFrame: reads the debug view, makes
  // the wants (RT1 and RT2, the depth chain: only while the field runs) and forgets the last frame's latch.
  void BeginFrame();
  // G6: the per-pixel pass is wanted this frame (the field runs, the frame's layout has the pass, the pass is made): what BeginFrame told the DENOISER board. The Scene asks the temporal system for a raw
  // frame only then.
  bool pixel_pass_wanted() const { return pixel_wanted_; }

  // What the occlusion asks while it plans its bounce light (ao.cpp PlanGi): the share of its strength it may run with this frame, from the field's published frame (gi_apply_rules.h SsgiShareFor);
  // latches it. 1 without a field.
  float TakeSsgiShare();
  // The occlusion tells that its bounce light ran with this share after all (0: it did not: the colour chain could not be made).
  void NoteSsgiRan(float share) { ssgi_ran_share_ = share; }
  // At the trigger, after the occlusion's OnResolve and before the light pass. `gbuffer_published` is the G-buffer's own answer for this resolve. Records the pass and returns true (the caller's list
  // state is gone). `frame_slot` is the frame's slot of the upload ring. Once a frame.
  // G6: `raw_requested` = DLSS Ray Reconstruction takes this frame and the effect's switch is on (TemporalSystem::RawModes().RtGi()): the per-pixel pass (if it runs) then hands its 1 spp signal on unfiltered.
  bool OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot, bool raw_requested = false, const float* regen_jitter = nullptr);
  // G6: the per-pixel pass's hit distance image of THIS frame, null unless the pass ran raw in it (RG16F at its own size, the distance in x, in the read state): the guide pass's last-choice hit distance.
  // Valid after OnTrigger, until the next frame's.
  const HostImage* RawHitImage() const;
  float AppliedFieldShare() const { return field_share_in_frame_; }
  const HostImage* CaptureLookup() const { return capture_lookup_ ? &lookup_ : nullptr; }
  // True while the effect has a field to apply this frame (the Scene calls OnTrigger only then).
  bool wants_draws() const { return enabled_; }

  // The bounce light level's strength (mcla_gpu_ssgi_intensity with its debug overrides): the number the apply multiplies by, and the one the integration puts in the field's frame (gi_integration.h). Any thread.
  static float LevelStrength();
  // THE WATCHDOG (G4). The screen-space bounce light is skipped for the frames the field stands (TakeSsgiShare 0); when the field stood and the apply could not add its light all the same (no G-buffer
  // planes in a cutscene, no pipeline, no camera or light, no colour target) for kStarvedFrames trigger frames in a row, the picture has had no bounce light for nothing: Starved() says so, the integration
  // idles the field for a while (it fades out and the screen-space bounce light crossfades back) and clears it. A frame with a strength of 0 (the level's own choice) is not starved.
  static constexpr uint32_t kStarvedFrames = 8;
  bool Starved() const { return starved_frames_ >= kStarvedFrames; }
  void ClearStarved() { starved_frames_ = 0; }
  // A guest present: the interval line and the idle count (the lookup image goes back after a while without use).
  void OnPresent();

  uint64_t HeldBytes() const;

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };
  // The pipelines: the lookup (no blend, RGBA16F, one sample), the add and the view at the main colour's format and sample count.
  enum class Kind : uint8_t { kLookup, kAdd, kView };
  struct PipelineEntry {
    Kind kind = Kind::kAdd;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
    uint32_t shader = 0;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };

  bool OnTriggerImpl(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot, bool raw_requested, const float* regen_jitter);
  bool EnsureLayout();
  bool EnsureSlot(uint32_t frame_slot);
  plume::RenderShader* ShaderFor(uint32_t which);
  plume::RenderPipeline* PipelineFor(Kind kind, plume::RenderFormat format, uint32_t samples, uint32_t shader);
  void Bind(plume::RenderCommandList* list, const Slot& slot) const;
  void Count(gi_apply::Skip why) { ++counts_[size_t(why)]; }
  void ReleaseImage();
  void LogInterval();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GBuffer* gbuffer_ = nullptr;
  ScreenShared* screen_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  const GiField* field_ = nullptr;
  GiPixel* pixel_ = nullptr;
  CarFrame* car_frame_ = nullptr;

  // resources
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 6> shaders_;
  std::vector<PipelineEntry> pipelines_;
  bool layout_failed_ = false;
  bool pipelines_failed_ = false;
  std::array<Slot, kFramesInFlight> slots_;
  HostImage lookup_;  // RGBA16F at the traced size: rgb = E_net x confidence, a = the linear depth the texel was looked up at
  float field_share_in_frame_ = 0.0f;
  bool capture_lookup_ = false;

  // the frame
  bool enabled_ = false;
  bool pixel_wanted_ = false;      // G6: the field runs, the layout has the per-pixel pass and it is made (BeginFrame: the DENOISER board's word for the bounce light)
  uint32_t view_ = 0;
  bool cabin_on_ = true;           // mcla_gpu_gi_cabin, read at the frame's start (the A/B of the player's cabin term)
  float ssgi_ran_share_ = 0.0f;
  bool trigger_seen_ = false;
  bool ran_this_frame_ = false;
  bool expect_add_ = false;        // this trigger frame the field stood with a strength above 0: the apply was expected to add its light (the watchdog's test)
  uint32_t starved_frames_ = 0;    // consecutive trigger frames that expected it and did not (Starved)
  uint64_t frame_ = 0;
  uint32_t idle_frames_ = 0;

  // the log
  std::array<uint64_t, size_t(gi_apply::Skip::kCount)> counts_{};
  uint64_t frames_run_ = 0;
  uint64_t frames_with_field_ = 0;
  uint64_t frames_upsampled_ = 0;
  uint64_t frames_with_visibility_ = 0;
  bool first_logged_ = false;
  bool no_pipeline_logged_ = false;
};

}  // namespace mcla::gpu
