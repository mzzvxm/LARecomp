#pragma once
// ===========================================================================
// MCLA GPU renderer -- car self-reflections (package G8b/G8c): a graphics setting, live, with an `auto`
// ===========================================================================
// The player's car paint (xCarPaintCustomizable::PS_CarPaint) reflects the world through two environment reads and nothing of
// the car itself: no wing in the trunk lid, no mirror in the door (feature-parity/G8A-CAR-SELFREFL.md). This is the host
// half of the paint variant of shaderpack/variants/car_selfrefl.h: the variant marches a short ray along the reflected
// view vector through an image of the PREVIOUS frame (the car is drawn before the world, so it cannot see the current
// frame's depth), and this keeps that image and the numbers that take a point of this frame into it.
//
//   mcla_gpu_car_selfrefl        auto | off | on, a graphics setting like the contact shadows (the pause menu's CAR
//                                SELF-REFLECTIONS row on REFLECTIONS, shared with mcla_gpu_car_selfrefl_rt, live, saved): on darkens the reflection where the
//                                ray passes just behind a part of the car (the specular occlusion) and reflects the car's own
//                                colour there. `auto` is the class's cell (gpu_class.h): off on Low, on on Mid and High.
//                                The setting has one level (the older low, the occlusion alone, cost the same as the full effect:
//                                perf/out/FXCOST-B6.txt, owner decision 2026-11-14), and a word that is no value counts as `auto`.
//                                host_plan.h has the rule that the precache and the draws share.
//   mcla_gpu_car_selfrefl_test   a debug-only override: 0 follows the setting (the default), 1 forces the occlusion-only mode
//                                (no setting reaches it any more), 2 forces the full effect, whatever the setting says (the G8b chain's runs
//                                and the old command lines use it).
//   mcla_gpu_car_selfrefl_view   a debug view in place of the paint's colour: 1 the hit weight, 2 the colour the ray
//                                sees (the hit pixel's, or the paint's own in shade for a hidden face), 3 the baked vertex AO,
//                                4 the state of the trace (yellow a hit, orange a hit that is probably the hidden face of a
//                                part of the march's, brighter the heavier; green no hit; the other colours in the variant's text), 5 the ray
//                                traced image at the paint's pixel (car_rt_refl.h: purple no image, cyan no data there, green no
//                                hit, yellow a hit on the player's own part, orange on another car), 6 the clear coat's layer's
//                                amount (written 64 times the target's unit so that a script can read it, RT-HIT-SHADING-6: it shows as about itself),
//                                7 what it adds (version 8 of the variant; 2 is the ray traced reflection alone at full intensity)
//   mcla_gpu_car_selfrefl_rt     auto | off: ray traced reflections of the car in its own paint where the device can (car_rt_refl.h);
//                                the march of this file is the fallback and the other devices' version
//   mcla_gpu_car_selfrefl_source a debug-only A/B of where the history pass takes the scene colour from: 0 (the default) the shared screen products' colour_post
//                                where the frame has it at the picture's size (screen_shared.h: another effect made it), else the main colour target's planes
//                                as before; 1 always the planes (the old path); 2 as 0 and this effect asks for colour_post itself, so that the two paths can be
//                                compared without a reflections effect. Both make the same bytes: the capture's arithmetic is the pass's own
//   mcla_gpu_car_selfrefl_length / _thickness / _strength / _steps / _gain    the tuning of the ray (car_selfrefl_rules.h)
// With the setting off the game's paint draws, bit for bit: the variant is not chosen (pipelines.cpp WantedFeatures asks
// Selected()), and no history is made. The compare key (a developer tool, effect_compare.h) flips the setting like the others.
//
// WHERE IT RUNS (menu_world.h): in the game view, in the pause menu's and the race pause's world and in the garage (the 4x view
// of the pause identities, which the host draws once for its three tiles, so the history at the first resolve is the whole
// picture); not in the map (the camera 9 km up sees no car to speak of: RulesFor(kMap).car_refl). The history is made from the
// camera of the frame before it, of the same kind of view, and a frame after a change of kind (the pause menu opening, the map)
// has none. In the pause view the vehicles are taken as still: the game's per-vehicle slots of a 4x frame are not trusted
// (temporal_menu.h UseGameVehicleMotion, fix/garage-view 11062cb2: the IDs are handed out in draw order across the three tiles,
// a slot may hold another car's matrix), and a car in the garage or under the pause menu does not move by itself.
//
// THE HISTORY: at the game's mid-scene depth copy (into 078B0000: the ambient occlusion's trigger, ao.h), after the AO's
// own apply, one pass reads the main view's stencil plane and, for the pixels whose stencil ID says a vehicle (1 to 23;
// sample 0), its depth and colour planes, and writes an RGBA16F image (the render size, or half of it: see below): a the
// view-space distance of a vehicle's pixel, rgb its scene colour (the mean of the host samples, in the target's units: the
// shaders' output times 2^exp_bias), all 0 elsewhere (hlsl/car_history_ps.hlsl). That image is what the NEXT frame's paint
// draws read, as points: it is the only copy of the frame's
// colour there is at that time (the game's own end-of-scene colour copy, 07FE0, is not alive at the car's first draw: the
// census of G8A found it in 0 of 600 frames, its depth copy in 600 of 600). The image exists only while the setting is
// on (it is given back after 2 s without it).
//
// THE NUMBERS: the history's own camera (the view-projection of the frame it was made in, read from the game's motion blur
// draw, the anchor, which comes after the trigger: since SS-1 the shared screen products read it, screen_shared.h Cameras(), and keep
// the previous frame's with its lens test; this class asks) and the vehicle's own motion (vehicle_motion.h: the game's
// c(69 + 4 ID) times the inverse of c69, taken at the last anchor and repeated for this frame as the ambient occlusion
// does) make the one matrix a draw of the paint is given (CarSelfReflConstants::m): this frame's world position to the
// history frame's clip space. The variant also tests that the shaded point finds itself in the history at the distance it
// has now; where it does not (a cut, a wrong guess of the motion, an occluder) the effect fades out.
//
// The moving car (play view): the vehicle's motion of the last anchor is repeated for this frame (a constant velocity guess, as
// the occlusion's history does), looked up by the stencil ID of the paint's own draw. The IDs are handed out in draw order each
// frame, so a car that changes its place in the order between two frames gets another car's motion for one frame; the variant's
// self-test (the point must be found in the history at its own distance) then fades the effect out for that frame instead of
// smearing it. A car with no usable slot (placed this frame) is taken as still, which at speed fails the same test.
//
// THE RAY (variant version 4, shaderpack/variants/car_selfrefl.h has the detail): the points are the same for every pixel and
// frame (no per-pixel noise: it made a part narrower than the steps a dotted patch), closer together near the shaded point. The
// ray is a chain of segments from point to point; each takes the share of its length that is inside the window of "just behind a
// vehicle pixel" (an exact integral of the window along the segment; a segment with the outline of a part in it is halved three
// times to find where, and takes the share on the window's side), the hit is the sum of the segments' covers with a fade along the
// ray, whole at a share of the ray's points (mcla_gpu_car_selfrefl_full), and made smaller where it is probably the hidden face of a
// part (the ray comes back out towards the surface from behind). The pure arithmetic is in car_selfrefl_rules.h with a reference
// march in the rules test. The paint's own colour in shade stands in for a hidden face in the reflection mode. The constants'
// b[3] is the share of the points (it was a noise frame before version 4).
//
// RAY TRACED (variant versions 5 and 6, car_rt_refl.h): where the device can trace the car's triangles (car_rt_scene.h) the history pass is followed by a
// compute pass that gives every vehicle texel of the history the car's reflection of ITSELF from its triangles (exact: a part the camera does not
// see, a thin part), as the share of a cone of rays that hit (a glossy reflection: soft edges), in an image of the history's size that the next
// frame's paint reads at the same place it reads the history (version 6: a 3x3 B-spline of texels), in place of the march. The march stays: where the image has no data (alpha 0: not the player's car, another vehicle, the first frame, no structures)
// the paint does version 4's march, and with mcla_gpu_car_selfrefl_rt off or on a device that cannot trace, version 4's output is the output
// (the constants' c[3] bit 0 says whether the frame has an image).
//
// The depth is linearised with the previous frame's lens (the trigger comes before this frame's anchor, as the occlusion's
// does); a lens that changed by more than 5% between the two frames (the pause view's field of view, a sweep) leaves no
// history for the next frame.
//
// WHAT IT COSTS WHEN IT CAN BE LEFT OUT: no history pass and no image while the setting is off (the image goes back after 2 s),
// and none in a frame whose own draws had no paint of the variant (no car on the screen: the pass runs at the end of the
// frame's opaque draws, after the car's, and its image is for the next frame's paint); the image goes back after 2 s without a
// car as well. The pass itself reads the stencil of every pixel and the depth and the colour of a vehicle's pixels only, into an
// image that is half the render size from 2560 pixels across (and for the occlusion-only test level at any size: a quarter of the
// memory, 15.8 MiB against 63.3 MiB at 3840x2160; car_selfrefl_rules.h HistoryDivisor, the debug cvar mcla_gpu_car_selfrefl_history).
//
// START AND SWITCH: the history pass has two pipelines (plain and multisampled source). They are made ahead on the host
// builder thread where the setting is on at the start, and in the background where it is switched on later (HostPrecache::
// AddLive): the effect is not drawn until they are made (a frame never waits for a compile), as the sun shafts' are.
//
// COST (the final measurement, perf/out/FXCOST-B6.txt, RTX 5080, 3840x2160, K-DAY): +0.107 ms for the occlusion alone and +0.095 ms
// for the full effect (both drew about 8400 paint draws per 600 frames), against the effect off, so the setting has one level. The first
// prototype's runs (G8B, perf/out/G8B.txt) were +0.17 ms and +0.26 ms; the numbers below are from that prototype:
// the history pass is one full-screen pass reading the 2x multisampled colour, depth and stencil planes, the ray is 12 steps of
// one RGBA16F load each on the car's pixels (about 7% of the picture in the K shots, 15% close behind it), the image 8 bytes a
// pixel (66 MB at 3x, 16 MB at 1080p). All of it only while the setting is on.
//
// CHECK: the pictures and the log say:
//   1. units: the history colour times 2^-exp_bias (the main target's bias is 4, gInvColorExpBias.x = 1/16) is taken to be the
//      paint's own units, the ones its environment reads are in. If the reflected car is too dark or bright against the
//      paint, mcla_gpu_car_selfrefl_gain moves it and view 2 shows the colour alone.
//   2. the paint's interpolants iTexCoord1, iTexCoord2 and iTexCoord6 are the world position, the world normal and the view
//      vector towards the camera (read from xCarPaintCustomizable__VS_CarPaint.hlsl, not seen at run time): view 4 is
//      all red if they are not, and view 3 shows the baked AO.
//   3. the game's c8-c11 as a world to clip matrix whose w is the distance AoLinearDepth gives (the rules test checks the
//      arithmetic on a projection of that shape, not on the game's).
//   4. the paint draws have no alpha test or alpha to coverage (the census of G8A: variant 0, a2m 0, and the G8B cost runs drew
//      14 paint draws a frame through the variant), else they keep the game's shader; and the stencil ID of the draw
//      (4 ID + {0, 1}) is the ID the history's stencil holds for the same car.
//   5. the garage and the pause menu: that the paint there is the same shader (439B...), that the history of a 4x frame is whole
//      at the first resolve, that the camera's anchor is there (the log's `world:` line names the world and the effect), and that
//      the picture is steady under an orbiting camera (a still car, motion taken as none).
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are made ahead on the host
// builder thread (host_precache.h).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "car_selfrefl_rules.h"
#include "constants.h"
#include "draw_state.h"
#include "edram.h"
#include "host_plan.h"
#include "host_passes.h"
#include "menu_world.h"
#include "records.h"
#include "resolve.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "vehicle_motion.h"

