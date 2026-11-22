#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class ModsSettings final : public rex::SettingOwner {
 public:
  ModsSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, model_mods, true, "Load model replacements from <exe>/modloader/<mod>/<asset>.obj (or mods/, "
    "models/). Each .obj is "
    "baked into a native drawable and packed into xarchive_mods.rpf, which is "
    "appended to the archive list -- the engine's own last-mounted-wins rule "
    "makes it override per file. Game archives are never modified.", rex::kRequiresRestart);

  REX_SETTING(bool, model_mods_anchor_bones, true, "Whether the repose plants each of the mod's joints on the bone it was "
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
    "better; in motion is what only the game can answer.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(i32, model_mods_bone, -1, "-1 keeps a skinned mesh's own weights (glTF with JOINTS_0/WEIGHTS_0), "
    "matching its joints to the driver's bones by bind-pose position, so the "
    "model follows the seated pose. Any value >= 0 ignores the weights and "
    "rides the whole mesh rigidly on that one bone, which is all a .obj can do.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_cabin_light, true, "Give a replacement car's cabin (the car-space part slots, interior0) the "
    "donor's per-vertex interior lighting, read off the donor's cabin vertex "
    "nearest each new one, instead of one flooded average.\n"
    "\n"
    "The colour word of a cabin vertex is how much of each of the car's three "
    "interior lights (siColor0..2: the interior neon among them) reaches it. "
    "Flooded with one average the BMW's cabin barely answered the interior neon "
    "being switched on. Off restores the flood.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(u32, model_mods_car_budget, 16777216, "The largest a replaced car body may be grown to, in bytes for LOD 0; the "
    "lower LODs get a quarter of it. Anything the mod brings beyond what fits "
    "is welded away, whatever model_mods_decimate says. It is a ceiling, not a "
    "size: a body that needs less is grown only to what it needs.\n"
    "\n"
    "Growing a drawable has no natural limit -- the mod hands over whatever it "
    "modelled and the resource follows -- but the game has one in practice. "
    "Counted over the 8849 vehicle drawables the game ships, the largest "
    "declares 1,163,264 bytes and the median 45,056; counted over body_lod_0 "
    "alone the largest is 884,736 and the median 266,240. The default used to "
    "sit just under that largest, so a mod could be as heavy as the heaviest "
    "thing the engine already streams and no heavier.\n"
    "\n"
    "It cannot any more. Each large buffer now starts on a block boundary and "
    "the padding counts against the ceiling; the shader passes are dealt their "
    "share in order, so by the last ones the padding has eaten the room: at "
    "4,194,304 the BMW came back with Chrome at 4 of 7353 triangles, CarLight "
    "at 124 of 2660 and CarGlass at 112 of 830. At 16,777,216 every pass "
    "arrives whole, a 7,340,032-byte body_lod_0, and the car drives "
    "correctly.\n"
    "\n"
    "0 lifts the ceiling entirely, which is the setting to try when you want to "
    "know whether size is what a car is failing on.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_car_lod1_copy, true, "Ship a replacement car's LOD 1 as a byte copy of the LOD 0 just built, for "
    "the body and for every part slot the donor gives a LOD 1.\n"
    "\n"
    "The garage draws LOD 1 close up. Measured on the BMW e38: the crumpled "
    "bonnet, the V cut into the plate and the bent grille in the garage are "
    "body_lod_1 exactly -- rasterised offline, LOD 0 is smooth and LOD 1 is that "
    "picture. LOD 1 was rewritten on a quarter of the budget, and a quarter "
    "buys 12,075 of the paint's 21,706 triangles through a clustering "
    "decimator, which is what crumples a panel. interior0's LOD 1 was worse: "
    "the donor has no paint there at all, so 10,138 triangles of shell were "
    "simply missing from it.\n"
    "\n"
    "A copy is what the game itself does for the Caprice -- its body_lod_0 and "
    "body_lod_1 are the same file, md5 and all -- and here it costs nothing: "
    "block rounding already makes both LODs of the body 2 MB.\n"
    "\n"
    "OFF rebuilds LOD 1 from the donor's own LOD 1 on a quarter budget, the "
    "old behaviour.", rex::kDebugOnly);

  REX_SETTING(bool, model_mods_car_pack_write, true, "Write the mod's atlases into the car's material packs."
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
    "pack rewrite is at fault; in pieces means the UV rewrite is.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_car_scale, 1.0, "Size multiplier for a replacement car, on top of the automatic fit. The "
    "fit matches the length of the mod's body to the length of the body it "
    "replaces, and every part of the car is then moved by that one factor -- "
    "scaling each part into its own slot instead would leave a hood a size "
    "larger than the doors beside it. 1.0 leaves it alone.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_car_space_only, true, "Write a donor car's replacement mesh only into the models the donor authors "
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
    "ground. Read at archive build time, so it needs a restart and no rebuild.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_car_textures, true, "Give a replacement car body its own textures. A car is drawn by several "
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
    "comes back wearing the donor's own artwork.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_car_verbatim, false, "Diagnostic, cars only. Copies each part's shipped file into the mod "
    "archive exactly as it lies in the game's own -- same bytes, same "
    "compressed length, same flag -- instead of decompressing it and packing it "
    "back. Everything else about the mod archive stays as it is, so this "
    "separates the two halves of a failure: if the game is happy with the "
    "verbatim copy, what it dislikes is the repack; if it still refuses, the "
    "repack is innocent and the problem is in serving a vehicle resource from a "
    "second archive at all. `model_mods_passthrough` cannot answer that on its "
    "own, because it still re-encodes the stream -- and an LZX of stored blocks "
    "is larger than the compressed original it replaces.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_car_yaw, 180.0, "Turns a replacement car about the vertical axis before its parts are "
    "placed, in degrees. 180 is the normal value and the default: glTF has a "
    "model face +Z and MCLA has it face -Z, which is checked rather than "
    "assumed -- the shipped cars keep their headlights at negative Z and their "
    "tail lights at positive Z, and an exported car has it the other way round.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_decimate, false, "Whether a replacement mesh larger than the slot it has to live in may be "
    "welded down to fit: a character, a rim, or a part replaced on a shipped "
    "car. A car built on a donor is always welded to its ceilings "
    "(model_mods_car_budget for the body) and ignores this. The slot is fixed "
    "-- for a character, the drawable's largest submesh, around 2300 vertices, "
    "and a few hundred on the low LOD -- and the welding is a blunt instrument: "
    "vertices that share a cell of a spatial grid are merged and averaged, "
    "which on a detailed model rounds off faces and hands. Off, a mesh still "
    "too big is refused with the reason in the log and that variant keeps the "
    "shipped model, so what a mod shows is what it brought; decimate it "
    "yourself in a modelling tool, where you can see what you are giving up. "
    "Expect the low LOD to be refused first, so the original driver reappears "
    "at a distance. On welds it in instead.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_diag, false, "Write a `.diag` folder beside the mods: one .txt per rebuilt asset "
    "holding the template's submesh table, both rigs with the joint-to-bone "
    "mapping, and how the mesh was dealt across the submeshes -- plus the mesh "
    "itself as .obj before and after the retarget. A character that comes out "
    "mangled is one of three things (a joint on the wrong bone, a bad deal, or "
    "a submesh the writer could not describe and therefore left drawing what it "
    "shipped) and they all look alike on screen; this tells them apart.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_glass_band, true, "Give a replacement body's glass the pane number of the donor glass nearest "
    "each piece of it, instead of the dominant pane of the submesh it is written "
    "into. texcoord1.x on CarGlass says which pane a vertex is -- 0 windscreen, "
    "1 rear, 2/3 doors -- and the window film tints 1-3 only, so a body flooded "
    "with 0 takes no film at all. See MeshOffset::band_by_piece. Read at archive "
    "build time.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_grow, false, "Give a replacement wheel or car part buffers of its own instead of welding "
    "it down into the ones the shipped resource came with. The resource is made "
    "longer and the submesh is pointed at the new space; nothing already in it "
    "moves. This is what removes the detail ceiling -- a wheel slot holds 3,701 "
    "vertices and a modern wheel model is fifty times that -- and it was thought "
    "impossible until the streamer's read budget turned out to be what had been "
    "corrupting grown resources. 65535 vertices per submesh remains, because a "
    "submesh counts and indexes its vertices in sixteen bits. Characters are not "
    "affected: a skinned submesh also caps its bone palette, which is a separate "
    "allocation, so they stay on the path that already works. Turn this off to "
    "go back to welding everything into the shipped buffers.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(i32, model_mods_grow_probe, 0, "With `model_mods_grow` on: 1 grows the resource and stops there, the segment "
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
    "is a changed count worth trying.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(u32, model_mods_grow_slack, 65536, "Room left unused at the end of a resource `model_mods_grow` has enlarged. "
    "The tail of a grown segment does not arrive intact, and this is what keeps "
    "the mesh out of it -- measured on a wheel: buffers ending 16 KB short of "
    "the segment's end came back with holes, and ending 82 KB short came back "
    "clean. Where between those two the boundary sits has not been pinned down, "
    "so the default is the distance that was measured to work rather than the "
    "smallest that might. 0 restores the old behaviour and the holes with it.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_offset_x, 0.0, "Sideways nudge for the replacement model, in metres.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_offset_y, 0.0, "Extra height for the replacement model, in metres, on top of the automatic "
    "lift onto its bone. Positive raises it. Use this if the model still sits "
    "below or above the seat -- it takes a restart but no rebuild.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_offset_z, 0.0, "Forward/back nudge for the replacement model, in metres.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(u32, model_mods_part_growth, 8, "How much larger than the donor's own a replacement PART may be, as a "
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
    "setting to try when a car loads without its parts.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_part_probe, false, "TEMPORARY diagnostic: every second, while the player's car is waiting to "
    "reattach its parts (car+6467) -- which is when the game draws none of it, "
    "wheels included -- log which selected part is not resident yet and the "
    "state of its stream (object, resource, handle, flags, state, refcount). "
    "See PartProbeLoop.", rex::kDebugOnly);

  REX_SETTING(bool, model_mods_passthrough, false, "Diagnostic. Repacks the original character resource without touching its "
    "geometry, so the .obj is ignored. If the driver still renders correctly "
    "with this on, the packing path (LZX, archive, mount, flags) is sound and "
    "any breakage belongs to the mesh rewrite; if it breaks, the packing path "
    "is at fault.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_pitch, 0.0, "Tips the replacement model forward or back, in degrees.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_proportions, 0.0, "How much of the replacement model's own build to keep when it is reposed "
    "onto the driver's skeleton, 0 to 1. At 0 every joint lands exactly on the "
    "bone it was matched to, which is what the game's skinning expects and what "
    "a human-shaped model wants. A model built to other proportions gets "
    "stretched to reach those bones -- a cartoon character with a six centimetre "
    "neck has it pulled out to the driver's twenty-six -- and raising this "
    "keeps its own bone lengths instead, taking only the pose from the "
    "skeleton. The cost is that limbs no longer end where the game thinks they "
    "do, so hands drift off the wheel as it goes up. Try 1 for anything that is "
    "not roughly human.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_autoalign, true, "Put a replacement wheel on the game's axle, in two steps. First the turn: "
    "every shipped wheel spins about X and is half as wide as it is tall, so the "
    "thinnest axis of the model is taken to be its axle and rotated onto that "
    "one -- a wheel exported lying flat comes in standing up without anyone "
    "having to work out which ninety degrees it needed. Then the depth: a wheel "
    "is not centred in its own bounding box, it keeps its hub and spokes in the "
    "negative half of the axle with only a thin lip at the far edge, so a bare "
    "rim left centred sits sunk inside the tyre. Its face is pushed out flush "
    "with the shipped wheel's instead, turning the model around first if it came "
    "in facing inward. The manual rim angles and nudge below are applied on top. "
    "Turn this off to place a wheel entirely by hand.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_band_profile, false, "Carry the template wheel's material bands onto the replacement instead of "
    "flooding one."
    "\n"
    "texcoord1.x is the rim's material selector: xRimMain's VS_Common indexes "
    "materialHookups and tintColors with it, and PS_Multi then reflects a "
    "dual-paraboloid environment map through whatever material came out. A "
    "shipped wheel's body uses FOUR bands, and two unrelated ones put them in "
    "the same places -- band 1 is a ring at 0.82..0.88 of the radius two "
    "thirds of the way in, band 2 the outer lip past 0.93, band 0 the face and "
    "spokes, band 3 scattered accents. Flooding the dominant band gives the "
    "whole rim one material, and one material over a mirror is a mirror ball: "
    "the highlight sweeps round as the wheel turns. The bands are rings, so "
    "they are carried by nearest neighbour in radius and axial depth, each "
    "normalised to its own mesh. Only the band moves -- the colour does not, "
    "which is what separates this from model_mods_rim_inherit_shade."
    "\n"
    "OFF, because it does not beat doing nothing. Validated against ground "
    "truth by predicting one shipped wheel's bands from another's and counting "
    "per-vertex agreement: flooding the dominant band alone already scores 59% "
    "on the skyline and 57% on the bbs, and the transfer scores 61%/51% "
    "matching on absolute depth and 57%/70% matching on each mesh's own depth "
    "span. Neither normalisation wins in both directions, so the bands are in "
    "the same PLACES on every wheel without being predictable per vertex from "
    "where a vertex sits. Kept, off, so the next idea has something to beat.", rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_inherit_shade, false, "Diagnostic. Copy the shipped wheel's per-vertex shading onto the "
    "replacement, vertex by vertex from whichever original vertex is nearest. "
    "This is what a character replacement does, and it works there because the "
    "mesh has been reposed onto the very skeleton it replaces. A wheel shares "
    "nothing with the wheel it replaces, so what arrives is the old wheel's "
    "baked shadows printed onto the new spokes, and its four material bands "
    "scattered by proximity across a shape that does not have them. Off, which "
    "is the default, gives the whole wheel one band and that band's own average "
    "shade, both read off the template.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_keep_badge, true, "Leave the wheel's badge quad as it shipped instead of silencing it.\n"
    "\n"
    "It is nine vertices on its own material, and that material is the only "
    "one on the wheel that samples a texture -- so silencing it, which is what "
    "the geometry rewrite did to every unused submesh, takes the badge off the "
    "car and leaves a mod's paint read by nothing.\n"
    "\n"
    "Off restores the old behaviour. Only mods that bring an image of their own "
    "reach this at all, so a wheel mod with no texture behaves the same either "
    "way -- which makes this the switch to try first when a textured wheel mod "
    "misbehaves and an untextured one on the same build does not.", rex::kDebugOnly);

  REX_SETTING(f64, model_mods_rim_offset_x, 0.0, "Nudge for a replacement wheel along its axle, where one unit is the "
    "wheel's own diameter. Positive pushes it further into the tyre, negative "
    "pulls it out. Only needed when the automatic flush above lands slightly "
    "off for a particular model.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_rim_pitch, 0.0, "Tips a replacement wheel forward or back, in degrees.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_rim_roll, 0.0, "Rolls a replacement wheel sideways, in degrees.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_rim_scale, 1.0, "Size multiplier for a replacement wheel, on top of the automatic fit to "
    "the shipped wheel's own bounds. 1.0 leaves it alone.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_shade_occlusion, true, "Bake the wheel's shade lane from occlusion measured on the replacement."
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
    "wheels -- no better than the flat average it replaces.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_rim_shade_profile, true, "Rebuild a replaced wheel's baked shading from the template instead of "
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
    "average, which is featureless but is not a guess.", rex::kDebugOnly);

  REX_SETTING(i32, model_mods_rim_texture_mode, -1, "Where a wheel mod's own image is applied: -1 auto, 0 the hub cap, 1 the "
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
    "it meant.", rex::kDebugOnly);

  REX_SETTING(f64, model_mods_rim_yaw, 0.0, "Turns a replacement wheel about the vertical axis, in degrees. Separate "
    "from the character angles because a wheel and a driver never want the same "
    "correction.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_roll, 0.0, "Rolls the replacement model sideways, in degrees.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_scale, 1.0, "Size multiplier on top of the automatic fit, which matches the model's "
    "height to the driver's. 1.0 leaves it alone.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_share_images, false, "Let two materials that name the same picture share one cell of the atlas."
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
    "keeps its own cell.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_submeshes, true, "Spread the replacement mesh across every submesh of the character instead "
    "of the largest one alone. The resource cannot be made bigger, but the "
    "driver is fifteen submeshes holding 8208 vertices and 37512 indices "
    "between them, against 2320 and 12957 in the biggest -- so this raises what "
    "a mod can bring by three and a half times, and most characters then arrive "
    "with no welding at all. Every submesh borrowed this way is repointed at "
    "the shader whose textures were replaced. Turn it off to go back to filling "
    "one submesh, which is the conservative path if a model renders oddly.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, model_mods_textures, true, "Use the replacement model's own textures. The images embedded in a .glb "
    "(one per material) are packed into a single atlas, each primitive's UVs "
    "are remapped into its own cell, and the result is written over the "
    "character's diffuse map in place -- same address, same size, same format. "
    "Off leaves the mesh wearing the original driver's skin.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(i32, model_mods_weight_smoothing, 4, "How many rounds of neighbour averaging the mod's skin weights get before "
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
    "a seam it changes nothing.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(f64, model_mods_yaw, 0.0, "Turns the replacement model about the vertical axis, in degrees. Source "
    "models rarely face the same way the game does -- 180 turns a model that "
    "sits with its back to the wheel.", rex::kRequiresRestart, rex::kDebugOnly);

};

