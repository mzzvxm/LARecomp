#include "modloader.h"

#include <rex/cvar.h>
#include <rex/runtime.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "../../larecomp_log.h"
#include "gltf.h"
#include "objmesh.h"
#include "rim_names.h"
#include "rpf3.h"
#include "rsc5.h"
#include "texture.h"
#include "xcompress.h"
#include "mc_engine/boot_progress.h"
#include "mc_engine/music/custom_music.h"

REXCVAR_DEFINE_BOOL(model_mods, true, "MCLA/Mods",
    "Load model replacements from <exe>/models/<mod>/<asset>.obj. Each .obj is "
    "baked into a native drawable and packed into xarchive_mods.rpf, which is "
    "appended to the archive list -- the engine's own last-mounted-wins rule "
    "makes it override per file. Game archives are never modified.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(model_mods_bone, -1, "MCLA/Mods",
    "-1 keeps a skinned mesh's own weights (glTF with JOINTS_0/WEIGHTS_0), "
    "matching its joints to the driver's bones by bind-pose position, so the "
    "model follows the seated pose. Any value >= 0 ignores the weights and "
    "rides the whole mesh rigidly on that one bone, which is all a .obj can do.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_offset_y, 0.0, "MCLA/Mods",
    "Extra height for the replacement model, in metres, on top of the automatic "
    "lift onto its bone. Positive raises it. Use this if the model still sits "
    "below or above the seat -- it takes a restart but no rebuild.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_offset_x, 0.0, "MCLA/Mods",
    "Sideways nudge for the replacement model, in metres.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_offset_z, 0.0, "MCLA/Mods",
    "Forward/back nudge for the replacement model, in metres.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_yaw, 0.0, "MCLA/Mods",
    "Turns the replacement model about the vertical axis, in degrees. Source "
    "models rarely face the same way the game does -- 180 turns a model that "
    "sits with its back to the wheel.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_pitch, 0.0, "MCLA/Mods",
    "Tips the replacement model forward or back, in degrees.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_roll, 0.0, "MCLA/Mods",
    "Rolls the replacement model sideways, in degrees.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_scale, 1.0, "MCLA/Mods",
    "Size multiplier on top of the automatic fit, which matches the model's "
    "height to the driver's. 1.0 leaves it alone.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_submeshes, true, "MCLA/Mods",
    "Spread the replacement mesh across every submesh of the character instead "
    "of the largest one alone. The resource cannot be made bigger, but the "
    "driver is fifteen submeshes holding 8208 vertices and 37512 indices "
    "between them, against 2320 and 12957 in the biggest -- so this raises what "
    "a mod can bring by three and a half times, and most characters then arrive "
    "with no welding at all. Every submesh borrowed this way is repointed at "
    "the shader whose textures were replaced. Turn it off to go back to filling "
    "one submesh, which is the conservative path if a model renders oddly.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_decimate, true, "MCLA/Mods",
    "Whether a replacement mesh larger than the slot it has to live in may be "
    "welded down to fit. The slot is fixed -- the drawable's largest submesh, "
    "around 2300 vertices, and a few hundred on the low LOD -- and the welding "
    "is a blunt instrument: vertices that share a cell of a spatial grid are "
    "merged and averaged, which on a detailed model rounds off faces and hands. "
    "Turn this off if you would rather decimate the model yourself in a "
    "modelling tool, where you can see what you are giving up; anything still "
    "too big is then refused and that variant keeps the shipped character. "
    "Expect the low LOD to be refused first, so the original driver reappears "
    "at a distance.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_proportions, 0.0, "MCLA/Mods",
    "How much of the replacement model's own build to keep when it is reposed "
    "onto the driver's skeleton, 0 to 1. At 0 every joint lands exactly on the "
    "bone it was matched to, which is what the game's skinning expects and what "
    "a human-shaped model wants. A model built to other proportions gets "
    "stretched to reach those bones -- a cartoon character with a six centimetre "
    "neck has it pulled out to the driver's twenty-six -- and raising this "
    "keeps its own bone lengths instead, taking only the pose from the "
    "skeleton. The cost is that limbs no longer end where the game thinks they "
    "do, so hands drift off the wheel as it goes up. Try 1 for anything that is "
    "not roughly human.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_textures, true, "MCLA/Mods",
    "Use the replacement model's own textures. The images embedded in a .glb "
    "(one per material) are packed into a single atlas, each primitive's UVs "
    "are remapped into its own cell, and the result is written over the "
    "character's diffuse map in place -- same address, same size, same format. "
    "Off leaves the mesh wearing the original driver's skin.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_car_textures, true, "MCLA/Mods",
    "Give a replacement car body its own textures. A car is drawn by several "
    "shaders of one drawable and each of them samples at most one image, so the "
    "mod's materials are grouped by the shader they were mapped to, packed into "
    "one atlas per shader with their UVs remapped, and written over that "
    "shader's colour map in the car's material packs (.xtl and .xtp) in place.\n"
    "\n"
    "Only shaders that sample anything get one. CarPaintCustomizable, CarGlass, "
    "BlackMatte and Chrome read no texture at all -- the effect decides that, "
    "not the data -- so their materials keep the UVs they came with, which is "
    "what paint needs for its vinyls to sit right.\n"
    "\n"
    "Off ships the donor's packs exactly as the game holds them, and the body "
    "comes back wearing the donor's own artwork.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_rim_autoalign, true, "MCLA/Mods",
    "Put a replacement wheel on the game's axle, in two steps. First the turn: "
    "every shipped wheel spins about X and is half as wide as it is tall, so the "
    "thinnest axis of the model is taken to be its axle and rotated onto that "
    "one -- a wheel exported lying flat comes in standing up without anyone "
    "having to work out which ninety degrees it needed. Then the depth: a wheel "
    "is not centred in its own bounding box, it keeps its hub and spokes in the "
    "negative half of the axle with only a thin lip at the far edge, so a bare "
    "rim left centred sits sunk inside the tyre. Its face is pushed out flush "
    "with the shipped wheel's instead, turning the model around first if it came "
    "in facing inward. The manual rim angles and nudge below are applied on top. "
    "Turn this off to place a wheel entirely by hand.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_rim_keep_badge, true, "MCLA/Mods",
    "Leave the wheel's badge quad as it shipped instead of silencing it.\n"
    "\n"
    "It is nine vertices on its own material, and that material is the only "
    "one on the wheel that samples a texture -- so silencing it, which is what "
    "the geometry rewrite did to every unused submesh, takes the badge off the "
    "car and leaves a mod's paint read by nothing.\n"
    "\n"
    "Off restores the old behaviour. Only mods that bring an image of their own "
    "reach this at all, so a wheel mod with no texture behaves the same either "
    "way -- which makes this the switch to try first when a textured wheel mod "
    "misbehaves and an untextured one on the same build does not.");

REXCVAR_DEFINE_BOOL(model_mods_rim_shade_profile, true, "MCLA/Mods",
    "Rebuild a replaced wheel's baked shading from the template instead of "
    "flooding it with one average.\n"
    "\n"
    "A shipped wheel carries its ambient occlusion per vertex, and it is not a "
    "small effect: measured across five of them the lane runs the full 0..255 "
    "with a deviation above fifty. Carrying it over vertex by vertex prints the "
    "old rim's spoke shadows onto the new one, so it was replaced by a single "
    "average -- which removes the shadows and every cavity with them. What is "
    "left is a rim shaded only by its material's specular, and because that is "
    "pinned to the geometry it sweeps around as the wheel turns and blows out "
    "as soon as the rim is painted.\n"
    "\n"
    "On, the lane is read from the template through coordinates that cannot "
    "carry a spoke: distance from the axle, depth along it, and how the surface "
    "faces in each. The shipped lane predicts itself that way to R^2 0.82, and "
    "read with a mod's own coordinates it gives structure of the right kind at "
    "about 60% of the shipped amplitude.\n"
    "\n"
    "It is not a reconstruction. Fitted on one wheel and scored against "
    "another's bake it reaches R^2 -0.09, no better than the flat average, at "
    "every grid size tried -- half of a wheel's bake is its own spokes, and "
    "there is no bake for geometry the game never saw. Off restores the flat "
    "average, which is featureless but is not a guess.");

REXCVAR_DEFINE_INT32(model_mods_rim_texture_mode, -1, "MCLA/Mods",
    "Where a wheel mod's own image is applied: -1 auto, 0 the hub cap, 1 the "
    "whole rim.\n"
    "\n"
    "A shipped wheel keeps no skin. Its metal is the shader and the baked "
    "per-vertex shade, and its texture is a decal sheet -- the maker's logo "
    "plus a few lug nuts and valve stems, most of it transparent -- sampled by "
    "a nine-vertex quad sitting on the hub. So there are two sensible things a "
    "mod's image can be, and they want opposite treatment.\n"
    "\n"
    "Auto decides on the image's own alpha, which is what tells the two apart: "
    "a badge is a logo on transparency, a skin has nothing to be transparent "
    "for. An image with real transparency goes to the cap alone, on the quad "
    "the game already draws there. An opaque one is taken for a rim skin, and "
    "the body is pointed at the textured material so it lands across the whole "
    "wheel.\n"
    "\n"
    "Either way the badge quad is left as it shipped, so the cap is never "
    "blank. Force the choice with 0 or 1 when a mod's alpha does not say what "
    "it meant.");

REXCVAR_DEFINE_BOOL(model_mods_rim_shade_occlusion, true, "MCLA/Mods",
    "Bake the wheel's shade lane from occlusion measured on the replacement."
    "\n"
    "A shipped wheel carries a baked lane: measured on three of them it runs "
    "0..252 with a deviation near fifty (bbs_ch 0..252 mean 155 dev 52, "
    "amer_razor 5..254 mean 186 dev 45, 5zigen_5zr 0..249 mean 144 dev 55). "
    "A modded wheel had none: flooding one average gives a deviation of ZERO, "
    "and the only shading left is the material's specular, which is pinned to "
    "the geometry and sweeps round as the wheel turns. That is what \"the "
    "normals change with rotation\" is, and it is not the normals."
    "\n"
    "The other answer -- model_mods_rim_inherit_shade -- carries the "
    "template's lane across by proximity, and lights the wheel NEON: the lane's "
    "colour is 0x000000 on every shipped wheel, so what moves is the UV1 band "
    "beside it, and a band picks a material (RimMaterialA..E). Scattered across "
    "a shape that does not share them, some vertices land on the emissive one."
    "\n"
    "So the lane is measured instead of copied or invented. Validated before "
    "being switched on: this car's rim comes out 0..255 mean 156 deviation 77 "
    "against a target of mean 155 deviation 52 -- the mean lands on its own, "
    "and only the contrast is pulled in. Off falls back to the flat fill."
    "\n"
    "Takes precedence over model_mods_rim_shade_profile, which guesses the same "
    "lane from statistics and which its own comment scores at R^2 -0.09 across "
    "wheels -- no better than the flat average it replaces.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_rim_inherit_shade, false, "MCLA/Mods",
    "Diagnostic. Copy the shipped wheel's per-vertex shading onto the "
    "replacement, vertex by vertex from whichever original vertex is nearest. "
    "This is what a character replacement does, and it works there because the "
    "mesh has been reposed onto the very skeleton it replaces. A wheel shares "
    "nothing with the wheel it replaces, so what arrives is the old wheel's "
    "baked shadows printed onto the new spokes, and its four material bands "
    "scattered by proximity across a shape that does not have them. Off, which "
    "is the default, gives the whole wheel one band and that band's own average "
    "shade, both read off the template.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_rim_offset_x, 0.0, "MCLA/Mods",
    "Nudge for a replacement wheel along its axle, where one unit is the "
    "wheel's own diameter. Positive pushes it further into the tyre, negative "
    "pulls it out. Only needed when the automatic flush above lands slightly "
    "off for a particular model.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_rim_yaw, 0.0, "MCLA/Mods",
    "Turns a replacement wheel about the vertical axis, in degrees. Separate "
    "from the character angles because a wheel and a driver never want the same "
    "correction.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_rim_pitch, 0.0, "MCLA/Mods",
    "Tips a replacement wheel forward or back, in degrees.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_rim_roll, 0.0, "MCLA/Mods",
    "Rolls a replacement wheel sideways, in degrees.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_rim_scale, 1.0, "MCLA/Mods",
    "Size multiplier for a replacement wheel, on top of the automatic fit to "
    "the shipped wheel's own bounds. 1.0 leaves it alone.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_car_yaw, 180.0, "MCLA/Mods",
    "Turns a replacement car about the vertical axis before its parts are "
    "placed, in degrees. 180 is the normal value and the default: glTF has a "
    "model face +Z and MCLA has it face -Z, which is checked rather than "
    "assumed -- the shipped cars keep their headlights at negative Z and their "
    "tail lights at positive Z, and an exported car has it the other way round.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(model_mods_car_scale, 1.0, "MCLA/Mods",
    "Size multiplier for a replacement car, on top of the automatic fit. The "
    "fit matches the length of the mod's body to the length of the body it "
    "replaces, and every part of the car is then moved by that one factor -- "
    "scaling each part into its own slot instead would leave a hood a size "
    "larger than the doors beside it. 1.0 leaves it alone.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_grow, false, "MCLA/Mods",
    "Give a replacement wheel or car part buffers of its own instead of welding "
    "it down into the ones the shipped resource came with. The resource is made "
    "longer and the submesh is pointed at the new space; nothing already in it "
    "moves. This is what removes the detail ceiling -- a wheel slot holds 3,701 "
    "vertices and a modern wheel model is fifty times that -- and it was thought "
    "impossible until the streamer's read budget turned out to be what had been "
    "corrupting grown resources. 65535 vertices per submesh remains, because a "
    "submesh counts and indexes its vertices in sixteen bits. Characters are not "
    "affected: a skinned submesh also caps its bone palette, which is a separate "
    "allocation, so they stay on the path that already works. Turn this off to "
    "go back to welding everything into the shipped buffers.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(model_mods_car_budget, 1048576, "MCLA/Mods",
    "The largest a replaced car body may be grown to, in bytes per LOD. "
    "Anything the mod brings beyond what fits is welded away.\n"
    "\n"
    "Growing a drawable has no natural limit -- the mod hands over whatever it "
    "modelled and the resource follows -- but the game has one in practice. "
    "Counted over the 8849 vehicle drawables the game ships, the largest "
    "declares 1,163,264 bytes and the median 45,056; counted over body_lod_0 "
    "alone the largest is 884,736 and the median 266,240. The default sits just "
    "under that largest, so a mod may be as heavy as the heaviest thing the "
    "engine already streams and no heavier.\n"
    "\n"
    "Measured, and not a verdict: a 4,194,304 budget gave the BMW a 3,825,664 "
    "byte body_lod_0 that drives correctly, so exceeding the shipped maximum is "
    "not by itself what breaks a car.\n"
    "\n"
    "0 lifts the ceiling entirely, which is the setting to try when you want to "
    "know whether size is what a car is failing on.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_car_pack_write, true, "MCLA/Mods",
    "Write the mod's atlases into the car's material packs."
    "\n"
    "A probe, not a feature. model_mods_car_textures gates TWO things at once: "
    "the atlas, which rewrites the mesh's UVs, and the pack rewrite, which "
    "unpacks .xtl/.xtp, drops the atlases in and repacks them through "
    "BuildRsc5File instead of shipping the donor's bytes as they came. "
    "Switching that cvar off made a broken car whole again, which says the "
    "fault is in one of those two and not which."
    "\n"
    "Off keeps the atlas and its UV rewrite and ships the packs untouched. The "
    "car then wears the donor's artwork through the mod's UVs, which looks "
    "wrong on purpose -- what is being read is the GEOMETRY. Whole means the "
    "pack rewrite is at fault; in pieces means the UV rewrite is.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_share_images, false, "MCLA/Mods",
    "Let two materials that name the same picture share one cell of the atlas."
    "\n"
    "A textures/ folder is keyed by material name, so a car whose three lamp "
    "materials all draw 750i_taillight ships three identical files and they "
    "take three cells. Collapsing them by content is free resolution -- the "
    "lamp shader went from six images in a 1024 atlas to two in a 512, which is "
    "128 pixels a material against 256."
    "\n"
    "OFF, because it is not free. Turning it on is the ONE change between the "
    "last build of this car that drew correctly and the one that came back as "
    "flat slabs, isolated by switching model_mods_car_textures off: with the "
    "atlas gone the geometry was whole again and the car merely wore the "
    "donor's artwork. Sharing a cell makes that cell cover the UNION of what "
    "every part sharing it asks for, which is the only thing here that changes "
    "a UV by more than a texel -- and something downstream of that is not "
    "surviving it. Until that is understood this stays off and each material "
    "keeps its own cell.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(model_mods_part_growth, 8, "MCLA/Mods",
    "How much larger than the donor's own a replacement PART may be, as a "
    "multiple of the shipped resource.\n"
    "\n"
    "A part is not a body and does not need the body's machinery: the Impala's "
    "front bumper is 1731 vertices where its shell is three thousand-odd, and "
    "the whole of a car's sixty parts together weigh less than one of its LODs. "
    "Eight times the shipped size is room for a part modelled far more finely "
    "than the one it stands in for, and still small enough that nothing has to "
    "be dealt out between shaders the way the body's room is.\n"
    "\n"
    "1 keeps every part at exactly the size the donor shipped, which is the "
    "setting to try when a car loads without its parts.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_car_verbatim, false, "MCLA/Mods",
    "Diagnostic, cars only. Copies each part's shipped file into the mod "
    "archive exactly as it lies in the game's own -- same bytes, same "
    "compressed length, same flag -- instead of decompressing it and packing it "
    "back. Everything else about the mod archive stays as it is, so this "
    "separates the two halves of a failure: if the game is happy with the "
    "verbatim copy, what it dislikes is the repack; if it still refuses, the "
    "repack is innocent and the problem is in serving a vehicle resource from a "
    "second archive at all. `model_mods_passthrough` cannot answer that on its "
    "own, because it still re-encodes the stream -- and an LZX of stored blocks "
    "is larger than the compressed original it replaces.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_passthrough, false, "MCLA/Mods",
    "Diagnostic. Repacks the original character resource without touching its "
    "geometry, so the .obj is ignored. If the driver still renders correctly "
    "with this on, the packing path (LZX, archive, mount, flags) is sound and "
    "any breakage belongs to the mesh rewrite; if it breaks, the packing path "
    "is at fault.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_anchor_bones, true, "MCLA/Mods",
    "Whether the repose plants each of the mod's joints on the bone it was "
    "matched to. On (the default) a hand lands exactly on the bone the game "
    "drives a hand with, and the body pays for it: a model's chest joint sits "
    "lower than the driver's chest bone, so the chest is hauled up and back "
    "while the collarbone barely moves, and the flesh between them loses 21 mm "
    "of shape -- the crease across the chest, the angular shoulder, and the "
    "belly pulled in behind a spine bone that sits at the back of a torso where "
    "the model's sits down the middle. Off, joints keep the offsets they were "
    "authored with and only turn: measured on the driver mod that halves the "
    "distortion, 5.7 mm to 2.8 mm averaged over every bone, at the cost of the "
    "wrist landing 38 mm and the foot 96 mm off their bones, which swings them "
    "on the wrong lever once animation starts. Standing still, off looks "
    "better; in motion is what only the game can answer.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(model_mods_weight_smoothing, 4, "MCLA/Mods",
    "How many rounds of neighbour averaging the mod's skin weights get before "
    "the model is reposed onto the driver's skeleton. 0 keeps them exactly as "
    "authored. The models people bring are rigidly weighted -- a GTA-era "
    "character puts every vertex at full strength on one bone and zero on the "
    "rest -- and the repose hands each bone its own rigid transform, built from "
    "a skeleton with different bone lengths. Two neighbouring vertices bound "
    "hard to different bones then get two unrelated transforms and the edge "
    "between them is torn: measured on the driver mod, edges came out at 7.6x "
    "their own length, which is the flat blade that was hanging off that "
    "character's back. Smoothing gives a seam vertex a share of both bones so "
    "the seam bends instead. Four rounds took the worst edge to 2.1x; away from "
    "a seam it changes nothing.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(model_mods_grow_slack, 65536, "MCLA/Mods",
    "Room left unused at the end of a resource `model_mods_grow` has enlarged. "
    "The tail of a grown segment does not arrive intact, and this is what keeps "
    "the mesh out of it -- measured on a wheel: buffers ending 16 KB short of "
    "the segment's end came back with holes, and ending 82 KB short came back "
    "clean. Where between those two the boundary sits has not been pinned down, "
    "so the default is the distance that was measured to work rather than the "
    "smallest that might. 0 restores the old behaviour and the holes with it.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_car_space_only, true, "MCLA/Mods",
    "Write a donor car's replacement mesh only into the models the donor authors "
    "in the CAR's frame, and silence the pass's submeshes in the others.\n"
    "\n"
    "A vehicle body is several models and the game draws each with its own part "
    "matrix -- a bumper, a lamp, a door window. Geometry that arrives in car "
    "space and lands in one of those goes wherever that bone is, which is the "
    "nose in the ground and the lamps above the roof.\n"
    "\n"
    "Silencing here zeroes the index and vertex counts and leaves the buffer "
    "pointers, which is the technique that twice failed in game when it emptied a "
    "whole part; applied to some submeshes of a shader rather than all, it has "
    "always been fine. It was suspected once of causing blades and cleared: those "
    "were buffers crossing a resource block boundary, and the fix for that is "
    "elsewhere (MeshOffset::block_align). Kept switchable because it is the one "
    "thing between a car in its right place and a car whose nose is in the "
    "ground. Read at archive build time, so it needs a restart and no rebuild.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(model_mods_grow_probe, 0, "MCLA/Mods",
    "With `model_mods_grow` on: 1 grows the resource and stops there, the segment "
    "gets bigger and nothing else changes -- no buffer moves, no pointer is "
    "rewritten, no count is touched, and the mesh is decimated into the buffers "
    "the template already owned. Growing and repointing have only ever been "
    "tried together, so the first attempt could be written off without saying "
    "which of the two was at fault. If this renders, a resource can be made "
    "larger safely and the fault is in the repointing; if it does not, growth "
    "alone is the fault and the rest never mattered. 2 goes one rung further: "
    "the buffers are put in the new space and repointed, but the counts are "
    "left alone, so the mesh is still decimated to what the template shipped. "
    "That is what says whether the bytes written into the grown space actually "
    "arrive and can be fetched from -- 1 never reads them. Only when both pass "
    "is a changed count worth trying.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(model_mods_diag, false, "MCLA/Mods",
    "Write a `models/.diag` folder beside the mods: one .txt per rebuilt asset "
    "holding the template's submesh table, both rigs with the joint-to-bone "
    "mapping, and how the mesh was dealt across the submeshes -- plus the mesh "
    "itself as .obj before and after the retarget. A character that comes out "
    "mangled is one of three things (a joint on the wrong bone, a bad deal, or "
    "a submesh the writer could not describe and therefore left drawing what it "
    "shipped) and they all look alike on screen; this tells them apart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mc::modloader {

namespace {

constexpr const char* kModArchiveName = "xarchive_mods.rpf";
constexpr const char* kSourceArchiveName = "xarchive_cache.rpf";

// Side of one atlas cell. Four materials land in a 2x2 grid of these, which is
// exactly the 512x512 the character's diffuse map measures, so the common case
// costs no resampling at all.
constexpr uint32_t kAtlasCell = 256;

// Bytes left between the last buffer of a rebuilt drawable and the end of its
// segment. The end of a resource does not arrive intact; see PadResourceTail
// for the two measurements that bracket it.
constexpr uint32_t kResourceTailSlack = 65536;

// Page class a rebuilt vehicle drawable is restated in: 256 << 8 = 65536, which
// makes the blocks it is split into 4096 << 8 = 1 MB. See
// MeshOffset::block_align for why a block has to be able to hold a whole buffer.
constexpr uint32_t kCarPageShift = 8;

bool g_mod_archive_ready = false;

std::filesystem::path ExeDir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) return {};
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    return std::filesystem::current_path(ec);
#endif
}

