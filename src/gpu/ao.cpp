// MCLA GPU renderer -- screen-space ambient occlusion. See ao.h.

#include "ao.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ao_fast.h"
#include "ao_halo.h"
#include "ao_paint.h"
#include "ao_stable.h"
#include "car_shadow_map.h"
#include "contact_trace.h"
#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_plan.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "split.h"
#include "ssgi_level.h"
#include "ssgi_light.h"
#include "ssgi_stable.h"
#include "texture_bytes.h"
#include "traffic_shadow.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_ao, "auto", "MCLA/GPU",
    "Ambient occlusion: darkens creases, kerbs, the ground under cars and where objects meet, "
    "from the picture's depth, only on the sky light and not on the sun's (the game's own "
    "constants say how much of a surface's light is which). off, high (half resolution, 2 slices), "
    "ultra (full resolution, 3 slices) or auto (the default): off on an integrated graphics card, "
    "high on a discrete one with less than 5.5 GiB of video memory, ultra on the rest. The pause menu "
    "and the launcher show the same words. A value that is none of these (the low of an older "
    "larecomp.toml) is ignored and the setting runs as auto. "
    "Costs graphics card time, ultra more than high. "
    "Also drawn behind the pause menu and on the map, so a change made there shows at once. "
    "Live.")
    .allowed({"auto", "off", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ao_intensity, "default", "MCLA/GPU",
    "How strong ambient occlusion looks: light, default, strong or max. Each is a set of four "
    "numbers: how strongly it darkens, how far it looks for things that block the sky light, how "
    "hard the measure is pushed, and how much it also darkens the part of a surface the sun lights; "
    "the stronger ones reach farther and make every crease, corner and under-car shadow darker. "
    "Only used while mcla_gpu_ao is not off (the pause menu's AO INTENSITY row). Live.")
    .allowed({"light", "default", "strong", "max"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_fade_end, 160.0, "MCLA/GPU",
    "The distance, in the game's world units (about metres), where ambient occlusion has faded "
    "out entirely; it starts to fade at half of it. Fog, rain and particles are not darkened "
    "either way: they are drawn over the picture afterwards. Live.")
    .debug_only()
    .range(20.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_car_reach, 0.4, "MCLA/GPU",
    "Diagnostic: how far what a car blocks of the sky light reaches on the road and the other things "
    "round it, as a share of the occlusion's radius (0.4 of the default 3 m is 1.2 m: the dark line "
    "where the tyres meet the road and the car's footprint, not a halo). The car's own body is "
    "darkened with the whole radius as before. 1 (or 0): a car blocks the sky light as far as a wall "
    "does. Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the ambient occlusion as a grey picture instead of applying it "
    "(only while it is on): 1 the factor the picture is multiplied by (white: none), 2 the "
    "visibility, 3 the share of the light that is ambient, 4 the surface normals, 5 the distance "
    "(a log scale), 6 how much of the last frames each pixel kept (white: all it can, black: none, "
    "so it is new, a vehicle whose own motion is not known, or has no history yet; the history itself "
    "goes on as usual), 7 the occlusion's own part of the factor (the visibility with the ambient "
    "share, the strength and the distance fade; white: none), 8 the pixels it takes for vehicles "
    "(white). 0: "
    "off. Live.")
    .debug_only()
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_history, 12, "MCLA/GPU",
    "Diagnostic: how many frames ambient occlusion averages to take its sampling noise out, 1 to "
    "64. Each frame's occlusion is found again in the next by the camera's motion and kept only "
    "where it is the same surface; more frames is smoother and slower to follow a moving shadow "
    "(a car's), fewer is noisier. 1: no averaging (two blur passes on every frame instead). Live.")
    .debug_only()
    .range(1, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ao_stable, true, "MCLA/GPU",
    "Diagnostic: the occlusion's noise stays as small in a big picture as in a small one: its blur reaches "
    "as far in the picture at 4K (7x7 pixels) as it does at 720 lines (3x3), the wide blur of the pixels "
    "that have no history follows, and the noise's shift from frame to frame is a lattice of 13 frames that "
    "any 12 consecutive frames cover evenly. The bounce light's a-trous filter has more iterations in a "
    "bigger picture and its history is clipped to a wider box. False: the first version's 3x3 blur and 5x5 "
    "wide blur at every size, its additive recurrence, its 2 a-trous iterations and its clip of 1.25 "
    "deviations, for comparing. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ao_fast_math, true, "MCLA/GPU",
    "Diagnostic: the bounce light's sectors take the angle of each sample's horizon from a polynomial (Abramowitz and "
    "Stegun's, 6.8e-5 radians at the most, 0.0014 of one of the 32 sectors) in place of the hardware's acos, which "
    "costs less; every other saving of the occlusion's and the bounce light's shader passes is the same maths to a "
    "rounding error and has no switch. False: the hardware's acos (the shader as it was in that one respect), for "
    "comparing the cost and the picture. Live; the first frame after the change makes the other pipeline on the "
    "render thread (a debug switch: a hitch of tens of milliseconds, once).")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ao_half_above_1080, true, "MCLA/GPU",
    "Diagnostic: above 1080 lines of render height (1440p, 4K, an upscaler's larger render size) the ambient "
    "occlusion's passes (depth chain, visibility, blur, history) run at half the picture's size, 720 lines for 1440p "
    "and 1080 for 4K, and the apply pass's depth-aware upsample puts the result back; 1080 lines and below are as "
    "they were. The occlusion's own 18 samples a pixel make a signal that has no more detail than that, so the picture "
    "is the same to within a thin edge and the passes cost a quarter. The contact shadows keep the picture's size "
    "(their own depth and images). False: every size at the size the quality says (the render size on ultra). With "
    "the bounce light on, mcla_gpu_ssgi_half decides instead (the bounce light's images give the visibility too). "
    "Live; the first frame after the change makes the images again.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ssgi_half, true, "MCLA/GPU",
    "Diagnostic: above 1080 lines of render height the bounce light (every quality, high and ultra) runs at half the "
    "picture's size, like mcla_gpu_ao_half_above_1080 does the occlusion, and its colour chain takes one of the "
    "colour target's MSAA samples a pixel (the four pixels of a 2x2 block give the texel) instead of all of them: the "
    "bounce light is the lowest frequency signal of the picture (it gathers 5 to 16 metres and is filtered over up to "
    "30 pixels), so the picture is the same to within a thin edge and its passes cost a quarter and the images a "
    "quarter of the memory. 1080 lines and below are as they were. False: the sizes and the chain as before "
    "(mcla_gpu_ssgi_low says the old half size choice). Live; the first frame after the change makes the images again.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_contact_shadows, "auto", "MCLA/GPU",
    "Contact shadows: the short sun shadows the game does not draw (under a car's sills, at the foot "
    "of a kerb or a wall, under props on the pavement), found from the picture's depth and put only "
    "on the sun's light, so they never darken what the game's own shadows already cover. Early in the "
    "morning and in the evening, when the game draws no sun shadows at all, they are the only ones. "
    "They are left out at night and in rain. off, high (8 steps), ultra (16 steps) or auto (the "
    "default): off on an integrated graphics card, high on a discrete one with less than 5.5 GiB of "
    "video memory, ultra on the rest. The pause menu and the launcher show the same words. A value "
    "that is none of these (the low of an older larecomp.toml) is ignored and the setting runs as "
    "auto. They are worked out at the size the game is drawn at, so a "
    "higher render scale gives sharper shadows (only high, with the ambient occlusion not on ultra, "
    "works at half that size). Costs graphics card time, ultra more than high. Also drawn behind the "
    "pause menu and on the map. Live.")
    .allowed({"auto", "off", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_length, 0.6, "MCLA/GPU",
    "Diagnostic: how far toward the sun contact shadows look, in the game's world units (about "
    "metres), with the sun high. The length grows with a low sun (0.7 / tan of its elevation, 1 to 3 "
    "times). Live.")
    .debug_only()
    .range(0.1, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_strength, 1.0, "MCLA/GPU",
    "Diagnostic: how strongly contact shadows darken the sun's light where they apply, 0 to 2 "
    "(the sun's elevation and the wetness scale it further). Live.")
    .debug_only()
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_thickness, 0.3, "MCLA/GPU",
    "Diagnostic: how far in front of a step of the ray a surface may be and still hide it, in the "
    "game's world units: a thing nearer to the camera than that only overlaps on screen and hides "
    "nothing. Live.")
    .debug_only()
    .range(0.05, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_vehicles, 1.0, "MCLA/GPU",
    "Diagnostic: how strongly contact shadows darken the ground under a car, 0 to 1. The game's sun "
    "shadow map holds only the player's car, so the ground under a traffic car is as lit as an open "
    "road; a ground pixel with a car's body above it and in the way of the sun (found from the "
    "picture's depth and the cars' pixels, seen through the gap under the car) gets the full contact "
    "shadow, as the player's car gets its sun shadow from the game. Cars the game's sun shadow map "
    "holds (the player's, and traffic with TRAFFIC SHADOWS) are left out. 0 is the contact shadows as "
    "they were (only a strip along the car's edges and its wheels). Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_fade, 0.6, "MCLA/GPU",
    "Diagnostic: how much of the contact shadows' ray a shadow stays fully dark over, 0.2 to 0.98: "
    "past that share of the ray's length it fades out, so that a shadow cut off by the ray's end "
    "does not end in a hard line. At 0.2 it looks like the first version's fade (a quarter of the "
    "shadow gone half way along the ray). Live.")
    .debug_only()
    .range(0.2, 0.98)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_contact_shadows_fast, true, "MCLA/GPU",
    "Diagnostic: the cheaper contact shadows, to compare against the plain ones (false: the passes as "
    "they were). The march takes fewer steps where its ray covers few pixels on the screen (far "
    "pixels, a smaller picture; the squared spacing is kept), and the 3x3 filter that keeps shadow "
    "edges is done inside the apply pass, which saves a pass and a full size image write (only where "
    "the contact passes run at the picture's size: ultra, or the occlusion on ultra). With that, and in a "
    "frame that has the game's sun shadow mask, the march is also left out where the game's own shadow "
    "covers a pixel and its neighbours (the apply cannot use the result there: exactly the same picture). "
    "The picture is the same to within a shadow edge moving by a pixel. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_contact_skip_stat, 0, "MCLA/GPU",
    "Diagnostic: with mcla_gpu_contact_shadows_fast and the game's sun shadow mask in the frame, every N frames (0: never) the contact march's "
    "skip is counted: a second march pass classifies every pixel (skipped under the game's own shadow, the sky or past the fade, turned from "
    "the sun, marching) with the mask's stored value at its centre, and the log gets the shares, the spread of those values (does the "
    "game's shade store exactly 0, which the skip needs?) and the model's march time for that share. That frame costs one more march and a "
    "readback of the picture. Live.")
    .debug_only()
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_mask_gain, 2.0, "MCLA/GPU",
    "Diagnostic: what the game's sun shadow mask's stored value is multiplied by to give the sun's "
    "visibility (1 lit): the game's collector stores half of it and the lit shaders multiply by 2. "
    "Look at mcla_gpu_contact_shadows_view 3 (white where lit) if shadows are darkened twice or not "
    "at all. Live.")
    .debug_only()
    .range(0.5, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_contact_shadows_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the contact shadows as a grey picture instead of applying them "
    "(only while they are on, and instead of mcla_gpu_ao_view): 1 the factor the picture is "
    "multiplied by (white: none), 2 the contact visibility (black where the ray is hidden), 3 the "
    "sun's visibility (the game's shadow mask: white lit; white everywhere where the frame has none), "
    "4 the sun's share of the light, 5 the player's car's own sun shadow map's term alone (white; black where "
    "the map shadows the pixel: the wing, the mirrors, the sills and the wheels on the car and on the road "
    "near it; white everywhere in a frame without the map: car_shadow_map.h). 0: off. Live.")
    .debug_only()
    .range(0, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_contact_shadows_car_map_march, false, "MCLA/GPU",
    "Diagnostic: with the player's car's own sun shadow map in the frame (car_shadow_map.h), the short march that "
    "the car's own pixels (and any vehicle's) take also runs on the pixels the map covers, and the pixel gets the darker "
    "of the two. False: where the map holds a part of the car at a vehicle's pixel the map is the truth about the car's "
    "own parts (a wing's plate and its posts, a mirror, a sill), which the march only guesses from the picture's depth "
    "(a thin part has no thickness there), so that pixel takes no march steps and its contact shadow is the map's. "
    "For comparing the two on the wing and the mirrors (view 5 shows the map's term). Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ssgi, "auto", "MCLA/GPU",
    "Bounce light: the light a lit wall or the ground sends back onto the road, the sides of cars and "
    "the walls beside it, found from the picture, so a red wall tints the pavement by it and a street "
    "lamp's pool lights the wall next to it. It takes the place of part of the sky light the ambient "
    "occlusion darkens, so a lit area does not get brighter overall, and a flat surface stays as it "
    "was. Needs ambient occlusion (mcla_gpu_ao) on. off, high (a coarser search: 2 directions and 2 steps "
    "each; half resolution where the occlusion is high too), ultra (3 and 3 at full resolution) or auto (the "
    "default): off on every graphics card for now, until its cost is measured. The pause menu and the "
    "launcher show the same words. A value that is none of these (the low of an older larecomp.toml) is "
    "ignored and the setting runs as auto. Costs graphics card time, "
    "ultra more than high. Also drawn behind the pause menu and in the garage, not on the map. Live.")
    .allowed({"auto", "off", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ssgi_intensity, "default", "MCLA/GPU",
    "How strong the bounce light looks: light, default, strong, stronger or strongest. Each is a set of four "
    "numbers: how much of the light the surroundings send back the picture gets, the most it may add to a pixel "
    "(a guard against noise in the dark), how far it looks for lit surfaces (5 to 16 metres, past the ambient "
    "occlusion's own reach), and the brightest light it may bring (never whiter than a white wall in full sun). "
    "The stronger ones lift shadows and the sides of cars more and look farther. Only used while the bounce light "
    "(mcla_gpu_ssgi) is on (the pause menu's BOUNCE LIGHT STRENGTH row). Live.")
    .allowed({"light", "default", "strong", "stronger", "strongest"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_strength, -1.0, "MCLA/GPU",
    "Diagnostic: replaces the level's strength of the bounce light (mcla_gpu_ssgi_intensity), 0 to 12: how much of "
    "the bounce light the picture gets (1: all that the surroundings send back by the picture's own colours; 0 "
    "runs the passes and adds nothing). -1: the level's. Live.")
    .debug_only()
    .range(-1.0, 12.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_radius, 1.0, "MCLA/GPU",
    "Diagnostic: how far the bounce light's NEAR samples look, as a multiple of the ambient occlusion's radius "
    "(mcla_gpu_ao_intensity); the same samples give the occlusion's visibility, so a change moves that too. 1: the "
    "occlusion's own. (The far ring that reaches past them is mcla_gpu_ssgi_reach.) Live.")
    .debug_only()
    .range(0.5, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_reach, -1.0, "MCLA/GPU",
    "Diagnostic: replaces the level's reach of the bounce light (mcla_gpu_ssgi_intensity), 0 to 16 metres: a "
    "second ring of samples from the occlusion's radius out to this, into what the near samples left open (the "
    "occlusion's own look does not change). 0: no ring. -1: the level's. Live.")
    .debug_only()
    .range(-1.0, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_far_steps, -1, "MCLA/GPU",
    "Diagnostic: the far ring's samples on each side of each direction, 0 to 7 (0: no ring). -1: by the bounce "
    "light's quality (2 at ultra, 1 at high). Live.")
    .debug_only()
    .range(-1, 7)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_clamp, -1.0, "MCLA/GPU",
    "Diagnostic: replaces the level's gain limit of the bounce light (mcla_gpu_ssgi_intensity), 0.05 to 10.2: the "
    "most the bounce may add to a pixel, as a multiple of that pixel's own colour (its largest channel; the hue "
    "stays; a soft limit that only guards a nearly black pixel's noise, so a shadow can gain several times "
    "its own light). The brightest the bounce can make a pixel is set by the cap, not by this. -1: the "
    "level's. Live.")
    .debug_only()
    .range(-1.0, 10.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_low, 0, "MCLA/GPU",
    "Diagnostic: what the bounce light's high does (mcla_gpu_ssgi high or auto that comes to high) to be cheaper "
    "than ultra: 0 fewer directions (2 slices and 2 steps each instead of 3 and 3) at the occlusion's own "
    "size, 1 half the picture's size (the occlusion's slices and steps) even where the occlusion is high, 2 "
    "both. The same samples give the occlusion's visibility, so 1 and 2 also make the occlusion's own "
    "picture half resolution while the bounce light is on. Live.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the bounce light's apply as a picture instead of applying it (only "
    "while the bounce light is on, and instead of the occlusion's and the contact shadows' views; the "
    "test's own views come first): 1 the factor the picture is multiplied by, in colour (the "
    "occlusion's plus the bounce), 2 the bounce each pixel gets (four times as bright as the others), "
    "3 the albedo estimate (the colour over the light the game's constants say fell on it, up to 1), "
    "4 that light, 5 how much of the colour is a surface (black: a source of light, which gets no "
    "bounce), 6 the change of the picture against the game's own (grey: none; lighter: brighter; "
    "darker). Shown at 0.3 of the value, except 6. 0: off. Live.")
    .debug_only()
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ssgi_test, false, "MCLA/GPU",
    "Diagnostic: the debug override of the bounce light (mcla_gpu_ssgi): forces its passes on, at the "
    "occlusion's own quality and whatever mcla_gpu_ssgi says (only while mcla_gpu_ao is not off). The "
    "scene colour at the occlusion's trigger is kept in a chain of images and the occlusion's main "
    "pass becomes a visibility bitmask that also finds what colour the blockers of each pixel's sky "
    "light are, then accumulated over frames and filtered. The occlusion's own look stays (its "
    "visibility now comes from the bitmask); the bounce is added to the picture as the setting adds it "
    "(mcla_gpu_ssgi_strength 0: not added), or shown by mcla_gpu_ssgi_test_view. Costs about as much "
    "as the occlusion again. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_thickness, 0.0, "MCLA/GPU",
    "Diagnostic: how thick the surfaces the bitmask finds are assumed to be, in the game's world units "
    "(about metres): a sample blocks the directions between its front and its back, the back being "
    "this far behind the front along the view ray. 0: solid, a sample blocks everything below its "
    "horizon (the occlusion's own horizon search, to within 1/32). Live.")
    .debug_only()
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_clamp, -1.0, "MCLA/GPU",
    "Diagnostic: replaces the level's cap (mcla_gpu_ssgi_intensity), 0.25 to 15.75: the brightest radiance a "
    "sample of the scene colour may carry into the bounce, and the ceiling the bounce may fill a pixel up to, as a "
    "multiple of the frame's ambient plus sun light (never under 0.5): a street lamp, neon or the sun's disc is a "
    "source, not a surface that bounces light, and may light its neighbours no whiter than a white wall in full "
    "sun. -1: the level's. Live.")
    .debug_only()
    .range(-1.0, 15.75)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_facing, 1.0, "MCLA/GPU",
    "Diagnostic: how much a sample's surface must face the pixel to light it, 0 to 1: 0 takes every "
    "surface to face the pixel, 1 weighs each by the slope of its depth along the slice (a roof seen "
    "from the ground, a wall seen from behind: nothing). Costs one more depth read per lit sector. "
    "Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the bounce light test as a picture instead of the scene colour "
    "(only while mcla_gpu_ssgi_test is on, and instead of the other debug views): 1 the bounce light "
    "the main pass found (one frame, noisy), 2 the scene colour chain's level 0 (the colour the "
    "blockers are taken from: averaged over the samples, capped), 3 its level 1, 4 its level 2, 5 the "
    "bounce light cleaned by a 5x5 mean, 6 the visibility the bitmask found (one frame, noisy: the "
    "occlusion's own view 2 shows it after the denoise and the history), 7 the bounce light after the "
    "accumulation over frames (the history), 8 the bounce light after the a-trous filter too (what the "
    "picture's bounce will be made of), 9 how much of the history each pixel kept (white: all it can, "
    "black: none, so it is new, a vehicle whose own motion is not known, or has no history yet). Shown at "
    "0.3 of its value times mcla_gpu_ssgi_test_view_gain (view 9 at full white). 0: off. Live.")
    .debug_only()
    .range(0, 9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_view_gain, 1.0, "MCLA/GPU",
    "Diagnostic: the brightness of mcla_gpu_ssgi_test_view's picture, 0.1 to 16 times. Live.")
    .debug_only()
    .range(0.1, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_history, 16, "MCLA/GPU",
    "Diagnostic: how many frames the bounce light test averages the bounce over, 2 to 64 (the "
    "occlusion's own visibility keeps mcla_gpu_ao_history). The bounce is found again in the last frame "
    "by the camera's and the vehicles' motion and kept only where it is the same surface; more frames is "
    "smoother and slower to follow a light that changed, fewer is noisier. Live.")
    .debug_only()
    .range(2, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_history_clamp, 2.0, "MCLA/GPU",
    "Diagnostic: how far the bounce's history may be from this frame's 3x3 neighbourhood of the bounce, in "
    "standard deviations of that neighbourhood (never under 5% of its mean, and 15% of the history's own value "
    "wider on each side): the history is cut to that box. Narrow follows a changed light at once and keeps more "
    "noise (the first version, 1.25 with no share of the history, made the accumulated bounce 2.5 to 3.5 times as "
    "noisy); wide keeps the history longer and leaves a ghost of a car that went by. Live.")
    .debug_only()
    .range(0.25, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_atrous, 2, "MCLA/GPU",
    "Diagnostic: the iterations of the a-trous filter that takes the noise out of the bounce light after the "
    "history, for a picture of 720 lines, 0 to 3 (0: none; each iteration reaches twice as far: 5x5, 9x9, 17x17 "
    "pixels, and stops at depth edges and corners). A bigger picture gets one more for each doubling of its "
    "height (3 at 1080 and 1440 lines, 4 at 2160, at most 5), so the filter reaches as much of the picture; "
    "mcla_gpu_ao_stable false keeps this number at every size. The history itself is never filtered. Live.")
    .debug_only()
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_census, 0, "MCLA/GPU",
    "Diagnostic: every Nth guest frame (and the first three frames with the main view), one block "
    "of log lines with the main view's draws around the game's mid-scene depth resolve, that "
    "resolve (where it is, how big, inside EndTiling or not), the light constants the occlusion "
    "uses (PS c26 ambient, PS c39 sun colour, VS c16 sun direction), the depth clear value, the "
    "lens the game's motion blur draw gives, what the history did and a statistic of the linear "
    "depth (read back from the graphics card, so the first three frames and every Nth one run the "
    "depth passes even with the occlusion off). Frames where the main view does not have exactly "
    "one qualifying resolve are logged as they come (the first 40). 0: off. Live.")
    .debug_only()
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_paint, 0.3125, "MCLA/GPU",
    "Diagnostic: how much of ambient occlusion a car's glossy paint takes. The colour of dark paint is nearly all the "
    "reflection of the sky, and a reflection is blocked only where the mirror ray is, not by the share of the sky a part "
    "cannot see, so a part that is partly covered (the uprights of a wing and the lid under it) lost most of its colour. On "
    "a car's pixels the occlusion's value (the image's, already raised to the look's power) is raised to this power: 1 "
    "darkens the paint like the world, smaller keeps more of its colour. The default, 0.3125, takes the default look's power "
    "(3.6) out again, so the paint is darkened by the share of the sky a part can see. Rounded to sixteenths, 1/16 to 1. "
    "Live.")
    .debug_only()
    .range(0.0625, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ao census]";
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                              uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);

// The registers the occlusion reads: PS c26 gLightAmbient, PS c39 gLightColor, VS c16 gLightPosDir
// (the direction the sunlight travels: at noon y is -0.867).
constexpr uint32_t kPsAmbient = 26;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsSunDirection = 16;
// VS c43: gReflectionParams, whose x is the road's wetness (0.095 dry, 1.17 in rain).
constexpr uint32_t kVsReflectionParams = 43;

// The settings the passes use that no cvar sets: the effect's falloff range as a share of the
// radius, the floor of the final visibility (no black blotches). The power of the visibility is
// part of the look (mcla_gpu_ao_intensity).
constexpr float kFalloffRatio = 0.615f;
static_assert(kFalloffRatio == ssgi_light::kFalloffRatio);  // the far ring's falloff (ssgi_light.h) is the same shape
constexpr float kMinVisibility = 0.1f;
static_assert(kMinVisibility == ao_paint::kStoredFloor);  // the paint's twin (ao_paint.h) assumes the same floor
// The frames the occlusion has to stay off before its images are given back.
constexpr uint32_t kReleaseAfterFrames = 60;
// Sizes below this (host pixels, either way) are not worth the passes.
constexpr uint32_t kMinRenderSize = 64;
// The history's flag in the push block's flags (hlsl/ao_temporal_ps.hlsl AoHistoryOn), and the flag
// of the pause/map view, whose vehicle pixels take the camera's motion like the world
// (AoVehiclesAsWorld).
constexpr uint32_t kHistoryFlag = 1u << 20;
constexpr uint32_t kVehiclesAsWorldFlag = 1u << 21;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// The vehicle table (hlsl/ao_temporal_ps.hlsl): a row an ID, four texels a row (the three columns of
// the matrix and the usable flag, hlsl/temporal_vehicle_table_ps.hlsl).
constexpr plume::RenderFormat kVehicleTableFormat = plume::RenderFormat::R32G32B32A32_FLOAT;
constexpr uint32_t kVehicleTableWidth = 4;
// The vehicle summary line is printed every this many guest frames (and once, the first time a table
// is drawn).
constexpr uint64_t kVehicleLogEvery = 3600;
// The vertex constants' view inverse is taken as the frame's own camera once it matched the
// anchor's within kVsMatch in a frame whose camera moved by at least kVsMoved (an entry of the
// view inverse: a step of that size shows in it; a stale copy of the last frame's would not match).
constexpr double kVsMatch = 5e-4;
constexpr double kVsMoved = 5e-3;
// The sky and anything the linearisation could not place (hlsl/ao_common.hlsli kAoSky).
constexpr float kSkyDistance = 60000.0f;

