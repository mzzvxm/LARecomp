// MCLA GPU renderer -- sharper UI art where the game's bitmaps are magnified: the cvars. See ui_sharp.h.

#include "ui_sharp.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(
    mcla_ui_sharp, true, "MCLA/GPU",
    "Sharper HUD and menu art where the game's bitmaps are blown up (a render scale above 1.25x of the console's 720p "
    "picture: 1080p and 4K windows). The game's buttons, icons, panels and text are bitmaps made for the console's "
    "1280x720 picture, drawn with a bilinear filter, so at 4K every texel is three pixels of soft edge. This reads them "
    "with a sharper filter (Catmull-Rom on the colour) and steepens the alpha across a bitmap's edges so that glyphs and "
    "icon outlines stay about one pixel soft, like a distance-field font. Off is the game's own picture, bit for bit; at "
    "720p (render scale 1x) it is the game's either way. Needs the shader pack's variant file (ui_sharp). Live; the "
    "first time it shows, the graphics card builds its shaders (a short stutter).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_ui_sharp_strength, 1.0, "MCLA/GPU",
    "Debug: how hard mcla_ui_sharp works. 1 is full Catmull-Rom on the colour and an alpha edge about one pixel wide; "
    "0.5 is half of it; 2 doubles the edge gain (a harder edge, more aliasing). Live.")
    .debug_only()
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_ui_sharp_census, false, "MCLA/GPU",
    "Debug: every 600 frames, log the textures the HUD and menus read (the draws after the scene is composed, the "
    "menus' render-to-texture views, and the frames that have no scene: loading, title, movies): the pixel shader, the "
    "texture's format, size, name and dictionary, the sampler's filters, the render scale of the target and how many "
    "reads were magnified. What the UI sharpening (mcla_ui_sharp) is tuned from. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::ui_sharp {

double Strength() {
  if (!REXCVAR_GET(mcla_ui_sharp)) {
    return 0.0;
  }
  return ClampStrength(REXCVAR_GET(mcla_ui_sharp_strength));
}

bool CensusOn() {
  return REXCVAR_GET(mcla_ui_sharp_census);
}

}  // namespace mcla::gpu::ui_sharp
