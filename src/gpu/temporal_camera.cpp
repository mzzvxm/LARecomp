// MCLA GPU renderer -- the camera of a frame. See temporal_camera.h.

#include "temporal_camera.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "records.h"

namespace mcla::gpu::temporal {

namespace {

constexpr double kPi = 3.14159265358979323846;

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

// The largest absolute entry of row `r`.
double RowMax(const Mat4& a, int r) {
  double worst = 0.0;
  for (int c = 0; c < 4; ++c) {
    worst = std::fmax(worst, std::fabs(a.m[r][c]));
  }
  return worst;
}

// Guest clip space to guest pixels as the game's c69 block applies it (the census, section 1.1,
// CHECK 3): x_pix = w/2 x + w/2 clip_w, y_pix = h/2 clip_w - h/2 y, depth and w columns kept.
Mat4 ClipToPixels(double width, double height) {
  Mat4 m;
  m.m[0][0] = width * 0.5;
  m.m[1][1] = -height * 0.5;
  m.m[2][2] = 1.0;
  m.m[3][0] = width * 0.5;
  m.m[3][1] = height * 0.5;
  m.m[3][3] = 1.0;
  return m;
}

// A previous-frame matrix that can be the game's: finite, and neither the denormals nor the huge
// numbers the slots of absent vehicles hold.
// CHECK: the bounds (the garbage seen was NaN, 2.2e29 and 1.4e-45; real entries are in the tens
// to millions of pixels), not measured on a driven car.
bool SanePrevious(const Mat4& a) {
  if (!Finite(a)) {
    return false;
  }
  const double biggest = MaxAbs(a);
  return biggest >= 1e-3 && biggest <= 1e12;
}

}  // namespace

const char* RejectName(Reject reject) {
  switch (reject) {
    case Reject::kNone: return "none";
    case Reject::kFrameSize: return "c62 is not the size of the anchor's fetches";
    case Reject::kConstants: return "c4-c15 are not finite";
    case Reject::kViewProjection: return "the view-projection has no inverse";
    case Reject::kViewInverse: return "view times view inverse is not the identity";
    case Reject::kLens: return "the projection has no sane near, far or field of view";
    case Reject::kCount: break;
  }
  return "?";
}

Reject ExtractCamera(const uint8_t* ps_bank, uint32_t fetch_width, uint32_t fetch_height,
                     uint64_t frame, uint64_t stencil_ids, Camera* out) {
  *out = Camera{};
  Camera& cam = *out;
  cam.frame = frame;
  const uint8_t* b = ps_bank;

  cam.width = fetch_width;
  cam.height = fetch_height;
  if (std::fabs(double(Reg(b, 62, 0)) - double(fetch_width)) >= 0.5 ||
      std::fabs(double(Reg(b, 62, 1)) - double(fetch_height)) >= 0.5) {
    return Reject::kFrameSize;
  }

  cam.view = ReadMat(b, 4);
  cam.view_projection = ReadMat(b, 8);
  cam.view_inverse = ReadMat(b, 12);
  if (!Finite(cam.view) || !Finite(cam.view_projection) || !Finite(cam.view_inverse)) {
    return Reject::kConstants;
  }
  if (!Inverse(cam.view_projection, &cam.view_projection_inverse) ||
      MaxAbsDiff(Mul(cam.view_projection, cam.view_projection_inverse), Identity()) >= 1e-4) {
    return Reject::kViewProjection;
  }
  // View times its inverse is the identity up to the float rounding of the two, which grows with
  // the translation (the title camera stands at x = 2903: the census saw an absolute 1e-3 fail in
  // 5% of the frames): the bound follows the translation rows.
  const double translation = std::max({1.0, RowMax(cam.view, 3), RowMax(cam.view_inverse, 3)});
  if (MaxAbsDiff(Mul(cam.view, cam.view_inverse), Identity()) >= 1e-5 * translation + 1e-3) {
    return Reject::kViewInverse;
  }

  // The camera: position and axes from the view inverse. The projection puts the near plane at
  // view z = -5 (census 1.2), so the camera looks along -z and c14 is the backward axis.
  for (int i = 0; i < 3; ++i) {
    cam.right[i] = cam.view_inverse.m[0][i];
    cam.up[i] = cam.view_inverse.m[1][i];
    cam.forward[i] = -cam.view_inverse.m[2][i];
    cam.position[i] = cam.view_inverse.m[3][i];
  }

  // The projection, view space to clip: P = inverse(V) * VP in row vectors. A point at view-space
  // z has clip z = z P22 + P32 and clip w = z P23 + P33. The game's depth is 1 near and 0 far, so
  // the near plane is where clip z = 0 and the far plane where clip z = clip w (the census read
  // near 5 and far about 10000). The matrices are float32 with far / near about 2000: the
  // arithmetic is in double and far is only good to a few units.
  // CHECK: that the host depth texture holds the same convention (census CHECK 2).
  cam.projection = Mul(cam.view_inverse, cam.view_projection);
  const Mat4& p = cam.projection;
  if (!(std::fabs(p.m[1][1]) > 0.0) || !(std::fabs(p.m[0][0]) > 0.0) || p.m[2][2] == 0.0 ||
      (p.m[2][3] - p.m[2][2]) == 0.0) {
    return Reject::kLens;
  }
  cam.fov_y = 2.0 * std::atan(1.0 / std::fabs(p.m[1][1]));
  cam.aspect = std::fabs(p.m[1][1] / p.m[0][0]);
  const double z_clip_0 = -p.m[3][2] / p.m[2][2];                                 // clip z = 0
  const double z_clip_1 = (p.m[3][2] - p.m[3][3]) / (p.m[2][3] - p.m[2][2]);      // clip z = w
  cam.near_plane = -z_clip_0;
  cam.far_plane = -z_clip_1;
  if (!std::isfinite(cam.fov_y) || !(cam.fov_y > 0.0) || !(cam.fov_y < kPi) ||
      !std::isfinite(cam.aspect) || !(cam.near_plane > 0.0) ||
      !(cam.far_plane > cam.near_plane) || !std::isfinite(cam.far_plane)) {
    return Reject::kLens;
  }

  // c64-c67 against pixel-to-clip * inverse(VP): a free cross-check of the pixel convention.
  cam.pixel_to_world = ReadMat(b, 64);
  Mat4 pixels_inverse;
  if (Finite(cam.pixel_to_world) &&
      Inverse(ClipToPixels(double(fetch_width), double(fetch_height)), &pixels_inverse)) {
    const Mat4 expected = Mul(pixels_inverse, cam.view_projection_inverse);
    bool agrees = true;
    for (int r = 2; r < 4; ++r) {
      const double bound = 1e-3 * RowMax(expected, r) + 1e-9;
      for (int c = 0; c < 4; ++c) {
        agrees &= std::fabs(cam.pixel_to_world.m[r][c] - expected.m[r][c]) <= bound;
      }
    }
    cam.pixel_to_world_agrees = agrees;
  }

  // The previous frame's matrices, only of the stencil IDs the main view wrote (the world's, k = 0,
  // always) and only when sane.
  cam.stencil_ids = stencil_ids;
  for (uint32_t k = 0; k < kVehicleMatrices; ++k) {
    if (k != kWorldId && !(stencil_ids & (uint64_t(1) << k))) {
      continue;
    }
    const Mat4 raw = ReadMat(b, 69 + 4 * k);
    if (!SanePrevious(raw)) {
      continue;
    }
    const Mat4 combined = Mul(cam.view_projection_inverse, raw);
    if (!Finite(combined)) {
      continue;
    }
    if (k == kWorldId) {
      cam.previous_world = raw;
    }
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        cam.clip_to_previous_pixels[k][r * 4 + c] = float(combined.m[r][c]);
      }
    }
    cam.previous_valid |= 1u << k;
  }
  return Reject::kNone;
}