namespace mcla::gpu {

class CarDump;
class CarRtRefl;
class PassTiming;
class ScreenShared;  // screen_shared.h: the camera history and the colour_post capture this class reads (SS-1)

class CarSelfRefl {
 public:
  using Mode = car_selfrefl::Mode;

  // The destination of the game's mid-scene depth copy in the retail build (shafts.h, ao.h): another build gives another
  // address and then there is no history, never a wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // True while the effect is on (the setting or the test override, and the history pass's pipelines made): the frame's own
  // answer, latched at BeginFrame, so every draw of a frame picks the same shader. pipelines.cpp WantedFeatures asks it
  // per draw of the car paint; a change of the setting shows at the next frame. Any thread.
  static bool Selected();

  CarSelfRefl() = default;
  ~CarSelfRefl();
  CarSelfRefl(const CarSelfRefl&) = delete;
  CarSelfRefl& operator=(const CarSelfRefl&) = delete;

  // Makes the shaders and lists the pipelines with the host precache (before the Renderer starts it) where the setting is on at
  // the start (host_plan.h); where it is off nothing is made until it is switched on. Never fails the renderer: without them
  // the variant draws as the game's paint (mode 0 constants).
  void Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();
  // The debug car dump (car_dump.h): handed the history image of its frame when it is drawn, to check its triangles against. Null: none.
  void SetCarDump(CarDump* dump) { dump_ = dump; }
  // The ray traced half (car_rt_refl.h): where its pass is wanted and can run it makes an image at the history trigger, right after the
  // history, that the paint variant (version 5) reads in place of the march. Null: the march everywhere.
  void SetCarRtRefl(CarRtRefl* rt) { rt_ = rt; }
  // The shared screen products (screen_shared.h, SS-1): the frames' cameras (the previous frame's lens and camera for the history, whether this frame's lens agrees) and the colour_post capture
  // the history pass reads where the frame has it. Not owned; null: no camera, so no history. Render thread, before the first frame; the renderer's Shutdown stops this class first.
  void SetScreenShared(ScreenShared* screen) { screen_ = screen; }
  // True when the effect was on at the start and its pipelines were listed with the host precache (Initialize): what CarRtRefl asks to
  // list its own pipeline the same way.
  bool ListedAtStart() const { return listed_; }

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame();
  // True while the draw hooks have anything to do.
  bool wants_draws() const { return mode_ != Mode::kOff; }

