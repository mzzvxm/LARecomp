#pragma once
// ===========================================================================
// MCLA GPU renderer -- a blue noise tile for the screen-space effects' sampling (pure maths, no GPU)
// ===========================================================================
// The ambient occlusion, the bounce light, the contact shadows and the reflections each cast a few samples a pixel and take the sampling
// error out with a small blur and an average over frames. The first versions drew the per-pixel part of that error from interleaved gradient
// noise (Jimenez 2014, frac(52.98 * frac(0.0671 x + 0.0058 y))). It is a LATTICE: a linear phase of the pixel's position, so its spectrum is
// a few sharp peaks, not a spread. The error of the effect is then a diagonal lattice too (hlsl/ao_common.hlsli had its period at 9.8
// pixels), a blur of a few pixels does not touch a peak at a period of ten, and the average over frames does not either when the camera moves
// or a pixel has no history: the owner saw it as thin diagonal lines on the road and on the kerbs (static) and, in the reflections, as stripes
// that crawl (the reflection's noise slid across the picture by 5.6 pixels a frame).
//
// A BLUE NOISE tile has no peak and no energy at the low spatial frequencies: neighbouring pixels get far apart numbers, so the error of
// neighbouring pixels is anti-correlated, and the blur removes what is left of it. This file makes one by the void-and-cluster method
// (Ulichney 1993, "The void-and-cluster method for dither array generation"), on a torus so that the tile repeats without a seam, with
// INTEGER arithmetic only (a Gaussian kernel rounded to integers once, energies summed as 64 bit integers, ties to the lowest index): the same
// tile on every compiler and platform.
//
// The generator's ranks are a PERMUTATION of 0 to kCount - 1. The shipped tile keeps the top 8 bits of each rank (kLevels = 256 levels, four pixels
// to a level, so a shader holds the whole tile in 256 words), and the noise's value (level + 0.5) / 256 is uniform on (0, 1) EXACTLY, and stays so
// whatever a shader adds to it modulo 1 (the frame's shift). A sample's SECOND number (the step offset; the reflection's second uniform number) is
// the same tile read at the pixel moved by (kSecondX, kSecondY): the offset is the one of the tile's own offsets of 11 pixels or more whose
// correlation with the tile is smallest over the 7 x 7 pixels round it (blue_noise_test.cpp searches for it and checks it: under 0.1), so
// the two numbers of a pixel are two independent blue noises, from a table half the size of two tiles. The shipped table is hlsl/blue_noise.hlsli,
// WRITTEN by `larecomp_blue_noise_test --emit` from this generator and checked against it by the same test: do not edit it by hand.
//
// WHAT THE FRAME'S SHIFT DOES TO IT. The shaders use value = frac(tile + shift(frame)) with the same shift for every pixel (the occlusion's
// lattice of 13 frames, the reflections' R2 sequence): a pixel's own sequence over frames is then low discrepancy whatever its tile value,
// and the picture of one frame is the tile moved round the circle, still a blue noise for any quantity that is periodic in the value (an angle,
// a position inside a step). The tile itself never moves: a noise that slides across the picture (the first reflections' 5.6 pixels a frame)
// makes a moving pattern of whatever error the average leaves.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace mcla::gpu::blue_noise {

#ifndef MCLA_BLUE_NOISE_SIZE
#define MCLA_BLUE_NOISE_SIZE 32
#endif
inline constexpr int kSize = MCLA_BLUE_NOISE_SIZE;  // the tile is kSize x kSize and repeats
inline constexpr int kCount = kSize * kSize;
inline constexpr int kChannels = 2;
inline constexpr int kLevels = 256;             // the tile keeps 8 bits of each pixel's rank
inline constexpr uint32_t kSeed = 0x6D2B79F5u;
inline constexpr int kSecondX = 13;             // where the second channel reads the tile, relative to the pixel
inline constexpr int kSecondY = 8;
inline constexpr double kSigma = 1.5;          // the Gaussian's width in pixels (Ulichney's 1.5)
inline constexpr int kKernelRadius = 8;        // taps beyond it are below 1e-6 of the centre
inline constexpr int64_t kKernelScale = int64_t(1) << 24;

// A tile's ranks, row by row: rank[y * size + x], a permutation of 0 .. size * size - 1.
struct Ranks {
  int size = 0;
  std::vector<uint16_t> rank;
};

namespace detail {

struct Lcg {
  uint32_t s;
  uint32_t Next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

// The energy field of a binary pattern on a torus: e[i] = the sum of the kernel over the ones, centred on pixel i (a one's own pixel is in its
// own energy: the same for every one, so it changes no comparison among ones, and among zeros nothing of the kind is there).
class Field {
 public:
  Field(int size, double sigma) : size_(size), e_(size_t(size) * size_t(size), 0) {
    const int r = kKernelRadius;
    const int w = 2 * r + 1;
    kernel_.assign(size_t(w) * size_t(w), 0);
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        kernel_[size_t(dy + r) * size_t(w) + size_t(dx + r)] =
            int64_t(std::llround(std::exp(-double(dx * dx + dy * dy) / (2.0 * sigma * sigma)) * double(kKernelScale)));
      }
    }
  }
  void Add(int index, int sign) {
    const int r = kKernelRadius;
    const int w = 2 * r + 1;
    const int cx = index % size_, cy = index / size_;
    for (int dy = -r; dy <= r; ++dy) {
      const int y = ((cy + dy) % size_ + size_) % size_;
      for (int dx = -r; dx <= r; ++dx) {
        const int x = ((cx + dx) % size_ + size_) % size_;
        e_[size_t(y) * size_t(size_) + size_t(x)] += int64_t(sign) * kernel_[size_t(dy + r) * size_t(w) + size_t(dx + r)];
      }
    }
  }
  int64_t operator[](int i) const { return e_[size_t(i)]; }
  void Clear() { std::fill(e_.begin(), e_.end(), int64_t(0)); }

