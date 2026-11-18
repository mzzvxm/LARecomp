#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pad_feed_core.h"

namespace mcla::ui {

// LARecomp's pad layer for its own dialogs (the save picker, the server browser, the mod browser,
// the identity window). Two SDLs can run in this process: the SDK's, inside rexruntime.dll, owns
// the pads in game; the exe's own static copy is shared with the wheel code. (On Linux they are
// one SDL.) The controller toast does not read pads: it follows the SDK's device list by events
// (controller_toast.cpp, input_state.h).
//
// Xbox pads (Windows) are read through XInput, read-only and shared with the SDK. A slot with
// no pad is XInput's slow case ("costs milliseconds"), so an empty slot is asked once a second
// and not on every drawn frame, and at once when the device watcher says a pad came or went.
// Every other pad the exe's SDL knows as a gamepad is read through SDL, which asks only the
// pads it has open: the DualSense, Switch and generic pads navigate the dialogs too, and on
// Linux and the Steam Deck every pad does.
//
// SDL's pads are opened when a dialog starts reading and let go when it closes
// (ReleaseOverlayPads), so the exe's SDL holds none while the game is played.

// Starts the exe's SDL joystick layer for the rest of the session, with the
// wheel code's hints whichever of the two runs first. Idempotent.
void EnsurePadLayer();

// Every pad there is, merged, for a dialog that is drawn this frame (XInput's button bits, sticks
// -1..1 with up and right positive). UI thread. Cheap when no pad is plugged in.
larecomp::padfeed::PadSample ReadOverlayPads();

// The dialog closed: close the pads this layer opened and look at the pad list afresh at the next
// reading. Idempotent; another dialog that is still open simply reopens them on its next frame.
void ReleaseOverlayPads();

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
  ImGuiIO* io_ = nullptr;
};

} // namespace mcla::ui
