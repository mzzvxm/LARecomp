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
// one, and the shipped data settles it -- of the 12,987 resources in
// xarchive_cache.rpf, 682 declare more than 127 pages and the largest declares
// 1454. Whatever +1540 counts, it is not one entry per page, so nothing here
// caps the page count. The page SIZE is what matters: it sets the blocks a
// resource is split into -- see kCoarsestGrownShift and PadResourceTail.
bool EncodeSegmentSize(uint32_t size, uint32_t& mantissa, uint32_t& shift,
                       uint32_t preferred_shift = 0);

// Wraps a resource back into the on-disk .xrsc shape. A payload of one LZX
// frame (32768 bytes) or less gets the 12-byte RSC5 header, the 8-byte
// XCompress marker and a stored LZX stream, with the compression bit set in
// `out_flag`. Anything larger ships uncompressed: bit 31 set, bit 30 clear and
// the payload straight after the 12-byte header -- see the implementation for
// why the streamer cannot take more than one stored frame. `out_flag` is what
// the archive entry must carry.
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

// Leaves at least `want` bytes between the last buffer any LOD draws from and
// the end of the segment that buffer lives in, growing the segment if it does
// not already have that much room.
//
// The end of a resource does not arrive intact. A probe once left the index
// buffer 16,516 bytes from the end of a wheel and it came back holed; 64 KB of
// nothing after it came back clean. The BMW hit it again from the other side:
// interior0_lod_0 shipped PERFECT -- every index in range, every edge sane,
// measured off the file in xarchive_mods.rpf -- with its last buffer 4,452
// bytes from the end of a 2.9 MB segment, and in the game those 10,138
// triangles of paint came back as slabs. The drawables that drew correctly all
// had between 72,844 and 125,694 bytes after their last buffer.
//
// Measuring is the point. The rewrite already asks for slack when it grows
// (MeshOffset::grow_slack), but a later shader pass writes its own buffers into
// that slack without growing anything, so "this resource was grown" says
// nothing about whether the last buffer is safe. Call this once, after every
// pass has had its turn.
// `out_room`, when given, comes back as the bytes that end up after the last
// buffer, so a caller can put the number the next log has to be judged on into
// its own line.
bool PadResourceTail(Rsc5Resource& resource, uint32_t want, std::string& error,
                     uint32_t* out_room = nullptr);

// The largest block the virtual segment will be split into, which is
// 4096 << page_shift. A buffer has to fit inside one of these.
uint32_t VirtualBlockSize(const Rsc5Resource& resource);

// Grows the virtual segment to a whole number of those blocks, so that every
// block is the same size and a buffer aligned to one is inside one. Without it
// the segment ends in a tail of halved blocks and the alignment means nothing
// there. Call after the last buffer is placed, with PadResourceTail.
bool RoundVirtualToBlock(Rsc5Resource& resource, std::string& error);

// Re-states the virtual segment in a coarser (or finer) page class, padding it
// up to a whole number of pages. Every virtual address is unchanged -- only the
// flag and the bytes after the end move -- and the point is the block size:
// coarser pages mean bigger blocks, and a buffer that has to be contiguous can
// only be as big as a block. Use before rewriting geometry, not after.
bool SetVirtualPageShift(Rsc5Resource& resource, uint32_t shift, std::string& error);

struct RewriteStats {
    uint32_t vertices = 0;   // what ended up in the resource
    uint32_t triangles = 0;
    uint32_t bones = 0;       // palette entries used, when the mesh came skinned
    uint32_t first_bone = 0;  // bone the heaviest palette slot maps to
    bool decimated = false;  // the mesh had to be reduced to fit
    // The shade lane that was baked, when MeshOffset::shade_occlusion asked for
    // one. Reported because a lane is invisible in the file and its whole point
    // is its spread: a shipped wheel runs about 0..252 with a deviation near
    // fifty, and a flooded one has a deviation of zero. Without these in the log
    // there is no way to tell from a screenshot whether the bake even ran.
    uint32_t shade_low = 0;
    uint32_t shade_high = 0;
    float shade_mean = 0.0f;
    float shade_deviation = -1.0f;   // negative when nothing was baked
    // How many distinct material bands the written mesh ended up with. One is
    // the flood; the shipped wheels have four.
    uint32_t bands = 0;
    // With MeshOffset::band_by_piece: which band each piece settled on, as
    // "band x: n piece(s), v vertices" -- for the log, since a pane number is
    // invisible in the file and only shows as film or no film in game.
    std::string band_summary;
    // Submeshes of this pass left silent because their model is bone-local.
    uint32_t local_silenced = 0;
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
// One shipped vertex's position and raw texcoord1 word, read from a drawable
// other than the one being written. See MeshOffset::extra_bands.
struct BandSample {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32_t texcoord1 = 0;
};

// A box in the car's space, mirrored across x (|x| is tested), whose vertices
// are given one lamp index outright. See MeshOffset::lamp_boxes.
struct LampBox {
    uint32_t index = 0;
    std::string material;  // only this mod material, or empty / "*" for any
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
};

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

