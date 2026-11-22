#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car windows reflecting the city, ray traced: the host side (CITY-RT M2 package R3b)
// ===========================================================================
// car_glass_rules.h has the design, the numbers and the arithmetic of the apply pass (and its tests); hlsl/car_rt_refl_glass.hlsli and hlsl/car_glass_apply_ps.hlsl the shaders. In one picture: the car's windows
// are drawn after the world and write no depth, so the picture under them is the interior; at the history trigger, after the screen-space reflections, this pass finds each pane with a primary ray against the
// player's glass instances alone (nearer than the picture's depth), casts one mirror ray through the cars' structures and the city's, and ADDS the colour it finds, by the glass's Fresnel, to the main colour.
//
//   TRACE  (this class, called by CarRtRefl::OnHistory right after the opponents' dispatch, into the same stats frame, before the denoise): the glass variant of car_rt_refl_cs.hlsl
//          (car_rt_refl_glass_cs: the same file with CAR_RT_REFL_GLASS, so the cone's TraceRay and the city hit library are the paint's) over the rectangle of the player's box in the history's grid, into an image
//          of its own (R16G16B16A16_FLOAT, the history's size: rgb the colour to add, alpha 1 for a pane). The layout is the opponents' (the history's bindless set, the outputs, the scene's, the city's rows,
//          the push block in space 4): the output set binds the glass image at u0, the player's aux image at u1 and the counters at u2, which this entry point does not write.
//   APPLY  (called by CarSelfRefl::RecordHistory after OnHistory, which has the main colour target): one full-screen additive draw into the main colour (car_glass_apply_ps: blend ONE ONE on the colour
//          channels, one pipeline per sample count of the target, 1 2 4 8, made in the background where they were not listed at the start). The history was made before it: it holds no glass reflection.
//
// THE PIPELINES: the trace is made ahead on the host builder thread where the ray traced reflections are on at the start (host_precache.h key (kCarRtRefl, 3)); the apply for one sample (kCarRtRefl, 4);
// the others (5 to 7: 2, 4 and 8 samples) are made in the background when a frame first asks. A frame whose pipelines are not ready goes without the pass (the glass stays the game's), never waits.
//
// SETTING (debug-only): mcla_gpu_city_refl_glass (true): CityRefl::glass_wanted(), live. The level serves the glass at high and ultra (city_refl_rules.h Serves).
//
// LOG: "[car glass]" lines: the first pass, a window every 300 passes (the rectangle's area, the frames not run by reason). The times are the PassTiming rows "car glass refl" and "car glass apply".
//
// CHECK (never run): the layout (the opponents' sets and a 112-byte push block) on D3D12 and Vulkan with the validation layers; the glass image's layouts (GENERAL for the trace, SHADER_READ for the apply) and
// the barrier between the player's dispatches and this one; the apply's pipelines on a multisampled main colour (2, 4, 8 host samples: one invocation a pixel writes every sample); that the glass of the
// player's car is mask 0x02 in the frames the car dump says (car_rt_masks 0: "drawn after the world's first draw" tells glass from the body only in a frame where some of the car's draws are and some are not);
// that the glass is already in the colour at the trigger (the picture shows the interior through the windows under the reflection); the cost.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_glass_rules.h"
#include "city_refl.h"
#include "edram.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
struct CarRtView;
class Device;
class FrameRing;
class PassTiming;

class CarRtGlassRefl {
 public:
  CarRtGlassRefl() = default;
  ~CarRtGlassRefl();
  CarRtGlassRefl(const CarRtGlassRefl&) = delete;
  CarRtGlassRefl& operator=(const CarRtGlassRefl&) = delete;

