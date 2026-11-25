#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side mirrors' glass overlay: the rules that need no renderer (perf/MIRROR-VIEW.md, package MV4)
// ===========================================================================
// WHAT THE OVERLAY IS. The player's two side mirror glasses, drawn again into the main colour at the end of the scene, after the HUD stamp, with a pixel shader that reflects the eye's ray off the
// glass plane and reads the shared rear view (mirror_view.h) where the reflected ray goes: a planar mirror lookup (mirror_view_rules.h UvForMirrorPixel, the reference this file's math is checked
// against). Where the ray traced side mirrors (car_mirror_rt.h) are on, they win and the overlay does nothing (Decide). Everything here is plain numbers: mirror_overlay.cpp and scene.cpp only ask
// these functions and do what they answer; hlsl/mirror_overlay.hlsli is the shader's own text of the same arithmetic (mirror_overlay_rules_test.cpp compiles it as C++ and holds the two together).
//
// THE SPACE THE SHADER WORKS IN. The main camera's own view space, EYE-RELATIVE (the matrix Frame::view: x right, y up, z forward = the distance along the view, the origin the eye): a point of the
// glass is `pv` there, so reflecting `normalize(pv)` about the glass's normal needs no world coordinates (the world is kilometres from its origin: nothing is subtracted in float). The rig of the
// shared view is that same space turned half a circle about its up axis (sub_82307F28 negates the camera's right and forward rows): a direction (x, y, z) in the main camera's axes is (-x, y, -z)
// in the rig's. A view held for a frame was drawn from a camera that has moved on: the pose keeps that camera's eye and axes (ViewPose) and the push block carries what the lookup needs of them
// (the eye's offset and the rotation from the main camera's axes to the rig's, a quaternion).
//
// THE PUSH BLOCK (128 bytes, the most every Vulkan driver takes: AMD's and Intel's maximum), both stages: eight float4s.
//   0..2  col_x, col_y, col_z   the glass's model space -> `pv` : pv.k = dot(float4(position, 1), col_k)
//   3     proj_a                x' = pv.x * a.x + pv.z * a.y ; y' = pv.y * a.z + pv.z * a.w          (the game's clip space with the draw's viewport transform and the frame's jitter folded in)
//   4     proj_b                z' = pv.z * b.x + b.y ; w' = pv.z ;  b.z, b.w tan of half the rig's horizontal and vertical field
//   5     normal                the glass's unit normal in the main camera's view space (the way it faces: a reflection does not care), w = the assumed distance t0 beyond the glass
//   6     delta                 the main camera's eye minus the rig's, in the main camera's axes (0 for a view drawn this frame), w = the picture's bindless slot (bits)
//   7     rot                   the quaternion (x, y, z, w) turning a direction of the main camera's axes into the rig's
// The game's clip space is sparse in the view space of Frame::view (a perspective projection: x' and y' take one axis and the depth, z' the depth, w' the depth): MakePush checks that of the
// matrices it is given and refuses a camera whose projection is not (Why::kProjection) rather than draw a mirror in the wrong place.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "car_shadow_map_rules.h"
#include "mirror_glass_need.h"
#include "mirror_view_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::mirror_overlay {

namespace mv = mirror_view;
namespace tp = temporal;

// The numbers of the shader (hlsl/mirror_overlay.hlsli holds the same two, the test checks the text).
inline constexpr double kEdgeFade = 0.04;     // the last share of the picture, on each side, that fades to the game's own glass
inline constexpr double kBiasMetres = 0.005;  // the overlay is pulled this far towards the eye along the view ray: it lands on the glass the game drew, not behind it (same pixel, nearer depth)
inline constexpr uint32_t kPushBytes = 128;
inline constexpr double kMinDistance = 3.0;    // the assumed distance t0's range (the debug setting is held to it)
inline constexpr double kMaxDistance = 200.0;
inline constexpr uint32_t kMaxStride = 4096;   // a vertex stride the overlay's pipeline key can hold (the car's are 12 to 36 bytes)

inline double ClampDistance(double t0) { return std::isfinite(t0) ? std::clamp(t0, kMinDistance, kMaxDistance) : mv::kAssumedDistance; }

