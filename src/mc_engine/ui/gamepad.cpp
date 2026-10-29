#include "gamepad.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#endif

namespace mcla::ui {

namespace {

// Buttons in ImGui's gamepad keys, with their XInput bit and SDL button.
struct ButtonKey {
  ImGuiKey key;
  uint16_t xinput;
  SDL_GamepadButton sdl;
};

constexpr ButtonKey kButtons[] = {
    {ImGuiKey_GamepadDpadUp, 0x0001, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {ImGuiKey_GamepadDpadDown, 0x0002, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {ImGuiKey_GamepadDpadLeft, 0x0004, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {ImGuiKey_GamepadDpadRight, 0x0008, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {ImGuiKey_GamepadStart, 0x0010, SDL_GAMEPAD_BUTTON_START},
    {ImGuiKey_GamepadBack, 0x0020, SDL_GAMEPAD_BUTTON_BACK},
    {ImGuiKey_GamepadFaceDown, 0x1000, SDL_GAMEPAD_BUTTON_SOUTH},
    {ImGuiKey_GamepadFaceRight, 0x2000, SDL_GAMEPAD_BUTTON_EAST},
    {ImGuiKey_GamepadFaceLeft, 0x4000, SDL_GAMEPAD_BUTTON_WEST},
    {ImGuiKey_GamepadFaceUp, 0x8000, SDL_GAMEPAD_BUTTON_NORTH},
};

// Up, down, left, right.
constexpr ImGuiKey kStickKeys[] = {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                                   ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight};

bool g_layer_up = false;

#if defined(_WIN32)
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn LoadXInput() {
  static const XInputGetStateFn fn = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
          return f;
        }
      }
    }
    return XInputGetStateFn(nullptr);
  }();
  return fn;
}
#endif

}  // namespace

void EnsurePadLayer() {
  static std::once_flag once;
  std::call_once(once, [] {
    // The wheel code's hints (wheel_input.cpp, WheelThread): this SDL must not
    // open pads through the drivers that would take them from the SDK's SDL.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    g_layer_up = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!g_layer_up) {
      REXLOG_WARN("[ui] Pad layer: SDL gamepad init failed: {}", SDL_GetError());
      return;
    }
    // Nothing pumps this SDL's event queue; state is read by polling.
    SDL_SetGamepadEventsEnabled(false);
    // The mappings the SDK's driver loads, so a DirectInput pad maps the same.
    const std::string mappings = rex::cvar::GetFlagByName("hid_mappings_file");
    std::error_code ec;
    if (!mappings.empty() && std::filesystem::exists(mappings, ec)) {
      SDL_AddGamepadMappingsFromFile(mappings.c_str());
    }
  });
}

std::vector<PadInfo> ListPads() {
  std::vector<PadInfo> pads;
  if (!g_layer_up) return pads;
  // Picks up devices that came or went since the last call.
  SDL_UpdateJoysticks();
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
    for (int i = 0; i < count; ++i) {
      // Mapped or not: in DirectInput mode an Xbox Elite has no mapping but
      // SDL still knows it as a gamepad by its USB ids. Wheels and the wheel
      // code's virtual device are not pads.
      if (SDL_IsJoystickVirtual(ids[i])) continue;
      if (!SDL_IsGamepad(ids[i]) && SDL_GetJoystickTypeForID(ids[i]) != SDL_JOYSTICK_TYPE_GAMEPAD) {
        continue;
      }
      const char* name = SDL_GetJoystickNameForID(ids[i]);
      pads.push_back({ids[i], name && name[0] ? name : "Controller"});
    }
    SDL_free(ids);
  }
  return pads;
}

MenuGamepad::MenuGamepad() {
  EnsurePadLayer();
}

MenuGamepad::~MenuGamepad() {
  Release(io_);
}

void MenuGamepad::Release(ImGuiIO* io) {
  for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad);
  pads_.clear();
  // Keys left down would stay down in ImGui after the pads are gone.
  if (io && fed_) {
    for (const ButtonKey& b : kButtons) io->AddKeyEvent(b.key, false);
    for (ImGuiKey k : kStickKeys) io->AddKeyAnalogEvent(k, false, 0.0f);
    io->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    fed_ = false;
  }
  connected_ = false;
}

void MenuGamepad::Feed(ImGuiIO& io) {
  io_ = &io;
  bool down[std::size(kButtons)] = {};
  float stick[4] = {};  // up, down, left, right, 0..1 before the dead zone
  bool any = false;

#if defined(_WIN32)
  // Xbox-family pads, the Elite included.
  if (XInputGetStateFn get = LoadXInput()) {
    for (DWORD slot = 0; slot < 4; ++slot) {
      XINPUT_STATE st{};
      if (get(slot, &st) != ERROR_SUCCESS) continue;
      any = true;
      for (size_t i = 0; i < std::size(kButtons); ++i) {
        down[i] = down[i] || (st.Gamepad.wButtons & kButtons[i].xinput) != 0;
      }
      const float x = st.Gamepad.sThumbLX / 32767.0f;
      const float y = st.Gamepad.sThumbLY / 32767.0f;  // XInput: up is positive
      stick[0] = std::max(stick[0], y);
      stick[1] = std::max(stick[1], -y);
      stick[2] = std::max(stick[2], -x);
      stick[3] = std::max(stick[3], x);
    }
  }
#endif

  // Everything else SDL has a DirectInput mapping for. An Xbox pad mapped
  // here as well reads twice; the presses are merged, so that is harmless.
  if (g_layer_up) {
    SDL_UpdateGamepads();
    int count = 0;
    std::unordered_map<uint32_t, SDL_Gamepad*> next;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
      for (int i = 0; i < count; ++i) {
        auto it = pads_.find(ids[i]);
        if (it != pads_.end()) {
          next.emplace(ids[i], it->second);
          pads_.erase(it);
        } else if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
          next.emplace(ids[i], pad);
        }
      }
      SDL_free(ids);
    }
    for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad);
    pads_ = std::move(next);
    for (auto& [id, pad] : pads_) {
      any = true;
      for (size_t i = 0; i < std::size(kButtons); ++i) {
        down[i] = down[i] || SDL_GetGamepadButton(pad, kButtons[i].sdl);
      }
      const float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
      const float y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;  // down positive
      stick[0] = std::max(stick[0], -y);
      stick[1] = std::max(stick[1], y);
      stick[2] = std::max(stick[2], -x);
      stick[3] = std::max(stick[3], x);
    }
  }

  connected_ = any;
  if (!any) {
    if (fed_) Release(&io);
    return;
  }
  constexpr float kDead = 0.35f;
  for (size_t i = 0; i < std::size(kButtons); ++i) io.AddKeyEvent(kButtons[i].key, down[i]);
  for (size_t i = 0; i < 4; ++i) {
    const float v = std::clamp((stick[i] - kDead) / (1.0f - kDead), 0.0f, 1.0f);
    io.AddKeyAnalogEvent(kStickKeys[i], v > 0.0f, v);
  }
  // Without it ImGui's NewFrame zeroes every gamepad key, so the events above
  // would never reach IsKeyPressed.
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  fed_ = true;
}

} // namespace mcla::ui
