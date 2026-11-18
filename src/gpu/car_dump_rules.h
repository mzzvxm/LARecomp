#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car dump prototype: the rules that need no renderer
// ===========================================================================
// perf/CAR-RT-SPIKE.md section 7. The prototype (car_dump.h) takes the triangles of the cars the game draws in one frame from guest
// memory and checks them against the car self-reflection history image. This header holds what needs no renderer, as pure
// functions on plain numbers, so that car_dump_rules_test.cpp builds on its own:
//
//   - the matrix of a draw out of its vertex shader bank (raw big-endian registers): the world matrix gWorld c0-c3, the world
//     to clip matrix gWorldViewProj c8-c11, and the bone the car vertex shaders pick out of the palette gCarMtxBuffer
//     (c64-c255, three registers a bone, the bone chosen per draw by gBoneIndex.x = c204.x):
//         local = [position 1] * palette(bone)     world = [local 1] * gWorld     clip = [local 1] * gWorldViewProj
//   - the position of a vertex out of the bytes the guest holds (the formats the game's declarations give a position) and
//     the index bytes under their endian swap
//   - the index list of a draw as a clean list of triangles (no degenerate ones, none that name a vertex the stream does not hold)
//   - a tiny CPU rasteriser with a nearest-distance buffer, and the statistics that compare it with an image of distances
//
// Matrices are row vectors (v' = v M) as in temporal_math.h: a product A * B applies A first. The bank's registers are the
// rows of gWorld and gWorldViewProj; the bone's three registers are the ROWS of a 3x4 matrix applied to a column vector (the
// shader dots each with the position), which is transposed into the row-vector form here.
//
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "temporal_math.h"

namespace mcla::gpu::car_dump {

// ---------------------------------------------------------------------------
// the registers of a draw
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRegisterCount = 256;
inline constexpr uint32_t kWorldRegister = 0;         // gWorld, c0-c3
inline constexpr uint32_t kWvpRegister = 8;           // gWorldViewProj, c8-c11
inline constexpr uint32_t kBoneIndexRegister = 204;   // gBoneIndex, x = the palette's bone
inline constexpr uint32_t kPaletteRegister = 64;      // gCarMtxBuffer, c64-c255
inline constexpr uint32_t kBoneRegisters = 3;         // a bone's rows
inline constexpr uint32_t kPaletteBones = (kRegisterCount - kPaletteRegister) / kBoneRegisters;  // 64
// How far gWorld times the camera's view-projection may be from the draw's gWorldViewProj, relative to the latter's largest entry.
inline constexpr double kWvpTolerance = 1e-3;

// Component `comp` (0 x, 1 y, 2 z, 3 w) of register `reg` of a bank of raw big-endian bytes (ConstantBanks::Banks).
inline float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  const uint8_t* p = bank + size_t(reg) * 16 + size_t(comp) * 4;
  const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Four registers as the rows of a matrix.
inline temporal::Mat4 BankMatrix(const uint8_t* bank, uint32_t first) {
  temporal::Mat4 m;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      m.m[r][c] = double(BankFloat(bank, first + r, c));
    }
  }
  return m;
}

// What one draw's bank says of where its model space is.
struct CarMatrices {
  bool finite = false;       // every number read is finite
  bool bone_valid = false;   // c204.x names a bone of the palette (0 to 63)
  uint32_t bone = 0;         // trunc(c204.x) when it does
  float bone_raw = 0.0f;     // c204.x as it is
  temporal::Mat4 world;      // gWorld, c0-c3
  temporal::Mat4 wvp;        // gWorldViewProj, c8-c11
  temporal::Mat4 palette;    // the bone's three rows as a row-vector matrix: local = [p 1] * palette (the identity without a bone)
  temporal::Mat4 model;      // palette * world: model space to world (recipe A); without a bone, gWorld alone
};