  // A prepared draw of the main view (temporal_identities.h MatchMainView): `stencil_ids` the IDs it writes into the
  // stencil (vehicle_motion.h StencilIdsWritten), for the vehicles' motion.
  void OnMainDraw(const temporal::MainMatch& match, uint32_t stencil_ids);
  // The game's motion blur draw: this frame's camera and the vehicles' motion; the history made this frame is theirs.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A resolve, after ResolveCache::Execute (and after the ambient occlusion's apply): the mid-scene depth copy is where the
  // history is made. True when passes were recorded (the caller's list state is gone).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary.
  void OnPresent();

  // The 112 bytes a draw of the paint variant reads (Scene::FillVariantBlock): the game's paint (mode 0) when
  // there is no history for this frame. `upscaler_running` only turns the ray traced pass's cone pattern by the frame (the march has no noise, since version 3).
  void FillConstants(const XenosState& state, bool upscaler_running, car_selfrefl::CarSelfReflConstants* out);

  // The world of the frame that is ending (menu_world.h), by the frame's own camera, and the level in force ("off", "on", or
  // "occlusion" for the debug test level), for the scene's one line per change of world.
  menu_world::World FrameWorld() const;
  const char* Level() const;

  // The video memory the image holds now.
  uint64_t HeldBytes() const;

