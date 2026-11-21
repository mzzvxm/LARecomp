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

  // 0 is auto (gpu/gpu_class.h): 3.0 on a high-end graphics card, 2.0 on a mid-range one, 1.5 on integrated graphics and until the
  // card is known. The scale multiplies the game's own distances for a traffic car (Patch_ScaleTrafficLOD, at 0x827903E0 in the
  // car's LOD pick): at 1.0 the most detailed model ends at about 40 m and the middle one at about 155 m, the coarsest goes on to
  // the 500 m where the car is not drawn. 3.0 puts the steps at 120 m and 465 m; more only pushes the second step past the cull.
  REX_SETTING(f64, lod_traffic_scale, 0.0, "How far away traffic cars keep their detailed models, as a multiple of the original distance. "
    "0 (auto, the default) is 3.0 on a high-end graphics card, 2.0 on a mid-range one and 1.5 on integrated graphics. "
    "Higher keeps the detail longer and costs processor time; lower simplifies it sooner (0.1 to 10; the menu has "
    "auto, 0.5, 1, 1.5, 2, 3 and 5).", rex::Range{0.0, 10.0});

};

inline LODSettings g_lod;

inline auto& lod_city_scale = g_lod.lod_city_scale;
inline auto& lod_traffic_scale = g_lod.lod_traffic_scale;

}  // namespace larecomp::settings
