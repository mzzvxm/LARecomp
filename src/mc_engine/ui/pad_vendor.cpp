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
#else
#include <SDL3/SDL.h>

#include "gamepad.h"
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
#else
  // Elsewhere SDL knows each pad's USB ids and type. It is the SDK's own SDL (input_backend = sdl),
  // which keeps its device list current, so this only reads the list.
  // CHECK: not run on Linux (a DualShock 4 and a DualSense, over USB and Bluetooth).
  EnsurePadLayer();
  bool found = false;
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
    for (int i = 0; i < count && !found; ++i) {
      if (SDL_IsJoystickVirtual(ids[i])) {
        continue;
      }
      const SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[i]);
      found = SDL_GetJoystickVendorForID(ids[i]) == 0x054C || type == SDL_GAMEPAD_TYPE_PS3 ||
              type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    }
    SDL_free(ids);
  }
  return found;
#endif
  return false;
}

}  // namespace mcla::ui