// ---------------------------------------------------------------------------
// what the overlay does this frame
// ---------------------------------------------------------------------------

enum class Why : uint8_t {
  kDraw,          // the glass is drawn
  kSettingOff,    // the overlay is not wanted: the SIDE MIRRORS row's view is not running and mcla_gpu_mirror_overlay is off
  kRtMirrors,     // the ray traced side mirrors are on: they win
  kNoPicture,     // the shared view has no whole picture (no phase ran, the images failed)
  kStalePicture,  // the picture is older than a held frame may be
  kNotPlayView,   // the pause view or the garage: the game's mirrors
  kNoGlass,       // the finder has no pair of glasses this frame
  kNotNeeded,     // a pair, but no glass is on the picture and wide enough to be worth it
  kNoField,       // the view's field of view is not known: the lookup cannot be made
  kNoPose,        // the camera the view was drawn from is not known
  kNoCamera,      // this frame has no usable camera (the car's draws carry none)
  kProjection,    // the camera's projection is not the perspective the shader's coefficients hold
  kDepthFunc,     // the glass draw's depth test is not one the overlay can repeat
  kTargets,       // the main colour or depth is not there at the copy
  kGeometry,      // the glass draw's host geometry is not usable
  kNotReady,      // a pipeline is still being made (never waited for)
  kCount
};

inline const char* WhyName(Why w) {
  switch (w) {
    case Why::kDraw: return "drawn";
    case Why::kSettingOff: return "setting off";
    case Why::kRtMirrors: return "the ray traced side mirrors are on";
    case Why::kNoPicture: return "no picture";
    case Why::kStalePicture: return "picture too old";
    case Why::kNotPlayView: return "not the play view";
    case Why::kNoGlass: return "no pair of glasses";
    case Why::kNotNeeded: return "no glass needed";
    case Why::kNoField: return "field of view unknown";
    case Why::kNoPose: return "view's camera unknown";
    case Why::kNoCamera: return "no camera this frame";
    case Why::kProjection: return "projection not a perspective";
    case Why::kDepthFunc: return "depth test not repeatable";
    case Why::kTargets: return "main targets missing";
    case Why::kGeometry: return "glass geometry unusable";
    case Why::kNotReady: return "pipeline not made yet";
    case Why::kCount: break;
  }
  return "?";
}

struct Facts {
  bool setting_on = false;       // mirror_setting::Current().overlay: the SIDE MIRRORS row's view runs, or mcla_gpu_mirror_overlay is on
  bool rt_mirrors_on = false;    // the ray traced side mirrors are on (CarMirrorRt::active): where they are, they win
  bool picture_whole = false;    // MirrorView holds a finished picture
  uint32_t picture_age = 0;      // frames since it was drawn (0: this frame)
  bool play_view = false;        // the main colour is the play view's
  bool glass_found = false;      // MirrorGlass found a pair
  uint32_t needed_mask = 0;      // bit k: glass k is on the picture and wide enough
  bool field_known = false;      // the view's field of view is known
  bool pose_known = false;       // the camera the view was drawn from is known
};

// The hand-off with the ray traced side mirrors, and the rest in the order a person would ask: the setting, then whose mirror it is, then whether there is anything to show.
inline Why Decide(const Facts& f) {
  if (!f.setting_on) return Why::kSettingOff;
  if (f.rt_mirrors_on) return Why::kRtMirrors;
  if (!f.picture_whole) return Why::kNoPicture;
  if (f.picture_age > mv::kMaxHeldFrames) return Why::kStalePicture;
  if (!f.play_view) return Why::kNotPlayView;
  if (!f.glass_found) return Why::kNoGlass;
  if ((f.needed_mask & 3u) == 0) return Why::kNotNeeded;
  if (!f.field_known) return Why::kNoField;
  if (!f.pose_known) return Why::kNoPose;
  return Why::kDraw;
}

// ---------------------------------------------------------------------------
// the camera and the pose of the view
// ---------------------------------------------------------------------------

