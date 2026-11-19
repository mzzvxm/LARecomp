#pragma once
// ===========================================================================
// MCLA GPU renderer -- ambient occlusion and contact shadows around the player's car and the cars beside it, ray traced
// ===========================================================================
// The screen-space occlusion (ao.h) and the contact march (hlsl/ao_contact_ps.hlsl) see only what the camera sees: not the far side of a wheel
// well, not the body over a tyre, not the road at the foot of a car whose underside hides it. On a device that can trace rays against the cars
// (car_rt_scene.h: one top level structure of the player's car and the nearby cars in the frame's view space) this pass casts a few SHORT rays from
// every pixel near them, and the occlusion's own passes take the answer: the main pass (the plain one and the bounce light's) the occlusion term (the
// darker of its own visibility and this one's), the contact march the sun term (the darker of its shadow, the car's shadow map's and this one's).
// car_rt_ao_rules.h has the design, the numbers and the maths that need no device; hlsl/car_rt_ao_cs.hlsl the pass.
//
// There is no setting of its own for a player: it belongs to the occlusion (mcla_gpu_ao) and the contact shadows (mcla_gpu_contact_shadows), runs
// where the device can trace the car (the same decision the ray traced car self-reflection makes: CarRtSupported; D3D12 with DXR 1.1, or Vulkan with
// ray queries) and where one of the two is on, and
// follows their levels (the occlusion's high traces 2 rays a pixel, ultra 4) and their class defaults (an integrated GPU has neither on). The debug
// cvars (ao.h's list):
//
//   mcla_gpu_ao_rt           auto | off | ao | contact. auto (the default) both terms; off none (the screen-space passes alone: the A/B);
//                            ao the occlusion term only; contact the sun ray only. Live.
//   mcla_gpu_ao_rt_length    the occlusion rays' reach in metres (0.6 by default). Live.
//   mcla_gpu_ao_rt_rays      the occlusion rays a pixel, 0 (the level's: 2 or 4) to 4. Live.
//   mcla_gpu_contact_shadows_view 6   the sun term alone as the contact visibility picture (white lit, black in the car's shadow).
//   mcla_gpu_ao_view 2       the occlusion's visibility (with the term in it): compare with mcla_gpu_ao_rt off.
//
// WHERE IT RUNS. The structures are built at the history trigger (CarRtScene::OnResolve, the game's mid-scene depth copy), and the occlusion's passes
// run at the same copy, so the Scene builds the structures BEFORE the occlusion's passes in every frame (scene.cpp OnResolve documents the order; the
// structures' check pass, which reads the occlusion's depth, comes after). The pass is one dispatch inside AmbientOcclusion::Run, after the depth chain
// and before the visibility pass: it reads the occlusion's level 0 linear depth (vehicle bit in its lowest bit) and writes an image of the same size
// that the visibility pass (lin.x, flag bit 31: the plain pass and the bounce light's) and the contact march (slots.x, flag bit 31) read.
//
// THE REGION. The player's car's world box (CarFrame's, or a sphere of a few metres where the frame has none) through the frame's camera gives a box in
// view space (car_rt_ao_rules.h ViewBoxFor), and each other car of the top level has its own (CarRtView::boxes: the transformed boxes of its listed
// meshes, car_rt_instances_rules.h); the boxes, not one box round them all, are the region (RegionRect: the hull of their pixel rectangles, each
// grown by the reach and extruded toward the sun; a car wholly behind the camera is left out). The shader traces a pixel only if it is within the
// reach of a box (the occlusion rays) or its sun ray meets one, and the rays hit the player's body and tyres and every part of the other cars
// (kRayPartsWithOthers, 0x15), so the road between two cars is darkened by both and a car's shadow falls on the other. A thread outside the rectangle
// writes "open" and ends, so the image is whole every frame and the consumers read it with no rectangle of their own.
//
// COST (an estimate, not measured: CHECK on the first run; the pass is timed as "ao rt" by PassTiming): a chase view has the car and its
// surroundings in about a tenth to a fifth of the picture; at the occlusion's traced size (1080 lines at 1080p and at 4K, the half size) that is
// 200,000 to 400,000 pixels, each with at most 4 occlusion rays, 1 sun ray and, for the car's own pixels, 1 primary ray: 1 to 2 million rays of at
// most 0.6 m (and the sun rays of up to 8 m against a tree of about 25 instances), which the refl pass's estimate (3 to 5 G rays/s on an RTX 5080
// class card) puts at 0.2 to 0.5 ms; the image write of the whole traced size is about 0.02 ms. High (2 rays) is a third less. Memory: one R8G8B8A8
// image of the traced size (8 MiB at 1080 lines), given back after 2 s without the pass. See perf/RT-AO-DESIGN.md.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipeline is made ahead on the host builder thread where the
// occlusion or the contact shadows are on at the start (host_precache.h), and in the background where one is switched on later; the pass is not
// recorded until it is made (a frame never waits for a compile).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_rt_ao_rules.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class CarFrame;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;

class CarRtAo {
 public:
  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

  CarRtAo() = default;
  ~CarRtAo();
  CarRtAo(const CarRtAo&) = delete;
  CarRtAo& operator=(const CarRtAo&) = delete;

