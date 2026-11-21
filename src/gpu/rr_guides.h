#pragma once
// ===========================================================================
// MCLA GPU renderer -- DLSS Ray Reconstruction's guide pass (perf/DLSS-RR.md, package RR-2)
// ===========================================================================
// Ray Reconstruction reads, next to the noisy colour, the depth and the motion vectors, five guide images at the render size that the frame has to make: the diffuse albedo, the specular albedo,
// the normals with the roughness, the specular hit distance and (optional) a responsivity mask. This class makes them with ONE compute pass at the anchor (the game's motion blur draw, where the
// upscaler runs: TemporalSystem::RecordUpscale calls it right before it hands the frame to the backend), from what the G-buffer already holds (gbuffer_types.h: RT1 the octahedral world normal, RT2
// the albedo and the lit weight, RT3 the smoothness and the class), the kept depth and the upscaler's masks. What each guide holds, the sky's and the no-data defaults, and the specular albedo
// (NVIDIA's EnvBRDFApprox2 over the class table's F0) are in hlsl/rr_guides_common.hlsli; the C++ twin and the rules that are not pixels are in rr_guides_rules.h; the check is rr_guides_test.cpp.
//
// WANT-DRIVEN. The pass runs only while it is wanted (BeginFrame, called by TemporalSystem at the frame boundary with rr_guides_rules.h PassWanted): the plan says Ray Reconstruction would be the
// choice if the guides were there (the DENOISER row, or the debug switch mcla_gpu_dlss_rr = on, chooses it; an RTX card that supports it, DLSS as the upscaler), or a debug view is on while an upscaler runs. While it is wanted
// it asks the G-buffer for all three planes (gbuffer::Consumer::kRayReconstruction: no plane is made unless somebody wants it), so the planes are there from the frame after the want. With the switch
// off and no view this class makes no image, no pipeline and no list command; a frame without the planes (the pause world, the garage, the map, a cutscene, the first frame) makes no guides and
// says why once (rr::FrameGate), and that frame is Super Resolution's.
//
// WHAT IT HANDS OVER. guides() is the TemporalGuides the frame's evaluate path tags (temporal_backend.h), ready() says that this frame's images were made. TemporalSystem::RecordRrGuides passes both to
// TemporalSystem::NoteRrGuides right after Record (RR-3): the frame's dispatch is Ray Reconstruction's only with them (and the plan, and the RR context: dlss_rr_rules.h ChooseDispatch), the next frame's
// plan reads that they were there. The hit distance image is written by this pass (0 on a surface,
// 65504 in the sky) and takes a writer's image through Inputs::hit_slot: the reflection effects of RR-4 draw their distances into an image of their own and hand its slot over.
//
// THE DEBUG VIEW (mcla_gpu_dlss_rr_view 1..6): after the upscaler has written its output, RecordView draws one guide over it (hlsl/rr_guides_view_cs.hlsl); the game's tone map and UI are drawn over
// that as usual. It needs the guide pass to have run in the frame and an upscaler to be running.
//
// MEMORY: five images at the render size, 28 bytes a pixel (rr_guides_rules.h): 58 MB at 1920x1080, 232 MB at 3840x2160 (DLAA at 4K), made by the first pass, kept while the pass is wanted and for
// kReleaseAfter frames after it stops being wanted (a quick switch back finds them), counted in TemporalSystem::HeldBytes.
//
// CHECK (nothing here was run): the planes' state at the anchor (SHADER_READ since the publish; a COMPUTE-stage barrier is recorded before the read, as RecordUpscale does for the depth), the storage
// image formats R16F on both backends (Vulkan: shaderStorageImageExtendedFormats, which every RTX card reports and Plume enables with the rest of what the device supports; the first image that cannot
// be made is logged by EnsureImage and the pass makes no guides, RR stays off; if it ever mattered the hit distance and the responsivity would become RGBA16F), the UAV barrier between the upscaler's write and the view's read-modify-write of its output, MSAA above 1 (the shader's variant for planes of the host's MSAA
// level; the upscalers hold the host MSAA at 1 except with the debug lever mcla_gpu_msaa_with_upscaler).
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "descriptors.h"
#include "dlss_rr_rules.h"
#include "gbuffer_types.h"
#include "host_passes.h"
#include "rr_guides_rules.h"
#include "temporal_backend.h"

namespace mcla::gpu {

class Device;
class FrameRing;
class GBuffer;
class PassTiming;

class RrGuides {
 public:
  // What the pass needs of the frame (TemporalSystem fills it at the anchor).
  struct Inputs {
    uint64_t frame = 0;
    uint32_t render_size[2] = {};
    const HostImage* depth = nullptr;          // the kept depth: R32_FLOAT, render size, 1 near / 0 far, SHADER_READ
    const HostImage* mask = nullptr;           // the upscaler's reactive mask (R8_UNORM, render size), or null
    const HostImage* transparency = nullptr;   // the transparency and composition mask, or null
    uint32_t hit_slot = rr_guides::kNoSlot;    // a hit distance writer's image (RR-4), or none
    // The camera of the frame's anchor: its axes in the world and the projection's numbers (temporal::Camera).
    float right[3] = {};
    float up[3] = {};
    float forward[3] = {};
    float tan_x = 0.0f;                        // tan of half the horizontal field of view
    float tan_y = 0.0f;                        // ... the vertical
    float lin[4] = {};                         // the projection's depth rows P22 P23 P32 P33
    bool pause_view = false;                   // the pause menu's, the garage's or the map's world (no G-buffer there)
  };

