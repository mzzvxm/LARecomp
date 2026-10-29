#pragma once
// ===========================================================================
// MCLA GPU renderer -- the names of the guest's textures
// ===========================================================================
// What a texture is called in the game's own data, for the dumps (texture
// mods name their files after it). Recorded from the three grcTextureXenon
// hooks in native_gfx/hooks.cpp, whichever renderer runs:
//
//   grcTextureXenon::Init (sub_821841B0)        r4 = the name; made at run
//                                               time, no dictionary.
//   grcTextureXenon(datResource&) (sub_82184458) the name at grc+24 (fixed up
//                                               by the constructor), the
//                                               dictionary = the datResource
//                                               the thread is fixing up:
//                                               *(*(r13) + 60), its name at +8
//                                               (sub_8218B708 prints it as
//                                               "resource '%s'").
//   ~grcTextureXenon (sub_82184ED0)             before the original: forgotten.
//
// Keyed by the physical page of the texture's base level (its D3DTexture at
// grc+28, fetch constant at D3DTexture+28), which is what TextureKey carries.
//
// Threading: written from the game's streaming threads, read from the render
// thread; one mutex.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::gpu::texture_names {

struct Name {
  std::string texture;     // "unnamed" when the game gave none
  std::string dictionary;  // file stem of the resource, "runtime" for Init's
};

void NoteInit(const uint8_t* base, uint32_t grc_texture, uint32_t name_va);
// `r13`: the calling thread's r13, for the datResource being fixed up.
void NoteResource(const uint8_t* base, uint32_t grc_texture, uint32_t r13);
void NoteDestroyed(const uint8_t* base, uint32_t grc_texture);

// The name of the texture whose base level is at physical page `base_page`;
// false when none is known.
bool Lookup(uint32_t base_page, Name* out);

// A string reduced to what a file or folder name can hold everywhere:
// [A-Za-z0-9._-], the rest '_'; empty becomes `fallback`.
std::string FileSafe(const std::string& s, const char* fallback);

}  // namespace mcla::gpu::texture_names
