#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's self-reflection, ray traced
// ===========================================================================
// The screen-space self-reflection (car_selfrefl.h) marches a ray through an image of the car as the camera saw it: a part the
// camera does not see cannot be reflected, a thin part is guessed from a few points. On a device that can trace rays this pass
// replaces the march for the player's own car with the answer from its triangles (car_rt_scene.h: one top-level structure of the
// car in the frame's view space, built at the history trigger), keeping the same picture of the car (the history image) for the
// COLOUR of what the ray hits.
//
//   mcla_gpu_car_selfrefl_rt          auto | off. auto (the default) is on where the device can ray trace the car (CarRtSupported: D3D12
//                                     with DXR 1.1 today) and the car self-reflections (mcla_gpu_car_selfrefl) are on; off keeps the
//                                     screen-space march everywhere. Live: it applies from the next frame. Where the device cannot,
//                                     auto is the march, so nothing changes for it.
//   mcla_gpu_car_selfrefl_rt_length   a debug override of the reflection ray's reach in metres (4 by default; the weight of a hit falls to
//                                     nothing from 0.6 of it)
//   mcla_gpu_car_selfrefl_rt_parts    a debug override of the parts the reflection ray hits: a mask of car_rt_common.hlsli's
//                                     CAR_RT_MASK_* (1 body, 2 glass, 4 tyres, 8 the driver; 7 by default)
//   mcla_gpu_car_selfrefl_view = 5    the debug view of this pass's image at the paint's pixel (car_selfrefl.h): purple no image this frame,
//                                     cyan the image has no data there (the march ran), green the ray hits nothing of the car, yellow a hit
//                                     on a part the camera sees, orange a hit on a part it does not (the fallback colour), brighter the
//                                     heavier the weight; red: the shaded point is not in the history (as view 4)
//
// WHAT IT DOES, at the same trigger as the history image (CarSelfRefl::RecordHistory, right after it): one compute pass
// (hlsl/car_rt_refl_cs.hlsl, cs_6_5, inline ray queries) with a thread for every texel of the history image. A texel that holds a
// vehicle's pixel casts a primary ray from the camera against the car's opaque parts in a narrow window around the pixel's distance: a hit
// there is the player's car at that pixel, with its exact point and its mesh normal. From it the reflection ray goes against body, glass
// and tyres; its hit is taken into the same frame's history image, where the colour of the part is if the camera sees it (the texels around
// the hit show a surface at its distance), else a stand-in (the pixel's own colour in shade, half the weight: car_rt_refl_rules.h says why).
// The output image is the history's size (R16G16B16A16_FLOAT, rgb colour, alpha the code and the weight: car_rt_refl_rules.h) and is for
// the NEXT frame's paint draws, as the history is: the paint variant (version 5) reads it at the shaded point's place in the history, with
// the history's own agreement test, in place of the march; where it has no data there (alpha 0: not the player's car by the triangles,
// another vehicle, a pixel whose primary ray found no surface at that distance) the march answers as before.
//
// TIMING: the car is drawn before the world, so its own paint cannot see the frame's triangles; the pass runs at the trigger (every car
// draw of the frame is in) and its image is used by the next frame, exactly as the history is, with the vehicle's motion and the
// camera's reprojected in the paint's matrix. CarRtScene must have built this frame's top level on the list BEFORE this pass (it is
// asked every frame this is on: Request).
//
// COST (an estimate, not measured: CHECK on the first run): the pass reads the history image once for every texel and writes the output
// image once (2.1 M texels at 4K with the half-size image: about 0.03 ms), and the two rays of a car texel are two short traversals of a
// tree of about 200 instances (a few tenths of a millisecond at most for the 0.1 to 0.15 of the picture a car takes, on an RTX class
// card). It is on top of the history pass. Memory: one more image of the history's size (8 bytes a texel: 16 MiB at 1080p, 16 MiB at
// 4K with half-size history, 63 MiB at 4K with the full size) while the effect is on; it goes back after 2 s without it, with the
// history.
//
// CHECK (nothing here has run; the first run looks at these, view 5 and the log's "[car rt refl]" lines first):
//   1. the names of CarRtScene's per-frame request (Request, written as CarFrame's), View(), SetDesc() and the generated shader header
//      (car_rt_refl_cs.hlsl.dxil.h, g_car_rt_refl_cs_dxil) that larecomp_gpu_host_shader_rt makes; the Scene calls CarRtScene's trigger entry BEFORE
//      car_refl_->OnResolve (else View().valid is false at this pass and the march runs: the log says "no acceleration structures"); the top
//      level is readable by a compute shader after its build (a UAV barrier: View().valid says it is built on this list).
//   2. the view-space axes of the instances against hlsl/ao_common.hlsli AoViewPos (the primary ray finds the car's pixel at the history's
//      distance only if they agree: view 5 is cyan over the whole car when it does not, and the log's census shows "frames wanted but without
//      structures" at 0).
//   3. the shading-normal problem: the reflection ray leaves along reflect(view, mesh normal) and the mesh normal can differ from the triangle's
//      plane by more than the angle the ray leaves the surface at (a large triangle with smooth normals, a grazing view), so the ray runs under
//      its own triangle and "hits" the panel it left. It shows as the paint reflecting its own colour on a flat or gently curved panel at grazing
//      angles (view 5: yellow patches on a hood that reflects nothing). The fix is to take the triangle's own normal from two more primary rays
//      (the three points on one primitive) and treat a ray under it as a miss; not done: the offset along the normal (4 mm + 2 mm a metre) is
//      all there is. Measure first.
//   4. the units of the history's colour against the paint's (the exponent bias gain, as version 4's CHECK 1), the weight of a hidden hit (0.5)
//      and its colour (the pixel's own times 0.4), the reflection ray's reach (4 m; mcla_gpu_car_selfrefl_rt_length) and the parts it hits
//      (mcla_gpu_car_selfrefl_rt_parts; glass in by default).
//   5. the cost (the pass at 4K with the half-size history, and at 1080p with the full-size one), against the history pass's 0.1 ms.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipeline is made ahead on the host builder
// thread where the effect is on at the start (host_precache.h), and in the background where it is switched on later: the pass is not
// recorded until it is made (a frame never waits for a compile).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_rt_refl_rules.h"
#include "car_selfrefl_rules.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;

