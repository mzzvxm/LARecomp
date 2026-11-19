#pragma once
//
// A sprite of another movie's .xsf, brought into the movies the city has
// loaded..
//
// The game loads a .xsf as one resource and then fixes it up character by
// character (swfOBJECT::Fixup, sub_825FDA90): every pointer inside is moved
// through the resource's page map and every object gets its class's vtable.
// Nothing else is done to it at load; the characters are plain data the swf
// player walks. So a sprite can be lifted out of a movie that is not loaded:
// the bytes of every character it uses are read from the shipped archive,
// copied into guest memory, and fixed up by that same function against a page
// map that covers the copy. What comes out is the movie's own sprite -- its
// timeline, shapes, gradients and animation -- for swf::Place in any movie.
//
// One thing ties a sprite to the movie it came from at run time: its
// PlaceObject2 commands name what they place by index into the current file's
// table (*(dword_828FF990 + 24), sub_8260B038), and a movie's update points
// that global at its own file. So while an imported instance updates, the
// global points at a stand-in file whose table holds the imported characters
// (the rex_sub_825FB0D8 wrapper). Drawing never reads it for shapes and
// sprites, the only kinds imported.
//
// Guest thread only.

#include <cstdint>

#include <rex/ppc/context.h>

namespace larecomp::swf {

struct ImportedSprite {
    uint32_t character = 0;  // the sprite, ready for swf::Place
    uint32_t file = 0;       // the stand-in swfFILE its commands resolve against
};

// Imports character `index` of the movie `archive_path` (archive-relative, in
// the shipped xarchive_cache.rpf) together with every character it uses. The
// first call does the work and logs it; later calls return that result. Null
// when it cannot be done -- the log says why.
const ImportedSprite* ImportSprite(const char* archive_path, int index);

// Timeline command of an imported sprite: in frame `frame`, the one of `type`
// (0 PlaceObject2, 1 named PlaceObject2, 2 RemoveObject2) acting on `depth`.
// 0 if there is none.
uint32_t FrameCommand(const uint8_t* base, uint32_t sprite, int frame, int type, uint16_t depth);

// Called inside the movie's update, right before an instance of `sprite`
// updates: the movie is bound, mid-update, and the imported file is current --
// the same point of the frame at which that sprite's own timeline runs. One
// callback per sprite.
using BeforeUpdateFn = void (*)(PPCContext& ctx, uint8_t* base, uint32_t inst);
void SetBeforeUpdate(uint32_t sprite, BeforeUpdateFn fn);

// Inside a BeforeUpdate callback only: runs the timeline command `cmd` on
// `parent`, the call the sprite's own timeline makes (command vtable +4).
void RunCommand(PPCContext& ctx, uint8_t* base, uint32_t cmd, uint32_t parent);

// Guest memory that lives for the session, for the strings and the small
// argument blocks guest calls take by pointer. 0 when out of room.
uint32_t GuestScratch(uint32_t bytes);
uint32_t GuestString(const char* text);

}  // namespace larecomp::swf
