#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class WheelSettings final : public rex::SettingOwner {
 public:
  WheelSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, wheel_auto_setup, true, "The first time a wheel is found that has no section in wheel_profiles.ini, ask whether to run the setup "
    "now (a yes/no box, shown once the game window is up). Answering no writes a marker so it is not asked "
    "again; the setup can always be started with wheel_learn. Off: never ask.", rex::kDebugOnly);

  REX_SETTING(f64, wheel_cancel_game_deadzone, 0.05, "The game puts a dead zone of about 5% on its stick and pedal axes (measured: steering comes out as "
    "(x - 0.05) / 0.95), written for a thumb stick. A wheel is precise, so the first few degrees would do nothing. "
    "This lifts the wheel's output by that amount, so the game's own dead zone cancels and the response is "
    "linear. 0 turns it off. Set it to what you measure if a wheel still feels dead around centre.", rex::kDebugOnly);

  REX_SETTING(std::string, wheel_device, "auto", "Which joystick is the wheel. auto: the first one SDL calls a wheel or a known wheel model. off: "
    "none. Anything else is matched against the device name (part of it, any case) or written as "
    "vendor:product in hex (046d:c24f) and opens that device even if SDL does not call it a wheel. "
    "Takes effect when the device is next scanned (about a second).", rex::kDebugOnly);

  REX_SETTING(i32, wheel_device_range, 900, "The lock-to-lock rotation the wheel is set to in its own driver, in degrees. The wheel reports "
    "-1..1 over that range; this and wheel_game_range turn it into the game's range.");

  REX_SETTING(std::string, wheel_fake, "off", "Present the game with a steering wheel without owning one. off: nothing changes. pad: the "
    "real pad or keyboard-and-mouse state is shown as a wheel (stick X steers, triggers are the "
    "pedals, bumpers shift). keys: A/D steer, W throttle, S brake, Q/E shift down/up. Takes "
    "effect on the next input poll.", rex::Allowed("off", "pad", "keys"), rex::kDebugOnly);

  REX_SETTING(bool, wheel_ffb, true, "Force feedback on the steering wheel. Off: the wheel is never given a force.");

  REX_SETTING(bool, wheel_ffb_autocenter, true, "Only used when wheel_ffb_centering is 0: leave the wheel driver's own centring spring on. With centring "
    "above 0 the driver's spring is switched off (two springs would fight). Takes effect when the wheel is "
    "opened.", rex::kDebugOnly);

  REX_SETTING(i32, wheel_ffb_boost, 200, "Multiplies the forces the GAME asks for (constant, vibration, its spring and damper), 100-500 %, before "
    "wheel_ffb_strength. The game's forces are mostly 10-40% of its scale, written for a light 360 wheel, so on a "
    "strong base they feel weak: 200 doubles them, with the result capped at the wheel's full force. Centring "
    "added by this program (wheel_ffb_centering) is not multiplied.");

  REX_SETTING(i32, wheel_ffb_centering, 40, "Centring added by this program while the game is not running its own spring (it does not while "
    "free-roaming), 0-100: the most force the centring spring can reach, before wheel_ffb_strength. It is "
    "about a third of that when parked and grows with the car's speed, like the self-aligning force of a real "
    "car, so the wheel returns to centre when let go. It is a native DirectInput spring (the wheel's firmware "
    "does the work) when the wheel allows one. 0 leaves the wheel slack between the game's effects.");

  REX_SETTING(bool, wheel_ffb_log, false, "Log every force-feedback command the game sends while wheel_fake is on: effect id, type, "
    "duration and parameters. Capped so a long race cannot fill the log.", rex::kDebugOnly);

  REX_SETTING(bool, wheel_ffb_panic, false, "Emergency stop for the wheel's force feedback: switching it on stops every force and "
    "disarms until wheel_ffb_rearm is switched on.");

  REX_SETTING(bool, wheel_ffb_rearm, false, "Switch on to arm force feedback again after an emergency stop (it re-checks the wheel first). "
    "Switch it off again before the next time.");

  REX_SETTING(bool, wheel_ffb_selftest, false, "When force feedback arms, play a staged test and log it: 0.5 s push one way, 0.5 s the other, 2 s of "
    "centring spring (turn the wheel a little and let go), 1 s of damper. Every device call's result and the "
    "wheel's position (every 50 ms) go to the log, so a dead force path shows in the log whatever you feel. "
    "Needs the wheel to be free.", rex::kDebugOnly);

  REX_SETTING(std::string, wheel_ffb_sign, "auto", "Which way a positive constant force turns the wheel (centring does not use it). auto: measured once per "
    "device with a gentle push (keep your hands off the wheel); normal / inverted: skip the test. If the game's "
    "constant forces push into the turn instead of against it, set the other one.", rex::Allowed("auto", "normal", "inverted"));

  REX_SETTING(i32, wheel_ffb_strength, 70, "Force feedback strength, 0-100: the share of the wheel's full force that the game's strongest effect "
    "reaches. The game's own forces are mostly 10-40% of its scale (it was written for a light 360 wheel), so a "
    "direct-drive wheel needs this high to feel them; a belt or gear wheel may want it lower. Check "
    "wheel_ffb_trace for the peak force the game actually asks for.");

  REX_SETTING(bool, wheel_ffb_trace, false, "Log the force sent to the wheel about four times a second, every device call's result, and every state "
    "change.", rex::kDebugOnly);

  REX_SETTING(i32, wheel_ffb_watchdog_ms, 500, "Force goes to zero when the game has not run an input frame for this long (a loading screen, "
    "a hang) and comes back by itself when it does.", rex::kDebugOnly);

  REX_SETTING(i32, wheel_game_range, 540, "The lock-to-lock rotation that reaches full steering lock in the game, in degrees. The game "
    "reads steering as 8 bits (255 steps), so a smaller range steers finer: 270 deg = 1.06 deg per "
    "step, 540 = 2.1, 900 = 3.5. Full lock is reached before the wheel's own end stop when this is "
    "smaller than wheel_device_range.");

  REX_SETTING(bool, wheel_learn, false, "Run the wheel setup: a console window walks through the steering, the pedals, the paddles and "
    "the H-shifter, watches which axes and buttons move, and writes the wheel's section of "
    "wheel_profiles.ini. Switches itself off when it is done. Start the game with --wheel_learn=true "
    "before playing.", rex::kDebugOnly);

  REX_SETTING(std::string, wheel_mode, "auto", "Who drives when a wheel is connected. auto: the wheel until the gamepad is moved, the gamepad "
    "until the wheel is. wheel: always the wheel. gamepad: the wheel is ignored and the game sees "
    "the pad. Only the driving controls switch; Start, Back, A, B and the D-pad work from both.", rex::Allowed("auto", "wheel", "gamepad"));

  REX_SETTING(bool, wheel_pedal_autorange, true, "Pedals that never reach the end of their travel read full once they have been pressed to where "
    "they do reach (after the first firm press). Off: the pedals use their full nominal range. A "
    "pedal with throttle_full / brake_full in the profile is never auto-ranged.", rex::kDebugOnly);

  REX_SETTING(f64, wheel_steer_deadzone, 0.01, "Fraction of the wheel's range around centre that reads as straight ahead (0.01 = 1%).");

  REX_SETTING(f64, wheel_steer_gamma, 1.0, "Shapes steering after the range scaling: 1 is linear, above 1 softens the centre. The game "
    "applies its own curve afterwards, so leave this at 1 unless the car feels twitchy.", rex::kDebugOnly);

  REX_SETTING(bool, wheel_trace, false, "Log, once a second, every axis and pressed button SDL reads from the wheel and what the game is given "
    "(who drives, steering, pedals, buttons). For finding out why a wheel behaves oddly: start the game with it, "
    "reproduce the problem, send the log.", rex::kDebugOnly);

  REX_SETTING(std::string, wheel_virtual, "off", "Test aid: create a virtual SDL wheel (type wheel, 4 axes, 20 buttons, a hat) driven from the "
    "keyboard (A/D steer, W throttle, S brake, Q/E paddles) so the whole SDL path can be run without "
    "hardware. off, low (pedals rest at -32768) or high (pedals rest at +32767).", rex::Allowed("off", "low", "high"), rex::kDebugOnly);

};

