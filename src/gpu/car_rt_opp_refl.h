#pragma once
// ===========================================================================
// MCLA GPU renderer -- ray traced reflections on the opponents' cars
// ===========================================================================
// The ray traced reflection pass (car_rt_refl.h) shades the player's car: at the history trigger one compute pass casts a primary ray at every vehicle texel of the history
// image, finds the player's body, and casts a cone of reflection rays from it against the car's top level (car_rt_scene.h); the image it writes is read by the NEXT frame's
// paint draws (the paint variant, version 6) in place of the screen-space march. The cars driving beside the player are in the same top level (the selection by stencil ID and distance
// from the player's vehicle, the class's cap: High 4 cars within 14 m, Mid 2 within 10 m, none on Low) and show in the player's paint, but THEIR paint took the march, because the
// primary ray never finds another car's paint (mask 0x10 is the reflection ray's, not the primary's).
//
// This class is the same pass for them: a second dispatch of the same shader file (hlsl/car_rt_refl_cs.hlsl, built with CAR_RT_REFL_OPPONENTS=1 into car_rt_refl_opp_cs: the player's main
// renamed away, the opponents' in hlsl/car_rt_refl_opp.hlsli), run by CarRtRefl::OnHistory right after the player's dispatch and before the denoise, into the same two images (the
// image the paint reads, and the denoise's aux image). Everything about it is in car_rt_opp_refl_rules.h (which pixels, each pixel's surface, the rays, the colour of a hit the history cannot show,
// the blending: the paint variant's own, unchanged, the budget); this file is the host side:
//
//   - the pipeline: a layout of its own with the player's three sets (the history's bindless set, the two storage images, the scene's) and a push block of 96 bytes (the player's 80
//     and `opp`: x, y the first texel of the rectangle, z the row of the player's record in the instances' table, w the rays a texel casts). The pipeline is made ahead on the host
//     builder thread where the reflection is on at the start (host_precache.h key (kCarRtRefl, 2)), in the background where it is switched on later, and is optional: a frame whose pipeline
//     is not ready goes on without it (the opponents' paint takes the march), it is never waited for.
//   - the frame: the rectangles of the other cars' boxes (CarRtView::boxes, the scene's view-space boxes) projected into the image with the history's lens (BoxRect), the budget's plan
//     (PlanDispatches), a UAV barrier on the two images (the player's dispatch wrote them; a texel the opponents' dispatch finds is overwritten), one dispatch per merged rectangle with a barrier
//     between. No dispatch when no instance of another car's paint is in the list (CarRtView::other_paint_instances): traffic, cars not drawn this frame and a frame with no other car cost nothing.
//
// SETTINGS (all debug-only; the player's own are mcla_gpu_car_selfrefl_rt, the RAY TRACED row of the REFLECTIONS page, and the car self-reflection's):
//   mcla_gpu_car_rt_opp_refl          true (the default): on wherever the car ray tracing runs and other cars are in the scene. false: the opponents' paint takes the march, as before. Live.
//   mcla_gpu_car_rt_opp_refl_texels   0 (the default): the class's budget (500k texels High, 200k Mid, in thousands here); N: that many thousand. Live.
//   mcla_gpu_car_rt_opp_refl_rays     0 (the default): by the budget (4, 2 or 1 rays a texel); 1, 2 or 4: forced. Live.
//
// LOG: "[car rt refl] opponents" lines: the first dispatch (cars, rectangles, rays), then a window every 300 passes: dispatches, cars admitted, texels counted and covered, frames at each
// number of rays, cars over the budget, the passes with no pipeline or no paint instance. The pass time is the PassTiming row "car rt refl opp" (mcla_gpu_pass_timing).
//
// CHECK (the first run's list; car_rt_opp_refl_rules.h and the check script in h11-work/rt-opponent-refl have the numbers):
//   1. the pipeline's layout (the player's sets and a 96-byte push block) on D3D12 and Vulkan, validation layers: the barriers between the two dispatches (image and aux GENERAL -> GENERAL) and
//      the descriptor sets bound again after the layout change.
//   2. the cost: "car rt refl opp" against the budget's estimate (500k texels High about 0.5 to 0.9 ms on a 5080 class card); the mid class's 200k.
//   3. the paint of an opponent in the picture (debug view 5: cyan = no data, so an opponent's paint that shows yellow, orange or green has the image); the door beside a bright car or a wall.
//   4. the player's car in an opponent's reflection (the flank facing the opponent is not seen by the chase camera: the record's colour).
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_rt_opp_refl_rules.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
struct CarRtView;
class Device;
class PassTiming;