// The main camera's axes in the world: the columns of the view matrix's rotation (row vectors: view = (p, 1) * V, so the view's x axis is the world direction (V00, V10, V20)), and its eye (the point
// whose view coordinates are 0: p = -t R^T). False where the matrix is not a rigid motion (the axes must be unit and at right angles).
struct CameraAxes {
  double eye[3] = {};
  double axis[3][3] = {};  // axis[k] = the world direction of view axis k (x right, y up, z forward)
};
inline bool AxesOfView(const tp::Mat4& view, CameraAxes* out) {
  if (!tp::Finite(view)) {
    return false;
  }
  CameraAxes a;
  for (int k = 0; k < 3; ++k) {
    for (int c = 0; c < 3; ++c) {
      a.axis[k][c] = view.m[c][k];
    }
  }
  for (int k = 0; k < 3; ++k) {
    a.eye[k] = -(view.m[3][0] * view.m[k][0] + view.m[3][1] * view.m[k][1] + view.m[3][2] * view.m[k][2]);
    double len2 = 0.0;
    for (int c = 0; c < 3; ++c) {
      len2 += a.axis[k][c] * a.axis[k][c];
    }
    if (!(std::fabs(len2 - 1.0) < 2e-3)) {
      return false;
    }
    for (int j = 0; j < k; ++j) {
      double d = 0.0;
      for (int c = 0; c < 3; ++c) {
        d += a.axis[k][c] * a.axis[j][c];
      }
      if (!(std::fabs(d) < 2e-3)) {
        return false;
      }
    }
  }
  *out = a;
  return true;
}

// The camera the shared view was drawn from: its eye and the rig's axes (the camera's, turned half a circle about its up axis), and the field the view was built with.
struct ViewPose {
  bool known = false;
  double eye[3] = {};
  double right[3] = {};
  double up[3] = {};
  double forward[3] = {};
  double tan_h = 0.0;  // tan of half the horizontal and vertical field
  double tan_v = 0.0;
  uint64_t frame = 0;  // the guest frame it was drawn in
};

inline double HalfTan(double fov_deg) { return std::tan(fov_deg * 0.5 * 3.14159265358979323846 / 180.0); }

// The pose of a view drawn from `view` (the frame's camera) with a field of `hfov_deg` across and `vfov_deg` up. False where the field or the matrix is unusable.
inline bool PoseOfView(const tp::Mat4& view, double hfov_deg, double vfov_deg, uint64_t frame, ViewPose* out) {
  CameraAxes a;
  if (!(hfov_deg > 1.0 && hfov_deg < 179.0 && vfov_deg > 1.0 && vfov_deg < 179.0) || !AxesOfView(view, &a)) {
    return false;
  }
  ViewPose p;
  p.known = true;
  for (int c = 0; c < 3; ++c) {
    p.eye[c] = a.eye[c];
    p.right[c] = -a.axis[0][c];
    p.up[c] = a.axis[1][c];
    p.forward[c] = -a.axis[2][c];
  }
  p.tan_h = HalfTan(hfov_deg);
  p.tan_v = HalfTan(vfov_deg);
  p.frame = frame;
  *out = p;
  return true;
}

// The mirror_view::Rig of a pose (the double precision reference the shader's float arithmetic is held against).
inline mv::Rig RigOf(const ViewPose& p) {
  mv::Rig r;
  r.eye = {p.eye[0], p.eye[1], p.eye[2]};
  r.forward = {p.forward[0], p.forward[1], p.forward[2]};
  r.up = {p.up[0], p.up[1], p.up[2]};
  r.right = {p.right[0], p.right[1], p.right[2]};
  r.tan_half_h = p.tan_h;
  r.tan_half_v = p.tan_v;
  return r;
}

// ---------------------------------------------------------------------------
// the quaternion
// ---------------------------------------------------------------------------

