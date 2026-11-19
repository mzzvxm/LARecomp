#pragma once
// A made-up guest for the checks of the wheels and the traffic (car_model_wheel_rules_test.cpp, car_model_traffic_rules_test.cpp): a big-endian byte image, bounds-checked like the
// walker's adapter, with room for the game's fixed globals (the wheel manager's pointer, the renderer's, the render buffer) at their real addresses. Test code only.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "car_model_walk_rules.h"

namespace parts_test {

namespace cw = mcla::gpu::car_model_walk;

inline int g_checks = 0;
inline int g_failed = 0;

inline void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

inline bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

class FakeMemory {
 public:
  static constexpr uint32_t kBase = 0x40000000u;
  explicit FakeMemory(size_t size = 1u << 21) : bytes_(size, 0) {
    // the game's fixed globals live at their real addresses: one page each
    globals_.push_back({0x8288E000u, std::vector<uint8_t>(0x1000, 0)});  // the wheel manager's pointer 0x8288E010
    globals_.push_back({0x8287E000u, std::vector<uint8_t>(0x1000, 0)});  // the renderer's pointer 0x8287E064
    globals_.push_back({0x827E5000u, std::vector<uint8_t>(0x1000, 0)});  // the render buffer 0x827E5640
  }

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
  uint8_t* At(uint32_t va) {
    if (va >= kBase && uint64_t(va - kBase) < bytes_.size()) {
      return bytes_.data() + (va - kBase);
    }
    for (Region& r : globals_) {
      if (va >= r.base && va - r.base < r.bytes.size()) {
        return r.bytes.data() + (va - r.base);
      }
    }
    std::printf("FakeMemory: write outside the image at %08x\n", va);
    std::abort();
  }
  void P32(uint32_t va, uint32_t v) {
    uint8_t* p = At(va);
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
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
    if (va >= kBase && uint64_t(va - kBase) + n <= bytes_.size()) {
      return bytes_.data() + (va - kBase);
    }
    for (const Region& r : globals_) {
      if (va >= r.base && uint64_t(va - r.base) + n <= r.bytes.size()) {
        return r.bytes.data() + (va - r.base);
      }
    }
    return nullptr;
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
  struct Region {
    uint32_t base;
    std::vector<uint8_t> bytes;
  };
  std::vector<uint8_t> bytes_;
  std::vector<Region> globals_;
  uint32_t next_ = 0;
};

// A vertex declaration object: the mask and the 16 four-bit types.
inline uint32_t MakeDeclaration(FakeMemory& m, uint32_t fvf, uint64_t types) {
  const uint32_t d = m.Alloc(16);
  m.P32(d, fvf);
  m.P32(d + 8, uint32_t(types >> 32));
  m.P32(d + 12, uint32_t(types));
  return d;
}

inline uint32_t MakeHeader(FakeMemory& m, uint32_t w0, uint32_t w6, uint32_t w7) {
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
  uint32_t decl_fvf = 0x4079;
  uint64_t decl_types = 0x0000000000000006ull;  // position: Float3 (component type 6) at semantic 0
  float x0 = 1.0f;
};

// A geometry with a vertex buffer (position float3 at offset 0), an index buffer (16 bit, 8in16 swapped) and their GPU headers.
inline uint32_t MakeGeometry(FakeMemory& m, const GeometrySpec& s) {
  const uint32_t vdata = m.Alloc(s.vertices * s.stride);
  for (uint32_t v = 0; v < s.vertices; ++v) {
    m.PF(vdata + v * s.stride + 0, s.x0 + float(v));
    m.PF(vdata + v * s.stride + 4, 0.5f * float(v));
    m.PF(vdata + v * s.stride + 8, -2.0f);
  }
  const uint32_t index_bytes = uint32_t(s.indices.size()) * 2u;
  const uint32_t idata = m.Alloc(index_bytes);
  for (uint32_t i = 0; i < s.indices.size(); ++i) {
    m.P16(idata + 2 * i, s.indices[i]);
  }
  const uint32_t decl = MakeDeclaration(m, s.decl_fvf, s.decl_types);
  const uint32_t vheader = MakeHeader(m, 0, (vdata & 0x1FFFFFFFu) | 3u, 0x10000002u | ((s.vertices * s.stride) & cw::kFetchSizeMask));
  const uint32_t iheader = MakeHeader(m, (1u << 29), idata & 0x1FFFFFFFu, index_bytes);
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
  return g;
}

// A drawable with one LOD object holding `models` models, each with the given geometries; `bones[m]` the model's bone index.
inline uint32_t MakeDrawable(FakeMemory& m, const std::vector<std::vector<uint32_t>>& models, const std::vector<uint32_t>& bones) {
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

// A table entry / holder: +0 the drawable, +8 the flags.
inline uint32_t MakeEntry(FakeMemory& m, uint32_t drawable, uint32_t flags = cw::kPlacedMask) {
  const uint32_t e = m.Alloc(32);
  m.P32(e + 0, drawable);
  m.P32(e + 8, flags);
  return e;
}

// A Matrix44 at `va`: the 3x3 given (row major, rows right, up, back) and the translation row.
inline void PutMatrix(FakeMemory& m, uint32_t va, const double rot[3][3], const double t[3]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m.PF(va + 16u * uint32_t(r) + 4u * uint32_t(c), float(rot[r][c]));
    }
    m.PF(va + 16u * uint32_t(r) + 12u, 0.0f);
  }
  for (int c = 0; c < 3; ++c) {
    m.PF(va + 48u + 4u * uint32_t(c), float(t[c]));
  }
  m.PF(va + 60u, 1.0f);
}

inline void PutIdentity(FakeMemory& m, uint32_t va, double x, double y, double z) {
  const double rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const double t[3] = {x, y, z};
  PutMatrix(m, va, rot, t);
}

}  // namespace parts_test
