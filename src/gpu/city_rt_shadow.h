#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's ray traced sun shadows (CITY-RT M1)
// ===========================================================================
// One compute pass inside the occlusion's passes (ao.cpp Run, next to car_rt_ao.h): from every pixel of the occlusion's level 0 linear depth image a few rays go toward
// the sun, spread over the sun's disc, against this frame's top level structure, which holds the city's sectors (city_rt_scene.h) beside the cars (car_rt_scene.h). The share
// that arrives is the g channel of an image in the layout car_rt_ao writes, and the contact march takes it as it takes the car's: the darker of its own shadow and the traced
// one, on the sun's share of the light only (ao.cpp rt_contact_slot_, hlsl/ao_contact_ps.hlsl THE RAY TRACED TERM). city_rt_shadow_rules.h has the maths and why the look is
// right; hlsl/city_rt_shadow_cs.hlsl the pass.
//
// THE SETTING. mcla_gpu_rt_shadows (auto, off, high, ultra; live; the pause menu row RT SHADOWS on GRAPHICS > LIGHTING AND SHADOWS): auto is high on the
// High class and off elsewhere (gpu_class.h), high or ultra by hand on any class with ray queries, never on Low. The level sets the class budget of the structures
// (city_rt_rules.h BudgetFor) and the rays a pixel (2 or 4). It works where the contact march does (CONTACT SHADOWS on, which auto is on High): the march is the consumer.
// The debug cvars: mcla_gpu_city_rt_sun (the sun's disc, times the real one; 3), _rays, _alpha (exact, stochastic, opaque, exclude), _alpha_ref (the exact mode's reference), _alpha_view (the
// exact mode's picture and counters), _radius (the hi detail ring in metres), _range (the most a shadow is applied to, metres), _check (the structures' check: primary rays against the
// depth, statistics in the log), _view (the check's picture as the contact shadow view 7: white within 1 percent, grey within 5, black else).
//
// THE CUT-OUTS (CITY-RT M1c). The fences, grates and neon cut-outs are the pack's non opaque triangles. In the exact mode, the default, a ray that meets one reads its alpha: the candidate's
// texture coordinates come from the hit data's corner streams (city_hit_scene.h, the pool at t6..t9 of the ray tracing set) and its diffuse texture's alpha from the city texture set
// (city_texture_set.h: the cut-outs' textures of the sectors in the ring, resident through the bindless heap, nearest first), at the level the ray's footprint calls for and not below the
// lowest that keeps a cut-out's shape (hlsl/city_hit_alpha.hlsli, city_hit_alpha_rules.h); the ray is blocked where the alpha reaches the reference. This pass asks the scene service for the
// hit data and the cut-out textures (Needs::hit, Needs::textures) in that mode only. A candidate whose alpha cannot be read (no hit file, no texture file, the texture not resident yet)
// follows the stochastic rule, as before: nothing waits and nothing is wrong while a texture loads. The debug view (mcla_gpu_city_rt_alpha_view) shows which way each pixel went.
//
// WHAT IT ASKS FOR. BeginFrame asks CityRtScene (the structures) and CarRtScene (the top level, which the city's instances join) for this frame; the structures are built at
// the history trigger before the occlusion's passes (scene.cpp OnResolve calls CarRtScene::OnResolve first), so Record finds them. A frame without them (the car is not found,
// the pack is not built, nothing is built yet, the sun is low or gone) records nothing and the screen-space contact shadows run as they were.
//
// WHERE IT APPLIES. Within `FadeRange()` of the camera: the structures' hi detail radius, the game's own hi detail radius (300 m times lod_city_scale) and the distance of the
// nearest sector whose hi structure is not built yet (city_rt_rules.h SafeReceiverRange). Beyond it the pass writes 1 (the game's own shadows stand). ao.cpp lets the contact
// term reach as far as that (its fade is 40 to 80 m without the traced sun term).
//
// THE COST (CHECK: measure; estimated 0.2 to 0.5 ms at 4K on an RTX 5080 class card for 2 rays a pixel at the occlusion's half size): pass timing label "city rt shadow",
// and "city rt build" for the structures. Memory: the image is R8G8B8A8_UNORM at the occlusion's traced size (8 MiB at 1080 lines), given back after 2 s without the pass.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "city_rt_rules.h"
#include "city_rt_shadow_rules.h"
#include "host_passes.h"
#include "rt_denoise.h"

namespace mcla::gpu {

class BindlessHeap;
class CarFrame;
class CarRtScene;
class CityRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;

class CityRtShadow {
 public:
  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

  CityRtShadow() = default;
  ~CityRtShadow();
  CityRtShadow(const CityRtShadow&) = delete;
  CityRtShadow& operator=(const CityRtShadow&) = delete;

