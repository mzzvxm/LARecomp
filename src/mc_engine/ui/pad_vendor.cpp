// Which controller family is attached, for button_prompts = auto (hooks/ui.cpp).
#include "pad_vendor.h"

#include <cctype>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mcla::ui {

bool PlayStationControllerPresent() {
#if defined(_WIN32)
  UINT count = 0;
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
    return false;
  }
  std::vector<RAWINPUTDEVICELIST> devices(count);
  if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) {
    return false;
  }
  for (UINT i = 0; i < count; ++i) {
    if (devices[i].dwType != RIM_TYPEHID) {
      continue;
    }
    char name[512];
    UINT size = sizeof name;
    if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name, &size) == UINT(-1)) {
      continue;
    }
    std::string upper(name);
    for (char& c : upper) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    // USB: VID_054C. Bluetooth: VID&0002054C.
    if (upper.find("VID_054C") != std::string::npos ||
        upper.find("VID&0002054C") != std::string::npos) {
      return true;
    }
  }
#endif
  return false;
}

}  // namespace mcla::ui
