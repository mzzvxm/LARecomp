#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections: THE HOST (CITY-RT package R4 of perf/CITY-RT-M2.md)
// ===========================================================================
// The design, the cells and the numbers are ssr_rt_rules.h (read its first page); the shaders are hlsl/ssr_rt_common.hlsli, ssr_rt_select_cs.hlsl and ssr_rt_trace_cs.hlsl, and the trace pass that writes the
// confidence target is hlsl/ssr_trace_ps.hlsl compiled with SSR_RT 1. This class owns the fallback's compute side: the pipeline layout (the SSR host pass layout, then the output set and the car scene's
// ray tracing set), the two compute pipelines (the trace in a plain and a multisampled variant), the control buffer and the list, the output descriptor set over the trace image, the governor and its two
// timestamps, the calibration's and the census's readback. ScreenReflections (ssr.cpp) owns the images (the trace image as a storage image, the confidence image) and the graphics side, and calls:
//
//   Initialize       after the car scene's and the scene service's (the ray tracing set exists): the layout and the shaders; the pipelines are made in the background when first wanted
//   BeginFrame       the effect's frame start: the debug word and the card's rung give the level (ssr_rt_rules.h Resolve), and the scene service is asked for the city's structures, hit data and
//                    textures (Consumer::kSsrFallback) in every frame the fallback is on, so the top level of the trigger has the city in it
//   PipelinesReady   the three pipelines this MSAA level needs are made (the first asking lists them with the host precache; a frame never waits for a compile)
//   Record           at the trigger, right after the trace pass: the select pass and the trace pass on the list, the trace image back to a shader read; false where the frame has no top level (the
//                    reason is counted and logged when it changes)
//   OnPresent        the frame boundary
//
// THE IDENTITY. On a card that does not trace rays, with REFLECTIONS ON SURFACES off or with mcla_gpu_ssr_rt off, none of this exists: BeginFrame says off, ScreenReflections makes no confidence image and no
// storage flags, draws the plain trace pass and records nothing here; the effect is what it was before the fallback.
//
// CHECK (nothing here ran on a device; ssr_rt_rules_test.cpp holds the rules and the shader texts, the shaders compile for DXIL and SPIR-V and validate): the output set's three views on D3D12 (the same
// counting as the city shadow pass's), a typed UAV load of RGBA16F on D3D12 (the trace image is read and written by the trace pass: TypedUAVLoadAdditionalFormats), the compute layout's seven sets on
// Vulkan (maxBoundDescriptorSets >= 8 on every desktop driver), the two timestamps (the governor has no signal where the device cannot write them: it holds the cell's cap), the cost.
//
// Threading: render thread only, except Initialize and Shutdown (the lifecycle thread, GPU idle and the FrameRing drained).
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "host_passes.h"
#include "ssr_rt_rules.h"
#include "ssr_rules.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;
class RtWorldFrame;

class ScreenReflectionsRt {
 public:
  ScreenReflectionsRt();   // (defined in ssr_rt.cpp with the destructor: Timer is incomplete here)
  ~ScreenReflectionsRt();
  ScreenReflectionsRt(const ScreenReflectionsRt&) = delete;
  ScreenReflectionsRt& operator=(const ScreenReflectionsRt&) = delete;

  // After the car scene's own Initialize (its descriptor set is in the layout) and before the host precache starts. Never fails the renderer: without the device's ray queries, the scene or a
  // shader, nothing is traced and the effect is what it was.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* car_scene, RtSceneService* service, RtWorldFrame* world);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  bool available() const { return ready_; }

  // The effect's frame start (ScreenReflections::BeginFrame): `ssr_level` the effect's level now. Reads the debug word, asks the scene service in a frame the fallback is on.
  void BeginFrame(ssr::Level ssr_level);
  bool wanted() const { return ready_ && resolved_.level != ssr_rt::Level::kOff; }
  const ssr_rt::Resolved& resolved() const { return resolved_; }

  // The pipelines of this MSAA level are made (listed with the precache the first time it is asked).
  bool PipelinesReady(bool multisampled);

