#pragma once
//
// The pause menu's upscaling rows (DISPLAY tab), next to nothing but themselves: the rows, what
// greys them, and what the NATIVE AA / MSAA rows (the game's own anti-aliasing) say while an
// upscaler does its job.
//
// Three rows, always the same three (what changes is each one's list, live):
//
//   UPSCALING     OFF / FSR 1 / FSR 3.1 / FSR 4 / DLSS    mcla_gpu_upscaler + mcla_gpu_fsr
//   <mode row>    named for the choice, below
//   SHARPNESS     OFF / 0.2 .. 1 (FSR 3.1, FSR 4) or 0.2 .. 1 (FSR 1); grey for OFF and DLSS
//
// UPSCALING lists only what this PC runs: FSR 1 (the renderer's own, on the present) always,
// FSR 3.1 and FSR 4 where their FidelityFX provider loads, DLSS on an NVIDIA RTX card. DLSS whose
// files are missing is listed as "DLSS (SETUP)" (the game runs without it); a saved choice that
// cannot run shows as OFF and is kept as it was saved. FSR 1 is not an upscaler of its own in the
// settings: it is mcla_gpu_upscaler = off with mcla_gpu_fsr on, and picking any other choice turns
// mcla_gpu_fsr off, so the two never stack from the menu (a larecomp.toml with both is shown as the
// temporal upscaler and left alone until the row is used).
//
// The mode row says "RENDER SCALE", "FSR 1 MODE", "FSR 3.1 MODE", "FSR 4 MODE" or "DLSS MODE" for
// what UPSCALING is on, and lists that choice's own modes, each with its own saved setting:
//
//   OFF       RENDER SCALE  100% / 150% / 200% / 300%         mcla_gpu_resolution_scale
//   FSR 1     FSR 1 MODE    ULTRA QUAL. (77%) / QUALITY (67%) / BALANCED (59%) / PERFORMANCE (50%)
//                                                              mcla_gpu_fsr_preset
//   FSR 3.1   FSR 3.1 MODE  NATIVE AA (100%) / QUALITY (67%) / BALANCED (59%) / PERFORMANCE (50%) /
//   FSR 4     FSR 4 MODE    ULTRA PERF. (33%)          mcla_gpu_fsr31_mode / mcla_gpu_fsr4_mode
//   DLSS      DLSS MODE     DLAA (100%) / QUALITY (67%) / BALANCED (58%) / PERFORMANCE (50%) /
//                           ULTRA PERF. (33%)                  mcla_gpu_dlss_mode
//
// The percentage is the share of the output the 3D scene is drawn at, per direction (the vendors'
// own names and ratios). RENDER SCALE is a share of the output too: 100% is the screen's scale (the
// window's 16:9 area, mcla_gpu_resolution_scale = 0, the default, which follows the window), and 150%,
// 200% and 300% are that many times it, written as the cvar's own number for the window as it is when
// the row is stepped (the cvar counts from the game's 1280x720: the scale the renderer makes is the
// same).
// The window caps the scale at 3x of 720p (2160p): a share the renderer cannot reach is not listed
// (at 1440p the list is 100% and 150%, at 4K it is 100% only). Values of the cvars that the lists do
// not offer (0.5X, a larecomp.toml number, FSR 1's "custom" and "ultra_performance") stay as they are
// and show as an entry of their own while the setting holds them, in order; stepping off one drops
// it. A scale the video memory cannot hold says "TOO BIG (NN%)" for the share that does run.
//
// A mode picked while the game runs is live: the scene is drawn at the new size from a frame
// boundary, once the upscaler has made what it needs for it (the old one works until then: about
// half a second for FSR, several seconds for DLSS, during which the UPSCALING row says "(WAIT)").
// DLSS modes the card cannot take at the output size say "(N/A)" in place of their share (gpu.h
// UpscalerAvailability::dlss_modes_refused): picking one saves the choice and the mode that runs
// goes on.
//
// SHARPNESS is one scale for every FSR: the RCAS pass's strength from 0 to 1, as FidelityFX's own
// sharpness. FSR 3.1 and FSR 4 keep it in mcla_gpu_upscaler_sharpness. FSR 1 keeps it as stops of
// reduction in mcla_gpu_fsr_sharpness (0 sharpest, 2 softest): the row shows 1 minus half of it,
// which is the same pass at the same strength (FSR 2 and 3 turn a sharpness s into 2 - 2s stops).
//
// DLSS on Vulkan: the graphics device is made with NVIDIA Streamline's help only when DLSS is the
// saved choice at the start (it costs seconds at every start), so a Vulkan game started with
// something else cannot switch to it. Choosing DLSS saves the choice for the next start, the
// UPSCALING row's label says "(RESTART)" (pause_menu.cpp reads the word in the row's label for its
// "Restart now?" prompt), its mode row is named for DLSS, grey, with "(RESTART)", and leaving the
// menu offers the restart. The renderer keeps what it runs until then (gpu.h
// UpscalerAvailability::kept): the AA rows and SHARPNESS follow that upscaler. A running FSR 1 is
// turned off by the pick (the saved choice is DLSS alone), since the settings never hold both.
//
// The temporal upscalers' shares are of the output, which the renderer takes as the screen's while one
// of them is the choice (the window's 16:9 area, whatever mcla_gpu_resolution_scale says: temporal.cpp
// ReadRenderScale, render_scale.h ChooseUpscalerOutputScale): in a 1080p window DLSS QUALITY draws
// 1280x720 and makes 1920x1080 of it. RENDER SCALE is therefore listed for OFF only, and its 100% (the
// cvar's 0, its default) is the same output.
// CHECK: in 720p, 1080p and 1440p windows with each upscaler on, against OFF at 100%: the picture
// the upscaler makes is the window's size (the log's "upscaling to the window's WxH") and the share in
// the mode row is of that; turning the upscaler on and off from the row keeps the picture's size at
// 100% and moves it to the RENDER SCALE row's choice when that is another (with a saved scale other
// than 100%).
//
// The AA rows of the NATIVE renderer follow a temporal upscaler: NATIVE AA (FXAA, SMAA) and MSAA
// are grey and say "(FSR 3.1 ON)" (or FSR 4, DLSS) while one runs. FSR 1 is not temporal and
// leaves them alone. With no upscaler the MSAA row is the player's own: OFF, 2X, 4X and 8X all
// change live ("(WAIT)" while the pipelines for a new level are made), none says RESTART.

#include "menu_items.h"

// The rows the DISPLAY tab appends after its own (MenuDef::extra). 0 and a null pointer without the
// Plume renderer's code in the build.
const mcla_menu::ItemDef* UpscalingMenuRows(int& count);

// Whether the rows UpscalingMenuRows returns are not the ones the page was built with (MenuDef::
// relayout). They never are: the three rows are the same whatever the PC can run, and what changes
// (the choices UPSCALING lists, a mode row's list) is read each time a row is drawn. Kept as the
// DISPLAY page's hook for a row set that does change.
bool UpscalingMenuRelayout(bool apply);

namespace mcla_menu {

// The NATIVE AA row's dynamics: grey, "(FSR 3.1 ON)" (or FSR 4, DLSS), while a temporal upscaler
// runs (FXAA and SMAA are held off then).
extern const RowDynamics kNativeAaDynamics;
// The MSAA row's: grey while a temporal upscaler runs, with the upscaler named in the suffix; with
// no upscaler the suffix says "(WAIT)" while a new level is being prepared and is empty otherwise.
extern const RowDynamics kMsaaDynamics;

}  // namespace mcla_menu
