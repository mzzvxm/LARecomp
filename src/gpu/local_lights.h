#pragma once
// ===========================================================================
// MCLA GPU renderer -- the deferred local light pass (perf/LIGHT-UNIFY.md 6.2 and 11.2 GB-H1; perf/GBUFFER-SURVEY.md 4.1)
// ===========================================================================
// THE SKELETON of the deferred lights: at the game's mid-scene depth copy (the trigger), AFTER the ambient occlusion's apply and BEFORE the sun shafts, the volumetric light and the game's own light
// volumes and glows, one scissored additive draw per light adds the light to the main view's colour. A pixel takes `albedo x lit weight x LightTerm(light, surface)` where the surface is the
// G-buffer's (RT1 world normal, RT2 albedo and lit weight, RT3 class where the stage writes it) and the main depth, the camera the AO's trigger camera (the frame's own pose where the vertex
// constants hold it), the light a row of the frame's GpuLight buffer and the term hlsl/light_laws.hlsli's law: the city's units, so a lamp added here is the lamp the game's grid draws.
//
// WHAT IT NEEDS, and what it makes the others do: the G-buffer's normal and albedo planes (GBuffer::Want(kLocalLights): the sidecar is made only while this runs), the shared screen products' camera
// (ScreenShared::Want(kLocalLights, lin_depth): the AO runs its depth chain for it when it is off), and, with the frame's lights as a source, the light manager (LightManager::Want). Nothing
// is made, bound or drawn while mcla_gpu_local_lights is off: the G-buffer has no consumer from it, the manager no consumer, the scene's draws are the game's.
//
// WHAT IS NOT IN IT YET: shadows (GB-H2: the atlas read through the one lookup and the forward headlight's A/B and retirement), the specular lobe, the city's lamps (they are the game's grid: LU-3's
// picker and GB-H4's shadow subtraction), police and mod lights outside the city (GB-H3), neon (not in v1: the owner's question). It is a debug feature (settings below), off by default: the owner
// decides after THE LAMP A/B.
//
// THE LAMP A/B (local_lights_rules.h has the story): one mod lamp (lights.bin) through the game's grid (A), through this pass with the grid's copy OFF (B, mcla_gpu_local_lights_ab = 2), and through
// neither (the control, 1). The A/B's lamps are read from the mod loader's published set (mod_light_source.h) whatever the sources mask says; mod_lights.cpp builds the grid with them off.
//
// SETTINGS (settings_gpu.h, all debug only): mcla_gpu_local_lights (the pass, default off, live), mcla_gpu_local_lights_sources (1 the frame's surface lights, 2 a test lamp; default 0),
// mcla_gpu_local_lights_ab (0 grid, 1 nowhere, 2 pass; restart), mcla_gpu_local_lights_gain (a multiplier, 1).
//
// BINDING: its own pipeline layout (the host passes' four bindless texture sets at spaces 0-3, the buffer of the header and the lights at (t0, space4), the push block at (b0, space5)), a buffer and
// a descriptor set per frame in flight, written at the trigger. The pipelines (one per host sample count, RGBA16F colour, blend ADD) are made on first use: the first frame with the pass on stutters.
//
// Logs (info, "[local-lights]"): the first frame the pass runs (lights, size, samples, the camera used, the units), every 600 frames the frames run, the lights drawn and why frames did not run
// (no G-buffer, no camera, no lights, no pipeline), and a line when the units' vote changes.
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread (GPU idle).
//
// THE RUN PLAN (the main session; this agent ran nothing, no game, no exe): h11-work\gb-h1-notes\RUNPLAN-gb-h1.txt. Unit first: larecomp_local_lights_test (7419 checks) and the neighbours'.
// CHECK (nothing here could be run; each is a line of the run plan):
//   LL1  THE UNITS: the lamp A/B decides  night (PS c63.w) x height factor (PS c56) x exposure (2^bias = 16) x gain  (local_lights_rules.h): a ratio near 16 or 1/16 is the exposure, one that follows
//        height is the factor, a constant one is night or the gain; the registers' numbers are read from xCityDefault::PSMultiLight and the vote takes the value most colour draws carry.
//   LL2  RT2 holds the albedo BEFORE the game multiplies the vertex colour's alpha (the baked occlusion) into it: the deferred lamp is brighter than the grid's in creases and under overhangs.
//   LL3  the cone ramp (smooth step in the cosine) and N.L of light_laws.h against the game's own lamp (survey C8), and the grid's quantisation of position and colour.
//   LL4  D3D12 until Plume's per target blend fix: a draw that writes no plane leaves the plane's old data under it (gbuffer.h), so the pass can light a pixel with a stale surface: judge the A/B on
//        Vulkan first. The roads, terrain and the families with no albedo take no light (RT2 = 0).
//   LL5  the layout (four texture sets, a raw buffer set at space 4, the push block at space 5) and the upload buffer as a raw SRV on both back ends; the main depth read in the shader-read layout
//        after the AO's apply; the debug layer and Vulkan validation clean.
//   LL6  the camera: the frame's own pose (AmbientOcclusion::TriggerViewInverse) against the last frame's: no swim of a pool at speed; the interval line counts the frames with the own pose.
//   LL7  the mod grid replaces the city's while the camera is over its rectangle, and a lamp written OFF with no intensity lights nothing in the game's builder (the control run C is the proof).
//   LL8  mod_lights.cpp could not be compiled here (no simde): the hook is five lines.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "frame.h"
#include "host_passes.h"
#include "light_frame.h"
#include "local_lights_rules.h"
#include "resolve.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class Device;
class FrameRing;
class GBuffer;
class LightManager;
class PassTiming;
class ScreenShared;

