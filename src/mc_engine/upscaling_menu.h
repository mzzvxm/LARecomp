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
//   UPSCALING        OFF / FSR 3.1 / FSR 4 / DLSS   mcla_gpu_upscaler
//   FSR 3.1          NATIVE AA (100%) / QUALITY (67%) / BALANCED (59%) / PERFORMANCE (50%) /
//                    ULTRA PERF. (33%)              mcla_gpu_fsr31_mode  (grey unless FSR 3.1 is on)
//   FSR 4            the same                       mcla_gpu_fsr4_mode   (grey unless FSR 4 is on)
//   DLSS             DLAA (100%) / QUALITY (67%) / BALANCED (58%) / PERFORMANCE (50%) /
//                    ULTRA PERF. (33%)              mcla_gpu_dlss_mode   (grey unless DLSS is on)
//   SHARPNESS        OFF / 0.2 .. 1                 mcla_gpu_upscaler_sharpness (FSR's; grey with
//                                                   "(FSR ONLY)" while off and for DLSS)
//
// The mode rows are labelled with the technology alone and their values shortened to fit the menu's
// width (pause_menu.cpp, "Page width"); the cvars keep the full values (ultra_performance).
//
// DLSS on an RTX card whose files are missing (or fail their signature check) is listed too, as
// "DLSS (SETUP)", with its mode row grey ("DLSS (NEEDS SETUP)"); the game runs without it. On
// Vulkan DLSS is listed as "DLSS (RESTART)" until the game was started with it chosen (the device
// is made with NVIDIA Streamline's help); its mode row says "DLSS (AFTER RESTART)" meanwhile.
//
// Each technology keeps its own mode; the value is the vendor's name and the share of the output
// the 3D scene is drawn at, per direction. NATIVE AA and DLAA draw it at the full size and only
// smooth it; the others draw it smaller and the upscaler makes it whole. The scene's size is fixed
// when the game starts, so the mode rows are restart-only like RENDER SCALE: held for the next
// start, with "(RESTART)" on the row.
//
// The set of rows depends only on what the probe found, which is final a moment after start, so it
// does not change under an open menu.
//
// The AA rows of the NATIVE renderer follow the upscaler too: NATIVE AA (FXAA, SMAA) and MSAA are
// grey and say "(FSR 3.1 ON)" (or FSR 4, DLSS) while one is active. MSAA is a start-up switch of the
// game: when the upscaler was turned on after a start with MSAA the row says "(RESTART)" instead
// (the upscaler takes over at the next start), as it does with the upscaler off.

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra). 0 and a null pointer where no
// upscaler can run.
const mcla_menu::ItemDef* UpscalingMenuRows(int& count);

namespace mcla_menu {

// The NATIVE AA row's dynamics: grey, "(FSR 3.1 ON)" (or FSR 4, DLSS), while an upscaler is
// chosen and can run (FXAA and SMAA are held off then).
extern const RowDynamics kNativeAaDynamics;
// The MSAA row's: grey while an upscaler is chosen and can run; the suffix says whether the game
// already runs without MSAA for it or does from the next start.
extern const RowDynamics kMsaaDynamics;

}  // namespace mcla_menu
