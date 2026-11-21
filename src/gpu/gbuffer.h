#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's core: the sidecar images, the frame window, the attach decisions' counts, the debug views (GB-A)
// ===========================================================================
// perf/GBUFFER-SURVEY.md, the hybrid path: the main view's lit draws also write a thin G-buffer into sidecar render targets, in the same draw as the
// game's colour. This class owns the sidecar IMAGES and decides nothing about a draw by itself: the Scene asks gbuffer_rules.h for each prepared draw
// (Scene::GBufferKey -> gbuffer::Decide), widens the pipeline's colour targets by the planes for the draws that carry them, and calls the hooks below.
//
// THE PLANES (gbuffer_types.h): RT1 the octahedral world normal (R16G16_UNORM), RT2 the albedo and the lit weight (R8G8B8A8_UNORM), RT3 the smoothness and the
// class (R8G8_UNORM). The pixel stage of the pack declares as many outputs as gbuffer::kStagePlanes says (all three: the families' transforms, GB-C and GB-D, write RT1,
// GB-W1's RT2 and GB-W2's RT3); the images, the framebuffers, the clears, the key's slots and the debug views handle all three the same way, so the day a family
// writes RT3 it is a change of that one constant and of the stage. A frame carries ALL the stage's planes on every draw that carries the sidecar, and publishes as
// valid the ones the setting and the consumers asked for.
//
// THE SIDECAR. Host images (HostPasses::EnsureImage) at the size and sample count of the main view's colour target (render scale and the host's MSAA level
// included), made on the first draw or clear that needs them and made again whenever the colour target changes (all planes together: a framebuffer cannot mix sizes
// or samples). They are not EDRAM identities: the tile map and the aliasing detector never see them. A draw that carries them binds the colour target, the planes
// after it and the depth (EdramTargets::Framebuffer with `extras`). Which draws carry them, and what each does to them, is gbuffer_rules.h Decide:
//   writer          the pixel shader is a G-buffer word: it writes the planes
//   clear           an opaque draw whose shader writes no planes (no variant yet, a mirror's): blend ZERO / ZERO, so the pixels it draws become "no data"
//   mask            a blended draw or one with no colour: write mask 0, the planes stay
//   not carried     the guest's own framebuffer: the pause world, a cutscene, every other target, the draws after the trigger
// The clear and the mask need a blend state and write mask per colour target: Vulkan has it, Plume's D3D12 back end not before its fix 5d60b70 (rules: kD3d12PerTargetBlend,
// the debug setting mcla_gpu_gbuffer_attach). Without it only the writers carry the sidecar and the pixels of the others keep what an earlier draw of the window
// wrote there: the one inexactness, which the "missing pixels" view (mcla_gpu_gbuffer_view 7) shows for the shaders that have no variant (their draws are counted by
// identity in the log either way).
//
// CHECK (not run): the clear and mask blend states on Vulkan (auto = all there) and on D3D12 with Plume's fix; the cutscene gate (no hook calls NoteCutsceneCamera yet); the rear view mirror's window; the memory cap.
//
// THE FRAME (gbuffer_rules.h Window):
//   - the guest's clear of the main colour is mirrored: the planes are cleared to 0 ("no data") with the same rectangle, at the target's scale. A day frame has two (the
//     guest clears again after the sun shadow collector, which draws into the same target), both mirrored; the collector's shaders have no variant, so with the
//     non writers carried they clear and then the guest's clear clears again. A main view colour draw that comes before any clear (CHECK: none known) clears the whole
//     sidecar first.
//   - the mid-scene depth copy of the main depth (the AO's trigger) closes the window: later draws of the main view carry no sidecar, the images go to SHADER_READ and the
//     frame is published (frame()), where a consumer reads it. The debug view is drawn there.
//   - the 4x views behind the pause menu and the map, a cutscene's frames, and every other target (the cascades, the reflection views, the UI) never carry it.
//
// WHO ASKS. The setting mcla_gpu_gbuffer is the debug switch (off | normals | full); `auto`, the default, is what the effects that are on ask for through Want(): the
// sidecar is made only while a consumer wants a plane (the ambient occlusion, GB-E, will), so with none no image exists and no draw changes. Wants are read at each frame
// boundary (live; one frame late the first time).
//
// The debug views (mcla_gpu_gbuffer_view 0..7, hlsl/gbuffer_view_ps.hlsl): the normal, the albedo, the smoothness, the class (a colour per family once RT3 is written),
// the coverage, the lit weight, and "missing": the pixels where the depth holds geometry and the planes hold no data.
//
// Logs (info, "[gbuffer]"): the first frame with the sidecar and every time it is made again (size, samples, planes, bytes), the policy (what the non writers do), the window's
// clears (the first few frames: how many, how many partial: survey C6), and a line every 600 frames with the draws by outcome and the pixel shaders that wrote it or that
// have no variant yet ("masked" in the counts: the draws that could have carried it and did not write it; their identities are the family picture, survey R2).
//
// Threading: render thread only, except NoteCutsceneCamera (the guest's thread: one atomic).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "edram.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "host_passes.h"
#include "resolve.h"

