#pragma once

#include <rex/ui/imgui_drawer.h>

namespace mcla::ui {

// Replaces the SDK's controller connect/disconnect toast with an MCLA panel.
// The SDK's toast only draws what its input driver queues, and that queue is
// closed to the app, so LARecomp turns it off (controller_toast_enable) and
// shows its own. It takes the controllers from the SDK's device list through
// the input state (mc_engine/input/input_state.h Controllers() and
// AddControllerListener()): the list is read when the SDK's device counter
// moves and the toast is told then, so nothing is polled and a quiet session
// costs one atomic exchange per drawn frame. The corner still follows the
// SDK's controller_toast_position setting; mcla_controller_toast turns it off.
void StartControllerToasts(rex::ui::ImGuiDrawer* drawer);

} // namespace mcla::ui
