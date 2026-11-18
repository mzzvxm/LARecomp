#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class UISettings final : public rex::SettingOwner {
 public:
  UISettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, button_prompts, "auto", "On-screen button glyphs. auto = the glyphs of what you used last, on every screen at once: "
    "PlayStation glyphs after a Sony controller, Xbox glyphs after any other, key caps after the "
    "keyboard or the mouse (the menu's keys while a menu is open, the driving keys on the road). "
    "xbox = A/B/X/Y + LB/RB/LT/RT, playstation = cross/circle/square/triangle + L1/R1/L2/R2, "
    "keyboard = key caps, whatever you use. Every set is drawn into the same button bitmaps "
    "(keyboard_native.cpp), so a change reaches a screen that is already open on the next frame; "
    "the UI movies' own 'platform' flag stays on Xbox (see prompts_live).", rex::Allowed("auto", "xbox", "playstation", "keyboard"));

  REX_SETTING(bool, prompts_live, true, "Button prompts follow the device in use on screens that are already open (the garage, a pop-up, the pause menu). The UI movies are told they run on an Xbox, "
    "and the PlayStation set and the key caps are painted over the Xbox button bitmaps; a screen picks a changed 'platform' flag only when it is built again, "
    "so the flag cannot do this alone (KBM-P0). Off = the older way: the flag shows the PlayStation set, screens already open keep their glyphs until reopened, "
    "and only the keyboard's key caps are painted. A debug switch.", rex::kDebugOnly);

  REX_SETTING(bool, ui_advance_on_input, true, "Menus answer a key press on the update it arrives in. A menu movie paced to 30 "
    "steps a second (ui_movie_pacing) would otherwise show the highlight moving up "
    "to 33 ms after the press, 17 ms on average. On, a menu key event (a press, or a "
    "repeat of a held direction) makes each menu, pop-up and map screen take one "
    "extra step in the update it comes in, and its 30 Hz schedule starts again from "
    "it, so the animations keep close to the console's speed. The HUD is not "
    "stepped for it.", rex::kDebugOnly);

  REX_SETTING(bool, ui_color_buttons, true, "Show the pause menu's color buttons (UI OPTIONS) as the garage's own "
    "PickColor: garage.xsf's color button, placed in the pause movie and "
    "driven the way the garage's Fillmenu drives it.", rex::kDebugOnly);

  REX_SETTING(bool, ui_countdown_fix, true, "Keep the race countdown (3, 2, 1, GO) in time when the HUD's gauge movie steps "
    "less often than the game updates (HUDGAUGE or '*' in ui_movie_pacing, or "
    "ui_interpolate on). The "
    "movie's script adds the last update's time to the "
    "countdown's clock on every step, so with 30 steps a second at 120 FPS the "
    "countdown ran at a quarter of its speed and a race began on the 2 or the 3. "
    "On, the clock is given the time of all the updates since the movie's last step.", rex::kDebugOnly);

  REX_SETTING(bool, ui_input_repeat_30hz, true, "Menu cursor auto-repeat at the console's pace. The game repeats a held "
    "direction or button by counting frames -- the first repeat after 7, then one "
    "every 6 -- which on the 360 meant 233 ms and then 200 ms. Above 30 FPS it "
    "repeats faster with every frame: at 144 FPS a quick tap moves a list or the "
    "photo album's slot cursor two places, and holding it scrolls ~24 items a "
    "second. On, the count advances once every 1/30 s.", rex::kDebugOnly);

  REX_SETTING(bool, ui_interpolate, true, "Smooth UI. Every UI movie (menus, pop-ups, map panels and the HUD) steps at the "
    "console's 30 a second, so they run at the console's speed at any frame rate, and "
    "what moved in the last step is drawn between where it was and where it is: "
    "needles, the song ticker, the menus' lists and highlights, page changes and "
    "fades glide instead of stepping. The picture is one step (up to 33 ms) behind the "
    "game; nothing is blended below about 47 FPS. Off, only the movies named in "
    "ui_movie_pacing step at 30 a second and nothing is blended.");

  REX_SETTING(bool, ui_interpolate_camera, true, "With ui_interpolate: also draw the pause menu's 3D between the movie's last two "
    "steps: the camera that swings in when the menu opens, between its pages and out "
    "again, and the place, size and turn of the 3D panels. Off leaves them in 30 Hz "
    "steps.", rex::kDebugOnly);

  REX_SETTING(std::string, ui_movie_pacing, "PHOTOALBUMMOVIE", "Flash movies advanced at the console's 30 frames a second instead of one frame "
    "per rendered frame. The game steps every UI movie exactly one frame per update, "
    "which on the 360 was 30 times a second; above 30 FPS those movies play faster "
    "-- the photo album twice as fast at 60. Comma-separated movie names "
    "(PHOTOALBUMMOVIE, POPUPMOVIE, PAUSEMOVIE, GARAGEMOVIE, NAVSYSMOVIE, ...), '*' "
    "for every movie, empty for none. The HUD is two movies, HUDMAPMOVIE and "
    "HUDGAUGE (needles, song ticker, race countdown); it is left out by default: "
    "stepping 30 times a second it would visibly lag a 60 FPS world, unless the "
    "frames between the steps are drawn (ui_interpolate, which paces every movie "
    "and ignores this list). Only movies the game steps one frame per update are "
    "paced.", rex::kDebugOnly);

  REX_SETTING(std::string, ui_theme_accent, "", "Replaces the blue/teal that every menu uses (selection bar, highlights, "
    "arrows) with this color, as RRGGBB hex. Empty keeps the game's own. "
    "Shades of the original keep their brightness relative to it.");

  REX_SETTING(std::string, ui_theme_frame, "", "Replaces the gray of the menu frame, as RRGGBB hex. Empty keeps the "
    "game's own.");

  REX_SETTING(std::string, ui_theme_skip_movies, "HUDMOVIE", "Comma-separated Flash movie names the theme leaves alone. The HUD movie "
    "shares the menus' teal and yellow in its speedometer; empty themes every "
    "movie.", rex::kDebugOnly);

  REX_SETTING(std::string, ui_theme_title, "", "Replaces the yellow of the menu titles and the spinning globe, as RRGGBB "
    "hex. Empty keeps the game's own.");

};

inline UISettings g_ui;

inline auto& button_prompts = g_ui.button_prompts;
inline auto& prompts_live = g_ui.prompts_live;
inline auto& ui_advance_on_input = g_ui.ui_advance_on_input;
inline auto& ui_color_buttons = g_ui.ui_color_buttons;
inline auto& ui_countdown_fix = g_ui.ui_countdown_fix;
inline auto& ui_input_repeat_30hz = g_ui.ui_input_repeat_30hz;
inline auto& ui_interpolate = g_ui.ui_interpolate;
inline auto& ui_interpolate_camera = g_ui.ui_interpolate_camera;
inline auto& ui_movie_pacing = g_ui.ui_movie_pacing;
inline auto& ui_theme_accent = g_ui.ui_theme_accent;
inline auto& ui_theme_frame = g_ui.ui_theme_frame;
inline auto& ui_theme_skip_movies = g_ui.ui_theme_skip_movies;
inline auto& ui_theme_title = g_ui.ui_theme_title;

}  // namespace larecomp::settings
