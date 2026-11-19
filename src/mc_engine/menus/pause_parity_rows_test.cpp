// Check of the value steps and texts of the pause-menu rows that bring the launcher's rows into the game
// (pause_parity_rows.h, SETTINGS-REVIEW.md package SX-G7). A program of its own: it needs no game, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_pause_parity_test     (CMakeLists.txt)
//   <build dir>/larecomp_pause_parity_test
//
// or, with no build directory:
//   clang++ -std=c++20 -I src/mc_engine src/mc_engine/menus/pause_parity_rows_test.cpp -o pause_parity_rows_test
//
// The expected lists and texts are written out here again on purpose: the other side of each is the
// launcher's SettingsSchema.cs (Camera Shake, DualSense Throttle / Brake Strength), so a step changed in
// the header alone fails here, and changing one on purpose means changing it here and in the launcher.

#include <algorithm>
#include <cstdio>
#include <string>

#include "pause_parity_rows.h"

using namespace mcla_menu::parity;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// The text the pause menu shows for a camera shake step: what ItemValueLabel (pause_menu.cpp) makes of a
// row with this format and zero label.
std::string ShakeLabel(double value) {
  if (value == 0.0) return kCameraShakeZeroLabel;
  char buf[48];
  std::snprintf(buf, sizeof(buf), kCameraShakeFormat, value);
  return buf;
}

// The text the launcher shows for the same step (SettingsSchema.cs: v == 0 ? "Off" : X1(v), X1 = "0.0x"),
// upper-cased: the pause menu's menu font has capitals only.
std::string LauncherShakeLabel(double value) {
  if (value == 0.0) return "OFF";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.1fX", value);
  return buf;
}

void CameraShake() {
  const double expected[] = {0.0, 0.3, 0.6, 1.0, 1.5, 2.0, 3.0};
  Check(kCameraShakeStepCount == 7, "camera shake has 7 steps");
  for (int i = 0; i < 7 && i < kCameraShakeStepCount; ++i)
    Check(kCameraShakeSteps[i] == expected[i], "camera shake step " + std::to_string(i));
  // Ascending, so left and right walk it in order; inside the cvar's range (0 to 3), default included.
  bool ascending = true;
  bool inRange = true;
  bool hasDefault = false;
  for (int i = 0; i < kCameraShakeStepCount; ++i) {
    if (i > 0 && !(kCameraShakeSteps[i] > kCameraShakeSteps[i - 1])) ascending = false;
    if (kCameraShakeSteps[i] < 0.0 || kCameraShakeSteps[i] > 3.0) inRange = false;
    if (kCameraShakeSteps[i] == 0.6) hasDefault = true;
  }
  Check(ascending, "camera shake steps ascend");
  Check(inRange, "camera shake steps are inside the cvar's range 0 to 3");
  Check(hasDefault, "the cvar's default 0.6 is a step");
  // Texts.
  const char* texts[] = {"OFF", "0.3X", "0.6X", "1.0X", "1.5X", "2.0X", "3.0X"};
  for (int i = 0; i < 7 && i < kCameraShakeStepCount; ++i) {
    Check(ShakeLabel(kCameraShakeSteps[i]) == texts[i], std::string("camera shake text ") + texts[i]);
    Check(ShakeLabel(kCameraShakeSteps[i]) == LauncherShakeLabel(kCameraShakeSteps[i]),
          std::string("camera shake text equals the launcher's: ") + texts[i]);
  }
  // The value written for a step is the cvar's text: "%g" of the step, as CvarSetDouble does.
  const char* written[] = {"0", "0.3", "0.6", "1", "1.5", "2", "3"};
  for (int i = 0; i < 7 && i < kCameraShakeStepCount; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", kCameraShakeSteps[i]);
    Check(std::string(buf) == written[i], std::string("camera shake writes ") + written[i]);
  }
}

void TriggerStrength() {
  Check(kTriggerStrengthStepCount == 8, "trigger strength has 8 steps");
  for (int i = 0; i < 8 && i < kTriggerStrengthStepCount; ++i)
    Check(kTriggerStrengthSteps[i] == double(i + 1), "trigger strength step " + std::to_string(i));
  const char* texts[] = {"1 (LIGHT)", "2", "3", "4", "5", "6", "7", "8 (FIRM)"};
  for (int i = 0; i < 8 && i < kTriggerStrengthStepCount; ++i)
    Check(TriggerStrengthLabel(kTriggerStrengthSteps[i]) == texts[i],
          std::string("trigger strength text ") + texts[i]);
  // The defaults (trigger_throttle 3, trigger_brake 6) are steps with plain numbers.
  Check(TriggerStrengthLabel(3.0) == "3", "throttle default reads 3");
  Check(TriggerStrengthLabel(6.0) == "6", "brake default reads 6");
  // The row never shows a value it is not on: no two steps share a text (RowDynamics::value_label
  // treats equal texts as one entry).
  bool distinct = true;
  for (int i = 0; i < kTriggerStrengthStepCount; ++i)
    for (int j = i + 1; j < kTriggerStrengthStepCount; ++j)
      if (TriggerStrengthLabel(kTriggerStrengthSteps[i]) == TriggerStrengthLabel(kTriggerStrengthSteps[j]))
        distinct = false;
  Check(distinct, "every strength step has a text of its own");
  // A value from outside the list (the cvar is an integer, so only a number rounded off) is named as
  // its nearest whole number, and the ends keep their words.
  Check(TriggerStrengthLabel(0.9999) == "1 (LIGHT)", "0.9999 reads as 1 (LIGHT)");
  Check(TriggerStrengthLabel(7.6) == "8 (FIRM)", "7.6 reads as 8 (FIRM)");
  Check(TriggerStrengthLabel(4.2) == "4", "4.2 reads as 4");
}

