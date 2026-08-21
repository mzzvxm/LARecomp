// BadassBaboon's Recomp Adjustments: RAGE engine and MCLA guest type definitions
// Reverse-engineered structure layouts and verified offsets from CodeX (RSC5).

#pragma once

#include <cstdint>
#include <cstddef>
#include <bit>
#include <cstring>
#include "mcla_symbol_resolver.h"

namespace rage {

// ---------------------------------------------------------------------------
// Big-endian guest memory access helpers
// ---------------------------------------------------------------------------

inline float ReadBEFloat(const uint8_t* ptr) {
  uint32_t val;
  std::memcpy(&val, ptr, sizeof(val));
  val = std::byteswap(val);
  float f;
  std::memcpy(&f, &val, sizeof(f));
  return f;
}

inline void WriteBEFloat(uint8_t* ptr, float value) {
  uint32_t val;
  std::memcpy(&val, &value, sizeof(val));
  val = std::byteswap(val);
  std::memcpy(ptr, &val, sizeof(val));
}

inline uint32_t ReadBEUInt32(const uint8_t* ptr) {
  uint32_t val;
  std::memcpy(&val, ptr, sizeof(val));
  return std::byteswap(val);
}

inline void WriteBEUInt32(uint8_t* ptr, uint32_t value) {
  uint32_t val = std::byteswap(value);
  std::memcpy(ptr, &val, sizeof(val));
}

// ---------------------------------------------------------------------------
// RAGE Vector and Bounding Box Types (RSC5)
// ---------------------------------------------------------------------------

struct Vector3BE {
  float x;
  float y;
  float z;

  float GetX() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&x)); }
  float GetY() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&y)); }
  float GetZ() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&z)); }

  void Set(float nx, float ny, float nz) {
    WriteBEFloat(reinterpret_cast<uint8_t*>(&x), nx);
    WriteBEFloat(reinterpret_cast<uint8_t*>(&y), ny);
    WriteBEFloat(reinterpret_cast<uint8_t*>(&z), nz);
  }
};

struct Vector4BE {
  float x;
  float y;
  float z;
  float w;

  float GetX() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&x)); }
  float GetY() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&y)); }
  float GetZ() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&z)); }
  float GetW() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&w)); }

  void Set(float nx, float ny, float nz, float nw) {
    WriteBEFloat(reinterpret_cast<uint8_t*>(&x), nx);
    WriteBEFloat(reinterpret_cast<uint8_t*>(&y), ny);
    WriteBEFloat(reinterpret_cast<uint8_t*>(&z), nz);
    WriteBEFloat(reinterpret_cast<uint8_t*>(&w), nw);
  }
};

struct AABB_BE {
  Vector4BE min;
  Vector4BE max;
};

// ---------------------------------------------------------------------------
// MCLA Game Engine Structures (Reconstructed from RSC5 / CodeX)
// ---------------------------------------------------------------------------

// mcAmbientDensityTuning - dynamic traffic, crowd and parked-vehicle spawning.
// Reconstructed from the sub_826F5B18 constructor.
//
// OFFSETS ARE STATIC-ASSERTED BELOW. They were previously wrong and nothing
// caught it: the comments claimed ped_density at +0x5C and parked_factor at
// +0x98, but `pad_18[72]` starting at +0x18 actually placed them at +0x60 and
// +0x9C. ped_density got away with it (+0x60 happens to be a real pedestrian
// value, 15.0), but +0x9C is never written by the constructor, so
// MCLA_PARKED_CAR_SCALE was scaling uninitialised memory and did nothing at all
// - it logged "parked_factor=-0.00 (was -0.00)" every time.
//
// Constructor stores, verified in IDA:
//   +0x08 = flt_82018B1C = 180.0     spawn_max
//   +0x10 = flt_8201CD44 = 400.0     unspawn_max
//   +0x14 = flt_8204E11C = 700.0     cull_max
//   +0x60 = flt_82007F9C = 15.0      ped_density
//   +0x98 = flt_82008DD0 = 0.25      parked_factor  (last store in the ctor)
struct mcAmbientDensityTuning {
  uint32_t vtable;             // +0x00
  uint32_t unk_04;             // +0x04
  float spawn_max;             // +0x08
  uint32_t unk_0c;             // +0x0C
  float unspawn_max;           // +0x10
  float cull_max;              // +0x14
  uint8_t pad_18[72];          // +0x18 .. +0x5F
  float ped_density;           // +0x60
  uint8_t pad_64[52];          // +0x64 .. +0x97
  float parked_factor;         // +0x98

