// Check of the rules of the walk through a car's own model data (car_model_walk_rules.h): the vertex declaration, the vehicle -> entity -> model info -> model
// set -> part table -> drawable -> LOD -> model -> geometry -> buffers chain, the 16-bit and 32-bit index formats, the bone index and the bone palette, the paint
// and the variants, what is "in use", the entries the game's accessor would not hand out, and the comparison with a frame's draws (matched, walked-only,
// drawn-only). The memory is made up: a big-endian byte image laid out the way the survey describes the game's objects. A program of its own: it needs no
// renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_model_walk_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_model_walk_rules_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "car_model_rt_rules.h"
#include "car_model_walk_rules.h"

namespace cw = mcla::gpu::car_model_walk;

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

// A made-up guest: one flat byte image from `kBase`; every read is bounds-checked like the adapter's.
class FakeMemory {
 public:
  static constexpr uint32_t kBase = 0x40000000u;
  explicit FakeMemory(size_t size = 1u << 20) : bytes_(size, 0) {}

  uint32_t Alloc(uint32_t size, uint32_t align = 16) {
    next_ = (next_ + align - 1) / align * align;
    const uint32_t at = kBase + next_;
    next_ += size;
    if (next_ > bytes_.size()) {
      std::printf("FakeMemory: out of room\n");
      std::abort();
    }
    return at;
  }
  uint8_t* At(uint32_t va) { return bytes_.data() + (va - kBase); }
  void P32(uint32_t va, uint32_t v) {
    At(va)[0] = uint8_t(v >> 24);
    At(va)[1] = uint8_t(v >> 16);
    At(va)[2] = uint8_t(v >> 8);
    At(va)[3] = uint8_t(v);
  }
  void P16(uint32_t va, uint32_t v) {
    At(va)[0] = uint8_t(v >> 8);
    At(va)[1] = uint8_t(v);
  }
  void P8(uint32_t va, uint32_t v) { At(va)[0] = uint8_t(v); }
  void PF(uint32_t va, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    P32(va, bits);
  }

  const uint8_t* Window(uint32_t va, uint32_t n) const {
    if (va < kBase || uint64_t(va - kBase) + n > bytes_.size()) {
      return nullptr;
    }
    return bytes_.data() + (va - kBase);
  }
  bool Word(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 4);
    if (!p) {
      return false;
    }
    *out = cw::Be32(p);
    return true;
  }
  bool Half(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 2);
    if (!p) {
      return false;
    }
    *out = cw::Be16(p);
    return true;
  }
  bool Byte(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 1);
    if (!p) {
      return false;
    }
    *out = *p;
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
  uint32_t next_ = 0;
};

// What the made-up car holds, for the checks.
struct Car {
  uint32_t vehicle = 0, entity = 0, info = 0, custom = 0, set = 0;
  uint32_t body_entry = 0, hood_entry = 0, hood_other = 0, hood_lod1 = 0;
  uint32_t rigid_geometry = 0, skinned_geometry = 0, wide_geometry = 0, body_geometry = 0;
  uint32_t palette = 0, hood_item = 0, hood_matrices = 0;
};

// A vertex declaration object: the mask and the 16 four-bit types.
uint32_t MakeDeclaration(FakeMemory& m, uint32_t fvf, uint64_t types) {
  const uint32_t d = m.Alloc(16);
  m.P32(d, fvf);
  m.P32(d + 8, uint32_t(types >> 32));
  m.P32(d + 12, uint32_t(types));
  return d;
}

// A GPU header: 8 dwords, words 0, 6 and 7 filled.
uint32_t MakeHeader(FakeMemory& m, uint32_t w0, uint32_t w6, uint32_t w7) {
  const uint32_t h = m.Alloc(32);
  m.P32(h + 0, w0);
  m.P32(h + 24, w6);
  m.P32(h + 28, w7);
  return h;
}

struct GeometrySpec {
  uint32_t vertices = 4;
  uint32_t stride = 32;
  std::vector<uint32_t> indices = {0, 1, 2, 2, 1, 3};
  bool index32 = false;
  uint32_t index_endian = 1;       // 8in16 for 16-bit big-endian indices; 8in32 (2) for 32-bit
  uint32_t palette_count = 0;      // a skinned geometry
  uint32_t decl_fvf = 0x4079;      // position, normal, colour, texcoord0, tangent: see below
  uint64_t decl_types = 0;
  float x0 = 1.0f;                 // the first vertex's x (the others are derived)
};

// A geometry with a vertex buffer (stride, declaration, data, GPU header), an index buffer and optionally a palette.
uint32_t MakeGeometry(FakeMemory& m, const GeometrySpec& s, Car* car) {
  const uint32_t vdata = m.Alloc(s.vertices * s.stride);
  for (uint32_t v = 0; v < s.vertices; ++v) {
    m.PF(vdata + v * s.stride + 0, s.x0 + float(v));
    m.PF(vdata + v * s.stride + 4, 0.5f * float(v));
    m.PF(vdata + v * s.stride + 8, -2.0f);
  }
  const uint32_t index_bytes = uint32_t(s.indices.size()) * (s.index32 ? 4u : 2u);
  const uint32_t idata = m.Alloc(index_bytes);
  for (uint32_t i = 0; i < s.indices.size(); ++i) {
    if (s.index32) {
      m.P32(idata + 4 * i, s.indices[i]);
    } else {
      m.P16(idata + 2 * i, s.indices[i]);
    }
  }
  const uint32_t decl = MakeDeclaration(m, s.decl_fvf, s.decl_types);
  // the vertex fetch constant: the address with the type 3, the size in bytes in bits 2..25 inside the 0x10000002 frame
  const uint32_t vheader = MakeHeader(m, 0, (vdata & 0x1FFFFFFFu) | 3u, 0x10000002u | ((s.vertices * s.stride) & cw::kFetchSizeMask));
  const uint32_t iheader = MakeHeader(m, (s.index32 ? 0x80000000u : 0u) | (s.index_endian << 29), idata & 0x1FFFFFFFu, index_bytes);
  const uint32_t vb = m.Alloc(32);
  m.P16(vb + 4, s.vertices);
  m.P32(vb + 8, vdata);
  m.P32(vb + 12, s.stride);
  m.P32(vb + 16, decl);
  m.P32(vb + 24, vdata);
  m.P32(vb + 28, vheader);
  const uint32_t ib = m.Alloc(16);
  m.P32(ib + 4, uint32_t(s.indices.size()));
  m.P32(ib + 8, idata);
  m.P32(ib + 12, iheader);
  const uint32_t g = m.Alloc(64);
  m.P32(g + 12, vb);
  m.P32(g + 28, ib);
  m.P32(g + 44, uint32_t(s.indices.size()));
  m.P32(g + 48, uint32_t(s.indices.size() / 3));
  m.P16(g + 52, s.vertices);
  m.P16(g + 54, cw::kPrimitiveTriangleList);
  if (s.palette_count != 0) {
    const uint32_t pal = m.Alloc(2 * s.palette_count);
    for (uint32_t i = 0; i < s.palette_count; ++i) {
      m.P16(pal + 2 * i, 10 + 3 * i);
    }
    m.P32(g + 56, pal);
    m.P16(g + 62, s.palette_count);
    car->palette = pal;
  }
  return g;
}