  struct Frame {
    plume::RenderCommandList* list = nullptr;
    HostImage* trace = nullptr;       // RGBA16F storage image at the SSR size, left in a shader-read layout by the trace pass
    const HostImage* confidence = nullptr;  // R8G8_UNORM, the trace pass's second target, shader-read
    ssr::Push push;                   // the trace pass's push block (the road view's word and share set): the fallback adds its words to a copy
    uint32_t width = 0;               // the SSR images' size
    uint32_t height = 0;
    bool multisampled = false;
    uint32_t ssr_rays = 1;            // the rays a pixel the effect's own trace walked (ssr_rules.h NumbersOf): the fallback regenerates ray indices 0 .. min(its cell's, this) - 1
    bool screen = false;              // the picture's lin_depth level 0 and colour_post are in the push: a hit the picture shows takes its colour
    uint64_t frame = 0;               // the effect's frame number (the log)
  };
  // Records the select and trace passes. False: nothing was recorded (no top level in this frame, no set); `trace` is as it was.
  bool Record(const Frame& frame);

  // The trace image the output set names was made again or given back (ScreenReflections::EnsureImages, ReleaseImages): the set goes back now. EnsureSet compares the texture's POINTER, and a texture
  // made after the old one was freed can take its address: a stale set (a view of the destroyed image) would be taken for the new image's.
  void NoteTraceChanged() { RetireSet(); }

  // The frame boundary: idle bookkeeping.
  void OnPresent();

  uint64_t HeldBytes() const;
  // "; ssr rt: ..." for the renderer's census line, empty when the fallback was never wanted.
  std::string CensusText() const;

 private:
  enum Variant : uint32_t { kVSelect, kVTrace, kVTraceMs, kVariantCount };
  struct Timer;

  bool MakeLayoutAndShaders(Device& device, HostPasses& passes, CarRtScene& scene);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  plume::RenderPipeline* Pipeline(uint32_t variant);
  bool EnsureSet(const HostImage& trace);
  void RetireSet();
  void Log(const ssr_rt::FrameLine& line);
  void ProcessReadback(uint64_t generation, uint32_t rt_frame, uint64_t frame, const ssr_rt::FrameLine& base, bool stats_wanted);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* car_scene_ = nullptr;
  RtSceneService* service_ = nullptr;
  RtWorldFrame* world_ = nullptr;

  bool ready_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;   // u0 the trace image, u1 the control and statistics words, u2 the list
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, kVariantCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> listed_{};
  std::array<bool, kVariantCount> failed_{};
  std::unique_ptr<plume::RenderBuffer> ctl_;           // the control and statistics words (ssr_rt_rules.h buf::)
  std::unique_ptr<plume::RenderBuffer> list_;          // the candidates' pixels
  std::unique_ptr<plume::RenderBuffer> readback_;
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  const plume::RenderTexture* set_texture_ = nullptr;
  bool readback_busy_ = false;
  bool reset_pending_ = true;                          // the control words are cleared by the first frame's select pass
  uint64_t generation_ = 0;
  std::array<uint32_t, ssr_rt::buf::kStatWords> last_words_{};
  bool have_last_words_ = false;

  // the frame
  ssr_rt::Resolved resolved_;
  ssr_rt::Level logged_level_ = ssr_rt::Level::kOff;
  bool logged_level_once_ = false;
  uint32_t rt_frame_ = 0;                              // frames the passes were recorded (the counters' rotation, the tile order, the thinning)
  ssr_rt::Why last_why_ = ssr_rt::Why::kSsrOff;
  uint32_t why_logged_ = ~0u;
  uint32_t idle_frames_ = 0;
  bool ran_ = false;

  // the governor and its timer
  ssr_rt::Governor governor_;
  std::unique_ptr<Timer> timer_;
  double ms_ = -1.0;
  std::chrono::steady_clock::time_point clock_start_ = std::chrono::steady_clock::now();
  uint32_t logged_step_ = 0;

  // the census
  ssr_rt::Totals totals_;
  uint64_t last_log_frame_ = 0;
  ssr_rt::FrameLine last_line_;
  bool have_line_ = false;
};

}  // namespace mcla::gpu
