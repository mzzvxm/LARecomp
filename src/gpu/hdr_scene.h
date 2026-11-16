#pragma once
// ===========================================================================
// MCLA GPU renderer -- the HDR frame at the present: the scene and the UI composed
// ===========================================================================
// HDR stage 2 (feature-parity/HDR-PLAN.md). With an HDR10 back buffer the present normally shows
// the guest's front buffer, the scene and the UI already blended in 8 bits. When FrameLayers
// (frame_layers.h) kept the frame's two layers, Compose draws them into one float image in the
// game's code values -- the UI's premultiplied colour plus the scene times (1 - the UI's
// coverage), which is how the game blends the UI -- and the present reads that image where it
// would read the front buffer, with the display's gamma table extended above 1 (present_ps.hlsl,
// flag kHdrFlagExtendedCodes). FSR1's passes read it too, in float16.
//
// Compose in the present (mcla_gpu_hdr_compose_in_present, on by default): the present's passes that
// read the frame 1:1 take that sum themselves from the two layers (hlsl/hdr_compose.hlsli, flag
// kHdrFlagComposeLayers), so the float image, its pass and its memory (63 MiB at 4K) are not
// needed; Select chooses the layers, the present asks for the image (Compose) only when it must:
// a scaled bilinear present reads four texels a pixel, and a debug view or the cvar off wants the
// image as before. An idle present reads the layers again: no guest frame is open then, so the
// layers are what the last present showed (Renderer::OnIdle).
//
// A frame whose scene is the 8-bit one (stage 2: stage 3 not available, or it could not make the
// frame's float scene, hdr_tonemap.h) gives the front buffer's picture (to the rounding of the 8-bit
// blends, see frame_layers.h). A frame that has a float scene (Frame::scene: the tone map's light
// above the game's white and the glows drawn after it) composes that under the UI instead, above
// paper white, and the UI stays at or below it: its own part of the sum never exceeds its
// coverage, so where it covers the scene it is paper white at most, and the scene's excess shows
// only where the UI lets the scene through. Debug views (mcla_gpu_hdr_layers_view): 1 the UI
// layer, 2 the scene (the float one when the frame has it), 3 the UI's coverage, 4 where the float
// scene is above the game's white (magenta), 5 its difference from the 8-bit scene times 16.
//
// A frame without layers (the front buffer is not their picture, reaches past them, none made: the
// pause menus, movies, loading) is shown from the front buffer as stage 1 does.
//
// The HUD fade (ui_fade.h) scales the UI's colour and coverage by FrameLayers::UiAlphaFor(frame) in
// the compose, so an SDR present composes the layers too while the alpha is below 1 (Renderer::
// ApplyUiFade makes it so). The composed image is made again when that alpha changes, also for an
// idle present. The debug views of mcla_gpu_hdr_layers_view do not apply it.
//
// The first host column and row at a render scale (FrameLayers::Frame::gap) are not in the picture
// the layers keep: the compose reads them from the next ones, as the guest's resolve does for the
// front buffer, so the two ways of showing a frame agree along the edges of the screen.
//
// Memory: the float image (R16G16B16A16_FLOAT) at the front buffer's size: 7.4 MB at 1x and
// 66 MB at 3x, made on the first composed frame and released after as many presents without
// layers (or presents that took the sum themselves) as FrameLayers waits. With the compose in the
// present it is made only by a scaled bilinear present or a debug view.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "frame_layers.h"
#include "host_passes.h"
#include "textures.h"

namespace mcla::gpu {

class PassTiming;

class HdrScene {
 public:
  HdrScene() = default;
  ~HdrScene();
  HdrScene(const HdrScene&) = delete;
  HdrScene& operator=(const HdrScene&) = delete;

  // After the FrameLayers. False (logged) when the shader or the pipeline could not be made.
  bool Initialize(HostPasses& passes, FrameLayers& layers, PassTiming& pass_timing);
  // GPU idle, before HostPasses shuts down.
  void Shutdown();
  // Initialize made the shader and the pipeline.
  bool ready() const { return pipeline_ != nullptr; }
  // HDR output went off (render thread, at a frame boundary): the composed image is released and
  // the next composition starts afresh. The pipeline stays for the next time.
  void Release();