// A drawable with one LOD object holding `models` models, each with the given geometries; `bones[m]` the model's bone index.
uint32_t MakeDrawable(FakeMemory& m, const std::vector<std::vector<uint32_t>>& models, const std::vector<uint32_t>& bones) {
  const uint32_t model_array = m.Alloc(4 * uint32_t(models.size()));
  const uint32_t bone_array = m.Alloc(4 * uint32_t(models.size()));
  for (uint32_t i = 0; i < models.size(); ++i) {
    const uint32_t geometry_array = m.Alloc(4 * uint32_t(models[i].size()));
    for (uint32_t g = 0; g < models[i].size(); ++g) {
      m.P32(geometry_array + 4 * g, models[i][g]);
    }
    const uint32_t model = m.Alloc(24);
    m.P32(model + 4, geometry_array);
    m.P16(model + 8, uint32_t(models[i].size()));
    m.P32(model_array + 4 * i, model);
    m.P32(bone_array + 4 * i, bones[i]);
  }
  const uint32_t lod = m.Alloc(24);
  m.P32(lod + 0, model_array);
  m.P16(lod + 4, uint32_t(models.size()));
  m.P32(lod + 8, bone_array);
  m.P32(lod + 16, 0x5);
  const uint32_t drawable = m.Alloc(64);
  m.P32(drawable + 8, lod);
  return drawable;
}

// A table entry: +0 the drawable, +8 the flags.
uint32_t MakeEntry(FakeMemory& m, uint32_t drawable, uint32_t flags = cw::kPlacedMask) {
  const uint32_t e = m.Alloc(32);
  m.P32(e + 0, drawable);
  m.P32(e + 8, flags);
  return e;
}

void SetEntry(FakeMemory& m, uint32_t set, uint32_t part, uint32_t variant, uint32_t lod, uint32_t entry, uint32_t state = cw::kStateLoaded, uint32_t count = 1) {
  const uint32_t flat = cw::FlatIndex(part, variant, lod);
  m.P32(set + 4 * (cw::kSetEntries + flat), entry);
  m.P32(set + 4 * (cw::kSetStates + flat), state);
  m.P32(set + 4 * (cw::kSetCounts + 10 * part + variant), count);
}

// The whole vehicle: a body (LOD 1 loaded, LOD 0 not), the hood at variants 3 (in use, lod 0 and 1) and 4 (not worn), a skinned part (the seats), paint.
Car MakeCar(FakeMemory& m, int32_t body_lod = 1, int32_t part_lod = 0, uint32_t hood_variant = 3) {
  Car car;
  car.vehicle = m.Alloc(7300);
  car.entity = m.Alloc(400);
  car.info = m.Alloc(64);
  car.custom = m.Alloc(2300);
  car.set = m.Alloc(cw::kSetBytes + 16);
  m.P32(car.vehicle + 20, car.entity);
  m.P32(car.entity + 80, car.info);
  m.P32(car.entity + 132, car.custom);
  m.P32(car.info + 32, car.set);
  m.P8(car.info + 40, 1);
  m.P8(car.set + cw::kSetHasLod0Byte, 1);
  // the world matrices: row 3 at +48
  m.PF(car.entity + 144 + 48, 10.0f);
  m.PF(car.entity + 144 + 52, 2.0f);
  m.PF(car.entity + 144 + 56, -30.0f);
  m.PF(car.entity + 208 + 48, 10.5f);
  // the vehicle's state
  m.P16(car.vehicle + 116, 1);
  m.P16(car.vehicle + 120, 1);
  m.P32(car.vehicle + 6004, uint32_t(body_lod));
  m.P32(car.vehicle + 6008, uint32_t(part_lod));
  m.P32(car.vehicle + 5996, 0xFFFFFFFFu);
  m.P32(car.vehicle + 6028, 7);
  m.PF(car.vehicle + 6048, 20.0f);
  m.PF(car.vehicle + 6052, 80.0f);
  const uint32_t state_object = m.Alloc(700);
  m.P32(state_object + 656, 3);
  m.P32(car.entity + 128, state_object);
  for (uint32_t w = 0; w < 4; ++w) {
    m.PF(car.vehicle + 1600 + 64 * w + 48, 1.0f * w);
  }
  // the paint of region 0 and 2, the variants
  m.PF(car.custom + 1632 + 0, 0.25f);
  m.PF(car.custom + 1632 + 4, 0.5f);
  m.PF(car.custom + 1632 + 8, 0.75f);
  m.PF(car.custom + 1632 + 12, 1.0f);
  m.PF(car.custom + 1632 + 16, 0.1f);
  m.P32(car.custom + 1632 + 32, 5);
  m.P32(car.custom + 1632 + 36, 2);
  m.P8(car.custom + 1632 + 40, 1);
  m.PF(car.custom + 1632 + 48 * 2 + 0, 0.9f);
  m.P8(car.custom + 2144 + 2, hood_variant);
  m.P8(car.custom + 2144 + 45, 0);
  m.P8(car.custom + 2203, 0);

  // the body: lod 1 loaded, lod 0 an entry that is not placed
  GeometrySpec body_spec;
  body_spec.vertices = 6;
  body_spec.indices = {0, 1, 2, 3, 4, 5, 0, 2, 4};
  car.body_geometry = MakeGeometry(m, body_spec, &car);
  car.body_entry = MakeEntry(m, MakeDrawable(m, {{car.body_geometry}}, {0}));
  m.P32(car.set + 4 * (cw::kSetBody + 1), car.body_entry);
  m.P32(car.set + 4 * (cw::kSetBody + 0), MakeEntry(m, 0, 0));
  m.P32(car.set + 4 * cw::kSetBodyState, cw::kStateLoaded);
  const uint32_t body_item = m.Alloc(64);
  const uint32_t body_matrices = m.Alloc(64 * 4);
  m.P32(body_item + 20, body_matrices);
  m.P32(car.vehicle + cw::kVehicleBodyItem, body_item);
  m.P32(car.vehicle + cw::kVehicleBodyItemGate, 1);

  // the hood (part 2): variant 3 lod 0 (a rigid geometry and a skinned one in two models) and lod 1; variant 4 lod 0 (not worn)
  GeometrySpec rigid;
  rigid.vertices = 4;
  car.rigid_geometry = MakeGeometry(m, rigid, &car);
  GeometrySpec skinned;
  skinned.vertices = 5;
  skinned.stride = 36;
  skinned.indices = {0, 1, 2, 2, 3, 4};
  skinned.palette_count = 5;
  skinned.decl_fvf = 0x405F;
  car.skinned_geometry = MakeGeometry(m, skinned, &car);
  car.hood_entry = MakeEntry(m, MakeDrawable(m, {{car.rigid_geometry}, {car.skinned_geometry}}, {4, 9}));
  SetEntry(m, car.set, 2, 3, 0, car.hood_entry);
  GeometrySpec wide;
  wide.vertices = 3;
  wide.indices = {0, 1, 2};
  wide.index32 = true;
  wide.index_endian = 2;
  car.wide_geometry = MakeGeometry(m, wide, &car);
  car.hood_lod1 = MakeEntry(m, MakeDrawable(m, {{car.wide_geometry}}, {4}));
  SetEntry(m, car.set, 2, 3, 1, car.hood_lod1);
  GeometrySpec other;
  car.hood_other = MakeEntry(m, MakeDrawable(m, {{MakeGeometry(m, other, &car)}}, {4}));
  SetEntry(m, car.set, 2, 4, 0, car.hood_other);
  // the hood's item and matrices
  car.hood_item = m.Alloc(64);
  car.hood_matrices = m.Alloc(64 * 8);
  m.P32(car.hood_item + 20, car.hood_matrices);
  m.PF(car.hood_matrices + 64 * 4 + 48, 10.5f);  // the hood's bone 4 (its lowest): its matrix's translation
  m.PF(car.hood_matrices + 64 * 4 + 52, 2.5f);
  m.PF(car.hood_matrices + 64 * 4 + 56, -29.0f);
  m.P32(car.vehicle + 6960 + 4 * 2, car.hood_item);
  return car;
}

