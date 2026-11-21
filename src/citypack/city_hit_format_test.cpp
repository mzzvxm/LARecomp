// Check of the city hit data's interface (city_hit_format.h and .cpp, hlsl/city_hit_common.hlsli): CITY-RT M2 package H0. A program of its own: it needs no renderer, no game files
// and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_city_hit_format_test (src/citypack/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src -I src/gpu src/citypack/city_hit_format_test.cpp src/citypack/city_hit_format.cpp src/citypack/pack_format.cpp
//   run: larecomp_city_hit_format_test [<hlsl/city_hit_common.hlsli>]   (without the argument the .hlsli is found from this file's own place: ../gpu/hlsl/)
//
// It checks: the material word against gbuffer_types.h (the same layout, every shift and flag), the instance slot against city_rt_rules.h, the float16 helpers (every half, the directed
// roundings), the R10G10B10 colours against the cars' formula, the records (material, texture, run) packed and unpacked, the UV transform's coverage and error, the corner streams (both
// encodings, damage), the hit blob (partition rules, hash, flags), the file's headers and sections, the pool block's plan and writer; and then compiles the .hlsli ITSELF as C++ (through a
// shim of the HLSL vector types and of the two buffer types, which also checks that every load is 4 byte aligned and inside the buffer) and decodes the hits of a synthetic pool
// that the C++ writers made: against the C++ twin (DecodeHit) and against the data that went in. Every #define of the .hlsli is compared with the C++ value.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "city_rt_rules.h"  // src/gpu: the instance id the structures carry
#include "gbuffer_types.h"  // src/gpu: the material word

using namespace larecomp::citypack;
namespace gb = mcla::gpu::gbuffer;

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct uint2 {
  uint x = 0, y = 0;
  uint2() = default;
  uint2(uint a, uint b) : x(a), y(b) {}
};
struct uint3 {
  uint x = 0, y = 0, z = 0;
  uint3() = default;
  uint3(uint a, uint b, uint c) : x(a), y(b), z(c) {}
};
struct uint4 {
  uint x = 0, y = 0, z = 0, w = 0;
  uint4() = default;
  uint4(uint a, uint b, uint c, uint d) : x(a), y(b), z(c), w(d) {}
};
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float2 operator+(float2 a, float2 b) { return float2(a.x + b.x, a.y + b.y); }
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float4 operator+(float4 a, float4 b) { return float4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
inline float4 operator*(float4 a, float s) { return float4(a.x * s, a.y * s, a.z * s, a.w * s); }
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
inline float3 asfloat(uint3 v) { return float3(asfloat(v.x), asfloat(v.y), asfloat(v.z)); }
// independent of the C++ twin's bit manipulation: the value of a half by its definition
inline float f16tof32(uint v) {
  const int exponent = int((v >> 10) & 31u);
  const int mantissa = int(v & 1023u);
  const float sign = (v & 0x8000u) != 0 ? -1.0f : 1.0f;
  if (exponent == 0) return sign * std::ldexp(float(mantissa), -24);
  if (exponent == 31) return sign * INFINITY;
  return sign * std::ldexp(float(1024 + mantissa), exponent - 25);
}

// ByteAddressBuffer: loads must be 4 byte aligned and inside; a violation is counted (the shader's buffer would return 0 or fault)
struct ByteAddressBuffer {
  const uint8_t* data = nullptr;
  size_t size = 0;
  mutable int errors = 0;
  uint Load(uint offset) const {
    if ((offset & 3u) != 0 || size_t(offset) + 4 > size) {
      ++errors;
      return 0;
    }
    return uint(data[offset]) | (uint(data[offset + 1]) << 8) | (uint(data[offset + 2]) << 16) | (uint(data[offset + 3]) << 24);
  }
  uint2 Load2(uint offset) const { return uint2(Load(offset), Load(offset + 4)); }
  uint3 Load3(uint offset) const { return uint3(Load(offset), Load(offset + 4), Load(offset + 8)); }
  uint4 Load4(uint offset) const { return uint4(Load(offset), Load(offset + 4), Load(offset + 8), Load(offset + 12)); }
};
template <typename T>
struct StructuredBuffer {
  const T* data = nullptr;
  size_t count = 0;
  mutable int errors = 0;
  T operator[](uint index) const {
    if (size_t(index) >= count) {
      ++errors;
      return T();
    }
    return data[index];
  }
};

ByteAddressBuffer gCityPool;
StructuredBuffer<uint4> gCitySectors;
StructuredBuffer<uint4> gCityMaterials;
StructuredBuffer<uint4> gCityTextures;
#define CITY_HIT_NO_RESOURCES
#include "hlsl/city_hit_common.hlsli"
}  // namespace hlsl_shim

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }  // 0 .. 1
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : (U32() >> 8) % n; }
};

bool NearlyEqual(double a, double b, double abs_tol, double rel_tol) { return std::fabs(a - b) <= abs_tol + rel_tol * std::max(std::fabs(a), std::fabs(b)); }

// ---------------------------------------------------------------------------
// the numbers shared with other headers
// ---------------------------------------------------------------------------
void SharedNumberChecks() {
  // the material word is the G-buffer's, field for field
  Check(kWordSmoothnessShift == gb::kMaterialSmoothnessShift && kWordClassShift == gb::kMaterialClassShift && kWordFlagsShift == gb::kMaterialFlagsShift &&
            kWordLitShift == gb::kMaterialLitShift,
        "the word's shifts are gbuffer_types.h's");
  Check(kWordClassMask == gb::kClassMask, "the class mask is gbuffer_types.h's");
  Check(kWordFlagSsr == gb::kMaterialFlagSsr && kWordFlagAlphaCovered == gb::kMaterialFlagAlphaCovered && kWordFlagVehicle == gb::kMaterialFlagVehicle,
        "the three flags are gbuffer_types.h's");
  const uint32_t from_gb = (200u << gb::kMaterialSmoothnessShift) | (uint32_t(gb::SurfaceClass::kGlass) << gb::kMaterialClassShift) |
                           ((gb::kMaterialFlagSsr | gb::kMaterialFlagVehicle) << gb::kMaterialFlagsShift) | (255u << gb::kMaterialLitShift);
  Check(PackMaterialWord(200, uint32_t(gb::SurfaceClass::kGlass), gb::kMaterialFlagSsr | gb::kMaterialFlagVehicle, 255, 0) == from_gb, "a word built from gbuffer_types.h's constants is the one PackMaterialWord makes");
  Check(kWordMetalShift == 24 && (from_gb >> 24) == 0, "the metal byte is the one gbuffer_types.h keeps reserved (bits 24..31 are 0 in the G-buffer's word)");
  Check(WordClass(PackMaterialWord(0, 31, 0, 0, 0)) == 31 && WordFlags(PackMaterialWord(0, 0, 7, 0, 0)) == 7, "the class and flags extract");
  // RT3 is the word's low two bytes: nothing of the city's extra bytes reaches it
  const uint32_t word = PackMaterialWord(77, 13, 2, 0, 99);
  Check((word & 0xFFu) == 77 && ((word >> 8) & 0xFFu) == (13u | (2u << 5)), "RT3's two bytes of a city word are the G-buffer's");

  // the instance slot
  Check(kInstanceIdFlag == mcla::gpu::city_rt::kInstanceIdFlag, "the instance id flag is city_rt_rules.h's");
  for (uint32_t sector : {0u, 1u, 7u, 100u, 438u, 100000u}) {
    for (auto detail : {mcla::gpu::city_rt::Detail::kHi, mcla::gpu::city_rt::Detail::kLod}) {
      const uint32_t id = mcla::gpu::city_rt::InstanceId(sector, detail);
      Check(SectorRowOfInstance(id) == SectorRowOf(sector, detail == mcla::gpu::city_rt::Detail::kLod), Num("the sector slot of instance id %.0f", double(id)));
    }
  }
  // the retail game's table: 439 sectors, two slots each
  Check(SectorRowOfInstance(mcla::gpu::city_rt::InstanceId(438, mcla::gpu::city_rt::Detail::kLod)) == 2 * 439 - 1, "the last sector's lod is the table's last slot");

  // the cars' R10G10B10 (car_rt_material_rules.h PackRgb10 on feat/rt-hit-shading 0a1cc4e4: copied here)
  Lcg rng;
  int mismatch = 0;
  for (int i = 0; i < 20000; ++i) {
    const float rgb[3] = {rng.Range(-0.5f, 2.5f), rng.Range(0.0f, 2.0f), i % 97 == 0 ? NAN : rng.Range(0.0f, 1.0f)};
    uint32_t car = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      const float v = std::clamp(std::isfinite(rgb[c]) ? rgb[c] : 0.0f, 0.0f, 2.0f) / 2.0f;
      car |= uint32_t(std::lround(v * 1023.0f)) << (10u * c);
    }
    if (PackRgb10(rgb) != car) ++mismatch;
  }
  Check(mismatch == 0, "PackRgb10 is the cars' formula bit for bit over 20000 colours (CHECK 1: compare again after the rt-hit-shading merge)");
  float back[3];
  UnpackRgb10(PackRgb10(std::array<float, 3>{0.5f, 1.0f, 2.0f}.data()), back);
  Check(NearlyEqual(back[0], 0.5, 1.0 / 1023, 0) && NearlyEqual(back[1], 1.0, 1.0 / 1023, 0) && back[2] == 2.0f, "R10G10B10 comes back within a step; 2.0 is exact");
}

