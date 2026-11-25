#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: the owner (SS-0: adopt what the AO makes, add the Hi-Z; SS-1: the colour captures, the history; SS-2: the AO's depth chain, normals and colour chain)
// ===========================================================================
// perf/LIGHT-UNIFY.md section 5. The frozen interface is screen_frame.h (ScreenFrame, Product, Consumer, Wants); the pure rules are screen_rules.h and screen_history.h; this class is the owner.
//
// SS-0 is the ADOPT step: "ScreenShared publishes what the AO already makes (it hands out depth_[], normal_, colour_[] slots; ao.cpp still builds them) and adds the new hiz_min
// pass + debug view". So nothing the AO draws changes:
//   - at the end of its depth chain (the place where it already says which image is the frame's linear depth, FrameLinearDepth) the AO hands over the images it made this frame
//     (Adopted) and ScreenShared publishes them as the frame (Frame(): slots, sizes, the camera, a generation);
//   - hiz_min is MADE HERE, one pass a level, only while a consumer (or the debug view) wants it: a min chain of the linear depth (hlsl/screen_hiz_ps.hlsl);
//   - the AO runs a depth-only frame for it (the census probe's frame without the readback) when a consumer outside the AO's family wants lin_depth or hiz_min while the
//     occlusion and the contact shadows are off (AmbientOcclusion::wants_draws, OnResolve): so the High class without SSR pays nothing, an SSR without the AO pays the depth
//     pass at the traced size and the Hi-Z;
//   - the debug view (mcla_gpu_screen_view, mcla_gpu_screen_view_level) draws a product over the main view's colour at the trigger, as the G-buffer's view does.
// With no consumer and no view nothing is recorded: the frame's pictures and passes are the ones of main (the A/B in RUN PLAN).
//
// SS-1 is the MOVE step for the colour captures and the history:
//   - colour_pre (the bounce light's colour chain, capped): the IMAGES are the owner's (EnsureColourPre / ColourPre / ReleaseColourPre: their lifetime, their memory, their publication) and
//     the call that records the chain is the owner's (CapturePre, the colour_pre step of frame_order.h's table). The pass itself, its pipelines and its constants (AoPush: the cap and the source share are
//     the AO's frame constants) stay in ao.cpp, which records it inside CapturePre: the same commands in the same place, so the picture and the pass timings are the ones of SS-0. SS-2 moved the
//     pass.
//   - colour_post: a new capture, CapturePost (the colour_post step of frame_order.h's table): the main colour averaged over its host samples into RGBA16F, uncapped, after the AO's apply, with a mip chain while the reflections
//     want it. Made ONLY while a consumer wants it (a Want of colour_post by anything but the car history in its automatic mode, or the debug view): the car self-reflection's history reads it
//     when the frame has it (below) and does not make it for itself, so the effects that run today cost what they cost.
//   - history: the last frame's colour_post (a ping-pong with the capture: no copy) with the camera pair, while a consumer wants it; and the CAMERA HISTORY (screen_history.h), kept here for
//     every consumer: OnMainDraw / OnAnchor / OnPresent take the frame's camera from the game's anchor and answer Previous / Before / Current / LensAgrees (the car self-reflection's own
//     bookkeeping moved here). The camera's exposure bias and its previous view-projection are filled from it.
//   - the car self-reflection's history pass reads colour_post instead of the multisampled colour target where the frame has it at the picture's size (car_selfrefl.h, the source setting);
//     the capture's arithmetic is the pass's own, so the history image is the same bytes (a unit check says the two shaders' maths agree, the run plan's A/B says the pictures do).
//
// WHO ASKS. A consumer calls Want(consumer, products) when its setting changes (sticky; 0 withdraws); the plan is made at BeginFrame, so a want is read at the next frame
// boundary, as the G-buffer's. The AO declares its own (lin_depth, plus normal and colour_pre where it makes them) for the census line; they never start a frame of their own.
// WantCamera(consumer, on) says the consumer reads the camera history (it costs a matrix read a frame, no GPU work).
//
// THE ORDER (scene.cpp OnResolve): BeginFrame at the frame's start, before the AO's; OnMainDraw for each draw of the main view, OnAnchor at the game's motion blur draw (before the effects'
// own), OnComposite, OnPresent at the frame's end; at the trigger (frame_order.h is the order, and the test holds it to scene.cpp and ao.cpp): CapturePre inside the AO's frame after its depth chain and normals, Publish at the END of the AO's frame (after its
// apply), CapturePost after the AO, the light pass, the sun shafts and the volumetric light and before the reflections and the car history, DrawView after the effects of the trigger, with the G-buffer's view. SS-2 moved the AO's depth, normal and colour passes here (RecordDepth, RecordNormals, RecordColourPre: ao.cpp calls them at the places it recorded them from).
//
// SS-2 is the EXTRACT step (screen_extract.h): the AO's depth chain (level 0 from the guest's depth, the two coarser levels, the full size depth of the contact shadows), the normal image (GB-E)
// and the bounce light's colour chain are recorded HERE, once a frame, into images this class owns; the AO (visibility, denoise, history, contact march, apply), the bounce light, the contact
// shadows, SSR and the car passes read them. The passes are the same shaders into images of the same sizes, formats and names, recorded in the same order at the same place of the AO's frame
// with the same push block, so the pictures are the same bits; the AO's old recording stays behind mcla_gpu_screen_extract (false: the AO records the passes itself into the same images) for an A/B.
// The AO still decides the frame's SIZES (ao_size.h, its plan) and which frame runs; it hands them over as a screen::extract::Frame. The images are kept and given back by size as the AO did
// (the owner's lifetime now: HeldBytes, Shutdown), and the owner's pipelines are made in the background the first frame a pass is wanted (ChainReady): a frame never waits for a compile.
//
// SS-3 is the FUSED Hi-Z (screen_extract.h "the fused Hi-Z"): while the Hi-Z is wanted (the reflections), the depth chain's own passes write its first levels beside their images in the same draw (a
// second render target): the depth pass writes level 0 (the depth with its marks off), the two coarser levels write the Hi-Z's levels 1 and 2, and RecordDepth then records the levels above
// (the Hi-Z's own pass, mode 1) right behind them, so that the frame's Hi-Z is whole when Publish runs and Publish records nothing for it. The images are the separate passes' to the bit
// (hlsl/screen_hiz.hlsli, screen_hiz_fuse_test.cpp); mcla_gpu_screen_hiz_fused false (read at BeginFrame, live), the fused pipelines not made yet, an image that cannot be made or an extraction
// that is the AO's own (mcla_gpu_screen_extract false) give the separate passes at Publish, as SS-0 made them. A frame never waits for a compile: the fused pipelines are listed with the host
// precache the first frame they are wanted and the frames go without them until they are made.
//
// RUN PLAN (the main session; this agent ran nothing): see the commit message of SS-1 and the CHECK lines in screen_shared.cpp.
//
// Threading: render thread only, except Initialize / Shutdown (lifecycle thread, GPU idle).
// ===========================================================================

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "ao_push.h"
#include "constants.h"
#include "edram.h"
#include "guest_frame.h"
#include "host_passes.h"
#include "resolve.h"
#include "screen_extract.h"
#include "screen_frame.h"
#include "screen_history.h"
#include "screen_rules.h"
#include "ssgi_lights.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;