cw::Options MakeOptions() {
  cw::Options o;
  o.tables_read = true;
  for (uint32_t p = 0; p < cw::kPartSlots; ++p) {
    o.draws_part[p] = 1;
    o.draws_part_alt[p] = 0;
    o.region_of_part[p] = 0;
  }
  o.region_of_part[2] = 2;
  // these fixtures were made with the phase 1 rule (every part at +6008); the retail rule has its own checks in TestProduction here and in car_model_rt_rules_test.cpp
  o.all_parts_at_part_lod = true;
  return o;
}

// The default layout of the made-up geometries: position float3 (6), normal Dec3N (10), colour (9), texcoord0 half2 (1), tangent Dec3N (10) = 12+4+4+4+4 = 28
// plus a 4-byte pad in the 32-byte stride is NOT counted by the declaration; the skinned one adds blend weights and indices (9, 9) = 36.
uint64_t Types(std::initializer_list<std::pair<uint32_t, uint32_t>> semantic_types) {
  uint64_t t = 0;
  for (const auto& [s, type] : semantic_types) {
    t |= uint64_t(type) << (4 * s);
  }
  return t;
}

void TestHelpers() {
  Check(cw::FlatIndex(0, 0, 0) == 0 && cw::FlatIndex(0, 0, 2) == 2 && cw::FlatIndex(0, 1, 0) == 3 && cw::FlatIndex(1, 0, 0) == 30, "flat index 3 * (10p + v) + lod");
  Check(cw::FlatIndex(54, 9, 2) == 1649, "the last entry is 1649: the body's words follow");
  Check(cw::kSetBody == cw::FlatIndex(54, 9, 2) + 1, "the body's entries follow the table");
  Check(cw::GameLod(true, 0) == 0 && cw::GameLod(true, 2) == 2, "a car with a LOD 0 asks for the lod as it is");
  Check(cw::GameLod(false, 0) == 1 && cw::GameLod(false, 2) == 2 && cw::GameLod(false, 7) == 3, "a car without a LOD 0 has it clamped to 1..3");
  Check(cw::GameLod(true, -1) == -1 && cw::GameLod(false, -1) == -1, "a negative lod is no lod");
  Check(cw::PhysicalOf(0x40001000u) == 0x1000u && cw::PhysicalOf(0xA0001000u) == 0x1000u, "windowed addresses lose their window");
  Check(cw::PhysicalOf(0xE0001000u) == 0x2000u, "the 0xE0000000 window adds a page, as the guest's conversion does");
  Check(cw::Plausible(0x82000000u) && !cw::Plausible(0x100u) && !cw::Plausible(0), "plausible object addresses");
}

void TestDeclaration() {
  // position, normal, colour, texcoord0, tangent: 12 + 4 + 4 + 4 + 4
  const uint64_t types = Types({{0, 6}, {3, 10}, {4, 9}, {6, 1}, {14, 10}});
  const uint32_t fvf = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 14);
  const cw::Declaration d = cw::DecodeDeclaration(fvf, uint32_t(types >> 32), uint32_t(types));
  Check(d.read && d.complete, "a declaration of known types is complete");
  Check(d.Has(cw::kSemPosition) && d.PositionIsFloat3(), "the position is a float3");
  Check(d.offset[0] == 0 && d.offset[3] == 12 && d.offset[4] == 16 && d.offset[6] == 20 && d.offset[14] == 24, "the elements are packed in semantic order: position at 0");
  Check(d.bytes == 28, "the declaration accounts for 28 bytes");
  Check(d.offset[1] == -1 && !d.Has(cw::kSemBlendWeights), "an absent semantic has no offset");

  // the skinned layout: blend weights and indices after the position
  const uint64_t skin_types = Types({{0, 6}, {1, 9}, {2, 9}, {3, 10}, {4, 9}, {6, 1}, {14, 10}});
  const cw::Declaration s = cw::DecodeDeclaration(0x405Fu, uint32_t(skin_types >> 32), uint32_t(skin_types));
  Check(s.complete && s.bytes == 36 && s.offset[1] == 12 && s.offset[2] == 16 && s.offset[3] == 20, "the skinned layout is 36 bytes with the blend elements after the position");

  // an unknown component type stops the placing, and says so
  const uint64_t odd = Types({{0, 6}, {3, 7}, {6, 1}});
  const cw::Declaration u = cw::DecodeDeclaration((1u << 0) | (1u << 3) | (1u << 6), uint32_t(odd >> 32), uint32_t(odd));
  Check(!u.complete && u.offset[0] == 0 && u.offset[3] == 12 && u.offset[6] == -1, "an unknown type: its own start is known, nothing after it");
  // a position that is not a float3
  const uint64_t half = Types({{0, 1}});
  Check(!cw::DecodeDeclaration(1, 0, uint32_t(half)).PositionIsFloat3(), "a half2 position is not a float3");
}

void TestIndicesAndPositions() {
  // 16-bit big-endian indices under the 8in16 swap
  const uint8_t be16[] = {0, 0, 0, 1, 0, 2, 0, 5};
  uint32_t max_index = 0, bad = 0;
  cw::ScanIndices(be16, 4, false, mcla::gpu::car_dump::kEndian8in16, 6, &max_index, &bad);
  Check(max_index == 5 && bad == 0, "16-bit indices: the largest is 5, none out of range of 6 vertices");
  cw::ScanIndices(be16, 4, false, mcla::gpu::car_dump::kEndian8in16, 5, &max_index, &bad);
  Check(max_index == 5 && bad == 1, "16-bit indices: one index out of range of 5 vertices");
  // 32-bit indices under the 8in32 swap
  const uint8_t be32[] = {0, 0, 0, 7, 0, 0, 1, 0};
  cw::ScanIndices(be32, 2, true, mcla::gpu::car_dump::kEndian8in32, 300, &max_index, &bad);
  Check(max_index == 256 && bad == 0, "32-bit indices: the largest is 256");
  // many indices (more than one chunk)
  std::vector<uint8_t> many(2 * 5000);
  for (uint32_t i = 0; i < 5000; ++i) {
    many[2 * i] = uint8_t(i >> 8);
    many[2 * i + 1] = uint8_t(i);
  }
  cw::ScanIndices(many.data(), 5000, false, mcla::gpu::car_dump::kEndian8in16, 5000, &max_index, &bad);
  Check(max_index == 4999 && bad == 0, "5000 indices read in chunks");
  // a float3 position
  uint8_t vertex[12];
  const float values[3] = {1.5f, -2.0f, 3.25f};
  for (int i = 0; i < 3; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &values[i], 4);
    vertex[4 * i] = uint8_t(bits >> 24);
    vertex[4 * i + 1] = uint8_t(bits >> 16);
    vertex[4 * i + 2] = uint8_t(bits >> 8);
    vertex[4 * i + 3] = uint8_t(bits);
  }
  float out[3];
  Check(cw::ReadFloat3(vertex, out) && out[0] == 1.5f && out[1] == -2.0f && out[2] == 3.25f, "a big-endian float3 position");
  vertex[0] = 0x7F;
  vertex[1] = 0xC0;
  vertex[2] = 0;
  vertex[3] = 0;
  Check(!cw::ReadFloat3(vertex, out), "a NaN position is not finite");
}

// The made-up geometries use the declarations below.
void FixDeclarations(FakeMemory& m, const Car& car) {
  const auto set_decl = [&](uint32_t geometry, uint32_t fvf, uint64_t types) {
    uint32_t vb = 0, decl = 0;
    m.Word(geometry + 12, &vb);
    m.Word(vb + 16, &decl);
    m.P32(decl, fvf);
    m.P32(decl + 8, uint32_t(types >> 32));
    m.P32(decl + 12, uint32_t(types));
  };
  const uint64_t rigid = Types({{0, 6}, {3, 10}, {4, 9}, {6, 1}, {14, 10}});
  const uint32_t rigid_fvf = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 14);
  const uint64_t skin = Types({{0, 6}, {1, 9}, {2, 9}, {3, 10}, {4, 9}, {6, 1}, {14, 10}});
  set_decl(car.body_geometry, rigid_fvf, rigid);
  set_decl(car.rigid_geometry, rigid_fvf, rigid);
  set_decl(car.wide_geometry, rigid_fvf, rigid);
  set_decl(car.skinned_geometry, 0x405Fu, skin);
}

