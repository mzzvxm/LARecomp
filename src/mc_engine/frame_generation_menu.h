#pragma once
//
// The pause menu's FRAME GENERATION rows (DISPLAY tab), next to nothing but themselves: the rows,
// what greys them, and what the FPS LIMIT and the latency reducer rows say while it runs.
//
// The renderer says what this PC can run (gpu.h GetFrameGenerationAvailability): no row at all where
// frame generation cannot run (no FidelityFX files, an unsupported card or driver, a build without
// it); once chosen and failed in this session the row stays and says so.
//
//   FRAME GENERATION        OFF / FSR 3.1              mcla_gpu_frame_generation  (live)
//                           ("(WAIT)" while the swap chain is made again and the generation context
//                           is made; "(FAILED)" after a failure: off until the next start;
//                           "(NO ROOM)" while the card has no free video memory for it: it is off
//                           and switches on by itself when there is room)
//   FRAME GEN FPS LIMIT     HALF DISPLAY 116 / 30 (60 SHOWN) .. 120 (240 SHOWN) / UNCAPPED
//                                                      mcla_gpu_frame_generation_fps_limit  (grey
//                           unless frame generation is on). The number is the frames the game
//                           RENDERS; twice as many are shown. HALF DISPLAY is the floor of 97% of half
//                           the display's rate and only offered on a display of 100 Hz or more: where it
//                           is not, that entry reads USE FPS LIMIT and the player's own cap
//                           (fps_limit) applies.
//
// While frame generation runs the FPS LIMIT row is grey ("FPS LIMIT (FRAME GEN)": the frame limiter
// uses FRAME GEN FPS LIMIT instead) and the vendor's latency reducer row (PERFORMANCE > RENDERING) is
// grey and on ("(FRAME GEN)": it is forced on, latency.h). Both rows are in pause_menu.cpp and read
// their state from here (kFpsLimitDynamics, LatencyRowLocked).
//
// Width (pause_menu.cpp "Page width"): these rows are on DISPLAY, whose widest label is
// "RENDER RES (SET BY FSR1)" (24 characters) and widest value "PERFORMANCE (50%)" (17): the longest
// label here is "FPS LIMIT (FRAME GEN)" (21) and the longest values "FSR 3.1 (NO ROOM)" (17) and
// "FSR 3.1 (FAILED)" and "HALF DISPLAY 116" (16 each).

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra): the upscaler's (upscaling_menu.h)
// and then frame generation's. The combined table is made once per combination of the two and kept:
// the menu holds pointers into it.
const mcla_menu::ItemDef* DisplayMenuRows(int& count);

namespace mcla_menu {

// The FPS LIMIT row's: grey, with "(FRAME GEN)", while frame generation runs.
extern const RowDynamics kFpsLimitDynamics;
// Whether the vendor's latency reducer row is locked (frame generation runs and forces it on) and
// the suffix it then carries.
bool LatencyRowLocked();

}  // namespace mcla_menu
