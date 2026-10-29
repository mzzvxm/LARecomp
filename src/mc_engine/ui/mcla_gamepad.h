#pragma once

#include <imgui.h>

#include <cstdint>
#include <unordered_map>

struct SDL_Gamepad;

namespace mcla::ui {

// Pad input for dialogs that run before the SDK's input driver exists (the
// save picker). It owns SDL's gamepad subsystem for its lifetime and shuts it
// fully down again when destroyed: the SDK driver learns about pads only from
// the SDL_EVENT_GAMEPAD_ADDED burst of its own SDL_InitSubSystem, so the
// subsystem must be down again by the time the driver starts.
//
// Buttons arrive as ImGui's gamepad keys (ImGuiKey_GamepadFaceDown for A,
// ImGuiKey_GamepadDpadUp, the left stick...), merged across every pad.
class MenuGamepad {
 public:
  MenuGamepad();
  ~MenuGamepad();
  MenuGamepad(const MenuGamepad&) = delete;
  MenuGamepad& operator=(const MenuGamepad&) = delete;

  // Call once per frame before reading ImGui's gamepad keys.
  void Feed(ImGuiIO& io);

  // Whether any pad is open, to pick pad or keyboard prompts.
  bool connected() const { return !pads_.empty(); }

 private:
  void Release(ImGuiIO* io);

  bool initialized_ = false;
  bool fed_ = false;
  std::unordered_map<uint32_t, SDL_Gamepad*> pads_;
  ImGuiIO* io_ = nullptr;
};

// Whether a SDL gamepad subsystem session ended since the last call: the
// controller toast uses it to tell a pad that really left from the hand-over
// between the picker's session and the SDK driver's.
bool TakeGamepadSessionEnded();

} // namespace mcla::ui
