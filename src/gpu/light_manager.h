#pragma once
// ===========================================================================
// MCLA GPU renderer -- the LightManager, PASSIVE: one LightFrame a frame from today's producers, and a check that it says what the consumers use
// (perf/LIGHT-UNIFY.md section 3 and 7.1 step U2, package LU-2)
// ===========================================================================
// WHAT IT DOES. Each guest frame it collects, into the one record light_frame.h freezes (Light, LightFrame), the lights that exist today: the headlight rig's set (CurrentHeadlights():
// ids from the car, slot-stable, a velocity from the scene clock), the shadow replay's maps as the rig keeps them (the last trigger's moved to this frame's set, then this frame's), and
// the street lamps the volumetric light's picker chose, each found in the game's lamp list by its place (the id), with the weight the picker folded into its colour. It uploads the 64
// byte GpuLight rows of the frame into the upload ring (once, at the guest present), logs a census line, and CHECKS the frame against each consumer's own data:
//   - the headlight block the lit shaders read (HeadlightRig::Block(), 672 B or 1072 B): at the frame's start (the lights of this set, the last trigger's maps) and after the trigger
//     (this frame's maps), every light's rows, the reach box, every shadow view by content, the atlas's slot and sampler;
//   - the volume's inputs (VolumetricsInputs::Fill, at its end): the street lamps through ToVolLamp (bit for bit but for the picker's weight), the headlights in the volume's units,
//     the shadow views each reads and the atlas;
//   - its own invariants: unique ids, a light that lived keeps its slot, the rankings, the GPU twin's round trip, the id formulas.
// CAR LIGHTS (LIGHT-UNIFY U7, wired in the groundwork branch): while the manager runs it also switches the hook's every-kind notes on (car_lights.h CarLightsSource::SetEnabled: the
// game's light points of all 19 kinds of every car in sight) and polls them once a frame at the present (Poll), as car_lights.h says the manager must; the census line adds a second
// line `[lights] car lights ...` (the cars, their lamps on, the police point lights MakePointLights would hand the picker) and up to kCensusCars `[car-lights]` lines (FormatCensus). The
// cars' frames are NOT passed yet (no frame of the renderer's own cars: `car_frames_` stays empty, so every car's frame is derived from its lamps), and nothing of it goes into the
// LightFrame, so the self-checks and the GPU buffer are what they were: LU-3 puts the point lights and the per car bits in. Off (the setting 0): SetEnabled(false), the hook costs one
// relaxed load.
// It is PASSIVE UNLESS A FLIP IS ON (mcla_gpu_light_frame, below; default off): NO CONSUMER READS IT otherwise. The lit shaders' block, the volume's lamps and headlights and the replay's maps are made and read exactly as before; the manager only looks (its
// calls take const references, the one non-const call is HeadlightRig::Block(), which rebuilds the block lazily if the replay's maps changed since: the same rebuild the first lit draw
// would do, and the camera in the block is overwritten per draw). The COLLECTING does not pick: the lamps in the frame are the ones the volume's picker gave. The picking of the other two roles IS here since LU-3 (light_pick.h): the manager holds the picker of the
// rig's four headlights and of the cars that get shadow views, and the one budget (Pickers(), always, with or without the census); the street lamps' pick (the volume's LampPicker, the lamp
// shadows') is reserved to move in with LU-4 / LU-5.
//
// THE FLIPS (LIGHT-UNIFY U4, package LU-4; mcla_gpu_light_frame, debug only, a bit mask, default 0). The consumers can read the frame instead of their own producers, one at a time, with
// the SAME BYTES (light_project.h: each consumer's builder run over the frame's data, and the checks that compare the two byte for byte):
//   bit 1  4a  the lit shaders' headlight block (HeadlightBlock(): Scene::FillVariantBlock reads it instead of HeadlightRig::Block())
//   bit 2  4b  the volumetric light's lamps, headlights and shadow views (VolumetricsInputs::Fill)
//   bit 4  4c  the lights the headlight shadow replay picks cars and makes views from (LocalShadow::PickViews)
//   bit 8  U5  the shadow views: the replay records into the one shadow atlas (shadow_atlas.h) and the frame's views are the atlas's published form read BY LIGHT ID (shadow_atlas_rules.h
//              RebindByIds), not the rig's carry moved by the car's key; with the census on both frames are made and compared field by field (Check::kShadowFrame)
// A flipped consumer asks Serving(bit) first and uses its own producer when it is false (the manager off, the frame not opened yet): nothing breaks if the manager is late. With the census
// ON the legacy producers' data is made too and compared with the projection every frame (Check::kBlockBytes, kVolumeBytes, kShadowLights; the census line counts them): that is the
// proof run. With the census OFF and a flip on, only the frame is made: no checks, no upload, no cars' light points (what a flip costs at play). Switching a flip off live goes back to the
// consumer's own producer, which kept running (the rig, the replay) or starts afresh (the volume's lamp picker: its first pick after a gap is at full weight).
//
// THE SWITCH. mcla_gpu_lights_census N (debug only, default 0): N > 0: it runs every guest frame, the census line is every N guest frames (300 is the usual), a difference found by a
// check is logged at once (the first three of each kind) and counted in the next census line. Live. The manager runs while the census is on OR a flip is; with both off it is not
// called and the cost is two setting reads a frame.
//
// THREADING. Render thread only. Initialize and Shutdown on the lifecycle thread.
//
// WHERE IT IS CALLED (scene.cpp, volumetrics_inputs.cpp and the three flipped consumers; LU-4 added the first row and the Serving / projection calls):
//   Scene::BeginFrame           BEFORE the replay's BeginFrame                    -> BeginLights  the settings; the frame's lights (the rig's set of this frame)
//   Scene::BeginFrame           after the rig's and the replay's BeginFrame       -> BeginFrame   the last trigger's maps; the block checks at the start
//   Scene::OnResolve            after the replay's OnResolve                      -> OnResolve    the trigger (this frame's maps); the block checks after it
//   VolumetricsInputs::Fill     at its end (and, flipped, in place of its producer) -> NoteVolumeInputs / FillVolumeInputs   the census's look at the volume's inputs / the volume's lights from the frame
//   Scene::OnPresent            before the rig makes the next frame's set         -> OnPresent    the frame's check, the upload, the census
//   Scene::FillVariantBlock     per lit draw, flipped                             -> HeadlightBlock   the headlight block of the frame
//   LocalShadow::PickViews      once a frame, flipped                             -> ShadowLights     the lights the replay picks cars and makes views from
//
// THE RUN PLAN (for the main session; nothing was run: CHECK all of it). No shader pack change (no variant text moved), no codegen: a normal build of this branch. Unit first:
// larecomp_light_manager_test (7701 checks on main's headers, 7707 on feat/hl-combined's, where the block is 1072 B and a light has two views) and larecomp_light_frame_test.
// Then one game at a time, class high on the RTX 5080 (volumetric light, headlights and light shadows on: the defaults), a 1080p window, display awake, no toast over the game, the
// toolkit scenes local-lights-open-night, local-lights-night and local-lights-rain (ports/scripts/scenes), D3D12 and then Vulkan:
//   1. `--mcla_gpu_lights_census=300 --mcla_gpu_vol_census=300 --mcla_gpu_headlight_log=true` (the two producers' own lines beside ours). Each log has `[lights]` lines. EXPECT in every
//      line: `self-check: N checks, 0 differed (... frame K/0)` for every kind that has checks (block at start, block after trigger, volume lamps, volume headlights, volume views,
//      frame); `headlights` = the rig's count in its own `[headlights]` line (`{n} lights`), `lamps` = the `[volumetrics] inputs` line's `picked + fading`, `0 without one` (a
//      nonzero count is the core's test lamp or a lamp fading out whose record is gone), `0 differ from their record`, `slots a frame ... 0.0x added, 0.0x gone` in a steady drive,
//      and `manager CPU` under 20 us a frame (the gate is 0.02 ms; the model says 8 us).
//   2. NO PICTURE CHANGE. (a) The scene with the setting at 0 against at 300, at the scene's fixed step (every toolkit scene is deterministic): the producers' own lines
//      (`[volumetrics] inputs, frame N:` and the rig's `[headlights] frame N:`, timestamps cut) are identical, and the pictures differ by no more than the control run of the same
//      scene (off against off2: the noise floor was 0.26 % on the three-view sheet). (b) The unit test holds that collecting and checking change no byte of the rig's set and
//      block, the volume's inputs, the lamp list or the replay's maps (every producer's data is compared before and after).
//   3. A live switch in a run: `mcla_gpu_lights_census` 0 -> 300 -> 0: the `[lights] on` and `[lights] off` lines, no stall.
//   4. A level change of VOLUMETRIC LIGHT and of LIGHT SHADOWS in the same drive: `generation` in the line changes and the check stays at 0 differences. A difference in the one frame
//      of a setting change is the replay giving its maps up at its own BeginFrame: read the line before and after it.
// CHECK (nothing here could be run): (i) the block at start is read with HeadlightRig::Block(), which rebuilds lazily when the replay's maps changed since the last build: the first
// lit draw would have done the same, and the block's camera (the only thing the two moments could make differ) is overwritten by every draw whose c15 is a number; (ii) the
// replay's trigger is found by `OnResolve` returning true or by what it publishes changing (a second trigger in one frame, or maps given up outside a trigger, would show as a
// difference); (iii) the volume's Fill is taken to be after the trigger (the manager's frame has this frame's maps by then); (iv) velocity is the step between two guest frames over
// the scene clock, and nothing checks it (LU-3 gives the picker's own); (v) the wiki's debug page wants a line for `mcla_gpu_lights_census` (main session).
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "car_lights.h"
#include "light_frame.h"
#include "headlight_rig_rules.h"
#include "light_manager_rules.h"
#include "light_pick.h"
#include "light_project.h"
#include "local_shadow_types.h"
#include "vol_lamp_list.h"
#include "volumetrics_lamp_pick.h"
#include "volumetrics_types.h"

