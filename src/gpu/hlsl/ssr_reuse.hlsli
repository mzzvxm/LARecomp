#ifndef MCLA_GPU_SSR_REUSE_HLSLI
#define MCLA_GPU_SSR_REUSE_HLSLI

float SsrPreviousHizAt(uint level, int cx, int cy) {
  // The chosen mip can differ between lanes. At each binding access the
  // slot itself is frame-uniform, as in SsrHizAt.
  const int3 texel = int3(cx, cy, 0);
  if (level == 0u) return g_textures[g_handoff.Load(24u)].Load(texel).x;
  if (level == 1u) return g_textures[g_handoff.Load(28u)].Load(texel).x;
  if (level == 2u) return g_textures[g_handoff.Load(32u)].Load(texel).x;
  if (level == 3u) return g_textures[g_handoff.Load(36u)].Load(texel).x;
  if (level == 4u) return g_textures[g_handoff.Load(40u)].Load(texel).x;
  return g_textures[g_handoff.Load(44u)].Load(texel).x;
}

#define SSR_PREVIOUS_TRACE
#include "ssr_trace.hlsli"

struct SsrReusedHit {
  float3 colour;
  float confidence;
  float distance;
};

// Walk the continuation in the previous view, where an off-screen or
// occluded hit may still be visible. Its depth and geometric normal come
// from the same frame as the colour.
SsrReusedHit SsrReuse(SsrTraceParams p, float3 origin, float3 direction, SsrHit current, float screen) {
  SsrReusedHit result = (SsrReusedHit)0;
  const uint levels = g_handoff.Load(12u);
  const bool occluded = current.behind_distance < kSsrMissLength;
  if (levels == 0u || screen >= 1.0f ||
      (!current.left_screen && !occluded && !current.hit && SsrFacingFade(direction.z) > 0.0f)) {
    return result;
  }

  const float4 m0 = asfloat(g_handoff.Load4(80u));
  const float4 m1 = asfloat(g_handoff.Load4(96u));
  const float4 m2 = asfloat(g_handoff.Load4(112u));
  const float3 ray = float3(dot(m0.xyz, direction), dot(m1.xyz, direction), dot(m2.xyz, direction));
  const float3 previous_origin = float3(dot(m0, float4(origin, 1.0f)), dot(m1, float4(origin, 1.0f)), dot(m2, float4(origin, 1.0f)));
  const bool match_current = current.hit && screen > 0.0f;
  const float candidate = !match_current && occluded ? current.behind_distance : current.ray_distance;
  const float tolerance = match_current ? max(0.15f, 0.015f * current.surface_z) : 0.0f;
  float start = max(0.0f, candidate - tolerance);
  const float4 lens = asfloat(g_handoff.Load4(48u));
  if (previous_origin.z + ray.z * start <= lens.z && ray.z > 0.0f) {
    start = max(start, (lens.z * 1.02f - previous_origin.z) / ray.z);
  }
  const float3 from = previous_origin + ray * start;
  if (!(from.z > lens.z && start < p.max_distance)) {
    return result;
  }
  const uint size = g_handoff.Load(20u);
  float3 current_normal = float3(0.0f, 0.0f, 0.0f);
  if (match_current) {
    const int2 current_texel = clamp(int2(current.px, current.py), int2(0, 0), int2(p.width_i, p.height_i) - 1);
    const float3 n = SsrHitNormal(p, current_texel.x, current_texel.y, current.surface_z);
    current_normal = float3(dot(m0.xyz, n), dot(m1.xyz, n), dot(m2.xyz, n));
  }
  p.width_i = SsrLo(size);
  p.height_i = SsrHi(size);
  p.width = float(p.width_i);
  p.height = float(p.height_i);
  p.tan_x = lens.x;
  p.tan_y = lens.y;
  p.near_plane = lens.z;
  const float4 projection = asfloat(g_handoff.Load4(64u));
  p.lin_x = projection.x;
  p.lin_y = projection.y;
  p.lin_z = projection.z;
  p.lin_w = projection.w;
  p.levels = levels;
  p.max_iter = min(p.max_iter, 24u);
  p.max_distance = (match_current ? min(p.max_distance, candidate + tolerance) : p.max_distance) - start;
  p.previous = true;
  const SsrHit previous = SsrTrace(p, from.x, from.y, from.z, ray.x, ray.y, ray.z);
  if (!previous.hit) {
    return result;
  }
  const int2 texel = clamp(int2(previous.px, previous.py), int2(0, 0), int2(p.width_i, p.height_i) - 1);
  const float3 normal = SsrHitNormal(p, texel.x, texel.y, previous.surface_z);
  const float facing = dot(normal, -ray);
  const float2 uv = float2(previous.px / p.width, previous.py / p.height);
  const float distance = start + previous.ray_distance;
  result.confidence = SsrReuseTrust(SsrEdgeFade(uv.x, uv.y), facing, previous.depth_error_fraction);
  if (match_current) {
    result.confidence *= SsrReuseAgreement(dot(normal, current_normal), (distance - candidate) / tolerance);
  }
  if (result.confidence > 0.0f) {
    const uint colour = g_handoff.Load(16u);
    result.colour = NgFinite3(g_textures[colour].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).rgb * lens.w);
    result.distance = distance;
  }
  return result;
}

#endif
