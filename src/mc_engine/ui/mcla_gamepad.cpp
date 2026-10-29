#include "mcla_gamepad.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

namespace mcla::ui {

namespace {

std::atomic<bool> g_session_ended{false};

struct ButtonKey {
  SDL_GamepadButton button;
  ImGuiKey key;
};

constexpr ButtonKey kButtons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
    {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
    {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
    {SDL_GAMEPAD_BUTTON_WEST, ImGuiKey_GamepadFaceLeft},
    {SDL_GAMEPAD_BUTTON_NORTH, ImGuiKey_GamepadFaceUp},
    {SDL_GAMEPAD_BUTTON_START, ImGuiKey_GamepadStart},
    {SDL_GAMEPAD_BUTTON_BACK, ImGuiKey_GamepadBack},
};

constexpr ImGuiKey kStickKeys[] = {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                                   ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight};

}  // namespace

MenuGamepad::MenuGamepad() {
  initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!initialized_) {
    REXLOG_WARN("[ui] Menu gamepad: SDL gamepad init failed: {}", SDL_GetError());
    return;
  }
  // The same mappings the SDK driver loads, so a pad that works in game works
  // here too.
  const std::string mappings = rex::cvar::GetFlagByName("hid_mappings_file");
  std::error_code ec;
  if (!mappings.empty() && std::filesystem::exists(mappings, ec)) {
    SDL_AddGamepadMappingsFromFile(mappings.c_str());
  }
}

MenuGamepad::~MenuGamepad() {
  Release(io_);
  if (initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_session_ended.store(true);
  }
}

void MenuGamepad::Release(ImGuiIO* io) {
  for (auto& [id, pad] : pads_) {
    SDL_CloseGamepad(pad);
  }
  pads_.clear();
  // Keys left down would stay down in ImGui after the pads are gone.
  if (io && fed_) {
    for (const ButtonKey& b : kButtons) io->AddKeyEvent(b.key, false);
    for (ImGuiKey k : kStickKeys) io->AddKeyAnalogEvent(k, false, 0.0f);
    io->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    fed_ = false;
  }
}

void MenuGamepad::Feed(ImGuiIO& io) {
  io_ = &io;
  if (!initialized_) return;

  // Driven by hand: the window's event loop may sleep while nothing happens.
  SDL_UpdateGamepads();

  // Open newly attached pads, drop the ones that left.
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

  if (pads_.empty()) {
    if (fed_) Release(&io);
    return;
  }

  bool down[std::size(kButtons)] = {};
  float stick[4] = {};  // up, down, left, right
  constexpr float kDead = 0.35f;
  for (auto& [id, pad] : pads_) {
    for (size_t i = 0; i < std::size(kButtons); ++i) {
      down[i] = down[i] || SDL_GetGamepadButton(pad, kButtons[i].button);
    }
    const float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    const float y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    stick[0] = std::max(stick[0], -y);
    stick[1] = std::max(stick[1], y);
    stick[2] = std::max(stick[2], -x);
    stick[3] = std::max(stick[3], x);
  }
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

bool TakeGamepadSessionEnded() {
  return g_session_ended.exchange(false);
}

} // namespace mcla::ui
