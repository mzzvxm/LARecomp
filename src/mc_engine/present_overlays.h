#pragma once

// The ImGui overlays (console, F3, settings, achievements, toasts) and direct
// presents (frame_pacing_direct_present).
//
// While the overlays are on the presenter, it repaints once per monitor
// refresh on its own UI thread and shows a new game frame only on the next of
// those repaints. Off it, it presents each frame the moment the native runtime
// hands it over -- which is what lets frame_pacing put a frame on screen
// exactly when it was simulated for. So with direct presents the overlays are
// taken off whenever nothing of theirs is on screen, and put back the moment
// a key that opens one is pressed or a toast has something to show.

namespace rex::ui {
class Window;
}

namespace mc::present_overlays {

// UI thread, once the window exists: starts watching the keys that open an
// overlay. They are seen ahead of the app's own handler, which runs the binds.
void Install(rex::ui::Window* window);

// Main thread, once a frame: whether frame pacing wants direct presents.
void Update(bool direct_wanted);

// True while the overlays are off the presenter.
bool Detached();

}  // namespace mc::present_overlays
