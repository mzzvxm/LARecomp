#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD FSR Ray Regeneration: the screen-space reflections' guide and signal pass (perf/RAY-REGEN.md, packages RG-2 and RG-3)
// ===========================================================================
// Ray Regeneration is a decoupled denoiser (ray_regen_rules.h): it denoises a SIGNAL image, not the colour, with guides in its own formats. For the screen-space reflections the signal is the trace's
// radiance (ssr.h: one ray a half-size pixel, never below 0) with the ray's hit distance in the alpha, and the guides (signed linear depth, motion, octahedral normals with the linear roughness, the
// specular albedo) are made from what the reflections already read at the trigger: the Hi-Z, the G-buffer's planes, the camera's own axes and motion. ONE compute pass (hlsl/ray_regen_ssr_cs.hlsl, Shader
// Model 6.0 plain compute; the 6.6 AMD asks for is its runtime's) writes all five images at the trace's size, and the dispatch (ray_regen.h Dispatch) is at that size too: a denoiser of a half-size trace.
//
// The pass is made for Direct3D 12 only (AMD ships no Vulkan provider): on Vulkan Initialize makes nothing and Available() is false. It is want-driven like rr_guides.h: the pipeline is listed with the host
// precache the first time a frame asks for it (a frame never waits for a compile), the images are made by the first frame that records the pass and given back after kReleaseAfterFrames frames in which
// no frame did (rgd::kReleaseAfterFrames), counted in HeldBytes (the renderer's VRAM line names it with the reflections).
//
// STATES. The trace (a graphics target the pass before it left in SHADER_READ) is moved to a state both stages read; the shared products the pass reads (the Hi-Z, the planes, the depth plane) are read
// in the layout their owners left them in, as the ray traced occlusion's compute pass reads the AO's depth (car_rt_ao.cpp: CHECK C8). The five outputs are written in GENERAL (unordered access) and left
// in SHADER_READ at the COMPUTE stage, the state the dispatch's inputs are declared in; the denoised image is made here too and handed to the runtime in GENERAL.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "descriptors.h"
#include "host_passes.h"
#include "ray_regen_guides_rules.h"
#include "temporal_backend.h"

namespace mcla::gpu {

namespace rgd = rayregen::guides;

class Device;
class FrameRing;
class PassTiming;

class RayRegenSsrPass {
 public:
  RayRegenSsrPass() = default;
  ~RayRegenSsrPass();
  RayRegenSsrPass(const RayRegenSsrPass&) = delete;
  RayRegenSsrPass& operator=(const RayRegenSsrPass&) = delete;

  // The layout and the shader (cheap); the pipeline is listed with the host precache when first asked for. Direct3D 12 only: nothing is made on another backend. Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing);
  // The pipeline goes first (the host builder's functions name this object's layout and shader), the GPU idle.
  void Shutdown();

  bool Available() const { return ready_to_list_; }
  // The pass's pipeline, null while it is being made (listed the first time it is asked for) or when it failed.
  bool PipelineReady();

  // At the trigger, right after the trace: makes the images at `width` x `height` (the trace's size) when they are not there, records the pass, and leaves the guides and the signal in SHADER_READ at
  // the COMPUTE stage. `trace` is the trace's image, which the pass reads through the push's slot. False: nothing was recorded (an image could not be made: logged by the host passes).
  bool Record(plume::RenderCommandList* list, const rgd::Push& push, HostImage& trace, uint32_t width, uint32_t height);
  // The denoised image (rgb the runtime's output, alpha the hit distance preserved): made with the others, in GENERAL for the dispatch. `ReadDenoised` moves it to the layout the resolve reads.
  void PrepareDenoised(plume::RenderCommandList* list);
  void ReadDenoised(plume::RenderCommandList* list);

  const HostImage& image(rgd::Image image) const { return images_[size_t(image)]; }
  // The image as the dispatch names it (texture, size, format, flags).
  TemporalImage Tagged(rgd::Image image) const;

  // The frame boundary: counts the frames that did not record the pass; the images go back after rgd::kReleaseAfterFrames of them.
  void OnFrame(bool recorded);
  uint64_t HeldBytes() const;
  uint64_t passes_run() const { return passes_run_; }

 private:
  // The register spaces of the layout: the bindless texture set four times (spaces 0 to 3), the sampler set (4: not read, bound for the root signature), the output set (5), the push block at (b0, space 4).
  static constexpr uint32_t kSamplerSet = 4;
  static constexpr uint32_t kOutputSet = 5;
  static constexpr uint32_t kPushSpace = 4;
  // The precache variant of the pass (ssr.h's own variants are below 32).
  static constexpr uint32_t kVariant = 64;

  bool MakeLayoutAndShader(Device& device, BindlessHeap& heap);
  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  bool EnsureImages(uint32_t width, uint32_t height);
  bool EnsureSet();
  void RetireSet();
  void ReleaseImages();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  bool ready_to_list_ = false;   // the layout and the shader exist (Direct3D 12)
  bool listed_ = false;
  bool failed_ = false;
  plume::RenderDescriptorSetBuilder outputs_builder_;   // u0 .. u4 the five images
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  std::array<HostImage, rgd::kImageCount> images_;
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  std::array<const plume::RenderTexture*, rgd::kPassImageCount> set_textures_{};

  uint32_t idle_frames_ = 0;
  uint32_t image_for_[2] = {};
  bool first_logged_ = false;
  uint64_t passes_run_ = 0;
};

}  // namespace mcla::gpu