// The flags of the apply pass (hlsl/ao_apply_ps.hlsl): the occlusion's term, the contact shadows', the
// game's shadow mask.
constexpr uint32_t kApplyOcclusionFlag = 1u << 22;
constexpr uint32_t kApplyContactFlag = 1u << 23;
constexpr uint32_t kApplyMaskFlag = 1u << 24;
// The contact shadows' image is at the picture's size while the occlusion's images are half size (hlsl/ao_apply_ps.hlsl AoContactFull; Plan::SplitContact):
// the apply then reads it without an upsample and takes its depth from the high 16 bits of slots2.y (the low 16 are the bounce light's colour slot).
// (The bounce light's own packing: bindless slots are below 32768, so two fit one word.)
constexpr uint32_t kApplyContactFullFlag = 1u << 21;
constexpr uint32_t kApplyHighSlotShift = 16;
static_assert(kBindlessTextureCount <= (1u << kApplyHighSlotShift), "two bindless slots must fit one 32 bit word");
// The power the occlusion is raised to on a car's paint rides in the apply's flags, bits 17-20 (ao_paint.h; the denoise's radius is
// there in the base push and the apply does not read it, so the code replaces it).
// The colour chain's level 0 takes one MSAA sample of each pixel, the host sample in slots2.x (hlsl/ao_color_ps.hlsl AoOneSample; Plan::OneSampleColour).
constexpr uint32_t kColourOneSampleFlag = 1u << 21;
// The bounce light's apply (hlsl/ao_apply_ps.hlsl, AO_GI): its gain limit rides in flags bits 8-15 (the slices'
// and steps' place, which the apply does not read) and its cap in bits 26-31 (ssgi_light.h LimitCode and
// CapCode), and its debug views are 16 to 21 (mcla_gpu_ssgi_view 1 to 6). The far ring's steps ride in the main
// pass's flags, bits 26-28, and its reach in extra.w (hlsl/ao_main_ps.hlsl).
constexpr uint32_t kGiFarStepsShift = 26;
constexpr uint32_t kGiFarStepsMask = 0x7u << kGiFarStepsShift;
constexpr uint32_t kGiApplyViewBase = 15;
// The frames the bounce light's images are kept after the last frame that used them: a pause spell (the
// pause menu's and the map's world runs the plain occlusion) does not make them again.
constexpr uint32_t kGiReleaseAfterFrames = 600;
// The flag of the pass that does the contact shadows' filter (hlsl/ao_common.hlsli AoContactFilter): one 3x3
// pass whose weights also fall with the difference of the visibility. The denoise pass has it, or, with
// mcla_gpu_contact_shadows_fast, the apply pass (which then reads the march's image and has no filter pass).
constexpr uint32_t kContactFilterFlag = 1u << 25;
// The debug views of the contact shadows are 8 to 11 in the apply pass's view field (the occlusion's
// are 1 to 6).
constexpr uint32_t kContactViewBase = 7;
// mcla_gpu_contact_shadows_view 5: the car's shadow map's term alone. The contact march writes it in place of the visibility (flag
// contact_trace.h kCarMapViewFlag) and the apply shows it as its view 9, the contact visibility.
constexpr int kContactViewCarMap = 5;
// The apply's debug view of the pixels the depth pass took for vehicles (the setting's 8: the contact
// shadows' views are 8 to 11 in the shader).
constexpr uint32_t kVehicleView = 12;

// Contact shadows. The game's shadow mask: the page of the resolve that makes it (the 1280x720
// k_8_8_8_8 collector output, resolved at the main view's phase 0x4000), a texture key's base page.
constexpr uint32_t kMaskPage = 0x06ACD000u >> 12;
constexpr uint8_t kTextureFormat8888 = 6;  // xenos::TextureFormat::k_8_8_8_8
constexpr uint32_t kMaskMinWidth = 640;
// The sun's elevation gate: no contact shadows at this many degrees and below (the night's light
// points up, 19 h is at 2.5 degrees with a grey sun), all of them from kGateFullDegrees.
constexpr double kGateOffDegrees = 3.0;
constexpr double kGateFullDegrees = 12.0;
// The wetness gate (VS c43.x: 0.095 dry, 1.17 in rain): full strength up to kWetDry, none from
// kWetDry + kWetSpan.
constexpr float kWetDry = 0.2f;
constexpr float kWetSpan = 0.5f;
// The ray grows with a low sun, 0.7 / tan(elevation), kept between 1 and 3 times the setting
// (contact_trace.h LengthGrow), and its steps are contact_trace.h's too.
// The distance fade of the contact shadows, in world units: a ray of a few tenths of a metre is
// under a pixel far away.
constexpr float kContactFadeStart = 40.0f;
constexpr float kContactFadeEnd = 80.0f;

// The SSGI test (hlsl/ao_color_ps.hlsl, the bitmask pass of hlsl/ao_main_ps.hlsl): the images are RGBA16F
// (the colour chain's three, and the main pass's visibility + bounce); a sample's radiance is capped at
// a multiple of the frame's ambient + sun and never under ssgi_light.h kCapFloor, so that a night's dim
// constants (ambient 0.05) do not cap a lamp's pool to nothing.
constexpr plume::RenderFormat kGiFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr float kGiCapFloor = ssgi_light::kCapFloor;
// The bounce light's limit (bits 8-15) and cap (bits 26-31) ride in the apply's flags beside the apply's own three
// (bits 22-24) and the contact filter's bit 25: none may overlap.
static_assert(((ssgi_light::kLimitMask | ssgi_light::kCapMask) &
               (kApplyOcclusionFlag | kApplyContactFlag | kApplyMaskFlag | kContactFilterFlag)) == 0,
              "the bounce light's limit and cap bits must not overlap the apply's flags or the contact filter flag");
static_assert((ao_paint::kCodeMask & (0xFFu | ssgi_light::kLimitMask | ssgi_light::kCapMask | (1u << 16) | kApplyContactFullFlag | kApplyOcclusionFlag |
                                      kApplyContactFlag | kApplyMaskFlag | kContactFilterFlag)) == 0,
              "the paint power's code must not overlap any other flag of the apply");
// The debug pictures of mcla_gpu_ssgi_test_view, as the colour view pass numbers them (ao_color_ps.hlsl
// pass 2): the image's rgb, its yzw, its x, its yzw cleaned by a 5x5 mean.
constexpr uint32_t kGiViewRgb = 1;
constexpr uint32_t kGiViewBounce = 2;
constexpr uint32_t kGiViewGrey = 3;
constexpr uint32_t kGiViewBounceMean = 4;
// What the colour view pass multiplies a debug picture by (ao_color_ps.hlsl kViewScale): a view that is
// a weight asks for it back so that 1 is white.
constexpr float kGiViewScale = 0.3f;

float Smoothstep(double edge0, double edge1, double x) {
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return float(t * t * (3.0 - 2.0 * t));
}

// The looks of mcla_gpu_ao_intensity (the pause menu's AO INTENSITY row): the darkening's strength,
// the radius in the game's world units, the power of the visibility and the direct share (how much
// of the sun-lit part is darkened too). "default" is the setting's default.
struct LookDef {
  const char* name;
  float strength;
  float radius;
  float power;
  float direct;
};
constexpr LookDef kLooks[] = {
    {"light", 1.0f, 2.5f, 3.2f, 0.25f},
    {"default", 1.0f, 3.0f, 3.6f, 0.375f},
    {"strong", 1.0f, 3.5f, 4.0f, 0.5f},
    {"max", 1.0f, 3.75f, 4.5f, 0.75f},
};
constexpr uint32_t kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);
constexpr uint32_t kDefaultLook = 1;

// The index of mcla_gpu_ao_intensity's look; anything the list does not know is the default.
uint32_t ReadLookIndex() {
  std::string value = REXCVAR_GET(mcla_gpu_ao_intensity);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (uint32_t i = 0; i < kLookCount; ++i) {
    if (value == kLooks[i].name) {
      return i;
    }
  }
  return kDefaultLook;
}

// A matrix that can be a camera's view inverse: finite, the last column (0, 0, 0, 1), unit axes.
bool PoseSane(const temporal::Mat4& w) {
  if (!temporal::Finite(w)) {
    return false;
  }
  if (std::fabs(w.m[0][3]) > 1e-3 || std::fabs(w.m[1][3]) > 1e-3 || std::fabs(w.m[2][3]) > 1e-3 ||
      std::fabs(w.m[3][3] - 1.0) > 1e-3) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] +
                                    w.m[r][2] * w.m[r][2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  return true;
}

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

double Length3(const float v[3]) {
  return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}

std::string Vec3(const float v[3]) {
  return std::format("({:.4g}, {:.4g}, {:.4g})", v[0], v[1], v[2]);
}

std::string RefusalNames(uint32_t refused) {
  static const char* const kNames[] = {
      "not a depth copy",
      "not the main view's depth",
      "inside EndTiling",
      "not the first qualifying one of the frame",
      "after the anchor or the composite marker",
      "not into 078B0000",
      "the same call clears the depth",
      "the rectangle does not start at the origin or leaves the picture",
      "another tile of the picture the trigger covered whole",
      "the destination is smaller than the main target (a 4x view's tile texture)"};
  std::string out;
  for (uint32_t i = 0; i < 10; ++i) {
    if (refused & (1u << i)) {
      out += std::format("{}{}", out.empty() ? "" : ", ", kNames[i]);
    }
  }
  return out;
}

const char* ModeName(AmbientOcclusion::Mode mode) {
  switch (mode) {
    case AmbientOcclusion::Mode::kOff: return "off";
    case AmbientOcclusion::Mode::kHigh: return "high";
    case AmbientOcclusion::Mode::kUltra: return "ultra";
  }
  return "?";
}

// The string settings are read through LiveText: the pause menu's thread writes the cvar's string while
// this one reads it (live_text.h).
LiveText& AoSetting() {
  static LiveText text("mcla_gpu_ao");
  return text;
}

LiveText& ContactSetting() {
  static LiveText text("mcla_gpu_contact_shadows");
  return text;
}

LiveText& SsgiSetting() {
  static LiveText text("mcla_gpu_ssgi");
  return text;
}

// The bounce light's strength level (mcla_gpu_ssgi_intensity), through LiveText like the settings above: its words
// are longer than 7 characters ("stronger", "strongest"), which LiveText holds in full now (live_text.h). The
// compare key does not flip this setting (it is not in compare::kEffects): the flip of the bounce light
// (mcla_gpu_ssgi) keeps the level the player chose.
LiveText& SsgiLevelSetting() {
  static LiveText text("mcla_gpu_ssgi_intensity");
  return text;
}

ssgi_level::Level ReadSsgiLevel() {
  return ssgi_level::Resolve(SsgiLevelSetting().Get());
}

// The levels of host_plan.h are the owners' Modes, in the same order.
static_assert(uint8_t(host_plan::Level::kOff) == uint8_t(AmbientOcclusion::Mode::kOff) &&
              uint8_t(host_plan::Level::kHigh) == uint8_t(AmbientOcclusion::Mode::kHigh) &&
              uint8_t(host_plan::Level::kUltra) == uint8_t(AmbientOcclusion::Mode::kUltra));
AmbientOcclusion::Mode ToMode(host_plan::Level level) {
  return AmbientOcclusion::Mode(uint8_t(level));
}
host_plan::Level ToLevel(AmbientOcclusion::Mode mode) {
  return host_plan::Level(uint8_t(mode));
}

// mcla_gpu_ao: auto (the class's cell), off, high or ultra (anything else counts as auto). host_plan.h has the rule, which the precache
// uses too.
host_plan::Choice AoChoice() {
  return host_plan::Resolve(host_plan::Pass::kOcclusion, gpu_class::Current(), true, AoSetting().Get());
}

AmbientOcclusion::Mode ReadMode() {
  return ToMode(AoChoice().level);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return value;
}

// mcla_gpu_contact_shadows with `auto` resolved for this GPU (gpu_class.h, host_plan.h).
host_plan::Choice ContactChoice() {
  return host_plan::Resolve(host_plan::Pass::kContact, gpu_class::Current(), true,
                            ContactSetting().Get());
}

AmbientOcclusion::Mode ReadContactMode() {
  return ToMode(ContactChoice().level);
}

// mcla_gpu_ssgi with the auto value resolved for this GPU (gpu_class.h, host_plan.h): off, high or ultra. Its
// `auto` is off until the class is known (and off on every class for now). That the occlusion is on is
// asked where it is used (host_plan::Decide holds it off for the precache, BeginFrame for the draws).
host_plan::Choice BounceChoice() {
  return host_plan::Resolve(host_plan::Pass::kBounce, gpu_class::Current(), gpu_class::Known(),
                            SsgiSetting().Get());
}

AmbientOcclusion::Mode ReadSsgiMode() {
  return ToMode(BounceChoice().level);
}

// A binary16 as a float.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

// A light constant that is a usable amount (a draw whose shaders do not use the register can leave
// anything there).
float Usable(float x) {
  return std::isfinite(x) && x > 0.0f ? x : 0.0f;
}

float Luma(const float c[3]) {
  return 0.2126f * Usable(c[0]) + 0.7152f * Usable(c[1]) + 0.0722f * Usable(c[2]);
}

}  // namespace

bool AmbientOcclusion::LightRaw::operator==(const LightRaw& o) const {
  return std::memcmp(dwords, o.dwords, sizeof(dwords)) == 0;
}

void AmbientOcclusion::DecodeLight(const LightRaw& raw, float ambient[3], float sun[3],
                                   float dir[3]) {
  for (int i = 0; i < 3; ++i) {
    ambient[i] = std::bit_cast<float>(raw.dwords[i]);
    sun[i] = std::bit_cast<float>(raw.dwords[3 + i]);
    dir[i] = std::bit_cast<float>(raw.dwords[6 + i]);
  }
}

AmbientOcclusion::~AmbientOcclusion() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::ApplyVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVApplyBase + (debug ? 4u : 0u) + log2_samples;
}

uint32_t AmbientOcclusion::GiApplyVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVApplyGiBase + (debug ? 4u : 0u) + log2_samples;
}

uint32_t AmbientOcclusion::GiViewVariant(uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVGiView + log2_samples;
}

uint32_t AmbientOcclusion::MainVariant(bool fewer) {
  return ao_fast::PlainCounts(fewer) == ao_fast::Counts::kHigh ? kVMain2 : kVMain3;
}

uint32_t AmbientOcclusion::MainGiVariant(bool fewer, uint32_t far_steps, bool fast_math) {
  switch (ao_fast::GiCounts(fewer, far_steps)) {
    case ao_fast::Counts::kUltra: return fast_math ? kVMainGi3 : kVMainGi3Exact;
    case ao_fast::Counts::kHigh: return fast_math ? kVMainGi2 : kVMainGi2Exact;
    default: return kVMainGi;  // a ring of another number of steps (the debug override): the pass that reads the counts
  }
}

uint32_t AmbientOcclusion::GiPlan::FarStepsSent() const {
  return reach > 0.0f ? std::min(far_steps, ssgi_light::kFarStepsMax) : 0u;
}

std::string AmbientOcclusion::VariantName(uint32_t variant) {
  switch (variant) {
    case kVDepth: return "ao depth";
    case kVDepthMs: return "ao depth (multisampled)";
    case kVMip: return "ao depth level";
    case kVMain: return "ao visibility";
    case kVDenoise: return "ao denoise";
    case kVTemporal: return "ao temporal";
    case kVTemporalMs: return "ao temporal (multisampled)";
    case kVVehicleTable: return "ao vehicle table";
    case kVContact: return "ao contact shadows";
    case kVMain3: return "ao visibility 3x3";
    case kVMain2: return "ao visibility 2x2";
    case kVMainGi: return "ssgi visibility bitmask";
    case kVMainGi3: return "ssgi visibility bitmask 3x3+2";
    case kVMainGi2: return "ssgi visibility bitmask 2x2+1";
    case kVMainGi3Exact: return "ssgi visibility bitmask 3x3+2 (exact acos)";
    case kVMainGi2Exact: return "ssgi visibility bitmask 2x2+1 (exact acos)";
    case kVColor: return "ssgi colour";
    case kVColorMs: return "ssgi colour (multisampled)";
    case kVColorMip: return "ssgi colour level";
    case kVDenoiseGi: return "ssgi denoise";
    case kVTemporalGi: return "ssgi temporal";
    case kVTemporalGiMs: return "ssgi temporal (multisampled)";
    case kVAtrous: return "ssgi a-trous";
    default: break;
  }
  if (variant >= kVGiView && variant < kVGiView + 4) {
    return std::format("ssgi view {}x", 1u << (variant - kVGiView));
  }
  if (variant >= kVApplyGiBase) {
    const uint32_t gi_index = variant - kVApplyGiBase;
    return std::format("ssgi {} {}x", (gi_index & 4) ? "apply view" : "apply", 1u << (gi_index & 3));
  }
  const uint32_t index = variant - kVApplyBase;
  return std::format("ao {} {}x", (index & 4) ? "view" : "apply", 1u << (index & 3));
}

void AmbientOcclusion::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                  HostPasses& passes, EdramTargets& edram,
                                  PassTiming& pass_timing, ResolveCache& resolve) {
  resolve_ = &resolve;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  // The host pass layout (host_passes.h) with a push block of 128 bytes instead of 48: the four
  // texture sets, the sampler set, the push range at (b0, space4), for both stages as there.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addPushConstant(0, 4, sizeof(Push),
                          plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "ao layout");
  bool ok = layout_ != nullptr;
  // The contact march's shader (and, in ListPrecache, its pipeline) is made only where the contact shadows
  // are on at the start (not on the Low class, where `auto` is off): where they are off it would be
  // compile work and memory for nothing. A live switch to on makes the shader then (EnsureContactShader)
  // and its pipeline in the background (HoldUntilBuilt). The bounce light's shaders (the passes after
  // kCorePassCount) the same way: made below where it is on at the start (it is off on every class for
  // now), else when it is switched on (EnsureGiShaders).
  const host_plan::Choice occlusion_at_start = AoChoice();
  const host_plan::Choice contact_at_start_choice = ContactChoice();
  host_plan::Choice bounce_at_start_choice =
      host_plan::HoldBounce(BounceChoice(), occlusion_at_start.level);  // it needs the occlusion's images
  const bool contact_at_start = contact_at_start_choice.level != host_plan::Level::kOff;
  contact_shader_failed_ = false;
  listed_.fill(false);
  occlusion_built_ = false;
  contact_built_ = false;
  bounce_built_ = false;
  for (uint32_t i = 0; i < kPassCount && ok; ++i) {
    if (!CorePass(i) || (i == kPassContact && !contact_at_start)) {
      continue;
    }
    const HostShaderBlob blob = AmbientOcclusionPixelShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] ao: the layout or a shader could not be made: no ambient occlusion this "
                 "session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    layout_.reset();
    return;
  }
  // The vehicle table's rows are drawn with the temporal system's own shader (three float4 of push
  // constants as the three columns, a flag in the fourth texel) into an image of the occlusion's own.
  // Without it the vehicles keep no history (the wide blur, as before the vehicles' motion was used).
  const HostShaderBlob table = TemporalVehicleTablePixelShader();
  vehicle_table_shader_ = passes.CreateShader(table.data, table.size, table.name);
  if (!vehicle_table_shader_) {
    REXLOG_ERROR("[gpu] ao: the vehicle table's shader could not be made: the vehicles' pixels keep "
                 "no history this session (the wide blur)");
  }
  ready_ = true;
  if (bounce_at_start_choice.level != host_plan::Level::kOff && !EnsureGiShaders()) {
    bounce_at_start_choice.level = host_plan::Level::kOff;  // EnsureGiShaders said why
  }
  ListPrecache(occlusion_at_start, contact_at_start_choice, bounce_at_start_choice);
}

bool AmbientOcclusion::EnsureGiShaders() {
  if (gi_ready_) {
    return true;
  }
  if (!ready_ || gi_shaders_failed_) {
    return false;
  }
  // The bounce light's shaders (and the debug override's) are made where the bounce light is on at the
  // start, or the first time it is asked for after (a session without it holds the occlusion's alone); a
  // failure takes the bounce light only.
  bool ok = true;
  for (uint32_t i = kCorePassCount; i < kPassCount && ok; ++i) {
    if (CorePass(i)) {
      continue;  // the occlusion's own (made at the start)
    }
    const HostShaderBlob blob = AmbientOcclusionPixelShader(i);
    shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    for (uint32_t i = kCorePassCount; i < kPassCount; ++i) {
      if (!CorePass(i)) {
        shaders_[i].reset();
      }
    }
    gi_shaders_failed_ = true;
    REXLOG_ERROR("[gpu] ao: a shader of the bounce light could not be made: no bounce light this "
                 "session (the occlusion is not affected)");
    return false;
  }
  gi_ready_ = true;
  return true;
}

void AmbientOcclusion::Shutdown() {
  // The host builder's functions name this object's layout and shaders: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  ++generation_;
  probe_buffer_.reset();
  probe_busy_ = false;
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  raw_ = HostImage{};
  denoised_ = HostImage{};
  depth_full_ = HostImage{};
  contact_raw_ = HostImage{};
  contact_dn_ = HostImage{};
  for (HostImage& image : history_) {
    image = HostImage{};
  }
  weight_ = HostImage{};
  for (HostImage& image : colour_) {
    image = HostImage{};
  }
  gi_ = HostImage{};
  gi_dn_ = HostImage{};
  for (HostImage& image : gi_hist_) {
    image = HostImage{};
  }
  depth_prev_ = HostImage{};
  gi_weight_ = HostImage{};
  vehicle_table_ = HostImage{};
  history_valid_ = false;
  history_gi_ = false;
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  vehicle_table_shader_.reset();
  layout_.reset();
  ready_ = false;
  gi_ready_ = false;
  gi_shaders_failed_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  resolve_ = nullptr;
}

std::vector<uint32_t> AmbientOcclusion::VariantsFor(const host_plan::AmbientNeeds& needs) const {
  std::vector<uint32_t> variants;
  if (needs.depth) {
    variants.push_back(kVDepth);
    variants.push_back(kVDepthMs);
  }
  if (needs.mip) {
    variants.push_back(kVMip);
  }
  if (needs.occlusion) {
    // The visibility pass with its counts compiled in, for both qualities (the setting is live: a switch of the level finds the other
    // made; ao_fast.h). The compilation that reads the counts is made on first use, where a count no variant has is asked for.
    variants.push_back(kVMain3);
    variants.push_back(kVMain2);
  }
  if (needs.denoise) {
    variants.push_back(kVDenoise);
  }
  if (needs.occlusion) {
    variants.push_back(kVTemporal);
    variants.push_back(kVTemporalMs);
  }
  if (needs.apply) {
    // The apply pass for the play view's colour target (1x, 2x) and the pause/map view's (4x), so
    // that opening the pause menu with the effect on waits for no pipeline.
    variants.push_back(ApplyVariant(false, 1));
    variants.push_back(ApplyVariant(false, 2));
    variants.push_back(ApplyVariant(false, 4));
  }
  // The contact march only where its shader was made (the contact shadows are on).
  if (needs.contact && shaders_[kPassContact]) {
    variants.push_back(kVContact);
  }
  // The host's MSAA level changes while the game runs (msaa_level.h) and gives the main view's
  // colour target the new level's samples: the apply pass for 8 samples is listed whatever the
  // level is, so a switch to 8x finds it made instead of making it on the render thread in the
  // first frame at that level (the guest's own pipelines are made ahead for the new level by
  // Pipelines::PrebuildForLevel; this pass is not one of them). 2x and 4x are listed above (the
  // pause and map view's colour target has the game view's samples, or 1 without MSAA). A level
  // the device cannot do is not listed.
  if (needs.apply && edram_ && edram_->SupportedMsaaLevel(8) == 8) {
    variants.push_back(ApplyVariant(false, 8));
  }
  if (needs.occlusion && vehicle_table_shader_) {
    variants.push_back(kVVehicleTable);
  }
  // The bounce light's own passes (the colour chain, the visibility bitmask, its denoise, history and
  // a-trous filter, and the apply with the bounce), only where their shaders were made (EnsureGiShaders:
  // at the start where it is on then, or when it is switched on). The occlusion's plain passes above stay
  // needed with it: the pause menu's and the map's world, and a frame that cannot have the bounce light,
  // run the plain occlusion. The debug override's pictures (kVGiView) are never listed.
  if (needs.bounce && gi_ready_) {
    // The bitmask pass with its counts compiled in, for both qualities (the setting is live), with the acos that
    // mcla_gpu_ao_fast_math says (the other is made on first use after a switch of the cvar: a debug A/B).
    const bool fast_math = REXCVAR_GET(mcla_gpu_ao_fast_math);
    for (const uint32_t variant : {uint32_t(fast_math ? kVMainGi3 : kVMainGi3Exact),
                                   uint32_t(fast_math ? kVMainGi2 : kVMainGi2Exact), uint32_t(kVColor), uint32_t(kVColorMs),
                                   uint32_t(kVColorMip), uint32_t(kVDenoiseGi), uint32_t(kVTemporalGi),
                                   uint32_t(kVTemporalGiMs), uint32_t(kVAtrous),
                                   GiApplyVariant(false, 1), GiApplyVariant(false, 2),
                                   GiApplyVariant(false, 4)}) {
      variants.push_back(variant);
    }
    if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
      variants.push_back(GiApplyVariant(false, 8));
    }
  }
  return variants;
}

void AmbientOcclusion::ListPrecache(const host_plan::Choice& occlusion,
                                    const host_plan::Choice& contact,
                                    const host_plan::Choice& bounce) {
  HostPrecache& precache = passes_->precache();
  const gpu_class::GpuClass cls = gpu_class::Current();
  // What the settings in force at the start use: the occlusion's own groups, what the contact
  // shadows add (they share the depth, denoise and apply passes) and what the bounce light adds (its
  // own passes: it is off on every class for now, so a start lists none of them unless the player
  // switched it on). The census (a debug log) runs the depth passes with everything off. A pass that is
  // off lists nothing: it is not compiled for nothing, and a switch to on makes its pipelines in the
  // background (HoldUntilBuilt).
  const bool census = REXCVAR_GET(mcla_gpu_ao_census) != 0;
  const auto list = [&](const host_plan::AmbientNeeds& needs) {
    uint32_t added = 0;
    for (const uint32_t variant : VariantsFor(needs)) {
      listed_[variant] = true;
      added += precache.Add(HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant),
                            VariantName(variant), [this, variant] { return MakePipeline(variant); })
                   ? 1u
                   : 0u;
    }
    return added;
  };
  using host_plan::Level;
  const uint32_t for_occlusion = list(host_plan::Ambient(occlusion.level, Level::kOff, Level::kOff, census));
  const uint32_t for_contact = list(host_plan::Ambient(occlusion.level, contact.level, Level::kOff, census));
  const uint32_t for_bounce =
      list(host_plan::Ambient(occlusion.level, contact.level, bounce.level, census));
  precache.Note(host_plan::Describe(host_plan::Pass::kOcclusion, occlusion, cls), for_occlusion);
  precache.Note(host_plan::Describe(host_plan::Pass::kContact, contact, cls), for_contact);
  precache.Note(host_plan::Describe(host_plan::Pass::kBounce, bounce, cls), for_bounce);
}

