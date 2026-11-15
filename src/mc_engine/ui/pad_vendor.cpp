// Which controller family is attached, for button_prompts = auto (hooks/ui.cpp).
#include "pad_vendor.h"

#include "../device_watch.h"

namespace mcla::ui {

// Used to walk the raw input device list every 2 s on the game's thread (Windows) or SDL's list
// (elsewhere). Both are the device watcher's job now: it hears about devices from the OS instead of
// asking, and this only reads what it keeps.
bool PlayStationControllerPresent() {
  return larecomp::devwatch::SonyPadPresent();
}

}  // namespace mcla::ui