// Cached decompressed template: our own header, then [virtual][physical].
constexpr uint32_t kTemplateMagic = 0x4D545031u;  // 'MTP1'

bool LoadTemplateCache(const std::filesystem::path& path, Rsc5Resource& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    uint32_t header[5] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || header[0] != kTemplateMagic) return false;

    out.virtual_size = header[1];
    out.physical_size = header[2];
    out.type = header[3];
    out.flag = header[4];

    const size_t total = static_cast<size_t>(out.virtual_size) + out.physical_size;
    out.data.assign(total, 0);
    in.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(total));
    return static_cast<size_t>(in.gcount()) == total;
}

void SaveTemplateCache(const std::filesystem::path& path, const Rsc5Resource& resource) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary);
    if (!out) return;

    const uint32_t header[5] = {kTemplateMagic, resource.virtual_size, resource.physical_size,
                                resource.type, resource.flag};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(resource.data.data()),
              static_cast<std::streamsize>(resource.data.size()));
}

// Pulls a resource out of the shipped archive and decompresses it.
bool ExtractTemplate(const Rpf3Reader& archive, const std::string& archive_path,
                     Rsc5Resource& out, std::string& error) {
    Rpf3Entry entry;
    if (!archive.Find(archive_path, entry)) {
        error = "not found in " + std::string(kSourceArchiveName);
        return false;
    }

    std::vector<uint8_t> raw;
    if (!archive.ReadFile(entry, raw)) {
        error = "cannot read archive data";
        return false;
    }

    size_t payload_offset = 0, payload_size = 0;
    if (!ParseRsc5Header(raw, out, payload_offset, payload_size, error)) return false;

    const size_t total = static_cast<size_t>(out.virtual_size) + out.physical_size;
    if (!LzxDecompress(raw.data() + payload_offset, payload_size, out.data, total)) {
        error = "LZX decompression failed";
        return false;
    }
    return true;
}

uint8_t* GuestPointer(uint32_t address) {
    auto* runtime = rex::Runtime::instance();
    if (!runtime || !address) return nullptr;

    auto* base = runtime->virtual_membase();
    return base ? base + address : nullptr;
}

struct ModEntry {
    std::string asset;              // e.g. drv_mp_01_set
    std::filesystem::path obj;      // source mesh
    std::string mod_name;           // owning folder, for logs

    // A wheel the game does not ship, built from one it does. Empty for the
    // ordinary case, where the asset replaces itself. A new car's stock wheel
    // is looked up by a name derived from the car's own (sub_8238F9D0 strips
    // "vp_" and asks for "whl_stk_<rest>"), so a car nobody has heard of needs
    // a wheel nobody has heard of, and there is no template under that name.
    std::string donor;
};

// A car replacement is a folder, not a file: one mesh plus the mapping that
// says which of its parts fills which slot of the vehicle.
struct VehicleMod {
    std::string car;                // e.g. vp_nsn_skyline_99
    std::filesystem::path folder;
    std::string mod_name;
};

// A file the mod ships as-is, and the archive path it has to land on.
struct RawFile {
    std::string archive_path;       // e.g. tune/vehicle/vehicle0003.lst
    std::filesystem::path source;
    std::string mod_name;
};

// Name a mesh this and it stands in for every character -- or, in the rims
// folder, for every wheel -- in the game.
constexpr const char* kEveryAsset = "all";

// Wheels live in their own folder inside a mod, because a wheel and a character
// are different assets under different paths and a name alone cannot say which
// one a mesh is meant for:
//
//   models/<mod>/<character>.glb       ->  resources/character/<name>/<name>.xrsc
//   models/<mod>/rims/<wheel>.glb      ->  resources/rims/<name>/body_lod_0.xrsc
//                                      +   resources/rims/<name>/<name>.xtp
//
// A wheel is two resources, not one: the drawable holds no textures, so the
// mesh goes into body_lod_0.xrsc and any image the mod brought goes into the
// texture pack beside it. Replacing only the first is what left modded rims
// wearing the shipped wheel's paint.
constexpr const char* kRimFolder = "rims";

// Cars live one folder deeper still, named after the vehicle they replace,
// because a car is not one asset: a player car ships as around a hundred and
// seventy resources, one per part, and a mod has to say which of its own parts
// goes into which of them.
//
//   models/<mod>/vehicles/vp_nsn_skyline_99/<anything>.glb
//   models/<mod>/vehicles/vp_nsn_skyline_99/parts.txt
//
// parts.txt is one line per slot, `slot = group[, group...]`, naming the glTF
// nodes that fill it:
//
//   widebody0 = bbme38_dno
//   hood0     = bbme38_hood_rest
//   door_l0   = bbme38_door_FL
//
// A slot with no line keeps the shipped part, which is what makes a partial
// swap possible -- MCLA's Skyline has five spoiler slots and a BMW has no
// spoiler at all.
constexpr const char* kVehicleFolder = "vehicles";


// Files that are not meshes at all, copied into the mod archive byte for byte
// under the path they already have:
//
//   models/<mod>/files/tune/vehicle/vehicle0003.lst
//     -> tune/vehicle/vehicle0003.lst
//
// The archive is mounted last, so anything here wins over the shipped copy, and
// a path the game has never seen is simply a new file. This is what lets a mod
// add a car to the showroom -- the roster is read from tune/vehicle/vehicle.lst
// plus vehicle0001.lst..vehicle0008.lst, and only 0001 and 0002 are taken.
//
// They go in uncompressed. The shipped archive holds 1718 entries like that and
// the rule they follow is that the flag word IS the size, with the compressed
// and resource bits clear, so nothing has to be encoded to add one.
constexpr const char* kFilesFolder = "files";

// Enough of the RSC5 header to tell a resource from a plain file and read the
// two words that describe it. rsc5.cpp keeps its own copies of these; they are
// four lines and duplicating them is cheaper than widening that header for one
// caller.
constexpr uint32_t kRsc5Magic = 0x05435352u;
constexpr size_t kRsc5HeaderSize = 16;

// 'LARC' -- a mod shipping an archive entry exactly as some other archive holds
// it: the bytes, its flag and its resource type, with nothing interpreted.
//
// This exists because most of what a mod wants to clone cannot be rebuilt. A
// per-car CarCfg, for one: all 401 of them are LZX-compressed with the framing
// used for plain files, which no host-side decoder here can read, so the only
// way to give a new car a config is to hand it another car's bytes untouched.
constexpr uint32_t kLarcMagic = 0x4C415243u;  // 'LARC'

uint32_t LoadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
constexpr const char* kVehiclePartsFile = "parts.txt";

// Textures a mod ships beside its model rather than inside it, one file per
// material name -- see LoadSidecarTextures.
constexpr const char* kTextureFolder = "textures";

// The part of the car every other part is measured against. One scale factor is
// worked out from this slot and used for all of them, so the car arrives as one
// vehicle rather than twenty independently resized pieces.
constexpr const char* kVehicleBodySlot = "widebody0";

// Every character resource the archive holds.
//
// Which one the game asks for is not a question that can be answered from the
// outside: a character is translated to another before it is loaded, on whether
// the racer is on a bike and whether they are the local player -- the engine
// says as much when it fails, "Cannot translate character to requested type %s,
// bike=%d local=%d" -- and the table that does it is built at runtime from data,
// keyed by name. So switching to a motorcycle can quietly ask for a resource
// nobody replaced, and the shipped driver comes back.
//
// The archive cannot be listed either: its table of contents keeps hashes, not
// names, and the only name in it is the root. What it can do is answer whether a
// name exists, so the shipped naming -- drv_<two letters>_<number>_set, with the
// odd word in the middle -- is walked and every hit kept. On the retail archive
// that turns up 33 characters, the eight selectable ones among them.
std::vector<std::string> DiscoverCharacterAssets(const Rpf3Reader& archive) {
    static const char* const kInfixes[] = {"", "jacket", "cap"};
    std::vector<std::string> found;
    char name[64];

    for (char first = 'a'; first <= 'z'; ++first) {
        for (char second = 'a'; second <= 'z'; ++second) {
            for (int number = 0; number <= 99; ++number) {
                for (const char* infix : kInfixes) {
                    for (int digits = 2; digits <= 3; ++digits) {
                        if (*infix) {
                            std::snprintf(name, sizeof name, "drv_%c%c_%s_%0*d_set", first, second,
                                          infix, digits, number);
                        } else {
                            std::snprintf(name, sizeof name, "drv_%c%c_%0*d_set", first, second,
                                          digits, number);
                        }
                        Rpf3Entry entry;
                        if (archive.Find(std::string("resources/character/") + name + "/" + name +
                                             ".xrsc",
                                         entry)) {
                            found.emplace_back(name);
                        }
                    }
                }
            }
        }
    }
    return found;
}

// A character ships as several resources built from the same mesh: the base
// set, the "_h" variant the game raises to up close
// (mcCineScript::UseRacerNativeCharacter), and the low LOD the cine loader
// falls back to. Replacing only the base leaves the original model showing
// whenever the game switches, so every variant that exists gets the same mesh.
std::vector<std::string> AssetVariants(const std::string& asset) {
    std::vector<std::string> variants{asset, asset + "_h"};

    constexpr const char* kSuffix = "_set";
    const size_t suffix_len = std::strlen(kSuffix);
    if (asset.size() > suffix_len &&
        asset.compare(asset.size() - suffix_len, suffix_len, kSuffix) == 0) {
        const std::string stem = asset.substr(0, asset.size() - suffix_len);
        variants.push_back(stem + "_lod02_set");
        variants.push_back(stem + "_lod02_set_h");
    }
    return variants;
}

bool IsMeshFile(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

void ScanFolder(const std::filesystem::path& folder, const std::string& mod_name,
                std::vector<ModEntry>& out) {
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!file.is_regular_file() || !IsMeshFile(file.path())) continue;

        // A sidecar naming the asset this one is built out of: put
        // "whl_am_bbs_ch" in whl_stk_bmw_740i_98.donor and the shipped BBS is
        // the template while the file lands under the new name.
        std::string donor;
        std::ifstream sidecar(folder / (file.path().stem().string() + ".donor"));
        if (sidecar) {
            std::getline(sidecar, donor);
            const size_t first = donor.find_first_not_of(" \t\r\n");
            const size_t last = donor.find_last_not_of(" \t\r\n");
            donor = first == std::string::npos ? std::string()
                                               : donor.substr(first, last - first + 1);
        }
        out.push_back(
            ModEntry{file.path().stem().string(), file.path(), mod_name, std::move(donor)});
    }
}

// Diagnostics land beside the cache, in a folder the mod scan skips for
// starting with a dot -- which matters, because one of the three files written
// per asset is an .obj and would otherwise be picked up as a mod of its own.
void WriteDiagnostics(const std::filesystem::path& cache_dir, const std::string& mod_name,
                      const std::string& variant, const RewriteStats& stats) {
    if (stats.report.empty() && stats.obj_after.empty()) return;

    const std::filesystem::path folder = cache_dir.parent_path() / ".diag";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    const std::string stem = mod_name + "__" + variant;
    auto put = [&](const char* suffix, const std::string& body) {
        if (body.empty()) return;
        std::ofstream out(folder / (stem + suffix), std::ios::binary);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    };
    put(".txt", stats.report);
    put("_before.obj", stats.obj_before);
    put("_after.obj", stats.obj_after);
    put("_template.obj", stats.obj_template);
}

// Collects everything under `files_dir`, keeping the folder structure as the
// archive path. Empty files are skipped: an entry of size zero would take the
// shipped file's place and give the game nothing.
void ScanRawFiles(const std::filesystem::path& files_dir, const std::string& mod_name,
                  std::vector<RawFile>& out) {
    std::error_code ec;
    for (const auto& file : std::filesystem::recursive_directory_iterator(files_dir, ec)) {
        if (ec) break;
        if (!file.is_regular_file()) continue;

        std::error_code rel_ec;
        const std::filesystem::path relative =
            std::filesystem::relative(file.path(), files_dir, rel_ec);
        if (rel_ec || relative.empty()) continue;

        std::string archive_path = relative.generic_string();
        if (archive_path.empty() || archive_path.front() == '.') continue;

        if (std::filesystem::file_size(file.path(), rel_ec) == 0 || rel_ec) {
            LARECOMP_APP_ERROR("[mods] {}/files/{}: empty, skipped", mod_name, archive_path);
            continue;
        }

        out.push_back(RawFile{std::move(archive_path), file.path(), mod_name});
    }
}

void ScanMods(const std::filesystem::path& models_dir, std::vector<ModEntry>& characters,
              std::vector<ModEntry>& rims, std::vector<VehicleMod>& vehicles,
              std::vector<RawFile>& raw_files) {
    std::error_code ec;

    for (const auto& mod_dir : std::filesystem::directory_iterator(models_dir, ec)) {
        if (ec) break;
        if (!mod_dir.is_directory()) continue;
        if (mod_dir.path().filename().string().rfind('.', 0) == 0) continue;  // .cache etc

        const std::string mod_name = mod_dir.path().filename().string();
        ScanFolder(mod_dir.path(), mod_name, characters);

        // Its own error_code: a mod without a rims folder is the normal case,
        // and letting that failure land in the iterator's own `ec` would end the
        // whole scan at the next `if (ec) break` -- every mod after the first one
        // without wheels silently stopped being seen.
        std::error_code sub_ec;
        const std::filesystem::path rim_dir = mod_dir.path() / kRimFolder;
        if (std::filesystem::is_directory(rim_dir, sub_ec)) ScanFolder(rim_dir, mod_name, rims);

        const std::filesystem::path files_dir = mod_dir.path() / kFilesFolder;
        if (std::filesystem::is_directory(files_dir, sub_ec))
            ScanRawFiles(files_dir, mod_name, raw_files);

        const std::filesystem::path vehicle_dir = mod_dir.path() / kVehicleFolder;
        if (!std::filesystem::is_directory(vehicle_dir, sub_ec)) continue;
        std::error_code car_ec;
        for (const auto& car : std::filesystem::directory_iterator(vehicle_dir, car_ec)) {
            if (car_ec) break;
            if (!car.is_directory()) continue;
            vehicles.push_back(VehicleMod{car.path().filename().string(), car.path(), mod_name});
        }
    }
}