const cw::PartEntry* FindEntry(const cw::CarWalk& w, uint32_t part, uint32_t variant, uint32_t lod) {
  for (const cw::PartEntry& e : w.parts) {
    if (e.part == part && e.variant == variant && e.lod == lod) {
      return &e;
    }
  }
  return nullptr;
}

void TestWalk() {
  FakeMemory m;
  const Car car = MakeCar(m);
  FixDeclarations(m, car);
  cw::CarWalk w;
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(w.ok && w.why.empty(), "the chain is readable");
  Check(w.entity == car.entity && w.info == car.info && w.custom == car.custom && w.model_set == car.set, "vehicle -> entity -> info -> model set, and the customisation");
  Check(w.visible == 1 && w.enabled == 1 && w.body_lod == 1 && w.part_lod == 0 && w.forced_lod == -1 && w.draw_slot == 7, "the vehicle's own state");
  Check(std::fabs(w.lod_distance[0] - 20.0f) < 1e-6f && std::fabs(w.lod_distance[1] - 80.0f) < 1e-6f, "the lod distances");
  Check(w.has_lod0 && w.info_lod0 && w.entity_state == 3 && w.body_state == 3, "the flags: lod 0 present, the entity's state 3, the body loaded");
  Check(w.position_read && w.position[0] == 10.0f && w.position[1] == 2.0f && w.position[2] == -30.0f && w.render_position[0] == 10.5f, "the entity's two world matrices' positions");
  Check(w.wheels_read && w.wheel_position[0][0] == 0.0f && w.wheel_position[3][0] == 3.0f, "the four wheel matrices' positions");

  // the paint and the variants
  Check(w.paint[0].read && w.paint[0].a[0] == 0.25f && w.paint[0].a[3] == 1.0f && std::fabs(w.paint[0].b[0] - 0.1f) < 1e-6f, "paint region 0: colours A and B");
  Check(w.paint[0].type == 5 && w.paint[0].mode == 2 && w.paint[0].use_default == 1 && std::fabs(w.paint[2].a[0] - 0.9f) < 1e-6f, "paint type, gradient mode, default flag, and region 2");
  Check(w.variants_read && w.variant_of_part[2] == 3 && w.table_mode == 0, "the variant of the hood is 3");

  // the table
  Check(w.parts.size() == 2 + 3, "the body's loaded lod 1 and its unplaced lod 0 entry, and three hood entries");
  const cw::PartEntry* body1 = FindEntry(w, cw::kBodyPart, 0, 1);
  const cw::PartEntry* body0 = FindEntry(w, cw::kBodyPart, 0, 0);
  Check(body1 && body1->valid && body1->in_use && body1->placed && body1->loaded, "the body's lod 1 is valid and in use (V+6004 = 1)");
  Check(body0 && !body0->valid && !body0->in_use && !body0->placed, "the body's lod 0 entry is not placed: not valid, not in use");
  const cw::PartEntry* h30 = FindEntry(w, 2, 3, 0);
  const cw::PartEntry* h31 = FindEntry(w, 2, 3, 1);
  const cw::PartEntry* h40 = FindEntry(w, 2, 4, 0);
  Check(h30 && h30->valid && h30->in_use && h30->region == 2 && h30->item == car.hood_item && h30->matrices == car.hood_matrices, "the hood, variant 3 lod 0: in use, region 2, its item and matrix array");
  Check(h31 && h31->valid && !h31->in_use, "the hood, variant 3 lod 1: valid, not in use (the parts' lod, V+6008, is 0)");
  Check(h40 && h40->valid && !h40->in_use, "the hood, variant 4: valid, not worn: not in use");
  Check(h30->matrix_read && h30->matrix_bone == 4 && h30->matrix_position[0] == 10.5f && h30->matrix_position[1] == 2.5f && h30->matrix_position[2] == -29.0f,
        "the hood's matrix source: the matrix array's entry of its lowest bone (4), row 3");
  Check(body1->matrix_read && body1->matrix_bone == 0, "the body's matrix source: bone 0 of the body item's array");
  Check(h30->models == 2 && h30->bucket_mask == 5 && h30->geometries == 2, "the hood has two models and two geometries; the LOD's bucket mask");
  Check(w.entries_present == 5 && w.entries_loaded == 5 && w.entries_valid == 4 && w.entries_unreadable == 0,
        "five entries present and loaded (the body's state word is 3), four valid (the body's lod 0 is not placed)");

  // the geometries
  Check(w.geometries.size() == 1 + 2 + 1 + 1, "five geometries: the body's, the hood's two, the lod 1's, the other variant's");
  Check(w.all.geometries == 5 && w.in_use.geometries == 3, "three of them in use: the body and the hood's two");
  const cw::Geometry* rigid = nullptr;
  const cw::Geometry* skin = nullptr;
  const cw::Geometry* wide = nullptr;
  for (const cw::Geometry& g : w.geometries) {
    rigid = g.address == car.rigid_geometry ? &g : rigid;
    skin = g.address == car.skinned_geometry ? &g : skin;
    wide = g.address == car.wide_geometry ? &g : wide;
  }
  Check(rigid && skin && wide, "the geometries are found by address");
  Check(rigid->part == 2 && rigid->variant == 3 && rigid->lod == 0 && rigid->model == 0 && rigid->slot == 0 && rigid->in_use, "the rigid geometry's place");
  Check(rigid->vertex_count == 4 && rigid->index_count == 6 && rigid->primitive_count == 2 && rigid->draw_mode == cw::kPrimitiveTriangleList, "counts and the triangle list mode");
  Check(rigid->vb_ok && rigid->vb_stride == 32 && rigid->vb_count == 4 && rigid->decl.PositionIsFloat3() && rigid->decl.offset[0] == 0, "vertex buffer: stride 32, the position float3 at offset 0");
  Check(rigid->vb_data != 0 && rigid->vb_data == rigid->vb_data_copy && rigid->HeaderMatchesData(), "the software pointers name the header's address");
  Check((rigid->vb_w6 & 3u) == 3u && (rigid->vb_w7 & cw::kFetchSizeMask) == 4u * 32u, "the vertex fetch constant: type 3 in the address word, the size in bytes in the second");
  Check(rigid->ib_ok && rigid->ib_count == 6 && !rigid->Index32() && rigid->IndexBytes() == 2 && rigid->IndexEndian() == 1, "index buffer: 16-bit indices, the 8in16 endian");
  Check(rigid->ib_w7 == 12, "the index buffer's size word is the byte count of 6 16-bit indices");
  Check(rigid->bone == 4 && rigid->palette_count == 0 && rigid->palette_read == 0, "a rigid geometry: its model's bone index 4, no palette");
  Check(skin->bone == 9 && skin->palette_count == 5 && skin->palette_read == 5 && skin->decl.Has(cw::kSemBlendIndices) && skin->vb_stride == 36, "the skinned geometry: bone 9, five palette entries, blend elements");
  for (uint32_t i = 0; i < 5; ++i) {
    Check(w.palette_entries[skin->palette_first + i] == 10 + 3 * i, "the bone palette's entry " + std::to_string(i));
  }
  Check(wide->Index32() && wide->IndexBytes() == 4 && wide->IndexEndian() == 2 && wide->index_count == 3 && !wide->in_use, "the 32-bit index format and its endian (a geometry of the lod 1 entry)");

  // what the data says of the in-use geometries
  Check(w.validated_in_use == 3 && w.resident_in_use == 3, "the three in-use geometries' data is readable, their indices in range, their positions finite");
  Check(rigid->validated && rigid->vb_readable && rigid->ib_readable && rigid->max_index == 3 && rigid->bad_indices == 0 && rigid->positions_finite, "the rigid geometry's data");
  Check(rigid->first_position[0] == 1.0f && rigid->first_position[2] == -2.0f, "its first vertex's position");
  Check(!wide->validated, "a geometry not in use is not validated");
  Check(w.in_use.triangles == (9 + 6 + 6) / 3 && w.all.triangles == (9 + 6 + 6 + 3 + 6) / 3, "the triangle sums (index count / 3)");
  Check(w.lod_tally[0].geometries == 3 && w.lod_tally[1].geometries == 2, "the geometries by lod slot: lod 0 has the hood's two and the other variant's, lod 1 the body and the hood's lod 1");
  Check(w.skinned == 1, "one skinned geometry");
}