std::unique_ptr<plume::RenderPipeline> AmbientOcclusion::MakePipeline(uint32_t variant) {
  const std::string name = VariantName(variant);
  const plume::RenderBlendDesc copy = plume::RenderBlendDesc::Copy();
  const auto make = [&](Pass pass, plume::RenderFormat format, const plume::RenderBlendDesc& blend,
                        uint32_t samples) -> std::unique_ptr<plume::RenderPipeline> {
    if (!shaders_[pass]) {
      return nullptr;  // the contact march's, while the contact shadows were never on (Pipeline asks first)
    }
    return passes_->CreatePipeline(*layout_, *shaders_[pass], format, blend, samples, name.c_str());
  };
  switch (variant) {
    case kVDepth: return make(kPassDepth, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVDepthMs: return make(kPassDepthMs, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVMip: return make(kPassMip, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMain: return make(kPassMain, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVDenoise: return make(kPassDenoise, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVTemporal: return make(kPassTemporal, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    case kVTemporalMs: return make(kPassTemporalMs, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    case kVContact: return make(kPassContact, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMain3: return make(kPassMain3, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMain2: return make(kPassMain2, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMainGi: return make(kPassMainGi, kGiFormat, copy, 1);
    case kVMainGi3: return make(kPassMainGi3, kGiFormat, copy, 1);
    case kVMainGi2: return make(kPassMainGi2, kGiFormat, copy, 1);
    case kVMainGi3Exact: return make(kPassMainGi3Exact, kGiFormat, copy, 1);
    case kVMainGi2Exact: return make(kPassMainGi2Exact, kGiFormat, copy, 1);
    case kVColor: return make(kPassColor, kGiFormat, copy, 1);
    case kVColorMs: return make(kPassColorMs, kGiFormat, copy, 1);
    case kVColorMip: return make(kPassColorMip, kGiFormat, copy, 1);
    case kVDenoiseGi: return make(kPassDenoiseGi, kGiFormat, copy, 1);
    case kVTemporalGi: return make(kPassTemporalGi, kGiFormat, copy, 1);
    case kVTemporalGiMs: return make(kPassTemporalGiMs, kGiFormat, copy, 1);
    case kVAtrous: return make(kPassAtrous, kGiFormat, copy, 1);
    case kVVehicleTable:
      // The host pass layout (the row's 48 bytes of push constants), as the temporal system's own table.
      return vehicle_table_shader_ ? passes_->CreatePipeline(*vehicle_table_shader_,
                                                             kVehicleTableFormat, name.c_str())
                                   : nullptr;
    default: break;
  }
  if (variant >= kVGiView && variant < kVGiView + 4) {
    // The test's debug picture: into the scene colour with the target's sample count, the rgb replaced
    // (the pass writes one colour for every sample of a pixel).
    plume::RenderBlendDesc view = copy;
    view.renderTargetWriteMask = kWriteRgb;
    return make(kPassColorView, plume::RenderFormat::R16G16B16A16_FLOAT, view,
                1u << (variant - kVGiView));
  }
  // The apply pass: into the scene colour (float16 RGBA) with the target's sample count. The
  // multiply is dst.rgb * src.rgb (the shader writes the factor); a debug view replaces the rgb.
  // Alpha is never written. The bounce light's apply (G7c) is the same pipeline with its own shader:
  // the factor is then one a colour channel, and may be above 1 (the target is a float one).
  const bool gi_apply = variant >= kVApplyGiBase;
  const uint32_t index = variant - (gi_apply ? kVApplyGiBase : kVApplyBase);
  const bool debug = (index & 4) != 0;
  const uint32_t samples = 1u << (index & 3);
  plume::RenderBlendDesc blend = copy;
  if (!debug) {
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::ZERO;
    blend.dstBlend = plume::RenderBlend::SRC_COLOR;
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  }
  blend.renderTargetWriteMask = kWriteRgb;
  if (gi_apply) {
    return make(samples > 1 ? kPassApplyGiMs : kPassApplyGi,
                plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples);
  }
  return make(samples > 1 ? kPassApplyMs : kPassApply,
              plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples);
}

bool AmbientOcclusion::EnsureContactShader() {
  if (shaders_[kPassContact]) {
    return true;
  }
  if (contact_shader_failed_ || !passes_) {
    return false;
  }
  // The contact shadows were off at the start and are on now: the shader is made here, on the render thread
  // (a short call; no worker reads the slot before HoldUntilBuilt lists the pipeline after this), and the
  // pipeline in the background by HoldUntilBuilt: the compile is not on this thread.
  const HostShaderBlob blob = AmbientOcclusionPixelShader(kPassContact);
  shaders_[kPassContact] = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (!shaders_[kPassContact]) {
    contact_shader_failed_ = true;
    REXLOG_ERROR("[gpu] ao: the contact shadows' shader could not be made: no contact shadows this session");
    return false;
  }
  REXLOG_INFO("[gpu] ao: the contact shadows were off at the start: their shader is made now, their "
              "pipeline in the background");
  return true;
}

bool AmbientOcclusion::VariantsReady(const std::vector<uint32_t>& variants) const {
  for (const uint32_t variant : variants) {
    if (pipelines_[variant] || pipeline_failed_[variant]) {
      continue;  // taken by Pipeline, or given up on (it logged why)
    }
    if (!passes_->precache().Ready(HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant))) {
      return false;
    }
  }
  return true;
}

void AmbientOcclusion::HoldUntilBuilt(Mode* occlusion, Mode* contact, Mode* bounce) {
  const bool want_occlusion = *occlusion != Mode::kOff;
  bool want_contact = *contact != Mode::kOff;
  // The bounce light reads the occlusion's images: it is not asked for while the occlusion is off.
  bool want_bounce = want_occlusion && *bounce != Mode::kOff;
  if ((!want_occlusion || occlusion_built_) && (!want_contact || contact_built_) &&
      (!want_bounce || bounce_built_)) {
    return;  // nothing is on, or everything that is on has its pipelines
  }
  if (want_contact && !EnsureContactShader()) {
    *contact = Mode::kOff;  // EnsureContactShader said why
    want_contact = false;
  }
  if (want_bounce && !EnsureGiShaders()) {
    *bounce = Mode::kOff;  // EnsureGiShaders said why
    want_bounce = false;
  }
  if (!want_occlusion && !want_contact) {
    return;
  }
  using host_plan::Level;
  const Level occlusion_level = want_occlusion ? ToLevel(*occlusion) : Level::kOff;
  const Level contact_level = want_contact ? ToLevel(*contact) : Level::kOff;
  const Level bounce_level = want_bounce ? ToLevel(*bounce) : Level::kOff;
  const bool census = census_every_ != 0;
  // What the start did not list: made on a worker of their own, in the order VariantsFor gives.
  std::vector<HostPrecache::Item> fresh;
  for (const uint32_t variant :
       VariantsFor(host_plan::Ambient(occlusion_level, contact_level, bounce_level, census))) {
    if (!listed_[variant] && !pipelines_[variant]) {  // (a debug view's pipeline may be made already)
      listed_[variant] = true;
      fresh.push_back({HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant),
                       VariantName(variant), [this, variant] { return MakePipeline(variant); }});
    }
  }
  if (!fresh.empty()) {
    // Named by what is still being made: an effect whose pipelines are done is not "switched on" again.
    std::string what;
    const auto name = [&what](bool pending, const char* effect) {
      if (pending) {
        what += what.empty() ? "" : ", ";
        what += effect;
      }
    };
    name(want_occlusion && !occlusion_built_, "ambient occlusion");
    name(want_contact && !contact_built_, "contact shadows");
    name(want_bounce && !bounce_built_, "bounce light");
    if (what.empty()) {
      what = "a graphics effect";  // a pass an earlier switch did not list (the census's, say)
    }
    const uint32_t count = passes_->precache().AddLive(what, std::move(fresh));
    REXLOG_INFO("[gpu] ao: {} switched on after the start: {} pipelines are made in the background "
                "first, and it is drawn from the frame they are done",
                what, count);
  }
  // Not drawn until every pipeline of the effect is made: a frame never waits for a compile.
  if (want_occlusion) {
    occlusion_built_ = VariantsReady(
        VariantsFor(host_plan::Ambient(occlusion_level, Level::kOff, Level::kOff, false)));
    if (!occlusion_built_) {
      *occlusion = Mode::kOff;
    }
  }
  if (want_contact) {
    contact_built_ = VariantsReady(
        VariantsFor(host_plan::Ambient(Level::kOff, contact_level, Level::kOff, false)));
    if (!contact_built_) {
      *contact = Mode::kOff;
    }
  }
  if (want_bounce) {
    // The bounce light's own passes (the occlusion's are the occlusion's own check above: it is drawn only
    // where the occlusion is).
    host_plan::AmbientNeeds own;
    own.bounce = true;
    bounce_built_ = VariantsReady(VariantsFor(own));
    if (!bounce_built_ || !occlusion_built_) {
      *bounce = Mode::kOff;
    }
  }
}

plume::RenderPipeline* AmbientOcclusion::MainGiPipeline(const GiPlan& gi) {
  plume::RenderPipeline* pipeline =
      Pipeline(MainGiVariant(gi.fewer, gi.FarStepsSent(), REXCVAR_GET(mcla_gpu_ao_fast_math)));
  // The compilation that reads the counts, if the one for them could not be made (it said why): the push block carries them.
  return pipeline ? pipeline : Pipeline(kVMainGi);
}

plume::RenderPipeline* AmbientOcclusion::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  if (variant == kVContact && !EnsureContactShader()) {
    pipeline_failed_[variant] = true;  // EnsureContactShader said why
    return nullptr;
  }
  // Made ahead when listed; a variant nobody listed (a debug view, a 4x target) is made here.
  pipelines_[variant] = passes_->precache().Acquire(
      HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant), VariantName(variant),
      [&] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("[gpu] ao: the pipeline '{}' could not be made: no {} this session in that "
                 "configuration",
                 VariantName(variant),
                 variant >= kVMainGi ? "bounce light" : "ambient occlusion");
  }
  return pipelines_[variant].get();
}

void AmbientOcclusion::BeginFrame() {
  census_every_ = uint32_t(std::max(0, REXCVAR_GET(mcla_gpu_ao_census)));
  Mode mode = ReadMode();
  if (!ready_ && mode != Mode::kOff) {
    mode = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(mode) != last_mode_logged_) {
    last_mode_logged_ = uint32_t(mode);
    const std::string setting = Lower(AoSetting().Get());
    REXLOG_INFO("[gpu] ao: ambient occlusion {}{} (mcla_gpu_ao {}, live)", ModeName(mode),
                setting == "auto" ? " (auto on this GPU)" : "", setting);
  }
  Mode contact = ReadContactMode();
  if (!ready_ || !resolve_) {
    contact = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(contact) != last_contact_logged_) {
    last_contact_logged_ = uint32_t(contact);
    const std::string setting = Lower(ContactSetting().Get());
    REXLOG_INFO("[gpu] contact shadows {}{} (mcla_gpu_contact_shadows {}, live)", ModeName(contact),
                setting == "auto" ? " (auto on this GPU)" : "", setting);
  }
  // The bounce light: the setting (its auto value resolved for this GPU) or the debug override
  // mcla_gpu_ssgi_test. It needs the occlusion's own images and passes (PlanGi says so again per frame).
  Mode gi_mode = ReadSsgiMode();
  if (!ready_) {
    gi_mode = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(gi_mode) != last_gi_logged_) {
    last_gi_logged_ = uint32_t(gi_mode);
    const std::string setting = Lower(SsgiSetting().Get());
    REXLOG_INFO("[gpu] ssgi: bounce light {}{} (mcla_gpu_ssgi {}, live; it needs mcla_gpu_ao on)",
                ModeName(gi_mode), setting == "auto" ? " (auto on this GPU)" : "", setting);
  }
  // The pipelines of what is on are made (listed at the start, or in the background since the switch);
  // until then that effect is drawn as off.
  HoldUntilBuilt(&mode, &contact, &gi_mode);
  mode_ = mode;
  contact_mode_ = contact;
  gi_mode_ = gi_mode;
  const bool test = REXCVAR_GET(mcla_gpu_ssgi_test);
  gi_active_ = (test || gi_mode_ != Mode::kOff) && mode_ != Mode::kOff && EnsureGiShaders();
  const bool gi_test = test && gi_active_;
  if (gi_test != gi_test_) {
    gi_test_ = gi_test;
    REXLOG_INFO("[gpu] ssgi: the bounce light test is {} (mcla_gpu_ssgi_test, live; it needs "
                "mcla_gpu_ao on)",
                gi_test ? "on" : "off");
  }
}

uint64_t AmbientOcclusion::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : depth_) {
    add(image);
  }
  add(raw_);
  add(denoised_);
  add(depth_full_);
  add(contact_raw_);
  add(contact_dn_);
  add(contact_skip_);
  add(contact_stat_);
  for (const HostImage& image : history_) {
    add(image);
  }
  add(weight_);
  for (const HostImage& image : colour_) {
    add(image);
  }
  add(gi_);
  add(gi_dn_);
  for (const HostImage& image : gi_hist_) {
    add(image);
  }
  add(depth_prev_);
  add(gi_weight_);
  add(vehicle_table_);
  return bytes;
}

void AmbientOcclusion::ReleaseImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(raw_);
  passes_->ReleaseImage(denoised_);
  passes_->ReleaseImage(depth_full_);
  passes_->ReleaseImage(contact_raw_);
  passes_->ReleaseImage(contact_dn_);
  passes_->ReleaseImage(contact_skip_);
  passes_->ReleaseImage(contact_stat_);
  for (HostImage& image : history_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(weight_);
  ReleaseGiImages();
  passes_->ReleaseImage(vehicle_table_);
  history_valid_ = false;
  history_cut_ = false;
  ++generation_;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks) {
  frame_had_main_view_ = true;
  if (draw.match.samples == temporal::kPauseSamples) {
    ++view4x_draws_;  // the pause menu's, the race pause's and the map's world
    pause_view_ = true;
  } else {
    play_view_ = true;
  }
  if (anchor_seen_ || composite_seen_) {
    ++draws_at_anchor_;
    return;
  }
  // The vehicle IDs the main view's draws write before the anchor (the anchor's constants hold the
  // motion of those only).
  stencil_ids_ |= draw.stencil_ids;
  if (qualifying_resolves_ != 0) {
    ++draws_after_;
    if (draw.match.depth && draw.writes_depth) {
      ++depth_writes_after_;
    }
    return;
  }
  ++draws_before_;
  if (!draw.match.color) {
    return;
  }
  ++color_draws_before_;
  // The registers the banks of this draw hold (a draw whose shaders do not use them leaves what an
  // earlier one set: the value most colour draws carry is the frame's).
  LightRaw light;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  // The wetness, for the contact shadows' gate: counted in bins of 0.1 (PickWet).
  if (contact_mode_ != Mode::kOff) {
    const float wet = BankFloat(vs, kVsReflectionParams, 0);
    if (std::isfinite(wet) && wet >= 0.0f && wet <= 4.0f) {
      ++wet_bins_[uint32_t(wet * 10.0f + 0.5f)];
      ++wet_draws_;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    light.dwords[i] = LoadBe32(ps + kPsAmbient * 16 + i * 4);
    light.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
    light.dwords[6 + i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
  }
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      vs_view_inverse_[r * 4 + c] = BankFloat(vs, 12 + r, c);
    }
  }
  ++vs_view_inverse_draws_;
  if (light_draws_ == 0) {
    first_light_ = light;
  }
  last_light_ = light;
  ++light_draws_;
  for (uint32_t i = 0; i < light_count_; ++i) {
    if (lights_[i].raw == light) {
      ++lights_[i].draws;
      return;
    }
  }
  if (light_count_ < kLightSlots) {
    lights_[light_count_].raw = light;
    lights_[light_count_].draws = 1;
    ++light_count_;
  } else {
    ++light_overflow_;
  }
}

bool AmbientOcclusion::SunDirection(double to_sun[3]) const {
  LightRaw light;
  uint32_t with_it = 0;
  if (!PickLight(&light, &with_it)) {
    return false;
  }
  float ambient[3], sun[3], dir[3];
  DecodeLight(light, ambient, sun, dir);
  const double length = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] + double(dir[2]) * dir[2]);
  if (!std::isfinite(length) || length < 0.5) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    to_sun[i] = -double(dir[i]) / length;  // c16 is the direction the light travels
  }
  return true;
}

bool AmbientOcclusion::PickLight(LightRaw* out, uint32_t* draws_with_it) const {
  if (light_draws_ == 0) {
    return false;
  }
  if (light_count_ == 0) {
    *out = last_light_;
    *draws_with_it = 0;
    return true;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < light_count_; ++i) {
    if (lights_[i].draws >= lights_[best].draws) {
      best = i;
    }
  }
  *out = lights_[best].raw;
  *draws_with_it = lights_[best].draws;
  return true;
}

void AmbientOcclusion::OnDepthClear(const EdramKey& key, float z) {
  if (temporal::MainDepthSamples(key) == 0) {
    return;
  }
  ++depth_clears_;
  depth_clear_z_ = z;
}

void AmbientOcclusion::OnAnchor(const ConstantBanks::Banks& banks, bool requested_cut) {
  anchor_seen_ = true;
  have_camera_ = false;
  vehicle_motion_.valid = 0;  // read again below, once this frame gives a camera
  lens_step_valid_ = false;
  vs_compare_valid_ = false;
  // The play view's camera and the pause/map world's (4x samples) are kept apart: the lens differs
  // (a field of view of 25 to 54 degrees), and the anchor draw runs in the 4x frames too with a
  // valid camera (918 of 918 frames in a recorded pause and map session). A frame with draws of
  // both kinds or of neither (the anchor without a main view) gives no camera; the frame after it
  // has none to use.
  if (play_view_ == pause_view_) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself here: only the temporal
  // system knows the anchor's fetches, and a frame size that is not the main view's is caught by
  // the rectangle of the trigger (ao.cpp Judge).
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    reject_ = temporal::Reject::kFrameSize;
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  temporal::Camera camera;
  reject_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_,
                                    &camera);
  if (reject_ != temporal::Reject::kNone) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    if (rejects_logged_ < 8) {
      ++rejects_logged_;
      REXLOG_INFO("[gpu] ao: the anchor's constants gave no camera: {} (frame {}, logged {} of 8)",
                  temporal::RejectName(reject_), frame_, rejects_logged_);
    }
    return;
  }
  // The lens the previous frame's camera gave the trigger, against this frame's own (the trigger
  // works with the previous frame's lens, which is this frame's outside cuts).
  lens_delta_valid_ = have_previous_ && previous_frame_ + 1 == frame_ && previous_pause_ == pause;
  if (lens_delta_valid_) {
    const auto rel = [](double a, double b) {
      return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
    };
    lens_delta_[0] = rel(camera.fov_y, previous_.fov_y);
    lens_delta_[1] = rel(camera.aspect, previous_.aspect);
    lens_delta_[2] = rel(camera.near_plane, previous_.near_plane);
    lens_delta_[3] = rel(camera.far_plane, previous_.far_plane);
    double dx = 0.0;
    double dot = 0.0;
    for (int i = 0; i < 3; ++i) {
      const double d = camera.position[i] - previous_.position[i];
      dx += d * d;
      dot += camera.forward[i] * previous_.forward[i];
    }
    lens_delta_[4] = std::sqrt(dx);
    lens_delta_[5] = dot;
    // The next frame's trigger uses this camera's lens: how fast the lens was moving is the best
    // guess of how far it is from that frame's own (a pause/map view only: see OnResolve).
    lens_step_ = std::max({lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3]});
    lens_step_valid_ = std::isfinite(lens_step_);
  }
  // The camera's motion from the frame before to this one, for the occlusion's history in the next
  // frame (FrameMotion): that frame is guessed to move the same way. A cut starts the history
  // again, and the step of a cut is not a motion to repeat.
  if (lens_delta_valid_) {
    const temporal::Cut cut =
        temporal::TestCut(camera, previous_, have_last_step_ ? last_step_ : nullptr);
    if (cut.fires || requested_cut) {
      history_valid_ = false;
      history_cut_ = true;
      have_last_step_ = false;
      last_motion_valid_ = false;
    } else {
      last_motion_ = temporal::Mul(previous_.view_inverse, camera.view);
      last_motion_valid_ = temporal::Finite(last_motion_);
      last_motion_frame_ = frame_;
      for (int i = 0; i < 3; ++i) {
        last_step_[i] = cut.step[i];
      }
      have_last_step_ = true;
    }
    // Whether the vertex constants of this frame's draws held this frame's view inverse (what the
    // census compares below): trusted once they did in a frame the camera moved in, dropped as
    // soon as they differ.
    if (vs_view_inverse_draws_ != 0) {
      temporal::Mat4 vertex;
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          vertex.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
        }
      }
      const double differs = temporal::MaxAbsDiff(vertex, camera.view_inverse);
      const double moved = temporal::MaxAbsDiff(camera.view_inverse, previous_.view_inverse);
      if (!(differs <= kVsMatch)) {
        vs_pose_trusted_ = false;
      } else if (moved >= kVsMoved) {
        vs_pose_trusted_ = true;
      }
    }
  } else {
    have_last_step_ = false;
    last_motion_valid_ = false;
    vs_pose_trusted_ = false;
  }
  // The vehicles' own motion over this frame, from the game's matrices in these constants, for the
  // next frame's history. The pause/map world has nothing that moves by itself: its vehicle pixels
  // take the camera's motion.
  if (!pause) {
    NoteVehicles(ps);
  }
  camera_ = camera;
  have_camera_ = true;
  previous_ = camera;
  previous_pause_ = pause;
  have_previous_ = true;
  previous_frame_ = frame_;

  // The main view's vertex bank against the camera just read, for the census. If c12-c15 of a
  // vertex bank are the view inverse, the frame's own lens exists before the anchor.
  if (vs_view_inverse_draws_ != 0) {
    double worst = 0.0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const double d =
            std::fabs(double(vs_view_inverse_[r * 4 + c]) - camera.view_inverse.m[r][c]);
        worst = std::isfinite(d) ? std::max(worst, d) : HUGE_VAL;
      }
    }
    vs_compare_valid_ = true;
    vs_compare_diff_ = worst;
    ++vs_compared_[pause ? 1 : 0];
    if (worst <= 1e-4) {
      ++vs_equal_[pause ? 1 : 0];
    }
  }
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::Judge(const ResolveRecord& record,
                                 const ResolveCache::Summary& summary) const {
  if (!summary.depth_copy) {
    return kRefusedNotDepthCopy;
  }
  uint32_t refused = 0;
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (samples == 0) {
    refused |= kRefusedNotMainDepth;
  }
  const bool pause = samples == temporal::kPauseSamples;
  if (record.flags & kEventInsideEndTiling) {
    refused |= kRefusedEndTiling;
  }
  // The game resolves the picture in tiles. The first is the trigger, which covers the whole
  // picture (the frame is drawn once: the depth target is complete); the others of the same
  // picture are not triggers, whatever their rectangle.
  const bool later_tile = IsLaterTile(summary);
  if (later_tile) {
    refused |= kRefusedLaterTile;
  } else if (qualifying_resolves_ != 0) {
    refused |= kRefusedNotFirst;
  }
  if (anchor_seen_ || composite_seen_) {
    refused |= kRefusedAfterAnchor;
  }
  if (summary.dest_address != kTriggerDestination) {
    refused |= kRefusedDestination;
  }
  if (summary.clears_depth) {
    refused |= kRefusedClearsDepth;
  }
  if (!later_tile) {
    uint32_t width = 0;
    uint32_t height = 0;
    PictureSize(summary, &width, &height);
    // The play view's first tile starts at the origin (the rows 0-512 of two). The pause/map
    // view's three tiles come in an order nobody has looked at: any rectangle inside the picture
    // will do for it, the occlusion covers the whole picture whichever tile triggers.
    const bool origin = summary.x0 == 0 && summary.y0 == 0;
    if ((!pause && !origin) || summary.x0 < 0 || summary.y0 < 0 || summary.x1 <= summary.x0 ||
        summary.y1 <= summary.y0 || uint32_t(summary.x1) > width || uint32_t(summary.y1) > height) {
      refused |= kRefusedRectangle;
    }
    // The pause/map view's destination must be the whole picture (1280x720 within the 1280x736
    // target), not a tile's own texture: the passes cover what the destination's size says.
    // (mcla_gpu_ao_census prints every depth copy.)
    if (pause && summary.source &&
        (width != summary.source->width ||
         height + EdramTargets::kHeightAlignment <= summary.source->height)) {
      refused |= kRefusedNotWholePicture;
    }
  }
  return refused;
}

void AmbientOcclusion::PictureSize(const ResolveCache::Summary& summary, uint32_t* width,
                                   uint32_t* height) {
  *width = summary.dest_width;
  *height = summary.dest_height;
  if (summary.source) {
    *width = std::min(*width, summary.source->width);
    *height = std::min(*height, summary.source->height);
  }
}

bool AmbientOcclusion::IsLaterTile(const ResolveCache::Summary& summary) const {
  // The same picture (source, destination texture) as the frame's first qualifying resolve, and a
  // rectangle of its own: the same rectangle again is a second copy, not a tile.
  return qualifying_resolves_ != 0 && summary.source_key == trigger_source_ &&
         summary.dest_address == trigger_destination_ && summary.dest_format == trigger_format_ &&
         summary.dest_width == trigger_size_[0] && summary.dest_height == trigger_size_[1] &&
         (summary.x0 != trigger_rect_[0] || summary.y0 != trigger_rect_[1] ||
          summary.x1 != trigger_rect_[2] || summary.y1 != trigger_rect_[3]);
}

