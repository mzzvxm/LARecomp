#pragma once
// ===========================================================================
// MCLA GPU renderer -- screen-space ambient occlusion
// ===========================================================================
// The game has baked vertex occlusion in the world and a contact shadow under cars, and nothing that
// darkens creases, kerbs and wheel arches. This adds a screen-space term of its own: GTAO after
// Intel's XeGTAO (MIT) written as pixel shaders (hlsl/ao_*_ps.hlsl), applied as a weighted multiply
// on the scene colour.
//
// WHERE: at the game's mid-scene depth resolve (the first resolve per frame of the main view's depth,
// outside EndTiling, into 078B0000): after every opaque draw of the main view, before the soft
// particles, the rain, the fog quad and the post chain, so fog and particles cover the darkened scene
// as they cover everything else, and a temporal upscaler (temporal.h) sees the occlusion as part of
// the colour it accumulates. The depth is complete there and is read straight from the EDRAM depth
// target, guest sample 0 (the sample the game's own particle and fog reads see). The game resolves
// that depth in tiles (two: the rows 0-512 and 512-720, a leftover of the 360 drawing the frame once
// per tile), but the renderer draws the frame once, so the target is whole at the first resolve: the
// occlusion is worked out over the whole picture there (the destination texture's size, not the
// tile's), and the later tiles' resolves of the same picture are not triggers.
//
// WHAT: five passes into images of their own, then one into the main view's colour target:
//   depth    the linear view-space distance, R32_FLOAT, at the AO's size (the render size, or half of
//            it at LOW), and two coarser levels, R16_FLOAT; the lowest bit of each level-0 depth says
//            whether the pixel is a vehicle (the stencil IDs 1 to 23)
//   main     the GTAO visibility (R16_FLOAT), its noise changing every frame; what a car blocks of the
//            sky light reaches a share of the radius on the road (mcla_gpu_ao_car_reach), not all of
//            it, so the road does not get a halo round every car, and the sum over the slices is
//            divided by what it is for a surface nothing blocks (an open road is 1, not 0.9)
//   denoise  a depth-aware 3x3 blur of it (R16_FLOAT): once when the occlusion has a history of its
//            own, twice when it has none; never left out
//   temporal the accumulation over frames: this frame's visibility averaged with the last frame's,
//            found again by the camera's own motion and kept only where it is the same surface
//            (see below); R16G16_FLOAT = (visibility, linear depth), the next frame's history
//   apply    rgb *= lerp(1, ao, strength * ambient_share * distance_fade) on every sample of the
//            colour target; ambient_share = La / (La + Ls * N.L) from the game's own constants (PS c26
//            the ambient, PS c39 the sun colour, VS c16 the direction the sunlight travels: the
//            direction to the sun is its opposite), N from the depth; the look's direct number
//            moves the share towards 1 (the occlusion then darkens the sun-lit part too)
// The lens (the projection) is the previous frame's: the camera exists only at the game's motion blur
// draw, which comes after the trigger (OnAnchor keeps each frame's). A frame without a previous
// frame's camera has no occlusion.
//
// The pause menu's, the race pause's, the garage's and the map's world (the same main view identities at 4x
// samples, drawn once a frame like the play view, resolved in three tiles; menu_world.h says which is which and
// what runs in each) is handled the same way,
// with its own camera: its motion blur draw runs in every one of those frames with a valid camera
// (918 of 918 frames in a recorded pause and map session), so the camera kept is the previous
// frame's of a pause/map view, never the play view's (the pause camera has another field of view,
// 53.75 degrees against 25). The first two frames of a pause spell have none (the second has no
// rate of change of the lens yet), and neither has a frame after a sweep of the lens (the camera's
// swing when the pause opens, the map): a change of the setting in the pause menu shows a couple of
// frames later, which looks immediate, without closing the menu. The trigger there is the same
// resolve (into 078B0000, the first of the frame); the host draws the 4x target once, so it is
// whole at the first tile.
//
// THE HISTORY (hlsl/ao_temporal_ps.hlsl): the sampling noise changes every frame and the
// occlusion averages it over frames, as the games' own do (mcla_gpu_ao_history: the frames, 12 by
// default; an exponential average once that many have been added up, the plain mean before). The
// game has no motion vectors for what moves by itself, so a pixel is found in the last frame from
// the camera alone (a vehicle's pixels also with the vehicle's own motion, below), and the history is
// trusted only where it is the same surface: the plane test and the depth test of the shader, a
// clamp to this frame's neighbourhood against ghosts. What it cannot trust gets the wide blur of
// this frame in its place. The
// camera of the trigger's own frame is not known when it fires (the anchor draw comes later), so
// the motion into the last frame is either the frame's own view inverse read from the vertex
// constants of its draws (c12-c15: trusted only once it matched the anchor's camera in a frame the
// camera moved in) or the last step repeated (the constant velocity guess); FrameMotion. In a 45
// second drive the vertex constants were trusted in 3610 of 4736 history frames, and the rest used
// the last step. The history starts again after a cut (the temporal system's own rule on the
// camera), a gap of frames, the pause/map view switching and a new image size. One history serves
// both views: switching is a reset (the pause view's first frame has no occlusion anyway).
//
// THE VEHICLES (vehicle_motion.h): what moves by itself cannot be found again from the camera, and
// a car's own pixels (the stencil ID 1 to 23 of the depth target) used to get no history at all, only
// a wide blur of one frame whose noise changes every frame, which showed as a moving film grain
// in the occluded places of a car (under a spoiler, round the rear window). They keep their history
// now, found with the vehicle's own motion on top of the camera's. At each anchor the game's matrices
// c(69 + 4 ID) give, for every ID the main view wrote that frame, the vehicle's rigid motion over the
// frame (ReadVehicleMotion; alive with the motion blur off, and with or without an upscaler, which
// keeps its own table of the same numbers). The next frame's trigger comes before its own anchor, as
// the camera's does, so it repeats that step (the constant velocity guess: the vehicle moves the way
// it did) and composes it with the camera's motion into one 3x4 per ID, drawn into a small table
// image (RecordVehicleTable) that the temporal pass reads by the pixel's stencil ID. An ID without a
// usable row keeps no history, as before. IDs are handed out per frame in draw order, so a car has no
// stable ID: when one changed owner, or the car moved differently, its pixels land on a texel that
// fails the plane, depth or clamp test and get the wide blur. The pause and map view moves nothing by
// itself: its vehicle pixels take the camera's motion like the world's.
//
// CONTACT SHADOWS (hlsl/ao_contact_ps.hlsl, mcla_gpu_contact_shadows): the game has no shadow for the
// small things of the world (its sun shadows are four cascades of 1 to 25 cm texels, 4-tap filtered,
// and at 6 h, 18 h and in rain none at all). They are found here from the same depth chain, at the
// same trigger, with the same camera and light constants as the occlusion, and applied in the same
// apply pass (hlsl/ao_apply_ps.hlsl), so they work with the occlusion off, and the occlusion with
// them off. A ray toward the sun is marched in view space over the linear depth over a short length
// (0.6 m, longer when the sun is low: 0.7 / tan(elevation), 1 to 3 times; 8 steps at low, 16 at
// high, closely spaced near the contact and wider apart toward the ray's end), the result is cleaned
// by one pass of the occlusion's denoise shader in its contact mode (a 3x3 bilateral whose weights
// also fall with the difference of the visibility, so noise goes and edges stay; the contact term has
// no history of its own: the noise rotates every frame while a temporal upscaler runs and averages
// it, and is a fixed pattern without one), and the
// apply pass darkens only the SUN's share of the light by it:
//   sun_share = Ls * N.L * vis / (La + Ls * N.L * vis)
//   factor    = 1 - sun_share * strength * (1 - contact) * distance_fade
// where vis is the game's own sun shadow mask (the 1280x720 k_8_8_8_8 resolve product at 06ACD000,
// the collector's output which the lit shaders read, stored as half the visibility: x2) in the
// frames that have one (8 h to 17 h by day), so nothing the game already shadows is darkened twice,
// and 1 where the game has none (6 h, 18 h and rain: the contact shadows are then the only shadow of
// the sun there). The strength has two gates, both from the frame's own constants: the sun's
// elevation (from VS c16: none at 3 degrees and below, all from 12: the night's light points up
// and 19 h is at 2.5 degrees with a grey sun) and the wetness (VS c43.x, gReflectionParams.x: 0.095
// dry, 1.17 in rain: the overcast key light has no shadows to draw). A gate of 0 records nothing:
// the frame is the one without the setting.
// With the setting on and the occlusion off the passes are the depth pass, the contact march, one
// filter pass and the apply: the coarser depth levels, the visibility and the history are not
// made. The pause menu's world, the garage and the map get them as the occlusion does.
// SHARPNESS (contact_trace.h has the rules and the reasons, contact_trace_test.cpp checks them): the
// images are the render size at every render scale (a scale of 3 traces 3840x2160 pixels; only the
// low setting with the occlusion not on high works at half of it, and the occlusion's own images being
// traced at half size above 1080 lines leaves the contact shadows' at the render size: see "THE SIZE OF
// THE PASSES" below), so what made the first version
// soft was the look of its trace, set at 1280x720 in world units: a ray-end fade that lightened the
// shadow all along the ray, a depth bias and ramp of 6 cm at 10 m (10 pixels at 720p, 30 at 4K), two
// plane-only blurs and equally spaced steps. The tolerance is now a few millimetres plus a few
// footprints (the world size of a pixel at that depth), a shadow stays full over the first 60% of
// the ray (mcla_gpu_contact_shadows_fade), the filter is one pass that keeps edges, and the cost is
// logged as a model per size (the march is unchanged, the filter one pass instead of two).
// THE CHEAPER PATH (mcla_gpu_contact_shadows_fast, debug-only, on by default; off is every pass as described above):
// (1) the march takes as many steps as the ray's length on the screen needs, never fewer than 4 and never more than
// the setting's (contact_trace.h AdaptiveSteps: the squared spacing is kept, the gap between the last two steps is at
// most 8 pixels): at 3840x2160 that is nearly all 16, at 1080p about 15, at 720p about 14; (2) where the passes run at
// the picture's size (not at half size) the 3x3 filter is done by the apply pass over the march's own image
// (hlsl/ao_apply_ps.hlsl, AoBilateralEdgeFolded: the same weights, with the neighbours' depths the apply reads for its
// normal; flag bit 25 there), so the filter pass and the write of its full size target are gone, and a pixel the contact
// shadows cannot darken (the game's own shadow, a surface turned from the sun) skips the filter. The apply pass is
// compiled the same for both states, so the lever is live and needs no new pipeline. The model (contact_trace.h
// CostModelFast, not measured) is in the log line.
// (3) the march is skipped under the game's own shadow (hlsl/ao_contact_ps.hlsl THE SKIP, contact_trace.h "the skip under
// the game's own shadow"): where the sun shadow mask (a frame that has it, the picture's size, no debug picture) is 0 over
// everything the apply's filter can read for a pixel, the apply cannot use the march's result (its weight, strength * sun
// share * fade, is 0 there, and below the filter's threshold the apply reads no neighbour), so the march returns 1 before
// its depth reads. Exact, not an approximation: the check compares the factor bit for bit with and without it. The mask is
// found before the march and the apply gets the same slot; the gain and the strength it is given are the apply's.
// It saves what the shadow's stored value being 0 lets it: CHECK that a pixel the game shadows stores 0 (view 10 black).
// CHECK: (none of this could be run) (1) the mask's polarity and range: the collector writes
// max(x) with x = z + (0.5 - z) * lit (PS_ShadowBlend, c253.x = 0.5, ShaderGlobals.z = PS c63.z), so 1 is
// lit and a lit pixel stores up to 0.5; the lit shaders' own use (x2) is from AO-DESIGN, not read in
// the road shaders: mcla_gpu_contact_shadows_view 3 must be white in the sun and black in a car's
// cascade shadow, else change mcla_gpu_contact_shadows_mask_gain; (2) that the mask product is still
// active and in the shader-read layout at the trigger (the log says once if not); (3) VS c43.x as the
// frame's wetness (0.095 dry, 1.17 in rain: census 6A.2 item 4) on the draws before the trigger;
// (4) in the pause/map view (no census of whether the collector runs there: the sun is then not
// shadowed by the mask and the contact shadows go on everything lit); (5) shimmer in motion without
// an upscaler (no history of their own: if visible, run ao_temporal_ps over a second history).
//
// THE SIZE OF THE PASSES (ao_size.h has the rules and the memory they come to, ao_size_test.cpp checks them; debug cvars
// mcla_gpu_ao_half_above_1080 and mcla_gpu_ssgi_half, both on by default, off is every size as it was). The occlusion's main pass takes
// the same 18 samples a pixel at every size, so what it makes has no more detail than a 720 line picture's, and the blur (ao_stable.h) that
// follows takes the rest: above 1080 lines of render height (1440p, 4K, an upscaler's larger render size) the occlusion's and the bounce
// light's passes (depth chain, visibility, blur, history, a-trous) are traced at HALF the picture's size, 720 lines for 1440p and 1080 for
// 4K, and the apply pass puts them back with its depth-aware 2x upsample (VisibilityAt, GiAt). 1080 lines and below are unchanged. The
// radius of the blur and the number of a-trous passes follow the TRACED image's height (plan.ao[1]), the history is dropped when the size
// changes (a toggle of the cvar, a change of the render size), and the temporal pass takes a half size pixel for a vehicle where the depth
// pass marked it (ao_temporal_ps.hlsl AoVehicleId). The half size depth takes the MEAN of a 2x2 block where its four samples are one
// surface and the sample nearest to the mean across an edge (ao_depth_ps.hlsl): the old rule's corner sample put every surface seen at a
// grazing angle half a pixel off its centre and the occlusion came out 0.04 darker on a road and a wall. The contact shadows keep the size
// they had (the render size where the occlusion on high or they on high pinned it): the depth pass is then drawn twice, the second time at
// the picture's size into depth_full_ (Plan::SplitContact) for their march and the apply's filter, and they have their own images
// (contact_raw_, contact_dn_) because the occlusion's scratch images are half size. The visibility passes' two scratch images are only held
// where something reads them (the plain occlusion, the contact march and its filter pass). The bounce light's colour chain takes ONE MSAA
// sample of each pixel of a 2x2 block where its size is half by the height rule (ao_color_ps.hlsl AoOneSample).
// MEASURED (software rasteriser, the real passes, a road, a wall, a car, a kerb and two poles, 13 frames of the noise lattice): the final
// AO factor at half size against full size is 0.002 (4K) and 0.003 (1440p) on average, 99% of the pixels within 0.012 and 0.017, bias -0.001,
// and what differs by more than 0.05 (0.1% of the pixels) is the kerb's 15 cm foot and the far pole: thin geometry. One frame is noisier
// (1.8 times at 1440p and 4K: a 3x3 / 5x5 blur over a quarter of the pixels), and the history takes that out.
// CHECK: (none of this could be run in the game) 1440p and 4K, parked and moving, the occlusion's debug views (mcla_gpu_ao_view 1, 2, 6, 7, 12):
// the tyre's contact line, a kerb's foot, poles, the base of a wall, a car's silhouette against the road; the apply's cost (its upsample
// reads 4 + 4 texels a pixel at half size; the "ao apply" pass label); the "ao contact depth" label (the extra depth pass).
//
// SCREEN-SPACE GLOBAL ILLUMINATION, FIRST STEP (hlsl/ao_color_ps.hlsl, hlsl/ao_main_ps.hlsl compiled
// with AO_GI; mcla_gpu_ssgi_test, off by default and debug-only: nothing of it runs for a player, and
// with it off every pass, push block and image of the occlusion is the one above, to the bit): the
// occlusion's main pass gets a second output, what colour the blockers are. At the trigger the main
// view's colour target (the opaque scene, before the fog and the particles) is averaged over its MSAA
// samples into a colour chain, RGBA16F at the passes' size and two coarser levels (one pass a level, as
// the depth chain's), each sample cleaned and capped at a multiple of the frame's ambient + sun
// (mcla_gpu_ssgi_test_clamp; a lamp or neon is the game's own light, not a surface that bounces it).
// The main pass is then the visibility bitmask version of the horizon search: 32 sectors of the slice's
// arc, the bits a sample newly sets add its radiance from the chain (weighted by how much its surface
// faces the pixel), the unset bits are the visibility. It writes one RGBA16F image, (visibility,
// bounce r, g, b): the occlusion's denoise and history take the visibility from it (the first denoise
// pass reads its x channel in place of the GTAO pass's image), the bounce is only shown, by the debug
// views of mcla_gpu_ssgi_test_view (the bounce, the chain's levels, a 5x5 mean of the bounce). Nothing
// is added to the picture yet: the history and denoise of the bounce, the albedo estimate that turns it
// into light and the setting are the next steps (design: feature-parity/GFX-UPGRADES-DESIGN.md 8.3,
// packages G7b and G7c). It needs the occlusion on and the main view's colour target at the depth's
// samples and scale; when anything it needs is missing the frame is the plain occlusion (logged once).
// CHECK: (none of this could be run) (1) the picture of a flat plane is unchanged and an inner corner's
// bounce is the neighbour's colour (debug view 1 or 5 at a wall base, with the sun low); (2) with the
// thickness at 0 (a solid) the visibility is the GTAO pass's to within 1/32: mcla_gpu_ao_view 2 with
// the test on against off; (3) the main view's colour target is readable at the trigger in the
// layout the transition gives it (the sun shafts read it there too).
//
// SECOND STEP: THE BOUNCE'S HISTORY AND DENOISE (hlsl/ao_denoise_ps.hlsl and hlsl/ao_temporal_ps.hlsl compiled
// with AO_GI, hlsl/ao_atrous_ps.hlsl; the same test cvar): one frame of the bounce is noisy (a handful of
// samples' colours, a different handful every frame), and the test now makes it steady the way the
// occlusion makes its visibility steady, on four channels at once (visibility x, bounce yzw):
//   denoise   the occlusion's depth-aware blur (3x3 at 720 lines, 5x5 or 7x7 in a bigger picture: ao_stable.h
//             DenoiseRadius) over the four channels (gi_ into gi_dn_); a frame with
//             no history to lean on blurs a second time (gi_dn_ into the idle history image)
//   temporal  the occlusion's own accumulation pass over four channels, into RGBA16F history images
//             that take turns. Everything the occlusion's history decides is the same decision here:
//             the camera's motion into the last frame (FrameMotion), the vehicles' rows in the table
//             (RecordVehicleTable), the plane and depth tests and the confidence, the wide blur where the
//             history does not hold, and the reasons for no history at all (a cut, a gap of frames, the
//             other kind of view, a new size, mcla_gpu_ao_history 1; plus the history being the other
//             kind: the test turned on or off). The last frame's linear depth is not in the history (the
//             plain history's second channel has no room now) but in an R32_FLOAT image of its own: the
//             depth image of the frame before, which depth_[0] and depth_prev_ trade places for at the end
//             of each frame. The visibility's part is to the last operation the plain pass's (its weight is
//             mcla_gpu_ao_history frames); the bounce averages over mcla_gpu_ssgi_test_history frames
//             and its history is clipped to a box round the mean of this frame's 3x3 neighbourhood,
//             mcla_gpu_ssgi_test_history_clamp standard deviations wide (never under 5% of the mean, and 15% of the
//             history's own value wider on each side: ssgi_stable.h has why the first 1.25 deviations alone made the
//             accumulated bounce 2.5 to 3.5 times as noisy): a light that changed is a ghost only until the history is
//             clipped to it.
//   a-trous   mcla_gpu_ssgi_test_atrous iterations (0 to 3, 2 by default at 720 lines, one more for each doubling of
//             the image's height: ssgi_stable.h AtrousIterations) of an edge-avoiding a-trous filter
//             of the bounce over the history (5x5 B3 spline, taps 1, 2, 4 pixels apart, each weighed by
//             its distance from the pixel's tangent plane): the history image itself stays unfiltered, the
//             filtered copy goes into gi_dn_ and the idle history image, and is what the picture's bounce
//             will be made of; mcla_gpu_ssgi_test_view shows the history (7), the filtered bounce (8) and
//             the weight each pixel kept of the history (9). Still nothing is added to the picture.
// While the test is on the occlusion's own RG16F history is not made (its images are given back): the apply
// pass reads the visibility from the x channel of the RGBA16F history instead.
// A model of the accumulation and the filter on a flat plane (ports/diffs/g7b-history-emu.py) chose the
// defaults: a history of 16 frames and a clip 1.25 deviations wide, which leaves 3 to 13% of noise after the
// history and 1 to 5% after the filter (from 30 to 100% of the light before), and a doubled light at 90% in 5
// to 14 frames.
// CHECK: (none of this could be run) (1) no crawl and no lag on a moving car with an upscaler on (DLSS, FSR):
// view 7 against view 8 on a drive, and view 9 (the car's own pixels white while its table row holds); (2)
// the filtered bounce keeps the edge of a wall's foot (view 8 at the corner); (3) the visibility is the
// plain occlusion's to within the bitmask (the history's x channel against mcla_gpu_ao_view 2 with the
// test off).
//
// THIRD STEP: THE BOUNCE LIGHT IN THE PICTURE (hlsl/ao_apply_ps.hlsl compiled with AO_GI; the player's setting
// mcla_gpu_ssgi, and still the debug override mcla_gpu_ssgi_test): the apply pass reads the filtered bounce
// (the a-trous output: x the visibility the history kept, yzw the bounce) and the colour chain's level 0
// and makes the factor one a colour channel,
//   factor.rgb = ao_factor * contact_factor + strength * B.rgb / max(T, luma(colour)) * (1 - capped) * fade
// kept under a limit (a share of the pixel's own colour). T is the light the game's constants say fell on the
// pixel (La + Ls N.L, the sun's term times the game's shadow mask where the frame has one), so the pixel's
// own colour divided by T is the albedo the technique has no G-buffer for, and colour x B / T is albedo x B:
// the same multiply (dst.rgb *= src.rgb, the occlusion's blend) carries it, no second pass. Replace, not add:
// the occlusion took the blocked share of the ambient away, the bounce gives back what the blockers really
// send, in their colour; a flat surface (B near 0) keeps the occlusion's factor. A pixel brighter than T says
// (a lamp's pool, headlights) is held to an albedo of 1, a source of light (the chain's alpha: neon, a
// glint, the sun's disc) gets nothing, and so does everything on the map (the 4x view seen from 9 km runs the
// plain occlusion: the bounce's few metres are under a pixel there; menu_world.h). The pause menu's world and the
// garage have the bounce light as the game view does. The setting's low is the main pass with 2 slices and 2 steps (the
// occlusion's low), at half the picture's size where nothing else asks for the whole of it (the occlusion
// low); high is 3 and 3 at the picture's size. `auto` is gpu_class.h's table (off on Low and Mid, low on High).
// The cost of a low that leaves the occlusion at full size is a guess (about the design's high at 4K, the
// history, denoise and a-trous passes being four channels at that size): mcla_gpu_ssgi_low 1 (half size) and
// 2 (both) are the cheaper ones to be measured, at the price of the occlusion's own picture being half size
// too while the bounce light is on (the same samples give both).
// With the setting off and the test off every pass, push block and image is the plain occlusion's, to the bit.
// The images go back 10 seconds after the last frame that used them (a pause spell does not make them again).
// CHECK: (none of this could be run) (1) the pictures of GFX-UPGRADES-DESIGN.md 8.3 (night with lamp pools and
// neon beside walls and car sides, sunlit asphalt beside a car, a coloured wall) off, low and high at the same
// frame: colour spill, no brighter picture on average (mcla_gpu_ssgi_view 21 against the plain occlusion's
// pixels), flat ground unchanged, the same in the pause menu's world and the garage, nothing on the map; (2) the
// albedo estimate (view 18) is the surface's
// colour and neon is black in view 20; (3) the mask's slot is readable in the apply at the trigger (the contact
// shadows read it there too).
//
// FOURTH STEP: A STRONGER BOUNCE LIGHT, AND LEVELS OF IT (ssgi_light.h and ssgi_level.h have the maths and the
// numbers; hlsl/ssgi_common.hlsli, ao_color_ps.hlsl, ao_main_ps.hlsl and ao_apply_ps.hlsl the same in the
// shaders). The third step's bounce was nearly invisible (owner, 2026-11-13: "even strength 2.0 with high shows a
// minimal difference") for three reasons: the factor was kept to half of the pixel's own colour, so a shaded pixel,
// where the bounce matters most, could gain next to nothing; the gather looked only as far as the occlusion's own
// radius (1 to 3 m, the same samples give its visibility) while a shaded street's light comes from the sunlit road
// and walls 4 to 10 m away; and the strength stopped at 2 with a default of 0.75 (the owner, playing with the debug
// cvars at their most, limit 4, radius x3 and strength 2: "believable, still kinda subtle"). Now:
//   cap       what a sample may carry into the bounce is a soft clip of its luma at `cap` times the frame's ambient +
//             sun (never under 0.5), and a pixel's own colour counts as a source of light (no bounce) from twice that
//             light on, ramped; the bounce may fill a pixel up to the cap's light (a ceiling) and not past it, so a
//             shadow gets lighter and nothing gets whiter than the sun-lit white of the frame. The per-pixel limit
//             is a soft guard of several times the pixel's own colour (2 to 10) against the noise of a dark pixel.
//   reach     a second ring of samples in each slice from the near samples' end out to the level's reach (5 to 16 m),
//             1 or 2 steps a side (low, high) from the coarser levels of the chains, with the horizon falloff at the
//             reach. The ring only adds light, into the sectors the near samples left open: the visibility is
//             counted from the near mask alone, so the occlusion looks as it did.
//   levels    mcla_gpu_ssgi_intensity (light, default, strong, stronger, strongest; the pause menu's BOUNCE LIGHT
//             STRENGTH row, live) is a set of strength, limit, reach and cap; the debug cvars mcla_gpu_ssgi_strength /
//             _clamp / _reach / _test_clamp / _far_steps each replace one number (-1: the level's).
// Cost: the ring adds 2 samples a side of each slice at high (3 slices: 12 more to the 18) and 1 at low, from
// coarse levels; the colour chain and the apply a few ALU. CHECK: measure the bounce light's passes (the "ssgi
// visibility" label) against the third step's at the same frame; judge the pictures of every level at dawn, noon,
// dusk and night (chain-ssgistrong.ps1): a sunlit road's light in the shade of a wall, a coloured wall's colour on
// the car beside it, neon and lamps not whitening the walls, no halo at a silhouette's edge from the far ring.
//
// The census (mcla_gpu_ao_census, off by default) is a diagnostic log: the trigger, the light
// constants, the lens, what the history did and, at its frames, a statistic of the linear depth
// read back from the GPU.
//
// Settings: mcla_gpu_ao (off, low, high; live; the pause menu's AMBIENT OCCLUSION row),
// mcla_gpu_contact_shadows (auto, off, low, high; live; the CONTACT SHADOWS row; auto is off on an
// integrated GPU, low on a discrete one with under 5.5 GiB, high on the rest; gpu_class.h) with the tuning cvars
// mcla_gpu_contact_shadows_length / _strength / _thickness / _fade / _mask_gain / _view / _fast, and
// mcla_gpu_ao_intensity (light, default, strong, max; live; the AO INTENSITY row, grey while the
// occlusion is off). An intensity is four numbers: strength, radius (in the game's world units,
// about metres), power (the visibility's exponent) and direct (how much the sun-lit part is
// darkened too). Also mcla_gpu_ao_history (the frames averaged), _fade_end, _view (the debug
// views) and _census (a diagnostic log). Bounce light: mcla_gpu_ssgi (auto, off, low, high; live; the pause
// menu's BOUNCE LIGHT row, grey while the occlusion is off) and mcla_gpu_ssgi_intensity (light, default, strong,
// stronger, strongest; live; the BOUNCE LIGHT STRENGTH row, grey while the bounce light is off) with the tuning
// cvars mcla_gpu_ssgi_strength / _radius / _clamp / _reach / _far_steps / _view (debug-only). The SSGI test, the debug override that forces the passes on at the
// occlusion's own quality whatever the setting says: mcla_gpu_ssgi_test and its tuning and view cvars
// mcla_gpu_ssgi_test_thickness / _clamp / _facing / _view / _view_gain / _history / _history_clamp /
// _atrous (all debug-only).
// The push block stays 128 bytes: power rides in shape.x, direct in the free lane extra.z.
//
// Threading: render thread only; Initialize and Shutdown on the lifecycle thread. The pipelines are
// made ahead on the host builder thread (host_precache.h), and only those the settings in force at the
// start use (host_plan.h: the occlusion's own groups when it is on, the depth, denoise and apply
// passes and the contact march when the contact shadows are, the bounce light's own passes (and its
// shaders) when it is on, nothing for what is off). A setting switched on later lists what it lacks in
// the background (HostPrecache::AddLive); until every pipeline is made the pass is not drawn
// (HoldUntilBuilt), so no frame waits for a compile. The contact shadows and the bounce light are one
// apply pass when both run (the apply with the bounce light multiplies by the contact factor too).
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "ao_size.h"
#include "constants.h"
#include "edram.h"
#include "host_passes.h"
#include "host_plan.h"
#include "menu_world.h"
#include "records.h"
#include "resolve.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "vehicle_motion.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class PassTiming;

