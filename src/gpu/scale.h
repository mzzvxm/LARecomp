#pragma once

// MCLA GPU renderer -- the internal resolution scale (M6e): n/20 host pixels
// per guest pixel, kept exact.
//
// Twentieths because 20 is the greatest common divisor of the game's images
// of the screen (1280x720 and its halvings down to 320x180, the ones the
// frame census found): on this grid each of them has a whole size at any
// scale, and the 2:1 between them stays exact. A guest edge x lands on host
// pixel Edge(x) = floor(x * s + 1/2), in integers -- the rounding
// EdgeOfTimeRecomp applies to its fractional scale -- so rectangles that share
// an edge in guest pixels share it on the host. At a whole scale Edge(x) is
// x * s.

#include <cstdint>
#include <format>
#include <string>

namespace mcla::gpu {

struct Scale {
  static constexpr uint32_t kDenominator = 20;

  // Twentieths: 20 is 1x. 0 only as "no reader" (TextureCache::Bind2D).
  uint32_t n = kDenominator;

  static constexpr Scale Whole(uint32_t s) { return Scale{s * kDenominator}; }
  static constexpr Scale None() { return Scale{0}; }

  constexpr auto operator<=>(const Scale&) const = default;
  constexpr bool none() const { return n == 0; }
  constexpr bool unit() const { return n == kDenominator; }
  constexpr bool whole() const { return n % kDenominator == 0; }
  // Host pixels per guest pixel (exact for a whole scale).
  constexpr float value() const { return float(n) / float(kDenominator); }
  // The host pixel a guest edge `x` lands on; also a size in host pixels.
  constexpr uint32_t Edge(uint32_t x) const {
    return uint32_t((uint64_t(x) * n + kDenominator / 2) / kDenominator);
  }
  // The host columns and rows at a target's origin that a full-screen draw
  // leaves uncovered with the D3D pixel centre: the geometry starts half a
  // guest pixel in, s/2 host pixels, so every host pixel whose centre lies
  // before that -- ceil((s - 1) / 2) of them, 0 at s <= 1.
  constexpr uint32_t HalfPixelGap() const {
    return n > kDenominator ? (n - kDenominator + 2 * kDenominator - 1) / (2 * kDenominator) : 0;
  }
  // "2" for a whole scale, "1.35" otherwise (logs print it followed by "x").
  std::string Text() const {
    if (whole()) {
      return std::to_string(n / kDenominator);
    }
    std::string text = std::format("{:.2f}", value());
    if (text.back() == '0') {
      text.pop_back();
    }
    return text;
  }
};

// The logs (fmt, found by argument-dependent lookup) print it as Text().
inline std::string format_as(Scale scale) { return scale.Text(); }

}  // namespace mcla::gpu

template <>
struct std::formatter<mcla::gpu::Scale> : std::formatter<std::string> {
  auto format(const mcla::gpu::Scale& scale, std::format_context& ctx) const {
    return std::formatter<std::string>::format(scale.Text(), ctx);
  }
};
