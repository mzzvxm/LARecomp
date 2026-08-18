#pragma once
//
// km/h on the live HUD speedometer, without shipping an asset.
//
// The digital readout is not produced by the game's unit formatter
// (sub_8238DDF0 -- that one serves menus and distances, which is what the
// `speed_units` cvar already covers). It comes out of the HUD movie's own
// ActionScript:
//
//     sub_822112A8 @0x822117FC   fsqrts f8, f9        ; |velocity| in m/s
//                  @0x82211800   stfs   f8, 0(r4)     ; -> GFx var d_Speedvalue
//
//     hud.xsf, action block @0xb982:
//         this.speedometer.speed.text = int(_global.d_Speedvalue * 2.237)
//
// 2.237 is m/s -> mph. Two bytes of intent, then: 3.6 gives km/h, and the unit
// label next to it is static text -- a per-glyph record, which is why searching
// the movie for the string "mph" never finds it.
//
// Both edits live in the resource's VIRTUAL segment, which after load is
// ordinary guest memory, so they are applied by patching RAM rather than by
// repacking resources/ui/hud/hud.xsf into a mod archive. Shipping the resource
// works too, but it forces the movie's bitmaps to be re-uploaded, and one of
// them (the tach glow ring, BC3 128x128) comes back wrong through that path --
// a bullseye in one quadrant plus a striped alpha, which reads on screen as a
// grating over the gauge. Patching memory never touches that path.
//

// Per-frame, driven from Patch_DeltaTimePre() in hooks.cpp. Scans for the two
// signatures once, applies them, then only re-checks that they are still in
// place (the movie can be reloaded across a title relaunch).
void TickHudUnits();
