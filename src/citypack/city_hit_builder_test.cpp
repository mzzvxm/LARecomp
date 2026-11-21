// The city hit data's builder (hit_builder.h, CITY-RT M2 package H1) on SYNTHETIC resources in the game's layout (no game files, no renderer): the shader instance parameters, the vertex
// layouts, the corners and the runs, the quantisation, the material table through the G-buffer's rules, the indexed corner stream (round trips, damage, the old reader's view of it),
// and the whole builder on a synthetic RPF3 archive: the corners of every triangle against the source, one thread and four give the same bytes, the reader and the validator, the
// stamp, a census, a cancelled build, a base pack that is not of these files.
//
//   build: see src/citypack/CMakeLists.txt (larecomp_city_hit_builder_test); run it with no argument (exit 0 = passed). It needs src/mc_engine/modloader/{rpf3,aes256,texture}.cpp.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/hit_builder.h"
#include "citypack/hit_collect.h"
#include "citypack/hit_corner_codec.h"
#include "citypack/hit_material.h"
#include "citypack/hit_params.h"
#include "citypack/material_class.h"
#include "citypack/pack_reader.h"
#include "mc_engine/modloader/rpf3.h"

using namespace larecomp::citypack;
namespace fs = std::filesystem;
namespace gb = mcla::gpu::gbuffer;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// ---------------------------------------------------------------------------
// synthetic resources in the game's layout (the pack's own test has the same helpers for the geometry; these add the shader instances' parameters and real vertex declarations)
// ---------------------------------------------------------------------------

struct Img {
  std::vector<uint8_t> d;  // the virtual segment (pointers are 0x50000000 + offset)
  uint32_t Alloc(size_t n, size_t align = 16) {
    while (d.size() % align) d.push_back(0);
    const uint32_t at = static_cast<uint32_t>(d.size());
    d.resize(d.size() + n, 0);
    return at;
  }
  static uint32_t P(uint32_t off) { return 0x50000000u + off; }
  void U32(uint32_t off, uint32_t v) {
    d[off] = uint8_t(v >> 24); d[off + 1] = uint8_t(v >> 16); d[off + 2] = uint8_t(v >> 8); d[off + 3] = uint8_t(v);
  }
  void U16(uint32_t off, uint16_t v) { d[off] = uint8_t(v >> 8); d[off + 1] = uint8_t(v); }
  void F32(uint32_t off, float v) { uint32_t b; std::memcpy(&b, &v, 4); U32(off, b); }
  uint32_t Str(const std::string& s) {
    const uint32_t at = Alloc(s.size() + 1, 4);
    std::memcpy(&d[at], s.data(), s.size());
    return at;
  }
};

struct Param {
  uint32_t hash = 0;
  bool texture = false;
  std::string name;       // a texture's file name
  float v[4] = {0, 0, 0, 0};
};
Param Tex(const char* param, const char* file) { Param p; p.hash = RageHashText(param); p.texture = true; p.name = file; return p; }
Param NullTex(const char* param) { Param p; p.hash = RageHashText(param); p.texture = true; return p; }
Param Vec(const char* param, float x, float y = 0, float z = 0, float w = 0) { Param p; p.hash = RageHashText(param); p.v[0] = x; p.v[1] = y; p.v[2] = z; p.v[3] = w; return p; }

struct SynthGeo {
  std::string group, preset;
  std::vector<float> positions;     // x, y, z per vertex
  std::vector<uint16_t> indices;
  std::vector<float> uv;            // u, v per vertex (exactly representable as float16)
  std::vector<uint32_t> colour;     // D3DCOLOR (ARGB) per vertex
  std::vector<Param> params;
  bool with_attributes = true;      // the retail 0x4069 declaration (position, normal, COLOR1, TEXCOORD0, tangent: stride 28); false: the pack test's bare one (no channel table)
  bool second_uv = false;           // the 0x41F9-like: add COLOR0 and TEXCOORD1 (stride 36 = position, normal, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TANGENT0 -> 0x40F9)
  uint32_t bucket = 1;
};

uint32_t FlagFor(uint32_t size) {
  uint32_t shift = 0;
  while ((1u << (shift + 8)) < size) ++shift;
  return 1u | (shift << 11);
}

std::vector<uint8_t> StoredFile(Img& img, uint32_t type) {
  uint32_t size = 2048;
  while (size < img.d.size()) size <<= 1;
  img.d.resize(size, 0xCD);
  const uint32_t flag = FlagFor(size) | 0x80000000u;
  std::vector<uint8_t> file(12 + img.d.size());
  const auto be = [&](size_t at, uint32_t v) { file[at] = uint8_t(v >> 24); file[at + 1] = uint8_t(v >> 16); file[at + 2] = uint8_t(v >> 8); file[at + 3] = uint8_t(v); };
  be(0, 0x05435352);
  be(4, type);
  be(8, flag);
  std::memcpy(&file[12], img.d.data(), img.d.size());
  return file;
}

// A shader instance with parameters in the layout of hit_params.h; returns its pointer.
uint32_t WriteShader(Img& img, const SynthGeo& g) {
  const uint32_t shader = img.Alloc(0x80);
  const uint32_t n = uint32_t(g.params.size());
  const uint32_t values = img.Alloc(4 * std::max(n, 1u));
  const uint32_t types = img.Alloc(std::max(n, 1u), 4);
  const uint32_t hashes = img.Alloc(4 * std::max(n, 1u));
  for (uint32_t i = 0; i < n; ++i) {
    const Param& p = g.params[i];
    img.U32(hashes + 4 * i, p.hash);
    img.d[types + i] = p.texture ? 0 : 1;
    if (p.texture) {
      if (p.name.empty()) continue;  // a null texture slot
      const uint32_t object = img.Alloc(0x40);
      img.U32(object + 24, Img::P(img.Str(p.name)));
      img.U32(values + 4 * i, Img::P(object));
    } else {
      const uint32_t vec = img.Alloc(16);
      for (uint32_t k = 0; k < 4; ++k) img.F32(vec + 4 * k, p.v[k]);
      img.U32(values + 4 * i, Img::P(vec));
    }
  }
  img.U32(shader + 16, Img::P(values));
  img.U16(shader + 24, uint16_t(n));
  img.U32(shader + 28, Img::P(types));
  img.U32(shader + 36, Img::P(hashes));
  img.U32(shader + 48, Img::P(img.Str(g.group)));
  img.U32(shader + 60, Img::P(img.Str(g.preset)));
  return Img::P(shader);
}

// The vertex layouts: 0x4069 / 28 (the common one) and 0x40F9 / 36 (+ COLOR0 and TEXCOORD1), or the pack test's bare declaration (stride 28, no channel table).
uint32_t WriteModel(Img& img, const SynthGeo& g, uint16_t shader_index) {
  const uint32_t vertex_count = uint32_t(g.positions.size() / 3);
  uint32_t fvf = 0x4069, stride = 28;
  if (g.second_uv) { fvf = 0x40F9; stride = 36; }
  const uint32_t vdata = img.Alloc(size_t(vertex_count) * stride);
  for (uint32_t v = 0; v < vertex_count; ++v) {
    uint32_t at = vdata + v * stride;
    for (int a = 0; a < 3; ++a) img.F32(at + 4 * uint32_t(a), g.positions[size_t(v) * 3 + size_t(a)]);
    img.U32(at + 12, 0x1FF00000u);  // normal
    uint32_t off = 16;
    const uint32_t colour = v < g.colour.size() ? g.colour[v] : 0xFFFFFFFFu;
    const float u = v * 2 + 1 < g.uv.size() ? g.uv[size_t(v) * 2] : 0.0f;
    const float w = v * 2 + 1 < g.uv.size() ? g.uv[size_t(v) * 2 + 1] : 0.0f;
    if (g.second_uv) { img.U32(at + off, 0x80402010u); off += 4; }  // COLOR0: the grime mask
    img.U32(at + off, colour); off += 4;                            // COLOR1
    img.U16(at + off, HalfFromFloat(u)); img.U16(at + off + 2, HalfFromFloat(w)); off += 4;  // TEXCOORD0
    if (g.second_uv) { img.U16(at + off, HalfFromFloat(0.5f)); img.U16(at + off + 2, HalfFromFloat(0.25f)); off += 4; }  // TEXCOORD1
    img.U32(at + off, 0xC0080400u);                                 // tangent
  }
  const uint32_t idata = img.Alloc(g.indices.size() * 2);
  for (size_t i = 0; i < g.indices.size(); ++i) img.U16(idata + uint32_t(i) * 2, g.indices[i]);
  const uint32_t declaration = img.Alloc(0x10);
  img.U32(declaration, g.with_attributes ? fvf : 0x4069u);
  if (g.with_attributes) {
    img.U32(declaration + 4, stride << 24 | (g.second_uv ? 7u : 5u));
    img.U32(declaration + 8, 0xAA111111u);
    img.U32(declaration + 12, 0x1199A996u);
  }
  const uint32_t vb = img.Alloc(0x30);
  img.U16(vb + 4, uint16_t(vertex_count));
  img.U32(vb + 12, stride);
  img.U32(vb + 16, Img::P(declaration));
  img.U32(vb + 24, Img::P(vdata));
  const uint32_t ib = img.Alloc(0x20);
  img.U32(ib + 4, uint32_t(g.indices.size()));
  img.U32(ib + 8, Img::P(idata));
  const uint32_t geometry = img.Alloc(0x40);
  img.U32(geometry + 12, Img::P(vb));
  img.U32(geometry + 28, Img::P(ib));
  img.U32(geometry + 44, uint32_t(g.indices.size()));
  img.U32(geometry + 48, uint32_t(g.indices.size() / 3));
  img.U16(geometry + 52, uint16_t(vertex_count));
  img.U16(geometry + 54, 3);
  const uint32_t array = img.Alloc(4);
  img.U32(array, Img::P(geometry));
  const uint32_t shader_array = img.Alloc(2);
  img.U16(shader_array, shader_index);
  const uint32_t model = img.Alloc(0x20);
  img.U32(model + 4, Img::P(array));
  img.U16(model + 8, 1);
  img.U16(model + 10, 1);
  img.U32(model + 16, Img::P(shader_array));
  return Img::P(model);
}

