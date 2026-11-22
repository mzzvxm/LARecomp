#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's per-pixel detail pass: the host (CITY-RT M3, package G5 of perf/CITY-RT-M3.md section 5)
// ===========================================================================
// gi_pixel_rules.h is the specification (read its first page: what the pass is, the ray, the hit, the miss, the denoiser, the mix, the raw mode prepared for G6, the identity), hlsl/gi_pixel_common.hlsli its
// twin, hlsl/gi_pixel_trace_cs.hlsl the trace. This file is the Plume side, modelled on the probe update (gi_update.h): the compute layout with the ray tracing set, the pass set, the buffers, the pipelines,
// the timestamps and the census; and on RtDenoise's first consumer (rt_denoise.h: the DIFFUSE profile over the plain guides).
//
//   WHEN      GiApply::OnTrigger calls Begin / RecordTrace / RecordDenoise / Publish BEFORE its lookup (the lookup reads the image), at the mid-scene depth copy after the occlusion's apply, outside any
//             render pass, in the list the frame's top level was built in. The marks are the apply's ('rt gi pixel' before the trace, 'rt gi denoise' before the temporal stage: gi_apply.cpp): the frame
//             order's table holds them to that file. A frame that does not run the pass records nothing and marks nothing.
//   WHAT      Begin decides (gi_pixel_rules.h Decide: the layout, the field, the top level, the world rows, the hit data, the pipelines, the images), writes the rows (the apply's buffer, the pixel rows, the
//             surface lights of the light manager's frame) and keeps the frame's numbers. RecordTrace: one dispatch, the signal and the three guides written as storage images. RecordDenoise: RtDenoise's
//             temporal stage (the last frame's result by the camera's motion; this class keeps the last pose, the guides' ping-pong and the history's count) then up to three spatial iterations, and the
//             barriers that let the apply's pixel stage read the result and the guides. Publish puts the image's slot, the guides' slots and the flag in the apply's copy of the field's frame.
//   GOVERNOR  two timestamps a frame slot around the trace and the denoiser feed gi_pixel_rules.h Governor, which takes the pass from the traced size to half of it on each axis (a quarter of the pixels)
//             when it stays over its cap, and back. Without timestamps the pass stays at the traced size.
//   RAW (G6)  in a frame DLSS Ray Reconstruction takes (Inputs::raw, from TemporalSystem::RawModes().RtGi()) RtDenoise's two stages do not run: the 1 spp signal image is what the apply reads (published as the
//             pixel image; a raw frame's weight is gi_pixel_rules.h RawWeight, 1 where traced) and the hit distance image is the guide pass's last-choice hit distance (RawOutput). The consumer is registered by the
//             apply (GiApply::BeginFrame: denoiser::NoteConsumer(rr::Consumer::kRtGi, ...)); the denoiser's history is not written in a raw frame, so the next frame starts it again. With the plan not RR nothing
//             of this exists: the same passes, the same words, the same shaders as before G6.
//
// IDENTITY. Nothing is made, recorded or bound while the layout has no per-pixel pass (HIGH, OFF, every card without a strong ray tracing tier), and the apply's shader reads nothing of it while the field's frame
// has no kPixelIrradiance. Images go back after kReleaseAfterFrames frames without a run.
//
// MEMORY. 60 bytes a pixel of the pass's image (gi_pixel_rules.h BytesPerPixel: the signal, two depth guides, the normal, the hit image, RtDenoise's four): 119 MiB at a traced 1080 lines, a third of that at
// the reduced size; HeldBytes() is what vram_budget.h's AddHolder asks.
//
// Threading: render thread only, except Initialize and Shutdown (the lifecycle thread, GPU idle and the FrameRing drained).
//
// CHECK (nothing here was run: no build, no game; each is a line of the first runs):
//   G5-1  the trace layout's five sets and the push block at space 5 (the heap's texture set twice, the samplers, the pass set, the ray tracing set); the pass set's six views (D3D12 counts them)
//   G5-2  typed storage image STORES of RGBA16F, R32F and RG16F (no typed loads here: D3D12 and Vulkan both do these); the denoiser's own reads are sampled
//   G5-3  the barriers: the four images to GENERAL for the trace, RtDenoise's reads to the compute stage's read state, the apply's guides to the read state of both stages (the pixel stage reads them)
//   G5-4  the previous depth guide bound but never written in the first frame (the temporal stage reads it only where the history is valid)
//   G5-5  the motion: the last pose of this class's own (the previous run's frame), valid when that run was the previous frame's and the camera did not cut (GiField::move)
//   G5-6  the cost against the doc (trace 1.0 to 1.5 ms, denoise 0.3 to 0.6 ms at a traced 1080 lines on the 5080): the first cost run decides every number marked COST PASS
//   G6-1  the raw barrier: the signal and the hit image go from GENERAL to SHADER_READ at GRAPHICS_AND_COMPUTE in a raw frame (the denoiser's own last pass does it in a plain one); the guide pass then
//         states the hit image at the compute stage (RR-4's images do the same)
//   G6-2  the raw frame's weight is 1 where traced: the apply's net-irradiance rules (baseline, ceiling, cap, dead band) act on the NOISY sample, so the mean of the clamped samples is not the clamp of the
//         mean where the clamps bite (a bright lamp's bounce): A/B against the filtered result (mcla_gpu_dlss_rr_rt_gi 0)
//   G6-3  the noise Ray Reconstruction gets is the traced size's (half the picture's above 1080 lines, and the governor's half of that): spatially correlated (the apply's reads are nearest or depth and
//         normal weighted bilinear), whereas RR's guide asks for independent noise per pixel: A/B a full size raw trace if the pictures show blocks
//   G6-4  the hit distance of a pixel whose ray found nothing within 12 m is kMissDistance (4096 m): "reflects far away" for a diffuse bounce whose reach is 12 m; mcla_gpu_dlss_rr_rt_gi 2 leaves the
//         hit distance out (NVIDIA's advice for a diffuse term) and is the A/B
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "gi_apply_rules.h"
#include "gi_field.h"
#include "gi_frame.h"
#include "gi_pixel_rules.h"
#include "host_passes.h"
#include "rt_denoise.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class CityHitScene;
class CityRtScene;
class Device;
class FrameRing;
class LightManager;
class PassTiming;
class RtWorldFrame;

