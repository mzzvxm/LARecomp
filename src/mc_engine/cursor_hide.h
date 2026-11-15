#pragma once
//
// Mouse pointer auto-hide during play.
//
// The pointer goes at once when the active device is a controller (input_state.h:
// the one that produced the last real input) and after about 3 seconds without
// mouse movement while the keyboard and mouse are, and comes back when the mouse
// moves on purpose, a click or the wheel switches the device back, or when the
// window loses focus. It is blanked over the game window only (SDL does not
// touch the title bar or other windows), and it stays while the mouse is over
// an ImGui overlay such as the F4 panel.
//
// The SDK has a CursorVisibility::kAutoHidden mode, but it is not enough here:
// it never hides on a pad, a click or the wheel does not bring the pointer
// back, its timer ignores focus and overlays, and map_mouse.cpp and
// camera_look.cpp put the window back to kVisible when they let go, which would
// drop the mode for good. So this drives kHidden and kVisible itself and only
// ever undoes its own hide.
//
// Option: hide_mouse_pointer (on by default).

// Per-frame, driven from Patch_DeltaTimePre() in hooks.cpp. Attaches the mouse
// listener on the first frame the window exists.
void TickCursorHide();