// The row-vector matrix of the palette's bone `bone`: local.x = dot(row0.xyz, p) + row0.w, and so on for y and z.
inline temporal::Mat4 BoneMatrix(const uint8_t* bank, uint32_t bone) {
  temporal::Mat4 m = temporal::Identity();
  const uint32_t first = kPaletteRegister + bone * kBoneRegisters;
  for (uint32_t row = 0; row < kBoneRegisters; ++row) {
    for (uint32_t r = 0; r < 3; ++r) {
      m.m[r][row] = double(BankFloat(bank, first + row, r));
    }
    m.m[3][row] = double(BankFloat(bank, first + row, 3));
  }
  return m;
}

inline CarMatrices DecodeCarMatrices(const uint8_t* vs_bank) {
  CarMatrices out;
  out.world = BankMatrix(vs_bank, kWorldRegister);
  out.wvp = BankMatrix(vs_bank, kWvpRegister);
  out.bone_raw = BankFloat(vs_bank, kBoneIndexRegister, 0);
  out.palette = temporal::Identity();
  const float raw = out.bone_raw;
  if (std::isfinite(raw) && raw >= 0.0f && raw < float(kPaletteBones)) {
    out.bone = uint32_t(raw);  // trunc, as the shader's
    out.bone_valid = true;
    out.palette = BoneMatrix(vs_bank, out.bone);
  }
  out.model = temporal::Mul(out.palette, out.world);
  out.finite = temporal::Finite(out.world) && temporal::Finite(out.wvp) && temporal::Finite(out.palette);
  return out;
}

// [p 1] * m, all four components of the result.
inline void TransformPoint(const temporal::Mat4& m, const double p[3], double out[4]) {
  for (int c = 0; c < 4; ++c) {
    out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
  }
}

// The largest entry of (world * view_projection - wvp) over the largest entry of wvp: 0 when the draw's gWorld and gWorldViewProj
// are the same transform seen through this camera (gWorld is the matrix the shader's own world position goes through).
inline double WvpError(const temporal::Mat4& world, const temporal::Mat4& view_projection, const temporal::Mat4& wvp) {
  const double scale = std::max(temporal::MaxAbs(wvp), 1e-30);
  return temporal::MaxAbsDiff(temporal::Mul(world, view_projection), wvp) / scale;
}

// ---------------------------------------------------------------------------
// vertices and indices as the guest holds them
// ---------------------------------------------------------------------------

// The formats the game's declarations give a POSITION (native_gfx/vertex_declaration.h, gpu/format.cpp VertexFormat), by their
// packed Xenon decltype. Floats are big-endian 32-bit words, half floats big-endian 16-bit words in the order x y z w.
enum class PositionFormat : uint8_t { kNone, kFloat2, kFloat3, kFloat4, kHalf2, kHalf4 };

inline PositionFormat PositionFormatOf(uint32_t packed_decltype) {
  switch (packed_decltype) {
    case 0x002A23B9: return PositionFormat::kFloat3;  // k_32_32_32_FLOAT
    case 0x001A23A6: return PositionFormat::kFloat4;  // k_32_32_32_32_FLOAT
    case 0x001A2360: return PositionFormat::kHalf4;   // k_16_16_16_16_FLOAT
    case 0x002C23A5: return PositionFormat::kFloat2;  // k_32_32_FLOAT
    case 0x002C235F: return PositionFormat::kHalf2;   // k_16_16_FLOAT
    default: return PositionFormat::kNone;
  }
}

inline uint32_t PositionBytes(PositionFormat format) {
  switch (format) {
    case PositionFormat::kFloat2: return 8;
    case PositionFormat::kFloat3: return 12;
    case PositionFormat::kFloat4: return 16;
    case PositionFormat::kHalf2: return 4;
    case PositionFormat::kHalf4: return 8;
    default: return 0;
  }
}

inline const char* PositionFormatName(PositionFormat format) {
  switch (format) {
    case PositionFormat::kFloat2: return "float2";
    case PositionFormat::kFloat3: return "float3";
    case PositionFormat::kFloat4: return "float4";
    case PositionFormat::kHalf2: return "half2";
    case PositionFormat::kHalf4: return "half4";
    default: return "none";
  }
}

