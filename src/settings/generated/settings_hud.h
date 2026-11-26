#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class HUDSettings final : public rex::SettingOwner {
 public:
  HUDSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, rearview, false, "Show the rear view mirror at the top right of the screen (a feature the retail game "
    "ships switched off). Takes effect immediately.");

  REX_SETTING(bool, rearview_cars, false, "Show the racers' cars in the mirror. Retail's mirror phase leaves them out, and the game only "
    "treats a car as visible when the main camera sees it, so this runs the car's visibility test "
    "again with the mirror's camera and draws it with the colour pass. Don't combine it with "
    "rearview_traffic: traffic draws the same cars as black silhouettes. Needs rearview on.");

  REX_SETTING(i32, rearview_cars_lod, 2, "Debug: minimum model level for racers in the shared rear view. 2 uses the coarsest, 1 uses medium or coarser, 0 uses the game choice. Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(i32, rearview_cars_max, 3, "Debug: maximum visible racers in the shared rear view, selected by distance. 0 draws every visible racer. Live.", rex::kDebugOnly, rex::Range{0, 16});

  REX_SETTING(bool, rearview_diag, false, "Log, every couple of seconds, how many objects each render client holds when the mirror's "
    "phase begins. For working out why cars and traffic do not draw in it.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_every, 1, "Debug: draw the shared rear view only every Nth frame when its timed refresh is disabled. 1 draws every frame. Live.", rex::kDebugOnly, rex::Range{1, 8});

  REX_SETTING(i32, rearview_refresh_hz, 30, "Debug: shared rear view refreshes per second. 0 uses the frame interval. Camera cuts refresh immediately. Live.", rex::kDebugOnly, rex::Range{0, 240});

  REX_SETTING(f64, rearview_far, 150.0, "Debug: limit the rear-view camera range in metres. 0 keeps the game camera range. Live.", rex::Range{0.0, 5000.0}, rex::kDebugOnly);

  REX_SETTING(bool, rearview_interior, true, "Enable the functional cabin mirror when MIRRORS is on. Live.", rex::kDebugOnly);

  REX_SETTING(bool, rearview_hide_with_interior, true, "Hide the HUD rear view while the cabin mirror is drawn. Live.", rex::kDebugOnly);

  REX_SETTING(f64, rearview_fov_scale, 1.0, "Debug, the cheap mirror (perf/MIRRORS-SURVEY.md B): a multiple of the mirror camera's field of view (the stamp is 5.2:1, so the "
    "game's own is very wide). 1.0 (the default) leaves it; 0.5 shows roughly half the angle, so the game's own culling keeps fewer draws. "
    "Only narrows (0.1 to 1.0); the picture changes. Live.", rex::Range{0.1, 1.0}, rex::kDebugOnly);

  REX_SETTING(f64, rearview_lod_distance, 0.0, "Debug, the cheap mirror (perf/MIRRORS-SURVEY.md C): while the mirror's phase runs, a city sector is drawn in detail only within "
    "this many metres (the game's own radius is 300 m times lod_city_scale); the rest is the coarse one-model-a-sector city. 0 (the default) leaves it. "
    "Only ever shrinks the radius. CHECK: it works only if the game walks the city's sectors inside the mirror's phase (rearview_diag and the log's "
    "mirror draw count say). Live.", rex::Range{0.0, 2000.0}, rex::kDebugOnly);

  REX_SETTING(bool, rearview_frame, true, "Draw the mirror's frame. The retail game ships the frame meshes without their textures "
    "(black_small.dds and rearView.dds are gone), so the frame comes out solid white. Off hides "
    "it and leaves just the mirror view. Takes effect immediately.");

  REX_SETTING(std::string, rearview_position, "stock", "Where the mirror sits: stock (under the race position, top right), left_top, middle_top, "
    "left_down, middle_footer or right_down.", rex::Allowed("stock", "left_top", "middle_top", "left_down", "middle_footer", "right_down"));

  REX_SETTING(i32, rearview_shift_x, 0, "Fine-tunes the mirror and its frame horizontally on top of rearview_position, in 1280x720 "
    "pixels. Negative is left.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_shift_y, 0, "Fine-tunes the mirror and its frame vertically on top of rearview_position, in 1280x720 "
    "pixels. Positive is down.", rex::kDebugOnly);

  REX_SETTING(bool, rearview_traffic, false, "Put ambient traffic back in the mirror's phase, where retail has it. It draws nothing useful "
    "there (cars show up as black silhouettes, or not at all) and a car close to the mirror's "
    "camera blacks the view out, so it is taken out of the phase unless this is on. Needs rearview on.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_window_dx, 22, "Offset of the scene viewport from the frame window's outer corner, horizontally, in pixels. "
    "The window's left and right edges lean, so the viewport rectangle has to start inside the "
    "narrowest part or its corner pokes out of the frame.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_window_dy, 2, "Offset of the scene viewport from the frame window's outer corner, vertically, in pixels.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_window_h, 44, "Height of the scene viewport in pixels. The widest rectangle that fits inside the leaning "
    "window is 44 tall.", rex::kDebugOnly);

  REX_SETTING(i32, rearview_window_w, 229, "Width of the scene viewport in pixels. The widest rectangle that fits inside the leaning "
    "window is 229 wide.", rex::kDebugOnly);

  REX_SETTING(std::string, speed_units, "mph", "Speedometer / distance units. Drives the game's own metric formatter "
    "(sub_8238DDF0): game = console profile default (mph on NTSC), kmh = metric "
    "(km/h, km, m), mph = imperial (mph, miles, ft). Converts both the number and "
    "the unit label; the analog dial tick art stays as authored.\n"
    "kmh also converts the live HUD speedometer, which does NOT go through that "
    "formatter: the HUD movie multiplies m/s by 2.237 in its own ActionScript, so "
    "hud_units.cpp rewrites that constant (and the per-glyph unit label) in guest "
    "memory. The radar detector's speed-limit sign follows it too, through the game's "
    "own km/h branch for it (rounded to the nearest 5), which the game never takes.", rex::Allowed("game", "kmh", "mph"));

};

inline HUDSettings& g_hud = larecomp::Leak<HUDSettings>();

inline auto& rearview = g_hud.rearview;
inline auto& rearview_cars = g_hud.rearview_cars;
inline auto& rearview_cars_lod = g_hud.rearview_cars_lod;
inline auto& rearview_cars_max = g_hud.rearview_cars_max;
inline auto& rearview_diag = g_hud.rearview_diag;
inline auto& rearview_every = g_hud.rearview_every;
inline auto& rearview_far = g_hud.rearview_far;
inline auto& rearview_interior = g_hud.rearview_interior;
inline auto& rearview_hide_with_interior = g_hud.rearview_hide_with_interior;
inline auto& rearview_fov_scale = g_hud.rearview_fov_scale;
inline auto& rearview_frame = g_hud.rearview_frame;
inline auto& rearview_lod_distance = g_hud.rearview_lod_distance;
inline auto& rearview_refresh_hz = g_hud.rearview_refresh_hz;
inline auto& rearview_position = g_hud.rearview_position;
inline auto& rearview_shift_x = g_hud.rearview_shift_x;
inline auto& rearview_shift_y = g_hud.rearview_shift_y;
inline auto& rearview_traffic = g_hud.rearview_traffic;
inline auto& rearview_window_dx = g_hud.rearview_window_dx;
inline auto& rearview_window_dy = g_hud.rearview_window_dy;
inline auto& rearview_window_h = g_hud.rearview_window_h;
inline auto& rearview_window_w = g_hud.rearview_window_w;
inline auto& speed_units = g_hud.speed_units;

}  // namespace larecomp::settings