class ScreenShared {
 public:
  // What the AO's frame tells the owner when it publishes the frame (the images are the owner's since SS-2: which of them this frame made, and the facts of the frame).
  struct Adopted {
    bool levels = false;           // the coarser depth levels were made this frame (the occlusion, the census's probe)
    bool depth_full = false;       // the full size depth was (the contact shadows keep the picture's size beside a half size trace)
    bool normal = false;           // the normal image was (the frame used the G-buffer's plane)
    uint32_t render[2] = {0, 0};   // the main view's size in host pixels
    uint32_t host_samples = 1;
    uint32_t sample0 = 0;
    bool pause_view = false;       // the pause menu's, the garage's or the map's world
    uint64_t owner_generation = 0; // the AO's counter, bumped when its images go
    screen::ScreenCamera camera;
  };

  ScreenShared() = default;
  ~ScreenShared();
  ScreenShared(const ScreenShared&) = delete;
  ScreenShared& operator=(const ScreenShared&) = delete;

  // Makes the shaders and the extraction's pass layout (the pipelines are made in the background when first wanted). Never fails the renderer: without them nothing is made and nothing is drawn (logged).
  void Initialize(Device& device, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The effects that read the products say which they want while they are on (screen_frame.h Consumer; 0 withdraws). Sticky: set it when the effect's setting changes. Read at the
  // next frame boundary.
  void Want(screen::Consumer consumer, uint32_t products) { wants_.Set(consumer, products); }
  // The effects that read the camera history (Cameras()) say so, sticky and read at once: the owner keeps the frames' cameras while any does (or any product is wanted).
  void WantCamera(screen::Consumer consumer, bool on);
  // True while the owner keeps the camera history: the Scene calls OnMainDraw and OnAnchor only then. An effect that asked (WantCamera), or a consumer outside the AO's family (which reads the owner's
  // camera and needs the trigger's rule: the frame's plan), or the debug view; the AO's own family keeps its cameras itself until SS-2.
  bool tracking() const { return camera_users_ != 0 || plan_.external != 0; }

  // The Scene opened a frame's first list: reads the settings and the wants, makes the frame's plan and empties the frame.
  void BeginFrame(uint64_t frame_serial);
  const screen::Plan& plan() const { return plan_; }
  // True when something outside the AO's family wants the depth chain (or the Hi-Z, or the debug view shows it): the AO then runs a frame for it although its own effects are off. The
  // plan's of this frame.
  bool NeedsDepthChain() const { return plan_.NeedsDepthChain(); }
  // True while a debug view is asked for (mcla_gpu_screen_view).
  bool view_active() const { return plan_.view != screen::View::kOff; }

  // --- what the frame's draws tell (SS-1: the camera history) -------------------------------------------------------------------------------
  // A prepared draw of the main view (temporal_identities.h MatchMainView): which kind of view the frame is (the play view's, or the pause/map world's 4 samples), and the exponent bias of the
  // main colour target the draw wrote (RB_COLOR_INFO color_exp_bias: 4 in the game; taken from the draws that write the main colour only). Only while tracking().
  void OnMainDraw(const temporal::MainMatch& match, int32_t exp_bias);
  // The game's motion blur draw: this frame's camera, read from its constants (temporal::ExtractCamera) into the camera history. A frame with draws of both kinds of view or of neither, or with
  // constants that are no camera, gives none and the history starts again. Only while tracking(); before the effects' own OnAnchor (they ask Cameras()).
  void OnAnchor(const ConstantBanks::Banks& banks);
  // The scene is composed: the UI starts (no trigger after this).
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary. Counts the frame, trades colour_post with its history, forgets the frame's draws.
  void OnPresent();
  // The frame number the camera history is kept by: the count of guest presents seen (every consumer counts the same ones).
  uint64_t frame_count() const { return frame_count_; }
  const screen::CameraHistory& Cameras() const { return cameras_; }
  // Whether this resolve is the frame's trigger (the game's mid-scene depth copy into 078B0000, before the anchor and the composite, once a frame): the rule every pass at the trigger shares.
  // The caller's own flags say what it has seen this frame (a copy after the anchor or the composite is no trigger; neither is a second one).
  static bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary, bool anchor_seen, bool composite_seen, bool trigger_seen);