class LocalLights {
 public:
  LocalLights() = default;
  ~LocalLights();
  LocalLights(const LocalLights&) = delete;
  LocalLights& operator=(const LocalLights&) = delete;

  // Never fails the renderer: the layout, shaders and pipelines are made when the setting is first on.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The owners of what it reads (not owned; null: that part is missing and the pass does not run).
  void SetGBuffer(GBuffer* gbuffer) { gbuffer_ = gbuffer; }
  void SetScreenShared(ScreenShared* screen) { screen_ = screen; }
  void SetAmbientOcclusion(AmbientOcclusion* ao) { ao_ = ao; }
  void SetLightManager(LightManager* manager) { manager_ = manager; }

  // The Scene opened a frame's first list, BEFORE the G-buffer's and the shared products' own BeginFrame (they read the wants it makes): reads the settings, makes the wants and forgets
  // the last frame's votes.
  void BeginFrame();
  // True while the pass is on: the Scene feeds it the main view's draws.
  bool wants_draws() const { return enabled_; }
  // A prepared draw of the main view (`colour`: it writes the main colour) with its constant banks, before the draw is recorded: votes the frame's units (PS c63.w and c56). Until the trigger.
  void OnMainDraw(const ConstantBanks::Banks& banks, bool colour);
  // At every resolve, after the AO's apply: `gbuffer_published` is the G-buffer's own answer for this resolve (it was the trigger and the planes were published). Records the pass then and
  // returns true (the caller's list state is gone). `frame_slot` is the frame's slot of the upload ring and of this class's buffers.
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot);
  // A guest present: the interval line.
  void OnPresent();

  uint64_t HeldBytes() const;

 private:
  enum class Skip : uint32_t { kNoPlanes, kNoCamera, kNoTarget, kNoLights, kNoPipeline, kNoBuffer, kRecorded, kCount };
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };

  bool EnsureResources();
  bool EnsureSlot(uint32_t frame_slot);
  plume::RenderPipeline* PipelineFor(plume::RenderFormat format, uint32_t samples);
  void Count(Skip why) { ++counts_[size_t(why)]; }
  void LogInterval();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  GBuffer* gbuffer_ = nullptr;
  ScreenShared* screen_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  LightManager* manager_ = nullptr;

  // the settings of this frame
  bool enabled_ = false;
  uint32_t sources_ = 0;
  larecomp::gpu::local_lights::Ab ab_ = larecomp::gpu::local_lights::Ab::kGrid;
  float gain_ = 1.0f;

  // resources
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 2> shaders_;  // one sample, multisampled
  struct PipelineEntry {
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t samples = 1;
    std::unique_ptr<plume::RenderPipeline> pipeline;
  };
  std::vector<PipelineEntry> pipelines_;  // one per (colour format, host samples) met: in practice RGBA16F at the host's MSAA level
  bool resources_failed_ = false;
  bool pipelines_failed_ = false;
  std::array<Slot, kFramesInFlight> slots_;

  // the frame
  larecomp::gpu::local_lights::UnitsVote votes_;
  bool trigger_seen_ = false;
  uint64_t frame_ = 0;
  std::vector<larecomp::gpu::Light> extras_;
  std::array<uint8_t, larecomp::gpu::local_lights::kBufferAlloc> scratch_{};

  // the log
  std::array<uint64_t, size_t(Skip::kCount)> counts_{};
  uint64_t frames_run_ = 0;
  uint64_t lights_drawn_ = 0;
  uint64_t pixels_scissored_ = 0;
  uint64_t frames_own_pose_ = 0;
  bool first_logged_ = false;
  std::array<uint32_t, 4> last_units_bits_{};
  bool units_logged_ = false;
  bool no_pipeline_logged_ = false;
};

}  // namespace mcla::gpu