  RrGuides() = default;
  ~RrGuides();
  RrGuides(const RrGuides&) = delete;
  RrGuides& operator=(const RrGuides&) = delete;

  // The layouts, the shaders and the pass's pipelines are listed with the host precache only when the pass is first wanted (AddLive). Never fails the renderer.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing);
  void SetGBuffer(GBuffer* gbuffer) { gbuffer_ = gbuffer; }
  // The pipelines go first (the host builder's functions name this object's layouts and shaders), the GPU idle.
  void Shutdown();

  // The debug view the setting asks for now (mcla_gpu_dlss_rr_view; live).
  static rr_guides::View ViewSetting();
  // The frame boundary (TemporalSystem::UpdateDenoiserPlan): whether the pass is wanted this frame (rr_guides_rules.h PassWanted). Declares the G-buffer's want, forgets the last frame's guides,
  // reads the view, releases the images after kReleaseAfter frames of not being wanted.
  void BeginFrame(bool wanted);
  bool wanted() const { return wanted_; }

  // At the anchor, outside a render pass, before the upscaler: makes the five images (on the first frame, or when the render size changes) and records the pass. False: no guides this frame (the
  // reason is logged once for each kind). Declares the guides' layout for the upscaler: SHADER_READ at the COMPUTE stage.
  bool Record(plume::RenderCommandList* list, const Inputs& in);

  // After the upscaler wrote `output` (R16G16B16A16_FLOAT, STORAGE | UNORDERED_ACCESS, SHADER_READ, the output size): the debug view over it. False: no view this frame. `output` is left in SHADER_READ.
  bool RecordView(plume::RenderCommandList* list, HostImage& output);
  bool viewing() const { return ready_ && view_ != rr_guides::View::kOff; }

  // What the upscaler's Ray Reconstruction dispatch tags: the images of the frame, valid when ready() (made in this frame's Record).
  const TemporalGuides& guides() const { return guides_; }
  bool ready() const { return ready_; }
  rr::Gate gate() const { return gate_; }
  uint64_t HeldBytes() const;

 private:
  // The register spaces of the layouts: the bindless texture set four times (spaces 0 to 3: host_pass_common.hlsli), the output set, the push block.
  static constexpr uint32_t kOutputSet = 4;
  static constexpr uint32_t kPushSpace = 5;
  // Frames of not being wanted after which the images are given back.
  static constexpr uint32_t kReleaseAfter = 600;

  struct OutputSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    std::array<const plume::RenderTexture*, rr_guides::kGuideCount> textures{};
  };

  bool MakeLayoutsAndShaders(Device& device, BindlessHeap& heap);
  // The pipeline `index` (0 single-sample guides, 1 multisampled guides, 2 the view) when it is made; null while it is being made (listed in the background the first time it is asked for: the
  // pass never waits for a compile) or when it failed.
  plume::RenderPipeline* Pipeline(uint32_t index);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t index);
  bool EnsureImages(uint32_t width, uint32_t height);
  bool EnsureSet();
  bool EnsureViewSet(HostImage& output);
  void RetireSets();
  void ReleaseImages();
  void Refuse(rr::Gate gate, uint64_t frame, const char* why);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GBuffer* gbuffer_ = nullptr;

  // The layouts and shaders (made at the start, cheap); the pipelines (made in the background the first time the pass is wanted).
  bool ready_to_list_ = false;  // layouts and shaders exist
  std::array<bool, 3> listed_{};  // pipeline 0 single-sample, 1 multisampled, 2 the view: listed with the host precache (made in the background)
  std::array<bool, 3> failed_{};
  plume::RenderDescriptorSetBuilder guides_builder_;  // u0..u4 the five images
  plume::RenderDescriptorSetBuilder view_builder_;    // u0 the upscaler's output
  std::unique_ptr<plume::RenderPipelineLayout> guides_layout_;
  std::unique_ptr<plume::RenderPipelineLayout> view_layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 3> shaders_;     // single-sample, multisampled, the view
  std::array<std::unique_ptr<plume::RenderPipeline>, 3> pipelines_;

  // The images, in rr_guides::Guide order, and the sets that name them as storage images (a new set for a new texture: a set in use by the GPU is never rewritten).
  std::array<HostImage, rr_guides::kGuideCount> images_;
  OutputSet set_;
  OutputSet view_set_;
  const plume::RenderTexture* view_output_ = nullptr;

  // The frame.
  bool wanted_ = false;
  rr_guides::View view_ = rr_guides::View::kOff;
  uint32_t unwanted_frames_ = 0;
  bool ready_ = false;  // this frame's images were made
  rr::Gate gate_ = rr::Gate::kOpen;
  TemporalGuides guides_;

  // The log and the totals.
  bool first_logged_ = false;
  std::array<bool, 8> refused_logged_{};
  uint32_t image_for_[2] = {};
  uint64_t passes_run_ = 0;
  uint64_t views_run_ = 0;
  uint64_t not_ready_ = 0;   // frames the pass was wanted and the pipeline was still being made
  uint64_t refused_ = 0;     // frames the pass was wanted and the frame could not have guides
};

}  // namespace mcla::gpu
