#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class InputKeyboardMenusSettings final : public rex::SettingOwner {
 public:
  InputKeyboardMenusSettings() : SettingOwner("larecomp") {}

  // ── Menus (the FlashNavigator mapper). Comma separated key names, the same names
  // as keybind_* ("Return", "Escape", "Space", "PageUp", "F5", "Semicolon" ...).
  // A combination such as Shift+Up counts as its last key.
  REX_SETTING(std::string, kb_ui_accept, "Return,Space", "Menus: A / accept");

  REX_SETTING(std::string, kb_ui_back, "Escape,Backspace", "Menus: B / back");

  REX_SETTING(std::string, kb_ui_down, "Down,S", "Menus: down");

  REX_SETTING(std::string, kb_ui_lb, "Q", "Menus: LB (previous tab)");

  REX_SETTING(std::string, kb_ui_left, "Left,A", "Menus: left");

  REX_SETTING(std::string, kb_ui_lt, "PageUp", "Menus: LT");

  REX_SETTING(std::string, kb_ui_rb, "E", "Menus: RB (next tab)");

  REX_SETTING(std::string, kb_ui_right, "Right,D", "Menus: right");

  REX_SETTING(std::string, kb_ui_rt, "PageDown", "Menus: RT");

  REX_SETTING(std::string, kb_ui_select, "Tab", "Menus: the Back/Select button (GPS, map). Start has no key here on purpose.");

  REX_SETTING(std::string, kb_ui_start, "Return,Escape", "Start: opens the pause menu on the road and answers \"PRESS START\" on the title screen. "
    "It is Start only while NO menu is open: with a menu up these keys are accept/back and "
    "nothing else, which is what keeps Enter from being accept and Start at once. A key held "
    "when a menu closes stays off until released, so the Enter that picks CONTINUE cannot "
    "reopen the pause menu.");

  REX_SETTING(std::string, kb_ui_up, "Up,W", "Menus: up");

  REX_SETTING(std::string, kb_ui_x, "X", "Menus: X");

  REX_SETTING(std::string, kb_ui_y, "Y", "Menus: Y");

};

inline InputKeyboardMenusSettings g_input_keyboard_menus;

inline auto& kb_ui_accept = g_input_keyboard_menus.kb_ui_accept;
inline auto& kb_ui_back = g_input_keyboard_menus.kb_ui_back;
inline auto& kb_ui_down = g_input_keyboard_menus.kb_ui_down;
inline auto& kb_ui_lb = g_input_keyboard_menus.kb_ui_lb;
inline auto& kb_ui_left = g_input_keyboard_menus.kb_ui_left;
inline auto& kb_ui_lt = g_input_keyboard_menus.kb_ui_lt;
inline auto& kb_ui_rb = g_input_keyboard_menus.kb_ui_rb;
inline auto& kb_ui_right = g_input_keyboard_menus.kb_ui_right;
inline auto& kb_ui_rt = g_input_keyboard_menus.kb_ui_rt;
inline auto& kb_ui_select = g_input_keyboard_menus.kb_ui_select;
inline auto& kb_ui_start = g_input_keyboard_menus.kb_ui_start;
inline auto& kb_ui_up = g_input_keyboard_menus.kb_ui_up;
inline auto& kb_ui_x = g_input_keyboard_menus.kb_ui_x;
inline auto& kb_ui_y = g_input_keyboard_menus.kb_ui_y;

}  // namespace larecomp::settings
