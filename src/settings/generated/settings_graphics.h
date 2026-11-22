#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class GraphicsSettings final : public rex::SettingOwner {
 public:
  GraphicsSettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, larecomp_texture_folder, "", "Folder the texture dumps are written to. Empty: textures_dump/ in the "
                      "mod folder (modloader/, mods/ or models/ beside the executable)", rex::kDebugOnly);

};

inline GraphicsSettings& g_graphics = larecomp::Leak<GraphicsSettings>();

inline auto& larecomp_texture_folder = g_graphics.larecomp_texture_folder;

}  // namespace larecomp::settings
