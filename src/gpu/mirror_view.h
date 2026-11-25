#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared rear view (perf/MIRROR-VIEW.md, package MV1: the host side)
// ===========================================================================
// The game's rear view mirror is one more render phase of the frame (phase 0x1000000, hooks/rear_view.cpp): the city and the racers drawn from behind into a 229x44 rectangle of the MAIN target. With
// mcla_gpu_mirror_view on, the host sends the draws of that phase, and its begin clear, into a small target of its own, scoped by two markers the guest puts in the command stream around the phase
// (mirror_view_link.h kMarkerBegin / kMarkerEnd, the one header both sides use; the guest calls NoteMirrorPhase, gpu.h; the host tells the guest what it does through mirror_view_link.h's Request, published in BeginFrame), and then stamps the finished picture into the rectangle at the end-of-scene copy of the main colour. The main
// target is untouched by the phase: no clear of the rectangle, no mirror depth in the scene depth, no host effect seeing a second camera's draws.
//
// THE IMAGES (made by the phase's first draw or clear, from the formats and the host sample count its pipeline names, so the pipelines the draws already have fit the framebuffer; remade when the
// samples change with the MSAA level, at the next phase; their size is the class's, PlanForRect):
//   colour    the main colour's host format, the main target's host samples, a HostImage (slot read as a Texture2DMS above one sample)
//   depth     the main depth's host format at the same samples
//   resolved  one sample, the average of the colour's samples (hlsl/mirror_view_ps.hlsl): what the readers sample; the colour itself at one sample
// THE PHASE (mirror_view_rules.h PhaseTracker): BeginPhase from the begin marker, Use at the first draw or clear (barriers, the begin clear of the colour), the draws and the guest's clear land in the
// framebuffer through the Scene (scene.cpp RecordMirrorDraw, OnClear), EndPhase from the end marker: the colour to the readable layout, the resolve, the picture whole. A phase never outlives its frame.
// THE READER (this package): the HUD stamp, one quad into the rectangle of the main colour at the first copy of the main colour after the end marker (a held frame sends an empty phase: the stamp is
// drawn from the held picture). The glass overlay of the side mirrors is package MV4.
// LIFETIME: the images are made when a redirected phase needs them and given back after about 3 s without one; the sizes are the class's (Low 384 wide, Mid 512, High 768; Low is the game's own
// mirror by default: AutoWanted), 0.3 to 12 MiB at 1 to 8 samples (HeldBytes, counted in the log's held list).
//
// CHECK: nothing of this has run on a device (no build in the package): the first run says whether the pipelines of the phase's draws accept the framebuffer on D3D12 and on Vulkan, whether the
// game's begin clear is seen (the census line counts the phases that had none), and what the stamp looks like.
//
// Threading: render thread only, after Initialize; Initialize and Shutdown on the lifecycle thread. NoteMirrorPhase is the guest's (gpu.h).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <plume_render_interface.h>

#include "edram.h"
#include "host_passes.h"
#include "mirror_draws.h"
#include "mirror_view_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class PassTiming;

class MirrorView {
 public:
  MirrorView();
  ~MirrorView();
  MirrorView(const MirrorView&) = delete;
  MirrorView& operator=(const MirrorView&) = delete;

  // The shaders are made when first needed (the switch is debug only): nothing here costs while it is off.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // Scene::BeginFrame: reads mcla_gpu_mirror_view, publishes the request to the guest half (mirror_view_link.h: redirect, and the side mirrors' need), closes a phase the last frame left open, gives the
  // images back after the idle time, writes the census line.
  void BeginFrame();
  bool enabled() const { return mode_ != 0; }
  // The images or the shaders could not be made lately: no phase is redirected, and the request to the guest says so (mirror_view_rules.h RequestFor).
  bool Blocked() const { return frame_ < blocked_until_ || shaders_failed_; }