// Whether an image is meant to cover a surface rather than sit on one.
//
// It is the only thing in a mod that says which. Every shipped wheel texture is
// a decal sheet: a logo floating in empty space, two thirds of it transparent,
// and across the DXT5 ones the median is a third opaque. A texture painted to
// be a rim's skin has nothing to be transparent for. So an image with real
// transparency is a badge and an opaque one is a skin, and the threshold is set
// far from both -- an image is a skin only if almost every texel is solid, which
// leaves room for a soft edge without letting a logo through.
// Only the cells that were filled. The atlas grid is square, so a mod carrying
// one texture and one untextured material fills two cells of a two by two and
// leaves the other two as the zeroes they were allocated with. Measuring the
// whole sheet reads that padding as transparency and calls a solid rim skin a
// badge, which is how a wheel mod ends up with its paint on the hub cap.
bool ImageIsOpaque(const Image& image, uint32_t cells, uint32_t cell_size) {
    if (image.empty() || cells == 0 || cell_size == 0) return false;

    const uint32_t columns = std::max(1u, image.width / cell_size);
    size_t texels = 0, clear = 0;
    for (uint32_t cell = 0; cell < cells; ++cell) {
        const uint32_t left = (cell % columns) * cell_size;
        const uint32_t top = (cell / columns) * cell_size;
        if (left + cell_size > image.width || top + cell_size > image.height) continue;
        for (uint32_t y = 0; y < cell_size; ++y) {
            const size_t row = (static_cast<size_t>(top + y) * image.width + left) * 4;
            for (uint32_t x = 0; x < cell_size; ++x) {
                ++texels;
                if (image.rgba[row + static_cast<size_t>(x) * 4 + 3] < 250) ++clear;
            }
        }
    }
    return texels != 0 && clear * 100 < texels * 2;
}

bool LoadMeshFile(const std::filesystem::path& path, Mesh& mesh, std::string& error) {
    std::string suffix = path.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix == ".obj" ? LoadObj(path, mesh, error) : LoadGltf(path, mesh, error);
}

// Image files a mod may drop beside its model, most wanted first. .dds is on
// the list because it is what the games these models come from stored, and
// making people convert a folder of them before anything shows is a step that
// buys nothing -- see DecodeDds.
constexpr const char* kTextureExtensions[] = {".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp"};

// Textures a mod ships as loose files rather than inside its model.
//
// A .glb can embed one image per material and a well-behaved exporter does, but
// two common cases do not reach here that way: a .obj carries no images at all,
// and a .glb baked from a converted model arrives with its materials stripped to
// names. Both are the shape a car mod actually turns up in -- the model comes
// out of one tool and the textures out of another -- so a `textures/` folder
// beside the model, one file per material name, is read as if the model had
// carried them.
//
// Only materials that have no image already: what the file itself says wins, so
// dropping a folder next to a fully textured .glb changes nothing.
size_t LoadSidecarTextures(const std::filesystem::path& folder, Mesh& mesh) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return 0;

    // The folder is listed once and keyed by lowercased stem: a material called
    // "MAT_7" has to find "mat_7.dds", and asking the filesystem about each
    // name in turn would be case-sensitive on the platforms that are.
    std::map<std::string, std::filesystem::path> by_stem;
    for (const auto& file : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!file.is_regular_file()) continue;

        std::string extension = file.path().extension().string();
        std::string stem = file.path().stem().string();
        for (std::string* text : {&extension, &stem}) {
            std::transform(text->begin(), text->end(), text->begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (std::find(std::begin(kTextureExtensions), std::end(kTextureExtensions), extension) ==
            std::end(kTextureExtensions)) {
            continue;
        }
        // Earlier extensions win, so a folder holding both mat_7.dds and
        // mat_7.png resolves the same way every run.
        const auto existing = by_stem.find(stem);
        if (existing != by_stem.end()) {
            std::string held = existing->second.extension().string();
            std::transform(held.begin(), held.end(), held.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto rank = [](const std::string& e) {
                return std::find(std::begin(kTextureExtensions), std::end(kTextureExtensions), e) -
                       std::begin(kTextureExtensions);
            };
            if (rank(held) <= rank(extension)) continue;
        }
        by_stem[stem] = file.path();
    }
    if (by_stem.empty()) return 0;

    std::map<std::string, int> image_of;  // stem -> index into mesh.images
    // Two materials naming the same picture have to end up on the same image,
    // or they each take a cell of the atlas and everything in it gets smaller.
    //
    // A folder keyed by material name cannot avoid holding duplicates -- this
    // car's three tail-lamp materials are three copies of one 750i_taillight,
    // because the map from material to texture is what the folder states -- so
    // the loader collapses them by content. Measured: the wheel-and-lamp shader
    // reported six images for four pictures and took a four by four grid at
    // 128 pixels a cell where two by two at 256 was available.
    std::map<std::pair<size_t, size_t>, int> image_of_content;
    size_t attached = 0;
    for (MeshPart& part : mesh.parts) {
        if (part.image >= 0 || part.material.empty()) continue;

        std::string stem = part.material;
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const auto cached = image_of.find(stem);
        if (cached != image_of.end()) {
            part.image = cached->second;
            if (cached->second >= 0) ++attached;
            continue;
        }

        const auto file = by_stem.find(stem);
        if (file == by_stem.end()) {
            image_of[stem] = -1;
            continue;
        }

        std::ifstream stream(file->second, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            image_of[stem] = -1;
            continue;
        }

        // Keyed by size and a checksum rather than by the bytes themselves:
        // these are whole textures, and holding a second copy of each to compare
        // against costs more than the cell it saves is worth.
        size_t sum = 0;
        for (uint8_t byte : bytes) sum = sum * 131u + byte;
        const auto same = REXCVAR_GET(model_mods_share_images)
                              ? image_of_content.find({bytes.size(), sum})
                              : image_of_content.end();
        if (same != image_of_content.end()) {
            image_of[stem] = same->second;
            part.image = same->second;
            ++attached;
            continue;
        }

        const int index = static_cast<int>(mesh.images.size());
        image_of_content[{bytes.size(), sum}] = index;
        mesh.images.push_back(std::move(bytes));
        image_of[stem] = index;
        part.image = index;
        ++attached;
    }
    return attached;
}

// Turns a wheel model onto the axle the game spins wheels about.
//
// Every shipped wheel measures half a unit across X against a full unit in Y and
// Z, so X is the axle and the other two are the diameter. A model exported from
// a modelling tool lands on whichever axis that tool calls up, and the thin axis
// is what gives it away -- a wheel is a disc, so its shortest extent is the one
// through the hub. Rotating that onto X is the whole correction, and it is a
// quarter turn either way or nothing at all.
void AlignRimToAxle(const Mesh& mesh, MeshOffset& offset) {
    if (mesh.vertices.empty()) return;

    float min[3], max[3];
    mesh.Bounds(min, max);

    int thinnest = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (max[axis] - min[axis] < max[thinnest] - min[thinnest]) thinnest = axis;
    }

    if (thinnest == 1) {
        offset.roll += 90.0f;   // about Z, so Y lands on X
    } else if (thinnest == 2) {
        offset.yaw += 90.0f;    // about Y, so Z lands on X
    }
}

// One slot of a car, and the glTF nodes that fill it.
struct PartMapping {
    std::string slot;
    std::vector<std::string> groups;
};

std::vector<PartMapping> ReadPartsFile(const std::filesystem::path& path, std::string& error) {
    std::vector<PartMapping> out;
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path.filename().string();
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        // A comment starts at a '#' that begins the line or follows whitespace.
        // Not at every '#': "InteriorTrim#3" names the fourth drawn InteriorTrim,
        // and cutting it there silently turned every such rule into plain
        // "InteriorTrim" -- the whole BMW cabin, fourteen materials meant for ten
        // shaders, landed on one.
        for (size_t at = line.find('#'); at != std::string::npos; at = line.find('#', at + 1)) {
            if (at == 0 || line[at - 1] == ' ' || line[at - 1] == '\t') {
                line.resize(at);
                break;
            }
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        auto trim = [](std::string text) {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string();
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        };

        PartMapping mapping;
        mapping.slot = trim(line.substr(0, equals));
        if (mapping.slot.empty()) continue;

        std::string rest = line.substr(equals + 1);
        size_t start = 0;
        while (start <= rest.size()) {
            const size_t comma = rest.find(',', start);
            std::string group = trim(rest.substr(start, comma == std::string::npos
                                                            ? std::string::npos
                                                            : comma - start));
            if (!group.empty()) mapping.groups.push_back(group);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!mapping.groups.empty()) out.push_back(std::move(mapping));
    }
    return out;
}

// The triangles a slot line names, as a mesh of their own.
//
// A name matches either the object a part came from (`o` in the .obj, the node
// in the .glb) or its MATERIAL. Both, because the two ways a model says "this
// is the cabin" are a node called that and a set of materials that are it, and
// a car exported out of GTA IV only ever offers the second: the bake collapses
// the whole shell into one node, so `interior0 = body` would claim the entire
// car while `interior0 = MAT_1, MAT_2, ...` claims the fourteen materials that
// really are its cabin.
//
// This is what lets a heavy car stop inflating body_lod_0. Measured on
// vp_chv_impala_96: its body_lod_0 is 245,760 bytes and its interior0_lod_0 is
// 950,272, drawing 25,069 triangles over TWENTY shader slots. The game does not
// put a cabin in the body, and neither should a mod.
bool NamesPart(const std::vector<std::string>& names, const MeshPart& part) {
    return std::find(names.begin(), names.end(), part.group) != names.end() ||
           std::find(names.begin(), names.end(), part.material) != names.end();
}

// The box a set of named groups occupies inside `mesh`.
bool GroupBounds(const Mesh& mesh, const std::vector<std::string>& groups,
                 float min_out[3], float max_out[3]) {
    bool any = false;
    for (const MeshPart& part : mesh.parts) {
        // Same match as ExtractGroups, or the box is measured over a different
        // set of triangles than the one that gets built.
        if (!NamesPart(groups, part)) continue;
        for (uint32_t i = 0; i < part.vertex_count; ++i) {
            const MeshVertex& vertex = mesh.vertices[part.first_vertex + i];
            const float p[3] = {vertex.px, vertex.py, vertex.pz};
            for (int c = 0; c < 3; ++c) {
                if (!any) {
                    min_out[c] = max_out[c] = p[c];
                } else {
                    min_out[c] = std::min(min_out[c], p[c]);
                    max_out[c] = std::max(max_out[c], p[c]);
                }
            }
            any = true;
        }
    }
    return any;
}

// Everything a slot line does NOT name. A material routed to a part slot has to
// leave the body, or the car is drawn twice: once in the body it was never
// removed from and once in the slot it was sent to, which reads as z-fighting
// over the whole cabin and spends the budget twice.
Mesh ExcludeGroups(const Mesh& mesh, const std::vector<std::string>& groups) {
    std::vector<uint32_t> remap(mesh.vertices.size(), 0xFFFFFFFFu);
    Mesh out;
    out.images = mesh.images;

    for (const MeshPart& part : mesh.parts) {
        if (NamesPart(groups, part)) continue;
        MeshPart copy = part;
        copy.first_vertex = static_cast<uint32_t>(out.vertices.size());
        for (uint32_t i = 0; i < part.vertex_count; ++i) {
            const uint32_t source = part.first_vertex + i;
            remap[source] = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(mesh.vertices[source]);
        }
        out.parts.push_back(copy);
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = remap[mesh.indices[i]];
        const uint32_t b = remap[mesh.indices[i + 1]];
        const uint32_t c = remap[mesh.indices[i + 2]];
        if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }
    return out;
}

// Everything belonging to `groups`, lifted out as a mesh of its own. A triangle
// comes along only when all three of its vertices do, so a primitive is never
// split down the middle.
Mesh ExtractGroups(const Mesh& mesh, const std::vector<std::string>& groups) {
    std::vector<uint32_t> remap(mesh.vertices.size(), 0xFFFFFFFFu);
    Mesh out;
    out.images = mesh.images;

    for (const MeshPart& part : mesh.parts) {
        if (!NamesPart(groups, part)) continue;
        MeshPart copy = part;
        copy.first_vertex = static_cast<uint32_t>(out.vertices.size());
        for (uint32_t i = 0; i < part.vertex_count; ++i) {
            const uint32_t source = part.first_vertex + i;
            remap[source] = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(mesh.vertices[source]);
        }
        out.parts.push_back(copy);
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = remap[mesh.indices[i]];
        const uint32_t b = remap[mesh.indices[i + 1]];
        const uint32_t c = remap[mesh.indices[i + 2]];
        if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }
    return out;
}

// Puts a part where the slot it replaces sits: turned to face the way the game
// does, scaled by the car's own factor -- never by the part's, or the pieces
// stop matching each other -- and moved so its middle lands on the slot's.
void PlacePart(Mesh& mesh, float yaw, float scale, const float slot_min[3],
               const float slot_max[3]) {
    TransformMesh(mesh, yaw, 0.0f, 0.0f, 1.0f);

    float min[3], max[3];
    mesh.Bounds(min, max);
    const float centre[3] = {(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f,
                             (min[2] + max[2]) * 0.5f};
    const float target[3] = {(slot_min[0] + slot_max[0]) * 0.5f,
                             (slot_min[1] + slot_max[1]) * 0.5f,
                             (slot_min[2] + slot_max[2]) * 0.5f};
    for (auto& vertex : mesh.vertices) {
        vertex.px = (vertex.px - centre[0]) * scale + target[0];
        vertex.py = (vertex.py - centre[1]) * scale + target[1];
        vertex.pz = (vertex.pz - centre[2]) * scale + target[2];
    }
}

// ---------------------------------------------------------------------------
// A car the game has never heard of.
//
// Everything above replaces a vehicle MCLA ships. This builds one it does not:
// the roster is data, so a `tune/vehicle/vehicle000N.lst` entry naming a car
// makes the showroom ask for `$/resources/vehicle/<name>/...`, and nothing
// under that path has to have existed before. What the loader
// (sub_823723C8) asks for, in order:
//
//   body_lod_0..2          the drawable, three LODs, resource type 63
//   <name>.xtl, <name>.xtp two type-83 material packs, which also carry the
//                          physics bound the player-vehicle path needs
//   <name>.xct             the car tune, type 37938 (50 in the archive entry)
//   <name>.xspm            type 19 -- the manifest saying which parts exist
//   <slot><i>_lod_<l>      55 slot names x 10 indices x 3 LODs, gated by the
//                          .xspm and each existence-checked
//
// Every one of those sits behind an exists-first check, so a file a mod does
// not ship is simply not drawn. That is what makes a donor workable: clone the
// parts of another vehicle that are not geometry -- its tune, its packs, its
// bound, its manifest -- and put the mod's own mesh in the drawable.
//
// The donor is named in parts.txt, and so is the material map, because a car is
// not one surface. MCLA draws paint, glass, lights and trim with different
// shaders of the same drawable, and a body dealt across all of them comes back
// with painted windows. A mod says which of its materials is which by naming
// the game's own car shader, and the rewrite then runs once per shader.
struct ShaderRule {
    std::string material;  // the mod's material name, or "*" for the fallback
    std::string effect;    // a name from the game's car shader list
    // Which drawable the rule speaks for: empty is the car's body, otherwise a
    // part slot such as "bumper_f0".
    //
    // One map for the whole car is not enough once parts are filled separately,
    // because the same shader index appears in more than one of its drawables.
    // Measured on the Impala: CarPaintCustomizable#1 draws 673 vertices of the
    // body AND 1731 of bumper_f0, so a material sent to it without saying which
    // drawable is meant would be written into both and drawn twice. A rule
    // naming a slot applies only there and beats the unqualified one.
    std::string slot;
};

struct VehiclePlan {
    std::string donor;
    std::vector<PartMapping> slots;
    std::vector<ShaderRule> shaders;
    std::vector<std::string> silenced;  // slots shipped empty
    std::vector<std::string> kept;  // effects left exactly as the donor drew them
    // Effects whose UVs are flooded with the template's, because the mod has no
    // picture for them -- glass, whose GTA shader carries no diffuse at all.
    std::vector<std::string> flat_uv;
    // How much of a capped body's room each effect is worth, by name. Absent
    // means 1. Triangle count alone says how much a shader brought, not how
    // much of it the player looks at -- a cabin blocker is half the mesh and
    // nobody sees it, the painted shell is the car's whole silhouette.
    std::map<std::string, float> effect_weight;
    float scale = 1.0f;
    float yaw = 0.0f;
    // Pitch and roll, for a model whose file does not agree with the game about
    // which way is up. A .glb out of a modelling tool usually does -- the
    // exporter is asked for Y-up and gives it -- but a .obj converted straight
    // out of another game does not: the openFormats export of this car measures
    // 4.857 along Y and 1.294 along Z, which is Z-up, and the quarter turn that
    // fixes it used to be done by hand in Blender on the way through.
    float pitch = 0.0f;
    float roll = 0.0f;
    float offset[3] = {0.0f, 0.0f, 0.0f};
    bool placed = false;  // whether a measured transform was given

    // Whether the tune is rewritten to carry this car's name, or copied exactly
    // as the donor holds it.
    //
    // Rewriting is what the car wants -- the tune objects are filed under
    // "<name>_base", "<name>_mods" and so on (sub_82363990 formats exactly
    // those), so a clone left carrying the donor's name has its handling looked
    // up under a name that is not its own. But the rewritten bytes cannot be
    // put back the way they came: a resource is stored LZX-compressed, a stored
    // LZX stream of 32768 bytes is larger than the 32768 the streamer will read
    // for it, and the only other shape on offer is uncompressed -- which the
    // game never uses. Not once: of the 14,927 resources in xarchive_cache.rpf,
    // 14,927 are compressed and none are not, and pgStreamer::Read sizes a
    // compressed request as vsize + psize + 12, the twelve being the header
    // that an uncompressed payload does not carry.
    //
    // So the default is the copy. A car with the donor's tune name drives like
    // nothing in particular; a car with an uncompressed tune never finished
    // loading, which is what four separate attempts at this produced. Those
    // attempts predate the 12-byte header BuildRsc5File now gives the
    // uncompressed road, so the rename is worth trying again -- but not by
    // default.
    bool rename_tune = false;

    // Where the donor's licence plate has to move to land on this car's rear.
    // Metres, in the car's own space: x is lateral, y up, z back.
    float plate[3] = {0.0f, 0.0f, 0.0f};

    // Clone the donor and rewrite nothing.
    //
    // A control, not a feature. Two things could keep a new car from loading --
    // the machinery that gives it a name and a set of files, or the geometry
    // written into its body -- and they can only be told apart by shipping one
    // without the other. A verbatim car is the donor under a new name, down to
    // the last byte of its drawables: if that loads, the naming is sound and
    // the fault is in the mesh; if it does not, the mesh was never the problem.
    bool verbatim = false;
};

// Pulls the reserved keys out of a parts.txt and leaves the rest as slots.
//
// The grammar is the one that was already there -- `key = value` -- so the new
// directives are only reserved keys, and `shader.<material>` carries its second
// operand in the key. No existing parts.txt changes meaning.
//
//   donor          = vp_chv_police_96
//   scale          = 1.0735
//   offset         = 0 0.7557 0.111
//   yaw            = 0
//   pitch          = -90        (a model whose file is Z-up)
//   roll           = 0
//   verbatim       = 1          (a control: clone the donor, rewrite nothing)
//   tune           = rename     (rewrite the tune's name; see rename_tune)
//   silence        = hood0, skirts0
//   keep           = LicensePlate
//   weight.CarPaintCustomizable = 2.5
//   shader.MAT_21  = CarPaintCustomizable
//   shader.*       = BumpSpecAlpha
//   shader.bumper_f0.MAT_21 = CarPaintCustomizable#1   (only in that part)
//   bumper_f0      = bumper_f   (a slot line: which of the model's groups fill it)
//   body           = admiral_high
VehiclePlan ReadVehiclePlan(const std::vector<PartMapping>& mappings) {
    VehiclePlan plan;
    for (const PartMapping& mapping : mappings) {
        const std::string& key = mapping.slot;
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower.rfind("shader.", 0) == 0) {
            // "shader.<material>" or "shader.<slot>.<material>". A material name
            // never holds a dot, so the second dot is unambiguous.
            std::string rest = key.substr(7);
            std::string slot;
            const size_t dot = rest.find('.');
            if (dot != std::string::npos) {
                slot = rest.substr(0, dot);
                rest = rest.substr(dot + 1);
            }
            plan.shaders.push_back(
                ShaderRule{std::move(rest), mapping.groups.front(), std::move(slot)});
            continue;
        }
        if (lower.rfind("weight.", 0) == 0) {
            plan.effect_weight[key.substr(7)] =
                std::strtof(mapping.groups.front().c_str(), nullptr);
            continue;
        }
        if (lower == "donor") {
            plan.donor = mapping.groups.front();
            continue;
        }
        if (lower == "verbatim") {
            plan.verbatim = mapping.groups.front() != "0";
            continue;
        }
        if (lower == "plate") {
            for (size_t i = 0; i < 3 && i < mapping.groups.size(); ++i)
                plan.plate[i] = std::strtof(mapping.groups[i].c_str(), nullptr);
            continue;
        }
        if (lower == "tune") {
            plan.rename_tune = mapping.groups.front() == "rename";
            continue;
        }
        if (lower == "silence") {
            for (const std::string& slot : mapping.groups) plan.silenced.push_back(slot);
            continue;
        }
        if (lower == "flat_uv") {
            for (const std::string& effect : mapping.groups) plan.flat_uv.push_back(effect);
            continue;
        }
        if (lower == "keep") {
            for (const std::string& effect : mapping.groups) plan.kept.push_back(effect);
            continue;
        }
        if (lower == "scale") {
            plan.scale = std::strtof(mapping.groups.front().c_str(), nullptr);
            plan.placed = true;
            continue;
        }
        if (lower == "yaw") {
            plan.yaw = std::strtof(mapping.groups.front().c_str(), nullptr);
            continue;
        }
        if (lower == "pitch") {
            plan.pitch = std::strtof(mapping.groups.front().c_str(), nullptr);
            continue;
        }
        if (lower == "roll") {
            plan.roll = std::strtof(mapping.groups.front().c_str(), nullptr);
            continue;
        }
        if (lower == "offset") {
            // Three numbers on one line, so they arrive as a single "group".
            std::istringstream stream(mapping.groups.front());
            for (int i = 0; i < 3; ++i) stream >> plan.offset[i];
            plan.placed = true;
            continue;
        }
        plan.slots.push_back(mapping);
    }
    return plan;
}

