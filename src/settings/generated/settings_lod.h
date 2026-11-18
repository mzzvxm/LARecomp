#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class LODSettings final : public rex::SettingOwner {
 public:
  LODSettings() : SettingOwner("larecomp") {}

  // 0 is auto (gpu/gpu_class.h): 2.0 on a mid-range or high-end graphics card, the game's own 1.0 on
  // integrated graphics and until the card is known. A city sector is drawn in detail when it is loaded
  // and nearer than 300 m times this scale times a camera factor (up to 1.5), and the city is loaded up to
  // 600 m out, so 2.0 already shows every loaded sector in detail and more adds nothing.
  REX_SETTING(f64, lod_city_scale, 0.0, "How far away buildings and other city scenery keep their detail, as a multiple of the original "
    "distance. 0 (auto, the default) is 2.0 on a mid-range or high-end graphics card and 1.0 (the "
    "original) on integrated graphics. 2.0 shows every loaded part of the city in full detail, so "
    "higher adds nothing and costs memory and speed; lower simplifies it sooner (0.1 to 10; the menu "
    "has auto, 0.5, 1, 1.5 and 2).", rex::Range{0.0, 10.0});

  REX_SETTING(f64, lod_traffic_scale, 1.0, "How far away traffic cars keep their detailed models. 1.0 is the original distance; higher "
    "keeps the detail longer and costs speed, lower switches to simpler models sooner (0.1 to 10).", rex::Range{0.1, 10.0});

};

inline LODSettings g_lod;

inline auto& lod_city_scale = g_lod.lod_city_scale;
inline auto& lod_traffic_scale = g_lod.lod_traffic_scale;

}  // namespace larecomp::settings