  // The layers of the frame to show and the scene under the UI, as Select chose them.
  struct Layers {
    FrameLayers::Frame frame;
    const HostImage* scene = nullptr;  // the float scene while the source has it, else the underlay
    bool float_scene = false;          // `scene` is the float one
    bool stale = false;                // the frame had a float scene that was gone by the present
    uint32_t scene_slot() const { return scene->slot; }
    uint32_t ui_slot() const { return frame.ui->slot; }
    uint32_t gap() const { return frame.gap; }
  };
  // Once per present: when the guest frame last presented has layers and `front` (the front
  // buffer's texture key) is their picture at `width` x `height` host pixels, says which layers
  // to show and returns true. False: show the front buffer.
  bool Select(const TextureKey& front, uint32_t width, uint32_t height, Layers* out);
  // The present may take the sum of the layers itself: mcla_gpu_hdr_compose_in_present is on and no
  // debug view (mcla_gpu_hdr_layers_view) is.
  bool ComposeInPresent() const;
  // The present took the sum itself (counted; a float image that stayed unused for long is released).
  void NoteInline(const Layers& layers);
  // Outside a render pass: composes the selected layers' top left corner of `width` x `height`
  // into the float image (once per frame: an idle present reuses it) and returns true; slot()
  // names the image. False: the image could not be made, show the front buffer.
  bool Compose(plume::RenderCommandList* list, const Layers& layers, uint32_t width,
               uint32_t height);
  // The composed image's bindless slot.
  uint32_t slot() const { return image_.slot; }
  // The float scene's source (not owned): a frame's scene is used while the source still has it.
  void SetSceneSource(SceneImageSource* source) { scene_source_ = source; }
  // The composed image holds the float scene (not the 8-bit underlay): the present then reads the
  // display's gamma table between its entries (kHdrFlagInterpolatedCodes).
  bool scene_is_float() const { return float_scene_; }

  // Memory held by the float image.
  uint64_t HeldBytes() const;

  std::string TakeSummary();

 private:
  struct Push {  // hlsl/frame_layers_ps.hlsl, LAYERS_COMPOSE
    uint32_t underlay;   // the scene: the float one when the frame has it, else the underlay
    uint32_t ui;
    uint32_t view;
    uint32_t reference;  // the 8-bit underlay (the debug views that compare)
    uint32_t gap;        // FrameLayers::Frame::gap: the first columns and rows are read from the next
    float ui_alpha;      // FrameLayers::UiAlphaFor: the UI layer's share of the frame (view 0 only)
  };
  static_assert(sizeof(Push) <= HostPasses::kPushConstantBytes);

  HostPasses* passes_ = nullptr;
  FrameLayers* layers_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  SceneImageSource* scene_source_ = nullptr;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  HostImage image_;                // hdr_source: the frame in the game's code values, float16
  uint64_t composed_generation_ = 0;  // the layers' generation, view and UI alpha the image holds
  uint32_t composed_view_ = 0;
  float composed_alpha_ = 1.0f;
  bool float_scene_ = false;      // the image holds the frame's float scene
  uint32_t idle_presents_ = 0;     // presents in a row without layers
  uint32_t inline_presents_ = 0;   // presents in a row that took the sum themselves (with an image held)

  struct Stats {
    uint64_t inlined = 0;         // presents that took the sum of the layers themselves
    uint64_t inlined_float = 0;   // ... of them with the float scene
    uint64_t composed = 0;
    uint64_t composed_float = 0;  // ... of them with the float scene (stage 3)
    uint64_t stale_scene = 0;     // a frame's float scene that was gone by the present
    uint64_t reused = 0;    // an idle present showed the last composition again
    uint64_t failed = 0;    // the image could not be made
  };
  Stats stats_;
};

}  // namespace mcla::gpu