bool AmbientOcclusion::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                                 plume::RenderCommandList* list, bool upscaler_running) {
  if (contact_mode_ != Mode::kOff || gi_active_) {
    NoteMaskResolve(record);  // the bounce light reads the sun's shadow mask too (the albedo estimate)
  }
  if (!summary.depth_copy) {
    ++other_resolves_;
    return false;
  }
  const uint32_t refused = Judge(record, summary);
  const bool qualifies = (refused & ~uint32_t(kRefusedNotFirst)) == 0;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  if (census_every_ != 0 && depth_resolves_.size() < 16) {
    DepthResolveNote note;
    note.caller = record.caller;
    note.flags = record.flags;
    note.source = summary.source_key;
    note.dest_address = summary.dest_address;
    note.dest_format = summary.dest_format;
    note.dest_width = summary.dest_width;
    note.dest_height = summary.dest_height;
    note.rect[0] = summary.x0;
    note.rect[1] = summary.y0;
    note.rect[2] = summary.x1;
    note.rect[3] = summary.y1;
    note.host_sample = summary.host_sample;
    note.host_samples = summary.source ? summary.source->host_samples : 0;
    note.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
    note.clears_depth = summary.clears_depth;
    note.refused = refused;
    note.draws_before = draws_before_ + draws_after_;
    note.anchor_before = anchor_seen_;
    depth_resolves_.push_back(note);
  }
  // A pause/map view's copy of the main depth that fails only on its destination: a build that
  // copies it elsewhere has no occlusion in that view, and the log says so once.
  if (pause && qualifying_resolves_ == 0 && refused == kRefusedDestination &&
      !pause_destination_logged_) {
    pause_destination_logged_ = true;
    REXLOG_INFO("[gpu] ao: a pause/map frame copies its main depth into {:08x} (format {}, {}x{}), "
                "not into {:08x} where the play view's mid-scene copy goes: no occlusion in that "
                "view (reported once; mcla_gpu_ao_census prints every copy)",
                summary.dest_address, summary.dest_format, summary.dest_width, summary.dest_height,
                kTriggerDestination);
  }
  if (qualifies) {
    if (qualifying_resolves_ == 0) {
      trigger_source_ = summary.source_key;
      trigger_destination_ = summary.dest_address;
      trigger_format_ = summary.dest_format;
      trigger_size_[0] = summary.dest_width;
      trigger_size_[1] = summary.dest_height;
      trigger_rect_[0] = summary.x0;
      trigger_rect_[1] = summary.y0;
      trigger_rect_[2] = summary.x1;
      trigger_rect_[3] = summary.y1;
    }
    ++qualifying_resolves_;
  } else if (refused & kRefusedLaterTile) {
    ++later_tiles_;
  }
  if (refused != 0 || !list || !ready_ || !summary.source) {
    return false;
  }

  // The trigger. The depth chain also runs, with the occlusion off, at the census's frames: its
  // statistic shows whether the depth convention holds (1 near, 0 far).
  trigger_pause_ = pause;
  // The world the trigger is in: the previous frame's camera of this kind of view is all there is (the anchor
  // comes after), and without one (the first frame of a spell) the frame has no effects anyway.
  trigger_world_ =
      pause ? menu_world::Classify(true,
                                   menu_world::LensOf(have_previous_ && previous_pause_ ? &previous_ : nullptr),
                                   menu_world::GarageLighting())
            : menu_world::World::kPlay;
  trigger_camera_valid_ = false;
  outcome_ = Outcome::kIdle;
  Plan plan;
  plan.mode = mode_;
  plan.occlusion = mode_ != Mode::kOff;
  PlanContact(&plan);
  const bool contact = plan.contact.mode != Mode::kOff;
  plan.probe = census_every_ != 0 && (probes_done_ < 3 || frame_ % census_every_ == 0);
  if (!plan.occlusion && !contact) {
    if (!plan.probe) {
      return false;
    }
    plan.mode = Mode::kHigh;  // the cheaper size is enough for a statistic
    plan.apply = false;
  }
  plan.upscaler = upscaler_running;
  plan.view = plan.occlusion ? uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_view), 0, 8)) : 0u;
  if (plan.view == 8u) {
    plan.view = kVehicleView;  // the contact shadows' views are 8 to 11
  }
  if (contact) {
    const int contact_view = std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_view), 0, kContactViewCarMap);
    if (contact_view != 0) {
      // instead of the occlusion's; 5 (the car's shadow map's term) is shown as the contact visibility, which the march then holds
      plan.contact.car_map_view = contact_view == kContactViewCarMap;
      plan.view = kContactViewBase + uint32_t(plan.contact.car_map_view ? 2 : contact_view);
    }
  }
  PlanGi(&plan);
  // (The size of the passes is decided once the picture's size is known, below: ao_size.h.)
  // The lens is the previous frame's (the camera exists at the anchor, after this), of the same
  // kind of view: none, no occlusion this frame (a cut's first frame, the first frame after
  // turning it on, the first frame of a pause spell: the play view's lens is not the pause
  // camera's).
  if (!have_previous_ || previous_frame_ + 1 != frame_ || previous_pause_ != pause) {
    ++skipped_no_lens_;
    outcome_ = Outcome::kNoCamera;
    if (pause) {
      ++pause_skipped_no_lens_;
      if (!no_lens_pause_logged_) {
        no_lens_pause_logged_ = true;
        REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame that has no previous pause/map "
                    "frame's camera (the first frame of a pause spell, a frame after one whose "
                    "anchor draw gave no camera); counted, logged once");
      }
    } else if (!no_lens_logged_) {
      no_lens_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a frame that has no previous frame's camera (the "
                  "first frame, a cut, a frame whose anchor draw gave no camera); counted, "
                  "logged once");
    }
    return false;
  }
  // A pause/map view whose lens was changing fast between the last two frames (the pause camera's
  // swing when the menu opens, the map's zoom), or whose rate of change is not known yet (the
  // second frame of a spell, the second after the setting was turned on): this frame's lens may be
  // as far from the previous one's, and a wrong lens is worse than none for a frame, so no
  // occlusion until it is known to be steady. The play view keeps its rule: its lens moves by
  // about 1e-4 a frame.
  trigger_lens_step_ = lens_step_valid_ ? lens_step_ : 0.0;
  if (pause && (!lens_step_valid_ || lens_step_ > temporal::kCutFov)) {
    ++pause_skipped_lens_;
    outcome_ = lens_step_valid_ ? Outcome::kLensChanging : Outcome::kLensUnknown;
    if (!lens_step_logged_) {
      lens_step_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame whose lens is not known to be "
                  "steady (the second frame of a spell, or a change of more than {:.0f}% between "
                  "the last two frames: the pause camera's swing, the map); counted, logged once",
                  100.0 * temporal::kCutFov);
    }
    return false;
  }
  trigger_camera_ = previous_;
  trigger_camera_valid_ = true;
  // The whole picture, not the tile this resolve copies (the game resolves it in two: the rows
  // 0-512 and 512-720), at the source's own host scale (the render scale, an upscaler's render
  // size): the target is complete now, so the occlusion covers all of it and the lens's field of
  // view spans the rows it was made for.
  uint32_t picture_width = 0;
  uint32_t picture_height = 0;
  PictureSize(summary, &picture_width, &picture_height);
  const Scale scale = summary.source->scale;
  plan.render[0] = scale.Edge(picture_width);
  plan.render[1] = scale.Edge(picture_height);
  if (plan.render[0] < kMinRenderSize || plan.render[1] < kMinRenderSize) {
    outcome_ = Outcome::kTooSmall;
    return false;
  }
  // The size of the passes (ao_size.h). The old rule: half the picture's size when nothing that is on asks for the whole of it (the
  // bounce light's ultra does, as the occlusion's and the contact shadows'). The new one, above 1080 lines of render height: the
  // occlusion's and the bounce light's images are traced at half size at every quality, and the contact shadows keep the size they had.
  {
    ao_size::Inputs in;
    in.render_height = plan.render[1];
    in.occlusion = plan.occlusion;
    in.occlusion_ultra = plan.mode == Mode::kUltra;
    in.contact = contact;
    in.contact_ultra = contact && plan.contact.mode == Mode::kUltra;
    in.bounce = plan.gi.on;
    in.bounce_ultra = plan.gi.on && plan.gi.mode == Mode::kUltra;
    in.bounce_half = plan.gi.on && plan.gi.half;
    in.half_above_ao = REXCVAR_GET(mcla_gpu_ao_half_above_1080);
    in.half_above_bounce = plan.gi.half_above;
    const ao_size::Choice choice = ao_size::Decide(in);
    plan.low = choice.low;
    plan.by_height = choice.by_height;
    plan.contact_full = choice.contact_full;
    const ao_size::Sizes sizes = ao_size::SizesOf(plan.render, choice);
    for (uint32_t axis = 0; axis < 2; ++axis) {
      plan.ao[axis] = sizes.ao[axis];
      plan.level[0][axis] = sizes.level0[axis];
      plan.level[1][axis] = sizes.level1[axis];
      plan.contact_size[axis] = sizes.contact[axis];
    }
  }
  const bool recorded = Run(summary, list, plan);
  outcome_ = recorded ? Outcome::kRecorded : Outcome::kNotRecorded;
  return recorded;
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

bool AmbientOcclusion::EnsureImages(const Plan& plan) {
  // The coarser depth levels are the occlusion's (and the census probe's); the contact shadows read
  // level 0 only. The two scratch images hold the occlusion's visibility passes and, after those,
  // the contact shadows'; the history is the occlusion's alone.
  const bool levels = plan.occlusion || plan.probe;
  const bool contact = plan.contact.mode != Mode::kOff;
  // The contact shadows have their own full size depth and images while the occlusion is traced at half size (Plan::SplitContact); else
  // the march and its filter use the occlusion's scratch images (raw_, denoised_), which the occlusion has finished with by then.
  const bool split = plan.SplitContact();
  const bool contact_scratch = plan.apply && contact && !split;
  const bool in_apply = plan.ContactFilterInApply();
  // The visibility passes' two images are read by the plain occlusion (the bounce light has its own images) and the contact shadows'
  // march and filter pass (the filter pass is not run where the apply does the filter): nothing else, so they are not held otherwise.
  const bool ao_scratch = plan.apply && plan.occlusion && !plan.gi.on;
  const bool raw_needed = ao_scratch || contact_scratch;
  const bool denoised_needed = ao_scratch || (contact_scratch && !in_apply);
  bool ok = passes_->EnsureImage(depth_[0], plan.ao[0], plan.ao[1], plume::RenderFormat::R32_FLOAT,
                                 "ao depth");
  if (ok && levels) {
    ok = passes_->EnsureImage(depth_[1], plan.level[0][0], plan.level[0][1],
                              plume::RenderFormat::R16_FLOAT, "ao depth 1/2") &&
         passes_->EnsureImage(depth_[2], plan.level[1][0], plan.level[1][1],
                              plume::RenderFormat::R16_FLOAT, "ao depth 1/4");
  } else if (!levels) {
    // Only the contact shadows run: they read level 0. What an occlusion that has been turned off left
    // (two coarser levels, about 5 MB at 4K) goes back, as the weight image does below.
    for (uint32_t i = 1; i < 3; ++i) {
      if (depth_[i].texture) {
        passes_->ReleaseImage(depth_[i]);
      }
    }
  }
  if (ok && raw_needed) {
    ok = passes_->EnsureImage(raw_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao visibility");
  } else if (!raw_needed && raw_.texture) {
    passes_->ReleaseImage(raw_);
  }
  if (ok && denoised_needed) {
    ok = passes_->EnsureImage(denoised_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao denoised visibility");
  } else if (!denoised_needed && denoised_.texture) {
    passes_->ReleaseImage(denoised_);
  }
  // The contact shadows' own images, at the picture's size.
  if (ok && split && plan.apply) {
    ok = passes_->EnsureImage(depth_full_, plan.render[0], plan.render[1], plume::RenderFormat::R32_FLOAT,
                              "ao contact depth") &&
         passes_->EnsureImage(contact_raw_, plan.render[0], plan.render[1], plume::RenderFormat::R16_FLOAT,
                              "ao contact visibility");
    if (ok && !in_apply) {
      ok = passes_->EnsureImage(contact_dn_, plan.render[0], plan.render[1], plume::RenderFormat::R16_FLOAT,
                                "ao contact filtered visibility");
    }
  }
  if (!split || !plan.apply) {
    for (HostImage* image : {&depth_full_, &contact_raw_}) {
      if (image->texture) {
        passes_->ReleaseImage(*image);
      }
    }
  }
  if ((!split || !plan.apply || in_apply) && contact_dn_.texture) {
    passes_->ReleaseImage(contact_dn_);
  }
  // The bounce light test keeps its own history (RGBA16F, EnsureGiImages) and the occlusion's is not made
  // while it runs: a frame that turns out not to have the test asks again with the test off (Run).
  if (ok && plan.occlusion && !plan.gi.on) {
    ok = passes_->EnsureImage(history_[0], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 0") &&
         passes_->EnsureImage(history_[1], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 1");
  } else if (!plan.occlusion) {
    // The history is the occlusion's alone (66 MB at 3840x2160): with the occlusion off and the contact
    // shadows on it would stay until both were off. Its content is gone with it, so the next occlusion
    // frame starts a history again (ChooseHistory: kFirst).
    for (HostImage& image : history_) {
      if (image.texture) {
        passes_->ReleaseImage(image);
      }
    }
    history_valid_ = false;
    history_cut_ = false;
  }
  // The picture of debug view 6, made only while that view is shown.
  if (ok && plan.occlusion && !plan.gi.on && plan.view == 6u) {
    ok = passes_->EnsureImage(weight_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16G16_FLOAT,
                              "ao history weight");
  } else if (weight_.texture) {
    passes_->ReleaseImage(weight_);
  }
  const uint32_t made_state = (split ? 1u : 0u) | (plan.low ? 2u : 0u);
  if (ok && (images_made_for_[0] != plan.ao[0] || images_made_for_[1] != plan.ao[1] || images_made_for_[2] != made_state)) {
    images_made_for_[0] = plan.ao[0];
    images_made_for_[1] = plan.ao[1];
    images_made_for_[2] = made_state;
    const char* what = plan.apply ? (plan.occlusion ? ModeName(plan.mode) : "contact shadows only")
                                  : "census probe only";
    const ao_size::Frame model = plan.SizeFrame();
    REXLOG_INFO("[gpu] ao: images at {}x{} for a {}x{} picture ({}{}): linear depth R32_FLOAT{}{}{}{}, "
                "{:.1f} MiB held (the model of ao_size.h: {:.1f} MiB, {:.1f} MiB at the size and with the images of the version "
                "before the height rule)",
                plan.ao[0], plan.ao[1], plan.render[0], plan.render[1], what,
                plan.by_height ? ", half size above 1080 lines: mcla_gpu_ao_half_above_1080 / mcla_gpu_ssgi_half"
                               : (plan.low ? ", half size by the quality's own rule" : ""),
                levels ? " with two coarser levels" : "",
                raw_needed ? (denoised_needed ? ", visibility R16_FLOAT x2" : ", visibility R16_FLOAT") : "",
                plan.occlusion && !plan.gi.on ? ", history R16G16_FLOAT x2" : "",
                split ? std::format(", the contact shadows keep the picture's size ({}x{}: their own depth R32_FLOAT and "
                                    "visibility R16_FLOAT{})",
                                    plan.render[0], plan.render[1], in_apply ? "" : " x2")
                      : std::string(),
                double(HeldBytes()) / (1024.0 * 1024.0), ao_size::Mib(ao_size::Bytes(model).Total()),
                ao_size::Mib(ao_size::BytesBefore(model).Total()));
    if (plan.occlusion) {
      const int radius = REXCVAR_GET(mcla_gpu_ao_stable) ? ao_stable::DenoiseRadius(plan.ao[1]) : 1;
      REXLOG_INFO("[gpu] ao: the blur is {0}x{0} pixels and the wide blur of the pixels that have no history {1}x{1} "
                  "at {2} lines ({3}: it reaches {4:.2f} of what it does at 720 lines; ao_stable.h, "
                  "mcla_gpu_ao_stable)",
                  2 * radius + 1, 2 * ao_stable::WideRadius(radius) + 1, plan.ao[1],
                  radius == 1 && !REXCVAR_GET(mcla_gpu_ao_stable) ? "the first version's, at every size"
                                                                  : "follows the image's height",
                  double(radius) * double(ao_stable::kReferenceHeight) / double(std::max(plan.ao[1], 1u)));
      if (!plan.gi.on) {
        // The cost model (ao_size.h Cost): the 5080's measured passes at 3840x2160 scaled by the pixels and the taps; the half size
        // numbers are not measured.
        const uint32_t taps = plan.mode == Mode::kHigh ? ao_fast::kHighSlices * ao_fast::kHighSteps
                                                       : ao_fast::kUltraSlices * ao_fast::kUltraSteps;
        ao_size::Choice before = plan.SizeChoice();
        before.low = plan.low && !plan.by_height;
        before.contact_full = false;
        const ao_size::Sizes sizes_before = ao_size::SizesOf(plan.render, before);
        const int radius_before = REXCVAR_GET(mcla_gpu_ao_stable) ? ao_stable::DenoiseRadius(sizes_before.ao[1]) : 1;
        const ao_size::Sizes sizes_now = ao_size::SizesOf(plan.render, plan.SizeChoice());
        const ao_size::AoCost cost_now =
            ao_size::Cost(sizes_now, plan.render, taps, uint32_t(radius), split);
        const ao_size::AoCost cost_before =
            ao_size::Cost(sizes_before, plan.render, taps, uint32_t(radius_before), false);
        REXLOG_INFO("[gpu] ao: cost model for the occlusion at {}x{} ({}): visibility {:.3f} + blur {:.3f} + history {:.3f} + "
                    "depth {:.3f} + apply {:.3f} = {:.3f} ms on an RTX 5080, {:.3f} ms at the size before the height rule "
                    "(the passes' measured 4K numbers scaled by pixels and taps; not measured at this size)",
                    plan.ao[0], plan.ao[1], plan.low ? "traced at half the picture's size" : "traced at the picture's size",
                    cost_now.main_ms, cost_now.denoise_ms, cost_now.temporal_ms, cost_now.depth_ms, cost_now.apply_ms,
                    cost_now.Total(), cost_before.Total());
      }
    }
  }
  return ok;
}

void AmbientOcclusion::Bind(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

void AmbientOcclusion::DrawPass(plume::RenderCommandList* list, HostImage& image,
                                const plume::RenderPipeline* pipeline, const Push& push) {
  // The same sequence as HostPasses::DrawInto, with this pass's own layout: 128 bytes of push
  // constants for the vertex and the pixel stage on both backends (the temporal motion pass uses
  // 80, the host passes' layout 48; Vulkan guarantees 128), and the host layout's texture sets and
  // sampler set bound again by Bind.
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    // A new colour target is initialised before it is drawn into (D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

void AmbientOcclusion::UpdateLook() {
  const uint32_t index = ReadLookIndex();
  const LookDef& def = kLooks[index];
  look_.strength = def.strength;
  look_.radius = def.radius;
  look_.power = def.power;
  look_.direct = def.direct;
  look_.car_reach = float(std::clamp(REXCVAR_GET(mcla_gpu_ao_car_reach), 0.0, 1.0));
  look_.paint_code = ao_paint::CodeFor(REXCVAR_GET(mcla_gpu_ao_paint));
  if (index != look_logged_index_) {
    look_logged_index_ = index;
    REXLOG_INFO("[gpu] ao: intensity {} (mcla_gpu_ao_intensity, live): strength {:.3g}, radius {:.3g} "
                "m, power {:.3g}, direct {:.3g}",
                def.name, look_.strength, look_.radius, look_.power, look_.direct);
  }
  if (look_.paint_code != paint_code_logged_) {
    paint_code_logged_ = look_.paint_code;
    REXLOG_INFO("[gpu] ao: a car's paint takes the occlusion raised to the power {:.4g} (mcla_gpu_ao_paint, live; 1 is the world's)",
                ao_paint::PowerOf(look_.paint_code));
  }
  if (look_.car_reach != car_reach_logged_) {
    car_reach_logged_ = look_.car_reach;
    if (ao_halo::CarReachLimits(look_.car_reach)) {
      REXLOG_INFO("[gpu] ao: a car blocks the sky light {:.3g} of the radius on the road ({:.3g} m at "
                  "this intensity; mcla_gpu_ao_car_reach, live)",
                  look_.car_reach, look_.car_reach * look_.radius);
    } else {
      REXLOG_INFO("[gpu] ao: a car blocks the sky light as far as anything else does "
                  "(mcla_gpu_ao_car_reach {:.3g})",
                  look_.car_reach);
    }
  }
}

void AmbientOcclusion::FillPush(Push* push, const ResolveCache::Summary& summary, const Plan& plan,
                                const LightRaw* light) const {
  std::memset(push, 0, sizeof(*push));
  push->slots[0] = summary.sample_slot;
  push->slots[1] = summary.host_sample;
  // The occlusion's own quality picks the slices and steps; the images are at half the picture's size
  // (the flag the shaders read) when nothing that is on asks for more (Plan::low).
  // (the bounce light's own quality when it runs: its main pass is the occlusion's)
  const bool ao_fewer = plan.gi.on ? plan.gi.fewer : plan.mode == Mode::kHigh;
  const bool low = plan.low;
  // (the counts the main pass's pipelines have compiled in: ao_fast.h, MainVariant and MainGiVariant pick the one that fits)
  const uint32_t slices = ao_fewer ? ao_fast::kHighSlices : ao_fast::kUltraSlices;
  const uint32_t steps = ao_fewer ? ao_fast::kHighSteps : ao_fast::kUltraSteps;
  // The noise changes every frame (hlsl/ao_main_ps.hlsl). The denoise's radius follows the image's height, so that
  // the blur reaches as far in the picture at 4K as at 720 lines (ao_stable.h: 1, 2 or 3); the first version's
  // 3x3 at every size is mcla_gpu_ao_stable false.
  const uint32_t denoise_radius =
      REXCVAR_GET(mcla_gpu_ao_stable) ? uint32_t(ao_stable::DenoiseRadius(plan.ao[1])) : 1u;
  push->slots2[1] = uint32_t(frame_ & 1023);
  push->slots2[2] = plan.view | (slices << 8) | (steps << 12) | ((low ? 1u : 0u) << 16) |
                    (denoise_radius << 17);

  // The lens: the previous frame's projection (row vectors, view space to clip, the camera looking
  // down -z): tan of the half field of view from its x and y scales, the depth rows for the
  // linearisation (t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored depth).
  const temporal::Mat4& p = previous_.projection;
  const float radius = look_.radius * (plan.gi.on ? plan.gi.radius : 1.0f);
  const float fade_end = float(std::clamp(REXCVAR_GET(mcla_gpu_ao_fade_end), 20.0, 1000.0));
  push->lens[0] = float(1.0 / std::max(std::fabs(p.m[0][0]), 1e-9));
  push->lens[1] = float(1.0 / std::max(std::fabs(p.m[1][1]), 1e-9));
  push->lens[2] = radius;
  push->lens[3] = kFalloffRatio;
  push->lin[0] = float(p.m[2][2]);
  push->lin[1] = float(p.m[2][3]);
  push->lin[2] = float(p.m[3][2]);
  push->lin[3] = float(p.m[3][3]);
  push->shape[0] = look_.power;
  push->shape[1] = kMinVisibility;
  push->shape[2] = look_.strength;
  push->shape[3] = 0.5f * fade_end;
  push->extra[1] = fade_end;
  push->extra[2] = look_.direct;

  // The light: the ambient and the sun as lumas, and the direction to the sun in this renderer's
  // view space (x right, y up, z forward). The game's view space looks along -z, so z changes
  // sign. c16 is the direction the light travels (at noon y is -0.867: the sun is overhead), so the
  // direction to the sun is -c16. No light constants or no direction: no sun, the whole light is
  // ambient.
  if (light) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(*light, ambient, sun, dir);
    push->light[3] = Luma(ambient);
    push->extra[0] = Luma(sun);
    double view[3] = {};
    for (int j = 0; j < 3; ++j) {
      for (int i = 0; i < 3; ++i) {
        view[j] += double(std::isfinite(dir[i]) ? dir[i] : 0.0f) * previous_.view.m[i][j];
      }
    }
    view[2] = -view[2];
    const double length = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (std::isfinite(length) && length > 1e-6) {
      for (int i = 0; i < 3; ++i) {
        push->light[i] = float(-view[i] / length);
      }
    }
  }
}

const char* AmbientOcclusion::HistoryWhyName(HistoryWhy why) {
  switch (why) {
    case HistoryWhy::kUsed: return "used";
    case HistoryWhy::kFirst:
      return "none yet (the first frame, the occlusion just turned on)";
    case HistoryWhy::kGap: return "not the frame after the one that made it";
    case HistoryWhy::kView: return "the other kind of view (play against pause/map)";
    case HistoryWhy::kSize: return "another image size";
    case HistoryWhy::kKind: return "the history of the other kind (the bounce light test switched)";
    case HistoryWhy::kCut: return "a cut of the camera";
    case HistoryWhy::kCap: return "mcla_gpu_ao_history is 1";
    case HistoryWhy::kCount: break;
  }
  return "?";
}

const char* AmbientOcclusion::PoseName(Pose pose) {
  switch (pose) {
    case Pose::kStill: return "standing still (no step measured)";
    case Pose::kPredicted: return "the last step repeated";
    case Pose::kVertexBank: return "the frame's own view inverse from the vertex constants";
  }
  return "?";
}

bool AmbientOcclusion::FrameMotion(temporal::Mat4* motion, Pose* source, bool* cut) const {
  *cut = false;
  // The frame's own camera, from the vertex constants of the draws before the trigger, once it is
  // known that they hold it (OnAnchor): the motion is exact then, as in any renderer with a
  // camera of its own. v_history = v_world * V_history, v_world = v_now * W_now.
  if (vs_pose_trusted_ && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 world;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        world.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
      }
    }
    if (PoseSane(world)) {
      *motion = temporal::Mul(world, previous_.view);
      *source = Pose::kVertexBank;
      temporal::Camera now;
      for (int i = 0; i < 3; ++i) {
        now.position[i] = world.m[3][i];
        now.forward[i] = -world.m[2][i];
      }
      now.fov_y = previous_.fov_y;
      *cut = temporal::TestCut(now, previous_, have_last_step_ ? last_step_ : nullptr).fires;
      return temporal::Finite(*motion);
    }
  }
  // The last step repeated: T took the view space of the frame before the previous one to the
  // previous one's, and the frame now is guessed to take the previous one's to its own, so what
  // moves a point of this frame's view space back into the previous one's is the inverse of T.
  if (last_motion_valid_ && last_motion_frame_ == previous_frame_) {
    temporal::Mat4 inverse;
    if (temporal::Inverse(last_motion_, &inverse)) {
      *motion = inverse;
      *source = Pose::kPredicted;
      return true;
    }
  }
  *motion = temporal::Identity();
  *source = Pose::kStill;
  return true;
}

AmbientOcclusion::HistoryUse AmbientOcclusion::ChooseHistory(const Plan& plan) const {
  HistoryUse use;
  use.cap = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_history), 1, 64));
  use.motion = temporal::Identity();
  if (use.cap < 2) {
    use.why = HistoryWhy::kCap;
  } else if (!history_valid_) {
    use.why = history_cut_ ? HistoryWhy::kCut : HistoryWhy::kFirst;
  } else if (history_frame_ + 1 != frame_) {
    use.why = HistoryWhy::kGap;
  } else if (history_pause_ != trigger_pause_) {
    use.why = HistoryWhy::kView;
  } else if (history_size_[0] != plan.ao[0] || history_size_[1] != plan.ao[1]) {
    use.why = HistoryWhy::kSize;
  } else if (history_gi_ != plan.gi.on) {
    use.why = HistoryWhy::kKind;
  } else {
    bool cut = false;
    if (!FrameMotion(&use.motion, &use.pose, &cut)) {
      use.why = HistoryWhy::kFirst;
      use.pose = Pose::kStill;
      use.motion = temporal::Identity();
    } else if (cut) {
      use.why = HistoryWhy::kCut;
    } else {
      use.on = true;
      use.why = HistoryWhy::kUsed;
      use.age = history_age_;
    }
  }
  return use;
}