// Which MCLA shader index each of the mod's materials must be drawn with, for
// one drawable of the car.
//
// `effects` is what the donor's own material pack states, one entry per shader
// index; a rule names an effect and the first shader running it wins. A rule
// naming an effect the donor does not have is dropped with a line in the log --
// sending those triangles somewhere else without saying so would be worse.
//
// `slot` is empty for the body and a part slot otherwise. Unqualified rules are
// read first and the slot's own rules are read after, so a slot rule overwrites
// the general one for that material and nothing else.
//
// `drawn` is the set of shader indices the BODY actually renders with, and it
// is not the same set as the material pack's. Measured on vp_chv_impala_96: the
// pack states 47 materials, the body draws 13, and CarPaintCustomizable appears
// at BOTH 19 and 23 with only 23 drawn. Matching by name alone takes the first
// in the pack, which is 19, and 19 has no geometry -- so the whole painted
// shell, 31,844 triangles, was silently left as the donor's and the car came
// back looking like a Chevrolet Impala. Pass it empty to match against the pack
// as before.
std::map<std::string, uint32_t> ResolveShaderMap(const VehiclePlan& plan,
                                                 const std::vector<uint32_t>& effects,
                                                 const std::vector<uint32_t>& drawn,
                                                 const std::string& mod_name,
                                                 const std::string& car, int32_t& fallback,
                                                 const std::string& slot = std::string()) {
    auto has_geometry = [&](size_t index) {
        return drawn.empty() || (index < drawn.size() && drawn[index] != 0);
    };
    std::map<std::string, uint32_t> out;
    fallback = -1;

    std::vector<const ShaderRule*> ordered;
    for (const ShaderRule& rule : plan.shaders) {
        if (rule.slot.empty()) ordered.push_back(&rule);
    }
    for (const ShaderRule& rule : plan.shaders) {
        if (!rule.slot.empty() && rule.slot == slot) ordered.push_back(&rule);
    }

    for (const ShaderRule* entry : ordered) {
        const ShaderRule& rule = *entry;
        // A rich donor runs the same effect on many shaders -- the Impala has
        // fourteen InteriorTrim materials, one per surface of its cabin, each
        // reading a texture of its own -- and a name alone can only ever name
        // the first of them. "#N" says which: "InteriorTrim#3" is the fourth
        // shader running InteriorTrim, and a bare "#12" is shader twelve
        // whatever it runs. Without the suffix the first match is taken, so
        // nothing already written changes meaning.
        std::string effect_name = rule.effect;
        int32_t nth = 0;
        const size_t hash = effect_name.find('#');
        if (hash != std::string::npos) {
            const std::string digits = effect_name.substr(hash + 1);
            if (digits.empty() ||
                digits.find_first_not_of("0123456789") != std::string::npos) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: '{}' does not end in a shader number",
                                   mod_name, car, rule.effect);
                continue;
            }
            nth = std::atoi(digits.c_str());
            effect_name.erase(hash);
        }

        int32_t shader = -1;
        if (effect_name.empty()) {
            // "#12": the shader index itself, for a donor whose effect names do
            // not tell its materials apart.
            if (static_cast<size_t>(nth) >= effects.size()) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the donor has {} shaders, so '{}' has "
                                   "nowhere to go", mod_name, car, effects.size(), rule.effect);
                continue;
            }
            shader = nth;
            if (!has_geometry(static_cast<size_t>(nth))) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: shader {} exists but the body draws "
                                   "nothing with it, so '{}' would be written into nothing",
                                   mod_name, car, nth, rule.effect);
                continue;
            }
        } else {
            const int32_t effect = CarEffectIndex(effect_name);
            if (effect < 0) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: '{}' is not one of the game's car "
                                   "shaders", mod_name, car, effect_name);
                continue;
            }
            // Only the occurrences that have geometry are counted, so "#1"
            // means the second DRAWN one rather than the second in the pack.
            int32_t seen = 0, in_pack = 0;
            for (size_t i = 0; i < effects.size(); ++i) {
                if (effects[i] != static_cast<uint32_t>(effect)) continue;
                ++in_pack;
                if (!has_geometry(i)) continue;
                if (seen++ < nth) continue;
                shader = static_cast<int32_t>(i);
                break;
            }
            if (shader < 0 && seen > 0) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the body draws with {} {} time(s), so "
                                   "'{}' has nowhere to go", mod_name, car, effect_name, seen,
                                   rule.effect);
                continue;
            }
            if (shader < 0 && in_pack > 0 && !slot.empty() && rule.slot.empty()) {
                continue;   // a body rule, and this slot simply does not draw that shader
            }
            if (shader < 0 && in_pack > 0) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the donor's pack states {} {} time(s) "
                                   "and the body draws NONE of them, so '{}' would be written "
                                   "into nothing -- pick a shader the body renders",
                                   mod_name, car, effect_name, in_pack, rule.material);
                continue;
            }
        }
        if (shader < 0) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the donor draws nothing with {}, so '{}' "
                               "has nowhere to go", mod_name, car, rule.effect, rule.material);
            continue;
        }
        if (rule.material == "*") {
            fallback = shader;
        } else {
            out[rule.material] = static_cast<uint32_t>(shader);
        }
    }
    return out;
}

// The triangles of `mesh` whose material is drawn by `shader`, as a mesh of
// their own. Vertices are renumbered, so what comes back stands alone.
Mesh ExtractShader(const Mesh& mesh, const std::map<std::string, uint32_t>& shader_of,
                   int32_t fallback, uint32_t shader) {
    std::vector<uint32_t> remap(mesh.vertices.size(), 0xFFFFFFFFu);
    Mesh out;
    out.images = mesh.images;

    for (const MeshPart& part : mesh.parts) {
        const auto rule = shader_of.find(part.material);
        const int32_t target =
            rule != shader_of.end() ? static_cast<int32_t>(rule->second) : fallback;
        if (target < 0 || static_cast<uint32_t>(target) != shader) continue;

        MeshPart copy = part;
        copy.first_vertex = static_cast<uint32_t>(out.vertices.size());
        for (uint32_t i = 0; i < part.vertex_count; ++i) {
            const uint32_t source = part.first_vertex + i;
            remap[source] = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(mesh.vertices[source]);
        }
        out.parts.push_back(std::move(copy));
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = remap[mesh.indices[i]];
        const uint32_t b = remap[mesh.indices[i + 1]];
        const uint32_t c = remap[mesh.indices[i + 2]];
        if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }
    return out;
}

// Moves the whole mesh into the game's space, once, before any slot sees it.
//
// The per-slot placement above centres a part on the box of the part it
// replaces, which is right when a mod is a set of parts standing in for the
// donor's. A whole-body swap wants the opposite: one measured transform for the
// entire car, so its wheels land on the axles the donor's skeleton keeps rather
// than wherever a bounding box happened to sit. The numbers come from
// measurement -- the donor states its axles in its own skeleton (`axl_0..3`),
// the mod states its wheels, and the ratio of the two wheelbases is the scale.
void PlaceCar(Mesh& mesh, const VehiclePlan& plan) {
    TransformMesh(mesh, plan.yaw, plan.pitch, plan.roll, plan.scale);
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.px += plan.offset[0];
        vertex.py += plan.offset[1];
        vertex.pz += plan.offset[2];
    }
}

// Renames a donor's own name where it is written inside a resource.
//
// The tune resource states the car it belongs to a dozen times -- "<car>",
// "<car>_base", "<car>_mods", "E@<car>" -- and those are the names the tune
// objects are filed under when the vehicle is built (sub_82363990 formats
// exactly those). A clone left carrying the donor's name would have its tunes
// looked up under a name that is not its own.
//
// Strings are rewritten where they sit, so the new name has to be no longer
// than the donor's; everything after it is kept and the slack is zeroed. Any
// word in the resource holding the RAGE hash of a string that changed is
// rewritten too, which is what keeps a dictionary's parallel hash array in step
// without having to know where that array is.
size_t RenameInResource(std::vector<uint8_t>& data, const std::string& from,
                        const std::string& to) {
    if (from.empty() || to.size() > from.size()) return 0;

    std::vector<std::pair<uint32_t, uint32_t>> hashes;  // old -> new
    size_t renamed = 0;

    for (size_t at = 0; at + from.size() <= data.size();) {
        if (std::memcmp(data.data() + at, from.data(), from.size()) != 0) {
            ++at;
            continue;
        }
        // Only a whole, NUL-terminated, printable string is a name. Anything
        // else that happens to hold these bytes is left alone.
        size_t start = at;
        while (start > 0 && data[start - 1] >= 0x20 && data[start - 1] < 0x7F) --start;
        size_t end = at + from.size();
        while (end < data.size() && data[end] >= 0x20 && data[end] < 0x7F) ++end;
        if (end >= data.size() || data[end] != 0) {
            at += from.size();
            continue;
        }

        const std::string before(reinterpret_cast<const char*>(data.data() + start), end - start);
        std::string after = before;
        for (size_t found = after.find(from); found != std::string::npos;
             found = after.find(from, found + to.size())) {
            after.replace(found, from.size(), to);
        }
        hashes.emplace_back(RageHash(before), RageHash(after));

        std::memcpy(data.data() + start, after.data(), after.size());
        std::memset(data.data() + start + after.size(), 0, (end - start) - after.size());
        ++renamed;
        at = end;
    }

    for (const auto& [old_hash, new_hash] : hashes) {
        for (size_t at = 0; at + 4 <= data.size(); at += 4) {
            if (LoadBE32(data.data() + at) != old_hash) continue;
            data[at + 0] = static_cast<uint8_t>(new_hash >> 24);
            data[at + 1] = static_cast<uint8_t>(new_hash >> 16);
            data[at + 2] = static_cast<uint8_t>(new_hash >> 8);
            data[at + 3] = static_cast<uint8_t>(new_hash);
        }
    }
    return renamed;
}

// The 55 part slots a player vehicle can carry, in the order the loader walks
// them (the table at 0x827E9608, read by sub_823723C8). It asks for
// `<slot><index>_lod_<lod>` with index 0..9 and lod 0..2 for every one of them,
// so these names plus that shape are the whole vocabulary of a car's folder --
// which is what lets a donor's hash-named files be recognised and renamed.
const char* const kCarSlots[] = {
    "conv_top",    "conv_topup",  "hood",        "bumper_r",    "trunk_swap",
    "intercooler", "bumper_f",    "fenders",     "skirts",      "spoiler",
    "spoiler_b",   "headlight",   "taillight",   "taillight_b", "frontgrill",
    "wheeliebar",  "blower",      "bike_tail",   "bike_tank",   "bike_cowl",
    "door_l",      "door_r",      "conv_topdown", "exh_pipe",   "exh_side",
    "armfl",       "armfr",       "armrl",       "armrr",       "shockfl",
    "shockfr",     "shockrl",     "shockrr",     "guiderl",     "guiderr",
    "axle",        "subbumf",     "subbumr",     "widebody",    "spoiler_wide",
    "hood_wide",   "door_l_wide", "door_r_wide", "trunk_wide",  "interior",
    "seats_f",     "seats_p",     "door_l_int",  "door_r_int",  "stereo",
    "stereo_l",    "stereo_r",    "steer_whl",   "post_gauge",  "occluder_",
};

// Every file name a car's folder can hold, as hash -> name. The archive files
// them by hash and keeps no strings, so this is the only way to know what a
// donor's children are called -- and a child whose hash is not in here is
// copied under its hash instead of being guessed at.
std::map<uint32_t, std::string> CarFolderNames(const std::string& car) {
    std::map<uint32_t, std::string> out;
    auto put = [&](std::string name) { out.emplace(RageHash(name), std::move(name)); };

    for (int lod = 0; lod < 3; ++lod) put("body_lod_" + std::to_string(lod) + ".xrsc");
    for (const char* extension : {".xct", ".xspm", ".xtl", ".xtp"}) put(car + extension);
    for (const char* slot : kCarSlots) {
        for (int index = 0; index < 10; ++index) {
            for (int lod = 0; lod < 3; ++lod) {
                put(std::string(slot) + std::to_string(index) + "_lod_" + std::to_string(lod) +
                    ".xrsc");
            }
        }
    }
    return out;
}

