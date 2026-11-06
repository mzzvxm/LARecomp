#pragma once
// ===========================================================================
// MCLA GPU renderer -- the reversible tone map around the temporal upscalers
// ===========================================================================
// The upscalers (FSR 3.1, DLSS) average the jittered frames. The game's scene colour is linear and
// scene referred (values up to about 32), so a sub-pixel detail 20 to 30 times brighter than what
// is around it (a sun glint on a car's roof, a lamp, a sign) is on a pixel in some jitter phases
// and not in others, and the average blinks (measured parked, 1280x720 window, Quality: 3 to 3.7% of
// the bright pixels against 0.02 to 0.2% with no upscaler). The standard remedy, here: the upscaler
// works on the compressed picture and its output is expanded again.
//
//   compress   the scene colour (a resolve product, render size) into `compressed()`:
//              c' = c / (1 + max(c.r, c.g, c.b)), in [0, 1), hue kept (hlsl/temporal_tonemap_ps.hlsl).
//              This is the upscaler's colour input; it is told the input is not HDR.
//   upscale    the backend writes `upscaled()` (output size, the compressed domain).
//   expand     c = c' / max(1 - max(c'.r, c'.g, c'.b), 1/64) into the image the rest of the frame
//              reads in place of the scene colour (TemporalSystem's output image).
//
// Two full-screen passes of the renderer's host layout (host_passes.h), RGBA16F both ways: the
// precision near 1 matters (c = 32 is c' = 0.97, and 1 - c' has to stay good to a few percent).
// It covers every mode, Native AA and DLAA included: they jitter the scene the same way and the
// upscaler's history blends the same bright outliers.
//
// Memory: `compressed()` is render size, `upscaled()` is output size, each RGBA16F (at 2560x1440
// about 28 MiB for the second, 63 MiB at 3840x2160), made when the upscaler first runs and released
// with it; HeldBytes() counts them (renderer.cpp's held list).
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "host_passes.h"

namespace mcla::gpu {

class PassTiming;

class TemporalToneMap {
 public:
  TemporalToneMap() = default;
  ~TemporalToneMap();
  TemporalToneMap(const TemporalToneMap&) = delete;
  TemporalToneMap& operator=(const TemporalToneMap&) = delete;

  // Nothing is made until the first Prepare.
  void Initialize(HostPasses& passes, PassTiming& pass_timing);
  // GPU idle (every retire action run), before HostPasses and the BindlessHeap shut down.
  void Shutdown();
  // The upscaler has been off for a while: the images go (retired behind the frames that name
  // them); the passes stay.
  void Release();
  // The video memory the two images hold now.
  uint64_t HeldBytes() const;

  // False once the passes could not be made (logged): the upscaler cannot run this session.
  bool available() const { return initialized_ && !failed_; }

  // The shaders and pipelines on first use, and the two images for these sizes (kept when they are
  // the same). False when one could not be made (logged).
  bool Prepare(uint32_t render_width, uint32_t render_height, uint32_t output_width,
               uint32_t output_height);

  // R16G16B16A16_FLOAT, render size, a render target. After RecordCompress it is in SHADER_READ
  // at the graphics stage; the caller takes it to the compute stage for the upscaler.
  HostImage& compressed() { return compressed_; }
  // R16G16B16A16_FLOAT, output size, STORAGE | UNORDERED_ACCESS: what the backend writes. The caller
  // makes it readable by a pixel shader after the dispatch (layout SHADER_READ, its `layout` field
  // set) and calls RecordExpand.
  HostImage& upscaled() { return upscaled_; }

  // Outside a render pass. `color_slot`: the bindless slot of the scene colour at the render size
  // (its gamma bit, if any, is dropped); the product must be readable by a pixel shader.
  void RecordCompress(plume::RenderCommandList* list, uint32_t color_slot);
  // Outside a render pass: `upscaled()` (in SHADER_READ) expanded into all of `target`, a render
  // target of the output size, left in SHADER_READ at the graphics stage.
  void RecordExpand(plume::RenderCommandList* list, HostImage& target);

 private:
  struct Push {  // hlsl/temporal_tonemap_ps.hlsl TonemapPush
    uint32_t source;
  };

  bool MakePipelines();

  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;

  std::unique_ptr<plume::RenderShader> compress_shader_;
  std::unique_ptr<plume::RenderShader> expand_shader_;
  std::unique_ptr<plume::RenderPipeline> compress_pipeline_;
  std::unique_ptr<plume::RenderPipeline> expand_pipeline_;

  HostImage compressed_;
  HostImage upscaled_;
  uint32_t logged_sizes_[4] = {};  // the sizes the log last named
};

}  // namespace mcla::gpu