namespace mcla::gpu {

class PassTiming;

class GBuffer {
 public:
  // What a draw that carries the sidecar binds after its colour target: the planes' textures, in plane order from RT1.
  struct Sidecar {
    const plume::RenderTexture* textures[gbuffer::kPlaneCount] = {};
    uint32_t count = 0;
    uint32_t generation = 0;  // changes whenever the images are replaced (the framebuffer memo's key)
  };

  GBuffer() = default;
  ~GBuffer();
  GBuffer(const GBuffer&) = delete;
  GBuffer& operator=(const GBuffer&) = delete;

  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The effects that read the planes say which they want while they are on (gbuffer_rules.h Consumer; 0 withdraws). Sticky: set it when the effect's setting changes. Read at the
  // next frame boundary.
  void Want(gbuffer::Consumer consumer, uint32_t planes) { wants_.Set(consumer, planes); }
  // The cutscene director's camera ran (the hook of the guest's cutscene camera calls this every frame a cutscene is on): the frames that follow carry no sidecar. Any thread.
  static void NoteCutsceneCamera();

  // The Scene opened a frame's first list: reads the settings and the consumers' wants, makes the frame's plan and starts the frame's window.
  void BeginFrame(uint64_t frame_serial);
  // True while the main view's draws are asked about the sidecar this frame: a plane is wanted, the frame is no cutscene's and the sidecar fits the memory cap.
  bool active() const { return plan_.attached != 0 && !suppressed_ && !blocked_; }
  // The same without the memory cap's verdict, which the guest's clear of the main colour renews at each frame (a change of the render scale or the MSAA level can lift it).
  bool tracking() const { return plan_.attached != 0 && !suppressed_; }
  // The key's colour format of a plane's slot.
  static plume::RenderFormat FormatOf(gbuffer::Plane plane);
  // The frame's plan: the planes asked for, the ones the stage writes (published as valid), the ones every carrying draw has.
  const gbuffer::FramePlan& plan() const { return plan_; }
  // What the non writers do (gbuffer_rules.h MaskedDrawsAttach) and what the backend can do.
  bool masked_attach() const { return masked_attach_; }
  const gbuffer::Caps& caps() const { return caps_; }
  // What the window says for the draw being prepared.
  gbuffer::Window::Gate Gate() const { return window_.GateForDraw(); }
  // One prepared main view draw's outcome, for the counts and the family picture.
  void NoteDraw(gbuffer::Why why, uint64_t ps_identity);
  // A draw whose sidecar pipeline was still being made went without the sidecar (Scene: the key falls back to the guest's).
  void NoteFallbackDraw() { ++fallback_draws_; }
  // The game's motion blur draw (the anchor) was met in a frame this class tracks: reads the frame's camera from its pixel constants (`ps_bank`: 256 registers of raw big endian
  // bytes, as temporal::ExtractCamera takes them) and keeps it for the next frame when this one is the play view's (gbuffer_rules.h CameraKeeper). The G-buffer reads it itself, so the
  // derivative normal's camera exists with or without a temporal upscaler.
  void OnAnchor(const uint8_t* ps_bank);
  // The camera the frame's draws hand their shaders (the derivative normal, a fragment's world point): the last frame's; null when there is none this frame (the first frame, a
  // cutscene's or the pause world's before it, an anchor whose constants gave none).
  const gbuffer::CameraAxes* camera() const { return camera_.Previous(); }
  // A writer's constants were filled, with the camera rows (true) or without (false: the shaders that need them write "no data"); for the log.
  void NoteCameraRows(bool had) { ++(had ? camera_rows_ : camera_missing_); }
  // Changes whenever the sidecar's textures are replaced; part of the Scene's framebuffer memo. 0 while there is no sidecar.
  uint32_t generation() const { return image_planes_ != 0 ? generation_ : 0; }

  // A draw that carries the sidecar is about to be recorded (outside the render pass or inside it: only barriers and clears are recorded, which the caller has ended the
  // pass for): makes the images for `colour0`'s size and samples when they are not that, clears them whole when the frame has had no clear yet, moves them to the colour
  // attachment layout. False (logged) when they cannot be made: the draw is then not recorded.
  bool Attach(plume::RenderCommandList* list, EdramTargets::Target& colour0, Sidecar* out);
  // The guest cleared the main view's colour in the rectangle `guest_rect` (guest pixels) of `colour0`: the same rectangle of every plane is cleared. The
  // list has no framebuffer bound that it needs afterwards (the planes' own is left bound: the caller forgets its binding).
  void OnColorClear(plume::RenderCommandList* list, EdramTargets::Target& colour0, const plume::RenderRect& guest_rect);
  // A resolve, after ResolveCache::Execute made its copy, `list` outside any render pass: the first depth copy of the main view's play frame at the AO's
  // trigger closes the window and publishes the frame. True when the frame was published (a barrier was recorded).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // Right after the other effects of the same trigger (the AO's apply, the sun shafts, the car passes) so that the picture replaces what they drew: the debug view,
  // when mcla_gpu_gbuffer_view asks for one and the frame was published. True when passes were recorded (the caller's list state is gone).
  bool DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // True once after a call that left the planes' own framebuffer bound on the list (a clear): the Scene then forgets which framebuffer it had bound.
  bool TakeFramebufferDirty() {
    const bool dirty = framebuffer_dirty_;
    framebuffer_dirty_ = false;
    return dirty;
  }

