#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's self-reflection, ray traced
// ===========================================================================
// The screen-space self-reflection (car_selfrefl.h) marches a ray through an image of the car as the camera saw it: a part the
// camera does not see cannot be reflected, a thin part is guessed from a few points. On a device that can trace rays this pass
// replaces the march for the player's own car with the answer from its triangles (car_rt_scene.h: one top-level structure of the
// car in the frame's view space, built at the history trigger), keeping the same picture of the car (the history image) for the
// COLOUR of what the ray hits.
//
//   mcla_gpu_car_selfrefl_rt          auto | off. auto (the default) is on where the device can ray trace the car (CarRtSupported: D3D12
//                                     with DXR 1.1, Vulkan with VK_KHR_ray_query) and the car self-reflections (mcla_gpu_car_selfrefl) are on; off keeps the
//                                     screen-space march everywhere. Live: it applies from the next frame. Where the device cannot,
//                                     auto is the march, so nothing changes for it.
//   mcla_gpu_car_selfrefl_rt_length   a debug override of the reflection ray's reach in metres for the player's OWN parts (4 by default; the weight of a hit falls to
//                                     nothing from 0.6 of it). A hit on ANOTHER car has a reach of its own, the class's (18 m on the High class, 12 on the Mid class:
//                                     mcla_gpu_car_rt_hit_reach moves it); the ray is cast as far as the longer when it may hit another car (car_rt_hit_rules.h CastReach).
//   mcla_gpu_car_rt_clearcoat         a debug override of the clear-coat ray (car_rt_hit_rules.h): one ray along the exact mirror direction beside the cone, added to it by a share
//                                     that follows the paint's fresnel; -1 (the default) by the class (High on, Mid off), 0 off, above 0 on at that strength
//   mcla_gpu_car_selfrefl_rt_parts    a debug override of the parts the reflection ray hits: a mask of car_rt_common.hlsli's
//                                     CAR_RT_MASK_* (1 body, 2 glass, 4 tyres, 8 the driver, 16 the other cars; 23 by default)
//   mcla_gpu_car_selfrefl_rt_gloss    a debug override of the half angle, in degrees, of the cone the rays are spread over (2.5 by default; 0 = a
//                                     perfect mirror, one ray: the first version's razor edge, for a picture to compare with)
//   mcla_gpu_car_selfrefl_rt_denoise  a debug override of the passes of the filter that takes the cone's grain out of the image before the paint
//                                     reads it (2 by default; 1 one pass; 0 none: the raw trace, for a picture to compare with). See DENOISE.
//   mcla_gpu_car_rt_coat_layer        a debug switch (RT-HIT-SHADING-4, on by default): the paint adds the clear coat's layer over the reflection of another car (the image's sign says which
//                                     texels: car_rt_hit_rules.h CoatLayerAmount; the variant's CarReflCoat)
//   mcla_gpu_car_selfrefl_view = 5    the debug view of this pass's image at the paint's pixel (car_selfrefl.h): purple no image this frame,
//                                     cyan the image has no data there (the march ran), green the ray hits nothing of the car, yellow a hit
//                                     on the player's own part, orange a hit on another car (the sign of the code; before RT-HIT-SHADING-4: on a part the camera does not see),
//                                     brighter the heavier the weight; red: the shaded point is not in the history (as view 4). View 2 is this pass's colour alone at full intensity
//                                     (no weight, no fresnel); 6 the clear coat's layer's amount, 7 what it adds
//
// OTHER CARS: the scene (car_rt_scene.h) also holds the cars driving alongside, so the paint and the chrome reflect an opponent or a car of the traffic beside
// the player's car. The reflection ray hits them (the parts' mask, 16); the primary ray never does, so the effect stays the player's own car's. What a hit on
// another car shows is the history's colour where the camera sees that part, and where it does not (the car's far side) the colour the camera sees of THAT
// car: the mean of the history's vehicle texels around its body centre, darkened by the same shade (car_rt_refl_rules.h FindOtherCarColour says why that and
// not a paint constant). mcla_gpu_car_rt_cars 0 takes them out of the scene (the player's car alone, as before), mcla_gpu_car_selfrefl_rt_parts 7 leaves
// them in the scene and out of the reflection.
// A HIT ON ANOTHER CAR'S PART THE CAMERA DOES NOT SEE IS SHADED FROM THE CAR'S OWN MATERIAL (RT-HIT-SHADING, car_rt_material_rules.h has the design, the record, the arithmetic and
// the test plan): its paint colour (the gradient of the draw's own constants, the livery over it), its diffuse texture at the mip level of the cone's footprint, the ambient the
// cars' draws saw and the sun (N.L and one visibility ray), the game's environment map by the fresnel, the lamps' glow at night. It replaces the stand-in colour described above
// wherever the instance has a material; the history's colour is still the answer where the camera sees the part, and the two blend by the share of the hit the history shows.
// mcla_gpu_car_rt_hit_shading (debug) is the A/B (off: the stand-in), mcla_gpu_car_rt_hit_gain, _ambient and _spec move the brightness.
// RT-HIT-SHADING PART TWO (car_rt_hit_rules.h has the design, the numbers and the cost): the reach to another car (15 to 20 m, the class's level), the sharp clear-coat ray, the game's sun
// cascades at the shaded hit (the frame rows of binding 5; Scene hands them over from the volumetric light's collector: CarRtScene::SetSunCascades) and the level table (High everything;
// Mid a shorter reach, the cone's outer pair, no clear coat).
// THE OPPONENTS' OWN PAINT (car_rt_opp_refl.h): the pass below is the player's car's. The paint of another car (an instance with CAR_RT_MASK_OTHER_PAINT, the draws of the customisable paint's
// shader) is traced by a second dispatch of the same shader file (the variant hlsl/car_rt_refl_opp.hlsli) that CarRtRefl::OnHistory records right after this one, into the same images: the paint
// variant reads an opponent's texel like the player's, so its door shows the cars round it too. mcla_gpu_car_rt_opp_refl (debug) is the A/B.
//
// OTHER CARS' CHROME (car_chrome_rt_rules.h, which has the design, the cost and the test plan): the primary ray of the pass also finds the chrome parts of the other
// cars (tailpipes, badges, trim: the instances of their draws of xChrome::PS_Chrome carry the extra bit CAR_RT_MASK_OTHER_CHROME) and for those texels the pass traces
// the chrome variant's two short probes as rays, writing a texel of a kind of its own (alpha 1 + the reflection probe's weight, red -(1 + the sky probe's weight)) that
// the chrome variant (version 2) reads in place of its march. The paint never takes the other cars' own paint from here: it has no data there, as before.
// mcla_gpu_car_chrome_rt_others (debug) is the A/B; it needs mcla_gpu_car_chrome_occlusion. Nothing changes for the player's own car, nor on a device that cannot trace.
//
// THE CITY (CITY-RT M2 R1 and R2; city_refl_rules.h has the design, city_refl.h the host, hlsl/city_refl.hlsli the shader): with mcla_gpu_rt_reflections not off (auto is high on a strong ray tracing card) the
// reflection ray's parts have the city's bit (0x80) and a ray that finds none of the cars' parts goes on into the city's structures in the same top level: its hit is the picture's own colour where it is on
// the screen (colour_post and lin_depth at the trigger) and is shaded from its material where it is not (the city hit library), by the fade of its distance over the level's reach (HIGH 150 m, ULTRA 1,000 m).
// The image's codes are the same, the paint's variant is unchanged. The cast length, the cone's rays and the opponents' budget follow the level's cell; the set 3 of the layout carries the city's rows and counters
// and the push block is in space 4. The `[city refl]` census lines are CityRefl's.
// RR-4b, THE RAW MODE (car_rt_refl_rules.h DenoisePassesFor / RawHitDistance; dlss_rr_rules.h RawModes; mcla_gpu_dlss_rr_car_refl is the A/B). In a frame DLSS Ray Reconstruction takes it is the
// denoiser, so the filter below does not run: the paint reads the trace's raw image (the cone's grain goes to Ray Reconstruction with the rest of the picture's noise), and one more pass
// (hlsl/car_rt_refl_hit_cs.hlsl) writes how far each reflection went into an R16_FLOAT image of the picture's size, which the guide pass reads as the hit distance (RawHitImage; rr_guides.h
// Inputs::hit[1]). The trace's image is the paint's of the NEXT frame (reprojected), so the colour Ray Reconstruction sees on the car is last frame's reflection; the distance is this frame's,
// in this frame's pixels. Asked for through HistoryInfo::raw; with the plan not RR the pass is what it was: the same dispatches, shaders and pipelines (the hit pass is made, in the background,
// only for a frame that is asked).
//
// WHAT IT DOES, at the same trigger as the history image (CarSelfRefl::RecordHistory, right after it): one compute pass
// (hlsl/car_rt_refl_cs.hlsl, cs_6_5, inline ray queries) with a thread for every texel of the history image. A texel that holds a
// vehicle's pixel casts a primary ray from the camera against the car's opaque parts in a narrow window around the pixel's distance: a hit
// there is the player's car at that pixel, with its exact point and its mesh normal. From it a CONE of rays goes against body, glass
// and tyres (car_rt_refl_rules.h: 4 rays over a cone of mcla_gpu_car_selfrefl_rt_gloss degrees, a pattern turned by a different angle at each
// texel and, with a temporal upscaler, each frame, so that the weight of a texel is the share of the cone a part covers: the edge of a
// reflected part is as soft as the cone is wide where it is, sharp near and soft far, like a glossy paint's). Each hit is taken into the
// same frame's history image, where the colour of the part is if the camera sees it (the texels around the hit show a surface at its
// distance), else a stand-in (the pixel's own surface's mean colour in shade, nearly the whole weight: car_rt_refl_rules.h says why). A
// ray that hits the panel it left (it ran under its own triangle's plane: the grazing self-hit) counts as a miss.
// The output image is the history's size (R16G16B16A16_FLOAT, rgb colour, alpha the code and the weight: car_rt_refl_rules.h) and is for
// the NEXT frame's paint draws, as the history is: the paint variant (version 6) reads it at the shaded point's place in the history with a
// 3x3 quadratic B-spline of the texels that are the paint's own surface (the history's agreement test), in place of the march; where it has no
// data there (alpha 0: not the player's car by the triangles, another vehicle, a pixel whose primary ray found no surface at that distance)
// the march answers as before.
//
// DENOISE (hlsl/car_rt_refl_denoise_cs.hlsl, car_rt_refl_rules.h "The denoise" has the why, the numbers and the test): every texel's pattern of 4 rays is
// turned by a different angle, so in the band where a reflected part's edge is the texel's weight is one of 0, 1/4 .. 1 almost at random: grain, which
// the paint's 3x3 B-spline averages only about 2.8 texels' worth of and, with no upscaler, nothing averages over the frames. After the trace, up to two
// edge-aware a-trous compute passes (taps 1 then 2 texels apart, a 3x3 kernel; image -> scratch -> image) take it out: a tap counts only where it holds
// data (a miss counts as weight 0), is the same instance and the same surface (the history's distance within the tolerance, the slope from N.V); the
// colours are averaged premultiplied by the weights; and how much of each pass is applied follows the cone's width at its hit in texels (the trace's aux
// image, RGBA8: hit distance, instance, N.V; 2 distance tan(half angle) / texel): nothing under 0.4 texels (a part a few centimetres off the panel has a
// binary edge: nothing to take out, nothing to blur), the first pass in full from 2.5 texels, the second (wider) from 10. On the pure model of a
// straight penumbra (the unit test) the grain along the edge falls 2 to 7 times (to under 0.011 of the weight) and the edge's ramp grows by at most a
// fifth, the 50% point stays where it was to 0.05 texels. The paint's variant is unchanged (the image has the same codes and the same size: version 6).
// A TEMPORAL accumulation was weighed and left out at first (car_rt_refl_rules.h says why: the reflection moves with the view, so a reprojected texel holds the
// last frame's direction and an accumulation over a few frames trails the edge; its rejection rule cannot see that, and the cut, pause, garage and
// no-motion frames each need their own; with an upscaler the upscaler is the temporal filter and the pattern turns by the frame). NOISE-RESOLVE (noise_resolve.h) added a third pass
// for the frames WITHOUT an upscaler: the pattern turns by the frame there too and the finished image is averaged with the last frame's result texel for texel, the history clamped to the
// finished image's 3 x 3 neighbourhood (which sees a penumbra that moved and bounds the ghost to about a texel); two more images of the image's size while it runs (mcla_gpu_noise_resolve_no_upscaler).
// The passes are
// skipped for a gloss of 0 (one ray: no noise), for the setting 0, and for the frames the denoise's pipeline or images are not ready (the raw image
// is read then). One pass leaves its result in the scratch image, which the paint then reads (SlotFor); two leave it in the trace's own (the history pass, where it runs, leaves it in its own image).
//
// RESOLUTION (decided with numbers): the image stays at the history's size (half of the picture at 4K: 1920x1080), not the picture's. A
// 4K picture has about 215k vehicle texels at half size and 860k at full: with a primary ray and a cone of 4 that is 1.1 M against 4.3 M rays
// (the second would cost about 1 to 1.6 ms, the first 0.25 to 0.45, see COST), and the image 66 MiB instead of 16. The cone already gives every
// texel four samples of the reflection, one for each picture pixel of the 2x2 block it stands for, so the ray density per pixel is the first
// version's at full size; the blocks the first version showed were its single binary ray per texel (a hit or a miss: an edge one
// texel wide, 2 pixels at 4K, stepping at every texel), which the cone's weight ramp (a hand's width wide at 1 m) and the paint's smooth 3x3
// reconstruction (no corner at a texel's edge) take away. The history's colour is not mipped: with the cone's four rays each reading the
// 2x2 footprint of its own hit, the colour of the cone is the mean over four points spread across it (a footprint growing with the hit's distance
// times the cone's angle, by supersampling), and the paint's 3x3 averages the texels' different turns of the pattern.
//
// TIMING: the car is drawn before the world, so its own paint cannot see the frame's triangles; the pass runs at the trigger (every car
// draw of the frame is in) and its image is used by the next frame, exactly as the history is, with the vehicle's motion and the
// camera's reprojected in the paint's matrix. CarRtScene must have built this frame's top level on the list BEFORE this pass (it is
// asked every frame this is on: Request).
//
// COST (an estimate, not measured: CHECK on the first run, the pass is timed as "car rt refl" by PassTiming): the pass reads the history
// image once for every texel and writes the output image once (2.1 M texels at 4K with the half-size image: about 0.03 ms); the vehicle's texels
// (about 215k of them at 4K in a chase view, a tenth) trace a primary ray and 2 to 4 cone rays (2 where the outer pair finds nothing: the
// roof, the bonnet, the sides, most of the car; 4 where a part is near), so 0.65 to 1.1 M rays of at most 4 m against a tree of 25 instances,
// at an assumed 3 to 5 G rays/s for rays this coherent and this short on an RTX 5080 class card (inline ray queries from compute, no shader
// reordering): 0.15 to 0.35 ms, plus about 25 history reads a texel for the hits' colours and the surface mean (5 M loads, a tenth of a
// millisecond at most) = 0.25 to 0.45 ms. It is on top of the history pass. Memory: one more image of the history's size (8 bytes a texel: 16 MiB
// at 4K with half-size history) while the effect is on; it goes back after 2 s without it, with the history. The denoise adds a scratch image of
// the same size (16 MiB at 4K) and the aux image (4 bytes a texel: 8 MiB), so 33 MiB in all instead of 16, in the same lifetime.
// DENOISE COST (an estimate, not measured: PassTiming's "car rt denoise"): the two dispatches run a thread for every texel of the image (2.1 M at
// 4K with the half-size image), which for a texel with no data reads one value and writes it (a tenth of the image is a vehicle's, the waves with
// none leave at once); a vehicle texel (about 215k) does 9 taps of 3 loads (the image, the history's distance, the aux) and about 150 operations:
// 215k x 27 x 2 passes = 11.6 M loads, nearly all from the cache (8 x 8 groups, the taps 1 and 2 texels apart), about 0.02 to 0.03 ms on an RTX 5080
// class card; the early outs about 0.01 ms; three barriers' drain of a few microseconds each: 0.04 to 0.07 ms, under the 0.1 ms asked. A weaker
// ray tracing card (an RTX 2060, an RX 6600 class) is 3 to 4 times that, 0.15 to 0.25 ms; the image is about 1080 rows on every card (the half-size
// history at 4K, the full-size at 1080p) so the work does not grow with the output's size.
//
// CHECK (the first run looks at these, view 5 and the log's "[car rt refl]" lines first):
//   1. the cost: the pass at 4K with the half-size history and at 1080p with the full-size one; with mcla_gpu_car_selfrefl_rt_gloss 0 (one ray)
//      the pass is the first version's cost, so the difference is the cone's.
//   2. the cone's width: 2.5 degrees is a guess at the paint's gloss. The paint's own constants (DL0.w, the specular power) were not logged for the
//      player's paint (perf/out/G8A.txt has only other shaders' groups); 2.5 degrees is the blur of the environment map's own sharp read (a 512x256
//      dual paraboloid: 0.7 degrees a texel at the middle of a hemisphere, 1.4 at its rim, bilinear), so a reflected part is as soft as the sky
//      in the same paint. Wider (6) is hazier, 0 is the razor.
//   3. the pattern: its noise should be invisible at 4K (the denoise, and the paint's 3x3 over its result); where an upscaler runs it turns by the
//      frame, where none does it stands still. Look at the trunk lid under the wing at gloss 2.5 and 6 with mcla_gpu_car_selfrefl_rt_denoise 0 (the
//      raw trace: the grain of the last run), 1 and 2, no upscaler, 4K and 1080p: the grain should be gone at 2, the edge as glossy as at 0 (not
//      blurrier). If the edge is too soft raise car_rt_refl_rules.h kDenoiseSecondFrom/To (the second pass) or kDenoiseFootprintTo; if grain
//      remains lower them; if it still shows the cone's rays can go to 8 (kConeRays and the shader's loops), or the cone's early out can go.
//   3b. the denoise's own things: the aux image's RGBA8 UAV store and the SRV read of it through the bindless heap (a Texture2D<float4> view of an
//      R8G8B8A8_UNORM texture), the barriers between the passes (image <-> scratch: no flicker of old frames at the car's edge), the cost
//      ("car rt denoise" in the pass timing against the estimate above), and the pause view (the car stands still there: the pattern too).
//   4. the hidden-hit stand-in (the surface's mean colour x 0.4 at 0.85): the wing's underside on the lid should be a smooth dark band, no patches.
//      The weight and the shade are push constants (tune.x, tune.y) and constants of the rules header.
//   5. the self-hit rule (instance and facing, IsSelfHit): a lid or a bonnet that reflects nothing must stay the environment's colour at grazing
//      angles (view 5: no yellow on a flat panel). The exact test needs the triangles' positions (a fifth binding of CarRtScene's set, t4, and
//      the geometric normal of the committed triangle): not done, CarRtScene is another branch's.
//   6. the units of the history's colour against the paint's (the exponent bias gain), the reflection ray's reach (4 m;
//      mcla_gpu_car_selfrefl_rt_length) and the parts it hits (mcla_gpu_car_selfrefl_rt_parts; glass in by default).
//   7. the names of CarRtScene's per-frame request (Request, written as CarFrame's), View(), SetDesc() and the generated shader headers
//      (car_rt_refl_cs.hlsl.<dxil|spirv>.h, host_shaders.h CarRtReflShader) that larecomp_gpu_host_shader_rt makes: the first game run
//      linked the DXIL ones (the census counted ray traced passes), so this is for a new merge only.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipeline is made ahead on the host builder
// thread where the effect is on at the start (host_precache.h), and in the background where it is switched on later: the pass is not
// recorded until it is made (a frame never waits for a compile).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "car_rt_denoise_v2.h"
#include "car_rt_glass_refl.h"
#include "car_rt_hit_rules.h"
#include "car_rt_opp_refl.h"
#include "car_rt_parked_paint_rules.h"
#include "car_rt_paint_tiles.h"
#include "car_rt_refl_rules.h"
#include "car_selfrefl_rules.h"
#include "city_refl.h"
#include "host_passes.h"
#include "noise_resolve.h"