inline WheelSettings& g_wheel = larecomp::Leak<WheelSettings>();

inline auto& wheel_auto_setup = g_wheel.wheel_auto_setup;
inline auto& wheel_cancel_game_deadzone = g_wheel.wheel_cancel_game_deadzone;
inline auto& wheel_device = g_wheel.wheel_device;
inline auto& wheel_device_range = g_wheel.wheel_device_range;
inline auto& wheel_fake = g_wheel.wheel_fake;
inline auto& wheel_ffb = g_wheel.wheel_ffb;
inline auto& wheel_ffb_autocenter = g_wheel.wheel_ffb_autocenter;
inline auto& wheel_ffb_boost = g_wheel.wheel_ffb_boost;
inline auto& wheel_ffb_centering = g_wheel.wheel_ffb_centering;
inline auto& wheel_ffb_log = g_wheel.wheel_ffb_log;
inline auto& wheel_ffb_panic = g_wheel.wheel_ffb_panic;
inline auto& wheel_ffb_rearm = g_wheel.wheel_ffb_rearm;
inline auto& wheel_ffb_selftest = g_wheel.wheel_ffb_selftest;
inline auto& wheel_ffb_sign = g_wheel.wheel_ffb_sign;
inline auto& wheel_ffb_strength = g_wheel.wheel_ffb_strength;
inline auto& wheel_ffb_trace = g_wheel.wheel_ffb_trace;
inline auto& wheel_ffb_watchdog_ms = g_wheel.wheel_ffb_watchdog_ms;
inline auto& wheel_game_range = g_wheel.wheel_game_range;
inline auto& wheel_learn = g_wheel.wheel_learn;
inline auto& wheel_mode = g_wheel.wheel_mode;
inline auto& wheel_pedal_autorange = g_wheel.wheel_pedal_autorange;
inline auto& wheel_steer_deadzone = g_wheel.wheel_steer_deadzone;
inline auto& wheel_steer_gamma = g_wheel.wheel_steer_gamma;
inline auto& wheel_trace = g_wheel.wheel_trace;
inline auto& wheel_virtual = g_wheel.wheel_virtual;

}  // namespace larecomp::settings