  // --- the products ---------------------------------------------------------------------------------------------------------------------
  // The AO's depth chain of this frame is done (EndDepthChain): publishes the frame and, when hiz_min is wanted, records its passes on `list` (outside any render pass; the caller's list state is
  // unchanged: DrawInto leaves no framebuffer bound). Called once a frame at most, from AmbientOcclusion::Run.
  void Publish(plume::RenderCommandList* list, const Adopted& adopted);

  // colour_pre (the bounce light's colour chain): the owner's images, three levels (the traced size, 1/2 and 1/4 of it); `format` is the AO's. False when one could not be made (nothing is
  // changed then: the AO's bounce light is off for the frame). Kept while the sizes are; ReleaseColourPre gives them back (the AO does when it gives its own back).
  bool EnsureColourPre(const uint32_t sizes[screen::kColourPreLevels][2], plume::RenderFormat format);
  HostImage& ColourPre(uint32_t level) { return colour_pre_[level < screen::kColourPreLevels ? level : 0]; }
  void ReleaseColourPre();
  // Step 5 of the order, the OLD path (mcla_gpu_screen_extract false): the chain is recorded now, by `record` (the AO's own pass body, which writes the images ColourPre hands out); the owner
  // counts it made this frame, so that Publish lists colour_pre. Called only while the bounce light runs (its want of colour_pre).
  void CapturePre(const std::function<void()>& record);
  // The same step recorded HERE (SS-2): the colour target's mean over its samples (cleaned and capped), then two coarser levels, from the AO's frame block `base` (screen_extract.h
  // ColourLevel0Push / ColourMipPush). The colour target is left in the shader-read layout (the apply pass moves it back). Needs the colour pipelines (ChainReady) and the images (EnsureColourPre).
  void RecordColourPre(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, EdramTargets::Target& colour, const screen::extract::ColourRecipe& recipe);

