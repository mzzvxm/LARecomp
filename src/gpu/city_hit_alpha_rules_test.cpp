// Check of the exact alpha of the city's cut-outs (hlsl/city_hit_alpha.hlsli and its C++ twin city_hit_alpha_rules.h): CITY-RT M1c. A program of its own: no renderer, no game files, no GPU;
// exit 0 = passed.
//
//   build: cmake --build <dir> --target larecomp_city_hit_alpha_rules_test, then run src/gpu/larecomp_city_hit_alpha_rules_test [<src/gpu dir>]
//   or by hand: clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/city_hit_alpha_rules_test.cpp src/citypack/city_hit_format.cpp src/citypack/pack_format.cpp
//
// It checks: the twin's arithmetic (the footprint, the level against the shape limit and the class's cap, the bilinear read and its wrap, the reference), then compiles the .hlsli ITSELF as C++
// (through a shim of the HLSL types, the pool, the tables and a fake texture set) and decodes the candidates of a synthetic city: a fence quad whose texture holds bars, in sectors that have
// everything and in sectors that lack one thing each (no corners, no uv in the run, no texture on the material, a texture that is not resident, a hit that is not in the pool). The shader's
// answer is compared with the twin's over thousands of random hits, the "not known" cases answer known = 0 (the caller falls back to the stochastic rule), and the shadow shader's own text is
// read for the wiring (the macro, the register space, the non uniform index, the candidate's values).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "city_hit_alpha_rules.h"
#include "city_rt_rules.h"

using namespace larecomp::citypack;
namespace ar = mcla::gpu::city_hit_alpha;

// ---------------------------------------------------------------------------
// the .hlsli as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct int2 {
  int x = 0, y = 0;
  int2() = default;
  int2(int a, int b) : x(a), y(b) {}
};
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
inline float floor(float v) { return std::floor(v); }
inline float log2(float v) { return std::log2(v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
inline float3 asfloat(uint3 v) { return float3(asfloat(v.x), asfloat(v.y), asfloat(v.z)); }
inline float f16tof32(uint v) {
  const int exponent = int((v >> 10) & 31u);
  const int mantissa = int(v & 1023u);
  const float sign = (v & 0x8000u) != 0 ? -1.0f : 1.0f;
  if (exponent == 0) return sign * std::ldexp(float(mantissa), -24);
  if (exponent == 31) return sign * INFINITY;
  return sign * std::ldexp(float(1024 + mantissa), exponent - 25);
}

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

// The fake texture set: slot (gamma bit already masked) -> a texture's resident levels, each a row-major grid of alpha.
struct FakeTexture {
  uint width = 0, height = 0;  // the first resident level's size
  std::vector<std::vector<float>> levels;
};
std::vector<FakeTexture> gFakeTextures;
uint gFakeFirstSlot = 100;
int gFakeErrors = 0;
int gFakeReads = 0;
inline float FakeAlphaTexel(uint slot, int2 texel, uint level) {
  ++gFakeReads;
  if (slot < gFakeFirstSlot || slot - gFakeFirstSlot >= gFakeTextures.size()) {
    ++gFakeErrors;
    return 0.0f;
  }
  const FakeTexture& t = gFakeTextures[slot - gFakeFirstSlot];
  if (level >= t.levels.size()) {
    ++gFakeErrors;
    return 0.0f;
  }
  const int w = std::max(int(t.width) >> level, 1), h = std::max(int(t.height) >> level, 1);
  if (texel.x < 0 || texel.y < 0 || texel.x >= w || texel.y >= h) {  // the shader wraps first: a coordinate outside the level is a bug
    ++gFakeErrors;
    return 0.0f;
  }
  return t.levels[level][size_t(texel.y) * size_t(w) + size_t(texel.x)];
}
#define CITY_HIT_NO_RESOURCES
#define CITY_HIT_ALPHA_TEXEL(slot, texel, level) hlsl_shim::FakeAlphaTexel(((slot) & 0x7FFFFFFFu), (texel), (level))
#include "hlsl/city_hit_alpha.hlsli"
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
#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond)

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : (U32() >> 8) % n; }
};

