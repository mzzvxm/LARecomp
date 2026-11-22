#pragma once
// ===========================================================================
// MCLA GPU renderer -- the checks every stochastic effect's noise runs (a test helper, no GPU, no renderer; NOISE-AUDIT, perf\NOISE-AUDIT.md)
// ===========================================================================
// AGENT-RULE-NOISE: a stochastic effect's noise changes every frame, resolves under a filter whose kernel covers it, and is never a fixed per-pixel lattice or a pattern that slides with the
// camera. Two measures say it for a noise function `noise(x, y, frame)` that gives the two uniform numbers of a pixel (the shader's own function, compiled as C++ through hlsl_shim.h):
//   PerFrameOf  the numbers change from frame to frame at (almost) every pixel; a pixel's numbers over 16 frames leave no gap over 0.25; they are uniform; and the step from one frame to the next
//               is the same for every pixel (the picture of the noise is moved round the circle: nothing slides across the screen)
//   Resolve     the error an exponential average over frames (weight 0.9, or what the effect keeps) leaves of a smooth response to the two numbers, after an optional 3 x 3 blur: how much of it is
//               in ONE spatial frequency and how unevenly it spreads over the directions (a lattice reads as lines), and whether the next frame's picture is this one's moved by a few
//               pixels (a pattern that crawls)
// The thresholds the effects' tests use are the ones blue_noise's test and ssr_test.cpp set for the first fixes: a lattice has over 2% of its error in one frequency or a direction 2.5 times
// the mean; the blue noise has under 1.2% and 2.4 times, and no shift matches the next frame (under 0.35). They are for the average over frames alone: after a blur of a few pixels the energy that
// is left sits in few bins whatever made it, so a blurred picture is judged by the effect's own test.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "blue_noise_spectrum.h"

namespace noise_check {

using Noise2 = std::function<std::pair<float, float>(int x, int y, uint32_t frame)>;
using Response = std::function<double(float u1, float u2)>;

inline std::string Num(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}

struct Checks {
  int total = 0;
  int fails = 0;
  bool print = false;
  void operator()(bool ok, const std::string& what) {
    ++total;
    if (!ok) {
      ++fails;
    }
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
  }
};

struct Resolved {
  blue_noise_spectrum::Shape shape;
  double rms = 0.0;
  double zero_corr = 0.0;   // the next frame's picture against this one's, in place
  double best_corr = 0.0;   // ... and at the best shift of up to 8 pixels
  int best_dx = 0;
  int best_dy = 0;
};

// The error image of frame `t`: the exponential mean of the response over `history` frames up to t, the picture's mean left in (the shape measures take it out).
inline std::vector<double> ErrorImage(const Noise2& noise, const Response& response, uint32_t t, int n, int history, double weight, bool blur3) {
  std::vector<double> acc(size_t(n) * size_t(n), 0.0);
  double w = 1.0 - weight, norm = 0.0;
  for (int k = 0; k < history; ++k) {
    std::vector<double> img(acc.size());
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        const std::pair<float, float> u = noise(x, y, t - uint32_t(k));
        img[size_t(y) * size_t(n) + size_t(x)] = response(u.first, u.second);
      }
    }
    if (blur3) {
      img = blue_noise_spectrum::Blur3(img, n);
    }
    for (size_t i = 0; i < acc.size(); ++i) {
      acc[i] += w * img[i];
    }
    norm += w;
    w *= weight;
  }
  for (double& v : acc) {
    v /= norm;
  }
  return acc;
}

