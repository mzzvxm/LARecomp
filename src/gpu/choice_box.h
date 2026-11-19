#pragma once
// ===========================================================================
// A box with two buttons that fit their labels (Windows)
// ===========================================================================
// The boxes that offer the compatibility renderer (renderer_fallback.h) have a button called "Use the compatibility renderer". The SDK's
// box on Windows (SDL's) gave that button the width of a short label and cut the text ("...compatibility re"), and it laid the buttons out
// right to left, so the action came last. This box measures every label with the font it draws, sizes each button from its own label
// (choice_box_layout.h has the numbers) and puts the action first, the way Windows' own boxes do:
//
//     [ Use the compatibility renderer ] [ Close ]
//
// The first button is the default (Enter), the last is the way out (Esc, the window's X, Alt+F4). The controls are plain BUTTON and
// STATIC windows in a dialog (class #32770): TaskDialog would size the buttons too, but it needs the common controls version 6 (this
// exe's manifest has none, and the SDK's boxes are plain Win32) and draws its buttons with DirectUI, which tools and screen readers
// cannot read the way they read a BUTTON.
//
// Where it cannot be shown (not Windows, the dialog could not be created) ShowTwoButtonBox returns nothing and the caller uses the SDK's
// own box, which works but may cut the label. The code is choice_box.cpp (in the gpu library, like the callers' renderer.cpp; the app's
// caller is in the build with the gpu library only).
//
// Like the SDK's box, it minimises this process's other windows while it is up (a full screen game window would hide it) and puts them
// back afterwards.

#include <functional>
#include <optional>
#include <string>

namespace mcla::gpu::fallback {

enum class BoxIcon { kError, kWarning, kInfo };

struct TwoButtonBox {
  BoxIcon icon = BoxIcon::kWarning;
  std::string title;    // UTF-8
  std::string body;     // UTF-8; line breaks are \n
  std::string action;   // the first button: the default (Enter)
  std::string way_out;  // the last button: Esc, the X and Alt+F4 press it

  // For checks without a screen (choice_box_test.cpp): false builds the box but never shows it (and does not touch other windows);
  // on_ready is called with the box's HWND (as a pointer) once its controls are in place, in the box's own message loop.
  bool show = true;
  std::function<void(void*)> on_ready;
};

// Shows the box and waits. 0 = the action (the first button), 1 = the way out (the last button, Esc, the X). Nothing when the box could
// not be built or shown (and always off Windows): the caller then asks the SDK's box.
std::optional<int> ShowTwoButtonBox(const TwoButtonBox& box);

}  // namespace mcla::gpu::fallback