    // With uniform_shade: flood texcoord1.x with this value instead of the
    // host's dominant band, when non-negative. The low half (y) stays the
    // host's.
    //
    // On InteriorTrim the band is an interior ZONE, and a zone carries more than
    // a colour: xInteriorTrim's VS_BumpSpec reads a0 = trunc(texcoord1.x) and
    // writes oTexCoord0.xy = texcoord0.xy * intDL0[a0].w -- a per-zone UV scale,
    // there because the donor's cabin tiles detail sheets (UVs -11..12 on the
    // 240SX's wheel). A mod's atlas UVs run 0..1, so a zone whose scale is not
    // one prints the picture over and over: the S15's stock wheel in zone 5
    // showed its hub badge three times. Zones the cabin already draws right
    // (0 and 9 on the S15) are the safe ones to point a part at.
    float fixed_band = -1.0f;

    // With uniform_shade: flood this colour word instead of the host's average,
    // when non-zero. For geometry written into a submesh that was never its own
    // -- a licence plate drawn in the Underbody -- the host's baked shade is the
    // wrong answer: the Impala's front-bumper Underbody averages 5 of 255 and a
    // plate flooded with it came out grey next to one at 38 on the boot lid.
    uint32_t fixed_shade = 0;

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

    // Bake the shade lane from occlusion measured on the replacement itself.
    //
    // The honest answer to a flat-lit wheel, and the only one of the three that
    // describes the mesh actually being drawn. See BakeOcclusion for what the
    // other two do and why neither works: a flooded lane has zero deviation
    // where every shipped wheel has about fifty, and carrying the template's
    // lane across by proximity lights the wheel neon. Requires uniform_shade,
    // whose flooded band it keeps and whose flat level it replaces.
    bool shade_occlusion = false;
    // Keep every buffer this pass places inside one block of this many bytes.
    //
    // A resource is split into blocks of halving powers of two starting at
    // 4096 << page_shift (sub_821E57B8), and each block is allocated separately
    // (sub_821E58F0). Every pointer is fixed up by the delta of the block it
    // falls in, so a buffer that runs past its block's end is read as though the
    // next block followed it in memory, and from that byte on the GPU fetches
    // another allocation entirely -- blades on screen from a file that is
    // perfect on disk. 0 places buffers wherever they fit, which is what the
    // loader did before this was measured.
    uint32_t block_align = 0;
    // Carry the template's MATERIAL BANDS across instead of flooding one.
    //
    // The band is texcoord1.x, and xRimMain's VS_Common indexes materialHookups
    // and tintColors with it -- the rim's whole appearance is picked per vertex
    // by this number, and PS_Multi then reflects a dual-paraboloid environment
    // map through whatever material it selected. Measured on two unrelated
    // shipped wheels, the body uses FOUR bands and they sit in the same places
    // on both:
    //
    //   band  r/R (5|50|95%)      x/X (5|50|95%)        what it is
    //   0     0.13|0.57|0.93      -0.97|-0.92|-0.66     the face and spokes
    //   1     0.82|0.84|0.88      -0.72|-0.66|-0.62     the bead step
    //   2     0.93|0.95|1.00      -1.00|-0.96|-0.92     the outer lip
    //   3     scattered, and the only band that reaches positive x
    //
    // whl_am_bbs_ch and whl_stk_nsn_skyline_99 agree to within 0.02 on the
    // first three. Flooding the dominant band gives the whole rim ONE material,
    // and one material over a mirror is a mirror ball: its highlight sweeps
    // round as the wheel turns, which is what "the normals change with the
    // rotation" describes.
    //
    // The bands are rings, so they are carried by nearest neighbour in
    // (radius, axial depth) normalised to each mesh's own extent -- not by 3D
    // proximity, which is what inherit_shade did, and not carrying the colour,
    // which is what made inherit_shade land vertices on the emissive band.
    // Requires uniform_shade.
    bool band_profile = false;
    // Take texcoord1 from the NEAREST shipped vertex instead of flooding one.
    //
    // On CarLight texcoord1.x is not a band but WHICH LAMP a vertex is:
    // xCarLight's VS_Light reads trunc(texcoord1.x) into a0 and takes the lamp's
    // colour from gCarConstantBuffer(4 + a0) and its on/off state from
    // gCarConstantBuffer(39 + a0). Measured on the police Caprice: the front
    // lamps (z -2.4) carry 2 and 3, the rear ones 0 (x +-0.79) and 6 (inboard),
    // the light bar 13 and 14. Flooding the dominant value of the submesh being
    // written -- the rear one -- made every lamp of the BMW a tail lamp, which is
    // the red headlights. A lamp index is a place on the car, so the nearest
    // shipped lamp vertex is the right donor for it. The shade word stays
    // flooded.
    bool nearest_band = false;

