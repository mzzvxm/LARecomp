// MCLA GPU renderer -- screen-space global illumination (the bounce light test of ao.h): the a-trous
// denoise of the bounce, after the accumulation over frames (ao_temporal_ps with AO_GI).
//
// The history keeps what the frames add up to and must not be blurred (the next frame would blur the
// blur): the filter reads it and writes a copy that only the picture sees. One pass is one iteration of an
// edge-avoiding a-trous wavelet filter (Dammertz, Sewtz, Hanika and Lensch 2010; written for this renderer
// from the idea): the 5x5 kernel of the B3 spline (1 4 6 4 1) / 16 on each axis, its taps `step` pixels
// apart (1, then 2, then 4: each iteration doubles the reach for the same 25 taps). A tap counts by that
// kernel and by how far its view-space position is from the tangent plane of the centre pixel (the depth-
// derived normal: the measure of the occlusion's own denoise, ao_common.hlsli AoBilateral), so the blur
// stops at a depth edge and at a corner, and a surface seen at a grazing angle is blurred like any other.
// A tap off the image, or on the sky, has no weight; the centre always has, so a pixel on a plane of its own
// keeps its value. The visibility (x) passes through untouched: the occlusion's own denoise and history made
// it; only the bounce (yzw) is filtered.
//
// Push fields: slots.z the image to filter (RGBA16F: visibility, bounce), slots.w the linear depth image,
// slots2.x the step in pixels, slots2.w the normal image's slot (0: none, the depth's normal; ao_common.hlsli AoNormalAt), sizes.xy the size of both, lens the lens, extra.y the
// distance fade's end.
// Output: RGBA16F, the same layout.

#include "ao_common.hlsli"
#include "ssgi_filter_plane.hlsli"

// The plane test's width as a share of the distance and its floor: tighter than the occlusion's (a bounce
// colour changes at a corner, a visibility does not much).
static const float kPlaneShare = 0.006f;
static const float kPlaneFloor = 0.01f;

// The B3 spline's weights at offsets 0, 1 and 2.
static const float kTap[3] = {0.375f, 0.25f, 0.0625f};

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.xy;
  const int2 isize = int2(size);
  const uint source_slot = g_push.slots.z;
  const uint depth_slot = g_push.slots.w;
  const int step = int(max(g_push.slots2.x, 1u));
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float4 centre = g_textures[source_slot].Load(int3(pixel, 0));
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalAt(g_push.slots2.w, depth_slot, pixel, isize, tc, pc);
  // The plane weight's reciprocal and the image's, made once for the pass: a division in each of the 24 taps otherwise.
  const float inv_plane_sigma = 1.0f / (kPlaneShare * tc + kPlaneFloor);
  const float2 clip_scale = 2.0f / size;
  const bool fast_plane = (g_push.slots2.z & (1u << 25)) != 0u;
  const float plane_at_centre = dot(normal, pc);
  const float2 plane_steps = float2(normal.x * g_push.lens.x, -normal.y * g_push.lens.y) * clip_scale * float(step);
  const float ray_at_centre = plane_at_centre / tc;
  float3 sum = centre.yzw * (kTap[0] * kTap[0]);
  float weights = kTap[0] * kTap[0];
  [unroll] for (int dy = -2; dy <= 2; ++dy) {
    [unroll] for (int dx = -2; dx <= 2; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int2 q = pixel + int2(dx, dy) * step;
      if (q.x < 0 || q.y < 0 || q.x >= isize.x || q.y >= isize.y) {
        continue;
      }
      const float tq = AoLoadLinear(depth_slot, q);
      if (tq >= kAoSky * 0.5f) {
        continue;
      }
      float off_plane;
      if (fast_plane) {
        off_plane = SsgiFilterPlaneOffset(tq, tc, ray_at_centre,
                                          float(dx) * plane_steps.x + float(dy) * plane_steps.y) * inv_plane_sigma;
      } else {
        const float3 pq = AoViewPosInv(float2(q) + 0.5f, clip_scale, tq);
        off_plane = dot(normal, pq - pc) * inv_plane_sigma;
      }
      const float w = kTap[abs(dx)] * kTap[abs(dy)] * exp2(kAoNegHalfLog2e * off_plane * off_plane);
      sum += g_textures[source_slot].Load(int3(q, 0)).yzw * w;
      weights += w;
    }
  }
  return float4(centre.x, sum / weights);
}
