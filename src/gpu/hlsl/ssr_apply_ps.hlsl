// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1), pass 3: the resolve into the main colour at the picture's size.
//
// A full-screen triangle into the main view's colour target (RGBA16F with the target's sample count; the pass writes one value for every sample of a pixel), the blend ADDING it to what the game and
// the effects before drew, alpha untouched; or, for a debug view (SSR_W_FLAGS' view bits), replacing the colour.
//
// Per pixel of the picture:
//   the class     RT3's class, flags and smoothness (or the stub's); the class gate (ssr_common.hlsli SsrClassGain), the road's wetness, the smoothness floor. A pixel that reflects nothing adds 0.
//   the normal    RT1's world normal in view space (a pixel RT1 holds none for, or that faces away, adds 0: without a normal there is no Fresnel term to trust).
//   Fresnel       the environment BRDF (SsrEnvBrdf) with F0 0.04, a metal's F0 its albedo (RT2) where the frame has it, saturated (the table overshoots to 1.04 at exactly grazing).
//   the image     the accumulated reflection (the temporal pass's output, half size) read at the pixel's place with the four nearest texels, each weighted by the bilinear share times how well its
//                 receiver depth agrees with this pixel's (the image's alpha): a texel of another surface, or one nothing was traced for (alpha 0), has no weight.
// The term is image x Fresnel x the class gain x the strength. The colour is in the target's own units, as colour_post, so no exposure is applied.
//
// The game's own reflection (the road's planar view, the glass' env map) is already in the colour this adds to; where the rays missed, or the hit was near the picture's edge, the image is 0 and
// the pixel is the game's alone.
//
// Compilations: SSR_MS 0 / 1; the pipeline's blend (add, or replace for a view) is the pipeline's.

#include "ssr_pass.hlsli"

float3 DebugGate(float gain, bool stub, uint cls, uint flags, bool has_data) {
  if (!has_data) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  if (gain > 0.0f) {
    return stub ? float3(0.2f, 0.3f, gain) : float3(0.1f, 0.3f + 0.7f * gain, 0.1f);
  }
  return (flags & uint(GB_FLAG_VEHICLE)) != 0u || cls == uint(GB_CLASS_PAINT) || cls == uint(GB_CLASS_CHROME) ? float3(0.9f, 0.5f, 0.1f) : float3(0.25f, 0.05f, 0.05f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const int2 render = SsrRender();
  if (p.x >= render.x || p.y >= render.y) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const uint view = SsrView();
  const int2 ssr = SsrSize();
  const uint accum_slot = SsrLo(SsrW(SSR_W_SRC));
  const uint trace_slot = SsrHi(SsrW(SSR_W_SRC));
  const float2 uv_ssr = (float2(p) + 0.5f) * 0.5f - 0.5f;   // the pixel's place in texels of the SSR images
  const int2 b0 = int2(floor(uv_ssr));
  const float2 f = uv_ssr - float2(b0);

  // the two debug views that are of the SSR images
  if (view == 3u || view == 4u) {
    const int2 q = clamp(p / 2, int2(0, 0), ssr - 1);
    const float4 t = g_textures[trace_slot].Load(int3(q, 0));
    if (view == 3u) {
      return float4(t.xyz * (1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f)), 0.0f);
    }
    if (t.w == 0.0f) {
      return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float d = abs(t.w);
    if (d >= kSsrMissLength - 1.0f) {
      return float4(0.8f, 0.1f, 0.1f, 0.0f);   // a miss
    }
    const float g = saturate(log2(d + 1.0f) / 8.0f);
    return float4(g, g, 0.3f + 0.7f * g, 0.0f);
  }

  const float stored = SsrStoredAt(p);
  const float z = SsrLinearDepth(stored, SsrF(SSR_W_LIN_X), SsrF(SSR_W_LIN_Y), SsrF(SSR_W_LIN_Z), SsrF(SSR_W_LIN_W));
  if (z >= kSsrSky * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const bool stub = SsrFlag(SSR_FLAG_STUB);
  const bool vehicle = stub && SsrVehicleAt(SsrFlag(SSR_FLAG_HZ) ? p.x : p.x / 2, SsrFlag(SSR_FLAG_HZ) ? p.y : p.y / 2);
  const SsrSurface surface = SsrSurfaceAt(p, vehicle);
  const float gain = SsrClassGain(surface.cls, surface.flags);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  const bool reflects = gain > 0.0f && smoothness >= SsrF(SSR_W_MIN_SMOOTH);
  if (view == 2u) {
    return float4(DebugGate(reflects ? gain : 0.0f, stub, surface.cls, surface.flags, surface.cls != uint(GB_CLASS_NONE) || surface.flags != 0u), 0.0f);
  }
  if (!reflects) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, float(render.x), float(render.y), z, tan_x, tan_y);
  const float4 g_normal = SsrViewNormalAt(p, pc);
  if (g_normal.w < 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 n = g_normal.xyz;
  const float nov = saturate(dot(n, pc * (-1.0f / length(pc))));
  float3 f0 = float3(0.04f, 0.04f, 0.04f);
  if (surface.cls == uint(GB_CLASS_METAL)) {
    const float4 rt2 = SsrRt2At(p);
    f0 = (rt2.x + rt2.y + rt2.z) > 0.0f ? GbAlbedoDecode(rt2.xyz) : float3(0.6f, 0.6f, 0.6f);
  }
  const float2 ab = SsrEnvBrdf(1.0f - smoothness, nov);
  const float3 fresnel = saturate(f0 * ab.x + ab.y);

  // the accumulated image at the pixel's place: four texels, bilinear x agreement of the receiver's depth
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float total = 0.0f;
  const float sigma = 0.03f * z + 0.03f;
  [unroll] for (int k = 0; k < 4; ++k) {
    const int2 q = clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), ssr - 1);
    const float4 t = g_textures[accum_slot].Load(int3(q, 0));
    const float wx = (k & 1) != 0 ? f.x : 1.0f - f.x;
    const float wy = (k >> 1) != 0 ? f.y : 1.0f - f.y;
    float w = 0.0f;
    if (t.w > 0.0f) {
      const float dz = (t.w - z) / sigma;
      w = wx * wy / (1.0f + dz * dz);
    }
    sum += t.xyz * w;
    total += w;
  }
  if (!(total > 1.0e-3f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 term = sum / total * fresnel * (gain * SsrF(SSR_W_STRENGTH));
  if (view == 1u) {
    return float4(term * (1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f)), 0.0f);
  }
  return float4(term, 0.0f);
}
