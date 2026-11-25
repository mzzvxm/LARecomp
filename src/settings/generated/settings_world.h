#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class WorldSettings final : public rex::SettingOwner {
 public:
  WorldSettings() : SettingOwner("larecomp") {}

  REX_SETTING(f64, mod_breakable_speed_scale, 1.0, "Multiplier on the speed a mod's breakable prop needs to break.", rex::kDebugOnly);

  REX_SETTING(bool, mod_breakables, true, "Props a mod baked into the city sectors break off when the car hits them fast "
    "enough (models/<mod>/breakables.bin); a slow bump still stops the car.", rex::kDebugOnly);

  REX_SETTING(f64, mod_glow_intensity, 1.0, "Mod glow intensity multiplier.", rex::kDebugOnly);

  REX_SETTING(f64, mod_glow_size, 1.8, "Mod glow size multiplier (on top of each glow's own).", rex::kDebugOnly);

  REX_SETTING(bool, mod_glows, true, "Light glows a mod places in the world (models/<mod>/glows.bin), drawn by MCLA's "
    "glow renderer.", rex::kDebugOnly);

  REX_SETTING(i32, mod_glows_max, 64, "Mod glows submitted per frame, nearest first. MCLA draws 96 a frame in all, "
    "car lights included.", rex::kDebugOnly);

  REX_SETTING(f64, mod_light_intensity, 1.0, "Mod light intensity multiplier, applied when the mod's light grid is built (city load).", rex::kDebugOnly);

  REX_SETTING(bool, mod_lights, true, "Dynamic lights a mod adds to its part of the map (models/<mod>/lights.bin): its lamps "
    "light the ground and the cars at night, on a light grid of its own. Built at city load.", rex::kDebugOnly);

  REX_SETTING(std::string, start_garage, "hollywood", "Which district garage the car starts the session at: hollywood (what the game "
    "always does), beaches, sc, or last (the garage nearest the car when the game was "
    "closed). The car is moved there once, when it first appears at Hollywood's door, so a "
    "change is seen at the next start of the game.", rex::kRequiresRestart);

  REX_SETTING(std::string, start_garage_last, "hollywood", "The garage nearest the car the last time it was driven (hollywood, beaches or sc); "
    "start_garage = last starts the session there. The game keeps it up to date.", rex::kDebugOnly);

  REX_SETTING(f64, time_of_day, 12.0, "Hour to hold the world at (0-24, fractional), applied while "
    "time_of_day_hold is on. Takes effect every frame, so dragging the bar walks "
    "the sun across the sky live. Menu presets: 6.0 sunrise, 16.75 afternoon, "
    "17.4 sunset, 23.5 night.", rex::Range{0.0, 24.0});

  REX_SETTING(bool, time_of_day_hold, false, "Freeze the world at time_of_day instead of letting the day cycle run. This "
    "is the on/off switch — the hour by itself changes nothing while this is "
    "off, so you can park a value and arm it later.");

  REX_SETTING(bool, time_of_day_realtime, false, "Follow your computer's clock: the world sits at the real local hour and "
    "keeps up with it. Outranks time_of_day_hold.");

  REX_SETTING(f64, time_of_day_speed, 1.0, "Multiplier on the day cycle speed. Only applies while time_of_day_hold is "
    "off — a held hour does not advance at all. 1 = stock, 0 = frozen where it "
    "is, 10 = ten times faster. The stock rate varies with the hour (about 2 "
    "minutes per hour at midday, 3 at night).", rex::Range{0.0, 100.0});

  REX_SETTING(std::string, warp_spot, "", "Where F9 drops the car: \"x y z heading\" (heading in degrees, 0 = facing +Z). "
    "Empty = the first point a mod names in models/<mod>/warps.txt. The car is "
    "dropped onto whatever collision is below the point.", rex::kDebugOnly);

  REX_SETTING(std::string, weather, "game", "Force the weather. 'game' leaves it alone, including its own random "
    "changes; a named sky pins that one and stops the automatic changes. 'real' "
    "follows the actual weather where you are, refreshed once an hour — it "
    "resolves your location from your IP through ipapi.co and reads the forecast "
    "from open-meteo.com, so those two services see your IP while it is on. "
    "Applies live.", rex::Allowed("game", "real", "nice", "cloudy", "stormy", "foggy"));

};

inline WorldSettings& g_world = larecomp::Leak<WorldSettings>();

inline auto& mod_breakable_speed_scale = g_world.mod_breakable_speed_scale;
inline auto& mod_breakables = g_world.mod_breakables;
inline auto& mod_glow_intensity = g_world.mod_glow_intensity;
inline auto& mod_glow_size = g_world.mod_glow_size;
inline auto& mod_glows = g_world.mod_glows;
inline auto& mod_glows_max = g_world.mod_glows_max;
inline auto& mod_light_intensity = g_world.mod_light_intensity;
inline auto& mod_lights = g_world.mod_lights;
inline auto& start_garage = g_world.start_garage;
inline auto& start_garage_last = g_world.start_garage_last;
inline auto& time_of_day = g_world.time_of_day;
inline auto& time_of_day_hold = g_world.time_of_day_hold;
inline auto& time_of_day_realtime = g_world.time_of_day_realtime;
inline auto& time_of_day_speed = g_world.time_of_day_speed;
inline auto& warp_spot = g_world.warp_spot;
inline auto& weather = g_world.weather;

}  // namespace larecomp::settings
