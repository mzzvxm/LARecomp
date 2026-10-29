#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Gamepad;

namespace mcla::ui {

// LARecomp's pad layer for its own dialogs. Two SDLs run in this process: the
// SDK's, inside rexruntime.dll, owns the pads in game; the exe's own static
// copy is shared with the wheel code, which keeps it to DirectInput (HIDAPI,
// RawInput, WGI and XInput off) so it never takes a pad from the SDK's. In
// that mode SDL only counts a pad as a gamepad when it has a DirectInput
// mapping for it, which the Xbox Elite has not. So Xbox-family pads are read
// through XInput (read-only, shared with the SDK), and SDL's DirectInput
// gamepads cover the rest (PlayStation, Switch, ...).

// Starts the exe's SDL joystick layer for the rest of the session, with the
// wheel code's hints whichever of the two runs first. Idempotent.
void EnsurePadLayer();

struct PadInfo {
  uint32_t id;
  std::string name;
};
// Every connected pad (gamepad-type joysticks, mapped or not; no wheels or
// virtual devices), for the controller toast.
std::vector<PadInfo> ListPads();

// Pad input for the save picker, which runs before the SDK's input system
// exists. Buttons arrive as ImGui's gamepad keys (ImGuiKey_GamepadFaceDown for
// A, ImGuiKey_GamepadDpadUp, the left stick...), merged across every pad.
class MenuGamepad {
 public:
  MenuGamepad();
  ~MenuGamepad();
  MenuGamepad(const MenuGamepad&) = delete;
  MenuGamepad& operator=(const MenuGamepad&) = delete;

  // Call once per frame before reading ImGui's gamepad keys.
  void Feed(ImGuiIO& io);

  // Whether any pad is there, to pick pad or keyboard prompts.
  bool connected() const { return connected_; }

 private:
  void Release(ImGuiIO* io);

  bool connected_ = false;
  bool fed_ = false;
  std::unordered_map<uint32_t, SDL_Gamepad*> pads_;
  ImGuiIO* io_ = nullptr;
};

} // namespace mcla::ui