// ---------------------------------------------------------------------------
// the twin's arithmetic
// ---------------------------------------------------------------------------
void TwinChecks() {
  CHECK(ar::kShapeMinLog2 == 3);
  CHECK(ar::ConeWidth(0.01f, 0.0f, 0.014f) == 0.01f && std::fabs(ar::ConeWidth(0.01f, 10.0f, 0.014f) - (0.01f + 0.28f)) < 1e-6f);
  CHECK(std::fabs(ar::Texels(0.5f, 0.5f, 64, 256) - 0.25f * 128.0f) < 1e-4f);  // 0.5 m of a 0.5 repeats a metre texture of 64 x 256 (geometric mean 128)

  // the shape limit: the lowest level that keeps 8 texels on the short side
  CHECK(ar::ShapeLimit(256, 256) == 5 && ar::ShapeLimit(64, 64) == 3 && ar::ShapeLimit(8, 8) == 0 && ar::ShapeLimit(4, 4) == 0 && ar::ShapeLimit(1, 1) == 0);
  CHECK(ar::ShapeLimit(1024, 8) == 0 && ar::ShapeLimit(8, 1024) == 0 && ar::ShapeLimit(512, 64) == 3 && ar::ShapeLimit(100, 100) == 3 && ar::ShapeLimit(0, 0) == 0);

  // the level: the footprint's, never coarser than the limit, from the first resident one, inside the stored chain
  CHECK(ar::Level(64, 64, 7, 0, 0.0f) == 0 && ar::Level(64, 64, 7, 0, 1.0f) == 0 && ar::Level(64, 64, 7, 0, 0.9f) == 0);
  CHECK(ar::Level(64, 64, 7, 0, 2.0f) == 1 && ar::Level(64, 64, 7, 0, 1.4f) == 0 && ar::Level(64, 64, 7, 0, 1.5f) == 1 && ar::Level(64, 64, 7, 0, 4.0f) == 2 && ar::Level(64, 64, 7, 0, 8.0f) == 3);
  CHECK(ar::Level(64, 64, 7, 0, 64.0f) == 3 && ar::Level(64, 64, 7, 0, 1e9f) == 3);  // the shape limit: 8 x 8 and no coarser
  CHECK(ar::Level(64, 64, 7, 1, 1e9f) == 2 && ar::Level(64, 64, 7, 1, 2.0f) == 0 && ar::Level(64, 64, 7, 1, 4.0f) == 1);  // the class's cap dropped level 0: counted from level 1
  CHECK(ar::Level(64, 64, 2, 0, 1e9f) == 1 && ar::Level(64, 64, 1, 0, 1e9f) == 0 && ar::Level(64, 64, 0, 0, 1e9f) == 0);  // a short stored chain: the last stored level
  CHECK(ar::Level(256, 256, 9, 2, 1e9f) == 3 && ar::Level(256, 256, 9, 5, 1e9f) == 0 && ar::Level(256, 256, 9, 7, 1e9f) == 0);
  uint32_t previous = 0;
  for (int i = 0; i <= 200; ++i) {
    const float texels = std::pow(1.07f, float(i)) - 0.5f;
    const uint32_t level = ar::Level(512, 256, 10, 1, texels);
    CHECK(level >= previous && level <= 4);  // monotone, and never past the limit (short side 256: level 5 from the full size, 4 from the first resident one)
    previous = level;
  }

  // the bilinear read: a texel centre is the texel, a middle is the mean, the wrap is exact
  const auto checker = [](int x, int y) { return ((x + y) & 1) ? 1.0f : 0.0f; };
  CHECK(ar::Bilinear(checker, 0.5f / 8.0f, 0.5f / 8.0f, 8, 8, 0, 0) == 0.0f && ar::Bilinear(checker, 1.5f / 8.0f, 0.5f / 8.0f, 8, 8, 0, 0) == 1.0f);
  CHECK(std::fabs(ar::Bilinear(checker, 1.0f / 8.0f, 1.0f / 8.0f, 8, 8, 0, 0) - 0.5f) < 1e-6f);
  for (float u : {0.1f, 0.37f, 0.93f}) {
    for (float v : {0.05f, 0.5f, 0.77f}) {
      const float base = ar::Bilinear(checker, u, v, 8, 8, 0, 0);
      CHECK(std::fabs(ar::Bilinear(checker, u + 3.0f, v - 2.0f, 8, 8, 0, 0) - base) < 1e-4f);
    }
  }
  int outside = 0;
  const auto strict = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= 4 || y >= 4) ++outside;  // level 1 of an 8 x 8: 4 x 4
    return 1.0f;
  };
  for (float u = -3.0f; u < 3.0f; u += 0.173f) ar::Bilinear(strict, u, -u * 0.7f, 8, 8, 0, 1);
  CHECK(outside == 0);
  // the level's size is the full size shifted by the first resident level and the level
  int sized = 0;
  const auto sizes = [&](int x, int y) {
    if (x >= 2 || y >= 2) ++sized;  // 32 >> (2 + 2) = 2
    return 0.0f;
  };
  ar::Bilinear(sizes, 0.99f, 0.99f, 32, 32, 2, 2);
  CHECK(sized == 0);

  // the reference
  CHECK(ar::Blocks(0.5f, 0.5f) && ar::Blocks(0.51f, 0.5f) && !ar::Blocks(0.49f, 0.5f) && ar::Blocks(1.0f, 0.98f) && !ar::Blocks(0.0f, 0.02f));
}

