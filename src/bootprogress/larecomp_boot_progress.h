#pragma once

#include <functional>
#include <string>

#include <rex/rex_app.h>

namespace larecomp {

// Starts `work` -- the host-side content build: mod archive and custom music --
// on a worker thread and returns at once. A small progress popup comes up in the
// bottom-right corner of the game window if the build is still going after a
// moment, so a boot with nothing to do still looks the way it always did. When
// the work is done, `then` runs on the UI thread.
//
// The build used to run straight on the UI thread, which meant no paint and no
// frame until it finished: a folder of a hundred fresh MP3s left a black,
// not-responding window for a minute. It then ran behind a nested message loop,
// which the SDK's SDL backend does not support (the stock SDK has no way to pump
// it at all). Now nothing waits: the popup keeps the window painting and the
// app's own loop runs, and the caller continues from `then` -- rex::ReXApp's
// LaunchModule, deferred, is what it is meant for.
//
// With no drawer the build runs synchronously, then `then`, as before.
void StartBootBuildWithOverlay(rex::ui::WindowedAppContext& app_context,
                               rex::ui::ImGuiDrawer* drawer, std::function<void()> work,
                               std::function<void()> then);

// Blocks until a build started above has finished. For shutdown: closing the
// window mid-build must not destroy the app under the worker. Call
// RequestBootBuildCancel first, so a shader build that is running stops at once
// instead of being waited for.
void WaitForBootBuild();

// The shader packs are built from the player's game files behind this popup (the
// first start takes from about fifteen seconds to a few minutes), and that is the
// one part of the boot work the player can stop.

// From the worker, when it starts and ends the part that can be stopped: the line
// the popup shows under its title, and whether it offers CANCEL. An empty
// subtitle and false give back the default ("Loading mods and music...").
void SetBootBuildHeading(std::string subtitle, bool cancellable);

// True once the player pressed CANCEL in the popup, or the app is closing. The
// work polls it (about every 25 ms) and stops. Reset when a build starts.
bool BootBuildCancelRequested();

// True once the app is closing (RequestBootBuildCancel): work that was cancelled
// for that reason has nobody to ask what to do next.
bool BootBuildClosing();

// For shutdown (the window's close, OnShutdown): cancels what can be cancelled
// and marks the app as closing. Any thread.
void RequestBootBuildCancel();

}  // namespace larecomp
