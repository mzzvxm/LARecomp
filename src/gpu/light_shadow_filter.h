#pragma once
// ===========================================================================
// MCLA GPU renderer -- the headlight shadows' lookup, on the CPU: the twin of the lit shaders' HeadlightShadow (HL-SHADOW-QUALITY, 2026-11-21)
// ===========================================================================
// WHAT WAS WRONG (the owner, 2026-11-21, on 4K: the poles', signs', kerbs' and cars' shadows "extremely dither-y and nowhere near as sharp as they should be"). The lookup was a box of
// 3 x 3 texels over a 512 texel view whose field is 84 degrees: a texel was 3.5 cm at 10 m and 7 cm at 20 m, the box 10 and 21 cm; a bollard or a sign post (10 cm) was 1.4 to 3
// texels wide, thinner than the box, and the lookup's shortcut (the four outer taps of the 4 x 4 footprint agree: the answer is theirs) never saw such a thing between them: its shadow
// was a dashed stripe that came and went with the receiver's place and the car's motion (the old test says so: "a feature thinner than the box is not seen"). A road seen at 4 degrees
// from a lamp 0.7 m up also needed a depth margin for its own slope over those 3 texels, which the map's slope bias (2.0) supplied by pushing every grazing blocker 1 m away at 10 m.
//
// WHAT IT IS NOW (shaderpack/variants/headlight.cpp HeadlightShadow is this file in HLSL; headlight_test.cpp holds the two to the same numbers):
//   1. FOUR GATHERS cover the whole 4 x 4 texel footprint: every texel is read, so nothing thinner than a texel is lost between taps (a Gather is one fetch: the same four fetches the old
//      shortcut took, and never the 16 or 20 of the old edge pixels).
//   2. THE COMPARE IS MADE AT THE RECEIVER'S PLANE: the receiver's depth at the centre of each texel it reads is its own depth plus the slope of its surface times the distance in texels
//      (z/w is affine over a plane in the map's uv), from the geometric normal and the view's matrix. A lit road then compares equal to its own stored depth in every texel, whatever the
//      angle, and the map needs a tiny constant bias instead of a margin for the slope (no acne, no shadows that start metres behind what casts them).
//   3. CONTACT HARDENING (the sun's PCSS, shaderpack/variants/shadow_filter.cpp): the kernel is a box of 2r texels whose r is the penumbra of the lamp (kLampRadius) seen through the
//      average blocker in the footprint: sharp where a shadow touches what casts it (r = 0.5: a bilinear compare), up to 1.5 texels far from it. r is held to [0.5, 1.5] because the 4 x 4
//      footprint holds a box of half width 1.5 wherever the receiver is in its texel. The box's weights are continuous in the receiver's place and in r (each texel by the part of it the
//      box covers), so the shadow moves smoothly: no steps, no grain; nothing in it depends on the screen position (no noise: nothing to be resolved by a temporal upscaler).
//   4. THE EDGE OF THE VIEW FADES: the shadow is blended away over the last kBorderFade of the tile, where the beam is below an eighth of its strength anyway (the view holds the cone up to
//      34 degrees: local_shadow_rules.h kShadowConeCapDegrees), instead of ending in a line.
//
// The depth conventions are the map's (local_shadow_types.h, local_shadow_rules.h): z/w = A (1 - n / w) for a point w metres along the view's axis (A = f / (f - n)), 0 at the near plane,
// 1 at the far plane and where nothing was drawn; lit when stored >= receiver - bias. A and the lateral scale come from the matrix itself (row 3 is the unit axis, row 2 is A times it),
// so the view table needs no new field.
//
// Pure, float, no renderer: it follows the shader's arithmetic (the order of the Gather's components, the clamps) so that the test can hold the HLSL text to it. Namespace
// larecomp::gpu::shadow_filter.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "local_shadow_types.h"