 private:
  // The push block of the pass (hlsl/car_history_ps.hlsl CarHistoryPush), 64 bytes: the host pass layout's push range.
  struct Push {
    uint32_t slots[4];   // depth plane, colour plane, stencil plane (0xFFFFFFFF: none), host samples
    uint32_t slots2[4];  // the host sample that is the guest's sample 0
    float sizes[4];      // the picture in host pixels
    float lin[4];        // the projection's rows P22, P23, P32, P33 (ao_common.hlsli AoLinearDepth)
  };
  static_assert(sizeof(Push) == 64 && sizeof(Push) == HostPasses::kPushConstantBytes);

  // The history pass's compilations (host_shaders.h CarHistoryPixelShader, hlsl/car_history_ps.hlsl CAR_HISTORY_MS): from plain planes, from multisampled planes, and from multisampled depth and
  // stencil planes with the colour taken from the shared colour_post.
  enum Pass : uint32_t { kPassPlain, kPassMs, kPassMsShared, kPassCount };
  static const char* PassName(uint32_t pass);

  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // The shared colour_post when this frame's history pass may read it (the source setting allows, the frame has it, and it is the picture's size: pixel for pixel the pass's own colour); else null.
  const HostImage* SharedColour(uint32_t render_width, uint32_t render_height) const;
  EdramTargets::Target* ColourTarget(const EdramKey& key, plume::RenderCommandList* list);
  bool RecordHistory(const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  plume::RenderPipeline* Pipeline(uint32_t pass);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t pass);
  // The shaders, made where the setting is on at the start or when it is first switched on. False: they could not be made.
  bool MakeShaders();
  // Lists the pipelines with the start's precache (the setting is on), or makes them in the background (it was off at the
  // start and is on now); the second says the mode off for the frames until every pipeline is made.
  void ListPrecache(const std::string& note);
  void HoldUntilBuilt(Mode* mode);
  void ReleaseImage();
  void Census();

