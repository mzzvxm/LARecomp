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
// The term is image x Fresnel x the class gain x the smoothness ramp x the strength. The colour is in the target's own units, as colour_post, so no exposure is applied.
//
// The game's own reflection (the road's planar view, the glass' env map) is already in the colour this adds to; where the rays missed, or the hit was near the picture's edge, the image is 0 and
// the pixel is the game's alone. GB-I2: on a road the image is the DIFFERENCE between the reflection the rays found and the road view the game already put there (hlsl/ssr_trace_ps.hlsl), so the
// term can be negative; it never takes more than kSsrGuard of the colour that was there (colour_post's, before this pass) away. The ramp (SsrSmoothRamp) takes the reflection in over the first 0.2
// of smoothness above the floor: a road that is just wet enough to cross the floor does not jump to a whole reflection.
//
// Compilations: SSR_MS 0 / 1; the pipeline's blend (add, or replace for a view) is the pipeline's.
//
// RR-4a, SSR_RAW 1 (ssr_apply_raw_ps; only with SSR_MS 0): the same pass for a frame Ray Reconstruction takes. The accumulation (ssr_temporal_ps) did not run: the image this reads is the TRACE itself, the
// current frame's one ray a pixel, and Ray Reconstruction is what cleans it. The trace's alpha is the hit (distance and vehicle), not the receiver's depth, so the depth that decides which texels
// stand for a pixel is the Hi-Z's at the texel (what the accumulation stores there: the receiver's depth); a texel nothing was traced for has alpha 0 in both. Everything else is the plain pass (a
// debug view makes the frame not raw, ssr_rules.h DecideRaw). The plain compilations do not contain this code.

#ifndef SSR_RAW
#define SSR_RAW 0
#endif

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
      return float4(max(t.xyz, float3(0.0f, 0.0f, 0.0f)) * (1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f)), 0.0f);   // the positive part: a road's trace is a difference and can be negative (view 6)
    }
    if (t.w == 0.0f) {
      return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float d = SsrHitDistance(t.w);
    if (d >= kSsrMissLength - 1.0f) {
      return float4(0.8f, 0.1f, 0.1f, 0.0f);   // a miss
    }
    const float g = saturate(log2(d + 1.0f) / 8.0f);
    if (SsrHitIsVehicle(t.w)) {
      return float4(0.4f + 0.6f * g, 0.25f + 0.35f * g, 0.05f, 0.0f);   // a hit on a vehicle: orange (an ID that is known is brighter in the red)
    }
    return float4(g, g, 0.3f + 0.7f * g, 0.0f);
  }
  if (view == 5u) {
    // GB-I2: the game's road reflection view as the trace reads it, stretched over the whole picture (black: no view in this frame)
    const float2 screen_uv = (float2(p) + 0.5f) / float2(render);
    return float4(SsrRoadViewAt(SsrW(SSR_W_ROAD_VIEW_APPLY), screen_uv) * (1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f)), 0.0f);
  }

  const float stored = SsrStoredAt(p);
  const float z = SsrLinearDepth(stored, SsrF(SSR_W_LIN_X), SsrF(SSR_W_LIN_Y), SsrF(SSR_W_LIN_Z), SsrF(SSR_W_LIN_W));
  if (z >= kSsrSky * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const bool stub = SsrFlag(SSR_FLAG_STUB);
  const bool vehicle = stub && SsrVehicleAt(SsrFlag(SSR_FLAG_HZ) ? p.x : p.x / 2, SsrFlag(SSR_FLAG_HZ) ? p.y : p.y / 2);
  const SsrSurface surface = SsrSurfaceAt(p, vehicle);
  const float gain = SsrReceiverGain(surface.cls, surface.flags, SsrW(SSR_W_FRAME));
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  const bool reflects = gain > 0.0f && smoothness >= SsrF(SSR_W_MIN_SMOOTH);
  const float ramp = SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH));
  if (view == 2u) {
    return float4(DebugGate(reflects ? gain * ramp : 0.0f, stub, surface.cls, surface.flags, surface.cls != uint(GB_CLASS_NONE) || surface.flags != 0u), 0.0f);
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
#if SSR_RAW
    if (t.w != 0.0f) {
      const float dz = (SsrHizAt(SsrHz(), q.x, q.y) - z) / sigma;
      w = wx * wy / (1.0f + dz * dz);
    }
#else
    if (t.w > 0.0f) {
      const float dz = (t.w - z) / sigma;
      w = wx * wy / (1.0f + dz * dz);
    }
#endif
    sum += t.xyz * w;
    total += w;
  }
  if (!(total > 1.0e-3f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float3 term = NgFinite3(sum / total * fresnel * (gain * ramp * SsrF(SSR_W_STRENGTH)));   // added to the scene: a NaN or an infinity here would be spread by the game's own blurs (nan_guard.hlsli)
  if (view == 6u) {
    // GB-I2: the term's sign: green what is added, red what is taken out of the game's own reflection (the luminance of each)
    const float scale = 1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f);
    const float3 added = max(term, float3(0.0f, 0.0f, 0.0f));
    const float3 taken = max(-term, float3(0.0f, 0.0f, 0.0f));
    return float4(dot(taken, float3(0.3333f, 0.3333f, 0.3333f)) * scale, dot(added, float3(0.3333f, 0.3333f, 0.3333f)) * scale, 0.0f, 0.0f);
  }
  if (term.x < 0.0f || term.y < 0.0f || term.z < 0.0f) {
    // the guard: a road view that was a worse estimate than the game's own must not black the road
    term = SsrGuardTerm(term, SsrColour((float2(p) + 0.5f) / float2(render), 0.0f));
  }
  if (view == 1u) {
    return float4(max(term, float3(0.0f, 0.0f, 0.0f)) * (1.0f / max(SsrF(SSR_W_CAP) * 0.25f, 1.0f)), 0.0f);
  }
  return float4(term, 0.0f);
}