// A sector resource: one shader instance for each geometry (so every geometry has its own parameters).
std::vector<uint8_t> MakeSectorFile(const std::vector<SynthGeo>& geos) {
  Img img;
  img.Alloc(0x30);
  const uint32_t group = img.Alloc(0x20);
  const uint32_t array = img.Alloc(4 * geos.size());
  img.U32(group + 8, Img::P(array));
  img.U16(group + 12, uint16_t(geos.size()));
  std::vector<uint32_t> models;
  for (size_t i = 0; i < geos.size(); ++i) {
    img.U32(array + 4 * uint32_t(i), WriteShader(img, geos[i]));
    models.push_back(WriteModel(img, geos[i], uint16_t(i)));
  }
  const uint32_t model_array = img.Alloc(4 * models.size());
  const uint32_t hash_array = img.Alloc(4 * models.size());
  const uint32_t city_models = img.Alloc(80 * models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    img.U32(model_array + 4 * uint32_t(i), models[i]);
    const uint32_t hash = 0x1000u + uint32_t(i);
    img.U32(hash_array + 4 * uint32_t(i), hash);
    img.U32(city_models + 80 * uint32_t(i) + 76, hash);
    img.d[city_models + 80 * uint32_t(i) + 74] = uint8_t(geos[i].bucket);
  }
  const uint32_t dictionary = img.Alloc(0x20);
  img.U32(dictionary + 16, Img::P(hash_array));
  img.U16(dictionary + 20, uint16_t(models.size()));
  img.U32(dictionary + 24, Img::P(model_array));
  const uint32_t drawable = img.Alloc(0x20);
  img.U32(drawable + 8, Img::P(group));
  img.U32(drawable + 12, Img::P(dictionary));
  img.U32(16, Img::P(drawable));
  img.U32(32, Img::P(city_models));
  img.U16(36, uint16_t(models.size()));
  return StoredFile(img, 1);
}

struct SynthSector {
  std::string name;
  float box_min[3], box_max[3];
};

std::vector<uint8_t> MakeCityTable(const std::vector<SynthSector>& sectors) {
  Img img;
  img.Alloc(0x30);
  const uint32_t array = img.Alloc(304 * sectors.size());
  for (size_t i = 0; i < sectors.size(); ++i) {
    const uint32_t e = array + 304 * uint32_t(i);
    for (uint32_t a = 0; a < 3; ++a) {
      img.F32(e + 208 + 4 * a, sectors[i].box_min[a]);
      img.F32(e + 224 + 4 * a, sectors[i].box_max[a]);
    }
    img.U32(e + 240, Img::P(img.Str(sectors[i].name)));
    img.U32(e + 244, uint32_t(i));
  }
  img.U32(20, Img::P(array));
  img.U16(24, uint16_t(sectors.size()));
  return StoredFile(img, 1086);
}

// lod.xmd: `models[i]` is the far model of sector i (no positions: a null model); one shader instance for each model.
std::vector<uint8_t> MakeLodFile(const std::vector<SynthGeo>& models) {
  Img img;
  img.Alloc(0x30);
  const uint32_t group = img.Alloc(0x20);
  const uint32_t shaders = img.Alloc(4 * models.size());
  img.U32(group + 8, Img::P(shaders));
  img.U16(group + 12, uint16_t(models.size()));
  const uint32_t array = img.Alloc(4 * models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    img.U32(shaders + 4 * uint32_t(i), WriteShader(img, models[i]));
    if (models[i].positions.empty()) continue;
    img.U32(array + 4 * uint32_t(i), WriteModel(img, models[i], uint16_t(i)));
  }
  img.U32(8, Img::P(group));
  img.U32(12, Img::P(array));
  img.U16(16, uint16_t(models.size()));
  img.U16(18, uint16_t(models.size()));
  return StoredFile(img, 1);
}

// A quad (two triangles 0 1 2 and 0 2 3) at height y with per-vertex uv and colour.
SynthGeo Quad(float x0, float z0, float x1, float z1, float y, const char* group, const char* preset, std::vector<float> uv, std::vector<uint32_t> colour, std::vector<Param> params) {
  SynthGeo g;
  g.group = group;
  g.preset = preset;
  g.positions = {x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1};
  g.indices = {0, 1, 2, 0, 2, 3};
  g.uv = std::move(uv);
  g.colour = std::move(colour);
  g.params = std::move(params);
  return g;
}

fs::path g_scratch;
std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class CountingSink final : public ProgressSink {
 public:
  void BeginPhase(const std::string&, int) override { ++phases; }
  void Step(const std::string&) override { ++steps; }
  void Log(LogLevel, const std::string& t) override { lines.push_back(t); }
  bool Cancelled() const override { return cancel_after >= 0 && steps >= cancel_after; }
  int phases = 0;
  int steps = 0;
  int cancel_after = -1;
  std::vector<std::string> lines;
};

// ---------------------------------------------------------------------------
// the parameters, the layouts, the quantisation
// ---------------------------------------------------------------------------

void TestParams() {
  // the names hash to the numbers the retail game's instances hold
  CHECK(RageHashConstexpr("DiffuseSampler") == 0xF1FE2B71u && RageHashText("DiffuseSampler") == 0xF1FE2B71u);
  CHECK(RageHashConstexpr("SpecularExponent") == 0x93D520E8u && RageHashConstexpr("NeonIntensity") == kParamNeonIntensity);
  CHECK(TextureStem("Wall_Brick_01.DDS") == "wall_brick_01" && TextureStem("no_extension") == "no_extension" && TextureStem("a.dds.png") == "a.dds.png");

  SynthGeo g = Quad(0, 0, 1, 1, 0, "CityBumpSpec", "CityBumpSpec_Cloth", {}, {},
                    {Tex("DiffuseSampler", "wall.dds"), NullTex("ReflectionSampler"), Vec("SpecularTint", 1, 1, 1, std::nanf("")), Vec("SpecularExponent", 15.0f)});
  Img img;
  img.Alloc(0x30);
  const uint32_t shader = WriteShader(img, g);
  img.d.resize(4096, 0);
  RscImage image;
  image.data = img.d;
  image.virtual_size = uint32_t(img.d.size());
  RscCursor cursor(image);
  ShaderParams params;
  CHECK(ReadShaderParams(cursor, shader, &params));
  CHECK(params.params.size() == 4);
  const ShaderParam* diffuse = params.Find(kParamDiffuseSampler);
  CHECK(diffuse && diffuse->type == kParamTypeTexture && !diffuse->empty && diffuse->texture == "wall.dds");
  const ShaderParam* reflection = params.Find(RageHashText("ReflectionSampler"));
  CHECK(reflection && reflection->empty && reflection->texture.empty());  // a null texture slot
  const ShaderParam* exponent = params.Find(kParamSpecularExponent);
  CHECK(exponent && exponent->type == kParamTypeFloat4 && exponent->value[0] == 15.0f);
  CHECK(params.Find(0x12345678u) == nullptr);
  // not a shader instance: no values pointer
  ShaderParams none;
  CHECK(!ReadShaderParams(cursor, Img::P(0x20), &none) || none.params.empty());
  CHECK(!ReadShaderParams(cursor, 0, &none));
}

void TestLayouts() {
  const uint64_t sizes = 0xAA1111111199A996ull;
  // the retail game's declarations: mask and stride
  VertexLayout a = LayoutOfDeclaration(0x4069, sizes, 28);  // position, normal, COLOR1, TEXCOORD0, tangent
  CHECK(a.valid && a.colour1 == 16 && a.uv0 == 20 && a.colour0 < 0 && a.uv1 < 0);
  VertexLayout b = LayoutOfDeclaration(0x69, sizes, 24);  // position, normal, COLOR1, TEXCOORD0
  CHECK(b.valid && b.colour1 == 16 && b.uv0 == 20);
  VertexLayout c = LayoutOfDeclaration(0x61, sizes, 20);  // position, COLOR1, TEXCOORD0
  CHECK(c.valid && c.colour1 == 12 && c.uv0 == 16);
  VertexLayout d = LayoutOfDeclaration(0x49, sizes, 20);  // position, normal, TEXCOORD0: no colour
  CHECK(d.valid && d.colour1 < 0 && d.uv0 == 16);
  VertexLayout e = LayoutOfDeclaration(0x41F9, sizes, 40);  // + COLOR0, TEXCOORD1, tangent
  CHECK(e.valid && e.colour0 == 16 && e.colour1 == 20 && e.uv0 == 24 && e.uv1 == 28);
  VertexLayout f = LayoutOfDeclaration(0x41, sizes, 16);  // position, TEXCOORD0 (the ocean's far model)
  CHECK(f.valid && f.uv0 == 12 && f.colour1 < 0);
  VertexLayout g = LayoutOfDeclaration(0x1F9, sizes, 36);
  CHECK(g.valid && g.colour0 == 16 && g.colour1 == 20 && g.uv0 == 24 && g.uv1 == 28);
  // wrong stride, no position, an unknown channel format, no table at all
  CHECK(!LayoutOfDeclaration(0x4069, sizes, 32).valid);
  CHECK(!LayoutOfDeclaration(0x48, sizes, 20).valid);
  CHECK(!LayoutOfDeclaration(0x4069, 0xAA1111111199A99Bull, 28).valid);  // the position's format is not float3
  CHECK(!LayoutOfDeclaration(0x4069, 0xAA1111111199F996ull, 28).valid);  // the normal's format code is unknown
  CHECK(!LayoutOfDeclaration(0x4069, 0, 28).valid);
  CHECK(!LayoutOfDeclaration(0x14069, sizes, 28).valid);
  // a texture coordinate in another format is not read as one
  VertexLayout h = LayoutOfDeclaration(0x69, 0xAA1111111099A996ull, 24);
  CHECK(!h.valid || h.uv0 < 0);
  // D3DCOLOR (A in the top byte) as the corner's RGBA8
  CHECK(CornerColourOfD3dColor(0x80112233u) == PackCornerColour(0x11, 0x22, 0x33, 0x80));
}