void TestLodRules() {
  // a car without a LOD 0: the byte M+6652 is 0, and the parts' lod 0 is raised to 1
  FakeMemory m;
  Car car = MakeCar(m, 0, 0);
  FixDeclarations(m, car);
  m.P8(car.set + cw::kSetHasLod0Byte, 0);
  cw::CarWalk w;
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  const cw::PartEntry* body1 = FindEntry(w, cw::kBodyPart, 0, 1);
  const cw::PartEntry* h31 = FindEntry(w, 2, 3, 1);
  const cw::PartEntry* h30 = FindEntry(w, 2, 3, 0);
  Check(!w.has_lod0 && body1 && body1->in_use, "no LOD 0: the body's lod 0 is raised to the lod 1 entry");
  Check(h31 && h31->in_use && h30 && !h30->in_use, "no LOD 0: the hood's lod 1 entry is the one in use");
}

void TestEntriesTheGameWouldNotUse() {
  FakeMemory m;
  Car car = MakeCar(m);
  FixDeclarations(m, car);
  cw::CarWalk w;
  // not loaded: the state word is not 3
  m.P32(car.set + 4 * (cw::kSetStates + cw::FlatIndex(2, 3, 0)), 1);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  const cw::PartEntry* h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && !h30->loaded && !h30->valid && h30->in_use && h30->geometries == 0, "state 1: not loaded, not valid, no geometry walked; still the game's choice (in_use)");
  Check(w.in_use.geometries == 1, "only the body's geometry is walked and in use now");
  // the variant's count is 0
  m.P32(car.set + 4 * (cw::kSetStates + cw::FlatIndex(2, 3, 0)), 3);
  m.P32(car.set + 4 * (cw::kSetCounts + 10 * 2 + 3), 0);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && h30->loaded && !h30->valid && h30->geometries == 0, "count 0: the variant does not exist");
  // an entry that is not placed
  m.P32(car.set + 4 * (cw::kSetCounts + 10 * 2 + 3), 1);
  m.P32(car.hood_entry + 8, 0x10000000u);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && !h30->placed && !h30->valid, "flags 0x10000000: half placed is not placed");
  // an entry that points out of memory
  m.P32(car.hood_entry + 8, cw::kPlacedMask);
  m.P32(car.set + 4 * (cw::kSetEntries + cw::FlatIndex(2, 3, 0)), 0x70000000u);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && h30->entry_unreadable && !h30->valid && w.entries_unreadable == 1, "an entry outside memory is unreadable, not a crash");
  // the part's item is null: the game skips the part
  m.P32(car.set + 4 * (cw::kSetEntries + cw::FlatIndex(2, 3, 0)), car.hood_entry);
  m.P32(car.vehicle + 6960 + 4 * 2, 0);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && h30->valid && !h30->in_use, "no item: valid but not in use");
  // the draw table says 0 for the part
  m.P32(car.vehicle + 6960 + 4 * 2, car.hood_item);
  cw::Options options = MakeOptions();
  options.draws_part[2] = 0;
  cw::WalkVehicle(m, car.vehicle, options, &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(h30 && h30->valid && !h30->in_use, "the draw table's 0: not in use");
  // ... but the alternative table (C+2203 != 0) says 1
  options.draws_part_alt[2] = 1;
  m.P8(car.custom + 2203, 1);
  cw::WalkVehicle(m, car.vehicle, options, &w);
  h30 = FindEntry(w, 2, 3, 0);
  Check(w.table_mode == 1 && h30 && h30->in_use, "the second table is chosen by C+2203");
  // the body's state word is not 3
  m.P32(car.set + 4 * cw::kSetBodyState, 1);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  const cw::PartEntry* body1 = FindEntry(w, cw::kBodyPart, 0, 1);
  Check(body1 && !body1->valid && !body1->in_use && w.body_state == 1, "body state 1: not valid");
}

void TestBrokenChains() {
  FakeMemory m;
  Car car = MakeCar(m);
  cw::CarWalk w;
  cw::WalkVehicle(m, 0, MakeOptions(), &w);
  Check(!w.ok && !w.why.empty(), "no vehicle");
  cw::WalkVehicle(m, 0x70000000u, MakeOptions(), &w);
  Check(!w.ok && w.parts.empty(), "a vehicle outside memory");
  m.P32(car.vehicle + 20, 0);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(!w.ok && w.why.find("entity") != std::string::npos, "a null entity says so");
  m.P32(car.vehicle + 20, car.entity);
  m.P32(car.entity + 80, 0);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(!w.ok && w.why.find("model info") != std::string::npos, "a null model info says so");
  m.P32(car.entity + 80, car.info);
  m.P32(car.info + 32, 0x70000000u);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(!w.ok && w.why.find("model set") != std::string::npos, "a model set outside memory says so");
  m.P32(car.info + 32, car.set);
  // no customisation record: the walk goes on, with no paint and no variants
  m.P32(car.entity + 132, 0);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(w.ok && !w.variants_read && !w.paint[0].read, "no customisation record: walked with no paint and the variant 0");
  // a geometry whose buffers are not readable
  m.P32(car.entity + 132, car.custom);
  uint32_t vb = 0;
  m.Word(car.rigid_geometry + 12, &vb);
  m.P32(vb + 28, 0x70000000u);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  bool found = false;
  for (const cw::Geometry& g : w.geometries) {
    if (g.address == car.rigid_geometry) {
      found = !g.vb_ok && g.ib_ok;
    }
  }
  Check(w.ok && found, "a vertex buffer with no readable header: vb_ok false, the rest walked");
}

void TestOutOfRangeAndBounds() {
  FakeMemory m;
  Car car = MakeCar(m);
  FixDeclarations(m, car);
  // an index past the vertex count
  uint32_t ib = 0, data = 0;
  m.Word(car.rigid_geometry + 28, &ib);
  m.Word(ib + 8, &data);
  m.P16(data + 4, 9);
  cw::CarWalk w;
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  const cw::Geometry* rigid = nullptr;
  for (const cw::Geometry& g : w.geometries) {
    rigid = g.address == car.rigid_geometry ? &g : rigid;
  }
  Check(rigid && rigid->validated && rigid->max_index == 9 && rigid->bad_indices == 1, "an index of 9 in a 4-vertex geometry is counted");
  Check(w.validated_in_use == 3 && w.resident_in_use == 2, "that geometry is not resident");
  // a model count beyond the bound is clamped, not followed
  uint32_t drawable = 0, lod = 0;
  m.Word(car.hood_entry, &drawable);
  m.Word(drawable + 8, &lod);
  m.P16(lod + 4, 60000);
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  Check(w.ok && w.geometries.size() <= cw::kMaxGeometries, "a model count of 60000 is bounded");
  // no validation asked for
  cw::Options options = MakeOptions();
  options.validate = false;
  cw::WalkVehicle(m, car.vehicle, options, &w);
  Check(w.validated_in_use == 0 && w.validated_bytes == 0, "validation off: no data is read");
  // a budget that does not cover the first buffer
  options.validate = true;
  options.validate_budget = 4;
  cw::WalkVehicle(m, car.vehicle, options, &w);
  Check(w.validated_in_use == 0, "a budget of 4 bytes validates nothing");
}

