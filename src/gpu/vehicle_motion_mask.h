#pragma once
// ===========================================================================
// MCLA GPU renderer -- the mask of the cars' shadow decals, for the temporal upscalers
// ===========================================================================
// feature-parity/GHOSTING-DESIGN.md 9.4 and 9.5. Under every car the game draws a dark blended patch on
// the road, and four smaller ones under the player's wheels. The patch moves with the car while the road
// under it moves with the camera, so each pixel of it has two motions at once and no motion vector is
// right: the upscalers drag the dark edge along behind the car. The remedy for a blended layer is the
// reactive mask: the pixel's history is trusted less in proportion to the layer's opacity.
//
// How, with no colour snapshot: the Scene (scene.cpp PrepareShadowMask, RecordShadowMask) draws each of
// these draws a second time, right after the game's own, into an RGBA8 image of the render size
// whose alpha is the patch's opacity (the pipeline of VariantKey: no depth, no stencil, only alpha
// written, alpha blended as the union of the layers), the way HdrToneMap duplicates its draws. At the
// anchor, Resolve() turns that alpha into the R8 mask the backends take: the opacity times a gain, kept
// only where the stencil is 0 (vehicle_motion_mask_ps.hlsl: the game's own draw has a stencil test of
// EQUAL 0, which the second draw has not). FSR reads it as its reactive mask, DLSS as the bias-current-
// colour hint (temporal_backend.h TemporalInputs::mask).
//
// The draws are told by the shader pair the census found (GHOSTING-DESIGN.md 9.4: the same pair for
// every car on screen, parked or driving; LARecomp's own identities, native_gfx/shader_identity.h):
//   PS 0252424701F07592 / VS A829AE1437B66C8B   the contact shadow of a car, 180 vertices, 1 a car
//   PS B4C34BD40E64FB0F / VS 72F466E81A37357E   the four wheel patches of the player's car
// A different game build or shader pack gives other values: nothing fails, the mask is just never made.
// CHECK: that these two draws are the shadow (the census told them by position, state and size, the
// picture with the draws left out was not taken): `mcla_gpu_test_vehicle_census_skip_ps` on a build with
// the census test patch shows it.
//
// What it does not cover: the sun's cast shadow of a car on the road is part of the road's own shading
// (the cascaded shadow map is read by the world's draws): it has no draw of its own.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "host_passes.h"

namespace mcla::gpu {

class Device;
class PassTiming;
struct PipelineKey;

class ShadowMask {
 public:
  // The mask's value where a decal is opaque (the decal's own opacity scales it down). 1: the history
  // of the pixel is rejected as far as the decal covers it.
  // CHECK: tune in the drive (frame dumps of the area under the car, FSR and DLSS, 30/60/144): too
  // high shimmers the road under the car, too low leaves a trail behind it.
  static constexpr float kGain = 1.0f;

  ShadowMask() = default;
  ~ShadowMask();
  ShadowMask(const ShadowMask&) = delete;
  ShadowMask& operator=(const ShadowMask&) = delete;

  // The shader pair of a decal this mask is made of.
  static bool IsDecal(uint64_t vs_identity, uint64_t ps_identity);
  // The key of the second draw: the game's, written into one RGBA8 target without depth or stencil
  // test, one sample, only alpha written and blended as the union of layers (a + (1 - a) dst).
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
  // A decal's second draw is about to be recorded, for a main view of `width` x `height` host pixels:
  // the image to draw into (null when it cannot be made, logged). `*clear` is true for the first draw
  // of a frame: the caller binds the image's framebuffer and clears it. The caller moves the image to
  // COLOR_WRITE itself, outside a render pass (HostImage::layout), and calls Drawn() once the draw is in.
  HostImage* Target(uint32_t width, uint32_t height, bool* clear);
  void Drawn() { ++stats_.drawn; }

  // At the anchor, outside any render pass: the R8 mask of this frame's decals at the render size, from
  // `stencil_ref` (TemporalMotion::StencilRef). False when the frame has none (or it cannot be made):
  // then mask() is null.
  bool Resolve(plume::RenderCommandList* list, uint32_t render_width, uint32_t render_height,
               uint32_t stencil_ref);
  // This frame's mask when Resolve made one, SHADER_READ; else null.
  const HostImage* mask() const { return mask_valid_ ? &mask_ : nullptr; }

  // For the summary: the draws and the frames with a mask.
  std::string Summary() const;

 private:
  struct MaskPush {  // hlsl/vehicle_motion_mask_ps.hlsl
    uint32_t coverage;
    uint32_t stencil;
    float gain;
    uint32_t reserved;
  };
  static_assert(sizeof(MaskPush) == 16);

  bool Prepare();

  struct Stats {
    uint64_t drawn = 0;        // second draws recorded
    uint64_t frames = 0;       // frames with at least one
    uint64_t resolved = 0;     // masks made
    uint64_t size_mismatch = 0;  // frames whose image was not the render size
    uint64_t image_failures = 0;
  };

  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  HostImage coverage_;  // RGBA8, alpha: the second draws' opacity, cleared at the frame's first draw
  HostImage mask_;      // R8_UNORM, the upscaler's input
  bool clear_pending_ = true;  // the next Target() is the frame's first
  bool drawn_ = false;         // a second draw went into coverage_ this frame
  bool mask_valid_ = false;
  Stats stats_;
  bool mismatch_logged_ = false;
};

}  // namespace mcla::gpu