class AmbientOcclusion {
 public:
  enum class Mode : uint8_t { kOff, kLow, kHigh };

  // The destination of the game's mid-scene depth resolve in the retail build (k_24_8_FLOAT,
  // 1280x720, the product the soft particles and the fog quad read), as the other identities of
  // temporal_identities.h: a different build gives another address and then there is no AO, never a
  // wrong picture.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  // What the Scene tells of a prepared draw of the main view (temporal_identities.h MatchMainView).
  struct MainDraw {
    temporal::MainMatch match;
    bool writes_depth = false;  // the pipeline writes depth
    // The stencil IDs the draw writes (bit k; temporal::StencilIdsWritten), for the vehicles' motion.
    uint32_t stencil_ids = 0;
  };

  // Why a depth resolve is or is not the trigger (one bit each for what failed).
  enum Refusal : uint32_t {
    kRefusedNotDepthCopy = 1u << 0,   // not a copy of an EDRAM depth target
    kRefusedNotMainDepth = 1u << 1,   // the source is not the main view's depth identity
    kRefusedEndTiling = 1u << 2,      // inside EndTiling (the end-of-scene copies)
    kRefusedNotFirst = 1u << 3,       // not the first qualifying resolve of the frame
    kRefusedAfterAnchor = 1u << 4,    // the anchor draw or the composite marker came first
    kRefusedDestination = 1u << 5,    // not into kTriggerDestination
    kRefusedClearsDepth = 1u << 6,    // the same call clears the depth after the copy
    kRefusedRectangle = 1u << 7,      // the rectangle does not start at the origin or leaves the picture
    kRefusedLaterTile = 1u << 8,      // another tile of the picture the trigger already covered whole
    kRefusedNotWholePicture = 1u << 9,  // a 4x view's destination is smaller than the main target
  };

