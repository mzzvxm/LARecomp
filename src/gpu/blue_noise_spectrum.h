#pragma once
// Spectra of noise images for the checks of the blue noise tile and of what uses it (test support only: no renderer, no GPU).
//
// Power() is the 2D power spectrum of an n x n image (n a power of two); Analyse() reads it the way the eye sees a pattern: how much of the energy sits in ONE
// frequency (a lattice's peak), how much in the band of 0.03 to 0.25 cycles a pixel (what a 3x3 blur does not take out), and how unevenly that band is spread over
// the directions (a diagonal lattice has one). Ign() is interleaved gradient noise, Blur3() the occlusion's first blur (3x3, sigma 0.9) on a periodic image.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace blue_noise_spectrum {

typedef std::complex<double> Cx;

inline void Fft(std::vector<Cx>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2.0 * 3.14159265358979323846 / double(len);
    const Cx wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      Cx w(1.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const Cx u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// The power spectrum of an n x n image (n a power of two), mean removed, optionally under a Hann window: power[ky * n + kx], frequency (kx, ky) / n with
// the negative half wrapped.
inline std::vector<double> Power(const std::vector<double>& img, int n, bool hann) {
  double mean = 0.0;
  for (double v : img) {
    mean += v;
  }
  mean /= double(img.size());
  std::vector<Cx> a(img.size());
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const double w = hann ? 0.25 * (1.0 - std::cos(2.0 * 3.14159265358979323846 * (x + 0.5) / n)) * (1.0 - std::cos(2.0 * 3.14159265358979323846 * (y + 0.5) / n)) : 1.0;
      a[size_t(y) * size_t(n) + size_t(x)] = (img[size_t(y) * size_t(n) + size_t(x)] - mean) * w;
    }
  }
  for (int y = 0; y < n; ++y) {
    std::vector<Cx> row(a.begin() + y * n, a.begin() + (y + 1) * n);
    Fft(row);
    std::copy(row.begin(), row.end(), a.begin() + y * n);
  }
  for (int x = 0; x < n; ++x) {
    std::vector<Cx> col(static_cast<size_t>(n));
    for (int y = 0; y < n; ++y) {
      col[size_t(y)] = a[size_t(y) * size_t(n) + size_t(x)];
    }
    Fft(col);
    for (int y = 0; y < n; ++y) {
      a[size_t(y) * size_t(n) + size_t(x)] = col[size_t(y)];
    }
  }
  std::vector<double> p(img.size());
  for (size_t i = 0; i < img.size(); ++i) {
    p[i] = std::norm(a[i]);
  }
  return p;
}

struct Shape {
  double total = 0.0;      // the energy of every bin but DC
  double peak_share = 0.0; // the largest bin's share of it: a lattice's peak
  double peak_abs = 0.0;   // the largest bin's energy, per pixel of the image
  double band_share = 0.0; // the share in the band the eye sees as pattern (kBandLo to kBandHi cycles a pixel)
  double wedge_ratio = 0.0;// the band's energy in its strongest of 12 directions over the mean of them: 1 is no direction
  double low_energy = 0.0; // the energy below kLowEnd cycles a pixel
};
inline constexpr double kBandLo = 0.03, kBandHi = 0.25, kLowEnd = 0.12;

inline Shape Analyse(const std::vector<double>& power, int n) {
  Shape s;
  constexpr int kWedges = 12;
  std::vector<double> wedge(kWedges, 0.0);
  double peak = 0.0, band = 0.0;
  for (int ky = 0; ky < n; ++ky) {
    for (int kx = 0; kx < n; ++kx) {
      if (kx == 0 && ky == 0) {
        continue;
      }
      const int fx = kx > n / 2 ? kx - n : kx, fy = ky > n / 2 ? ky - n : ky;
      const double f = std::hypot(double(fx), double(fy)) / double(n);
      const double e = power[size_t(ky) * size_t(n) + size_t(kx)];
      s.total += e;
      peak = std::max(peak, e);
      if (f < kLowEnd) {
        s.low_energy += e;
      }
      if (f > kBandLo && f < kBandHi) {
        double ang = std::atan2(double(fy), double(fx));
        if (ang < 0.0) {
          ang += 3.14159265358979323846;
        }
        if (ang >= 3.14159265358979323846) {
          ang -= 3.14159265358979323846;
        }
        wedge[size_t(std::min(kWedges - 1, int(ang / 3.14159265358979323846 * kWedges)))] += e;
        band += e;
      }
    }
  }
  s.peak_share = s.total > 0.0 ? peak / s.total : 0.0;
  s.peak_abs = peak / double(n) / double(n);
  s.band_share = s.total > 0.0 ? band / s.total : 0.0;
  s.wedge_ratio = band > 0.0 ? *std::max_element(wedge.begin(), wedge.end()) / (band / kWedges) : 0.0;
  return s;
}


inline double Ign(double x, double y) {
  double t = 0.06711056 * x + 0.00583715 * y;
  t -= std::floor(t);
  t *= 52.9829189;
  return t - std::floor(t);
}

// The 3x3 Gaussian of the occlusion's first blur (sigma 0.9) on a periodic image.
inline std::vector<double> Blur3(const std::vector<double>& img, int n) {
  std::vector<double> out(img.size());
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      double s = 0.0, w = 0.0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const double k = std::exp(-0.5 * double(dx * dx + dy * dy) / (0.9 * 0.9));
          s += k * img[size_t((y + dy + n) % n) * size_t(n) + size_t((x + dx + n) % n)];
          w += k;
        }
      }
      out[size_t(y) * size_t(n) + size_t(x)] = s / w;
    }
  }
  return out;
}

}  // namespace blue_noise_spectrum