// ---------------------------------------------------------------------------
// float16
// ---------------------------------------------------------------------------
void HalfChecks() {
  int bad = 0;
  for (uint32_t h = 0; h < 65536; ++h) {
    if (((h >> 10) & 31u) == 31u) continue;  // infinity and NaN are not produced
    const float f = HalfToFloat(uint16_t(h));
    if (HalfFromFloat(f) != uint16_t(h)) ++bad;
    if (f != hlsl_shim::f16tof32(h) && !(f == 0.0f && hlsl_shim::f16tof32(h) == 0.0f)) ++bad;  // the shim's definition agrees
  }
  Check(bad == 0, Num("every finite half round-trips and the C++ decode agrees with the definition the HLSL shim uses (%.0f bad)", bad));
  Check(HalfFromFloat(1e9f) == 0x7BFF && HalfFromFloat(-1e9f) == 0xFBFF && HalfToFloat(HalfFromFloat(INFINITY)) == 65504.0f, "values beyond 65504 saturate (never infinity)");
  Check(HalfFromFloat(NAN) == 0, "NaN is 0");
  Check(HalfFromFloat(1e-9f) == 0 && HalfFromFloat(-1e-9f) == 0x8000, "values far below the smallest subnormal are signed zero");
  Check(HalfFromFloat(2.9802322e-8f) == 0 && HalfFromFloat(5.960465e-8f) == 1 && HalfFromFloat(8.940697e-8f) == 2, "the subnormal ties go to even (2^-25 -> 0, 2^-24 -> 1, 1.5 x 2^-24 -> 2)");
  {
    // an independent reference for the nearest half: search the sorted values (ties to the even bit pattern)
    std::vector<float> values;  // the non negative finite halves, ascending: index = bit pattern
    for (uint32_t h = 0; h <= 0x7BFF; ++h) values.push_back(HalfToFloat(uint16_t(h)));
    const auto reference = [&](float x) -> uint16_t {
      const uint16_t sign = std::signbit(x) ? 0x8000u : 0u;
      const double ax = std::fabs(double(x));
      if (ax >= 65504.0) return uint16_t(sign | 0x7BFFu);
      const size_t upper = size_t(std::upper_bound(values.begin(), values.end(), float(ax)) - values.begin());  // first value > ax
      const size_t lower = upper - 1;
      if (upper >= values.size()) return uint16_t(sign | lower);
      const double below = ax - double(values[lower]), above = double(values[upper]) - ax;
      size_t pick = below < above ? lower : above < below ? upper : ((lower & 1u) == 0 ? lower : upper);
      return uint16_t(sign | pick);
    };
    Lcg rng;
    int differ = 0;
    for (int i = 0; i < 300000; ++i) {
      const float f = (rng.Next() - 0.5f) * std::ldexp(1.0f, int(rng.Below(40)) - 24);
      if (HalfFromFloat(f) != reference(f)) ++differ;
    }
    int tie_wrong = 0;
    for (uint32_t h = 0; h < 0x7BFF; ++h) {  // the exact midpoint of every neighbouring pair goes to the even one
      const float mid = (values[h] + values[h + 1]) * 0.5f;
      const uint16_t want = (h & 1u) == 0 ? uint16_t(h) : uint16_t(h + 1);
      if (HalfFromFloat(mid) != want || HalfFromFloat(-mid) != (want | 0x8000u)) ++tie_wrong;
    }
    Check(differ == 0, Num("the nearest half equals a table search over 300000 floats (%.0f differ)", differ));
    Check(tie_wrong == 0, Num("the midpoint of every pair of neighbouring halves goes to the even one, both signs (%.0f wrong)", tie_wrong));
  }
  // the directed roundings: down <= x <= up, and nothing between them but the two ends
  Lcg r2;
  int wrong = 0;
  std::vector<float> all;
  for (uint32_t h = 0; h < 65536; ++h)
    if (((h >> 10) & 31u) != 31u) all.push_back(HalfToFloat(uint16_t(h)));
  for (int i = 0; i < 400; ++i) {
    const float x = (r2.Next() - 0.5f) * std::ldexp(1.0f, int(r2.Below(26)) - 10);
    const float lo = HalfToFloat(HalfFromFloatDown(x));
    const float hi = HalfToFloat(HalfFromFloatUp(x));
    if (!(lo <= x && x <= hi)) ++wrong;
    for (float v : all)
      if (v > lo && v < hi) {
        ++wrong;
        break;
      }
  }
  // exact values stay
  for (float x : {0.0f, 1.0f, 8.0f, -3.5f, 65504.0f, 0.25f}) {
    if (HalfToFloat(HalfFromFloatDown(x)) != x || HalfToFloat(HalfFromFloatUp(x)) != x) ++wrong;
  }
  Check(wrong == 0, Num("HalfFromFloatDown / Up bracket the value with no half between (%.0f wrong)", wrong));
}

// ---------------------------------------------------------------------------
// the records
// ---------------------------------------------------------------------------
void RecordChecks() {
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 5000; ++i) {
    HitMaterial m;
    m.word = PackMaterialWord(rng.Below(256), rng.Below(16), rng.Below(8), rng.Below(256), rng.Below(256));
    m.texture = i % 5 == 0 ? kNoTexture : uint16_t(rng.Below(1748));
    m.layer = kNoTexture;
    for (int c = 0; c < 3; ++c) {
      m.tint[c] = rng.Range(0.0f, 2.0f);
      m.average[c] = rng.Range(0.0f, 2.0f);
    }
    m.emissive = i % 7 == 0 ? rng.Range(0.0f, 30.0f) : 0.0f;
    m.key = (uint64_t(rng.U32()) << 32) | rng.U32();
    m.shader_hash = rng.U32();
    uint32_t words[8];
    m.Pack(words);
    const HitMaterial u = HitMaterial::Unpack(words);
    bool ok = u.word == m.word && u.texture == m.texture && u.layer == m.layer && u.key == m.key && u.shader_hash == m.shader_hash && u.emissive == m.emissive;
    for (int c = 0; c < 3; ++c) ok = ok && NearlyEqual(u.tint[c], m.tint[c], 1.0 / 1023.0, 0) && NearlyEqual(u.average[c], m.average[c], 1.0 / 1023.0, 0);
    ok = ok && (words[2] >> 30) == 0 && (words[3] >> 30) == 0;  // the two spare bits are zero
    ByteWriter w;
    m.Write(w);
    ByteReader r(w.bytes().data(), w.size());
    HitMaterial read;
    ok = ok && w.size() == kHitMaterialBytes && HitMaterial::Read(r, &read) && read.word == m.word && read.key == m.key;
    if (!ok) ++bad;
  }
  Check(bad == 0, Num("a material record packs, unpacks, writes and reads (%.0f bad of 5000)", bad));
  {
    HitMaterial m;
    m.emissive = -3.0f;
    uint32_t w[8];
    m.Pack(w);
    Check(std::bit_cast<float>(w[4]) == 0.0f, "a negative emissive scale is written as 0");
    const HitMaterial d;
    Check(WordClass(d.word) == uint32_t(gb::SurfaceClass::kWorld) && WordLit(d.word) == 255 && WordSmoothness(d.word) == 38 && WordMetal(d.word) == 0, "the default material is a lit world surface of smoothness 0.15");
    uint8_t cut[16] = {};
    ByteReader r(cut, sizeof(cut));
    HitMaterial out;
    Check(!HitMaterial::Read(r, &out), "a cut material record does not read");
  }

  // the texture record and its sizes
  Check(HitMipBytes(HitTextureFormat::kBc1, 4, 4) == 8 && HitMipBytes(HitTextureFormat::kBc1, 1, 1) == 8 && HitMipBytes(HitTextureFormat::kBc3, 1, 1) == 16 &&
            HitMipBytes(HitTextureFormat::kRgba8, 3, 5) == 60 && HitMipBytes(HitTextureFormat::kBc2, 5, 9) == 2 * 3 * 16,
        "bytes of a level: blocks round up, RGBA8 is 4 a pixel");
  HitTexture t;
  t.key = 0x0123456789ABCDEFull;
  t.width = 64;
  t.height = 64;
  t.format = HitTextureFormat::kBc3;
  t.mip_count = 7;  // 64, 32, 16, 8, 4, 2, 1
  t.flags = kTextureGamma | kTextureAlpha;
  t.data_offset = 0x1'0000'0040ull;
  t.data_size = uint32_t(t.ChainBytes());
  Check(t.MipOffset(0) == 0 && t.MipOffset(1) == 16 * 16 * 16 && t.MipOffset(2) == 16 * 16 * 16 + 8 * 8 * 16, "mip offsets: tightly packed, the full size first");
  Check(t.ChainBytes() == 4096 + 1024 + 256 + 64 + 16 + 16 + 16 && t.ChainBytes(2) == 256 + 64 + 16 + 16 + 16 && t.ChainBytes(7) == 0, "bytes of the chain from a level (blocks do not shrink below one)");
  {
    uint32_t words[8];
    t.Pack(words);
    const HitTexture u = HitTexture::Unpack(words);
    Check(u.key == t.key && u.width == 64 && u.height == 64 && u.format == t.format && u.mip_count == 7 && u.flags == t.flags && u.data_offset == t.data_offset && u.data_size == t.data_size,
          "a texture record packs and unpacks (a 33 bit offset too)");
    ByteWriter w;
    t.Write(w);
    ByteReader r(w.bytes().data(), w.size());
    HitTexture read;
    Check(w.size() == kHitTextureBytes && HitTexture::Read(r, &read) && read.key == t.key, "a texture record writes 32 bytes and reads");
    // CITY-HIT-SIZE: the record's last word is the stored size (0: the chain as it is); a record written before it (word 7 = 0) reads as a plain one
    Check(words[7] == 0 && u.stored_size == 0 && !u.IsCompressed() && u.StoredBytes() == t.data_size && read.stored_size == 0, "a texture record without a stored size is a plain chain");
    HitTexture z = t;
    z.stored_size = t.data_size / 2;
    uint32_t zwords[8];
    z.Pack(zwords);
    const HitTexture zu = HitTexture::Unpack(zwords);
    Check(zwords[7] == z.stored_size && zu.stored_size == z.stored_size && zu.IsCompressed() && zu.StoredBytes() == z.stored_size && zu.data_size == t.data_size && zu.key == t.key &&
              zu.data_offset == t.data_offset,
          "a texture record with a stored size packs and unpacks it in the last word, the rest unchanged");
    ByteWriter zw;
    z.Write(zw);
    ByteReader zr(zw.bytes().data(), zw.size());
    HitTexture zread;
    Check(zw.size() == kHitTextureBytes && HitTexture::Read(zr, &zread) && zread.stored_size == z.stored_size, "a texture record with a stored size writes 32 bytes and reads");
    z.stored_size = z.data_size;
    Check(!z.IsCompressed() && z.StoredBytes() == z.data_size, "a stored size equal to the chain's size means the chain as it is");
    words[3] = (words[3] & ~0xFFu) | 9u;  // a format this build does not know
    ByteWriter w2;
    for (uint32_t v : words) w2.U32(v);
    ByteReader r2(w2.bytes().data(), w2.size());
    Check(!HitTexture::Read(r2, &read), "a texture of an unknown format is refused");
  }
  {
    const GpuTextureRow row = MakeTextureRow(t, 1234, 2);
    Check(row.slot == (1234u | kGammaBit) && row.size == (64u | (64u << 16)) && (row.mips & 0xFF) == 5 && ((row.mips >> 8) & 0xFF) == 2 && ((row.mips >> 16) & 0xFF) == t.flags,
          "a texture row: slot with the gamma bit, the full size, five resident levels from level 2");
    HitTexture plain = t;
    plain.flags = 0;
    Check(MakeTextureRow(plain, 77, 0).slot == 77u, "no gamma flag, no gamma bit");
    Check(MakeTextureRow(t, kNoSlot, 0).slot == kNoSlot, "an unuploaded texture stays kNoSlot");
    Check(((MakeTextureRow(t, 5, 99).mips >> 8) & 0xFF) == 6, "a first level past the chain keeps the last one");
  }
}

