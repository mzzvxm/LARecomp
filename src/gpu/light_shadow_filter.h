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
// THE LIGHT SIZE AND THE FOOTPRINT ARE SETTINGS (HSQ-PCSS, 2026-11-22, the diagnostic mcla_gpu_headlight_shadow_lamp_cm and mcla_gpu_headlight_shadow_kernel): the owner asked why a shadow
// does not get visibly softer with its distance from what casts it. It does, by the formula of 3.: the penumbra of a 14 cm lamp is 2 R L / w_b wide at L metres behind a blocker w_b metres
// from the lamp (1.3 cm at 1 m, 8.8 cm at 7 m behind a pole 9 m away): small, and the 4 x 4 footprint's 1.5 texel cap (about 7 cm at 16 m) trims only the end of that. A larger lamp
// (Params::lamp_radius) needs a larger kernel to show: Params::gathers = G reads G x G gathers, a 2G x 2G texel footprint (G = 2 is the 4 x 4 above, 4 fetches; 3 is 6 x 6, 9 fetches,
// kernel up to 2.5 texels; 4 is 8 x 8, 16 fetches, up to 3.5), and the same arithmetic holds for any G (the weights of a box over the footprint, the blockers averaged over all of it).
// The default Params are the lookup of 1. to 4. bit for bit (the tests hold them to the old numbers).
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

inline constexpr int kMinGathers = 2;           // gathers a side: 2 x 2 gathers = the 4 x 4 texel footprint of the first version
inline constexpr int kMaxGathers = 4;           // ... up to 4 x 4 gathers = 8 x 8 texels (16 fetches)
inline constexpr float kMinLampRadius = 0.005f; // m: the settings' range, 1 cm across to ...
inline constexpr float kMaxLampRadius = 0.5f;   // ... a metre across

// What the lookup is told besides the view and the point: the size of the lamp the penumbra is worked out for, and how many gathers a side the footprint is. The default is the
// lookup of the first version (kLampRadius, 2).
struct Params {
  float lamp_radius = kLampRadius;  // m
  int gathers = kMinGathers;        // a side
};
// The largest kernel half width (texels) a footprint of G x G gathers holds wherever the receiver is in its texel: its 2G texels reach G - 0.5 from the point's texel centre.
inline constexpr float MaxRadius(int gathers) { return float(gathers) - 0.5f; }
// The Params a setting's numbers make: the gathers held to [kMinGathers, kMaxGathers], the radius to [kMinLampRadius, kMaxLampRadius] (a number that is not finite takes the default).
inline Params MakeParams(double lamp_radius_m, int gathers) {
  Params p;
  p.gathers = std::min(std::max(gathers, kMinGathers), kMaxGathers);
  p.lamp_radius = std::isfinite(lamp_radius_m) ? std::min(std::max(float(lamp_radius_m), kMinLampRadius), kMaxLampRadius) : kLampRadius;
  return p;
}
// The deferred pass reads the Params as one word of its shadow table's header: bits 0-3 gathers, bits 8-23 the radius in millimetres; 0 stands for the default (the header is then what it
// was before the settings). The lit shaders' block carries them as two floats (HeadlightConstants::header y = the radius in metres, z = the gathers; 0 = the default).
inline uint32_t PackParams(const Params& p) {
  const Params q = MakeParams(p.lamp_radius, p.gathers);
  const uint32_t mm = uint32_t(q.lamp_radius * 1000.0f + 0.5f);
  if (q.gathers == kMinGathers && mm == uint32_t(kLampRadius * 1000.0f + 0.5f)) {
    return 0u;
  }
  return uint32_t(q.gathers) | (mm << 8);
}
inline Params UnpackParams(uint32_t word) {
  Params p;
  const uint32_t g = word & 15u;
  p.gathers = g < uint32_t(kMinGathers) ? kMinGathers : (g > uint32_t(kMaxGathers) ? kMaxGathers : int(g));
  const uint32_t mm = (word >> 8) & 0xFFFFu;
  p.lamp_radius = mm == 0u ? kLampRadius : float(mm) * 0.001f;
  return p;
}

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
  int blockers = 0;          // texels of the footprint (4 x 4 by default) nearer than the receiver's plane
  float radius = 0.0f;       // the kernel's half width in texels (0 when the footprint was all lit or all dark)
  Float2 slope{};            // the receiver's depth change over one texel along u and v (z/w)
  float depth_over_a = 0.0f; // z of the receiver
  int gathers = 0;           // fetches taken
};

