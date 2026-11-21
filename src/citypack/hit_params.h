#pragma once
// ===========================================================================
// LARecomp city pack -- the parameters of a shader instance (CITY-RT M2, package H1)
// ===========================================================================
// A sector's shader group holds one INSTANCE of an effect for each material (CityBumpSpec / CityBumpSpec_Cloth): the effect's parameters with the values this material gives
// them. The hit data needs three kinds of them: the DIFFUSE TEXTURE (a texture's file name: the instance names its texture, the dictionaries hold them by hash), the TINT and the
// glow of the shaders that have one, and the SpecularExponent that the raster G-buffer turns into the smoothness (gbuffer_material.h). The layout was read from the player's own game
// files (hw_ss_07 and all 439 sectors; all fields big endian, pointers are the resource's own: rsc_image.h):
//
//   shader (one of the group's array)   +16 pointer to the VALUES: one pointer a parameter
//                                       +24 u16 parameter count
//                                       +28 pointer to the TYPES, one byte a parameter: 0 = a texture, 1 = a float4
//                                       +36 pointer to the parameters' NAME HASHES, one u32 a parameter: the rage hash (material_class.h RageHashText) of the parameter's
//                                           own name in the effect, "DiffuseSampler", "SpecularExponent", "DiffuseColor", ... (the names are the translated shaders' own: 142 of
//                                           them are known, the 1,748 textures' hash table is not needed to read a material)
//                                       +48 pointer to the group's name, +60 pointer to the preset's name (sector_parse.h)
//   a texture parameter                 the value is a pointer to a texture OBJECT (null: the material leaves the slot empty), whose +24 is a pointer to the file name
//                                       ("wall_brick_01.dds"); the object holds no pixels (the city's textures are in the dictionaries common.xtd and lod_0.xtd)
//   a float4 parameter                  the value is a pointer to four big endian floats; the scalar parameters (SpecularExponent, NeonIntensity ...) are the x of one; a colour
//                                       is rgb (w is a NaN pattern 0x7F800001 where the game gives none)
//
// The parameters are found BY NAME HASH, never by position: the order is the effect's declaration order and differs between presets of one group.
//
// CHECK (C9 of perf/CITY-RT-M2.md, settled: the parameter's hash names it, so the order of the fetch slots does not matter): hw_ss_07 and the census of all 439 sectors read 54,843
// instances with 151,779 texture and 260,267 float parameters; every texture parameter is a pointer to an object with a name or is null.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "citypack/rsc_image.h"

namespace larecomp::citypack {

// The rage hash of a name (lower case, '\\' as '/', Jenkins one-at-a-time: material_class.h RageHashText), at compile time.
constexpr uint32_t RageHashConstexpr(std::string_view text) {
  uint32_t h = 0;
  for (const char raw : text) {
    uint32_t ch = static_cast<uint8_t>(raw);
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (ch == '\\') ch = '/';
    h += ch;
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

// The parameters the hit data reads (hashes verified against the retail game's instances by the static asserts below and by hit_builder_test.cpp).
inline constexpr uint32_t kParamDiffuseSampler = RageHashConstexpr("DiffuseSampler");        // the surface's texture (almost every lit material)
inline constexpr uint32_t kParamDiffuseSamplerA = RageHashConstexpr("DiffuseSamplerA");      // the terrain's and the grass's first layer (they have no DiffuseSampler)
inline constexpr uint32_t kParamSpecularExponent = RageHashConstexpr("SpecularExponent");    // x: the Blinn-Phong exponent (gbuffer_material.h)
inline constexpr uint32_t kParamDiffuseColor = RageHashConstexpr("DiffuseColor");            // rgb: the flat colour of the tinted materials (CityGrimeMetalTint ...)
inline constexpr uint32_t kParamTintColor = RageHashConstexpr("TintColor");                  // rgb: CityTintBumpDecal2's tint
inline constexpr uint32_t kParamWindowTintColor = RageHashConstexpr("WindowTintColor");      // rgb: the colour of a window's glass by day
inline constexpr uint32_t kParamNightColor = RageHashConstexpr("NightColor");                // rgb: what a CityNeonTint glows with
inline constexpr uint32_t kParamDayColor = RageHashConstexpr("DayColor");                    // rgb: what it looks like by day
inline constexpr uint32_t kParamNeonIntensity = RageHashConstexpr("NeonIntensity");          // x: the glow of CityNeon
inline constexpr uint32_t kParamNightIntensity = RageHashConstexpr("NightIntensity");        // x: the glow of CityNeonTint and CityStoreSign
static_assert(kParamDiffuseSampler == 0xF1FE2B71u && kParamSpecularExponent == 0x93D520E8u && kParamDiffuseColor == 0xC194EF36u && kParamTintColor == 0x91299309u &&
                  kParamWindowTintColor == 0x95B09A44u && kParamDayColor == 0xCB39572Bu && kParamNightColor == 0x7E2FF13Eu && kParamNeonIntensity == 0x2FED46C3u &&
                  kParamNightIntensity == 0xCEBC7801u && kParamDiffuseSamplerA == 0xC9A79FEDu,
              "the parameter names hash to the numbers the game's instances hold");

inline constexpr uint8_t kParamTypeTexture = 0;
inline constexpr uint8_t kParamTypeFloat4 = 1;

struct ShaderParam {
  uint32_t hash = 0;
  uint8_t type = kParamTypeFloat4;
  bool empty = false;    // a texture slot with no texture (a null value), or a value that could not be read
  float value[4] = {0, 0, 0, 0};
  std::string texture;   // type 0: the file name as the resource has it ("wall_brick_01.dds"), "" when empty
};

struct ShaderParams {
  std::vector<ShaderParam> params;
  const ShaderParam* Find(uint32_t hash) const {
    for (const ShaderParam& p : params) {
      if (p.hash == hash) return &p;
    }
    return nullptr;
  }
};

// The parameters of the shader instance at `shader` (a pointer in the image). False when the instance does not have the layout above (no values, no types); parameters that cannot be
// read are kept with `empty` set. At most 64 parameters are read (the city's most is 13).
bool ReadShaderParams(RscCursor& cursor, uint32_t shader, ShaderParams* out);

// "Wall_Brick.DDS" -> "wall_brick": the name the texture dictionaries are searched by (lower case, no extension).
std::string TextureStem(std::string_view file_name);

}  // namespace larecomp::citypack