void TestQuantisation() {
  HitRules exact;
  exact.ao_bits = 8;
  exact.tint_bits = 8;
  CHECK(exact.QuantiseColour(0x80112233u) == 0x80112233u);
  const HitRules rules;
  CHECK(rules.QuantiseColour(0xFFFFFFFFu) == 0xFFFFFFFFu && rules.QuantiseColour(0u) == 0u);  // the ends stay
  uint32_t last = 0;
  for (uint32_t a = 0; a <= 255; ++a) {  // alpha: monotonic, within one step of 1/128
    const uint32_t q = rules.QuantiseColour(a << 24) >> 24;
    CHECK(q >= last && (q > a ? q - a : a - q) <= 2);
    last = q;
  }
  CHECK(rules.QuantiseColour(0x0000000Au) != rules.QuantiseColour(0x00000014u) || (rules.tint_bits == 8));  // small tints stay apart on the square root scale
  // the tint: monotonic on its square root scale, and the three channels are quantised alike
  uint32_t last_tint = 0;
  for (uint32_t v = 0; v <= 255; ++v) {
    const uint32_t q = rules.QuantiseColour(v) & 0xFFu;
    CHECK(q >= last_tint && q <= 255);
    CHECK(rules.QuantiseColour(v * 0x010101u) == (q * 0x010101u));
    last_tint = q;
  }
  CHECK(HitRules().Pack() != exact.Pack());
}

// ---------------------------------------------------------------------------
// the indexed corner stream
// ---------------------------------------------------------------------------

std::vector<uint32_t> RandomCorners(size_t n, uint32_t seed, uint32_t pairs) {
  std::mt19937 rng(seed);
  std::vector<uint64_t> pool(pairs);
  for (uint64_t& p : pool) p = (uint64_t(rng() & 0xFFFFu) | (uint64_t(rng() & 0xFFFFu) << 16)) | (uint64_t(rng() % 40u) * 0x01010101ull << 32);
  std::vector<uint32_t> out;
  size_t at = 0;
  for (size_t i = 0; i < n; ++i) {
    // mostly a recent pair, sometimes a far one, sometimes a new one: every token class is used
    uint64_t pick;
    const uint32_t kind = rng() % 10;
    if (kind < 6 && at > 0) pick = pool[(at - 1 - rng() % std::min<size_t>(at, 6)) % pairs];
    else if (kind < 8 && at > 0) pick = pool[rng() % std::min<size_t>(at, pairs)];
    else pick = pool[at++ % pairs];
    out.push_back(uint32_t(pick));
    out.push_back(uint32_t(pick >> 32));
  }
  return out;
}

void TestIndexedCorners() {
  std::string error;
  std::vector<uint32_t> back;
  // nothing, one corner, a few
  CHECK(DecodeIndexedCorners(EncodeIndexedCorners(nullptr, 0).data(), EncodeIndexedCorners(nullptr, 0).size(), 0, &back, &error) && back.empty());
  const uint32_t one[2] = {0x00050007u, 0xFF102030u};
  {
    const std::vector<uint8_t> s = EncodeIndexedCorners(one, 1);
    CHECK(DecodeIndexedCorners(s.data(), s.size(), 1, &back, &error) && back.size() == 2 && back[0] == one[0] && back[1] == one[1]);
  }
  // random streams with every token: near distances, 13..268, 269 and more, new entries; several sizes (an odd count too)
  for (const size_t n : {2u, 3u, 7u, 100u, 1001u, 30000u}) {
    for (const uint32_t pairs : {1u, 5u, 400u, 100000u}) {
      const std::vector<uint32_t> words = RandomCorners(n, uint32_t(n * 31 + pairs), pairs);
      const std::vector<uint8_t> s = EncodeIndexedCorners(words.data(), n);
      CHECK(DecodeIndexedCorners(s.data(), s.size(), n, &back, &error));
      CHECK(back == words);
    }
  }
  // far references: the same pair again after 13, 268, 269, 300 and 70000 other corners
  for (const uint32_t gap : {12u, 13u, 14u, 267u, 268u, 269u, 270u, 300u, 70000u}) {
    std::vector<uint32_t> words;
    words.push_back(0x11112222u);
    words.push_back(0xABCDEF01u);
    for (uint32_t i = 0; i < gap; ++i) {
      words.push_back(0x00010000u + i);
      words.push_back(0x01020304u);
    }
    words.push_back(0x11112222u);
    words.push_back(0xABCDEF01u);
    const size_t n = words.size() / 2;
    const std::vector<uint8_t> s = EncodeIndexedCorners(words.data(), n);
    CHECK(DecodeIndexedCorners(s.data(), s.size(), n, &back, &error) && back == words);
  }
  // it is much smaller than the raw words where pairs repeat
  {
    const std::vector<uint32_t> words = RandomCorners(30000, 5, 400);
    CHECK(EncodeIndexedCorners(words.data(), 30000).size() < 30000 * 8 / 4);
  }
  // damage: cut, a flipped byte in each section, wrong counts, a token that is not one
  {
    const std::vector<uint32_t> words = RandomCorners(500, 9, 60);
    const std::vector<uint8_t> s = EncodeIndexedCorners(words.data(), 500);
    CHECK(!DecodeIndexedCorners(s.data(), s.size() - 1, 500, &back, &error) && !error.empty());
    CHECK(!DecodeIndexedCorners(s.data(), 10, 500, &back, &error));
    CHECK(!DecodeIndexedCorners(s.data(), s.size(), 498, &back, &error));  // the nibble section is not that many corners'
    std::vector<uint8_t> bad = s;
    bad[0] = 9;  // another version
    CHECK(!DecodeIndexedCorners(bad.data(), bad.size(), 500, &back, &error));
    bad = s;
    const uint32_t palette = uint32_t(s[8]) | (uint32_t(s[9]) << 8);
    bad[kIndexedCornerHeaderBytes + palette * 4] = 0xFF;  // the first token pair: 15 (invalid) in the low nibble
    CHECK(!DecodeIndexedCorners(bad.data(), bad.size(), 500, &back, &error));
    // whatever a flipped byte does, a decode never reads outside its stream: it returns true or false, it does not crash
    {
      const std::vector<uint32_t> small = RandomCorners(40, 11, 9);
      const std::vector<uint8_t> t = EncodeIndexedCorners(small.data(), 40);
      for (size_t at = 0; at < t.size(); ++at) {
        std::vector<uint8_t> f = t;
        f[at] ^= 0xFF;
        DecodeIndexedCorners(f.data(), f.size(), 40, &back, &error);
      }
    }
    // a first corner that refers back
    std::vector<uint8_t> first = s;
    first[kIndexedCornerHeaderBytes + palette * 4] = uint8_t((first[kIndexedCornerHeaderBytes + palette * 4] & 0xF0) | 1);
    CHECK(!DecodeIndexedCorners(first.data(), first.size(), 500, &back, &error));
  }
  // a blob: the runs and the indexed stream in the container of city_hit_format.h; the old reader sees the runs and no corners, DecodeHitBlobAny sees both
  {
    HitBlob blob;
    blob.triangle_count = 4;
    blob.opaque_triangles = 3;
    blob.flags = kHitBlobHasCorners;
    HitRun r0;
    r0.first_triangle = 0;
    r0.triangle_count = 3;
    r0.material = 7;
    r0.flags = kRunHasUv | kRunHasColour;
    HitRun r1;
    r1.first_triangle = 3;
    r1.triangle_count = 1;
    r1.material = 2;
    r1.flags = kRunAlphaTested | kRunHasUv;
    blob.runs = {r0, r1};
    blob.corners = RandomCorners(12, 3, 5);
    const std::vector<uint8_t> stream = EncodeIndexedCorners(blob.corners.data(), 12);
    const std::vector<uint8_t> bytes = AssembleIndexedHitBlob(blob, stream);
    HitBlob any;
    CHECK(DecodeHitBlobAny(bytes.data(), bytes.size(), &any, &error));
    CHECK(any.has_corners() && any.corners == blob.corners && any.runs.size() == 2 && any.runs[0].material == 7 && any.runs[1].flags == (kRunAlphaTested | kRunHasUv));
    CHECK(any.triangle_count == 4 && any.opaque_triangles == 3 && (any.flags & kHitBlobHasIndexedCorners) == 0);
    HitBlob old;
    CHECK(DecodeHitBlob(bytes.data(), bytes.size(), &old, &error));  // city_hit_format.h's own reader: no error, no corners
    CHECK(!old.has_corners() && old.corners.empty() && old.runs.size() == 2 && (old.flags & kHitBlobHasIndexedCorners) != 0);
    // a plain blob (H0's encodings) is read as it is
    const std::vector<uint8_t> plain = EncodeHitBlob(blob, CornerEncoding::kDelta);
    HitBlob plain_back;
    CHECK(DecodeHitBlobAny(plain.data(), plain.size(), &plain_back, &error) && plain_back.corners == blob.corners);
    // damage: a flipped byte anywhere in the corner stream is caught by the blob's hash
    std::vector<uint8_t> flipped = bytes;
    flipped[flipped.size() - 9] ^= 0x10;
    CHECK(!DecodeHitBlobAny(flipped.data(), flipped.size(), &any, &error));
    CHECK(!DecodeHitBlobAny(bytes.data(), bytes.size() - 4, &any, &error));
  }
}