    // With nearest_band: settle the band once per PIECE of the mesh instead of
    // per vertex -- every vertex of a connected piece (joined by shared corners
    // or by sharing a position) takes the band most of the piece's vertices
    // found.
    //
    // For glass, where texcoord1.x is which pane a vertex belongs to: 0 the
    // windscreen, 1 the rear window, 2 and 3 the doors, and the window film
    // (xcc+0x160) tints 1-3 and never 0. Flooded with the dominant band of the
    // submesh written into, a whole replacement body's glass is one pane --
    // vp_nsn_240sx_98's car-space glass is {0: 90, 1: 84, 2: 29, 3: 29}
    // vertices, so every window of the S15 was windscreen and the film tinted
    // nothing. The nearest donor pane is the right answer for a pane, but not
    // vertex by vertex: the band is read in the vertex shader and blended across
    // a triangle, so a windscreen whose top corners sit nearer the donor's rear
    // window would fade into tint. One band per piece keeps each pane whole.
    bool band_by_piece = false;

    // Take the whole colour word from the NEAREST shipped vertex (of this pass,
    // preferring one facing the same side) instead of flooding one.
    //
    // In a cabin the colour word is lighting, not a band: xInteriorTrim's VS
    // weights siColor0, siColor1 and siColor2 -- the car's three interior light
    // colours -- by three of its bytes and hands the fourth to the pixel shader.
    // The Impala's interior runs them 0..253 from vertex to vertex; flooded with
    // one average, a whole cabin answered the interior neon about as much as the
    // darkest corner of the donor's does, which is to say not visibly. Where the
    // cabin sits in the donor's own cabin, the donor's vertex there is the best
    // available answer for how much of each light reaches it. Requires
    // uniform_shade.
    bool nearest_shade = false;
    // More candidates for nearest_band, from other drawables of the donor.
    //
    // A modifiable donor keeps its lamps in part slots: the Impala's body and
    // interior0 draw CarLight only as lamp indices 3, 11 and 12 (a lens and two
    // cabin lights), while headlight0 carries 2 and 3 and taillight0 0, 6 and
    // 9. A BMW lamp written into interior0 took 11/12 from the nearest cabin
    // light -- the high beam never lit. These come from `lamp_from` in
    // parts.txt, in the car's own space.
    std::vector<BandSample> extra_bands;
    // Lamp indices stated by region, applied over nearest_band.
    //
    // The index is a lamp TYPE -- the game's own table (0x827E9814, one
    // pointer per index): 0 brake light, 1 taillight, 2 turn signal,
    // 3 headlight, 4 high beam, 5 side marker, 6 reverse light, 7 interior
    // light, 8 fog light, 9 red / 10 amber / 11 clear reflector, 12 third
    // brakelight, 13 red / 14 blue coplight, 15..18 turn signal FL/FR/BL/BR.
    // Nearest-shipped-lamp cannot give a type the donor never put there: the
    // Impala's headlight0 carries 2 (turn signal) and 3 (headlight) only, so
    // half of the BMW's four round lamps came out amber and none was a high
    // beam. A box says it directly.
    std::vector<LampBox> lamp_boxes;
    // Write only into models authored in the CAR's frame, and silence this
    // pass's submeshes in the others.
    //
    // A vehicle body is several models, and the game draws each one with its
    // own matrix: a bumper, a door window, a mirror, a lamp. Those models store
    // their vertices relative to that part's bone, so geometry that arrives in
    // car space and is dealt into one of them lands wherever the bone puts it.
    // Measured on vp_chv_impala_96's body_lod_0: m0 (the boot lid: paint,
    // BrushedMetal, BumpSpec) is y -0.27..0.02, z 0..0.92; m1 (the ONLY CarLight) is a point
    // at y -0.27, z 0.90; m2/m3 (door glass) are y -0.08..0.35. The BMW came
    // back with its nose in the ground, its lamps above the car and its windows
    // tilted -- exactly those three. The caprice never showed it because its
    // big models, m8 and m9, are the car-space ones and the writer happened to
    // favour them.
    //
    // A model is taken to be local when its box centre sits below the car's
    // floor (y < 0.15) or its box straddles the origin on every axis; every car-
    // space model on both donors is well above that and none straddles.
    bool car_space_only = false;

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