  // After CarRtRefl made its layout (CarRtRefl::Initialize): the player's output set description and the city's, shared. `list_at_start`: the trace's pipeline and the one-sample apply's are listed with
  // the start's precache. Never fails the renderer: without the shaders or the layout the pass does nothing.
  void Initialize(Device& device, HostPasses& passes, BindlessHeap& heap, FrameRing& frames, PassTiming& pass_timing, CarRtScene& scene, const plume::RenderDescriptorSetBuilder& output_builder,
                  const plume::RenderDescriptorSetBuilder& city_builder, bool list_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool ready() const { return ready_; }
  // The frame's start (CarRtRefl::BeginFrame): the last frame's trace is not this one's.
  void NewFrame() { traced_ = false; }

  // What the player's dispatch used and wrote (CarRtRefl::OnHistory).
  struct Frame {
    plume::RenderCommandList* list = nullptr;
    const CarRtView* view = nullptr;           // this frame's top level and the player's box
    const void* base_push = nullptr;           // the player's dispatch's push block (CarRtRefl::Push), copied
    uint32_t base_push_bytes = 0;
    const HostImage* aux = nullptr;            // the player's aux image (bound at u1, not written)
    const plume::RenderBuffer* stats = nullptr;  // the player's counters (bound at u2, not written: the stats flag is cleared)
    const CityRefl::Frame* city = nullptr;     // the city's set (bound at set 3) and cell this frame; null: no pass
    bool wanted = false;                       // CityRefl::glass_wanted()
    uint32_t colour_samples = 1;               // the main colour target's host samples (the apply's pipeline)
    uint32_t image_width = 0;                  // the history image's size in texels
    uint32_t image_height = 0;
    uint32_t render_width = 0;                 // the picture's
    uint32_t render_height = 0;
    uint32_t divisor = 1;
    double tan_x = 0.0;                        // the history's lens
    double tan_y = 0.0;
    uint64_t frame = 0;
  };
  // Records the trace on `f.list`. True when recorded (the image holds this frame's term and Apply may draw it). False: not wanted, nothing to trace, or a pipeline is not ready (counted).
  bool Trace(const Frame& f);

  // The additive draw into the main colour, after Trace recorded this frame (false otherwise). `colour` is the main view's colour target at the depth's samples and scale; the viewport is the picture's.
  bool Apply(plume::RenderCommandList* list, EdramTargets& edram, EdramTargets::Target& colour, uint32_t render_width, uint32_t render_height);

  // The image goes back (the history's went, or the pass was off for a while).
  void ReleaseImage();
  uint64_t HeldBytes() const;
  // The text CarRtRefl's census line takes; empty where nothing ran.
  std::string CensusText();

 private:
  // The push block (hlsl/car_rt_refl_cs.hlsl CarRtReflPush with CAR_RT_REFL_GLASS): the player's 96 bytes (CarRtRefl::Push) and glass.
  struct Push {
    uint32_t slots[4];
    float image[4];
    float lens[4];
    float tune[4];
    float up[4];
    float coat[4];
    uint32_t glass[4];  // x, y the first texel of the rectangle, z the strength (float bits), w reserved
  };
  static_assert(sizeof(Push) == 112);
  static constexpr uint32_t kBasePushBytes = 96;
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kCitySet = CityRefl::kSetIndex;
  static constexpr uint32_t kPushSpace = CityRefl::kPushSpace;
  static constexpr uint32_t kApplyVariants = 4;  // the apply's pipelines: 1, 2, 4 and 8 samples

  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  std::unique_ptr<plume::RenderPipeline> MakeApplyPipeline(uint32_t index);
  plume::RenderPipeline* Pipeline();
  plume::RenderPipeline* ApplyPipeline(uint32_t index);
  bool EnsureImage(uint32_t width, uint32_t height);
  bool EnsureSet(const HostImage& aux, const plume::RenderBuffer* stats);
  void RetireSet();
  void NoteNotRun(car_glass::Why why, uint64_t frame);
  void LogWindow(uint64_t frame);

  Device* device_ = nullptr;
  HostPasses* passes_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  FrameRing* frames_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  bool ready_ = false;
  bool listed_ = false;
  bool pipeline_failed_ = false;
  const plume::RenderDescriptorSetBuilder* output_builder_ = nullptr;  // the player's (u0 the image, u1 the aux image, u2 the counters): CarRtRefl's, which outlives this object
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
  const plume::RenderTexture* set_aux_ = nullptr;

  // The frame the trace recorded last and what the apply draws from it.
  bool traced_ = false;
  uint64_t traced_frame_ = 0;
  car_rt_opp_refl::TexelRect rect_;  // the texels the trace wrote
  uint32_t divisor_ = 1;             // the picture over the image, 1 or 2

  car_glass::Window window_;
  uint32_t idle_frames_ = 0;
  bool first_logged_ = false;
  uint64_t passes_run_ = 0;
  uint64_t applies_run_ = 0;
  uint64_t census_passes_ = 0;
};

}  // namespace mcla::gpu