// ---------------------------------------------------------------------------
// the materials
// ---------------------------------------------------------------------------

ShaderNames MakeNames(const std::string& group, const std::string& preset, std::vector<Param> params) {
  SynthGeo g;
  g.group = group;
  g.preset = preset;
  g.params = std::move(params);
  Img img;
  img.Alloc(0x30);
  const uint32_t shader = WriteShader(img, g);
  img.d.resize(4096, 0);
  RscImage image;
  image.data = img.d;
  image.virtual_size = uint32_t(img.d.size());
  RscCursor cursor(image);
  ShaderNames names;
  names.group = group;
  names.preset = preset;
  auto params_read = std::make_shared<ShaderParams>();
  CHECK(ReadShaderParams(cursor, shader, params_read.get()));
  names.params = params_read;
  return names;
}

// A texture set that resolves the names it is given (and not the others), with keys and averages the test knows.
class FakeTextures final : public TextureSet {
 public:
  std::string Identity() const override { return "fake1"; }
  bool Resolve(const std::vector<TextureName>& names, std::vector<ResolvedTexture>* answers, std::vector<HitTexture>* table, std::string*) override {
    asked = names;
    answers->assign(names.size(), ResolvedTexture());
    table->clear();
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i].stem.rfind("known", 0) != 0) continue;
      HitTexture t;
      t.key = 0xABC0000000000000ull + i;
      t.width = 256;
      t.height = 128;
      t.format = HitTextureFormat::kBc1;
      t.mip_count = 1;
      t.data_size = 16384;
      (*answers)[i].index = uint16_t(table->size());
      (*answers)[i].key = t.key;
      (*answers)[i].average[0] = 0.5f;
      (*answers)[i].average[1] = 0.25f;
      (*answers)[i].average[2] = 1.0f;
      table->push_back(t);
    }
    return true;
  }
  bool Finish(const fs::path& file, uint64_t hit_table_hash, std::string*) override {
    finished_hash = hit_table_hash;
    TexHeader h;
    h.texture_count = 1;
    h.hit_table_hash = hit_table_hash;
    h.file_size = kTexHeaderBytes;
    ByteWriter w;
    h.Write(w);
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(w.bytes().data()), std::streamsize(w.size()));
    return true;
  }
  std::vector<TextureName> asked;
  uint64_t finished_hash = 0;
};

void TestMaterials() {
  // a CityBumpSpec: world class, the pixel shader's exponent, SSR off (walls), the lit weight 1
  {
    const ShaderNames names = MakeNames("CityBumpSpec", "CityBumpSpec_Cloth", {Tex("DiffuseSampler", "known_wall.dds"), Tex("HeightSpecularSampler", "known_wall_bs.dds"), Vec("SpecularTint", 1, 1, 1, std::nanf("")),
                                                                          Vec("SpecularExponent", 15.0f), Vec("SpecularIntensity", 1.0f)});
    MaterialFacts facts;
    const MaterialDef def = MaterialFromShader(&names, MaterialClass::kSolid, &facts);
    CHECK(facts.group_known && facts.gloss_pixel && !facts.gloss_vertex && facts.has_exponent && !facts.no_params);
    CHECK(WordClass(def.word) == uint32_t(gb::SurfaceClass::kWorld) && WordFlags(def.word) == 0 && WordLit(def.word) == 255 && WordMetal(def.word) == 0);
    CHECK(WordSmoothness(def.word) == uint32_t(gb::ToByte(gb::GlossSmoothness(gb::GlossBank::kPixel, 15.0f, 0.15f))));
    CHECK(def.texture == "known_wall.dds" && def.tint[0] == 1.0f && def.emissive == 0.0f && !def.flat_colour && def.shader_hash == ShaderHash("CityBumpSpec", "CityBumpSpec_Cloth"));
  }
  // the day city shader is lit per vertex: the vertex bank (the power itself, not twice it)
  {
    const ShaderNames names = MakeNames("CityDefault", "CityDefault", {Tex("DiffuseSampler", "d.dds"), Vec("SpecularExponent", 8.0f)});
    MaterialFacts facts;
    const MaterialDef def = MaterialFromShader(&names, MaterialClass::kSolid, &facts);
    CHECK(facts.gloss_vertex && !facts.gloss_pixel);
    CHECK(WordSmoothness(def.word) == uint32_t(gb::ToByte(gb::GlossSmoothness(gb::GlossBank::kVertex, 8.0f, 0.15f))));
    CHECK(WordSmoothness(def.word) != uint32_t(gb::ToByte(gb::GlossSmoothness(gb::GlossBank::kPixel, 8.0f, 0.15f))));
  }
  // no exponent: the class default; the road is SSR eligible; a tinted material has its flat colour
  {
    const ShaderNames road = MakeNames("CityRoad", "CityRoad_Asphalt", {Tex("DiffuseSampler", "r.dds")});
    MaterialFacts facts;
    const MaterialDef def = MaterialFromShader(&road, MaterialClass::kGround, &facts);
    CHECK(!facts.has_exponent && WordClass(def.word) == uint32_t(gb::SurfaceClass::kRoad) && WordFlags(def.word) == uint32_t(gb::kMaterialFlagSsr));
    CHECK(WordSmoothness(def.word) == uint32_t(gb::ToByte(gb::DefaultSmoothness(gb::SurfaceClass::kRoad))));
    const ShaderNames tint = MakeNames("CityGrimeMetalTint", "CityGrimeMetalTint", {Vec("SpecularExponent", 12.0f), Vec("DiffuseColor", 0.329f, 0.5f, 5.0f, 1.0f)});
    const MaterialDef t = MaterialFromShader(&tint, MaterialClass::kSolid, nullptr);
    CHECK(t.flat_colour && t.texture.empty() && Near(t.tint[0], 0.329f, 1e-6f) && Near(t.tint[2], kHitRgb10Max, 1e-6f));  // the tint is clamped into the record's range
  }
  // a glowing material: class emissive, lit weight 0, the group's intensity; a window has its glass colour; the terrain's texture is its A layer
  {
    const ShaderNames neon = MakeNames("CityNeon", "CityNeonAlpha_Metal", {Tex("DiffuseSampler", "n.dds"), Tex("NeonSampler", "n_lit.dds"), Vec("NeonIntensity", 3.5f), Vec("SpecularExponent", 175.0f)});
    const MaterialDef n = MaterialFromShader(&neon, MaterialClass::kAlphaTested, nullptr);
    CHECK(WordClass(n.word) == uint32_t(gb::SurfaceClass::kEmissive) && WordLit(n.word) == 0 && n.emissive == 3.5f && (WordFlags(n.word) & kWordFlagAlphaCovered) != 0);
    const ShaderNames sign = MakeNames("CityNeonTint", "CityNeonTint", {Vec("NightColor", 1.0f, 0.5f, 0.25f), Vec("DayColor", 0.2f, 0.2f, 0.2f), Vec("NightIntensity", 2.0f)});
    const MaterialDef s = MaterialFromShader(&sign, MaterialClass::kEmissive, nullptr);
    CHECK(s.emissive == 2.0f && s.flat_colour && s.tint[1] == 0.5f);  // glows with its night colour
    const ShaderNames lamp = MakeNames("CityLampGlassTint", "x", {});
    CHECK(MaterialFromShader(&lamp, MaterialClass::kEmissive, nullptr).emissive == 1.0f);  // no intensity parameter: unit glow
    const ShaderNames window = MakeNames("CityWindowUpper", "w", {Tex("DiffuseSampler", "w.dds"), Vec("WindowTintColor", 0.5f, 0.6f, 0.7f), Vec("SpecularExponent", 14.0f)});
    const MaterialDef w = MaterialFromShader(&window, MaterialClass::kGlass, nullptr);
    CHECK(WordClass(w.word) == uint32_t(gb::SurfaceClass::kGlass) && (WordFlags(w.word) & kWordFlagSsr) != 0 && Near(w.tint[2], 0.7f, 1e-6f) && !w.flat_colour);
    const ShaderNames terrain = MakeNames("CityTerrain", "CityTerrain", {Tex("DiffuseSamplerA", "ta.dds"), Tex("DiffuseSamplerB", "tb.dds")});
    CHECK(MaterialFromShader(&terrain, MaterialClass::kGround, nullptr).texture == "ta.dds");
    const ShaderNames interior = MakeNames("CityBumpSpec", "CityBumpSpec_int", {});
    CHECK(WordClass(MaterialFromShader(&interior, MaterialClass::kInterior, nullptr).word) == uint32_t(gb::SurfaceClass::kInterior));
  }
  // a group the table does not know, no names, no parameters: the default surface
  {
    const ShaderNames odd = MakeNames("SomeModShader", "x", {Tex("DiffuseSampler", "m.dds")});
    MaterialFacts facts;
    const MaterialDef def = MaterialFromShader(&odd, MaterialClass::kSolid, &facts);
    CHECK(!facts.group_known && def.word == MaterialDef().word && def.texture.empty());
    CHECK(MaterialFromShader(nullptr, MaterialClass::kSolid, nullptr).word == MaterialDef().word);
    ShaderNames bare;
    bare.group = "CityBumpSpec";
    bare.preset = "p";
    MaterialFacts f2;
    const MaterialDef b = MaterialFromShader(&bare, MaterialClass::kSolid, &f2);
    CHECK(f2.no_params && f2.group_known && b.texture.empty() && WordClass(b.word) == uint32_t(gb::SurfaceClass::kWorld));
  }
  // the global table: equal materials are one; material 0 is the default
  MaterialSet set;
  CHECK(set.size() == 1 && set.defs()[0].word == MaterialDef().word);
  const ShaderNames wall_a = MakeNames("CityBumpSpec", "p", {Tex("DiffuseSampler", "known_a.dds"), Vec("SpecularExponent", 15.0f)});
  const ShaderNames wall_a2 = MakeNames("CityBumpSpec", "p", {Tex("DiffuseSampler", "known_a.dds"), Vec("SpecularExponent", 15.0f)});
  const ShaderNames wall_b = MakeNames("CityBumpSpec", "p", {Tex("DiffuseSampler", "known_b.dds"), Vec("SpecularExponent", 15.0f)});
  const ShaderNames wall_c = MakeNames("CityBumpSpec", "p", {Tex("DiffuseSampler", "missing_c.dds"), Vec("SpecularExponent", 40.0f), Vec("DiffuseColor", 0.5f, 1.0f, 1.0f)});
  const uint32_t ia = set.Add(MaterialFromShader(&wall_a, MaterialClass::kSolid, nullptr));
  CHECK(ia == 1 && set.Add(MaterialFromShader(&wall_a2, MaterialClass::kSolid, nullptr)) == 1);
  const uint32_t ib = set.Add(MaterialFromShader(&wall_b, MaterialClass::kSolid, nullptr));
  const uint32_t ic = set.Add(MaterialFromShader(&wall_c, MaterialClass::kSolid, nullptr));
  CHECK(ib == 2 && ic == 3 && set.Add(MaterialDef()) == 0 && set.size() == 4);

  // finalising: the set's answers, the average, the rows
  FakeTextures textures;
  std::vector<HitMaterial> materials;
  std::vector<HitTexture> table;
  FinalizeStats stats;
  std::string error;
  CHECK(FinalizeMaterials(set, &textures, nullptr, &materials, &table, &stats, &error));
  CHECK(materials.size() == 4 && table.size() == 2 && stats.texture_names == 3 && stats.names_resolved == 2 && stats.materials_textured == 3 && stats.materials_unresolved == 1);
  CHECK(textures.asked.size() == 3 && textures.asked[0].stem == "known_a" && textures.asked[0].stem_hash == RageHashText("known_a") && textures.asked[2].file == "missing_c.dds");
  CHECK(materials[1].texture == 0 && materials[2].texture == 1 && materials[3].texture == kNoTexture && materials[0].texture == kNoTexture);
  CHECK(materials[1].key == table[0].key && materials[3].key == 0);
  CHECK(Near(materials[1].average[0], 0.5f, 1e-6f) && Near(materials[1].average[1], 0.25f, 1e-6f) && Near(materials[1].average[2], 1.0f, 1e-6f));  // tint (1,1,1) x the texture's mean
  CHECK(Near(materials[3].average[0], 0.25f, 1e-6f) && Near(materials[3].average[1], 0.5f, 1e-6f));  // an unresolved texture: the tint x a mid grey
  CHECK(materials[0].average[0] == 0.5f && materials[0].tint[0] == 1.0f);  // the default surface
  // the records pack and unpack (the file's bytes)
  uint32_t words[8];
  materials[3].Pack(words);
  const HitMaterial round = HitMaterial::Unpack(words);
  CHECK(round.word == materials[3].word && round.shader_hash == materials[3].shader_hash && Near(round.tint[1], 1.0f, 0.003f));

  // GB-F2 rows by the texture's key: class=glass for known_b only
  char text[160];
  std::snprintf(text, sizeof(text), "@version 1\nhash:%llx class=glass metal=1\n", static_cast<unsigned long long>(table[1].key));
  const fs::path rows_file = g_scratch / "rows.txt";
  fs::create_directories(g_scratch);
  { std::ofstream(rows_file) << text; }
  gb::TextureTable rows;
  uint64_t rows_hash = 0;
  size_t bad = 0;
  CHECK(LoadTextureRows({rows_file, g_scratch / "absent.txt"}, &rows, &rows_hash, &bad) && rows.size() == 1 && rows_hash != 0 && bad == 0);
  FinalizeStats with_rows;
  CHECK(FinalizeMaterials(set, &textures, &rows, &materials, &table, &with_rows, &error));
  CHECK(with_rows.rows_applied == 1 && WordClass(materials[2].word) == uint32_t(gb::SurfaceClass::kGlass) && WordMetal(materials[2].word) == 255 && WordClass(materials[1].word) == 1);
  CHECK(FinalizeMaterials(set, nullptr, nullptr, &materials, &table, &stats, &error) && table.empty() && stats.names_resolved == 0 && materials[1].texture == kNoTexture);  // no set: none
}

