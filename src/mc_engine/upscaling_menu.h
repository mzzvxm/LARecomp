#pragma once
//
// The pause menu's UPSCALING rows (DISPLAY tab), next to nothing but themselves: the rows, what
// greys them, and what the NATIVE AA row (FXAA/SMAA) says while an upscaler does its job.
//
// The renderer's upscaler backends (src/gpu/temporal_backend.h) say what this PC can run
// (gpu.h GetUpscalerAvailability). Rows for what it cannot run are not listed: no UPSCALING row at
// all where no upscaler can run (no FidelityFX files, an unsupported card or driver, a build
// without them), FSR 3.1 only where FSR 4's provider does not load, FSR 4 listed only when it
// really loads, DLSS only on an NVIDIA RTX card. A saved choice that cannot run shows as OFF and is
// kept as it was saved.
//
//   UPSCALING        OFF / FSR 3.1 / FSR 4 / DLSS   mcla_gpu_upscaler  ("(WAIT)" while a change is made)
//   FSR 3.1          NATIVE AA (100%) / QUALITY (67%) / BALANCED (59%) / PERFORMANCE (50%) /
//                    ULTRA PERF. (33%)              mcla_gpu_fsr31_mode  (grey unless FSR 3.1 is on)
//   FSR 4            the same                       mcla_gpu_fsr4_mode   (grey unless FSR 4 is on)
//   DLSS             DLAA (100%) / QUALITY (67%) / BALANCED (58%) / PERFORMANCE (50%) /
//                    ULTRA PERF. (33%)              mcla_gpu_dlss_mode   (grey unless DLSS is on)
//   SHARPNESS        OFF / 0.2 .. 1                 mcla_gpu_upscaler_sharpness (FSR's; grey while
//                                                   off, and DLSS has none)
//
// The mode rows are labelled with the technology alone and their values shortened to fit the menu's
// width (26 characters of label and value: "FSR 3.1: PERFORMANCE (50%)", so no suffix fits them
// either; they have none while they are live); the cvars keep the full values (ultra_performance).
//
// DLSS on an RTX card whose files are missing (or fail their signature check) is listed too, as
// "DLSS (SETUP)", with its mode row grey ("DLSS (NEEDS SETUP)"); the game runs without it. On
// Vulkan the device is made with NVIDIA Streamline's help whenever the PC can run DLSS (an RTX
// card with the files in place), whatever is chosen at the start, so DLSS switches on live there
// too. Only an RTX card whose device was made without it (Plume took another GPU than the one the
// hook looked at) lists DLSS as "DLSS (RESTART)", and its mode row says "DLSS (AFTER RESTART)":
// the one upscaler choice that can still wait for a start.
//
// Each technology keeps its own mode; the value is the vendor's name and the share of the output
// the 3D scene is drawn at, per direction. NATIVE AA and DLAA draw it at the full size and only
// smooth it; the others draw it smaller and the upscaler makes it whole. The upscaler, its choice
// and its mode are live: the scene is drawn at the new size from a frame boundary, once the new
// upscaler context is ready (the old one works until then: about half a second for FSR, several
// seconds for DLSS, during which the UPSCALING row says "(WAIT)"). The picture itself changes after
// resuming: the pause menu draws the world at 4x samples, which the upscalers do not take.
//
// The set of rows depends only on what the probe found, which is final a moment after start, so it
// does not change under an open menu.
//
// The AA rows of the NATIVE renderer follow the upscaler too: NATIVE AA (FXAA, SMAA) and MSAA are
// grey and say "(HANDLED BY FSR 3.1)" (or FSR 4, DLSS) while one is active. MSAA is a start-up switch of the game
// (not part of the live change): when the upscaler was turned on after a start with MSAA the row
// says "(FSR 3.1 AFTER RESTART)" instead, and with the upscaler off it keeps its own "(RESTART)".

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra). 0 and a null pointer where no
// upscaler can run.
const mcla_menu::ItemDef* UpscalingMenuRows(int& count);

namespace mcla_menu {

// The NATIVE AA row's dynamics: grey, "(HANDLED BY FSR 3.1)" (or FSR 4, DLSS), while an upscaler is
// chosen and can run (FXAA and SMAA are held off then).
extern const RowDynamics kNativeAaDynamics;
// The MSAA row's: grey while an upscaler is chosen and can run; the suffix says whether the game
// already runs without MSAA for it or does from the next start.
extern const RowDynamics kMsaaDynamics;

}  // namespace mcla_menu
