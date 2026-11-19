#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class UIWindowSettings final : public rex::SettingOwner {
 public:
  UIWindowSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, fullscreen_exclusive, false, "With fullscreen on: exclusive fullscreen. The display is switched to the size of "
    "`fullscreen_resolution` (the nearest mode it lists; the display's own size when it is empty) "
    "while the game has the focus, and put back when the game loses it or closes. Off is borderless "
    "fullscreen: a window without a frame the size of the display, the display left as it is. "
    "Windows only.");

  REX_SETTING(std::string, fullscreen_resolution, "", "With fullscreen and fullscreen_exclusive on: the size the display is switched to, as \"WxH\" "
    "(the nearest mode the display lists). Empty, the default: the display's own size, no switch. The "
    "window's size is `resolution`, which FULLSCREEN does not use: a size chosen for a window was once "
    "taken as the display mode and put the display in 1280x720. Windows only.");

};

inline UIWindowSettings g_ui_window;

inline auto& fullscreen_exclusive = g_ui_window.fullscreen_exclusive;
inline auto& fullscreen_resolution = g_ui_window.fullscreen_resolution;

}  // namespace larecomp::settings