  // --- the depth chain and the normal image (SS-2): the images the owner keeps and the passes it records ------------------------------------------------------------
  // Whether the extraction's passes are recorded here this frame (mcla_gpu_screen_extract, read at BeginFrame) or by the AO's old recording into the same images.
  bool extract_on() const { return extract_ && wide_layout_ != nullptr; }
  // Whether the Hi-Z's first levels are written by the depth chain's passes when the frame makes both (mcla_gpu_screen_hiz_fused, read at BeginFrame; SS-3). The Hi-Z levels this frame's RecordDepth wrote
  // (all of them: the levels above the fused ones too), 0 when it wrote none (Publish makes the chain the separate way).
  bool hiz_fused() const { return hiz_fused_; }
  uint32_t hiz_made_in_depth() const { return hiz_made_; }
  // The pipelines of `kinds` (screen::extract::Bit): listed with the host precache the first time they are asked for (the pass of a first frame is made in the background and the AO goes
  // without until it is: a frame never waits for a compile), and true when every one of them is made. False for good when one could not be made (logged once).
  bool ChainReady(uint32_t kinds);
  // At the start, after Initialize and BEFORE HostPrecache::Start (SS-PRECACHE): lists the pipelines of `kinds` with the boot precache, so that a cold start makes them with the rest of the boot list and
  // the first frame that wants them does not have the effects held off until they are made (the AO's own table had them there before SS-2; ChainReady is the late list, for what the start did not
  // choose). The same pipelines, the same keys: ChainReady and ChainPipeline find them listed. Returns how many were added; a kind with no shader, already listed or failed is skipped.
  uint32_t ListChainAtStart(uint32_t kinds);
  // The depth chain's images for the frame: level 0, the coarser levels while `frame.levels` (else they go back), the full size depth while `frame.full` (else it goes back). False when one could
  // not be made (logged by the pass layer). The sizes are the AO's (ao_size.h).
  bool EnsureDepth(const screen::extract::Frame& frame);
  // The SSGI's depth history: the image that trades places with level 0 at the end of a frame that ran the bounce light (EndDepthChain).
  bool EnsureDepthHistory(uint32_t width, uint32_t height);
  // Gives the depth chain's images (and the history, and the normals) back.
  void ReleaseDepth();
  void ReleaseDepthHistory();
  HostImage& Depth(uint32_t level) { return depth_[level < screen::kDepthLevels ? level : 0]; }
  HostImage& DepthFull() { return depth_full_; }
  HostImage& DepthHistory() { return depth_prev_; }
  // The normal image: traced size and, while the contact shadows keep the picture's size beside a half size trace, one at the picture's. Given back by ReleaseNormals.
  bool EnsureNormal(uint32_t width, uint32_t height);
  bool EnsureNormalFull(uint32_t width, uint32_t height);
  void ReleaseNormalFull();
  void ReleaseNormals();
  HostImage& Normal() { return normal_; }
  HostImage& NormalFull() { return normal_full_; }
  // The end of the frame's depth chain: with the bounce light on, level 0 and the history trade places now that nothing of this frame reads level 0 for the next frame's sake (the next frame's
  // depth pass writes the other); this frame's level 0 is then the history image. FrameDepth0 is this frame's level 0 either way (null before the first chain).
  void EndDepthChain(bool trade);
  const HostImage* FrameDepth0() const { return frame_depth0_; }
  // Step 2 of the order, recorded HERE: level 0 from the depth target (`stencil_slot` its stencil plane), the coarser levels while `frame.levels`, and the full size depth while `frame.full`, with
  // the labels the pass timing has always had ("ao depth", "ao contact depth"). `base` is the AO's frame block, `source_bits` its normal plane decision (slots2.w), `multisampled` the depth
  // target's samples. The depth target is in the shader-read layout (the caller transitions it).
  void RecordDepth(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, uint32_t stencil_slot, bool source_bits, bool multisampled);
  // The normal pass(es) recorded HERE: the traced image from level 0, and the picture's size one (while `frame.full` and its image exists) from the full size depth.
  void RecordNormals(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, const screen::extract::NormalRecipe& recipe);