    // Restrict the rewrite to the submeshes one shader already draws, or -1 for
    // all of them.
    //
    // A character is one surface and a wheel is nearly one, so dealing a mod
    // across whatever submeshes exist and pointing them all at a single shader
    // is right for both. A car is not: paint, glass, lights and trim are
    // separate shaders of the same drawable, and a body dealt across all of
    // them comes back with its windows painted and its tail lights bodywork.
    //
    // With this set the drawable is rewritten one shader at a time -- the
    // caller hands over only the triangles whose material belongs to that
    // shader, and only the submeshes that shader already draws are candidates.
    // Slots of every other shader are neither written nor counted, which is
    // what makes the calls composable: each leaves the others exactly as it
    // found them.
    int32_t only_shader = -1;

    // Restrict the rewrite to one model of the drawable, or -1 for all of them.
    //
    // A car body keeps a panel the game swings or drops as a model of its own,
    // authored around the bone that moves it: model 0 of vp_chv_impala_96's
    // body_lod_0 is the boot lid, x +-0.77, y -0.27..0.02, z 0..0.93 from the
    // `tk` hinge. Its paint is shader 23, the same as the shell's, so writing a
    // lid into it has to reach that model's submeshes and no other's.
    int32_t only_model = -1;

    // Whether submeshes this pass did not fill keep what they shipped.
    //
    // Leaving them is only right when the pass can see slots that are not its
    // own. With `only_shader` set the candidate list is already that shader's
    // slots and nothing else, so clearing the unfilled ones cannot reach a slot
    // another pass is about to use -- and it must be cleared, or a shader the
    // mod claims but only partly fills keeps drawing the donor there. That is
    // where a police light bar survived on the roof of the car that replaced
    // it: the whole-shader silencing below never looked at CarLight, because
    // CarLight was claimed.
    bool keep_unused = false;

    // Whether the texture coordinate is flooded with one value from the
    // template instead of the mesh's own.
    //
    // For a shader whose picture the mod does not have. The model's .mtl says
    // gta_vehicle_vehglass carries no diffuse at all, so a window's UVs
    // describe nothing; written as they are, the glass samples wherever they
    // land in the donor's pack, which is how a BMW ended up with red windows.
    // The donor's own glass samples one place and looks like glass.
    bool uniform_uv = false;

    // Whether a submesh with no texture coordinate may be written.
    //
    // The layout reader demands position and UV, because a character's every
    // submesh samples a skin and one without a UV would be a misread
    // declaration rather than a real slot. A car has real ones: MCLA draws its
    // cabin blockers and shadow hulls with BlackMatte, a shader that reads
    // nothing, and ships them as twelve-byte position-only vertices. Those are
    // writable -- there is simply no UV to write.
    bool allow_untextured = false;

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

