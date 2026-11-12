// MCLA GPU renderer -- the motion of vehicles for the vectors. See vehicle_motion.h.

#include "vehicle_motion.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "records.h"
#include "temporal_math.h"

namespace mcla::gpu::temporal {

namespace {

// How far a slot may be from a rigid transform. The census of the real game saw, over driven stretches
// of some 120,000 slot-frames, a determinant of 0.99995 to 1.00013, an orthonormality error of 1e-7 to
// 3e-4 and a last column error of 1e-7 to 4e-4 (float32 entries of 1e5 to 1e6); the slots of a car placed
// in that frame hold garbage a long way off (a last column error of 33).
constexpr double kRigidTolerance = 2e-3;

float Reg(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

// Four registers from `first`, one per row.
Mat4 ReadMat(const uint8_t* bank, uint32_t first) {
  Mat4 m;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      m.m[r][c] = double(Reg(bank, first + r, c));
    }
  }
  return m;
}

// A previous-frame matrix that can be the game's: finite, and neither the denormals nor the huge
// numbers the slots of absent vehicles hold (the garbage seen was NaN, 2.2e29 and 1.4e-45; real entries
// are in the tens to millions of pixels).
bool SanePrevious(const Mat4& a) {
  if (!Finite(a)) {
    return false;
  }
  const double biggest = MaxAbs(a);
  return biggest >= 1e-3 && biggest <= 1e12;
}

// A rigid transform of row vectors: the rotation block has orthonormal rows and determinant 1, and the
// last column is (0, 0, 0, 1).
bool IsRigid(const Mat4& t) {
  double last = std::fabs(t.m[3][3] - 1.0);
  for (int r = 0; r < 3; ++r) {
    last = std::fmax(last, std::fabs(t.m[r][3]));
  }
  double orthonormal = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double dot = 0.0;
      for (int k = 0; k < 3; ++k) {
        dot += t.m[i][k] * t.m[j][k];
      }
      orthonormal = std::fmax(orthonormal, std::fabs(dot - (i == j ? 1.0 : 0.0)));
    }
  }
  const double determinant =
      t.m[0][0] * (t.m[1][1] * t.m[2][2] - t.m[1][2] * t.m[2][1]) -
      t.m[0][1] * (t.m[1][0] * t.m[2][2] - t.m[1][2] * t.m[2][0]) +
      t.m[0][2] * (t.m[1][0] * t.m[2][1] - t.m[1][1] * t.m[2][0]);
  return last <= kRigidTolerance && orthonormal <= kRigidTolerance &&
         std::fabs(determinant - 1.0) <= kRigidTolerance;
}

}  // namespace

uint32_t StencilIdsWritten(const XenosState& s) {
  if (!s.depth_bound || !s.depth_control.stencil_enable) {
    return 0;
  }
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  uint32_t ids = 0;
  const auto add = [&ids](uint32_t reference) {
    const uint32_t id = (reference & 0xFFu) >> 2;
    if (id < kVehicleIds) {
      ids |= 1u << id;
    }
  };
  if ((s.stencil.stencilwritemask & 0xFFu) != 0 && dc.stencilzpass == xe::StencilOp::kReplace) {
    add(s.stencil.stencilref);
  }
  if (dc.backface_enable && (s.stencil_bf.stencilwritemask & 0xFFu) != 0 &&
      dc.stencilzpass_bf == xe::StencilOp::kReplace) {
    add(s.stencil_bf.stencilref);
  }
  return ids;
}

VehicleStats BuildVehicleTable(const uint8_t* ps_bank, uint32_t ids, const Camera& now,
                               const Camera& before, VehicleTable* out) {
  *out = VehicleTable{};
  VehicleStats stats;
  const uint32_t vehicles = ids & ~1u & ((1u << kVehicleIds) - 1u);
  if (vehicles == 0) {
    return stats;
  }
  stats.asked = uint32_t(std::popcount(vehicles));

  // The world's own previous matrix: what every vehicle's is taken relative to.
  const Mat4 world = ReadMat(ps_bank, kVehicleFirstRegister);
  Mat4 world_inverse;
  if (!SanePrevious(world) || !Inverse(world, &world_inverse)) {
    stats.no_world = true;
    return stats;
  }

  // This frame's clip space to the world, and the previous frame's world to its guest pixels, both
  // from this renderer's own cameras (the world's own motion is the camera term of temporal_mv.h).
  const Mat4 to_world = now.view_projection_inverse;
  const Mat4 from_world =
      Mul(before.view_projection, ClipToPixels(double(now.width), double(now.height)));

  for (uint32_t k = 1; k < kVehicleIds; ++k) {
    if (!(vehicles & (1u << k))) {
      continue;
    }
    const Mat4 slot = ReadMat(ps_bank, kVehicleFirstRegister + 4 * k);
    if (!SanePrevious(slot)) {
      ++stats.not_sane;
      continue;
    }
    // The vehicle's own motion since the game's previous frame, in world coordinates: the world's
    // matrix cancels, whatever it is.
    const Mat4 motion = Mul(slot, world_inverse);
    if (!IsRigid(motion)) {
      ++stats.not_rigid;
      continue;
    }
    const Mat4 c = Mul(Mul(to_world, motion), from_world);
    if (!Finite(c)) {
      ++stats.not_finite;
      continue;
    }
    // Columns x, y and w, one float4 each (the same as temporal_mv.cpp SetColumns).
    float* columns = out->columns[k];
    for (int row = 0; row < 4; ++row) {
      columns[0 + row] = float(c.m[row][0]);
      columns[4 + row] = float(c.m[row][1]);
      columns[8 + row] = float(c.m[row][3]);
    }
    out->valid |= 1u << k;
    ++stats.taken;
  }
  return stats;
}

}  // namespace mcla::gpu::temporal
