#pragma once
// ===========================================================================
// MCLA GPU renderer -- the masks for the temporal upscalers (reactive; transparency and composition)
// ===========================================================================
// feature-parity/GHOSTING-DESIGN.md 4 and 9.4/9.5. A motion vector says where one thing moved; a pixel
// that shows two things at once (a dark patch on the road that moves with the car while the road moves
// with the camera, smoke in front of a wall, glass in front of a car's seat) has no right vector, and
// the upscalers drag the older picture along behind it. The remedy is a mask: how much of the pixel is
// such a layer, so that the upscaler trusts its history less there.
//
//   reactive      R8, the share of the pixel that is a particle, a light volume or a shadow decal.
//                 FSR 3.1/4: `reactive`. DLSS (Streamline): the bias-current-colour hint and the
//                 reactive mask hint (TemporalInputs::mask).
//   transparency  R8, the share of the pixel that is car glass or broken glass. FSR:
//   and composition  `transparencyAndComposition` (the pixel lock is reduced); DLSS: the transparency
//                 and composition hint (TemporalInputs::transparency).
//
// How, with no colour snapshot and no test of the colour's change (that flagged stable light pools in the
// fork): the Scene (scene.cpp PrepareUpscalerMask, RecordUpscalerMask) draws each such draw of the main
// view a second time, right after the game's own, into an RGBA8 "coverage" image of the render size
// (one per layer) with the game's own depth and stencil test, the depth not written, only the coverage
// written (VariantKey), the way HdrToneMap duplicates its draws. The same vertex shader, pixel shader and
// constants, so the same jitter, and the pixel shader's alpha or colour is what is left. At the anchor
// Resolve() turns each image into its R8 mask (upscaler_masks_ps.hlsl).
//
// The draws are told by shader identity (LARecomp's own, native_gfx/shader_identity.h: FNV-1a 64 of
// the vfetch-normalised microcode). The renderer has no table of shader names, so the identities were
// read off the shipped shader containers by the pack builder's own walk (tools/build_shader_pack_v8.py
// with the HLSL tree's shader report, which names every container's shaders: group and entry point),
// the same walk that gives the pack's keys; the shadow decals' pair, the anchor and the tone map came out
// with the names the census gave them (PS_CalcShadowsQuad, PSStreakMotionBlur, PSToneMap). The tables in
// upscaler_masks.cpp say each identity's family and entry point.
// CHECK: that each of these families really draws into the main view's colour target before the anchor,
// blended, in the phases GHOSTING-DESIGN.md 4.2 names, and what blends they use: the census test patch
// (ports\diffs\upscaler-masks-census.patch) lists, for every blended main view draw before the anchor,
// its pair, blend, depth state and how many a frame, with the family the tables give it (or none).
//
// Not covered, on purpose: the fog quad (it changes every far pixel smoothly: not reactive), the night
// glows (xstar_glow is drawn after the upscaler), the sun's cast shadow of a car (no draw of its own).
//
// Threading: render thread only.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "host_passes.h"

namespace mcla::gpu {

class Device;
class PassTiming;
struct PipelineKey;

class UpscalerMasks {
 public:
  // What a draw is, for the mask: the shadow decals of the cars and the particle families (reactive),
  // the light volumes (reactive) and the glass (transparency and composition).
  enum class Kind : uint8_t { kNone, kShadowDecal, kParticle, kLightVolume, kGlass, kCount };
  enum class Layer : uint8_t { kReactive, kTransparency, kCount };
  static constexpr size_t kKindCount = size_t(Kind::kCount);
  static constexpr size_t kLayerCount = size_t(Layer::kCount);

  // The value of the reactive mask where a layer is opaque (the layer's own opacity scales it down), and
  // of a draw that adds light by its colour alone, and the transparency mask where a glass pixel has
  // any opacity at all: glass is mostly transparent (an alpha of 0.1 to 0.4 is a windscreen), and the
  // pixel lock is wanted gone wherever it shows, so the opacity is multiplied up.
  // CHECK: tune in the drive (frame dumps, FSR and DLSS, 30/60/144, 1080p and 720p): too high shimmers
  // the road under a car, the smoke and the glass; too low leaves the trail behind them.
  static constexpr float kReactiveGain = 1.0f;
  static constexpr float kAddedLightGain = 1.0f;
  static constexpr float kTransparencyGain = 4.0f;

  UpscalerMasks() = default;
  ~UpscalerMasks();
  UpscalerMasks(const UpscalerMasks&) = delete;
  UpscalerMasks& operator=(const UpscalerMasks&) = delete;