namespace mcla::gpu {

class HeadlightRig;
class LocalShadow;
class ShadowAtlas;   // shadow_atlas.h
class UploadRing;

class LightManager {
 public:
  LightManager();
  ~LightManager();
  LightManager(const LightManager&) = delete;
  LightManager& operator=(const LightManager&) = delete;

  // The producers it looks at and the ring it uploads to. Pointers: they must outlive it (the Renderer's members). Never fails the renderer.
  void Initialize(UploadRing& upload, HeadlightRig& rig, LocalShadow& shadow);
  void Shutdown();
  // LU-5 (mcla_gpu_light_frame bit 8): the one shadow atlas whose published form the frame's shadow views are read from when that flip is on. Null (the default): the carry's. Render thread.
  void SetShadowAtlas(ShadowAtlas* atlas) { atlas_ = atlas; }

  // The Scene opened a frame's first list, BEFORE the shadow replay's BeginFrame (LU-4c: the replay picks its cars from the frame's lights): reads the settings (the census, the flips);
  // when either is on, opens the frame with the rig's lights of this frame. `frame` is the Scene's frame serial, `upload_slot` the frame's slot in the upload ring. A second call of the
  // same guest frame changes nothing but the slot.
  void BeginLights(uint64_t frame, uint32_t upload_slot);
  // The same list, AFTER the rig's and the replay's own BeginFrame: the last trigger's maps (the replay gave them up if the frame before had no trigger) and, with the census on, the
  // block check at the start. Opens the frame itself if BeginLights was not called. A second list of the same guest frame is ignored.
  void BeginFrame(uint64_t frame, uint32_t upload_slot);
  // After the replay's OnResolve of every resolve (`shadow_recorded` is its return value): when it was the trigger, this frame's maps; then the block is checked again.
  void OnResolve(bool shadow_recorded);
  // 4b (mcla_gpu_light_frame bit 2): the volume's lamps, headlights and shadow views made from the frame instead of by the volume's own producer, into `out` (which holds the core's
  // defaults and its test lamp: the lamps come after it, the way FillLamps appends them). The street lamps are picked HERE, by the same pick the volume runs (volumetrics_lamp_pick.h: the
  // list's freshness is the caller's, the camera's step, the room and the picker are the class's), and kept in the frame as Lights with their exact weights; the headlights and the
  // views are the frame's rig lights and maps. `limit` = mcla_gpu_vol_lamp_limit (0: the level's), `lamps_on` / `heads_on` = which of the two the volume reads. `list`: the newest copy of
  // the game's lamp list when the caller has one it also hands its own producer (the proof run, so that both pick from the same list), else null and the manager takes its own.
  // Call only when Serving(light_project::kFlipVolume).
  void FillVolumeInputs(const volumetrics::VolFrameContext& context, uint32_t limit, bool lamps_on, bool heads_on, volumetrics::VolInputs* out,
                        const std::vector<vol_lamps::GameLamp>* list);
  // 4c (mcla_gpu_light_frame bit 4): the lights the shadow replay picks cars and makes views from, made from the frame: the rig's lights of this frame as local_shadow::Light (each named for
  // its place in the rig), `out` has room for four; returns how many. Call only when Serving(light_project::kFlipShadowLights). RigLightCount() is how many lights the frame holds (the
  // replay's "a headlight is lit" test, which the rig's set answered before).
  uint32_t ShadowLights(local_shadow::Light out[4]);
  uint32_t RigLightCount() const;
  // 4c's proof, when Checking(): the lights the replay made from the rig's set (`legacy`) against the ones made from the frame, bit for bit.
  void CheckShadowLights(const local_shadow::Light* legacy, uint32_t legacy_count, const local_shadow::Light* from_frame, uint32_t from_frame_count);
  // 4b's proof, when Checking(): what FillVolumeInputs made (`from_frame`) against what the volume's own producer made from the same entry (`legacy`), byte for byte.
  void CheckVolumeBytes(const volumetrics::VolInputs& from_frame, const volumetrics::VolInputs& legacy);
  // The volume's inputs of the frame, at the end of VolumetricsInputs::Fill: `first_lamp` = how many lamps `in` held before the producer put the game's in (the core's test lamp),
  // `lamps_on` / `heads_on` = which of the two the producer filled.
  void NoteVolumeInputs(const volumetrics::VolFrameContext& context, const volumetrics::VolInputs& in, bool lamps_on, bool heads_on, uint32_t first_lamp);
  // The guest present, BEFORE the rig makes the next frame's set: the frame's own check, the upload, the census.
  void OnPresent();