// The unit quaternion (x, y, z, w) of a rotation matrix `m` (v' = m v on column vectors), the standard branch on the largest of the trace and the diagonal. The shader turns a vector by
// v + 2 cross(q.xyz, cross(q.xyz, v) + q.w v).
inline void QuaternionOf(const double m[3][3], double q[4]) {
  const double trace = m[0][0] + m[1][1] + m[2][2];
  double x, y, z, w;
  if (trace > 0.0) {
    const double s = 0.5 / std::sqrt(trace + 1.0);
    w = 0.25 / s;
    x = (m[2][1] - m[1][2]) * s;
    y = (m[0][2] - m[2][0]) * s;
    z = (m[1][0] - m[0][1]) * s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
    w = (m[2][1] - m[1][2]) / s;
    x = 0.25 * s;
    y = (m[0][1] + m[1][0]) / s;
    z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
    w = (m[0][2] - m[2][0]) / s;
    x = (m[0][1] + m[1][0]) / s;
    y = 0.25 * s;
    z = (m[1][2] + m[2][1]) / s;
  } else {
    const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
    w = (m[1][0] - m[0][1]) / s;
    x = (m[0][2] + m[2][0]) / s;
    y = (m[1][2] + m[2][1]) / s;
    z = 0.25 * s;
  }
  const double length = std::sqrt(x * x + y * y + z * z + w * w);
  const double inv = length > 0.0 ? 1.0 / length : 1.0;
  q[0] = x * inv;
  q[1] = y * inv;
  q[2] = z * inv;
  q[3] = w * inv;
}

// ---------------------------------------------------------------------------
// the draw's depth test
// ---------------------------------------------------------------------------

// plume::RenderComparisonFunction's values (mirror_overlay.cpp static_asserts them): the guest draw's pipeline key holds `enable | write << 1 | func << 2`.
inline constexpr uint32_t kCmpLess = 2, kCmpLessEqual = 4, kCmpGreater = 5, kCmpGreaterEqual = 7;
enum class DepthTest : uint8_t { kNone, kLessEqual, kGreaterEqual };

