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

  REX_SETTING(bool, fullscreen_exclusive, false, "With fullscreen on: exclusive fullscreen. The display is switched to the size of `resolution` "
    "(the nearest mode it lists; the display's own size when `resolution` is empty) while the game "
    "has the focus, and put back when the game loses it or closes. Off is borderless fullscreen: a "
    "window without a frame the size of the display, the display left as it is. Windows only.");

};

inline UIWindowSettings g_ui_window;

inline auto& fullscreen_exclusive = g_ui_window.fullscreen_exclusive;

}  // namespace larecomp::settings