void AmbientOcclusion::NoteHistory(const HistoryUse& use) {
  history_why_ = use.why;
  history_pose_ = use.pose;
  if (use.on) {
    ++history_used_;
    ++pose_frames_[size_t(use.pose)];
    if (!history_logged_) {
      history_logged_ = true;
      REXLOG_INFO("[gpu] ao: the first frame that averages over a history: {} frames at most "
                  "(mcla_gpu_ao_history), the camera's motion into the last frame from {}; one "
                  "denoise pass instead of two, the wide blur only where the history does not "
                  "hold (reported once)",
                  use.cap, PoseName(use.pose));
    }
  } else {
    ++history_resets_[size_t(use.why)];
  }
}

void AmbientOcclusion::NoteVehicles(const uint8_t* ps_bank) {
  const temporal::VehicleStats s =
      temporal::ReadVehicleMotion(ps_bank, stencil_ids_, &vehicle_motion_);
  vehicle_motion_frame_ = frame_;
  VehicleCounts& v = vehicles_;
  ++v.anchors;
  if (s.asked != 0) {
    ++v.anchors_with_ids;
    v.ids_asked += s.asked;
    v.ids_taken += s.taken;
    v.not_sane += s.not_sane;
    v.not_rigid += s.not_rigid;
    v.max_asked = std::max(v.max_asked, s.asked);
  }
  v.no_world += s.no_world ? 1 : 0;
}

uint32_t AmbientOcclusion::RecordVehicleTable(plume::RenderCommandList* list, const HistoryUse& use,
                                              uint32_t* slot) {
  *slot = kNoSlot;
  // The pause/map view's vehicles take the camera's motion (no table); a motion that is not the last
  // frame's anchor's (a frame with no camera in between) is not used.
  if (!use.on || trigger_pause_ || vehicle_motion_.valid == 0 ||
      vehicle_motion_frame_ + 1 != frame_) {
    return 0;
  }
  plume::RenderPipeline* pipeline = Pipeline(kVVehicleTable);
  if (!pipeline ||
      !passes_->EnsureImage(vehicle_table_, kVehicleTableWidth, temporal::kVehicleIds,
                            kVehicleTableFormat, "ao vehicle table")) {
    return 0;
  }
  // For a point of vehicle k, this frame's game view space into the last frame's: the camera's motion
  // `use.motion` is v_now W_now V_prev for the world; the vehicle's own step T_k takes this frame's world
  // position of the point to its position in the last frame (vehicle_motion.h), so the point goes
  // v_now W_now T_k V_prev = use.motion * W_prev * T_k * V_prev (V_prev W_prev is the identity: previous_
  // is the camera the history was made with). Then into this renderer's view space (x right, y up, z
  // forward: S M S with S = diag(1, 1, -1, 1), as the camera's rows of the push block), three columns of
  // four floats the shader takes as rows.
  // CHECK: the direction of T_k (this frame's world to the last frame's, as BuildVehicleTable uses it
  // between clip_now and clip_prev) and the repeat of last frame's step for this frame; a car that
  // drives away from its own history in debug view 6 (black on a moving car) says one of them is wrong.
  const temporal::Mat4 to_world = temporal::Mul(use.motion, previous_.view_inverse);
  struct Row {
    uint32_t id;
    float columns[12];
  };
  static_assert(sizeof(Row::columns) == 48);  // the host pass layout's push range is 64 bytes
  Row rows[temporal::kVehicleIds];
  uint32_t count = 0;
  uint32_t mask = 0;
  for (uint32_t k = 1; k < temporal::kVehicleIds; ++k) {
    if (!(vehicle_motion_.valid & (1u << k))) {
      continue;
    }
    const temporal::Mat4 m =
        temporal::Mul(temporal::Mul(to_world, vehicle_motion_.world[k]), previous_.view);
    if (!temporal::Finite(m)) {
      continue;
    }
    Row& row = rows[count++];
    row.id = k;
    for (int c = 0; c < 3; ++c) {
      for (int r = 0; r < 4; ++r) {
        const double sign = ((r == 2) != (c == 2)) ? -1.0 : 1.0;
        row.columns[c * 4 + r] = float(sign * m.m[r][c]);
      }
    }
    mask |= 1u << k;
  }
  if (count == 0) {
    return 0;
  }
  // One draw a vehicle over the four texels of its row (hlsl/temporal_vehicle_table_ps.hlsl); the
  // image is cleared first, so the row of an ID nobody drew reads as not usable.
  // CHECK: the same sequence as TemporalMotion::RecordVehicleTable (host pass layout, 4 x 1 viewports
  // and scissors, a clear every frame) in this frame's list between two of the occlusion's own passes.
  HostImage& image = vehicle_table_;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  passes_->Bind(list);
  list->setPipeline(pipeline);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = rows[i].id;
    const plume::RenderViewport viewport{0.0f, float(id), float(kVehicleTableWidth), 1.0f};
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, int32_t(id), int32_t(kVehicleTableWidth), int32_t(id + 1));
    list->setScissors(&scissor, 1);
    list->setGraphicsPushConstants(0, rows[i].columns, 0, sizeof(rows[i].columns));
    list->drawInstanced(3, 1, 0, 0);
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  *slot = image.slot;
  return mask;
}

void AmbientOcclusion::LogVehicleSummary() {
  const VehicleCounts& v = vehicles_;
  if (v.anchors_with_ids == 0 && v.frames_with_ids == 0 && v.pause_with_ids == 0) {
    return;  // no vehicle seen yet
  }
  const uint64_t seen = v.history_frames + v.pause_frames;
  if (seen == vehicles_logged_at_) {
    return;
  }
  vehicles_logged_at_ = seen;
  REXLOG_INFO("[gpu] ao: vehicle history, totals so far: of {} play frames that averaged over a history, "
              "{} had vehicle IDs in the stencil; {} of those {} IDs had a motion row ({:.1f}%), a "
              "table was drawn in {} frames ({} rows); the anchors' own constants gave a usable "
              "matrix to {} of {} IDs in {} of {} anchors (up to {} IDs an anchor; not sane {}, not "
              "rigid {}, c69 unusable in {} anchors); pause/map view: {} of {} history frames had "
              "vehicle IDs (up to {} a frame), which take the camera's motion like the world",
              v.history_frames, v.frames_with_ids, v.ids_with_row, v.ids_now,
              v.ids_now ? 100.0 * double(v.ids_with_row) / double(v.ids_now) : 0.0, v.table_frames,
              v.table_rows, v.ids_taken, v.ids_asked, v.anchors_with_ids, v.anchors, v.max_asked,
              v.not_sane, v.not_rigid, v.no_world, v.pause_with_ids, v.pause_frames,
              v.pause_max_ids);
}