// ---------------------------------------------------------------------------
// the UV transform
// ---------------------------------------------------------------------------
void UvChecks() {
  Lcg rng;
  int bad_cover = 0, bad_error = 0, rejected = 0;
  double worst_texels = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const int kind = i % 4;
    float lo[2], hi[2];
    for (int a = 0; a < 2; ++a) {
      lo[a] = kind == 0 ? rng.Range(0.0f, 1.0f) : kind == 1 ? rng.Range(8.0f, 16.0f) : kind == 2 ? rng.Range(-60.0f, 60.0f) : rng.Range(-1000.0f, 1000.0f);
      hi[a] = lo[a] + (kind == 0 ? rng.Range(0.0f, 1.0f) : kind == 1 ? rng.Range(0.0f, 8.0f) : kind == 2 ? rng.Range(0.0f, 100.0f) : (i % 3 == 0 ? 0.0f : rng.Range(0.0f, 256.0f)));
    }
    HitRun run;
    if (!ChooseUvTransform(lo[0], hi[0], lo[1], hi[1], &run)) {
      ++rejected;
      continue;
    }
    for (int a = 0; a < 2; ++a) {
      // float16 exact
      if (HalfToFloat(HalfFromFloat(run.uv_bias[a])) != run.uv_bias[a] || HalfToFloat(HalfFromFloat(run.uv_scale[a])) != run.uv_scale[a]) ++bad_cover;
      // the range is inside 0..1 without clamping
      const double t_lo = (double(lo[a]) - run.uv_bias[a]) / run.uv_scale[a];
      const double t_hi = (double(hi[a]) - run.uv_bias[a]) / run.uv_scale[a];
      if (t_lo < 0.0 || t_hi > 1.0 + 1e-12 || run.uv_scale[a] <= 0.0f) ++bad_cover;
    }
    for (int s = 0; s < 4; ++s) {
      const float u = s == 0 ? lo[0] : s == 1 ? hi[0] : rng.Range(lo[0], hi[0]);
      const float v = s == 0 ? lo[1] : s == 1 ? hi[1] : rng.Range(lo[1], hi[1]);
      float uv[2];
      DecodeCornerUv(run, EncodeCornerUv(run, u, v), uv);
      const double limit_u = run.uv_scale[0] / 131070.0 * 1.001 + 8.0 * std::ldexp(std::max(1.0, std::fabs(double(u))), -23);
      const double limit_v = run.uv_scale[1] / 131070.0 * 1.001 + 8.0 * std::ldexp(std::max(1.0, std::fabs(double(v))), -23);
      if (std::fabs(double(uv[0]) - u) > limit_u || std::fabs(double(uv[1]) - v) > limit_v) ++bad_error;
      if (kind == 1) worst_texels = std::max(worst_texels, std::max(std::fabs(double(uv[0]) - u), std::fabs(double(uv[1]) - v)) * 256.0);
    }
  }
  Check(bad_cover == 0, Num("the UV transform covers its range with float16 numbers (%.0f bad)", bad_cover));
  Check(bad_error == 0, Num("a quantised texture coordinate is within half a step of the original (%.0f bad)", bad_error));
  Check(worst_texels < 0.5, Num("at texture coordinates of 8 to 16 (ranges up to 8) the error is %.4f texel at 256 pixels (limit 0.5)", worst_texels));
  Check(rejected == 0, Num("no range of the test (up to +-1000 and 256 wide) is refused (%.0f of 20000)", rejected));
  HitRun r;
  Check(!ChooseUvTransform(NAN, 1, 0, 1, &r) && !ChooseUvTransform(0, 1, 2, 1, &r) && !ChooseUvTransform(-70000, 0, 0, 1, &r) && !ChooseUvTransform(-60000, 60000, 0, 1, &r),
        "a NaN, an inverted range, a bound or a range beyond float16 is refused");
  Check(ChooseUvTransform(3.0f, 3.0f, -2.0f, -2.0f, &r) && r.uv_scale[0] == 1.0f && r.uv_bias[0] == 3.0f && r.uv_bias[1] == -2.0f, "a flat range gets scale 1");
  // clamping outside the range, and NaN
  HitRun c;
  ChooseUvTransform(0, 1, 0, 1, &c);
  float uv[2];
  DecodeCornerUv(c, EncodeCornerUv(c, -5.0f, 9.0f), uv);
  Check(uv[0] == 0.0f && std::fabs(uv[1] - 1.0f) < 1e-6f, "a coordinate outside the range clamps to its edge");
  DecodeCornerUv(c, EncodeCornerUv(c, NAN, 0.5f), uv);
  Check(uv[0] == 0.0f, "a NaN coordinate is 0");

  // a run's record
  HitRun a;
  a.first_triangle = 100;
  a.triangle_count = 250;
  a.material = 4321;
  a.flags = kRunHasUv | kRunHasColour;
  ChooseUvTransform(8.25f, 15.5f, -3.0f, 0.0f, &a);
  uint32_t words[8];
  a.Pack(words);
  const HitRun b = HitRun::Unpack(words);
  Check(b.first_triangle == 100 && b.triangle_count == 250 && b.material == 4321 && b.flags == a.flags && b.uv_bias[0] == a.uv_bias[0] && b.uv_bias[1] == a.uv_bias[1] &&
            b.uv_scale[0] == a.uv_scale[0] && b.uv_scale[1] == a.uv_scale[1] && words[6] == 0 && words[7] == 0,
        "a run packs and unpacks (the last two words are zero)");
}

// ---------------------------------------------------------------------------
// the corner stream
// ---------------------------------------------------------------------------
std::vector<uint32_t> RandomCorners(Lcg& rng, size_t corners, bool smooth) {
  std::vector<uint32_t> w;
  uint32_t u = rng.Below(65536), v = rng.Below(65536), colour = rng.U32();
  for (size_t i = 0; i < corners; ++i) {
    if (smooth) {
      u = uint32_t(std::clamp(int(u) + int(rng.Below(601)) - 300, 0, 65535));
      v = uint32_t(std::clamp(int(v) + int(rng.Below(601)) - 300, 0, 65535));
      if (rng.Below(40) == 0) colour = rng.U32();
    } else {
      u = rng.Below(65536);
      v = rng.Below(65536);
      colour = rng.U32();
    }
    w.push_back(u | (v << 16));
    w.push_back(colour);
  }
  return w;
}

