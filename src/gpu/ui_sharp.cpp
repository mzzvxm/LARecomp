// MCLA GPU renderer -- sharper UI art where the game's bitmaps are magnified: the two debug cvars. See ui_sharp.h.

#include "ui_sharp.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_DOUBLE(
    mcla_ui_sharp, 0.0, "MCLA/GPU",
    "Debug: sharper HUD and menu art where the game's bitmaps are blown up (at a render scale above 1x: 1080p and 4K "
    "windows). The game's buttons, icons, panels and text are bitmaps made for the console's 1280x720 picture, drawn "
    "with a bilinear filter, so at 4K every texel is three pixels of soft edge. This reads them with a sharper filter "
    "(Catmull-Rom on the colour) and steepens the alpha across a bitmap's edges so that glyphs and icon outlines stay "
    "about one pixel soft, like a distance-field font. 0 is off (the game's own, bit for bit), 1 is on, 2 is a harder "
    "edge. Draws that are not magnified and render scale 1x are the game's either way. Needs the shader pack's variant "
    "file (ui_sharp). Live; the first time it shows, the graphics card builds its shaders (a short stutter).")
    .debug_only()
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_ui_sharp_census, false, "MCLA/GPU",
    "Debug: every 600 frames, log the textures the HUD and menus (the draws after the scene is composed) read: the "
    "pixel shader, the texture's format, size, name and dictionary, the sampler's filters, the render scale of the "
    "target and how many reads were magnified. What the UI sharpening (mcla_ui_sharp) is tuned from. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::ui_sharp {

double Strength() {
  return ClampStrength(REXCVAR_GET(mcla_ui_sharp));
}

bool CensusOn() {
  return REXCVAR_GET(mcla_ui_sharp_census);
}

}  // namespace mcla::gpu::ui_sharp
