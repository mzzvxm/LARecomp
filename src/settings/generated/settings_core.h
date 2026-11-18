#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class CoreSettings final : public rex::SettingOwner {
 public:
  CoreSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, mcla_controller_toast, true, "Show the MCLA-styled toast when a controller connects or disconnects. Its corner follows "
    "controller_toast_position.");

  REX_SETTING(bool, profile_auto_select, false, "Start with the save named by user_name and never show the save picker, even when several "
    "saves exist. The picker's 'Always use this save' box sets it; hold Shift while the game "
    "starts to see the picker once anyway.");

  REX_SETTING(bool, skip_save_import, false, "Skip the save import wizard shown on first launch and start with no save. "
    "The wizard is skipped automatically anyway when no Xenia or RPCS3 save is "
    "found on the machine.", rex::kDebugOnly);

};

inline CoreSettings g_core;

inline auto& mcla_controller_toast = g_core.mcla_controller_toast;
inline auto& profile_auto_select = g_core.profile_auto_select;
inline auto& skip_save_import = g_core.skip_save_import;

}  // namespace larecomp::settings