  float GetSpawnMax() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&spawn_max)); }
  float GetUnspawnMax() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&unspawn_max)); }
  float GetCullMax() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&cull_max)); }
  float GetPedDensity() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&ped_density)); }
  float GetParkedFactor() const { return ReadBEFloat(reinterpret_cast<const uint8_t*>(&parked_factor)); }

  void SetSpawnMax(float v) { WriteBEFloat(reinterpret_cast<uint8_t*>(&spawn_max), v); }
  void SetUnspawnMax(float v) { WriteBEFloat(reinterpret_cast<uint8_t*>(&unspawn_max), v); }
  void SetCullMax(float v) { WriteBEFloat(reinterpret_cast<uint8_t*>(&cull_max), v); }
  void SetPedDensity(float v) { WriteBEFloat(reinterpret_cast<uint8_t*>(&ped_density), v); }
  void SetParkedFactor(float v) { WriteBEFloat(reinterpret_cast<uint8_t*>(&parked_factor), v); }
};



// mcDofObject - Depth of Field composite parameters
// Reconstructed from sub_8260EBB8
struct mcDofObject {
  uint8_t pad_00[0xF0];        // +0x00..0xEF (0..239)
  // +0xF0 - VERIFIED: uploaded as a 16-byte float4 shader constant by
  // sub_8260EBB8 (addi r29,r31,0xF0 -> sub_8218A6E0 with size 0x10).
  Vector4BE coc_vector;

  void ZeroCoC() {
    coc_vector.Set(0.0f, 0.0f, 0.0f, 0.0f);
  }
};

// rage::grmCitySector / Rsc5CityMapSector
// VFT: 0x825CAF3C. Represents a streaming city grid block in MCLA (.xcs / .xct)
// Size: exactly 304 bytes (0x130)
struct grmCitySector {
  uint8_t pad_00[0xD0];        // +0x00..0xCF (0..207) - Sector header and entity tables
  Vector4BE aabb_min;          // +0xD0 (208) - Sector world bounding box minimum
  Vector4BE aabb_max;          // +0xE0 (224) - Sector world bounding box maximum
  uint32_t name_ptr;           // +0xF0 (240) - Pointer to ASCII sector name
  uint8_t pad_f4[0x3C];        // +0xF4..0x12F (244..303)
};

// mcCity - City & Sector Streaming Manager Singleton (at 0x827E0DC8)
// Reconstructed from RSC5 Rsc5City and sub_822D5A30
struct mcCity {
  uint32_t vtable;             // +0x00 (0) - 0x825CAF3C
  uint32_t map_bounds_ptr;     // +0x04 (4) - Pointer to Rsc5CityBounds
  uint32_t unk_08;             // +0x08 (8)
  uint32_t sector_count;       // +0x0C (12) - Number of sectors in city
  uint32_t unk_10;             // +0x10 (16)
  uint32_t sectors_arr_ptr;    // +0x14 (20) - Pointer to grmCitySector array

  uint32_t GetSectorCount() const { return ReadBEUInt32(reinterpret_cast<const uint8_t*>(&sector_count)); }
  uint32_t GetSectorsArrayPtr() const { return ReadBEUInt32(reinterpret_cast<const uint8_t*>(&sectors_arr_ptr)); }
};

