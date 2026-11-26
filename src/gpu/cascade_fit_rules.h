#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mcla::gpu::cascade_fit {

using Vec = std::array<double, 3>;
using Corners = std::array<Vec, 8>;

inline Vec Add(const Vec& a, const Vec& b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline Vec Sub(const Vec& a, const Vec& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Vec Scale(const Vec& v, double s) { return {v[0] * s, v[1] * s, v[2] * s}; }
inline double Dot(const Vec& a, const Vec& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline double Length(const Vec& v) { return std::sqrt(Dot(v, v)); }
inline Vec Cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
inline Vec Unit(const Vec& v) { return Scale(v, 1.0 / Length(v)); }

struct Pose {
  Vec eye{};
  Vec forward{};
  Vec up{};
};

struct Basis {
  Vec right{};
  Vec up{};
  Vec sun{};
};
inline Basis WorldBasis(const Vec& sun) {
  // Y is up. At the zenith the horizontal world X axis avoids the singularity.
  const Vec axis = std::abs(sun[1]) < 0.99 ? Vec{0, 1, 0} : Vec{1, 0, 0};
  const Vec right = Unit(Cross(sun, axis));
  return {right, Cross(right, sun), sun};  // same handedness as the guest's frame
}

struct Sphere {
  Vec center{};
  double radius = 0.0;
};
inline Sphere EncloseCone(const Corners& corners) {
  Vec near_center{}, far_center{};
  for (unsigned i = 0; i < 4; ++i) {
    near_center = Add(near_center, Scale(corners[i], 0.25));
    far_center = Add(far_center, Scale(corners[i + 4], 0.25));
  }
  const Vec axis = Sub(far_center, near_center);
  const double length_squared = Dot(axis, axis);
  if (!(length_squared > 0.01) || !std::isfinite(length_squared)) return {};
  const Vec near_edge = Sub(corners[0], near_center);
  const Vec far_edge = Sub(corners[4], far_center);
  const double t = std::clamp(0.5 + (Dot(far_edge, far_edge) - Dot(near_edge, near_edge)) /
                                        (2.0 * length_squared), 0.0, 1.0);
  const Vec center = Add(near_center, Scale(axis, t));
  double radius = 0;
  for (const Vec& corner : corners) {
    const double distance = Length(Sub(corner, center));
    if (!std::isfinite(distance)) return {};
    radius = std::max(radius, distance);
  }
  return {center, radius};
}

inline constexpr double kOneFrameTravel = 3.0;  // SHADOW-ARCH 7.1; the archived hitch was 2.9 m
inline constexpr double kCasterReach = 12.0;
inline constexpr double kFarStep = 8.0;
inline constexpr double kMaxDepthTravel = 8.0;
inline constexpr double kMinDepthViewCos = 0.998629535;  // 3 degrees, including roll
inline constexpr double kHoldTurnChord = 0.070;  // just over four degrees

inline double MotionBound(double travel, double cosine, double lever) {
  return travel + lever * std::sqrt(std::max(0.0, 2.0 - 2.0 * cosine));
}

inline double VisibleFar(const Pose& sample, const Pose& current, double depth, double stock_far) {
  const double travel = Length(Sub(sample.eye, current.eye));
  const double cosine = std::min(Dot(sample.forward, current.forward), Dot(sample.up, current.up));
  if (!(depth > 0 && depth < stock_far && travel <= kMaxDepthTravel && cosine >= kMinDepthViewCos)) {
    return stock_far;
  }
  return std::min(stock_far, depth + kCasterReach + kOneFrameTravel +
                                 MotionBound(travel, cosine, stock_far));
}

inline float SelectFar(float stock, float inner, double required, float previous) {
  if (required >= stock) return stock;
  // Keep adjacent tiles distinct even if the farthest receiver fits in the near tiles.
  const double target = std::max(required, double(inner) + kFarStep);
  const float band = float(std::min(double(stock), std::ceil(target / kFarStep) * kFarStep));
  if (previous >= band && previous <= stock && previous - band <= kFarStep) return previous;
  return band;
}

inline double LinearDepth(float stored, const std::array<double, 4>& lens) {
  if (!(stored > 0.0f && stored <= 1.0f)) return 0.0;
  const double ndc = 1.0 - stored;
  const double den = lens[0] - ndc * lens[1];
  const double depth = den != 0.0 ? (lens[2] - ndc * lens[3]) / den : 0.0;
  return std::isfinite(depth) && depth > 0.0 ? depth : 0.0;
}

struct Box {
  Vec center{};
  double half = 0.0;
  double texel = 0.0;
};
inline double FitMargin(double lever) {
  return kOneFrameTravel + kHoldTurnChord * lever;
}

inline Box Snap(const Sphere& sphere, const Basis& basis, uint32_t texels, double margin) {
  if (!(sphere.radius > 0.0) || texels < 2) return {};
  // Upward size quantisation damps small lens changes. Leave room for the
  // receiver motion guard, and half a texel for rounding on every axis.
  const double padded = std::ceil((sphere.radius + margin + 0.001) * 4.0) * 0.25;
  const double half = padded / (1.0 - 1.0 / texels);
  const double texel = 2.0 * half / texels;
  const auto grid = [texel](double x) { return std::round(x / texel) * texel; };
  return {{grid(Dot(sphere.center, basis.right)), grid(Dot(sphere.center, basis.up)),
           grid(Dot(sphere.center, basis.sun))}, half, texel};
}

inline bool HoldCovers(double travel, double cosine, double lever) {
  return MotionBound(travel, cosine, lever) <= FitMargin(lever);
}

// A copied guest cull plane takes the sphere if dot(n, center) + w >= -radius.
// Plane normals need not have exact unit length.
inline float PadPlane(const Vec& normal, float w, double margin) {
  return float(w + Length(normal) * margin);
}

inline constexpr std::array<unsigned, 6> kBoundOrder{0, 1, 2, 0, 1, 2};
inline std::array<double, 6> Bounds(const Box& box) {
  std::array<double, 6> result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    result[i] = box.center[kBoundOrder[i]] + (i < 3 ? -box.half : box.half);
  }
  return result;
}

}  // namespace mcla::gpu::cascade_fit