class CarRtRefl {
 public:
  CarRtRefl() = default;
  ~CarRtRefl();
  CarRtRefl(const CarRtRefl&) = delete;
  CarRtRefl& operator=(const CarRtRefl&) = delete;

  // After the CarRtScene's own Initialize (its descriptor set exists): the device's answer (CarRtSupported), the layout, the compute
  // shader, and the pipeline's place in the host precache where the car self-reflections are on at the start
  // (`reflection_on_at_start`: CarSelfRefl::ListedAtStart). Never fails the renderer: without them nothing is traced. `scene` may be null.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* scene,
                  bool reflection_on_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The frame's start (CarSelfRefl::BeginFrame): reads the setting. `reflection_on` is whether the car self-reflections are on. When the
  // pass is wanted and can run, asks the scene for this frame's structures.
  void BeginFrame(bool reflection_on);
  // True while the pass is wanted this frame.
  bool active() const { return active_; }

  // What the history pass says about the image it just made (CarSelfRefl::RecordHistory).
  struct HistoryInfo {
    HostImage* image = nullptr;     // R16G16B16A16_FLOAT, left readable by the history pass
    uint32_t divisor = 1;           // the picture over the image, 1 or 2 (car_selfrefl_rules.h HistoryDivisor)
    uint32_t render_width = 0;      // the picture in host pixels
    uint32_t render_height = 0;
    double tan_x = 0.0;             // tan of half the horizontal and vertical field of view the history was linearised with
    double tan_y = 0.0;             //   (1 / |P00| and 1 / |P11| of the previous frame's projection, as the AO takes them)
    uint64_t frame = 0;             // the guest frame the history is of (what SlotFor is asked with)
  };
  // Records the pass on `list` (outside any render pass, at the trigger, after the history pass and the scene's top-level build). True when
  // recorded. False: not wanted, no structures this frame, or the pass could not be set up; the paint then has no image for the frame.
  bool OnHistory(plume::RenderCommandList* list, const HistoryInfo& info);

  // The bindless slot of the image the paint draws of the frame after `history_frame` read, or car_selfrefl::kNoRtImage when the pass did
  // not run for that history.
  uint32_t SlotFor(uint64_t history_frame) const;

  // The image goes back (the history's went: no car for 2 s, or the setting off).
  void ReleaseImage();
  uint64_t HeldBytes() const;
  // The text CarSelfRefl's census line ends with (the totals, and the interval since the last call); empty where there is no ray tracing.
  std::string CensusText();

 private:
  // The push block of the pass (hlsl/car_rt_refl_cs.hlsl CarRtReflPush), 64 bytes.
  struct Push {
    uint32_t slots[4];  // history slot, the reflection ray's parts, the primary ray's parts, unused
    float image[4];     // the image's size, the picture's size
    float lens[4];      // tan half field of view x and y, the divisor, the reflection ray's length
    float tune[4];      // the shade, the weight of a hidden hit, where the fade starts, unused
  };
  static_assert(sizeof(Push) == 64);

  // The register spaces of the layout: the bindless texture set, the output's, the car's structures' (hlsl CAR_RT_SPACE), the push block.
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kPushSpace = 3;

  bool MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  plume::RenderPipeline* Pipeline();
  // The pipeline is made (or being made): false while it is not, so the pass is not wanted for the frame (a live switch to on).
  bool PipelineReady();
  bool EnsureOutput(uint32_t width, uint32_t height);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* scene_ = nullptr;

  bool supported_ = false;      // the device can trace the car (CarRtSupported)
  bool ready_ = false;          // layout and shader exist
  bool listed_ = false;         // the pipeline was listed (at the start, or in the background after it)
  bool pipeline_failed_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  // The output image and its descriptor set (a new set for a new texture: a set in use by the GPU is never rewritten).
  HostImage image_;
  std::unique_ptr<plume::RenderDescriptorSet> output_set_;
  const plume::RenderTexture* output_set_texture_ = nullptr;

  // The frame.
  bool active_ = false;
  bool recorded_ = false;      // the image holds the trace of `recorded_frame_`'s history
  uint64_t recorded_frame_ = 0;

  // The log and the totals.
  bool first_logged_ = false;
  bool no_scene_logged_ = false;
  uint32_t image_for_[2] = {};
  uint64_t passes_run_ = 0;
  uint64_t no_scene_ = 0;       // frames the pass was wanted but the scene had no structures
  uint64_t not_ready_ = 0;      // frames it was wanted but the pipeline was still being made
  uint64_t census_passes_ = 0;
};

}  // namespace mcla::gpu
