#pragma once
// ===========================================================================
// LARecomp city pack -- the city's texture dictionaries (CITY-RT M2 package H2)
// ===========================================================================
// Read from the player's own files (checked on all 441 of them; CITY-RT-M2 section 3.4 expected two dictionaries and "the third", which is NOT one):
//
//   resources/city/sc/common.xtd   a dictionary: 508 textures, 31 MB, with no mips on 10 of them
//   resources/city/sc/lod_0.xtd    a dictionary: 1,240 textures, 36 MB, 1,058 of them single level
//   resources/city/sc/<sector>.xcs the sector's OWN dictionary, at the resource root + 8 (the model dictionary is the drawable's): 39,236 entries over the 439 sectors (a sector has
//                                  from a few to 252 textures, up to 6.2 MB); with the other two, 8,421 distinct names. The shaders of a sector name their textures
//                                  ("l_be_sb_ground_sand_01.dds": the grcTexture a shader holds has the name and no data) and ALL of those names resolve (89,895 over the sectors: 39,235 in the
//                                  sector's own dictionary, 50,660 in common.xtd). A name is the same picture in every dictionary that has it, bar a few dozen that are different pictures.
//   the 14.5 MB resource beside them (hash 3e5fd91d, resource type 1) is NOT a texture dictionary: no grcTexture in it, its root holds two counts of 20,000 and arrays of floats
//                                  (CHECK C4 of the plan: answered, it is not textures; the type 26 and type 1086 files beside it are not either)
//
// A dictionary is a pgDictionary: +16 pointer to the u32 name hashes, +20 u16 count, +24 pointer to the grcTexture pointer array. The hash is the rage hash of the texture's file name WITH
// its extension ("l_be_sb_ground_sand_01.dds"; the grcTexture's own name pointer is empty in a dictionary: the names were stripped). A grcTexture: +28 pointer to the D3DBaseTexture,
// whose fetch constant (six dwords, big endian) is at +0x1C; +32 u16 width and +34 u16 height (the same as the fetch constant's). Everything is in the resource's virtual segment
// (pointers 0x5xxxxxxx; the physical segment of these files is empty).
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "citypack/rsc_image.h"
#include "citypack/xenos_texture.h"

namespace larecomp::citypack {

inline constexpr const char* kCommonDictionaryPath = "resources/city/sc/common.xtd";
inline constexpr const char* kLodDictionaryPath = "resources/city/sc/lod_0.xtd";

// The hash a dictionary and a shader name a texture by: the rage hash of "<name>.dds" lowercased ("L_Be.DDS" and "l_be.dds" are the same). `name` may or may not carry ".dds".
uint32_t TextureNameHash(std::string_view name);

struct DictionaryTexture {
  uint32_t name_hash = 0;
  uint32_t address = 0;          // the grcTexture's pointer (for messages)
  uint32_t grc_width = 0;        // the grcTexture's own size, which must equal the fetch constant's
  uint32_t grc_height = 0;
  xenos::TextureDesc desc;
};

struct DictionaryStats {
  uint32_t entries = 0;                 // the dictionary's count
  uint32_t textures = 0;                // read
  uint32_t bad_pointer = 0;             // a grcTexture, a fetch constant or a hash that is outside the resource
  uint32_t not_supported = 0;           // the fetch constant says 1D, 3D, cube, stacked ... (DescribeTexture)
  uint32_t unsupported_format = 0;      // a format this reader does not decode (per format below)
  uint32_t no_base_level = 0;           // a texture whose base level is not stored (mip_min_level > 0)
  uint32_t size_mismatch = 0;           // the grcTexture's size differs from the fetch constant's (the fetch constant wins)
  uint32_t not_tiled = 0;
  uint32_t odd_swizzle = 0;             // a swizzle that is not r g b a
  uint32_t odd_endian = 0;              // another endian than 8in16
  std::vector<uint32_t> unsupported_formats;  // the codes seen
  void Merge(const DictionaryStats& other);
};

// The dictionary a resource holds at `dictionary_pointer`. False (with `error`) when there is no dictionary there (no pointer, no array, a count of zero or past what the image holds).
bool ParseDictionaryAt(const RscImage& image, uint32_t dictionary_pointer, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error);
// common.xtd and lod_0.xtd: the dictionary IS the resource's root.
bool ParseDictionaryFile(const RscImage& image, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error);
// A sector (.xcs): the dictionary at the root's +8.
bool ParseSectorDictionary(const RscImage& image, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error);

// The bytes of the resource from `pointer` (0x5.. virtual, 0x6.. physical) to the end of the image; null (size 0) for a pointer outside it.
const uint8_t* ResourceBytesAt(const RscImage& image, uint32_t pointer, size_t* size);

}  // namespace larecomp::citypack
