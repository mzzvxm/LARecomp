#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced visibility of the top K lights: the host (RT-LIGHTS-D1 part 1; perf/RT-DIRECT-SURVEY.md sections 4 O1 + O2, 5, 10; perf/RT-LIGHTS-D1.md)
// ===========================================================================
// rt_lights_rules.h is the specification (read its first page: what the pass is, the composition and its identity, the top K, the channels), hlsl/rt_lights_trace_cs.hlsl the trace, hlsl/rt_mask_read.hlsli
// and hlsl/local_light_ps.hlsl the reader. This file is the Plume side, modelled on the probe field's per-pixel pass (gi_pixel.h) and on RtDenoise's second consumer (rt_denoise.h): the compute layout
// with the ray tracing set, the pass set, the buffers, the pipeline, the images, the denoiser, the counters and the census.
//
//   WHEN      LocalLights::OnResolve (local_lights.h) calls it, at the mid-scene depth copy after the occlusion's apply, in this order: AppendLampLights BEFORE the pass makes its selection (the nearest street
//             lamps the atlas did not pick are handed to it so that they get a ray: a game's lamp the pass would otherwise never look at); Plan AFTER the selection (the top K of the lights the pass is going
//             to draw, the channels, the annotated and trimmed selection); RecordTrace and RecordDenoise before the buffer is built (outside any render pass, in the list the frame's top level was built
//             in); Masks() into the pass's buffer. A frame that does not run the pass records nothing and marks nothing, and the pass is the pass it was.
//   WHAT      Plan decides (rt_lights_rules.h Decide: the setting, the device, the top level, the world frame's rows, the occlusion's depth, the lights, the pipelines, the images), writes the rows (a 64 byte
//             header and a 64 byte row for each light, world space: the shader moves them into the top level's view space with the world frame's axes) and keeps the frame's numbers. RecordTrace: one
//             dispatch, three signal images (a group of three lights each) and the denoiser's three guides written as storage images. RecordDenoise: RtDenoise's temporal stage for each group (the last
//             frame's result by the camera's motion; one denoiser instance a group: instances 2 to 4) then its spatial iterations; the result is what the light pass reads.
//   IDENTITY  Nothing is made, recorded or bound while mcla_gpu_rt_lights is off, or auto on a card whose RT SHADOWS level is off, or the device does not trace rays, or the setting's K is under one: the light
//             pass's buffer has no table, no light has the flag, the shader reads nothing of it: the pass is what it was (rt_lights_test.cpp and lamp_shadows_test.cpp hold the twins to it).
//             mcla_gpu_rt_lights_check 1 traces and costs but writes every visibility as 1: the picture must equal the pass without the setting, bit for bit (the gate's identity test).
//
// SETTINGS (debug only; settings_gpu.h): mcla_gpu_rt_lights (auto | off | on; live; off until part 2's gate), mcla_gpu_rt_lights_k (0: the level's: 3, 6 at the largest LIGHT SHADOWS level; 1 to 9),
// mcla_gpu_rt_lights_radius (metres, 0: by class), mcla_gpu_rt_lights_rays (1 to 4 a light and pixel), mcla_gpu_rt_lights_view (0 off, 1 the filtered visibility drawn instead of the lights, 2 the raw one),
// mcla_gpu_rt_lights_check (0, 1 every visibility 1, 2 every visibility 0), mcla_gpu_rt_lights_lamp_slot (0: class first pick as it is; 1: one of K >= 2 channels is reserved for the strongest street
// lamp when the headlights and neon fill all of them: the A/B of RTL-D1-REVIEW S2). The noise follows mcla_gpu_noise_blue.
//
// MEMORY (part 2, C15: rt_lights_rules.h "the memory"). The pass is traced at the MASK's size (the occlusion's traced size over a step of 1, or 2 where that size is taller than 720 lines:
// mcla_gpu_rt_lights_scale), every image is 8 bit (the signal and RtDenoise's four a group RGBA8, the depth guides R16F, the normal and the share one RGBA8 snorm, no hit image): 28 bytes a mask texel
// for K 3, 13.8 MiB at 1080p and at 4K and 24.6 MiB at 1440p, against part 1's 118.7 / 118.7 / 52.7 MiB. The light pass reads the mask back by a depth guided bilinear. vram_budget.h's AddHolder
// asks HeldBytes(); the census line says it. CHECK (C15): the numbers on a device (the `[rt lights] the pass's images` line), the 8 bit history's look (the stall rule: Nudge8), the upsample's look at 4K.
//
// RAW MODE (part 2, the Ray Reconstruction hook, perf/DLSS-RR.md; dlss_rr_rules.h RawModes().RtLights()). SetRawMode(true) before Plan, in a frame DLSS Ray Reconstruction takes and
// mcla_gpu_dlss_rr_rt_lights is not 0: the trace runs as always, RtDenoise's two stages do NOT (RR is the denoiser: its input is the picture the light pass draws with the 1 spp visibility in it), the
// light pass reads the unfiltered signal (the mask table's slots are the signal images', kMaskFlagRrRaw), no history is written (the next frame that is not raw starts it again: RtDenoise's history
// is the last filtered result) and the guide pass gets no hit distance from it (a visibility has none: NVIDIA's advice for a diffuse term, G6-4). auto_uses_rr stays false until the D4 A/B (C11).
//
// CHECK (nothing here was run: no build, no game; each is a line of the first runs; perf/RT-LIGHTS-D1.md has them with the chain):
//   D1-1  the trace layout's three sets and the push block at space 3, and the pass set's eight views (D3D12 counts them: descriptor_count_test reads it); the debug layer and Vulkan validation clean
//   D1-2  typed storage image STORES of RGBA16F, R32F and RG16F on both APIs (no typed loads here), the barriers: the signals and the guides to GENERAL for the trace, RtDenoise's reads to the read state of
//         both stages, the guides to the read state of both stages after the denoisers (the pixel stage reads the results, the compute stage the guides next frame)
//   D1-3  the light pass reads the result images and the occlusion's linear depth image through the heap's set 0 in the pixel stage: their layout after RecordDenoise (RtDenoise's final pass states both
//         stages), the linear depth image's layout as the screen products publish it
//   D1-4  the history: a group whose channels changed owner starts again (Plan keeps the mask of groups until RecordDenoise consumes it); a camera cut (rt_lights_rules.h CameraCut) starts all of them
//   D1-5  the cost (the labels 'rt lights' and 'rt lights denoise' in the pass timing) against the survey's 0.3 to 0.6 ms at 1080 and 0.4 to 0.8 at 4K on the 5080 (the gate's number is <= +0.6 / +0.8)
//   D1-6  the lamps the pass is handed for their rays: the nearest K of the game's list by what they add to the view, a lamp the atlas picked is not handed twice; with the list's hook not in the build
//         (a toolkit scene) there are no lamps and the headlights alone get rays
//
// Threading: render thread only, except Initialize and Shutdown (the lifecycle thread, GPU idle and the FrameRing drained).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"
#include "gi_pixel_rules.h"
#include "host_passes.h"
#include "light_frame.h"
#include "local_lights_rules.h"
#include "pause_world_fx.h"
#include "rt_denoise.h"
#include "rt_lights_rules.h"
#include "vol_lamp_list.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;
class RtWorldFrame;