// The overlay repeats the glass draw's own depth test with the equal case allowed (it lands where the glass is): a draw that tests "less" is a LESS_EQUAL, one that tests "greater" (the game's main
// scene stores 1 near and 0 far) a GREATER_EQUAL. A draw with no test, or one that is neither (equal, always), is not repeated: the overlay would cover what stands in front of the glass.
inline DepthTest DepthTestOf(uint32_t depth_word) {
  if ((depth_word & 1u) == 0) {
    return DepthTest::kNone;
  }
  switch ((depth_word >> 2) & 0xFu) {
    case kCmpLess:
    case kCmpLessEqual: return DepthTest::kLessEqual;
    case kCmpGreater:
    case kCmpGreaterEqual: return DepthTest::kGreaterEqual;
    default: break;
  }
  return DepthTest::kNone;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

// One pipeline per vertex variant (the position's kind, the draw's topology and stride: car_shadow_map_rules.h VariantKey), per sample count of the main colour (1, 2, 4, 8) and per depth test.
constexpr uint32_t SampleIndex(uint32_t samples) { return samples == 1 ? 0u : samples == 2 ? 1u : samples == 4 ? 2u : samples == 8 ? 3u : 4u; }
constexpr uint32_t SamplesOf(uint32_t index) { return 1u << index; }
// The key in the host precache's variant word: vertex variant (20 bits) | sample index | the test.
constexpr uint32_t Variant(uint32_t vertex_variant, uint32_t sample_index, DepthTest test) {
  return (vertex_variant << 4) | ((sample_index & 3u) << 1) | (test == DepthTest::kGreaterEqual ? 1u : 0u);
}
constexpr uint32_t VertexOf(uint32_t variant) { return variant >> 4; }
constexpr uint32_t SampleIndexOf(uint32_t variant) { return (variant >> 1) & 3u; }
constexpr DepthTest TestOf(uint32_t variant) { return (variant & 1u) != 0 ? DepthTest::kGreaterEqual : DepthTest::kLessEqual; }

// ---------------------------------------------------------------------------
// the push block
// ---------------------------------------------------------------------------

struct Push {
  float v[8][4] = {};
};
static_assert(sizeof(Push) == kPushBytes, "the push block is 128 bytes (hlsl/mirror_overlay.hlsli MirrorOverlayPush)");

struct PushInputs {
  tp::Mat4 model_to_world;        // the glass draw's matrix (row vectors)
  tp::Mat4 view;                  // the frame's camera: world -> the main camera's view space (Frame::view)
  tp::Mat4 view_projection;       // the frame's clip space (Frame::view_projection), the game's own: the draws' world view projection
  float ndc_scale[3] = {1.0f, 1.0f, 1.0f};   // the glass draw's viewport transform (ViewportInfo): oPos.xyz = oPos.xyz * scale + offset * oPos.w
  float ndc_offset[3] = {0.0f, 0.0f, 0.0f};
  float jitter_ndc[2] = {0.0f, 0.0f};        // the frame's sub-pixel jitter in NDC (added to the offset of x and y), as the draws were moved by
  double normal_view[3] = {};     // the glass's unit normal in the main camera's view space
  ViewPose rig;                   // the camera and field the shared view was drawn with
  double distance = mv::kAssumedDistance;
  uint32_t picture_slot = 0;
};

enum class PushResult : uint8_t { kOk, kBadMatrix, kBadCamera, kProjection, kBadNormal, kBadPose };

inline float Bits(uint32_t v) {
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
inline uint32_t BitsOf(float f) {
  uint32_t v;
  std::memcpy(&v, &f, sizeof(v));
  return v;
}

// The game's clip space turned into the coefficients of the shader: C = P T, P the projection of the view space (view^-1 view_projection) and T the viewport transform of the draw with the jitter. On
// return `c` holds C scaled so that w' = pv.z (the shader's `w`), and the structure has been checked.
inline bool ProjectionCoefficients(const PushInputs& in, double c[4][4]) {
  tp::Mat4 inverse;
  if (!tp::Inverse(in.view, &inverse)) {
    return false;
  }
  const tp::Mat4 p = tp::Mul(inverse, in.view_projection);
  tp::Mat4 t = tp::Identity();
  t.m[0][0] = in.ndc_scale[0];
  t.m[1][1] = in.ndc_scale[1];
  t.m[2][2] = in.ndc_scale[2];
  t.m[3][0] = double(in.ndc_offset[0]) + double(in.jitter_ndc[0]);
  t.m[3][1] = double(in.ndc_offset[1]) + double(in.jitter_ndc[1]);
  t.m[3][2] = in.ndc_offset[2];
  const tp::Mat4 total = tp::Mul(p, t);
  if (!tp::Finite(total) || !(std::fabs(total.m[2][3]) > 1e-9)) {
    return false;
  }
  const double scale = 1.0 / total.m[2][3];
  double worst = 0.0;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      c[i][j] = total.m[i][j] * scale;
      worst = std::max(worst, std::fabs(c[i][j]));
    }
  }
  // the coefficients the shader does not have must be zero: x' takes (x, z), y' (y, z), z' (z, 1) and w' = z
  const double tol = 1e-5 * std::max(1.0, worst);
  const double zeros[] = {c[1][0], c[3][0], c[0][1], c[3][1], c[0][2], c[1][2], c[0][3], c[1][3], c[3][3]};
  for (double z : zeros) {
    if (!(std::fabs(z) <= tol)) {
      return false;
    }
  }
  return std::fabs(c[0][0]) > 1e-12 && std::fabs(c[1][1]) > 1e-12;
}

// A known camera that the overlay cannot replay has no use for a side-only rear view.
// Keep the ordinary hold for gaps in camera or draw capture between usable frames.
inline mirror_glass::NeedState UpdateSideNeed(mirror_glass::NeedHold& hold, const mirror_glass::NeedState& need, bool camera_known,
                                             const tp::Mat4& view, const tp::Mat4& view_projection) {
  if (camera_known && (need.need() || hold.holding())) {
    const PushInputs projection{.view = view, .view_projection = view_projection};
    double coefficients[4][4];
    if (!ProjectionCoefficients(projection, coefficients)) {
      hold.Reset();
      return {.found = need.found};
    }
  }
  return hold.Update(need);
}

