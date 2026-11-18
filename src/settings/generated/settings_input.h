#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class InputSettings final : public rex::SettingOwner {
 public:
  InputSettings() : SettingOwner("larecomp") {}

  // The one setting both effects used to share. Only read for old settings files: false there turns
  // trigger_resistance and trigger_vibration off at the first tick, and this goes back to true.
  REX_SETTING(bool, adaptive_triggers, true, "Old setting, replaced by trigger_resistance and trigger_vibration. False in an older settings "
    "file turns both off.");

  REX_SETTING(bool, controller_vibration, true, "Controller vibration. Off keeps the game's rumble from reaching any controller and stops a "
    "motor that is running.");

  REX_SETTING(bool, hide_mouse_pointer, true, "Hide the mouse pointer during play. It goes at once when a controller is "
    "used and after about 3 seconds without mouse movement, only over the game "
    "window while it has focus. Mouse movement, a click, the wheel or losing "
    "focus brings it back, and it stays while the mouse is over an overlay.");

  REX_SETTING(f64, impulse_trigger_strength, 50.0, "How strong the Xbox trigger rumble is, in percent of what the trigger motors can do "
    "(10 to 100).", rex::Range{10.0, 100.0});

  REX_SETTING(bool, impulse_triggers, true, "Xbox One and Xbox Series trigger rumble: the right trigger shakes while the driven wheels "
    "spin under power, the left while the brakes lock the wheels, harder the more they slip. "
    "Controllers without trigger motors, and a DualSense (which has its own settings), are not "
    "touched.");

  REX_SETTING(bool, input_log, false, "Log every key, mouse and controller event that the active-device rules looked at and what they "
    "decided (counted, or why it was ignored), at most 4000 lines. The switches themselves are always "
    "logged.", rex::kDebugOnly);

  REX_SETTING(bool, input_test_pad_loss, false, "Test only: set to true to act as if the controller in the player's hands had just been unplugged: the "
    "same rule as a real removal (only while a pad is the active device and the game is driven; the pad "
    "need not be real). The next pad poll takes it; set it to false and true again for another.", rex::kDebugOnly);

  REX_SETTING(bool, keyboard_native, true, "Native keyboard, always on. Feeds the keyboard and the mouse buttons straight into the game's own "
    "input layer (rage ioMapper) from the window's key events: menus and driving get separate key sets, "
    "so Enter can be 'accept' in a menu and Pause on the road without ever being both. A debug switch "
    "to tell whether a bug comes from it (off = the keyboard does nothing).", rex::kDebugOnly);

  REX_SETTING(std::string, keycaps_dir, "", "Folder of the 64x64 key cap PNGs the keyboard glyphs are painted from, named by "
    "lower-case key (return.png, escape.png, a.png, capslock.png ...). Empty = the "
    "set embedded in the executable (src/keycaps/). Built by tools/keycaps/pack_keycaps.py.", rex::kDebugOnly);

  REX_SETTING(f64, left_stick_deadzone, 0.0, "Dead zone of the left stick, in percent of its range: a stick that rests off centre (drift) "
    "reads as centred inside it. 0 = none.", rex::Range{0.0, 50.0});

  REX_SETTING(bool, map_mouse, true, "Drive the full map's cursor with the mouse. The pointer position is "
    "unprojected onto the map plane, so the cursor lands where you point "
    "instead of drifting like a stick would.");

  REX_SETTING(bool, map_mouse_diag, false, "Log the map screen pointer, camera basis and the world point the mouse "
    "unprojects to, once a second.", rex::kDebugOnly);

  REX_SETTING(bool, map_mouse_flip_x, false, "Mirror the horizontal axis of the map cursor. Needed only if the camera "
    "basis rows turn out to be left-handed -- the cursor then moves the wrong "
    "way along one axis while the other tracks correctly.", rex::kDebugOnly);

  REX_SETTING(bool, map_mouse_flip_y, false, "Mirror the vertical axis of the map cursor. See map_mouse_flip_x.", rex::kDebugOnly);

  REX_SETTING(f64, map_mouse_fov, 45.0, "Vertical field of view, in degrees, of the camera the full map is drawn "
    "with. The only number the unprojection cannot read out of the game. Too "
    "small and the cursor lags behind the pointer towards the screen edges; "
    "too large and it runs ahead. The centre of the screen is correct at any "
    "value, so calibrate by pointing at a corner.", rex::Range{5.0, 150.0}, rex::kDebugOnly);

  REX_SETTING(bool, map_mouse_hide_cursor, true, "Hide the OS pointer while the full map is up. The in-game cursor tracks "
    "it one to one, so the visible cursor is the game's own.");

  REX_SETTING(bool, map_mouse_magnet, false, "Leave the map's cursor magnet alone. The map snaps the cursor onto the "
    "selected icon, both on selecting it and again on every frame it stays "
    "selected, which is how a thumbstick is made to feel sticky over "
    "waypoints and what welds a pointer to the first one it brushes. Off "
    "suppresses both snaps for whichever device currently owns the cursor "
    "being the mouse -- a stick moved out of its deadzone takes ownership "
    "back and gets its magnet with it. The selection and its highlight are "
    "never touched either way, so hovering works regardless.");

  REX_SETTING(i32, map_mouse_max_steps, 96, "Ceiling on how many stick-sized steps one frame may walk the cursor "
    "towards the pointer. The map's boundary sweep is written for the small "
    "deltas a stick produces and reads a null neighbour pointer when handed a "
    "long chord, so the move is subdivided rather than teleported. Anything "
    "left over is covered by the next frame, so a ceiling too low shows up as "
    "a cursor that crawls after a fast flick rather than as anything worse. "
    "The walk stops as soon as it arrives or stops making progress, so a high "
    "ceiling costs nothing on the small movements that make up most frames.", rex::Range{1, 512}, rex::kDebugOnly);

  REX_SETTING(bool, map_mouse_zoom, true, "Zoom the full map with the scroll wheel. The camera height is the zoom, "
    "so this walks it through the game's own min/max at +1124/+1128.");

  REX_SETTING(f64, map_mouse_zoom_smooth, 0.22, "How much of the remaining distance to the wheel's target height the "
    "camera covers each frame. A detent is a step, and stepping the camera "
    "straight to it is what made the zoom read as jerky; this eases into it "
    "instead. 1.0 restores the instant jump.", rex::Range{0.02, 1.0}, rex::kDebugOnly);

  REX_SETTING(f64, map_mouse_zoom_step, 1.15, "Multiplier applied to the map camera's height per wheel detent.", rex::Range{1.01, 3.0}, rex::kDebugOnly);

  REX_SETTING(bool, map_mouse_zoom_to_cursor, true, "Zoom around the point under the pointer instead of around the middle of "
    "the view. With this off the camera only changes height, which slides the "
    "world under a pointer that has not moved and drags the cursor towards "
    "the centre on every detent.", rex::kDebugOnly);

  REX_SETTING(std::string, prompts_glyph_dump, "", "Debug: a folder; when the UI's texture dictionary is first found, every small DXT5 bitmap in it is written there as raw RGBA (<name>_<w>x<h>.rgba, plus index.txt), "
    "to see which bitmap is which glyph. Empty = off. Read once at start.", rex::kDebugOnly);

  REX_SETTING(std::string, prompts_ps_map, "", "Debug: which dictionary bitmaps hold the game's own PlayStation shoulder, Start and Back buttons, as SLOT=shared_N.dds pairs separated by commas "
    "(slots A B X Y LB RB LT RT Start Back Phone), e.g. \"LB=shared_8.dds,RB=shared_32.dds\". A slot with no entry here or in the built-in table (prompt_glyphs.h) "
    "gets the shipped picture (src/keycaps/ps_*.png). Used to try candidates without a build.", rex::kDebugOnly);

  REX_SETTING(f64, right_stick_deadzone, 0.0, "Dead zone of the right stick, in percent of its range: a stick that rests off centre (drift) "
    "reads as centred inside it. 0 = none.", rex::Range{0.0, 50.0});

  REX_SETTING(i32, script_pad_latch_ms, 100, "A script that asks whether a button was pressed only every few updates (the title screen's "
    "Start above 30 FPS) misses a press that comes in an update it does not look in. It is told "
    "about that press, at its next look, if its previous look was at most this many milliseconds "
    "before. Without it the button has to be pressed several times. 0 = the game's own "
    "behaviour: a press is seen only by a script that looks in the update it happened in.", rex::Range{0, 500}, rex::kDebugOnly);

  REX_SETTING(i32, trigger_brake, 6, "L2 resistance at the end of its travel, 1 (light) to 8 (firm). Used by trigger_resistance.", rex::Range{1, 8});

  REX_SETTING(bool, trigger_resistance, true, "DualSense trigger resistance: a throttle spring on R2 and brake resistance on L2 while a car "
    "is driven, slack everywhere else. Other controllers are not touched.");

  REX_SETTING(bool, trigger_slip_log, false, "Development: once a second while a car is driven, write each wheel's ground speed, slip speed "
    "and the wheelspin and lock-up amounts the trigger effects use to the log.", rex::kDebugOnly);

  REX_SETTING(i32, trigger_throttle, 3, "R2 resistance at the end of its travel, 1 (light) to 8 (firm). Used by trigger_resistance.", rex::Range{1, 8});

  REX_SETTING(bool, trigger_vibration, true, "DualSense trigger vibration: R2 vibrates while the driven wheels spin under power and L2 while "
    "the brakes lock the wheels, harder the more they slip. Other controllers are not touched.");

};