void CornerChecks() {
  Lcg rng;
  for (CornerEncoding enc : {CornerEncoding::kRaw, CornerEncoding::kDelta}) {
    for (bool smooth : {false, true}) {
      const std::vector<uint32_t> in = RandomCorners(rng, 3000, smooth);
      ByteWriter w;
      EncodeCorners(in.data(), in.size() / 2, enc, w);
      ByteReader r(w.bytes().data(), w.size());
      std::vector<uint32_t> out;
      const bool ok = DecodeCorners(r, in.size() / 2, enc, &out);
      Check(ok && out == in && r.left() == 0, std::string("corners round-trip: ") + (enc == CornerEncoding::kRaw ? "raw" : "delta") + (smooth ? " smooth" : " random"));
      if (enc == CornerEncoding::kRaw) Check(w.size() == in.size() / 2 * 8, "raw is 8 bytes a corner");
      if (enc == CornerEncoding::kDelta && smooth) Check(w.size() < in.size() / 2 * 8, Num("smooth corners are smaller in delta (%.2f bytes a corner)", double(w.size()) / double(in.size() / 2)));
      // cut short
      if (w.size() > 4) {
        ByteReader cut(w.bytes().data(), w.size() - 3);
        std::vector<uint32_t> o2;
        Check(!DecodeCorners(cut, in.size() / 2, enc, &o2), "cut corners do not decode");
      }
    }
  }
  // the first corner against 0, a zero colour costs nothing, the extremes
  const uint32_t extremes[] = {0u, 0u, 0xFFFFu | (0xFFFFu << 16), 0xFFFFFFFFu, 0u, 0u};
  ByteWriter w;
  EncodeCorners(extremes, 3, CornerEncoding::kDelta, w);
  ByteReader r(w.bytes().data(), w.size());
  std::vector<uint32_t> out;
  Check(DecodeCorners(r, 3, CornerEncoding::kDelta, &out) && out == std::vector<uint32_t>(extremes, extremes + 6), "the extremes (0 and 65535 both ways, a colour changing to 0xFFFFFFFF and back) round-trip");
  // a stream that walks off the 16 bit range is refused
  ByteWriter bad;
  bad.Varint(2u << 1);  // zigzag 2 = +1: u becomes 1, the colour stays
  bad.Varint(3u);       // zigzag 3 = -2: v becomes -2, below 0
  ByteReader br(bad.bytes().data(), bad.size());
  Check(!DecodeCorners(br, 1, CornerEncoding::kDelta, &out), "a delta that leaves 0..65535 is refused");
  Check(!DecodeCorners(br, 1, static_cast<CornerEncoding>(7), &out), "an unknown corner encoding is refused");
}

// ---------------------------------------------------------------------------
// the hit blob, the file
// ---------------------------------------------------------------------------
// Runs that partition [0, triangles) with the opaque boundary respected; materials random below `materials`.
std::vector<HitRun> RandomRuns(Lcg& rng, uint32_t triangles, uint32_t opaque, uint32_t materials, bool uv_ranges) {
  std::vector<HitRun> runs;
  uint32_t at = 0;
  while (at < triangles) {
    const uint32_t limit = at < opaque ? opaque : triangles;
    const uint32_t count = std::min(limit - at, 1 + rng.Below(std::max(2u, triangles / 6)));
    HitRun run;
    run.first_triangle = at;
    run.triangle_count = count;
    run.material = rng.Below(materials);
    run.flags = (at >= opaque ? kRunAlphaTested : 0u) | kRunHasUv | kRunHasColour;
    if (uv_ranges) {
      const float lo_u = rng.Below(3) == 0 ? rng.Range(8.0f, 12.0f) : rng.Range(-2.0f, 2.0f);
      const float lo_v = rng.Range(-3.0f, 9.0f);
      ChooseUvTransform(lo_u, lo_u + rng.Range(0.01f, 4.0f), lo_v, lo_v + rng.Range(0.0f, 5.0f), &run);
    }
    runs.push_back(run);
    at += count;
  }
  return runs;
}

HitBlob RandomBlob(Lcg& rng, uint32_t triangles, uint32_t opaque, uint32_t materials, bool corners) {
  HitBlob b;
  b.triangle_count = triangles;
  b.opaque_triangles = opaque;
  b.flags = corners ? kHitBlobHasCorners : 0;
  b.runs = RandomRuns(rng, triangles, opaque, materials, true);
  if (corners) {
    for (const HitRun& run : b.runs) {
      for (uint32_t t = 0; t < run.triangle_count; ++t) {
        for (int k = 0; k < 3; ++k) {
          const float u = run.uv_bias[0] + run.uv_scale[0] * rng.Next();
          const float v = run.uv_bias[1] + run.uv_scale[1] * rng.Next();
          b.corners.push_back(EncodeCornerUv(run, u, v));
          b.corners.push_back(rng.Below(3) == 0 ? PackCornerColour(rng.Below(256), rng.Below(256), rng.Below(256), rng.Below(256)) : 0xFF808080u);
        }
      }
    }
  }
  return b;
}

void BlobChecks() {
  Lcg rng;
  for (CornerEncoding enc : {CornerEncoding::kRaw, CornerEncoding::kDelta}) {
    for (int with : {0, 1}) {
      for (uint32_t opaque_kind = 0; opaque_kind < 3; ++opaque_kind) {
        const uint32_t tri = 400;
        const uint32_t opaque = opaque_kind == 0 ? 0 : opaque_kind == 1 ? tri : 170;
        const HitBlob b = RandomBlob(rng, tri, opaque, 5000, with != 0);
        Check(CheckRuns(b.runs, tri, opaque).empty(), "the random runs are a partition");
        const std::vector<uint8_t> bytes = EncodeHitBlob(b, enc);
        HitBlob out;
        std::string error;
        const bool ok = DecodeHitBlob(bytes.data(), bytes.size(), &out, &error);
        bool same = ok && out.flags == b.flags && out.triangle_count == tri && out.opaque_triangles == opaque && out.runs.size() == b.runs.size() && out.corners == b.corners;
        for (size_t i = 0; same && i < b.runs.size(); ++i) {
          const HitRun &x = b.runs[i], &y = out.runs[i];
          same = x.first_triangle == y.first_triangle && x.triangle_count == y.triangle_count && x.material == y.material && x.flags == y.flags && x.uv_bias[0] == y.uv_bias[0] &&
                 x.uv_bias[1] == y.uv_bias[1] && x.uv_scale[0] == y.uv_scale[0] && x.uv_scale[1] == y.uv_scale[1];
        }
        Check(same, "a hit blob round-trips: " + error);
        if (!bytes.empty()) {
          std::vector<uint8_t> damaged = bytes;
          damaged[damaged.size() / 2 + 3] ^= 0x40;
          Check(!DecodeHitBlob(damaged.data(), damaged.size(), &out, &error) && error.find("hash") != std::string::npos, "a damaged blob fails its hash");
          Check(!DecodeHitBlob(bytes.data(), bytes.size() - 5, &out, &error), "a cut blob does not decode");
        }
      }
    }
  }
  // the runs' partition rules
  HitBlob b = RandomBlob(rng, 100, 60, 10, false);
  auto flawed = [&](auto mutate) {
    std::vector<HitRun> r = b.runs;
    mutate(r);
    return !CheckRuns(r, 100, 60).empty();
  };
  Check(flawed([](auto& r) { r[0].first_triangle = 1; }), "runs must start at 0");
  Check(flawed([](auto& r) { r[1].first_triangle += 1; }), "runs must not leave a gap");
  Check(flawed([](auto& r) { r.pop_back(); }), "runs must cover every triangle");
  Check(flawed([](auto& r) { r[0].triangle_count = 0; }), "a run is not empty");
  Check(flawed([](auto& r) { r.back().triangle_count += 5; }), "a run does not pass the triangles");
  Check(flawed([](auto& r) { r.front().flags |= kRunAlphaTested; }), "the alpha tested flag follows the boundary");
  Check(CheckRuns({}, 0, 0).empty() && !CheckRuns({}, 5, 0).empty(), "no runs for no triangles is fine; none for some triangles is refused");
  {
    HitRun across;
    across.first_triangle = 0;
    across.triangle_count = 100;
    Check(!CheckRuns({across}, 100, 60).empty(), "a run across the opaque boundary is refused");
  }
  Check(ExpandRunIds(b.runs, 100).size() == 100 && ExpandRunIds({}, 100).empty() && ExpandRunIds(b.runs, 99).empty(), "run ids expand from a partition and from nothing else");
  {
    const std::vector<uint16_t> ids = ExpandRunIds(b.runs, 100);
    bool ok = true;
    for (size_t i = 0; i < b.runs.size(); ++i)
      for (uint32_t t = 0; t < b.runs[i].triangle_count; ++t) ok = ok && ids[b.runs[i].first_triangle + t] == i;
    Check(ok, "every triangle's run id is its run's index");
  }
  // a blob whose flags say corners and whose stream is not there
  {
    const HitBlob nb = RandomBlob(rng, 50, 50, 10, false);
    std::vector<uint8_t> bytes = EncodeHitBlob(nb);
    bytes[4] |= kHitBlobHasCorners;  // flags word: not covered by the hash
    HitBlob out;
    std::string error;
    Check(!DecodeHitBlob(bytes.data(), bytes.size(), &out, &error) && error.find("flags") != std::string::npos, "flags that promise corners the blob does not hold are refused");
    bytes = EncodeHitBlob(nb);
    bytes[0] ^= 1;
    Check(!DecodeHitBlob(bytes.data(), bytes.size(), &out, &error), "a blob with the wrong magic is refused");
  }
}

