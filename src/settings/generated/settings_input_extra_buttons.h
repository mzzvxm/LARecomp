#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class InputExtraButtonsSettings final : public rex::SettingOwner {
 public:
  InputExtraButtonsSettings() : SettingOwner("larecomp") {}

  // The extra buttons of a pad (back paddles: L4, L5, R4, R5). A pad that has them sends nothing
  // of its own for them -- the maker's software makes each one press some other button or key --
  // so the host key the pad is set to send is named here and the game gets a button of its own.
  // Bind it in Options > Controller > Customize like any other button. Empty = not used.
  REX_SETTING(std::string, extra_button_l4, "", "Host key(s) that the pad's L4 button sends (e.g. \"Numpad4\"; several: \"Numpad4,F13\"). "
    "The key stops acting as a keyboard key and becomes the game button L4. Empty = unused.");

  REX_SETTING(std::string, extra_button_l5, "", "Host key(s) that the pad's L5 button sends. See extra_button_l4.");

  REX_SETTING(std::string, extra_button_r4, "", "Host key(s) that the pad's R4 button sends. See extra_button_l4.");

  REX_SETTING(std::string, extra_button_r5, "", "Host key(s) that the pad's R5 button sends. See extra_button_l4.");

};

inline InputExtraButtonsSettings& g_input_extra_buttons = larecomp::Leak<InputExtraButtonsSettings>();

inline auto& extra_button_l4 = g_input_extra_buttons.extra_button_l4;
inline auto& extra_button_l5 = g_input_extra_buttons.extra_button_l5;
inline auto& extra_button_r4 = g_input_extra_buttons.extra_button_r4;
inline auto& extra_button_r5 = g_input_extra_buttons.extra_button_r5;

}  // namespace larecomp::settings
