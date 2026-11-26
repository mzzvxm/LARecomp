// Radial blur of the sky mask. The first pass filters one segment toward
// the sun, and the second combines eight segments of the remaining ray.
// With an offscreen sun the segment endpoint is the ray's screen-edge hit.

#include "shafts_common.hlsli"
#include "shafts_noise.hlsli"
#include "host_pass_samplers.hlsli"

#ifndef SHAFTS_LEGACY
#define SHAFTS_LEGACY 0
#endif
#ifndef SHAFTS_ACCUMULATE
#define SHAFTS_ACCUMULATE 0
#endif

// R, the distance in image heights at which a sample counts for half.
static const float kRadialRange = 1.5f;

// slots.z bit 0: the start noise is the blue noise tile's (the host: mcla_gpu_noise_blue)
static const uint kBlueNoiseFlag = 1u;

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 size = g_push.sizes.xy;
  const float2 uv = position.xy / size;
  const float2 sun = g_push.sun.xy;
  const float2 to_sun = sun - uv;

  // How far along the way to the sun the ray goes: the density, or less when the image ends first.
  // (No vector ternaries: HLSL 2021 rejects them. The edge each axis runs into, and the distance to
  // it in units of the whole way; an axis that does not move reaches it never.)
  const float2 edge = step(0.0f, to_sun);
  const float2 safe_way = (edge * 2.0f - 1.0f) * max(abs(to_sun), 1e-6f);
  const float2 reach = (edge - uv) / safe_way;
  const float reach_screen = min(1.0f, min(reach.x, reach.y));
#if !SHAFTS_ACCUMULATE
  const float noise = ShaftsStartNoise(int(position.x), int(position.y), position.xy, (g_push.slots.z & kBlueNoiseFlag) != 0u, uint(g_push.tint.w));
#endif
#if SHAFTS_LEGACY
  const uint steps = max(g_push.slots.w, 1u);
  const float length_of_way = min(g_push.sun.z, reach_screen);
  const float2 step_uv = to_sun * (length_of_way / float(steps));

  const float aspect = size.x / size.y;
  const float inv_range2 = 1.0f / (kRadialRange * kRadialRange);

  float2 at = uv + step_uv * noise;
  float weight = 1.0f;
  float sum = 0.0f;
  [loop] for (uint i = 0u; i < steps; ++i) {
    at += step_uv;
    const float shine =
        g_textures[g_push.slots.y].SampleLevel(g_samplers[kSamplerLinearClamp], at, 0).x;
    const float2 q = (at - sun) * float2(aspect, 1.0f);
    sum += shine * weight / (1.0f + dot(q, q) * inv_range2);
    weight *= g_push.sun.w;
  }
  return float4(sum / float(steps), 0.0f, 0.0f, 0.0f);
#else
  static const uint kTaps = 8u;
  const float ratio = g_push.sun.w;
  const float2 ray = to_sun * reach_screen;
  float sum = 0.0f;
#if SHAFTS_ACCUMULATE
  const float4 near_weights = float4(asfloat(g_push.slots.x), asfloat(g_push.slots.w), g_push.sizes.zw);
  const float4 far_weights = g_push.tint;
  float remaining = 1.0f;
  [unroll] for (uint i = 0u; i < kTaps; ++i) {
    const float2 at = uv + ray * (1.0f - remaining);
    const float weight = i < 4u ? near_weights[i & 3u] : far_weights[i & 3u];
    sum += weight * g_textures[g_push.slots.y].SampleLevel(g_samplers[kSamplerLinearClamp], at, 0).x;
    remaining *= ratio;
  }
#else
  const float2 step_uv = ray * ((1.0f - ratio) / float(kTaps));
  const float aspect = size.x / size.y;
  [unroll] for (uint i = 0u; i < kTaps; ++i) {
    const float2 at = uv + step_uv * (float(i) + noise);
    const float2 q = (at - sun) * float2(aspect, 1.0f);
    const float shine = g_textures[g_push.slots.y].SampleLevel(g_samplers[kSamplerLinearClamp], at, 0).x;
    sum += shine / (1.0f + dot(q, q) / (kRadialRange * kRadialRange));
  }
  sum /= float(kTaps);
#endif
  return float4(sum, 0.0f, 0.0f, 0.0f);
#endif
}
