#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class MenuCameraSettings final : public rex::SettingOwner {
 public:
  MenuCameraSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, menu_cam_custom, false, "Place the menu camera at the shot from <exe dir>/menu_cameras.txt matching "
    "the current slot. Slots with no line keep the stock camera. The file is "
    "re-read when it changes, so you can tune it with the menu on screen.", rex::kDebugOnly);

  REX_SETTING(f64, menu_cam_cycle, 0.0, "Seconds between shots, rotating through every line in menu_cameras.txt. "
    "0 = no rotation. The game does not rotate the front-end camera itself — it "
    "only calls setMenuCam for the network and garage menus, so without this the "
    "boot screen sits on one shot forever. Ignored while menu_cam_lock is on.", rex::Range{0.0, 600.0}, rex::kDebugOnly);

  REX_SETTING(bool, menu_cam_diag, false, "Log the camera the game hands to the renderer (mode id, position, matrix "
    "rows) once per mode change, and every shot this places. Turn on if a shot "
    "does not show up or points the wrong way.", rex::kDebugOnly);

  REX_SETTING(bool, menu_cam_lock, false, "Always show the same shot instead of letting the game rotate. Off = the "
    "game picks (setMenuCam plus its own visibility probes) and menu_cam_slot is "
    "ignored; on = menu_cam_slot every time. This is the on/off switch — the "
    "slot number by itself never locks anything.", rex::kDebugOnly);

  REX_SETTING(bool, menu_cam_only_custom, false, "Show only your own shots. Whatever slot the game picks is remapped onto the "
    "lines present in menu_cameras.txt, so the stock cameras never appear. The "
    "game's rotation still decides which of your shots comes up; turn on "
    "menu_cam_lock to pin one. Needs menu_cam_custom.", rex::kDebugOnly);

  REX_SETTING(bool, menu_cam_record, false, "Capture the game's own menu shots into menu_cameras.txt as 'cam' lines. "
    "There is no table of stock camera spots in the XEX — they only exist as "
    "runtime state — so this samples the real camera whenever it moves somewhere "
    "new (25+ units) and appends it with the next free slot number. Leave it on "
    "while the menus cycle, then turn it off: every shot, stock and custom, is "
    "now a line you own. Works with menu_cam_custom on; it always records what "
    "the game produced, never what this placed.", rex::kDebugOnly);

  REX_SETTING(i32, menu_cam_slot, 0, "Which shot menu_cam_lock pins (0-63). Ignored while menu_cam_lock is off, "
    "so parking a number here is harmless.", rex::kDebugOnly);

};

inline MenuCameraSettings& g_menu_camera = larecomp::Leak<MenuCameraSettings>();

inline auto& menu_cam_custom = g_menu_camera.menu_cam_custom;
inline auto& menu_cam_cycle = g_menu_camera.menu_cam_cycle;
inline auto& menu_cam_diag = g_menu_camera.menu_cam_diag;
inline auto& menu_cam_lock = g_menu_camera.menu_cam_lock;
inline auto& menu_cam_only_custom = g_menu_camera.menu_cam_only_custom;
inline auto& menu_cam_record = g_menu_camera.menu_cam_record;
inline auto& menu_cam_slot = g_menu_camera.menu_cam_slot;

}  // namespace larecomp::settings
