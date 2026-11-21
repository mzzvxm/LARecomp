// MCLA GPU renderer -- screen-space reflections (ssr.h, RR-4a), the raw mode's extra pass: the hit distance guide of DLSS Ray Reconstruction.
//
// Recorded only in a frame Ray Reconstruction takes and the reflections run RAW (ssr_rules.h DecideRaw): right after the apply, which added the trace's one ray a pixel to the colour with no
// accumulation of its own, this pass tells Ray Reconstruction how far each of those reflections went, which is what it moves the reflection's history by (perf/DLSS-RR.md, the specular hit
// distance guide). A full-screen triangle into an R16_FLOAT image of the PICTURE's size (rr_guides.h Inputs::hit[0]; the guide pass reads it at the anchor, the larger of it and the cars' own
// image wins):
//
//   0                a pixel the apply adds nothing to (the sky, a class that keeps its own reflection, a surface under the smoothness floor, no normal), or one no ray was traced for: the surface itself is
//                    the reflection as far as Ray Reconstruction knows (the guide pass writes the 0 as the surface's own)
//   the distance     where the apply adds a term: the world distance from the pixel's surface to the hit of the ray of the trace texel that stands for the pixel (the trace's alpha, SsrHitDistance: the
//                    virtual image's distance the accumulation's reprojection uses), kSsrMissLength (4096) where the ray found nothing, which is how far the environment is
//
// The texel is the one of the four nearest the apply weighs most (the bilinear share times the agreement of the receiver's depth, ssr_apply_ps.hlsl's SSR_RAW loop, which this pass repeats:
// change one, change the other): a distance is not blended, the mean of two surfaces' distances is no surface's. Pixels of a vehicle are never traced (the class gate), so they are 0 here and the
// cars' ray traced reflections (car_rt_refl.h) own them.
//
// Push block: the trace in the low half of SSR_W_SRC; the planes, the Hi-Z, the lens, the camera and the smoothness numbers as the apply's.

#include "ssr_pass.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
  const int2 render = SsrRender();
  if (p.x >= render.x || p.y >= render.y) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float stored = SsrStoredAt(p);
  const float z = SsrLinearDepth(stored, SsrF(SSR_W_LIN_X), SsrF(SSR_W_LIN_Y), SsrF(SSR_W_LIN_Z), SsrF(SSR_W_LIN_W));
  if (z >= kSsrSky * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // the apply's own gates: the class, the smoothness, the ramp, the normal
  const SsrSurface surface = SsrSurfaceAt(p, false);
  const float gain = SsrClassGain(surface.cls, surface.flags);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  if (!(gain > 0.0f) || smoothness < SsrF(SSR_W_MIN_SMOOTH) || !(SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH)) > 0.0f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, float(render.x), float(render.y), z, tan_x, tan_y);
  if (SsrViewNormalAt(p, pc).w < 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // the four nearest texels of the trace, weighted as the apply weighs them; the heaviest one's distance
  const int2 ssr = SsrSize();
  const uint trace_slot = SsrLo(SsrW(SSR_W_SRC));
  const float2 uv_ssr = (float2(p) + 0.5f) * 0.5f - 0.5f;
  const int2 b0 = int2(floor(uv_ssr));
  const float2 f = uv_ssr - float2(b0);
  const float sigma = 0.03f * z + 0.03f;
  float total = 0.0f;
  float best = 0.0f;
  float best_hit = 0.0f;
  [unroll] for (int k = 0; k < 4; ++k) {
    const int2 q = clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), ssr - 1);
    const float4 t = g_textures[trace_slot].Load(int3(q, 0));
    const float wx = (k & 1) != 0 ? f.x : 1.0f - f.x;
    const float wy = (k >> 1) != 0 ? f.y : 1.0f - f.y;
    if (t.w != 0.0f) {
      const float dz = (SsrHizAt(SsrHz(), q.x, q.y) - z) / sigma;
      const float w = wx * wy / (1.0f + dz * dz);
      total += w;
      if (w > best) {
        best = w;
        best_hit = t.w;
      }
    }
  }
  if (!(total > 1.0e-3f)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);   // nothing traced near: the apply added nothing either
  }
  return float4(SsrHitDistance(best_hit), 0.0f, 0.0f, 0.0f);
}
