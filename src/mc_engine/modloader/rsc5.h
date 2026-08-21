// RSC5 resource container + rmcDrawable surgery.
//
// A .xrsc file is:
//   +0  'RSC5' magic 0x05435352 (big endian)
//   +4  resource type (6 = drawable, 3 = bounds)
//   +8  flag: segment sizes + bit30 compressed + bit31 resource
//   +12 0x0FF512EF, the XCompress marker zlibInflater::InflateBegin checks
//   +16 compressed length
//   +20 LZX stream
//
// Decompressed it is one buffer holding the virtual segment followed by the
// physical segment. Pointers inside are absolute: 0x5xxxxxxx indexes the
// virtual segment, 0x6xxxxxxx the physical one.
//
// Rather than authoring a drawable from scratch we rewrite one: the shipped
// resource is the template, and only the geometry of the largest model is
// swapped for the mod mesh. Shader group, skeleton, bone palettes, bounding
// volumes and LOD wiring stay exactly as the game authored them, which is why
// the result loads with no other changes anywhere.
//
// Two resource types come through here. Type 6 is a character, whose drawable
// hangs off a wrapper at virtual+16. Type 63 is a wheel or a vehicle body
// (body_lod_N.xrsc), which is its own root at the start of the virtual segment
// and keeps its LODs and skeleton at different offsets. Everything below a LOD
// is identical between them, so the rewrite is shared and only the root is
// described per type -- see DrawableLayout in the implementation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "objmesh.h"
#include "texture.h"

namespace mc::modloader {

struct Rsc5Resource {
    std::vector<uint8_t> data;  // [virtual][physical]
    uint32_t virtual_size = 0;
    uint32_t physical_size = 0;
    uint32_t type = 0;
    uint32_t flag = 0;
};

// Splits the 20-byte header off a .xrsc and reports the sizes the payload
// decompresses to. Does not decompress.
bool ParseRsc5Header(const std::vector<uint8_t>& file, Rsc5Resource& out,
                     size_t& payload_offset, size_t& payload_size, std::string& error);

// Encodes a segment size the way the flag word does: mantissa << (shift + 8),
// mantissa capped at 0x7FF. Shifts below 3 are never used, so a segment is
// always a whole number of 2048-byte sectors. `preferred_shift` is tried first,
// which lets a grown segment keep the page class the original resource used
// instead of silently switching to a finer one.
// A mantissa is a page count, and there was a moment where that looked like a
// budget: sub_821BC140 walks a chunk array at +8 of the request with its count
// at +1540, twelve bytes an entry, which reads as a ceiling of 127. It is not
// one, and the shipped data says so -- the driver's own resource declares 26
// virtual pages and 207 physical, 233 chunks, and loads. Whatever +1540 counts,
// it is not one entry per page, so nothing here caps the page count.
bool EncodeSegmentSize(uint32_t size, uint32_t& mantissa, uint32_t& shift,
                       uint32_t preferred_shift = 0);

// Wraps a resource back into the on-disk .xrsc shape: the 12-byte RSC5 header,
// the 8-byte XCompress marker, then an LZX stream. `out_flag` comes back with
// the compression bit set and is what the archive entry must carry.
//
// The resource is taken by reference because it may have to grow: the game
// reads a fixed number of 32768-byte blocks, worked out from the uncompressed
// size and not from the file, and a stream of stored LZX blocks is slightly
// larger than the data it carries. When that does not fit, the virtual segment
// is padded with zeroes until it does -- see the comment on the implementation
// for why every vehicle part needs this and no character ever did.
bool BuildRsc5File(Rsc5Resource& resource, std::vector<uint8_t>& out_file,
                   uint32_t& out_flag, std::string& error);

// Appends `bytes` of zeroes to the physical segment and declares it that much
// larger. Nothing already in the resource moves, and no pointer changes.
//
// This exists to settle a question that decides whether mod meshes have to be
// decimated at all. Growing a resource was tried once and written off: vertex
// and index buffers were appended past the end of the physical segment, the
// size in the flag was raised, the geometry was repointed -- and it rendered
// garbage, even when the bytes moved were the originals. That verdict is now
// suspect, because "garbage" is exactly what the streamer's read budget
// produces: a stream longer than ceil(uncompressed / 32768) blocks has its tail
// truncated, and the tail was where the new buffers had been put. With the
// budget respected, growing may simply work -- and if it does, the whole
// vertex ceiling goes away.
//
// So this changes one variable and nothing else: the segment gets bigger, the
// resource is otherwise untouched. If it still renders, growth is safe.
bool GrowPhysicalSegment(Rsc5Resource& resource, uint32_t bytes, std::string& error);

// The same for the virtual segment, which is where a vehicle part keeps its
// buffers: those resources have no physical segment at all. The bytes go on the
// end of the virtual segment, so every virtual address is untouched, and the
// physical segment moves along with it -- which changes nothing either, since a
// physical address is resolved relative to wherever the virtual segment ends.
bool GrowVirtualSegment(Rsc5Resource& resource, uint32_t bytes, std::string& error);

struct RewriteStats {
    uint32_t vertices = 0;   // what ended up in the resource
    uint32_t triangles = 0;
    uint32_t bones = 0;       // palette entries used, when the mesh came skinned
    uint32_t first_bone = 0;  // bone the heaviest palette slot maps to
    bool decimated = false;  // the mesh had to be reduced to fit
    uint32_t submeshes = 0;  // how many of the drawable's slots it was dealt into
    // Which shader of the group ends up drawing the mesh -- the texture swap
    // needs it to know whose diffuse map to overwrite.
    uint32_t shader = 0xFFFFFFFFu;

