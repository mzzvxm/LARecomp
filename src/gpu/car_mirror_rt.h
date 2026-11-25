#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's side mirrors show the real view behind, ray traced: the host side (MIRROR-RT)
// ===========================================================================
// car_mirror_rt_rules.h has the design, the numbers and the tests (with car_mirror_patch_rules.h: how the mirror glass is found among the car's draws); hlsl/car_mirror_rt_cs.hlsl and
// hlsl/car_mirror_apply_ps.hlsl are the shaders. In one picture: the game's side mirrors reflect its environment map (a sky made around the camera), which is not the street behind. At the car ray
// tracing's trigger (the game's mid-scene depth copy, right after CarRtScene built this frame's top level) this pass traces a mirror ray from every pixel of the two mirror glasses of the player's car
// through the cars' structures (the car's own flank and rear, the cars beside it, shaded from their materials) and the world's colour where it hits nothing, and draws the result over the game's glass
// in the main colour.
//
// THE CITY (MIRROR-CITY, car_mirror_city_rules.h, perf/MIRROR-CITY.md): where the mirror's world is the city (mcla_gpu_car_mirror_rt_world: auto follows RT CITY REFLECTIONS, city, env), the mirror ray goes on
// into the city's triangles in the same top level (the city line's structures and hit data, the scene service's wants) and a hit on them is shaded by the city hit library with the world frame's light and
// lamps: the street, the buildings, the lamps behind the car, what the environment map only blurred. A hit with no data is a miss (the environment stays); the pass shows the environment until the city's data stands.
// A mirror of a few pixels (a chase camera) shows the environment and asks for nothing: the city is traced and kept for a pass whose larger mirror covers kCityMinPixels (car_mirror_city_rules.h).
//
//   TRACE  one compute dispatch for each mirror over the rectangle its box covers on the picture, into one image of kImageWidth x kImageHeight (R16G16B16A16_FLOAT, 4 MiB, both rectangles side by side).
//   APPLY  one draw for each mirror into the main colour, scissored to the rectangle, blended SRC_ALPHA / INV_SRC_ALPHA on the colour channels (one pipeline per sample count of the target:
//          1, 2, 4, 8), the colour target moved to a colour attachment for it and left there (the game's next draw moves things back itself, as the occlusion's apply does).
//
// ONLY WHERE IT SHOWS: nothing is recorded (and nothing is built beyond what the car's other effects build) in a frame with no mirror pair in the car's draws (CarRtView::mirror), a mirror off the
// picture, too small or over the cap (Limits), the pause or garage view, or a pipeline not made yet; the frame goes on with the game's mirror and never waits for a compile.
//
// SETTINGS (mcla_gpu_car_mirror_rt, live): auto (the default: on where the device can ray trace the car and it is a strong card, the High class), off, on (any card that can ray trace the car and is not of the Low class; the Mid
// class's cap). The player's row is RT SIDE MIRRORS on GRAPHICS > REFLECTIONS (car_mirror_setting.h). BeginFrame asks CarRtScene for the car's structures in every frame the pass is wanted (the car self-reflections ask for them anyway where they are on).
//   mcla_gpu_car_mirror_rt_pixels (debug): the most pixels one mirror's rectangle may cover, in thousands (0: the class's cell).
//   mcla_gpu_car_mirror_rt_strength (debug): the share of the game's glass the traced picture replaces (0.92).
//   mcla_gpu_car_mirror_rt_world (debug, MIRROR-CITY): what the mirror ray shows where it finds none of the cars: auto (the default: the city where the player's RT CITY REFLECTIONS row comes to on,
//   which its AUTO does on a strong ray tracing card), env (the game's environment map: the mirror as it was), city (the city on any card that can trace, whatever that row says). The city is the same one the paint's reflections use: the scene service
//   (rt_scene_service.h, Consumer::kCityMirror) gets the wants for its structures, hit data and textures, and the mirror shows the environment until the data stands (car_mirror_city_rules.h).
//
// PIPELINES: the trace's and the one-, two- and four-sample applies' are listed with the start's precache (host_precache.h kCarMirrorRt: 0 the trace, 1 to 4 the applies of 1, 2, 4, 8 samples) where the
// pass is wanted at the start, made in the background where it is wanted later (AddLive).
//
// LOG: "[car mirror]" lines: the first pass (the rectangles, the pair), a window every 300 passes (mirrors drawn, pixels, frames not run by reason). The times are the PassTiming rows
// "car mirror rt" and "car mirror apply".
//
// CHECK (nothing here was run: no game, no build): the layout (the bindless set, one storage image, the car's set, an 80-byte push block) on D3D12 and Vulkan with the validation layers; the image's
// layouts (GENERAL for the trace, SHADER_READ for the apply) and the barrier between them; the apply's pipelines on a multisampled main colour (2, 4, 8 host samples: one invocation a pixel writes
// every sample); that the main colour at the trigger already holds the game's mirror glass (the glass is a draw "after the world", before the trigger: the car dump says every draw of the car is in);
// that a mirror pair is found on a car other than the R34 (the first pair's log line); the units of the environment map against the colour target's (RtHitSample * 2^exp_bias, as the paint's
// hit shading); the look: the mirror's size, brightness and edges in the cockpit and bumper cameras, the shaded car in it, the see-through window; the cost.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_mirror_city_rules.h"
#include "car_mirror_rt_rules.h"
#include "edram.h"
#include "host_passes.h"
#include "resolve.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;