inline PushResult MakePush(const PushInputs& in, Push* out) {
  if (!tp::Finite(in.model_to_world) || !tp::Finite(in.view_projection)) {
    return PushResult::kBadMatrix;
  }
  CameraAxes now;
  if (!AxesOfView(in.view, &now)) {
    return PushResult::kBadCamera;
  }
  if (!in.rig.known || !(in.rig.tan_h > 0.0) || !(in.rig.tan_v > 0.0)) {
    return PushResult::kBadPose;
  }
  double c[4][4];
  if (!ProjectionCoefficients(in, c)) {
    return PushResult::kProjection;
  }
  const double n2 = in.normal_view[0] * in.normal_view[0] + in.normal_view[1] * in.normal_view[1] + in.normal_view[2] * in.normal_view[2];
  if (!std::isfinite(n2) || !(n2 > 0.25) || !(n2 < 4.0)) {
    return PushResult::kBadNormal;
  }
  const double n_inv = 1.0 / std::sqrt(n2);

  Push p;
  // the model space -> the main camera's view space, eye-relative
  const tp::Mat4 a = tp::Mul(in.model_to_world, in.view);
  for (int k = 0; k < 3; ++k) {
    for (int r = 0; r < 4; ++r) {
      p.v[k][r] = float(a.m[r][k]);
    }
  }
  p.v[3][0] = float(c[0][0]);
  p.v[3][1] = float(c[2][0]);
  p.v[3][2] = float(c[1][1]);
  p.v[3][3] = float(c[2][1]);
  p.v[4][0] = float(c[2][2]);
  p.v[4][1] = float(c[3][2]);
  p.v[4][2] = float(in.rig.tan_h);
  p.v[4][3] = float(in.rig.tan_v);
  for (int k = 0; k < 3; ++k) {
    p.v[5][k] = float(in.normal_view[k] * n_inv);
  }
  p.v[5][3] = float(ClampDistance(in.distance));
  // the main camera's eye minus the rig's, in the main camera's axes
  for (int k = 0; k < 3; ++k) {
    double d = 0.0;
    for (int c3 = 0; c3 < 3; ++c3) {
      d += (now.eye[c3] - in.rig.eye[c3]) * now.axis[k][c3];
    }
    p.v[6][k] = float(d);
  }
  p.v[6][3] = Bits(in.picture_slot);
  // the rotation from the main camera's axes to the rig's: row r = the rig's axis r in the main camera's axes
  double m[3][3];
  const double* rig_axis[3] = {in.rig.right, in.rig.up, in.rig.forward};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      double d = 0.0;
      for (int c3 = 0; c3 < 3; ++c3) {
        d += rig_axis[r][c3] * now.axis[k][c3];
      }
      m[r][k] = d;
    }
  }
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (!(det > 0.99 && det < 1.01)) {
    return PushResult::kBadPose;  // the rig's axes are not a turn of the camera's: nothing is drawn rather than something reversed
  }
  double q[4];
  QuaternionOf(m, q);
  for (int k = 0; k < 4; ++k) {
    p.v[7][k] = float(q[k]);
  }
  *out = p;
  return PushResult::kOk;
}

// ---------------------------------------------------------------------------
// the picture's coordinates, for a world point of the glass: the double precision reference of the shader
// ---------------------------------------------------------------------------

// What the shader should answer for a point `glass` (world) of the glass whose plane has the unit normal `n` (world), seen from `eye_now` (world), for a view drawn from `rig` with the assumed distance:
// the design's UvForMirrorPixel, with the unit tests' own name for it here.
inline mv::Uv ReferenceUv(const mv::Rig& rig, const double eye_now[3], const double glass[3], const double n[3], double distance) {
  return mv::UvForMirrorPixel(rig, {eye_now[0], eye_now[1], eye_now[2]}, {glass[0], glass[1], glass[2]}, {n[0], n[1], n[2]}, distance);
}

// The edge fade of a coordinate pair, 0 on and outside the picture's edge to 1 from kEdgeFade inside it (the reference of MirrorOverlayEdgeFade).
inline double EdgeFade(double u, double v, double width = kEdgeFade) {
  const double e = std::min(std::min(u, 1.0 - u), std::min(v, 1.0 - v));
  return std::clamp(e / width, 0.0, 1.0);
}

}  // namespace mcla::gpu::mirror_overlay
