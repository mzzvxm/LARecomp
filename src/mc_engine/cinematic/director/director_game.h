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
enum class CameraSource : uint8_t { kNone, kFreecam, kGameForced };

// Called by FreecamApply on every hook run while the Director is on, with the guest memory, the guest stack pointer of sub_822C0320 (the camera slots' frame)
// and the camera mode. The Director reads the camera from the slots, or, for the scene's camera, evaluates the scene's camera track itself the way the hook
// will (a shadow rig). CHECK: an exact reading of the scene camera needs one call in camera_runtime.cpp after WriteCamera (see director_runtime.cpp).
void NoteCameraHook(uint8_t* base, uint32_t sp, uint32_t mode, CameraSource who);

}  // namespace mc::cinematic::director
