#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class InputKeyboardDrivingSettings final : public rex::SettingOwner {
 public:
  InputKeyboardDrivingSettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, kb_drive_brake, "S,Down", "Driving: brake / reverse");

  REX_SETTING(std::string, kb_drive_camera, "V", "Driving: change camera, one step along the game's cycle (the pad's RB)");

  REX_SETTING(std::string, kb_drive_camera_keys, "1,2,3,4,5", "Driving: keys that jump straight to a camera, slot 1..5 (empty = off). The game only "
    "has a button that steps through its cameras; the jump presses it for you, as many "
    "times as the cycle needs, and stops on the one asked for.");

  REX_SETTING(std::string, kb_drive_camera_views, "4,0,3,2,1", "Driving: which camera each slot above selects, as the game numbers them: 0 bumper, "
    "1 cockpit, 2/3/4 the chase views. Default: slot 1 action(4), 2 bumper(0), 3 classic(3), "
    "4 static(2), 5 cockpit(1). Swap numbers here if 3 and 4 turn out the other way round.");

  REX_SETTING(std::string, kb_drive_gas, "W,Up", "Driving: accelerate");

  REX_SETTING(std::string, kb_drive_handbrake, "Space", "Driving: handbrake (the pad's A)");

  REX_SETTING(std::string, kb_drive_headlights, "H", "Driving: headlights (the pad's Y)");

  REX_SETTING(std::string, kb_drive_horn, "G", "Driving: horn");

  REX_SETTING(std::string, kb_drive_lookback, "R,RMB", "Driving: look back. RMB is the right mouse button.");

  REX_SETTING(std::string, kb_drive_map, "M", "Driving: map");

  // "Shift,X,LMB": X stays on nitro as well as Shift (a pad habit). It used to be needed so that the prompt for the pad's X, painted with the menu's X key, did what the pad's X
  // does on the road (KBM-DESIGN 1.3 item 8); the prompts now show the driving key on the road (prompt_glyphs.h, Context), so the first key with a cap, Shift, is what shows there.
  REX_SETTING(std::string, kb_drive_nitro, "Shift,X,LMB", "Driving: nitro (the pad's X). On the road the X prompt shows the first of these keys that has a cap. LMB is the left mouse button.");

  REX_SETTING(std::string, kb_drive_pause, "", "Driving: the PAUSE action of the control map. Empty by default: the pause menu is opened "
    "by Start in the UI mapper (kb_ui_start), which is where the pad's Start goes as well.");

  REX_SETTING(std::string, kb_drive_popup_back, "", "Driving: answer a pop-up with B (empty = pad only)");

  REX_SETTING(std::string, kb_drive_popup_select, "", "Driving: answer a pop-up with A (empty = pad only; Enter would also be Pause)");

  REX_SETTING(std::string, kb_drive_shift_down, "Q", "Driving: gear down (manual transmission)");

  REX_SETTING(std::string, kb_drive_shift_up, "E", "Driving: gear up (manual transmission)");

  // ── Driving (the player's control-map mapper).
  REX_SETTING(std::string, kb_drive_steer_left, "A,Left", "Driving: steer left (pairs with the right key at the same position)");

  REX_SETTING(std::string, kb_drive_steer_right, "D,Right", "Driving: steer right");

  REX_SETTING(std::string, kb_drive_talk, "T", "Driving: talk");

  // The defaults of the road (KBM-DESIGN.md 2.7, phase P5): WASD and the arrows steer, accelerate and brake; Space is the handbrake; Ctrl is the weight transfer (Caps Lock was a
  // lock key that toggles the host's state); Shift, X or the left mouse button is nitro; the right mouse button looks back (the mouse itself looks around: mouse_look.h). None of
  // these is also a menu key that matters on the road: the menu's keys (kb_ui_*) live in the UI mapper, which the road's mapper never sees, and Start (Enter, Esc) is live only while
  // no menu is open. Q and E are the gears here and the tabs (LB, RB) in a menu; Tab is the map's and the GPS's Back button, never a driving key.
  REX_SETTING(std::string, kb_drive_weight, "Control", "Driving: weight transfer, the 'turn in' of the pad's B (either Ctrl key)");

  REX_SETTING(bool, kb_steer_log, false, "Log the steering ramp: one line per input poll while a steering key is held or the wheel is coming back (at most 3000 lines).", rex::kDebugOnly);

  REX_SETTING(bool, kb_steer_ramp, true, "Debug: the steering ramp above. Off = the old key pair, full lock at once and centre at once (it rebuilds the driving mapper).", rex::kDebugOnly);

  REX_SETTING(f64, kb_steer_return, 0.25, "Keyboard steering: seconds from full lock back to centre when the steering key is let go, and the time the wheel takes to come through the centre when you steer the other "
    "way. Shorter = the car straightens up quicker.", rex::Range{0.05, 2.0});

  // Keyboard steering is not a 0 / 100 % switch: full lock builds up while a key is held and returns to centre when it is let go (mouse_look_core.h StepSteer). The ramp
  // rides the left stick's X axis of the state the game reads (KeySteerScope, XamInputGetState wrapper), so the game steers exactly as it does for a stick pushed that far.
  REX_SETTING(f64, kb_steer_rise, 0.40, "Keyboard steering: seconds from centre to full lock while a steering key is held (a tap is a small correction). Shorter = twitchier, 0.05 is nearly the old on / off steering.", rex::Range{0.05, 2.0});

};

