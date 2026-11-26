#pragma once
// ===========================================================================
// The final camera: the pose the game hands the renderer, for code that is not on the camera's thread
// ===========================================================================
// menu_camera.cpp's Hook_MenuCameraFinal sits where every one of the camera system's branches has produced its result (sub_822C0320's
// loc_822C0868) and publishes what is in the stack slots on its way out, after the trailer toolkit's cameras and the front-end shots of
// menu_camera.cpp have written theirs. Another thread reads the latest pose here (hooks/title_road_plane.cpp: the ground the title screen
// is looking at). Rows: right, up, BACK (the forward direction is minus the third), position = the fourth row, Y up; the field of view is
// the slot's (degrees).
// ===========================================================================

#include <cstdint>

namespace mc::final_camera {

struct Pose {
  float pos[3] = {0, 0, 0};
  float right[3] = {1, 0, 0};
  float up[3] = {0, 1, 0};
  float back[3] = {0, 0, 1};
  float fov_degrees = 0.0f;
  uint32_t mode = 0;  // the camera system's mode id (6 = a UI / menu camera is driving)
};

// The camera hook's thread, once a frame.
void Publish(const Pose& pose);

// Any thread: the latest pose and its age in seconds. False before the first one.
bool Latest(Pose* pose, double* age_seconds);

}  // namespace mc::final_camera