// ---------------------------------------------------------------------------
// the collector (corners, runs)
// ---------------------------------------------------------------------------

// A source geometry the way sector_parse.h hands it over, from a SynthGeo (big endian vertex data).
struct SourceHolder {
  std::vector<uint8_t> vertices, indices;
  SourceGeometry g;
};
SourceHolder MakeSource(const SynthGeo& s) {
  SourceHolder h;
  const uint32_t stride = s.second_uv ? 36 : 28;
  const uint32_t vertex_count = uint32_t(s.positions.size() / 3);
  h.vertices.assign(size_t(vertex_count) * stride, 0);
  const auto put32 = [&](size_t at, uint32_t v) { h.vertices[at] = uint8_t(v >> 24); h.vertices[at + 1] = uint8_t(v >> 16); h.vertices[at + 2] = uint8_t(v >> 8); h.vertices[at + 3] = uint8_t(v); };
  for (uint32_t v = 0; v < vertex_count; ++v) {
    const size_t at = size_t(v) * stride;
    for (int a = 0; a < 3; ++a) { uint32_t b; std::memcpy(&b, &s.positions[size_t(v) * 3 + size_t(a)], 4); put32(at + 4 * size_t(a), b); }
    size_t off = 16;
    if (s.second_uv) off += 4;
    put32(at + off, v < s.colour.size() ? s.colour[v] : 0xFFFFFFFFu);
    off += 4;
    const uint32_t uvword = (uint32_t(HalfFromFloat(s.uv[size_t(v) * 2])) << 16) | HalfFromFloat(s.uv[size_t(v) * 2 + 1]);
    put32(at + off, uvword);
  }
  h.indices.resize(s.indices.size() * 2);
  for (size_t i = 0; i < s.indices.size(); ++i) { h.indices[2 * i] = uint8_t(s.indices[i] >> 8); h.indices[2 * i + 1] = uint8_t(s.indices[i]); }
  h.g.vertex_count = vertex_count;
  h.g.stride = stride;
  h.g.index_count = uint32_t(s.indices.size());
  h.g.fvf = s.second_uv ? 0x40F9u : 0x4069u;
  h.g.fvf_sizes = 0xAA1111111199A996ull;
  h.g.vertices = h.vertices.data();
  h.g.indices = h.indices.data();
  return h;
}