// ---------------------------------------------------------------------------
// a synthetic city of fences
// ---------------------------------------------------------------------------

// A fence texture: bars 2 texels wide every 8 on a 64 x 64 base, the chain made by box filters (what an alpha mip is).
std::vector<std::vector<float>> FenceChain(uint32_t size, uint32_t first_level, uint32_t levels_total) {
  std::vector<std::vector<float>> all(levels_total);
  all[0].resize(size_t(size) * size);
  for (uint32_t y = 0; y < size; ++y)
    for (uint32_t x = 0; x < size; ++x) all[0][size_t(y) * size + x] = (x % 8) < 2 ? 1.0f : 0.0f;
  for (uint32_t l = 1; l < levels_total; ++l) {
    const uint32_t w = std::max(size >> l, 1u), pw = std::max(size >> (l - 1), 1u);
    all[l].resize(size_t(w) * w);
    for (uint32_t y = 0; y < w; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        float sum = 0.0f;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) sum += all[l - 1][size_t(std::min(y * 2 + dy, pw - 1)) * pw + std::min(x * 2 + dx, pw - 1)];
        all[l][size_t(y) * w + x] = sum * 0.25f;
      }
  }
  return std::vector<std::vector<float>>(all.begin() + first_level, all.end());
}

struct Sector {
  MeshData mesh;
  HitBlob blob;
  bool runs = true, corners = true;
  PoolBlockPlan plan;
  uint64_t base = 0;
  uint32_t sector = 0;
};

// The fence: a 4 m x 2 m quad at z = 0 (two cut-out triangles), the texture repeating twice across it, and optionally an opaque floor triangle before it (so the cut-outs are geometry 1).
Sector MakeSector(uint32_t sector, bool with_floor, uint32_t material, uint32_t run_flags, bool runs, bool corners) {
  Sector s;
  s.sector = sector;
  s.runs = runs;
  s.corners = corners && runs;
  s.mesh.positions = {0, 0, 0, 4, 0, 0, 4, 2, 0, 0, 2, 0,   // the fence
                      0, 0, -10, 10, 0, -10, 10, 0, 0};     // the floor
  const uint32_t floor_tris = with_floor ? 1 : 0;
  if (with_floor) s.mesh.indices = {4, 5, 6};
  for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) s.mesh.indices.push_back(i);
  s.mesh.opaque_triangles = floor_tris;
  if (!with_floor) s.mesh.positions.resize(12);  // the floor's vertices are not there either
  const uint32_t triangles = s.mesh.triangle_count();
  s.blob.triangle_count = triangles;
  s.blob.opaque_triangles = floor_tris;
  s.blob.flags = s.corners ? kHitBlobHasCorners : 0;
  if (with_floor) {
    HitRun floor_run;
    floor_run.first_triangle = 0;
    floor_run.triangle_count = 1;
    floor_run.material = 0;
    floor_run.flags = kRunHasUv;
    ChooseUvTransform(0, 1, 0, 1, &floor_run);
    s.blob.runs.push_back(floor_run);
  }
  HitRun fence;
  fence.first_triangle = floor_tris;
  fence.triangle_count = 2;
  fence.material = material;
  fence.flags = run_flags;
  ChooseUvTransform(0.0f, 2.0f, 0.0f, 1.0f, &fence);
  s.blob.runs.push_back(fence);
  if (s.corners) {
    if (with_floor) {
      for (int k = 0; k < 3; ++k) {
        s.blob.corners.push_back(EncodeCornerUv(s.blob.runs[0], k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f));
        s.blob.corners.push_back(0xFF808080u);
      }
    }
    const float tri_uv[2][3][2] = {{{0, 0}, {2, 0}, {2, 1}}, {{0, 0}, {2, 1}, {0, 1}}};
    for (int t = 0; t < 2; ++t)
      for (int k = 0; k < 3; ++k) {
        s.blob.corners.push_back(EncodeCornerUv(fence, tri_uv[t][k][0], tri_uv[t][k][1]));
        s.blob.corners.push_back(0xFF808080u);
      }
  }
  return s;
}

