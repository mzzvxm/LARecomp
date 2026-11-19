#pragma once
//
// Mouse look, the runtime half (KBM-DESIGN.md phase P5, INPUT-EVENTS-AUDIT.md item 7). The pure rules are in mouse_look_core.h (the gate, the delta sum, the look
// angles, the steering ramp) and are unit-tested; this file is the window side: relative mouse mode, the listener that sums the raw motion, and the facts the gate
// is given.
//
// What replaced what: the look used to hide the pointer, recentre it with SetCursorPos every frame and read the pixel distance from the centre (Windows only, accelerated
// by the pointer settings, with fake motion events that looked like mouse activity to the device state machine, counts lost between the two calls). Now the window is
// put in RELATIVE MOUSE MODE (SDL: the pointer is hidden and locked, motion arrives as raw counts, no recentring), a layer-0 window listener sums every motion event's
// dx / dy into two atomics on the UI thread, and the game thread takes them with exchange(0) once per camera tick. It is the same SDL call on Windows, Linux and the
// Steam Deck.
//
// Relative mode is on only while ALL of these hold (mouse_look_core.h Gate, Facts): mouse look is wanted (cam_freelook, or the debug free camera's mouse), the active
// device is the keyboard and mouse (input_state.h), the window has the focus, a car is on the road (the race HUD is updating and the game is not paused), and no menu
// screen, the Sidekick phone, the full map or an overlay (F4, a browser) is up. It is released at once when the device changes to a controller, on focus loss, when a menu
// or an overlay opens or the setting is turned off; a controller's right stick then works as before. The state machine sees only real motion (relative events carry no
// position change, and nothing is recentred).
//
// rexglue 0.11: the window is put in relative mode by IInputSystem::SetPointerMode(kLocked) (the pointer arbiter in sdk_access.h gives the input system the strongest wish of
// mouse look, the map and the cursor hide; the UI system applies it on the UI thread), and the raw counts are IInputSystem::TakeMotion() (it sums the motion only while the
// pointer is locked, applied and the window in front). No listener of ours is involved any more.
//
// Threads: Want / Tick / Take are called on the game thread (Tick once a frame from Patch_DeltaTimePre via TickCameraLook, Want from the camera code, Take once per camera
// tick); the pointer mode is applied by the UI system on the UI thread.
//

namespace larecomp::mouselook {

// Who asks for the raw deltas. Look = the gameplay camera's mouse look (camera_look.cpp). DebugCam = the debug free-fly camera (hooks/camera.cpp; debug_cam = free):
// it is not gated by the device or the road and it takes the deltas away from Look while it is on.
enum class Client { Look, DebugCam };

// The client wants raw mouse motion right now. A want lasts 250 ms: a client that stops calling (the setting off, the camera system not running) lets go by itself.
void Want(Client client);

// Once per game frame. Decides, from the wants and the facts, whether relative mouse mode should be on, and posts the change to the UI thread. Logs every change.
void Tick();

// The raw counts since the client's last Take, and whether relative mode is on for it. False (and zeros) when it is not, or when the other client owns the mouse.
bool Take(Client client, float& dx, float& dy);

// Relative mouse mode is on (any thread). The pointer is then SDL's to hide: cursor_hide.cpp stands down.
bool Engaged();

}  // namespace larecomp::mouselook
