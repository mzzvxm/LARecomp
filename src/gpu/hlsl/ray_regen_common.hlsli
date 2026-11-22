// MCLA GPU renderer -- AMD FSR Ray Regeneration's guide conversion: the pure functions (ray_regen_ssr_cs.hlsl uses them; ray_regen_guides.h, perf/RAY-REGEN.md, package RG-2).
//
// Ray Regeneration reads its own formats, not ours (denoising.md 2.5, 5.2 to 5.5; ffx_denoiser.h ffxDispatchDescDenoiser):
//
//   linear depth     R32F  the SIGNED view-space z of the game's own camera: the view looks along -z, so a surface ahead is NEGATIVE (the sign is a push value, so that the mirrored convention that the
//                    RAY-REGEN.md fallback describes is one number in ray_regen_guides_rules.h); a texel with nothing to denoise is given a depth far outside the bounds of the dispatch, which is how
//                    AMD's passthrough is asked for (linearDepthBounds: outside = the output is the input)
//   motion vectors   RGBA16F  rg = PreviousUV - CurrentUV (jitter taken out, +y down), b = the signed linear depth delta PreviousLinearDepth - CurrentLinearDepth
//   normals          RGBA16F  rg the octahedral world normal (AMD's map: the G-buffer's own), b the LINEAR roughness, a the material type as a fraction of 3 (0: the default, every texel for now)
//   specular albedo  RGBA16F  linear (the dispatch carries FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO), the Fresnel term the reflection's resolve multiplies by
//   signal           RGBA16F  the indirect specular signal: the radiance WITHOUT the first bounce's albedo (the trace's own rgb: the resolve multiplies the Fresnel back in after the denoise), a the
//                    ray's hit distance (what AMD moves a reflection's history by)
//
// Every function below is pure; src/gpu/ray_regen_guides_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL vector types, as the G-buffer's, the reflections' and the Ray
// Reconstruction guides' tests do) and checks each against the twin in ray_regen_guides_rules.h, so the text keeps to a subset both languages read: scalar and float2 / float3 / float4
// arithmetic, .x .y .z .w, no swizzles of more than one component, no `out` parameters.

#ifndef MCLA_GPU_RAY_REGEN_COMMON_HLSLI
#define MCLA_GPU_RAY_REGEN_COMMON_HLSLI

// A depth for a texel that is not denoised. FP16-safe (the runtime may keep it in a half float) and far above the far plane (about 10,000), so it is outside every dispatch's bounds.
static const float kRgNoDepth = 60000.0f;
// The shortest hit distance a traced texel carries: a traced texel has a hit distance above 0 in its alpha (the reflection's resolve reads "alpha is not 0" as "a ray was traced here").
static const float kRgMinHit = 0.02f;
// Material types 0 to 3: the stored value is a fraction of the largest.
static const float kRgMaterialMax = 3.0f;

float RgSignNotZero(float v) {
  return v >= 0.0f ? 1.0f : -1.0f;
}

// AMD's octahedral map (denoising.md 5.3.1) with sign() made total (0 is positive): the G-buffer's OctEncode is the same map.
float2 RgOctahedronUv(float3 n) {
  const float inv = 1.0f / (abs(n.x) + abs(n.y) + abs(n.z));
  const float x = n.x * inv;
  const float y = n.y * inv;
  float ox = x;
  float oy = y;
  if (n.z < 0.0f) {
    ox = (1.0f - abs(y)) * RgSignNotZero(x);
    oy = (1.0f - abs(x)) * RgSignNotZero(y);
  }
  return float2(ox * 0.5f + 0.5f, oy * 0.5f + 0.5f);
}

// The `normals` texel: octahedral world normal, linear roughness (clamped to 0..1), the material type as a fraction.
float4 RgNormalsTexel(float3 world_normal, float linear_roughness, uint material) {
  const float2 uv = RgOctahedronUv(world_normal);
  const float r = min(max(linear_roughness, 0.0f), 1.0f);
  const float m = float(min(material, 3u)) / kRgMaterialMax;
  return float4(uv.x, uv.y, r, m);
}

// The signed linear depth of a view distance t (the game's view space looks along -z: negative ahead; `sign` is -1, or +1 for the mirrored convention). A texel that is not traced, or whose
// distance is no distance, gets the same sign with kRgNoDepth: outside the dispatch's bounds.
float RgDepthTexel(bool traced, float t, float sign) {
  const bool valid = traced && t > 0.0f && t < kRgNoDepth;
  return (valid ? t : kRgNoDepth) * sign;
}

// The motion vector texel. `cur` the texel's UV in the unjittered picture, `prev_u`, `prev_v`, `prev_z` the same surface point in the last frame's picture (UV and the distance along its view
// axis, positive ahead: ssr_common.hlsli SsrToPrevious' result), `t` its distance in this frame's, `valid` the point is in front of the last camera (prev_z above the near limit). rg = PreviousUV -
// CurrentUV; b = PreviousLinearDepth - CurrentLinearDepth in the signed convention (`sign` times each distance: for the game's negative-ahead view space that is cur_t - prev_t). 0 where the
// last frame has no place for the point (the dispatch resets its history there anyway).
float4 RgMotionTexel(float2 cur, float prev_u, float prev_v, float prev_z, float t, bool valid, float sign) {
  if (!valid) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return float4(prev_u - cur.x, prev_v - cur.y, sign * prev_z - sign * t, 0.0f);
}

// The first bounce's reflectance the resolve multiplies the denoised radiance by (the reflection's resolve, ssr_apply_ps.hlsl: saturate(F0 * scale + bias)), as the specular albedo guide. `ab` is
// (scale, bias) of the environment BRDF at the surface's roughness and N.V.
float3 RgSpecularAlbedo(float3 f0, float2 ab) {
  return saturate(f0 * ab.x + float3(ab.y, ab.y, ab.y));
}

// The signal texel. The radiance is the trace's own (never below 0: a road's trace is a DIFFERENCE from the game's own reflection and can be negative, which a denoiser of radiance must not be
// given; the negative part is left to the game's reflection that is in the colour already), alpha the hit distance, at least kRgMinHit for a traced texel, 0 for one that is not.
float4 RgSignalTexel(float3 radiance, float hit_distance, bool traced) {
  if (!traced) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return float4(max(radiance.x, 0.0f), max(radiance.y, 0.0f), max(radiance.z, 0.0f), max(hit_distance, kRgMinHit));
}

#endif  // MCLA_GPU_RAY_REGEN_COMMON_HLSLI
