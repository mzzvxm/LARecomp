#pragma once
//
// The game's own UI on and off for a trailer (trailer toolkit, work package M1c). The renderer keeps the UI the game draws
// (HUD, map, phone, prompts, gamertags, world icons, subtitles) as a layer of its own and shows the 3D picture plus that layer
// times an alpha: 1 is the game's frame, 0 the picture with no UI, in between a fade (src/gpu/ui_fade.h). The alpha is the
// debug-only cvar mcla_cin_hud_alpha, which has a row on the DEBUG menu's TOOLS page and which a scene sets through
// SetHudAlpha; the Plume renderer follows it at every frame. The pause menu, the DEBUG menu, the race pause and the map
// show their UI whole whatever the alpha is, and so do the F3 card and the other overlays of the SDK.
//
// At alpha 1 nothing runs: the renderer does not even ask for the layers.
//
// For the scene runner (M1a), which evaluates a HudTrack (hud_track.h) on every update:
//   ArmHud(true) when the scene has HUD keys, so that the renderer has made its pipeline and images before the first key
//   (otherwise that frame pays for them); SetHudAlpha(track.AlphaAt(t), 0) on every update (a value that has not changed
//   costs a compare); ReleaseHud() when the scene ends or is replaced, which puts the alpha back to what it was before the
//   scene.
//

namespace mc::cinematic {

// Show the UI at `alpha` (clamped to 0..1). The change takes `fade_seconds` (0 or less: at once) on the renderer's clock,
// which is the wall clock: a runner that steps its own clock (a capture) calls it with 0 on every update instead. Writes the
// cvar (so the DEBUG row shows it, and the last writer, a scene or a hand, wins). Any thread.
void SetHudAlpha(float alpha, float fade_seconds = 0.0f);

// The alpha asked for now (the cvar), 1 when nothing is hidden.
float HudAlpha();

// Keep the UI layer ready at alpha 1 (see above). Any thread.
void ArmHud(bool armed);

// The scene is over: the alpha goes back (over `fade_seconds`) to what it was when the scene first called SetHudAlpha, and
// the layer is no longer armed. Does nothing if SetHudAlpha was never called.
void ReleaseHud(float fade_seconds = 0.0f);

}  // namespace mc::cinematic
