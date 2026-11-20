#pragma once
//
// What the Director needs from the game and the game's hooks need from the Director: the few calls that cross between them. Everything else of the
// Director is pure (director.h) or inside its own files. Game thread unless said. Nothing here does anything while mcla_director is off: Enabled() is one atomic
// load, and every caller tests it first.
//

#include <cstdint>
#include <string>

namespace mc::cinematic::director {

// mcla_director is on and the listener's queue is open.
bool Enabled();

// Which code wrote the final camera in the hook (freecam_runtime.cpp's FreecamApply, the first thing Hook_MenuCameraFinal runs):
//   kFreecam      the flight wrote the slots
//   kGameForced   the Director forced the game's own camera: the slots are the game's
//   kNone         nobody yet: the slots hold the game's camera and the scene's camera (camera_runtime.cpp) may be written next
//   kScene        the scene's camera: camera_runtime.cpp wrote it into the slots just now (NoteSceneCameraWritten)
enum class CameraSource : uint8_t { kNone, kFreecam, kGameForced, kScene };

// Called by FreecamApply on every hook run while the Director is on, with the guest memory, the guest stack pointer of sub_822C0320 (the camera slots' frame)
// and the camera mode. The Director reads the camera from the slots, or, for the scene's camera, evaluates the scene's camera track itself the way the hook
// will (a shadow rig: the fallback for a hook run in which the scene's camera does not write; NoteSceneCameraWritten replaces it when it does).
void NoteCameraHook(uint8_t* base, uint32_t sp, uint32_t mode, CameraSource who);

// Called by CinematicCameraApply (camera_runtime.cpp) right after WriteCamera, while the Director is on: the slots hold the scene's camera, exactly the pose the
// frame (and a take) shows. The Director reads it from them, so scene.get / camera.get / query.visible see the real camera, lag, springs and all.
void NoteSceneCameraWritten(uint8_t* base, uint32_t sp, uint32_t mode);

}  // namespace mc::cinematic::director