inline Resolved Resolve(const Noise2& noise, const Response& response, uint32_t t = 100, int n = 128, int history = 48, double weight = 0.9, bool blur3 = false) {
  const std::vector<double> a = ErrorImage(noise, response, t, n, history, weight, blur3);
  const std::vector<double> b = ErrorImage(noise, response, t + 1, n, history, weight, blur3);
  Resolved r;
  r.shape = blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(a, n, true), n);
  double mean = 0.0;
  for (double v : a) {
    mean += v;
  }
  mean /= double(a.size());
  for (double v : a) {
    r.rms += (v - mean) * (v - mean);
  }
  r.rms = std::sqrt(r.rms / double(a.size()));
  const int m = 12;
  for (int dy = -8; dy <= 8; ++dy) {
    for (int dx = -8; dx <= 8; ++dx) {
      double sab = 0.0, saa = 0.0, sbb = 0.0, ma = 0.0, mb = 0.0;
      int cnt = 0;
      for (int y = m; y < n - m; ++y) {
        for (int x = m; x < n - m; ++x) {
          ma += a[size_t(y) * size_t(n) + size_t(x)];
          mb += b[size_t(y + dy) * size_t(n) + size_t(x + dx)];
          ++cnt;
        }
      }
      ma /= cnt;
      mb /= cnt;
      for (int y = m; y < n - m; ++y) {
        for (int x = m; x < n - m; ++x) {
          const double va = a[size_t(y) * size_t(n) + size_t(x)] - ma, vb = b[size_t(y + dy) * size_t(n) + size_t(x + dx)] - mb;
          sab += va * vb;
          saa += va * va;
          sbb += vb * vb;
        }
      }
      const double c = std::fabs(sab / std::sqrt(std::max(saa * sbb, 1e-30)));
      if (dx == 0 && dy == 0) {
        r.zero_corr = c;
      }
      if (c > r.best_corr) {
        r.best_corr = c;
        r.best_dx = dx;
        r.best_dy = dy;
      }
    }
  }
  return r;
}

inline void Print(const char* what, const Resolved& r) {
  std::printf("  %-34s rms %.4f, one frequency %.4f of it, strongest direction %.2f x the mean, best match with the next frame at (%d, %d): %.2f (in place %.2f)\n", what, r.rms, r.shape.peak_share,
              r.shape.wedge_ratio, r.best_dx, r.best_dy, r.best_corr, r.zero_corr);
}

// The shape every blue noise path must have after the resolve (the thresholds above), and that the first version's has not.
inline bool NoLattice(const Resolved& r, double wedge_limit = 2.4) { return r.shape.peak_share < 0.012 && r.shape.wedge_ratio < wedge_limit; }
// A pattern that crawls: the next frame's picture is this one's moved by a few pixels (a match over 0.5 at a shift other than none). A picture that stays where it is, whatever it holds, does not crawl.
inline bool Crawls(const Resolved& r) { return (r.best_dx != 0 || r.best_dy != 0) && r.best_corr > 0.5; }
inline bool IsLattice(const Resolved& r) { return r.shape.peak_share > 0.02 || r.shape.wedge_ratio > 2.5; }

struct PerFrame {
  double changed = 0.0;      // the share of (pixel, frame) pairs whose number differs from the frame before
  double max_gap = 0.0;      // the widest gap of a pixel's numbers over 16 frames on the circle, the worst pixel
  double mean = 0.0;         // over a 64 x 64 picture at one frame
  double var = 0.0;
  double step_spread = 0.0;  // how far apart (on the circle) two pixels' steps from one frame to the next are, the worst pair: 0 is a picture moved rigidly round the circle
};

inline double CircleDistance(double a, double b) {
  double d = std::fabs(a - b);
  d -= std::floor(d);
  return std::min(d, 1.0 - d);
}

