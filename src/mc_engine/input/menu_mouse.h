#pragma once
//
// The mouse in the game's menus (KBM-DESIGN.md, phase P6): hover moves the highlight to the row under the pointer, a left click is the menu's ACCEPT on that row (on a settings row's
// `<` or `>` side, LEFT or RIGHT: it steps the value), the right button and the back button are BACK, the wheel moves the highlight up and down. The pointer is shown while a menu is
// up and the mouse is the device in use (input_state.h). One setting, `menu_mouse` (MOUSE IN MENUS, on by default, live).
//
// How (menu_mouse_core.h has the rules and says why): the game's menus take no mouse, so the rows are read from the movie's own layout (menu.slotsA.slotN: the instance tree is walked to
// the stage), the pointer is put on the same stage (the middle 16:9 block of the frame, the way the UI is drawn), and what the pointer does goes in through the game's own menu buttons
// (keyboard_native.h PressMenuKey), so a click is exactly A. The one direct call is the list's own "select row" slot for the hover.
//
// Two kinds of menu can be read:
//   lists    the pause menu's pages and every settings page (an mcListViewFixed: highlight, scroll and row objects are read from the list; the hover sets the highlight directly);
//   UIMenus  the garage, the pop-ups and dialogs FlashNavigator draws into a movie's `menu` object (the highlighted row is read from the UIMenu; the hover walks the highlight with
//            UP / DOWN presses and gives the menu up when the presses do not move it).
// Anything else (a movie with no `slotsA`, a layout that is not a list of text rows, an object that does not look like a live list) is a FALLBACK menu: hover does nothing, a left click
// is ACCEPT on the highlighted row wherever the pointer is, the wheel and the right button work as always. The log says which menus were which (`[menu-mouse] menu ...`).
//
// Not handled: the pause menu's tab strip (LB / RB on the keyboard), the full map (map_mouse.cpp has its own), the Sidekick phone, the title and the sign-in screens.

#include <cstdint>

#include <rex/ppc/context.h>

namespace larecomp {

// Once per UI update (swf_dom.cpp, right after the game's UI step), guest thread: takes the window's mouse events since the last tick and does what they mean.
void MenuMouseTick(uint8_t* base);

// The pointer belongs on screen: a menu is up, the mouse is the device in use, the window has the focus and the setting is on. cursor_hide.cpp keeps the pointer visible while it is
// true. Any thread.
bool MenuMouseWantsPointer();

}  // namespace larecomp
