#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: the owner (SS-0: adopt what the AO makes, add the Hi-Z)
// ===========================================================================
// perf/LIGHT-UNIFY.md section 5. The frozen interface is screen_frame.h (ScreenFrame, Product, Consumer, Wants); the pure rules are screen_rules.h; this class is the owner.
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
// WHO ASKS. A consumer calls Want(consumer, products) when its setting changes (sticky; 0 withdraws); the plan is made at BeginFrame, so a want is read at the next frame
// boundary, as the G-buffer's. The AO declares its own (lin_depth, plus normal and colour_pre where it makes them) for the census line; they never start a frame of their own.
//
// THE ORDER (scene.cpp OnResolve): BeginFrame at the frame's start, before the AO's; Publish inside the AO's frame (step 3 of the order comment, after its depth chain, normal
// pass and colour chain, before the ray traced pass); DrawView after the effects of the trigger, with the G-buffer's view. SS-1 moves the colour captures and the history here,
// SS-2 the AO's depth, normal and colour passes: the frame then has one more caller and ao.cpp one less.
//
// RUN PLAN (the main session; this agent ran nothing): (1) build main and this branch; frozen views (the trailer toolkit's fixed scenes, both APIs, MSAA 1 and 4, 1080p and
// 4K, AO on/off, contact on/off, the bounce light on): the pictures with the defaults must be IDENTICAL (image diff 0) and the pass timings equal within the noise floor, because
// nothing is recorded without a want. (2) mcla_gpu_screen_view 3 with mcla_gpu_screen_view_level 0..6 on the same scenes, both APIs, AO on and then AO and contact off: no red
// anywhere (the chain is a min) and the picture changes only by the view; view 2 shows the blocks of the level; view 1 shows the depth. The log says "[screen] ..." lines once.
// (3) Cost: the pass timings "screen hiz" (7 passes, expected below 0.05 ms at 1080p traced, CHECK) with view 2 on against off; memory +11 MiB at 1080p traced (HeldBytes in the
// frame stats line, "screen products"). (4) A live switch of the view on and off, a resolution change and the pause menu (4x view): no validation-layer message, no leak at quit.
// CHECK items are marked in screen_shared.cpp.
//
// Threading: render thread only, except Initialize / Shutdown (lifecycle thread, GPU idle).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "edram.h"
#include "host_passes.h"
#include "resolve.h"
#include "screen_frame.h"
#include "screen_rules.h"

namespace mcla::gpu {

class PassTiming;

class ScreenShared {
 public:
  // What the owner of the products that exist today (the AO) hands over at the end of its depth chain: the images it made THIS frame (not owned; null: not made). `depth[0]` is the
  // frame's level 0 linear depth (the AO's ping-pong swap already done) and is required: without it nothing is published.
  struct Adopted {
    const HostImage* depth[screen::kDepthLevels] = {};
    const HostImage* depth_full = nullptr;
    const HostImage* normal = nullptr;
    const HostImage* colour_pre[screen::kColourPreLevels] = {};
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

  // The Scene opened a frame's first list: reads the settings and the wants, makes the frame's plan and empties the frame.
  void BeginFrame(uint64_t frame_serial);
  const screen::Plan& plan() const { return plan_; }
  // True when something outside the AO's family wants the depth chain (or the Hi-Z, or the debug view shows it): the AO then runs a frame for it although its own effects are off. The
  // plan's of this frame.
  bool NeedsDepthChain() const { return plan_.NeedsDepthChain(); }
  // True while a debug view is asked for (mcla_gpu_screen_view).
  bool view_active() const { return plan_.view != screen::View::kOff; }

  // The AO's depth chain of this frame is done: publishes the frame and, when hiz_min is wanted, records its passes on `list` (outside any render pass; the caller's list state is
  // unchanged: DrawInto leaves no framebuffer bound). Called once a frame at most, from AmbientOcclusion::Run.
  void Publish(plume::RenderCommandList* list, const Adopted& adopted);

  // What a consumer reads: valid from the trigger to the frame's end (BeginFrame empties it).
  const screen::ScreenFrame& Frame() const { return frame_; }
  // The image behind a product of the published frame (a compute consumer needs the texture for its barrier; a pixel consumer reads the slot). Null where the frame has none.
  const HostImage* Image(screen::Product product, uint32_t level = 0) const;

  // The debug view, right after the other effects of the trigger so that the picture replaces what they drew. True when passes were recorded (the caller's list state is gone).
  bool DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // The video memory the images this class made hold now (the Hi-Z's; the AO's are the AO's until SS-2).
  uint64_t HeldBytes() const;

 private:
  struct HizPush {
    uint32_t slots[4] = {};
    uint32_t sizes[4] = {};
  };
  static_assert(sizeof(HizPush) == 32);
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

  // The Hi-Z's pipeline: listed with the host precache the first frame it is wanted and drawn from the frame it is made (a frame never waits for a compile); null until then and
  // when it could not be made.
  plume::RenderPipeline* HizPipeline();
  std::unique_ptr<plume::RenderPipeline> MakeHizPipeline();
  // Records the chain for `depth0` (the frame's linear depth, level 0); returns the number of levels and their slots in `slots`, 0 when there is none this frame.
  uint32_t RecordHiz(plume::RenderCommandList* list, const HostImage& depth0, uint32_t slots[screen::kHizMaxLevels]);
  void ReleaseHiz();
  plume::RenderPipeline* ViewPipelineFor(plume::RenderFormat format, uint32_t samples);
  void LogWants();

  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  std::unique_ptr<plume::RenderShader> hiz_shader_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  std::unique_ptr<plume::RenderPipeline> hiz_pipeline_;
  bool hiz_listed_ = false;
  bool hiz_failed_ = false;
  bool view_failed_ = false;
  std::vector<ViewPipeline> view_pipelines_;

  std::array<HostImage, screen::kHizMaxLevels> hiz_;
  uint32_t hiz_images_ = 0;          // the levels that have an image now
  uint32_t hiz_idle_frames_ = 0;     // frames since the Hi-Z was last wanted (the images go back after kHizReleaseAfterFrames)
  uint64_t hiz_generation_ = 0;      // bumped when the Hi-Z's images are made again
  uint32_t hiz_logged_size_[2] = {};
  bool hiz_images_failed_logged_ = false;

  screen::Wants wants_;
  screen::Plan plan_;
  uint32_t logged_wanted_ = ~0u;
  screen::Generation generation_;
  screen::ScreenFrame frame_;
  Adopted adopted_;
  bool published_ = false;
  uint64_t frame_serial_ = 0;
  uint64_t published_frames_ = 0;
  bool first_publish_logged_ = false;
};

}  // namespace mcla::gpu
