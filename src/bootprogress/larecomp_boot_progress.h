#pragma once

#include <functional>

#include <rex/rex_app.h>

namespace larecomp {

// Runs `work` -- the host-side content build: mod archive and custom music --
// on a worker thread, with a small progress popup in the bottom-right corner of
// the game window, and returns once the work is done.
//
// The build used to run straight on the UI thread, which meant no paint and no
// frame until it finished: a folder of a hundred fresh MP3s left a black,
// not-responding window for a minute with nothing to say for itself. The popup
// only appears if the build is still going after a moment, so a boot with
// nothing to do still looks the way it always did.
void RunBootBuildWithOverlay(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                             rex::ui::ImGuiDrawer* drawer, std::function<void()> work);

}  // namespace larecomp