void TestSharedDrawable() {
  // two entries (the hood's lod 1 and another variant's lod 0) pointing at the same drawable: its geometries are listed once
  FakeMemory m;
  Car car = MakeCar(m);
  FixDeclarations(m, car);
  uint32_t shared = 0;
  m.Word(car.hood_entry, &shared);  // the hood variant 3 lod 0's drawable
  const uint32_t again = MakeEntry(m, shared);
  SetEntry(m, car.set, 5, 0, 0, again);  // the intercooler's lod 0 shares the hood's drawable
  m.P32(car.vehicle + 6960 + 4 * 5, car.hood_item);
  cw::CarWalk w;
  cw::WalkVehicle(m, car.vehicle, MakeOptions(), &w);
  const cw::PartEntry* a = FindEntry(w, 5, 0, 0);
  const cw::PartEntry* b = FindEntry(w, 2, 3, 0);
  Check(a && b && a->shared_drawable == true && b->shared_drawable == false && a->first_geometry == b->first_geometry && a->geometries == b->geometries, "the second entry shares the first one's geometries");
  Check(w.drawables_shared == 1 && w.geometries.size() == 5, "no geometry is listed twice");
}

// A DrawKey as a draw of the geometry would be captured: the fetch constant is the header's two words, the device's index buffer is the IB's header.
cw::DrawKey DrawOf(const cw::Geometry& g, uint32_t id) {
  cw::DrawKey d;
  d.id = id;
  d.vertex_d0 = g.vb_w6;
  d.vertex_d1 = g.vb_w7;
  d.stride = g.vb_stride;
  d.position_offset = 0;
  d.indexed = true;
  d.index_object = g.ib_header;
  d.index_dword0 = g.ib_w0;
  d.index_address = g.ib_w6;
  d.count = g.index_count;
  return d;
}

const cw::Geometry& GeometryAt(const cw::CarWalk& w, uint32_t address) {
  for (const cw::Geometry& g : w.geometries) {
    if (g.address == address) {
      return g;
    }
  }
  static const cw::Geometry none;
  return none;
}

void TestMatching() {
  FakeMemory m;
  const Car a = MakeCar(m);
  const Car b = MakeCar(m);  // a second car, never drawn (culled)
  FixDeclarations(m, a);
  FixDeclarations(m, b);
  cw::CarWalk wa, wb;
  cw::WalkVehicle(m, a.vehicle, MakeOptions(), &wa);
  cw::WalkVehicle(m, b.vehicle, MakeOptions(), &wb);
  Check(wa.in_use.geometries == 3 && wb.in_use.geometries == 3, "both cars have three geometries in use");

  // the frame draws car A's three in-use geometries (the body twice: a pre-pass and the colour pass) under the stencil ID 4, and the geometry of its lod 1
  // (not in use) under ID 4 too, and something nobody walked under ID 4 and under ID 9
  std::vector<cw::DrawKey> draws;
  draws.push_back(DrawOf(GeometryAt(wa, a.body_geometry), 4));
  draws.push_back(DrawOf(GeometryAt(wa, a.body_geometry), 4));
  draws.push_back(DrawOf(GeometryAt(wa, a.rigid_geometry), 4));
  draws.push_back(DrawOf(GeometryAt(wa, a.skinned_geometry), 4));
  draws.push_back(DrawOf(GeometryAt(wa, a.wide_geometry), 4));
  cw::DrawKey stranger = DrawOf(GeometryAt(wa, a.rigid_geometry), 4);
  stranger.vertex_d0 = 0x0BADF00Du | 3u;
  draws.push_back(stranger);   // ID 4, a window nobody walked: drawn-only for car A
  stranger.id = 9;
  draws.push_back(stranger);   // ID 9, owned by nobody
  cw::DrawKey vertex_only = DrawOf(GeometryAt(wa, a.rigid_geometry), 4);
  vertex_only.index_object = 0x12340000u;
  vertex_only.index_address = 0x00ABCD00u;
  draws.push_back(vertex_only);  // the vertex window of A's rigid geometry with an index buffer that is not its: vertex-only
  cw::DrawKey auto_draw = DrawOf(GeometryAt(wa, a.rigid_geometry), 4);
  auto_draw.indexed = false;
  draws.push_back(auto_draw);    // not indexed: vertex-only too

  const cw::FrameMatch r = cw::MatchFrame({&wa, &wb}, draws);
  Check(r.cars.size() == 2 && r.draws.size() == draws.size() && r.draws_total == 9, "one result per car and per draw");
  const cw::CarMatch& ma = r.cars[0];
  const cw::CarMatch& mb = r.cars[1];
  Check(ma.drawn && ma.geometries == 5 && ma.in_use == 3, "car A is drawn: five geometries walked, three in use");
  Check(ma.matched == 4 && ma.matched_in_use == 3 && ma.matched_not_in_use == 1 && ma.walked_only_in_use == 0, "A: four geometries matched, the three in use among them; the lod-1 one is matched though not in use");
  Check(ma.draws == 5, "A: five draws are its geometries (the body twice)");
  Check(ma.drawn_only == 3 && ma.vertex_only == 2, "A: three draws with its ID are none of its geometries, two of them by the vertex window only");
  Check(r.draws_matched == 5 && r.draws_vertex_only == 2 && r.draws_unowned == 1, "frame: 5 matched, 2 vertex-only, 1 unowned (ID 9)");
  Check(ma.exact_words == 4 && ma.index_object_equal == 4 && ma.index_address_equal == 4 && ma.index_count_equal == 4 && ma.stride_equal == 4 && ma.position_offset_equal == 4,
        "every matched geometry equals its draw exactly (the header words are the fetch constants)");
  Check(ma.ids.size() == 1 && ma.ids[0] == 4, "A's draws carry the ID 4");
  Check(ma.walked_by_lod[0] == 3 && ma.walked_by_lod[1] == 2 && ma.walked_by_lod[2] == 0 && ma.matched_by_lod[0] == 2 && ma.matched_by_lod[1] == 2 && ma.matched_by_lod[2] == 0,
        "the matched geometries by lod slot say which lod the frame's draws are of: lod 0 two of three walked, lod 1 two of two");
  Check(!mb.drawn && mb.draws == 0 && mb.matched == 0 && mb.geometries == 5 && mb.in_use == 3 && mb.walked_only_in_use == 3 && mb.drawn_only == 0,
        "car B is culled: its geometries are all walked-only");
  Check(r.draws[0].hit == cw::DrawHit::kGeometry && r.draws[0].car == 0 && r.draws[5].hit == cw::DrawHit::kNone && r.draws[7].hit == cw::DrawHit::kVertexOnly,
        "per draw: a geometry of car 0; a stranger none; the one with another index buffer vertex-only");
}

