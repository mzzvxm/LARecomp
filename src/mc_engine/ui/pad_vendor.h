#pragma once

namespace mcla::ui {

// Whether a Sony controller (USB vendor 054C, over USB or Bluetooth) is attached, from the Windows
// raw input device list: no SDL, so it reads the same whichever SDL owns the pads. False elsewhere.
// It enumerates every HID device, so call it at most every second or two.
bool PlayStationControllerPresent();

}  // namespace mcla::ui
