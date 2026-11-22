#pragma once
// ===========================================================================
// MCLA GPU renderer -- screen-space reflections (packages GB-I1 and GB-I2 of perf/LIGHT-UNIFY.md 11.2, perf/GBUFFER-SURVEY.md 4.2)
// ===========================================================================
// Reflections on the surfaces that show them in the screen's own picture: the wet road at night, the sidewalk, glass, metal, water. A Hi-Z ray march of the shared depth at HALF the picture's size, one
// GGX-sampled ray a pixel (two at ultra), the colour of what it hits read from the shared colour_post, an accumulation over frames by the virtual image's reprojection, and an additive resolve into the
// main colour at the picture's size. THE ROW (GB-J): REFLECTIONS ON SURFACES, GRAPHICS > REFLECTIONS (mcla_gpu_ssr: auto, off, high, ultra); OFF by default because `auto` is the GPU class's cell
// (gpu_class.h Effect::kSsr) and every cell is off until the owner has seen the pictures (the prepared cells are in gpu_class.h).
//   LEVELS      what each changes (ssr_rules.h NumbersOf): HIGH one ray a pixel, up to 40 Hi-Z steps, the history weight 0.90 (a mean of about 10 frames); ULTRA two rays a pixel (their mean: the trace
//               shader's), up to 64 steps, 0.92 (about 12 frames). The size is the same for both: the images are HALF the picture's (11.9 MiB at 1080p, 47.5 MiB at 4K), the
//               resolve into the colour is at the picture's size. The temporal part (clip, edge rule, vehicle rows) is part of both levels; the debug settings below tune it.
//
// WHERE THINGS COME FROM (all of it shared, nothing is made twice):
//   ScreenShared (screen_shared.h)  hiz_min (the min chain of the AO's linear depth: the walk), colour_post with its mip chain (the radiance of a hit: the effect's Want of it also turns the mips on),
//                                   the camera. The Want is (hiz_min, colour_post); the owner's `history` product (the last frame's colour_post) is not used: the accumulation keeps its own images,
//                                   because what it must carry over is the reflection, which colour_post (made before this pass) does not hold.
//   G-buffer (gbuffer.h)            RT1 the world normal, RT2 the albedo (a metal's F0), RT3 the smoothness, class and flags (GB-W2 writes it; GB-F1's class table says what reflects: the SSR flag).
//                                   RT3 is in the frame whenever this effect asks for the planes (GB-W2 is merged: gbuffer::kAllPlanes), so the class table decides. Only where a frame has no RT3 (or mcla_gpu_ssr_stub 1 forces it)
//                                   the STUB stands in (hlsl/ssr_common.hlsli): every pixel whose world normal points up and is no vehicle is a road of mcla_gpu_ssr_stub's smoothness, a debug aid for comparing;
//                                   the frame says "stub" in the log. The vehicle mark of the depth chain (a stencil ID) keeps the cars out in the stub.
//   the AO (ao.h FrameCameraNow)    the frame's own camera: the right and up axes (RT1 is a WORLD normal) and the motion into the last frame (the history). Without it (or without a camera it
//                                   trusts) the pass takes the last frame's anchor camera for the axes and runs with no history.
//
// THE PASSES (hlsl/ssr_trace_ps.hlsl, ssr_temporal_ps.hlsl, ssr_apply_ps.hlsl; the walk is hlsl/ssr_trace.hlsli, checked in ssr_test.cpp against analytic scenes and a brute force march), recorded at the
// trigger right after the colour capture (scene.cpp OnResolve; the SSR step of frame_order.h's table):
//   1 trace     half size, RGBA16F: per pixel the class gate and the smoothness (a pixel that reflects nothing costs the gate's reads), a GGX ray, the Hi-Z walk, the colour of the hit at the mip the
//               lobe's width asks for (the sky at the vanishing point where the ray ended on the picture), faded at the picture's edge and for a ray that points back at the camera; a = the distance
//   2 temporal  half size, RGBA16F x 2 (history ping-pong): the virtual image's reprojection, the receiver's surface test, the clamp to the neighbourhood's box, a = the receiver's depth
//   3 apply     the picture's size, into the main colour, additive, alpha untouched (or a debug view in its place): the class's gain, the environment BRDF's Fresnel, the four nearest texels of the
//               accumulated image weighted by depth agreement
// What no ray finds stays the game's own reflection (the road's planar view, the glass' env map); a car's paint and chrome never take the effect (the class gate), they keep their own.
//
// GB-I2 (the three parts that make it a finished effect):
//   TEMPORAL     the accumulation clips the history to the trace's neighbourhood (mean +- 1.25 sigma inside its min / max: a variance clip), reads no history at the picture's edge, and moves a REFLECTED
//                VEHICLE's image with the vehicle's own motion: the trace marks a hit on a vehicle's pixel with its stencil ID in the alpha, and the AO's vehicle table (VehicleTableNow: the very rows its own
//                history uses, vehicle_motion.h) gives that ID's 3x4 (hlsl/ssr_common.hlsli SsrVehicleImage). No row: the image is the world's and the history keeps little (so a car does not smear).
//   WET ROAD     the game's own scalar, VS c43.x (gReflectionParams.x: 0.095 dry, 1.17 in rain; the census says it is what the road shaders multiply their puddle mask and reflection by), as GB-I1 read it, now
//                smoothed over a second of TIME (not of frames), with a smooth ramp of the term over the first 0.2 of smoothness above the floor (a road that has just crossed it does not jump to a whole
//                reflection), and c43.y (the strength the game shows its road view with) next to it. Per-pixel puddles are the game's texture's: the effect has the wetness of the frame, not of the pixel.
//   FALLBACK     on a ROAD the game's own reflection (the 256 x 256 planar view of the whole screen, side_views.h) is already in the colour. The trace reads that view (the host target of its identity,
//                found by ScreenReflections::OnTrigger) at the pixel's screen position and brings the DIFFERENCE between the reflection a ray found and the view, so the sharp reflection replaces the
//                soft one (no doubling) and where the ray found nothing, left the picture or hit a back face the difference is 0 and the game's reflection stays alone (no seam). The share taken out is
//                mcla_gpu_ssr_fallback (1) times the strength the game shows the view with; 0 adds on top as GB-I1 did. A guard keeps a bad estimate from blacking the road. Other classes (glass, metal,
//                sidewalk): the game's environment map stays under the term, which is only faded (its dual paraboloid mapping is the game's own and not reproduced here).
//
// THE ORDER (LIGHT-UNIFY 5.3; the table is frame_order.h, GB-I3): after the light pass (GB-H1: step 7) and the colour capture (8), so that the reflection shows the lights; before the volumetric composite and
// the glows, which are drawn after the trigger. colour_post is made before the effect adds to the colour: a reflection is not a reflection of a reflection. Three neighbours, settled by GB-I3:
//   the car history   is taken AFTER this pass (as the design has it) and does not see it: it holds a vehicle's pixels only (hlsl/car_history_ps.hlsl) and this effect adds nothing to a vehicle's
//                     pixel (the class gate's vehicle flag, the stub's stencil mark), so what the history reads from the main colour above 1080 lines is the same either side of SSR;
//   the sun shafts    and the volumetric light's inject / integrate record BEFORE the capture (they write images of their own; the composites are at the end-of-scene copy, after this pass, in the play
//                     view; only the pause and map worlds and the debug switch composite the shafts at the trigger, where SSR does not run or reflects their light with the rest of the picture);
//   the wants         are declared at the very start of the frame, before the G-buffer and the screen owner read them (lag 0).
//
// WHEN IT RUNS (ssr_rules.h Decide): the setting, the pipelines made (in the background: a frame never waits), the play view's frame (not the pause menu's, the garage's or the map's world), the
// G-buffer's normal plane, the Hi-Z and colour_post (with its mips) in the frame, a size the rule knows, one host sample count for the depth and the planes. The reason is logged when it changes.
//
// SETTINGS (live): the row's mcla_gpu_ssr auto | off | high | ultra (a player setting now), and the debug-only mcla_gpu_ssr_view 0..6; mcla_gpu_ssr_strength; mcla_gpu_ssr_min_smooth; mcla_gpu_ssr_stub; mcla_gpu_ssr_wet; mcla_gpu_ssr_history;
// mcla_gpu_ssr_fallback (GB-I2); mcla_gpu_ssr_vehicle_rows (GB-I2).
//
// CHECK (nothing here was run against the game; ssr_test.cpp pins the maths, the shaders compile for DXIL and SPIR-V): the list is in the RUN PLAN of the commit and in notes\RUNPLAN-gb-i1.txt.
//
// Threading: render thread only, except Initialize / Shutdown (lifecycle thread, GPU idle).
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "host_passes.h"
#include "resolve.h"
#include "screen_frame.h"
#include "ssr_rules.h"
#include "temporal_identities.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class Device;
class FrameRing;
class GBuffer;
class PassTiming;
class ScreenShared;