inline float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // A subnormal half: normalise it.
      uint32_t e = 113;
      uint32_t m = mantissa;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --e;
      }
      bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

inline uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t LoadBe16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

// The position at `be` (the guest's raw bytes of one vertex's element) as three floats; a two-component format has z = 0.
inline void DecodePosition(const uint8_t* be, PositionFormat format, float out[3]) {
  const auto f32 = [&](uint32_t i) {
    const uint32_t bits = LoadBe32(be + 4 * i);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  const auto f16 = [&](uint32_t i) { return HalfToFloat(LoadBe16(be + 2 * i)); };
  out[0] = out[1] = out[2] = 0.0f;
  switch (format) {
    case PositionFormat::kFloat2:
      out[0] = f32(0);
      out[1] = f32(1);
      break;
    case PositionFormat::kFloat3:
    case PositionFormat::kFloat4:
      out[0] = f32(0);
      out[1] = f32(1);
      out[2] = f32(2);
      break;
    case PositionFormat::kHalf2:
      out[0] = f16(0);
      out[1] = f16(1);
      break;
    case PositionFormat::kHalf4:
      out[0] = f16(0);
      out[1] = f16(1);
      out[2] = f16(2);
      break;
    default:
      break;
  }
}

// xenos::Endian: the swap of raw index bytes (geometry.cpp ApplyEndian). `length` is a multiple of the unit.
inline constexpr uint32_t kEndianNone = 0;
inline constexpr uint32_t kEndian8in16 = 1;
inline constexpr uint32_t kEndian8in32 = 2;
inline constexpr uint32_t kEndian16in32 = 3;

inline void SwapEndian(uint8_t* dst, const uint8_t* src, uint32_t length, uint32_t endian) {
  switch (endian) {
    case kEndian8in16:
      for (uint32_t i = 0; i + 2 <= length; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
      }
      break;
    case kEndian8in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
      }
      break;
    case kEndian16in32:
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 3];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 1];
      }
      break;
    default:
      std::memmove(dst, src, length);
      break;
  }
}

// `count` guest indices (16 or 32 bit) at `raw`, swapped by `endian`, as host numbers.
inline void DecodeIndices(const uint8_t* raw, uint32_t count, bool index32, uint32_t endian, std::vector<uint32_t>* out) {
  const uint32_t size = index32 ? 4u : 2u;
  std::vector<uint8_t> swapped(size_t(count) * size);
  SwapEndian(swapped.data(), raw, count * size, endian);
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (index32) {
      uint32_t v;
      std::memcpy(&v, swapped.data() + size_t(i) * 4, 4);
      (*out)[i] = v;
    } else {
      uint16_t v;
      std::memcpy(&v, swapped.data() + size_t(i) * 2, 2);
      (*out)[i] = v;
    }
  }
}

// ---------------------------------------------------------------------------
// the triangles of a draw
// ---------------------------------------------------------------------------

// A draw's triangles as vertex numbers into its stream, once expanded to a list (topology.h ConvertIndices / StripToList /
// GenerateIndices do that on the renderer's side): a triangle with a repeated vertex has no area (a strip's joins) and is
// counted and left out, one that names a vertex the stream does not hold is counted and left out.
struct TriangleSet {
  std::vector<uint32_t> tri;  // 3 vertex numbers a triangle
  uint32_t degenerate = 0;
  uint32_t out_of_range = 0;
  uint32_t first_vertex = 0xFFFFFFFFu;  // the smallest and largest vertex number the kept triangles name
  uint32_t last_vertex = 0;
  uint32_t triangles() const { return uint32_t(tri.size() / 3); }
  uint32_t span() const { return tri.empty() ? 0u : last_vertex - first_vertex + 1; }
};