    // Filled only when MeshOffset::diagnose is set. `report` is a written
    // account of the template's submeshes, both rigs, the joint mapping and how
    // the mesh was dealt out; the two .obj bodies are the mesh as it stood
    // before the retarget and as it was written, so a deformation can be told
    // from a dealing mistake by opening them side by side.
    std::string report;
    std::string obj_before;
    std::string obj_after;
    // The template's own vertices, as a point cloud: where the game puts its
    // flesh around the bones the mod is being fitted onto.
    std::string obj_template;
};

// Pass as `bone` to let a skinned mesh keep its own weights; any real bone
// index instead forces the rigid single-bone path, which is the fallback when
// skinning misbehaves.
constexpr uint32_t kAutomaticBone = 0xFFFFFFFFu;

// Diagnostic: bind rigidly to palette slot 1 and leave the palette exactly as
// shipped. Two separate attempts at multi-bone skinning made the model vanish,
// and both of them wrote palette entries as well as non-zero blend indices.
// This isolates the two: the mesh renders on the template's own second bone if
// non-zero indices are fine and the palette writes were at fault, and vanishes
// if the indices themselves are what the renderer rejects.
constexpr uint32_t kSlotOneTest = 0xFFFFFFFEu;

// Writes `mesh` over the geometry of the template's largest model and silences
// the other submeshes. The mesh is scaled into the drawable's own bounding box
// first, so bounds, LOD distances and culling data stay valid untouched, then
// decimated if needed and written into the buffers the template already owns --
// the resource keeps its size, its addresses and its flag.
//
// The mesh rides `bone` rigidly: every vertex gets full weight on palette slot
// 0 and that slot is pointed at `bone`. A .obj carries no skin weights, and
// deriving them from the original mesh by proximity was tried and rejected --
// it made the model vanish. Real per-vertex skinning needs weights to come in
// with the mesh.
// Nudge applied to the mesh after it is fitted, in metres, model space.
struct MeshOffset {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;  // degrees, applied before fitting
    float scale = 1.0f;                           // extra scale on top of the fit

    // How much of the model's own build to keep when it is reposed onto the
    // driver's skeleton. 0 puts every joint exactly on its bone, which is what
    // the game's skinning expects and what a human-proportioned model wants. 1
    // takes only the direction from the skeleton and keeps the model's own bone
    // lengths, which stops a character built to different proportions from being
    // stretched to fit. See the cvar for the trade.
    float proportions = 0.0f;

    // Whether a mesh too big for the slot may be welded down to fit. Off, it is
    // refused instead and the variant keeps the shipped model.
    bool decimate = true;

    // Whether the mesh may be dealt across every submesh of the drawable rather
    // than crammed into the largest one. Off restores the single-slot behaviour.
    bool submeshes = true;

    // Wheels only: push the mesh out along the axle until its face is flush with
    // the shipped wheel's. Fitting alone centres the mesh in the template's box,
    // and a wheel's box is not where its wheel is -- every shipped rim keeps its
    // hub and spokes in the negative half of the axle and only a thin lip at the
    // far edge, so a rim thinner than the box (a bare rim with no tyre) comes out
    // sunk into the tyre. See AlignMeshToAxle.
    bool align_axle = false;

    // Whether to leave the shipped per-vertex shading where it is instead of
    // carrying it over vertex by vertex.
    //
    // Carrying it over is right for a character: the mesh has been reposed into
    // the very skeleton it is replacing, so the nearest shipped vertex is on the
    // same shoulder. A wheel shares nothing with the wheel it replaces -- five
    // spokes against ten -- so the nearest shipped vertex is wherever the old
    // spoke happened to be, and its baked occlusion arrives as the old wheel's
    // shadows printed onto the new one. The second UV set is worse: it selects
    // which material band a vertex belongs to, and scattering four bands by
    // proximity across a shape that does not share them lights the wheel in
    // patches. Uniform means one value for the whole mesh, taken from the
    // template's own most-used band.
    bool uniform_shade = false;

