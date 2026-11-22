#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: the owner (SS-0: adopt what the AO makes, add the Hi-Z; SS-1: the colour captures, the history)
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
//     the AO's frame constants) stay in ao.cpp, which records it inside CapturePre: the same commands in the same place, so the picture and the pass timings are the ones of SS-0. SS-2 moves the
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
// apply), CapturePost after the AO, the light pass, the sun shafts and the volumetric light and before the reflections and the car history, DrawView after the effects of the trigger, with the G-buffer's view. SS-2 moves the AO's depth, normal and colour passes here: the frame then has one more caller and ao.cpp one less.
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

#include "constants.h"
#include "edram.h"
#include "host_passes.h"
#include "resolve.h"
#include "screen_frame.h"
#include "screen_history.h"
#include "screen_rules.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class PassTiming;

class ScreenShared {
 public:
  // What the owner of the products that exist today (the AO) hands over at the end of its depth chain: the images it made THIS frame (not owned; null: not made). `depth[0]` is the
  // frame's level 0 linear depth (the AO's ping-pong swap already done) and is required: without it nothing is published. (colour_pre is not handed over any more: its images are the
  // owner's, CapturePre says whether the chain was recorded this frame.)
  struct Adopted {
    const HostImage* depth[screen::kDepthLevels] = {};
    const HostImage* depth_full = nullptr;
    const HostImage* normal = nullptr;
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

  // Makes the shaders (the pipelines are made in the background when first wanted). Never fails the renderer: without them nothing is made and nothing is drawn (logged).
  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
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
  // The AO's depth chain of this frame is done: publishes the frame and, when hiz_min is wanted, records its passes on `list` (outside any render pass; the caller's list state is
  // unchanged: DrawInto leaves no framebuffer bound). Called once a frame at most, from AmbientOcclusion::Run.
  void Publish(plume::RenderCommandList* list, const Adopted& adopted);

  // colour_pre (the bounce light's colour chain): the owner's images, three levels (the traced size, 1/2 and 1/4 of it); `format` is the AO's. False when one could not be made (nothing is
  // changed then: the AO's bounce light is off for the frame). Kept while the sizes are; ReleaseColourPre gives them back (the AO does when it gives its own back).
  bool EnsureColourPre(const uint32_t sizes[screen::kColourPreLevels][2], plume::RenderFormat format);
  HostImage& ColourPre(uint32_t level) { return colour_pre_[level < screen::kColourPreLevels ? level : 0]; }
  void ReleaseColourPre();
  // Step 5 of the order: the chain is recorded now, by `record` (the AO's pass body, which writes the images ColourPre hands out); the owner counts it made this frame, so that Publish lists
  // colour_pre. Called only while the bounce light runs (its want of colour_pre).
  void CapturePre(const std::function<void()>& record);

  // colour_post (and the history): the colour_post step of frame_order.h's table. At the frame's trigger, after the AO's apply, records the capture when the plan makes it (a consumer wants it, or the history, or the view
  // shows it). True when passes were recorded (the caller's list state is gone). Once a frame.
  bool CapturePost(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // True while a capture is planned and not made yet this frame (the Scene calls CapturePost only then).
  bool capture_wanted() const { return !post_done_ && (plan_.make & screen::ProductBit(screen::kColourPost)) != 0; }

  // What a consumer reads: valid from the trigger to the frame's end (BeginFrame empties it).
  const screen::ScreenFrame& Frame() const { return frame_; }
  // The image behind a product of the published frame (a compute consumer needs the texture for its barrier; a pixel consumer reads the slot). Null where the frame has none. The history's
  // image is `Image(kHistory)`.
  const HostImage* Image(screen::Product product, uint32_t level = 0) const;

  // The debug view, right after the other effects of the trigger so that the picture replaces what they drew. True when passes were recorded (the caller's list state is gone).
  bool DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // The video memory the images this class holds now (the Hi-Z's, the colour captures', the history's; the AO's own are the AO's until SS-2).
  uint64_t HeldBytes() const;

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
  // The colour capture's three compilations (host_shaders.h ScreenColourPixelShader).
  enum ColourPass : uint32_t { kColourPlain, kColourMs, kColourMip, kColourPassCount };

  // The Hi-Z's pipeline: listed with the host precache the first frame it is wanted and drawn from the frame it is made (a frame never waits for a compile); null until then and
  // when it could not be made.
  plume::RenderPipeline* HizPipeline();
  std::unique_ptr<plume::RenderPipeline> MakeHizPipeline();
  // Records the chain for `depth0` (the frame's linear depth, level 0); returns the number of levels and their slots in `slots`, 0 when there is none this frame.
  uint32_t RecordHiz(plume::RenderCommandList* list, const HostImage& depth0, uint32_t slots[screen::kHizMaxLevels]);
  void ReleaseHiz();
  // The colour capture's pipelines, the same way: null while one is being made or when it could not be made (no capture then).
  plume::RenderPipeline* ColourPipeline(uint32_t pass);
  std::unique_ptr<plume::RenderPipeline> MakeColourPipeline(uint32_t pass);
  bool ColourPipelinesReady();
  void ReleaseColourPost();
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

  std::array<HostImage, screen::kHizMaxLevels> hiz_;
  uint32_t hiz_images_ = 0;          // the levels that have an image now
  uint32_t hiz_idle_frames_ = 0;     // frames since the Hi-Z was last wanted (the images go back after kHizReleaseAfterFrames)
  uint64_t hiz_generation_ = 0;      // bumped when the Hi-Z's images are made again
  uint32_t hiz_logged_size_[2] = {};
  bool hiz_images_failed_logged_ = false;

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
  uint64_t frame_serial_ = 0;
  uint64_t published_frames_ = 0;
  bool first_publish_logged_ = false;
};

}  // namespace mcla::gpu
