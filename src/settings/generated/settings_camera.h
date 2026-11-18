#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class CameraSettings final : public rex::SettingOwner {
 public:
  CameraSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, cam_freelook, true, "Mouse look on the gameplay camera. The shipped lookaround is four digital pan actions "
    "easing towards clamped limits, which is why the camera settles on a handful of angles "
    "instead of following you. With this on, the mouse steers the view directly while you drive "
    "with the keyboard and mouse: the pointer is hidden and locked (the window's relative mouse "
    "mode, raw counts from the mouse) and the view eases back behind the car when the mouse "
    "has been still for a moment. It is active only while the keyboard and mouse are the last "
    "thing you used, with the car on the road: no menu, map, phone or overlay up, and the window "
    "in front. A controller's right stick looks around as it always did.");

  // Both default to 0 = leave the shipped value alone. Their units are not known
  // -- "rate" reads as per-second and "factor" as a 0..1 blend, but neither is
  // confirmed -- and a wrong value here is indistinguishable from the whole
  // approach not working. Changing one thing at a time is the point.
  REX_SETTING(f64, cam_freelook_approach, 0.0, "LookaroundApproachRate while free look is on. 0 leaves the shipped value. "
    "The shipped one eases the view towards its target over a good fraction of "
    "a second, which reads as lag under a mouse.", rex::Range{0.0, 200.0}, rex::kDebugOnly);

  REX_SETTING(bool, cam_freelook_collide, false, "Turn the camera one stage earlier, at the camera state (0x8231CEB0), so "
    "the game's own camera collision resolves the rotated position and the "
    "view stops passing through walls and buildings. OFF by default because it "
    "is unproven: it hands the look to sub_8231CE30, and any rig whose camera "
    "state does not route through there gets no free look at all while this is "
    "on. Turn cam_freelook_diag on with it -- the log says whether the hook "
    "ran, which camera state it got, and what CollideType that state carries. "
    "With this off, cam_look_phase decides everything, exactly as before.", rex::kDebugOnly);

  REX_SETTING(bool, cam_freelook_cs_invert, false, "Flip the sign used when feeding the camera state's own two angle channels "
    "(cs+776 yaw, cs+772 pitch). The offset and direction were derived from "
    "sub_82202E38 and sub_82227DE0 -- both store `pi/2 - angle`, so a turn is a "
    "subtraction -- but which way that reads on screen is a handedness question "
    "the maths does not settle. If the view turns opposite to the mouse with "
    "cam_freelook_collide on, this is the switch. Only affects the collide "
    "path.", rex::kDebugOnly);

  REX_SETTING(bool, cam_freelook_diag, false, "Log the lookaround tune fields as they are found and the angles being "
    "written, once a second.", rex::kDebugOnly);

  // A camera bolted to the car -- cockpit, bumper, bonnet -- has nothing to
  // orbit: it turns where it sits, like a head. Swinging it round a centre seven
  // metres away, which is right for a chase camera, walks it straight out
  // through the bodywork.
  //
  // Which rigs those are cannot be worked out from here. The honest signal would
  // be the camera's real distance to the car, and that distance is exactly what
  // this feature never manages to find out -- it is why the orbit radius is a
  // cvar in the first place. So it is a list, and the diagnostic prints the rig
  // number every time it applies, which makes filling the list a matter of
  // reading one line per view.
  //
  // r24 in sub_822C0320 is the rig the camera system settled on this frame, and
  // Hook_CameraSystem has been recording it since the first probe. Per the note
  // on Patch_DebugCam it is 1 for the cockpit view and 2 for a static one.
  REX_SETTING(std::string, cam_freelook_inplace_rigs, "0,1", "Comma-separated camera rig indices that turn in place instead of "
    "orbiting: the views mounted on the car. Switch to a view, read the rig "
    "number out of the cam_freelook_diag line, add it here. Empty orbits "
    "everything.\n"
    "\n"
    "The camera button cycles 3, 2, 1, 0 and wraps; 4 belongs to some other "
    "context and turns up on its own. 1 is the cockpit, and 0 is the bumper "
    "sitting next to it in the cycle -- which is a reading of the cycle order, "
    "not something measured, so if a view mounted on the car still swings out "
    "of the bodywork, its number belongs in this list.", rex::kDebugOnly);

  REX_SETTING(bool, cam_freelook_invert_x, false, "Mirror the horizontal axis. Needed if the tune's left/right limits turn "
    "out to be signed the other way round from what this assumes.");

  REX_SETTING(bool, cam_freelook_invert_y, false, "Mirror the vertical axis. See cam_freelook_invert_x.");

  REX_SETTING(f64, cam_freelook_lerp, 0.0, "LookaroundLerpFactor while free look is on. 0 leaves the shipped value.", rex::Range{0.0, 1.0}, rex::kDebugOnly);

  REX_SETTING(f64, cam_freelook_max_elev, 1.3, "Highest the camera may go, same units. Short of pi/2 on purpose: at dead "
    "overhead the forward vector is parallel to world up and the yaw stops "
    "meaning anything.", rex::Range{0.2, 1.5}, rex::kDebugOnly);

  // Rotating the finished matrix means the game's camera collision never sees
  // the result, so the floor has to be put back by hand. It states cleanly in
  // geometry: pos = centre - forward * d with d negative, so the camera sits
  // forward.y * |d| above the point it orbits and its elevation is
  // asin(forward.y). Clamping that angle is exactly clamping how far under the
  // car the view can get. Orbiting rigs only -- in place the position never
  // moves, so there is nothing to keep out of the ground.
  REX_SETTING(f64, cam_freelook_min_elev, -0.08, "Lowest the camera may go, as an angle in radians above the horizontal "
    "through the point it orbits. 0 is dead level with the car; below that it "
    "goes under it and into the ground. Negative values are allowed for "
    "deliberately low shots, on the understanding that the ground is not "
    "consulted.", rex::Range{-0.5, 1.0}, rex::kDebugOnly);

  REX_SETTING(f64, cam_freelook_orbit, 7.5, "Distance from the camera to the point it turns around, used whenever the "
    "object's own +156 is not usable.\n"
    "\n"
    "It usually is not. +156 was measured at -7.34 during a held look and that "
    "is where this default comes from, but it is the w lane of the position "
    "row and the game computes it the same way it computes the angle at +140 "
    "-- both read zero whenever the game's own look is idle, which is exactly "
    "when this feature is doing the looking instead. The field still wins when "
    "it holds something sane, so a camera that keeps a real distance there is "
    "followed rather than overridden.", rex::Range{1.0, 200.0}, rex::kDebugOnly);

  REX_SETTING(f64, cam_freelook_pitch_limit, 0.7, "How much tilt the mouse may accumulate, in radians. This caps the input, "
    "not the result -- where the camera ends up is decided by the two "
    "elevation limits below, which are what keep it out of the ground.", rex::Range{0.05, 1.5}, rex::kDebugOnly);

  // The shipped camera recentres itself: LookaroundDelayTime then
  // LookaroundDecayRate. Not reproducing that was the difference between a free
  // look and a view that silently accumulates every stray twitch of the mouse
  // until the car is sitting crooked and there is no way to put it back.
  REX_SETTING(f64, cam_freelook_return_delay, 0.35, "Seconds the mouse has to sit still before the view starts easing back "
    "behind the car. The game's own equivalent is LookaroundDelayTime. Set it "
    "very high to keep the view wherever it was left.", rex::Range{0.0, 30.0});

  REX_SETTING(f64, cam_freelook_return_rate, 4.0, "How quickly the view eases back once the delay is up, as a fraction of "
    "the remaining angle per second. The game's own equivalent is "
    "LookaroundDecayRate.", rex::Range{0.1, 40.0});

  // The sensitivity rule (mouse_look_core.h): radians of view per RAW mouse count. At Windows' default pointer speed one count was one cursor pixel, which is what this
  // used to be measured in, so 0.0035 keeps the same feel at default sensitivity whatever the mouse's DPI is (360 degrees = 1795 counts: 5.7 cm at 800 DPI, 2.8 cm at 1600).
  REX_SETTING(f64, cam_freelook_sens, 0.0035, "Mouse look sensitivity: radians of view movement per count the mouse reports. 0.0035 "
    "turns the view a full circle for about 6 cm of movement on an 800 DPI mouse and about 3 cm "
    "on a 1600 DPI one; a mouse with a higher DPI turns further for the same movement, so "
    "lower this for it.", rex::Range{0.0001, 0.05});

  REX_SETTING(bool, cam_freelook_tune, false, "Also pin the LookaroundLimit* tune fields, the first approach. It did not "
    "hold on its own and is kept only so the two can be tried together.", rex::kDebugOnly);

  REX_SETTING(f64, cam_freelook_yaw_limit, 3.14159, "How far the view may turn left or right, in radians. Pi is all the way "
    "round. This is the cap on our own angle, not the game's -- the game's own "
    "limits are what this feature overwrites.", rex::Range{0.1, 3.15}, rex::kDebugOnly);

  // ── Driving the camera object the probe found ──────────────────────────
  //
  // The measurement landed on one object -- camsys+860, which came out as
  // 0xBD910980 -- and on fourteen floats inside it, at +96 through +156. Three
  // rows of unit-length vectors at +96/+112/+128 and a world position at +144:
  // the camera's own matrix, rotating about Y and orbiting as the view swings.
  //
  // The two floats that are not part of it are the interesting ones. +140 and
  // +156 are the w lanes of the third and fourth rows, which is where RAGE
  // habitually parks a scalar it did not want to give a field of its own, and
  // they went from about zero to -0.947685 and -7.34199 -- an angle in radians
  // and, to within the accuracy of reading a matrix, the radius the position
  // orbited on.
  //
  // Whether +140 is what the camera READS to build that matrix or what it WROTE
  // afterwards is the only thing left to settle, and no amount of staring
  // settles it. So the offsets are cvars, the write happens inside the camera
  // tick before the update runs, and the value is read back on the next frame
  // and compared against what was written. That answers it in one run and lets
  // other candidates be tried without a rebuild.
  REX_SETTING(i32, cam_look_obj, 860, "Byte offset into the camera system holding the pointer to the object the "
    "look lives in. 860 is what the field probe found.", rex::Range{0, 4096}, rex::kDebugOnly);

  REX_SETTING(i32, cam_look_phase, 2, "Which matrix the look turns. 2 is the real one: the camera matrix "
    "sub_8220AA68 is handed at 0x8220AA88, before it has read a byte of "
    "it, from which the game publishes to dword_8286D804+4208 and +4272 "
    "and mirrors into the cine director. 0 and 1 turn that mirror "
    "instead, at 0x822C0790 and at the camera tick epilogue, and are "
    "kept only because they were tried first -- sub_823CC2E8 is a plain "
    "setter that copies the real matrix into it, so writing there is "
    "writing to a copy and nothing on screen ever changed. The camera "
    "collision lives on its own switch, cam_freelook_collide, and not on "
    "this one -- see there.", rex::Range{0, 2}, rex::kDebugOnly);

  REX_SETTING(f64, cam_probe_arm_delay, 4.0, "Seconds between arming cam_probe_run and the baseline sample. Long "
    "enough to close the overlay and let the camera settle.", rex::Range{1.0, 30.0}, rex::kDebugOnly);

  REX_SETTING(bool, cam_probe_dump, false, "Button: snapshot the camera again, diff it against cam_probe_mark and "
    "write every float that moved to <exe>/cam_fields.txt. Flips back off.", rex::kDebugOnly);

  REX_SETTING(f64, cam_probe_hold_delay, 6.0, "Seconds between the baseline sample and the diff. Hold the look for all "
    "of it.", rex::Range{1.0, 30.0}, rex::kDebugOnly);

  REX_SETTING(bool, cam_probe_mark, false, "Button: snapshot the gameplay camera object. Flips back off. Do it with "
    "the car still and the view centred, then look around, then use "
    "cam_probe_dump.", rex::kDebugOnly);

  // The two buttons above cannot be used for the measurement they were written
  // for: reaching them means opening the settings overlay, and opening it takes
  // the game out of whatever camera state was being held. This runs the whole
  // sequence on a timer instead, so the overlay is only ever open while the
  // camera is idle and both samples are taken with it shut.
  REX_SETTING(bool, cam_probe_run, false, "Button: run the whole camera measurement on a timer. Close the overlay "
    "when it is armed; the baseline is taken after cam_probe_arm_delay with "
    "the camera left alone, and the diff cam_probe_hold_delay after that with "
    "the look held. Both samples happen with nothing on screen. Flips back "
    "off.", rex::kDebugOnly);

  REX_SETTING(f64, camera_shake_scale, 0.6, "Camera shake intensity multiplier (0.0 = disabled, 0.6 = balanced PC rumble, 1.0 = full console shake).", rex::Range{0.0, 3.0});

  REX_SETTING(f64, chase_cam_smoothing_factor, 1.0, "Chase camera boom smoothing factor multiplier (0.1 - 3.0).", rex::Range{0.1, 3.0});

  REX_SETTING(std::string, debug_cam, "off", "Free-fly camera during live gameplay: left stick moves, right stick looks, triggers change "
    "speed. Gameplay keeps running underneath (drive, traffic, physics).", rex::Allowed("off", "free"), rex::kDebugOnly);

  REX_SETTING(bool, debug_cam_mouse, true, "Free-fly camera: use the mouse to look (the pointer is hidden and locked while it is active).", rex::kDebugOnly);

  REX_SETTING(f64, debug_cam_mouse_sens, 0.0025, "Free-fly mouse look sensitivity (radians per raw mouse count).", rex::Range{0.0002, 0.02}, rex::kDebugOnly);

  REX_SETTING(f64, debug_cam_sens, 2.5, "Free-fly camera look sensitivity (radians/second at full stick).", rex::Range{0.2, 10.0}, rex::kDebugOnly);

  REX_SETTING(f64, debug_cam_speed, 40.0, "Free-fly camera move speed (world units/second).", rex::Range{1.0, 500.0}, rex::kDebugOnly);

  REX_SETTING(f64, fov_1p_scale, 1.0, "FOV scale — 1st person / cockpit (0.5 = narrower, 2.0 = wider)", rex::Range{0.5, 5.0});

  REX_SETTING(f64, fov_3p_scale, 1.0, "FOV scale — 3rd person / chase cam (0.5 = narrower, 2.0 = wider)", rex::Range{0.5, 5.0});

  REX_SETTING(bool, mouse_look_log, false, "Log what the mouse look does: the raw mouse counts and the view angles about every tenth of a second while it moves "
    "(at most 4000 lines). The changes of relative mouse mode are always logged.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Continuous-time camera shake scaling
  REX_SETTING(bool, scale_camera_shake, true, "Fix: Scale camera shake intensity across framerates (60+ FPS) to prevent violent noise jitter during drifts, burnouts, and crashes.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Smooth camera transition interpolation & wrap fix
  REX_SETTING(bool, smooth_camera_transitions, true, "Fix: Eliminate camera transition jitter and 180-degree wrap flipping when transitioning between orbit, menu, and gameplay cameras.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Continuous exponential camera boom smoothing at 60 FPS
  REX_SETTING(bool, smooth_chase_cam, true, "Fix: Smooth chase camera boom interpolation at 60 FPS using continuous-time exponential decay.", rex::kDebugOnly);

};

inline CameraSettings g_camera;

inline auto& cam_freelook = g_camera.cam_freelook;
inline auto& cam_freelook_approach = g_camera.cam_freelook_approach;
inline auto& cam_freelook_collide = g_camera.cam_freelook_collide;
inline auto& cam_freelook_cs_invert = g_camera.cam_freelook_cs_invert;
inline auto& cam_freelook_diag = g_camera.cam_freelook_diag;
inline auto& cam_freelook_inplace_rigs = g_camera.cam_freelook_inplace_rigs;
inline auto& cam_freelook_invert_x = g_camera.cam_freelook_invert_x;
inline auto& cam_freelook_invert_y = g_camera.cam_freelook_invert_y;
inline auto& cam_freelook_lerp = g_camera.cam_freelook_lerp;
inline auto& cam_freelook_max_elev = g_camera.cam_freelook_max_elev;
inline auto& cam_freelook_min_elev = g_camera.cam_freelook_min_elev;
inline auto& cam_freelook_orbit = g_camera.cam_freelook_orbit;
inline auto& cam_freelook_pitch_limit = g_camera.cam_freelook_pitch_limit;
inline auto& cam_freelook_return_delay = g_camera.cam_freelook_return_delay;
inline auto& cam_freelook_return_rate = g_camera.cam_freelook_return_rate;
inline auto& cam_freelook_sens = g_camera.cam_freelook_sens;
inline auto& cam_freelook_tune = g_camera.cam_freelook_tune;
inline auto& cam_freelook_yaw_limit = g_camera.cam_freelook_yaw_limit;
inline auto& cam_look_obj = g_camera.cam_look_obj;
inline auto& cam_look_phase = g_camera.cam_look_phase;
inline auto& cam_probe_arm_delay = g_camera.cam_probe_arm_delay;
inline auto& cam_probe_dump = g_camera.cam_probe_dump;
inline auto& cam_probe_hold_delay = g_camera.cam_probe_hold_delay;
inline auto& cam_probe_mark = g_camera.cam_probe_mark;
inline auto& cam_probe_run = g_camera.cam_probe_run;
inline auto& camera_shake_scale = g_camera.camera_shake_scale;
inline auto& chase_cam_smoothing_factor = g_camera.chase_cam_smoothing_factor;
inline auto& debug_cam = g_camera.debug_cam;
inline auto& debug_cam_mouse = g_camera.debug_cam_mouse;
inline auto& debug_cam_mouse_sens = g_camera.debug_cam_mouse_sens;
inline auto& debug_cam_sens = g_camera.debug_cam_sens;
inline auto& debug_cam_speed = g_camera.debug_cam_speed;
inline auto& fov_1p_scale = g_camera.fov_1p_scale;
inline auto& fov_3p_scale = g_camera.fov_3p_scale;
inline auto& mouse_look_log = g_camera.mouse_look_log;
inline auto& scale_camera_shake = g_camera.scale_camera_shake;
inline auto& smooth_camera_transitions = g_camera.smooth_camera_transitions;
inline auto& smooth_chase_cam = g_camera.smooth_chase_cam;

}  // namespace larecomp::settings