  // What a consumer reads: valid from the trigger to the frame's end (BeginFrame empties it).
  const gbuffer::GBufferFrame& frame() const { return frame_; }
  // The video memory the sidecar holds now.
  uint64_t HeldBytes() const;

 private:
  struct ViewPipeline {
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };
  struct ViewPush {
    uint32_t slots[4] = {};
    uint32_t info[4] = {};
  };
  static_assert(sizeof(ViewPush) == 32);

  bool EnsureImages(uint32_t width, uint32_t height, uint32_t samples);
  void ReleaseImages();
  void Transition(plume::RenderCommandList* list, plume::RenderTextureLayout layout);
  void ClearRect(plume::RenderCommandList* list, const plume::RenderRect& host);
  void FillSidecar(Sidecar* out) const;
  bool CheckAffordable(const EdramTargets::Target& colour0);
  plume::RenderPipeline* ViewPipelineFor(plume::RenderFormat format, uint32_t samples);
  void LogInterval();

  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  // The images, one per plane of the stage's set (a plane outside it has none).
  std::array<HostImage, gbuffer::kPlaneCount> images_;
  uint32_t image_planes_ = 0;  // PlaneBit of the planes that have an image now
  uint32_t generation_ = 0;
  bool needs_clear_ = true;  // the images are new: their contents are not 0 until a clear
  bool framebuffer_dirty_ = false;

  gbuffer::Wants wants_;
  gbuffer::FramePlan plan_;        // this frame's
  gbuffer::FramePlan last_plan_;   // the one before it (the log says when it changes)
  gbuffer::View view_ = gbuffer::View::kOff;
  bool masked_attach_ = false;
  bool masked_attach_logged_ = false;
  gbuffer::Caps caps_;
  gbuffer::CutsceneGate cutscene_gate_;
  bool suppressed_ = false;          // this frame is a cutscene's
  bool blocked_ = false;             // the sidecar would be past the memory cap at the main target's size and samples
  bool blocked_logged_ = false;
  gbuffer::Window window_;
  gbuffer::GBufferFrame frame_;
  uint64_t frame_serial_ = 0;

  std::array<std::unique_ptr<plume::RenderShader>, 2> view_shaders_;  // one sample, multisampled
  bool view_shaders_failed_ = false;
  std::vector<ViewPipeline> view_pipelines_;

  // The counts of an interval of frames (LogInterval), and of the first frames' windows.
  uint64_t interval_frames_ = 0;
  uint64_t active_frames_ = 0;
  uint64_t published_frames_ = 0;
  uint64_t suppressed_frames_ = 0;
  uint64_t fallback_draws_ = 0;
  std::array<uint64_t, size_t(gbuffer::Why::kCount)> counts_{};
  uint64_t clears_ = 0;
  uint64_t partial_clears_ = 0;
  uint64_t initial_clears_ = 0;
  uint64_t late_clears_ = 0;  // a mirrored clear that found the window closed
  std::unordered_map<uint64_t, uint32_t> written_by_;  // pixel shader identity -> draws that wrote the sidecar (this interval)
  std::unordered_map<uint64_t, uint32_t> masked_by_;   // ... draws of the main view's window whose shader has no variant (yet): the next families
  uint32_t window_logs_ = 0;                           // the first frames' window lines logged
  uint64_t held_bytes_ = 0;

  // The view the frame's draws were of (gbuffer_rules.h ViewTally), the camera kept from the last frame's anchor, and what the log says of both.
  void EndOfFrameView();  // folds the finished frame's tally into the pause spell's counts (BeginFrame, before the next frame's tally starts)
  gbuffer::ViewTally tally_;
  gbuffer::CameraKeeper camera_;
  uint32_t pause_spell_frames_ = 0;    // frames of the pause world's spell running now (0: none)
  uint64_t pause_spell_draws_ = 0;
  uint64_t pause_frames_ = 0;          // this interval's
  uint64_t pause_draws_ = 0;
  uint64_t mixed_frames_ = 0;          // frames with draws of both kinds: the pause world and the play view in one frame (never expected)
  bool mixed_logged_ = false;
  uint64_t anchors_ = 0;               // this interval's anchors met, and how many gave a camera that was kept
  uint64_t anchors_kept_ = 0;
  uint64_t camera_rows_ = 0;           // writers' constants filled with / without the camera rows
  uint64_t camera_missing_ = 0;
  bool camera_logged_ = false;
};

}  // namespace mcla::gpu
