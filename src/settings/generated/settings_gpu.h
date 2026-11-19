#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class GPUSettings final : public rex::SettingOwner {
 public:
  GPUSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, build_shader_packs, false, "Builds the shader packs from your game files and closes, without starting the game: no "
    "window, no graphics. The launcher runs this at the end of its setup so that the first start "
    "has nothing to build. Progress is printed to the console; the log is logs/shader_build.txt. "
    "Exit code 0 when the packs are in place.", rex::kRestart, rex::kDebugOnly);

  REX_SETTING(bool, build_shader_packs_force, false, "With build_shader_packs: build even when the packs in place are current.", rex::kRestart, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_compare_effect, "bounce", "Which graphics effect the compare key flips off and on, to see what it does: bounce (the bounce light, the "
    "default), contact (contact shadows), shafts (sun shafts), occlusion (ambient occlusion), filter (the sun "
    "shadow filter), selfrefl (the car's reflection of its own body) or bloom (the glow size fix: it has no row, set it "
    "here or on the command line). The key is "
    "F1 until it is rebound in the key bindings of F4 (bind_mcla_compare). Pressing it shows the effect the other "
    "way than your setting has it, a label at the top of the screen says how, and the next press puts your "
    "setting back. The flip is never saved, and it is dropped when the setting it flips is changed or this one "
    "is. Live.", rex::Allowed("bounce", "contact", "shafts", "occlusion", "filter", "selfrefl", "bloom"));

  REX_SETTING(f64, mcla_compare_label_seconds, 1.5, "Diagnostic: how long the compare key's label stays up before it fades out (about 0.35 s more), 0.5 to 30. "
    "A test run takes its pictures a second after the press and sets it higher. Live.", rex::kDebugOnly, rex::Range{0.5, 30.0});

  // One alpha-to-mask pattern for every draw. MCLA draws its alpha-tested
  // surfaces twice over the same depth: the depth pre-pass (the ShadowBlend
  // shaders, whose colour becomes the shadow collector) with the solid offsets
  // 2, 2, 2, 2, then, after a colour clear, the colour pass under GREATER_EQUAL
  // with the dithered 3, 1, 0, 2. In two pixels of each quad the dithered
  // threshold is above the solid one (by 1/(2N) and 1/(4N) at N samples), so a
  // sample whose alpha lies between them keeps the pre-pass's depth -- what is
  // behind it fails the depth test -- and gets no colour: it keeps the clear,
  // 2.38 in the HDR target, a white dot. That is what the hardware rule the
  // shaders follow gives (Xenia's; Xenia shows the same dots): half a pixel at
  // the console's 2x, the whole pixel at 1x (mcla_gpu_msaa off); dots on fences
  // and foliage, a checkerboard on LOD cross-fades. Every draw on one pattern
  // makes a surface's depth and colour cover the same samples at any sample
  // count. With MSAA it is the dithered one the game gives its colour passes.
  // At 1 sample (mcla_gpu_msaa off, every upscaler) a dither has one sample per
  // pixel to spread over: a fixed 2x2 screen door on grass and fences that no
  // upscaler takes out (it does not move with the jitter). There every draw
  // takes the solid 2, 2, 2, 2 of the game's pre-passes, a plain threshold.
  REX_SETTING(bool, mcla_gpu_a2c_dither_all, true, "Alpha to coverage: every draw takes the same offsets, so a depth pre-pass the game draws "
    "with the solid ones (2, 2, 2, 2) covers the samples its colour pass (dithered 3, 1, 0, 2) "
    "covers: the dithered with MSAA, the solid at 1 sample (a dither there is a fixed screen "
    "door on grass and fences). Off: each draw keeps the game's offsets, as the "
    "console and Xenia -- samples the pre-pass gave depth and the colour pass dropped keep the "
    "clear (white dots on fences and foliage, a white checkerboard on LOD cross-fades; with "
    "MSAA off, whole pixels). Hot reload.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_aa, "off", "Anti-aliasing of the composed scene, before the UI (M6c): off, fxaa (FXAA "
    "3.11, Xenia's preset) or smaa (SMAA 1x, preset HIGH). The image the game resolves into the "
    "front buffer includes it (photos too). Live.");

  REX_SETTING(bool, mcla_gpu_ab_interleave, false, "Diagnostic (M6a): alternate the render thread's code under test between its old "
    "(A) and candidate (B) path every statistics window, and log each window's prepare time "
    "under its variant. Off: the candidate path always runs.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_adapter, "", "Adapter name to create the device on. Empty: on D3D12 the adapter Windows' "
                      "graphics setting for this app names, else the high-performance GPU; on "
                      "Vulkan Plume picks, or the GPU graphics_backend auto chose Vulkan for. The "
                      "emulated GPU (RENDERER: XENIA) runs on the same one unless d3d12_adapter or "
                      "vulkan_device is set.", rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_anisotropy, "game", "Anisotropic texture filtering: game (the game's own fetch constants), off, 2, 4, 8 or 16. "
    "Applies to mipmapped textures whose min, mag and mip filters are all linear. Live.");

  REX_SETTING(std::string, mcla_gpu_ao, "auto", "Ambient occlusion: darkens creases, kerbs, the ground under cars and where objects meet, "
    "from the picture's depth, only on the sky light and not on the sun's (the game's own "
    "constants say how much of a surface's light is which). off, high (half resolution, 2 slices), "
    "ultra (full resolution, 3 slices) or auto (the default): off on an integrated graphics card, "
    "high on a discrete one with less than 5.5 GiB of video memory, ultra on the rest. The pause menu "
    "and the launcher show the same words. A value that is none of these (the low of an older "
    "larecomp.toml) is ignored and the setting runs as auto. "
    "Costs graphics card time, ultra more than high. "
    "Also drawn behind the pause menu and on the map, so a change made there shows at once. "
    "Live.", rex::Allowed("auto", "off", "high", "ultra"));

  REX_SETTING(f64, mcla_gpu_ao_car_reach, 0.4, "Diagnostic: how far what a car blocks of the sky light reaches on the road and the other things "
    "round it, as a share of the occlusion's radius (0.4 of the default 3 m is 1.2 m: the dark line "
    "where the tyres meet the road and the car's footprint, not a halo). The car's own body is "
    "darkened with the whole radius as before. 1 (or 0): a car blocks the sky light as far as a wall "
    "does. Live.", rex::kDebugOnly, rex::Range{0.0, 1.0});

  REX_SETTING(i32, mcla_gpu_ao_census, 0, "Diagnostic: every Nth guest frame (and the first three frames with the main view), one block "
    "of log lines with the main view's draws around the game's mid-scene depth resolve, that "
    "resolve (where it is, how big, inside EndTiling or not), the light constants the occlusion "
    "uses (PS c26 ambient, PS c39 sun colour, VS c16 sun direction), the depth clear value, the "
    "lens the game's motion blur draw gives, what the history did and a statistic of the linear "
    "depth (read back from the graphics card, so the first three frames and every Nth one run the "
    "depth passes even with the occlusion off). Frames where the main view does not have exactly "
    "one qualifying resolve are logged as they come (the first 40). 0: off. Live.", rex::kDebugOnly, rex::Range{0, 100000});

  REX_SETTING(f64, mcla_gpu_ao_fade_end, 160.0, "The distance, in the game's world units (about metres), where ambient occlusion has faded "
    "out entirely; it starts to fade at half of it. Fog, rain and particles are not darkened "
    "either way: they are drawn over the picture afterwards. Live.", rex::kDebugOnly, rex::Range{20.0, 1000.0});

  REX_SETTING(bool, mcla_gpu_ao_fast_math, true, "Diagnostic: the bounce light's sectors take the angle of each sample's horizon from a polynomial (Abramowitz and "
    "Stegun's, 6.8e-5 radians at the most, 0.0014 of one of the 32 sectors) in place of the hardware's acos, which "
    "costs less; every other saving of the occlusion's and the bounce light's shader passes is the same maths to a "
    "rounding error and has no switch. False: the hardware's acos (the shader as it was in that one respect), for "
    "comparing the cost and the picture. Live; the first frame after the change makes the other pipeline on the "
    "render thread (a debug switch: a hitch of tens of milliseconds, once).", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_ao_half_above_1080, true, "Diagnostic: above 1080 lines of render height (1440p, 4K, an upscaler's larger render size) the ambient "
    "occlusion's passes (depth chain, visibility, blur, history) run at half the picture's size, 720 lines for 1440p "
    "and 1080 for 4K, and the apply pass's depth-aware upsample puts the result back; 1080 lines and below are as "
    "they were. The occlusion's own 18 samples a pixel make a signal that has no more detail than that, so the picture "
    "is the same to within a thin edge and the passes cost a quarter. The contact shadows keep the picture's size "
    "(their own depth and images). False: every size at the size the quality says (the render size on ultra). With "
    "the bounce light on, mcla_gpu_ssgi_half decides instead (the bounce light's images give the visibility too). "
    "Live; the first frame after the change makes the images again.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_ao_history, 12, "Diagnostic: how many frames ambient occlusion averages to take its sampling noise out, 1 to "
    "64. Each frame's occlusion is found again in the next by the camera's motion and kept only "
    "where it is the same surface; more frames is smoother and slower to follow a moving shadow "
    "(a car's), fewer is noisier. 1: no averaging (two blur passes on every frame instead). Live.", rex::kDebugOnly, rex::Range{1, 64});

  REX_SETTING(std::string, mcla_gpu_ao_intensity, "default", "How strong ambient occlusion looks: light, default, strong or max. Each is a set of four "
    "numbers: how strongly it darkens, how far it looks for things that block the sky light, how "
    "hard the measure is pushed, and how much it also darkens the part of a surface the sun lights; "
    "the stronger ones reach farther and make every crease, corner and under-car shadow darker. "
    "Only used while mcla_gpu_ao is not off (the pause menu's AO INTENSITY row). Live.", rex::Allowed("light", "default", "strong", "max"));

  REX_SETTING(f64, mcla_gpu_ao_paint, 0.3125, "Diagnostic: how much of ambient occlusion a car's glossy paint takes. The colour of dark paint is nearly all the "
    "reflection of the sky, and a reflection is blocked only where the mirror ray is, not by the share of the sky a part "
    "cannot see, so a part that is partly covered (the uprights of a wing and the lid under it) lost most of its colour. On "
    "a car's pixels the occlusion's value (the image's, already raised to the look's power) is raised to this power: 1 "
    "darkens the paint like the world, smaller keeps more of its colour. The default, 0.3125, takes the default look's power "
    "(3.6) out again, so the paint is darkened by the share of the sky a part can see. Rounded to sixteenths, 1/16 to 1. "
    "Live.", rex::kDebugOnly, rex::Range{0.0625, 1.0});

  REX_SETTING(bool, mcla_gpu_ao_stable, true, "Diagnostic: the occlusion's noise stays as small in a big picture as in a small one: its blur reaches "
    "as far in the picture at 4K (7x7 pixels) as it does at 720 lines (3x3), the wide blur of the pixels "
    "that have no history follows, and the noise's shift from frame to frame is a lattice of 13 frames that "
    "any 12 consecutive frames cover evenly. The bounce light's a-trous filter has more iterations in a "
    "bigger picture and its history is clipped to a wider box. False: the first version's 3x3 blur and 5x5 "
    "wide blur at every size, its additive recurrence, its 2 a-trous iterations and its clip of 1.25 "
    "deviations, for comparing. Live.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_ao_view, 0, "Diagnostic: shows a stage of the ambient occlusion as a grey picture instead of applying it "
    "(only while it is on): 1 the factor the picture is multiplied by (white: none), 2 the "
    "visibility, 3 the share of the light that is ambient, 4 the surface normals, 5 the distance "
    "(a log scale), 6 how much of the last frames each pixel kept (white: all it can, black: none, "
    "so it is new, a vehicle whose own motion is not known, or has no history yet; the history itself "
    "goes on as usual), 7 the occlusion's own part of the factor (the visibility with the ambient "
    "share, the strength and the distance fade; white: none), 8 the pixels it takes for vehicles "
    "(white). 0: "
    "off. Live.", rex::kDebugOnly, rex::Range{0, 8});

  REX_SETTING(u32, mcla_gpu_batch_draws, 0, "Submit the guest frame's command list to the GPU every this many prepared draws, unfenced, "
    "and continue in a new list (0: one submission per guest frame, the default). For measuring "
    "submission batching.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_bloom_scale, true, "Fix: the glow of lights (the bloom) keeps the size the game gives it at render scales above 1x "
    "(a higher render resolution, or an upscaler to a 1080p or 4K output). The game blurs the bloom "
    "with taps a fixed number of pixels apart; at 2x or 3x the same taps reach half or a third as far "
    "on the screen, so a window's glow shrinks and the glow of a lamp or a tail light disappears. This "
    "makes each tap cover the pixels it stands for, so the glow looks as it does at 1x. At a render "
    "scale of 1x the picture is the same either way. Off: the game's own taps. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_car_chrome_occlusion, true, "Fix: the chrome of the player's car (the tailpipes, the rims, the badges and trim) is not lit by what the car's own body covers. "
    "The game's chrome shader looks up the sky in the environment map and adds the sky's ambient colour wherever the surface "
    "points, and nothing in it knows the bumper over a tailpipe, so the tips, which sit 2 to 7 cm under and behind the parts "
    "that cover them, glow as if lit from above. This marches two short rays from each chrome pixel through the picture of the "
    "car from the frame before (the car self-reflection's image): one along the reflection, one towards the sky, and darkens the "
    "reflection and the ambient where they hit a part of the car, by up to 90 percent. It runs wherever the car "
    "self-reflection runs (mcla_gpu_car_selfrefl) and needs the shader pack's variant file. Off: the game's own chrome. Live; "
    "the first time it shows, the graphics card builds its shader (a short stutter).", rex::kDebugOnly);

  REX_SETTING(u64, mcla_gpu_car_dump, 0, "Debug: takes the triangles of every car the game draws in ONE frame out of the game's memory, puts them in world space with "
    "each draw's own matrices and writes them to car_dump_<frame>.obj next to the executable (one group per draw, for the car "
    "chosen by mcla_gpu_car_dump_id), with a census of the car draws (how many draws, vertices and triangles each car has and "
    "which shaders draw it) in the log under [car dump]. 0 (the default) is off. A number N dumps the first frame at or after the "
    "game's frame N that has a car in it. With the car self-reflections on (mcla_gpu_car_selfrefl_test=1) it also checks the "
    "triangles against the picture of the cars the self-reflections keep, and logs how much of the car they cover and how close "
    "their distances are. Park the car, switch the upscaler off and use the game's own view (not the garage or the pause menu). A "
    "new value dumps again. It costs nothing while it is off and one slow frame when it dumps.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_car_dump_id, 0, "Debug: which car mcla_gpu_car_dump writes to its OBJ file and checks first, by the number the game stamps into the stencil "
    "for it (1 to 23, handed out in drawing order each frame, so a car has no fixed number). 0 (the default) takes the player's "
    "car: the one nearest to the vehicle the player drives in the game's memory, else the one with paint draws nearest to the "
    "camera, else the one with the most body draws; the log says which. Live.", rex::kDebugOnly, rex::Range{0, 23});

  REX_SETTING(bool, mcla_gpu_car_frame_log, false, "Debug: logs one line under [car frame] whenever the player's car changes in the draw list the car effects share (its "
    "stencil ID, how many draws it has, how many are rigid parts and how many skinned): what was found, how it was found and what "
    "was left out. With this on the list is collected in every frame, whether a car effect (the car shadow map, the car ray tracing) "
    "asks for it or not, so the line shows by itself. A second line, 'keys', says when the car's meshes get new keys (a new key is a "
    "mesh the car effects read or build again): a few seconds after the car appears there should be none in a parked car; at most one "
    "line every few seconds. Never saved. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_car_rt_check, false, "Debug: proves the player's car as ray tracing structures (car_rt_scene.h) on a graphics card that has them (Direct3D 12 with "
    "DirectX Raytracing 1.1). After the ambient occlusion's passes one ray is traced from the camera through every vehicle pixel of its "
    "linear depth image and the hit's distance is compared with the pixel's; the log says every 120 frames, under [car rt], how many "
    "vehicle pixels the structures cover (the other cars and the driver count as misses) and how far the distances are apart. It "
    "needs the ambient occlusion on, and it asks for the car's meshes to be built. Costs a ray a vehicle pixel; never saved. Live.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_car_rt_masks, 0, "Debug: how the car's parts are told apart in the top level: 0 the tyres by their shader and glass by being drawn after the world's "
    "first draw (only in a frame where some of the car's draws are and some are not: where all are, nothing is glass), 1 every part "
    "that is no tyre is a body. Live.", rex::kDebugOnly, rex::Range{0, 1});

  REX_SETTING(i32, mcla_gpu_car_rt_view, 0, "Debug: shows the car ray tracing check's picture over the scene (only while mcla_gpu_car_rt_check is on): 1 the error of each "
    "vehicle pixel's hit (green below 2%, yellow at 2%, red from 10%, magenta: nothing hit), 2 the hit's normal as colour (x right, y up, "
    "z forward, times 0.5 plus 0.5). 0: off. Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(std::string, mcla_gpu_car_selfrefl, "auto", "Car self-reflections: your car's paint reflects its own body, which the game's paint does not: the wing shows in the "
    "trunk lid and the mirror in the door, and the reflection is darker where the body shades it, under a spoiler or "
    "in a wheel arch, and it also shows the car's own colour in the reflection. on is the one level; off leaves the game's "
    "paint as it is. auto (the default) is on for a discrete graphics card and off for an integrated one (a handheld, or the "
    "iGPU of a desktop CPU). Costs the graphics card a little while it is on (about 0.1 ms a frame at 4K on an RTX 5080). "
    "It works in the game, behind the pause menu and in the garage, not on the map. Live.", rex::Allowed("auto", "off", "on"));

  REX_SETTING(f64, mcla_gpu_car_selfrefl_full, 0.25, "Diagnostic: the share of the car self-reflection ray's points that, all inside a part of the car, take the whole hit "
    "(0.25: 3 of the 12). A smaller one makes every hit heavier and the outline of a part a steeper step, a larger one lighter and "
    "softer. Live.", rex::kDebugOnly, rex::Range{0.05, 1.0});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_gain, 1.0, "Diagnostic: a multiplier on the brightness of the car's own colour in its reflection (1: the target's exponent bias "
    "taken out, which is the paint's own units; unverified: raise it if the reflection of the car looks too dark). Live.", rex::kDebugOnly, rex::Range{0.0, 16.0});

  REX_SETTING(i32, mcla_gpu_car_selfrefl_history, 0, "Diagnostic: the size of the car self-reflection's history image: 0 the rule (half of the render size for the occlusion-only "
    "test level and from 2560 pixels across, the full size otherwise), 1 the full render size, 2 half of it in each direction (a quarter "
    "of the memory and of the pass's work). Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_length, 2.0, "Diagnostic: how far the car self-reflection's ray goes, in the game's world units (about metres). Live.", rex::kDebugOnly, rex::Range{0.2, 6.0});

  REX_SETTING(std::string, mcla_gpu_car_selfrefl_rt, "auto", "Ray traced car self-reflections: where the graphics card can ray trace (Direct3D 12 with ray tracing 1.1), your car's reflection of "
    "its own body comes from its real shape, not from the picture of the car on the screen: a part the camera cannot see (the back of the "
    "wing, a mirror's far side) shows in the paint as it does in a mirror, and thin parts are exact. auto (the default) is on there while the "
    "car self-reflections are on; off keeps the screen-space version everywhere. A graphics card that cannot ray trace uses the "
    "screen-space version whatever this says. Live.", rex::Allowed("auto", "off"));

  REX_SETTING(i32, mcla_gpu_car_selfrefl_rt_denoise, 2, "Diagnostic: the passes of the filter that takes the grain out of the soft edge of a part of your car reflected in another (the ray traced car "
    "self-reflection) before the paint reads it: 2 (the default) two passes, 1 one pass, 0 none (the raw trace, to compare with). Each pass is "
    "applied only where the soft edge is wide enough to have grain, so a sharp edge stays sharp; there is none for a gloss of 0. Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_rt_gloss, 2.5, "Diagnostic: the half angle, in degrees, of the cone the ray traced car self-reflection casts its rays over: the paint's gloss. A part of "
    "your car reflected in another has an edge as soft as the cone is wide at its distance (2.5 degrees: about a hand's width at 1 m); 0 is a perfect "
    "mirror (one ray, a razor edge). Live.", rex::kDebugOnly, rex::Range{0.0, 12.0});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_rt_length, 4.0, "Diagnostic: how far the ray traced car self-reflection's ray reaches, in the game's world units (about metres); what it finds weighs "
    "less from 0.6 of it and nothing at it. Live.", rex::kDebugOnly, rex::Range{0.5, 20.0});

  REX_SETTING(i32, mcla_gpu_car_selfrefl_rt_parts, 7, "Diagnostic: the parts of your car the ray traced self-reflection's ray can hit, a sum of: 1 the body, 2 the glass, 4 the tyres, 8 the "
    "driver. 7 (the default): the body, the glass and the tyres. Live.", rex::kDebugOnly, rex::Range{1, 15});

  REX_SETTING(i32, mcla_gpu_car_selfrefl_steps, 12, "Diagnostic: the points on the car self-reflection's ray, 4 to 24 (the cost is one texture read each, and three more for "
    "each segment between two points that holds the outline of a part; they are closer together near the car's surface, and "
    "the step the weight makes at a part's outline is a point's share of the whole hit). Live.", rex::kDebugOnly, rex::Range{4, 24});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_strength, 1.0, "Diagnostic: the most of the reflection the car self-reflection takes over, 0 to 1. Live.", rex::kDebugOnly, rex::Range{0.0, 1.0});

  REX_SETTING(i32, mcla_gpu_car_selfrefl_test, 0, "Diagnostic: forces the car self-reflections (mcla_gpu_car_selfrefl) to a level whatever the setting says, for a test "
    "or a comparison. 0: follow the setting (the default). 1: the specular occlusion alone, the reflection is darkened where "
    "a short ray along it passes just behind a part of the car (the setting has no such level: it costs the same as 2). "
    "2: the full effect, the same, and the car's own colour of the previous frame is reflected there (what the setting on does). "
    "Needs the shader pack's "
    "car_selfrefl variant and the motion blur draw's camera. Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(f64, mcla_gpu_car_selfrefl_thickness, 0.3, "Diagnostic: how far behind a part of the car's surface (as the previous frame saw it) a ray point still hits it, "
    "in the game's world units; the weight of a point is full up to half of it and falls to nothing at it. Live.", rex::kDebugOnly, rex::Range{0.02, 1.0});

  REX_SETTING(i32, mcla_gpu_car_selfrefl_view, 0, "Diagnostic: shows a stage of the car self-reflection instead of the paint (only while the car self-reflections are on): "
    "1 the weight of the hit, 2 the colour the ray found, 3 the paint's baked vertex occlusion, 4 the state of the trace "
    "(red: the point is not found in the previous frame, blue: the ray left the picture, green: no hit, yellow: a hit, "
    "orange where the hit is probably the hidden face of a part, the brighter the more of the reflection it takes), 5 the "
    "ray traced image at the paint's pixel (purple: no image this frame, cyan: no data there, so the march ran, green: no hit, "
    "yellow: a hit on a part the camera sees, orange: on a part it does not). 0: off. Live.", rex::kDebugOnly, rex::Range{0, 5});

  REX_SETTING(std::string, mcla_gpu_car_shadow_map, "auto", "Car shadow map: your car casts a shadow of its real shape on itself and on the road beside it. The game's own sun shadows "
    "draw the car in a rough low-detail form into maps whose texels are 1 to 25 cm, so the wing, the mirrors, the sills and the "
    "wheels cast no shadow of their own; this draws the car again from the sun, at full detail, into a small map fitted to it "
    "(2048 by 2048 on ultra, 1024 by 1024 on high), and the contact shadows read it. It only runs while the contact shadows "
    "(mcla_gpu_contact_shadows) are on, in the game view, in daylight. off, high, ultra or auto (the default): ultra on a discrete "
    "graphics card, high on an integrated one. Costs a little graphics card time (the car's triangles drawn once more, depth only) "
    "and 8 or 32 MiB of video memory. Live.", rex::Allowed("auto", "off", "high", "ultra"));

  REX_SETTING(i32, mcla_gpu_car_shadow_map_view, 0, "Diagnostic: 1 draws the car shadow map in a square in the lower left corner of the picture (grey by depth, near the sun "
    "bright; dark blue where nothing of the car is), while the map is drawn. 0: off. Live.", rex::kDebugOnly, rex::Range{0, 1});

  REX_SETTING(std::string, mcla_gpu_census_ps, "", "Diagnostic: up to 4 pixel shader identities, hex, comma separated. Per "
    "guest frame, the draws and vertices recorded with each; per report window, their "
    "distribution. Empty: off.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_census_scale, false, "Diagnostic (M6e.0): logs once per case each resolve product a draw at another resolution "
    "scale reads (with the fetch's filters) and each copy of an image of the screen that lands "
    "away from its source. Off: nothing is checked.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_census_targets, false, "Diagnostic: per report window, every pass the frames drew into (its EDRAM "
    "targets) with its draws per frame and the pixel shaders that drew there.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_class, "auto", "Diagnostic: forces the GPU class the graphics upgrades' auto values are chosen from (low: "
    "integrated GPUs and handhelds, mid: discrete with under about 6 GiB of video memory or a "
    "slower GPU family such as the GTX 10 and 16 series and the RX 400/500 series, high: the "
    "rest). "
    "auto: the class the renderer found for this GPU, logged at start as '[gpu] class: ...' with "
    "the rule that chose it (by adapter type, by memory or by name). Only "
    "the value auto of the upgrades follows it (shadow filter and detail, car and road reflection "
    "detail, ambient occlusion, contact shadows, sun shafts, bounce light); "
    "a choice of the player's own is never changed. Live.", rex::Allowed("auto", "low", "mid", "high"), rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_contact_shadows, "auto", "Contact shadows: the short sun shadows the game does not draw (under a car's sills, at the foot "
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
    "pause menu and on the map. Live.", rex::Allowed("auto", "off", "high", "ultra"));

  REX_SETTING(bool, mcla_gpu_contact_shadows_car_map_march, false, "Diagnostic: with the player's car's own sun shadow map in the frame (car_shadow_map.h), the short march that "
    "the car's own pixels (and any vehicle's) take also runs on the pixels the map covers, and the pixel gets the darker "
    "of the two. False: where the map holds a part of the car at a vehicle's pixel the map is the truth about the car's "
    "own parts (a wing's plate and its posts, a mirror, a sill), which the march only guesses from the picture's depth "
    "(a thin part has no thickness there), so that pixel takes no march steps and its contact shadow is the map's. "
    "For comparing the two on the wing and the mirrors (view 5 shows the map's term). Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_contact_shadows_fade, 0.6, "Diagnostic: how much of the contact shadows' ray a shadow stays fully dark over, 0.2 to 0.98: "
    "past that share of the ray's length it fades out, so that a shadow cut off by the ray's end "
    "does not end in a hard line. At 0.2 it looks like the first version's fade (a quarter of the "
    "shadow gone half way along the ray). Live.", rex::kDebugOnly, rex::Range{0.2, 0.98});

  REX_SETTING(bool, mcla_gpu_contact_shadows_fast, true, "Diagnostic: the cheaper contact shadows, to compare against the plain ones (false: the passes as "
    "they were). The march takes fewer steps where its ray covers few pixels on the screen (far "
    "pixels, a smaller picture; the squared spacing is kept), and the 3x3 filter that keeps shadow "
    "edges is done inside the apply pass, which saves a pass and a full size image write (only where "
    "the contact passes run at the picture's size: ultra, or the occlusion on ultra). With that, and in a "
    "frame that has the game's sun shadow mask, the march is also left out where the game's own shadow "
    "covers a pixel and its neighbours (the apply cannot use the result there: exactly the same picture). "
    "The picture is the same to within a shadow edge moving by a pixel. Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_contact_shadows_length, 0.6, "Diagnostic: how far toward the sun contact shadows look, in the game's world units (about "
    "metres), with the sun high. The length grows with a low sun (0.7 / tan of its elevation, 1 to 3 "
    "times). Live.", rex::kDebugOnly, rex::Range{0.1, 3.0});

  REX_SETTING(f64, mcla_gpu_contact_shadows_mask_gain, 2.0, "Diagnostic: what the game's sun shadow mask's stored value is multiplied by to give the sun's "
    "visibility (1 lit): the game's collector stores half of it and the lit shaders multiply by 2. "
    "Look at mcla_gpu_contact_shadows_view 3 (white where lit) if shadows are darkened twice or not "
    "at all. Live.", rex::kDebugOnly, rex::Range{0.5, 4.0});

  REX_SETTING(f64, mcla_gpu_contact_shadows_strength, 1.0, "Diagnostic: how strongly contact shadows darken the sun's light where they apply, 0 to 2 "
    "(the sun's elevation and the wetness scale it further). Live.", rex::kDebugOnly, rex::Range{0.0, 2.0});

  REX_SETTING(f64, mcla_gpu_contact_shadows_thickness, 0.3, "Diagnostic: how far in front of a step of the ray a surface may be and still hide it, in the "
    "game's world units: a thing nearer to the camera than that only overlaps on screen and hides "
    "nothing. Live.", rex::kDebugOnly, rex::Range{0.05, 3.0});

  REX_SETTING(f64, mcla_gpu_contact_shadows_vehicles, 1.0, "Diagnostic: how strongly contact shadows darken the ground under a car, 0 to 1. The game's sun "
    "shadow map holds only the player's car, so the ground under a traffic car is as lit as an open "
    "road; a ground pixel with a car's body above it and in the way of the sun (found from the "
    "picture's depth and the cars' pixels, seen through the gap under the car) gets the full contact "
    "shadow, as the player's car gets its sun shadow from the game. Cars the game's sun shadow map "
    "holds (the player's, and traffic with TRAFFIC SHADOWS) are left out. 0 is the contact shadows as "
    "they were (only a strip along the car's edges and its wheels). Live.", rex::kDebugOnly, rex::Range{0.0, 1.0});

  REX_SETTING(i32, mcla_gpu_contact_shadows_view, 0, "Diagnostic: shows a stage of the contact shadows as a grey picture instead of applying them "
    "(only while they are on, and instead of mcla_gpu_ao_view): 1 the factor the picture is "
    "multiplied by (white: none), 2 the contact visibility (black where the ray is hidden), 3 the "
    "sun's visibility (the game's shadow mask: white lit; white everywhere where the frame has none), "
    "4 the sun's share of the light, 5 the player's car's own sun shadow map's term alone (white; black where "
    "the map shadows the pixel: the wing, the mirrors, the sills and the wheels on the car and on the road "
    "near it; white everywhere in a frame without the map: car_shadow_map.h). 0: off. Live.", rex::kDebugOnly, rex::Range{0, 5});

  REX_SETTING(i32, mcla_gpu_contact_skip_stat, 0, "Diagnostic: with mcla_gpu_contact_shadows_fast and the game's sun shadow mask in the frame, every N frames (0: never) the contact march's "
    "skip is counted: a second march pass classifies every pixel (skipped under the game's own shadow, the sky or past the fade, turned from "
    "the sun, marching) with the mask's stored value at its centre, and the log gets the shares, the spread of those values (does the "
    "game's shade store exactly 0, which the skip needs?) and the model's march time for that share. That frame costs one more march and a "
    "readback of the picture. Live.", rex::kDebugOnly, rex::Range{0, 100000});

  REX_SETTING(bool, mcla_gpu_coverage_queries, false, "With mcla_gpu_edram_aliasing: an occlusion query around each opaque draw the detector "
    "takes to cover its scissor, checking that it did. Costs GPU time, for "
    "checking a report. Off: no query pool is made, read or reset, and such takes go "
    "unchecked (listed once per pair).", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_debug_layer, false, "Enable the D3D12 debug layer before the device exists and log its warnings "
                    "and errors. Needs D3D12SDKLayers.dll from the same Agility SDK release as "
                    "D3D12\\D3D12Core.dll, placed next to it (the system's copy belongs to the OS "
                    "runtime). Vulkan validation is a property of a Debug build of Plume and "
                    "cannot be switched on here.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, mcla_gpu_dlss_mask_tags, 7, "Diagnostic: which of Streamline's hint inputs carry the masks DLSS is given (what is drawn "
    "again for the upscaler: shadows of cars, particles, light volumes, car glass). Add up: 1 the "
    "reactive mask goes in as the bias-current-colour hint, 2 as the reactive mask hint, 4 the glass "
    "mask goes in as the transparency and composition hint. 7: all three. Used with "
    "mcla_gpu_upscaler_mask_test (an all-ones mask) to see which hint the DLSS plugin reads: with "
    "the test on, 1 alone, then 2 alone, then 4 alone; the one that makes the picture lose its "
    "smoothness over time is read. Live.", rex::Range{0, 7}, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_dlss_mode, "dlaa", "DLSS's mode, NVIDIA's names and the share of the output the scene is drawn at: dlaa (100%, "
    "only smoothed), quality (67%), balanced (58%), performance (50%) or ultra_performance (33%), "
    "per direction, of the output (the window's 16:9 area, whatever mcla_gpu_resolution_scale "
    "says). Live: the scene is drawn at the new size from a frame boundary once the upscaler is "
    "ready for it (several seconds for DLSS), and the old mode works until then.");

  REX_SETTING(bool, mcla_gpu_edram_aliasing, false, "EDRAM inheritance detector (M5.2d): per EDRAM "
    "tile and aspect, the identity that wrote it last, reporting a draw or a resolve that would "
    "see another identity's bytes on the console -- which the host does not carry over. Off: "
    "none of its work is done.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_edram_capture, 0, "With mcla_gpu_edram_aliasing: a RenderDoc capture of the frame after the first report of "
    "each kind picked by these bits, 4 at most: 1 a pair met by a write that keeps "
    "or blends another identity's bytes, 2 a pair met by a read, 4 the first reader of "
    "inherited bytes of each kind. 0: none. Needs the game started under RenderDoc.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_env_resolution, "auto", "How sharp the reflections in the paint, windows and bonnet of the cars are: the game draws the "
    "world around the car as a 512x256 picture for them to reflect, which is why the reflections look "
    "soft up close. game (the game's own 512x256), high (1024x512), ultra (1536x768), or auto (game on "
    "an integrated graphics chip or a card with less than about 6 GB of memory, high on any other card). "
    "Costs a little graphics card time and video memory (about 4 MiB for high, 16 MiB for ultra). Live.", rex::Allowed("auto", "game", "high", "ultra"));

  REX_SETTING(bool, mcla_gpu_flicker_probe, false, "Diagnostic: read every guest frame back, as the window shows it, and log "
    "the blocks of 16x16 pixels that change for 1-5 frames and come back to exactly their "
    "old bytes (a flicker). Meant for a static scene such as the pause menu. Costs a copy of "
    "the image per frame on the GPU and about a millisecond of render-thread CPU. Off: "
    "nothing is made.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_flicker_probe_dumps, 12, "With mcla_gpu_flicker_probe: how many flicker events also write the frame before, the "
    "first frame of the blip and the frame after as mcla_flicker_<event>_<frame>_<what>.tga "
    "next to the executable (3.7 MB each at 1280x720).", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_flicker_probe_shots, "", "With mcla_gpu_flicker_probe: a comma-separated list of guest frame numbers, each written "
    "as mcla_flicker_000_<frame>_shot.tga next to the executable whether or not it flickered. "
    "For comparing what two platforms or builds put in the window at the same frame.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_frame_generation, "off", "Frame generation: off, fsr31 (AMD FSR 3.1 frame generation, on any graphics card, with or "
    "without an upscaler) or dlss (NVIDIA DLSS frame generation through Streamline, on an RTX 40 or "
    "50 series card: 2x, and 3x or 4x on an RTX 50 with mcla_gpu_frame_generation_multiplier; "
    "Streamline presents the frames, and on Vulkan it applies from the next start). An interpolated "
    "frame is made between every two the game renders and "
    "shown half a frame before the real one, so a game that renders 60 frames a second shows 120. "
    "It does not make the game react faster: the real frames are shown one frame later and the "
    "latency reducer (NVIDIA Reflex, AMD Anti-Lag 2) is switched on while it runs, where the card "
    "has one. Only the 3D scene is generated; the menus, the pause menu, the map and the loading "
    "screens are shown as the game draws them, and the interface is drawn over each generated "
    "frame. The frame rate limit counts rendered frames while it runs "
    "(mcla_gpu_frame_generation_fps_limit). Wants a display of 100 Hz or more or a variable refresh "
    "display. Live: it is taken at a frame boundary and the picture may flicker once while the "
    "window's swap chain is made again. Only offered where the graphics card and the AMD files next "
    "to the game can run it.");

  REX_SETTING(i32, mcla_gpu_frame_generation_fps_limit, -1, "The frame rate cap while frame generation runs, in frames the game RENDERS a second (twice as "
    "many are shown): -1 is HALF DISPLAY, the default, the floor of 97% of half of the display's "
    "refresh rate (116 at 240 Hz, 58 at 120 Hz), which fills the display; it is only offered on a "
    "display of 100 Hz or more, and where it is not the cap you chose (fps_limit) applies. 0 is no "
    "cap, and a number from 1 to 360 caps the rendered frames at it (60 shows 120). A cap above half "
    "the display's rate adds latency: the frames queue in front of the display. Live.", rex::Range{-1, 360});

  REX_SETTING(i32, mcla_gpu_frame_generation_multiplier, 2, "How many frames DLSS frame generation shows for each one the game renders (mcla_gpu_frame_"
    "generation = dlss): 2 makes one in between, 3 two, 4 three. 3 and 4 need an RTX 50 series card "
    "(the plugin says how many the card takes; a higher number than it takes is lowered). The frame "
    "cap counts rendered frames (mcla_gpu_frame_generation_fps_limit). Live.", rex::Range{2, 4});

  REX_SETTING(bool, mcla_gpu_frame_generation_tearing, false, "Frame generation presents with vertical sync (FIFO), so a frame is never torn and a variable "
    "refresh display follows the frames. On presents immediately instead, which tears. Read when "
    "frame generation is switched on.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_fsr, false, "FSR1 on the present (M6c.1): when the window's letterbox is larger than the "
    "front buffer on both axes, EASU passes (at most 2x per axis each) up to it, then RCAS. "
    "Otherwise the bilinear present. Live.");

  REX_SETTING(std::string, mcla_gpu_fsr31_mode, "native_aa", "FSR 3.1's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the output (the window's 16:9 area, whatever "
    "mcla_gpu_resolution_scale says). Live: the scene is drawn at the new size from a frame "
    "boundary once the upscaler is ready for it, and the old mode works until then.");

  REX_SETTING(std::string, mcla_gpu_fsr4_mode, "native_aa", "FSR 4's mode, AMD's names and the share of the output the scene is drawn at: native_aa "
    "(100%, only smoothed), quality (67%), balanced (59%), performance (50%) or ultra_performance "
    "(33%), per direction, of the output (the window's 16:9 area, whatever "
    "mcla_gpu_resolution_scale says). Live: the scene is drawn at the new size from a frame "
    "boundary once the upscaler is ready for it, and the old mode works until then.");

  REX_SETTING(bool, mcla_gpu_fsr_debug_view, false, "Test lever (FSR 3.1 and FSR 4 only): the upscaler's context is made with AMD's debug "
    "visualization (it holds more video memory) and every dispatch asks it to draw its own views "
    "into the picture it makes: the motion vectors it received and how much it trusts its history "
    "(its reactive, disocclusion and blend factor views, as its version has them; in FSR 4 the "
    "bright purple is a pixel whose history it does not trust). Everything the game does "
    "afterwards (bloom, tone map) reads that picture, overlay included. A change makes a new "
    "context: about half a second of plain frames. For A/B. Live.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_fsr_max_passes, 4, "Most FSR1 EASU passes (each at most 2x per axis); a bilinear stretch finishes what they "
    "leave. The SDK presenter's present_fsr_max_upsampling_passes. Live.", rex::kDebugOnly, rex::Range{1, 4});

  REX_SETTING(f64, mcla_gpu_fsr_min_disocclusion_accumulation, -1.0, "Test lever (FSR 3.1): FSR's fMinDisocclusionAccumulation (its default is -0.333; only 0 to 1 "
    "can be set here). AMD: raising it may reduce the white flicker of thin objects that "
    "disocclude one another often; too high, ghosting. -1: not set, FSR's own default. Passed to the "
    "context once it exists and again when it changes. For A/B. Live.", rex::kDebugOnly, rex::Range{-1.0, 1.0});

  REX_SETTING(std::string, mcla_gpu_fsr_preset, "custom", "With mcla_gpu_fsr on: the internal resolution scale is the screen's (the window's 16:9 "
    "area over the game's 1280x720, followed while the game runs) divided by an FSR1 quality "
    "ratio -- ultra_quality 1.3, quality 1.5, balanced 1.7, performance 2, ultra_performance 3 -- "
    "on the 0.05 grid and at least 0.5, and FSR1 upscales it to the window. custom: "
    "mcla_gpu_resolution_scale (by default the screen's, which leaves FSR1 nothing to upscale: pick "
    "a preset). With mcla_gpu_fsr off it is not used, nor while a temporal upscaler "
    "(mcla_gpu_upscaler) runs. Live, like mcla_gpu_resolution_scale.");

  REX_SETTING(f64, mcla_gpu_fsr_shading_change_scale, 0.0, "FSR 3.1's fShadingChangeScale, 0 and up (FSR's own default is 1). More makes its computed "
    "shading change count for more, so a pixel whose lighting changed lets go of its history "
    "faster; 0, the default here, turns that off (together with the velocity factor of 0, the "
    "strongest flickers of a still scene drop by about two thirds). -1: not set, FSR's own default "
    "is restored. Passed to the context once it exists and again when it changes. Debug only. Live.", rex::kDebugOnly, rex::Range{-1.0, 16.0});

  REX_SETTING(f64, mcla_gpu_fsr_sharpness, 0.2, "FSR1 RCAS sharpness reduction in stops: 0 sharpest, 2 softest (the SDK presenter's "
    "present_fsr_sharpness_reduction and its default). Live.", rex::Range{0.0, 2.0});

  // FSR's own tuning constants, passed through ffxConfigure on the context (FfxApiConfigureUpscaleKey;
  // debug only, live, never saved). -1 is "not set": FSR's own default. A value of 0 or more is passed to
  // the context before its first dispatch and again whenever it changes; the runtime clamps it to its
  // range. FSR 3.1 reads them; FSR 4's provider may refuse the keys (the log says what it answered).
  // The first two are 0 by default (the play build's values: measured on a still scene, the strongest
  // flickers drop by about two thirds, and no ghosting shows while driving); the third is a test lever
  // with FSR's own default.
  REX_SETTING(f64, mcla_gpu_fsr_velocity_factor, 0.0, "FSR's fVelocityFactor, 0 to 1 (FSR's own default is 1). 0, the default here, trusts the history "
    "less where something moved, which steadies bright pixels (on a still scene the strongest "
    "flickers drop by about two thirds, with no visible ghosting while driving). -1: not set, FSR's "
    "own default is restored. Passed to the context once it exists and again when it changes. Debug "
    "only. Live.", rex::kDebugOnly, rex::Range{-1.0, 1.0});

  REX_SETTING(u32, mcla_gpu_ghost_log_interval, 0, "Diagnostic: log one live frame in every N with the 1280x720 scene clears "
    "and three-stripe resolves, plus all EDRAM target creations and growths. 0: off; 1: every "
    "frame. Hot reload for reproducing the frame ghost.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_glow_visibility, "soft", "How the glow of street lamps (their halo and star) and the other glow sprites decide whether they are "
    "hidden by the scene: game (the game's own: one depth test at the glow's centre, so a lamp beside a palm "
    "trunk or a pole pops fully on and off as the wind or the camera moves it by a fraction of a pixel) or "
    "soft (the share of 24 tests on a small disc round the centre scales the glow: a glow in the open is the "
    "game's, one at an edge fades over about two pixels instead of popping; the disc is the same size at every "
    "render scale). game leaves the game's own shaders, and the picture, as they are. Needs the shader pack's "
    "variant file. Live; the first time soft shows, the graphics card builds its shaders (a short stutter).", rex::kDebugOnly, rex::Allowed("game", "soft"));

  REX_SETTING(bool, mcla_gpu_gpu_based_validation, false, "With mcla_gpu_debug_layer on D3D12, enable GPU-based validation before device creation. "
    "Diagnostic; requires the matching D3D12SDKLayers.dll and a restart.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_hdr, "off", "HDR output for an HDR10 display. off: the usual picture. auto: HDR while Windows has HDR "
    "turned on for the display the game is on, following Windows' HDR switch and the display the "
    "window is moved to. on: HDR even if Windows does not say its HDR is on; where the display or "
    "the driver cannot show it the usual picture is used and the log says why. The HUD and the "
    "menus are shown at your paper white setting; the game's lights, the sun, the sky at dusk and "
    "the headlights go above it (how far: mcla_gpu_hdr_highlights). Takes effect within a moment, "
    "at the end of a frame; the picture may flicker once while the window's swap chain is made "
    "again.", rex::Allowed("off", "auto", "on"));

  REX_SETTING(bool, mcla_gpu_hdr_compose_in_present, true, "Diagnostic (HDR output): the present's passes that read the frame 1:1 add the UI layer to the "
    "scene themselves, instead of reading a float image the compose pass made of the two. On by "
    "default: one full-screen float16 pass and its image less (about 0.15 ms and 63 MiB at 4K). Off: "
    "the compose pass and its image as before, for comparing the two. A scaled present and the "
    "debug views of mcla_gpu_hdr_layers_view use the image whatever this says. Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_hdr_highlights, 2.0, "HDR output: how far the game's lights, the sun, the sky at dusk and the headlights go above "
    "the game's white: 1 normal, 1.5 bright, 2 brighter, 3 brightest. It is the steepness of the "
    "HDR curve above where it leaves the game's, so the brightest light stays under the display's "
    "peak whatever it is. Changes at once, also in the world behind the pause menu and the map; "
    "nothing happens while HDR output is off.", rex::Range{1.0, 4.0});

  REX_SETTING(i32, mcla_gpu_hdr_layers_view, 0, "Diagnostic (HDR output): what a frame whose UI was kept as a layer shows. 0 the frame, 1 the "
    "UI layer alone, 2 the scene alone, 3 the UI's coverage as grey, 4 magenta where the scene "
    "is above the game's white, 5 the float scene's difference from the 8-bit scene times 16. "
    "Frames without layers show the front buffer whatever this says. Live.", rex::kDebugOnly, rex::Range{0, 5});

  REX_SETTING(f64, mcla_gpu_hdr_paper_white_nits, 0.0, "How bright the game's white is in HDR, in nits (Windows' own slider covers 80 to 480). 0: "
    "the Windows SDR content brightness setting, or 200 when Windows does not report it. "
    "Changes at once.", rex::Range{0.0, 10000.0});

  REX_SETTING(f64, mcla_gpu_hdr_peak_nits, 0.0, "The brightest level HDR output may reach, in nits (200 to 4000 covers most displays). 0: "
    "what the display reports, or 1000 when it reports nothing. Changes at once.", rex::Range{0.0, 10000.0});

  REX_SETTING(bool, mcla_gpu_hdr_test_pattern, false, "In HDR, shows brightness bars instead of the game. The top row starts at 100 nits and "
    "doubles every 4 bars, to see where the display stops showing steps; the bottom left is "
    "paper white and the bottom right the same bars rolled off to the peak setting. It covers the "
    "whole picture, the menus too. The pause menu's TEST PATTERN row shows it only while that row "
    "is highlighted and never saves it; set on the command line it stays on for the run. Changes at "
    "once.");

  REX_SETTING(bool, mcla_gpu_lock_ahead_verify, false, "Diagnostic: a lock whose pages an earlier lock read ahead still reads them from the pool (it "
    "waits for the GPU as before), and the bytes are compared with the ones kept. Reported in the "
    "log with the prefix 'lock read-ahead check:' (an error line for every page that differs, the "
    "first 8). The vinyl editor is where it counts. Off: nothing is done.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_lock_dump, false, "Diagnostic: after a Lock write-back, every 8888 level the locked range "
                    "covers is decoded from guest memory and written as "
                    "mcla_gpu_lock_<serial>_<base>_<w>x<h>_L<level>.tga next to the executable "
                    "(the vinyl's mip chain).", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_low_latency, "auto", "The GPU vendor's latency reducer: NVIDIA Reflex on an NVIDIA GPU, AMD Anti-Lag 2 on an AMD "
    "one. It holds the game back at the start of a frame just long enough that the frame meets a "
    "free GPU: less input lag when the GPU is the limit, at the price of some frame rate when "
    "it is. auto (Reflex on; Anti-Lag 2 off until it has been tried on a Radeon), off, on, or "
    "boost (Reflex's On + Boost: the GPU clocks stay high; Anti-Lag 2 has no boost and takes it "
    "as on). Where the GPU, the driver or the graphics API has no reducer the pause menu has no "
    "row for it and it stays off whatever this says (the log says why at start). While frame "
    "generation runs (mcla_gpu_frame_generation) it is on whatever this says. Live.");

  REX_SETTING(bool, mcla_gpu_luminance_scale_fix, true, "Fix: the game's auto-exposure (how bright the picture is made, which adapts over time) measures the "
    "same part of the screen at render scales above 1x as at 1x. The game averages the brightness of the "
    "frame from a small set of taps a fixed number of pixels apart; at 2x or 3x the same taps cover a "
    "half or a third of the area they covered at 1x (28% of the picture at 3x), so a small bright light "
    "(a lamp, a lit window, a tail light) moving in and out of one of them can make the brightness pump "
    "or flicker, most at night. This makes each tap average the pixels it stands for. At a render scale "
    "of 1x the picture is the same either way. Off: the game's own taps. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_memory_census, false, "Watch every vertex and index range the executed draws read, the way the buffer cache will, "
    "and log every 600 frames what that cache would upload ("
    "24.21). Diagnostic: it write-protects guest pages.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_msaa, "2", "MSAA of the images of the screen the game multisamples (M6d.2): off (1 sample), 2 (the "
    "console's), 4 or 8 samples on the host. The game itself always draws its own 2 sample "
    "layout; the host gives its images of the screen the samples chosen here, so its pause menu "
    "and map world view, which the game draws with 4 samples, get the same as the game view. "
    "Targets the game does not multisample, or that are not images of the screen, keep the "
    "game's samples. Live: a change is made between two frames, after the pipelines for the new "
    "sample count are made. While an upscaler (mcla_gpu_upscaler) runs, MSAA is 1 sample "
    "whatever this says (the upscaler smooths the picture, and MSAA under it only costs time); "
    "the choice is kept and comes back when the upscaler is off.");

  REX_SETTING(bool, mcla_gpu_msaa_with_upscaler, false, "While FSR or DLSS draws the scene, keep the MSAA level of mcla_gpu_msaa (2, 4 or 8 samples) "
    "instead of 1 sample. The upscaler then gets a scene whose edges and alpha tested foliage are "
    "already smoothed by the sample coverage, and has less aliasing to average over its jitter; it "
    "costs the multisampled scene at the render size. The upscaler's masks (mcla_gpu_upscaler_masks) "
    "are not affected: they are drawn into coverage images with the same samples and averaged, so "
    "the two switches are independent. For A/B. Live (read at start too).", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_object_names, false, "Give the GPU's textures, buffers, shaders and pipelines their debug names, which "
    "RenderDoc, PIX, Nsight Graphics and the D3D12 debug layer show. A name costs a few "
    "microseconds, and a world loads hundreds of textures in one frame, so they are set only "
    "when something can read them: on their own when RenderDoc, PIX or Nsight Graphics started "
    "the game, with mcla_gpu_debug_layer, and with mcla_gpu_perf_markers. This turns them on for "
    "a tool that does not load a DLL (Radeon GPU Profiler) or that attaches later. Read when the "
    "device opens.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_pass_timing, false, "Measure GPU time per pass: a timestamp at every pass boundary (target change, resolve, "
    "clear, present, overlay), summed per pass and logged by cost with the frame statistics. "
    "A measurement for the performance work, off by "
    "default.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_pause_vehicle_vectors, false, "Diagnostic: in the pause menu's world, the garage and the map, take the game's own per-vehicle motion for a "
    "vehicle's pixels in the temporal upscalers' motion vectors, as in the game view. Off (the default): a vehicle "
    "there takes the camera's motion like the world (a car in the garage is still while the camera orbits it, and "
    "the game's slots for the vehicles are not trusted in those frames). Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_perf_markers, false, "Name the frame's phases in the command lists for GPU profilers: PIX events on D3D12, "
    "debug utils labels on Vulkan; how deep, mcla_gpu_perf_markers_level. Nsight GPU "
    "Trace's multi-pass metrics need them. Off: no label is recorded. Read when a frame "
    "starts.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_perf_markers_level, 1, "How deep mcla_gpu_perf_markers names the frame. 0: Frame alone. 1: Frame and its phases "
    "(Scene, EndOfFrame, Present, Overlay) -- the same in every frame, what Nsight's multi-pass "
    "metrics need. 2: also a label per guest clear, resolve, composite and Lock write-back "
    "inside Scene; their count changes from frame to frame, so multi-pass metrics drop them. "
    "Read when a frame starts.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_pipeline_cache, true, "Keep the pipelines the game asked for across sessions ("
    "24.35, M5.3): their keys go to cache/mcla_gpu_pipelines_<backend>.bin beside the "
    "executable on exit, and are read back at start -- the other backend's file too, so what one "
    "API saw is ready in the other (24.40 V2). Off: nothing is read, recorded or written.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_present_dither, true, "Adds a faint noise, at most half of one colour step of the screen, to the finished picture "
    "just before it is shown, so that smooth gradients (the sky at dusk) show fewer bands. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_present_release, true, "Release the guest's Swap as soon as the render thread has read the last guest memory a "
    "present needs (after the scene's end-of-frame work and the front buffer's texture), and "
    "before it submits the frame and presents. Without it the guest waits for the whole present, "
    "about 0.2 ms a frame at 240 FPS. Not used with mcla_gpu_vsync, nor when the previous present "
    "took longer than 1 ms after that point (the GPU is behind): the guest then waits as before, "
    "so it never runs more than that ahead.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_present_release_check, false, "Diagnostic, with mcla_gpu_present_release: checks what that rests on. It releases the guest at "
    "every guest present (also when the GPU is behind), and from the release to the end of the "
    "present it counts, and logs the first 16, each read or write of guest memory by the render "
    "thread. The count is in the '[gpu] present release:' line at shutdown; it must be 0.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_present_wait, false, "Pace presents with the swap chain's latency wait (DXGI waitable object / "
    "VK_KHR_present_wait) when the device has it. On D3D12 Plume then presents with sync "
    "interval 0 and the wait alone paces the frame, so measure before turning it on.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_process_memory_log, 0, "Diagnostic: every this many frames, log what the process's memory is made of (the commit "
    "charge and working set, the committed address space by kind, the private regions by size, "
    "the process heaps, the biggest regions). It walks the whole address space, which takes "
    "some milliseconds on the render thread each time. 0: off.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_readable_cache, true, "Ask the guest heap whether a guest range is readable once a guest frame per 4 KiB page "
    "instead of once a draw: the capture checks about ten ranges a draw, and each check takes "
    "the heap's lock (about 0.2 ms a frame at 2,600 draws). The pages found readable are "
    "forgotten at every present. A page the guest unmaps within the frame and that a later draw "
    "still points at would be read as before the unmapping (the memory stays mapped, so it "
    "cannot fault); off asks the heap at every check, as before.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_readable_cache_check, false, "Diagnostic, with mcla_gpu_readable_cache: also ask the heap at every check the cache answers, "
    "and count and log (the first 16) the answers that differ. The count is in the "
    "'[gpu] readable cache:' line at shutdown; it must be 0. The heap is asked as often as without "
    "the cache.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_reflection_resolution, "auto", "How sharp the road reflection is: the wet roads, the shop and building windows and the water "
    "show a mirror image of the city that the game draws at 256x256 and stretches over the whole "
    "screen, which is why it looks soft. game (the game's own 256x256), high (512x512), ultra "
    "(768x768), or auto (game on an integrated graphics chip, high on any other card). Costs a "
    "little graphics card time and video memory (about 4 MiB for high, 13 MiB for ultra). Live.", rex::Allowed("auto", "game", "high", "ultra"));

  REX_SETTING(f64, mcla_gpu_reflection_scale, 0.0, "Diagnostic: the road reflection's size as a multiple of the game's 256x256 (any step of 0.05), "
    "instead of what mcla_gpu_reflection_resolution says. 0: the setting's. Live.", rex::kDebugOnly, rex::Range{0.0, 4.0});

  REX_SETTING(f64, mcla_gpu_resolution_scale, 0.0, "Render scale: every render target that is an image of the screen (the scene, the UI, the "
    "post-processing sized from the screen) is this many times the guest's in each direction, from "
    "0.5 to 3 in steps of 0.05 (0.75 = 960x540, 1.5 = 1920x1080, 2 = 2560x1440, 3 = 3840x2160 from "
    "720p; other values go to the nearest step, and outside 0.5 to 3 to the nearer end). 0, the "
    "default: the screen's -- the window's 16:9 area over the game's 1280x720, followed while the "
    "game runs (fullscreen, a resized window): the picture is drawn at the size of the window "
    "(\"Render Scale 100%\" in the pause menu); above it is supersampling. Not used while a "
    "temporal upscaler (mcla_gpu_upscaler) runs: its output is always the screen's, and the scene "
    "is drawn at its mode's share of that. Not used either while mcla_gpu_fsr is on with a preset "
    "(mcla_gpu_fsr_preset). Live: the images are made again at the new scale at a frame boundary, "
    "once the value has stood for a moment (and, while an upscaler runs, once its context for the "
    "new sizes is ready); a scale whose images the card cannot make (video memory is short) is not "
    "made: the old one is kept, the pause menu's RENDER SCALE row says LIMITED and the log says so. "
    "The guest keeps seeing its own sizes.");

  REX_SETTING(bool, mcla_gpu_resolve_7e3_ceiling, true, "A resolve of a 7e3 colour target (k_2_10_10_10_FLOAT, the 3D scene) clamps "
                    "each sample's red, green and blue to what the console's target holds, as "
                    "Xenia does: 0 to 31.875 as stored, which with the game's exponent bias of 4 "
                    "is a colour of 1.99 at most. The host's float16 target holds far more, "
                    "and the tone map and the bloom then see lights the console never had: a "
                    "red underglow pool is white in the middle instead of pink. Off: no clamp. "
                    "Not applied to a frame that can carry the HDR float scene (HDR output with "
                    "the HDR pack loaded and no FXAA or SMAA running): its tone map wants the "
                    "range. "
                    "A/B switch.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_resolve_fill_half_pixel, true, "With mcla_gpu_resolution_scale above 1, a resolve of a scaled target reads "
                    "its first host columns and rows -- the half-pixel gap no full-screen draw "
                    "covers -- from the first one surely covered, as Xenia's "
                    "resolve_resolution_scale_fill_half_pixel_offset does. Off: "
                    "the gap keeps whatever was there, a 1-pixel line on the top and left "
                    "edges. A/B switch.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_resolve_memory_check, false, "Diagnostic, slow: every encode also runs at once into a second, reference pool (what "
    "mcla_gpu_resolve_memory_encode_delay 0 gives), and at each reader of the real pool (decode, "
    "vertex copy, lock write-back) and for the whole pool every 600 frames the bytes are compared "
    "with it. Reported in the log with the prefix 'resolve memory check:' every 600 frames and "
    "at shutdown (RESULT PASS or FAIL). For wrong or flickering textures that the delayed encode "
    "may cause: the answer is in the log. Off: nothing is done.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_resolve_memory_encode_delay, 2, "Frames a colour resolve's encode into the resolve memory (and that of a depth resolve into "
    "k_8_8_8_8) may wait. 0: every "
    "encode of a frame is recorded at its end. N: recorded once a reader or writer "
    "of its pages needs it, or after N frames; a resolve that rewrites the same rectangle "
    "replaces it before it runs, so most are never recorded (75% on a drive). 2 by default: on a "
    "Radeon iGPU 57 FPS against 52 at 0. At most 120. Hot reload.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_road_reflection_cars_test, 0, "Debug test, live, never saved: lets the game's own code draw cars into the wet-road reflection. 0 = off "
    "(the game as it is). 1 = the player's car. 2 = the player's car and the traffic. It opens the game's "
    "phase-2 vehicle draw (a camera mirrored about the player's ground height) only while a car is driven, "
    "not in a menu, the pause screen, the map or the garage. It may draw nothing (the car materials may "
    "have no reflection pass) and may thin out the other reflections while on; the log (\"[road refl cars]\") "
    "says how many vehicle draws went in.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(bool, mcla_gpu_scale_depth_bias, true, "With a resolution scale above 1 (M6b.3c): the slope-scaled depth bias times the "
    "scale of the draw's targets, as Xenia does -- a host pixel is 1/scale of the guest's. Off: "
    "the guest's factor unchanged (for A/B). Hot reload.", rex::kDebugOnly);

  // The scene's extra texture detail bias under an upscaler (Scene::SceneBias): live, for A/B.
  REX_SETTING(f64, mcla_gpu_scene_extra_bias, -1.0, "While an upscaler draws the scene smaller than it is shown, its textures are sampled by "
    "log2(render size / output size) levels of detail lower, as the output's size would pick, plus "
    "this many levels more. -1 is what AMD's FSR guide asks for (a sharper picture the upscaler "
    "fills in over its jitter); 0 or -0.5 sample blurrier levels, which shimmer less on fine "
    "texture detail (grass, road grain, fences) and soften it. For A/B. Live.", rex::kDebugOnly, rex::Range{-3.0, 1.0});

  REX_SETTING(bool, mcla_gpu_scene_profile, false, "Diagnostic (M6a): the render thread's time per stage of a draw (constant banks, "
    "state decode, shaders and layout, geometry, pipeline, constants, textures, upload, targets, "
    "draw), one line per window of 600 frames. Off: nothing is timed.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_shader_memo_check, false, "Diagnostic: no draw takes its shaders from the frame's identity memo; every draw reads the "
    "guest's shader objects and looks the identity up in full, and the memo's answer, address, "
    "size and variant are compared with it. The differences are counted and logged (the first 16); "
    "the count is in the '[gpu] shader memo:' line at shutdown and must be 0. The lookups cost as "
    "much as without the memo.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_shadow_filter, "auto", "How the sun's shadows are filtered: game (the game's own: four taps at an angle that changes from "
    "pixel to pixel, a grainy edge), soft (more taps on a disk, turned by noise of the screen pixel, a "
    "smooth edge of the same width, or narrower on a finer shadow map: SHADOW DETAIL), pcss (soft, with "
    "contact hardening: a shadow is sharp where it touches what casts it and softer with the distance "
    "from it, as the sun's own shadows are; an integrated graphics card gets soft). auto (the default) "
    "picks by the graphics card: game on an integrated one, soft on the rest. The sun's shadows show from "
    "8 to 17 hours in clear weather. Costs graphics card time in the screen's shadowed pixels (pcss "
    "about the same as soft: away from the edges it takes fewer taps). Live; the first time soft or pcss "
    "shows, the graphics card builds its shaders (a short stutter).", rex::Allowed("auto", "game", "soft", "pcss"));

  REX_SETTING(bool, mcla_gpu_shadow_filter_pcf, false, "Diagnostic: how a tap of the soft shadow filter reads the shadow map. Off: with the game's own "
    "sampler, which compares a depth blended from four texels (what the game's four taps do). On: "
    "it compares the four texels one by one and blends the answers (a true percentage-closer tap); "
    "the shadow edge is a little smoother and its bias a little different. Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_shadow_filter_radius, 1.0, "Diagnostic: how wide the soft shadow filter is, as a multiple of the game's own filter measured in "
    "texels of the shadow map (1: the edge of a shadow takes the same number of shadow map texels, so "
    "the same width as the game's on its own 1280x1280 map, half as wide on high and a third on ultra: "
    "mcla_gpu_shadow_resolution). Live.", rex::kDebugOnly, rex::Range{0.25, 4.0});

  REX_SETTING(f64, mcla_gpu_shadow_filter_sun, 4.0, "Diagnostic: how big the sun is for the contact hardening of pcss, as a multiple of its real angular "
    "radius (0.27 degrees). 1 is the real sun, whose penumbra is narrower than one texel of the shadow map "
    "for anything lower than about 3 metres, so a shadow is sharp everywhere; a larger sun makes the "
    "edge of the shadow of a car or a pole visibly softer with the distance from what casts it (the "
    "width is capped at twice the soft filter's). Live.", rex::kDebugOnly, rex::Range{0.25, 15.0});

  REX_SETTING(i32, mcla_gpu_shadow_filter_taps, 0, "Diagnostic: how many taps the soft shadow filter takes, 4 to 32. 0: by the graphics card (8 on an "
    "integrated one, 16 on the rest). Live.", rex::kDebugOnly, rex::Range{0, 32});

  REX_SETTING(std::string, mcla_gpu_shadow_resolution, "auto", "How fine the sun's shadows can be: the game draws them into four 640x640 maps in one 1280x1280 "
    "picture, which limits how sharp an edge can get. game (the game's own 1280x1280), high "
    "(2560x2560), ultra (3840x3840), or auto (game on an integrated graphics chip, high on any other "
    "card). With SHADOW FILTER on soft the edge of a shadow is half as wide on high and a third as "
    "wide on ultra; with the game's own filter it keeps its width and gets smoother. Costs a little "
    "graphics card time and video memory (about 25 MiB for high, 56 MiB for ultra, and as much again "
    "for the maps being drawn). The sun's shadows show from 8 to 17 hours in clear weather. Live; "
    "the first time it changes the graphics card builds its shaders (a short stutter).", rex::Allowed("auto", "game", "high", "ultra"));

  REX_SETTING(std::string, mcla_gpu_skip_shaders, "", "Workaround for a driver that dies compiling one particular shader combination: a "
    "comma-separated list of shader identities (the 16 hex digits of the pack's keys). A "
    "guest pipeline that uses any of them is not built, and its draws are treated like those "
    "of a pipeline that failed. Measured on Mesa's dzn (Vulkan over D3D12 in WSL2), which "
    "overflows its stack in vkCreateGraphicsPipelines for the pair b000e9d48f010bd8 + "
    "68387d4a0b431769 (each alone is fine).", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(f64, mcla_gpu_split, -1.0, "TRAILER TOOLKIT: a before/after split in one frame. A position from 0 to 1 across the picture's width: the effects "
    "named by mcla_gpu_split_effects (ambient occlusion, the present's dither) are applied only left of it, and right of "
    "it the picture is drawn without them. -1 is off (the default; nothing changes and nothing costs), 1 is the effect "
    "everywhere. A scene's `split <a>..<b> over <s>` keys write it; a compositor draws the divider line. Live.", rex::kDebugOnly, rex::Range{-1.0, 1.0});

  REX_SETTING(std::string, mcla_gpu_split_effects, "all", "TRAILER TOOLKIT: which effects mcla_gpu_split cuts: ao (ambient occlusion only), dither (the present's dither only) "
    "or all. An effect that is off in its own setting stays off. Used only while mcla_gpu_split is on. Live.", rex::Allowed("all", "ao", "dither"), rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_ssgi, "auto", "Bounce light: the light a lit wall or the ground sends back onto the road, the sides of cars and "
    "the walls beside it, found from the picture, so a red wall tints the pavement by it and a street "
    "lamp's pool lights the wall next to it. It takes the place of part of the sky light the ambient "
    "occlusion darkens, so a lit area does not get brighter overall, and a flat surface stays as it "
    "was. Needs ambient occlusion (mcla_gpu_ao) on. off, high (a coarser search: 2 directions and 2 steps "
    "each; half resolution where the occlusion is high too), ultra (3 and 3 at full resolution) or auto (the "
    "default): off on every graphics card for now, until its cost is measured. The pause menu and the "
    "launcher show the same words. A value that is none of these (the low of an older larecomp.toml) is "
    "ignored and the setting runs as auto. Costs graphics card time, "
    "ultra more than high. Also drawn behind the pause menu and in the garage, not on the map. Live.", rex::Allowed("auto", "off", "high", "ultra"));

  REX_SETTING(f64, mcla_gpu_ssgi_clamp, -1.0, "Diagnostic: replaces the level's gain limit of the bounce light (mcla_gpu_ssgi_intensity), 0.05 to 10.2: the "
    "most the bounce may add to a pixel, as a multiple of that pixel's own colour (its largest channel; the hue "
    "stays; a soft limit that only guards a nearly black pixel's noise, so a shadow can gain several times "
    "its own light). The brightest the bounce can make a pixel is set by the cap, not by this. -1: the "
    "level's. Live.", rex::kDebugOnly, rex::Range{-1.0, 10.2});

  REX_SETTING(i32, mcla_gpu_ssgi_far_steps, -1, "Diagnostic: the far ring's samples on each side of each direction, 0 to 7 (0: no ring). -1: by the bounce "
    "light's quality (2 at ultra, 1 at high). Live.", rex::kDebugOnly, rex::Range{-1, 7});

  REX_SETTING(bool, mcla_gpu_ssgi_half, true, "Diagnostic: above 1080 lines of render height the bounce light (every quality, high and ultra) runs at half the "
    "picture's size, like mcla_gpu_ao_half_above_1080 does the occlusion, and its colour chain takes one of the "
    "colour target's MSAA samples a pixel (the four pixels of a 2x2 block give the texel) instead of all of them: the "
    "bounce light is the lowest frequency signal of the picture (it gathers 5 to 16 metres and is filtered over up to "
    "30 pixels), so the picture is the same to within a thin edge and its passes cost a quarter and the images a "
    "quarter of the memory. 1080 lines and below are as they were. False: the sizes and the chain as before "
    "(mcla_gpu_ssgi_low says the old half size choice). Live; the first frame after the change makes the images again.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_ssgi_intensity, "default", "How strong the bounce light looks: light, default, strong, stronger or strongest. Each is a set of four "
    "numbers: how much of the light the surroundings send back the picture gets, the most it may add to a pixel "
    "(a guard against noise in the dark), how far it looks for lit surfaces (5 to 16 metres, past the ambient "
    "occlusion's own reach), and the brightest light it may bring (never whiter than a white wall in full sun). "
    "The stronger ones lift shadows and the sides of cars more and look farther. Only used while the bounce light "
    "(mcla_gpu_ssgi) is on (the pause menu's BOUNCE LIGHT STRENGTH row). Live.", rex::Allowed("light", "default", "strong", "stronger", "strongest"));

  REX_SETTING(i32, mcla_gpu_ssgi_low, 0, "Diagnostic: what the bounce light's high does (mcla_gpu_ssgi high or auto that comes to high) to be cheaper "
    "than ultra: 0 fewer directions (2 slices and 2 steps each instead of 3 and 3) at the occlusion's own "
    "size, 1 half the picture's size (the occlusion's slices and steps) even where the occlusion is high, 2 "
    "both. The same samples give the occlusion's visibility, so 1 and 2 also make the occlusion's own "
    "picture half resolution while the bounce light is on. Live.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(f64, mcla_gpu_ssgi_radius, 1.0, "Diagnostic: how far the bounce light's NEAR samples look, as a multiple of the ambient occlusion's radius "
    "(mcla_gpu_ao_intensity); the same samples give the occlusion's visibility, so a change moves that too. 1: the "
    "occlusion's own. (The far ring that reaches past them is mcla_gpu_ssgi_reach.) Live.", rex::kDebugOnly, rex::Range{0.5, 3.0});

  REX_SETTING(f64, mcla_gpu_ssgi_reach, -1.0, "Diagnostic: replaces the level's reach of the bounce light (mcla_gpu_ssgi_intensity), 0 to 16 metres: a "
    "second ring of samples from the occlusion's radius out to this, into what the near samples left open (the "
    "occlusion's own look does not change). 0: no ring. -1: the level's. Live.", rex::kDebugOnly, rex::Range{-1.0, 16.0});

  REX_SETTING(f64, mcla_gpu_ssgi_strength, -1.0, "Diagnostic: replaces the level's strength of the bounce light (mcla_gpu_ssgi_intensity), 0 to 12: how much of "
    "the bounce light the picture gets (1: all that the surroundings send back by the picture's own colours; 0 "
    "runs the passes and adds nothing). -1: the level's. Live.", rex::kDebugOnly, rex::Range{-1.0, 12.0});

  REX_SETTING(bool, mcla_gpu_ssgi_test, false, "Diagnostic: the debug override of the bounce light (mcla_gpu_ssgi): forces its passes on, at the "
    "occlusion's own quality and whatever mcla_gpu_ssgi says (only while mcla_gpu_ao is not off). The "
    "scene colour at the occlusion's trigger is kept in a chain of images and the occlusion's main "
    "pass becomes a visibility bitmask that also finds what colour the blockers of each pixel's sky "
    "light are, then accumulated over frames and filtered. The occlusion's own look stays (its "
    "visibility now comes from the bitmask); the bounce is added to the picture as the setting adds it "
    "(mcla_gpu_ssgi_strength 0: not added), or shown by mcla_gpu_ssgi_test_view. Costs about as much "
    "as the occlusion again. Live.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_ssgi_test_atrous, 2, "Diagnostic: the iterations of the a-trous filter that takes the noise out of the bounce light after the "
    "history, for a picture of 720 lines, 0 to 3 (0: none; each iteration reaches twice as far: 5x5, 9x9, 17x17 "
    "pixels, and stops at depth edges and corners). A bigger picture gets one more for each doubling of its "
    "height (3 at 1080 and 1440 lines, 4 at 2160, at most 5), so the filter reaches as much of the picture; "
    "mcla_gpu_ao_stable false keeps this number at every size. The history itself is never filtered. Live.", rex::kDebugOnly, rex::Range{0, 3});

  REX_SETTING(f64, mcla_gpu_ssgi_test_clamp, -1.0, "Diagnostic: replaces the level's cap (mcla_gpu_ssgi_intensity), 0.25 to 15.75: the brightest radiance a "
    "sample of the scene colour may carry into the bounce, and the ceiling the bounce may fill a pixel up to, as a "
    "multiple of the frame's ambient plus sun light (never under 0.5): a street lamp, neon or the sun's disc is a "
    "source, not a surface that bounces light, and may light its neighbours no whiter than a white wall in full "
    "sun. -1: the level's. Live.", rex::kDebugOnly, rex::Range{-1.0, 15.75});

  REX_SETTING(f64, mcla_gpu_ssgi_test_facing, 1.0, "Diagnostic: how much a sample's surface must face the pixel to light it, 0 to 1: 0 takes every "
    "surface to face the pixel, 1 weighs each by the slope of its depth along the slice (a roof seen "
    "from the ground, a wall seen from behind: nothing). Costs one more depth read per lit sector. "
    "Live.", rex::kDebugOnly, rex::Range{0.0, 1.0});

  REX_SETTING(i32, mcla_gpu_ssgi_test_history, 16, "Diagnostic: how many frames the bounce light test averages the bounce over, 2 to 64 (the "
    "occlusion's own visibility keeps mcla_gpu_ao_history). The bounce is found again in the last frame "
    "by the camera's and the vehicles' motion and kept only where it is the same surface; more frames is "
    "smoother and slower to follow a light that changed, fewer is noisier. Live.", rex::kDebugOnly, rex::Range{2, 64});

  REX_SETTING(f64, mcla_gpu_ssgi_test_history_clamp, 2.0, "Diagnostic: how far the bounce's history may be from this frame's 3x3 neighbourhood of the bounce, in "
    "standard deviations of that neighbourhood (never under 5% of its mean, and 15% of the history's own value "
    "wider on each side): the history is cut to that box. Narrow follows a changed light at once and keeps more "
    "noise (the first version, 1.25 with no share of the history, made the accumulated bounce 2.5 to 3.5 times as "
    "noisy); wide keeps the history longer and leaves a ghost of a car that went by. Live.", rex::kDebugOnly, rex::Range{0.25, 8.0});

  REX_SETTING(f64, mcla_gpu_ssgi_test_thickness, 0.0, "Diagnostic: how thick the surfaces the bitmask finds are assumed to be, in the game's world units "
    "(about metres): a sample blocks the directions between its front and its back, the back being "
    "this far behind the front along the view ray. 0: solid, a sample blocks everything below its "
    "horizon (the occlusion's own horizon search, to within 1/32). Live.", rex::kDebugOnly, rex::Range{0.0, 4.0});

  REX_SETTING(i32, mcla_gpu_ssgi_test_view, 0, "Diagnostic: shows a stage of the bounce light test as a picture instead of the scene colour "
    "(only while mcla_gpu_ssgi_test is on, and instead of the other debug views): 1 the bounce light "
    "the main pass found (one frame, noisy), 2 the scene colour chain's level 0 (the colour the "
    "blockers are taken from: averaged over the samples, capped), 3 its level 1, 4 its level 2, 5 the "
    "bounce light cleaned by a 5x5 mean, 6 the visibility the bitmask found (one frame, noisy: the "
    "occlusion's own view 2 shows it after the denoise and the history), 7 the bounce light after the "
    "accumulation over frames (the history), 8 the bounce light after the a-trous filter too (what the "
    "picture's bounce will be made of), 9 how much of the history each pixel kept (white: all it can, "
    "black: none, so it is new, a vehicle whose own motion is not known, or has no history yet). Shown at "
    "0.3 of its value times mcla_gpu_ssgi_test_view_gain (view 9 at full white). 0: off. Live.", rex::kDebugOnly, rex::Range{0, 9});

  REX_SETTING(f64, mcla_gpu_ssgi_test_view_gain, 1.0, "Diagnostic: the brightness of mcla_gpu_ssgi_test_view's picture, 0.1 to 16 times. Live.", rex::kDebugOnly, rex::Range{0.1, 16.0});

  REX_SETTING(i32, mcla_gpu_ssgi_view, 0, "Diagnostic: shows a stage of the bounce light's apply as a picture instead of applying it (only "
    "while the bounce light is on, and instead of the occlusion's and the contact shadows' views; the "
    "test's own views come first): 1 the factor the picture is multiplied by, in colour (the "
    "occlusion's plus the bounce), 2 the bounce each pixel gets (four times as bright as the others), "
    "3 the albedo estimate (the colour over the light the game's constants say fell on it, up to 1), "
    "4 that light, 5 how much of the colour is a surface (black: a source of light, which gets no "
    "bounce), 6 the change of the picture against the game's own (grey: none; lighter: brighter; "
    "darker). Shown at 0.3 of the value, except 6. 0: off. Live.", rex::kDebugOnly, rex::Range{0, 6});

  REX_SETTING(std::string, mcla_gpu_sun_shafts, "auto", "Sun shafts: a low sun behind trees and buildings streams light across the scene, as light "
    "through a haze. They show at dawn and dusk (strongest between 2.5 and 27 degrees of sun "
    "height, none by day above 45 degrees, none at night or in rain) and only while the sun is on "
    "the screen or not far beyond its edge. The value is how strong they are: off is the game's own "
    "picture, then light, default, strong, stronger and strongest (1x to 5x the built-in strength; default is 2x). "
    "auto (the default) is default on a discrete graphics card and off on an integrated one (a "
    "handheld, or the iGPU of a desktop CPU). The ray quality follows the card by itself. low and high "
    "are values of an older version and mean default. Costs graphics card time only while they show. Live.", rex::Allowed("auto", "off", "light", "default", "strong", "stronger", "strongest", "low", "high"));

  REX_SETTING(bool, mcla_gpu_sun_shafts_cap, true, "Diagnostic: caps the sun shafts' two working images at 960x540 (the size they are at 1080p, a quarter "
    "of their old size at 4K, the same shape as the picture; a picture whose half size is under that is "
    "untouched). The shafts are a smooth, blurred light, so the picture is the same to within the ray "
    "noise being coarser; it saves a good part of the passes' time at 1440p and 4K while the shafts "
    "show. False: the images are half the picture's size, as they were. Live.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_gpu_sun_shafts_census, 0, "Diagnostic: every Nth guest frame (and the first three frames with the main view), one log "
    "line of what the sun shafts did: the sun's height and where it is in the picture, the "
    "fades, what became of the frame's two points. 0: off. Live.", rex::kDebugOnly, rex::Range{0, 100000});

  REX_SETTING(f64, mcla_gpu_sun_shafts_density, 0.9, "Diagnostic: how much of the way to the sun each ray of the sun shafts covers, 0.3 to 1 "
    "(1: all of it). Longer rays are longer streaks; shorter ones stay near the sun. Live.", rex::kDebugOnly, rex::Range{0.3, 1.0});

  REX_SETTING(bool, mcla_gpu_sun_shafts_early, false, "Diagnostic: adds the sun shafts at the game's mid-scene depth copy, before the fog and the "
    "particles, instead of at the game's copy of the main colour after them (the fog then covers "
    "them), to compare the two. Without it the shafts go in at that copy and fall back to the "
    "mid-scene point by themselves when no such copy comes. Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_sun_shafts_intensity, 1.0, "Diagnostic: a fine multiplier on the strength mcla_gpu_sun_shafts gives the sun shafts (1 is "
    "the level as it is). Live.", rex::kDebugOnly, rex::Range{0.0, 8.0});

  REX_SETTING(f64, mcla_gpu_sun_shafts_sky_depth, 1e-5, "Diagnostic: the stored depth (1 near, 0 far) at or under which a pixel is sky for the sun "
    "shafts. The sky is drawn at the far end, 0; raise it if the shafts find no sky. Live.", rex::kDebugOnly, rex::Range{0.0, 0.01});

  REX_SETTING(i32, mcla_gpu_sun_shafts_steps, 0, "Diagnostic: how many steps each ray of the sun shafts marches (8 to 128). 0: by the graphics "
    "card (32 on a mid-range one, 64 on a fast one). More steps make smoother rays and cost more; "
    "the light the rays gather stays the same. Live.", rex::kDebugOnly, rex::Range{0, 128});

  REX_SETTING(f64, mcla_gpu_sun_shafts_threshold, 0.5, "Diagnostic: the brightness (the scene's linear light, 1 is white) the sky must pass to light "
    "the sun shafts; what is under it does not shine. Lower lets more of the sky shine. Live.", rex::kDebugOnly, rex::Range{0.0, 4.0});

  REX_SETTING(i32, mcla_gpu_sun_shafts_view, 0, "Diagnostic: shows a stage of the sun shafts instead of adding them (only while they are on "
    "and the sun gives shafts): 1 the sky image the rays march over, 2 the blurred image, 3 the "
    "added light alone. 0: off. Live.", rex::kDebugOnly, rex::Range{0, 3});

  REX_SETTING(bool, mcla_gpu_sync_builds, false, "Trailer toolkit (M2d): while on, nothing is left out for a pipeline that is still being made. "
    "A draw whose pipeline a builder thread has not finished (\"skipped [pipeline pending ...]\" in "
    "the scene lines), and the second draws of the HDR scene and the upscaler masks, wait for it "
    "on the render thread instead, so no frame of a recording misses an object. The frames that "
    "make a new pipeline are slow while it is on. The log counts the waits and their time "
    "(\"[gpu] capture sync\"). Capture mode turns the same switch on by itself while it records. "
    "Off: as before. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_temporal_jitter_hold, true, "Diagnostic: hold the temporal upscalers' sub-pixel jitter at zero while the history restarts in every frame (a "
    "camera the cut rule or the game's previous-matrix test cannot follow): a frame resolved from one frame is "
    "steadier unjittered. Off: the jitter goes on as before. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_texture_decode_selftest, false, "Test only: at start-up, decodes a few thousand random texture levels (every block size, "
    "endian, tiled and linear, packed-tail offsets, DXT decompression) with the fast decoder and "
    "with the plain per-block one, and logs whether every byte matched.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, mcla_gpu_texture_decode_threads, -1, "Threads that decode the textures of the draws queued behind the one the renderer is on "
    "(untile, byte swap), so a burst of new textures as the game streams in the next stretch of "
    "road does not stall the render thread. -1: one thread per 4 logical processors, up to 3, none "
    "below 4 processors. 0: the render thread decodes every texture itself, when its draw needs "
    "it. The threads run below the game's.", rex::kDebugOnly, rex::Range{-1, 8}, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_texture_decode_verify, false, "Test only: every texture level the game uploads is decoded a second time with the plain "
    "per-block decoder and compared byte for byte with the fast one's; a difference is logged "
    "once per texture. Slow: it brings the old cost back.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_texture_dump, false, "Write every texture the NATIVE renderer uploads to <mods>/textures_dump/<dictionary>/"
    "<name>__<hash>.dds, once per texture per session (F2 does it for one frame). Texture mods "
    "replace a texture by the hash at the end of the file name, from "
    "<mods>/<Mod>/textures_replace/; <mods> is modloader/, mods/ or models/.", rex::kDebugOnly);

  REX_SETTING(u32, mcla_gpu_trace_count, 1, "How many consecutive frames mcla_gpu_trace_frame writes, one file each.", rex::kDebugOnly);

  REX_SETTING(u64, mcla_gpu_trace_frame, 0, "Write a raw trace of the guest frame that begins after this many guest "
                      "frames (0 = off) to mcla_gpu_trace_<frame>.txt next to the executable. "
                      "Decoded by tools/gpu/trace_analyze.py. A 6000-draw frame is tens of MB.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_traffic_shadows, "auto", "Traffic and the other AI cars cast into the sun's shadow map like the player's car, so the "
    "ground under them is dark (the game only draws the player's car into it). off: the game's own "
    "picture. near: the nearest few cars within a short distance of the camera. all: every car "
    "within a long distance (costs more graphics and processor time). auto (the default): near on a "
    "discrete graphics card, off on integrated graphics. Daylight only (the game has no sun shadow "
    "at night and in rain). Live: it applies to the next frame, to the cars already on the road.", rex::Allowed("auto", "off", "near", "all"));

  REX_SETTING(i32, mcla_gpu_traffic_shadows_cars, 0, "Diagnostic: the most cars that cast into the sun shadow map at once (the nearest ones). 0 is "
    "the setting's own (near 4, all 16); at most 24. Live.", rex::kDebugOnly, rex::Range{0, 24});

  REX_SETTING(i32, mcla_gpu_traffic_shadows_cascades, 0, "Diagnostic: the cars are drawn into the first this many of the four sun shadow cascades "
    "(each one costs the cars' draws again). 0 or 4 is all four. Live.", rex::kDebugOnly, rex::Range{0, 4});

  REX_SETTING(f64, mcla_gpu_traffic_shadows_radius, 0.0, "Diagnostic: how far from the camera a car may be and still cast into the sun shadow map, in "
    "metres. 0 is the setting's own (near 40, all 150). Live.", rex::kDebugOnly, rex::Range{0.0, 1000.0});

  REX_SETTING(std::string, mcla_gpu_upscaler, "off", "Upscaling of the 3D scene: off, fsr31 (AMD FSR 3.1), fsr4 (AMD FSR 4, only where the "
    "graphics card and driver run it) or dlss (NVIDIA DLSS, only on an NVIDIA RTX card, with "
    "NVIDIA's files next to the game). The scene is moved by a different sub-pixel amount every "
    "frame and the upscaler rebuilds a sharper, steadier picture from the frames; the menus, the "
    "HUD and the bloom, glow and tone map stay at full size. The picture it makes is the size of "
    "the window (its 16:9 area), whatever mcla_gpu_resolution_scale says, and the scene is drawn "
    "at the mode's share of that. While one is on, FXAA and SMAA are "
    "held off and MSAA is 1 sample (your MSAA choice, mcla_gpu_msaa, is kept and comes back when "
    "the upscaler is off). A choice this PC cannot run is treated as off. Live: turning an "
    "upscaler on, off or to another one is made at a frame boundary once the new one is ready, "
    "while the old one keeps working (FSR in about half a "
    "second, DLSS in several seconds). The exception is DLSS on Vulkan: that graphics device is "
    "made through NVIDIA's Streamline only when dlss is the saved choice at the start (it adds "
    "several seconds to every start), so choosing dlss in a Vulkan game started with something "
    "else is saved and applies at the next start, and the menu offers a restart. Until then the "
    "game keeps what it runs (the upscaler and its mode as they were, or none), it is not turned "
    "off.");

  REX_SETTING(bool, mcla_gpu_upscaler_lazy_load, true, "Debug (A/B): load the upscalers' libraries only when they are needed. On (the default): "
    "AMD's FidelityFX runtime (amd_fidelityfx_upscaler_*.dll, 35 to 55 MiB) is opened when an FSR "
    "is first used, not at every start (an AMD graphics card still asks it at the start, because "
    "only the runtime can say whether FSR 4 is offered), and NVIDIA's Streamline and DLSS (D3D12 on "
    "an RTX card) start their background warm-up only once the pause menu's UPSCALING row has been "
    "shown or DLSS frame generation is chosen, not five seconds after every start; DLSS and FSR "
    "chosen at the start load at the start as always. Off: the old behaviour, FidelityFX opened by "
    "every start and the DLSS warm-up five seconds after the game first draws. Read at the start.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, mcla_gpu_upscaler_mask_test, 0, "Diagnostic: tests whether the upscalers read their mask inputs. 1 gives the reactive mask (what "
    "FSR calls reactive; DLSS's bias-current-colour and reactive hints) every pixel at 1, 2 gives the "
    "transparency and composition mask every pixel at 1, 3 both. An upscaler that reads a mask then "
    "stops trusting its history: the picture loses its smoothness over time (edges crawl, thin lines "
    "flicker); one that does not read it looks as before. Only while FSR or DLSS runs. 0: the real "
    "masks. Live.", rex::Range{0, 3}, rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_upscaler_masks, true, "Whether the upscalers get the reactive mask (the cars' shadow decals, particles, light "
    "volumes) and the transparency and composition mask (car glass): on, they are drawn "
    "(upscaler_masks.h). Off: no mask is made and the upscaler trusts its history everywhere, "
    "for A/B (a mask that is wrong shows as shimmer where it covers). Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_upscaler_post_reads_output, true, "While FSR or DLSS draws the scene at the output's size (FSR's Native AA, DLAA): the game's post "
    "chain before the upscale (the half-size downsample that the bloom's bright pass, the exposure "
    "and the motion blur's half-size layer are made from) reads the previous frame's upscaled "
    "picture, as it does at every other mode (on, the default since 2026-11-14: measured on a still "
    "pause view with FSR Native AA, flat areas 8x and the distant tower 3x steadier). Off: the old "
    "path, the raw jittered scene at that size, kept for A/B. Live.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_gpu_upscaler_sharpness, 0.2, "How much the FSR upscalers sharpen their picture (AMD's RCAS pass), 0 to 1: 0 adds no "
    "sharpening, 1 the most. DLSS has no sharpening. Live.", rex::Range{0.0, 1.0});

  // Test levers for the static-scene flicker analysis (debug only, live, never saved; defaults = the
  // behaviour without them).
  REX_SETTING(bool, mcla_gpu_upscaler_zero_jitter, false, "While FSR or DLSS draws the scene: no sub-pixel jitter at all. The projection is not moved and "
    "the upscaler is told a jitter of 0 (one value, so the draws, the motion vectors and the upscaler "
    "agree). The upscaler then has no sub-pixel samples to rebuild detail from and the picture is "
    "no sharper than the render size; a flicker that follows the jitter phase is gone with it, one "
    "that stays is not the jitter's. For A/B. Live.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_validate, false, "Run the milestone M3 validator on every record besides the replay: per-record counters and "
    "the mcla_gpu_validation_*.csv files next to the executable. Decodes every draw a second "
    "time on the render thread (3.5-7 ms/frame measured in M3).", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, mcla_gpu_verify_constants, false, "Compare every 64-byte block of both constant banks on every draw, not only the blocks the "
    "guest's dirty masks name, and send (and count) any that changed without its dirty bit. "
    "Costs guest CPU per draw. Off by default since M3 measured no block outside the masks; the "
    "guest's XXH3 of both banks every 32 draws still checks the rebuilt banks "
    ".", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_vram_log, 0, "Diagnostic: every this many frames (at the next check of the video memory, every 30 frames), "
    "log how much video memory the process uses against the budget the system gives it, and what "
    "the texture, resolve product, EDRAM target and resolve memory caches hold, whether or not "
    "memory is short. For a report of stutter or crashes on a card with little video memory. "
    "0: off.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_vsync, false, "Present on vertical blank. Read every frame.");

  REX_SETTING(std::string, mcla_gpu_watch_base, "", "Diagnostic: a guest physical address in hex. Every resolve into, product "
    "activated or deactivated over, texture fetched from and resolve-memory decode of "
    "[base, base + mcla_gpu_watch_size) is logged in order with the guest frame and draw: the "
    "last 1500 events are kept until the first decode of the range, then written out with "
    "every later one (at most 20000 lines). Empty: off.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(u32, mcla_gpu_watch_capture, 1, "Diagnostic, with mcla_gpu_watch_base: a RenderDoc capture of the frame after 1 = the first "
    "decode of the range from the resolve memory, 2 = the first product over it deactivated by "
    "another resolve. 0: none. Needs the game started under RenderDoc.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(std::string, mcla_gpu_watch_size, "400000", "Diagnostic: the length, in hex, of mcla_gpu_watch_base's range.", rex::kDebugOnly, rex::kRestart);

  // The switch over the renderer work of 17/10/2026, asked for by
  // the user, live: on, every part of it runs as its own cvar says; off, the
  // renderer does what it did before that day.
  REX_SETTING(bool, mcla_gpu_work_17oct, true, "On/off for all of the renderer work of 17/10/2026: the half-pixel fill of "
    "scaled resolves (mcla_gpu_resolve_fill_half_pixel), the depth bias times the scale "
    "(mcla_gpu_scale_depth_bias), the delayed colour encodes "
    "(mcla_gpu_resolve_memory_encode_delay) and the encode batch counters. Off: as before "
    "that day. Hot reload.", rex::kDebugOnly);

  REX_SETTING(bool, precache_pipelines, false, "Prepares the graphics pipelines the game compiles while it starts, and closes, without "
    "starting the game: no window, no graphics output. The launcher runs this at the end of its "
    "setup (after the shader packs are built), so that the first start has no shaders to compile "
    "and no pipeline pending. It fills the graphics driver's own cache for this PC; it does not "
    "have to be repeated until the driver, the game files or LARecomp change (the next run "
    "notices and does nothing when everything is current). Progress is printed to the console. "
    "Exit code 0 when the pipelines are prepared or need nothing.", rex::kRestart, rex::kDebugOnly);

  REX_SETTING(std::string, precache_pipelines_backend, "", "With precache_pipelines: which graphics API to prepare. Empty (or auto) is the one the game "
    "runs on (graphics_backend). d3d12 or vulkan prepares that one; one API per run.", rex::kRestart, rex::kDebugOnly);

  REX_SETTING(bool, precache_pipelines_force, false, "With precache_pipelines: prepare even when the last run says the pipelines are current. For "
    "after the graphics driver's shader cache was cleared (Windows' Disk Cleanup does that).", rex::kRestart, rex::kDebugOnly);

  REX_SETTING(i32, precache_pipelines_threads, 0, "With precache_pipelines: how many threads compile the pipelines. 0 uses every processor but "
    "one. A lower number leaves the PC more usable meanwhile, and the preparation takes longer.", rex::Range{0, 16}, rex::kRestart, rex::kDebugOnly);

  REX_SETTING(std::string, shader_pack_dir, "", "The folder the shader packs (mcla_shaders.v8.pack and mcla_shaders_variants.v2.pack) are read from "
    "and built into. Empty: the cache/shaders folder next to the program or, where that folder "
    "cannot be written to, larecomp/cache/shaders in the local application data folder; a pack "
    "left in the program's assets folder by an older install is still used when there is none "
    "in that folder. When this is set, only this folder is used.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, shader_pack_threads, 0, "How many threads build the shader packs from your game files on the first start. 0 chooses "
    "by itself from the processor's cores and the free memory. A lower number leaves the PC more "
    "usable during the build, which then takes longer.", rex::Range{0, 64}, rex::kRestart, rex::kDebugOnly);

};

inline GPUSettings g_gpu;

inline auto& build_shader_packs = g_gpu.build_shader_packs;
inline auto& build_shader_packs_force = g_gpu.build_shader_packs_force;
inline auto& mcla_compare_effect = g_gpu.mcla_compare_effect;
inline auto& mcla_compare_label_seconds = g_gpu.mcla_compare_label_seconds;
inline auto& mcla_gpu_a2c_dither_all = g_gpu.mcla_gpu_a2c_dither_all;
inline auto& mcla_gpu_aa = g_gpu.mcla_gpu_aa;
inline auto& mcla_gpu_ab_interleave = g_gpu.mcla_gpu_ab_interleave;
inline auto& mcla_gpu_adapter = g_gpu.mcla_gpu_adapter;
inline auto& mcla_gpu_anisotropy = g_gpu.mcla_gpu_anisotropy;
inline auto& mcla_gpu_ao = g_gpu.mcla_gpu_ao;
inline auto& mcla_gpu_ao_car_reach = g_gpu.mcla_gpu_ao_car_reach;
inline auto& mcla_gpu_ao_census = g_gpu.mcla_gpu_ao_census;
inline auto& mcla_gpu_ao_fade_end = g_gpu.mcla_gpu_ao_fade_end;
inline auto& mcla_gpu_ao_fast_math = g_gpu.mcla_gpu_ao_fast_math;
inline auto& mcla_gpu_ao_half_above_1080 = g_gpu.mcla_gpu_ao_half_above_1080;
inline auto& mcla_gpu_ao_history = g_gpu.mcla_gpu_ao_history;
inline auto& mcla_gpu_ao_intensity = g_gpu.mcla_gpu_ao_intensity;
inline auto& mcla_gpu_ao_paint = g_gpu.mcla_gpu_ao_paint;
inline auto& mcla_gpu_ao_stable = g_gpu.mcla_gpu_ao_stable;
inline auto& mcla_gpu_ao_view = g_gpu.mcla_gpu_ao_view;
inline auto& mcla_gpu_batch_draws = g_gpu.mcla_gpu_batch_draws;
inline auto& mcla_gpu_bloom_scale = g_gpu.mcla_gpu_bloom_scale;
inline auto& mcla_gpu_car_chrome_occlusion = g_gpu.mcla_gpu_car_chrome_occlusion;
inline auto& mcla_gpu_car_dump = g_gpu.mcla_gpu_car_dump;
inline auto& mcla_gpu_car_dump_id = g_gpu.mcla_gpu_car_dump_id;
inline auto& mcla_gpu_car_frame_log = g_gpu.mcla_gpu_car_frame_log;
inline auto& mcla_gpu_car_rt_check = g_gpu.mcla_gpu_car_rt_check;
inline auto& mcla_gpu_car_rt_masks = g_gpu.mcla_gpu_car_rt_masks;
inline auto& mcla_gpu_car_rt_view = g_gpu.mcla_gpu_car_rt_view;
inline auto& mcla_gpu_car_selfrefl = g_gpu.mcla_gpu_car_selfrefl;
inline auto& mcla_gpu_car_selfrefl_full = g_gpu.mcla_gpu_car_selfrefl_full;
inline auto& mcla_gpu_car_selfrefl_gain = g_gpu.mcla_gpu_car_selfrefl_gain;
inline auto& mcla_gpu_car_selfrefl_history = g_gpu.mcla_gpu_car_selfrefl_history;
inline auto& mcla_gpu_car_selfrefl_length = g_gpu.mcla_gpu_car_selfrefl_length;
inline auto& mcla_gpu_car_selfrefl_rt = g_gpu.mcla_gpu_car_selfrefl_rt;
inline auto& mcla_gpu_car_selfrefl_rt_denoise = g_gpu.mcla_gpu_car_selfrefl_rt_denoise;
inline auto& mcla_gpu_car_selfrefl_rt_gloss = g_gpu.mcla_gpu_car_selfrefl_rt_gloss;
inline auto& mcla_gpu_car_selfrefl_rt_length = g_gpu.mcla_gpu_car_selfrefl_rt_length;
inline auto& mcla_gpu_car_selfrefl_rt_parts = g_gpu.mcla_gpu_car_selfrefl_rt_parts;
inline auto& mcla_gpu_car_selfrefl_steps = g_gpu.mcla_gpu_car_selfrefl_steps;
inline auto& mcla_gpu_car_selfrefl_strength = g_gpu.mcla_gpu_car_selfrefl_strength;
inline auto& mcla_gpu_car_selfrefl_test = g_gpu.mcla_gpu_car_selfrefl_test;
inline auto& mcla_gpu_car_selfrefl_thickness = g_gpu.mcla_gpu_car_selfrefl_thickness;
inline auto& mcla_gpu_car_selfrefl_view = g_gpu.mcla_gpu_car_selfrefl_view;
inline auto& mcla_gpu_car_shadow_map = g_gpu.mcla_gpu_car_shadow_map;
inline auto& mcla_gpu_car_shadow_map_view = g_gpu.mcla_gpu_car_shadow_map_view;
inline auto& mcla_gpu_census_ps = g_gpu.mcla_gpu_census_ps;
inline auto& mcla_gpu_census_scale = g_gpu.mcla_gpu_census_scale;
inline auto& mcla_gpu_census_targets = g_gpu.mcla_gpu_census_targets;
inline auto& mcla_gpu_class = g_gpu.mcla_gpu_class;
inline auto& mcla_gpu_contact_shadows = g_gpu.mcla_gpu_contact_shadows;
inline auto& mcla_gpu_contact_shadows_car_map_march = g_gpu.mcla_gpu_contact_shadows_car_map_march;
inline auto& mcla_gpu_contact_shadows_fade = g_gpu.mcla_gpu_contact_shadows_fade;
inline auto& mcla_gpu_contact_shadows_fast = g_gpu.mcla_gpu_contact_shadows_fast;
inline auto& mcla_gpu_contact_shadows_length = g_gpu.mcla_gpu_contact_shadows_length;
inline auto& mcla_gpu_contact_shadows_mask_gain = g_gpu.mcla_gpu_contact_shadows_mask_gain;
inline auto& mcla_gpu_contact_shadows_strength = g_gpu.mcla_gpu_contact_shadows_strength;
inline auto& mcla_gpu_contact_shadows_thickness = g_gpu.mcla_gpu_contact_shadows_thickness;
inline auto& mcla_gpu_contact_shadows_vehicles = g_gpu.mcla_gpu_contact_shadows_vehicles;
inline auto& mcla_gpu_contact_shadows_view = g_gpu.mcla_gpu_contact_shadows_view;
inline auto& mcla_gpu_contact_skip_stat = g_gpu.mcla_gpu_contact_skip_stat;
inline auto& mcla_gpu_coverage_queries = g_gpu.mcla_gpu_coverage_queries;
inline auto& mcla_gpu_debug_layer = g_gpu.mcla_gpu_debug_layer;
inline auto& mcla_gpu_dlss_mask_tags = g_gpu.mcla_gpu_dlss_mask_tags;
inline auto& mcla_gpu_dlss_mode = g_gpu.mcla_gpu_dlss_mode;
inline auto& mcla_gpu_edram_aliasing = g_gpu.mcla_gpu_edram_aliasing;
inline auto& mcla_gpu_edram_capture = g_gpu.mcla_gpu_edram_capture;
inline auto& mcla_gpu_env_resolution = g_gpu.mcla_gpu_env_resolution;
inline auto& mcla_gpu_flicker_probe = g_gpu.mcla_gpu_flicker_probe;
inline auto& mcla_gpu_flicker_probe_dumps = g_gpu.mcla_gpu_flicker_probe_dumps;
inline auto& mcla_gpu_flicker_probe_shots = g_gpu.mcla_gpu_flicker_probe_shots;
inline auto& mcla_gpu_frame_generation = g_gpu.mcla_gpu_frame_generation;
inline auto& mcla_gpu_frame_generation_fps_limit = g_gpu.mcla_gpu_frame_generation_fps_limit;
inline auto& mcla_gpu_frame_generation_multiplier = g_gpu.mcla_gpu_frame_generation_multiplier;
inline auto& mcla_gpu_frame_generation_tearing = g_gpu.mcla_gpu_frame_generation_tearing;
inline auto& mcla_gpu_fsr = g_gpu.mcla_gpu_fsr;
inline auto& mcla_gpu_fsr31_mode = g_gpu.mcla_gpu_fsr31_mode;
inline auto& mcla_gpu_fsr4_mode = g_gpu.mcla_gpu_fsr4_mode;
inline auto& mcla_gpu_fsr_debug_view = g_gpu.mcla_gpu_fsr_debug_view;
inline auto& mcla_gpu_fsr_max_passes = g_gpu.mcla_gpu_fsr_max_passes;
inline auto& mcla_gpu_fsr_min_disocclusion_accumulation = g_gpu.mcla_gpu_fsr_min_disocclusion_accumulation;
inline auto& mcla_gpu_fsr_preset = g_gpu.mcla_gpu_fsr_preset;
inline auto& mcla_gpu_fsr_shading_change_scale = g_gpu.mcla_gpu_fsr_shading_change_scale;
inline auto& mcla_gpu_fsr_sharpness = g_gpu.mcla_gpu_fsr_sharpness;
inline auto& mcla_gpu_fsr_velocity_factor = g_gpu.mcla_gpu_fsr_velocity_factor;
inline auto& mcla_gpu_ghost_log_interval = g_gpu.mcla_gpu_ghost_log_interval;
inline auto& mcla_gpu_glow_visibility = g_gpu.mcla_gpu_glow_visibility;
inline auto& mcla_gpu_gpu_based_validation = g_gpu.mcla_gpu_gpu_based_validation;
inline auto& mcla_gpu_hdr = g_gpu.mcla_gpu_hdr;
inline auto& mcla_gpu_hdr_compose_in_present = g_gpu.mcla_gpu_hdr_compose_in_present;
inline auto& mcla_gpu_hdr_highlights = g_gpu.mcla_gpu_hdr_highlights;
inline auto& mcla_gpu_hdr_layers_view = g_gpu.mcla_gpu_hdr_layers_view;
inline auto& mcla_gpu_hdr_paper_white_nits = g_gpu.mcla_gpu_hdr_paper_white_nits;
inline auto& mcla_gpu_hdr_peak_nits = g_gpu.mcla_gpu_hdr_peak_nits;
inline auto& mcla_gpu_hdr_test_pattern = g_gpu.mcla_gpu_hdr_test_pattern;
inline auto& mcla_gpu_lock_ahead_verify = g_gpu.mcla_gpu_lock_ahead_verify;
inline auto& mcla_gpu_lock_dump = g_gpu.mcla_gpu_lock_dump;
inline auto& mcla_gpu_low_latency = g_gpu.mcla_gpu_low_latency;
inline auto& mcla_gpu_luminance_scale_fix = g_gpu.mcla_gpu_luminance_scale_fix;
inline auto& mcla_gpu_memory_census = g_gpu.mcla_gpu_memory_census;
inline auto& mcla_gpu_msaa = g_gpu.mcla_gpu_msaa;
inline auto& mcla_gpu_msaa_with_upscaler = g_gpu.mcla_gpu_msaa_with_upscaler;
inline auto& mcla_gpu_object_names = g_gpu.mcla_gpu_object_names;
inline auto& mcla_gpu_pass_timing = g_gpu.mcla_gpu_pass_timing;
inline auto& mcla_gpu_pause_vehicle_vectors = g_gpu.mcla_gpu_pause_vehicle_vectors;
inline auto& mcla_gpu_perf_markers = g_gpu.mcla_gpu_perf_markers;
inline auto& mcla_gpu_perf_markers_level = g_gpu.mcla_gpu_perf_markers_level;
inline auto& mcla_gpu_pipeline_cache = g_gpu.mcla_gpu_pipeline_cache;
inline auto& mcla_gpu_present_dither = g_gpu.mcla_gpu_present_dither;
inline auto& mcla_gpu_present_release = g_gpu.mcla_gpu_present_release;
inline auto& mcla_gpu_present_release_check = g_gpu.mcla_gpu_present_release_check;
inline auto& mcla_gpu_present_wait = g_gpu.mcla_gpu_present_wait;
inline auto& mcla_gpu_process_memory_log = g_gpu.mcla_gpu_process_memory_log;
inline auto& mcla_gpu_readable_cache = g_gpu.mcla_gpu_readable_cache;
inline auto& mcla_gpu_readable_cache_check = g_gpu.mcla_gpu_readable_cache_check;
inline auto& mcla_gpu_reflection_resolution = g_gpu.mcla_gpu_reflection_resolution;
inline auto& mcla_gpu_reflection_scale = g_gpu.mcla_gpu_reflection_scale;
inline auto& mcla_gpu_resolution_scale = g_gpu.mcla_gpu_resolution_scale;
inline auto& mcla_gpu_resolve_7e3_ceiling = g_gpu.mcla_gpu_resolve_7e3_ceiling;
inline auto& mcla_gpu_resolve_fill_half_pixel = g_gpu.mcla_gpu_resolve_fill_half_pixel;
inline auto& mcla_gpu_resolve_memory_check = g_gpu.mcla_gpu_resolve_memory_check;
inline auto& mcla_gpu_resolve_memory_encode_delay = g_gpu.mcla_gpu_resolve_memory_encode_delay;
inline auto& mcla_gpu_road_reflection_cars_test = g_gpu.mcla_gpu_road_reflection_cars_test;
inline auto& mcla_gpu_scale_depth_bias = g_gpu.mcla_gpu_scale_depth_bias;
inline auto& mcla_gpu_scene_extra_bias = g_gpu.mcla_gpu_scene_extra_bias;
inline auto& mcla_gpu_scene_profile = g_gpu.mcla_gpu_scene_profile;
inline auto& mcla_gpu_shader_memo_check = g_gpu.mcla_gpu_shader_memo_check;
inline auto& mcla_gpu_shadow_filter = g_gpu.mcla_gpu_shadow_filter;
inline auto& mcla_gpu_shadow_filter_pcf = g_gpu.mcla_gpu_shadow_filter_pcf;
inline auto& mcla_gpu_shadow_filter_radius = g_gpu.mcla_gpu_shadow_filter_radius;
inline auto& mcla_gpu_shadow_filter_sun = g_gpu.mcla_gpu_shadow_filter_sun;
inline auto& mcla_gpu_shadow_filter_taps = g_gpu.mcla_gpu_shadow_filter_taps;
inline auto& mcla_gpu_shadow_resolution = g_gpu.mcla_gpu_shadow_resolution;
inline auto& mcla_gpu_skip_shaders = g_gpu.mcla_gpu_skip_shaders;
inline auto& mcla_gpu_split = g_gpu.mcla_gpu_split;
inline auto& mcla_gpu_split_effects = g_gpu.mcla_gpu_split_effects;
inline auto& mcla_gpu_ssgi = g_gpu.mcla_gpu_ssgi;
inline auto& mcla_gpu_ssgi_clamp = g_gpu.mcla_gpu_ssgi_clamp;
inline auto& mcla_gpu_ssgi_far_steps = g_gpu.mcla_gpu_ssgi_far_steps;
inline auto& mcla_gpu_ssgi_half = g_gpu.mcla_gpu_ssgi_half;
inline auto& mcla_gpu_ssgi_intensity = g_gpu.mcla_gpu_ssgi_intensity;
inline auto& mcla_gpu_ssgi_low = g_gpu.mcla_gpu_ssgi_low;
inline auto& mcla_gpu_ssgi_radius = g_gpu.mcla_gpu_ssgi_radius;
inline auto& mcla_gpu_ssgi_reach = g_gpu.mcla_gpu_ssgi_reach;
inline auto& mcla_gpu_ssgi_strength = g_gpu.mcla_gpu_ssgi_strength;
inline auto& mcla_gpu_ssgi_test = g_gpu.mcla_gpu_ssgi_test;
inline auto& mcla_gpu_ssgi_test_atrous = g_gpu.mcla_gpu_ssgi_test_atrous;
inline auto& mcla_gpu_ssgi_test_clamp = g_gpu.mcla_gpu_ssgi_test_clamp;
inline auto& mcla_gpu_ssgi_test_facing = g_gpu.mcla_gpu_ssgi_test_facing;
inline auto& mcla_gpu_ssgi_test_history = g_gpu.mcla_gpu_ssgi_test_history;
inline auto& mcla_gpu_ssgi_test_history_clamp = g_gpu.mcla_gpu_ssgi_test_history_clamp;
inline auto& mcla_gpu_ssgi_test_thickness = g_gpu.mcla_gpu_ssgi_test_thickness;
inline auto& mcla_gpu_ssgi_test_view = g_gpu.mcla_gpu_ssgi_test_view;
inline auto& mcla_gpu_ssgi_test_view_gain = g_gpu.mcla_gpu_ssgi_test_view_gain;
inline auto& mcla_gpu_ssgi_view = g_gpu.mcla_gpu_ssgi_view;
inline auto& mcla_gpu_sun_shafts = g_gpu.mcla_gpu_sun_shafts;
inline auto& mcla_gpu_sun_shafts_cap = g_gpu.mcla_gpu_sun_shafts_cap;
inline auto& mcla_gpu_sun_shafts_census = g_gpu.mcla_gpu_sun_shafts_census;
inline auto& mcla_gpu_sun_shafts_density = g_gpu.mcla_gpu_sun_shafts_density;
inline auto& mcla_gpu_sun_shafts_early = g_gpu.mcla_gpu_sun_shafts_early;
inline auto& mcla_gpu_sun_shafts_intensity = g_gpu.mcla_gpu_sun_shafts_intensity;
inline auto& mcla_gpu_sun_shafts_sky_depth = g_gpu.mcla_gpu_sun_shafts_sky_depth;
inline auto& mcla_gpu_sun_shafts_steps = g_gpu.mcla_gpu_sun_shafts_steps;
inline auto& mcla_gpu_sun_shafts_threshold = g_gpu.mcla_gpu_sun_shafts_threshold;
inline auto& mcla_gpu_sun_shafts_view = g_gpu.mcla_gpu_sun_shafts_view;
inline auto& mcla_gpu_sync_builds = g_gpu.mcla_gpu_sync_builds;
inline auto& mcla_gpu_temporal_jitter_hold = g_gpu.mcla_gpu_temporal_jitter_hold;
inline auto& mcla_gpu_texture_decode_selftest = g_gpu.mcla_gpu_texture_decode_selftest;
inline auto& mcla_gpu_texture_decode_threads = g_gpu.mcla_gpu_texture_decode_threads;
inline auto& mcla_gpu_texture_decode_verify = g_gpu.mcla_gpu_texture_decode_verify;
inline auto& mcla_gpu_texture_dump = g_gpu.mcla_gpu_texture_dump;
inline auto& mcla_gpu_trace_count = g_gpu.mcla_gpu_trace_count;
inline auto& mcla_gpu_trace_frame = g_gpu.mcla_gpu_trace_frame;
inline auto& mcla_gpu_traffic_shadows = g_gpu.mcla_gpu_traffic_shadows;
inline auto& mcla_gpu_traffic_shadows_cars = g_gpu.mcla_gpu_traffic_shadows_cars;
inline auto& mcla_gpu_traffic_shadows_cascades = g_gpu.mcla_gpu_traffic_shadows_cascades;
inline auto& mcla_gpu_traffic_shadows_radius = g_gpu.mcla_gpu_traffic_shadows_radius;
inline auto& mcla_gpu_upscaler = g_gpu.mcla_gpu_upscaler;
inline auto& mcla_gpu_upscaler_lazy_load = g_gpu.mcla_gpu_upscaler_lazy_load;
inline auto& mcla_gpu_upscaler_mask_test = g_gpu.mcla_gpu_upscaler_mask_test;
inline auto& mcla_gpu_upscaler_masks = g_gpu.mcla_gpu_upscaler_masks;
inline auto& mcla_gpu_upscaler_post_reads_output = g_gpu.mcla_gpu_upscaler_post_reads_output;
inline auto& mcla_gpu_upscaler_sharpness = g_gpu.mcla_gpu_upscaler_sharpness;
inline auto& mcla_gpu_upscaler_zero_jitter = g_gpu.mcla_gpu_upscaler_zero_jitter;
inline auto& mcla_gpu_validate = g_gpu.mcla_gpu_validate;
inline auto& mcla_gpu_verify_constants = g_gpu.mcla_gpu_verify_constants;
inline auto& mcla_gpu_vram_log = g_gpu.mcla_gpu_vram_log;
inline auto& mcla_gpu_vsync = g_gpu.mcla_gpu_vsync;
inline auto& mcla_gpu_watch_base = g_gpu.mcla_gpu_watch_base;
inline auto& mcla_gpu_watch_capture = g_gpu.mcla_gpu_watch_capture;
inline auto& mcla_gpu_watch_size = g_gpu.mcla_gpu_watch_size;
inline auto& mcla_gpu_work_17oct = g_gpu.mcla_gpu_work_17oct;
inline auto& precache_pipelines = g_gpu.precache_pipelines;
inline auto& precache_pipelines_backend = g_gpu.precache_pipelines_backend;
inline auto& precache_pipelines_force = g_gpu.precache_pipelines_force;
inline auto& precache_pipelines_threads = g_gpu.precache_pipelines_threads;
inline auto& shader_pack_dir = g_gpu.shader_pack_dir;
inline auto& shader_pack_threads = g_gpu.shader_pack_threads;

}  // namespace larecomp::settings
