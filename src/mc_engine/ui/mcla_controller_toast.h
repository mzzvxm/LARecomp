#pragma once

#include <rex/ui/imgui_drawer.h>

namespace mcla::ui {

// Replaces the SDK's controller connect/disconnect toast with an MCLA panel.
// The SDK's toast only draws what its input driver queues, and that queue is
// closed to the app, so LARecomp turns it off (controller_toast_enable) and
// watches SDL's gamepad list itself. The corner still follows the SDK's
// controller_toast_position setting; mcla_controller_toast turns it off.
void StartControllerToasts(rex::ui::ImGuiDrawer* drawer);

} // namespace mcla::ui
