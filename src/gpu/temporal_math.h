#pragma once
// 4x4 matrices in double precision for the temporal system (temporal_camera.cpp). The game's
// shader constants are row vectors (v' = v M, one register per row), so a product A * B applies A
// first.

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace mcla::gpu::temporal {

struct Mat4 {
  double m[4][4] = {};
};

inline Mat4 Identity() {
  Mat4 r;
  for (int i = 0; i < 4; ++i) {
    r.m[i][i] = 1.0;
  }
  return r;
}

// v' = v A B.
inline Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[i][k] * b.m[k][j];
      }
      r.m[i][j] = sum;
    }
  }
  return r;
}

inline bool Finite(const Mat4& a) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (!std::isfinite(a.m[i][j])) {
        return false;
      }
    }
  }
  return true;
}

// The largest absolute entry.
inline double MaxAbs(const Mat4& a) {
  double worst = 0.0;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      worst = std::fmax(worst, std::fabs(a.m[i][j]));
    }
  }
  return worst;
}

inline double MaxAbsDiff(const Mat4& a, const Mat4& b) {
  double worst = 0.0;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      worst = std::fmax(worst, std::fabs(a.m[i][j] - b.m[i][j]));
    }
  }
  return worst;
}

// Gauss-Jordan with partial pivoting. False for a singular matrix (or one with a NaN).
inline bool Inverse(const Mat4& a, Mat4* out) {
  double w[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      w[r][c] = a.m[r][c];
      w[r][4 + c] = r == c ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(w[r][col]) > std::fabs(w[pivot][col])) {
        pivot = r;
      }
    }
    if (!(std::fabs(w[pivot][col]) > 1e-300)) {
      return false;
    }
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) {
        std::swap(w[pivot][c], w[col][c]);
      }
    }
    const double d = w[col][col];
    for (int c = 0; c < 8; ++c) {
      w[col][c] /= d;
    }
    for (int r = 0; r < 4; ++r) {
      if (r == col) {
        continue;
      }
      const double f = w[r][col];
      if (f != 0.0) {
        for (int c = 0; c < 8; ++c) {
          w[r][c] -= f * w[col][c];
        }
      }
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out->m[r][c] = w[r][4 + c];
    }
  }
  return Finite(*out);
}

// "[a b c d | e f g h | ...]", one register per row.
inline std::string Text(const Mat4& a) {
  std::string s = "[";
  for (int i = 0; i < 4; ++i) {
    s += std::format("{}{:.6g} {:.6g} {:.6g} {:.6g}", i ? " | " : "", a.m[i][0], a.m[i][1],
                     a.m[i][2], a.m[i][3]);
  }
  return s + "]";
}

}  // namespace mcla::gpu::temporal