namespace mcla::gpu {

class BindlessHeap;
class CarRtScene;
class Device;
class FrameRing;
class PassTiming;
class RtSceneService;

class CarRtRefl {
 public:
  CarRtRefl() = default;
  ~CarRtRefl();
  CarRtRefl(const CarRtRefl&) = delete;
  CarRtRefl& operator=(const CarRtRefl&) = delete;

  // After the CarRtScene's own Initialize (its descriptor set exists): the device's answer (CarRtSupported), the layout, the compute
  // shader, and the pipeline's place in the host precache where the car self-reflections are on at the start
  // (`reflection_on_at_start`: CarSelfRefl::ListedAtStart). Never fails the renderer: without them nothing is traced. `scene` may be null.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* scene,
                  bool reflection_on_at_start);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // The frame's start (CarSelfRefl::BeginFrame): reads the setting. `reflection_on` is whether the car self-reflections are on. When the
  // pass is wanted and can run, asks the scene for this frame's structures.
  void BeginFrame(bool reflection_on, bool garage);
  // True while the pass is wanted this frame.
  bool active() const { return active_; }

  // What the history pass says about the image it just made (CarSelfRefl::RecordHistory).
  struct HistoryInfo {
    HostImage* image = nullptr;     // R16G16B16A16_FLOAT, left readable by the history pass
    uint32_t tile_slot = kInvalidBindlessSlot;
    bool parked_history = false;    // parked marks are outside the stencil tile mask
    uint32_t divisor = 1;           // the picture over the image, 1 or 2 (car_selfrefl_rules.h HistoryDivisor)
    uint32_t render_width = 0;      // the picture in host pixels
    uint32_t render_height = 0;
    double tan_x = 0.0;             // tan of half the horizontal and vertical field of view the history was linearised with
    double tan_y = 0.0;             //   (1 / |P00| and 1 / |P11| of the previous frame's projection, as the AO takes them)
    uint64_t frame = 0;             // the guest frame the history is of (what SlotFor is asked with)
    bool garage = false;
    bool reset_history = true;
    bool upscaler = false;          // a temporal upscaler works on this frame: the cone's pattern turns by the frame (it averages the pattern out)
    uint32_t pattern_phase = 0;     // 0 .. 1023: the pattern's step for this frame (the scene's noise index): used while an upscaler runs or the pass's own history does (NOISE-RESOLVE: OnHistory says which); else the pattern stands still
    bool raw = false;               // RR-4b: DLSS Ray Reconstruction takes this frame and this effect's switch is on (TemporalSystem::RawModes().CarRefl()): no filter, the hit distance written
    int32_t exp_bias = car_selfrefl::kMainTargetExpBias;  // the main colour target's exponent bias: the history's colour is the shaders' output times 2^exp_bias; the pass's own colours
                                                          // (a car's paint, the grey of a remembered car) are in the paint's units and are multiplied by 2^exp_bias (car_selfrefl_rules.h ExpBiasUnits)
    CityRefl::ScreenIn screen;                            // CITY-RT M2: lin_depth and colour_post's chain as the screen owner published them this frame (the city hits' screen shortcut); invalid: none
    uint32_t colour_samples = 1;                          // CITY-RT M2 R3b: the main colour target's host samples (the glass's additive pass has a pipeline for each)
  };
  // Records the pass on `list` (outside any render pass, at the trigger, after the history pass and the scene's top-level build). True when
  // recorded. False: not wanted, no structures this frame, or the pass could not be set up; the paint then has no image for the frame.
  bool OnHistory(plume::RenderCommandList* list, const HistoryInfo& info);
  // RT-PARKED-PAINT: the marks the history pass draws into its own image BEFORE OnHistory (car_rt_parked_paint_rules.h): the parked cars the opponents' pass will trace this frame, each a rectangle of
  // history texels and a view distance range. `info` as OnHistory will get it (the image, the divisor, the sizes, the lens). Empty where there is nothing to mark: not wanted, no structures this
  // frame, the opponents' pass off or not made yet, no parked car in the scene.
  std::vector<car_rt_parked_paint::Mark> ParkedMarks(const HistoryInfo& info);

