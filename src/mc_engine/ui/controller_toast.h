#pragma once

#include <rex/ui/ui_system.h>

namespace mcla::ui {

// The controller connect/disconnect toast, as an MCLA panel. The 0.11 SDK has no controller toast of its own
// any more (0.10's drew what its input driver queued, which was closed to the app), so this is the only one. It
// takes the controllers from the SDK's device list through the input state (mc_engine/input_state.h Controllers()
// and AddControllerListener(); rex::hid::IInputDeviceList underneath): the list is read when the SDK's device
// generation moves and the toast is told then, so nothing is polled and a quiet session costs one atomic
// exchange per drawn frame. mcla_controller_toast turns it off; the corner is controller_toast_position, which
// the SDK no longer defines (CHECK: LARecomp's own setting now, or the top right corner).
// UI thread; the UI system owns the dialog.
void StartControllerToasts(rex::ui::IUISystem& ui);

} // namespace mcla::ui
