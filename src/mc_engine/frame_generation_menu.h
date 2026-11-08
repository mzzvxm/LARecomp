#pragma once
//
// The pause menu's FRAME GENERATION rows (DISPLAY tab), next to nothing but themselves: the rows,
// what greys them, and what the FPS LIMIT and the latency reducer rows say while it runs.
//
// The renderer says what this PC can run (gpu.h GetFrameGenerationAvailability for AMD's FSR 3.1,
// GetDlssFrameGenerationAvailability for NVIDIA's DLSS): no row at all where neither can run (no
// FidelityFX files, no RTX 40 or 50 series card with the DLSS files, an unsupported card or driver, a
// build without them); once chosen and failed in this session the row stays and says so.
//
//   FRAME GENERATION        OFF / FSR 3.1 / DLSS       mcla_gpu_frame_generation  (live)
//                           (each value only where it can run here; "(APPLYING)" while the swap chain is
//                           made again and the generation context is made, or Streamline has not
//                           started; "(FAILED)" after a failure: off until the next start; "(NO ROOM)"
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
// uses FRAME GEN FPS LIMIT instead) and the vendor's latency reducer row (PERFORMANCE > RENDERING) is
// grey and on ("(FRAME GEN)": it is forced on, latency.h). Both rows are in pause_menu.cpp and read
// their state from here (kFpsLimitDynamics, LatencyRowLocked).
//
// Width (pause_menu.cpp "Page width"): these rows are on DISPLAY, whose widest label is
// "RESOLUTION (7680X4320)" (22; 23 at five digits across) and whose other values go to 17 ("PERFORMANCE
// (50%)"). The labels here: "FRAME GENERATION" (16), "FRAME GEN (RESTART)" (19), "FRAME GEN MULTIPLIER"
// (20), "FRAME GEN FPS LIMIT" (19) and "FPS LIMIT (FRAME GEN)" (21), all under the page's widest. The values:
// "OFF", "FSR 3.1 (APPLYING)" (18), "FSR 3.1 (NO ROOM)" (17), "FSR 3.1 (FAILED)" (16), "DLSS (APPLYING)"
// (15), "DLSS (FAILED)" (13), "DLSS (N/A)" (10), "2X" to "4X", "HALF DISPLAY 116" (16), "1/4 DISPLAY 58" (14),
// "120 (480 SHOWN)" (15) and "UNCAPPED" (8). "FSR 3.1 (APPLYING)" is the page's widest value now, 18: with
// the widest label that is 40 characters at 7680 across and 41 at five digits across, the page's 41.

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra), in this order: the upscaler's
// (upscaling_menu.h), `picture` (the page's own MSAA, POST AA, ANISOTROPIC and ambient occlusion rows,
// pause_menu.cpp kPictureItems), and then frame generation's. The combined table is made once per
// combination of the three and kept: the menu holds pointers into it. Without the Plume renderer in
// the build `picture` is returned as it is.
const mcla_menu::ItemDef* DisplayMenuRows(const mcla_menu::ItemDef* picture, int picture_count,
                                          int& count);

namespace mcla_menu {

// The FPS LIMIT row's: grey, with "(FRAME GEN)", while frame generation runs.
extern const RowDynamics kFpsLimitDynamics;
// Whether the vendor's latency reducer row is locked (frame generation runs and forces it on) and
// the suffix it then carries.
bool LatencyRowLocked();

}  // namespace mcla_menu
