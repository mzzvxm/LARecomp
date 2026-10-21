#pragma once
//
// The swatch palette behind the pause menu's UI OPTIONS rows.
//
// The 24 colors are meant to be the ones the garage's PickColorA/PickColorB
// buttons offer: sub_826A3780 reads swatch i from the game's paint table at
// table + 1568 + 48 * i (four big-endian floats, RGB first), where
// table = *(*(*(dword_82874374[2 or 34] + 48) + 132) + 64). That table holds no
// colors while the player is in the city (measured 20/10/2026), so the built-in
// set is what the pause menu normally shows.
//
// The rows are color buttons (mcla_menu::Color): closed they show their color
// beside the label; A opens a strip of five swatches with a yellow cursor and
// arrows, left/right walk it, A keeps the color and B puts the old one back --
// the garage's PickColor. The button is the garage's own: garage.xsf's sprite
// #1070, imported into guest memory (swf_import.h) because the city does not
// load the garage movie, placed in the pause movie's menu clip and driven the
// way the garage's Fillmenu drives it.
//

#include <cstdint>

namespace larecomp {

constexpr int kUiPaletteColors = 24;

// Fills the palette from the game (once; falls back to a built-in set when the
// game's table is not readable). Safe to call repeatedly. Guest thread.
void UiPaletteInit();

// Value k of a theme row: 0 is "" (the game's own color), 1..24 are the swatches
// as RRGGBB. Stable pointers after UiPaletteInit().
const char* UiPaletteHex(int k);

// Keeps the buttons in the pause movie in step with the UI OPTIONS screen.
// Called once per UI update, after mcUIManager::Update (swf_dom.cpp); the
// garage's button is imported the first time a color row is on screen. Guest
// thread.
void UiPaletteTick(uint8_t* base);

}  // namespace larecomp