    // Wheels only: rebuild the shade lane from the template instead of flooding
    // it with one average.
    //
    // Uniform is the right cure for the wrong disease. It stops the old rim's
    // spoke shadows being printed on the new one, and in doing so throws away a
    // lane that runs the whole 0..255 on every shipped wheel -- so the mod
    // arrives with no cavities at all, lit only by the material's specular,
    // which is pinned to the geometry and sweeps round as the wheel turns.
    //
    // This reads the same template and stops reading it by position: the lane
    // is binned on distance from the axle, depth along it, and how the surface
    // faces in each, none of which say where a spoke is. Requires uniform_shade
    // -- it replaces that mode's flat fill, and keeps its band and tint.
    bool shade_profile = false;

    // Whether the mesh already sits where it belongs and must not be fitted.
    //
    // A character and a wheel each replace one whole thing, so scaling them into
    // the template's box is exactly right. A car is different: it is delivered as
    // twenty separate slots, and every one of them has to be scaled by the SAME
    // factor or the hood comes back a size larger than the doors it sits between.
    // The caller works that factor out once against the car body and places each
    // part itself; this says so, and the rewrite writes the vertices as they
    // arrived.
    bool pre_fitted = false;

    // Whether the drawable's models are functional units that must not be left
    // half written.
    //
    // A character's fifteen models are interchangeable pieces of one body, so
    // dealing a mod across all of them and silencing whatever is left over is
    // exactly right. A car's are not: the body drawable alone holds the shell,
    // the grille, the plate recess and the rear trim as separate models, each
    // attached to something. Spreading one mesh over all of them empties most,
    // and a drawable full of models with nothing in them is not something the
    // game is ever handed. With this set the rewrite stays inside the single
    // largest model -- it is filled and its own leftovers silenced -- and every
    // other model is left exactly as it shipped.
    bool whole_models = false;

    // Whether the mesh may be given buffers of its own instead of being made to
    // fit the ones the template shipped with.
    //
    // Everything else here works by writing inside what already exists, which is
    // why a replacement has always had to be welded down to a few thousand
    // vertices. It does not have to be: a resource can be made larger, and the
    // geometry pointed at the new space. That was believed impossible until the
    // read-budget bug was found -- growing was tested, rendered garbage, and the
    // garbage was the truncated tail, not the growth.
    //
    // Only the rigid path uses this. A skinned submesh also caps how many bones
    // its palette may name, and that is a separate allocation with its own
    // rules; a character is left on the path that already works.
    bool grow_buffers = false;

    // Which shader of the group the mesh must be drawn with, or -1 to let the
    // rewrite choose.
    //
    // A wheel ships two materials and its body is on the one with no texture at
    // all: the metal is the shader and the baked per-vertex shade, and the only
    // thing sampling the wheel's texture is the badge quad. Every shipped rim
    // texture is a decal sheet -- a maker's logo plus lug nuts and valve stems,
    // most of it transparent -- so this is right as it stands, and pointing the
    // body at the textured material is only wanted when the mod brought a skin
    // for the whole rim rather than a badge. Only the texture pack knows which
    // material that is.
    int32_t force_shader = -1;

    // A shader whose submeshes must be left exactly as they shipped -- neither
    // written into nor silenced. Used to protect a wheel's badge quad, which is
    // both far too small to be worth filling and the one surface that reads the
    // wheel's texture. Ignored if it would claim every submesh there is.
    int32_t reserve_shader = -1;

    // How many rounds to spread each vertex's influences over its neighbours
    // before the mesh is reposed. 0 leaves the weights exactly as authored.
    //
    // The models people bring are rigidly weighted -- one bone per vertex at
    // full strength -- and a retarget hands neighbouring bones unrelated rigid
    // transforms, so a rigidly weighted seam is torn apart rather than bent. See
    // SmoothSkinWeights.
    int weight_smoothing = 4;