namespace larecomp::gpu::shadow_filter {

inline constexpr float kPcfMinRadius = 0.5f;   // texels: the kernel's half width at a contact (a bilinear compare of the nearest four texels)
inline constexpr float kPcfMaxRadius = 1.5f;   // ... and the most the 4 x 4 footprint holds wherever the receiver is in its texel
inline constexpr float kLampRadius = 0.07f;    // m: the size of a headlamp seen from the receiver (a lens about 14 cm across)
inline constexpr float kMaxSlope = 60.0f;      // the steepest receiver believed: 60 m of depth per metre across (a surface 0.95 degrees from the light's ray: a road 42 m from a lamp 0.7 m up); a flatter one takes the limit
inline constexpr float kSlopeBias = 0.15f;     // the share of a receiver's own depth change over one texel (|du| + |dv|) added to the bias: the normal is the pixel quad's, not the mesh's
inline constexpr float kBorderFade = 0.05f;    // the share of the tile, from its border in, over which the shadow fades to light
inline constexpr float kDegenerate = 1.0e-3f;  // the sine of the angle between the light's ray and the receiver's plane below which the plane is not believed (no slope: the bias alone)
inline constexpr float kNearlyOne = 1.0e-6f;

struct Float2 {
  float x = 0.0f, y = 0.0f;
};
struct Float3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Float3 Sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Float3 Scale(Float3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Float3 Cross(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Float3 a) { return std::sqrt(Dot(a, a)); }
inline float Saturate(float x) { return std::min(std::max(x, 0.0f), 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// The four texels one Gather returns, in the order the hardware does (D3D12 and Vulkan): x = (0, 1), y = (1, 1), z = (1, 0), w = (0, 0) of the quad (column, row from the lowest).
struct Quad {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// What the lookup tells besides the answer (the tests read it; the shader does not make it).
struct Detail {
  bool outside = true;       // behind the view, outside its rectangle or its depth range: lit, nothing was read
  float fade = 0.0f;         // the border fade's weight of the shadow
  int blockers = 0;          // texels of the 4 x 4 footprint nearer than the receiver's plane
  float radius = 0.0f;       // the kernel's half width in texels (0 when the footprint was all lit or all dark)
  Float2 slope{};            // the receiver's depth change over one texel along u and v (z/w)
  float depth_over_a = 0.0f; // z of the receiver
  int gathers = 0;           // fetches taken
};

// One view at a world point with the surface's unit normal: 1 lit .. 0 in shadow. `gather(ix, iy)` returns the four texels of the quad whose lowest texel is (ix, iy) of the ATLAS
// (what GatherRed at the corner shared by the four returns). `view.texel_size` is one texel as a share of the tile.
template <class Gather>
inline float ViewLit(const larecomp::gpu::LocalShadowView& view, const float world[3], const float normal_in[3], Gather&& gather, Detail* detail = nullptr) {
  Detail local;
  Detail& d = detail ? *detail : local;
  d = Detail{};
  const float* m = view.world_to_shadow;
  const Float3 p{world[0], world[1], world[2]};
  const Float3 normal{normal_in[0], normal_in[1], normal_in[2]};
  const Float3 row0{m[0], m[1], m[2]}, row1{m[4], m[5], m[6]}, row2{m[8], m[9], m[10]}, row3{m[12], m[13], m[14]};
  const float clip_x = Dot(row0, p) + m[3];
  const float clip_y = Dot(row1, p) + m[7];
  const float clip_z = Dot(row2, p) + m[11];
  const float clip_w = Dot(row3, p) + m[15];
  if (clip_w <= 0.0f) {
    return 1.0f;
  }
  const float inv_w = 1.0f / clip_w;
  const float sx = clip_x * inv_w, sy = clip_y * inv_w, sz = clip_z * inv_w;
  const float* rect = view.atlas_rect;
  if (sx < rect[0] || sy < rect[1] || sx > rect[2] || sy > rect[3] || sz < 0.0f || sz > 1.0f) {
    return 1.0f;
  }
  d.outside = false;
  const Float2 tile{rect[2] - rect[0], rect[3] - rect[1]};
  const Float2 t{tile.x * view.texel_size, tile.y * view.texel_size};  // one texel of the tile in atlas uv
  const float border = std::min(std::min(sx - rect[0], rect[2] - sx) / tile.x, std::min(sy - rect[1], rect[3] - sy) / tile.y);
  const float fade = Saturate(border * (1.0f / kBorderFade));
  d.fade = fade;
  if (fade <= 0.0f) {
    return 1.0f;
  }

  // THE RECEIVER'S PLANE. The Jacobians of (u, v, z) with respect to the world point, u and v in texels: d(c / w) = (row - s row3) / w. In the plane of normal n, a step of (du, dv)
  // texels is the point step dp with ju . dp = du, jv . dp = dv, n . dp = 0, and the depth changes by jz . dp.
  const Float3 axis = row3;
  const Float3 ju = Scale(Sub(row0, Scale(axis, sx)), inv_w / t.x);
  const Float3 jv = Scale(Sub(row1, Scale(axis, sy)), inv_w / t.y);
  const Float3 jz = Scale(Sub(row2, Scale(axis, sz)), inv_w);
  const float len_u = Length(ju), len_v = Length(jv);
  const float a_scale = Dot(row2, axis) / Dot(axis, axis);  // A = f / (f - n)
  const float limit_u = kMaxSlope * (a_scale - sz) * inv_w / len_u;
  const float limit_v = kMaxSlope * (a_scale - sz) * inv_w / len_v;
  const Float3 vn = Cross(jv, normal);
  const float det = Dot(ju, vn);
  Float2 grad{};
  if (std::fabs(det) > kDegenerate * len_u * len_v) {
    grad.x = std::min(std::max(Dot(jz, vn) / det, -limit_u), limit_u);
    grad.y = std::min(std::max(Dot(jz, Cross(normal, ju)) / det, -limit_v), limit_v);
  }
  d.slope = grad;
  d.depth_over_a = sz;
  const float bias = view.depth_bias + kSlopeBias * (std::fabs(grad.x) + std::fabs(grad.y));
  const float zr = sz - bias;

  // THE FOOTPRINT: 4 x 4 texels from `first`, read by four gathers at the corners shared by each quad of four.
  const Float2 c{sx / t.x, sy / t.y};  // the receiver's place in texels of the atlas
  const Float2 first{std::floor(c.x - 0.5f) - 1.0f, std::floor(c.y - 0.5f) - 1.0f};
  const int ix = int(first.x), iy = int(first.y);
  const Quad q00 = gather(ix, iy), q10 = gather(ix + 2, iy), q01 = gather(ix, iy + 2), q11 = gather(ix + 2, iy + 2);
  d.gathers = 4;
  // The receiver's offset from each texel's centre, in texels: column a is at first + a + 0.5.
  const float ox[4] = {first.x + 0.5f - c.x, first.x + 1.5f - c.x, first.x + 2.5f - c.x, first.x + 3.5f - c.x};
  const float oy[4] = {first.y + 0.5f - c.y, first.y + 1.5f - c.y, first.y + 2.5f - c.y, first.y + 3.5f - c.y};
  // The quad's components x y z w are the texels (col 0, row 1), (1, 1), (1, 0), (0, 0) of the quad: its columns 2 qa + {0, 1, 1, 0}, its rows 2 qb + {1, 1, 0, 0}.
  const auto lit_of = [&](const Quad& q, int qa, int qb, float out[4]) {
    const float stored[4] = {q.x, q.y, q.z, q.w};
    const int col[4] = {0, 1, 1, 0}, row[4] = {1, 1, 0, 0};
    for (int k = 0; k < 4; ++k) {
      const float need = std::min(zr + grad.x * ox[2 * qa + col[k]] + grad.y * oy[2 * qb + row[k]], 1.0f);
      out[k] = stored[k] >= need ? 1.0f : 0.0f;
    }
  };
  float l00[4], l10[4], l01[4], l11[4];
  lit_of(q00, 0, 0, l00);
  lit_of(q10, 1, 0, l10);
  lit_of(q01, 0, 1, l01);
  lit_of(q11, 1, 1, l11);
  const float* lits[4] = {l00, l10, l01, l11};
  const Quad* quads[4] = {&q00, &q10, &q01, &q11};
  float blockers = 0.0f, blocker_sum = 0.0f;
  for (int q = 0; q < 4; ++q) {
    const float stored[4] = {quads[q]->x, quads[q]->y, quads[q]->z, quads[q]->w};
    for (int k = 0; k < 4; ++k) {
      const float blocks = 1.0f - lits[q][k];
      blockers += blocks;
      blocker_sum += stored[k] * blocks;
    }
  }
  d.blockers = int(blockers + 0.5f);
  if (blockers < 0.5f) {
    return 1.0f;
  }
  if (blockers > 15.5f) {
    return Lerp(1.0f, 0.0f, fade);
  }
  // CONTACT HARDENING. The lamp's disc seen from the receiver through the blockers' plane has the radius R (w_r - w_b) / w_r there; a texel is w_b / (w_r |ju|) wide there, and
  // w_b / w_r = (A - z_r) / (A - z_b), so the kernel's half width in texels is R |ju| (z_r - z_b) / (A - z_r).
  const float mean_blocker = blocker_sum / blockers;
  float radius = kLampRadius * 0.5f * (len_u + len_v) * (sz - mean_blocker) / std::max(a_scale - sz, 1.0e-6f);
  radius = std::min(std::max(radius, kPcfMinRadius), kPcfMaxRadius);
  d.radius = radius;
  // The box [c - r, c + r] over the texel intervals [e, e + 1] of the footprint, each texel by the part of it the box covers (the weights along an axis sum to 2 r).
  float wx[4], wy[4];
  for (int k = 0; k < 4; ++k) {
    const float ex = first.x + float(k), ey = first.y + float(k);
    wx[k] = Saturate(std::min(ex + 1.0f, c.x + radius) - std::max(ex, c.x - radius));
    wy[k] = Saturate(std::min(ey + 1.0f, c.y + radius) - std::max(ey, c.y - radius));
  }
  // Row b of the footprint, columns 0..3: row 0 is l00.w l00.z l10.w l10.z, row 1 is l00.x l00.y l10.x l10.y, rows 2 and 3 the same of l01 and l11.
  const float rows[4][4] = {{l00[3], l00[2], l10[3], l10[2]}, {l00[0], l00[1], l10[0], l10[1]}, {l01[3], l01[2], l11[3], l11[2]}, {l01[0], l01[1], l11[0], l11[1]}};
  float share = 0.0f;
  for (int b = 0; b < 4; ++b) {
    share += wy[b] * (wx[0] * rows[b][0] + wx[1] * rows[b][1] + wx[2] * rows[b][2] + wx[3] * rows[b][3]);
  }
  share /= 4.0f * radius * radius;
  return Lerp(1.0f, std::min(std::max(share, 0.0f), 1.0f), fade);
}

}  // namespace larecomp::gpu::shadow_filter