struct World {
  std::vector<Sector> sectors;
  std::vector<uint8_t> pool;
  std::vector<uint32_t> slots;
  std::vector<uint32_t> material_words;
  std::vector<GpuTextureRow> rows;
  std::vector<HitTexture> textures;
};

// Sectors (each: hi detail only), materials and textures:
//   sector 0  floor + fence, material 1 (texture 0, resident from level `first_mip`)
//   sector 1  fence only (geometry 0), material 1
//   sector 2  runs but no corners
//   sector 3  corners but the run says no uv
//   sector 4  material 3: no texture
//   sector 5  material 2: texture 1, not resident
//   sector 6  not built (its slot stays zero)
//   sector 7  no runs at all (geometry only)
World Build(uint32_t first_mip, uint32_t resident_levels) {
  World w;
  const uint32_t full_levels = 7;  // 64 .. 1
  // textures: 0 resident, 1 not resident
  for (int i = 0; i < 2; ++i) {
    HitTexture t;
    t.width = 64;
    t.height = 64;
    t.format = HitTextureFormat::kBc3;
    t.mip_count = uint8_t(full_levels);
    t.flags = kTextureAlpha | (i == 0 ? kTextureGamma : 0);
    w.textures.push_back(t);
  }
  w.rows.push_back(MakeTextureRow(w.textures[0], 100, first_mip));
  GpuTextureRow gone = MakeTextureRow(w.textures[1], kNoSlot, 0);
  w.rows.push_back(gone);
  hlsl_shim::gFakeTextures.clear();
  hlsl_shim::FakeTexture fake;
  fake.width = std::max(64u >> first_mip, 1u);
  fake.height = fake.width;
  fake.levels = FenceChain(64, first_mip, full_levels);
  fake.levels.resize(std::min<size_t>(fake.levels.size(), resident_levels));
  hlsl_shim::gFakeTextures.push_back(fake);
  w.rows[0] = MakeTextureRow(w.textures[0], 100, first_mip);
  w.rows[0].mips = (uint32_t(fake.levels.size()) & 0xFFu) | ((first_mip & 0xFFu) << 8) | (uint32_t(w.textures[0].flags) << 16);

  // materials: 0 default, 1 fence (texture 0), 2 fence (texture 1), 3 none
  std::vector<HitMaterial> mats(4);
  mats[1].texture = 0;
  mats[2].texture = 1;
  mats[3].texture = kNoTexture;
  mats[1].word = PackMaterialWord(30, 1, kWordFlagAlphaCovered, 255, 0);
  w.material_words.resize(4 * 8);
  for (uint32_t m = 0; m < 4; ++m) mats[m].Pack(&w.material_words[size_t(m) * 8]);

  const uint32_t fence_flags = kRunAlphaTested | kRunHasUv | kRunHasColour;
  w.sectors.push_back(MakeSector(0, true, 1, fence_flags, true, true));
  w.sectors.push_back(MakeSector(1, false, 1, fence_flags, true, true));
  w.sectors.push_back(MakeSector(2, true, 1, fence_flags, true, false));
  w.sectors.push_back(MakeSector(3, true, 1, kRunAlphaTested | kRunHasColour, true, true));
  w.sectors.push_back(MakeSector(4, true, 3, fence_flags, true, true));
  w.sectors.push_back(MakeSector(5, true, 2, fence_flags, true, true));
  w.sectors.push_back(MakeSector(6, true, 1, fence_flags, true, true));
  w.sectors.push_back(MakeSector(7, true, 1, fence_flags, false, false));
  uint64_t end = 64;
  for (Sector& s : w.sectors) {
    s.plan = PlanPoolBlock(s.mesh.vertex_count(), s.mesh.triangle_count(), s.runs ? uint32_t(s.blob.runs.size()) : 0, s.runs, s.corners);
    s.base = end;
    end += s.plan.total_bytes;
    end = (end + 15) / 16 * 16;
  }
  w.pool.assign(size_t(end), 0xA5);
  w.slots.assign(w.sectors.size() * 2 * 8, 0);
  for (Sector& s : w.sectors) {
    std::string error;
    if (!FillPoolBlock(s.plan, s.mesh, s.runs ? &s.blob : nullptr, w.pool.data() + s.base, size_t(s.plan.total_bytes), &error)) std::printf("FillPoolBlock: %s\n", error.c_str());
    GpuSectorRow row;
    MakeSectorRow(s.plan, s.base, s.mesh.triangle_count(), s.mesh.opaque_triangles, 0, &row);
    if (s.sector == 6) row = GpuSectorRow{};  // not built
    std::memcpy(&w.slots[size_t(SectorRowOf(s.sector, false)) * 8], &row, sizeof(row));
  }
  return w;
}