// Builds one car the game does not ship, from a donor that it does.
size_t BuildDonorCar(const VehicleMod& vehicle, const VehiclePlan& plan, Mesh mesh,
                     const Rpf3Reader& archive, Rpf3Writer& writer) {
    const std::string& car = vehicle.car;
    const std::string& donor = plan.donor;
    const std::string donor_dir = "resources/vehicle/" + donor;
    const std::string car_dir = "resources/vehicle/" + car;
    std::string error;

    if (car.size() > donor.size()) {
        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the name is longer than the donor's ({}), and "
                           "the donor's tune states its own name in strings that are rewritten "
                           "where they sit -- pick a name of at most {} characters",
                           vehicle.mod_name, car, donor, donor.size());
        return 0;
    }

    // What the donor draws with, one effect per shader index. Its material pack
    // is the only place a vehicle says this: the drawable's own shader group is
    // a class with no names in it at all.
    Rsc5Resource pack;
    if (!ExtractTemplate(archive, donor_dir + "/" + donor + ".xtl", pack, error)) {
        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: donor {}: {}", vehicle.mod_name, car, donor,
                           error);
        return 0;
    }
    std::vector<uint32_t> effects;
    if (!ReadPackEffects(pack, effects) || effects.empty()) {
        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot read the donor's material pack",
                           vehicle.mod_name, car);
        return 0;
    }
    {
        std::string list;
        for (size_t i = 0; i < effects.size(); ++i) {
            list += (i ? ", " : "") + std::to_string(i) + "=" + CarEffectName(effects[i]);
        }
        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: donor {} draws with {}", vehicle.mod_name, car,
                          donor, list);
    }

    // Which of those shaders the BODY renders with. Read off body_lod_0 rather
    // than off the pack, because the two disagree and it is the drawable that
    // decides: a shader index the pack states and the drawable never names has
    // no geometry to write into, and a pass aimed at one is dropped in silence.
    std::vector<uint32_t> drawn;
    {
        Rsc5Resource body;
        std::string body_error;
        if (ExtractTemplate(archive, donor_dir + "/body_lod_0.xrsc", body, body_error)) {
            ShaderVertexStrides(body, drawn);
        }
        if (drawn.empty()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot read what donor {} draws with; "
                               "shader names will be matched against its pack instead, which "
                               "can land a pass on a shader that has no geometry",
                               vehicle.mod_name, car, donor);
        } else {
            std::string list;
            size_t count = 0;
            for (size_t i = 0; i < drawn.size(); ++i) {
                if (!drawn[i]) continue;
                list += (count++ ? ", " : "") + std::to_string(i) + "=" +
                        CarEffectName(i < effects.size() ? effects[i] : 0);
            }
            LARECOMP_APP_INFO("[mods] {}/vehicles/{}: body_lod_0 renders {} of them: {}",
                              vehicle.mod_name, car, count, list);
        }
    }

    int32_t fallback = -1;
    const std::map<std::string, uint32_t> shader_of =
        ResolveShaderMap(plan, effects, drawn, vehicle.mod_name, car, fallback);

    if (plan.placed) {
        PlaceCar(mesh, plan);   // once, on the whole model: see the caller
        float min[3], max[3];
        mesh.Bounds(min, max);
        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: placed at scale {:.4f}, box ({:.2f} {:.2f} "
                          "{:.2f}) to ({:.2f} {:.2f} {:.2f})", vehicle.mod_name, car, plan.scale,
                          min[0], min[1], min[2], max[0], max[1], max[2]);
    }

    // Which of the donor's shaders can be given a picture at all.
    //
    // The effect decides it, and the data cannot argue: CarPaintCustomizable,
    // CarGlass, BlackMatte and Chrome declare no texture parameter, so there is
    // nowhere to put one and nothing would sample it if there were. That also
    // makes them the shaders whose UVs must be left alone -- paint reads its
    // own set to place vinyls, and folding it into an atlas cell would move
    // every decal on the car.
    //
    // What is left is the donor's textured shaders, and how many of them there
    // are is the whole ceiling on a replacement's artwork. The police Caprice
    // has three: BumpSpecAlpha, CarLight and LicensePlate. A modifiable car has
    // fifteen or more, with real InteriorTrim slots for the cabin.
    std::vector<PackMaterial> pack_materials;
    const bool want_car_textures =
        REXCVAR_GET(model_mods_car_textures) && !plan.verbatim && !mesh.images.empty() &&
        ReadPackMaterials(pack, pack_materials);
    if (want_car_textures) {
        std::string list;
        size_t writable = 0;
        for (size_t i = 0; i < pack_materials.size(); ++i) {
            if (!pack_materials[i].writable) continue;
            ++writable;
            list += (list.empty() ? "" : ", ") + std::to_string(i) + "=" +
                    pack_materials[i].diffuse_name + " " +
                    std::to_string(pack_materials[i].width) + "x" +
                    std::to_string(pack_materials[i].height);
        }
        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: donor {} offers {} writable texture slot(s): {}",
                          vehicle.mod_name, car, donor, writable,
                          list.empty() ? "none" : list);
    }

    // The shell. Every group the `body` line names, which is the whole model
    // when it names none -- the shape a mod that ships one lump arrives in.
    const auto body_line = std::find_if(plan.slots.begin(), plan.slots.end(),
                                        [](const PartMapping& m) { return m.slot == "body"; });
    Mesh body_mesh = body_line != plan.slots.end() && !body_line->groups.empty()
                         ? ExtractGroups(mesh, body_line->groups)
                         : mesh;

    // Whatever a part slot claimed leaves the body. Without this the cabin is
    // built twice -- once in the body it was never removed from and once in the
    // slot it was routed to -- which both z-fights and spends the body's budget
    // on geometry that is already somewhere else, so the shell gets decimated
    // to pay for a copy of a cabin nobody can see.
    {
        std::vector<std::string> claimed;
        for (const PartMapping& mapping : plan.slots) {
            if (mapping.slot == "body" || mapping.slot == kVehicleBodySlot) continue;
            claimed.insert(claimed.end(), mapping.groups.begin(), mapping.groups.end());
        }
        if (!claimed.empty()) {
            const size_t before = body_mesh.indices.size() / 3;
            body_mesh = ExcludeGroups(body_mesh, claimed);
            const size_t after = body_mesh.indices.size() / 3;
            if (after != before) {
                LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} of {} triangles moved out of the "
                                  "body into part slot(s)", vehicle.mod_name, car,
                                  before - after, before);
            }
        }
    }
    if (!plan.verbatim && (body_mesh.vertices.empty() || body_mesh.indices.size() < 3)) {
        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the 'body' line names no geometry in the model",
                           vehicle.mod_name, car);
        return 0;
    }

    // One atlas per shader, built while the body is, kept for the packs below.
    // The packs store one picture per shader for every LOD, so only the atlas
    // LOD 0 produced is kept -- the other two are built solely to carry the same
    // UV rewrite into their own copy of the mesh.
    std::map<uint32_t, Image> atlas_of;

    // Which of the donor's own files a slot line claims, so the clone below
    // knows not to copy over what was just built.
    std::vector<std::string> built;
    size_t written = 0;

    // The body, three LODs. The near two are grown to hold the mesh whole and
    // the far one is welded down into the buffers the donor already had, which
    // is both smaller and what that LOD is for.
    for (int lod = 0; lod < 3 && !plan.verbatim; ++lod) {
        const std::string name = "body_lod_" + std::to_string(lod) + ".xrsc";
        Rsc5Resource resource;
        if (!ExtractTemplate(archive, donor_dir + "/" + name, resource, error)) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: donor {}/{}: {}", vehicle.mod_name, car,
                               donor, name, error);
            continue;
        }

        // Coarsen the page class before a single buffer is placed.
        //
        // The blocks a resource is split into are 4096 << page_shift, and a
        // buffer has to fit inside one. The donor ships an 8192 page, so a block
        // is 131072 -- and the replacement's paint alone wants 622272 bytes of
        // vertices. A 65536 page makes the block a megabyte, which covers
        // everything measured. The cost is that the segment rounds up to whole
        // 64 KB pages; the alternative is geometry read across two allocations.
        if (lod <= 1) {
            std::string shift_error;
            if (!SetVirtualPageShift(resource, kCarPageShift, shift_error)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: body_lod_{}: {}", vehicle.mod_name,
                                   car, lod, shift_error);
            }
        }

        const uint32_t shipped_size = resource.virtual_size;
        size_t filled = 0;
        std::vector<bool> claimed(effects.size(), false);

        // How the room a body may grow into is divided between the shaders.
        //
        // The passes run in order and the segment only ever grows, so a ceiling
        // applied flat would be first-come-first-served: paint asks for a
        // megabyte, takes everything, and the glass behind it gets the fifteen
        // hundred vertices the donor shipped. Each pass is given a cumulative
        // ceiling instead -- the base plus its share of the room, weighted by
        // how many triangles it actually brought -- so every shader comes out
        // reduced by roughly the same fraction and the car stays in proportion.
        size_t all_triangles = 0;
        std::vector<size_t> triangles(effects.size(), 0);
        for (uint32_t shader = 0; shader < effects.size(); ++shader) {
            const Mesh part = ExtractShader(body_mesh, shader_of, fallback, shader);
            triangles[shader] = part.indices.size() / 3;
            all_triangles += triangles[shader];
        }
        // Weighted by triangles, but with a floor under every shader that has
        // any. Strict proportion is fair by volume and wrong by eye: the glass
        // on this car is one percent of its triangles, so a strict share left
        // the windscreen with a hundred triangles while the cabin blocker,
        // which nobody looks at, kept sixteen thousand. A shader that brought
        // anything gets at least a thirty-second of the room.
        const size_t floor_weight = all_triangles / 32;
        size_t all_weight = 0;
        std::vector<size_t> weight(effects.size(), 0);
        for (uint32_t shader = 0; shader < effects.size(); ++shader) {
            if (triangles[shader] == 0) continue;
            const auto tuned = plan.effect_weight.find(CarEffectName(effects[shader]));
            const float scale = tuned != plan.effect_weight.end() ? tuned->second : 1.0f;
            weight[shader] =
                static_cast<size_t>((double(triangles[shader]) + double(floor_weight)) *
                                    double(scale > 0.0f ? scale : 0.01f));
            if (weight[shader] == 0) weight[shader] = 1;
            all_weight += weight[shader];
        }
        // The budget is per LOD, and LOD 1 was being handed the same as LOD 0 --
        // the whole mesh twice. The donor ships both at one size because its own
        // mesh is small enough not to care; a mod's is not. A quarter is still
        // four times the donor's entire body, and nothing that far away shows
        // the difference.
        //
        // LOD 2 keeps the buffers it already had, and it has to.
        //
        // It looks wrong on paper: the police Caprice's body_lod_2 is ONE model
        // of three submeshes, 92 vertices and 52 triangles in total, 44 of them
        // on the paint, so a 31,844-triangle shell welded into it comes out as
        // 28 triangles with a median edge of a metre. Letting it grow at an
        // eighth of the budget fixed that on its own terms -- 6431 triangles,
        // 10 cm edges, 448 KB -- and broke the game: the car came back as slabs
        // and the city with it.
        //
        // The pair that says so is body_lod_2 and nothing else. Run 993 drew
        // this car whole at a four-megabyte budget with LOD 2 at its shipped
        // 53,248 bytes; run 997, same budget, same 3,555,328-byte LOD 0 and
        // 1,114,112-byte LOD 1, with LOD 2 grown to 458,752, was in pieces.
        // Whatever the far LOD of a vehicle is allowed to be, it is not that.
        const uint32_t full = REXCVAR_GET(model_mods_car_budget);
        const uint32_t budget = lod == 0 ? full : full / 4;
        const uint32_t room = budget > shipped_size ? budget - shipped_size : 0;
        // What each shader is actually being given, in the unit the ceiling is
        // in. The weight is per triangle and the room is bytes, so a share only
        // means something once the stride is beside it -- 12 bytes a vertex on
        // BlackMatte against 28 on everything else, measured, which is nearly a
        // factor of two in what the same triangle count costs.
        //
        // The order matters as much as the share, and that is the part with no
        // sign of itself in the game: a ceiling is cumulative, so the FIRST pass
        // is handed its own slice and nothing more, while the last may use
        // whatever every pass before it left. Moving a material from a late
        // shader to an early one cuts its allowance even at the same weight.
        // Both numbers are printed here so the next car mod can be read off the
        // log instead of off the screen.
        if (lod == 0) {
            std::vector<uint32_t> stride_of;
            ShaderVertexStrides(resource, stride_of);
            size_t running = 0;
            for (uint32_t shader = 0; shader < effects.size(); ++shader) {
                if (triangles[shader] == 0) continue;
                running += weight[shader];
                const uint32_t ceiling =
                    all_weight == 0 ? 0
                                    : shipped_size + static_cast<uint32_t>(
                                                         uint64_t(room) * running / all_weight);
                LARECOMP_APP_INFO("[mods]   budget {} : {} tris, stride {}, {:.1f}% of the room, "
                                  "ceiling {} bytes", CarEffectName(effects[shader]),
                                  triangles[shader],
                                  shader < stride_of.size() ? stride_of[shader] : 0,
                                  all_weight == 0 ? 0.0
                                                  : 100.0 * double(weight[shader]) /
                                                        double(all_weight),
                                  ceiling);
            }
        }
        size_t dealt = 0;
        for (uint32_t shader = 0; shader < effects.size(); ++shader) {
            Mesh part = ExtractShader(body_mesh, shader_of, fallback, shader);
            if (part.vertices.empty() || part.indices.size() < 3) continue;

            // The mod's own picture for this shader, and the UVs that reach it.
            //
            // Everything one MCLA car shader draws samples one image, and a mod
            // arrives with one image per material -- leather, wood, dial, badge,
            // lamp. Packing the ones that landed on this shader into a grid and
            // sending each part's UVs to its own cell is what lets a single
            // texture slot carry all of them, and it is the same answer the
            // character and wheel paths already give.
            //
            // Only for a shader that samples anything: see the note on
            // pack_materials for why a paint or glass shader must keep the UVs
            // it came with.
            if (want_car_textures && shader < pack_materials.size() &&
                pack_materials[shader].writable) {
                Image shader_atlas;
                uint32_t cells = 0;
                std::string atlas_error;
                if (BuildMeshAtlas(part, kAtlasCell, shader_atlas, atlas_error, &cells)) {
                    if (lod == 0) {
                        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} -> {}: {} image(s) in a "
                                          "{}x{} atlas", vehicle.mod_name, car,
                                          CarEffectName(effects[shader]),
                                          pack_materials[shader].diffuse_name, cells,
                                          shader_atlas.width, shader_atlas.height);
                        atlas_of[shader] = std::move(shader_atlas);
                    }
                } else if (lod == 0) {
                    // Not an error: a shader may legitimately have drawn only
                    // materials the mod gave no picture to. The donor's own
                    // texture then stays, which is the old behaviour.
                    LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} keeps the donor's {}: {}",
                                      vehicle.mod_name, car, CarEffectName(effects[shader]),
                                      pack_materials[shader].diffuse_name, atlas_error);
                }
            }

            MeshOffset offset;
            offset.pre_fitted = true;
            // Flood the band, do not carry it over. Tried the other way and it
            // is worse in game: holes through the doors, a grey shell, panels
            // lit as if they were something else.
            //
            // The reason is the one already written on the wheel path -- the
            // second UV set selects which material band a vertex belongs to,
            // and scattering bands by proximity across a shape that does not
            // share them lights it in patches. A car body fitted into the
            // donor's box looked like the exception, because the donor is a
            // saloon of the same size; it is not. Panel boundaries do not line
            // up, so "the nearest shipped vertex" crosses from a door to a
            // window often enough to break the surface.
            //
            // So the colour blocks -- red roof, red glass -- are NOT this. They
            // survive with either setting.
            offset.uniform_shade = true;
            offset.submeshes = true;
            offset.decimate = true;
            offset.allow_untextured = true;
            // The pass owns one shader, and with only_shader set the candidate
            // list holds that shader's slots and nothing else -- so the slots it
            // does not fill are the donor's leftovers within that shader, and
            // clearing them cannot touch a slot another pass is about to use.
            // That is where the police light bar and siren were surviving on the
            // roof of the replacement: silencing a whole unclaimed shader was
            // never enough, because CarLight was claimed and only partly filled.
            offset.keep_unused = false;
            offset.uniform_uv =
                std::find(plan.flat_uv.begin(), plan.flat_uv.end(),
                          CarEffectName(effects[shader])) != plan.flat_uv.end();
            offset.only_shader = static_cast<int32_t>(shader);
            offset.force_shader = static_cast<int32_t>(shader);
            // The model arrives in car space, so it only goes into models that
            // are in car space too. See MeshOffset::car_space_only.
            offset.car_space_only = REXCVAR_GET(model_mods_car_space_only);
            offset.block_align = VirtualBlockSize(resource);
            // LOD 0 and 1 hold the mesh whole; the donor ships them as the same
            // file, so that is what it meant by them. LOD 2 is welded down into
            // the buffers it already had -- see the note above the budget for
            // what happens when it is not.
            offset.grow_buffers = lod <= 1;
            dealt += weight[shader];
            offset.grow_ceiling =
                budget == 0 || all_weight == 0
                    ? 0
                    : shipped_size + static_cast<uint32_t>(uint64_t(room) * dealt / all_weight);
            // A pass is not the last one, and only the last buffer in the
            // segment needs to stop short of its end -- so the slack that
            // matters is added once, after every shader has had its turn,
            // rather than seven times over in the middle of the resource where
            // it is nothing but dead space.
            offset.grow_slack = 4096;

            RewriteStats stats;
            std::string shader_error;
            if (!RewriteDrawableGeometry(resource, part, 0, offset, shader_error, &stats)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{} body_lod_{} {}: {}", vehicle.mod_name,
                                   car, lod, CarEffectName(effects[shader]), shader_error);
                continue;
            }
            claimed[shader] = true;
            ++filled;
            if (lod == 0) {
                LARECOMP_APP_INFO("[mods]   {} <- {} of {} tris in {} submesh(es){}{}",
                                  CarEffectName(effects[shader]), stats.triangles,
                                  part.indices.size() / 3, stats.submeshes,
                                  stats.decimated ? ", decimated" : "",
                                  stats.local_silenced
                                      ? fmt::format(", {} bone-local submesh(es) silenced",
                                                    stats.local_silenced)
                                      : std::string());
            }
        }

        // A shader the mod brought no material for would otherwise keep drawing
        // the donor's own geometry through the replacement -- a police light bar
        // standing on a BMW.
        for (uint32_t shader = 0; shader < effects.size(); ++shader) {
            if (claimed[shader]) continue;
            // Some of the donor's own geometry is worth keeping: its licence
            // plate is a four-vertex quad in the right place on any car of the
            // same shape, and no mod is going to bring one.
            const char* const name = CarEffectName(effects[shader]);
            if (std::find(plan.kept.begin(), plan.kept.end(), name) != plan.kept.end()) {
                const bool move = plan.plate[0] != 0.0f || plan.plate[1] != 0.0f ||
                                  plan.plate[2] != 0.0f;
                size_t moved = 0;
                if (move) {
                    moved = TranslateShaderGeometry(resource, static_cast<int32_t>(shader),
                                                    plan.plate[0], plan.plate[1], plan.plate[2]);
                }
                if (lod == 0) {
                    if (move) {
                        LARECOMP_APP_INFO("[mods]   {} kept, {} submesh(es) moved by "
                                          "({} {} {})", name, moved, plan.plate[0],
                                          plan.plate[1], plan.plate[2]);
                    } else {
                        LARECOMP_APP_INFO("[mods]   {} kept as the donor drew it", name);
                    }
                }
                continue;
            }
            const size_t gone = SilenceShaderGeometry(resource, static_cast<int32_t>(shader));
            if (gone && lod == 0) {
                LARECOMP_APP_INFO("[mods]   {} silenced ({} submesh(es) the mod has no material "
                                  "for)", name, gone);
            }
        }
        if (filled == 0) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: body_lod_{} took no geometry at all",
                               vehicle.mod_name, car, lod);
            continue;
        }

        // The end of a resource does not arrive intact, so whichever buffer
        // ended up last has to stop short of it. See PadResourceTail.
        uint32_t tail = 0;
        {
            std::string pad_error;
            if (!PadResourceTail(resource, kResourceTailSlack, pad_error, &tail) ||
                !RoundVirtualToBlock(resource, pad_error)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: body_lod_{}: {}", vehicle.mod_name,
                                   car, lod, pad_error);
            }
        }

        std::vector<uint8_t> file;
        uint32_t flag = 0;
        if (!BuildRsc5File(resource, file, flag, error)) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: body_lod_{}: {}", vehicle.mod_name, car,
                               lod, error);
            continue;
        }
        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: body_lod_{} is {} bytes ({} on disk), "
                          "{} free after the last buffer", vehicle.mod_name, car, lod,
                          resource.virtual_size, file.size(), tail);
        writer.Add(car_dir + "/" + name, std::move(file), flag, resource.type);
        built.push_back(name);
        ++written;
    }

    // ---------------------------------------------------------------------
    // The parts a car sheds.
    //
    // Everything above writes the shell. A modifiable car is not only a shell:
    // the same folder holds bumper_f0_lod_N.xrsc, interior0, steer_whl0 and
    // forty more, each its own drawable, and those are the pieces MCLA can
    // knock off, open, or let the player swap. A donor that ships them is what
    // makes them reachable -- the police Caprice ships two, a modifiable car
    // ships sixty.
    //
    // They all index the SAME material pack as the body, which is what makes
    // this worth doing at all. Measured on the Impala: interior0_lod_0 draws
    // with shaders 3..22, and those include the cabin's real textures --
    // tmp_plasticbump_c, tmp_carpet_c, perf_leather_c, tmp_leather_c,
    // tmp_cloth_c, every one of them 256 square -- none of which appears
    // anywhere in the body. bumper_r0_lod_0 is where LicensePlate lives, so a
    // plate follows the bumper it is bolted to rather than having to be moved.
    //
    // No budget games here. A part is small -- the Impala's front bumper is
    // 1731 vertices against the body's three thousand-odd -- and the ceiling is
    // a plain multiple of what the donor shipped, so a replacement may be a good
    // deal heavier than the original and still nothing like a body.
    for (const PartMapping& mapping : plan.slots) {
        if (plan.verbatim || mapping.slot == "body" || mapping.groups.empty()) continue;
        if (mapping.slot == kVehicleBodySlot) continue;

        Mesh source = ExtractGroups(mesh, mapping.groups);
        if (source.vertices.empty() || source.indices.size() < 3) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: slot {} names no geometry in the model",
                               vehicle.mod_name, car, mapping.slot);
            continue;
        }

        // A part is its own drawable and renders with its own handful of the
        // donor's shaders -- the impala's steer_whl0 is not going to draw with
        // CarPaintCustomizable -- so the set of shaders that have geometry is
        // read off THIS slot rather than off the body.
        std::vector<uint32_t> part_drawn;
        {
            Rsc5Resource first;
            std::string part_error;
            if (ExtractTemplate(archive, donor_dir + "/" + mapping.slot + "_lod_0.xrsc", first,
                                part_error)) {
                ShaderVertexStrides(first, part_drawn);
            }
        }

        int32_t part_fallback = -1;
        const std::map<std::string, uint32_t> part_shader_of =
            ResolveShaderMap(plan, effects, part_drawn, vehicle.mod_name, car, part_fallback,
                             mapping.slot);

        size_t lods = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const std::string name = mapping.slot + "_lod_" + std::to_string(lod) + ".xrsc";
            Rsc5Resource resource;
            // A part need not have every LOD -- the donor's own hood has no LOD
            // two -- so a missing template is the file simply not existing
            // rather than a failure.
            if (!ExtractTemplate(archive, donor_dir + "/" + name, resource, error)) continue;

            // A part does NOT live in the car's space, and this is where that
            // was got wrong first. Measured on the Impala: steer_whl0_lod_0 is a
            // disc on the ORIGIN (x -0.220..0.220, y -0.220..0.220,
            // z -0.078..0.025) and bumper_f0_lod_0 is centred there too --
            // every one of them is authored relative to the bone that places
            // it, not to the car. Only seats_f0 is in car space, because its
            // bone sits at the origin.
            //
            // So the piece cut out of the model, which IS in car space, has to
            // be moved onto the template's own box before it is written. The
            // scale is already applied -- PlaceCar ran over the whole model
            // before any of this -- so only the translation is wanted, which is
            // what a scale of one asks PlacePart for.
            //
            // Shipping it pre-fitted instead is what leaves a steering wheel a
            // metre from the driver's hands.
            float slot_min[3], slot_max[3];
            if (!ReadDrawableBounds(resource, slot_min, slot_max)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {} states no bounds to place on",
                                   vehicle.mod_name, car, name);
                continue;
            }
            // Two kinds of slot, told apart by where the donor's own part sits.
            //
            // A part authored in its OWN space is centred on its bone, so its
            // box straddles the origin on every axis -- measured on the impala,
            // steer_whl0 is x +-0.22, y +-0.22, z -0.078..0.025 and bumper_f0
            // is x +-0.939, y -0.218..0.277, z -0.388..0.333. Those are
            // re-centred onto the slot, which is what puts a steering wheel in
            // the driver's hands.
            //
            // A part authored in CAR space is not: interior0 is x +-0.971,
            // y 0.235..1.414, z -1.136..2.018, the same frame as body_lod_0,
            // and seats_f0 sits wholly on the driver's side. For those the
            // model is already where it belongs -- PlaceCar put it there, in
            // the donor's frame, measured off the axles -- and re-centring it
            // onto the donor's box slides the BMW's cabin to wherever the
            // Chevrolet's cabin happened to be centred, which on this pair is
            // above and behind the BMW's own body.
            const bool own_space = slot_min[0] < 0.0f && slot_max[0] > 0.0f &&
                                   slot_min[1] < 0.0f && slot_max[1] > 0.0f &&
                                   slot_min[2] < 0.0f && slot_max[2] > 0.0f;
            Mesh placed = source;
            if (own_space) PlacePart(placed, 0.0f, 1.0f, slot_min, slot_max);
            if (lod == 0) {
                LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} is authored in {} space, box "
                                  "({:.2f} {:.2f} {:.2f}) to ({:.2f} {:.2f} {:.2f})",
                                  vehicle.mod_name, car, name, own_space ? "its own" : "car",
                                  slot_min[0], slot_min[1], slot_min[2], slot_max[0],
                                  slot_max[1], slot_max[2]);
            }

            // The same coarser page the body gets, and for the same reason: a
            // part slot that takes a whole cabin holds buffers of hundreds of
            // kilobytes, and a block of 131072 cannot hold one.
            {
                std::string shift_error;
                if (!SetVirtualPageShift(resource, kCarPageShift, shift_error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car,
                                       name, shift_error);
                }
            }

            const uint32_t shipped_size = resource.virtual_size;
            const uint32_t ceiling = shipped_size * REXCVAR_GET(model_mods_part_growth);
            size_t filled = 0;
            std::vector<bool> claimed(effects.size(), false);

            for (uint32_t shader = 0; shader < effects.size(); ++shader) {
                Mesh part = ExtractShader(placed, part_shader_of, part_fallback, shader);
                if (part.vertices.empty() || part.indices.size() < 3) continue;

                if (want_car_textures && shader < pack_materials.size() &&
                    pack_materials[shader].writable) {
                    Image shader_atlas;
                    uint32_t cells = 0;
                    std::string atlas_error;
                    if (BuildMeshAtlas(part, kAtlasCell, shader_atlas, atlas_error, &cells) &&
                        lod == 0) {
                        // The pack holds one picture per shader for the whole
                        // car, so the first drawable to claim a shader is the
                        // one whose atlas it gets. With a shader mapped in only
                        // one drawable -- which is what the per-slot rules are
                        // for -- that never comes up; say so when it does,
                        // because the answer is a parts.txt change.
                        if (atlas_of.count(shader)) {
                            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {} and an earlier drawable "
                                               "both draw with {}, which stores one picture -- "
                                               "give one of them a shader of its own",
                                               vehicle.mod_name, car, name,
                                               CarEffectName(effects[shader]));
                        } else {
                            LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} -> {}: {} image(s) in a "
                                              "{}x{} atlas", vehicle.mod_name, car,
                                              CarEffectName(effects[shader]),
                                              pack_materials[shader].diffuse_name, cells,
                                              shader_atlas.width, shader_atlas.height);
                            atlas_of[shader] = std::move(shader_atlas);
                        }
                    }
                }

                MeshOffset offset;
                offset.pre_fitted = true;
                offset.uniform_shade = true;
                offset.submeshes = true;
                offset.decimate = true;
                offset.allow_untextured = true;
                offset.keep_unused = false;
                offset.only_shader = static_cast<int32_t>(shader);
                offset.force_shader = static_cast<int32_t>(shader);
                // A car-space slot has car-space models that need the same guard
                // as the body; an own-space slot is ALL bone-local by definition,
                // and the part has already been moved into that frame.
                offset.car_space_only = !own_space && REXCVAR_GET(model_mods_car_space_only);
                offset.block_align = VirtualBlockSize(resource);
                offset.grow_buffers = lod <= 1;
                offset.grow_ceiling = ceiling;
                offset.grow_slack = 4096;

                RewriteStats stats;
                std::string shader_error;
                if (!RewriteDrawableGeometry(resource, part, 0, offset, shader_error, &stats)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{} {} {}: {}", vehicle.mod_name, car,
                                       name, CarEffectName(effects[shader]), shader_error);
                    continue;
                }
                claimed[shader] = true;
                ++filled;
                if (lod == 0) {
                    LARECOMP_APP_INFO("[mods]   {} {} <- {} of {} tris in {} submesh(es){}{}",
                                      name, CarEffectName(effects[shader]), stats.triangles,
                                      part.indices.size() / 3, stats.submeshes,
                                      stats.decimated ? ", decimated" : "",
                                      stats.local_silenced
                                          ? fmt::format(", {} bone-local submesh(es) silenced",
                                                        stats.local_silenced)
                                          : std::string());
                }
            }

            if (filled == 0) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {} took no geometry, shipped as the "
                                   "donor holds it", vehicle.mod_name, car, name);
                continue;
            }

            // Whatever the mod brought no material for keeps drawing the donor's
            // part through the replacement, exactly as on the body.
            for (uint32_t shader = 0; shader < effects.size(); ++shader) {
                if (claimed[shader]) continue;
                const char* const effect = CarEffectName(effects[shader]);
                if (std::find(plan.kept.begin(), plan.kept.end(), effect) != plan.kept.end())
                    continue;
                const size_t gone = SilenceShaderGeometry(resource, static_cast<int32_t>(shader));
                if (gone && lod == 0) {
                    LARECOMP_APP_INFO("[mods]   {} {} silenced ({} submesh(es))", name, effect,
                                      gone);
                }
            }

            // The same tail pad the body gets, and for the same reason: this is
            // the path that shipped interior0_lod_0 with its last buffer 4,452
            // bytes from the end and put 10,138 triangles of paint on screen as
            // slabs. See PadResourceTail.
            uint32_t tail = 0;
            {
                std::string pad_error;
                if (!PadResourceTail(resource, kResourceTailSlack, pad_error, &tail) ||
                    !RoundVirtualToBlock(resource, pad_error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car,
                                       name, pad_error);
                }
            }

            std::vector<uint8_t> file;
            uint32_t flag = 0;
            if (!BuildRsc5File(resource, file, flag, error)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car, name,
                                   error);
                continue;
            }
            LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} is {} bytes (was {}), {} free after the "
                              "last buffer", vehicle.mod_name, car, name, resource.virtual_size,
                              shipped_size, tail);
            writer.Add(car_dir + "/" + name, std::move(file), flag, resource.type);
            built.push_back(name);
            ++written;
            ++lods;
        }

        if (lods == 0) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: donor {} ships no {} to build on",
                               vehicle.mod_name, car, donor, mapping.slot);
        }
    }

    // Everything else the donor's folder holds, under the new car's name.
    //
    // The archive files children by hash and keeps no strings, so a child is
    // matched against every name a car folder can have. What matches is written
    // under that name -- with the donor's own name swapped for this car's in the
    // four files named after it -- and what does not is written under the hash
    // it already had, which addresses it exactly as the original was addressed.
    std::vector<Rpf3Entry> children;
    if (!archive.ListDirectory(donor_dir, children)) {
        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot list the donor's folder",
                           vehicle.mod_name, car);
        return written;
    }

    const std::map<uint32_t, std::string> donor_names = CarFolderNames(donor);
    for (const Rpf3Entry& child : children) {
        if (child.is_directory()) continue;

        const auto known = donor_names.find(child.hash);
        std::string name = known != donor_names.end() ? known->second : std::string();
        if (!name.empty() &&
            std::find(built.begin(), built.end(), name) != built.end()) {
            continue;
        }

        std::vector<uint8_t> raw;
        if (!archive.ReadFile(child, raw) || raw.empty()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot read donor entry {:#010x}",
                               vehicle.mod_name, car, child.hash);
            continue;
        }

        // The tune is the one file whose contents name the car, so it is the one
        // rewritten rather than copied. BuildRsc5File picks its shape: stored LZX
        // for a single frame or less, uncompressed past that.
        const bool is_tune = plan.rename_tune && !name.empty() && name == donor + ".xct";
        // The two material packs, which is where a car's textures live. The
        // drawable has none of its own: its shader group is thirty-two byte
        // objects picked by vtable with nothing embedded, and the pack beside
        // it holds both the dictionary and the materials that read it. They
        // come in a pair, the same five-or-fifty pictures at two sizes -- .xtl
        // at half of .xtp -- and both are written, because the game picks
        // between them by distance and writing one leaves the car changing
        // skin as you drive away from it.
        const bool is_pack = !atlas_of.empty() && REXCVAR_GET(model_mods_car_pack_write) &&
                             !name.empty() &&
                             (name == donor + ".xtl" || name == donor + ".xtp");
        // A slot the mod ships empty: the manifest still promises it, so the
        // resource has to load, and it simply draws nothing.
        bool silence = false;
        for (const std::string& slot : plan.silenced) {
            for (int lod = 0; lod < 3 && !silence; ++lod) {
                if (name == slot + "_lod_" + std::to_string(lod) + ".xrsc") silence = true;
            }
            if (silence) break;
        }

        std::string target = name.empty() ? std::string() : name;
        if (!target.empty() && target.rfind(donor + ".", 0) == 0) {
            target = car + target.substr(donor.size());
        }

        // A part the mod does not want is left out of the archive entirely.
        //
        // Two ways of shipping it empty were tried first and both brought the
        // loader down on "Invalid fixup, address is neither virtual nor
        // physical", with an address that differed between runs of the same
        // build -- uninitialised memory. Emptying every geometry leaves their
        // vertex and index buffer pointers for mcCarDrawableChunk's placer to
        // follow; zeroing the model count above them, which changed exactly one
        // byte of the donor's file, failed the same way. The placer wants a LOD
        // with something in it.
        //
        // Absence is what the game itself uses: the donor's own hood has a LOD
        // zero and one and no LOD two at all, and the loader opens every part
        // path unconditionally -- sub_82130008 is a stub returning one, not a
        // test -- so a part that is not there simply leaves a null pointer.
        if (silence) {
            LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} left out, the mod ships its own",
                              vehicle.mod_name, car, target);
            continue;
        }

        if (is_tune || is_pack) {
            Rsc5Resource resource;
            size_t payload_offset = 0, payload_size = 0;
            if (!ParseRsc5Header(raw, resource, payload_offset, payload_size, error)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car,
                                   target, error);
                continue;
            }
            const size_t total =
                static_cast<size_t>(resource.virtual_size) + resource.physical_size;
            if (!LzxDecompress(raw.data() + payload_offset, payload_size, resource.data, total)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot unpack {}", vehicle.mod_name,
                                   car, target);
                continue;
            }

            if (is_pack) {
                // One atlas per shader, over the colour map that shader reads.
                //
                // The two packs are written from the same atlases and each
                // resamples them to whatever it stores, so nothing here has to
                // know that .xtl is the half-size copy.
                //
                // A texture shared between materials is written once, by the
                // first shader that claims it. That is not a case to work
                // around quietly: on a rich donor several InteriorTrim
                // materials point at the same tmp_leather_c, so two of the
                // mod's shaders can be asking for the same bytes, and the
                // second would silently overwrite the first. It is named in the
                // log instead, because the answer is a parts.txt change -- pick
                // a different "Effect#N" -- and not something this can guess.
                std::vector<uint32_t> claimed;
                size_t replaced = 0;
                for (const auto& entry : atlas_of) {
                    const uint32_t shader = entry.first;
                    if (shader >= pack_materials.size()) continue;
                    const uint32_t texture = pack_materials[shader].diffuse;

                    if (std::find(claimed.begin(), claimed.end(), texture) != claimed.end()) {
                        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: shader {} ({}) shares {} "
                                           "with a shader already written -- name the material "
                                           "'<effect>#N' in parts.txt to pick another",
                                           vehicle.mod_name, car, target, shader,
                                           CarEffectName(effects[shader]),
                                           pack_materials[shader].diffuse_name);
                        continue;
                    }
                    claimed.push_back(texture);

                    TextureStats written_texture;
                    if (!ReplacePackMaterialDiffuse(resource, shader, entry.second, error,
                                                    &written_texture)) {
                        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: shader {}: {}",
                                           vehicle.mod_name, car, target, shader, error);
                        continue;
                    }
                    ++replaced;
                    LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {}: {} -> {} {}x{}, {} mip level(s)",
                                      vehicle.mod_name, car, target,
                                      CarEffectName(effects[shader]), written_texture.name,
                                      written_texture.width, written_texture.height,
                                      written_texture.levels);
                }

                if (replaced == 0) {
                    // Nothing was written, so the donor's file travels as it
                    // came rather than being repacked for no reason.
                    writer.Add(car_dir + "/" + target, std::move(raw), child.flag,
                               child.resource_type());
                    ++written;
                    continue;
                }

                std::vector<uint8_t> file;
                uint32_t flag = 0;
                if (!BuildRsc5File(resource, file, flag, error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car,
                                       target, error);
                    continue;
                }
                writer.Add(car_dir + "/" + target, std::move(file), flag, resource.type);
                ++written;
                continue;
            }

            if (is_tune) {
                const size_t renamed = RenameInResource(resource.data, donor, car);
                std::vector<uint8_t> file;
                uint32_t flag = 0;
                if (!BuildRsc5File(resource, file, flag, error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, car,
                                       target, error);
                    continue;
                }
                // The shape (stored LZX or uncompressed) is BuildRsc5File's call,
                // made on the payload size; see the comment there.
                LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} renamed {} string(s) from {}, "
                                  "{} bytes packed", vehicle.mod_name, car, target, renamed,
                                  donor, resource.virtual_size);
                writer.Add(car_dir + "/" + target, std::move(file), flag, resource.type);
            }
            ++written;
            continue;
        }

        // Everything else travels exactly as the shipped archive holds it.
        if (target.empty()) {
            char label[16] = {};
            std::snprintf(label, sizeof label, "%08X", child.hash);
            writer.AddHashed(car_dir + "/" + label, child.hash, std::move(raw), child.flag,
                             child.resource_type());
        } else {
            writer.Add(car_dir + "/" + target, std::move(raw), child.flag,
                       child.resource_type());
        }
        ++written;
    }

    LARECOMP_APP_INFO("[mods] {} -> {}: {} file(s) from donor {}", vehicle.mod_name, car, written,
                      donor);
    return written;
}