  // A consumer of Frame() (the deferred local light pass, local_lights.h) asks for the frame while it is on: the manager then runs although mcla_gpu_lights_census is 0 (it collects, checks and
  // keeps the frame; no census line and no GPU upload, which the census's own run adds) and polls the cars' light points for it. Sticky: set it when the consumer's setting changes. Read at the
  // next BeginLights.
  void Want(bool wanted) {
    want_pass_ = wanted;
    wanted_ = want_pass_ || want_rt_;
  }
  // GI-BOUNCE-RT (rt_emitters_rules.h): the ray traced bounce light's emitters read the same frame (the headlights) and the cars' lamps. A flag of its own, so that the light pass's Want(false) of a frame in which its
  // pass is off does not take the manager away from the ray traced bounce light (the two are separate consumers and set their flags at different places). Sticky, read at the next BeginLights.
  void WantRt(bool wanted) {
    want_rt_ = wanted;
    wanted_ = want_pass_ || want_rt_;
  }
  // Whether the manager is running (the census, a flip or a consumer was on at the last BeginLights): the volume asks before it hands its inputs over.
  bool active() const { return active_; }
  // LU-4: whether the consumer of this flip (light_project.h kFlip*) is to read the frame now: the flip is set, the manager runs and the frame is open. False otherwise: the consumer
  // uses its own producer.
  bool Serving(uint32_t flip) const { return active_ && begun_ && (flips_ & flip) != 0; }
  // Whether the legacy producers' data is made too and compared with the frame's projection (the census is on): the proof run.
  bool Checking() const { return active_ && every_ > 0; }
  // 4a: the headlight block of this frame made from the frame (the frame's lights, the maps as of now), or null when Serving(kFlipHeadlightBlock) is false or the frame's maps are not
  // in yet. The camera in it is the rig's fallback; the Scene writes the draw's own over it. Valid until the next call after a change of the frame. Render thread.
  const headlight_rules::HeadlightConstants* HeadlightBlock();
  // The frame as it is now. Valid on the render thread between BeginFrame and OnPresent (the frame is complete once the volume has filled).
  const larecomp::gpu::LightFrame& Frame() const { return collector_.frame(); }
  // The cars' lamps as the last present's poll made them (car_lights.h; GB-H3's police, head and rear lamps read them): up to eight cars, the player's first and then the nearest to the reference,
  // one frame old when the pass reads them at the trigger. Empty while the manager is not running.
  const larecomp::gpu::car_lights::CarLightsFrame& Cars() const { return cars_.Frame(); }
  // The world point the cars are ranked by (the camera): a consumer of Cars() gives its own at its trigger and the poll at the present that follows uses it; without one the cars are ranked by
  // their key and a police car can be one of those dropped past eight. Cleared with the manager.
  void SetCarReference(const double xyz[3]) {
    for (int i = 0; i < 3; ++i) {
      car_reference_[i] = xyz[i];
    }
    have_car_reference_ = true;
  }