// `channel` 0 or 1: which of the two numbers.
inline PerFrame PerFrameOf(const Noise2& noise, int channel, uint32_t first_frame = 100) {
  const auto at = [&](int x, int y, uint32_t f) {
    const std::pair<float, float> u = noise(x, y, f);
    return double(channel == 0 ? u.first : u.second);
  };
  PerFrame r;
  int pairs = 0, changed = 0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      for (uint32_t f = first_frame; f < first_frame + 8; ++f) {
        ++pairs;
        changed += std::fabs(at(x, y, f + 1) - at(x, y, f)) > 1e-6 ? 1 : 0;
      }
    }
  }
  r.changed = double(changed) / double(pairs);
  for (int p = 0; p < 24; ++p) {
    const int x = p * 5 + 1, y = p * 3 + 2;
    std::vector<double> v;
    for (uint32_t f = first_frame; f < first_frame + 16; ++f) {
      v.push_back(at(x, y, f));
    }
    std::sort(v.begin(), v.end());
    double gap = v.front() + 1.0 - v.back();
    for (size_t i = 1; i < v.size(); ++i) {
      gap = std::max(gap, v[i] - v[i - 1]);
    }
    r.max_gap = std::max(r.max_gap, gap);
  }
  double sum = 0.0, sum2 = 0.0;
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const double v = at(x, y, first_frame);
      sum += v;
      sum2 += v * v;
    }
  }
  r.mean = sum / 4096.0;
  r.var = sum2 / 4096.0 - r.mean * r.mean;
  const double step0 = at(0, 0, first_frame + 1) - at(0, 0, first_frame);
  for (int y = 0; y < 48; y += 3) {
    for (int x = 0; x < 48; x += 3) {
      r.step_spread = std::max(r.step_spread, CircleDistance(at(x, y, first_frame + 1) - at(x, y, first_frame), step0));
    }
  }
  return r;
}

inline void Print(const char* what, const PerFrame& p) {
  std::printf("  %-34s changed %.3f, widest gap in 16 frames %.3f, mean %.3f, variance %.4f, step spread %.5f\n", what, p.changed, p.max_gap, p.mean, p.var, p.step_spread);
}

// The shortest period P (2 .. max_period) with which the noise repeats over `frames` frames (every probed pixel's two numbers at frame f + P equal those at f for every f), or 0 for none (NOISE-FOLLOW). A frame term
// that wraps after 8 or 16 frames is not a fault of the per-frame checks (it changes every frame, 16 values are spread) but its line in the spectrum survives an exponential average: at a weight of 0.75 a third
// of a period-8 term, at 0.9 a quarter of a period-16 term, as a shimmer of 7.5 / 3.75 Hz at 60 fps. The effects' frame terms count to 1024 (`& 1023`) and no period under max_period shows.
inline uint32_t ShortestPeriod(const Noise2& noise, uint32_t first_frame = 100, uint32_t frames = 512, uint32_t max_period = 128) {
  static const int kProbe[5][2] = {{3, 5}, {41, 77}, {100, 20}, {7, 63}, {18, 18}};
  std::vector<float> a[5][2];
  for (int p = 0; p < 5; ++p) {
    for (uint32_t f = 0; f < frames + max_period; ++f) {
      const std::pair<float, float> u = noise(kProbe[p][0], kProbe[p][1], first_frame + f);
      a[p][0].push_back(u.first);
      a[p][1].push_back(u.second);
    }
  }
  for (uint32_t period = 2; period <= max_period; ++period) {
    bool repeats = true;
    for (int p = 0; p < 5 && repeats; ++p) {
      for (int c = 0; c < 2 && repeats; ++c) {
        for (uint32_t f = 0; f < frames; ++f) {
          if (std::fabs(a[p][c][f + period] - a[p][c][f]) > 1e-6f) {
            repeats = false;
            break;
          }
        }
      }
    }
    if (repeats) {
      return period;
    }
  }
  return 0;
}

// The reading of a source file of the tree a test is built from: `dir` is the src/gpu folder (the program's first argument, else the folder of the test's own source).
inline std::string ReadFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return std::string();
  }
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    s.append(buf, n);
  }
  std::fclose(f);
  return s;
}

inline std::string DirOf(const char* file) {
  std::string s(file);
  const size_t at = s.find_last_of("/\\");
  return at == std::string::npos ? std::string(".") : s.substr(0, at);
}

inline bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace noise_check
