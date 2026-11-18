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
//   mcla_gpu_car_selfrefl_rt_gloss    a debug override of the half angle, in degrees, of the cone the rays are spread over (2.5 by default; 0 = a
//                                     perfect mirror, one ray: the first version's razor edge, for a picture to compare with)
//   mcla_gpu_car_selfrefl_view = 5    the debug view of this pass's image at the paint's pixel (car_selfrefl.h): purple no image this frame,
//                                     cyan the image has no data there (the march ran), green the ray hits nothing of the car, yellow a hit
//                                     on a part the camera sees, orange a hit on a part it does not (the fallback colour), brighter the
//                                     heavier the weight; red: the shaded point is not in the history (as view 4)
//
// WHAT IT DOES, at the same trigger as the history image (CarSelfRefl::RecordHistory, right after it): one compute pass
// (hlsl/car_rt_refl_cs.hlsl, cs_6_5, inline ray queries) with a thread for every texel of the history image. A texel that holds a
// vehicle's pixel casts a primary ray from the camera against the car's opaque parts in a narrow window around the pixel's distance: a hit
// there is the player's car at that pixel, with its exact point and its mesh normal. From it a CONE of rays goes against body, glass
// and tyres (car_rt_refl_rules.h: 4 rays over a cone of mcla_gpu_car_selfrefl_rt_gloss degrees, a pattern turned by a different angle at each
// texel and, with a temporal upscaler, each frame, so that the weight of a texel is the share of the cone a part covers: the edge of a
// reflected part is as soft as the cone is wide where it is, sharp near and soft far, like a glossy paint's). Each hit is taken into the
// same frame's history image, where the colour of the part is if the camera sees it (the texels around the hit show a surface at its
// distance), else a stand-in (the pixel's own surface's mean colour in shade, nearly the whole weight: car_rt_refl_rules.h says why). A
// ray that hits the panel it left (it ran under its own triangle's plane: the grazing self-hit) counts as a miss.
// The output image is the history's size (R16G16B16A16_FLOAT, rgb colour, alpha the code and the weight: car_rt_refl_rules.h) and is for
// the NEXT frame's paint draws, as the history is: the paint variant (version 6) reads it at the shaded point's place in the history with a
// 3x3 quadratic B-spline of the texels that are the paint's own surface (the history's agreement test), in place of the march; where it has no
// data there (alpha 0: not the player's car by the triangles, another vehicle, a pixel whose primary ray found no surface at that distance)
// the march answers as before.
//
// RESOLUTION (decided with numbers): the image stays at the history's size (half of the picture at 4K: 1920x1080), not the picture's. A
// 4K picture has about 215k vehicle texels at half size and 860k at full: with a primary ray and a cone of 4 that is 1.1 M against 4.3 M rays
// (the second would cost about 1 to 1.6 ms, the first 0.25 to 0.45, see COST), and the image 66 MiB instead of 16. The cone already gives every
// texel four samples of the reflection, one for each picture pixel of the 2x2 block it stands for, so the ray density per pixel is the first
// version's at full size; the blocks the first version showed were its single binary ray per texel (a hit or a miss: an edge one
// texel wide, 2 pixels at 4K, stepping at every texel), which the cone's weight ramp (a hand's width wide at 1 m) and the paint's smooth 3x3
// reconstruction (no corner at a texel's edge) take away. The history's colour is not mipped: with the cone's four rays each reading the
// 2x2 footprint of its own hit, the colour of the cone is the mean over four points spread across it (a footprint growing with the hit's distance
// times the cone's angle, by supersampling), and the paint's 3x3 averages the texels' different turns of the pattern.
//
// TIMING: the car is drawn before the world, so its own paint cannot see the frame's triangles; the pass runs at the trigger (every car
// draw of the frame is in) and its image is used by the next frame, exactly as the history is, with the vehicle's motion and the
// camera's reprojected in the paint's matrix. CarRtScene must have built this frame's top level on the list BEFORE this pass (it is
// asked every frame this is on: Request).
//
// COST (an estimate, not measured: CHECK on the first run, the pass is timed as "car rt refl" by PassTiming): the pass reads the history
// image once for every texel and writes the output image once (2.1 M texels at 4K with the half-size image: about 0.03 ms); the vehicle's texels
// (about 215k of them at 4K in a chase view, a tenth) trace a primary ray and 2 to 4 cone rays (2 where the outer pair finds nothing: the
// roof, the bonnet, the sides, most of the car; 4 where a part is near), so 0.65 to 1.1 M rays of at most 4 m against a tree of 25 instances,
// at an assumed 3 to 5 G rays/s for rays this coherent and this short on an RTX 5080 class card (inline ray queries from compute, no shader
// reordering): 0.15 to 0.35 ms, plus about 25 history reads a texel for the hits' colours and the surface mean (5 M loads, a tenth of a
// millisecond at most) = 0.25 to 0.45 ms. It is on top of the history pass. Memory: one more image of the history's size (8 bytes a texel: 16 MiB
// at 4K with half-size history) while the effect is on; it goes back after 2 s without it, with the history.
//
// CHECK (the first run looks at these, view 5 and the log's "[car rt refl]" lines first):
//   1. the cost: the pass at 4K with the half-size history and at 1080p with the full-size one; with mcla_gpu_car_selfrefl_rt_gloss 0 (one ray)
//      the pass is the first version's cost, so the difference is the cone's.
//   2. the cone's width: 2.5 degrees is a guess at the paint's gloss. The paint's own constants (DL0.w, the specular power) were not logged for the
//      player's paint (perf/out/G8A.txt has only other shaders' groups); 2.5 degrees is the blur of the environment map's own sharp read (a 512x256
//      dual paraboloid: 0.7 degrees a texel at the middle of a hemisphere, 1.4 at its rim, bilinear), so a reflected part is as soft as the sky
//      in the same paint. Wider (6) is hazier, 0 is the razor.
//   3. the pattern: its noise should be invisible at 4K (the paint's 3x3 averages 9 texels of 4 rays); where an upscaler runs it turns by the
//      frame, where none does it stands still. If the grain shows, the cone's rays can go to 8 (car_rt_refl_rules.h kConeRays and the shader's loops).
//   4. the hidden-hit stand-in (the surface's mean colour x 0.4 at 0.85): the wing's underside on the lid should be a smooth dark band, no patches.
//      The weight and the shade are push constants (tune.x, tune.y) and constants of the rules header.
//   5. the self-hit rule (instance and facing, IsSelfHit): a lid or a bonnet that reflects nothing must stay the environment's colour at grazing
//      angles (view 5: no yellow on a flat panel). The exact test needs the triangles' positions (a fifth binding of CarRtScene's set, t4, and
//      the geometric normal of the committed triangle): not done, CarRtScene is another branch's.
//   6. the units of the history's colour against the paint's (the exponent bias gain), the reflection ray's reach (4 m;
//      mcla_gpu_car_selfrefl_rt_length) and the parts it hits (mcla_gpu_car_selfrefl_rt_parts; glass in by default).
//   7. the names of CarRtScene's per-frame request (Request, written as CarFrame's), View(), SetDesc() and the generated shader header
//      (car_rt_refl_cs.hlsl.dxil.h, g_car_rt_refl_cs_dxil) that larecomp_gpu_host_shader_rt makes: the first game run linked them (the
//      census counted ray traced passes), so this is for a new merge only.
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
    bool upscaler = false;          // a temporal upscaler works on this frame: the cone's pattern turns by the frame (it averages the pattern out)
    uint32_t pattern_phase = 0;     // 0 .. 7: the pattern's step for this frame (0 where no upscaler runs: the pattern stands still on the screen)
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
    uint32_t slots[4];  // history slot, the reflection ray's parts, the primary ray's parts, the cone pattern's phase
    float image[4];     // the image's size, the picture's size
    float lens[4];      // tan half field of view x and y, the divisor, the reflection ray's length
    float tune[4];      // the shade, the weight of a hidden hit, where the fade starts, the tangent of the cone's half angle
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