// The pages' widths ("Page width" in pause_menu.cpp): the pause movie sizes a page to its widest label
// (suffix included) plus its widest value, and a page that goes past 41 characters of that pair has to be
// measured on screen before it ships. The labels below are the ones in pause_menu.cpp's tables.
void PageWidths() {
  constexpr int kPageLimit = 41;
  // CONTROLLER, after the two strength rows: the labels and the longest value on the page.
  const char* controller[] = {"VIBRATION",
                              "DUALSENSE RESISTANCE",
                              "DUALSENSE VIBRATION",
                              "DUALSENSE THROTTLE STRENGTH",
                              "DUALSENSE BRAKE STRENGTH",
                              "XBOX TRIGGER RUMBLE",
                              "XBOX TRIGGER STRENGTH",
                              "LEFT STICK DEAD ZONE",
                              "RIGHT STICK DEAD ZONE",
                              "BUTTON LAYOUT"};
  size_t widestLabel = 0;
  for (const char* label : controller) widestLabel = std::max(widestLabel, std::string(label).size());
  size_t widestValue = std::string("PLAYSTATION").size();  // BUTTON LAYOUT
  for (int i = 0; i < kTriggerStrengthStepCount; ++i)
    widestValue = std::max(widestValue, TriggerStrengthLabel(kTriggerStrengthSteps[i]).size());
  Check(widestLabel == 27, "CONTROLLER's widest label is DUALSENSE THROTTLE STRENGTH (27)");
  Check(widestLabel + widestValue <= size_t(kPageLimit), "CONTROLLER is within 41 characters");

  // RECOMP SETTINGS: the suffixes count.
  const char* recomp[] = {"HIDE MOUSE POINTER", "MOUSE IN MENUS", "ORIGINAL INTRO SPEED (NEXT START)",
                          "SKIP INTRO VIDEOS (NEXT START)"};
  size_t recompWidest = 0;
  for (const char* label : recomp) recompWidest = std::max(recompWidest, std::string(label).size());
  Check(recompWidest + 3 <= size_t(kPageLimit), "RECOMP SETTINGS is within 41 characters");
  Check(std::string(recomp[3]).size() < std::string(recomp[2]).size(),
        "SKIP INTRO VIDEOS is narrower than the row that already set the page's width");

  // CHASE CAMERA: two rows, values "OFF" and "3.0X" at most.
  size_t chase = std::max(std::string("CAM SMOOTH FACTOR").size(), std::string("CAMERA SHAKE").size());
  Check(chase + 4 <= size_t(kPageLimit), "CHASE CAMERA is within 41 characters");

  // REAR VIEW: the new row has no suffix (a grey reason would not fit).
  Check(std::string("CARS IN REAR VIEW").size() + std::string("MIDDLE FOOTER").size() <= size_t(kPageLimit),
        "REAR VIEW is within 41 characters");
  Check(std::string("CARS IN REAR VIEW (NO MIRROR)").size() + std::string("MIDDLE FOOTER").size() >
            size_t(kPageLimit),
        "a grey reason on CARS IN REAR VIEW would not fit (why the row is not greyed)");

  // MAP MOUSE: four checkboxes.
  const char* mapPage[] = {"MAP MOUSE", "MAP MOUSE ZOOM", "HIDE POINTER ON MAP", "MAP CURSOR MAGNET"};
  size_t mapWidest = 0;
  for (const char* label : mapPage) mapWidest = std::max(mapWidest, std::string(label).size());
  Check(mapWidest + 3 <= size_t(kPageLimit), "MAP MOUSE is within 41 characters");

  // The one-line texts under a page's title: at most 40 characters (the form takes 41).
  const char* lines[] = {"Camera, map, time of day and cars.", "Smoothing and shake of the chase camera.",
                         "The rear view mirror and the cars in it.", "Mouse control of the full map."};
  for (const char* line : lines)
    Check(std::string(line).size() <= 40, std::string("the page line fits: ") + line);
}

}  // namespace

int main() {
  CameraShake();
  TriggerStrength();
  PageWidths();
  if (g_failed == 0) {
    std::printf("pause_parity_rows_test: %d checks passed\n", g_checks);
    return 0;
  }
  std::printf("pause_parity_rows_test: %d of %d checks FAILED\n", g_failed, g_checks);
  return 1;
}
