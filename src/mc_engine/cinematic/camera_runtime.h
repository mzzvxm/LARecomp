#pragma once
//
// The scene camera in the game (trailer toolkit, work package M1b): the glue between the camera rig (camera.h) and the game's final
// camera slots. The scene runner (scene_runtime.h) hands the rig the scene time every update through a TrackApplier registered
// for the `cam` and `cut` tracks; Hook_MenuCameraFinal (menu_camera.cpp, 0x822C0868, the last point before the renderer, in every
// camera mode) calls CinematicCameraApply, which evaluates the rig against the cars as they are AFTER this update's physics and
// writes the camera into the slots. Nothing changes unless a scene with a camera track is running.
//

#include <cstdint>

namespace mc::cinematic {

// Called first thing by Hook_MenuCameraFinal. `base` is the guest memory, `sp` the guest stack pointer of sub_822C0320, `mode` the
// camera mode (r24). Returns true when the scene's camera replaced the game's (the caller returns without running the menu
// camera's code on top of it). One flag test when no camera track runs.
bool CinematicCameraApply(uint8_t* base, uint32_t sp, uint32_t mode);

// True while the scene's camera is the final camera.
bool CinematicCameraActive();

// Where a scene's `car:<id>` comes from, for every id but `player` (the player's own car is built in): the car package (M4)
// installs one. Fills `matrix` with the car's world matrix as the game stores it, row by row: right, up, back, position (16
// floats), and returns false when the id is not a car that exists. Called on the game thread inside the camera hook.
using CarResolver = bool (*)(const char* id, float matrix[16]);
void SetCarResolver(CarResolver resolver);

}  // namespace mc::cinematic