Cut TestCut(const Camera& now, const Camera& before) {
  double dist2 = 0.0;
  double dot = 0.0;
  double n_now = 0.0;
  double n_before = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double d = now.position[i] - before.position[i];
    dist2 += d * d;
    dot += now.forward[i] * before.forward[i];
    n_now += now.forward[i] * now.forward[i];
    n_before += before.forward[i] * before.forward[i];
  }
  Cut cut;
  cut.distance = std::sqrt(dist2);
  cut.cosine = dot / std::max(1e-30, std::sqrt(n_now * n_before));
  cut.fov_change = std::fabs(now.fov_y - before.fov_y) / std::max(1e-30, before.fov_y);
  cut.fires = cut.distance > kCutDistance || cut.cosine < kCutCosine || cut.fov_change > kCutFov;
  return cut;
}

History CompareToHistory(const Camera& now, const Camera& before) {
  const Mat4 q = Mul(before.view_projection_inverse, now.previous_world);
  const double w = double(now.width);
  const double h = double(now.height);
  Mat4 flip = ClipToPixels(w, h);
  Mat4 noflip = flip;
  noflip.m[1][1] = h * 0.5;
  // x, y and w only: the columns the motion vectors use (z may be scaled otherwise).
  const auto error = [&](const Mat4& expected) {
    double worst = 0.0;
    for (int i = 0; i < 4; ++i) {
      for (int j : {0, 1, 3}) {
        worst = std::fmax(worst, std::fabs(q.m[i][j] - expected.m[i][j]));
      }
    }
    return worst;
  };
  History history;
  history.flip = error(flip);
  history.noflip = error(noflip);
  return history;
}

double Halton(uint32_t index, uint32_t base) {
  double f = 1.0;
  double r = 0.0;
  while (index > 0) {
    f /= double(base);
    r += f * double(index % base);
    index /= base;
  }
  return r;
}

}  // namespace mcla::gpu::temporal