  // CITY-RT M2 R3b (car_rt_glass_refl.h): the player's windows' additive pass into the main colour, after OnHistory recorded this frame's trace of them (false otherwise). CarSelfRefl::RecordHistory calls it
  // with the main view's colour target and the picture's size.
  bool ApplyGlass(plume::RenderCommandList* list, EdramTargets& edram, EdramTargets::Target& colour, uint32_t render_width, uint32_t render_height) {
    return glass_.Apply(list, edram, colour, render_width, render_height);
  }

  // The bindless slot of the image the paint draws of the frame after `history_frame` read (the denoise's result: the trace's image after two
  // passes or none, the scratch image after one), or car_selfrefl::kNoRtImage when the pass did not run for that history.
  uint32_t SlotFor(uint64_t history_frame) const;
  const HostImage* ImageFor(uint64_t history_frame) const;
  // RR-4b: this frame's hit distance image (R16_FLOAT, the picture's size, readable) when the frame ran raw and wrote it, else null. Valid until the next frame's pass.
  const HostImage* RawHitImage() const { return hit_ready_ ? &hit_ : nullptr; }

  // CITY-RT M2: the scene service that merges the reflections' wants (the city's structures and hit data), set once the renderer has made it; and what the pass asks the screen owner for (0 while the city
  // is not in the reflections). CarSelfRefl declares it to the screen owner at its frame start.
  void SetSceneService(RtSceneService* service) { city_.SetService(service); }
  uint32_t ScreenProducts() const { return city_.ScreenProducts(); }
  const CityRefl& city() const { return city_; }