class RtLights {
 public:
  // What the pass reads of this frame that the light pass already holds (LocalLights::OnResolve).
  struct Inputs {
    uint64_t frame = 0;                  // the light pass's frame counter (consecutive frames' histories are valid)
    uint32_t frame_slot = 0;
    gbuffer::PauseFxView pause_fx = gbuffer::PauseFxView::kNone;
    float position[3] = {};              // the camera's place, and the axes of its pose in the world (right, up, forward)
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float forward[3] = {0, 0, 1};
    float tan_x = 0.0f, tan_y = 0.0f;    // the lens
    uint32_t traced[2] = {0, 0};         // the occlusion's traced size
    uint32_t render[2] = {0, 0};         // the picture's size
    uint32_t lin_slot = larecomp::gpu::kNoLightSlot;   // the occlusion's linear depth image's bindless slot (the screen products' lin_depth_slot[0])
  };

  RtLights() = default;
  ~RtLights();
  RtLights(const RtLights&) = delete;
  RtLights& operator=(const RtLights&) = delete;

  // After the car scene's own Initialize (its descriptor set exists) and the world frame's, before the host precache starts. Never fails the renderer: where the device cannot trace, or a layout, a shader or
  // RtDenoise cannot be made, available() stays false and nothing asks for it.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene& car_scene, RtWorldFrame& world, RtSceneService& service);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // The frame's start (LocalLights::BeginFrame, which the Scene calls before everything that reads the structures' wants): reads the settings, asks the ray tracing scene service for the structures and
  // the game's lamp list for its copy while the pass is wanted. `pass_runs`: the light pass runs this frame (nothing else makes a use of this class).
  // `ultra`: the largest LIGHT SHADOWS level (K 6 where the setting says the level's).
  void BeginFrame(bool pass_runs, bool ultra);
  bool wanted() const { return wanted_; }
  // RT-LIGHTS-D1 part 2: Ray Reconstruction takes this frame and the effect's switch is on (TemporalSystem::RawModes().RtLights()), asked once a frame before Plan: the filter is skipped, the light pass reads the
  // 1 spp signal. False (the default, and every frame RR does not take) is the pass as it was.
  void SetRawMode(bool raw) { raw_requested_ = raw; }
  bool raw_frame() const { return raw_frame_; }