// `count` list entries (whole triangles only), each plus `vertex_base` the vertex number; `available` vertices exist.
inline void CollectTriangles(const uint32_t* list, uint32_t count, int64_t vertex_base, uint32_t available, TriangleSet* out) {
  out->tri.reserve(out->tri.size() + size_t(count));
  for (uint32_t t = 0; t + 3 <= count; t += 3) {
    int64_t v[3];
    bool inside = true;
    for (int k = 0; k < 3; ++k) {
      v[k] = int64_t(list[t + k]) + vertex_base;
      inside &= v[k] >= 0 && v[k] < int64_t(available);
    }
    if (!inside) {
      ++out->out_of_range;
      continue;
    }
    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
      ++out->degenerate;
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      const uint32_t n = uint32_t(v[k]);
      out->tri.push_back(n);
      out->first_vertex = std::min(out->first_vertex, n);
      out->last_vertex = std::max(out->last_vertex, n);
    }
  }
}

// ---------------------------------------------------------------------------
// the rasteriser
// ---------------------------------------------------------------------------

struct Vec4 {
  double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
};

// The image a triangle is rasterised into: `width` x `height` pixels, the pixel (x, y) sampled at (x + sample, y + sample).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  double sample = 0.5;
};

// Where the history image's texel is read at: car_history_ps.hlsl takes the pixel at position.xy * divisor, which is the pixel at
// the lower right of a 2x2 block at divisor 2 (its centre is 3/4 of the way across the texel), and the pixel itself at 1.
inline double HistorySample(uint32_t divisor) {
  const uint32_t d = std::max(divisor, 1u);
  return (double(d / 2) + 0.5) / double(d);
}

inline Vec4 Project(const temporal::Mat4& view_projection_of_model, const double p[3]) {
  double c[4];
  TransformPoint(view_projection_of_model, p, c);
  return Vec4{c[0], c[1], c[2], c[3]};
}

// Clip-space w a vertex must have to be kept: the camera's near plane in w (w is the view distance).
inline constexpr double kNearW = 1e-3;

// The triangle clipped against w >= kNearW: 0 (all behind), 3 or 4 vertices (Sutherland-Hodgman, one plane).
inline int ClipNear(const Vec4 in[3], Vec4 out[4]) {
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    const Vec4& a = in[i];
    const Vec4& b = in[(i + 1) % 3];
    const bool a_in = a.w >= kNearW;
    const bool b_in = b.w >= kNearW;
    if (a_in) {
      out[n++] = a;
    }
    if (a_in != b_in) {
      const double t = (kNearW - a.w) / (b.w - a.w);
      out[n++] = Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, kNearW};
    }
  }
  return n;
}

// The pixel position of a clip-space vertex (w > 0): ndc x to the left edge 0 and the right edge `width`, ndc y up to row 0 at the top.
inline void ToImage(const Vec4& v, const Image& im, double* x, double* y) {
  *x = (v.x / v.w * 0.5 + 0.5) * double(im.width);
  *y = (0.5 - v.y / v.w * 0.5) * double(im.height);
}