class CarRtOppRefl {
 public:
  CarRtOppRefl() = default;
  ~CarRtOppRefl();
  CarRtOppRefl(const CarRtOppRefl&) = delete;
  CarRtOppRefl& operator=(const CarRtOppRefl&) = delete;

  // After CarRtRefl made its layout and shaders (CarRtRefl::Initialize): the player's output set description, shared. `list_at_start`: the pipeline is listed with the start's precache
  // (the car self-reflections are on at the start and the ray traced setting is not off), else it is made in the background when first wanted. Never fails the renderer: without
  // the shader or the layout the pass does nothing.
  void Initialize(Device& device, HostPasses& passes, BindlessHeap& heap, PassTiming& pass_timing, CarRtScene& scene,
                  const plume::RenderDescriptorSetBuilder& output_builder, bool list_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool ready() const { return ready_; }

  // What the player's dispatch used and wrote (CarRtRefl::OnHistory).
  struct Frame {
    plume::RenderCommandList* list = nullptr;
    const CarRtView* view = nullptr;           // this frame's top level and the boxes
    const void* base_push = nullptr;           // the player's dispatch's push block (CarRtRefl::Push), copied
    uint32_t base_push_bytes = 0;
    plume::RenderDescriptorSet* output_set = nullptr;  // u0 the image, u1 the aux image: the player's trace set
    HostImage* image = nullptr;                // both are in GENERAL, written by the player's dispatch
    HostImage* aux = nullptr;
    uint32_t image_width = 0;                  // the history image's size in texels
    uint32_t image_height = 0;
    uint32_t render_width = 0;                 // the picture's
    uint32_t render_height = 0;
    uint32_t divisor = 1;
    double tan_x = 0.0;                        // the history's lens
    double tan_y = 0.0;
    uint64_t frame = 0;
  };
  // Records the opponents' dispatches after the player's. True when any was recorded (the images are in GENERAL still).
  bool Record(const Frame& frame);

  // The text CarRtRefl's census line takes; empty where nothing ran.
  std::string CensusText();

 private:
  // The push block (hlsl/car_rt_refl_cs.hlsl CarRtReflPush with CAR_RT_REFL_OPPONENTS): the player's 80 bytes (CarRtRefl::Push) and opp.
  struct Push {
    uint32_t slots[4];
    float image[4];
    float lens[4];
    float tune[4];
    float up[4];
    uint32_t opp[4];
  };
  static_assert(sizeof(Push) == 96);
  static constexpr uint32_t kBasePushBytes = 80;  // CarRtRefl::Push, checked at run time (the type is private to it)
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kPushSpace = 3;

  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  plume::RenderPipeline* Pipeline();

  // One window of the log.
  struct Window {
    uint64_t first_frame = 0;
    uint32_t passes = 0;
    uint64_t dispatches = 0, cars = 0, admitted = 0, skipped = 0, counted = 0, covered = 0;
    uint64_t most_covered = 0;
    uint32_t frames_at_rays[3] = {};  // 4, 2, 1
    uint32_t no_paint = 0, no_rect = 0, not_ready = 0, off = 0;
  };

  Device* device_ = nullptr;
  HostPasses* passes_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;
  bool listed_ = false;
  bool pipeline_failed_ = false;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  Window window_;
  bool first_logged_ = false;
  bool size_logged_ = false;
  uint64_t passes_run_ = 0;
  uint64_t dispatches_run_ = 0;
  uint64_t census_passes_ = 0;
};

}  // namespace mcla::gpu
