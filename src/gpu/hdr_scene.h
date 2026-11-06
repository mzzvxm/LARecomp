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
// Until stage 3 the scene is the 8-bit one, so the picture is the front buffer's (to the
// rounding of the 8-bit blends, see frame_layers.h) and stage 2 is checked with the debug views
// (mcla_gpu_hdr_layers_view: 1 the UI layer, 2 the scene, 3 the UI's coverage). Stage 3 puts a
// float scene here, above paper white, and the UI stays at or below it: its own part of the sum
// never exceeds its coverage, so where it covers the scene it is paper white at most, and the
// scene's excess shows only where the UI lets the scene through.
//
// A frame without layers (the front buffer is not their picture, reaches past them, none made: the
// pause menus, movies, loading) is shown from the front buffer as stage 1 does.
//
// Memory: the float image (R16G16B16A16_FLOAT) at the front buffer's size: 7.4 MB at 1x and
// 66 MB at 3x, made on the first composed frame and released after as many presents without
// layers as FrameLayers waits.
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

  // Outside a render pass, once per present: when the guest frame last presented has layers and
  // `front` (the front buffer's texture key) is their picture at `width` x `height` host pixels,
  // composes their top left corner of that size into the float image (once per frame: an idle
  // present reuses it) and returns true; slot() names the image. False: show the front buffer.
  bool Compose(plume::RenderCommandList* list, const TextureKey& front, uint32_t width,
               uint32_t height);
  // The composed image's bindless slot.
  uint32_t slot() const { return image_.slot; }

  // Memory held by the float image.
  uint64_t HeldBytes() const;

  std::string TakeSummary();

 private:
  struct Push {  // hlsl/frame_layers_ps.hlsl, LAYERS_COMPOSE
    uint32_t underlay;
    uint32_t ui;
    uint32_t view;
    uint32_t reserved;
  };
  static_assert(sizeof(Push) <= HostPasses::kPushConstantBytes);

  HostPasses* passes_ = nullptr;
  FrameLayers* layers_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  HostImage image_;                // hdr_source: the frame in the game's code values, float16
  uint64_t composed_generation_ = 0;  // the layers' generation and view the image holds
  uint32_t composed_view_ = 0;
  uint32_t idle_presents_ = 0;     // presents in a row without layers

  struct Stats {
    uint64_t composed = 0;
    uint64_t reused = 0;    // an idle present showed the last composition again
    uint64_t failed = 0;    // the image could not be made
  };
  Stats stats_;
};

}  // namespace mcla::gpu
