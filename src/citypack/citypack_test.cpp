// The city pack's checks (no game files, no renderer): the byte helpers, the index codec, the mesh blob (both encodings, damage), the weld, the shader
// classes, the reader of the game's resources on SYNTHETIC resources made here in the game's layout, and the whole builder on a synthetic RPF3 archive:
// the pack's bytes are the same with one thread and with four, the reader and the validator agree with the census, the stamp says current and then not.
//
//   build: see src/citypack/CMakeLists.txt (larecomp_citypack_test); run it with no argument (exit 0 = passed).
//   The builder test needs src/mc_engine/modloader/{rpf3,aes256,texture}.cpp (the archive writer and reader) in the same program.

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/material_class.h"
#include "citypack/mesh_builder.h"
#include "citypack/pack_format.h"
#include "citypack/pack_reader.h"
#include "citypack/rsc_image.h"
#include "citypack/sector_parse.h"
#include "citypack/stamp.h"
#include "mc_engine/modloader/rpf3.h"

using namespace larecomp::citypack;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                            \
  } while (0)

bool NearlyEqual(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// ---------------------------------------------------------------------------
// synthetic resources in the game's layout
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

struct SynthGeo {
  std::string group, preset;
  std::vector<float> positions;     // x, y, z per vertex
  std::vector<uint16_t> indices;
  uint32_t stride = 28;
  uint32_t bucket = 1;
};

// The segment size flag for a virtual segment of `size` bytes (a power of two, at least 2048): mantissa 1 << shift.
uint32_t FlagFor(uint32_t size) {
  uint32_t shift = 0;
  while ((1u << (shift + 8)) < size) ++shift;
  return 1u | (shift << 11);
}

std::vector<uint8_t> StoredFile(Img& img, uint32_t type, uint32_t* flag_out) {
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
  if (flag_out) *flag_out = flag;
  return file;
}

// A shader table, written once: returns the group pointer. Shaders are (group, preset) pairs.
uint32_t WriteShaderGroup(Img& img, const std::vector<std::pair<std::string, std::string>>& shaders) {
  const uint32_t group = img.Alloc(0x20);
  const uint32_t array = img.Alloc(4 * shaders.size());
  img.U32(group + 8, Img::P(array));
  img.U16(group + 12, uint16_t(shaders.size()));
  for (size_t i = 0; i < shaders.size(); ++i) {
    const uint32_t shader = img.Alloc(0x80);
    img.U32(shader + 48, Img::P(img.Str(shaders[i].first)));
    img.U32(shader + 60, Img::P(img.Str(shaders[i].second)));
    img.U32(array + 4 * uint32_t(i), Img::P(shader));
  }
  return Img::P(group);
}

// One grmModel per geometry list entry (a model has one geometry), its vertex and index data.
uint32_t WriteModel(Img& img, const SynthGeo& g, uint16_t shader_index) {
  const uint32_t vertex_count = uint32_t(g.positions.size() / 3);
  const uint32_t vdata = img.Alloc(size_t(vertex_count) * g.stride);
  for (uint32_t v = 0; v < vertex_count; ++v) {
    for (int a = 0; a < 3; ++a) img.F32(vdata + v * g.stride + 4 * uint32_t(a), g.positions[size_t(v) * 3 + size_t(a)]);
    for (uint32_t k = 12; k + 4 <= g.stride; k += 4) img.U32(vdata + v * g.stride + k, 0x01020304u);  // other attributes: any bytes
  }
  const uint32_t idata = img.Alloc(g.indices.size() * 2);
  for (size_t i = 0; i < g.indices.size(); ++i) img.U16(idata + uint32_t(i) * 2, g.indices[i]);
  const uint32_t declaration = img.Alloc(0x10);
  img.U32(declaration, 0x4069);  // FVF with bit 0
  const uint32_t vb = img.Alloc(0x30);
  img.U16(vb + 4, uint16_t(vertex_count));
  img.U32(vb + 12, g.stride);
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

// A sector resource: the shader table is the distinct (group, preset) pairs of the geometries.
std::vector<uint8_t> MakeSectorFile(const std::vector<SynthGeo>& geos) {
  Img img;
  img.Alloc(0x30);  // the root at offset 0
  std::vector<std::pair<std::string, std::string>> shaders;
  std::vector<uint16_t> shader_of(geos.size());
  for (size_t i = 0; i < geos.size(); ++i) {
    const auto key = std::make_pair(geos[i].group, geos[i].preset);
    auto it = std::find(shaders.begin(), shaders.end(), key);
    if (it == shaders.end()) {
      shaders.push_back(key);
      it = shaders.end() - 1;
    }
    shader_of[i] = uint16_t(it - shaders.begin());
  }
  const uint32_t group = WriteShaderGroup(img, shaders);
  std::vector<uint32_t> models;
  for (size_t i = 0; i < geos.size(); ++i) models.push_back(WriteModel(img, geos[i], shader_of[i]));
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
  img.U32(drawable + 8, group);
  img.U32(drawable + 12, Img::P(dictionary));
  img.U32(16, Img::P(drawable));
  img.U32(32, Img::P(city_models));
  img.U16(36, uint16_t(models.size()));
  return StoredFile(img, 1, nullptr);
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
  return StoredFile(img, 1086, nullptr);
}

// lod.xmd: `models[i]` is the geometry of sector i's far model (an empty positions list: a null model).
std::vector<uint8_t> MakeLodFile(const std::vector<SynthGeo>& models) {
  Img img;
  img.Alloc(0x30);
  const uint32_t group = WriteShaderGroup(img, {{"CityDefault", "CityDefault"}});
  const uint32_t array = img.Alloc(4 * models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    if (models[i].positions.empty()) continue;
    img.U32(array + 4 * uint32_t(i), WriteModel(img, models[i], 0));
  }
  img.U32(8, group);
  img.U32(12, Img::P(array));
  img.U16(16, uint16_t(models.size()));
  img.U16(18, uint16_t(models.size()));
  return StoredFile(img, 1, nullptr);
}

SynthGeo Quad(float x0, float z0, float x1, float z1, float y, const char* group, const char* preset, uint32_t stride = 28, uint32_t bucket = 1) {
  SynthGeo g;
  g.group = group;
  g.preset = preset;
  g.stride = stride;
  g.bucket = bucket;
  g.positions = {x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1};
  g.indices = {0, 1, 2, 0, 2, 3};
  return g;
}

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

void TestBytes() {
  ByteWriter w;
  w.U8(0xAB);
  w.U16(0x1234);
  w.U32(0xDEADBEEF);
  w.U64(0x0123456789ABCDEFull);
  w.F32(-1.5f);
  w.Varint(0);
  w.Varint(127);
  w.Varint(128);
  w.Varint(0xFFFFFFFFu);
  ByteReader r(w.bytes().data(), w.size());
  CHECK(r.U8() == 0xAB);
  CHECK(r.U16() == 0x1234);
  CHECK(r.U32() == 0xDEADBEEFu);
  CHECK(r.U64() == 0x0123456789ABCDEFull);
  CHECK(r.F32() == -1.5f);
  CHECK(r.Varint() == 0);
  CHECK(r.Varint() == 127);
  CHECK(r.Varint() == 128);
  CHECK(r.Varint() == 0xFFFFFFFFu);
  CHECK(r.ok() && r.left() == 0);
  r.U8();
  CHECK(!r.ok());  // a read past the end fails and stays failed
  CHECK(r.U32() == 0);
  const uint8_t be[4] = {0x41, 0x20, 0x00, 0x00};
  CHECK(LoadBEFloat(be) == 10.0f);
  CHECK(LoadBE16(be) == 0x4120);
  // a varint of six bytes is not a 32 bit value
  const uint8_t bad[6] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  ByteReader br(bad, sizeof(bad));
  br.Varint();
  CHECK(!br.ok());
}

void TestHashAndClasses() {
  // values from the game's own hash (atlas_game.py rage_hash; the shader's name hash in the sector data is 0xed86037f for CityBumpSpec)
  CHECK(RageHashText("be_smb_01") == 0xd2849820u);
  CHECK(RageHashText("hw_ss_07") == 0x51653248u);
  CHECK(RageHashText("CityBumpSpec") == 0xed86037fu);
  CHECK(RageHashText("CITYBUMPSPEC") == RageHashText("citybumpspec"));
  CHECK(RageHashText("a\\b") == RageHashText("a/b"));
  CHECK(RageHashText("") == 0);

  CHECK(ClassifyShader("CityBumpSpec", "CityBumpSpec_Cloth") == MaterialClass::kSolid);
  CHECK(ClassifyShader("CityDefault", "CityDefault") == MaterialClass::kSolid);
  CHECK(ClassifyShader("CityGrime", "CityGrime_PaintedWall") == MaterialClass::kSolid);
  CHECK(ClassifyShader("CityGrimeMetal", "") == MaterialClass::kSolid);
  CHECK(ClassifyShader("CityRoad", "CityRoad_Asphalt") == MaterialClass::kGround);
  CHECK(ClassifyShader("CityTerrain", "") == MaterialClass::kGround);
  CHECK(ClassifyShader("CityWindowUpper", "CityWindowUpper") == MaterialClass::kGlass);
  CHECK(ClassifyShader("CityWindowLower", "") == MaterialClass::kGlass);
  CHECK(ClassifyShader("CityNeon", "CityNeon_UnpaintedWall") == MaterialClass::kEmissive);
  CHECK(ClassifyShader("CityNeon", "CityNeonAlpha_Metal") == MaterialClass::kAlphaTested);
  CHECK(ClassifyShader("CityBillboard", "CityBillboard") == MaterialClass::kBillboard);
  CHECK(ClassifyShader("CityGrimeDecal", "CityGrimeDecal") == MaterialClass::kDecal);
  CHECK(ClassifyShader("CityGroundGrimeDecal", "") == MaterialClass::kDecal);  // decal beats ground
  CHECK(ClassifyShader("GroundGrimeDecal", "") == MaterialClass::kDecal);
  CHECK(ClassifyShader("TintBumpDecal2", "") == MaterialClass::kDecal);
  CHECK(ClassifyShader("CityStoreInterior", "") == MaterialClass::kInterior);
  CHECK(ClassifyShader("CityGrime", "CityGrime_UnpaintedWall_int") == MaterialClass::kInterior);
  CHECK(ClassifyShader("CityBumpSpec", "CityBumpSpecAlpha_Metal_int") == MaterialClass::kInterior);  // interior beats alpha
  CHECK(ClassifyShader("CityPrint", "CityPrint") == MaterialClass::kSolid);                          // "int" inside a word is not _int
  CHECK(ClassifyShader("CityWindowDirt", "CityWindowDirt") == MaterialClass::kDecal);
  CHECK(ClassifyShader("CityWater", "") == MaterialClass::kWater);
  CHECK(ClassifyShader("CityStreetSign", "") == MaterialClass::kSolid);  // "street" has "tree" in it: no short words
  CHECK(ClassifyShader("", "") == MaterialClass::kSolid);
  CHECK(ShaderHash("CityBumpSpec", "x") == RageHashText("CityBumpSpec/x"));
  CHECK((kDefaultDropMask & ClassBit(MaterialClass::kDecal)) != 0 && (kDefaultDropMask & ClassBit(MaterialClass::kSolid)) == 0);
  CHECK(IsOpaqueClass(MaterialClass::kGlass) && !IsOpaqueClass(MaterialClass::kAlphaTested));
}

void TestIndexCodec() {
  std::vector<uint32_t> indices;
  for (uint32_t t = 0; t < 1000; ++t) {
    indices.push_back(t);
    indices.push_back(t + 1);
    indices.push_back(t + 2);
  }
  indices.push_back(5);          // a long jump back
  indices.push_back(65535);
  indices.push_back(0);
  ByteWriter w;
  EncodeIndicesDelta(indices.data(), indices.size(), w);
  CHECK(w.size() < indices.size() * 2);  // a strip-like list is about a byte and a bit each
  std::vector<uint32_t> back;
  ByteReader r(w.bytes().data(), w.size());
  CHECK(DecodeIndicesDelta(r, indices.size(), 70000, &back));
  CHECK(back == indices);
  // past the vertices
  ByteReader r2(w.bytes().data(), w.size());
  CHECK(!DecodeIndicesDelta(r2, indices.size(), 65535, &back));
  // cut short
  ByteReader r3(w.bytes().data(), w.size() / 2);
  CHECK(!DecodeIndicesDelta(r3, indices.size(), 70000, &back));
  // the largest values
  const std::vector<uint32_t> big = {0, 0x7FFFFFFF, 0, 0x7FFFFFFF};
  ByteWriter w2;
  EncodeIndicesDelta(big.data(), big.size(), w2);
  ByteReader r4(w2.bytes().data(), w2.size());
  std::vector<uint32_t> big_back;
  CHECK(DecodeIndicesDelta(r4, big.size(), 0x80000000u, &big_back) && big_back == big);
}

MeshData MakeMesh() {
  MeshBuilder b;
  const SynthGeo a = Quad(0, 0, 10, 10, 0, "CityRoad", "CityRoad_Asphalt");
  const SynthGeo c = Quad(10, 0, 20, 10, 0, "CityRoad", "CityRoad_Asphalt");  // shares the edge x = 10
  const SynthGeo glass = Quad(0, 0, 4, 4, 5, "CityWindowUpper", "w");
  const SynthGeo alpha = Quad(0, 0, 3, 3, 8, "CityNeon", "CityNeonAlpha_Metal");
  const uint32_t h1 = ShaderHash("CityRoad", "CityRoad_Asphalt");
  b.AddGeometry(a.positions.data(), 4, a.indices.data(), 6, MaterialClass::kGround, 0, h1);
  b.AddGeometry(alpha.positions.data(), 4, alpha.indices.data(), 6, MaterialClass::kAlphaTested, 3, 77);
  b.AddGeometry(c.positions.data(), 4, c.indices.data(), 6, MaterialClass::kGround, 0, h1);
  b.AddGeometry(glass.positions.data(), 4, glass.indices.data(), 6, MaterialClass::kGlass, 2, 55);
  return b.Finish();
}

void TestMeshBuilder() {
  const MeshData mesh = MakeMesh();
  // road quad + road quad (the shared edge's two corners weld: 6 vertices) + glass (4 more) + alpha (4 more)
  CHECK(mesh.triangle_count() == 8);
  CHECK(mesh.opaque_triangles == 6);          // alpha last
  CHECK(mesh.alpha_triangles() == 2);
  // the two road quads and the glass quad: positions (0,0,0),(10,0,0),(10,0,10),(0,0,10) | (20,0,0),(20,0,10) | glass (4,5,0),(4,5,4),(0,5,4),(0,5,0) | alpha
  CHECK(mesh.vertex_count() == 4 + 2 + 4 + 4);
  // the runs: the two road geometries are adjacent in the opaque list and merge; glass is its own; alpha comes after with its flag
  CHECK(mesh.runs.size() == 3);
  CHECK(mesh.runs[0].material == MaterialClass::kGround && mesh.runs[0].first_triangle == 0 && mesh.runs[0].triangle_count == 4);
  CHECK(mesh.runs[1].material == MaterialClass::kGlass && mesh.runs[1].first_triangle == 4 && mesh.runs[1].triangle_count == 2);
  CHECK(mesh.runs[2].material == MaterialClass::kAlphaTested && mesh.runs[2].first_triangle == 6 && (mesh.runs[2].flags & 1) != 0);
  CHECK(mesh.bounds_min[0] == 0 && mesh.bounds_max[0] == 20 && mesh.bounds_min[1] == 0 && mesh.bounds_max[1] == 8);
  for (const uint32_t i : mesh.indices) CHECK(i < mesh.vertex_count());

  // degenerate and bad triangles
  MeshBuilder b;
  const std::vector<float> p = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0, 0, 3, 0, 0};  // v1 and v2 coincide; v4, v5 are collinear with v0
  const std::vector<uint16_t> idx = {0, 1, 3,    // fine
                                     0, 1, 2,    // two corners at one position
                                     0, 4, 5,    // collinear: no area
                                     0, 1, 9};   // an index past the vertices
  CHECK(b.AddGeometry(p.data(), 6, idx.data(), 12, MaterialClass::kSolid, 0, 1) == 1);
  CHECK(b.stats().dropped_degenerate == 2 && b.stats().dropped_bad_index == 1);
  const std::vector<float> nan = {0, 0, 0, 1, 0, 0, NAN, 1, 0};
  const std::vector<uint16_t> ni = {0, 1, 2};
  CHECK(b.AddGeometry(nan.data(), 3, ni.data(), 3, MaterialClass::kSolid, 0, 1) == 0 && b.stats().dropped_not_finite == 1);
  // negative zero welds with zero
  MeshBuilder z;
  const std::vector<float> zp = {0, 0, 0, -0.0f, 0, 1, 1, 0, 0, 0, 0, 0.0f, 1, 0, -0.0f, 5, 5, 5};
  const std::vector<uint16_t> zi = {0, 2, 5, 3, 4, 5};
  z.AddGeometry(zp.data(), 6, zi.data(), 6, MaterialClass::kSolid, 0, 1);
  const MeshData zm = z.Finish();
  CHECK(zm.vertex_count() == 3);  // (0,0,0) = vertices 0 and 3, (1,0,0) = vertices 2 and 4 (its z is -0), (5,5,5); vertex 1 is used by no triangle
}

void TestMeshBlob() {
  const MeshData mesh = MakeMesh();
  for (const PositionEncoding pe : {PositionEncoding::kFloat32, PositionEncoding::kGrid16}) {
    for (const IndexEncoding ie : {IndexEncoding::kRaw32, IndexEncoding::kDeltaVarint}) {
      const std::vector<uint8_t> blob = EncodeMesh(mesh, pe, ie);
      MeshData back;
      std::string error;
      CHECK(DecodeMesh(blob.data(), blob.size(), &back, &error));
      CHECK(back.indices == mesh.indices);
      CHECK(back.opaque_triangles == mesh.opaque_triangles);
      CHECK(back.runs.size() == mesh.runs.size());
      for (size_t i = 0; i < mesh.runs.size() && i < back.runs.size(); ++i) {
        CHECK(back.runs[i].first_triangle == mesh.runs[i].first_triangle && back.runs[i].triangle_count == mesh.runs[i].triangle_count);
        CHECK(back.runs[i].material == mesh.runs[i].material && back.runs[i].flags == mesh.runs[i].flags);
        CHECK(back.runs[i].shader_index == mesh.runs[i].shader_index && back.runs[i].shader_hash == mesh.runs[i].shader_hash);
      }
      CHECK(back.positions.size() == mesh.positions.size());
      const float tolerance = pe == PositionEncoding::kFloat32 ? 0.0f : (20.0f / 65535.0f) * 0.5f + 1e-5f;
      for (size_t i = 0; i < mesh.positions.size() && i < back.positions.size(); ++i) CHECK(NearlyEqual(back.positions[i], mesh.positions[i], tolerance));
      // damage: a flipped byte anywhere after the header fails the hash; a cut blob fails; a wrong magic fails
      std::vector<uint8_t> damaged = blob;
      damaged[blob.size() / 2] ^= 0x40;
      CHECK(!DecodeMesh(damaged.data(), damaged.size(), &back, &error));
      CHECK(!DecodeMesh(blob.data(), blob.size() - 9, &back, &error));
      std::vector<uint8_t> magic = blob;
      magic[0] ^= 0xFF;
      CHECK(!DecodeMesh(magic.data(), magic.size(), &back, &error));
    }
  }
  // the delta encoding is smaller than raw on this mesh, and the grid is smaller than floats
  CHECK(EncodeMesh(mesh, PositionEncoding::kFloat32, IndexEncoding::kDeltaVarint).size() < EncodeMesh(mesh, PositionEncoding::kFloat32, IndexEncoding::kRaw32).size());
  CHECK(EncodeMesh(mesh, PositionEncoding::kGrid16, IndexEncoding::kRaw32).size() < EncodeMesh(mesh, PositionEncoding::kFloat32, IndexEncoding::kRaw32).size());
  // an empty mesh round-trips
  MeshData empty;
  const std::vector<uint8_t> blob = EncodeMesh(empty);
  MeshData back;
  std::string error;
  CHECK(DecodeMesh(blob.data(), blob.size(), &back, &error) && back.empty());
  // a stream id this build does not know is skipped (a later version's attribute stream)
  {
    ByteWriter body;
    std::vector<StreamEntry> table;
    const MeshData one = [] { MeshBuilder b; const SynthGeo q = Quad(0, 0, 1, 1, 0, "a", "b"); b.AddGeometry(q.positions.data(), 4, q.indices.data(), 6, MaterialClass::kSolid, 0, 1); return b.Finish(); }();
    const uint32_t body_start = kMeshHeaderBytes + 4 * kStreamEntryBytes;
    StreamEntry pos{kStreamPositions, 0, 0, 0, one.vertex_count(), 12};
    pos.offset = body_start + uint32_t(body.size());
    for (float v : one.positions) body.F32(v);
    pos.size = uint32_t(one.positions.size() * 4);
    StreamEntry idx{kStreamIndices, 0, 0, 0, one.triangle_count() * 3, 4};
    idx.offset = body_start + uint32_t(body.size());
    for (uint32_t v : one.indices) body.U32(v);
    idx.size = uint32_t(one.indices.size() * 4);
    StreamEntry unknown{kStreamFirstAttribute + 5, 9, 0, 0, 3, 4};
    unknown.offset = body_start + uint32_t(body.size());
    for (int i = 0; i < 3; ++i) body.U32(0x11111111u);
    unknown.size = 12;
    StreamEntry runs{kStreamRuns, 0, 0, 0, 0, kRunBytes};
    runs.offset = body_start + uint32_t(body.size());
    table = {pos, unknown, idx, runs};
    ByteWriter tail;
    for (const StreamEntry& e : table) e.Write(tail);
    tail.Bytes(body.bytes().data(), body.size());
    MeshHeader h;
    h.vertex_count = one.vertex_count();
    h.triangle_count = one.triangle_count();
    h.opaque_triangles = one.triangle_count();
    h.run_count = 0;
    h.stream_count = 4;
    h.body_hash = Fnv1a64(tail.bytes().data(), tail.size());
    ByteWriter blob2;
    h.Write(blob2);
    blob2.Bytes(tail.bytes().data(), tail.size());
    MeshData out;
    CHECK(DecodeMesh(blob2.bytes().data(), blob2.size(), &out, &error));
    CHECK(out.triangle_count() == 2 && out.vertex_count() == 4);
  }
}

void TestHeaderAndRecord() {
  PackHeader h;
  h.sector_count = 439;
  h.names_bytes = 1234;
  h.table_offset = 128;
  h.names_offset = 128 + 439 * 160;
  h.blobs_offset = 99999;
  h.file_size = 123456789012ull;
  h.table_hash = 0xCAFEF00Dull;
  ByteWriter w;
  h.Write(w);
  CHECK(w.size() == kHeaderBytes);
  PackHeader back;
  std::string error;
  ByteReader r(w.bytes().data(), w.size());
  CHECK(PackHeader::Read(r, &back, &error));
  CHECK(back.sector_count == 439 && back.file_size == 123456789012ull && back.table_hash == 0xCAFEF00Dull && back.blobs_offset == 99999);
  std::vector<uint8_t> wrong = w.bytes();
  wrong[0] = 'X';
  ByteReader r2(wrong.data(), wrong.size());
  CHECK(!PackHeader::Read(r2, &back, &error));
  std::vector<uint8_t> version = w.bytes();
  version[8] = 9;  // another format version
  ByteReader r3(version.data(), version.size());
  CHECK(!PackHeader::Read(r3, &back, &error) && error.find("format version") != std::string::npos);

  SectorRecord s;
  s.name_hash = 1; s.name_offset = 2; s.name_length = 3; s.flags = 7;
  s.aabb_min[0] = -4076.25f; s.aabb_max[2] = 99.5f;
  s.hi_offset = 0x100000000ull; s.hi_size = 5; s.hi_triangles = 6; s.hi_vertices = 7; s.hi_opaque_triangles = 8;
  s.lod_offset = 0x200000000ull; s.lod_size = 9; s.lod_triangles = 10; s.lod_vertices = 11; s.dropped_triangles = 12;
  s.lod_min[1] = 3.0f; s.hi_max[0] = 4.0f;
  ByteWriter sw;
  s.Write(sw);
  CHECK(sw.size() == kSectorRecordBytes);
  SectorRecord sb;
  ByteReader sr(sw.bytes().data(), sw.size());
  CHECK(SectorRecord::Read(sr, &sb));
  CHECK(sb.aabb_min[0] == -4076.25f && sb.aabb_max[2] == 99.5f && sb.hi_offset == 0x100000000ull && sb.lod_offset == 0x200000000ull && sb.dropped_triangles == 12);
  CHECK(sb.lod_min[1] == 3.0f && sb.hi_max[0] == 4.0f && sb.has_hi() && sb.has_lod());
}

void TestParseSynthetic() {
  // a sector with a road, a decal, a window and a cut-out geometry; the strides differ (20, 28, 36)
  std::vector<SynthGeo> geos = {Quad(0, 0, 10, 10, 0, "CityRoad", "CityRoad_Asphalt", 20, 4),
                                Quad(0, 0, 5, 5, 0.01f, "CityGrimeDecal", "CityGrimeDecal", 28, 1),
                                Quad(0, 0, 2, 2, 3, "CityWindowUpper", "w", 36, 1),
                                Quad(0, 0, 1, 1, 4, "CityNeon", "CityNeonAlpha_Metal", 28, 1)};
  const std::vector<uint8_t> file = MakeSectorFile(geos);
  RscImage image;
  std::string error;
  CHECK(DecodeResourceFile(file, nullptr, &image, &error));
  CHECK(image.type == 1 && image.virtual_size == image.data.size() && image.physical_size == 0);
  std::vector<ShaderNames> shaders;
  ParseStats stats;
  std::vector<SourceGeometry> seen;
  CHECK(ParseSector(image, &shaders, [&](const SourceGeometry& g) { seen.push_back(g); }, &stats, &error));
  CHECK(shaders.size() == 4 && shaders[0].group == "CityRoad" && shaders[0].preset == "CityRoad_Asphalt");
  CHECK(seen.size() == 4 && stats.geometries == 4 && stats.triangles == 8 && stats.vertices == 16 && stats.models == 4);
  CHECK(seen[0].stride == 20 && seen[2].stride == 36 && seen[0].bucket_mask == 4 && seen[1].bucket_mask == 1);
  CHECK(stats.models_without_record == 0 && stats.city_models == 4);
  CHECK(seen[1].shader && seen[1].shader->group == "CityGrimeDecal");
  float p[3];
  SourcePosition(seen[0], 2, p);
  CHECK(p[0] == 10 && p[1] == 0 && p[2] == 10);
  CHECK(LoadBE16(seen[0].indices + 4) == 2);

  // not an RSC5 file, a cut file, a compressed file with no decoder, a stored file shorter than its segments
  std::vector<uint8_t> bad = file;
  bad[0] = 0;
  CHECK(!DecodeResourceFile(bad, nullptr, &image, &error));
  CHECK(!DecodeResourceFile(std::vector<uint8_t>(file.begin(), file.begin() + 100), nullptr, &image, &error));
  std::vector<uint8_t> comp = file;
  comp[8] |= 0x40;  // bit 30
  CHECK(!DecodeResourceFile(comp, nullptr, &image, &error));
  // a damaged pointer: the model array points past the image; the sector parses without it
  RscImage damaged = image;
  damaged.data[16] = 0x5F;  // the drawable pointer past the segment
  std::vector<ShaderNames> s2;
  ParseStats st2;
  CHECK(!ParseSector(damaged, &s2, [](const SourceGeometry&) {}, &st2, &error));

  // the city table
  const std::vector<SynthSector> table = {{"be_smb_01", {-100, -5, -100}, {100, 50, 100}}, {"hw_ss_07", {100, 0, 100}, {300, 60, 300}}};
  RscImage timg;
  CHECK(DecodeResourceFile(MakeCityTable(table), nullptr, &timg, &error));
  std::vector<SectorInfo> sectors;
  CHECK(ParseCityTable(timg, &sectors, &error));
  CHECK(sectors.size() == 2 && sectors[0].name == "be_smb_01" && sectors[1].index == 1);
  CHECK(sectors[0].name_hash == 0xd2849820u && sectors[1].name_hash == 0x51653248u);
  CHECK(sectors[0].aabb_min[0] == -100 && sectors[1].aabb_max[2] == 300);
  CHECK(!ParseCityTable(image, &sectors, &error));  // a sector is not the table

  // the lod: model 1 is null
  SynthGeo lod0 = Quad(-50, -50, 50, 50, 10, "CityDefault", "CityDefault");
  SynthGeo none;
  RscImage limg;
  CHECK(DecodeResourceFile(MakeLodFile({lod0, none}), nullptr, &limg, &error));
  std::vector<ShaderNames> ls;
  ParseStats lstats;
  std::vector<uint32_t> lod_models;
  CHECK(ParseLod(limg, &ls, [&](const SourceGeometry& g) { lod_models.push_back(g.model_index); }, &lstats, &error));
  CHECK(lod_models == std::vector<uint32_t>{0} && lstats.null_models == 1 && lstats.geometries == 1);
}

// ---------------------------------------------------------------------------
// the builder, on a synthetic archive
// ---------------------------------------------------------------------------

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

void TestBuilder(const fs::path& scratch) {
  fs::create_directories(scratch);
  const fs::path rpf = scratch / "xarchive_cache.rpf";
  const std::vector<SynthSector> table = {{"s_a", {0, -5, 0}, {100, 50, 100}}, {"s_b", {100, -5, 0}, {200, 50, 100}}, {"s_missing", {200, -5, 0}, {300, 50, 100}}};
  mc::modloader::Rpf3Writer writer;
  const uint32_t resource_flag = 0x80000000u | FlagFor(2048);
  const auto add = [&](const std::string& path, std::vector<uint8_t> file, uint32_t type) {
    // the archive entry carries the flag word of the file's own header and the payload is the file
    uint32_t flag = 0;
    flag = (uint32_t(file[8]) << 24) | (uint32_t(file[9]) << 16) | (uint32_t(file[10]) << 8) | uint32_t(file[11]);
    writer.Add(path, std::move(file), flag, type & 0xFF);
  };
  (void)resource_flag;
  add(kCityTablePath, MakeCityTable(table), 50);
  // s_a: a road, a decal (dropped), a window, an alpha geometry; the road and window share corners on the x = 10 edge
  add(SectorArchivePath("s_a"),
      MakeSectorFile({Quad(0, 0, 10, 10, 0, "CityRoad", "CityRoad_Asphalt", 20, 4), Quad(0, 0, 5, 5, 0.01f, "CityGrimeDecal", "CityGrimeDecal"),
                      Quad(10, 0, 20, 10, 0, "CityWindowUpper", "w", 36), Quad(0, 0, 1, 1, 4, "CityNeon", "CityNeonAlpha_Metal")}),
      1);
  add(SectorArchivePath("s_b"), MakeSectorFile({Quad(100, 0, 150, 50, 1, "CityBumpSpec", "CityBumpSpec_Cloth"), Quad(100, 0, 110, 10, 1.5f, "CityStoreInterior", "i")}), 1);
  add(kLodPath, MakeLodFile({Quad(0, 0, 100, 100, 20, "CityDefault", "CityDefault"), Quad(100, 0, 200, 100, 20, "CityDefault", "CityDefault"), SynthGeo{}}), 1);
  CHECK(writer.Write(rpf));

  BuildRequest request;
  request.source_rpf = rpf;
  request.out_dir = scratch / "out1";
  request.lzx = nullptr;  // stored resources only
  CountingSink sink;
  BuildResult none = BuildCityPack(request, &sink);
  CHECK(!none.ok);  // no decoder is a refusal, not a crash
  request.lzx = [](const uint8_t*, size_t, std::vector<uint8_t>&, size_t) { return false; };

  request.threads = 1;
  const BuildResult one = BuildCityPack(request, &sink);
  CHECK(one.ok && !one.up_to_date);
  if (!one.ok) std::fprintf(stderr, "build failed: %s\n", one.error.c_str());
  CHECK(one.census.sectors == 3 && one.census.sectors_built == 2 && one.census.sectors_without_file == 1);
  CHECK(one.census.hi.geometries == 6 && one.census.hi.triangles == 12);
  // dropped: the decal (2 triangles) and the interior (2)
  CHECK(one.census.dropped_by_class[size_t(MaterialClass::kDecal)] == 2 && one.census.dropped_by_class[size_t(MaterialClass::kInterior)] == 2);
  CHECK(one.census.hi_kept_triangles == 8);  // s_a: road, window, alpha = 6 (the decal is dropped); s_b: the cloth quad = 2 (the interior is dropped)
  CHECK(one.census.hi_kept_alpha_triangles == 2);
  CHECK(one.census.lod_kept_triangles == 4 && one.census.lod_models_checked == 2 && one.census.lod_models_outside_box == 0);
  CHECK(one.census.vertices_outside_box == 0);
  CHECK(!one.census.Report().empty());

  PackReader reader;
  std::string error;
  CHECK(reader.Open(one.pack_path, &error));
  CHECK(reader.sector_count() == 3 && reader.name(0) == "s_a" && reader.name(2) == "s_missing");
  CHECK(reader.sector(0).has_hi() && reader.sector(0).has_lod() && reader.sector(2).has_lod() == false && !reader.sector(2).has_hi());
  CHECK(reader.sector(0).hi_triangles == 6 && reader.sector(0).hi_opaque_triangles == 4 && reader.sector(0).dropped_triangles == 2);
  CHECK(reader.sector(1).hi_triangles == 2 && reader.sector(1).dropped_triangles == 2 && reader.sector(1).name_hash == RageHashText("s_b"));
  CHECK(reader.sector(0).aabb_max[0] == 100 && reader.sector(1).aabb_min[0] == 100);
  CHECK(reader.header().drop_mask == kDefaultDropMask);
  MeshData mesh;
  CHECK(reader.ReadMesh(0, MeshKind::kHi, &mesh, &error));
  // road (4 vertices) + window quad sharing the x = 10 edge: 4 + 2 = 6 welded, + alpha 4 = 10
  CHECK(mesh.vertex_count() == 10 && mesh.triangle_count() == 6 && mesh.opaque_triangles == 4);
  CHECK(mesh.runs.size() == 3 && mesh.runs[0].material == MaterialClass::kGround && mesh.runs[1].material == MaterialClass::kGlass && mesh.runs[2].material == MaterialClass::kAlphaTested);
  CHECK(!reader.ReadMesh(2, MeshKind::kHi, &mesh, &error) && error.empty());  // no mesh is not an error
  CHECK(reader.ReadMesh(1, MeshKind::kLod, &mesh, &error) && mesh.triangle_count() == 2);
  std::vector<std::string> problems;
  CHECK(ValidatePack(reader, &problems) == 0);

  // up to date now
  CHECK(CheckCityPack(request).current);
  request.threads = 4;
  const BuildResult again = BuildCityPack(request, &sink);
  CHECK(again.ok && again.up_to_date);

  // the same pack with four threads, forced into another folder: byte for byte the same
  BuildRequest four = request;
  four.out_dir = scratch / "out4";
  four.force = true;
  const BuildResult f = BuildCityPack(four, &sink);
  CHECK(f.ok && !f.up_to_date);
  CHECK(ReadAll(one.pack_path) == ReadAll(f.pack_path));

  // the mod archive counts only for the city's own files in it (CityOverlayHash): a car or a song in it leaves the pack current, a sector in it (the same size,
  // other vertices) makes it stale, and a build with it reads the modded sector
  {
    const fs::path mods = scratch / "xarchive_mods.rpf";
    const auto add_mod = [](mc::modloader::Rpf3Writer& w, const std::string& path, std::vector<uint8_t> file) {
      const uint32_t flag = (uint32_t(file[8]) << 24) | (uint32_t(file[9]) << 16) | (uint32_t(file[10]) << 8) | uint32_t(file[11]);
      w.Add(path, std::move(file), flag, 1);
    };
    fs::remove(mods);
    CHECK(CityOverlayHash(mods) == 0 && CityOverlayHash(fs::path()) == 0);  // no mod archive
    BuildRequest with_mods = request;
    with_mods.mods_rpf = mods;
    CHECK(CheckCityPack(with_mods).current);  // no archive: as built

    mc::modloader::Rpf3Writer car_mod;
    add_mod(car_mod, "resources/vehicle/car_a.xdr", MakeSectorFile({Quad(0, 0, 1, 1, 0, "a", "b")}));
    CHECK(car_mod.Write(mods));
    CHECK(CityOverlayHash(mods) == 0);       // not the city
    CHECK(CheckCityPack(with_mods).current);  // a mod archive of another kind: the pack is current, nothing to rebuild

    // s_a with the window quad 5 m longer: the same number of bytes
    const auto modded_sector = [&] {
      return MakeSectorFile({Quad(0, 0, 10, 10, 0, "CityRoad", "CityRoad_Asphalt", 20, 4), Quad(0, 0, 5, 5, 0.01f, "CityGrimeDecal", "CityGrimeDecal"),
                             Quad(10, 0, 25, 10, 0, "CityWindowUpper", "w", 36), Quad(0, 0, 1, 1, 4, "CityNeon", "CityNeonAlpha_Metal")});
    };
    mc::modloader::Rpf3Writer sector_mod;
    add_mod(sector_mod, SectorArchivePath("s_a"), modded_sector());
    CHECK(sector_mod.Write(mods));
    const uint64_t overlay = CityOverlayHash(mods);
    CHECK(overlay != 0 && overlay == CityOverlayHash(mods));  // stable
    const StampCheck stale = CheckCityPack(with_mods);
    CHECK(!stale.current && stale.why.find("mods") != std::string::npos);

    BuildRequest build_mod = with_mods;
    build_mod.out_dir = scratch / "out_mod";
    build_mod.threads = 2;
    std::atomic<int> workers_started{0};
    build_mod.worker_start = [&] { ++workers_started; };
    const BuildResult m = BuildCityPack(build_mod, &sink);
    CHECK(m.ok && !m.up_to_date);
    CHECK(workers_started.load() == 2);  // the hook ran once on each worker
    PackReader mr;
    CHECK(mr.Open(m.pack_path, &error));
    MeshData modded;
    CHECK(mr.ReadMesh(0, MeshKind::kHi, &modded, &error) && modded.bounds_max[0] == 25.0f);  // the modded window, not the shipped one (20)
    CHECK(CheckCityPack(build_mod).current);
    // the mod archive turns into one with no city file in it: the overlay is gone, so the pack is not the stamp's
    fs::remove(mods);
    CHECK(car_mod.Write(mods));
    CHECK(!CheckCityPack(build_mod).current);
    // an unreadable mod archive is never the same as none
    { std::ofstream(mods, std::ios::binary) << "x"; }
    CHECK(CityOverlayHash(mods) != 0 && !CheckCityPack(build_mod).current && !CheckCityPack(with_mods).current);
    fs::remove(mods);
    CHECK(CheckCityPack(request).current);
  }

  // the stamp notices a changed archive, a changed option, a changed builder
  {
    BuildRequest changed = request;
    changed.drop_mask = 0;
    CHECK(!CheckCityPack(changed).current);
    changed = request;
    changed.positions = PositionEncoding::kGrid16;
    CHECK(!CheckCityPack(changed).current);
    BuildRequest with_mods = request;
    with_mods.mods_rpf = scratch / "xarchive_mods.rpf";
    { std::ofstream(with_mods.mods_rpf, std::ios::binary) << "x"; }
    CHECK(!CheckCityPack(with_mods).current);  // a mod archive appeared
    fs::remove(with_mods.mods_rpf);
    CHECK(CheckCityPack(request).current);
    Stamp stamp;
    CHECK(Stamp::Read(request.out_dir / kStampFileName, &stamp, &error));
    stamp.Set("builder_version", uint64_t(kBuilderVersion + 1));
    CHECK(stamp.Write(request.out_dir / kStampFileName, &error));
    const StampCheck c = CheckCityPack(request);
    CHECK(!c.current && c.why.find("builder") != std::string::npos);
  }

  // keeping the decals and interiors: more triangles, another stamp, still a whole pack; the grid encoding is smaller on disk and reads back
  {
    BuildRequest keep = request;
    keep.out_dir = scratch / "out_keep";
    keep.drop_mask = 0;
    keep.positions = PositionEncoding::kGrid16;
    const BuildResult k = BuildCityPack(keep, &sink);
    CHECK(k.ok && k.census.hi_kept_triangles == 12);
    PackReader kr;
    CHECK(kr.Open(k.pack_path, &error));
    CHECK(ValidatePack(kr, nullptr) == 0);
    CHECK(kr.header().position_encoding == 1);
  }

  // census only writes nothing
  {
    BuildRequest census = request;
    census.out_dir = scratch / "out_census";
    census.census_only = true;
    const BuildResult c = BuildCityPack(census, &sink);
    CHECK(c.ok && c.census.hi.triangles == 12 && !fs::exists(census.out_dir / kPackFileName));
  }

  // a cancelled build leaves no pack and no stamp
  {
    BuildRequest cancel = request;
    cancel.out_dir = scratch / "out_cancel";
    cancel.force = true;
    CountingSink stopper;
    stopper.cancel_after = 2;  // the lod step and one sector
    const BuildResult c = BuildCityPack(cancel, &stopper);
    CHECK(!c.ok && c.cancelled);
    CHECK(!fs::exists(cancel.out_dir / kPackFileName) && !fs::exists(cancel.out_dir / kStampFileName));
  }

  // a damaged pack does not open: a flipped byte in the table, a cut file
  {
    std::vector<uint8_t> bytes = ReadAll(one.pack_path);
    std::vector<uint8_t> flipped = bytes;
    flipped[kHeaderBytes + 20] ^= 0x01;
    const fs::path damaged = scratch / "damaged.pack";
    { std::ofstream(damaged, std::ios::binary).write(reinterpret_cast<const char*>(flipped.data()), std::streamsize(flipped.size())); }
    PackReader d;
    CHECK(!d.Open(damaged, &error));
    { std::ofstream(damaged, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size() - 7)); }
    CHECK(!d.Open(damaged, &error));
    CHECK(!d.Open(scratch / "no_such.pack", &error));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "larecomp_citypack_test";
  std::error_code ec;
  fs::remove_all(scratch, ec);
  TestBytes();
  TestHashAndClasses();
  TestIndexCodec();
  TestMeshBuilder();
  TestMeshBlob();
  TestHeaderAndRecord();
  TestParseSynthetic();
  TestBuilder(scratch);
  fs::remove_all(scratch, ec);
  if (g_failures != 0) {
    std::fprintf(stderr, "citypack_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("citypack_test: %d checks passed\n", g_checks);
  return 0;
}
