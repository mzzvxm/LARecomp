#pragma once
//
// Pause-menu rows that the launcher already had (SETTINGS-REVIEW.md, package SX-G7): the value steps and the
// text of the two kinds of row that need more than a checkbox. The rows themselves are in pause_menu.cpp, next
// to their neighbours; this header only holds what has no game code in it, so a standalone program can check it
// (pause_parity_rows_test.cpp).
//
//   CAMERA SHAKE (CHASE CAMERA page)            camera_shake_scale   OFF, 0.3X, 0.6X, 1.0X, 1.5X, 2.0X, 3.0X
//   DUALSENSE THROTTLE / BRAKE STRENGTH         trigger_throttle,    1 (LIGHT), 2 ... 7, 8 (FIRM)
//   (CONTROLLER page)                           trigger_brake
//
// The steps are the launcher's own lists (SettingsSchema.cs: camera_shake_scale, trigger_throttle and
// trigger_brake), so a value that one surface writes is a step of the other. Both cvars are read at every use
// (hooks/camera.cpp, adaptive_triggers.cpp), so a step takes hold at once.
//
// Width (pause_menu.cpp, "Page width"): the CONTROLLER page's widest label is now "DUALSENSE THROTTLE STRENGTH"
// (27) and its widest value "PLAYSTATION" (11): 38 of the 41 a page can take. CHASE CAMERA: "CAM SMOOTH FACTOR" 17
// with "OFF" 3 or "1.5X" 4.

#include <cstdio>
#include <string>

namespace mcla_menu {
namespace parity {

// camera_shake_scale is a double in 0 to 3 (default 0.6); 0 removes the shake. The row's text is "%.1fX" with
// "OFF" for 0, like the other multiplier rows.
constexpr double kCameraShakeSteps[] = {0.0, 0.3, 0.6, 1.0, 1.5, 2.0, 3.0};
constexpr int kCameraShakeStepCount = int(sizeof(kCameraShakeSteps) / sizeof(kCameraShakeSteps[0]));
constexpr const char* kCameraShakeFormat = "%.1fX";
constexpr const char* kCameraShakeZeroLabel = "OFF";

// trigger_throttle (R2, default 3) and trigger_brake (L2, default 6) are integers in 1 to 8.
constexpr double kTriggerStrengthSteps[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
constexpr int kTriggerStrengthStepCount =
    int(sizeof(kTriggerStrengthSteps) / sizeof(kTriggerStrengthSteps[0]));

// The text of a strength: the ends say what they mean, the rest are the number ("1 (LIGHT)" ... "8 (FIRM)").
// A value that is not a whole number (a hand-written larecomp.toml cannot hold one: the cvar is an integer) is
// shown rounded.
inline std::string TriggerStrengthLabel(double value) {
    const int step = int(value < 0.0 ? value - 0.5 : value + 0.5);
    if (step == 1) return "1 (LIGHT)";
    if (step == 8) return "8 (FIRM)";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", step);
    return buf;
}

}  // namespace parity
}  // namespace mcla_menu
