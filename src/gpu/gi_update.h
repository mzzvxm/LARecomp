#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe update (CITY-RT M3, package G2 of perf/CITY-RT-M3.md sections 3.1 to 3.3, 6, 7)
// ===========================================================================
// G1 made the field (gi_field.h) and gave it the seams; this is what fills it with light. gi_update_rules.h is the specification of everything that needs no device (the ray set, the classification, the
// blend, the scheduler, the governor, the budgets, the census), hlsl/gi_update_common.hlsli its twin, hlsl/gi_update_trace_cs.hlsl and hlsl/gi_update_blend_cs.hlsl the passes. This file is the Plume side:
//
//   WHEN      GiIntegration::OnResolve calls Record between GiField::RecordMaintenance and RecordFinish (gi_field.h THE FRAME, step 3), at the mid-scene depth copy, outside any render pass, in the list the top
//             level of the frame was built in (CarRtScene::OnResolve and RtWorldFrame::OnTrigger have run: scene.cpp), and only where the hit data's pool stands (CityHitScene::ready(), a block committed): a
//             hit with no data would be shaded black and the field would fade in as a field of zeros that has replaced the screen-space bounce light. GiIntegration::BeginFrame calls OnWant (before the field's own BeginFrame): the scene service
//             is asked for the city's structures and its hit data at the setting's level, which is how the safe range the field needs exists at all.
//   WHAT      a PLAN (the scheduler: the field's dirty probes, the stale ones, the round robin; gi_update_rules.h Scheduler), made into chunks of the layout's probes a frame (the ray buffer's capacity: G1's
//             update_reserve exactly; the warm-up after a cut is up to four chunks). For each chunk two compute dispatches: TRACE (one thread a ray: the closest hit in the city, shaded in the city hit
//             library's GI mode, one ray record each) and BLEND (one 10 x 10 group a probe: classification, integration, the blend with adaptive hysteresis, the tile, the state word). The field's three textures
//             are sampled by the trace (the field one frame old is the hit's second bounce) and written as storage images by the blend: a transition between the two (GENERAL <-> SHADER_READ) for each chunk.
//   GOVERNOR  two timestamps around the update (a pool a frame slot, read back when the slot comes round: the GpuTiming rule) feed gi_update_rules.h Governor, which scales the later cascades' share of the
//             budget between 0.5 and 1.5 against the layout's cap. Without timestamps the scale stays 1.
//   CENSUS    the passes add to a counters buffer (never cleared); every kReadbackEveryFrames frames it is copied to a readback buffer and the difference is printed as the '[rt gi]' lines the chain's report
//             reads (perf/city-rt-m3/scenes/README.md). The pass-timing labels are `rt gi trace` and `rt gi blend` (mcla_gpu_pass_timing).
//
// IDENTITY. Nothing is recorded, and the field's frame is published exactly as before, while any of these holds: the device or the layout could not be made (then SetUpdate is never given and the setting's row
// says NOT READY), the field is not usable this frame (the world is not the play view's, or the safe range is 0), the frame has no top level or no world rows (the car scene's builder did not run, the
// light is not known yet), the pipelines are still being made, or a resource could not be made. A frame that records nothing leaves every probe as it was.
//
// MEMORY. The ray buffer is the layout's rays a frame x 16 bytes (G1's update_reserve: 7.0 MiB at HIGH on a strong card), the probe list a uint a probe of four chunks (115 KiB a slot at HIGH), 512 bytes of rows
// a slot, 256 bytes of counters. HeldBytes() is what vram_budget.h's AddHolder asks.
//
// Threading: render thread only, except Initialize and Shutdown (the lifecycle thread).
//
// CHECK (nothing here was run: no build, no game; each is a line of the first runs):
//   G2-a  the first frame's order and barriers: the field's textures in GENERAL from RecordMaintenance, to SHADER_READ for the trace and back for the blend (Stage::COMPUTE: NON_PIXEL_SHADER_RESOURCE on D3D12), the
//         counters', ray buffer's and state buffer's UAV barriers; the D3D12 debug layer and Vulkan validation clean
//   G2-b  the layouts: the trace's four sets and the push block at space 4, the blend's one set and the push block at space 1, the pass set's seven views (D3D12 counts them), the descriptor set shared by the
//         two layouts; the trace shader with the ray tracing set bound at set 3 (CAR_RT_SPACE 3: t10 and t11 the world frame's)
//   G2-c  typed UAV loads of RGBA16F and RG16F in the blend (D3D12 TypedUAVLoadAdditionalFormats: every DX12 card of the strong tier has them; Vulkan reads with the format given to SPIR-V explicitly)
//   G2-d  the units: the trace shades in the paint's units (params.units = 1), the field's, as gi_frame.h says; the apply's baseline and cap are in the same units (gi_apply.h G3h)
//   G2-e  the shadow rays of the hit shading see the cars in the top level (the library's sun and lamp rays use every mask): a car's shadow enters the probes beside it
//   G2-f  the stale boxes: a built sector marks the probes near it in the scheduler's own set (not the field's dirty sets: they drive the field's fade), CityRtScene::DrainBuilt
//   G2-g  the timestamps: a pool of two a frame slot, reset and written in the middle of the frame's list and read when the slot comes round, only for frames that were submitted (OnSubmitted)
//   G2-h  the cost against M3 section 6 (trace 0.4 to 0.7 ms, blend 0.1 to 0.2 at HIGH on a 5080 at 4K): the first cost run decides every number marked COST PASS in gi_frame.h and gi_update_rules.h
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "gi_field.h"
#include "gi_frame.h"
#include "gi_update_rules.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class CityHitScene;
class CityRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;
class RtWorldFrame;