void TestMatchingLooseWords() {
  // the draw's fetch constant need not equal the header's words bit for bit: the address and the size in bytes decide, the endian field and the frame bits do not
  FakeMemory m;
  const Car a = MakeCar(m);
  FixDeclarations(m, a);
  cw::CarWalk wa;
  cw::WalkVehicle(m, a.vehicle, MakeOptions(), &wa);
  cw::DrawKey d = DrawOf(GeometryAt(wa, a.rigid_geometry), 3);
  d.vertex_d1 = (d.vertex_d1 & cw::kFetchSizeMask) | 2u;  // the frame bits dropped, the endian 8in32
  d.index_object = 0;                                      // no object recorded: the address decides
  const cw::FrameMatch r = cw::MatchFrame({&wa}, {d});
  Check(r.draws_matched == 1 && r.cars[0].matched == 1 && r.cars[0].exact_words == 0 && r.cars[0].index_object_equal == 0 && r.cars[0].index_address_equal == 1,
        "a draw whose words differ in the frame bits still matches by address and size, and is counted as not exact");
  // the window with the other size is another buffer
  cw::DrawKey e = DrawOf(GeometryAt(wa, a.rigid_geometry), 3);
  e.vertex_d1 = (e.vertex_d1 & ~cw::kFetchSizeMask) | ((4u * 32u + 4u) & cw::kFetchSizeMask);
  const cw::FrameMatch r2 = cw::MatchFrame({&wa}, {e});
  Check(r2.draws_matched == 0 && r2.cars[0].drawn_only == 0 && r2.draws_unowned == 1, "another size is another vertex window");
}

void TestSharedBuffers() {
  // two walked vehicles whose model set points at the same drawable (one model, one copy): both own the vertex window
  FakeMemory m;
  const Car a = MakeCar(m);
  FixDeclarations(m, a);
  const Car b = MakeCar(m);
  FixDeclarations(m, b);
  m.P32(b.set + 4 * (cw::kSetEntries + cw::FlatIndex(2, 3, 0)), a.hood_entry);
  cw::CarWalk wa, wb;
  cw::WalkVehicle(m, a.vehicle, MakeOptions(), &wa);
  cw::WalkVehicle(m, b.vehicle, MakeOptions(), &wb);
  const cw::FrameMatch r = cw::MatchFrame({&wa, &wb}, {DrawOf(GeometryAt(wa, a.rigid_geometry), 2)});
  Check(r.cars[0].shared_buffers >= 1 && r.cars[1].shared_buffers >= 1, "a vertex window both cars own is counted as shared");
  Check(r.cars[0].draws == 1 && r.cars[1].draws == 1, "the draw is credited to both owners");
  // RT-MODEL-CARS-3: with the owner of each draw (the stencil ID and the origin tell the twins apart) only that car is drawn: the first check run's 88.7 percent car was a twin 56 m
  // away that the frame did not draw, credited with its neighbour's draws
  const cw::DrawKey d = DrawOf(GeometryAt(wa, a.rigid_geometry), 2);
  std::vector<int32_t> owner_b = {1};
  const cw::FrameMatch rb = cw::MatchFrame({&wa, &wb}, {d}, &owner_b);
  Check(!rb.cars[0].drawn && rb.cars[0].draws == 0 && rb.cars[0].matched == 0 && rb.cars[0].walked_only_in_use == rb.cars[0].in_use && rb.cars[0].ids.empty(),
        "owned by car B: car A (the twin) is not drawn: nothing matched, every in-use geometry walked-only");
  Check(rb.cars[1].drawn && rb.cars[1].draws == 1 && rb.cars[1].matched == 1 && rb.draws_matched == 1 && rb.draws[0].hit == cw::DrawHit::kGeometry && rb.draws[0].car == 1u,
        "... and car B is drawn with the draw");
  Check(rb.cars[0].shared_buffers >= 1 && rb.cars[1].shared_buffers >= 1, "the buffers are still counted as shared");
  std::vector<int32_t> nobody = {-1};
  const cw::FrameMatch rn = cw::MatchFrame({&wa, &wb}, {d}, &nobody);
  Check(!rn.cars[0].drawn && !rn.cars[1].drawn && rn.draws_matched == 0 && rn.draws_vertex_only == 1 && rn.draws[0].hit == cw::DrawHit::kVertexOnly,
        "a draw with no owner (no car within reach of its IDs' origin) is vertex-only and credits no car");
  std::vector<int32_t> short_list;  // fewer owners than draws: the draw is credited as without a list
  const cw::FrameMatch rs = cw::MatchFrame({&wa, &wb}, {d}, &short_list);
  Check(rs.cars[0].draws == 1 && rs.cars[1].draws == 1, "an owner list shorter than the draws leaves the rest credited to every owner");
}

}  // namespace


// ---------------------------------------------------------------------------
// the production walk (RT-MODEL-CARS phase 2): one entry a part, the lod of the reflection, the part matrices, the chain down to a mesh
// ---------------------------------------------------------------------------