  // colour_post (and the history): the colour_post step of frame_order.h's table. At the frame's trigger, after the AO's apply, records the capture when the plan makes it (a consumer wants it, or the history, or the view
  // shows it). True when passes were recorded (the caller's list state is gone). Once a frame.
  bool CapturePost(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // True while a capture is planned and not made yet this frame (the Scene calls CapturePost only then).
  bool capture_wanted() const { return !post_done_ && (plan_.make & screen::ProductBit(screen::kColourPost)) != 0; }

  // --- the lights' share of the bounce light's radiance (GI-BOUNCE-SS: ssgi_lights.h, screen_lights.cpp, perf/GI-BOUNCE.md) ------------------------------------------------------------------
  // The AO says, once a frame in which its bounce light runs with the lights' share (Plan::GiPlan::lights), that the colour before and after the deferred light pass is wanted. Per frame, not sticky: the
  // images go back after a couple of seconds without it.
  void WantLights(bool on) { lights_wanted_ = on; }
  bool lights_wanted() const { return lights_wanted_; }
  // The last frame's two captures for the colour chain of THIS frame (the owner's frame count), in a view of kind `pause`, at the traced size width x height, when the AO's own history holds this frame
  // (`ao_history`: its camera motion is the one the reprojection uses): the bindless slots of the colour after and before the light pass. Not usable: the first frame, a cut, another kind of view or size, and
  // any frame after one that had no light pass (the chain is then today's).
  struct LightHistory {
    bool usable = false;
    uint32_t post_slot = screen::kNoSlot;
    uint32_t pre_slot = screen::kNoSlot;
  };
  LightHistory LightHistoryFor(bool pause, uint32_t width, uint32_t height, bool ao_history) const;
  // BEFORE the deferred light pass (the Scene calls it just ahead of it, only while the pass will be asked): the main colour averaged over its host samples into the "before" image, at the traced size. True
  // when recorded (the caller's list state is gone). Once a frame; only in a frame whose AO ran the bounce light (the colour chain was recorded) and wanted the lights.
  bool CaptureLightsBefore(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // RIGHT AFTER the light pass, when it recorded: the same into the "after" image. Makes the pair the next frame's history. True when recorded. Once a frame.
  bool CaptureLightsAfter(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // What a consumer reads: valid from the trigger to the frame's end (BeginFrame empties it).
  const screen::ScreenFrame& Frame() const { return frame_; }
  // The exposure factor of the main colour target as the last tracked draw saw it (2^exp_bias: the shaders' output times this is what colour_post holds; the game's bias is 4: 16), 1 where no draw has
  // told it (only drawn while a consumer wants the screen products). The ray tracing world frame's `units` (rt_world_frame.h): read before this frame's products are published.
  float ExposureFactor() const;
  // The image behind a product of the published frame (a compute consumer needs the texture for its barrier; a pixel consumer reads the slot). Null where the frame has none. The history's
  // image is `Image(kHistory)`.
  const HostImage* Image(screen::Product product, uint32_t level = 0) const;

  // The debug view, right after the other effects of the trigger so that the picture replaces what they drew. True when passes were recorded (the caller's list state is gone).
  bool DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // The video memory the images this class holds now (the depth chain, the normals, the colour chain, the Hi-Z, the colour captures, the history).
  uint64_t HeldBytes() const;
  // The part of it that is the AO's frame's inputs (the depth chain, its full size depth and history, the normal images): what the AO's own count held before SS-2 (its log line adds it back).
  uint64_t ChainBytes() const;

 private:
  struct HizPush {
    uint32_t slots[4] = {};
    uint32_t sizes[4] = {};
  };
  static_assert(sizeof(HizPush) == 32);
  struct ColourPush {
    uint32_t slots[4] = {};  // x the source's slot, y its host samples, z the ratio of the source to this image
    uint32_t sizes[4] = {};  // xy this image's size, zw the source's
  };
  static_assert(sizeof(ColourPush) == 32);
  struct ViewPush {
    uint32_t slots[4] = {};
    uint32_t sizes[4] = {};
    uint32_t level[4] = {};
  };
  static_assert(sizeof(ViewPush) == 48);
  struct ViewPipeline {
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };
  // The extraction's layout, shaders and images (screen_extract.cpp).
  void InitializeChain(Device& device, BindlessHeap& heap);
  void ShutdownChain();
  // The extraction's pass: the AO's pass layout (the host's with a push block of 128 bytes), a full-screen triangle into `image` with `pipeline` and `push` (the AO's DrawPass, ao.cpp).
  void DrawWide(plume::RenderCommandList* list, HostImage& image, const plume::RenderPipeline* pipeline, const AoPush& push);
  // One pipeline of the extraction (screen::extract::Kind): null until it is made; ChainReady lists and takes them.
  plume::RenderPipeline* ChainPipeline(screen::extract::Kind kind);
  std::unique_ptr<plume::RenderPipeline> MakeChainPipeline(screen::extract::Kind kind);
  // The colour capture's three compilations (host_shaders.h ScreenColourPixelShader).
  enum ColourPass : uint32_t { kColourPlain, kColourMs, kColourMip, kColourPassCount };

  // The Hi-Z's pipeline: listed with the host precache the first frame it is wanted and drawn from the frame it is made (a frame never waits for a compile); null until then and
  // when it could not be made.
  plume::RenderPipeline* HizPipeline();
  std::unique_ptr<plume::RenderPipeline> MakeHizPipeline();
  // Records the chain for `depth0` (the frame's linear depth, level 0); returns the number of levels and their slots in `slots`, 0 when there is none this frame.
  uint32_t RecordHiz(plume::RenderCommandList* list, const HostImage& depth0, uint32_t slots[screen::kHizMaxLevels]);
  void ReleaseHiz();
  // The Hi-Z's images for a level 0 of width x height (one a level, kept while the size is; a different size remakes them and bumps the generation); the number of levels in `levels`. False when one could
  // not be made (logged once). RecordHiz's and the fused recording's (SS-3).
  bool EnsureHizImages(uint32_t width, uint32_t height, uint32_t* levels);
  // The Hi-Z's own pass for the levels from `from` to `levels - 1` (mode 1: the min of 2x2 of the level below, in order; from 0 also makes level 0, mode 0, from `depth0`). The caller has the images.
  void RecordHizPasses(plume::RenderCommandList* list, const HostImage* depth0, uint32_t from, uint32_t levels, plume::RenderPipeline* pipeline);
  void LogHiz(uint32_t levels, uint32_t width, uint32_t height);
  // --- the fused Hi-Z (SS-3, screen_extract.cpp) -----------------------------------------------------------------------------------------------
  // A pass of two render targets (the depth chain's image and the Hi-Z's level of its size) with the 128 byte layout, as DrawWide; false when its framebuffer could not be made (nothing is recorded then).
  bool DrawWideMrt(plume::RenderCommandList* list, HostImage& first, HostImage& second, const plume::RenderPipeline* pipeline, const AoPush& push);
  plume::RenderFramebuffer* MrtFramebufferFor(const HostImage& first, const HostImage& second);
  // Gives the cached two-target framebuffers back (after the lists that name them): the images they name go, or their owner shuts down.
  void PurgeMrt();
  // The fused recording of RecordDepth: returns the Hi-Z levels in all (the depth passes' and the levels above them) when it recorded the depth chain's levels 0 to 2 and the Hi-Z; 0 when it recorded nothing
  // (the caller records the plain passes and Publish makes the Hi-Z the separate way). `mip` is the plain coarser level's pipeline, for a fused draw that cannot be made.
  uint32_t RecordDepthFused(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& level0, plume::RenderPipeline* mip, uint32_t stencil_slot, bool source_bits,
                            bool multisampled);
  // The colour capture's pipelines, the same way: null while one is being made or when it could not be made (no capture then).
  plume::RenderPipeline* ColourPipeline(uint32_t pass);
  std::unique_ptr<plume::RenderPipeline> MakeColourPipeline(uint32_t pass);
  bool ColourPipelinesReady();
  void ReleaseColourPost();
  // The lights' share (screen_lights.cpp): the frame's start (the wants, the idle count), the images' release, and the capture both sides of the light pass share.
  void LightsBeginFrame();
  void ReleaseLights();
  bool CaptureLightsInto(HostImage& image, const char* label, const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool* pause_out);
  plume::RenderPipeline* ViewPipelineFor(plume::RenderFormat format, uint32_t samples);
  void LogWants();
  // The frame as the inputs say, with the generation of the last sealed frame; Seal gives it its own (once the frame's last product is in).
  void BuildFrame();
  void Seal();

  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  std::unique_ptr<plume::RenderShader> hiz_shader_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  std::array<std::unique_ptr<plume::RenderShader>, kColourPassCount> colour_shaders_;
  std::unique_ptr<plume::RenderPipeline> hiz_pipeline_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kColourPassCount> colour_pipelines_;
  bool hiz_listed_ = false;
  bool hiz_failed_ = false;
  bool view_failed_ = false;
  bool colour_listed_ = false;
  bool colour_failed_ = false;
  std::vector<ViewPipeline> view_pipelines_;

  // The extraction (SS-2): the pass layout (128 bytes of push constants, both stages), the shaders and pipelines of screen::extract::Kind, and the images it keeps
  Device* device_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  bool extract_ = true;              // mcla_gpu_screen_extract, read at BeginFrame
  std::unique_ptr<plume::RenderPipelineLayout> wide_layout_;
  std::array<std::unique_ptr<plume::RenderShader>, size_t(screen::extract::Kind::kCount)> chain_shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, size_t(screen::extract::Kind::kCount)> chain_pipelines_;
  uint32_t chain_listed_ = 0;        // kinds listed with the host precache (screen::extract::Bit)
  uint32_t chain_failed_ = 0;        // ... that could not be made
  std::array<HostImage, screen::kDepthLevels> depth_;  // the linear depth chain: the traced size (half the picture's above 1080 lines), then 1/2 and 1/4 of it
  HostImage depth_full_;             // the full size depth of the contact shadows beside a half size trace
  HostImage depth_prev_;             // the bounce light's depth history, the partner of depth_[0] in the ping-pong
  HostImage normal_;                 // R16G16_UNORM at the traced size
  HostImage normal_full_;            // ... and at the picture's size while the contact shadows keep it
  const HostImage* frame_depth0_ = nullptr;  // this frame's level 0 (depth_[0], or the history image after the trade)

  std::array<HostImage, screen::kHizMaxLevels> hiz_;
  uint32_t hiz_images_ = 0;          // the levels that have an image now
  uint32_t hiz_idle_frames_ = 0;     // frames since the Hi-Z was last wanted (the images go back after kHizReleaseAfterFrames)
  uint64_t hiz_generation_ = 0;      // bumped when the Hi-Z's images are made again
  uint32_t hiz_logged_size_[2] = {};
  bool hiz_images_failed_logged_ = false;
  // SS-3: the fused Hi-Z. The setting (read at BeginFrame), the levels this frame's depth passes wrote (0: none), and the two-target framebuffers kept by the serials of the images they name.
  bool hiz_fused_ = true;
  uint32_t hiz_made_ = 0;
  struct MrtFramebuffer {
    uint64_t serial[2] = {0, 0};
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::vector<MrtFramebuffer> mrt_;
  bool mrt_failed_logged_ = false;

  // colour_pre: the bounce light's colour chain (the AO records it into these)
  std::array<HostImage, screen::kColourPreLevels> colour_pre_;
  bool pre_made_ = false;            // the chain was recorded this frame (CapturePre)

  // colour_post: this frame's capture, and the last frame's level 0 (the history); they trade places at the frame's end while the history is wanted (no copy)
  std::array<HostImage, screen::kColourPostMaxLevels> post_;
  HostImage history_;
  uint32_t post_images_ = 0;         // the levels of post_ that have an image now
  uint32_t post_idle_frames_ = 0;    // frames since the capture was last planned (the images go back after kPostReleaseAfterFrames)
  uint32_t hist_idle_frames_ = 0;    // frames since the history was last wanted (its image goes back after the same)
  uint64_t colour_generation_ = 0;   // bumped when the colour images (pre, post, history) are made again; a trade of the pair is not
  bool post_done_ = false;           // the capture was made this frame
  screen::CaptureKey last_capture_;  // the last frame's capture, for the history's rule
  screen::CaptureKey this_capture_;
  bool post_images_failed_logged_ = false;
  uint32_t colour_missing_run_ = 0;       // trigger frames in a row whose depth had no colour target at its samples (screen_rules.h JudgeColourPartner)
  bool colour_missing_noted_ = false;     // the one note about such a frame was logged
  bool post_first_logged_ = false;
  uint64_t post_captures_ = 0;
  uint32_t post_logged_size_[2] = {};

  // the lights' share of the bounce light's radiance (screen_lights.cpp): the colour before and after the deferred light pass at the traced size. Each frame's pair is the next frame's history (the AO's colour chain
  // reads it before this frame's captures overwrite it, in command order), so one pair is enough: no trade.
  HostImage lights_pre_;
  HostImage lights_post_;
  ssgi_lights::Key lights_key_;      // the frame that made both, and the view kind and size they are
  bool lights_wanted_ = false;       // the AO's want of this frame (BeginFrame turns it into the idle count)
  bool lights_before_done_ = false;  // this frame's captures
  bool lights_after_done_ = false;
  bool lights_pause_ = false;
  uint32_t lights_idle_frames_ = 0;
  bool lights_failed_logged_ = false;
  bool lights_first_logged_ = false;

  // the camera history and the frame's own draws
  screen::CameraHistory cameras_;
  uint32_t camera_users_ = 0;        // bit of a Consumer per effect that reads the camera history
  uint64_t frame_count_ = 0;         // guest presents seen
  bool play_view_ = false;           // the frame's main view draws were the game view's
  bool pause_view_ = false;          // ... the pause menu's, the garage's or the map's (4 guest samples); never both in a usable frame
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  int32_t exp_bias_ = 0;             // the main colour target's exponent bias as the last draw saw it
  bool exposure_known_ = false;
  uint32_t anchor_rejects_logged_ = 0;

  screen::Wants wants_;
  screen::Plan plan_;
  uint32_t logged_wanted_ = ~0u;
  screen::Generation generation_;
  uint32_t last_generation_ = 0;
  screen::FrameInputs inputs_;
  screen::ScreenFrame frame_;
  Adopted adopted_;
  bool published_ = false;
  bool sealed_ = false;
  guest_frame::Gate guest_;          // BeginFrame runs for every list: a second one after the frame was published keeps the frame (guest_frame.h, review F2)
  uint64_t frame_serial_ = 0;
  uint64_t published_frames_ = 0;
  bool first_publish_logged_ = false;
};

}  // namespace mcla::gpu
