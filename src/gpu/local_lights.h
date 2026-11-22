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
// GB-H3 (local_lights_sources.h has the story and the budgets): the pass is also fed the police light bars (the game's own state of each lamp: its flash), the other cars' headlights the forward
// headlight stage does not light (small spot lights), the cars' brake / tail / turn / reverse lamps (small red and orange lights) and the lamps of the mods the camera is not over (outside the
// mod's rectangle no grid lights them); one budget a class and a tier (the light shadows' level words), 32 in all. The cars come from the light manager's car lights (the points of all 19 kinds;
// the manager is told the camera so that it keeps the cars nearest to it), the mods from mod_light_source.h's built sets.
//
// GB-H2 (headlight_pass_rules.h has the story, the A/B, the identity rule and the retirement gate): the frame's headlights, the lights of the cars the rig lights, are lit by this pass WITH THEIR SHADOWS:
// a light's ShadowRef (its views of the shadow atlas, one for each headlamp of a car the three level table gives views to) is read through the ONE lookup (hlsl/light_shadow_lookup.hlsli, the lit
// shaders' box kernel) from the frame's view table, which is written after the lights' rows of the buffer; the atlas is the bindless depth plane the replay recorded THIS frame, just before.
// mcla_gpu_headlight_pass (forward | pass | both) switches the forward headlight stage off and this pass on for them (the A/B), and the pass then lights what the forward stage leaves out: the
// 62 lit shaders it does not edit, the cars' paint, the pedestrians. The forward stage is deleted only after the owner has seen the pictures (the retirement steps are in headlight_pass_rules.h).
//
// WHAT IS NOT IN IT YET: the specular lobe, the city's lamps (they are the game's grid: LU-3's picker and GB-H4's shadow subtraction), neon (not in v1: the owner's question), shadows for the
// cars' other lamps (police, rear, mods: they have no views). It is a debug feature (settings below), off by default: the owner decides after THE LAMP A/B and the headlight A/B.
//
// THE LAMP A/B (local_lights_rules.h has the story): one mod lamp (lights.bin) through the game's grid (A), through this pass with the grid's copy OFF (B, mcla_gpu_local_lights_ab = 2), and through
// neither (the control, 1). The A/B's lamps are read from the mod loader's published set (mod_light_source.h) whatever the sources mask says; mod_lights.cpp builds the grid with them off.
//
// SETTINGS (settings_gpu.h, all debug only): mcla_gpu_local_lights (the pass, default off, live), mcla_gpu_local_lights_sources (a sum: 1 the frame's surface lights, 2 a test lamp, 4 police,
// 8 the cars' head lamps the forward stage does not light, 16 the cars' rear lamps, 32 the mods' lamps outside their grid; default 0), mcla_gpu_local_lights_ab (0 grid, 1 nowhere, 2 pass;
// restart), mcla_gpu_local_lights_gain (a multiplier, 1), mcla_gpu_local_lights_car_gain (the cars' lights' look, a multiplier, 1), mcla_gpu_headlight_pass (GB-H2: forward | pass | both, live;
// `pass` and `both` run this pass for the frame's headlights whatever mcla_gpu_local_lights says).
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
//   LL8  mod_lights.cpp could not be compiled here (no simde): the hook is five lines (GB-H3 adds two: the built sets' publish).
// GB-H3, nothing run (the run plan: h11-work\gb-h3-notes\RUNPLAN-gb-h3.txt):
//   LS1  THE POLICE KINDS: which of Police0 / 1 / 2 is red, blue or white, which side of the bar, the flash's rhythm and level (a police chase's [car-lights] census lines per kind say what
//        is on); kind 8 is a siren only away from the headlights (the probe read it beside the headlights of every ordinary car, on all night: local_lights_sources.h kPoliceNearHeadlamp).
//   LS2  the rear lamps: brake against tail glow, the turn signals' blink, a car's lamps as one light at their middle; the look (colour, I 2 to 3, reach 10 to 16 m) by eye.
//   LS3  the uncovered cars' head lamps: the strength (half the rig's) and the reach against the forward stage, the cars the rig does not light (a fifth car; HEADLIGHTS off), the aim of a car
//        whose frame is derived from its lamps (a traffic car's: the horizontal axis; kTrustOwnerlessDirection stays false).
//   LS4  the cost: every light the camera is inside the reach of is a full picture draw (ScissorOfLight); the class caps per tier (6 / 10 / 16 police ...) and the pass's 32 are numbers to read
//        from the pass timing "local lights" in a chase at 4K, 1080p, 720p.
//   LS5  the manager's car reference: the cars are those nearest to the camera of the last present (one frame old): a police car among more than eight cars in sight must be kept.
//   LS6  a mod lamp outside its rectangle: the pass lights it from any camera, and the grid takes over when the camera enters the rectangle: the pop at the border (the A/B holds the two
//        within 10 per cent), and a lamp inside the city's rectangle that is not in the city's grid.
//   LS7  the tier from the light shadows' level words (off and medium are the smallest): after LU-3 the same words are Level::kMedium / kHigh / kUltra.
// CHECK (GB-H2, nothing run; the run plan: h11-work\gb-h2-notes\RUNPLAN-gb-h2.txt):
//   HP1  THE UNITS FOR A HEADLIGHT: the pass multiplies the law's term by the exposure (2^bias, 16), the height factor and the gain, and not by the night light intensity (the headlight's colour has its
//        own night factor); the forward stage adds the term into the sum the lit shader multiplies by its albedo, the height factor and the exposure. They are the same only if the lit shaders
//        apply the exposure inside (LL1): the A/B's ratio says (near 16 or 1/16: the exposure is applied twice or not at all).
//   HP2  THE ATLAS READ IN THE PASS: the replay's record at the trigger leaves the atlas in the shader read layout for the pixel and compute stages (shadow_atlas.h EndRecord); the pass reads it through the
//        heap's set 0 (Texture2D<float4>, the depth plane viewed as one float): the debug layer and Vulkan validation clean (no SYNC-HAZARD with the volume's compute reads), D3D12 and Vulkan.
//   HP3  THE SAME MAPS: the forward stage reads the maps of the LAST frame (remapped to this frame's cars), the pass THIS frame's (the record is before it): a moving car's shadow edge differs by a
//        frame's motion between the two paths; the identity rule is judged on frozen cars (the scenes hold still) and the moving ones are the look, not the rule.
//   HP4  THE NORMAL: the forward stage lights by the geometric normal turned to the camera, the pass by the G-buffer's (the exported vertex normal; a derivative normal where there is none): a bumpy
//        surface and a two sided one differ; the surfaces whose G-buffer normal is the derivative one are the same as the forward stage.
//   HP5  SELF LIGHT: a car's own headlights light its own bonnet and the pass now reaches the cars (the forward stage never lit the player's paint): the near fade (0.8 m, +1.5 m) is the law's; a
//        replay map that holds the car's own body shadows its bonnet (CHECK: acne on the bonnet at ultra).
//   HP6  THE COST: one scissored draw per headlight (a spot of 70 m reach: a large rectangle) with the lookup (16 taps where the four outer taps disagree) for the lights with views (2 / 2 / 3 cars at
//        the three levels), and the G-buffer's planes while HEADLIGHTS is on (day included: CHECK whether they are to be wanted at night only).
//   HP11 THE OCCLUSION: the forward stage's light is in the main colour when the ambient occlusion applies (it can darken it in a crease), the pass adds its light AFTER the apply (nothing darkens
//        it): a difference of the two pictures in creases and under overhangs that is the pass's choice (the same as the lamp A/B's); the identity rule's per pixel share will show it.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "frame.h"
#include "headlight_pass_rules.h"
#include "host_passes.h"
#include "light_frame.h"
#include "local_lights_rules.h"
#include "local_lights_sources.h"
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
  bool wants_draws() const { return enabled_ && planes_wanted_; }
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
  float car_gain_ = 1.0f;
  larecomp::gpu::local_lights::Tier tier_ = larecomp::gpu::local_lights::Tier::kMedium;  // the budgets' tier: the light shadows' level
  larecomp::gpu::headlight_pass::Route route_ = larecomp::gpu::headlight_pass::Route::kForward;  // GB-H2: which of the two ways lights the frame's headlights (mcla_gpu_headlight_pass)
  bool keep_forward_ = true;  // GB-H2-FOLLOW: mcla_gpu_headlight_pass_keep_forward as last logged
  bool planes_wanted_ = false;  // the G-buffer's planes and the shared depth are wanted this frame (the setting is on, or the route's headlights are)

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
  larecomp::gpu::local_lights::Candidates candidates_;  // the cars' and the mods' lights of the frame (ranked, the unseen left out)
  std::array<uint8_t, larecomp::gpu::local_lights::kBufferAlloc> scratch_{};

  // the log
  std::array<uint64_t, size_t(Skip::kCount)> counts_{};
  uint64_t frames_run_ = 0;
  uint64_t lights_drawn_ = 0;
  uint64_t pixels_scissored_ = 0;
  uint64_t frames_own_pose_ = 0;
  std::array<uint64_t, larecomp::gpu::local_lights::kSourceClassCount> class_drawn_{};  // lights taken, by SourceClass
  std::array<uint64_t, larecomp::gpu::local_lights::kSourceClassCount> class_cut_{};    // wanted and cut by the class's budget or the pass's 32
  uint64_t cars_seen_ = 0;                                                               // sum over the frames of the cars the manager held
  uint64_t shadowed_lights_ = 0;  // GB-H2: lights drawn that had views of the atlas (the lookup ran for them), summed over the interval
  uint64_t shadow_views_ = 0;     // ... and the views of those lights
  uint64_t shadow_frames_ = 0;    // frames the table was written
  bool first_logged_ = false;
  std::array<uint32_t, 4> last_units_bits_{};
  bool units_logged_ = false;
  bool no_pipeline_logged_ = false;
};

}  // namespace mcla::gpu