void FileChecks() {
  // the header
  HitHeader h;
  h.sector_count = 439;
  h.material_count = 7345;
  h.texture_count = 1748;
  h.base_table_hash = 0x1122334455667788ull;
  h.base_file_size = 248ull << 20;
  const HitFileLayout l = PlanHitFile(h.sector_count, h.material_count, h.texture_count);
  h.table_offset = l.table_offset;
  h.materials_offset = l.materials_offset;
  h.textures_offset = l.textures_offset;
  h.blobs_offset = l.blobs_offset;
  h.file_size = l.blobs_offset + 1000;
  h.table_hash = 0xAABBCCDDEEFF0011ull;
  ByteWriter w;
  h.Write(w);
  Check(w.size() == kHitHeaderBytes && std::memcmp(w.bytes().data(), "LARCHIT1", 8) == 0, "the header is 128 bytes and starts with its magic");
  ByteReader r(w.bytes().data(), w.size());
  HitHeader back;
  std::string error;
  Check(HitHeader::Read(r, &back, &error) && back.sector_count == 439 && back.material_count == 7345 && back.texture_count == 1748 && back.base_table_hash == h.base_table_hash &&
            back.blobs_offset == l.blobs_offset && back.table_hash == h.table_hash && back.file_size == h.file_size,
        "the header reads back: " + error);
  {
    std::vector<uint8_t> bytes = w.bytes();
    bytes[8] = 2;  // format version
    ByteReader r2(bytes.data(), bytes.size());
    Check(!HitHeader::Read(r2, &back, &error) && error.find("version") != std::string::npos, "another format version is refused");
    bytes = w.bytes();
    bytes[0] = 'X';
    ByteReader r3(bytes.data(), bytes.size());
    Check(!HitHeader::Read(r3, &back, &error), "the wrong magic is refused");
    bytes = w.bytes();
    bytes[24] = 40;  // sector_record_bytes
    ByteReader r4(bytes.data(), bytes.size());
    Check(!HitHeader::Read(r4, &back, &error), "another record size is refused");
    ByteReader r5(w.bytes().data(), 60);
    Check(!HitHeader::Read(r5, &back, &error), "a cut header is refused");
  }
  // the sections
  Check(l.table_offset == 128 && l.materials_offset % 16 == 0 && l.textures_offset % 16 == 0 && l.blobs_offset % 16 == 0 && l.materials_offset >= l.table_offset + 439ull * 48 &&
            l.textures_offset >= l.materials_offset + 7345ull * 32 && l.blobs_offset >= l.textures_offset + 1748ull * 32 && l.blobs_offset < l.textures_offset + 1748ull * 32 + 16,
        "the sections follow each other, each on 16 bytes, with at most the padding between");
  const HitFileLayout empty = PlanHitFile(0, 0, 0);
  Check(empty.table_offset == 128 && empty.blobs_offset == 128, "an empty file has its blobs at the header's end");

  // the sector record
  HitSectorRecord s;
  s.name_hash = 0xDEADBEEF;
  s.flags = kHitSectorFoliageHeavy;
  s.hi_offset = 0x2'0000'0000ull;
  s.lod_offset = 0x2'0001'0000ull;
  s.hi_size = 123456;
  s.lod_size = 7890;
  s.hi_triangles = 49800;
  s.lod_triangles = 900;
  s.hi_runs = 321;
  s.lod_runs = 12;
  ByteWriter sw;
  s.Write(sw);
  ByteReader sr(sw.bytes().data(), sw.size());
  HitSectorRecord sb;
  Check(sw.size() == kHitSectorRecordBytes && HitSectorRecord::Read(sr, &sb) && sb.name_hash == s.name_hash && sb.hi_offset == s.hi_offset && sb.lod_offset == s.lod_offset &&
            sb.hi_size == s.hi_size && sb.lod_size == s.lod_size && sb.hi_triangles == s.hi_triangles && sb.lod_triangles == s.lod_triangles && sb.hi_runs == 321 && sb.lod_runs == 12 &&
            sb.flags == kHitSectorFoliageHeavy,
        "a sector record is 48 bytes and reads back");

  // the texture file's header
  TexHeader t;
  t.texture_count = 1748;
  t.hit_table_hash = h.table_hash;
  t.file_size = 67ull << 20;
  ByteWriter tw;
  t.Write(tw);
  ByteReader tr(tw.bytes().data(), tw.size());
  TexHeader tb;
  Check(tw.size() == kTexHeaderBytes && std::memcmp(tw.bytes().data(), "LARCTEX1", 8) == 0 && TexHeader::Read(tr, &tb, &error) && tb.texture_count == 1748 && tb.hit_table_hash == h.table_hash &&
            tb.data_offset == kTexHeaderBytes && tb.file_size == t.file_size,
        "the texture file's header is 64 bytes and reads back");
  std::vector<uint8_t> tbytes = tw.bytes();
  tbytes[8] = 3;
  ByteReader tr2(tbytes.data(), tbytes.size());
  Check(!TexHeader::Read(tr2, &tb, &error), "a texture file of another version is refused");
  // CITY-HIT-SIZE: the flags word carries kTexFlagZstd; a flag this build does not know is the reader's to refuse (TextureFileReader::Open)
  Check(t.flags == 0 && kTexFlagsKnown == kTexFlagZstd && kTexFlagZstd == 1, "the texture file's flags: none, and zstd is the one known bit");
  TexHeader tz = t;
  tz.flags = kTexFlagZstd | 0x80u;
  ByteWriter tzw;
  tz.Write(tzw);
  ByteReader tzr(tzw.bytes().data(), tzw.size());
  TexHeader tzb;
  Check(TexHeader::Read(tzr, &tzb, &error) && tzb.flags == (kTexFlagZstd | 0x80u), "the texture file's flags word reads back whole (the reader of the file decides what it knows)");
}