  // THE ONE PICKER (LIGHT-UNIFY U3, light_pick.h): the picker of each role and the budget, held here whether or not the census runs. The headlight rig's Tracker picks its four lights with
  // `headlights`, the shadow replay's ViewPicker picks the cars that get views with `shadow_cars` (mcla_gpu_light_pick = legacy runs their own first code instead); the two consumers
  // note the levels they resolved to into `budget` once a frame. Render thread.
  light_pick::LightPicker& Pickers() { return pickers_; }
  const light_pick::LightPicker& Pickers() const { return pickers_; }

 private:
  struct Window {
    uint32_t frames = 0, with_lights = 0, with_volume = 0;
    uint32_t max_headlights = 0, max_lamps = 0;
    uint64_t kept = 0, added = 0, gone = 0;
    uint64_t cpu_ns = 0, cpu_max_ns = 0;
    uint32_t max_cars = 0, max_car_lamps_on = 0, max_point_lights = 0;  // the cars' lights (LU-7), the most of the window
    uint64_t car_frames = 0;                                           // frames of the window in which the table gave any car
  };
  void RefreshCarry();
  void Upload();
  void Census();
  void PollCars();
  void Reset();

  UploadRing* upload_ = nullptr;
  HeadlightRig* rig_ = nullptr;
  LocalShadow* shadow_ = nullptr;
  ShadowAtlas* atlas_ = nullptr;   // LU-5: where the shadow views come from with bit 8
  uint64_t atlas_adopted_ = 0;     // frames whose shadow views were read from it