  // What a draw with these shader identities is (kNone: nothing the masks take). The Scene asks for the
  // draws of the main view before the anchor that write one colour target.
  static Kind KindOf(uint64_t vs_identity, uint64_t ps_identity);
  static const char* KindName(Kind kind);
  static Layer LayerOf(Kind kind) {
    return kind == Kind::kGlass ? Layer::kTransparency : Layer::kReactive;
  }
  // The key of the second draw: the game's, written into one RGBA8 target of one sample with the
  // game's depth and stencil state and neither written, only the coverage written, and nothing else
  // changed: the blend is the coverage's (an alpha blend leaves its opacity in alpha, the union of the
  // layers; an additive blend, or a premultiplied one, its colour added in rgb).
  static void VariantKey(const PipelineKey& key, PipelineKey* out);

  void Initialize(HostPasses& passes, PassTiming& pass_timing);
  // GPU idle, before HostPasses shuts down.
  void Shutdown();
  // The switch went off: the images go (retired behind the frames that name them).
  void Release();
  // The video memory the images hold now.
  uint64_t HeldBytes() const;

  // The Scene's frame boundary: nothing is drawn yet in the new frame.
  void BeginFrame();

  // Why a draw that was a candidate left no coverage (counted, for the summary).
  enum class Skip : uint8_t { kNotBlended, kNoDepth, kSizeOrSamples, kNoImage, kPipelinePending, kCount };
  void Skipped(Skip why) { ++stats_.skipped[size_t(why)]; }

  // A second draw is about to be recorded, for a main view of `width` x `height` host pixels: the
  // image of `layer` to draw into (null when it cannot be made, counted). `*clear` is true for the
  // layer's first draw of a frame: the caller binds the image's own framebuffer and clears it. The
  // caller moves the image to COLOR_WRITE itself, outside a render pass (HostImage::layout), draws with
  // DepthFramebuffer(), and calls Drawn() once the draw is in.
  HostImage* Target(Layer layer, uint32_t width, uint32_t height, bool* clear);
  // The framebuffer of `layer`'s image with the game's depth `depth` beside it (the depth target's
  // texture, and `generation` of the texture it is, EdramTargets::Target::generation). Kept until the
  // image or the depth is another one. Null when it cannot be made (logged).
  plume::RenderFramebuffer* DepthFramebuffer(Layer layer, const plume::RenderTexture* depth,
                                             uint32_t generation);
  // A second draw of `kind` is in: the layer's image holds something (and is cleared for this frame).
  void Drawn(Kind kind) {
    LayerState& l = layers_[size_t(LayerOf(kind))];
    l.clear_pending = false;
    l.drawn = true;
    ++stats_.drawn[size_t(kind)];
    ++frame_drawn_;
  }

  // At the anchor, outside any render pass: the R8 masks of this frame at the render size. A layer
  // gets one when the frame drew into it (or mcla_gpu_upscaler_mask_test asks for all ones); false
  // when neither has one. The mask is null for a layer that has none.
  bool Resolve(plume::RenderCommandList* list, uint32_t render_width, uint32_t render_height);
  // This frame's mask of `layer` when Resolve made one, SHADER_READ; else null.
  const HostImage* mask(Layer layer) const {
    const LayerState& l = layers_[size_t(layer)];
    return l.mask_valid ? &l.mask : nullptr;
  }

  // For the summary: the draws recorded again by family, the most in one frame, the frames with a mask.
  std::string Summary() const;

 private:
  struct MaskPush {  // hlsl/upscaler_masks_ps.hlsl
    uint32_t coverage;
    float gain;
    float color_gain;
    uint32_t mode;
  };
  static_assert(sizeof(MaskPush) == 16);

  struct LayerState {
    HostImage coverage;  // RGBA8: the second draws' coverage, cleared at the layer's first draw of a frame
    HostImage mask;      // R8_UNORM, the upscaler's input
    // The coverage image's framebuffer with the game's depth beside it, for the draws that test depth.
    std::unique_ptr<plume::RenderFramebuffer> depth_framebuffer;
    const plume::RenderTexture* fb_depth = nullptr;
    const plume::RenderTexture* fb_color = nullptr;
    uint32_t fb_generation = 0;
    bool clear_pending = true;  // the next Target() is the frame's first
    bool drawn = false;         // a second draw went into `coverage` this frame
    bool mask_valid = false;
  };

  struct Stats {
    uint64_t drawn[kKindCount] = {};   // second draws recorded, by kind
    uint64_t frames[kLayerCount] = {};     // frames with at least one, by layer
    uint64_t resolved[kLayerCount] = {};   // masks made, by layer
    uint64_t forced[kLayerCount] = {};     // ... of them all ones (the diagnostic)
    uint64_t skipped[size_t(Skip::kCount)] = {};
    uint64_t size_mismatch = 0;        // frames whose image was not the render size
    uint64_t image_failures = 0;
    uint64_t max_in_frame = 0;         // the most second draws in one frame
    uint64_t frames_any = 0;           // frames with at least one second draw (any layer)
  };

  bool Prepare();
  void DropFramebuffer(LayerState& layer);

  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  LayerState layers_[kLayerCount];
  uint64_t frame_drawn_ = 0;  // second draws in the frame being recorded
  Stats stats_;
  bool mismatch_logged_ = false;
};

}  // namespace mcla::gpu