// ---------------------------------------------------------------------------
// RAGE Texture System & Formats (RSC5 Rsc5Texture.cs)
// ---------------------------------------------------------------------------

enum class grcTextureFormat : uint32_t {
  D3DFMT_L8       = 2,
  D3DFMT_DXT1     = 82,  // BC1 - Standard diffuse with 1-bit alpha
  D3DFMT_DXT3     = 83,  // BC2 - Explicit alpha
  D3DFMT_DXT5     = 84,  // BC3 - Interpolated alpha
  D3DFMT_CTX1     = 113, // 3Dc / ATI2N / CTX1 - Xbox 360 two-channel normal maps
  D3DFMT_A8R8G8B8 = 134  // 32-bit uncompressed RGBA
};

// rage::grcTexture / Rsc5Texture
// Texture descriptor mapping into Xenos memory and D3D resources
struct grcTexture {
  uint32_t vtable;             // +0x00 (0) - 0x82516D84
  uint32_t unk_04;             // +0x04 (4)
  uint32_t unk_08;             // +0x08 (8)
  uint32_t unk_0c;             // +0x0C (12)
  uint32_t unk_10;             // +0x10 (16)
  uint32_t unk_14;             // +0x14 (20)
  uint32_t name_ptr;           // +0x18 (24) - Pointer to ASCII / DDS name
  uint32_t d3d_base_ptr;       // +0x1C (28) - Pointer to D3DBaseTexture block
  uint16_t width;              // +0x20 (32) - Texture pixel width
  uint16_t height;             // +0x22 (34) - Texture pixel height
  uint16_t stride;             // +0x24 (36) - Row stride in bytes
  uint8_t  texture_type;       // +0x26 (38) - 0 = 2D, 1 = Cube, 3 = Volume
  uint8_t  mip_levels;         // +0x27 (39) - Mipmap count
  float    color_exp_r;        // +0x28 (40)
  float    color_exp_g;        // +0x2C (44)
  float    color_exp_b;        // +0x30 (48)
  float    color_ofs_r;        // +0x34 (52)
  float    color_ofs_g;        // +0x38 (56)
  float    color_ofs_b;        // +0x3C (60)

  uint16_t GetWidth() const { return std::byteswap(width); }
  uint16_t GetHeight() const { return std::byteswap(height); }
  uint16_t GetStride() const { return std::byteswap(stride); }
};

// rage::pgDictionary<rage::grcTexture> / Rsc5TextureDictionary
// Container holding hashed texture entries and pointer array (.xtd / .xtl)
struct pgTextureDictionary {
  uint32_t vtable;             // +0x00 (0)
  uint32_t parent_dict_ptr;    // +0x04 (4)
  uint32_t usage_count;        // +0x08 (8)
  uint32_t hashes_arr_ptr;     // +0x0C (12) - Array of JenkHash identifiers
  uint32_t hashes_count;       // +0x10 (16)
  uint32_t textures_arr_ptr;   // +0x14 (20) - Array of pointers to grcTexture
  uint32_t textures_count;     // +0x18 (24)

  uint32_t GetTextureCount() const { return ReadBEUInt32(reinterpret_cast<const uint8_t*>(&textures_count)); }
  uint32_t GetHashesArrayPtr() const { return ReadBEUInt32(reinterpret_cast<const uint8_t*>(&hashes_arr_ptr)); }
  uint32_t GetTexturesArrayPtr() const { return ReadBEUInt32(reinterpret_cast<const uint8_t*>(&textures_arr_ptr)); }
};

// Known Global Memory Addresses in MCLA
constexpr uint32_t kCityManagerGlobalPtr   = 0x827E0DC8;
constexpr uint32_t kBaseLodDistanceAddr    = 0x827E0DE0; // float: 300.0f stock
constexpr uint32_t kActiveLodDistanceAddr  = 0x827E0E50; // float: dynamic scaled distance
constexpr uint32_t kLodSpeedScaleAddr      = 0x827E0DEC; // float: velocity-interpolated factor