inline ModsSettings g_mods;

inline auto& model_mods = g_mods.model_mods;
inline auto& model_mods_anchor_bones = g_mods.model_mods_anchor_bones;
inline auto& model_mods_bone = g_mods.model_mods_bone;
inline auto& model_mods_cabin_light = g_mods.model_mods_cabin_light;
inline auto& model_mods_car_budget = g_mods.model_mods_car_budget;
inline auto& model_mods_car_lod1_copy = g_mods.model_mods_car_lod1_copy;
inline auto& model_mods_car_pack_write = g_mods.model_mods_car_pack_write;
inline auto& model_mods_car_scale = g_mods.model_mods_car_scale;
inline auto& model_mods_car_space_only = g_mods.model_mods_car_space_only;
inline auto& model_mods_car_textures = g_mods.model_mods_car_textures;
inline auto& model_mods_car_verbatim = g_mods.model_mods_car_verbatim;
inline auto& model_mods_car_yaw = g_mods.model_mods_car_yaw;
inline auto& model_mods_decimate = g_mods.model_mods_decimate;
inline auto& model_mods_diag = g_mods.model_mods_diag;
inline auto& model_mods_glass_band = g_mods.model_mods_glass_band;
inline auto& model_mods_grow = g_mods.model_mods_grow;
inline auto& model_mods_grow_probe = g_mods.model_mods_grow_probe;
inline auto& model_mods_grow_slack = g_mods.model_mods_grow_slack;
inline auto& model_mods_offset_x = g_mods.model_mods_offset_x;
inline auto& model_mods_offset_y = g_mods.model_mods_offset_y;
inline auto& model_mods_offset_z = g_mods.model_mods_offset_z;
inline auto& model_mods_part_growth = g_mods.model_mods_part_growth;
inline auto& model_mods_part_probe = g_mods.model_mods_part_probe;
inline auto& model_mods_passthrough = g_mods.model_mods_passthrough;
inline auto& model_mods_pitch = g_mods.model_mods_pitch;
inline auto& model_mods_proportions = g_mods.model_mods_proportions;
inline auto& model_mods_rim_autoalign = g_mods.model_mods_rim_autoalign;
inline auto& model_mods_rim_band_profile = g_mods.model_mods_rim_band_profile;
inline auto& model_mods_rim_inherit_shade = g_mods.model_mods_rim_inherit_shade;
inline auto& model_mods_rim_keep_badge = g_mods.model_mods_rim_keep_badge;
inline auto& model_mods_rim_offset_x = g_mods.model_mods_rim_offset_x;
inline auto& model_mods_rim_pitch = g_mods.model_mods_rim_pitch;
inline auto& model_mods_rim_roll = g_mods.model_mods_rim_roll;
inline auto& model_mods_rim_scale = g_mods.model_mods_rim_scale;
inline auto& model_mods_rim_shade_occlusion = g_mods.model_mods_rim_shade_occlusion;
inline auto& model_mods_rim_shade_profile = g_mods.model_mods_rim_shade_profile;
inline auto& model_mods_rim_texture_mode = g_mods.model_mods_rim_texture_mode;
inline auto& model_mods_rim_yaw = g_mods.model_mods_rim_yaw;
inline auto& model_mods_roll = g_mods.model_mods_roll;
inline auto& model_mods_scale = g_mods.model_mods_scale;
inline auto& model_mods_share_images = g_mods.model_mods_share_images;
inline auto& model_mods_submeshes = g_mods.model_mods_submeshes;
inline auto& model_mods_textures = g_mods.model_mods_textures;
inline auto& model_mods_weight_smoothing = g_mods.model_mods_weight_smoothing;
inline auto& model_mods_yaw = g_mods.model_mods_yaw;

}  // namespace larecomp::settings
