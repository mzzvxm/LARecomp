#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class VideoSettings final : public rex::SettingOwner {
 public:
  VideoSettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, display_gamma, "tv", "Display gamma ramp. tv: the console's for an HDTV (BT.709), as the game ships. pc: sRGB, "
    "for PC monitors, with brighter shadows. Live.", rex::Allowed("tv", "pc"));

};

inline VideoSettings& g_video = larecomp::Leak<VideoSettings>();

inline auto& display_gamma = g_video.display_gamma;

}  // namespace larecomp::settings