class GiPixel {
 public:
  // Everything the pass reads of this frame that the apply already holds.
  struct Inputs {
    uint64_t frame = 0;                              // the apply's frame counter (consecutive frames' histories are valid)
    uint32_t frame_slot = 0;
    const gi::GiFrame* field = nullptr;              // the field's published frame
    const gi_apply::Buffer* rows = nullptr;          // the apply's rows: the field's 20 and its 9
    const gi_apply::Frame* camera = nullptr;         // the camera, the sizes and the light as the apply has them
    uint32_t traced[2] = {0, 0};                     // the occlusion's traced size
    uint32_t render[2] = {0, 0};                     // the picture's size
    uint32_t depth_slot = gi::kNoSlot, normal_slot = gi::kNoSlot, albedo_slot = gi::kNoSlot, lin_slot = gi::kNoSlot;   // the main depth plane, RT1, RT2, the occlusion's linear depth
    uint32_t sample0 = 0;                            // the host sample that is the guest's sample 0
    bool multisampled = false;
    bool raw = false;                                // G6: Ray Reconstruction takes this frame and the effect's switch is on (TemporalSystem::RawModes().RtGi()): no denoiser, the signal is handed on as it is
  };

  GiPixel() = default;
  ~GiPixel();
  GiPixel(const GiPixel&) = delete;
  GiPixel& operator=(const GiPixel&) = delete;

