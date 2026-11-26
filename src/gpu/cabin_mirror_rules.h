#pragma once

#include <array>

#include "car_mirror_patch_rules.h"
#include "mirror_view_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::cabin_mirror {

using mirror_view::Dot;
using mirror_view::Length;
using mirror_view::Normalize;
using mirror_view::V3;
namespace cm = car_mirror;

inline constexpr uint8_t kReader = 1u << 2;
inline constexpr uint32_t kGlassIndex = 2;
inline constexpr int kCockpitRig = 1;
inline constexpr uint32_t kMaskWidth = 256;
inline constexpr uint32_t kMaskHeight = 64;

inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Vector(V3 p, const temporal::Mat4& m) {
  return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0],
          p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1],
          p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2]};
}
inline V3 Point(V3 p, const temporal::Mat4& m) { return Vector(p, m) + V3{m.m[3][0], m.m[3][1], m.m[3][2]}; }

inline bool ClassMayBeCabin(uint8_t cls) {
  return cls == cm::kClassChrome || cls == cm::kClassGlass || cls == cm::kClassInterior || cls == cm::kClassOther || cls == cm::kClassNone;
}

struct Aperture {
  V3 across;
  V3 up;
  double left = 0, right = 0, bottom = 0, top = 0;  // relative to mesh centroid
};

inline bool ShapeMayBeCabin(const cm::PatchStats& stats) {
  return stats.valid && stats.triangles >= 8 && stats.triangles <= 60 && stats.area >= 0.004f && stats.area <= 0.06f &&
         stats.flat >= 0.95f && stats.extent <= 0.40f;
}

// Find the principal axis in the plane of the glass, independently of mesh orientation.
inline Aperture Measure(const cm::PatchStats& stats, const std::vector<float>& positions, const std::vector<uint32_t>& indices) {
  if (!ShapeMayBeCabin(stats)) return {};
  const V3 n{stats.normal[0], stats.normal[1], stats.normal[2]};
  const V3 centre{stats.centroid[0], stats.centroid[1], stats.centroid[2]};
  const V3 x = Normalize(Cross(std::abs(n.y) < 0.9 ? V3{0, 1, 0} : V3{1, 0, 0}, n));
  const V3 y = Cross(n, x);
  const auto relative = [&](size_t i) { return V3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]} - centre; };
  std::array<uint32_t, 180> unique_indices;
  if (indices.size() > unique_indices.size()) return {};
  const auto end = std::copy(indices.begin(), indices.end(), unique_indices.begin());
  std::sort(unique_indices.begin(), end);
  const auto unique_end = std::unique(unique_indices.begin(), end);
  double xx = 0, xy = 0, yy = 0;
  for (auto i = unique_indices.begin(); i != unique_end; ++i) {
    const V3 p = relative(*i);
    const double u = Dot(p, x), v = Dot(p, y);
    xx += u * u;
    xy += u * v;
    yy += v * v;
  }
  const double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
  Aperture a;
  a.across = x * std::cos(angle) + y * std::sin(angle);
  a.up = Cross(n, a.across);
  for (uint32_t i : indices) {
    const V3 p = relative(i);
    const double u = Dot(p, a.across), v = Dot(p, a.up);
    a.left = std::min(a.left, u);
    a.right = std::max(a.right, u);
    a.bottom = std::min(a.bottom, v);
    a.top = std::max(a.top, v);
  }
  return a;
}

inline bool IsCabinCandidate(const cm::PatchStats& stats, const Aperture& a, uint8_t cls) {
  const double width = a.right - a.left, height = a.top - a.bottom;
  return ShapeMayBeCabin(stats) && height > 0.0 && width >= 2.0 * height && width <= 0.40 && ClassMayBeCabin(cls);
}

// Vehicle axes are right, up and back. The best rear-facing patch is high
// and near the centre line. The width gate excludes narrow vehicles.
inline double Score(V3 centre, V3 normal, V3 body_min, V3 body_max) {
  const double width = body_max.x - body_min.x, height = body_max.y - body_min.y;
  const double lateral = std::abs(centre.x - (body_min.x + body_max.x) * 0.5);
  const double elevation = centre.y - body_min.y;
  if (width < 1.15 || !(height > 0.0) || lateral > 0.08 || elevation < 0.80 * height ||
      normal.z < 0.85 || std::abs(normal.y) > 0.50) return -1.0;
  return (1.0 - lateral / 0.08) + elevation / height + normal.z;
}

inline V3 VirtualOffset(V3 eye, V3 centre, V3 normal) { return normal * (2.0 * Dot(centre - eye, normal)); }

// 8 mm precision, +/-4 m on each camera axis. Zero means the phase used its
// original eye. The phase's actual offset travels in its ordered end record.
inline uint32_t PackOffset(V3 offset) {
  const auto fits = [](double v) { return std::isfinite(v) && v >= -4.0 && v <= 4.0; };
  if (!fits(offset.x) || !fits(offset.y) || !fits(offset.z) || !(offset.z > 0.0)) return 0;
  const auto pack = [](double v) { return uint32_t(int32_t(std::lround(v * 125.0))) & 1023u; };
  return (1u << 30) | pack(offset.x) | (pack(offset.y) << 10) | (pack(offset.z) << 20);
}

inline V3 UnpackOffset(uint32_t tag) {
  if (!(tag & (1u << 30))) return {};
  const auto unpack = [](uint32_t v) { return double(int32_t((v & 1023u) ^ 512u) - 512) / 125.0; };
  return {unpack(tag), unpack(tag >> 10), unpack(tag >> 20)};
}

inline V3 OffsetInRearAxes(V3 offset, V3 right, V3 up, V3 back) { return up * offset.y - right * offset.x + back * offset.z; }

inline bool IsBlocker(uint64_t ps, uint8_t cls, bool depth_write) {
  return !cm::IsSeeThroughShader(ps) && cls != cm::kClassTyre && cls != cm::kClassLight &&
         (depth_write || cls == cm::kClassInterior);
}

// The plane of the glass is w = 1. The near clip plane excludes surfaces
// between the virtual eye and the glass, including the glass itself.
inline temporal::Mat4 MaskProjection(const Aperture& a, V3 centre, V3 normal, V3 eye) {
  temporal::Mat4 m{};
  const double distance = Dot(centre - eye, normal);
  const double width = a.right - a.left, height = a.top - a.bottom;
  const V3 delta = centre - eye;
  const V3 w = normal * (1.0 / distance);
  const V3 x = a.across * (2.0 / width) - w * ((a.left + a.right + 2.0 * Dot(delta, a.across)) / width);
  const V3 y = a.up * (2.0 / height) - w * ((a.bottom + a.top + 2.0 * Dot(delta, a.up)) / height);
  const auto put = [&](int col, V3 axis, double constant) {
    m.m[0][col] = axis.x;
    m.m[1][col] = axis.y;
    m.m[2][col] = axis.z;
    m.m[3][col] = constant - Dot(eye, axis);
  };
  put(0, x, 0.0);
  put(1, y, 0.0);
  put(2, w, -1.0 - 0.005 / distance);
  put(3, w, 0.0);
  return m;
}

inline constexpr int64_t kRefreshMs = 500;
inline bool Refresh(bool valid, bool parts_changed, double eye_motion, int64_t elapsed_ms) {
  return !valid || parts_changed || eye_motion >= 0.08 || (eye_motion >= 0.005 && elapsed_ms >= kRefreshMs);
}

}  // namespace mcla::gpu::cabin_mirror