struct Bound {
  std::vector<hlsl_shim::uint4> slot_rows, material_rows, texture_rows;
  HitView view;
};

void Bind(World& w, Bound* b) {
  b->slot_rows.resize(w.slots.size() / 4);
  for (size_t i = 0; i < b->slot_rows.size(); ++i) b->slot_rows[i] = hlsl_shim::uint4(w.slots[4 * i], w.slots[4 * i + 1], w.slots[4 * i + 2], w.slots[4 * i + 3]);
  b->material_rows.resize(w.material_words.size() / 4);
  for (size_t i = 0; i < b->material_rows.size(); ++i)
    b->material_rows[i] = hlsl_shim::uint4(w.material_words[4 * i], w.material_words[4 * i + 1], w.material_words[4 * i + 2], w.material_words[4 * i + 3]);
  b->texture_rows.resize(w.rows.size());
  for (size_t i = 0; i < w.rows.size(); ++i) b->texture_rows[i] = hlsl_shim::uint4(w.rows[i].slot, w.rows[i].size, w.rows[i].mips, w.rows[i].reserved);
  hlsl_shim::gCityPool.data = w.pool.data();
  hlsl_shim::gCityPool.size = w.pool.size();
  hlsl_shim::gCitySectors.data = b->slot_rows.data();
  hlsl_shim::gCitySectors.count = b->slot_rows.size();
  hlsl_shim::gCityMaterials.data = b->material_rows.data();
  hlsl_shim::gCityMaterials.count = b->material_rows.size();
  hlsl_shim::gCityTextures.data = b->texture_rows.data();
  hlsl_shim::gCityTextures.count = b->texture_rows.size();
  b->view.pool = w.pool.data();
  b->view.pool_size = w.pool.size();
  b->view.sectors = w.slots.data();
  b->view.sector_slots = w.slots.size() / 8;
  b->view.materials = w.material_words.data();
  b->view.material_count = w.material_words.size() / 8;
}

// The candidate's alpha the way the twin would work it out, from the C++ decode (DecodeHit) and city_hit_alpha_rules.h: the shader's reference.
struct Expect {
  bool known = false;
  float alpha = 1.0f;
  uint32_t level = 0;
  float uv[2] = {0, 0};
};
Expect Reference(World& w, const Bound& b, uint32_t sector, uint32_t geometry, uint32_t primitive, float b1, float b2, float cone) {
  Expect e;
  const uint32_t instance = mcla::gpu::city_rt::kInstanceIdFlag | (sector << 1);
  const DecodedHit d = DecodeHit(b.view, instance, geometry, primitive, b1, b2);
  if (!d.valid) return e;
  const uint32_t slot_base = SectorRowOf(sector, false) * 8;
  const uint32_t flags = w.slots[slot_base + 7];
  if ((flags & (kSectorRowRuns | kSectorRowCorners)) != (kSectorRowRuns | kSectorRowCorners) || (d.run_flags & kRunHasUv) == 0) return e;
  const uint32_t material_word1 = w.material_words[size_t(d.material) * 8 + 1];
  const uint32_t texture_index = material_word1 & 0xFFFFu;
  if (texture_index == kNoTexture || texture_index >= w.rows.size()) return e;
  const GpuTextureRow& row = w.rows[texture_index];
  const uint32_t mips = row.mips & 255u, first = (row.mips >> 8) & 255u;
  const uint32_t width = row.size & 0xFFFFu, height = row.size >> 16;
  if (row.slot == kNoSlot || mips == 0 || width == 0 || height == 0) return e;
  const float density = std::sqrt(d.uv_area / std::max(d.world_area, 1e-12f));
  e.level = ar::Level(width, height, mips, first, ar::Texels(cone, density, width, height));
  const hlsl_shim::FakeTexture& fake = hlsl_shim::gFakeTextures[(row.slot & 0x7FFFFFFFu) - hlsl_shim::gFakeFirstSlot];
  e.alpha = ar::Bilinear([&](int x, int y) { return fake.levels[e.level][size_t(y) * size_t(std::max(int(fake.width) >> e.level, 1)) + size_t(x)]; }, d.uv[0], d.uv[1], width, height, first, e.level);
  e.uv[0] = d.uv[0];
  e.uv[1] = d.uv[1];
  e.known = true;
  return e;
}

