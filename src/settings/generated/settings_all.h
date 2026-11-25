#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include "settings/generated/settings_audio.h"
#include "settings/generated/settings_camera.h"
#include "settings/generated/settings_carbon.h"
#include "settings/generated/settings_cinematic.h"
#include "settings/generated/settings_core.h"
#include "settings/generated/settings_cutscenes.h"
#include "settings/generated/settings_debug.h"
#include "settings/generated/settings_diag.h"
#include "settings/generated/settings_diagnostics.h"
#include "settings/generated/settings_difficulty.h"
#include "settings/generated/settings_discord.h"
#include "settings/generated/settings_fun.h"
#include "settings/generated/settings_gpu.h"
#include "settings/generated/settings_garage.h"
#include "settings/generated/settings_graphics.h"
#include "settings/generated/settings_hud.h"
#include "settings/generated/settings_input_extra_buttons.h"
#include "settings/generated/settings_input_keyboard_driving.h"
#include "settings/generated/settings_input_keyboard_menus.h"
#include "settings/generated/settings_input.h"
#include "settings/generated/settings_lod.h"
#include "settings/generated/settings_localization.h"
#include "settings/generated/settings_menu_camera.h"
#include "settings/generated/settings_mods.h"
#include "settings/generated/settings_multiplayer.h"
#include "settings/generated/settings_nativegfx.h"
#include "settings/generated/settings_online.h"
#include "settings/generated/settings_patches.h"
#include "settings/generated/settings_performance.h"
#include "settings/generated/settings_photomode.h"
#include "settings/generated/settings_physics.h"
#include "settings/generated/settings_settings.h"
#include "settings/generated/settings_system_string_replacement.h"
#include "settings/generated/settings_textures.h"
#include "settings/generated/settings_ui.h"
#include "settings/generated/settings_ui_window.h"
#include "settings/generated/settings_video.h"
#include "settings/generated/settings_wheel.h"
#include "settings/generated/settings_world.h"

#include <span>

namespace larecomp::settings {

// Every generated owner, for RegisterAll.
inline std::span<rex::SettingOwner* const> GeneratedOwners() {
  static rex::SettingOwner* const owners[] = {
      &g_audio,
      &g_camera,
      &g_carbon,
      &g_cinematic,
      &g_core,
      &g_cutscenes,
      &g_debug,
      &g_diag,
      &g_diagnostics,
      &g_difficulty,
      &g_discord,
      &g_fun,
      &g_gpu,
      &g_garage,
      &g_graphics,
      &g_hud,
      &g_input_extra_buttons,
      &g_input_keyboard_driving,
      &g_input_keyboard_menus,
      &g_input,
      &g_lod,
      &g_localization,
      &g_menu_camera,
      &g_mods,
      &g_multiplayer,
      &g_nativegfx,
      &g_online,
      &g_patches,
      &g_performance,
      &g_photomode,
      &g_physics,
      &g_settings,
      &g_system_string_replacement,
      &g_textures,
      &g_ui,
      &g_ui_window,
      &g_video,
      &g_wheel,
      &g_world,
  };
  return owners;
}

}  // namespace larecomp::settings