// One screen triangle (clip space, all w > 0): calls sink(x, y, distance) for every pixel whose sample point it covers (edges
// inclusive, so two triangles sharing an edge leave no gap), with the distance w interpolated perspective-correct (1/w is
// linear on the screen). Either winding.
template <typename Sink>
void RasterizeScreen(const Image& im, const Vec4& a, const Vec4& b, const Vec4& c, Sink&& sink) {
  double ax, ay, bx, by, cx, cy;
  ToImage(a, im, &ax, &ay);
  ToImage(b, im, &bx, &by);
  ToImage(c, im, &cx, &cy);
  const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (!(std::fabs(area) > 1e-12) || !std::isfinite(area)) {
    return;
  }
  const double min_x = std::min({ax, bx, cx});
  const double max_x = std::max({ax, bx, cx});
  const double min_y = std::min({ay, by, cy});
  const double max_y = std::max({ay, by, cy});
  if (max_x < 0.0 || max_y < 0.0 || min_x > double(im.width) || min_y > double(im.height)) {
    return;
  }
  const int64_t x0 = std::max<int64_t>(0, int64_t(std::ceil(min_x - im.sample)));
  const int64_t x1 = std::min<int64_t>(int64_t(im.width) - 1, int64_t(std::floor(max_x - im.sample)));
  const int64_t y0 = std::max<int64_t>(0, int64_t(std::ceil(min_y - im.sample)));
  const int64_t y1 = std::min<int64_t>(int64_t(im.height) - 1, int64_t(std::floor(max_y - im.sample)));
  const double iwa = 1.0 / a.w, iwb = 1.0 / b.w, iwc = 1.0 / c.w;
  const double inv_area = 1.0 / area;
  constexpr double kEdge = -1e-9;
  for (int64_t y = y0; y <= y1; ++y) {
    const double py = double(y) + im.sample;
    for (int64_t x = x0; x <= x1; ++x) {
      const double px = double(x) + im.sample;
      const double la = ((cx - bx) * (py - by) - (cy - by) * (px - bx)) * inv_area;
      const double lb = ((ax - cx) * (py - cy) - (ay - cy) * (px - cx)) * inv_area;
      const double lc = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv_area;
      if (la < kEdge || lb < kEdge || lc < kEdge) {
        continue;
      }
      const double inverse = la * iwa + lb * iwb + lc * iwc;
      if (inverse > 0.0) {
        sink(uint32_t(x), uint32_t(y), 1.0 / inverse);
      }
    }
  }
}

// A triangle in clip space: clipped at the near plane, then rasterised (a clipped triangle may leave a quad: two triangles).
template <typename Sink>
void RasterizeClip(const Image& im, const Vec4 clip[3], Sink&& sink) {
  Vec4 poly[4];
  const int n = ClipNear(clip, poly);
  for (int i = 1; i + 1 < n; ++i) {
    RasterizeScreen(im, poly[0], poly[i], poly[i + 1], sink);
  }
}

// What the triangle is relative to the image, for the "off the image" count: no vertex of it is in front of the camera and
// inside the image's rectangle.
inline bool OffImage(const Vec4 clip[3], const Image& im) {
  for (int i = 0; i < 3; ++i) {
    if (!(clip[i].w >= kNearW)) {
      continue;
    }
    double x, y;
    ToImage(clip[i], im, &x, &y);
    if (x >= 0.0 && x < double(im.width) && y >= 0.0 && y < double(im.height)) {
      return false;
    }
  }
  return true;
}
inline bool BehindCamera(const Vec4 clip[3]) { return !(clip[0].w >= kNearW) && !(clip[1].w >= kNearW) && !(clip[2].w >= kNearW); }

// The nearest distance of every pixel a triangle covers (infinity where none does).
struct DepthGrid {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> distance;

  void Reset(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    distance.assign(size_t(w) * h, std::numeric_limits<float>::infinity());
  }
  void Splat(uint32_t x, uint32_t y, double w) {
    float& d = distance[size_t(y) * width + x];
    if (float(w) < d) {
      d = float(w);
    }
  }
  bool Covered(size_t i) const { return distance[i] < std::numeric_limits<float>::infinity(); }
};

// ---------------------------------------------------------------------------
// the statistics
// ---------------------------------------------------------------------------

// The q-th quantile (0 to 1) of `v` (reordered): 0 for none.
inline double Quantile(std::vector<float>* v, double q) {
  if (v->empty()) {
    return 0.0;
  }
  const size_t k = std::min(v->size() - 1, size_t(q * double(v->size() - 1) + 0.5));
  std::nth_element(v->begin(), v->begin() + std::ptrdiff_t(k), v->end());
  return double((*v)[k]);
}