  // After the car scene's own Initialize (its descriptor set exists) and the world frame's, before the host precache starts. Never fails the renderer: where the device cannot trace, or a layout, a shader or
  // RtDenoise cannot be made, available() stays false and the apply never asks. `lights` may be null (no surface lights: the headlights' bounce is off).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, GiField& field, CarRtScene& car_scene, CityRtScene& city_scene,
                  CityHitScene& hit_scene, RtWorldFrame& world, const LightManager* lights);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // The apply, once a frame, before its lookup: decides and prepares. True when the pass runs this frame (RecordTrace and RecordDenoise follow, then Publish).
  bool Begin(const Inputs& in);
  void RecordTrace(plume::RenderCommandList* list);
  void RecordDenoise(plume::RenderCommandList* list);
  // Puts the denoised image, its guides and the flag in the apply's copy of the field's frame; false (the frame untouched) when there is no image this frame.
  bool Publish(gi::GiFrame* frame) const;
  bool ran_this_frame() const { return ran_frame_ == frame_; }

  // G6's door: the 1 spp radiance image and the hit distance image of the frame the pass ran (the apply's pixel image, and the guide pass's hit distance input), valid only after RecordTrace and only in a
  // frame whose plan says raw. Both are in the read state of the graphics and compute stages once RecordDenoise has run.
  struct RawOutput {
    bool valid = false;
    uint64_t frame = 0;                  // the apply's frame counter of the frame it describes (a caller that did not run the pass this frame compares it)
    const HostImage* signal = nullptr;   // RGBA16F, rgb the sample's radiance (the field's units: albedo x it is what the apply adds)
    const HostImage* hit = nullptr;      // RG16F, x the hit distance (gi_pixel_rules.h HitDistance), y the share
    uint32_t width = 0, height = 0;
  };
  RawOutput raw_output() const;

  // After FrameRing::Submit executed the slot's list: the slot's timestamps are in.
  void OnSubmitted(uint32_t frame_slot);
  // A guest present: the interval line and the idle count.
  void OnPresent();

  uint64_t HeldBytes() const;
  std::string CensusText() const;

 private:
  enum Variant : uint32_t { kVPlain = 0, kVMs = 1, kVariantCount = 2 };
  static const char* VariantName(uint32_t variant);
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> rows;
    std::array<std::unique_ptr<plume::RenderDescriptorSet>, 2> set;   // by the depth guide's parity
    std::array<uint64_t, 2> set_key = {0, 0};
    std::unique_ptr<plume::RenderQueryPool> stamps;
    bool stamps_written = false;
    bool stamps_pending = false;
  };

  bool MakeLayoutsAndShaders(CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  plume::RenderPipeline* Pipeline(uint32_t variant);
  bool PipelineReady(uint32_t variant);
  bool EnsureImages(uint32_t width, uint32_t height);
  bool EnsureSlot(uint32_t frame_slot);
  void ReleaseImages();
  void RetireSets();
  void Harvest(Slot& slot);
  void ProcessReadback(uint64_t frame, uint64_t generation);
  void LogInterval(uint64_t frame, const std::array<uint32_t, gi_pixel::kStatWords>& delta, uint32_t frames);
  void CountWhy(gi_pixel::Why why) { ++why_counts_[uint32_t(why)]; }

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GiField* field_ = nullptr;
  CarRtScene* car_scene_ = nullptr;
  CityRtScene* city_scene_ = nullptr;
  CityHitScene* hit_scene_ = nullptr;
  RtWorldFrame* world_ = nullptr;
  const LightManager* lights_ = nullptr;

  bool ready_ = false;
  std::array<bool, kVariantCount> pipeline_failed_ = {};
  std::array<bool, kVariantCount> listed_ = {};
  plume::RenderDescriptorSetBuilder pass_set_builder_;
  uint32_t rows_index_ = 0, signal_index_ = 0, depth_index_ = 0, normal_index_ = 0, hit_index_ = 0, stats_index_ = 0;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, kVariantCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<Slot, kFramesInFlight> slots_;
  bool stamps_ok_ = false;
  RtDenoise denoise_;

  // The images: the signal, the depth guide (two, by parity), the view normal guide, the hit image.
  HostImage signal_;
  std::array<HostImage, 2> depth_;
  HostImage normal_;
  HostImage hit_;
  uint64_t image_serial_ = 0;
  uint32_t image_width_ = 0, image_height_ = 0;
  uint32_t idle_frames_ = 0;
  std::unique_ptr<plume::RenderBuffer> stats_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  bool readback_busy_ = false;
  uint64_t generation_ = 0;
  std::array<uint32_t, gi_pixel::kStatWords> last_words_ = {};
  bool have_last_words_ = false;
  uint64_t next_readback_ = 0;

  // The frame.
  gi_pixel::Governor governor_;
  gi_pixel::Plan plan_;
  Inputs in_;
  gi::PixelParams params_;
  uint32_t divisor_ = 1;
  uint32_t parity_ = 0;                // the depth guide this frame's trace writes
  uint32_t lights_in_rows_ = 0;
  bool stats_frame_ = false;
  bool history_ok_ = false;            // this frame may read the last frame's result (Begin: the previous frame at this size, no cut)
  bool ran_since_present_ = false;     // a run since the last guest present (OnPresent counts the frames without one)
  const HostImage* result_ = nullptr;  // the image the apply reads
  uint64_t frame_ = 0;
  uint64_t ran_frame_ = ~uint64_t(0);
  gi_pixel::CameraPose pose_;
  float tan_[2] = {1, 1};
  // the last run's pose and lens: the next frame's history
  bool have_prev_ = false;
  uint64_t prev_frame_ = 0;
  gi_pixel::CameraPose prev_pose_;
  float prev_tan_[2] = {1, 1};
  uint32_t prev_size_[2] = {0, 0};
  uint32_t history_frames_ = 0;

  // The log and the totals.
  bool first_logged_ = false;
  uint64_t frames_run_ = 0, frames_raw_ = 0, frames_reduced_ = 0;
  bool raw_logged_ = false;   // the raw state the log last said (a change is one line)
  std::array<uint64_t, size_t(gi_pixel::Why::kCount)> why_counts_ = {};
};

}  // namespace mcla::gpu