class GiUpdate {
 public:
  GiUpdate() = default;
  ~GiUpdate();
  GiUpdate(const GiUpdate&) = delete;
  GiUpdate& operator=(const GiUpdate&) = delete;

  // After the car scene's own Initialize (its descriptor set exists) and the world frame's, before the host precache starts. Never fails the renderer: where the device cannot trace, or a layout or a shader
  // cannot be made, available() stays false and the update is never given to the integration. `listed_at_start`: the setting is not off at the start, so the two pipelines go in the precache; else they are
  // made in the background when first wanted.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, GiField& field, CarRtScene& car_scene, CityRtScene& city_scene,
                  CityHitScene& hit_scene, RtSceneService& service, RtWorldFrame& world, bool listed_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // GiIntegration::BeginFrame, every frame, before the field's BeginFrame: `level` is the setting's, resolved (off where the gate is shut). Asks the scene service for the city's structures and hit data while
  // a field is wanted.
  void OnWant(gi::Level level);
  // The probe update (gi_integration.h UpdateFn): records the frame's chunks on `list`. Does nothing where THE IDENTITY (above) says.
  void Record(plume::RenderCommandList* list, uint32_t frame_slot, const gi::GiFrame& update_frame);
  // After FrameRing::Submit executed the slot's list (not after an abandoned one): the slot's timestamps are in.
  void OnSubmitted(uint32_t frame_slot);