  // The images go back (the history's went: no car for 2 s, or the setting off).
  void ReleaseImage();
  uint64_t HeldBytes() const;
  // The text CarSelfRefl's census line ends with (the totals, and the interval since the last call); empty where there is no ray tracing.
  std::string CensusText();

  // RT-HIT-SHADING-3 (car_rt_hit_rules.h census::): the exposure of the shaded colours of another car's hidden parts, in stops, measured against the history's own colours of the parts the camera
  // sees (0 until a window had enough samples; always 0 with mcla_gpu_car_rt_hit_calibrate off). The scene multiplies its frame's gain by GainOf of it.
  float HitStops() const;

 private:
  // The push block of the pass (hlsl/car_rt_refl_cs.hlsl CarRtReflPush), 96 bytes.
  struct Push {
    uint32_t slots[4];  // history slot, the reflection ray's parts, the primary ray's parts, the cone pattern's phase (low three bits) and the level's cone rays (from bit 8)
    float image[4];     // the image's size, the picture's size
    float lens[4];      // tan half field of view x and y, the divisor, the reflection ray's length (the longer of the two reaches where the parts include another car)
    float tune[4];      // the shade, the weight of a hidden hit, where the fade starts, the tangent of the cone's half angle
    float up[4];        // the world's up in view space (CarRtView::up: the chrome's sky probe), w the units: the history's over the paint's (2^exp_bias)
    float coat[4];      // the reach of a hit on the player's own parts, the reach of a hit on another car, where its fade starts (a share of it), the clear-coat ray's gain (0: none)
  };
  static_assert(sizeof(Push) == 96);
  struct TracePush {
    Push base;
    uint32_t region[4];  // xy: dispatch offset, z: stencil tile slot plus one (0: no mask)
  };
  static_assert(sizeof(TracePush) == 112);
  // The push block of one denoise pass (hlsl/car_rt_refl_denoise_cs.hlsl CarRtDenoisePush).
  struct DenoisePush {
    uint32_t slots[4];  // history slot, the image to filter, the aux image, the pass (0: taps one texel apart, 1: two)
    float image[4];     // the image's size in texels
    float lens[4];      // tan half the vertical field of view, the reflection ray's length, the tangent of the cone's half angle
    float tune[4];      // the first pass's footprint where it starts and where it is in full, the second pass's
    uint32_t region[4];  // xy: dispatch offset, z: one-ray reconstruction
  };
  static_assert(sizeof(DenoisePush) == 80);
  // The push block of the raw mode's hit distance pass (hlsl/car_rt_refl_hit_cs.hlsl CarRtHitPush), 64 bytes within the denoise layout's push range.
  struct HitPush {
    uint32_t slots[4];  // the aux image's slot
    float image[4];     // the output's size (the picture's), the trace image's size in texels
    float lens[4];      // the reflection ray's reach, the divisor, the distance a miss writes
    float reserved[4];
  };
  static_assert(sizeof(HitPush) == 64);