  // Settings, read at the frame's start.
  Mode mode_ = Mode::kOff;
  int source_ = 0;  // mcla_gpu_car_selfrefl_source
  car_selfrefl::Tuning tuning_;
  uint32_t off_frames_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  bool play_view_ = false;   // the frame's main view draws were the game view's (2 guest samples)
  bool pause_view_ = false;  // ... the pause menu's, the garage's or the map's (4 guest samples); never both in a usable frame
  uint32_t stencil_ids_ = 0;      // the vehicle IDs the main view's draws wrote before the anchor
  uint32_t frame_paint_draws_ = 0;  // draws of the paint through the variant so far this frame (the car is on the screen)
  bool history_pending_ = false;  // the pass was recorded this frame; the anchor says whose camera it goes with

  // The camera the trigger worked with: the previous frame's anchor camera, from the shared screen products' camera history (Previous), copied when the history pass is recorded. Its lens
  // linearised the history's depth; whether this frame's own anchor agrees with it is the owner's LensAgrees.
  temporal::Camera trigger_camera_;
  ScreenShared* screen_ = nullptr;
  // The world the frame's trigger was in (menu_world.h): by the previous frame's camera of the same kind of view.
  menu_world::World trigger_world_ = menu_world::World::kNone;

  // The history: made at the trigger of frame `history_frame_`, valid for the draws of the frame after it, which are of the
  // same kind of view (`history_pause_`).
  bool history_valid_ = false;
  uint64_t history_frame_ = 0;
  bool history_pause_ = false;
  temporal::Camera history_camera_;
  // The vehicles' own motion of the history frame's anchor, repeated for the next frame (the play view's only).
  temporal::VehicleMotion motion_;
  uint64_t motion_frame_ = 0;

  // The GPU side.
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarDump* dump_ = nullptr;
  CarRtRefl* rt_ = nullptr;
  bool upscaler_running_ = false;  // FillConstants' flag of the frame's last paint draw (its draws come before the trigger)
  bool ready_ = false;            // the shaders exist
  bool shaders_failed_ = false;   // they could not be made: not tried again
  bool listed_ = false;           // the pipelines were listed at the start (or in the background after it)
  bool built_ = false;            // ... and every one is made or has failed
  std::array<std::unique_ptr<plume::RenderShader>, kPassCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kPassCount> pipelines_;
  std::array<bool, kPassCount> pipeline_failed_{};
  HostImage history_;  // R16G16B16A16_FLOAT, the picture's size

  // What the log has said once, and the totals.
  uint32_t last_mode_logged_ = ~0u;
  bool no_stencil_logged_ = false;
  bool failed_logged_ = false;
  bool bias_logged_ = false;
  bool coat_layer_logged_ = false;
  // The main colour target's exponent bias as the last paint draw saw it (RT0's color_exp_bias): the ray traced pass multiplies the colours it makes up from a paint constant by
  // 2^bias to be in the history's units. A paint draw comes after the trigger in a frame, so the pass uses the frame before's (the game keeps one bias); 4 until a draw has told.
  int32_t exp_bias_ = car_selfrefl::kMainTargetExpBias;
  bool first_paint_logged_ = false;
  bool pause_paint_logged_ = false;
  uint32_t image_for_[2] = {};
  uint64_t passes_run_ = 0;
  uint64_t passes_shared_ = 0;  // ... of them with the colour read from the shared colour_post
  uint64_t triggers_ = 0;
  uint64_t no_camera_ = 0;
  uint64_t lens_changed_ = 0;
  uint64_t world_skipped_ = 0;  // triggers in a world the effect does not run in (the map)
  uint64_t skipped_no_car_ = 0;  // triggers of a frame with no draw of the paint variant: no history pass
  uint64_t paint_draws_ = 0;
  uint64_t paint_with_history_ = 0;
  uint64_t paint_no_id_ = 0;
  uint64_t paint_motion_ = 0;
  uint64_t paint_pause_ = 0;  // paint draws with a history in the pause view's worlds (the car taken as still)
  uint64_t paint_with_rt_ = 0;  // paint draws given the ray traced image (car_rt_refl.h)
  // The totals at the last census line, for the interval's own numbers.
  uint64_t census_paint_ = 0;
  uint64_t census_with_history_ = 0;
  uint64_t census_pause_ = 0;
};

}  // namespace mcla::gpu
