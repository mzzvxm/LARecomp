#pragma once
// ===========================================================================
// MCLA GPU renderer -- RtDenoise: THE HOST, shared (CITY-RT package R6 of perf/CITY-RT-M2.md; CITY-RT-M3.md G5 uses the same class)
// ===========================================================================
// The design and the numbers are rt_denoise_rules.h (read its first page); the passes are hlsl/rt_denoise_engine.hlsli over a consumer's guides (hlsl/ssr_denoise_consumer.hlsli: the screen-space
// reflections' fallback; hlsl/rt_denoise_plain_consumer.hlsli: plain guide images). This class owns what every consumer shares: the compute layout (the bindless texture sets, the samplers, the output set with
// one storage image, the push block), the two pipelines built from the consumer's blobs, the result images (a ping-pong pair for the spatial iterations, a pair for the temporal history), the output
// descriptor sets, and the barriers round the dispatches. An effect makes one instance, hands it the blobs of its compile, and records:
//
//   RecordSpatial   n iterations (steps 1, 2, 4) of the bilateral a-trous from the effect's signal image into this class's own images; returns the last result (a shader-read image, valid until the next
//                   call), which the effect's next pass reads in place of the signal
//   RecordTemporal  the temporal pass from the effect's current signal and the history this class keeps (the last result), into the other history image; returns it (rgb the accumulated radiance, a its
//                   length); `ResetHistory` after a camera cut or a size change (the class also resets when the images are made again)
//
// The consumer keeps the push block (the words that name its images and the lens are its own: a `Patch` says which words the class sets: the signal's slot per iteration, the iteration, the history's slot
// and the has-history bit). Every call states the images the passes read besides the signal (the guides): they are moved to the compute stage's read state before the first dispatch, and stay in it.
//
// THE IDENTITY. Nothing exists until a consumer asks: Initialize makes the layout and the shaders (cheap: the blobs); the pipelines are made in the background the first time a stage is asked for
// (SpatialReady / TemporalReady list them with the host precache; a frame never waits for a compile), the images the first time a stage records, and they go back after a spell of frames
// without the stage (OnPresent). An effect that never asks costs nothing.
//
// CHECK (nothing here ran on a device; rt_denoise_test.cpp holds the rules, the sources and the shaders' text, the three shaders compile for DXIL and SPIR-V and validate): the output set's one
// view on D3D12 (the count is the literal in CreateDescriptorSet), RGBA16F storage image writes on both APIs (typed UAV store; no typed load is used), the compute layout's six sets on Vulkan, the
// barriers (the effect's images arrive in a graphics stage's read state: the class moves them to the compute stage's), the cost (the label the effect's Mark gives the span).
//
// Threading: render thread only, except Initialize and Shutdown (the lifecycle thread, GPU idle and the FrameRing drained).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "host_passes.h"
#include "rt_denoise_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;

class RtDenoise {
 public:
  // The consumer's compile of a stage (a null `data`: the consumer has no such stage).
  struct Blob {
    const void* data = nullptr;
    uint64_t size = 0;
    const char* name = "";
  };

  RtDenoise() = default;
  ~RtDenoise() = default;
  RtDenoise(const RtDenoise&) = delete;
  RtDenoise& operator=(const RtDenoise&) = delete;