  // What the update has done (for the tests of the order and the log).
  bool recorded_this_frame() const { return recorded_frame_ == frame_; }
  uint64_t chunks() const { return chunks_total_; }
  float governor_scale() const { return governor_.scale(); }

  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  enum Variant : uint32_t { kVTrace = 0, kVBlend = 1, kVariantCount = 2 };
  static const char* VariantName(uint32_t variant);
  // The push block both passes read (hlsl/gi_update_*_cs.hlsl GiuPush), 64 bytes.
  struct Push {
    uint32_t control[4];  // the probes in this chunk, the rays a probe, the chunk's first probe in the list, the frame's serial
    uint32_t flags[4];    // the counters on, 0, 0, 0
    float tune[4];        // the bounce weight, the rays' reach, the hysteresis, the jump hysteresis
    float tune2[4];       // the tangent of a ray's cone's half angle, 0, 0, 0
  };
  static_assert(sizeof(Push) == gi_update::kPushBytes, "the push block is 64 bytes (hlsl/gi_update_trace_cs.hlsl GiuPush)");
  // Per frame slot: the rows' and the probe list's upload buffers, the pass set naming them with the ray buffer, the textures, the state and the counters, and the timestamps.
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> rows;
    std::unique_ptr<plume::RenderBuffer> list;
    uint64_t list_bytes = 0;
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t set_key = 0;  // what the set was made for (SetKey)
    std::unique_ptr<plume::RenderQueryPool> stamps;
    bool stamps_written = false;
    bool stamps_pending = false;
  };

  bool MakeLayoutsAndShaders(CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  plume::RenderPipeline* Pipeline(uint32_t variant);
  bool PipelinesReady();
  bool EnsureBuffers(const gi_update::Budget& budget);
  bool EnsureSlot(uint32_t frame_slot, const gi_update::Budget& budget);
  uint64_t SetKey() const;
  void RetireSets();
  void ReleaseBuffers();
  void Harvest(Slot& slot);
  void DrainBuiltSectors();
  void ProcessReadback(uint64_t frame, uint64_t generation);
  void LogInterval(uint64_t frame, const gi_update::StatSummary& stats, uint32_t frames);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GiField* field_ = nullptr;
  CarRtScene* car_scene_ = nullptr;
  CityRtScene* city_scene_ = nullptr;
  CityHitScene* hit_scene_ = nullptr;
  RtSceneService* service_ = nullptr;
  RtWorldFrame* world_ = nullptr;

  bool ready_ = false;
  bool listed_ = false;
  std::array<bool, kVariantCount> pipeline_failed_ = {};
  plume::RenderDescriptorSetBuilder pass_set_builder_;
  uint32_t rows_index_ = 0, probes_index_ = 0, rays_index_ = 0, irradiance_index_ = 0, moments_index_ = 0, state_index_ = 0, stats_index_ = 0;
  std::unique_ptr<plume::RenderPipelineLayout> trace_layout_;
  std::unique_ptr<plume::RenderPipelineLayout> blend_layout_;
  std::unique_ptr<plume::RenderShader> trace_shader_;
  std::unique_ptr<plume::RenderShader> blend_shader_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<Slot, kFramesInFlight> slots_;
  bool stamps_ok_ = false;

  // The buffers the passes share: the ray records (sized for the layout), the counters and their readback.
  std::unique_ptr<plume::RenderBuffer> rays_;
  uint64_t ray_bytes_ = 0;
  uint32_t ray_serial_ = 0;
  gi_update::Budget buffers_for_;
  std::unique_ptr<plume::RenderBuffer> stats_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  bool readback_busy_ = false;
  uint64_t generation_ = 0;
  std::array<uint32_t, gi_update::kStatWords> last_words_ = {};
  bool have_last_words_ = false;

  // The frame.
  gi_update::Scheduler scheduler_;
  gi_update::Governor governor_;
  uint32_t scheduler_generation_ = 0;
  uint64_t frame_ = 0;
  uint64_t recorded_frame_ = ~uint64_t(0);
  uint64_t next_readback_ = 0;
  double last_camera_[3] = {0, 0, 0};
  bool have_camera_ = false;

  // The log and the totals.
  bool first_logged_ = false;
  uint64_t chunks_total_ = 0;
  uint64_t frames_run_ = 0;
  uint64_t rays_planned_ = 0;
  uint64_t cuts_ = 0;
  std::array<uint64_t, gi::kMaxCascades> interval_probes_ = {};
  uint64_t interval_frames_ = 0;
  uint64_t interval_planned_rays_ = 0;
  uint32_t no_scene_ = 0, no_world_ = 0, no_hit_data_ = 0, not_ready_ = 0, no_resources_ = 0, not_usable_ = 0;
  uint64_t stale_marked_ = 0;
  uint64_t sectors_built_seen_ = 0;
};

}  // namespace mcla::gpu