  // After the scene's own Initialize (its descriptor set exists) and before the host precache starts: the device's answer (CarRtSupported), the
  // layout, the shader and, where the occlusion or the contact shadows are on at the start (`listed_at_start`), the pipeline's place in the
  // precache. Never fails the renderer: without them nothing is traced. `scene` and `car_frame` may be null (nothing is traced).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* scene,
                  const CarFrame* car_frame, bool listed_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  // The device can trace the car and the pass's layout and shader exist.
  bool available() const { return ready_; }

  // The frame's start (AmbientOcclusion::BeginFrame): reads the setting. `occlusion_on` and `contact_on` are the effects' levels in this frame
  // (not off, their pipelines built). When the pass is wanted and can run, asks the scene for this frame's structures.
  void BeginFrame(bool occlusion_on, bool contact_on);
  // True while the pass is wanted this frame (the occlusion's passes then record it in Run, if the scene has a top level).
  bool wanted() const { return wanted_; }

  // What the occlusion's passes tell of this frame (AmbientOcclusion::Run).
  struct Inputs {
    const HostImage* depth = nullptr;  // the occlusion's level 0 linear depth (R32_FLOAT, the vehicle bit in its lowest bit), left readable
    float tan_x = 0.0f;                // tan of the half field of view it was made with
    float tan_y = 0.0f;
    float fade_end = 160.0f;           // the distance beyond which nothing is computed
    bool occlusion = false;            // the occlusion term is wanted in this frame (the plain visibility pass runs)
    bool contact = false;              // the contact shadows run in this frame (not gated off by a low sun or rain)
    bool ultra = false;                // the occlusion's level: 4 rays against 2
    float to_sun[3] = {0.0f, 0.0f, 0.0f};  // the direction to the sun in view space (unit; zero: none)
    uint32_t noise_code = 0;           // what the main pass is given in slots2.y (ao_stable.h NoiseCode)
    uint32_t sun_frame = 0;            // the frame the sun ray's spread is shifted by (0: a fixed pattern)
    uint64_t frame = 0;                // for the log
  };
  // What a recorded pass gives the consumers.
  struct Result {
    bool recorded = false;
    uint32_t slot = kNoSlot;  // the image's bindless slot (a recorded frame)
    bool occlusion = false;   // the image's r channel holds a traced term
    bool contact = false;     // ... its g channel
  };
  // Records the pass on `list` (outside any render pass, after the depth pass and before the passes that read the image; the structures built
  // before it in the same list). Nothing recorded (the scene has no top level this frame, no car, the car is off the picture, the setting is
  // off, the pipeline is not made): `recorded` false and the consumers' flags stay off.
  Result Record(plume::RenderCommandList* list, const Inputs& inputs);

  // The image goes back (the pass has not run for a while).
  void ReleaseImage();
  uint64_t HeldBytes() const;
  // The text AmbientOcclusion's census line ends with; empty where there is no ray tracing.
  std::string CensusText() const;

 private:
  // The push block of the pass (hlsl/car_rt_ao_cs.hlsl CarRtAoPush), 112 bytes.
  struct Push {
    uint32_t slots[4];  // the depth image's slot, the image's width and height, the flags (car_rt_ao_rules.h kPush*: the rays a pixel in bits 8-11, the other cars' boxes 12-15)
    uint32_t rect[4];   // the pixel rectangle that is looked at: x0, y0, x1, y1
    uint32_t noise[4];  // the noise code, the parts the rays hit, the sun ray's frame, the first row of the other cars' boxes in the car RT table
    float lens[4];      // tan half field of view x and y, the occlusion rays' reach, the distance beyond which nothing is traced
    float light[4];     // the direction to the sun in view space, the tangent of the sun disc's radius
    float box_min[4];   // the car's box in view space, the sun ray's reach
    float box_max[4];   // the car's box in view space, reserved
  };
  static_assert(sizeof(Push) == 112);

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
  void RetireSet();
  void LogState(const car_rt_ao::Terms& terms, const Inputs& inputs, const car_rt_ao::Rect& rect, uint32_t rays, float reach, uint32_t instances,
                const car_rt_ao::Box& box, uint32_t other_boxes);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* scene_ = nullptr;
  const CarFrame* car_frame_ = nullptr;

  bool supported_ = false;  // the device can trace the car (CarRtSupported)
  bool ready_ = false;      // layout and shader exist
  bool listed_ = false;     // the pipeline was listed (at the start, or in the background after it)
  bool pipeline_failed_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;  // u0 the image
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  HostImage image_;  // R8G8B8A8_UNORM, the occlusion's traced size
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  const plume::RenderTexture* set_texture_ = nullptr;

  // The frame.
  bool wanted_ = false;
  car_rt_ao::Setting setting_ = car_rt_ao::Setting::kAuto;
  uint32_t idle_frames_ = 0;

  // The log and the totals.
  bool first_logged_ = false;
  bool no_scene_logged_ = false;
  bool no_box_logged_ = false;
  uint32_t last_state_ = ~0u;  // the terms and the rays the log last said
  uint64_t passes_run_ = 0;
  uint64_t rays_estimated_ = 0;
  uint64_t no_scene_ = 0;   // frames the pass was asked for but the scene had no structures
  uint64_t no_box_ = 0;     // ... the car's box was not known or off the picture
  uint64_t not_ready_ = 0;  // ... the pipeline was still being made
  uint64_t rect_pixels_ = 0;  // the rectangles' pixels, summed over the passes
  uint32_t image_for_[2] = {};
};

}  // namespace mcla::gpu