// Builds every car part replacement into `writer`, and reports how many landed.
size_t BuildVehicleMods(const std::vector<VehicleMod>& vehicles, const Rpf3Reader& archive,
                        const std::filesystem::path& cache_dir, bool passthrough,
                        Rpf3Writer& writer) {
    size_t built = 0;

    for (const VehicleMod& vehicle : vehicles) {
        std::error_code ec;
        std::filesystem::path source;
        for (const auto& file : std::filesystem::directory_iterator(vehicle.folder, ec)) {
            if (ec) break;
            if (file.is_regular_file() && IsMeshFile(file.path())) {
                source = file.path();
                break;
            }
        }

        std::string error;
        const std::vector<PartMapping> mappings =
            ReadPartsFile(vehicle.folder / kVehiclePartsFile, error);
        if (mappings.empty()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}", vehicle.mod_name, vehicle.car,
                               error.empty() ? "parts.txt maps no slots" : error);
            continue;
        }

        // A parts.txt naming a donor is not asking for a car to be replaced. It
        // is asking for one to exist, built out of a car that already does --
        // a different job with a different shape, so it takes its own path.
        const VehiclePlan plan = ReadVehiclePlan(mappings);

        Mesh mesh;
        if (!plan.verbatim) {
            if (source.empty()) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: no .glb, .gltf or .obj in the folder",
                                   vehicle.mod_name, vehicle.car);
                continue;
            }
            if (!LoadMeshFile(source, mesh, error)) {
                LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}", vehicle.mod_name, vehicle.car,
                                   error);
                continue;
            }
            const size_t sidecar =
                LoadSidecarTextures(vehicle.folder / kTextureFolder, mesh);
            if (sidecar != 0) {
                LARECOMP_APP_INFO("[mods] {}/vehicles/{}: {} material(s) textured from {}/",
                                  vehicle.mod_name, vehicle.car, sidecar, kTextureFolder);
            }
        }

        if (!plan.donor.empty()) {
            // The WHOLE model goes in, not just the body's groups. A car the
            // game does not ship is several drawables -- the shell, each
            // bumper, the cabin, the wheel in front of the driver -- and they
            // have to be placed by ONE transform or they arrive at different
            // sizes. Cutting the body out here left the rest of the model
            // behind before anything could ask for it.
            built += BuildDonorCar(vehicle, plan, std::move(mesh), archive, writer);
            continue;
        }

        // One scale for the whole car, taken along its length: the body slot of
        // the game's car against the same groups in the mod. Every part is then
        // moved by that factor and no other, which is what keeps them fitting
        // each other once they are back on the vehicle.
        const auto body = std::find_if(mappings.begin(), mappings.end(),
                                       [](const PartMapping& m) {
                                           return m.slot == kVehicleBodySlot;
                                       });
        if (body == mappings.end()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: parts.txt has no '{}' line, and without it "
                               "there is nothing to size the car against",
                               vehicle.mod_name, vehicle.car, kVehicleBodySlot);
            continue;
        }

        Rsc5Resource body_template;
        const std::string body_path = "resources/vehicle/" + vehicle.car + "/" +
                                      kVehicleBodySlot + "_lod_0.xrsc";
        if (!ExtractTemplate(archive, body_path, body_template, error)) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name, vehicle.car,
                               body_path, error);
            continue;
        }
        float car_min[3], car_max[3], src_min[3], src_max[3];
        if (!ReadDrawableBounds(body_template, car_min, car_max) ||
            !GroupBounds(mesh, body->groups, src_min, src_max)) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: cannot measure the car body",
                               vehicle.mod_name, vehicle.car);
            continue;
        }

        const float yaw = static_cast<float>(REXCVAR_GET(model_mods_car_yaw));
        const float source_length = src_max[2] - src_min[2];
        if (source_length <= 1e-4f) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: the body groups have no length",
                               vehicle.mod_name, vehicle.car);
            continue;
        }
        const float scale = (car_max[2] - car_min[2]) / source_length *
                            static_cast<float>(REXCVAR_GET(model_mods_car_scale));
        LARECOMP_APP_INFO("[mods] {}/vehicles/{}: car is {:.2f} long against the game's {:.2f}, "
                          "scale {:.3f}", vehicle.mod_name, vehicle.car, source_length,
                          car_max[2] - car_min[2], scale);

        const bool verbatim = REXCVAR_GET(model_mods_car_verbatim);
        if (verbatim) {
            LARECOMP_APP_INFO("[mods] {}/vehicles/{}: verbatim copy, the mesh is ignored",
                              vehicle.mod_name, vehicle.car);
        }

        size_t slots = 0;
        for (const PartMapping& mapping : mappings) {
            // Both LODs of the slot get the same mesh; the low one simply has
            // less room and is welded down harder.
            for (int lod = 0; lod < 2; ++lod) {
                const std::string archive_path = "resources/vehicle/" + vehicle.car + "/" +
                                                 mapping.slot + "_lod_" + std::to_string(lod) +
                                                 ".xrsc";
                const std::filesystem::path cache_file =
                    cache_dir / kVehicleFolder / vehicle.car /
                    (mapping.slot + "_lod_" + std::to_string(lod) + ".tpl");

                if (verbatim) {
                    // Straight out of the shipped archive and straight into
                    // ours: never decompressed, so nothing about the resource
                    // can be blamed on this code.
                    Rpf3Entry entry;
                    std::vector<uint8_t> raw;
                    if (!archive.Find(archive_path, entry) || !archive.ReadFile(entry, raw)) {
                        if (lod == 0) {
                            LARECOMP_APP_ERROR("[mods] {}/vehicles/{} {}: cannot copy the shipped "
                                               "file", vehicle.mod_name, vehicle.car,
                                               mapping.slot);
                        }
                        continue;
                    }
                    writer.Add(archive_path, std::move(raw), entry.flag, entry.resource_type());
                    ++built;
                    if (lod == 0) ++slots;
                    continue;
                }

                Rsc5Resource resource;
                if (!LoadTemplateCache(cache_file, resource)) {
                    if (!ExtractTemplate(archive, archive_path, resource, error)) {
                        // Not every slot has a second LOD, and a mod naming a
                        // slot this car does not have is worth saying once.
                        if (lod == 0) {
                            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: {}", vehicle.mod_name,
                                               vehicle.car, mapping.slot, error);
                        }
                        continue;
                    }
                    SaveTemplateCache(cache_file, resource);
                }

                float slot_min[3], slot_max[3];
                if (!ReadDrawableBounds(resource, slot_min, slot_max)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: cannot measure the slot",
                                       vehicle.mod_name, vehicle.car, mapping.slot);
                    continue;
                }

                Mesh part = ExtractGroups(mesh, mapping.groups);
                if (part.vertices.empty() || part.indices.empty()) {
                    if (lod == 0) {
                        LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}: none of its groups are in "
                                           "the model", vehicle.mod_name, vehicle.car,
                                           mapping.slot);
                    }
                    continue;
                }
                const size_t part_tris = part.indices.size() / 3;
                PlacePart(part, yaw, scale, slot_min, slot_max);

                // A part that comes out far bigger than the slot it went into is
                // almost always a mapping mistake rather than a modelling one.
                // Files that carry alternatives park them off to one side --
                // this BMW keeps a second pair of tail light frames at x=+1.68
                // and x=-1.41 on a car 1.8 wide -- and naming a parked left with
                // a mounted right gives a slot whose two halves are three metres
                // apart. It still writes; it just comes out stretched across the
                // car, which is worth a line in the log rather than a silent
                // wrong result. Only checked on the high LOD, since both LODs
                // get the same mesh.
                if (lod == 0) {
                    float part_min[3], part_max[3];
                    part.Bounds(part_min, part_max);
                    for (int c = 0; c < 3; ++c) {
                        const float slot_size = slot_max[c] - slot_min[c];
                        const float part_size = part_max[c] - part_min[c];
                        if (slot_size <= 1e-4f || part_size <= slot_size * 1.75f) continue;
                        LARECOMP_APP_ERROR(
                            "[mods] {}/vehicles/{} {}: the mapped group(s) span {:.2f} on axis {} "
                            "against the slot's {:.2f} -- check parts.txt for a variant that sits "
                            "away from the car",
                            vehicle.mod_name, vehicle.car, mapping.slot, part_size, c, slot_size);
                        break;
                    }
                }

                MeshOffset offset;
                offset.pre_fitted = true;
                offset.uniform_shade = true;
                offset.whole_models = true;
                offset.grow_buffers = REXCVAR_GET(model_mods_grow);
                offset.grow_probe = REXCVAR_GET(model_mods_grow_probe);
                offset.grow_slack = REXCVAR_GET(model_mods_grow_slack);
                offset.decimate = REXCVAR_GET(model_mods_decimate);
                offset.submeshes = REXCVAR_GET(model_mods_submeshes);
                offset.diagnose = REXCVAR_GET(model_mods_diag);

                RewriteStats stats;
                if (!passthrough &&
                    !RewriteDrawableGeometry(resource, part, 0, offset, error, &stats)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{} {}: {}", vehicle.mod_name,
                                       vehicle.car, mapping.slot, error);
                    continue;
                }
                WriteDiagnostics(cache_dir, vehicle.mod_name,
                                 vehicle.car + "_" + mapping.slot + "_lod" +
                                     std::to_string(lod),
                                 stats);

                std::string pad_error;
                if (!passthrough && !PadResourceTail(resource, kResourceTailSlack, pad_error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{} {}: {}", vehicle.mod_name,
                                       vehicle.car, mapping.slot, pad_error);
                }

                std::vector<uint8_t> file;
                uint32_t flag = 0;
                if (!BuildRsc5File(resource, file, flag, error)) {
                    LARECOMP_APP_ERROR("[mods] {}/vehicles/{} {}: {}", vehicle.mod_name,
                                       vehicle.car, mapping.slot, error);
                    continue;
                }

                writer.Add(archive_path, std::move(file), flag, resource.type);
                ++built;
                if (lod == 0) {
                    ++slots;
                    LARECOMP_APP_INFO("[mods]   {} <- {} ({} tris -> {}{})", mapping.slot,
                                      mapping.groups.front(), part_tris, stats.triangles,
                                      stats.decimated ? ", decimated" : "");
                }
            }
        }
        LARECOMP_APP_INFO("[mods] {} -> {}: {} slot(s) replaced", vehicle.mod_name, vehicle.car,
                          slots);
    }
    return built;
}

