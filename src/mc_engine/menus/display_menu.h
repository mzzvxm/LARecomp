#pragma once
//
// The pause menu's DISPLAY MODE and RESOLUTION rows (DISPLAY tab), next to nothing but themselves:
// what they list, what greys them, and what they write. The window they change is display_mode.h's.
//
//   DISPLAY MODE   WINDOWED / BORDERLESS / FULLSCREEN
//                  `fullscreen` + `fullscreen_exclusive`: WINDOWED is fullscreen off, BORDERLESS is
//                  fullscreen on with exclusive off (what the old FULLSCREEN row's ON was), FULLSCREEN
//                  is exclusive: the display is switched to the resolution. Where a display mode
//                  cannot be switched (not Windows) the row has the first two. Live.
//   RESOLUTION     decides by the mode, and is the output resolution: what the upscalers make and
//                  where the HUD is drawn (the window's size; RENDER SCALE is a share of it).
//                  WINDOWED     16:9 sizes from 1280X720 up that fit the display with the window's
//                               frame; the saved size when it is none of them (a larecomp.toml number)
//                               is an entry of its own while it is the setting. Live on Windows and,
//                               through SDL, on Linux. Grey with the window's size in the label
//                               ("RESOLUTION (1280X800)") where the compositor sets it (gamescope).
//                               "(RESTART)" only where the SDK's SDL window was not found: the size
//                               is then read at the next start.
//                  BORDERLESS   grey: the display's own size, in the label ("RESOLUTION (3840X2160)").
//                  FULLSCREEN   the modes the display lists, one per size (the display's own refresh
//                               rate where the size has it, else the highest it lists; the driver's
//                               extra modes above the monitor's own, NVIDIA DSR and AMD VSR, are
//                               among them); the mode nearest to a saved size the display does not
//                               list. The size is the one chosen here, kept in `fullscreen_resolution`:
//                               entering FULLSCREEN from a window starts at the display's own size, not
//                               at the window's. Live. The list is
//                               the display's own, however low the size it is switched to: the display's
//                               own mode, the mode it is in and the monitor's own size are always on it
//                               (display_mode.h ExclusiveSizes, display_sizes.h).
//                  Sizes are PHYSICAL pixels (the client area of the window; display_mode.h): at 150%
//                  scaling 1600X900 is a window of 1600 by 900 pixels plus its frame.
//                  It is written as "WxH" (the SDK's text, which reads "720p", "1080p", "1440p", "4k" and
//                  "WxH"; an old value ("4k") is matched by its size) into `resolution` in WINDOWED, and
//                  into `fullscreen_resolution` in FULLSCREEN. The SDK also takes `resolution` as the
//                  game's video mode at the next start.
//
// Width (pause_menu.cpp, "Page width"), in characters: DISPLAY MODE 12; RESOLUTION 10, 22 with its
// suffix " (7680X4320)" (a display of 10240 pixels across: 23; the grey WINDOWED row of gamescope has
// the same form), 20 with " (RESTART)" or " (DISPLAY)" (where the display's size cannot be asked). Values: "BORDERLESS" and "FULLSCREEN" 10; sizes 9 at most
// ("3840X2160"), 10 at five digits across. The page's widest label is the latency row's "AMD ANTI-LAG 2
// (NOT WORKING)" (28, pause_menu.cpp "Page width"), then this one at 23 (22 at 7680 across); its widest value
// 10: 38 of 41.
// CHECK: DISPLAY open on a 4K display in BORDERLESS ("RESOLUTION (3840X2160)" grey), in FULLSCREEN and in
// WINDOWED, on NATIVE and on XENIA (four rows there): the right edge, the button prompts and the scroll bar are
// not cut. The page is built once ("native rows built" once) while the lists change under it: stepping DISPLAY
// MODE greys and relabels RESOLUTION at once, and its value list changes length (a handful of sizes in
// WINDOWED, about 20 modes in FULLSCREEN, one in BORDERLESS) without the row showing a stale text.

#include "settings/ui_expr_menu.h"