  // What became of the frame's trigger (the census says it).
  enum class Outcome : uint8_t {
    kNoTrigger,      // no resolve of the frame was the trigger
    kIdle,           // the trigger, with the occlusion off and no depth statistic due
    kNoCamera,       // no previous frame's camera of this kind of view
    kLensUnknown,    // a pause/map view's second frame: the lens' rate of change is not known yet
    kLensChanging,   // a pause/map view whose lens was still sweeping
    kTooSmall,       // the picture is smaller than the passes are worth
    kNotRecorded,    // a pipeline or an image could not be made
    kRecorded,       // the passes were recorded
  };

  AmbientOcclusion() = default;
  ~AmbientOcclusion();
  AmbientOcclusion(const AmbientOcclusion&) = delete;
  AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

  // Makes the layout and the shaders and lists the pipelines with the host precache (before the
  // Renderer starts it). Never fails the renderer: without them there is no occlusion (logged).
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                  EdramTargets& edram, PassTiming& pass_timing, ResolveCache& resolve);
  // GPU idle and the FrameRing drained, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // The Scene opened a frame's first list: reads the settings.
  void BeginFrame();
  // True while the draw hooks have anything to do (the occlusion or the contact shadows are on, or
  // the census).
  bool wants_draws() const {
    return mode_ != Mode::kOff || contact_mode_ != Mode::kOff || census_every_ != 0;
  }

  // A prepared draw into the main view's identities (the play view's or the pause/map view's), with
  // the draw's own banks, before the draw is recorded.
  void OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks);
  // The game's motion blur draw (in the play view and in the pause/map view alike): the camera of
  // this frame, which the next frame's trigger of the same kind of view uses.
  void OnAnchor(const ConstantBanks::Banks& banks);
  // A depth clear of the identity `key` (the main view's depth is cleared at the start of the main
  // view; its z says which end of the range is far).
  void OnDepthClear(const EdramKey& key, float z);
  // A resolve, after ResolveCache::Execute made its copy, with `list` the frame's open list
  // outside any render pass. `upscaler_running`: a temporal upscaler works on this frame (only
  // the log says it: the noise changes every frame whatever runs). True when passes were
  // recorded: the caller's list state is gone.
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                 plume::RenderCommandList* list, bool upscaler_running);
  // The scene is composed: the UI starts.
  void OnComposite() { composite_seen_ = true; }
  // A guest present: the frame boundary. Prints the census frame.
  void OnPresent();

  // The world of the frame that is ending, for the scene's one line per change of world (menu_world.h): the
  // game view, the pause menu's world, the garage or the map by the frame's own camera, or none (no main view
  // drew, or both kinds did). Read before OnPresent, which forgets the frame.
  menu_world::World FrameWorld() const;
  // The levels in force for the line: the occlusion's, the contact shadows' and the bounce light's, "off" when
  // one is (the bounce light's is "test" under the debug override).
  void Levels(std::string* occlusion, std::string* contact, std::string* bounce) const;

  // The video memory the images hold now.
  uint64_t HeldBytes() const;

 private:
  // The push block of every pass (hlsl/ao_common.hlsli AoPush), 128 bytes.
  struct Push {
    uint32_t slots[4];
    uint32_t slots2[4];
    float sizes[4];
    float lens[4];
    float lin[4];
    float shape[4];
    float light[4];
    float extra[4];
  };
  static_assert(sizeof(Push) == 128);

  // The pixel shaders, in the order host_shaders.h AmbientOcclusionPixelShader numbers them.
  enum Pass : uint32_t {
    kPassDepth,
    kPassDepthMs,
    kPassMip,
    kPassMain,
    kPassDenoise,
    kPassApply,
    kPassApplyMs,
    kPassTemporal,
    kPassTemporalMs,
    kPassContact,
    kCorePassCount,  // the passes above are the occlusion's: without one of them there is none
    kPassMainGi = kCorePassCount,  // the visibility bitmask and the bounce (the test only)
    kPassColor,
    kPassColorMs,
    kPassColorMip,
    kPassColorView,
    kPassDenoiseGi,                // the denoise over the four channels (G7b)
    kPassTemporalGi,               // the accumulation over the four channels
    kPassTemporalGiMs,
    kPassAtrous,                   // the a-trous filter of the bounce
    kPassApplyGi,                  // the apply with the bounce light (G7c), plain and multisampled colour target
    kPassApplyGiMs,
    // The visibility pass with its counts compiled in (ao_fast.h: 3 slices of 3 steps, 2 of 2; the bitmask's also the ring's steps).
    // The occlusion's two are made with the core passes, the bounce light's four (the polynomial acos, then the hardware's) with
    // its own. ao_main_ps / ao_main_gi_ps (kPassMain, kPassMainGi) read the counts from the push block and stay for a count that
    // no variant has; kPassMainGi is also the one with the hardware's acos where the counts are not the variants'.
    kPassMain3,
    kPassMain2,
    kPassMainGi3,
    kPassMainGi2,
    kPassMainGi3Exact,
    kPassMainGi2Exact,
    kPassCount,
  };
  // A pass the occlusion needs (made at the start with the others), against the bounce light's (made when it is on).
  static constexpr bool CorePass(uint32_t pass) {
    return pass < kCorePassCount || pass == kPassMain3 || pass == kPassMain2;
  }

  // The pipelines: one per pass and kind of depth plane, and the apply pass per colour target's
  // sample count and per kind of output (the multiply, or a debug view's write).
  enum Variant : uint32_t {
    kVDepth = 0,
    kVDepthMs = 1,
    kVMip = 2,
    kVMain = 3,
    kVDenoise = 4,
    kVTemporal = 5,
    kVTemporalMs = 6,
    kVVehicleTable = 7,  // a row of the vehicle table (the temporal system's shader)
    kVApplyBase = 8,     // + 4 * debug + log2(samples)
    kVContact = kVApplyBase + 8,
    // The occlusion's visibility pass with its counts compiled in (ao_fast.h): 3 slices of 3 steps, 2 of 2. Both are listed where the
    // occlusion is on (the setting is live: a switch of the level finds the other made), in place of kVMain, which stays for a count
    // that no variant has and is made on first use.
    kVMain3 = kVContact + 1,
    kVMain2 = kVMain3 + 1,
    // The bounce light's (listed with the host precache where the setting is on: host_plan.h; the debug
    // override's pictures, kVGiView, are made on first use). kVMainGi reads the counts (and has the hardware's acos): the pass for a
    // ring of another number of steps; the four after it are the counts compiled in, 3x3 with 2 ring steps and 2x2 with 1, with the
    // polynomial acos (mcla_gpu_ao_fast_math) and with the hardware's. The first two are listed, the exact two made on first use.
    kVMainGi = kVMain2 + 1,
    kVMainGi3 = kVMainGi + 1,
    kVMainGi2 = kVMainGi3 + 1,
    kVMainGi3Exact = kVMainGi2 + 1,
    kVMainGi2Exact = kVMainGi3Exact + 1,
    kVColor = kVMainGi2Exact + 1,
    kVColorMs = kVColor + 1,
    kVColorMip = kVColorMs + 1,
    kVGiView = kVColorMip + 1,  // + log2(samples), four
    kVDenoiseGi = kVGiView + 4,
    kVTemporalGi = kVDenoiseGi + 1,
    kVTemporalGiMs = kVTemporalGi + 1,
    kVAtrous = kVTemporalGiMs + 1,
    kVApplyGiBase = kVAtrous + 1,  // + 4 * debug + log2(samples): the apply with the bounce light
    kVariantCount = kVApplyGiBase + 8,
  };
  static uint32_t ApplyVariant(bool debug, uint32_t host_samples);
  static uint32_t GiApplyVariant(bool debug, uint32_t host_samples);
  static uint32_t GiViewVariant(uint32_t host_samples);
  static std::string VariantName(uint32_t variant);
  // The visibility pass's pipeline for the counts a frame draws (ao_fast.h): `low` is the occlusion's own quality (the plain pass),
  // `fewer` the bounce light's with `far_steps` the ring's steps a side the push block says. The bitmask's reads
  // mcla_gpu_ao_fast_math (live): the polynomial acos or the hardware's; a count that no variant has is kVMainGi.
  static uint32_t MainVariant(bool low);
  static uint32_t MainGiVariant(bool fewer, uint32_t far_steps, bool fast_math);

  // The light constants of a draw, as the raw big-endian dwords of the registers: PS c26.xyz (the
  // ambient, gLightAmbient), PS c39.xyz (the sun colour, gLightColor), VS c16.xyz (gLightPosDir:
  // the direction the sunlight travels, measured at noon: y negative; the direction to the sun is
  // its opposite). Compared as bytes; decoded for the log and the shaders.
  struct LightRaw {
    uint32_t dwords[9] = {};
    bool operator==(const LightRaw& o) const;
  };
  struct LightSeen {
    LightRaw raw;
    uint32_t draws = 0;
  };
  static constexpr size_t kLightSlots = 8;

  // One resolve of the main view's depth, for the census.
  struct DepthResolveNote {
    uint32_t caller = 0;
    uint32_t flags = 0;
    EdramKey source;
    uint32_t dest_address = 0;
    uint32_t dest_format = 0;
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
    int32_t rect[4] = {};
    uint32_t host_sample = 0;
    uint32_t host_samples = 0;
    bool inside_end_tiling = false;
    bool clears_depth = false;
    uint32_t refused = 0;
    uint32_t draws_before = 0;
    bool anchor_before = false;
  };

  // The trigger test: 0 = this resolve is the trigger. A 4x main depth (the pause/map view) is
  // judged by the same rules, except that its first tile may start anywhere (the tile order is not
  // known) and its destination must be the whole picture.
  uint32_t Judge(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  // The whole picture the resolve belongs to, in the source's guest pixels: the destination
  // texture's size, within the source target (the target's height is rounded up to rows of 32:
  // 736 for the picture's 720). The tile a resolve copies is a part of it.
  static void PictureSize(const ResolveCache::Summary& summary, uint32_t* width, uint32_t* height);
  // True when `summary` is another tile of the picture the trigger of this frame covered.
  bool IsLaterTile(const ResolveCache::Summary& summary) const;
  // The light constants most main view draws carried before the trigger (the last one's when none
  // repeats); false when no draw has been seen.
  bool PickLight(LightRaw* out, uint32_t* draws_with_it) const;
  static void DecodeLight(const LightRaw& raw, float ambient[3], float sun[3], float dir[3]);
  void Census();
  // The frame's view for the census: play, pause/map, both, or none.
  const char* ViewName() const;
  // Where the trigger's camera came from, and what became of the trigger, for the census.
  std::string TriggerText() const;

  // --- the passes (ao.cpp) ---
  // What the contact shadows do in a frame (PlanContact): off when the setting is, when a gate says 0,
  // or when the frame has no light constants.
  struct ContactPlan {
    Mode mode = Mode::kOff;
    float strength = 0.0f;         // the setting's, times the elevation and the wetness gates
    float length = 0.6f;           // the ray's length, world units
    float thickness = 0.3f;        // how far in front of a step a surface may be and still hide it
    uint32_t steps = 8;
    float end_fade_start = 0.6f;   // the share of the ray over which a shadow stays full (contact_trace.h)
    float fade_start = 40.0f;      // the distance fade, world units
    float fade_end = 80.0f;
    float mask_gain = 2.0f;        // the mask's stored value times this is the sun's visibility
    float under_vehicle = 0.0f;    // how strongly the ground under a car is shadowed (contact_trace.h), 0 = off
    bool fast = true;              // mcla_gpu_contact_shadows_fast: the adaptive step count and the filter in the apply pass
  };
  // What the SSGI test does in a frame (PlanGi): the colour chain and the bitmask pass in place of the
  // GTAO main pass, and a debug picture of them. Off unless mcla_gpu_ssgi_test is on and the occlusion
  // is; Run turns it off for the frame when anything it needs is missing.
  struct GiPlan {
    bool on = false;
    float thickness = 0.0f;        // the back face of a sample, world units: 0 = a solid (GTAO's horizons)
    float facing = 1.0f;           // the weight of the facing test (0: surfaces are taken to face the pixel)
    float view_gain = 1.0f;
    // 0 none; 1 the bounce, 2-4 the chain's levels 0-2, 5 the bounce cleaned, 6 the visibility, 7 the
    // bounce's history, 8 the bounce after the a-trous filter, 9 the history's weight
    uint32_t view = 0;
    uint32_t history = 16;         // the frames the bounce's history spans at most
    float clamp_width = 2.0f;      // the clip of the bounce's history, in standard deviations of the neighbourhood (ssgi_stable.h)
    float clip_history = 0.15f;    // ... plus this share of the history's own value on each side (the first version: 0)
    uint32_t atrous = 2;           // the a-trous iterations at 720 lines (taps 1, 2, 4 pixels apart); Run adds one for each
                                   // doubling of the image's height (ssgi_stable.h AtrousIterations)
    // The third step (G7c): the bounce in the picture.
    bool test = false;             // mcla_gpu_ssgi_test forced it (the debug override: the occlusion's own quality)
    Mode mode = Mode::kHigh;       // the setting's quality (high: 3 slices and 3 steps at the picture's size)
    bool fewer = false;            // the main pass takes 2 slices and 2 steps (the occlusion's low)
    bool half = false;             // the passes run at half the picture's size even where the occlusion is high (mcla_gpu_ssgi_low 1 / 2)
    bool half_above = true;        // mcla_gpu_ssgi_half: half size above 1080 lines at every quality (ao_size.h), and then one MSAA sample
                                   // in the colour chain (Plan::OneSampleColour)
    bool apply = false;            // the apply pass with the bounce runs (else the plain apply reads the visibility)
    // The numbers of the strength level (mcla_gpu_ssgi_intensity; ssgi_level.h) with the debug cvars' overrides:
    float strength = 1.25f;        // the bounce's gain; 0 runs the passes and adds nothing
    float limit = 2.5f;            // the most it may add to a pixel, as a multiple of the pixel's own colour (a soft guard)
    float cap = 2.0f;              // the brightest a sample may carry and the ceiling the bounce fills a pixel to, in the frame's light
    float reach = 6.0f;            // how far the far ring looks, in world units (0: no ring)
    uint32_t far_steps = 2;        // the ring's samples a side of each slice
    uint32_t level = 1;            // the level (ssgi_level.h Level) these numbers are of
    bool overridden = false;       // a debug cvar replaced one of them
    float radius = 1.0f;           // the near search radius, a multiple of the occlusion's
    uint32_t apply_view = 0;       // mcla_gpu_ssgi_view: 1 the factor in colour, 2 the bounce, 3 the albedo estimate,
                                   // 4 the light T, 5 the surface share, 6 the change against the game's own picture
    // The ring's steps a side the main pass is told (flags bits 26-28): none where there is no ring.
    uint32_t FarStepsSent() const;
  };
  struct Plan {
    Mode mode = Mode::kHigh;       // the occlusion's own quality (slices, steps)
    bool probe = false;            // read the coarsest level back for the census
    bool apply = true;             // the apply pass runs (false: the census's probe only)
    bool occlusion = true;         // the occlusion's term is computed (the AO setting is not off)
    bool low = false;              // the occlusion's and the bounce light's passes run at half the picture's size (the trace)
    bool by_height = false;        // ... only because of the height rule (above 1080 lines: ao_size.h), not the old one
    bool contact_full = false;     // the contact shadows keep the picture's size (their own depth and images) while `low`
    bool upscaler = false;
    uint32_t render[2] = {};       // the main view, host pixels
    uint32_t ao[2] = {};           // the traced passes' size (the depth chain's level 0)
    uint32_t level[2][2] = {};     // the coarser levels' sizes
    uint32_t contact_size[2] = {}; // the contact shadows' images: the picture's, or half of it where the old rule says so
    uint32_t view = 0;             // the debug view: 0 none, 1-6 the occlusion's, 8-11 the contact's
    ContactPlan contact;
    GiPlan gi;
    // The contact filter is done by the apply pass (mcla_gpu_contact_shadows_fast): the march's image goes to it as
    // it is and the denoise pass does not run for the contact shadows. Only at the picture's size: at half size the
    // apply upsamples a filtered image (hlsl/ao_apply_ps.hlsl VisibilityAt), and that pass is a quarter of the pixels.
    bool ContactFilterInApply() const { return ao_size::ContactFilterInApply(contact.fast, SizeChoice()); }
    // The contact shadows have images and a depth of their own (the full size ones) beside a half size trace.
    bool SplitContact() const { return contact.mode != Mode::kOff && contact_full; }
    // The bounce light's colour chain takes one sample of the colour target's MSAA samples (per pixel of a 2x2 block), not all.
    bool OneSampleColour() const { return gi.on && by_height && gi.half_above; }
    ao_size::Choice SizeChoice() const {
      ao_size::Choice c;
      c.legacy_low = low && !by_height;
      c.low = low;
      c.by_height = by_height;
      c.contact_full = contact_full;
      return c;
    }
    // What the frame asks the image rules of ao_size.h (memory and cost models, the tests).
    ao_size::Frame SizeFrame() const {
      ao_size::Frame f;
      f.render[0] = render[0];
      f.render[1] = render[1];
      f.choice = SizeChoice();
      f.occlusion = occlusion;
      f.apply = apply;
      f.probe = probe;
      f.contact = contact.mode != Mode::kOff;
      f.contact_fast = contact.fast;
      f.bounce = gi.on;
      return f;
    }
  };
  // Records the passes for the trigger `summary` (whose source target is the depth). True when
  // anything was recorded.
  bool Run(const ResolveCache::Summary& summary, plume::RenderCommandList* list, const Plan& plan);
  // --- the contact shadows (hlsl/ao_contact_ps.hlsl) ---
  // Decides what they do in this frame: the setting, the sun's elevation and the wetness gates from
  // the light constants and the wetness the frame's draws carried before the trigger (the same
  // light the occlusion uses), the ray's length, the steps, the fades. Logs a change of state once.
  void PlanContact(Plan* plan);
  // The ray march into raw_ and its filter (one pass of the denoise shader's contact mode) into
  // denoised_, after the occlusion's own passes have finished with both. Returns the slot of the
  // result (denoised_), or kNoSlot. With mcla_gpu_contact_shadows_fast and the passes at the picture's
  // size (Plan::ContactFilterInApply) there is no filter pass: the result is raw_, and the apply
  // pass filters it (its flag bit 25). `mask_slot` is the game's sun shadow mask (FindMaskSlot, kNoSlot without it): the
  // march skips the pixels under it that the apply cannot use (the apply is given the same slot).
  uint32_t RecordContact(plume::RenderCommandList* list, const Plan& plan, const Push& base, uint32_t mask_slot);
  // --- the SSGI test (hlsl/ao_color_ps.hlsl, the bitmask pass of hlsl/ao_main_ps.hlsl) ---
  // Decides what it does in this frame: the setting, the occlusion being on, the tuning cvars.
  void PlanGi(Plan* plan);
  // Makes the bounce light's shaders (and the test's) the first time they are asked for: at the start where
  // it is on then, else when it is switched on; false (logged once) when they cannot be. Render thread (or
  // Initialize, before anything draws).
  bool EnsureGiShaders();
  // Everything the frame's SSGI needs that can fail, before anything is recorded: the pipelines, the
  // main view's colour target (at the depth's samples and scale; *colour, *colour_slot), the images.
  // False, logged once, and the frame is the plain occlusion.
  bool PrepareGi(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                 const Plan& plan, EdramTargets::Target** colour, uint32_t* colour_slot);
  // The colour chain: the colour target's mean over its samples (cleaned and capped), then two coarser
  // levels. The colour target is left in the shader-read layout (the apply pass moves it back).
  void RecordColourChain(plume::RenderCommandList* list, const Plan& plan, const Push& base,
                         EdramTargets::Target& colour, uint32_t colour_slot);
  // The SSGI images: the colour chain's three and the main pass's; given back when the test is off.
  bool EnsureGiImages(const Plan& plan);
  void ReleaseGiImages();
  // A resolve of the frame into the game's shadow mask page: remembers its texture key.
  void NoteMaskResolve(const ResolveRecord& record);
  // The bindless slot of the game's shadow mask product, if this frame's resolve made it and it is
  // still what the page holds; kNoSlot otherwise (the frame has no mask: visibility 1).
  uint32_t FindMaskSlot();
  // The wetness the frame's main view draws carried (VS c43.x): the commonest value, to 0.1.
  bool PickWet(float* value) const;
  bool EnsureImages(const Plan& plan);
  void ReleaseImages();
  plume::RenderPipeline* Pipeline(uint32_t variant);
  // The bitmask pass's pipeline for the counts and the acos this frame has (MainGiVariant), else the one that reads the counts.
  plume::RenderPipeline* MainGiPipeline(const GiPlan& gi);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  // The contact march's shader, made when it was not at the start (the contact shadows were off). Render thread.
  bool EnsureContactShader();
  // The pipeline variants the groups of `needs` use, in the order they are made (the likeliest first).
  std::vector<uint32_t> VariantsFor(const host_plan::AmbientNeeds& needs) const;
  // The start: lists what the settings in force use with the host precache and says so for its log line.
  void ListPrecache(const host_plan::Choice& occlusion, const host_plan::Choice& contact,
                    const host_plan::Choice& bounce);
  // Every frame, after the settings are read: lists, on a worker, the pipelines of what is on that the
  // start did not list (a setting switched on since), and turns an effect off for this frame until its
  // pipelines are made. Render thread.
  void HoldUntilBuilt(Mode* occlusion, Mode* contact, Mode* bounce);
  // Every variant of `variants` is made, taken or failed (nothing is being made for it).
  bool VariantsReady(const std::vector<uint32_t>& variants) const;
  void Bind(plume::RenderCommandList* list) const;
  // A full-screen triangle into `image` (all of it) with `pipeline` and `push`.
  void DrawPass(plume::RenderCommandList* list, HostImage& image,
                const plume::RenderPipeline* pipeline, const Push& push);
  // The apply pass into the main view's colour target; false when it could not be made.
  bool RecordApply(plume::RenderCommandList* list, const ResolveCache::Summary& summary,
                   const Plan& plan, const Push& push);
  void FillPush(Push* push, const ResolveCache::Summary& summary, const Plan& plan,
                const LightRaw* light) const;
  // The look (strength, radius, power, direct): mcla_gpu_ao_intensity's, read when the passes are
  // recorded; logged when it changes.
  struct Look {
    float strength = 1.0f;
    float radius = 3.0f;
    float power = 3.6f;
    float direct = 0.375f;
    // How far a vehicle's samples reach on a pixel that is not a vehicle, as a share of the radius
    // (mcla_gpu_ao_car_reach; 0 or 1: all of it).
    float car_reach = 0.4f;
  };
  void UpdateLook();

  // --- the history (hlsl/ao_temporal_ps.hlsl) ---
  // Why a frame's occlusion had no history to lean on (ChooseHistory), or kUsed.
  enum class HistoryWhy : uint8_t {
    kUsed,
    kFirst,  // none yet, or given back with the images
    kGap,    // not the frame after the one that made it
    kView,   // the other kind of view (play against pause/map)
    kSize,   // another image size
    kKind,   // the history of the other kind (the bounce light test turned on or off)
    kCut,    // the camera jumped or turned (the temporal system's cut rule)
    kCap,    // mcla_gpu_ao_history is 1
    kCount,
  };
  // Where the camera's motion into the last frame came from.
  enum class Pose : uint8_t { kStill, kPredicted, kVertexBank };
  struct HistoryUse {
    bool on = false;
    HistoryWhy why = HistoryWhy::kFirst;
    Pose pose = Pose::kStill;
    uint32_t age = 0;        // pictures already averaged into the history, less the first
    uint32_t cap = 12;       // the frames the average spans at most
    temporal::Mat4 motion;   // this frame's game view space into the last frame's (row vectors)
  };
  static const char* HistoryWhyName(HistoryWhy why);
  static const char* PoseName(Pose pose);
  // Decides the frame's history: its use, and the matrix that moves this frame's view space into
  // the history's.
  HistoryUse ChooseHistory(const Plan& plan) const;
  // The motion of the trigger's frame's camera into the previous frame's (true unless the
  // matrix is not finite); *cut says that the frame's own camera jumped (vertex bank only).
  bool FrameMotion(temporal::Mat4* motion, Pose* source, bool* cut) const;
  // Counts what ChooseHistory decided (the census says it) and logs the first use.
  void NoteHistory(const HistoryUse& use);
  // --- the vehicles' history (hlsl/ao_temporal_ps.hlsl, vehicle_motion.h) ---
  // The vehicles' own motion of the anchor that just ended, kept for the next frame's trigger.
  void NoteVehicles(const uint8_t* ps_bank);
  // Draws the vehicle table for the trigger's temporal pass: for each ID with a usable motion, the 3x4
  // that moves this frame's view space into the history's with the camera's motion `use.motion` and the
  // vehicle's step repeated. Returns the IDs (bit k) that got a row; 0 means no table was drawn and
  // `*slot` is then none.
  uint32_t RecordVehicleTable(plume::RenderCommandList* list, const HistoryUse& use, uint32_t* slot);
  // One summary line of the totals (every few thousand frames, and the first time a table is drawn).
  void LogVehicleSummary();
  // The census's depth statistic: a copy of the coarsest level into a readback buffer, and its
  // processing when the GPU has run the copy.
  void RecordProbe(plume::RenderCommandList* list, const Plan& plan);
  void ProcessProbe(uint64_t frame, uint64_t generation, uint32_t width, uint32_t height,
                    uint32_t row_texels, uint32_t level_shift);

  // Settings, read at the frame's start.
  Mode mode_ = Mode::kOff;
  Mode contact_mode_ = Mode::kOff;    // mcla_gpu_contact_shadows, `auto` resolved for this GPU
  bool gi_test_ = false;              // mcla_gpu_ssgi_test (the debug override), shaders made
  Mode gi_mode_ = Mode::kOff;         // mcla_gpu_ssgi, `auto` resolved for this GPU (off until its pipelines are made)
  bool gi_active_ = false;            // the test or the setting is on, the occlusion is, the shaders exist
  uint32_t census_every_ = 0;
  uint32_t off_frames_ = 0;

  // The frame.
  uint64_t frame_ = 0;
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool play_view_ = false;            // the frame had main view draws at the play samples
  bool pause_view_ = false;           // ... at the pause/map samples (4x)
  uint32_t pause_spell_ = 0;          // consecutive pause/map frames up to this one
  uint32_t qualifying_resolves_ = 0;  // resolves that passed every test but "the first"
  // The picture the frame's first qualifying resolve covered, and how many resolves of its other
  // tiles followed (they are not triggers, and not counted as qualifying).
  EdramKey trigger_source_;
  uint32_t trigger_destination_ = 0;
  uint32_t trigger_format_ = 0;
  uint32_t trigger_size_[2] = {};
  int32_t trigger_rect_[4] = {};
  uint32_t later_tiles_ = 0;
  uint32_t draws_before_ = 0;         // main view draws before the first qualifying resolve
  uint32_t color_draws_before_ = 0;
  uint32_t draws_after_ = 0;          // ... after it, before the anchor
  uint32_t depth_writes_after_ = 0;   // ... of them, drawing into the main depth with depth writes
  uint32_t draws_at_anchor_ = 0;      // ... after the anchor (the UI's depth-only draw, 1x)
  uint32_t view4x_draws_ = 0;         // main view draws at the pause/map samples, whenever
  std::array<LightSeen, kLightSlots> lights_{};
  uint32_t light_count_ = 0;
  uint32_t light_overflow_ = 0;       // draws with a value past the table's room
  uint32_t light_draws_ = 0;
  LightRaw first_light_;
  LightRaw last_light_;
  float depth_clear_z_ = -1.0f;
  uint32_t depth_clears_ = 0;
  std::vector<DepthResolveNote> depth_resolves_;
  uint32_t other_resolves_ = 0;
  bool frame_had_main_view_ = false;
  bool ran_this_frame_ = false;
  bool applied_this_frame_ = false;   // ... and the apply pass drew into the colour target

  // The camera: this frame's (read at the anchor) and the frame before's, with the kind of view
  // each came from (a pause/map frame's camera is never used for a play frame or the other way).
  temporal::Camera camera_;
  bool have_camera_ = false;
  temporal::Camera previous_;
  bool have_previous_ = false;
  bool previous_pause_ = false;
  uint64_t previous_frame_ = 0;
  // The trigger's own lens (the camera of the frame before it), kept for the census: previous_
  // is replaced by this frame's camera at the anchor.
  temporal::Camera trigger_camera_;
  bool trigger_camera_valid_ = false;
  bool trigger_pause_ = false;        // the frame's trigger was a pause/map view's
  // The world the frame's trigger was in (menu_world.h): by the previous frame's camera of the same kind of
  // view, which is all that exists at the trigger. It gates the bounce light (RulesFor).
  menu_world::World trigger_world_ = menu_world::World::kNone;
  Outcome outcome_ = Outcome::kNoTrigger;
  // How much the lens changed from the camera before the previous to the previous one (the same
  // kind of view, consecutive frames): the largest relative change of fov, aspect, near and far.
  // A pause/map view whose lens is sweeping (the pause camera's swing, the map), or whose rate of
  // change is not known yet (its second frame), has no occlusion: the previous frame's lens may
  // then be too far from this frame's.
  bool lens_step_valid_ = false;
  double lens_step_ = 0.0;
  double trigger_lens_step_ = 0.0;    // lens_step_ as the frame's trigger saw it
  temporal::Reject reject_ = temporal::Reject::kNone;
  uint32_t rejects_logged_ = 0;
  // The previous frame's lens against this frame's: relative change of the vertical field of view,
  // the aspect, near and far, then the distance the camera moved and the cosine between the two
  // forward vectors.
  bool lens_delta_valid_ = false;
  double lens_delta_[6] = {};
  // Whether the main view's vertex bank holds the camera, so that a frame's own lens could be had
  // before the anchor (for the census): VS c12-c15 of the last colour draw before the trigger
  // against the view inverse the anchor reads (PS c12-c15), per kind of view.
  float vs_view_inverse_[16] = {};
  uint32_t vs_view_inverse_draws_ = 0;
  bool vs_compare_valid_ = false;
  double vs_compare_diff_ = 0.0;
  uint32_t vs_compared_[2] = {};      // frames compared (play, pause/map)
  uint32_t vs_equal_[2] = {};         // ... of them equal

  // The contact shadows. The wetness the colour draws before the trigger carried in VS c43.x (the
  // game's gReflectionParams.x), counted in bins of 0.1 up to 4: the commonest one is the frame's.
  static constexpr uint32_t kWetBins = 41;
  std::array<uint16_t, kWetBins> wet_bins_{};
  uint32_t wet_draws_ = 0;
  // The game's sun shadow mask: the texture key of the last resolve into its page and the frame it
  // came in (a frame without such a resolve has no mask: the page then holds the last frame's depth
  // copy, which must never be read as one).
  TextureKey mask_key_;
  uint64_t mask_frame_ = ~uint64_t(0);
  // What PlanContact found, for the census and the log: the state of the last trigger.
  enum class ContactState : uint8_t {
    kOff,       // the setting is off
    kNoLight,   // no usable light constants before the trigger
    kGated,     // the sun's elevation or the wetness gate is 0
    kActive,
  };
  ContactState contact_state_ = ContactState::kOff;
  uint32_t contact_state_logged_ = ~0u;
  double contact_elevation_ = 0.0;    // degrees
  float contact_wet_ = 0.0f;
  float contact_gate_sun_ = 0.0f;
  float contact_gate_wet_ = 0.0f;
  float contact_strength_ = 0.0f;
  bool contact_mask_ = false;         // the last contact frame had the mask
  uint64_t contact_runs_ = 0;
  uint64_t contact_mask_frames_ = 0;
  uint64_t contact_no_mask_frames_ = 0;
  uint32_t last_contact_logged_ = ~0u;
  uint32_t contact_cost_logged_[4] = {};  // the size (x, y), the steps and the lever's state (bit 0 fast, bit 1 filter in the apply) the cost model line was last logged for
  bool mask_why_logged_ = false;
  bool contact_skip_logged_ = false;  // the skip under the game's own shadow was logged once

  // The vehicles. stencil_ids_: the IDs (bit k) the frame's main view wrote before the anchor.
  // vehicle_motion_ is the last anchor's own motion of them (valid bits; none after a frame that gave
  // no camera or a pause/map frame), made in frame vehicle_motion_frame_.
  uint32_t stencil_ids_ = 0;
  temporal::VehicleMotion vehicle_motion_;
  uint64_t vehicle_motion_frame_ = 0;
  std::unique_ptr<plume::RenderShader> vehicle_table_shader_;
  HostImage vehicle_table_;  // R32G32B32A32_FLOAT, 4 x 24: a row an ID (hlsl/ao_temporal_ps.hlsl)
  // The totals for the log. Anchor side: what the game's matrices gave. Trigger side: the frames that
  // averaged over a history, the vehicle IDs the main view had written by then and how many of them
  // had a row in the table.
  struct VehicleCounts {
    uint64_t anchors = 0;          // play-view anchors read
    uint64_t anchors_with_ids = 0;
    uint64_t ids_asked = 0;        // IDs 1 to 23 the main view wrote before the anchor
    uint64_t ids_taken = 0;        // ... with a usable matrix
    uint64_t not_sane = 0;
    uint64_t not_rigid = 0;
    uint64_t no_world = 0;         // anchors whose c69 was not usable
    uint32_t max_asked = 0;
    uint64_t history_frames = 0;   // play-view frames that averaged over a history
    uint64_t frames_with_ids = 0;  // ... with vehicle IDs in the stencil by the trigger
    uint64_t ids_now = 0;          // ... the IDs there, summed
    uint64_t ids_with_row = 0;     // ... of them with a row in the table
    uint64_t table_frames = 0;     // ... with a table drawn
    uint64_t table_rows = 0;
    uint64_t pause_frames = 0;     // pause/map frames that averaged over a history
    uint64_t pause_with_ids = 0;   // ... with vehicle IDs (they take the camera's motion)
    uint32_t pause_max_ids = 0;
  };
  VehicleCounts vehicles_;
  uint64_t vehicles_logged_at_ = 0;  // history_frames + pause_frames when the summary was last printed
  bool vehicles_first_logged_ = false;

  // The census.
  uint32_t census_frames_logged_ = 0;
  uint32_t census_anomalies_logged_ = 0;
  uint32_t census_pause_logged_ = 0;
  uint32_t probes_done_ = 0;
  uint32_t probe_frames_ = 0;

  // The GPU side.
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  ResolveCache* resolve_ = nullptr;  // finds the game's shadow mask (contact shadows)
  bool ready_ = false;  // the layout and the shaders exist (the contact march's only when it is on: Initialize)
  bool contact_shader_failed_ = false;
  // The bounce light's shaders (the passes after kCorePassCount) exist: made at the start where the bounce
  // light is on then, when it is switched on later, or the first frame the debug override
  // mcla_gpu_ssgi_test is on (EnsureGiShaders); or they could not be made (they take only the bounce
  // light with them).
  bool gi_ready_ = false;
  bool gi_shaders_failed_ = false;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, kPassCount> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, kVariantCount> pipelines_;
  std::array<bool, kVariantCount> pipeline_failed_{};
  std::array<bool, kVariantCount> listed_{};  // with the host precache, at the start or later
  bool occlusion_built_ = false;  // every pipeline of the occlusion is made (HoldUntilBuilt asks no more)
  bool contact_built_ = false;    // ... of the contact shadows
  bool bounce_built_ = false;     // ... of the bounce light
  Look look_;
  uint32_t look_logged_index_ = ~0u;
  float car_reach_logged_ = -1.0f;
  HostImage depth_[3];  // the linear depth chain: the traced size (half the picture's above 1080 lines), then 1/2 and 1/4 of it
  HostImage raw_;       // the main pass's visibility (also the contact shadows' march while their size is the traced one)
  HostImage denoised_;  // the denoise's (also the contact shadows' filter pass's then)
  // The contact shadows' own images while the occlusion is traced at half size and they are not (Plan::SplitContact): the picture's size,
  // the linear depth (R32_FLOAT, the vehicle bit too) that their march and filter read, the march's image and the filter pass's.
  HostImage depth_full_;
  HostImage contact_raw_;
  HostImage contact_dn_;
  // The history: (visibility, linear depth) of the last frame that ran the passes, R16G16_FLOAT,
  // two images that take turns (a pass does not read the image it writes); history_[history_index_]
  // is the one written last.
  HostImage history_[2];
  // Debug view 6's picture: the temporal pass's weights, in the same format; only while that view
  // is shown.
  HostImage weight_;
  // The SSGI test's: the colour chain (RGBA16F: the AO's size, then 1/2 and 1/4 of it) and the main
  // pass's output (RGBA16F: visibility, bounce r, g, b).
  HostImage colour_[3];
  HostImage gi_;
  // The bounce's history and denoise (the second step): the denoise's output (also the a-trous filter's
  // first scratch), the history's two images (RGBA16F: visibility, bounce; they take turns and the idle one
  // is the a-trous filter's second scratch), the last frame's linear depth (R32_FLOAT: depth_[0] and this
  // trade places at the end of a frame that ran the test) and the history weight's picture (made only
  // while the debug view that shows it is on). The history_[] images above are not made while the test
  // runs: history_gi_ says which kind the history written last is.
  HostImage gi_dn_;
  HostImage gi_hist_[2];
  HostImage depth_prev_;
  HostImage gi_weight_;
  bool history_gi_ = false;
  uint32_t history_index_ = 0;
  bool history_valid_ = false;    // it holds a picture the next frame may lean on
  bool history_cut_ = false;      // ... and was dropped by a cut (for the census)
  uint64_t history_frame_ = 0;    // the frame that wrote it
  bool history_pause_ = false;    // ... which kind of view it was
  uint32_t history_size_[2] = {}; // ... at which image size
  float history_tan_[2] = {};     // ... with which lens (tan of the half field of view, x and y)
  uint32_t history_age_ = 0;      // pictures averaged into it, less the first
  // The camera's motion from the frame before the previous to the previous one, measured at the
  // previous frame's anchor draw (game view space to game view space, row vectors): the guess of
  // the step the next frame takes (FrameMotion), and the position change of that step for the
  // cut rule.
  temporal::Mat4 last_motion_;
  bool last_motion_valid_ = false;
  uint64_t last_motion_frame_ = 0;
  double last_step_[3] = {};
  bool have_last_step_ = false;
  // The vertex constants c12-c15 held this frame's view inverse in the last frame the camera moved
  // in (OnAnchor compares them with the anchor's): the trigger may then use them as the frame's own
  // camera.
  bool vs_pose_trusted_ = false;
  // The census: what the last frame's trigger did with the history, and the totals.
  HistoryWhy history_why_ = HistoryWhy::kFirst;
  Pose history_pose_ = Pose::kStill;
  uint64_t history_used_ = 0;
  uint64_t history_resets_[size_t(HistoryWhy::kCount)] = {};
  uint64_t pose_frames_[3] = {};
  bool history_logged_ = false;
  uint32_t images_made_for_[3] = {};  // the traced size and a word of what the set holds (bit 0 the contact shadows' own images, bit 1 the bounce light)
  bool images_failed_logged_ = false;
  // The SSGI test. What the log says once: why a frame had none; and the frames it ran.
  bool gi_failed_logged_ = false;
  uint32_t last_gi_logged_ = ~0u;
  uint32_t gi_level_logged_ = ~0u;  // the strength level (and whether a debug cvar changed it) the log last said
  bool gi_light_logged_[5] = {};   // per menu_world::World: the light constants the bounce light first ran with there were logged
  uint32_t gi_idle_frames_ = 0;    // frames since the images were last used (they go back after a while)
  bool gi_apply_logged_ = false;
  uint64_t gi_runs_ = 0;
  uint64_t gi_history_runs_ = 0;   // ... of them with a history to lean on
  // The census probe's readback.
  std::unique_ptr<plume::RenderBuffer> probe_buffer_;
  uint64_t probe_buffer_bytes_ = 0;
  bool probe_busy_ = false;
  uint64_t generation_ = 1;  // bumped when the images go: a probe in flight is then ignored

  // What the log has said once.
  bool no_lens_logged_ = false;
  bool no_lens_pause_logged_ = false;
  bool lens_step_logged_ = false;
  bool pause_destination_logged_ = false;
  bool refused_target_logged_ = false;
  uint32_t last_mode_logged_ = ~0u;
  uint64_t runs_ = 0;
  uint64_t skipped_no_lens_ = 0;
  uint64_t pause_runs_ = 0;                // occlusion passes recorded in pause/map frames
  uint64_t pause_skipped_no_lens_ = 0;     // pause/map triggers without a camera of their kind
  uint64_t pause_skipped_lens_ = 0;        // ... held back for the lens (unknown or sweeping)
};

}  // namespace mcla::gpu