class CarMirrorRt {
 public:
  CarMirrorRt() = default;
  ~CarMirrorRt();
  CarMirrorRt(const CarMirrorRt&) = delete;
  CarMirrorRt& operator=(const CarMirrorRt&) = delete;

  // After CarRtScene's own Initialize and before the precache starts: the device's answer (the scene is ready only where CarRtSupported said yes), the layout and the shaders, and the pipelines'
  // places in the host precache where the pass is wanted at the start. Never fails the renderer: without them the pass does nothing.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, EdramTargets& edram, CarRtScene& scene);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool ready() const { return ready_; }

  // The scene service the city's wants go to (MIRROR-CITY; not owned, null: the mirror shows the environment map). Before the first frame.
  void SetService(RtSceneService* service) { service_ = service; }
  // The frame's start (Scene::BeginFrame): reads the settings; asks the scene for the car's structures when the pass is wanted, and the service for the city's where the mirror's world is the city.
  void BeginFrame();
  bool active() const { return active_; }
  // At the car ray tracing's trigger, after CarRtScene::OnResolve returned with TriggeredThisResolve(): records the trace and the apply on `list` (outside any render pass). `exp_bias` is the main
  // colour target's exponent bias as the car self-reflection last saw it. True when anything was recorded (the caller's list state is gone).
  bool OnResolve(const ResolveCache::Summary& summary, plume::RenderCommandList* list, int32_t exp_bias);
  // A guest present: the frame boundary.
  void OnPresent();
  uint64_t HeldBytes() const;

 private:
  // The trace's push block (hlsl/car_mirror_rt_cs.hlsl CarMirrorPush), 80 bytes.
  struct Push {
    uint32_t rect[4];   // x, y the first pixel of the rectangle, z, w its size
    uint32_t image[4];  // x, y the rectangle's first texel in the image, z the primary ray's parts, w the mirror ray's parts
    float lens[4];      // tan x, tan y, the picture's width and height
    float tune[4];      // the strength, the cars' reach, the fade's start, the units
    uint32_t city[4];   // MIRROR-CITY: x the flags (car_mirror_city_rules.h kFlag*), y the city's reach (m) and z the share of it up to which a hit weighs 1 (float bits), w reserved
  };
  static_assert(sizeof(Push) == car_mirror_city::kPushBytes);
  // The apply's push block (hlsl/car_mirror_apply_ps.hlsl MirrorApplyPush), 32 bytes.
  struct ApplyPush {
    uint32_t image[4];  // x the image's bindless slot, y, z the rectangle's first texel in the image
    uint32_t rect[4];   // x0, y0, x1, y1
  };
  static_assert(sizeof(ApplyPush) == 32);
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kPushSpace = 3;
  static constexpr uint32_t kApplyVariants = 4;  // the apply's pipelines: 1, 2, 4 and 8 samples

  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  std::unique_ptr<plume::RenderPipeline> MakeApplyPipeline(uint32_t index);
  plume::RenderPipeline* Pipeline();
  plume::RenderPipeline* ApplyPipeline(uint32_t index);
  bool EnsureImage();
  void RetireSet();
  void ReleaseImage();
  bool NotRun(car_mirror::Why why);
  void LogWindow();
  void UpdateWorld();

  // One window of the log.
  struct Window {
    uint64_t first_frame = 0;
    uint32_t passes = 0;
    uint64_t mirrors = 0;
    uint64_t pixels = 0;
    uint32_t city_passes = 0;  // of the passes, those that traced the city (the rest showed the environment map)
    uint64_t not_run[size_t(car_mirror::Why::kCount)] = {};
    uint64_t rect_skipped[size_t(car_mirror::Why::kCount)] = {};
  };

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  EdramTargets* edram_ = nullptr;
  CarRtScene* scene_ = nullptr;
  RtSceneService* service_ = nullptr;
  car_mirror_city::Decision world_;  // this frame's world of the mirror: the city, or why the environment map (BeginFrame)
  int last_world_state_ = -1;        // the decision the log last said
  bool city_seen_ = false;           // a pass whose larger mirror was big enough to show the city (car_mirror_city_rules.h BigEnough) has run ...
  uint64_t city_seen_frame_ = 0;     // ... last in this frame: the city's wants are kept for kCityHoldFrames after it
  bool ready_ = false;
  bool active_ = false;
  bool listed_ = false;
  bool pipeline_failed_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderShader> apply_shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kApplyVariants> apply_;
  std::array<bool, kApplyVariants> apply_listed_ = {};
  std::array<bool, kApplyVariants> apply_failed_ = {};

  HostImage image_;
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  const plume::RenderTexture* set_image_ = nullptr;

  uint64_t frame_ = 0;
  uint64_t next_log_frame_ = 1800;
  uint32_t idle_frames_ = 0;
  bool recorded_this_frame_ = false;
  Window window_;
  bool first_logged_ = false;
  bool pair_logged_ = false;
  uint64_t passes_run_ = 0;
};

}  // namespace mcla::gpu