void CompareAll(World& w, const char* name, uint32_t samples, uint32_t first_mip, uint32_t resident) {
  Bound bound;
  Bind(w, &bound);
  hlsl_shim::gCityPool.errors = hlsl_shim::gCitySectors.errors = hlsl_shim::gCityMaterials.errors = hlsl_shim::gCityTextures.errors = 0;
  hlsl_shim::gFakeErrors = 0;
  Lcg rng;
  uint32_t known = 0, unknown = 0, level_wrong = 0, alpha_wrong = 0, known_wrong = 0, blocks = 0, passes = 0;
  double worst = 0.0;
  uint32_t levels_seen[8] = {};
  for (uint32_t i = 0; i < samples; ++i) {
    const uint32_t sector = rng.Below(8);
    const bool floor_first = sector != 1;  // every sector but the second has the opaque floor triangle: the cut-outs are its geometry 1
    const uint32_t geometry = floor_first ? 1u : 0u;
    const uint32_t primitive = rng.Below(2);
    float b1 = rng.Next(), b2 = rng.Next();
    if (b1 + b2 > 1.0f) {
      b1 = 1.0f - b1;
      b2 = 1.0f - b2;
    }
    // a spread of footprints: a pixel's width at 5 m to a spread over 200 m, on a log scale
    const float cone = 0.001f * std::pow(1.07f * 1.0f, float(rng.Below(120)));
    const Expect e = Reference(w, bound, sector, geometry, primitive, b1, b2, cone);
    const hlsl_shim::CityHitAlpha a = hlsl_shim::CityHitAlphaOfCandidate(mcla::gpu::city_rt::kInstanceIdFlag | (sector << 1), geometry, primitive, hlsl_shim::float2(b1, b2), cone);
    if ((a.known != 0) != e.known) {
      ++known_wrong;
      continue;
    }
    if (!e.known) {
      ++unknown;
      if (a.alpha != 1.0f || a.level != 0) ++alpha_wrong;  // the not-known answer is alpha 1, level 0
      continue;
    }
    ++known;
    if (a.level != e.level) ++level_wrong;
    worst = std::max(worst, double(std::fabs(a.alpha - e.alpha)));
    if (std::fabs(a.alpha - e.alpha) > 1e-4f) ++alpha_wrong;
    levels_seen[std::min(a.level, 7u)]++;
    (ar::Blocks(a.alpha, 0.5f) ? blocks : passes)++;
  }
  Check(known_wrong == 0, std::string(name) + ": the shader knows exactly what the twin knows (" + std::to_string(known_wrong) + " differ)");
  Check(level_wrong == 0 && alpha_wrong == 0, std::string(name) + ": level and alpha equal the twin's (" + std::to_string(level_wrong) + " levels, " + std::to_string(alpha_wrong) + " alphas wrong, worst " + std::to_string(worst) + ")");
  Check(known > samples / 5 && unknown > samples / 8, std::string(name) + ": both kinds of candidate were met (" + std::to_string(known) + " known, " + std::to_string(unknown) + " not)");
  // the bars are 2 of 8 texels: a level of 8 x 8 texels (3 below the full size) averages every texel to a quarter, which no reference of 0.5 keeps: only the finer levels have bars to block
  if (first_mip <= 1) Check(blocks > 0 && passes > 0, std::string(name) + ": the bars block some rays and let others through (" + std::to_string(blocks) + " / " + std::to_string(passes) + ")");
  if (first_mip >= 3) Check(blocks == 0 && passes > 0, std::string(name) + ": at 8 x 8 texels the bars are a grey of a quarter, no blocker");
  Check(hlsl_shim::gCityPool.errors == 0 && hlsl_shim::gCitySectors.errors == 0 && hlsl_shim::gCityMaterials.errors == 0 && hlsl_shim::gCityTextures.errors == 0, std::string(name) + ": no misaligned or out of range load on the pool or the tables");
  Check(hlsl_shim::gFakeErrors == 0, std::string(name) + ": every texel read is inside the level it names, of a slot that exists");
  uint32_t distinct = 0;
  for (uint32_t l : levels_seen) distinct += l != 0 ? 1u : 0u;
  if (resident >= 2 && first_mip <= 2) Check(distinct >= 2, std::string(name) + ": more than one level was read");
  if (resident == 1) Check(distinct == 1, std::string(name) + ": one stored level is the one that is read");
}