  // ---- the phase --------------------------------------------------------------------------------------------------------------------
  // The begin marker, with the rectangle the guest published (guest pixels of the main target). True when the phase's draws and clear are redirected.
  bool BeginPhase(const mirror_draws::Rect& rect);
  bool redirecting() const { return phase_.redirecting(); }
  const mirror_draws::Rect& rect() const { return rect_; }
  // Where a prepared draw goes, and the count that says why a drop happened. The first draw that is a kView names the formats of the phase (Use).
  mirror_view::Route RouteDraw(const mirror_view::DrawFacts& facts);
  void NoteDrop() { ++stats_.dropped; }
  // The framebuffer the phase's draws and clears go into, ready to draw (the first call of a phase makes the images, moves them to their write layouts and clears the colour). The formats and the samples
  // are those of the first call; a null framebuffer means the images could not be made (the rest of the phase is dropped, and no new phase is asked for a while). `first` is true when the call
  // recorded commands outside a render pass: the Scene's bound framebuffer is gone.
  struct Target {
    plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool first = false;
  };
  Target Use(plume::RenderCommandList* list, plume::RenderFormat color, plume::RenderFormat depth, uint32_t samples);
  // The guest's begin clear of the phase, in the view's pixels (MapScissor of the clear's rectangle): `colour` four floats as the main target would store them (EncodeColorClear) or null.
  // After Use.
  void Clear(plume::RenderCommandList* list, const float* colour, bool depth, bool stencil, float z, uint32_t stencil_value, const plume::RenderRect& rect);
  // A viewport and a scissor of a draw (host pixels of the main target at `twentieths`/20 host pixels per guest pixel) as the view's.
  plume::RenderViewport MapViewport(const plume::RenderViewport& viewport, uint32_t twentieths) const;
  plume::RenderRect MapScissor(const plume::RenderRect& scissor, uint32_t twentieths) const;
  void NoteDraw() { ++stats_.draws; }
  // The end marker. True when it recorded commands outside a render pass (the view's resolve): the Scene's list state is gone.
  bool EndPhase(plume::RenderCommandList* list);

  // ---- the reader --------------------------------------------------------------------------------------------------------------------
  // A whole picture and a rectangle are up and the end marker of this frame has come: the first copy of the main colour stamps it.
  bool StampArmed() const { return stamp_armed_; }
  // The stamp into `colour` (the main colour target, its host samples as the pipeline needs them) over `host` (the rectangle in the target's host pixels). True when drawn: the list state is gone.
  bool RecordStamp(plume::RenderCommandList* list, EdramTargets::Target& colour, const mirror_view::IRect& host);
  // Said by the Scene when the first copy came and a stamp was not possible: the frame's stamp is given up.
  void DisarmStamp() { stamp_armed_ = false; }

  // The video memory the images hold.
  uint64_t HeldBytes() const;

  struct Stats {
    uint64_t phases = 0;             // redirected phases begun
    uint64_t completed = 0;          // ... whose end came
    uint64_t draws = 0;              // draws recorded into the view
    uint64_t dropped = 0;            // draws of a redirected phase left out (they cannot go into the view)
    uint64_t clears = 0;             // the guest's clears redirected into the view
    uint64_t phases_without_clear = 0;
    uint64_t stamps = 0;
    uint64_t stamp_failed = 0;
    uint64_t images_made = 0;
    uint64_t image_failures = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  bool MakeShaders();
  bool EnsureImages(plume::RenderFormat color, plume::RenderFormat depth, uint32_t samples);
  void ReleaseImages();
  plume::RenderPipeline* ResolvePipeline(plume::RenderFormat format);
  plume::RenderPipeline* StampPipeline(plume::RenderFormat format, uint32_t host_samples);
  uint32_t PictureSlot() const;
  mirror_view::Class SizeClass() const;
  void Census(bool force);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  std::unique_ptr<plume::RenderShader> resolve_shader_;
  std::unique_ptr<plume::RenderShader> stamp_shader_;
  bool shaders_failed_ = false;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> resolve_pipelines_;  // by the colour format
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> stamp_pipelines_;    // by format and host samples
  std::unordered_map<uint64_t, bool> pipeline_failed_;

  // The setting as BeginFrame read it: 0 off, 1 the card's class, 2 to 4 the Low, Mid or High size.
  int32_t mode_ = 0;
  uint64_t frame_ = 0;
  uint64_t blocked_until_ = 0;  // the frame before which the images are not asked for again after a failure

  // The phase.
  mirror_view::PhaseTracker phase_;
  mirror_view::ViewFormats formats_;
  mirror_view::Plan plan_;
  mirror_draws::Rect rect_;
  bool used_ = false;            // Use has made the images ready in this phase
  bool failed_phase_ = false;    // the images could not be made: the rest of the phase is dropped
  uint32_t clears_in_phase_ = 0;
  bool no_clear_logged_ = false;

  // The images.
  HostImage colour_;
  HostImage resolved_;  // one sample, only above one sample of the colour
  std::unique_ptr<plume::RenderTexture> depth_;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer_;
  plume::RenderFormat depth_format_ = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureLayout depth_layout_ = plume::RenderTextureLayout::UNKNOWN;
  uint32_t samples_ = 1;
  bool colour_new_ = false;      // made since the last clear: the depth is cleared too (the first use of a new image)
  bool whole_ = false;           // a finished picture exists
  uint64_t picture_frame_ = 0;   // the frame it was finished in
  bool stamp_armed_ = false;
  mirror_view::Lifetime life_;

  Stats stats_;
  uint64_t census_frame_ = 0;
  uint64_t census_phases_ = 0;
  bool image_failure_logged_ = false;
  bool shaders_failure_logged_ = false;
};

}  // namespace mcla::gpu