    // Whether each joint is planted on the bone it was matched to.
    //
    // On, the repose puts every joint exactly where the skeleton says, and a
    // hand therefore lands exactly on the bone the game drives a hand with. It
    // also drags the body: a model's chest joint sits at 73% of its height and
    // the driver's chest bone at 77.5%, so the chest is hauled up 78 mm and back
    // 70 mm while the collarbone moves 24 mm, and the flesh between them takes
    // the difference -- 21 mm of shape lost at the shoulder, a crease across the
    // chest, and a belly pulled in behind a spine bone that lives at the back of
    // a torso where the model's lives down the middle.
    //
    // Off, nothing is planted: the joints keep the offsets they were authored
    // with and only turn, so the body arrives with the shape the fit gave it.
    // Measured on the driver mod that halves the distortion, 5.7 mm to 2.8 mm
    // averaged over every bone. The cost is at the far end of a limb, which now
    // reaches as far as the MODEL's arm does rather than as far as the game's:
    // 38 mm at the wrist and 96 mm at the foot on that same model. A hand that
    // far from the bone driving it swings on the wrong lever once animation
    // starts, and no measurement here can say how that reads in motion -- which
    // is the whole reason this is a switch and not a decision.
    bool anchor_bones = true;

    // Grow the resource and stop there: no buffer is moved, no pointer is
    // rewritten, no count changes, and the mesh is decimated into the
    // buffers the template already owned. Growing and repointing have only
    // ever been tried together, so a failure could not be pinned on either;
    // this holds back the second half so the first can be judged alone.
    // 0 off. 1 grows the resource and stops: no buffer moves, no pointer is
    // rewritten, no count changes. 2 also puts the buffers in the new space and
    // repoints them, but leaves the counts alone, so the mesh is still decimated
    // to what the template shipped.
    //
    // Growing and repointing have only ever been tried together, so a failure
    // could not be pinned on either. 1 says whether a resource may be made
    // larger at all; 2 says whether the bytes in the new space actually arrive
    // and can be fetched from. Only after both is a changed count worth trying.
    int grow_probe = 0;

    // Extra bytes asked for beyond what the buffers need, so they stop well
    // short of the segment's end. The mesh a probe writes lands within a few
    // kilobytes of that end, and what breaks breaks there; slack says whether
    // the tail of a grown segment is what fails to arrive.
    uint32_t grow_slack = 65536;

    // Fill RewriteStats::report and the two .obj bodies. Off by default: the
    // report walks both rigs and the .obj text is the whole mesh again, so it is
    // paid for only when someone is looking.
    bool diagnose = false;
};

// The box the template's geometry occupies, in the drawable's own space. What
// the caller needs to place a part into a slot without scaling it: the slot says
// where its own centre is, and the part is moved onto it.
bool ReadDrawableBounds(const Rsc5Resource& resource, float min_out[3], float max_out[3]);

bool RewriteDrawableGeometry(Rsc5Resource& resource, Mesh mesh, uint32_t bone,
                             const MeshOffset& offset, std::string& error,
                             RewriteStats* stats = nullptr);

struct TextureStats {
    std::string name;      // the shipped texture that was overwritten
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;   // mip levels rewritten alongside the base
};

// Writes `atlas` over the diffuse texture of shader `shader_index`, in place.
//
// The atlas is resampled to whatever the shipped texture measures and encoded
// in the format it already declares, so the bytes replace it exactly: same
// address, same size, same fetch constant. The mip chain lives at its own
// address and is rewritten too -- skipping it leaves the original driver's skin
// showing at any distance, because minification is what mips are for. Only the
// levels the resource already stores are touched, and never past the start of
// whatever is allocated next.
bool ReplaceShaderDiffuse(Rsc5Resource& resource, uint32_t shader_index, const Image& atlas,
                          std::string& error, TextureStats* stats = nullptr);

// Writes `image` over the texture a wheel keeps in its sibling `<wheel>.xtp`.
//
// A wheel's drawable carries no textures at all -- its shader group is a
// different class with no name pointer and nothing embedded -- so replacing the
// mesh alone leaves a modded rim wearing the shipped wheel's paint. The paint
// lives in a texture pack next to it, resource type 83: a material pack holding
// a grcTextureDictionary of grcTextures, each one the same struct a character's
// shader parameters point at. That means the write itself is the one that
// already works -- resample to the shipped size, encode in the shipped format,
// tile, drop it at the same address -- and only the walk down to the array is
// specific to the pack.
//
// Fails, leaving the resource untouched, when the wheel ships no texture: 28 of
// the 175 have no physical segment and no dictionary, which is how the game
// stores a wheel that is only lit and never sampled.
bool ReplaceDictionaryTexture(Rsc5Resource& resource, const Image& image, std::string& error,
                              TextureStats* stats = nullptr);

// Which shader index of the wheel's drawable samples the texture this pack
// holds -- the pack's materials are in the same order as the drawable's shader
// indices. Pass it as MeshOffset::force_shader so the replaced mesh is drawn
// with a material that reads a texture at all. False when the pack has no
// writable texture, or no material admits to sampling it.
bool TexturePackShader(const Rsc5Resource& resource, uint32_t& shader_index);

}  // namespace mc::modloader
