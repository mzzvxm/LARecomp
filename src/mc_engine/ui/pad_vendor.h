#pragma once

namespace mcla::ui {

// Whether a Sony controller (USB vendor 054C, over USB or Bluetooth) is attached, from the Windows
// raw input device list: no SDL, so it reads the same whichever SDL owns the pads. On Linux from
// SDL's list of pads (vendor 054C, or a PlayStation gamepad type). It enumerates every HID device
// on Windows, so call it at most every second or two.
bool PlayStationControllerPresent();

}  // namespace mcla::ui