    // The largest the resource's segment may be grown to, or 0 for no ceiling.
    //
    // Growing has no natural limit -- a mod hands over whatever it modelled and
    // the segment follows. The game does have one in practice: of the 9045
    // type-63 drawables it ships, the largest declares 1,163,264 bytes, and a
    // vehicle body four times that is asking the streamer and the resource heap
    // for something neither has ever been handed. When the next buffer would
    // cross this, the slot is left at the size it shipped with and the mesh is
    // welded down to what the slots between them can hold.
    uint32_t grow_ceiling = 0;

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

// Sets the rest pose of every bone of the drawable's skeleton whose name is
// `prefix` followed by digits (extPrimary0, extPrimary1...): translation at +32
// and Euler rotation at +48 of the bone record, relative to its parent. The
// record is the one whose +0 points at the name and whose +16 (parent) points
// into the virtual segment. The skeleton's ready matrices, which are what the
// game builds the bone from, are rewritten with it. Returns how many were set.
size_t SetBonePose(Rsc5Resource& resource, const std::string& prefix, const float translation[3],
                   const float rotation[3]);

bool RewriteDrawableGeometry(Rsc5Resource& resource, Mesh mesh, uint32_t bone,
                             const MeshOffset& offset, std::string& error,
                             RewriteStats* stats = nullptr);

// Makes every submesh drawn by `shader` draw nothing, and reports how many.
//
// The counterpart to a shader-at-a-time rewrite: a donor car brings shaders the
// mod has no material for -- a police light bar, a grille the new body models
// itself -- and those slots would otherwise keep drawing the donor's geometry
// through the replacement. Pass -1 to silence every submesh of the drawable,
// which is how a part slot the new car does not use is delivered: the resource
// still loads, it just has nothing in it. With `only_model` set, only that
// model's submeshes are touched.
size_t SilenceShaderGeometry(Rsc5Resource& resource, int32_t shader, int32_t only_model = -1);

// Gives the high LOD's `model` back the counts `pristine` shipped for the
// submeshes `shader` draws in it (-1: all of them), and reports how many.
//
// Silencing zeroes a submesh's counts and leaves its buffers where they are, so
// this is all it takes to read the donor's vertices again. A body panel written
// into a model the shell pass silenced needs them: the flood of the colour word
// (MeshOffset::uniform_shade) is taken from the vertices the target still has,
// and with none the panel comes out at full brightness beside a shaded shell.
// `pristine` must be the same template before any buffer of that model moved.
size_t RestoreModelGeometry(Rsc5Resource& resource, const Rsc5Resource& pristine, int32_t model,
                            int32_t shader);

// Which vertex semantics each shader's submeshes carry, one bit per semantic in
// declaration order (position 0, normal 3, colour 4, texcoord0 6, texcoord1 7,
// tangent 14), OR-ed over the shader's submeshes of the high LOD -- or of one
// model of it with `only_model` set.
//
// For choosing a host to borrow. A shader drawn from a submesh laid out for
// another reads the vertex through that submesh's declaration, so the host
// has to carry what the borrowed shader reads. Measured on vp_chv_impala_96:
// Chrome and LicensePlate ship POSITION NORMAL COLOUR0 TEX0 TANGENT, EngineBay
// ships the same without the tangent, and BrushedMetal carries TEX1 where the
// others have the tangent -- a Chrome or a plate written into BrushedMetal
// would have no tangent to read.
bool ShaderVertexSemantics(const Rsc5Resource& resource, std::vector<uint32_t>& mask_of,
                           int32_t only_model = -1);

// The layout almost every car shader ships with: POSITION NORMAL COLOUR0 TEX0
// TANGENT. What a borrowed shader is assumed to read when no submesh of it is
// there to ask.
uint32_t CarVertexSemantics();

// How badly a host laid out as `host` serves a shader that reads `wanted`:
// every semantic it lacks costs ten, every one it carries for nothing costs
// one, and a tangent carried for nothing one more. Lower is better.
int LayoutFitScore(uint32_t host, uint32_t wanted);

// Moves every submesh a shader draws by a fixed amount, in the car's own
// space. For the donor's licence plate, which is centred but sits at the
// donor's rear rather than the replacement's.
size_t TranslateShaderGeometry(Rsc5Resource& resource, int32_t shader, float dx, float dy,
                               float dz);

// Every vertex of the high LOD that `shader` draws, as position + texcoord1.
// Returns how many were read; zero when the layout has no texcoord1.
size_t ShaderBandSamples(const Rsc5Resource& resource, int32_t shader,
                         std::vector<BandSample>& out);

// Hides a whole drawable by zeroing each LOD's model count, leaving every
// pointer intact. The only shape of empty part the loader accepts: emptying the
// geometries instead leaves their buffer pointers behind for the placer to
// follow into memory nobody wrote.
size_t SilenceDrawable(Rsc5Resource& resource);

// Which effect each shader index of a vehicle's material pack runs, as an index
// into the game's own car shader list (shaders/cars/preload.list).
//
// A vehicle body's shader group is not one this code can read -- thirty-two
// byte shaders picked by vtable, no name pointer -- but the sibling .xtl/.xtp
// pack states it plainly: its materials are in shader-index order and each
// holds the effect number at +12. That is the whole reason a car mod can say
// "this material is paint" and have it land on the shader that paints.
bool ReadPackEffects(const Rsc5Resource& pack, std::vector<uint32_t>& effects);

// The name of car effect `index`, or an empty string when it is out of range.
const char* CarEffectName(uint32_t index);

// The index of the car effect called `name`, case-insensitively, or -1.
int32_t CarEffectIndex(const std::string& name);

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

// The vertex stride each shader of the drawable's high LOD draws with, by
// shader index, and zero for a shader with no submesh in that LOD.
//
// The room a body may grow into is measured in bytes, and a triangle does not
// cost the same on every shader. Measured on the police Caprice's body_lod_0:
// BlackMatte is drawn from position-only submeshes of twelve bytes a vertex and
// every other shader from POS/NRM/UV0/UV1/TAN at twenty-eight, so the same
// triangle count is nearly twice the bytes on one as on the other.
//
// The split is weighted by triangles, which means a `weight.<effect>` is not a
// share of the room -- it is a share of the room per triangle, and it silently
// changes meaning the moment a material moves between shaders. That is not a
// hypothetical: this car's cabin moved from BlackMatte to BumpSpecAlpha under
// the same 0.25 and came back at 4535 triangles instead of 20,859. Reporting
// the stride beside the triangle count is what makes that visible in the log
// rather than only in the game.
// With `only_model` set, only that model of the high LOD is read: which shaders
// one body panel's model draws, and at what stride.
bool ShaderVertexStrides(const Rsc5Resource& resource, std::vector<uint32_t>& stride_of,
                         int32_t only_model = -1);

// One material of a vehicle's material pack, in shader-index order.
//
// A car is not one surface and its textures are not one image: the pack states,
// per shader, which effect draws it and which of the dictionary's textures that
// effect reads. Both halves are needed to give a replacement body its own skin
// -- the effect says whether the shader samples anything at all (paint, glass,
// black matte and chrome sample nothing, and no amount of writing gives them a
// picture), and the texture says where the picture goes.
struct PackMaterial {
    uint32_t effect = 0;        // index into the game's car shader list
    uint32_t address = 0;       // the material struct
    uint32_t diffuse = 0;       // the texture its colour map reads, 0 for none
    std::string diffuse_name;
    uint32_t width = 0;         // of that texture, as the pack stores it
    uint32_t height = 0;
    bool writable = false;      // the texture is there and in a format this can encode
};

// Every material of `pack`, in the order the drawable's shader indices use.
bool ReadPackMaterials(const Rsc5Resource& pack, std::vector<PackMaterial>& out);

// Writes `image` over the colour map of pack material `material_index`.
//
// The same write ReplaceShaderDiffuse performs, addressed through a material
// pack instead of a drawable's shader group: resampled to the size the shipped
// texture already is, encoded in the format it already declares, tiled, and
// dropped at the address it already lives at, mip chain included. Nothing grows.
//
// Two companions of the colour map are handled with it. A normal map (its name
// ends in _n) is flattened, because it is still the donor's and the mesh no
// longer has the donor's UVs. An illuminated variant (the colour map's name plus
// _i, which is how a car's lights carry their lit state) is written with the
// same image, so a lamp does not change picture when it comes on.
//
// With `growth`, a colour map smaller than `image` is not squeezed: it is moved
// to room of its own at the end of the virtual segment, at the image's size,
// and its grcTexture and fetch constant are pointed there. Every car pack the
// game ships is 3.3-3.5 MB and every cabin texture in one is 256 at most, so a
// mod's 1024 cabin sheet came out at a sixteenth of its texels. Call
// FinishPackGrowth once after the last write.
struct PackGrowth {
    uint32_t end = 0;        // first free byte past what was placed, 0 = none yet
    uint32_t textures = 0;   // how many were moved
    uint32_t bytes = 0;      // what they took, mips included
};
bool ReplacePackMaterialDiffuse(Rsc5Resource& pack, uint32_t material_index, const Image& image,
                                std::string& error, TextureStats* stats = nullptr,
                                PackGrowth* growth = nullptr);

// Leaves a pack that had textures moved into it in the shape the streamer needs:
// the last texture stopped well short of the segment's end, and the segment a
// whole number of blocks (see RoundVirtualToBlock). A no-op when nothing moved.
bool FinishPackGrowth(Rsc5Resource& pack, const PackGrowth& growth, std::string& error);

}  // namespace mc::modloader
