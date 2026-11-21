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
// It is PASSIVE: NO CONSUMER READS IT. The lit shaders' block, the volume's lamps and headlights and the replay's maps are made and read exactly as before; the manager only looks (its
// calls take const references, the one non-const call is HeadlightRig::Block(), which rebuilds the block lazily if the replay's maps changed since: the same rebuild the first lit draw
// would do, and the camera in the block is overwritten per draw). The COLLECTING does not pick: the lamps in the frame are the ones the volume's picker gave. The picking of the other two roles IS here since LU-3 (light_pick.h): the manager holds the picker of the
// rig's four headlights and of the cars that get shadow views, and the one budget (Pickers(), always, with or without the census); the street lamps' pick (the volume's LampPicker, the lamp
// shadows') is reserved to move in with LU-4 / LU-5.
//
// THE SWITCH. mcla_gpu_lights_census N (debug only, default 0: the manager is not called, the cost is one setting read a frame). N > 0: it runs every guest frame, the census line is
// every N guest frames (300 is the usual), a difference found by a check is logged at once (the first three of each kind) and counted in the next census line. Live.
//
// THREADING. Render thread only. Initialize and Shutdown on the lifecycle thread.
//
// WHERE IT IS CALLED (4 places; scene.cpp and volumetrics_inputs.cpp):
//   Scene::BeginFrame           after the rig's and the replay's BeginFrame       -> BeginFrame   the frame's lights and the last trigger's maps; the block check at the start
//   Scene::OnResolve            after the replay's OnResolve                      -> OnResolve    the trigger (this frame's maps); the block check after it
//   VolumetricsInputs::Fill     at its end                                        -> NoteVolumeInputs   the lamps; the volume checks
//   Scene::OnPresent            before the rig makes the next frame's set         -> OnPresent    the frame's check, the upload, the census
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
#include "light_manager_rules.h"
#include "light_pick.h"
#include "local_shadow_types.h"
#include "vol_lamp_list.h"
#include "volumetrics_types.h"

namespace mcla::gpu {

class HeadlightRig;
class LocalShadow;
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

  // The Scene opened a frame's first list: reads the setting; with it on, collects the frame's headlights and the last trigger's maps and checks the block. `frame` is the Scene's
  // frame serial, `upload_slot` the frame's slot in the upload ring. A second list of the same guest frame is ignored.
  void BeginFrame(uint64_t frame, uint32_t upload_slot);
  // After the replay's OnResolve of every resolve (`shadow_recorded` is its return value): when it was the trigger, this frame's maps; then the block is checked again.
  void OnResolve(bool shadow_recorded);
  // The volume's inputs of the frame, at the end of VolumetricsInputs::Fill: `first_lamp` = how many lamps `in` held before the producer put the game's in (the core's test lamp),
  // `lamps_on` / `heads_on` = which of the two the producer filled.
  void NoteVolumeInputs(const volumetrics::VolFrameContext& context, const volumetrics::VolInputs& in, bool lamps_on, bool heads_on, uint32_t first_lamp);
  // The guest present, BEFORE the rig makes the next frame's set: the frame's own check, the upload, the census.
  void OnPresent();

  // Whether the manager is running (the setting was on at the last BeginFrame): the volume asks before it hands its inputs over.
  bool active() const { return active_; }
  // The frame as it is now. Valid on the render thread between BeginFrame and OnPresent (the frame is complete once the volume has filled).
  const larecomp::gpu::LightFrame& Frame() const { return collector_.frame(); }

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

  light_pick::LightPicker pickers_;  // the one picker and budget (always: the rig and the replay pick with it whether or not the census runs)
  larecomp::gpu::light_manager::Collector collector_;
  larecomp::gpu::light_manager::ShadowCarry<larecomp::gpu::LocalShadowFrame> carry_;
  larecomp::gpu::light_manager::SelfCheck check_;

  int every_ = 0;
  bool active_ = false;
  bool begun_ = false;        // a frame has been opened and not yet presented
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
  std::vector<larecomp::gpu::Light> point_lights_;
  uint32_t cars_now_ = 0, car_lamps_on_ = 0, car_point_lights_ = 0;

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