  // Makes the layout and the shaders. `instance` numbers the consumer among the host precache's keys (0 to 15), `name` is for the logs and the labels. Never fails the renderer: without them nothing
  // is recorded (the effect keeps its own picture). `format`: the result images' (RGBA16F; RGBA8 for a consumer whose shaders are compiled with RT_DN_OUT_UNORM8: the ray traced light visibility).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, uint32_t instance, const char* name, const Blob& spatial, const Blob& temporal,
                  rt_denoise::Format format = rt_denoise::Format::kRgba16f);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // The stage's pipeline is made (listed with the host precache the first time it is asked for).
  bool SpatialReady();
  bool TemporalReady();
  void InitializeV2(const Blob& spatial, const Blob& temporal);
  void InitializeV2Multisampled(const Blob& temporal);
  static bool V2Enabled();

  struct Frame {
    plume::RenderCommandList* list = nullptr;
    HostImage* signal = nullptr;                  // the signal: shader-read; for RecordSpatial the first iteration's input, for RecordTemporal the current frame's one-sample image
    std::array<const HostImage*, 8> reads{};      // guide images to transition before dispatch; null entries are skipped
    uint32_t width = 0;                           // the signal's size
    uint32_t height = 0;
    uint32_t iterations = 1;                      // spatial: 1 to 3
    std::array<uint32_t, rt_denoise::kPushWords> push{};   // the consumer's block (the class sets the words of `patch`)
    rt_denoise::Patch patch;
    bool history_valid = false;                   // temporal: the consumer's motion and depth say the last result may be read (the class also needs it to exist at this size)
    bool multisampled = false;                    // V2: SSR's material planes use the guest's first sample
  };
  // The spatial stage. Null where nothing was recorded (not ready, no images); the signal is untouched.
  const HostImage* RecordSpatial(const Frame& frame);
  // The temporal stage. Null where nothing was recorded. The result is the next frame's history; its alpha is the length.
  const HostImage* RecordTemporal(const Frame& frame);
  // V2 includes both stages. The returned image preserves the consumer's alpha contract.
  const HostImage* RecordV2(const Frame& frame);
  struct VisibilityFrame {
    std::array<float, 12> motion{};
    std::array<uint32_t, 4> bounds{};  // left, top, right, bottom. Empty uses the full image.
    bool motion_valid = false;
  };
  const HostImage* RecordVisibility(plume::RenderCommandList* list, HostImage& signal, const HostImage& depth, const VisibilityFrame& view,
                                    uint64_t frame, float tan_x, float tan_y, float range, uint32_t key);
  // Temporal confidence in alpha is the effective sample count (scaled by 32 for RGBA8).
  const HostImage* V2Confidence() const { return v2_history_valid_ ? &images_[kHistoryA + history_current_] : nullptr; }
  // The next temporal frame reads no history (a camera cut).
  void ResetHistory() { history_valid_ = v2_history_valid_ = false; }
  // True when the last RecordTemporal found a history it could read (the class had one at this size and the frame said it was valid).
  bool last_history_read() const { return last_history_read_; }

  // The frame boundary: the images go back after a spell of frames in which no stage ran.
  void OnPresent();
  uint64_t HeldBytes() const;
  uint64_t spatial_passes() const { return spatial_passes_; }
  uint64_t temporal_passes() const { return temporal_passes_; }

 private:
  enum StageKind : uint32_t { kSpatial, kTemporal, kV2Spatial, kV2Temporal, kV2TemporalMs, kStageCount };
  enum Slot : uint32_t { kSpatialA, kSpatialB, kHistoryA, kHistoryB, kSlotCount };

  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t stage);
  plume::RenderPipeline* Pipeline(uint32_t stage);
  bool EnsureImage(uint32_t slot, uint32_t width, uint32_t height);
  bool EnsureSet(uint32_t slot);
  void RetireSet(uint32_t slot);
  void ReleaseAll();
  void ReleaseV2();
  bool EnsureV2History(uint32_t index, uint32_t width, uint32_t height);
  // One dispatch: `output` slot written from the push block (already patched), then moved to the read state (`final`: graphics and compute, else the compute stage's).
  void Dispatch(plume::RenderCommandList* list, uint32_t stage, uint32_t output_slot, const std::array<uint32_t, rt_denoise::kPushWords>& push, uint32_t width, uint32_t height, bool final_pass);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  std::string name_;
  uint32_t instance_ = 0;
  rt_denoise::Format format_ = rt_denoise::Format::kRgba16f;
  bool ready_ = false;

  plume::RenderDescriptorSetBuilder output_builder_;   // u0: the result image
  plume::RenderDescriptorSetBuilder output_v2_builder_; // u0: result, u1: temporal features
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_v2_;
  std::array<std::unique_ptr<plume::RenderShader>, kStageCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kStageCount> pipelines_;
  std::array<bool, kStageCount> listed_{};
  std::array<bool, kStageCount> failed_{};
  std::array<HostImage, kSlotCount> images_;
  std::array<std::unique_ptr<plume::RenderDescriptorSet>, kSlotCount> sets_;
  std::array<const plume::RenderTexture*, kSlotCount> set_textures_{};
  std::array<HostImage, 2> features_;
  std::array<std::unique_ptr<plume::RenderDescriptorSet>, 2> v2_history_sets_;
  bool v2_history_valid_ = false;
  bool v2_ran_ = false;
  uint32_t v2_idle_frames_ = 0;
  uint64_t visibility_frame_ = 0;
  uint32_t visibility_key_ = 0;
  std::array<float, 2> visibility_lens_{};

  // the temporal history: which of the two images holds the last result, and whether it can be read
  uint32_t history_current_ = 0;
  bool history_valid_ = false;
  bool last_history_read_ = false;

  uint32_t idle_frames_ = 0;
  bool ran_ = false;
  uint64_t spatial_passes_ = 0;
  uint64_t temporal_passes_ = 0;
  uint64_t v2_frames_ = 0;
};

}  // namespace mcla::gpu
