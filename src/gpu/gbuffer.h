#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's sidecar: the images, the frame window, the debug view (the spike, phase 0)
// ===========================================================================
// perf/GBUFFER-SURVEY.md, the hybrid path: the main view's lit draws also write a thin G-buffer into sidecar render targets, in the same draw as the
// game's colour. This class owns the sidecar IMAGES and decides nothing about a draw by itself: the Scene asks gbuffer_rules.h for each prepared draw
// (Scene::GBufferDraw), widens the pipeline's colour targets by the sidecar for the draws that carry it, and calls the hooks below. Phase 0 has ONE plane,
// RT1 (the octahedral world normal, R16G16_UNORM, gbuffer_types.h), and one family that writes it (xCityDefault, shaderpack/variants/gbuffer.h).
//
// The sidecar. A host image (HostPasses::EnsureImage) at the size and sample count of the main view's colour target (render scale and the host's MSAA level
// included), made on the first draw that carries it and made again whenever the colour target changes. It is not an EDRAM identity: the tile map and the
// aliasing detector never see it. Its framebuffer is the colour target's, the sidecar after it, and the depth (EdramTargets::Framebuffer with `extra`).
//
// The frame (gbuffer_rules.h Window):
//   - the guest's clear of the main colour is mirrored: the sidecar is cleared to 0 ("no data") with the same rectangle, at the target's scale. A day frame
//     has two (the guest clears again after the sun shadow collector, which draws into the same target), both mirrored; the collector's shaders have no
//     variant, so they write nothing. A main view colour draw that comes before any clear (CHECK: none known) clears the whole sidecar first.
//   - the mid-scene depth copy of the main depth (the AO's trigger) closes the window: later draws of the main view carry no sidecar, the image goes to
//     SHADER_READ and the frame is published (frame()), where a consumer reads it. The debug view is drawn there.
//   - the 4x views behind the pause menu and the map, and every other target (the cascades, the reflection views, the UI), never carry it.
//
// The settings (debug-only): mcla_gpu_gbuffer off | normals | full | auto (auto: what the effects that are on ask for: nothing yet, so off), and
// mcla_gpu_gbuffer_view 0..6 (hlsl/gbuffer_view_ps.hlsl). Both are read at each frame boundary (live); turning it off gives the images back.
//
// Logs (info, "[gbuffer]"): the first frame with the sidecar and every time it is made again (size, samples, bytes), the window's clears (the first
// few frames: how many, how many partial: survey C6), and a line every 600 frames with the draws by outcome and the pixel shaders that wrote it or were
// left masked (their identities: the family picture, survey R2).
//
// Threading: render thread only.
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
  GBuffer() = default;
  ~GBuffer();
  GBuffer(const GBuffer&) = delete;
  GBuffer& operator=(const GBuffer&) = delete;

  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the settings, and starts the frame's window.
  void BeginFrame(uint64_t frame_serial);
  // True while the main view's draws are asked about the sidecar.
  bool active() const { return planes_ != 0; }
  // What the window says for the draw being prepared.
  gbuffer::Window::Gate Gate() const { return window_.GateForDraw(); }
  // One prepared main view draw's outcome, for the counts and the family picture.
  void NoteDraw(gbuffer::Why why, uint64_t ps_identity);
  // Changes whenever the sidecar's texture is replaced; part of the Scene's framebuffer memo. 0 while there is no sidecar.
  uint32_t generation() const { return image_.texture ? generation_ : 0; }

  // A draw that carries the sidecar is about to be recorded (outside the render pass or inside it: only barriers and clears are recorded, which the
  // caller has ended the pass for): makes the image for `colour0`'s size and samples when it is not that, clears it whole when the frame has had no clear
  // yet, moves it to the colour attachment layout. The texture, or null (logged) when it cannot be made: the draw is then not recorded.
  plume::RenderTexture* Attach(plume::RenderCommandList* list, EdramTargets::Target& colour0);
  // The guest cleared the main view's colour in the rectangle `guest_rect` (guest pixels) of `colour0`: the same rectangle of the sidecar is cleared. The
  // list has no framebuffer bound that it needs afterwards (the sidecar's own is left bound: the caller forgets its binding).
  void OnColorClear(plume::RenderCommandList* list, EdramTargets::Target& colour0, const plume::RenderRect& guest_rect);
  // A resolve, after ResolveCache::Execute made its copy, `list` outside any render pass: the first depth copy of the main view's play frame at the AO's
  // trigger closes the window and publishes the frame. True when the frame was published (a barrier was recorded).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // Right after the other effects of the same trigger (the AO's apply, the sun shafts, the car passes) so that the picture replaces what they drew: the debug view,
  // when mcla_gpu_gbuffer_view asks for one and the frame was published. True when passes were recorded (the caller's list state is gone).
  bool DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // True once after a call that left the sidecar's own framebuffer bound on the list (a clear): the Scene then forgets which framebuffer it had bound.
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

  bool EnsureImage(uint32_t width, uint32_t height, uint32_t samples);
  void ReleaseImage();
  void Transition(plume::RenderCommandList* list, plume::RenderTextureLayout layout);
  void ClearRect(plume::RenderCommandList* list, const plume::RenderRect& host);
  plume::RenderPipeline* ViewPipelineFor(plume::RenderFormat format, uint32_t samples);
  void LogInterval();

  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  HostImage image_;
  uint32_t generation_ = 0;
  bool needs_clear_ = true;  // the image is new: its contents are not 0 until a clear
  bool framebuffer_dirty_ = false;

  uint32_t planes_ = 0;  // gbuffer::PlaneBit() of the planes the setting asks for
  gbuffer::View view_ = gbuffer::View::kOff;
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
  std::array<uint64_t, size_t(gbuffer::Why::kCount)> counts_{};
  uint64_t clears_ = 0;
  uint64_t partial_clears_ = 0;
  uint64_t initial_clears_ = 0;
  uint64_t late_clears_ = 0;  // a mirrored clear that found the window closed
  std::unordered_map<uint64_t, uint32_t> written_by_;  // pixel shader identity -> draws that wrote the sidecar (this interval)
  std::unordered_map<uint64_t, uint32_t> masked_by_;   // ... draws that carried it and left it masked for want of a variant
  uint32_t window_logs_ = 0;                           // the first frames' window lines logged
  uint64_t held_bytes_ = 0;
};

}  // namespace mcla::gpu