  // 1. BEFORE the pass's selection: the nearest street lamps of the game's list that no provider handed over (`extras` holds the atlas's lamps: a lamp with the same id is not made twice), as lights the game
  // lights with no atlas views, at most K, appended. `position` is the camera's. Returns how many. Nothing while the pass is not wanted.
  uint32_t AppendLampLights(const float position[3], std::vector<larecomp::gpu::Light>* extras);
  // 2. AFTER the selection: the top K of the selection's lights, their channels, the selection annotated and trimmed (local_lights_rules.h AnnotateSelection, TrimSelection: a lamp handed over for a ray and
  // not picked is taken out). True when the pass will run this frame (RecordTrace and RecordDenoise follow, then Masks()).
  bool Plan(const Inputs& in, larecomp::gpu::local_lights::Selection* selection);
  void RecordTrace(plume::RenderCommandList* list);
  void RecordDenoise(plume::RenderCommandList* list);   // the filter; in a raw frame (SetRawMode) no filter: the light pass reads the signal
  // What the light pass is handed: valid only after RecordDenoise of a frame that ran.
  const larecomp::gpu::rt_lights::MaskFrame& Masks() const { return masks_; }
  bool ran_this_frame() const { return ran_frame_ == frame_; }

  // A guest present: the interval line, the denoisers' frame boundary and the idle count.
  void OnPresent();

  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> rows;
    std::array<std::unique_ptr<plume::RenderDescriptorSet>, 2> set;   // by the depth guide's parity
    std::array<uint64_t, 2> set_key = {0, 0};
  };

  bool MakeLayoutAndShader(CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  bool PipelineReady();
  bool EnsureImages(uint32_t width, uint32_t height, uint32_t groups);
  bool EnsureSlot(uint32_t frame_slot);
  void ReleaseImages();
  void RecordRaw(plume::RenderCommandList* list);
  larecomp::gpu::rt_lights::MaskInputs MakeMaskInputs() const;
  void RetireSets();
  void ProcessReadback(uint64_t frame, uint64_t generation);
  void LogInterval(uint64_t frame, const std::array<uint32_t, larecomp::gpu::rt_lights::kStatWords>& delta);
  void CountWhy(larecomp::gpu::rt_lights::Why why) { ++why_counts_[uint32_t(why)]; }
  void Fail(larecomp::gpu::rt_lights::Why why, larecomp::gpu::local_lights::Selection* selection);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* car_scene_ = nullptr;
  RtWorldFrame* world_ = nullptr;
  RtSceneService* service_ = nullptr;

  bool ready_ = false;
  bool pipeline_failed_ = false;
  bool listed_ = false;
  plume::RenderDescriptorSetBuilder pass_set_builder_;
  uint32_t rows_index_ = 0, signal_index_[3] = {0, 0, 0}, depth_index_ = 0, normal_index_ = 0, stats_index_ = 0;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  std::array<Slot, kFramesInFlight> slots_;
  std::array<RtDenoise, larecomp::gpu::rt_lights::kMaxImages> denoise_;   // one a group of three lights: instances 2 to 4

  // The images, all at the MASK's size: the signals (a group each, RGBA8), the depth guide (R16F, two, by parity), the view normal guide with the share in its alpha (RGBA8 snorm).
  std::array<HostImage, larecomp::gpu::rt_lights::kMaxImages> signal_;
  std::array<HostImage, 2> depth_;
  HostImage normal_;
  uint64_t image_serial_ = 0;
  uint32_t image_width_ = 0, image_height_ = 0, image_groups_ = 0;
  uint32_t idle_frames_ = 0;
  std::unique_ptr<plume::RenderBuffer> stats_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  bool readback_busy_ = false;
  uint64_t generation_ = 0;
  std::array<uint32_t, larecomp::gpu::rt_lights::kStatWords> last_words_ = {};
  bool have_last_words_ = false;
  uint64_t next_readback_ = 0;

  // The settings of this frame.
  larecomp::gpu::rt_lights::Mode mode_ = larecomp::gpu::rt_lights::Mode::kOff;
  bool wanted_ = false;
  int k_setting_ = 0;
  double radius_setting_ = 0.0;
  uint32_t rays_ = 1;
  int view_ = 0;
  int check_ = 0;
  int scale_setting_ = 0;
  bool lamp_slot_ = false;   // mcla_gpu_rt_lights_lamp_slot
  bool raw_requested_ = false;
  bool blue_ = true;
  bool lamp_hook_on_ = false;
  bool ultra_ = false;
  std::vector<vol_lamps::GameLamp> list_;   // the game's lamp list, the newest copy the hook table gave
  uint64_t list_seq_ = 0;

  // The frame.
  Inputs in_;
  larecomp::gpu::rt_lights::Why why_ = larecomp::gpu::rt_lights::Why::kOff;
  uint32_t k_ = 0;
  uint32_t step_ = 1;                                  // the traced pixels a mask texel spans on an axis this frame
  uint32_t groups_ = 0;
  uint32_t light_count_ = 0;
  larecomp::gpu::rt_lights::Assignment channels_;      // who holds each channel, as of the last Plan that ran
  uint32_t pending_reset_ = 0;                         // groups whose channels changed owner since the last RecordDenoise: their history starts again
  larecomp::gpu::rt_lights::MaskFrame masks_;
  std::array<const HostImage*, larecomp::gpu::rt_lights::kMaxImages> result_ = {};
  uint32_t parity_ = 0;                                // the depth guide this frame's trace writes
  bool stats_frame_ = false;
  bool history_ok_ = false;                            // this frame may read the last frame's result (the previous frame, the same size, no cut)
  gbuffer::PauseFxView prev_pause_fx_ = gbuffer::PauseFxView::kNone;
  bool raw_frame_ = false;                             // this frame is raw: Ray Reconstruction is the denoiser (SetRawMode, taken at Plan)
  uint64_t frame_ = 0;
  uint64_t ran_frame_ = ~uint64_t(0);
  bool ran_since_present_ = false;
  gi_pixel::CameraPose pose_;
  float tan_[2] = {1, 1};
  bool have_prev_ = false;
  uint64_t prev_frame_ = 0;
  gi_pixel::CameraPose prev_pose_;
  float prev_tan_[2] = {1, 1};
  uint32_t prev_size_[2] = {0, 0};
  uint32_t prev_step_ = 1;
  uint32_t history_frames_ = 0;
  std::array<larecomp::gpu::rt_lights::GpuRtLight, larecomp::gpu::rt_lights::kMaxLights> rows_{};
  std::array<larecomp::gpu::rt_lights::Class, larecomp::gpu::rt_lights::kMaxLights> classes_{};

  // The log and the totals.
  bool first_logged_ = false;
  bool raw_logged_ = false;
  uint32_t last_state_ = ~0u;
  uint64_t frames_run_ = 0;
  uint64_t raw_frames_ = 0;
  uint64_t lamps_handed_ = 0;
  std::array<uint64_t, size_t(larecomp::gpu::rt_lights::Why::kCount)> why_counts_ = {};
  std::array<uint64_t, 4> class_lights_ = {};          // lights that held a channel, summed over the frames that ran, by Class
};

}  // namespace mcla::gpu
