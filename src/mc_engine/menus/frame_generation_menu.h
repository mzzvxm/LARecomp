#pragma once
//
// The pause menu's FRAME GENERATION rows (on the DISPLAY page, after the upscaler's), next to nothing but themselves: the rows,
// what greys them, and what the FPS LIMIT and the latency reducer rows say while it runs.
//
// The renderer says what this PC can run (gpu.h GetFrameGenerationAvailability for AMD's FSR 3.1,
// GetDlssFrameGenerationAvailability for NVIDIA's DLSS): no row at all where neither can run (no
// FidelityFX files, no RTX 40 or 50 series card with the DLSS files, an unsupported card or driver, a
// build without them); once chosen and failed in this session the row stays and says so.
//
//   FRAME GENERATION        OFF / FSR 3.1 / DLSS       mcla_gpu_frame_generation  (live)
//                           (each value only where it can run here; the row's label says "FRAME GEN (APPLYING)"
//                           while the swap chain is made again and the generation context is made, or Streamline
//                           has not started; "(FAILED)" after a failure: off until the next start; "(NO ROOM)"
//                           while the card has no free video memory for FSR 3.1's (it is off and
//                           switches on by itself when there is room); DLSS says
//                           "(N/A)" where Streamline said it cannot run here, and on Vulkan the row
//                           reads "FRAME GEN (RESTART)": the device is made through Streamline only
//                           when DLSS frame generation is saved at the start)
//   FRAME GEN MULTIPLIER    2X / 3X / 4X               mcla_gpu_frame_generation_multiplier  (live;
//                           only on an RTX 50 series card, grey unless DLSS is chosen: the 4X and 3X
//                           are what the plugin says the card takes)
//   FRAME GEN FPS LIMIT     HALF DISPLAY 116 / 30 (60 SHOWN) .. 120 (240 SHOWN) / UNCAPPED
//                                                      mcla_gpu_frame_generation_fps_limit  (grey
//                           unless frame generation is on). The number is the frames the game
//                           RENDERS; twice as many are shown. HALF DISPLAY is the floor of 97% of half
//                           the display's rate and only offered on a display of 100 Hz or more: where it
//                           is not, that entry reads USE FPS LIMIT and the player's own cap
//                           (fps_limit) applies.
//
// While frame generation runs the FPS LIMIT row is grey ("FPS LIMIT (FRAME GEN)": the frame limiter
// uses FRAME GEN FPS LIMIT instead) and the vendor's latency reducer row (DISPLAY, after FPS LIMIT) is
// grey and on ("(FRAME GEN)": it is forced on, latency.h). Both rows are on DISPLAY, in pause_menu.cpp,
// and read their state from here (kFpsLimitDynamics, LatencyRowLocked).
//
// Width (pause_menu.cpp "Page width"): these rows are on DISPLAY, whose widest label is
// "AMD FSR RAY REGENERATION" (24, the denoiser row on a Radeon: upscaling_menu.cpp) and whose widest value is 17
// ("PERFORMANCE (50%)", the mode row's longest, and "FSR 3.1 (NO ROOM)" here): 41, the page's limit. The labels here:
// "FRAME GENERATION" (16), "FRAME GEN (APPLYING)" (20), "FRAME GEN (RESTART)" (19), "FRAME GEN MULTIPLIER"
// (20), "FRAME GEN FPS LIMIT" (19) and "FPS LIMIT (FRAME GEN)" (21), all under the page's widest. The values:
// "OFF", "FSR 3.1" (7), "FSR 3.1 (NO ROOM)" (17), "FSR 3.1 (FAILED)" (16), "DLSS" (4), "DLSS (FAILED)"
// (13), "DLSS (N/A)" (10), "2X" to "4X", "HALF DISPLAY 116" (16), "1/4 DISPLAY 58" (14),
// "120 (480 SHOWN)" (15) and "UNCAPPED" (8). "(APPLYING)" used to be the FSR 3.1 value's word (18 characters, one over
// what the page's 24 character label leaves): it is the row's label now, as UPSCALING's is.

#include "menu_items.h"

// The upscaling rows of the DISPLAY page (they follow its output rows; MenuDef::extra), in this order: the upscaler's (upscaling_menu.h: UPSCALING,
// its mode row, SHARPNESS, and the denoiser row where this PC has one) and then frame generation's (FRAME GENERATION, FRAME GEN MULTIPLIER,
// FRAME GEN FPS LIMIT), 7 at most. The combined table is made once per combination of the two and kept: the menu holds pointers
// into it. Without the Plume renderer in the build there are no rows. Once the page has been built (UpscalingMenuRelayout) this keeps returning the
// rows it was built with.
const mcla_menu::ItemDef* UpscalingPageRows(int& count);

// The page's relayout hook (MenuDef::relayout). apply=true: the page is being built (or built again): the rows it is built with are the live ones from now on. apply=false: whether the live rows
// (what this PC lists now: an upscaler's or frame generation's availability that was not known, or the denoiser row) are no longer those, so that the page is built again when it is opened next.
bool UpscalingMenuRelayout(bool apply);

namespace mcla_menu {

// The FPS LIMIT row's: grey, with "(FRAME GEN)", while frame generation runs.
extern const RowDynamics kFpsLimitDynamics;
// Whether the vendor's latency reducer row is locked (frame generation runs and forces it on) and
// the suffix it then carries.
bool LatencyRowLocked();

}  // namespace mcla_menu