  light_pick::LightPicker pickers_;  // the one picker and budget (always: the rig and the replay pick with it whether or not the census runs)
  larecomp::gpu::light_manager::Collector collector_;
  larecomp::gpu::light_manager::ShadowCarry<larecomp::gpu::LocalShadowFrame> carry_;
  larecomp::gpu::light_manager::SelfCheck check_;

  int every_ = 0;
  bool wanted_ = false;        // a consumer of the frame asked for it (Want or WantRt: either of the two flags below)
  bool want_pass_ = false;     // the deferred light pass's
  bool want_rt_ = false;       // the ray traced bounce light's emitters'
  uint32_t flips_ = 0;         // mcla_gpu_light_frame as read at the last BeginLights (light_project.h kFlip*)
  bool active_ = false;
  bool begun_ = false;         // a frame has been opened and not yet presented
  bool shadows_begun_ = false; // ... and BeginFrame has put the last trigger's maps in it
  bool cars_on_ = false;       // the hook's every-kind notes are on (the census or a consumer of the cars' lamps is)
  bool volume_seen_ = false;  // the volume filled in this frame
  uint32_t upload_slot_ = 0;
  uint64_t frame_ = 0;
  uint64_t presents_ = 0;
  uint64_t frames_total_ = 0;

  // the game's lamp list, as the hook table gave the newest copy (the volume has its own copy and its own sequence number)
  std::vector<vol_lamps::GameLamp> list_;
  uint64_t list_seq_ = 0;

  // the cars' lights (LU-7): the source polls the hook's table once a frame; `car_frames_` is the renderer's cars (none yet); `point_lights_` the scratch of the police point lights the lamps make
  larecomp::gpu::car_lights::CarLightsSource cars_;
  std::vector<larecomp::gpu::car_lights::FrameIn> car_frames_;
  double car_reference_[3] = {0.0, 0.0, 0.0};  // SetCarReference
  bool have_car_reference_ = false;
  std::vector<larecomp::gpu::Light> point_lights_;
  uint32_t cars_now_ = 0, car_lamps_on_ = 0, car_point_lights_ = 0;

  // LU-4a: the projected headlight block, remade when the frame changes (the lights at BeginLights, the maps at BeginFrame and at the trigger)
  headlight_rules::HeadlightConstants block_{};
  bool block_dirty_ = true;
  uint64_t block_made_ = 0, block_draws_ = 0;  // for the census: projections made, draws that took the block

  void CheckBlocks(larecomp::gpu::light_manager::Check kind);
  bool RefreshList();

  // LU-4b: the manager's own lamp pick for the volume (the pick of VolumetricsInputs::FillLamps, run here while the flip is on), and what it took
  uint64_t shadow_picks_ = 0;       // 4c: picks the replay made from the frame
  volumetrics::VolumeLampPick vol_pick_;
  bool volume_from_frame_ = false;  // the volume's inputs of this frame were made from the frame: the frame's lamps are the pick's, not read back from the volume
  uint64_t volume_fills_ = 0;       // fills made from the frame

  // the volume's budgets of the last fill, for the census
  uint32_t grid_lamps_ = 0, grid_headlights_ = 0;
  uint32_t volume_lamp_count_ = 0, volume_headlight_count_ = 0;

  // the upload
  uint64_t uploads_ = 0;
  uint64_t last_upload_offset_ = 0;
  uint64_t upload_failed_ = 0;

  // the frame's own cost and the census window
  std::chrono::steady_clock::duration frame_cpu_{};
  Window window_;
  bool lights_seen_ = false;
};

}  // namespace mcla::gpu