inline InputKeyboardDrivingSettings g_input_keyboard_driving;

inline auto& kb_drive_brake = g_input_keyboard_driving.kb_drive_brake;
inline auto& kb_drive_camera = g_input_keyboard_driving.kb_drive_camera;
inline auto& kb_drive_camera_keys = g_input_keyboard_driving.kb_drive_camera_keys;
inline auto& kb_drive_camera_views = g_input_keyboard_driving.kb_drive_camera_views;
inline auto& kb_drive_gas = g_input_keyboard_driving.kb_drive_gas;
inline auto& kb_drive_handbrake = g_input_keyboard_driving.kb_drive_handbrake;
inline auto& kb_drive_headlights = g_input_keyboard_driving.kb_drive_headlights;
inline auto& kb_drive_horn = g_input_keyboard_driving.kb_drive_horn;
inline auto& kb_drive_lookback = g_input_keyboard_driving.kb_drive_lookback;
inline auto& kb_drive_map = g_input_keyboard_driving.kb_drive_map;
inline auto& kb_drive_nitro = g_input_keyboard_driving.kb_drive_nitro;
inline auto& kb_drive_pause = g_input_keyboard_driving.kb_drive_pause;
inline auto& kb_drive_popup_back = g_input_keyboard_driving.kb_drive_popup_back;
inline auto& kb_drive_popup_select = g_input_keyboard_driving.kb_drive_popup_select;
inline auto& kb_drive_shift_down = g_input_keyboard_driving.kb_drive_shift_down;
inline auto& kb_drive_shift_up = g_input_keyboard_driving.kb_drive_shift_up;
inline auto& kb_drive_steer_left = g_input_keyboard_driving.kb_drive_steer_left;
inline auto& kb_drive_steer_right = g_input_keyboard_driving.kb_drive_steer_right;
inline auto& kb_drive_talk = g_input_keyboard_driving.kb_drive_talk;
inline auto& kb_drive_weight = g_input_keyboard_driving.kb_drive_weight;
inline auto& kb_steer_log = g_input_keyboard_driving.kb_steer_log;
inline auto& kb_steer_ramp = g_input_keyboard_driving.kb_steer_ramp;
inline auto& kb_steer_return = g_input_keyboard_driving.kb_steer_return;
inline auto& kb_steer_rise = g_input_keyboard_driving.kb_steer_rise;

}  // namespace larecomp::settings
