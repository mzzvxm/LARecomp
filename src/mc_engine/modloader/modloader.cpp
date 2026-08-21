#include "modloader.h"

#include <rex/cvar.h>
#include <rex/runtime.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        out.push_back(ModEntry{file.path().stem().string(), file.path(), mod_name});
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
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);

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

// The box a set of named groups occupies inside `mesh`.
bool GroupBounds(const Mesh& mesh, const std::vector<std::string>& groups,
                 float min_out[3], float max_out[3]) {
    bool any = false;
    for (const MeshPart& part : mesh.parts) {
        if (std::find(groups.begin(), groups.end(), part.group) == groups.end()) continue;
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

// Everything belonging to `groups`, lifted out as a mesh of its own. A triangle
// comes along only when all three of its vertices do, so a primitive is never
// split down the middle.
Mesh ExtractGroups(const Mesh& mesh, const std::vector<std::string>& groups) {
    std::vector<uint32_t> remap(mesh.vertices.size(), 0xFFFFFFFFu);
    Mesh out;
    out.images = mesh.images;

    for (const MeshPart& part : mesh.parts) {
        if (std::find(groups.begin(), groups.end(), part.group) == groups.end()) continue;
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
        if (source.empty()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: no .glb, .gltf or .obj in the folder",
                               vehicle.mod_name, vehicle.car);
            continue;
        }

        std::string error;
        const std::vector<PartMapping> mappings =
            ReadPartsFile(vehicle.folder / kVehiclePartsFile, error);
        if (mappings.empty()) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}", vehicle.mod_name, vehicle.car,
                               error.empty() ? "parts.txt maps no slots" : error);
            continue;
        }

        Mesh mesh;
        if (!LoadMeshFile(source, mesh, error)) {
            LARECOMP_APP_ERROR("[mods] {}/vehicles/{}: {}", vehicle.mod_name, vehicle.car, error);
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

        const std::string archive_path = "resources/rims/" + mod.asset + "/body_lod_0.xrsc";
        const std::filesystem::path cache_file = cache_dir / kRimFolder / (mod.asset + ".tpl");

        Rsc5Resource resource;
        if (!LoadTemplateCache(cache_file, resource)) {
            if (!ExtractTemplate(archive, archive_path, resource, error)) {
                LARECOMP_APP_ERROR("[mods] {}/rims/{}: {}", mod.mod_name, mod.asset, error);
                continue;
            }
            SaveTemplateCache(cache_file, resource);
        }

        // The texture pack comes first, because it decides something the
        // geometry rewrite needs: which of the wheel's materials samples a
        // texture. The wheel body ships on the one that does not.
        const std::string texture_path = "resources/rims/" + mod.asset + "/" + mod.asset + ".xtp";
        const std::filesystem::path texture_cache =
            cache_dir / kRimFolder / (mod.asset + ".xtp.tpl");

        Rsc5Resource textures;
        bool have_textures = false;
        if (!atlas.empty()) {
            have_textures = LoadTemplateCache(texture_cache, textures);
            if (!have_textures && ExtractTemplate(archive, texture_path, textures, error)) {
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
        offset.shade_profile = offset.uniform_shade && REXCVAR_GET(model_mods_rim_shade_profile);
        offset.grow_buffers = REXCVAR_GET(model_mods_grow);
        offset.x = static_cast<float>(REXCVAR_GET(model_mods_rim_offset_x));
        offset.yaw += static_cast<float>(REXCVAR_GET(model_mods_rim_yaw));
        offset.pitch += static_cast<float>(REXCVAR_GET(model_mods_rim_pitch));
        offset.roll += static_cast<float>(REXCVAR_GET(model_mods_rim_roll));
        offset.scale = static_cast<float>(REXCVAR_GET(model_mods_rim_scale));
        offset.decimate = REXCVAR_GET(model_mods_decimate);
        offset.submeshes = REXCVAR_GET(model_mods_submeshes);
        offset.diagnose = REXCVAR_GET(model_mods_diag);

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

        LARECOMP_APP_INFO("[mods] {} -> rim {} ({} tris in {} submesh(es){}{})", mod.mod_name,
                          mod.asset, stats.triangles, stats.submeshes,
                          stats.decimated ? ", decimated" : "", paint);
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
    for (const RawFile& file : raw_files) {
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
        std::vector<uint8_t> bytes;
        if (!read_ec && size <= 0x3FFFFFFFull) {
            std::ifstream input(file.source, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
        }
        if (bytes.empty() || bytes.size() != static_cast<size_t>(size)) {
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

    const bool passthrough = REXCVAR_GET(model_mods_passthrough);
    if (passthrough) {
        LARECOMP_APP_INFO("[mods] passthrough is on: repacking the original geometry, "
                          ".obj meshes are ignored");
    }

    for (const auto& mod : build_list) {
        Mesh mesh;
        std::string error;
        if (!LoadMeshFile(mod.obj, mesh, error)) {
            LARECOMP_APP_ERROR("[mods] {}/{}: {}", mod.mod_name, mod.asset, error);
            continue;
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

    if (!rim_mods.empty())
        BuildRimMods(rim_mods, archive, cache_dir, passthrough, writer);
    if (!car_mods.empty())
        BuildVehicleMods(car_mods, archive, cache_dir, passthrough, writer);

    const std::filesystem::path mod_archive = game_root / kModArchiveName;

    if (writer.empty()) {
        // Leaving a stale archive behind would silently keep an old mod alive.
        std::filesystem::remove(mod_archive, ec);
        LARECOMP_APP_ERROR("[mods] nothing built, model replacement disabled");
        return;
    }

    if (!writer.Write(mod_archive)) {
        LARECOMP_APP_ERROR("[mods] cannot write {}", mod_archive.string());
        return;
    }

    g_mod_archive_ready = true;
    LARECOMP_APP_INFO("[mods] {} with {} replacement(s), mounted last", kModArchiveName,
                      writer.size());
}

}  // namespace mc::modloader
