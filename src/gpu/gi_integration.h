#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's integration (CITY-RT M3, package G4 of perf/CITY-RT-M3.md sections 4.3, 9)
// ===========================================================================
// G1 made the field (gi_field.h), G3 the apply (gi_apply.h); this is what ties them to the setting and to the frame, so that "RT BOUNCE LIGHT" does something:
//
//   BeginFrame(frame)   at the scene's frame start, BEFORE GiApply::BeginFrame (which reads GiField::running() to make its wants): reads the setting (gi_setting.h Current: the word, the class, the ray tracing tier,
//                       the gate), tells gi_setting.h whether the field replaced the screen-space bounce light in the last frame (the BOUNCE LIGHT row's "(REPLACED)"), and starts the field's frame with
//                       its Inputs: the rung and level, whether the world is the play view's, the camera and the light of the last trigger, M1's safe range, the bounce light level's strength, the probes' feedback.
//   OnResolve(...)      at the mid-scene depth copy, the FIRST copy of the main view's depth in a frame, BEFORE the occlusion's OnResolve (which asks GiApply::TakeSsgiShare while it plans): notes the world
//                       the frame is in and latches the pose and the light for the next BeginFrame, then, while the field runs, RecordMaintenance, the probe update (G2: SetUpdate), RecordFinish: the order
//                       the field's header fixes. The field is published before the occlusion plans, so the occlusion knows whether the screen-space bounce light may be skipped.
//
// WHERE THE SCREEN-SPACE BOUNCE LIGHT GOES (the crossfade and the fallback; the rules are G3's, gi_apply_rules.h SsgiShareFor / FieldShare): it runs whole where there is no field (the setting is off,
// the card cannot trace, the world is the pause menu's, the garage's or the map's, M1 has no structures standing: safe range 0), at (1 - fade) while the field fades in (0.4 s after it is ready: the
// finest cascade mostly traced; a cut makes it start again) and out (0.5 s where the world stops being usable), and not at all once the field stands. This class adds the two things that decision needs and G3
// could not know:
//   * THE WORLD. The field exists only in the play view: the main view's depth at the game view's samples (2, or 1 with no MSAA), seen at the last trigger and not more than a few frames ago; the pause menu's,
//     the race pause's, the garage's and the map's world are the 4 sample identity (temporal_identities.h kPauseSamples), where SSGI runs as before (menu_world.h).
//   * THE WATCHDOG. A field that stands but cannot draw its light (GiApply::Starved: no G-buffer planes in a cutscene, no pipeline, no camera) must not leave the picture without any bounce light: the field is idled
//     for kStarvedHoldFrames (it fades out, the screen-space bounce light crossfades back) and tried again after.
//
// THE PROBE UPDATE (G2) IS A SEAM: SetUpdate gives the integration the function that traces and blends (it is called between RecordMaintenance and RecordFinish, with the update's frame), and says the update
// exists (gi_setting.h NoteUpdateBuilt: until then the RT BOUNCE LIGHT row is grey with "(NOT READY)", the setting resolves to off and no field is made). G2 (gi_update.h GiUpdate) calls it in the renderer's start,
// where the device can trace and its layouts and shaders exist, with its Record as the update and its OnWant as the want.
// mcla_gpu_rt_gi_dry_run (debug) opens the gate without an update, to run the field's owner and the frame's order alone (nothing traces into the probes: the field never becomes ready, the screen-space bounce
// light stays the source, 75 MiB are held at HIGH on a strong card).
//
// THREADING: render thread only, except SetUpdate (the renderer's start, before the first frame).
//
// CHECK (nothing here was run: no build, no game):
//   G4a  the first frame's order: GiField::BeginFrame, GiApply::BeginFrame, then the trigger's RecordMaintenance / RecordFinish before the occlusion's OnResolve: Scene::BeginFrame and OnResolve have the calls
//   G4b  the trigger is the first depth copy of the MAIN view's depth in a frame (temporal::MainDepthSamples of the source's key != 0), the same copy the occlusion's Judge qualifies; a frame with no such copy has no field
//        (the occlusion would not have run either)
//   G4c  the camera and the light are the last trigger's (AmbientOcclusion::TriggerViewInverse, FrameLight): the field's windows follow the camera one frame late, which the cascades' 2 to 16 m cells never show
//   G4d  GiField::Inputs' ambient_down is the same registers as ambient_up (PS c26): the car hit's frame rows have a c57 sky colour that this branch does not read; only the apply's baseline reads the up colour
//   G4e  the game's cutscenes: the G-buffer has no planes there, so the watchdog is what brings SSGI back after 8 frames; if the cutscene's first frames show no bounce light, world_ok can read the G-buffer
//   G4f  a camera cut the game asked for (the trailer toolkit's shots: Scene::NoteAnchorCut, cut_request.h) is a teleport for the field (NoteRequestedCut, Inputs::cut = kTeleport on the next frame's start: every
//        probe and the pixel pass's history start again, whatever the camera's step); any other jump is a cut by the camera's step only (GiField::Move, 200 m, by design: the probes are fixed to the world, a jump under
//        200 m moves the windows and keeps the data that is still in them). A time of day jump (kRelight) has no signal in the renderer yet: the old light fades by the update's hysteresis (about 33 updates)
// ===========================================================================