// ---------------------------------------------------------------------------
// the pool block
// ---------------------------------------------------------------------------
void PoolChecks() {
  // a known block: 5 vertices, 3 triangles, 2 runs
  const PoolBlockPlan p = PlanPoolBlock(5, 3, 2, true, true);
  Check(p.index_offset == 0 && p.position_offset == 48 && p.run_id_offset == 112 && p.run_table_offset == 128 && p.attribute_offset == 192 && p.total_bytes == 272 && p.has_runs && p.has_corners,
        Num("the plan of 5 vertices, 3 triangles, 2 runs: positions %.0f, run ids %.0f, run table %.0f, corners %.0f (expected 48, 112, 128, 192)", double(p.position_offset), double(p.run_id_offset), double(p.run_table_offset), double(p.attribute_offset)));
  const PoolBlockPlan geometry = PlanPoolBlock(5, 3, 0, false, true);
  Check(!geometry.has_runs && !geometry.has_corners && geometry.total_bytes == 112, "without runs there are no corners either: indices and positions only (112 bytes)");
  const PoolBlockPlan runs_only = PlanPoolBlock(5, 3, 2, true, false);
  Check(runs_only.has_runs && !runs_only.has_corners && runs_only.total_bytes == 192, "runs without corners: 192 bytes");
  const PoolBlockPlan none = PlanPoolBlock(0, 0, 0, false, false);
  Check(none.total_bytes == 0, "nothing is nothing");
  // order, alignment, no overlap on random counts
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 2000; ++i) {
    const uint32_t v = rng.Below(100000), t = rng.Below(250000), r = 1 + rng.Below(700);
    const PoolBlockPlan q = PlanPoolBlock(v, t, r, true, true);
    const bool aligned = q.index_offset % 16 == 0 && q.position_offset % 16 == 0 && q.run_id_offset % 16 == 0 && q.run_table_offset % 16 == 0 && q.attribute_offset % 16 == 0 && q.total_bytes % 16 == 0;
    const bool ordered = q.index_offset + uint64_t(t) * 12 <= q.position_offset && q.position_offset + uint64_t(v) * 12 <= q.run_id_offset && q.run_id_offset + uint64_t((t + 1) / 2) * 4 <= q.run_table_offset &&
                         q.run_table_offset + uint64_t(r) * 32 <= q.attribute_offset && q.attribute_offset + uint64_t(t) * 24 <= q.total_bytes;
    const bool tight = q.total_bytes < uint64_t(t) * 12 + uint64_t(v) * 12 + uint64_t((t + 1) / 2) * 4 + uint64_t(r) * 32 + uint64_t(t) * 24 + 5 * 16;
    if (!aligned || !ordered || !tight) ++bad;
  }
  Check(bad == 0, "a plan's arrays are aligned, in order, do not overlap and waste under five alignments");
  // the sector row
  GpuSectorRow row;
  Check(MakeSectorRow(p, 4096, 3, 2, kSectorRowFoliage | 0x80, &row) && row.index_offset == 4096 && row.position_offset == 4096 + 48 && row.run_id_offset == 4096 + 112 && row.run_table_offset == 4096 + 128 &&
            row.attribute_offset == 4096 + 192 && row.triangle_count == 3 && row.opaque_triangles == 2 && row.flags == (kSectorRowValid | kSectorRowRuns | kSectorRowCorners | kSectorRowFoliage),
        "a sector row holds absolute offsets and the flags (an unknown extra flag is not copied)");
  Check(MakeSectorRow(geometry, 0, 3, 3, 0, &row) && row.flags == kSectorRowValid && row.run_id_offset == 0 && row.run_table_offset == 0 && row.attribute_offset == 0, "a geometry-only row has no run or corner offsets");
  Check(!MakeSectorRow(p, 4097, 3, 2, 0, &row), "a base that is not on 16 bytes is refused");
  Check(!MakeSectorRow(p, (kMaxPoolBytes - 64) & ~uint64_t(15), 3, 2, 0, &row) && MakeSectorRow(p, (kMaxPoolBytes - 272) & ~uint64_t(15), 3, 2, 0, &row), "a block past the 32 bit offsets is refused, the last one that fits is not");
  Check(!MakeSectorRow(p, 0, 3, 4, 0, &row), "more opaque triangles than triangles is refused");
  // the destination checks of the writer
  MeshData mesh;
  mesh.positions.assign(15, 1.0f);
  mesh.indices = {0, 1, 2, 1, 2, 3, 2, 3, 4};
  mesh.opaque_triangles = 2;
  HitBlob blob;
  blob.triangle_count = 3;
  blob.opaque_triangles = 2;
  blob.flags = kHitBlobHasCorners;
  HitRun r0;
  r0.first_triangle = 0;
  r0.triangle_count = 2;
  r0.flags = kRunHasUv;
  HitRun r1;
  r1.first_triangle = 2;
  r1.triangle_count = 1;
  r1.flags = kRunAlphaTested | kRunHasUv;
  blob.runs = {r0, r1};
  blob.corners.assign(18, 0);
  std::vector<uint8_t> dest(272 + 16, 0xEE);
  std::string error;
  Check(FillPoolBlock(p, mesh, &blob, dest.data(), dest.size(), &error), "the writer fills a block: " + error);
  Check(dest[272] == 0xEE, "the writer stays inside the block");
  Check(!FillPoolBlock(p, mesh, &blob, dest.data(), 100, &error), "a destination that is too small is refused");
  Check(!FillPoolBlock(p, mesh, nullptr, dest.data(), dest.size(), &error), "a block with runs and no blob is refused");
  HitBlob wrong = blob;
  wrong.opaque_triangles = 1;
  Check(!FillPoolBlock(p, mesh, &wrong, dest.data(), dest.size(), &error), "a blob whose opaque triangles are not the mesh's is refused");
  Check(!FillPoolBlock(PlanPoolBlock(5, 4, 2, true, true), mesh, &blob, dest.data(), dest.size(), &error), "a plan that is not this mesh's is refused");
  wrong = blob;
  wrong.corners.pop_back();
  Check(!FillPoolBlock(p, mesh, &wrong, dest.data(), dest.size(), &error), "a blob with a corner missing is refused");
  // the written bytes: the first index word, the run id word (two 16 bit ids), the run record
  Check(dest[4] == 1 && dest[0] == 0 && dest[8] == 2, "indices are little endian uint32");
  Check(dest[112] == 0 && dest[113] == 0 && dest[114] == 0 && dest[115] == 0 && dest[116] == 1 && dest[117] == 0, "run ids: triangles 0 and 1 in run 0, triangle 2 in run 1, two to a word");
}

// ---------------------------------------------------------------------------
// the .hlsli against the C++ values
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void HlslDefinesChecks(const std::string& text) {
  std::map<std::string, double> defines;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("#define CITY_HIT_", 0) != 0) continue;
    std::istringstream one(line);
    std::string define, name, value;
    one >> define >> name >> value;
    if (value.empty() || name.find('(') != std::string::npos) continue;  // the join macros
    char* end = nullptr;
    const double v = std::strtod(value.c_str(), &end);
    if (end == value.c_str()) continue;  // a macro name, not a number (CITY_HIT_SPACE_NAME)
    defines[name] = v;
  }
  const std::map<std::string, double> want = {
      {"CITY_HIT_FORMAT_VERSION", double(kHitFormatVersion)},
      {"CITY_HIT_INSTANCE_ID_MASK", double(kInstanceRowMask)},
      {"CITY_HIT_SECTOR_ROWS", 2},
      {"CITY_HIT_MATERIAL_ROWS", double(kHitMaterialBytes / 16)},
      {"CITY_HIT_RUN_BYTES", double(kHitRunBytes)},
      {"CITY_HIT_TRIANGLE_BYTES", 12},
      {"CITY_HIT_POSITION_BYTES", 12},
      {"CITY_HIT_CORNER_BYTES", 8},
      {"CITY_HIT_CORNERS_BYTES", 24},
      {"CITY_HIT_SECTOR_VALID", double(kSectorRowValid)},
      {"CITY_HIT_SECTOR_RUNS", double(kSectorRowRuns)},
      {"CITY_HIT_SECTOR_CORNERS", double(kSectorRowCorners)},
      {"CITY_HIT_SECTOR_FOLIAGE", double(kSectorRowFoliage)},
      {"CITY_HIT_RUN_ALPHA_TESTED", double(kRunAlphaTested)},
      {"CITY_HIT_RUN_HAS_UV", double(kRunHasUv)},
      {"CITY_HIT_RUN_HAS_COLOUR", double(kRunHasColour)},
      {"CITY_HIT_RUN_TWO_SIDED", double(kRunTwoSided)},
      {"CITY_HIT_NO_TEXTURE", double(kNoTexture)},
      {"CITY_HIT_NO_SLOT", double(kNoSlot)},
      {"CITY_HIT_GAMMA_BIT", double(kGammaBit)},
      {"CITY_HIT_RGB10_MAX", double(kHitRgb10Max)},
      {"CITY_HIT_WORD_SMOOTHNESS_SHIFT", double(kWordSmoothnessShift)},
      {"CITY_HIT_WORD_CLASS_SHIFT", double(kWordClassShift)},
      {"CITY_HIT_WORD_FLAGS_SHIFT", double(kWordFlagsShift)},
      {"CITY_HIT_WORD_LIT_SHIFT", double(kWordLitShift)},
      {"CITY_HIT_WORD_METAL_SHIFT", double(kWordMetalShift)},
      {"CITY_HIT_WORD_CLASS_MASK", double(kWordClassMask)},
      {"CITY_HIT_WORD_FLAG_SSR", double(kWordFlagSsr)},
      {"CITY_HIT_WORD_FLAG_ALPHA_COVERED", double(kWordFlagAlphaCovered)},
      {"CITY_HIT_WORD_FLAG_VEHICLE", double(kWordFlagVehicle)},
  };
  Check(defines.size() == want.size(), Num("the .hlsli has %.0f CITY_HIT_ numeric defines (the test knows %.0f)", double(defines.size()), double(want.size())));
  for (const auto& [name, value] : want) {
    const auto it = defines.find(name);
    Check(it != defines.end() && it->second == value, name + " is the C++ value");
  }
  // the G-buffer's own HLSL numbers: the word's fields in gbuffer_common.hlsli are the same (read from the C++ header they are checked against above)
  Check(text.find("gCityPool : register(t6,") != std::string::npos && text.find("gCitySectors : register(t7,") != std::string::npos &&
            text.find("gCityMaterials : register(t8,") != std::string::npos && text.find("gCityTextures : register(t9,") != std::string::npos,
        "the buffers sit at t6, t7, t8, t9 (after the car scene's t0..t5)");
  Check(text.find("ByteAddressBuffer gCityPool") != std::string::npos && text.find("StructuredBuffer<uint4> gCitySectors") != std::string::npos, "the pool is a ByteAddressBuffer, the tables StructuredBuffer<uint4>");
}

// ---------------------------------------------------------------------------
// a synthetic city: C++ writers -> the pool -> the HLSL decode (as C++) and the twin
// ---------------------------------------------------------------------------
struct SynthSector {
  MeshData mesh;
  HitBlob blob;
  bool runs = true, corners = true;
  PoolBlockPlan plan;
  uint64_t base = 0;
  uint32_t sector = 0;
  bool lod = false;
};