  // After the car scene's own Initialize (its descriptor set exists) and before the host precache starts. Never fails the renderer: without the device's ray queries nothing is
  // traced. `listed_at_start`: the setting is not off at the start, so the pipeline goes in the precache; else it is made in the background when first wanted.
  // `service` (rt_scene_service.h): the pass is a CONSUMER of the ray tracing scene, it asks for the city's structures (and the cars') there instead of driving the scenes itself.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* car_scene, CityRtScene* city_scene,
                  RtSceneService* service, bool listed_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // The frame's start (AmbientOcclusion::BeginFrame). `contact_on`: the contact march runs this frame (the consumer). Reads the setting, tells the city scene, asks the car scene for
  // the top level. Always called (the structures are given back after a while without a request).
  void BeginFrame(bool contact_on);
  bool wanted() const { return wanted_; }
  // The setting's level this frame.
  city_rt::Level level() const { return level_; }
  // How far from the camera the contact term reaches while this pass is on (metres; 0: the contact shadows' own). The last frame's: the plan is made before the pass runs.
  float FadeRange() const { return wanted_ ? fade_range_ : 0.0f; }

  struct Inputs {
    const HostImage* depth = nullptr;  // the occlusion's level 0 linear depth (R32_FLOAT, the vehicle bit in its lowest bit), left readable
    float tan_x = 0.0f;                // tan of the half field of view it was made with
    float tan_y = 0.0f;
    bool ultra = false;                // the occlusion's level (unused: the setting's decides)
    float to_sun[3] = {0.0f, 0.0f, 0.0f};  // the direction to the sun in view space (unit; zero: none)
    uint32_t sun_frame = 0;            // the frame the rays' noise is shifted by (0: a fixed pattern) in the low 10 bits, bit 29 the blue noise tile (ao_stable.h kBlueNoiseFlag)
    uint32_t car_slot = kNoSlot;       // the car ray traced image (car_rt_ao.h) of this frame: the result is the darker of the two where it exists
    uint64_t frame = 0;
    RtDenoise::VisibilityFrame denoise;
  };
  struct Result {
    bool recorded = false;
    float receiver_range = 0.0f;
    uint32_t slot = kNoSlot;  // the image's bindless slot (a recorded frame): its g channel is the sun's open share
  };
  // Records the pass on `list` (outside any render pass, after the depth chain and the car's ray traced pass, before the passes that read the image).
  Result Record(plume::RenderCommandList* list, const Inputs& inputs);

  void ReleaseImage();
  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  // The push block (hlsl/city_rt_shadow_cs.hlsl CityRtShadowPush), 96 bytes.
  struct Push {
    uint32_t slots[4];  // the depth image's slot, its width and height, the flags (city_rt_shadow_rules.h kFlag*)
    uint32_t noise[4];  // the noise's shift, the ray mask, the frame serial, the car image's slot
    float lens[4];      // tan half field of view x and y, the rays' length, the range
    float bounds_min[4];
    float bounds_max[4];
    float light[4];     // the direction to the sun in view space, the tangent of the sun disc's radius
  };
  static_assert(sizeof(Push) == 96);
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kPushSpace = 3;
  static constexpr uint64_t kCounterBytes = uint64_t(city_rt_shadow::kStatWords) * 4;

  bool MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  plume::RenderPipeline* Pipeline();
  bool PipelineReady();
  bool EnsureOutput(uint32_t width, uint32_t height);
  void RetireSet();
  void ProcessReadback(uint64_t frame, uint64_t generation, uint32_t width, uint32_t height, uint32_t instances, bool check, bool alpha_view);

  Device* device_ = nullptr;
  RtDenoise denoise_;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* car_scene_ = nullptr;
  CityRtScene* city_scene_ = nullptr;
  RtSceneService* service_ = nullptr;

  bool ready_ = false;
  bool listed_ = false;
  bool pipeline_failed_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;  // u0 the image, u1 the check's counters
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  HostImage image_;  // R8G8B8A8_UNORM, the occlusion's traced size
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  const plume::RenderTexture* set_texture_ = nullptr;
  std::unique_ptr<plume::RenderBuffer> counters_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  bool readback_busy_ = false;
  uint64_t next_readback_ = 0;
  uint32_t last_words_[city_rt_shadow::kStatWords] = {};
  bool have_last_words_ = false;
  uint64_t generation_ = 0;

  // The frame.
  bool wanted_ = false;
  city_rt::Level level_ = city_rt::Level::kOff;
  float fade_range_ = 0.0f;
  uint32_t idle_frames_ = 0;

  // The log and the totals.
  bool first_logged_ = false;
  bool no_scene_logged_ = false;
  uint32_t last_state_ = ~0u;
  uint64_t passes_run_ = 0;
  uint64_t no_scene_ = 0;    // frames the pass was asked for but the top level had no city in it
  uint64_t no_range_ = 0;    // ... no receiver was within the safe range (nothing built near the camera yet)
  uint64_t no_sun_ = 0;      // ... the sun was gone or too low
  uint64_t not_ready_ = 0;   // ... the pipeline was still being made
  uint64_t rays_estimated_ = 0;
  uint32_t image_for_[2] = {};
};

}  // namespace mcla::gpu