void TestProduction() {
  namespace crt = mcla::gpu::car_model_rt;
  // The car: V+6004 = 1 (the body and the parts under 44), V+6008 = 0 (the parts from 44), the hood (part 2) at variant 3 with lods 0 and 1, the body at lod 1.
  FakeMemory m;
  Car car = MakeCar(m, 1, 0);
  FixDeclarations(m, car);
  // the arrays the draw sets gWorld from: the hood's and the body's
  const uint32_t hood_worlds = m.Alloc(64 * 8);
  const uint32_t body_worlds = m.Alloc(64 * 4);
  m.P32(car.vehicle + 5740 + 4 * 2, hood_worlds);
  m.P32(car.vehicle + 5736, body_worlds);
  cw::Options options = MakeOptions();
  options.all_parts_at_part_lod = false;  // the retail rule
  options.production = true;
  options.validate = false;
  options.min_lod = 1;
  cw::CarWalk w;
  cw::WalkVehicle(m, car.vehicle, options, &w);
  Check(w.ok && w.parts.size() == 2, "production: one entry for the body and one for the hood, not every variant and lod");
  const cw::PartEntry* body = FindEntry(w, cw::kBodyPart, 0, 1);
  const cw::PartEntry* hood = FindEntry(w, 2, 3, 1);
  Check(body && body->in_use && body->valid && hood && hood->in_use && hood->valid, "the body at lod 1 and the hood's variant 3 at lod 1 (the parts under 44 follow V+6004, not V+6008)");
  Check(hood && hood->world_matrices == hood_worlds && body && body->world_matrices == body_worlds && hood->matrices == car.hood_matrices,
        "each entry names its two matrix arrays: the item's (+20) and the one the draw sets gWorld from (V+5740+4p, the body's V+5736)");
  Check(w.geometries.size() == 2 && w.in_use.geometries == 2, "two geometries in all: the body's and the hood's lod 1");
  bool entry_ok = true;
  for (const cw::Geometry& g : w.geometries) {
    entry_ok = entry_ok && g.entry_index < w.parts.size() && w.parts[g.entry_index].part == g.part;
  }
  Check(entry_ok, "a geometry names the entry whose matrices place it");
  Check(w.validated_in_use == 0 && !w.geometries[0].validated, "production does not validate the data");

  // the lod floor: a car the main view shows is walked at the game's own lod (the parts under 44 follow the body's: 1 here, so the same entries)
  options.min_lod = 0;
  cw::WalkVehicle(m, car.vehicle, options, &w);
  Check(w.parts.size() == 2 && FindEntry(w, 2, 3, 1) != nullptr, "floor 0: the game's lod for the parts is the vehicle's 1, not +6008's 0");
  // with the body at lod 0 (not loaded) and the parts' floor 0: the hood's lod 0
  FakeMemory m2;
  Car car2 = MakeCar(m2, 0, 0);
  FixDeclarations(m2, car2);
  options.min_lod = 0;
  cw::WalkVehicle(m2, car2.vehicle, options, &w);
  Check(FindEntry(w, 2, 3, 0) && FindEntry(w, 2, 3, 0)->in_use && FindEntry(w, 2, 3, 1) == nullptr, "V+6004 = 0, floor 0: the hood's lod 0 is the one, and only it");
  Check(FindEntry(w, cw::kBodyPart, 0, 1) && FindEntry(w, cw::kBodyPart, 0, 1)->in_use, "the body's lod 0 is not placed: the next coarser lod that is placed takes its place");
  options.min_lod = 1;
  cw::WalkVehicle(m2, car2.vehicle, options, &w);
  Check(FindEntry(w, 2, 3, 1) && FindEntry(w, 2, 3, 1)->in_use && FindEntry(w, 2, 3, 0) == nullptr, "floor 1 raises the reflection's lod to 1 whatever the game's is");
  // a part with no entry at the wanted lod and the coarser ones takes the finer: the hood's lod 1 not loaded
  FakeMemory m3;
  Car car3 = MakeCar(m3, 1, 0);
  FixDeclarations(m3, car3);
  m3.P32(car3.set + 4 * (cw::kSetStates + cw::FlatIndex(2, 3, 1)), 0);
  cw::WalkVehicle(m3, car3.vehicle, options, &w);
  Check(FindEntry(w, 2, 3, 0) && FindEntry(w, 2, 3, 0)->in_use && FindEntry(w, 2, 3, 1) == nullptr, "the hood's lod 1 is not loaded: the finer lod 0 is taken (coarser first, then finer)");
  // a part with no lod loaded: nothing of it
  m3.P32(car3.set + 4 * (cw::kSetStates + cw::FlatIndex(2, 3, 0)), 0);
  cw::WalkVehicle(m3, car3.vehicle, options, &w);
  Check(FindEntry(w, 2, 3, 0) == nullptr && FindEntry(w, 2, 3, 1) == nullptr && w.parts.size() == 1, "no lod of the hood loaded: only the body is walked");

  // the draw's loop: the byte that cuts it to 44 parts takes a part from 44 up out of the walk
  FakeMemory m4;
  Car car4 = MakeCar(m4, 1, 0);
  FixDeclarations(m4, car4);
  GeometrySpec interior;
  interior.vertices = 4;
  const uint32_t interior_geometry = MakeGeometry(m4, interior, &car4);
  const uint32_t interior_entry = MakeEntry(m4, MakeDrawable(m4, {{interior_geometry}}, {0}));
  SetEntry(m4, car4.set, 44, 0, 1, interior_entry);
  const uint32_t interior_item = m4.Alloc(64);
  m4.P32(interior_item + 20, m4.Alloc(64));
  m4.P32(car4.vehicle + 6960 + 4 * 44, interior_item);
  options.min_lod = 1;
  cw::WalkVehicle(m4, car4.vehicle, options, &w);
  Check(FindEntry(w, 44, 0, 1) && FindEntry(w, 44, 0, 1)->in_use && w.loop_parts == cw::kPartSlots, "the interior (part 44) is drawn while the loop covers all 55 parts");
  m4.P8(car4.vehicle + 6478, 1);
  cw::WalkVehicle(m4, car4.vehicle, options, &w);
  Check(w.flags.loop44_b && w.loop_parts == 44 && FindEntry(w, 44, 0, 1) == nullptr, "V+6478 set: the loop covers 44 parts, the interior is not walked");
  cw::Options debug = MakeOptions();
  debug.all_parts_at_part_lod = false;
  cw::WalkVehicle(m4, car4.vehicle, debug, &w);
  const cw::PartEntry* debug_interior = FindEntry(w, 44, 0, 1);
  Check(debug_interior && !debug_interior->in_use, "the debug walk lists the interior's entry but it is not in use while the loop is cut to 44 (the 73 percent car's missing geometries)");

  // THE CHAIN: walk -> picks -> the matrices of the bones -> the poses -> the mesh. The hood's rigid geometry is in model 0 of its lod-1 drawable (bone 4).
  FakeMemory mc;
  Car carc = MakeCar(mc, 1, 0);
  FixDeclarations(mc, carc);
  const uint32_t worlds = mc.Alloc(64 * 8);
  mc.P32(carc.vehicle + 5740 + 4 * 2, worlds);
  const auto put = [&](uint32_t at, double yaw, double x, double y, double z) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 3; ++c) {
        double v = r == c ? 1.0 : 0.0;
        if (r == 3) {
          v = c == 0 ? x : c == 1 ? y : z;
        }
        if (r == 0 && c == 0) {
          v = std::cos(yaw);
        } else if (r == 0 && c == 2) {
          v = -std::sin(yaw);
        } else if (r == 2 && c == 0) {
          v = std::sin(yaw);
        } else if (r == 2 && c == 2) {
          v = std::cos(yaw);
        }
        mc.PF(at + 16u * uint32_t(r) + 4u * uint32_t(c), float(v));
      }
    }
  };
  put(carc.hood_matrices, 0.3, 11.0, 3.0, -29.0);              // A[0]: the first bone
  put(carc.hood_matrices + 64 * 4, 0.3, 11.0, 3.5, -28.5);     // A[4]: the hood's bone (the fixture's test reads its translation too)
  put(worlds + 64 * 4, 0.3, 11.0, 3.5, -28.5);                 // B[4]
  cw::CarWalk wc;
  cw::WalkVehicle(mc, carc.vehicle, options, &wc);
  const std::vector<uint32_t> picks = crt::PickGeometries(wc, 3, 0);
  Check(picks.size() == 2, "both geometries are picked (3 indices or more, rigid, in use)");
  uint32_t hood_geometry = 0;
  for (const uint32_t g : picks) {
    if (wc.geometries[g].part == 2) {
      hood_geometry = g;
    }
  }
  const cw::Geometry& hg = wc.geometries[hood_geometry];
  Check(hg.part == 2 && hg.bone == 4 && hg.index_count == 3, "the hood's lod 1 geometry: bone 4, three indices");
  const crt::BoneMatrices bm = crt::ReadBoneMatrices(mc, wc.parts[hg.entry_index], hg.bone);
  const mcla::gpu::temporal::Mat4 entity = wc.entity_matrix;
  const crt::PoseSet poses = crt::ComposePoses(bm, wc.has_entity_matrix ? &entity : nullptr);
  Check(bm.item && bm.root && bm.world && poses.ok[0] && poses.ok[1] && poses.ok[2], "the bone's matrices are read through the entry");
  Check(std::fabs(poses.m[size_t(crt::Pose::kBoneWorld)].m[3][1] - 3.5) < 1e-6 && std::fabs(poses.m[size_t(crt::Pose::kPaletteBone)].m[3][1] - 3.5 - 0.5) < 1e-5,
        "pose 0 is B[4]; pose 2 is the palette (A[4] minus A[0]'s translation: 0.5 up) times B[4]");
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  crt::MeshRead info;
  Check(crt::ReadGeometryMesh(mc, hg, &positions, &indices, &info) == crt::MeshProblem::kNone && indices == std::vector<uint32_t>({0, 1, 2}) && positions.size() == 9,
        "the hood geometry's mesh: one triangle of three vertices (32-bit indices, the header's endian)");
  Check(crt::GeometryKey(hg) != 0 && crt::GeometryKey(hg) != crt::GeometryKey(wc.geometries[picks[0] == hood_geometry ? picks[1] : picks[0]]), "the two geometries have keys of their own");
}

int main() {
  TestHelpers();
  TestDeclaration();
  TestIndicesAndPositions();
  TestWalk();
  TestLodRules();
  TestEntriesTheGameWouldNotUse();
  TestBrokenChains();
  TestOutOfRangeAndBounds();
  TestSharedDrawable();
  TestMatching();
  TestMatchingLooseWords();
  TestSharedBuffers();
  TestProduction();
  std::printf("car_model_walk_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