bool AmbientOcclusion::Run(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                           const Plan& requested) {
  // The frame's plan, which the SSGI test may take its part out of (PrepareGi).
  Plan plan = requested;
  EdramTargets::Target& depth_target = *summary.source;
  const bool ms = depth_target.host_samples > 1;
  // Everything that can fail before anything is recorded. The coarser depth levels are the
  // occlusion's (and the census probe's); the contact shadows need the depth pass, their own march
  // and the denoise pass the occlusion uses.
  const bool levels = plan.occlusion || plan.probe;
  const bool contact = plan.contact.mode != Mode::kOff;
  plume::RenderPipeline* depth_pipeline = Pipeline(ms ? kVDepthMs : kVDepth);
  plume::RenderPipeline* mip_pipeline = levels ? Pipeline(kVMip) : nullptr;
  plume::RenderPipeline* main_pipeline = nullptr;
  plume::RenderPipeline* denoise_pipeline = nullptr;
  plume::RenderPipeline* temporal_pipeline = nullptr;
  plume::RenderPipeline* contact_pipeline = nullptr;
  if (plan.occlusion) {
    // The plain pass with the occlusion's own counts compiled in (also the frame whose bounce light cannot run, below).
    main_pipeline = Pipeline(MainVariant(plan.mode == Mode::kHigh));
    temporal_pipeline = Pipeline(ms ? kVTemporalMs : kVTemporal);
  }
  if (plan.apply) {
    denoise_pipeline = Pipeline(kVDenoise);
  }
  if (contact) {
    contact_pipeline = Pipeline(kVContact);
  }
  if (!depth_pipeline || (levels && !mip_pipeline) ||
      (plan.occlusion && (!main_pipeline || !temporal_pipeline)) ||
      (plan.apply && !denoise_pipeline) || (contact && !contact_pipeline)) {
    return false;
  }
  if (!EnsureImages(plan)) {
    if (!images_failed_logged_) {
      images_failed_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the images could not be made: no occlusion while that lasts (asked "
                   "again every frame)");
    }
    return false;
  }
  // The bounce light test: its pipelines, the main view's colour target and its images. A frame
  // that cannot have it is the plain occlusion; with the test off its images go back.
  EdramTargets::Target* gi_colour = nullptr;
  uint32_t gi_colour_slot = kNoSlot;
  if (plan.gi.on) {
    if (PrepareGi(summary, list, plan, &gi_colour, &gi_colour_slot)) {
      gi_idle_frames_ = 0;
      // The test has a history of its own: the occlusion's is given back (made again if the test goes).
      for (HostImage& image : history_) {
        if (image.texture) {
          passes_->ReleaseImage(image);
        }
      }
      if (weight_.texture) {
        passes_->ReleaseImage(weight_);
      }
    } else {
      // This frame is the plain occlusion: its history is what EnsureImages left out for the test.
      plan.gi = GiPlan{};
      if (!EnsureImages(plan)) {
        return false;
      }
    }
  } else if (++gi_idle_frames_ >= kGiReleaseAfterFrames) {
    // A frame without the bounce light (the map, a setting turned off): its images are kept for a while, so
    // that a visit to the map does not make them again.
    ReleaseGiImages();
  }
  LightRaw light;
  uint32_t with_it = 0;
  const bool have_light = PickLight(&light, &with_it);
  UpdateLook();
  Push base;
  FillPush(&base, summary, plan, have_light ? &light : nullptr);
  // The bounce's a-trous passes: the setting's number at 720 lines, one more for each doubling of the image's height
  // (ssgi_stable.h), so that the filter reaches as much of the picture at 4K as at 720 lines.
  if (plan.gi.on && REXCVAR_GET(mcla_gpu_ao_stable)) {
    plan.gi.atrous = uint32_t(ssgi_stable::AtrousIterations(int(plan.gi.atrous), plan.ao[1]));
  }

  // The depth plane is read as a texture by every pass and by the apply (after the resolve it is
  // already there: the call records a barrier only when it is elsewhere).
  edram_->Transition(list, depth_target, plume::RenderTextureLayout::SHADER_READ);

  Push push = base;
  pass_timing_->Mark(list, "ao depth");
  push.sizes[0] = float(plan.ao[0]);
  push.sizes[1] = float(plan.ao[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  // The depth target's stencil plane (the vehicles' IDs): the pass marks the vehicles' pixels.
  push.slots[2] = summary.stencil_slot;
  DrawPass(list, depth_[0], depth_pipeline, push);
  if (levels) {
    // The coarser levels.
    push.slots[2] = depth_[0].slot;
    push.sizes[0] = float(plan.level[0][0]);
    push.sizes[1] = float(plan.level[0][1]);
    push.sizes[2] = float(plan.ao[0]);
    push.sizes[3] = float(plan.ao[1]);
    DrawPass(list, depth_[1], mip_pipeline, push);
    push.slots[2] = depth_[1].slot;
    push.sizes[0] = float(plan.level[1][0]);
    push.sizes[1] = float(plan.level[1][1]);
    push.sizes[2] = float(plan.level[0][0]);
    push.sizes[3] = float(plan.level[0][1]);
    DrawPass(list, depth_[2], mip_pipeline, push);
  }
  if (plan.SplitContact()) {
    // The occlusion is traced at half size and the contact shadows keep the picture's: their march and filter read this full size
    // linear depth (the depth pass again, without the half size flag; the vehicle bit is in it as in level 0).
    pass_timing_->Mark(list, "ao contact depth");
    push = base;
    push.slots2[2] &= ~(1u << 16);
    push.slots[2] = summary.stencil_slot;
    push.sizes[0] = push.sizes[2] = float(plan.render[0]);
    push.sizes[1] = push.sizes[3] = float(plan.render[1]);
    DrawPass(list, depth_full_, depth_pipeline, push);
  }
  if (plan.gi.on) {
    RecordColourChain(list, plan, base, *gi_colour, gi_colour_slot);
  }

  // The visibility image the apply pass reads for the occlusion's term (the depth image stands in
  // while the term is off: the apply pass does not read it then).
  uint32_t ao_slot = depth_[0].slot;
  // The test's images the debug views show: the bounce's history, and the bounce after the a-trous
  // filter (the history itself when there is no filtering).
  HostImage* gi_history = nullptr;
  HostImage* gi_bounce = nullptr;
  if (plan.occlusion) {
    // The visibility from the chain.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi visibility" : "ao visibility");
    push = base;
    push.slots[2] = depth_[0].slot;
    push.slots[3] = depth_[1].slot;
    push.slots2[0] = depth_[2].slot;
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    // How far a car's samples reach on the road, as a share of the radius (extra.x, the sun's luma that the apply
    // reads and this pass does not; extra.w is the bounce light's far reach, set below).
    push.extra[0] = look_.car_reach;
    // The frame's shift of the noise: the phase of a lattice of 13 frames (ao_stable.h NoiseCode: its own slot, the
    // contact shadows' pass keeps the frame number base carries), or the first version's frame number.
    push.slots2[1] = ao_stable::NoiseCode(frame_, REXCVAR_GET(mcla_gpu_ao_stable));
    // The bounce light test's main pass is the same pass as a visibility bitmask: it reads the colour
    // chain too (slots.x, slots.y, slots2.w; the plain pass leaves them alone), the thickness and the
    // facing weight (shape.z, shape.w), and writes (visibility, bounce) into gi_, whose x channel the
    // first denoise pass below reads in place of raw_.
    HostImage* main_out = &raw_;
    if (plan.gi.on) {
      Push gi_push = push;
      gi_push.slots[0] = colour_[0].slot;
      gi_push.slots[1] = colour_[1].slot;
      gi_push.slots2[3] = colour_[2].slot;
      gi_push.shape[2] = plan.gi.thickness;
      gi_push.shape[3] = plan.gi.facing;
      // The far ring: its reach and its steps (hlsl/ao_main_ps.hlsl; none: the near samples only).
      const uint32_t far_steps = plan.gi.reach > 0.0f ? std::min(plan.gi.far_steps, ssgi_light::kFarStepsMax) : 0u;
      gi_push.extra[3] = far_steps != 0 ? plan.gi.reach : 0.0f;
      gi_push.slots2[2] = (gi_push.slots2[2] & ~kGiFarStepsMask) | ((far_steps << kGiFarStepsShift) & kGiFarStepsMask);
      // The pipeline with the slices, steps and ring steps this frame sends compiled in (FillPush sent the quality's; PrepareGi made it).
      DrawPass(list, gi_, MainGiPipeline(plan.gi), gi_push);
      main_out = &gi_;
    } else {
      DrawPass(list, raw_, main_pipeline, push);
    }
    // The history this frame can lean on, and the camera's motion into it.
    const HistoryUse use = ChooseHistory(plan);
    NoteHistory(use);
    // The history's two images take turns as the one this frame reads and the one it writes: the
    // test's (RGBA16F) or the occlusion's (RG16F).
    const uint32_t out_index = 1 - history_index_;
    HostImage* history_pair = plan.gi.on ? gi_hist_ : history_;
    HostImage& history_in = history_pair[history_index_];
    HostImage& history_out = history_pair[out_index];
    // The denoise: once with a history to average into, twice (the second pass back into the first
    // one's input image) without. It is never left out, whatever the quality or the upscaler.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi denoise" : "ao denoise");
    push.slots[2] = main_out->slot;
    push.slots[3] = depth_[0].slot;
    HostImage* visibility = nullptr;
    if (plan.gi.on) {
      // The same blur over the four channels. The test's second pass (a frame with no history) goes into
      // the history's idle image: nothing reads it this frame before the accumulation writes the other.
      plume::RenderPipeline* denoise_gi = Pipeline(kVDenoiseGi);
      DrawPass(list, gi_dn_, denoise_gi, push);
      visibility = &gi_dn_;
      if (!use.on) {
        push.slots[2] = gi_dn_.slot;
        DrawPass(list, history_in, denoise_gi, push);
        visibility = &history_in;
      }
    } else {
      DrawPass(list, denoised_, denoise_pipeline, push);
      visibility = &denoised_;
      if (!use.on) {
        push.slots[2] = denoised_.slot;
        DrawPass(list, raw_, denoise_pipeline, push);
        visibility = &raw_;
      }
    }
    // The accumulation over frames: this frame's visibility with the history's into the other
    // history image, which is the next frame's history and what the apply reads.
    // The two R16G16_FLOAT history images are render targets and shader resources in turn
    // (DrawPass's barriers and first use clear); the pass also reads the stencil plane view of the
    // depth target (Texture2D<uint4> / Texture2DMS<uint4>) in the depth target's SHADER_READ
    // layout. The census line "history:" counts the frames that used a history against the resets
    // and where the camera's motion came from.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi temporal" : "ao temporal");
    push = base;
    push.slots[2] = visibility->slot;
    push.slots[3] = depth_[0].slot;
    // Without a history the slot is any readable image: the shader does not read it.
    push.slots2[0] = use.on ? history_in.slot : depth_[0].slot;
    if (plan.gi.on) {
      // The test's accumulation: the last frame's depth is the image the depth pass wrote then (slots2.y
      // carried the frame number, which this pass does not read), the weight the bounce's history keeps
      // (its own number of frames) and the width of its clip.
      push.slots2[1] = use.on ? depth_prev_.slot : depth_[0].slot;
      push.lens[2] =
          use.on ? float(1.0 - 1.0 / double(std::min(use.age + 2, plan.gi.history))) : 0.0f;
      push.lens[3] = plan.gi.clamp_width;
      push.slots2[2] |= (ssgi_stable::ClipHistoryCode(plan.gi.clip_history) << ssgi_stable::kClipHistoryShift) &
                        ssgi_stable::kClipHistoryMask;
    }
    push.slots2[2] |= use.on ? kHistoryFlag : 0u;
    push.slots2[3] = summary.stencil_slot;
    // The vehicles' rows: the vehicle table is drawn first (the list is outside any pass here), and the
    // pause/map view's vehicle pixels take the camera's motion instead.
    // CHECK: that nothing moves by itself in the pause/map view (the cars there are still, as the
    // world is): a car that did move in a pause spell would be reprojected as still and fail the
    // plane, depth or clamp test (the wide blur, ghosts held to the clamp's margin). The census's
    // "vehicles" line and the summary line count the pause frames that had vehicle IDs.
    uint32_t vehicle_slot = kNoSlot;
    const uint32_t vehicle_rows = RecordVehicleTable(list, use, &vehicle_slot);
    push.slots[0] = vehicle_slot;
    push.slots2[2] |= trigger_pause_ ? kVehiclesAsWorldFlag : 0u;
    if (use.on) {
      // What the log says: the vehicle IDs the main view had written by now and how many had a row.
      const uint32_t ids_now = stencil_ids_ & ~1u & ((1u << temporal::kVehicleIds) - 1u);
      const uint32_t count_now = uint32_t(std::popcount(ids_now));
      VehicleCounts& v = vehicles_;
      if (trigger_pause_) {
        ++v.pause_frames;
        if (count_now != 0) {
          ++v.pause_with_ids;
          v.pause_max_ids = std::max(v.pause_max_ids, count_now);
        }
      } else {
        ++v.history_frames;
        if (count_now != 0) {
          ++v.frames_with_ids;
          v.ids_now += count_now;
          v.ids_with_row += uint32_t(std::popcount(ids_now & vehicle_rows));
        }
        if (vehicle_rows != 0) {
          ++v.table_frames;
          v.table_rows += uint32_t(std::popcount(vehicle_rows));
        }
      }
    }
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    // The motion in this renderer's view space (x right, y up, z forward): the game's looks along
    // -z, so the matrix is S G S with S = diag(1, 1, -1, 1). The shader takes the columns as rows.
    const auto motion = [&use](int i, int j) {
      const double sign = ((i == 2) != (j == 2)) ? -1.0 : 1.0;
      return float(sign * use.motion.m[i][j]);
    };
    for (int k = 0; k < 4; ++k) {
      push.lin[k] = motion(k, 0);
      push.shape[k] = motion(k, 1);
      push.light[k] = motion(k, 2);
    }
    push.extra[0] = history_tan_[0];
    push.extra[2] = history_tan_[1];
    push.extra[3] = use.on ? float(1.0 - 1.0 / double(std::min(use.age + 2, use.cap))) : 0.0f;
    // Debug view 6 shows how much history each pixel kept: the pass runs once more into an image of
    // its own, so that the history that goes on to the next frame is the normal one whatever the
    // view.
    // The test's weight picture is also what its debug view 9 shows.
    plume::RenderPipeline* accumulate =
        plan.gi.on ? Pipeline(ms ? kVTemporalGiMs : kVTemporalGi) : temporal_pipeline;
    HostImage& weight_image = plan.gi.on ? gi_weight_ : weight_;
    if (plan.view == 6u || (plan.gi.on && plan.gi.view == 9u)) {
      push.slots2[2] = (push.slots2[2] & ~0xFFu) | 6u;
      DrawPass(list, weight_image, accumulate, push);
    }
    push.slots2[2] &= ~0xFFu;  // the frame's own run writes no debug picture
    DrawPass(list, history_out, accumulate, push);
    if (plan.gi.on) {
      // The a-trous filter of the bounce over the history, into the two images nothing needs now (the
      // denoised image and the history's idle one), each iteration reading the last one's output: the
      // history itself is never filtered.
      gi_history = &history_out;
      gi_bounce = &history_out;
      if (plan.gi.atrous != 0) {
        pass_timing_->Mark(list, "ssgi a-trous");
        plume::RenderPipeline* atrous = Pipeline(kVAtrous);
        HostImage* scratch[2] = {&gi_dn_, &history_in};
        for (uint32_t i = 0; i < plan.gi.atrous; ++i) {
          Push filter = base;
          filter.slots2[2] &= ~0xFFu;
          filter.slots[2] = gi_bounce->slot;
          filter.slots[3] = depth_[0].slot;
          filter.slots2[0] = 1u << i;  // the taps' distance in pixels
          filter.sizes[0] = filter.sizes[2] = float(plan.ao[0]);
          filter.sizes[1] = filter.sizes[3] = float(plan.ao[1]);
          HostImage& target = *scratch[i & 1];
          DrawPass(list, target, atrous, filter);
          gi_bounce = &target;
        }
      }
      if (use.on) {
        ++gi_history_runs_;
      }
    }
    history_index_ = out_index;
    history_valid_ = true;
    history_cut_ = false;
    history_frame_ = frame_;
    history_pause_ = trigger_pause_;
    history_size_[0] = plan.ao[0];
    history_size_[1] = plan.ao[1];
    history_tan_[0] = base.lens[0];
    history_tan_[1] = base.lens[1];
    // The test's bounce averages over more frames than the visibility does: its age runs on to its cap.
    history_age_ =
        use.on ? std::min(use.age + 1, plan.gi.on ? std::max(use.cap, plan.gi.history) : use.cap)
               : 0u;
    history_gi_ = plan.gi.on;
    ao_slot = (plan.view == 6u ? weight_image : history_out).slot;
  }
  // The contact shadows: their march and its filter, into the two scratch images the occlusion has
  // finished with.
  uint32_t contact_slot = kNoSlot;
  uint32_t mask_slot = kNoSlot;
  if (contact) {
    // The game's sun shadow mask is found first: with mcla_gpu_contact_shadows_fast the march leaves out the pixels
    // under it that the apply cannot use (RecordContact). The apply gets the very same slot.
    const uint32_t contact_mask = FindMaskSlot();
    contact_slot = RecordContact(list, plan, base, contact_mask);
    if (contact_slot != kNoSlot) {
      mask_slot = contact_mask;
    }
  }
  if (mask_slot == kNoSlot && plan.gi.apply) {
    mask_slot = FindMaskSlot();  // the bounce light's albedo estimate takes the sun's shadow into account
  }
  const bool contact_applied = contact_slot != kNoSlot;
  bool applied = false;
  if (plan.apply) {
    // The apply into the scene colour: the occlusion's term and the contact shadows' in one factor.
    push = base;
    push.slots[2] = ao_slot;
    push.slots[3] = depth_[0].slot;
    push.sizes[0] = float(plan.render[0]);
    push.sizes[1] = float(plan.render[1]);
    push.sizes[2] = float(plan.ao[0]);
    push.sizes[3] = float(plan.ao[1]);
    // The paint power's code (ao_paint.h, hlsl/ao_apply_ps.hlsl AoPaintPower), over the denoise's radius bits which this pass does not read.
    push.slots2[2] = (push.slots2[2] & ~ao_paint::kCodeMask) | (look_.paint_code << ao_paint::kCodeShift);
    // The contact filter's flag (bit 25) tells the apply that slots2.x is the march's own image and that it does the
    // 3x3 filter itself (mcla_gpu_contact_shadows_fast); without it the image is the denoise pass's output.
    push.slots2[2] |= (plan.occlusion ? kApplyOcclusionFlag : 0u) |
                      (contact_applied ? kApplyContactFlag : 0u) |
                      (contact_applied && plan.ContactFilterInApply() ? kContactFilterFlag : 0u) |
                      (mask_slot != kNoSlot ? kApplyMaskFlag : 0u);
    push.slots2[0] = contact_applied ? contact_slot : 0u;
    push.slots2[3] = mask_slot != kNoSlot ? mask_slot : 0u;
    // slots2.y: the bounce light's colour slot (low 16 bits, below) and, while the contact shadows keep the picture's size beside a half
    // size occlusion, the slot of their full size depth (high 16 bits); the apply reads neither otherwise (it was the frame number).
    push.slots2[1] = 0u;
    if (contact_applied && plan.SplitContact()) {
      push.slots2[2] |= kApplyContactFullFlag;
      push.slots2[1] = depth_full_.slot << kApplyHighSlotShift;
    }
    push.lens[2] = plan.contact.fade_start;
    push.lens[3] = plan.contact.fade_end;
    push.shape[0] = plan.contact.mask_gain;
    push.extra[3] = plan.contact.strength;
    if (plan.gi.apply && gi_bounce) {
      // The bounce light's apply (hlsl/ao_apply_ps.hlsl, AO_GI): the filtered image holds the visibility
      // (x, as the history kept it) and the bounce, the colour chain's level 0 the pixel's own colour (and
      // whether it is a source of light); the strength rides in shape.y, the gain limit in the flags' bits 8-15 and
      // the cap (the ceiling) in bits 26-31.
      push.slots[2] = gi_bounce->slot;
      push.slots2[1] |= colour_[0].slot;
      push.shape[1] = plan.gi.strength;
      push.slots2[2] = (push.slots2[2] & ~(ssgi_light::kLimitMask | ssgi_light::kCapMask)) |
                       ((ssgi_light::LimitCode(plan.gi.limit) << ssgi_light::kLimitShift) & ssgi_light::kLimitMask) |
                       ((ssgi_light::CapCode(plan.gi.cap) << ssgi_light::kCapShift) & ssgi_light::kCapMask);
      if (plan.gi.apply_view != 0) {
        push.slots2[2] = (push.slots2[2] & ~0xFFu) | (kGiApplyViewBase + plan.gi.apply_view);
      }
    }
    if (plan.gi.on && plan.gi.view != 0) {
      // The test's debug picture in place of the apply: an image of the test over the colour target
      // (hlsl/ao_color_ps.hlsl pass 2): 1 the bounce, 2-4 the chain's levels, 5 the bounce cleaned,
      // 6 the visibility.
      const uint32_t v = plan.gi.view;
      const HostImage& shown = (v >= 2 && v <= 4) ? colour_[v - 2]
                               : v == 7           ? *gi_history
                               : v == 8           ? *gi_bounce
                               : v == 9           ? gi_weight_
                                                  : gi_;
      push = base;
      push.slots[2] = shown.slot;
      push.sizes[0] = float(plan.render[0]);
      push.sizes[1] = float(plan.render[1]);
      push.sizes[2] = float(shown.width);
      push.sizes[3] = float(shown.height);
      push.slots2[2] = (v == 1 || v == 7 || v == 8) ? kGiViewBounce
                       : v == 5                      ? kGiViewBounceMean
                       : (v == 6 || v == 9)          ? kGiViewGrey
                                                     : kGiViewRgb;
      // The weight is shown at full white for 1 (the other pictures at 0.3 of their value).
      push.shape[0] = plan.gi.view_gain * (v == 9 ? 1.0f / kGiViewScale : 1.0f);
    }
    applied = RecordApply(list, summary, plan, push);
    applied_this_frame_ = applied;
  }
  if (plan.gi.on) {
    if (gi_runs_ == 0) {
      REXLOG_INFO("[gpu] ssgi: the first frame of the bounce light test: the colour chain {}x{} and "
                  "two coarser levels (RGBA16F) from the {}-sample main colour target, the "
                  "visibility bitmask in place of the GTAO horizons ({}), thickness {:.2f} ({}), a "
                  "sample's radiance capped at {:.2f} x (ambient + sun) and at least {:.2f}, facing "
                  "weight {:.2f}, debug view {}, {:.1f} MiB held by all the occlusion's images",
                  plan.ao[0], plan.ao[1], depth_target.host_samples, ModeName(plan.mode),
                  plan.gi.thickness, plan.gi.thickness > 0.0f ? "a slab" : "a solid", plan.gi.cap,
                  kGiCapFloor, plan.gi.facing, plan.gi.view, double(HeldBytes()) / (1024.0 * 1024.0));
      REXLOG_INFO("[gpu] ssgi: the bounce has its own history and denoise: the occlusion's 3x3 blur over "
                  "the four channels, an accumulation over frames (RGBA16F history, the last frame's depth "
                  "in an R32_FLOAT image of its own; the same camera motion, vehicle rows and tests as the "
                  "occlusion's history; the bounce over {} frames at most, its history clipped to {:.2f} "
                  "deviations of this frame's 3x3 neighbourhood, {:.0f}% of its own value wider on each side) "
                  "and {} a-trous iteration(s) of the history's copy at {} lines (the picture's bounce comes from "
                  "it; the history is never filtered; ssgi_stable.h, mcla_gpu_ao_stable). "
                  "The occlusion's own RG16F history is not made while the test is on; the visibility "
                  "comes from the x channel of the new history (reported once)",
                  plan.gi.history, plan.gi.clamp_width, 100.0 * double(plan.gi.clip_history), plan.gi.atrous,
                  plan.ao[1]);
    }
    if (plan.gi.apply && !gi_apply_logged_) {
      gi_apply_logged_ = true;
      REXLOG_INFO("[gpu] ssgi: the bounce light is added to the picture ({} from {}): {} slices and {} steps "
                  "at {}x{} for the {}x{} picture, plus {} far ring step(s) a side out to {:.1f} m, strength "
                  "{:.2f}, gain limit {:.2f}, cap {:.2f} x the frame's light, near search radius x{:.2f} of "
                  "the occlusion's; the sun's shadow mask {} (without it the albedo estimate takes the sun as "
                  "unshadowed); the pause menu's and the map's world runs the plain occlusion",
                  ModeName(plan.gi.mode), plan.gi.test ? "the debug override mcla_gpu_ssgi_test" : "mcla_gpu_ssgi",
                  plan.gi.fewer ? 2 : 3, plan.gi.fewer ? 2 : 3, plan.ao[0],
                  plan.ao[1], plan.render[0], plan.render[1], plan.gi.reach > 0.0f ? plan.gi.far_steps : 0u,
                  plan.gi.reach, plan.gi.strength, plan.gi.limit, plan.gi.cap, plan.gi.radius,
                  mask_slot != kNoSlot ? "found" : "not found in this frame");
    }
    ++gi_runs_;
  }
  if (contact_applied) {
    contact_mask_ = mask_slot != kNoSlot;
    if (contact_runs_ == 0) {
      REXLOG_INFO("[gpu] contact shadows: the first frame: {} at {}x{} (images {}x{}), {} steps over "
                  "{:.2f} m (thickness {:.2f} m, full over the first {:.0f}% of the ray), strength "
                  "{:.2f}, the game's shadow mask {} (the sun's visibility is 1 where it is not "
                  "found), {}",
                  ModeName(plan.contact.mode), plan.render[0], plan.render[1], plan.contact_size[0],
                  plan.contact_size[1], plan.contact.steps, plan.contact.length, plan.contact.thickness,
                  100.0f * plan.contact.end_fade_start, plan.contact.strength,
                  contact_mask_ ? "found" : "not found",
                  plan.occlusion ? "with the occlusion" : "without the occlusion");
      const contact_trace::Vec3 up = contact_trace::UpInView(previous_.view.m);
      REXLOG_INFO("[gpu] contact shadows: the ground under a car {} (strength {:.2f}, "
                  "mcla_gpu_contact_shadows_vehicles; the world's up in view space {:+.2f} {:+.2f} {:+.2f}, "
                  "the cars' pixels from the depth pass's vehicle bit; it stands aside for the player's car and "
                  "for the cars TRAFFIC SHADOWS draws into the sun shadow map, within {:.1f} m of the camera "
                  "now)",
                  plan.contact.under_vehicle > 0.0f && contact_trace::Dot(up, up) > 0.5f ? "is shadowed"
                                                                                           : "is as it was",
                  plan.contact.under_vehicle, up.x, up.y, up.z,
                  contact_trace::UnderCastDistance(traffic_shadow::CastDistance()));
    }
    const uint32_t cost_mode = (plan.contact.fast ? 1u : 0u) | (plan.ContactFilterInApply() ? 2u : 0u);
    if (contact_cost_logged_[0] != plan.contact_size[0] || contact_cost_logged_[1] != plan.contact_size[1] ||
        contact_cost_logged_[2] != plan.contact.steps || contact_cost_logged_[3] != cost_mode) {
      // The cost model (contact_trace.h CostModel), once per size, step count and lever state: the march
      // reads the depth `steps` times a pixel of the images, the filter is one 3x3 pass. The numbers are the
      // RTX 5080's measured at 3840x2160 and scale with the pixels and the steps; the first version's two
      // filter passes are shown for comparison, and with mcla_gpu_contact_shadows_fast on the model of the
      // cheaper path (not measured) next to the plain one.
      contact_cost_logged_[0] = plan.contact_size[0];
      contact_cost_logged_[1] = plan.contact_size[1];
      contact_cost_logged_[2] = plan.contact.steps;
      contact_cost_logged_[3] = cost_mode;
      const contact_trace::Cost plain =
          contact_trace::CostModel(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, 1, true);
      const contact_trace::Cost before =
          contact_trace::CostModel(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, 2, false);
      REXLOG_INFO("[gpu] contact shadows: cost model for {}x{} ({:.2f} megapixels, {}): the march "
                  "reads the depth {:.1f} million times ({:.3f} ms) and one filter pass runs over "
                  "all of it ({:.3f} ms), {:.3f} ms in all on an RTX 5080 (the first version, two "
                  "plain filter passes: {:.3f} ms); the images are the render size: a higher "
                  "render scale traces more pixels, so the edges stay as sharp in pixels",
                  plan.contact_size[0], plan.contact_size[1], plain.megapixels,
                  plan.contact_size[0] < plan.render[0] ? "half the render size"
                                                        : (plan.low ? "the render size, beside an occlusion traced at half size"
                                                                    : "the render size"),
                  plain.mega_steps, plain.march_ms, plain.filter_ms, plain.total_ms, before.total_ms);
      if (plan.contact.fast) {
        const contact_trace::Cost fast = contact_trace::CostModelFast(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, true);
        const bool adaptive = contact_trace::AdaptiveOnAt(plan.contact_size[1]);
        REXLOG_INFO("[gpu] contact shadows: the cheaper path is on (mcla_gpu_contact_shadows_fast): {}, {}: model {:.3f} ms in all ({:.3f} + {:.3f}) "
                    "against {:.3f} ms with it off; not measured",
                    adaptive ? std::format("the march keeps {:.0f}% of the steps on average (a ray that is short on the screen needs fewer; the "
                                           "model's street average for this size)",
                                           100.0 * contact_trace::AdaptiveStepShare(plan.contact_size[1]))
                             : std::string("the march keeps every step (the adaptive count is used up to 1080 lines of picture)"),
                    plan.ContactFilterInApply() ? "the filter is done inside the apply pass (no pass, no target write)"
                                                : "the filter pass stays (the passes run at half the picture's size)",
                    fast.total_ms, fast.march_ms, fast.filter_ms, plain.total_ms);
        if (plan.ContactFilterInApply()) {
          // The skip under the game's own shadow (a frame that has the mask; the model's shadowed shares, not measured).
          const contact_trace::Cost lit =
              contact_trace::CostModelFast(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, true, 0.0);
          const contact_trace::Cost third =
              contact_trace::CostModelFast(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, true, 0.3);
          const contact_trace::Cost half =
              contact_trace::CostModelFast(plan.contact_size[0], plan.contact_size[1], plan.contact.steps, true, 0.5);
          REXLOG_INFO("[gpu] contact shadows: with the game's sun shadow mask in the frame the march also skips the "
                      "pixels the game's own shadow covers (the picture is unchanged): model {:.3f} ms of march + {:.3f} ms of block pass with none of "
                      "the pixels in shadow, {:.3f} ms at 30% and {:.3f} ms at 50% in shadow (a frame without "
                      "the mask: {:.3f} ms; the plain march {:.3f} ms); only if the game's shade stores exactly 0 "
                      "(mcla_gpu_contact_skip_stat says); not measured",
                      lit.march_ms, lit.block_ms, third.march_ms, half.march_ms, fast.march_ms, plain.march_ms);
        }
      }
    }
    ++contact_runs_;
    ++(contact_mask_ ? contact_mask_frames_ : contact_no_mask_frames_);
  }
  if (plan.probe) {
    RecordProbe(list, plan);
  }
  if (plan.gi.on) {
    // The depth image of this frame is the next frame's depth history: the two trade places now that
    // nothing of this frame reads depth_[0] again (the next frame's depth pass writes the other).
    std::swap(depth_[0], depth_prev_);
  }
  // This frame's linear depth, for the passes that read it after these (FrameLinearDepth): with the bounce light's swap above it is the
  // history image now.
  linear_depth_image_ = plan.gi.on ? &depth_prev_ : &depth_[0];
  linear_depth_tan_[0] = base.lens[0];
  linear_depth_tan_[1] = base.lens[1];
  ran_this_frame_ = true;
  if (plan.occlusion && applied) {
    if (runs_ == 0) {
      REXLOG_INFO("[gpu] ao: the first occlusion pass: {} at {}x{} (visibility {}x{}), {} depth, "
                  "{}{}",
                  ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                  ms ? "multisampled" : "single-sampled",
                  plan.upscaler ? "noise rotating every frame, a temporal upscaler running too"
                                : "noise rotating every frame",
                  plan.view ? ", debug view" : "");
    }
    ++runs_;
    if (trigger_pause_) {
      if (pause_runs_ == 0) {
        const temporal::Camera& c = previous_;
        REXLOG_INFO("[gpu] ao: the first occlusion pass in a pause/map view: {} at {}x{} "
                    "(visibility {}x{}), {} host samples of the depth (guest sample 0 = host "
                    "sample {}), the lens of the previous pause/map frame's anchor draw: vertical "
                    "field of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}",
                    ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                    depth_target.host_samples, summary.host_sample,
                    c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane);
      }
      ++pause_runs_;
    }
  }
  return true;
}

bool AmbientOcclusion::RecordApply(plume::RenderCommandList* list,
                                   const ResolveCache::Summary& summary, const Plan& plan,
                                   const Push& push) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and
  // scale (the depth key's layer is 0 while the scene phase is open, the colour's is always 0).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples ||
      found->scale != depth.scale) {
    if (!refused_target_logged_) {
      refused_target_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the main view's colour target {} {} (depth: {} host samples at {}x): "
                   "no occlusion applied (reported once)",
                   DescribeEdramKey(key),
                   !found ? "does not exist" : (!found->texture ? "has no texture" : "differs"),
                   depth.host_samples, depth.scale);
    }
    return false;
  }
  uint32_t variant = ApplyVariant(plan.view != 0, depth.host_samples);
  if (plan.gi.on && plan.gi.view != 0) {
    variant = GiViewVariant(depth.host_samples);
  } else if (plan.gi.apply) {
    variant = GiApplyVariant(plan.gi.apply_view != 0, depth.host_samples);
  }
  plume::RenderPipeline* pipeline = Pipeline(variant);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  // The scene's own sequence for a target it draws into: the layout, a framebuffer of that target
  // alone (no depth: nothing here tests or writes it). It is a colour-only render pass on the main
  // target in the middle of the frame: the depth plane stays in SHADER_READ, the colour goes to
  // COLOR_WRITE as for any draw, and the next draw of the game moves the depth back to DEPTH_WRITE
  // itself.
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  pass_timing_->Mark(list, plan.gi.apply ? "ssgi apply" : "ao apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(plan.render[0]), float(plan.render[1])};
  list->setViewports(&viewport, 1);
  // The before/after split (split.h, mcla_gpu_split): the occlusion is applied left of it only, so the scissor ends at its column and
  // the pixels right of it are left as the game drew them. Off (the default): the whole target, nothing computed.
  uint32_t apply_columns = plan.render[0];
  if (const std::optional<double> cut = split::PositionFor(split::kEffectAo)) {
    apply_columns = split::EffectColumns(*cut, plan.render[0]);
  }
  const plume::RenderRect scissor(0, 0, int32_t(apply_columns), int32_t(plan.render[1]));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the contact shadows
// ---------------------------------------------------------------------------

bool AmbientOcclusion::PickWet(float* value) const {
  if (wet_draws_ == 0) {
    return false;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < kWetBins; ++i) {
    if (wet_bins_[i] > wet_bins_[best]) {
      best = i;
    }
  }
  *value = 0.1f * float(best);
  return true;
}

void AmbientOcclusion::PlanContact(Plan* plan) {
  plan->contact = ContactPlan{};
  ContactState state = ContactState::kOff;
  const auto finish = [&] {
    contact_state_ = state;
    if (uint32_t(state) != contact_state_logged_) {
      contact_state_logged_ = uint32_t(state);
      switch (state) {
        case ContactState::kOff:
          break;
        case ContactState::kNoLight:
          REXLOG_INFO("[gpu] contact shadows: no usable light constants before the trigger: none "
                      "this frame (logged when the state changes)");
          break;
        case ContactState::kGated:
          REXLOG_INFO("[gpu] contact shadows: none now: the sun is at {:.1f} degrees (gate {:.2f}, "
                      "off at {:.0f} and below, full from {:.0f}) and the wetness is {:.1f} (gate "
                      "{:.2f}) (logged when the state changes)",
                      contact_elevation_, contact_gate_sun_, kGateOffDegrees, kGateFullDegrees,
                      contact_wet_, contact_gate_wet_);
          break;
        case ContactState::kActive:
          REXLOG_INFO("[gpu] contact shadows: on: the sun is at {:.1f} degrees (gate {:.2f}), the "
                      "wetness {:.1f} (gate {:.2f}), strength {:.2f} (logged when the state "
                      "changes)",
                      contact_elevation_, contact_gate_sun_, contact_wet_, contact_gate_wet_,
                      contact_strength_);
          break;
      }
    }
  };
  if (contact_mode_ == Mode::kOff) {
    finish();
    return;
  }
  LightRaw light;
  uint32_t with_it = 0;
  float ambient[3], sun[3], dir[3];
  if (!PickLight(&light, &with_it)) {
    state = ContactState::kNoLight;
    finish();
    return;
  }
  DecodeLight(light, ambient, sun, dir);
  const double length = Length3(dir);
  if (!std::isfinite(length) || length < 0.5) {
    state = ContactState::kNoLight;
    finish();
    return;
  }
  // VS c16 is the direction the light travels, so the sun's height above the horizon is -c16.y: noon
  // is +0.867 (60 degrees), 19 h 2.5 degrees, the night's light points up (negative here).
  const double sin_elevation = std::clamp(-double(dir[1]) / length, -1.0, 1.0);
  const double elevation = std::asin(sin_elevation) * 180.0 / 3.14159265358979323846;
  float wet = 0.0f;
  PickWet(&wet);  // none readable: dry
  contact_elevation_ = elevation;
  contact_wet_ = wet;
  contact_gate_sun_ = Smoothstep(kGateOffDegrees, kGateFullDegrees, elevation);
  contact_gate_wet_ = 1.0f - std::clamp((wet - kWetDry) / kWetSpan, 0.0f, 1.0f);
  const float setting = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_strength), 0.0, 2.0));
  contact_strength_ = setting * contact_gate_sun_ * contact_gate_wet_;
  if (!(contact_strength_ > 0.002f) || !(Luma(sun) > 1e-4f)) {
    state = ContactState::kGated;
    finish();
    return;
  }
  state = ContactState::kActive;
  ContactPlan& c = plan->contact;
  c.mode = contact_mode_;
  c.strength = contact_strength_;
  // The ray grows with a low sun (its shadows are long): 0.7 / tan(elevation), 1 to 3 times.
  c.length = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_length), 0.1, 3.0) *
                   contact_trace::LengthGrow(elevation));
  c.thickness = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_thickness), 0.05, 3.0));
  c.steps = contact_mode_ == Mode::kHigh ? contact_trace::kStepsHigh : contact_trace::kStepsUltra;
  c.end_fade_start = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_fade),
                                      double(contact_trace::kFadeStartMin),
                                      double(contact_trace::kFadeStartMax)));
  c.fade_start = kContactFadeStart;
  c.fade_end = kContactFadeEnd;
  c.mask_gain = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_mask_gain), 0.5, 4.0));
  c.under_vehicle = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_vehicles), 0.0, 1.0));
  c.fast = REXCVAR_GET(mcla_gpu_contact_shadows_fast);
  finish();
}

void AmbientOcclusion::NoteMaskResolve(const ResolveRecord& record) {
  // A colour copy (not a depth one, not a clear alone) into the mask's page, as the lit shaders fetch
  // it: k_8_8_8_8, the picture's size, level 0.
  const uint32_t source = record.resolve_flags & 7;
  if (source >= 4 || (record.resolve_flags & 0x2000) != 0 || record.dest_level != 0 ||
      !(record.flags & kEventHasDest) || (record.flags & kEventDestUnreadable)) {
    return;
  }
  uint32_t dwords[6];
  for (uint32_t i = 0; i < 6; ++i) {
    dwords[i] = LoadBe32(record.dest_header + guest::kTextureFetchOffset + 4 * i);
  }
  const TextureFetch dest = ReadTextureFetch(dwords);
  if (!dest.is_texture || dest.key.base_page != kMaskPage ||
      dest.key.format != kTextureFormat8888 || dest.key.width < kMaskMinWidth) {
    return;
  }
  mask_key_ = dest.key;
  mask_frame_ = frame_;
}

uint32_t AmbientOcclusion::FindMaskSlot() {
  // Only a resolve of this frame made the mask: at night, at 6 h and 18 h and in rain none comes,
  // and the page then holds the last frame's depth copy (the same key, a depth source), which is
  // not a mask. The game's sun shadow is then not there to leave out: the visibility is 1.
  if (!resolve_ || mask_frame_ != frame_) {
    return kNoSlot;
  }
  ResolveCache::Product* product = resolve_->Find(mask_key_, mask_key_.base_page);
  if (!product || !product->texture || product->depth_source ||
      product->layout != plume::RenderTextureLayout::SHADER_READ) {
    if (!mask_why_logged_) {
      mask_why_logged_ = true;
      REXLOG_INFO("[gpu] contact shadows: a resolve into the mask's page came this frame but the "
                  "product is {}: no mask, the sun's visibility is 1 (reported once)",
                  !product ? "not found"
                           : (!product->texture ? "without a texture"
                              : (product->depth_source ? "a depth copy"
                                                       : "not in the shader-read layout")));
    }
    return kNoSlot;
  }
  const uint32_t slot =
      product->views.Slot(*device_, *heap_, *product->texture, product->format,
                          mask_key_.levels(), format::kHostSwizzleRGBA, "ao contact mask view");
  // The product is made at the host scale of the targets its resolve read: the picture's size at a render scale (3840x2160 at 3x).
  mask_dims_[0] = product->scale.Edge(mask_key_.width);
  mask_dims_[1] = product->scale.Edge(mask_key_.height);
  return slot == kInvalidBindlessSlot ? kNoSlot : slot;
}