void TestCollector() {
  const HitRules rules;
  // two geometries: a quad with distinct per-vertex attributes, then a quad whose texture coordinates jump far apart (u 0 and u 1000): the run is cut
  SynthGeo a = Quad(0, 0, 1, 1, 0, "g", "p", {0.0f, 0.0f, 2.0f, 0.0f, 2.0f, 3.0f, 0.0f, 3.0f}, {0xFF102030u, 0xC0102030u, 0x80405060u, 0x40708090u}, {});
  SynthGeo b = Quad(2, 0, 3, 1, 0, "g", "p", {0.0f, 0.0f, 0.5f, 0.0f, 1000.0f, 1.0f, 1000.0f, 0.5f}, {}, {});
  SynthGeo c = a;  // no colour at all (white), a second uv set present
  c.second_uv = true;
  c.colour.clear();
  const SourceHolder ha = MakeSource(a), hb = MakeSource(b), hc = MakeSource(c);
  HitCollector collector(rules);
  const std::vector<uint32_t> kept_all = {0, 1};
  collector.AddGeometry(ha.g, false, kept_all, 5);
  collector.AddGeometry(hb.g, false, kept_all, 6);
  collector.AddGeometry(hc.g, true, {1}, 7);   // an alpha tested geometry that kept its second triangle only
  collector.AddGeometry(ha.g, false, {}, 9);   // nothing kept: no run
  HitBlob blob;
  std::string error;
  CHECK(!collector.Finish(3, 5, &blob, &error) && !error.empty());  // the pack's mesh has other counts: refused
  CHECK(collector.Finish(4, 5, &blob, &error));
  CHECK(blob.triangle_count == 5 && blob.opaque_triangles == 4 && blob.has_corners() && blob.corners.size() == 5 * 3 * 2);
  CHECK(CheckRuns(blob.runs, 5, 4).empty());
  // runs: geometry a (1 run), geometry b cut in two (its second triangle is 1000 uv units away), then the alpha one
  CHECK(blob.runs.size() == 4);
  CHECK(blob.runs[0].material == 5 && blob.runs[0].triangle_count == 2 && blob.runs[0].flags == (kRunHasUv | kRunHasColour));
  CHECK(blob.runs[1].material == 6 && blob.runs[1].triangle_count == 1 && blob.runs[2].material == 6 && blob.runs[2].triangle_count == 1 && blob.runs[1].flags == (kRunHasUv | kRunHasColour) );
  CHECK(blob.runs[3].material == 7 && blob.runs[3].triangle_count == 1 && blob.runs[3].first_triangle == 4 && (blob.runs[3].flags & kRunAlphaTested) != 0);
  CHECK(collector.stats().runs == 4 && collector.stats().runs_split == 1 && collector.stats().geometries == 3 && collector.stats().geometries_second_uv == 1 && collector.stats().geometries_colour0 == 1);
  // every corner decodes to its source's texture coordinate (to a step of its run's transform) and colour (after the loss)
  const auto expect_corner = [&](const SynthGeo& s, uint32_t triangle_in_geo, uint32_t k, size_t corner_index, const HitRun& run, bool white) {
    const uint32_t vertex = s.indices[triangle_in_geo * 3 + k];
    float uv[2];
    DecodeCornerUv(run, blob.corners[corner_index * 2], uv);
    const float step_u = run.uv_scale[0] / 65535.0f, step_v = run.uv_scale[1] / 65535.0f;
    CHECK(Near(uv[0], s.uv[vertex * 2], step_u) && Near(uv[1], s.uv[vertex * 2 + 1], step_v));
    const uint32_t colour = white ? kWhiteCorner : rules.QuantiseColour(CornerColourOfD3dColor(s.colour[vertex]));
    CHECK(blob.corners[corner_index * 2 + 1] == colour);
  };
  for (uint32_t t = 0; t < 2; ++t) for (uint32_t k = 0; k < 3; ++k) expect_corner(a, t, k, size_t(t * 3 + k), blob.runs[0], false);
  for (uint32_t k = 0; k < 3; ++k) expect_corner(b, 0, k, size_t(6 + k), blob.runs[1], true);
  for (uint32_t k = 0; k < 3; ++k) expect_corner(b, 1, k, size_t(9 + k), blob.runs[2], true);
  for (uint32_t k = 0; k < 3; ++k) expect_corner(c, 1, k, size_t(12 + k), blob.runs[3], true);
  // the step: a run whose range is narrower than 2^-12 x 65535 = 16 repeats has a transform at least that wide (small quantised numbers), a wide one keeps its own range
  CHECK(blob.runs[0].uv_scale[0] >= 16.0f && blob.runs[0].uv_scale[1] >= 16.0f);
  CHECK(blob.runs[2].uv_scale[0] >= 1000.0f);
  // no attributes anywhere: a blob without corners
  {
    SynthGeo bare = a;
    bare.with_attributes = false;
    HitCollector plain(rules);
    SourceHolder hp = MakeSource(bare);
    hp.g.fvf = 0x4069;
    hp.g.fvf_sizes = 0;  // no channel table: the layout is not read
    plain.AddGeometry(hp.g, false, kept_all, 1);
    HitBlob none;
    CHECK(plain.Finish(2, 2, &none, &error) && !none.has_corners() && none.corners.empty() && none.runs.size() == 1 && none.runs[0].flags == 0 && plain.stats().geometries_layout_unknown == 1);
  }
  // exact rules (step 0, 8 bit colour) keep the colour word and the unorm16 over the run's own range
  {
    HitRules exact;
    exact.uv_step_log2 = 0;
    exact.ao_bits = 8;
    exact.tint_bits = 8;
    HitCollector lossless(exact);
    lossless.AddGeometry(ha.g, false, kept_all, 1);
    HitBlob lb;
    CHECK(lossless.Finish(2, 2, &lb, &error));
    CHECK(lb.corners[1] == CornerColourOfD3dColor(a.colour[0]) && lb.runs[0].uv_scale[0] == 2.0f && lb.runs[0].uv_scale[1] == 3.0f);
  }
}

// ---------------------------------------------------------------------------
// the whole builder on a synthetic archive
// ---------------------------------------------------------------------------