// ---------------------------------------------------------------------------
// Guest struct offset assertions.
//
// These exist because mcAmbientDensityTuning silently had TWO wrong offsets:
// pad_18[72] starting at +0x18 pushed ped_density to +0x60 and parked_factor to
// +0x9C, while the comments claimed +0x5C and +0x98. Nothing caught it -
// parked_factor read memory the constructor never writes, so
// MCLA_PARKED_CAR_SCALE scaled nothing for the entire life of the feature and
// logged "-0.00 (was -0.00)" every time.
//
// WHAT THESE PROVE: the C++ layout matches the offsets in the comments. That is
// the padding-arithmetic bug class, now impossible to reintroduce silently.
//
// WHAT THEY DO NOT PROVE: that an offset is the right field in the guest. Only
// reverse engineering establishes that. Verification status is per struct
// below - never treat an assert as evidence of semantic correctness.
// ---------------------------------------------------------------------------

// VERIFIED against the sub_826F5B18 constructor.
static_assert(offsetof(mcAmbientDensityTuning, spawn_max)     == 0x08);
static_assert(offsetof(mcAmbientDensityTuning, unspawn_max)   == 0x10);
static_assert(offsetof(mcAmbientDensityTuning, cull_max)      == 0x14);
static_assert(offsetof(mcAmbientDensityTuning, ped_density)   == 0x60);
static_assert(offsetof(mcAmbientDensityTuning, parked_factor) == 0x98);

// VERIFIED in sub_8260EBB8. Not accessed by displacement, which is why an
// earlier audit grepping for "0xF0(r31)" wrongly concluded it was unused. It is
// taken by address and uploaded as a shader constant:
//
//   8260ed20  lwz  r11, 0x80(r31)    ; shader / effect object
//   8260ed24  addi r29, r31, 0xF0    ; &coc_vector
//   8260ed34  li   r7, 0x10          ; 16 bytes = one float4
//   8260ed38  mr   r6, r29           ; data pointer
//   8260ed44  bl   sub_8218A6E0      ; shader parameter setter
//
// sub_8218A6E0 indexes a parameter table and scales by 16 (rotlwi r6,4 - the
// float4 constant-register stride), confirming a float4 shader constant. So
// zeroing it genuinely feeds the DoF shader a zero blur vector.
static_assert(sizeof(Vector4BE) == 16);
static_assert(offsetof(mcDofObject, coc_vector) == 0xF0);

// UNVERIFIED against the binary - documentation only, not read or written by
// any hook today. The asserts pin the C++ layout so it cannot drift, but
// confirm the offsets in IDA before using any of these at runtime.
static_assert(offsetof(grmCitySector, aabb_min) == 0xD0);
static_assert(offsetof(grmCitySector, aabb_max) == 0xE0);
static_assert(offsetof(grmCitySector, name_ptr) == 0xF0);
static_assert(sizeof(grmCitySector) == 0x130);

static_assert(offsetof(mcCity, map_bounds_ptr)  == 0x04);
static_assert(offsetof(mcCity, sector_count)    == 0x0C);
static_assert(offsetof(mcCity, sectors_arr_ptr) == 0x14);

static_assert(offsetof(grcTexture, name_ptr)     == 0x18);
static_assert(offsetof(grcTexture, d3d_base_ptr) == 0x1C);
static_assert(offsetof(grcTexture, width)        == 0x20);
static_assert(offsetof(grcTexture, height)       == 0x22);
static_assert(offsetof(grcTexture, stride)       == 0x24);
static_assert(offsetof(grcTexture, mip_levels)   == 0x27);
static_assert(offsetof(grcTexture, color_exp_r)  == 0x28);

} // namespace rage