void ShimChecks() {
  // the defines against the twin
  Check(CITY_HIT_ALPHA_SHAPE_MIN_LOG2 == ar::kShapeMinLog2, "the .hlsli's shape limit number is the twin's");
  Check(hlsl_shim::CityHitAlphaShapeLimit(hlsl_shim::CityHitTexture{0, 64, 64, 7, 0, 0}) == ar::ShapeLimit(64, 64), "the shape limit of 64 x 64");
  Check(hlsl_shim::CityHitAlphaShapeLimit(hlsl_shim::CityHitTexture{0, 1024, 8, 11, 0, 0}) == 0 && hlsl_shim::CityHitAlphaShapeLimit(hlsl_shim::CityHitTexture{0, 512, 64, 10, 0, 0}) == 3, "and of strips");
  for (uint32_t first : {0u, 1u, 3u}) {
    for (uint32_t resident : {1u, 4u, 7u}) {
      if (first + resident > 7) continue;
      World w = Build(first, resident);
      const std::string name = "first level " + std::to_string(first) + ", " + std::to_string(resident) + " resident";
      CompareAll(w, name.c_str(), 6000, first, resident);
    }
  }
  World w = Build(0, 7);
  Bound bound;
  Bind(w, &bound);
  const auto at = [&](uint32_t sector, uint32_t geometry, uint32_t primitive, float b1, float b2, float cone) {
    return hlsl_shim::CityHitAlphaOfCandidate(mcla::gpu::city_rt::kInstanceIdFlag | (sector << 1), geometry, primitive, hlsl_shim::float2(b1, b2), cone);
  };
  // each way a candidate is not known
  Check(at(0, 1, 0, 0.3f, 0.3f, 0.01f).known == 1, "sector 0: known");
  Check(at(1, 0, 0, 0.3f, 0.3f, 0.01f).known == 1, "sector 1 (no opaque triangle, geometry 0): known");
  Check(at(2, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "no corners: not known");
  Check(at(3, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "the run has no uv: not known");
  Check(at(4, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "the material has no texture: not known");
  Check(at(5, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "the texture is not resident: not known");
  Check(at(6, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "the sector is not built: not known");
  Check(at(7, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "no runs: not known");
  Check(at(0, 1, 5, 0.3f, 0.3f, 0.01f).known == 0, "a triangle past the sector's: not known");
  // a texture that becomes resident: the same candidate becomes known; one that is given back: not known again
  w.rows[1] = MakeTextureRow(w.textures[1], 101, 0);
  hlsl_shim::FakeTexture extra;
  extra.width = 64;
  extra.height = 64;
  extra.levels = FenceChain(64, 0, 7);
  hlsl_shim::gFakeTextures.push_back(extra);
  Bind(w, &bound);
  Check(at(5, 1, 0, 0.3f, 0.3f, 0.01f).known == 1, "the texture arrived: known");
  w.rows[1].slot = kNoSlot;
  Bind(w, &bound);
  Check(at(5, 1, 0, 0.3f, 0.3f, 0.01f).known == 0, "the texture was given back: not known");
  // the gamma bit of a slot does not matter to the alpha
  w.rows[0].slot |= kGammaBit;
  Bind(w, &bound);
  hlsl_shim::gFakeErrors = 0;
  Check(at(0, 1, 0, 0.3f, 0.3f, 0.01f).known == 1 && hlsl_shim::gFakeErrors == 0, "a slot with the gamma bit reads the same texture");

  // what the picture is: the bars are 2 of 8 texels at the fine level (a quarter coverage) and the quad's uv covers 2 repeats over 4 m: a bar every 0.5 m, 0.125 m wide; at 5 mm a texel
  // is 4 mm, so the bar centre is solid and the gap is clear
  World f = Build(0, 7);
  Bound fb;
  Bind(f, &fb);
  // The second cut-out triangle of the quad (corners (0,0) (2,1) (0,1) in uv) at b1 = u / 2 and b2 = 0.2 - u / 2 has the uv (u, 0.2): a bar is texels 0 and 1, so u = 1/64 is a bar's middle
  // and u = 5/64 the middle of a gap. The sector without the floor has the cut-outs as geometry 0.
  const float bar_u = 1.0f / 64.0f, gap_u = 5.0f / 64.0f;
  const hlsl_shim::CityHitAlpha bar = hlsl_shim::CityHitAlphaOfCandidate(mcla::gpu::city_rt::kInstanceIdFlag | (1u << 1), 0, 1, hlsl_shim::float2(bar_u / 2.0f, 0.2f - bar_u / 2.0f), 0.004f);
  const hlsl_shim::CityHitAlpha gap = hlsl_shim::CityHitAlphaOfCandidate(mcla::gpu::city_rt::kInstanceIdFlag | (1u << 1), 0, 1, hlsl_shim::float2(gap_u / 2.0f, 0.2f - gap_u / 2.0f), 0.004f);
  Check(bar.known && gap.known && bar.alpha > 0.9f && gap.alpha < 0.1f && bar.level == 0 && gap.level == 0, "at a texel's footprint the bar blocks and the gap lets the ray through");
  Check(ar::Blocks(bar.alpha, 0.5f) && !ar::Blocks(gap.alpha, 0.5f), "against the reference: a blocker, and a hole");
  // a wide footprint reads a coarse level (never past the shape limit) and the bars average to their coverage
  const hlsl_shim::CityHitAlpha wide = hlsl_shim::CityHitAlphaOfCandidate(mcla::gpu::city_rt::kInstanceIdFlag | (1u << 1), 0, 1, hlsl_shim::float2(0.3f, 0.2f), 5.0f);
  Check(wide.known && wide.level == uint32_t(ar::ShapeLimit(64, 64)) && wide.alpha > 0.1f && wide.alpha < 0.45f, "a ray as wide as the fence reads the coarsest level that keeps its shape, and sees the bars' coverage");
}

void TextChecks(const std::string& dir) {
  std::ifstream hlsli(dir + "/hlsl/city_hit_alpha.hlsli");
  std::ifstream shader(dir + "/hlsl/city_rt_shadow_cs.hlsl");
  if (!hlsli || !shader) {
    std::printf("  (the shader sources are not at %s: their wiring is not checked)\n", dir.c_str());
    return;
  }
  std::stringstream a, s;
  a << hlsli.rdbuf();
  s << shader.rdbuf();
  const std::string alpha = a.str(), text = s.str();
  Check(alpha.find("#define CITY_HIT_ALPHA_SHAPE_MIN_LOG2 3") != std::string::npos, "the shape limit's number is the twin's (3)");
  Check(alpha.find("#error \"define CITY_HIT_ALPHA_TEXEL") != std::string::npos, "the includer must say how a texel is read");
  Check(text.find("#define CITY_HIT_SPACE 2") != std::string::npos && text.find("#include \"city_hit_alpha.hlsli\"") != std::string::npos, "the shadow pass reads the hit data in the ray tracing set's space");
  Check(text.find("NonUniformResourceIndex((slot) & 0x7FFFFFFFu)") != std::string::npos && text.find(".Load(int3(texel, int(level))).w") != std::string::npos,
        "the alpha is the bindless texture's own, by a non uniform slot with the gamma bit masked");
  Check(text.find("g_textures[] : register(t0, space0)") != std::string::npos && text.find("#define CITY_HIT_ALPHA_TEXEL") != std::string::npos && text.find("g_textures[] : register(t0, space0)") < text.find("#define CITY_HIT_ALPHA_TEXEL"),
        "the texture array is declared before the macro that reads it");
  Check(text.find("query.CandidateInstanceID()") != std::string::npos && text.find("query.CandidateGeometryIndex()") != std::string::npos && text.find("query.CandidatePrimitiveIndex()") != std::string::npos &&
            text.find("query.CandidateTriangleBarycentrics()") != std::string::npos && text.find("query.CandidateTriangleRayT()") != std::string::npos,
        "the candidate's own instance, geometry, primitive, barycentrics and distance are decoded");
  Check(text.find("(instance & kInstanceCity) != 0u") != std::string::npos, "only the city's candidates are read for their alpha (the cars' have no hit data)");
  Check(text.find("kInstanceCity = 0x800000") != std::string::npos && mcla::gpu::city_rt::kInstanceIdFlag == 0x800000u, "the city's instance id flag is city_rt_rules.h's");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "";
  if (dir.empty()) {
    // run from the repository's src folder or from anywhere with the folder given
    for (const char* guess : {"src/gpu", "gpu", "."}) {
      std::ifstream probe(std::string(guess) + "/hlsl/city_hit_alpha.hlsli");
      if (probe) {
        dir = guess;
        break;
      }
    }
  }
  TwinChecks();
  ShimChecks();
  if (!dir.empty()) TextChecks(dir);
  std::printf("city hit alpha test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