void DecodeChecks() {
  Lcg rng;
  const uint32_t materials = 40;
  const uint32_t textures = 12;

  // the global tables
  std::vector<HitMaterial> material_list(materials);
  std::vector<uint32_t> material_words(size_t(materials) * 8);
  for (uint32_t m = 0; m < materials; ++m) {
    HitMaterial& hm = material_list[m];
    hm.word = PackMaterialWord(rng.Below(256), rng.Below(16), rng.Below(8), rng.Below(256), rng.Below(256));
    hm.texture = m % 4 == 0 ? kNoTexture : uint16_t(rng.Below(textures));
    for (int c = 0; c < 3; ++c) {
      hm.tint[c] = rng.Range(0.0f, 2.0f);
      hm.average[c] = rng.Range(0.0f, 1.5f);
    }
    hm.emissive = m % 6 == 0 ? rng.Range(0.5f, 20.0f) : 0.0f;
    hm.Pack(&material_words[size_t(m) * 8]);
  }
  std::vector<HitTexture> texture_list(textures);
  std::vector<GpuTextureRow> texture_rows(textures);
  for (uint32_t t = 0; t < textures; ++t) {
    HitTexture& tx = texture_list[t];
    tx.width = uint16_t(64u << rng.Below(4));
    tx.height = uint16_t(64u << rng.Below(4));
    tx.format = static_cast<HitTextureFormat>(rng.Below(4));
    tx.mip_count = uint8_t(1 + rng.Below(8));
    tx.flags = uint8_t(rng.Below(4));
    texture_rows[t] = MakeTextureRow(tx, t % 3 == 0 ? kNoSlot : 100 + t, rng.Below(3));
  }

  // the sectors: some full, one with runs but no corners, one geometry only, one never built
  const uint32_t sector_count = 5;
  std::vector<SynthSector> sectors;
  uint64_t pool_end = 64;  // some bytes in front, so no block starts at 0
  for (uint32_t s = 0; s < sector_count; ++s) {
    for (int lod = 0; lod < 2; ++lod) {
      SynthSector ss;
      ss.sector = s;
      ss.lod = lod != 0;
      ss.runs = !(s == 2 && lod == 0);                // geometry only
      ss.corners = ss.runs && !(s == 1 && lod == 0);  // runs without corners
      const uint32_t tri = 40 + rng.Below(500);
      const uint32_t vertices = 20 + rng.Below(300);
      const uint32_t opaque = (s + uint32_t(lod)) % 3 == 0 ? tri : (s + uint32_t(lod)) % 3 == 1 ? 0 : tri / 2;
      ss.mesh.positions.resize(size_t(vertices) * 3);
      for (float& p : ss.mesh.positions) p = rng.Range(-2000.0f, 2000.0f);
      ss.mesh.indices.resize(size_t(tri) * 3);
      for (uint32_t& i : ss.mesh.indices) i = rng.Below(vertices);
      ss.mesh.opaque_triangles = opaque;
      ss.blob = RandomBlob(rng, tri, opaque, materials, ss.corners);
      ss.plan = PlanPoolBlock(vertices, tri, ss.runs ? uint32_t(ss.blob.runs.size()) : 0, ss.runs, ss.corners);
      ss.base = pool_end + 16 * rng.Below(5);
      pool_end = ss.base + ss.plan.total_bytes;
      sectors.push_back(std::move(ss));
    }
  }
  std::vector<uint8_t> pool(size_t(pool_end), 0xA5);
  std::vector<uint32_t> slots(size_t(sector_count) * 2 * 8, 0);
  for (SynthSector& ss : sectors) {
    std::string error;
    Check(FillPoolBlock(ss.plan, ss.mesh, ss.runs ? &ss.blob : nullptr, pool.data() + ss.base, size_t(ss.plan.total_bytes), &error), "the synthetic block is written: " + error);
    GpuSectorRow row;
    const bool made = MakeSectorRow(ss.plan, ss.base, ss.mesh.triangle_count(), ss.mesh.opaque_triangles, ss.sector == 3 ? kSectorRowFoliage : 0, &row);
    Check(made, "the synthetic row is made");
    std::memcpy(&slots[size_t(SectorRowOf(ss.sector, ss.lod)) * 8], &row, sizeof(row));
  }
  // sector 4's hi detail is "not built": its slot stays all zero (flags 0)
  std::memset(&slots[size_t(SectorRowOf(4, false)) * 8], 0, 32);

  // the buffers the shim reads
  std::vector<hlsl_shim::uint4> slot_rows(slots.size() / 4), material_rows(material_words.size() / 4), texture_table(textures);
  for (size_t i = 0; i < slot_rows.size(); ++i) slot_rows[i] = hlsl_shim::uint4(slots[4 * i], slots[4 * i + 1], slots[4 * i + 2], slots[4 * i + 3]);
  for (size_t i = 0; i < material_rows.size(); ++i)
    material_rows[i] = hlsl_shim::uint4(material_words[4 * i], material_words[4 * i + 1], material_words[4 * i + 2], material_words[4 * i + 3]);
  for (uint32_t t = 0; t < textures; ++t) texture_table[t] = hlsl_shim::uint4(texture_rows[t].slot, texture_rows[t].size, texture_rows[t].mips, texture_rows[t].reserved);
  hlsl_shim::gCityPool.data = pool.data();
  hlsl_shim::gCityPool.size = pool.size();
  hlsl_shim::gCitySectors.data = slot_rows.data();
  hlsl_shim::gCitySectors.count = slot_rows.size();
  hlsl_shim::gCityMaterials.data = material_rows.data();
  hlsl_shim::gCityMaterials.count = material_rows.size();
  hlsl_shim::gCityTextures.data = texture_table.data();
  hlsl_shim::gCityTextures.count = texture_table.size();

  HitView view;
  view.pool = pool.data();
  view.pool_size = pool.size();
  view.sectors = slots.data();
  view.sector_slots = slots.size() / 8;
  view.materials = material_words.data();
  view.material_count = materials;

  int invalid_mismatch = 0, triangle_wrong = 0, material_wrong = 0, position_wrong = 0, geometry_wrong = 0, uv_wrong = 0, colour_wrong = 0, twin_wrong = 0, shim_wrong = 0, flags_wrong = 0;
  double worst_shim_uv = 0.0;
  long hits = 0;
  using namespace hlsl_shim;
  for (const SynthSector& ss : sectors) {
    const uint32_t id = mcla::gpu::city_rt::InstanceId(ss.sector, ss.lod ? mcla::gpu::city_rt::Detail::kLod : mcla::gpu::city_rt::Detail::kHi);
    const uint32_t tri_count = ss.mesh.triangle_count();
    const uint32_t opaque = ss.mesh.opaque_triangles;
    const bool built = !(ss.sector == 4 && !ss.lod);
    for (int n = 0; n < 400; ++n) {
      const uint32_t tri = rng.Below(tri_count);
      // the geometry and primitive a ray query commits: opaque triangles are geometry 0; the alpha tested ones are geometry 1 when there are opaque ones, else 0
      const uint32_t geometry = tri < opaque ? 0u : (opaque != 0 ? 1u : 0u);
      const uint32_t primitive = tri < opaque ? tri : tri - opaque;
      const float b1 = rng.Range(0.0f, 1.0f), b2 = rng.Range(0.0f, 1.0f - b1);
      const DecodedHit d = DecodeHit(view, id, geometry, primitive, b1, b2);
      const CityHit h = CityHitDecode(id, geometry, primitive, float2(b1, b2));
      ++hits;
      if (!built) {
        if (d.valid || h.valid != 0 || d.sector_flags != 0 || h.sectorFlags != 0) ++invalid_mismatch;
        continue;
      }
      if (!d.valid || h.valid != 1) {
        ++invalid_mismatch;
        continue;
      }
      if (d.triangle != tri || h.triangleIndex != tri) ++triangle_wrong;
      // the geometry, against the mesh that went in
      double want_p[3][3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = ss.mesh.indices[size_t(tri) * 3 + size_t(k)];
        for (int c = 0; c < 3; ++c) want_p[k][c] = ss.mesh.positions[size_t(v) * 3 + size_t(c)];
      }
      const float3 hp[3] = {h.p0, h.p1, h.p2};
      for (int k = 0; k < 3; ++k) {
        if (d.p[k][0] != float(want_p[k][0]) || d.p[k][1] != float(want_p[k][1]) || d.p[k][2] != float(want_p[k][2])) ++position_wrong;
        if (hp[k].x != d.p[k][0] || hp[k].y != d.p[k][1] || hp[k].z != d.p[k][2]) ++position_wrong;
      }
      const double e1[3] = {want_p[1][0] - want_p[0][0], want_p[1][1] - want_p[0][1], want_p[1][2] - want_p[0][2]};
      const double e2[3] = {want_p[2][0] - want_p[0][0], want_p[2][1] - want_p[0][1], want_p[2][2] - want_p[0][2]};
      const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
      const double len = std::sqrt(cx * cx + cy * cy + cz * cz);
      const double want_area = 0.5 * len;
      if (len > 1.0) {
        const double nx = cx / len, ny = cy / len, nz = cz / len;
        if (!NearlyEqual(d.normal[0], nx, 2e-3, 0) || !NearlyEqual(d.normal[1], ny, 2e-3, 0) || !NearlyEqual(d.normal[2], nz, 2e-3, 0)) ++geometry_wrong;
        if (!NearlyEqual(h.normal.x, d.normal[0], 1e-6, 0) || !NearlyEqual(h.normal.y, d.normal[1], 1e-6, 0) || !NearlyEqual(h.normal.z, d.normal[2], 1e-6, 0)) ++twin_wrong;
        if (!NearlyEqual(d.world_area, want_area, 0, 1e-3) || !NearlyEqual(h.worldArea, d.world_area, 0, 1e-6)) ++geometry_wrong;
      }
      // the material
      uint32_t want_material = kDefaultMaterial, want_flags = 0;
      const HitRun* run = nullptr;
      if (ss.runs) {
        for (const HitRun& r : ss.blob.runs)
          if (tri >= r.first_triangle && tri < r.first_triangle + r.triangle_count) run = &r;
        want_material = run->material;
        want_flags = run->flags;
      }
      if (d.material != want_material || h.material != want_material) ++material_wrong;
      if (d.run_flags != want_flags || h.runFlags != want_flags) ++flags_wrong;
      const uint32_t want_sector_flags = kSectorRowValid | (ss.runs ? kSectorRowRuns : 0u) | (ss.corners ? kSectorRowCorners : 0u) | (ss.sector == 3 ? kSectorRowFoliage : 0u);
      if (d.sector_flags != want_sector_flags || h.sectorFlags != want_sector_flags) ++flags_wrong;
      // the corners: uv and colour interpolated, against the source corners (their uv is decoded by the run's transform)
      if (ss.corners) {
        double uv[3][2], col[3][4];
        for (int k = 0; k < 3; ++k) {
          float f[2];
          DecodeCornerUv(*run, ss.blob.corners[(size_t(tri) * 3 + size_t(k)) * 2], f);
          uv[k][0] = f[0];
          uv[k][1] = f[1];
          const uint32_t cw = ss.blob.corners[(size_t(tri) * 3 + size_t(k)) * 2 + 1];
          for (int a = 0; a < 4; ++a) col[k][a] = double((cw >> (8 * a)) & 255u) / 255.0;
        }
        const double w0 = 1.0 - double(b1) - double(b2);
        const double want_u = uv[0][0] * w0 + uv[1][0] * b1 + uv[2][0] * b2;
        const double want_v = uv[0][1] * w0 + uv[1][1] * b1 + uv[2][1] * b2;
        const double tol = 1e-5 + 4e-7 * (std::fabs(want_u) + std::fabs(want_v) + 1.0);
        if (!NearlyEqual(d.uv[0], want_u, tol, 0) || !NearlyEqual(d.uv[1], want_v, tol, 0)) ++uv_wrong;
        worst_shim_uv = std::max(worst_shim_uv, std::max(std::fabs(double(h.uv.x) - d.uv[0]), std::fabs(double(h.uv.y) - d.uv[1])));
        if (std::fabs(double(h.uv.x) - d.uv[0]) > 1e-5 || std::fabs(double(h.uv.y) - d.uv[1]) > 1e-5) ++shim_wrong;
        for (int a = 0; a < 4; ++a) {
          const double want_c = col[0][a] * w0 + col[1][a] * b1 + col[2][a] * b2;
          if (!NearlyEqual(d.colour[a], want_c, 1e-5, 0)) ++colour_wrong;
          const float hc[4] = {h.colour.x, h.colour.y, h.colour.z, h.colour.w};
          if (!NearlyEqual(hc[a], d.colour[a], 1e-6, 0)) ++shim_wrong;
        }
        const double want_uv_area = 0.5 * std::fabs((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]));
        if (!NearlyEqual(d.uv_area, want_uv_area, 1e-6, 1e-4) || !NearlyEqual(h.uvArea, d.uv_area, 1e-6, 1e-4)) ++uv_wrong;
      } else {
        if (d.uv[0] != 0.0f || d.uv[1] != 0.0f || d.colour[3] != 1.0f || h.uv.x != 0.0f || h.colour.w != 1.0f || d.uv_area != 0.0f || h.uvArea != 0.0f) ++uv_wrong;
      }
      // the material and texture rows, through the shim
      if (h.material >= materials) {
        ++shim_wrong;
        continue;
      }
      const CityHitMaterial m = CityHitLoadMaterial(h.material);
      const HitMaterial& hm = material_list[h.material];
      const uint32_t w = hm.word;
      const bool material_ok = m.word == w && m.cls == WordClass(w) && m.flags == WordFlags(w) && NearlyEqual(m.smoothness, WordSmoothness(w) / 255.0, 1e-6, 0) && NearlyEqual(m.lit, WordLit(w) / 255.0, 1e-6, 0) &&
                               NearlyEqual(m.metal, WordMetal(w) / 255.0, 1e-6, 0) && m.textureIndex == hm.texture && m.layerIndex == kNoTexture && m.emissive == hm.emissive &&
                               NearlyEqual(m.tint.x, hm.tint[0], 1.0 / 1023.0, 0) && NearlyEqual(m.tint.y, hm.tint[1], 1.0 / 1023.0, 0) && NearlyEqual(m.tint.z, hm.tint[2], 1.0 / 1023.0, 0) &&
                               NearlyEqual(m.average.x, hm.average[0], 1.0 / 1023.0, 0) && NearlyEqual(m.average.y, hm.average[1], 1.0 / 1023.0, 0) && NearlyEqual(m.average.z, hm.average[2], 1.0 / 1023.0, 0);
      if (!material_ok) ++shim_wrong;
      float un[3];
      UnpackRgb10(material_words[size_t(h.material) * 8 + 2], un);
      if (!NearlyEqual(m.tint.x, un[0], 1e-6, 0) || !NearlyEqual(m.tint.y, un[1], 1e-6, 0) || !NearlyEqual(m.tint.z, un[2], 1e-6, 0)) ++shim_wrong;
      const CityHitTexture t = CityHitLoadTexture(m.textureIndex);
      if (m.textureIndex == kNoTexture) {
        if (t.slot != kNoSlot || t.width != 0 || t.mipCount != 0) ++shim_wrong;
      } else {
        const HitTexture& tx = texture_list[m.textureIndex];
        const GpuTextureRow& row = texture_rows[m.textureIndex];
        if (t.slot != row.slot || t.width != tx.width || t.height != tx.height || t.mipCount != (row.mips & 255u) || t.firstMip != ((row.mips >> 8) & 255u) || t.flags != tx.flags) ++shim_wrong;
      }
    }
  }
  Check(hlsl_shim::gCityPool.errors == 0, Num("the HLSL decode made no misaligned or out of range load on the pool (%.0f)", hlsl_shim::gCityPool.errors));
  Check(hlsl_shim::gCitySectors.errors == 0 && hlsl_shim::gCityMaterials.errors == 0 && hlsl_shim::gCityTextures.errors == 0, "... nor on the tables");
  Check(invalid_mismatch == 0, Num("a slot that is not built is invalid in both decodes, a built one valid (%.0f)", invalid_mismatch));
  Check(triangle_wrong == 0, Num("the triangle of (geometry, primitive) is the triangle that went in, with and without opaque triangles (%.0f wrong of %.0f hits)", triangle_wrong, double(hits)));
  Check(position_wrong == 0, Num("the three positions are exactly the mesh's, in both decodes (%.0f)", position_wrong));
  Check(geometry_wrong == 0 && twin_wrong == 0, Num("the geometric normal and the world area match an independent double computation (%.0f, %.0f)", geometry_wrong, twin_wrong));
  Check(material_wrong == 0 && flags_wrong == 0, Num("the material id, the run's flags and the slot's flags are the run's and the sector's (%.0f, %.0f)", material_wrong, flags_wrong));
  Check(uv_wrong == 0, Num("the uv and its area match the corners that went in (%.0f)", uv_wrong));
  Check(colour_wrong == 0, Num("the interpolated vertex colour matches (%.0f)", colour_wrong));
  Check(shim_wrong == 0, Num("the HLSL decode equals the C++ twin: uv to %.1e, colour, material, texture rows (%.0f wrong)", worst_shim_uv, shim_wrong));
  Check(hits == long(sectors.size()) * 400, "the decode ran 400 hits in each of the ten sector details");
  {
    CityHit density = CityHitEmpty();
    density.worldArea = 4.0f;
    density.uvArea = 1.0f;
    CityHit flat = CityHitEmpty();
    Check(NearlyEqual(CityHitUvDensity(density), 0.5, 1e-6, 0) && CityHitUvDensity(flat) == 0.0f, "the uv density is sqrt(uv area / world area), 0 for a triangle with no area");
  }

  // a slot index past the table, and a triangle past the sector: invalid, not a crash
  const DecodedHit past_slot = DecodeHit(view, mcla::gpu::city_rt::InstanceId(sector_count + 10, mcla::gpu::city_rt::Detail::kHi), 0, 0, 0.2f, 0.2f);
  Check(!past_slot.valid, "an instance past the sector table is invalid");
  const SynthSector& first = sectors[0];
  const uint32_t id0 = mcla::gpu::city_rt::InstanceId(0, mcla::gpu::city_rt::Detail::kHi);
  Check(!DecodeHit(view, id0, 0, first.mesh.triangle_count() + 5, 0.2f, 0.2f).valid && CityHitDecode(id0, 0, first.mesh.triangle_count() + 5, float2(0.2f, 0.2f)).valid == 0, "a triangle past the sector's is invalid in both");
}

}  // namespace

int main(int argc, char** argv) {
  std::string hlsli = argc > 1 ? argv[1] : std::string();
  if (hlsli.empty()) {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    hlsli = (slash == std::string::npos ? std::string() : here.substr(0, slash + 1)) + "../gpu/hlsl/city_hit_common.hlsli";
  }
  SharedNumberChecks();
  HalfChecks();
  RecordChecks();
  UvChecks();
  CornerChecks();
  BlobChecks();
  FileChecks();
  PoolChecks();
  const std::string text = ReadFile(hlsli);
  Check(!text.empty(), "the .hlsli is readable: " + hlsli);
  if (!text.empty()) HlslDefinesChecks(text);
  DecodeChecks();
  const HitBudget budget;
  Check(!budget.on() && budget.pool_bytes == 0 && budget.texture_upload_bytes == (8u << 20), "a default budget is off");
  std::printf("city_hit_format_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
