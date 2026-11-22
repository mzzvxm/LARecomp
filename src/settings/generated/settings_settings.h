#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class SettingsSettings final : public rex::SettingOwner {
 public:
  SettingsSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, show_test_movie, false, "Show the TestMovie debug button in the pause menu", rex::kDebugOnly);

};

inline SettingsSettings& g_settings = larecomp::Leak<SettingsSettings>();

inline auto& show_test_movie = g_settings.show_test_movie;

}  // namespace larecomp::settings
