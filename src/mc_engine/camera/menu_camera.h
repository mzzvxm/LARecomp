#pragma once

#include <cstdint>
//
// Front-end (boot / initial menus) camera control. See menu_camera.cpp for the
// RE notes.
//

// Sampled from the free-fly debug camera (Patch_DebugCam) so the menu_cam_dump
// command can emit the spot the user is currently flying at. Pure host state,
// no guest access.
void MenuCam_NoteFreecam(float x, float y, float z, float yaw, float pitch);

// The dcam manager path can supply a photo or free-fly view after the gameplay rig was selected.
void MenuCam_NoteManagerView();

// The last camera handed to the renderer, or -1 before publication or when another camera supplants the rig.
int32_t CurrentRenderedCameraRig();