  // The register spaces of the layout: the bindless texture set, the output's, the car's structures' (hlsl CAR_RT_SPACE), the city's rows and counters, the push block (space 4: the city's set took 3).
  static constexpr uint32_t kTextureSet = 0;
  static constexpr uint32_t kOutputSet = 1;
  static constexpr uint32_t kSceneSet = 2;
  static constexpr uint32_t kCitySet = CityRefl::kSetIndex;
  static constexpr uint32_t kPushSpace = CityRefl::kPushSpace;
  // The denoise's layout has no structures' set: the texture set, the output's, the push block.
  static constexpr uint32_t kDenoisePushSpace = 2;

  // A descriptor set that names one or two images as storage images (a new set for a new texture: a set in use by the GPU is never rewritten).
  struct OutputSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    const plume::RenderTexture* first = nullptr;
    const plume::RenderTexture* second = nullptr;
    const plume::RenderTexture* guide = nullptr;
  };

  bool MakeLayoutAndShader(Device& device, HostPasses& passes, CarRtScene& scene);
  bool MakeDenoiseLayoutAndShader(Device& device, HostPasses& passes);
  std::unique_ptr<plume::RenderPipeline> MakePipeline();
  std::unique_ptr<plume::RenderPipeline> MakeDenoisePipeline();
  plume::RenderPipeline* Pipeline();
  // The denoise's pipeline when it is made, else null (the frame goes on with the raw image: the denoise is never waited for).
  plume::RenderPipeline* DenoisePipeline();
  // RR-4b: the hit distance pass's pipeline when it is made; null while it is being made (listed with the host precache the first time a raw frame asks: a frame never waits for a compile) or
  // when it could not be.
  plume::RenderPipeline* HitPipeline();
  std::unique_ptr<plume::RenderPipeline> MakeHitPipeline();
  // The hit distance image (R16_FLOAT, STORAGE | UNORDERED_ACCESS) at the picture's size and the set that names it; false: no raw frame (the image could not be made).
  bool EnsureHitImage(uint32_t width, uint32_t height);
  // The pipeline is made (or being made): false while it is not, so the pass is not wanted for the frame (a live switch to on).
  bool PipelineReady();
  // The trace's image and aux image, and the set that names them.
  bool EnsureOutput(uint32_t width, uint32_t height);
  // The scratch image of the denoise and the sets of its two passes; false: no denoise this frame (the images are released).
  bool EnsureDenoiseImages(uint32_t width, uint32_t height);
  bool EnsureSet(OutputSet& out, plume::RenderDescriptorSetBuilder& builder, const HostImage& first, const HostImage* second, const char* name,
                 const plume::RenderBuffer* stats = nullptr, const HostImage* guide = nullptr);
  void RetireSet(OutputSet& out);
  // The census's three buffers (the counters, 32 zero words to copy into them, the readback): made with the layout.
  bool MakeStatsBuffers(Device& device);
  // A stats frame's counters, read back once the GPU is done with them: the window, the exposure, the log line.
  void ProcessStats(uint64_t frame, uint64_t generation);
  void ReleaseScratch();
  // One compute dispatch of the denoise: `input` filtered into the image `output` names, `pass` 0 or 1; pass 2 (NOISE-RESOLVE) averages `input` with the image in bindless slot `previous_slot` at the weight `keep`.
  void DispatchDenoise(plume::RenderCommandList* list, plume::RenderPipeline* pipeline, uint32_t history_slot, const HostImage& input,
                       const OutputSet& output, uint32_t pass, uint32_t width, uint32_t height, float tan_y, float length, float tan_half,
                       const car_rt_refl::PaintRects& regions, bool resolve_paint = false,
                       float keep = 0.0f, uint32_t previous_slot = 0, bool one_ray = false);
  // NOISE-RESOLVE: the history's two images (the finished image averaged with the last frame's, hlsl/car_rt_refl_denoise_cs.hlsl pass 2), made while the pass's own history runs.
  bool EnsureStableImages(uint32_t width, uint32_t height);
  void ReleaseStable();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarRtScene* scene_ = nullptr;

  bool one_ray_ = false;
  bool supported_ = false;      // the device can trace the car (CarRtSupported)
  bool ready_ = false;          // layout and shader exist
  bool listed_ = false;         // the pipeline was listed (at the start, or in the background after it)
  bool pipeline_failed_ = false;
  plume::RenderDescriptorSetBuilder output_builder_;  // u0 the image, u1 the aux image, u2 the census's counters
  std::unique_ptr<plume::RenderBuffer> stats_;           // u2: car_rt_hit::census::kWords counters, zeroed before a stats frame
  std::unique_ptr<plume::RenderBuffer> stats_zero_;      // the zero words (an upload buffer written once)
  std::unique_ptr<plume::RenderBuffer> stats_readback_;
  bool stats_busy_ = false;         // a stats frame's readback is pending
  bool stats_ever_ = false;
  uint64_t stats_last_frame_ = 0;   // the history frame of the last stats frame
  uint64_t stats_generation_ = 0;   // bumped at Shutdown: a readback of an older one is dropped
  uint64_t stats_frames_ = 0;       // stats frames read back since the start
  car_rt_hit::census::Totals window_;  // the counters since the last log line
  uint32_t window_frames_ = 0;
  uint64_t cal_count_ = 0;          // the exposure's samples gathered since it last moved
  uint64_t cal_seen_ = 0;
  uint64_t cal_model_ = 0;
  float hit_stops_ = 0.0f;          // the exposure in force, in stops (car_rt_hit::census::NextStops)
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  // The denoise: its own layout and shader, and its pipeline in the host precache beside the trace's (key variant 1); optional: the trace runs
  // without it.
  bool denoise_ready_ = false;  // its layout and shader exist
  bool denoise_failed_ = false;
  plume::RenderDescriptorSetBuilder denoise_builder_;  // u0 the image the pass writes
  std::unique_ptr<plume::RenderPipelineLayout> denoise_layout_;
  std::unique_ptr<plume::RenderShader> denoise_shader_;
  std::unique_ptr<plume::RenderPipeline> denoise_pipeline_;

  // The images: the trace's output (what the paint reads after two denoise passes or none), the aux (hit distance, instance, N.V), the scratch
  // (between the two denoise passes; what the paint reads after one), and the sets that name them as storage images.
  HostImage image_;
  HostImage aux_;
  HostImage scratch_;
  OutputSet trace_set_;   // u0 image_, u1 aux_
  OutputSet first_set_;   // u0 scratch_
  OutputSet final_set_;   // u0 image_
  // NOISE-RESOLVE: while no temporal upscaler runs the cone's pattern changes every frame and the finished image is averaged with the last frame's result (noise_resolve.h): two images of the same size
  // that take turns (the paint reads the one written last), their sets, and what the next frame needs to know of it.
  car_rt_refl::PaintCoverage trace_coverage_;
  car_rt_refl::PaintCoverage scratch_coverage_;
  car_rt_refl::PaintCoverage stable_coverage_[2];
  HostImage stable_[2];
  HostImage unused_guide_;
  CarRtDenoiseV2 v2_;
  OutputSet stable_set_[2];       // u0 stable_[i]
  noise_resolve::HistoryLifetime stable_life_;   // NOISE-FOLLOW: how long the two images stay while an upscaler runs (noise_resolve::kHistoryKeepFrames)
  uint32_t stable_index_ = 0;
  car_rt_refl::ConePose stable_pose_;
  bool stable_garage_ = false;
  bool stable_valid_ = false;     // stable_[stable_index_] holds the result of `stable_frame_`
  bool stable_resolve_paint_ = false;
  bool stable_one_ray_ = false;
  bool stable_city_ = false;
  float stable_length_ = 0.0f;
  float stable_spread_ = 0.0f;
  float stable_coat_ = 0.0f;
  int stable_exp_bias_ = 0;
  uint64_t stable_frame_ = 0;
  uint32_t stable_frames_ = 0;    // frames of history the last result holds
  bool result_in_stable_ = false; // the paint reads stable_[stable_index_] for the frame `recorded_frame_`
  uint64_t stable_runs_ = 0;
  bool stable_logged_ = false;

  // RR-4b: the raw mode's hit distance pass: its shader (made when a raw frame first asks) and pipeline on the denoise's layout, its image and the set that names it.
  std::unique_ptr<plume::RenderShader> hit_shader_;
  std::unique_ptr<plume::RenderPipeline> hit_pipeline_;
  bool hit_listed_ = false;
  bool hit_failed_ = false;
  HostImage hit_;
  OutputSet hit_set_;     // u0 hit_
  bool hit_ready_ = false;       // this frame ran raw and wrote hit_
  uint32_t plain_streak_ = 0;    // frames the pass ran without the raw mode (the hit image goes back after car_rt_refl::kRawReleaseAfter)
  uint64_t raw_passes_ = 0;
  uint32_t raw_state_logged_ = ~0u;  // 0 raw, 1 asked and not possible, 2 not asked after having been
  bool raw_ever_asked_ = false;

  // The opponents' cars (car_rt_opp_refl.h): a second dispatch of the trace's shader file into the same images, after the player's.
  CarRtOppRefl opp_;
  // The player's windows (car_rt_glass_refl.h, CITY-RT M2 R3b): a third dispatch of the same file, into an image of its own, and the additive pass that puts it into the main colour.
  CarRtGlassRefl glass_;
  // The city in the reflections (city_refl.h): the setting, the rows and counters of set 3, the census.
  CityRefl city_;

  // The frame.
  bool active_ = false;
  bool recorded_ = false;      // the image holds the trace of `recorded_frame_`'s history
  uint64_t recorded_frame_ = 0;
  bool result_in_scratch_ = false;  // the paint reads the scratch image for that history (one denoise pass), else the trace's

  // The log and the totals.
  bool first_logged_ = false;
  bool no_scene_logged_ = false;
  uint32_t image_for_[2] = {};
  uint64_t passes_run_ = 0;
  uint64_t denoise_runs_ = 0;   // frames the trace was denoised (one or two passes)
  bool denoise_logged_ = false;
  uint64_t no_scene_ = 0;       // frames the pass was wanted but the scene had no structures
  uint64_t not_ready_ = 0;      // frames it was wanted but the pipeline was still being made
  uint64_t census_passes_ = 0;
};

}  // namespace mcla::gpu