void TestBuilder() {
  fs::create_directories(g_scratch);
  const fs::path rpf = g_scratch / "xarchive_cache.rpf";
  const std::vector<SynthSector> table = {{"s_a", {0, -5, 0}, {100, 50, 100}}, {"s_b", {100, -5, 0}, {200, 50, 100}}, {"s_missing", {200, -5, 0}, {300, 50, 100}}};
  const auto add = [](mc::modloader::Rpf3Writer& w, const std::string& path, std::vector<uint8_t> file) {
    const uint32_t flag = (uint32_t(file[8]) << 24) | (uint32_t(file[9]) << 16) | (uint32_t(file[10]) << 8) | uint32_t(file[11]);
    w.Add(path, std::move(file), flag, 1);
  };
  const auto asphalt = [&] {
    return Quad(0, 0, 10, 10, 0, "CityRoad", "CityRoad_Asphalt", {0.0f, 0.0f, 2.0f, 0.0f, 2.0f, 3.0f, 0.0f, 3.0f}, {0xFF102030u, 0xC0102030u, 0x80405060u, 0x40708090u},
                {Tex("DiffuseSampler", "known_road.dds"), Vec("SpecularExponent", 8.0f)});
  };
  // s_a: a road, a decal (dropped), a window sharing the x = 10 edge with the road but not its texture coordinates, an alpha tested neon sign with a far uv
  const auto sector_a = [&] {
    SynthGeo window = Quad(10, 0, 20, 10, 0, "CityWindowUpper", "w", {7.0f, 7.0f, 8.0f, 7.0f, 8.0f, 8.0f, 7.0f, 8.0f}, {0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u},
                           {Tex("DiffuseSampler", "known_win.dds"), Vec("WindowTintColor", 0.5f, 0.6f, 0.7f), Vec("SpecularExponent", 14.0f)});
    window.second_uv = true;
    return std::vector<SynthGeo>{asphalt(), Quad(0, 0, 5, 5, 0.01f, "CityGrimeDecal", "CityGrimeDecal", {0, 0, 1, 0, 1, 1, 0, 1}, {}, {Tex("DiffuseSampler", "known_decal.dds")}), window,
                                 Quad(0, 0, 1, 1, 4, "CityNeon", "CityNeonAlpha_Metal", {0.0f, 0.0f, 1000.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 1.0f}, {0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u},
                                      {Tex("DiffuseSampler", "missing_sign.dds"), Vec("NeonIntensity", 3.5f)})};
  };
  const auto sector_b = [&] {
    SynthGeo bare = Quad(100, 0, 110, 10, 1.5f, "CityBumpSpec", "CityBumpSpec_Cloth", {}, {}, {Tex("DiffuseSampler", "known_road.dds")});  // a declaration with no channel table
    bare.with_attributes = false;
    bare.uv = {0, 0, 0, 0, 0, 0, 0, 0};
    return std::vector<SynthGeo>{Quad(100, 0, 150, 50, 1, "CityBumpSpec", "CityBumpSpec_Cloth", {0.5f, 0.5f, 1.5f, 0.5f, 1.5f, 1.5f, 0.5f, 1.5f}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                      {Tex("DiffuseSampler", "known_wall.dds"), Vec("SpecularExponent", 15.0f)}),
                                 Quad(100, 0, 110, 10, 1.5f, "CityStoreInterior", "i", {0, 0, 1, 0, 1, 1, 0, 1}, {}, {}), bare};
  };
  const auto lod = [&] {
    return std::vector<SynthGeo>{Quad(0, 0, 100, 100, 20, "CityDefault", "CityDefault", {0, 0, 1, 0, 1, 1, 0, 1}, {0xFF202020u, 0xFF202020u, 0xFF202020u, 0xFF202020u}, {Tex("DiffuseSampler", "known_lod.dds"), Vec("SpecularExponent", 8.0f)}),
                                 Quad(100, 0, 200, 100, 20, "CityDefault", "CityDefault", {0, 0, 1, 0, 1, 1, 0, 1}, {}, {Tex("DiffuseSampler", "known_lod.dds"), Vec("SpecularExponent", 8.0f)}), SynthGeo{}};
  };
  mc::modloader::Rpf3Writer writer;
  add(writer, kCityTablePath, MakeCityTable(table));
  add(writer, SectorArchivePath("s_a"), MakeSectorFile(sector_a()));
  add(writer, SectorArchivePath("s_b"), MakeSectorFile(sector_b()));
  add(writer, kLodPath, MakeLodFile(lod()));
  CHECK(writer.Write(rpf));

  BuildRequest request;
  request.source_rpf = rpf;
  request.out_dir = g_scratch / "out";
  request.lzx = [](const uint8_t*, size_t, std::vector<uint8_t>&, size_t) { return false; };
  request.threads = 1;
  CountingSink sink;
  HitBuildRequest hit;
  hit.base = request;

  // no base pack yet: a refusal that says so
  HitBuildResult none = BuildCityHitPack(hit, &sink);
  CHECK(!none.ok && none.error.find("city pack") != std::string::npos);

  const BuildResult base = BuildCityPack(request, &sink);
  CHECK(base.ok);
  if (!base.ok) { std::fprintf(stderr, "base build failed: %s\n", base.error.c_str()); return; }

  FakeTextures textures;
  hit.textures = &textures;
  const HitBuildResult one = BuildCityHitPack(hit, &sink);
  CHECK(one.ok && !one.up_to_date);
  if (!one.ok) { std::fprintf(stderr, "hit build failed: %s\n", one.error.c_str()); return; }
  CHECK(one.census.sectors == 3 && one.census.hi_blobs == 2 && one.census.lod_blobs == 2 && one.census.sectors_failed == 0);
  CHECK(one.census.hi_triangles == 10 && one.census.lod_triangles == 4);   // s_a: road 2 + window 2 + alpha neon 2 (the decal is dropped); s_b: wall 2 + the bare quad 2 (the interior is dropped)
  CHECK(one.census.hi_runs == one.census.collect.runs - 2 && one.census.hi_runs + one.census.lod_runs == one.census.collect.runs);
  // materials: the default, the road, the window, the neon sign, the far CityDefault (one for both lod models), the wall, the bare quad = 7; texture names: known_road, known_win,
  // missing_sign, known_lod, known_wall = 5, of which the four "known" ones resolve (the dropped decal's texture is not asked for)
  CHECK(one.census.materials == 7 && one.census.finalize.texture_names == 5 && one.census.finalize.names_resolved == 4 && one.census.textures == 4);
  CHECK(!one.census.Report().empty());

  HitPackReader reader;
  std::string error;
  CHECK(reader.Open(one.hit_path, &error));
  PackReader base_reader;
  CHECK(base_reader.Open(request.out_dir / kPackFileName, &error));
  CHECK(reader.sector_count() == 3 && reader.sector(0).hi_size != 0 && reader.sector(0).lod_size != 0 && reader.sector(2).hi_size == 0 && reader.sector(2).lod_size == 0);
  CHECK(reader.sector(0).hi_triangles == 6 && reader.sector(1).hi_triangles == 4 && reader.sector(0).name_hash == RageHashText("s_a"));
  CHECK(reader.header().base_table_hash == base_reader.header().table_hash && reader.header().base_file_size == base_reader.header().file_size);
  CHECK(reader.header().texture_count == reader.textures().size() && reader.materials().size() == reader.header().material_count);
  CHECK(ValidateHitPack(reader, base_reader, nullptr) == 0);

  // s_a's blob: the opaque triangles (road, window) then the alpha one; every corner against the source
  HitBlob blob;
  CHECK(reader.ReadHitBlob(0, MeshKind::kHi, &blob, &error));
  CHECK(blob.triangle_count == 6 && blob.opaque_triangles == 4 && blob.has_corners() && blob.corners.size() == 6 * 6);
  const std::vector<SynthGeo> geos = sector_a();
  struct Expected { const SynthGeo* geo; bool alpha; uint32_t first_run_flags; };
  const HitRules rules;
  // runs: road, window, then the neon's two pieces (its far uv: 0 and 1000 in one triangle fan: the second triangle holds u 1000 for two of its corners)
  CHECK(blob.runs.size() >= 3);
  CHECK(CheckRuns(blob.runs, 6, 4).empty());
  const HitRun& road_run = blob.runs[0];
  CHECK(road_run.triangle_count == 2 && road_run.flags == (kRunHasUv | kRunHasColour));
  const HitMaterial& road_material = reader.materials()[road_run.material];
  CHECK(WordClass(road_material.word) == uint32_t(gb::SurfaceClass::kRoad) && (WordFlags(road_material.word) & kWordFlagSsr) != 0 && road_material.texture != kNoTexture);
  CHECK(road_material.key == reader.textures()[road_material.texture].key && road_material.shader_hash == ShaderHash("CityRoad", "CityRoad_Asphalt"));
  for (uint32_t t = 0; t < 2; ++t) {
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t vertex = geos[0].indices[t * 3 + k];
      float uv[2];
      DecodeCornerUv(road_run, blob.corners[size_t(t * 3 + k) * 2], uv);
      CHECK(Near(uv[0], geos[0].uv[vertex * 2], road_run.uv_scale[0] / 65535.0f) && Near(uv[1], geos[0].uv[vertex * 2 + 1], road_run.uv_scale[1] / 65535.0f));
      CHECK(blob.corners[size_t(t * 3 + k) * 2 + 1] == rules.QuantiseColour(CornerColourOfD3dColor(geos[0].colour[vertex])));
    }
  }
  // the window shares two vertices (the x = 10 edge) with the road in the welded mesh, but its corners carry the window's own texture coordinates
  const HitRun& window_run = blob.runs[1];
  CHECK(window_run.triangle_count == 2);
  for (uint32_t t = 0; t < 2; ++t) {
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t vertex = geos[2].indices[t * 3 + k];
      float uv[2];
      DecodeCornerUv(window_run, blob.corners[size_t(6 + t * 3 + k) * 2], uv);
      CHECK(Near(uv[0], geos[2].uv[vertex * 2], window_run.uv_scale[0] / 65535.0f) && uv[0] >= 7.0f - 1e-3f);
    }
  }
  const HitMaterial& window_material = reader.materials()[window_run.material];
  CHECK(WordClass(window_material.word) == uint32_t(gb::SurfaceClass::kGlass) && Near(window_material.tint[1], 0.6f, 0.003f));
  // the alpha tested neon sign: the flag, the glow, an unresolved texture (the average stands), and its runs are on the alpha side
  const HitRun& neon_run = blob.runs.back();
  CHECK((neon_run.flags & kRunAlphaTested) != 0 && neon_run.first_triangle >= 4);
  const HitMaterial& neon_material = reader.materials()[neon_run.material];
  CHECK(WordClass(neon_material.word) == uint32_t(gb::SurfaceClass::kEmissive) && neon_material.emissive == 3.5f && neon_material.texture == kNoTexture && Near(neon_material.average[0], 0.5f, 0.003f));
  // s_b: the wall (attributes), the bare quad (no channel table: no corner attributes, but a run, a material and the pack's triangles)
  HitBlob blob_b;
  CHECK(reader.ReadHitBlob(1, MeshKind::kHi, &blob_b, &error));
  CHECK(blob_b.triangle_count == 4 && blob_b.runs.size() == 2 && blob_b.runs[1].flags == 0);
  CHECK(blob_b.corners.size() == 4 * 6 && blob_b.corners[2 * 6 + 1] == kWhiteCorner && (blob_b.corners[2 * 6] == 0));
  // the whole way to the GPU's layout: the pool block H0's reference writer makes from the base pack's mesh and this blob, read back by H0's decode twin (what the hit shading's
  // library reads): the road's triangle 0 at its corner 1 and at its middle
  {
    MeshData mesh;
    CHECK(base_reader.ReadMesh(0, MeshKind::kHi, &mesh, &error));
    const PoolBlockPlan plan = PlanPoolBlock(mesh.vertex_count(), mesh.triangle_count(), uint32_t(blob.runs.size()), true, true);
    std::vector<uint8_t> pool(size_t(plan.total_bytes));
    CHECK(FillPoolBlock(plan, mesh, &blob, pool.data(), pool.size(), &error));
    GpuSectorRow row;
    CHECK(MakeSectorRow(plan, 0, mesh.triangle_count(), mesh.opaque_triangles, 0, &row));
    std::vector<uint32_t> slots(2 * 8, 0);  // the slots of sector 0: detail and far
    const uint32_t row_words[8] = {row.index_offset, row.position_offset, row.attribute_offset, row.run_id_offset, row.run_table_offset, row.triangle_count, row.opaque_triangles, row.flags};
    std::copy(row_words, row_words + 8, slots.begin());
    std::vector<uint32_t> material_words;
    for (const HitMaterial& m : reader.materials()) {
      uint32_t w[8];
      m.Pack(w);
      material_words.insert(material_words.end(), w, w + 8);
    }
    HitView view;
    view.pool = pool.data();
    view.pool_size = pool.size();
    view.sectors = slots.data();
    view.sector_slots = 2;
    view.materials = material_words.data();
    view.material_count = reader.materials().size();
    const uint32_t instance = kInstanceIdFlag | SectorRowOf(0, false);
    const DecodedHit corner1 = DecodeHit(view, instance, 0, 0, 1.0f, 0.0f);  // the barycentrics (b1, b2) = (1, 0): corner 1
    CHECK(corner1.valid && corner1.triangle == 0 && corner1.material == road_run.material && (corner1.run_flags & kRunHasUv) != 0);
    const uint32_t v1 = geos[0].indices[1];
    CHECK(Near(corner1.uv[0], geos[0].uv[v1 * 2], 0.002f) && Near(corner1.uv[1], geos[0].uv[v1 * 2 + 1], 0.002f));
    const float ao = float(rules.QuantiseColour(CornerColourOfD3dColor(geos[0].colour[v1])) >> 24) / 255.0f;
    CHECK(Near(corner1.colour[3], ao, 0.003f));
    const DecodedHit middle = DecodeHit(view, instance, 0, 1, 1.0f / 3.0f, 1.0f / 3.0f);  // the second road triangle (0 2 3), its centre
    const float mu = (geos[0].uv[geos[0].indices[3] * 2] + geos[0].uv[geos[0].indices[4] * 2] + geos[0].uv[geos[0].indices[5] * 2]) / 3.0f;
    CHECK(middle.valid && middle.triangle == 1 && Near(middle.uv[0], mu, 0.003f) && middle.world_area > 0.0f && middle.uv_area > 0.0f);
    // an alpha tested triangle is found by geometry index 1 and its primitive within that geometry (opaque triangles first)
    const DecodedHit neon = DecodeHit(view, instance, 1, 0, 0.0f, 0.0f);
    CHECK(neon.valid && neon.triangle == 4 && (neon.run_flags & kRunAlphaTested) != 0 && reader.materials()[neon.material].emissive == 3.5f);
  }
  // the far models: one blob each, runs and corners
  HitBlob lod_blob;
  CHECK(reader.ReadHitBlob(1, MeshKind::kLod, &lod_blob, &error) && lod_blob.triangle_count == 2 && lod_blob.runs.size() == 1);
  CHECK(!reader.ReadHitBlob(2, MeshKind::kLod, &lod_blob, &error) && error.empty());  // no blob is not an error
  // the textures: names in the order the materials first use them, and the file the set wrote is stamped with the hit table's hash
  CHECK(textures.finished_hash == reader.header().table_hash && textures.finished_hash == one.table_hash);
  CHECK(fs::exists(request.out_dir / kTexFileName));
  { std::vector<uint8_t> tex = ReadAll(request.out_dir / kTexFileName); ByteReader tr(tex.data(), tex.size()); TexHeader th; CHECK(TexHeader::Read(tr, &th, &error) && th.hit_table_hash == reader.header().table_hash); }

  // current now; the same pack with four threads (in another folder over a copy of the base pack): byte for byte the same
  CHECK(CheckCityHitPack(hit).current);
  hit.base.threads = 4;
  CHECK(BuildCityHitPack(hit, &sink).up_to_date);
  {
    HitBuildRequest four = hit;
    four.base.out_dir = g_scratch / "out4";
    four.base.force = true;
    fs::create_directories(four.base.out_dir);
    fs::copy_file(request.out_dir / kPackFileName, four.base.out_dir / kPackFileName, fs::copy_options::overwrite_existing);
    fs::copy_file(request.out_dir / kStampFileName, four.base.out_dir / kStampFileName, fs::copy_options::overwrite_existing);
    FakeTextures other;
    four.textures = &other;
    const HitBuildResult f = BuildCityHitPack(four, &sink);
    CHECK(f.ok && !f.up_to_date);
    CHECK(ReadAll(one.hit_path) == ReadAll(f.hit_path));
  }
  hit.base.threads = 1;

  // the stamp notices other rules, another texture set, other rows, a changed base pack, a missing file
  {
    HitBuildRequest changed = hit;
    changed.rules.uv_step_log2 = 8;
    StampCheck c = CheckCityHitPack(changed);
    CHECK(!c.current && c.why.find("precision") != std::string::npos);
    changed = hit;
    changed.textures = nullptr;
    c = CheckCityHitPack(changed);
    CHECK(!c.current && c.why.find("texture set") != std::string::npos);
    changed = hit;
    const fs::path rows_file = g_scratch / "rows2.txt";
    { std::ofstream(rows_file) << "@version 1\n*/known_road class=metal\n"; }
    changed.texture_rows = {rows_file};
    c = CheckCityHitPack(changed);
    CHECK(!c.current && c.why.find("rows") != std::string::npos);
    Stamp stamp;
    CHECK(Stamp::Read(request.out_dir / kHitStampFileName, &stamp, &error));
    stamp.Set("hit_builder_version", uint64_t(kHitBuilderVersion + 1));
    CHECK(stamp.Write(request.out_dir / kHitStampFileName, &error));
    c = CheckCityHitPack(hit);
    CHECK(!c.current && c.why.find("builder") != std::string::npos);
    CHECK(BuildCityHitPack(hit, &sink).ok);  // rebuilt (stale): current again
    CHECK(CheckCityHitPack(hit).current);
    // the base pack rebuilt from other bytes: the hit data is not over it any more
    BuildRequest forced = request;
    forced.force = true;
    forced.drop_mask = 0;  // keeps the decal and the interior: another pack
    CHECK(BuildCityPack(forced, &sink).ok);
    c = CheckCityHitPack(hit);
    CHECK(!c.current && c.why.find("city pack") != std::string::npos);
    // ... and it is built over the new one, with the base's own drop mask (the decal and the interior are in it now)
    const HitBuildResult kept = BuildCityHitPack(hit, &sink);
    CHECK(kept.ok && kept.census.hi_triangles == 14);
    CHECK(CheckCityHitPack(hit).current);
    HitPackReader kept_reader;
    PackReader kept_base;
    CHECK(kept_reader.Open(kept.hit_path, &error) && kept_base.Open(request.out_dir / kPackFileName, &error) && ValidateHitPack(kept_reader, kept_base, nullptr) == 0);
    // restore the default base pack
    BuildRequest restore = request;
    restore.force = true;
    CHECK(BuildCityPack(restore, &sink).ok);
  }

  // the validator finds a hit file of another base, a damaged table, a cut file
  {
    HitPackReader reader2;
    CHECK(BuildCityHitPack(hit, &sink).ok && reader2.Open(hit.base.out_dir / kHitFileName, &error));
    std::vector<uint8_t> bytes = ReadAll(hit.base.out_dir / kHitFileName);
    const fs::path damaged = g_scratch / "damaged.pack";
    std::vector<uint8_t> flipped = bytes;
    flipped[kHitHeaderBytes + 6] ^= 0x01;  // a sector record
    { std::ofstream(damaged, std::ios::binary).write(reinterpret_cast<const char*>(flipped.data()), std::streamsize(flipped.size())); }
    HitPackReader d;
    CHECK(!d.Open(damaged, &error));
    { std::ofstream(damaged, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size() - 5)); }
    CHECK(!d.Open(damaged, &error));
    CHECK(!d.Open(g_scratch / "no_such.pack", &error));
    // a flipped byte inside a blob is found by the validator (the blob's hash), not by Open
    flipped = bytes;
    flipped[size_t(reader2.sector(0).hi_offset) + 100] ^= 0x40;
    { std::ofstream(damaged, std::ios::binary).write(reinterpret_cast<const char*>(flipped.data()), std::streamsize(flipped.size())); }
    HitPackReader dd;
    CHECK(dd.Open(damaged, &error));
    std::vector<std::string> problems;
    CHECK(ValidateHitPack(dd, base_reader, &problems) >= 1 && !problems.empty());
  }

  // census only writes nothing, a cancelled build leaves no file and no stamp, a build that fails leaves none either
  {
    HitBuildRequest census = hit;
    census.base.out_dir = g_scratch / "out_census";
    fs::create_directories(census.base.out_dir);
    fs::copy_file(request.out_dir / kPackFileName, census.base.out_dir / kPackFileName, fs::copy_options::overwrite_existing);
    census.base.census_only = true;
    const HitBuildResult c = BuildCityHitPack(census, &sink);
    CHECK(c.ok && c.census.hi_triangles == 10 && c.census.file_bytes > 0 && !fs::exists(census.base.out_dir / kHitFileName) && !fs::exists(census.base.out_dir / kHitStampFileName));
    HitBuildRequest cancel = hit;
    cancel.base.out_dir = g_scratch / "out_cancel";
    cancel.base.force = true;
    fs::create_directories(cancel.base.out_dir);
    fs::copy_file(request.out_dir / kPackFileName, cancel.base.out_dir / kPackFileName, fs::copy_options::overwrite_existing);
    CountingSink stopper;
    stopper.cancel_after = 2;
    const HitBuildResult x = BuildCityHitPack(cancel, &stopper);
    CHECK(!x.ok && x.cancelled);
    CHECK(!fs::exists(cancel.base.out_dir / kHitFileName) && !fs::exists(cancel.base.out_dir / kHitStampFileName) && !fs::exists(cancel.base.out_dir / (std::string(kHitFileName) + ".body")));
  }

  // a base pack that is not of these files (the archive changed after it was built): the hit pass says so, nothing is written
  {
    mc::modloader::Rpf3Writer changed;
    add(changed, kCityTablePath, MakeCityTable(table));
    std::vector<SynthGeo> other = sector_a();
    other.pop_back();  // no neon sign any more
    add(changed, SectorArchivePath("s_a"), MakeSectorFile(other));
    add(changed, SectorArchivePath("s_b"), MakeSectorFile(sector_b()));
    add(changed, kLodPath, MakeLodFile(lod()));
    const fs::path rpf2 = g_scratch / "other" / "xarchive_cache.rpf";
    fs::create_directories(rpf2.parent_path());
    CHECK(changed.Write(rpf2));
    HitBuildRequest wrong = hit;
    wrong.base.source_rpf = rpf2;
    wrong.base.force = true;
    wrong.base.out_dir = g_scratch / "out_wrong";
    fs::create_directories(wrong.base.out_dir);
    fs::copy_file(request.out_dir / kPackFileName, wrong.base.out_dir / kPackFileName, fs::copy_options::overwrite_existing);
    const HitBuildResult w = BuildCityHitPack(wrong, &sink);
    CHECK(!w.ok && w.error.find("base pack") != std::string::npos);
    CHECK(!fs::exists(wrong.base.out_dir / kHitFileName) && !fs::exists(wrong.base.out_dir / kHitStampFileName));
  }
}

}  // namespace

int main(int argc, char** argv) {
  g_scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "larecomp_city_hit_builder_test";
  std::error_code ec;
  fs::remove_all(g_scratch, ec);
  TestParams();
  TestLayouts();
  TestQuantisation();
  TestIndexedCorners();
  TestMaterials();
  TestCollector();
  TestBuilder();
  fs::remove_all(g_scratch, ec);
  if (g_failures != 0) {
    std::fprintf(stderr, "city_hit_builder_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("city_hit_builder_test: %d checks passed\n", g_checks);
  return 0;
}