// Builds every wheel replacement into `writer`, and reports how many were built.
//
// A wheel is a simpler asset than a character: one LOD, no variants, no skin.
// The mesh rides bone 0 rigidly -- the skeleton is a single bone called "root"
// sitting at the identity, which is the wheel's own hub.
//
// It is two resources, though. A wheel's textures are not in its drawable at
// all; they live in the sibling <name>.xtp, and both are rewritten here and
// added to the archive together. A mod that brings no image of its own gets
// only the mesh, and keeps the shipped wheel's paint.
size_t BuildRimMods(const std::vector<ModEntry>& mods, const Rpf3Reader& archive,
                    const std::filesystem::path& cache_dir, bool passthrough, Rpf3Writer& writer) {
    // A mesh named "all" stands in for every wheel; a mesh named after one wheel
    // still wins for that wheel.
    std::vector<ModEntry> build_list;
    std::vector<const ModEntry*> wildcards;
    for (const ModEntry& mod : mods) {
        std::string stem = mod.asset;
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem == kEveryAsset) {
            wildcards.push_back(&mod);
        } else {
            build_list.push_back(mod);
        }
    }
    for (const ModEntry* wildcard : wildcards) {
        size_t added = 0;
        for (const char* asset : kRimAssets) {
            const bool taken = std::any_of(build_list.begin(), build_list.end(),
                                           [&](const ModEntry& e) { return e.asset == asset; });
            if (taken) continue;
            Rpf3Entry entry;
            if (!archive.Find(std::string("resources/rims/") + asset + "/body_lod_0.xrsc", entry))
                continue;
            build_list.push_back(ModEntry{asset, wildcard->obj, wildcard->mod_name});
            ++added;
        }
        LARECOMP_APP_INFO("[mods] {}: standing in for {} wheel(s)", wildcard->mod_name, added);
    }

    size_t built = 0;
    for (const ModEntry& mod : build_list) {
        Mesh mesh;
        std::string error;
        if (!LoadMeshFile(mod.obj, mesh, error)) {
            LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}", mod.mod_name, mod.asset, error);
            continue;
        }
        if (LoadSidecarTextures(mod.obj.parent_path() / kTextureFolder, mesh) != 0) {
            LARECOMP_APP_INFO("[mods] {}/rims/{}: textured from {}/", mod.mod_name, mod.asset,
                              kTextureFolder);
        }

        // The atlas is built before the rewrite, as it is for a character: it
        // remaps the UVs per primitive, and the rewrite decimates and reorders
        // vertices, after which the primitive boundaries that remap needs are
        // gone.
        Image atlas;
        uint32_t atlas_cells = 0;
        const bool want_textures = REXCVAR_GET(model_mods_textures) && !passthrough;
        if (want_textures && !mesh.images.empty()) {
            std::string atlas_error;
            if (!BuildMeshAtlas(mesh, kAtlasCell, atlas, atlas_error, &atlas_cells)) {
                LARECOMP_APP_ERROR("[mods] {}/rims/{}: {} -- keeping the original paint",
                                   mod.mod_name, mod.asset, atlas_error);
                atlas = Image{};
            }
        }

        // Where the template is read from, and where the result lands. They are
        // the same name unless the mod named a donor.
        const std::string source = mod.donor.empty() ? mod.asset : mod.donor;
        const std::string source_path = "resources/rims/" + source + "/body_lod_0.xrsc";
        const std::string archive_path = "resources/rims/" + mod.asset + "/body_lod_0.xrsc";
        const std::filesystem::path cache_file = cache_dir / kRimFolder / (source + ".tpl");

        Rsc5Resource resource;
        if (!LoadTemplateCache(cache_file, resource)) {
            if (!ExtractTemplate(archive, source_path, resource, error)) {
                LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}: {}", mod.mod_name, mod.asset,
                                   source_path, error);
                continue;
            }
            SaveTemplateCache(cache_file, resource);
        }

        // The texture pack comes first, because it decides something the
        // geometry rewrite needs: which of the wheel's materials samples a
        // texture. The wheel body ships on the one that does not.
        const std::string texture_source = "resources/rims/" + source + "/" + source + ".xtp";
        const std::string texture_path = "resources/rims/" + mod.asset + "/" + mod.asset + ".xtp";
        const std::filesystem::path texture_cache =
            cache_dir / kRimFolder / (source + ".xtp.tpl");

        Rsc5Resource textures;
        bool have_textures = false;
        if (!atlas.empty()) {
            have_textures = LoadTemplateCache(texture_cache, textures);
            if (!have_textures && ExtractTemplate(archive, texture_source, textures, error)) {
                SaveTemplateCache(texture_cache, textures);
                have_textures = true;
            }
            if (!have_textures) {
                LARECOMP_APP_ERROR("[mods] {}/rims/{}: no texture pack: {}", mod.mod_name,
                                   mod.asset, error);
            }
        }

        MeshOffset offset;
        if (REXCVAR_GET(model_mods_rim_autoalign)) {
            AlignRimToAxle(mesh, offset);
            offset.align_axle = true;  // the depth half of it, done against the template
        }
        // Where the mod's image belongs, and which submesh must survive to
        // carry it. Both answers come from the texture pack, so neither can be
        // worked out until it has been read.
        bool whole_rim = false;
        bool sampled = false;
        uint32_t textured_shader = 0;
        if (have_textures && TexturePackShader(textures, textured_shader)) {
            sampled = true;
            if (REXCVAR_GET(model_mods_rim_keep_badge))
                offset.reserve_shader = static_cast<int32_t>(textured_shader);

            const int32_t mode = REXCVAR_GET(model_mods_rim_texture_mode);
            whole_rim = mode < 0 ? ImageIsOpaque(atlas, atlas_cells, kAtlasCell) : mode != 0;
            if (whole_rim) offset.force_shader = static_cast<int32_t>(textured_shader);
        }
        offset.uniform_shade = !REXCVAR_GET(model_mods_rim_inherit_shade);
        offset.shade_occlusion =
            offset.uniform_shade && REXCVAR_GET(model_mods_rim_shade_occlusion);
        // The statistical guess only runs when the measurement is not: they
        // write the same lane, and the one that describes the actual mesh wins.
        offset.shade_profile = offset.uniform_shade && !offset.shade_occlusion &&
                               REXCVAR_GET(model_mods_rim_shade_profile);
        offset.grow_buffers = REXCVAR_GET(model_mods_grow);
        offset.x = static_cast<float>(REXCVAR_GET(model_mods_rim_offset_x));
        offset.yaw += static_cast<float>(REXCVAR_GET(model_mods_rim_yaw));
        offset.pitch += static_cast<float>(REXCVAR_GET(model_mods_rim_pitch));
        offset.roll += static_cast<float>(REXCVAR_GET(model_mods_rim_roll));
        offset.scale = static_cast<float>(REXCVAR_GET(model_mods_rim_scale));
        offset.decimate = REXCVAR_GET(model_mods_decimate);
        offset.submeshes = REXCVAR_GET(model_mods_submeshes);
        offset.diagnose = REXCVAR_GET(model_mods_diag);
        offset.grow_probe = REXCVAR_GET(model_mods_grow_probe);
        offset.grow_slack = REXCVAR_GET(model_mods_grow_slack);

        RewriteStats stats;
        // Bone 0 explicitly: a wheel's skeleton is one bone, so there is nothing
        // to retarget onto, and asking for the automatic path would put a skinned
        // model through a landmark search that cannot describe it.
        if (!passthrough &&
            !RewriteDrawableGeometry(resource, mesh, 0, offset, error, &stats)) {
            LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}", mod.mod_name, mod.asset, error);
            continue;
        }
        WriteDiagnostics(cache_dir, mod.mod_name, "rim_" + mod.asset, stats);

        std::string rim_pad_error;
        if (!passthrough && !PadResourceTail(resource, kResourceTailSlack, rim_pad_error)) {
            LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}", mod.mod_name, mod.asset, rim_pad_error);
        }

        std::vector<uint8_t> file;
        uint32_t flag = 0;
        if (!BuildRsc5File(resource, file, flag, error)) {
            LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}", mod.mod_name, mod.asset, error);
            continue;
        }

        writer.Add(archive_path, std::move(file), flag, resource.type);
        ++built;

        // The paint, which is a resource of its own.
        std::string paint;
        if (have_textures) {
            TextureStats texture;
            if (!ReplaceDictionaryTexture(textures, atlas, error, &texture)) {
                LARECOMP_APP_ERROR("[mods] {}/rims/{}: paint not replaced: {}", mod.mod_name,
                                   mod.asset, error);
            } else {
                std::vector<uint8_t> texture_file;
                uint32_t texture_flag = 0;
                if (!BuildRsc5File(textures, texture_file, texture_flag, error)) {
                    LARECOMP_APP_ERROR("[mods] {}/rims/{}: paint not packed: {}", mod.mod_name,
                                       mod.asset, error);
                } else {
                    writer.Add(texture_path, std::move(texture_file), texture_flag, textures.type);
                    paint = ", " + texture.name + " " + std::to_string(texture.width) + "x" +
                            std::to_string(texture.height) + " + " +
                            std::to_string(texture.levels) + " mip(s) replaced";
                    paint += !sampled
                                 ? ", but no material admits to sampling it -- it will not show"
                             : whole_rim ? " across the whole rim (material " +
                                               std::to_string(textured_shader) + ")"
                                         : " on the hub cap";
                    if (sampled && offset.reserve_shader < 0) paint += ", badge silenced";
                }
            }
        }

        // A wheel built from a donor needs the donor's paint under its own name
        // as well, unless the mod brought an image and the block above already
        // wrote one. Without it the game asks for a texture pack that is not
        // there and the rim arrives unpainted.
        if (!mod.donor.empty() && paint.empty()) {
            Rpf3Entry entry;
            std::vector<uint8_t> raw;
            if (archive.Find(texture_source, entry) && archive.ReadFile(entry, raw)) {
                writer.Add(texture_path, std::move(raw), entry.flag, entry.resource_type());
                ++built;
                paint = ", paint copied from " + mod.donor;
            }
        }

        // The shade lane is reported because it is the whole of the wheel's
        // lighting and it is invisible from a screenshot: a flat lane and a
        // baked one look the same until the wheel turns. A deviation of zero
        // means the bake did not run.
        std::string shade;
        if (stats.shade_deviation >= 0.0f) {
            shade = fmt::format(", shade {}..{} mean {:.0f} deviation {:.0f}", stats.shade_low,
                                stats.shade_high, stats.shade_mean, stats.shade_deviation);
        } else if (offset.shade_profile) {
            shade = ", shade profiled";
        } else if (offset.uniform_shade) {
            shade = ", shade flat";
        }
        LARECOMP_APP_INFO("[mods] {} -> rim {} ({} tris in {} submesh(es){}{}{})", mod.mod_name,
                          mod.asset, stats.triangles, stats.submeshes,
                          stats.decimated ? ", decimated" : "", paint, shade);
    }
    return built;
}

}  // namespace

void AppendModArchiveTo(uint32_t buffer, size_t capacity) {
    if (!g_mod_archive_ready || capacity == 0) return;

    uint8_t* list = GuestPointer(buffer);
    if (!list) return;

    // The engine's copy is bounded but not guaranteed to terminate, so find the
    // terminator ourselves and give up if there is none.
    size_t length = 0;
    while (length < capacity && list[length] != 0) ++length;
    if (length >= capacity) return;

    const std::string current(reinterpret_cast<const char*>(list), length);
    if (current.find(kModArchiveName) != std::string::npos) return;

    const std::string suffix = std::string(";game:/") + kModArchiveName;
    if (length + suffix.size() + 1 > capacity) {
        LARECOMP_APP_ERROR("[mods] archive list is full, {} will not be mounted",
                           kModArchiveName);
        return;
    }

    std::memcpy(list + length, suffix.data(), suffix.size());
    list[length + suffix.size()] = 0;

    LARECOMP_APP_INFO("[mods] archive list: {}",
                      reinterpret_cast<const char*>(list));
}