class ScreenReflections {
 public:
  ScreenReflections() = default;
  ~ScreenReflections();
  ScreenReflections(const ScreenReflections&) = delete;
  ScreenReflections& operator=(const ScreenReflections&) = delete;

  // Makes the layout and the shaders (the pipelines are made in the background when first wanted). Never fails the renderer: without them nothing runs (logged).
  void Initialize(Device& device, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The owners of what the effect reads (not owned; null: the effect is off). The AO gives the camera (optional). Render thread, before the first frame; Shutdown stops this class first.
  void SetScreenShared(ScreenShared* screen) { screen_ = screen; }
  void SetGBuffer(GBuffer* gbuffer) { gbuffer_ = gbuffer; }
  void SetAmbientOcclusion(AmbientOcclusion* ao) { ao_ = ao; }

  // A frame's first list: reads the settings, declares what the effect wants of the shared products and of the G-buffer (ssr_rules.h WantedProducts / WantedPlanes: only while it is on; sticky; the
  // owners read them at their own frame start, which Scene::BeginFrame runs AFTER this call, so the wants take effect in this very frame: frame_order.h kBeginSsr, lag 0), forgets the frame's draws.
  void BeginFrame();
  // True while the effect is on: the Scene then tells it the main view's draws (the wetness).
  bool wants_draws() const { return level_ != ssr::Level::kOff; }
  // A prepared draw of the main view, before the trigger: its vertex constants' wetness (VS c43.x, gReflectionParams.x: 0.095 dry, 1.17 in rain) is a vote for the road's.
  void OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks);
  // The trigger's colour capture was just recorded (ScreenShared::CapturePost returned true): runs the passes. True when passes were recorded (the caller's list state is gone). Once a frame. `upscaler_running`: a temporal
  // upscaler works on this frame (the accumulation then takes no floor for its clip: hlsl/ssr_common.hlsli SsrClipKeepFor).
  bool OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool upscaler_running);
  // A guest present: the frame boundary (the history's ping-pong, the idle count).
  void OnPresent();

  // The video memory the images hold now.
  uint64_t HeldBytes() const;

 private:
  // The pixel shaders, in the order host_shaders.h ScreenReflectionsPixelShader numbers them.
  enum Shader : uint32_t { kShaderTrace, kShaderTraceMs, kShaderTemporal, kShaderApply, kShaderApplyMs, kShaderCount };
  // The pipelines: trace, temporal, then the apply pass for each host sample count (1, 2, 4, 8) and for the add and the view blend.
  enum Variant : uint32_t { kVTrace, kVTraceMs, kVTemporal, kVApplyBase, kVariantCount = kVApplyBase + 8 };

  const char* VariantName(uint32_t variant) const;
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  // The pipeline of a variant, null while it is being made (listed with the host precache the first time it is asked for) or when it could not be made.
  plume::RenderPipeline* Pipeline(uint32_t variant);
  static uint32_t ApplyVariant(uint32_t host_samples, bool view) {
    uint32_t log2 = 0;
    for (uint32_t s = host_samples; s > 1 && log2 < 3; s >>= 1) ++log2;
    return kVApplyBase + log2 * 2 + (view ? 1 : 0);
  }
  void Bind(plume::RenderCommandList* list) const;
  // The same sequence as HostPasses::DrawInto with this layout (128 bytes of push constants), into `image` at its own size.
  void DrawPass(plume::RenderCommandList* list, HostImage& image, const plume::RenderPipeline* pipeline, const ssr::Push& push);
  bool EnsureImages(uint32_t width, uint32_t height);
  void ReleaseImages();

  Device* device_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  ScreenShared* screen_ = nullptr;
  GBuffer* gbuffer_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;

  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, kShaderCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> listed_{};
  std::array<bool, kVariantCount> failed_{};
  bool ready_ = false;                // the layout and the shaders exist

  // the settings of this frame
  ssr::Level level_ = ssr::Level::kOff;
  ssr::Level logged_level_ = ssr::Level::kOff;
  bool logged_level_once_ = false;
  ssr::View view_ = ssr::View::kOff;
  float strength_ = ssr::kDefaultStrength;
  float min_smooth_ = ssr::kDefaultMinSmooth;
  bool force_stub_ = false;
  float wet_override_ = -1.0f;
  float history_override_ = -1.0f;
  float fallback_setting_ = 1.0f;     // GB-I2: mcla_gpu_ssr_fallback
  bool vehicle_rows_ = true;          // GB-I2: mcla_gpu_ssr_vehicle_rows

  // the images: the trace, and the accumulated image twice (this frame's output and the last frame's)
  HostImage trace_;
  std::array<HostImage, 2> accum_;
  uint32_t write_ = 0;                // the accum_ image this frame writes (the other one is the history)
  uint32_t size_[2] = {0, 0};
  uint32_t idle_frames_ = 0;          // frames since the effect last ran (the images go back after a while)
  uint64_t images_generation_ = 0;    // bumped when the images are made again (the history is gone)

  // the frame
  bool trigger_done_ = false;
  bool ran_ = false;                  // this frame recorded the passes
  ssr::WetVotes wet_votes_;
  float wet_ = 0.0f;
  bool wet_known_ = false;
  float wet_strength_ = 1.0f;         // GB-I2: VS c43.y, the strength the game shows its road view with (smoothed with the wetness)
  float wet_raw_ = 0.0f;              // the commonest VS c43.x of the last frame that had a vote (the log)
  std::chrono::steady_clock::time_point wet_clock_;   // the smoothing is in seconds
  bool wet_clock_valid_ = false;
  uint32_t road_logged_ = ~0u;        // the road view's last logged reason
  bool vehicle_table_logged_ = false;
  uint64_t vehicle_table_frames_ = 0;
  ssr::HistoryKey last_key_;          // the last frame that ran
  float last_tan_[2] = {0.0f, 0.0f};  // the lens it ran with
  uint64_t frames_ = 0;
  uint64_t frames_run_ = 0;
  uint32_t why_logged_ = ~0u;
  bool stub_logged_ = false;
  bool camera_logged_ = false;
  bool first_logged_ = false;
  uint64_t history_frames_ = 0;
  uint64_t no_motion_frames_ = 0;
  uint32_t size_logged_[2] = {0, 0};
};

}  // namespace mcla::gpu