uint32_t AmbientOcclusion::RecordContact(plume::RenderCommandList* list, const Plan& plan,
                                         const Push& base, uint32_t mask_slot) {
  plume::RenderPipeline* march = Pipeline(kVContact);
  // With mcla_gpu_contact_shadows_fast the apply pass does the filter: the denoise pass is not drawn.
  const bool in_apply = plan.ContactFilterInApply();
  plume::RenderPipeline* denoise = in_apply ? nullptr : Pipeline(kVDenoise);
  if (!march || (!in_apply && !denoise)) {
    return kNoSlot;
  }
  const ContactPlan& c = plan.contact;
  // Beside a half size occlusion the contact shadows have a full size depth and images of their own (Plan::SplitContact);
  // otherwise they read the occlusion's depth and use its scratch images, which it has finished with by now.
  const bool split = plan.SplitContact();
  const HostImage& contact_depth = split ? depth_full_ : depth_[0];
  HostImage& contact_raw = split ? contact_raw_ : raw_;
  HostImage& contact_dn = split ? contact_dn_ : denoised_;
  // The march: the linear depth in, the contact visibility out (contact_raw). Its constants are in the
  // header of hlsl/ao_contact_ps.hlsl.
  pass_timing_->Mark(list, "ao contact");
  Push push = base;
  push.slots[2] = contact_depth.slot;
  push.sizes[0] = push.sizes[2] = float(plan.contact_size[0]);
  push.sizes[1] = push.sizes[3] = float(plan.contact_size[1]);
  push.lens[2] = c.length;
  push.lens[3] = c.thickness;
  push.shape[0] = c.end_fade_start;
  push.shape[1] = float(c.steps);
  push.extra[1] = c.fade_end;
  // The ground under a vehicle (contact_trace.h): the world's up in view space in lin.xyz and how strongly
  // it shadows in lin.w. The march does not read the depth rows that lin holds in the other passes (the
  // filter below does not either). No usable up (a camera the sun's arrangement cannot read) or a strength
  // of 0: lin.w is 0 and the pass is as it was.
  const contact_trace::Vec3 up = contact_trace::UpInView(previous_.view.m);
  const bool under = c.under_vehicle > 0.0f && contact_trace::Dot(up, up) > 0.5f;
  push.lin[0] = under ? up.x : 0.0f;
  push.lin[1] = under ? up.y : 0.0f;
  push.lin[2] = under ? up.z : 0.0f;
  push.lin[3] = under ? c.under_vehicle : 0.0f;
  // Cars the game draws into the sun shadow map have their own shadow: the player's car always, and with TRAFFIC SHADOWS
  // (traffic_shadow.h) the traffic cars up to the distance the hook publishes. The term stands aside for a car within this
  // distance of the camera (extra.w; contact_trace.h UnderCastDistance: at least kPlayerCarCastDistance, so that the ground
  // beyond the player's car, lit in the game's mask, is not darkened as if it were under the car).
  // Only the march reads extra.w; the filter below does not, and the value is put back for it all the same.
  const float base_extra_w = push.extra[3];
  push.extra[3] = under ? contact_trace::UnderCastDistance(traffic_shadow::CastDistance()) : 0.0f;
  // The ray start's noise has no history here, only the filter below. Moved every frame it would make
  // the edge of every contact shadow crawl; it is a fixed pattern (frame 0) unless a temporal upscaler
  // runs and averages the frames, as the shadow filter's and the sun shafts' are.
  push.slots2[1] = plan.upscaler ? base.slots2[1] : 0u;
  // The adaptive step count (contact_trace.h AdaptiveSteps): the end gap in pixels in shape.z, which the march
  // does not otherwise read; 0 is every step, as the setting says, and above 1080 lines of picture (kAdaptMaxHeight) the
  // count costs more than it saves, so it is left off there.
  push.shape[2] = (c.fast && contact_trace::AdaptiveOnAt(plan.contact_size[1])) ? contact_trace::kAdaptEndPixels : 0.0f;
  // The skip under the game's own shadow (contact_trace.h "the skip under the game's own shadow"): the march returns 1
  // where every mask texel the apply's filter can reach for the pixel is 0 (or so near it that the apply leaves the pixel
  // out), which changes no pixel of the picture. Only where the apply does the filter itself (in_apply), only in a frame
  // that has the mask, and not while a debug picture is on (the apply filters every pixel then and shows the visibility).
  // The limit is made here from the apply's own numbers (the strength, the gain, the frame's lights in the base push); a block pass
  // (the same shader, flag kSideFlag) makes an image of 4 x 4 texel blocks of the mask first, which the march reads (slots2.w).
  const bool debug_picture = plan.view != 0u || plan.gi.view != 0u || plan.gi.apply_view != 0u;
  const uint32_t mask_w = mask_slot != kNoSlot ? mask_dims_[0] : 0u;
  const uint32_t mask_h = mask_slot != kNoSlot ? mask_dims_[1] : 0u;
  const float skip_limit = (c.fast && in_apply && mask_slot != kNoSlot)
                               ? contact_trace::SkipLimit(base.light[3], base.extra[0], c.strength, c.mask_gain)
                               : -1.0f;
  const bool skip = contact_trace::MaskSkipOn(
      c.fast, in_apply, mask_slot != kNoSlot, debug_picture, skip_limit,
      contact_trace::SkipScaleOk(int(mask_w), int(mask_h), int(plan.contact_size[0]), int(plan.contact_size[1])));
  if (skip) {
    const uint32_t block_w = uint32_t(contact_trace::SkipBlockCount(int(mask_w)));
    const uint32_t block_h = uint32_t(contact_trace::SkipBlockCount(int(mask_h)));
    if (!passes_->EnsureImage(contact_skip_, block_w, block_h, plume::RenderFormat::R16_FLOAT, "ao contact skip blocks")) {
      return kNoSlot;
    }
    // The block pass: slots2.w the mask's slot, shape.w the limit, extra.x and extra.z one over the mask's size.
    pass_timing_->Mark(list, "ao contact skip");
    Push blocks = base;
    blocks.slots2[2] = contact_trace::kSideFlag;
    blocks.slots2[3] = mask_slot;
    blocks.shape[3] = skip_limit;
    blocks.extra[0] = 1.0f / float(mask_w);
    blocks.extra[2] = 1.0f / float(mask_h);
    DrawPass(list, contact_skip_, march, blocks);
    pass_timing_->Mark(list, "ao contact");
    push.slots2[2] |= contact_trace::kSkipFlag;
    push.slots2[3] = contact_skip_.slot;
    push.sizes[0] = float(mask_w) / float(plan.contact_size[0]) * 0.25f;
    push.sizes[1] = float(mask_h) / float(plan.contact_size[1]) * 0.25f;
    push.extra[0] = 1.0f / float(block_w);
    push.extra[2] = 1.0f / float(block_h);
    if (!contact_skip_logged_) {
      contact_skip_logged_ = true;
      REXLOG_INFO("[gpu] contact shadows: the march skips the pixels under the game's own sun shadow (mask slot {}, {}x{} against the {}x{} "
                  "picture; limit {:.3g} of the stored value, so a shade that stores more than {:.1f}/255 is not skipped; blocks {}x{}; "
                  "mcla_gpu_contact_shadows_fast; the picture is not changed: contact_trace.h, logged once; "
                  "mcla_gpu_contact_skip_stat counts what it skips)",
                  mask_slot, mask_w, mask_h, plan.contact_size[0], plan.contact_size[1], skip_limit, double(skip_limit) * 255.0, block_w,
                  block_h);
    }
  } else if (contact_skip_.texture) {
    passes_->ReleaseImage(contact_skip_);
  }
  const bool stat_wanted = REXCVAR_GET(mcla_gpu_contact_skip_stat) > 0 && c.fast && in_apply && mask_slot != kNoSlot && !debug_picture;
  Push stat_push = push;  // the march's push as it is: the statistics pass is the same shader, the flag and the mask's slot added
  // The player's car's own sun shadow map (car_shadow_map.h; contact_trace.h "the player's car's own sun shadow map"): in a frame that has
  // one the march reads it for the pixels it covers (the push block has no room for the matrix: the shader reads its three rows from the
  // 4 x 1 matrix texture; slots.w the map's slot, slots2.x the matrix's, flag bit 28 = a map is there). Without one, the pass is as it was.
  uint32_t map_flags = 0;
  if (car_shadow_map_ != nullptr) {
    // CHECK: CarShadowMap::View() is the contract's accessor (car_shadow_map.h): the map of THIS frame's player car, recorded on this list
    // before the AO's passes, left in the shader-read layout (the map and its matrix texture), valid == false when none was drawn.
    const CarShadowMapView& map = car_shadow_map_->View();
    if (map.valid && map.slot != kNoSlot && map.matrix_slot != kNoSlot && map.size >= 2 && map.texel_m > 0.0f && map.depth_m > 0.0f) {
      push.slots[3] = map.slot;
      push.slots2[0] = map.matrix_slot;
      map_flags = contact_trace::kCarMapFlag;
      if (plan.contact.car_map_view) {
        map_flags |= contact_trace::kCarMapViewFlag;
      }
      if (REXCVAR_GET(mcla_gpu_contact_shadows_car_map_march)) {
        map_flags |= contact_trace::kCarMapKeepMarchFlag;
      }
      push.slots2[2] |= map_flags;
      if (!car_map_logged_) {
        car_map_logged_ = true;
        REXLOG_INFO("[gpu] contact shadows: the march reads the player's car's sun shadow map (slot {}, matrix slot {}, {}x{} texels of {:.1f} mm, "
                    "depth range {:.2f} m): the car's own parts shadow the car and the road near it exactly, a vehicle's pixel the map covers takes "
                    "no march steps{} (contact_trace.h; mcla_gpu_contact_shadows_view 5 shows the map's term; logged once)",
                    map.slot, map.matrix_slot, map.size, map.size, double(map.texel_m) * 1000.0, double(map.depth_m),
                    (map_flags & contact_trace::kCarMapKeepMarchFlag) != 0u ? " (kept by mcla_gpu_contact_shadows_car_map_march)" : "");
      }
    }
  }
  if (map_flags == 0u && plan.contact.car_map_view) {
    push.extra[1] = 0.0f;  // the debug view of the map's term in a frame without a map: nothing is computed, every pixel is white
  }
  DrawPass(list, contact_raw, march, push);
  push.shape[2] = base.shape[2];
  push.extra[3] = base_extra_w;
  push.extra[1] = c.fade_end;
  push.slots[3] = base.slots[3];
  push.slots2[0] = base.slots2[0];
  push.slots2[2] &= ~(contact_trace::kCarMapFlag | contact_trace::kCarMapViewFlag | contact_trace::kCarMapKeepMarchFlag);
  if (in_apply) {
    if (stat_wanted) {
      RecordContactStat(list, plan, stat_push, mask_slot, skip ? skip_limit : -1.0f);
    } else if (contact_stat_.texture) {
      passes_->ReleaseImage(contact_stat_);
    }
    // The apply pass reads the march's own image and does the 3x3 filter itself (flag bit 25 there, ao_apply_ps).
    return contact_raw.slot;
  }
  // The filter: one 3x3 pass of the occlusion's denoise shader in its contact mode, whose weights also
  // fall with the difference of the visibility: the noise of the ray's start goes, the edges of the
  // shadow and of the depth stay (the first version ran the plane-only blur twice, which softened every
  // edge and cost more than the march). Into contact_dn (denoised_), which the apply reads.
  pass_timing_->Mark(list, "ao contact denoise");
  push.slots[2] = contact_raw.slot;
  push.slots[3] = contact_depth.slot;
  push.slots2[2] |= kContactFilterFlag;
  DrawPass(list, contact_dn, denoise, push);
  return contact_dn.slot;
}

// ---------------------------------------------------------------------------
// the contact skip's statistics
// ---------------------------------------------------------------------------

void AmbientOcclusion::RecordContactStat(plume::RenderCommandList* list, const Plan& plan, const Push& march_push, uint32_t mask_slot,
                                         float limit) {
  const int every = REXCVAR_GET(mcla_gpu_contact_skip_stat);
  if (stat_busy_ || every <= 0 || (stat_taken_ != 0 && frame_ < stat_last_frame_ + uint64_t(every))) {
    return;
  }
  plume::RenderPipeline* march = Pipeline(kVContact);
  const uint32_t width = plan.contact_size[0];
  const uint32_t height = plan.contact_size[1];
  if (!march || !passes_->EnsureImage(contact_stat_, width, height, plume::RenderFormat::R16_FLOAT, "ao contact skip statistics")) {
    return;
  }
  const uint32_t row_bytes = (width * 2 + 255) / 256 * 256;  // R16_FLOAT, rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!stat_buffer_ || stat_buffer_bytes_ < bytes) {
    stat_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes), "ao contact skip readback");
    stat_buffer_bytes_ = stat_buffer_ ? bytes : 0;
    if (!stat_buffer_) {
      return;
    }
  }
  pass_timing_->Mark(list, "ao contact skip statistics");
  Push push = march_push;
  push.slots[0] = mask_slot;  // the statistics pass reads the mask's own value at the pixel's centre, at (pixel + 0.5) x sizes.xy x 4
  push.sizes[0] = float(mask_dims_[0]) / float(width) * 0.25f;
  push.sizes[1] = float(mask_dims_[1]) / float(height) * 0.25f;
  push.slots2[2] |= contact_trace::kSideFlag | contact_trace::kStatFlag;
  DrawPass(list, contact_stat_, march, push);
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(contact_stat_.texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
  contact_stat_.layout = plume::RenderTextureLayout::COPY_SOURCE;  // the next DrawPass moves it on
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(stat_buffer_.get(), plume::RenderFormat::R16_FLOAT, width, height, 1, row_bytes / 2, 0),
      plume::RenderTextureCopyLocation::Subresource(contact_stat_.texture.get()));
  stat_busy_ = true;
  stat_last_frame_ = frame_;
  ++stat_taken_;
  const uint64_t frame = frame_;
  const uint64_t generation = generation_;
  const uint32_t row_texels = row_bytes / 2;
  const bool skip_on = (march_push.slots2[2] & contact_trace::kSkipFlag) != 0u;
  const uint32_t mask_w = mask_dims_[0], mask_h = mask_dims_[1];
  frames_->RetireAction([this, frame, generation, width, height, row_texels, limit, skip_on, mask_w, mask_h] {
    ProcessContactStat(frame, generation, width, height, row_texels, limit, skip_on, mask_w, mask_h);
  });
}

void AmbientOcclusion::ProcessContactStat(uint64_t frame, uint64_t generation, uint32_t width, uint32_t height, uint32_t row_texels,
                                          float limit, bool skip_on, uint32_t mask_w, uint32_t mask_h) {
  stat_busy_ = false;
  if (generation != generation_ || !stat_buffer_ || width == 0 || height == 0) {
    return;
  }
  const void* mapped = stat_buffer_->map();
  if (!mapped) {
    REXLOG_ERROR("[gpu] contact skip statistics of frame {}: the readback could not be mapped", frame);
    return;
  }
  // What each pixel wrote (hlsl/ao_contact_ps.hlsl SkipStat, contact_trace.h kStat*): -1 skipped, 2 the sky or past the contact fade,
  // 3 + v turned from the sun, v (0 to 1) marching, v the mask's stored value at the pixel's centre.
  uint64_t far_count = 0, skipped = 0, back = 0, march = 0;
  // The spread of the stored value v of the pixels that were not skipped, in 1/255 units: 0, 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65-120, above (a lit pixel stores
  // up to 0.5 = 127.5/255).
  uint64_t spread[10] = {};
  uint64_t tile_counted = 0, tile_all_skipped = 0, tile_skipped_pixels = 0;  // 8 x 4 tiles with a non-sky pixel, of which all skipped
  const uint16_t* source = static_cast<const uint16_t*>(mapped);
  for (uint32_t y = 0; y < height; ++y) {
    const uint16_t* row = source + size_t(y) * row_texels;
    for (uint32_t x = 0; x < width; ++x) {
      const float value = HalfToFloat(row[x]);
      if (value < -0.5f) {
        ++skipped;
        continue;
      }
      if (value > 1.5f && value < 2.5f) {
        ++far_count;
        continue;
      }
      const bool facing_away = value >= 2.5f;
      const float stored = facing_away ? value - contact_trace::kStatBack : value - contact_trace::kStatMarch;
      (facing_away ? back : march)++;
      const float units = stored * 255.0f;
      const size_t bucket = units < 0.5f ? 0 : units < 1.5f ? 1 : units < 2.5f ? 2 : units < 4.5f ? 3 : units < 8.5f ? 4 : units < 16.5f ? 5
                            : units < 32.5f ? 6 : units < 64.5f ? 7 : units < 120.5f ? 8 : 9;
      ++spread[bucket];
    }
  }
  // Whole 8 x 4 tiles (a warp's pixels, about) in which every pixel that is not sky is skipped: what the GPU saves.
  for (uint32_t ty = 0; ty < height; ty += 4) {
    for (uint32_t tx = 0; tx < width; tx += 8) {
      uint32_t counted = 0, tile_skipped = 0;
      for (uint32_t y = ty; y < std::min(ty + 4, height); ++y) {
        const uint16_t* row = source + size_t(y) * row_texels;
        for (uint32_t x = tx; x < std::min(tx + 8, width); ++x) {
          const float value = HalfToFloat(row[x]);
          if (value > 1.5f && value < 2.5f) {
            continue;
          }
          ++counted;
          tile_skipped += value < -0.5f ? 1 : 0;
        }
      }
      if (counted != 0) {
        ++tile_counted;
        if (tile_skipped == counted) {
          ++tile_all_skipped;
          tile_skipped_pixels += counted;
        }
      }
    }
  }
  stat_buffer_->unmap();
  const uint64_t all = uint64_t(width) * height;
  const uint64_t not_far = skipped + back + march;
  const auto share = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
  const uint64_t not_skipped = back + march;
  REXLOG_INFO("[gpu] contact skip statistics of frame {} ({}x{}, mask {}x{}, the skip {}, limit {:.3g}): of {} pixels the sky or past the fade {:.1f}%; of the "
              "rest ({:.1f}%): skipped {:.1f}%, turned from the sun {:.1f}%, marching {:.1f}%; whole 8x4 tiles skipped (what the GPU saves) {:.1f}% of the pixels "
              "that are not sky; the mask's stored value at the {} pixels that were not skipped, in 1/255: 0: {:.1f}%, 1: {:.1f}%, 2: {:.1f}%, 3-4: {:.1f}%, 5-8: {:.1f}%, "
              "9-16: {:.1f}%, 17-32: {:.1f}%, 33-64: {:.1f}%, 65-120: {:.1f}%, 121 or more: {:.1f}% (a game shade that stores more than {:.1f}/255 "
              "cannot be skipped; the sun stores up to 127.5)",
              frame, width, height, mask_w, mask_h, skip_on ? "on" : "off", limit, all, share(far_count, all), share(not_far, all),
              share(skipped, not_far), share(back, not_far), share(march, not_far), share(tile_skipped_pixels, not_far), not_skipped,
              share(spread[0], not_skipped), share(spread[1], not_skipped), share(spread[2], not_skipped), share(spread[3], not_skipped),
              share(spread[4], not_skipped), share(spread[5], not_skipped), share(spread[6], not_skipped), share(spread[7], not_skipped),
              share(spread[8], not_skipped), share(spread[9], not_skipped), double(std::max(limit, 0.0f)) * 255.0);
  (void)tile_counted;
  (void)tile_all_skipped;
  const double shadowed = not_far != 0 ? double(skipped) / double(not_far) : 0.0;
  const contact_trace::Cost plain = contact_trace::CostModel(width, height, contact_trace::kStepsUltra, 1, true);
  const contact_trace::Cost fast = contact_trace::CostModelFast(width, height, contact_trace::kStepsUltra, true, shadowed);
  REXLOG_INFO("[gpu] contact skip statistics of frame {}: the model's march at this share skipped ({:.1f}% of the pixels that are not sky): {:.3f} ms + the block "
              "pass {:.3f} ms against the plain march's {:.3f} ms (the model of contact_trace.h, an RTX 5080 at 3840x2160 scales with the pixels; not measured)",
              frame, 100.0 * shadowed, fast.march_ms, fast.block_ms, plain.march_ms);
}

// ---------------------------------------------------------------------------
// the bounce light test (screen-space global illumination, first step)
// ---------------------------------------------------------------------------

void AmbientOcclusion::PlanGi(Plan* plan) {
  plan->gi = GiPlan{};
  // The game view, the pause menu's world and the garage have it; the map (the 4x view seen from 9 km: the
  // bounce's few metres are under a pixel) runs the plain occlusion (menu_world.h RulesFor).
  if (!gi_active_ || !plan->occlusion || !plan->apply || !menu_world::RulesFor(trigger_world_).bounce) {
    return;
  }
  // The albedo estimate divides a pixel's colour by the light the game's constants say fell on it: with no light
  // constants at all (no lit draw before the trigger) that light reads as nothing, every pixel's albedo as 1, and the
  // bounce would add the surroundings' colour to every pixel whatever its own. No light, no bounce this frame.
  LightRaw known_light;
  uint32_t known_light_draws = 0;
  if (!PickLight(&known_light, &known_light_draws)) {
    return;
  }
  GiPlan& g = plan->gi;
  g.on = true;
  // What the albedo estimate divides by, once for each world the bounce light first runs in (the garage's lamps are the
  // question: if its ambient and sun are near nothing the estimate reads every pixel as bright, and this line says so).
  if (const size_t world_index = size_t(trigger_world_); world_index < std::size(gi_light_logged_) && !gi_light_logged_[world_index]) {
    gi_light_logged_[world_index] = true;
    float ambient[3], sun[3], dir[3];
    DecodeLight(known_light, ambient, sun, dir);
    REXLOG_INFO("[gpu] ssgi: the light the bounce light divides by in the {}: ambient {:.3g}, sun {:.3g} (PS c26 and c39 lumas, "
                "the commonest of {} draws' values; T = ambient + sun x N.L x the sun's shadow mask, so a pixel brighter than "
                "that is taken as a surface of albedo 1 or a light); logged once for each world",
                menu_world::Name(trigger_world_), Luma(ambient), Luma(sun), known_light_draws);
  }
  g.test = gi_test_;
  g.mode = gi_test_ ? plan->mode : gi_mode_;
  // What the quality means: the debug override is the occlusion's own (as it was); the setting's ultra is 3 and
  // 3 at the picture's size and its high is cheaper by fewer directions, by half the size, or both
  // (mcla_gpu_ssgi_low, named for the level that was called low).
  const int low_how = std::clamp(REXCVAR_GET(mcla_gpu_ssgi_low), 0, 2);
  g.fewer = gi_test_ ? plan->mode == Mode::kHigh : (g.mode == Mode::kHigh && low_how != 1);
  g.half = !gi_test_ && g.mode == Mode::kHigh && low_how >= 1;
  g.half_above = REXCVAR_GET(mcla_gpu_ssgi_half);  // above 1080 lines, every quality (ao_size.h)
  // The numbers of the strength level (mcla_gpu_ssgi_intensity), each one replaced by its debug cvar when that
  // is set (ssgi_level.h).
  const ssgi_level::Level level = ReadSsgiLevel();
  ssgi_level::Overrides overrides;
  overrides.strength = REXCVAR_GET(mcla_gpu_ssgi_strength);
  overrides.limit = REXCVAR_GET(mcla_gpu_ssgi_clamp);
  overrides.reach = REXCVAR_GET(mcla_gpu_ssgi_reach);
  overrides.cap = REXCVAR_GET(mcla_gpu_ssgi_test_clamp);
  overrides.far_steps = REXCVAR_GET(mcla_gpu_ssgi_far_steps);
  const ssgi_level::Effective numbers = ssgi_level::Choose(level, overrides);
  g.level = uint32_t(level);
  g.overridden = numbers.overridden;
  g.strength = numbers.strength;
  g.limit = numbers.limit;
  g.cap = numbers.cap;
  g.reach = numbers.reach;
  g.far_steps = ssgi_light::FarSteps(!g.fewer, numbers.far_steps, numbers.reach);
  const uint32_t level_key = (uint32_t(level) << 1) | (numbers.overridden ? 1u : 0u);
  if (level_key != gi_level_logged_) {
    gi_level_logged_ = level_key;
    REXLOG_INFO("[gpu] ssgi: bounce strength {}{} (mcla_gpu_ssgi_intensity, live): strength {:.3g}, gain limit "
                "{:.3g}, reach {:.3g} m ({} far step(s) a side), cap {:.3g} x the frame's light",
                ssgi_level::Name(level), numbers.overridden ? " with debug overrides" : "", g.strength, g.limit,
                g.reach, g.far_steps, g.cap);
  }
  g.radius = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_radius), 0.5, 3.0));
  g.apply_view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_view), 0, 6));
  g.thickness = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_thickness), 0.0, 4.0));
  g.facing = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_facing), 0.0, 1.0));
  g.view_gain = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_view_gain), 0.1, 16.0));
  g.view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_view), 0, 9));
  g.history = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history), 2, 64));
  // The clip box of the bounce's history: the setting's width and a share of the history's own value, or the first
  // version's 1.25 deviations alone (ssgi_stable.h).
  const bool stable = REXCVAR_GET(mcla_gpu_ao_stable);
  g.clamp_width = stable ? float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history_clamp), 0.25, 8.0))
                         : ssgi_stable::kFirstClipWidth;
  g.clip_history = stable ? ssgi_stable::kClipHistoryShare : ssgi_stable::kFirstClipHistoryShare;
  g.atrous = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_atrous), 0, 3));
  if (g.view != 0) {
    plan->view = 0;  // instead of the other debug views: they are of the plain passes
    g.apply_view = 0;  // the test's own pictures come first
  } else if (g.apply_view != 0) {
    plan->view = 0;
  }
  // The apply with the bounce: always (the visibility comes from the same image), except where the
  // occlusion's or the contact shadows' debug view asks for the plain apply, or the test shows a picture
  // of its own; a strength of 0 is the test as it was (the plain apply, the bitmask's visibility).
  g.apply = g.view == 0 && (g.apply_view != 0 || (g.strength > 0.0f && plan->view == 0));
}

bool AmbientOcclusion::PrepareGi(const ResolveCache::Summary& summary,
                                 plume::RenderCommandList* list, const Plan& plan,
                                 EdramTargets::Target** colour, uint32_t* colour_slot) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and
  // scale (RecordApply finds it the same way).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  std::string why;
  const EdramTargets::Target* found = edram_->Find(key);
  EdramTargets::Target* target = nullptr;
  uint32_t slot = kNoSlot;
  if (!found || !found->texture) {
    why = std::format("the main view's colour target {} {}", DescribeEdramKey(key),
                      !found ? "does not exist" : "has no texture");
  } else if (found->host_samples != depth.host_samples || found->scale != depth.scale) {
    why = std::format("the main view's colour target {} differs from the depth (samples {} against "
                      "{}, scale {}x against {}x)",
                      DescribeEdramKey(key), found->host_samples, depth.host_samples, found->scale,
                      depth.scale);
  } else {
    bool unset = false;
    uint32_t stencil = 0;
    target = edram_->Get(key, found->height, found->scale, 0, list, &unset);
    if (!target || !edram_->SampleSlots(*target, &slot, &stencil)) {
      why = "the main view's colour target could not be read as a texture";
    }
  }
  if (why.empty()) {
    // The pipelines (made here on first use: the test's are never listed ahead) and the images.
    const bool ms = depth.host_samples > 1;
    plume::RenderPipeline* main = MainGiPipeline(plan.gi);
    plume::RenderPipeline* level0 = Pipeline(ms ? kVColorMs : kVColor);
    plume::RenderPipeline* mip = Pipeline(kVColorMip);
    plume::RenderPipeline* view = plan.gi.view != 0 ? Pipeline(GiViewVariant(depth.host_samples)) : mip;
    plume::RenderPipeline* denoise = Pipeline(kVDenoiseGi);
    plume::RenderPipeline* temporal = Pipeline(ms ? kVTemporalGiMs : kVTemporalGi);
    plume::RenderPipeline* atrous = plan.gi.atrous != 0 ? Pipeline(kVAtrous) : mip;
    plume::RenderPipeline* apply =
        plan.gi.apply ? Pipeline(GiApplyVariant(plan.gi.apply_view != 0, depth.host_samples)) : mip;
    if (!main || !level0 || !mip || !view || !denoise || !temporal || !atrous || !apply) {
      why = "a pipeline of the test could not be made";
    } else if (!EnsureGiImages(plan)) {
      why = "an image of the test could not be made";
    }
  }
  if (!why.empty()) {
    if (!gi_failed_logged_) {
      gi_failed_logged_ = true;
      REXLOG_ERROR("[gpu] ssgi: no bounce light test in the frames that cannot have it (the occlusion "
                   "runs as usual): {} (reported once)",
                   why);
    }
    return false;
  }
  *colour = target;
  *colour_slot = slot;
  return true;
}

