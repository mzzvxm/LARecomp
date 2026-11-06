#pragma once
//
// The pause menu's UPSCALING rows (DISPLAY tab), next to nothing but themselves: the rows, what
// greys them, and what the NATIVE AA row (FXAA/SMAA) says while an upscaler does its job.
//
// The renderer's upscaler backend (src/gpu/temporal_backend.h) says what this PC can run
// (gpu.h GetUpscalerAvailability). Rows for what it cannot run are not listed: no UPSCALING row at
// all where no upscaler can run (no FidelityFX files, an unsupported card or driver, a build
// without them), FSR 3.1 only where FSR 4's provider does not load, FSR 4 listed only when it
// really loads. A saved choice that cannot run shows as OFF and is kept as it was saved.
//
//   UPSCALING        OFF / FSR 3.1 / FSR 4          mcla_gpu_upscaler
//   FSR 3.1 MODE     NATIVE AA (100%)               mcla_gpu_fsr31_mode  (grey unless FSR 3.1 is on)
//   FSR 4 MODE       NATIVE AA (100%)               mcla_gpu_fsr4_mode   (grey unless FSR 4 is on)
//   SHARPNESS        OFF / 0.2 .. 1                 mcla_gpu_upscaler_sharpness (grey while off)
//
// Each technology keeps its own mode. NATIVE AA (100%) is the only mode in this step: the scene is
// drawn at the full size and only smoothed; the vendors' other modes come with a scene drawn smaller.
//
// The set of rows depends only on what the probe found, which is final a moment after start, so it
// does not change under an open menu.
//
// The AA rows of the NATIVE renderer follow the upscaler too: NATIVE AA (FXAA, SMAA) and MSAA are
// grey and say "(HANDLED BY FSR 3.1)" while one is active. MSAA is a start-up switch of the game:
// when the upscaler was turned on after a start with MSAA the row says "(FSR 3.1 AFTER RESTART)"
// instead, and with the upscaler off it keeps its own "(RESTART)".

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra). 0 and a null pointer where no
// upscaler can run.
const mcla_menu::ItemDef* UpscalingMenuRows(int& count);

namespace mcla_menu {

// The NATIVE AA row's dynamics: grey, "(HANDLED BY FSR 3.1)", while an upscaler is chosen and can
// run (FXAA and SMAA are held off then).
extern const RowDynamics kNativeAaDynamics;
// The MSAA row's: grey while an upscaler is chosen and can run; the suffix says whether the game
// already runs without MSAA for it or does from the next start.
extern const RowDynamics kMsaaDynamics;

}  // namespace mcla_menu
