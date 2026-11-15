#pragma once

namespace mcla::ui {

// Whether a Sony controller (USB vendor 054C, over USB or Bluetooth) is attached, for
// button_prompts = auto (hooks/ui.cpp). The answer is the device watcher's (mc_engine/device_watch.h):
// on Windows the OS's own arrival and removal notifications for HID interfaces, with one list taken
// at start on a thread of its own; elsewhere SDL's joystick added/removed events. This is an atomic
// read, cheap enough for every frame and safe from any thread. False until the watcher's first list
// has landed (a few milliseconds after the app's setup).
bool PlayStationControllerPresent();

}  // namespace mcla::ui