bool AmbientOcclusion::EnsureGiImages(const Plan& plan) {
  bool ok =
      passes_->EnsureImage(colour_[0], plan.ao[0], plan.ao[1], kGiFormat, "ssgi colour") &&
      passes_->EnsureImage(colour_[1], plan.level[0][0], plan.level[0][1], kGiFormat,
                           "ssgi colour 1/2") &&
      passes_->EnsureImage(colour_[2], plan.level[1][0], plan.level[1][1], kGiFormat,
                           "ssgi colour 1/4") &&
      passes_->EnsureImage(gi_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi visibility and bounce") &&
      passes_->EnsureImage(gi_dn_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi denoised") &&
      passes_->EnsureImage(gi_hist_[0], plan.ao[0], plan.ao[1], kGiFormat, "ssgi history 0") &&
      passes_->EnsureImage(gi_hist_[1], plan.ao[0], plan.ao[1], kGiFormat, "ssgi history 1") &&
      passes_->EnsureImage(depth_prev_, plan.ao[0], plan.ao[1], plume::RenderFormat::R32_FLOAT,
                           "ssgi depth history");
  // The picture of the history weight (debug view 9, or the occlusion's own view 6 while the test runs),
  // made only while one of them is shown.
  if (ok && (plan.gi.view == 9u || plan.view == 6u)) {
    ok = passes_->EnsureImage(gi_weight_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi history weight");
  } else if (gi_weight_.texture) {
    passes_->ReleaseImage(gi_weight_);
  }
  return ok;
}

void AmbientOcclusion::ReleaseGiImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : colour_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
    }
  }
  for (HostImage* image : {&gi_, &gi_dn_, &gi_hist_[0], &gi_hist_[1], &depth_prev_, &gi_weight_}) {
    if (image->texture) {
      passes_->ReleaseImage(*image);
    }
  }
  // A history of the test's kind goes with its images (a plain history is made again when it is asked for).
  if (history_gi_) {
    history_valid_ = false;
    history_gi_ = false;
  }
}

void AmbientOcclusion::RecordColourChain(plume::RenderCommandList* list, const Plan& plan,
                                         const Push& base, EdramTargets::Target& colour,
                                         uint32_t colour_slot) {
  // The colour target is read as a texture: after the opaque draws it is in its write layout; the
  // apply pass (or the game's next draw) moves it back.
  edram_->Transition(list, colour, plume::RenderTextureLayout::SHADER_READ);
  pass_timing_->Mark(list, "ssgi colour chain");
  const bool ms = colour.host_samples > 1;
  // Level 0: the colour target's mean over the pixels and samples a texel covers, each sample capped at
  // a multiple of the frame's ambient + sun (ao_color_ps.hlsl).
  Push push = base;
  push.slots2[2] &= ~0xFFu;  // no debug view
  push.slots[0] = colour_slot;
  push.slots[1] = colour.host_samples;
  push.sizes[0] = float(plan.ao[0]);
  push.sizes[1] = float(plan.ao[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  push.shape[0] = ssgi_light::RadianceCap(plan.gi.cap, base.light[3], base.extra[0]);
  push.shape[1] = ssgi_light::SourceStart(base.light[3], base.extra[0]);
  if (ms && plan.OneSampleColour()) {
    // Half size (above 1080 lines): the texel is the mean of the 2x2 pixels' one sample each, the depth's own host sample, not of every
    // sample of them (the bounce is a smooth signal, blurred and filtered over many pixels after this; half the colour target's reads).
    push.slots2[2] |= kColourOneSampleFlag;
    push.slots2[0] = base.slots[1];
  }
  DrawPass(list, colour_[0], Pipeline(ms ? kVColorMs : kVColor), push);
  // The coarser levels, one pass each, as the depth chain's.
  plume::RenderPipeline* mip = Pipeline(kVColorMip);
  push = base;
  push.slots2[2] &= ~0xFFu;
  push.slots[2] = colour_[0].slot;
  push.sizes[0] = float(plan.level[0][0]);
  push.sizes[1] = float(plan.level[0][1]);
  push.sizes[2] = float(plan.ao[0]);
  push.sizes[3] = float(plan.ao[1]);
  DrawPass(list, colour_[1], mip, push);
  push.slots[2] = colour_[1].slot;
  push.sizes[0] = float(plan.level[1][0]);
  push.sizes[1] = float(plan.level[1][1]);
  push.sizes[2] = float(plan.level[0][0]);
  push.sizes[3] = float(plan.level[0][1]);
  DrawPass(list, colour_[2], mip, push);
}

// ---------------------------------------------------------------------------
// the census's depth statistic
// ---------------------------------------------------------------------------

void AmbientOcclusion::RecordProbe(plume::RenderCommandList* list, const Plan&) {
  if (probe_busy_) {
    return;
  }
  HostImage& level = depth_[2];
  const uint32_t width = level.width;
  const uint32_t height = level.height;
  const uint32_t row_bytes = (width * 2 + 255) / 256 * 256;  // R16_FLOAT, rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!probe_buffer_ || probe_buffer_bytes_ < bytes) {
    probe_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes),
                                          "ao census readback");
    probe_buffer_bytes_ = probe_buffer_ ? bytes : 0;
    if (!probe_buffer_) {
      return;
    }
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(level.texture.get(),
                                             plume::RenderTextureLayout::COPY_SOURCE));
  level.layout = plume::RenderTextureLayout::COPY_SOURCE;  // the next DrawPass moves it on
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(probe_buffer_.get(),
                                                        plume::RenderFormat::R16_FLOAT, width,
                                                        height, 1, row_bytes / 2, 0),
      plume::RenderTextureCopyLocation::Subresource(level.texture.get()));
  probe_busy_ = true;
  ++probes_done_;
  const uint64_t frame = frame_;
  const uint64_t generation = generation_;
  const uint32_t row_texels = row_bytes / 2;
  frames_->RetireAction([this, frame, generation, width, height, row_texels] {
    ProcessProbe(frame, generation, width, height, row_texels, 2);
  });
}

void AmbientOcclusion::ProcessProbe(uint64_t frame, uint64_t generation, uint32_t width,
                                    uint32_t height, uint32_t row_texels, uint32_t level_shift) {
  probe_busy_ = false;
  if (generation != generation_ || !probe_buffer_ || width == 0 || height == 0) {
    return;
  }
  const void* mapped = probe_buffer_->map();
  if (!mapped) {
    REXLOG_ERROR("{} depth statistic of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  std::vector<float> values(size_t(width) * height);
  const uint16_t* source = static_cast<const uint16_t*>(mapped);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      values[size_t(y) * width + x] = HalfToFloat(source[size_t(y) * row_texels + x]);
    }
  }
  probe_buffer_->unmap();

  // Sky and distances over the whole picture, and over its thirds: outdoors the road at the bottom
  // of the picture is nearer than the middle (the guest's 1 near / 0 far, linearised with the
  // lens, gives that), and the sky is at the top.
  struct Third {
    uint64_t count = 0;
    uint64_t sky = 0;
    double sum = 0.0;
  };
  Third thirds[3];
  std::vector<float> sample;
  sample.reserve(16384);
  const size_t stride = std::max<size_t>(values.size() / 16384, 1);
  uint64_t sky = 0;
  for (uint32_t y = 0; y < height; ++y) {
    Third& third = thirds[std::min(3u * y / height, 2u)];
    for (uint32_t x = 0; x < width; ++x) {
      const float t = values[size_t(y) * width + x];
      ++third.count;
      if (!(t < 0.5f * kSkyDistance)) {
        ++third.sky;
        ++sky;
      } else {
        third.sum += double(t);
        if ((size_t(y) * width + x) % stride == 0) {
          sample.push_back(t);
        }
      }
    }
  }
  const auto share = [](uint64_t part, uint64_t whole) {
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
  };
  const auto mean = [](const Third& t) {
    return t.count > t.sky ? t.sum / double(t.count - t.sky) : 0.0;
  };
  float p[4] = {};  // min, median, 90th percentile, max
  if (!sample.empty()) {
    std::sort(sample.begin(), sample.end());
    p[0] = sample.front();
    p[1] = sample[sample.size() / 2];
    p[2] = sample[sample.size() * 9 / 10];
    p[3] = sample.back();
  }
  const double middle = mean(thirds[1]);
  const double bottom = mean(thirds[2]);
  const char* verdict =
      thirds[1].count == thirds[1].sky || thirds[2].count == thirds[2].sky
          ? "no ground to compare (an all-sky third)"
      : bottom < middle ? "the bottom of the picture is nearer than the middle: the guest's "
                          "1 near / 0 far holds"
                        : "the bottom of the picture is NOT nearer than the middle: look at the "
                          "convention (or this is not a street view)";
  REXLOG_INFO("{} depth statistic of frame {} (the linear depth chain's 1/{} level, {}x{}): sky "
              "{:.1f}%, distance min {:.2f} median {:.2f} 90th percentile {:.1f} max {:.1f}; top "
              "third {:.0f}% sky, mean distance of the middle third {:.1f}, of the bottom third "
              "{:.1f}: {}",
              kPrefix, frame, 1u << level_shift, width, height, share(sky, values.size()), p[0],
              p[1], p[2], p[3], share(thirds[0].sky, thirds[0].count), middle, bottom, verdict);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnPresent() {
  pause_spell_ = pause_view_ && !play_view_ ? pause_spell_ + 1 : 0;
  if (frame_ % kVehicleLogEvery == 0 || (!vehicles_first_logged_ && vehicles_.table_frames != 0)) {
    vehicles_first_logged_ = vehicles_first_logged_ || vehicles_.table_frames != 0;
    LogVehicleSummary();
  }
  if (census_every_ != 0 && frame_had_main_view_) {
    Census();
  }
  // Nothing to compute for a second (the occlusion off, the contact shadows off or gated to nothing):
  // the images go back (they are made again by the first trigger after).
  const bool contact_working = contact_mode_ != Mode::kOff && contact_state_ == ContactState::kActive;
  if (mode_ == Mode::kOff && !contact_working) {
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames &&
        census_every_ == 0) {
      ReleaseImages();
      images_made_for_[0] = images_made_for_[1] = images_made_for_[2] = 0;
    }
  } else {
    off_frames_ = 0;
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  stencil_ids_ = 0;
  frame_had_main_view_ = false;
  qualifying_resolves_ = 0;
  later_tiles_ = 0;
  wet_bins_.fill(0);
  wet_draws_ = 0;
  draws_before_ = 0;
  color_draws_before_ = 0;
  draws_after_ = 0;
  depth_writes_after_ = 0;
  draws_at_anchor_ = 0;
  view4x_draws_ = 0;
  light_count_ = 0;
  light_overflow_ = 0;
  light_draws_ = 0;
  depth_clears_ = 0;
  depth_resolves_.clear();
  other_resolves_ = 0;
  lens_delta_valid_ = false;
  vs_view_inverse_draws_ = 0;
  vs_compare_valid_ = false;
  have_camera_ = false;
  trigger_camera_valid_ = false;
  trigger_pause_ = false;
  trigger_world_ = menu_world::World::kNone;
  outcome_ = Outcome::kNoTrigger;
  ran_this_frame_ = false;
  applied_this_frame_ = false;
  ++frame_;
}

bool AmbientOcclusion::FrameLinearDepth(LinearDepthView* out) const {
  if (!ran_this_frame_ || !linear_depth_image_ || !linear_depth_image_->texture) {
    return false;
  }
  out->image = linear_depth_image_;
  out->tan_half_fov[0] = linear_depth_tan_[0];
  out->tan_half_fov[1] = linear_depth_tan_[1];
  return true;
}

menu_world::World AmbientOcclusion::FrameWorld() const {
  if (play_view_ == pause_view_) {
    return menu_world::World::kNone;  // no main view in the frame, or both kinds
  }
  if (play_view_) {
    return menu_world::World::kPlay;
  }
  // This frame's own camera (the anchor has come by the end of the frame), else the last one of the kind.
  const temporal::Camera* camera =
      have_camera_ ? &camera_ : (have_previous_ && previous_pause_ ? &previous_ : nullptr);
  return menu_world::Classify(true, menu_world::LensOf(camera), menu_world::GarageLighting());
}

void AmbientOcclusion::Levels(std::string* occlusion, std::string* contact, std::string* bounce) const {
  *occlusion = ModeName(mode_);
  *contact = ModeName(contact_mode_);
  if (contact_mode_ != Mode::kOff && contact_state_ == ContactState::kGated) {
    *contact += " (none now: the sun's height or the rain gates it)";  // PlanContact's gates
  }
  *bounce = !gi_active_ ? "off" : (gi_test_ ? "test" : ModeName(gi_mode_));
}

const char* AmbientOcclusion::ViewName() const {
  if (play_view_ && pause_view_) {
    return "play + pause/map (mixed)";
  }
  return pause_view_ ? "pause/map (4x)" : play_view_ ? "play" : "no main";
}

std::string AmbientOcclusion::TriggerText() const {
  const char* kind = trigger_pause_ ? "pause/map" : "play";
  switch (outcome_) {
    case Outcome::kNoTrigger:
      return "no resolve of the frame was the trigger: no occlusion";
    case Outcome::kIdle:
      return "a trigger, with the occlusion off and no depth statistic due: nothing recorded";
    case Outcome::kNoCamera:
      return std::format("a {} trigger, but no previous frame's camera of a {} view (the frame "
                         "before was not a consecutive {} frame with a camera): no occlusion",
                         kind, kind, kind);
    case Outcome::kLensUnknown:
      return "a pause/map trigger in the second frame of a spell (or of the setting being on): the "
             "lens' rate of change is not known yet, no occlusion for this frame";
    case Outcome::kLensChanging:
      return std::format("a pause/map trigger, but the lens changed by {:.1f}% between the last "
                         "two frames (limit {:.0f}%): no occlusion until it settles",
                         100.0 * trigger_lens_step_, 100.0 * temporal::kCutFov);
    case Outcome::kTooSmall:
      return std::format("a {} trigger, but the picture is smaller than {} pixels: no occlusion",
                         kind, kMinRenderSize);
    case Outcome::kNotRecorded:
      return std::format("a {} trigger, but a pipeline or an image could not be made (see the "
                         "errors above): no occlusion", kind);
    case Outcome::kRecorded:
      break;
  }
  const char* done = applied_this_frame_ ? "passes recorded and applied to the main view's colour"
                     : mode_ == Mode::kOff   ? "the depth passes only (the census probe), nothing "
                                               "applied"
                                             : "passes recorded but the apply pass could not be "
                                               "drawn (see the errors above)";
  if (!trigger_camera_valid_) {
    return std::format("a {} trigger, {}", kind, done);
  }
  const temporal::Camera& c = trigger_camera_;
  return std::format("a {} trigger, {} with the lens of the PREVIOUS frame's motion blur draw "
                     "(frame {}, a {} view, the step from the frame before {:.2e}): vertical field "
                     "of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}, camera at "
                     "({:.1f}, {:.1f}, {:.1f}); the sun direction uses that frame's view rotation",
                     kind, done, c.frame, kind, trigger_lens_step_,
                     c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane,
                     c.position[0], c.position[1], c.position[2]);
}

void AmbientOcclusion::Census() {
  const bool main_view = play_view_ || pause_view_;
  const bool anomaly = main_view && qualifying_resolves_ != 1 && census_anomalies_logged_ < 40;
  // The first four frames of every pause/map spell are logged whatever the interval: a spell is
  // short, and its first frames are where its camera comes from.
  const bool pause_entry =
      pause_view_ && !play_view_ && pause_spell_ <= 4 && census_pause_logged_ < 40;
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!anomaly && !periodic && !pause_entry) {
    return;
  }
  if (anomaly) {
    ++census_anomalies_logged_;
  }
  if (pause_entry) {
    ++census_pause_logged_;
  }
  ++census_frames_logged_;
  const std::string spell = pause_view_ && !play_view_
                                ? std::format(" (frame {} of a pause/map spell)", pause_spell_)
                                : std::string();
  REXLOG_INFO("{} frame {}{}{}: {} view, main view draws {} before the first qualifying depth "
              "resolve ({} with colour), {} after it before the anchor ({} of them write the main "
              "depth), {} after the anchor, 4x (pause/map) draws {}; qualifying resolves {} "
              "(expect exactly 1), later tiles of its picture {}; anchor draw {}; clears of the "
              "main depth {} (last z {}); resolves of other kinds {}",
              kPrefix, frame_, anomaly ? " (UNEXPECTED trigger count)" : "", spell, ViewName(),
              draws_before_, color_draws_before_, draws_after_, depth_writes_after_,
              draws_at_anchor_, view4x_draws_, qualifying_resolves_, later_tiles_,
              anchor_seen_ ? "seen" : "NOT seen", depth_clears_,
              depth_clears_ ? std::format("{}", depth_clear_z_) : std::string("none"),
              other_resolves_);
  uint32_t index = 0;
  for (const DepthResolveNote& n : depth_resolves_) {
    REXLOG_INFO("{}   depth resolve {}: caller {:#010x}, source {}, host sample {} of {}, "
                "destination {:08x} format {} {}x{}, rect ({}, {})-({}, {}), {}, {}, after {} main "
                "view draws, anchor {}: {}",
                kPrefix, index++, n.caller, DescribeEdramKey(n.source), n.host_sample,
                n.host_samples, n.dest_address, n.dest_format, n.dest_width, n.dest_height,
                n.rect[0], n.rect[1], n.rect[2], n.rect[3],
                n.inside_end_tiling ? "inside EndTiling" : "plain resolve",
                n.clears_depth ? "clears the depth after" : "no clear", n.draws_before,
                n.anchor_before ? "already drawn" : "not yet",
                n.refused == 0 ? "THE TRIGGER"
                               : std::format("not the trigger ({})", RefusalNames(n.refused)));
  }
  LightRaw picked;
  uint32_t with_it = 0;
  if (PickLight(&picked, &with_it)) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(picked, ambient, sun, dir);
    float first_a[3], first_s[3], first_d[3], last_a[3], last_s[3], last_d[3];
    DecodeLight(first_light_, first_a, first_s, first_d);
    DecodeLight(last_light_, last_a, last_s, last_d);
    REXLOG_INFO("{}   light constants: {} distinct values over {} colour draws before the trigger "
                "({} draws past the table); the commonest ({} draws): PS c26 ambient {}, PS c39 sun "
                "colour {}, VS c16 the direction the light travels {} (length {:.4f}); first draw: "
                "{} {} {}; last draw: {} {} {}",
                kPrefix, light_count_ + (light_overflow_ ? 1 : 0), light_draws_, light_overflow_,
                with_it, Vec3(ambient), Vec3(sun), Vec3(dir), Length3(dir), Vec3(first_a),
                Vec3(first_s), Vec3(first_d), Vec3(last_a), Vec3(last_s), Vec3(last_d));
    if (have_camera_) {
      // The direction to the sun, -c16, through this frame's view (row vectors, v' = v M, the
      // game's view space looks along -z): at noon the sun is overhead, so it is up in the world
      // (y > 0) when c16 is the direction the light travels.
      double view[3] = {};
      for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
          view[j] -= double(dir[i]) * camera_.view.m[i][j];
        }
      }
      const double ambient_luma = Luma(ambient);
      const double sun_luma = Luma(sun);
      REXLOG_INFO("{}   sun: c16 is the direction the light travels, y {:+.3f}, so the direction to "
                  "the sun (-c16) has world y {:+.3f} (+ is up: a sun overhead at noon gives +) and, "
                  "in the game's view space, ({:+.3f}, {:+.3f}, {:+.3f}); luma ambient {:.4g}, "
                  "sun {:.4g}: ambient share for a surface facing the sun {:.3f}, a surface in "
                  "shade 1.000",
                  kPrefix, double(dir[1]), -double(dir[1]), view[0], view[1], view[2],
                  ambient_luma, sun_luma,
                  ambient_luma + sun_luma > 1e-9 ? ambient_luma / (ambient_luma + sun_luma) : 1.0);
    }
  } else {
    REXLOG_INFO("{}   light constants: no colour draw of the main view before the trigger", kPrefix);
  }
  if (have_camera_) {
    const temporal::Camera& c = camera_;
    REXLOG_INFO("{}   lens (the game's motion blur draw, this frame): vertical field of view {:.3f} "
                "deg, aspect {:.4f}, near {:.4f}, far {:.1f}, c62 {}x{}, camera at ({:.1f}, {:.1f}, "
                "{:.1f}) looking ({:+.3f}, {:+.3f}, {:+.3f})",
                kPrefix, c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane,
                c.far_plane, c.width, c.height, c.position[0], c.position[1], c.position[2],
                c.forward[0], c.forward[1], c.forward[2]);
    if (lens_delta_valid_) {
      REXLOG_INFO("{}   the previous frame's lens (what the trigger uses) against this one's: "
                  "relative change fov {:.2e}, aspect {:.2e}, near {:.2e}, far {:.2e}; camera moved "
                  "{:.3f}, cosine of the turn {:.6f}",
                  kPrefix, lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3],
                  lens_delta_[4], lens_delta_[5]);
    } else {
      REXLOG_INFO("{}   no previous frame's camera of this kind of view (the frame before was "
                  "not a consecutive {} frame with a camera): the trigger gives no occlusion this "
                  "frame",
                  kPrefix, pause_view_ ? "pause/map" : "play");
    }
  } else {
    REXLOG_INFO("{}   no camera this frame ({})", kPrefix,
                anchor_seen_ ? temporal::RejectName(reject_) : "no anchor draw");
  }
  if (vs_compare_valid_) {
    REXLOG_INFO("{}   vertex bank: VS c12-c15 of the last colour draw before the trigger "
                "against the view inverse the anchor reads from PS c12-c15: largest entry "
                "difference {:.3e} ({}); frames compared so far: play {} of {} equal, pause/map {} "
                "of {} equal (where they agree, the frame's own camera exists in the vertex bank "
                "before the anchor)",
                kPrefix, vs_compare_diff_,
                vs_compare_diff_ <= 1e-4 ? "equal" : "DIFFERENT", vs_equal_[0], vs_compared_[0],
                vs_equal_[1], vs_compared_[1]);
  }
  REXLOG_INFO("{}   trigger: {}", kPrefix, TriggerText());
  REXLOG_INFO("{}   history: this frame's trigger {}{}; frames that used a history {}, did not: first {}, "
              "gap {}, view {}, size {}, kind {}, cut {}, cap {}; motion from the vertex constants {}, the "
              "last step {}, standing still {}; the vertex constants {} as the frame's camera",
              kPrefix,
              history_why_ == HistoryWhy::kUsed
                  ? std::format("used the history, camera motion from {}", PoseName(history_pose_))
                  : std::format("had no history: {}", HistoryWhyName(history_why_)),
              ran_this_frame_ ? "" : " (no passes recorded this frame)", history_used_,
              history_resets_[size_t(HistoryWhy::kFirst)], history_resets_[size_t(HistoryWhy::kGap)],
              history_resets_[size_t(HistoryWhy::kView)], history_resets_[size_t(HistoryWhy::kSize)],
              history_resets_[size_t(HistoryWhy::kKind)], history_resets_[size_t(HistoryWhy::kCut)],
              history_resets_[size_t(HistoryWhy::kCap)],
              pose_frames_[size_t(Pose::kVertexBank)], pose_frames_[size_t(Pose::kPredicted)],
              pose_frames_[size_t(Pose::kStill)],
              vs_pose_trusted_ ? "are trusted" : "are not trusted yet");
  REXLOG_INFO("{}   vehicles: IDs the main view wrote before this point {:#08x}; the last anchor "
              "(frame {}) gave a usable motion for {:#08x}; table rows drawn so far {} in {} frames",
              kPrefix, stencil_ids_ & ~1u, vehicle_motion_frame_,
              vehicle_motion_.valid, vehicles_.table_rows, vehicles_.table_frames);
  REXLOG_INFO("{}   occlusion: {} (mcla_gpu_ao), passes {} this frame, {} runs in all, {} frames "
              "without a previous frame's camera; pause/map view: {} runs, {} triggers without "
              "a pause/map camera, {} held back for the lens (second frame or a sweep); the depth "
              "statistic follows when the GPU has run its copy",
              kPrefix, ModeName(mode_), ran_this_frame_ ? "recorded" : "not recorded", runs_,
              skipped_no_lens_, pause_runs_, pause_skipped_no_lens_, pause_skipped_lens_);
  if (contact_mode_ != Mode::kOff) {
    float wet = 0.0f;
    const bool have_wet = PickWet(&wet);
    const char* state = contact_state_ == ContactState::kActive    ? "active"
                        : contact_state_ == ContactState::kGated   ? "gated to nothing"
                        : contact_state_ == ContactState::kNoLight ? "no light constants"
                                                                   : "off";
    REXLOG_INFO("{}   contact shadows: {} (mcla_gpu_contact_shadows), {}; sun at {:.1f} degrees (gate "
                "{:.2f}), wetness VS c43.x {} (gate {:.2f}), strength {:.2f}; the game's shadow "
                "mask: a resolve into its page {} this frame; frames with contact shadows so far: "
                "{} with the mask, {} without it, {} in all",
                kPrefix, ModeName(contact_mode_), state, contact_elevation_, contact_gate_sun_,
                have_wet ? std::format("{:.1f} ({} draws)", wet, wet_draws_) : std::string("not read"),
                contact_gate_wet_, contact_strength_,
                mask_frame_ == frame_ ? "came" : "did not come", contact_mask_frames_,
                contact_no_mask_frames_, contact_runs_);
  }
  if (gi_active_ || gi_runs_ != 0) {
    REXLOG_INFO("{}   bounce light: {} (mcla_gpu_ssgi), the test {} (mcla_gpu_ssgi_test), frames recorded "
                "so far {}, {} of them with a history to lean on ({} frames at most, clip {:.2f} "
                "deviations, {} a-trous iterations); images held {:.1f} MiB",
                kPrefix, ModeName(gi_mode_), gi_test_ ? "on" : "off", gi_runs_, gi_history_runs_,
                uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history), 2, 64)),
                std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history_clamp), 0.25, 8.0),
                uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_atrous), 0, 3)),
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
}

}  // namespace mcla::gpu