// One view at a world point with the surface's unit normal: 1 lit .. 0 in shadow. `gather(ix, iy)` returns the four texels of the quad whose lowest texel is (ix, iy) of the ATLAS
// (what GatherRed at the corner shared by the four returns). `view.texel_size` is one texel as a share of the tile.
template <class Gather>
inline float ViewLit(const larecomp::gpu::LocalShadowView& view, const float world[3], const float normal_in[3], Gather&& gather, Detail* detail = nullptr, const Params& params = Params{}) {
  const Params use = MakeParams(params.lamp_radius, params.gathers);
  const int G = use.gathers;  // gathers a side
  const int N = 2 * G;        // texels a side
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

  // THE FOOTPRINT: N x N texels (4 x 4 by default) from `first`, read by G x G gathers at the corners shared by each quad of four.
  const Float2 c{sx / t.x, sy / t.y};  // the receiver's place in texels of the atlas
  const Float2 first{std::floor(c.x - 0.5f) - float(G - 1), std::floor(c.y - 0.5f) - float(G - 1)};
  const int ix = int(first.x), iy = int(first.y);
  Quad quads[kMaxGathers * kMaxGathers];
  for (int qb = 0; qb < G; ++qb) {
    for (int qa = 0; qa < G; ++qa) {
      quads[qb * G + qa] = gather(ix + 2 * qa, iy + 2 * qb);
    }
  }
  d.gathers = G * G;
  // The receiver's offset from each texel's centre, in texels: column a is at first + a + 0.5.
  float ox[2 * kMaxGathers], oy[2 * kMaxGathers];
  for (int k = 0; k < N; ++k) {
    ox[k] = first.x + 0.5f + float(k) - c.x;
    oy[k] = first.y + 0.5f + float(k) - c.y;
  }
  // The quad's components x y z w are the texels (col 0, row 1), (1, 1), (1, 0), (0, 0) of the quad: its columns 2 qa + {0, 1, 1, 0}, its rows 2 qb + {1, 1, 0, 0}.
  float lit[2 * kMaxGathers][2 * kMaxGathers];     // [row from the lowest][column]
  float stored[2 * kMaxGathers][2 * kMaxGathers];
  const int col[4] = {0, 1, 1, 0}, row[4] = {1, 1, 0, 0};
  for (int qb = 0; qb < G; ++qb) {
    for (int qa = 0; qa < G; ++qa) {
      const Quad& q = quads[qb * G + qa];
      const float values[4] = {q.x, q.y, q.z, q.w};
      for (int k = 0; k < 4; ++k) {
        const int cc = 2 * qa + col[k], rr = 2 * qb + row[k];
        const float need = std::min(zr + grad.x * ox[cc] + grad.y * oy[rr], 1.0f);
        stored[rr][cc] = values[k];
        lit[rr][cc] = values[k] >= need ? 1.0f : 0.0f;
      }
    }
  }
  // The blockers, summed in the order of the gathers' components (quad by quad, as the first version did for its four).
  float blockers = 0.0f, blocker_sum = 0.0f;
  for (int qb = 0; qb < G; ++qb) {
    for (int qa = 0; qa < G; ++qa) {
      for (int k = 0; k < 4; ++k) {
        const int cc = 2 * qa + col[k], rr = 2 * qb + row[k];
        const float blocks = 1.0f - lit[rr][cc];
        blockers += blocks;
        blocker_sum += stored[rr][cc] * blocks;
      }
    }
  }
  d.blockers = int(blockers + 0.5f);
  if (blockers < 0.5f) {
    return 1.0f;
  }
  if (blockers > float(N * N) - 0.5f) {
    return Lerp(1.0f, 0.0f, fade);
  }
  // CONTACT HARDENING. The lamp's disc seen from the receiver through the blockers' plane has the radius R (w_r - w_b) / w_r there; a texel is w_b / (w_r |ju|) wide there, and
  // w_b / w_r = (A - z_r) / (A - z_b), so the kernel's half width in texels is R |ju| (z_r - z_b) / (A - z_r).
  const float mean_blocker = blocker_sum / blockers;
  float radius = use.lamp_radius * 0.5f * (len_u + len_v) * (sz - mean_blocker) / std::max(a_scale - sz, 1.0e-6f);
  radius = std::min(std::max(radius, kPcfMinRadius), MaxRadius(G));
  d.radius = radius;
  // The box [c - r, c + r] over the texel intervals [e, e + 1] of the footprint, each texel by the part of it the box covers (the weights along an axis sum to 2 r).
  float wx[2 * kMaxGathers], wy[2 * kMaxGathers];
  for (int k = 0; k < N; ++k) {
    const float ex = first.x + float(k), ey = first.y + float(k);
    wx[k] = Saturate(std::min(ex + 1.0f, c.x + radius) - std::max(ex, c.x - radius));
    wy[k] = Saturate(std::min(ey + 1.0f, c.y + radius) - std::max(ey, c.y - radius));
  }
  float share = 0.0f;
  for (int b = 0; b < N; ++b) {
    float along = 0.0f;
    for (int a = 0; a < N; ++a) {
      along += wx[a] * lit[b][a];
    }
    share += wy[b] * along;
  }
  share /= 4.0f * radius * radius;
  return Lerp(1.0f, std::min(std::max(share, 0.0f), 1.0f), fade);
}

}  // namespace larecomp::gpu::shadow_filter