void Init() {
    // Called from OnPostSetup (through the boot progress overlay) and again
    // from InitHooks, which is where it used to live. Building twice would
    // rewrite the archive under a game that has already been told to mount it.
    static bool built = false;
    if (built) return;
    built = true;

    g_mod_archive_ready = false;

    if (!REXCVAR_GET(model_mods)) return;

    const std::filesystem::path exe_dir = ExeDir();
    const std::filesystem::path models_dir = exe_dir / "models";

    std::error_code ec;
    if (!std::filesystem::is_directory(models_dir, ec)) return;

    auto* runtime = rex::Runtime::instance();
    if (!runtime) {
        LARECOMP_APP_ERROR("[mods] no runtime, model replacement disabled");
        return;
    }

    const std::filesystem::path game_root = runtime->game_data_root();

    std::vector<ModEntry> mods, rim_mods;
    std::vector<VehicleMod> car_mods;
    std::vector<RawFile> raw_files;
    ScanMods(models_dir, mods, rim_mods, car_mods, raw_files);

    // Native custom music builds its three files (the .dat pair and one bank per
    // track) into a cache folder and rides in as raw files, so it inherits the
    // dedup, the resource sniffing and the logging below for free.
    for (music::GeneratedFile& generated : music::Build(exe_dir, game_root)) {
        raw_files.push_back(RawFile{std::move(generated.archive_path),
                                    std::move(generated.source), "music"});
    }

    if (mods.empty() && rim_mods.empty() && car_mods.empty() && raw_files.empty()) {
        std::filesystem::remove(game_root / kModArchiveName, ec);
        return;
    }
    const bool has_meshes = !mods.empty() || !rim_mods.empty() || !car_mods.empty();

    // A mesh mod needs the shipped archive as its template and xcompress32.dll to
    // read it. A raw file needs neither -- it is already the finished bytes -- so
    // neither of those is allowed to sink a mod that only carries files.
    const std::filesystem::path source_archive = game_root / kSourceArchiveName;
    if (!std::filesystem::exists(source_archive, ec)) {
        LARECOMP_APP_ERROR("[mods] {} not found, {}", source_archive.string(),
                           has_meshes ? "model replacement disabled" : "meshes unavailable");
        if (has_meshes) return;
    }

    if (has_meshes && !XCompressAvailable(exe_dir)) {
        LARECOMP_APP_ERROR(
            "[mods] xcompress32.dll missing next to the executable -- it is needed to read the "
            "original models. {} mod(s) skipped.",
            mods.size() + rim_mods.size() + car_mods.size());
        return;
    }

    Rpf3Reader archive;
    if (has_meshes && !archive.Open(source_archive)) {
        LARECOMP_APP_ERROR("[mods] cannot open {}", source_archive.string());
        return;
    }

    // A mesh named "all" replaces every character there is, which is the way out
    // of having to know which resource a given racer, car or bike will ask for.
    // A mesh named after one character still wins for that character, so one can
    // be picked out of the crowd.
    std::vector<ModEntry> build_list;
    std::vector<const ModEntry*> wildcards;
    for (const ModEntry& mod : mods) {
        std::string stem = mod.asset;
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem == kEveryAsset) {
            wildcards.push_back(&mod);
        } else {
            build_list.push_back(mod);
        }
    }
    // Probing the archive costs a few hundred milliseconds, so it happens once
    // and only when something actually asks for every character.
    const std::vector<std::string> every_character =
        wildcards.empty() ? std::vector<std::string>{} : DiscoverCharacterAssets(archive);
    for (const ModEntry* wildcard : wildcards) {
        size_t added = 0;
        for (const std::string& asset : every_character) {
            const bool taken = std::any_of(build_list.begin(), build_list.end(),
                                           [&](const ModEntry& e) { return e.asset == asset; });
            if (taken) continue;
            build_list.push_back(ModEntry{asset, wildcard->obj, wildcard->mod_name});
            ++added;
        }
        LARECOMP_APP_INFO("[mods] {}: standing in for {} character(s)", wildcard->mod_name, added);
    }

    Rpf3Writer writer;
    const std::filesystem::path cache_dir = models_dir / ".cache";

    // Rpf3Writer::Write refuses an archive that names the same path twice, and
    // it refuses the whole thing rather than the offending entry -- so a second
    // mod shipping the same file would take every other mod down with it. Drop
    // the repeat here instead. Paths are compared lowercased because RageHash
    // folds case and the archive would collide even when the strings differ.
    //
    // The same trap is still open between a raw file and a mesh mod: name a
    // file after a resource some mesh also builds and nothing gets built at
    // all. Nothing checks for that, because a mod has no reason to do it.
    std::vector<std::string> taken_paths;
    if (!raw_files.empty()) boot::BeginPhase("Files", static_cast<int>(raw_files.size()));
    for (const RawFile& file : raw_files) {
        boot::Step(file.archive_path);
        std::string key = file.archive_path;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(taken_paths.begin(), taken_paths.end(), key) != taken_paths.end()) {
            LARECOMP_APP_ERROR("[mods] {}/files/{}: already shipped by another mod, skipped",
                               file.mod_name, file.archive_path);
            continue;
        }

        std::error_code read_ec;
        const auto size = std::filesystem::file_size(file.source, read_ec);
        if (read_ec || size == 0 || size > 0x3FFFFFFFull) {
            LARECOMP_APP_ERROR("[mods] {}/files/{}: cannot read", file.mod_name,
                               file.archive_path);
            continue;
        }

        // Only a resource has to be read: its header has to be inspected and its
        // segments may have to be repacked. A plain file goes in byte for byte,
        // so it is left where it is and streamed at Write time -- a music folder
        // is hundreds of multi-megabyte banks, and holding them all in memory at
        // once is what used to make a big folder die on boot.
        uint8_t head[kRsc5HeaderSize] = {};
        {
            std::ifstream input(file.source, std::ios::binary);
            if (!input) {
                LARECOMP_APP_ERROR("[mods] {}/files/{}: cannot read", file.mod_name,
                                   file.archive_path);
                continue;
            }
            input.read(reinterpret_cast<char*>(head),
                       static_cast<std::streamsize>(std::min<uint64_t>(size, sizeof(head))));
        }
        const bool is_resource_file =
            size > kRsc5HeaderSize &&
            (LoadBE32(head) == kLarcMagic || LoadBE32(head) == kRsc5Magic);

        if (!is_resource_file) {
            writer.AddFromFile(file.archive_path, file.source, size,
                               static_cast<uint32_t>(size), 0);
            taken_paths.push_back(std::move(key));
            LARECOMP_APP_INFO("[mods] {}: {} ({} bytes, verbatim)", file.mod_name,
                              file.archive_path, size);
            continue;
        }

        std::vector<uint8_t> bytes;
        {
            std::ifstream input(file.source, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
        }
        if (bytes.size() != static_cast<size_t>(size)) {
            LARECOMP_APP_ERROR("[mods] {}/files/{}: cannot read", file.mod_name,
                               file.archive_path);
            continue;
        }

        // Plain file or resource?
        //
        // A plain file goes in with the flag word equal to its size and nothing
        // else, which is the shape every uncompressed file in the shipped
        // archive has. A resource cannot: the engine reads its segment sizes out
        // of that same word, so it has to carry the real flag and its type.
        //
        // Both arrive here as a file starting with the RSC5 magic, and the
        // compression bit says which is which:
        //
        //   bit30 set   - a resource straight out of an archive, header and LZX
        //                 stream intact. It is already exactly what the engine
        //                 streams, so it goes through untouched.
        //   bit30 clear - segments the mod built itself, behind a 16-byte header
        //                 that only exists to name the type and flag. Those are
        //                 packed here into a real RSC5 file.
        //
        // The segments cannot simply be stored as they are. Every resource in
        // the shipped archives is an RSC5 container -- header, XCompress marker,
        // LZX stream -- and the streamer reads it as one; handing it bare
        // segments instead loads nothing, which is a black showroom picture
        // rather than an error.
        uint32_t flag = static_cast<uint32_t>(bytes.size());
        uint32_t resource_type = 0;

        if (bytes.size() > kRsc5HeaderSize && LoadBE32(bytes.data()) == kLarcMagic) {
            resource_type = LoadBE32(bytes.data() + 4);
            flag = LoadBE32(bytes.data() + 8);
            bytes.erase(bytes.begin(), bytes.begin() + kRsc5HeaderSize);

            // An uncompressed resource IS readable -- sub_821BC140 has a branch
            // for it that skips the inflater and reads each destination chunk
            // straight out of the file -- but only in a precise shape: the
            // payload at offset twelve and the entry exactly 12 + virtual +
            // physical bytes long, because pgStreamer::Read measures an
            // uncompressed request against the entry's own size. This path
            // ships what it is handed, unchanged, so it cannot produce that
            // shape. BuildRsc5File can, and the branch below routes through it.
            if ((flag & 0xC0000000u) == 0x80000000u) {
                LARECOMP_APP_ERROR("[mods] {}/files/{}: flag {:#010x} marks an uncompressed "
                                   "resource, which this path ships unchanged and so cannot "
                                   "lay out -- ship the bare segments behind an RSC5 header "
                                   "instead and they will be packed",
                                   file.mod_name, file.archive_path, flag);
                continue;
            }
        } else if (bytes.size() > kRsc5HeaderSize && LoadBE32(bytes.data()) == kRsc5Magic) {
            resource_type = LoadBE32(bytes.data() + 4);
            flag = LoadBE32(bytes.data() + 8);

            if ((flag & 0x40000000u) == 0) {
                Rsc5Resource resource;
                resource.type = resource_type;
                resource.flag = flag;
                resource.virtual_size = (flag & 0x7FFu) << (((flag >> 11) & 0xFu) + 8);
                resource.physical_size = ((flag >> 15) & 0x7FFu) << (((flag >> 26) & 0xFu) + 8);

                const size_t expected = kRsc5HeaderSize +
                                        static_cast<size_t>(resource.virtual_size) +
                                        resource.physical_size;
                if (bytes.size() != expected) {
                    LARECOMP_APP_ERROR("[mods] {}/files/{}: flag {:#010x} wants {} bytes of "
                                       "segments, the file carries {}",
                                       file.mod_name, file.archive_path, flag,
                                       expected - kRsc5HeaderSize,
                                       bytes.size() - kRsc5HeaderSize);
                    continue;
                }

                resource.data.assign(bytes.begin() + kRsc5HeaderSize, bytes.end());
                std::vector<uint8_t> packed;
                std::string pack_error;
                if (!BuildRsc5File(resource, packed, flag, pack_error)) {
                    LARECOMP_APP_ERROR("[mods] {}/files/{}: cannot pack resource: {}",
                                       file.mod_name, file.archive_path, pack_error);
                    continue;
                }
                bytes = std::move(packed);
            }
        }

        const size_t stored = bytes.size();
        writer.Add(file.archive_path, std::move(bytes), flag, resource_type);
        taken_paths.push_back(std::move(key));
        if (resource_type) {
            LARECOMP_APP_INFO("[mods] {}: {} ({} bytes, resource type {}, flag {:#010x}, {})",
                              file.mod_name, file.archive_path, stored, resource_type, flag,
                              (flag & 0x40000000u) ? "compressed" : "uncompressed");
        } else {
            LARECOMP_APP_INFO("[mods] {}: {} ({} bytes, verbatim)", file.mod_name,
                              file.archive_path, stored);
        }
    }

    if (!raw_files.empty()) boot::EndPhase();

    const bool passthrough = REXCVAR_GET(model_mods_passthrough);
    if (passthrough) {
        LARECOMP_APP_INFO("[mods] passthrough is on: repacking the original geometry, "
                          ".obj meshes are ignored");
    }

    if (!build_list.empty()) boot::BeginPhase("Models", static_cast<int>(build_list.size()));
    for (const auto& mod : build_list) {
        boot::Step(mod.asset);
        Mesh mesh;
        std::string error;
        if (!LoadMeshFile(mod.obj, mesh, error)) {
            LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, mod.asset, error);
            continue;
        }
        if (LoadSidecarTextures(mod.obj.parent_path() / kTextureFolder, mesh) != 0) {
            LARECOMP_APP_INFO("[mods] {}/{}: textured from {}/", mod.mod_name, mod.asset,
                              kTextureFolder);
        }
        const size_t triangles = mesh.indices.size() / 3;

        // One atlas per model, built before the mesh is handed to the rewrite:
        // it remaps the UVs, and the rewrite decimates and reorders vertices,
        // after which the primitive boundaries the remap needs are gone.
        Image atlas;
        const bool want_textures = REXCVAR_GET(model_mods_textures) && !passthrough;
        if (want_textures && !mesh.images.empty()) {
            std::string atlas_error;
            if (!BuildMeshAtlas(mesh, kAtlasCell, atlas, atlas_error)) {
                LARECOMP_APP_ERROR("[mods] {}/{}: {} -- keeping the original textures",
                                   mod.mod_name, mod.asset, atlas_error);
                atlas = Image{};
            }
        }

        int built = 0;
        std::string detail;
        for (const std::string& variant : AssetVariants(mod.asset)) {
            const std::string archive_path =
                "resources/character/" + variant + "/" + variant + ".xrsc";
            const std::filesystem::path cache_file = cache_dir / (variant + ".tpl");

            Rsc5Resource resource;
            if (!LoadTemplateCache(cache_file, resource)) {
                // Only the base asset is required; the variants are best effort.
                if (!ExtractTemplate(archive, archive_path, resource, error)) {
                    if (variant == mod.asset)
                        LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, variant, error);
                    continue;
                }
                SaveTemplateCache(cache_file, resource);
            }

            RewriteStats stats;
            const int32_t configured_bone = REXCVAR_GET(model_mods_bone);
            const uint32_t bone = configured_bone == -2  ? kSlotOneTest
                                  : configured_bone < 0 ? kAutomaticBone
                                                        : static_cast<uint32_t>(configured_bone);
            MeshOffset offset;
            offset.x = static_cast<float>(REXCVAR_GET(model_mods_offset_x));
            offset.y = static_cast<float>(REXCVAR_GET(model_mods_offset_y));
            offset.z = static_cast<float>(REXCVAR_GET(model_mods_offset_z));
            offset.yaw = static_cast<float>(REXCVAR_GET(model_mods_yaw));
            offset.pitch = static_cast<float>(REXCVAR_GET(model_mods_pitch));
            offset.roll = static_cast<float>(REXCVAR_GET(model_mods_roll));
            offset.scale = static_cast<float>(REXCVAR_GET(model_mods_scale));
            offset.proportions = static_cast<float>(REXCVAR_GET(model_mods_proportions));
            offset.decimate = REXCVAR_GET(model_mods_decimate);
            offset.submeshes = REXCVAR_GET(model_mods_submeshes);
            offset.weight_smoothing = REXCVAR_GET(model_mods_weight_smoothing);
            offset.anchor_bones = REXCVAR_GET(model_mods_anchor_bones);
            offset.grow_buffers = REXCVAR_GET(model_mods_grow);
            offset.grow_probe = REXCVAR_GET(model_mods_grow_probe);
            offset.grow_slack = REXCVAR_GET(model_mods_grow_slack);
            offset.diagnose = REXCVAR_GET(model_mods_diag);
            if (!passthrough &&
                !RewriteDrawableGeometry(resource, mesh, bone, offset, error, &stats)) {
                LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, variant, error);
                continue;
            }
            WriteDiagnostics(cache_dir, mod.mod_name, variant, stats);
            if (!atlas.empty() && stats.shader != 0xFFFFFFFFu) {
                TextureStats texture;
                if (ReplaceShaderDiffuse(resource, stats.shader, atlas, error, &texture)) {
                    if (variant == mod.asset) {
                        detail += (detail.empty() ? "" : ", ") + texture.name + " " +
                                  std::to_string(texture.width) + "x" +
                                  std::to_string(texture.height) + " + " +
                                  std::to_string(texture.levels) + " mip(s) replaced on shader " +
                                  std::to_string(stats.shader);
                    }
                } else {
                    LARECOMP_APP_ERROR("[mods] {}/{}: texture not replaced: {}", mod.mod_name,
                                       variant, error);
                }
            }
            if (variant == mod.asset && stats.bones == 0) {
                // Worth saying out loud: the model is in, but nothing will move
                // it. A glTF can carry a skeleton and still leave JOINTS_0 off
                // its primitives, and then there is nothing to skin with.
                detail += (detail.empty() ? "" : ", ") +
                          std::string("no per-vertex weights, riding one bone rigidly");
            }
            if (variant == mod.asset && stats.bones > 0) {
                detail += (detail.empty() ? "" : ", ") + std::to_string(stats.bones) +
                          " bone(s) skinned, heaviest maps to bone " +
                          std::to_string(stats.first_bone);
            }
            if (stats.decimated) {
                detail += (detail.empty() ? "" : ", ") + variant + " decimated to " +
                          std::to_string(stats.triangles) + " tris";
            }
            if (variant == mod.asset && stats.submeshes > 1) {
                detail += (detail.empty() ? "" : ", ") + std::to_string(stats.submeshes) +
                          " submeshes used";
            }

            std::string pad_error;
            if (!passthrough && !PadResourceTail(resource, kResourceTailSlack, pad_error)) {
                LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, variant, pad_error);
            }

            std::vector<uint8_t> file;
            uint32_t flag = 0;
            if (!BuildRsc5File(resource, file, flag, error)) {
                LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, variant, error);
                continue;
            }

            writer.Add(archive_path, std::move(file), flag, resource.type);
            ++built;
        }

        if (built > 0) {
            LARECOMP_APP_INFO("[mods] {} -> {} ({} tris, {} variant(s)){}{}", mod.mod_name,
                              mod.asset, triangles, built, detail.empty() ? "" : " -- ", detail);
        }
    }

    if (!build_list.empty()) boot::EndPhase();

    if (!rim_mods.empty())
        BuildRimMods(rim_mods, archive, cache_dir, passthrough, writer);
    if (!car_mods.empty())
        BuildVehicleMods(car_mods, archive, cache_dir, passthrough, writer);

    const std::filesystem::path mod_archive = game_root / kModArchiveName;
    boot::BeginPhase("Archive", 1);

    if (writer.empty()) {
        // Leaving a stale archive behind would silently keep an old mod alive.
        std::filesystem::remove(mod_archive, ec);
        LARECOMP_APP_ERROR("[mods] nothing built, model replacement disabled");
        boot::EndPhase();
        return;
    }

    if (!writer.Write(mod_archive)) {
        LARECOMP_APP_ERROR("[mods] cannot write {}", mod_archive.string());
        boot::EndPhase();
        return;
    }
    boot::Step(kModArchiveName);
    boot::EndPhase();

    g_mod_archive_ready = true;
    LARECOMP_APP_INFO("[mods] {} with {} replacement(s), mounted last", kModArchiveName,
                      writer.size());
}

}  // namespace mc::modloader