// The rasterised triangles against an image of view distances (the car history image's alpha: > 0 on a vehicle's pixel, 0
// elsewhere): how much of the vehicle's pixels the triangles cover and how close their nearest distance is to it.
struct CheckStats {
  uint64_t pixels = 0;          // pixels of the image
  uint64_t vehicle = 0;         // ... whose distance is > 0
  uint64_t covered = 0;         // vehicle pixels with a triangle
  uint64_t raster_only = 0;     // pixels with a triangle and no vehicle (hidden behind the world, or off the car)
  uint64_t within_1pct = 0;     // covered pixels whose distances differ by at most 1%
  uint64_t within_2pct = 0;
  uint64_t triangle_nearer = 0;   // covered pixels where the triangle is nearer than the vehicle surface by more than the tolerance
  uint64_t triangle_farther = 0;  // ... farther
  double coverage = 0.0;        // covered / vehicle
  double mean_rel = 0.0, p50_rel = 0.0, p95_rel = 0.0, max_rel = 0.0;  // |triangle - image| / image over the covered pixels
  double mean_abs = 0.0, p95_abs = 0.0;                                 // the same in the image's units
};

inline CheckStats CompareToDistance(const std::vector<float>& image, const DepthGrid& grid, double tolerance) {
  CheckStats s;
  s.pixels = std::min<uint64_t>(image.size(), grid.distance.size());
  std::vector<float> rel, absolute;
  for (size_t i = 0; i < s.pixels; ++i) {
    const float reference = image[i];
    const bool vehicle = std::isfinite(reference) && reference > 0.0f;
    const bool covered = grid.Covered(i);
    if (vehicle) {
      ++s.vehicle;
    }
    if (covered && !vehicle) {
      ++s.raster_only;
    }
    if (!vehicle || !covered) {
      continue;
    }
    ++s.covered;
    const double d = double(grid.distance[i]);
    const double diff = d - double(reference);
    const double r = std::fabs(diff) / double(reference);
    rel.push_back(float(r));
    absolute.push_back(float(std::fabs(diff)));
    s.mean_rel += r;
    s.mean_abs += std::fabs(diff);
    s.max_rel = std::max(s.max_rel, r);
    s.within_1pct += r <= 0.01;
    s.within_2pct += r <= 0.02;
    s.triangle_nearer += diff < -tolerance * double(reference);
    s.triangle_farther += diff > tolerance * double(reference);
  }
  s.coverage = s.vehicle ? double(s.covered) / double(s.vehicle) : 0.0;
  if (s.covered) {
    s.mean_rel /= double(s.covered);
    s.mean_abs /= double(s.covered);
  }
  s.p50_rel = Quantile(&rel, 0.5);
  s.p95_rel = Quantile(&rel, 0.95);
  s.p95_abs = Quantile(&absolute, 0.95);
  return s;
}

// How a draw's triangles sit against the image, pixel by pixel of THEIR OWN coverage (no nearest-triangle test: a closed mesh
// has surfaces behind the visible one, which the image does not see). A model matrix that is right gives mostly `match` on the
// pixels where the draw is the visible surface and `behind` on the others, and next to no `front`; a wrong one gives all three
// at random.
struct FitCount {
  uint64_t samples = 0;  // triangle pixels on a vehicle's pixel
  uint64_t match = 0;    // distance within the tolerance of the image's
  uint64_t front = 0;    // nearer than the image's by more than the tolerance
  uint64_t behind = 0;   // farther
  uint64_t off = 0;      // triangle pixels on a pixel that is no vehicle's
  void Add(double distance, float reference, double tolerance) {
    if (!(reference > 0.0f)) {
      ++off;
      return;
    }
    ++samples;
    const double diff = distance - double(reference);
    const double limit = tolerance * double(reference);
    if (diff < -limit) {
      ++front;
    } else if (diff > limit) {
      ++behind;
    } else {
      ++match;
    }
  }
  void Merge(const FitCount& o) {
    samples += o.samples;
    match += o.match;
    front += o.front;
    behind += o.behind;
    off += o.off;
  }
  double Share(uint64_t part) const { return samples ? double(part) / double(samples) : 0.0; }
};

}  // namespace mcla::gpu::car_dump