inline InputSettings g_input;

inline auto& adaptive_triggers = g_input.adaptive_triggers;
inline auto& controller_vibration = g_input.controller_vibration;
inline auto& hide_mouse_pointer = g_input.hide_mouse_pointer;
inline auto& impulse_trigger_strength = g_input.impulse_trigger_strength;
inline auto& impulse_triggers = g_input.impulse_triggers;
inline auto& input_log = g_input.input_log;
inline auto& input_test_pad_loss = g_input.input_test_pad_loss;
inline auto& keyboard_native = g_input.keyboard_native;
inline auto& keycaps_dir = g_input.keycaps_dir;
inline auto& left_stick_deadzone = g_input.left_stick_deadzone;
inline auto& map_mouse = g_input.map_mouse;
inline auto& map_mouse_diag = g_input.map_mouse_diag;
inline auto& map_mouse_flip_x = g_input.map_mouse_flip_x;
inline auto& map_mouse_flip_y = g_input.map_mouse_flip_y;
inline auto& map_mouse_fov = g_input.map_mouse_fov;
inline auto& map_mouse_hide_cursor = g_input.map_mouse_hide_cursor;
inline auto& map_mouse_magnet = g_input.map_mouse_magnet;
inline auto& map_mouse_max_steps = g_input.map_mouse_max_steps;
inline auto& map_mouse_zoom = g_input.map_mouse_zoom;
inline auto& map_mouse_zoom_smooth = g_input.map_mouse_zoom_smooth;
inline auto& map_mouse_zoom_step = g_input.map_mouse_zoom_step;
inline auto& map_mouse_zoom_to_cursor = g_input.map_mouse_zoom_to_cursor;
inline auto& prompts_glyph_dump = g_input.prompts_glyph_dump;
inline auto& prompts_ps_map = g_input.prompts_ps_map;
inline auto& right_stick_deadzone = g_input.right_stick_deadzone;
inline auto& script_pad_latch_ms = g_input.script_pad_latch_ms;
inline auto& trigger_brake = g_input.trigger_brake;
inline auto& trigger_resistance = g_input.trigger_resistance;
inline auto& trigger_slip_log = g_input.trigger_slip_log;
inline auto& trigger_throttle = g_input.trigger_throttle;
inline auto& trigger_vibration = g_input.trigger_vibration;

}  // namespace larecomp::settings