 private:
  int size_;
  std::vector<int64_t> e_;
  std::vector<int64_t> kernel_;
};

// The one (value == 1) with the highest energy: the tightest cluster; or, with value == 0, the zero with the lowest: the largest void. Ties: the lowest index.
inline int Tightest(const Field& f, const std::vector<uint8_t>& bits, int n) {
  int best = -1;
  for (int i = 0; i < n; ++i) {
    if (bits[size_t(i)] == 1 && (best < 0 || f[i] > f[best])) {
      best = i;
    }
  }
  return best;
}
inline int Largest(const Field& f, const std::vector<uint8_t>& bits, int n) {
  int best = -1;
  for (int i = 0; i < n; ++i) {
    if (bits[size_t(i)] == 0 && (best < 0 || f[i] < f[best])) {
      best = i;
    }
  }
  return best;
}

}  // namespace detail

// The void-and-cluster ranks of a size x size torus: an initial pattern of a tenth of the pixels, relaxed until no one is in a tighter cluster than
// the largest void; phase 1 ranks the initial ones from the tightest cluster down; phase 2 gives the pixels from the initial count up to the end the
// rank of the largest void as it fills (past the half the largest void of the ones is the tightest cluster of the zeros: one rule to the end).
inline Ranks Generate(int size, uint32_t seed, double sigma = kSigma) {
  const int n = size * size;
  detail::Lcg rng{seed};
  std::vector<int> order(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    order[size_t(i)] = i;
  }
  for (int i = n - 1; i > 0; --i) {
    const int j = int(rng.Next() % uint32_t(i + 1));
    std::swap(order[size_t(i)], order[size_t(j)]);
  }
  const int ones = n / 10;
  std::vector<uint8_t> bits(static_cast<size_t>(n), uint8_t(0));
  detail::Field field(size, sigma);
  for (int i = 0; i < ones; ++i) {
    bits[size_t(order[size_t(i)])] = 1;
    field.Add(order[size_t(i)], +1);
  }
  // Relax: move the tightest cluster's one to the largest void until the void is where the one came from.
  for (int guard = 0; guard < 8 * n; ++guard) {
    const int tight = detail::Tightest(field, bits, n);
    bits[size_t(tight)] = 0;
    field.Add(tight, -1);
    const int hole = detail::Largest(field, bits, n);
    bits[size_t(hole)] = 1;
    field.Add(hole, +1);
    if (hole == tight) {
      break;
    }
  }
  const std::vector<uint8_t> initial = bits;

  Ranks out;
  out.size = size;
  out.rank.assign(size_t(n), 0);
  // Phase 1: the initial ones, tightest cluster first, take the ranks ones - 1 down to 0.
  for (int r = ones - 1; r >= 0; --r) {
    const int tight = detail::Tightest(field, bits, n);
    out.rank[size_t(tight)] = uint16_t(r);
    bits[size_t(tight)] = 0;
    field.Add(tight, -1);
  }
  // Phase 2: from the initial pattern again, the largest void takes the next rank, to the end.
  bits = initial;
  field.Clear();
  for (int i = 0; i < n; ++i) {
    if (bits[size_t(i)] == 1) {
      field.Add(i, +1);
    }
  }
  for (int r = ones; r < n; ++r) {
    const int hole = detail::Largest(field, bits, n);
    out.rank[size_t(hole)] = uint16_t(r);
    bits[size_t(hole)] = 1;
    field.Add(hole, +1);
  }
  return out;
}

// The shipped tile: one 8 bit level a pixel, row by row (level = the generator's rank * kLevels / kCount).
struct Tile {
  uint8_t level[kCount];
};
inline Tile MakeTile() {
  Tile t{};
  const Ranks r = Generate(kSize, kSeed);
  for (int i = 0; i < kCount; ++i) {
    t.level[i] = uint8_t(int(r.rank[size_t(i)]) * kLevels / kCount);
  }
  return t;
}

// The noise value of a level: uniform on (0, 1), the same function the shader uses (hlsl/blue_noise.hlsli).
inline float Value(uint8_t level) { return (float(level) + 0.5f) * (1.0f / float(kLevels)); }

// The wrapped lookup: any integer pixel position (the tile repeats). Channel 0 is the tile at the pixel, channel 1 at the pixel moved by (kSecondX, kSecondY).
inline int Wrap(int v) { return v & (kSize - 1); }
inline float Sample(const Tile& t, int channel, int x, int y) {
  if (channel != 0) {
    x += kSecondX;
    y += kSecondY;
  }
  return Value(t.level[Wrap(y) * kSize + Wrap(x)]);
}

}  // namespace mcla::gpu::blue_noise