#include <cstdint>
#include <functional>

#include <plume_render_interface.h>

#include "gi_field.h"
#include "gi_setting.h"
#include "resolve.h"

namespace mcla::gpu {

class AmbientOcclusion;
class CityRtScene;
class GiApply;

class GiIntegration {
 public:
  // A field this many frames after the last trigger that saw the play view is not usable (a loading screen, a menu with no 3D view): the trigger comes once a guest frame.
  static constexpr uint64_t kWorldStaleFrames = 4;
  // After the watchdog (GiApply::Starved) the field idles for this many frames (4 s at 60) and is tried again.
  static constexpr uint64_t kStarvedHoldFrames = 240;

  // Not owned. `city` may be null (no ray tracing structures: no safe range, no field).
  void Initialize(GiField* field, GiApply* apply, AmbientOcclusion* ao, CityRtScene* city);
  void Shutdown();

  // The probe update (package G2): called between RecordMaintenance and RecordFinish while the field runs, with the field's update frame (the same table the readers get, the products marked present). Giving
  // one says the update exists; an empty function takes it away.
  using UpdateFn = std::function<void(plume::RenderCommandList* list, uint32_t frame_slot, const gi::GiFrame& update_frame)>;
  // The update's other seam (G2: gi_update.h OnWant): called every frame in BeginFrame, after the setting is read and BEFORE the field's own BeginFrame, with the level the setting comes to (off where the gate is
  // shut). The update asks the ray tracing scene service for the city's structures and hit data with it: without them there is no safe range and the field never starts.
  using WantFn = std::function<void(gi::Level level)>;
  void SetUpdate(UpdateFn update, WantFn want = nullptr);

  // Scene::BeginFrame, before GiApply::BeginFrame. `frame` is the scene's frame serial.
  void BeginFrame(uint64_t frame);
  // Scene::OnResolve, for every resolve, before the occlusion's OnResolve. True when the field's frame was recorded (the caller's list state is gone then, as for the other passes).
  bool OnResolve(const ResolveCache::Summary& summary, plume::RenderCommandList* list, uint32_t frame_slot);

  // Scene, at the anchor draw: the game asked for a camera cut at this frame (cut_request.h). The next BeginFrame hands the field a teleport (GiField::Inputs::cut), once.
  void NoteRequestedCut() { cut_pending_ = true; }

  // What the frame knows (for the tests of the order and the log).
  bool world_play() const { return world_play_; }
  bool idle_by_watchdog(uint64_t frame) const { return frame < idle_until_; }

 private:
  void LatchPoseAndLight();
  void LogDecision(const gi_setting::Decision& decision);

  GiField* field_ = nullptr;
  GiApply* apply_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  CityRtScene* city_ = nullptr;
  UpdateFn update_;
  WantFn want_;

  uint64_t frame_ = 0;
  uint64_t trigger_frame_ = 0;       // the last frame whose trigger was seen (once a frame)
  uint64_t world_frame_ = 0;         // ... and the frame the last main view depth copy was seen in
  bool world_play_ = false;          // ... and it was the play view's (not the 4 sample pause / garage / map identity)
  uint64_t idle_until_ = 0;          // the watchdog's hold: no field before this frame
  uint64_t watchdog_trips_ = 0;
  bool cut_pending_ = false;         // the game asked for a camera cut at the last anchor: the next frame's Inputs say so (once)

  // The last trigger's camera and light (the next frame's Inputs).
  bool have_camera_ = false;
  double camera_[3] = {0, 0, 0};
  bool have_light_ = false;
  float sun_rgb_[3] = {0, 0, 0};
  float ambient_rgb_[3] = {0, 0, 0};
  float sun_sin_elevation_ = 0.0f;

  // The decision last logged (the line is written when it changes).
  bool logged_any_ = false;
  gi::Level logged_level_ = gi::Level::kOff;
  gi::Layout logged_layout_ = gi::Layout::kNone;
  gi_setting::Gate logged_gate_ = gi_setting::Gate::kNoRt;
};

}  // namespace mcla::gpu
